#!/usr/bin/env bash
# Dry-runs the module installer (customize.sh) inside a chroot that looks
# like a few real phones, with BusyBox as the only userland - the same
# shell and awk Magisk/KernelSU/APatch give the installer.
#
# usage: tools/sim_install.sh MODULE_ZIP SAMPLE_DIR WORK_DIR [profile...]
# profiles: daisy mtk legacy7 pixel_aidl daisy_update
set -euo pipefail

ZIP="$(readlink -f "$1")"
SAMPLES="$(readlink -f "$2")"
WORK="$(readlink -f "$3")"
shift 3
PROFILES=("$@")
[ ${#PROFILES[@]} -eq 0 ] && PROFILES=(daisy mtk legacy7 pixel_aidl daisy_update)
BB="$(command -v busybox)"
ID=Mic_VolBoost_Swap_A2L

mkroot() {
  local fr="$1"
  rm -rf "$fr"
  mkdir -p "$fr"/{bin,system/bin,system/etc,vendor/etc,dev,tmp,data/adb/modules,data/adb/modules_update,test,proc}
  cp "$BB" "$fr/bin/busybox"
  for a in $("$BB" --list); do [ "$a" = busybox ] || ln -sf busybox "$fr/bin/$a"; done
  # getprop backed by a props file
  cat > "$fr/system/bin/getprop" <<'EOF'
#!/bin/sh
[ -z "$1" ] && exit 0
sed -n "s/^$1=//p" /test/props | head -n 1
EOF
  chmod 755 "$fr/system/bin/getprop"
  cp "$ZIP" "$fr/test/module.zip"
}

# installer environment of Magisk's install_module: functions + variables
write_driver() {
  local fr="$1" api="$2" arch="$3"
  cat > "$fr/test/run.sh" <<EOF
export PATH=/bin:/system/bin
ui_print() { echo "\$1"; }
abort() { echo "ABORT: \$1"; exit 1; }
set_perm() { chown \$2:\$3 "\$1"; chmod \$4 "\$1"; echo "\$1 \${5:-u:object_r:system_file:s0}" >> /test/contexts; }
set_perm_recursive() {
  find "\$1" -type d | while read -r d; do set_perm "\$d" \$2 \$3 \$4 \$6; done
  find "\$1" -type f -o -type l | while read -r f; do set_perm "\$f" \$2 \$3 \$5 \$6; done
}
API=$api ARCH=$arch IS64BIT=true BOOTMODE=true MODID=$ID
MAGISK_VER=27.0 MAGISK_VER_CODE=27000
ZIPFILE=/test/module.zip
MODPATH=/data/adb/modules_update/$ID
TMPDIR=/dev/tmp
rm -rf "\$MODPATH" "\$TMPDIR"; mkdir -p "\$MODPATH" "\$TMPDIR"
: > /test/contexts
unzip -o -q "\$ZIPFILE" customize.sh -d "\$MODPATH"
. "\$MODPATH/customize.sh"
EOF
}

props() { printf '%s\n' "$@" > "$FR/test/props"; }
dummy() { mkdir -p "$(dirname "$FR$1")"; echo "ELF stub" > "$FR$1"; }

run_install() {
  echo "================ $1"
  chroot "$FR" /bin/sh /test/run.sh
  echo "---- module tree"
  (cd "$FR/data/adb/modules_update/$ID" && find . -type f | sort | grep -v '^./common/' | sed 's/^/   /')
  if [ -f "$FR/data/adb/modules_update/$ID/system.prop" ]; then
    echo "---- system.prop"; sed 's/^/   /' "$FR/data/adb/modules_update/$ID/system.prop"
  fi
}

for p in "${PROFILES[@]}"; do
  FR="$WORK/$p"
  mkroot "$FR"
  case "$p" in
    daisy|daisy_update)
      props ro.product.device=daisy_sprout ro.product.name=daisy ro.product.model="Mi A2 Lite" \
        ro.product.manufacturer=Xiaomi ro.board.platform=msm8953 ro.hardware=qcom \
        ro.build.version.release=10 ro.vendor.audio.sdk.fluencetype=fluence \
        persist.vendor.audio.fluence.voicecall=true persist.vendor.audio.fluence.speaker=true \
        persist.vendor.audio.fluence.voicerec=false
      cp "$SAMPLES/crdroid_daisy.xml" "$FR/vendor/etc/audio_effects.xml"
      unzip -p "$ZIP" devices/daisy/mixer_paths_mtp_global.xml > "$FR/vendor/etc/mixer_paths_mtp_global.xml"
      dummy /vendor/lib/soundfx/libqcomvoiceprocessing.so
      dummy /vendor/lib64/soundfx/libqcomvoiceprocessing.so
      write_driver "$FR" 29 arm64
      ;;
    mtk)
      props ro.product.device=lancelot ro.product.model="Redmi 9" ro.product.manufacturer=Xiaomi \
        ro.board.platform=mt6768 ro.hardware=mt6768 ro.soc.manufacturer=Mediatek ro.build.version.release=11
      cp "$SAMPLES/mtk_like.xml" "$FR/vendor/etc/audio_effects.xml"
      dummy /vendor/lib/soundfx/libaudiopreprocessing.so
      dummy /vendor/lib64/soundfx/libaudiopreprocessing.so
      write_driver "$FR" 30 arm64
      ;;
    legacy7)
      props ro.product.device=santoni ro.product.model="Redmi 4X" ro.product.manufacturer=Xiaomi \
        ro.board.platform=msm8937 ro.hardware=qcom ro.build.version.release=7.1.2 \
        ro.qc.sdk.audio.fluencetype=fluence
      cp "$SAMPLES/aosp_cm14.conf" "$FR/system/etc/audio_effects.conf"
      cp "$SAMPLES/qcom_lineage-18.1-caf-msm8996_msm8937.conf" "$FR/vendor/etc/audio_effects.conf"
      dummy /system/lib/soundfx/libaudiopreprocessing.so
      write_driver "$FR" 25 arm
      ;;
    pixel_aidl)
      props ro.product.device=husky ro.product.model="Pixel 8 Pro" ro.product.manufacturer=Google \
        ro.board.platform=zuma ro.soc.manufacturer=Google ro.build.version.release=14
      cp "$SAMPLES/aosp_l21.xml" "$FR/vendor/etc/audio_effects_config.xml"
      mkdir -p "$FR/odm/etc"
      cp "$SAMPLES/aosp_l21.xml" "$FR/odm/etc/audio_effects.xml"
      dummy /vendor/lib64/soundfx/libaudiopreprocessing.so
      write_driver "$FR" 34 arm64
      ;;
  esac
  run_install "$p"
  if [ "$p" = daisy_update ]; then
    # Second install over the first: the patched file is what the device
    # shows now, the user changed a setting and an older settings.conf
    # lacks a key that the new version introduced.
    M="$FR/data/adb/modules/$ID"
    rm -rf "$M"
    cp -a "$FR/data/adb/modules_update/$ID" "$M"
    cp "$M/system/vendor/etc/audio_effects.xml" "$FR/vendor/etc/audio_effects.xml"
    sed -i 's/^NS_STRENGTH=.*/NS_STRENGTH=70/; /^VOICE_GATE_DB=/d' "$M/settings.conf"
    run_install "daisy_update (second install)"
  fi
done
