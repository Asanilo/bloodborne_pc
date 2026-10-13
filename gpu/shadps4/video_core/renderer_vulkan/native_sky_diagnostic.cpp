// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_sky_diagnostic.h"
#include "vk_instance.h"
#include "vk_scheduler.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
namespace Vulkan {
bool NativeSkyDiagnostic::Enabled()noexcept{
 const char*d=std::getenv("BB_NR_NATIVE_ASYNC_DIAGNOSTIC_ONCE");
 const char*m=std::getenv("BB_NR_NATIVE_ASYNC_MODE");
 return d&&m&&!std::strcmp(d,"1")&&!std::strcmp(m,"localnoop");
}
bool NativeSkyDiagnostic::EditEnabled()noexcept {
 const char*d=std::getenv("BB_NR_NATIVE_ASYNC_EDIT_DIAGNOSTIC_ONCE");
 const char*m=std::getenv("BB_NR_NATIVE_ASYNC_MODE");
 const char*p=std::getenv("BB_NR_NATIVE_ORIGIN_MODE");
 return d&&m&&p&&!std::strcmp(d,"1")&&!std::strcmp(m,"resident")&&!std::strcmp(p,"resident");
}
NativeSkyDiagnostic::NativeSkyDiagnostic(const Instance&i,bool edit):edit(edit){
 const char*r=std::getenv("BB_NR_NATIVE_ASYNC_RUN_DIR"),*s=std::getenv("BB_NR_NATIVE_ASYNC_SESSION");
 if(!r||!s)throw std::invalid_argument("Native cold diagnostic private directory");
 run=r;session=s;
 if(run!="/home/arinp/Code/nrbridge-design/game-poc-20261011/bbport/runs/native-async-game-"+session)
  throw std::invalid_argument("Native cold diagnostic private root");
 for(auto&b:buffers)b=std::make_unique<VideoCore::Buffer>(i,0,33177600,
  VideoCore::MemoryType::HostCached,"Native Sky ONCE local-noop source/composite audit");
 writer=std::jthread([this](std::stop_token s){Run(s);});
}
NativeSkyDiagnostic::NativeSkyDiagnostic(const Instance&i,PreSrTag):pre_sr(true){
 const char*r=std::getenv("BB_NR_NATIVE_PRE_SR_RUN_DIR"),*name=std::getenv("BB_NR_NATIVE_PRE_SR_SESSION");
 if(!r||!name||!BbPreSr::DiagnosticEnabled())throw std::invalid_argument("preSR diagnostic resident opt-in/private directory");
 run=r;session=name;
 if(run!="/home/arinp/Code/nrbridge-design/game-poc-20261011/bbport/runs/native-pre-sr-game-"+session)
  throw std::invalid_argument("preSR diagnostic private root");
 for(auto&b:buffers)b=std::make_unique<VideoCore::Buffer>(i,0,BbPreSr::DiagnosticImageBytes,
  VideoCore::MemoryType::HostCached,"PreSR ONCE same-source original/actual SR NR half720");
 writer=std::jthread([this](std::stop_token token){Run(token);});
}
NativeSkyDiagnostic::~NativeSkyDiagnostic(){writer.request_stop();if(writer.joinable())writer.join();}
bool NativeSkyDiagnostic::RecordEditOnce(Scheduler&s,vk::Image raw,vk::Image selected,
 const BbNativeAsync::Frame&f,const EditContext&ctx) {
 if(!edit||recorded.load()||!raw||!selected||raw==selected||!ctx.identity.Eligible()||
    f.source!=ctx.identity.current||f.epoch!=ctx.identity.current_epoch||
    f.history!=ctx.identity.current_history||f.params!=ctx.identity.current_params||
    !ctx.native_input_tick||!ctx.helper_serial||ctx.resource_set.empty())return false;
 edit_context=ctx;
 RecordOnce(s,raw,selected,f);
 std::printf("NATIVE_SKY_EDIT_DIAGNOSTIC_RECORDED session=%s origin=%llu current_source=%llu job=%llu age=%llu target_tick=%llu selected_output=%s source_edited=0 performance_excluded=1 pixels=NOT_YET_AUDITED\n",
  session.c_str(),(unsigned long long)ctx.identity.origin,(unsigned long long)f.source,
  (unsigned long long)ctx.identity.job,(unsigned long long)(f.source-ctx.identity.origin),
  (unsigned long long)target_tick,ctx.replay?"replay-current-packet":"regular-current-packet");
 return true;
}
void NativeSkyDiagnostic::EditMergeRecorded(Scheduler&s,uint64_t source) {
 if(!edit||!recorded.load()||merge_registered||source!=frame.source)return;
 merge_tick=s.CurrentTick();merge_registered=true;
 // Separate actual merge callback: no assumption that a premerge copy target is
 // a completed merge. Both callbacks retain staging until their own GPU proof.
 s.DeferPriorityOperation([hold=shared_from_this()]{hold->merge_ready.store(true,std::memory_order_release);});
 std::printf("NATIVE_SKY_EDIT_DIAGNOSTIC_MERGE session=%s current_source=%llu job=%llu copy_tick=%llu merge_tick=%llu Present_proof=REQUIRES_UNIQUE_LEDGER\n",
  session.c_str(),(unsigned long long)source,(unsigned long long)edit_context.identity.job,
  (unsigned long long)target_tick,(unsigned long long)merge_tick);
}
void NativeSkyDiagnostic::RecordOnce(Scheduler&s,vk::Image raw,vk::Image composite,const BbNativeAsync::Frame&f){
 if(recorded.exchange(true))return;
 frame=f;target_tick=s.CurrentTick();
 auto cmd=s.CommandBuffer();
 const std::array images{raw,composite};
 for(unsigned k=0;k<2;k++){
  const vk::ImageMemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eComputeShader,
   .srcAccessMask=vk::AccessFlagBits2::eShaderWrite,.dstStageMask=vk::PipelineStageFlagBits2::eTransfer,
   .dstAccessMask=vk::AccessFlagBits2::eTransferRead,.oldLayout=vk::ImageLayout::eGeneral,
   .newLayout=vk::ImageLayout::eGeneral,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
   .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=images[k],
   .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
  cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b});
  const vk::BufferImageCopy c{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
   .imageExtent={1920,1080,1}};
  cmd.copyImageToBuffer(images[k],vk::ImageLayout::eGeneral,buffers[k]->Handle(),c);
  const vk::BufferMemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,
   .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,
   .dstAccessMask=vk::AccessFlagBits2::eHostRead,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
   .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=buffers[k]->Handle(),.offset=0,.size=33177600};
  auto restore=b;restore.srcStageMask=vk::PipelineStageFlagBits2::eTransfer;
  restore.srcAccessMask=vk::AccessFlagBits2::eTransferRead;
  restore.dstStageMask=vk::PipelineStageFlagBits2::eComputeShader;
  restore.dstAccessMask=vk::AccessFlagBits2::eShaderRead;
  cmd.pipelineBarrier2({.bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&host,
    .imageMemoryBarrierCount=1,.pImageMemoryBarriers=&restore});
 }
 // Registration uses the existing open target. Subsequent merge is recorded to
 // that target; callback means actual completion, not command-recording ready.
 s.DeferPriorityOperation([hold=shared_from_this()]{hold->gpu_ready.store(true,std::memory_order_release);});
 std::printf("NATIVE_SKY_DIAGNOSTIC_RECORDED session=%s source=%llu epoch=%llu target_tick=%llu readback_bytes=66355200 once=1 extra_flush=0\n",
  session.c_str(),(unsigned long long)frame.source,(unsigned long long)frame.epoch,(unsigned long long)target_tick);
}
void NativeSkyDiagnostic::Run(std::stop_token stop){
 struct End{std::atomic<bool>&flag;~End(){flag.store(true,std::memory_order_release);}} end{writer_finished};
 if(pre_sr){RunPreSr(stop);return;}
 while(!stop.stop_requested()&&(!gpu_ready.load(std::memory_order_acquire)||(edit&&!merge_ready.load(std::memory_order_acquire))))
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
 if(!gpu_ready.load(std::memory_order_acquire)||(edit&&!merge_ready.load(std::memory_order_acquire)))return;
 try{
  const std::string dir=run+(edit?"/native-sky-edit-once":"/native-sky-once");std::filesystem::create_directory(dir);
  const char*names[]{"source.rgba32f","composite.rgba32f"};
  for(unsigned k=0;k<2;k++){
   buffers[k]->Invalidate(0,33177600);
   std::ofstream f(dir+"/"+names[k],std::ios::binary|std::ios::trunc);
   f.write(reinterpret_cast<const char*>(buffers[k]->mapped_data.data()),33177600);
   if(!f)throw std::runtime_error("cold native audit local write failed");
  }
  nlohmann::json j{{"session",session},{"source_game_frame",frame.source},{"origin_source",frame.source},
   {"origin_epoch",frame.epoch},{"origin_history",frame.history},{"params_version",frame.params},
   {"native_target_tick",target_tick},{"actual_native_GPU_complete",true},{"same_frame_merge_tick",target_tick},
   {"format",109u},{"width",1920u},{"height",1080u},{"bytes_per_image",33177600u},
   {"source_edited",false},{"mode","localnoop"},{"model_calls",0u},{"phase","UNVERIFIED"},
   {"Present_proof","REQUIRES_UNIQUE_NATIVE_PRESENT_LEDGER"}};
  if(edit){
   const auto& c=edit_context;const auto& id=c.identity;
   j.update(nlohmann::json{{"mode","resident"},{"origin_source",id.origin},{"current_source",id.current},
    {"origin_epoch",id.origin_epoch},{"current_epoch",id.current_epoch},
    {"origin_history",id.origin_history},{"current_history",id.current_history},
    {"params_version",id.current_params},{"job",id.job},{"resource_set",c.resource_set},
    {"age",id.current-id.origin},{"native_input_tick",c.native_input_tick},
    {"helper_gpu_serial_source_only",c.helper_serial},{"same_frame_merge_tick",merge_tick},
    {"actual_native_merge_GPU_complete",true},{"selected_view_image_match",true},
    {"selected_output_packet",c.replay?"replay-current":"regular-current"},
    {"source_packet_is_current",true},{"readback_bytes",66355200u},{"excluded_from_performance",true},
    {"scope","actual_selected_edit_vs_same_current_unedited_SR"},
    {"pixels_and_quality","NOT_AUDITED"},{"model_calls","NOT_MEASURED_BY_NATIVE_DIAGNOSTIC"},
    {"present_join",{{"event","NATIVE_ASYNC_CURRENT_PRESENT"},{"session",session},
     {"source_game_frame",frame.source},{"merge_tick",merge_tick},{"sparse_job",id.job},
     {"required_effect_applied",true},{"required_nonreuse",true}}}});
  }
  std::ofstream meta(dir+"/readback.json.tmp");meta<<j.dump(2)<<"\n";meta.close();
  if(!meta)throw std::runtime_error("cold native metadata write failed");
  std::filesystem::rename(dir+"/readback.json.tmp",dir+"/readback.json");
  std::printf("NATIVE_SKY_DIAGNOSTIC_COMPLETE source=%llu actual_native_tick=%llu files_bytes=66355200 Present_proof=REQUIRES_LEDGER\n",
   (unsigned long long)frame.source,(unsigned long long)target_tick);
 }catch(const std::exception&e){std::fprintf(stderr,"NATIVE_SKY_DIAGNOSTIC_FAILED reason=%s\n",e.what());}
}
bool NativeSkyDiagnostic::RecordPreSrOnce(Scheduler&s,vk::Image raw,vk::Image selected,
 const std::array<vk::ImageCreateInfo,2>&ci,const BbNativeAsync::Frame&f,const PreSrContext&ctx){
 if(!pre_sr||recorded.load()||!raw||!selected||raw==selected||!ctx.identity.Eligible()||
  f.source!=ctx.identity.current.source||f.epoch!=ctx.identity.current.epoch||
  f.history!=ctx.identity.current.history||f.params!=ctx.identity.current.params||
  f.source_edited||!ctx.helper_serial||ctx.resource_set.empty()||s.CurrentTick()!=ctx.identity.sr_read_tick)return false;
 for(const auto&c:ci)if(c.format!=vk::Format::eR16G16B16A16Sfloat||c.extent.width!=1280||c.extent.height!=720||
  c.extent.depth!=1||c.mipLevels!=1||c.arrayLayers!=1||c.samples!=vk::SampleCountFlagBits::e1||
  !(c.usage&vk::ImageUsageFlagBits::eTransferSrc))return false;
 pre_sr_context=ctx;frame=f;target_tick=s.CurrentTick();recorded.store(true);
 const auto cmd=s.CommandBuffer();const std::array images{raw,selected};
 for(unsigned k=0;k<2;k++){
  const vk::ImageMemoryBarrier2 before{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
   .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
   .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,.dstAccessMask=vk::AccessFlagBits2::eTransferRead,
   .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
   .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
   .image=images[k],.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
  cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&before});
  const vk::BufferImageCopy copy{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={1280,720,1}};
  cmd.copyImageToBuffer(images[k],vk::ImageLayout::eGeneral,buffers[k]->Handle(),copy);
  const vk::BufferMemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,
   .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,
   .dstAccessMask=vk::AccessFlagBits2::eHostRead,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
   .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=buffers[k]->Handle(),.offset=0,.size=BbPreSr::DiagnosticImageBytes};
  auto restore=before;restore.srcStageMask=vk::PipelineStageFlagBits2::eTransfer;
  restore.srcAccessMask=vk::AccessFlagBits2::eTransferRead;restore.dstStageMask=vk::PipelineStageFlagBits2::eAllCommands;
  restore.dstAccessMask=vk::AccessFlagBits2::eShaderRead;
  cmd.pipelineBarrier2({.bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&host,
   .imageMemoryBarrierCount=1,.pImageMemoryBarriers=&restore});
 }
 // Recorded AFTER real DLSS SR commands in this Native queue; this target covers
 // the SR read and copies. Source images also have a bridge last-use lease.
 s.DeferPriorityOperation([hold=shared_from_this()]{hold->gpu_ready.store(true,std::memory_order_release);});
 std::printf("NATIVE_PRE_SR_DIAGNOSTIC_RECORDED session=%s source=%llu job=%llu input_tick=%llu return_tick=%llu SR_read_tick=%llu copy_tick=%llu readback_bytes=14745600 same_source=1 extra_flush=0 performance_excluded=1 pixels=NOT_AUDITED\n",
  session.c_str(),(unsigned long long)f.source,(unsigned long long)ctx.identity.job,
  (unsigned long long)ctx.identity.input_tick,(unsigned long long)ctx.identity.return_tick,
  (unsigned long long)ctx.identity.sr_read_tick,(unsigned long long)target_tick);
 return true;
}
void NativeSkyDiagnostic::PreSrMergeRecorded(Scheduler&s,uint64_t source){
 if(!pre_sr||!recorded.load()||merge_registered||source!=frame.source)return;
 if(s.CurrentTick()<target_tick)throw std::runtime_error("preSR diagnostic Native merge order mismatch");
 merge_tick=s.CurrentTick();merge_registered=true;
 s.DeferPriorityOperation([hold=shared_from_this()]{hold->merge_ready.store(true,std::memory_order_release);});
 std::printf("NATIVE_PRE_SR_DIAGNOSTIC_MERGE session=%s source=%llu job=%llu copy_tick=%llu merge_tick=%llu actualPresent=REQUIRES_UNIQUE_NONREUSE_LEDGER\n",
  session.c_str(),(unsigned long long)source,(unsigned long long)pre_sr_context.identity.job,
  (unsigned long long)target_tick,(unsigned long long)merge_tick);
}
bool NativeSkyDiagnostic::RetirePreSrFinished(){
 if(!pre_sr||!recorded.load()||!gpu_ready.load(std::memory_order_acquire)||
  !merge_ready.load(std::memory_order_acquire)||!Finished())return false;
 JoinFinished();
 if(!pre_sr_retired){
  for(auto&buffer:buffers)buffer.reset(); // actual GPU last use and CPU writer both ended
  pre_sr_retired=true;
  std::printf("NATIVE_PRE_SR_DIAGNOSTIC_RETIRED session=%s source=%llu copy_SR_merge_GPU_complete=1 writer_joined=1 staging_buffers=0\n",session.c_str(),(unsigned long long)frame.source);
 }
 return true;
}
void NativeSkyDiagnostic::RunPreSr(std::stop_token stop){
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(300);
 while(!stop.stop_requested()&&std::chrono::steady_clock::now()<deadline&&
  (!gpu_ready.load(std::memory_order_acquire)||!merge_ready.load(std::memory_order_acquire)))
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
 if(!gpu_ready.load(std::memory_order_acquire)||!merge_ready.load(std::memory_order_acquire))return;
 try{
  const std::string dir=run+"/native-pre-sr-once";
  if(!std::filesystem::create_directory(dir))throw std::runtime_error("preSR once output directory already exists");
  const char*names[]{"original-hdr720.rgba16f","nr-sr-input720.rgba16f"};
  for(unsigned k=0;k<2;k++){
   buffers[k]->Invalidate(0,BbPreSr::DiagnosticImageBytes);
   std::ofstream out(dir+"/"+names[k],std::ios::binary|std::ios::trunc);
   out.write(reinterpret_cast<const char*>(buffers[k]->mapped_data.data()),BbPreSr::DiagnosticImageBytes);
   out.close();if(!out)throw std::runtime_error("preSR once bounded pixel write failed");
  }
  const auto&c=pre_sr_context;const auto&id=c.identity;
  nlohmann::json j{{"schema","native-pre-sr-visual-once-v1"},{"session",session},{"mode","resident"},
   {"source_game_frame",frame.source},{"origin_source",id.result.source},{"job",id.job},
   {"origin_epoch",id.result.epoch},{"origin_history",id.result.history},{"params_version",id.result.params},
   {"native_input_tick",id.input_tick},{"native_return_tick",id.return_tick},{"native_SR_read_tick",id.sr_read_tick},
   {"native_copy_tick",target_tick},{"native_merge_tick",merge_tick},{"native_timeline_scope","same_renderer_Scheduler_only"},
   {"helper_serial_source_only",c.helper_serial},{"resource_set",c.resource_set},
   {"actual_native_copy_and_SR_read_GPU_complete",true},{"actual_native_merge_GPU_complete",true},
   {"original_input_immutable",true},{"selected_image_is_actual_SR_input",true},{"age",0u},
   {"format",97u},{"width",1280u},{"height",720u},{"bytes_per_image",BbPreSr::DiagnosticImageBytes},
   {"readback_bytes",2*BbPreSr::DiagnosticImageBytes},{"source_file",names[0]},{"actual_NR_SR_input_file",names[1]},
   {"source_sr_reset_requested",frame.sr_reset},{"applied_sr_reset",c.applied_sr_reset},
   {"source_jitter_metadata",{{"viewport_jitter_x",frame.viewport[0]},{"viewport_jitter_y",frame.viewport[1]},
    {"previous_viewport_jitter_x",frame.previous_viewport[0]},{"previous_viewport_jitter_y",frame.previous_viewport[1]},
    {"sr_jitter_x",frame.sr_jitter[0]},{"sr_jitter_y",frame.sr_jitter[1]},
    {"previous_sr_jitter_x",frame.previous_sr_jitter[0]},{"previous_sr_jitter_y",frame.previous_sr_jitter[1]}}},
   {"excluded_from_performance",true},{"pixels_and_quality","NOT_AUDITED"},{"phase","UNVERIFIED"},
   {"same_source_SR_only1080_AB","NOT_AVAILABLE"},
   {"present_proof","REQUIRES_EXISTING_UNIQUE_ACTUAL_LEDGER_AND_FRAME_DUMP"},
   {"present_join",{{"event","NATIVE_PRE_SR_CURRENT_PRESENT"},{"session",session},{"source_game_frame",frame.source},
    {"job",id.job},{"merge_tick",merge_tick},{"required_selected_NR720",true},{"required_swapchain_present",true},
    {"required_nonreuse",true},{"scope","CPU_swapchain_Present_return_not_photon"}}},
   {"final1080_frame_dump_join",{{"stage","PRE_SR"},{"session",session},{"source_game_frame",frame.source},
    {"merge_tick",merge_tick},{"required_selected_output",true},{"required_file_ok",true},
    {"scheduled_event","PRE_SR_PRESENT_DUMP_SCHEDULED"},{"completed_event","PRE_SR_PRESENT_DUMP_COMPLETE"}}}};
  std::ofstream out(dir+"/readback.json.tmp");out<<j.dump(2)<<"\n";out.close();
  if(!out)throw std::runtime_error("preSR once metadata write failed");
  std::filesystem::rename(dir+"/readback.json.tmp",dir+"/readback.json");
  std::printf("NATIVE_PRE_SR_DIAGNOSTIC_COMPLETE session=%s source=%llu job=%llu readback_bytes=14745600 copy_and_SR_GPU_complete=1 merge_GPU_complete=1 pixels=NOT_AUDITED actualPresent=REQUIRES_EXISTING_LEDGER\n",
   session.c_str(),(unsigned long long)frame.source,(unsigned long long)id.job);
 }catch(const std::exception&e){std::fprintf(stderr,"NATIVE_PRE_SR_DIAGNOSTIC_FAILED reason=%s\n",e.what());}
}

}
