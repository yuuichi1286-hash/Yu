#ifndef MICNS_SYSROOT_STDIO_H
#define MICNS_SYSROOT_STDIO_H
#include <stddef.h>
typedef struct __sFILE FILE;
FILE *fopen(const char *path, const char *mode);
char *fgets(char *buf, int size, FILE *f);
int fclose(FILE *f);
#endif
