/*
 * 2단계 응용 서버: 이미지 필터 캐시 서버.
 *
 * 스레드 (프로세스당 3종류, 클라 N개면 N + W + 1개)
 *  - 수신 스레드 (클라이언트당 1): 요청을 받아 작업 큐에 넣는다. HELLO는 시작 장벽, BYE는 끝.
 *  - 워커 스레드 (W개, 기본 4): 작업 큐에서 꺼내 캐시 조회, 미스면 필터 연산 후 삽입, 응답 전송.
 *  - 시그널 스레드 (1): SIGINT/SIGTERM을 sigwait로 받아 IPC 자원을 지우고 끝낸다.
 *
 * 공유 자원과 동기화
 *  - 작업 큐 (크기 제한 링): mutex + 조건 변수 2개 (not_empty, not_full). 생산자-소비자.
 *  - 캐시 (해시맵 + LRU 리스트): mutex 1개. 조회도 LRU 순서를 바꾸므로 읽기/쓰기 잠금을 구분하지 않는다.
 *  - 캐시 항목: 참조 카운트. 전송은 잠금 밖에서 하고, 전송 중인 항목은 쫓겨나도 해제하지 않는다.
 *  - 클라이언트별 송신 채널: 클라이언트별 mutex. 응답 헤더와 본문 메시지가 섞이지 않게 한다.
 *  - 시작 장벽: mutex + 조건 변수.
 *  - 통계: 워커별로 따로 세고 종료 때 합산 (측정 중 공유 쓰기 없음).
 *  잠금 순서: 캐시 잠금과 송신 잠금을 겹쳐 잡지 않는다 (교착 상태 방지).
 */
#include "app_common.h"
#include "common.h"
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CLIENTS 64
#define MAX_WORKERS 64
#define QUEUE_CAP 64
#define HASH_BUCKETS 4096

/* ---------- 작업 큐 ---------- */
typedef struct {
    int client;
    req_msg_t req;
    uint64_t t_enq;
} job_t;

static struct {
    job_t buf[QUEUE_CAP];
    int head, count, closed;
    pthread_mutex_t mu;
    pthread_cond_t not_empty, not_full;
} q = {.mu = PTHREAD_MUTEX_INITIALIZER, .not_empty = PTHREAD_COND_INITIALIZER, .not_full = PTHREAD_COND_INITIALIZER};

static void queue_push(const job_t *j)
{
    pthread_mutex_lock(&q.mu);
    while (q.count == QUEUE_CAP)
        pthread_cond_wait(&q.not_full, &q.mu); /* 큐가 차면 수신 스레드가 멈춤 -> 클라 전송이 늦어지는 역압 */
    q.buf[(q.head + q.count) % QUEUE_CAP] = *j;
    q.count++;
    pthread_cond_signal(&q.not_empty);
    pthread_mutex_unlock(&q.mu);
}

/* 꺼내면 1, 큐가 닫히고 비었으면 0 */
static int queue_pop(job_t *j)
{
    pthread_mutex_lock(&q.mu);
    while (q.count == 0 && !q.closed)
        pthread_cond_wait(&q.not_empty, &q.mu);
    if (q.count == 0) {
        pthread_mutex_unlock(&q.mu);
        return 0;
    }
    *j = q.buf[q.head];
    q.head = (q.head + 1) % QUEUE_CAP;
    q.count--;
    pthread_cond_signal(&q.not_full);
    pthread_mutex_unlock(&q.mu);
    return 1;
}

static void queue_close(void)
{
    pthread_mutex_lock(&q.mu);
    q.closed = 1;
    pthread_cond_broadcast(&q.not_empty);
    pthread_mutex_unlock(&q.mu);
}

/* ---------- 캐시 ---------- */
typedef struct entry {
    uint32_t image_id, filter, param;
    size_t len;
    uint8_t *data;
    int refs;    /* 전송 중인 워커 수 (캐시 잠금으로 보호) */
    int evicted; /* 캐시에서 빠졌지만 아직 전송 중 */
    struct entry *hnext;
    struct entry *prev, *next; /* LRU: head가 최근 */
} entry_t;

