// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <cstring>
#include <string>
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>
#include <functional>

#include "bbport_copy.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "bbport_toggles.h"
#include "bbport_wait_trace.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "bbport_threads.h"

namespace Vulkan {

std::mutex Scheduler::submit_mutex;
Scheduler::Q0PhaseRecord Scheduler::q0_submit_owner;

namespace {
// bbport: ProducerScope state of this thread: the producer function it is in (any scheduler).
thread_local const char* producer_scope_where = nullptr;
std::atomic<u32> producer_reports{0};

/// Names a thread of this process (its comm).
std::string ThreadName(u32 tid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%u/comm", tid);
    std::string name = "?";
    if (FILE* file = std::fopen(path, "r")) {
        char buffer[32]{};
        if (std::fgets(buffer, sizeof(buffer), file)) {
            name = buffer;
            if (!name.empty() && name.back() == '\n') {
                name.pop_back();
            }
        }
        std::fclose(file);
    }
    return name;
}

// Q0-only observation. The monitor never reads scheduler queues or producer-owned mutable
// fields: only atomics and the immutable target copied by the waiting thread. A scope stays
// registered under this separate mutex until its wait has ended, keeping every pointer alive.
struct Q0SlowWaitRecord {
    const char* function{};
    unsigned kind{};
    const void* scheduler{}; // identity only; never dereferenced
    void* caller{};
    u32 tid{};
    u64 target{};
    u64 entry_done{};
    const std::atomic<u64>* done{};
    const std::atomic<u64>* signal_issued{};
    const std::atomic<u64>* signal_done{};
    const std::atomic<u64>* host_done{};
    const std::atomic<size_t>* ordered_queued{};
    const std::atomic<u32>* active_worker{};
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::atomic<const char*> phase{"entry"};
    bool reported = false; // protected by Q0SlowWaitMonitor::mutex, not the scheduler mutex
};

class Q0SlowWaitMonitor {
public:
    Q0SlowWaitMonitor() : thread([this](std::stop_token stop) {
        std::unique_lock lock{mutex};
        while (!stop.stop_requested()) {
            cv.wait_for(lock, std::chrono::milliseconds(10));
            for (const auto& record : records) {
                if (!record->reported && !claimed[record->kind] &&
                    std::chrono::steady_clock::now() - record->start > std::chrono::milliseconds(100)) {
                    Report(*record, true);
                }
            }
        }
    }) {}
    ~Q0SlowWaitMonitor() {
        thread.request_stop();
        cv.notify_all();
        thread.join();
    }
    void Add(const std::shared_ptr<Q0SlowWaitRecord>& record) {
        std::scoped_lock lock{mutex};
        records.push_back(record);
    }
    void Remove(const std::shared_ptr<Q0SlowWaitRecord>& owned) {
        auto& record = *owned;
        std::scoped_lock lock{mutex};
        // Preserve the distinction if the monitor itself was not scheduled before return.
        if (!record.reported && !claimed[record.kind] &&
            std::chrono::steady_clock::now() - record.start > std::chrono::milliseconds(100)) {
            Report(record, false);
        }
        if (record.reported) Print(record, "END", false);
        std::erase(records, owned);
    }
private:
    static void Print(const Q0SlowWaitRecord& r, const char* event, bool active) {
        const auto now = std::chrono::steady_clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        const double wall_ms = std::chrono::duration<double, std::milli>(now - r.start).count();
        Dl_info symbol{};
        dladdr(r.caller, &symbol);
        std::fprintf(stderr,
            "Q0_SLOW_WAIT event=%s function=%s clock=steady_clock monotonic_ns=%lld "
            "wall_ms=%.6f threshold_ms=100 observed_wait_active=%u tid=%u thread=%s "
            "scheduler=%p direct_caller=%p caller_object=%s caller_offset=0x%llx phase=%s "
            "target_issued_entry=%llu target_done_entry=%llu target_done_now=%llu "
            "deferred_issued_now=%llu deferred_done_now=%llu host_done_now=%llu "
            "ordered_queued_now=%zu active_worker_now=%u copy_pool_queue=NOT_AVAILABLE "
            "snapshot_atomic_fields_independent=1 root_cause=UNPROVEN\n",
            event, r.function, (long long)ns, wall_ms, unsigned(active), r.tid,
            ThreadName(r.tid).c_str(), r.scheduler, r.caller,
            symbol.dli_fname ? symbol.dli_fname : "?",
            (unsigned long long)(reinterpret_cast<uintptr_t>(r.caller) -
                                 reinterpret_cast<uintptr_t>(symbol.dli_fbase)),
            r.phase.load(std::memory_order_relaxed), (unsigned long long)r.target,
            (unsigned long long)r.entry_done,
            (unsigned long long)r.done->load(std::memory_order_acquire),
            (unsigned long long)r.signal_issued->load(std::memory_order_relaxed),
            (unsigned long long)r.signal_done->load(std::memory_order_acquire),
            (unsigned long long)r.host_done->load(std::memory_order_acquire),
            r.ordered_queued->load(std::memory_order_acquire),
            r.active_worker->load(std::memory_order_relaxed));
        std::fflush(stderr); // visible while the waiting thread may still be inside WaitAsync
    }
    void Report(Q0SlowWaitRecord& record, bool active) {
        claimed[record.kind] = true;
        record.reported = true;
        Print(record, "START", active);
    }
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::shared_ptr<Q0SlowWaitRecord>> records;
    bool claimed[2]{}; // first slow wait per function, across all schedulers in this process
    std::jthread thread;
};

class Q0SlowWaitScope {
public:
    Q0SlowWaitScope(const char* function, unsigned kind, const void* scheduler, void* caller,
                    u64 target, const std::atomic<u64>& done,
                    const std::atomic<u64>& signal_issued, const std::atomic<u64>& signal_done,
                    const std::atomic<u64>& host_done, const std::atomic<size_t>& ordered_queued,
                    const std::atomic<u32>& active_worker) noexcept
    {
        static const bool enabled = [] {
            const char* value = std::getenv("BB_Q0_SLOW_WAIT_DIAG");
            return value && std::strcmp(value, "1") == 0;
        }();
        if (!enabled) return;
        try {
            record = std::make_shared<Q0SlowWaitRecord>();
            record->function = function;
            record->kind = kind;
            record->scheduler = scheduler;
            record->caller = caller; // waiting-thread entry address, not the watcher's stack
            record->tid = u32(gettid());
            record->target = target;
            record->entry_done = done.load(std::memory_order_acquire);
            record->done = &done;
            record->signal_issued = &signal_issued;
            record->signal_done = &signal_done;
            record->host_done = &host_done;
            record->ordered_queued = &ordered_queued;
            record->active_worker = &active_worker;
            static Q0SlowWaitMonitor monitor;
            owner = &monitor;
            owner->Add(record);
        } catch (...) {
            owner = nullptr;
            static std::atomic_flag registration_failure_reported = ATOMIC_FLAG_INIT;
            if (!registration_failure_reported.test_and_set(std::memory_order_relaxed)) {
                std::fprintf(stderr, "Q0_SLOW_WAIT diagnostic_registration_failed=1 wait_unchanged=1\n");
            }
        }
    }
    ~Q0SlowWaitScope() { if (owner) owner->Remove(record); }
    void Phase(const char* phase) { if (owner) record->phase.store(phase, std::memory_order_relaxed); }
private:
    std::shared_ptr<Q0SlowWaitRecord> record;
    Q0SlowWaitMonitor* owner = nullptr;
};

void ReportProducer(const char* what, u32 other, const char* other_where, const char* where) {
    if (producer_reports.fetch_add(1, std::memory_order_relaxed) >= 8) {
        return;
    }
    const u32 self = u32(gettid());
    std::fprintf(stderr,
                 "Scheduler: %s: thread %u (%s) entering %s while thread %u (%s) is in %s\n",
                 what, self, ThreadName(self).c_str(), where, other, ThreadName(other).c_str(),
                 other_where ? other_where : "?");
    void* frames[24];
    const int depth = backtrace(frames, 24);
    backtrace_symbols_fd(frames, depth, 2);
}
} // namespace

void Scheduler::ProducerScope::Enter(const char* where) noexcept {
    previous_where = producer_scope_where;
    static thread_local const u32 tid = u32(gettid());
    producer_scope_where = where;
    u32 expected = 0;
    if (scheduler.producer_tid.compare_exchange_strong(expected, tid,
                                                       std::memory_order_acq_rel)) {
        outer = true;
        scheduler.producer_where.store(where, std::memory_order_relaxed);
        return;
    }
    if (expected != tid) {
        ReportProducer("CONCURRENT recording", expected,
                       scheduler.producer_where.load(std::memory_order_relaxed), where);
    }
}

void Scheduler::ProducerScope::Leave() noexcept {
    producer_scope_where = previous_where;
    if (outer) {
        scheduler.producer_tid.store(0, std::memory_order_release);
    }
}

namespace {

/// bbport: Vulkan recording threads: BB_VK_RECORD_THREADS, else 2 from 8 hardware threads and 3
/// from 12. One thread records ~75% of a core at 200 FPS (4 cores / 8 threads); the segments
/// let the stream use more than one.
u32 RecordingThreadCount() {
    if (const char* env = std::getenv("BB_VK_RECORD_THREADS")) {
        return std::clamp(std::atoi(env), 1, 8);
    }
    const unsigned available = BbThreads::Available();
    return available >= 12 ? 3 : available >= 8 ? 2 : 1;
}

vk::CommandBuffer BeginCommandBuffer(CommandPool& pool) {
    const vk::CommandBuffer cmdbuf = pool.Commit();
    Check(cmdbuf.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
    return cmdbuf;
}

} // namespace

Scheduler::Scheduler(const Instance& instance, bool threaded_recording)
    : instance{instance}, work_semaphore{instance}, command_pool{instance, &work_semaphore} {
    const char* q0_env = std::getenv("BB_Q0_WAIT_STATE_DIAG");
    q0_wait_state_enabled = q0_env && std::strcmp(q0_env, "1") == 0;
    if (q0_wait_state_enabled) {
        try {
            q0_actor_phase = std::make_shared<Q0PhaseRecord>();
            q0_actor_phase->identity.store(reinterpret_cast<uintptr_t>(this));
        } catch (...) {
            static std::atomic_flag failed = ATOMIC_FLAG_INIT;
            if (!failed.test_and_set())
                std::fprintf(stderr, "Q0_WAIT_STATE actor_registration_failed=1 phase=UNAVAILABLE wait_unchanged=1\n");
        }
    }
    crumb_stream = Breadcrumbs::NewStream(
        instance, threaded_recording ? "draws: game and presenter" : "presentation");
    // bbport: BB_VK_RECORD_THREAD=0 records on the calling thread.
    const char* env = std::getenv("BB_VK_RECORD_THREAD");
    if (threaded_recording && !(env && env[0] == '0')) {
        record_chunk = AcquireChunk();
        ordered_chunk = AcquireChunk();
        // A segment of ~64 KiB of closures is ~60 draws: a frame of ~700 draws gives each
        // thread several segments, and the render passes a cut reopens stay few.
        const char* segment_kb = std::getenv("BB_VK_SEGMENT_KB");
        split_bytes = size_t(segment_kb ? std::max(1, std::atoi(segment_kb)) : 64) * 1024;
        const u32 count = RecordingThreadCount();
        for (u32 i = 0; i < count; ++i) {
            auto worker = std::make_unique<Worker>();
            if (i == 0) {
                worker->pool = &command_pool;
            } else {
                worker->own_pool = std::make_unique<CommandPool>(instance, &work_semaphore);
                worker->pool = worker->own_pool.get();
            }
            workers.push_back(std::move(worker));
        }
        for (u32 i = 0; i < count; ++i) {
            workers[i]->thread =
                std::jthread(std::bind_front(&Scheduler::RecorderThread, this), i);
        }
        std::printf("GPU: %u Vulkan recording thread%s, segments of %zu KiB\n", count,
                    count == 1 ? "" : "s", split_bytes / 1024);
    }
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    // bbport BB_ASYNC_SUBMIT (default on; 0: off): submissions go out from the recording threads
    // once their segments are recorded, instead of this thread waiting for them (~65 us each).
    if (!workers.empty()) {
        const char* async = std::getenv("BB_ASYNC_SUBMIT");
        async_submit = !async || async[0] != '0';
        if (async_submit) {
            std::printf("GPU: submissions go out from the recording threads (BB_ASYNC_SUBMIT)\n");
        }
    }
    AllocateWorkerCommandBuffers();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    if (!workers.empty()) {
        SyncRecording();
        for (auto& worker : workers) {
            worker->thread.request_stop(); // wakes its wait on worker->cv
        }
        for (auto& worker : workers) {
            worker->thread.join();
        }
    }
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    // bbport: a cut of the command stream (MaybeSplit) closed this render pass; it continues
    // in the next command buffer without clearing its attachments again.
    const bool resume = resume_rendering && !is_rendering && render_state == new_state;
    resume_rendering = false;
    EndRendering();
    CarrySuspend(false);
    is_rendering = true;
    render_state = new_state;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear && !resume ? vk::AttachmentLoadOp::eClear
                                             : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear && !resume ? vk::AttachmentLoadOp::eClear
                                            : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear && !resume ? vk::AttachmentLoadOp::eClear
                                              : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    if (workers.empty()) {
        current_cmdbuf.beginRendering(rendering_info);
        CarryResume(true);
        return;
    }
    // The attachment infos live on this stack frame: the recorded closure keeps copies.
    Record([info = rendering_info, color_attachments, depth_attachment,
            stencil_attachment](vk::CommandBuffer cmdbuf) mutable {
        info.pColorAttachments = color_attachments.data();
        if (info.pDepthAttachment) {
            info.pDepthAttachment = &depth_attachment;
        }
        if (info.pStencilAttachment) {
            info.pStencilAttachment = &stencil_attachment;
        }
        cmdbuf.beginRendering(info);
    });
    CarryResume(true);
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    CarrySuspend(true);
    is_rendering = false;
    Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRendering(); });
    CarryResume(false);
}

