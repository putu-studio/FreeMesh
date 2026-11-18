// Codec2 asks for these when __EMBEDDED__ is set (it keeps its big tables in flash then)
#include <stdlib.h>
void *codec2_malloc(size_t size) { return malloc(size); }
void *codec2_calloc(size_t n, size_t size) { return calloc(n, size); }
void codec2_free(void *p) { free(p); }
