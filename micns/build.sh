#!/usr/bin/env bash
# Cross-compile libmicns.so for the four Android ABIs with a stock
# clang + lld (no NDK needed). Bionic is represented by the tiny headers in
# sysroot/include and by stub libc/libm/liblog built from the exact list of
# symbols the library imports, so the result links like an NDK build:
# DT_NEEDED on libc.so/libm.so/liblog.so, only AELI exported.
#
# Usage: ./build.sh [outdir]   (default: ./out)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
API=21
CC="${CC:-clang}"

SRCS=(
  src/micns_effect.c
  src/builtins_arm.c
  src/speexdsp/resample.c
  src/rnnoise/denoise.c
  src/rnnoise/rnn.c
  src/rnnoise/rnn_data.c
  src/rnnoise/pitch.c
  src/rnnoise/kiss_fft.c
  src/rnnoise/celt_lpc.c
)

# Everything the library may import from bionic, by library.
LIBC_SYMS="malloc calloc realloc free memcpy memmove memset memcmp strlen strcmp strchr strtol abort fopen fgets fclose access __system_property_get"
LIBM_SYMS="sqrt exp log log10 pow cos sin floor fabs sqrtf expf logf log10f powf cosf sinf floorf fabsf"
LIBLOG_SYMS="__android_log_print"
LIBDL_SYMS="dlopen dlsym dlerror"

COMMON_CFLAGS=(
  -O2 -fPIC -fvisibility=hidden -fno-stack-protector -fno-math-errno
  -ffunction-sections -fdata-sections -fno-common
  -nostdlibinc -isystem "$HERE/sysroot/include" -I "$HERE/src"
  -DNDEBUG -DFLOATING_POINT -DOUTSIDE_SPEEX -DRANDOM_PREFIX=micns -DEXPORT=
  -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
  -Wno-null-dereference # rnn.c's deliberate crash on an unknown activation
  -Werror=implicit-function-declaration
  # keep sin()+cos() pairs from being fused into the GNU sincos()
  -fno-builtin-sin -fno-builtin-cos -fno-builtin-sinf -fno-builtin-cosf
)

abi_flags() {
  case "$1" in
    arm64-v8a)   echo "--target=aarch64-linux-android$API -mno-outline-atomics" ;;
    armeabi-v7a) echo "--target=armv7a-linux-androideabi$API -mfloat-abi=softfp -mfpu=neon -mthumb" ;;
    x86)         echo "--target=i686-linux-android$API -mssse3 -mfpmath=sse -mstackrealign" ;;
    x86_64)      echo "--target=x86_64-linux-android$API" ;;
  esac
}

abi_ldflags() {
  case "$1" in
    armeabi-v7a) echo "-Wl,--fix-cortex-a8" ;;
    *) echo "" ;;
  esac
}

stub_lib() { # abi flags name syms...
  local dir="$1" flags="$2" name="$3"; shift 3
  local c="$dir/$name.c"
  : > "$c"
  for s in "$@"; do echo "void $s(void) {}" >> "$c"; done
  # shellcheck disable=SC2086
  "$CC" $flags -fPIC -nostdlib -shared -fuse-ld=lld -Wno-builtin-requires-header \
    -Wno-incompatible-library-redeclaration -fno-builtin \
    -Wl,-soname,"$name.so" -o "$dir/$name.so" "$c"
}

# Every import must be one we stub; anything else (stack protector,
# 64-bit division helpers, ...) would fail to resolve on a device.
check_imports() { # abi objs...
  local abi="$1" undef defined s missing=()
  shift
  undef="$(llvm-nm -u --format=just-symbols "$@" | sort -u)"
  defined="$(llvm-nm --defined-only --format=just-symbols "$@" | sort -u)"
  for s in $undef; do
    grep -qx "$s" <<<"$defined" && continue
    [ "$s" = _GLOBAL_OFFSET_TABLE_ ] && continue # provided by the linker (i686 PIC)
    case " $LIBC_SYMS $LIBM_SYMS $LIBLOG_SYMS $LIBDL_SYMS " in
      *" $s "*) ;;
      *) missing+=("$s") ;;
    esac
  done
  if [ "${#missing[@]}" -ne 0 ]; then
    echo "[$abi] unexpected imports: ${missing[*]}" >&2
    exit 1
  fi
}

