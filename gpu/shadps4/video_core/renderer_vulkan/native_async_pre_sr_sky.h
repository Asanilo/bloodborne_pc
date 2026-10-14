// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_async_state.h"
#include "native_sky_constants.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/texture_cache/image.h"
#include <memory>
#include <span>
namespace Vulkan {
class Instance; class Scheduler;
// Renderer-thread-only recording. No queue submit, wait, file access, readback or model.
// Noop anchor is current unedited SR itself, NOT an NR inference or quality claim.
class NativeAsyncPreSrSky {
public:
 explicit NativeAsyncPreSrSky(const Instance&);
 vk::ImageView Record(Scheduler&,unsigned index,const std::array<vk::Image,3>&,
                      bool capture,const BbNativeAsync::Frame&);
 // Acquisition-only entry: record immutable raw -> converted textures WITHOUT
 // Capture/Reproject/Apply or writing active edit banks. Caller must retain the
 // physical packet/source views/descriptors until ALL true previous uses ended.
 // Recorded conversion is NOT GPU-ready, peer INPUT permission, or an edit grant.
 void RecordSourceConversionOnly(Scheduler&,unsigned,const std::array<vk::Image,3>&,
                                 const BbNativeAsync::Frame&);
 // Edit recording consumes precisely the already recorded packet metadata. This
 // API split alone does NOT separate active edit origin from pending acquisition.
 vk::ImageView RecordEditHistory(Scheduler&,unsigned,bool capture);
 // C-only physical packet API. Caller proves slot's complete prior actual use
 // and peer retirement before conversion; this does NOT grant INPUT publication.
 void RecordAcquisitionConversion(Scheduler&,unsigned physical,const std::array<vk::Image,3>&,const BbNativeAsync::Frame&);
 vk::ImageView RecordAcquisitionAdvance(Scheduler&,unsigned physical);
 vk::ImageView RecordAcquisitionCatchup(Scheduler&,std::span<const unsigned> suffix,vk::ImageView NR);
 std::array<vk::Image,3> ConvertedOrigin()const noexcept;
 std::array<vk::Image,3> Converted(unsigned i)const noexcept{return {vk::Image(packets[i].converted[0].image),vk::Image(packets[i].converted[1].image),vk::Image(packets[i].converted[2].image)};}
 vk::ImageView RawColorView(unsigned i)const noexcept{return *packets[i].source_views[0];}
 vk::Image Output(unsigned i)const noexcept{return vk::Image(packets[i].output.image);}
 // Resolve only an actual current packet's selected view. Replay and regular
 // outputs are different images; a flag or the origin packet is not sufficient.
 vk::Image SelectedOutput(unsigned i,vk::ImageView selected,bool& replay)const noexcept {
  if(i>=packets.size()||!selected)return {};
  if(selected==packets[i].output.View()){replay=false;return vk::Image(packets[i].output.image);}
  if(selected==replay_packets[i].output.View()){replay=true;return vk::Image(replay_packets[i].output.image);}
  return {};
 }
 vk::ImageView RecordSparseCatchup(Scheduler&,unsigned current_index,vk::ImageView actual_origin_NR);
 void CompleteSparseNativeUse()noexcept {replay_busy.store(false,std::memory_order_release);}
 bool SparseScratchBusy()const noexcept {return replay_busy.load(std::memory_order_acquire);}
 // Existing caller's AllUsesReady gate remains mandatory. Neither a counter
 // reset nor conversion recording proves last-use completion or peer retirement.
 void Restart() noexcept {next_index=0;next_conversion_index=0;}
 uint64_t ImageBytes()const noexcept;
private:
 struct Image {VideoCore::UniqueImage image;vk::UniqueImageView view;vk::ImageView borrowed{};
 vk::ImageView View()const noexcept{return view?*view:borrowed;} };
 struct Bank {Image edit,guide,l1guide;std::array<Image,3> levels;};
 struct Packet {
  std::array<Image,3> converted;Image output;BbNativeAsync::Frame frame;
  std::array<vk::UniqueImageView,3> source_views;
  std::array<std::unique_ptr<VideoCore::Buffer>,4> constants;
  std::array<vk::DescriptorSet,4> sets{};vk::DescriptorSet conversion{};
 };
 Image Make(vk::Format,unsigned,unsigned);
 void InitImages(vk::CommandBuffer);
 void Barrier(vk::CommandBuffer);
 void Pass(vk::CommandBuffer,Packet&,unsigned,const DlssNrCacheConstants&,bool,
           const std::array<Image*,12>&,const std::array<Image*,7>&);
 DlssNrCacheConstants Base(unsigned)const;
 vk::Device device;VmaAllocator allocator;
 std::array<Bank,2> banks;
 std::array<Packet,BbNativeAsync::NodeCount> packets,replay_packets;
 std::atomic<bool> replay_busy{false};
 Image dummy,exposure,stats;
 vk::UniqueSampler linear,nearest;
 vk::UniqueDescriptorSetLayout layout,conversion_layout;
 vk::UniquePipelineLayout pipeline_layout,conversion_pipeline_layout;
 vk::UniquePipeline pipeline,conversion_pipeline;
 vk::UniqueDescriptorPool pool;
 bool initialized=false;unsigned next_index=0,next_conversion_index=0;
 std::array<uint64_t,BbNativeAsync::NodeCount> acquisition_sources{};
 bool acquisition_history=false;unsigned acquisition_bank=0;BbNativeAsync::Frame acquisition_previous{};
 void AcquisitionStep(vk::CommandBuffer,Packet&,Packet&,unsigned prev,unsigned next,bool capture,vk::ImageView NR,bool apply);
};
}
