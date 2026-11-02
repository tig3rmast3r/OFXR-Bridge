#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/ram_timing.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace xrfg {
namespace {

#ifndef XRFG_IMPLEMENTATION_VERSION
#define XRFG_IMPLEMENTATION_VERSION 0
#endif

constexpr wchar_t kConfigurationFileName[] = L"ofxr_bridge.ini";
constexpr wchar_t kConfigurationSection[] = L"diagnostics";
constexpr std::uint64_t kMegabyte = 1024ull * 1024ull;

[[nodiscard]] const char* operation_name(
    BridgeFlightOperation operation) noexcept {
    switch (operation) {
    case BridgeFlightOperation::steamvr_delivery_attach: return "steamvr_delivery_attach";
    case BridgeFlightOperation::view_configuration: return "view_configuration";
    case BridgeFlightOperation::vulkan_negotiation: return "vulkan_negotiation";
    case BridgeFlightOperation::compositor_vsync: return "compositor_vsync";
    case BridgeFlightOperation::compositor_presentation: return "compositor_presentation";
    case BridgeFlightOperation::compositor_timeline: return "compositor_timeline";
    case BridgeFlightOperation::native_dlssg_record: return "native_dlssg_record";
    case BridgeFlightOperation::native_dlssg_evaluate: return "native_dlssg_evaluate";
    case BridgeFlightOperation::native_dlssg_decision: return "native_dlssg_decision";
    case BridgeFlightOperation::native_dlssg_provider: return "native_dlssg_provider";
    case BridgeFlightOperation::vulkan_bridge: return "vulkan_bridge";
    case BridgeFlightOperation::process_excluded: return "process_excluded";
    case BridgeFlightOperation::projection_view_rect: return "projection_view_rect";
    case BridgeFlightOperation::steamvr_delivery: return "steamvr_delivery";
    case BridgeFlightOperation::d3d11_bridge: return "d3d11_bridge";
    case BridgeFlightOperation::vram_usage: return "vram_usage";
    case BridgeFlightOperation::fsr_eye_binding: return "fsr_eye_binding";
    case BridgeFlightOperation::fsr_hudless_pair: return "fsr_hudless_pair";
    case BridgeFlightOperation::fsr_recovery: return "fsr_recovery";
    case BridgeFlightOperation::fsr_context_lifecycle: return "fsr_context_lifecycle";
    case BridgeFlightOperation::fsr_submit_stage: return "fsr_submit_stage";
    case BridgeFlightOperation::fsr_eye_resources: return "fsr_eye_resources";
    case BridgeFlightOperation::fsr_eye_rects: return "fsr_eye_rects";
    case BridgeFlightOperation::fsr_eye_motion: return "fsr_eye_motion";
    case BridgeFlightOperation::fsr_eye_pose: return "fsr_eye_pose";
    case BridgeFlightOperation::fsr_eye_orientation: return "fsr_eye_orientation";
    case BridgeFlightOperation::fsr_eye_fov: return "fsr_eye_fov";
    case BridgeFlightOperation::fsr_eye_clip_vertex: return "fsr_eye_clip_vertex";

    case BridgeFlightOperation::logger: return "logger";
    case BridgeFlightOperation::negotiation: return "negotiation";
    case BridgeFlightOperation::instance_create: return "instance_create";
    case BridgeFlightOperation::instance_destroy: return "instance_destroy";
    case BridgeFlightOperation::session_create: return "session_create";
    case BridgeFlightOperation::session_destroy: return "session_destroy";
    case BridgeFlightOperation::session_begin: return "session_begin";
    case BridgeFlightOperation::session_end: return "session_end";
    case BridgeFlightOperation::application_wait_frame: return "app_wait_frame";
    case BridgeFlightOperation::application_begin_frame: return "app_begin_frame";
    case BridgeFlightOperation::application_end_frame: return "app_end_frame";
    case BridgeFlightOperation::application_swapchain_acquire:
        return "app_swapchain_acquire";
    case BridgeFlightOperation::application_swapchain_wait:
        return "app_swapchain_wait";
    case BridgeFlightOperation::application_swapchain_release:
        return "app_swapchain_release";
    case BridgeFlightOperation::private_swapchain_acquire:
        return "private_swapchain_acquire";
    case BridgeFlightOperation::private_swapchain_wait:
        return "private_swapchain_wait";
    case BridgeFlightOperation::private_swapchain_release:
        return "private_swapchain_release";
    case BridgeFlightOperation::synthesis_initialize: return "synthesis_initialize";
    case BridgeFlightOperation::synthesis_prime: return "synthesis_prime";
    case BridgeFlightOperation::rife_worker: return "rife_worker";
    case BridgeFlightOperation::rife_memory: return "rife_memory";
    case BridgeFlightOperation::synthesis_pair: return "synthesis_pair";
    case BridgeFlightOperation::downstream_first_end_frame:
        return "downstream_first_end_frame";
    case BridgeFlightOperation::internal_wait_frame: return "internal_wait_frame";
    case BridgeFlightOperation::internal_begin_frame: return "internal_begin_frame";
    case BridgeFlightOperation::internal_end_frame: return "internal_end_frame";
    case BridgeFlightOperation::continuity_reset: return "continuity_reset";
    case BridgeFlightOperation::gpu_drain: return "gpu_drain";
    case BridgeFlightOperation::swapchain_create: return "swapchain_create";
    case BridgeFlightOperation::swapchain_eligibility: return "swapchain_eligibility";
    case BridgeFlightOperation::projection_mapping: return "projection_mapping";
    case BridgeFlightOperation::generation_prepare: return "generation_prepare";
    case BridgeFlightOperation::session_binding: return "session_binding";
    case BridgeFlightOperation::swapchain_image: return "swapchain_image";
    case BridgeFlightOperation::d3d11_capture: return "d3d11_capture";
    case BridgeFlightOperation::d3d11_publish: return "d3d11_publish";
    case BridgeFlightOperation::history_capture: return "history_capture";
    case BridgeFlightOperation::three_slot_worker_wait: return "three_slot_worker_wait";
    case BridgeFlightOperation::three_slot_buffer_wait: return "three_slot_buffer_wait";
    case BridgeFlightOperation::three_slot_handoff: return "three_slot_handoff";
    case BridgeFlightOperation::three_slot_gpu_ready: return "three_slot_gpu_ready";
    case BridgeFlightOperation::three_slot_present: return "three_slot_present";
    case BridgeFlightOperation::composition_layer: return "composition_layer";
    case BridgeFlightOperation::composition_publish: return "composition_publish";
    case BridgeFlightOperation::timing_worker_run: return "timing_worker_run";
    case BridgeFlightOperation::timing_slot_wait: return "timing_slot_wait";
    case BridgeFlightOperation::timing_director_wait: return "timing_director_wait";
    case BridgeFlightOperation::timing_worker_publish: return "timing_worker_publish";
    case BridgeFlightOperation::timing_runtime_gate: return "timing_runtime_gate";
    case BridgeFlightOperation::timing_worker_prepare: return "timing_worker_prepare";
    case BridgeFlightOperation::timing_synthesis_complete: return "timing_synthesis_complete";
    case BridgeFlightOperation::timing_publication_complete: return "timing_publication_complete";
    case BridgeFlightOperation::timing_aux_complete: return "timing_aux_complete";
    case BridgeFlightOperation::timing_worker_build: return "timing_worker_build";
    case BridgeFlightOperation::runtime_identity: return "runtime_identity";
    case BridgeFlightOperation::presenter_submission:
        return "presenter_submission";
    case BridgeFlightOperation::presenter_source:
        return "presenter_source";
    case BridgeFlightOperation::presenter_clock_anchor:
        return "presenter_clock_anchor";
    case BridgeFlightOperation::presenter_clock_capability:
        return "presenter_clock_capability";
    case BridgeFlightOperation::presenter_queue_drop:
        return "presenter_queue_drop";
    case BridgeFlightOperation::presenter_transition:
        return "presenter_transition";
    case BridgeFlightOperation::nvidia_gpu_stages:
        return "nvidia_gpu_stages";
    case BridgeFlightOperation::nvidia_gpu_total:
        return "nvidia_gpu_total";
    case BridgeFlightOperation::presenter_pace:
        return "presenter_pace";
    case BridgeFlightOperation::runtime_entry_section:
        return "runtime_entry_section";
    case BridgeFlightOperation::synthesis_frame_start_wait:
        return "synthesis_frame_start_wait";
    case BridgeFlightOperation::embedded_configuration: return "embedded_configuration";
    case BridgeFlightOperation::synthesis_gpu_span:
        return "synthesis_gpu_span";
    case BridgeFlightOperation::virtual_clock_clamp:
        return "virtual_clock_clamp";
    case BridgeFlightOperation::session_state:
        return "session_state";
    case BridgeFlightOperation::presenter_pair_release:
        return "presenter_pair_release";
    case BridgeFlightOperation::luke_guide_contract: return "luke_guide_contract";
    case BridgeFlightOperation::luke_projection_contract:
        return "luke_projection_contract";
    case BridgeFlightOperation::luke_output_contract: return "luke_output_contract";
    case BridgeFlightOperation::luke_frame_contract: return "luke_frame_contract";
    case BridgeFlightOperation::luke_pose_contract: return "luke_pose_contract";
    case BridgeFlightOperation::luke_afw_stage: return "luke_afw_stage";
    case BridgeFlightOperation::gpu_fence_snapshot: return "gpu_fence_snapshot";
    case BridgeFlightOperation::luke_inline_view: return "luke_inline_view";
    case BridgeFlightOperation::luke_inline_pose_bits: return "luke_inline_pose_bits";
    case BridgeFlightOperation::gpu_device_status: return "gpu_device_status";
    case BridgeFlightOperation::history_capture_wait: return "history_capture_wait";
    case BridgeFlightOperation::menu_status: return "menu_status";
    }
    if (operation == BridgeFlightOperation::luke_release) return "luke_release";
    if (operation == BridgeFlightOperation::luke_filter) return "luke_filter";
    if (operation == BridgeFlightOperation::luke_cycle_wait) return "luke_cycle_wait";
    if (operation == BridgeFlightOperation::steamvr_queue_configuration) return "steamvr_queue_configuration";
    if (operation == BridgeFlightOperation::benchmark_configuration) return "benchmark_configuration";
    if (operation == BridgeFlightOperation::benchmark_rates) return "benchmark_rates";
    if (operation == BridgeFlightOperation::benchmark_cycles) return "benchmark_cycles";
    if (operation == BridgeFlightOperation::benchmark_stages) return "benchmark_stages";
    if (operation == BridgeFlightOperation::benchmark_command) return "benchmark_command";
    if (operation == BridgeFlightOperation::gpu_afw) return "gpu_afw";
    return "unknown";
}

[[nodiscard]] bool startup_operation(BridgeFlightOperation operation, std::int64_t result) noexcept {
    return (operation == BridgeFlightOperation::logger && result > 0) ||
        operation == BridgeFlightOperation::runtime_identity ||
        operation == BridgeFlightOperation::session_binding ||
        operation == BridgeFlightOperation::steamvr_queue_configuration ||
        operation == BridgeFlightOperation::benchmark_configuration ||
        operation == BridgeFlightOperation::presenter_clock_capability ||
        operation == BridgeFlightOperation::view_configuration ||
        operation == BridgeFlightOperation::instance_create ||
        operation == BridgeFlightOperation::session_create ||
        operation == BridgeFlightOperation::swapchain_eligibility ||
        (operation == BridgeFlightOperation::synthesis_initialize && result < 0) ||
        (operation == BridgeFlightOperation::generation_prepare && result < 0) ||
        operation == BridgeFlightOperation::native_dlssg_provider ||
        operation == BridgeFlightOperation::negotiation ||
        (operation == BridgeFlightOperation::d3d11_bridge && result != 4) ||
        (operation == BridgeFlightOperation::presenter_transition && result >= 600 && result <= 604) ||
        operation == BridgeFlightOperation::vulkan_bridge;
}

[[nodiscard]] std::filesystem::path fallback_log_directory() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::filesystem::path(buffer.data()) / L"ofxr-bridge" / L"Logs";
}

