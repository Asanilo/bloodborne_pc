// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_async_pre_sr_sky.h"
#include "native_sky_r2_spv.h"
#include "vk_instance.h"
#include "vk_scheduler.h"
#include "vk_shader_util.h"
#include "video_core/host_shaders/native_sky_convert_comp.h"
#include <cstring>
#include <stdexcept>
namespace Vulkan {
NativeAsyncPreSrSky::Image NativeAsyncPreSrSky::Make(vk::Format f,unsigned w,unsigned h){
 Image x;x.image=VideoCore::UniqueImage(device,allocator);
 x.image.Create({.imageType=vk::ImageType::e2D,.format=f,.extent={w,h,1},.mipLevels=1,.arrayLayers=1,
  .samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,
  .usage=vk::ImageUsageFlagBits::eStorage|vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eTransferSrc,
  .initialLayout=vk::ImageLayout::eUndefined});
 x.view=device.createImageViewUnique({.image=vk::Image(x.image),.viewType=vk::ImageViewType::e2D,.format=f,
  .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}}).value;return x;
}
NativeAsyncPreSrSky::NativeAsyncPreSrSky(const Instance& i):device(i.GetDevice()),allocator(i.GetAllocator()){
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
 for(auto& b:banks){b.edit=Make(vk::Format::eR32G32B32A32Sfloat,1280,720);b.guide=Make(vk::Format::eR32G32B32A32Sfloat,1280,720);b.l1guide=Make(vk::Format::eR32G32B32A32Sfloat,320,180);unsigned w=320,h=180;for(auto& l:b.levels){l=Make(vk::Format::eR32G32B32A32Sfloat,w,h);w=(w+3)/4;h=(h+3)/4;}}
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
 std::array<vk::DescriptorPoolSize,5> sizes{{{vk::DescriptorType::eUniformBuffer,40},{vk::DescriptorType::eSampledImage,480},{vk::DescriptorType::eStorageImage,295},{vk::DescriptorType::eSampler,40},{vk::DescriptorType::eCombinedImageSampler,15}}};
 pool=device.createDescriptorPoolUnique({.maxSets=45,.poolSizeCount=5,.pPoolSizes=sizes.data()}).value;
 for(auto& p:packets){p.converted[0]=Make(vk::Format::eR32G32B32A32Sfloat,1280,720);p.converted[1]=Make(vk::Format::eR32Sfloat,1280,720);p.converted[2]=Make(vk::Format::eR32G32Sfloat,1280,720);p.output=Make(vk::Format::eR32G32B32A32Sfloat,1280,720);
  for(unsigned j=0;j<4;j++){p.constants[j]=std::make_unique<VideoCore::Buffer>(i,0,256,VideoCore::MemoryType::HostCached,"Native Sky tick-owned constants");p.sets[j]=device.allocateDescriptorSets({.descriptorPool=*pool,.descriptorSetCount=1,.pSetLayouts=&*layout}).value.front();}
  p.conversion=device.allocateDescriptorSets({.descriptorPool=*pool,.descriptorSetCount=1,.pSetLayouts=&*conversion_layout}).value.front();
 }
 for(auto& p:replay_packets){p.output=Make(vk::Format::eR32G32B32A32Sfloat,1280,720);for(unsigned j=0;j<4;j++){p.constants[j]=std::make_unique<VideoCore::Buffer>(i,0,256,VideoCore::MemoryType::HostCached,"Native Sky sparse catchup tick-owned constants");p.sets[j]=device.allocateDescriptorSets({.descriptorPool=*pool,.descriptorSetCount=1,.pSetLayouts=&*layout}).value.front();}}

}
void NativeAsyncPreSrSky::Barrier(vk::CommandBuffer cmd){const vk::MemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,.srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,.dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,.dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite};cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&b});}
void NativeAsyncPreSrSky::InitImages(vk::CommandBuffer cmd){
 vk::ClearColorValue zero{};for(unsigned k=0;k<4;k++)zero.uint32[k]=0;
 const auto init=[&](Image& x){const vk::ImageMemoryBarrier2 b{.dstStageMask=vk::PipelineStageFlagBits2::eTransfer,.dstAccessMask=vk::AccessFlagBits2::eTransferWrite,.oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eGeneral,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=vk::Image(x.image),.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b});cmd.clearColorImage(vk::Image(x.image),vk::ImageLayout::eGeneral,zero,vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor,0,1,0,1});};
 for(auto& b:banks){init(b.edit);init(b.guide);init(b.l1guide);for(auto& l:b.levels)init(l);}for(auto& p:packets){for(auto& x:p.converted)init(x);init(p.output);}for(auto& p:replay_packets)init(p.output);init(dummy);init(exposure);init(stats);Barrier(cmd);initialized=true;
}
DlssNrCacheConstants NativeAsyncPreSrSky::Base(unsigned m)const {DlssNrCacheConstants c{};c.Mode=m;c.Width=1280;c.Height=720;c.DepthWidth=c.MotionWidth=1280;c.DepthHeight=c.MotionHeight=720;c.MvScaleX=c.MvScaleY=1;c.Epsilon=1.f/512;c.DepthTolerance=.1f;c.ColourTolerance=.5f;c.HighDecay=.97f;c.RefreshBlend=c.LowGain=c.HighGain=1;c.Bilateral=1;c.ExposurePreMul=1;c.MaxLumaEdit=1.5f;return c;}
void NativeAsyncPreSrSky::Pass(vk::CommandBuffer cmd,Packet& p,unsigned slot,const DlssNrCacheConstants& c,bool whole,const std::array<Image*,12>& reads,const std::array<Image*,7>& writes){
 auto& buf=*p.constants[slot];std::memcpy(buf.mapped_data.data(),&c,256);const uint32_t v=whole?1:0;std::memcpy(buf.mapped_data.data()+156,&v,4);buf.Flush(0,256);
 const vk::BufferMemoryBarrier2 hb{.srcStageMask=vk::PipelineStageFlagBits2::eHost,.srcAccessMask=vk::AccessFlagBits2::eHostWrite,.dstStageMask=vk::PipelineStageFlagBits2::eComputeShader,.dstAccessMask=vk::AccessFlagBits2::eUniformRead,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=buf.Handle(),.offset=0,.size=256};cmd.pipelineBarrier2({.bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&hb});
 const vk::DescriptorBufferInfo bi{buf.Handle(),0,256};std::array<vk::DescriptorImageInfo,20> ii{};std::array<vk::WriteDescriptorSet,21> ds{};
 for(unsigned k=0;k<12;k++)ii[k]={.imageView=reads[k]->View(),.imageLayout=vk::ImageLayout::eGeneral};for(unsigned k=0;k<7;k++)ii[k+12]={.imageView=writes[k]->View(),.imageLayout=vk::ImageLayout::eGeneral};ii[19].sampler=*linear;
 for(unsigned k=0;k<21;k++)ds[k]={.dstSet=p.sets[slot],.dstBinding=k,.descriptorCount=1,.descriptorType=k==0?vk::DescriptorType::eUniformBuffer:k<=12?vk::DescriptorType::eSampledImage:k<=19?vk::DescriptorType::eStorageImage:vk::DescriptorType::eSampler,.pImageInfo=k?&ii[k-1]:nullptr,.pBufferInfo=k?nullptr:&bi};
 device.updateDescriptorSets(ds,{});cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*pipeline);cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,*pipeline_layout,0,p.sets[slot],{});cmd.dispatch((c.Width+7)/8,(c.Height+7)/8,1);Barrier(cmd);
}
vk::ImageView NativeAsyncPreSrSky::Record(Scheduler& scheduler,unsigned index,const std::array<vk::Image,3>& source,bool capture,const BbNativeAsync::Frame& frame){
 if(index!=next_index||index>=packets.size()||capture!=(index==0))throw std::runtime_error("Native Sky actual consecutive lineage required");
 RecordSourceConversionOnly(scheduler,index,source,frame);
 return RecordEditHistory(scheduler,index,capture);
}
void NativeAsyncPreSrSky::RecordSourceConversionOnly(Scheduler& scheduler,unsigned index,const std::array<vk::Image,3>& source,const BbNativeAsync::Frame& frame){
 if(index!=next_conversion_index||index>=packets.size())throw std::runtime_error("Native Sky conversion packet order/duplicate rejected");
 auto cmd=scheduler.CommandBuffer();if(!initialized)InitImages(cmd);auto& p=packets[index];p.frame=frame;
 for(unsigned k=0;k<3;k++){p.source_views[k]=device.createImageViewUnique({.image=source[k],.viewType=vk::ImageViewType::e2D,.format=k==0?vk::Format::eR16G16B16A16Sfloat:k==1?vk::Format::eD32SfloatS8Uint:vk::Format::eR16G16Sfloat,.subresourceRange={k==1?vk::ImageAspectFlagBits::eDepth:vk::ImageAspectFlagBits::eColor,0,1,0,1}}).value;}
 std::array<vk::DescriptorImageInfo,6> ci{};std::array<vk::WriteDescriptorSet,6> cw{};
 for(unsigned k=0;k<6;k++){ci[k]={.sampler=k<3?*nearest:vk::Sampler{},.imageView=k<3?*p.source_views[k]:*p.converted[k-3].view,.imageLayout=vk::ImageLayout::eGeneral};cw[k]={.dstSet=p.conversion,.dstBinding=k,.descriptorCount=1,.descriptorType=k<3?vk::DescriptorType::eCombinedImageSampler:vk::DescriptorType::eStorageImage,.pImageInfo=&ci[k]};}
 Barrier(cmd);device.updateDescriptorSets(cw,{});cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*conversion_pipeline);cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,*conversion_pipeline_layout,0,p.conversion,{});cmd.dispatch(160,90,1);Barrier(cmd);
 next_conversion_index++;
}
vk::ImageView NativeAsyncPreSrSky::RecordEditHistory(Scheduler& scheduler,unsigned index,bool capture){
 if(index!=next_index||index>=next_conversion_index||index>=packets.size()||capture!=(index==0))throw std::runtime_error("Native Sky edit requires exact recorded packet order");
 auto cmd=scheduler.CommandBuffer();auto& p=packets[index];const auto& frame=p.frame;
 auto& prev=banks[index%2];auto& next=banks[(index+1)%2];auto& color=p.converted[0];auto& depth=p.converted[1];auto& mv=p.converted[2];
 // Controlled noop origin: edited source is exactly the same converted immutable SR.
 std::array<Image*,12> reads{&prev.edit,&prev.guide,&color,&depth,&mv,&color,&dummy,&dummy,&dummy,&dummy,&exposure,&dummy};
 auto c=Base(capture?2:1);c.HistoryValid=capture?0:1;c.JitterDeltaX=(frame.previous_viewport[0]-frame.viewport[0])/1280.f;c.JitterDeltaY=(frame.previous_viewport[1]-frame.viewport[1])/720.f;Pass(cmd,p,0,c,true,reads,{&next.edit,&next.guide,&p.output,&next.levels[0],&next.l1guide,&stats,&dummy});
 for(unsigned j=1;j<3;j++){auto d=Base(3);d.Width=next.levels[j].image.image_ci.extent.width;d.Height=next.levels[j].image.image_ci.extent.height;d.SourceWidth=next.levels[j-1].image.image_ci.extent.width;d.SourceHeight=next.levels[j-1].image.image_ci.extent.height;reads[5]=&next.levels[j-1];Pass(cmd,p,j,d,true,reads,{&next.levels[j],&dummy,&p.output,&dummy,&dummy,&stats,&dummy});}
 reads={&next.edit,&next.guide,&color,&depth,&mv,&next.levels[0],&next.l1guide,&next.levels[1],&next.levels[2],&dummy,&exposure,&dummy};Pass(cmd,p,3,Base(4),true,reads,{&p.output,&dummy,&dummy,&dummy,&dummy,&stats,&dummy});
 next_index++;return *p.output.view;
}

