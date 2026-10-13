// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/native_pre_sr_policy.h"
#include "video_core/renderer_vulkan/native_pre_sr_wire.h"
#include "video_core/renderer_vulkan/p0_wait_budget.h"
#include <cstdio>
#include <cstdlib>
int main(){unsigned cases=0;auto check=[&](bool x){++cases;if(!x)std::abort();};
 BbPreSr::SourceIdentity a{100,3,2},b=a;
 check(BbPreSr::CanGrant(a,b,true,true,true,true));
 b.source=99;check(!BbPreSr::CanGrant(a,b,true,true,true,true));b=a;b.source=101;check(!BbPreSr::CanGrant(a,b,true,true,true,true));
 b=a;b.epoch++;check(!BbPreSr::CanGrant(a,b,true,true,true,true));b=a;b.history++;check(!BbPreSr::CanGrant(a,b,true,true,true,true));
 b=a;b.params=2;check(!BbPreSr::CanGrant(a,b,true,true,true,true));b=a;b.original720=false;check(!BbPreSr::CanGrant(a,b,true,true,true,true));
 b=a;b.hdr=false;check(!BbPreSr::CanGrant(a,b,true,true,true,true));b=a;b.source_edited=true;check(!BbPreSr::CanGrant(a,b,true,true,true,true));
 for(unsigned k=0;k<4;k++)check(!BbPreSr::CanGrant(a,a,k!=0,k!=1,k!=2,k!=3));
 BbPreSr::InputDomain domain;check(!domain.Change(false));check(domain.Change(true));check(!domain.Change(true));check(domain.Change(false));check(!domain.Change(false));
 check(!BbPreSr::InputDomain::AppliedReset(false,false));check(BbPreSr::InputDomain::AppliedReset(false,true));check(BbPreSr::InputDomain::AppliedReset(true,false));
 std::array<BbPreSr::SourceCI,3> ci{{{97,1280,720,1,1,1,1,true},{130,1280,720,1,1,1,1,true},{83,1280,720,1,1,1,1,true}}};
 check(BbPreSr::SourcesSupported(ci));
 for(unsigned k=0;k<3;k++){auto badci=ci;badci[k].transfer_src=false;check(!BbPreSr::SourcesSupported(badci));}
 auto badci=ci;badci[0].width=1920;check(!BbPreSr::SourcesSupported(badci));badci=ci;badci[1].format=100;check(!BbPreSr::SourcesSupported(badci));
 badci=ci;badci[2].format=103;check(!BbPreSr::SourcesSupported(badci));badci=ci;badci[0].samples=4;check(!BbPreSr::SourcesSupported(badci));
 namespace W=Vulkan::NativePreSrWire;auto config=W::config_expected("pre-test","resident",std::string(32,'a'),std::string(32,'b'));
 config=W::parse(config.dump());W::config(config,"pre-test","resident");check(config["width"]==1280&&config["height"]==720&&config["quality_profile"]["sr_output_width"]==1920);
 check(W::u(config,"native_soft_deadline_ms")==500);
 auto reject=[&](auto j){bool failed=false;try{W::config(j,"pre-test","resident");}catch(...){failed=true;}check(failed);};
 auto bad=config;bad["width"]=1920u;reject(bad);bad=config;bad["native_soft_deadline_ms"]=true;reject(bad);
 bad=config;bad["native_soft_deadline_ms"]=499u;reject(bad);bad=config;bad["quality_profile"]["route"]="postSR";reject(bad);
 bad=config;bad["quality_profile"]["working_scale"]=false;reject(bad);bad=config;bad["profile_id"]="native-async-origin720-current-sr1080-experimental-v1";reject(bad);
 namespace P=Vulkan::P0Wait;check(P::Remaining(500000000,0).nanoseconds==500000000);check(P::Remaining(500000000,500000000).kind==P::BudgetKind::Expired);
 check(P::Classify(P::Success,true,false)==P::Outcome::Expired);check(P::Classify(P::Success,false,true)==P::Outcome::InvalidSuccess);check(P::Classify(-4,false,true)==P::Outcome::Error);
 check(P::Classify(P::Timeout,false,true)==P::Outcome::Expired);check(P::Classify(P::Success,true,true)==P::Outcome::Completed);
 check(BbPreSr::ExactSceneToken("pre-test\n","pre-test"));
 for(const auto token:{"pre-test","pre-test\r\n","other\n","pre-test\nextra","pre-test\n\n"})check(!BbPreSr::ExactSceneToken(token,"pre-test"));
 check(!BbPreSr::ExactSceneToken("\n",""));
 check(!BbPreSr::ExactSceneToken(std::string(64,'x')+"\n",std::string(64,'x')));
 check(BbPreSr::CanStartPeer(true,true,false));
 check(!BbPreSr::CanStartPeer(false,true,false));check(!BbPreSr::CanStartPeer(true,false,false));check(!BbPreSr::CanStartPeer(true,true,true));
 BbPreSr::NoPeerExitProof exit{true,true,true,true,false,0,0};check(BbPreSr::CanExitWithoutPeer(exit));
 auto x=exit;x.no_owner=false;check(!BbPreSr::CanExitWithoutPeer(x));x=exit;x.no_snapshot=false;check(!BbPreSr::CanExitWithoutPeer(x));
 x=exit;x.no_pending=false;check(!BbPreSr::CanExitWithoutPeer(x));x=exit;x.arm_joined=false;check(!BbPreSr::CanExitWithoutPeer(x));
 x=exit;x.unknown=true;check(!BbPreSr::CanExitWithoutPeer(x));x=exit;x.attempted_jobs=1;check(!BbPreSr::CanExitWithoutPeer(x));
 x=exit;x.maximum_native_use_tick=1;check(!BbPreSr::CanExitWithoutPeer(x)); // N0 never excuses an actual GPU use
 setenv("BB_NR_NATIVE_PRE_SR_SCENE_FIRST","0",1);check(!BbPreSr::SceneFirstEnabled());
 setenv("BB_NR_NATIVE_PRE_SR_SCENE_FIRST","1",1);check(BbPreSr::SceneFirstEnabled());unsetenv("BB_NR_NATIVE_PRE_SR_SCENE_FIRST");check(!BbPreSr::SceneFirstEnabled());
 std::printf("{\"status\":\"PASS\",\"cases\":%u,\"scope\":\"production_same_source_profile_domain_reset_finite_wait_CPU_only\"}\n",cases);
}
