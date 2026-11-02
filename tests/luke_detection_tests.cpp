#include "xrfg/luke_detection.hpp"
#include <windows.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct Module {
    HMODULE value{};
    explicit Module(const wchar_t* path) : value(LoadLibraryW(path)) {
        require(value != nullptr, "load real marker fixture");
    }
    void unload() {
        require(value && FreeLibrary(value), "unload real marker fixture");
        value = nullptr;
    }
    ~Module() { if (value) FreeLibrary(value); }
};
}

int wmain(int argc, wchar_t** argv) {
    using namespace xrfg::native_ingress;
    try {
        require(argc >= 3, "custom and conventional fixture paths required");
        if (argc > 3 && std::wstring_view(argv[3]) == L"--legacy-vulkan") {
            require(legacy_luke_vulkan_detection(),"selected executable uses V666 recognition");
            start_luke_detection_notifications();refresh_luke_detection(false);
            require(!luke_detection_notifications_ready_for_test()&&!luke_detected(),"legacy starts negative without notifications");
            {Module custom(argv[1]);require(luke_detected(),"legacy detects late custom marker without refresh");}
            require(!luke_detected(),"legacy notices custom unload without refresh");
            {Module conventional(argv[2]);require(luke_detected(),"legacy detects conventional marker without refresh");}
            require(!luke_detected(),"legacy notices conventional unload without refresh");
            require(luke_detection_scan_count_for_test()==0,"selected route does not invoke cached discovery or refcount lookup");
            std::cout<<"V666 Vulkan recognition: negative, late custom/conventional load and unload without cache refresh PASS\n";
            return 0;
        }
        if (argc > 3 && std::wstring_view(argv[3]) == L"--preloaded") {
            Module preloaded(argv[1]);
            refresh_luke_detection();
            require(luke_detected(), "startup discovers already loaded custom-path Luke");
            std::cout << "Preloaded custom-path Luke detected at startup\n";
            return 0;
        }
        start_luke_detection_notifications();
        require(luke_detection_notifications_ready_for_test(), "real Windows loader notifications registered");
        require(!luke_detected(), "plain native process starts non-Luke");
        auto scans = luke_detection_scan_count_for_test();
        for (unsigned i = 0; i < 100000; ++i) {
            require(!luke_detected(), "negative cached read");
            refresh_luke_detection(false);
        }
        require(luke_detection_scan_count_for_test() == scans,
            "native frames and unchanged monitor ticks do not rescan modules");

        Module custom(argv[1]);
        require(!luke_detected(), "loader callback only invalidates, never discovers under loader lock");
        refresh_luke_detection(false);
        require(luke_detected(), "late custom-path Luke detected after initial negative");
        require(luke_detection_scan_count_for_test() == scans + 1,
            "late load triggers one discovery");
        scans = luke_detection_scan_count_for_test();
        for (unsigned i = 0; i < 100000; ++i) {
            require(luke_detected(), "positive cached read");
            refresh_luke_detection(false);
        }
        require(luke_detection_scan_count_for_test() == scans,
            "Luke frames and unchanged monitor ticks do not rescan modules");
        custom.unload();
        refresh_luke_detection(false);
        require(!luke_detected(), "producer unload invalidates positive classification");
        require(luke_detection_scan_count_for_test() == scans + 1,
            "unload triggers one discovery");

        scans = luke_detection_scan_count_for_test();
        Module conventional(argv[2]);
        std::atomic_bool bad_read{};
        std::vector<std::thread> workers;
        for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&] {
            refresh_luke_detection(false);
            for (unsigned n = 0; n < 10000; ++n)
                if (!luke_detected()) bad_read.store(true);
        });
        for (auto& worker : workers) worker.join();
        require(!bad_read.load(), "concurrent readers see completed conventional-name detection");
        require(luke_detection_scan_count_for_test() == scans + 1,
            "concurrent dirty refreshes are coalesced into one scan");
        conventional.unload();
        refresh_luke_detection(false);
        require(!luke_detected(), "late conventional-name producer unload observed");

        scans = luke_detection_scan_count_for_test();
        refresh_luke_detection();
        require(!luke_detected(), "new native XR session stays non-Luke");
        require(luke_detection_scan_count_for_test() == scans + 1,
            "new session explicitly refreshes role independent of dirty state");
        std::cout << "Luke lifecycle: startup, late custom/name load, unload, concurrency and zero per-frame scans pass\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
