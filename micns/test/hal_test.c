/*
 * Tests the audio HAL hook (audio.primary.micns.so) against a fake HAL.
 *
 *   hal_test HOOK.so LIBMICNS.so FAKEHAL.so
 *
 * Built twice from this file: with -DFAKE_HAL as the fake audio.primary
 * module, otherwise as the driver. The driver points micns.conf at the fake
 * HAL, opens the hook the way libhardware does and opens input streams for
 * several sources. What read() returns must be bit-identical to the fake
 * HAL's signal run through libmicns directly with the expected effect
 * (sources MicNS handles) or to the untouched signal (all others).
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "android_effect_api.h"

#define HWMT 0x48574D54u
#define HWDT 0x48574454u

typedef struct hw_module_t hw_module_t;
typedef struct hw_device_t hw_device_t;
typedef struct {
    int (*open)(const hw_module_t *module, const char *id, hw_device_t **device);
} hw_module_methods_t;
struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version, hal_api_version;
    const char *id, *name, *author;
    hw_module_methods_t *methods;
    void *dso;
    uintptr_t reserved[25];
};
struct hw_device_t {
    uint32_t tag, version;
    hw_module_t *module;
    uintptr_t reserved[12];
    int (*close)(hw_device_t *device);
};
struct audio_config {
    uint32_t sample_rate, channel_mask, format;
};
struct audio_stream {
    uint32_t (*get_sample_rate)(const struct audio_stream *stream);
    void *set_sample_rate, *get_buffer_size;
    uint32_t (*get_channels)(const struct audio_stream *stream);
    uint32_t (*get_format)(const struct audio_stream *stream);
    void *more[9];
};
typedef struct audio_stream_in {
    struct audio_stream common;
    void *set_gain;
    long (*read)(struct audio_stream_in *stream, void *buffer, size_t bytes);
} audio_stream_in;
typedef struct audio_hw_device {
    hw_device_t common;
    void *before[13];
    int (*open_input_stream)(struct audio_hw_device *dev, int32_t handle, uint32_t devices,
                             struct audio_config *config, audio_stream_in **stream_in,
                             uint32_t flags, const char *address, int source);
    void (*close_input_stream)(struct audio_hw_device *dev, audio_stream_in *stream_in);
} audio_hw_device;

/* Noise plus a 200-ish Hz triangle: speech-band energy for the denoiser. */
static void gen(uint32_t *seed, uint32_t *pos, int16_t *out, size_t n) {
    size_t i;
    for (i = 0; i < n; i++, (*pos)++) {
        int tri = (int)(*pos % 240);
        *seed = *seed * 1664525u + 1013904223u;
        out[i] = (int16_t)((tri < 120 ? tri : 240 - tri) * 60 - 3600 + (int)(*seed >> 22) - 512);
    }
}

#ifdef FAKE_HAL

typedef struct {
    audio_stream_in s;
    struct audio_config cfg;
    uint32_t seed, pos;
} fake_in;

int fake_closed_ok; /* streams closed with their own read() back in place */

static uint32_t f_rate(const struct audio_stream *s) { return ((const fake_in *)s)->cfg.sample_rate; }
static uint32_t f_channels(const struct audio_stream *s) { return ((const fake_in *)s)->cfg.channel_mask; }
static uint32_t f_format(const struct audio_stream *s) { return ((const fake_in *)s)->cfg.format; }

static long f_read(audio_stream_in *in, void *buffer, size_t bytes) {
    fake_in *f = (fake_in *)in;
    gen(&f->seed, &f->pos, buffer, bytes / 2);
    return (long)bytes;
}

static int f_open_in(audio_hw_device *dev, int32_t handle, uint32_t devices, struct audio_config *config,
                     audio_stream_in **stream_in, uint32_t flags, const char *address, int source) {
    fake_in *f = calloc(1, sizeof(*f));
    (void)dev, (void)devices, (void)flags, (void)address, (void)source;
    f->s.common.get_sample_rate = f_rate;
    f->s.common.get_channels = f_channels;
    f->s.common.get_format = f_format;
    f->s.read = f_read;
    f->cfg = *config;
    f->seed = (uint32_t)handle;
    *stream_in = &f->s;
    return 0;
}

