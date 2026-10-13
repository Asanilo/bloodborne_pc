#!/usr/bin/env bash
# Integration alone owns approval, GPU lock, launch ledger and outer watchdog.
set -euo pipefail
[[ $# == 2 ]] || { echo 'Usage: run_game_native_async.sh SESSION localnoop|resident|skipEvaluate' >&2; exit 2; }
SESSION=$1 MODE=$2
ROLLING=${BB_NR_NATIVE_ASYNC_ROLLING-0}
[[ $ROLLING == 0 || $ROLLING == 1 ]] || { echo "NATIVE_ASYNC_LAUNCH BLOCKED rolling must be exact0|1" >&2; exit 2; }
SCENE_FIRST=${BB_NR_NATIVE_ASYNC_SCENE_FIRST-0}
[[ $SCENE_FIRST == 0 || $SCENE_FIRST == 1 ]] || { echo "NATIVE_ASYNC_LAUNCH BLOCKED scene-first must be exact0|1" >&2; exit 2; }
[[ $SESSION =~ ^[A-Za-z0-9][A-Za-z0-9-]{0,62}$ && ( $MODE == localnoop || $MODE == resident || $MODE == skipEvaluate ) ]] || exit 2
WT=$(cd -- "$(dirname -- "$0")/../.." && pwd -P)
R=/home/arinp/Code/nrbridge-design/game-poc-20261011
RUN="$R/bbport/runs/native-async-game-$SESSION"
PROFILE="$RUN/profile"
GAME="$R/playtest/assets/CUSA03173"
BUILD=$(printenv BB_NATIVE_ASYNC_BUILD_DIR || true)
[[ -n $BUILD ]] || BUILD="$WT/out"
GE=/home/arinp/.local/share/Steam/compatibilitytools.d/GE-Proton11-7-x86_64
HELPER="$R/opti-highfps-worktree/poc/game_nr/async-origin/out/wine_origin_nr.exe"
[[ -f "$PROFILE/.native-async-profile" && $(cat "$PROFILE/.native-async-profile") == "$SESSION" ]] || {
  echo 'NATIVE_ASYNC_LAUNCH BLOCKED private seed marker missing/mismatched' >&2; exit 3;
}
for P in "$BUILD/bb-probe" "$BUILD/gpu/libbbgpu.so" "$BUILD/libbbport_dlss.so" "$GAME/eboot.bin"; do
  [[ -f $P ]] || { echo "NATIVE_ASYNC_LAUNCH BLOCKED missing $P" >&2; exit 3; }
done
[[ $MODE == localnoop || -f $HELPER ]] || exit 3
# Fresh case required. Integration creates arm only after actual Clinic + READY.
[[ ! -e "$RUN/native-scene-arm" && ! -e "$RUN/async-config.json" && ( $SCENE_FIRST == 0 || ! -e "$RUN/native-helper-start" ) ]] || {
  echo 'NATIVE_ASYNC_LAUNCH BLOCKED stale arm/config' >&2; exit 3;
}
DIAG=$(printenv BB_NR_NATIVE_ASYNC_DIAGNOSTIC_ONCE || true)
[[ -n $DIAG ]] || DIAG=0
[[ $DIAG == 0 || ( $DIAG == 1 && $MODE == localnoop ) ]] || exit 2
EDIT_DIAG=$(printenv BB_NR_NATIVE_ASYNC_EDIT_DIAGNOSTIC_ONCE || true)
[[ -n $EDIT_DIAG ]] || EDIT_DIAG=0
[[ $EDIT_DIAG == 0 || ( $EDIT_DIAG == 1 && $MODE == resident ) ]] || exit 2
mkdir -p -m 700 "$RUN/dump" "$PROFILE/logs" "$PROFILE/wine-prefix"
cat > "$PROFILE/bbport.ini" <<'CONFIG'
upscaler=dlss
preset=1
output_res=1920x1080
jitter=1
object_motion=1
sharpen=0
CONFIG
chmod 600 "$PROFILE/bbport.ini"
: > "$RUN/pad.txt"
chmod 600 "$RUN/pad.txt"
unset BB_NR_P0_MODE BB_NR_Q1_MODE BB_NR_C4_MODE BB_NR_C3_MODE BB_NR_C2_MODE BB_NR_C1_MODE BB_NR_C4_BASELINE
unset BB_NR_Q0_CAPTURE BB_NR_C0_CAPTURE BB_Q0_SLOW_WAIT_DIAG BB_Q0_WAIT_STATE_DIAG
unset BB_RENDER_RES BB_OUTPUT_RES BB_AUTO_RENDER_RES BB_PRESET_FILE BB_TOGGLE_FILE BB_TIMEOUT
export BB_GAME_DIR="$GAME" BB_DATA_DIR="$PROFILE" BB_CONFIG="$PROFILE/bbport.ini"
export BB_USER_DIR="$PROFILE/user" BB_GPU_USER_DIR="$PROFILE/user"
export BB_PREBUILT=1 BB_PROBE="$BUILD/bb-probe" BB_UPSCALER=dlss BB_UPSCALE_PRESET=1 BB_DLSS=1 BB_DLSS_AUDIT=1
export BB_JITTER=1 BB_FPS=uncap BB_FPS_LIMIT=90 BB_FRAME_STATS=1 BB_FRAME_LOG="$PROFILE/logs/frames.csv"
export BB_SAVE_LOG=1 BB_MODS_ENABLED=0 BB_IME_TEXT=NRTest BB_PAD_FILE="$RUN/pad.txt" BB_DUMP_DIR="$RUN/dump"
export BB_PRESENT_DUMP_TRIGGER="$RUN/present-dump-trigger" BB_PRESENT_DUMP_COUNT=1
export BB_NR_NATIVE_ASYNC_ROLLING="$ROLLING"
export BB_NR_NATIVE_ASYNC_SCENE_FIRST="$SCENE_FIRST"
export BB_NR_NATIVE_ASYNC=1 BB_NR_NATIVE_ASYNC_SESSION="$SESSION" BB_NR_NATIVE_ASYNC_MODE="$MODE"
export BB_NR_NATIVE_ASYNC_RUN_DIR="$RUN" BB_NR_NATIVE_ASYNC_SCENE_ARM="$RUN/native-scene-arm"
export BB_NR_NATIVE_ASYNC_EDIT_DIAGNOSTIC_ONCE="$EDIT_DIAG"
export BB_NR_NATIVE_ASYNC_SOURCE_LIMIT=120 BB_NR_NATIVE_ASYNC_DIAGNOSTIC_ONCE="$DIAG"
export BB_NR_NATIVE_ORIGIN_RUN_DIR="$RUN" BB_NR_NATIVE_ORIGIN_HELPER="$HELPER"
export BB_NR_NATIVE_ORIGIN_WINE="$GE/files/lib/wine/x86_64-unix/wine"
export BB_NR_NATIVE_ORIGIN_PRELOADER="$GE/files/lib/wine/x86_64-unix/wine-preloader"
export BB_NR_NATIVE_ORIGIN_PREFIX="$PROFILE/wine-prefix" BB_NR_NATIVE_ORIGIN_WINEDEBUG='-all,warn+vulkan,err+vulkan'
if [[ $MODE == localnoop ]]; then unset BB_NR_NATIVE_ORIGIN_MODE; else export BB_NR_NATIVE_ORIGIN_MODE="$MODE"; fi
export DLSSNR_DISABLE=1 DLSSNR_AUTO_DOWNLOAD=0
{
  printf 'session=%s\nmode=%s\nrun=%s\nmarker=%s\nbuild=%s\nassets=%s\n' "$SESSION" "$MODE" "$RUN" "$PROFILE/.native-async-profile" "$BUILD" "$GAME"
  printf 'scene_arm=%s\nscene_arm_content=%s\\n\nmax_jobs=4\nsource_limit=120\nouter_seconds=300\n' "$RUN/native-scene-arm" "$SESSION"
  printf 'scene_first=%s\nhelper_start=%s\nhelper_start_content=%s\\n\nhelper_start_scope=resident_or_skip_only_localnoop_unchanged\n' "$SCENE_FIRST" "$RUN/native-helper-start" "$SESSION"
  printf 'rolling=%s\nphysical_packet_cap=7\nlogical_link_cap=4\n' "$ROLLING"
  printf 'BB_FPS=uncap\nBB_FPS_LIMIT=90\njitter=1\nrender=1280x720\noutput=1920x1080\n'
  printf 'localnoop_starts_helper=0\ndiagnostic_once=%s\nphase=UNVERIFIED\nruntime=NOT_RUN_AT_BUILD\n' "$DIAG"
  printf 'edit_diagnostic_once=%s\nedit_diagnostic_performance_excluded=1\n' "$EDIT_DIAG"
  env | sort | sed -n '/^BB_NR_NATIVE_/p'
} > "$RUN/native-launch.env"
chmod 600 "$RUN/native-launch.env"
printf 'NATIVE_ASYNC_LAUNCH_READY session=%s mode=%s arm=%s helper_starts_localnoop=0\n' "$SESSION" "$MODE" "$RUN/native-scene-arm"
exec bash "$WT/run.sh"
