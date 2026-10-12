#!/usr/bin/env bash
# P0 Quality unequal-extents BUILD_ONLY; old Q0 artifacts and evidence are preserved.
# Reuses the audited offline copier; never starts the game or GPU.
set -euo pipefail
WT=$(cd -- "$(dirname -- "$0")/../.." && pwd -P)
POC_ROOT=${BB_GAME_POC_ROOT:-/home/arinp/Code/nrbridge-design/game-poc-20261011}
[[ $(sha256sum "$POC_ROOT/integration/playtest/p0-wire-spec.json" | awk '{print $1}') == 45673c70c172a428078ff9b750c32c87a0a70f3d0ef6bd6fcc09a53f5a290107 ]] || { echo "P0_BUILD BLOCKED canonical pin mismatch" >&2; exit 3; }
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
RUN="$POC_ROOT/bbport/runs/p0-build-$STAMP"
mkdir -m 700 -p "$RUN"
SOURCE_LIST=(
    gpu/shadps4/video_core/renderer_vulkan/p0_controller.cpp
    gpu/shadps4/video_core/renderer_vulkan/p0_controller.h
    gpu/shadps4/video_core/renderer_vulkan/p0_game_bridge.cpp
    gpu/shadps4/video_core/renderer_vulkan/p0_game_bridge.h
    gpu/shadps4/video_core/renderer_vulkan/p0_nr_process.cpp
    gpu/shadps4/video_core/renderer_vulkan/p0_nr_process.h
    gpu/shadps4/video_core/renderer_vulkan/p0_status.cpp
    gpu/shadps4/video_core/renderer_vulkan/p0_status.h
    gpu/shadps4/video_core/renderer_vulkan/p0_wire.h
    gpu/shadps4/video_core/renderer_vulkan/p0_wait_budget.h
    gpu/shadps4/video_core/renderer_vulkan/p0_timing_math.h
    gpu/shadps4/video_core/renderer_vulkan/p3_submit_clock.h
    gpu/shadps4/video_core/renderer_vulkan/p0_sr_timing.h
    gpu/shadps4/video_core/renderer_vulkan/p0_sr_timing.cpp
    gpu/shadps4/video_core/host_shaders/p0_game_bridge.comp
    gpu/shim/bbport_overlay.cpp
    gpu/shim/bbgpu.cpp
    tools/nr_probe/build_p0.sh
    tools/nr_probe/run_game_p0.sh
    tools/nr_probe/test_p0_cpu.cpp
    gpu/shadps4/video_core/renderer_vulkan/q1_nr_process.cpp
    gpu/shadps4/video_core/renderer_vulkan/q1_nr_process.h
    gpu/shadps4/video_core/renderer_vulkan/q1_game_bridge.cpp
    gpu/shadps4/video_core/renderer_vulkan/q1_game_bridge.h
    gpu/shadps4/video_core/renderer_vulkan/q1_wire.h
    gpu/shadps4/video_core/host_shaders/q1_game_bridge.comp
    tools/nr_probe/run_game_q1.sh
    tools/nr_probe/build_q1.sh

    gpu/CMakeLists.txt
    gpu/shadps4/nlohmann/json.hpp
    gpu/shadps4/nlohmann/LICENSE.MIT
    gpu/shadps4/video_core/host_shaders/c1_game_bridge.comp
    gpu/shadps4/video_core/host_shaders/c2_game_bridge.comp
    gpu/shadps4/video_core/renderer_vulkan/c1_game_bridge.cpp
    gpu/shadps4/video_core/renderer_vulkan/c1_game_bridge.h
    gpu/shadps4/video_core/renderer_vulkan/c1_noop_process.cpp
    gpu/shadps4/video_core/renderer_vulkan/c1_noop_process.h
    gpu/shadps4/video_core/renderer_vulkan/c2_game_bridge.cpp
    gpu/shadps4/video_core/renderer_vulkan/c2_game_bridge.h
    gpu/shadps4/video_core/renderer_vulkan/c2_nr_process.cpp
    gpu/shadps4/video_core/renderer_vulkan/c2_nr_process.h
    gpu/shadps4/video_core/renderer_vulkan/vk_camera_motion.h
    gpu/shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp
    gpu/shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.h
    gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp
    gpu/shadps4/video_core/renderer_vulkan/vk_presenter.h
    tools/nr_probe/build_c0.sh
    tools/nr_probe/build_c1.sh
    tools/nr_probe/build_c2.sh
    tools/nr_probe/build_c3.sh
    tools/nr_probe/run_game_c3.sh
    gpu/shadps4/video_core/host_shaders/c3_game_bridge.comp
    gpu/shadps4/video_core/host_shaders/c3_frame_validate.comp
    gpu/shadps4/video_core/renderer_vulkan/c3_nr_process.cpp
    gpu/shadps4/video_core/renderer_vulkan/c3_nr_process.h
    gpu/shadps4/video_core/renderer_vulkan/c3_game_bridge.cpp
    gpu/shadps4/video_core/renderer_vulkan/c3_game_bridge.h
    gpu/shadps4/video_core/renderer_vulkan/c3_frame_validate.h
    tools/nr_probe/run_game_c0.sh
    tools/nr_probe/run_game_c1.sh
    tools/nr_probe/run_game_c2.sh
    tools/nr_probe/build_c4.sh
    tools/nr_probe/build_q0.sh
    tools/nr_probe/build_q0_wait_diag.sh
    tools/nr_probe/run_game_q0_wait_diag.sh
    gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h
    tools/nr_probe/build_q0_wait_state.sh
    tools/nr_probe/build_q1.sh
    tools/nr_probe/run_game_q0_wait_state.sh
    gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp
    tools/nr_probe/run_game_q0.sh
    gpu/shadps4/video_core/renderer_vulkan/vk_camera_motion.cpp
    gpu/shadps4/video_core/renderer_vulkan/vk_scene_resolution.cpp
    gpu/shadps4/video_core/renderer_vulkan/vk_scene_resolution.h
    tools/nr_probe/run_game_c4.sh
    gpu/shadps4/video_core/host_shaders/c4_game_bridge.comp
    gpu/shadps4/video_core/renderer_vulkan/c4_nr_process.cpp
    gpu/shadps4/video_core/renderer_vulkan/c4_nr_process.h
    gpu/shadps4/video_core/renderer_vulkan/c4_game_bridge.cpp
    gpu/shadps4/video_core/renderer_vulkan/c4_game_bridge.h
    gpu/shadps4/video_core/renderer_vulkan/c4_frame_validate.h
)
# Include all actual modified tracked inputs, not just the new bridge shortlist.
while IFS= read -r -d '' rel; do
    [[ -f "$WT/$rel" ]] && SOURCE_LIST+=("$rel")