static struct {
    entry_t *bucket[HASH_BUCKETS];
    entry_t *head, *tail;
    size_t bytes, cap;
    pthread_mutex_t mu;
} cache = {.mu = PTHREAD_MUTEX_INITIALIZER};

static unsigned hash_key(uint32_t id, uint32_t f, uint32_t p)
{
    uint32_t h = id * 2654435761u ^ f * 40503u ^ p * 2246822519u;
    return (h ^ (h >> 15)) % HASH_BUCKETS;
}

static void lru_unlink(entry_t *e)
{
    if (e->prev) e->prev->next = e->next; else cache.head = e->next;
    if (e->next) e->next->prev = e->prev; else cache.tail = e->prev;
    e->prev = e->next = NULL;
}

static void lru_push_front(entry_t *e)
{
    e->prev = NULL;
    e->next = cache.head;
    if (cache.head) cache.head->prev = e;
    cache.head = e;
    if (!cache.tail) cache.tail = e;
}

static entry_t *cache_find(uint32_t id, uint32_t f, uint32_t p)
{
    for (entry_t *e = cache.bucket[hash_key(id, f, p)]; e; e = e->hnext)
        if (e->image_id == id && e->filter == f && e->param == p)
            return e;
    return NULL;
}

static void hash_remove(entry_t *e)
{
    entry_t **pp = &cache.bucket[hash_key(e->image_id, e->filter, e->param)];
    while (*pp != e)
        pp = &(*pp)->hnext;
    *pp = e->hnext;
}

static void entry_free(entry_t *e)
{
    free(e->data);
    free(e);
}

/* 캐시 잠금을 잡은 상태에서 부른다. 용량을 넘으면 오래된 항목부터 뺀다 */
static long cache_evict_locked(void)
{
    long n = 0;
    entry_t *e = cache.tail;
    while (cache.bytes > cache.cap && e) {
        entry_t *prev = e->prev;
        lru_unlink(e);
        hash_remove(e);
        cache.bytes -= e->len;
        if (e->refs > 0)
            e->evicted = 1; /* 전송이 끝나면 마지막 워커가 해제 */
        else
            entry_free(e);
        n++;
        e = prev;
    }
    return n;
}

/* ---------- 서버 상태 ---------- */
typedef struct {
    ipc_chan_t *ch;
    pthread_mutex_t send_mu;
    int id, failed;
} client_t;

typedef struct {
    int id, failed;
    uint64_t jobs, hits, misses, evictions, bytes;
    uint64_t lock_wait_ns, queue_wait_ns, send_wait_ns, filter_ns;
} worker_t;

static ipc_cfg_t g_cfg;
static int g_clients;
static client_t g_cl[MAX_CLIENTS];
static uint8_t *g_images[N_IMAGES];

static struct {
    int hello;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} barrier = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static void *signal_thread(void *arg)
{
    sigset_t *set = arg;
    int sig = 0;
    if (sigwait(set, &sig) == 0) {
        for (int i = 0; i < g_clients; i++)
            ipc_unlink(&g_cfg, i);
        _exit(128 + sig);
    }
    return NULL;
}

static int send_locked(client_t *c, const void *a, size_t alen, const void *b, size_t blen)
{
    int rc = 0;
    if (ipc_send(c->ch, a, alen) == -1)
        rc = -1;
    else if (b && ipc_send(c->ch, b, blen) == -1)
        rc = -1;
    return rc;
}

