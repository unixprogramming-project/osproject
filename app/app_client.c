/*
 * 2단계 응용 클라이언트: 정해진 워크로드로 이미지 필터 요청을 보내고 지연을 잰다.
 *
 * 스레드 (프로세스당 3개 + main)
 *  - 요청 스레드: 요청열을 만들어 보낸다. 미처리 요청이 창(window) 크기만큼 쌓이면 기다린다.
 *  - 응답 스레드: 응답 헤더와 본문을 받아 지연을 기록하고, 창을 하나 비운다. (--verify면 내용 확인)
 *  - 수집 스레드: 워밍업과 측정 구간을 정하고, 끝나면 미처리 요청이 0이 될 때까지 기다려 통계를 낸다.
 *
 * 공유 자원과 동기화
 *  - 미처리 요청 수와 정지 플래그: mutex + 조건 변수 (창이 비면 요청 스레드, 0이 되면 수집 스레드를 깨움)
 *  - 요청별 보낸 시각 표: 요청 스레드가 ipc_send 직전에 쓰고 응답 스레드가 읽는다. 둘 다 창 mutex 안에서 접근한다.
 *    (처음에는 "응답이 오려면 요청이 먼저 도착해야 하므로 IPC가 순서를 보장한다"고 보고 잠그지 않았으나,
 *     ThreadSanitizer가 shm 버전에서 경쟁으로 지적했다. 프로세스 사이 공유 메모리 동기화에 기대지 않고 잠근다.)
 *    같은 칸은 SLOT_TABLE(1024)개 요청 뒤에야 재사용되고, 창이 4라 그때는 이미 읽힌 뒤다.
 *  - 종료: 요청 스레드가 멈추면 BYE를 보내고, 서버가 BYE 확인을 돌려준다. 응답 스레드는 확인을 받고
 *    미처리 요청이 0이 되면 끝난다. (확인 없이 끝내면 마지막 recv에서 영원히 막힐 수 있다)
 *  - 측정 구간 시각: 수집 스레드가 쓰고 응답 스레드가 읽음 -> 같은 mutex로 보호
 *  - 지연 표본 배열: 응답 스레드만 쓰고, 수집 스레드는 응답 스레드가 끝난 뒤(join) 읽는다.
 * 측정 구간 안에서는 출력하지 않는다. CSV는 끝난 뒤 한 번 쓴다.
 */
#include "app_common.h"
#include "common.h"
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SLOT_TABLE 1024 /* 창 크기보다 커야 한다 */

typedef struct {
    int id, clients, hit, window, duration_ms, warmup_ms, verify, rep, team;
    uint64_t seed;
    const char *csv;
} opts_t;

static opts_t o;
static ipc_chan_t *g_ch;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int outstanding, stop, failed;
    uint64_t t_start, t_end; /* 측정 구간 (0이면 아직 미정) */
} st = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static uint64_t g_sent_at[SLOT_TABLE];
static uint32_t g_size_of[SLOT_TABLE];

/* 응답 스레드 결과 (응답 스레드만 씀) */
static uint64_t *g_lat;
static size_t g_nlat, g_caplat;
static uint64_t g_bytes, g_done_total, g_bad;
static uint64_t g_req_total;

static void set_failed(const char *what)
{
    fprintf(stderr, "client %d: %s: %s\n", o.id, what, strerror(errno));
    pthread_mutex_lock(&st.mu);
    st.failed = 1;
    st.stop = 1;
    pthread_cond_broadcast(&st.cv);
    pthread_mutex_unlock(&st.mu);
}

static void *request_main(void *arg)
{
    (void)arg;
    reqgen_t g;
    reqgen_init(&g, o.seed, (uint32_t)o.id, o.hit);
    req_msg_t m;
    for (;;) {
        pthread_mutex_lock(&st.mu);
        while (st.outstanding >= o.window && !st.stop)
            pthread_cond_wait(&st.cv, &st.mu);
        if (st.stop) {
            int failed = st.failed;
            pthread_mutex_unlock(&st.mu);
            if (!failed) {
                req_msg_t bye = {.type = MSG_BYE};
                if (ipc_send(g_ch, &bye, sizeof bye) == -1)
                    set_failed("send bye");
            }
            break;
        }
        st.outstanding++;
        pthread_mutex_unlock(&st.mu);

        reqgen_next(&g, &m);
        unsigned slot = m.req_id % SLOT_TABLE;
        pthread_mutex_lock(&st.mu);
        g_size_of[slot] = (uint32_t)image_bytes(m.image_id);
        g_sent_at[slot] = now_ns(); /* 타임스탬프 1: 요청 전송 직전 */
        pthread_mutex_unlock(&st.mu);
        if (ipc_send(g_ch, &m, sizeof m) == -1) {
            set_failed("send");
            break;
        }
        g_req_total++;
    }
    return NULL;
}

