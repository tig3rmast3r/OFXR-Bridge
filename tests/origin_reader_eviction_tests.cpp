// Exercise the actual cache and actual D3D12 fences without a game/headset.
#include "../src/d3d12/luke_color_origin.cpp"
#include <iostream>
#include <algorithm>
#include <stdexcept>

static void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
int main() {
    try {
        using Microsoft::WRL::ComPtr;
        ComPtr<ID3D12Device> device;
        require(SUCCEEDED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"D3D12 device");
        std::array<ComPtr<ID3D12CommandQueue>,12> queues;
        std::array<std::shared_ptr<xrfg::Reader>,8> held;
        D3D12_COMMAND_QUEUE_DESC desc{};
        for(auto& queue:queues)require(SUCCEEDED(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue))),"queue");
        for(unsigned i=0;i<8;++i)require(SUCCEEDED(xrfg::get_reader(queues[i].Get(),held[i])),"initial reader");
        std::shared_ptr<xrfg::Reader> next;
        require(xrfg::get_reader(queues[8].Get(),next)==HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY),"held readers must not retire");
        auto fence=held[0]->fence;held[0]->last_fence=1;held[0].reset();
        require(xrfg::get_reader(queues[8].Get(),next)==HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY),"pending GPU receipt must not retire");
        require(SUCCEEDED(fence->Signal(1)),"complete actual fence");
        require(SUCCEEDED(xrfg::get_reader(queues[8].Get(),next)),"completed unused reader must retire");
        require(xrfg::cache().readers.size()==8,"cache must remain bounded");
        require(std::none_of(xrfg::cache().readers.begin(),xrfg::cache().readers.end(),[&](const auto& reader){return reader->queue.Get()==queues[0].Get();}),"retired queue still cached");
        for(unsigned i=9;i<12;++i) {
            next.reset();require(SUCCEEDED(xrfg::get_reader(queues[i].Get(),next)),"repeated session changes exhaust cache");
        }
        std::cout<<"Actual reader cache: held/pending protection, completed eviction and repeated session changes PASS\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
