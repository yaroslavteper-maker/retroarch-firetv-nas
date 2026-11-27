#!/usr/bin/env python3
# Client for bridge.py.
#   tv.py shell input keyevent KEYCODE_HOME   -> any adb args (run against the TV)
#   tv.py --screencap | --connect
#   tv.py --install path/rel/to/firetv.apk
#   tv.py --build [args]   -> submit build, print job id, don't wait
#   tv.py --wait <id> [seconds]
import json, sys, time, uuid, pathlib
H = pathlib.Path(__file__).resolve().parent
(H/"inbox").mkdir(exist_ok=True)
a = sys.argv[1:]

def wait(jid, secs=60):
    out = H/"outbox"/f"{jid}.out"
    for _ in range(int(secs*4)):
        if out.exists(): print(out.read_text()); return 0
        time.sleep(0.25)
    print(json.dumps({"id": jid, "pending": True, "error": "no response yet - is bridge.py running?"})); return 1

if a[:1] == ["--wait"]:
    sys.exit(wait(a[1], float(a[2]) if len(a) > 2 else 60))
jid = time.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:6]
if a == ["--screencap"]: job = {"screencap": True}
elif a == ["--connect"]: job = {"connect": True}
elif a[:1] == ["--install"]: job = {"install": a[1]}
elif a[:1] == ["--build"]: job = {"build": True, "args": a[1:]}
else: job = {"args": a}
(H/"inbox"/f"{jid}.tmp").write_text(json.dumps(job)); (H/"inbox"/f"{jid}.tmp").rename(H/"inbox"/f"{jid}.json")
if job.get("build"): print(jid); sys.exit(0)
sys.exit(wait(jid, 600 if job.get("install") else 60))
