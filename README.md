# RetroArch NAS for Fire TV

> **Unofficial build — not affiliated with or endorsed by libretro / the RetroArch team.**
> Based on [RetroArch](https://github.com/libretro/RetroArch) 1.22.2 (GPL‑3.0). Report problems here, not to libretro.

**RetroArch 1.22.2 for Amazon Fire TV that plays games straight from a network share** —
a Raspberry Pi NAS, a Synology/QNAP, a Windows shared folder, anything that speaks SMB.

Stock RetroArch on Android can only load games from local storage, and Fire OS can't mount network
shares. This build adds an `smb://` scheme to RetroArch's virtual file system, so the file browser,
playlists and the content loader all accept paths like

```
smb://<server>/<share>/roms/snes/Super Mario World (USA).7z#Super Mario World (USA).sfc
```

It installs as its own app, **RetroArch NAS** (`com.retroarch.nas`), next to any other RetroArch.

---

## Quick start

You need: a Mac (Apple Silicon or Intel), a Fire TV running **Fire OS** on the same network as your
share, and the share's address and login.

### 1. Build the app (once, ~30 min mostly downloads)

```sh
git clone https://github.com/<you>/retroarch-firetv-nas.git
cd retroarch-firetv-nas
bash setup_android_build.sh      # Rosetta, Android SDK/NDK, RetroArch + libsmb2 sources
bash build.sh                    # -> out/RetroArch-NAS.apk  (~3-4 min)
```

### 2. Get the cores you want

```sh
bash build.sh fetch-cores stella2023 a5200 prosystem gearcoleco fceumm snes9x \
     genesis_plus_gx mednafen_pce_fast pcsx_rearmed fbneo mame2003 mame2003_plus mame2010
```

(Any armeabi‑v7a core on the libretro buildbot works; they land in `out/cores/`.
You can also skip this and use *Online Updater → Core Downloader* on the TV.)

### 3. Your share login

```sh
cp config/smb.cfg.example smb.cfg      # then edit user / password
```

Not sure of the share name? From the Mac: `smbutil view //<server-ip>` lists the shares.

### 4. Put it on the Fire TV

On the TV: **Settings → My Fire TV → About** (click the device name 7×), then
**Developer Options → ADB debugging: On**, and note the IP under **About → Network**.

```sh
brew install android-platform-tools     # if adb isn't installed
bash firetv_setup.sh <fire-tv-ip>
```

Accept *“Allow USB debugging?”* on the TV. The script installs the app, your login, the cores, a
fixed Xbox One controller profile and sensible controller settings.

### 5. Point RetroArch at your games

Either browse to them on the TV — set **Settings → Directory → File Browser** to
`smb://<server>/<share>/<folder>` and use *Load Content* — or reuse existing playlists:

```sh
adb pull /storage/emulated/0/RetroArch/playlists old_playlists
python3 tools/retarget_playlists.py old_playlists new_playlists \
    --from /storage/1234-ABCD/roms --to "smb://<server>/<share>/roms" \
    --core "Nintendo - Super Nintendo Entertainment System.lpl=snes9x"
adb push new_playlists/. /storage/emulated/0/RetroArch/playlists/
```

`--from` is whatever local/USB path your old playlists used (Windows `F:\Games` paths work too).

---

## How it works

| Piece | What it does |
|---|---|
| `libretro-common/vfs/vfs_implementation_smb.c` | Read‑only SMB2/3 backend on [libsmb2](https://github.com/sahlberg/libsmb2): connection pool with auto‑reconnect, read‑ahead buffering, directory listing, stat. |
| `libretro-common/vfs/vfs_implementation.c` | Routes `smb://` paths to that backend (modelled on the existing `saf://` scheme). |
| `tasks/task_content.c` | Before a game loads, copies it from the share into a local cache (`RetroArch/temp/smb_cache`) and gives the core the local path, so **every core works**, including ones that open files themselves (MAME, PCSX ReARMed, Flycast…). Cached copies are reused while size + timestamp match; the oldest are evicted past `cache_limit_mb`. |
| Companion files | `.cue`, `.gdi`, `.m3u`, `.ccd` bring the track/disc files they reference. Arcade sets bring their parent/BIOS sets listed in `smb_deps.txt`. |
| `pkg/android/…` | Builds libsmb2 into the APK and adds the `nas` flavour (`.nas` package suffix, armeabi‑v7a, debuggable so cores can be copied in with `adb shell run-as`). |

Saves, save states and settings stay on the TV; nothing is ever written to the share.
The complete change against upstream v1.22.2 is [`retroarch-1.22.2-smb.patch`](retroarch-1.22.2-smb.patch);
the same files are laid out under `patch/RetroArch/` for `build.sh`.

## Configuration (`/storage/emulated/0/RetroArch/smb.cfg` on the TV)

```ini
user           = "pi"
password       = "secret"
domain         = "WORKGROUP"   # optional
cache_limit_mb = 1536          # use ~500 on 8 GB Fire TV Sticks
deps_file      = "/storage/emulated/0/RetroArch/smb_deps.txt"   # optional, this is the default
```

A user name in the URL (`smb://alice@server/share/...`) overrides `user`.

## Arcade games

Arcade ROM sets only work with the MAME/FBNeo version they were made for, and many need other
sets from the same folder (a parent, or a BIOS like `neogeo.zip`).

1. On the TV, list every zip's contents:
   `adb shell /data/local/tmp/smbls -zip "smb://<server>/<share>/Arcade" > arczip.txt`
   (build the tool with `bash build.sh smbls` and `adb push out/smbls /data/local/tmp/`).
2. Download the DATs (`mame2003.xml`, `mame2003-plus.xml`, `mame2010.xml`, FBNeo arcade DAT) from
   the libretro repos and run `tools/arcade/analyze_arcade_sets.py` — it picks, per game, a core
   whose ROM list is fully present and records the parent/BIOS sets each game needs.
3. Turn those dependencies into `smb_deps.txt` (format in `config/smb_deps.example.txt`) and push it.

`scripts/arctest.sh` can then launch every game on the TV and report which ones boot.

## Xbox One controller on Fire TV

Fire TV has its own key layout for the Xbox One S Bluetooth pad (and the many third‑party pads that
report the same VID 045e / PID 02e0), so RetroArch's stock profile for that ID is scrambled there:
View/Menu do nothing, the bumpers act as Select/Start, triggers and right stick are swapped.
`config/Xbox One Wireless Controller (Fire TV).cfg` fixes that (`firetv_setup.sh` installs it).
*Online Updater → Update Controller Profiles* will put the broken one back.

Settings applied by `firetv_setup.sh` for arcade play: **View = Select/coin**, **Menu = Start**,
**hold View + press Menu = close the game**; B confirms and A goes back in menus; every controller
(including the Fire TV remote) can drive the menu.

## Helper scripts

| File | Where | Purpose |
|---|---|---|
| `setup_android_build.sh`, `build.sh` | Mac | Toolchain setup; build APK, `smbls`, or `fetch-cores …`. |
| `firetv_setup.sh <ip>` | Mac | Install and configure everything on a Fire TV over adb. |
| `tools/retarget_playlists.py` | any | Point existing playlists at the share; set default cores; drop missing games. |
| `tools/arcade/analyze_arcade_sets.py` | any | Match arcade zips to cores using DATs. |
| `patch/tools/smbls.c` | TV | `smbls <dir>`, `-r` recursive, `-c` check paths from stdin, `-cat` file, `-zip` list zip CRCs. |
| `scripts/launch.sh` | TV | Start a game with a given core: `sh launch.sh snes9x "smb://…"`. |
| `scripts/arctest.sh` | TV | Launch every `core|path` in a list and record OK / FAIL. |
| `bridge.py`, `tv.py` | Mac | Optional job queue so another machine (or an AI assistant) can drive `adb` and `build.sh` on the Mac through a shared folder: `python3 bridge.py <fire-tv-ip>`. |
| `tools/test/smbtest.c` | Linux/Mac | Host test of the SMB backend against a local SMB server (e.g. impacket's `smbserver`). |

## Compatibility and limits

- **Fire OS devices** (Fire TV Cube, Fire TV Stick 4K / 4K Max / Lite, …). Newer devices on Amazon's
  **Vega OS** can't run Android apps at all.
- 32‑bit (armeabi‑v7a) build, because Fire OS on these devices runs 32‑bit apps. That also means no
  Dolphin (GameCube/Wii) or PS2 — those emulators are 64‑bit only.
- Read‑only: the share is never written to.
- The first launch of a big game (e.g. a 500 MB PlayStation image) copies it first and the screen
  freezes until the copy finishes (~5–10 MB/s from a Pi). Later launches use the cache.

## Licences

RetroArch is GPL‑3.0 (`LICENSE`). The new libretro‑common files use libretro‑common's permissive
licence header. libsmb2 (LGPL‑2.1) is downloaded at build time and not stored in this repository.
