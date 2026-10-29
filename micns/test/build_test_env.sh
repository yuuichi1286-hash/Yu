#!/usr/bin/env bash
# Builds everything the tests need into $1 (default ./tbuild):
#   host/      glibc build of the library from source (+ ASan/UBSan variant)
#   <abi>/     harness + liblog/libc/libm shims able to dlopen the Android
#              build of that ABI (x86_64 and x86 natively, arm64/arm via qemu)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src"
T="${1:-$HERE/tbuild}"
mkdir -p "$T"

SRCS=(
  "$SRC/micns_effect.c" "$SRC/speexdsp/resample.c" "$SRC/rnnoise/denoise.c"
  "$SRC/rnnoise/rnn.c" "$SRC/rnnoise/rnn_data.c" "$SRC/rnnoise/pitch.c"
  "$SRC/rnnoise/kiss_fft.c" "$SRC/rnnoise/celt_lpc.c"
)
DEFS=(-DFLOATING_POINT -DOUTSIDE_SPEEX -DRANDOM_PREFIX=micns -DEXPORT= -I "$SRC" -I "$HERE/hostinc")

shims() { # cc flags dir
  local cc="$1" flags="$2" dir="$3"
  mkdir -p "$dir"
  # shellcheck disable=SC2086
  $cc $flags -shared -fPIC -Wl,-soname,liblog.so -o "$dir/liblog.so" "$HERE/liblog_shim.c"
  # shellcheck disable=SC2086
  $cc $flags -shared -fPIC -Wl,-soname,libc.so -o "$dir/libc.so" "$HERE/empty_shim.c"
  # shellcheck disable=SC2086
  $cc $flags -shared -fPIC -Wl,-soname,libm.so -o "$dir/libm.so" "$HERE/empty_shim.c"
  # shellcheck disable=SC2086
  $cc $flags -shared -fPIC -Wl,-soname,libdl.so -o "$dir/libdl.so" "$HERE/empty_shim.c"
  # shellcheck disable=SC2086
  $cc $flags -shared -fPIC -DFAKE_HAL -I "$SRC" -o "$dir/fakehal.so" "$HERE/hal_test.c"
  # shellcheck disable=SC2086
  $cc $flags -O1 -g -I "$SRC" -o "$dir/hal_test" "$HERE/hal_test.c" -Wl,--no-as-needed -lm -ldl
  # shellcheck disable=SC2086
  $cc $flags -O1 -g -o "$dir/harness" "$HERE/harness.c" -Wl,--no-as-needed -lm -ldl
}

# Host builds straight from source (glibc libc/libm, shim liblog only).
mkdir -p "$T/host" "$T/host-asan"
clang -shared -fPIC -Wl,-soname,liblog.so -o "$T/host/liblog.so" "$HERE/liblog_shim.c"
cp "$T/host/liblog.so" "$T/host-asan/"
clang -O1 -g -o "$T/host/harness" "$HERE/harness.c" -Wl,--no-as-needed -lm -ldl
clang -O2 -g -shared -fPIC -fvisibility=hidden "${DEFS[@]}" -o "$T/host/libmicns.so" \
  "${SRCS[@]}" -L"$T/host" -llog -lm
gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -fno-omit-frame-pointer -shared -fPIC "${DEFS[@]}" -o "$T/host-asan/libmicns.so" \
  "${SRCS[@]}" -L"$T/host" -llog -lm
gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -o "$T/host-asan/harness" "$HERE/harness.c" -Wl,--no-as-needed -lm -ldl
# shellcheck disable=SC2054 # the comma belongs to -fsanitize
ASAN=(-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -I "$SRC" -I "$HERE/hostinc")
gcc "${ASAN[@]}" -shared -fPIC -o "$T/host-asan/audio.primary.micns.so" "$SRC/micns_hal.c" \
  "$HERE/empty_shim.c" -L"$T/host" -llog -ldl
gcc "${ASAN[@]}" -shared -fPIC -DFAKE_HAL -o "$T/host-asan/fakehal.so" "$HERE/hal_test.c"
gcc "${ASAN[@]}" -o "$T/host-asan/hal_test" "$HERE/hal_test.c" -Wl,--no-as-needed -ldl

# Loaders for the Android builds.
shims clang "" "$T/x86_64"
shims clang "-m32" "$T/x86"
shims aarch64-linux-gnu-gcc "" "$T/arm64-v8a"
shims arm-linux-gnueabi-gcc "-march=armv7-a -mfloat-abi=softfp -mfpu=neon" "$T/armeabi-v7a"
echo "test env ready in $T"
