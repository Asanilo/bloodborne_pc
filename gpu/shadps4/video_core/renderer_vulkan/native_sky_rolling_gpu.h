// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_async_rolling_policy.h"
#include "native_sky_constants.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/texture_cache/image.h"
#include <memory>
namespace Vulkan {
class Instance; class Scheduler;
// Renderer-thread-only recording. No queue submit, wait, file access, readback or model.
// Noop anchor is current unedited SR itself, NOT an NR inference or quality claim.
class NativeSkyRollingGpu {
public:
 explicit NativeSkyRollingGpu(const Instance&);
 void Convert(Scheduler&,unsigned physical_slot,const std::array<vk::Image,3>&);
 vk::ImageView Advance(Scheduler&,unsigned physical_slot);
 vk::ImageView CaptureAndCatchup(Scheduler&,const BbNativeAsync::Rolling::Span&,vk::ImageView actual_NR);
 std::array<vk::Image,3> Converted(unsigned i)const noexcept{return {vk::Image(packets[i].converted[0].image),vk::Image(packets[i].converted[1].image),vk::Image(packets[i].converted[2].image)};}
 vk::Image SelectedOutput(unsigned i,vk::ImageView selected,bool& replay)const noexcept{
  if(i>=packets.size()||!selected)return {};if(selected==packets[i].output.View()){replay=false;return vk::Image(packets[i].output.image);}if(selected==replay_packets[i].output.View()){replay=true;return vk::Image(replay_packets[i].output.image);}return {};
 }
 void Invalidate()noexcept{history_valid=false;}
 void CompleteSparseNativeUse()noexcept{replay_busy.store(false,std::memory_order_release);}
 bool SparseScratchBusy()const noexcept{return replay_busy.load(std::memory_order_acquire);}
 uint64_t PacketLastUse(unsigned i)const noexcept{return std::max(packet_use[i],replay_use[i]);}
 uint64_t LastUse()const noexcept{return last_use;}
 void MergeUse(unsigned slot,uint64_t tick)noexcept{packet_use[slot]=std::max(packet_use[slot],tick);last_use=std::max(last_use,tick);}
 uint64_t AllocatedImageBytes()const noexcept;
 uint64_t ImageBytes()const noexcept;
private:
 struct Image {VideoCore::UniqueImage image;vk::UniqueImageView view;vk::ImageView borrowed{};
 vk::ImageView View()const noexcept{return view?*view:borrowed;} };
 struct Bank {Image edit,guide,l1guide;std::array<Image,3> levels;};
 struct Packet {
  std::array<Image,3> converted;Image output;
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
 std::array<Packet,BbNativeAsync::Rolling::Capacity> packets,replay_packets;
 std::atomic<bool> replay_busy{false};
 Image dummy,exposure,stats;
 vk::UniqueSampler linear,nearest;
 vk::UniqueDescriptorSetLayout layout,conversion_layout;
 vk::UniquePipelineLayout pipeline_layout,conversion_pipeline_layout;
 vk::UniquePipeline pipeline,conversion_pipeline;
 vk::UniqueDescriptorPool pool;
 void TouchBanks(uint64_t);
 std::array<uint64_t,BbNativeAsync::Rolling::Capacity>packet_use{},replay_use{};
 std::array<uint64_t,2>bank_use{};uint64_t last_use=0;
 bool initialized=false,history_valid=false;unsigned active_bank=0;
};
}
