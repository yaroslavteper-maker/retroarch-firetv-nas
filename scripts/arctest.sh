#!/system/bin/sh
# Try each "core|path" line from $1 in RetroArch NAS; append "RESULT|core|path" to $2.
LOG=/storage/emulated/0/RetroArch/logs/retroarch.log
while IFS='|' read -r core path; do
  [ -z "$core" ] && continue
  : > $LOG
  sh /data/local/tmp/launch.sh "$core" "$path" >/dev/null 2>&1
  r=TIMEOUT; i=0
  while [ $i -lt 240 ]; do
    sleep 1; i=$((i+1))
    if grep -aq -e 'Failed to load content' -e 'readroms failed' -e 'Not found on network share' $LOG; then r=FAIL; break; fi
    if grep -aq -e '\[Core\] Geometry' $LOG; then sleep 3; if grep -aq 'Failed to load content' $LOG; then r=FAIL; else r=OK; fi; break; fi
  done
  echo "$r|$core|$path" >> "$2"
done < "$1"
echo DONE >> "$2"
am force-stop com.retroarch.nas
