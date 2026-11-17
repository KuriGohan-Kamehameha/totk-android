#!/bin/bash
# One test command for both games (MAINTENANCE.md):
#   1. the host test suite (python -m unittest discover -s tests), once: it covers what differs between the games itself
#      (tests/test_game_variables.py, both companion screens, both audits, ...);
#   2. for each game in games/games.toml: the native runtime with that game's defines (make -C games/totk/android
#      VERSION=<tag>, headless executable, Android NDK) and the app's Java with that game's GameBuild (apk/build_dex.py
#      --game), so code compiled out for one game is still compiled for the other.
#
#   tools/check_all.sh [game ...]          (default: every game)
# Environment: NDK (default /opt/android-sdk/ndk/28.2.13676358), ANDROID_HOME (/opt/android-sdk), OPUS (libopus prefix
# for the target, default /dev/shm/totk-libs/opus-prefix), OUT (scratch, default /dev/shm/check-all), PYTHON (python3),
# JOBS (4), SKIP_UNITTEST=1, SKIP_NATIVE=1.
# Prints one line per step and game, then CHECK_ALL_EXIT=<number of failed steps>. Logs: $OUT/<step>-<game>.log.
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
export LC_ALL=${LC_ALL:-C.UTF-8} ANDROID_HOME=${ANDROID_HOME:-/opt/android-sdk}
export ANDROID_SDK_ROOT=$ANDROID_HOME
NDK=${NDK:-$ANDROID_HOME/ndk/28.2.13676358}
OPUS=${OPUS:-/dev/shm/totk-libs/opus-prefix}
OUT=${OUT:-/dev/shm/check-all}
PYTHON=${PYTHON:-python3}
mkdir -p "$OUT"
cd "$REPO" || exit 3
GAMES=${*:-$($PYTHON tools/games.py list | cut -d' ' -f1)}
failed=0
step() {   # step <name> <game> <command...>
  local name=$1 game=$2; shift 2
  if "$@" > "$OUT/$name-$game.log" 2>&1; then echo "PASS $name $game"
  else echo "FAIL $name $game (exit $?, $OUT/$name-$game.log)"; failed=$((failed + 1)); fi
}
if [ "${SKIP_UNITTEST:-0}" != 1 ]; then
  step unittest all $PYTHON -m unittest discover -s tests
  tail -3 "$OUT/unittest-all.log" | sed 's/^/  /'
fi
for g in $GAMES; do
  tag=$($PYTHON tools/games.py get "$g" nso_version) || { echo "FAIL unknown game $g"; failed=$((failed + 1)); continue; }
  if [ "${SKIP_NATIVE:-0}" != 1 ]; then
    step native "$g" make -C games/totk/android TARGET=android NDK="$NDK" VERSION="$tag" OUT="$OUT/native-$tag" \
      OPUS="$OPUS" PYTHON="$PYTHON" -j"${JOBS:-4}" all
  fi
  step java "$g" $PYTHON games/totk/android/apk/build_dex.py --game "$g" --android-sdk "$ANDROID_HOME" --out "$OUT/classes-$g.dex"
done
echo CHECK_ALL_EXIT=$failed
exit $failed
