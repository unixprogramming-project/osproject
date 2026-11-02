#include "common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void die(const char *what)
{
    fprintf(stderr, "error: %s: %s\n", what, strerror(errno));
    exit(1);
}

long env_long(const char *name, long fallback)
{
    const char *s = getenv(name);
    if (!s || !*s)
        return fallback;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end || v <= 0) {
        fprintf(stderr, "error: invalid %s=%s\n", name, s);
        exit(1);
    }
    return v;
}

void msleep(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

static inline uint64_t next_word(uint64_t *x)
{
    *x ^= *x << 13;
    *x ^= *x >> 7;
    *x ^= *x << 17;
    return *x;
}

static inline uint64_t seed_state(uint64_t seed)
{
    return (seed * 0x9E3779B97F4A7C15ull) | 1ull;
}

void pattern_fill(void *buf, size_t len, uint64_t seed)
{
    uint8_t *p = buf;
    uint64_t x = seed_state(seed);
    while (len) {
        uint64_t w = next_word(&x);
        size_t n = len < 8 ? len : 8;
        memcpy(p, &w, n);
        p += n;
        len -= n;
    }
}

int pattern_ok(const void *buf, size_t len, uint64_t seed)
{
    const uint8_t *p = buf;
    uint64_t x = seed_state(seed);
    while (len) {
        uint64_t w = next_word(&x);
        size_t n = len < 8 ? len : 8;
        if (memcmp(p, &w, n) != 0)
            return 0;
        p += n;
        len -= n;
    }
    return 1;
}

long read_vm_kb(const char *field)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return -1;
    char line[256];
    long kb = -1;
    size_t n = strlen(field);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, field, n) == 0 && line[n] == ':') {
            kb = strtol(line + n + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    return kb;
}

uint64_t clock_overhead_ns(void)
{
    enum { N = 100000 };
    uint64_t a = now_ns(), b = a;
    for (int i = 0; i < N; i++)
        b = now_ns();
    return (b - a) / N;
}

static double tv_us(struct timeval t)
{
    return (double)t.tv_sec * 1e6 + (double)t.tv_usec;
}

usage_t usage_now(void)
{
    struct rusage r;
    usage_t u = {0, 0, 0, 0};
    if (getrusage(RUSAGE_SELF, &r) == 0) {
        u.user_us = tv_us(r.ru_utime);
        u.sys_us = tv_us(r.ru_stime);
        u.nvcsw = r.ru_nvcsw;
        u.nivcsw = r.ru_nivcsw;
    }
    return u;
}

usage_t usage_diff(usage_t e, usage_t s)
{
    usage_t d = {e.user_us - s.user_us, e.sys_us - s.sys_us, e.nvcsw - s.nvcsw, e.nivcsw - s.nivcsw};
    return d;
}
