#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <sys/resource.h>
#include <time.h>

#define CONNECT_TIMEOUT_MS 10000

/* 측정 구간에서 쓰는 시계. 강의 Chap04와 안내문 5장의 CLOCK_MONOTONIC */
static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void die(const char *what);
long env_long(const char *name, long fallback);
void msleep(int ms);

void pattern_fill(void *buf, size_t len, uint64_t seed);
int pattern_ok(const void *buf, size_t len, uint64_t seed);

long read_vm_kb(const char *field); /* /proc/self/status 의 VmRSS 등 */
uint64_t clock_overhead_ns(void);

typedef struct {
    double user_us, sys_us;
    long nvcsw, nivcsw;
} usage_t;
usage_t usage_now(void);
usage_t usage_diff(usage_t end, usage_t start);

#endif
