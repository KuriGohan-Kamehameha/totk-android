#!/bin/bash
# Build one app flavour from this checkout: the one entry point for both apps (MAINTENANCE.md).
#
#   tools/build_app.sh <game> <flavour> [build_release.py options, e.g. --version-name 1.0.1-fork.3]
#     game:    totk | botw                     (games/games.toml)
#     flavour: totk: fork | dev; botw: release | dev
#
# Package, launcher label, version, signing key and launcher art come from games/games.toml; the keys stay in
# TOTK_KEYS_DIR (default /root/totk-work/keys) and never enter git. The defaults below are the build machine's (CT 923);
# override them through the environment:
#   NDK, ANDROID_HOME, PREBUILT (libtotk_turnip.so, libtotk_xlate.so), VENV (Python 3.12+ with cryptography),
#   WORK (work folder, default /dev/shm/app-<game>-<flavour>), JOBS (default 4), DUO_JOBS (default 2: libduo_host.so
#   compiles take about 1 GB each).
# The last line of output is BUILD_EXIT=<status>; the APK is <WORK>/<label>-<version>.apk.
set -u
if [ $# -lt 2 ]; then sed -n '2,15p' "$0"; exit 2; fi
GAME=$1 FLAVOUR=$2; shift 2
REPO=$(cd "$(dirname "$0")/.." && pwd)
export ANDROID_HOME=${ANDROID_HOME:-/opt/android-sdk} LC_ALL=${LC_ALL:-C.UTF-8}
export ANDROID_SDK_ROOT=$ANDROID_HOME DUO_JOBS=${DUO_JOBS:-2}
export PATH=$ANDROID_HOME/cmake/3.22.1/bin:/root/.local/bin:$PATH
NDK=${NDK:-$ANDROID_HOME/ndk/28.2.13676358}
PREBUILT=${PREBUILT:-/root/totk-work/prebuilt}
VENV=${VENV:-/dev/shm/venv}
WORK=${WORK:-/dev/shm/app-$GAME-$FLAVOUR}
if [ ! -x "$VENV/bin/python" ]; then
  uv venv -q --python 3.13 "$VENV" && uv pip install -q --python "$VENV/bin/python" cryptography==50.0.2 || { echo BUILD_EXIT=4; exit 4; }
fi
cd "$REPO" || { echo BUILD_EXIT=3; exit 3; }
echo "source: $(git log --oneline -1) ($REPO)"
nice "$VENV/bin/python" games/totk/android/build_release.py --game "$GAME" --flavour "$FLAVOUR" \
  --ndk "$NDK" --android-sdk "$ANDROID_HOME" \
  --turnip "$PREBUILT/libtotk_turnip.so" --translator "$PREBUILT/libtotk_xlate.so" \
  --work "$WORK" --jobs "${JOBS:-4}" "$@"
rc=$?
echo BUILD_EXIT=$rc
exit $rc
