// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_sky_rolling_gpu.h"
#include "native_sky_r2_spv.h"
#include "vk_instance.h"
#include "vk_scheduler.h"
#include <vk_mem_alloc.h>
#include "vk_shader_util.h"
#include "video_core/host_shaders/native_sky_convert_comp.h"
#include <cstring>
#include <stdexcept>
namespace Vulkan {
NativeSkyRollingGpu::Image NativeSkyRollingGpu::Make(vk::Format f,unsigned w,unsigned h){
 Image x;x.image=VideoCore::UniqueImage(device,allocator);
 x.image.Create({.imageType=vk::ImageType::e2D,.format=f,.extent={w,h,1},.mipLevels=1,.arrayLayers=1,
  .samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,
  .usage=vk::ImageUsageFlagBits::eStorage|vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eTransferSrc,
  .initialLayout=vk::ImageLayout::eUndefined});
 x.view=device.createImageViewUnique({.image=vk::Image(x.image),.viewType=vk::ImageViewType::e2D,.format=f,
  .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}}).value;return x;
}
NativeSkyRollingGpu::NativeSkyRollingGpu(const Instance& i):device(i.GetDevice()),allocator(i.GetAllocator()){
 for(auto f:{vk::Format::eR32G32B32A32Sfloat,vk::Format::eR32Sfloat,vk::Format::eR32G32Sfloat,vk::Format::eR32Uint}){
  const auto flags=i.GetPhysicalDevice().getFormatProperties(f).optimalTilingFeatures;
  const auto required=vk::FormatFeatureFlagBits::eStorageImage|vk::FormatFeatureFlagBits::eSampledImage|vk::FormatFeatureFlagBits::eTransferDst|vk::FormatFeatureFlagBits::eTransferSrc;
  if((flags&required)!=required)throw std::runtime_error("Native Sky format support missing");
  if(f==vk::Format::eR32G32B32A32Sfloat&&!(flags&vk::FormatFeatureFlagBits::eSampledImageFilterLinear))throw std::runtime_error("Native Sky RGBA32F linear unsupported");
  if(f==vk::Format::eR32Uint&&!(flags&vk::FormatFeatureFlagBits::eStorageImageAtomic))throw std::runtime_error("Native Sky R32UI atomic unsupported");
 }
 if(!i.GetPhysicalDevice().getFeatures().shaderStorageImageExtendedFormats)throw std::runtime_error("Native Sky extended storage formats unsupported");
 const auto limits=i.GetPhysicalDevice().getProperties().limits;
 if(limits.maxPerStageDescriptorStorageImages<7||limits.maxPerStageDescriptorSampledImages<12||limits.maxUniformBufferRange<256)throw std::runtime_error("Native Sky descriptor limits");
 for(auto& b:banks){b.edit=Make(vk::Format::eR32G32B32A32Sfloat,1920,1080);b.guide=Make(vk::Format::eR32G32B32A32Sfloat,1920,1080);b.l1guide=Make(vk::Format::eR32G32B32A32Sfloat,480,270);unsigned w=480,h=270;for(auto& l:b.levels){l=Make(vk::Format::eR32G32B32A32Sfloat,w,h);w=(w+3)/4;h=(h+3)/4;}}
 dummy=Make(vk::Format::eR32G32B32A32Sfloat,1,1);exposure=Make(vk::Format::eR32G32B32A32Sfloat,1,1);stats=Make(vk::Format::eR32Uint,4,1);
 const auto sampler=[&](vk::Filter f){return device.createSamplerUnique({.magFilter=f,.minFilter=f,.mipmapMode=vk::SamplerMipmapMode::eNearest,.addressModeU=vk::SamplerAddressMode::eClampToEdge,.addressModeV=vk::SamplerAddressMode::eClampToEdge,.addressModeW=vk::SamplerAddressMode::eClampToEdge,.maxLod=0}).value;};
 linear=sampler(vk::Filter::eLinear);nearest=sampler(vk::Filter::eNearest);
 std::array<vk::DescriptorSetLayoutBinding,21> bindings{};
 for(unsigned k=0;k<21;k++)bindings[k]={.binding=k,.descriptorType=k==0?vk::DescriptorType::eUniformBuffer:k<=12?vk::DescriptorType::eSampledImage:k<=19?vk::DescriptorType::eStorageImage:vk::DescriptorType::eSampler,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute};
 layout=device.createDescriptorSetLayoutUnique({.bindingCount=21,.pBindings=bindings.data()}).value;
 std::array<vk::DescriptorSetLayoutBinding,6> cb{};for(unsigned k=0;k<6;k++)cb[k]={.binding=k,.descriptorType=k<3?vk::DescriptorType::eCombinedImageSampler:vk::DescriptorType::eStorageImage,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute};
 conversion_layout=device.createDescriptorSetLayoutUnique({.bindingCount=6,.pBindings=cb.data()}).value;
 pipeline_layout=device.createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*layout}).value;
 conversion_pipeline_layout=device.createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*conversion_layout}).value;
 const auto create=[&](std::span<const u32> code,vk::PipelineLayout pl,const char* entry){auto mod=CompileSPV(code,device);auto p=device.createComputePipelineUnique({}, {.stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=mod,.pName=entry},.layout=pl});device.destroyShaderModule(mod);return std::move(p.value);};
 pipeline=create(NativeSkyR2Spv,*pipeline_layout,"CSMain");conversion_pipeline=create(NATIVE_SKY_CONVERT_COMP,*conversion_pipeline_layout,"main");
 std::array<vk::DescriptorPoolSize,5> sizes{{{vk::DescriptorType::eUniformBuffer,56},{vk::DescriptorType::eSampledImage,672},{vk::DescriptorType::eStorageImage,413},{vk::DescriptorType::eSampler,56},{vk::DescriptorType::eCombinedImageSampler,21}}};
 pool=device.createDescriptorPoolUnique({.maxSets=63,.poolSizeCount=5,.pPoolSizes=sizes.data()}).value;
 for(auto& p:packets){p.converted[0]=Make(vk::Format::eR32G32B32A32Sfloat,1920,1080);p.converted[1]=Make(vk::Format::eR32Sfloat,1280,720);p.converted[2]=Make(vk::Format::eR32G32Sfloat,1280,720);p.output=Make(vk::Format::eR32G32B32A32Sfloat,1920,1080);
  for(unsigned j=0;j<4;j++){p.constants[j]=std::make_unique<VideoCore::Buffer>(i,0,256,VideoCore::MemoryType::HostCached,"Native Sky tick-owned constants");p.sets[j]=device.allocateDescriptorSets({.descriptorPool=*pool,.descriptorSetCount=1,.pSetLayouts=&*layout}).value.front();}
  p.conversion=device.allocateDescriptorSets({.descriptorPool=*pool,.descriptorSetCount=1,.pSetLayouts=&*conversion_layout}).value.front();
 }
 for(auto& p:replay_packets){p.output=Make(vk::Format::eR32G32B32A32Sfloat,1920,1080);for(unsigned j=0;j<4;j++){p.constants[j]=std::make_unique<VideoCore::Buffer>(i,0,256,VideoCore::MemoryType::HostCached,"Native Sky sparse catchup tick-owned constants");p.sets[j]=device.allocateDescriptorSets({.descriptorPool=*pool,.descriptorSetCount=1,.pSetLayouts=&*layout}).value.front();}}

}
void NativeSkyRollingGpu::Barrier(vk::CommandBuffer cmd){const vk::MemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,.srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,.dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,.dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite};cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&b});}
void NativeSkyRollingGpu::InitImages(vk::CommandBuffer cmd){
 vk::ClearColorValue zero{};for(unsigned k=0;k<4;k++)zero.uint32[k]=0;
 const auto init=[&](Image& x){const vk::ImageMemoryBarrier2 b{.dstStageMask=vk::PipelineStageFlagBits2::eTransfer,.dstAccessMask=vk::AccessFlagBits2::eTransferWrite,.oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eGeneral,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=vk::Image(x.image),.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b});cmd.clearColorImage(vk::Image(x.image),vk::ImageLayout::eGeneral,zero,vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor,0,1,0,1});};
 for(auto& b:banks){init(b.edit);init(b.guide);init(b.l1guide);for(auto& l:b.levels)init(l);}for(auto& p:packets){for(auto& x:p.converted)init(x);init(p.output);}for(auto& p:replay_packets)init(p.output);init(dummy);init(exposure);init(stats);Barrier(cmd);initialized=true;
}
DlssNrCacheConstants NativeSkyRollingGpu::Base(unsigned m)const {DlssNrCacheConstants c{};c.Mode=m;c.Width=1920;c.Height=1080;c.DepthWidth=c.MotionWidth=1280;c.DepthHeight=c.MotionHeight=720;c.MvScaleX=c.MvScaleY=1;c.Epsilon=1.f/512;c.DepthTolerance=.1f;c.ColourTolerance=.5f;c.HighDecay=.97f;c.RefreshBlend=c.LowGain=c.HighGain=1;c.Bilateral=1;c.ExposurePreMul=1;c.MaxLumaEdit=1.5f;return c;}
void NativeSkyRollingGpu::Pass(vk::CommandBuffer cmd,Packet& p,unsigned slot,const DlssNrCacheConstants& c,bool whole,const std::array<Image*,12>& reads,const std::array<Image*,7>& writes){
 auto& buf=*p.constants[slot];std::memcpy(buf.mapped_data.data(),&c,256);const uint32_t v=whole?1:0;std::memcpy(buf.mapped_data.data()+156,&v,4);buf.Flush(0,256);
 const vk::BufferMemoryBarrier2 hb{.srcStageMask=vk::PipelineStageFlagBits2::eHost,.srcAccessMask=vk::AccessFlagBits2::eHostWrite,.dstStageMask=vk::PipelineStageFlagBits2::eComputeShader,.dstAccessMask=vk::AccessFlagBits2::eUniformRead,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=buf.Handle(),.offset=0,.size=256};cmd.pipelineBarrier2({.bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&hb});
 const vk::DescriptorBufferInfo bi{buf.Handle(),0,256};std::array<vk::DescriptorImageInfo,20> ii{};std::array<vk::WriteDescriptorSet,21> ds{};
 for(unsigned k=0;k<12;k++)ii[k]={.imageView=reads[k]->View(),.imageLayout=vk::ImageLayout::eGeneral};for(unsigned k=0;k<7;k++)ii[k+12]={.imageView=writes[k]->View(),.imageLayout=vk::ImageLayout::eGeneral};ii[19].sampler=*linear;
 for(unsigned k=0;k<21;k++)ds[k]={.dstSet=p.sets[slot],.dstBinding=k,.descriptorCount=1,.descriptorType=k==0?vk::DescriptorType::eUniformBuffer:k<=12?vk::DescriptorType::eSampledImage:k<=19?vk::DescriptorType::eStorageImage:vk::DescriptorType::eSampler,.pImageInfo=k?&ii[k-1]:nullptr,.pBufferInfo=k?nullptr:&bi};
 device.updateDescriptorSets(ds,{});cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*pipeline);cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,*pipeline_layout,0,p.sets[slot],{});cmd.dispatch((c.Width+7)/8,(c.Height+7)/8,1);Barrier(cmd);
}
void NativeSkyRollingGpu::Convert(Scheduler& scheduler,unsigned slot,const std::array<vk::Image,3>&source){
 if(slot>=packets.size())throw std::runtime_error("rolling physical slot out of bounds");
 auto cmd=scheduler.CommandBuffer();if(!initialized)InitImages(cmd);auto&p=packets[slot];
 for(unsigned k=0;k<3;k++)p.source_views[k]=device.createImageViewUnique({.image=source[k],.viewType=vk::ImageViewType::e2D,.format=k==0?vk::Format::eR16G16B16A16Sfloat:k==1?vk::Format::eD32SfloatS8Uint:vk::Format::eR16G16Sfloat,.subresourceRange={k==1?vk::ImageAspectFlagBits::eDepth:vk::ImageAspectFlagBits::eColor,0,1,0,1}}).value;
 std::array<vk::DescriptorImageInfo,6>ci{};std::array<vk::WriteDescriptorSet,6>cw{};
 for(unsigned k=0;k<6;k++){ci[k]={.sampler=k<3?*nearest:vk::Sampler{},.imageView=k<3?*p.source_views[k]:*p.converted[k-3].view,.imageLayout=vk::ImageLayout::eGeneral};cw[k]={.dstSet=p.conversion,.dstBinding=k,.descriptorCount=1,.descriptorType=k<3?vk::DescriptorType::eCombinedImageSampler:vk::DescriptorType::eStorageImage,.pImageInfo=&ci[k]};}
 Barrier(cmd);device.updateDescriptorSets(cw,{});cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*conversion_pipeline);cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,*conversion_pipeline_layout,0,p.conversion,{});cmd.dispatch(240,135,1);Barrier(cmd);
 packet_use[slot]=scheduler.CurrentTick();last_use=std::max(last_use,scheduler.CurrentTick());
}
vk::ImageView NativeSkyRollingGpu::Advance(Scheduler&s,unsigned slot){
 if(slot>=packets.size()||!history_valid)return {};
 auto cmd=s.CommandBuffer();auto&p=packets[slot];auto&prev=banks[active_bank];auto&next=banks[1-active_bank];
 auto&color=p.converted[0];auto&depth=p.converted[1];auto&mv=p.converted[2];
 std::array<Image*,12>reads{&prev.edit,&prev.guide,&color,&depth,&mv,&color,&dummy,&dummy,&dummy,&dummy,&exposure,&dummy};
 auto c=Base(1);c.HistoryValid=1;Pass(cmd,p,0,c,true,reads,{&next.edit,&next.guide,&p.output,&next.levels[0],&next.l1guide,&stats,&dummy});
 for(unsigned j=1;j<3;j++){auto d=Base(3);d.Width=next.levels[j].image.image_ci.extent.width;d.Height=next.levels[j].image.image_ci.extent.height;d.SourceWidth=next.levels[j-1].image.image_ci.extent.width;d.SourceHeight=next.levels[j-1].image.image_ci.extent.height;reads[5]=&next.levels[j-1];Pass(cmd,p,j,d,true,reads,{&next.levels[j],&dummy,&p.output,&dummy,&dummy,&stats,&dummy});}
 reads={&next.edit,&next.guide,&color,&depth,&mv,&next.levels[0],&next.l1guide,&next.levels[1],&next.levels[2],&dummy,&exposure,&dummy};Pass(cmd,p,3,Base(4),true,reads,{&p.output,&dummy,&dummy,&dummy,&dummy,&stats,&dummy});
 active_bank=1-active_bank;packet_use[slot]=s.CurrentTick();TouchBanks(s.CurrentTick());return *p.output.view;
}
vk::ImageView NativeSkyRollingGpu::CaptureAndCatchup(Scheduler&s,const BbNativeAsync::Rolling::Span&span,vk::ImageView nr){
 if(!nr||!span||span.size>BbNativeAsync::NodeCount||replay_busy.exchange(true,std::memory_order_acq_rel))return {};
 auto cmd=s.CommandBuffer();Image edited;edited.borrowed=nr;
 for(unsigned index=0;index<span.size;index++){
  const unsigned slot=span.slots[index];if(slot>=packets.size())throw std::runtime_error("rolling actual span slot");
  auto&p=replay_packets[slot];auto&src=packets[slot];auto&prev=banks[index%2];auto&next=banks[(index+1)%2];
  auto&color=src.converted[0];auto&depth=src.converted[1];auto&mv=src.converted[2];
  std::array<Image*,12>reads{&prev.edit,&prev.guide,&color,&depth,&mv,&edited,&dummy,&dummy,&dummy,&dummy,&exposure,&dummy};
  auto c=Base(index?1:2);c.HistoryValid=index?1:0;
  Barrier(cmd);Pass(cmd,p,0,c,true,reads,{&next.edit,&next.guide,&p.output,&next.levels[0],&next.l1guide,&stats,&dummy});
  for(unsigned j=1;j<3;j++){auto d=Base(3);d.Width=next.levels[j].image.image_ci.extent.width;d.Height=next.levels[j].image.image_ci.extent.height;d.SourceWidth=next.levels[j-1].image.image_ci.extent.width;d.SourceHeight=next.levels[j-1].image.image_ci.extent.height;reads[5]=&next.levels[j-1];Pass(cmd,p,j,d,true,reads,{&next.levels[j],&dummy,&p.output,&dummy,&dummy,&stats,&dummy});}
  if(index+1==span.size){reads={&next.edit,&next.guide,&color,&depth,&mv,&next.levels[0],&next.l1guide,&next.levels[1],&next.levels[2],&dummy,&exposure,&dummy};Pass(cmd,p,3,Base(4),true,reads,{&p.output,&dummy,&dummy,&dummy,&dummy,&stats,&dummy});}
  packet_use[slot]=replay_use[slot]=s.CurrentTick();
 }
 active_bank=span.size%2;history_valid=true;TouchBanks(s.CurrentTick());return *replay_packets[span.slots[span.size-1]].output.view;
}
void NativeSkyRollingGpu::TouchBanks(uint64_t tick){for(auto&v:bank_use)v=std::max(v,tick);last_use=std::max(last_use,tick);}
uint64_t NativeSkyRollingGpu::AllocatedImageBytes()const noexcept{
 uint64_t n=0;const auto add=[&](const Image&x){VmaAllocationInfo info{};if(x.image.allocation){vmaGetAllocationInfo(allocator,x.image.allocation,&info);n+=info.size;}};
 for(const auto&b:banks){add(b.edit);add(b.guide);add(b.l1guide);for(const auto&x:b.levels)add(x);}for(const auto&p:packets){for(const auto&x:p.converted)add(x);add(p.output);}for(const auto&p:replay_packets)add(p.output);add(dummy);add(exposure);add(stats);return n;
}
uint64_t NativeSkyRollingGpu::ImageBytes()const noexcept{uint64_t n=0;const auto add=[&](const Image& x){const auto& c=x.image.image_ci;n+=uint64_t(c.extent.width)*c.extent.height*(c.format==vk::Format::eR32G32B32A32Sfloat?16:c.format==vk::Format::eR32G32Sfloat?8:4);};for(const auto& b:banks){add(b.edit);add(b.guide);add(b.l1guide);for(const auto& l:b.levels)add(l);}for(const auto& p:packets){for(const auto& x:p.converted)add(x);add(p.output);}for(const auto& p:replay_packets)add(p.output);add(dummy);add(exposure);add(stats);return n;}
}