void Scheduler::TraceDirectRecording(void* caller) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_RECORDER_TRACE");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static std::mutex mutex;
    static std::unordered_map<void*, u64> callers;
    static u64 calls;
    std::scoped_lock lk{mutex};
    ++callers[caller];
    if (++calls % 2000) {
        return;
    }
    std::vector<std::pair<u64, void*>> top;
    for (const auto& [address, count] : callers) {
        top.emplace_back(count, address);
    }
    std::ranges::sort(top, std::greater{});
    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
        Dl_info info{};
        dladdr(top[i].second, &info);
        std::printf("Recorder sync caller: %llu x %s+0x%lx\n",
                    static_cast<unsigned long long>(top[i].first),
                    info.dli_fname ? info.dli_fname : "?",
                    static_cast<unsigned long>(reinterpret_cast<uintptr_t>(top[i].second) -
                                               reinterpret_cast<uintptr_t>(info.dli_fbase)));
    }
    callers.clear();
}

std::unique_ptr<RecordChunk> Scheduler::AcquireChunk() {
    std::scoped_lock lk{recorder_mutex};
    if (free_chunks.empty()) {
        return std::make_unique<RecordChunk>();
    }
    auto chunk = std::move(free_chunks.back());
    free_chunks.pop_back();
    return chunk;
}

