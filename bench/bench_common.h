#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include <stdint.h>

#define MODE_PINGPONG 0
#define MODE_THROUGHPUT 1
#define VERIFY_MIN_SIZE 8

/* 검증용 패턴 시드: 클라이언트나 순번이 섞이면 검출된다 */
static inline uint64_t msg_seed(int client_id, uint64_t seq)
{
    return ((uint64_t)(client_id + 1) << 40) ^ seq;
}

#endif
