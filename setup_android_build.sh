#!/bin/bash
# One-time setup for building RetroArch (Android, armeabi-v7a) on this Mac.
# Installs: Rosetta 2, JDK 11 + 17, Android SDK 30, build-tools 30.0.3, NDK 22.0.7026061.
# Downloads: RetroArch v1.22.2 and libsmb2 6.3.2 sources into ./build next to this script.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="$HOME/Library/Android/sdk"
SRC="$HERE/build"
say(){ printf '\n\033[1m== %s ==\033[0m\n' "$*"; }

say "Rosetta 2 (NDK 22 tools are Intel binaries)"
if arch -x86_64 /usr/bin/true 2>/dev/null; then echo "already installed"
else softwareupdate --install-rosetta --agree-to-license; fi

say "Java 11 (for Gradle) and Java 17 (for sdkmanager)"
for v in 11 17; do
  if /usr/libexec/java_home -v $v >/dev/null 2>&1; then echo "JDK $v present"
  else brew install --cask temurin@$v || brew install --cask zulu@$v; fi
done
J11="$(/usr/libexec/java_home -v 11)"; J17="$(/usr/libexec/java_home -v 17)"

say "Android command-line tools"
brew list --cask android-commandlinetools >/dev/null 2>&1 || brew install --cask android-commandlinetools
SDKM="$(brew --prefix)/bin/sdkmanager"

say "Android SDK packages (several GB; takes a while)"
mkdir -p "$SDK"
yes | JAVA_HOME="$J17" "$SDKM" --sdk_root="$SDK" --licenses >/dev/null || true
JAVA_HOME="$J17" "$SDKM" --sdk_root="$SDK" "platforms;android-30" "build-tools;30.0.3" "ndk;22.0.7026061"

say "Source code"
mkdir -p "$SRC"; cd "$SRC"
if [ ! -d RetroArch ]; then
  curl -fL https://codeload.github.com/libretro/RetroArch/tar.gz/refs/tags/v1.22.2 | tar xz
  mv RetroArch-1.22.2 RetroArch
fi
if [ ! -d libsmb2 ]; then
  curl -fL https://codeload.github.com/sahlberg/libsmb2/tar.gz/refs/tags/libsmb2-6.3.2 | tar xz
  mv libsmb2-libsmb2-6.3.2 libsmb2
fi

cat > "$HERE/build.env" <<ENV
export JAVA_HOME="$J11"
export ANDROID_HOME="$SDK"
export ANDROID_SDK_ROOT="$SDK"
ENV
say "Done. Next: bash build.sh"
