#include "march_ffi.h"
#include <stdio.h>
int64_t p_effect(int64_t tag) { fprintf(stderr, "p_effect called with %lld\n", (long long)tag); return tag; }
int64_t p_effect0(void) { fprintf(stderr, "p_effect0 called\n"); return 1; }