void Scheduler::SignalAfterHostCopies(std::function<void()> signal) {
    // A direct segment queues it too: signals queued before it must not be overtaken.
    if (!IsRecordingDeferred() && !direct_segment) {
        WaitHostCopies();
        signal();
        return;
    }
    BbCopy::FlushBatch();
    deferred_signals_issued.fetch_add(1, std::memory_order_relaxed);
    RecordOrdered([signal = std::move(signal), done = deferred_signals_done,
                   diagnostic = q0_actor_phase]() mutable {
        // Observe only this synchronous invocation. The asynchronous signal callback below
        // retains its original capture list: no Scheduler or diagnostic lifetime dependency.
        PublishQ0Phase(diagnostic.get(), 2, 2, true);
        BbCopy::AfterCopies([signal = std::move(signal), done = std::move(done)] {
            signal();
            done->fetch_add(1, std::memory_order_release);
        });
        PublishQ0Phase(diagnostic.get(), 2, 11, false, true);
    });
    KickRecording(true);
}

// Ordered task phase and global submit-owner are independent of the locked queue shadow.
// Each record has one writer (serialized ordered tasks, or the actual global mutex owner).
void Scheduler::PublishQ0Phase(Q0PhaseRecord* record, u64 kind, u64 stage,
                               bool entry, bool exit, u64 identity) {
    if (!record) return;
    record->seq.fetch_add(1);
    const u64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    record->kind.store(kind); record->stage.store(stage);
    record->tid.store(kind ? u32(gettid()) : 0);
    if (identity || !kind) record->identity.store(identity);
    if (entry) {
        record->entries.fetch_add(1); record->entry_ns.store(now); record->exit_ns.store(0);
        record->claim_epoch.store(record->selected_epoch.load());
    }
    if (exit) { record->exits.fetch_add(1); record->exit_ns.store(now); }
    record->updated_ns.store(now);
    record->seq.fetch_add(1);
}

Scheduler::Q0SubmitOwnerScope::Q0SubmitOwnerScope(const Scheduler* identity, u64 actor_kind)
    : kind(actor_kind) {
    static const bool requested = [] {
        const char* value = std::getenv("BB_Q0_WAIT_STATE_DIAG");
        return value && std::strcmp(value, "1") == 0;
    }();
    enabled = requested;
    if (enabled) PublishQ0Phase(&q0_submit_owner, kind, 0, true, false,
                                reinterpret_cast<uintptr_t>(identity));
}
Scheduler::Q0SubmitOwnerScope::~Q0SubmitOwnerScope() {
    if (enabled) PublishQ0Phase(&q0_submit_owner, 0, 0, false, true);
}
void Scheduler::Q0SubmitOwnerScope::Stage(u64 stage) {
    if (enabled) PublishQ0Phase(&q0_submit_owner, kind, stage);
}

// Reason is an observation of the original OrderedReady predicate, never its replacement.
// One sequentially-consistent sequence covers the locked groups. Atomic payload avoids data
// races even when the bounded reader retries a concurrent publication.
void Scheduler::PublishQ0WaitState() {
    if (!q0_wait_state_enabled) return;
    auto& q = q0_wait_state;
    q.seq.fetch_add(1);
    q.queue.store(ordered_queue.size());
    q.running.store(ordered_running);
    u64 reason = 0, segment = 0, after = 0, blocked = 0, recorded = 0, handed = 0;
    if (!ordered_queue.empty()) {
        const auto& head = *ordered_queue.front();
        segment = head.segment;
        after = head.after;
        reason = ordered_running ? 1 : 4;
        if (!ordered_running) {
            for (u32 i = 0; i < head.segment; ++i) {
                if (head.batch->recorded[i] != head.batch->handed[i]) {
                    reason = 2; blocked = i;
                    recorded = head.batch->recorded[i]; handed = head.batch->handed[i];
                    break;
                }
            }
            if (reason == 4) {
                blocked = head.segment;
                recorded = head.batch->recorded[head.segment];
                handed = head.batch->handed[head.segment];
                if (recorded < head.after) reason = 3;
            }
        }
    }
    q.reason.store(reason); q.segment.store(segment); q.after.store(after);
    q.blocked_segment.store(blocked); q.recorded.store(recorded); q.handed.store(handed);
    q.worker_count.store(workers.size());
    u64 asleep = 0, busy = 0;
    for (size_t i = 0; i < workers.size(); ++i) {
        const auto& w = *workers[i];
        q.worker_queue[i].store(w.queue.size());
        q.wait_returns[i].store(w.q0_wait_returns);
        q.command_begin[i].store(w.q0_command_begin); q.command_end[i].store(w.q0_command_end);
        q.ordered_begin[i].store(w.q0_ordered_begin); q.ordered_end[i].store(w.q0_ordered_end);
        if (workers[i]->sleeping) asleep |= 1ull << i;
        if (workers[i]->busy) busy |= 1ull << i;
    }
    q.sleep_mask.store(asleep); q.busy_mask.store(busy);
    q.enqueued.store(q0_enqueued_chunks); q.popped.store(q0_popped_chunks);
    q.claim_epoch.store(q0_ordered_claim_epoch);
    q.updated_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    q.seq.fetch_add(1);
}

