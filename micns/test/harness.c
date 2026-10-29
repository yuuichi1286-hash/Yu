/*
 * Test driver for libmicns.so. Loads the library with dlopen exactly like
 * Android's effect factory does (AELI symbol), then plays the role of an
 * audio HAL: INIT, SET_CONFIG, ENABLE, process() in arbitrary slices.
 *
 *   harness LIB run VARIANT RATE CH PATTERN MODE IN.raw OUT.raw
 *       VARIANT voice|pure, PATTERN fixed:N | rand:SEED:MAX, MODE sep|inplace
 *   harness LIB selftest
 *       command/edge-case checks plus a randomized configuration stress run
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/android_effect_api.h"

/* errno.h needs per-arch kernel headers; the value is the same everywhere. */
#define EINVAL 22

static const effect_uuid_t kVoice = {0x2470ae61, 0x735c, 0x41ba, 0x9565, {0x69, 0x23, 0xcc, 0x71, 0x3c, 0x54}};
static const effect_uuid_t kPure = {0xc6b4d85a, 0x7071, 0x4520, 0x9121, {0x82, 0x8c, 0xff, 0x4f, 0x59, 0x6f}};
static const effect_uuid_t kCall = {0xd7bbe82a, 0x9292, 0x411e, 0x9656, {0x52, 0xb9, 0x12, 0xec, 0xe4, 0x27}};
static const effect_uuid_t kTypeNs = {0x58b4b260, 0x8e06, 0x11e0, 0xaa8e, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};

static const audio_effect_library_t *g_lib;
static int g_failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                      \
            fprintf(stderr, "\n");                             \
            g_failures++;                                      \
        }                                                      \
    } while (0)

static uint32_t rng_state = 12345;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static uint32_t in_mask_for(int ch) {
    switch (ch) {
    case 1: return 0x10;       /* AUDIO_CHANNEL_IN_MONO */
    case 2: return 0xC;        /* AUDIO_CHANNEL_IN_STEREO */
    default: return 0x80000000u | ((1u << ch) - 1); /* index mask */
    }
}

static int cmd_int(effect_handle_t e, uint32_t code, uint32_t size, void *data) {
    int reply = -1234;
    uint32_t rsize = sizeof(reply);
    int ret = (*e)->command(e, code, size, data, &rsize, &reply);
    if (ret) return ret;
    return reply;
}

static int configure(effect_handle_t e, uint32_t rate, int ch, uint8_t format) {
    effect_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.inputCfg.samplingRate = cfg.outputCfg.samplingRate = rate;
    cfg.inputCfg.channels = cfg.outputCfg.channels = in_mask_for(ch);
    cfg.inputCfg.format = cfg.outputCfg.format = format;
    cfg.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
    cfg.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_WRITE;
    cfg.inputCfg.mask = cfg.outputCfg.mask = EFFECT_CONFIG_ALL;
    cfg.inputCfg.buffer.frameCount = cfg.outputCfg.buffer.frameCount = rate / 100;
    return cmd_int(e, EFFECT_CMD_SET_CONFIG, sizeof(cfg), &cfg);
}

static effect_handle_t create(const effect_uuid_t *uuid) {
    effect_handle_t e = NULL;
    int ret = g_lib->create_effect(uuid, 7, 13, &e);
    if (ret || !e) {
        fprintf(stderr, "create failed: %d\n", ret);
        exit(2);
    }
    return e;
}

static int16_t *read_file(const char *path, size_t *samples) {
    FILE *f = fopen(path, "rb");
    long len;
    int16_t *buf;
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(len + 16);
    if (fread(buf, 1, len, f) != (size_t)len) exit(2);
    fclose(f);
    *samples = len / 2;
    return buf;
}