std::array<vk::Image,3> NativeAsyncPreSrSky::ConvertedOrigin()const noexcept {return {vk::Image(packets[0].converted[0].image),vk::Image(packets[0].converted[1].image),vk::Image(packets[0].converted[2].image)};}
vk::ImageView NativeAsyncPreSrSky::RecordSparseCatchup(Scheduler& s,unsigned current,vk::ImageView nr){
 if(!nr||current>=next_index||current>=packets.size()||replay_busy.exchange(true,std::memory_order_acq_rel))return {};
 // A fresh, independent descriptor/UBO set for every actual retained link. Earlier
 // local-noop/per-frame packets may STILL be in flight; never rewrite those sets.
 // The renderer validates complete identity/lineage before entering this API.
 auto cmd=s.CommandBuffer();Image edited;edited.borrowed=nr;
 for(unsigned index=0;index<=current;index++){
  auto& p=replay_packets[index];auto& src=packets[index];auto& prev=banks[index%2];auto& next=banks[(index+1)%2];
  auto& color=src.converted[0];auto& depth=src.converted[1];auto& mv=src.converted[2];
  std::array<Image*,12> reads{&prev.edit,&prev.guide,&color,&depth,&mv,&edited,&dummy,&dummy,&dummy,&dummy,&exposure,&dummy};
  auto c=Base(index?1:2);c.HistoryValid=index?1:0;c.JitterDeltaX=(src.frame.previous_viewport[0]-src.frame.viewport[0])/1280.f;c.JitterDeltaY=(src.frame.previous_viewport[1]-src.frame.viewport[1])/720.f;
  Barrier(cmd);Pass(cmd,p,0,c,true,reads,{&next.edit,&next.guide,&p.output,&next.levels[0],&next.l1guide,&stats,&dummy});
  for(unsigned j=1;j<3;j++){auto d=Base(3);d.Width=next.levels[j].image.image_ci.extent.width;d.Height=next.levels[j].image.image_ci.extent.height;d.SourceWidth=next.levels[j-1].image.image_ci.extent.width;d.SourceHeight=next.levels[j-1].image.image_ci.extent.height;reads[5]=&next.levels[j-1];Pass(cmd,p,j,d,true,reads,{&next.levels[j],&dummy,&p.output,&dummy,&dummy,&stats,&dummy});}
  // Only the final CURRENT link may grant a composite. Earlier replay output is
  // private scratch, never a displayed NR frame. Apply remains the frozen r2 pass.
  if(index==current){reads={&next.edit,&next.guide,&color,&depth,&mv,&next.levels[0],&next.l1guide,&next.levels[1],&next.levels[2],&dummy,&exposure,&dummy};Pass(cmd,p,3,Base(4),true,reads,{&p.output,&dummy,&dummy,&dummy,&dummy,&stats,&dummy});}
 }
 return *replay_packets[current].output.view;
}

