// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "native_pre_sr_policy.h"
#include "video_core/texture_cache/image.h"
#include "video_core/buffer_cache/buffer.h"
namespace Vulkan {
class Instance;class Scheduler;
// One independently owned720 input/return slot. Caller must prove actual last-use
// completion before another RecordSource/descriptor update or destruction.
class NativePreSrGpu {
public:
 explicit NativePreSrGpu(const Instance&);
 static bool SourcesSupported(const std::array<vk::Image,3>&,const std::array<vk::ImageCreateInfo,3>&)noexcept;
 void RecordSource(Scheduler&,const std::array<vk::Image,3>&,
                   const std::array<vk::ImageCreateInfo,3>&);
 void RecordReturn(Scheduler&,vk::ImageView actual_owned_output);
 std::array<vk::Image,3> Converted()const noexcept;
 vk::Image ReturnImage()const noexcept{return vk::Image(returned.image);}
 vk::ImageView ReturnView()const noexcept{return *returned.view;}
 uint32_t ReadGuardAfterGPUComplete(); // caller must independently prove target
private:
 struct Image{VideoCore::UniqueImage image;vk::UniqueImageView view;};
 Image Make(vk::Format,vk::ImageAspectFlagBits);
 vk::Device device;VmaAllocator allocator;
 std::array<Image,3> source,converted;
 Image returned;
 vk::UniqueImageView source_bits,return_bits;
 std::unique_ptr<VideoCore::Buffer> guard;
 vk::UniqueSampler nearest;
 vk::UniqueDescriptorSetLayout input_layout,return_layout;
 vk::UniquePipelineLayout input_pl,return_pl;
 vk::UniquePipeline input_pipeline,return_pipeline;
 vk::UniqueDescriptorPool pool;
 vk::DescriptorSet input_set{},return_set{};
 bool initialized=false,return_initialized=false;
};
}
