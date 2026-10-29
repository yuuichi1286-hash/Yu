#!/system/bin/sh
# Action button: AI noise suppression on/off without a reboot (the effect
# re-reads micns.conf whenever a recording starts), plus a status summary.
MODDIR=${0%/*}
. "$MODDIR/common/functions.sh"
SETTINGS=$MODDIR/settings.conf

echo "Mic Boost + AI Noise Suppression"
echo "--------------------------------"
conf=$(micns_conf_in "$MODDIR")
if [ -n "$conf" ]; then
  if grep -q '^enable=1' "$conf"; then
    micns_conf_set "$conf" enable 0
    echo "AI noise suppression: OFF"
  else
    micns_conf_set "$conf" enable 1
    echo "AI noise suppression: ON"
  fi
  echo "Berlaku mulai rekaman berikutnya, tanpa reboot."
  echo "(Applies from the next recording, no reboot.)"
else
  echo "AI noise suppression tidak terpasang (AI_NS=0 / ABI tidak didukung)."
fi
echo
echo "Status:"
for f in $(effect_configs); do
  grep -q 'micns:' "$f" 2>/dev/null && echo "  NS aktif di $f"
done
fl=$(getprop persist.vendor.audio.fluence.voicecall)
[ -n "$fl" ] && echo "  Qualcomm Fluence: call=$fl speaker=$(getprop persist.vendor.audio.fluence.speaker) rec=$(getprop persist.vendor.audio.fluence.audiorec) voicerec=$(getprop persist.vendor.audio.fluence.voicerec)"
if [ "$(soc_vendor)" = qcom ]; then
  echo
  echo "Qualcomm: peredam bising dijalankan DSP (Fluence/ECNS)."
  echo "Tombol ini hanya mengatur AI RNNoise untuk HAL yang menjalankan efek software."
fi
echo
echo "Laporan instalasi: $MODDIR/micns_report.txt"
echo "Pengaturan: $SETTINGS"
update_description "$MODDIR"
sleep 2
