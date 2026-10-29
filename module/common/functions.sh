# shellcheck shell=sh
# Shared helpers for customize.sh, action.sh and service.sh.
# Callers set MODDIR (module directory); the installer also has MODPATH,
# TMPDIR, API, ARCH and Magisk's ui_print/set_perm.

# shellcheck disable=SC2034 # used by customize.sh
MICNS_LIB=libmicns.so
HOOK_LIB=audio.primary.micns.so

# Prints to the installer console when there is one, stdout otherwise.
say() {
  if command -v ui_print >/dev/null 2>&1; then ui_print "$1"; else echo "$1"; fi
}

# System property, falling back to build.prop files when getprop has
# nothing (recovery installs).
prop() {
  _v=$(getprop "$1" 2>/dev/null)
  if [ -z "$_v" ]; then
    for _f in /vendor/build.prop /odm/etc/build.prop /vendor/odm/etc/build.prop \
      /system/build.prop /system/system/build.prop /product/etc/build.prop \
      /system_ext/etc/build.prop /vendor/default.prop; do
      [ -f "$_f" ] || continue
      _v=$(sed -n "s/^$1=//p" "$_f" | head -n 1)
      [ -n "$_v" ] && break
    done
  fi
  echo "$_v"
}

# setting KEY DEFAULT: value from settings.conf (KEY=value lines). Parsed,
# not sourced, so the file can never run code.
setting() {
  _v=""
  [ -f "$SETTINGS" ] && _v=$(sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\([^#[:space:]]*\).*/\1/p" "$SETTINGS" | tail -n 1 | tr -d "\"'\r")
  [ -n "$_v" ] && echo "$_v" || echo "$2"
}

# qcom, mtk, exynos, unisoc, tensor, kirin or other.
soc_vendor() {
  _m=$(prop ro.soc.manufacturer | tr '[:upper:]' '[:lower:]')
  case "$_m" in
    qti*|qualcomm*) echo qcom; return ;;
    mediatek*) echo mtk; return ;;
    google*) echo tensor; return ;;
    samsung*) echo exynos; return ;;
    unisoc*|spreadtrum*) echo unisoc; return ;;
  esac
  if [ -n "$(prop ro.vendor.audio.sdk.fluencetype)$(prop ro.qc.sdk.audio.fluencetype)" ]; then
    echo qcom; return
  fi
  _p="$(prop ro.board.platform) $(prop ro.hardware) $(prop ro.hardware.chipname)"
  _p=$(echo "$_p" | tr '[:upper:]' '[:lower:]')
  case " $_p" in
    *" mt"[0-9]*|*mediatek*) echo mtk ;;
    *" msm"*|*" sdm"*|*" sm"[0-9]*|*" apq"*|*" qcs"*|*" trinket"*|*" bengal"*|*" lito"*|*" kona"*|\
    *" lahaina"*|*" taro"*|*" kalama"*|*" pineapple"*|*" sun "*|*" holi"*|*" atoll"*|\
    *" parrot"*|*" crow"*|*" blair"*|*" monaco"*|*" pitti"*|*" khaje"*|*" cliffs"*|*" volcano"*|\
    *qcom*) echo qcom ;;
    *exynos*|*" universal"*|*" s5e"*|*" erd"*) echo exynos ;;
    *" ums"*|*" sc9"*|*" sp9"*|*unisoc*|*sprd*) echo unisoc ;;
    *" gs1"*|*" gs2"*|*zuma*|*laguna*) echo tensor ;;
    *kirin*|*" hi36"*|*" hi37"*) echo kirin ;;
    *) echo other ;;
  esac
}

is_daisy() {
  for _p in ro.product.device ro.product.vendor.device ro.build.product ro.product.name; do
    case "$(prop "$_p")" in daisy*) return 0 ;; esac
  done
  return 1
}

# SELinux context of an existing file, empty if unknown.
file_ctx() {
  _c=$(ls -Zd "$1" 2>/dev/null | tr ' ' '\n' | grep -m 1 '^u:object_r:')
  [ -z "$_c" ] && _c=$(stat -c %C "$1" 2>/dev/null | grep '^u:object_r:')
  echo "$_c"
}

