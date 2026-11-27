#!/bin/bash
# make_apk.sh BUILD_DIR SDL_LIB_DIR: the APK from the Android build's
# libmain.so and SDL3's libSDL3.so (arm64-v8a). The game files are not in
# the APK: push your dump to the app's files folder once, e.g.
#   adb push "Burnout 3 Takedown/." /sdcard/Android/data/local.burnout3recomp/files/game_files/
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD=$1 SDL=$2
mkdir -p "$HERE/app/libs/arm64-v8a"
cp "$BUILD/src/game/libmain.so" "$SDL/libSDL3.so" "$HERE/app/libs/arm64-v8a/"
[ -f "$HERE/local.properties" ] || echo "sdk.dir=${ANDROID_HOME:-$HOME/Library/Android/sdk}" > "$HERE/local.properties"
cd "$HERE"
JAVA_HOME="${JAVA_HOME:-$(/usr/libexec/java_home 2>/dev/null || true)}" ./gradlew --quiet assembleDebug
ls -la app/build/outputs/apk/debug/*.apk
