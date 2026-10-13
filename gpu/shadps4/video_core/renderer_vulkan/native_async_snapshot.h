// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "native_async_owner.h"
#include "native_origin_lease.h"
#include "native_sky_gpu.h"
#include "native_origin_gpu.h"
#include "native_async_frontend.h"
#include "native_sky_diagnostic.h"
#include "common/types.h"
#include "video_core/texture_cache/image.h"
#include "video_core/renderer_vulkan/vk_common.h"
namespace Vulkan {
class Instance;class Scheduler;class NativeOriginOwner;class NativeAsyncRolling;
class NativeAsyncSnapshot {
public:
 bool Enabled()const noexcept{return BbNativeAsync::OptIn();}
 bool TryCaptureOrigin(const Instance&,Scheduler&,const std::array<vk::Image,3>&,
             const std::array<vk::ImageCreateInfo,3>&,BbNativeAsync::Frame);
 void PollReady(const Instance&,Scheduler&,bool supported);
 bool Armed()const noexcept;
 bool EffectSelected()const noexcept;
 unsigned EffectJob()const noexcept;
 vk::ImageView RecordAdvanceAndApply(Scheduler&,uint64_t current_source);
 bool TryRecordSparseInput(Scheduler&,const NativeOriginResources&,const NativeOriginIdentity&,
                           const std::shared_ptr<NativeOriginGpuProof>&);
 // Completion is a managed owner-verified wire record. Not a renderer file read.
 struct SparseCompletion {NativeOriginIdentity identity;uint64_t input_tick_echo=0,helper_gpu_serial=0;
  bool verified_wire=false,helper_GPU_complete=false,output_valid=false,model_valid=false,expected_skip=false;
  uint64_t evaluate_calls=0,evaluate_result=0,resolve_calls=0,model_reset=0;};
 enum class SparseDisposition {Unavailable,Captured,ObsoleteDiscard,ExpectedSkip};
 struct SparseReturn {vk::ImageView view{};SparseDisposition disposition=SparseDisposition::Unavailable;};
 SparseReturn TryRecordSparseComplete(Scheduler&,const NativeOriginResources&,const SparseCompletion&,
                                     uint64_t current_source,const std::shared_ptr<NativeOriginGpuProof>&);
 bool FinalizeSparseLease(const NativeOriginIdentity& id,const std::shared_ptr<NativeOriginGpuProof>& proof,bool consumed_published_by_owner)noexcept;
 void DrainUnavailable(Scheduler&);
 void MergeRecorded(Scheduler&,uint64_t current_source);
 void RetireByTick(Scheduler&);
 std::optional<BbNativeAsync::Frame> ReadyOrigin()const noexcept;
 // Recorded producer ordering, not CPU copyReady or permission to publish INPUT.
 std::optional<BbNativeAsync::Frame> RecordedOrigin(const Scheduler&)const noexcept;
 void Shutdown(Scheduler&);
 void RejectAssociation()noexcept;
 void Quarantine()noexcept;
 std::string Session()const;
 uint64_t Epoch()const noexcept;
 uint64_t OriginSource()const noexcept;
private:
 std::shared_ptr<NativeAsyncRolling>rolling;
 struct Retention {
  std::array<VideoCore::UniqueImage,3*BbNativeAsync::NodeCount> images;
  std::array<std::shared_ptr<BbNativeAsync::Node>,BbNativeAsync::NodeCount> records{};
  std::array<std::shared_ptr<BbNativeAsync::OriginLease>,BbNativeAsync::NodeCount> leases{};
  std::unique_ptr<NativeSkyGpu> sky;
  std::shared_ptr<NativeOriginQuiescence> quiescence;
  ~Retention(){
   if(quiescence){uint64_t actual=0;for(const auto& lease:leases)if(lease)actual=std::max(actual,lease->observed.load(std::memory_order_acquire));
    auto old=quiescence->observed_tick.load();while(old<actual&&!quiescence->observed_tick.compare_exchange_weak(old,actual)){}
   }
   sky.reset();for(auto& x:images)x=VideoCore::UniqueImage{};
   if(quiescence)quiescence->stopped_and_destroyed.store(true,std::memory_order_release);}
  bool AllUsesReady()const noexcept {for(const auto& p:leases)if(p&&!p->CanReuse())return false;return true;}

 };
 void Create(const Instance&);
 void TrackUse(Scheduler&);
 void PollPeer(Scheduler&,bool supported);
 static void HoldUnknown(std::shared_ptr<Retention>);
 std::shared_ptr<Retention> retained;
 std::shared_ptr<NativeOriginOwner> peer;
 std::unique_ptr<BbNativeAsync::Frontend> frontend;
 std::shared_ptr<NativeSkyDiagnostic> diagnostic,edit_diagnostic;
 std::optional<NativeSkyDiagnostic::EditContext> edit_anchor;
 bool snapshot_creation_attempted=false,peer_creation_attempted=false,peer_ready_observed=false;
 uint64_t maximum_native_use_tick=0;
 bool sparse_anchor=false,effect_selected=false;unsigned effect_job=0;
 uint64_t source_count=0;
 std::shared_ptr<BbNativeAsync::Owner> owner;
 std::optional<NativeOriginIdentity> pending_sparse;
 std::optional<BbNativeAsync::RecordedOrigin> recorded_origin;
 std::shared_ptr<NativeOriginGpuProof> sparse_input_proof,sparse_final_proof;
 uint64_t last_helper_serial=0;

 BbNativeAsync::Chain chain;
 bool rejected=false,stopping=false;uint64_t last_epoch=0,last_reported_ready=0;
};
}
