#include "xrfg/bridge_flight_logger.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>

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
    case BridgeFlightOperation::runtime_identity: return "runtime_identity";
    case BridgeFlightOperation::presenter_submission:
        return "presenter_submission";
    case BridgeFlightOperation::presenter_transition:
        return "presenter_transition";
    case BridgeFlightOperation::nvidia_gpu_stages:
        return "nvidia_gpu_stages";
    case BridgeFlightOperation::nvidia_gpu_total:
        return "nvidia_gpu_total";
    }
    return "unknown";
}

[[nodiscard]] std::filesystem::path fallback_log_directory() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::filesystem::path(buffer.data()) / L"OFXR Bridge" / L"Logs";
}

[[nodiscard]] std::wstring log_file_name() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::array<wchar_t, 128> name{};
    _snwprintf_s(
        name.data(),
        name.size(),
        _TRUNCATE,
        L"ofxr-bridge-flight-%04u%02u%02u-%02u%02u%02u-pid%lu.log",
        static_cast<unsigned>(time.wYear),
        static_cast<unsigned>(time.wMonth),
        static_cast<unsigned>(time.wDay),
        static_cast<unsigned>(time.wHour),
        static_cast<unsigned>(time.wMinute),
        static_cast<unsigned>(time.wSecond),
        static_cast<unsigned long>(GetCurrentProcessId()));
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
        const auto path = directory / log_file_name();
        HANDLE file = CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            *output_path = path;
        }
        return file;
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

struct BridgeFlightLogger::Impl {
    HANDLE file{INVALID_HANDLE_VALUE};
    LARGE_INTEGER frequency{};
    LARGE_INTEGER origin{};
    SRWLOCK write_lock = SRWLOCK_INIT;
    std::atomic<std::uint64_t> next_sequence{1};
    std::filesystem::path path;
    std::uint64_t bytes_written{};
    std::uint64_t maximum_bytes{32 * kMegabyte};
    bool flush_each_event{};
    bool active{};

    void write(
        std::uint64_t sequence,
        std::int64_t counter,
        char phase,
        BridgeFlightOperation operation,
        std::int64_t result,
        std::uint64_t duration_microseconds,
        std::uint64_t a,
        std::uint64_t b,
        std::uint64_t c) noexcept {
        if (!active || file == INVALID_HANDLE_VALUE) {
            return;
        }

        const double elapsed_ms = frequency.QuadPart > 0
            ? static_cast<double>(counter - origin.QuadPart) * 1000.0 /
                  static_cast<double>(frequency.QuadPart)
            : 0.0;
        std::array<char, 384> line{};
        const int length = std::snprintf(
            line.data(),
            line.size(),
            "seq=%llu ms=%.3f tid=%lu phase=%c op=%s result=%lld dur_us=%llu "
            "a=%llu b=%llu c=%llu\r\n",
            static_cast<unsigned long long>(sequence),
            elapsed_ms,
            static_cast<unsigned long>(GetCurrentThreadId()),
            phase,
            operation_name(operation),
            static_cast<long long>(result),
            static_cast<unsigned long long>(duration_microseconds),
            static_cast<unsigned long long>(a),
            static_cast<unsigned long long>(b),
            static_cast<unsigned long long>(c));
        if (length <= 0 || static_cast<std::size_t>(length) >= line.size()) {
            return;
        }

        AcquireSRWLockExclusive(&write_lock);
        if (bytes_written + static_cast<std::uint64_t>(length) > maximum_bytes) {
            LARGE_INTEGER beginning{};
            if (SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN) &&
                SetEndOfFile(file)) {
                bytes_written = 0;
            }
        }
        DWORD written = 0;
        if (WriteFile(
                file,
                line.data(),
                static_cast<DWORD>(length),
                &written,
                nullptr)) {
            bytes_written += written;
            if (flush_each_event) {
                static_cast<void>(FlushFileBuffers(file));
            }
        }
        ReleaseSRWLockExclusive(&write_lock);
    }
};

BridgeFlightLogger::BridgeFlightLogger() noexcept = default;

BridgeFlightLogger::~BridgeFlightLogger() {
    shutdown();
}

