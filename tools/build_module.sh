#!/usr/bin/env bash
# Builds the flashable module zip:
#   1. cross-compiles libmicns.so for the four Android ABIs (micns/build.sh)
#   2. stages module/ plus the libraries and third-party licenses
#   3. zips it into release/
#
# usage: tools/build_module.sh [outdir]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/release}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

VERSION=$(sed -n 's/^version=//p' "$ROOT/module/module.prop")
ID=$(sed -n 's/^id=//p' "$ROOT/module/module.prop")

"$ROOT/micns/build.sh" "$WORK/libs" >/dev/null

STAGE="$WORK/stage"
cp -a "$ROOT/module" "$STAGE"
for abi in arm64-v8a armeabi-v7a x86_64 x86; do
  mkdir -p "$STAGE/lib/$abi"
  cp "$WORK/libs/$abi/libmicns.so" "$STAGE/lib/$abi/"
done
mkdir -p "$STAGE/licenses"
cp "$ROOT/micns/third_party_licenses/"* "$STAGE/licenses/"

# Shell scripts must keep LF endings and the executables their mode.
find "$STAGE" -name '*.sh' -exec chmod 0755 {} +
chmod 0755 "$STAGE/META-INF/com/google/android/update-binary"

ZIP="$OUT/${ID}-${VERSION}.zip"
rm -f "$ZIP"
(cd "$STAGE" && TZ=UTC find . -exec touch -h -d '2026-01-01 00:00:00' {} + && \
  find . -type f | LC_ALL=C sort | sed 's|^\./||' | zip -X -9 -q "$ZIP" -@)
echo "$ZIP"
unzip -l "$ZIP" | tail -n 1
