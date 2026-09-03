#include "march_ffi.h"
#include <stdio.h>
#include <stdint.h>
/* NativeArray layout (runtime/march_runtime.c): hdr {rc i64, tag i32, pad i32}, len i64 @16, kind @24, data @32 */
int64_t probe_f32_len(void *arr) { return *(int64_t *)((char *)arr + 16); }
double probe_f32_sum(void *arr) {
  int64_t n = *(int64_t *)((char *)arr + 16); float *d = (float *)((char *)arr + 32);
  double s = 0; for (int64_t i = 0; i < n; i++) s += d[i]; return s;
}
int64_t probe_u8_first(void *arr) { return *((uint8_t *)((char *)arr + 32)); }
int64_t probe_bytes_len(march_value b) { return (int64_t)march_bytes_borrow(b).len; }
void probe_unit(double f) { printf("unit got %f\n", f); }
