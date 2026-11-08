/*
 * 2단계 응용(이미지 필터 캐시 서버) 공통 모듈.
 * 메시지 형식, 이미지 생성, 필터 연산, 요청열 생성을 서버와 클라이언트가 함께 쓴다.
 * IPC와 무관한 응용 로직만 둔다. IPC 부분은 ipc.h 구현(pipe/shm)을 링크 시점에 고른다.
 */
#ifndef APP_COMMON_H
#define APP_COMMON_H

#include <stddef.h>
#include <stdint.h>

/* 메시지 종류 */
#define MSG_HELLO 1 /* 클라 -> 서버: 접속 알림 (모든 클라가 모이면 START) */
#define MSG_REQ 2   /* 클라 -> 서버: 필터 요청 */
#define MSG_BYE 3   /* 클라 -> 서버: 끝 */
#define MSG_START 4 /* 서버 -> 클라: 측정 시작 */
#define MSG_RESP 5  /* 서버 -> 클라: 응답 헤더. 바로 뒤에 이미지 본문 메시지 1개가 온다 */

#define REQ_MSG_SIZE 64 /* 요청 메시지 크기 고정 (워크로드 명세) */

typedef struct {
    uint32_t type, req_id, image_id, filter, param;
    uint8_t pad[REQ_MSG_SIZE - 20];
} req_msg_t;

typedef struct {
    uint32_t type, req_id, len, status;
} resp_hdr_t;

/* 이미지: ID 0~31 128x128(16KB), 32~51 512x512(256KB), 52~63 1024x1024(1MB) */
#define N_IMAGES 64
#define N_FILTERS 3 /* 0 반전, 1 임계값 이진화, 2 3x3 박스 블러 */
#define MAX_IMAGE_BYTES (1024 * 1024)

int image_side(uint32_t id);
size_t image_bytes(uint32_t id);
void image_generate(uint32_t id, uint8_t *out);
/* src(side x side)에 필터를 적용해 dst에 쓴다. param의 하위 8비트만 연산에 쓴다 */
void filter_apply(uint32_t filter, uint32_t param, const uint8_t *src, uint8_t *dst, int side);

/* 요청열: 고정 시드로 클라이언트마다 같은 순서를 만든다 (두 IPC 버전에서 같다) */
typedef struct {
    uint64_t x;
    uint32_t client_id, seq;
    int hit_pct; /* 인기 키에서 고를 확률 (%) */
} reqgen_t;
void reqgen_init(reqgen_t *g, uint64_t seed, uint32_t client_id, int hit_pct);
void reqgen_next(reqgen_t *g, req_msg_t *m);

#define HOT_PARAM_LIMIT 256u /* 인기 키의 param은 이 값 미만, 미스용 param은 이 값 이상으로 매번 새로 만든다 */

#endif
