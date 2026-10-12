// SPDX-License-Identifier: GPL-2.0-or-later
#include "p0_game_bridge.h"
#include "p0_wire.h"
#include "p0_wait_budget.h"
#include "p0_timing_math.h"

#include <cerrno>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <set>
#include <cmath>
#include <unordered_set>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <nlohmann/json.hpp>

#include "video_core/renderer_vulkan/p0_nr_process.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/host_shaders/p0_game_bridge_comp.h"
#include "video_core/host_shaders/c3_frame_validate_comp.h"
#include "video_core/host_shaders/c3_frame_validate_return_comp.h"

namespace Vulkan {
namespace {
struct AcceptanceExpired : std::runtime_error { using std::runtime_error::runtime_error; };
using Json = nlohmann::json;
constexpr vk::ExternalMemoryHandleTypeFlagBits HandleType =
    vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
constexpr size_t MaxControlBytes = 64 * 1024;
constexpr std::string_view ProfileId = P0Wire::ProfileId;
constexpr vk::ImageUsageFlags SharedUsage =
    vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
    vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage;

std::string Read(const std::string& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (!error && size > MaxControlBytes)
        throw std::runtime_error("P0 control exceeds 64 KiB: " + path);
    std::ifstream input(path, std::ios::binary);
    return input ? std::string(std::istreambuf_iterator<char>(input), {}) : std::string{};
}

void AtomicWrite(const std::string& path, const std::string& value) {
    const auto temporary = path + ".tmp-" + std::to_string(getpid());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("P0 open temp control: " + temporary);
    output << value << '\n';
    output.flush();
    if (!output) throw std::runtime_error("P0 write temp control: " + temporary);
    output.close();
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        unlink(temporary.c_str());
        throw std::runtime_error("P0 publish control: " + std::string(std::strerror(errno)));
    }
}

std::vector<P0FdIdentity> SnapshotFds(pid_t pid, const std::string& output_path) {
    const auto root = std::filesystem::path("/proc") / std::to_string(pid) / "fd";
    if (!std::filesystem::is_directory(root))
        throw std::runtime_error("P0 fd inventory unavailable for pid=" + std::to_string(pid));
    std::vector<P0FdIdentity> records;
    Json inventory = Json::array();
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const std::string name = entry.path().filename().string();
        char* end = nullptr;
        const long number = std::strtol(name.c_str(), &end, 10);
        if (!end || *end || number < 0 || number > INT32_MAX) continue;
        std::array<char, 4096> target{};
        const auto link = entry.path().string();
        const ssize_t size = readlink(link.c_str(), target.data(), target.size() - 1);
        struct stat st{};
        P0FdIdentity record;
        record.number = static_cast<int>(number);
        record.target = size < 0 ? "<readlink-error:" + std::to_string(errno) + ">"
                                 : std::string(target.data(), static_cast<size_t>(size));
        record.stat_ok = stat(link.c_str(), &st) == 0;
        if (record.stat_ok) {
            record.device = static_cast<uint64_t>(st.st_dev);
            record.inode = static_cast<uint64_t>(st.st_ino);
        }
        inventory.push_back({{"fd", record.number}, {"target", record.target},
                             {"stat_ok", record.stat_ok}, {"device", record.device},
                             {"inode", record.inode}});
        records.push_back(std::move(record));
    }
    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
        return a.number < b.number;
    });
    AtomicWrite(output_path, inventory.dump());
    return records;
}

bool SamePayload(const P0FdIdentity& a, const P0FdIdentity& b) {
    return a.stat_ok && b.stat_ok && a.target == b.target && a.device == b.device &&
           a.inode == b.inode;
}

bool SameInventoryEntry(const P0FdIdentity& a, const P0FdIdentity& b) {
    return a.number == b.number && a.target == b.target && a.stat_ok == b.stat_ok &&
           a.device == b.device && a.inode == b.inode;
}

Json FdInventoryJson(const std::vector<P0FdIdentity>& records) {
    Json result = Json::array();
    for (const auto& record : records)
        result.push_back({{"fd", record.number}, {"target", record.target},
                          {"stat_ok", record.stat_ok}, {"device", record.device},
                          {"inode", record.inode}});
    return result;
}

bool HasFile(const std::string& path) {
    return std::filesystem::is_regular_file(path);
}

Json StrictJson(std::string_view text, std::string_view source) {return P0Wire::Parse(text,source);}

std::optional<Json> TryReadJson(const std::string& path) {
    if (!HasFile(path)) return std::nullopt;
    return StrictJson(Read(path), path);
}

std::string WaitJson(const std::string& path, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto contents = Read(path);
        if (!contents.empty() && std::chrono::steady_clock::now() < deadline) return contents;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return {};
}

int DuplicateRemoteFd(pid_t exporter, int remote_fd) {
    if (exporter <= 1 || remote_fd < 0) throw std::runtime_error("P0 invalid exporter fd");
    const int pidfd = static_cast<int>(syscall(SYS_pidfd_open, exporter, 0));
    if (pidfd < 0) throw std::runtime_error("P0 pidfd_open errno=" + std::to_string(errno));
    const int duplicate = static_cast<int>(syscall(SYS_pidfd_getfd, pidfd, remote_fd, 0));
    const int error = errno;
    close(pidfd);
    if (duplicate < 0) throw std::runtime_error("P0 pidfd_getfd errno=" + std::to_string(error));
    return duplicate;
}

uint32_t ExpectedFormat(std::string_view role) {
    if (role == "color" || role == "output") return 109; // R32G32B32A32_SFLOAT
    if (role == "depth") return 100;                     // R32_SFLOAT
    if (role == "mv") return 103;                        // R32G32_SFLOAT
    return 0;
}

Json Envelope(const std::string& session,std::string_view mode,std::string_view execution_mode,
              std::string_view op,std::string_view status,u32 frame,u64 source_frame) {
 Json e{{"protocol_version",6u},{"transport_layout",P0Wire::Layout},{"execution_mode",execution_mode},
 {"session",session},{"mode",mode},{"generation",0u},{"frame",frame},{"resource_set",session+"-g0"},
 {"source_game_frame",source_frame},{"profile_id",ProfileId},{"width",1920u},{"height",1080u},
 {"render_width",1280u},{"render_height",720u},{"quality_profile",P0Wire::Profile()},
 {"op",op},{"status",status},{"completed_GPU",true},{"output_valid",false},{"model_valid",false},
 {"validation_profile",P0Wire::Validation(execution_mode)},{"audit_kind",execution_mode=="diagnostic"?"full":"off"},
 {"source_frame_valid",frame!=0 || source_frame!=0},{"parameter_application",P0Wire::ParameterApplication()}};
 if(execution_mode=="hot") {e["numerical_validation"]="NOT_CHECKED";e["pixel_validation"]="NOT_RUN";e["signature"]="NOT_RUN";e["dump_count"]=0u;}
 return e;
}
uint64_t RequireUnsigned(const Json& value,std::string_view key) {return P0Wire::Uint(value,key);}

std::string RequireString(const Json& value, std::string_view key) {
    const std::string field(key);
    if (!value.is_object() || !value.contains(field) || !value.at(field).is_string())
        throw std::runtime_error("P0 string JSON field required: " + field);
    return value.at(field).get<std::string>();
}
bool RequireBool(const Json& value, std::string_view key) {
    const std::string field(key);
    if (!value.is_object() || !value.contains(field) || !value.at(field).is_boolean())
        throw std::runtime_error("P0 boolean JSON field required: " + field);
    return value.at(field).get<bool>();
}

void ValidateEnvelope(const Json& value, const std::string& session, std::string_view mode, std::string_view execution_mode,
                      std::string_view op, std::string_view status, u32 frame, u64 source_frame,
                      bool output_valid = false, bool model_valid = false) {
    P0Wire::ValidateProfile(value.at("quality_profile"));
    if(RequireString(value,"validation_profile")!=P0Wire::Validation(execution_mode)||
       RequireString(value,"audit_kind")!=(execution_mode=="diagnostic"?"full":"off")||
       value.at("parameter_application")!=P0Wire::ParameterApplication()) throw std::runtime_error("P0 exact scope/application mismatch");
    if(execution_mode=="hot"&&(RequireString(value,"numerical_validation")!="NOT_CHECKED"||RequireString(value,"pixel_validation")!="NOT_RUN"||
       RequireString(value,"signature")!="NOT_RUN"||RequireUnsigned(value,"dump_count"))) throw std::runtime_error("P0 false hot scope");
    if(value.at("quality_profile").dump()!=P0Wire::Profile().dump()) throw std::runtime_error("P0 exact profile wire echo mismatch");
    if (RequireUnsigned(value, "protocol_version") != 6 ||
        RequireString(value,"transport_layout") != P0Wire::Layout ||
        RequireString(value,"validation_profile") != P0Wire::Validation(execution_mode) || RequireString(value,"audit_kind") != (execution_mode=="diagnostic"?"full":"off") || RequireString(value, "execution_mode") != execution_mode ||
        RequireString(value, "session") != session || RequireString(value, "mode") != mode ||
        RequireUnsigned(value, "generation") != 0 || RequireUnsigned(value, "frame") != frame ||
        RequireString(value, "resource_set") != session + "-g0" ||
        RequireUnsigned(value, "source_game_frame") != source_frame ||
        RequireBool(value,"source_frame_valid") != (frame!=0 || source_frame!=0) ||
        RequireString(value, "profile_id") != ProfileId ||
        RequireUnsigned(value, "width") != 1920 || RequireUnsigned(value, "height") != 1080 ||
        RequireUnsigned(value, "render_width") != 1280 ||
        RequireUnsigned(value, "render_height") != 720 || RequireString(value, "op") != op ||
        RequireString(value, "status") != status || !RequireBool(value, "completed_GPU") ||
        RequireBool(value, "output_valid") != output_valid ||
        RequireBool(value, "model_valid") != model_valid)
        throw std::runtime_error("P0 strict control tuple/status mismatch");
}

vk::ImageView FindView(const std::vector<P0ImportedImage>& images, std::string_view role) {
    for (const auto& image : images) if (image.role == role) return image.view;
    return {};
}

P0ImportedImage& FindImage(std::vector<P0ImportedImage>& images, std::string_view role) {
    for (auto& image : images) if (image.role == role) return image;
    throw std::runtime_error("P0 image role absent: " + std::string(role));
}

uint64_t Fnv1a(const uint8_t* bytes, size_t size) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}

std::string Hex64(uint64_t value) {
    char text[17]{};
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

uint32_t HalfBitsToFloatBits(uint16_t h) {
    const uint32_t sign=uint32_t(h&0x8000u)<<16;
    uint32_t exp=(h>>10)&31u, mant=h&1023u;
    if(exp==31u) throw std::runtime_error("P0 actual source half is nonfinite");
    if(!exp) {
        if(!mant) return sign;
        int e=-14; while(!(mant&1024u)){mant<<=1;--e;}
        return sign|(uint32_t(e+127)<<23)|((mant&1023u)<<13);
    }
    return sign|((exp+112u)<<23)|(mant<<13);
}

bool JsonVkBool32(const Json& value, std::string_view field) {
    if (value.is_boolean()) return value.get<bool>();
    if (!value.is_number_unsigned() && !value.is_number_integer())
        throw std::runtime_error("P0 metadata VkBool32 has invalid JSON type: " +
                                 std::string(field));
    const auto integer = value.get<int64_t>();
    if (integer != 0 && integer != 1)
        throw std::runtime_error("P0 metadata VkBool32 is outside 0/1: " +
                                 std::string(field));
    return integer == 1;
}

uint64_t JsonUnsigned(const Json& value, std::string_view field, uint64_t maximum = UINT64_MAX) {
    if (!value.is_number_unsigned() && !value.is_number_integer())
        throw std::runtime_error("P0 metadata integer has invalid JSON type: " +
                                 std::string(field));
    if (value.is_number_integer() && value.get<int64_t>() < 0)
        throw std::runtime_error("P0 metadata integer is negative: " + std::string(field));
    const uint64_t result = value.get<uint64_t>();
    if (result > maximum)
        throw std::runtime_error("P0 metadata integer out of range: " + std::string(field));
    return result;
}

bool WaitSubmitted(const Instance& instance, Scheduler& scheduler, u64 tick,
                   std::chrono::steady_clock::time_point deadline) {
    static_assert(static_cast<int32_t>(vk::Result::eSuccess)==P0Wait::Success &&
                  static_cast<int32_t>(vk::Result::eTimeout)==P0Wait::Timeout);
    // Same actual-tick fast path. This does not implicitly submit/Flush any commands.
    if (scheduler.IsFree(tick)) return true;
    const auto budget=P0Wait::Remaining(P0Wait::EpochNs(deadline),
                                       P0Wait::EpochNs(std::chrono::steady_clock::now()));
    if(budget.kind==P0Wait::BudgetKind::Expired) return false;
    if(budget.kind!=P0Wait::BudgetKind::Finite || !budget.nanoseconds)
        throw std::runtime_error("P0 invalid finite timeline wait budget");
    const auto handle=scheduler.GetWorkSemaphore()->Handle();
    const vk::SemaphoreWaitInfo info{.semaphoreCount=1,.pSemaphores=&handle,.pValues=&tick};
    const auto result=instance.GetDevice().waitSemaphores(&info,budget.nanoseconds);
    // IsFree refreshes the real timeline counter when the cache is behind. Only a
    // successful API result may query/confirm completion; timeout/errors remain held.
    const bool completed=result==vk::Result::eSuccess && scheduler.IsFree(tick);
    const auto outcome=P0Wait::Classify(static_cast<int32_t>(result),completed,
                                       std::chrono::steady_clock::now()<deadline);
    if(outcome==P0Wait::Outcome::Completed) return true;
    if(outcome==P0Wait::Outcome::Expired) return false;
    if(outcome==P0Wait::Outcome::InvalidSuccess)
        throw std::runtime_error("P0 timeline API success without actual target tick completion");
    throw std::runtime_error("P0 finite timeline wait failed: "+vk::to_string(result));
}
} // namespace

