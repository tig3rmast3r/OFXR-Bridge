#include "xrfg/d3d12_rife.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/rife_runtime_locator.hpp"
#include "xrfg/rife_runtime_lease.hpp"
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
#include <chrono>
#include <condition_variable>
#include <thread>

namespace xrfg { namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT hr) { if(FAILED(hr)) throw hr; }
template<class F>
void rife_phase(BridgeFlightLogger* logger,BridgeFlightOperation operation,
    std::uint64_t id,std::uint64_t dimensions,std::uint64_t fraction_eyes,F&& run){
    if(!logger){run();return;}
    const auto token=logger->begin(operation,id,dimensions,fraction_eyes);
    try{run();logger->end(token,operation,S_OK,id,dimensions,fraction_eyes);}
    catch(HRESULT hr){logger->end(token,operation,hr,id,dimensions,fraction_eyes);throw;}
    catch(...){logger->end(token,operation,E_FAIL,id,dimensions,fraction_eyes);throw;}
}
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
    RifeRuntimeReadLease runtime_lease_;
    Handle process_,input_,output_,job_;
    std::mutex mutex_;
    std::string line_;
    std::atomic<bool> live_{};
    bool fraction_batch_{};
    bool vulkan_{};
    bool memory_after_inference_{}; // protected by the transaction mutex
    std::atomic<bool> optimizing_{}, tensorrt_{};
    std::atomic<unsigned> optimization_error_{};
    std::atomic<std::uint64_t> execution_updated_ms_{};
    std::atomic<std::uint64_t> memory_until_{};
    std::mutex memory_mutex_;
    RifeMemoryStatus memory_{};
    std::mutex memory_wakeup_mutex_;
    std::condition_variable memory_wakeup_;
    std::jthread memory_sampler_;
    void sample_memory(std::stop_token stop) noexcept {
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
        std::unique_lock wake(memory_wakeup_mutex_);
        while(!stop.stop_requested()) {
            memory_wakeup_.wait_for(wake,std::chrono::seconds(1),[&]{return stop.stop_requested() || GetTickCount64()<memory_until_.load(std::memory_order_relaxed);});
            if(stop.stop_requested())break;
            if(GetTickCount64()>=memory_until_.load(std::memory_order_relaxed))continue;
            wake.unlock();
            // Never wait behind an inference or send concurrent pipe messages.
            // Active inference can still piggyback the existing memory receipt.
            {
                std::unique_lock transaction(mutex_,std::try_to_lock);
                if(transaction.owns_lock() && alive()) {
                    bool stale{};
                    {std::scoped_lock sample(memory_mutex_);stale=GetTickCount64()-memory_.updated_ms>=1000;}
                    if(stale)request_locked("{\"op\":\"memory\"}");
                }
            }
            wake.lock();
            memory_wakeup_.wait_for(wake,std::chrono::seconds(1),[&]{return stop.stop_requested();});
        }
    }
    static bool number(const std::string& text,const char* key,std::uint64_t& out) noexcept {
        auto at=text.find(key);if(at==std::string::npos)return false;
        at=text.find(':',at);if(at==std::string::npos)return false;
        ++at;while(at<text.size() && text[at]==' ')++at;
        const auto parsed=std::from_chars(text.data()+at,text.data()+text.size(),out);
        return parsed.ec==std::errc{};
    }
    static bool boolean(const std::string& text,const char* key) noexcept {
        auto at=text.find(key);if(at==std::string::npos)return false;
        at=text.find(':',at);if(at==std::string::npos)return false;
        ++at;while(at<text.size() && (text[at]==' ' || text[at]=='\t'))++at;
        return text.compare(at,4,"true")==0;
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
        live_=false;optimizing_=false;tensorrt_=false;execution_updated_ms_=0;input_.reset();
        if(process_.value && WaitForSingleObject(process_.value,500)!=WAIT_OBJECT_0){TerminateProcess(process_.value,1);WaitForSingleObject(process_.value,5000);}
        process_.reset();output_.reset();job_.reset();
    }