[[nodiscard]] std::wstring log_file_name() {
    static std::atomic<std::uint64_t> serial{0};
    const auto ordinal = serial.fetch_add(1, std::memory_order_relaxed);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::array<wchar_t, 128> name{};
    _snwprintf_s(
        name.data(),
        name.size(),
        _TRUNCATE,
        L"ofxr-bridge-flight-%04u%02u%02u-%02u%02u%02u-%03u-pid%lu-%llu.log",
        static_cast<unsigned>(time.wYear),
        static_cast<unsigned>(time.wMonth),
        static_cast<unsigned>(time.wDay),
        static_cast<unsigned>(time.wHour),
        static_cast<unsigned>(time.wMinute),
        static_cast<unsigned>(time.wSecond),
        static_cast<unsigned>(time.wMilliseconds),
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long long>(ordinal));
    return name.data();
}

[[nodiscard]] HANDLE create_log_file(
    const std::filesystem::path& directory,
    std::filesystem::path* output_path) noexcept {
    try {
        if (directory.empty() || output_path == nullptr) {
            return INVALID_HANDLE_VALUE;
        }
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            return INVALID_HANDLE_VALUE;
        }
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            const auto path = directory / log_file_name();
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                *output_path = path;
                return file;
            }
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
                return INVALID_HANDLE_VALUE;
        }
        return INVALID_HANDLE_VALUE;
    } catch (...) {
        return INVALID_HANDLE_VALUE;
    }
}

