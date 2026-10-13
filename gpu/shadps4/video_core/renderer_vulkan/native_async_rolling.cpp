// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_async_rolling.h"
#include "native_async_scene_first.h"
#include "vk_instance.h"
#include "vk_scheduler.h"
#include <vk_mem_alloc.h>
#include <cstdio>
#include <mutex>
#include <vector>
namespace Vulkan {
namespace R=BbNativeAsync::Rolling;
namespace {
void Max(std::atomic<uint64_t>&v,uint64_t n){auto old=v.load();while(old<n&&!v.compare_exchange_weak(old,n)){} }
}
std::string NativeAsyncRolling::Session()const{const char*p=std::getenv("BB_NR_NATIVE_ASYNC_SESSION");return p?p:"";}
NativeAsyncRolling::Retention::~Retention(){
 if(q)Max(q->observed_tick,observed.load(std::memory_order_acquire));
 sky.reset();for(auto&image:images)image=VideoCore::UniqueImage{};
 if(q)q->stopped_and_destroyed.store(true,std::memory_order_release);
}
std::array<std::optional<NativeAsyncRolling::Frame>,R::Capacity>NativeAsyncRolling::Frames()const{
 std::array<std::optional<Frame>,R::Capacity>x{};if(retained)for(unsigned k=0;k<R::Capacity;k++)if(retained->records[k])x[k]=retained->records[k]->frame;return x;
}
bool NativeAsyncRolling::Armed()const noexcept{return frontend&&frontend->Armed()&&peer&&peer_ready;}
void NativeAsyncRolling::Create(const Instance&i){
 created=true;auto next=std::make_shared<Retention>();next->q=peer->Quiescence();
 uint64_t raw_bytes=0,allocated=0;
 for(unsigned k=0;k<next->images.size();k++){
  const unsigned role=k%3;auto&image=next->images[k];image=VideoCore::UniqueImage(i.GetDevice(),i.GetAllocator());
  image.Create({.imageType=vk::ImageType::e2D,.format=role==0?vk::Format::eR16G16B16A16Sfloat:role==1?vk::Format::eD32SfloatS8Uint:vk::Format::eR16G16Sfloat,
   .extent={role?1280u:1920u,role?720u:1080u,1},.mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,
   .usage=vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eTransferSrc|vk::ImageUsageFlagBits::eSampled,.initialLayout=vk::ImageLayout::eUndefined});
  raw_bytes+=uint64_t(role?1280:1920)*(role?720:1080)*(role==2?4:8);VmaAllocationInfo info{};vmaGetAllocationInfo(i.GetAllocator(),image.allocation,&info);allocated+=info.size;
 }
 next->sky=std::make_unique<NativeSkyRollingGpu>(i);allocated+=next->sky->AllocatedImageBytes();
 std::printf("NATIVE_ASYNC_ROLLING_INIT session=%s physical_packets=7 logical_links=4 history_banks=2 packed_texel_bytes=%llu image_allocation_bytes=%llu allocation_scope=VMA_image_regions_not_model_or_driver_total hot_CPU_readback=0 shared_jobs=1\n",Session().c_str(),(unsigned long long)(raw_bytes+next->sky->ImageBytes()),(unsigned long long)allocated);
 retained=std::move(next);if(NativeSkyDiagnostic::EditEnabled())diagnostic=std::make_shared<NativeSkyDiagnostic>(i,true);
}
void NativeAsyncRolling::Use(Scheduler&s,const Span&span){
 const auto t=s.CurrentTick();max_tick=std::max(max_tick,t);auto hold=retained;Max(peer->Quiescence()->last_use_tick,t);
 std::array<std::shared_ptr<BbNativeAsync::OriginLease>,BbNativeAsync::NodeCount>leases{};
 for(unsigned k=0;k<span.size;k++)leases[k]=hold->leases[span.slots[k]];
 s.DeferPriorityOperation([hold,leases,t]{for(const auto&lease:leases)if(lease)lease->CompleteNative(t);Max(hold->observed,t);Max(hold->q->observed_tick,t);});
}
void NativeAsyncRolling::Observe(Scheduler&s){
 if(!retained||!max_tick||retained->observed.load()>=max_tick||max_tick>=s.CurrentTick()||!s.IsFree(max_tick))return;
 const auto actual=s.GetWorkSemaphore()->KnownGpuTick();Max(retained->observed,actual);Max(retained->q->observed_tick,actual);
 for(const auto&lease:retained->leases)if(lease)lease->CompleteNative(actual);
}
bool NativeAsyncRolling::AllReady()const{
 if(!retained)return true;if(retained->observed.load()<std::max(max_tick,retained->sky->LastUse()))return false;
 for(const auto&lease:retained->leases)if(lease&&!lease->CanReuse())return false;return true;
}
bool NativeAsyncRolling::Pinned(unsigned slot,uint64_t current)const{
 if(!retained->records[slot])return false;const auto&f=retained->records[slot]->frame;
 return (pending&&(slot==pending_slot||R::PinForLineage(f,pending->origin,current)))||
  (active&&R::PinForLineage(f,active->origin,current));
}
void NativeAsyncRolling::Finalize(){
 if(!pending||peer->State()!=NativeOriginOwner::Phase::Finalized||!pending->consumed_published.load(std::memory_order_acquire)||!pending->final->native_GPU_complete.load(std::memory_order_acquire))return;
 auto lease=retained->leases[pending_slot];if(!lease||lease->unknown||pending->final->native_target_tick<lease->capture)return;
 lease->peer_read=false;peer->FinalizedAck(pending);pending.reset();
}
void NativeAsyncRolling::Poll(const Instance&i,Scheduler&s,bool supported){
 selected=false;
 if(!frontend)frontend=std::make_unique<BbNativeAsync::Frontend>();
 const bool exiting=BbNativeAsync::ExitRequested();if(exiting)frontend->RequestStop();
 if(!peer&&exiting){if(frontend->Finished())frontend->JoinFinished();
  const BbNativeAsync::SceneFirst::NoPeerExitProof proof{.no_peer=true,.no_retained=!retained,.no_snapshot_owner=true,.no_pending=!pending,.frontend_joined=frontend->Joined(),.unknown=unknown,.peer_creation_attempted=peer_attempted,.snapshot_creation_attempted=created,.copies_ever=copies,.maximum_native_use_tick=max_tick};
  if(BbNativeAsync::SceneFirst::CanExitWithoutPeer(proof)&&!BbNativeAsync::ExitComplete()){
   std::printf("NATIVE_ASYNC_EXIT_NO_PEER session=%s actual_no_owner=1 actual_no_snapshot=1 actual_no_pending=1 maximum_native_use_tick=0 preparation_joined=1 no_peer_created=1 no_NR_claim=1 scope=native_async_only rolling=1\n",Session().c_str());
   BbNativeAsync::PublishExitComplete();
  }
  return;
 }
 if(!peer&&supported&&frontend->HelperStartRequested()&&!exiting){peer_attempted=true;peer=std::make_shared<NativeOriginOwner>();peer->Start(i);}
 if(!peer)return;using Phase=NativeOriginOwner::Phase;
 if(peer->State()==Phase::Ready)peer_ready=true;
 if(exiting||(!supported&&created)){stopping=true;peer->RequestStop(exiting?2:3);frontend->RequestStop();}
 if(copies>=120){stopping=true;peer->RequestStop(4);}
 if(peer->State()==Phase::Failed){
  if(exiting&&!created&&!pending&&!max_tick&&peer->SafeEmptyBootstrapExit(false,false,0)&&frontend->Finished()){
   peer->JoinFinished();frontend->JoinFinished();BbNativeAsync::PublishAbnormalEmptyExitComplete();return;
  }
  Quarantine();return;
 }
 Finalize();Observe(s);
 if(stopping&&pending&&peer->State()==Phase::Complete)Complete(s,0);
 if(peer->State()==Phase::StopRequested||peer->State()==Phase::AwaitNativeRelease||peer->State()==Phase::Retired)stopping=true;
 if(stopping&&!unknown&&!pending&&AllReady()){
  retained.reset();if(!max_tick)peer->Quiescence()->stopped_and_destroyed.store(true,std::memory_order_release);
  if(exiting&&peer->ReapedNormally()&&frontend->Finished()){peer->JoinFinished();frontend->JoinFinished();BbNativeAsync::PublishExitComplete();}
 }
}
bool NativeAsyncRolling::Capture(const Instance&i,Scheduler&s,const std::array<vk::Image,3>&source,const std::array<vk::ImageCreateInfo,3>&ci,Frame f){
 selected=false;if(!Armed()||stopping||unknown||copies>=120||!BbNativeAsync::Finite(f)||f.source<=last_seen)return false;
 if(!i.IsSeparateDepthStencilLayoutsEnabled())return false;
 for(unsigned k=0;k<3;k++){const auto&c=ci[k];if(!source[k]||c.format!=(k==0?vk::Format::eR16G16B16A16Sfloat:k==1?vk::Format::eD32SfloatS8Uint:vk::Format::eR16G16Sfloat)||c.extent.width!=(k?1280u:1920u)||c.extent.height!=(k?720u:1080u)||c.extent.depth!=1||c.samples!=vk::SampleCountFlagBits::e1||c.mipLevels!=1||c.arrayLayers!=1||!(c.usage&vk::ImageUsageFlagBits::eTransferSrc)){active.reset();return false;}}
 if(last_seen&&(f.sr_reset||f.source!=last_seen+1)){++epoch;++history;active.reset();if(retained)retained->sky->Invalidate();}
 f.previous=last_seen;last_seen=f.source;f.epoch=epoch;f.history=history;f.copy_tick=s.CurrentTick();current_source=f.source;
 if(active&&!R::AgeValid(active->id.origin_source,f.source)){active.reset();if(retained)retained->sky->Invalidate();}
 if(!retained)Create(i);Observe(s);const auto observed=retained->observed.load(std::memory_order_acquire);
 unsigned slot=R::Capacity;
 for(unsigned k=0;k<R::Capacity;k++)if(!retained->records[k]){slot=k;break;}
 if(slot==R::Capacity)for(unsigned k=0;k<R::Capacity;k++){
  const auto&lease=retained->leases[k];const auto use=std::max(lease->LastUse(),retained->sky->PacketLastUse(k));
  if(R::CanReuse(use,observed,lease->peer_read,lease->unknown,Pinned(k,f.source))&&(slot==R::Capacity||retained->records[k]->frame.source<retained->records[slot]->frame.source))slot=k;
 }
 if(slot==R::Capacity){active.reset();retained->sky->Invalidate();std::printf("NATIVE_ASYNC_ROLLING_BUSY source=%llu current_SR=1 chain_invalidated=1 physical_cap=7 no_wait=1\n",(unsigned long long)f.source);return false;}
 current_slot=slot;auto record=std::make_shared<BbNativeAsync::Node>(f);auto lease=std::make_shared<BbNativeAsync::OriginLease>();lease->copy=f.copy_tick;retained->records[slot]=record;retained->leases[slot]=lease;
 auto cmd=s.CommandBuffer();
 for(unsigned role=0;role<3;role++){
  const auto aspect=role==1?vk::ImageAspectFlagBits::eDepth:vk::ImageAspectFlagBits::eColor;const auto dest=vk::Image(retained->images[3*slot+role]);
  const vk::ImageMemoryBarrier2 src{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,.srcAccessMask=vk::AccessFlagBits2::eMemoryWrite,.dstStageMask=vk::PipelineStageFlagBits2::eTransfer,.dstAccessMask=vk::AccessFlagBits2::eTransferRead,.oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=source[role],.subresourceRange={aspect,0,1,0,1}};
  const vk::ImageMemoryBarrier2 dst{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,.srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,.dstStageMask=vk::PipelineStageFlagBits2::eTransfer,.dstAccessMask=vk::AccessFlagBits2::eTransferWrite,.oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eGeneral,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=dest,.subresourceRange={aspect,0,1,0,1}};
  const std::array barriers{src,dst};cmd.pipelineBarrier2({.imageMemoryBarrierCount=2,.pImageMemoryBarriers=barriers.data()});
  cmd.copyImage(source[role],vk::ImageLayout::eGeneral,dest,vk::ImageLayout::eGeneral,vk::ImageCopy{.srcSubresource={aspect,0,0,1},.dstSubresource={aspect,0,0,1},.extent={role?1280u:1920u,role?720u:1080u,1}});
  auto ready=dst;ready.srcStageMask=vk::PipelineStageFlagBits2::eTransfer;ready.srcAccessMask=vk::AccessFlagBits2::eTransferWrite;ready.dstStageMask=vk::PipelineStageFlagBits2::eComputeShader;ready.dstAccessMask=vk::AccessFlagBits2::eShaderRead;ready.oldLayout=vk::ImageLayout::eGeneral;cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&ready});
  auto restore=src;restore.srcStageMask=vk::PipelineStageFlagBits2::eTransfer;restore.srcAccessMask=vk::AccessFlagBits2::eTransferRead;restore.dstStageMask=vk::PipelineStageFlagBits2::eAllCommands;restore.dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite;cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&restore});
 }
 auto hold=retained;s.DeferPriorityOperation([hold,record,lease]{record->gpu_ready.store(true,std::memory_order_release);lease->CompleteNative(record->frame.copy_tick);});
 ++copies;Use(s,Span{{slot},1});std::printf("NATIVE_ASYNC_ROLLING_SOURCE source=%llu physical_packet=%u epoch=%llu history=%llu copy_tick=%llu unedited_input=1 active_origin=%llu pending_origin=%llu\n",(unsigned long long)f.source,slot,(unsigned long long)f.epoch,(unsigned long long)f.history,(unsigned long long)f.copy_tick,(unsigned long long)(active?active->id.origin_source:0),(unsigned long long)(pending?pending->id.origin_source:0));return true;
}
vk::ImageView NativeAsyncRolling::Complete(Scheduler&s,uint64_t current){
 if(!pending||!retained||peer->State()!=NativeOriginOwner::Phase::Complete||pending->final->native_target_tick)return {};
 const auto&e=pending->complete;const auto&id=pending->id;const auto frames=Frames();
 if(!frames[pending_slot]||!id.Matches(*frames[pending_slot])||!e.verified_wire||!e.helper_GPU_complete||!e.helper_gpu_serial||e.helper_gpu_serial<=last_serial||!pending->input->native_GPU_complete.load(std::memory_order_acquire)||e.input_tick_echo!=pending->input->native_target_tick||e.identity.session!=id.session||e.identity.resource_set!=id.resource_set||e.identity.generation!=id.generation||e.identity.job!=id.job||e.identity.origin_source!=id.origin_source||e.identity.origin_epoch!=id.origin_epoch||e.identity.origin_history!=id.origin_history||e.identity.params_version!=id.params_version||e.model_reset!=1||!pending->resources.Valid(id))throw std::runtime_error("rolling exact immutable completion");
 const bool valid=e.output_valid&&e.model_valid&&e.evaluate_calls==1&&e.evaluate_result==1&&e.resolve_calls==1;
 const bool skip=e.expected_skip&&!e.output_valid&&!e.model_valid&&!e.evaluate_calls&&!e.evaluate_result&&!e.resolve_calls;
 if(!valid&&!skip)throw std::runtime_error("rolling normal model predicate");
 auto span=stopping||unknown?Span{}:R::FindSpan(frames,pending->origin,current);if(valid&&span&&retained->sky->SparseScratchBusy())return {};
 vk::ImageView result{};AcquireOriginOutput(s,pending->resources,id);auto disposition=skip?NativeAsyncSnapshot::SparseDisposition::ExpectedSkip:NativeAsyncSnapshot::SparseDisposition::ObsoleteDiscard;
 if(valid&&span){
  const auto view=retained->sky->CaptureAndCatchup(s,span,pending->resources.views[3]);if(!view)throw std::runtime_error("rolling catchup after acquire");
  Active next{.id=id,.origin=pending->origin,.advanced_source=current};next.diagnostic.identity.origin=id.origin_source;next.diagnostic.identity.origin_epoch=id.origin_epoch;next.diagnostic.identity.origin_history=id.origin_history;next.diagnostic.identity.origin_params=id.params_version;next.diagnostic.identity.job=id.job;next.diagnostic.identity.resident=true;next.diagnostic.identity.captured=true;next.diagnostic.native_input_tick=e.input_tick_echo;next.diagnostic.helper_serial=e.helper_gpu_serial;next.diagnostic.resource_set=id.resource_set;
  result=view;active=next;selected=true;disposition=NativeAsyncSnapshot::SparseDisposition::Captured;
  for(unsigned k=0;k<span.size;k++)retained->leases[span.slots[k]]->catchup=s.CurrentTick();
 }
 ReleaseOriginOutput(s,pending->resources,id,pending->final);retained->leases[pending_slot]->capture=std::max(retained->leases[pending_slot]->capture,pending->final->native_target_tick);
 Use(s,span?span:Span{{pending_slot},1});
 if(valid&&span){auto hold=retained;s.DeferPriorityOperation([hold]{hold->sky->CompleteSparseNativeUse();});}
 last_serial=e.helper_gpu_serial;peer->FinalRecorded(pending,disposition);
 std::printf("NATIVE_ASYNC_ROLLING_COMPLETE job=%llu pending_origin=%llu current=%llu captured=%u links=%u selected_active_origin=%llu native_final_tick=%llu\n",(unsigned long long)id.job,(unsigned long long)id.origin_source,(unsigned long long)current,disposition==NativeAsyncSnapshot::SparseDisposition::Captured,span?span.size-1:0,(unsigned long long)(active?active->id.origin_source:0),(unsigned long long)pending->final->native_target_tick);
 return result;
}
vk::ImageView NativeAsyncRolling::Advance(Scheduler&s,uint64_t current){
 if(!retained||current!=current_source||!retained->records[current_slot]||retained->records[current_slot]->frame.source!=current||stopping||unknown)return {};
 const auto f=retained->records[current_slot]->frame;const std::array<vk::Image,3>src{vk::Image(retained->images[current_slot*3]),vk::Image(retained->images[current_slot*3+1]),vk::Image(retained->images[current_slot*3+2])};
 retained->sky->Convert(s,current_slot,src);Use(s,Span{{current_slot},1});
 vk::ImageView view{};
 if(active){auto span=R::FindSpan(Frames(),active->origin,current);
  if(span&&active->advanced_source+1==current){view=retained->sky->Advance(s,current_slot);active->advanced_source=current;selected=bool(view);retained->leases[current_slot]->catchup=s.CurrentTick();Use(s,Span{{current_slot},1});}
  else{active.reset();retained->sky->Invalidate();}
 }
 const auto fresh=Complete(s,current);if(fresh)view=fresh;
 if(selected&&active){bool replay=false;auto regular=retained->sky->SelectedOutput(current_slot,view,replay);
  if(!regular)throw std::runtime_error("rolling current selected image identity");
  if(diagnostic){auto c=active->diagnostic;c.identity.current=current;c.identity.current_epoch=f.epoch;c.identity.current_history=f.history;c.identity.current_params=f.params;c.identity.actual_view_image_match=true;c.replay=replay;diagnostic->RecordEditOnce(s,retained->sky->Converted(current_slot)[0],regular,f,c);}
 }
 if(!pending&&peer->State()==NativeOriginOwner::Phase::Ready){
  if(auto job=peer->TryReserve(f)){
   pending=job;pending_slot=current_slot;retained->leases[current_slot]->peer_read=true;
   RecordOriginInput(s,job->resources,job->id,retained->sky->Converted(current_slot),job->input);retained->leases[current_slot]->capture=job->input->native_target_tick;Use(s,Span{{current_slot},1});peer->InputRecorded(job);
   std::printf("NATIVE_ASYNC_ROLLING_INPUT_RECORDED job=%llu pending_origin=%llu physical_packet=%u active_origin=%llu input_tick=%llu producer=same_Native_recorded native_GPU_ready=NOT_YET_OBSERVED no_extra_flush=1 recursive_capture=0\n",(unsigned long long)job->id.job,(unsigned long long)job->id.origin_source,current_slot,(unsigned long long)(active?active->id.origin_source:0),(unsigned long long)job->input->native_target_tick);
  }
 }
 return selected?view:vk::ImageView{};
}
void NativeAsyncRolling::Merge(Scheduler&s,uint64_t source){
 if(!selected||!retained||source!=current_source)return;
 retained->leases[current_slot]->merge=s.CurrentTick();retained->sky->MergeUse(current_slot,s.CurrentTick());Use(s,Span{{current_slot},1});if(diagnostic)diagnostic->EditMergeRecorded(s,source);
 std::printf("NATIVE_ASYNC_ROLLING_EDIT_CURRENT source=%llu active_origin=%llu active_job=%u age=%llu pending_origin=%llu merge_tick=%llu origin_age_reset=0\n",(unsigned long long)source,(unsigned long long)Origin(),Job(),(unsigned long long)(source-Origin()),(unsigned long long)(pending?pending->id.origin_source:0),(unsigned long long)s.CurrentTick());
}
void NativeAsyncRolling::Drain(Scheduler&s){Complete(s,0);}
void NativeAsyncRolling::Quarantine()noexcept{unknown=true;stopping=true;selected=false;if(peer)peer->RequestStop(3);if(retained)for(auto&lease:retained->leases)if(lease)lease->unknown=true;}
void NativeAsyncRolling::Shutdown(Scheduler&s){
 stopping=true;if(frontend)frontend->RequestStop();if(peer)peer->RequestStop(2);Observe(s);
 if(retained){
  if(unknown||pending||!AllReady()){
   // Managed process lifetime HOLD, never elapsed-time destruction.
   static auto*mutex=new std::mutex;static auto*held=new std::vector<std::shared_ptr<Retention>>;std::scoped_lock lock(*mutex);held->push_back(std::move(retained));
  }else retained.reset();
 }
}
}