public:
    Worker(){
        auto root=runtime_path();
        vulkan_=std::filesystem::is_regular_file(root/L"vulkan/backend.json");
        if(!runtime_lease_.acquire(root))throw HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
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
        // One per-user engine cache; keep Python/CUDA search paths scrubbed.
        wchar_t local_appdata[32768]{};
        if(GetEnvironmentVariableW(L"LOCALAPPDATA",local_appdata,32768))
            vars.push_back(L"LOCALAPPDATA="+std::wstring(local_appdata));
        std::sort(vars.begin(),vars.end());std::wstring environment;for(auto& var:vars){environment+=var;environment+=L'\0';}environment+=L'\0';
        auto python=root/(vulkan_?L"vulkan/ofxr_rife_vulkan.exe":L"python/python.exe");
        std::wstring command=vulkan_?L"\""+python.wstring()+L"\" --worker \""+root.wstring()+L"\"":
            L"\""+python.wstring()+L"\" -I -u \""+(root/L"app/worker.py").wstring()+L"\"";
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
        try{const auto hello=read(60000);std::uint64_t protocol{};if(!number(hello,"\"protocol\"",protocol)||protocol!=2||!boolean(hello,"\"ok\""))throw E_FAIL;fraction_batch_=boolean(hello,"\"fraction_batch\"");live_=true;
            memory_sampler_=std::jthread([this](std::stop_token token){sample_memory(token);});
        }catch(...){stop();throw;}
    }
    ~Worker(){
        if(memory_sampler_.joinable()){memory_sampler_.request_stop();memory_wakeup_.notify_all();memory_sampler_.join();}
        stop();
    }
    bool alive() const noexcept {return live_.load(std::memory_order_acquire);}
    bool fraction_batch() const noexcept {return fraction_batch_;}
    bool vulkan() const noexcept {return vulkan_;}
    RifeExecutionStatus execution() const noexcept {
        const auto updated=execution_updated_ms_.load(std::memory_order_acquire);
        const auto now=GetTickCount64();
        const bool active=alive() && updated && now>=updated && now-updated<5000;
        return {updated,active,active && optimizing_.load(std::memory_order_relaxed),
            active && tensorrt_.load(std::memory_order_relaxed),
            active?optimization_error_.load(std::memory_order_relaxed):0U,active&&vulkan_};
    }
    RifeMemoryStatus memory(bool request) noexcept {
        const auto now=GetTickCount64();
        if(request){memory_until_.store(now+2000,std::memory_order_relaxed);memory_wakeup_.notify_one();}
        std::unique_lock lock(memory_mutex_,std::try_to_lock);
        RifeMemoryStatus result;
        if(lock.owns_lock())result=memory_;
        result.active=alive();
        result.valid=result.active && result.valid && now-result.updated_ms<5000;
        return result;
    }
    HANDLE process()const{return process_.value;}
    HRESULT request(const std::string& message,std::int32_t* backend_flags=nullptr) noexcept {
        std::scoped_lock lock(mutex_);
        return request_locked(message,backend_flags);
    }