void P0GameBridge::Quarantine(std::string_view reason) {
    quarantined = true;
    const std::string message = "P0_QUARANTINE reason=" + std::string(reason) +
        " resources_retained=" + std::to_string(resources.size()) +
        " last_submitted_tick=" + std::to_string(last_submitted_tick) +
        " last_completed_tick=" + std::to_string(last_completed_tick) +
        " outstanding_tick=" + std::to_string(outstanding_tick) +
        " cpu_monotonic_ns=" + std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) + "\n";
    std::fprintf(stderr, "%s", message.c_str());
    if (process_owner) {
        std::fprintf(stderr,
            "P0_PE_STATE child_pid=%d exporter_pid=%d completed=%u exit_status=%d identity_current=%u\n",
            process_owner->ChildPid(), process_owner->ExporterPid(), process_owner->Completed(),
            process_owner->ExitStatus(), process_owner->ExporterIdentityCurrent());
    }
    if (!soft_expired && !run_dir.empty() && !abort_sent) {
        try {
            AtomicWrite(run_dir + "/native.abort", message);
            abort_sent = true;
            std::fprintf(stderr, "P0_HELPER_ABORT published=1 exporter_pid=%d\n", exporter_pid);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "P0_HELPER_ABORT published=0 reason=%s\n", e.what());
        }
    }
}

void P0GameBridge::CreateLocalReturnAndPipeline(const Instance& instance) {
    const auto allocation = P0Wire::SelectBridgeResources(mode,frame_count,execution_mode,width,height);
    local_return = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
    local_return.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    local_return_view = instance.GetDevice().createImageViewUnique({
        .image = vk::Image(local_return), .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }).value;
    std::array<vk::DescriptorSetLayoutBinding, 9> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {.binding = i,
                       .descriptorType = i == 1 ? vk::DescriptorType::eCombinedImageSampler
                                                : i == 8 ? vk::DescriptorType::eStorageBuffer : vk::DescriptorType::eStorageImage,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    }
    descriptor_layout = instance.GetDevice().createDescriptorSetLayoutUnique({
        .bindingCount = static_cast<u32>(bindings.size()), .pBindings = bindings.data(),
    }).value;
    const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                     .offset = 0, .size = sizeof(u32)};
    pipeline_layout = instance.GetDevice().createPipelineLayoutUnique({
        .setLayoutCount = 1, .pSetLayouts = &*descriptor_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
    }).value;
    const auto module = CompileSPV(P0_GAME_BRIDGE_COMP, instance.GetDevice());
    pipeline = instance.GetDevice().createComputePipelineUnique({}, {
        .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
        .layout = *pipeline_layout,
    }).value;
    instance.GetDevice().destroyShaderModule(module);
    const std::array<vk::DescriptorPoolSize, 3> pool_sizes{{
        {vk::DescriptorType::eStorageImage, 7},
        {vk::DescriptorType::eCombinedImageSampler, 1},
        {vk::DescriptorType::eStorageBuffer, 1},
    }};
    descriptor_pool = instance.GetDevice().createDescriptorPoolUnique({
        .maxSets = 1, .poolSizeCount = static_cast<u32>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    }).value;
    descriptor_set = instance.GetDevice().allocateDescriptorSets({
        .descriptorPool = *descriptor_pool, .descriptorSetCount = 1,
        .pSetLayouts = &*descriptor_layout,
    }).value.front();
    depth_sampler = instance.GetDevice().createSamplerUnique({
        .magFilter = vk::Filter::eNearest, .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge, .maxLod = 0.0f,
    }).value;
    conversion_guard = std::make_unique<VideoCore::Buffer>(
        instance, 0, 4, VideoCore::MemoryType::HostCached, "P0 fused conversion finite/range flag");
    if (conversion_guard->mapped_data.size() < 4) throw std::runtime_error("P0 guard not mapped");
    timing_device = instance.GetDevice();
    const auto queue_properties = instance.GetPhysicalDevice().getQueueFamilyProperties();
    timestamp_valid_bits = queue_properties.at(instance.GetGraphicsQueueFamilyIndex()).timestampValidBits;
    timestamp_period = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
    if (timestamp_valid_bits && timestamp_period > 0 && std::isfinite(timestamp_period))
        timing_pool = timing_device.createQueryPoolUnique({.queryType = vk::QueryType::eTimestamp, .queryCount = 6}).value;
    if (allocation.diagnostic) {
    const u64 pixels = static_cast<u64>(width) * height;
    readback = std::make_unique<VideoCore::Buffer>(
        instance, 0, allocation.full_pixel_buffer_bytes, VideoCore::MemoryType::HostCached,
        "P0 scheduled full audit readback");
    if (readback->mapped_data.size() < pixels * 48)
        throw std::runtime_error("P0 host readback buffer is not mapped to requested size");
    guide_readback = std::make_unique<VideoCore::Buffer>(instance,0,18432000,VideoCore::MemoryType::HostCached,"P0 source and converted guide evidence");
    if (guide_readback->mapped_data.size()<18432000) throw std::runtime_error("P0 guide readback not mapped");
    validation_stats = std::make_unique<VideoCore::Buffer>(
        instance, 0, sizeof(C4FrameValidationStats), VideoCore::MemoryType::HostCached,
        "P0 per-frame 64B validation");
    if (validation_stats->mapped_data.size() < sizeof(C4FrameValidationStats))
        throw std::runtime_error("P0 validation readback is not mapped");
    return_validation_stats = std::make_unique<VideoCore::Buffer>(
        instance, 0, 16, VideoCore::MemoryType::HostCached, "P0 native-only return-half validation");
    if (return_validation_stats->mapped_data.size() < 16)
        throw std::runtime_error("P0 return-half counter is not mapped");
    const std::array<vk::DescriptorSetLayoutBinding, 5> validation_bindings{{
        {0, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {4, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    const auto device = instance.GetDevice();
    validation_descriptor_layout = device.createDescriptorSetLayoutUnique({
        .bindingCount = 5, .pBindings = validation_bindings.data()}).value;
    const vk::PushConstantRange validation_push{vk::ShaderStageFlagBits::eCompute, 0, 8};
    validation_pipeline_layout = device.createPipelineLayoutUnique({
        .setLayoutCount = 1, .pSetLayouts = &*validation_descriptor_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &validation_push}).value;
    const auto validation_module = CompileSPV(C3_FRAME_VALIDATE_COMP, device);
    validation_pipeline = device.createComputePipelineUnique({}, {
        .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                  .module = validation_module, .pName = "main"},
        .layout = *validation_pipeline_layout}).value;
    device.destroyShaderModule(validation_module);
    const auto return_module = CompileSPV(C3_FRAME_VALIDATE_RETURN_COMP, device);
    validation_return_pipeline = device.createComputePipelineUnique({}, {
        .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = return_module, .pName = "main"},
        .layout = *validation_pipeline_layout}).value;
    device.destroyShaderModule(return_module);
    const std::array<vk::DescriptorPoolSize, 2> validation_pool_sizes{{
        {vk::DescriptorType::eStorageImage, 3}, {vk::DescriptorType::eStorageBuffer, 2}}};
    validation_descriptor_pool = device.createDescriptorPoolUnique({
        .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = validation_pool_sizes.data()}).value;
    validation_descriptor_set = device.allocateDescriptorSets({
        .descriptorPool = *validation_descriptor_pool, .descriptorSetCount = 1,
        .pSetLayouts = &*validation_descriptor_layout}).value.front();
    }
    if(bool(readback)!=allocation.diagnostic || bool(guide_readback)!=allocation.diagnostic ||
       bool(validation_stats)!=allocation.diagnostic || bool(return_validation_stats)!=allocation.diagnostic)
        throw std::runtime_error("P0 actual buffer selection differs from declared execution scope");
    u64 vma_block_bytes=0,vma_allocation_bytes=0;
    VmaDeviceUsage(vma_block_bytes,vma_allocation_bytes);
    std::printf("P0_NATIVE_ALLOCATION execution_mode=%s full_pixel_readback_allocated=%u guard_bytes=4 timestamp_valid_bits=%u period_ns=%.9g native_VMA_bytes=%llu global_usage_bytes=%llu global_budget_bytes=%llu memory_scope=VMA_excludes_direct_imports_global_heap_shared_counted_once\n",
        execution_mode.c_str(),bool(readback),timestamp_valid_bits,timestamp_period,
        (unsigned long long)vma_allocation_bytes,(unsigned long long)instance.GetDeviceMemoryUsage(),
        (unsigned long long)instance.GetDeviceMemoryBudgetNow());
}

void P0GameBridge::RecordValidation(const Instance& instance, Scheduler& scheduler,
                                   vk::ImageView source, vk::ImageView output, bool validate_return) {
    const auto cmd = scheduler.CommandBuffer();
    // Prior use of this single stats buffer was fenced before descriptor/counter reuse.
    cmd.fillBuffer(validation_stats->Handle(), 0, sizeof(C4FrameValidationStats), 0);
    if (validate_return) cmd.fillBuffer(return_validation_stats->Handle(), 0, 16, 0);
    const vk::BufferMemoryBarrier2 clear_to_compute{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = validation_stats->Handle(), .offset = 0, .size = sizeof(C4FrameValidationStats)};
    cmd.pipelineBarrier2({.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &clear_to_compute});
    if (validate_return) {
        auto clear_half = clear_to_compute;
        clear_half.buffer = return_validation_stats->Handle(); clear_half.size = 16;
        cmd.pipelineBarrier2({.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &clear_half});
    }
    const std::array<vk::DescriptorImageInfo, 3> infos{{
        {.imageView = source, .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = output, .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = *local_return_view, .imageLayout = vk::ImageLayout::eGeneral}}};
    const vk::DescriptorBufferInfo buffer_info{validation_stats->Handle(), 0,
                                              sizeof(C4FrameValidationStats)};
    const vk::DescriptorBufferInfo half_info{return_validation_stats->Handle(), 0, 16};
    const std::array<vk::WriteDescriptorSet, 5> writes{{
        {.dstSet = validation_descriptor_set, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &infos[0]},
        {.dstSet = validation_descriptor_set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &infos[1]},
        {.dstSet = validation_descriptor_set, .dstBinding = 2, .descriptorCount = 1,
          .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &buffer_info},
        {.dstSet = validation_descriptor_set, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &infos[2]},
        {.dstSet = validation_descriptor_set, .dstBinding = 4, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &half_info}}};
    instance.GetDevice().updateDescriptorSets(writes, {});
    const std::array<u32, 2> extent{width, height};
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, validate_return ? *validation_return_pipeline : *validation_pipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *validation_pipeline_layout, 0,
                          validation_descriptor_set, {});
    cmd.pushConstants(*validation_pipeline_layout, vk::ShaderStageFlagBits::eCompute,
                      0, sizeof(extent), extent.data());
    cmd.dispatch((width + 7) / 8, (height + 7) / 8, 1);
    const vk::BufferMemoryBarrier2 compute_to_host{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = validation_stats->Handle(), .offset = 0, .size = sizeof(C4FrameValidationStats)};
    cmd.pipelineBarrier2({.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &compute_to_host});
    if (validate_return) {
        auto half_to_host = compute_to_host;
        half_to_host.buffer = return_validation_stats->Handle(); half_to_host.size = 16;
        cmd.pipelineBarrier2({.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &half_to_host});
    }
}

C4FrameValidationStats P0GameBridge::ReadValidation() {
    validation_stats->Invalidate(0, sizeof(C4FrameValidationStats));
    C4FrameValidationStats result{};
    std::memcpy(&result, validation_stats->mapped_data.data(), sizeof(result));
    if (result.raw_rgb_finite || result.raw_rgb_written || result.raw_rgb_nan ||
        result.reserved0 || result.reserved1 ||
        result.visited_pixels != static_cast<u64>(width) * height)
        throw std::runtime_error("P0 native 64B GPU layout/visited/raw-not-applicable mismatch");
    return result;
}

bool P0GameBridge::AuditFdRetired(const Instance& instance, const P0NrProcess& process) {
    try {
        const auto path = run_dir + "/fd-retired-" + session + "-g0.json";
        if (!HasFile(path)) return false;
        const Json fd_retired = StrictJson(Read(path), path);
        ValidateEnvelope(fd_retired, session, mode, execution_mode, "fd_retired", "FD_RETIRED", 0, 0);
        if (!fd_retired.at("closed_fd").is_array() || fd_retired.at("closed_fd").size() != 4 ||
            resources.size() != 4 || exporter_raw_fds.size() != 4)
            throw std::runtime_error("P0 fd-retired must map four imported role/fd pairs");
        std::set<std::string> closed_roles;
        for (const auto& item : fd_retired.at("closed_fd")) {
            if (!item.is_object() || !item.contains("role") || !item["role"].is_string() ||
                !item.contains("fd") || !item["fd"].is_number_unsigned())
                throw std::runtime_error("P0 fd-retired role/fd JSON type mismatch");
            const auto role = item.at("role").get<std::string>();
            const auto fd64 = item.at("fd").get<uint64_t>();
            if (fd64 > INT32_MAX) throw std::runtime_error("P0 fd-retired fd overflow");
            const auto fd = static_cast<int>(fd64);
            const auto resource = std::find_if(resources.begin(), resources.end(),
                [&](const auto& image) { return image.role == role && image.raw_fd == fd; });
            if (!closed_roles.insert(role).second || resource == resources.end())
                throw std::runtime_error("P0 fd-retired role does not match imported raw fd");
            const auto original = std::find_if(exporter_raw_fds.begin(), exporter_raw_fds.end(),
                [&](const auto& record) { return record.number == fd; });
            if (original == exporter_raw_fds.end() || !original->stat_ok)
                throw std::runtime_error("P0 original raw FD identity missing");
        }
        if (closed_roles != std::set<std::string>{"color", "depth", "mv", "output"})
            throw std::runtime_error("P0 fd-retired role set incomplete");
        const auto after = SnapshotFds(process.ExporterPid(),
            run_dir + "/wine-fd-active-retired.json");
        for (const auto& original : exporter_raw_fds) {
            if (std::any_of(after.begin(), after.end(), [&](const auto& record) {
                    return SamePayload(original, record);
                }))
                throw std::runtime_error("P0 original raw DMA_BUF payload remains after consume");
        }
        fd_retired_ready = true;
        CreateLocalReturnAndPipeline(instance);
        prepared = true;
        std::printf("P0_IMPORT_GATE PASS session=%s resources=4 fd_retired_role_payloads=4\n",
                    session.c_str());
        return true;
    } catch (const std::exception& e) {
        Quarantine(e.what());
        return false;
    }
}

