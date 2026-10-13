// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <array>
#include <string_view>
namespace BbPreSr {
inline bool Enabled()noexcept {
 const char*p=std::getenv("BB_NR_NATIVE_PRE_SR");if(!p||std::strcmp(p,"1"))return false;
 for(const char*k:{"BB_NR_NATIVE_ASYNC","BB_NR_P0_MODE","BB_NR_Q1_MODE","BB_NR_C4_MODE","BB_NR_C3_MODE","BB_NR_C2_MODE","BB_NR_C1_MODE"}){
  const char*v=std::getenv(k);if(v&&v[0]&&std::strcmp(v,"0"))return false;
 }
 return true;
}
// Local preparation gate only; not Helper protocol or a GPU completion proof.
inline bool SceneFirstEnabled()noexcept {
 const char*p=std::getenv("BB_NR_NATIVE_PRE_SR_SCENE_FIRST");
 return p&&std::strcmp(p,"1")==0;
}
inline bool ExactSceneToken(std::string_view bytes,std::string_view session)noexcept {
 return !session.empty()&&session.size()<=63&&bytes.size()==session.size()+1&&
  bytes.substr(0,session.size())==session&&bytes.back()=='\n';
}
inline bool CanStartPeer(bool supported,bool requested,bool exiting)noexcept {
 return supported&&requested&&!exiting;
}
struct NoPeerExitProof {
 bool no_owner=false,no_snapshot=false,no_pending=false,arm_joined=false,unknown=false;
 uint64_t attempted_jobs=0,maximum_native_use_tick=0;
};
inline bool CanExitWithoutPeer(const NoPeerExitProof&p)noexcept {
 return p.no_owner&&p.no_snapshot&&p.no_pending&&p.arm_joined&&!p.unknown&&
  p.attempted_jobs==0&&p.maximum_native_use_tick==0;
}
struct SourceCI {
 uint32_t format=0,width=0,height=0,depth=0,samples=0,mips=0,layers=0;bool transfer_src=false;
};
inline bool SourcesSupported(const std::array<SourceCI,3>&actual)noexcept {
 constexpr uint32_t formats[]{97,130,83};
 for(unsigned k=0;k<3;k++){const auto&c=actual[k];if(c.format!=formats[k]||c.width!=1280||c.height!=720||
  c.depth!=1||c.samples!=1||c.mips!=1||c.layers!=1||!c.transfer_src)return false;}
 return true;
}
// Same-current input only. This route cannot use a late previous-frame NR image.
struct SourceIdentity {
 uint64_t source=0,epoch=0,history=0,params=1;
 bool original720=true,hdr=true,source_edited=false;
 bool Valid()const noexcept{return source&&epoch&&history&&params==1&&original720&&hdr&&!source_edited;}
 bool Exact(const SourceIdentity&x)const noexcept{return Valid()&&x.Valid()&&source==x.source&&epoch==x.epoch&&history==x.history&&params==x.params;}
};
// Cold visual diagnostic admission only, not a model/image-quality grant.
inline bool DiagnosticEnabled()noexcept {
 const char*d=std::getenv("BB_NR_NATIVE_PRE_SR_DIAGNOSTIC_ONCE");
 const char*m=std::getenv("BB_NR_NATIVE_ORIGIN_MODE");
 return Enabled()&&d&&m&&std::strcmp(d,"1")==0&&std::strcmp(m,"resident")==0;
}
inline constexpr uint64_t DiagnosticImageBytes=1280ull*720*8;
struct DiagnosticIdentity {
 SourceIdentity current,result;
 uint64_t job=0,input_tick=0,return_tick=0,sr_read_tick=0;
 bool resident=false,selected=false,sr_success=false,actual_image_match=false;
 bool Eligible()const noexcept {
  return resident&&selected&&sr_success&&actual_image_match&&current.Exact(result)&&job<4&&
   input_tick&&return_tick>=input_tick&&sr_read_tick>=return_tick;
 }
};
struct InputDomain {
 bool nr=false;
 bool Change(bool next)noexcept{const bool changed=next!=nr;nr=next;return changed;}
 static bool AppliedReset(bool original,bool domain_changed)noexcept{return original||domain_changed;}
};
inline bool CanGrant(const SourceIdentity&current,const SourceIdentity&result,bool real_model,
 bool real_GPU,bool local_return_GPU,bool half_guard_clear)noexcept {
 return current.Exact(result)&&real_model&&real_GPU&&local_return_GPU&&half_guard_clear;
}
}