[[nodiscard]] std::filesystem::path current_module_directory() noexcept {
    try {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&initialize_bridge_flight_logger),
                &module)) {
            return {};
        }
        std::array<wchar_t, 32768> path{};
        const DWORD length = GetModuleFileNameW(
            module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size()) {
            return {};
        }
        return std::filesystem::path(path.data()).parent_path();
    } catch (...) {
        return {};
    }
}

} // namespace

// Rendering threads enqueue raw records only. A single diagnostic writer owns
// formatting, disk writes, wrapping and flushing; queue pressure drops records
// instead of blocking the game, synthesis worker or director.
struct BridgeFlightLogger::Impl {
    struct Record {
        std::uint64_t sequence{}, duration{}, a{}, b{}, c{};
        std::int64_t counter{}, result{};
        DWORD thread{};
        BridgeFlightOperation operation{};
        char phase{};
    };
    struct Slot { std::atomic<std::uint64_t> turn{}; Record record{}; };
    static constexpr std::uint64_t capacity = 262144;
    std::uint64_t epoch{};
    HANDLE file{INVALID_HANDLE_VALUE};
    HANDLE benchmark_file{INVALID_HANDLE_VALUE};
    LARGE_INTEGER frequency{}, origin{};
    std::atomic<std::uint64_t> next_sequence{1};
    std::filesystem::path path;
    std::uint64_t maximum_bytes{32 * kMegabyte};
    bool flush_each_event{};
    std::atomic<bool> active{false}, stop{false};
    std::unique_ptr<Slot[]> slots;
    std::atomic<std::uint64_t> enqueue_position{0}, dropped{0};
    std::uint64_t dequeue_position{}, file_position{};
    std::array<char, 65536> batch{};
    std::size_t batch_size{};
    // Writer-owned bounded metadata: re-emitted after truncation, never from
    // render threads. No frame-by-frame events are retained.
    std::array<Record, 128> startup_records{};
    std::size_t startup_count{};
    std::thread writer;
    std::mutex flush_mutex;
    std::condition_variable flush_condition;
    std::atomic<std::uint64_t> flush_target{0}, flush_requested{0}, flush_completed{0};

