// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/native_pre_sr_hot120_policy.h"
#include "video_core/renderer_vulkan/native_pre_sr_wire.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <string>
using namespace BbPreSr::Hot120;
struct Id {std::string session="cpu-120",resource_set="cpu-120-g0";uint64_t generation=0,job=0,origin_source=100,origin_epoch=1,origin_history=1,params_version=1;};
int main(){unsigned n=0;auto check=[&](bool x){++n;if(!x)std::abort();};
 check(Configuration("120","0","0","1"));
 for(const char*c:{"4","119","121","true",""})check(!Configuration(c,"0","0","1"));
 check(!Configuration("120","1","0","1"));check(!Configuration("120","0","1","1"));check(!Configuration("120","0","0","0"));
 Id id;BbNativeAsync::Frame f{.source=100,.epoch=1,.history=1};
 for(uint64_t job:{0ull,3ull,4ull,63ull,64ull,119ull}){id.job=job;check(IdentityMatches(id,f));}
 id.job=120;check(!IdentityMatches(id,f));id.job=0;id.origin_source=101;check(!IdentityMatches(id,f));
 id.origin_source=100;id.generation=1;check(!IdentityMatches(id,f));id.generation=0;id.session="bad/name";check(!IdentityValid(id));
 id=Id{};id.resource_set="wrong-g0";check(!IdentityValid(id));id=Id{};id.params_version=2;check(!IdentityValid(id));
 check(SafeReuse(true,true,true));for(unsigned k=0;k<3;k++)check(!SafeReuse(k!=0,k!=1,k!=2));
 check(CanPollPrior(9,10));check(!CanPollPrior(10,10));check(!CanPollPrior(11,10));check(!CanPollPrior(0,10));
 check(CanAssociate(100,101,10,10));check(!CanAssociate(100,102,10,11));check(!CanAssociate(100,101,10,9));check(!CanAssociate(UINT64_MAX,0,10,11));
 namespace W=Vulkan::NativePreSrWire;auto c=W::parse(W::config_expected("cpu-120","resident",std::string(32,'a'),std::string(32,'b')).dump());W::config(c,"cpu-120","resident");check(W::u(c,"max_jobs")==120);
 for(auto value:{W::J(true),W::J(4u),W::J(119u),W::J(121u)}){auto x=c;x["max_jobs"]=value;bool rejected=false;try{W::config(x,"cpu-120","resident");}catch(...){rejected=true;}check(rejected);}
 for(uint64_t job:{0ull,63ull,64ull,119ull}){
  const auto path=W::job_path("/owned","input","cpu-120",job);
  check(path=="/owned/frames/bucket-"+std::string(job<64?"000000":"000001")+"/async-input-cpu-120-g0-f"+std::to_string(job)+".json");
 }
 bool rejected=false;try{W::job_path("/owned","input","cpu-120",120);}catch(...){rejected=true;}check(rejected);
 W::Counts counts;for(uint64_t job=0;job<120;job++){W::J x{{"job",job},{"origin_source",100+job},{"origin_epoch",1u},{"origin_history",1u}};counts.finalize(x,"captured");}
 check(counts.finalized==120&&counts.captured==120);counts.check(counts.fields());check(true);
 rejected=false;try{counts.finalize(W::J{{"job",120u}},"captured");}catch(...){rejected=true;}check(rejected);
 W::Counts empty;empty.check(empty.fields());check(!W::b(empty.fields(),"source_frame_valid"));
 // Real CPU threads: a consumed ack alone never grants a still-inflight slot.
 std::atomic<bool>done{false},consumed{false};std::atomic<unsigned>phase{0};
 std::jthread worker([&]{consumed.store(true,std::memory_order_release);phase.store(1,std::memory_order_release);while(phase.load(std::memory_order_acquire)!=2)std::this_thread::yield();done.store(true,std::memory_order_release);});
 while(phase.load(std::memory_order_acquire)!=1){std::this_thread::yield();}
 check(!SafeReuse(done.load(),consumed.load(),true));
 phase.store(2,std::memory_order_release);worker.join();check(SafeReuse(done.load(),consumed.load(),true));
 std::printf("{\"status\":\"PASS\",\"cases\":%u,\"scope\":\"private120_identity_paths_counts_actual_proof_state_CPU_only\",\"GPU\":\"NOT_RUN\"}\n",n);
}
