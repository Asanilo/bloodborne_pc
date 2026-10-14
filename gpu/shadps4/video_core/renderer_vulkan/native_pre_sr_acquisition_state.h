// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "native_async_state.h"
#include <optional>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
namespace BbNativePreSrAcquisition {
inline bool Enabled()noexcept{const char* v=std::getenv("BB_NR_NATIVE_ASYNC_PRE_SR_DECOUPLED_ACQUISITION");return v&&!std::strcmp(v,"1");}
// Renderer-only metadata. These counters NEVER prove GPU/peer completion.
// Physical packet choice separately requires SourceLease::CanReuse actual proof.
struct State {
 static constexpr unsigned Capacity=BbNativeAsync::NodeCount;
 std::array<BbNativeAsync::Frame,Capacity> packets{};
 unsigned current=Capacity,pending_count=0;
 std::array<unsigned,Capacity> pending_slots{};
 bool pending=false,pending_continuous=false,active=false;
 BbNativeAsync::Frame active_origin{};uint64_t active_current=0,active_job=0;
 uint64_t last=0,epoch=1,history=1;
 bool Protected(unsigned slot)const noexcept{
  if(!pending)return false;
  const unsigned n=pending_continuous?pending_count:1;
  for(unsigned i=0;i<n;++i)if(pending_slots[i]==slot)return true;
  return false;
 }
 template<class Ready> std::optional<unsigned> Select(Ready ready)const{
  for(unsigned i=0;i<Capacity;++i)if(!Protected(i)&&ready(i))return i;
  return {};
 }
 bool CanAdmit(const BbNativeAsync::Frame& f)const noexcept{return BbNativeAsync::Finite(f)&&f.source>last;}
 void RejectCurrent()noexcept{current=Capacity;active=false;if(pending)pending_continuous=false;}
 void Admit(unsigned slot,BbNativeAsync::Frame f){
  if(slot>=Capacity||Protected(slot)||!CanAdmit(f))throw std::runtime_error("acquisition metadata admission mismatch");
  if(last&&(f.source!=last+1||f.sr_reset)){++epoch;++history;active=false;if(pending)pending_continuous=false;}
  f.previous=last;f.epoch=epoch;f.history=history;
  if(pending&&pending_continuous){const auto& prev=packets[pending_slots[pending_count-1]];
   const auto& o=packets[pending_slots[0]];
   if(f.source!=prev.source+1||f.sr_reset||f.epoch!=o.epoch||f.history!=o.history||f.source-o.source>BbNativeAsync::MaxLinks||pending_count==Capacity)pending_continuous=false;
   else pending_slots[pending_count++]=slot;
  }
  packets[slot]=f;last=f.source;current=slot;
 }
 bool StartPending(unsigned slot)noexcept{
  if(pending||slot!=current||slot>=Capacity||!packets[slot].source)return false;
  pending=true;pending_continuous=true;pending_slots[0]=slot;pending_count=1;return true;
 }
 bool PendingToCurrent()const noexcept{
  if(!pending||!pending_continuous||!pending_count||current>=Capacity||pending_slots[pending_count-1]!=current)return false;
  const auto&o=packets[pending_slots[0]];const auto&c=packets[current];
  return c.source>=o.source&&c.source-o.source<=BbNativeAsync::MaxLinks&&c.epoch==o.epoch&&c.history==o.history;
 }
 bool AdvanceActive()const noexcept{
  if(!active||current>=Capacity)return false;
  const auto& f=packets[current];
  return f.source==active_current+1&&!f.sr_reset&&f.epoch==active_origin.epoch&&f.history==active_origin.history&&f.params==active_origin.params&&f.source>=active_origin.source&&f.source-active_origin.source<=BbNativeAsync::MaxLinks;
 }
 bool Capture(uint64_t job)noexcept{
  if(!PendingToCurrent())return false;
  active_origin=packets[pending_slots[0]];active_current=packets[current].source;active_job=job;active=true;return true;
 }
 void FinishPending()noexcept{pending=false;pending_continuous=false;pending_count=0;}
};
static_assert(State::Capacity==5&&BbNativeAsync::MaxLinks==4);
} // namespace
