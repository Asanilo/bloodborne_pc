// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/ui_composition.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

#include <vk_mem_alloc.h>

#include "bbport_settings.h"
#include "bbport_toggles.h"
#include "ffx_vk_portable.h"
#include "video_core/host_shaders/upscale_merge_comp.h"
#include "video_core/host_shaders/native_sky_merge_comp.h"
#include "video_core/host_shaders/upscale_reactive_comp.h"
#include "video_core/host_shaders/taa_comp.h"
#include "video_core/host_shaders/taa_sharpen_comp.h"
#include "video_core/host_shaders/taa_sharpen_ldr_comp.h"
#include "video_core/host_shaders/fsr4_decode_comp.h"
#include "video_core/host_shaders/fsr4_encode_comp.h"
#include "video_core/host_shaders/fsr4_reactive_comp.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

FfxVkPortableImage Describe(vk::Image image, vk::Format format, u32 width, u32 height,
                            vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect,
                            FfxVkPortableResourceState state) {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    out.image = image;
    out.format = static_cast<VkFormat>(format);
    out.extent = {width, height};
    out.mipCount = 1;
    out.arrayLayers = 1;
    out.usage = static_cast<VkImageUsageFlags>(usage);
    out.aspect = static_cast<VkImageAspectFlags>(aspect);
    out.state = state;
    return out;
}

/// bbport: BB_DUMP_TRIGGER=<file> BB_DUMP_DIR=<dir> (default out/dump): creating the file dumps
/// the upscaler's images of the next BB_DUMP_FRAMES (8) frames as raw files
/// <dir>/fNNN_<name>_<w>x<h>_<format>.raw, for checking temporal stability offline.
/// Returns the frame number to dump, or -1.
bool Q0CaptureEnabled();

int DumpFrame() {
    static const char* trigger = std::getenv("BB_DUMP_TRIGGER");
    static int remaining = 0, index = 0, polls = 0;
    if (!trigger) {
        return -1;
    }
    if (remaining == 0) {
        if (++polls % 30 != 0 || std::remove(trigger) != 0) {
            return -1;
        }
        const char* frames = std::getenv("BB_DUMP_FRAMES");
        remaining = frames ? std::max(1, std::atoi(frames)) : 8;
        if (!Q0CaptureEnabled()) index = 0; // Q0 retains unique indices across trigger sequences.
    }
    --remaining;
    return index++;
}

bool C0CaptureEnabled();

struct DumpImage {
    vk::Image image; ///< in layout General
    u32 width, height, bytes_per_pixel;
    const char* name;
    const char* format;
    vk::ImageAspectFlagBits aspect = vk::ImageAspectFlagBits::eColor;
};

/// Copies `images` into host buffers after the commands recorded so far and writes them to
/// files once the GPU is done.
void DumpImages(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                int frame, std::initializer_list<DumpImage> images,
                std::optional<C1PresentAudit> c1_present_audit = std::nullopt,
                u64 q0_source_game_frame = 0) {
    static const std::string dir = [] {
        const char* env = std::getenv("BB_DUMP_DIR");
        return std::string{env && env[0] ? env : "out/dump"};
    }();
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    for (const auto& image : images) {
        const VkDeviceSize size = VkDeviceSize(image.width) * image.height * image.bytes_per_pixel;
        const VkBufferCreateInfo buffer_ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        const VmaAllocationCreateInfo alloc_ci{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        };
        VkBuffer buffer{};
        VmaAllocation allocation{};
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                            &info) != VK_SUCCESS) {
            std::printf("Dump: no host memory for %s\n", image.name);
            continue;
        }
        const vk::BufferImageCopy region{
            .imageSubresource = {image.aspect, 0, 0, 1},
            .imageExtent = {image.width, image.height, 1},
        };
        cmdbuf.copyImageToBuffer(image.image, vk::ImageLayout::eGeneral, buffer, region);
        char path[512];
        std::snprintf(path, sizeof(path), "%s/f%03d_%s_%ux%u_%s.raw", dir.c_str(), frame,
                      image.name, image.width, image.height, image.format);
        const bool c0_capture = C0CaptureEnabled();
        const bool q0_capture = Q0CaptureEnabled();
        const auto* completion = scheduler.GetWorkSemaphore();
        const bool c1_present = c1_present_audit.has_value() && image.name == std::string_view{"present"};
        const u64 recorded_tick = scheduler.CurrentTick();
        scheduler.DeferPriorityOperation(
            [allocator = instance.GetAllocator(), buffer, allocation, info, size, frame,
             file = std::string{path}, image_name = std::string{image.name}, c0_capture,
             recorded_tick, c1_present, c1_audit = c1_present_audit.value_or(C1PresentAudit{}),
             q0_capture, q0_source_game_frame, completion] {
                vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
                size_t written = 0;
                bool file_ok = false;
                if (FILE* f = std::fopen(file.c_str(), "wb")) {
                    written = std::fwrite(info.pMappedData, 1, size, f);
                    file_ok = std::fclose(f) == 0 && written == size;
                }
                if (c0_capture) {
                    std::printf(
                        "C0_DUMP_COMPLETE dump_frame=%d name=%s recorded_tick=%llu bytes=%llu "
                        "written=%llu file_ok=%u path=%s\n",
                        frame, image_name.c_str(), static_cast<unsigned long long>(recorded_tick),
                        static_cast<unsigned long long>(size),
                        static_cast<unsigned long long>(written), file_ok, file.c_str());
                }
                if (q0_capture) {
                    std::printf("Q0_DUMP_COMPLETE dump_frame=%d source_game_frame=%llu name=%s "
                                "recorded_tick=%llu completed_tick=%llu GPU_complete=%u "
                                "bytes=%llu written=%llu file_ok=%u path=%s\n",
                        frame, (unsigned long long)q0_source_game_frame, image_name.c_str(),
                        (unsigned long long)recorded_tick,
                        (unsigned long long)completion->KnownGpuTick(), completion->IsFree(recorded_tick),
                        (unsigned long long)size, (unsigned long long)written, file_ok, file.c_str());
                }
                if (c1_present) {
                    std::printf(
                        "%s_PRESENT_DUMP_COMPLETE stage=%s mode=%s session=%s selected_output=%u "
                        "source_game_frame=%llu merge_tick=%llu guest_buffer_address=0x%llx buffer_prepare_gpu_frame=%llu "
                        "buffer_prepare_tick=%llu present_counter_before=%llu dump_frame=%d "
                        "recorded_tick=%llu bytes=%llu written=%llu file_ok=%u path=%s\n",
                        c1_audit.stage.c_str(), c1_audit.stage.c_str(), c1_audit.mode.c_str(),
                        c1_audit.session.c_str(), c1_audit.selected_output,
                        static_cast<unsigned long long>(c1_audit.source_game_frame),
                        static_cast<unsigned long long>(c1_audit.merge_tick),
                        static_cast<unsigned long long>(c1_audit.buffer_address),
                        static_cast<unsigned long long>(c1_audit.buffer_prepare_gpu_frame),
                        static_cast<unsigned long long>(c1_audit.buffer_prepare_tick),
                        static_cast<unsigned long long>(c1_audit.present_frame_counter_before),
                        frame,
                        static_cast<unsigned long long>(recorded_tick),
                        static_cast<unsigned long long>(size),
                        static_cast<unsigned long long>(written), file_ok, file.c_str());
                }
                vmaDestroyBuffer(allocator, buffer, allocation);
            });
    }
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eNone,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    std::printf("Dump: frame %d -> %s\n", frame, dir.c_str());
}

bool C0CaptureEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_NR_C0_CAPTURE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

bool Q0CaptureEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_NR_Q0_CAPTURE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

void PrintQ0Image(int dump, u64 source_frame, const char* role, vk::Image image,
                  vk::ImageView view, const vk::ImageCreateInfo& ci, vk::Format view_format,
                  const char* aspect, u32 valid_width, u32 valid_height) {
    std::printf("Q0_IMAGE dump_frame=%d source_game_frame=%llu role=%s image=%p view=%p "
                "create_format=%s view_format=%s extent=%ux%u depth=%u usage=0x%x flags=0x%x "
                "mips=%u layers=%u samples=%u tiling=%s aspect=%s layout=GENERAL "
                "valid_rect=0,0,%u,%u transfer_src=%u\n",
                dump, (unsigned long long)source_frame, role, (void*)VkImage(image),
                (void*)VkImageView(view), vk::to_string(ci.format).c_str(),
                vk::to_string(view_format).c_str(), ci.extent.width, ci.extent.height,
                ci.extent.depth, unsigned(VkImageUsageFlags(ci.usage)),
                unsigned(VkImageCreateFlags(ci.flags)), ci.mipLevels, ci.arrayLayers,
                unsigned(ci.samples), vk::to_string(ci.tiling).c_str(), aspect,
                valid_width, valid_height, bool(ci.usage & vk::ImageUsageFlagBits::eTransferSrc));
}

void PrintC0Image(const char* name, const vk::ImageCreateInfo& ci, vk::Format view_format,
                  const char* aspect) {
    std::printf(
        "C0_IMAGE name=%s create_format=%s view_format=%s extent=%ux%u depth=%u tiling=%s "
        "usage=0x%x flags=0x%x mip_levels=%u array_layers=%u samples=%u aspect=%s "
        "layout=GENERAL\n",
        name, vk::to_string(ci.format).c_str(), vk::to_string(view_format).c_str(),
        ci.extent.width, ci.extent.height, ci.extent.depth, vk::to_string(ci.tiling).c_str(),
        static_cast<unsigned>(static_cast<VkImageUsageFlags>(ci.usage)),
        static_cast<unsigned>(static_cast<VkImageCreateFlags>(ci.flags)), ci.mipLevels,
        ci.arrayLayers, static_cast<unsigned>(ci.samples), aspect);
}

template <size_t N>
std::string HexBytes(const std::array<uint8_t, N>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(N * 2);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0xf]);
    }
    return result;
}

void PrintIssues(const char* what, u64 issues) {
    std::printf("Upscaler: %s invalid:", what);
    for (u32 bit = 0; bit < 64; ++bit) {
        if (issues & (1ull << bit)) {
            std::printf(" %s", ffxVkPortableValidationIssueName(1ull << bit));
        }
    }
    std::printf("\n");
}

} // namespace

void DumpFinalFrameIfDue(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                         vk::Image image, u32 width, u32 height, vk::Format format) {
    static const char* trigger = std::getenv("BB_FINAL_DUMP_TRIGGER");
    static int index = 0;
    if (!trigger || std::remove(trigger) != 0) {
        return;
    }
    const bool bgra = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
    DumpImages(instance, scheduler, cmdbuf, index++,
               {{image, width, height, 4, "final", bgra ? "bgra" : "rgba"}});
}

