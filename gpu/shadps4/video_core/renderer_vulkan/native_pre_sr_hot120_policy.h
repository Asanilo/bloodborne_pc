// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "native_async_state.h"
#include <string_view>
#include <cstdlib>
#include <cstring>
namespace BbPreSr::Hot120 {
inline constexpr uint64_t MaxJobs=120,MaxPassivePresents=30000;
inline bool Configuration(std::string_view cap,std::string_view pixels,std::string_view diag,std::string_view scene)noexcept {
 return cap=="120"&&pixels=="0"&&diag=="0"&&scene=="1";
}
inline bool Environment()noexcept {
 const auto env=[](const char*k){const char*p=std::getenv(k);return std::string_view(p?p:"");};
 return Configuration(env("BB_NR_NATIVE_PRE_SR_MAX_JOBS"),env("BB_NR_NATIVE_PRE_SR_PRESENT_READBACK"),
  env("BB_NR_NATIVE_PRE_SR_DIAGNOSTIC_ONCE"),env("BB_NR_NATIVE_PRE_SR_SCENE_FIRST"));
}
// PreSR-private identity check; never changes old NativeOriginIdentity::Matches.
template<class Id>inline bool IdentityValid(const Id&id)noexcept {
 if(id.session.empty()||id.session.size()>63||id.resource_set!=id.session+"-g0"||id.generation||
  id.job>=MaxJobs||!id.origin_source||!id.origin_epoch||!id.origin_history||id.params_version!=1)return false;
 for(const char c:id.session)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'))return false;
 return true;
}
template<class Id>inline bool IdentityMatches(const Id&id,const BbNativeAsync::Frame&f)noexcept {
 return IdentityValid(id)&&BbNativeAsync::Finite(f)&&id.origin_source==f.source&&
  id.origin_epoch==f.epoch&&id.origin_history==f.history&&id.params_version==f.params;
}
inline bool SafeReuse(bool real_previous_use,bool exact_consumed,bool peer_ready)noexcept {
 return real_previous_use&&exact_consumed&&peer_ready;
}
inline bool CanPollPrior(uint64_t target,uint64_t current)noexcept {
 return target&&target<current; // CPU closed tick only, NOT proof of queue submission
}
inline bool CanAssociate(uint64_t source,uint64_t prepare_frame,uint64_t merge,uint64_t prepare)noexcept {
 return source&&source!=UINT64_MAX&&prepare_frame==source+1&&merge&&prepare>=merge;
}
struct Observation {
 uint64_t source=0,job=0,epoch=0,history=0;
 bool supported=false,known=false,selected=false,job_valid=false;
 const char*reason="NOT_MEASURED";
};
}
