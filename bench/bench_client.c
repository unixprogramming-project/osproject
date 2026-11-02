/*
 * 1단계 벤치마크 클라이언트. 부모가 클라이언트 프로세스 N개를 fork한다.
 * 모두 연결을 마친 뒤 파이프 EOF를 신호로 동시에 시작하고, 결과는 공유 익명 매핑에 남긴다.
 * 측정 구간에서는 출력이나 파일 쓰기를 하지 않는다. 정렬과 CSV 기록은 끝난 뒤 부모가 한다.
 *
 * 타임스탬프 위치
 *  - pingpong: ipc_send 직전 ~ ipc_recv 직후 (왕복 전체, 사용자 코드의 전송 비용 포함)
 *  - throughput: 워밍업 동기화 응답 직후 ~ 측정 끝 동기화 응답 직후 (버퍼에 남은 데이터까지 포함)
 */
#include "bench_common.h"
#include "common.h"
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    int mode, clients, rep, verify, team;
    size_t size, max_msg;
    uint64_t warmup_ns, duration_ns;
    size_t max_samples;
    const char *csv;
} opts_t;

typedef struct {
    uint64_t n, elapsed_ns;
    usage_t usage;
    long rss_before, rss_after;
    int done;
} result_t;

static void pingpong(ipc_chan_t *ch, const opts_t *o, int id, char *tx, char *rx, uint32_t *samples, result_t *r)
{
    uint64_t seq = 0;
    uint64_t warm_end = now_ns() + o->warmup_ns;
    while (now_ns() < warm_end) {
        if (ipc_send(ch, tx, o->size) == -1 || ipc_recv(ch, rx, o->max_msg) != (ssize_t)o->size)
            die("warmup");
    }
    usage_t u0 = usage_now();
    uint64_t start = now_ns(), deadline = start + o->duration_ns, t1 = start, n = 0;
    while (n < o->max_samples) {
        if (o->verify) {
            pattern_fill(tx, o->size, msg_seed(id, seq));
            memset(rx, 0, o->size);
        }
        uint64_t t0 = now_ns();
        if (ipc_send(ch, tx, o->size) == -1)
            die("send");
        ssize_t got = ipc_recv(ch, rx, o->max_msg);
        t1 = now_ns();
        if (got != (ssize_t)o->size) {
            errno = got < 0 ? errno : EPROTO;
            die("recv");
        }
        uint64_t d = t1 - t0;
        samples[n++] = d > UINT32_MAX ? UINT32_MAX : (uint32_t)d;
        if (o->verify && !pattern_ok(rx, o->size, msg_seed(id, seq))) {
            errno = EBADMSG;
            die("payload verify");
        }
        seq++;
        if (t1 >= deadline)
            break;
    }
    r->n = n;
    r->elapsed_ns = t1 - start;
    r->usage = usage_diff(usage_now(), u0);
    if (ipc_send(ch, tx, 0) == -1)
        die("send end");
}

static uint64_t sync_point(ipc_chan_t *ch)
{
    uint64_t ack = 0;
    if (ipc_send(ch, &ack, 0) == -1)
        die("send sync");
    if (ipc_recv(ch, &ack, sizeof ack) != (ssize_t)sizeof ack)
        die("recv ack");
    return ack;
}

static void throughput(ipc_chan_t *ch, const opts_t *o, int id, char *tx, result_t *r)
{
    uint64_t seq = 0;
    uint64_t warm_end = now_ns() + o->warmup_ns;
    while (now_ns() < warm_end) {
        if (o->verify)
            pattern_fill(tx, o->size, msg_seed(id, seq));
        if (ipc_send(ch, tx, o->size) == -1)
            die("warmup");
        seq++;
    }
    sync_point(ch);

    usage_t u0 = usage_now();
    uint64_t start = now_ns(), deadline = start + o->duration_ns, n = 0;
    while (now_ns() < deadline) {
        if (o->verify)
            pattern_fill(tx, o->size, msg_seed(id, seq));
        if (ipc_send(ch, tx, o->size) == -1)
            die("send");
        seq++;
        n++;
    }
    uint64_t acked = sync_point(ch);
    uint64_t end = now_ns();
    if (acked != n) {
        fprintf(stderr, "client %d: sent %llu but server counted %llu\n", id, (unsigned long long)n,
                (unsigned long long)acked);
        exit(1);
    }
    r->n = n;
    r->elapsed_ns = end - start;
    r->usage = usage_diff(usage_now(), u0);
}

