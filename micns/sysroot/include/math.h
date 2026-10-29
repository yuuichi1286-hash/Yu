#ifndef MICNS_SYSROOT_MATH_H
#define MICNS_SYSROOT_MATH_H
#define M_PI 3.14159265358979323846
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#define isnan(x) __builtin_isnan(x)
double sqrt(double x);
double exp(double x);
double log(double x);
double log10(double x);
double pow(double x, double y);
double cos(double x);
double sin(double x);
double floor(double x);
double fabs(double x);
float sqrtf(float x);
float expf(float x);
float logf(float x);
float log10f(float x);
float powf(float x, float y);
float cosf(float x);
float sinf(float x);
float floorf(float x);
float fabsf(float x);
#endif