void BridgeFlightLogger::initialize(
    const std::filesystem::path& module_directory) noexcept {
    try {
        if (impl_) {
            return;
        }
        const auto config_path = module_directory / kConfigurationFileName;
        if (GetPrivateProfileIntW(
                kConfigurationSection,
                L"logging_enabled",
                0,
                config_path.c_str()) == 0) {
            return;
        }

        auto implementation = std::make_unique<Impl>();
        const UINT maximum_mb = std::clamp<UINT>(
            GetPrivateProfileIntW(
                kConfigurationSection,
                L"max_file_mb",
                32,
                config_path.c_str()),
            1,
            256);
        implementation->maximum_bytes =
            static_cast<std::uint64_t>(maximum_mb) * kMegabyte;
        implementation->flush_each_event = GetPrivateProfileIntW(
            kConfigurationSection,
            L"flush_each_event",
            0,
            config_path.c_str()) != 0;
        implementation->file = create_log_file(
            module_directory, &implementation->path);
        if (implementation->file == INVALID_HANDLE_VALUE) {
            implementation->file = create_log_file(
                fallback_log_directory(), &implementation->path);
        }
        if (implementation->file == INVALID_HANDLE_VALUE ||
            !QueryPerformanceFrequency(&implementation->frequency) ||
            !QueryPerformanceCounter(&implementation->origin)) {
            if (implementation->file != INVALID_HANDLE_VALUE) {
                CloseHandle(implementation->file);
            }
            return;
        }
        implementation->active = true;
        impl_ = std::move(implementation);
        event(
            BridgeFlightOperation::logger,
            XRFG_IMPLEMENTATION_VERSION,
            maximum_mb,
            impl_->flush_each_event ? 1u : 0u,
            GetCurrentProcessId());
    } catch (...) {
        impl_.reset();
    }
}

void BridgeFlightLogger::shutdown() noexcept {
    try {
        if (!impl_) {
            return;
        }
        event(BridgeFlightOperation::logger, 0);
        AcquireSRWLockExclusive(&impl_->write_lock);
        impl_->active = false;
        if (impl_->file != INVALID_HANDLE_VALUE) {
            static_cast<void>(FlushFileBuffers(impl_->file));
            CloseHandle(impl_->file);
            impl_->file = INVALID_HANDLE_VALUE;
        }
        ReleaseSRWLockExclusive(&impl_->write_lock);
        impl_.reset();
    } catch (...) {
        impl_.reset();
    }
}

bool BridgeFlightLogger::enabled() const noexcept {
    return impl_ && impl_->active;
}

std::filesystem::path BridgeFlightLogger::log_path() const {
    return impl_ ? impl_->path : std::filesystem::path{};
}

BridgeFlightToken BridgeFlightLogger::begin(
    BridgeFlightOperation operation,
    std::uint64_t a,
    std::uint64_t b,
    std::uint64_t c) noexcept {
    if (!enabled()) {
        return {};
    }
    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now)) {
        return {};
    }
    const BridgeFlightToken token{
        impl_->next_sequence.fetch_add(1, std::memory_order_relaxed),
        now.QuadPart};
    impl_->write(
        token.sequence, token.start_counter, 'B', operation, 0, 0, a, b, c);
    return token;
}

void BridgeFlightLogger::end(
    BridgeFlightToken token,
    BridgeFlightOperation operation,
    std::int64_t result,
    std::uint64_t a,
    std::uint64_t b,
    std::uint64_t c) noexcept {
    if (!enabled() || token.sequence == 0) {
        return;
    }
    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now)) {
        return;
    }
    const std::uint64_t duration = impl_->frequency.QuadPart > 0 &&
            now.QuadPart >= token.start_counter
        ? static_cast<std::uint64_t>(
              (now.QuadPart - token.start_counter) * 1000000ll /
              impl_->frequency.QuadPart)
        : 0;
    impl_->write(
        token.sequence, now.QuadPart, 'E', operation, result, duration, a, b, c);
}

void BridgeFlightLogger::event(
    BridgeFlightOperation operation,
    std::int64_t result,
    std::uint64_t a,
    std::uint64_t b,
    std::uint64_t c) noexcept {
    if (!enabled()) {
        return;
    }
    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now)) {
        return;
    }
    impl_->write(
        impl_->next_sequence.fetch_add(1, std::memory_order_relaxed),
        now.QuadPart,
        'I',
        operation,
        result,
        0,
        a,
        b,
        c);
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