bool P0GameBridge::Prepare(const Instance& instance, Scheduler& scheduler,
                           P0NrProcess& process, std::string_view requested_session,
                           const std::string& requested_run_dir,
                           const std::string& device_uuid, const std::string& driver_uuid,
                           u32 requested_width, u32 requested_height) {
    if (prepared || quarantined || finished) return prepared && !quarantined;
    if (requested_width != 1920 || requested_height != 1080) return false;
    process_owner = &process;
    if (!imported && (session.empty() || run_dir.empty())) {
        session = requested_session;
        mode = std::string(process.Mode());
        execution_mode = process.ExecutionMode();
        run_dir = requested_run_dir;
        frame_count = process.FrameCount();
        width = requested_width;
        height = requested_height;
    }
    if (process.Completed()) {
        Quarantine("P0 peer exited before bootstrap completed; no silent unbounded readiness wait");
        return false;
    }
    if (process.SetupFailed()) {
        Quarantine("P0 helper identity/bootstrap worker failed: " + process.Failure());
        return false;
    }
    if (!process.SetupReady() || !process.ExporterIdentityCurrent()) return false;
    if (!cold_deadline_started) {
        cold_deadline_started = true;
        cold_started_at = std::chrono::steady_clock::now();
        control_deadline = cold_started_at + std::chrono::seconds(120);
        std::printf("P0_NATIVE_DEADLINE phase=cold_ready_ms limit=120000 start=verified_exporter_identity\n");
    }
    if (session != requested_session || run_dir != requested_run_dir || width != requested_width ||
        height != requested_height) {
        Quarantine("P0 session/profile changed while slot is active");
        return false;
    }
    exporter_pid = process.ExporterPid();
    if (imported) {
        if (!fd_retired_ready) {
            if (!HasFile(run_dir + "/fd-retired-" + session + "-g0.json")) {
                if (std::chrono::steady_clock::now() >= control_deadline)
                    Quarantine("P0 fd_retired control deadline 60000ms expired; imports retained");
                return false;
            }
            if (!AuditFdRetired(instance, process)) return false;
            return true;
        }
        return prepared;
    }
    const auto baseline_path = run_dir + "/baseline-" + session + "-g0.json";
    if (!HasFile(baseline_path)) {
        if (std::chrono::steady_clock::now() >= control_deadline)
            Quarantine("P0 cold model readiness deadline 120000ms expired before baseline");
        return false;
    }
    try {
        if (!baseline_audited) {
            const Json baseline = StrictJson(Read(baseline_path), baseline_path);
            ValidateEnvelope(baseline, session, mode, execution_mode, "baseline", "BASELINE", 0, 0);
            if (!baseline.contains("model_init") || RequireBool(baseline, "model_init") != (mode!="noop") ||
                !baseline.contains("feature_created") ||
                RequireBool(baseline, "feature_created") != (mode!="noop") ||
                RequireUnsigned(baseline, "cold_init_timeout_ms") != 120000 ||
                RequireUnsigned(baseline, "control_timeout_ms") != 60000 ||
                RequireUnsigned(baseline, "gpu_timeout_ms") != 10000 ||
                RequireUnsigned(baseline, "frame_completion_timeout_ms") != 30000)
                throw std::runtime_error("P0 baseline model readiness/budgets mismatch");
            const auto baseline_fds = SnapshotFds(getpid(),
                run_dir + "/native-fd-baseline.json");
            exporter_baseline_fds = SnapshotFds(process.ExporterPid(),
                run_dir + "/wine-fd-baseline.json");
            auto baseline_ack = Envelope(session, mode, execution_mode, "baseline_audit", "BASELINE_AUDITED", 0, 0);
            baseline_ack["baseline_fd_count"] = baseline_fds.size();
            baseline_ack["inventory"] = "native-fd-baseline.json";
            AtomicWrite(run_dir + "/native-baseline-" + session + "-g0.json",
                        baseline_ack.dump());
            baseline_audited = true;
            control_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            std::printf("P0_NATIVE_DEADLINE phase=metadata_control_ms limit=60000 start=baseline_ack\n");
            return false;
        }

        const auto metadata_path = run_dir + "/meta-g0.json";
        if (!HasFile(metadata_path)) {
            if (std::chrono::steady_clock::now() >= control_deadline)
                Quarantine("P0 metadata control deadline 60000ms expired");
            return false;
        }
        const Json metadata = StrictJson(Read(metadata_path), metadata_path);
        if (RequireUnsigned(metadata, "protocol_version") != 6 ||
            RequireString(metadata,"transport_layout") != P0Wire::Layout ||
            RequireString(metadata, "execution_mode") != execution_mode ||
            RequireString(metadata, "session") != session ||
            RequireString(metadata, "mode") != mode || RequireUnsigned(metadata, "generation") != 0 ||
            RequireString(metadata, "resource_set") != session + "-g0" ||
            RequireUnsigned(metadata, "linux_pid") != static_cast<uint64_t>(process.ExporterPid()) ||
            RequireUnsigned(metadata, "handle_type") != static_cast<uint32_t>(HandleType) ||
            RequireString(metadata, "op") != "metadata" || RequireString(metadata, "profile_id") != ProfileId ||
            RequireUnsigned(metadata, "frame") != 0 || RequireUnsigned(metadata, "source_game_frame") != 0 ||
            RequireBool(metadata, "output_valid") || RequireBool(metadata, "model_valid") ||
            RequireUnsigned(metadata, "max_frames") != frame_count ||
            RequireUnsigned(metadata, "frame_count") != frame_count ||
            RequireUnsigned(metadata, "history_version") != 1 ||
            RequireUnsigned(metadata, "params_version") != 1 ||
            RequireUnsigned(metadata, "cold_init_timeout_ms") != 120000 ||
            RequireUnsigned(metadata, "control_timeout_ms") != 60000 ||
            RequireUnsigned(metadata, "gpu_timeout_ms") != 10000 ||
            RequireUnsigned(metadata, "frame_completion_timeout_ms") != 30000 ||
            P0Wire::Number(metadata, "manual_white_point") != 1.0 || RequireBool(metadata, "passthrough") ||
            RequireString(metadata, "device_uuid") != device_uuid ||
            RequireString(metadata, "driver_uuid") != driver_uuid || RequireString(metadata, "status") != "READY" ||
            !RequireBool(metadata, "completed_GPU") || RequireUnsigned(metadata, "width") != width ||
            RequireUnsigned(metadata, "height") != height || RequireUnsigned(metadata, "render_width") != 1280 ||
            RequireUnsigned(metadata, "render_height") != 720)
            throw std::runtime_error("P0 metadata identity/extent/UUID mismatch");
        ValidateEnvelope(metadata,session,mode,execution_mode,"metadata","READY",0,0);
        const auto& entries = metadata.at("resources");
        if (!entries.is_array() || entries.size() != 4)
            throw std::runtime_error("P0 metadata must contain four resources");

        std::set<std::string> roles;
        std::set<uint32_t> raw_fd_numbers;
        for (const auto& entry : entries) {
            const std::string role = entry.at("role").get<std::string>();
            if (!ExpectedFormat(role) || !roles.insert(role).second)
                throw std::runtime_error("P0 metadata has unknown or duplicate resource role");
            const auto raw_fd = JsonUnsigned(entry.at("fd"), "fd", INT32_MAX);
            if (!raw_fd || !raw_fd_numbers.insert(static_cast<uint32_t>(raw_fd)).second)
                throw std::runtime_error("P0 duplicate or invalid raw fd number");
            for (const char* key : {"format", "width", "height", "usage", "tiling",
                                    "image_type", "samples", "mip_levels", "array_layers",
                                    "flags", "sharing_mode", "offset", "runtime_layout",
                                    "memory_type_index", "req_memory_type_bits",
                                    "external_features", "compatible_handle_types"})
                (void)JsonUnsigned(entry.at(key), key);
            (void)JsonUnsigned(entry.at("allocation_size"), "allocation_size");
            (void)JsonUnsigned(entry.at("alignment"), "alignment");
            (void)RequireBool(entry, "dedicated");
            (void)JsonVkBool32(entry.at("requires_dedicated"), "requires_dedicated");
            (void)JsonVkBool32(entry.at("prefers_dedicated"), "prefers_dedicated");
        }
        if (roles != std::set<std::string>{"color", "depth", "mv", "output"})
            throw std::runtime_error("P0 metadata role set is incomplete");

        const auto device = instance.GetDevice();
        const auto physical = instance.GetPhysicalDevice();
        const int pidfd = static_cast<int>(syscall(SYS_pidfd_open, process.ExporterPid(), 0));
        if (pidfd < 0) throw std::runtime_error("P0 pidfd_open errno=" + std::to_string(errno));
        try {
            for (const auto& entry : entries) {
                const std::string role = entry.at("role").get<std::string>();
                const uint32_t format = ExpectedFormat(role);
                if (!format || entry.at("format") != format || entry.at("width") != (role=="depth"||role=="mv"?1280u:width) ||
                    entry.at("height") != (role=="depth"||role=="mv"?720u:height) || entry.at("usage") != 15 ||
                    entry.at("tiling") != 0 || entry.at("image_type") != 1 ||
                    entry.at("samples") != 1 || entry.at("mip_levels") != 1 ||
                    entry.at("array_layers") != 1 || entry.at("sharing_mode") != 0 ||
                    entry.at("flags") != 0 || entry.at("offset") != 0 ||
                    entry.at("dedicated") != true || entry.at("runtime_layout") != 1)
                    throw std::runtime_error("P0 per-image contract mismatch for " + role);
                const uint32_t raw_fd = entry.at("fd").get<uint32_t>();
                const auto raw_snapshot = SnapshotFds(process.ExporterPid(),
                    run_dir + "/wine-fd-before-import.json");
                const auto raw_identity = std::find_if(raw_snapshot.begin(), raw_snapshot.end(),
                    [&](const auto& record) { return record.number == static_cast<int>(raw_fd); });
                if (raw_fd > static_cast<uint32_t>(INT32_MAX) ||
                    raw_identity == raw_snapshot.end() || !raw_identity->stat_ok)
                    throw std::runtime_error("P0 raw DMA_BUF fd identity is not auditable");
                if (std::any_of(exporter_raw_fds.begin(), exporter_raw_fds.end(),
                        [&](const auto& other) { return SamePayload(other, *raw_identity); }))
                    throw std::runtime_error("P0 roles alias the same raw DMA_BUF payload");
                exporter_raw_fds.push_back(*raw_identity);
                if (!process.ExporterIdentityCurrent())
                    throw std::runtime_error("P0 exporter identity changed before pidfd_getfd");
                const int fd = static_cast<int>(syscall(SYS_pidfd_getfd, pidfd, raw_fd, 0));
                if (fd < 0)
                    throw std::runtime_error("P0 pidfd_getfd role=" + role + " errno=" +
                                             std::to_string(errno));
                const auto duplicate_snapshot = SnapshotFds(getpid(),
                    run_dir + "/native-fd-dup-check.json");
                const auto duplicate_identity = std::find_if(duplicate_snapshot.begin(),
                    duplicate_snapshot.end(), [&](const auto& record) { return record.number == fd; });
                if (duplicate_identity == duplicate_snapshot.end() ||
                    !SamePayload(*raw_identity, *duplicate_identity)) {
                    close(fd);
                    throw std::runtime_error("P0 pidfd_getfd duplicate payload differs from exporter raw fd");
                }

                const auto vk_format = static_cast<vk::Format>(format);
                const vk::ExternalMemoryImageCreateInfo external{.handleTypes = HandleType};
                const vk::ImageCreateInfo image_ci{
                    .pNext = &external,
                    .imageType = vk::ImageType::e2D,
                    .format = vk_format,
                    .extent = {role=="depth"||role=="mv"?1280u:width, role=="depth"||role=="mv"?720u:height, 1},
                    .mipLevels = 1,
                    .arrayLayers = 1,
                    .samples = vk::SampleCountFlagBits::e1,
                    .tiling = vk::ImageTiling::eOptimal,
                    .usage = SharedUsage,
                    .sharingMode = vk::SharingMode::eExclusive,
                    .initialLayout = vk::ImageLayout::eUndefined,
                };
                const auto [create_result, image] = device.createImage(image_ci);
                if (create_result != vk::Result::eSuccess) {
                    close(fd);
                    throw std::runtime_error("P0 vkCreateImage role=" + role + " result=" +
                                             vk::to_string(create_result));
                }
                vk::MemoryDedicatedRequirements dedicated{};
                const vk::ImageMemoryRequirementsInfo2 req_info{.image = image};
                vk::MemoryRequirements2 reqs{.pNext = &dedicated};
                device.getImageMemoryRequirements2(&req_info, &reqs);
                const auto& memory_reqs = reqs.memoryRequirements;
                const auto [fd_result, fd_props] = device.getMemoryFdPropertiesKHR(HandleType, fd);
                if (fd_result != vk::Result::eSuccess) {
                    close(fd);
                    device.destroyImage(image);
                    throw std::runtime_error("P0 vkGetMemoryFdPropertiesKHR role=" + role +
                                             " result=" + vk::to_string(fd_result));
                }
                const uint32_t declared_type = entry.at("memory_type_index").get<uint32_t>();
                const uint32_t declared_bits = entry.at("req_memory_type_bits").get<uint32_t>();
                const vk::DeviceSize declared_size = entry.at("allocation_size").get<vk::DeviceSize>();
                const vk::DeviceSize declared_alignment = entry.at("alignment").get<vk::DeviceSize>();
                const uint32_t external_features = entry.at("external_features").get<uint32_t>();
                const uint32_t compatible_handles = entry.at("compatible_handle_types").get<uint32_t>();
                const uint32_t compatible = memory_reqs.memoryTypeBits & fd_props.memoryTypeBits;
                if (declared_type >= 32 || declared_bits != memory_reqs.memoryTypeBits ||
                    !(compatible & (1u << declared_type)) || declared_size != memory_reqs.size ||
                    declared_alignment != memory_reqs.alignment || declared_alignment == 0 ||
                    JsonVkBool32(entry.at("requires_dedicated"), "requires_dedicated") !=
                        static_cast<bool>(dedicated.requiresDedicatedAllocation) ||
                    JsonVkBool32(entry.at("prefers_dedicated"), "prefers_dedicated") !=
                        static_cast<bool>(dedicated.prefersDedicatedAllocation) ||
                    !(external_features & 0x2u) || !(compatible_handles & 0x200u) ||
                    (dedicated.requiresDedicatedAllocation && !entry.at("dedicated").get<bool>())) {
                    close(fd);
                    device.destroyImage(image);
                    throw std::runtime_error("P0 memory type / dedicated requirement mismatch for " + role);
                }
                const vk::MemoryDedicatedAllocateInfo dedicated_alloc{.image = image};
                const vk::ImportMemoryFdInfoKHR import_info{
                    .pNext = &dedicated_alloc, .handleType = HandleType, .fd = fd};
                const vk::MemoryAllocateInfo alloc_info{
                    .pNext = &import_info,
                    .allocationSize = declared_size,
                    .memoryTypeIndex = declared_type,
                };
                const auto [alloc_result, memory] = device.allocateMemory(alloc_info);
                if (alloc_result != vk::Result::eSuccess) {
                    close(fd);
                    device.destroyImage(image);
                    throw std::runtime_error("P0 vkAllocateMemory import role=" + role +
                                             " result=" + vk::to_string(alloc_result));
                }
                const auto bind_result = device.bindImageMemory(image, memory, 0);
                if (bind_result != vk::Result::eSuccess) {
                    device.freeMemory(memory);
                    device.destroyImage(image);
                    throw std::runtime_error("P0 vkBindImageMemory role=" + role +
                                             " result=" + vk::to_string(bind_result));
                }
                const auto [view_result, view] = device.createImageView({
                    .image = image,
                    .viewType = vk::ImageViewType::e2D,
                    .format = vk_format,
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
                });
                if (view_result != vk::Result::eSuccess) {
                    device.freeMemory(memory);
                    device.destroyImage(image);
                    throw std::runtime_error("P0 vkCreateImageView role=" + role +
                                             " result=" + vk::to_string(view_result));
                }
                resources.push_back({role, image, memory, view, vk_format,
                                     static_cast<int>(raw_fd)});
                std::printf("P0_IMPORT role=%s fd=%u bits=0x%x type=%u GENERAL_EXTERNAL=1\n",
                            role.c_str(), raw_fd, compatible, declared_type);
            }
        } catch (...) {
            close(pidfd);
            throw;
        }
        close(pidfd);
        if (!process.ExporterIdentityCurrent())
            throw std::runtime_error("P0 exporter identity changed after imports");
        auto imported_ack = Envelope(session, mode, execution_mode, "imported", "IMPORTED", 0, 0);
        AtomicWrite(run_dir + "/imported-" + session + "-g0.json", imported_ack.dump());
        imported = true;
        control_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        std::printf("P0_NATIVE_DEADLINE phase=fd_retired_control_ms limit=60000 start=IMPORTED_ack\n");
        if (!AuditFdRetired(instance, process)) return false;
        return true;
    } catch (const std::exception& e) {
        Quarantine(e.what());
        return false;
    }
}

