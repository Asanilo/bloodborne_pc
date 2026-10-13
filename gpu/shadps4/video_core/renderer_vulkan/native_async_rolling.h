// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "native_origin_owner.h"
#include "native_sky_rolling_gpu.h"
namespace Vulkan {
class NativeAsyncRolling {
public:
 void Poll(const Instance&,Scheduler&,bool supported);
 bool Armed()const noexcept;
 bool Capture(const Instance&,Scheduler&,const std::array<vk::Image,3>&,const std::array<vk::ImageCreateInfo,3>&,BbNativeAsync::Frame);
 vk::ImageView Advance(Scheduler&,uint64_t current);
 void Merge(Scheduler&,uint64_t source);
 void Drain(Scheduler&);
 void Shutdown(Scheduler&);
 void Quarantine()noexcept;
 bool Effect()const noexcept{return selected;}
 unsigned Job()const noexcept{return selected&&active?unsigned(active->id.job):0;}
 uint64_t Origin()const noexcept{return selected&&active?active->id.origin_source:0;}
 uint64_t Epoch()const noexcept{return selected&&active?active->id.origin_epoch:0;}
private:
 using Frame=BbNativeAsync::Frame;using Span=BbNativeAsync::Rolling::Span;
 struct Active {NativeOriginIdentity id;Frame origin;uint64_t advanced_source=0;NativeSkyDiagnostic::EditContext diagnostic;};
 struct Retention {
  std::array<VideoCore::UniqueImage,3*BbNativeAsync::Rolling::Capacity>images;
  std::array<std::shared_ptr<BbNativeAsync::Node>,BbNativeAsync::Rolling::Capacity>records{};
  std::array<std::shared_ptr<BbNativeAsync::OriginLease>,BbNativeAsync::Rolling::Capacity>leases{};
  std::unique_ptr<NativeSkyRollingGpu>sky;std::shared_ptr<NativeOriginQuiescence>q;
  std::atomic<uint64_t>observed{0};
  ~Retention();
 };
 std::array<std::optional<Frame>,BbNativeAsync::Rolling::Capacity>Frames()const;
 void Create(const Instance&);
 void Use(Scheduler&,const Span&);
 void Observe(Scheduler&);
 bool AllReady()const;
 bool Pinned(unsigned slot,uint64_t current)const;
 vk::ImageView Complete(Scheduler&,uint64_t current);
 void Finalize();
 std::string Session()const;
 std::shared_ptr<Retention>retained;
 std::shared_ptr<NativeOriginOwner>peer;
 std::unique_ptr<BbNativeAsync::Frontend>frontend;
 std::shared_ptr<NativeSkyDiagnostic>diagnostic;
 std::optional<Active>active;
 std::shared_ptr<NativeOriginOwner::Job>pending;
 unsigned pending_slot=0,current_slot=0;uint64_t current_source=0,last_seen=0,epoch=1,history=1,copies=0,max_tick=0,last_serial=0;
 bool created=false,peer_attempted=false,peer_ready=false,stopping=false,unknown=false,selected=false;
};
}
