#include "xrfg/d3d12_rife.hpp"
#include "xrfg/rife_runtime_locator.hpp"
#include "rife_pack_shader.hpp"
#include "rife_vertex_shader.hpp"
#include "rife_unpack_shader.hpp"
#include <windows.h>
#include <filesystem>
#include <sstream>
#include <map>
#include <atomic>
#include <algorithm>
#include <vector>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <charconv>

namespace xrfg { namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT hr) { if(FAILED(hr)) throw hr; }
struct Handle {
    HANDLE value{};
    ~Handle(){ if(value && value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle()=default; Handle(const Handle&)=delete; Handle& operator=(const Handle&)=delete;
    void reset(HANDLE h=nullptr){if(value)CloseHandle(value);value=h;}
};
std::filesystem::path runtime_path() {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&runtime_path),&module);
    wchar_t file[32768]{}; if(!GetModuleFileNameW(module,file,32768))throw E_FAIL;
    const auto runtime=staged_rife_runtime(std::filesystem::path(file).parent_path());
    if(!runtime.empty())return runtime;
    throw HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}
class Worker {
    Handle process_,input_,output_,job_;
    std::mutex mutex_;
    std::string line_;
    std::atomic<bool> live_{};
    std::atomic<std::uint64_t> memory_until_{};
    std::mutex memory_mutex_;
    RifeMemoryStatus memory_{};
    static bool number(const std::string& text,const char* key,std::uint64_t& out) noexcept {
        auto at=text.find(key);if(at==std::string::npos)return false;
        at=text.find(':',at);if(at==std::string::npos)return false;
        ++at;while(at<text.size() && text[at]==' ')++at;
        const auto parsed=std::from_chars(text.data()+at,text.data()+text.size(),out);
        return parsed.ec==std::errc{};
    }
    std::string read(DWORD timeout) {
        const auto deadline=GetTickCount64()+timeout;
        for(;;){
            if(auto newline=line_.find('\n');newline!=std::string::npos){auto out=line_.substr(0,newline);line_.erase(0,newline+1);return out;}
            char data[4096];DWORD count{};Handle event;event.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if(!event.value)throw E_FAIL;
            OVERLAPPED operation{};operation.hEvent=event.value;
            if(!ReadFile(output_.value,data,sizeof(data),&count,&operation)){
                if(GetLastError()!=ERROR_IO_PENDING)throw E_FAIL;
                HANDLE waiters[]{event.value,process_.value};
                const auto now=GetTickCount64();
                const auto wait=WaitForMultipleObjects(2,waiters,FALSE,now>=deadline?0:static_cast<DWORD>(deadline-now));
                if(wait!=WAIT_OBJECT_0){
                    CancelIoEx(output_.value,&operation);
                    GetOverlappedResult(output_.value,&operation,&count,TRUE);
                    throw HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                }
                if(!GetOverlappedResult(output_.value,&operation,&count,FALSE))throw E_FAIL;
            }
            if(!count)throw E_FAIL;line_.append(data,count);if(line_.size()>65536)throw E_FAIL;
        }
    }
    void stop() noexcept {
        live_=false; input_.reset();
        if(process_.value && WaitForSingleObject(process_.value,500)!=WAIT_OBJECT_0){TerminateProcess(process_.value,1);WaitForSingleObject(process_.value,5000);}
        process_.reset();output_.reset();job_.reset();
    }
public:
    Worker(){
        auto root=runtime_path();
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        Handle child_input,child_output;
        HANDLE parent_write{};
        if(!CreatePipe(&child_input.value,&parent_write,&security,0))throw E_FAIL;
        input_.value=parent_write;
        // An overlapped reply pipe wakes on completion. Polling Sleep(1)
        // otherwise adds scheduler-dependent gaps to every inference pair.
        static std::atomic_uint64_t pipe_sequence{};
        const std::wstring pipe_name=L"\\\\.\\pipe\\OFXR-RIFE-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(++pipe_sequence);
        output_.value=CreateNamedPipeW(pipe_name.c_str(),PIPE_ACCESS_INBOUND|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,nullptr);
        if(output_.value==INVALID_HANDLE_VALUE)throw E_FAIL;
        child_output.value=CreateFileW(pipe_name.c_str(),GENERIC_WRITE,0,&security,OPEN_EXISTING,0,nullptr);
        if(child_output.value==INVALID_HANDLE_VALUE)throw E_FAIL;
        Handle connected;connected.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);OVERLAPPED connection{};connection.hEvent=connected.value;
        if(!ConnectNamedPipe(output_.value,&connection)){
            const auto error=GetLastError();
            if(error==ERROR_IO_PENDING){
                DWORD transferred{};
                if(WaitForSingleObject(connected.value,5000)!=WAIT_OBJECT_0){
                    CancelIoEx(output_.value,&connection);GetOverlappedResult(output_.value,&connection,&transferred,TRUE);throw E_FAIL;
                }
                if(!GetOverlappedResult(output_.value,&connection,&transferred,FALSE))throw E_FAIL;
            }else if(error!=ERROR_PIPE_CONNECTED)throw E_FAIL;
        }
        SetHandleInformation(input_.value,HANDLE_FLAG_INHERIT,0);
        SetHandleInformation(output_.value,HANDLE_FLAG_INHERIT,0);
        Handle error;
        error.value=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput=child_input.value;startup.StartupInfo.hStdOutput=child_output.value;startup.StartupInfo.hStdError=error.value;
        SIZE_T bytes{};InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<std::byte> attributes(bytes);
        startup.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if(!InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&bytes))throw E_FAIL;
        HANDLE inherited[]{child_input.value,child_output.value,error.value};
        if(!UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)){
            DeleteProcThreadAttributeList(startup.lpAttributeList);throw E_FAIL;
        }
        wchar_t system[32768]{};GetWindowsDirectoryW(system,32768);
        // No ambient Python, Conda, CUDA or game-local DLL search paths.
        std::vector<std::wstring> vars{L"SystemRoot="+std::wstring(system),L"WINDIR="+std::wstring(system),
            L"PATH="+(root/L"python").wstring()+L";"+(root/L"python/Lib/site-packages/torch/lib").wstring()+L";"+std::wstring(system)+L"\\System32"};
        wchar_t temp[32768]{};if(GetTempPathW(32768,temp)){vars.push_back(L"TEMP="+std::wstring(temp));vars.push_back(L"TMP="+std::wstring(temp));}
        std::sort(vars.begin(),vars.end());std::wstring environment;for(auto& var:vars){environment+=var;environment+=L'\0';}environment+=L'\0';
        auto python=root/L"python/python.exe";
        std::wstring command=L"\""+python.wstring()+L"\" -I -u \""+(root/L"app/worker.py").wstring()+L"\"";
        PROCESS_INFORMATION info{};
        auto created=CreateProcessW(python.c_str(),command.data(),nullptr,nullptr,TRUE,
            CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT|CREATE_SUSPENDED,
            environment.data(),root.c_str(),&startup.StartupInfo,&info);
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if(!created)throw HRESULT_FROM_WIN32(GetLastError());
        process_.value=info.hProcess;Handle thread;thread.value=info.hThread;
        job_.value=CreateJobObjectW(nullptr,nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!job_.value || !SetInformationJobObject(job_.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)) || !AssignProcessToJobObject(job_.value,process_.value)){
            TerminateProcess(process_.value,1);throw E_FAIL;
        }
        ResumeThread(thread.value);child_input.reset();child_output.reset();
        try{if(read(60000).find("\"protocol\": 2")==std::string::npos)throw E_FAIL;live_=true;}catch(...){stop();throw;}
    }
    ~Worker(){stop();}
    bool alive() const noexcept {return live_.load(std::memory_order_acquire);}
    RifeMemoryStatus memory(bool request) noexcept {
        const auto now=GetTickCount64();
        if(request)memory_until_.store(now+2000,std::memory_order_relaxed);
        std::unique_lock lock(memory_mutex_,std::try_to_lock);
        RifeMemoryStatus result;
        if(lock.owns_lock())result=memory_;
        result.active=alive();
        result.valid=result.active && result.valid && now-result.updated_ms<5000;
        return result;
    }
    HANDLE process()const{return process_.value;}
    HRESULT request(const std::string& message) noexcept {
        std::scoped_lock lock(mutex_);
        if(!live_)return E_FAIL;
        try{
            auto text=message;
            if(GetTickCount64()<memory_until_.load(std::memory_order_relaxed) && text.find("\"op\":\"infer\"")!=std::string::npos){
                std::scoped_lock sample(memory_mutex_);
                if(GetTickCount64()-memory_.updated_ms>=1000){text.pop_back();text+=",\"memory\":true}";}
            }
            text+="\n";DWORD written{};
            if(!WriteFile(input_.value,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)||written!=text.size())throw E_FAIL;
            const auto reply=read(30000); // first model graph warm-up may take seconds
            if(reply.find("\"ok\": true")==std::string::npos)throw E_FAIL;
            std::uint64_t usage{},reserved{};
            if(number(reply,"\"vram_private\"",usage) && number(reply,"\"torch_reserved\"",reserved)){
                std::scoped_lock sample(memory_mutex_);
                memory_={GetTickCount64(),usage,reserved,GetProcessId(process_.value),true,true};
            }
            return S_OK;
        }catch(HRESULT hr){stop();return hr;}catch(...){stop();return E_FAIL;}
    }
};
std::mutex registry_mutex;
std::map<std::uint64_t,std::weak_ptr<Worker>> workers;
std::shared_ptr<Worker> worker_for(std::uint64_t luid){
    std::scoped_lock lock(registry_mutex);auto worker=workers[luid].lock();
    if(!worker || !worker->alive()){worker=std::make_shared<Worker>();workers[luid]=worker;}return worker;
}
std::atomic_uint64_t identifiers{1};
D3D12_RESOURCE_BARRIER barrier(ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};return b;
}
struct Params{
    std::array<float,4> rotation,source_fov,target_fov,rect;
    UINT width,height,slice,offset,texture_width,texture_height,proxy,reserved;
    std::array<float,4> rotation_a,fov_a;
};
static_assert(sizeof(Params)==128);
std::array<float,4> tangents(const D3D12FieldOfView& f){return {std::tan(f.angle_left),std::tan(f.angle_right),std::tan(f.angle_up),std::tan(f.angle_down)};}
D3D12ImageRect rect_of(const D3D12ReprojectionView& view,const D3D12_RESOURCE_DESC& d){
    return view.image_rect.width&&view.image_rect.height?view.image_rect:D3D12ImageRect{0,0,static_cast<UINT>(d.Width),d.Height};
}
}
RifeMemoryStatus rife_memory_for_adapter(LUID adapter,bool request) noexcept {
    try{
        std::unique_lock lock(registry_mutex,std::try_to_lock);
        if(!lock.owns_lock())return {0,0,0,0,true,false};
        std::uint64_t key{};std::memcpy(&key,&adapter,sizeof(key));
        const auto found=workers.find(key);if(found==workers.end())return {};
        auto worker=found->second.lock();lock.unlock();
        return worker?worker->memory(request):RifeMemoryStatus{};
    }catch(...){return {};}
}
struct D3D12Rife::Impl{
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pack,unpack;
    ComPtr<ID3D12DescriptorHeap> heap,rtvs;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;Handle event;
    std::array<ComPtr<ID3D12Resource>,3> buffers;
    std::array<ComPtr<ID3D12Resource>,3> inflight_images;
    std::shared_ptr<Worker> worker;
    DXGI_FORMAT format{};int model{};float scale{};UINT width{},height{},eyes{},proxy{};
    std::uint64_t id{identifiers.fetch_add(1)},luid{},value{},submitted{};
    bool configured{};
    ~Impl(){
        bool safe=true;
        if(submitted && fence && fence->GetCompletedValue()<submitted){
            fence->SetEventOnCompletion(submitted,event.value);
            safe=WaitForSingleObject(event.value,30000)==WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason());
        }
        if(configured && worker)worker->request("{\"op\":\"release\",\"id\":"+std::to_string(id)+"}");
        // An unresponsive live GPU must retain objects referenced by its list.
        if(!safe){for(auto& b:buffers)b.Detach();for(auto& image:inflight_images)image.Detach();heap.Detach();rtvs.Detach();allocator.Detach();commands.Detach();root.Detach();pack.Detach();unpack.Detach();fence.Detach();}
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT i){auto h=heap->GetCPUDescriptorHandleForHeapStart();h.ptr+=i*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);return h;}
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT i){auto h=heap->GetGPUDescriptorHandleForHeapStart();h.ptr+=i*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);return h;}
    void begin(){check(allocator->Reset());check(commands->Reset(allocator.Get(),nullptr));}
    void execute(){
        check(commands->Close());ID3D12CommandList* list=commands.Get();queue->ExecuteCommandLists(1,&list);
        submitted=++value;check(queue->Signal(fence.Get(),submitted));
        if(fence->GetCompletedValue()<submitted){check(fence->SetEventOnCompletion(submitted,event.value));
            if(WaitForSingleObject(event.value,30000)!=WAIT_OBJECT_0)throw HRESULT_FROM_WIN32(ERROR_TIMEOUT);}
        check(device->GetDeviceRemovedReason());
    }
    void size(UINT w,UINT h,UINT count){
        if(w==width&&h==height&&count==eyes)return;
        if(configured){check(worker->request("{\"op\":\"release\",\"id\":"+std::to_string(id)+"}"));configured=false;}
        for(auto& b:buffers)b.Reset();width=w;height=h;eyes=count;
        auto bytes=static_cast<UINT64>(w)*h*count*5*2;
        if(bytes>UINT_MAX*2ULL)throw E_INVALIDARG;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;
        desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;
        desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES properties{};properties.Type=D3D12_HEAP_TYPE_DEFAULT;
        std::ostringstream message;message<<"{\"op\":\"configure\",\"id\":"<<id<<",\"luid\":"<<luid<<",\"width\":"<<w<<",\"height\":"<<h<<",\"eyes\":"<<count<<",\"buffers\":[";
        // NT handles are duplicated only into our child, never inherited by
        // the game or exposed by name. CUDA closes them after importing.
        std::array<HANDLE,3> duplicate{};
        try{for(UINT i=0;i<3;++i){
            const auto elements=static_cast<UINT64>(w)*h*count*(i==2?5:3);
            desc.Width=elements*2;
            check(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_SHARED,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&buffers[i])));
            Handle shared;check(device->CreateSharedHandle(buffers[i].Get(),nullptr,GENERIC_ALL,nullptr,&shared.value));
            if(!DuplicateHandle(GetCurrentProcess(),shared.value,worker->process(),&duplicate[i],0,FALSE,DUPLICATE_SAME_ACCESS))throw E_FAIL;
            auto allocation=device->GetResourceAllocationInfo(0,1,&desc);
            if(i)message<<',';message<<'['<<reinterpret_cast<std::uintptr_t>(duplicate[i])<<','<<allocation.SizeInBytes<<']';
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R16_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uav.Buffer.NumElements=static_cast<UINT>(elements);
            for(UINT base:{0U,6U,12U})device->CreateUnorderedAccessView(buffers[i].Get(),nullptr,&uav,cpu(base+3+i));
        }}catch(...){
            for(auto hnd:duplicate)if(hnd){HANDLE local{};if(DuplicateHandle(worker->process(),hnd,GetCurrentProcess(),&local,0,FALSE,DUPLICATE_CLOSE_SOURCE))CloseHandle(local);}throw;
        }
        message<<"]}";check(worker->request(message.str()));configured=true;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format=DXGI_FORMAT_R16_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;srv.Buffer.NumElements=w*h*count*5;
        for(UINT base:{0U,6U,12U})device->CreateShaderResourceView(buffers[2].Get(),&srv,cpu(base+2));
    }
    Params params(const D3D12ReprojectionView& view,const D3D12ReprojectionView& target,const D3D12_RESOURCE_DESC& desc,const std::array<float,4>& rotation,UINT eye){
        auto rect=rect_of(view,desc);return {rotation,tangents(view.fov),tangents(target.fov),
            {float(rect.offset_x),float(rect.offset_y),float(rect.width),float(rect.height)},width,height,
            view.array_slice==UINT_MAX?eye:view.array_slice,eye*width*height*3,static_cast<UINT>(desc.Width),desc.Height,proxy,0};
    }
};
D3D12Rife::D3D12Rife()=default;D3D12Rife::~D3D12Rife()=default;
HRESULT D3D12Rife::initialize(ID3D12Device* device,ID3D12CommandQueue* queue,DXGI_FORMAT format,int model,float scale)noexcept{
    try{
        if(!device||!queue||model<0||model>4||scale<=0||scale>1)return E_INVALIDARG;
        auto p=std::make_unique<Impl>();p->device=device;p->queue=queue;p->format=format;p->model=model;p->scale=scale;
        auto luid=device->GetAdapterLuid();std::memcpy(&p->luid,&luid,sizeof(luid));p->worker=worker_for(p->luid);
        p->proxy=format==DXGI_FORMAT_R16G16B16A16_FLOAT||format==DXGI_FORMAT_R32G32B32A32_FLOAT?1:
            format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB?2:0;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,0,0,0};ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,3,0,0,3};
        D3D12_ROOT_PARAMETER parameters[2]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants={0,0,32};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[1].DescriptorTable={2,ranges};
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC signature{2,parameters,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
        check(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
        check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&p->root)));
        D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};compute.pRootSignature=p->root.Get();compute.CS={g_xrfg_rife_pack_shader,sizeof(g_xrfg_rife_pack_shader)};
        check(device->CreateComputePipelineState(&compute,IID_PPV_ARGS(&p->pack)));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};graphics.pRootSignature=p->root.Get();
        graphics.VS={g_xrfg_rife_vertex_shader,sizeof(g_xrfg_rife_vertex_shader)};graphics.PS={g_xrfg_rife_unpack_shader,sizeof(g_xrfg_rife_unpack_shader)};
        graphics.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        graphics.SampleMask=UINT_MAX;graphics.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;graphics.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;graphics.RasterizerState.DepthClipEnable=TRUE;
        graphics.DepthStencilState.DepthEnable=FALSE;graphics.DepthStencilState.StencilEnable=FALSE;graphics.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        graphics.NumRenderTargets=1;graphics.RTVFormats[0]=format;graphics.SampleDesc.Count=1;
        check(device->CreateGraphicsPipelineState(&graphics,IID_PPV_ARGS(&p->unpack)));
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=18;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&p->heap)));
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=2;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&p->rtvs)));
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&p->allocator)));
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,p->allocator.Get(),nullptr,IID_PPV_ARGS(&p->commands)));check(p->commands->Close());
        check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&p->fence)));p->event.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!p->event.value)throw E_FAIL;
        impl_=std::move(p);return S_OK;
    }catch(HRESULT hr){return hr;}catch(...){return E_FAIL;}
}
HRESULT D3D12Rife::generate(ID3D12Resource* a,ID3D12Resource* b,ID3D12Resource* out,
    std::span<const D3D12ReprojectionView> av,std::span<const D3D12ReprojectionView> bv,std::span<const D3D12ReprojectionView> tv,
    std::span<const std::array<float,4>> ar,std::span<const std::array<float,4>> br,D3D12_RESOURCE_STATES output_state,float fraction,Timing timing)noexcept{
    try{
        if(!impl_||!a||!b||!out||av.empty()||av.size()>2||av.size()!=bv.size()||av.size()!=tv.size()||ar.size()!=tv.size()||br.size()!=tv.size())return E_INVALIDARG;
        auto& p=*impl_;const auto desc=b->GetDesc();const auto roi=rect_of(bv[0],desc);
        p.inflight_images={a,b,out};
        const UINT w=std::max(32U,static_cast<UINT>(std::ceil(roi.width*p.scale))),h=std::max(32U,static_cast<UINT>(std::ceil(roi.height*p.scale)));
        p.size(w,h,static_cast<UINT>(av.size()));
        const auto mark=[&](UINT query){if(timing.queries)p.commands->EndQuery(timing.queries,D3D12_QUERY_TYPE_TIMESTAMP,timing.base+query);};
        p.begin();if(timing.first){mark(0);mark(5);}
        // The validated target layout is disjoint. A complete union needs no
        // preliminary B copy; cropped/packed subrects preserve B outside it.
        std::uint64_t covered=0;
        for(const auto& view:tv){const auto rect=rect_of(view,desc);covered+=static_cast<std::uint64_t>(rect.width)*rect.height;}
        if(covered!=desc.Width*desc.Height*desc.DepthOrArraySize){
            auto bs=barrier(b,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
            auto os=barrier(out,output_state,D3D12_RESOURCE_STATE_COPY_DEST);
            p.commands->ResourceBarrier(1,&bs);p.commands->ResourceBarrier(1,&os);p.commands->CopyResource(out,b);
            bs=barrier(b,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);os=barrier(out,D3D12_RESOURCE_STATE_COPY_DEST,output_state);
            p.commands->ResourceBarrier(1,&bs);p.commands->ResourceBarrier(1,&os);
        }
        p.commands->SetPipelineState(p.pack.Get());p.commands->SetComputeRootSignature(p.root.Get());
        ID3D12DescriptorHeap* pack_heaps[]{p.heap.Get()};p.commands->SetDescriptorHeaps(1,pack_heaps);
        for(unsigned source_index=0;source_index<2;++source_index){
            const UINT base=source_index*6;
            auto resource=source_index?b:a;const auto d=resource->GetDesc();auto views=source_index?bv:av;auto rotations=source_index?br:ar;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=p.format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2DArray.MipLevels=1;srv.Texture2DArray.ArraySize=d.DepthOrArraySize;
            p.device->CreateShaderResourceView(resource,&srv,p.cpu(base));
            p.device->CreateShaderResourceView(resource,&srv,p.cpu(base+1));
            auto before=barrier(resource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);p.commands->ResourceBarrier(1,&before);
            auto input=barrier(p.buffers[source_index].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);p.commands->ResourceBarrier(1,&input);
            // Each immutable table survives the whole combined A/B pack.
            D3D12_UNORDERED_ACCESS_VIEW_DESC selected{};selected.Format=DXGI_FORMAT_R16_FLOAT;
            selected.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;selected.Buffer.NumElements=w*h*p.eyes*3;
            p.device->CreateUnorderedAccessView(p.buffers[source_index].Get(),nullptr,&selected,p.cpu(base+3));
            p.commands->SetComputeRootDescriptorTable(1,p.gpu(base));
            for(UINT eye=0;eye<p.eyes;++eye){auto constants=p.params(views[eye],tv[eye],d,rotations[eye],eye);p.commands->SetComputeRoot32BitConstants(0,32,&constants,0);p.commands->Dispatch((w+7)/8,(h+7)/8,1);}
            before=barrier(resource,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);input=barrier(p.buffers[source_index].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
            p.commands->ResourceBarrier(1,&before);p.commands->ResourceBarrier(1,&input);
        }
        if(timing.first)mark(1);p.execute();
        check(p.worker->request("{\"op\":\"infer\",\"id\":"+std::to_string(p.id)+",\"model\":"+std::to_string(p.model)+",\"fraction\":"+std::to_string(fraction)+"}"));
        // Restore full-resolution A/B descriptors after both reduced-input
        // packs have retired. Only flow/mask planes return from Python.
        for(UINT source_index=0;source_index<2;++source_index){
            auto* resource=source_index?b:a;const auto description=resource->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=p.format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2DArray.MipLevels=1;srv.Texture2DArray.ArraySize=description.DepthOrArraySize;
            p.device->CreateShaderResourceView(resource,&srv,p.cpu(12+source_index));
        }
        p.begin();if(timing.last){mark(2);mark(3);}
        auto output=barrier(out,output_state,D3D12_RESOURCE_STATE_RENDER_TARGET);auto result=barrier(p.buffers[2].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        auto alpha=barrier(b,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);p.commands->ResourceBarrier(1,&alpha);
        auto peer=barrier(a,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);p.commands->ResourceBarrier(1,&peer);
        if(output_state!=D3D12_RESOURCE_STATE_RENDER_TARGET)p.commands->ResourceBarrier(1,&output);
        p.commands->ResourceBarrier(1,&result);
        p.commands->SetPipelineState(p.unpack.Get());p.commands->SetGraphicsRootSignature(p.root.Get());ID3D12DescriptorHeap* heaps[]{p.heap.Get()};p.commands->SetDescriptorHeaps(1,heaps);
        p.commands->SetGraphicsRootDescriptorTable(1,p.gpu(12));p.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for(UINT eye=0;eye<p.eyes;++eye){
            const auto r=rect_of(tv[eye],desc);const auto slice=tv[eye].array_slice==UINT_MAX?eye:tv[eye].array_slice;
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};rtv.Format=p.format;rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;rtv.Texture2DArray.FirstArraySlice=slice;rtv.Texture2DArray.ArraySize=1;
            auto handle=p.rtvs->GetCPUDescriptorHandleForHeapStart();handle.ptr+=eye*p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            p.device->CreateRenderTargetView(out,&rtv,handle);p.commands->OMSetRenderTargets(1,&handle,FALSE,nullptr);
            D3D12_VIEWPORT viewport{float(r.offset_x),float(r.offset_y),float(r.width),float(r.height),0,1};D3D12_RECT scissor{LONG(r.offset_x),LONG(r.offset_y),LONG(r.offset_x+r.width),LONG(r.offset_y+r.height)};
            p.commands->RSSetViewports(1,&viewport);p.commands->RSSetScissorRects(1,&scissor);
            auto constants=p.params(bv[eye],tv[eye],desc,br[eye],eye);
            constants.offset=eye*w*h*5;constants.rotation_a=ar[eye];constants.fov_a=tangents(av[eye].fov);constants.reserved=av[eye].array_slice==UINT_MAX?eye:av[eye].array_slice;
            p.commands->SetGraphicsRoot32BitConstants(0,32,&constants,0);p.commands->DrawInstanced(3,1,0,0);
        }
        output=barrier(out,D3D12_RESOURCE_STATE_RENDER_TARGET,output_state);result=barrier(p.buffers[2].Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        alpha=barrier(b,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);p.commands->ResourceBarrier(1,&alpha);
        peer=barrier(a,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);p.commands->ResourceBarrier(1,&peer);
        if(output_state!=D3D12_RESOURCE_STATE_RENDER_TARGET)p.commands->ResourceBarrier(1,&output);
        p.commands->ResourceBarrier(1,&result);if(timing.last){mark(4);mark(6);}
        if(timing.last&&timing.queries&&timing.readback)p.commands->ResolveQueryData(timing.queries,D3D12_QUERY_TYPE_TIMESTAMP,timing.base,7,timing.readback,timing.base*sizeof(std::uint64_t));
        p.execute();p.inflight_images={};return S_OK;
    }catch(HRESULT hr){return hr;}catch(...){return E_FAIL;}
}
}