vk::ImageView P0GameBridge::ProcessFrame(const Instance& instance, Scheduler& scheduler,
                                         P0NrProcess& process, vk::Image sr_color,
                                         vk::ImageView sr_view, vk::Image depth,
                                         vk::ImageView depth_view, vk::Image motion,
                                         vk::ImageView motion_view,
                                         vk::ImageLayout depth_layout,
                                         u64 source_game_frame, bool source_reset,
                                         float source_frame_ms,
                                         std::array<float, 2> viewport_jitter,
                                         std::array<float, 2> previous_viewport_jitter,
                                         std::array<float, 2> sr_jitter,
                                         std::array<float, 2> previous_sr_jitter,
                                         bool sr_initial_reset_observed,
                                         u64 sr_initial_source_game_frame,
                                         bool sr_history_jitter_enabled_from_start,
                                         const std::array<vk::ImageCreateInfo,3>& source_infos) {
    frame_processed = false;
    output_sample_started = false;
    if (!prepared || quarantined || finished || idle || retire_requested || stop_pending || control_request || processed_frames >= frame_count ||
        source_game_frame == 0 || (last_source_frame && source_game_frame <= last_source_frame))
        return VK_NULL_HANDLE;
    const auto frame_started = std::chrono::steady_clock::now();
    const auto hook_deadline = frame_started + std::chrono::milliseconds(acceptance_budget_ms);
    const auto monotonic_ns = [] { return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count(); };
    frame_start_monotonic_ns = monotonic_ns();
    double prior_merge_wait_ms = 0, input_flush_ms = 0, input_wait_ms = 0,
           peer_wait_ms = 0, output_flush_ms = 0, output_wait_ms = 0;
    const auto ms_since = [](auto start) { return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count(); };
    try {
        // consumed only releases the shared slot. Its independent local-return image cannot
        // be overwritten until the previous merge's actual scheduler submission completes.
        if (pending_source_frame) {
            const auto prior_wait_start = std::chrono::steady_clock::now();
            if (!pending_merge_tick) throw std::runtime_error("P0 prior frame lacks actual merge tick");
            if (pending_merge_tick >= scheduler.CurrentTick()) {
                last_submitted_tick = scheduler.CurrentTick();
                outstanding_tick = last_submitted_tick;
                scheduler.Flush(); // submit the recorded tick; never wait on an unsubmitted tick
            }
            if (!WaitSubmitted(instance, scheduler, pending_merge_tick, hook_deadline))
                throw AcceptanceExpired("P0 prior merge last-use fence exceeded acceptance deadline; no slot reuse");
            last_completed_tick = std::max(last_completed_tick, pending_merge_tick);
            outstanding_tick = 0;
            std::printf("P0_SLOT_REUSE frame=%u prior_source=%llu prior_merge_tick=%llu GPU_complete=1 consumed_published=1\n",
                        processed_frames, (unsigned long long)*pending_source_frame,
                        (unsigned long long)pending_merge_tick);
            prior_merge_wait_ms = ms_since(prior_wait_start);
            ReadMergeTiming(processed_frames - 1, *pending_source_frame);
            pending_source_frame.reset();
            pending_merge_tick = 0;
            retirement_scheduled = false;
            returned_view = nullptr;
        }
        if (process.SetupFailed() || process.Completed() || !process.ExporterIdentityCurrent())
            throw std::runtime_error("P0 resident helper exited/changed before current input");
        if (depth_layout != vk::ImageLayout::eGeneral)
            throw std::runtime_error("P0 profile requires HDR Run depth GENERAL");
        const auto finite_jitter=[](const auto& value){return std::isfinite(value[0])&&std::isfinite(value[1]);};
        if (!std::isfinite(source_frame_ms)||source_frame_ms<=0 || !finite_jitter(viewport_jitter)||
            !finite_jitter(previous_viewport_jitter)||!finite_jitter(sr_jitter)||!finite_jitter(previous_sr_jitter)||
            !sr_initial_reset_observed||!sr_history_jitter_enabled_from_start||!sr_initial_source_game_frame||
            sr_initial_source_game_frame>source_game_frame)
            throw std::runtime_error("P0 actual enabled-jitter SR history/finite frame profile gate failed");
        Json source_descriptors=Json::object();
        const std::array<const char*,3> source_roles{"color","depth","mv"};
        for(size_t i=0;i<source_infos.size();++i) {
            const auto& ci=source_infos[i]; const u32 sw=i?1280u:1920u, sh=i?720u:1080u;
            const vk::Format sf=i==0?vk::Format::eR16G16B16A16Sfloat:i==1?vk::Format::eD32SfloatS8Uint:vk::Format::eR16G16Sfloat;
            if(ci.format!=sf||ci.extent.width!=sw||ci.extent.height!=sh||ci.extent.depth!=1||
               ci.samples!=vk::SampleCountFlagBits::e1||ci.mipLevels!=1||ci.arrayLayers!=1||
               !(ci.usage&vk::ImageUsageFlagBits::eTransferSrc))
               throw std::runtime_error("P0 actual copy-source CI/extent/format/TRANSFER_SRC mismatch");
            source_descriptors[source_roles[i]]={{"format",static_cast<u32>(ci.format)},
              {"aspect",i==1?2u:1u},{"width",sw},{"height",sh},{"usage",static_cast<u32>(ci.usage)},
              {"layout",1u},{"valid_rect",{{"x",0u},{"y",0u},{"width",sw},{"height",sh}}}};
        }
        const u32 frame = processed_frames;
        const u64 previous_source = last_source_frame;
        const std::string reset_reason = !frame ? "first" : source_reset ? "sr_reset" :
            resume_reset_pending ? "resume" : source_game_frame != previous_source + 1 ? "source_gap" : "continuous";
        const u32 nr_reset = reset_reason == "continuous" ? 0u : 1u;
        const bool noop=false; // P0 exposes no new noop model path; Q1 noop is frozen evidence.
        const bool diagnostic = execution_mode == "diagnostic";
        const bool full = diagnostic;
        current_audit_full = full;
        const std::string audit_kind = full ? "full" : "off";
        const bool skip = mode == "skipEvaluate";
        const u64 pixels = static_cast<u64>(width) * height, bytes32 = pixels * 16, bytes16 = pixels * 8;
        const auto& color = FindImage(resources, "color");
        const auto& scene_depth = FindImage(resources, "depth");
        const auto& mv = FindImage(resources, "mv");
        const auto& output = FindImage(resources, "output");
        const auto device = instance.GetDevice();
        auto cmd = scheduler.CommandBuffer();
        const auto family = instance.GetGraphicsQueueFamilyIndex();
        auto image_barrier = [&](vk::Image image, u32 src_family, u32 dst_family,
                                  vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                  vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                                  vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor) {
            const vk::ImageMemoryBarrier2 barrier{
                .srcStageMask = src_stage, .srcAccessMask = src_access,
                .dstStageMask = dst_stage, .dstAccessMask = dst_access,
                .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = src_family, .dstQueueFamilyIndex = dst_family,
                .image = image, .subresourceRange = {aspect, 0, 1, 0, 1}};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        };
        auto copy_readback = [&](vk::Image image, u64 offset) {
            const vk::BufferImageCopy copy{.bufferOffset = offset,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageExtent = {width, height, 1}};
            cmd.copyImageToBuffer(image, vk::ImageLayout::eGeneral, readback->Handle(), copy);
        };
        auto host_barrier = [&](u64 offset, u64 size) {
            const vk::BufferMemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = readback->Handle(), .offset = offset, .size = size};
            cmd.pipelineBarrier2({.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier});
        };
        auto update_conversion = [&] {
            const std::array<vk::DescriptorImageInfo, 8> infos{{
                {.imageView = sr_view, .imageLayout = vk::ImageLayout::eGeneral},
                {.sampler = *depth_sampler, .imageView = depth_view, .imageLayout = vk::ImageLayout::eGeneral},
                {.imageView = motion_view, .imageLayout = vk::ImageLayout::eGeneral},
                {.imageView = color.view, .imageLayout = vk::ImageLayout::eGeneral},
                {.imageView = scene_depth.view, .imageLayout = vk::ImageLayout::eGeneral},
                {.imageView = mv.view, .imageLayout = vk::ImageLayout::eGeneral},
                {.imageView = *local_return_view, .imageLayout = vk::ImageLayout::eGeneral},
                {.imageView = output.view, .imageLayout = vk::ImageLayout::eGeneral}}};
            std::array<vk::WriteDescriptorSet, 8> writes{};
            for (u32 i = 0; i < writes.size(); ++i)
                writes[i] = {.dstSet = descriptor_set, .dstBinding = i, .descriptorCount = 1,
                    .descriptorType = i == 1 ? vk::DescriptorType::eCombinedImageSampler : vk::DescriptorType::eStorageImage,
                    .pImageInfo = &infos[i]};
            device.updateDescriptorSets(writes, {});
            const vk::DescriptorBufferInfo guard_info{conversion_guard->Handle(), 0, 4};
            const vk::WriteDescriptorSet guard_write{
                .dstSet = descriptor_set, .dstBinding = 8, .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &guard_info};
            device.updateDescriptorSets(guard_write, {});
        };
        auto conversion = [&](u32 conversion_mode) {
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, descriptor_set, {});
            cmd.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                              sizeof(conversion_mode), &conversion_mode);
            cmd.dispatch(((conversion_mode==2?1280u:width)+7)/8, ((conversion_mode==2?720u:height)+7)/8, 1);
        };
        if (timing_pool) cmd.resetQueryPool(*timing_pool, 0, 6);
        for (const auto& image : resources)
            image_barrier(image.image, VK_QUEUE_FAMILY_EXTERNAL, family,
                vk::PipelineStageFlagBits2::eAllCommands, {},
                vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite);
        for (const auto [image, aspect] : std::array<std::pair<vk::Image, vk::ImageAspectFlags>, 3>{{
                 {sr_color, vk::ImageAspectFlagBits::eColor},
                 {depth, vk::ImageAspectFlagBits::eDepth},
                 {motion, vk::ImageAspectFlagBits::eColor}}})
            image_barrier(image, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryWrite,
                vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, aspect);
        // Independent local copy of the actual game sources; this is not the converted payload.
        auto guide_copy=[&](vk::Image image,vk::ImageAspectFlags aspect,u64 offset) {
            const vk::BufferImageCopy copy{.bufferOffset=offset,
                .imageSubresource={aspect,0,0,1},.imageOffset={0,0,0},.imageExtent={1280,720,1}};
            cmd.copyImageToBuffer(image,vk::ImageLayout::eGeneral,guide_readback->Handle(),copy);
        };
        if(diagnostic) {
        guide_copy(depth,vk::ImageAspectFlagBits::eDepth,0);
        guide_copy(motion,vk::ImageAspectFlagBits::eColor,3686400);
        }
        if(noop) {copy_readback(sr_color,bytes32*2+bytes16);host_barrier(bytes32*2+bytes16,bytes16);}
        update_conversion();
        Timestamp(scheduler, 0);
        conversion(0); conversion(2); // Separate 1080 Color and 720 guides; no phase/unit correction
        Timestamp(scheduler, 1);
        const vk::ClearColorValue poison{std::array<float, 4>{-1234.f, -1234.f, -1234.f, -1234.f}};
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        cmd.clearColorImage(output.image, vk::ImageLayout::eGeneral, poison, range);
        image_barrier(color.image, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
            vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead);
        for(const auto* guide:{&scene_depth,&mv})
            image_barrier(guide->image,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,
                vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderWrite,
                vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferRead);
        if(diagnostic) {
        guide_copy(scene_depth.image,vk::ImageAspectFlagBits::eColor,7372800);
        guide_copy(mv.image,vk::ImageAspectFlagBits::eColor,11059200);
        const vk::BufferMemoryBarrier2 guides_to_host{
            .srcStageMask=vk::PipelineStageFlagBits2::eTransfer,.srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .buffer=guide_readback->Handle(),.offset=0,.size=18432000};
        cmd.pipelineBarrier2({.bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&guides_to_host});
        RecordValidation(instance, scheduler, color.view, color.view); // input-only: source bound twice
        if (full) { copy_readback(color.image, 0); host_barrier(0, bytes32); }
        }
        for (const auto& image : resources)
            image_barrier(image.image, family, VK_QUEUE_FAMILY_EXTERNAL,
                vk::PipelineStageFlagBits2::eAllCommands,
                vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                vk::PipelineStageFlagBits2::eBottomOfPipe, {});
        input_tick = scheduler.CurrentTick();
        last_submitted_tick = outstanding_tick = input_tick;
        std::shared_ptr<P3Clock::Record> input_clock;
        try { input_clock=std::make_shared<P3Clock::Record>(scheduler.P3ClockContext(),
            P3Clock::OpaqueKey(static_cast<VkSemaphore>(scheduler.GetWorkSemaphore()->Handle())),input_tick); }
        catch(...) {} // optional telemetry allocation cannot change input correctness/fallback
        SubmitInfo input_submit{};input_submit.p3_input_clock=input_clock;
        const auto input_flush_start = std::chrono::steady_clock::now();
        scheduler.Flush(input_submit); // same ONE Flush; only optional shared data attached
        input_flush_ms = ms_since(input_flush_start);
        const auto input_wait_start = std::chrono::steady_clock::now();
        if(input_clock)input_clock->WaitEntered();
        const bool input_complete=WaitSubmitted(instance, scheduler, input_tick, hook_deadline);
        if(input_clock)input_clock->WaitObserved(input_complete); // only existing proof, no extra IsFree
        if (!input_complete) throw AcceptanceExpired("P0 input GPU deadline native acceptance deadline");
        last_completed_tick = input_tick; outstanding_tick = 0;
        input_wait_ms = ms_since(input_wait_start);
        const auto input_stats = diagnostic ? ReadValidation() : C4FrameValidationStats{};
        if (diagnostic && (input_stats.source_rgba_finite != pixels * 4 || input_stats.source_alpha_finite != pixels))
            throw std::runtime_error("P0 actual GPU source contains nonfinite values");
        Json guide_conversion,guide_digests;
        const auto guide_leaf=P0Wire::FrameDir(run_dir,frame,frame_count);
        const auto save_binary=[&](const std::string& basename,const uint8_t* data,size_t size) {
            std::ofstream out(guide_leaf+"/"+basename,std::ios::binary);out.write(reinterpret_cast<const char*>(data),size);
            if(!out)throw std::runtime_error("P0 local evidence write failed "+basename);
        };
        if(diagnostic) {
        guide_readback->Invalidate(0,18432000);
        const auto* guide_bytes=guide_readback->mapped_data.data();
        const auto* actual_depth=reinterpret_cast<const uint32_t*>(guide_bytes);
        const auto* actual_mv=reinterpret_cast<const uint16_t*>(guide_bytes+3686400);
        std::vector<uint32_t> expected_mv(1843200);
        uint64_t depth_finite=0,depth_range=0;
        for(size_t i=0;i<921600;++i) {
            float d;std::memcpy(&d,&actual_depth[i],4);
            depth_finite+=std::isfinite(d);depth_range+=std::isfinite(d)&&d>=0&&d<=1;
        }
        for(size_t i=0;i<expected_mv.size();++i) expected_mv[i]=HalfBitsToFloatBits(actual_mv[i]);
        const auto* converted_depth=guide_bytes+7372800;const auto* converted_mv=guide_bytes+11059200;
        auto mismatch=[](const uint8_t* a,const uint8_t* b,size_t size) {
            uint64_t n=0;for(size_t i=0;i<size;++i)n+=a[i]!=b[i];return n;
        };
        const auto depth_mismatches=mismatch(guide_bytes,converted_depth,3686400);
        const auto mv_mismatches=mismatch(reinterpret_cast<const uint8_t*>(expected_mv.data()),converted_mv,7372800);
        guide_conversion={
            {"depth",{{"compared_bytes",3686400u},{"mismatch_bytes",depth_mismatches},
                       {"expected_digest",Hex64(Fnv1a(guide_bytes,3686400))},
                       {"converted_digest",Hex64(Fnv1a(converted_depth,3686400))},
                       {"finite",depth_finite},{"device_z_in_0_1",depth_range}}},
            {"mv",{{"compared_bytes",7372800u},{"mismatch_bytes",mv_mismatches},
                    {"expected_digest",Hex64(Fnv1a(reinterpret_cast<const uint8_t*>(expected_mv.data()),7372800))},
                    {"converted_digest",Hex64(Fnv1a(converted_mv,7372800))}}}};
        save_binary("native-source-depth-f"+std::to_string(frame)+".f32",guide_bytes,3686400);
        save_binary("native-source-mv-f"+std::to_string(frame)+".rg16f",guide_bytes+3686400,3686400);
        save_binary("native-depth-f"+std::to_string(frame)+".r32f",converted_depth,3686400);
        save_binary("native-mv-f"+std::to_string(frame)+".rg32f",converted_mv,7372800);
        AtomicWrite(guide_leaf+"/native-guide-conversion-f"+std::to_string(frame)+".json",guide_conversion.dump());
        if(depth_finite!=921600||depth_range!=921600||depth_mismatches||mv_mismatches)
            throw std::runtime_error("P0 actual source-to-converted guide byte/range validation failed");
        guide_digests={
            {"depth",{{"algorithm","fnv1a64_r32f_bytes"},{"bytes",3686400u},{"digest",Hex64(Fnv1a(converted_depth,3686400))}}},
            {"mv",{{"algorithm","fnv1a64_rg32f_bytes"},{"bytes",7372800u},{"digest",Hex64(Fnv1a(converted_mv,7372800))}}}};
        }
        std::string input_digest;
        u64 source_rgb_negative = 0;
        if (full) {
            readback->Invalidate(0, bytes32);
            const auto* source = readback->mapped_data.data();
            input_digest = Hex64(Fnv1a(source, bytes32));
            for (u64 i = 0; i < pixels * 4; ++i) {
                if ((i & 3) == 3) continue;
                float value; std::memcpy(&value, source + i * 4, 4);
                source_rgb_negative += std::isfinite(value) && value < 0;
            }
            const auto cpu_signature = C4SignatureRgba32(reinterpret_cast<const uint32_t*>(source), width, height);
            if (cpu_signature.xor32 != input_stats.source_xor || cpu_signature.sum32 != input_stats.source_sum)
                throw std::runtime_error("P0 independent CPU source signature disagrees with GPU");
            std::ofstream dump(P0Wire::FrameDir(run_dir,frame,frame_count) + "/native-source-f" + std::to_string(frame) + ".rgba32f", std::ios::binary);
            dump.write(reinterpret_cast<const char*>(source), bytes32);
            if (!dump) throw std::runtime_error("P0 native full source dump failed");
        }
        Json input = Envelope(session, mode, execution_mode, "input", "INPUT_READY", frame, source_game_frame);
        input["prev_source_game_frame"] = previous_source;
        input["history_version"] = history_version; input["params_version"] = 1u;
        input["nr_reset"] = nr_reset; input["reset_reason"] = reset_reason;
        input["audit_kind"] = audit_kind;
        input["validation_profile"] = P0Wire::Validation(execution_mode);
        if (diagnostic) {
            input["signature_algorithm"] = "rgba32f_mix32_xor_sum_v1";
            input["input_signature_xor32"] = input_stats.source_xor;
            input["input_signature_sum32"] = input_stats.source_sum;
        } else {
            input["numerical_validation"] = "NOT_CHECKED"; input["pixel_validation"] = "NOT_RUN";
            input["signature"] = "NOT_RUN"; input["dump_count"] = 0u;
        }
        input["hdr"] = true; input["exposure_source"] = "unknown"; input["exposure_valid"] = false;
        input["source_reset"] = source_reset; input["source_frame_ms"] = source_frame_ms;
        auto add_jitter = [&](std::string_view prefix, const std::array<float, 2>& value) {
            input[std::string(prefix) + "_x"] = value[0]; input[std::string(prefix) + "_y"] = value[1];
        };
        add_jitter("viewport_jitter", viewport_jitter); add_jitter("previous_viewport_jitter", previous_viewport_jitter);
        add_jitter("sr_jitter", sr_jitter); add_jitter("previous_sr_jitter", previous_sr_jitter);
        input["valid_rect"] = {{"x", 0}, {"y", 0}, {"width", width}, {"height", height}};
        input["depth_inverted"] = false; input["sr_mvec_scale_x"] = 1.0; input["sr_mvec_scale_y"] = 1.0;
        input["nr_jitter_parameter_present"] = false;
        input["manual_white_point"] = 1.0; input["passthrough"] = false;
        input["sr_initial_reset_observed"] = sr_initial_reset_observed;
        input["sr_initial_source_game_frame"] = sr_initial_source_game_frame;
        input["sr_history_jitter_enabled_from_start"] = sr_history_jitter_enabled_from_start;
        input["input_descriptors"] = source_descriptors;
        if(diagnostic) input["guide_input_digests"] = guide_digests;
        if (full) {
            input["input_digest_algorithm"] = "fnv1a64_rgba32f_bytes";
            input["input_digest_bytes"] = bytes32; input["input_digest"] = input_digest;
        }
        const auto suffix = session + "-g0-f" + std::to_string(frame) + ".json";
        const auto leaf=P0Wire::FrameDir(run_dir,frame,frame_count);
        const auto input_path = leaf + "/input-" + suffix;
        if (HasFile(input_path)) throw std::runtime_error("P0 duplicate input tuple");
        const auto input_wire = input.dump();
        const auto wire_input = StrictJson(input_wire, "published P0 input");
        const auto complete_deadline = hook_deadline;
        const auto input_published_ns = monotonic_ns();
        const auto peer_wait_start = std::chrono::steady_clock::now();
        pending_input=wire_input;
        late_completion_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
        AtomicWrite(input_path, input_wire);
        const auto input_publish_completed_ns = monotonic_ns(); // successful rename has returned
        std::printf("P0_INPUT_READY mode=%s session=%s execution_mode=%s frame=%u source_game_frame=%llu prev_source_game_frame=%llu source_reset=%u nr_reset=%u reset_reason=%s audit_kind=%s input_tick=%llu actual_jitter_recorded=1 pixel_scope=%s signature=%s\n",
            mode.c_str(), session.c_str(), execution_mode.c_str(), frame, (unsigned long long)source_game_frame,
            (unsigned long long)previous_source, source_reset, nr_reset, reset_reason.c_str(),
            audit_kind.c_str(), (unsigned long long)input_tick, diagnostic ? "FULL" : "NOT_RUN", diagnostic ? "FULL_GPU_AND_FNV" : "NOT_RUN");
        const auto complete_path = leaf + "/complete-" + suffix;
        std::string complete_text;
        u32 completion_poll_attempts = 0, completion_poll_sleeps = 0;
        const auto completion_poll_started = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() < complete_deadline) {
            if (process.Completed() || process.SetupFailed()) throw std::runtime_error("P0 helper exited before completion");
            ++completion_poll_attempts;
            complete_text = Read(complete_path);
            if (!complete_text.empty() && std::chrono::steady_clock::now() < complete_deadline) break;
            complete_text.clear();
            ++completion_poll_sleeps;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto completion_poll_elapsed_ms = ms_since(completion_poll_started);
        if (complete_text.empty() || std::chrono::steady_clock::now() >= complete_deadline) {
            std::fprintf(stderr, "P0_COMPLETION_POLL_EXPIRED frame=%u source_game_frame=%llu attempts=%u sleeps=%u interval_ms=1 elapsed_ms=%.6f deadline_ms=%u\n",
                frame, (unsigned long long)source_game_frame, completion_poll_attempts,
                completion_poll_sleeps, completion_poll_elapsed_ms, acceptance_budget_ms);
            AtomicWrite(leaf + "/native-expired-" + suffix, wire_input.dump());
            throw AcceptanceExpired("P0 current tuple expired at native acceptance deadline; late output forbidden");
        }
        peer_wait_ms = ms_since(peer_wait_start);
        const auto complete_observed_ns = monotonic_ns();
        const Json complete = StrictJson(complete_text, complete_path);
        ValidateEnvelope(complete, session, mode, execution_mode, "complete", skip ? "INVALID" : noop ? "NOOP_VALID" : "OUTPUT_VALID",
                         frame, source_game_frame, !skip, !skip && !noop);
        const std::set<std::string> tuple_keys{"session", "mode", "generation", "frame", "resource_set",
            "source_game_frame", "profile_id", "width", "height", "render_width", "render_height",
            "op", "status", "completed_GPU", "output_valid", "model_valid"};
        for (const auto& [key, value] : wire_input.items()) {
            if (tuple_keys.contains(key)) continue;
            if (!complete.contains(key) || complete.at(key).type() != value.type() || complete.at(key) != value)
                throw std::runtime_error("P0 exact input echo mismatch: " + key);
        }
        const auto peer_serial=RequireUnsigned(complete,"GPU_fence_serial");
        if(!peer_serial||peer_serial<=last_peer_fence_serial) throw std::runtime_error("P0 actual peer GPU fence serial missing/replayed");
        last_peer_fence_serial=peer_serial;
        complete_GPU_observed=true;
        last_helper_evaluate_ms.reset();
        if(complete.contains("gpu_timestamps")) {
            const auto& times=complete.at("gpu_timestamps");
            if(RequireBool(times,"available")&&times.contains("evaluate_commands_ms")&&!times.at("evaluate_commands_ms").is_null()) {
                const double value=P0Wire::Number(times,"evaluate_commands_ms");
                if(value<0) throw std::runtime_error("P0 negative actual GPU duration");
                last_helper_evaluate_ms=value;
            }
        }
        const auto count = [&](const char* key) { return RequireUnsigned(complete, key); };
        const bool model_initialized=RequireBool(complete,"model_init");
        const bool feature_created=RequireBool(complete,"feature_created");
        const bool normal_model=mode!="noop" && count("reset")==nr_reset &&
            RequireString(complete,"status")=="OUTPUT_VALID"&&RequireBool(complete,"output_valid")&&RequireBool(complete,"model_valid")&&
            model_initialized&&feature_created&&count("evaluate_calls")==1&&
            count("evaluate_result")==1&&count("resolve_calls")==1&&count("evaluate_calls_total")==frame+1&&count("resolve_calls_total")==frame+1;
        const bool noop_accept=noop&&RequireString(complete,"status")=="NOOP_VALID"&&RequireBool(complete,"output_valid")&&
            !RequireBool(complete,"model_valid")&&!model_initialized&&!feature_created&&count("copy_calls")==1&&
            count("evaluate_calls")==0&&count("evaluate_result")==0&&count("resolve_calls")==0&&
            count("evaluate_calls_total")==0&&count("resolve_calls_total")==0&&RequireString(complete,"model_validation")=="NOT_RUN";
        const bool skip_accept=skip&&!normal_model&&RequireString(complete,"status")=="INVALID"&&
            !RequireBool(complete,"output_valid")&&!RequireBool(complete,"model_valid")&&model_initialized&&feature_created&&
            count("evaluate_calls")==0&&count("evaluate_result")==0&&count("resolve_calls")==0&&
            count("evaluate_calls_total")==0&&count("resolve_calls_total")==0&&count("reset")==nr_reset;
        if(skip?!skip_accept:noop?!noop_accept:!normal_model)
            throw std::runtime_error("P0 separate model/noop/invalid API/exact tuple acceptance failed");
        if(complete.at("parameter_application")!=P0Wire::ParameterApplication()) throw std::runtime_error("P0 false application claim");
        Json gv;
        if(diagnostic) {
        P0Wire::ValidateDiagnosticReadbackAccounting(complete);
        if(RequireString(complete,"validation_profile")!=P0Wire::Validation(execution_mode)||RequireString(complete,"model_acceptance_scope")!=
               (noop?"GPU_copy_and_full_diagnostic_pixels_phase_UNVERIFIED":"API_GPU_tuple_and_full_diagnostic_pixels_phase_UNVERIFIED"))
            throw std::runtime_error("P0 actual diagnostic profile/readback scope mismatch");
        gv=complete.at("guide_validation");
        if(RequireString(gv,"scope")!="same_texel_transport_only_NR_phase_UNVERIFIED") throw std::runtime_error("P0 guide scope mismatch");
        for(const char* role:{"depth","mv"}) {
            const auto& v=gv.at(role);const auto& expected=guide_digests.at(role);
            if(RequireString(v,"algorithm")!=RequireString(expected,"algorithm")||RequireUnsigned(v,"bytes")!=RequireUnsigned(expected,"bytes")||
               RequireString(v,"digest")!=RequireString(expected,"digest")||!RequireBool(v,"source_digest_match")||
               RequireUnsigned(v,"finite")!=(std::string_view(role)=="depth"?921600u:1843200u))
                throw std::runtime_error("P0 independent Wine guide readback differs from current native payload");
        }
        if(RequireUnsigned(gv.at("depth"),"samples")!=921600||RequireUnsigned(gv.at("depth"),"device_z_in_0_1")!=921600||
           RequireUnsigned(gv.at("mv"),"components")!=1843200) throw std::runtime_error("P0 guide full count mismatch");
        const bool common_evidence=count("source_rgba_finite")==pixels*4&&count("source_alpha_finite")==pixels&&
            count("source_rgb_negative")==source_rgb_negative&&count("source_rgba_nonfinite")==0&&
            count("source_alpha_total")==pixels&&count("visited_pixels")==pixels&&count("resolved_rgba_total")==pixels*4&&
            RequireBool(complete,"current_input_signature_match")&&RequireString(complete,"signature_algorithm")=="rgba32f_mix32_xor_sum_v1"&&
            count("input_signature_xor32")==input_stats.source_xor&&count("input_signature_sum32")==input_stats.source_sum&&
            count("output_signature_xor32")<=UINT32_MAX&&count("output_signature_sum32")<=UINT32_MAX&&count("resolved_rgba_finite")==pixels*4;
        if(!common_evidence) throw std::runtime_error("P0 actual shared source/output full counter/signature mismatch");
        if(noop) {
            for(const auto& [key,value]:complete.items()) if(key.starts_with("raw_")) throw std::runtime_error("P0 noop reports inapplicable raw model counter");
            if(count("source_alpha_preserved")!=pixels) throw std::runtime_error("P0 noop alpha mismatch");
        } else {
            if(count("raw_rgb_total")!=pixels*3 || P0Wire::Number(complete,"sentinel")!=-1234.0)
                throw std::runtime_error("P0 raw/sentinel evidence missing");
            if(skip) {
                if(count("raw_rgb_nan")!=pixels*3||count("raw_rgb_finite")||count("raw_rgb_written")||
                   count("resolved_rgba_written")||count("output_sentinel_count")!=pixels*4)
                    throw std::runtime_error("P0 skipped Evaluate poison gate failed; invalid not accepted");
            } else if(count("raw_rgb_finite")!=pixels*3||count("raw_rgb_written")!=pixels*3||count("raw_rgb_nan")||
                      count("resolved_rgba_written")!=pixels*4||count("source_alpha_preserved")!=pixels||count("output_sentinel_count"))
                throw std::runtime_error("P0 actual NR full finite/written/alpha evidence failed");
        }
        if(!RequireBool(complete,"current_input_digest_match")||RequireString(complete,"input_digest")!=input_digest||
           RequireString(complete,"input_digest_algorithm")!="fnv1a64_rgba32f_bytes"||count("input_digest_bytes")!=bytes32||
           RequireString(complete,"output_digest_algorithm")!="fnv1a64_rgba32f_bytes"||count("output_digest_bytes")!=bytes32)
            throw std::runtime_error("P0 current full source/output FNV identity mismatch");
        } else {
            if(RequireString(complete,"numerical_validation")!="NOT_CHECKED"||RequireString(complete,"pixel_validation")!="NOT_RUN"||RequireString(complete,"signature")!="NOT_RUN"||count("dump_count")||count("helper_pixel_readback_bytes")||count("gpu_validator_readback_bytes")||(complete.contains("guide_readback_bytes")&&count("guide_readback_bytes"))) throw std::runtime_error("P0 hot diagnostic claim/readback forbidden");
            for(const auto& [key,value]:complete.items()) if(key.find("digest")!=std::string::npos||key.starts_with("input_signature")||key.starts_with("output_signature")||key=="guide_validation"||key=="source_alpha_preserved") throw std::runtime_error("P0 hot diagnostic-only field");
        }
        if (std::chrono::steady_clock::now() >= complete_deadline)
            throw AcceptanceExpired("P0 completion verification exceeded active deadline; not accepted");
        output_sample_started = true;
        cmd = scheduler.CommandBuffer(); // previous input Flush invalidated every old command handle
        for (const auto* image : {&color, &output})
            if (diagnostic || image == &output)
            image_barrier(image->image, VK_QUEUE_FAMILY_EXTERNAL, family,
                vk::PipelineStageFlagBits2::eAllCommands, {},
                vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead);
        if (full) { copy_readback(output.image, bytes32); host_barrier(bytes32, bytes32); }
        vk::ImageView accepted_view{};
        if (!skip) {
            const vk::ImageMemoryBarrier2 to_write{
                .srcStageMask = local_return_initialized ? vk::PipelineStageFlagBits2::eAllCommands : vk::PipelineStageFlagBits2::eTopOfPipe,
                .srcAccessMask = local_return_initialized ? vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite : vk::AccessFlags2{},
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader, .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .oldLayout = local_return_initialized ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = vk::Image(local_return), .subresourceRange = range};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_write});
            cmd.fillBuffer(conversion_guard->Handle(), 0, 4, 0);
            const vk::BufferMemoryBarrier2 clear_guard{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = conversion_guard->Handle(), .offset = 0, .size = 4};
            cmd.pipelineBarrier2({.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &clear_guard});
            Timestamp(scheduler, 2);
            update_conversion(); conversion(1);
            Timestamp(scheduler, 3);
            const vk::BufferMemoryBarrier2 guard_to_host{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer | vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost, .dstAccessMask = vk::AccessFlagBits2::eHostRead,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = conversion_guard->Handle(), .offset = 0, .size = 4};
            cmd.pipelineBarrier2({.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &guard_to_host});
            if (full) {
                image_barrier(vk::Image(local_return), VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
                copy_readback(vk::Image(local_return), bytes32 * 2); host_barrier(bytes32 * 2, bytes16);
            }
            accepted_view = *local_return_view;
        }
        if (!skip) {
            image_barrier(vk::Image(local_return), VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferRead,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
        }
        if (diagnostic) RecordValidation(instance, scheduler, color.view, output.view, !skip);
        for (const auto* image : {&color, &output})
            if (diagnostic || image == &output)
            image_barrier(image->image, family, VK_QUEUE_FAMILY_EXTERNAL,
                vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead,
                vk::PipelineStageFlagBits2::eBottomOfPipe, {});
        const u64 output_tick = scheduler.CurrentTick();
        last_submitted_tick = outstanding_tick = output_tick;
        const auto output_flush_start = std::chrono::steady_clock::now();
        scheduler.Flush();
        output_flush_ms = ms_since(output_flush_start);
        const auto output_wait_start = std::chrono::steady_clock::now();
        if (!WaitSubmitted(instance, scheduler, output_tick, hook_deadline)) throw AcceptanceExpired("P0 output/validation GPU deadlinenative acceptance deadline");
        last_completed_tick = output_tick; outstanding_tick = 0;
        output_wait_ms = ms_since(output_wait_start);
        u32 guard_flag = 0;
        if (!skip) {
            conversion_guard->Invalidate(0, 4);
            std::memcpy(&guard_flag, conversion_guard->mapped_data.data(), 4);
            if (guard_flag) throw std::runtime_error("P0 finite_abs_le65504 conversion guard failed; entire frame SR fallback; not clamped");
        }
        const auto output_stats = diagnostic ? ReadValidation() : C4FrameValidationStats{};
        std::array<u32, 4> half_gpu_stats{};
        if (diagnostic && !skip) {
            return_validation_stats->Invalidate(0, 16);
            std::memcpy(half_gpu_stats.data(), return_validation_stats->mapped_data.data(), 16);
            if (half_gpu_stats[0] != pixels * 4 || half_gpu_stats[1] != pixels ||
                half_gpu_stats[2] || half_gpu_stats[3])
                throw std::runtime_error("P0 actual returned RGBA16F full-GPU finite guard failed; SR fallback");
        }
        if (diagnostic && (output_stats.source_rgba_finite != pixels * 4 || output_stats.source_alpha_finite != pixels ||
            output_stats.source_xor != input_stats.source_xor || output_stats.source_sum != input_stats.source_sum ||
            output_stats.out_xor != count("output_signature_xor32") || output_stats.out_sum != count("output_signature_sum32") ||
            output_stats.out_rgba_finite != pixels * 4 ||
            (skip ? (output_stats.out_sentinel_count != pixels * 4 || output_stats.out_rgba_written != 0) :
                    ((!noop && (output_stats.out_sentinel_count != 0 || output_stats.out_rgba_written != pixels * 4)) || output_stats.alpha_bits_equal != pixels))))
            throw std::runtime_error("P0 actual native full-GPU source/output/signature/alpha validation failed");
        if (full) {
            readback->Invalidate(0, bytes32 * 2 + (skip ? 0 : bytes16));
            const auto* source = readback->mapped_data.data();
            const auto* out = source + bytes32;
            const auto output_digest = Hex64(Fnv1a(out, bytes32));
            if (output_digest != RequireString(complete, "output_digest"))
                throw std::runtime_error("P0 native output full FNV differs from Wine");
            const auto cpu_signature = C4SignatureRgba32(reinterpret_cast<const uint32_t*>(out), width, height);
            if (cpu_signature.xor32 != output_stats.out_xor || cpu_signature.sum32 != output_stats.out_sum)
                throw std::runtime_error("P0 CPU output signature differs from actual GPU");
            const auto* source_words = reinterpret_cast<const uint32_t*>(source);
            const auto* out_words = reinterpret_cast<const uint32_t*>(out);
            u64 finite = 0, written = 0, alpha = 0;
            for (u64 i = 0; i < pixels * 4; ++i) {
                float value; std::memcpy(&value, &out_words[i], 4);
                finite += std::isfinite(value); written += std::isfinite(value) && value != -1234.f;
                if ((i & 3) == 3) alpha += source_words[i] == out_words[i];
            }
            if (finite != output_stats.out_rgba_finite || written != output_stats.out_rgba_written ||
                alpha != output_stats.alpha_bits_equal)
                throw std::runtime_error("P0 scheduled full CPU output counters differ from GPU");
            if(noop && std::memcmp(source,out,bytes32)!=0) throw std::runtime_error("P0 noop shared RGBA32F differs bitwise from current SR conversion");
            std::ofstream dump(leaf + "/native-resolve-f" + std::to_string(frame) + ".rgba32f", std::ios::binary);
            dump.write(reinterpret_cast<const char*>(out), bytes32);
            if (!dump) throw std::runtime_error("P0 native output dump failed");
            if (!skip) {
                const auto* half = reinterpret_cast<const uint16_t*>(source + bytes32 * 2);
                u64 half_finite = 0;
                for (u64 i = 0; i < pixels * 4; ++i) half_finite += (half[i] & 0x7c00u) != 0x7c00u;
                if (half_finite != pixels * 4 || half_finite != half_gpu_stats[0]) throw std::runtime_error("P0 scheduled return-half CPU/GPU finite audit failed");
                if(noop) {
                    readback->Invalidate(bytes32*2+bytes16,bytes16);
                    const auto* fallback=source+bytes32*2+bytes16;
                    if(std::memcmp(fallback,half,bytes16)!=0) throw std::runtime_error("P0 noop actual return half differs from immutable SR half");
                    save_binary("native-sr-f"+std::to_string(frame)+".rgba16f",fallback,bytes16);
                }
                std::ofstream half_dump(leaf + "/native-return-f" + std::to_string(frame) + ".rgba16f", std::ios::binary);
                half_dump.write(reinterpret_cast<const char*>(half), bytes16);
                if (!half_dump) throw std::runtime_error("P0 native return-half dump failed");
            }
        }
        if (std::chrono::steady_clock::now() >= complete_deadline)
            throw AcceptanceExpired("P0 current-frame validation exceeded deadline; no acceptance");
        if (!process.ExporterIdentityCurrent()) throw std::runtime_error("P0 helper changed before consumed");
        Json consumed = Envelope(session, mode, execution_mode, "consumed", "CONSUMED", frame, source_game_frame);
        consumed["accepted_for_merge"] = !skip;
        consumed["source_frame_valid"] = true;
        for (const char* key : {"prev_source_game_frame", "history_version", "params_version", "nr_reset", "reset_reason", "audit_kind", "validation_profile"})
            consumed[key] = wire_input.at(key);
        const auto consumed_path = leaf + "/consumed-" + suffix;
        if (HasFile(consumed_path)) throw std::runtime_error("P0 duplicate consumed tuple");
        AtomicWrite(consumed_path, consumed.dump());
        local_return_initialized |= !skip;
        pending_source_frame = source_game_frame; last_source_frame = source_game_frame;
        ++processed_frames; accepted_count += !skip; resume_reset_pending=false; pending_input=Json{}; complete_GPU_observed=false; frame_processed = true; returned_view = accepted_view;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - frame_started).count();
        Json frame_record = consumed;
        frame_record["status"] = skip ? "INVALID_REJECTED" : "ACCEPTED";
        frame_record["op"] = "native_validation"; frame_record["selected_output"] = !skip;
        if (diagnostic) {
        frame_record["signature_algorithm"] = "rgba32f_mix32_xor_sum_v1";
        frame_record["input_signature_xor32"] = input_stats.source_xor; frame_record["input_signature_sum32"] = input_stats.source_sum;
        frame_record["output_signature_xor32"] = output_stats.out_xor; frame_record["output_signature_sum32"] = output_stats.out_sum;
        frame_record["native_GPU_stats"] = {{"source_rgba_finite", output_stats.source_rgba_finite},
            {"source_alpha_finite", output_stats.source_alpha_finite}, {"out_rgba_finite", output_stats.out_rgba_finite},
            {"out_rgba_written", output_stats.out_rgba_written}, {"alpha_bits_equal", output_stats.alpha_bits_equal},
            {"output_sentinel_count", output_stats.out_sentinel_count}, {"visited_pixels", output_stats.visited_pixels}};
        frame_record["native_return_half_finite"] = half_gpu_stats[0];
        frame_record["native_return_half_visited"] = half_gpu_stats[1];
        }
        if(diagnostic) {
        frame_record["guide_source_conversion"] = guide_conversion;
        frame_record["guide_input_digests"] = guide_digests;
        frame_record["guide_validation"] = gv;
        }
        frame_record["noop_half_bitexact"] = noop;
        frame_record["source_descriptors"] = source_descriptors;
        if(diagnostic) {frame_record["source_rgb_negative"] = source_rgb_negative;frame_record["source_rgba_nonfinite"] = 0u;}
        frame_record["input_tick"] = input_tick; frame_record["output_tick"] = output_tick;
        frame_record["native_elapsed_ms"] = elapsed;
        frame_record["native_soft_deadline_ms"] = acceptance_budget_ms;
        frame_record["validation_profile"] = P0Wire::Validation(execution_mode);
        frame_record["numerical_validation"] = diagnostic ? "FULL_GPU_AND_CPU" : "NOT_CHECKED";
        frame_record["pixel_validation"] = diagnostic ? "PASS" : "NOT_RUN";
        if (!diagnostic) { frame_record["signature"] = "NOT_RUN"; frame_record["dump_count"] = 0u; }
        frame_record["guard_flag"] = skip ? Json(nullptr) : Json(guard_flag);
        frame_record["guard_readback_bytes"] = skip ? 0 : 4;
        frame_record["guard_profile"] = "finite_abs_le65504";
        frame_record["audit_readback_bytes"] = diagnostic?bytes32*2+(skip?0:bytes16)+18432000+(skip?128:148):4u;
        frame_record["input_observed_cpu_monotonic_ns"] = frame_start_monotonic_ns;
        frame_record["input_published_cpu_monotonic_ns"] = input_published_ns; // legacy BEFORE-write point
        frame_record["input_published_cpu_monotonic_scope"] = "legacy_before_AtomicWrite_not_publication_completion";
        frame_record["input_publish_completed_cpu_monotonic_ns"] = input_publish_completed_ns;
        frame_record["input_publish_completed_scope"] = "native_AtomicWrite_close_and_successful_atomic_rename_returned_not_peer_observation";
        frame_record["input_publication_cpu_ms"] = double(input_publish_completed_ns-input_published_ns)/1e6;
        frame_record["published_to_complete_observed_cpu_ms"] = double(complete_observed_ns-input_publish_completed_ns)/1e6;
        frame_record["CPU_clock_scope"] = "same_native_steady_clock_no_Helper_clock_synchronization";
        frame_record["complete_observed_cpu_monotonic_ns"] = complete_observed_ns;
        const auto selection_ready_ns=monotonic_ns();
        frame_record["selection_ready_cpu_monotonic_ns"] = selection_ready_ns;
        frame_record["selection_ready_scope"] = "native_validation_and_consumed_complete_before_caller_selection_and_merge_not_Present";
        frame_record["publication_clock_order_valid"] = P0Timing::PublishOrder(
            input_published_ns,input_publish_completed_ns,complete_observed_ns,selection_ready_ns);
        frame_record["completion_poll"] = {{"interval_ms", 1u},
            {"attempts", completion_poll_attempts}, {"sleeps", completion_poll_sleeps},
            {"elapsed_ms", completion_poll_elapsed_ms}, {"clock", "steady_clock monotonic"},
            {"scope", "active complete-file reads, peer status checks and sleeps; subset of peer_completion_wait"}};
        frame_record["cpu_phase_ms"] = {{"prior_merge_wait", prior_merge_wait_ms}, {"input_flush", input_flush_ms},
            {"input_wait", input_wait_ms}, {"peer_completion_wait", peer_wait_ms}, {"output_flush", output_flush_ms},
            {"output_wait", output_wait_ms}, {"source_to_selection_ready", ms_since(frame_started)}};
        const auto input_gpu_ms = QueryDuration(0, 1);
        const auto output_gpu_ms = skip ? std::optional<double>{} : QueryDuration(2, 3);
        frame_record["input_conversion_gpu_ms"] = input_gpu_ms ? Json(*input_gpu_ms) : Json(nullptr);
        frame_record["output_conversion_and_fused_guard_gpu_ms"] = output_gpu_ms ? Json(*output_gpu_ms) : Json(nullptr);
        frame_record["guard_overhead_ms"] = nullptr;
        frame_record["guard_overhead_scope"] = "fused mandatory conversion, not separately timed";
        frame_record["timestamp_valid_bits"] = timestamp_valid_bits;
        frame_record["timestamp_period_ns"] = timestamp_period;
        frame_record["timestamp_wrap_handling"] = "delta masked to timestampValidBits";
        frame_record["input_timestamp_availability"] = input_gpu_ms ? "AVAILABLE" : "NOT_AVAILABLE";
        frame_record["output_timestamp_availability"] = output_gpu_ms ? "AVAILABLE" : "NOT_AVAILABLE";
        frame_record["query_readback_bytes"] = timing_pool ? (skip ? 32 : 64) : 0;
        frame_record["GPU_timing_scope"] = "same game queue conversion commands and dependencies; durations not cross-device clocks; no WAIT_BIT";
        // Local telemetry snapshot only. API return can still be pending after actual
        // target observation; do not wait, retry or gate correctness on this record.
        if(input_clock)try {
            const auto c=input_clock->Read();const auto bounds=P3Clock::WaitBounds(c);
            Json clock{{"status",bounds?"MEASURED_BOUNDS":"NOT_MEASURED_PENDING_OR_INVALID"},
                {"coherent",c.coherent},{"exact_context_timeline_tick",c.exact},
                {"native_scheduler_context",input_clock->context},{"native_timeline_key",input_clock->timeline},
                {"target_input_tick",input_clock->tick},{"actual_context",c.actual_context},
                {"actual_timeline",c.actual_timeline},{"actual_signal_tick",c.actual_tick},
                {"clock_scope","same_native_steady_clock_no_Wine_QPC"},
                {"submit_return_scope","CPU_driver_return_upper_bound_not_enqueue_instant"},
                {"completion_scope","existing_WaitSubmitted_verified_tick_host_observation_not_GPU_signal_instant"}};
            clock["submit_enter_cpu_monotonic_ns"]=(c.flags&P3Clock::Enter)?Json(c.enter):Json(nullptr);
            clock["submit_return_cpu_monotonic_ns"]=(c.flags&P3Clock::Returned)?Json(c.returned):Json(nullptr);
            clock["submit_api_result"]=(c.flags&P3Clock::Returned)?Json(c.api_result):Json(nullptr);
            clock["local_wait_enter_cpu_monotonic_ns"]=(c.flags&P3Clock::Wait)?Json(c.wait):Json(nullptr);
            clock["target_complete_observed_cpu_monotonic_ns"]=(c.flags&P3Clock::Verified)?Json(c.observed):Json(nullptr);
            if(bounds)clock["wait_bounds_ns"]={{"before_actual_submit_call",bounds->pre_call_wait_ns},
                {"inside_driver_call_unknown_enqueue",bounds->driver_call_overlap_ns},
                {"after_driver_return_queue_work_and_observation",bounds->post_return_wait_ns}};
            frame_record["p3_input_submit_clock"]=std::move(clock);
        } catch(...) {} // omit unavailable telemetry; never turn it into an ownership gate
        AtomicWrite(leaf + "/native-frame-" + suffix, frame_record.dump());
        std::printf("P0_FRAME_%s mode=%s session=%s execution_mode=%s frame=%u source_game_frame=%llu prev_source_game_frame=%llu nr_reset=%u reset_reason=%s audit_kind=%s guard_flag=%u guard_readback_bytes=%u input_tick=%llu output_tick=%llu elapsed_ms=%lld SR_fallback=%u pixel_scope=%s\n",
            skip ? "INVALID_REJECTED" : "ACCEPTED", mode.c_str(), session.c_str(), execution_mode.c_str(), frame,
            (unsigned long long)source_game_frame, (unsigned long long)previous_source, nr_reset,
            reset_reason.c_str(), audit_kind.c_str(), guard_flag, skip ? 0u : 4u,
            (unsigned long long)input_tick, (unsigned long long)output_tick, (long long)elapsed, skip,
            diagnostic ? "full_C3_diagnostic" : "API_GPU_identity_plus_conservative_guard_only");
        return returned_view;
    } catch (const AcceptanceExpired& e) {
        if(output_sample_started) { Quarantine("P0 acceptance expired after native output use: abnormal held ownership, no false no_sample discard"); return {}; }
        soft_expired=true;late_completion_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);BeginStop("unsupported");Quarantine(e.what());return VK_NULL_HANDLE;
    } catch (const std::exception& e) { Quarantine(e.what());return VK_NULL_HANDLE; }
}

