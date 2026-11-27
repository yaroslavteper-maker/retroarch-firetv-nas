#!/bin/bash
# Set up RetroArch NAS on a Fire TV over adb (no bridge needed).
#
#   bash firetv_setup.sh <fire-tv-ip>
#
# Does, in order (each step is skipped if its input is missing):
#   1. adb connect, install out/RetroArch-NAS.apk, grant storage permission
#   2. push smb.cfg (your share login) and smb_deps.txt (arcade parents/BIOS)
#   3. start the app once so it creates its folders and config
#   4. copy every core in out/cores/ into the app (build.sh fetch-cores ... fills that folder)
#   5. install the corrected Xbox One controller profile for Fire TV
#   6. apply recommended settings (controller hotkeys, all controllers drive the menu)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
[ $# -ge 1 ] || { echo "usage: $0 <fire-tv-ip>"; exit 1; }
TV="$1"; [[ "$TV" == *:* ]] || TV="$TV:5555"
PKG=com.retroarch.nas
DATA=/data/user/0/$PKG
SD=/storage/emulated/0
CFG=$SD/Android/data/$PKG/files/retroarch.cfg
A() { adb -s "$TV" "$@"; }
say(){ printf '\n\033[1m== %s ==\033[0m\n' "$*"; }

say "connect $TV (accept 'Allow USB debugging?' on the TV)"
adb connect "$TV"; sleep 2; A get-state

say "install app"
[ -f "$HERE/out/RetroArch-NAS.apk" ] || { echo "build first: bash build.sh"; exit 2; }
A install -r "$HERE/out/RetroArch-NAS.apk"
A shell pm grant $PKG android.permission.READ_EXTERNAL_STORAGE || true
A shell pm grant $PKG android.permission.WRITE_EXTERNAL_STORAGE || true

say "share login and arcade dependencies"
A shell mkdir -p $SD/RetroArch
if [ -f "$HERE/smb.cfg" ]; then A push "$HERE/smb.cfg" $SD/RetroArch/smb.cfg
else echo "no smb.cfg here - copy config/smb.cfg.example to smb.cfg and fill it in"; fi
[ -f "$HERE/smb_deps.txt" ] && A push "$HERE/smb_deps.txt" $SD/RetroArch/smb_deps.txt || true

say "first start (creates config)"
A shell monkey -p $PKG -c android.intent.category.LAUNCHER 1 >/dev/null
sleep 15; A shell am force-stop $PKG

say "cores"
if ls "$HERE"/out/cores/*.so >/dev/null 2>&1; then
  A shell mkdir -p /data/local/tmp/racores
  for f in "$HERE"/out/cores/*.so; do
    A push "$f" /data/local/tmp/racores/ >/dev/null
    b=$(basename "$f")
    A shell "chmod 644 /data/local/tmp/racores/$b; run-as $PKG cp /data/local/tmp/racores/$b $DATA/cores/; rm /data/local/tmp/racores/$b"
    echo "   $b"
  done
else echo "no cores in out/cores (use: bash build.sh fetch-cores snes9x fbneo mame2003 ...) or use Online Updater on the TV"; fi

say "Xbox One controller profile"
A push "$HERE/config/Xbox One Wireless Controller (Fire TV).cfg" /data/local/tmp/xbox.cfg >/dev/null
A shell "chmod 644 /data/local/tmp/xbox.cfg; run-as $PKG mkdir -p $DATA/autoconfig/android; run-as $PKG cp /data/local/tmp/xbox.cfg '$DATA/autoconfig/android/Xbox One Wireless Controller.cfg'"

say "recommended settings"
A shell "sed -i \
 -e 's/^all_users_control_menu = .*/all_users_control_menu = \"true\"/' \
 -e 's/^input_enable_hotkey_btn = .*/input_enable_hotkey_btn = \"109\"/' \
 -e 's/^input_close_content_btn = .*/input_close_content_btn = \"108\"/' \
 -e 's/^input_hotkey_block_delay = .*/input_hotkey_block_delay = \"10\"/' \
 -e 's/^menu_swap_ok_cancel_buttons = .*/menu_swap_ok_cancel_buttons = \"false\"/' $CFG"
echo "done. Start 'RetroArch NAS' on the TV."