    void start() {
        slots = std::make_unique<Slot[]>(capacity);
        for (std::uint64_t i = 0; i < capacity; ++i) slots[i].turn.store(i);
        active.store(true);
        writer = std::thread([this] { run(); });
    }
    void write(std::uint64_t sequence, std::int64_t counter, char phase,
        BridgeFlightOperation operation, std::int64_t result, std::uint64_t duration,
        std::uint64_t a, std::uint64_t b, std::uint64_t c, DWORD thread = 0) noexcept {
        if (!active.load(std::memory_order_relaxed)) return;
        auto position = enqueue_position.load(std::memory_order_relaxed);
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            auto& slot = slots[position & (capacity - 1)];
            const auto turn = slot.turn.load(std::memory_order_acquire);
            if (turn == position) {
                if (!enqueue_position.compare_exchange_weak(position, position + 1,
                        std::memory_order_relaxed)) continue;
                slot.record = {sequence, duration, a, b, c, counter, result,
                    thread ? thread : GetCurrentThreadId(), operation, phase};
                slot.turn.store(position + 1, std::memory_order_release);
                return;
            }
            if (turn < position) break; // full: never wait for the writer
            position = enqueue_position.load(std::memory_order_relaxed);
        }
        dropped.fetch_add(1, std::memory_order_relaxed);
    }
    bool pop(Record& record) noexcept {
        auto& slot = slots[dequeue_position & (capacity - 1)];
        if (slot.turn.load(std::memory_order_acquire) != dequeue_position + 1) return false;
        record = slot.record;
        slot.turn.store(dequeue_position + capacity, std::memory_order_release);
        ++dequeue_position;
        return true;
    }
    bool write_batch() noexcept {
        if (batch_size == 0) return true;
        DWORD written{};
        const bool good = WriteFile(file, batch.data(), static_cast<DWORD>(batch_size),
            &written, nullptr) && written == batch_size;
        batch_size = 0;
        if (!good) active.store(false);
        return good;
    }
    void serialize(const Record& record, bool replayed = false) noexcept {
        if (!replayed) {
            const bool metadata = startup_operation(record.operation, record.result);
            if (metadata) {
                auto found = std::find_if(startup_records.begin(), startup_records.begin() + startup_count,
                    [&](const Record& prior) { return prior.operation == record.operation &&
                        prior.a == record.a && prior.result == record.result && prior.phase == record.phase; });
                if (found != startup_records.begin() + startup_count) *found = record;
                else if (startup_count < startup_records.size()) startup_records[startup_count++] = record;
            }
        }
        const double ms = frequency.QuadPart > 0
            ? static_cast<double>(record.counter - origin.QuadPart) * 1000.0 / frequency.QuadPart : 0.0;
        std::array<char, 384> line{};
        const int length = std::snprintf(line.data(), line.size(),
            "seq=%llu ms=%.3f tid=%lu phase=%c op=%s result=%lld dur_us=%llu a=%llu b=%llu c=%llu\r\n",
            static_cast<unsigned long long>(record.sequence), ms, static_cast<unsigned long>(record.thread),
            record.phase, operation_name(record.operation), static_cast<long long>(record.result),
            static_cast<unsigned long long>(record.duration), static_cast<unsigned long long>(record.a),
            static_cast<unsigned long long>(record.b), static_cast<unsigned long long>(record.c));
        if (length <= 0 || static_cast<std::size_t>(length) >= line.size()) return;
        // The same asynchronous writer keeps a compact benchmark sidecar.
        // It does not wrap when verbose flight records reach their size limit.
        const bool benchmark = record.operation == BridgeFlightOperation::benchmark_configuration ||
            record.operation == BridgeFlightOperation::benchmark_command ||
            record.operation == BridgeFlightOperation::benchmark_rates || record.operation == BridgeFlightOperation::benchmark_cycles ||
            record.operation == BridgeFlightOperation::benchmark_stages ||
            record.operation == BridgeFlightOperation::logger || record.operation == BridgeFlightOperation::session_binding ||
            record.operation == BridgeFlightOperation::steamvr_queue_configuration || record.operation == BridgeFlightOperation::view_configuration ||
            record.operation == BridgeFlightOperation::runtime_identity;
        if (!replayed && benchmark && benchmark_file != INVALID_HANDLE_VALUE) {
            DWORD written{};
            if (!WriteFile(benchmark_file, line.data(), static_cast<DWORD>(length), &written, nullptr) || written != length) {
                CloseHandle(benchmark_file); benchmark_file = INVALID_HANDLE_VALUE;
            }
        }
        if (file_position + length > maximum_bytes) {
            if (!write_batch()) return;
            LARGE_INTEGER beginning{};
            if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) {
                active.store(false);
                return;
            }
            file_position = 0;
            for (std::size_t index = 0; index < startup_count; ++index) {
                auto metadata = startup_records[index];
                metadata.sequence = next_sequence.fetch_add(1, std::memory_order_relaxed);
                metadata.phase = 'M';
                serialize(metadata, true);
            }
        }
        if (batch_size + length > batch.size() && !write_batch()) return;
        std::memcpy(batch.data() + batch_size, line.data(), length);
        batch_size += length;
        file_position += length;
        if (flush_each_event) {
            static_cast<void>(write_batch());
            static_cast<void>(FlushFileBuffers(file)); // writer thread only
        }
    }
    void run() noexcept {
        for (;;) {
            Record record{};
            bool found = false;
            for (unsigned i = 0; i < 4096 && pop(record); ++i) {
                found = true;
                if (active.load()) serialize(record);
            }
            const auto lost = dropped.exchange(0);
            if (lost && active.load()) {
                LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
                serialize({next_sequence.fetch_add(1), 0, lost, capacity, 0,
                    now.QuadPart, -1, GetCurrentThreadId(), BridgeFlightOperation::logger, 'I'});
            }
            static_cast<void>(write_batch());
            const auto request = flush_requested.load(std::memory_order_acquire);
            if (request > flush_completed.load() && dequeue_position >= flush_target.load()) {
                static_cast<void>(FlushFileBuffers(file));
                { std::scoped_lock lock(flush_mutex); flush_completed.store(request); }
                flush_condition.notify_all();
            }
            if (stop.load() && dequeue_position == enqueue_position.load()) break;
            if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        static_cast<void>(FlushFileBuffers(file));
        if (benchmark_file != INVALID_HANDLE_VALUE) static_cast<void>(FlushFileBuffers(benchmark_file));
    }
    void flush() {
        std::unique_lock lock(flush_mutex);
        flush_target.store(enqueue_position.load(), std::memory_order_release);
        const auto request = flush_requested.fetch_add(1, std::memory_order_release) + 1;
        flush_condition.wait(lock, [&] { return flush_completed.load() >= request; });
    }
    void finish() noexcept {
        stop.store(true);
        if (writer.joinable()) writer.join();
        active.store(false);
    }
    ~Impl() {
        finish();
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (benchmark_file != INVALID_HANDLE_VALUE) CloseHandle(benchmark_file);
    }
};

