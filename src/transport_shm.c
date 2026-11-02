/*
 * POSIX shared memory 구현.
 * 클라이언트마다 세그먼트 1개에 방향별 링 버퍼 2개를 둔다.
 * 프로세스 공유 세마포어(pshared=1, Chap03 73쪽)로 생산자-소비자 동기화를 한다.
 * 링 하나의 생산자와 소비자는 각 1개이므로 읽기/쓰기 위치는 공유하지 않고 각자 갖는다.
 *
 * 복사는 보내는 쪽 1회, 받는 쪽 1회다. 공유 메모리라고 복사가 없는 구현이 아니다.
 * IPC_SHM_SLOTS, IPC_SHM_SLOT_SIZE 환경 변수로 버퍼 크기 실험을 할 수 있다 (서버 쪽에서 지정).
 */
#include "common.h"
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAGIC 0x4f53484du
#define ALIGN64(x) (((size_t)(x) + 63u) & ~(size_t)63u)

typedef struct {
    _Atomic uint32_t magic; /* 초기화가 끝나면 서버가 마지막에 설정한다 */
    uint32_t slots;
    uint32_t slot_size;
    uint32_t stride;
    uint64_t total_size;
    uint64_t ring_off[2]; /* 0: 클라이언트->서버, 1: 서버->클라이언트 */
} shm_hdr_t;

typedef struct {
    sem_t items;  /* 읽을 수 있는 슬롯 수 */
    sem_t spaces; /* 쓸 수 있는 슬롯 수 */
} ring_t;

typedef struct {
    uint32_t total; /* 메시지 전체 길이 */
    uint32_t n;     /* 이 슬롯의 데이터 길이 */
} slot_hdr_t;

struct ipc_chan {
    char *base;
    size_t size;
    ring_t *tx, *rx;
    char *tx_slots, *rx_slots;
    uint32_t slots, slot_size, stride;
    uint32_t tx_idx, rx_idx;
    int owner;
};

const char *ipc_name(void) { return "shm"; }

static void seg_name(const ipc_cfg_t *cfg, int id, char *out)
{
    snprintf(out, 128, "/osproject_%d_c%d_shm", cfg->team, id);
}

void ipc_unlink(const ipc_cfg_t *cfg, int id)
{
    char n[128];
    seg_name(cfg, id, n);
    shm_unlink(n);
}

static size_t ring_head_size(void) { return ALIGN64(sizeof(ring_t)); }

static void bind_rings(ipc_chan_t *c, shm_hdr_t *h, int tx_dir, int rx_dir)
{
    c->slots = h->slots;
    c->slot_size = h->slot_size;
    c->stride = h->stride;
    c->tx = (ring_t *)(c->base + h->ring_off[tx_dir]);
    c->rx = (ring_t *)(c->base + h->ring_off[rx_dir]);
    c->tx_slots = (char *)c->tx + ring_head_size();
    c->rx_slots = (char *)c->rx + ring_head_size();
}

ipc_chan_t *ipc_server_create(const ipc_cfg_t *cfg, int id)
{
    char name[128];
    seg_name(cfg, id, name);
    ipc_unlink(cfg, id);

    /* 방향별 총 512KiB. 슬롯 수와 크기는 docs/decisions.md의 예비 실험으로 정했다 */
    uint32_t slots = (uint32_t)env_long("IPC_SHM_SLOTS", 64);
    uint32_t slot_size = (uint32_t)env_long("IPC_SHM_SLOT_SIZE", 8192);
    size_t stride = ALIGN64(sizeof(slot_hdr_t) + slot_size);
    size_t ring_size = ring_head_size() + stride * slots;
    size_t off0 = ALIGN64(sizeof(shm_hdr_t));
    size_t total = off0 + ring_size * 2;

    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1)
        return NULL;
    if (ftruncate(fd, (off_t)total) == -1) {
        close(fd);
        shm_unlink(name);
        return NULL;
    }
    char *base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        shm_unlink(name);
        return NULL;
    }
    memset(base, 0, total);

    shm_hdr_t *h = (shm_hdr_t *)base;
    h->slots = slots;
    h->slot_size = slot_size;
    h->stride = (uint32_t)stride;
    h->total_size = total;
    h->ring_off[0] = off0;
    h->ring_off[1] = off0 + ring_size;
    for (int d = 0; d < 2; d++) {
        ring_t *r = (ring_t *)(base + h->ring_off[d]);
        if (sem_init(&r->items, 1, 0) == -1 || sem_init(&r->spaces, 1, slots) == -1) {
            munmap(base, total);
            shm_unlink(name);
            return NULL;
        }
    }

    ipc_chan_t *c = calloc(1, sizeof *c);
    if (!c) {
        munmap(base, total);
        shm_unlink(name);
        return NULL;
    }
    c->base = base;
    c->size = total;
    c->owner = 1;
    bind_rings(c, h, 1, 0);
    atomic_store_explicit(&h->magic, MAGIC, memory_order_release);
    return c;
}

