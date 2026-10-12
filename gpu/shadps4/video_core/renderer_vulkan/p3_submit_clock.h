// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>

namespace Vulkan::P3Clock {
inline uint64_t NewContext() noexcept {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1,std::memory_order_relaxed); // zero on wrap => invalid telemetry
}
inline int64_t NowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
template<class T> uint64_t OpaqueKey(T value) noexcept {
    static_assert(std::is_trivially_copyable_v<T> && sizeof(T)<=sizeof(uint64_t));
    uint64_t key=0;std::memcpy(&key,&value,sizeof(T));return key; // never dereference a handle
}
enum : uint32_t { Enter=1,Returned=2,Wait=4,Stopped=8,Verified=16,Bad=32,
    EnterClaim=64,ReturnClaim=128,WaitClaim=256,StopClaim=512 };
struct View {
    bool coherent=false,exact=false;
    uint32_t flags=0;
    uint64_t actual_context=0,actual_timeline=0,actual_tick=0;
    int64_t enter=0,returned=0,wait=0,observed=0;
    int32_t api_result=0;
};
// Independent data only. Shared by the producer and existing SubmitInfo async closure;
// no raw Scheduler/Bridge pointer, Vulkan resource ownership, wait or control operation.
class Record {
public:
    Record(uint64_t c,uint64_t t,uint64_t v):context(c),timeline(t),tick(v){}
    const uint64_t context,timeline,tick;
    void SubmitEnter(uint64_t c,uint64_t t,uint64_t v,int64_t ns=NowNs()) noexcept {
        if(!Claim(EnterClaim))return;
        ac=c;at=t;av=v;enter=ns;exact=context&&timeline&&tick&&c==context&&t==timeline&&v==tick;
        flags.fetch_or(Enter,std::memory_order_release);
    }
    void SubmitReturned(int32_t result,int64_t ns=NowNs()) noexcept {
        if(!Claim(ReturnClaim))return;
        if(!(flags.load(std::memory_order_acquire)&Enter))flags.fetch_or(Bad,std::memory_order_release);
        code=result;returned=ns;flags.fetch_or(Returned,std::memory_order_release);
    }
    void WaitEntered(int64_t ns=NowNs()) noexcept {
        if(!Claim(WaitClaim))return;
        wait=ns;flags.fetch_or(Wait,std::memory_order_release);
    }
    void WaitObserved(bool actual_tick_verified,int64_t ns=NowNs()) noexcept {
        if(!Claim(StopClaim))return;
        if(!(flags.load(std::memory_order_acquire)&Wait))flags.fetch_or(Bad,std::memory_order_release);
        observed=ns;flags.fetch_or(Stopped|(actual_tick_verified?static_cast<uint32_t>(Verified):uint32_t{0}),std::memory_order_release);
    }
    View Read() const noexcept {
        for(unsigned attempt=0;attempt<3;++attempt) {
            View v;v.flags=flags.load(std::memory_order_acquire);
            if(v.flags&Enter){v.actual_context=ac.load();v.actual_timeline=at.load();v.actual_tick=av.load();v.enter=enter.load();v.exact=exact.load();}
            if(v.flags&Returned){v.returned=returned.load();v.api_result=code.load();}
            if(v.flags&Wait)v.wait=wait.load();
            if(v.flags&Stopped)v.observed=observed.load();
            if(flags.load(std::memory_order_acquire)==v.flags){v.coherent=true;return v;}
        }
        return {}; // bounded read, never spin/wait for writer
    }
private:
    bool Claim(uint32_t bit) noexcept {
        if(flags.fetch_or(bit,std::memory_order_acq_rel)&bit){flags.fetch_or(Bad,std::memory_order_release);return false;}
        return true;
    }
    std::atomic<uint32_t> flags{0};
    std::atomic<bool> exact{false};
    std::atomic<uint64_t> ac{0},at{0},av{0};
    std::atomic<int64_t> enter{0},returned{0},wait{0},observed{0};
    std::atomic<int32_t> code{0};
};
struct Bounds { int64_t pre_call_wait_ns,driver_call_overlap_ns,post_return_wait_ns; };
inline std::optional<Bounds> WaitBounds(const View& v) noexcept {
    constexpr uint32_t required=Enter|Returned|Wait|Stopped|Verified;
    if(!v.coherent||!v.exact||(v.flags&required)!=required||(v.flags&Bad)||v.api_result!=0||
       v.enter<=0||v.returned<v.enter||v.wait<=0||v.observed<v.wait||v.observed<v.enter)return {};
    // Completion may precede API return! Enqueue instant inside driver call is UNKNOWN.
    const auto overlap=std::min(v.returned,v.observed)-std::max(v.enter,v.wait);
    return Bounds{std::max<int64_t>(0,v.enter-v.wait),std::max<int64_t>(0,overlap),
                  std::max<int64_t>(0,v.observed-std::max(v.returned,v.wait))};
}
} // namespace Vulkan::P3Clock