class Scheduler::Q0WaitStateScope {
public:
    Q0WaitStateScope(Scheduler& scheduler, const char* function, u64 target, void* caller)
        : s(scheduler), function(function), target(target), caller(caller),
          enabled(scheduler.q0_wait_state_enabled && reports.load() < 3) {
        if (enabled) start = std::chrono::steady_clock::now();
    }
    ~Q0WaitStateScope() {
        if (!enabled) return;
        Check("wait_return", true);
        if (reported) Print("END", "wait_return", false);
    }
    void Check(const char* phase, bool force = false) {
        if (!enabled || reported) return;
        // Read the clock once per 256 outer polls. WaitAsync may itself block: no in-flight
        // claim can be made during that interval; the final forced check covers its return.
        if (!force && (++polls & 255)) return;
        if (std::chrono::steady_clock::now() - start <= std::chrono::milliseconds(100)) return;
        const auto slot = reports.fetch_add(1);
        if (slot >= 3) { enabled = false; return; }
        reported = true;
        Print("START", phase, std::strcmp(phase, "wait_return") != 0);
    }
private:
    static void ReadPhase(const Q0PhaseRecord* record, char* out, size_t capacity) {
        if (!record) { std::snprintf(out, capacity, "UNAVAILABLE"); return; }
        u64 seq = 0, kind = 0, stage = 0, tid = 0, identity = 0, entered = 0, updated = 0,
            exited = 0, entries = 0, exits = 0, epoch = 0;
        bool coherent = false;
        unsigned attempts = 0;
        for (; attempts < 3; ++attempts) {
            const u64 before = record->seq.load();
            if (before & 1) continue;
            seq = before; kind = record->kind.load(); stage = record->stage.load();
            tid = record->tid.load(); identity = record->identity.load();
            entered = record->entry_ns.load(); updated = record->updated_ns.load();
            exited = record->exit_ns.load(); entries = record->entries.load(); exits = record->exits.load(); epoch = record->claim_epoch.load();
            coherent = before == record->seq.load();
            if (coherent) { ++attempts; break; }
        }
        const u64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const char* kinds[] = {"NONE", "HOST_COPY", "AFTER_COPIES_SYNC_CALL", "ASYNC_SUBMIT",
                               "UNUSED", "SYNC_SUBMIT", "PRESENTER"};
        const char* stages[] = {"NONE", "HOST_COPY_CALL", "AFTER_COPIES_CALL", "WAIT_ASYNC",
            "BUILD_SUBMIT_INFO", "WAIT_SUBMIT_MUTEX", "BEFORE_SUBMIT", "QUEUE_SUBMIT",
            "POST_SUBMIT", "WAIT_FINAL_RECORDER_MUTEX", "RETIRE_BATCH", "EXIT",
            "SYNC_RECORDING", "SYNC_COPIES", "SYNC_BEFORE_SUBMIT", "PRESENT_CALL",
            "PRESENT_POST_CALL", "PRESENT_RECREATE"};
        std::snprintf(out, capacity,
            "%s:seq%llu:tries%u:kind%s:stage%s:tid%llu:scheduler0x%llx:entry%llu:exit%llu:updated%llu:sample%llu:age_ms%.6f:entries%llu:exits%llu:claim_epoch%llu",
            coherent && seq ? "COHERENT" : coherent ? "UNPUBLISHED" : "MIXED",
            (unsigned long long)seq, attempts,
            coherent && kind < std::size(kinds) ? kinds[kind] : "UNKNOWN",
            coherent && stage < std::size(stages) ? stages[stage] : "UNKNOWN",
            (unsigned long long)tid, (unsigned long long)identity,
            (unsigned long long)entered, (unsigned long long)exited, (unsigned long long)updated, (unsigned long long)now,
            updated && now >= updated ? double(now - updated) / 1e6 : -1.0,
            (unsigned long long)entries, (unsigned long long)exits, (unsigned long long)epoch);
    }
    void Print(const char* event, const char* phase, bool active) {
        struct Snapshot {
            u64 seq{}, updated{}, queue{}, running{}, reason{}, segment{}, after{}, blocked{}, recorded{},
                handed{}, workers{}, asleep{}, busy{}, enqueued{}, popped{}, claim_epoch{};
            std::array<u64, 8> queued{}, waits{}, cb{}, ce{}, ob{}, oe{};
        } v;
        auto& q = s.q0_wait_state;
        bool coherent = false;
        unsigned attempts = 0;
        for (; attempts < 3; ++attempts) {
            const u64 before = q.seq.load();
            if (before & 1) continue;
            v.seq = before; v.updated = q.updated_ns.load(); v.queue = q.queue.load();
            v.running = q.running.load();
            v.reason = q.reason.load(); v.segment = q.segment.load(); v.after = q.after.load();
            v.blocked = q.blocked_segment.load(); v.recorded = q.recorded.load();
            v.handed = q.handed.load(); v.workers = q.worker_count.load();
            v.asleep = q.sleep_mask.load(); v.busy = q.busy_mask.load();
            v.enqueued = q.enqueued.load(); v.popped = q.popped.load(); v.claim_epoch = q.claim_epoch.load();
            for (size_t i = 0; i < 8; ++i) {
                v.queued[i] = q.worker_queue[i].load(); v.waits[i] = q.wait_returns[i].load();
                v.cb[i] = q.command_begin[i].load(); v.ce[i] = q.command_end[i].load();
                v.ob[i] = q.ordered_begin[i].load(); v.oe[i] = q.ordered_end[i].load();
            }
            coherent = before == q.seq.load();
            if (coherent) { ++attempts; break; }
        }
        const auto now = std::chrono::steady_clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        const char* reasons[] = {"EMPTY", "RUNNING", "PRIOR_SEGMENT", "CURRENT_AFTER", "READY"};
        char workers[2048]{};
        size_t offset = 0;
        for (size_t i = 0; i < std::min<u64>(v.workers, 8); ++i) {
            const int n = std::snprintf(workers + offset, sizeof(workers) - offset,
                "%s%zu:q%llu/wr%llu/n%llu/cb%llu/ce%llu/ob%llu/oe%llu",
                i ? "," : "", i, (unsigned long long)v.queued[i], (unsigned long long)v.waits[i],
                (unsigned long long)q.notify_calls[i].load(), (unsigned long long)v.cb[i],
                (unsigned long long)v.ce[i], (unsigned long long)v.ob[i], (unsigned long long)v.oe[i]);
            if (n < 0 || size_t(n) >= sizeof(workers) - offset) break;
            offset += size_t(n);
        }
        // Each independent phase read has its own sample clock and coherence sequence.
        // Only a matching scheduler and claim_epoch associates an actor with the batch.
        char actor_phase[640]{}, owner_phase[640]{};
        ReadPhase(s.q0_actor_phase.get(), actor_phase, sizeof(actor_phase));
        ReadPhase(&q0_submit_owner, owner_phase, sizeof(owner_phase));
        std::fprintf(stderr,
            "Q0_WAIT_STATE event=%s function=%s tid=%u observer=waiting_thread clock=steady_clock "
            "monotonic_ns=%lld wall_ms=%.6f threshold_ms=100 observed_wait_active=%u phase=%s "
            "wait_async_inflight_observation=UNKNOWN direct_caller=%p scheduler=%p target_entry=%llu "
            "deferred_issued_now=%llu deferred_done_now=%llu host_done_now=%llu "
            "shadow_coherence=%s shadow_seq=%llu snapshot_attempts=%u shadow_updated_ns=%llu "
            "shadow_age_ms=%.6f head_reason=%s ordered_queue=%llu ordered_running=%llu "
            "blocked_dependency_valid=%u front_segment=%llu "
            "front_after=%llu blocked_segment=%llu blocked_recorded=%llu blocked_handed=%llu "
            "sleep_mask=0x%llx busy_mask=0x%llx enqueued_chunks=%llu popped_chunks=%llu ordered_claim_epoch=%llu "
            "worker_columns=index:queue/wait_returns/notify_calls/command_begin/end/ordered_batch_selected/completed "
            "worker_execution_unit=chunks ordered_counts_scope=selected_before_loop/completed_after_loop "
            "per_ordered_chunk_execute_entry=NOT_OBSERVED notify_counts_independent=1 signal_counts_independent=1 "
            "workers=%s actor_phase=%s submit_owner=%s phase_groups_independent=1 "
            "async_signal_callback_phase=NOT_OBSERVED root_cause=UNPROVEN\n",
            event, function, u32(gettid()), (long long)ns,
            std::chrono::duration<double, std::milli>(now - start).count(), unsigned(active), phase,
            caller, static_cast<void*>(&s), (unsigned long long)target,
            (unsigned long long)s.deferred_signals_issued.load(),
            (unsigned long long)s.deferred_signals_done->load(), (unsigned long long)s.host_copies_done.load(),
            coherent && v.seq ? "COHERENT" : coherent ? "UNPUBLISHED" : "MIXED", (unsigned long long)v.seq,
            attempts, (unsigned long long)v.updated, v.updated ? double(ns - v.updated) / 1e6 : -1.0,
            coherent && v.seq && v.reason < 5 ? reasons[v.reason] : "UNKNOWN",
            (unsigned long long)v.queue, (unsigned long long)v.running,
            unsigned(coherent && (v.reason == 2 || v.reason == 3)),
            (unsigned long long)v.segment, (unsigned long long)v.after,
            (unsigned long long)v.blocked, (unsigned long long)v.recorded, (unsigned long long)v.handed,
            (unsigned long long)v.asleep, (unsigned long long)v.busy, (unsigned long long)v.enqueued,
            (unsigned long long)v.popped, (unsigned long long)v.claim_epoch, workers, actor_phase, owner_phase);
        std::fflush(stderr);
    }
    inline static std::atomic<u32> reports{0}; // first three slow waits, process-wide
    Scheduler& s;
    const char* function;
    u64 target;
    void* caller;
    bool enabled{}, reported{};
    u32 polls{};
    std::chrono::steady_clock::time_point start{};
};