void NativeAsyncPreSrSky::RecordAcquisitionConversion(Scheduler& scheduler,unsigned index,const std::array<vk::Image,3>& source,const BbNativeAsync::Frame& frame){
 if(index>=packets.size()||!BbNativeAsync::Finite(frame)||frame.source<=acquisition_sources[index])throw std::runtime_error("C conversion physical packet/source mismatch");
 auto cmd=scheduler.CommandBuffer();if(!initialized)InitImages(cmd);auto& p=packets[index];p.frame=frame;
 for(unsigned k=0;k<3;k++){p.source_views[k]=device.createImageViewUnique({.image=source[k],.viewType=vk::ImageViewType::e2D,.format=k==0?vk::Format::eR16G16B16A16Sfloat:k==1?vk::Format::eD32SfloatS8Uint:vk::Format::eR16G16Sfloat,.subresourceRange={k==1?vk::ImageAspectFlagBits::eDepth:vk::ImageAspectFlagBits::eColor,0,1,0,1}}).value;}
 std::array<vk::DescriptorImageInfo,6> ci{};std::array<vk::WriteDescriptorSet,6> cw{};
 for(unsigned k=0;k<6;k++){ci[k]={.sampler=k<3?*nearest:vk::Sampler{},.imageView=k<3?*p.source_views[k]:*p.converted[k-3].view,.imageLayout=vk::ImageLayout::eGeneral};cw[k]={.dstSet=p.conversion,.dstBinding=k,.descriptorCount=1,.descriptorType=k<3?vk::DescriptorType::eCombinedImageSampler:vk::DescriptorType::eStorageImage,.pImageInfo=&ci[k]};}
 Barrier(cmd);device.updateDescriptorSets(cw,{});cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*conversion_pipeline);cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,*conversion_pipeline_layout,0,p.conversion,{});cmd.dispatch(160,90,1);Barrier(cmd);
 acquisition_sources[index]=frame.source;
}
void NativeAsyncPreSrSky::AcquisitionStep(vk::CommandBuffer cmd,Packet& p,Packet& src,unsigned prev_index,unsigned next_index,bool capture,vk::ImageView NR,bool apply){
 auto& prev=banks[prev_index];auto& next=banks[next_index];Image edited;edited.borrowed=NR;
 auto& color=src.converted[0];auto& depth=src.converted[1];auto& mv=src.converted[2];
 const auto& f=src.frame;
 std::array<Image*,12> reads{&prev.edit,&prev.guide,&color,&depth,&mv,capture?&edited:&color,&dummy,&dummy,&dummy,&dummy,&exposure,&dummy};
 auto c=Base(capture?2:1);c.HistoryValid=capture?0:1;c.JitterDeltaX=(f.previous_viewport[0]-f.viewport[0])/1280.f;c.JitterDeltaY=(f.previous_viewport[1]-f.viewport[1])/720.f;
 Barrier(cmd);Pass(cmd,p,0,c,true,reads,{&next.edit,&next.guide,&p.output,&next.levels[0],&next.l1guide,&stats,&dummy});
 for(unsigned j=1;j<3;j++){auto d=Base(3);d.Width=next.levels[j].image.image_ci.extent.width;d.Height=next.levels[j].image.image_ci.extent.height;d.SourceWidth=next.levels[j-1].image.image_ci.extent.width;d.SourceHeight=next.levels[j-1].image.image_ci.extent.height;reads[5]=&next.levels[j-1];Pass(cmd,p,j,d,true,reads,{&next.levels[j],&dummy,&p.output,&dummy,&dummy,&stats,&dummy});}
 if(apply){reads={&next.edit,&next.guide,&color,&depth,&mv,&next.levels[0],&next.l1guide,&next.levels[1],&next.levels[2],&dummy,&exposure,&dummy};Pass(cmd,p,3,Base(4),true,reads,{&p.output,&dummy,&dummy,&dummy,&dummy,&stats,&dummy});}
}
vk::ImageView NativeAsyncPreSrSky::RecordAcquisitionAdvance(Scheduler& scheduler,unsigned slot){
 if(slot>=packets.size()||!acquisition_history)return {};
 auto& p=packets[slot];const auto& f=p.frame;
 if(acquisition_sources[slot]!=f.source||f.source!=acquisition_previous.source+1||f.previous!=acquisition_previous.source||f.sr_reset||f.epoch!=acquisition_previous.epoch||f.history!=acquisition_previous.history||f.params!=acquisition_previous.params)return {};
 const unsigned next=1-acquisition_bank;AcquisitionStep(scheduler.CommandBuffer(),p,p,acquisition_bank,next,false,{},true);
 acquisition_bank=next;acquisition_previous=f;return p.output.View();
}
vk::ImageView NativeAsyncPreSrSky::RecordAcquisitionCatchup(Scheduler& scheduler,std::span<const unsigned> suffix,vk::ImageView nr){
 if(!nr||suffix.empty()||suffix.size()>BbNativeAsync::NodeCount)return {};
 for(unsigned i=0;i<suffix.size();++i){const auto slot=suffix[i];if(slot>=packets.size()||acquisition_sources[slot]!=packets[slot].frame.source)return {};
  if(i){const auto& p=packets[suffix[i-1]].frame;const auto& f=packets[slot].frame;if(f.source!=p.source+1||f.previous!=p.source||f.sr_reset||f.epoch!=p.epoch||f.history!=p.history||f.params!=p.params)return {};}
 }
 if(replay_busy.exchange(true,std::memory_order_acq_rel))return {};
 auto cmd=scheduler.CommandBuffer();unsigned bank=0;
 for(unsigned i=0;i<suffix.size();++i){const unsigned slot=suffix[i],next=1-bank;
  AcquisitionStep(cmd,replay_packets[slot],packets[slot],bank,next,i==0,nr,i+1==suffix.size());bank=next;
 }
 acquisition_bank=bank;acquisition_previous=packets[suffix.back()].frame;acquisition_history=true;
 return replay_packets[suffix.back()].output.View();
}

uint64_t NativeAsyncPreSrSky::ImageBytes()const noexcept{uint64_t n=0;const auto add=[&](const Image& x){const auto& c=x.image.image_ci;n+=uint64_t(c.extent.width)*c.extent.height*(c.format==vk::Format::eR32G32B32A32Sfloat?16:c.format==vk::Format::eR32G32Sfloat?8:4);};for(const auto& b:banks){add(b.edit);add(b.guide);add(b.l1guide);for(const auto& l:b.levels)add(l);}for(const auto& p:packets){for(const auto& x:p.converted)add(x);add(p.output);}for(const auto& p:replay_packets)add(p.output);add(dummy);add(exposure);add(stats);return n;}
}
