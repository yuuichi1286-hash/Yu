#!/system/bin/sh
# Shows what is active in the module description once the device booted.
MODDIR=${0%/*}
. "$MODDIR/common/functions.sh"
until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 5; done
update_description "$MODDIR"