void P0GameBridge::Timestamp(Scheduler& scheduler, u32 index) {
    if (timing_pool) scheduler.CommandBuffer().writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *timing_pool, index);
}

std::optional<double> P0GameBridge::QueryDuration(u32 begin, u32 end) {
    if (!timing_pool || end != begin + 1) return std::nullopt;
    std::array<uint64_t, 4> results{}; // value, availability for two consecutive queries
    const auto code = timing_device.getQueryPoolResults(*timing_pool, begin, 2,
        sizeof(results), results.data(), 2 * sizeof(uint64_t),
        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
    if (code != vk::Result::eSuccess || !results[1] || !results[3]) return std::nullopt;
    const uint64_t mask = timestamp_valid_bits == 64 ? UINT64_MAX : ((uint64_t{1} << timestamp_valid_bits) - 1);
    return double((results[2] - results[0]) & mask) * timestamp_period / 1e6;
}

void P0GameBridge::BeginMergeTiming(Scheduler& scheduler) { Timestamp(scheduler, 4); }
void P0GameBridge::EndMergeTiming(Scheduler& scheduler) { Timestamp(scheduler, 5); }
void P0GameBridge::ReadMergeTiming(u32 frame, u64 source) {
    Json data{{"session", session}, {"frame", frame}, {"source_game_frame", source},
              {"protocol_version", 6u}, {"transport_layout",P0Wire::Layout}, {"execution_mode", execution_mode},
              {"timestamp_valid_bits", timestamp_valid_bits}, {"timestamp_period_ns", timestamp_period},
              {"wrap_handling", "delta masked to timestampValidBits"},
              {"scope", "game-queue merge commands and dependencies; not pure model; no WAIT_BIT"},
              {"query_result_bytes", timing_pool ? 32 : 0}};
    const auto value = QueryDuration(4, 5);
    data["merge_gpu_ms"] = value ? Json(*value) : Json(nullptr);
    data["availability"] = value ? "AVAILABLE" : "NOT_AVAILABLE";
    AtomicWrite(P0Wire::FramePath(run_dir,frame,frame_count,"native-merge-timing-"+session+"-g0-f"+std::to_string(frame)+".json"), data.dump());
}

void P0GameBridge::PrepareReturnForMerge(Scheduler& scheduler) {
    if (!frame_processed || !returned_view || quarantined || mode == "skipEvaluate")
        throw std::runtime_error("P0 return-image dependency requested without accepted resident output");
    const vk::ImageMemoryBarrier2 return_to_merge{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader |
                        vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite |
                         vk::AccessFlagBits2::eTransferRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = vk::Image(local_return),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    scheduler.CommandBuffer().pipelineBarrier2({.imageMemoryBarrierCount = 1,
                                                 .pImageMemoryBarriers = &return_to_merge});
    std::printf("P0_RETURN_MERGE_BARRIER mode=%s source_game_frame=%llu src=COMPUTE|TRANSFER/SHADER_WRITE|TRANSFER_READ dst=COMPUTE/SHADER_READ\n",
                mode.c_str(), static_cast<unsigned long long>(pending_source_frame.value_or(0)));
}

void P0GameBridge::DeferRetirement(const Instance&, Scheduler& scheduler) {
    if (!pending_source_frame || finished || quarantined) return;
    pending_merge_tick=scheduler.CurrentTick();
    std::printf("P0_MERGE_LAST_USE frame=%u source_game_frame=%llu tick=%llu submitted_yet=0\n",
       processed_frames-1,(unsigned long long)*pending_source_frame,(unsigned long long)pending_merge_tick);
    if(processed_frames>=frame_count) BeginStop("budget");
}

bool P0GameBridge::NativeUsesComplete(Scheduler& scheduler) {
    if (outstanding_tick) {
        if (!scheduler.IsFree(outstanding_tick)) return false;
        last_completed_tick=std::max(last_completed_tick,outstanding_tick);
        outstanding_tick=0;
    }
    if (pending_merge_tick) {
        if (pending_merge_tick>=scheduler.CurrentTick()) {
            scheduler.EndRendering();
            last_submitted_tick=outstanding_tick=scheduler.CurrentTick();
            scheduler.Flush(); // Submit the recorded merge before any completion check.
        }
        if(!scheduler.IsFree(pending_merge_tick)) return false;
        last_completed_tick=std::max(last_completed_tick,pending_merge_tick);
        if(pending_source_frame) ReadMergeTiming(processed_frames-1,*pending_source_frame);
        pending_merge_tick=0;pending_source_frame.reset();returned_view=nullptr;
        if(outstanding_tick&&scheduler.IsFree(outstanding_tick)) outstanding_tick=0;
    }
    return !outstanding_tick;
}

bool P0GameBridge::RequestControl(std::string_view action,std::string_view reason) {
    if(!prepared||finished||retire_requested||control_request||!pending_input.empty()||
       outstanding_tick||pending_merge_tick||abort_sent) return false;
    if(quarantined&&!soft_expired) return false;
    if(action=="idle"&&idle) return true;
    if(action=="resume"&&!idle) return false;
    const std::set<std::string_view> actions{"idle","resume","drain","stop"};
    const std::set<std::string_view> reasons{"enable_off","enable_on","pause","resume","load","unsupported","exit","budget"};
    if(!actions.contains(action)||!reasons.contains(reason)||control_epoch==UINT64_MAX)
        throw std::runtime_error("P0 invalid control action/reason/epoch");
    Json request{{"protocol_version",6u},{"profile_id",ProfileId},{"execution_mode",execution_mode},
        {"session",session},{"generation",0u},{"resource_set",session+"-g0"},
        {"kind","control"},{"state","REQUESTED"},{"control_epoch",control_epoch+1},
        {"action",action},{"completed_count",processed_frames},{"next_frame",processed_frames},
        {"source_game_frame",last_source_frame},{"source_frame_valid",processed_frames!=0},
        {"history_version",history_version+(action=="resume")},{"params_version",1u},{"reason",reason}};
    control_request=StrictJson(request.dump(),"P0 own control wire roundtrip");
    AtomicWrite(run_dir+"/control-request-"+session+"-g0.json",control_request->dump());
    managed_control_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
    std::printf("P0_CONTROL_REQUEST action=%.*s reason=%.*s session=%s epoch=%llu finalized=%u source=%llu native_last_use_complete=1\n",
      int(action.size()),action.data(),int(reason.size()),reason.data(),session.c_str(),
      (unsigned long long)(control_epoch+1),processed_frames,(unsigned long long)last_source_frame);
    return true;
}

void P0GameBridge::PollManaged(const Instance& instance,Scheduler& scheduler,P0NrProcess& process) {
    if(!prepared||retirement_audited||abort_sent) return;
    try {
        const auto now=std::chrono::steady_clock::now();
        const bool own_complete=NativeUsesComplete(scheduler);
        if(soft_expired&&!pending_input.empty()) {
            if(now>=late_completion_deadline) {
                soft_expired=false; // Unknown peer/GPU completion cannot use normal release.
                throw std::runtime_error("P0 late completion grace60s exhausted; resources held");
            }
            const u32 f=RequireUnsigned(pending_input,"frame");
            const u64 source=RequireUnsigned(pending_input,"source_game_frame");
            const auto complete_path=P0Wire::FramePath(run_dir,f,frame_count,
                "complete-"+session+"-g0-f"+std::to_string(f)+".json");
            if(!own_complete||!HasFile(complete_path)) return;
            const Json complete=StrictJson(Read(complete_path),complete_path);
            const auto status=RequireString(complete,"status");
            if(status!=(mode=="skipEvaluate"?"INVALID":"OUTPUT_VALID"))
                throw std::runtime_error("P0 late completion lacks expected released-frame proof");
            ValidateEnvelope(complete,session,mode,execution_mode,"complete",status,f,source,
                RequireBool(complete,"output_valid"),RequireBool(complete,"model_valid"));
            const std::set<std::string> operation{"op","status","output_valid","model_valid"};
            for(const auto&[key,value]:pending_input.items())
                if(!operation.contains(key)&&(!complete.contains(key)||complete.at(key).type()!=value.type()||complete.at(key)!=value))
                    throw std::runtime_error("P0 late exact input echo mismatch: "+key);
            const auto peer_serial=RequireUnsigned(complete,"GPU_fence_serial");
            if(!peer_serial || (complete_GPU_observed ? peer_serial!=last_peer_fence_serial : peer_serial<=last_peer_fence_serial))
                throw std::runtime_error("P0 late current peer GPU fence proof invalid");
            const bool negative=mode=="skipEvaluate";
            if(RequireBool(complete,"output_valid")!=!negative||RequireBool(complete,"model_valid")!=!negative||
               RequireUnsigned(complete,"evaluate_calls")!=(negative?0u:1u)||RequireUnsigned(complete,"evaluate_result")!=(negative?0u:1u)||
               RequireUnsigned(complete,"resolve_calls")!=(negative?0u:1u)) throw std::runtime_error("P0 late released frame API proof mismatch");
            last_peer_fence_serial=peer_serial;
            if(!process.ExporterIdentityCurrent()) throw std::runtime_error("P0 late peer identity changed");
            // No shared output sample/copy or merge occurs. A confirmed actual completion
            // plus all native-use ticks permits only discard and terminal drain.
            Json discard=pending_input;discard["op"]="discard";discard["status"]="DISCARDED";
            discard["kind"]="discard";discard["state"]="DISCARDED";
            discard["output_valid"]=false;discard["model_valid"]=false;
            discard["reason"]="native_deadline_expired";discard["no_sample_no_merge"]=true;
            discard["accepted_for_merge"]=false;discard["completed_GPU"]=true;
            AtomicWrite(P0Wire::FramePath(run_dir,f,frame_count,
                "discard-"+session+"-g0-f"+std::to_string(f)+".json"),discard.dump());
            ++processed_frames;++discarded_count;last_source_frame=source;pending_input=Json{};
            complete_GPU_observed=true;BeginStop("unsupported");
            std::printf("P0_LATE_DISCARDED session=%s frame=%u source_game_frame=%llu own_GPU_complete=1 Helper_COMPLETE_GPU=1 no_sample_no_merge=1 accepted_for_merge=0 finalized=%u\n",
                session.c_str(),f,(unsigned long long)source,processed_frames);
        }
        if(control_request) {
            const auto path=run_dir+"/control-ack-"+session+"-g0.json";
            if(!HasFile(path)) {
                if(now>=managed_control_deadline) throw std::runtime_error("P0 control ACK60s timeout; no normal release");
                return;
            }
            const Json ack=StrictJson(Read(path),path);
            const u64 epoch=RequireUnsigned(ack,"control_epoch");
            if(epoch<RequireUnsigned(*control_request,"control_epoch")) return;
            const auto action=RequireString(*control_request,"action");
            const auto expected=action=="idle"?"IDLE":action=="resume"?"READY":action=="drain"?"DRAIN_READY":"STOP_READY";
            if(RequireString(ack,"kind")!="control_ack"||RequireString(ack,"state")!=expected)
                throw std::runtime_error("P0 wrong control ACK state");
            for(const auto&[key,value]:control_request->items())
                if(key!="kind"&&key!="state"&&(!ack.contains(key)||ack.at(key).type()!=value.type()||ack.at(key)!=value))
                    throw std::runtime_error("P0 control exact ACK mismatch: "+key);
            if(RequireUnsigned(ack,"finalized_count")!=processed_frames||RequireUnsigned(ack,"accepted_count")!=accepted_count||
               RequireUnsigned(ack,"discarded_count")!=discarded_count)
                throw std::runtime_error("P0 Helper finalized/accepted/discard counts mismatch");
            control_epoch=epoch;
            if(action=="idle") idle=true;
            else if(action=="resume") {idle=false;++history_version;resume_reset_pending=true;}
            else {retire_requested=true;idle=false;}
            control_request.reset();
            std::printf("P0_CONTROL_ACK session=%s epoch=%llu action=%s finalized=%u history_version=%llu\n",
                session.c_str(),(unsigned long long)epoch,action.c_str(),processed_frames,(unsigned long long)history_version);
        }
        if(stop_pending&&!retire_requested&&!control_request&&own_complete&&pending_input.empty())
            RequestControl("stop",stop_reason);
        if(!retire_requested) return;
        if(!release_published) {
            if(!own_complete||!pending_input.empty()) return;
            const auto device=instance.GetDevice();
            local_return_view.reset();local_return=VideoCore::UniqueImage{};returned_view=nullptr;
            for(auto&image:resources) {
                if(image.view) device.destroyImageView(image.view);
                if(image.image) device.destroyImage(image.image);
                if(image.memory) device.freeMemory(image.memory);
            }
            resources.clear();
            Json release=Envelope(session,mode,execution_mode,"release","NATIVE_RELEASED",processed_frames,last_source_frame);
            release["source_frame_valid"]=processed_frames!=0;release["finalized_count"]=processed_frames;
            release["discarded_count"]=discarded_count;release["accepted_count"]=accepted_count;
            AtomicWrite(run_dir+"/native-released-"+session+"-g0.json",release.dump());
            release_published=true;managed_control_deadline=now+std::chrono::seconds(60);
            std::printf("P0_NATIVE_RELEASED session=%s finalized=%u actual_native_last_use_complete=1 managed_owner=1\n",session.c_str(),processed_frames);
        }
        const auto path=run_dir+"/retired-"+session+"-g0.json";
        if(!HasFile(path)) {
            if(now>=managed_control_deadline) throw std::runtime_error("P0 retire60s timeout");
            return;
        }
        const Json retired=StrictJson(Read(path),path);
        ValidateEnvelope(retired,session,mode,execution_mode,"retired","RETIRED",processed_frames,last_source_frame);
        if(RequireUnsigned(retired,"finalized_count")!=processed_frames||RequireUnsigned(retired,"discarded_count")!=discarded_count||
           RequireUnsigned(retired,"accepted_count")!=accepted_count)
            throw std::runtime_error("P0 terminal actual counts mismatch");
        const auto native=SnapshotFds(getpid(),run_dir+"/native-fd-final.json");
        const auto wine=SnapshotFds(exporter_pid,run_dir+"/wine-fd-final.json");
        for(const auto&original:exporter_raw_fds)
            if(std::any_of(native.begin(),native.end(),[&](const auto&v){return SamePayload(original,v);})||
               std::any_of(wine.begin(),wine.end(),[&](const auto&v){return SamePayload(original,v);}))
                throw std::runtime_error("P0 original raw payload remains at retirement");
        Json added=Json::array(),removed=Json::array();
        for(const auto&v:wine) if(std::none_of(exporter_baseline_fds.begin(),exporter_baseline_fds.end(),[&](const auto&x){return SameInventoryEntry(x,v);})) added.push_back(FdInventoryJson({v}).front());
        for(const auto&v:exporter_baseline_fds) if(std::none_of(wine.begin(),wine.end(),[&](const auto&x){return SameInventoryEntry(x,v);})) removed.push_back(FdInventoryJson({v}).front());
        AtomicWrite(run_dir+"/wine-fd-baseline-final-diff.json",Json{{"baseline_count",exporter_baseline_fds.size()},{"final_count",wine.size()},{"added",added},{"removed",removed}}.dump());
        Json audit=Envelope(session,mode,execution_mode,"audit","FD_AUDITED",processed_frames,last_source_frame);
        audit["source_frame_valid"]=processed_frames!=0;audit["finalized_count"]=processed_frames;
        audit["accepted_count"]=accepted_count;audit["discarded_count"]=discarded_count;
        audit["native_fd_count"]=native.size();audit["wine_fd_count"]=wine.size();
        AtomicWrite(run_dir+"/native-audited-"+session+"-g0.json",audit.dump());
        retirement_audited=true;finished=true;
        std::printf("P0_RETIREMENT_AUDIT session=%s actual_finalized=%u raw_payload_remaining=0 managed_owner=1 peer_reap_pending=1\n",session.c_str(),processed_frames);
    } catch(const std::exception&e) {soft_expired=false;Quarantine(e.what());}
}

} // namespace Vulkan
