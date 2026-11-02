/*
 * POSIX message queue 구현.
 * 기본 설정의 메시지 크기 상한(8192바이트)을 넘는 메시지는 조각으로 나눈다.
 * 첫 조각에만 4바이트 전체 길이를 넣고, 나머지는 사용자 버퍼에서 바로 보낸다.
 * 채점 환경에서 커널 설정을 바꾸지 않아도 동작하도록 설정 변경에 기대지 않는다.
 *
 * IPC_MQ_DEPTH, IPC_MQ_MSGSIZE 환경 변수로 원인 확인 실험을 할 수 있다 (서버 쪽에서 지정).
 */
#include "common.h"
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HDR sizeof(uint32_t)

struct ipc_chan {
    mqd_t q_tx, q_rx;
    size_t msgsize;
    char *tmp_tx, *tmp_rx; /* 방향별로 따로 둔다. 전이중 사용 시 공유하면 경쟁 상태가 된다 */
};

const char *ipc_name(void) { return "mq"; }

static void names(const ipc_cfg_t *cfg, int id, char *req, char *resp)
{
    snprintf(req, 128, "/osproject_%d_c%d_req", cfg->team, id);
    snprintf(resp, 128, "/osproject_%d_c%d_resp", cfg->team, id);
}

void ipc_unlink(const ipc_cfg_t *cfg, int id)
{
    char req[128], resp[128];
    names(cfg, id, req, resp);
    mq_unlink(req);
    mq_unlink(resp);
}

static ipc_chan_t *alloc_chan(size_t msgsize)
{
    ipc_chan_t *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->q_tx = c->q_rx = (mqd_t)-1;
    c->msgsize = msgsize;
    c->tmp_tx = malloc(msgsize);
    c->tmp_rx = malloc(msgsize);
    if (!c->tmp_tx || !c->tmp_rx) {
        free(c->tmp_tx);
        free(c->tmp_rx);
        free(c);
        return NULL;
    }
    return c;
}

ipc_chan_t *ipc_server_create(const ipc_cfg_t *cfg, int id)
{
    char req[128], resp[128];
    names(cfg, id, req, resp);
    ipc_unlink(cfg, id);
    struct mq_attr a;
    memset(&a, 0, sizeof a);
    a.mq_maxmsg = env_long("IPC_MQ_DEPTH", 5);
    a.mq_msgsize = env_long("IPC_MQ_MSGSIZE", 8192);
    if ((size_t)a.mq_msgsize <= HDR) {
        errno = EINVAL;
        return NULL;
    }
    ipc_chan_t *c = alloc_chan((size_t)a.mq_msgsize);
    if (!c)
        return NULL;
    c->q_rx = mq_open(req, O_CREAT | O_EXCL | O_RDONLY, 0600, &a);
    if (c->q_rx != (mqd_t)-1)
        c->q_tx = mq_open(resp, O_CREAT | O_EXCL | O_WRONLY, 0600, &a);
    if (c->q_rx == (mqd_t)-1 || c->q_tx == (mqd_t)-1) {
        int e = errno;
        ipc_close(c);
        ipc_unlink(cfg, id);
        errno = e;
        return NULL;
    }
    return c;
}

int ipc_server_accept(ipc_chan_t *c)
{
    (void)c;
    return 0;
}

static mqd_t open_retry(const char *name, int flags, int timeout_ms)
{
    for (int waited = 0;; waited++) {
        mqd_t q = mq_open(name, flags);
        if (q != (mqd_t)-1 || errno != ENOENT || waited >= timeout_ms)
            return q;
        msleep(1);
    }
}

ipc_chan_t *ipc_client_connect(const ipc_cfg_t *cfg, int id, int timeout_ms)
{
    char req[128], resp[128];
    names(cfg, id, req, resp);
    mqd_t tx = open_retry(req, O_WRONLY, timeout_ms);
    if (tx == (mqd_t)-1)
        return NULL;
    mqd_t rx = open_retry(resp, O_RDONLY, timeout_ms);
    if (rx == (mqd_t)-1) {
        mq_close(tx);
        return NULL;
    }
    struct mq_attr a;
    if (mq_getattr(tx, &a) == -1) {
        mq_close(tx);
        mq_close(rx);
        return NULL;
    }
    ipc_chan_t *c = alloc_chan((size_t)a.mq_msgsize);
    if (!c) {
        mq_close(tx);
        mq_close(rx);
        return NULL;
    }
    c->q_tx = tx;
    c->q_rx = rx;
    return c;
}

static int send_retry(mqd_t q, const char *p, size_t n)
{
    while (mq_send(q, p, n, 0) == -1) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

static ssize_t recv_retry(mqd_t q, char *p, size_t cap)
{
    for (;;) {
        ssize_t n = mq_receive(q, p, cap, NULL);
        if (n != -1 || errno != EINTR)
            return n;
    }
}

int ipc_send(ipc_chan_t *c, const void *buf, size_t len)
{
    const char *p = buf;
    uint32_t total = (uint32_t)len;
    size_t first = len < c->msgsize - HDR ? len : c->msgsize - HDR;

    memcpy(c->tmp_tx, &total, HDR);
    if (first)
        memcpy(c->tmp_tx + HDR, p, first);
    if (send_retry(c->q_tx, c->tmp_tx, HDR + first) == -1)
        return -1;

    for (size_t off = first; off < len;) {
        size_t n = len - off < c->msgsize ? len - off : c->msgsize;
        if (send_retry(c->q_tx, p + off, n) == -1)
            return -1;
        off += n;
    }
    return 0;
}

ssize_t ipc_recv(ipc_chan_t *c, void *buf, size_t cap)
{
    char *p = buf;
    uint32_t total;
    ssize_t n = recv_retry(c->q_rx, c->tmp_rx, c->msgsize);
    if (n == -1)
        return -1;
    if ((size_t)n < HDR) {
        errno = EPROTO;
        return -1;
    }
    memcpy(&total, c->tmp_rx, HDR);
    if (total > cap) {
        errno = EMSGSIZE;
        return -1;
    }
    size_t off = (size_t)n - HDR;
    if (off > total) {
        errno = EPROTO;
        return -1;
    }
    if (off)
        memcpy(p, c->tmp_rx + HDR, off);

    while (off < total) {
        size_t left = total - off;
        /* mq_receive는 버퍼가 mq_msgsize 이상이어야 하므로 꼬리 조각은 임시 버퍼로 받는다 */
        if (left >= c->msgsize) {
            n = recv_retry(c->q_rx, p + off, c->msgsize);
        } else {
            n = recv_retry(c->q_rx, c->tmp_rx, c->msgsize);
            if (n > 0 && (size_t)n <= left)
                memcpy(p + off, c->tmp_rx, (size_t)n);
        }
        if (n == -1)
            return -1;
        if (n == 0 || (size_t)n > left) {
            errno = EPROTO;
            return -1;
        }
        off += (size_t)n;
    }
    return (ssize_t)total;
}

void ipc_close(ipc_chan_t *c)
{
    if (!c)
        return;
    if (c->q_tx != (mqd_t)-1)
        mq_close(c->q_tx);
    if (c->q_rx != (mqd_t)-1)
        mq_close(c->q_rx);
    free(c->tmp_tx);
    free(c->tmp_rx);
    free(c);
}