static void child_main(const opts_t *o, int id, int ready_fd, int go_fd, uint32_t *samples, result_t *r)
{
    ipc_cfg_t cfg = {o->team};
    char *tx = malloc(o->max_msg), *rx = malloc(o->max_msg);
    if (!tx || !rx)
        die("malloc");
    /* 측정 중 페이지 폴트가 끼지 않도록 미리 건드린다 */
    memset(tx, 0x5a, o->max_msg);
    memset(rx, 0, o->max_msg);
    if (o->mode == MODE_PINGPONG)
        memset(samples, 0, o->max_samples * sizeof *samples);
    r->rss_before = read_vm_kb("VmRSS");

    ipc_chan_t *ch = ipc_client_connect(&cfg, id, CONNECT_TIMEOUT_MS);
    if (!ch)
        die("ipc_client_connect");

    char b = 1;
    if (write(ready_fd, &b, 1) != 1)
        die("write ready");
    close(ready_fd);
    /* 부모가 쓰기 끝을 닫으면 EOF로 깨어난다. 모든 클라이언트가 함께 시작한다 */
    while (read(go_fd, &b, 1) == -1 && errno == EINTR) {
    }
    close(go_fd);

    if (o->mode == MODE_PINGPONG)
        pingpong(ch, o, id, tx, rx, samples, r);
    else
        throughput(ch, o, id, tx, r);

    r->rss_after = read_vm_kb("VmRSS");
    r->done = 1;
    ipc_close(ch);
    free(tx);
    free(rx);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

typedef struct {
    double mean;
    uint32_t p50, p99, max;
} lat_t;

static lat_t lat_stats(uint32_t *s, size_t n)
{
    lat_t l = {0, 0, 0, 0};
    if (!n)
        return l;
    double sum = 0;
    qsort(s, n, sizeof *s, cmp_u32);
    for (size_t i = 0; i < n; i++)
        sum += s[i];
    l.mean = sum / (double)n;
    l.p50 = s[(n - 1) / 2];
    l.p99 = s[(n * 99 - 1) / 100];
    l.max = s[n - 1];
    return l;
}

static void write_row(int fd, const opts_t *o, const char *role, int id, uint64_t n, uint64_t elapsed, const lat_t *l,
                      double mps, double mbps, usage_t u, long rb, long ra, uint64_t ovh)
{
    char lat[128] = ",,,";
    char line[768];
    if (l)
        snprintf(lat, sizeof lat, "%.1f,%u,%u,%u", l->mean, l->p50, l->p99, l->max);
    int len = snprintf(line, sizeof line, "%s,%s,%s,%zu,%d,%d,%d,%llu,%llu,%s,%.1f,%.3f,%.0f,%.0f,%ld,%ld,%ld,%ld,%llu,%d\n",
                       role, ipc_name(), o->mode == MODE_PINGPONG ? "pingpong" : "throughput", o->size, o->clients, o->rep,
                       id, (unsigned long long)n, (unsigned long long)elapsed, lat, mps, mbps, u.user_us, u.sys_us, u.nvcsw,
                       u.nivcsw, rb, ra, (unsigned long long)ovh, o->verify);
    if (write(fd, line, (size_t)len) != len)
        die("write csv");
}

static void report(const opts_t *o, result_t *res, uint32_t *samples, uint64_t ovh)
{
    int fd = STDOUT_FILENO;
    if (o->csv) {
        fd = open(o->csv, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd == -1)
            die("open csv");
    }
    usage_t total = {0, 0, 0, 0};
    uint64_t n_all = 0, max_elapsed = 0;
    double mps_all = 0, mbps_all = 0;
    long rb_all = 0, ra_all = 0;
    for (int i = 0; i < o->clients; i++) {
        result_t *r = &res[i];
        double sec = (double)r->elapsed_ns / 1e9;
        double mps = o->mode == MODE_THROUGHPUT && sec > 0 ? (double)r->n / sec : 0;
        double mbps = mps * (double)o->size / 1e6;
        lat_t l, *lp = NULL;
        if (o->mode == MODE_PINGPONG) {
            l = lat_stats(samples + (size_t)i * o->max_samples, r->n);
            lp = &l;
        }
        write_row(fd, o, "client", i, r->n, r->elapsed_ns, lp, mps, mbps, r->usage, r->rss_before, r->rss_after, ovh);
        n_all += r->n;
        mps_all += mps;
        mbps_all += mbps;
        total.user_us += r->usage.user_us;
        total.sys_us += r->usage.sys_us;
        total.nvcsw += r->usage.nvcsw;
        total.nivcsw += r->usage.nivcsw;
        rb_all += r->rss_before;
        ra_all += r->rss_after;
        if (r->elapsed_ns > max_elapsed)
            max_elapsed = r->elapsed_ns;
    }
    lat_t all, *ap = NULL;
    if (o->mode == MODE_PINGPONG) {
        /* 클라이언트별 표본을 앞으로 모아 전체 분위수를 구한다 */
        size_t w = 0;
        for (int i = 0; i < o->clients; i++) {
            uint32_t *src = samples + (size_t)i * o->max_samples;
            memmove(samples + w, src, res[i].n * sizeof *samples);
            w += res[i].n;
        }
        all = lat_stats(samples, w);
        ap = &all;
    }
    write_row(fd, o, "all", -1, n_all, max_elapsed, ap, mps_all, mbps_all, total, rb_all, ra_all, ovh);
    if (o->csv)
        close(fd);
}

static pid_t *g_pids;
static int g_nchildren;

/* 부모가 종료 시그널을 받으면 자식도 함께 끝낸다. 막혀 있는 자식이 고아로 남지 않게 한다 */
static void on_term(int sig)
{
    for (int i = 0; i < g_nchildren; i++) {
        if (g_pids[i] > 0)
            kill(g_pids[i], SIGKILL);
    }
    _exit(128 + sig);
}

static void usage_exit(void)
{
    fprintf(stderr, "usage: client --clients N --size B [--mode pingpong|throughput] [--duration-ms D]\n"
                    "              [--warmup-ms W] [--max-msg B] [--team T] [--csv F --rep R] [--verify]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    opts_t o = {MODE_PINGPONG, 1, 0, 0, 0, 0, 1024 * 1024, 100000000ull, 500000000ull, 400000, NULL};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--verify") == 0) { o.verify = 1; continue; }
        if (i + 1 >= argc)
            usage_exit();
        const char *v = argv[++i];
        if (strcmp(a, "--clients") == 0) o.clients = atoi(v);
        else if (strcmp(a, "--size") == 0) o.size = strtoul(v, NULL, 10);
        else if (strcmp(a, "--mode") == 0) {
            if (strcmp(v, "pingpong") == 0) o.mode = MODE_PINGPONG;
            else if (strcmp(v, "throughput") == 0) o.mode = MODE_THROUGHPUT;
            else usage_exit();
        }
        else if (strcmp(a, "--duration-ms") == 0) o.duration_ns = strtoull(v, NULL, 10) * 1000000ull;
        else if (strcmp(a, "--warmup-ms") == 0) o.warmup_ns = strtoull(v, NULL, 10) * 1000000ull;
        else if (strcmp(a, "--max-msg") == 0) o.max_msg = strtoul(v, NULL, 10);
        else if (strcmp(a, "--team") == 0) o.team = atoi(v);
        else if (strcmp(a, "--csv") == 0) o.csv = v;
        else if (strcmp(a, "--rep") == 0) o.rep = atoi(v);
        else usage_exit();
    }
    /* 길이 0은 제어 메시지로 쓰므로 측정 크기는 1 이상이다 */
    if (o.clients < 1 || o.clients > 64 || o.size == 0 || o.size > o.max_msg)
        usage_exit();
    if (o.verify && o.size < VERIFY_MIN_SIZE) {
        fprintf(stderr, "--verify needs size >= %d\n", VERIFY_MIN_SIZE);
        return 2;
    }

    uint64_t ovh = clock_overhead_ns();
    size_t res_size = (size_t)o.clients * sizeof(result_t);
    size_t sample_size = o.mode == MODE_PINGPONG ? (size_t)o.clients * o.max_samples * sizeof(uint32_t) : 0;
    char *shared = mmap(NULL, res_size + sample_size + 8, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED)
        die("mmap");
    result_t *res = (result_t *)shared;
    uint32_t *samples = (uint32_t *)(shared + ((res_size + 7) & ~(size_t)7));

    int ready[2], go[2];
    if (pipe(ready) == -1 || pipe(go) == -1)
        die("pipe");

    pid_t *pids = calloc((size_t)o.clients, sizeof *pids);
    if (!pids)
        die("calloc");
    g_pids = pids;
    g_nchildren = o.clients;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigfillset(&sa.sa_mask);
    sigset_t term_set, old_set;
    sigemptyset(&term_set);
    sigaddset(&term_set, SIGINT);
    sigaddset(&term_set, SIGTERM);
    /* fork 도중 시그널이 와도 자식 pid를 놓치지 않도록 막아 둔다 */
    sigprocmask(SIG_BLOCK, &term_set, &old_set);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    for (int i = 0; i < o.clients; i++) {
        pid_t p = fork();
        if (p == -1)
            die("fork");
        if (p == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            sigprocmask(SIG_SETMASK, &old_set, NULL);
            close(ready[0]);
            close(go[1]);
            child_main(&o, i, ready[1], go[0], samples + (size_t)i * o.max_samples, &res[i]);
            _exit(0);
        }
        pids[i] = p;
    }
    sigprocmask(SIG_SETMASK, &old_set, NULL);
    close(ready[1]);
    close(go[0]);

    int ok = 1;
    for (int i = 0; i < o.clients; i++) {
        char b;
        ssize_t n;
        while ((n = read(ready[0], &b, 1)) == -1 && errno == EINTR) {
        }
        if (n != 1) {
            ok = 0;
            break;
        }
    }
    close(ready[0]);
    if (!ok) {
        fprintf(stderr, "client: a child failed before start\n");
        for (int i = 0; i < o.clients; i++)
            kill(pids[i], SIGTERM);
    }
    close(go[1]); /* 시작 신호 */

    for (int i = 0; i < o.clients; i++) {
        int st = 0;
        while (waitpid(pids[i], &st, 0) == -1 && errno == EINTR) {
        }
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || !res[i].done)
            ok = 0;
    }
    if (!ok) {
        fprintf(stderr, "client: run failed\n");
        return 1;
    }
    report(&o, res, samples, ovh);
    free(pids);
    return 0;
}
