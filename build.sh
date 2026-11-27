#!/bin/bash
# Builds "RetroArch NAS" (com.retroarch.nas, armeabi-v7a) from build/RetroArch
# with the files in patch/RetroArch laid over it. Output: out/RetroArch-NAS.apk
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/build.env"

# Gradle 6.7.1 (used by RetroArch 1.22) needs JDK 8-15, and NDK 22 only runs on
# Intel, so the whole build runs under Rosetta with a private Intel JDK 11.
JDK11="$HERE/jdk11-x64"
if [ ! -x "$JDK11/Contents/Home/bin/java" ]; then
  echo "-- downloading JDK 11 (Temurin, x64) into $JDK11"
  ARCH=x64
  rm -rf "$JDK11.tmp"; mkdir -p "$JDK11.tmp"
  curl -fsSL "https://api.adoptium.net/v3/binary/latest/11/ga/mac/$ARCH/jdk/hotspot/normal/eclipse" \
    | tar xz -C "$JDK11.tmp"
  mv "$JDK11.tmp"/*/ "$JDK11"; rm -rf "$JDK11.tmp"
fi
export JAVA_HOME="$JDK11/Contents/Home"
"$JAVA_HOME/bin/java" -version 2>&1 | head -1
[ -d "$ANDROID_HOME/ndk/22.0.7026061" ] || { echo "NDK 22.0.7026061 missing in $ANDROID_HOME"; ls "$ANDROID_HOME" "$ANDROID_HOME/ndk" 2>&1; exit 3; }
SRC="$HERE/build/RetroArch"
echo "== $(date) =="

# "build.sh smbls" builds the smbls test tool for the TV into out/smbls
if [ "${1:-}" = "smbls" ]; then
  NDK="$ANDROID_HOME/ndk/22.0.7026061"
  CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/armv7a-linux-androideabi21-clang"
  P="$HERE/patch/RetroArch"; L="$HERE/build/libsmb2"
  mkdir -p "$HERE/out"
  arch -x86_64 "$CC" -O2 -w -std=gnu99 -DHAVE_SMB -DHAVE_CONFIG_H -D_U_="__attribute__((unused))" \
    -I"$P/deps/libsmb2/android" -I"$L/include" -I"$L/include/smb2" -I"$L/lib" \
    -I"$SRC/libretro-common/include" -I"$P/libretro-common/include" \
    "$HERE/patch/tools/smbls.c" "$P/libretro-common/vfs/vfs_implementation_smb.c" \
    "$SRC/libretro-common/compat/compat_strl.c" \
    $(for f in aes.c aes128ccm.c alloc.c asn1-ber.c compat.c dcerpc-lsa.c dcerpc-srvsvc.c dcerpc.c errors.c hmac-md5.c hmac.c init.c krb5-wrapper.c libsmb2.c md4c.c md5.c ntlmssp.c pdu.c sha1.c sha224-256.c sha384-512.c smb2-cmd-close.c smb2-cmd-create.c smb2-cmd-echo.c smb2-cmd-error.c smb2-cmd-flush.c smb2-cmd-ioctl.c smb2-cmd-lock.c smb2-cmd-logoff.c smb2-cmd-negotiate.c smb2-cmd-notify-change.c smb2-cmd-oplock-break.c smb2-cmd-query-directory.c smb2-cmd-query-info.c smb2-cmd-read.c smb2-cmd-session-setup.c smb2-cmd-set-info.c smb2-cmd-tree-connect.c smb2-cmd-tree-disconnect.c smb2-cmd-write.c smb2-data-file-info.c smb2-data-filesystem-info.c smb2-data-reparse-point.c smb2-data-security-descriptor.c smb2-share-enum.c smb2-signing.c smb3-seal.c socket.c spnego-wrapper.c sync.c timestamps.c unicode.c usha.c; do echo "$L/lib/$f"; done) \
    -llog -o "$HERE/out/smbls" && ls -la "$HERE/out/smbls" && echo "SMBLS OK"
  exit 0
fi

# "build.sh fetch-cores name1 name2 ..." downloads armeabi-v7a cores from the
# libretro buildbot into out/cores (no build).
if [ "${1:-}" = "fetch-cores" ]; then
  shift; mkdir -p "$HERE/out/cores"; cd "$HERE/out/cores"
  for c in "$@"; do
    f="${c}_libretro_android.so"
    echo "-- $f"
    curl -fsSL -o "$f.zip" "https://buildbot.libretro.com/nightly/android/latest/armeabi-v7a/$f.zip" \
      && unzip -o -q "$f.zip" && rm -f "$f.zip" && ls -la "$f" || echo "   FAILED: $c"
  done
  echo "FETCH DONE"; exit 0
fi
[ -d "$SRC" ] || { echo "Source missing: run setup_android_build.sh first"; exit 2; }

echo "-- applying patch overlay"
( cd "$HERE/patch/RetroArch" && find . -type f -print | while read -r f; do
    mkdir -p "$SRC/$(dirname "$f")"; cp -p "$f" "$SRC/$f"; echo "   $f"; done )

echo "-- libsmb2 into deps"
mkdir -p "$SRC/deps/libsmb2"
cp -R "$HERE/build/libsmb2/include" "$HERE/build/libsmb2/lib" "$SRC/deps/libsmb2/"

cd "$SRC/pkg/android/phoenix"
chmod +x gradlew
echo "-- gradle assembleNasRelease (JAVA_HOME=$JAVA_HOME)"
arch -x86_64 /bin/bash ./gradlew --no-daemon --console=plain assembleNasRelease "$@"

mkdir -p "$HERE/out"
APK="$(find build/outputs/apk/nas/release -name '*.apk' | head -1)"
cp "$APK" "$HERE/out/RetroArch-NAS.apk"
ls -la "$HERE/out/RetroArch-NAS.apk"
echo "BUILD OK"