static void *receiver_main(void *arg)
{
    client_t *c = arg;
    req_msg_t m;
    if (ipc_server_accept(c->ch) == -1) {
        fprintf(stderr, "server: accept %d: %s\n", c->id, strerror(errno));
        c->failed = 1;
        return NULL;
    }
    for (;;) {
        ssize_t n = ipc_recv(c->ch, &m, sizeof m);
        if (n != (ssize_t)sizeof m) {
            fprintf(stderr, "server: recv %d: %s\n", c->id, n < 0 ? strerror(errno) : "short message");
            c->failed = 1;
            break;
        }
        if (m.type == MSG_REQ) {
            job_t j = {.client = c->id, .req = m, .t_enq = now_ns()};
            queue_push(&j);
        } else if (m.type == MSG_HELLO) {
            /* 모든 클라이언트가 접속하면 동시에 시작한다 (측정 구간을 맞추기 위함) */
            pthread_mutex_lock(&barrier.mu);
            if (++barrier.hello == g_clients)
                pthread_cond_broadcast(&barrier.cv);
            while (barrier.hello < g_clients)
                pthread_cond_wait(&barrier.cv, &barrier.mu);
            pthread_mutex_unlock(&barrier.mu);
            resp_hdr_t h = {.type = MSG_START};
            pthread_mutex_lock(&c->send_mu);
            int rc = send_locked(c, &h, sizeof h, NULL, 0);
            pthread_mutex_unlock(&c->send_mu);
            if (rc == -1) {
                c->failed = 1;
                break;
            }
        } else if (m.type == MSG_BYE) {
            /* 확인을 돌려준다. 아직 처리 중인 응답보다 먼저 갈 수 있으므로 클라는 미처리 수로 끝을 판단한다 */
            resp_hdr_t h = {.type = MSG_BYE};
            pthread_mutex_lock(&c->send_mu);
            if (send_locked(c, &h, sizeof h, NULL, 0) == -1)
                c->failed = 1;
            pthread_mutex_unlock(&c->send_mu);
            break;
        } else {
            fprintf(stderr, "server: bad message type %u\n", m.type);
            c->failed = 1;
            break;
        }
    }
    return NULL;
}

