// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_pre_sr_origin_gpu.h"
#include "vk_scheduler.h"
#include "native_pre_sr_hot120_policy.h"
#include <stdexcept>
namespace Vulkan {
bool NativePreSrResources::Valid(const NativeOriginIdentity& id)const noexcept {
 if(!BbPreSr::Hot120::IdentityValid(id)||!actual_import_fd_retired||!import_owner||session!=id.session||resource_set!=id.resource_set||native_queue_family==VK_QUEUE_FAMILY_IGNORED)return false;
 for(unsigned k=0;k<4;k++){
  const auto& c=infos[k];
  if(!images[k]||!views[k]||c.format!=(k==1?vk::Format::eR32Sfloat:k==2?vk::Format::eR32G32Sfloat:vk::Format::eR32G32B32A32Sfloat)||
   c.extent.width!=(1280u)||c.extent.height!=(720u)||c.extent.depth!=1||c.samples!=vk::SampleCountFlagBits::e1||c.mipLevels!=1||c.arrayLayers!=1||c.flags||
   c.tiling!=vk::ImageTiling::eOptimal||c.sharingMode!=vk::SharingMode::eExclusive||c.imageType!=vk::ImageType::e2D||
   c.usage!=(vk::ImageUsageFlagBits::eTransferSrc|vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eStorage))return false;
  for(unsigned j=0;j<k;j++)if(images[k]==images[j])return false;
 }
 return true;
}
static vk::ImageMemoryBarrier2 Ownership(vk::Image image,uint32_t local,bool acquire,bool input){
 return {.srcStageMask=acquire?vk::PipelineStageFlagBits2::eNone:input?vk::PipelineStageFlagBits2::eTransfer:vk::PipelineStageFlagBits2::eComputeShader,
 .srcAccessMask=acquire?vk::AccessFlagBits2::eNone:input?vk::AccessFlagBits2::eTransferWrite:vk::AccessFlagBits2::eShaderRead,
 .dstStageMask=acquire?(input?vk::PipelineStageFlagBits2::eTransfer:vk::PipelineStageFlagBits2::eComputeShader):vk::PipelineStageFlagBits2::eNone,
 .dstAccessMask=acquire?(input?vk::AccessFlagBits2::eTransferWrite:vk::AccessFlagBits2::eShaderRead):vk::AccessFlagBits2::eNone,
 .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
 .srcQueueFamilyIndex=acquire?VK_QUEUE_FAMILY_EXTERNAL:local,.dstQueueFamilyIndex=acquire?local:VK_QUEUE_FAMILY_EXTERNAL,
 .image=image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
}
static void PublishTarget(Scheduler& s,const NativePreSrResources& r,const std::shared_ptr<NativeOriginGpuProof>& proof){
 if(!proof||proof->native_target_tick||proof->native_GPU_complete.load())throw std::runtime_error("Native origin GPU proof must be new");
 proof->native_target_tick=s.CurrentTick();auto hold=r.import_owner;
 s.DeferPriorityOperation([proof,hold]{proof->native_GPU_complete.store(true,std::memory_order_release);});
}
void RecordPreSrInput(Scheduler& s,const NativePreSrResources& r,const NativeOriginIdentity& id,const std::array<vk::Image,3>& src,const std::shared_ptr<NativeOriginGpuProof>& proof){
 if(!proof||proof->native_target_tick||!r.Valid(id))throw std::runtime_error("Native origin input requires audited imported slot/fresh proof");
 auto cmd=s.CommandBuffer();
 for(unsigned k=0;k<3;k++){
  if(!src[k])throw std::runtime_error("Native origin source conversion absent");
  auto acquire=Ownership(r.images[k],r.native_queue_family,true,true);cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&acquire});
  const vk::ImageMemoryBarrier2 ready{.srcStageMask=vk::PipelineStageFlagBits2::eComputeShader,.srcAccessMask=vk::AccessFlagBits2::eShaderWrite,.dstStageMask=vk::PipelineStageFlagBits2::eTransfer,.dstAccessMask=vk::AccessFlagBits2::eTransferRead,.oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=src[k],.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
  cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&ready});
  const vk::ImageCopy cp{.srcSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.dstSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.extent=r.infos[k].extent};
  cmd.copyImage(src[k],vk::ImageLayout::eGeneral,r.images[k],vk::ImageLayout::eGeneral,cp);
  auto release=Ownership(r.images[k],r.native_queue_family,false,true);cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&release});
  auto restore=ready;restore.srcStageMask=vk::PipelineStageFlagBits2::eTransfer;restore.srcAccessMask=vk::AccessFlagBits2::eTransferRead;restore.dstStageMask=vk::PipelineStageFlagBits2::eComputeShader;restore.dstAccessMask=vk::AccessFlagBits2::eShaderRead;cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&restore});
 }
 PublishTarget(s,r,proof);
}
void AcquirePreSrOutput(Scheduler& s,const NativePreSrResources& r,const NativeOriginIdentity& id){if(!r.Valid(id))throw std::runtime_error("Native origin unaudited output acquire");auto b=Ownership(r.images[3],r.native_queue_family,true,false);s.CommandBuffer().pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b});}
void ReleasePreSrOutput(Scheduler& s,const NativePreSrResources& r,const NativeOriginIdentity& id,const std::shared_ptr<NativeOriginGpuProof>& proof){if(!r.Valid(id)||!proof||proof->native_target_tick)throw std::runtime_error("Native origin invalid output release proof");auto b=Ownership(r.images[3],r.native_queue_family,false,false);s.CommandBuffer().pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b});PublishTarget(s,r,proof);}
}
