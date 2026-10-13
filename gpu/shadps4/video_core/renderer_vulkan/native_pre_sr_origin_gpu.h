// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "native_origin_gpu.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include <memory>
#include <string>
namespace Vulkan {
class Scheduler;
enum class PreSrDisposition {Unavailable,Captured,ObsoleteDiscard,ExpectedSkip};
struct NativePreSrCompletion {NativeOriginIdentity identity;uint64_t input_tick_echo=0,helper_gpu_serial=0;
 bool verified_wire=false,helper_GPU_complete=false,output_valid=false,model_valid=false,expected_skip=false;
 uint64_t evaluate_calls=0,evaluate_result=0,resolve_calls=0,model_reset=0;};

// Borrowed API handles; owner must have performed the canonical UUID/PID/FD/CI
// import audit. Shared ownership is lifetime ONLY, not release/fence authority.
struct NativePreSrResources {
 std::string session,resource_set;
 std::array<vk::Image,4> images{};
 std::array<vk::ImageView,4> views{};
 std::array<vk::ImageCreateInfo,4> infos{};
 std::shared_ptr<void> import_owner;
 bool actual_import_fd_retired=false;
 uint32_t native_queue_family=VK_QUEUE_FAMILY_IGNORED;
 bool Valid(const NativeOriginIdentity&)const noexcept;
};
// Renderer commands ONLY. No file IO, queue submission, CPU waiting or external
// semaphore machinery. Native serial and Helper serial have separate domains.
void RecordPreSrInput(Scheduler&,const NativePreSrResources&,
                       const NativeOriginIdentity&,const std::array<vk::Image,3>& converted,
                       const std::shared_ptr<NativeOriginGpuProof>&);
void AcquirePreSrOutput(Scheduler&,const NativePreSrResources&,const NativeOriginIdentity&);
void ReleasePreSrOutput(Scheduler&,const NativePreSrResources&,const NativeOriginIdentity&,
                         const std::shared_ptr<NativeOriginGpuProof>&);
}