BridgeFlightLogger::BridgeFlightLogger() noexcept = default;
BridgeFlightLogger::~BridgeFlightLogger() { shutdown(); }

void BridgeFlightLogger::remember_startup(BridgeFlightOperation operation,
    char phase, std::int64_t counter, std::int64_t result, std::uint64_t duration,
    std::uint64_t a, std::uint64_t b, std::uint64_t c) noexcept {
    if (!startup_operation(operation, result)) return;
    try {
        std::scoped_lock lock(startup_mutex_);
        auto found = std::find_if(startup_.begin(), startup_.begin() + startup_count_,
            [&](const StartupRecord& prior) { return prior.operation == operation &&
                prior.a == a && prior.result == result && prior.phase == phase; });
        const StartupRecord record{operation, phase, counter, result, duration,
            a, b, c, GetCurrentThreadId()};
        if (found != startup_.begin() + startup_count_) *found = record;
        else if (startup_count_ < startup_.size()) startup_[startup_count_++] = record;
    } catch (...) {}
}

void BridgeFlightLogger::start_disk_locked() {
    auto implementation = std::make_shared<Impl>();
    const auto config_path = module_directory_ / kConfigurationFileName;
    const UINT maximum_mb = std::clamp<UINT>(GetPrivateProfileIntW(
        kConfigurationSection, L"max_file_mb", 32, config_path.c_str()), 1, 256);
    implementation->maximum_bytes = static_cast<std::uint64_t>(maximum_mb) * kMegabyte;
    implementation->flush_each_event = GetPrivateProfileIntW(kConfigurationSection,
        L"flush_each_event", 0, config_path.c_str()) != 0;
    implementation->file = create_log_file(module_directory_, &implementation->path);
    if (implementation->file == INVALID_HANDLE_VALUE)
        implementation->file = create_log_file(fallback_log_directory(), &implementation->path);
    if (implementation->file == INVALID_HANDLE_VALUE || base_frequency_ <= 0) return;
    const auto benchmark_path = benchmark_log_path(implementation->path);
    implementation->benchmark_file = CreateFileW(benchmark_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    implementation->frequency.QuadPart = base_frequency_;
    implementation->origin.QuadPart = base_origin_;
    implementation->epoch = ++disk_epoch_;
    implementation->start();
    {
        // Close the off-to-on metadata race: a new metadata event either joins
        // this snapshot or observes the published active writer after the lock.
        std::scoped_lock lock(startup_mutex_);
        for (std::size_t index = 0; index < startup_count_; ++index) {
            const auto& record = startup_[index];
            implementation->write(implementation->next_sequence.fetch_add(1), record.counter,
                'M', record.operation, record.result, record.duration,
                record.a, record.b, record.c, record.thread);
        }
        impl_.store(implementation, std::memory_order_release);
        disk_enabled_.store(true, std::memory_order_release);
    }
    event(BridgeFlightOperation::logger, XRFG_IMPLEMENTATION_VERSION,
        maximum_mb, implementation->flush_each_event ? 1 : 0, GetCurrentProcessId());
}

void BridgeFlightLogger::initialize(const std::filesystem::path& module_directory) noexcept {
    try {
        std::scoped_lock control(control_mutex_);
        if (initialized_.load()) return;
        module_directory_ = module_directory;
        LARGE_INTEGER frequency{}, origin{};
        QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&origin);
        base_frequency_ = frequency.QuadPart; base_origin_ = origin.QuadPart;
        const auto config_path = module_directory_ / kConfigurationFileName;
        gpu_instrumentation_enabled_.store(GetPrivateProfileIntW(kConfigurationSection,
            L"gpu_instrumentation_enabled", 0, config_path.c_str()) != 0);
        candidate_enabled_ = GetPrivateProfileIntW(kConfigurationSection,
            L"logging_enabled", 0, config_path.c_str()) != 0;
        candidate_since_ = GetTickCount64();
        if (candidate_enabled_ && GetPrivateProfileIntW(kConfigurationSection, L"timing_capture", 0, config_path.c_str()) != 0) {
            auto capture = std::make_unique<RamTimingCapture>();
            const bool pose = GetPrivateProfileIntW(kConfigurationSection,
                L"pose_cadence_capture", 0, config_path.c_str()) != 0;
            if (capture->initialize(pose)) ram_ = std::move(capture);
            ram_recording_.store(ram_ != nullptr);
            ram_mode_ = true; // Exclusive even if RAM initialization failed.
        } else {
            if (candidate_enabled_) start_disk_locked();
        }
        initialized_.store(true, std::memory_order_release);
    } catch (...) { disk_enabled_.store(false); impl_.store(nullptr); }
}

