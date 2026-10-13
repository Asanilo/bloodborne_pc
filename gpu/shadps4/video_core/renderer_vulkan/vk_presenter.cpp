// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "bbport_game_menu.h"
#include "bbport_overlay.h"
#include "bbport_timeline.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/p0_timing_math.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <vk_mem_alloc.h>

namespace Vulkan {

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

// bbport: screenshot capture and ImGui overlays removed; the port presents directly.

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, true}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    pp_pass.Create(device, swapchain.GetSurfaceFormat().format);
    BbOverlay::Init(instance, swapchain.GetSurfaceFormat().format, num_images);

}

Presenter::~Presenter() {

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            Breadcrumbs::ReportDeviceLost("waiting for a frame");
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    BbTimeline::Note(BbTimeline::PipeTask, 4, draw_scheduler.CurrentTick());
    // bbport: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    C1PresentAudit c1_audit{};
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
    }

    Frame* frame = GetRenderFrame();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    // bbport: recorded like draws, on the recording threads. CommandBuffer() would first wait
    // for them to record everything handed over before (~90 us a frame of the draw recording
    // thread) and then submit from this thread.
    draw_scheduler.EndRendering();

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    vk::ImageView image_view{};
    vk::Extent2D image_size{};
    if (upscaled) {
        const auto device = instance.GetDevice();
        image_view = Check(device.createImageView({
            .image = display.image,
            .viewType = vk::ImageViewType::e2D,
            .format = view_info.format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        draw_scheduler.DeferOperation([device, image_view] { device.destroyImageView(image_view); });
        image_size = vk::Extent2D{display.width, display.height};
        draw_scheduler.Record([image = display.image](vk::CommandBuffer cmdbuf) {
            const vk::ImageMemoryBarrier2 to_read{
                .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
        });
    } else {
        auto& image = texture_cache.GetImage(image_id);
        image_view = *image.FindView(view_info).image_view;
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        auto& upscaler = rasterizer->GetUpscaler();
        const u64 frame_counter_before = BbStats::frame_number.load(std::memory_order_relaxed);
        const u64 gpu_frame_at_prepare = BbStats::gpu_frames.load(std::memory_order_relaxed);
        const u64 prepare_tick = draw_scheduler.CurrentTick();
        const bool c1_selected = upscaler.TakeC1PresentAudit(
            cpu_address, frame_counter_before, gpu_frame_at_prepare, prepare_tick, c1_audit);
        // The 1:1 HDR Run route consumes the actual guest VideoOut buffer, not the scaled
        // display override. Bind this one-shot ledger to that real address and buffer.
        const bool pre_sr_automatic_readback = c1_audit.stage != "PRE_SR" || BbPreSr::AutomaticPresentReadback();
        if (c1_selected && c1_audit.stage == "PRE_SR")
            std::printf("NATIVE_PRE_SR_PRESENT_READBACK_POLICY session=%s source_game_frame=%llu job=%s automatic_present_readback=%u automatic_full_pixel_readback_bytes=%llu scope=PRE_SR_automatic_Frame_dump_only explicit_dump_trigger=UNCHANGED actual_Frame_ledger=UNCHANGED guard_timestamps=UNCHANGED\n",
                c1_audit.session.c_str(),(unsigned long long)c1_audit.source_game_frame,c1_audit.pre_sr_job_valid?std::to_string(c1_audit.nr_frame).c_str():"NOT_MEASURED",
                pre_sr_automatic_readback,(unsigned long long)(pre_sr_automatic_readback?uint64_t(image_size.width)*image_size.height*4:0));
        if (c1_selected && pre_sr_automatic_readback && ((c1_audit.stage != "C3" && c1_audit.stage != "C4" && c1_audit.stage != "P0" && c1_audit.stage != "NATIVE_ASYNC") || c1_audit.audit_full)) {
            runtime.Transit(&image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eTransfer,
                            vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            upscaler.DumpPresented(image.GetImage(), image_size.width, image_size.height,
                                   view_info.format, c1_audit);
        } else if (upscaler.PresentDumpDue()) {
            runtime.Transit(&image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eTransfer,
                            vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            upscaler.DumpPresented(image.GetImage(), image_size.width, image_size.height,
                                   view_info.format);
        }
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    const C1PresentAudit& frame_audit = upscaled ? display.c1_audit : c1_audit;
    frame->c1_source_game_frame = frame_audit.source_game_frame;
    frame->c1_merge_tick = frame_audit.merge_tick;
    frame->c1_guest_buffer_address = frame_audit.buffer_address;
    frame->c1_guest_prepare_gpu_frame = frame_audit.buffer_prepare_gpu_frame;
    frame->c1_guest_prepare_tick = frame_audit.buffer_prepare_tick;
    frame->c1_present_frame_number = 0;
    frame->audit_stage = frame_audit.stage;
    frame->audit_execution_mode = frame_audit.execution_mode;
    frame->audit_mode = frame_audit.mode;
    frame->audit_session = frame_audit.session;
    frame->audit_selected_output = frame_audit.selected_output;
    frame->pre_sr_selection_known = frame_audit.pre_sr_selection_known;
    frame->pre_sr_supported_SR = frame_audit.pre_sr_supported_SR;
    frame->pre_sr_job_valid = frame_audit.pre_sr_job_valid;
    frame->pre_sr_epoch = frame_audit.pre_sr_epoch;frame->pre_sr_history = frame_audit.pre_sr_history;
    frame->pre_sr_fallback_reason = frame_audit.pre_sr_fallback_reason;
    frame->p0_selection_known = true;
    frame->audit_nr_frame = frame_audit.nr_frame;
    frame->audit_full = frame_audit.audit_full;
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;

    // Numbered here, in command order; marked where the commands are recorded.
    const u32 stream = draw_scheduler.CrumbStream();
    const bool crumbs = Breadcrumbs::Enabled();
    const u32 fsr_crumb = crumbs ? Breadcrumbs::Note(stream, {.name = "presenter FSR"}) : 0;
    const u32 pp_crumb = crumbs ? Breadcrumbs::Note(stream, {.name = "presenter post process"}) : 0;
    draw_scheduler.Record([this, frame, image_view, image_size, frame_subresources, stream,
                           fsr_crumb, pp_crumb, fsr = fsr_settings,
                           pp = pp_settings](vk::CommandBuffer cmdbuf) {
        // Frames of different submissions may be recorded on two threads at once.
        std::scoped_lock lock{passes_mutex};
        const auto pre_barrier = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = frame->image,
            .subresourceRange{frame_subresources},
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &pre_barrier,
        });
        if (fsr_crumb) {
            Breadcrumbs::Mark(cmdbuf, stream, fsr_crumb, false);
        }
        const vk::ImageView input = fsr_pass.Render(cmdbuf, image_view, image_size,
                                                    {frame->width, frame->height}, fsr,
                                                    frame->is_hdr);
        if (fsr_crumb) {
            Breadcrumbs::Mark(cmdbuf, stream, fsr_crumb, true);
            Breadcrumbs::Mark(cmdbuf, stream, pp_crumb, false);
        }
        pp_pass.Render(cmdbuf, input, image_size, *frame, pp);
        if (pp_crumb) {
            Breadcrumbs::Mark(cmdbuf, stream, pp_crumb, true);
        }
    });

    // Flush frame creation commands.
    frame->c1_present_frame_number =
        BbStats::frame_number.fetch_add(1, std::memory_order_relaxed) + 1; // game frames shown
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);
    if (frame->c1_source_game_frame != 0 && frame->audit_stage == "P0") {
        std::printf("P0_PRESENT_PREPARED stage=P0 execution_mode=%s mode=%s session=%s frame=%u selected_output=%u source_game_frame=%llu merge_tick=%llu guest_buffer_address=0x%llx gpu_frame_at_prepare=%llu guest_prepare_tick=%llu ready_tick=%llu present_frame_number=%llu audit_kind=%s\n",
            frame->audit_execution_mode.c_str(), frame->audit_mode.c_str(), frame->audit_session.c_str(), frame->audit_nr_frame,
            frame->audit_selected_output, (unsigned long long)frame->c1_source_game_frame,
            (unsigned long long)frame->c1_merge_tick, (unsigned long long)frame->c1_guest_buffer_address,
            (unsigned long long)frame->c1_guest_prepare_gpu_frame,
            (unsigned long long)frame->c1_guest_prepare_tick,
            (unsigned long long)frame->ready_tick, (unsigned long long)frame->c1_present_frame_number,
            frame->audit_full ? "full" : "off");
    }
    if (frame->c1_source_game_frame != 0 && frame->audit_stage == "Q1") {
        std::printf("Q1_PRESENT_PREPARED stage=Q1 execution_mode=%s mode=%s session=%s frame=%u selected_output=%u source_game_frame=%llu merge_tick=%llu guest_buffer_address=0x%llx gpu_frame_at_prepare=%llu guest_prepare_tick=%llu ready_tick=%llu present_frame_number=%llu audit_kind=%s\n",
            frame->audit_execution_mode.c_str(), frame->audit_mode.c_str(), frame->audit_session.c_str(), frame->audit_nr_frame,
            frame->audit_selected_output, (unsigned long long)frame->c1_source_game_frame,
            (unsigned long long)frame->c1_merge_tick, (unsigned long long)frame->c1_guest_buffer_address,
            (unsigned long long)frame->c1_guest_prepare_gpu_frame,
            (unsigned long long)frame->c1_guest_prepare_tick,
            (unsigned long long)frame->ready_tick, (unsigned long long)frame->c1_present_frame_number,
            frame->audit_full ? "full" : "off");
    } else if (frame->c1_source_game_frame != 0 && frame->audit_stage == "C4") {
        std::printf("C4_PRESENT_PREPARED stage=C4 execution_mode=%s mode=%s session=%s frame=%u selected_output=%u source_game_frame=%llu merge_tick=%llu guest_buffer_address=0x%llx gpu_frame_at_prepare=%llu guest_prepare_tick=%llu ready_tick=%llu present_frame_number=%llu audit_kind=%s\n",
            frame->audit_execution_mode.c_str(), frame->audit_mode.c_str(), frame->audit_session.c_str(), frame->audit_nr_frame,
            frame->audit_selected_output, (unsigned long long)frame->c1_source_game_frame,
            (unsigned long long)frame->c1_merge_tick, (unsigned long long)frame->c1_guest_buffer_address,
            (unsigned long long)frame->c1_guest_prepare_gpu_frame,
            (unsigned long long)frame->c1_guest_prepare_tick,
            (unsigned long long)frame->ready_tick, (unsigned long long)frame->c1_present_frame_number,
            frame->audit_full ? "full" : "off");
    } else if (frame->c1_source_game_frame != 0 && frame->audit_stage == "C3") {
        std::printf("C3_PRESENT_PREPARED stage=C3 mode=%s session=%s frame=%u selected_output=%u source_game_frame=%llu merge_tick=%llu guest_buffer_address=0x%llx gpu_frame_at_prepare=%llu guest_prepare_tick=%llu ready_tick=%llu present_frame_number=%llu audit_kind=%s\n",
            frame->audit_mode.c_str(), frame->audit_session.c_str(), frame->audit_nr_frame,
            frame->audit_selected_output, (unsigned long long)frame->c1_source_game_frame,
            (unsigned long long)frame->c1_merge_tick, (unsigned long long)frame->c1_guest_buffer_address,
            (unsigned long long)frame->c1_guest_prepare_gpu_frame,
            (unsigned long long)frame->c1_guest_prepare_tick,
            (unsigned long long)frame->ready_tick, (unsigned long long)frame->c1_present_frame_number,
            frame->audit_full ? "full" : "gpu_full_scan");
    } else if (frame->c1_source_game_frame != 0 && frame->audit_stage == "C2") {
        std::printf("C2_PRESENT_PREPARED stage=C2 mode=%s session=%s selected_output=%u "
                    "route=nonupscaled_videoout source_game_frame=%llu merge_tick=%llu "
                    "guest_buffer_address=0x%llx gpu_frame_at_prepare=%llu "
                    "frame_counter_before=%llu present_frame_number=%llu guest_prepare_tick=%llu ready_tick=%llu\n",
                    frame->audit_mode.c_str(), frame->audit_session.c_str(), frame->audit_selected_output,
                    static_cast<unsigned long long>(frame->c1_source_game_frame),
                    static_cast<unsigned long long>(frame->c1_merge_tick),
                    static_cast<unsigned long long>(frame->c1_guest_buffer_address),
                    static_cast<unsigned long long>(frame->c1_guest_prepare_gpu_frame),
                    static_cast<unsigned long long>(c1_audit.present_frame_counter_before),
                    static_cast<unsigned long long>(frame->c1_present_frame_number),
                    static_cast<unsigned long long>(frame->c1_guest_prepare_tick),
                    static_cast<unsigned long long>(frame->ready_tick));
    } else if (frame->c1_source_game_frame != 0 && (frame->audit_stage!="PRE_SR"||frame->pre_sr_job_valid)) {
        std::printf("C1_PRESENT_PREPARED route=nonupscaled_videoout source_game_frame=%llu merge_tick=%llu "
                    "guest_buffer_address=0x%llx gpu_frame_at_prepare=%llu "
                    "frame_counter_before=%llu present_frame_number=%llu "
                    "guest_prepare_tick=%llu ready_tick=%llu\n",
                    static_cast<unsigned long long>(frame->c1_source_game_frame),
                    static_cast<unsigned long long>(frame->c1_merge_tick),
                    static_cast<unsigned long long>(frame->c1_guest_buffer_address),
                    static_cast<unsigned long long>(c1_audit.buffer_prepare_gpu_frame),
                    static_cast<unsigned long long>(c1_audit.present_frame_counter_before),
                    static_cast<unsigned long long>(frame->c1_present_frame_number),
                    static_cast<unsigned long long>(frame->c1_guest_prepare_tick),
                    static_cast<unsigned long long>(frame->ready_tick));
    }
    BbTimeline::Note(BbTimeline::PipeTask, 5, frame->ready_tick);

    // bbport: the GPU command thread runs at most BB_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    static const u32 frames_ahead = [] {
        const char* env = std::getenv("BB_FRAMES_AHEAD");
        return env ? u32(std::max(0, std::atoi(env))) : 1u;
    }();
    if (frames_ahead) {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                BbStats::WaitTimer timer{BbStats::present_wait_ns};
                draw_scheduler.Wait(tick);
            }
        }
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();
    frame->p0_selection_known = false; // stale audit values on pooled blank frames are not evidence
    frame->c1_source_game_frame = 0;
    frame->c1_merge_tick = 0;
    frame->c1_guest_buffer_address = 0;
    frame->c1_guest_prepare_gpu_frame = 0;
    frame->c1_guest_prepare_tick = 0;

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // bbport: nothing to present to while the window is minimised (a 0x0 surface, issue #31).
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        free_frame();
        return;
    }
    // Recreate the swapchain if the window was resized (or was minimised at the last try).
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight() ||
        !swapchain.IsPresentable()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.IsPresentable()) {
            free_frame();
            return;
        }
    }

    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            free_frame();
            return;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    // bbport: the game frame is blitted (letterboxed) straight into the swapchain image.
    const vk::Image swapchain_image = swapchain.Image();
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();
    DumpFinalFrameIfDue(instance, scheduler, cmdbuf, frame->image, frame->width, frame->height,
                        swapchain.GetSurfaceFormat().format);

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        const vk::Extent2D extent = swapchain.GetExtent();
        SetExpectedGameSize(s32(extent.width), s32(extent.height));
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                               {}, {}, pre_barriers);
        const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
        cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal, black, color_range);
        const vk::MemoryBarrier clear_done{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
        cmdbuf.blitImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height),
                         vk::Filter::eLinear);
        // bbport: the settings menu / FPS counter over the frame, at display resolution.
        BbGameMenu::Poll(); // the port's pages in the game's System menu
        const bool overlay = BbOverlay::Visible();
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                               vk::AccessFlagBits::eColorAttachmentWrite
                                         : vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                     : vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        if (overlay) {
            BbOverlay::Render(cmdbuf, swapchain.ImageView(), extent);
            const vk::ImageMemoryBarrier to_present{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eBottomOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.

    // bbport: the frame's submission goes out from a recording thread (BB_ASYNC_SUBMIT); this
    // one, waiting for it on the same queue, must not get there first.
    if (frame->ready_semaphore == draw_scheduler.GetWorkSemaphore()->Handle()) {
        draw_scheduler.WaitSubmitted(frame->ready_tick);
    }
    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    const u64 present_submit_tick = scheduler.CurrentTick();
    scheduler.Flush(info);

    // Present to swapchain.
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        Scheduler::Q0SubmitOwnerScope q0_owner{&scheduler, 6};
        q0_owner.Stage(15);
        const bool presented = swapchain.Present();
        q0_owner.Stage(16);
        const auto c4_now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const char* p0_mode=std::getenv("BB_NR_P0_MODE");
        if(is_game_frame && !is_reusing_frame && p0_mode && std::strcmp(p0_mode,"srOnly")==0)
            std::printf("P0_SR_PRESENT mode=srOnly actual_game_frame=1 reused=0 cpu_monotonic_ns=%lld present_frame_number=%llu swapchain_present=%u helper_started=0 IPC=0 scope=CPU_present_call_completed_not_input_to_photon\n",
                (long long)c4_now_ns,(unsigned long long)BbStats::frame_number.load(),presented);
        // Distinct base-game ledger for resident off/startup/fallback. This checks
        // actual Frame selection, not requested_enable or assumed Helper state.
        const bool selected_p0 = frame->audit_stage == "P0" && frame->audit_selected_output;
        if (P0Timing::BaseSrPresent(is_game_frame,is_reusing_frame,
                p0_mode && std::strcmp(p0_mode,"resident")==0,
                frame->p0_selection_known && frame->c1_present_frame_number!=0,selected_p0)) {
            const char* base_session=std::getenv("BB_NR_P0_SESSION");
            std::printf("P0_BASE_SR_PRESENT mode=resident base_session=%s actual_game_frame=1 reused=0 selected_P0_NR=0 selection_known=1 cpu_monotonic_ns=%lld present_frame_number=%llu ready_tick=%llu present_submit_tick=%llu swapchain_present=%u scope=CPU_present_call_completed_not_photon actual_selection_scope=prepared_Frame_no_P0_NR_selection_not_proof_of_SR_dispatch Helper_state=NOT_INFERRED IPC=NOT_INFERRED\n",
                base_session?base_session:"NOT_AVAILABLE",(long long)c4_now_ns,
                (unsigned long long)frame->c1_present_frame_number,(unsigned long long)frame->ready_tick,
                (unsigned long long)present_submit_tick,presented);
        }
        if (is_game_frame && !is_reusing_frame && std::getenv("BB_NR_C4_BASELINE"))
            std::printf("C4_SR_PRESENT mode=srOnly actual_game_frame=1 reused=0 cpu_monotonic_ns=%lld present_frame_number=%llu swapchain_present=%u helper_started=0 IPC=0\n",
                (long long)c4_now_ns, (unsigned long long)BbStats::frame_number.load(), presented);
        if(is_game_frame&&!is_reusing_frame&&frame->audit_stage=="PRE_SR"&&frame->c1_source_game_frame&&frame->pre_sr_job_valid)
            std::printf("NATIVE_PRE_SR_CURRENT_PRESENT session=%s source_game_frame=%llu job=%u selected_NR720=%u present_frame_number=%llu merge_tick=%llu prepare_tick=%llu ready_tick=%llu swapchain_present=%u cpu_monotonic_ns=%lld route=original720_NR720_nativeSR1080 phase=UNVERIFIED\n",
                frame->audit_session.c_str(),(unsigned long long)frame->c1_source_game_frame,frame->audit_nr_frame,
                frame->audit_selected_output,(unsigned long long)frame->c1_present_frame_number,
                (unsigned long long)frame->c1_merge_tick,(unsigned long long)frame->c1_guest_prepare_tick,
                (unsigned long long)frame->ready_tick,presented,(long long)c4_now_ns);
        if(is_game_frame&&!is_reusing_frame&&BbPreSr::Enabled()){
            if(pre_sr_passive_present_count<BbPreSr::Hot120::MaxPassivePresents){
                ++pre_sr_passive_present_count;
                const bool known=frame->audit_stage=="PRE_SR"&&frame->pre_sr_selection_known&&frame->c1_source_game_frame!=0;
                const bool job_valid=frame->audit_stage=="PRE_SR"&&frame->pre_sr_job_valid;
                const char* session=std::getenv("BB_NR_NATIVE_PRE_SR_SESSION");
                std::printf("NATIVE_PRE_SR_FRAME_PRESENT session=%s source_game_frame=%s present_frame_number=%llu cpu_monotonic_ns=%lld swapchain_present=%u selection_known=%u supported_SR=%s selected_NR720=%s job_valid=%u job=%s fallback_reason=%s native_epoch=%s native_history=%s scope=actual_nonreuse_Frame_CPU_present_return_not_photon\n",
                    session?session:"NOT_MEASURED",known?std::to_string(frame->c1_source_game_frame).c_str():"NOT_MEASURED",
                    (unsigned long long)frame->c1_present_frame_number,(long long)c4_now_ns,presented,known,
                    known?(frame->pre_sr_supported_SR?"1":"0"):"NOT_MEASURED",known?(frame->audit_selected_output?"1":"0"):"NOT_MEASURED",
                    job_valid,job_valid?std::to_string(frame->audit_nr_frame).c_str():"NOT_MEASURED",
                    frame->pre_sr_fallback_reason.c_str(),job_valid?std::to_string(frame->pre_sr_epoch).c_str():"NOT_MEASURED",
                    job_valid?std::to_string(frame->pre_sr_history).c_str():"NOT_MEASURED");
            }else if(pre_sr_passive_present_count==BbPreSr::Hot120::MaxPassivePresents){
                ++pre_sr_passive_present_count;std::fprintf(stderr,"NATIVE_PRE_SR_FRAME_PRESENT_LIMIT max=30000 coverage=NOT_ALL_OBSERVED\n");
            }
        }
        if(is_game_frame&&!is_reusing_frame&&frame->audit_stage=="NATIVE_ASYNC"&&frame->c1_source_game_frame)
            std::printf("NATIVE_ASYNC_CURRENT_PRESENT session=%s snapshot_scope=%s source_game_frame=%llu present_frame_number=%llu merge_tick=%llu prepare_tick=%llu ready_tick=%llu swapchain_present=%u cpu_monotonic_ns=%lld current_SR=1 cache_selected=%u effect_applied=%u sparse_job=%u source_edited=0 current_NR_inference_claim=0\n",
                frame->audit_session.c_str(),frame->audit_mode.c_str(),(unsigned long long)frame->c1_source_game_frame,
                (unsigned long long)frame->c1_present_frame_number,(unsigned long long)frame->c1_merge_tick,
                (unsigned long long)frame->c1_guest_prepare_tick,(unsigned long long)frame->ready_tick,presented,(long long)c4_now_ns,frame->audit_selected_output,frame->audit_mode=="sparse-NR-Sky-current",frame->audit_nr_frame);
        if (is_game_frame && !is_reusing_frame && frame->audit_stage == "C4" && frame->c1_source_game_frame)
            std::printf("C4_PRESENT_CPU_TIME session=%s frame=%u source_game_frame=%llu cpu_monotonic_ns=%lld selected_output=%u swapchain_present=%u scope=CPU_present_call_completed_not_input_to_photon\n",
                frame->audit_session.c_str(), frame->audit_nr_frame, (unsigned long long)frame->c1_source_game_frame,
                (long long)c4_now_ns, frame->audit_selected_output, presented);
        if (frame->c1_source_game_frame != 0 && is_game_frame && !is_reusing_frame &&
            frame->audit_stage == "P0") {
            std::printf("P0_PRESENT_SUBMITTED stage=P0 execution_mode=%s mode=%s session=%s frame=%u selected_output=%u source_game_frame=%llu merge_tick=%llu guest_buffer_address=0x%llx guest_prepare_tick=%llu ready_tick=%llu present_frame_number=%llu present_submit_tick=%llu swapchain_present=%u audit_kind=%s cpu_monotonic_ns=%lld scope=CPU_present_call_completed_not_input_to_photon\n",
                frame->audit_execution_mode.c_str(), frame->audit_mode.c_str(), frame->audit_session.c_str(), frame->audit_nr_frame,
                frame->audit_selected_output, (unsigned long long)frame->c1_source_game_frame,
                (unsigned long long)frame->c1_merge_tick, (unsigned long long)frame->c1_guest_buffer_address,
                (unsigned long long)frame->c1_guest_prepare_tick, (unsigned long long)frame->ready_tick,
                (unsigned long long)frame->c1_present_frame_number, (unsigned long long)present_submit_tick,
                presented, frame->audit_full ? "full" : "off", (long long)c4_now_ns);
        }
        if (frame->c1_source_game_frame != 0 && is_game_frame && !is_reusing_frame &&
            frame->audit_stage == "Q1") {
            std::printf("Q1_PRESENT_SUBMITTED stage=Q1 execution_mode=%s mode=%s session=%s frame=%u selected_output=%u source_game_frame=%llu merge_tick=%llu guest_buffer_address=0x%llx guest_prepare_tick=%llu ready_tick=%llu present_frame_number=%llu present_submit_tick=%llu swapchain_present=%u audit_kind=%s\n",
                frame->audit_execution_mode.c_str(), frame->audit_mode.c_str(), frame->audit_session.c_str(), frame->audit_nr_frame,
                frame->audit_selected_output, (unsigned long long)frame->c1_source_game_frame,
                (unsigned long long)frame->c1_merge_tick, (unsigned long long)frame->c1_guest_buffer_address,
                (unsigned long long)frame->c1_guest_prepare_tick, (unsigned long long)frame->ready_tick,
                (unsigned long long)frame->c1_present_frame_number, (unsigned long long)present_submit_tick,
                presented, frame->audit_full ? "full" : "off");
        } else if (frame->c1_source_game_frame != 0 && is_game_frame && !is_reusing_frame &&
            frame->audit_stage == "C4") {
            std::printf("C4_PRESENT_SUBMITTED stage=C4 execution_mode=%s mode=%s session=%s frame=%u selected_output=%u source_game_frame=%llu merge_tick=%llu guest_buffer_address=0x%llx guest_prepare_tick=%llu ready_tick=%llu present_frame_number=%llu present_submit_tick=%llu swapchain_present=%u audit_kind=%s\n",
                frame->audit_execution_mode.c_str(), frame->audit_mode.c_str(), frame->audit_session.c_str(), frame->audit_nr_frame,
                frame->audit_selected_output, (unsigned long long)frame->c1_source_game_frame,
                (unsigned long long)frame->c1_merge_tick, (unsigned long long)frame->c1_guest_buffer_address,
                (unsigned long long)frame->c1_guest_prepare_tick, (unsigned long long)frame->ready_tick,
                (unsigned long long)frame->c1_present_frame_number, (unsigned long long)present_submit_tick,
                presented, frame->audit_full ? "full" : "off");
        } else if (frame->c1_source_game_frame != 0 && is_game_frame && !is_reusing_frame &&
            frame->audit_stage == "C3") {
            std::printf("C3_PRESENT_SUBMITTED stage=C3 mode=%s session=%s frame=%u selected_output=%u source_game_frame=%llu merge_tick=%llu guest_buffer_address=0x%llx guest_prepare_tick=%llu ready_tick=%llu present_frame_number=%llu present_submit_tick=%llu swapchain_present=%u audit_kind=%s\n",
                frame->audit_mode.c_str(), frame->audit_session.c_str(), frame->audit_nr_frame,
                frame->audit_selected_output, (unsigned long long)frame->c1_source_game_frame,
                (unsigned long long)frame->c1_merge_tick, (unsigned long long)frame->c1_guest_buffer_address,
                (unsigned long long)frame->c1_guest_prepare_tick, (unsigned long long)frame->ready_tick,
                (unsigned long long)frame->c1_present_frame_number, (unsigned long long)present_submit_tick,
                presented, frame->audit_full ? "full" : "gpu_full_scan");
        } else if (frame->c1_source_game_frame != 0 && is_game_frame && frame->audit_stage == "C2") {
            std::printf("C2_PRESENT_SUBMITTED stage=C2 mode=%s session=%s selected_output=%u "
                        "route=nonupscaled_videoout source_game_frame=%llu merge_tick=%llu "
                        "guest_buffer_address=0x%llx guest_prepare_gpu_frame=%llu guest_prepare_tick=%llu "
                        "present_frame_number=%llu frame_ready_tick=%llu present_submit_tick=%llu "
                        "game_frame=%u swapchain_present=%u\n",
                        frame->audit_mode.c_str(), frame->audit_session.c_str(), frame->audit_selected_output,
                        static_cast<unsigned long long>(frame->c1_source_game_frame),
                        static_cast<unsigned long long>(frame->c1_merge_tick),
                        static_cast<unsigned long long>(frame->c1_guest_buffer_address),
                        static_cast<unsigned long long>(frame->c1_guest_prepare_gpu_frame),
                        static_cast<unsigned long long>(frame->c1_guest_prepare_tick),
                        static_cast<unsigned long long>(frame->c1_present_frame_number),
                        static_cast<unsigned long long>(frame->ready_tick),
                        static_cast<unsigned long long>(present_submit_tick), is_game_frame, presented);
        } else if (frame->c1_source_game_frame != 0 && is_game_frame && frame->audit_stage != "NATIVE_ASYNC" && (frame->audit_stage!="PRE_SR"||frame->pre_sr_job_valid)) {
            std::printf("C1_PRESENT_SUBMITTED route=nonupscaled_videoout source_game_frame=%llu merge_tick=%llu "
                        "guest_buffer_address=0x%llx guest_prepare_gpu_frame=%llu "
                        "guest_prepare_tick=%llu present_frame_number=%llu "
                        "frame_ready_tick=%llu present_submit_tick=%llu "
                        "game_frame=%u swapchain_present=%u\n",
                        static_cast<unsigned long long>(frame->c1_source_game_frame),
                        static_cast<unsigned long long>(frame->c1_merge_tick),
                        static_cast<unsigned long long>(frame->c1_guest_buffer_address),
                        static_cast<unsigned long long>(frame->c1_guest_prepare_gpu_frame),
                        static_cast<unsigned long long>(frame->c1_guest_prepare_tick),
                        static_cast<unsigned long long>(frame->c1_present_frame_number),
                        static_cast<unsigned long long>(frame->ready_tick),
                        static_cast<unsigned long long>(present_submit_tick), is_game_frame,
                        presented);
        }
        if (!presented) {
            q0_owner.Stage(17);
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }

    free_frame();
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
    }
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        if (result == vk::Result::eErrorDeviceLost) {
            Breadcrumbs::ReportDeviceLost("waiting for a frame");
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return; // minimised: keep the last size (a 0x0 frame image cannot be made, issue #31)
    }
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
