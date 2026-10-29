# shellcheck shell=sh
# shellcheck disable=SC2034
# Mic Boost + AI Noise Suppression - installer (Magisk, KernelSU, APatch).
SKIPUNZIP=1

COMMON=$MODPATH/common
REPORT=$MODPATH/micns_report.txt

ui_print "*******************************************"
ui_print "  Mic Boost + AI Noise Suppression  v3.2   "
ui_print "  AI NS - Deep Voice - Fluence - AEC - A2L  "
ui_print "*******************************************"

unzip -o "$ZIPFILE" -x 'META-INF/*' -d "$MODPATH" >&2 || abort "! Unable to extract the module"
[ -f "$COMMON/functions.sh" ] || abort "! Module files missing"
. "$COMMON/functions.sh"

[ -z "$MODID" ] && MODID=$(sed -n 's/^id=//p' "$MODPATH/module.prop")

# Keep the user's settings across updates; new keys get their defaults.
SETTINGS=$MODPATH/settings.conf
OLD=/data/adb/modules/$MODID/settings.conf
if [ -f "$OLD" ]; then
  cp -f "$SETTINGS" "$TMPDIR/settings.default"
  cp -f "$OLD" "$SETTINGS"
  for key in $(sed -n 's/^\([A-Z0-9_]*\)=.*/\1/p' "$TMPDIR/settings.default"); do
    grep -q "^$key=" "$SETTINGS" || grep "^$key=" "$TMPDIR/settings.default" >> "$SETTINGS"
  done
  # v3.1 deep voice defaults follow the new ones unless the user changed them
  sed -i 's/^DEEP_PITCH=-2$/DEEP_PITCH=-2.5/; s/^DEEP_FORMANT=-4$/DEEP_FORMANT=-5/; s/^DEEP_BASS=4$/DEEP_BASS=5/' "$SETTINGS"
  ui_print "- Keeping your settings.conf"
fi

: > "$REPORT"
report() { echo "$1" >> "$REPORT"; ui_print "$1"; }

SOC=$(soc_vendor)
report "- Device : $(prop ro.product.manufacturer) $(prop ro.product.model) ($(prop ro.product.device))"
report "- Android: $(prop ro.build.version.release) (API $API), $ARCH, SoC: $SOC"
if [ -n "$KSU" ]; then MANAGER="KernelSU"; elif [ -n "$APATCH" ]; then MANAGER="APatch"; else MANAGER="Magisk $MAGISK_VER"; fi
report "- Manager: $MANAGER"

