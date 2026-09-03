#include "march_ffi.h"
#include <stdio.h>
#include <stdint.h>
static int64_t len(void *a) { return *(int64_t *)((char *)a + 16); }
void *p3(void *dst, int64_t di, void *src) { fprintf(stderr, "p3: dst rc=%lld len=%lld di=%lld src len=%lld\n", (long long)*(int64_t*)dst, (long long)len(dst), (long long)di, (long long)len(src)); return dst; }
void *p4(void *dst, int64_t di, void *src, int64_t si) { fprintf(stderr, "p4: dst rc=%lld len=%lld di=%lld src len=%lld si=%lld\n", (long long)*(int64_t*)dst, (long long)len(dst), (long long)di, (long long)len(src), (long long)si); return dst; }
void *p5(void *dst, int64_t di, void *src, int64_t si, int64_t n) { fprintf(stderr, "p5: dst rc=%lld len=%lld di=%lld src len=%lld si=%lld n=%lld\n", (long long)*(int64_t*)dst, (long long)len(dst), (long long)di, (long long)len(src), (long long)si, (long long)n); return dst; }
void *p5b(void *dst, void *src, int64_t di, int64_t si, int64_t n) { fprintf(stderr, "p5b: dst rc=%lld len=%lld src len=%lld di=%lld si=%lld n=%lld\n", (long long)*(int64_t*)dst, (long long)len(dst), (long long)len(src), (long long)di, (long long)si, (long long)n); return dst; }