static void *worker_main(void *arg)
{
    worker_t *w = arg;
    job_t j;
    while (queue_pop(&j)) {
        uint64_t t0 = now_ns();
        w->queue_wait_ns += t0 - j.t_enq;
        w->jobs++;
        req_msg_t *r = &j.req;
        resp_hdr_t h = {.type = MSG_RESP, .req_id = r->req_id, .status = 0};
        if (r->image_id >= N_IMAGES || r->filter >= N_FILTERS) {
            h.status = 1;
            client_t *c = &g_cl[j.client];
            pthread_mutex_lock(&c->send_mu);
            if (send_locked(c, &h, sizeof h, NULL, 0) == -1)
                w->failed = 1;
            pthread_mutex_unlock(&c->send_mu);
            continue;
        }

        /* 1. 캐시 조회 (잠금 안에서는 조회, LRU 갱신, 참조 증가만) */
        uint64_t tl = now_ns();
        pthread_mutex_lock(&cache.mu);
        w->lock_wait_ns += now_ns() - tl;
        entry_t *e = cache_find(r->image_id, r->filter, r->param);
        if (e) {
            lru_unlink(e);
            lru_push_front(e);
            e->refs++;
            w->hits++;
        }
        pthread_mutex_unlock(&cache.mu);

        /* 2. 미스: 잠금 밖에서 필터 연산 후 삽입 */
        if (!e) {
            w->misses++;
            size_t len = image_bytes(r->image_id);
            entry_t *ne = calloc(1, sizeof *ne);
            uint8_t *data = ne ? malloc(len) : NULL;
            if (!data) {
                free(ne);
                fprintf(stderr, "server: out of memory\n");
                w->failed = 1;
                break;
            }
            uint64_t tf = now_ns();
            filter_apply(r->filter, r->param, g_images[r->image_id], data, image_side(r->image_id));
            w->filter_ns += now_ns() - tf;
            ne->image_id = r->image_id;
            ne->filter = r->filter;
            ne->param = r->param;
            ne->len = len;
            ne->data = data;
            tl = now_ns();
            pthread_mutex_lock(&cache.mu);
            w->lock_wait_ns += now_ns() - tl;
            e = cache_find(r->image_id, r->filter, r->param);
            if (e) { /* 그 사이 다른 워커가 넣었다 */
                lru_unlink(e);
                lru_push_front(e);
                entry_free(ne);
            } else {
                e = ne;
                unsigned b = hash_key(e->image_id, e->filter, e->param);
                e->hnext = cache.bucket[b];
                cache.bucket[b] = e;
                lru_push_front(e);
                cache.bytes += e->len;
            }
            e->refs++; /* 먼저 참조를 잡아 아래 축출에서 해제되지 않게 한다 */
            w->evictions += (uint64_t)cache_evict_locked();
            pthread_mutex_unlock(&cache.mu);
        }

        /* 3. 응답 전송 (캐시 잠금 밖, 클라이언트별 송신 잠금 안) */
        h.len = (uint32_t)e->len;
        client_t *c = &g_cl[j.client];
        uint64_t ts = now_ns();
        pthread_mutex_lock(&c->send_mu);
        w->send_wait_ns += now_ns() - ts;
        if (send_locked(c, &h, sizeof h, e->data, e->len) == -1) {
            fprintf(stderr, "server: send %d: %s\n", c->id, strerror(errno));
            w->failed = 1;
        }
        pthread_mutex_unlock(&c->send_mu);
        w->bytes += e->len;

        /* 4. 참조 해제. 전송 중 축출된 항목은 마지막 참조가 해제한다 */
        pthread_mutex_lock(&cache.mu);
        int free_it = (--e->refs == 0 && e->evicted);
        pthread_mutex_unlock(&cache.mu);
        if (free_it)
            entry_free(e);
        /* 전송 실패해도 큐는 계속 비운다. 멈추면 수신 스레드가 꽉 찬 큐에서 영원히 기다린다 */
    }
    return NULL;
}

