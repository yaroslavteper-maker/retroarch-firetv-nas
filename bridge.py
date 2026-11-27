#!/usr/bin/env python3
"""Fire TV bridge: runs adb commands dropped as JSON jobs into ./inbox, writes results to ./outbox.
Runs adb against one Fire TV, plus ./build.sh in this folder for the RetroArch build.

    python3 bridge.py <fire-tv-ip>      # first time; the address is remembered in firetv.conf
    python3 bridge.py                   # later runs
Stop with Ctrl+C."""
import json, os, shutil, subprocess, sys, time, pathlib

HERE = pathlib.Path(__file__).resolve().parent
CONF = HERE / "firetv.conf"
if len(sys.argv) > 1:
    TV = sys.argv[1]
elif os.environ.get("FIRETV_ADDR"):
    TV = os.environ["FIRETV_ADDR"]
elif CONF.exists():
    TV = CONF.read_text().strip()
else:
    sys.exit("Usage: python3 bridge.py <fire-tv-ip>\n"
             "Find the IP on the TV under Settings > My Fire TV > About > Network.")
if ":" not in TV: TV += ":5555"
CONF.write_text(TV + "\n")
INBOX, OUTBOX = HERE / "inbox", HERE / "outbox"
INBOX.mkdir(exist_ok=True); OUTBOX.mkdir(exist_ok=True)

ADB = shutil.which("adb")
for p in ["/opt/homebrew/bin/adb", "/usr/local/bin/adb",
          os.path.expanduser("~/Library/Android/sdk/platform-tools/adb")]:
    if not ADB and os.path.exists(p): ADB = p
if not ADB:
    sys.exit("adb not found. Install it with:  brew install android-platform-tools")

def adb(args, out=None, timeout=30):
    cmd = [ADB] + args
    if out:
        with open(out, "wb") as f:
            r = subprocess.run(cmd, stdout=f, stderr=subprocess.PIPE, timeout=timeout)
        return r.returncode, "", r.stderr.decode(errors="replace")
    r = subprocess.run(cmd, capture_output=True, timeout=timeout)
    return r.returncode, r.stdout.decode(errors="replace"), r.stderr.decode(errors="replace")

print(f"adb: {ADB}")
try:
    print(adb(["connect", TV], timeout=10)[1].strip() or "connect sent")
except Exception:
    print(f"Could not reach {TV} yet (TV asleep, IP changed, or ADB debugging off). Will retry on each command.")
print("If the TV shows 'Allow USB debugging?', choose Always allow + OK.")
print("Bridge running. Leave this window open; Ctrl+C to stop.")

while True:
    for f in sorted(INBOX.glob("*.json")):
        try:
            job = json.loads(f.read_text())
            jid = f.stem
            res = {"id": jid}
            try:
                if job.get("build"):
                    # Runs ./build.sh in this folder and logs to outbox/<id>.log
                    log = OUTBOX / f"{jid}.log"
                    with open(log, "wb") as lf:
                        r = subprocess.run(["bash", str(HERE / "build.sh")] + list(job.get("args", [])),
                                           stdout=lf, stderr=subprocess.STDOUT, cwd=HERE, timeout=4 * 3600)
                    rc, so, se = r.returncode, "", ""
                    res["log"] = log.name
                elif job.get("install"):
                    rc, so, se = adb(["-s", TV, "install", "-r", str(HERE / job["install"])], timeout=600)
                elif job.get("connect"):
                    rc, so, se = adb(["connect", TV])
                elif job.get("screencap"):
                    png = OUTBOX / f"{jid}.png"
                    rc, so, se = adb(["-s", TV, "exec-out", "screencap", "-p"], out=png)
                    res["png"] = png.name
                else:
                    try: adb(["connect", TV], timeout=8)
                    except Exception: pass
                    rc, so, se = adb(["-s", TV] + list(job["args"]), timeout=job.get("timeout", 30))
                res.update(rc=rc, stdout=so[-20000:], stderr=se[-5000:])
            except Exception as e:
                res.update(rc=-1, error=str(e))
            (OUTBOX / f"{jid}.tmp").write_text(json.dumps(res))
            os.replace(OUTBOX / f"{jid}.tmp", OUTBOX / f"{jid}.out")
            print(time.strftime("%H:%M:%S"), job, "->", res.get("rc"))
        except Exception as e:
            print("bad job", f.name, e)
        finally:
            try: f.unlink()
            except FileNotFoundError: pass
    time.sleep(0.25)
