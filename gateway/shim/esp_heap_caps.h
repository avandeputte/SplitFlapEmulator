#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM  (1 << 10)
#define MALLOC_CAP_8BIT    (1 << 2)
#define MALLOC_CAP_INTERNAL (1 << 11)
static inline void* heap_caps_malloc(size_t n, unsigned) { return malloc(n); }
static inline void* heap_caps_realloc(void* p, size_t n, unsigned) { return realloc(p, n); }
static inline void  heap_caps_free(void* p) { free(p); }
