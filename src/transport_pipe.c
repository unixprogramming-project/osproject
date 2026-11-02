/*
 * FIFO 구현. 독립 실행 클라이언트를 연결하려면 이름 있는 파이프가 필요하다 (Chap05 27쪽).
 * 쓰기 원자성은 PIPE_BUF까지이므로 클라이언트마다 요청/응답 FIFO를 따로 둔다.
 * 프레임: 4바이트 길이 + 데이터. writev로 시스템 콜 1회에 함께 쓴다.
 */
#include "common.h"
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

struct ipc_chan {
    int fd_tx, fd_rx;
    char req[128], resp[128];
};

const char *ipc_name(void) { return "pipe"; }

static void paths(const ipc_cfg_t *cfg, int id, char *req, char *resp)
{
    snprintf(req, 128, "/tmp/osproject_%d_c%d_req", cfg->team, id);
    snprintf(resp, 128, "/tmp/osproject_%d_c%d_resp", cfg->team, id);
}

void ipc_unlink(const ipc_cfg_t *cfg, int id)
{
    char req[128], resp[128];
    paths(cfg, id, req, resp);
    unlink(req);
    unlink(resp);
}

ipc_chan_t *ipc_server_create(const ipc_cfg_t *cfg, int id)
{
    ipc_chan_t *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->fd_tx = c->fd_rx = -1;
    paths(cfg, id, c->req, c->resp);
    ipc_unlink(cfg, id);
    if (mkfifo(c->req, 0600) == -1 || mkfifo(c->resp, 0600) == -1) {
        free(c);
        return NULL;
    }
    signal(SIGPIPE, SIG_IGN); /* 상대가 먼저 끝나면 SIGPIPE 대신 EPIPE로 처리 (Chap05 14쪽) */
    return c;
}

/* 열기 순서를 서버/클라이언트가 같게 맞춰 FIFO open 교착을 막는다: req 먼저, resp 나중 */
int ipc_server_accept(ipc_chan_t *c)
{
    if ((c->fd_rx = open(c->req, O_RDONLY)) == -1)
        return -1;
    if ((c->fd_tx = open(c->resp, O_WRONLY)) == -1)
        return -1;
    return 0;
}

ipc_chan_t *ipc_client_connect(const ipc_cfg_t *cfg, int id, int timeout_ms)
{
    ipc_chan_t *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->fd_tx = c->fd_rx = -1;
    paths(cfg, id, c->req, c->resp);
    signal(SIGPIPE, SIG_IGN);
    for (int waited = 0;; waited++) {
        c->fd_tx = open(c->req, O_WRONLY);
        if (c->fd_tx != -1 || errno != ENOENT)
            break;
        if (waited >= timeout_ms) {
            free(c);
            return NULL;
        }
        msleep(1);
    }
    if (c->fd_tx == -1 || (c->fd_rx = open(c->resp, O_RDONLY)) == -1) {
        if (c->fd_tx != -1)
            close(c->fd_tx);
        free(c);
        return NULL;
    }
    return c;
}

int ipc_send(ipc_chan_t *c, const void *buf, size_t len)
{
    uint32_t hdr = (uint32_t)len;
    struct iovec iov[2] = {{&hdr, sizeof hdr}, {(void *)buf, len}};
    int idx = 0, cnt = len ? 2 : 1;
    while (idx < cnt) {
        ssize_t n = writev(c->fd_tx, &iov[idx], cnt - idx);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        size_t left = (size_t)n;
        while (idx < cnt && left >= iov[idx].iov_len) {
            left -= iov[idx].iov_len;
            idx++;
        }
        if (idx < cnt) {
            iov[idx].iov_base = (char *)iov[idx].iov_base + left;
            iov[idx].iov_len -= left;
        }
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

ssize_t ipc_recv(ipc_chan_t *c, void *buf, size_t cap)
{
    uint32_t hdr;
    if (read_full(c->fd_rx, &hdr, sizeof hdr) == -1)
        return -1;
    if (hdr > cap) {
        errno = EMSGSIZE;
        return -1;
    }
    if (hdr && read_full(c->fd_rx, buf, hdr) == -1)
        return -1;
    return (ssize_t)hdr;
}

void ipc_close(ipc_chan_t *c)
{
    if (!c)
        return;
    if (c->fd_tx != -1)
        close(c->fd_tx);
    if (c->fd_rx != -1)
        close(c->fd_rx);
    free(c);
}
