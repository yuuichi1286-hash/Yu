/* Host stand-in for Android's liblog (tests only). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list ap;
    if (getenv("MICNS_QUIET")) return 0;
    va_start(ap, fmt);
    fprintf(stderr, "[%d/%s] ", prio, tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return 0;
}
