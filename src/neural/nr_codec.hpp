#pragma once
#include "nr_host.hpp"
#include "DlssNr_Common.h"

// NR-only immutable shader bindings; source/constants and readback resources
// remain owned until the module's GPU work is quiescent. No rotating ring.
class DlssNr_Dx12 {
    struct DispatchBinding;
    std::vector<std::unique_ptr<DispatchBinding>> _bindings;
    std::mutex _bindingsMutex;
    Microsoft::WRL::ComPtr<ID3D12Device> _device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _pipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _guidePipelineState;
    bool _init=false;
    uint64_t _recording{};
public:
    DlssNr_Dx12(std::string name,ID3D12Device* device);
    ~DlssNr_Dx12();
    bool IsInit()const noexcept{return _init;}
    // One codec is owned by one native handle. Call begin only after that
    // handle's exact previous GPU submission retired. End removes bindings
    // unused by this recording, retaining the immutable bindings it recorded.
    void begin_recording();
    void end_recording();
    bool Dispatch(ID3D12GraphicsCommandList* list,const DlssNrConstants& constants,
        ID3D12Resource* source,ID3D12Resource* model,ID3D12Resource* original,
        ID3D12Resource* motion,ID3D12Resource* previous_edit,ID3D12Resource* target,
        ID3D12Resource* keep);
};