done < <(git -C "$WT" diff --name-only -z HEAD)
mapfile -t SOURCE_LIST < <(printf '%s\n' "${SOURCE_LIST[@]}" | sort -u)
for rel in "${SOURCE_LIST[@]}"; do
    [[ -f "$WT/$rel" ]] || { echo "P0_BUILD BLOCKED missing source: $rel" >&2; exit 3; }
done
JSON_SHA=aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63
[[ $(sha256sum "$WT/gpu/shadps4/nlohmann/json.hpp" | awk '{print $1}') == "$JSON_SHA" ]] || {
    echo 'P0_BUILD BLOCKED vendored nlohmann header pin mismatch' >&2; exit 3;
}
source_manifest() {
    local output=$1
    : > "$output"
    for rel in "${SOURCE_LIST[@]}"; do sha256sum "$WT/$rel" >> "$output"; done
}
source_manifest "$RUN/source-sha256.txt"
sha256sum "$POC_ROOT/integration/playtest/p0-wire-spec.json" "$POC_ROOT/integration/playtest/frontend-interface.json" > "$RUN/contract-sha256.txt"
mkdir -p "$RUN/source-snapshot"
for rel in "${SOURCE_LIST[@]}"; do
    mkdir -p "$RUN/source-snapshot/$(dirname "$rel")"
    cp -a "$WT/$rel" "$RUN/source-snapshot/$rel"
