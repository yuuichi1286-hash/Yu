/*
 * MicNS audio HAL hook.
 *
 * Qualcomm's audio HAL never runs software pre-processing effects, so on
 * those phones libmicns.so cannot reach the microphone through
 * audio_effects.xml. This library sits in front of the real HAL instead:
 * the module sets ro.hardware.audio.primary=micns, libhardware then loads
 * audio.primary.micns.so, which loads the real audio.primary.<platform>.so
 * and hands out its device untouched except for open/close_input_stream.
 * Each input stream that should be processed gets its read() wrapped: the
 * PCM the HAL returns (already through the DSP's own echo canceller and
 * noise suppressor) runs in place through a MicNS effect instance created
 * from libmicns.so with the regular effect API, before AudioFlinger sees it.
 *
 * Only the leading members of the public HAL structs are used, which have
 * not changed since Android 5.0, and none of the HAL's private data, so the
 * hook does not depend on the HAL version. Settings come from micns.conf:
 *   hal64= / hal32=   the real HAL (else found the way libhardware does)
 *   fx64=  / fx32=    libmicns.so
 *   hal_voip=1        also process internet calls (voice_communication)
 *   hal_camcorder=0   also process video recording
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <android/log.h>

#include "android_effect_api.h"

#define TAG "MicNS-HAL"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

/* From bionic; declared here to keep the sysroot small. */
void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);
char *dlerror(void);
int access(const char *path, int mode);
int __system_property_get(const char *name, char *value);

#if defined(__LP64__)
#define DL_NOW 2 /* RTLD_NOW */
#define LIBDIR "lib64"
#define BITS "64"
#else
#define DL_NOW 0 /* RTLD_NOW is 0 in 32-bit bionic */
#define LIBDIR "lib"
#define BITS "32"
#endif

#define PROP_VALUE_MAX 92
#define HARDWARE_MODULE_TAG 0x48574D54u /* "HWMT" */
#define AUDIO_DEVICE_API_VERSION_3_0 0x0300u
#define AUDIO_INPUT_FLAG_MMAP_NOIRQ 0x10u /* shared memory, read() unused */

/* ------------------------------------------------------------------------- */
/* Public HAL structs (hardware.h, audio.h), leading members only            */
/* ------------------------------------------------------------------------- */

struct hw_module_t;
struct hw_device_t;

typedef struct hw_module_methods_t {
    int (*open)(const struct hw_module_t *module, const char *id, struct hw_device_t **device);
} hw_module_methods_t;

typedef struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    hw_module_methods_t *methods;
    void *dso;
    uintptr_t reserved[32 - 7];
} hw_module_t;

typedef struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct hw_module_t *module;
    uintptr_t reserved[12];
    int (*close)(struct hw_device_t *device);
} hw_device_t;

struct audio_config;

struct audio_stream {
    uint32_t (*get_sample_rate)(const struct audio_stream *stream);
    void *set_sample_rate, *get_buffer_size;
    uint32_t (*get_channels)(const struct audio_stream *stream);
    uint32_t (*get_format)(const struct audio_stream *stream);
    void *more[9]; /* set_format ... remove_audio_effect */
};

typedef struct audio_stream_in audio_stream_in;
typedef long (*read_fn)(audio_stream_in *stream, void *buffer, size_t bytes);

struct audio_stream_in {
    struct audio_stream common;
    void *set_gain;
    read_fn read;
};

typedef struct audio_hw_device audio_hw_device;
typedef int (*open_in_fn)(audio_hw_device *dev, int32_t handle, uint32_t devices,
                          struct audio_config *config, audio_stream_in **stream_in,
                          uint32_t flags, const char *address, int source);
typedef void (*close_in_fn)(audio_hw_device *dev, audio_stream_in *stream_in);

struct audio_hw_device {
    hw_device_t common;
    void *before[13]; /* get_supported_devices ... close_output_stream */
    open_in_fn open_input_stream;
    close_in_fn close_input_stream;
};

/* audio_source_t */
enum {
    SRC_DEFAULT = 0,
    SRC_MIC = 1,
    SRC_CAMCORDER = 5,
    SRC_VOICE_RECOGNITION = 6,
    SRC_VOICE_COMMUNICATION = 7,
};

