// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "native_pre_sr_gpu.h"
#include "native_pre_sr_owner.h"
#include <thread>
namespace Vulkan {
class NativePreSrBridge {
public:
 ~NativePreSrBridge(){arm_thread.request_stop();if(arm_thread.joinable())arm_thread.join();} // before atomic member teardown
 bool Enabled()const noexcept{return BbPreSr::Enabled();}
 void Poll(const Instance&,Scheduler&,bool supported);
 vk::ImageView TryCurrent(const Instance&,Scheduler&,const std::array<vk::Image,3>&,
   const std::array<vk::ImageCreateInfo,3>&,BbNativeAsync::Frame);
 bool ConfirmBeforeSr(uint64_t source); // final soft-cutoff after fresh command buffer acquisition
 void SrReadRecorded(Scheduler&,uint64_t source,bool SR_api_success);
 void Shutdown(Scheduler&);
 void RejectAssociation()noexcept;
 bool CurrentSelected(uint64_t source)const noexcept{return source==current_source&&selected;}
 bool CurrentAttempt(uint64_t source)const noexcept{return source==current_source&&attempted;}
 uint64_t CurrentJob()const noexcept{return current_job;}
 vk::Image ReturnImage()const noexcept{return slot?slot->gpu->ReturnImage():vk::Image{};}
 std::string Session()const;
 bool ChangeInputDomain(bool nr)noexcept{return domain.Change(nr);}
private:
 struct Slot {
  explicit Slot(const Instance&i,std::shared_ptr<NativeOriginQuiescence> q):gpu(std::make_unique<NativePreSrGpu>(i)),quiescence(std::move(q)){}
  ~Slot(){gpu.reset();if(quiescence)quiescence->stopped_and_destroyed.store(true,std::memory_order_release);}
  std::unique_ptr<NativePreSrGpu> gpu;
  std::shared_ptr<NativeOriginQuiescence> quiescence;
  std::atomic<uint64_t> observed{0};uint64_t last_use=0;
  bool Done()const noexcept{return !last_use||observed.load(std::memory_order_acquire)>=last_use;}
 };
 void StartArm();
 void Use(Scheduler&);
 void FinalizeLate(Scheduler&);
 void HoldUnknown()noexcept;
 static bool WaitTarget(const Instance&,Scheduler&,uint64_t,std::chrono::steady_clock::time_point);
 std::shared_ptr<NativePreSrOwner> peer;
 std::shared_ptr<Slot> slot;
 std::shared_ptr<NativePreSrOwner::Job> pending;
 std::jthread arm_thread;std::atomic<bool> armed{false},arm_finished{true},helper_start_requested{false};
 bool arm_started=false,stopping=false,unknown=false,selected=false,attempted=false;
 bool expired=false;BbPreSr::InputDomain domain;
 std::chrono::steady_clock::time_point acceptance_deadline{};
 uint64_t current_source=0,current_job=0,seen_source=0,history=1,attempt_count=0;
 uint64_t maximum_native_use_tick=0; // actual recorded Native use, never Helper fence serial
};
}
