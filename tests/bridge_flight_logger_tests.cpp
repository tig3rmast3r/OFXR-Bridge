#include "xrfg/bridge_flight_logger.hpp"

#include <windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

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

} // namespace

int main() {
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

        xrfg::BridgeFlightLogger enabled;
        enabled.initialize(enabled_directory);
        require(enabled.enabled(), "enabled INI did not start bridge logging");
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
        require(
            log.find(
                "op=nvidia_gpu_stages result=2 dur_us=0 a=101 b=202 c=303") !=
                    std::string::npos &&
                log.find(
                    "op=nvidia_gpu_total result=0 dur_us=0 a=404 b=1010 c=77") !=
                    std::string::npos,
            "bridge log is missing V049 NVIDIA GPU timing fields");

        std::cout << "Bridge flight logger tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failure: " << exception.what() << '\n';
        return 1;
    }
}