TemporalUpscaler::TemporalUpscaler(const Instance& instance_, Scheduler& scheduler_,
                                   VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                                   CameraMotion& camera_motion_, SceneTargets& scene_targets_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, camera_motion{camera_motion_}, scene_targets{scene_targets_} {
    // Reject unsupported shaders before allocating resources or recording a frame.
    BbSettings::ConfigureUpscalerSupport(instance.IsFsr4Int8Supported(),
                                         instance.IsFsr411Supported());
    fsr4 = std::make_unique<Fsr4Upscaler>(instance, scheduler);
    {
        const Dlss* dlss = Dlss::Get();
        static std::string problem;
        problem = !dlss ? "the DLSS bridge and NVIDIA's DLSS library are not installed"
                        : dlss->Problem();
        BbSettings::ConfigureDlssSupport(dlss && dlss->Available(), problem.c_str());
    }
    // Available unless BB_UPSCALER=none; on/off and the parameters are the menu's settings.
    const char* env = std::getenv("BB_UPSCALER");
    enabled = !(env && std::strcmp(env, "none") == 0);
    if (const char* hash = std::getenv("BB_UPSCALE_BEFORE_CS")) {
        trigger_hash = std::strtoull(hash, nullptr, 16);
    }
    if (const char* hash = std::getenv("BB_UI_TRIGGER_VS")) {
        ui_trigger_vs = std::strtoull(hash, nullptr, 16);
    }
    if (const char* res = std::getenv("BB_OUTPUT_RES")) {
        u32 w = 0, h = 0;
        if (std::sscanf(res, "%ux%u", &w, &h) == 2 && w && h) {
            target_width = w;
            target_height = h;
        }
    }
    // Explicit resolution overrides retain the old compatibility path. Normal presets
    // keep guest allocations/UI native and change only host raster targets at frame boundaries.
    scaled_session = std::getenv("BB_RENDER_RES") && std::getenv("BB_RENDER_RES")[0];
    render_width = 1920;
    render_height = 1080;
    if (const char* res = std::getenv("BB_RENDER_RES")) {
        u32 w = 0, h = 0;
        if (std::sscanf(res, "%ux%u", &w, &h) == 2 && w && h) {
            render_width = w;
            render_height = h;
        }
    }
    // Scene depth copied into the output-size UI depth by a blit (depth aspect).
    const auto features = instance.GetPhysicalDevice()
                              .getFormatProperties(vk::Format::eD32SfloatS8Uint)
                              .optimalTilingFeatures;
    depth_blit = (features & vk::FormatFeatureFlagBits::eBlitSrc) &&
                 (features & vk::FormatFeatureFlagBits::eBlitDst);
    if (enabled && !instance.IsStorageImageWriteWithoutFormatEnabled()) {
        std::printf("Upscaler: shaderStorageImageWriteWithoutFormat unsupported, FSR 3 off\n");
        enabled = false;
    }
    if (enabled) {
        std::printf("Upscaler: FSR 3.1 available (%s) on scene color before compute shader "
                    "%016llx\n",
                    BbSettings::Get().upscaler != BbSettings::UpscalerOff ? "on" : "off",
                    static_cast<unsigned long long>(trigger_hash));
    }
}

TemporalUpscaler::~TemporalUpscaler() {
    native_pre_sr.Shutdown(scheduler);
    native_async_snapshot.RetireByTick(scheduler);
    p0_controller.Shutdown(instance, scheduler);
    if (resources_ready) scheduler.Finish();
    if (context) {
        scheduler.Finish();
        ffxVkPortableUpscaleContextDestroy(context);
    }
}

bool TemporalUpscaler::Active() const {
    // Toggle 1 << 24 switches it off at run time (A/B); history restarts after.
    return enabled && !failed &&
           (BbSettings::Get().upscaler == BbSettings::UpscalerFsr3 ||
            BbSettings::IsFsr4(BbSettings::Get().upscaler) ||
            BbSettings::Get().upscaler == BbSettings::UpscalerTaa ||
            BbSettings::Get().upscaler == BbSettings::UpscalerDlss) &&
           !BbToggle::Disabled(1u << 24);
}

bool TemporalUpscaler::ReactiveOn() const {
    // FSR 4 takes no reactive mask itself: the mask blends its output afterwards
    // (RecordFsr4Reactive).
    // DLSS takes no reactive mask.
    return BbSettings::Get().reactive && !BbToggle::Disabled(1u << 27) && !UseDlss() &&
           BbSettings::Get().upscaler != BbSettings::UpscalerTaa;
}

void TemporalUpscaler::RecordFsr4Reactive(vk::CommandBuffer cmdbuf, vk::ImageView color, u32 w,
                                          u32 h, u32 ow, u32 oh) {
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const vk::MemoryBarrier2 before{
        .srcStageMask = all, .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    const vk::DescriptorImageInfo current{.sampler = *fsr4_linear_sampler, .imageView = color,
                                          .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo mask{.sampler = *fsr4_linear_sampler, .imageView = *reactive_view,
                                       .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo out{.imageView = *output_view,
                                      .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 3> writes{{
        {.dstBinding = 0, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &current},
        {.dstBinding = 1, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &mask},
        {.dstBinding = 2, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &out},
    }};
    const std::array<float, 5> push{float(w), float(h), jitter[0], jitter[1], 1.0f};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *fsr4_reactive_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *fsr4_reactive_pipeline_layout, 0,
                                writes);
    cmdbuf.pushConstants(*fsr4_reactive_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(push), push.data());
    cmdbuf.dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = all,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
}

void TemporalUpscaler::OnSceneColor(VideoCore::ImageId color) {
    scene_color = color;
}

namespace {
float Halton(u32 index, u32 base) {
    float f = 1.0f, result = 0.0f;
    for (u32 i = index; i > 0; i /= base) {
        f /= float(base);
        result += f * float(i % base);
    }
    return result;
}
} // namespace

bool TemporalUpscaler::ReducedScene(const VideoCore::Image& color) const {
    return !scaled_session && scene_targets.Reduced() && scene_targets.EligibleScene(color);
}

bool TemporalUpscaler::RasterScaling() const {
    return !scaled_session && scene_targets.Reduced() && !done_this_frame;
}

bool TemporalUpscaler::OnFrameStart() {
    const auto& p0_settings=BbSettings::Get();
    native_async_snapshot.PollReady(instance,scheduler,!Scaled()&&UseDlss()&&p0_settings.preset==1&&p0_settings.jitter);
    native_pre_sr.Poll(instance,scheduler,!Scaled()&&UseDlss()&&p0_settings.preset==1&&p0_settings.jitter);
    p0_controller.Tick(instance,scheduler,!Scaled()&&UseDlss()&&p0_settings.preset==1&&p0_settings.jitter,BbOverlay::MenuOpen());
    // bbport: BB_PRESET_FILE=<file> holding a preset number, read about once a second: switches
    // the preset like the menu does (scripted tests of live preset changes).
    static const char* preset_file = std::getenv("BB_PRESET_FILE");
    if (preset_file && ++preset_file_frames % 64 == 0) {
        if (FILE* f = std::fopen(preset_file, "r")) {
            int value = 0;
            int output = -1;
            int provider = -1;
            const int fields = std::fscanf(f, "%d %d %d", &value, &output, &provider);
            if (fields >= 1 && value >= 0 &&
                value < BbSettings::PresetCount) {
                BbSettings::Get().preset.store(value);
            }
            if (fields >= 2 && output >= 0 && output < BbSettings::OutputCount) {
                BbSettings::Get().output_res.store(output);
            }
            if (fields == 3 && provider >= 0 && provider < BbSettings::UpscalerCount) {
                BbSettings::Get().upscaler.store(provider);
            }
            std::fclose(f);
        }
    }
    const auto& settings = BbSettings::Get();
    // The guest's startup resolution patch remains in effect until restart. Keep the FSR
    // model at the applied preset while the menu saves the requested one for run.sh.
    const int preset = BbSettings::RenderPreset();
    if (applied_preset != preset || settings.upscaler == BbSettings::UpscalerOff) failed = false;
    if (dispatch_failed.exchange(false, std::memory_order_relaxed)) {
        failed = true;
    }
    const bool active = Active();
    const bool jitter_on = active && settings.jitter && !BbToggle::Disabled(1u << 25);
    const int upscaler = settings.upscaler.load();
    const int output = settings.output_res.load();
    const bool output_changed = !scaled_session && applied_output != output;
    if (output_changed) {
        target_width = BbSettings::OutputWidths[output];
        target_height = BbSettings::OutputHeights[output];
        std::printf("Output resolution: %ux%u (live)\n", target_width, target_height);
        failed = false;
        fsr4_failed = false;
    }
    const bool changed = output_changed || applied_preset != preset || active != last_active ||
                         jitter_on != last_jitter || applied_upscaler != upscaler;
    if (applied_upscaler != upscaler) {
        // A failed provider keeps a fatal flag internally; a user retry gets a fresh context.
        scheduler.Finish();
        fsr4 = std::make_unique<Fsr4Upscaler>(instance, scheduler);
        if (BbSettings::IsFsr4(upscaler)) BbSettings::Get().fsr4_problem = nullptr;
    }
    if (applied_upscaler != upscaler) fsr4_failed = false; // retry after a menu change
    // Dynamic scene resolution scaling (live preset switching) works on all GPUs.
    // On GPUs without D32S8 blit support, UI depth is cleared instead of copied from scene.
    if (!scaled_session) {
        scene_targets.SetSize(SceneResolution::ForPreset(active ? preset : 0,
                                                        {target_width, target_height}));
        render_width = scene_targets.Size().width;
        render_height = scene_targets.Size().height;
    }
    BbSettings::Get().active_render_width = Scaled() ? render_width : scene_targets.Size().width;
    BbSettings::Get().active_render_height = Scaled() ? render_height : scene_targets.Size().height;
    if (changed || !dispatched_last_frame) reset = true;
    if (changed) jitter_index = 0;
    applied_preset = preset;
    applied_output = output;
    applied_upscaler = upscaler;
    last_active = active;
    last_jitter = jitter_on;
    dispatched_last_frame = false;

    // The display pass of an upscaled frame reads the upscaled UI image.
    display_redirect = ui_phase;
    ui_phase = false;
    ui_read_barrier = false;
    done_this_frame = false;
    ldr_target = {};
    scene_color = {};
    snapshot_taken = false;
    opaque_valid = false;
    mask_ready = false;
    // Halton(2, 3); the menu or toggle 1 << 25 disables it.
    if (!jitter_on) {
        jitter = {};
        return changed;
    }
    // CameraMotion::OnDisplayPass has already cleared Depth(). Use the context's render
    // size, which survives the frame boundary (Performance 960 -> 1920 needs 32 phases).
    const u32 phases = Scaled() ? Motion::JitterPhases(render_width, target_width)
        : Motion::JitterPhases(scene_targets.Size().width, 1920);
    jitter_index = jitter_index % phases + 1;
    jitter = {Halton(jitter_index, 2) - 0.5f, Halton(jitter_index, 3) - 0.5f};
    return changed;
}

void TemporalUpscaler::OnDispatch(u64 cs_hash) {
    if (cs_hash != trigger_hash || done_this_frame || failed || Scaled()) {
        return;
    }
    done_this_frame = true;
    if (!scene_color || !camera_motion.Ready() || !Active()) {
        reset = true;
        return;
    }
    Run();
}

bool TemporalUpscaler::EnsureResources(u32 w, u32 h, u32 ow, u32 oh, bool hdr) {
    // DLSS, like FSR 4, has its own context: no portable FSR 3 context is made.
    const bool use_fsr4 = UseFsr4() || UseDlss();
    const bool use_taa = BbSettings::Get().upscaler == BbSettings::UpscalerTaa;
    if (use_taa && (w != ow || h != oh)) {
        std::printf("TAA: remove BB_RENDER_RES to use native-resolution TAA\n");
        return false;
    }
    if (resources_ready && w == width && h == height && ow == out_width && oh == out_height &&
        hdr == context_hdr && use_fsr4 == resources_fsr4 && use_taa == resources_taa) {
        return true;
    }
    const auto device = instance.GetDevice();
    // FSR 4 owns no portable FSR 3 context, but its images can still be in flight.
    scheduler.Finish();
    resources_ready = false;
    if (context) {
        ffxVkPortableUpscaleContextDestroy(context);
        context = nullptr;
    }
    width = w;
    height = h;
    out_width = ow;
    out_height = oh;
    context_hdr = hdr;

    FfxVkPortableDeviceInfo device_info{};
    device_info.structSize = sizeof(device_info);
    device_info.instance = instance.GetInstance();
    device_info.physicalDevice = instance.GetPhysicalDevice();
    device_info.device = device;
    device_info.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    device_info.queue = instance.GetGraphicsQueue();
    device_info.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex();
    device_info.shaderFloat16Enabled = instance.IsShaderFloat16Enabled();
    device_info.subgroupSizeControlEnabled = instance.IsSubgroupSizeControlEnabled();
    device_info.synchronization2Enabled = VK_TRUE;
    device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;

    FfxVkPortableUpscaleCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    // Native AA runs on the HDR scene color; scaled presets on the game's tonemapped frame.
    create_info.flags = hdr ? FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT |
                                  FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE
                            : 0;
    create_info.maxRenderSize = {w, h};
    create_info.maxOutputSize = {ow, oh};
    // FSR 4 has its own model context (vk_fsr4); the images below are shared.
    if (!use_fsr4 && !use_taa) {
        if (const u64 issues = ffxVkPortableValidateUpscaleCreateInfo(&create_info)) {
            PrintIssues("create info", issues);
            return false;
        }
        if (ffxVkPortableUpscaleContextCreate(&device_info, &create_info, &context) !=
            FFX_VK_PORTABLE_OK) {
            std::printf("Upscaler: FSR 3 context creation failed\n");
            context = nullptr;
            return false;
        }
    }

    const auto allocator = instance.GetAllocator();
    motion_view.reset();
    output_view.reset();
    motion_image = VideoCore::UniqueImage(device, allocator);
    auto motion_usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
    if (C0CaptureEnabled() || Q0CaptureEnabled() || Q1DiagnosticEnabled() || P0Enabled() || BbNativeAsync::OptIn() || BbPreSr::Enabled()) {
        motion_usage |= vk::ImageUsageFlagBits::eTransferSrc;
    }
    motion_image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = motion_usage,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    output_image = VideoCore::UniqueImage(device, allocator);
    output_image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {ow, oh, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    output_view = Check(device.createImageViewUnique({
        .image = vk::Image(output_image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    motion_view = Check(device.createImageViewUnique({
        .image = vk::Image(motion_image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    const auto make_image = [&](VideoCore::UniqueImage& image, vk::UniqueImageView& view,
                                vk::Format format, vk::ImageUsageFlags usage, u32 iw = 0,
                                u32 ih = 0, vk::ImageAspectFlags aspect =
                                                vk::ImageAspectFlagBits::eColor,
                                vk::ImageCreateFlags flags = {}) {
        view.reset();
        image = VideoCore::UniqueImage(device, allocator);
        image.Create(vk::ImageCreateInfo{
            .flags = flags,
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {iw ? iw : w, ih ? ih : h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        view = Check(device.createImageViewUnique({
            .image = vk::Image(image),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        }));
    };
    make_image(opaque_image, opaque_view, vk::Format::eR16G16B16A16Sfloat,
               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst |
                   vk::ImageUsageFlagBits::eTransferSrc);
    make_image(reactive_image, reactive_view, vk::Format::eR8Unorm,
               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled);
    fsr4_linear_view.reset();
    fsr4_linear_image = VideoCore::UniqueImage{};
    if (use_fsr4) {
        make_image(fsr4_linear_image, fsr4_linear_view, vk::Format::eR16G16B16A16Sfloat,
                   vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled);
    }
    for (u32 i = 0; i < taa_history.size(); ++i) {
        taa_history_views[i].reset();
        taa_history[i] = VideoCore::UniqueImage{};
        if (use_taa) {
            make_image(taa_history[i], taa_history_views[i], vk::Format::eR32G32B32A32Sfloat,
                       vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                           vk::ImageUsageFlagBits::eTransferSrc,
                       ow, oh);
        }
    }
    taa_next = 0;
    CreatePipelines();
    opaque_valid = false;
    reset = true;
    resources_ready = true;
    resources_fsr4 = use_fsr4;
    resources_taa = use_taa;
    if (use_taa) {
        std::printf("TAA: context %ux%u -> %ux%u (%s), no FSR model\n", w, h, ow, oh,
                    hdr ? "HDR scene color" : "tonemapped frame");
        return true;
    }
    if (use_fsr4) {
        std::printf("Upscaler: FSR 4 inputs %ux%u -> %ux%u\n", w, h, ow, oh);
        return true;
    }
    FfxVkPortableMemoryUsage usage{};
    usage.structSize = sizeof(usage);
    ffxVkPortableUpscaleContextGetMemoryUsage(context, &usage);
    std::printf("Upscaler: FSR 3 context %ux%u -> %ux%u (%s), %.1f MB\n", w, h, ow, oh,
                hdr ? "HDR scene color" : "tonemapped frame", usage.totalUsageInBytes / 1e6);
    return true;
}

void TemporalUpscaler::CreatePipelines() {
    if (merge_pipeline) {
        return;
    }
    const auto device = instance.GetDevice();
    const auto storage_layout = [&](u32 count, vk::UniqueDescriptorSetLayout& layout,
                                    vk::UniquePipelineLayout& pipeline_layout, u32 push_size) {
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
        for (u32 i = 0; i < count; ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = vk::DescriptorType::eStorageImage,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = count,
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                         .offset = 0,
                                         .size = push_size};
        pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        }));
    };
    const auto compute = [&](const auto& code, vk::PipelineLayout layout) {
        const auto module = CompileSPV(code, device);
        auto pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = layout,
                }));
        device.destroyShaderModule(module);
        return pipeline;
    };
    {
        // Merge: three storage images, then the motion and object motion images (debug).
        std::array<vk::DescriptorSetLayoutBinding, 5> bindings{};
        for (u32 i = 0; i < bindings.size(); ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = i < 3 ? vk::DescriptorType::eStorageImage
                                                   : vk::DescriptorType::eSampledImage,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        merge_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                         .offset = 0,
                                         .size = sizeof(u32)};
        merge_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*merge_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        }));
    }
    merge_pipeline = compute(UPSCALE_MERGE_COMP, *merge_pipeline_layout);
    if (BbNativeAsync::OptIn()) native_sky_merge_pipeline = compute(NATIVE_SKY_MERGE_COMP, *merge_pipeline_layout);
    storage_layout(3, reactive_desc_layout, reactive_pipeline_layout, 3 * sizeof(float));
    reactive_pipeline = compute(UPSCALE_REACTIVE_COMP, *reactive_pipeline_layout);
    storage_layout(2, taa_sharpen_desc_layout, taa_sharpen_pipeline_layout, sizeof(float));
    taa_sharpen_pipeline = compute(TAA_SHARPEN_COMP, *taa_sharpen_pipeline_layout);
    taa_sharpen_ldr_pipeline = compute(TAA_SHARPEN_LDR_COMP, *taa_sharpen_pipeline_layout);
    storage_layout(1, fsr4_encode_desc_layout, fsr4_encode_pipeline_layout, sizeof(u32));
    fsr4_encode_pipeline = compute(FSR4_ENCODE_COMP, *fsr4_encode_pipeline_layout);
    {
        const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
            {.binding = 0, .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 1, .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        }};
        fsr4_decode_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(u32)};
        fsr4_decode_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1, .pSetLayouts = &*fsr4_decode_desc_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
        fsr4_decode_pipeline = compute(FSR4_DECODE_COMP, *fsr4_decode_pipeline_layout);
    }
    {
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
        for (u32 i = 0; i < bindings.size(); ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = i < 2 ? vk::DescriptorType::eCombinedImageSampler
                                                   : vk::DescriptorType::eStorageImage,
                           .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        fsr4_reactive_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 5 * sizeof(float)};
        fsr4_reactive_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1, .pSetLayouts = &*fsr4_reactive_desc_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
        fsr4_linear_sampler = Check(device.createSamplerUnique({
            .magFilter = vk::Filter::eLinear, .minFilter = vk::Filter::eLinear,
            .mipmapMode = vk::SamplerMipmapMode::eNearest,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge}));
        fsr4_reactive_pipeline = compute(FSR4_REACTIVE_COMP, *fsr4_reactive_pipeline_layout);
    }
    {
        std::array<vk::DescriptorSetLayoutBinding, 7> bindings{};
        for (u32 i = 0; i < bindings.size(); ++i) {
            bindings[i] = {.binding = i,
                .descriptorType = i < 4 ? vk::DescriptorType::eCombinedImageSampler
                                        : vk::DescriptorType::eStorageImage,
                .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        taa_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 64};
        taa_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1, .pSetLayouts = &*taa_desc_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
        taa_sampler = Check(device.createSamplerUnique({
            .magFilter = vk::Filter::eNearest, .minFilter = vk::Filter::eNearest,
            .mipmapMode = vk::SamplerMipmapMode::eNearest,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge}));
        taa_pipeline = compute(TAA_COMP, *taa_pipeline_layout);
    }
}

void TemporalUpscaler::RecordTaa(vk::CommandBuffer cmdbuf, vk::ImageView color,
                                vk::ImageView depth) {
    // Make scene writes and the last storage-image history writes visible to sampled reads.
    const vk::MemoryBarrier2 inputs{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite |
                         vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead |
                         vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1,.pMemoryBarriers = &inputs});
    // Histories alternate between reads and writes. Queue ordering plus this dependency
    // protects both the previous write and a reused destination's previous read.
    std::array<vk::ImageMemoryBarrier2, 2> barriers{};
    for (u32 i = 0; i < barriers.size(); ++i) {
        barriers[i] = {.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
                             vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead |
                             vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = reset ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(taa_history[i]),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(barriers.size()),
                             .pImageMemoryBarriers = barriers.data()});
    const std::array<vk::ImageView, 7> views{
        color, depth, *motion_view, *taa_history_views[1 - taa_next],
        *output_view, *taa_history_views[taa_next], *opaque_view};
    std::array<vk::DescriptorImageInfo, 7> infos{};
    std::array<vk::WriteDescriptorSet, 7> writes{};
    for (u32 i = 0; i < infos.size(); ++i) {
        infos[i] = {.sampler = i < 4 ? *taa_sampler : vk::Sampler{},
                    .imageView = views[i], .imageLayout = vk::ImageLayout::eGeneral};
        writes[i] = {.dstBinding = i, .descriptorCount = 1,
            .descriptorType = i < 4 ? vk::DescriptorType::eCombinedImageSampler
                                    : vk::DescriptorType::eStorageImage,
            .pImageInfo = &infos[i]};
    }
    struct Params {
        std::array<float, 2> jitter;
        u32 reset, pad;
        std::array<std::array<float, 4>, 3> depth;
    } params{jitter, reset ? 1u : 0u, std::getenv("BB_TAA_DIAGNOSTICS") ?
                 u32(std::clamp(std::atoi(std::getenv("BB_TAA_DIAGNOSTICS")),1,3)) : 0u,
             camera_motion.TaaDepthParameters()};
    // Optional techniques for A/B in one run (BB_TOGGLE_FILE bits 51-54): tonemapped blending,
    // YCoCg clipping, variance clipping, 3x3 reconstruction. Measured in game on 2026-10-02
    // (static and panning camera, interleaved captures): none improved stability beyond run
    // noise; YCoCg clipping and the reconstruction made it worse. Default: all off.
    params.pad |= (BbToggle::Disabled(BbToggle::TaaTonemapBlend) ? 1u << 8 : 0u) |
                  (BbToggle::Disabled(BbToggle::TaaClip) ? 2u << 8 : 0u) |
                  (BbToggle::Disabled(BbToggle::TaaVariance) ? 4u << 8 : 0u) |
                  (BbToggle::Disabled(BbToggle::TaaFilter) ? 8u << 8 : 0u) |
                  (BbToggle::Disabled(BbToggle::TaaKeepNearerHistory) ? 16u << 8 : 0u);
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *taa_pipeline);
    if (params.pad & 0xffu) {
        static u32 diagnostic_frames = 0;
        if (diagnostic_frames++ < 16)
            std::printf("TAA diagnostic: reset=%u jitter=%.6f,%.6f mode=%u\n",
                        params.reset, jitter[0], jitter[1], params.pad);
    }
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *taa_pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*taa_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(params), &params);
    cmdbuf.dispatch((out_width + 7) / 8, (out_height + 7) / 8, 1);
    const auto& settings = BbSettings::Get();
    const float strength = std::clamp(settings.sharpness.load(), 0.0f, 2.0f);
    if (settings.sharpen && strength > 0.0f && !(params.pad & 0xffu)) {
        const vk::MemoryBarrier2 resolved{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead |
                             vk::AccessFlagBits2::eShaderStorageWrite};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &resolved});
        const std::array<vk::DescriptorImageInfo, 2> sharpen_infos{{
            {.imageView = *taa_history_views[taa_next], .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = *output_view, .imageLayout = vk::ImageLayout::eGeneral}}};
        std::array<vk::WriteDescriptorSet, 2> sharpen_writes{};
        for (u32 i = 0; i < sharpen_writes.size(); ++i)
            sharpen_writes[i] = {.dstBinding = i, .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &sharpen_infos[i]};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *taa_sharpen_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
                                    *taa_sharpen_pipeline_layout, 0, sharpen_writes);
        cmdbuf.pushConstants(*taa_sharpen_pipeline_layout, vk::ShaderStageFlagBits::eCompute,
                             0, sizeof(strength), &strength);
        cmdbuf.dispatch((out_width + 7) / 8, (out_height + 7) / 8, 1);
    }
    taa_next = 1 - taa_next;
}

