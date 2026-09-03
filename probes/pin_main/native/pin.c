/* pin.c — G15 probe shim: expose the two runtime facts the probe reports. */
#include "march_ffi.h"
#include <pthread.h>
#include <stdint.h>

extern int64_t march_live_allocs(void);

/* 1 when the calling OS thread is the process main thread (Cocoa/GLFW need it). */
int64_t pm_is_main_thread(void) { return pthread_main_np() ? 1 : 0; }

int64_t pm_live_allocs(void) { return march_live_allocs(); }
