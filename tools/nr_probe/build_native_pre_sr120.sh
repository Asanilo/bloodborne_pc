#!/usr/bin/env bash
# Native-only snapshot/metadata BUILD_ONLY, no Helper/Wine/game/GPU execution.
set -euo pipefail
WT=$(cd -- "$(dirname -- "$0")/../.." && pwd -P)
R=/home/arinp/Code/nrbridge-design/game-poc-20261011
RUN="$R/bbport/runs/native-pre-sr120-build-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -m 700 -p "$RUN/source-snapshot"
mapfile -t INPUTS < <(python3 - "$R/bbport/runs/p0-build-20261012T101054Z/source-sha256.txt" <<'PY'
import pathlib,sys
p=pathlib.Path('/home/arinp/Code/nrbridge-design/game-poc-20261011/bbport-worktree')
for line in pathlib.Path(sys.argv[1]).read_text().splitlines(): print(pathlib.Path(line.split(None,1)[1].strip()).relative_to(p))
PY
)
INPUTS+=(gpu/shadps4/video_core/renderer_vulkan/native_async_state.h gpu/shadps4/video_core/renderer_vulkan/native_async_owner.h gpu/shadps4/video_core/renderer_vulkan/native_async_owner.cpp gpu/shadps4/video_core/renderer_vulkan/native_async_snapshot.h gpu/shadps4/video_core/renderer_vulkan/native_async_snapshot.cpp tools/nr_probe/test_native_async_cpu.cpp tools/nr_probe/build_native_async.sh docs/NATIVE-ASYNC-SNAPSHOT.md gpu/shim/bbgpu.cpp gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp)
INPUTS+=(gpu/shadps4/video_core/renderer_vulkan/native_sky_gpu.h gpu/shadps4/video_core/renderer_vulkan/native_sky_gpu.cpp gpu/shadps4/video_core/renderer_vulkan/native_sky_constants.h gpu/shadps4/video_core/renderer_vulkan/native_sky_r2_spv.h gpu/shadps4/video_core/renderer_vulkan/native_sky_provenance.json gpu/shadps4/video_core/renderer_vulkan/native_origin_lease.h gpu/shadps4/video_core/host_shaders/native_sky_convert.comp gpu/shadps4/video_core/host_shaders/upscale_merge.comp gpu/CMakeLists.txt docs/NATIVE-SKY-GPU.md)
INPUTS+=(gpu/shadps4/video_core/renderer_vulkan/native_origin_identity_policy.h tools/nr_probe/test_native_origin_identity_cpu.cpp)
INPUTS+=(gpu/shadps4/video_core/renderer_vulkan/native_origin_gpu.h gpu/shadps4/video_core/renderer_vulkan/native_origin_gpu.cpp gpu/shadps4/video_core/renderer_vulkan/native_origin_owner.h gpu/shadps4/video_core/renderer_vulkan/native_origin_owner.cpp gpu/shadps4/video_core/renderer_vulkan/native_origin_wire.h gpu/shadps4/video_core/renderer_vulkan/native_origin_spec.h gpu/shadps4/video_core/renderer_vulkan/native_async_frontend.h gpu/shadps4/video_core/renderer_vulkan/native_sky_diagnostic.h gpu/shadps4/video_core/renderer_vulkan/native_sky_diagnostic.cpp tools/nr_probe/run_game_native_async.sh tools/nr_probe/test_native_origin_wire_cpu.cpp docs/NATIVE-ASYNC-ORIGIN.md)
INPUTS+=(gpu/shadps4/video_core/renderer_vulkan/native_edit_diagnostic_policy.h tools/nr_probe/test_native_edit_diagnostic_cpu.cpp docs/NATIVE-ASYNC-EDIT-DIAGNOSTIC.md)
INPUTS+=(gpu/shadps4/video_core/renderer_vulkan/native_pre_sr_hot120_policy.h tools/nr_probe/test_native_pre_sr120_cpu.cpp tools/nr_probe/run_game_native_pre_sr120.sh tools/nr_probe/build_native_pre_sr120.sh docs/NATIVE-PRE-SR120.md)
INPUTS+=(gpu/shadps4/video_core/renderer_vulkan/native_pre_sr_policy.h gpu/shadps4/video_core/renderer_vulkan/native_pre_sr_spec.h gpu/shadps4/video_core/renderer_vulkan/native_pre_sr_wire.h gpu/shadps4/video_core/renderer_vulkan/native_pre_sr_owner.h gpu/shadps4/video_core/renderer_vulkan/native_pre_sr_owner.cpp gpu/shadps4/video_core/renderer_vulkan/native_pre_sr_origin_gpu.h gpu/shadps4/video_core/renderer_vulkan/native_pre_sr_origin_gpu.cpp gpu/shadps4/video_core/renderer_vulkan/native_pre_sr_gpu.h gpu/shadps4/video_core/renderer_vulkan/native_pre_sr_gpu.cpp gpu/shadps4/video_core/renderer_vulkan/native_pre_sr_bridge.h gpu/shadps4/video_core/renderer_vulkan/native_pre_sr_bridge.cpp gpu/shadps4/video_core/host_shaders/native_pre_sr_return.comp tools/nr_probe/test_native_pre_sr_cpu.cpp tools/nr_probe/run_game_native_pre_sr.sh tools/nr_probe/build_native_pre_sr.sh docs/NATIVE-PRE-SR.md)
while IFS= read -r -d '' f;do [[ ! -f "$WT/$f" ]]||INPUTS+=("$f");done < <(git -C "$WT" diff --name-only -z HEAD)
mapfile -t INPUTS < <(printf '%s\n' "${INPUTS[@]}"|sort -u)
manifest(){ : > "$1";for f in "${INPUTS[@]}";do sha256sum "$WT/$f" >> "$1";done;}
manifest "$RUN/source-sha256.txt"
for f in "${INPUTS[@]}";do mkdir -p "$RUN/source-snapshot/$(dirname "$f")";cp -a "$WT/$f" "$RUN/source-snapshot/$f";done
git -C "$WT" diff HEAD > "$RUN/tracked-source.patch"
printf 'command=bash tools/nr_probe/build_c0.sh\nworktree=%s\nbase=%s\nno_GPU_no_Wine_no_game=1\nexisting_p0_wire_and_Helper_unchanged=1\n' "$WT" "$(git -C "$WT" rev-parse HEAD)" > "$RUN/commands.txt"
set +e
c++ -std=c++20 -pthread -Wall -Wextra -Werror -I"$WT/gpu/shadps4" "$WT/tools/nr_probe/test_native_async_cpu.cpp" "$WT/gpu/shadps4/video_core/renderer_vulkan/native_async_owner.cpp" -o "$RUN/test-native-async-cpu" > "$RUN/cpu-build.log" 2>&1
cpu=$?
if [[ $cpu -eq 0 ]];then timeout 20s "$RUN/test-native-async-cpu" > "$RUN/cpu-tests.json" 2>&1;cpu=$?;fi
if [[ $cpu -eq 0 ]];then
 c++ -std=c++20 -pthread -Wall -Wextra -Werror -I"$WT/gpu/shadps4" -I"$WT/gpu/shadps4/externals/nlohmann" "$WT/tools/nr_probe/test_native_origin_wire_cpu.cpp" -o "$RUN/test-native-origin-wire" > "$RUN/wire-build.log" 2>&1
 cpu=$?
 if [[ $cpu -eq 0 ]];then timeout 20s "$RUN/test-native-origin-wire" > "$RUN/wire-tests.json" 2>&1;cpu=$?;fi
