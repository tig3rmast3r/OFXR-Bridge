#include "xrfg/bridge_flight_logger.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write(const fs::path& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << contents;
    require(static_cast<bool>(stream), "fixture write failed");
}

std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(stream)), {}};
}

struct TempDirectory {
    fs::path path;

    TempDirectory() {
        std::array<wchar_t, MAX_PATH> root{};
        GetTempPathW(static_cast<DWORD>(root.size()), root.data());
        path = fs::path(root.data()) /
            (L"xrfg-flight-tests-" + std::to_wstring(GetCurrentProcessId()));
        fs::remove_all(path);
        fs::create_directories(path);
    }

    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void concurrent_records(const fs::path& directory, bool wrap, bool flush) {
    fs::create_directories(directory);
    write(directory / "ofxr_bridge.ini", std::string("[diagnostics]\r\n") +
        "logging_enabled=1\r\nmax_file_mb=" + (wrap ? "1" : "8") +
        "\r\nflush_each_event=" + (flush ? "1" : "0") + "\r\n");
    xrfg::BridgeFlightLogger logger;
    logger.initialize(directory);
    require(logger.enabled(), "concurrent logger did not initialize");
    const auto path = logger.log_path();
    logger.event(xrfg::BridgeFlightOperation::runtime_identity, 999, 42, 7, 3);
    if (wrap) {
        logger.event(xrfg::BridgeFlightOperation::synthesis_initialize,
            E_INVALIDARG, 0x661D0069ULL, 256, 3);
        logger.event(xrfg::BridgeFlightOperation::benchmark_configuration, 0, 42, 33, 100);
        logger.event(xrfg::BridgeFlightOperation::benchmark_rates, 1, 42, 43, 42);
        logger.event(xrfg::BridgeFlightOperation::fsr_recovery, -1, 113, 5, 6);
        logger.event(xrfg::BridgeFlightOperation::fsr_eye_motion, 0, 42, 2, 3);
        logger.event(xrfg::BridgeFlightOperation::presenter_transition, 5750, 123, 456, 789);
        logger.event(xrfg::BridgeFlightOperation::presenter_transition, 5751, 1, 0, 0);
        const auto infer = logger.begin(xrfg::BridgeFlightOperation::rife_infer, 42);
        logger.end(infer, xrfg::BridgeFlightOperation::rife_infer, 0, 42, 2, 3);
        logger.event(xrfg::BridgeFlightOperation::presenter_transition, 752, 0, 70, 1);
        logger.event(xrfg::BridgeFlightOperation::presenter_transition, 752, 0, 70, 1);
        logger.event(xrfg::BridgeFlightOperation::presenter_transition, 752, 0, 71, 1);
        logger.flush();
    }
    constexpr unsigned threads = 8;
    const unsigned iterations = wrap ? 12000 : flush ? 32 : 1500;
    std::atomic<unsigned> ready{};
    std::atomic<bool> start{};
    std::vector<std::thread> writers;
    for (unsigned t = 0; t < threads; ++t) {
        writers.emplace_back([&, t] {
            ready.fetch_add(1);
            while (!start.load()) std::this_thread::yield();
            for (unsigned i = 0; i < iterations; ++i) {
                logger.event(xrfg::BridgeFlightOperation::synthesis_pair, 123,
                    t, i, static_cast<std::uint64_t>(t) * iterations + i);
            }
        });
    }
    while (ready.load() != threads) std::this_thread::yield();
    start.store(true);
    for (auto& writer : writers) writer.join();
    // Written after joining: it must survive even if the last writer wrapped.
    logger.event(xrfg::BridgeFlightOperation::application_wait_frame, 456);
    logger.shutdown();
    const auto contents = read(path);
    require(contents.size() <= (wrap ? 1u : 8u) * 1024 * 1024,
        "concurrent logger exceeded file size cap");
    require(contents.find('\0') == std::string::npos, "holes in positioned log");
    require(contents.ends_with("\r\n"), "partial trailing record");
    require(contents.find("op=app_wait_frame result=456") != std::string::npos,
        "final event missing after concurrent writes");
    require(contents.find("op=logger result=0") != std::string::npos,
        "shutdown event missing after concurrent writes");
    std::unordered_set<unsigned long long> sequences, identities;
    unsigned long long reported_drops{}, drop_records{};
    std::istringstream lines(contents);
    std::string line;
    while (std::getline(lines, line)) {
        unsigned long long seq{}, duration{}, a{}, b{}, c{};
        double ms{};
        unsigned long tid{};
        char phase{}, operation[64]{};
        long long result{};
        int consumed{};
        const int fields = std::sscanf(line.c_str(),
            "seq=%llu ms=%lf tid=%lu phase=%c op=%63s result=%lld dur_us=%llu "
            "a=%llu b=%llu c=%llu%n", &seq, &ms, &tid, &phase, operation,
            &result, &duration, &a, &b, &c, &consumed);
        require(fields == 10 && line.substr(consumed) == "\r",
            "torn, overlapping or malformed concurrent log record");
        require(sequences.insert(seq).second, "duplicated record sequence");
        if (std::string(operation) == "logger" && result == -1) {
            reported_drops += a;
            ++drop_records;
        }
        if (std::string(operation) == "synthesis_pair") {
            require(result == 123 && phase == 'I' && duration == 0 &&
                a < threads && b < iterations && c == a * iterations + b,
                "concurrent event payload corrupted");
            require(identities.insert(c).second, "duplicated event payload");
        }
    }
    require(!identities.empty(), "concurrent log lost all worker records");
    if (!wrap) require(identities.size() + reported_drops == threads * iterations &&
        sequences.size() + reported_drops == threads * iterations + 4 + drop_records,
        "non-wrapping logger lost records without reporting bounded queue pressure");
    if (wrap) {
        require(identities.size() < threads * iterations, "wrap fixture did not exercise truncation");
        require(contents.find("phase=M op=synthesis_initialize result=" +
            std::to_string(static_cast<std::int64_t>(E_INVALIDARG)) +
            " dur_us=0 a=" + std::to_string(0x661D0069ULL) + " b=256 c=3") != std::string::npos,
            "rotation lost the initialization failure guard needed for late F9 inspection");
        require(contents.find("phase=M op=runtime_identity result=999") != std::string::npos &&
            contents.find("phase=M op=logger result=") != std::string::npos,
            "rotation lost startup identity or implementation metadata");
        const auto compact = xrfg::benchmark_log_path(path);
        const auto benchmark = read(compact);
        require(benchmark.find("op=benchmark_configuration result=0 dur_us=0 a=42 b=33 c=100") != std::string::npos &&
            benchmark.find("op=benchmark_rates result=1 dur_us=0 a=42 b=43 c=42") != std::string::npos,
            "compact benchmark lost first measurements during verbose rotation");
        require(benchmark.find("op=fsr_recovery result=-1 dur_us=0 a=113 b=5 c=6")!=std::string::npos &&
            benchmark.find("op=fsr_eye_motion result=0 dur_us=0 a=42 b=2 c=3")!=std::string::npos,
            "compact diagnostics lost FSR failure or sampled guide inputs during rotation");
        require(benchmark.find("op=presenter_transition result=5750 dur_us=0 a=123 b=456 c=789")!=std::string::npos &&
            benchmark.find("op=presenter_transition result=5751 dur_us=0 a=1 b=0 c=0")!=std::string::npos &&
            benchmark.find("phase=E op=rife_infer result=0")!=std::string::npos &&
            benchmark.find("phase=B op=rife_infer") == std::string::npos,
            "compact log lost RIFE inference or native-cycle evidence during verbose rotation");
        const std::string identity="op=presenter_transition result=752 dur_us=0 a=0 b=70 c=1";
        const auto first=benchmark.find(identity);
        require(first!=std::string::npos && benchmark.find(identity,first+identity.size())==std::string::npos &&
            benchmark.find("op=presenter_transition result=752 dur_us=0 a=0 b=71 c=1")!=std::string::npos,
            "compact eye identities must retain changes without duplicating steady frames");
    }
}

} // namespace

