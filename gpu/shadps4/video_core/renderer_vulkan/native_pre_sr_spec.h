// SPDX-License-Identifier: GPL-2.0-or-later
// DERIVED ONLY: Root base 5f4da5793c45d94181f1719d987b3f9c00c77faa0f26583304267a4f0de3af58; delta revision2 ad2e589a36bdb59b6425d2842572dd0a8acbbbd6fc7fecdcbbc398e4b72d0770
#pragma once
inline constexpr char NativePreSrSpecJson[]=R"PRESPEC({
  "schema": "native-sparse-origin-nr-v1",
  "status": "CANONICAL_MINIMAL_IMPLEMENTATION_INTERFACE_RUNTIME_NOT_APPROVED",
  "protocol_version": 1,
  "profile_id": "pre-sr-quality-hdr720-j1-experimental-v1",
  "isolation": "New optional Native and Helper adapters only. P0 protocol6/parser/acceptance remain unchanged.",
  "ownership": "Native edit cache each game frame; Helper sparse origin NR only",
  "quality_profile": {
    "color_width": 1280,
    "color_height": 720,
    "guide_width": 1280,
    "guide_height": 720,
    "model_width": 1280,
    "model_height": 720,
    "output_width": 1280,
    "output_height": 720,
    "working_scale": 1.0,
    "color_domain": "scene_hdr",
    "passthrough": false,
    "white_point": 1.0,
    "exposure_source": "manual_unknown_game_exposure",
    "exposure_valid": false,
    "stored_mv_units": "render_pixels_previous_minus_current",
    "mv_values_jittered": false,
    "guide_sampling": "raw_jittered_raster_same_texel",
    "phase_correction": "none",
    "nr_phase_status": "UNVERIFIED",
    "k_output_status": "NOT_IDENTIFIED",
    "nr_mvec_scale_x": 1.0,
    "nr_mvec_scale_y": 1.0,
    "nr_mvec_scale_status": "EXPERIMENTAL_UNVERIFIED",
    "model_input": "fixed Encode720 from original pre-SR HDR renderColor",
    "resolved_output": "fixed Resolve720; modified renderColor only to same-source Native DLSS SR1080",
    "cost_status": "PRE_SR_GAME_COST_UNMEASURED",
    "route": "pre_sr_nr_to_native_dlss_sr",
    "sr_output_width": 1920,
    "sr_output_height": 1080
  },
  "model_parameters": {
    "style": 0,
    "intensity": 1.0,
    "white_point": 1.0,
    "mvec_scale": [
      1.0,
      1.0
    ],
    "reset": 1
  },
  "parameter_application": {
    "frame_ms": "HOST_FIXTURE_DECLARATION_NOT_SENT_TO_NR_API",
    "exposure": "HOST_FIXTURE_DECLARATION_NOT_SENT_TO_NR_API",
    "pre_exposure": "HOST_FIXTURE_DECLARATION_NOT_SENT_TO_NR_API",
    "runtime_default_values": "UNVERIFIED"
  },
  "shared_images": {
    "exporter": "Helper exports one persistent 4-image DMA-BUF resource set using proved P0 bootstrap; Native imports after actual UUID/PID/format/fd checks",
    "roles": {
      "Color": {
        "format": 109,
        "extent": [
          1280,
          720
        ]
      },
      "Depth": {
        "format": 100,
        "extent": [
          1280,
          720
        ]
      },
      "MotionVectors": {
        "format": 103,
        "extent": [
          1280,
          720
        ]
      },
      "Output": {
        "format": 109,
        "extent": [
          1280,
          720
        ]
      }
    },
    "layout": "GENERAL and real queue-family EXTERNAL handoff at every producer/consumer stage",
    "slot_count": 1,
    "reuse": "No next INPUT until exact prior CONSUMED after Native shared-image last-use GPU proof. Shared FD identity/generation remains immutable."
  },
  "job_identity_fields": [
    "session",
    "generation",
    "resource_set",
    "job",
    "origin_source",
    "origin_epoch",
    "origin_history",
    "params_version",
    "profile_id"
  ],
  "generation": 0,
  "resource_set": "SESSION-g0",
  "job": "strict ordinal0..max_jobs-1; differs from real game source and Present",
  "origin": "actual immutable unedited Native source snapshot; at most4 complete real adjacent links retained; capture and all future peer/native uses leased",
  "bootstrap": "Reuse P0 MODEL_READY + exporter identity (game_pid/start_ticks, Linux PID) + resource registry + Native import/FD_READY stages with explicit new protocol/profile; no external semaphore import assumption",
  "messages": {
    "INPUT": {
      "op": "input",
      "status": "ORIGIN_INPUT_READY",
      "required": [
        "all job_identity_fields",
        "quality_profile",
        "parameter_application",
        "native_input_tick",
        "native_input_GPU_complete",
        "source_reset",
        "source_jitter_metadata",
        "model_reset"
      ],
      "conditions": "Native copy/convert from immutable origin and EXTERNAL release; exact actual submitted target completion observed by owner before AtomicWrite. model_reset=1 each sparse job, independent of edit history."
    },
    "COMPLETE": {
      "op": "complete",
      "status": "ORIGIN_NR_COMPLETE",
      "required": [
        "all job_identity_fields",
        "native_input_tick_echo",
        "helper_gpu_serial",
        "helper_GPU_complete",
        "output_valid",
        "model_valid",
        "evaluate_calls",
        "evaluate_result",
        "resolve_calls",
        "model_reset",
        "evaluate_parameters",
        "parameter_application",
        "gpu_times"
      ],
      "conditions": "Actual Helper acquire/encode1080/downsample720/Evaluate720/resolve1080/EXTERNAL release and current fence. Resident exactly1 Evaluate/resolve with result1; skip0, output_valid=false/model_valid=false. Never labels arrival-current as origin."
    },
    "CONSUMED": {
      "op": "consumed",
      "status": "ORIGIN_RESOURCES_RELEASED",
      "required": [
        "all job_identity_fields",
        "helper_gpu_serial_echo",
        "native_shared_last_use_tick",
        "native_shared_GPU_complete",
        "disposition"
      ],
      "conditions": "Native acquired real completed shared images. Capture uses original retained Color/Depth vs NR[origin]; or safely drains obsolete job. EXTERNAL release and exact native shared last-use completion before owner publishes. disposition=captured|obsolete_discard|expected_skip; no Present or current-frame acceptance claim."
    }
  },
  "timeline_domains": "Native input/last-use ticks refer only to held same Native timeline. Helper fence serial refers only to Helper current submission. Echoes are identity, never cross-domain completion proof.",
  "native_mailbox": "Renderer bounded memory-only TryCapture/ready/lease checks. Worker handles existing already-submitted completion observations and files; worker never records Scheduler commands. No extra Flush/wait/readback or per-frame file polling.",
  "cache_admission": "Same retained origin/session/epoch/history/params; actual NR completion; full genuine contiguous chain reaches exact current source <=4. Complete all catchup before view eligible; pending/stale/reset/missing/unsupported => current raw SR.",
  "history": "NR model reset1 for each sparse job. Native edit history only advances via actual continuous game sources; source gap/reset/off/route/params changes invalidate eligibility without overwriting unknown owned uses.",
  "shutdown": "Stop new origins, drain exact actual jobs and every Native/Helper last use, release/retired/audit/reap actual terminal count including0. Unknown peer/GPU completion quarantines; kill/timeout never proves GPU release. Existing normal tuple discipline reused in new namespace.",
  "first_runtime_limits": {
    "max_jobs": 4,
    "max_cache_age_links": 4,
    "watchdog_seconds": 300,
    "helper_start_allowance": 1,
    "full_CPU_pixel_readback_per_hot_frame": 0
  },
  "not_in_this_interface": [
    "Helper per-frame cache",
    "NR-to-SR route migration",
    "new Scheduler",
    "new semaphore FD machinery",
    "live model/style/extent changes",
    "normal playable or60FPS declaration"
  ],
  "implementation_targets": {
    "A": "new async-origin helper using pinned production/lowres adapter and proved bootstrap",
    "B": "new native-only origin bridge/leases/owner and Sky GPU hook"
  },
  "endpoint_confirmation": "Root sole canonical. A/B report concrete unsupported API/resource or missing literal before action; no alternate schema. Initial model720 choice reuses actual lowres functional path, cost NOT_IDENTIFIED. No GPU permission comes from this interface.",
  "helper_directory": "opti-highfps-worktree/poc/game_nr/async-origin",
  "helper_executable": "out/wine_origin_nr.exe",
  "helper_cli": [
    "resident|skipEvaluate",
    "RUN_DIR_WINDOWS",
    "SESSION"
  ],
  "config_filename": "async-config.json",
  "transport_layout": "frame-buckets-64-v1",
  "job_filenames": "Reuse existing frame_dir(job) bucket64; basename async-{input|complete|consumed}-SESSION-g0-fJOB.json. fJOB is a job ordinal only, not current game source.",
  "bootstrap_filenames": "Use bootstrap_exact literal filenames; async-exporter.pid is the actual Linux exporter PID reply, async-pid-request.json the public WinPID/exe discovery request.",
  "stop_control": {
    "request": "async-control-request-SESSION-g0.json",
    "ack": "async-control-ack-SESSION-g0.json",
    "action": "stop",
    "control_epoch": "one pending strict increment; exact duplicate idempotent; mismatch rejects",
    "condition": "No new origin jobs. In-flight valid tuple still needs actual completion and safe Native shared release/CONSUMED before normal STOP_READY.",
    "wait_input": "No missing-input frame timeout while sparse idle. Peer identity, finite service wall, abort and control still checked.",
    "terminal_files": "Reuse real release/retired/native-audited/reap discipline with new async- prefix and actual finalized job count N including0; no source Present inference."
  },
  "budgets": {
    "cold_init_ms": 120000,
    "active_job_ms": 30000,
    "GPU_fence_ms": 10000,
    "control_ms": 60000,
    "service_wall_seconds": 300,
    "render_wait_for_current_NR_ms": 0
  },
  "cache_mapping": {
    "MvScale": [
      1.0,
      1.0
    ],
    "JitterDeltaUV": [
      0.0,
      0.0
    ],
    "status": "POST_SR_PHASE_NONE_EXPERIMENTAL; actual source jitter metadata retained, no claim game jitter0",
    "source": "r2 SKY-PASS-INTERFACE.md and original HLSL MotionToUv formula; NR API scale semantics separate"
  },
  "counters": {
    "finalized_jobs": "exact prior safe CONSUMED count, includes captured/obsolete/expected_skip",
    "captured_jobs": "real NR output captured by Native from bound immutable origin, not unique Present proof",
    "obsolete_jobs": "safe discarded obsolete job, not accepted edit",
    "skip_jobs": "safe expected skip output invalid; no Evaluate/resolve",
    "native_current_Present": "renderer independent actual real current source/Frame ledger",
    "native_effect_Present": "actual current Frame selecting valid cache composite; includes effect_origin and age, never Helper counter"
  },
  "interface_revision": 3,
  "bootstrap_exact": {
    "strict_types": "Exact common keys plus the listed stage keys. Unknown/missing/duplicate keys, bool-as-integer, negative/overflow or nonfinite values reject. uint IDs are <=2^53-1, counts <=4. Config sample values are a schema/example, not evidence that a process ran. All runtime-dependent values come from actual APIs.",
    "source_producers": {
      "Helper": "playable-quality/playable_transport.h metadata/create_images/initial_release/retire_fds and wine_game_nr_playable.cpp config/main/baseline; adapt only new namespace/protocol/model720 in new source",
      "Native": "renderer_vulkan/p0_nr_process.cpp setup/FindExporter/PID identity and p0_game_bridge.cpp baseline/metadata/import/FD retirement checks; new adapter only"
    },
    "common_required": {
      "protocol_version": "uint exact1",
      "transport_layout": "string exact frame-buckets-64-v1",
      "execution_mode": "string exact hot",
      "mode": "string CLI resident|skipEvaluate exact",
      "session": "string CLI safe ASCII token exact",
      "generation": "uint exact0",
      "resource_set": "string SESSION-g0 exact",
      "profile_id": "string canonical profile_id exact",
      "quality_profile": "object canonical quality_profile exact",
      "params_version": "uint exact1",
      "parameter_application": "object canonical four keys exact"
    },
    "async-config.json": {
      "producer": "Native owner before PE spawn",
      "exact_sample": {
        "protocol_version": 1,
        "transport_layout": "frame-buckets-64-v1",
        "execution_mode": "hot",
        "mode": "resident",
        "session": "ASYNC-SAMPLE",
        "generation": 0,
        "resource_set": "ASYNC-SAMPLE-g0",
        "profile_id": "pre-sr-quality-hdr720-j1-experimental-v1",
        "quality_profile": {
          "color_width": 1280,
          "color_height": 720,
          "guide_width": 1280,
          "guide_height": 720,
          "model_width": 1280,
          "model_height": 720,
          "output_width": 1280,
          "output_height": 720,
          "working_scale": 1.0,
          "color_domain": "scene_hdr",
          "passthrough": false,
          "white_point": 1.0,
          "exposure_source": "manual_unknown_game_exposure",
          "exposure_valid": false,
          "stored_mv_units": "render_pixels_previous_minus_current",
          "mv_values_jittered": false,
          "guide_sampling": "raw_jittered_raster_same_texel",
          "phase_correction": "none",
          "nr_phase_status": "UNVERIFIED",
          "k_output_status": "NOT_IDENTIFIED",
          "nr_mvec_scale_x": 1.0,
          "nr_mvec_scale_y": 1.0,
          "nr_mvec_scale_status": "EXPERIMENTAL_UNVERIFIED",
          "model_input": "fixed Encode720 from original pre-SR HDR renderColor",
          "resolved_output": "fixed Resolve720; modified renderColor only to same-source Native DLSS SR1080",
          "cost_status": "PRE_SR_GAME_COST_UNMEASURED",
          "route": "pre_sr_nr_to_native_dlss_sr",
          "sr_output_width": 1920,
          "sr_output_height": 1080
        },
        "params_version": 1,
        "parameter_application": {
          "frame_ms": "HOST_FIXTURE_DECLARATION_NOT_SENT_TO_NR_API",
          "exposure": "HOST_FIXTURE_DECLARATION_NOT_SENT_TO_NR_API",
          "pre_exposure": "HOST_FIXTURE_DECLARATION_NOT_SENT_TO_NR_API",
          "runtime_default_values": "UNVERIFIED"
        },
        "width": 1280,
        "height": 720,
        "render_width": 1280,
        "render_height": 720,
        "model_width": 1280,
        "model_height": 720,
        "max_jobs": 4,
        "initial_history_version": 1,
        "service_wall_seconds": 300,
        "cold_init_timeout_ms": 120000,
        "control_timeout_ms": 60000,
        "gpu_timeout_ms": 10000,
        "job_completion_timeout_ms": 30000,
        "parameters": {
          "style": 0,
          "intensity": 1.0,
          "working_scale": 1.0,
          "white_point": 1.0,
          "mvec_scale": [
            1.0,
            1.0
          ]
        },
        "manual_white_point": 1.0,
        "passthrough": false,
        "device_uuid": "8734fac9db30c279a340fe117ace6365",
        "driver_uuid": "ad6325108a6658c8aa0c9ffd3f6f97fe",
        "working_scale": 1.0,
        "native_soft_deadline_ms": 500
      },
      "variable_fields": "mode/session/resource_set/device_uuid/driver_uuid bound to actual CLI and Native device; all other values exact sample/canonical profile. working_scale denotes implemented encode/downsample dimensions, not an NR API scalar setter."
    },
    "async-pid-request.json": {
      "producer": "Helper after config checks, before Vulkan/model init",
      "required_stage_fields": {
        "op": "string exact pid_request",
        "status": "string exact PID_REQUEST",
        "exe": "string actual GetModuleFileNameA(self) Z path; Native NormalizeWinePath must equal pinned helper absolute path",
        "win_pid": "uint1..UINT32_MAX actual GetCurrentProcessId",
        "completed_GPU": "bool exactfalse",
        "output_valid": "bool exactfalse",
        "model_valid": "bool exactfalse"
      }
    },
    "async-exporter.pid": {
      "producer": "Native verified process selector; replaces misleading old native.pid name in new namespace",
      "format": "ASCII actual verified exporter Linux PID in1..INT32_MAX plus one newline; never launcher or WinPID",
      "preconditions": "Actual descendant game+launcher, pinned PE mapping and command RUN/mode/session unique, current /proc PID start_ticks. No private Wine ABI."
    },
    "native-exporter-identity.json": {
      "producer": "Native owner together with async-exporter.pid",
      "required_stage_fields": {
        "game_pid": "uint1..INT32_MAX actual Native game PID",
        "game_start_ticks": "uint>0 actual /proc/game_pid/stat field22",
        "launcher_pid": "uint1..INT32_MAX actual posix_spawn child PID",
        "exporter_pid": "uint equal async-exporter.pid",
        "exporter_start_ticks": "uint>0 actual /proc/exporter/stat field22",
        "descendant_game": "bool exacttrue after actual check",
        "descendant_launcher": "bool exacttrue after actual check",
        "request_exe_match": "bool exacttrue",
        "unique_run_cmd": "bool exacttrue",
        "exact_pe_maps": "bool exacttrue"
      }
    },
    "async-baseline-SESSION-g0.json": {
      "producer": "Helper after actual720 feature create and creation submit/fence, before shared export",
      "required_stage_fields": {
        "op": "string exact baseline",
        "status": "string exact BASELINE",
        "completed_GPU": "bool exacttrue",
        "output_valid": "bool exactfalse",
        "model_valid": "bool exactfalse",
        "model_init": "bool exacttrue",
        "feature_created": "bool exacttrue",
        "feature_create_calls": "uint exact1",
        "creation_GPU_complete": "bool exacttrue",
        "creation_GPU_serial": "uint>0 actual observed Helper submission serial",
        "model_width": "uint exact1280",
        "model_height": "uint exact720",
        "cold_init_timeout_ms": "uint exact120000",
        "control_timeout_ms": "uint exact60000",
        "gpu_timeout_ms": "uint exact10000",
        "job_completion_timeout_ms": "uint exact30000"
      }
    },
    "async-native-baseline-SESSION-g0.json": {
      "producer": "Native owner after identity/model-baseline check and Native+Helper FD inventories",
      "required_stage_fields": {
        "op": "string exact baseline_audit",
        "status": "string exact BASELINE_AUDITED",
        "completed_GPU": "bool exactfalse; FD inventory is host operation, not Native GPU work",
        "output_valid": "bool exactfalse",
        "model_valid": "bool exactfalse",
        "helper_creation_GPU_serial_echo": "uint equal Helper creation serial, identity only",
        "baseline_fd_count": "uint actual Native FD inventory count",
        "inventory": "string exact async-native-fd-baseline.json",
        "helper_inventory": "string exact async-wine-fd-baseline.json"
      }
    },
    "async-meta-g0.json": {
      "producer": "Helper after four create_images and actual initial GENERAL/EXTERNAL release fence",
      "required_stage_fields": {
        "op": "string exact metadata",
        "status": "string exact READY",
        "completed_GPU": "bool exacttrue",
        "output_valid": "bool exactfalse",
        "model_valid": "bool exactfalse",
        "linux_pid": "uint equal verified exporter PID",
        "win_pid": "uint actual WinPID, not Linux PID",
        "device_uuid": "string32 lower hex actual API UUID equal Native expected",
        "driver_uuid": "string32 lower hex actual API UUID equal Native expected",
        "handle_type": "uint exact512 (DMA_BUF)",
        "queue_family": "uint32 actual Helper queue family index",
        "initial_release_GPU_serial": "uint actual observed Helper serial > creation serial",
        "width": "uint exact1280",
        "height": "uint exact720",
        "render_width": "uint exact1280",
        "render_height": "uint exact720",
        "model_width": "uint exact1280",
        "model_height": "uint exact720",
        "max_jobs": "uint exact4",
        "initial_history_version": "uint exact1",
        "manual_white_point": "number finite exact1",
        "passthrough": "bool exactfalse",
        "resources": "array exact4 resource_required entries"
      },
      "resource_required": {
        "role": "string exact color|depth|mv|output, four unique in that order",
        "fd": "uint1..INT32_MAX actual exporter raw fd, unique",
        "allocation_size": "uint>0 actual req.size; exact Native matching CI/FD requirements",
        "memory_type_index": "uint0..31 actual exporter type",
        "req_memory_type_bits": "uint32 nonzero actual exporter req bits",
        "alignment": "uint>0 actual req.alignment",
        "width": "uint exact per-role canonical extent",
        "height": "uint exact per-role canonical extent",
        "format": "uint exact color/output109 depth100 mv103",
        "usage": "uint exact15",
        "tiling": "uint exact0 (OPTIMAL)",
        "image_type": "uint exact1 (2D)",
        "samples": "uint exact1",
        "mip_levels": "uint exact1",
        "array_layers": "uint exact1",
        "flags": "uint exact0",
        "sharing_mode": "uint exact0 (EXCLUSIVE)",
        "dedicated": "bool exacttrue",
        "offset": "uint exact0",
        "runtime_layout": "uint exact1 (GENERAL)",
        "external_features": "uint32 actual query; require importable+exportable bits and dedicated-only handled",
        "compatible_handle_types": "uint32 actual query; require DMA_BUF bit512",
        "prefers_dedicated": "uint VkBool32 0|1 from actual API, not JSON bool",
        "requires_dedicated": "uint VkBool32 0|1 from actual API, not JSON bool"
      }
    },
    "async-imported-SESSION-g0.json": {
      "producer": "Native owner after actual pidfd FD duplication, matching Native CI/FD memory requirements, successful vkAllocateMemory import ownership and image bind",
      "required_stage_fields": {
        "op": "string exact imported",
        "status": "string exact IMPORTED",
        "completed_GPU": "bool exactfalse; CPU Vulkan import does not prove Native acquisition submit",
        "output_valid": "bool exactfalse",
        "model_valid": "bool exactfalse",
        "initial_release_GPU_serial_echo": "uint exact metadata serial",
        "imported_roles": "array exact [color,depth,mv,output]",
        "raw_fd_pairs": "array four objects {role:string,fd:uint} equal exact metadata role/raw exporter fd pairs",
        "native_imported_count": "uint exact4",
        "owned_duplicate_fd_count_after_import": "uint exact0 only after successful API transfer/host close as required"
      }
    },
    "async-fd-retired-SESSION-g0.json": {
      "producer": "Helper after IMPORTED exact acknowledgement and proved public Vulkan FD ownership transfer for every raw fd",
      "required_stage_fields": {
        "op": "string exact fd_retired",
        "status": "string exact FD_RETIRED",
        "completed_GPU": "bool exacttrue for still-proven initial release, not a fictitious new submit",
        "output_valid": "bool exactfalse",
        "model_valid": "bool exactfalse",
        "initial_release_GPU_serial": "uint equal metadata serial",
        "closed_fd": "array exact four {role:string,fd:uint} matching original metadata, actual API transferred ownership",
        "export_raw_fd_owned": "uint exact0"
      }
    },
    "readiness": "Native Ready only after actual import + Helper fd-retired tuple and targeted FD identity audit. All bootstrap/file IO and process discovery stay on owner; renderer never blocks for them. No model input before Ready.",
    "scope_truth": "pid request/FD inventories/import ACK are host proofs; no blanket completed_GPU=true envelope before submission. Never overwrite old P0 or old user ledgers."
  },
  "source_jitter_metadata_exact": {
    "viewport_jitter_x": "finite JSON number actual origin metadata; no jitter setter application claim",
    "viewport_jitter_y": "finite JSON number actual origin metadata; no jitter setter application claim",
    "previous_viewport_jitter_x": "finite JSON number actual origin metadata; no jitter setter application claim",
    "previous_viewport_jitter_y": "finite JSON number actual origin metadata; no jitter setter application claim",
    "sr_jitter_x": "finite JSON number actual origin metadata; no jitter setter application claim",
    "sr_jitter_y": "finite JSON number actual origin metadata; no jitter setter application claim",
    "previous_sr_jitter_x": "finite JSON number actual origin metadata; no jitter setter application claim",
    "previous_sr_jitter_y": "finite JSON number actual origin metadata; no jitter setter application claim"
  },
  "source_jitter_metadata_scope": "Exactly those8 scalar keys inside source_jitter_metadata. Same names as actual P0 Native add_jitter and Helper finite_value parser, now nested in new message. No arrays/aliases; actual origin values, not arrival frame.",
  "job_identity_types": {
    "session": "safe ASCII exact CLI session",
    "generation": "uint exact0",
    "resource_set": "string SESSION-g0",
    "job": "uint ordinal0..max_jobs-1",
    "origin_source": "uint>0 actual retained raw source",
    "origin_epoch": "uint>0 Native edit cohort",
    "origin_history": "uint>0 Native edit history, NOT NR feature history",
    "params_version": "uint exact1",
    "profile_id": "string canonical exact"
  },
  "terminal_exact": {
    "common_required": "bootstrap_exact.common_required (without op/status which are stage-specific); exact four count keys below in every control/terminal message.",
    "count_fields": {
      "finalized_jobs": "uint0..max_jobs actual safe finalized counts",
      "captured_jobs": "uint0..max_jobs actual safe finalized counts",
      "obsolete_jobs": "uint0..max_jobs actual safe finalized counts",
      "skip_jobs": "uint0..max_jobs actual safe finalized counts"
    },
    "count_invariant": "finalized_jobs=captured_jobs+obsolete_jobs+skip_jobs; no fifth accepted_count/completed_count alias",
    "last_origin_fields": {
      "job": "uint equal finalized_jobs=N, terminal ordinal not next published input",
      "origin_source": "uint last safely finalized job source; exact0 iff N0",
      "origin_epoch": "uint last finalized job epoch; exact0 iff N0",
      "origin_history": "uint last finalized origin history; exact config initial_history_version1 iff N0",
      "source_frame_valid": "bool exact (N>0), only last finalized ORIGIN validity, not current game frame",
      "next_job": "uint exactN"
    },
    "N0": "No finalized NR job does not prove no Native GPU use: snapshots/cache may already have recorded work. Native release proof below depends on actual uses, independently of N.",
    "control_request_filename": "async-control-request-SESSION-g0.json",
    "control_ack_filename": "async-control-ack-SESSION-g0.json",
    "request_stage": {
      "op": "string exact control",
      "status": "string exact REQUESTED",
      "control_epoch": "uint>=1 strictly next accepted epoch",
      "action": "string exact stop",
      "reason": "string enable_off|exit|profile_change|budget",
      "completed_GPU": "bool exactfalse, host control request not GPU proof",
      "output_valid": "bool exactfalse",
      "model_valid": "bool exactfalse"
    },
    "request_when": "UI/render only requests stop in memory. Owner publishes request only after previously published INPUT has valid COMPLETE + real shared last-use CONSUMED/finalization. Stop cannot replace current consumed. No new job while stopping. N/counts/origin then exact and quiescent.",
    "ack_stage": {
      "op": "string exact control_ack",
      "status": "string exact STOP_READY",
      "control_epoch": "uint exact pending request epoch",
      "action": "string exact stop",
      "reason": "string exact request reason",
      "completed_GPU": "bool exacttrue, Helper current real creation/job fence observed",
      "output_valid": "bool exactfalse",
      "model_valid": "bool exactfalse",
      "helper_gpu_serial": "uint>0 actual Helper current completed serial"
    },
    "ack_rules": "All common/count/last-origin/control fields echo exact request. Only after Helper no active job, exact current CONSUMED observed and real current fence. Exact same epoch payload idempotent; conflicting/stale/skipped epoch rejected; no fake N=max_jobs requirement.",
    "native_release_filename": "async-native-released-SESSION-g0.json",
    "native_release_stage": {
      "op": "string exact release",
      "status": "string exact NATIVE_RELEASED",
      "control_epoch": "uint exact accepted STOP epoch",
      "terminal_reason": "string exact stop reason",
      "completed_GPU": "bool true iff last-use proof ACTUAL_NATIVE_GPU_LAST_USE; false iff NOT_REQUIRED_NO_NATIVE_USE",
      "output_valid": "bool exactfalse",
      "model_valid": "bool exactfalse",
      "native_last_use_tick": "uint actual max of source/peer-capture/cache/composite/merge/shared return dependencies;0 only if no recorded Native use ever",
      "native_last_use_observed_tick": "uint same held Native timeline actual observed completion;>=last use;0 if not required",
      "native_last_use_proof": "string ACTUAL_NATIVE_GPU_LAST_USE|NOT_REQUIRED_NO_NATIVE_USE",
      "native_imported_images_owned": "uint exact0 after actual destroy/bind memory cleanup at safe boundary",
      "native_raw_duplicate_fd_owned": "uint exact0 after actual import transfer/close",
      "native_origin_leases_owned": "uint exact0 after peer plus all Native last uses safe",
      "native_cache_owners_owned": "uint exact0 after actual all cache/composite last use cleanup"
    },
    "release_rules": "Actual Native source/current cache/merged frame GPU uses and shared ownership safe before freeing. Unknown timeline/peer state never emits NATIVE_RELEASED. Destruction counts are post-call proof, not permission to free by elapsed time.",
    "helper_retired_filename": "async-retired-SESSION-g0.json",
    "helper_retired_stage": {
      "op": "string exact retired",
      "status": "string exact RETIRED",
      "control_epoch": "uint exact accepted STOP epoch",
      "terminal_reason": "string exact stop reason",
      "completed_GPU": "bool exacttrue current Helper GPU fence known",
      "output_valid": "bool exactfalse",
      "model_valid": "bool exactfalse",
      "helper_gpu_serial": "uint actual completed serial",
      "shared_images_owned": "uint exact0 after actual4 views/images/allocations destroyed",
      "export_raw_fd_owned": "uint exact0",
      "normal_shared_retirement": "bool exacttrue"
    },
    "native_audited_filename": "async-native-audited-SESSION-g0.json",
    "native_audited_stage": {
      "op": "string exact audit",
      "status": "string exact FD_AUDITED",
      "control_epoch": "uint exact accepted STOP epoch",
      "terminal_reason": "string exact stop reason",
      "completed_GPU": "bool follows same native_last_use_proof as NATIVE_RELEASED",
      "output_valid": "bool exactfalse",
      "model_valid": "bool exactfalse",
      "native_last_use_tick": "uint exact release proof",
      "native_last_use_observed_tick": "uint same Native timeline >=target",
      "native_last_use_proof": "string exact release proof",
      "tracked_NR_FD_remaining": "uint exact0 actual targeted native import/duplicate + Helper original exported FD identities audit",
      "native_inventory": "string exact async-native-fd-final.json",
      "helper_inventory": "string exact async-wine-fd-final.json"
    },
    "FD_scope": "Keep full baseline/final inventories as evidence. tracked_NR_FD_remaining concerns exactly owned shared resources and actual FD identities; do not claim every unrelated renderer/system FD vanished. Existing strict source identity/retirement mechanism reused, no P0 rule relaxed.",
    "helper_result_filename": "async-wine-result.json",
    "helper_result_stage": {
      "op": "string exact result",
      "status": "string exact PASS",
      "control_epoch": "uint exact accepted STOP epoch",
      "terminal_reason": "string exact stop reason",
      "completed_GPU": "bool exacttrue actual final Helper GPU last use known",
      "output_valid": "bool exactfalse",
      "model_valid": "bool exactfalse",
      "normal_release": "bool exacttrue",
      "normal_shutdown": "bool exacttrue",
      "feature_create_calls": "uint exact1 actual720 feature",
      "feature_release_calls": "uint exact1 actual release after real GPU completion",
      "evaluate_calls_total": "uint exactN resident or0 skipEvaluate",
      "resolve_calls_total": "uint exactN resident or0 skipEvaluate",
      "NR_phase": "string UNVERIFIED",
      "native_present_count": "string REQUIRES_NATIVE_LEDGER_NOT_INFERRED"
    },
    "reap": "Native/Root actual waitpid must confirm exact private peer identity normal exit0 after result/real release. Result alone or process kill does not prove GPU completion. Unknown/failed job records FAIL/BLOCKED and held ownership separately, not normal terminal.",
    "zero_job_rules": "STOP_READY still requires actual Helper creation/export release fence. Native N0 uses last-use proof per actual recorded copies/cache, not N. source_frame_valid=false and origin_source/epoch0 do not erase true current game frame evidence."
  }
})PRESPEC";
