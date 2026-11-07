#include "xrfg/gpu_usage.hpp"
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cwchar>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xrfg {
GpuUsageStatus gpu_busiest_engine(std::span<const GpuEngineSample> samples,LUID luid,std::uint64_t now) {
    wchar_t identity[64]{};
    std::swprintf(identity,std::size(identity),L"luid_0x%08x_0x%08x_phys_",
        static_cast<unsigned>(luid.HighPart),static_cast<unsigned>(luid.LowPart));
    std::map<std::wstring_view,double> engines;
    for(const auto& s:samples) {
        if(!s.valid || !std::isfinite(s.percent) || s.percent<0)continue;
        const auto length=std::wcslen(identity);
        const auto found=std::search(s.instance.begin(),s.instance.end(),identity,identity+length,
            [](wchar_t a,wchar_t b){return std::towlower(a)==std::towlower(b);});
        if(found==s.instance.end())continue;
        const auto engine=s.instance.substr(std::size_t(found-s.instance.begin())+length);
        if(engine.find(L"_eng_")==std::wstring_view::npos)continue;
        // Identical phys/engine identities from different PIDs share one GPU
        // engine. Driver-supplied names are stable for a collection interval.
        engines[engine]+=s.percent;
    }
    if(engines.empty())return {};
    double busiest{};
    for(const auto& [engine,percent]:engines)busiest=std::max(busiest,percent);
    return {now,static_cast<float>(std::clamp(busiest,0.,100.)),true};
}

namespace {
// Independent CPU query: a missing GPU counter must not hide CPU usage.
struct CpuCounter {
    PDH_HQUERY query{};
    PDH_HCOUNTER counter{};
    ~CpuCounter(){close();}
    void close(){if(query)PdhCloseQuery(query);query=nullptr;counter=nullptr;}
    CpuUsageStatus collect() {
        if(!query) {
            if(PdhOpenQueryW(nullptr,0,&query)!=ERROR_SUCCESS)return {};
            if(PdhAddEnglishCounterW(query,L"\\Processor Information(_Total)\\% Processor Time",0,&counter)!=ERROR_SUCCESS ||
               PdhCollectQueryData(query)!=ERROR_SUCCESS)close();
            return {}; // prime the interval; startup is unavailable, not idle
        }
        PDH_FMT_COUNTERVALUE value{};
        if(PdhCollectQueryData(query)!=ERROR_SUCCESS ||
           PdhGetFormattedCounterValue(counter,PDH_FMT_DOUBLE,nullptr,&value)!=ERROR_SUCCESS ||
           (value.CStatus!=PDH_CSTATUS_VALID_DATA && value.CStatus!=PDH_CSTATUS_NEW_DATA) ||
           !std::isfinite(value.doubleValue) || value.doubleValue<0) {
            close();return {};
        }
        return {GetTickCount64(),static_cast<float>(std::clamp(value.doubleValue,0.,100.)),true};
    }
};
struct Counter {
    PDH_HQUERY query{};
    PDH_HCOUNTER counter{};
    ~Counter(){close();}
    void close(){if(query)PdhCloseQuery(query);query=nullptr;counter=nullptr;}
    bool open() {
        close();
        if(PdhOpenQueryW(nullptr,0,&query)!=ERROR_SUCCESS)return false;
        // English object/counter names work on localized Windows; the wildcard
        // handle is read through the documented formatted-array API.
        if(PdhAddEnglishCounterW(query,L"\\GPU Engine(*)\\Utilization Percentage",0,&counter)!=ERROR_SUCCESS) {
            close();return false;
        }
        if(PdhCollectQueryData(query)!=ERROR_SUCCESS){close();return false;}
        return true; // prime the interval, never display it as a measured zero
    }
    GpuUsageStatus collect(LUID luid) {
        if(PdhCollectQueryData(query)!=ERROR_SUCCESS)return {};
        for(unsigned attempt=0;attempt<3;++attempt) {
            DWORD bytes{},count{};
            if(PdhGetFormattedCounterArrayW(counter,PDH_FMT_DOUBLE,&bytes,&count,nullptr)!=PDH_MORE_DATA || !bytes)return {};
            std::vector<std::byte> buffer(bytes);
            auto* values=reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
            const auto result=PdhGetFormattedCounterArrayW(counter,PDH_FMT_DOUBLE,&bytes,&count,values);
            if(result==PDH_MORE_DATA)continue; // processes may appear between reads
            if(result!=ERROR_SUCCESS)return {};
            std::vector<GpuEngineSample> samples;samples.reserve(count);
            for(DWORD i=0;i<count;++i) {
                const auto& v=values[i];
                if(v.szName)samples.push_back({v.szName,v.FmtValue.doubleValue,
                    v.FmtValue.CStatus==PDH_CSTATUS_VALID_DATA || v.FmtValue.CStatus==PDH_CSTATUS_NEW_DATA});
            }
            return gpu_busiest_engine(samples,luid,GetTickCount64());
        }
        return {};
    }
};
bool same(LUID a,LUID b){return a.HighPart==b.HighPart && a.LowPart==b.LowPart;}
}

struct GpuUsageSampler::Impl {
    std::mutex mutex;
    std::condition_variable changed;
    LUID adapter{};
    std::uint64_t requested_ms{};
    GpuUsageStatus cached{};
    CpuUsageStatus cpu_cached{};
    std::jthread worker;
    ~Impl(){if(worker.joinable()){worker.request_stop();changed.notify_all();worker.join();}}
    void run(std::stop_token stop) noexcept {
        try {
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
            Counter counter;CpuCounter cpu;LUID selected{};
            std::unique_lock lock(mutex);
            while(!stop.stop_requested()) {
                const auto now=GetTickCount64();
                if(!requested_ms || now-requested_ms>=3000) {
                    counter.close();cpu.close();cached={};cpu_cached={};
                    changed.wait_for(lock,std::chrono::seconds(1));continue;
                }
                const auto wanted=adapter;
                const bool prime=!counter.query || !same(selected,wanted);
                selected=wanted;
                lock.unlock();
                GpuUsageStatus result{};
                if(prime)counter.open();
                else result=counter.collect(wanted);
                const auto cpu_result=cpu.collect();
                lock.lock();
                cpu_cached=cpu_result;
                if(same(adapter,wanted))cached=result;
                changed.wait_for(lock,std::chrono::seconds(1),[&]{return stop.stop_requested() || !same(adapter,selected);});
            }
        }catch(...){/* Missing/failed counters remain explicitly unavailable. */}
    }
};
GpuUsageSampler::GpuUsageSampler():impl_(std::make_unique<Impl>()){}
GpuUsageSampler::~GpuUsageSampler()=default;
GpuUsageStatus GpuUsageSampler::request(LUID adapter) noexcept {
    try {
        auto& s=*impl_;std::unique_lock lock(s.mutex,std::try_to_lock);
        if(!lock.owns_lock())return {};
        const auto now=GetTickCount64();
        const bool wake=!s.requested_ms || now-s.requested_ms>=3000 || !same(s.adapter,adapter);
        if(!same(s.adapter,adapter))s.cached={};
        s.adapter=adapter;s.requested_ms=now;
        if(!s.worker.joinable())s.worker=std::jthread([&s](std::stop_token stop){s.run(stop);});
        if(wake)s.changed.notify_all();
        if(!s.cached.valid || now<s.cached.updated_ms || now-s.cached.updated_ms>=3000)return {};
        return s.cached;
    }catch(...){return {};}
}
CpuUsageStatus GpuUsageSampler::cpu_usage() noexcept {
    try {
        auto& s=*impl_;std::unique_lock lock(s.mutex,std::try_to_lock);
        if(!lock.owns_lock())return {};
        const auto now=GetTickCount64();
        if(!s.requested_ms || now<s.requested_ms || now-s.requested_ms>=3000 ||
           !s.cpu_cached.valid || now<s.cpu_cached.updated_ms || now-s.cpu_cached.updated_ms>=3000)return {};
        return s.cpu_cached;
    }catch(...){return {};}
}

}
