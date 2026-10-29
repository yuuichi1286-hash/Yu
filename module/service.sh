#!/system/bin/sh
# Shows what is active in the module description once the device booted.
MODDIR=${0%/*}
. "$MODDIR/common/functions.sh"
until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 5; done
update_description "$MODDIR"

# Safety net for the HAL hook: if audio does not come up with it, or the
# audio server keeps restarting, take the hook out for good and restart the
# audio HAL without it.
if [ "$(getprop ro.hardware.audio.primary)" = micns ]; then
  sleep 20
  bad=""
  af=$(timeout 15 dumpsys media.audio_flinger 2>&1)
  case "$af" in *"Output thread"*) ;; *) bad="no audio output after boot" ;; esac
  last=$(pidof audioserver)
  restarts=0
  n=0
  while [ -z "$bad" ] && [ $n -lt 360 ]; do # the first 30 minutes
    sleep 5
    p=$(pidof audioserver)
    if [ -n "$p" ] && [ "$p" != "$last" ]; then
      restarts=$((restarts + 1))
      last=$p
    fi
    [ $restarts -ge 3 ] && bad="audio server restarted $restarts times"
    n=$((n + 1))
  done
  if [ -n "$bad" ]; then
    sed -i '/^ro\.hardware\.audio\.primary=micns$/d' "$MODDIR/system.prop"
    echo "$bad" > "$MODDIR/hal_hook_off"
    resetprop --delete ro.hardware.audio.primary 2>/dev/null || resetprop -d ro.hardware.audio.primary
    for p in $(pidof android.hardware.audio.service_64 android.hardware.audio.service \
      android.hardware.audio@2.0-service); do
      kill "$p"
    done
    update_description "$MODDIR"
  fi
fi