static const effect_uuid_t k_voice = {0x2470ae61, 0x735c, 0x41ba, 0x9565, {0x69, 0x23, 0xcc, 0x71, 0x3c, 0x54}};
static const effect_uuid_t k_pure = {0xc6b4d85a, 0x7071, 0x4520, 0x9121, {0x82, 0x8c, 0xff, 0x4f, 0x59, 0x6f}};
static const effect_uuid_t k_call = {0xd7bbe82a, 0x9292, 0x411e, 0x9656, {0x52, 0xb9, 0x12, 0xec, 0xe4, 0x27}};

/* ------------------------------------------------------------------------- */
/* Settings                                                                  */
/* ------------------------------------------------------------------------- */

static struct {
    char hal[256];
    char fx[256];
    int voip;
    int camcorder;
} g_cfg = {"", "", 1, 0};

static const char *const g_conf_paths[] = {
    "/vendor/etc/micns.conf",
    "/odm/etc/micns.conf",
    "/system/vendor/etc/micns.conf",
    "/system/etc/micns.conf",
};

static char *trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

/* Bounded string append; out always stays terminated. */
static void append(char *out, size_t size, const char *s) {
    size_t n = strlen(out), m = strlen(s);
    if (n + m >= size) m = size - 1 - n;
    memcpy(out + n, s, m);
    out[n + m] = 0;
}

