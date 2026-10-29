# Mic Boost + AI Noise Suppression (Magisk module)

Update of the **Mic_VolBoost_Swap_A2L** Magisk module (Mi A2 Lite mic boost
and L/R stereo swap) that adds Discord-style noise suppression to every
microphone path on any Android device, plus an optional natural "deep
voice" (pitch-shifted speech with preserved formants and a bass shelf).

- Flashable zip: [`release/Mic_VolBoost_Swap_A2L-v3.3.zip`](release/Mic_VolBoost_Swap_A2L-v3.3.zip)
- User guide (Bahasa Indonesia): [`module/README.md`](module/README.md)

## Layout

| Path | What |
| --- | --- |
| `module/` | Module source: installer (`customize.sh`), boot scripts, Action button, settings, config patchers (`common/*.awk`), Mi A2 Lite mixer |
| `micns/` | `libmicns.so`: RNNoise wrapped as an Android audio pre-processing effect (effect API 3.0), plus SpeexDSP resampler |
| `micns/test/` | Effect tests: API conformance, randomized stress (ASan/UBSan), bit-exact FIFO/latency, denoising quality on real speech + noise |
| `tools/build_module.sh` | Builds the libraries for arm64-v8a, armeabi-v7a, x86, x86_64 and zips the module |
| `tools/test_patchers.py` | Runs the `audio_effects.xml`/`.conf` patchers on real vendor files with BusyBox awk and mawk and checks the result against a model of Android's loaders |
| `tools/sim_install.sh` | Runs the installer in BusyBox chroots that mimic a Mi A2 Lite, a MediaTek phone, an Android 7 phone and a Pixel with an AIDL effect HAL |

## How the noise suppression reaches every app

1. The installer patches the device's own `audio_effects.xml` /
   `audio_effects.conf` / `audio_effects_config.xml` (only inserting, wrapped
   in markers so a reinstall starts from the original) so the framework
   attaches a noise suppressor to the `mic`, `voice_recognition` and
   `voice_communication` capture sources for every app.
2. On HALs that run software pre-processing effects (MediaTek, Unisoc, ...)
   that suppressor is `libmicns.so` (RNNoise at 48 kHz with resampling and
   a decision-directed Wiener post-filter on its gains, high-pass, voice
   gate, clarity EQ, leveler, PSOLA deep voice driven by RNNoise's pitch
   tracker, and limiter). VoIP gets the device's AEC + NS followed by
   `micns_call` (the same chain without the leveler), last in the chain so
   it only ever sees echo-cancelled audio.
3. Qualcomm HALs never run software effects, so there the module puts
   `audio.primary.micns.so` in front of the real HAL
   (`ro.hardware.audio.primary=micns`): it loads the platform HAL, wraps
   `open/close_input_stream` and the `read()` of each capture stream, and
   runs MicNS in place on what the HAL (and its DSP AEC/NS) returns
   (`micns/src/micns_hal.c`, tested against a fake HAL in
   `micns/test/hal_test.c`). `service.sh` removes it again if audio does not
   come up or audioserver keeps restarting.
4. Qualcomm HALs hand capture-side noise suppression to the DSP: the same
   config entries switch on its ECNS/NS devices, and the installer enables
   Fluence (multi-mic NS) for calls, speakerphone, voice typing and
   recording when the device supports it.

## Build and test

Requirements: clang + lld (no NDK needed), zip, python3 with numpy; for the
cross-ABI tests qemu-user plus aarch64/armel glibc cross toolchains, busybox
and mawk.

```sh
tools/build_module.sh                       # -> release/<id>-<version>.zip
micns/test/run_all.sh OUT AUDIO             # OUT from micns/build.sh, AUDIO = MS-SNSD clips
tools/test_patchers.py SAMPLES              # real audio_effects files
tools/sim_install.sh ZIP SAMPLES WORKDIR    # installer dry runs
```

## Credits

Original module by pexxie. RNNoise and the SpeexDSP resampler are by
Xiph.Org (BSD 3-clause, see `micns/third_party_licenses/`).
