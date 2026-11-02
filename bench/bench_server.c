/*
 * 1단계 벤치마크 서버. 클라이언트마다 스레드 1개가 전용 채널을 처리한다.
 *  - pingpong: 받은 메시지를 그대로 돌려준다. 길이 0 메시지가 오면 끝낸다.
 *  - throughput: 받고 버린다. 길이 0 메시지는 동기화 지점이며, 그때까지 받은 개수로 응답한다.
 *    동기화는 2번(워밍업 끝, 측정 끝)이다.
 * SIGINT/SIGTERM을 받으면 IPC 자원을 지우고 종료한다.
 */
#include "bench_common.h"
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

typedef struct {
    ipc_chan_t *ch;
    int id, mode, verify;
    size_t max_msg;
    int failed;
} worker_t;

static ipc_cfg_t g_cfg;
static int g_clients;

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

static void fail(worker_t *w, const char *what)
{
    fprintf(stderr, "server worker %d: %s: %s\n", w->id, what, strerror(errno));
    w->failed = 1;
}

static void *worker_main(void *arg)
{
    worker_t *w = arg;
    char *buf = malloc(w->max_msg);
    if (!buf) {
        fail(w, "malloc");
        return NULL;
    }
    memset(buf, 1, w->max_msg);
    if (ipc_server_accept(w->ch) == -1) {
        fail(w, "accept");
        free(buf);
        return NULL;
    }
    if (w->mode == MODE_PINGPONG) {
        for (;;) {
            ssize_t n = ipc_recv(w->ch, buf, w->max_msg);
            if (n < 0) {
                fail(w, "recv");
                break;
            }
            if (n == 0)
                break;
            if (ipc_send(w->ch, buf, (size_t)n) == -1) {
                fail(w, "send");
                break;
            }
        }
    } else {
        uint64_t count = 0, seq = 0;
        int syncs = 0;
        while (syncs < 2) {
            ssize_t n = ipc_recv(w->ch, buf, w->max_msg);
            if (n < 0) {
                fail(w, "recv");
                break;
            }
            if (n == 0) {
                if (ipc_send(w->ch, &count, sizeof count) == -1) {
                    fail(w, "send ack");
                    break;
                }
                count = 0;
                syncs++;
                continue;
            }
            if (w->verify && n >= VERIFY_MIN_SIZE && !pattern_ok(buf, (size_t)n, msg_seed(w->id, seq))) {
                errno = EBADMSG;
                fail(w, "payload verify");
                break;
            }
            seq++;
            count++;
        }
    }
    free(buf);
    return NULL;
}

static void append_stats(const char *csv, const char *mode, long size, int clients, int rep, int verify, usage_t u,
                         long rss_before, long rss_after, uint64_t ovh)
{
    if (!csv)
        return;
    char line[512];
    int n = snprintf(line, sizeof line, "server,%s,%s,%ld,%d,%d,-1,,,,,,,,,%.0f,%.0f,%ld,%ld,%ld,%ld,%llu,%d\n",
                     ipc_name(), mode, size, clients, rep, u.user_us, u.sys_us, u.nvcsw, u.nivcsw, rss_before, rss_after,
                     (unsigned long long)ovh, verify);
    int fd = open(csv, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd == -1)
        die("open csv");
    if (write(fd, line, (size_t)n) != n)
        die("write csv");
    close(fd);
}

static void usage(void)
{
    fprintf(stderr, "usage: server --clients N [--mode pingpong|throughput] [--max-msg B] [--team T]\n"
                    "              [--csv F --size S --rep R] [--verify] [--cleanup-only]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int clients = 1, verify = 0, cleanup_only = 0, rep = 0, mode = MODE_PINGPONG;
    long size = 0;
    size_t max_msg = 1024 * 1024;
    const char *csv = NULL, *mode_s = "pingpong";
    g_cfg.team = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--verify") == 0) { verify = 1; continue; }
        if (strcmp(a, "--cleanup-only") == 0) { cleanup_only = 1; continue; }
        if (i + 1 >= argc)
            usage();
        const char *v = argv[++i];
        if (strcmp(a, "--clients") == 0) clients = atoi(v);
        else if (strcmp(a, "--mode") == 0) {
            mode_s = v;
            if (strcmp(v, "pingpong") == 0) mode = MODE_PINGPONG;
            else if (strcmp(v, "throughput") == 0) mode = MODE_THROUGHPUT;
            else usage();
        }
        else if (strcmp(a, "--max-msg") == 0) max_msg = strtoul(v, NULL, 10);
        else if (strcmp(a, "--team") == 0) g_cfg.team = atoi(v);
        else if (strcmp(a, "--csv") == 0) csv = v;
        else if (strcmp(a, "--size") == 0) size = atol(v);
        else if (strcmp(a, "--rep") == 0) rep = atoi(v);
        else usage();
    }
    if (clients < 1 || clients > MAX_CLIENTS || max_msg == 0)
        usage();
    g_clients = clients;

    if (cleanup_only) {
        for (int i = 0; i < MAX_CLIENTS; i++)
            ipc_unlink(&g_cfg, i);
        return 0;
    }

    /* 스레드를 만들기 전에 막아 두어야 시그널이 sigwait 스레드로만 간다 */
    static sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    if ((errno = pthread_sigmask(SIG_BLOCK, &set, NULL)) != 0)
        die("pthread_sigmask");

    long rss_before = read_vm_kb("VmRSS");
    uint64_t ovh = clock_overhead_ns();
    usage_t u0 = usage_now();

    worker_t *w = calloc((size_t)clients, sizeof *w);
    pthread_t *tid = calloc((size_t)clients, sizeof *tid);
    if (!w || !tid)
        die("calloc");

    pthread_t sig_tid;
    if ((errno = pthread_create(&sig_tid, NULL, signal_thread, &set)) != 0)
        die("pthread_create");

    for (int i = 0; i < clients; i++) {
        w[i].ch = ipc_server_create(&g_cfg, i);
        if (!w[i].ch) {
            int e = errno;
            for (int j = 0; j <= i; j++)
                ipc_unlink(&g_cfg, j);
            errno = e;
            die("ipc_server_create");
        }
        w[i].id = i;
        w[i].mode = mode;
        w[i].verify = verify;
        w[i].max_msg = max_msg;
    }
    for (int i = 0; i < clients; i++) {
        if ((errno = pthread_create(&tid[i], NULL, worker_main, &w[i])) != 0)
            die("pthread_create");
    }

    int failed = 0;
    for (int i = 0; i < clients; i++) {
        pthread_join(tid[i], NULL);
        failed |= w[i].failed;
    }
    usage_t u = usage_diff(usage_now(), u0);
    long rss_after = read_vm_kb("VmRSS");

    for (int i = 0; i < clients; i++) {
        ipc_close(w[i].ch);
        ipc_unlink(&g_cfg, i);
    }
    if (!failed)
        append_stats(csv, mode_s, size, clients, rep, verify, u, rss_before, rss_after, ovh);
    free(w);
    free(tid);
    return failed ? 1 : 0;
}
