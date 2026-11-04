#pragma once
#include "xrfg/bridge_flight_logger.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace xrfg {
// F9-only, one reusable probe per RIFE eye worker. No CPU completion wait.
// gpu_fence_snapshot results86720..25: a=source serial,b=GPU time on flight
// timeline in microseconds,c=probe identity. Markers: queue,A,B,acquire,pack0,pack1.
class RifeDependencyTiming {
    template<class T>using ComPtr=Microsoft::WRL::ComPtr<T>;
    ComPtr<ID3D12QueryHeap> queries_;
    ComPtr<ID3D12Resource> readback_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    std::array<ComPtr<ID3D12GraphicsCommandList>,5> lists_;
    std::uint64_t pending_{},serial_{};
    unsigned count_{};bool initialized_{},failed_{};
    HRESULT initialize(ID3D12Device* device) noexcept {
        D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=6;
        auto hr=device->CreateQueryHeap(&q,IID_PPV_ARGS(&queries_));if(FAILED(hr))return hr;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=48;
        desc.Height=1;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback_));if(FAILED(hr))return hr;
        hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator_));if(FAILED(hr))return hr;
        for(auto& list:lists_){hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator_.Get(),nullptr,IID_PPV_ARGS(&list));if(FAILED(hr))return hr;
            hr=list->Close();if(FAILED(hr))return hr;}
        initialized_=true;return S_OK;
    }
    void error(HRESULT hr,std::uint64_t serial) noexcept {
        failed_=true;bridge_flight_logger().event(BridgeFlightOperation::gpu_fence_snapshot,86799,serial,static_cast<std::uint32_t>(hr),identity());
    }
public:
    std::uint64_t identity()const noexcept{return reinterpret_cast<std::uintptr_t>(this);}
    ID3D12QueryHeap* queries()const noexcept{return queries_.Get();}
    bool allocated()const noexcept{return initialized_;}
    bool pending()const noexcept{return pending_!=0;}
    void poll(ID3D12Fence* completion,ID3D12CommandQueue* queue) noexcept {
        if(!pending_ || !completion)return;
        const auto completed=completion->GetCompletedValue();if(completed!=UINT64_MAX && completed<pending_)return;
        pending_=0;if(completed==UINT64_MAX || !count_ || !bridge_flight_logger().enabled())return;
        std::uint64_t* data{};D3D12_RANGE range{0,count_*sizeof(std::uint64_t)};
        const auto hr=readback_->Map(0,&range,reinterpret_cast<void**>(&data));if(FAILED(hr)){error(hr,serial_);return;}
        std::array<std::uint64_t,6> ticks{};for(unsigned i=0;i<count_;++i)ticks[i]=data[i];
        D3D12_RANGE written{};readback_->Unmap(0,&written);
        auto& log=bridge_flight_logger();UINT64 frequency{},gpu{},cpu{};LARGE_INTEGER qpc{};
        const bool calibrated=SUCCEEDED(queue->GetTimestampFrequency(&frequency)) && frequency &&
            SUCCEEDED(queue->GetClockCalibration(&gpu,&cpu)) && QueryPerformanceFrequency(&qpc);
        log.event(BridgeFlightOperation::gpu_fence_snapshot,86701,serial_,frequency,identity());
        for(unsigned i=0;i<count_;++i){
            if(calibrated){const auto counter=static_cast<std::int64_t>(static_cast<long double>(cpu)+
                (static_cast<long double>(ticks[i])-gpu)*qpc.QuadPart/frequency);
                log.event(BridgeFlightOperation::gpu_fence_snapshot,86720+i,serial_,log.microseconds_for_counter(counter),identity());}
            else log.event(BridgeFlightOperation::gpu_fence_snapshot,86730+i,serial_,ticks[i],identity());
        }
    }
    bool begin(ID3D12Device* device,bool enabled,std::uint64_t serial,std::uint64_t value) noexcept {
        if(!enabled || pending_ || failed_)return false;
        auto hr=initialized_?S_OK:initialize(device);if(SUCCEEDED(hr))hr=allocator_->Reset();
        for(unsigned i=0;i<4 && SUCCEEDED(hr);++i){hr=lists_[i]->Reset(allocator_.Get(),nullptr);
            if(SUCCEEDED(hr)){lists_[i]->EndQuery(queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,i);hr=lists_[i]->Close();}}
        if(FAILED(hr)){error(hr,serial);return false;}
        serial_=serial;count_=0;
        bridge_flight_logger().event(BridgeFlightOperation::gpu_fence_snapshot,86700,serial,value,identity());return true;
    }
    void marker(ID3D12CommandQueue* queue,unsigned index) noexcept {
        ID3D12CommandList* list=lists_[index].Get();queue->ExecuteCommandLists(1,&list);
    }
    void finish(ID3D12CommandQueue* queue,std::uint64_t value,bool packed) noexcept {
        // Mark pending even on diagnostic failure: never reset objects that the
        // GPU may still reference. The ordinary job completion fence retires them.
        pending_=value;count_=packed?6:4;
        auto hr=lists_[4]->Reset(allocator_.Get(),nullptr);
        if(SUCCEEDED(hr)){lists_[4]->ResolveQueryData(queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,count_,readback_.Get(),0);hr=lists_[4]->Close();}
        if(FAILED(hr)){count_=0;error(hr,serial_);return;}
        ID3D12CommandList* list=lists_[4].Get();queue->ExecuteCommandLists(1,&list);
    }
};
}
