#!/system/bin/sh
# Files on partitions the module managers do not overlay (a separate /odm,
# /my_product, ...) are bind-mounted here, before the audio HAL starts.
MODDIR=${0%/*}
[ -d "$MODDIR/bind" ] || exit 0
cd "$MODDIR/bind" || exit 0
find . -type f | while read -r f; do
  target="/${f#./}"
  [ -f "$target" ] && mount -o bind "$MODDIR/bind/${f#./}" "$target"
done