done
mkdir -p "$RUN/pre-p0-out"
for artifact in "$WT/out/bb-probe" "$WT/out/gpu/libbbgpu.so" "$WT/out/libbbport_dlss.so" "$WT"/out/libnvidia-ngx-dlss.so.*; do
    rel=${artifact#"$WT/"}
    if [[ -f "$WT/$rel" ]]; then
        mkdir -p "$RUN/pre-p0-out/$(dirname "$rel")"
        cp -a "$WT/$rel" "$RUN/pre-p0-out/$rel"
        sha256sum "$WT/$rel" >> "$RUN/pre-p0-artifact-sha256.txt"
    fi
done
{
    printf 'scope=P0_BUILD_ONLY_NO_GPU\nworktree=%s\nhead=%s\n' "$WT" "$(git -C "$WT" rev-parse HEAD)"
    printf 'command=bash %s/tools/nr_probe/build_c0.sh (audited offline dependency copier; no install/network)\n' "$WT"
    printf 'canonical_contract=%s\ncanonical_contract_sha256=%s\n' \
        "$POC_ROOT/integration/playtest/p0-wire-spec.json" "$(sha256sum "$POC_ROOT/integration/playtest/p0-wire-spec.json" | awk '{print $1}')"
    printf 'diagnostic_opt_in=BB_NR_P0_MODE=noop|single|skipEvaluate|resident; old wait diagnostics OFF; actual runtime NOT_RUN\n'
    printf 'helper_runtime=NOT_STARTED; P0 build-only; diagnostic run owned by Root\n'
    printf 'helper_runtime=NOT_STARTED\ngame=NOT_RUN\nGPU=NOT_RUN\nstable_repository_write=none\n'
} > "$RUN/commands.txt"
set +e
c++ -std=c++23 -pthread -Igpu/shadps4 "$WT/tools/nr_probe/test_p0_cpu.cpp" "$WT/gpu/shadps4/video_core/renderer_vulkan/p0_status.cpp" -o "$RUN/test-p0-cpu" > "$RUN/cpu-build.log" 2>&1 && "$RUN/test-p0-cpu" > "$RUN/cpu-tests.json" 2>&1
cpu_rc=$?
if [[ $cpu_rc -eq 0 ]]; then bash "$WT/tools/nr_probe/build_c0.sh" > "$RUN/build.log" 2>&1; else cp "$RUN/cpu-build.log" "$RUN/build.log"; false; fi
rc=$?
set -e
source_manifest "$RUN/source-sha256.post.txt"
sha256sum -c "$RUN/contract-sha256.txt" > "$RUN/contract-check.txt" || rc=3
if ! cmp -s "$RUN/source-sha256.txt" "$RUN/source-sha256.post.txt"; then
    echo 'P0_BUILD FAIL source input changed during build' >> "$RUN/build.log"
    rc=2
fi
if [[ $rc -eq 0 ]]; then
    sha256sum "$WT/out/bb-probe" "$WT/out/gpu/libbbgpu.so" \
        "$WT/out/libbbport_dlss.so" "$WT"/out/libnvidia-ngx-dlss.so.* > "$RUN/artifact-sha256.txt"
fi
python3 - "$RUN" "$WT" "$rc" <<'PY'
import json, pathlib, subprocess, sys
run, wt, code = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), int(sys.argv[3])
underlying = None
for path in sorted(run.parent.glob("c0-build-*/build-result.json"),
                   key=lambda p: p.stat().st_mtime, reverse=True):
    data = json.loads(path.read_text())
    if pathlib.Path(data.get("worktree", "")) == wt and data.get("status") in ("PASS", "FAIL", "BLOCKED"):
        underlying = str(path)
        break
result = {
    "status": "PASS" if code == 0 else "BLOCKED" if code == 3 else "FAIL",
    "scope": "P0_BUILD_ONLY_NO_GPU", "Q1_Q2_baseline_commit": "fc7135e8101b7ef62ed4842232cc2122a29f45bf", "exit_code": code,
    "worktree": str(wt),
    "head": subprocess.check_output(["git", "-C", str(wt), "rev-parse", "HEAD"], text=True).strip(),
    "build_log": str(run / "build.log"), "offline_builder_evidence": underlying,
    "source_manifest": str(run / "source-sha256.txt"),
    "source_snapshot": str(run / "source-snapshot"),
    "pre_q1_artifacts_preserved": str(run / "pre-p0-out"),
    "artifact_manifest": str(run / "artifact-sha256.txt") if (run / "artifact-sha256.txt").exists() else None,
    "gpu_status": "NOT_RUN", "game_status": "NOT_RUN",
}
(run / "build-result.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
PY
exit "$rc"