int ipc_server_accept(ipc_chan_t *c)
{
    (void)c;
    return 0;
}

ipc_chan_t *ipc_client_connect(const ipc_cfg_t *cfg, int id, int timeout_ms)
{
    char name[128];
    seg_name(cfg, id, name);
    int waited = 0, fd = -1;
    size_t size = 0;

    /* 서버가 ftruncate 하기 전에 매핑하면 SIGBUS가 날 수 있어 크기까지 확인한다 */
    for (;; waited++) {
        if (fd == -1) {
            fd = shm_open(name, O_RDWR, 0);
            if (fd == -1 && errno != ENOENT)
                return NULL;
        }
        if (fd != -1) {
            struct stat st;
            if (fstat(fd, &st) == -1) {
                close(fd);
                return NULL;
            }
            if ((size_t)st.st_size >= sizeof(shm_hdr_t)) {
                size = (size_t)st.st_size;
                break;
            }
        }
        if (waited >= timeout_ms) {
            if (fd != -1)
                close(fd);
            errno = ETIMEDOUT;
            return NULL;
        }
        msleep(1);
    }
    char *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED)
        return NULL;

    shm_hdr_t *h = (shm_hdr_t *)base;
    while (atomic_load_explicit(&h->magic, memory_order_acquire) != MAGIC) {
        if (waited++ >= timeout_ms) {
            munmap(base, size);
            errno = ETIMEDOUT;
            return NULL;
        }
        msleep(1);
    }
    ipc_chan_t *c = calloc(1, sizeof *c);
    if (!c) {
        munmap(base, size);
        return NULL;
    }
    c->base = base;
    c->size = size;
    bind_rings(c, h, 0, 1);
    return c;
}

static int sem_wait_retry(sem_t *s)
{
    while (sem_wait(s) == -1) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

int ipc_send(ipc_chan_t *c, const void *buf, size_t len)
{
    const char *p = buf;
    size_t off = 0;
    do { /* do-while: 길이 0 메시지도 슬롯 1개로 보낸다 */
        if (sem_wait_retry(&c->tx->spaces) == -1)
            return -1;
        char *slot = c->tx_slots + (size_t)c->tx_idx * c->stride;
        slot_hdr_t *sh = (slot_hdr_t *)slot;
        size_t n = len - off < c->slot_size ? len - off : c->slot_size;
        sh->total = (uint32_t)len;
        sh->n = (uint32_t)n;
        if (n)
            memcpy(slot + sizeof(slot_hdr_t), p + off, n);
        c->tx_idx = (c->tx_idx + 1) % c->slots;
        off += n;
        if (sem_post(&c->tx->items) == -1)
            return -1;
    } while (off < len);
    return 0;
}

ssize_t ipc_recv(ipc_chan_t *c, void *buf, size_t cap)
{
    char *p = buf;
    size_t off = 0, total;
    do {
        if (sem_wait_retry(&c->rx->items) == -1)
            return -1;
        char *slot = c->rx_slots + (size_t)c->rx_idx * c->stride;
        slot_hdr_t *sh = (slot_hdr_t *)slot;
        total = sh->total;
        size_t n = sh->n;
        if (total > cap) {
            errno = EMSGSIZE;
            return -1;
        }
        if (off + n > total) {
            errno = EPROTO;
            return -1;
        }
        if (n)
            memcpy(p + off, slot + sizeof(slot_hdr_t), n);
        off += n;
        c->rx_idx = (c->rx_idx + 1) % c->slots;
        if (sem_post(&c->rx->spaces) == -1)
            return -1;
    } while (off < total);
    return (ssize_t)total;
}

void ipc_close(ipc_chan_t *c)
{
    if (!c)
        return;
    if (c->owner) {
        sem_destroy(&c->tx->items);
        sem_destroy(&c->tx->spaces);
        sem_destroy(&c->rx->items);
        sem_destroy(&c->rx->spaces);
    }
    munmap(c->base, c->size);
    free(c);
}
