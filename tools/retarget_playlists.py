#!/usr/bin/env python3
"""Rewrite RetroArch playlists so their games point at a network share.

    python3 tools/retarget_playlists.py playlists/ out_playlists/ \
        --from /storage/1234-ABCD/roms --to "smb://nas.local/share/roms"

Every "path" that starts with --from gets that prefix replaced with --to (several
--from/--to pairs may be given, matched in order). Optional:
  --core SYSTEM.lpl=core_name   set a playlist's default core, e.g.
                                "Nintendo - Super Nintendo Entertainment System.lpl=snes9x"
  --missing missing.txt         drop entries listed as "MISSING <path>" (output of `smbls -c`)
Paths to cores use the RetroArch NAS app folder on the TV.
"""
import argparse, json, os, re

CORES = "/data/user/0/com.retroarch.nas/cores/"

def base(p):
    m = re.search(r'\.(zip|7z)#', p, re.I)
    return p[:m.end() - 1] if m else p

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("src"); ap.add_argument("dst")
ap.add_argument("--from", dest="frm", action="append", required=True)
ap.add_argument("--to", action="append", required=True)
ap.add_argument("--core", action="append", default=[])
ap.add_argument("--missing")
a = ap.parse_args()
if len(a.frm) != len(a.to):
    ap.error("give one --to for every --from")
cores = dict(c.split("=", 1) for c in a.core)
missing = set()
if a.missing:
    missing = {l[8:].strip() for l in open(a.missing) if l.startswith("MISSING ")}
os.makedirs(a.dst, exist_ok=True)
for f in sorted(os.listdir(a.src)):
    if not f.endswith(".lpl"):
        continue
    d = json.load(open(os.path.join(a.src, f), encoding="utf-8"))
    n_in = len(d.get("items", []))
    keep = []
    for it in d.get("items", []):
        p = it["path"].replace("\\", "/")
        for fr, to in zip(a.frm, a.to):
            fr = fr.replace("\\", "/").rstrip("/")
            if p.lower().startswith(fr.lower()):
                p = to.rstrip("/") + p[len(fr):]
                break
        it["path"] = p
        if base(p) not in missing:
            keep.append(it)
    d["items"] = keep
    if f in cores:
        d["default_core_path"] = CORES + cores[f] + "_libretro_android.so"
        d["default_core_name"] = cores[f]
    json.dump(d, open(os.path.join(a.dst, f), "w", encoding="utf-8"), indent=2, ensure_ascii=False)
    print(f"{f}: {len(keep)}/{n_in}")
