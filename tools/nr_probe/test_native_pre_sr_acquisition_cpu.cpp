// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/native_pre_sr_acquisition_state.h"
#include "video_core/renderer_vulkan/native_async_pre_sr_policy.h"
#include <cstdio>
#include <stdexcept>
using BbNativePreSrAcquisition::State;
int main(){unsigned checks=0;const auto check=[&](bool b){++checks;if(!b)throw std::runtime_error("C metadata/proof decision FAIL");};
 const auto frame=[](uint64_t s){BbNativeAsync::Frame f;f.source=s;f.copy_tick=s;return f;};
 State s;s.Admit(0,frame(100));check(s.StartPending(0));check(!s.StartPending(0));
 s.Admit(1,frame(101));s.Admit(2,frame(102));check(s.PendingToCurrent());check(s.Capture(9));check(s.active_origin.source==100&&s.active_current==102);
 s.FinishPending();s.Admit(3,frame(103));check(s.AdvanceActive());s.active_current=103;
 check(s.StartPending(3));check(s.Protected(3));check(s.active_origin.source==100); // pending origin != actual active edit
 s.Admit(4,frame(104));check(s.AdvanceActive());s.active_current=104;
 check(s.PendingToCurrent());s.Admit(1,frame(105));check(!s.AdvanceActive());check(s.PendingToCurrent());check(s.Capture(10));check(s.active_origin.source==103&&s.active_current==105);
 auto chosen=s.Select([](unsigned){return false;});check(!chosen); // no proof => no overwrite/wait
 check(!s.Select([&](unsigned i){return s.Protected(i); })); // peer-held suffix wins even when GPU-ready
 auto reset=frame(106);reset.sr_reset=true;s.Admit(0,reset);check(!s.PendingToCurrent()&&!s.active);check(s.Protected(3)&&!s.Protected(4));
 s.FinishPending();s.Admit(2,frame(108));check(s.packets[2].history==3);check(!s.CanAdmit(frame(108)));s.RejectCurrent();check(s.current==5&&!s.active);
 BbNativeAsyncPreSr::SourceLease lease;lease.native.copy=7;lease.sr_read=9;lease.native.merge=10;lease.CompleteNative(9);check(!lease.CanReuse());lease.CompleteNative(10);check(lease.CanReuse());lease.native.peer_read=true;check(!lease.CanReuse());lease.native.peer_read=false;lease.native.unknown=true;check(!lease.CanReuse());
 std::printf("{\"status\":\"PASS_CPU_ACQUISITION_IDENTITY_AND_ACTUAL_LEASE_DECISIONS\",\"checks\":%u,\"GPU\":\"NOT_RUN\"}\n",checks);
}
