"""Pick a working libretro arcade core for every set in a folder.

Compares the CRCs inside each .zip (listed with `smbls -zip <smb://folder>` on the TV)
against the ROM lists (DATs) of MAME 2003, MAME 2003-Plus, MAME 2010 and FinalBurn Neo,
preferring the core the PC playlists used. Writes arcade_resolved.json with the chosen core
and the parent/BIOS sets each game needs (used to build smb_deps.txt).

Inputs in the working folder:
  arczip.txt         output of `smbls -zip`
  arcade_map.json    PC playlist entries keyed by zip name -> [(core, label, playlist)]
  dl_*.xml / *.dat   DATs from the libretro repos (mame2003, mame2003-plus, mame2010, FBNeo)
"""
import re, json, collections, xml.etree.ElementTree as ET
def load(fn):
    db={}
    for ev,el in ET.iterparse(fn, events=("end",)):
        if el.tag in ("game","machine"):
            roms=[]
            for r in el.findall("rom"):
                if r.get("status")=="nodump" or not r.get("crc"): continue
                roms.append((r.get("crc").lower(), r.get("merge"), r.get("name")))
            db[el.get("name")]={"isbios":el.get("isbios")=="yes","cloneof":el.get("cloneof"),"romof":el.get("romof"),"roms":roms,
                "desc":(el.findtext("description") or el.get("name"))}
            el.clear()
    return db
dats={"mame2003":"dl_mame2003.xml","mame2003_plus":"dl_mame2003-plus.xml","fbneo":"dl_FinalBurn20Neo20(ClrMame20Pro20XML2C20Arcade20only).dat","mame2010":"dl_mame2010.xml"}
DB={k:load(v) for k,v in dats.items()}
print({k:len(v) for k,v in DB.items()})
zips={}; cur=None
for l in open("arczip.txt",encoding="utf-8",errors="replace"):
    l=l.rstrip("\n")
    if l.startswith("== "): cur=l[3:].lower()[:-4]; zips[cur]=set()
    elif cur and len(l)>9: zips[cur].add(l.split(" ",1)[0])
def check(core,g):
    db=DB[core]
    if g not in db: return None
    chain=[]; x=g; seen=set()
    while x and x not in seen:
        seen.add(x); chain.append(x)
        x=db.get(x,{}).get("romof") if x in db else None
    have=set(); deps=[]
    for c in chain:
        if c in zips:
            have|=zips[c]
            if c!=g: deps.append(c)
    bios_crcs=set()
    for c in chain[1:]:
        if c in db and db[c].get("isbios"): bios_crcs|={r[0] for r in db[c]["roms"]}
    biosdeps=[c for c in chain[1:] if c in db and db[c].get("isbios")]
    if biosdeps and not any(b in zips for b in biosdeps): return (0.0,deps,db[g]["desc"])
    roms=[r for r in db[g]["roms"] if r[0] not in bios_crcs]
    if not roms: return (1.0,deps,db[g]["desc"])
    ok=sum(1 for crc,_,_ in roms if crc in have)
    return (ok/len(roms),deps,db[g]["desc"])
pcmap=json.load(open("arcade_map.json"))
import ntpath
def pccore(g):
    v=pcmap.get(g+".zip")
    if not v: return []
    out=[]
    for c,lab,f in v:
        c=ntpath.basename(c).replace("_libretro.dll","") if "\\" in c else c
        out.append(c)
    return out
res={}; stat=collections.Counter()
for g in sorted(zips):
    pcs=pccore(g)
    order=[c for c in pcs]+[c for c in ["mame2003_plus","mame2003","fbneo","mame2010"] if c not in pcs]
    choice=None; detail={}
    for c in order:
        if c in DB:
            r=check(c,g); detail[c]=None if r is None else round(r[0],2)
            if r and r[0]>=0.999: choice=(c,r[1],r[2],"verified"); break
        elif c in pcs and choice is None:
            pass
    if not choice:
        # no DAT-verified core: keep PC choice (unverifiable cores like mame2015/2016/hbmame), else best partial
        unver=[c for c in pcs if c not in DB]
        if unver: choice=(unver[0],[],g,"pc-unverified")
        else:
            best=max(((v,c) for c,v in detail.items() if v is not None), default=None)
            choice=(best[1] if best else "fbneo",[],g,"partial %s"%(best[0] if best else 0))
    res[g]={"core":choice[0],"deps":choice[1],"desc":choice[2],"how":choice[3],"pc":pcs,"detail":detail}
    stat[(choice[3].split()[0])]+=1
print(stat)
chg=[(g,r["pc"],r["core"]) for g,r in res.items() if r["pc"] and r["core"] not in [("mame2016" if c=="mame" else c) for c in r["pc"]]]
print("changed from PC choice:",len(chg)); print(chg[:30])
print("partial:",[(g,r["detail"]) for g,r in res.items() if r["how"].startswith("partial")][:40])
json.dump(res,open("arcade_resolved.json","w"),indent=1)