void Scheduler::WaitDeferredSignals() {
    const u64 issued = deferred_signals_issued.load(std::memory_order_relaxed);
    if (deferred_signals_done->load(std::memory_order_acquire) >= issued ||
        BbToggle::Disabled(BbToggle::OrderedGuestWrites)) {
        return;
    }
    Q0SlowWaitScope trace{"WaitDeferredSignals", 0, this, __builtin_return_address(0), issued,
                          *deferred_signals_done, deferred_signals_issued, *deferred_signals_done,
                          host_copies_done, ordered_queued, active_worker};
    Q0WaitStateScope state{*this, "WaitDeferredSignals", issued, __builtin_return_address(0)};
    BbStats::WaitTimer timer{BbStats::host_copies_wait_ns};
    trace.Phase("kick_recording");
    KickRecording(true);
    while (deferred_signals_done->load(std::memory_order_acquire) < issued) {
        // Helps the copy threads the signals wait for.
        state.Check("before_wait_async");
        trace.Phase("copy_pool_wait");
        BbCopy::WaitAsync();
        trace.Phase("signal_poll");
        state.Check("after_wait_async");
        std::this_thread::yield();
    }
}

namespace {
constexpr u32 HostCopyGranuleShift = 16;
constexpr u64 HostCopyMaxGranules = 64; // larger notes and checks are treated as everywhere
u32 HostCopySlotOf(u64 granule, u32 slots) {
    return u32((granule * 0x9e3779b97f4a7c15ull) >> 40) % slots;
}
} // namespace

void Scheduler::NoteHostCopySource(u64 address, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 first = address >> HostCopyGranuleShift;
    const u64 last = (address + size - 1) >> HostCopyGranuleShift;
    std::scoped_lock lk{host_copy_sources_mutex};
    const u64 seq = ++host_copy_source_seq;
    if (last - first >= HostCopyMaxGranules) {
        host_copy_big_seq = seq;
        return;
    }
    for (u64 g = first; g <= last; ++g) {
        auto& slot = host_copy_slots[HostCopySlotOf(g, HostCopySlots)];
        if (slot.seq > host_copy_sources_done && slot.granule != g) {
            slot.granule = HostCopyWildcard; // two pending granules share it
        } else if (slot.granule != HostCopyWildcard || slot.seq <= host_copy_sources_done) {
            slot.granule = g;
        }
        slot.seq = seq;
    }
}

void Scheduler::WaitHostCopiesFor(u64 address, u64 size) {
    {
        std::scoped_lock lk{host_copy_sources_mutex};
        bool overlap = host_copy_big_seq > host_copy_sources_done;
        const u64 first = address >> HostCopyGranuleShift;
        const u64 last = (address + std::max<u64>(size, 1) - 1) >> HostCopyGranuleShift;
        if (last - first >= HostCopyMaxGranules) {
            overlap = true;
        }
        for (u64 g = first; !overlap && g <= last; ++g) {
            const auto& slot = host_copy_slots[HostCopySlotOf(g, HostCopySlots)];
            overlap = slot.seq > host_copy_sources_done &&
                      (slot.granule == g || slot.granule == HostCopyWildcard);
        }
        if (!overlap) {
            BbStats::host_copy_waits_skipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    WaitHostCopies();
}

void Scheduler::WaitHostCopies() {
    // This immutable entry target is taken on the same producer that already reads the
    // non-atomic issued value below. The monitor never reads host_copies_issued directly.
    Q0SlowWaitScope trace{"WaitHostCopies", 1, this, __builtin_return_address(0), host_copies_issued,
                          host_copies_done, deferred_signals_issued, *deferred_signals_done,
                          host_copies_done, ordered_queued, active_worker};
    Q0WaitStateScope state{*this, "WaitHostCopies", host_copies_issued, __builtin_return_address(0)};
    trace.Phase("source_snapshot");
    u64 sources_seen;
    {
        std::scoped_lock lk{host_copy_sources_mutex};
        sources_seen = host_copy_source_seq;
    }
    // Copies noted after this point may still run when the waits below return.
    struct Forget {
        Scheduler& s;
        u64 seen;
        ~Forget() {
            std::scoped_lock lk{s.host_copy_sources_mutex};
            s.host_copy_sources_done = std::max(s.host_copy_sources_done, seen);
        }
    } forget{*this, sources_seen};
    if (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
        BbStats::WaitTimer timer{BbStats::host_copies_wait_ns};
        BbStats::host_copy_waits.fetch_add(1, std::memory_order_relaxed);
        trace.Phase("kick_recording");
        KickRecording(true);
        trace.Phase("recorded_copy_poll");
        while (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
            state.Check("recorded_copy_poll");
            std::this_thread::yield();
        }
    }
    BbStats::WaitTimer timer{BbStats::copy_threads_wait_ns};
    trace.Phase("copy_pool_wait");
    state.Check("before_wait_async", true);
    BbCopy::WaitAsync();
}

void Scheduler::KickRecording(bool force) {
    ProducerScope producer{*this, "KickRecording"};
    if (workers.empty()) {
        return;
    }
    // Callers kick where nobody holds the raw command buffer: deferral resumes.
    if (direct_segment) {
        LeaveDirectSegment();
    }
    direct_mode = false;
    if (!force) {
        MaybeSplit();
    }
    // Batches of tens of KiB keep the queue handoff cheap relative to the work it carries.
    if (!force && full_chunks.empty() && CurrentChunk().Size() < 32 * 1024) {
        return;
    }
    HandOver();
}

void Scheduler::MaybeSplit() {
    if (workers.size() < 2 || current_segment + 1 >= MaxSegments || !IsRecordingDeferred() ||
        BbToggle::Disabled(BbToggle::ParallelRecording)) {
        return;
    }
    // Cutting inside a render pass closes and reopens it: only after a longer stretch.
    const size_t bytes = segment_bytes + CurrentChunk().Size();
    if (bytes < (is_rendering ? 2 * split_bytes : split_bytes)) {
        return;
    }
    if (is_rendering) {
        CarrySuspend(true);
        Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRendering(); });
        is_rendering = false;
        resume_rendering = true;
    } else {
        CarrySuspend(false);
    }
    if (async_submit) {
        // Its recording thread ends the segment's command buffer (the pool is that thread's).
        Record([](vk::CommandBuffer cmdbuf) { Check(cmdbuf.end()); });
    }
    HandOver();
    ++current_segment;
    batch->segments[current_segment] = vk::CommandBuffer{};
    segment_bytes = 0;
    active_worker.store(current_segment % workers.size(), std::memory_order_relaxed);
    // The next command buffer starts without dynamic state: the next draw sets all of it.
    // Everything else (pipeline, vertex and index buffers, descriptors, push constants) every
    // draw and dispatch records itself.
    dynamic_state.Invalidate();
    CarryResume(false);
}

