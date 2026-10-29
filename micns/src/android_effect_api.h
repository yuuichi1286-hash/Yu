/*
 * Minimal re-declaration of the Android audio effect ABI
 * (system/audio_effect.h + hardware/audio_effect.h, effect API 3.0).
 *
 * Only what an effect library needs. Layouts must match AOSP exactly;
 * the static asserts below pin them for every ABI we build.
 */
#ifndef MICNS_ANDROID_EFFECT_API_H
#define MICNS_ANDROID_EFFECT_API_H

#include <stddef.h>
#include <stdint.h>

typedef struct effect_uuid_s {
    uint32_t timeLow;
    uint16_t timeMid;
    uint16_t timeHiAndVersion;
    uint16_t clockSeq;
    uint8_t node[6];
} effect_uuid_t;

#define EFFECT_STRING_LEN_MAX 64

typedef struct effect_descriptor_s {
    effect_uuid_t type;
    effect_uuid_t uuid;
    uint32_t apiVersion;
    uint32_t flags;
    uint16_t cpuLoad;
    uint16_t memoryUsage;
    char name[EFFECT_STRING_LEN_MAX];
    char implementor[EFFECT_STRING_LEN_MAX];
} effect_descriptor_t;

typedef struct audio_buffer_s {
    size_t frameCount;
    union {
        void *raw;
        float *f32;
        int32_t *s32;
        int16_t *s16;
        uint8_t *u8;
    };
} audio_buffer_t;

typedef int32_t (*buffer_function_t)(void *cookie, audio_buffer_t *buffer);

typedef struct buffer_provider_s {
    buffer_function_t getBuffer;
    buffer_function_t releaseBuffer;
    void *cookie;
} buffer_provider_t;

typedef struct buffer_config_s {
    audio_buffer_t buffer;
    uint32_t samplingRate;
    uint32_t channels;
    buffer_provider_t bufferProvider;
    uint8_t format;
    uint8_t accessMode;
    uint16_t mask;
} buffer_config_t;

typedef struct effect_config_s {
    buffer_config_t inputCfg;
    buffer_config_t outputCfg;
} effect_config_t;

typedef struct effect_param_s {
    int32_t status;
    uint32_t psize;
    uint32_t vsize;
    char data[];
} effect_param_t;

struct effect_interface_s;
typedef const struct effect_interface_s **effect_handle_t;

struct effect_interface_s {
    int32_t (*process)(effect_handle_t self, audio_buffer_t *inBuffer,
                       audio_buffer_t *outBuffer);
    int32_t (*command)(effect_handle_t self, uint32_t cmdCode, uint32_t cmdSize,
                       void *pCmdData, uint32_t *replySize, void *pReplyData);
    int32_t (*get_descriptor)(effect_handle_t self, effect_descriptor_t *pDescriptor);
    int32_t (*process_reverse)(effect_handle_t self, audio_buffer_t *inBuffer,
                               audio_buffer_t *outBuffer);
};

typedef struct audio_effect_library_s {
    uint32_t tag;
    uint32_t version;
    const char *name;
    const char *implementor;
    int32_t (*create_effect)(const effect_uuid_t *uuid, int32_t sessionId, int32_t ioId,
                             effect_handle_t *pHandle);
    int32_t (*release_effect)(effect_handle_t handle);
    int32_t (*get_descriptor)(const effect_uuid_t *uuid, effect_descriptor_t *pDescriptor);
    /* API 3.1 only; left NULL because we advertise 3.0. */
    int32_t (*create_effect_3_1)(const effect_uuid_t *uuid, int32_t sessionId, int32_t ioId,
                                 int32_t deviceId, effect_handle_t *pHandle);
} audio_effect_library_t;

#define EFFECT_MAKE_API_VERSION(M, m) (((M) << 16) | ((m) & 0xFFFF))
#define EFFECT_CONTROL_API_VERSION EFFECT_MAKE_API_VERSION(2, 0)
#define EFFECT_LIBRARY_API_VERSION EFFECT_MAKE_API_VERSION(3, 0)
#define AUDIO_EFFECT_LIBRARY_TAG ((('A') << 24) | (('E') << 16) | (('L') << 8) | ('T'))

enum {
    EFFECT_CMD_INIT = 0,
    EFFECT_CMD_SET_CONFIG = 1,
    EFFECT_CMD_RESET = 2,
    EFFECT_CMD_ENABLE = 3,
    EFFECT_CMD_DISABLE = 4,
    EFFECT_CMD_SET_PARAM = 5,
    EFFECT_CMD_SET_PARAM_DEFERRED = 6,
    EFFECT_CMD_SET_PARAM_COMMIT = 7,
    EFFECT_CMD_GET_PARAM = 8,
    EFFECT_CMD_SET_DEVICE = 9,
    EFFECT_CMD_SET_VOLUME = 10,
    EFFECT_CMD_SET_AUDIO_MODE = 11,
    EFFECT_CMD_SET_CONFIG_REVERSE = 12,
    EFFECT_CMD_SET_INPUT_DEVICE = 13,
    EFFECT_CMD_GET_CONFIG = 14,
    EFFECT_CMD_GET_CONFIG_REVERSE = 15,
    EFFECT_CMD_GET_FEATURE_SUPPORTED_CONFIGS = 16,
    EFFECT_CMD_GET_FEATURE_CONFIG = 17,
    EFFECT_CMD_SET_FEATURE_CONFIG = 18,
    EFFECT_CMD_SET_AUDIO_SOURCE = 19,
    EFFECT_CMD_OFFLOAD = 20,
    EFFECT_CMD_DUMP = 21,
};

enum {
    EFFECT_CONFIG_BUFFER = 1 << 0,
    EFFECT_CONFIG_SMP_RATE = 1 << 1,
    EFFECT_CONFIG_CHANNELS = 1 << 2,
    EFFECT_CONFIG_FORMAT = 1 << 3,
    EFFECT_CONFIG_ACC_MODE = 1 << 4,
    EFFECT_CONFIG_ALL = 0x1f,
};

enum {
    EFFECT_BUFFER_ACCESS_WRITE = 0,
    EFFECT_BUFFER_ACCESS_READ = 1,
    EFFECT_BUFFER_ACCESS_ACCUMULATE = 2,
};

#define EFFECT_FLAG_TYPE_PRE_PROC 3u
#define EFFECT_FLAG_DEVICE_IND (1u << 9)

#define AUDIO_FORMAT_PCM_16_BIT 0x1u

/* All positional input channel bits (AUDIO_CHANNEL_IN_ALL). */
#define AUDIO_CHANNEL_IN_ALL 0x77FFFCu
#define AUDIO_CHANNEL_REPRESENTATION_INDEX 2u

/* Noise suppression parameters (audio_effects/effect_ns.h). */
#define NS_PARAM_LEVEL 0
#define NS_LEVEL_LOW 0
#define NS_LEVEL_MEDIUM 1
#define NS_LEVEL_HIGH 2

_Static_assert(sizeof(effect_uuid_t) == 16, "effect_uuid_t");
_Static_assert(sizeof(effect_descriptor_t) == 172, "effect_descriptor_t");
_Static_assert(offsetof(buffer_config_t, samplingRate) == 2 * sizeof(void *), "buffer_config_t");
_Static_assert(offsetof(buffer_config_t, format) == 4 * sizeof(void *) + 8 + sizeof(void *),
               "buffer_config_t.format");
_Static_assert(sizeof(effect_param_t) == 12, "effect_param_t");

#endif
