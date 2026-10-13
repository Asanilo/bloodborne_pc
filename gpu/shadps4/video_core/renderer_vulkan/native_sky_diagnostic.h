// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "native_async_state.h"
#include "native_edit_diagnostic_policy.h"
#include "native_pre_sr_policy.h"
#include "video_core/buffer_cache/buffer.h"
#include <memory>
#include <thread>
namespace Vulkan {
class Instance;class Scheduler;
class NativeSkyDiagnostic:public std::enable_shared_from_this<NativeSkyDiagnostic> {
public:
 static bool Enabled()noexcept;
 static bool EditEnabled()noexcept;
 struct EditContext {
  BbNativeAsync::EditDiagnosticIdentity identity;
  uint64_t native_input_tick=0,helper_serial=0;bool replay=false;
  std::string resource_set;
 };
 struct PreSrTag {};
 struct PreSrContext {
  BbPreSr::DiagnosticIdentity identity;
  uint64_t helper_serial=0;std::string resource_set;
  bool applied_sr_reset=false;
 };
 explicit NativeSkyDiagnostic(const Instance&,bool edit=false);
 NativeSkyDiagnostic(const Instance&,PreSrTag); // same managed staging/callback primitive, half720 only
 ~NativeSkyDiagnostic();
 void RecordOnce(Scheduler&,vk::Image raw,vk::Image composite,const BbNativeAsync::Frame&);
 bool RecordEditOnce(Scheduler&,vk::Image raw,vk::Image selected,
                     const BbNativeAsync::Frame&,const EditContext&);
 void EditMergeRecorded(Scheduler&,uint64_t actual_source);
 bool RecordPreSrOnce(Scheduler&,vk::Image original,vk::Image selected,
  const std::array<vk::ImageCreateInfo,2>&actual_ci,const BbNativeAsync::Frame&,const PreSrContext&);
 void PreSrMergeRecorded(Scheduler&,uint64_t actual_source);
 bool RetirePreSrFinished(); // real copy/SR/merge completion + joined writer, then staging API destruction
 bool Finished()const noexcept{return writer_finished.load(std::memory_order_acquire);}
 void JoinFinished(){if(Finished()&&writer.joinable())writer.join();}
private:
 void Run(std::stop_token);
 void RunPreSr(std::stop_token);
 std::array<std::unique_ptr<VideoCore::Buffer>,2> buffers;
 std::atomic<bool> recorded{false},gpu_ready{false},merge_ready{false};
 bool edit=false,merge_registered=false,pre_sr=false,pre_sr_retired=false;
 std::atomic<bool> writer_finished{false};PreSrContext pre_sr_context;
 EditContext edit_context;uint64_t merge_tick=0;
 BbNativeAsync::Frame frame;uint64_t target_tick=0;
 std::string run,session;
 std::jthread writer;
};
}