private:
    HRESULT request_locked(const std::string& message,std::int32_t* backend_flags=nullptr) noexcept {
        if(!live_)return E_FAIL;
        try{
            auto text=message;
            const bool inference=text.find("\"op\":\"infer\"")!=std::string::npos || text.find("\"op\":\"infer_batch\"")!=std::string::npos;
            if(text.find("\"op\":\"configure\"")!=std::string::npos || text.find("\"op\":\"release\"")!=std::string::npos)
                memory_after_inference_=false;
            if(GetTickCount64()<memory_until_.load(std::memory_order_relaxed) && inference){
                std::scoped_lock sample(memory_mutex_);
                // An idle pre-CUDA receipt must not suppress the first sample
                // after a configuration actually allocates inference resources.
                if(!memory_after_inference_ || GetTickCount64()-memory_.updated_ms>=1000){text.pop_back();text+=",\"memory\":true}";}
            }
            text+="\n";DWORD written{};
            if(!WriteFile(input_.value,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)||written!=text.size())throw E_FAIL;
            const auto reply=read(vulkan_ && text.find("\"op\":\"configure\"")!=std::string::npos?300000:30000);
            if(vulkan_ && boolean(reply,"\"ok\"") && boolean(reply,"\"preparing\"")){
                optimizing_.store(true,std::memory_order_relaxed);execution_updated_ms_=GetTickCount64();
                return E_PENDING; // Owned compiler is running; keep this worker alive.
            }
            if(!boolean(reply,"\"ok\"")){
                // An older/custom worker may lack this diagnostic command.
                // An explicit refusal leaves memory unavailable, never kills
                // a functioning inference worker or changes its execution state.
                if(message=="{\"op\":\"memory\"}")return E_NOTIMPL;
                throw E_FAIL;
            }
            if(inference) {
                const bool optimizing=boolean(reply,"\"optimizing\"");
                optimizing_.store(optimizing,std::memory_order_relaxed);
                // Protocol values are emitted by our owned portable worker.
                auto backend=reply.find("\"backend\"");
                backend=backend==std::string::npos?backend:reply.find(':',backend);
                const auto value=backend==std::string::npos?backend:reply.find('"',backend+1);
                const bool tensorrt=value!=std::string::npos && reply.compare(value,10,"\"tensorrt\"")==0;
                tensorrt_.store(tensorrt,std::memory_order_relaxed);
                const auto error_at=reply.find("\"optimization_error\"");
                const unsigned error=error_at==std::string::npos?0U:
                    reply.find("timed out",error_at)!=std::string::npos?1U:
                    reply.find("not installed",error_at)!=std::string::npos ||
                    reply.find("unavailable",error_at)!=std::string::npos?2U:3U;
                optimization_error_.store(error,std::memory_order_relaxed);
                const auto updated=GetTickCount64();
                execution_updated_ms_.store(updated,std::memory_order_release);
                if(backend_flags){
                    std::uint64_t profile{};if(vulkan_)number(reply,"\"precision_profile\"",profile);
                    *backend_flags=(tensorrt?1:0)|(optimizing?2:0)|4|(error?8:0)|(vulkan_?16:0)|
                        (profile<=255?static_cast<std::int32_t>(profile<<8):0);
                }
            }
            std::uint64_t usage{},reserved{};
            if(number(reply,"\"vram_private\"",usage) && number(reply,"\"torch_reserved\"",reserved)){
                std::scoped_lock sample(memory_mutex_);
                memory_={GetTickCount64(),usage,reserved,GetProcessId(process_.value),true,true};
                if(inference)memory_after_inference_=true;
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
RifeExecutionStatus rife_execution_status() noexcept {
    try {
        std::unique_lock lock(registry_mutex,std::try_to_lock);
        if(!lock.owns_lock())return {};
        RifeExecutionStatus result;
        for(const auto& [luid,weak]:workers) {
            if(auto worker=weak.lock()) {
                const auto value=worker->execution();
                if(!value.active)continue;
                result.updated_ms=std::max(result.updated_ms,value.updated_ms);
                result.active=true;result.optimizing|=value.optimizing;result.tensorrt|=value.tensorrt;
                result.vulkan|=value.vulkan;
                result.error=std::max(result.error,value.error);
            }
        }
        return result;
    }catch(...){return {};}
}
struct D3D12Rife::Impl{
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pack,unpack;
    ComPtr<ID3D12DescriptorHeap> heap,rtvs;
    std::array<ComPtr<ID3D12CommandAllocator>,2> allocators;
    std::array<ComPtr<ID3D12GraphicsCommandList>,2> lists;
    ID3D12GraphicsCommandList* commands{};
    ComPtr<ID3D12Fence> fence;Handle event;
    std::array<ComPtr<ID3D12Resource>,3> buffers;
    std::vector<ComPtr<ID3D12Resource>> inflight_images;
    std::shared_ptr<Worker> worker;
    std::array<std::unique_ptr<D3D12Rife>,2> fractional;
    DXGI_FORMAT format{};int model{};float scale{};UINT width{},height{},eyes{},proxy{};
    std::uint64_t id{identifiers.fetch_add(1)},luid{},value{},submitted{};
    bool configured{};
    HRESULT failure{S_OK};
    ~Impl(){
        bool safe=true;
        if(submitted && fence && fence->GetCompletedValue()<submitted){
            fence->SetEventOnCompletion(submitted,event.value);
            safe=WaitForSingleObject(event.value,30000)==WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason());
        }
        if(configured && worker)worker->request("{\"op\":\"release\",\"id\":"+std::to_string(id)+"}");
        // An unresponsive live GPU must retain objects referenced by its list.
        if(!safe){for(auto& b:buffers)b.Detach();for(auto& image:inflight_images)image.Detach();heap.Detach();rtvs.Detach();for(auto& a:allocators)a.Detach();for(auto& l:lists)l.Detach();root.Detach();pack.Detach();unpack.Detach();fence.Detach();}
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT i){auto h=heap->GetCPUDescriptorHandleForHeapStart();h.ptr+=i*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);return h;}
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT i){auto h=heap->GetGPUDescriptorHandleForHeapStart();h.ptr+=i*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);return h;}
    void wait_submitted(){
        if(submitted && fence->GetCompletedValue()<submitted){
            check(fence->SetEventOnCompletion(submitted,event.value));
            if(WaitForSingleObject(event.value,30000)!=WAIT_OBJECT_0)throw HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        check(device->GetDeviceRemovedReason());
    }
    void begin(bool compose=false){
        const auto i=compose?1U:0U;
        // Pack's allocator retired in its own receipt or the batch's last-pack
        // receipt on this same queue. Both also retire the preceding compose.
        check(allocators[i]->Reset());commands=lists[i].Get();
        check(commands->Reset(allocators[i].Get(),nullptr));
    }
    void execute(BridgeFlightLogger* diagnostic=nullptr,std::uint64_t fraction_eyes=0,bool compose=false,bool wait=true){
        const auto dimensions=(static_cast<std::uint64_t>(width)<<32)|height;
        rife_phase(diagnostic,compose?BridgeFlightOperation::rife_compose_execute:BridgeFlightOperation::rife_pack_execute,
            id,dimensions,fraction_eyes,[&]{
                check(commands->Close());ID3D12CommandList* list=commands;queue->ExecuteCommandLists(1,&list);
                submitted=++value;check(queue->Signal(fence.Get(),submitted));
            });
        if(!wait)return;
        rife_phase(diagnostic,compose?BridgeFlightOperation::rife_compose_wait:BridgeFlightOperation::rife_pack_wait,
            id,dimensions,fraction_eyes,[&]{
                wait_submitted();
            });
    }
    void size(UINT w,UINT h,UINT count){
        if(w==width&&h==height&&count==eyes&&configured)return;
        if(worker->vulkan()){
            std::ostringstream prepare;prepare<<"{\"op\":\"prepare\",\"luid\":"<<luid<<",\"width\":"<<w<<",\"height\":"<<h<<",\"eyes\":"<<count<<'}';
            check(worker->request(prepare.str()));
        }
        // Reconfiguration must not release CUDA buffers/descriptors still read
        // by the last queued composition.
        wait_submitted();inflight_images.clear();
        if(configured){check(worker->request("{\"op\":\"release\",\"id\":"+std::to_string(id)+"}"));configured=false;}
        for(auto& b:buffers)b.Reset();width=w;height=h;eyes=count;
        auto bytes=static_cast<UINT64>(w)*h*count*5*2;
        if(bytes>UINT_MAX*2ULL)throw E_INVALIDARG;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;
        desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;
        desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES properties{};properties.Type=D3D12_HEAP_TYPE_DEFAULT;
        std::ostringstream message;message<<"{\"op\":\"configure\",\"optimize\":true,\"id\":"<<id<<",\"luid\":"<<luid<<",\"width\":"<<w<<",\"height\":"<<h<<",\"eyes\":"<<count<<",\"buffers\":[";
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
        if(p->worker->vulkan() && model!=3)return E_NOTIMPL; // Radeon starts with validated RIFE4.4 only.
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
        for(UINT i=0;i<2;++i){
            check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&p->allocators[i])));
            check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,p->allocators[i].Get(),nullptr,IID_PPV_ARGS(&p->lists[i])));
            check(p->lists[i]->Close());
        }
        check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&p->fence)));p->event.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!p->event.value)throw E_FAIL;
        impl_=std::move(p);return S_OK;
    }catch(HRESULT hr){return hr;}catch(...){return E_FAIL;}
}
HRESULT D3D12Rife::generate(ID3D12Resource* a,ID3D12Resource* b,ID3D12Resource* out,
    std::span<const D3D12ReprojectionView> av,std::span<const D3D12ReprojectionView> bv,std::span<const D3D12ReprojectionView> tv,
    std::span<const std::array<float,4>> ar,std::span<const std::array<float,4>> br,D3D12_RESOURCE_STATES output_state,float fraction,Timing timing,Completion completion)noexcept{
    return generate_impl(a,b,out,av,bv,tv,ar,br,output_state,fraction,timing,completion,0);
}
HRESULT D3D12Rife::generate_impl(ID3D12Resource* a,ID3D12Resource* b,ID3D12Resource* out,
    std::span<const D3D12ReprojectionView> av,std::span<const D3D12ReprojectionView> bv,std::span<const D3D12ReprojectionView> tv,
    std::span<const std::array<float,4>> ar,std::span<const std::array<float,4>> br,D3D12_RESOURCE_STATES output_state,float fraction,Timing timing,Completion completion,unsigned stage)noexcept{
    try{
        if(!impl_||!a||!b||!out||av.empty()||av.size()>2||av.size()!=bv.size()||av.size()!=tv.size()||ar.size()!=tv.size()||br.size()!=tv.size())return E_INVALIDARG;
        auto& p=*impl_;if(FAILED(p.failure))return p.failure;const auto desc=b->GetDesc();const auto roi=rect_of(bv[0],desc);
        const UINT raw_w=std::max(32U,static_cast<UINT>(std::ceil(roi.width*p.scale))),raw_h=std::max(32U,static_cast<UINT>(std::ceil(roi.height*p.scale)));
        // Vulkan tensors have aligned planar channel strides. Use the same
        // 32-pixel graph alignment in the offline compiler and shared buffers.
        const UINT w=p.worker->vulkan()?(raw_w+31)/32*32:raw_w,h=p.worker->vulkan()?(raw_h+31)/32*32:raw_h;
        p.size(w,h,static_cast<UINT>(av.size()));
        // Keep the preceding deferred output and the current transaction alive
        // until pack's receipt proves the preceding compose has retired.
        if(stage!=2)for(auto* image:{a,b,out})p.inflight_images.emplace_back(image);
        auto& flight=bridge_flight_logger();
        auto* diagnostic=flight.enabled()?&flight:nullptr;
        std::uint64_t fraction_eyes=0;
        if(diagnostic){std::uint32_t bits{};std::memcpy(&bits,&fraction,sizeof(bits));fraction_eyes=(static_cast<std::uint64_t>(p.eyes)<<32)|bits;}
        const auto mark=[&](UINT query){if(timing.queries)p.commands->EndQuery(timing.queries,D3D12_QUERY_TYPE_TIMESTAMP,timing.base+query);};
        if(stage!=2){
        p.begin();if(timing.first){mark(0);mark(5);}
        if(timing.input_queries)p.commands->EndQuery(timing.input_queries,D3D12_QUERY_TYPE_TIMESTAMP,4);
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
        if(timing.first)mark(1);
        if(timing.input_queries)p.commands->EndQuery(timing.input_queries,D3D12_QUERY_TYPE_TIMESTAMP,5);
        p.execute(diagnostic,fraction_eyes,false,stage!=1);
        if(stage==1)return S_OK;
        }
        p.inflight_images={a,b,out};
        std::int32_t backend_flags{};
        const auto dimensions=(static_cast<std::uint64_t>(w)<<32)|h;
        if(stage==0){rife_phase(diagnostic,BridgeFlightOperation::rife_infer,p.id,dimensions,fraction_eyes,[&]{
            check(p.worker->request("{\"op\":\"infer\",\"id\":"+std::to_string(p.id)+",\"model\":"+std::to_string(p.model)+",\"fraction\":"+std::to_string(fraction)+"}",diagnostic?&backend_flags:nullptr));
        });
        if(diagnostic)diagnostic->event(BridgeFlightOperation::rife_backend,
            backend_flags,p.id,dimensions,fraction_eyes);
        }
        // Restore full-resolution A/B descriptors after both reduced-input
        // packs have retired. Only flow/mask planes return from Python.
        for(UINT source_index=0;source_index<2;++source_index){
            auto* resource=source_index?b:a;const auto description=resource->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=p.format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2DArray.MipLevels=1;srv.Texture2DArray.ArraySize=description.DepthOrArraySize;
            p.device->CreateShaderResourceView(resource,&srv,p.cpu(12+source_index));
        }
        // Pack's fence also retires the preceding compose before descriptor
        // slots12..17, its list/allocator or the shared CUDA output are reused.
        p.begin(true);if(timing.last){mark(2);mark(3);}
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
        const bool wait=completion==Completion::wait;
        p.execute(diagnostic,fraction_eyes,true,wait);
        if(wait)p.inflight_images.clear();
        return S_OK;
    }catch(HRESULT hr){if(impl_&&hr!=E_PENDING)impl_->failure=hr;return hr;}
    catch(...){if(impl_)impl_->failure=E_FAIL;return E_FAIL;}
}
HRESULT D3D12Rife::generate_batch(ID3D12Resource* a,ID3D12Resource* b,
    std::span<const D3D12ReprojectionView> av,std::span<const D3D12ReprojectionView> bv,
    std::span<const Output> outputs,D3D12_RESOURCE_STATES state,Completion completion) noexcept {
    if(!impl_||outputs.empty()||outputs.size()>3)return E_INVALIDARG;
    if(!impl_->worker->fraction_batch()){
        for(const auto& o:outputs){const auto hr=generate(a,b,o.image,av,bv,o.views,o.a_rotations,o.b_rotations,state,o.fraction,o.timing,completion);if(FAILED(hr))return hr;}
        return S_OK;
    }
    if(outputs.size()==1){const auto& o=outputs[0];return generate(a,b,o.image,av,bv,o.views,o.a_rotations,o.b_rotations,state,o.fraction,o.timing,completion);}
    std::array<D3D12Rife*,3> generators{this};
    auto poison=[&](HRESULT hr){if(hr!=E_PENDING)for(auto* g:generators)if(g&&g->impl_)g->impl_->failure=hr;return hr;};
    try{
        if(FAILED(impl_->failure))return impl_->failure;
        // Validate the entire request before queuing any part of a batch.
        for(const auto& o:outputs)if(!a||!b||!o.image||av.empty()||av.size()>2||bv.size()!=av.size()||o.views.size()!=av.size()||o.a_rotations.size()!=av.size()||o.b_rotations.size()!=av.size()||!(o.fraction>=.05F&&o.fraction<=.95F))return E_INVALIDARG;
        for(size_t i=1;i<outputs.size();++i){
            auto& g=impl_->fractional[i-1];
            if(!g){auto candidate=std::make_unique<D3D12Rife>();check(candidate->initialize(impl_->device.Get(),impl_->queue.Get(),impl_->format,impl_->model,impl_->scale));g=std::move(candidate);}
            generators[i]=g.get();
        }
        for(size_t i=0;i<outputs.size();++i){const auto& o=outputs[i];
            check(generators[i]->generate_impl(a,b,o.image,av,bv,o.views,o.a_rotations,o.b_rotations,state,o.fraction,o.timing,Completion::queue_ordered,1));
        }
        auto& flight=bridge_flight_logger();auto* diagnostic=flight.enabled()?&flight:nullptr;
        auto& last=*generators[outputs.size()-1]->impl_;
        const auto dimensions=(std::uint64_t(last.width)<<32)|last.height;
        // Low word 2/3 denotes a fractional batch rather than float fraction
        // bits; high word keeps the existing number-of-views contract.
        const auto batch_identity=(std::uint64_t(last.eyes)<<32)|outputs.size();
        rife_phase(diagnostic,BridgeFlightOperation::rife_pack_wait,impl_->id,dimensions,batch_identity,[&]{last.wait_submitted();});
        std::ostringstream request;request<<"{\"op\":\"infer_batch\",\"requests\":[";
        for(size_t i=0;i<outputs.size();++i){if(i)request<<',';request<<"{\"id\":"<<generators[i]->impl_->id<<",\"model\":"<<impl_->model<<",\"fraction\":"<<outputs[i].fraction<<'}';}
        request<<"]}";std::int32_t flags{};
        rife_phase(diagnostic,BridgeFlightOperation::rife_infer,impl_->id,dimensions,batch_identity,[&]{check(impl_->worker->request(request.str(),diagnostic?&flags:nullptr));});
        if(diagnostic)flight.event(BridgeFlightOperation::rife_backend,flags,impl_->id,dimensions,batch_identity);
        for(size_t i=0;i<outputs.size();++i){const auto& o=outputs[i];
            check(generators[i]->generate_impl(a,b,o.image,av,bv,o.views,o.a_rotations,o.b_rotations,state,o.fraction,o.timing,Completion::queue_ordered,2));
        }
        if(completion==Completion::wait){last.wait_submitted();for(size_t i=0;i<outputs.size();++i)generators[i]->impl_->inflight_images.clear();}
        return S_OK;
    }catch(HRESULT hr){return poison(hr);}catch(...){return poison(E_FAIL);}
}
}
