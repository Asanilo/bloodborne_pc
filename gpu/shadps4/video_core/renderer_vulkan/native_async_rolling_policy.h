// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "native_async_state.h"
#include <optional>
#include <cstdlib>
#include <string_view>
namespace BbNativeAsync::Rolling {
inline constexpr unsigned Capacity=7;
inline bool ValidConfiguration(std::string_view x)noexcept{return x=="0"||x=="1";}
inline bool Enabled()noexcept{
 const char*f=std::getenv("BB_NR_NATIVE_ASYNC_ROLLING");const char*m=std::getenv("BB_NR_NATIVE_ASYNC_MODE");
 return f&&std::string_view(f)=="1"&&m&&(std::string_view(m)=="resident"||std::string_view(m)=="skipEvaluate");
}
struct Span {std::array<unsigned,NodeCount> slots{};unsigned size=0;explicit operator bool()const noexcept{return size>0;}};
inline bool SameDomain(const Frame&a,const Frame&b)noexcept{
 return a.epoch==b.epoch&&a.history==b.history&&a.params==b.params;
}
inline bool AgeValid(uint64_t origin,uint64_t current)noexcept{return origin&&current>=origin&&current-origin<=MaxLinks;}
inline Span FindSpan(const std::array<std::optional<Frame>,Capacity>&frames,const Frame&origin,uint64_t current)noexcept{
 Span result;if(!Finite(origin)||!origin.epoch||!origin.history||!AgeValid(origin.source,current))return result;
 for(unsigned step=0;step<=current-origin.source;++step){
  const uint64_t source=origin.source+step;
  unsigned found=Capacity;
  for(unsigned k=0;k<Capacity;++k)if(frames[k]&&frames[k]->source==source){if(found!=Capacity)return {};found=k;}
  if(found==Capacity){return {};}
  const auto&f=*frames[found];
  if(!Finite(f)||!f.copy_tick||!SameDomain(f,origin)||(source>origin.source&&(f.sr_reset||f.previous!=source-1)))return {};
  result.slots[result.size++]=found;
 }
 return result;
}
inline bool PinForLineage(const Frame&packet,const Frame&origin,uint64_t current)noexcept{
 return AgeValid(origin.source,current)&&SameDomain(packet,origin)&&packet.source>=origin.source&&packet.source<=current;
}
inline bool CanReuse(uint64_t last_use,uint64_t observed,bool peer,bool unknown,bool pinned)noexcept{
 return last_use&&observed>=last_use&&!peer&&!unknown&&!pinned;
}
}