# ---------------------------------------------------------------------------
# 1. MicNS AI noise suppression library (RNNoise)
# ---------------------------------------------------------------------------
MICNS=0
if [ "$API" -ge 26 ]; then FXROOT=/vendor; FXBASE=$MODPATH/system/vendor; else FXROOT=/system; FXBASE=$MODPATH/system; fi
if [ "$(setting AI_NS 1)" = 1 ]; then
  case "$ARCH" in
    arm64) ABIS="arm64-v8a:lib64 armeabi-v7a:lib" ;;
    arm) ABIS="armeabi-v7a:lib" ;;
    x64) ABIS="x86_64:lib64 x86:lib" ;;
    x86) ABIS="x86:lib" ;;
    *) ABIS="" ;;
  esac
  for pair in $ABIS; do
    abi=${pair%%:*}
    dir=${pair##*:}
    [ -f "$MODPATH/lib/$abi/$MICNS_LIB" ] || continue
    mkdir -p "$FXBASE/$dir/soundfx"
    cp -f "$MODPATH/lib/$abi/$MICNS_LIB" "$FXBASE/$dir/soundfx/$MICNS_LIB"
    MICNS=1
  done
  if [ $MICNS = 1 ]; then
    report "- AI noise suppression (RNNoise): installed for $ABIS"
  else
    report "- AI noise suppression: no build for $ARCH, using the device's own NS"
  fi
else
  report "- AI noise suppression: off in settings.conf"
fi
rm -rf "${MODPATH:?}/lib"

# Runtime settings the effect reads at the start of every recording.
if [ $MICNS = 1 ]; then
  mkdir -p "$FXBASE/etc"
  cat > "$FXBASE/etc/micns.conf" <<EOF
# MicNS runtime settings, read at the start of every recording (no reboot
# needed). Generated from settings.conf; the Action button toggles enable.
enable=1
strength=$(setting NS_STRENGTH 90)
postfilter=$(setting NS_POSTFILTER 1)
hpf=$(setting VOICE_LOWCUT 1)
clarity=$(setting VOICE_CLARITY 1)
leveler=$(setting VOICE_LEVELER 1)
gain_db=$(setting VOICE_GAIN_DB 0)
gate=$(setting VOICE_GATE 1)
gate_db=$(setting VOICE_GATE_DB 18)
deep=$(setting VOICE_DEEP 1)
deep_pitch=$(setting DEEP_PITCH -2.5)
deep_formant=$(setting DEEP_FORMANT -5)
deep_bass=$(setting DEEP_BASS 5)
EOF
fi

# ---------------------------------------------------------------------------
# 2. Noise suppression on every capture path (audio_effects configs)
# ---------------------------------------------------------------------------
if [ $MICNS = 1 ]; then
  PLAN="mic=ai;voice_recognition=ai_pure"
  [ "$(setting NS_CAMCORDER 0)" = 1 ] && PLAN="$PLAN;camcorder=ai_pure"
else
  if [ "$(setting VOICE_LEVELER 1)" = 1 ]; then PLAN="mic=ns,agc"; else PLAN="mic=ns"; fi
  PLAN="$PLAN;voice_recognition=ns"
  [ "$(setting NS_CAMCORDER 0)" = 1 ] && PLAN="$PLAN;camcorder=ns"
fi
# Internet calls: the platform echo canceller + NS, then MicNS on top
# (micns_call, last in the chain so it never sees the echo).
VC=""
[ "$(setting NS_VOIP 1)" = 1 ] && VC="aec,ns"
[ $MICNS = 1 ] && [ "$(setting NS_VOIP_AI 1)" = 1 ] && VC="${VC:+$VC,}ai_call"
[ -n "$VC" ] && PLAN="$PLAN;voice_communication=$VC"

AOSP_OK=0
AOSP_PATH=$(soundfx_lib libaudiopreprocessing.so) && AOSP_OK=1
# On Treble devices only the vendor copy is loadable by the effect HAL.
if [ "$API" -ge 26 ]; then
  case "$AOSP_PATH" in /system/*) AOSP_OK=0 ;; esac
fi
AOSP_CONF_PATH=$(echo "$AOSP_PATH" | sed 's|/lib64/|/lib/|')

PATCHED=0
for src in $(effect_configs); do
  dst=$(module_path_for "$src")
  case "$src" in *.conf) kind=conf ;; *) kind=xml ;; esac
  ok=$MICNS
  aosp_ok=$AOSP_OK
  aosp_file=libaudiopreprocessing.so
  # The AIDL effect HAL (audio_effects_config.xml) cannot load legacy
  # libraries; it has its own build of the AOSP pre-processing effects.
  case "$src" in
    *audio_effects_config.xml)
      ok=0
      aosp_file=libpreprocessingaidl.so
      aosp_ok=0
      soundfx_lib $aosp_file >/dev/null && aosp_ok=1
      ;;
  esac
  report "- Patching $src"
  awk -f "$COMMON/micns_strip.awk" "$src" > "$TMPDIR/orig" || continue
  mkdir -p "${dst%/*}"
  if [ $kind = xml ]; then
    awk -v plan="$PLAN" -v micns_ok=$ok -v aosp_ok=$aosp_ok -v aosp_file=$aosp_file \
      -v report="$TMPDIR/rep" -f "$COMMON/micns_xml.awk" "$TMPDIR/orig" > "$dst"
  else
    awk -v plan="$PLAN" -v micns_ok=$ok -v aosp_ok=$AOSP_OK -v report="$TMPDIR/rep" \
      -v micns_path="$FXROOT/lib/soundfx/$MICNS_LIB" -v aosp_path="$AOSP_CONF_PATH" \
      -f "$COMMON/micns_conf.awk" "$TMPDIR/orig" > "$dst"
  fi
  [ -f "$TMPDIR/rep" ] && { cat "$TMPDIR/rep" >> "$REPORT"; while IFS= read -r l; do ui_print "$l"; done < "$TMPDIR/rep"; rm -f "$TMPDIR/rep"; }
  if [ ! -s "$dst" ] || cmp -s "$dst" "$TMPDIR/orig"; then
    # nothing to change (or patching failed): leave the original visible
    rm -f "$dst"
    rmdir -p "${dst%/*}" 2>/dev/null
    continue
  fi
  if [ $kind = xml ] && ! grep -q '</audio_effects_conf>' "$dst"; then
    report "  ! output looks broken, keeping the original"
    rm -f "$dst"
    continue
  fi
  ctx=$(file_ctx "$src")
  chmod 0644 "$dst"
  echo "$dst $ctx" >> "$TMPDIR/ctx"
  PATCHED=$((PATCHED + 1))
done
[ $PATCHED = 0 ] && report "- No audio effects config needed changes"

# Other modules that replace the same files win or lose depending on mount
# order; tell the user instead of guessing.
for m in /data/adb/modules/*; do
  [ -d "$m" ] || continue
  [ "${m##*/}" = "$MODID" ] && continue
  [ -f "$m/disable" ] || [ -f "$m/remove" ] && continue
  for f in system/vendor/etc/audio_effects.xml system/etc/audio_effects.xml \
    system/vendor/etc/audio_effects.conf system/etc/audio_effects.conf; do
    [ -f "$m/$f" ] && report "  ! Module '${m##*/}' also replaces /${f#system/}. Install MicNS after it (or disable one)."
  done
done

