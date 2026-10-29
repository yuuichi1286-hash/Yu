# Sample audio effects configurations

Inputs for `tools/test_patchers.py` and `tools/sim_install.sh`.

| File | Origin |
| --- | --- |
| `crdroid_daisy.xml` | crDroid device tree for the Mi A2 Lite (daisy), `configs/audio/audio_effects.xml` |
| `aosp_l21.xml` | AOSP `frameworks/av/media/libeffects/data/audio_effects.xml` (LineageOS 21 mirror) |
| `aosp_cm14.conf` | AOSP `audio_effects.conf` from Android 7.1 (CyanogenMod 14.1 mirror) |
| `qcom_*` | Qualcomm CAF audio HAL `configs/<platform>/` (LineageOS `android_hardware_qcom_audio`) |
| `mtk_like.xml` | Hand-written: MediaTek-style config with AOSP pre-processing effects and existing preprocess streams |
| `minified.xml`, `crlf_tabs.xml`, `bare.xml`, `legacy_*.conf`, `no_final_newline.conf` | Hand-written edge cases (single line, CRLF + tabs, name clashes, missing sections, one-line blocks, no trailing newline) |
