/*
 * MicNS - AI noise suppression for the Android capture path.
 *
 * An Android audio effect library (effect API 3.0) of type "noise
 * suppression" that wraps the RNNoise recurrent neural network denoiser
 * (Xiph.Org). Audio HALs that run software pre-processing effects call
 * process() with 16-bit PCM at the stream rate; we resample to 48 kHz,
 * denoise in 10 ms blocks (RNNoise plus a Wiener post-filter on its
 * gains), optionally shape the voice (clarity EQ, voice gate, speech
 * leveler, deeper voice) and convert back.
 *
 * Three implementations share the code:
 *   micns       - high-pass + denoise + clarity EQ + gate + leveler + deep voice + limiter
 *   micns_call  - the same without the leveler, for internet calls
 *   micns_pure  - high-pass + denoise + limiter
 *
 * Runtime settings are read from micns.conf every time an effect is
 * created, i.e. at the start of every recording session.
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <android/log.h>

#include "android_effect_api.h"
#include "rnnoise/rnnoise.h"
#include "speexdsp/speex_resampler.h"

void rnnoise_global_init(void);
void rnnoise_reset(DenoiseState *st);
void rnnoise_set_postfilter(DenoiseState *st, int on, float floor_gain);
void rnnoise_get_pitch(const DenoiseState *st, int *period, float *gain);

#define MICNS_TAG "MicNS"
#define MICNS_VERSION "1.1"

#define MICNS_RATE 48000u
#define MICNS_BLOCK 480        /* one RNNoise frame: 10 ms at 48 kHz */
#define MICNS_CHUNK 1024       /* stream frames handled per pipeline pass */
#define MICNS_MAX_PROC_CH 2
#define MICNS_MIN_RATE 8000u
#define MICNS_MAX_RATE 192000u
#define MICNS_RESAMPLE_QUALITY 5
#define PS_LOOKAHEAD 1024  /* deep voice: input needed past a grain centre */
#define PS_LATENCY 1664    /* deep voice: fixed delay (35 ms), > LOOKAHEAD + MAX_PERIOD */
#define PS_HOP 240         /* deep voice: grain spacing for unvoiced sound */
#define PS_MIN_PERIOD 96   /* 500 Hz */
#define PS_MAX_PERIOD 600  /* 80 Hz */

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, MICNS_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, MICNS_TAG, __VA_ARGS__)

/* ------------------------------------------------------------------------- */
/* Descriptors                                                               */
/* ------------------------------------------------------------------------- */

/* voice: everything (recording, voice notes); pure: noise removal only
   (speech recognition); call: for internet calls, after the platform echo
   canceller, without the leveler since call apps run their own AGC. */
enum { VARIANT_VOICE = 0, VARIANT_PURE = 1, VARIANT_CALL = 2, VARIANT_COUNT };
static const char *const g_variant_names[VARIANT_COUNT] = {"voice", "pure", "call"};

/* FX_IID_NS: the generic noise suppression type, so audio HALs and the
   framework treat us exactly like the platform NoiseSuppressor. */
#define MICNS_TYPE_NS {0x58b4b260, 0x8e06, 0x11e0, 0xaa8e, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}

static const effect_descriptor_t g_descriptors[VARIANT_COUNT] = {
    {
        MICNS_TYPE_NS,
        {0x2470ae61, 0x735c, 0x41ba, 0x9565, {0x69, 0x23, 0xcc, 0x71, 0x3c, 0x54}},
        EFFECT_CONTROL_API_VERSION,
        EFFECT_FLAG_TYPE_PRE_PROC | EFFECT_FLAG_DEVICE_IND,
        400, /* ~40 MIPS */
        160, /* ~160 KB */
        "MicNS AI Noise Suppression (voice)",
        "MicNS / RNNoise (Xiph.Org)",
    },
    {
        MICNS_TYPE_NS,
        {0xc6b4d85a, 0x7071, 0x4520, 0x9121, {0x82, 0x8c, 0xff, 0x4f, 0x59, 0x6f}},
        EFFECT_CONTROL_API_VERSION,
        EFFECT_FLAG_TYPE_PRE_PROC | EFFECT_FLAG_DEVICE_IND,
        380,
        150,
        "MicNS AI Noise Suppression (pure)",
        "MicNS / RNNoise (Xiph.Org)",
    },
    {
        MICNS_TYPE_NS,
        {0xd7bbe82a, 0x9292, 0x411e, 0x9656, {0x52, 0xb9, 0x12, 0xec, 0xe4, 0x27}},
        EFFECT_CONTROL_API_VERSION,
        EFFECT_FLAG_TYPE_PRE_PROC | EFFECT_FLAG_DEVICE_IND,
        400,
        160,
        "MicNS AI Noise Suppression (call)",
        "MicNS / RNNoise (Xiph.Org)",
    },
};

/* ------------------------------------------------------------------------- */
/* Settings (micns.conf)                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    int enable;    /* 0 = pass audio through untouched */
    int strength;  /* 0..100, how far noise is pushed down */
    int hpf;       /* 80 Hz rumble filter */
    int clarity;   /* voice variant: low-mid cut + presence lift */
    int leveler;   /* voice variant: speech level normalisation */
    int gain_db;   /* voice variant: fixed make-up gain */
    int gate;      /* voice variant: duck the gaps between words */
    int gate_db;   /* voice variant: how far the gaps are ducked */
    int postfilter;     /* per-bin residual noise post-filter */
    int deep;           /* voice variant: deeper voice */
    float deep_pitch;   /* semitones, negative = lower */
    float deep_formant; /* percent, negative = bigger/darker vocal tract */
    float deep_bass;    /* dB of low shelf at 150 Hz */
} micns_settings;