void BridgeFlightLogger::refresh_settings() noexcept {
    if (!initialized_.load(std::memory_order_acquire)) return;
    const auto now = GetTickCount64();
    auto next = next_poll_.load(std::memory_order_relaxed);
    if (now < next || !next_poll_.compare_exchange_strong(next, now + 250)) return;
    try {
        std::scoped_lock control(control_mutex_);
        if (!initialized_.load()) return;
        // Retire the old writer on the control caller, never on whichever
        // producer happens to release the last transient shared reference.
        std::erase_if(retired_, [](const auto& writer) { return writer.use_count() == 1; });
        const bool desired = GetPrivateProfileIntW(kConfigurationSection, L"logging_enabled", 0,
            (module_directory_ / kConfigurationFileName).c_str()) != 0;
        if (desired != candidate_enabled_) {
            candidate_enabled_ = desired; candidate_since_ = now; return;
        }
        if (now - candidate_since_ < 250) return;
        if (ram_mode_) { ram_recording_.store(desired && ram_ != nullptr, std::memory_order_release); return; }
        if (desired == disk_enabled_.load()) return;
        if (desired) start_disk_locked();
        else {
            event(BridgeFlightOperation::logger, 0);
            disk_enabled_.store(false, std::memory_order_release);
            auto prior = impl_.exchange(nullptr, std::memory_order_acq_rel);
            if (prior) retired_.push_back(std::move(prior));
        }
    } catch (...) {} // Live diagnostics cannot fail a frame.
}

