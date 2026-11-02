/*
 * 공통 IPC 전송 인터페이스.
 * 벤치마크와 응용은 이 함수만 쓰고, 링크 시점에 pipe/mq/shm 구현을 고른다.
 *
 * 스레드 규칙: 채널은 전이중이다. 송신 스레드 1개와 수신 스레드 1개가 동시에 쓸 수 있다.
 * 같은 방향을 여러 스레드가 쓰려면 호출하는 쪽에서 잠가야 한다.
 * 메시지는 조각으로 나뉠 수 있어 잠그지 않으면 서로 섞인다.
 */
#ifndef IPC_H
#define IPC_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int team; /* 조 번호. POSIX IPC 이름 /osproject_조번호_용도 에 쓴다 */
} ipc_cfg_t;

typedef struct ipc_chan ipc_chan_t;

const char *ipc_name(void);

/* 서버: 클라이언트 1개용 자원을 만든다. 남아 있던 같은 이름의 자원은 먼저 지운다. */
ipc_chan_t *ipc_server_create(const ipc_cfg_t *cfg, int client_id);
/* 서버: 클라이언트가 붙을 때까지 기다린다. 필요 없는 구현에서는 바로 돌아온다. */
int ipc_server_accept(ipc_chan_t *ch);
/* 클라이언트: 서버가 자원을 만들 때까지 timeout_ms 동안 재시도한다. */
ipc_chan_t *ipc_client_connect(const ipc_cfg_t *cfg, int client_id, int timeout_ms);

/* len == 0 인 메시지도 전달된다. 성공 0, 실패 -1 */
int ipc_send(ipc_chan_t *ch, const void *buf, size_t len);
/* 메시지 1개 전체를 받아 길이를 돌려준다. 실패 -1 (상대 종료 EPIPE, 버퍼 부족 EMSGSIZE) */
ssize_t ipc_recv(ipc_chan_t *ch, void *buf, size_t cap);

void ipc_close(ipc_chan_t *ch);
/* 이름 있는 자원을 지운다. 여러 번 불러도 안전하다. */
void ipc_unlink(const ipc_cfg_t *cfg, int client_id);

#endif
