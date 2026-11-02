#include "xrfg/gpu_usage.hpp"
#include "xrfg/autonomous_control.hpp"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
int main(){try {
    using namespace xrfg;
    const LUID adapter{0xabcd,0};
    const std::array<GpuEngineSample,6> sample{{
        {L"pid_1_luid_0x00000000_0x0000abcd_phys_0_eng_0_engtype_3D",30,true},
        {L"pid_2_luid_0x00000000_0x0000abcd_phys_0_eng_0_engtype_3D",40,true},
        {L"pid_2_luid_0x00000000_0x0000abcd_phys_0_eng_1_engtype_Compute",60,true},
        {L"pid_9_luid_0x00000000_0x00001234_phys_0_eng_0_engtype_3D",99,true},
        {L"pid_1_luid_0x00000000_0x0000abcd_phys_0_eng_2_engtype_Copy",99,false},
        {L"pid_1_luid_0x00000000_0x0000abcd_phys_0_eng_3_engtype_Copy",std::numeric_limits<double>::infinity(),true}}};
    const auto result=gpu_busiest_engine(sample,adapter,100);
    check(result.valid && result.percent==70,"sum processes on the same engine; max across different engines; ignore other adapters and invalid data");
    const std::array<GpuEngineSample,1> uppercase{{{L"pid_1_luid_0x00000000_0x0000ABCD_phys_0_eng_0_engtype_3D",33,true}}};
    check(gpu_busiest_engine(uppercase,adapter,100).percent==33,"Windows uppercase LUID matches the same adapter");
    check(!gpu_busiest_engine(sample,LUID{1,0},100).valid,"missing adapter is not a fabricated idle zero");
    const std::array<GpuEngineSample,1> idle{{{sample[0].instance,0,true}}};
    check(gpu_busiest_engine(idle,adapter,100).valid,"measured idle zero stays valid");
    auto saturated=sample;saturated[0].percent=90;
    check(gpu_busiest_engine(saturated,adapter,100).percent==100,"counter interval jitter is bounded at100");
    autonomous::Snapshot view;view.connected=true;view.runtime.gpu_usage=result;
    check(autonomous::interface_gpu_text(view,100)=="GPU 70%","fresh GPU percentage");
    check(autonomous::interface_gpu_text(view,3100)=="GPU N/A","stale GPU sample expires");
    view.connected=false;check(autonomous::interface_gpu_text(view,100)=="GPU N/A","disconnected GPU sample hidden");

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;Microsoft::WRL::ComPtr<IDXGIAdapter1> gpu;
    check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))),"DXGI factory");
    DXGI_ADAPTER_DESC1 desc{};
    for(UINT i=0;SUCCEEDED(factory->EnumAdapters1(i,&gpu));++i) {
        gpu->GetDesc1(&desc);if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))break;gpu.Reset();
    }
    check(gpu!=nullptr,"hardware adapter for live read-only counter check");
    GpuUsageSampler sampler;GpuUsageStatus live{};
    const auto deadline=GetTickCount64()+8000;
    std::uint64_t worst_call_us{};
    while(GetTickCount64()<deadline) {
        const auto entered=std::chrono::steady_clock::now();live=sampler.request(desc.AdapterLuid);
        const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-entered).count();
        worst_call_us=std::max(worst_call_us,std::uint64_t(us));
        if(live.valid)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(live.valid && live.percent>=0 && live.percent<=100,"live selected-adapter PDH interval must produce a real sample");
    std::cout<<"GPU live="<<live.percent<<"% cached request max="<<worst_call_us<<"us; aggregation, unavailable, stale and idle contracts PASS\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
