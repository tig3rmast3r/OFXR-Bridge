#include "xrfg/d3d12_native_binding.hpp"
// Query-only COM adapters: production normalization only invokes IUnknown.
// No graphics command is ever recorded through these fixture interfaces.
namespace nr_test {
struct ListIdentityProxy final : IUnknown {
    IUnknown* target{};
    GUID contract{};
    ULONG refs{1};
    HRESULT result{S_OK};
    bool null_success{};
    ListIdentityProxy(IUnknown* object,GUID id):target(object),contract(id) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(id==xrfg::d3d_native::streamline_native_guid || id==xrfg::d3d_native::reshade_unwrapped_guid) {
            if(id!=contract)return E_NOINTERFACE;
            if(FAILED(result))return result;
            if(!null_success){*out=target;target->AddRef();}return S_OK;
        }
        if(id==__uuidof(IUnknown)||id==__uuidof(ID3D12CommandList)||id==__uuidof(ID3D12GraphicsCommandList)) {
            *out=this;AddRef();return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{return --refs;}
    ID3D12CommandList* query_list(){return reinterpret_cast<ID3D12CommandList*>(static_cast<IUnknown*>(this));}
};
void reshade_submission_run(Gpu& gpu) {
    using namespace xrfg::d3d_native;
    using DlssNr::NrNativeCommandList;
    ListIdentityProxy reshade(gpu.list.Get(),reshade_unwrapped_guid);
    ListIdentityProxy streamline(&reshade,streamline_native_guid);
    ListIdentityProxy reverse_inner(gpu.list.Get(),streamline_native_guid);
    ListIdentityProxy reverse_outer(&reverse_inner,reshade_unwrapped_guid);
    need(!NrNativeCommandList(nullptr),"null list rejected");
    {
        auto direct=NrNativeCommandList(gpu.list.Get());
        auto wrapped=NrNativeCommandList(reshade.query_list());
        auto nested=NrNativeCommandList(streamline.query_list());
        auto reverse=NrNativeCommandList(reverse_outer.query_list());
        need(direct.Get()==gpu.list.Get()&&wrapped.Get()==direct.Get()&&
            nested.Get()==direct.Get()&&reverse.Get()==direct.Get(),
            "NR recording must match native queue identity through ReShade and nested Streamline");
    }
    for(unsigned cycle=0;cycle<20;++cycle) {
        if(cycle)gpu.reset();
        for(unsigned handle=1;handle<=4;++handle) {
            auto* source=handle==1?reshade.query_list():handle==2?streamline.query_list():handle==3?reverse_outer.query_list():static_cast<ID3D12CommandList*>(gpu.list.Get());
            auto identity=NrNativeCommandList(source);
            MarkNrRecorded(handle,identity.Get());
            need(!RetirePreviousNrEvaluate(handle),"unsubmitted scratch cannot be reused");
        }
        ID3D12CommandList* aliases[]{gpu.list.Get(),reshade.query_list(),streamline.query_list()};
        auto receipts=xrfg::neural_rendering::take_submission_receipts(3,aliases);
        need(receipts.size()==4,"four foveated NR histories detach once despite proxy aliases");
        need(xrfg::neural_rendering::take_submission_receipts(3,aliases).empty(),"submission receipts cannot be claimed twice");
        hr(gpu.list->Close());ID3D12CommandList* actual[]{gpu.list.Get()};gpu.queue->ExecuteCommandLists(1,actual);
        xrfg::neural_rendering::complete_submission(gpu.queue.Get(),receipts);
        hr(gpu.queue->Signal(gpu.done.Get(),++gpu.serial));gpu.finish();
        for(unsigned handle=1;handle<=4;++handle)need(RetirePreviousNrEvaluate(handle),
            "actual native submission retires every NR history instead of permanent pending");
        need(reshade.refs==1&&streamline.refs==1&&reverse_inner.refs==1&&reverse_outer.refs==1,
            "repeated unwrap retains no proxy references");
    }
    reshade.result=E_FAIL;need(!NrNativeCommandList(streamline.query_list()),"inner failure must not fall back to wrapper");
    reshade.result=S_OK;reshade.null_success=true;need(!NrNativeCommandList(reshade.query_list()),"null success rejected");
    reshade.null_success=false;reshade.target=&streamline;
    need(!NrNativeCommandList(streamline.query_list()),"cyclic proxy chain rejected");reshade.target=gpu.list.Get();
    g_nrFlights.clear();g_nrSubmissionLedger.Clear();
    std::puts("NR ReShade submission: native/nested identities,4 histories x20 cycles,exact queue fences,no double claim,malformed chains PASS");
}
}