static const char *const g_conf_paths[] = {
    "/vendor/etc/micns.conf",
    "/odm/etc/micns.conf",
    "/system/vendor/etc/micns.conf",
    "/system/etc/micns.conf",
};

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void settings_defaults(micns_settings *s) {
    s->enable = 1;
    s->strength = 90;
    s->hpf = 1;
    s->clarity = 1;
    s->leveler = 1;
    s->gain_db = 0;
    s->gate = 1;
    s->gate_db = 18;
    s->postfilter = 1;
    s->deep = 0;
    s->deep_pitch = -2.f;
    s->deep_formant = -4.f;
    s->deep_bass = 4.f;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void settings_apply(micns_settings *s, const char *key, float fvalue) {
    int value;
    fvalue = clampf(fvalue, -1e6f, 1e6f); /* keep the int conversion defined */
    value = (int)(fvalue >= 0.f ? fvalue + 0.5f : fvalue - 0.5f);
    if (!strcmp(key, "enable")) s->enable = value != 0;
    else if (!strcmp(key, "strength")) s->strength = clampi(value, 0, 100);
    else if (!strcmp(key, "hpf")) s->hpf = value != 0;
    else if (!strcmp(key, "clarity")) s->clarity = value != 0;
    else if (!strcmp(key, "leveler")) s->leveler = value != 0;
    else if (!strcmp(key, "gain_db")) s->gain_db = clampi(value, -12, 12);
    else if (!strcmp(key, "gate")) s->gate = value != 0;
    else if (!strcmp(key, "gate_db")) s->gate_db = clampi(value, 0, 40);
    else if (!strcmp(key, "postfilter")) s->postfilter = value != 0;
    else if (!strcmp(key, "deep")) s->deep = value != 0;
    else if (!strcmp(key, "deep_pitch")) s->deep_pitch = clampf(fvalue, -6.f, 6.f);
    else if (!strcmp(key, "deep_formant")) s->deep_formant = clampf(fvalue, -15.f, 15.f);
    else if (!strcmp(key, "deep_bass")) s->deep_bass = clampf(fvalue, 0.f, 12.f);
}

/* "-2.5" style numbers without pulling in strtod. */
static int parse_num(const char *p, float *out) {
    float sign = 1.f, v = 0.f, scale = 1.f;
    int digits = 0;
    if (*p == '-' || *p == '+') sign = *p++ == '-' ? -1.f : 1.f;
    for (; *p >= '0' && *p <= '9'; p++, digits++) v = v * 10.f + (float)(*p - '0');
    if (*p == '.')
        for (p++; *p >= '0' && *p <= '9'; p++, digits++) v += (float)(*p - '0') * (scale *= 0.1f);
    *out = sign * v;
    return digits > 0;
}

static char *trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static void settings_load(micns_settings *s) {
    size_t i;
    settings_defaults(s);
    for (i = 0; i < sizeof(g_conf_paths) / sizeof(g_conf_paths[0]); i++) {
        char line[128];
        FILE *f = fopen(g_conf_paths[i], "r");
        if (!f) continue;
        while (fgets(line, sizeof(line), f)) {
            char *key, *val, *eq;
            float v;
            key = trim(line);
            if (*key == '#' || *key == 0) continue;
            eq = strchr(key, '=');
            if (!eq) continue;
            *eq = 0;
            key = trim(key);
            val = trim(eq + 1);
            if (!parse_num(val, &v)) continue;
            settings_apply(s, key, v);
        }
        fclose(f);
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* Small DSP helpers                                                         */
/* ------------------------------------------------------------------------- */

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} biquad;

static void biquad_set(biquad *f, double b0, double b1, double b2, double a0, double a1,
                       double a2) {
    f->b0 = (float)(b0 / a0);
    f->b1 = (float)(b1 / a0);
    f->b2 = (float)(b2 / a0);
    f->a1 = (float)(a1 / a0);
    f->a2 = (float)(a2 / a0);
    f->z1 = f->z2 = 0.f;
}

/* RBJ audio EQ cookbook. */
static void biquad_highpass(biquad *f, double fs, double f0, double q) {
    double w = 2.0 * M_PI * f0 / fs, cw = cos(w), alpha = sin(w) / (2.0 * q);
    biquad_set(f, (1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
}

static void biquad_peaking(biquad *f, double fs, double f0, double q, double gain_db) {
    double a = pow(10.0, gain_db / 40.0);
    double w = 2.0 * M_PI * f0 / fs, cw = cos(w), alpha = sin(w) / (2.0 * q);
    biquad_set(f, 1 + alpha * a, -2 * cw, 1 - alpha * a, 1 + alpha / a, -2 * cw, 1 - alpha / a);
}

static void biquad_lowshelf(biquad *f, double fs, double f0, double gain_db) {
    double a = pow(10.0, gain_db / 40.0);
    double w = 2.0 * M_PI * f0 / fs, cw = cos(w);
    double sa = sin(w) * sqrt(a); /* shelf slope S = 1 */
    biquad_set(f, a * ((a + 1) - (a - 1) * cw + sa), 2 * a * ((a - 1) - (a + 1) * cw),
               a * ((a + 1) - (a - 1) * cw - sa), (a + 1) + (a - 1) * cw + sa,
               -2 * ((a - 1) + (a + 1) * cw), (a + 1) + (a - 1) * cw - sa);
}

static void biquad_run(biquad *f, float *x, int n) {
    float b0 = f->b0, b1 = f->b1, b2 = f->b2, a1 = f->a1, a2 = f->a2;
    float z1 = f->z1, z2 = f->z2;
    int i;
    for (i = 0; i < n; i++) {
        float in = x[i];
        float out = b0 * in + z1;
        z1 = b1 * in - a1 * out + z2;
        z2 = b2 * in - a2 * out;
        x[i] = out;
    }
    /* Keep the state out of the denormal range during digital silence. */
    if (fabsf(z1) < 1e-15f) z1 = 0.f;
    if (fabsf(z2) < 1e-15f) z2 = 0.f;
    f->z1 = z1;
    f->z2 = z2;
}

static float db_to_lin(float db) {
    return powf(10.f, db / 20.f);
}

static int16_t to_s16(float v) {
    if (!(v == v)) return 0; /* NaN */
    if (v >= 32767.f) return 32767;
    if (v <= -32768.f) return -32768;
    return (int16_t)(v >= 0.f ? v + 0.5f : v - 0.5f);
}

static uint32_t popcount32(uint32_t v) {
    uint32_t c = 0;
    while (v) {
        v &= v - 1;
        c++;
    }
    return c;
}

static uint32_t channel_count(uint32_t mask) {
    uint32_t n;
    if ((mask >> 30) == AUDIO_CHANNEL_REPRESENTATION_INDEX) return popcount32(mask & 0x3FFFFFFFu);
    n = popcount32(mask & AUDIO_CHANNEL_IN_ALL);
    return n ? n : popcount32(mask);
}

/* ------------------------------------------------------------------------- */
/* Effect context                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    DenoiseState *rnn;
    SpeexResamplerState *up;   /* stream rate -> 48 kHz, NULL at 48 kHz */
    SpeexResamplerState *down; /* 48 kHz -> stream rate */
    float *in48;               /* 48 kHz samples waiting for a full block */
    uint32_t in48_pos, in48_len, in48_cap;
    float *out;                /* stream-rate samples ready to be returned */
    uint32_t out_pos, out_len, out_cap;
    float dry[MICNS_BLOCK];    /* last high-passed block, aligned with RNNoise delay */
    float den[MICNS_BLOCK];
    biquad hpf, eq_low, eq_pres, bass;
    struct psola *ps;          /* deep voice state, NULL when off */
    int period;                /* pitch of the current block (48 kHz samples) */
    float pgain;               /* how periodic (voiced) it is, 0..1 */
    float pvad;                /* RNNoise speech probability of the block */
    int vperiod;               /* period deep voice used last block, 0 = unvoiced */
    int vheld;                 /* blocks vperiod was held without a pitch */
} micns_chan;

/* Deep voice state, see deep_block(). */
#define PS_RING 8192
#define PS_MASK (PS_RING - 1)
#define PS_BLOCKS 16

struct psola {
    float in[PS_RING];
    float acc[PS_RING];
    float wsum[PS_RING];
    int period[PS_BLOCKS];  /* per block, 0 = unvoiced */
    uint32_t cur;           /* ring position of the newest block */
    uint32_t nblk;
    double ts;              /* next synthesis mark, relative to cur */
    double ta;              /* last analysis mark, relative to cur */
};

typedef struct micns_ctx {
    const struct effect_interface_s *itfe; /* must stay first */
    int variant;
    micns_settings set;
    effect_config_t config;
    effect_config_t config_reverse;
    int configured;
    int enabled;
    int bypass;          /* configured for a layout we leave untouched */
    uint32_t rate;
    uint32_t nch;        /* channels in the stream */
    uint32_t nproc;      /* channels we denoise (nch when <= 2) */
    uint32_t prime;      /* stream frames of latency primed into the output FIFO */
    int accumulate;
    float mix_dry;       /* share of the high-passed original kept in the output */
    float fixed_gain;
    float level_db;      /* leveler: running speech level estimate */
    float gain_db;       /* leveler: currently applied gain */
    float gain_lin;
    float lim_env;
    float lim_release;
    float gate_floor;    /* gate: gain applied while nobody talks */
    float gate_gain;
    int gate_hold;       /* gate: blocks left before closing */
    int talk_run;        /* gate: consecutive talking blocks */
    float vad_prev;
    int deep_on;
    double deep_alpha;   /* pitch ratio, < 1 lowers */
    float deep_beta;     /* formant ratio, < 1 lowers */
    micns_chan ch[MICNS_MAX_PROC_CH];
    float *tmp_in[MICNS_MAX_PROC_CH];
    float *tmp_out[MICNS_MAX_PROC_CH];
    float *tmp48;        /* one block resampled, worst case */
    uint32_t tmp48_cap;
} micns_ctx;

static const struct effect_interface_s g_interface;

static void chan_free(micns_chan *c) {
    if (c->rnn) rnnoise_destroy(c->rnn);
    if (c->up) speex_resampler_destroy(c->up);
    if (c->down) speex_resampler_destroy(c->down);
    free(c->in48);
    free(c->out);
    free(c->ps);
    memset(c, 0, sizeof(*c));
}

static void ctx_free_dsp(micns_ctx *x) {
    uint32_t i;
    for (i = 0; i < MICNS_MAX_PROC_CH; i++) {
        chan_free(&x->ch[i]);
        free(x->tmp_in[i]);
        free(x->tmp_out[i]);
        x->tmp_in[i] = x->tmp_out[i] = NULL;
    }
    free(x->tmp48);
    x->tmp48 = NULL;
    x->tmp48_cap = 0;
    x->nproc = 0;
}

static void ctx_reset_dsp(micns_ctx *x) {
    uint32_t i;
    double fs = MICNS_RATE;
    for (i = 0; i < x->nproc; i++) {
        micns_chan *c = &x->ch[i];
        rnnoise_reset(c->rnn);
        if (c->up) speex_resampler_reset_mem(c->up);
        if (c->down) speex_resampler_reset_mem(c->down);
        c->in48_pos = c->in48_len = 0;
        c->out_pos = 0;
        c->out_len = x->prime;
        memset(c->out, 0, x->prime * sizeof(float));
        memset(c->dry, 0, sizeof(c->dry));
        biquad_highpass(&c->hpf, fs, 80.0, 0.7071);
        biquad_peaking(&c->eq_low, fs, 250.0, 1.0, -1.5);
        biquad_peaking(&c->eq_pres, fs, 3200.0, 0.9, 2.5);
        biquad_lowshelf(&c->bass, fs, 150.0, x->set.deep_bass);
        rnnoise_set_postfilter(c->rnn, x->set.postfilter, 0.3f);
        c->period = c->vperiod = c->vheld = 0;
        c->pgain = c->pvad = 0.f;
        if (c->ps) {
            memset(c->ps, 0, sizeof(*c->ps));
            c->ps->ts = c->ps->ta = -(double)PS_LATENCY;
        }
    }
    x->level_db = -21.f;
    x->gain_db = 0.f;
    x->gain_lin = 1.f;
    x->lim_env = 0.f;
    x->gate_gain = 1.f;
    x->gate_hold = 0;
    x->talk_run = 0;
    x->vad_prev = 0.f;
}

static int ctx_setup_dsp(micns_ctx *x) {
    uint32_t i;
    /* 32-bit math is enough here (rate <= 192 kHz) and avoids 64-bit
       division helpers on 32-bit ARM. */
    uint32_t chunk48 = (MICNS_CHUNK * MICNS_RATE + x->rate - 1) / x->rate;
    uint32_t block_out = (MICNS_BLOCK * x->rate + MICNS_RATE - 1) / MICNS_RATE;

    ctx_free_dsp(x);
    x->nproc = x->nch;
    /* One block of latency plus resampler slack keeps the output FIFO from
       running dry no matter how the HAL slices its buffers. */
    x->prime = block_out + 2 + (x->rate + 999) / 1000;
    x->tmp48_cap = block_out * 2 + 64 > MICNS_BLOCK * 2 ? block_out * 2 + 64 : MICNS_BLOCK * 2;

    x->tmp48 = calloc(x->tmp48_cap, sizeof(float));
    if (!x->tmp48) return -ENOMEM;
    for (i = 0; i < x->nproc; i++) {
        micns_chan *c = &x->ch[i];
        int err = 0;
        x->tmp_in[i] = calloc(MICNS_CHUNK, sizeof(float));
        x->tmp_out[i] = calloc(MICNS_CHUNK, sizeof(float));
        c->in48_cap = chunk48 + MICNS_BLOCK + 64;
        c->in48 = calloc(c->in48_cap, sizeof(float));
        c->out_cap = x->prime + MICNS_CHUNK + 4 * block_out + 256;
        c->out = calloc(c->out_cap, sizeof(float));
        if (!x->tmp_in[i] || !x->tmp_out[i] || !c->in48 || !c->out) return -ENOMEM;
        c->rnn = rnnoise_create(NULL);
        if (!c->rnn) return -ENOMEM;
        if (x->deep_on) {
            c->ps = calloc(1, sizeof(*c->ps));
            if (!c->ps) return -ENOMEM;
        }
        if (x->rate != MICNS_RATE) {
            c->up = speex_resampler_init(1, x->rate, MICNS_RATE, MICNS_RESAMPLE_QUALITY, &err);
            if (!c->up || err) return -ENOMEM;
            c->down = speex_resampler_init(1, MICNS_RATE, x->rate, MICNS_RESAMPLE_QUALITY, &err);
            if (!c->down || err) return -ENOMEM;
        }
    }
    ctx_reset_dsp(x);
    return 0;
}

static void ctx_apply_settings(micns_ctx *x) {
    int s = x->set.strength;
    /* strength 100 removes everything RNNoise flags as noise; lower values
       keep a little of the original so heavy noise never sounds "underwater". */
    x->mix_dry = s >= 100 ? 0.f : db_to_lin(-0.45f * (float)s);
    x->fixed_gain = x->variant == VARIANT_VOICE ? db_to_lin((float)x->set.gain_db) : 1.f;
    /* ~60 ms limiter release at 48 kHz */
    x->lim_release = expf(-1.f / (0.060f * (float)MICNS_RATE));
    x->gate_floor = x->variant != VARIANT_PURE && x->set.gate ? db_to_lin(-(float)x->set.gate_db) : 1.f;
    x->deep_on = x->variant != VARIANT_PURE && x->set.deep;
    x->deep_alpha = exp(x->set.deep_pitch / 12.0 * 0.69314718055994531); /* 2^(st/12) */
    x->deep_beta = 1.f + x->set.deep_formant / 100.f;
}

static int ctx_configure(micns_ctx *x, const effect_config_t *cfg) {
    const buffer_config_t *in = &cfg->inputCfg, *out = &cfg->outputCfg;
    uint32_t rate = in->samplingRate;
    uint32_t nch = channel_count(in->channels);
    int ret;

    if (in->samplingRate != out->samplingRate) return -EINVAL;
    if (in->format != AUDIO_FORMAT_PCM_16_BIT || out->format != AUDIO_FORMAT_PCM_16_BIT)
        return -EINVAL; /* the framework retries with 16-bit PCM */
    if (rate < MICNS_MIN_RATE || rate > MICNS_MAX_RATE) return -EINVAL;
    if (nch == 0 || nch > 8 || nch != channel_count(out->channels)) return -EINVAL;

    x->config = *cfg;
    x->accumulate = out->accessMode == EFFECT_BUFFER_ACCESS_ACCUMULATE;
    x->rate = rate;
    x->nch = nch;
    x->configured = 0;

    if (nch > MICNS_MAX_PROC_CH) {
        /* Multi-mic raw layouts: leave them alone rather than smear phase. */
        ctx_free_dsp(x);
        x->bypass = 1;
        x->configured = 1;
        LOGI("configured %u Hz x%u: pass-through", rate, nch);
        return 0;
    }
    x->bypass = 0;
    ret = ctx_setup_dsp(x);
    if (ret) {
        ctx_free_dsp(x);
        LOGW("configure %u Hz x%u failed: %d", rate, nch, ret);
        return ret;
    }
    x->configured = 1;
    LOGI("v%s %s: %u Hz x%u, strength %d, clarity %d, leveler %d, enable %d", MICNS_VERSION,
         g_variant_names[x->variant], rate, nch, x->set.strength,
         x->set.clarity, x->set.leveler, x->set.enable);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Processing                                                                */
/* ------------------------------------------------------------------------- */

static void fifo_compact(float *buf, uint32_t *pos, uint32_t len) {
    if (*pos) {
        if (len) memmove(buf, buf + *pos, len * sizeof(float));
        *pos = 0;
    }
}

/* Stream-rate samples into the 48 kHz block FIFO. */
static void push_input(micns_ctx *x, micns_chan *c, const float *in, uint32_t n) {
    fifo_compact(c->in48, &c->in48_pos, c->in48_len);
    if (!c->up) {
        uint32_t room = c->in48_cap - c->in48_len;
        if (n > room) n = room;
        memcpy(c->in48 + c->in48_len, in, n * sizeof(float));
        c->in48_len += n;
        return;
    }
    while (n) {
        uint32_t in_len = n, out_len = c->in48_cap - c->in48_len;
        if (!out_len) break;
        speex_resampler_process_float(c->up, 0, in, &in_len, c->in48 + c->in48_len, &out_len);
        c->in48_len += out_len;
        if (!in_len && !out_len) break;
        in += in_len;
        n -= in_len;
    }
    (void)x;
}

/* One processed 48 kHz block into the stream-rate output FIFO. */
static void push_output(micns_ctx *x, micns_chan *c, const float *blk) {
    fifo_compact(c->out, &c->out_pos, c->out_len);
    if (!c->down) {
        uint32_t n = MICNS_BLOCK, room = c->out_cap - c->out_len;
        if (n > room) n = room;
        memcpy(c->out + c->out_len, blk, n * sizeof(float));
        c->out_len += n;
        return;
    } else {
        uint32_t left = MICNS_BLOCK;
        const float *src = blk;
        while (left) {
            uint32_t in_len = left, out_len = x->tmp48_cap;
            uint32_t room = c->out_cap - c->out_len;
            speex_resampler_process_float(c->down, 0, src, &in_len, x->tmp48, &out_len);
            if (out_len > room) out_len = room;
            memcpy(c->out + c->out_len, x->tmp48, out_len * sizeof(float));
            c->out_len += out_len;
            if (!in_len) break;
            src += in_len;
            left -= in_len;
        }
    }
}

static uint32_t pop_output(micns_chan *c, float *dst, uint32_t n) {
    uint32_t got = c->out_len < n ? c->out_len : n;
    uint32_t i;
    if (got < n) {
        /* Underrun: emit silence first so the newest audio stays aligned. */
        for (i = 0; i < n - got; i++) dst[i] = 0.f;
        dst += n - got;
    }
    memcpy(dst, c->out + c->out_pos, got * sizeof(float));
    c->out_pos += got;
    c->out_len -= got;
    return got;
}

static float denoise_block(micns_ctx *x, micns_chan *c, const float *in, float *blk) {
    float vad;
    float wet = 1.f - x->mix_dry;
    int i;

    memcpy(blk, in, MICNS_BLOCK * sizeof(float));
    if (x->set.hpf) biquad_run(&c->hpf, blk, MICNS_BLOCK);
    /* Inaudible (1e-6 LSB) dither keeps RNNoise's recursive filters out of
       denormals while the HAL delivers digital silence. */
    for (i = 0; i < MICNS_BLOCK; i += 2) {
        blk[i] += 1e-6f;
        blk[i + 1] -= 1e-6f;
    }

    vad = rnnoise_process_frame(c->rnn, c->den, blk);
    rnnoise_get_pitch(c->rnn, &c->period, &c->pgain);
    c->pvad = vad;

    /* RNNoise output lags its input by exactly one block: mix it with the
       previous high-passed block, then remember the current one. */
    for (i = 0; i < MICNS_BLOCK; i++) {
        float d = c->den[i];
        if (!(d == d) || d > 1e6f || d < -1e6f) d = 0.f; /* never let NaN/inf out */
        c->den[i] = wet * d + x->mix_dry * c->dry[i];
    }
    memcpy(c->dry, blk, MICNS_BLOCK * sizeof(float));
    memcpy(blk, c->den, MICNS_BLOCK * sizeof(float));

    if (x->variant != VARIANT_PURE && x->set.clarity) {
        biquad_run(&c->eq_low, blk, MICNS_BLOCK);
        biquad_run(&c->eq_pres, blk, MICNS_BLOCK);
    }
    return vad;
}

/*
 * Voice gate. RNNoise reports voice activity for the block it just analysed,
 * one block ahead of the audio it returns. Talking means that block and the
 * one being returned are both active, so a lone keyboard click or bump (one
 * block) stays ducked while a word opens the gate at its first block. The
 * hold after talking grows with how long the talking lasted: real speech
 * keeps the gate open 150 ms past the last word, while a short false trigger
 * from noise closes again after a few blocks. Then the gap is ducked by
 * gate_db.
 */
static int is_talking(const micns_ctx *x, float vad_next) {
    return vad_next > 0.6f && x->vad_prev > 0.6f;
}

static float gate_block(micns_ctx *x, int talking) {
    float target;
    if (x->gate_floor >= 1.f) return 1.f;
    if (talking) {
        int hold;
        x->talk_run++;
        hold = 2 + 2 * x->talk_run;
        if (hold > 15) hold = 15;
        if (hold > x->gate_hold) x->gate_hold = hold;
    } else {
        x->talk_run = 0;
    }
    if (talking || x->gate_hold > 0) {
        if (!talking) x->gate_hold--;
        target = 1.f;
    } else {
        target = x->gate_floor;
    }
    if (target > x->gate_gain) x->gate_gain = target; /* open at once */
    else x->gate_gain += 0.15f * (target - x->gate_gain); /* close over ~65 ms */
    return x->gate_gain;
}

/* Linked-channel speech leveler and gate on one 48 kHz block.
   vad is the activity of the block RNNoise just analysed (see is_talking). */
static void dynamics_block(micns_ctx *x, float *blk[], uint32_t nproc, float vad) {
    const int fast = 96; /* 2 ms ramp when the gain has to go up */
    int talking = is_talking(x, vad);
    float g0 = x->gain_lin, g1;
    float gate = gate_block(x, talking);
    uint32_t c;
    int i, ramp;

    x->vad_prev = vad;

    if (x->variant == VARIANT_VOICE && x->set.leveler) {
        const float target = -21.f, max_gain = 12.f, min_gain = -6.f;
        double acc = 0;
        float rms_db, want;
        for (c = 0; c < nproc; c++)
            for (i = 0; i < MICNS_BLOCK; i++) acc += (double)blk[c][i] * blk[c][i];
        acc /= (double)MICNS_BLOCK * nproc;
        rms_db = 10.f * log10f((float)acc / (32768.f * 32768.f) + 1e-12f);
        /* Only learn the level while someone is talking. */
        if (talking && rms_db > -55.f) {
            float a = rms_db > x->level_db ? 0.10f : 0.02f;
            x->level_db += a * (rms_db - x->level_db);
        }
        want = target - x->level_db;
        if (want > max_gain) want = max_gain;
        if (want < min_gain) want = min_gain;
        x->gain_db += (want < x->gain_db ? 0.20f : 0.05f) * (want - x->gain_db);
        g1 = db_to_lin(x->gain_db) * x->fixed_gain;
    } else {
        g1 = x->fixed_gain;
    }
    if (x->gate_floor < 1.f) {
        /* The gate's floor is absolute: make-up gain only rides on speech,
           it must not lift the ducked gaps back up. */
        float open = (gate - x->gate_floor) / (1.f - x->gate_floor);
        float closed = g1 < x->gate_floor ? g1 : x->gate_floor;
        g1 = closed + (g1 - closed) * open;
    }

    /* Rising gain (gate opening at a word onset) must be quick so the first
       consonant is not swallowed; falling gain glides over the whole block. */
    ramp = g1 > g0 * 1.25f ? fast : MICNS_BLOCK;
    for (i = 0; i < MICNS_BLOCK; i++) {
        float g = i < ramp ? g0 + (g1 - g0) * (float)(i + 1) / (float)ramp : g1;
        for (c = 0; c < nproc; c++) blk[c][i] *= g;
    }
    x->gain_lin = g1;
}

/* Linked-channel peak limiter, the last stage before conversion to 16 bit. */
static void limit_block(micns_ctx *x, float *blk[], uint32_t nproc) {
    const float thr = 0.94f * 32767.f;
    uint32_t c;
    int i;
    for (i = 0; i < MICNS_BLOCK; i++) {
        float peak = 0.f, lg;
        for (c = 0; c < nproc; c++) {
            float a = fabsf(blk[c][i]);
            if (a > peak) peak = a;
        }
        x->lim_env = peak > x->lim_env ? peak : x->lim_env * x->lim_release + peak * (1.f - x->lim_release);
        lg = x->lim_env > thr ? thr / x->lim_env : 1.f;
        if (lg < 1.f)
            for (c = 0; c < nproc; c++) blk[c][i] *= lg;
    }
    if (x->lim_env < 1e-6f) x->lim_env = 0.f;
}

/* ------------------------------------------------------------------------- */
/* Deep voice: pitch-synchronous overlap-add (PSOLA)                         */
/* ------------------------------------------------------------------------- */

/*
 * Lowers the pitch of voiced speech while keeping the formants (the shape of
 * the vocal tract) in place, which is what makes a lower voice sound like a
 * person rather than a slowed-down recording. RNNoise already tracks the
 * pitch period of every block: grains two periods long are cut at
 * period-spaced analysis marks and laid down at wider spacing. Reading the
 * grains slightly slower than they are written also moves the formants down
 * a few percent (a "bigger" voice). Unvoiced sound goes through plain
 * overlap-add unchanged. The output is exactly PS_LATENCY samples late.
 */
static int psola_period(const struct psola *ps, double t) {
    int off = (int)floor(t / MICNS_BLOCK);
    if (off > 0) off = 0;
    if (off < -(PS_BLOCKS - 1)) off = -(PS_BLOCKS - 1);
    return ps->period[(ps->nblk + (uint32_t)off) & (PS_BLOCKS - 1)];
}

/* Hann-windowed grain read around `src` (stepping by beta) and added around
   output position `center`; window sums are kept for normalisation. */
static void psola_grain(struct psola *ps, int center, double src, int half, float beta) {
    double step = M_PI / (double)(half + 1);
    double cr = cos(-half * step), ci = sin(-half * step);
    double sr = cos(step), si = sin(step);
    int k;
    for (k = -half; k <= half; k++) {
        double sp = src + k * (double)beta;
        double fl = floor(sp);
        int i0 = (int)fl;
        float fr = (float)(sp - fl);
        float a = ps->in[(ps->cur + (uint32_t)i0) & PS_MASK];
        float b = ps->in[(ps->cur + (uint32_t)(i0 + 1)) & PS_MASK];
        float w = 0.5f * (1.f + (float)cr);
        uint32_t o = (ps->cur + (uint32_t)(center + k)) & PS_MASK;
        double t;
        ps->acc[o] += w * (a + fr * (b - a));
        ps->wsum[o] += w;
        t = cr * sr - ci * si; /* rotate to the next window angle */
        ci = cr * si + ci * sr;
        cr = t;
    }
}

/* Voicing with hysteresis: a block that drops out of a vowel for a moment
   would otherwise snap back to the original pitch and make it warble. */
static int deep_voicing(micns_chan *c) {
    int p = c->period, prev = c->vperiod;
    int in_range = p >= PS_MIN_PERIOD && p <= PS_MAX_PERIOD;
    int cur = 0;
    if (in_range && c->pgain > 0.45f) {
        cur = p; /* clearly voiced */
    } else if (prev && in_range && c->pgain > 0.2f && (p > prev ? p - prev : prev - p) * 5 < prev) {
        cur = p; /* weaker, but continues the same pitch */
    } else if (prev && c->vheld < 2 && c->pvad > 0.5f) {
        c->vheld++; /* no usable estimate while still talking: hold 20 ms */
        return c->vperiod;
    }
    c->vheld = 0;
    c->vperiod = cur;
    return cur;
}

static void deep_block(micns_ctx *x, micns_chan *c, float *blk) {
    struct psola *ps = c->ps;
    int i;

    if (ps && (x->deep_alpha != 1.0 || x->deep_beta != 1.f)) {
        ps->cur += MICNS_BLOCK;
        ps->nblk++;
        ps->ts -= MICNS_BLOCK;
        ps->ta -= MICNS_BLOCK;
        for (i = 0; i < MICNS_BLOCK; i++) ps->in[(ps->cur + (uint32_t)i) & PS_MASK] = blk[i];
        ps->period[ps->nblk & (PS_BLOCKS - 1)] = deep_voicing(c);

        /* Place every grain whose source is fully available. */
        while (ps->ts <= MICNS_BLOCK - PS_LOOKAHEAD) {
            int per = psola_period(ps, ps->ts);
            double pa = per ? per : PS_HOP;
            if (per) {
                /* nearest analysis mark on the pitch-period grid */
                double m = floor((ps->ts - ps->ta) / pa + 0.5);
                ps->ta += (m > 0 ? m : 0) * pa;
            } else {
                ps->ta = ps->ts;
            }
            psola_grain(ps, (int)floor(ps->ts + 0.5), ps->ta, (int)pa, x->deep_beta);
            ps->ts += per ? pa / x->deep_alpha : PS_HOP;
        }
        /* Everything PS_LATENCY behind the newest block is complete. */
        for (i = 0; i < MICNS_BLOCK; i++) {
            uint32_t o = (ps->cur - PS_LATENCY + (uint32_t)i) & PS_MASK;
            float w = ps->wsum[o];
            blk[i] = ps->acc[o] / (w > 0.3f ? w : 0.3f);
            ps->acc[o] = ps->wsum[o] = 0.f;
        }
    }
    biquad_run(&c->bass, blk, MICNS_BLOCK);
}

static void run_blocks(micns_ctx *x) {
    float *blk[MICNS_MAX_PROC_CH];
    float blkbuf[MICNS_MAX_PROC_CH][MICNS_BLOCK];
    uint32_t c;

    for (c = 0; c < x->nproc; c++) blk[c] = blkbuf[c];
    while (x->ch[0].in48_len >= MICNS_BLOCK) {
        float vad = 0.f;
        for (c = 0; c < x->nproc; c++) {
            micns_chan *ch = &x->ch[c];
            float v;
            if (ch->in48_len < MICNS_BLOCK) {
                memset(blk[c], 0, sizeof(blkbuf[c]));
                continue;
            }
            v = denoise_block(x, ch, ch->in48 + ch->in48_pos, blk[c]);
            ch->in48_pos += MICNS_BLOCK;
            ch->in48_len -= MICNS_BLOCK;
            if (v > vad) vad = v;
        }
        dynamics_block(x, blk, x->nproc, vad);
        if (x->deep_on)
            for (c = 0; c < x->nproc; c++) deep_block(x, &x->ch[c], blk[c]);
        limit_block(x, blk, x->nproc);
        for (c = 0; c < x->nproc; c++) push_output(x, &x->ch[c], blk[c]);
    }
}

static void copy_through(const audio_buffer_t *in, audio_buffer_t *out, uint32_t nch, int accumulate) {
    size_t n = in->frameCount * nch, i;
    if (accumulate) {
        for (i = 0; i < n; i++) out->s16[i] = to_s16((float)out->s16[i] + (float)in->s16[i]);
    } else if (in->raw != out->raw) {
        memmove(out->raw, in->raw, n * sizeof(int16_t));
    }
}

static int32_t micns_process(effect_handle_t self, audio_buffer_t *in, audio_buffer_t *out) {
    micns_ctx *x = (micns_ctx *)self;
    size_t frames, off;
    uint32_t nch, c;

    if (!x || !in || !out || !in->raw || !out->raw) return -EINVAL;
    if (in->frameCount != out->frameCount) return -EINVAL;
    frames = in->frameCount;

    if (!x->configured) return -EINVAL;
    nch = x->nch;
    if (!x->enabled || x->bypass || !x->set.enable) {
        copy_through(in, out, nch, x->accumulate);
        return 0;
    }

    for (off = 0; off < frames; off += MICNS_CHUNK) {
        uint32_t n = (uint32_t)(frames - off < MICNS_CHUNK ? frames - off : MICNS_CHUNK);
        const int16_t *src = in->s16 + off * nch;
        int16_t *dst = out->s16 + off * nch;
        uint32_t i;

        /* Read the whole chunk before writing it: in and out may alias. */
        for (c = 0; c < x->nproc; c++) {
            float *t = x->tmp_in[c];
            for (i = 0; i < n; i++) t[i] = (float)src[i * nch + c];
        }
        for (c = 0; c < x->nproc; c++) push_input(x, &x->ch[c], x->tmp_in[c], n);
        run_blocks(x);
        for (c = 0; c < x->nproc; c++) pop_output(&x->ch[c], x->tmp_out[c], n);

        for (c = 0; c < x->nproc; c++) {
            const float *t = x->tmp_out[c];
            if (x->accumulate) {
                for (i = 0; i < n; i++)
                    dst[i * nch + c] = to_s16((float)dst[i * nch + c] + t[i]);
            } else {
                for (i = 0; i < n; i++) dst[i * nch + c] = to_s16(t[i]);
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Commands                                                                  */
/* ------------------------------------------------------------------------- */

static int reply_int(uint32_t *replySize, void *pReplyData, int value) {
    if (!pReplyData || !replySize || *replySize < sizeof(int)) return -EINVAL;
    *(int *)pReplyData = value;
    *replySize = sizeof(int);
    return 0;
}

static int level_from_strength(int strength) {
    if (strength >= 95) return NS_LEVEL_HIGH;
    if (strength >= 70) return NS_LEVEL_MEDIUM;
    return NS_LEVEL_LOW;
}

static int32_t micns_command(effect_handle_t self, uint32_t cmdCode, uint32_t cmdSize,
                             void *pCmdData, uint32_t *replySize, void *pReplyData) {
    micns_ctx *x = (micns_ctx *)self;
    if (!x) return -EINVAL;

    switch (cmdCode) {
    case EFFECT_CMD_INIT:
        if (pReplyData) return reply_int(replySize, pReplyData, 0);
        return 0;

    case EFFECT_CMD_SET_CONFIG:
        if (!pCmdData || cmdSize < sizeof(effect_config_t)) return -EINVAL;
        return reply_int(replySize, pReplyData, ctx_configure(x, (const effect_config_t *)pCmdData));

    case EFFECT_CMD_GET_CONFIG:
        if (!pReplyData || !replySize || *replySize < sizeof(effect_config_t)) return -EINVAL;
        memcpy(pReplyData, &x->config, sizeof(effect_config_t));
        *replySize = sizeof(effect_config_t);
        return 0;

    case EFFECT_CMD_SET_CONFIG_REVERSE:
        if (!pCmdData || cmdSize < sizeof(effect_config_t)) return -EINVAL;
        memcpy(&x->config_reverse, pCmdData, sizeof(effect_config_t));
        if (pReplyData) return reply_int(replySize, pReplyData, 0);
        return 0;

    case EFFECT_CMD_GET_CONFIG_REVERSE:
        if (!pReplyData || !replySize || *replySize < sizeof(effect_config_t)) return -EINVAL;
        memcpy(pReplyData, &x->config_reverse, sizeof(effect_config_t));
        *replySize = sizeof(effect_config_t);
        return 0;

    case EFFECT_CMD_RESET:
        if (x->configured && !x->bypass) ctx_reset_dsp(x);
        if (pReplyData && replySize && *replySize >= sizeof(int)) return reply_int(replySize, pReplyData, 0);
        return 0;

    case EFFECT_CMD_ENABLE:
    case EFFECT_CMD_DISABLE: {
        int on = cmdCode == EFFECT_CMD_ENABLE;
        if (on && !x->enabled && x->configured && !x->bypass) ctx_reset_dsp(x);
        x->enabled = on;
        return reply_int(replySize, pReplyData, 0);
    }

    case EFFECT_CMD_SET_PARAM: {
        effect_param_t *p = (effect_param_t *)pCmdData;
        int status = -EINVAL;
        if (!p || cmdSize < sizeof(effect_param_t) + 2 * sizeof(int32_t)) return -EINVAL;
        if (p->psize == sizeof(int32_t) && p->vsize >= sizeof(int32_t)) {
            int32_t param, value;
            memcpy(&param, p->data, sizeof(param));
            memcpy(&value, p->data + sizeof(int32_t), sizeof(value));
            if (param == NS_PARAM_LEVEL && value >= NS_LEVEL_LOW && value <= NS_LEVEL_HIGH) {
                static const int strength[] = {60, 80, 100};
                x->set.strength = strength[value];
                ctx_apply_settings(x);
                status = 0;
            }
        }
        return reply_int(replySize, pReplyData, status);
    }

    case EFFECT_CMD_GET_PARAM: {
        effect_param_t *p = (effect_param_t *)pCmdData;
        effect_param_t *r = (effect_param_t *)pReplyData;
        int32_t param;
        uint32_t need = sizeof(effect_param_t) + 2 * sizeof(int32_t);
        if (!p || cmdSize < sizeof(effect_param_t) + sizeof(int32_t) || !r || !replySize ||
            *replySize < need || p->psize != sizeof(int32_t))
            return -EINVAL;
        memcpy(&param, p->data, sizeof(param));
        memmove(r, p, sizeof(effect_param_t) + sizeof(int32_t));
        r->psize = sizeof(int32_t);
        if (param == NS_PARAM_LEVEL) {
            int32_t value = level_from_strength(x->set.strength);
            memcpy(r->data + sizeof(int32_t), &value, sizeof(value));
            r->vsize = sizeof(int32_t);
            r->status = 0;
        } else {
            r->vsize = 0;
            r->status = -EINVAL;
        }
        *replySize = sizeof(effect_param_t) + sizeof(int32_t) + r->vsize;
        return 0;
    }

    case EFFECT_CMD_SET_DEVICE:
    case EFFECT_CMD_SET_INPUT_DEVICE:
    case EFFECT_CMD_SET_AUDIO_MODE:
    case EFFECT_CMD_SET_AUDIO_SOURCE:
    case EFFECT_CMD_SET_VOLUME:
    case EFFECT_CMD_DUMP:
        if (pReplyData && replySize && *replySize == sizeof(int)) return reply_int(replySize, pReplyData, 0);
        return 0;

    default:
        return -EINVAL;
    }
}

static int32_t micns_get_descriptor(effect_handle_t self, effect_descriptor_t *desc) {
    micns_ctx *x = (micns_ctx *)self;
    if (!x || !desc) return -EINVAL;
    *desc = g_descriptors[x->variant];
    return 0;
}

static const struct effect_interface_s g_interface = {
    micns_process,
    micns_command,
    micns_get_descriptor,
    NULL,
};

/* ------------------------------------------------------------------------- */
/* Library entry points                                                      */
/* ------------------------------------------------------------------------- */

static int g_init_lock;
static int g_init_done;

static void global_init(void) {
    if (__atomic_load_n(&g_init_done, __ATOMIC_ACQUIRE)) return;
    while (__atomic_exchange_n(&g_init_lock, 1, __ATOMIC_ACQUIRE)) {
    }
    if (!g_init_done) {
        rnnoise_global_init();
        __atomic_store_n(&g_init_done, 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_init_lock, 0, __ATOMIC_RELEASE);
}

static int variant_for_uuid(const effect_uuid_t *uuid) {
    int i;
    if (!uuid) return -1;
    for (i = 0; i < VARIANT_COUNT; i++)
        if (!memcmp(uuid, &g_descriptors[i].uuid, sizeof(*uuid))) return i;
    return -1;
}

static int32_t lib_create(const effect_uuid_t *uuid, int32_t sessionId, int32_t ioId,
                          effect_handle_t *pHandle) {
    int variant = variant_for_uuid(uuid);
    micns_ctx *x;
    (void)sessionId;
    (void)ioId;
    if (variant < 0 || !pHandle) return -EINVAL;
    global_init();
    x = calloc(1, sizeof(*x));
    if (!x) return -ENOMEM;
    x->itfe = &g_interface;
    x->variant = variant;
    settings_load(&x->set);
    ctx_apply_settings(x);
    *pHandle = (effect_handle_t)x;
    return 0;
}

static int32_t lib_release(effect_handle_t handle) {
    micns_ctx *x = (micns_ctx *)handle;
    if (!x) return -EINVAL;
    ctx_free_dsp(x);
    x->itfe = NULL;
    free(x);
    return 0;
}

static int32_t lib_get_descriptor(const effect_uuid_t *uuid, effect_descriptor_t *desc) {
    int variant = variant_for_uuid(uuid);
    if (variant < 0 || !desc) return -EINVAL;
    *desc = g_descriptors[variant];
    return 0;
}

__attribute__((visibility("default")))
const audio_effect_library_t AELI = {
    AUDIO_EFFECT_LIBRARY_TAG,
    EFFECT_LIBRARY_API_VERSION,
    "MicNS AI Noise Suppression Library",
    "MicNS / RNNoise (Xiph.Org)",
    lib_create,
    lib_release,
    lib_get_descriptor,
    NULL,
};