void Scheduler::HandOver() {
    // Entered from inside Record*() on this thread: a fault handler recording mid-command.
    if (const char* inside = producer_scope_where;
        inside && (std::strcmp(inside, "Record") == 0 || std::strcmp(inside, "RecordData") == 0 ||
                   std::strcmp(inside, "RecordOrdered") == 0 ||
                   std::strcmp(inside, "RetireChunk") == 0)) {
        ReportProducer("REENTERED recording", u32(gettid()), inside, "HandOver");
    }
    ProducerScope producer{*this, "HandOver"};
    if (!record_chunk || !ordered_chunk) {
        // Left behind by another HandOver interrupted between handing a chunk over and
        // taking a new one (see ProducerScope). Recover instead of dereferencing null.
        ReportProducer("HandOver without a current chunk", producer_tid.load(), "?", "HandOver");
        if (!record_chunk) {
            record_chunk = AcquireChunk();
        }
        if (!ordered_chunk) {
            ordered_chunk = AcquireChunk();
        }
    }
    const bool commands = !full_chunks.empty() || !record_chunk->Empty();
    const bool ordered = !ordered_full.empty() || !ordered_chunk->Empty();
    if (!commands && !ordered) {
        return;
    }
    // The chunks that take the current ones' places, taken first (AcquireChunk locks
    // recorder_mutex) and swapped in below: record_chunk and ordered_chunk are never null, not
    // even for a fault handler recording on this thread meanwhile (issue #100).
    std::unique_ptr<RecordChunk> next_record = record_chunk->Empty() ? nullptr : AcquireChunk();
    std::unique_ptr<RecordChunk> next_ordered = ordered_chunk->Empty() ? nullptr : AcquireChunk();
    const size_t q0_worker_index = current_segment % workers.size();
    Worker& worker = *workers[q0_worker_index];
    bool wake = false;
    {
        std::scoped_lock lk{recorder_mutex};
        if (commands) {
            for (auto& chunk : full_chunks) {
                chunk->segment = current_segment;
                chunk->batch = batch;
                worker.queue.push_back(std::move(chunk));
                ++batch->handed[current_segment];
            }
            if (next_record) {
                record_chunk.swap(next_record); // next_record: the chunk handed over
                segment_bytes += next_record->Size();
                next_record->segment = current_segment;
                next_record->batch = batch;
                worker.queue.push_back(std::move(next_record));
                ++batch->handed[current_segment];
            }
            worker.queued.store(worker.queue.size(), std::memory_order_release);
            wake = worker.sleeping;
        }
        if (ordered) {
            // They run after the commands handed over with them (recorded before or among them).
            const auto queue = [&](std::unique_ptr<RecordChunk> chunk) {
                chunk->segment = current_segment;
                chunk->batch = batch;
                chunk->after = batch->handed[current_segment];
                ordered_queue.push_back(std::move(chunk));
                if (q0_wait_state_enabled) ++q0_enqueued_chunks;
            };
            for (auto& chunk : ordered_full) {
                queue(std::move(chunk));
            }
            if (next_ordered) {
                ordered_chunk.swap(next_ordered); // next_ordered: the chunk handed over
                queue(std::move(next_ordered));
            }
            ordered_queued.store(ordered_queue.size(), std::memory_order_release);
            // Any thread may run them; the current segment's thread is woken for them (when it
            // sleeps): it spins for work anyway, and waking another thread per small copy cost
            // a few percent of the frame rate on 4 cores / 8 threads. A thread that is awake
            // picks them up by itself.
            if (!ordered_running) {
                wake |= worker.sleeping;
            }
        }
        PublishQ0WaitState();
    }
    full_chunks.clear();
    ordered_full.clear();
    // A busy thread picks the new chunks up by itself: waking it is a syscall per draw.
    if (wake) {
        worker.cv.notify_one();
        if (q0_wait_state_enabled) q0_wait_state.notify_calls[q0_worker_index].fetch_add(1);
    }
}

bool Scheduler::RecordingIdle() const {
    if (!ordered_queue.empty() || ordered_running) {
        return false;
    }
    for (const auto& worker : workers) {
        if (!worker->queue.empty() || worker->busy) {
            return false;
        }
    }
    return true;
}

bool Scheduler::OrderedReady() const {
    if (ordered_queue.empty() || ordered_running) {
        return false;
    }
    const RecordChunk& next = *ordered_queue.front();
    for (u32 i = 0; i < next.segment; ++i) {
        if (next.batch->recorded[i] != next.batch->handed[i]) {
            return false;
        }
    }
    return next.batch->recorded[next.segment] >= next.after;
}

void Scheduler::SyncRecording() {
    if (workers.empty()) {
        return;
    }
    KickRecording(true);
    BbStats::WaitTimer timer{BbStats::sync_recording_ns};
    std::unique_lock lk{recorder_mutex};
    recorder_idle_cv.wait(lk, [this] { return RecordingIdle(); });
}

void Scheduler::EnterDirectMode() {
    // bbport: with submissions going out from the recording threads, the commands recorded
    // here get a segment of their own, from this thread's pool, after the segments handed over:
    // the recording threads go on with those instead of this thread waiting for them (~90 us,
    // the upscaler's passes every frame). BB_DIRECT_SEGMENT=0: the wait.
    static const bool own_segment = [] {
        const char* env = std::getenv("BB_DIRECT_SEGMENT");
        return !(env && env[0] == '0');
    }();
    if (own_segment && async_submit && current_segment + 2 < MaxSegments &&
        !BbToggle::Disabled(BbToggle::ThreadedRecording)) {
        if (is_rendering) {
            CarrySuspend(true);
            Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRendering(); });
            is_rendering = false;
            resume_rendering = true;
        } else {
            CarrySuspend(false);
        }
        // A segment nothing was handed to yet is not begun: it becomes the direct one.
        const bool untouched =
            batch->handed[current_segment] == 0 && full_chunks.empty() && CurrentChunk().Empty();
        if (!untouched) {
            // Its recording thread ends the segment's command buffer (the pool is that thread's).
            Record([](vk::CommandBuffer cmdbuf) { Check(cmdbuf.end()); });
            HandOver();
            ++current_segment;
        }
        if (!direct_pool) {
            direct_pool = std::make_unique<CommandPool>(instance, &work_semaphore);
        }
        current_cmdbuf = BeginCommandBuffer(*direct_pool);
        batch->segments[current_segment] = current_cmdbuf;
        segment_bytes = 0;
        direct_mode = true;
        direct_segment = true;
        dynamic_state.Invalidate();
        CarryResume(false);
        return;
    }
    SyncRecording();
    direct_mode = true;
    // The recording threads are idle: this thread may use the segment's command pool.
    auto& cmdbuf = batch->segments[current_segment];
    if (!cmdbuf) {
        cmdbuf = BeginCommandBuffer(*workers[current_segment % workers.size()]->pool);
    }
    current_cmdbuf = cmdbuf;
}

void Scheduler::LeaveDirectSegment() {
    if (is_rendering) {
        CarrySuspend(true);
        current_cmdbuf.endRendering();
        is_rendering = false;
        resume_rendering = true;
    } else {
        CarrySuspend(false);
    }
    Check(current_cmdbuf.end());
    current_cmdbuf = vk::CommandBuffer{};
    direct_mode = false;
    direct_segment = false;
    ++current_segment;
    batch->segments[current_segment] = vk::CommandBuffer{};
    segment_bytes = 0;
    active_worker.store(current_segment % workers.size(), std::memory_order_relaxed);
    dynamic_state.Invalidate();
    CarryResume(false);
}

