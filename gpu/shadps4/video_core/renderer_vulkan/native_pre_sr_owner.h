// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_pre_sr_origin_gpu.h"
#include <mutex>
#include <thread>
namespace Vulkan {
class Instance;
class NativePreSrOwner {
public:
 enum class Phase {Disabled,Bootstrap,Ready,Reserved,InputRecorded,WaitingComplete,Complete,
                    FinalRecorded,Finalized,StopRequested,AwaitNativeRelease,Retired,Failed};
 struct Job {
  NativeOriginIdentity id;BbNativeAsync::Frame origin;NativePreSrResources resources;
  std::shared_ptr<NativeOriginGpuProof> input=std::make_shared<NativeOriginGpuProof>();
  std::shared_ptr<NativeOriginGpuProof> final=std::make_shared<NativeOriginGpuProof>();
  NativePreSrCompletion complete;
  PreSrDisposition disposition=PreSrDisposition::Unavailable;
  std::atomic<bool> consumed_published{false};
 };
 NativePreSrOwner()=default;~NativePreSrOwner();
 bool Start(const Instance&); // one managed owner; never records Scheduler commands
 bool Enabled()const noexcept;
 Phase State()const noexcept{return phase.load(std::memory_order_acquire);}
 std::shared_ptr<Job> TryReserve(const BbNativeAsync::Frame&); // bounded try_lock
 std::shared_ptr<Job> TryJob();
 void CancelReservation(const std::shared_ptr<Job>&);
 void InputRecorded(const std::shared_ptr<Job>&);
 void FinalRecorded(const std::shared_ptr<Job>&,PreSrDisposition);
 void FinalizedAck(const std::shared_ptr<Job>&);
 void RequestAbort()noexcept {abort_requested.store(true,std::memory_order_release);}
 void RequestStop(unsigned reason)noexcept; // 1off/2exit/3profile/4budget, memory only
 std::shared_ptr<NativeOriginQuiescence> Quiescence()const{return quiescence;}
 bool SafeEmptyBootstrapExit(bool has_snapshot,bool pending_lease,uint64_t copies_ever)const noexcept;
 void JoinFinished(); // only after actual worker_finished; no peer/model wait
 int ActualWaitStatus()const noexcept{return actual_wait_status.load();}
 bool ReapedNormally()const noexcept{return normal_reap.load(std::memory_order_acquire);}
private:
 void Run(std::stop_token,const Instance&);
 void Fail(const std::string&)noexcept;
 std::atomic<Phase> phase{Phase::Disabled};std::atomic<unsigned> stop_reason{0};
 std::atomic<bool> started{false},normal_reap{false},abort_requested{false};
 std::atomic<bool> peer_spawned{false},peer_reaped{false},worker_finished{false},empty_imports_destroyed{false};
 std::atomic<uint64_t> import_objects_ever_created{0};
 std::atomic<int> actual_wait_status{-1};
 std::mutex mutex;std::jthread worker;std::shared_ptr<Job> job;
 std::shared_ptr<NativeOriginQuiescence> quiescence=std::make_shared<NativeOriginQuiescence>();
 NativePreSrResources registry;std::string session,mode,run,helper,wine,preloader,prefix;
 std::atomic<uint64_t> finalized{0};
 // Under mutex: a recorded INPUT cannot be issued again for the same origin,
 // even when completed CONSUMED leaves records[0] alive. Cancel before GPU input
 // leaves this unchanged; no automatic retry after an issued/failed job.
 uint64_t last_recorded_origin=0;
};
}