static void f_close_in(audio_hw_device *dev, audio_stream_in *in) {
    (void)dev;
    fake_closed_ok += in->read == f_read;
    free(in);
}

static audio_hw_device g_dev = {{HWDT, 0x0300, NULL, {0}, NULL}, {0}, f_open_in, f_close_in};

static int f_open(const hw_module_t *module, const char *id, hw_device_t **device) {
    (void)module;
    if (strcmp(id, "audio_hw_if")) return -22;
    *device = &g_dev.common; /* the same device every time, like Qualcomm's */
    return 0;
}

static hw_module_methods_t f_methods = {f_open};
hw_module_t HMI = {HWMT, 1, 0, "audio", "fake", "test", &f_methods, NULL, {0}};

#else

static int g_fails;
#define CHECK(c, what)                                \
    do {                                              \
        if (!(c)) {                                   \
            fprintf(stderr, "FAIL: %s\n", what);      \
            g_fails++;                                \
        }                                             \
    } while (0)

static const effect_uuid_t k_voice = {0x2470ae61, 0x735c, 0x41ba, 0x9565, {0x69, 0x23, 0xcc, 0x71, 0x3c, 0x54}};
static const effect_uuid_t k_pure = {0xc6b4d85a, 0x7071, 0x4520, 0x9121, {0x82, 0x8c, 0xff, 0x4f, 0x59, 0x6f}};
static const effect_uuid_t k_call = {0xd7bbe82a, 0x9292, 0x411e, 0x9656, {0x52, 0xb9, 0x12, 0xec, 0xe4, 0x27}};

static int cmd(effect_handle_t fx, uint32_t code, uint32_t size, void *data) {
    int32_t reply = -1;
    uint32_t rsize = sizeof(reply);
    int32_t ret = (*fx)->command(fx, code, size, data, &rsize, &reply);
    return ret ? ret : reply;
}