void live_switch_records(const fs::path& directory) {
    fs::create_directories(directory);
    const auto ini = directory / "ofxr_bridge.ini";
    write(ini, "[diagnostics]\r\nlogging_enabled=0\r\ngpu_instrumentation_enabled=1\r\n");
    xrfg::BridgeFlightLogger logger;
    logger.initialize(directory);
    logger.event(xrfg::BridgeFlightOperation::runtime_identity, 777, 1, 2, 3);
    logger.event(xrfg::BridgeFlightOperation::steamvr_queue_configuration, S_OK, 23, 3, 131073);
    logger.event(xrfg::BridgeFlightOperation::swapchain_eligibility, 13, 99, 0x887A0004, 1);
    require(!logger.enabled(), "live fixture started with logging active");
    std::atomic<bool> stop{};
    std::vector<std::thread> producers;
    for (unsigned index = 0; index < 4; ++index) {
        producers.emplace_back([&, index] {
            while (!stop.load()) {
                const auto token = logger.begin(xrfg::BridgeFlightOperation::application_wait_frame, index);
                logger.event(xrfg::BridgeFlightOperation::synthesis_pair, 8, index);
                logger.end(token, xrfg::BridgeFlightOperation::application_wait_frame, 8, index);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    const auto switch_to = [&](bool on) {
        require(WritePrivateProfileStringW(L"diagnostics", L"logging_enabled", on ? L"1" : L"0", ini.c_str()),
            "fixture switch write");
        logger.refresh_settings();
        std::this_thread::sleep_for(std::chrono::milliseconds(270));
        logger.refresh_settings();
        std::this_thread::sleep_for(std::chrono::milliseconds(270));
        logger.refresh_settings();
        require(logger.enabled() == on, "live logger did not follow stable switch");
        require(logger.gpu_instrumentation_enabled() == on, "GPU instrumentation must follow the logging switch");
    };
    try {
        switch_to(true);
        const auto first = logger.log_path();
        logger.flush();
        require(read(first).find("runtime_identity result=777") == std::string::npos &&
            read(first).find("steamvr_queue_configuration result=0 dur_us=0 a=23 b=3 c=131073") == std::string::npos &&
            read(first).find("swapchain_eligibility result=13") == std::string::npos,
            "logging off must not record hidden bootstrap metadata");
        logger.event(xrfg::BridgeFlightOperation::runtime_identity,777,1,2,3);
        logger.flush();
        require(read(first).find("phase=I op=runtime_identity result=777")!=std::string::npos,
            "active logging must retain real metadata for a later restart");
        const auto stale = logger.begin(xrfg::BridgeFlightOperation::application_wait_frame);
        require(stale.sequence != 0, "first live writer token");
        switch_to(false);
        // Respect the next 250ms control poll before the following toggle.
        std::this_thread::sleep_for(std::chrono::milliseconds(270));
        switch_to(true);
        const auto second = logger.log_path();
        require(first != second, "live restart truncated previous log");
        logger.end(stale, xrfg::BridgeFlightOperation::application_wait_frame, 4242);
        logger.flush();
        require(read(second).find("result=4242") == std::string::npos,
            "a span crossed recorder epochs");
        require(read(second).find("phase=M op=runtime_identity result=777") != std::string::npos,
            "live restart lost retained identity metadata");
    } catch (...) {
        stop.store(true); for (auto& producer : producers) producer.join();
        logger.shutdown(); throw;
    }
    stop.store(true); for (auto& producer : producers) producer.join();
    logger.shutdown();
}

int main(int argc, char** argv) {
    if (argc == 2) {
        xrfg::BridgeFlightLogger child;
        child.initialize(argv[1]);
        child.event(xrfg::BridgeFlightOperation::application_wait_frame, 6, 11, 22, 33);
        child.flush(); // Forced termination has no normal logger teardown.
        TerminateProcess(GetCurrentProcess(), 73);
        return 1;
    }
    try {
        TempDirectory temporary;

        xrfg::BridgeFlightLogger disabled;
        disabled.initialize(temporary.path / "disabled");
        require(!disabled.enabled(), "missing INI must keep logging disabled");
        require(disabled.begin(
                    xrfg::BridgeFlightOperation::application_wait_frame)
                    .sequence == 0,
            "disabled logger produced a token");

        const fs::path enabled_directory = temporary.path / "enabled";
        fs::create_directories(enabled_directory);
        write(
            enabled_directory / "ofxr_bridge.ini",
            "[diagnostics]\r\n"
            "logging_enabled=1\r\n"
            "max_file_mb=1\r\n"
            "flush_each_event=0\r\n");

        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr, exe, 32768);
        std::wstring command = L"\"" + std::wstring(exe) + L"\" \"" + enabled_directory.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
            nullptr, nullptr, &startup, &process) != FALSE, "child launch failed");
        const DWORD wait = WaitForSingleObject(process.hProcess, 10000);
        if (wait != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 74);
        DWORD code{};
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        require(wait == WAIT_OBJECT_0 && code == 73, "child did not terminate as expected");
        bool persisted = false;
        for (const auto& item : fs::directory_iterator(enabled_directory)) {
            if (item.path().extension() == ".log") {
                persisted |= read(item.path()).find(
                    "op=app_wait_frame result=6 dur_us=0 a=11 b=22 c=33") != std::string::npos;
            }
        }
        require(persisted, "last recorder event lost after forced termination");

        xrfg::BridgeFlightLogger enabled;
        enabled.initialize(enabled_directory);
        require(enabled.enabled(), "enabled INI did not start bridge logging");
        require(!enabled.gpu_instrumentation_enabled(), "normal logging enabled GPU instrumentation");
        const fs::path log_path = enabled.log_path();
        require(!log_path.empty() && log_path.parent_path() == enabled_directory,
            "bridge log was not created beside the DLL configuration");

        const auto token = enabled.begin(
            xrfg::BridgeFlightOperation::application_wait_frame,
            11,
            22,
            33);
        require(token.sequence != 0, "enabled logger did not produce a token");
        enabled.end(
            token,
            xrfg::BridgeFlightOperation::application_wait_frame,
            7,
            44,
            55,
            66);
        enabled.event(
            xrfg::BridgeFlightOperation::synthesis_pair,
            9,
            77,
            88,
            99);
        enabled.event(
            xrfg::BridgeFlightOperation::runtime_identity,
            1,
            2,
            3,
            4);
        enabled.event(
            xrfg::BridgeFlightOperation::presenter_transition,
            3);
        enabled.event(
            xrfg::BridgeFlightOperation::presenter_submission,
            0);
        enabled.event(
            xrfg::BridgeFlightOperation::presenter_source,
            0, 17, 123456, 17);
        enabled.event(
            xrfg::BridgeFlightOperation::presenter_clock_anchor,
            0, 123456, 234567, 10000000);
        enabled.event(
            xrfg::BridgeFlightOperation::presenter_clock_capability,
            1, 1, 1, 0);
        enabled.event(
            xrfg::BridgeFlightOperation::presenter_queue_drop,
            2, 1, 1, 0);
        enabled.event(
            xrfg::BridgeFlightOperation::three_slot_buffer_wait,
            0, 1234, 16666666, 0);
        enabled.event(
            xrfg::BridgeFlightOperation::nvidia_gpu_stages,
            2,
            101,
            202,
            303);
        enabled.event(
            xrfg::BridgeFlightOperation::nvidia_gpu_total,
            0,
            404,
            1010,
            77);
        enabled.shutdown();
        const auto gpu_directory = temporary.path / "gpu";
        fs::create_directories(gpu_directory);
        write(gpu_directory / "ofxr_bridge.ini",
            "[diagnostics]\r\nlogging_enabled=0\r\ngpu_instrumentation_enabled=1\r\n");
        xrfg::BridgeFlightLogger gpu;
        gpu.initialize(gpu_directory);
        require(!gpu.enabled() && !gpu.gpu_instrumentation_enabled(),
            "a retained GPU diagnostic flag must not activate instrumentation while logging is off");

        const std::string log = read(log_path);
        require(log.find("phase=B op=app_wait_frame") != std::string::npos,
            "bridge log is missing the wait begin boundary");
        require(log.find("phase=E op=app_wait_frame result=7") !=
                std::string::npos,
            "bridge log is missing the matching wait completion");
        require(log.find("phase=I op=synthesis_pair result=9") !=
                std::string::npos,
            "bridge log is missing a synthesis event");
        require(log.find("a=77 b=88 c=99") != std::string::npos,
            "bridge log lost diagnostic event fields");
        require(log.find("op=runtime_identity result=1") != std::string::npos &&
                log.find("op=presenter_transition result=3") !=
                    std::string::npos &&
                log.find("op=presenter_submission result=0") !=
                    std::string::npos,
            "bridge log is missing V043 presenter event names");
        require(log.find("op=presenter_queue_drop result=2 dur_us=0 a=1 b=1") !=
                    std::string::npos,
            "bridge log is missing latest-frame drop evidence");
        require(log.find("op=presenter_source result=0 dur_us=0 a=17 b=123456 c=17") !=
                    std::string::npos &&
                log.find("op=presenter_clock_anchor result=0 dur_us=0 a=123456 b=234567 c=10000000") !=
                    std::string::npos &&
                log.find("op=presenter_clock_capability result=1 dur_us=0 a=1 b=1") !=
                    std::string::npos,
            "bridge log is missing presenter deadline correlation fields");
        require(log.find("op=three_slot_buffer_wait result=0 dur_us=0 a=1234 b=16666666") !=
                    std::string::npos,
            "bridge log is missing three-slot handoff evidence");
        require(
            log.find(
                "op=nvidia_gpu_stages result=2 dur_us=0 a=101 b=202 c=303") !=
                    std::string::npos &&
                log.find(
                    "op=nvidia_gpu_total result=0 dur_us=0 a=404 b=1010 c=77") !=
                    std::string::npos,
            "bridge log is missing V049 NVIDIA GPU timing fields");

        // Repeated startups in one process must not truncate one another.
        const auto unique_directory = temporary.path / "unique";
        fs::create_directories(unique_directory);
        write(unique_directory / "ofxr_bridge.ini", "[diagnostics]\r\nlogging_enabled=1\r\n");
        std::unordered_set<std::wstring> paths;
        for (unsigned index = 0; index < 8; ++index) {
            xrfg::BridgeFlightLogger distinct;
            distinct.initialize(unique_directory);
            const auto path = distinct.log_path();
            require(distinct.enabled() && paths.insert(path.wstring()).second, "logger startup reused a filename");
            distinct.event(xrfg::BridgeFlightOperation::runtime_identity, index + 1000);
            distinct.shutdown();
            require(read(path).find("op=runtime_identity result=" + std::to_string(index + 1000)) != std::string::npos,
                "unique recorder lost its own startup record");
        }
        concurrent_records(temporary.path / "concurrent", false, false);
        concurrent_records(temporary.path / "wrap", true, false);
        concurrent_records(temporary.path / "flush", false, true);
        live_switch_records(temporary.path / "live-switch");

        std::cout << "Bridge flight logger tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failure: " << exception.what() << '\n';
        return 1;
    }
}
