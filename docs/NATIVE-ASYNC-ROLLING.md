# Native rolling acquisition/cache candidate

Independent WT bbport-async-rolling-worktree / branch poc/nr-native-async-rolling-20261014,
base bb9bd2a69ce541518b8c2bd6cb66c98a90140e60. Sole opt-in
BB_NR_NATIVE_ASYNC_ROLLING=exact0|1/default0; localnoop stays legacy. New path
uses7 physical raw/converted/output/replay packets, logicalNodeCount5/MaxLinks4
unchanged, same2 historybanks. Max4 Helper jobs/source120/once cold edit diagnostic.
Old Snapshot default bodies, original Sky renderer, owner/v3/model/Shader/SPIRV/math
are preserved; no recursive capture, wait, Flush or renderer file polling.

Physical packet/source metadata and active editOrigin are separate. New INPUT
is raw current converted SR, never cache output. Next reserve requires exact prior
CONSUMED/Ready. Verified result must match its immutable original packet, actual
input GPUrelease and Helper fence, exact onejob/reset/API predicate; catchup uses
explicit recorded consecutive physical span <=4links. Historybanks updated only
by valid cached reprojection or verified new NR Capture; acquisition does not reset
old edit. Age uses actual model origin, not last reprojected frame; age>4/reset/gap/
unknown/busy returns current originalSR. No coverage/FPS/phase/quality promise.

Slots are reusable only after actual max raw/converted/sourceViews/current output/
replay/descriptor/UBO Native use and peer lease, and while unpinned by active/pending
lineage. Source recording and catchup are on the same Native queue; barriers/order
cover new commands. Callback closures retain old immutable lease objects. History
bank read/write uses are tracked separately from raw packets; merge samples only
actual selected current output (thus marks that packet), not every old packet.
Packet capacity busy is bounded raw fallback and invalidates continuity.
Packed image accounting:7 raw packets * (16588800+7372800+3686400) =
193536000B; Sky converted/output/replay/banks/dummy images =915426288B;
total1108962288B across71 images. The old5 raw+Sky total was832482288B,
so two added packets still cost276480000B (2*138240000), within the authorized
incremental cap. The original175953 freeze/docs reported970722288B incorrectly
by excluding raw images from the old baseline; that historical accounting is
retained as ERROR, not MATCH. Runtime image summation/logger was already correct.
VMA actual allocation regions are logged independently; CB/logicalbytes and
hidden model/driver VRAM are not included. No extra historycache/model/sharedslot.

Build dependency mounts temporarily replace empty gitlink directories with
symlinks to private local dependency copies. Stable dependency sources are read
only; FSR patches apply only to the private copy without its .git. Default Git
status can error on the fsr-vulkan symlink and is not a universal clean proof.
Use tracked status with --ignore-submodules=all plus explicit dependency source,
copy and patched-content SHA manifests. build_c0.sh restores empty mountpoints
on exit; record actual status return/output and link state at verification time.

Same two-arg launcher tools/nr_probe/run_game_native_async.sh SESSION MODE;
use rolling1 +sceneFirst1, Clinic -> native-helper-start -> actualReady -> scene-arm.
MODE resident|skipEvaluate, Root exclusive assets/seed/approval/GPU budget. The old
actualCurrentPresent fields bind active displayed NR origin/job; pending acquisition
is separate diagnostic. Once readback is raw/current selected sameSource, active
origin/job/age/history and actual merge/Present; defaulthot0readback.

BUILD_ONLY and CPU state/slot tests are not runtime/VRAM/coverage/quality acceptance.
Counter direction already actual no-gain/0 proof events; no repeatedGPU here.