fi
if [[ $cpu -eq 0 ]];then
 c++ -std=c++20 -Wall -Wextra -Werror -I"$WT/gpu/shadps4" "$WT/tools/nr_probe/test_native_origin_identity_cpu.cpp" -o "$RUN/test-native-origin-identity" > "$RUN/identity-build.log" 2>&1
 cpu=$?
 if [[ $cpu -eq 0 ]];then timeout 20s "$RUN/test-native-origin-identity" > "$RUN/identity-tests.json" 2>&1;cpu=$?;fi
fi
if [[ $cpu -eq 0 ]];then
 c++ -std=c++20 -Wall -Wextra -Werror -I"$WT/gpu/shadps4" "$WT/tools/nr_probe/test_native_edit_diagnostic_cpu.cpp" -o "$RUN/test-native-edit-diagnostic" > "$RUN/edit-diagnostic-build.log" 2>&1
 cpu=$?
 if [[ $cpu -eq 0 ]];then "$RUN/test-native-edit-diagnostic" > "$RUN/edit-diagnostic-tests.json" 2>&1;cpu=$?;fi
fi
if [[ $cpu -eq 0 ]];then
 c++ -std=c++20 -Wall -Wextra -Werror -I"$WT/gpu/shadps4" -I"$WT/gpu/shadps4/externals/nlohmann" "$WT/tools/nr_probe/test_native_pre_sr_cpu.cpp" -o "$RUN/test-native-pre-sr" > "$RUN/pre-sr-cpu-build.log" 2>&1
 cpu=$?
 if [[ $cpu -eq 0 ]];then "$RUN/test-native-pre-sr" > "$RUN/pre-sr-cpu-tests.json" 2>&1;cpu=$?;fi