static void record(uint64_t lat)
{
    if (g_nlat == g_caplat) {
        size_t nc = g_caplat ? g_caplat * 2 : 65536;
        uint64_t *p = realloc(g_lat, nc * sizeof *p);
        if (!p)
            return;
        g_lat = p;
        g_caplat = nc;
    }
    g_lat[g_nlat++] = lat;
}

static void *response_main(void *arg)
{
    (void)arg;
    uint8_t *body = malloc(MAX_IMAGE_BYTES), *expect = o.verify ? malloc(MAX_IMAGE_BYTES) : NULL,
            *src = o.verify ? malloc(MAX_IMAGE_BYTES) : NULL;
    reqgen_t vg;
    reqgen_init(&vg, o.seed, (uint32_t)o.id, o.hit);
    req_msg_t vm;
    if (!body || (o.verify && (!expect || !src))) {
        set_failed("malloc");
        return NULL;
    }
    int got_bye = 0;
    for (;;) {
        pthread_mutex_lock(&st.mu);
        int finish = got_bye && st.outstanding == 0;
        pthread_mutex_unlock(&st.mu);
        if (finish)
            break;
        resp_hdr_t h;
        ssize_t n = ipc_recv(g_ch, &h, sizeof h);
        if (n == (ssize_t)sizeof h && h.type == MSG_BYE) {
            got_bye = 1; /* 이후 남은 응답은 없거나, 워커가 아직 보내는 중 */
            continue;
        }
        if (n != (ssize_t)sizeof h || h.type != MSG_RESP) {
            errno = n < 0 ? errno : EBADMSG;
            set_failed("recv header");
            break;
        }
        ssize_t blen = 0;
        if (h.status == 0) {
            blen = ipc_recv(g_ch, body, MAX_IMAGE_BYTES);
            if (blen < 0 || (size_t)blen != h.len) {
                errno = blen < 0 ? errno : EBADMSG;
                set_failed("recv body");
                break;
            }
        }
        uint64_t t = now_ns(); /* 타임스탬프 2: 응답 본문까지 모두 받은 직후 */
        unsigned slot = h.req_id % SLOT_TABLE;
        pthread_mutex_lock(&st.mu);
        uint64_t sent = g_sent_at[slot];
        uint32_t want = g_size_of[slot];
        pthread_mutex_unlock(&st.mu);
        uint64_t lat = t - sent;
        if (h.status != 0 || h.len != want)
            g_bad++;
        if (o.verify) { /* 요청열을 다시 만들어 기대 결과와 비교 (측정에는 쓰지 않음) */
            do
                reqgen_next(&vg, &vm);
            while (vm.req_id < h.req_id);
            if (vm.req_id == h.req_id) {
                image_generate(vm.image_id, src);
                filter_apply(vm.filter, vm.param, src, expect, image_side(vm.image_id));
                if (memcmp(expect, body, h.len) != 0)
                    g_bad++;
            }
            /* 응답 순서가 바뀌면 건너뛴 요청은 비교하지 않는다. 그래서 검증 실행은 --window 1로 한다 */
        }
        g_done_total++;

        pthread_mutex_lock(&st.mu);
        int in_window = st.t_start && t >= st.t_start && (!st.t_end || t <= st.t_end) && sent >= st.t_start;
        st.outstanding--;
        pthread_cond_broadcast(&st.cv);
        pthread_mutex_unlock(&st.mu);
        if (in_window) {
            record(lat);
            g_bytes += (uint64_t)blen;
        }
    }
    free(body);
    free(expect);
    free(src);
    return NULL;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void *collector_main(void *arg)
{
    pthread_t *tids = arg; /* [0] 요청, [1] 응답 */
    usage_t u0 = usage_now();
    msleep(o.warmup_ms);
    pthread_mutex_lock(&st.mu);
    st.t_start = now_ns();
    pthread_mutex_unlock(&st.mu);
    msleep(o.duration_ms);
    pthread_mutex_lock(&st.mu);
    st.t_end = now_ns();
    st.stop = 1;
    pthread_cond_broadcast(&st.cv);
    pthread_mutex_unlock(&st.mu);
    usage_t u = usage_diff(usage_now(), u0);

    pthread_join(tids[0], NULL); /* 요청 스레드가 멈춘 뒤 남은 응답을 다 받으면 응답 스레드가 끝난다 */
    pthread_join(tids[1], NULL);
    pthread_mutex_lock(&st.mu);
    int failed = st.failed;
    pthread_mutex_unlock(&st.mu);
    if (failed)
        return NULL;

    double mean = 0;
    uint64_t p50 = 0, p99 = 0, mx = 0;
    if (g_nlat) {
        for (size_t i = 0; i < g_nlat; i++)
            mean += (double)g_lat[i];
        mean /= (double)g_nlat;
        qsort(g_lat, g_nlat, sizeof *g_lat, cmp_u64);
        p50 = g_lat[g_nlat / 2];
        p99 = g_lat[(size_t)((double)(g_nlat - 1) * 0.99)];
        mx = g_lat[g_nlat - 1];
    }
    uint64_t window_ns = st.t_end - st.t_start;
    double rps = (double)g_nlat * 1e9 / (double)window_ns, mbps = (double)g_bytes / 1e6 * 1e9 / (double)window_ns;
    long rss = read_vm_kb("VmRSS");
    if (o.csv) {
        char line[512];
        int n = snprintf(line, sizeof line,
                         "client,%s,%d,%d,%d,%d,%d,%zu,%llu,%llu,%.1f,%.1f,%.1f,%llu,%llu,%llu,%.0f,%.0f,%ld,%ld,%ld,%llu,%d\n",
                         ipc_name(), o.clients, o.hit, o.rep, o.id, o.window, g_nlat, (unsigned long long)window_ns,
                         (unsigned long long)g_bytes, rps, mbps, mean, (unsigned long long)p50, (unsigned long long)p99,
                         (unsigned long long)mx, u.user_us, u.sys_us, u.nvcsw, u.nivcsw, rss,
                         (unsigned long long)g_bad, o.verify);
        int fd = open(o.csv, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd == -1 || write(fd, line, (size_t)n) != n)
            die("write csv");
        close(fd);
    }
    if (g_bad) {
        fprintf(stderr, "client %d: %llu bad responses\n", o.id, (unsigned long long)g_bad);
        pthread_mutex_lock(&st.mu);
        st.failed = 1;
        pthread_mutex_unlock(&st.mu);
    }
    return NULL;
}

static void usage(void)
{
    fprintf(stderr, "usage: app_client --id I --clients N --hit PCT [--window W] [--duration-ms D] [--warmup-ms W]\n"
                    "                  [--seed S] [--team T] [--csv F --rep R] [--verify]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    o = (opts_t){.id = 0, .clients = 1, .hit = 80, .window = 4, .duration_ms = 3000, .warmup_ms = 500, .seed = 2026};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--verify") == 0) { o.verify = 1; continue; }
        if (i + 1 >= argc)
            usage();
        const char *v = argv[++i];
        if (strcmp(a, "--id") == 0) o.id = atoi(v);
        else if (strcmp(a, "--clients") == 0) o.clients = atoi(v);
        else if (strcmp(a, "--hit") == 0) o.hit = atoi(v);
        else if (strcmp(a, "--window") == 0) o.window = atoi(v);
        else if (strcmp(a, "--duration-ms") == 0) o.duration_ms = atoi(v);
        else if (strcmp(a, "--warmup-ms") == 0) o.warmup_ms = atoi(v);
        else if (strcmp(a, "--seed") == 0) o.seed = strtoull(v, NULL, 10);
        else if (strcmp(a, "--team") == 0) o.team = atoi(v);
        else if (strcmp(a, "--csv") == 0) o.csv = v;
        else if (strcmp(a, "--rep") == 0) o.rep = atoi(v);
        else usage();
    }
    if (o.id < 0 || o.window < 1 || o.window >= SLOT_TABLE || o.hit < 0 || o.hit > 100 || o.duration_ms < 1)
        usage();

    ipc_cfg_t cfg = {.team = o.team};
    g_ch = ipc_client_connect(&cfg, o.id, CONNECT_TIMEOUT_MS);
    if (!g_ch)
        die("ipc_client_connect");
    /* 시작 장벽: HELLO를 보내고 START를 기다린다 */
    req_msg_t hello = {.type = MSG_HELLO};
    resp_hdr_t start;
    if (ipc_send(g_ch, &hello, sizeof hello) == -1 || ipc_recv(g_ch, &start, sizeof start) != (ssize_t)sizeof start ||
        start.type != MSG_START)
        die("start barrier");

    pthread_t t[3];
    if ((errno = pthread_create(&t[0], NULL, request_main, NULL)) != 0 ||
        (errno = pthread_create(&t[1], NULL, response_main, NULL)) != 0 ||
        (errno = pthread_create(&t[2], NULL, collector_main, t)) != 0)
        die("pthread_create");
    pthread_join(t[2], NULL);
    ipc_close(g_ch);
    free(g_lat);
    return st.failed ? 1 : 0;
}
