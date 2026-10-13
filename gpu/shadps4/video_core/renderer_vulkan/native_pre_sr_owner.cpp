// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_pre_sr_owner.h"
#include "native_pre_sr_policy.h"
#include "native_pre_sr_wire.h"
#include "native_origin_identity_policy.h"
#include "vk_instance.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
extern char** environ;
namespace Vulkan {
namespace {
namespace W=NativePreSrWire;using J=W::J;using Clock=std::chrono::steady_clock;
std::string Env(const char*k){const char*p=std::getenv(k);return p?p:"";}
std::string Read(const std::string&p){std::error_code e;auto n=std::filesystem::file_size(p,e);if(!e&&n>65536)throw std::runtime_error("async control64KiB");std::ifstream f(p,std::ios::binary);return f?std::string(std::istreambuf_iterator<char>(f),{}):std::string{};}
std::string Proc(const std::string&p){std::ifstream f(p,std::ios::binary);return f?std::string(std::istreambuf_iterator<char>(f),{}):std::string{};}
void Atomic(const std::string&p,const std::string&t){auto tmp=p+".tmp-"+std::to_string(getpid());int fd=open(tmp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(fd<0)throw std::runtime_error("async atomic open");size_t n=0;while(n<t.size()){auto w=write(fd,t.data()+n,t.size()-n);if(w<0&&errno==EINTR)continue;if(w<=0){close(fd);unlink(tmp.c_str());throw std::runtime_error("async write");}n+=w;}if(fsync(fd)){close(fd);unlink(tmp.c_str());throw std::runtime_error("async fsync");}close(fd);if(rename(tmp.c_str(),p.c_str())){unlink(tmp.c_str());throw std::runtime_error("async rename");}}
std::string Normalize(std::string p){std::replace(p.begin(),p.end(),'\\','/');if(p.rfind("Z:",0)==0)p.erase(0,2);return p;}
uint64_t Ticks(pid_t p){auto t=Proc("/proc/"+std::to_string(p)+"/stat");auto c=t.rfind(')');if(c==std::string::npos)return 0;std::istringstream in(t.substr(c+2));std::string v;for(unsigned i=0;i<=19;i++){if(!(in>>v))return 0;if(i==0&&(v=="Z"||v=="X"||v=="x"))return 0;if(i==19){try{return std::stoull(v);}catch(...){return 0;}}}return 0;}
pid_t Parent(pid_t p){std::istringstream in(Proc("/proc/"+std::to_string(p)+"/status"));std::string l;while(std::getline(in,l))if(l.rfind("PPid:",0)==0){try{return std::stol(l.substr(5));}catch(...){return 0;}}return 0;}
bool Descendant(pid_t p,pid_t a){for(unsigned n=0;n<128&&p>1;n++){if(p==a)return true;p=Parent(p);}return false;}
using Identity= BbNativeAsync::IdentityPolicy::Candidate;
struct Candidates {
 J records=J::array();std::map<pid_t,std::string> last;
 unsigned scans=0,accepted=0;bool truncated=false;
 void Observe(const Identity&c,const BbNativeAsync::IdentityPolicy::Decision&d){
  J x{{"unix_pid",c.pid},{"start_before",c.start_before},{"start_after",c.start_after},
   {"descendant_game",c.descendant_game},{"descendant_launcher",c.descendant_launcher},
   {"actual_elf",c.elf},{"argv",c.argv},{"exact_pe_maps",c.exact_pe_maps},
   {"accepted",d.Accepted()},{"checks",{{"pid",d.pid},{"ancestry",d.ancestry},
    {"start_stable",d.start_stable},{"exact_argv",d.argv},{"configured_elf_pair",d.elf},{"exact_pe_maps",d.maps}}}};
  auto key=x.dump();if(last.contains(pid_t(c.pid))&&last.at(pid_t(c.pid))==key)return;
  if(records.size()>=8){truncated=true;return;}
  last[pid_t(c.pid)]=key;
  for(auto&v:x["argv"])if(v.get_ref<const std::string&>().size()>512)v=v.get<std::string>().substr(0,512);
  records.push_back(x);
 }
 J Report()const{return J{{"scope","owned_descendant_identity_candidates_only"},{"scans",scans},
  {"last_accepted_candidates",accepted},{"snapshot_limit",8u},{"truncated",truncated},{"observations",records}};}
};
Identity ReadCandidate(pid_t pid,pid_t launcher,const std::string&helper){
 Identity c{.pid=pid,.game=getpid(),.launcher=launcher,.start_before=Ticks(pid),
  .descendant_game=Descendant(pid,getpid()),.descendant_launcher=Descendant(pid,launcher)};
 auto raw=Proc("/proc/"+std::to_string(pid)+"/cmdline");size_t first=0;
 for(size_t k=0;k<=raw.size();k++)if(k==raw.size()||!raw[k]){
  if(k>first)c.argv.push_back(Normalize(raw.substr(first,k-first)));first=k+1;
 }
 std::array<char,4096>b{};auto n=readlink(("/proc/"+std::to_string(pid)+"/exe").c_str(),b.data(),b.size()-1);
 if(n>0)c.elf.assign(b.data(),size_t(n));
 std::istringstream maps(Proc("/proc/"+std::to_string(pid)+"/maps"));
 std::string line;while(std::getline(maps,line))if(line.ends_with(" "+helper))c.exact_pe_maps=true;
 c.start_after=Ticks(pid);return c;
}
std::pair<pid_t,uint64_t> Find(const BbNativeAsync::IdentityPolicy::Expected&e,pid_t launcher,Candidates&report){
 ++report.scans;std::vector<std::pair<pid_t,uint64_t>>found;
 for(auto&entry:std::filesystem::directory_iterator("/proc")){
  auto name=entry.path().filename().string();
  if(name.empty()||!std::all_of(name.begin(),name.end(),[](char c){return c>='0'&&c<='9';}))continue;
  auto pid=pid_t(std::strtol(name.c_str(),nullptr,10));
  if(pid<=1||pid==getpid()||!Descendant(pid,getpid())||!Descendant(pid,launcher))continue;
  auto c=ReadCandidate(pid,launcher,e.helper);
  auto d=BbNativeAsync::IdentityPolicy::Check(c,e);
  // Only relevant owned candidates; no unrelated host process snapshots.
  if(c.exact_pe_maps||(!c.argv.empty()&&(c.argv[0]==e.helper||
    c.argv[0]==std::filesystem::path(e.helper).filename())))report.Observe(c,d);
  if(d.Accepted())found.emplace_back(pid,c.start_after);
 }
 report.accepted=unsigned(found.size());
 return found.size()==1?found.front():std::pair<pid_t,uint64_t>{-1,0};
}
struct Fd {int fd=-1;std::string target;uint64_t dev=0,ino=0;bool stat_ok=false;};
std::vector<Fd> Inventory(pid_t p,const std::string&output){std::vector<Fd>v;J j=J::array();for(auto&e:std::filesystem::directory_iterator("/proc/"+std::to_string(p)+"/fd")){auto name=e.path().filename().string();char*end=nullptr;auto n=std::strtol(name.c_str(),&end,10);if(!end||*end||n<0||n>INT32_MAX)continue;std::array<char,4096>b{};auto count=readlink(e.path().c_str(),b.data(),b.size()-1);struct stat st{};Fd r{.fd=int(n),.target=count>0?std::string(b.data(),count):"<UNREADABLE>"};r.stat_ok=stat(e.path().c_str(),&st)==0;if(r.stat_ok){r.dev=st.st_dev;r.ino=st.st_ino;}v.push_back(r);}
 std::sort(v.begin(),v.end(),[](const auto&a,const auto&b){return a.fd<b.fd;});for(auto&r:v)j.push_back({{"fd",r.fd},{"target",r.target},{"device",r.dev},{"inode",r.ino},{"stat_ok",r.stat_ok}});if(!output.empty())Atomic(output,j.dump()+"\n");return v;
}
bool Payload(const Fd&a,const Fd&b){return a.stat_ok&&b.stat_ok&&a.dev==b.dev&&a.ino==b.ino&&a.target==b.target;}
struct Imports {vk::Device device;std::array<vk::Image,4>images{};std::array<vk::DeviceMemory,4>memory{};std::array<vk::ImageView,4>views{};bool destroyed=false;
 void Destroy(){if(destroyed)return;for(unsigned k=0;k<4;k++){if(views[k])device.destroyImageView(views[k]);if(images[k])device.destroyImage(images[k]);if(memory[k])device.freeMemory(memory[k]);views[k]=nullptr;images[k]=nullptr;memory[k]=nullptr;}destroyed=true;}
 ~Imports(){if(!destroyed)std::fprintf(stderr,"NATIVE_PRE_SR_PEER_IMPORTS_HELD no_safe_destroy_proof=1\n");}
};
std::string Hex(const uint8_t*p){static constexpr char h[]="0123456789abcdef";std::string s;for(unsigned k=0;k<16;k++){s+=h[p[k]>>4];s+=h[p[k]&15];}return s;}
}
NativePreSrOwner::~NativePreSrOwner(){if(worker.joinable()){worker.request_stop();worker.join();}}
bool NativePreSrOwner::Enabled()const noexcept {const auto m=Env("BB_NR_NATIVE_ORIGIN_MODE");return BbPreSr::Enabled()&&(m=="resident"||m=="skipEvaluate");}
bool NativePreSrOwner::Start(const Instance&i){if(!Enabled())return false;bool expected=false;if(!started.compare_exchange_strong(expected,true))return State()!=Phase::Failed;session=Env("BB_NR_NATIVE_PRE_SR_SESSION");mode=Env("BB_NR_NATIVE_ORIGIN_MODE");run=Env("BB_NR_NATIVE_PRE_SR_RUN_DIR");helper=Env("BB_NR_NATIVE_ORIGIN_HELPER");wine=Env("BB_NR_NATIVE_ORIGIN_WINE");preloader=Env("BB_NR_NATIVE_ORIGIN_PRELOADER");prefix=Env("BB_NR_NATIVE_ORIGIN_PREFIX");
 const auto abs=[](const auto&s){return !s.empty()&&s[0]=='/';};
 if(session.empty()||session.size()>63||!abs(run)||run.rfind("/home/arinp/Code/nrbridge-design/game-poc-20261011/bbport/runs/",0)!=0||!abs(helper)||!abs(wine)||!abs(preloader)||!abs(prefix)||prefix!=run+"/profile/wine-prefix"){phase.store(Phase::Failed);empty_imports_destroyed.store(true);worker_finished.store(true);std::fprintf(stderr,"NATIVE_PRE_SR_PEER_ENV_INVALID no_spawn=1\n");return false;}phase.store(Phase::Bootstrap);worker=std::jthread([this,&i](std::stop_token s){Run(s,i);worker_finished.store(true,std::memory_order_release);});return true;}
std::shared_ptr<NativePreSrOwner::Job> NativePreSrOwner::TryReserve(const BbNativeAsync::Frame&f){std::unique_lock lock(mutex,std::try_to_lock);if(!lock||State()!=Phase::Ready||stop_reason.load()||finalized.load()>=4||f.source<=last_recorded_origin)return {};auto next=std::make_shared<Job>();next->id={session,session+"-g0",0,finalized.load(),f.source,f.epoch,f.history,f.params};if(!next->id.Matches(f)||!registry.Valid(next->id))return {};next->origin=f;next->resources=registry;job=next;phase.store(Phase::Reserved,std::memory_order_release);return next;}
std::shared_ptr<NativePreSrOwner::Job> NativePreSrOwner::TryJob(){std::unique_lock lock(mutex,std::try_to_lock);return lock?job:nullptr;}
void NativePreSrOwner::CancelReservation(const std::shared_ptr<Job>&j){std::unique_lock lock(mutex,std::try_to_lock);if(lock&&job==j&&State()==Phase::Reserved){job.reset();phase.store(Phase::Ready);}}
void NativePreSrOwner::InputRecorded(const std::shared_ptr<Job>&j){std::scoped_lock lock(mutex);if(job!=j||State()!=Phase::Reserved||!j->input->native_target_tick||j->id.origin_source<=last_recorded_origin)throw std::runtime_error("async input reservation mismatch");last_recorded_origin=j->id.origin_source;phase.store(Phase::InputRecorded,std::memory_order_release);}
void NativePreSrOwner::FinalRecorded(const std::shared_ptr<Job>&j,PreSrDisposition d){std::scoped_lock lock(mutex);if(job!=j||State()!=Phase::Complete||!j->final->native_target_tick||d==PreSrDisposition::Unavailable)throw std::runtime_error("async final reservation mismatch");j->disposition=d;phase.store(Phase::FinalRecorded,std::memory_order_release);}
void NativePreSrOwner::FinalizedAck(const std::shared_ptr<Job>&j){std::scoped_lock lock(mutex);if(job==j&&State()==Phase::Finalized&&j->consumed_published.load()){job.reset();phase.store(Phase::Ready,std::memory_order_release);}}
void NativePreSrOwner::RequestStop(unsigned r)noexcept{if(r>=1&&r<=4){unsigned zero=0;stop_reason.compare_exchange_strong(zero,r);}}
void NativePreSrOwner::Fail(const std::string&why)noexcept {phase.store(Phase::Failed,std::memory_order_release);std::fprintf(stderr,"NATIVE_PRE_SR_PEER_QUARANTINE reason=%s normal_release=0 no_gpu_free=1\n",why.c_str());try{if(!run.empty())Atomic(run+"/native.abort",why+"\n");}catch(...){} }
void NativePreSrOwner::Run(std::stop_token stop,const Instance&instance){
 pid_t child=-1,exporter=-1;uint64_t exporter_ticks=0;bool imported_native_used=false;auto imports=std::make_shared<Imports>();imports->device=instance.GetDevice();std::vector<Fd>raw_fds;Candidates candidates;const auto start=Clock::now();
 try{
  for(const auto&p:{run,prefix}){std::filesystem::create_directories(p);W::need(!std::filesystem::is_symlink(p)&&chmod(p.c_str(),0700)==0,"private owned directory");}
  W::need(!std::filesystem::exists(run+"/async-config.json")&&!std::filesystem::exists(run+"/async-pid-request.json"),"no stale async run reuse");std::filesystem::create_directories(run+"/frames/bucket-000000");chmod((run+"/frames").c_str(),0700);chmod((run+"/frames/bucket-000000").c_str(),0700);
  vk::PhysicalDeviceIDProperties identity{};vk::PhysicalDeviceProperties2 props{.pNext=&identity};instance.GetPhysicalDevice().getProperties2(&props);auto config=W::config_expected(session,mode,Hex(identity.deviceUUID.data()),Hex(identity.driverUUID.data()));config=W::parse(config.dump());W::config(config,session,mode);const auto common=W::common(session,mode);
  const auto env=[](const std::string&prefix){std::vector<std::string>v;const std::string ge="/home/arinp/.local/share/Steam/compatibilitytools.d/GE-Proton11-7-x86_64/files/lib/";std::map<std::string,std::string>set{{"WINEPREFIX",prefix},{"WINELOADERNOEXEC","1"},{"WINEDLLOVERRIDES","mscoree,mshtml=;nvapi64=n;dxgi=n"},{"WINEDLLPATH",ge+"vkd3d:"+ge+"wine"},{"DXVK_ENABLE_NVAPI","1"},{"NVIDIA_WINE_DLL_DIR","/usr/lib64/nvidia/wine"},{"WINEDEBUG",Env("BB_NR_NATIVE_ORIGIN_WINEDEBUG").empty()?"-all":Env("BB_NR_NATIVE_ORIGIN_WINEDEBUG")}};for(char**p=environ;p&&*p;p++){std::string s(*p);if(!set.contains(s.substr(0,s.find('='))))v.push_back(s);}for(auto&[k,x]:set)v.push_back(k+"="+x);return v;};
  Atomic(run+"/async-config.json",config.dump()+"\n");std::array<std::string,6>args{preloader,wine,"Z:"+helper,mode,"Z:"+run,session};std::array<char*,7>av{};for(unsigned k=0;k<6;k++)av[k]=args[k].data();auto ev=env(prefix);std::vector<char*>ep;for(auto&s:ev)ep.push_back(s.data());ep.push_back(nullptr);
  int rc=posix_spawn(&child,preloader.c_str(),nullptr,nullptr,av.data(),ep.data());W::need(rc==0,"async posix_spawn absolute64");peer_spawned.store(true,std::memory_order_release);Atomic(run+"/async-native-launch.json",J{{"actual_starts",1u},{"launcher_pid",child},{"game_pid",getpid()},{"protocol_version",1u},{"argv",args}}.dump()+"\n");
  const BbNativeAsync::IdentityPolicy::Expected expected{helper,run,mode,session,
    std::filesystem::canonical(wine).string(),std::filesystem::canonical(preloader).string()};
  const auto check=[&]{W::need(!abort_requested.load(std::memory_order_acquire),"renderer unknown preSR owner abort HOLD");W::need(!stop.stop_requested()&&Clock::now()-start<std::chrono::seconds(300),"owner service300s/stop HOLD");if(exporter>1)W::need(Ticks(exporter)==exporter_ticks&&Descendant(exporter,getpid())&&Descendant(exporter,child),"exporter exact live identity");int status=0;auto p=waitpid(child,&status,WNOHANG);if(p==child){actual_wait_status.store(status);peer_reaped.store(true,std::memory_order_release);Atomic(run+"/async-native-helper-exit.json",J{{"wait_status",status},{"normal_exit",false},{"premature",true}}.dump());throw std::runtime_error("async peer exited before normal terminal");}W::need(p>=0||errno==EINTR,"waitpid owner");};
  const auto wait=[&](const std::string&path,std::chrono::seconds budget){const auto deadline=Clock::now()+budget;while(Clock::now()<deadline){check();auto s=Read(path);if(!s.empty())return W::parse(s);std::this_thread::sleep_for(std::chrono::milliseconds(2));}throw std::runtime_error("async bounded control timeout "+path);};
  auto request=wait(run+"/async-pid-request.json",std::chrono::seconds(60));W::bootstrap(request,common,"async-pid-request.json");W::need(W::s(request,"op")=="pid_request"&&W::s(request,"status")=="PID_REQUEST"&&!W::b(request,"completed_GPU")&&!W::b(request,"output_valid")&&!W::b(request,"model_valid")&&W::u(request,"win_pid")>0&&W::u(request,"win_pid")<=UINT32_MAX&&Normalize(W::s(request,"exe"))==helper,"actual pid request");
  auto identity_limit=Clock::now()+std::chrono::seconds(60);while(exporter<1&&Clock::now()<identity_limit){check();const auto match=Find(expected,child,candidates);exporter=match.first;exporter_ticks=match.second;if(exporter<1)std::this_thread::sleep_for(std::chrono::milliseconds(5));}W::need(exporter>1,"unique actual PE exporter");Atomic(run+"/async-exporter-candidates.json",candidates.Report().dump()+"\n");
  W::need(exporter_ticks>0&&Ticks(exporter)==exporter_ticks,"exporter stable start_ticks");
  W::need(BbNativeAsync::IdentityPolicy::Check(ReadCandidate(exporter,child,helper),expected).Accepted(),"identity immediately before PID reply");auto id=common;id.update(J{{"game_pid",unsigned(getpid())},{"game_start_ticks",Ticks(getpid())},{"launcher_pid",unsigned(child)},{"exporter_pid",unsigned(exporter)},{"exporter_start_ticks",exporter_ticks},{"descendant_game",true},{"descendant_launcher",true},{"request_exe_match",true},{"unique_run_cmd",true},{"exact_pe_maps",true}});W::bootstrap(W::parse(id.dump()),common,"native-exporter-identity.json");Atomic(run+"/native-exporter-identity.json",id.dump()+"\n");Atomic(run+"/async-exporter.pid",std::to_string(exporter)+"\n");
  const auto baseline=wait(run+"/"+W::base("baseline",session),std::chrono::seconds(120));W::bootstrap(baseline,common,"async-baseline-SESSION-g0.json");W::need(W::s(baseline,"op")=="baseline"&&W::s(baseline,"status")=="BASELINE"&&W::b(baseline,"completed_GPU")&&!W::b(baseline,"output_valid")&&!W::b(baseline,"model_valid")&&W::b(baseline,"model_init")&&W::b(baseline,"feature_created")&&W::b(baseline,"creation_GPU_complete")&&W::u(baseline,"feature_create_calls")==1&&W::u(baseline,"creation_GPU_serial")>0,"real model creation baseline");for(auto[k,v]:std::initializer_list<std::pair<const char*,uint64_t>>{{"model_width",1280},{"model_height",720},{"cold_init_timeout_ms",120000},{"control_timeout_ms",60000},{"gpu_timeout_ms",10000},{"job_completion_timeout_ms",30000}})W::need(W::u(baseline,k)==v,"baseline actual profile/budgets");
  auto native_base=Inventory(getpid(),run+"/async-native-fd-baseline.json");Inventory(exporter,run+"/async-wine-fd-baseline.json");auto b=common;b.update(J{{"op","baseline_audit"},{"status","BASELINE_AUDITED"},{"completed_GPU",false},{"output_valid",false},{"model_valid",false},{"helper_creation_GPU_serial_echo",baseline["creation_GPU_serial"]},{"baseline_fd_count",native_base.size()},{"inventory","async-native-fd-baseline.json"},{"helper_inventory","async-wine-fd-baseline.json"}});W::bootstrap(W::parse(b.dump()),common,"async-native-baseline-SESSION-g0.json");Atomic(run+"/"+W::base("native-baseline",session),b.dump()+"\n");
  auto meta=wait(run+"/async-meta-g0.json",std::chrono::seconds(60));W::bootstrap(meta,common,"async-meta-g0.json");W::need(W::s(meta,"op")=="metadata"&&W::s(meta,"status")=="READY"&&W::b(meta,"completed_GPU")&&!W::b(meta,"output_valid")&&!W::b(meta,"model_valid")&&W::u(meta,"linux_pid")==unsigned(exporter)&&W::u(meta,"win_pid")==W::u(request,"win_pid")&&W::s(meta,"device_uuid")==W::s(config,"device_uuid")&&W::s(meta,"driver_uuid")==W::s(config,"driver_uuid")&&W::u(meta,"handle_type")==512&&W::u(meta,"queue_family")<=UINT32_MAX&&W::u(meta,"initial_release_GPU_serial")>W::u(baseline,"creation_GPU_serial")&&meta["resources"].is_array()&&meta["resources"].size()==4,"actual metadata identity");
  for(auto[k,v]:std::initializer_list<std::pair<const char*,uint64_t>>{{"render_width",1280},{"render_height",720},{"model_width",1280},{"model_height",720},{"max_jobs",4},{"initial_history_version",1}})W::need(W::u(meta,k)==v,"shared dimension");
  W::need(meta["manual_white_point"].is_number()&&!meta["manual_white_point"].is_boolean()&&meta["manual_white_point"].get<double>()==1&&!W::b(meta,"passthrough"),"manual profile");
  std::set<uint64_t>fd_numbers;for(unsigned k=0;k<4;k++){W::resource(meta["resources"][k],k);W::need(fd_numbers.insert(W::u(meta["resources"][k],"fd")).second,"unique raw FD numbers");}
  for(auto[k,value]:std::initializer_list<std::pair<const char*,uint64_t>>{{"width",1280},{"height",720},{"render_width",1280},{"render_height",720},{"model_width",1280},{"model_height",720},{"max_jobs",4},{"initial_history_version",1}})W::need(W::u(meta,k)==value,"preSR metadata exact720 profile scalars");
  W::need(meta["manual_white_point"].is_number()&&!meta["manual_white_point"].is_boolean()&&meta["manual_white_point"]==1&&!W::b(meta,"passthrough"),"preSR metadata manualWP1 passthroughfalse");
  auto before=Inventory(exporter,run+"/async-wine-fd-before-import.json");registry.session=session;registry.resource_set=session+"-g0";registry.native_queue_family=instance.GetGraphicsQueueFamilyIndex();registry.import_owner=imports;J pairs=J::array();
  for(unsigned k=0;k<4;k++){check();const auto&e=meta["resources"][k];int raw=W::u(e,"fd");auto found=std::find_if(before.begin(),before.end(),[&](auto&f){return f.fd==raw;});W::need(found!=before.end()&&found->stat_ok,"raw FD readable");for(auto&old:raw_fds)W::need(!Payload(*found,old),"independent payloads");raw_fds.push_back(*found);pairs.push_back({{"role",e["role"]},{"fd",e["fd"]}});
   W::need(Ticks(exporter)==exporter_ticks&&BbNativeAsync::IdentityPolicy::Check(ReadCandidate(exporter,child,helper),expected).Accepted(),"exact identity before pidfd import");
   int pidfd=syscall(SYS_pidfd_open,exporter,0);W::need(pidfd>=0,"pidfd_open");int dup=syscall(SYS_pidfd_getfd,pidfd,raw,0);close(pidfd);W::need(dup>=0,"pidfd_getfd");struct stat st{};std::array<char,4096>link{};auto l=readlink(("/proc/self/fd/"+std::to_string(dup)).c_str(),link.data(),link.size()-1);if(fstat(dup,&st)||l<1||uint64_t(st.st_dev)!=found->dev||uint64_t(st.st_ino)!=found->ino||std::string(link.data(),l)!=found->target){close(dup);throw std::runtime_error("duplicate payload mismatch");}
   try{
    vk::ExternalMemoryImageCreateInfo ex{.handleTypes=vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
    const vk::ImageCreateInfo ci{.pNext=&ex,.imageType=vk::ImageType::e2D,.format=static_cast<vk::Format>(W::u(e,"format")),.extent={unsigned(W::u(e,"width")),unsigned(W::u(e,"height")),1},.mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,.usage=vk::ImageUsageFlags{15},.sharingMode=vk::SharingMode::eExclusive,.initialLayout=vk::ImageLayout::eUndefined};
    auto [cr,image]=imports->device.createImage(ci);W::need(cr==vk::Result::eSuccess,"import image");imports->images[k]=image;import_objects_ever_created.fetch_add(1,std::memory_order_release);
    vk::MemoryDedicatedRequirements dedicated{};vk::MemoryRequirements2 req{.pNext=&dedicated};const vk::ImageMemoryRequirementsInfo2 ri{.image=image};imports->device.getImageMemoryRequirements2(&ri,&req);
    auto [fr,fp]=imports->device.getMemoryFdPropertiesKHR(vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,dup);W::need(fr==vk::Result::eSuccess,"FD memory properties");const auto type=W::u(e,"memory_type_index");
    W::need(req.memoryRequirements.size==W::u(e,"allocation_size")&&req.memoryRequirements.alignment==W::u(e,"alignment")&&req.memoryRequirements.memoryTypeBits==W::u(e,"req_memory_type_bits")&&((fp.memoryTypeBits&req.memoryRequirements.memoryTypeBits)&(1u<<type))&&unsigned(dedicated.prefersDedicatedAllocation)==W::u(e,"prefers_dedicated")&&unsigned(dedicated.requiresDedicatedAllocation)==W::u(e,"requires_dedicated"),"matching actual requirements");
    vk::MemoryDedicatedAllocateInfo de{.image=image};vk::ImportMemoryFdInfoKHR fi{.pNext=&de,.handleType=vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,.fd=dup};
    vk::MemoryAllocateInfo ai{.pNext=&fi,.allocationSize=req.memoryRequirements.size,.memoryTypeIndex=unsigned(type)};auto [ar,mem]=imports->device.allocateMemory(ai);W::need(ar==vk::Result::eSuccess,"allocate FD ownership");dup=-1;imports->memory[k]=mem;
    W::need(imports->device.bindImageMemory(image,mem,0)==vk::Result::eSuccess,"bind imported image");
    auto[vv,view]=imports->device.createImageView({.image=image,.viewType=vk::ImageViewType::e2D,.format=ci.format,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}});W::need(vv==vk::Result::eSuccess,"import view");imports->views[k]=view;
    registry.images[k]=image;registry.views[k]=view;registry.infos[k]=ci;registry.infos[k].pNext=nullptr;
   }catch(...){if(dup>=0)close(dup);throw;}
  }
  auto ack=common;ack.update(J{{"op","imported"},{"status","IMPORTED"},{"completed_GPU",false},{"output_valid",false},{"model_valid",false},{"initial_release_GPU_serial_echo",meta["initial_release_GPU_serial"]},{"imported_roles",J::array({"color","depth","mv","output"})},{"raw_fd_pairs",pairs},{"native_imported_count",4u},{"owned_duplicate_fd_count_after_import",0u}});
  W::bootstrap(W::parse(ack.dump()),common,"async-imported-SESSION-g0.json");Atomic(run+"/"+W::base("imported",session),ack.dump()+"\n");
  auto retired_fd=wait(run+"/"+W::base("fd-retired",session),std::chrono::seconds(60));W::bootstrap(retired_fd,common,"async-fd-retired-SESSION-g0.json");
  W::need(W::s(retired_fd,"op")=="fd_retired"&&W::s(retired_fd,"status")=="FD_RETIRED"&&W::b(retired_fd,"completed_GPU")&&!W::b(retired_fd,"output_valid")&&!W::b(retired_fd,"model_valid")&&retired_fd["closed_fd"]==pairs&&W::u(retired_fd,"initial_release_GPU_serial")==W::u(meta,"initial_release_GPU_serial")&&!W::u(retired_fd,"export_raw_fd_owned"),"raw retirement exact");
  auto consumed_fds=Inventory(exporter,run+"/async-wine-fd-active-retired.json");for(auto&f:consumed_fds)for(auto&raw:raw_fds)W::need(!Payload(f,raw),"raw payload gone");
  registry.actual_import_fd_retired=true;phase.store(Phase::Ready,std::memory_order_release);std::printf("NATIVE_PRE_SR_PEER_READY session=%s actual_imports=4 FD_retired=1 scene_arm=REQUIRED model_phase=UNVERIFIED\n",session.c_str());W::Counts counts;uint64_t last_helper_serial=W::u(meta,"initial_release_GPU_serial");
  const auto terminal=[&](const char*op,const char*status,bool gpu){auto x=common;x.update(counts.fields());x.update(J{{"op",op},{"status",status},{"completed_GPU",gpu},{"output_valid",false},{"model_valid",false}});return x;};
  while(true){check();const auto state=State();if(counts.finalized==4)RequestStop(4);
   if(stop_reason.load()&&state==Phase::Ready){std::scoped_lock lock(mutex);if(State()==Phase::Ready&&!job){phase.store(Phase::StopRequested);break;}}
   auto current=TryJob();
   if(state==Phase::InputRecorded&&current&&current->input->native_GPU_complete.load(std::memory_order_acquire)){
    imported_native_used=true;auto x=common;x.update(J{{"job",current->id.job},{"origin_source",current->id.origin_source},{"origin_epoch",current->id.origin_epoch},{"origin_history",current->id.origin_history},{"op","input"},{"status","ORIGIN_INPUT_READY"},{"native_input_tick",current->input->native_target_tick},{"native_input_GPU_complete",true},{"source_reset",current->origin.sr_reset},{"model_reset",1u},
     {"source_jitter_metadata",{{"viewport_jitter_x",current->origin.viewport[0]},{"viewport_jitter_y",current->origin.viewport[1]},{"previous_viewport_jitter_x",current->origin.previous_viewport[0]},{"previous_viewport_jitter_y",current->origin.previous_viewport[1]},{"sr_jitter_x",current->origin.sr_jitter[0]},{"sr_jitter_y",current->origin.sr_jitter[1]},{"previous_sr_jitter_x",current->origin.previous_sr_jitter[0]},{"previous_sr_jitter_y",current->origin.previous_sr_jitter[1]}}}});
    x=W::parse(x.dump());W::input(x,common,counts.finalized);Atomic(W::job_path(run,"input",session,counts.finalized),x.dump()+"\n");std::printf("NATIVE_PRE_SR_PEER_INPUT_PUBLISHED session=%s job=%llu origin=%llu native_input_tick=%llu native_monotonic_ns=%llu scope=after_successful_atomic_publish clock=native_steady helper_serial=NOT_YET_AVAILABLE\n",
     session.c_str(),(unsigned long long)current->id.job,(unsigned long long)current->id.origin_source,
     (unsigned long long)current->input->native_target_tick,
     (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
    phase.store(Phase::WaitingComplete,std::memory_order_release);
    auto e=wait(W::job_path(run,"complete",session,counts.finalized),std::chrono::seconds(30));auto keys=W::keys(common);
    for(auto k:{"job","origin_source","origin_epoch","origin_history","op","status","native_input_tick_echo","helper_gpu_serial","helper_GPU_complete","output_valid","model_valid","evaluate_calls","evaluate_result","resolve_calls","model_reset","evaluate_parameters","gpu_times"})keys.insert(k);
    W::exact(e,keys);W::common_check(e,common);
    W::need(W::job_fields(e)==W::job_fields(x)&&W::s(e,"op")=="complete"&&W::s(e,"status")=="ORIGIN_NR_COMPLETE"&&W::u(e,"native_input_tick_echo")==current->input->native_target_tick&&W::u(e,"helper_gpu_serial")>last_helper_serial&&W::b(e,"helper_GPU_complete")&&W::u(e,"model_reset")==1&&e["gpu_times"].is_object(),"exact actual completion");
    const bool negative=mode=="skipEvaluate";W::need(W::b(e,"output_valid")==!negative&&W::b(e,"model_valid")==!negative&&W::u(e,"evaluate_calls")==unsigned(!negative)&&W::u(e,"evaluate_result")==unsigned(!negative)&&W::u(e,"resolve_calls")==unsigned(!negative),"mode-specific real model predicate");
    J params=negative?J::object():J{{"model_extent",{1280u,720u}},{"guide_extent",{1280u,720u}},{"color_format",109u},{"raw_format",97u},{"DepthInverted",0u},{"Reset",1u},{"Intensity",1.0},{"Style",0u},{"LocalStructure",1.0},{"LocalTone",1.0},{"SkinStructure",1.0},{"AutoMask",1u},{"MVecScale",{1.0,1.0}},{"jitter_setter","NONE"}};
    W::need(W::same_typed(e["evaluate_parameters"],W::parse(J{{"p",params}}.dump())["p"]),"actual fixed Evaluate arguments");last_helper_serial=W::u(e,"helper_gpu_serial");
    current->complete={current->id,current->input->native_target_tick,last_helper_serial,true,true,!negative,!negative,negative,W::u(e,"evaluate_calls"),W::u(e,"evaluate_result"),W::u(e,"resolve_calls"),1};
    std::printf("NATIVE_PRE_SR_PEER_COMPLETE_VERIFIED session=%s job=%llu origin=%llu native_input_tick=%llu helper_serial=%llu native_monotonic_ns=%llu scope=after_strict_complete_validation_before_phase_publish clock=native_steady helper_serial_domain=source_identifier_only\n",
     session.c_str(),(unsigned long long)current->id.job,(unsigned long long)current->id.origin_source,
     (unsigned long long)current->input->native_target_tick,(unsigned long long)last_helper_serial,
     (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
    phase.store(Phase::Complete,std::memory_order_release);
   }else if(state==Phase::FinalRecorded&&current&&current->final->native_GPU_complete.load(std::memory_order_acquire)){
    auto d=current->disposition;std::string disposition=d==PreSrDisposition::Captured?"captured":d==PreSrDisposition::ExpectedSkip?"expected_skip":"obsolete_discard";
    auto x=common;x.update(J{{"job",current->id.job},{"origin_source",current->id.origin_source},{"origin_epoch",current->id.origin_epoch},{"origin_history",current->id.origin_history},{"op","consumed"},{"status","ORIGIN_RESOURCES_RELEASED"},{"helper_gpu_serial_echo",current->complete.helper_gpu_serial},{"native_shared_last_use_tick",current->final->native_target_tick},{"native_shared_GPU_complete",true},{"disposition",disposition}});
    Atomic(W::job_path(run,"consumed",session,counts.finalized),x.dump()+"\n");counts.finalize(x,disposition);finalized.store(counts.finalized);
    current->consumed_published.store(true,std::memory_order_release);phase.store(Phase::Finalized,std::memory_order_release);
   }
   std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const char* reasons[]{"UNKNOWN","enable_off","exit","profile_change","budget"};
  const unsigned reason_index=stop_reason.load();
  W::need(reason_index>=1&&reason_index<=4,"exact managed stop reason");
  const std::string reason=reasons[reason_index];
  auto request_stop=terminal("control","REQUESTED",false);
  request_stop.update(J{{"control_epoch",1u},{"action","stop"},{"reason",reason}});
  request_stop=W::parse(request_stop.dump());
  W::terminal(request_stop,common,counts,"request_stage");
  Atomic(run+"/"+W::base("control-request",session),request_stop.dump()+"\n");
  auto stop_ack=wait(run+"/"+W::base("control-ack",session),std::chrono::seconds(60));
  W::terminal(stop_ack,common,counts,"ack_stage");
  W::need(W::s(stop_ack,"op")=="control_ack"&&W::s(stop_ack,"status")=="STOP_READY"&&
    W::u(stop_ack,"control_epoch")==1&&W::s(stop_ack,"action")=="stop"&&
    W::s(stop_ack,"reason")==reason&&W::b(stop_ack,"completed_GPU")&&
    !W::b(stop_ack,"output_valid")&&!W::b(stop_ack,"model_valid")&&
    W::u(stop_ack,"helper_gpu_serial")>=last_helper_serial,"actual STOP_READY");
  phase.store(Phase::AwaitNativeRelease,std::memory_order_release);
  const auto release_limit=Clock::now()+std::chrono::seconds(60);
  while(!quiescence->stopped_and_destroyed.load(std::memory_order_acquire)){
    check();W::need(Clock::now()<release_limit,"actual Native cleanup bounded HOLD");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const uint64_t target=quiescence->last_use_tick.load(std::memory_order_acquire);
  const uint64_t observed=quiescence->observed_tick.load(std::memory_order_acquire);
  W::need(target?observed>=target:observed==0&&!imported_native_used,
          "Native max last use independently of finalized N");
  imports->Destroy();registry.actual_import_fd_retired=false;
  auto release=terminal("release","NATIVE_RELEASED",target>0);
  release.update(J{{"control_epoch",1u},{"terminal_reason",reason},{"native_last_use_tick",target},
    {"native_last_use_observed_tick",observed},{"native_last_use_proof",target?
      "ACTUAL_NATIVE_GPU_LAST_USE":"NOT_REQUIRED_NO_NATIVE_USE"},
    {"native_imported_images_owned",0u},{"native_raw_duplicate_fd_owned",0u},
    {"native_origin_leases_owned",0u},{"native_cache_owners_owned",0u}});
  release=W::parse(release.dump());W::terminal(release,common,counts,"native_release_stage");W::native_last_use(release);
  Atomic(run+"/"+W::base("native-released",session),release.dump()+"\n");
  auto retired=wait(run+"/"+W::base("retired",session),std::chrono::seconds(60));
  W::terminal(retired,common,counts,"helper_retired_stage");
  W::need(W::s(retired,"op")=="retired"&&W::s(retired,"status")=="RETIRED"&&
    W::u(retired,"control_epoch")==1&&W::s(retired,"terminal_reason")==reason&&
    W::b(retired,"completed_GPU")&&!W::b(retired,"output_valid")&&!W::b(retired,"model_valid")&&
    W::u(retired,"helper_gpu_serial")>=W::u(stop_ack,"helper_gpu_serial")&&
    !W::u(retired,"shared_images_owned")&&!W::u(retired,"export_raw_fd_owned")&&
    W::b(retired,"normal_shared_retirement"),"post-call actual shared retirement");
  auto nf=Inventory(getpid(),run+"/async-native-fd-final.json");
  auto wf=Inventory(exporter,run+"/async-wine-fd-final.json");
  for(const auto&raw:raw_fds){for(const auto&fd:nf)W::need(!Payload(fd,raw),"native raw payload remains");
    for(const auto&fd:wf)W::need(!Payload(fd,raw),"Helper raw payload remains");}
  auto audit=terminal("audit","FD_AUDITED",target>0);
  audit.update(J{{"control_epoch",1u},{"terminal_reason",reason},{"native_last_use_tick",target},
    {"native_last_use_observed_tick",observed},{"native_last_use_proof",release["native_last_use_proof"]},
    {"tracked_NR_FD_remaining",0u},{"native_inventory","async-native-fd-final.json"},
    {"helper_inventory","async-wine-fd-final.json"}});
  audit=W::parse(audit.dump());W::terminal(audit,common,counts,"native_audited_stage");W::native_last_use(audit);
  Atomic(run+"/"+W::base("native-audited",session),audit.dump()+"\n");
  // After audit, normal Helper exit is permitted. Do not use the pre-terminal
  // liveness/reap function, which classifies any early exit as abnormal.
  const auto result_limit=Clock::now()+std::chrono::seconds(60);
  J result;
  while(Clock::now()<result_limit&&Clock::now()-start<std::chrono::seconds(300)&&!stop.stop_requested()){
    auto data=Read(run+"/async-wine-result.json");
    if(!data.empty()){result=W::parse(data);break;}
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  W::terminal(result,common,counts,"helper_result_stage");
  W::need(W::s(result,"op")=="result"&&W::s(result,"status")=="PASS"&&
    W::u(result,"control_epoch")==1&&W::s(result,"terminal_reason")==reason&&
    W::b(result,"completed_GPU")&&!W::b(result,"output_valid")&&!W::b(result,"model_valid")&&
    W::b(result,"normal_release")&&W::b(result,"normal_shutdown")&&
    W::u(result,"feature_create_calls")==1&&W::u(result,"feature_release_calls")==1&&
    W::u(result,"evaluate_calls_total")==(mode=="resident"?counts.finalized:0)&&
    W::u(result,"resolve_calls_total")==(mode=="resident"?counts.finalized:0)&&
    W::s(result,"NR_phase")=="UNVERIFIED"&&
    W::s(result,"native_present_count")=="REQUIRES_NATIVE_LEDGER_NOT_INFERRED",
    "actual Helper terminal result, not native Present proof");
  int status=0;pid_t reaped=0;
  const auto reap_limit=Clock::now()+std::chrono::seconds(60);
  while(Clock::now()<reap_limit&&Clock::now()-start<std::chrono::seconds(300)&&!stop.stop_requested()){
    reaped=waitpid(child,&status,WNOHANG);
    if(reaped==child)break;
    W::need(reaped==0||(reaped<0&&errno==EINTR),"normal waitpid");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  W::need(reaped==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"actual normal private peer reap0");
  Atomic(run+"/async-native-helper-exit.json",J{{"wait_status",status},{"normal_exit",true},
    {"exporter_pid",unsigned(exporter)},{"exporter_start_ticks",exporter_ticks},
    {"finalized_jobs",counts.finalized}}.dump()+"\n");
  actual_wait_status.store(status);peer_reaped.store(true,std::memory_order_release);normal_reap.store(true,std::memory_order_release);
  phase.store(Phase::Retired,std::memory_order_release);
 }catch(const std::exception&e){
  const bool no_native_recording=!imported_native_used&&quiescence->last_use_tick.load()==0&&unsigned(State())<unsigned(Phase::Reserved);
  if(no_native_recording){imports->Destroy();empty_imports_destroyed.store(true,std::memory_order_release);}
  try{Atomic(run+"/async-exporter-candidates.json",candidates.Report().dump()+"\n");}catch(...){}
  Fail(e.what()); // Functional FAIL survives any later safe host close.
  if(child>1&&!peer_reaped.load(std::memory_order_acquire)){
   const auto reap_limit=Clock::now()+std::chrono::seconds(60);
   while(!stop.stop_requested()&&Clock::now()<reap_limit&&Clock::now()-start<std::chrono::seconds(300)){
    int status=0;const auto p=waitpid(child,&status,WNOHANG);
    if(p==child){actual_wait_status.store(status);peer_reaped.store(true,std::memory_order_release);
     try{Atomic(run+"/async-native-helper-exit.json",J{{"wait_status",status},{"normal_exit",false}}.dump()+"\n");}catch(...){}
     break;
    }
    if(p<0&&errno!=EINTR)break; // ECHILD does NOT invent an actual reap proof.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
   }
  }
 }
}
bool NativePreSrOwner::SafeEmptyBootstrapExit(bool snapshots,bool pending,uint64_t copies)const noexcept {
 const auto q=Quiescence();
 return BbNativeAsync::IdentityPolicy::SafeAbnormalClose({
  .failed=State()==Phase::Failed,.worker_finished=worker_finished.load(std::memory_order_acquire),
  .spawned=peer_spawned.load(std::memory_order_acquire),.reaped=peer_reaped.load(std::memory_order_acquire),
  .import_objects_destroyed=empty_imports_destroyed.load(std::memory_order_acquire),
  .has_snapshot=snapshots,.has_pending_lease=pending,
  .imports_ever_created=import_objects_ever_created.load(std::memory_order_acquire),
  .source_copies_ever=copies,.native_last_use=q->last_use_tick.load(std::memory_order_acquire),
  .native_observed=q->observed_tick.load(std::memory_order_acquire)});
}
void NativePreSrOwner::JoinFinished(){
 if(worker_finished.load(std::memory_order_acquire)&&worker.joinable())worker.join();
}
}
