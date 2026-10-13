// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_pre_sr_gpu.h"
#include "vk_instance.h"
#include "vk_scheduler.h"
#include "vk_shader_util.h"
#include "video_core/host_shaders/native_sky_convert_comp.h"
#include "video_core/host_shaders/native_pre_sr_return_comp.h"
#include <cstring>
#include <stdexcept>
namespace Vulkan {
NativePreSrGpu::Image NativePreSrGpu::Make(vk::Format f,vk::ImageAspectFlagBits aspect){
 Image x;x.image=VideoCore::UniqueImage(device,allocator);
 const bool depth=aspect==vk::ImageAspectFlagBits::eDepth;
 x.image.Create({.flags=f==vk::Format::eR16G16B16A16Sfloat?vk::ImageCreateFlags(vk::ImageCreateFlagBits::eMutableFormat):vk::ImageCreateFlags{},
 .imageType=vk::ImageType::e2D,.format=f,.extent={1280,720,1},.mipLevels=1,.arrayLayers=1,
 .samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,
 .usage=vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferSrc|vk::ImageUsageFlagBits::eTransferDst|
        (depth?vk::ImageUsageFlags{}:vk::ImageUsageFlags(vk::ImageUsageFlagBits::eStorage)),
 .initialLayout=vk::ImageLayout::eUndefined});
 x.view=device.createImageViewUnique({.image=vk::Image(x.image),.viewType=vk::ImageViewType::e2D,.format=f,
 .subresourceRange={aspect,0,1,0,1}}).value;return x;
}
NativePreSrGpu::NativePreSrGpu(const Instance&i):device(i.GetDevice()),allocator(i.GetAllocator()){
 if(!i.IsSeparateDepthStencilLayoutsEnabled()||!i.GetPhysicalDevice().getFeatures().shaderStorageImageExtendedFormats)
  throw std::runtime_error("preSR source aspect/extended formats unsupported");
 for(auto f:{vk::Format::eR32G32B32A32Sfloat,vk::Format::eR32Sfloat,vk::Format::eR32G32Sfloat,vk::Format::eR16G16B16A16Sfloat}){
  auto flags=i.GetPhysicalDevice().getFormatProperties(f).optimalTilingFeatures;
  auto need=vk::FormatFeatureFlagBits::eStorageImage|vk::FormatFeatureFlagBits::eSampledImage|vk::FormatFeatureFlagBits::eTransferSrc|vk::FormatFeatureFlagBits::eTransferDst;
  if((flags&need)!=need)throw std::runtime_error("preSR format capabilities missing");
 }
 source[0]=Make(vk::Format::eR16G16B16A16Sfloat,vk::ImageAspectFlagBits::eColor);
 source[1]=Make(vk::Format::eD32SfloatS8Uint,vk::ImageAspectFlagBits::eDepth);
 source[2]=Make(vk::Format::eR16G16Sfloat,vk::ImageAspectFlagBits::eColor);
 converted[0]=Make(vk::Format::eR32G32B32A32Sfloat,vk::ImageAspectFlagBits::eColor);
 converted[1]=Make(vk::Format::eR32Sfloat,vk::ImageAspectFlagBits::eColor);
 converted[2]=Make(vk::Format::eR32G32Sfloat,vk::ImageAspectFlagBits::eColor);
 returned=Make(vk::Format::eR16G16B16A16Sfloat,vk::ImageAspectFlagBits::eColor);
 if(!(i.GetPhysicalDevice().getFormatProperties(vk::Format::eR16G16B16A16Uint).optimalTilingFeatures&vk::FormatFeatureFlagBits::eStorageImage))throw std::runtime_error("preSR exact original alpha uint16 storage unsupported");
 const auto bits=[&](vk::Image image){return device.createImageViewUnique({.image=image,.viewType=vk::ImageViewType::e2D,.format=vk::Format::eR16G16B16A16Uint,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}}).value;};
 source_bits=bits(vk::Image(source[0].image));return_bits=bits(vk::Image(returned.image));
 guard=std::make_unique<VideoCore::Buffer>(i,0,4,VideoCore::MemoryType::HostCached,"preSR720 return finite/alpha guard");
 nearest=device.createSamplerUnique({.magFilter=vk::Filter::eNearest,.minFilter=vk::Filter::eNearest,
 .mipmapMode=vk::SamplerMipmapMode::eNearest,.addressModeU=vk::SamplerAddressMode::eClampToEdge,
 .addressModeV=vk::SamplerAddressMode::eClampToEdge,.addressModeW=vk::SamplerAddressMode::eClampToEdge,.maxLod=0}).value;
 std::array<vk::DescriptorSetLayoutBinding,6> in{};
 for(unsigned k=0;k<6;k++)in[k]={.binding=k,.descriptorType=k<3?vk::DescriptorType::eCombinedImageSampler:vk::DescriptorType::eStorageImage,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute};
 input_layout=device.createDescriptorSetLayoutUnique({.bindingCount=6,.pBindings=in.data()}).value;
 std::array<vk::DescriptorSetLayoutBinding,4> out{};
 for(unsigned k=0;k<4;k++)out[k]={.binding=k,.descriptorType=k==0?vk::DescriptorType::eCombinedImageSampler:k<3?vk::DescriptorType::eStorageImage:vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute};
 return_layout=device.createDescriptorSetLayoutUnique({.bindingCount=4,.pBindings=out.data()}).value;
 input_pl=device.createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*input_layout}).value;
 return_pl=device.createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*return_layout}).value;
 const auto pipeline=[&](auto code,vk::PipelineLayout pl){auto module=CompileSPV(code,device);
  auto p=device.createComputePipelineUnique({}, {.stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=module,.pName="main"},.layout=pl});device.destroyShaderModule(module);return std::move(p.value);};
 input_pipeline=pipeline(std::span<const u32>(NATIVE_SKY_CONVERT_COMP),*input_pl);
 return_pipeline=pipeline(std::span<const u32>(NATIVE_PRE_SR_RETURN_COMP),*return_pl);
 const std::array<vk::DescriptorPoolSize,3> sizes{{{vk::DescriptorType::eCombinedImageSampler,4},{vk::DescriptorType::eStorageImage,5},{vk::DescriptorType::eStorageBuffer,1}}};
 pool=device.createDescriptorPoolUnique({.maxSets=2,.poolSizeCount=3,.pPoolSizes=sizes.data()}).value;
 input_set=device.allocateDescriptorSets({.descriptorPool=*pool,.descriptorSetCount=1,.pSetLayouts=&*input_layout}).value[0];
 return_set=device.allocateDescriptorSets({.descriptorPool=*pool,.descriptorSetCount=1,.pSetLayouts=&*return_layout}).value[0];
}
std::array<vk::Image,3> NativePreSrGpu::Converted()const noexcept{return {vk::Image(converted[0].image),vk::Image(converted[1].image),vk::Image(converted[2].image)};}
static void Barrier(vk::CommandBuffer cmd,vk::Image image,vk::ImageAspectFlagBits aspect,
 vk::PipelineStageFlags2 src,vk::AccessFlags2 sa,vk::PipelineStageFlags2 dst,vk::AccessFlags2 da,
 vk::ImageLayout old=vk::ImageLayout::eGeneral){
 const vk::ImageMemoryBarrier2 b{.srcStageMask=src,.srcAccessMask=sa,.dstStageMask=dst,.dstAccessMask=da,
 .oldLayout=old,.newLayout=vk::ImageLayout::eGeneral,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
 .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={aspect,0,1,0,1}};
 cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b});
}
bool NativePreSrGpu::SourcesSupported(const std::array<vk::Image,3>&images,const std::array<vk::ImageCreateInfo,3>&ci)noexcept {
 std::array<BbPreSr::SourceCI,3> actual{};
 for(unsigned k=0;k<3;k++){if(!images[k])return false;const auto&c=ci[k];
  actual[k]={uint32_t(c.format),c.extent.width,c.extent.height,c.extent.depth,uint32_t(c.samples),c.mipLevels,c.arrayLayers,bool(c.usage&vk::ImageUsageFlagBits::eTransferSrc)};
 }
 return BbPreSr::SourcesSupported(actual);
}
void NativePreSrGpu::RecordSource(Scheduler&s,const std::array<vk::Image,3>&actual,const std::array<vk::ImageCreateInfo,3>&ci){
 if(!SourcesSupported(actual,ci))throw std::runtime_error("preSR actual source CI/TransferSrc720 mismatch");
 auto cmd=s.CommandBuffer();const auto all=vk::PipelineStageFlagBits2::eAllCommands;
 const auto rw=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite;
 for(unsigned k=0;k<3;k++){
  const auto a=k==1?vk::ImageAspectFlagBits::eDepth:vk::ImageAspectFlagBits::eColor;
  Barrier(cmd,actual[k],a,all,rw,vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferRead);
  Barrier(cmd,vk::Image(source[k].image),a,all,rw,vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferWrite,initialized?vk::ImageLayout::eGeneral:vk::ImageLayout::eUndefined);
  const vk::ImageCopy cp{.srcSubresource={a,0,0,1},.dstSubresource={a,0,0,1},.extent={1280,720,1}};
  cmd.copyImage(actual[k],vk::ImageLayout::eGeneral,vk::Image(source[k].image),vk::ImageLayout::eGeneral,cp);
  Barrier(cmd,actual[k],a,vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferRead,all,rw);
  Barrier(cmd,vk::Image(source[k].image),a,vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferWrite,vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderRead);
  Barrier(cmd,vk::Image(converted[k].image),vk::ImageAspectFlagBits::eColor,all,rw,vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderWrite,initialized?vk::ImageLayout::eGeneral:vk::ImageLayout::eUndefined);
 }
 std::array<vk::DescriptorImageInfo,6> info{};std::array<vk::WriteDescriptorSet,6> writes{};
 for(unsigned k=0;k<6;k++){info[k]={.sampler=k<3?*nearest:vk::Sampler{},.imageView=k<3?*source[k].view:*converted[k-3].view,.imageLayout=vk::ImageLayout::eGeneral};writes[k]={.dstSet=input_set,.dstBinding=k,.descriptorCount=1,.descriptorType=k<3?vk::DescriptorType::eCombinedImageSampler:vk::DescriptorType::eStorageImage,.pImageInfo=&info[k]};}
 device.updateDescriptorSets(writes,{});cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*input_pipeline);
 cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,*input_pl,0,input_set,{});cmd.dispatch(160,90,1);
 for(auto& x:converted)Barrier(cmd,vk::Image(x.image),vk::ImageAspectFlagBits::eColor,vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderWrite,all,rw);
 initialized=true;
}
void NativePreSrGpu::RecordReturn(Scheduler&s,vk::ImageView owned_output){
 if(!initialized||!owned_output)throw std::runtime_error("preSR return without source/owned output");
 auto cmd=s.CommandBuffer();cmd.fillBuffer(guard->Handle(),0,4,0);
 const vk::BufferMemoryBarrier2 before{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,.srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eComputeShader,.dstAccessMask=vk::AccessFlagBits2::eShaderRead|vk::AccessFlagBits2::eShaderWrite,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=guard->Handle(),.offset=0,.size=4};cmd.pipelineBarrier2({.bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&before});
 Barrier(cmd,vk::Image(returned.image),vk::ImageAspectFlagBits::eColor,vk::PipelineStageFlagBits2::eAllCommands,vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderWrite,return_initialized?vk::ImageLayout::eGeneral:vk::ImageLayout::eUndefined);
 const std::array<vk::DescriptorImageInfo,3> info{{{.sampler=*nearest,.imageView=owned_output,.imageLayout=vk::ImageLayout::eGeneral},{.imageView=*source_bits,.imageLayout=vk::ImageLayout::eGeneral},{.imageView=*return_bits,.imageLayout=vk::ImageLayout::eGeneral}}};
 vk::DescriptorBufferInfo gi{.buffer=guard->Handle(),.offset=0,.range=4};std::array<vk::WriteDescriptorSet,4>writes{};
 for(unsigned k=0;k<4;k++)writes[k]={.dstSet=return_set,.dstBinding=k,.descriptorCount=1,.descriptorType=k==0?vk::DescriptorType::eCombinedImageSampler:k<3?vk::DescriptorType::eStorageImage:vk::DescriptorType::eStorageBuffer,.pImageInfo=k<3?&info[k]:nullptr,.pBufferInfo=k==3?&gi:nullptr};
 device.updateDescriptorSets(writes,{});cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*return_pipeline);cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,*return_pl,0,return_set,{});cmd.dispatch(160,90,1);
 auto host=before;host.srcStageMask=vk::PipelineStageFlagBits2::eTransfer|vk::PipelineStageFlagBits2::eComputeShader;host.srcAccessMask=vk::AccessFlagBits2::eTransferWrite|vk::AccessFlagBits2::eShaderWrite;host.dstStageMask=vk::PipelineStageFlagBits2::eHost;host.dstAccessMask=vk::AccessFlagBits2::eHostRead;cmd.pipelineBarrier2({.bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&host});
 Barrier(cmd,vk::Image(returned.image),vk::ImageAspectFlagBits::eColor,vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderWrite,vk::PipelineStageFlagBits2::eAllCommands,vk::AccessFlagBits2::eShaderRead);return_initialized=true;
}
uint32_t NativePreSrGpu::ReadGuardAfterGPUComplete(){guard->Invalidate(0,4);uint32_t value;std::memcpy(&value,guard->mapped_data.data(),4);return value;}
}
