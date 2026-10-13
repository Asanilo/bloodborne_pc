#!/usr/bin/env bash
# Integration alone owns approval, GPU lock, launch ledger and outer watchdog.
set -euo pipefail
[[ $# == 2 ]] || { echo 'Usage: run_game_native_pre_sr.sh SESSION resident|skipEvaluate' >&2; exit 2; }
SESSION=$1 MODE=$2
[[ $SESSION =~ ^[A-Za-z0-9][A-Za-z0-9-]{0,62}$ && ( $MODE == resident || $MODE == skipEvaluate ) ]] || exit 2
WT=$(cd -- "$(dirname -- "$0")/../.." && pwd -P)
R=/home/arinp/Code/nrbridge-design/game-poc-20261011
RUN="$R/bbport/runs/native-pre-sr-game-$SESSION"
PROFILE="$RUN/profile"
GAME="$R/playtest/assets/CUSA03173"
BUILD=$(printenv BB_NATIVE_PRE_SR_BUILD_DIR || true)
[[ -n $BUILD ]] || BUILD="$WT/out"
GE=/home/arinp/.local/share/Steam/compatibilitytools.d/GE-Proton11-7-x86_64
HELPER="$R/opti-pre-sr-worktree/poc/game_nr/pre-sr-game/out/wine_presr_nr.exe"
[[ -f "$PROFILE/.native-pre-sr-profile" && $(cat "$PROFILE/.native-pre-sr-profile") == "$SESSION" ]] || {
  echo 'NATIVE_PRE_SR_LAUNCH BLOCKED private seed marker missing/mismatched' >&2; exit 3;
}
for P in "$BUILD/bb-probe" "$BUILD/gpu/libbbgpu.so" "$BUILD/libbbport_dlss.so" "$GAME/eboot.bin"; do
  [[ -f $P ]] || { echo "NATIVE_PRE_SR_LAUNCH BLOCKED missing $P" >&2; exit 3; }
done
[[ -f $HELPER ]] || exit 3
# Fresh case required. Integration creates arm only after actual Clinic + READY.
[[ ! -e "$RUN/native-scene-arm" && ! -e "$RUN/async-config.json" ]] || {
  echo 'NATIVE_PRE_SR_LAUNCH BLOCKED stale arm/config' >&2; exit 3;
}
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
unset BB_NR_NATIVE_ASYNC BB_NR_NATIVE_ASYNC_SESSION BB_NR_NATIVE_ASYNC_MODE
unset BB_NR_NATIVE_ASYNC_DIAGNOSTIC_ONCE BB_NR_NATIVE_ASYNC_EDIT_DIAGNOSTIC_ONCE
export BB_NR_NATIVE_PRE_SR=1 BB_NR_NATIVE_PRE_SR_SESSION="$SESSION"
export BB_NR_NATIVE_PRE_SR_RUN_DIR="$RUN"
export BB_NR_NATIVE_ORIGIN_RUN_DIR="$RUN" BB_NR_NATIVE_ORIGIN_HELPER="$HELPER"
export BB_NR_NATIVE_ORIGIN_WINE="$GE/files/lib/wine/x86_64-unix/wine"
export BB_NR_NATIVE_ORIGIN_PRELOADER="$GE/files/lib/wine/x86_64-unix/wine-preloader"
export BB_NR_NATIVE_ORIGIN_PREFIX="$PROFILE/wine-prefix" BB_NR_NATIVE_ORIGIN_WINEDEBUG='-all,warn+vulkan,err+vulkan'
export BB_NR_NATIVE_ORIGIN_MODE="$MODE"
export DLSSNR_DISABLE=1 DLSSNR_AUTO_DOWNLOAD=0
{
  printf 'session=%s\nmode=%s\nrun=%s\nmarker=%s\nbuild=%s\nassets=%s\n' "$SESSION" "$MODE" "$RUN" "$PROFILE/.native-pre-sr-profile" "$BUILD" "$GAME"
  printf 'scene_arm=%s\nscene_arm_content=%s\\n\nmax_jobs=4\nsource_limit=120\nouter_seconds=300\n' "$RUN/native-scene-arm" "$SESSION"
  printf 'BB_FPS=uncap\nBB_FPS_LIMIT=90\njitter=1\nrender=1280x720\noutput=1920x1080\n'
  printf 'route=original_HDR720_NR720_native_SR1080\nsoft_acceptance_ms=500\nphase=UNVERIFIED\nruntime=NOT_RUN_AT_BUILD\n'
  env | sort | sed -n '/^BB_NR_NATIVE_/p'
} > "$RUN/native-pre-sr-launch.env"
chmod 600 "$RUN/native-pre-sr-launch.env"
printf 'NATIVE_PRE_SR_LAUNCH_READY session=%s mode=%s arm=%s route=pre_sr_nr_to_native_dlss_sr\n' "$SESSION" "$MODE" "$RUN/native-scene-arm"
exec bash "$WT/run.sh"