static void conf_load(void) {
    size_t i;
    for (i = 0; i < sizeof(g_conf_paths) / sizeof(g_conf_paths[0]); i++) {
        char line[300];
        FILE *f = fopen(g_conf_paths[i], "r");
        if (!f) continue;
        while (fgets(line, sizeof(line), f)) {
            char *key = trim(line), *val, *eq = strchr(key, '=');
            if (*key == '#' || !eq) continue;
            *eq = 0;
            key = trim(key);
            val = trim(eq + 1);
            if (!strcmp(key, "hal" BITS)) {
                g_cfg.hal[0] = 0;
                append(g_cfg.hal, sizeof(g_cfg.hal), val);
            } else if (!strcmp(key, "fx" BITS)) {
                g_cfg.fx[0] = 0;
                append(g_cfg.fx, sizeof(g_cfg.fx), val);
            } else if (!strcmp(key, "hal_voip")) {
                g_cfg.voip = *val == '1';
            } else if (!strcmp(key, "hal_camcorder")) {
                g_cfg.camcorder = *val == '1';
            }
        }
        fclose(f);
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* The real HAL and libmicns.so                                              */
/* ------------------------------------------------------------------------- */

static hw_module_t *g_real;
static audio_effect_library_t *g_fx;
static open_in_fn g_open_in;
static close_in_fn g_close_in;

static hw_module_t *load_module(const char *path) {
    void *h = dlopen(path, DL_NOW);
    hw_module_t *m;
    if (!h) {
        LOGW("cannot load %s: %s", path, dlerror());
        return NULL;
    }
    m = (hw_module_t *)dlsym(h, "HMI");
    if (!m || m->tag != HARDWARE_MODULE_TAG || !m->methods || !m->methods->open) {
        LOGW("%s is not a HAL module", path);
        return NULL;
    }
    m->dso = h; /* what libhardware would have done */
    LOGI("real audio HAL: %s", path);
    return m;
}

/* The configured HAL, else the one libhardware would pick without us. */
static hw_module_t *find_real(void) {
    static const char *const props[] = {"ro.hardware", "ro.product.board", "ro.board.platform", "ro.arch"};
    static const char *const dirs[] = {"/odm/" LIBDIR "/hw/", "/vendor/" LIBDIR "/hw/", "/system/" LIBDIR "/hw/"};
    char v[PROP_VALUE_MAX], path[256];
    size_t i, d;
    if (g_cfg.hal[0]) {
        hw_module_t *m = load_module(g_cfg.hal);
        if (m) return m;
    }
    for (i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
        if (__system_property_get(props[i], v) <= 0 || !strcmp(v, "micns")) continue;
        for (d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
            path[0] = 0;
            append(path, sizeof(path), dirs[d]);
            append(path, sizeof(path), "audio.primary.");
            append(path, sizeof(path), v);
            append(path, sizeof(path), ".so");
            if (access(path, 0) == 0) return load_module(path);
        }
    }
    return NULL;
}

static int load_fx(void) {
    static const char *const paths[] = {"/vendor/" LIBDIR "/soundfx/libmicns.so",
                                        "/system/" LIBDIR "/soundfx/libmicns.so"};
    size_t i;
    for (i = 0; !g_fx && i <= sizeof(paths) / sizeof(paths[0]); i++) {
        const char *p = i ? paths[i - 1] : g_cfg.fx;
        void *h = *p ? dlopen(p, DL_NOW) : NULL;
        if (h) g_fx = (audio_effect_library_t *)dlsym(h, "AELI");
        if (g_fx && g_fx->tag != AUDIO_EFFECT_LIBRARY_TAG) g_fx = NULL;
    }
    if (!g_fx) LOGW("libmicns.so not found");
    return g_fx != NULL;
}

/* ------------------------------------------------------------------------- */
/* Input streams                                                             */
/* ------------------------------------------------------------------------- */

#define MAX_STREAMS 32

typedef struct {
    audio_stream_in *in;
    read_fn read;
    effect_handle_t fx;
    uint32_t frame_bytes;
} slot_t;

static slot_t g_slots[MAX_STREAMS];
static read_fn g_any_read; /* fallback should a stream ever be unknown */
static int g_lock;

static void lock(void) {
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {
    }
}

static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

static long hook_read(audio_stream_in *in, void *buffer, size_t bytes) {
    read_fn read = g_any_read;
    effect_handle_t fx = NULL;
    uint32_t frame_bytes = 2;
    long n;
    int i;
    lock();
    for (i = 0; i < MAX_STREAMS; i++) {
        if (g_slots[i].in == in) {
            read = g_slots[i].read;
            fx = g_slots[i].fx;
            frame_bytes = g_slots[i].frame_bytes;
            break;
        }
    }
    unlock();
    if (!read) return -ENODEV;
    n = read(in, buffer, bytes);
    if (n > 0 && fx) {
        audio_buffer_t b;
        b.frameCount = (size_t)n / frame_bytes;
        b.raw = buffer;
        if (b.frameCount) (*fx)->process(fx, &b, &b);
    }
    return n;
}

static int fx_cmd(effect_handle_t fx, uint32_t code, uint32_t size, void *data) {
    int32_t reply = -1;
    uint32_t reply_size = sizeof(reply);
    int32_t ret = (*fx)->command(fx, code, size, data, &reply_size, &reply);
    return ret ? ret : reply;
}

static int fx_setup(effect_handle_t fx, uint32_t rate, int ch) {
    effect_config_t c;
    memset(&c, 0, sizeof(c));
    c.inputCfg.samplingRate = c.outputCfg.samplingRate = rate;
    c.inputCfg.channels = c.outputCfg.channels = ch == 1 ? 0x10u : 0xCu; /* IN_MONO, IN_STEREO */
    c.inputCfg.format = c.outputCfg.format = AUDIO_FORMAT_PCM_16_BIT;
    c.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
    c.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_WRITE;
    c.inputCfg.mask = c.outputCfg.mask = EFFECT_CONFIG_ALL;
    c.inputCfg.buffer.frameCount = c.outputCfg.buffer.frameCount = rate / 100;
    if (fx_cmd(fx, EFFECT_CMD_INIT, 0, NULL)) return -1;
    if (fx_cmd(fx, EFFECT_CMD_SET_CONFIG, sizeof(c), &c)) return -1;
    return fx_cmd(fx, EFFECT_CMD_ENABLE, 0, NULL);
}

static int channel_count(uint32_t mask) {
    int n = 0;
    for (mask &= 0x3FFFFFFFu; mask; mask &= mask - 1) n++; /* minus the representation bits */
    return n;
}

static void attach(audio_stream_in *in, int32_t io, uint32_t flags, int source) {
    const effect_uuid_t *uuid = NULL;
    effect_handle_t fx = NULL;
    uint32_t rate, format;
    int ch, i;

    switch (source) {
    case SRC_DEFAULT:
    case SRC_MIC: uuid = &k_voice; break;
    case SRC_VOICE_RECOGNITION: uuid = &k_pure; break;
    case SRC_VOICE_COMMUNICATION: uuid = g_cfg.voip ? &k_call : NULL; break;
    case SRC_CAMCORDER: uuid = g_cfg.camcorder ? &k_pure : NULL; break;
    default: break; /* unprocessed, call recording, hotword...: untouched */
    }
    if (!uuid || (flags & AUDIO_INPUT_FLAG_MMAP_NOIRQ) || in->read == hook_read || !load_fx()) return;
    rate = in->common.get_sample_rate(&in->common);
    format = in->common.get_format(&in->common);
    ch = channel_count(in->common.get_channels(&in->common));
    if (format != AUDIO_FORMAT_PCM_16_BIT || ch < 1 || ch > 2) {
        LOGI("input %d: format %#x x%d not processed", (int)io, (unsigned)format, ch);
        return;
    }
    if (g_fx->create_effect(uuid, 0, io, &fx) || !fx) return;
    if (fx_setup(fx, rate, ch)) {
        g_fx->release_effect(fx);
        return;
    }
    lock();
    for (i = 0; i < MAX_STREAMS && g_slots[i].in; i++) {
    }
    if (i < MAX_STREAMS) {
        g_slots[i].in = in;
        g_slots[i].read = in->read;
        g_slots[i].fx = fx;
        g_slots[i].frame_bytes = 2u * (uint32_t)ch;
        if (!g_any_read) g_any_read = in->read;
        in->read = hook_read;
    }
    unlock();
    if (i == MAX_STREAMS) {
        g_fx->release_effect(fx);
        return;
    }
    LOGI("input %d: source %d, %u Hz x%d through MicNS", (int)io, source, (unsigned)rate, ch);
}

static int hook_open_in(audio_hw_device *dev, int32_t handle, uint32_t devices,
                        struct audio_config *config, audio_stream_in **stream_in, uint32_t flags,
                        const char *address, int source) {
    int ret = g_open_in(dev, handle, devices, config, stream_in, flags, address, source);
    if (ret == 0 && stream_in && *stream_in) attach(*stream_in, handle, flags, source);
    return ret;
}

static void hook_close_in(audio_hw_device *dev, audio_stream_in *in) {
    effect_handle_t fx = NULL;
    int i;
    lock();
    for (i = 0; i < MAX_STREAMS; i++) {
        if (g_slots[i].in == in) {
            fx = g_slots[i].fx;
            in->read = g_slots[i].read;
            memset(&g_slots[i], 0, sizeof(g_slots[i]));
            break;
        }
    }
    unlock();
    if (fx) g_fx->release_effect(fx);
    g_close_in(dev, in);
}

/* ------------------------------------------------------------------------- */
/* Module                                                                    */
/* ------------------------------------------------------------------------- */

__attribute__((visibility("default"))) hw_module_t HMI;

static int hook_open(const hw_module_t *module, const char *id, hw_device_t **device) {
    audio_hw_device *adev;
    int ret;
    (void)module;
    if (!g_real) {
        conf_load();
        g_real = find_real();
        if (!g_real) return -ENOENT;
        HMI.module_api_version = g_real->module_api_version;
        HMI.hal_api_version = g_real->hal_api_version;
    }
    ret = g_real->methods->open(g_real, id, device);
    if (ret || !device || !*device || strcmp(id, "audio_hw_if") ||
        (*device)->version < AUDIO_DEVICE_API_VERSION_3_0)
        return ret;
    /* The HAL may hand out the same device again: never wrap ourselves. */
    adev = (audio_hw_device *)*device;
    if (adev->open_input_stream && adev->open_input_stream != hook_open_in &&
        adev->close_input_stream && adev->close_input_stream != hook_close_in) {
        g_open_in = adev->open_input_stream;
        g_close_in = adev->close_input_stream;
        adev->open_input_stream = hook_open_in;
        adev->close_input_stream = hook_close_in;
        LOGI("capture path hooked (device version %#x)", (unsigned)(*device)->version);
    }
    return 0;
}

static hw_module_methods_t g_methods = {hook_open};

hw_module_t HMI = {
    HARDWARE_MODULE_TAG,
    1, /* AUDIO_MODULE_API_VERSION_0_1, replaced by the real HAL's */
    0, /* HARDWARE_HAL_API_VERSION */
    "audio",
    "MicNS audio HAL hook",
    "MicNS",
    &g_methods,
    NULL,
    {0},
};
