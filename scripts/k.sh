#!/bin/bash
# usage: k.sh DOWN RIGHT CENTER ...  then prints focused items
cd "$(dirname "$0")"; for k in "$@"; do python3 tv.py shell input keyevent KEYCODE_$( [[ $k =~ ^(UP|DOWN|LEFT|RIGHT|CENTER)$ ]] && echo DPAD_$k || echo $k ) >/dev/null; sleep 0.6; done; sleep 1; python3 ui.py | grep '>>'
