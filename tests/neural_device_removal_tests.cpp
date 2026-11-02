#define NOMINMAX
#include "DeviceRemovalDred.h"
#include "xrfg/neural_rendering.hpp"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace DeviceRemovalDred;
static unsigned checks = 0;
static void Check(bool ok, const char* message)
{
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
static std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
static void EnableDred()
{
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings;
    Check(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))), "DRED settings");
    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
}
static ComPtr<ID3D12Device5> Warp()
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
    Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "WARP adapter");
    ComPtr<ID3D12Device5> device;
    Check(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "WARP device5");
    return device;
}
static bool WaitReport(const std::filesystem::path& directory, unsigned timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    do
    {
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            if (Read(entry.path()).find("report_complete") != std::string::npos) return true;
        Sleep(10);
    } while (GetTickCount64() < deadline);
    return false;
}
static void TestWriter(const std::filesystem::path& root)
{
    const auto path = root / "fixture.txt";
    const UINT completed = 7;
    const UINT zero = 0;
    D3D12_AUTO_BREADCRUMB_OP history[10] {};
    history[7] = D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER;
    const wchar_t contextName[] = L"V23216 pre-SR NVIDIA NR model";
    D3D12_DRED_BREADCRUMB_CONTEXT context {6, contextName};
    // More than the retired collector's64 pending-node limit before the useful list.
    std::vector<D3D12_AUTO_BREADCRUMB_NODE1> nodes(81);
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        nodes[i].BreadcrumbCount = 10;
        nodes[i].pLastBreadcrumbValue = &zero;
        nodes[i].pCommandHistory = history;
        nodes[i].pNext = i + 1 < nodes.size() ? &nodes[i+1] : nullptr;
    }
    nodes.back().pCommandListDebugNameW = L"Graphic list";
    nodes.back().pLastBreadcrumbValue = &completed;
    nodes.back().BreadcrumbContextsCount = 1;
    nodes.back().pBreadcrumbContexts = &context;
    {
        File file; Check(file.Open(path), "fixture open");
        WriteBreadcrumbs(file, nodes.data());
        Check(file.Good(), "fixture IO");
    }
    const auto text = Read(path);
    Check(text.find("node=80") < text.find("node=0 "), "partial node precedes pending nodes");
    Check(text.find("<NEXT>7:") != std::string::npos, "exact next operation");
    Check(text.find("context_text=V23216 pre-SR NVIDIA NR model") != std::string::npos, "NR scope retained");
    Check(text.find("partial=1 pending=80") != std::string::npos, "all pending lists counted");

    std::vector<D3D12_AUTO_BREADCRUMB_OP> ring(65536, D3D12_AUTO_BREADCRUMB_OP_DISPATCH);
    ring[0] = D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER;
    auto node = nodes.back();
    node.BreadcrumbCount = 65538;
    const UINT last = 65536;
    node.pLastBreadcrumbValue = &last;
    node.pCommandHistory = ring.data();
    {
        File file; Check(file.Open(root / "ring.txt"), "ring fixture open");
        WriteNode(file, node, 0);
        Check(file.Good(), "ring IO");
    }
    const auto wrapped = Read(root / "ring.txt");
    Check(wrapped.find("retained_first=2 end=65538") != std::string::npos, "ring retained interval");
    Check(wrapped.find("<NEXT>65536:" + std::to_string(unsigned(D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER))) != std::string::npos,
          "wrapped operation indexed modulo65536");
}
static void TestLifetime(const std::filesystem::path& root)
{
    auto device = Warp();
    D3D12_COMMAND_QUEUE_DESC desc {};
    ComPtr<ID3D12CommandQueue> queue;
    Check(SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))), "queue creation");
    auto a = Monitor::Observe(device.Get(), root);
    auto b = Monitor::Observe(queue.Get(), root);
    Check(a && a == b, "one observer per device across device and queue owners");
    const auto firstPath = a->Path();
    Check(Read(firstPath).find("armed version=") != std::string::npos, "armed receipt flushed");
    Sleep(50);
    Check(Read(firstPath).find("device_removed utc=") == std::string::npos, "no healthy notification");
    a.reset();
    Check(b.use_count() == 1, "one owner release preserves other observer");
    b.reset();
    auto next = Monitor::Observe(device.Get(), root);
    Check(next && next->Path() != firstPath, "healthy teardown permits fresh observer");
    const auto nextPath = next->Path();

    // Only a WARP test device is removed; no game/physical GPU device is used.
    device->RemoveDevice();
    Check(WaitReport(root, 5000), "real removal notification without any Present");
    next.reset();
    const auto report = Read(nextPath);
    Check(report.find("device_removed utc=") != std::string::npos, "removed receipt");
    Check(report.find("nr_flight_begin version=") != std::string::npos, "RAM flight ring persisted on removal");
    Check(report.find("nr_flight_end") != std::string::npos, "RAM flight ring completed before DRED query");
    Check(report.find("breadcrumb_query=") != std::string::npos, "actual DRED queried");
    Check(report.find("page_fault_query=") != std::string::npos, "actual page fault data queried");
    Check(report.find("unexpected_healthy_notification") == std::string::npos, "real failure reason");
    Check(report.find("report_complete") == report.rfind("report_complete"), "one report per observer");
    queue.Reset();
    auto* raw = device.Detach();
    Check(raw->Release() == 0, "observer leaves no device reference after teardown");

    // A second device generation in the same process must not inherit the old
    // process-wide 'dredReported' suppression from the retired collector.
    auto replacement = Warp();
    Check(!Monitor::Observe(replacement.Get(), root / "missing-directory"), "file setup failure is nonfatal");
    auto replacementMonitor = Monitor::Observe(replacement.Get(), root);
    Check(bool(replacementMonitor), "replacement device arms after setup failure");
    const auto replacementPath = replacementMonitor->Path();
    replacement->RemoveDevice();
    const auto deadline = GetTickCount64() + 5000;
    while (Read(replacementPath).find("report_complete") == std::string::npos && GetTickCount64() < deadline) Sleep(10);
    Check(Read(replacementPath).find("report_complete") != std::string::npos, "second device gets its own complete report");
    replacementMonitor.reset();
    raw = replacement.Detach();
    Check(raw->Release() == 0, "replacement observer releases device");
}
static void TestTermination(const std::filesystem::path& root)
{
    struct Child
    {
        PROCESS_INFORMATION info {};
        ~Child()
        {
            if (info.hProcess) { TerminateProcess(info.hProcess, 99); WaitForSingleObject(info.hProcess, 5000); CloseHandle(info.hProcess); }
            if (info.hThread) CloseHandle(info.hThread);
        }
    } child;
    wchar_t executable[32768]; GetModuleFileNameW(nullptr, executable, 32768);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --child \"" + root.wstring() + L"\"";
    STARTUPINFOW startup {}; startup.cb = sizeof(startup);
    Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &startup, &child.info) != FALSE, "child launch");
    Check(WaitReport(root, 10000), "child persisted DRED before process termination");
    Check(TerminateProcess(child.info.hProcess, 99) != FALSE, "child terminate");
    Check(WaitForSingleObject(child.info.hProcess, 5000) == WAIT_OBJECT_0, "child exit");
    Check(WaitReport(root, 10), "complete report survives TerminateProcess without logger shutdown");
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        NrFlightTrace::SetEnabled(true);
        EnableDred();
        if (argc == 3 && std::wstring(argv[1]) == L"--child")
        {
            auto device = Warp();
            auto monitor = Monitor::Observe(device.Get(), argv[2]);
            Check(bool(monitor), "child monitor");
            device->RemoveDevice();
            Sleep(INFINITE);
            return 2;
        }
        const auto root = std::filesystem::absolute("build/dred-tests-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(root / "lifetime");
        std::filesystem::create_directories(root / "child");
        std::filesystem::create_directories(root / "crash-only");
        {
            auto device = Warp();
            NrFlightTrace::SetEnabled(false);
            Check(!Monitor::Observe(device.Get(), root / "disabled"), "logging off does not arm observer");
            Check(!std::filesystem::exists(root / "disabled"), "logging off creates no diagnostic directory");
            const auto sequence = NrFlightTrace::GetState().sequence.load();
            NrFlightTrace::Marker(nullptr, L"disabled");
            Check(NrFlightTrace::GetState().sequence.load() == sequence, "logging off records no NR event");
            NrFlightTrace::SetEnabled(true);
            NrFlightTrace::Marker(nullptr, L"enabled");
            Check(NrFlightTrace::GetState().sequence.load() == sequence + 1, "logging on retains NR event");
        }
        {
            auto device=Warp();
            xrfg::neural_rendering::Settings settings{};
            Check(!xrfg::neural_rendering::observe_device_removal(device.Get()),
                "default-off public device observer disabled");
            settings.crash_debug=true;
            xrfg::neural_rendering::configure(settings);
            auto owner=xrfg::neural_rendering::observe_device_removal(device.Get());
            auto monitor=std::static_pointer_cast<Monitor>(owner);
            Check(bool(monitor),"crash debug arms observer without F9");
            auto sequence=NrFlightTrace::GetState().sequence.load();
            NrFlightTrace::BarrierAfter(nullptr,L"verbose-only",nullptr,
                D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
            Check(NrFlightTrace::GetState().sequence.load()==sequence,
                "crash-only mode does not collect verbose per-barrier records");
            NrFlightTrace::Write("NR_SUBMIT",nullptr,L"crash-only receipt");
            Check(NrFlightTrace::GetState().sequence.load()==sequence+1,
                "crash debug retains submission receipt without F9");
            device->RemoveDevice();
            const auto deadline=GetTickCount64()+10000;
            while(Read(monitor->Path()).find("report_complete")==std::string::npos && GetTickCount64()<deadline)
                Sleep(10);
            Check(Read(monitor->Path()).find("report_complete")!=std::string::npos,
                "crash-only report persisted without F9");
            const auto text=Read(monitor->Path());
            Check(text.find("recorder_mode=crash-only")!=std::string::npos &&
                text.find("label=crash-only receipt")!=std::string::npos,
                "crash-only mode and retained receipt are present in actual report");
            settings.crash_debug=false;
            xrfg::neural_rendering::configure(settings);
            NrFlightTrace::SetEnabled(true);
        }
        TestWriter(root);
        TestLifetime(root / "lifetime");
        TestTermination(root / "child");
        std::cout << "PASS " << checks << " device-removal/DRED checks; evidence " << root.string() << '\n';
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