static int run(int argc, char **argv) {
    const char *variant = argv[3];
    uint32_t rate = (uint32_t)atoi(argv[4]);
    int ch = atoi(argv[5]);
    const char *pattern = argv[6];
    int inplace = !strcmp(argv[7], "inplace");
    size_t samples, frames, pos = 0;
    int16_t *in = read_file(argv[8], &samples);
    int16_t *out = calloc(samples + 16, sizeof(int16_t));
    effect_handle_t e = create(!strcmp(variant, "pure") ? &kPure : !strcmp(variant, "call") ? &kCall : &kVoice);
    unsigned fixed = 0, maxr = 0;
    FILE *f;
    (void)argc;

    if (!strncmp(pattern, "fixed:", 6)) fixed = (unsigned)atoi(pattern + 6);
    else if (!strncmp(pattern, "rand:", 5)) {
        rng_state = (uint32_t)strtoul(pattern + 5, NULL, 10) | 1;
        maxr = (unsigned)atoi(strchr(pattern + 5, ':') + 1);
    }
    frames = samples / ch;
    CHECK(cmd_int(e, EFFECT_CMD_INIT, 0, NULL) == 0, "init");
    CHECK(configure(e, rate, ch, AUDIO_FORMAT_PCM_16_BIT) == 0, "config");
    CHECK(cmd_int(e, EFFECT_CMD_ENABLE, 0, NULL) == 0, "enable");

    while (pos < frames) {
        size_t n = fixed ? fixed : 1 + rnd() % maxr;
        audio_buffer_t ib, ob;
        int ret;
        if (n > frames - pos) n = frames - pos;
        if (inplace) {
            memcpy(out + pos * ch, in + pos * ch, n * ch * sizeof(int16_t));
            ib.raw = ob.raw = out + pos * ch;
        } else {
            ib.raw = in + pos * ch;
            ob.raw = out + pos * ch;
        }
        ib.frameCount = ob.frameCount = n;
        ret = (*e)->process(e, &ib, &ob);
        CHECK(ret == 0, "process ret %d", ret);
        pos += n;
    }
    CHECK(cmd_int(e, EFFECT_CMD_DISABLE, 0, NULL) == 0, "disable");
    CHECK(g_lib->release_effect(e) == 0, "release");

    f = fopen(argv[9], "wb");
    fwrite(out, sizeof(int16_t), frames * ch, f);
    fclose(f);
    free(in);
    free(out);
    return g_failures ? 1 : 0;
}

