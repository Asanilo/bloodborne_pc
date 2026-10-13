// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_vulkan/native_pre_sr_spawn_log.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sys/wait.h>
extern char** environ;
namespace S=Vulkan::NativePreSrSpawnLog;
std::string Read(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
int Fds() {
    int n=0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry; ++n;
    }
    return n;
}
int main(int argc,char** argv_in) {
    if(argc==3&&std::string(argv_in[1])=="--simulate-pe-main"){
        // CPU-only simulation: startup streams then the actual PE-style wb reopen.
        std::printf("startup stdout warning\n");std::fflush(stdout);
        std::fprintf(stderr,"startup stderr warning\n");std::fflush(stderr);
        const std::filesystem::path root(argv_in[2]);
        if(!std::freopen((root/"async-helper.log").c_str(),"wb",stdout))return 41;
        if(!std::freopen((root/"async-helper-stderr.log").c_str(),"wb",stderr))return 42;
        std::printf("PE main stdout\n");std::fprintf(stderr,"PE main stderr\n");
        std::fflush(stdout);std::fflush(stderr);return 0;
    }
    unsigned checks=0;
    const auto check=[&](bool ok) { ++checks; if(!ok) std::abort(); };
    char dir[]="/tmp/pre-sr-spawn-log-XXXXXX";
    check(::mkdtemp(dir)!=nullptr); const std::filesystem::path run(dir);
    struct Cleanup {std::filesystem::path p; ~Cleanup(){std::filesystem::remove_all(p);}} cleanup{run};
    const int baseline=Fds();
    struct stat before1{},before2{},after1{},after2{};
    check(::fstat(1,&before1)==0&&::fstat(2,&before2)==0);
    char shell[]="/bin/sh", opt[]="-c";
    char script[]="printf 'cpu_monotonic_ns=40820278490024\\n'; printf 'warn:vulkan:init_physical_device\\n' >&2";
    char* argv[]={shell,opt,script,nullptr};pid_t child=-1;
    check(S::Spawn(&child,shell,argv,environ,(run/"async-helper-launch.log").string())==0);
    check(child>0); int status=0;check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    check(Read(run/"async-helper-launch.log")=="cpu_monotonic_ns=40820278490024\nwarn:vulkan:init_physical_device\n");
    struct stat st{};check(::stat((run/"async-helper-launch.log").c_str(),&st)==0&&S_ISREG(st.st_mode)&&(st.st_mode&0777)==0600&&st.st_nlink==1);
    check(Fds()==baseline);
    check(::fstat(1,&after1)==0&&::fstat(2,&after2)==0&&before1.st_dev==after1.st_dev&&before1.st_ino==after1.st_ino&&before2.st_dev==after2.st_dev&&before2.st_ino==after2.st_ino);
    // Existing file and symlink must fail closed without rewriting evidence.
    bool rejected=false;try { S::Spawn(&child,shell,argv,environ,(run/"async-helper-launch.log").string()); } catch(const std::system_error& e) { rejected=e.code().value()==EEXIST; }
    check(rejected);check(Read(run/"async-helper-launch.log").size()==65);check(Fds()==baseline);
    std::ofstream(run/"target")<<"untouched";std::filesystem::create_symlink(run/"target",run/"link");
    rejected=false;try { S::Spawn(&child,shell,argv,environ,(run/"link").string()); } catch(const std::system_error&) { rejected=true; }
    check(rejected);check(Read(run/"target")=="untouched");check(Fds()==baseline);
    // Failed spawn API retains empty log and cleans parent FD/actions.
    check(S::Spawn(&child,"/no-such-pre-sr-cpu-program",argv,environ,(run/"failed.log").string())==ENOENT);
    check(Read(run/"failed.log").empty());check(Fds()==baseline);
    rejected=false;try { S::Spawn(&child,shell,argv,environ,(run/"missing"/"log").string()); } catch(const std::system_error& e) { rejected=e.code().value()==ENOENT; }
    check(rejected);check(Fds()==baseline);
    // Restrictive inherited umask still results in exact0600, not permissive mode.
    const mode_t mask=::umask(0777);
    const int rc=S::Spawn(&child,shell,argv,environ,(run/"masked.log").string());::umask(mask);
    check(rc==0);check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    check(::stat((run/"masked.log").c_str(),&st)==0&&(st.st_mode&0777)==0600);check(Fds()==baseline);
    // Exercise source-fd collision with closed stdin; restore parent stdin.
    const int input=::open("/dev/null",O_RDONLY|O_CLOEXEC);check(input>=0);
    const int saved=::fcntl(0,F_DUPFD_CLOEXEC,3);check(saved>=0);check(::close(0)==0);
    const int low_rc=S::Spawn(&child,shell,argv,environ,(run/"low.log").string());
    check(::dup2(saved,0)==0);::close(saved);::close(input);
    check(low_rc==0);check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    check(Read(run/"low.log")==Read(run/"async-helper-launch.log"));check(Fds()==baseline);
    const auto reopened=run/"pe-reopen";check(std::filesystem::create_directory(reopened));
    char self[]="/proc/self/exe", simulation[]="--simulate-pe-main";
    std::string root=reopened.string();char* pe_argv[]={self,simulation,root.data(),nullptr};
    check(S::Spawn(&child,self,pe_argv,environ,(reopened/"async-helper-launch.log").string())==0);
    check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    check(Read(reopened/"async-helper-launch.log")=="startup stdout warning\nstartup stderr warning\n");
    check(Read(reopened/"async-helper.log")=="PE main stdout\n");
    check(Read(reopened/"async-helper-stderr.log")=="PE main stderr\n");
    check(Fds()==baseline);
    check(::fstat(1,&after1)==0&&::fstat(2,&after2)==0&&before1.st_dev==after1.st_dev&&before1.st_ino==after1.st_ino&&before2.st_dev==after2.st_dev&&before2.st_ino==after2.st_ino);
    std::printf("{\"status\":\"PASS\",\"checks\":%u,\"scope\":\"actual_CPU_posix_spawn_log_fd_actions_cleanup_no_Wine_no_GPU\",\"GPU\":\"NOT_RUN\"}\n",checks);
}
