#!/system/bin/sh
# usage: sh launch.sh <core_name> <rom path>
CORE="/data/user/0/com.retroarch.nas/cores/$1_libretro_android.so"
CFG=/storage/emulated/0/Android/data/com.retroarch.nas/files/retroarch.cfg
am force-stop com.retroarch.nas
sleep 1
am start -n com.retroarch.nas/com.retroarch.browser.retroactivity.RetroActivityFuture \
  -e ROM "$2" -e LIBRETRO "$CORE" -e CONFIGFILE "$CFG" \
  -e IME com.android.inputmethod.latin/.LatinIME -e DATADIR /data/user/0/com.retroarch.nas \
  -e APK /data/app/com.retroarch.nas/base.apk -e SDCARD /storage/emulated/0 \
  -e EXTERNAL /storage/emulated/0/Android/data/com.retroarch.nas/files
