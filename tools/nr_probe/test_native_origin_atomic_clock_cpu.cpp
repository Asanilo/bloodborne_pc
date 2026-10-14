// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_vulkan/native_origin_atomic_clock.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <sys/stat.h>
using namespace Vulkan::NativeOriginAtomicClock;
int main(){
 int checks=0;const auto check=[&](bool b){if(!b)throw std::runtime_error("Atomic clock CPU check failed");++checks;};
 check(!Observation::Slow(0));check(!Observation::Slow(10000000));check(Observation::Slow(10000001));
 Observation m;
 errno=0;check(m.Measure(Observation::Write,[]{errno=EINTR;return -1;})==-1);check(errno==EINTR&&m.errors[Observation::Write]==EINTR);
 check(m.Measure(Observation::Write,[]{return 3;})==3);check(m.errors[Observation::Write]==EINTR);
 Observation failed;check(failed.Measure(Observation::Fsync,[]{errno=EIO;return -1;})==-1);check(errno==EIO&&failed.errors[Observation::Fsync]==EIO);
 failed.failure_errno=EIO;FILE* rejected=tmpfile();check(rejected!=nullptr);failed.Print(rejected,"control.json",10000001);rewind(rejected);char error_row[1024]{};check(fgets(error_row,sizeof(error_row),rejected)!=nullptr);check(std::string(error_row).find("success=0 errno=5")!=std::string::npos);fclose(rejected);
 FILE* f=tmpfile();check(f!=nullptr);m.Print(f,"/secret/full/path/input.json",10000000);check(ftell(f)==0);
 m.success=true;m.ns[Observation::Fsync]=12000000;m.Print(f,"/secret/full/path/input.json",13000000);rewind(f);
 std::string row;char buf[1024];while(fgets(buf,sizeof(buf),f))row+=buf;fclose(f);
 check(row.find("basename=input.json")!=std::string::npos&&row.find("/secret/")==std::string::npos);
 check(row.find("fsync_ns=12000000")!=std::string::npos&&row.find("success=1 errno=0")!=std::string::npos);
 check(row.find("GPU_proof=NOT_MEASURED")!=std::string::npos);
 char temp[]="/tmp/native-atomic-clock-XXXXXX";char* dir=mkdtemp(temp);check(dir!=nullptr);const std::string root=dir,path=root+"/control.json";
 const std::string payload="{\"strict\":true}\n";Publish(path,payload);
 std::ifstream in(path);std::string bytes((std::istreambuf_iterator<char>(in)),{});check(bytes==payload);
 struct stat st{};check(stat(path.c_str(),&st)==0&&(st.st_mode&0777)==0600);check(!std::filesystem::exists(path+".tmp-"+std::to_string(getpid())));
 const auto fail=[&](const std::string& target,const char* expected){try{Publish(target,payload);return false;}catch(const std::runtime_error& e){return std::string(e.what())==expected;}};
 check(fail(root+"/missing/control.json","async atomic open"));
 const auto duplicate=path+".tmp-"+std::to_string(getpid());{std::ofstream out(duplicate);out<<"keep";}
 check(fail(path,"async atomic open"));{std::ifstream old(duplicate);std::string x;old>>x;check(x=="keep");}unlink(duplicate.c_str());
 const auto directory=root+"/as-directory";std::filesystem::create_directory(directory);check(fail(directory,"async rename"));
 check(!std::filesystem::exists(directory+".tmp-"+std::to_string(getpid())));check(std::filesystem::is_directory(directory));
 std::filesystem::remove_all(root);
 std::cout<<"{\"status\":\"PASS_CPU_ONLY\",\"checks\":"<<checks<<",\"GPU\":\"NOT_RUN\",\"mock_slow_format\":\"NOT_RUNTIME_LATENCY_PROOF\"}\n";
}
