#!/bin/bash
# make_app.sh BUILD_DIR GAME_FILES OUT_DIR [iphonesimulator|iphoneos]
#
# Turns the iOS build's burnout3.app into an installable app: Info.plist, the
# game files (your own dump, copied in, links followed), the stock options
# and an ad hoc signature. A device build needs a real signing identity:
# set CODESIGN_IDENTITY (and a provisioning profile in PROFILE).
# The result holds your copy of the game: for your own devices only.
set -euo pipefail
BUILD=$1 GAME=$2 OUT=$3 PLATFORM=${4:-iphonesimulator}
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
ID=${BUNDLE_ID:-local.burnout3-recomp}
VER=$(date +%Y%m%d.%H%M)
case $PLATFORM in iphonesimulator) PL=iPhoneSimulator ;; *) PL=iPhoneOS ;; esac

[ -f "$GAME/default.xbe" ] || { echo "no default.xbe in $GAME" >&2; exit 1; }
mkdir -p "$OUT"
APP="$OUT/Burnout3.app"
rm -rf "$APP"
cp -R "$BUILD/burnout3.app" "$APP"
sed -e "s/@BUNDLE_ID@/$ID/" -e "s/@SHORT_VERSION@/1.0/" -e "s/@VERSION@/$VER/" \
    -e "s/@PLATFORM@/$PL/" "$HERE/Info.plist.in" > "$APP/Info.plist"
cp "$ROOT/packaging/config.default.toml" "$APP/config.default.toml"
# Not the dump's UDATA and TDATA: the bundle is read-only on a device, and
# the kernel keeps saves in a game folder that has them (kernel_path.c), so
# the first save failed (access denied) and the title exited. Saves go to
# Documents/hdd instead.
rsync -aL --delete --exclude /UDATA --exclude /TDATA "$GAME/" "$APP/game_files/"
if [ -n "${PROFILE:-}" ]; then cp "$PROFILE" "$APP/embedded.mobileprovision"; fi
# A device build: ENTITLEMENTS names the plist its profile allows (the
# application-identifier, the team, get-task-allow for a development one).
codesign --force --sign "${CODESIGN_IDENTITY:--}" --timestamp=none \
    ${ENTITLEMENTS:+--entitlements "$ENTITLEMENTS"} "$APP"
echo "$APP"