static void selftest_commands(void) {
    effect_descriptor_t d;
    effect_handle_t e;
    effect_config_t cfg;
    uint32_t rsize;
    int16_t buf[4096];
    audio_buffer_t ib, ob;
    struct {
        effect_param_t p;
        int32_t param;
        int32_t value;
    } sp;
    int32_t reply_buf[16];
    effect_uuid_t bogus = kVoice;

    bogus.node[5] ^= 1;
    CHECK(g_lib->tag == AUDIO_EFFECT_LIBRARY_TAG, "tag");
    CHECK(g_lib->version == EFFECT_LIBRARY_API_VERSION, "version");
    CHECK(g_lib->get_descriptor(&kVoice, &d) == 0 && !memcmp(&d.type, &kTypeNs, 16), "voice desc");
    CHECK((d.flags & 7) == EFFECT_FLAG_TYPE_PRE_PROC, "pre-proc flag");
    CHECK(g_lib->get_descriptor(&kPure, &d) == 0 && !memcmp(&d.uuid, &kPure, 16), "pure desc");
    CHECK(g_lib->get_descriptor(&kCall, &d) == 0 && !memcmp(&d.uuid, &kCall, 16), "call desc");
    CHECK(g_lib->get_descriptor(&bogus, &d) == -EINVAL, "bogus desc");
    CHECK(g_lib->get_descriptor(NULL, &d) == -EINVAL, "null desc");
    {
        effect_handle_t x = NULL;
        CHECK(g_lib->create_effect(&bogus, 0, 0, &x) == -EINVAL && !x, "bogus create");
    }

    e = create(&kVoice);
    CHECK((*e)->get_descriptor(e, &d) == 0 && !memcmp(&d.uuid, &kVoice, 16), "itfe desc");

    /* process before configuration is refused without touching memory */
    ib.frameCount = ob.frameCount = 160;
    ib.raw = ob.raw = buf;
    CHECK((*e)->process(e, &ib, &ob) == -EINVAL, "process unconfigured");

    CHECK(configure(e, 48000, 1, 5 /* float */) == -EINVAL, "float rejected");
    CHECK(configure(e, 4000, 1, AUDIO_FORMAT_PCM_16_BIT) == -EINVAL, "4k rejected");
    CHECK(configure(e, 16000, 1, AUDIO_FORMAT_PCM_16_BIT) == 0, "16k mono");

    rsize = sizeof(cfg);
    CHECK((*e)->command(e, EFFECT_CMD_GET_CONFIG, 0, NULL, &rsize, &cfg) == 0 &&
              cfg.inputCfg.samplingRate == 16000 && rsize == sizeof(cfg),
          "get config");

    /* NS level parameter round trip */
    memset(&sp, 0, sizeof(sp));
    sp.p.psize = sizeof(int32_t);
    sp.p.vsize = sizeof(int32_t);
    sp.param = NS_PARAM_LEVEL;
    sp.value = NS_LEVEL_HIGH;
    CHECK(cmd_int(e, EFFECT_CMD_SET_PARAM, sizeof(sp), &sp) == 0, "set level");
    sp.value = 7;
    CHECK(cmd_int(e, EFFECT_CMD_SET_PARAM, sizeof(sp), &sp) == -EINVAL, "bad level");
    sp.value = 0;
    rsize = sizeof(reply_buf);
    CHECK((*e)->command(e, EFFECT_CMD_GET_PARAM, sizeof(effect_param_t) + 4, &sp, &rsize,
                        reply_buf) == 0,
          "get level");
    CHECK(((effect_param_t *)reply_buf)->status == 0 && reply_buf[4] == NS_LEVEL_HIGH, "level value");

    CHECK(cmd_int(e, EFFECT_CMD_ENABLE, 0, NULL) == 0, "enable");
    CHECK(cmd_int(e, EFFECT_CMD_RESET, 0, NULL) == 0, "reset");
    CHECK(cmd_int(e, EFFECT_CMD_SET_AUDIO_SOURCE, sizeof(int), &(int){1}) == 0, "source");
    CHECK(cmd_int(e, EFFECT_CMD_SET_INPUT_DEVICE, sizeof(int), &(int){4}) == 0, "device");
    CHECK(cmd_int(e, 0x10055, 0, NULL) == -EINVAL, "unknown cmd");
    CHECK((*e)->command(e, EFFECT_CMD_SET_CONFIG, 3, &cfg, &rsize, reply_buf) == -EINVAL, "short cfg");
    CHECK((*e)->process(e, NULL, &ob) == -EINVAL, "null in");
    ib.frameCount = 10;
    ob.frameCount = 11;
    CHECK((*e)->process(e, &ib, &ob) == -EINVAL, "mismatched counts");

    /* zero-length and huge slices */
    ib.frameCount = ob.frameCount = 0;
    CHECK((*e)->process(e, &ib, &ob) == 0, "zero frames");
    {
        size_t big = 48000 * 3;
        int16_t *b = calloc(big, sizeof(int16_t));
        ib.raw = ob.raw = b;
        ib.frameCount = ob.frameCount = big;
        CHECK((*e)->process(e, &ib, &ob) == 0, "3 s slice");
        free(b);
    }

    /* more than two channels pass through untouched */
    CHECK(configure(e, 48000, 4, AUDIO_FORMAT_PCM_16_BIT) == 0, "4ch config");
    {
        int16_t a[4 * 64], b[4 * 64];
        int i;
        for (i = 0; i < 4 * 64; i++) a[i] = (int16_t)(i * 37);
        ib.raw = a;
        ob.raw = b;
        ib.frameCount = ob.frameCount = 64;
        CHECK((*e)->process(e, &ib, &ob) == 0 && !memcmp(a, b, sizeof(a)), "4ch pass-through");
    }

    /* disabled effect copies input to output */
    CHECK(configure(e, 44100, 2, AUDIO_FORMAT_PCM_16_BIT) == 0, "44.1k stereo");
    CHECK(cmd_int(e, EFFECT_CMD_DISABLE, 0, NULL) == 0, "disable");
    {
        int16_t a[2 * 100], b[2 * 100];
        int i;
        for (i = 0; i < 200; i++) a[i] = (int16_t)(i * 101);
        ib.raw = a;
        ob.raw = b;
        ib.frameCount = ob.frameCount = 100;
        CHECK((*e)->process(e, &ib, &ob) == 0 && !memcmp(a, b, sizeof(a)), "disabled copy");
    }
    CHECK(g_lib->release_effect(e) == 0, "release");
}