# ---------------------------------------------------------------------------
# 3. Qualcomm Fluence: multi-mic noise suppression in the DSP for calls,
#    speakerphone, voice typing and recordings
# ---------------------------------------------------------------------------
FLUENCE=$(setting FLUENCE auto)
if [ "$SOC" = qcom ] && [ "$FLUENCE" != off ]; then
  FTYPE=$(prop ro.vendor.audio.sdk.fluencetype)
  LEGACY_FTYPE=$(prop ro.qc.sdk.audio.fluencetype)
  [ -z "$FTYPE" ] && FTYPE=$LEGACY_FTYPE
  case "$FTYPE" in fluence*) CAPABLE=1 ;; *) CAPABLE=0 ;; esac
  if [ $CAPABLE = 1 ] || [ "$FLUENCE" = force ]; then
    REC=$(setting FLUENCE_RECORD 1)
    {
      echo "# Qualcomm Fluence multi-mic noise suppression (MicNS)"
      if [ $CAPABLE = 0 ]; then
        echo "ro.vendor.audio.sdk.fluencetype=fluence"
        echo "ro.qc.sdk.audio.fluencetype=fluence"
      fi
      for p in persist.vendor.audio.fluence persist.audio.fluence; do
        # the pre-Android 9 names are only read by old HALs
        [ $p = persist.audio.fluence ] && [ "$API" -ge 28 ] && [ -z "$LEGACY_FTYPE" ] && continue
        echo "$p.voicecall=true"
        echo "$p.speaker=true"
        echo "$p.voicerec=true"
        [ "$REC" = 1 ] && echo "$p.audiorec=true"
      done
    } > "$MODPATH/system.prop"
    if [ $CAPABLE = 1 ]; then
      what="calls, speakerphone, voice typing"
      [ "$REC" = 1 ] && what="$what, recording"
      report "- Qualcomm Fluence ($FTYPE): $what"
    else
      report "- Qualcomm Fluence forced on (device reports '${FTYPE:-none}')"
    fi
  else
    report "- Qualcomm Fluence: not supported by this device (fluencetype '${FTYPE:-none}')"
  fi
fi

# ---------------------------------------------------------------------------
# 4. Mi A2 Lite mixer (the original module: mic boost + L/R stereo swap)
# ---------------------------------------------------------------------------
A2L=$(setting A2L_MIXER auto)
MIXER=/vendor/etc/mixer_paths_mtp_global.xml
if [ "$A2L" != off ] && [ -f "$MIXER" ] && { is_daisy || [ "$A2L" = force ]; }; then
  dst=$(module_path_for "$MIXER")
  mkdir -p "${dst%/*}"
  cp -f "$MODPATH/devices/daisy/mixer_paths_mtp_global.xml" "$dst"
  echo "$dst $(file_ctx "$MIXER")" >> "$TMPDIR/ctx"
  report "- Mi A2 Lite: mic volume boost + L/R stereo swap applied"
elif is_daisy; then
  report "- Mi A2 Lite: $MIXER not found on this ROM, mixer left untouched"
fi
rm -rf "${MODPATH:?}/devices"

# ---------------------------------------------------------------------------
# 5. Permissions and SELinux labels
# ---------------------------------------------------------------------------
ui_print "- Setting permissions"
set_perm_recursive "$MODPATH" 0 0 0755 0644
ETC_CTX=$(file_ctx /vendor/etc/audio_effects.xml)
[ -z "$ETC_CTX" ] && ETC_CTX=$(file_ctx /vendor/etc/audio_policy_configuration.xml)
[ -z "$ETC_CTX" ] && [ "$API" -ge 28 ] && ETC_CTX=u:object_r:vendor_configs_file:s0
[ -z "$ETC_CTX" ] && ETC_CTX=u:object_r:vendor_file:s0
if [ -d "$MODPATH/system/vendor" ]; then
  set_perm_recursive "$MODPATH/system/vendor" 0 0 0755 0644 u:object_r:vendor_file:s0
  [ -d "$MODPATH/system/vendor/etc" ] && set_perm_recursive "$MODPATH/system/vendor/etc" 0 0 0755 0644 "$ETC_CTX"
fi
if [ -f "$TMPDIR/ctx" ]; then
  while read -r f c; do
    if [ -z "$c" ]; then
      case "$f" in
        "$MODPATH/system/vendor/"*|"$MODPATH/bind/"*) c=${ETC_CTX:-u:object_r:vendor_configs_file:s0} ;;
        *) c=u:object_r:system_file:s0 ;;
      esac
    fi
    set_perm "$f" 0 0 0644 "$c"
  done < "$TMPDIR/ctx"
fi
for f in action.sh service.sh post-fs-data.sh; do
  [ -f "$MODPATH/$f" ] && set_perm "$MODPATH/$f" 0 0 0755
done

ui_print ""
ui_print "- Done. Reboot to apply."
ui_print "- Report: /data/adb/modules/$MODID/micns_report.txt"
ui_print "- Settings: /data/adb/modules/$MODID/settings.conf"
[ $MICNS = 1 ] && ui_print "- Action button: AI noise suppression on/off (no reboot)"
