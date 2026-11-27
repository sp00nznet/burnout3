#!/bin/bash
# push_game.sh DUMP_DIR: your Burnout 3 dump into the installed debug app's
# internal storage (files/game_files), where the app can read it. adb push
# alone lands files the app may not open (another owner); run-as copies them
# as the app. Needs a debuggable build (make_apk.sh's) and adb on PATH or
# ANDROID_HOME. Saves and logs stay in Android/data/<package>/files.
set -euo pipefail
DUMP=$1
PKG=local.burnout3recomp
ADB=${ADB:-$(command -v adb || echo "${ANDROID_HOME:-$HOME/Library/Android/sdk}/platform-tools/adb")}
"$ADB" shell rm -rf /data/local/tmp/bo3
"$ADB" push "$DUMP/." /data/local/tmp/bo3/
"$ADB" shell run-as $PKG sh -c "'rm -rf files/game_files && mkdir -p files && cp -r /data/local/tmp/bo3 files/game_files'"
"$ADB" shell rm -rf /data/local/tmp/bo3
echo "game files in $PKG's internal storage"