void BridgeFlightLogger::shutdown() noexcept {
    try {
        std::scoped_lock control(control_mutex_);
        event(BridgeFlightOperation::logger, 0);
        disk_enabled_.store(false, std::memory_order_release);
        auto prior = impl_.exchange(nullptr, std::memory_order_acq_rel);
        if (prior) retired_.push_back(std::move(prior));
        retired_.clear(); // Teardown callers have already drained producers.
        ram_recording_.store(false); ram_.reset(); ram_mode_ = false;
        gpu_instrumentation_enabled_.store(false);
        initialized_.store(false, std::memory_order_release);
        next_poll_.store(0);
        { std::scoped_lock startup(startup_mutex_); startup_count_ = 0; }
    } catch (...) {}
}

bool BridgeFlightLogger::enabled() const noexcept {
    if (!disk_enabled_.load(std::memory_order_acquire)) return false;
    const auto implementation = impl_.load(std::memory_order_acquire);
    return implementation && implementation->active.load(std::memory_order_relaxed);
}
void BridgeFlightLogger::flush() noexcept {
    try { const auto implementation = impl_.load(); if (implementation) implementation->flush(); }
    catch (...) {}
}
bool BridgeFlightLogger::ram_enabled() const noexcept { return ram_recording_.load(std::memory_order_relaxed); }
std::int64_t BridgeFlightLogger::microseconds_for_counter(std::int64_t counter) const noexcept {
    const auto implementation = impl_.load();
    if (!implementation || implementation->frequency.QuadPart <= 0) return 0;
    return (counter - implementation->origin.QuadPart) * 1000000ll / implementation->frequency.QuadPart;
}
std::filesystem::path BridgeFlightLogger::log_path() const {
    const auto implementation = impl_.load();
    return implementation ? implementation->path : std::filesystem::path{};
}
BridgeFlightToken BridgeFlightLogger::begin(BridgeFlightOperation operation,
    std::uint64_t a, std::uint64_t b, std::uint64_t c) noexcept {
    if (ram_) return ram_enabled() ? ram_->begin(operation, a, b, c) : BridgeFlightToken{};
    if (!disk_enabled_.load(std::memory_order_acquire)) return {};
    LARGE_INTEGER now{}; if (!QueryPerformanceCounter(&now)) return {};
    remember_startup(operation, 'B', now.QuadPart, 0, 0, a, b, c);
    if (!disk_enabled_.load(std::memory_order_acquire)) return {};
    const auto implementation = impl_.load(std::memory_order_acquire);
    if (!implementation || !implementation->active.load()) return {};
    const BridgeFlightToken token{implementation->next_sequence.fetch_add(1), now.QuadPart, implementation->epoch};
    implementation->write(token.sequence, now.QuadPart, 'B', operation, 0, 0, a, b, c);
    return token;
}
void BridgeFlightLogger::end(BridgeFlightToken token, BridgeFlightOperation operation,
    std::int64_t result, std::uint64_t a, std::uint64_t b, std::uint64_t c) noexcept {
    if (ram_) { if (ram_enabled()) ram_->end(token, operation, result, a, b, c); return; }
    if (!disk_enabled_.load(std::memory_order_acquire)) return;
    LARGE_INTEGER now{}; if (!QueryPerformanceCounter(&now)) return;
    const auto duration = base_frequency_ > 0 && token.start_counter > 0 && now.QuadPart >= token.start_counter
        ? static_cast<std::uint64_t>((now.QuadPart - token.start_counter) * 1000000ll / base_frequency_) : 0;
    remember_startup(operation, 'E', now.QuadPart, result, duration, a, b, c);
    if (!disk_enabled_.load(std::memory_order_acquire) || token.sequence == 0) return;
    const auto implementation = impl_.load(std::memory_order_acquire);
    if (!implementation || token.recorder_epoch != implementation->epoch) return;
    implementation->write(token.sequence, now.QuadPart, 'E', operation, result, duration, a, b, c);
}
void BridgeFlightLogger::event(BridgeFlightOperation operation, std::int64_t result,
    std::uint64_t a, std::uint64_t b, std::uint64_t c) noexcept {
    if (ram_) { if (ram_enabled()) ram_->event(operation, result, a, b, c); return; }
    if (!disk_enabled_.load(std::memory_order_acquire)) return;
    LARGE_INTEGER now{}; if (!QueryPerformanceCounter(&now)) return;
    remember_startup(operation, 'I', now.QuadPart, result, 0, a, b, c);
    if (!disk_enabled_.load(std::memory_order_acquire)) return;
    const auto implementation = impl_.load(std::memory_order_acquire);
    if (!implementation) return;
    implementation->write(implementation->next_sequence.fetch_add(1), now.QuadPart,
        'I', operation, result, 0, a, b, c);
}

BridgeFlightLogger& bridge_flight_logger() noexcept {
    static BridgeFlightLogger logger;
    return logger;
}

void initialize_bridge_flight_logger() noexcept {
    static std::once_flag once;
    try {
        std::call_once(once, [] {
            bridge_flight_logger().initialize(current_module_directory());
        });
    } catch (...) {
    }
}

} // namespace xrfg