# Where a device file is replaced from inside the module: Magisk, KernelSU
# and APatch overlay /system, /vendor, /product and /system_ext through
# system/; anything else (a separate /odm, /my_product, ...) is
# bind-mounted by post-fs-data.sh from bind/.
module_path_for() {
  _r=$(readlink -f "$1" 2>/dev/null)
  [ -z "$_r" ] && _r=$1
  case "$_r" in
    /system/*) echo "$MODPATH$_r" ;;
    /vendor/*|/product/*|/system_ext/*) echo "$MODPATH/system$_r" ;;
    *) echo "$MODPATH/bind$_r" ;;
  esac
}

# Every audio effects configuration the framework or the effect HAL may read.
effect_configs() {
  for _f in \
    /odm/etc/audio_effects.xml \
    /vendor/etc/audio/sku_*/audio_effects.xml \
    /vendor/etc/audio/audio_effects.xml \
    /vendor/etc/audio_effects.xml \
    /system/etc/audio_effects.xml \
    /odm/etc/audio_effects_config.xml \
    /vendor/etc/audio_effects_config.xml \
    /vendor/etc/audio_effects.conf \
    /system/etc/audio_effects.conf; do
    [ -f "$_f" ] && echo "$_f"
  done
}

# First existing copy of a soundfx library, in the order effect loaders search.
soundfx_lib() {
  for _d in /odm/lib64 /vendor/lib64 /system/lib64 /odm/lib /vendor/lib /system/lib; do
    [ -f "$_d/soundfx/$1" ] && { echo "$_d/soundfx/$1"; return 0; }
  done
  return 1
}

# micns.conf inside the module (the copy the effect reads through the overlay).
micns_conf_in() {
  for _f in "$1/system/vendor/etc/micns.conf" "$1/system/etc/micns.conf"; do
    [ -f "$_f" ] && { echo "$_f"; return 0; }
  done
  return 1
}

# Rewrite one key of micns.conf in place. The overlay bind-mounts this very
# inode, so it must be rewritten, never replaced (no sed -i, no mv).
micns_conf_set() {
  _file=$1 _key=$2 _val=$3
  _new=$(sed "s/^$_key=.*/$_key=$_val/" "$_file")
  grep -q "^$_key=" "$_file" || _new="$_new
$_key=$_val"
  printf '%s\n' "$_new" > "$_file"
}

# The audio HAL libhardware loads (audio.primary.<variant>.so), found the
# way it does: the vendor's ro.hardware.audio.primary, then ro.hardware,
# ro.product.board, ro.board.platform, ro.arch. "micns" is our own hook.
hal_variant() {
  _vp=$(cat /odm/etc/build.prop /vendor/odm/etc/build.prop /vendor/build.prop /system/build.prop \
    2>/dev/null | sed -n 's/^ro\.hardware\.audio\.primary=//p' | head -n 1)
  for _v in "$_vp" "$(prop ro.hardware)" "$(prop ro.product.board)" "$(prop ro.board.platform)" \
    "$(prop ro.arch)" default; do
    { [ -z "$_v" ] || [ "$_v" = micns ]; } && continue
    for _d in /odm /vendor /system; do
      for _l in lib64 lib; do
        [ -f "$_d/$_l/hw/audio.primary.$_v.so" ] && { echo "$_v"; return 0; }
      done
    done
  done
  return 1
}

# Prefix the module description with a live status line, e.g.
# "[AI NS: on | NS configs: 2 | Fluence: on]".
update_description() {
  _mp=$1/module.prop
  [ -f "$_mp" ] || return
  _ai=off
  _c=$(micns_conf_in "$1") && grep -q '^enable=1' "$_c" && _ai=on
  [ -z "$_c" ] && _ai="n/a"
  _dp=off
  [ -n "$_c" ] && grep -q '^deep=1' "$_c" && _dp=on
  _hk=off
  [ "$(getprop ro.hardware.audio.primary)" = micns ] && _hk=on
  [ -f "$1/hal_hook_off" ] && _hk=failed
  _n=0
  for _f in $(effect_configs); do grep -q 'micns:' "$_f" 2>/dev/null && _n=$((_n + 1)); done
  _fl=$(getprop persist.vendor.audio.fluence.voicecall)
  [ -z "$_fl" ] && _fl=$(getprop persist.audio.fluence.voicecall)
  case "$_fl" in true) _fl=on ;; "") _fl="n/a" ;; *) _fl=off ;; esac
  _d=$(sed -n 's/^description=//p' "$_mp" | sed 's/^\[[^]]*\] *//')
  _new=$(awk -v d="description=[AI NS: $_ai | Deep: $_dp | HAL hook: $_hk | NS configs: $_n | Fluence: $_fl] $_d" \
    '/^description=/ { print d; next } { print }' "$_mp")
  printf '%s\n' "$_new" > "$_mp"
}