/* Random rates, layouts, variants, slice sizes and command interleavings. */
static void selftest_stress(int rounds) {
    static const uint32_t rates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000,
                                     44100, 48000, 88200, 96000, 192000};
    int r;
    for (r = 0; r < rounds; r++) {
        uint32_t rate = rates[rnd() % (sizeof(rates) / sizeof(rates[0]))];
        int ch = 1 + (int)(rnd() % 3);
        uint32_t v = rnd() % 3;
        effect_handle_t e = create(v == 0 ? &kVoice : v == 1 ? &kPure : &kCall);
        size_t total = rate / 2 + rnd() % rate, pos = 0;
        int16_t *buf = malloc((total + 8) * ch * sizeof(int16_t));
        size_t i;
        for (i = 0; i < total * ch; i++) {
            uint32_t v = rnd();
            /* mix of silence, full-scale bursts and noise */
            if ((i / 4000) % 3 == 0) buf[i] = 0;
            else if ((i / 4000) % 3 == 1) buf[i] = (int16_t)(v & 1 ? 32767 : -32768);
            else buf[i] = (int16_t)(v >> 16);
        }
        CHECK(configure(e, rate, ch, AUDIO_FORMAT_PCM_16_BIT) == 0, "stress config %u x%d", rate, ch);
        CHECK(cmd_int(e, EFFECT_CMD_ENABLE, 0, NULL) == 0, "stress enable");
        while (pos < total) {
            size_t n = 1 + rnd() % 5000;
            audio_buffer_t ib, ob;
            if (n > total - pos) n = total - pos;
            ib.raw = ob.raw = buf + pos * ch;
            ib.frameCount = ob.frameCount = n;
            CHECK((*e)->process(e, &ib, &ob) == 0, "stress process");
            pos += n;
            switch (rnd() % 64) {
            case 0: cmd_int(e, EFFECT_CMD_RESET, 0, NULL); break;
            case 1: cmd_int(e, EFFECT_CMD_DISABLE, 0, NULL); break;
            case 2: cmd_int(e, EFFECT_CMD_ENABLE, 0, NULL); break;
            case 3: configure(e, rates[rnd() % 12], ch, AUDIO_FORMAT_PCM_16_BIT); break;
            default: break;
            }
        }
        g_lib->release_effect(e);
        free(buf);
    }
}

int main(int argc, char **argv) {
    void *h;
    if (argc < 3) {
        fprintf(stderr, "usage: %s LIB run|selftest ...\n", argv[0]);
        return 2;
    }
    h = dlopen(argv[1], RTLD_NOW);
    if (!h) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 2;
    }
    g_lib = (const audio_effect_library_t *)dlsym(h, "AELI");
    if (!g_lib) {
        fprintf(stderr, "no AELI\n");
        return 2;
    }
    if (!strcmp(argv[2], "run") && argc == 10) return run(argc, argv);
    if (!strcmp(argv[2], "selftest")) {
        int rounds = argc > 3 ? atoi(argv[3]) : 40;
        selftest_commands();
        selftest_stress(rounds);
        printf("selftest: %d failure(s)\n", g_failures);
        return g_failures ? 1 : 0;
    }
    fprintf(stderr, "bad arguments\n");
    return 2;
}