void Scheduler::RecorderThread(std::stop_token stoken, u32 index) {
    if (index == 0) {
        Common::SetCurrentThreadName("bb:VkRecorder");
    } else {
        char name[16];
        std::snprintf(name, sizeof(name), "bb:VkRecorder%u", index);
        Common::SetCurrentThreadName(name);
    }
    Worker& worker = *workers[index];
    std::vector<std::unique_ptr<RecordChunk>> ordered;
    const auto has_work = [&] {
        return !worker.queue.empty() || OrderedReady();
    };
    while (true) {
        // The thread of the segment being recorded spins briefly before sleeping: its next
        // chunk usually follows within microseconds, and a sleeping thread costs the producer
        // a wake-up syscall per kick. With few hardware threads (Steam Deck: 8) the spin would
        // take time from guest threads. The others sleep until a new segment reaches them.
        if (active_worker.load(std::memory_order_relaxed) == index) {
            static const auto spin_time =
                std::chrono::microseconds(BbThreads::Available() >= 12 ? 200 : 20);
            const auto spin_until = std::chrono::steady_clock::now() + spin_time;
            for (u32 spins = 1; worker.queued.load(std::memory_order_acquire) == 0 &&
                                ordered_queued.load(std::memory_order_acquire) == 0;
                 ++spins) {
                __builtin_ia32_pause();
                // The clock is read every 256 pauses, not per iteration.
                if (!(spins & 255) &&
                    (stoken.stop_requested() || std::chrono::steady_clock::now() >= spin_until)) {
                    break;
                }
            }
        }
        std::unique_ptr<RecordChunk> chunk;
        {
            std::unique_lock lk{recorder_mutex};
            if (!has_work()) {
                worker.sleeping = true;
                PublishQ0WaitState();
                worker.cv.wait(lk, stoken, has_work);
                worker.sleeping = false;
                if (q0_wait_state_enabled) ++worker.q0_wait_returns;
                PublishQ0WaitState();
                if (!has_work()) {
                    return; // stop requested
                }
            }
            if (OrderedReady()) {
                // Every chunk whose commands are recorded, in order.
                do {
                    ordered.push_back(std::move(ordered_queue.front()));
                    ordered_queue.pop_front();
                    if (q0_wait_state_enabled) ++q0_popped_chunks;
                } while (OrderedReady());
                ordered_running = true;
                if (q0_wait_state_enabled) {
                    worker.q0_ordered_begin += ordered.size();
                    ++q0_ordered_claim_epoch;
                    if (q0_actor_phase) q0_actor_phase->selected_epoch.store(q0_ordered_claim_epoch);
                }
                ordered_queued.store(ordered_queue.size(), std::memory_order_release);
            } else {
                chunk = std::move(worker.queue.front());
                worker.queue.pop_front();
                worker.queued.store(worker.queue.size(), std::memory_order_release);
                worker.busy = true;
                if (q0_wait_state_enabled) ++worker.q0_command_begin;
            }
            PublishQ0WaitState();
        }
        if (!ordered.empty()) {
            for (auto& task : ordered) {
                task->Execute({});
            }
            std::scoped_lock lk{recorder_mutex};
            for (auto& task : ordered) {
                free_chunks.push_back(std::move(task));
            }
            if (q0_wait_state_enabled) worker.q0_ordered_end += ordered.size();
            ordered.clear();
            ordered_running = false;
            PublishQ0WaitState();
            recorder_idle_cv.notify_all();
            continue;
        }
        // The segment's command buffer is begun by the first chunk; it is ended at the
        // submission, after SyncRecording(). Only this thread uses the pool meanwhile.
        auto& cmdbuf = chunk->batch->segments[chunk->segment];
        if (!cmdbuf) {
            cmdbuf = BeginCommandBuffer(*worker.pool);
        }
        chunk->Execute(cmdbuf);
        {
            std::scoped_lock lk{recorder_mutex};
            ++chunk->batch->recorded[chunk->segment];
            free_chunks.push_back(std::move(chunk));
            worker.busy = false;
            if (q0_wait_state_enabled) ++worker.q0_command_end;
            PublishQ0WaitState();
            if (worker.queue.empty()) {
                recorder_idle_cv.notify_all();
            }
        }
    }
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    Wait(presubmit_tick);
}

void Scheduler::Wait(u64 tick) {
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    BbWaitTrace::Scope trace;
    BbStats::WaitTimer timer{BbStats::tick_wait_ns};
    work_semaphore.Wait(tick);
}

void Scheduler::PopPendingOperations() {
    if (num_pending_ops.load(std::memory_order_acquire) == 0) {
        return; // every draw comes here
    }
    std::unique_lock lk(pending_ops_mutex);
    // bbport: this runs on every draw and dispatch. Querying the timeline semaphore is an
    // ioctl, so it is skipped when nothing waits and done once per 32 calls (~0.3 ms; reading
    // the clock per draw instead was itself a hot spot).
    if (pending_ops.empty()) {
        return;
    }
    if (!work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        if ((++pending_polls & 31) != 0 && !BbToggle::Disabled(BbToggle::PendingPollLimit)) {
            return;
        }
        work_semaphore.Refresh();
    }
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        num_pending_ops.fetch_sub(1, std::memory_order_release);
    }
}

SubmissionBatch* Scheduler::AcquireBatch() {
    std::scoped_lock lk{recorder_mutex};
    SubmissionBatch* next;
    if (free_batches.empty()) {
        next = all_batches.emplace_back(std::make_unique<SubmissionBatch>()).get();
    } else {
        next = free_batches.back();
        free_batches.pop_back();
    }
    next->Reset();
    return next;
}

void Scheduler::AllocateWorkerCommandBuffers() {
    if (async_submit) {
        // BB_ASYNC_SUBMIT: the earlier submissions may still be recorded (and their pools
        // used) on the recording threads: segment 0 is begun there too, on its first chunk,
        // or by EnterDirectMode once they are idle.
        batch = AcquireBatch();
        current_cmdbuf = vk::CommandBuffer{};
        direct_mode = false;
    } else {
        const vk::CommandBufferBeginInfo begin_info = {
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        };
        current_cmdbuf = command_pool.Commit();
        Check(current_cmdbuf.begin(begin_info));
        // bbport: the first segment (worker 0, whose pool this is) of the next submission.
        if (!batch) {
            batch = AcquireBatch();
        }
        batch->Reset();
        batch->segments[0] = current_cmdbuf;
    }
    current_segment = 0;
    segment_bytes = 0;
    active_worker.store(0, std::memory_order_relaxed);

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    ProducerScope producer{*this, "SubmitExecution"};
    if (async_submit) {
        SubmitAsync(info);
        return;
    }
    std::scoped_lock lk{submit_mutex};
    Q0SubmitOwnerScope q0_owner{this, 5};
    const u64 signal_value = work_semaphore.NextTick();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, batch->segments[0]);
    }
