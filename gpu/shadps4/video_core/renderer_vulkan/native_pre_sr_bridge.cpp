// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_pre_sr_bridge.h"
#include "vk_instance.h"
#include "vk_scheduler.h"
#include "p0_wait_budget.h"
#include "native_sky_diagnostic.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>
namespace Vulkan {
namespace {using Clock=std::chrono::steady_clock;
std::string Env(const char*k){const char*p=std::getenv(k);return p?p:"";}
}
std::string NativePreSrBridge::Session()const{return Env("BB_NR_NATIVE_PRE_SR_SESSION");}
void NativePreSrBridge::StartArm(){
 if(arm_started)return;arm_started=true;
 const bool scene_first=BbPreSr::SceneFirstEnabled();
 helper_start_requested.store(!scene_first,std::memory_order_release);
 const auto session=Session(),run=Env("BB_NR_NATIVE_PRE_SR_RUN_DIR");
 if(session.empty()||run!="/home/arinp/Code/nrbridge-design/game-poc-20261011/bbport/runs/native-pre-sr-game-"+session)return;
 arm_finished.store(false);arm_thread=std::jthread([this,run,session,scene_first](std::stop_token stop){
  struct End{std::atomic<bool>&f;~End(){f.store(true,std::memory_order_release);}} end{arm_finished};
  // All local file IO stays on this managed preparation thread, not renderer.
  const auto token_matches=[&](const char*name){
   std::ifstream f(run+"/"+name,std::ios::binary);char b[65]{};
   if(!f)return false;f.read(b,sizeof(b));
   return BbPreSr::ExactSceneToken(std::string_view(b,size_t(f.gcount())),session);
  };
  const auto limit=Clock::now()+std::chrono::seconds(300);
  while(!stop.stop_requested()&&Clock::now()<limit){
   if(scene_first&&!helper_start_requested.load(std::memory_order_acquire)&&token_matches("native-helper-start")){
    helper_start_requested.store(true,std::memory_order_release);
    std::printf("NATIVE_PRE_SR_HELPER_START_REQUEST session=%s scene_first=1 scene_proof=REQUIRES_ROOT GPU_proof=NONE\n",session.c_str());
   }
   if(!armed.load(std::memory_order_acquire)&&token_matches("native-scene-arm")){
    armed.store(true,std::memory_order_release);
    std::printf("NATIVE_PRE_SR_ARM session=%s scene_proof=REQUIRES_ROOT\n",session.c_str());
   }
   if(armed.load(std::memory_order_acquire)&&helper_start_requested.load(std::memory_order_acquire))return;
   std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
 });
}
void NativePreSrBridge::Use(Scheduler&s){
 if(!slot||!peer)return;const auto t=s.CurrentTick();maximum_native_use_tick=std::max(maximum_native_use_tick,t);slot->last_use=std::max(slot->last_use,t);
 auto q=peer->Quiescence();auto old=q->last_use_tick.load();while(old<t&&!q->last_use_tick.compare_exchange_weak(old,t)){}
 s.DeferPriorityOperation([hold=slot,q,t]{auto n=hold->observed.load();while(n<t&&!hold->observed.compare_exchange_weak(n,t)){}
  n=q->observed_tick.load();while(n<t&&!q->observed_tick.compare_exchange_weak(n,t)){}
 });
}
void NativePreSrBridge::HoldUnknown()noexcept{
 unknown=true;stopping=true;if(peer)peer->RequestAbort();
 // Sticky resources: no last-use inference from N0, timeout or reset pointers.
 static auto*lock=new std::mutex;static auto*held=new std::vector<std::shared_ptr<Slot>>;
 static auto*held_diagnostics=new std::vector<std::shared_ptr<NativeSkyDiagnostic>>;
 if(slot||diagnostic){std::scoped_lock guard(*lock);if(slot)held->push_back(slot);if(diagnostic)held_diagnostics->push_back(diagnostic);}
 std::fprintf(stderr,"NATIVE_PRE_SR_QUARANTINE source=%llu normal_release=0 resources_held=1\n",(unsigned long long)current_source);
}
void NativePreSrBridge::RejectAssociation()noexcept{HoldUnknown();}
bool NativePreSrBridge::WaitTarget(const Instance&i,Scheduler&s,uint64_t t,Clock::time_point deadline){
 if(s.IsFree(t))return true;
 const auto budget=P0Wait::Remaining(P0Wait::EpochNs(deadline),P0Wait::EpochNs(Clock::now()));
 if(budget.kind==P0Wait::BudgetKind::Expired)return false;
 if(budget.kind!=P0Wait::BudgetKind::Finite)throw std::runtime_error("preSR wait budget range");
 const auto sem=s.GetWorkSemaphore()->Handle();const vk::SemaphoreWaitInfo info{.semaphoreCount=1,.pSemaphores=&sem,.pValues=&t};
 const auto result=i.GetDevice().waitSemaphores(&info,budget.nanoseconds);
 const auto outcome=P0Wait::Classify(int32_t(result),result==vk::Result::eSuccess&&s.IsFree(t),Clock::now()<deadline);
 if(outcome==P0Wait::Outcome::Completed)return true;if(outcome==P0Wait::Outcome::Expired)return false;
 throw std::runtime_error("preSR finite target wait result/tick error: "+vk::to_string(result));
}
void NativePreSrBridge::FinalizeLate(Scheduler&s){
 if(!peer||!pending||unknown)return;
 if(peer->State()!=NativePreSrOwner::Phase::Complete)return;
 const auto&c=pending->complete;const auto&id=pending->id;
 if(!c.verified_wire||!c.helper_GPU_complete||!c.helper_gpu_serial||
    c.identity.origin_source!=id.origin_source||c.identity.origin_epoch!=id.origin_epoch||
    c.identity.origin_history!=id.origin_history||c.input_tick_echo!=pending->input->native_target_tick||
    !pending->input->native_GPU_complete.load(std::memory_order_acquire))throw std::runtime_error("preSR late exact identity/GPU proof");
 AcquirePreSrOutput(s,pending->resources,id);Use(s);
 ReleasePreSrOutput(s,pending->resources,id,pending->final);Use(s);
 peer->FinalRecorded(pending,c.expected_skip?PreSrDisposition::ExpectedSkip:PreSrDisposition::ObsoleteDiscard);
 std::printf("NATIVE_PRE_SR_LATE_DISCARD job=%llu origin=%llu current=%llu target=%llu output_sampled=0 accepted_for_SR=0\n",(unsigned long long)id.job,(unsigned long long)id.origin_source,(unsigned long long)current_source,(unsigned long long)pending->final->native_target_tick);
}
void NativePreSrBridge::Poll(const Instance&i,Scheduler&s,bool supported){
 if(!Enabled())return;StartArm();
 const bool exiting=BbNativeAsync::ExitRequested();
 if(exiting&&!peer){
  stopping=true;arm_thread.request_stop();if(arm_thread.joinable())arm_thread.join();
  const BbPreSr::NoPeerExitProof proof{!peer,!slot,!pending,
   arm_finished.load(std::memory_order_acquire)&&!arm_thread.joinable(),unknown,
   attempt_count,maximum_native_use_tick};
  if(BbPreSr::CanExitWithoutPeer(proof)){
   std::printf("NATIVE_PRE_SR_EXIT_NO_OWNER session=%s actual_no_owner=1 actual_no_snapshot=1 actual_no_pending=1 maximum_native_use_tick=0 arm_joined=1 no_peer_created=1 no_NR_claim=1\n",Session().c_str());
   BbNativeAsync::PublishExitComplete();
  }
  return;
 }
 if(!peer&&BbPreSr::CanStartPeer(supported,helper_start_requested.load(std::memory_order_acquire),exiting)){
  peer=std::make_shared<NativePreSrOwner>();peer->Start(i);
 }
 if(!peer)return;
 if(BbNativeAsync::ExitRequested()){stopping=true;peer->RequestStop(2);arm_thread.request_stop();}
 if(!supported&&slot){stopping=true;peer->RequestStop(3);}
 if(peer->State()==NativePreSrOwner::Phase::Failed){
  if(BbNativeAsync::ExitRequested()&&!slot&&!pending&&!attempt_count&&arm_finished.load(std::memory_order_acquire)&&peer->SafeEmptyBootstrapExit(false,false,0)){
   peer->JoinFinished();if(arm_thread.joinable())arm_thread.join();BbNativeAsync::PublishAbnormalEmptyExitComplete();return;
  }
  if(!unknown)HoldUnknown();return;
 }
 try{if(expired)FinalizeLate(s);}catch(const std::exception&e){std::fprintf(stderr,"NATIVE_PRE_SR_DRAIN_FAILED reason=%s\n",e.what());HoldUnknown();return;}
 if(peer->State()==NativePreSrOwner::Phase::Finalized&&pending&&pending->consumed_published.load(std::memory_order_acquire)){
  peer->FinalizedAck(pending);pending.reset();
 }
 if(peer->State()==NativePreSrOwner::Phase::StopRequested||peer->State()==NativePreSrOwner::Phase::AwaitNativeRelease||peer->State()==NativePreSrOwner::Phase::Retired)stopping=true;
 if(stopping&&!unknown&&(!slot||slot->Done())){
  if(slot)slot.reset();
  else if(!peer->Quiescence()->last_use_tick.load())peer->Quiescence()->stopped_and_destroyed.store(true,std::memory_order_release);
  if(BbNativeAsync::ExitRequested()&&peer->ReapedNormally()&&arm_finished.load(std::memory_order_acquire)&&(!diagnostic||diagnostic->RetirePreSrFinished())){
   peer->JoinFinished();if(arm_thread.joinable())arm_thread.join();if(diagnostic)diagnostic->JoinFinished();BbNativeAsync::PublishExitComplete();
  }
 }
}
vk::ImageView NativePreSrBridge::TryCurrent(const Instance&i,Scheduler&s,const std::array<vk::Image,3>&images,
 const std::array<vk::ImageCreateInfo,3>&ci,BbNativeAsync::Frame f){
 current_source=f.source;selected=false;attempted=false;
 if(!Enabled()||!armed.load(std::memory_order_acquire)||!peer||stopping||unknown||expired||
    pending||peer->State()!=NativePreSrOwner::Phase::Ready||!BbNativeAsync::Finite(f)||attempt_count>=120||
    (slot&&!slot->Done()))return {};
 if(!i.IsSeparateDepthStencilLayoutsEnabled()||!NativePreSrGpu::SourcesSupported(images,ci))return {};
 // All descriptors and source/return storage remain immutable until real final use.
 const auto begin=Clock::now();const auto deadline=begin+std::chrono::milliseconds(500);acceptance_deadline=deadline;
 if(seen_source&&f.source<=seen_source)return {};
 if(seen_source&&(f.sr_reset||f.source!=seen_source+1))++history;
 f.epoch=attempt_count+1;f.history=history;seen_source=f.source;
 auto job=peer->TryReserve(f);if(!job)return {};
 pending=job;current_job=job->id.job;attempted=true;++attempt_count;
 try{
  if(!slot)slot=std::make_shared<Slot>(i,peer->Quiescence());
  Use(s);slot->gpu->RecordSource(s,images,ci);
  RecordPreSrInput(s,job->resources,job->id,slot->gpu->Converted(),job->input);Use(s);
  peer->InputRecorded(job);const auto input_tick=job->input->native_target_tick;
  s.Flush(); // Actual camera/source conversion/input release, before NR can run.
  if(!WaitTarget(i,s,input_tick,deadline)){expired=true;stopping=true;peer->RequestStop(4);return {};}
  while(peer->State()!=NativePreSrOwner::Phase::Complete&&Clock::now()<deadline){
   if(peer->State()==NativePreSrOwner::Phase::Failed)throw std::runtime_error("preSR peer failure while same-source pending");
   std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if(peer->State()!=NativePreSrOwner::Phase::Complete||Clock::now()>=deadline){expired=true;stopping=true;peer->RequestStop(4);return {};}
  const auto&c=job->complete;const auto&id=job->id;
  if(!c.verified_wire||!c.helper_GPU_complete||!c.helper_gpu_serial||c.identity.origin_source!=f.source||
     c.identity.origin_epoch!=f.epoch||c.identity.origin_history!=f.history||c.identity.params_version!=f.params||
     c.input_tick_echo!=input_tick||!job->input->native_GPU_complete.load(std::memory_order_acquire)||!job->resources.Valid(id))throw std::runtime_error("preSR same current completion proof mismatch");
  const bool valid=c.output_valid&&c.model_valid&&c.evaluate_calls==1&&c.evaluate_result==1&&c.resolve_calls==1&&c.model_reset==1;
  const bool skip=c.expected_skip&&!c.output_valid&&!c.model_valid&&!c.evaluate_calls&&!c.evaluate_result&&!c.resolve_calls;
  if(!valid&&!skip)throw std::runtime_error("preSR actual API/output predicate mismatch");
  AcquirePreSrOutput(s,job->resources,id);Use(s);
  if(valid){slot->gpu->RecordReturn(s,job->resources.views[3]);}
  ReleasePreSrOutput(s,job->resources,id,job->final);Use(s);s.Flush();
  const bool return_done=WaitTarget(i,s,job->final->native_target_tick,deadline);
  const uint32_t flag=valid&&return_done?slot->gpu->ReadGuardAfterGPUComplete():0;
  const BbPreSr::SourceIdentity exact{f.source,f.epoch,f.history,f.params};
  selected=BbPreSr::CanGrant(exact,{id.origin_source,id.origin_epoch,id.origin_history,id.params_version},valid,c.helper_GPU_complete,return_done,flag==0)&&Clock::now()<deadline;
  // For valid output, defer CONSUMED disposition until the caller has acquired
  // its fresh command buffer and confirmed the final same-frame acceptance cut.
  if(!selected)peer->FinalRecorded(job,skip?PreSrDisposition::ExpectedSkip:PreSrDisposition::ObsoleteDiscard);
  if(!return_done||Clock::now()>=deadline){expired=true;stopping=true;peer->RequestStop(4);}
  std::printf("NATIVE_PRE_SR_DECISION session=%s job=%llu source=%llu NR_origin=%llu selected_NR720=%u native_input_tick=%llu native_return_tick=%llu helper_serial_source_only=%llu return_guard=%u same_source=%u hook_ms=%.6f soft_ms=500 original_input_immutable=1 phase=UNVERIFIED\n",Session().c_str(),(unsigned long long)id.job,(unsigned long long)f.source,(unsigned long long)id.origin_source,selected,(unsigned long long)input_tick,(unsigned long long)job->final->native_target_tick,(unsigned long long)c.helper_gpu_serial,flag,id.origin_source==f.source,std::chrono::duration<double,std::milli>(Clock::now()-begin).count());
  return selected?slot->gpu->ReturnView():vk::ImageView{};
 }catch(const std::exception&e){std::fprintf(stderr,"NATIVE_PRE_SR_FAILED source=%llu reason=%s SR_original_fallback=1\n",(unsigned long long)f.source,e.what());HoldUnknown();return {};}
}
bool NativePreSrBridge::ConfirmBeforeSr(uint64_t source){
 if(source!=current_source||!selected||!pending||!peer)return false;
 const bool timely=Clock::now()<acceptance_deadline;
 peer->FinalRecorded(pending,timely?PreSrDisposition::Captured:PreSrDisposition::ObsoleteDiscard);
 if(!timely){selected=false;expired=true;stopping=true;peer->RequestStop(4);
  std::printf("NATIVE_PRE_SR_FINAL_ACCEPTANCE_EXPIRED source=%llu same_source_NR_to_SR=0 original_SR=1\n",(unsigned long long)source);
 }
 return timely;
}
void NativePreSrBridge::SrReadRecorded(const Instance&i,Scheduler&s,uint64_t source,bool ok,bool applied_sr_reset,
 vk::Image actual_SR_color,vk::ImageView actual_SR_view){
 if(source!=current_source||!selected)return;
 Use(s); // MUST include the actual SR read target, not just return conversion.
 std::printf("NATIVE_PRE_SR_ACTUAL_SR_READ session=%s source=%llu job=%llu target_tick=%llu SR_api_result=%u input720=%p\n",Session().c_str(),(unsigned long long)source,(unsigned long long)current_job,(unsigned long long)s.CurrentTick(),ok,(void*)VkImage(slot->gpu->ReturnImage()));
 if(ok&&BbPreSr::DiagnosticEnabled()&&!diagnostic_attempted&&slot&&pending){
  diagnostic_attempted=true; // once per session, including allocation/admission failure
  try{
   const auto&f=pending->origin;const auto&id=pending->id;
   NativeSkyDiagnostic::PreSrContext ctx{
    .identity={{f.source,f.epoch,f.history,f.params},{id.origin_source,id.origin_epoch,id.origin_history,id.params_version},
     id.job,pending->input->native_target_tick,pending->final->native_target_tick,s.CurrentTick(),
     true,selected,ok,actual_SR_color==slot->gpu->ReturnImage()&&actual_SR_view==slot->gpu->ReturnView()},
    .helper_serial=pending->complete.helper_gpu_serial,.resource_set=id.resource_set,
    .applied_sr_reset=applied_sr_reset};
   diagnostic=std::make_shared<NativeSkyDiagnostic>(i,NativeSkyDiagnostic::PreSrTag{});
   Use(s); // real copy/SR target retains immutable source and exact selected return
   if(!diagnostic->RecordPreSrOnce(s,slot->gpu->OriginalColorImage(),slot->gpu->ReturnImage(),
       {slot->gpu->OriginalColorCI(),slot->gpu->ReturnCI()},f,ctx)){
    std::fprintf(stderr,"NATIVE_PRE_SR_DIAGNOSTIC_NOT_RECORDED source=%llu admission=REJECTED\n",(unsigned long long)source);
    diagnostic.reset(); // no commands recorded on admission rejection
   }
  }catch(const std::exception&e){
   std::fprintf(stderr,"NATIVE_PRE_SR_DIAGNOSTIC_RECORD_FAILED source=%llu reason=%s\n",(unsigned long long)source,e.what());
   HoldUnknown(); // never free potentially recorded staging on an unknown failure
  }
 }
}
void NativePreSrBridge::MergeRecorded(Scheduler&s,uint64_t source){
 if(source!=current_source||!selected||!diagnostic)return;
 Use(s);diagnostic->PreSrMergeRecorded(s,source);
}
void NativePreSrBridge::Shutdown(Scheduler&s){
 stopping=true;arm_thread.request_stop();if(peer)peer->RequestStop(2);
 if(unknown)return;
 if(slot){Use(s);auto hold=std::move(slot);
  s.DeferOperation([hold]{});
 }else if(peer)peer->Quiescence()->stopped_and_destroyed.store(true,std::memory_order_release);
}
}
