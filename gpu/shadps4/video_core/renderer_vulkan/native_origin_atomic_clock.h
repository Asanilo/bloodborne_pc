// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
namespace Vulkan::NativeOriginAtomicClock {
using Clock=std::chrono::steady_clock;
struct Observation {
 enum Stage {Open,Write,Fsync,Close,Rename,StageCount};
 std::array<uint64_t,StageCount> ns{};
 std::array<int,StageCount> errors{};
 bool success=false;
 int failure_errno=0;
 static constexpr uint64_t SlowNs=10000000;
 static bool Slow(uint64_t total)noexcept{return total>SlowNs;}
 template<class F> auto Measure(Stage stage,F&& call){
  const auto start=Clock::now();const auto result=call();
  const int error=result<0?errno:0;
  const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
  ns[stage]+=uint64_t(elapsed);if(error)errors[stage]=error;
  // Preserve syscall errno even if observation code changes it.
  if(error)errno=error;
  return result;
 }
 void Print(FILE* stream,const std::string& path,uint64_t total)const noexcept {
  if(!Slow(total))return;
  const char* basename=path.c_str();for(const char* p=basename;*p;++p)if(*p=='/')basename=p+1;
  char safe[256];size_t n=0;
  for(const unsigned char* p=reinterpret_cast<const unsigned char*>(basename);*p;++p){const auto c=*p;if(n==sizeof(safe)-1)break;safe[n++]=(c>=33&&c<=126&&c!='='&&c!='\\')?char(c):'_';}safe[n]=0;
  const auto stages=ns[Open]+ns[Write]+ns[Fsync]+ns[Close]+ns[Rename];
  std::fprintf(stream,"NATIVE_ORIGIN_ATOMIC_SLOW basename=%s total_ns=%llu open_ns=%llu write_ns=%llu fsync_ns=%llu close_ns=%llu rename_ns=%llu other_ns=%llu success=%u errno=%d open_errno=%d write_errno=%d fsync_errno=%d close_errno=%d rename_errno=%d clock=native_steady scope=background_wall_syscall_intervals_including_descheduling_not_CPU_or_GPU_time GPU_proof=NOT_MEASURED\n",
   safe,(unsigned long long)total,(unsigned long long)ns[Open],(unsigned long long)ns[Write],(unsigned long long)ns[Fsync],(unsigned long long)ns[Close],(unsigned long long)ns[Rename],(unsigned long long)(total>=stages?total-stages:0),unsigned(success),failure_errno,errors[Open],errors[Write],errors[Fsync],errors[Close],errors[Rename]);
 }
};
struct Report {
 const std::string& path;Observation sample;const Clock::time_point start=Clock::now();
 ~Report()noexcept {
  const auto total=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
  // Output is after completion/cleanup and excluded from the measured duration.
  sample.Print(stdout,path,uint64_t(total));
 }
};
inline void Publish(const std::string& path,const std::string& text){
 Report report{path,{}};auto& m=report.sample;
 const auto tmp=path+".tmp-"+std::to_string(getpid());
 const int fd=m.Measure(Observation::Open,[&]{return open(tmp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);});
 if(fd<0){m.failure_errno=errno;throw std::runtime_error("async atomic open");}
 size_t n=0;
 while(n<text.size()){
  const auto count=m.Measure(Observation::Write,[&]{return write(fd,text.data()+n,text.size()-n);});
  if(count<0&&errno==EINTR)continue;
  if(count<=0){m.failure_errno=count<0?errno:0;m.Measure(Observation::Close,[&]{return close(fd);});unlink(tmp.c_str());throw std::runtime_error("async write");}
  n+=size_t(count);
 }
 if(m.Measure(Observation::Fsync,[&]{return fsync(fd);})){m.failure_errno=errno;m.Measure(Observation::Close,[&]{return close(fd);});unlink(tmp.c_str());throw std::runtime_error("async fsync");}
 // Keep original behavior: a close error is observed, not turned into a new failure.
 m.Measure(Observation::Close,[&]{return close(fd);});
 if(m.Measure(Observation::Rename,[&]{return rename(tmp.c_str(),path.c_str());})){m.failure_errno=errno;unlink(tmp.c_str());throw std::runtime_error("async rename");}
 m.success=true;
}
} // namespace Vulkan::NativeOriginAtomicClock