void TemporalUpscaler::ExtraSharpen(vk::Image target, bool ldr, u32 w, u32 h) {
    const auto& settings = BbSettings::Get();
    const float extra = std::clamp(settings.sharpness.load(), 0.0f, 2.0f) - 1.0f;
    if (!settings.sharpen || extra <= 0.0f) {
        return;
    }
    const auto device = instance.GetDevice();
    if (extra_sharpen_width != w || extra_sharpen_height != h) {
        // Submitted work may still read the old copy: destroyed once the GPU is past it. (A
        // Finish() here submitted the command buffer Run() goes on recording into.)
        scheduler.DeferOperation([image = std::move(extra_sharpen_image),
                                  view = std::move(extra_sharpen_view)]() mutable {
            view.reset();
            image.Destroy();
        });
        extra_sharpen_image = VideoCore::UniqueImage(device, instance.GetAllocator());
        extra_sharpen_image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR32G32B32A32Sfloat,
            .extent = {w, h, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        extra_sharpen_view = Check(device.createImageViewUnique({
            .image = vk::Image(extra_sharpen_image), .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR32G32B32A32Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        extra_sharpen_width = w;
        extra_sharpen_height = h;
    }
    vk::ImageView target_view = *output_view;
    if (ldr) {
        if (!ui_storage_view) {
            // The UI image's own format may be sRGB; FSR 3 writes it through a UNORM view too.
            ui_storage_view = Check(device.createImageViewUnique({
                .image = target, .viewType = vk::ImageViewType::e2D,
                .format = vk::Format::eR8G8B8A8Unorm,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            }));
        }
        target_view = *ui_storage_view;
    }
    // bbport: the commands are recorded through the scheduler (a recording thread for FSR 3).
    scheduler.RecordCrumb({.name = "TAA extra sharpen"}, [target, copy = vk::Image(extra_sharpen_image),
                      copy_view = *extra_sharpen_view, target_view, extra, w, h,
                      pipeline = ldr ? *taa_sharpen_ldr_pipeline : *taa_sharpen_pipeline,
                      layout = *taa_sharpen_pipeline_layout](vk::CommandBuffer cmdbuf) {
        const auto image_barrier = [&](vk::Image image, vk::ImageLayout old_layout,
                                       vk::PipelineStageFlags2 src, vk::AccessFlags2 src_access,
                                       vk::PipelineStageFlags2 dst, vk::AccessFlags2 dst_access) {
            const vk::ImageMemoryBarrier2 b{
                .srcStageMask = src, .srcAccessMask = src_access,
                .dstStageMask = dst, .dstAccessMask = dst_access,
                .oldLayout = old_layout, .newLayout = vk::ImageLayout::eGeneral,
                .image = image, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
        };
        constexpr auto all = vk::PipelineStageFlagBits2::eAllCommands;
        constexpr auto rw = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
        image_barrier(target, vk::ImageLayout::eGeneral, all, rw,
                      vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferRead);
        image_barrier(copy, vk::ImageLayout::eUndefined, all, rw,
                      vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite);
        const vk::ImageBlit region{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffsets = std::array{vk::Offset3D{}, vk::Offset3D{s32(w), s32(h), 1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{}, vk::Offset3D{s32(w), s32(h), 1}},
        };
        cmdbuf.blitImage(target, vk::ImageLayout::eGeneral, copy, vk::ImageLayout::eGeneral,
                         region, vk::Filter::eNearest);
        image_barrier(copy, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eBlit,
                      vk::AccessFlagBits2::eTransferWrite,
                      vk::PipelineStageFlagBits2::eComputeShader,
                      vk::AccessFlagBits2::eShaderStorageRead);
        image_barrier(target, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eBlit,
                      vk::AccessFlagBits2::eTransferRead,
                      vk::PipelineStageFlagBits2::eComputeShader,
                      vk::AccessFlagBits2::eShaderStorageWrite);
        const std::array<vk::DescriptorImageInfo, 2> infos{{
            {.imageView = copy_view, .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = target_view, .imageLayout = vk::ImageLayout::eGeneral}}};
        std::array<vk::WriteDescriptorSet, 2> writes{};
        for (u32 i = 0; i < writes.size(); ++i)
            writes[i] = {.dstBinding = i, .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &infos[i]};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
        cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(extra), &extra);
        cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
        image_barrier(target, vk::ImageLayout::eGeneral,
                      vk::PipelineStageFlagBits2::eComputeShader,
                      vk::AccessFlagBits2::eShaderStorageWrite, all, rw);
    });
}

void TemporalUpscaler::OnBlendedSceneDraw() {
    // The mask is opt-in (menu, BB_REACTIVE=1): on Bloodborne's thin mist it trades trails for
    // jitter shimmer, which looked worse. Toggle 1 << 27 switches it off.
    if (snapshot_taken || !scene_color || !Active() || !ReactiveOn()) {
        return;
    }
    snapshot_taken = true;
    auto& color = texture_cache.GetImage(scene_color);
    // With a live preset the scene is in the reduced proxy: the mask and the FSR context
    // must use that size, or each frame would recreate the context (native <-> reduced).
    const bool reduced = ReducedScene(color);
    const u32 ow = color.info.size.width, oh = color.info.size.height;
    // Scaled presets: the scene fills the top-left of its aligned targets (see RunScaled).
    const auto scene = Scaled() ? SceneSize(ow, oh) : std::array<u32, 2>{ow, oh};
    const u32 w = reduced ? scene_targets.Size().width : scene[0];
    const u32 h = reduced ? scene_targets.Size().height : scene[1];
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eTransferSrc)) {
        return;
    }
    if (!EnsureResources(w, h, Scaled() ? target_width : ow,
                         Scaled() ? target_height : oh, !Scaled())) {
        failed = true;
        return;
    }
    scheduler.EndRendering();
    vk::Image source = color.GetImage();
    vk::ImageLayout source_layout = vk::ImageLayout::eTransferSrcOptimal;
    if (reduced) {
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        source = scene_targets.Read(scene_color, ci, vk::PipelineStageFlagBits2::eTransfer,
                                    vk::AccessFlagBits2::eTransferRead).image;
        source_layout = vk::ImageLayout::eGeneral;
    } else {
        runtime.Transit(&color, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
    }
    const vk::Image opaque = vk::Image(opaque_image);
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {w, h, 1},
    };
    // On the recording thread: direct recording here would move the rest of the scene there.
    scheduler.Record([opaque, source, source_layout, region](vk::CommandBuffer cmdbuf) {
        const auto to_general = [&](vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                    vk::ImageLayout old_layout, vk::PipelineStageFlags2 dst_stage,
                                    vk::AccessFlags2 dst_access) {
            const vk::ImageMemoryBarrier2 barrier{
                .srcStageMask = src_stage,
                .srcAccessMask = src_access,
                .dstStageMask = dst_stage,
                .dstAccessMask = dst_access,
                .oldLayout = old_layout,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = opaque,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        };
        // The previous frame's mask pass read it: wait for that before overwriting.
        to_general(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eNone,
                   vk::ImageLayout::eUndefined, vk::PipelineStageFlagBits2::eTransfer,
                   vk::AccessFlagBits2::eTransferWrite);
        cmdbuf.copyImage(source, source_layout, opaque, vk::ImageLayout::eGeneral, region);
        to_general(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                   vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                   vk::AccessFlagBits2::eShaderRead);
    });
    opaque_valid = true;
}

void TemporalUpscaler::OnSceneComposite() {
    if (!opaque_valid || mask_ready || failed || !ReactiveOn()) {
        return;
    }
    auto& color = texture_cache.GetImage(scene_color);
    const bool reduced = ReducedScene(color);
    const auto scene = Scaled() ? SceneSize(color.info.size.width, color.info.size.height)
                                : std::array<u32, 2>{color.info.size.width, color.info.size.height};
    const u32 w = reduced ? scene_targets.Size().width : scene[0];
    const u32 h = reduced ? scene_targets.Size().height : scene[1];
    if (w != width || h != height || !(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        return;
    }
    scheduler.EndRendering();
    if (reduced) {
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        const auto proxy = scene_targets.Read(scene_color, ci);
        mask_ready = RecordReactive(proxy.view);
        return;
    }
    const auto device = instance.GetDevice();
    const auto color_view = Check(device.createImageView({
        .image = vk::Image(color.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    mask_ready = RecordReactive(color_view);
    scheduler.DeferOperation([device, color_view] { device.destroyImageView(color_view); });
}

bool TemporalUpscaler::RecordReactive(vk::ImageView color_view) {
    if (!opaque_valid || !ReactiveOn()) {
        return false;
    }
    const vk::Image reactive = vk::Image(reactive_image);
    const vk::ImageView opaque = *opaque_view, mask = *reactive_view;
    const vk::Pipeline pipeline = *reactive_pipeline;
    const vk::PipelineLayout layout = *reactive_pipeline_layout;
    const u32 w = width, h = height;
    // Relative color change times the scale (default 1), zero below the threshold (0.2), at
    // most the maximum (0.9, never fully reactive) — the defaults of AMD's mask generator.
    const auto& settings = BbSettings::Get();
    const std::array<float, 3> params{settings.reactive_scale, settings.reactive_max,
                                      settings.reactive_threshold};
    scheduler.RecordCrumb({.name = "TAA reactive mask"}, [=](vk::CommandBuffer cmdbuf) {
        const vk::ImageMemoryBarrier2 to_write{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = reactive,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_write});
        const vk::DescriptorImageInfo opaque_info{.imageView = opaque,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo scene_info{.imageView = color_view,
                                                 .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo mask_info{.imageView = mask,
                                                .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 3> writes = {{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &opaque_info},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &scene_info},
            {.dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &mask_info},
        }};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0,
                                    writes);
        cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(params), params.data());
        cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = reactive,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    });
    return true;
}

void TemporalUpscaler::Run() {
    previous_dlss_jitter_x = last_dlss_jitter_x;
    previous_dlss_jitter_y = last_dlss_jitter_y;
    last_dlss_called = false;
    last_dlss_hdr = false;
    last_dlss_reset = false;
    last_dlss_jitter_x = last_dlss_jitter_y = last_dlss_frame_ms = 0.0f;
    if (auto* profiler = GpuProfiler::Get()) {
        const char* label = BbSettings::Get().upscaler == BbSettings::UpscalerTaa
            ? "upscaler Run (TAA)" : "upscaler Run (FSR)";
        profiler->Mark(0xF5A0'0000ull ^ std::hash<std::string_view>{}(label),
                       [label] { return std::string{label}; });
    }
    auto& color = texture_cache.GetImage(scene_color);
    auto& depth = texture_cache.GetImage(camera_motion.Depth());
    const u32 ow = color.info.size.width, oh = color.info.size.height;
    const bool reduced = ReducedScene(color) && scene_targets.EligibleScene(depth);
    const u32 w = reduced ? scene_targets.Size().width : ow;
    const u32 h = reduced ? scene_targets.Size().height : oh;
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
        depth.info.size.width != ow || depth.info.size.height != oh ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        return;
    }
    if (!EnsureResources(w, h, ow, oh, true)) {
        failed = true;
        return;
    }
    const auto depth_format = depth.info.pixel_format;
    const auto depth_view =
        CachedView(depth, depth_format, vk::ImageAspectFlagBits::eDepth);
    const auto color_view =
        CachedView(color, vk::Format::eR16G16B16A16Sfloat, vk::ImageAspectFlagBits::eColor);

    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    vk::Image input_color = color.GetImage(), input_depth = depth.GetImage();
    vk::ImageView input_color_view = color_view, input_depth_view = depth_view;
    if (reduced) {
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        const auto c = scene_targets.Read(scene_color, ci);
        ci.format = depth.info.pixel_format;
        const auto d = scene_targets.Read(camera_motion.Depth(), ci);
        input_color = c.image;
        input_depth = d.image;
        input_color_view = c.view;
        input_depth_view = d.view;
    }
    auto cmdbuf = scheduler.CommandBuffer();
    std::optional<Breadcrumbs::Scope> crumb;
    crumb.emplace(cmdbuf, scheduler.CrumbStream(), "TAA");

    const auto own_barrier = [&](vk::Image image, vk::ImageLayout old_layout,
                                 vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                 vk::ImageLayout new_layout, vk::PipelineStageFlags2 dst_stage,
                                 vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    };
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    // Previous contents are not needed: the layouts start from undefined every frame.
    own_barrier(vk::Image(motion_image), vk::ImageLayout::eUndefined, all,
                vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
    own_barrier(vk::Image(output_image), vk::ImageLayout::eUndefined, all,
                vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral, all, rw);

    camera_motion.RecordMotion(input_depth_view, *motion_view, w, h);
    own_barrier(vk::Image(motion_image), vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);

    const bool has_reactive = mask_ready || RecordReactive(input_color_view);

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_frame).count();
    if (frame_ms <= 0.0f || frame_ms > 200.0f) {
        frame_ms = 16.6f;
    }
    last_frame = now;

    bool dispatched = false;
    if (BbSettings::Get().upscaler == BbSettings::UpscalerTaa) {
        RecordTaa(cmdbuf, input_color_view, input_depth_view);
        dispatched = true;
    } else if (UseFsr4()) {
        dispatched = RecordFsr4(cmdbuf, {input_color, input_color_view, w, h},
                                {input_depth, input_depth_view, w, h}, w, h, ow, oh, frame_ms);
        if (dispatched && has_reactive) {
            RecordFsr4Reactive(cmdbuf, input_color_view, w, h, ow, oh);
        }
    } else if (UseDlss()) {
        if(native_pre_sr.Enabled())crumb.reset();
        dispatched = RecordDlss(
            cmdbuf,
            {input_color, input_color_view, color.info.pixel_format, vk::ImageAspectFlagBits::eColor, w, h},
            {input_depth, input_depth_view, depth_format, vk::ImageAspectFlagBits::eDepth, w, h}, w, h,
            ow, oh, frame_ms, true);
        if(native_pre_sr.Enabled()){
            cmdbuf=scheduler.CommandBuffer();
            crumb.emplace(cmdbuf,scheduler.CrumbStream(),"TAA pre-SR NR then native SR");
        }
    } else {
        FfxVkPortableUpscaleDispatchInfo info{};
        info.structSize = sizeof(info);
        info.commandBuffer = cmdbuf;
        info.color = Describe(input_color, color.info.pixel_format, w, h,
                              color.usage_flags, vk::ImageAspectFlagBits::eColor,
                              FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.depth = Describe(input_depth, depth_format, w, h, depth.usage_flags,
                              vk::ImageAspectFlagBits::eDepth,
                              FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.motionVectors = Describe(vk::Image(motion_image), vk::Format::eR16G16Sfloat, w, h,
                                      vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                      vk::ImageAspectFlagBits::eColor,
                                      FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.output = Describe(vk::Image(output_image), vk::Format::eR16G16B16A16Sfloat, ow, oh,
                               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                                   vk::ImageUsageFlagBits::eTransferSrc,
                               vk::ImageAspectFlagBits::eColor,
                               FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
        // Optional inputs, absent: a described but null image.
        info.exposure.structSize = sizeof(info.exposure);
        info.reactiveMask.structSize = sizeof(info.reactiveMask);
        if (has_reactive) {
            info.reactiveMask = Describe(vk::Image(reactive_image), vk::Format::eR8Unorm, w, h,
                                         vk::ImageUsageFlagBits::eStorage |
                                             vk::ImageUsageFlagBits::eSampled,
                                         vk::ImageAspectFlagBits::eColor,
                                         FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        }
        info.transparencyAndCompositionMask.structSize = sizeof(info.transparencyAndCompositionMask);
        // Toggle 1 << 26 (tests): the opposite sign convention for FSR.
        const float sign = BbToggle::Disabled(1u << 26) ? -1.0f : 1.0f;
        info.jitterOffset = {sign * jitter[0], sign * jitter[1]};
        info.motionVectorScale = {1.0f, 1.0f};
        info.renderSize = {w, h};
        info.outputSize = {ow, oh};
        info.frameTimeMilliseconds = frame_ms;
        info.preExposure = 1.0f;
        info.cameraNear = camera_motion.Near();
        info.cameraFar = 3000.0f;
        info.cameraVerticalFovRadians = camera_motion.VerticalFov();
        info.viewSpaceToMeters = 1.0f;
        // RCAS strength 0..1 (menu, BB_FSR_SHARPNESS); jitter at 1:1 softens the image slightly.
        // AMD's RCAS ends at 1; ExtraSharpen adds the rest of the menu's 0..2.
        info.sharpness = std::min(BbSettings::Get().sharpness.load(), 1.0f);
        info.enableSharpening = BbSettings::Get().sharpen ? VK_TRUE : VK_FALSE;
        info.reset = reset ? VK_TRUE : VK_FALSE;
        info.frameId = frame_id++;
    
        FfxVkPortableUpscaleCreateInfo create_info{};
        create_info.structSize = sizeof(create_info);
        create_info.flags = FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT | FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;
        create_info.maxRenderSize = {w, h};
        create_info.maxOutputSize = {ow, oh};
        if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&create_info, &info)) {
            static bool printed = false;
            if (!printed) {
                printed = true;
                PrintIssues("dispatch", issues);
            }
            failed = true;
        } else if (ffxVkPortableUpscaleContextRecordDispatch(context, &info) !=
                   FFX_VK_PORTABLE_OK) {
            std::printf("Upscaler: FSR 3 dispatch failed\n");
            failed = true;
        } else {
            dispatched = true;
        }
    }
    if (dispatched) {
        const u64 source_game_frame =
            BbStats::gpu_frames.load(std::memory_order_relaxed);
        reset = false;
        dispatched_last_frame = true;
        if (BbSettings::Get().upscaler != BbSettings::UpscalerTaa) {
            ExtraSharpen(vk::Image(output_image), false, ow, oh);
        }
        // bbport: BB_DUMP_TRIGGER on this (HDR scene color) path too: the upscaler's inputs and
        // its result, for ghosting and history checks (tools/dump_view.py).
        if (const int dump = DumpFrame(); dump >= 0) {
            if (Q0CaptureEnabled()) {
                const auto proxy_color = scene_targets.CaptureCreateInfo(input_color);
                const auto proxy_depth = scene_targets.CaptureCreateInfo(input_depth);
                const auto vc = camera_motion.CurrentJitter(), vp = camera_motion.PreviousJitter();
                const auto& settings = BbSettings::Get();
                const bool profile_match = !Scaled() && reduced && w == 1280 && h == 720 &&
                    ow == 1920 && oh == 1080 && proxy_color && proxy_depth &&
                    settings.upscaler == BbSettings::UpscalerDlss && settings.jitter &&
                    last_dlss_called && last_dlss_hdr &&
                    proxy_color->extent.width == w && proxy_color->extent.height == h &&
                    proxy_depth->extent.width == w && proxy_depth->extent.height == h;
                const bool transfer_ok = proxy_color && proxy_depth &&
                    bool(proxy_color->usage & vk::ImageUsageFlagBits::eTransferSrc) &&
                    bool(proxy_depth->usage & vk::ImageUsageFlagBits::eTransferSrc) &&
                    bool(motion_image.image_ci.usage & vk::ImageUsageFlagBits::eTransferSrc) &&
                    bool(output_image.image_ci.usage & vk::ImageUsageFlagBits::eTransferSrc);
                const bool depth_ok = depth_format == vk::Format::eD32Sfloat ||
                                      depth_format == vk::Format::eD32SfloatS8Uint;
                std::printf("Q0_SR_CAPTURE dump_frame=%d source_game_frame=%llu route=Run "
                    "scaled=%u reduced=%u profile_match=%u status=%s render=%ux%u output=%ux%u "
                    "record_dlss=%u SR_success=%u hdr=%u source_reset=%u "
                    "viewport_jitter=%.9g,%.9g previous_viewport_jitter=%.9g,%.9g "
                    "sr_jitter=%.9g,%.9g previous_sr_jitter=%.9g,%.9g "
                    "frame_ms=%.9g recorded_tick=%llu transfer_sources_legal=%u "
                    "guidephase=UNVERIFIED_FOR_NR exposure_valid=0 exposure_source=unknown\n",
                    dump, (unsigned long long)source_game_frame, Scaled(), reduced, profile_match,
                    profile_match && transfer_ok && depth_ok ? "RECORDED_NOT_YET_COMPLETED" : "BLOCKED",
                    w, h, ow, oh, last_dlss_called, last_dlss_called && last_dlss_hdr,
                    last_dlss_hdr, last_dlss_reset,
                    vc[0], vc[1], vp[0], vp[1], last_dlss_jitter_x, last_dlss_jitter_y,
                    previous_dlss_jitter_x, previous_dlss_jitter_y, last_dlss_frame_ms,
                    (unsigned long long)scheduler.CurrentTick(), transfer_ok);
                PrintQ0Image(dump, source_game_frame, "guest_color_allocation", color.GetImage(),
                    color_view, color.backing->image.image_ci, color.info.pixel_format, "COLOR", ow, oh);
                PrintQ0Image(dump, source_game_frame, "guest_depth_allocation", depth.GetImage(),
                    depth_view, depth.backing->image.image_ci, depth_format, "DEPTH", ow, oh);
                if (proxy_color) PrintQ0Image(dump, source_game_frame, "actual_SR_input_proxy", input_color,
                    input_color_view, *proxy_color, color.info.pixel_format, "COLOR", w, h);
                if (proxy_depth) PrintQ0Image(dump, source_game_frame, "actual_SR_depth_proxy", input_depth,
                    input_depth_view, *proxy_depth, proxy_depth->format, "DEPTH", w, h);
                PrintQ0Image(dump, source_game_frame, "actual_MV", vk::Image(motion_image), *motion_view,
                    motion_image.image_ci, vk::Format::eR16G16Sfloat, "COLOR", w, h);
                PrintQ0Image(dump, source_game_frame, "actual_SR_output", vk::Image(output_image), *output_view,
                    output_image.image_ci, vk::Format::eR16G16B16A16Sfloat, "COLOR", ow, oh);
                camera_motion.PrintQualityCaptureState(dump, source_game_frame);
                if (transfer_ok && depth_ok) {
                    DumpImages(instance, scheduler, cmdbuf, dump,
                        {{input_color, w, h, 8, "input", "rgba16f"},
                         {input_depth, w, h, 4, "depth", "f32", vk::ImageAspectFlagBits::eDepth},
                         {vk::Image(motion_image), w, h, 4, "motion", "rg16f"},
                         {vk::Image(output_image), ow, oh, 8, "output", "rgba16f"}},
                        std::nullopt, source_game_frame);
                }
                // Object attachment is optional; do not copy an uninspected extra source.
                std::printf("Q0_OBJECT_DUMP dump_frame=%d source_game_frame=%llu status=NOT_DUMPED "
                            "reason=nonessential_attachment_usage_not_inspected\n", dump,
                            (unsigned long long)source_game_frame);
            } else {
            if (C0CaptureEnabled()) {
                const auto viewport_jitter = camera_motion.CurrentJitter();
                const auto previous_viewport_jitter = camera_motion.PreviousJitter();
                const auto viewport_size = camera_motion.RenderSize();
                const bool profile_match =
                    !Scaled() && !reduced && w == ow && h == oh && ow == 1920 && oh == 1080 &&
                    viewport_size[0] == w && viewport_size[1] == h && last_dlss_called &&
                    last_dlss_hdr;
                const u64 recorded_tick = scheduler.CurrentTick();
                std::printf(
                    "C0_SR_CAPTURE dump_frame=%d source_game_frame=%llu "
                    "source_frame_counter=BbStats::gpu_frames route=Run scaled=%u reduced=%u "
                    "record_dlss=%u hdr=%u profile_match=%u render=%ux%u output=%ux%u "
                    "viewport=%ux%u source_reset_valid=%u source_reset=%u "
                    "sr_jitter=%.9g,%.9g viewport_jitter=%.9g,%.9g "
                    "previous_viewport_jitter=%.9g,%.9g "
                    "guidephase=depth_jittered_motion_unjittered_phase_relation_unverified "
                    "exposure_valid=0 exposure_source=unknown frame_ms=%.9g "
                    "recorded_tick=%llu\n",
                    dump, static_cast<unsigned long long>(source_game_frame), Scaled(), reduced,
                    last_dlss_called, last_dlss_hdr, profile_match, w, h, ow, oh,
                    viewport_size[0], viewport_size[1], last_dlss_called, last_dlss_reset,
                    last_dlss_jitter_x, last_dlss_jitter_y, viewport_jitter[0],
                    viewport_jitter[1], previous_viewport_jitter[0],
                    previous_viewport_jitter[1], last_dlss_frame_ms,
                    static_cast<unsigned long long>(recorded_tick));
                PrintC0Image("color", color.backing->image.image_ci, color.info.pixel_format,
                             "COLOR");
                PrintC0Image("depth", depth.backing->image.image_ci, depth.info.pixel_format,
                             "DEPTH");
                PrintC0Image("motion", motion_image.image_ci, vk::Format::eR16G16Sfloat, "COLOR");
                PrintC0Image("sr_output", output_image.image_ci,
                             vk::Format::eR16G16B16A16Sfloat, "COLOR");
                std::printf(
                    "C0_DUMP dump_frame=%d source_game_frame=%llu route=Run "
                    "record_dlss=%u hdr=%u recorded_tick=%llu\n",
                    dump, static_cast<unsigned long long>(source_game_frame), last_dlss_called,
                    last_dlss_hdr, static_cast<unsigned long long>(scheduler.CurrentTick()));
            }
            camera_motion.PrintState(dump);
            const vk::Format format = color.info.pixel_format;
            const bool rgba16f = format == vk::Format::eR16G16B16A16Sfloat;
            const bool r11g11b10 = format == vk::Format::eB10G11R11UfloatPack32;
            if (rgba16f || r11g11b10) {
                DumpImages(instance, scheduler, cmdbuf, dump,
                           {{input_color, w, h, rgba16f ? 8u : 4u, "input",
                             rgba16f ? "rgba16f" : "r11g11b10f"}});
            }
            DumpImages(instance, scheduler, cmdbuf, dump,
                       {{vk::Image(motion_image), w, h, 4, "motion", "rg16f"},
                        {vk::Image(output_image), ow, oh, 8, "output", "rgba16f"}});
            DumpImages(instance, scheduler, cmdbuf, dump,
                       {{input_depth, w, h, 4, "depth", "f32", vk::ImageAspectFlagBits::eDepth}});
            if (const auto object = camera_motion.ObjectMotionImage(w, h); object) {
                DumpImages(instance, scheduler, cmdbuf, dump,
                           {{object, w, h, 16, "objects", "rgba32f"}});
            }
            }
        }
        vk::ImageView merge_view = *output_view; // immutable SR fallback remains intact
        bool c2_selected = false;
        bool c2_frame_processed = false;
        const auto c2_current_viewport_jitter = camera_motion.CurrentJitter();
        const auto c2_previous_viewport_jitter = camera_motion.PreviousJitter();
        const std::array<float, 2> c2_current_sr_jitter{last_dlss_jitter_x, last_dlss_jitter_y};
        const std::array<float, 2> c2_previous_sr_jitter{previous_dlss_jitter_x, previous_dlss_jitter_y};
        const auto c2_viewport_size = camera_motion.RenderSize();
        const bool c2_profile = !Scaled() && !reduced && w == 1920 && h == 1080 &&
            ow == 1920 && oh == 1080 && c2_viewport_size[0] == w &&
            c2_viewport_size[1] == h && last_dlss_called && last_dlss_hdr &&
            c2_sr_jitter_zero_from_start && c2_sr_initial_reset_observed;
        if (c2_nr_process.Enabled() && c2_profile) {
            vk::PhysicalDeviceIDProperties identity{};
            vk::PhysicalDeviceProperties2 properties{.pNext = &identity};
            instance.GetPhysicalDevice().getProperties2(&properties);
            const auto device_uuid = HexBytes(identity.deviceUUID);
            const auto driver_uuid = HexBytes(identity.driverUUID);
            if (c2_nr_process.Start(device_uuid, driver_uuid, ow, oh) &&
                c2_game_bridge.Prepare(instance, scheduler, c2_nr_process,
                    c2_nr_process.Session(), c2_nr_process.RunDir(),
                    device_uuid, driver_uuid, ow, oh)) {
                const char* trigger = std::getenv("BB_NR_C2_TRIGGER");
                std::error_code trigger_error;
                const bool trigger_present = trigger && trigger[0] &&
                    std::filesystem::is_regular_file(trigger, trigger_error);
                if (!c2_trigger_consumed && trigger_present) {
                    if (std::remove(trigger) == 0) {
                        c2_trigger_consumed = true;
                        std::printf("C2_TRIGGER_CONSUMED mode=%s source_game_frame=%llu path=%s\n",
                            c2_nr_process.Mode().c_str(),
                            static_cast<unsigned long long>(source_game_frame), trigger);
                        crumb.reset();
                        const auto c2_view = c2_game_bridge.ProcessFrame(
                            instance, scheduler, c2_nr_process, vk::Image(output_image),
                            *output_view, input_depth, input_depth_view,
                            vk::Image(motion_image), *motion_view, vk::ImageLayout::eGeneral,
                            source_game_frame, last_dlss_reset, last_dlss_frame_ms,
                            c2_current_viewport_jitter, c2_previous_viewport_jitter,
                            c2_current_sr_jitter, c2_previous_sr_jitter,
                            c2_sr_initial_reset_observed, c2_sr_initial_source_game_frame,
                            c2_sr_jitter_zero_from_start);
                        cmdbuf = scheduler.CommandBuffer();
                        crumb.emplace(cmdbuf, scheduler.CrumbStream(), "TAA C2 NR merge");
                        c2_frame_processed = c2_game_bridge.FrameProcessed();
                        if (c2_view) {
                            c2_game_bridge.PrepareReturnForMerge(scheduler);
                            merge_view = c2_view;
                            c2_selected = true;
                            std::printf("C2_MERGE_SELECT mode=%s source_game_frame=%llu source_reset=%u "
                                "source_frame_ms=%.9g viewport=%.9g,%.9g previous_viewport=%.9g,%.9g "
                                "sr_jitter=%.9g,%.9g previous_sr=%.9g,%.9g merge_tick=%llu\n",
                                c2_nr_process.Mode().c_str(),
                                static_cast<unsigned long long>(source_game_frame), last_dlss_reset,
                                last_dlss_frame_ms, c2_current_viewport_jitter[0],
                                c2_current_viewport_jitter[1], c2_previous_viewport_jitter[0],
                                c2_previous_viewport_jitter[1], c2_current_sr_jitter[0],
                                c2_current_sr_jitter[1], c2_previous_sr_jitter[0],
                                c2_previous_sr_jitter[1],
                                static_cast<unsigned long long>(scheduler.CurrentTick()));
                        } else if (c2_frame_processed) {
                            std::printf("C2_SKIP_SR_FALLBACK source_game_frame=%llu mode=skipEvaluate\n",
                                static_cast<unsigned long long>(source_game_frame));
                        }
                    } else {
                        std::fprintf(stderr, "C2_TRIGGER consume_failed errno=%d SR_fallback=1\n", errno);
                    }
                }
            } else if (c2_nr_process.SetupFailed()) {
                std::fprintf(stderr, "C2_SETUP_FAILED phase=bootstrap reason=%s SR_fallback=1\n",
                             c2_nr_process.Failure().c_str());
            }
        }
        bool c3_selected = false;
        bool c3_frame_processed = false;
        if (c3_nr_process.Enabled() && c2_profile) {
            vk::PhysicalDeviceIDProperties identity{};
            vk::PhysicalDeviceProperties2 properties{.pNext = &identity};
            instance.GetPhysicalDevice().getProperties2(&properties);
            const auto device_uuid = HexBytes(identity.deviceUUID);
            const auto driver_uuid = HexBytes(identity.driverUUID);
            if (c3_nr_process.Start(device_uuid, driver_uuid, ow, oh) &&
                c3_game_bridge.Prepare(instance, scheduler, c3_nr_process,
                    c3_nr_process.Session(), c3_nr_process.RunDir(),
                    device_uuid, driver_uuid, ow, oh)) {
                if (!c3_trigger_consumed) {
                    const char* trigger = std::getenv("BB_NR_C3_TRIGGER");
                    std::error_code error;
                    if (trigger && *trigger && std::filesystem::is_regular_file(trigger, error)) {
                        if (std::remove(trigger) == 0) {
                            c3_trigger_consumed = true;
                            std::printf("C3_TRIGGER_CONSUMED session=%s mode=%s frames=%u source_game_frame=%llu path=%s\n",
                                c3_nr_process.Session().c_str(), c3_nr_process.Mode().c_str(),
                                c3_nr_process.FrameCount(), (unsigned long long)source_game_frame, trigger);
                        } else std::fprintf(stderr, "C3_TRIGGER consume_failed errno=%d SR_fallback=1\n", errno);
                    }
                }
                bool pending_present = false;
                { std::scoped_lock lock{c1_audit_mutex}; pending_present = c3_pending_audit.has_value(); }
                if (c3_trigger_consumed && !pending_present && !c3_game_bridge.Finished() &&
                    !c3_game_bridge.Quarantined() && c3_game_bridge.HasRemainingFrames()) {
                    const u32 next_nr_frame = c3_game_bridge.NextFrameIndex();
                    const u32 count = c3_nr_process.FrameCount();
                    const bool audit_full = count == 4 || !next_nr_frame ||
                        (next_nr_frame + 1) % 30 == 0 || next_nr_frame + 1 == count;
                    if (next_nr_frame < count && audit_full) {
                        std::printf("C3_CAMERA_STATE session=%s frame=%u source_game_frame=%llu audit_kind=full\n",
                            c3_nr_process.Session().c_str(), next_nr_frame,
                            (unsigned long long)source_game_frame);
                        camera_motion.PrintState(static_cast<int>(next_nr_frame));
                    }
                    crumb.reset();
                    const auto view = c3_game_bridge.ProcessFrame(
                        instance, scheduler, c3_nr_process, vk::Image(output_image), *output_view,
                        input_depth, input_depth_view, vk::Image(motion_image), *motion_view,
                        vk::ImageLayout::eGeneral, source_game_frame, last_dlss_reset, last_dlss_frame_ms,
                        c2_current_viewport_jitter, c2_previous_viewport_jitter,
                        c2_current_sr_jitter, c2_previous_sr_jitter,
                        c2_sr_initial_reset_observed, c2_sr_initial_source_game_frame,
                        c2_sr_jitter_zero_from_start);
                    cmdbuf = scheduler.CommandBuffer();
                    crumb.emplace(cmdbuf, scheduler.CrumbStream(), "TAA C3 resident NR merge");
                    c3_frame_processed = c3_game_bridge.FrameProcessed();
                    if (view) {
                        c3_game_bridge.PrepareReturnForMerge(scheduler);
                        merge_view = view;
                        c3_selected = true;
                        std::printf("C3_MERGE_SELECT stage=C3 mode=%s session=%s frame=%u source_game_frame=%llu merge_tick=%llu original_SR_immutable=1\n",
                            c3_nr_process.Mode().c_str(), c3_nr_process.Session().c_str(),
                            c3_game_bridge.FrameIndex(), (unsigned long long)source_game_frame,
                            (unsigned long long)scheduler.CurrentTick());
                    } else if (c3_frame_processed) {
                        std::printf("C3_SKIP_SR_FALLBACK stage=C3 mode=skipEvaluate session=%s frame=%u source_game_frame=%llu\n",
                            c3_nr_process.Session().c_str(), c3_game_bridge.FrameIndex(),
                            (unsigned long long)source_game_frame);
                    }
                } else if (pending_present && c3_trigger_consumed) {
                    std::printf("C3_BYPASS_PENDING_PRESENT source_game_frame=%llu SR_fallback=1 counted_valid=0\n",
                                (unsigned long long)source_game_frame);
                }
            }
        }
        const auto q1_proxy_depth=scene_targets.CaptureCreateInfo(input_depth);
        const bool q1_profile=!Scaled()&&reduced&&w==1280&&h==720&&ow==1920&&oh==1080&&
            last_dlss_called&&last_dlss_hdr&&UseDlss()&&BbSettings::Get().jitter&&q1_proxy_depth;
        bool native_async_recorded=false,native_async_current=false,native_sky_selected=false;
        if(native_async_snapshot.Enabled()&&native_async_snapshot.Armed()&&q1_profile) {
            bool pending=false;{std::scoped_lock lock{c1_audit_mutex};pending=native_async_pending_audit.has_value();}
            if(!pending)try {
                native_async_current=true;
                native_async_recorded=native_async_snapshot.TryCaptureOrigin(instance,scheduler,
                    {vk::Image(output_image),input_depth,vk::Image(motion_image)},
                    {output_image.image_ci,*q1_proxy_depth,motion_image.image_ci},
                    {.source=source_game_frame,.sr_reset=last_dlss_reset,
                     .viewport=c2_current_viewport_jitter,.previous_viewport=c2_previous_viewport_jitter,
                     .sr_jitter=c2_current_sr_jitter,.previous_sr_jitter=c2_previous_sr_jitter});
                // Current genuine link is recorded before checking complete catchup.
                const auto native_edit=native_async_recorded ? native_async_snapshot.RecordAdvanceAndApply(scheduler,source_game_frame) : vk::ImageView{};
                if(native_edit){merge_view=native_edit;native_sky_selected=true;}
                if(!native_async_recorded)native_async_snapshot.DrainUnavailable(scheduler);
            }catch(const std::exception& e){native_async_snapshot.Quarantine();std::fprintf(stderr,"NATIVE_ASYNC_HOLD reason=%s current_SR=1 no_edit_selected=1\n",e.what());}
        }
        bool p0_selected=false, p0_frame_processed=false;
        if(P0Enabled() && q1_profile) {
            bool pending=false;
            {std::scoped_lock lock{c1_audit_mutex};pending=p0_pending_audit.has_value();}
            if(!pending) {
                if(p0_controller.AuditFull()) camera_motion.PrintQualityCaptureState(static_cast<int>(p0_controller.FrameIndex()),source_game_frame);
                crumb.reset();
                const auto view=p0_controller.ProcessScene(instance,scheduler,vk::Image(output_image),*output_view,
                    input_depth,input_depth_view,vk::Image(motion_image),*motion_view,vk::ImageLayout::eGeneral,
                    source_game_frame,last_dlss_reset,last_dlss_frame_ms,
                    c2_current_viewport_jitter,c2_previous_viewport_jitter,c2_current_sr_jitter,c2_previous_sr_jitter,
                    c2_sr_initial_reset_observed,c2_sr_initial_source_game_frame,q1_sr_jitter_enabled_from_start,
                    {output_image.image_ci,*q1_proxy_depth,motion_image.image_ci});
                cmdbuf=scheduler.CommandBuffer();
                crumb.emplace(cmdbuf,scheduler.CrumbStream(),"TAA P0 managed NR merge");
                p0_frame_processed=p0_controller.FrameProcessed();
                if(view) {p0_controller.PrepareReturnForMerge(scheduler);merge_view=view;p0_selected=true;}
            }
        }
        bool q1_selected = false;
        bool q1_frame_processed = false;
        if (q1_nr_process.Enabled() && q1_profile) {
            vk::PhysicalDeviceIDProperties identity{};
            vk::PhysicalDeviceProperties2 properties{.pNext = &identity};
            instance.GetPhysicalDevice().getProperties2(&properties);
            const auto device_uuid = HexBytes(identity.deviceUUID);
            const auto driver_uuid = HexBytes(identity.driverUUID);
            if (q1_nr_process.Start(device_uuid, driver_uuid, ow, oh) &&
                q1_game_bridge.Prepare(instance, scheduler, q1_nr_process,
                    q1_nr_process.Session(), q1_nr_process.RunDir(),
                    device_uuid, driver_uuid, ow, oh)) {
                if (!q1_trigger_consumed) {
                    const char* trigger = std::getenv("BB_NR_Q1_TRIGGER");
                    std::error_code error;
                    if (trigger && *trigger && std::filesystem::is_regular_file(trigger, error)) {
                        if (std::remove(trigger) == 0) {
                            q1_trigger_consumed = true;
                            std::printf("Q1_TRIGGER_CONSUMED session=%s mode=%s frames=%u source_game_frame=%llu path=%s\n",
                                q1_nr_process.Session().c_str(), q1_nr_process.Mode().c_str(),
                                q1_nr_process.FrameCount(), (unsigned long long)source_game_frame, trigger);
                        } else std::fprintf(stderr, "Q1_TRIGGER consume_failed errno=%d SR_fallback=1\n", errno);
                    }
                }
                bool pending_present = false;
                { std::scoped_lock lock{c1_audit_mutex}; pending_present = q1_pending_audit.has_value(); }
                if (q1_trigger_consumed && !pending_present && !q1_game_bridge.Finished() &&
                    !q1_game_bridge.Quarantined() && q1_game_bridge.HasRemainingFrames()) {
                    const u32 next_nr_frame = q1_game_bridge.NextFrameIndex();
                    const u32 count = q1_nr_process.FrameCount();
                    const bool audit_full = q1_nr_process.ExecutionMode() == "diagnostic";
                    if (next_nr_frame < count && audit_full) {
                        std::printf("Q1_CAMERA_STATE session=%s frame=%u source_game_frame=%llu audit_kind=full\n",
                            q1_nr_process.Session().c_str(), next_nr_frame,
                            (unsigned long long)source_game_frame);
                        camera_motion.PrintQualityCaptureState(static_cast<int>(next_nr_frame),source_game_frame);
                        const auto log_source = [&](const char* role, vk::Image image, vk::ImageView view,
                                                    const vk::ImageCreateInfo& ci, vk::Format view_format, u32 aspect) {
                            std::printf("Q1_SOURCE_IMAGE session=%s frame=%u source_game_frame=%llu role=%s image=%p view=%p create_format=%u view_format=%u width=%u height=%u depth=%u usage=0x%x flags=0x%x mips=%u layers=%u samples=%u tiling=%u aspect=%u layout=GENERAL valid_rect=0,0,%u,%u\n",
                                q1_nr_process.Session().c_str(), next_nr_frame, (unsigned long long)source_game_frame,
                                role, (void*)VkImage(image), (void*)VkImageView(view), unsigned(ci.format),
                                unsigned(view_format), ci.extent.width, ci.extent.height, ci.extent.depth,
                                unsigned(VkImageUsageFlags(ci.usage)), unsigned(VkImageCreateFlags(ci.flags)),
                                ci.mipLevels, ci.arrayLayers, unsigned(ci.samples), unsigned(ci.tiling), aspect,
                                ci.extent.width, ci.extent.height);
                        };
                        if (const auto proxy_color = scene_targets.CaptureCreateInfo(input_color))
                            log_source("actual_SR_input_proxy",input_color,input_color_view,*proxy_color,color.info.pixel_format,1);
                        log_source("actual_SR_depth_proxy",input_depth,input_depth_view,*q1_proxy_depth,depth.info.pixel_format,2);
                        log_source("actual_MV",vk::Image(motion_image),*motion_view,motion_image.image_ci,vk::Format::eR16G16Sfloat,1);
                        log_source("actual_post_SR_Color",vk::Image(output_image),*output_view,output_image.image_ci,vk::Format::eR16G16B16A16Sfloat,1);
                    }
                    crumb.reset();
                    const auto view = q1_game_bridge.ProcessFrame(
                        instance, scheduler, q1_nr_process, vk::Image(output_image), *output_view,
                        input_depth, input_depth_view, vk::Image(motion_image), *motion_view,
                        vk::ImageLayout::eGeneral, source_game_frame, last_dlss_reset, last_dlss_frame_ms,
                        c2_current_viewport_jitter, c2_previous_viewport_jitter,
                        c2_current_sr_jitter, c2_previous_sr_jitter,
                        c2_sr_initial_reset_observed, c2_sr_initial_source_game_frame,
                        q1_sr_jitter_enabled_from_start,
                        {output_image.image_ci,*q1_proxy_depth,motion_image.image_ci});
                    cmdbuf = scheduler.CommandBuffer();
                    crumb.emplace(cmdbuf, scheduler.CrumbStream(), "TAA Q1 resident NR merge");
                    q1_frame_processed = q1_game_bridge.FrameProcessed();
                    if (view) {
                        q1_game_bridge.PrepareReturnForMerge(scheduler);
                        merge_view = view;
                        q1_selected = true;
                        std::printf("Q1_MERGE_SELECT stage=Q1 mode=%s session=%s frame=%u source_game_frame=%llu merge_tick=%llu original_SR_immutable=1\n",
                            q1_nr_process.Mode().c_str(), q1_nr_process.Session().c_str(),
                            q1_game_bridge.FrameIndex(), (unsigned long long)source_game_frame,
                            (unsigned long long)scheduler.CurrentTick());
                    } else if (q1_frame_processed) {
                        std::printf("Q1_SKIP_SR_FALLBACK stage=Q1 mode=skipEvaluate session=%s frame=%u source_game_frame=%llu\n",
                            q1_nr_process.Session().c_str(), q1_game_bridge.FrameIndex(),
                            (unsigned long long)source_game_frame);
                    }
                } else if (pending_present && q1_trigger_consumed) {
                    std::printf("Q1_BYPASS_PENDING_PRESENT source_game_frame=%llu SR_fallback=1 counted_valid=0\n",
                                (unsigned long long)source_game_frame);
                }
            }
        }
        bool c4_selected = false;
        bool c4_frame_processed = false;
        if (c4_nr_process.Enabled() && c2_profile) {
            vk::PhysicalDeviceIDProperties identity{};
            vk::PhysicalDeviceProperties2 properties{.pNext = &identity};
            instance.GetPhysicalDevice().getProperties2(&properties);
            const auto device_uuid = HexBytes(identity.deviceUUID);
            const auto driver_uuid = HexBytes(identity.driverUUID);
            if (c4_nr_process.Start(device_uuid, driver_uuid, ow, oh) &&
                c4_game_bridge.Prepare(instance, scheduler, c4_nr_process,
                    c4_nr_process.Session(), c4_nr_process.RunDir(),
                    device_uuid, driver_uuid, ow, oh)) {
                if (!c4_trigger_consumed) {
                    const char* trigger = std::getenv("BB_NR_C4_TRIGGER");
                    std::error_code error;
                    if (trigger && *trigger && std::filesystem::is_regular_file(trigger, error)) {
                        if (std::remove(trigger) == 0) {
                            c4_trigger_consumed = true;
                            std::printf("C4_TRIGGER_CONSUMED session=%s mode=%s frames=%u source_game_frame=%llu path=%s\n",
                                c4_nr_process.Session().c_str(), c4_nr_process.Mode().c_str(),
                                c4_nr_process.FrameCount(), (unsigned long long)source_game_frame, trigger);
                        } else std::fprintf(stderr, "C4_TRIGGER consume_failed errno=%d SR_fallback=1\n", errno);
                    }
                }
                bool pending_present = false;
                { std::scoped_lock lock{c1_audit_mutex}; pending_present = c4_pending_audit.has_value(); }
                if (c4_trigger_consumed && !pending_present && !c4_game_bridge.Finished() &&
                    !c4_game_bridge.Quarantined() && c4_game_bridge.HasRemainingFrames()) {
                    const u32 next_nr_frame = c4_game_bridge.NextFrameIndex();
                    const u32 count = c4_nr_process.FrameCount();
                    const bool audit_full = c4_nr_process.ExecutionMode() == "diagnostic";
                    if (next_nr_frame < count && audit_full) {
                        std::printf("C4_CAMERA_STATE session=%s frame=%u source_game_frame=%llu audit_kind=full\n",
                            c4_nr_process.Session().c_str(), next_nr_frame,
                            (unsigned long long)source_game_frame);
                        camera_motion.PrintState(static_cast<int>(next_nr_frame));
                    }
                    crumb.reset();
                    const auto view = c4_game_bridge.ProcessFrame(
                        instance, scheduler, c4_nr_process, vk::Image(output_image), *output_view,
                        input_depth, input_depth_view, vk::Image(motion_image), *motion_view,
                        vk::ImageLayout::eGeneral, source_game_frame, last_dlss_reset, last_dlss_frame_ms,
                        c2_current_viewport_jitter, c2_previous_viewport_jitter,
                        c2_current_sr_jitter, c2_previous_sr_jitter,
                        c2_sr_initial_reset_observed, c2_sr_initial_source_game_frame,
                        c2_sr_jitter_zero_from_start);
                    cmdbuf = scheduler.CommandBuffer();
                    crumb.emplace(cmdbuf, scheduler.CrumbStream(), "TAA C4 resident NR merge");
                    c4_frame_processed = c4_game_bridge.FrameProcessed();
                    if (view) {
                        c4_game_bridge.PrepareReturnForMerge(scheduler);
                        merge_view = view;
                        c4_selected = true;
                        std::printf("C4_MERGE_SELECT stage=C4 mode=%s session=%s frame=%u source_game_frame=%llu merge_tick=%llu original_SR_immutable=1\n",
                            c4_nr_process.Mode().c_str(), c4_nr_process.Session().c_str(),
                            c4_game_bridge.FrameIndex(), (unsigned long long)source_game_frame,
                            (unsigned long long)scheduler.CurrentTick());
                    } else if (c4_frame_processed) {
                        std::printf("C4_SKIP_SR_FALLBACK stage=C4 mode=skipEvaluate session=%s frame=%u source_game_frame=%llu\n",
                            c4_nr_process.Session().c_str(), c4_game_bridge.FrameIndex(),
                            (unsigned long long)source_game_frame);
                    }
                } else if (pending_present && c4_trigger_consumed) {
                    std::printf("C4_BYPASS_PENDING_PRESENT source_game_frame=%llu SR_fallback=1 counted_valid=0\n",
                                (unsigned long long)source_game_frame);
                }
            }
        }
        bool c1_selected = false;
        const bool c1_profile = !Scaled() && !reduced && w == 1920 && h == 1080 &&
            ow == 1920 && oh == 1080 && last_dlss_called && last_dlss_hdr;
        if (c1_noop_process.Enabled() && c1_profile) {
            vk::PhysicalDeviceIDProperties identity{};
            vk::PhysicalDeviceProperties2 properties{.pNext = &identity};
            instance.GetPhysicalDevice().getProperties2(&properties);
            const auto device_uuid = HexBytes(identity.deviceUUID);
            const auto driver_uuid = HexBytes(identity.driverUUID);
            if (c1_noop_process.Start(device_uuid, driver_uuid, ow, oh) &&
                c1_game_bridge.Prepare(instance, scheduler, c1_noop_process,
                    c1_noop_process.Session(), c1_noop_process.RunDir(),
                    device_uuid, driver_uuid, ow, oh)) {
                const char* trigger = std::getenv("BB_NR_C1_TRIGGER");
                std::error_code trigger_error;
                const bool trigger_present = trigger && trigger[0] &&
                    std::filesystem::is_regular_file(trigger, trigger_error);
                if (!c1_trigger_consumed && trigger_present) {
                    // One explicit root-controlled Clinic trigger selects exactly one real
                    // SR frame. Setup/import may progress before this file exists, but title
                    // and early-scene frames always keep the original SR merge input.
                    const int consume_rc = std::remove(trigger);
                    if (consume_rc == 0) {
                        c1_trigger_consumed = true;
                        std::printf("C1_TRIGGER_CONSUMED path=%s source_game_frame=%llu\n",
                                    trigger,
                                    static_cast<unsigned long long>(source_game_frame));
                        const auto viewport_jitter = camera_motion.CurrentJitter();
                        crumb.reset();
                        const auto c1_view = c1_game_bridge.ProcessFrame(
                            instance, scheduler, c1_noop_process, vk::Image(output_image),
                            *output_view, input_depth, input_depth_view,
                            vk::Image(motion_image), *motion_view,
                            vk::ImageLayout::eGeneral, source_game_frame, last_dlss_reset,
                            viewport_jitter, {last_dlss_jitter_x, last_dlss_jitter_y});
                        cmdbuf = scheduler.CommandBuffer();
                        crumb.emplace(cmdbuf, scheduler.CrumbStream(), "TAA C1 merge");
                        if (c1_view) {
                            merge_view = c1_view;
                            c1_selected = true;
                            std::printf("C1_MERGE_SELECT source_game_frame=%llu "
                                        "source_reset=%u viewport_jitter=%.9g,%.9g "
                                        "sr_jitter=%.9g,%.9g merge_tick=%llu\n",
                                static_cast<unsigned long long>(source_game_frame),
                                last_dlss_reset, viewport_jitter[0], viewport_jitter[1],
                                last_dlss_jitter_x, last_dlss_jitter_y,
                                static_cast<unsigned long long>(scheduler.CurrentTick()));
                        }
                    } else {
                        std::fprintf(stderr, "C1_TRIGGER consume_failed errno=%d SR_fallback=1\n",
                                     errno);
                    }
                } else if (!c1_trigger_consumed) {
                    static u64 waiting_notice = 0;
                    if (++waiting_notice == 1 || waiting_notice % 300 == 0) {
                        std::printf("C1_WAIT_TRIGGER source_game_frame=%llu helper_pid_ready=1 "
                                    "bridge_ready=1 trigger=%s\n",
                                    static_cast<unsigned long long>(source_game_frame),
                                    trigger ? trigger : "unset");
                    }
                }
            } else if (c1_noop_process.SetupFailed()) {
                std::fprintf(stderr, "C1_SETUP_FAILED reason=%s SR_fallback=1\n",
                             c1_noop_process.Failure().c_str());
            }
        }
        // The result replaces the scene color's RGB (its alpha carries data for the post).
        own_barrier(vk::Image(output_image), vk::ImageLayout::eGeneral, all, rw,
                    vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
        runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        runtime.FlushBarriers();
        const vk::DescriptorImageInfo out_info{.imageView = merge_view,
                                               .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo scene_info{.imageView = color_view,
                                                 .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo mask_info{.imageView = *reactive_view,
                                                .imageLayout = vk::ImageLayout::eGeneral};
        // Sampled for the debug view only; the object image falls back to the motion image.
        const vk::ImageView objects = camera_motion.ObjectMotionView();
        const vk::DescriptorImageInfo motion_info{.imageView = *motion_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo objects_info{
            .imageView = objects ? objects : *motion_view,
            .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 5> writes = {{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &out_info},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &scene_info},
            {.dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &mask_info},
            {.dstBinding = 3,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &motion_info},
            {.dstBinding = 4,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &objects_info},
        }};
        // Debug (menu or toggle 1 << 28): the reactive mask in red over a darkened frame;
        // or the motion vectors (menu).
        const int debug_view = BbSettings::Get().debug_view;
        const u32 mode = has_reactive && (debug_view == BbSettings::DebugReactive ||
                                          BbToggle::Disabled(1u << 28))
                             ? 1
                         : debug_view == BbSettings::DebugMotion ? 2
                                                                 : 0;
        if (c4_frame_processed) c4_game_bridge.BeginMergeTiming(scheduler);
        if (q1_frame_processed) q1_game_bridge.BeginMergeTiming(scheduler);
        if (p0_frame_processed) p0_controller.BeginMergeTiming(scheduler);
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, native_sky_selected ? *native_sky_merge_pipeline : *merge_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *merge_pipeline_layout, 0,
                                    writes);
        cmdbuf.pushConstants(*merge_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(mode), &mode);
        cmdbuf.dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
        if (c4_frame_processed) c4_game_bridge.EndMergeTiming(scheduler);
        if (q1_frame_processed) q1_game_bridge.EndMergeTiming(scheduler);
        if (p0_frame_processed) p0_controller.EndMergeTiming(scheduler);
        if(native_pre_sr.Enabled()&&native_pre_sr.CurrentAttempt(source_game_frame)){
            native_pre_sr.MergeRecorded(scheduler,source_game_frame);
            std::scoped_lock lock{c1_audit_mutex};
            if(native_pre_sr_pending_audit)throw std::runtime_error("preSR cannot overwrite pending current-source ledger");
            native_pre_sr_pending_audit=C1PresentAudit{.stage="PRE_SR",.mode=native_pre_sr.CurrentSelected(source_game_frame)?"NR-before-SR":"original-SR-fallback",
                .session=native_pre_sr.Session(),.execution_mode="hot",.selected_output=native_pre_sr.CurrentSelected(source_game_frame),
                .audit_full=false,.nr_frame=unsigned(native_pre_sr.CurrentJob()),.source_game_frame=source_game_frame,.merge_tick=scheduler.CurrentTick()};
            std::printf("NATIVE_PRE_SR_MERGE_RECORDED session=%s source_game_frame=%llu job=%llu selected_NR720=%u merge_tick=%llu SR_display=1920x1080 original_scene_alpha_preserved=1\n",
                native_pre_sr.Session().c_str(),(unsigned long long)source_game_frame,
                (unsigned long long)native_pre_sr.CurrentJob(),native_pre_sr.CurrentSelected(source_game_frame),(unsigned long long)scheduler.CurrentTick());
        }
        if(native_async_current) {
            if(native_sky_selected) native_async_snapshot.MergeRecorded(scheduler,source_game_frame);
            std::scoped_lock lock{c1_audit_mutex};
            if(native_async_pending_audit)throw std::runtime_error("Native async cannot overwrite pending source ledger");
            native_async_pending_audit=C1PresentAudit{.stage="NATIVE_ASYNC",.mode=(native_sky_selected&&native_async_snapshot.EffectSelected())?"sparse-NR-Sky-current":native_sky_selected?"local-noop-Sky-current":"busy-current-SR",.session=native_async_snapshot.Session(),
                .execution_mode="native-only",.selected_output=native_sky_selected,.audit_full=false,.nr_frame=native_async_snapshot.EffectJob(),
                .source_game_frame=source_game_frame,.merge_tick=scheduler.CurrentTick()};
            std::printf("NATIVE_ASYNC_MERGE_CURRENT_SR source_game_frame=%llu tick=%llu origin_epoch=%llu origin_source=%llu cache_selected=%u effect_applied=%u sparse_job=%u source_edited=0\n",
                (unsigned long long)source_game_frame,(unsigned long long)scheduler.CurrentTick(),(unsigned long long)native_async_snapshot.Epoch(),(unsigned long long)native_async_snapshot.OriginSource(),native_sky_selected,native_sky_selected&&native_async_snapshot.EffectSelected(),native_async_snapshot.EffectJob());
        }
        if (c3_frame_processed) {
            std::scoped_lock lock{c1_audit_mutex};
            if (c3_pending_audit) throw std::runtime_error("C3 must not overwrite an unbound presentation audit");
            c3_pending_audit = C1PresentAudit{
                .stage = "C3", .mode = c3_nr_process.Mode(), .session = c3_nr_process.Session(),
                .selected_output = c3_selected, .audit_full = c3_game_bridge.AuditFull(),
                .nr_frame = c3_game_bridge.FrameIndex(), .source_game_frame = source_game_frame,
                .merge_tick = scheduler.CurrentTick()};
            std::printf("C3_MERGE_RECORDED stage=C3 mode=%s session=%s frame=%u source_game_frame=%llu tick=%llu selected_output=%u audit_kind=%s fallback_immutable=1\n",
                c3_pending_audit->mode.c_str(), c3_pending_audit->session.c_str(),
                c3_pending_audit->nr_frame, (unsigned long long)source_game_frame,
                (unsigned long long)scheduler.CurrentTick(), c3_selected,
                c3_pending_audit->audit_full ? "full" : "gpu_full_scan");
            c3_game_bridge.DeferRetirement(instance, scheduler);
        }
        if(p0_frame_processed) {
            std::scoped_lock lock{c1_audit_mutex};
            if(p0_pending_audit) throw std::runtime_error("P0 cannot overwrite unbound Present ledger");
            p0_pending_audit=C1PresentAudit{.stage="P0",.mode=p0_controller.Mode(),.session=p0_controller.Session(),
                .execution_mode=p0_controller.ExecutionMode(),.selected_output=p0_selected,.audit_full=p0_controller.AuditFull(),
                .nr_frame=p0_controller.FrameIndex(),.source_game_frame=source_game_frame,.merge_tick=scheduler.CurrentTick()};
            std::printf("P0_MERGE_RECORDED stage=P0 mode=%s session=%s execution_mode=%s frame=%u source_game_frame=%llu tick=%llu selected_output=%u audit_kind=%s fallback_immutable=1\n",
                p0_pending_audit->mode.c_str(),p0_pending_audit->session.c_str(),p0_pending_audit->execution_mode.c_str(),p0_pending_audit->nr_frame,
                (unsigned long long)source_game_frame,(unsigned long long)scheduler.CurrentTick(),p0_selected,p0_pending_audit->audit_full?"full":"off");
            p0_controller.DeferRetirement(instance,scheduler);
        }
        if (q1_frame_processed) {
            std::scoped_lock lock{c1_audit_mutex};
            if (q1_pending_audit) throw std::runtime_error("Q1 must not overwrite an unbound presentation audit");
            q1_pending_audit = C1PresentAudit{
                .stage = "Q1", .mode = q1_nr_process.Mode(), .session = q1_nr_process.Session(),
                .execution_mode = q1_nr_process.ExecutionMode(),
                .selected_output = q1_selected, .audit_full = q1_game_bridge.AuditFull(),
                .nr_frame = q1_game_bridge.FrameIndex(), .source_game_frame = source_game_frame,
                .merge_tick = scheduler.CurrentTick()};
            std::printf("Q1_MERGE_RECORDED stage=Q1 mode=%s session=%s frame=%u source_game_frame=%llu tick=%llu selected_output=%u audit_kind=%s fallback_immutable=1\n",
                q1_pending_audit->mode.c_str(), q1_pending_audit->session.c_str(),
                q1_pending_audit->nr_frame, (unsigned long long)source_game_frame,
                (unsigned long long)scheduler.CurrentTick(), q1_selected,
                q1_pending_audit->audit_full ? "full" : "off");
            q1_game_bridge.DeferRetirement(instance, scheduler);
        }
        if (c4_frame_processed) {
            std::scoped_lock lock{c1_audit_mutex};
            if (c4_pending_audit) throw std::runtime_error("C4 must not overwrite an unbound presentation audit");
            c4_pending_audit = C1PresentAudit{
                .stage = "C4", .mode = c4_nr_process.Mode(), .session = c4_nr_process.Session(),
                .execution_mode = c4_nr_process.ExecutionMode(),
                .selected_output = c4_selected, .audit_full = c4_game_bridge.AuditFull(),
                .nr_frame = c4_game_bridge.FrameIndex(), .source_game_frame = source_game_frame,
                .merge_tick = scheduler.CurrentTick()};
            std::printf("C4_MERGE_RECORDED stage=C4 mode=%s session=%s frame=%u source_game_frame=%llu tick=%llu selected_output=%u audit_kind=%s fallback_immutable=1\n",
                c4_pending_audit->mode.c_str(), c4_pending_audit->session.c_str(),
                c4_pending_audit->nr_frame, (unsigned long long)source_game_frame,
                (unsigned long long)scheduler.CurrentTick(), c4_selected,
                c4_pending_audit->audit_full ? "full" : "off");
            c4_game_bridge.DeferRetirement(instance, scheduler);
        }
        if (c2_frame_processed) {
            std::scoped_lock lock{c1_audit_mutex};
            c2_pending_source_game_frame = source_game_frame;
            c2_pending_merge_tick = scheduler.CurrentTick();
            c2_pending_mode = std::string(c2_game_bridge.Mode());
            c2_pending_session = c2_nr_process.Session();
            c2_pending_selected_output = c2_selected;
            std::printf("C2_MERGE_RECORDED stage=C2 mode=%s session=%s source_game_frame=%llu "
                        "tick=%llu selected_output=%u fallback_immutable=1\n",
                c2_pending_mode.c_str(), c2_pending_session.c_str(),
                static_cast<unsigned long long>(source_game_frame),
                static_cast<unsigned long long>(scheduler.CurrentTick()), c2_selected);
            c2_game_bridge.DeferRetirement(instance, scheduler);
        }
        if (c1_selected) {
            std::scoped_lock lock{c1_audit_mutex};
            c1_pending_source_game_frame = source_game_frame;
            c1_pending_merge_tick = scheduler.CurrentTick();
            std::printf("C1_MERGE_RECORDED source_game_frame=%llu tick=%llu image=independent-return\n",
                static_cast<unsigned long long>(source_game_frame),
                static_cast<unsigned long long>(scheduler.CurrentTick()));
            c1_game_bridge.DeferRetirement(instance, scheduler);
        }
    }
}

vk::ImageView TemporalUpscaler::CachedView(const VideoCore::Image& image, vk::Format format,
                                           vk::ImageAspectFlags aspect) {
    const vk::Image handle = image.GetImage();
    ViewEntry* oldest = &view_cache[0];
    for (auto& entry : view_cache) {
        if (entry.view && entry.image == handle && entry.uid == image.image_uid &&
            entry.format == format && entry.aspect == aspect) {
            entry.last_use = ++view_uses;
            return entry.view;
        }
        if (entry.last_use < oldest->last_use) {
            oldest = &entry;
        }
    }
    const auto device = instance.GetDevice();
    if (oldest->view) {
        scheduler.DeferOperation([device, view = oldest->view] { device.destroyImageView(view); });
    }
    *oldest = {
        .image = handle,
        .uid = image.image_uid,
        .format = format,
        .aspect = aspect,
        .view = Check(device.createImageView({
            .image = handle,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        })),
        .last_use = ++view_uses,
    };
    return oldest->view;
}

} // namespace Vulkan

namespace Vulkan {

std::array<u32, 2> TemporalUpscaler::SceneSize(u32 w, u32 h) const {
    if (!scaled_session) return {render_width, render_height};
    // The game's own render size (scene constants); BB_RENDER_RES until the first camera.
    auto size = camera_motion.RenderSize();
    if (size[0] == 0 || size[1] == 0) {
        size = {render_width, render_height};
    }
    return {std::min(size[0], w), std::min(size[1], h)};
}

float TemporalUpscaler::SceneMipBias() const {
    if (!Active() || BbSettings::Get().upscaler == BbSettings::UpscalerTaa) return 0.0f;
    const float render = float(BbSettings::Get().active_render_width.load());
    const float output = float(Scaled() ? target_width : 1920u);
    return render > 0.0f && render < output ? std::log2(render / output) : 0.0f;
}

bool TemporalUpscaler::Scaled() const {
    return scaled_session || target_width != 1920 || target_height != 1080;
}

void TemporalUpscaler::OnColorTarget(VideoCore::ImageId color) {
    if (ui_phase || !camera_motion.Depth()) {
        return;
    }
    const auto& image = texture_cache.GetImage(color);
    const auto& depth = texture_cache.GetImage(camera_motion.Depth());
    if (image.info.pixel_format == vk::Format::eR8G8B8A8Unorm &&
        image.info.size.width == depth.info.size.width &&
        image.info.size.height == depth.info.size.height) {
        ldr_target = color;
    }
}

void TemporalUpscaler::OnDraw(u64 vs_hash, VideoCore::ImageId color,
                              VideoCore::ImageId depth, bool native_viewport) {
    if (!Scaled() && vs_hash == ui_trigger_vs) done_this_frame = true;
    // bbport (issue #67, patch by bmy): a UI draw without a color target (a Scaleform mask, color
    // writes off) writes the stencil the UI's next draws test, so it must go into the UI's
    // output-size depth like them. Left in the guest depth, the mask was lost when the next UI
    // draw switched to the UI depth (PrepareUiDepth clears its stencil): masked images (the
    // loading screen's item picture, cut out of a sheet) vanished.
    if (ui_phase && !color && depth && depth != ui_depth && ui_color) {
        const auto& depth_image = texture_cache.GetImage(depth);
        const auto& ui_target = texture_cache.GetImage(ui_color);
        if (depth_image.info.size.width == ui_target.info.size.width &&
            depth_image.info.size.height == ui_target.info.size.height) {
            EnsureUiResources(ui_width, ui_height, ui_format, depth_image.info.pixel_format);
            PrepareUiDepth(depth);
            ui_depth = depth;
        }
        return;
    }
    if (!Scaled() || !color) {
        return;
    }
    if (ui_phase) {
        // A UI movie can start without stencil and enable it for later text/masks.
        if (color == ui_color && depth && depth != ui_depth) {
            EnsureUiResources(ui_width, ui_height, ui_format,
                              texture_cache.GetImage(depth).info.pixel_format);
            PrepareUiDepth(depth);
            ui_depth = depth;
        }
        return;
    }
    const auto& image = texture_cache.GetImage(color);
    const bool movie = vs_hash == ui_trigger_vs || UiComposition::MovieShader(vs_hash) ||
                       (scaled_session && native_viewport);
    const bool ui_draw = movie &&
        (image.info.pixel_format == vk::Format::eR8G8B8A8Unorm ||
         image.info.pixel_format == vk::Format::eR8G8B8A8Srgb) &&
        RenderTarget(image.info.size.width, image.info.size.height) &&
        !FrameCapture::IsDisplayBuffer(image.info.guest_address);
    switch (UiComposition::Choose(Scaled(), ui_draw,
                                  camera_motion.Ready() && ldr_target == color,
                                  Active() && !failed)) {
    case UiComposition::Background::None:
        return;
    case UiComposition::Background::Temporal:
        RunScaled();
        if (ui_phase) {
            return;
        }
        // A failed FSR dispatch must not force UI text back to the scene resolution.
        [[fallthrough]];
    case UiComposition::Background::Copy:
        RunUiOnly(color, depth);
        return;
    }
}

void TemporalUpscaler::EnsureUiResources(u32 w, u32 h, vk::Format color, vk::Format depth) {
    const bool resized = w != ui_width || h != ui_height;
    const bool new_color = !ui_image || resized || color != ui_format;
    const bool new_depth = !ui_depth_image || resized || depth != ui_depth_format;
    if (!new_color && !new_depth) {
        return;
    }
    scheduler.Finish();
    const auto device = instance.GetDevice();
    const auto allocator = instance.GetAllocator();
    ui_width = w;
    ui_height = h;
    if (new_color) {
        ui_views.clear();
        ui_view.reset();
        ui_storage_view.reset();
        ui_image = VideoCore::UniqueImage(device, allocator);
        ui_image.Create(vk::ImageCreateInfo{
            .flags = vk::ImageCreateFlagBits::eMutableFormat | vk::ImageCreateFlagBits::eExtendedUsage,
            .imageType = vk::ImageType::e2D,
            .format = color,
            .extent = {w, h, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                     vk::ImageUsageFlagBits::eTransferSrc,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        ui_format = color;
    }
    if (new_depth) {
        ui_depth_view.reset();
        ui_depth_image = VideoCore::UniqueImage(device, allocator);
        ui_depth_image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = depth,
            .extent = {w, h, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                     vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        const auto aspect = vk::ImageAspectFlagBits::eDepth |
            (depth == vk::Format::eD32SfloatS8Uint ? vk::ImageAspectFlagBits::eStencil
                                                : vk::ImageAspectFlags{});
        ui_depth_view = Check(device.createImageViewUnique({
            .image = vk::Image(ui_depth_image), .viewType = vk::ImageViewType::e2D,
            .format = depth, .subresourceRange = {aspect, 0, 1, 0, 1},
        }));
        ui_depth_format = depth;
    }
    std::printf("UI: native composition %ux%u (scene %ux%u), independent of FSR history\n",
                w, h, render_width, render_height);
}

void TemporalUpscaler::PrepareUiDepth(VideoCore::ImageId depth_id) {
    scheduler.EndRendering();
    const auto aspect = vk::ImageAspectFlagBits::eDepth |
        (ui_depth_format == vk::Format::eD32SfloatS8Uint ? vk::ImageAspectFlagBits::eStencil
                                                      : vk::ImageAspectFlags{});
    // Copy depth if blit is supported; otherwise just clear it (UI depth test still works).
    const bool copy = depth_id && depth_blit &&
        texture_cache.GetImage(depth_id).info.pixel_format == ui_depth_format;
    if (copy) {
        runtime.Transit(&texture_cache.GetImage(depth_id), vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
    }
    const vk::ImageSubresourceRange range{aspect, 0, 1, 0, 1};
    // Without blit support, UI depth is cleared but not copied from the scene.
    // UI elements will still depth-test correctly against the cleared buffer.
    vk::Image source{};
    vk::ImageBlit region{};
    if (copy) {
        const auto& image = texture_cache.GetImage(depth_id);
        source = vk::Image(image.backing->image);
        region = {
            .srcSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
            .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                vk::Offset3D{s32(image.info.size.width), s32(image.info.size.height), 1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                vk::Offset3D{s32(ui_width), s32(ui_height), 1}},
        };
    }
    // bbport: recorded on the recording thread (direct recording here waited for it to finish
    // everything queued, every frame).
    scheduler.Record([depth = vk::Image(ui_depth_image), range, source,
                      region](vk::CommandBuffer cmd) {
        vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = depth, .subresourceRange = range,
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        cmd.clearDepthStencilImage(depth, vk::ImageLayout::eTransferDstOptimal,
                                   {.depth = 1.0f, .stencil = 0}, range);
        if (source) {
            barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
            barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
            barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
            cmd.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, depth,
                          vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eNearest);
        }
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                               vk::PipelineStageFlagBits2::eLateFragmentTests;
        barrier.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
        barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        barrier.newLayout = vk::ImageLayout::eGeneral;
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    });
}

void TemporalUpscaler::RunUiOnly(VideoCore::ImageId color_id, VideoCore::ImageId depth_id) {
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xF5A1'0000ull, [] { return std::string{"upscaler RunUiOnly"}; });
    }
    const auto& color = texture_cache.GetImage(color_id);
    const auto depth_format = depth_id ? texture_cache.GetImage(depth_id).info.pixel_format
                                      : vk::Format::eD32SfloatS8Uint;
    EnsureUiResources(target_width, target_height, color.info.pixel_format, depth_format);
    PrepareUiDepth(depth_id);
    // Copy only the background already drawn. The subsequent menu/HUD/text draws execute
    // directly at output resolution, including frames with no scene or camera at all.
    vk::Image source = color.GetImage();
    auto source_layout = vk::ImageLayout::eTransferSrcOptimal;
    u32 source_width = color.info.size.width, source_height = color.info.size.height;
    if (!scaled_session && camera_motion.Depth() && scene_targets.Reduced() &&
        scene_targets.EligibleScene(color)) {
        VideoCore::ImageViewInfo view;
        view.format = color.info.pixel_format;
        const auto proxy = scene_targets.Read(color_id, view,
                                              vk::PipelineStageFlagBits2::eTransfer,
                                              vk::AccessFlagBits2::eTransferRead);
        source = proxy.image;
        source_layout = proxy.layout;
        source_width = render_width;
        source_height = render_height;
    } else {
        runtime.Transit(&texture_cache.GetImage(color_id), vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
    }
    const vk::ImageBlit region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
            vk::Offset3D{s32(source_width), s32(source_height), 1}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
            vk::Offset3D{s32(ui_width), s32(ui_height), 1}},
    };
    scheduler.Record([source, source_layout, ui = vk::Image(ui_image),
                      region](vk::CommandBuffer cmd) {
        vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = ui,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        cmd.blitImage(source, source_layout, ui, vk::ImageLayout::eTransferDstOptimal, region,
                      vk::Filter::eLinear);
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        barrier.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                                vk::AccessFlagBits2::eColorAttachmentWrite;
        barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        barrier.newLayout = vk::ImageLayout::eGeneral;
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    });
    reset = true; // returning to 3D must not reuse history from before a menu/loading screen
    done_this_frame = ui_phase = true;
    ui_color = color_id;
    ui_depth = depth_id;
}

void TemporalUpscaler::RunScaled() {
    if (Q0CaptureEnabled()) {
        std::fprintf(stderr, "Q0_ROUTE_BLOCKED route=RunScaled expected=normal_live_HDR_Run "
                            "source_game_frame=%llu profile_match=0\n",
                            (unsigned long long)BbStats::gpu_frames.load(std::memory_order_relaxed));
    }
    if (auto* profiler = GpuProfiler::Get()) {
        const char* label = BbSettings::Get().upscaler == BbSettings::UpscalerTaa
            ? "upscaler RunScaled (TAA)" : "upscaler RunScaled (FSR)";
        profiler->Mark(0xF5A0'0000ull ^ std::hash<std::string_view>{}(label),
                       [label] { return std::string{label}; });
    }
    auto& color = texture_cache.GetImage(ldr_target);
    auto& depth = texture_cache.GetImage(camera_motion.Depth());
    // The scene fills the top-left w x h of its targets (iw x ih, aligned by the game).
    const u32 iw = color.info.size.width, ih = color.info.size.height;
    const auto [w, h] = SceneSize(iw, ih);
    const u32 ow = target_width, oh = target_height;
    if (depth.info.size.width != iw || depth.info.size.height != ih || w > ow || h > oh) {
        return;
    }
    if (!EnsureResources(w, h, ow, oh, false)) {
        failed = true;
        return;
    }
    EnsureUiResources(ow, oh, color.info.pixel_format, depth.info.pixel_format);
    PrepareUiDepth(camera_motion.Depth());
    const auto depth_format = depth.info.pixel_format;
    // Views kept across frames: FSR 4 registers images by view in a registry of eight.
    vk::Image depth_image = depth.GetImage(), color_image = color.GetImage();
    vk::ImageView depth_view{}, color_view{};
    u32 source_width = iw, source_height = ih;
    if (!scaled_session) {
        VideoCore::ImageViewInfo ci, di;
        ci.format = color.info.pixel_format;
        di.format = depth_format;
        const auto cp = scene_targets.Read(ldr_target, ci);
        const auto dp = scene_targets.Read(camera_motion.Depth(), di);
        color_image = cp.image;
        color_view = cp.view;
        depth_image = dp.image;
        depth_view = dp.view;
        source_width = w;
        source_height = h;
    } else {
        depth_view = CachedView(depth, depth_format, vk::ImageAspectFlagBits::eDepth);
        color_view = CachedView(color, color.info.pixel_format, vk::ImageAspectFlagBits::eColor);
        runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }

    scheduler.EndRendering();
    // bbport: the motion pass and FSR 3 are recorded through the scheduler, on a recording
    // thread (recording them here cost the draw recording thread ~5% of its time, plus a wait
    // for the recording thread every frame). FSR 4 and TAA keep their frame bookkeeping on
    // this thread and record directly below.
    const auto barrier = [&](vk::Image image, vk::ImageAspectFlags aspect,
                             vk::ImageLayout old_layout, vk::PipelineStageFlags2 src_stage,
                             vk::AccessFlags2 src_access, vk::ImageLayout new_layout,
                             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        };
        scheduler.Record([b](vk::CommandBuffer cmdbuf) {
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
        });
    };
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto color_access = vk::AccessFlagBits2::eColorAttachmentRead |
                              vk::AccessFlagBits2::eColorAttachmentWrite;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    // The previous frame's display pass read it: all commands before.
    barrier(vk::Image(ui_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
            all, vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral, all, rw);
    barrier(vk::Image(motion_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
            all, vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
    camera_motion.RecordMotion(depth_view, *motion_view, w, h);
    barrier(vk::Image(motion_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
            vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_frame).count();
    if (frame_ms <= 0.0f || frame_ms > 200.0f) {
        frame_ms = 16.6f;
    }
    last_frame = now;

    // bbport: FSR 4 writes its HDR-format output, copied into the output-size UI image.
    if (UseFsr4() || UseDlss() || BbSettings::Get().upscaler == BbSettings::UpscalerTaa) {
        const auto cmdbuf = scheduler.CommandBuffer(); // after the commands recorded above
        barrier(vk::Image(output_image), vk::ImageAspectFlagBits::eColor,
                vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eNone,
                vk::ImageLayout::eGeneral, all, rw);
        bool ok4 = true;
        if (BbSettings::Get().upscaler == BbSettings::UpscalerTaa) {
            RecordTaa(cmdbuf, color_view, depth_view);
        } else if (UseDlss()) {
            fsr4_linear_frame = false;
            ok4 = RecordDlss(cmdbuf,
                             {color_image, color_view, color.info.pixel_format,
                              vk::ImageAspectFlagBits::eColor, source_width, source_height},
                             {depth_image, depth_view, depth_format, vk::ImageAspectFlagBits::eDepth,
                              source_width, source_height},
                             w, h, ow, oh, frame_ms, false);
        } else {
            // bbport: FSR 4 treats its colour as linear light; this frame is the game's
            // tonemapped, sRGB-encoded one. Decoded first, the output encoded again below.
            // BB_FSR4_LINEAR=0 or toggle bit 63: the encoded frame as before (A/B).
            static const bool linear_env = [] {
                const char* env = std::getenv("BB_FSR4_LINEAR");
                return !env || env[0] != '0';
            }();
            // An sRGB-format frame is decoded by its view already (and encoded by the blit).
            const bool srgb_format =
                vk::to_string(color.info.pixel_format).find("Srgb") != std::string::npos;
            fsr4_linear_frame = linear_env && fsr4_linear_image && !srgb_format &&
                                !BbToggle::Disabled(BbToggle::Fsr4EncodedInput);
            Fsr4Upscaler::Image input{color_image, color_view, source_width, source_height};
            {
                static int reported = -1;
                if (int(fsr4_linear_frame) != reported) {
                    reported = int(fsr4_linear_frame);
                    std::printf("Upscaler: FSR 4 input %s (%s)\n",
                                fsr4_linear_frame ? "decoded to linear light" : "as it is",
                                vk::to_string(color.info.pixel_format).c_str());
                }
            }
            if (fsr4_linear_frame) {
                const vk::ImageMemoryBarrier2 to_write{
                    .srcStageMask = all, .srcAccessMask = vk::AccessFlagBits2::eNone,
                    .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                    .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral,
                    .image = vk::Image(fsr4_linear_image),
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
                const vk::MemoryBarrier2 source_ready{
                    .srcStageMask = all, .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead};
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &source_ready,
                                         .imageMemoryBarrierCount = 1,
                                         .pImageMemoryBarriers = &to_write});
                const vk::DescriptorImageInfo src{.imageView = color_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
                const vk::DescriptorImageInfo dst{.imageView = *fsr4_linear_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
                const std::array<vk::WriteDescriptorSet, 2> writes{{
                    {.dstBinding = 0, .descriptorCount = 1,
                     .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &src},
                    {.dstBinding = 1, .descriptorCount = 1,
                     .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &dst},
                }};
                const u32 unused = 0;
                cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *fsr4_decode_pipeline);
                cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
                                            *fsr4_decode_pipeline_layout, 0, writes);
                cmdbuf.pushConstants(*fsr4_decode_pipeline_layout,
                                     vk::ShaderStageFlagBits::eCompute, 0, sizeof(unused), &unused);
                cmdbuf.dispatch((source_width + 7) / 8, (source_height + 7) / 8, 1);
                const vk::MemoryBarrier2 decoded{
                    .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                    .dstStageMask = all,
                    .dstAccessMask = vk::AccessFlagBits2::eShaderRead};
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &decoded});
                input = {vk::Image(fsr4_linear_image), *fsr4_linear_view, source_width,
                         source_height};
            }
            ok4 = RecordFsr4(cmdbuf, input,
                            {depth_image, depth_view, source_width, source_height}, w, h, ow,
                            oh, frame_ms);
            if (ok4 && fsr4_linear_frame) {
                const vk::MemoryBarrier2 upscaled{
                    .srcStageMask = all, .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead |
                                     vk::AccessFlagBits2::eShaderStorageWrite};
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &upscaled});
                const vk::DescriptorImageInfo out{.imageView = *output_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
                const vk::WriteDescriptorSet write{.dstBinding = 0, .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &out};
                const u32 unused = 0;
                cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *fsr4_encode_pipeline);
                cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
                                            *fsr4_encode_pipeline_layout, 0, write);
                cmdbuf.pushConstants(*fsr4_encode_pipeline_layout,
                                     vk::ShaderStageFlagBits::eCompute, 0, sizeof(unused), &unused);
                cmdbuf.dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
                const vk::MemoryBarrier2 encoded{
                    .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                    .dstStageMask = all,
                    .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                     vk::AccessFlagBits2::eMemoryWrite};
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &encoded});
            }
            // The game's (encoded) frame, as the output is encoded again by now.
            if (ok4 && mask_ready) {
                RecordFsr4Reactive(cmdbuf, color_view, w, h, ow, oh);
            }
        }
        if (ok4) {
            if (BbSettings::Get().upscaler != BbSettings::UpscalerTaa) {
                ExtraSharpen(vk::Image(output_image), false, ow, oh);
            }
            barrier(vk::Image(output_image), vk::ImageAspectFlagBits::eColor,
                    vk::ImageLayout::eGeneral, all, rw, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferRead);
            barrier(vk::Image(ui_image), vk::ImageAspectFlagBits::eColor,
                    vk::ImageLayout::eGeneral, all, rw, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite);
            const vk::ImageBlit region{
                .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                         vk::Offset3D{s32(ow), s32(oh), 1}},
                .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                         vk::Offset3D{s32(ow), s32(oh), 1}},
            };
            cmdbuf.blitImage(vk::Image(output_image), vk::ImageLayout::eGeneral,
                             vk::Image(ui_image), vk::ImageLayout::eGeneral, region,
                             vk::Filter::eNearest);
            barrier(vk::Image(ui_image), vk::ImageAspectFlagBits::eColor,
                    vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eBlit,
                    vk::AccessFlagBits2::eTransferWrite, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eColorAttachmentOutput, color_access);
            reset = false;
            dispatched_last_frame = true;
            if (const int dump = DumpFrame(); dump >= 0) {
                camera_motion.PrintState(dump);
                DumpImages(instance, scheduler, cmdbuf, dump,
                           {{color_image, source_width, source_height, 4, "input", "rgba"},
                            {vk::Image(motion_image), w, h, 4, "motion", "rg16f"},
                            {vk::Image(output_image), ow, oh, 8, "output", "rgba16f"}});
                DumpImages(instance, scheduler, cmdbuf, dump,
                           {{depth_image, source_width, source_height, 4, "depth", "f32",
                             vk::ImageAspectFlagBits::eDepth}});
                if (BbSettings::Get().upscaler == BbSettings::UpscalerTaa &&
                    std::getenv("BB_TAA_DIAGNOSTICS")) {
                    DumpImages(instance, scheduler, cmdbuf, dump,
                               {{vk::Image(taa_history[taa_next]),ow,oh,16,
                                 "previous_history","rgba32f"},
                                {vk::Image(opaque_image),ow,oh,8,"history_sample","rgba16f"}});
                }
                // ObjectMotion's attachment can be in a different resolution from the FSR
                // inputs: only dump it when it matches, to avoid an out-of-bounds GPU copy.
                if (const auto object = camera_motion.ObjectMotionImage(w, h); object) {
                    DumpImages(instance, scheduler, cmdbuf, dump,
                               {{object, w, h, 16, "objects", "rgba32f"}});
                }
            }
        }
        done_this_frame = true;
        if (ok4) {
            ui_phase = true;
            ui_color = ldr_target;
            ui_depth = camera_motion.Depth();
        }
        return;
    }

    const auto& settings = BbSettings::Get();
    FfxVkPortableUpscaleDispatchInfo info{};
    info.structSize = sizeof(info);
    info.color = Describe(color_image, color.info.pixel_format, source_width, source_height,
                          color.usage_flags, vk::ImageAspectFlagBits::eColor,
                          FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.depth = Describe(depth_image, depth_format, source_width, source_height, depth.usage_flags,
                          vk::ImageAspectFlagBits::eDepth,
                          FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.motionVectors = Describe(vk::Image(motion_image), vk::Format::eR16G16Sfloat, w, h,
                                  vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                  vk::ImageAspectFlagBits::eColor,
                                  FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.output = Describe(vk::Image(ui_image), vk::Format::eR8G8B8A8Unorm, ow, oh,
                           vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                               vk::ImageUsageFlagBits::eColorAttachment,
                           vk::ImageAspectFlagBits::eColor,
                           FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    info.exposure.structSize = sizeof(info.exposure);
    info.reactiveMask.structSize = sizeof(info.reactiveMask);
    if (mask_ready && ReactiveOn()) {
        info.reactiveMask = Describe(vk::Image(reactive_image), vk::Format::eR8Unorm, w, h,
            vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            vk::ImageAspectFlagBits::eColor, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    }

    info.transparencyAndCompositionMask.structSize = sizeof(info.transparencyAndCompositionMask);
    const float sign = BbToggle::Disabled(1u << 26) ? -1.0f : 1.0f;
    info.jitterOffset = {sign * jitter[0], sign * jitter[1]};
    info.motionVectorScale = {1.0f, 1.0f};
    info.renderSize = {w, h};
    info.outputSize = {ow, oh};
    info.frameTimeMilliseconds = frame_ms;
    info.preExposure = 1.0f;
    info.cameraNear = camera_motion.Near();
    info.cameraFar = 3000.0f;
    info.cameraVerticalFovRadians = camera_motion.VerticalFov();
    info.viewSpaceToMeters = 1.0f;
    info.sharpness = std::min(settings.sharpness.load(), 1.0f);
    info.enableSharpening = settings.sharpen ? VK_TRUE : VK_FALSE;
    info.reset = reset ? VK_TRUE : VK_FALSE;
    info.frameId = frame_id++;

    FfxVkPortableUpscaleCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    create_info.maxRenderSize = {w, h};
    create_info.maxOutputSize = {ow, oh};
    // The command buffer is the recording thread's, known when the dispatch is recorded: the
    // check sees a placeholder.
    auto checked = info;
    checked.commandBuffer = reinterpret_cast<VkCommandBuffer>(uintptr_t{1});
    bool ok = false;
    if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&create_info, &checked)) {
        PrintIssues("scaled dispatch", issues);
        failed = true;
    } else {
        // The context is only used here and destroyed after scheduler.Finish(); a dispatch
        // that fails sets `failed` at the next frame (the UI then draws over this one).
        scheduler.Record([this, ctx = context, info](vk::CommandBuffer cmdbuf) mutable {
            info.commandBuffer = cmdbuf;
            if (ffxVkPortableUpscaleContextRecordDispatch(ctx, &info) != FFX_VK_PORTABLE_OK) {
                std::printf("Upscaler: FSR 3 scaled dispatch failed\n");
                dispatch_failed.store(true, std::memory_order_relaxed);
            }
        });
        ok = true;
        reset = false;
        dispatched_last_frame = true;
        ExtraSharpen(vk::Image(ui_image), true, ow, oh);
    }
    barrier(vk::Image(ui_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral, all,
            rw, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            color_access);

    done_this_frame = true; // the UI is not jittered
    if (ok) {
        ui_phase = true;
        ui_color = ldr_target;
        ui_depth = camera_motion.Depth();
    }
}

vk::ImageView TemporalUpscaler::Mirror(vk::Image image, std::vector<MirrorView>& views,
                                       vk::Format format, vk::ComponentMapping mapping) {
    for (const auto& entry : views) {
        if (entry.format == format && entry.mapping == mapping) {
            return *entry.view;
        }
    }
    const vk::ImageViewUsageCreateInfo usage{
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment,
    };
    auto view = Check(instance.GetDevice().createImageViewUnique({
        .pNext = &usage,
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .components = mapping,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    const vk::ImageView handle = *view;
    views.push_back({format, mapping, std::move(view)});
    return handle;
}

bool TemporalUpscaler::RedirectColor(VideoCore::ImageId color,
                                     const VideoCore::ImageViewInfo& view_info, Target& target) {
    if (ui_phase && color == ui_color) {
        target = {Mirror(vk::Image(ui_image), ui_views, view_info.format, {}),
                  vk::ImageLayout::eGeneral, ui_width, ui_height, true};
        return true;
    }
    const auto& image = texture_cache.GetImage(color);
    const VAddr address = image.info.guest_address;
    if (!FrameCapture::IsDisplayBuffer(address)) {
        display_redirect = false;
        return false;
    }
    std::scoped_lock lock{display_mutex};
    auto& display = displays[address];
    if (!display_redirect) {
        display.valid = false; // not upscaled: the presenter shows the guest buffer
        display.c1_audit = {};
        return false;
    }
    if (!display.image || display.format != image.info.pixel_format ||
        display.width != ui_width || display.height != ui_height) {
        const auto device = instance.GetDevice();
        scheduler.Finish();
        display.views.clear();
        std::printf("Display: host buffer %ux%u (live)\n", ui_width, ui_height);
        display.format = image.info.pixel_format;
        display.width = ui_width;
        display.height = ui_height;
        display.image = VideoCore::UniqueImage(device, instance.GetAllocator());
        display.image.Create(vk::ImageCreateInfo{
            .flags = vk::ImageCreateFlagBits::eMutableFormat,
            .imageType = vk::ImageType::e2D,
            .format = display.format,
            .extent = {ui_width, ui_height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eTransferSrc,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        display.views.clear();
    }
    scheduler.EndRendering();
    const vk::ImageMemoryBarrier2 b{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(display.image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    scheduler.Record([b](vk::CommandBuffer cmd) {
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    });
    display.valid = true;
    if (c1_pending_source_game_frame != 0) {
        display.c1_audit = {
            .source_game_frame = c1_pending_source_game_frame,
            .merge_tick = c1_pending_merge_tick,
            .buffer_address = address,
            .buffer_prepare_gpu_frame = BbStats::gpu_frames.load(std::memory_order_relaxed),
            .buffer_prepare_tick = scheduler.CurrentTick(),
        };
        std::printf("C1_DISPLAY_IMAGE_BIND source_game_frame=%llu merge_tick=%llu "
                    "buffer_address=0x%llx prepare_gpu_frame=%llu prepare_tick=%llu\n",
                    static_cast<unsigned long long>(display.c1_audit.source_game_frame),
                    static_cast<unsigned long long>(display.c1_audit.merge_tick),
                    static_cast<unsigned long long>(address),
                    static_cast<unsigned long long>(display.c1_audit.buffer_prepare_gpu_frame),
                    static_cast<unsigned long long>(display.c1_audit.buffer_prepare_tick));
        c1_pending_source_game_frame = 0;
        c1_pending_merge_tick = 0;
    }
    target = {Mirror(vk::Image(display.image), display.views, view_info.format, {}),
              vk::ImageLayout::eGeneral, ui_width, ui_height};
    return true;
}

bool TemporalUpscaler::RedirectDepth(VideoCore::ImageId depth, Target& target) {
    if (!ui_phase || depth != ui_depth) {
        return false;
    }
    target = {*ui_depth_view, vk::ImageLayout::eGeneral, ui_width, ui_height, true};
    return true;
}

bool TemporalUpscaler::RedirectSampled(VideoCore::ImageId image,
                                       const VideoCore::ImageViewInfo& info, vk::ImageView& view,
                                       vk::ImageLayout& layout) {
    if (!display_redirect || image != ui_color) {
        return false;
    }
    if (!ui_read_barrier) {
        ui_read_barrier = true;
        scheduler.EndRendering();
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(ui_image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        scheduler.Record([b](vk::CommandBuffer cmd) {
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
        });
    }
    view = Mirror(vk::Image(ui_image), ui_views, info.format, info.mapping);
    layout = vk::ImageLayout::eGeneral;
    return true;
}

bool TemporalUpscaler::DisplayOverride(VAddr address, Display& display) {
    std::scoped_lock lock{display_mutex};
    const auto it = displays.find(address);
    if (it == displays.end() || !it->second.valid) {
        return false;
    }
    display = {vk::Image(it->second.image), it->second.format, it->second.width,
               it->second.height, it->second.c1_audit};
    if (display.c1_audit.source_game_frame != 0) {
        display.c1_audit.buffer_address = address;
        DumpPresented(display.image, display.width, display.height, display.format,
                      display.c1_audit);
        std::printf("C1_PRESENT_DUMP_SCHEDULED source_game_frame=%llu merge_tick=%llu "
                    "buffer_address=0x%llx prepare_gpu_frame=%llu prepare_tick=%llu\n",
                    static_cast<unsigned long long>(display.c1_audit.source_game_frame),
                    static_cast<unsigned long long>(display.c1_audit.merge_tick),
                    static_cast<unsigned long long>(address),
                    static_cast<unsigned long long>(display.c1_audit.buffer_prepare_gpu_frame),
                    static_cast<unsigned long long>(display.c1_audit.buffer_prepare_tick));
        it->second.c1_audit = {};
    } else if (PresentDumpDue()) {
        DumpPresented(display.image, display.width, display.height, display.format);
    }
    return true;
}

bool TemporalUpscaler::PresentDumpDue() {
    // Diagnostic capture of the actual completed display buffer, including native UI.
    // BB_PRESENT_DUMP_COUNT=N: the trigger dumps N consecutive frames (temporal stability).
    static const char* dump = std::getenv("BB_PRESENT_DUMP_TRIGGER");
    static const int dump_count = [] {
        const char* env = std::getenv("BB_PRESENT_DUMP_COUNT");
        return env ? std::max(1, std::atoi(env)) : 1;
    }();
    if (dump && present_dump_remaining == 0 && std::remove(dump) == 0) {
        present_dump_remaining = dump_count;
    }
    if (present_dump_remaining == 0) {
        return false;
    }
    --present_dump_remaining;
    return true;
}

bool TemporalUpscaler::TakeC1PresentAudit(VAddr address, u64 present_frame_counter_before,
                                          u64 gpu_frame_at_prepare, u64 prepare_tick,
                                          C1PresentAudit& audit) {
    std::scoped_lock lock{c1_audit_mutex};
    if(native_pre_sr_pending_audit){
        const auto& pending=*native_pre_sr_pending_audit;
        if(gpu_frame_at_prepare!=pending.source_game_frame+1||prepare_tick<pending.merge_tick){
            std::fprintf(stderr,"NATIVE_PRE_SR_PRESENT_ASSOCIATION_REJECTED source=%llu actual_prepare_frame=%llu counted_valid=0\n",
                (unsigned long long)pending.source_game_frame,(unsigned long long)gpu_frame_at_prepare);
            native_pre_sr.RejectAssociation();native_pre_sr_pending_audit.reset();audit={};return false;
        }
        audit=pending;audit.buffer_address=address;audit.buffer_prepare_gpu_frame=gpu_frame_at_prepare;audit.buffer_prepare_tick=prepare_tick;audit.present_frame_counter_before=present_frame_counter_before;
        std::printf("NATIVE_PRE_SR_PRESENT_BIND session=%s source_game_frame=%llu job=%u merge_tick=%llu gpu_frame_at_prepare=%llu guest_buffer_address=0x%llx prepare_tick=%llu association=next_nonupscaled_videoout_prepare_after_merge\n",
            audit.session.c_str(),(unsigned long long)audit.source_game_frame,audit.nr_frame,(unsigned long long)audit.merge_tick,
            (unsigned long long)gpu_frame_at_prepare,(unsigned long long)address,(unsigned long long)prepare_tick);
        native_pre_sr_pending_audit.reset();return true;
    }
    if(native_async_pending_audit) {
        const auto& pending=*native_async_pending_audit;
        if(gpu_frame_at_prepare!=pending.source_game_frame+1||prepare_tick<pending.merge_tick) {
            native_async_snapshot.RejectAssociation();native_async_pending_audit.reset();audit={};
            std::fprintf(stderr,"NATIVE_ASYNC_ASSOCIATION_REJECTED current_SR=1 edited=0 counted_valid=0\n");return false;
        }
        audit=pending;audit.buffer_address=address;audit.buffer_prepare_gpu_frame=gpu_frame_at_prepare;
        audit.buffer_prepare_tick=prepare_tick;audit.present_frame_counter_before=present_frame_counter_before;
        std::printf("NATIVE_ASYNC_GUEST_BIND source_game_frame=%llu gpu_frame_at_prepare=%llu merge_tick=%llu prepare_tick=%llu guest_address=0x%llx edited=0\n",
            (unsigned long long)audit.source_game_frame,(unsigned long long)gpu_frame_at_prepare,(unsigned long long)audit.merge_tick,
            (unsigned long long)prepare_tick,(unsigned long long)address);
        native_async_pending_audit.reset();return true;
    }
    if (p0_pending_audit) {
        // HDR Run records the completed scene before the display-pass NoteFrameStart
        // increments gpu_frames (vk_rasterizer.cpp). Require that immediate epoch;
        // a later VideoOut buffer must never inherit this older NR ledger.
        const auto& pending = *p0_pending_audit;
        if (gpu_frame_at_prepare != pending.source_game_frame + 1 ||
            prepare_tick < pending.merge_tick) {
            std::fprintf(stderr, "P0_PRESENT_ASSOCIATION_REJECTED stage=P0 mode=%s session=%s frame=%u source_game_frame=%llu expected_gpu_frame=%llu actual_gpu_frame=%llu merge_tick=%llu prepare_tick=%llu counted_valid=0\n",
                pending.mode.c_str(), pending.session.c_str(), pending.nr_frame,
                (unsigned long long)pending.source_game_frame,
                (unsigned long long)(pending.source_game_frame + 1),
                (unsigned long long)gpu_frame_at_prepare,
                (unsigned long long)pending.merge_tick, (unsigned long long)prepare_tick);
            p0_controller.RejectPresentationAssociation();
            p0_pending_audit.reset();
            audit = {};
            return false;
        }
        audit = pending;
        audit.buffer_address = address;
        audit.buffer_prepare_gpu_frame = gpu_frame_at_prepare;
        audit.buffer_prepare_tick = prepare_tick;
        audit.present_frame_counter_before = present_frame_counter_before;
        std::printf("P0_GUEST_BUFFER_BIND stage=P0 mode=%s session=%s frame=%u route=nonupscaled_videoout source_game_frame=%llu merge_tick=%llu guest_buffer_address=0x%llx gpu_frame_at_prepare=%llu frame_counter_before=%llu prepare_tick=%llu selected_output=%u audit_kind=%s association=next_nonupscaled_videoout_prepare_after_merge\n",
            audit.mode.c_str(), audit.session.c_str(), audit.nr_frame,
            (unsigned long long)audit.source_game_frame, (unsigned long long)audit.merge_tick,
            (unsigned long long)address, (unsigned long long)gpu_frame_at_prepare,
            (unsigned long long)present_frame_counter_before, (unsigned long long)prepare_tick,
            audit.selected_output, audit.audit_full ? "full" : "off");
        p0_pending_audit.reset();
        return true;
    }
    if (q1_pending_audit) {
        // HDR Run records the completed scene before the display-pass NoteFrameStart
        // increments gpu_frames (vk_rasterizer.cpp). Require that immediate epoch;
        // a later VideoOut buffer must never inherit this older NR ledger.
        const auto& pending = *q1_pending_audit;
        if (gpu_frame_at_prepare != pending.source_game_frame + 1 ||
            prepare_tick < pending.merge_tick) {
            std::fprintf(stderr, "Q1_PRESENT_ASSOCIATION_REJECTED stage=Q1 mode=%s session=%s frame=%u source_game_frame=%llu expected_gpu_frame=%llu actual_gpu_frame=%llu merge_tick=%llu prepare_tick=%llu counted_valid=0\n",
                pending.mode.c_str(), pending.session.c_str(), pending.nr_frame,
                (unsigned long long)pending.source_game_frame,
                (unsigned long long)(pending.source_game_frame + 1),
                (unsigned long long)gpu_frame_at_prepare,
                (unsigned long long)pending.merge_tick, (unsigned long long)prepare_tick);
            q1_game_bridge.RejectPresentationAssociation();
            q1_pending_audit.reset();
            audit = {};
            return false;
        }
        audit = pending;
        audit.buffer_address = address;
        audit.buffer_prepare_gpu_frame = gpu_frame_at_prepare;
        audit.buffer_prepare_tick = prepare_tick;
        audit.present_frame_counter_before = present_frame_counter_before;
        std::printf("Q1_GUEST_BUFFER_BIND stage=Q1 mode=%s session=%s frame=%u route=nonupscaled_videoout source_game_frame=%llu merge_tick=%llu guest_buffer_address=0x%llx gpu_frame_at_prepare=%llu frame_counter_before=%llu prepare_tick=%llu selected_output=%u audit_kind=%s association=next_nonupscaled_videoout_prepare_after_merge\n",
            audit.mode.c_str(), audit.session.c_str(), audit.nr_frame,
            (unsigned long long)audit.source_game_frame, (unsigned long long)audit.merge_tick,
            (unsigned long long)address, (unsigned long long)gpu_frame_at_prepare,
            (unsigned long long)present_frame_counter_before, (unsigned long long)prepare_tick,
            audit.selected_output, audit.audit_full ? "full" : "off");
        q1_pending_audit.reset();
        return true;
    }
    if (c4_pending_audit) {
        // HDR Run records the completed scene before the display-pass NoteFrameStart
        // increments gpu_frames (vk_rasterizer.cpp). Require that immediate epoch;
        // a later VideoOut buffer must never inherit this older NR ledger.
        const auto& pending = *c4_pending_audit;
        if (gpu_frame_at_prepare != pending.source_game_frame + 1 ||
            prepare_tick < pending.merge_tick) {
            std::fprintf(stderr, "C4_PRESENT_ASSOCIATION_REJECTED stage=C4 mode=%s session=%s frame=%u source_game_frame=%llu expected_gpu_frame=%llu actual_gpu_frame=%llu merge_tick=%llu prepare_tick=%llu counted_valid=0\n",
                pending.mode.c_str(), pending.session.c_str(), pending.nr_frame,
                (unsigned long long)pending.source_game_frame,
                (unsigned long long)(pending.source_game_frame + 1),
                (unsigned long long)gpu_frame_at_prepare,
                (unsigned long long)pending.merge_tick, (unsigned long long)prepare_tick);
            c4_game_bridge.RejectPresentationAssociation();
            c4_pending_audit.reset();
            audit = {};
            return false;
        }
        audit = pending;
        audit.buffer_address = address;
        audit.buffer_prepare_gpu_frame = gpu_frame_at_prepare;
        audit.buffer_prepare_tick = prepare_tick;
        audit.present_frame_counter_before = present_frame_counter_before;
        std::printf("C4_GUEST_BUFFER_BIND stage=C4 mode=%s session=%s frame=%u route=nonupscaled_videoout source_game_frame=%llu merge_tick=%llu guest_buffer_address=0x%llx gpu_frame_at_prepare=%llu frame_counter_before=%llu prepare_tick=%llu selected_output=%u audit_kind=%s association=next_nonupscaled_videoout_prepare_after_merge\n",
            audit.mode.c_str(), audit.session.c_str(), audit.nr_frame,
            (unsigned long long)audit.source_game_frame, (unsigned long long)audit.merge_tick,
            (unsigned long long)address, (unsigned long long)gpu_frame_at_prepare,
            (unsigned long long)present_frame_counter_before, (unsigned long long)prepare_tick,
            audit.selected_output, audit.audit_full ? "full" : "off");
        c4_pending_audit.reset();
        return true;
    }
    if (c3_pending_audit) {
        // HDR Run records the completed scene before the display-pass NoteFrameStart
        // increments gpu_frames (vk_rasterizer.cpp). Require that immediate epoch;
        // a later VideoOut buffer must never inherit this older NR ledger.
        const auto& pending = *c3_pending_audit;
        if (gpu_frame_at_prepare != pending.source_game_frame + 1 ||
            prepare_tick < pending.merge_tick) {
            std::fprintf(stderr, "C3_PRESENT_ASSOCIATION_REJECTED stage=C3 mode=%s session=%s frame=%u source_game_frame=%llu expected_gpu_frame=%llu actual_gpu_frame=%llu merge_tick=%llu prepare_tick=%llu counted_valid=0\n",
                pending.mode.c_str(), pending.session.c_str(), pending.nr_frame,
                (unsigned long long)pending.source_game_frame,
                (unsigned long long)(pending.source_game_frame + 1),
                (unsigned long long)gpu_frame_at_prepare,
                (unsigned long long)pending.merge_tick, (unsigned long long)prepare_tick);
            c3_game_bridge.RejectPresentationAssociation();
            c3_pending_audit.reset();
            audit = {};
            return false;
        }
        audit = pending;
        audit.buffer_address = address;
        audit.buffer_prepare_gpu_frame = gpu_frame_at_prepare;
        audit.buffer_prepare_tick = prepare_tick;
        audit.present_frame_counter_before = present_frame_counter_before;
        std::printf("C3_GUEST_BUFFER_BIND stage=C3 mode=%s session=%s frame=%u route=nonupscaled_videoout source_game_frame=%llu merge_tick=%llu guest_buffer_address=0x%llx gpu_frame_at_prepare=%llu frame_counter_before=%llu prepare_tick=%llu selected_output=%u audit_kind=%s association=next_nonupscaled_videoout_prepare_after_merge\n",
            audit.mode.c_str(), audit.session.c_str(), audit.nr_frame,
            (unsigned long long)audit.source_game_frame, (unsigned long long)audit.merge_tick,
            (unsigned long long)address, (unsigned long long)gpu_frame_at_prepare,
            (unsigned long long)present_frame_counter_before, (unsigned long long)prepare_tick,
            audit.selected_output, audit.audit_full ? "full" : "gpu_full_scan");
        c3_pending_audit.reset();
        return true;
    }
    if (c2_pending_source_game_frame != 0) {
        audit = {
            .stage = "C2", .mode = c2_pending_mode, .session = c2_pending_session,
            .selected_output = c2_pending_selected_output,
            .source_game_frame = c2_pending_source_game_frame,
            .merge_tick = c2_pending_merge_tick,
            .buffer_address = address,
            .buffer_prepare_gpu_frame = gpu_frame_at_prepare,
            .buffer_prepare_tick = prepare_tick,
            .present_frame_counter_before = present_frame_counter_before,
        };
        std::printf("C2_GUEST_BUFFER_BIND stage=C2 mode=%s session=%s route=nonupscaled_videoout "
                    "source_game_frame=%llu merge_tick=%llu guest_buffer_address=0x%llx "
                    "gpu_frame_at_prepare=%llu frame_counter_before=%llu prepare_tick=%llu "
                    "selected_output=%u association=next_nonupscaled_videoout_prepare_after_merge\n",
            audit.mode.c_str(), audit.session.c_str(),
            static_cast<unsigned long long>(audit.source_game_frame),
            static_cast<unsigned long long>(audit.merge_tick),
            static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(gpu_frame_at_prepare),
            static_cast<unsigned long long>(present_frame_counter_before),
            static_cast<unsigned long long>(prepare_tick), audit.selected_output);
        c2_pending_source_game_frame = 0;
        c2_pending_merge_tick = 0;
        c2_pending_mode.clear();
        c2_pending_session.clear();
        c2_pending_selected_output = false;
        return true;
    }
    if (c1_pending_source_game_frame == 0) return false;
    audit = {
        .stage = "C1", .mode = "noop", .session = c1_noop_process.Session(),
        .selected_output = true,
        .source_game_frame = c1_pending_source_game_frame,
        .merge_tick = c1_pending_merge_tick,
        .buffer_address = address,
        .buffer_prepare_gpu_frame = gpu_frame_at_prepare,
        .buffer_prepare_tick = prepare_tick,
        .present_frame_counter_before = present_frame_counter_before,
    };
    std::printf("C1_GUEST_BUFFER_BIND route=nonupscaled_videoout source_game_frame=%llu merge_tick=%llu "
                "guest_buffer_address=0x%llx gpu_frame_at_prepare=%llu "
                "frame_counter_before=%llu prepare_tick=%llu "
                "association=next_nonupscaled_videoout_prepare_after_merge\n",
                static_cast<unsigned long long>(audit.source_game_frame),
                static_cast<unsigned long long>(audit.merge_tick),
                static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(gpu_frame_at_prepare),
                static_cast<unsigned long long>(present_frame_counter_before),
                static_cast<unsigned long long>(prepare_tick));
    c1_pending_source_game_frame = 0;
    c1_pending_merge_tick = 0;
    return true;
}

void TemporalUpscaler::DumpPresented(vk::Image image, u32 width, u32 height, vk::Format format,
                                    std::optional<C1PresentAudit> c1_audit) {
    scheduler.EndRendering();
    const bool bgra = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
    const int dump_frame = present_dump_index++;
    const u64 recorded_tick = scheduler.CurrentTick();
    if (c1_audit && c1_audit->source_game_frame != 0) {
        std::printf("%s_PRESENT_DUMP_SCHEDULED stage=%s mode=%s session=%s selected_output=%u dump_frame=%d source_game_frame=%llu "
                    "merge_tick=%llu guest_buffer_address=0x%llx "
                    "gpu_frame_at_prepare=%llu guest_prepare_tick=%llu "
                    "dump_recorded_tick=%llu route=nonupscaled_videoout\n",
                    c1_audit->stage.c_str(), c1_audit->stage.c_str(), c1_audit->mode.c_str(),
                    c1_audit->session.c_str(), c1_audit->selected_output,
                    dump_frame, static_cast<unsigned long long>(c1_audit->source_game_frame),
                    static_cast<unsigned long long>(c1_audit->merge_tick),
                    static_cast<unsigned long long>(c1_audit->buffer_address),
                    static_cast<unsigned long long>(c1_audit->buffer_prepare_gpu_frame),
                    static_cast<unsigned long long>(c1_audit->buffer_prepare_tick),
                    static_cast<unsigned long long>(recorded_tick));
    }
    DumpImages(instance, scheduler, scheduler.CommandBuffer(), dump_frame,
               {{image, width, height, 4, "present", bgra ? "bgra" : "rgba"}}, c1_audit);
}

} // namespace Vulkan

namespace Vulkan {

bool TemporalUpscaler::UseFsr4() const {
    const int selected = BbSettings::Get().upscaler;
    const bool supported = selected == BbSettings::UpscalerFsr411
                               ? instance.IsFsr411Supported()
                               : instance.IsFsr4Int8Supported();
    return BbSettings::IsFsr4(selected) && supported && !fsr4_failed;
}

bool TemporalUpscaler::RecordFsr4(vk::CommandBuffer cmdbuf, Fsr4Upscaler::Image color,
                                  Fsr4Upscaler::Image depth, u32 w, u32 h, u32 ow, u32 oh,
                                  float frame_ms) {
    const auto& settings = BbSettings::Get();
    // Same jitter convention as FSR 3; the menu (or toggle 1 << 26) flips it for tests.
    const float sign =
        BbToggle::Disabled(1u << 26) != settings.fsr4_invert_jitter.load() ? -1.0f : 1.0f;
    const bool ok = fsr4->Record({
        .cmdbuf = cmdbuf,
        .color = color,
        .depth = depth,
        .motion = {vk::Image(motion_image), *motion_view, w, h},
        .output = {vk::Image(output_image), *output_view, ow, oh},
        .render_width = w,
        .render_height = h,
        .preset = applied_preset,
        .jitter = {sign * jitter[0], sign * jitter[1]},
        .frame_ms = frame_ms,
        .near_plane = camera_motion.Near(),
        .far_plane = 3000.0f,
        .vertical_fov = camera_motion.VerticalFov(),
        .sharpness = std::min(settings.sharpness.load(), 1.0f),
        .sharpen = settings.sharpen,
        .reset = reset,
        .auto_exposure = settings.fsr4_auto_exposure,
    });
    // The menu shows the reason; it outlives this frame (FSR 4 keeps its last message).
    static std::string shown;
    const char* problem = fsr4->Problem();
    if (!problem) {
        BbSettings::Get().fsr4_problem = nullptr;
    } else if (shown != problem) {
        shown = problem;
        static std::array<std::string, 8> kept;
        static u32 next = 0;
        kept[next] = shown;
        BbSettings::Get().fsr4_problem = kept[next].c_str();
        next = (next + 1) % kept.size();
    }
    if (!ok && fsr4->Fatal()) {
        std::printf("Upscaler: falling back to FSR 3.1\n");
        BbSettings::Get().upscaler = BbSettings::UpscalerFsr3;
        fsr4_failed = true; // EnsureResources creates the FSR 3 context next frame
        reset = true;
    }
    return ok;
}

bool TemporalUpscaler::UseDlss() const {
    const Dlss* dlss = Dlss::Get();
    return BbSettings::Get().upscaler == BbSettings::UpscalerDlss && dlss && dlss->Available() &&
           !dlss_failed;
}

bool TemporalUpscaler::RecordDlss(vk::CommandBuffer cmdbuf, const Dlss::Resource& color,
                                  const Dlss::Resource& depth, u32 w, u32 h, u32 ow, u32 oh,
                                  float frame_ms, bool hdr) {
    Dlss* dlss = Dlss::Get();
    const Dlss::FeatureDesc desc{w, h, ow, oh, Dlss::QualityForScale(float(ow) / float(w)), hdr};
    bool ok = true;
    if (!dlss->HasFeature(desc)) {
        // The previous feature may still be in use by submitted work; `cmdbuf` stays open.
        scheduler.WaitSubmittedWork();
        dlss->ReleaseFeature();
        ok = dlss->CreateFeature(cmdbuf, desc);
        reset = true;
    }
    if (ok) {
        // Capture the exact values passed to Evaluate; feature recreation above may set reset.
        const float sign = BbToggle::Disabled(1u << 26) ? -1.0f : 1.0f;
        const auto& settings = BbSettings::Get();
        last_dlss_called = true;
        last_dlss_hdr = hdr;
        last_dlss_reset = reset;
        last_dlss_jitter_x = sign * jitter[0];
        last_dlss_jitter_y = sign * jitter[1];
        last_dlss_frame_ms = frame_ms;
        Dlss::Resource sr_color=color;
        const u64 pre_source=BbStats::gpu_frames.load(std::memory_order_relaxed);
        const bool pre_requested_reset=reset;
        if(native_pre_sr.Enabled()){
            const auto cc=scene_targets.CaptureCreateInfo(color.image);
            const auto dc=scene_targets.CaptureCreateInfo(depth.image);
            bool pending=false;{std::scoped_lock lock{c1_audit_mutex};pending=native_pre_sr_pending_audit.has_value();}
            const bool profile=hdr&&!Scaled()&&w==1280&&h==720&&ow==1920&&oh==1080&&
                BbSettings::Get().preset==1&&BbSettings::Get().jitter&&cc&&dc&&!pending;
            vk::ImageView edited{};
            if(profile)edited=native_pre_sr.TryCurrent(instance,scheduler,
                {color.image,depth.image,vk::Image(motion_image)},{*cc,*dc,motion_image.image_ci},
                {.source=pre_source,.sr_reset=pre_requested_reset,
                 .viewport=camera_motion.CurrentJitter(),.previous_viewport=camera_motion.PreviousJitter(),
                 .sr_jitter={sign*jitter[0],sign*jitter[1]},
                 .previous_sr_jitter={previous_dlss_jitter_x,previous_dlss_jitter_y}});
            cmdbuf=scheduler.CommandBuffer();
            if(edited&&!native_pre_sr.ConfirmBeforeSr(pre_source))edited=vk::ImageView{};
            const bool nr=bool(edited);
            if(nr)sr_color={native_pre_sr.ReturnImage(),edited,vk::Format::eR16G16B16A16Sfloat,
                           vk::ImageAspectFlagBits::eColor,w,h};
            const bool domain_changed=native_pre_sr.ChangeInputDomain(nr);
            reset=BbPreSr::InputDomain::AppliedReset(pre_requested_reset,domain_changed);last_dlss_reset=reset;
            std::printf("NATIVE_PRE_SR_INPUT_DOMAIN session=%s source=%llu source_sr_reset_requested=%u input_domain=%s input_domain_changed=%u applied_sr_reset=%u NR_reset=1 NR_reset_does_not_force_SR_reset=1 original_jitter=%.9g,%.9g\n",
                native_pre_sr.Session().c_str(),(unsigned long long)pre_source,pre_requested_reset,
                nr?"NR":"raw",domain_changed,reset,sign*jitter[0],sign*jitter[1]);
        }
        // Only the normal opt-in Quality HDR route. Feature creation above, motion
        // generation, sharpening, NR, merge and Present are outside this interval.
        const u64 sr_timing_source=BbStats::gpu_frames.load(std::memory_order_relaxed);
        std::optional<unsigned> sr_timing_slot;
        if (P0SrTiming::Enabled() && hdr && !Scaled() && w==1280 && h==720 && ow==1920 && oh==1080) {
            if (!p0_sr_timing) p0_sr_timing=std::make_shared<P0SrTiming>(instance);
            sr_timing_slot=p0_sr_timing->Begin(cmdbuf,sr_timing_source);
        }
        ok = dlss->Evaluate(cmdbuf, {
            .color = sr_color,
            .depth = depth,
            .motion = {vk::Image(motion_image), *motion_view, vk::Format::eR16G16Sfloat,
                       vk::ImageAspectFlagBits::eColor, w, h},
            .output = {vk::Image(output_image), *output_view, vk::Format::eR16G16B16A16Sfloat,
                       vk::ImageAspectFlagBits::eColor, ow, oh},
            .jitter_x = sign * jitter[0],
            .jitter_y = sign * jitter[1],
            .reset = reset,
            .frame_ms = frame_ms,
            .sharpness = settings.sharpen ? std::min(settings.sharpness.load(), 1.0f) : 0.0f,
        });
        if (sr_timing_slot) p0_sr_timing->End(cmdbuf,scheduler,*sr_timing_slot,sr_timing_source,ok);
        native_pre_sr.SrReadRecorded(instance,scheduler,pre_source,ok,reset,sr_color.image,sr_color.view);
    }
    if (ok) {
        const u64 source_frame = BbStats::gpu_frames.load(std::memory_order_relaxed);
        const auto viewport_now = camera_motion.CurrentJitter();
        if (!BbSettings::Get().jitter) q1_sr_jitter_enabled_from_start=false;
        const auto viewport_prev = camera_motion.PreviousJitter();
        if (!c2_sr_initial_reset_observed && last_dlss_reset) {
            c2_sr_initial_reset_observed = true;
            c2_sr_initial_source_game_frame = source_frame;
            if (q1_nr_process.Enabled())
                std::printf("Q1_SR_HISTORY initial_reset_observed=1 source_game_frame=%llu jitter_enabled=%u\n",(unsigned long long)source_frame,unsigned(BbSettings::Get().jitter.load()));
            if (c4_nr_process.Enabled())
                std::printf("C4_SR_HISTORY initial_reset_observed=1 source_game_frame=%llu\n", (unsigned long long)source_frame);
            if (c3_nr_process.Enabled())
                std::printf("C3_SR_HISTORY initial_reset_observed=1 source_game_frame=%llu\n", (unsigned long long)source_frame);
            std::printf("C2_SR_HISTORY initial_reset_observed=1 source_game_frame=%llu\n",
                        static_cast<unsigned long long>(source_frame));
        }
        const auto zero = [](const std::array<float, 2>& v) {
            return std::isfinite(v[0]) && std::isfinite(v[1]) && v[0] == 0.0f && v[1] == 0.0f;
        };
        if (!zero(viewport_now) || !zero(viewport_prev) || last_dlss_jitter_x != 0.0f ||
            last_dlss_jitter_y != 0.0f)
            c2_sr_jitter_zero_from_start = false;
    }
    if (!ok) {
        std::printf("Upscaler: DLSS failed; falling back to FSR 3.1\n");
        BbSettings::Get().upscaler = BbSettings::UpscalerFsr3;
        dlss_failed = true; // EnsureResources creates the FSR 3 context next frame
        reset = true;
    }
    return ok;
}

} // namespace Vulkan
