#ifndef MICNS_SYSROOT_ANDROID_LOG_H
#define MICNS_SYSROOT_ANDROID_LOG_H
enum {
    ANDROID_LOG_DEBUG = 3,
    ANDROID_LOG_INFO = 4,
    ANDROID_LOG_WARN = 5,
    ANDROID_LOG_ERROR = 6,
};
int __android_log_print(int prio, const char *tag, const char *fmt, ...)
    __attribute__((__format__(printf, 3, 4)));
#endif
