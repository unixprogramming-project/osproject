#include "app_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int image_side(uint32_t id)
{
    if (id < 32)
        return 128;
    if (id < 52)
        return 512;
    return 1024;
}

size_t image_bytes(uint32_t id)
{
    size_t s = (size_t)image_side(id);
    return s * s;
}

/* 결정적 흑백 이미지: 그라데이션 + ID별 무늬. 라이브러리 없이 바이트 배열로 만든다 */
void image_generate(uint32_t id, uint8_t *out)
{
    int s = image_side(id);
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++)
            out[(size_t)y * s + x] = (uint8_t)((x * 255 / s + y * 3 + (int)id * 17 + ((x ^ y) & 31)) & 0xff);
}

void filter_apply(uint32_t filter, uint32_t param, const uint8_t *src, uint8_t *dst, int side)
{
    size_t n = (size_t)side * side;
    int p = (int)(param & 0xff);
    switch (filter) {
    case 0: /* 반전 + 밝기 보정 */
        for (size_t i = 0; i < n; i++) {
            int v = 255 - src[i] + (p - 128);
            dst[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
        break;
    case 1: /* 임계값 이진화 */
        for (size_t i = 0; i < n; i++)
            dst[i] = src[i] >= p ? 255 : 0;
        break;
    default: /* 3x3 박스 블러를 원본과 p/255 비율로 섞음. 가장자리는 원본 유지 */
        memcpy(dst, src, n);
        for (int y = 1; y < side - 1; y++) {
            const uint8_t *a = src + (size_t)(y - 1) * side, *b = a + side, *c = b + side;
            uint8_t *d = dst + (size_t)y * side;
            for (int x = 1; x < side - 1; x++) {
                int sum = a[x - 1] + a[x] + a[x + 1] + b[x - 1] + b[x] + b[x + 1] + c[x - 1] + c[x] + c[x + 1];
                d[x] = (uint8_t)((sum / 9 * p + b[x] * (255 - p)) / 255);
            }
        }
        break;
    }
}

static uint32_t rnd(reqgen_t *g)
{
    g->x ^= g->x << 13;
    g->x ^= g->x >> 7;
    g->x ^= g->x << 17;
    return (uint32_t)(g->x >> 32);
}

void reqgen_init(reqgen_t *g, uint64_t seed, uint32_t client_id, int hit_pct)
{
    g->x = ((seed + 1) * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)(client_id + 1) << 32) ^ 0x5bd1e995ull;
    if (!g->x)
        g->x = 1;
    g->client_id = client_id;
    g->seq = 0;
    g->hit_pct = hit_pct;
    g->size_class = -1;
    const char *s = getenv("APP_SIZE_CLASS"); /* 응답 크기 구간 고정: 16, 256, 1024 (KB) */
    if (s && *s) {
        int kb = atoi(s);
        g->size_class = kb == 16 ? 0 : kb == 256 ? 1 : kb == 1024 ? 2 : -2;
        if (g->size_class == -2) {
            fprintf(stderr, "APP_SIZE_CLASS must be 16, 256 or 1024\n");
            exit(2);
        }
    }
}

/*
 * 크기 분포: 16KB 50%, 256KB 35%, 1MB 15% (요청 기준).
 * 인기 키: 크기 구간마다 8개씩 24개 (구간 안 앞쪽 이미지, 필터·param 고정) -> 워밍업 뒤 항상 적중.
 * 미스: 구간 안 임의 이미지·필터 + 클라이언트와 순번으로 만든 새 param -> 캐시에 없는 키.
 */
void reqgen_next(reqgen_t *g, req_msg_t *m)
{
    memset(m, 0, sizeof *m);
    uint32_t r = rnd(g) % 100, base, count;
    if (g->size_class >= 0) /* 구간 고정: 난수는 그대로 소비해 나머지 요청열(적중 여부 등)은 기본과 같게 둔다 */
        r = g->size_class == 0 ? 0 : g->size_class == 1 ? 50 : 85;
    if (r < 50) { base = 0; count = 32; }
    else if (r < 85) { base = 32; count = 20; }
    else { base = 52; count = 12; }
    m->type = MSG_REQ;
    m->req_id = g->seq;
    if ((int)(rnd(g) % 100) < g->hit_pct) {
        uint32_t k = rnd(g) % 8;
        m->image_id = base + k;
        m->filter = k % N_FILTERS;
        m->param = 40 + k * 20;
    } else {
        m->image_id = base + rnd(g) % count;
        m->filter = rnd(g) % N_FILTERS;
        m->param = HOT_PARAM_LIMIT + ((g->client_id << 24) | (g->seq & 0xffffff));
    }
    g->seq++;
}