build_abi() {
  local abi="$1" flags ldflags obj_dir stub_dir
  flags="$(abi_flags "$abi")"
  ldflags="$(abi_ldflags "$abi")"
  obj_dir="$OUT/obj/$abi"
  stub_dir="$OUT/stubs/$abi"
  mkdir -p "$obj_dir" "$stub_dir" "$OUT/$abi"

  local objs=() o extra
  for s in "${SRCS[@]}"; do
    o="$obj_dir/$(basename "${s%.c}").o"
    extra=""
    # the run-time ABI helpers must not be lowered back into themselves
    [ "$(basename "$s")" = builtins_arm.c ] && extra="-fno-builtin"
    # shellcheck disable=SC2086
    "$CC" $flags "${COMMON_CFLAGS[@]}" $extra -c "$HERE/$s" -o "$o"
    objs+=("$o")
  done

  check_imports "$abi" "${objs[@]}"

  # shellcheck disable=SC2086
  stub_lib "$stub_dir" "$flags" libc $LIBC_SYMS
  # shellcheck disable=SC2086
  stub_lib "$stub_dir" "$flags" libm $LIBM_SYMS
  # shellcheck disable=SC2086
  stub_lib "$stub_dir" "$flags" liblog $LIBLOG_SYMS
  # shellcheck disable=SC2086
  stub_lib "$stub_dir" "$flags" libdl $LIBDL_SYMS

  # shellcheck disable=SC2086
  "$CC" $flags -shared -nostdlib -fuse-ld=lld \
    -Wl,-soname,libmicns.so -Wl,--version-script="$HERE/micns.map" \
    -Wl,--no-undefined -Wl,--gc-sections -Wl,--hash-style=both -Wl,--build-id=sha1 \
    -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -Wl,-z,max-page-size=16384 \
    $ldflags -o "$OUT/$abi/libmicns.so" "${objs[@]}" \
    -L"$stub_dir" -llog -lm -lc
  llvm-strip --strip-unneeded "$OUT/$abi/libmicns.so"

  # The audio HAL hook for HALs that never run software effects
  # (src/micns_hal.c); it drives libmicns.so through the effect API.
  local hook=("$obj_dir/micns_hal.o" "$obj_dir/builtins_arm.o")
  # shellcheck disable=SC2086
  "$CC" $flags "${COMMON_CFLAGS[@]}" -c "$HERE/src/micns_hal.c" -o "$obj_dir/micns_hal.o"
  check_imports "$abi" "${hook[@]}"
  # shellcheck disable=SC2086
  "$CC" $flags -shared -nostdlib -fuse-ld=lld \
    -Wl,-soname,audio.primary.micns.so -Wl,--version-script="$HERE/micns_hal.map" \
    -Wl,--no-undefined -Wl,--gc-sections -Wl,--hash-style=both -Wl,--build-id=sha1 \
    -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -Wl,-z,max-page-size=16384 \
    $ldflags -o "$OUT/$abi/audio.primary.micns.so" "${hook[@]}" \
    -L"$stub_dir" -llog -ldl -lc
  llvm-strip --strip-unneeded "$OUT/$abi/audio.primary.micns.so"
  echo "[$abi] $(stat -c %s "$OUT/$abi/libmicns.so") + $(stat -c %s "$OUT/$abi/audio.primary.micns.so") bytes"
}

cd "$HERE"
for abi in arm64-v8a armeabi-v7a x86_64 x86; do
  build_abi "$abi"
done