static void usage(void)
{
    fprintf(stderr, "usage: app_server --clients N [--workers W] [--cache-mb M] [--team T]\n"
                    "                  [--csv F --rep R --hit H] [--cleanup-only]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int clients = 1, workers = 4, cleanup_only = 0, rep = 0, hit = -1;
    long cache_mb = 64;
    const char *csv = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--cleanup-only") == 0) { cleanup_only = 1; continue; }
        if (i + 1 >= argc)
            usage();
        const char *v = argv[++i];
        if (strcmp(a, "--clients") == 0) clients = atoi(v);
        else if (strcmp(a, "--workers") == 0) workers = atoi(v);
        else if (strcmp(a, "--cache-mb") == 0) cache_mb = atol(v);
        else if (strcmp(a, "--team") == 0) g_cfg.team = atoi(v);
        else if (strcmp(a, "--csv") == 0) csv = v;
        else if (strcmp(a, "--rep") == 0) rep = atoi(v);
        else if (strcmp(a, "--hit") == 0) hit = atoi(v);
        else usage();
    }
    if (cleanup_only) {
        for (int i = 0; i < MAX_CLIENTS; i++)
            ipc_unlink(&g_cfg, i);
        return 0;
    }
    if (clients < 1 || clients > MAX_CLIENTS || workers < 1 || workers > MAX_WORKERS || cache_mb < 1)
        usage();
    g_clients = clients;
    cache.cap = (size_t)cache_mb << 20;

    /* 스레드를 만들기 전에 막아 두어야 시그널이 sigwait 스레드로만 간다 */
    static sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    if ((errno = pthread_sigmask(SIG_BLOCK, &set, NULL)) != 0)
        die("pthread_sigmask");
    pthread_t sig_tid;
    if ((errno = pthread_create(&sig_tid, NULL, signal_thread, &set)) != 0)
        die("pthread_create signal");

    for (uint32_t i = 0; i < N_IMAGES; i++) { /* 원본 이미지 (두 버전 같음) */
        g_images[i] = malloc(image_bytes(i));
        if (!g_images[i])
            die("malloc image");
        image_generate(i, g_images[i]);
    }

    for (int i = 0; i < clients; i++) {
        g_cl[i].id = i;
        pthread_mutex_init(&g_cl[i].send_mu, NULL);
        g_cl[i].ch = ipc_server_create(&g_cfg, i);
        if (!g_cl[i].ch) {
            int e = errno;
            for (int k = 0; k <= i; k++)
                ipc_unlink(&g_cfg, k);
            errno = e;
            die("ipc_server_create");
        }
    }

    long rss_before = read_vm_kb("VmRSS");
    usage_t u0 = usage_now();
    worker_t *w = calloc((size_t)workers, sizeof *w);
    pthread_t *wt = calloc((size_t)workers, sizeof *wt), rt[MAX_CLIENTS];
    if (!w || !wt)
        die("calloc");
    for (int i = 0; i < workers; i++) {
        w[i].id = i;
        if ((errno = pthread_create(&wt[i], NULL, worker_main, &w[i])) != 0)
            die("pthread_create worker");
    }
    for (int i = 0; i < clients; i++)
        if ((errno = pthread_create(&rt[i], NULL, receiver_main, &g_cl[i])) != 0)
            die("pthread_create receiver");

    int failed = 0;
    for (int i = 0; i < clients; i++) {
        pthread_join(rt[i], NULL);
        failed |= g_cl[i].failed;
    }
    queue_close(); /* 남은 작업을 모두 처리한 뒤 워커가 끝난다 */
    worker_t sum = {0};
    for (int i = 0; i < workers; i++) {
        pthread_join(wt[i], NULL);
        failed |= w[i].failed;
        sum.jobs += w[i].jobs; sum.hits += w[i].hits; sum.misses += w[i].misses;
        sum.evictions += w[i].evictions; sum.bytes += w[i].bytes;
        sum.lock_wait_ns += w[i].lock_wait_ns; sum.queue_wait_ns += w[i].queue_wait_ns;
        sum.send_wait_ns += w[i].send_wait_ns; sum.filter_ns += w[i].filter_ns;
    }
    long rss_after = read_vm_kb("VmRSS");
    long rss_peak = read_vm_kb("VmHWM");
    usage_t u = usage_diff(usage_now(), u0);

    for (int i = 0; i < clients; i++) {
        ipc_close(g_cl[i].ch);
        ipc_unlink(&g_cfg, i);
    }

    if (csv && !failed) {
        char line[640];
        int n = snprintf(line, sizeof line,
                         "server,%s,%d,%d,%d,%d,%ld,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.0f,%.0f,%ld,%ld,%ld,%ld,%ld\n",
                         ipc_name(), clients, hit, rep, workers, cache_mb, (unsigned long long)sum.jobs,
                         (unsigned long long)sum.hits, (unsigned long long)sum.misses,
                         (unsigned long long)sum.evictions, (unsigned long long)sum.bytes,
                         (unsigned long long)sum.lock_wait_ns, (unsigned long long)sum.queue_wait_ns,
                         (unsigned long long)sum.send_wait_ns, (unsigned long long)sum.filter_ns, u.user_us, u.sys_us,
                         u.nvcsw, u.nivcsw, rss_before, rss_after, rss_peak);
        int fd = open(csv, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd == -1 || write(fd, line, (size_t)n) != n)
            die("write csv");
        close(fd);
    }
    /* 종료 정리: 캐시와 원본 이미지 해제 */
    for (entry_t *e = cache.head; e;) {
        entry_t *nx = e->next;
        entry_free(e);
        e = nx;
    }
    for (int i = 0; i < N_IMAGES; i++)
        free(g_images[i]);
    free(w);
    free(wt);
    return failed ? 1 : 0;
}