int main(int argc, char **argv) {
    static const struct {
        int source;
        uint32_t rate;
        int ch;
        const effect_uuid_t *uuid;
    } cases[] = {
        {1, 48000, 1, &k_voice},  /* mic: voice notes, recorders */
        {7, 16000, 1, &k_call},   /* voice_communication: calls, Discord */
        {6, 16000, 1, &k_pure},   /* voice_recognition */
        {0, 44100, 2, &k_voice},  /* default, stereo */
        {9, 48000, 1, NULL},      /* unprocessed: left alone */
        {5, 48000, 2, NULL},      /* camcorder: off by default */
        {1999, 16000, 1, NULL},   /* hotword: left alone */
    };
    const char *bits = sizeof(void *) == 8 ? "64" : "32";
    audio_effect_library_t *lib;
    hw_module_t *hmi;
    hw_device_t *d1 = NULL, *d2 = NULL;
    audio_hw_device *dev;
    int *closed_ok;
    void *hook, *fake;
    size_t c;
    FILE *f;

    if (argc != 4) {
        fprintf(stderr, "usage: %s HOOK.so LIBMICNS.so FAKEHAL.so\n", argv[0]);
        return 2;
    }
    f = fopen("/vendor/etc/micns.conf", "w");
    if (!f) {
        perror("/vendor/etc/micns.conf");
        return 2;
    }
    fprintf(f, "deep=1\nhal%s=%s\nfx%s=%s\nhal_voip=1\nhal_camcorder=0\n", bits, argv[3], bits, argv[2]);
    fclose(f);

    hook = dlopen(argv[1], RTLD_NOW);
    lib = (audio_effect_library_t *)dlsym(dlopen(argv[2], RTLD_NOW), "AELI");
    fake = dlopen(argv[3], RTLD_NOW);
    if (!hook || !lib || !fake) {
        fprintf(stderr, "load failed: %s\n", dlerror());
        return 2;
    }
    closed_ok = (int *)dlsym(fake, "fake_closed_ok");
    hmi = (hw_module_t *)dlsym(hook, "HMI");
    CHECK(hmi && hmi->tag == HWMT && !strcmp(hmi->id, "audio"), "module info");
    CHECK(hmi->methods->open(hmi, "audio_hw_if", &d1) == 0 && d1, "open device");
    dev = (audio_hw_device *)d1;
    {
        void *hooked = (void *)dev->open_input_stream;
        CHECK(hmi->methods->open(hmi, "audio_hw_if", &d2) == 0 && d2 == d1 &&
                  (void *)dev->open_input_stream == hooked,
              "second open keeps one hook");
    }

    for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint32_t rate = cases[c].rate, seed = 100 + (uint32_t)c, pos = 0;
        int ch = cases[c].ch;
        size_t frames = rate * 2, done = 0, k = 0;
        int16_t *out = calloc(frames * ch, 2), *want = calloc(frames * ch, 2);
        struct audio_config cfg = {rate, ch == 1 ? 0x10u : 0xCu, 1};
        audio_stream_in *in = NULL;
        effect_handle_t fx = NULL;
        char what[64];

        snprintf(what, sizeof(what), "source %d %u Hz x%d", cases[c].source, rate, ch);
        CHECK(dev->open_input_stream(dev, (int32_t)seed, 0x80000004u, &cfg, &in, 0, "", cases[c].source) == 0,
              what);
        gen(&seed, &pos, want, frames * ch);
        if (cases[c].uuid) {
            effect_config_t ec;
            memset(&ec, 0, sizeof(ec));
            ec.inputCfg.samplingRate = ec.outputCfg.samplingRate = rate;
            ec.inputCfg.channels = ec.outputCfg.channels = cfg.channel_mask;
            ec.inputCfg.format = ec.outputCfg.format = AUDIO_FORMAT_PCM_16_BIT;
            ec.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
            ec.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_WRITE;
            ec.inputCfg.mask = ec.outputCfg.mask = EFFECT_CONFIG_ALL;
            ec.inputCfg.buffer.frameCount = ec.outputCfg.buffer.frameCount = rate / 100;
            CHECK(lib->create_effect(cases[c].uuid, 0, (int32_t)(100 + c), &fx) == 0 &&
                      cmd(fx, EFFECT_CMD_INIT, 0, NULL) == 0 &&
                      cmd(fx, EFFECT_CMD_SET_CONFIG, sizeof(ec), &ec) == 0 &&
                      cmd(fx, EFFECT_CMD_ENABLE, 0, NULL) == 0,
                  "reference effect");
        }
        /* odd read sizes, as AudioFlinger's vary with the client */
        while (in && done < frames) {
            size_t n = rate / 100 * (1 + k % 3) + k % 7, got;
            if (n > frames - done) n = frames - done;
            got = (size_t)in->read(in, out + done * ch, n * ch * 2) / (ch * 2);
            if (fx && got) {
                audio_buffer_t b;
                b.frameCount = got;
                b.raw = want + done * ch;
                (*fx)->process(fx, &b, &b);
            }
            done += got;
            k++;
        }
        CHECK(done == frames && !memcmp(out, want, frames * ch * 2), what);
        if (cases[c].uuid) {
            /* and it really was processed: compare with the raw signal */
            uint32_t s2 = 100 + (uint32_t)c, p2 = 0;
            size_t diff = 0;
            gen(&s2, &p2, want, frames * ch);
            for (k = 0; k < frames * ch; k++) diff += out[k] != want[k];
            CHECK(diff > frames * ch / 2, "processed");
            lib->release_effect(fx);
        }
        if (in) dev->close_input_stream(dev, in);
        free(out);
        free(want);
    }
    CHECK(closed_ok && *closed_ok == (int)(sizeof(cases) / sizeof(cases[0])), "streams closed unhooked");
    remove("/vendor/etc/micns.conf");
    printf("hal hook: %d failure(s)\n", g_fails);
    return g_fails != 0;
}

#endif
