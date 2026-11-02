/*
 * 전송 모듈 검증. 한 프로세스 안에서 서버/클라이언트 양쪽의 송수신 스레드를 동시에 돌린다.
 * 크기 경계값을 양방향으로 보내고 내용과 순서를 확인한다.
 * 같은 프로세스이므로 ThreadSanitizer가 링 버퍼와 임시 버퍼의 경쟁 상태까지 검사할 수 있다.
 */
#include "common.h"
#include "ipc.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MSG (1024 * 1024 + 1)

static const size_t SIZES[] = {0,    1,    3,    4,     5,     8,     63,    64,    4095,   4096,
                               4097, 8187, 8188, 8189,  8191,  8192,  8193,  16379, 16380,  16384,
                               65535, 65536, 65537, 131072, 262144, 524288, 1048576, 1048577};
#define NSIZES (sizeof SIZES / sizeof SIZES[0])

typedef struct {
    ipc_chan_t *ch;
    uint64_t dir;
    int rounds;
} job_t;

static void *tx_main(void *arg)
{
    job_t *j = arg;
    char *buf = malloc(MAX_MSG);
    uint64_t seq = 0;
    for (int r = 0; r < j->rounds; r++) {
        for (size_t i = 0; i < NSIZES; i++, seq++) {
            pattern_fill(buf, SIZES[i], (j->dir << 48) ^ seq);
            if (ipc_send(j->ch, buf, SIZES[i]) == -1)
                die("send");
        }
    }
    free(buf);
    return NULL;
}

static void *rx_main(void *arg)
{
    job_t *j = arg;
    char *buf = malloc(MAX_MSG);
    uint64_t seq = 0;
    for (int r = 0; r < j->rounds; r++) {
        for (size_t i = 0; i < NSIZES; i++, seq++) {
            ssize_t n = ipc_recv(j->ch, buf, MAX_MSG);
            if (n != (ssize_t)SIZES[i] || !pattern_ok(buf, SIZES[i], (j->dir << 48) ^ seq)) {
                fprintf(stderr, "rx mismatch: dir=%llu expected=%zu got=%zd seq=%llu (%s)\n", (unsigned long long)j->dir,
                        SIZES[i], n, (unsigned long long)seq, strerror(errno));
                exit(1);
            }
        }
    }
    free(buf);
    return NULL;
}

static void *accept_main(void *arg)
{
    return ipc_server_accept(arg) == 0 ? arg : NULL;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 20;
    ipc_cfg_t cfg = {987};
    ipc_chan_t *srv = ipc_server_create(&cfg, 0);
    if (!srv)
        die("server create");

    pthread_t acc;
    if ((errno = pthread_create(&acc, NULL, accept_main, srv)) != 0)
        die("pthread_create");
    ipc_chan_t *cli = ipc_client_connect(&cfg, 0, CONNECT_TIMEOUT_MS);
    if (!cli)
        die("client connect");
    void *acc_res;
    pthread_join(acc, &acc_res);
    if (!acc_res)
        die("accept");

    job_t jobs[4] = {{cli, 1, rounds}, {srv, 1, rounds}, {srv, 2, rounds}, {cli, 2, rounds}};
    pthread_t t[4];
    pthread_create(&t[0], NULL, tx_main, &jobs[0]); /* 클라이언트 -> 서버 */
    pthread_create(&t[1], NULL, rx_main, &jobs[1]);
    pthread_create(&t[2], NULL, tx_main, &jobs[2]); /* 서버 -> 클라이언트 */
    pthread_create(&t[3], NULL, rx_main, &jobs[3]);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    ipc_close(cli);
    ipc_close(srv);
    ipc_unlink(&cfg, 0);
    printf("%s duplex ok: %d rounds, %zu sizes\n", ipc_name(), rounds, NSIZES);
    return 0;
}
