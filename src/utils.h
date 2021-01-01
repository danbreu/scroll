#ifndef __utils_h__
#define __utils_h__

#include <stdlib.h>
#include <stdio.h>

#ifdef DEBUG
#define _debug(M, ...) printf("DEBUG: %s:%d " M "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define _debug(M, ...)
#endif

#define _err(M, ...) fprintf(stderr, "ERROR: " M "\n", ##__VA_ARGS__)
#define _warn(M, ...) fprintf(stderr, "WARNING: " M "\n", ##__VA_ARGS__)
#define _log(M, ...) fprintf(stderr, "INFO: " M "\n", ##__VA_ARGS__)

#define _check(A, M, ...)  if(!(A)) {\
    _err(M, ##__VA_ARGS__);\
    goto error;\
}

#define _check_or_die(A, M, ...) if(!(A)) {\
    _err(M, ##__VA_ARGS__);\
    exit(1);\
}

void *_malloc_or_die(size_t s);

#endif