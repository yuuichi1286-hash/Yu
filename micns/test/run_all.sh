#!/usr/bin/env bash
# Full test pass: sanitizer stress on the host build, then the shipped
# Android builds of every ABI (x86/x86_64 natively, ARM through qemu-user)
# for API conformance, bit-exact FIFO behaviour and denoising quality.
#
#   run_all.sh OUT_DIR AUDIO_DIR [TEST_BUILD_DIR]
# OUT_DIR is ./build.sh's output, AUDIO_DIR holds the MS-SNSD clips
# (clean_test/clnsp*.wav renamed ms_clean_*.wav, noise_test/*.wav as ms_noise_*.wav).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$1"
AUDIO="$2"
T="${3:-$HERE/tbuild}"
export MICNS_QUIET=1

"$HERE/build_test_env.sh" "$T" >/dev/null

runner_for() {
  case "$1" in
    arm64-v8a) echo "env QEMU_LD_PREFIX=/usr/aarch64-linux-gnu qemu-aarch64" ;;
    armeabi-v7a) echo "env QEMU_LD_PREFIX=/usr/arm-linux-gnueabi qemu-arm" ;;
    *) echo "" ;;
  esac
}

echo "== host build under ASan/UBSan"
LD_LIBRARY_PATH="$T/host-asan" "$T/host-asan/harness" "$T/host-asan/libmicns.so" selftest 80

for abi in x86_64 x86 arm64-v8a armeabi-v7a; do
  runner="$(runner_for "$abi")"
  lib="$OUT/$abi/libmicns.so"
  echo "== $abi: selftest"
  # shellcheck disable=SC2086
  LD_LIBRARY_PATH="$T/$abi" $runner "$T/$abi/harness" "$lib" selftest 20
  echo "== $abi: FIFO / latency"
  LD_LIBRARY_PATH="$T/$abi" python3 "$HERE/audio_test.py" fifo "$T/$abi/harness" "$lib" --runner "$runner"
  for variant in pure voice; do
    echo "== $abi: denoise ($variant)"
    LD_LIBRARY_PATH="$T/$abi" python3 "$HERE/audio_test.py" denoise "$T/$abi/harness" "$lib" \
      "$AUDIO/ms_clean_clnsp2.wav" "$AUDIO/ms_noise_ac.wav" "$AUDIO/ms_noise_VacuumCleaner_1.wav" \
      "$AUDIO/ms_noise_CopyMachine_1.wav" "$AUDIO/ms_noise_Typing_1.wav" "$AUDIO/ms_noise_Babble_1.wav" \
      --rates 16000,48000 --variant "$variant" --runner "$runner"
  done
  echo "== $abi: deep voice"
  LD_LIBRARY_PATH="$T/$abi" python3 "$HERE/audio_test.py" deep "$T/$abi/harness" "$lib" \
    "$AUDIO/ms_clean_clnsp2.wav" "$AUDIO/ms_clean_clnsp4.wav" --runner "$runner"
done
echo "ALL TESTS PASSED"
