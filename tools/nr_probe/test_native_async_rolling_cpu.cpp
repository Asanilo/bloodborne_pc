// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/native_async_rolling_policy.h"
#include <cstdio>
#include <cstdlib>
using namespace BbNativeAsync;
namespace R=BbNativeAsync::Rolling;
int main(){unsigned n=0;const auto check=[&](bool b){++n;if(!b)std::abort();};
 check(R::Capacity==7&&NodeCount==5&&MaxLinks==4);
 check(R::ValidConfiguration("0")&&R::ValidConfiguration("1"));for(const auto*s:{"","2","01","true"})check(!R::ValidConfiguration(s));
 std::array<std::optional<Frame>,7>frames{};const unsigned physical[]{6,1,5,0,2};
 for(unsigned k=0;k<5;k++)frames[physical[k]]=Frame{.source=100+k,.previous=k?99+k:0,.epoch=3,.history=2,.copy_tick=200+k};
 const auto origin=*frames[6];auto span=R::FindSpan(frames,origin,104);check(span&&span.size==5);
 for(unsigned k=0;k<5;k++)check(span.slots[k]==physical[k]);
 check(!R::FindSpan(frames,origin,105));check(!R::FindSpan(frames,origin,99));
 for(unsigned k=0;k<5;k++){auto bad=frames;bad[physical[k]].reset();check(!R::FindSpan(bad,origin,104));}
 for(unsigned k=1;k<5;k++)for(unsigned field=0;field<5;field++){
  auto bad=frames;auto&f=*bad[physical[k]];
  if(field==0){f.previous--;}if(field==1){f.epoch++;}if(field==2){f.history++;}if(field==3){f.params++;}if(field==4){f.sr_reset=true;}
  check(!R::FindSpan(bad,origin,104));
 }
 auto duplicate=frames;duplicate[3]=origin;check(!R::FindSpan(duplicate,origin,104));
 // Actual pending origin differs from still-valid displayed origin; no age rejuvenation.
 const auto pending=*frames[5];check(pending.source==102);auto pending_span=R::FindSpan(frames,pending,104);
 check(pending_span&&pending_span.size==3);check(R::AgeValid(origin.source,104)&&R::AgeValid(pending.source,104));
 check(!R::AgeValid(origin.source,105)&&R::AgeValid(pending.source,105));
 check(R::PinForLineage(*frames[0],origin,104));check(!R::PinForLineage(origin,pending,104));
 check(R::CanReuse(200,200,false,false,false));check(!R::CanReuse(200,199,false,false,false));
 check(!R::CanReuse(200,999,true,false,false));check(!R::CanReuse(200,999,false,true,false));check(!R::CanReuse(200,999,false,false,true));check(!R::CanReuse(0,999,false,false,false));
 std::array<std::optional<Frame>,7>edge{};Frame max{.source=UINT64_MAX,.epoch=1,.history=1,.copy_tick=1};edge[4]=max;
 auto one=R::FindSpan(edge,max,UINT64_MAX);check(one&&one.size==1&&one.slots[0]==4);
 // Frame5 absent remains a gap, not a generated link or prior-frame NR image.
 auto broken=frames;broken[0].reset();check(!R::FindSpan(broken,pending,104));
 std::printf("{\"status\":\"PASS\",\"checks\":%u,\"scope\":\"CPU_actual_packet_map_age4_gap_peer_unknown_pin_no_GPU_proof\",\"GPU\":\"NOT_RUN\"}\n",n);
}