#endif

    if (on_submit) {
        on_submit(info);
    }

    EndRendering();
    CarrySuspend(false);
    if (auto* profiler = GpuProfiler::Get(); profiler && profiler->Records(this)) {
        // Until the next submission's first timestamp: mostly the GPU waiting for it.
        profiler->Mark(0x5B317ull, [] { return std::string{"(between submissions: GPU idle)"}; });
    }
    q0_owner.Stage(12); // SYNC_RECORDING: only the actual call below
    SyncRecording();
    // Guest memory copies into staging read by this submission (copy threads).
    q0_owner.Stage(13); // SYNC_COPIES: only the actual wait below
    WaitHostCopies();
    q0_owner.Stage(4); // BUILD_SUBMIT_INFO
    resume_rendering = false;
    // bbport: the segments' command buffers in stream order (the recording threads are idle).
    std::array<vk::CommandBuffer, MaxSegments> cmdbufs;
    u32 num_cmdbufs = 0;
    if (workers.empty()) {
        cmdbufs[num_cmdbufs++] = current_cmdbuf;
    } else {
        for (u32 i = 0; i <= current_segment; ++i) {
            if (batch->segments[i]) {
                cmdbufs[num_cmdbufs++] = batch->segments[i];
            }
        }
        recorded_segments.fetch_add(num_cmdbufs, std::memory_order_relaxed);
        recorded_submissions.fetch_add(1, std::memory_order_relaxed);
    }
    for (u32 i = 0; i < num_cmdbufs; ++i) {
        Check(cmdbufs[i].end());
    }

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    static constexpr std::array<vk::PipelineStageFlags, 2> wait_stage_masks = {
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
    };

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = num_cmdbufs,
        .pCommandBuffers = cmdbufs.data(),
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    q0_owner.Stage(14);
    for (auto& operation : info.before_submit) {
        operation();
    }
    ImGui::Core::TextureManager::Submit();
    q0_owner.Stage(7);
    if(info.p3_input_clock)info.p3_input_clock->SubmitEnter(p3_clock_context,
        P3Clock::OpaqueKey(static_cast<VkSemaphore>(work_semaphore.Handle())),signal_value);
    auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    if(info.p3_input_clock)info.p3_input_clock->SubmitReturned(static_cast<int32_t>(submit_result));
    if (submit_result == vk::Result::eErrorDeviceLost) {
        Breadcrumbs::ReportDeviceLost("submit");
    }
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");
    q0_owner.Stage(8);
    submitted_tick.store(signal_value, std::memory_order_release);
    submitted_tick.notify_all();

    // bbport: no semaphore query here (an ioctl per submission, ~3% of the recording thread):
    // PopPendingOperations asks when an operation waits, the GPU signal thread on every fence.
    AllocateWorkerCommandBuffers();
    CarryResume(false);

    // Apply pending operations
    PopPendingOperations();
}

void Scheduler::SubmitAsync(SubmitInfo& info) {
    const u64 signal_value = work_semaphore.NextTick();
    if (on_submit) {
        on_submit(info);
    }
    EndRendering();
    CarrySuspend(false);
    if (auto* profiler = GpuProfiler::Get(); profiler && profiler->Records(this)) {
        profiler->Mark(0x5B317ull, [] { return std::string{"(between submissions: GPU idle)"}; });
    }
    resume_rendering = false;
    // Every segment's command buffer is ended by its recording thread (the pool is its own);
    // MaybeSplit ended the earlier ones. A direct segment is this thread's.
    if (direct_segment) {
        Check(current_cmdbuf.end());
        current_cmdbuf = vk::CommandBuffer{};
        direct_mode = false;
        direct_segment = false;
    } else {
        Record([](vk::CommandBuffer cmdbuf) { Check(cmdbuf.end()); });
    }
    // Guest memory copies into staging this submission reads: on the copy threads by then.
    BbCopy::FlushBatch();
    info.AddSignal(work_semaphore.Handle(), signal_value);
    // Goes out once the commands handed over before it are recorded (an ordered task: in order
    // with the other submissions and after the copies and signals queued before it).
    RecordOrdered([this, submitted = batch, info = std::move(info), signal_value,
                   diagnostic = q0_actor_phase]() mutable {
        PublishQ0Phase(diagnostic.get(), 3, 3, true);
        BbCopy::WaitAsync();
        PublishQ0Phase(diagnostic.get(), 3, 4);
        std::array<vk::CommandBuffer, MaxSegments> cmdbufs;
        u32 num_cmdbufs = 0;
        for (u32 i = 0; i < MaxSegments; ++i) {
            if (submitted->segments[i]) {
                cmdbufs[num_cmdbufs++] = submitted->segments[i];
            }
        }
        recorded_segments.fetch_add(num_cmdbufs, std::memory_order_relaxed);
        recorded_submissions.fetch_add(1, std::memory_order_relaxed);
        static constexpr std::array<vk::PipelineStageFlags, 4> wait_stage_masks = {
            vk::PipelineStageFlagBits::eAllCommands,
            vk::PipelineStageFlagBits::eColorAttachmentOutput,
            vk::PipelineStageFlagBits::eAllCommands,
            vk::PipelineStageFlagBits::eAllCommands,
        };
        const vk::TimelineSemaphoreSubmitInfo timeline_si = {
            .waitSemaphoreValueCount = info.num_wait_semas,
            .pWaitSemaphoreValues = info.wait_ticks.data(),
            .signalSemaphoreValueCount = info.num_signal_semas,
            .pSignalSemaphoreValues = info.signal_ticks.data(),
        };
        const vk::SubmitInfo submit_info = {
            .pNext = &timeline_si,
            .waitSemaphoreCount = info.num_wait_semas,
            .pWaitSemaphores = info.wait_semas.data(),
            .pWaitDstStageMask = wait_stage_masks.data(),
            .commandBufferCount = num_cmdbufs,
            .pCommandBuffers = cmdbufs.data(),
            .signalSemaphoreCount = info.num_signal_semas,
            .pSignalSemaphores = info.signal_semas.data(),
        };
        PublishQ0Phase(diagnostic.get(), 3, 5); // waiter must not overwrite mutex owner
        {
            std::scoped_lock lk{submit_mutex};
            Q0SubmitOwnerScope q0_owner{this, 3};
            q0_owner.Stage(6);
            PublishQ0Phase(diagnostic.get(), 3, 6);
            for (auto& operation : info.before_submit) {
                operation();
            }
            q0_owner.Stage(7);
            PublishQ0Phase(diagnostic.get(), 3, 7);
            if(info.p3_input_clock)info.p3_input_clock->SubmitEnter(p3_clock_context,
                P3Clock::OpaqueKey(static_cast<VkSemaphore>(work_semaphore.Handle())),signal_value);
            const auto result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
            if(info.p3_input_clock)info.p3_input_clock->SubmitReturned(static_cast<int32_t>(result));
            if (result == vk::Result::eErrorDeviceLost) {
                Breadcrumbs::ReportDeviceLost("submit");
            }
            ASSERT_MSG(result != vk::Result::eErrorDeviceLost, "Device lost during submit");
        }
        PublishQ0Phase(diagnostic.get(), 3, 8);
        submitted_tick.store(signal_value, std::memory_order_release);
        submitted_tick.notify_all();
        PublishQ0Phase(diagnostic.get(), 3, 9);
        std::scoped_lock lk{recorder_mutex};
        PublishQ0Phase(diagnostic.get(), 3, 10);
        free_batches.push_back(submitted);
        PublishQ0Phase(diagnostic.get(), 3, 11, false, true);
    });
    KickRecording(true);
    AllocateWorkerCommandBuffers();
    CarryResume(false);
    PopPendingOperations();
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
    // A null command buffer only updates the dirty flags, exactly as recording would.
    CommitWith(instance.IsDepthBoundsSupported(), instance.IsDynamicColorWriteMaskSupported(),
               instance.IsAttachmentFeedbackLoopLayoutSupported(), [&](auto&& command) {
                   if (cmdbuf) {
                       command(cmdbuf);
                   }
               });
}

} // namespace Vulkan
