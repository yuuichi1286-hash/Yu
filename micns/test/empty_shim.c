/* Placeholder for bionic's libc.so/libm.so when loading the Android build
   on glibc: the real symbols come from the harness' libc.so.6/libm.so.6. */
void micns_shim_placeholder(void) {}

/* Bionic only; the HAL hook asks for ro.* properties when micns.conf does
   not name the real HAL. Tests always name it. */
int __system_property_get(const char *name, char *value) {
    (void)name;
    value[0] = 0;
    return 0;
}