fi
if [[ $cpu -eq 0 ]];then
 c++ -std=c++20 -pthread -Wall -Wextra -Werror -I"$WT/gpu/shadps4" -I"$WT/gpu/shadps4/externals/nlohmann" "$WT/tools/nr_probe/test_native_pre_sr120_cpu.cpp" -o "$RUN/test-native-pre-sr120" > "$RUN/pre-sr120-cpu-build.log" 2>&1
 cpu=$?
 if [[ $cpu -eq 0 ]];then "$RUN/test-native-pre-sr120" > "$RUN/pre-sr120-cpu-tests.json" 2>&1;cpu=$?;fi
fi
rc=$cpu
if [[ $rc -eq 0 ]];then bash "$WT/tools/nr_probe/build_c0.sh" > "$RUN/build.log" 2>&1;rc=$?;fi
set -e
[[ ! -f "$WT/out/gpu-build.log" ]]||cp -a "$WT/out/gpu-build.log" "$RUN/gpu-build.log"
manifest "$RUN/source-sha256.post.txt"
cmp -s "$RUN/source-sha256.txt" "$RUN/source-sha256.post.txt" || rc=3
if [[ $rc -eq 0 ]];then sha256sum "$WT/out/bb-probe" "$WT/out/gpu/libbbgpu.so" "$WT/out/libbbport_dlss.so" "$WT"/out/libnvidia-ngx-dlss.so.* > "$RUN/artifact-sha256.txt";fi
python3 - "$RUN" "$WT" "$rc" "$cpu" <<'PY'
import json,pathlib,sys
r,wt,rc,cpu=pathlib.Path(sys.argv[1]),sys.argv[2],int(sys.argv[3]),int(sys.argv[4])
d={'status':'PASS' if rc==0 else 'FAIL','scope':'NATIVE_PRE_SR_SAME_CURRENT_BUILD_ONLY','exit_code':rc,'CPU_exit':cpu,'worktree':wt,'GPU':'NOT_RUN','NR_transport':'SPARSE_ADAPTER_RUNTIME_NOT_RUN','NR_edit_selection':'PRE_SR_NR720_NATIVE_SR1080_RUNTIME_NOT_RUN','source_manifest':str(r/'source-sha256.txt'),'artifacts':str(r/'artifact-sha256.txt') if rc==0 else None}
(r/'build-result.json').write_text(json.dumps(d,indent=2)+'\n');print(json.dumps(d))
PY
exit "$rc"
