#include "video_core/renderer_vulkan/p3_submit_clock.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <semaphore>
#include <thread>
#include <vector>
using namespace Vulkan::P3Clock;
static std::atomic<unsigned long> checks{0};
static void Check(bool ok,const char* msg){checks.fetch_add(1);if(!ok){std::cerr<<"FAIL "<<msg<<'\n';std::abort();}}
static void BoundsEqual(const View& v,int64_t a,int64_t b,int64_t c){auto x=WaitBounds(v);Check(bool(x),"bounds measured");if(x){Check(x->pre_call_wait_ns==a,"pre-call");Check(x->driver_call_overlap_ns==b,"driver overlap");Check(x->post_return_wait_ns==c,"post-return");}}
static void Populate(Record& r,int32_t result=0,bool verified=true){r.SubmitEnter(1,2,3,10);r.SubmitReturned(result,30);r.WaitEntered(20);r.WaitObserved(verified,40);}
static void CompletionBeforeReturn(){
 auto r=std::make_shared<Record>(1,2,3);std::binary_semaphore enter{0},complete{0},returned{0},allowreturn{0};
 std::thread driver([r,&enter,&allowreturn,&returned]{r->SubmitEnter(1,2,3,50);enter.release();allowreturn.acquire();r->SubmitReturned(0,120);returned.release();});
 std::thread waiter([r,&enter,&complete]{r->WaitEntered(20);enter.acquire();r->WaitObserved(true,80);complete.release();});
 complete.acquire();auto partial=r->Read();Check(partial.coherent&&partial.exact,"completion-first coherent");Check((partial.flags&(Enter|Wait|Stopped|Verified))==(Enter|Wait|Stopped|Verified),"completion-first stages");Check(!(partial.flags&Returned)&&partial.returned==0,"return pending is absent");Check(!WaitBounds(partial),"pending return not measured");allowreturn.release();returned.acquire();BoundsEqual(r->Read(),30,30,0);driver.join();waiter.join();
}
static void PartialSnapshots(){
 auto r=std::make_shared<Record>(1,2,3);std::binary_semaphore next{0},published{0};
 std::thread producer([r,&next,&published]{next.acquire();r->WaitEntered(20);published.release();next.acquire();r->SubmitEnter(1,2,3,50);published.release();next.acquire();r->SubmitReturned(0,70);published.release();next.acquire();r->WaitObserved(true,120);published.release();});
 auto initial=r->Read();Check(initial.coherent&&initial.flags==0&&!WaitBounds(initial),"empty coherent pending");
 next.release();published.acquire();auto v=r->Read();Check(v.coherent&&v.wait==20&&v.enter==0&&!WaitBounds(v),"wait-only pending");
 next.release();published.acquire();v=r->Read();Check(v.coherent&&v.exact&&v.enter==50&&v.returned==0&&!WaitBounds(v),"enter partial");
 next.release();published.acquire();v=r->Read();Check(v.coherent&&v.returned==70&&v.observed==0&&!WaitBounds(v),"return partial");
 next.release();published.acquire();BoundsEqual(r->Read(),30,20,50);producer.join();
}
static void InvalidCases(){
 for(unsigned i=0;i<3;++i){Record r(1,2,3);std::thread t([&]{r.SubmitEnter(i==0?99:1,i==1?99:2,i==2?99:3,10);r.SubmitReturned(0,30);});r.WaitEntered(20);t.join();r.WaitObserved(true,40);Check(!r.Read().exact&&!WaitBounds(r.Read()),"three independent binding mismatches");}
 for(int i=0;i<4;++i){Record r(1,2,3);Populate(r);std::thread t([&]{switch(i){case 0:r.SubmitEnter(9,9,9,99);break;case 1:r.SubmitReturned(-4,99);break;case 2:r.WaitEntered(99);break;case 3:r.WaitObserved(false,99);break;}});t.join();auto v=r.Read();Check((v.flags&Bad)&&!WaitBounds(v),"duplicate invalidates");Check(v.enter==10&&v.returned==30&&v.wait==20&&v.observed==40&&v.api_result==0&&v.exact,"duplicate does not overwrite");}
 for(int code:{-4,1,2}){Record r(1,2,3);std::thread t([&]{Populate(r,code);});t.join();Check(r.Read().api_result==code&&!WaitBounds(r.Read()),"non-success API not completed telemetry");}
 {Record r(1,2,3);Populate(r,0,false);Check(!WaitBounds(r.Read()),"success without target verified");}
 {Record r(1,2,3);r.SubmitReturned(0,30);r.SubmitEnter(1,2,3,10);r.WaitEntered(20);r.WaitObserved(true,40);Check(!WaitBounds(r.Read())&&(r.Read().flags&Bad),"return before enter");}
 {Record r(1,2,3);r.SubmitEnter(1,2,3,10);r.SubmitReturned(0,30);r.WaitObserved(true,40);r.WaitEntered(20);Check(!WaitBounds(r.Read())&&(r.Read().flags&Bad),"observe before wait");}
 for(int i=0;i<5;++i){Record r(1,2,3);r.SubmitEnter(1,2,3,i==0?0:10);r.SubmitReturned(0,i==1?9:30);r.WaitEntered(i==2?0:20);r.WaitObserved(true,i==3?19:i==4?9:40);Check(!WaitBounds(r.Read()),"invalid monotonic order rejected");}
 {Record r(0,2,3);r.SubmitEnter(0,2,3,10);r.SubmitReturned(0,30);r.WaitEntered(20);r.WaitObserved(true,40);Check(!WaitBounds(r.Read()),"zero binding invalid");}
 {Record r(1,2,3);r.SubmitEnter(1,2,3,10);r.SubmitReturned(0,20);r.WaitEntered(30);r.WaitObserved(true,40);BoundsEqual(r.Read(),0,0,10);}
 {Record r(1,2,3);r.SubmitEnter(1,2,3,10);r.SubmitReturned(0,20);r.WaitEntered(5);r.WaitObserved(true,10);BoundsEqual(r.Read(),5,0,0);}
}
static void SharedLifetime(){
 auto producer=std::make_shared<Record>(1,2,3);std::weak_ptr<Record> weak=producer;std::binary_semaphore ready{0},go{0};
 std::thread closure([owner=producer,&ready,&go]{ready.release();go.acquire();Populate(*owner);BoundsEqual(owner->Read(),0,10,10);});ready.acquire();producer.reset();Check(!weak.expired(),"async closure owns after producer gone");go.release();closure.join();Check(weak.expired(),"released after closure last-use");
 std::shared_ptr<Record> optional;Check(!optional,"optional disabled has no owner");
}
static void ConcurrentReads(){
 std::atomic<unsigned long> samples{0};
 for(unsigned i=0;i<300;++i){Record r(1,2,3);std::binary_semaphore start{0};std::atomic<unsigned> done{0};
  std::thread submit([&]{start.acquire();r.SubmitEnter(1,2,3,10);r.SubmitReturned(0,30);done.fetch_add(1,std::memory_order_release);});
  std::thread wait([&]{start.acquire();r.WaitEntered(20);r.WaitObserved(true,40);done.fetch_add(1,std::memory_order_release);});
  start.release();start.release();
  do{const auto v=r.Read();++samples;if(v.coherent){if(v.flags&Enter)Check(v.exact&&v.actual_context==1&&v.actual_timeline==2&&v.actual_tick==3&&v.enter==10,"coherent entered payload");if(v.flags&Returned)Check(v.returned==30&&v.api_result==0,"coherent return payload");if(v.flags&Wait)Check(v.wait==20,"coherent wait payload");if(v.flags&Stopped)Check(v.observed==40,"coherent observation payload");if(auto b=WaitBounds(v))Check(b->pre_call_wait_ns==0&&b->driver_call_overlap_ns==10&&b->post_return_wait_ns==10,"concurrent partitions");}}
  while(done.load(std::memory_order_acquire)!=2);submit.join();wait.join();BoundsEqual(r.Read(),0,10,10);
 }
 Check(samples.load()>=300,"actual concurrent snapshot samples");std::cout<<"{\"concurrent_read_samples\":"<<samples.load()<<",\"checks\":"<<checks.load()<<",\"status\":\"PASS\"}\n";
}
static void Contexts(){
 std::vector<std::vector<uint64_t>> ids(4);std::vector<std::thread> threads;
 for(unsigned i=0;i<4;++i)threads.emplace_back([&,i]{for(unsigned j=0;j<512;++j)ids[i].push_back(NewContext());});
 for(auto& t:threads){t.join();}
 std::vector<uint64_t> all;for(auto& v:ids)all.insert(all.end(),v.begin(),v.end());std::sort(all.begin(),all.end());Check(all.front()!=0&&std::adjacent_find(all.begin(),all.end())==all.end(),"concurrent context uniqueness");
 Check(OpaqueKey(uint64_t{0x123456789})==0x123456789,"opaque numeric key");int x;Check(OpaqueKey(&x)!=0,"opaque pointer never dereferenced");Check(NowNs()>0,"native monotonic timestamp");
}
int main(){CompletionBeforeReturn();PartialSnapshots();InvalidCases();SharedLifetime();Contexts();ConcurrentReads();}
