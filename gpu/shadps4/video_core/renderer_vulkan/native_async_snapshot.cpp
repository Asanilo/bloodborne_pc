// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_async_snapshot.h"
#include "native_async_rolling.h"
#include "native_origin_owner.h"
#include "vk_instance.h"
#include "vk_scheduler.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
namespace Vulkan {
std::string NativeAsyncSnapshot::Session()const {const char* p=std::getenv("BB_NR_NATIVE_ASYNC_SESSION");return p?p:"";}
bool NativeAsyncSnapshot::Armed()const noexcept {
 if(rolling)return rolling->Armed();
 return frontend&&frontend->Armed()&&(!BbNativeAsync::SceneFirst::Active()||
  (frontend->HelperStartRequested()&&peer&&peer_ready_observed));
}
void NativeAsyncSnapshot::Create(const Instance& instance) {
 snapshot_creation_attempted=true; // EVER, even partial creation before an exception.
 auto next=std::make_shared<Retention>();
 if(peer)next->quiescence=peer->Quiescence();
 if(NativeSkyDiagnostic::Enabled()&&!diagnostic)diagnostic=std::make_shared<NativeSkyDiagnostic>(instance);
 if(NativeSkyDiagnostic::EditEnabled()&&!edit_diagnostic)edit_diagnostic=std::make_shared<NativeSkyDiagnostic>(instance,true);
 for(unsigned i=0;i<next->images.size();++i){
  const bool color=i%3==0,depth=i%3==1;
  auto& image=next->images[i];image=VideoCore::UniqueImage(instance.GetDevice(),instance.GetAllocator());
  image.Create(vk::ImageCreateInfo{.imageType=vk::ImageType::e2D,
   .format=color?vk::Format::eR16G16B16A16Sfloat:depth?vk::Format::eD32SfloatS8Uint:vk::Format::eR16G16Sfloat,
   .extent={color?1920u:1280u,color?1080u:720u,1},.mipLevels=1,.arrayLayers=1,
   .samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,
   .usage=vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eTransferSrc|vk::ImageUsageFlagBits::eSampled,
   .initialLayout=vk::ImageLayout::eUndefined});
 }
 next->sky=std::make_unique<NativeSkyGpu>(instance);
 std::printf("NATIVE_SKY_RESOURCES rgba32f_history_banks=2 packets=5 packed_image_texel_bytes=%llu CPU_readback=0 cache_scale=1 jitter_delta=0 phase=UNVERIFIED model_calls=0\n",(unsigned long long)next->sky->ImageBytes());
 retained=std::move(next);owner=std::make_shared<BbNativeAsync::Owner>();
 std::printf("NATIVE_ASYNC_INIT session=%s origin1_links4=1 native_snapshots=15 hot_CPU_readback=0 sparse_adapter=%u localnoop=%u\n",Session().c_str(),bool(peer),!peer);
}
void NativeAsyncSnapshot::HoldUnknown(std::shared_ptr<Retention> hold) {
 // Deliberate sticky lifetime, not detached cleanup. No free/reuse under unknown
 // peer/GPU state; Root records abnormal process cleanup separately.
 static auto* mutex=new std::mutex;
 static auto* held=new std::vector<std::shared_ptr<Retention>>;
 std::scoped_lock lock(*mutex);held->push_back(std::move(hold));
}
void NativeAsyncSnapshot::TrackUse(Scheduler& scheduler){
 maximum_native_use_tick=std::max(maximum_native_use_tick,scheduler.CurrentTick());
 if(!peer)return;
 auto q=peer->Quiescence();const auto tick=scheduler.CurrentTick();
 auto old=q->last_use_tick.load();while(old<tick&&!q->last_use_tick.compare_exchange_weak(old,tick)){}
 scheduler.DeferPriorityOperation([q,tick]{
  auto old=q->observed_tick.load();while(old<tick&&!q->observed_tick.compare_exchange_weak(old,tick)){}
 });
}
void NativeAsyncSnapshot::PollPeer(Scheduler& scheduler,bool supported){
 if(!peer)return;
 using Phase=NativeOriginOwner::Phase;
 if(peer->State()==Phase::Ready)peer_ready_observed=true;
 if(BbNativeAsync::ExitRequested())peer->RequestStop(2);
 if(!supported&&retained)peer->RequestStop(3);
 if(peer->State()==Phase::Failed){Quarantine();return;}
 auto job=peer->TryJob();
 if(peer->State()==Phase::Finalized&&job){
  if(FinalizeSparseLease(job->id,job->final,job->consumed_published.load(std::memory_order_acquire)))
   peer->FinalizedAck(job);
 }
 if(peer->State()==Phase::Complete&&job&&(!supported||stopping||BbNativeAsync::ExitRequested())){
  auto final=TryRecordSparseComplete(scheduler,job->resources,job->complete,0,job->final);
  if(final.disposition!=SparseDisposition::Unavailable){TrackUse(scheduler);peer->FinalRecorded(job,final.disposition);}
 }
 if(peer->State()==Phase::StopRequested||peer->State()==Phase::AwaitNativeRelease||peer->State()==Phase::Retired)stopping=true;
}
void NativeAsyncSnapshot::PollReady(const Instance& instance,Scheduler& scheduler,bool supported) {
 if(Enabled()&&BbNativeAsync::Rolling::Enabled()){if(!rolling)rolling=std::make_shared<NativeAsyncRolling>();rolling->Poll(instance,scheduler,supported);return;}
 if(!Enabled())return;
 if(!frontend){frontend=std::make_unique<BbNativeAsync::Frontend>();std::printf("NATIVE_ASYNC_FRONTEND_READY session=%s scene_arm=REQUIRED availability_only=1 no_GPU_proof=1\n",Session().c_str());}
 if(BbNativeAsync::SceneFirst::Active()&&BbNativeAsync::ExitRequested()&&!peer){
  stopping=true;if(frontend)frontend->RequestStop();
  if(frontend&&frontend->Finished())frontend->JoinFinished();
  const BbNativeAsync::SceneFirst::NoPeerExitProof proof{
   .no_peer=!peer,.no_retained=!retained,.no_snapshot_owner=!owner,
   .no_pending=!pending_sparse&&!sparse_input_proof&&!sparse_final_proof&&!recorded_origin,
   .frontend_joined=frontend&&frontend->Joined(),.unknown=rejected,
   .peer_creation_attempted=peer_creation_attempted,.snapshot_creation_attempted=snapshot_creation_attempted,
   .copies_ever=source_count,.maximum_native_use_tick=maximum_native_use_tick,.chain_size=chain.Size()};
  if(BbNativeAsync::SceneFirst::CanExitWithoutPeer(proof)&&!BbNativeAsync::ExitComplete()){
   std::printf("NATIVE_ASYNC_EXIT_NO_PEER session=%s actual_no_owner=1 actual_no_snapshot=1 actual_no_pending=1 maximum_native_use_tick=0 preparation_joined=1 no_peer_created=1 no_NR_claim=1 scope=native_async_only\n",Session().c_str());
   BbNativeAsync::PublishExitComplete();
  }
  return;
 }
 const bool start_requested=!BbNativeAsync::SceneFirst::Active()||(frontend&&frontend->HelperStartRequested());
 const bool can_start=BbNativeAsync::SceneFirst::Active()?
  BbNativeAsync::SceneFirst::CanStartPeer(supported,start_requested,BbNativeAsync::ExitRequested()):supported;
 if(!peer&&can_start){
  auto candidate=std::make_shared<NativeOriginOwner>();
  if(candidate->Enabled()){peer_creation_attempted=true;peer=candidate;peer->Start(instance);}
 }
 if(BbNativeAsync::ExitRequested()&&frontend)frontend->RequestStop();
 PollPeer(scheduler,supported);
 if(owner&&owner->ReadyCount()!=last_reported_ready){
  last_reported_ready=owner->ReadyCount();
  std::printf("NATIVE_ASYNC_READY_OBSERVED session=%s copies_complete=%llu latest_source=%llu proof=actual_native_copy_tick_not_Helper_serial\n",Session().c_str(),
   (unsigned long long)last_reported_ready,(unsigned long long)owner->LatestReadySource());
 }
 const bool permanent=rejected||BbNativeAsync::ExitRequested()||(peer&&stopping);
 if(BbNativeAsync::NeedsHold(supported,bool(retained),BbNativeAsync::ExitRequested(),rejected))stopping=true;
 if(source_count>=120){stopping=true;if(peer)peer->RequestStop(4);}
 if(stopping&&(!retained||retained->AllUsesReady())){
  if(owner)owner->Stop();
  owner.reset();retained.reset();
  if(peer){if(!peer->Quiescence()->last_use_tick.load())peer->Quiescence()->stopped_and_destroyed.store(true);
   if(BbNativeAsync::ExitRequested()&&peer->ReapedNormally()&&(!frontend||frontend->Finished())){
    if(frontend)frontend->JoinFinished();BbNativeAsync::PublishExitComplete();
   }
   else if(BbNativeAsync::ExitRequested()&&!BbNativeAsync::ExitComplete()&&(!frontend||frontend->Finished())&&peer->SafeEmptyBootstrapExit(snapshot_creation_attempted||bool(retained),bool(pending_sparse),source_count)){
    peer->JoinFinished();if(frontend)frontend->JoinFinished();
    std::printf("NATIVE_ASYNC_EMPTY_BOOTSTRAP_EXIT_SAFE functional=FAIL normal_peer=0 actual_wait_status=%d imports_ever=0 snapshots_ever=0 native_async_GPU_use=0 scope=native_async_only\n",peer->ActualWaitStatus());
    BbNativeAsync::PublishAbnormalEmptyExitComplete();
   }
  }else if(BbNativeAsync::ExitRequested()&&(!frontend||frontend->Finished())){
   if(frontend)frontend->JoinFinished();BbNativeAsync::PublishExitComplete();
  }
  if(!permanent&&source_count<120){chain.Restart(true);stopping=false;}
 }
}
std::optional<BbNativeAsync::Frame> NativeAsyncSnapshot::ReadyOrigin()const noexcept {
 if(stopping||rejected||!retained||!retained->records[0]||!retained->records[0]->gpu_ready.load(std::memory_order_acquire))return {};
 return retained->records[0]->frame;
}
std::optional<BbNativeAsync::Frame> NativeAsyncSnapshot::RecordedOrigin(const Scheduler&s)const noexcept {
 if(stopping||rejected||!retained||!chain.Size()||!retained->records[0]||!recorded_origin)return {};
 const auto&f=retained->records[0]->frame;
 if(!recorded_origin->Matches(f,reinterpret_cast<uintptr_t>(&s),s.CurrentTick()))return {};
 return f;
}
vk::ImageView NativeAsyncSnapshot::RecordAdvanceAndApply(Scheduler& scheduler,uint64_t current_source) {
 if(rolling)return rolling->Advance(scheduler,current_source);
 if(stopping||rejected||!retained||!chain.Size())return {};
 const auto& origin=chain.Origin();
 if(!chain.CompleteTo(origin.source,current_source,origin.epoch,origin.history))return {};
 const unsigned index=chain.Size()-1;
 const auto record=retained->records[index];const auto lease=retained->leases[index];
 if(!record||!lease||record->frame.source!=current_source)return {};
 // Native producer and consumer commands are in the SAME actual timeline/queue.
 // Copy->sample barriers, not CPU copyReady, establish this recorded dependency.
 // No Helper ownership/read lease exists in this explicitly LOCAL_NOOP mode.
 effect_selected=false;
 lease->capture=index==0?scheduler.CurrentTick():0;lease->catchup=scheduler.CurrentTick();
 const std::array<vk::Image,3> images{vk::Image(retained->images[index*3]),vk::Image(retained->images[index*3+1]),vk::Image(retained->images[index*3+2])};
 auto view=retained->sky->Record(scheduler,index,images,index==0);
 if(index==0)recorded_origin=BbNativeAsync::RecordedOrigin{
  record->frame.source,record->frame.epoch,record->frame.history,record->frame.params,
  record->frame.copy_tick,scheduler.CurrentTick(),reinterpret_cast<uintptr_t>(&scheduler)};
 std::printf("NATIVE_SKY_RECORD origin=%llu current_source=%llu epoch=%llu history=%llu params=%llu age=%u Capture=%u Reproject=%u Pyramid=2 Apply=1 tick=%llu source_format=97 cache_format=109 Native_model_calls=0 mode=source-chain phase=UNVERIFIED\n",
  (unsigned long long)origin.source,(unsigned long long)current_source,(unsigned long long)origin.epoch,(unsigned long long)origin.history,(unsigned long long)origin.params,index,index==0,index!=0,(unsigned long long)scheduler.CurrentTick());
 TrackUse(scheduler);
 if(diagnostic)diagnostic->RecordOnce(scheduler,retained->sky->Converted(index)[0],
   retained->sky->Output(index),record->frame);
 if(peer){
  auto job=peer->TryJob();
  if(peer->State()==NativeOriginOwner::Phase::Complete&&job){
   auto final=TryRecordSparseComplete(scheduler,job->resources,job->complete,current_source,job->final);
   if(final.disposition!=SparseDisposition::Unavailable){
    TrackUse(scheduler);peer->FinalRecorded(job,final.disposition);
    if(final.view){view=final.view;sparse_anchor=true;effect_job=unsigned(job->id.job);
     // Only a verified, actually Captured sparse result can establish this
     // diagnostic origin. Cache applications must remain in the same lineage.
     NativeSkyDiagnostic::EditContext c;
     c.identity.origin=job->id.origin_source;c.identity.origin_epoch=job->id.origin_epoch;
     c.identity.origin_history=job->id.origin_history;c.identity.origin_params=job->id.params_version;
     c.identity.job=job->id.job;c.identity.resident=true;
     c.identity.captured=final.disposition==SparseDisposition::Captured;
     c.native_input_tick=job->complete.input_tick_echo;c.helper_serial=job->complete.helper_gpu_serial;
     c.resource_set=job->id.resource_set;edit_anchor=c;
    }
   }
  }
  if(peer->State()==NativeOriginOwner::Phase::Ready){
   if(auto origin=RecordedOrigin(scheduler))if(auto job=peer->TryReserve(*origin)){
    if(TryRecordSparseInput(scheduler,job->resources,job->id,job->input)){TrackUse(scheduler);peer->InputRecorded(job);}
    else peer->CancelReservation(job);
   }
  }
  if(!sparse_anchor)return {};
  effect_selected=true;
 }
 // AFTER sparse completion/catchup: resolve the final returned view to its
 // actual image. The current converted SR is never the old origin's Color.
 if(edit_diagnostic&&peer&&effect_selected&&edit_anchor){
  auto c=*edit_anchor;c.identity.current=record->frame.source;
  c.identity.current_epoch=record->frame.epoch;c.identity.current_history=record->frame.history;
  c.identity.current_params=record->frame.params;
  bool replay=false;auto selected=retained->sky->SelectedOutput(index,view,replay);
  c.identity.actual_view_image_match=bool(selected);c.replay=replay;
  if(c.identity.origin==origin.source&&c.identity.origin_epoch==origin.epoch&&
     c.identity.origin_history==origin.history&&c.identity.origin_params==origin.params)
   edit_diagnostic->RecordEditOnce(scheduler,retained->sky->Converted(index)[0],selected,record->frame,c);
 }
 return view;
}

bool NativeAsyncSnapshot::TryRecordSparseInput(Scheduler& scheduler,const NativeOriginResources& resources,
 const NativeOriginIdentity& id,const std::shared_ptr<NativeOriginGpuProof>& proof) {
 if(stopping||rejected||pending_sparse||!retained||!chain.Size()||!proof||proof->native_target_tick||
    !id.Matches(chain.Origin())||!resources.Valid(id)||!recorded_origin||
    !recorded_origin->Matches(retained->records[0]->frame,reinterpret_cast<uintptr_t>(&scheduler),scheduler.CurrentTick()))return false;
 auto lease=retained->leases[0];if(!lease||lease->peer_read||lease->unknown)return false;
 // The lease is installed BEFORE any shared writes. An INPUT is not publishable
 // until proof callback reports this native copied/released target actually done.
 lease->peer_read=true;pending_sparse=id;sparse_input_proof=proof;
 RecordOriginInput(scheduler,resources,id,retained->sky->ConvertedOrigin(),proof);
 std::printf("NATIVE_ORIGIN_INPUT_RECORDED origin=%llu job=%llu source_copy_tick=%llu source_conversion_tick=%llu input_target_tick=%llu producer=same_native_scheduler_recorded actual_input_GPU_complete=NOT_YET_OBSERVED no_extra_flush=1\n",
  (unsigned long long)id.origin_source,(unsigned long long)id.job,
  (unsigned long long)recorded_origin->copy_tick,(unsigned long long)recorded_origin->conversion_tick,
  (unsigned long long)proof->native_target_tick);
 lease->capture=std::max(lease->capture,proof->native_target_tick);
 scheduler.DeferPriorityOperation([hold=retained,lease,proof]{lease->CompleteNative(proof->native_target_tick);});
 return true;
}
NativeAsyncSnapshot::SparseReturn NativeAsyncSnapshot::TryRecordSparseComplete(Scheduler& scheduler,
 const NativeOriginResources& resources,const SparseCompletion& complete,uint64_t current_source,
 const std::shared_ptr<NativeOriginGpuProof>& final_proof) {
 if(sparse_final_proof||!pending_sparse||!sparse_input_proof||!retained||!final_proof||final_proof->native_target_tick)return {};
 const auto& id=complete.identity;const auto& expected=*pending_sparse;
 if(!complete.verified_wire||!complete.helper_GPU_complete||!complete.helper_gpu_serial||complete.helper_gpu_serial<=last_helper_serial||
    !sparse_input_proof->native_GPU_complete.load(std::memory_order_acquire)||complete.input_tick_echo!=sparse_input_proof->native_target_tick||
    id.session!=expected.session||id.resource_set!=expected.resource_set||id.generation!=expected.generation||id.job!=expected.job||
    id.origin_source!=expected.origin_source||id.origin_epoch!=expected.origin_epoch||id.origin_history!=expected.origin_history||id.params_version!=expected.params_version||
    !resources.Valid(id)||!id.Matches(chain.Origin())||complete.model_reset!=1)throw std::runtime_error("Native sparse exact completion/native input identity mismatch");
 const bool valid=complete.output_valid&&complete.model_valid&&complete.evaluate_calls==1&&complete.evaluate_result==1&&complete.resolve_calls==1;
 const bool skip=complete.expected_skip&&!complete.output_valid&&!complete.model_valid&&!complete.evaluate_calls&&!complete.evaluate_result&&!complete.resolve_calls;
 if(!valid&&!skip)throw std::runtime_error("Native sparse normal model predicate rejected unexpected output");
 const bool current=!stopping&&!rejected&&current_source>0&&chain.CompleteTo(id.origin_source,current_source,id.origin_epoch,id.origin_history);
 if(valid&&current&&retained->sky->SparseScratchBusy())return {};
 AcquireOriginOutput(scheduler,resources,id);
 SparseReturn result;result.disposition=skip?SparseDisposition::ExpectedSkip:SparseDisposition::ObsoleteDiscard;
 auto hold=retained;const auto lease=hold->leases[0];
 if(valid&&current){result.view=hold->sky->RecordSparseCatchup(scheduler,chain.Size()-1,resources.views[3]);
  if(!result.view)throw std::runtime_error("Native sparse catchup reservation failed after output acquire");
  result.disposition=SparseDisposition::Captured;
 }
 ReleaseOriginOutput(scheduler,resources,id,final_proof);
 const auto tick=final_proof->native_target_tick;
 for(const auto& used:hold->leases)if(used)used->catchup=std::max(used->catchup,tick);
 const auto leases=hold->leases;
 // No Helper serial is compared to tick. Peer bit is renderer-only; owner receives
 // the proof for CONSUMED, then passes a finalized memory event back to renderer.
 scheduler.DeferPriorityOperation([hold,leases,tick]{for(const auto& used:leases)if(used)used->CompleteNative(tick);hold->sky->CompleteSparseNativeUse();});
 last_helper_serial=complete.helper_gpu_serial;sparse_final_proof=final_proof;
 return result;
}

bool NativeAsyncSnapshot::FinalizeSparseLease(const NativeOriginIdentity& id,const std::shared_ptr<NativeOriginGpuProof>& proof,bool consumed_published_by_owner)noexcept {
 if(!consumed_published_by_owner||proof!=sparse_final_proof||!pending_sparse||!retained||!proof||!proof->native_target_tick||!proof->native_GPU_complete.load(std::memory_order_acquire))return false;
 const auto& e=*pending_sparse;
 if(id.session!=e.session||id.resource_set!=e.resource_set||id.generation!=e.generation||id.job!=e.job||id.origin_source!=e.origin_source||id.origin_epoch!=e.origin_epoch||id.origin_history!=e.origin_history||id.params_version!=e.params_version)return false;
 const auto lease=retained->leases[0];if(!lease||lease->unknown||proof->native_target_tick<lease->capture||proof->native_target_tick<lease->catchup)return false;
 lease->peer_read=false;pending_sparse.reset();sparse_input_proof.reset();sparse_final_proof.reset();return true;
}
void NativeAsyncSnapshot::DrainUnavailable(Scheduler& s) {
 if(rolling){rolling->Drain(s);return;}
 if(!peer)return;
 auto job=peer->TryJob();
 if(job&&peer->State()==NativeOriginOwner::Phase::Complete){
  auto final=TryRecordSparseComplete(s,job->resources,job->complete,0,job->final);
  if(final.disposition!=SparseDisposition::Unavailable){TrackUse(s);peer->FinalRecorded(job,final.disposition);}
 }
}
void NativeAsyncSnapshot::MergeRecorded(Scheduler& scheduler,uint64_t source) {
 if(rolling){rolling->Merge(scheduler,source);return;}
 if(!retained||chain.LastSource()!=source)return;
 TrackUse(scheduler);
 const auto tick=scheduler.CurrentTick();auto hold=retained;
 if(edit_diagnostic)edit_diagnostic->EditMergeRecorded(scheduler,source);
 // Every retained origin/link and both history banks have participated in this
 // queue-ordered cache chain. Keep them through the REAL merge final-use tick.
 for(const auto& lease:hold->leases)if(lease)lease->merge=tick;
 const auto leases=hold->leases; // immutable ownership snapshot, not mutable slot array
 scheduler.DeferPriorityOperation([hold,leases,tick]{for(const auto& lease:leases)if(lease)lease->CompleteNative(tick);});
}
void NativeAsyncSnapshot::RetireByTick(Scheduler& scheduler) {Shutdown(scheduler);}
void NativeAsyncSnapshot::Shutdown(Scheduler& scheduler) {
 if(rolling){rolling->Shutdown(scheduler);return;}
 stopping=true;
 if(BbNativeAsync::SceneFirst::Active()&&frontend)frontend->RequestStop();
 if(peer)peer->RequestStop(2);
 if(owner)owner->Stop();
 owner.reset();
 if(retained){
  if(pending_sparse||rejected){HoldUnknown(std::move(retained));return;}
  auto hold=std::move(retained);
  scheduler.DeferOperation([hold=std::move(hold)]{});
 }
}
bool NativeAsyncSnapshot::TryCaptureOrigin(const Instance& instance,Scheduler& scheduler,
 const std::array<vk::Image,3>& source,const std::array<vk::ImageCreateInfo,3>& ci,BbNativeAsync::Frame meta) {
 if(rolling)return rolling->Capture(instance,scheduler,source,ci,meta);
 if(!Enabled()||!Armed()||source_count>=120||stopping||rejected||!BbNativeAsync::Finite(meta))return false;
 // DEPTH-only GENERAL barriers on format130 require the actually enabled feature.
 if(!instance.IsSeparateDepthStencilLayoutsEnabled())return false;
 for(unsigned i=0;i<3;++i){const auto& c=ci[i];
  const auto format=i==0?vk::Format::eR16G16B16A16Sfloat:i==1?vk::Format::eD32SfloatS8Uint:vk::Format::eR16G16Sfloat;
  if(!source[i]||c.format!=format||c.extent.width!=(i?1280u:1920u)||c.extent.height!=(i?720u:1080u)||c.extent.depth!=1||
     c.samples!=vk::SampleCountFlagBits::e1||c.mipLevels!=1||c.arrayLayers!=1||!(c.usage&vk::ImageUsageFlagBits::eTransferSrc))return false;
 }
 if(!retained)Create(instance);
 auto decision=chain.Classify(meta);
 if(decision==BbNativeAsync::Chain::Decision::Reject)return false;
 if(decision==BbNativeAsync::Chain::Decision::Full||decision==BbNativeAsync::Chain::Decision::Discontinuity){
  if(!retained->AllUsesReady()){
   // A delayed callback is not itself a GPU dependency. Observe ONLY this
   // Scheduler's actual completed timeline; never wait, Flush, or use Helper
   // serials. Peer-read/unknown flags remain independent and cannot be cleared.
   uint64_t target=0;bool peer_blocked=false;
   for(const auto& lease:retained->leases)if(lease){
    target=std::max(target,lease->LastUse());peer_blocked|=lease->peer_read||lease->unknown;
   }
   if(!peer_blocked&&target&&target<scheduler.CurrentTick()&&scheduler.IsFree(target)){
    const auto observed=scheduler.GetWorkSemaphore()->KnownGpuTick();
    for(const auto& lease:retained->leases)if(lease)lease->CompleteNative(observed);
    std::printf("NATIVE_ASYNC_REUSE_GPU_OBSERVED prior_origin=%llu target_tick=%llu observed_tick=%llu proof=actual_same_Native_timeline_counter callback_pending_allowed=1 peer_and_unknown_gates=UNCHANGED\n",
     (unsigned long long)chain.Origin().source,(unsigned long long)target,(unsigned long long)observed);
   }
  }
  if(!retained->AllUsesReady())return false; // Real GPU OR peer/unknown incomplete: current SR only.
  chain.Restart(decision==BbNativeAsync::Chain::Decision::Discontinuity);
  for(auto& record:retained->records)record.reset();
  for(auto& lease:retained->leases)lease.reset();
  retained->sky->Restart();sparse_anchor=false;edit_anchor.reset();recorded_origin.reset();
 }
 const unsigned index=chain.Size();meta.copy_tick=scheduler.CurrentTick();
 BbNativeAsync::Frame next=meta; // commit chain only if bounded owner reservation succeeds
 auto candidate=chain; if(!candidate.Append(next))return false;
 auto record=std::make_shared<BbNativeAsync::Node>(next);
 if(!owner->TryPost(record))return false;
 retained->records[index]=record; // unknown partial recording stays held
 auto lease=std::make_shared<BbNativeAsync::OriginLease>();lease->copy=next.copy_tick;retained->leases[index]=lease;
 auto cmd=scheduler.CommandBuffer();
 for(unsigned role=0;role<3;++role){
  const unsigned target=3*index+role;
  const auto aspect=role==1?vk::ImageAspectFlagBits::eDepth:vk::ImageAspectFlagBits::eColor;
  const vk::ImageMemoryBarrier2 src{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,.srcAccessMask=vk::AccessFlagBits2::eMemoryWrite,
   .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,.dstAccessMask=vk::AccessFlagBits2::eTransferRead,
   .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
   .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
   .image=source[role],.subresourceRange={aspect,0,1,0,1}};
  const auto dest=vk::Image(retained->images[target]);
  const vk::ImageMemoryBarrier2 dst{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
   .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
   .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,.dstAccessMask=vk::AccessFlagBits2::eTransferWrite,
   .oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eGeneral,
   .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
   .image=dest,.subresourceRange={aspect,0,1,0,1}};
  const std::array barriers{src,dst};cmd.pipelineBarrier2({.imageMemoryBarrierCount=2,.pImageMemoryBarriers=barriers.data()});
  const vk::ImageCopy copy{.srcSubresource={aspect,0,0,1},.dstSubresource={aspect,0,0,1},.extent={role?1280u:1920u,role?720u:1080u,1}};
  cmd.copyImage(source[role],vk::ImageLayout::eGeneral,dest,vk::ImageLayout::eGeneral,copy);
  const vk::ImageMemoryBarrier2 ready{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,.srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
   .dstStageMask=vk::PipelineStageFlagBits2::eComputeShader|vk::PipelineStageFlagBits2::eTransfer,
   .dstAccessMask=vk::AccessFlagBits2::eShaderRead|vk::AccessFlagBits2::eTransferRead,
   .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
   .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
   .image=dest,.subresourceRange={aspect,0,1,0,1}};
  cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&ready});
  auto restore=src;restore.srcStageMask=vk::PipelineStageFlagBits2::eTransfer;restore.srcAccessMask=vk::AccessFlagBits2::eTransferRead;
  restore.dstStageMask=vk::PipelineStageFlagBits2::eAllCommands;restore.dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite;
  cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&restore});
 }
 chain=candidate;last_epoch=next.epoch;++source_count;TrackUse(scheduler);
 scheduler.DeferPriorityOperation([hold=retained,record,lease,notify=owner]{
  lease->CompleteNative(record->frame.copy_tick);
  record->gpu_ready.store(true,std::memory_order_release);notify->Signal();
 });
 std::printf("NATIVE_ASYNC_SOURCE session=%s origin=%llu source=%llu previous=%llu epoch=%llu history=%llu link=%u sr_reset=%u copy_tick=%llu viewport=%.9g,%.9g previous_viewport=%.9g,%.9g sr_jitter=%.9g,%.9g previous_sr_jitter=%.9g,%.9g source_edited=0 GPU_ready=NOT_YET_OBSERVED current_SR=1\n",
  Session().c_str(),(unsigned long long)chain.Origin().source,(unsigned long long)next.source,(unsigned long long)next.previous,
  (unsigned long long)next.epoch,(unsigned long long)next.history,index,next.sr_reset,(unsigned long long)next.copy_tick,
  next.viewport[0],next.viewport[1],next.previous_viewport[0],next.previous_viewport[1],next.sr_jitter[0],next.sr_jitter[1],next.previous_sr_jitter[0],next.previous_sr_jitter[1]);
 return true;
}
}

namespace Vulkan {
bool NativeAsyncSnapshot::EffectSelected()const noexcept{return rolling?rolling->Effect():effect_selected;}
unsigned NativeAsyncSnapshot::EffectJob()const noexcept{return rolling?rolling->Job():effect_job;}
uint64_t NativeAsyncSnapshot::Epoch()const noexcept{return rolling?rolling->Epoch():last_epoch;}
uint64_t NativeAsyncSnapshot::OriginSource()const noexcept{return rolling?rolling->Origin():(chain.Size()?chain.Origin().source:0);}
void NativeAsyncSnapshot::RejectAssociation()noexcept{if(rolling){rolling->Quarantine();return;}rejected=true;}
void NativeAsyncSnapshot::Quarantine()noexcept{if(rolling){rolling->Quarantine();return;}rejected=true;stopping=true;if(retained)for(const auto&p:retained->leases)if(p)p->unknown=true;}
}
