/* Minimal bionic subset used by MicNS (see build.sh). */
#ifndef MICNS_SYSROOT_STDLIB_H
#define MICNS_SYSROOT_STDLIB_H
#include <stddef.h>
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
long strtol(const char *s, char **end, int base);
void abort(void) __attribute__((__noreturn__));
#define abs(x) __builtin_abs(x)
#endif
