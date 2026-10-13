# Original game HDR720 NR -> same-source native DLSS SR1080 candidate

Independent branch/worktree based8256406c. Default off BB_NR_NATIVE_PRE_SR=1,
mutually exclusive with old async/P0/Q1/C1-C4 paths. Root owns approval/GPU lock,
private asset/seed/profile, Clinic arm and bounded300s launches. Entry:
tools/nr_probe/run_game_native_pre_sr.sh SESSION resident|skipEvaluate;
case bbport/runs/native-pre-sr-game-SESSION/profile/.native-pre-sr-profile;
arm file native-scene-arm exactly SESSION newline; max4 jobs/source cap120.
BB_NATIVE_PRE_SR_BUILD_DIR optionally selects a Root approved isolated bundle.

Only normal !Scaled Quality HDR Run, original SceneTargets Color97/Depth130 DEPTH
and actual camera motion83 all1280x720. Read/Transit GENERAL and actual motion
write->read barrier precede the hook. Mandatory actual CI/TransferSrc/aspect checks;
independent same-format immutable snapshots then same-texel32F109/100/103.
Original Color proxy is never overwritten. No SR1080 input or downsample.

Root sole base5f4da579 plus revision2 deltaad2e589a deterministically generate
native_pre_sr_spec.h; A generated fullJSON is equal. v3 names/strict stages,
hostACKfalse/realGPUtrue, UUID/PID/preloader/PE/FD and exact job/terminal retained.
Shared Color/Depth/MV/Output all720. Create/Evaluate/Resolve720, WorkingScale1,
manualWP1/Style0/Intensity1/MVscale1 experimental. NR Reset1 each initial job.
Actual jitter8scalar retained; no NR jitter setter, phase/MVscale UNVERIFIED.

Cold Helper on managed owner; no renderer cold-init wait or file polling.
Only Ready/scene-arm supports actual current frame. Whole bridge soft500ms uses
existing finite Native timeline wait and managed completion state; not GPU cancel.
Input copy+EXTERNAL release are submitted/proved before INPUT publication. A real
exact-source completion alone can be acquired and converted to private97720;
4B GPU guard rejects nonfinite/abs>65504/alpha-bit mismatch without clamp.
Private mutable-compatible uint16 views copy original alpha bits literally,
including signed zero/subnormals; only NR RGB is packed to half. SR sees the
same private image through its normal format97 view. Only
actual return target completion, guardclear, same source/epoch/history/params and
before soft deadline grant that source's pending SR Color. Final acceptance is
rechecked after the caller acquires the new command buffer, before selecting SR
Color; a missed cut gets obsolete CONSUMED and original SR. No late frame reuse.
Unknown peer/GPU state stays held/quarantined. Late actual completed output is
acquired/discarded/released and exactCONSUMED after actual last-use; managed stop.

Run crumb ends before Flush, caller and RecordDlss reacquire CommandBuffer; barrier
closures use updated cmd. Returned720 lives until actual native DLSS SR read tick,
not conversion tick. Slot reuse requires exact consumed plus actual local final
GPU use; normal retire marks destroyed only after owned API objects are destroyed.
Model/helper fence serial never numerically compared with Native timeline.

Hook follows original DLSS feature preparation (which may request reset) but is
before Evaluate. source_reset wire is the real original predecision request.
Applied SR reset = original OR raw/NR input-domain switch; NRReset1 does not force
SR reset. SR still receives actual original jitter/frame_ms, guide720 and output
1080. Sharpen/original HDR RGB merge/scene alpha/guest Present route unchanged.
Distinct PRE_SR ledger keeps exact source+1, same-scheduler merge<=prepare<=ready,
nonreuse actual Present. No old async inference/effect labels are substituted.

BUILD_ONLY/CPU are offline gates. No GPU runtime, game pixels/phase, performance,
quality or playable PASS. Soft deadline cannot preempt a blocked graphics driver.
Original async825114/4, P0/P3, formal files/real saves/twoC1 and Root budgets remain
protected. Root alone chooses bounded runtime cases. Failures are preserved.

Scene-first preparation delta (default flag0 preserves the frozen first candidate):
Root sets BB_NR_NATIVE_PRE_SR_SCENE_FIRST=1. The existing managed StartArm thread
checks RUN/native-helper-start every100ms; content must be exactly SESSION newline,
no CRLF/extra bytes/other session. It publishes only helper_start_requested atomic.
Renderer Poll creates Peer only when actual supported route and that request are
true, never reads files; Root first proves Clinic, writes helper-start, then waits
for actual model/import FD_READY before existing native-scene-arm. No wire delta.
No start file does not claim unsupported/failed model and leaves current raw SR.

Window close before any Peer creation stops and joins the preparation thread before
ExitComplete. Actual peer absence, slot/snapshot absence, pending absence, attempted
jobs0 AND maximum recorded Native use tick0, no unknown state and actual thread
join are independently checked. N0 alone never grants release/exit. New explicit
NATIVE_PRE_SR_EXIT_NO_OWNER ledger proves absence, not normal Helper reap0 or NR.
Any created peer/import/snapshot/recorded use retains existing managed retirement
and unknown hold. Source/frame math, job/reset/four counters,500ms and fences do
not change. No GPU runtime or launch qualification is implied by CPU/build PASS.
