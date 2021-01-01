#include "utils.h"

void *_malloc_or_die(size_t s) {
    void *p = malloc(s);

    _check_or_die(p, "Failed to allocate memory with size %zu", s);

    return p;
}