// Run the shipping NR host, codec and exact retirement ledger. Only the NGX
// model/core callbacks are replaced; the GPU chain and resource states are real.
#include "../src/neural/nr_feature.cpp"
#include "../src/neural/nr_host.cpp"
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <fstream>
#include <map>
#include <set>

namespace nr_test {struct Gpu;}
namespace nr_witness {void verify(nr_test::Gpu&,bool,unsigned);}
namespace nr_test {
using Microsoft::WRL::ComPtr;
void need(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
void hr(HRESULT value){if(FAILED(value))throw std::runtime_error(std::format("HRESULT {:X}",unsigned(value)));}
struct Params final:NVSDK_NGX_Parameter {
    std::map<std::string,long double> numbers;
    std::map<std::string,void*> pointers;
#define NUMBER(T) void Set(const char* n,T v)override{numbers[n]=v;} \
    NVSDK_NGX_Result Get(const char* n,T* v)const override{auto i=numbers.find(n);if(i==numbers.end())return NVSDK_NGX_Result_FAIL_InvalidParameter;*v=T(i->second);return NVSDK_NGX_Result_Success;}
    NUMBER(unsigned long long) NUMBER(float) NUMBER(double) NUMBER(unsigned int) NUMBER(int)
#undef NUMBER
#define POINTER(T) void Set(const char* n,T* v)override{pointers[n]=v;} \
    NVSDK_NGX_Result Get(const char* n,T** v)const override{auto i=pointers.find(n);if(i==pointers.end())return NVSDK_NGX_Result_FAIL_InvalidParameter;*v=static_cast<T*>(i->second);return NVSDK_NGX_Result_Success;}
    POINTER(ID3D11Resource) POINTER(ID3D12Resource) POINTER(void)
#undef POINTER
    void Reset()override{numbers.clear();pointers.clear();}
};
NVSDK_NGX_Result capabilities(NVSDK_NGX_Parameter** p){*p=new Params;return NVSDK_NGX_Result_Success;}
NVSDK_NGX_Result destroy_parameters(NVSDK_NGX_Parameter* p){delete static_cast<Params*>(p);return NVSDK_NGX_Result_Success;}
using Tune=std::tuple<int,float,int,float,float,float,int>;
struct Model {unsigned source{},pass{},width{},height{};Tune tune;};
std::set<Model*> live;
std::vector<unsigned> released;
bool refuse_second{};
void* __cdecl create(const wchar_t*,const wchar_t*,ID3D12Device*,ID3D12GraphicsCommandList*,
    void* parameters,unsigned width,unsigned height,int preset,float intensity,int style,
    float structure,float tone,float skin,int mask,int){
    need(g_retirementSource.has_value(),"model has a source history owner");
    auto& owner=g_nrByHandle.at(*g_retirementSource);
    const unsigned pass=owner.second && owner.second->params==parameters?2:1;
    if(pass==2 && refuse_second)return nullptr;
    auto* model=new Model{*g_retirementSource,pass,width,height,{preset,intensity,style,structure,tone,skin,mask}};
    live.insert(model);return model;
}
void __cdecl release(void* pointer){auto* m=static_cast<Model*>(pointer);need(live.erase(m)==1,"feature released exactly once");released.push_back(m->pass);delete m;}
void __cdecl contract(void* p,unsigned w,unsigned h,unsigned ow,unsigned oh,unsigned flags,int,unsigned){
    auto* v=static_cast<Params*>(p);v->Set("width",w);v->Set("height",h);v->Set("out_width",ow);v->Set("out_height",oh);v->Set("flags",flags);
}
void __cdecl extras(void*,float,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,unsigned,unsigned,unsigned,unsigned){}
void move(ID3D12GraphicsCommandList* l,ID3D12Resource* p,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
    if(a==b)return;D3D12_RESOURCE_BARRIER r{};r.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;r.Transition={p,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};l->ResourceBarrier(1,&r);
}
struct Readback {ComPtr<ID3D12Resource> buffer;D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT64 bytes{};};
struct Evaluation {Model* model{};unsigned pass{},source{},width{},height{};ID3D12Resource* depth{};ID3D12Resource* motion{};int reset{};float x{},y{};Readback input,depthPixels,motionPixels;};
struct Gpu {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12InfoQueue> messages;
    ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> done;
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;
    std::vector<ComPtr<ID3D12DescriptorHeap>> bindings;
    std::vector<ComPtr<ID3D12Resource>> uploads;
    std::vector<Evaluation> evaluations;
    UINT64 serial{};
    Gpu(){
        ComPtr<IDXGIFactory4> f;ComPtr<IDXGIAdapter> a;hr(CreateDXGIFactory1(IID_PPV_ARGS(&f)));hr(f->EnumWarpAdapter(IID_PPV_ARGS(&a)));
        hr(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));device.As(&messages);
        D3D12_COMMAND_QUEUE_DESC q{};hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        hr(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));hr(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)));
        D3D12_DESCRIPTOR_RANGE ranges[2]{};ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1};
        D3D12_ROOT_PARAMETER table{};table.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;table.DescriptorTable={2,ranges};
        D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=1;rs.pParameters=&table;
        ComPtr<ID3DBlob> blob,error;hr(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
        hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
        constexpr char shader[]=R"(Texture2D<float4> a:register(t0);RWTexture2D<float4> b:register(u0);
        [numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){uint w,h;b.GetDimensions(w,h);if(p.x<w&&p.y<h){float4 v=a.Load(int3(p.xy,0));b[p.xy]=float4(v.rgb+0.125,v.a);}})";
        hr(D3DCompile(shader,sizeof(shader)-1,"mock NR model",nullptr,nullptr,"main","cs_5_0",0,0,&blob,&error));
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root.Get();ps.CS={blob->GetBufferPointer(),blob->GetBufferSize()};hr(device->CreateComputePipelineState(&ps,IID_PPV_ARGS(&pipeline)));
    }
    ComPtr<ID3D12Resource> buffer(UINT64 bytes,D3D12_HEAP_TYPE type){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES h{};h.Type=type;ComPtr<ID3D12Resource> r;
        hr(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_READBACK?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&r)));return r;
    }
    ComPtr<ID3D12Resource> texture(unsigned w,unsigned h,DXGI_FORMAT format,D3D12_RESOURCE_STATES state,unsigned array=1,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=array;d.MipLevels=d.SampleDesc.Count=1;d.Format=format;d.Flags=flags;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> r;hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));return r;
    }
    void fill(ID3D12Resource* r,D3D12_RESOURCE_STATES state,std::array<float,4> pixel,unsigned ox=0,unsigned oy=0,unsigned w=0,unsigned h=0,unsigned slice=0){
        auto d=r->GetDesc();if(w)d.Width=w;if(h)d.Height=h;d.DepthOrArraySize=1;D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
        auto u=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD);void* p{};D3D12_RANGE empty{};hr(u->Map(0,&empty,&p));
        const unsigned channels=d.Format==DXGI_FORMAT_R32_FLOAT?1:d.Format==DXGI_FORMAT_R32G32_FLOAT?2:4;
        for(unsigned y=0;y<d.Height;++y)for(unsigned x=0;x<d.Width;++x)memcpy(static_cast<char*>(p)+fp.Offset+y*fp.Footprint.RowPitch+x*channels*sizeof(float),pixel.data(),channels*sizeof(float));u->Unmap(0,nullptr);
        move(list.Get(),r,state,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=u.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;dst.pResource=r;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=slice;
        list->CopyTextureRegion(&dst,ox,oy,0,&src,nullptr);move(list.Get(),r,D3D12_RESOURCE_STATE_COPY_DEST,state);uploads.push_back(u);
    }
    Readback capture(ID3D12Resource* r,D3D12_RESOURCE_STATES state){
        Readback out;auto d=r->GetDesc();device->GetCopyableFootprints(&d,0,1,0,&out.layout,nullptr,nullptr,&out.bytes);out.buffer=buffer(out.bytes,D3D12_HEAP_TYPE_READBACK);
        move(list.Get(),r,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=r;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=out.buffer.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=out.layout;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);move(list.Get(),r,D3D12_RESOURCE_STATE_COPY_SOURCE,state);return out;
    }
    void model(Model* m,ID3D12Resource* input,ID3D12Resource* output){
        auto inputDesc=input->GetDesc();auto outputDesc=output->GetDesc();need(inputDesc.Width==m->width&&inputDesc.Height==m->height&&outputDesc.Width==m->width&&outputDesc.Height==m->height,"both model passes keep the same configured working dimensions");
        D3D12_DESCRIPTOR_HEAP_DESC d{};d.NumDescriptors=2;d.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;d.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;ComPtr<ID3D12DescriptorHeap> heap;hr(device->CreateDescriptorHeap(&d,IID_PPV_ARGS(&heap)));
        auto cpu=heap->GetCPUDescriptorHandleForHeapStart();D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=inputDesc.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;device->CreateShaderResourceView(input,&srv,cpu);
        cpu.ptr+=device->GetDescriptorHandleIncrementSize(d.Type);D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=outputDesc.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(output,nullptr,&uav,cpu);
        ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());list->SetPipelineState(pipeline.Get());list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());list->Dispatch((m->width+7)/8,(m->height+7)/8,1);
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;barrier.UAV.pResource=output;list->ResourceBarrier(1,&barrier);bindings.push_back(heap);
    }
    void submit(bool wait=true){
        hr(list->Close());ID3D12CommandList* p[]{list.Get()};auto receipts=xrfg::neural_rendering::take_submission_receipts(1,p);queue->ExecuteCommandLists(1,p);xrfg::neural_rendering::complete_submission(queue.Get(),receipts);hr(queue->Signal(done.Get(),++serial));if(wait)finish();
    }
    void finish(){HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);need(e!=nullptr,"fence event");hr(done->SetEventOnCompletion(serial,e));auto r=WaitForSingleObject(e,10000);CloseHandle(e);need(r==WAIT_OBJECT_0,"NR exact submission timed out");}
    void reset(){hr(allocator->Reset());hr(list->Reset(allocator.Get(),nullptr));bindings.clear();uploads.clear();evaluations.clear();}
    std::array<float,4> pixel(const Readback& r,unsigned x=0,unsigned y=0){void* p{};D3D12_RANGE range{0,size_t(r.bytes)},empty{};hr(r.buffer->Map(0,&range,&p));std::array<float,4> v{};memcpy(v.data(),static_cast<char*>(p)+r.layout.Offset+y*r.layout.Footprint.RowPitch+x*16,sizeof(v));r.buffer->Unmap(0,&empty);return v;}
};
Gpu* active{};
int __cdecl evaluate(ID3D12GraphicsCommandList* list,void* handle,void*,ID3D12Resource* input,
    ID3D12Resource* depth,ID3D12Resource* motion,ID3D12Resource* output,unsigned w,unsigned h,
    unsigned,unsigned,int,int reset,float intensity,int style,float structure,float tone,float skin,int mask,float x,float y){
    auto* m=static_cast<Model*>(handle);need(live.contains(m)&&active->list.Get()==list,"model owns a live feature and native recording");
    const auto& [preset,ci,cs,cstr,ctone,cskin,cmask]=m->tune;
    need(ci==intensity&&cs==style&&cstr==structure&&ctone==tone&&cskin==skin&&cmask==mask,"second model uses the same tuning at create and evaluation");
    need(w==m->width&&h==m->height,"evaluate uses the feature's working extent");
    active->evaluations.push_back({m,m->pass,m->source,w,h,depth,motion,reset,x,y,active->capture(input,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),active->capture(depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),active->capture(motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)});
    active->model(m,input,output);return NVSDK_NGX_Result_Success;
}
struct Eye {unsigned id;Params params;ComPtr<ID3D12Resource> color,output;};
}

void quad_run(nr_test::Gpu& gpu,float scale,bool noUav=false,bool arrayDepth=false) {
    using namespace nr_test;
    constexpr auto state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto color=gpu.texture(256,192,DXGI_FORMAT_R32G32B32A32_FLOAT,state,1,noUav?D3D12_RESOURCE_FLAG_NONE:D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto depth=gpu.texture(256,192,DXGI_FORMAT_R32_FLOAT,state);
    auto motion=gpu.texture(256,192,DXGI_FORMAT_R32G32_FLOAT,state);
    // Output array belongs to SR. Pre-SR must touch ONLY each input colour ROI.
    auto output=gpu.texture(200,160,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    gpu.fill(color.Get(),state,{.03f,.03f,.03f,.6f});gpu.fill(depth.Get(),state,{.9f,0,0,0});gpu.fill(motion.Get(),state,{8,8,0,0});
    auto depthArray=gpu.texture(100,80,DXGI_FORMAT_R32_FLOAT,state,4);
    if(arrayDepth)for(unsigned slice=0;slice<4;++slice)
        gpu.fill(depthArray.Get(),state,{slice?.8f:.1f,0,0,0},0,0,100,80,slice);
    if(arrayDepth)for(unsigned slice=1;slice<4;++slice) {
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={depthArray.Get(),slice,state,D3D12_RESOURCE_STATE_COPY_DEST};
        gpu.list->ResourceBarrier(1,&barrier);
    }
    std::array<Eye,4> views{{{301},{302},{303},{304}}};
    std::array<std::array<unsigned,2>,4> origins{{{0,0},{128,0},{0,96},{128,96}}};
    for(unsigned i=0;i<4;++i){auto& v=views[i];v.color=color;v.output=output;
        const auto c=origins[i],d=origins[(i+1)%4],m=origins[(i+2)%4];
        v.params.Set(NVSDK_NGX_Parameter_Color,color.Get());v.params.Set(NVSDK_NGX_Parameter_Output,output.Get());
        v.params.Set(NVSDK_NGX_Parameter_Depth,depth.Get());v.params.Set(NVSDK_NGX_Parameter_MotionVectors,motion.Get());
        v.params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,100U);v.params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,80U);
        v.params.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,c[0]);v.params.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,c[1]);
        v.params.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,d[0]);v.params.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,d[1]);
        v.params.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,m[0]);v.params.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,m[1]);
        v.params.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,unsigned(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
        v.params.Set(NVSDK_NGX_Parameter_MV_Scale_X,100.f);v.params.Set(NVSDK_NGX_Parameter_MV_Scale_Y,80.f);
        if(arrayDepth && i==0) {
            v.params.Set(NVSDK_NGX_Parameter_Depth,depthArray.Get());
            v.params.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,0U);
            v.params.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,0U);
        }
        gpu.fill(depth.Get(),state,{.1f*(i+1),0,0,0},d[0],d[1],100,80);
        gpu.fill(motion.Get(),state,{.01f*(i+1),.02f*(i+1),0,0},m[0],m[1],100,80);
    }
    xrfg::neural_rendering::Settings settings;settings.enabled=true;settings.before_sr=true;settings.working_scale=scale;
    settings.second_pass=true;settings.transfer_strength=1;settings.colour_strength=1;settings.max_ratio=8;
    xrfg::neural_rendering::configure(settings);
    const auto record=[&]{for(unsigned i=0;i<4;++i){auto c=origins[i];
        gpu.fill(color.Get(),state,{.1f*(i+1),.2f,.3f,.6f},c[0],c[1],100,80);
        xrfg::neural_rendering::before_sr(views[i].id,gpu.list.Get(),&views[i].params,gpu.queue.Get());}};
    record();gpu.submit();need(live.size()==8,"four views each own two independent temporal histories");gpu.reset();
    record();auto result=gpu.capture(color.Get(),state);gpu.submit();
    need(gpu.evaluations.size()==8,"ALL four atlas views run both model passes");
    for(unsigned i=0;i<4;++i){const auto& e=gpu.evaluations[2*i];const auto c=origins[i];
        need(e.source==views[i].id && e.width==unsigned(100*scale) && e.height==unsigned(80*scale),"view identity/working extent preserved");
        need(std::abs(gpu.pixel(e.input)[0]-.1f*(i+1))<.00001f,"colour comes from this view, never adjacent eye");
        need(std::abs(gpu.pixel(e.depthPixels)[0]-.1f*(i+1))<.00001f,"depth uses its independent view origin at 100 and 50 percent");
        auto mv=gpu.pixel(e.motionPixels);need(std::abs(mv[0]-.01f*(i+1))<.00001f && std::abs(mv[1]-.02f*(i+1))<.00001f,"motion uses independent view region without changing encoding");
        need(std::abs(e.x-100*scale)<.00001f&&std::abs(e.y-80*scale)<.00001f,"MV scale applied exactly once");
        std::printf("quad view%u pixel=%.7f expected=%.7f\n",i,gpu.pixel(result,c[0]+30,c[1]+30)[0],.1f*(i+1)+.25f);
        need(std::abs(gpu.pixel(result,c[0]+30,c[1]+30)[0]-(.1f*(i+1)+.25f))<.0001f,"resolved NR edit reaches every view's own atlas rectangle");
        need(g_nrByHandle.at(views[i].id).resolvedFrames==1,"receipt counts successful resolve, not entry");
    }
    need(std::abs(gpu.pixel(result,110,90)[0]-.03f)<.00001f,"outside ALL view rectangles remains untouched");
    nr_witness::verify(gpu,true,4);
    gpu.reset();record();gpu.submit();for(auto& e:gpu.evaluations)need(!e.reset,"four histories remain independently stable");
    gpu.reset();views[0].params.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,128U);views[0].params.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,96U);
    record();gpu.submit();for(auto& e:gpu.evaluations)need(bool(e.reset)==(e.source==views[0].id),"a changed guide region resets only its own two model histories");
    for(auto& v:views)xrfg::neural_rendering::retire_handle(v.id);TickNrRetired();xrfg::neural_rendering::shutdown();need(live.empty(),"all quad-view histories retire");
    std::printf("NR QUAD %.0f%%: four histories, independent colour/depth/MV regions, exact resolve and guard pixels PASS\n",scale*100);
}

namespace nr_witness {
bool enabled=true;
std::set<std::uint64_t> handles;
struct Image {xrfg::TwoFrameImageInfo info;nr_test::Readback pixels;};
std::vector<Image> images;
std::vector<xrfg::TwoFrameNrEvaluation> rows;
void verify(nr_test::Gpu& gpu,bool before,unsigned views) {
    nr_test::need(rows.size()==views*2,"F8 captures both exact model calls for every view");
    nr_test::need(images.size()==views*11,"F8 source/proxy/input/guides/output/resolve witnesses complete");
    for(const auto& row:rows)nr_test::need(row.before_sr==before && row.result==1 && row.resources[0]!=row.resources[1],"F8 mode/result/resources match actual model call");
    for(const auto& image:images) {
        const auto stage=std::string_view(image.info.stage);
        if(stage=="nr_pass1_color") {const auto rgb=gpu.pixel(image.pixels);nr_test::need(std::max({rgb[0],rgb[1],rgb[2]})-std::min({rgb[0],rgb[1],rgb[2]})>.01f,"captured model input retains known chroma, never depth");}
        if(stage=="nr_pass1_depth")nr_test::need(image.pixels.layout.Footprint.Format==DXGI_FORMAT_R32_FLOAT,"actual model depth format preserved");
    }
    const auto count=images.size();enabled=false;
    nr_test::need(xrfg::two_frame_capture_claim_nr(999)==0 && images.size()==count,"no request means no readback work");
    std::printf("NR F8 %s: %u views, %zu exact GPU images, %zu model records PASS\n",before?"pre-SR":"post-SR",views,images.size(),rows.size());
}
}
namespace xrfg {
std::uint64_t two_frame_capture_claim_nr(std::uint64_t handle) noexcept {
    if(!nr_witness::enabled || !nr_witness::handles.insert(handle).second)return 0;
    return nr_witness::handles.size();
}
void two_frame_capture_nr_evaluation(const TwoFrameNrEvaluation& value) noexcept {nr_witness::rows.push_back(value);}
void two_frame_capture_record_ng_x(const TwoFrameImageInfo& info,ID3D12Device*,ID3D12GraphicsCommandList*,ID3D12Resource* source,D3D12_RESOURCE_STATES state) noexcept {
    nr_witness::images.push_back({info,nr_test::active->capture(source,state)});
}
}

int main(int argc,char** argv)try{
    using namespace nr_test;
    const bool before=argc<2||std::string_view(argv[1])!="post";
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    Gpu gpu;active=&gpu;
    auto folder=std::filesystem::temp_directory_path()/(L"ofxr-nr2-host-"+std::to_wstring(GetCurrentProcessId()));std::filesystem::create_directories(folder);
    std::ofstream(folder/L"nvngx_dlssnr.dll",std::ios::binary).put('0');g_dllDir=folder;
    g_runtime.forwarder=LoadLibraryW(L"kernel32.dll");g_runtime.forwarderReady=true;g_runtime.floatSlotKnown=true;
    g_runtime.create=create;g_runtime.evaluate=evaluate;g_runtime.release=release;g_runtime.setContract=contract;g_runtime.setExtras=extras;g_capabilityFloatSlot=1;
    services().initialized=true;services().capability=capabilities;services().destroy=destroy_parameters;
    if(argc>1 && std::string_view(argv[1]).starts_with("quad")){quad_run(gpu,std::string_view(argv[1]).find("half")!=std::string_view::npos?.5f:1.f,std::string_view(argv[1])=="quad-copy",std::string_view(argv[1]).starts_with("quad-array"));
        if(gpu.messages)for(UINT64 i=0;i<gpu.messages->GetNumStoredMessages();++i){SIZE_T size{};gpu.messages->GetMessage(i,nullptr,&size);std::vector<char> b(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(b.data());hr(gpu.messages->GetMessage(i,m,&size));need(m->Severity!=D3D12_MESSAGE_SEVERITY_ERROR&&m->Severity!=D3D12_MESSAGE_SEVERITY_CORRUPTION,m->pDescription);}
        std::filesystem::remove(folder/L"nvngx_dlssnr.dll");std::filesystem::remove(folder);return 0;}
    auto depth=gpu.texture(100,80,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto motion=gpu.texture(100,80,DXGI_FORMAT_R32G32_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu.fill(depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,{.5f,0,0,0});gpu.fill(motion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,{0,0,0,0});
    std::array<Eye,2> eyes{{{101},{202}}};
    for(auto& eye:eyes){
        eye.color=gpu.texture(100,80,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        eye.output=gpu.texture(200,160,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        eye.params.Set(NVSDK_NGX_Parameter_Color,eye.color.Get());eye.params.Set(NVSDK_NGX_Parameter_Output,eye.output.Get());eye.params.Set(NVSDK_NGX_Parameter_Depth,depth.Get());eye.params.Set(NVSDK_NGX_Parameter_MotionVectors,motion.Get());
        eye.params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,100U);eye.params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,80U);eye.params.Set(NVSDK_NGX_Parameter_Width,100U);eye.params.Set(NVSDK_NGX_Parameter_Height,80U);eye.params.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,static_cast<unsigned>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
        eye.params.Set(NVSDK_NGX_Parameter_MV_Scale_X,1.f);eye.params.Set(NVSDK_NGX_Parameter_MV_Scale_Y,1.f);
    }
    xrfg::neural_rendering::Settings settings;settings.enabled=true;settings.before_sr=before;settings.working_scale=.5f;settings.second_pass=true;settings.preset=2;settings.intensity=.625f;settings.local_tone=.75f;settings.transfer_strength=0;settings.colour_strength=1;
    xrfg::neural_rendering::configure(settings);
    const auto record=[&]{for(auto& eye:eyes){auto* target=before?eye.color.Get():eye.output.Get();gpu.fill(target,before?D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_UNORDERED_ACCESS,{.95f,.2f,.4f,.6f});
        if(before)xrfg::neural_rendering::before_sr(eye.id,gpu.list.Get(),&eye.params,gpu.queue.Get());else xrfg::neural_rendering::after_sr(eye.id,gpu.list.Get(),&eye.params,gpu.queue.Get());}};
    record();gpu.submit();need(live.size()==4,"two eyes have four independent NR histories");need(gpu.evaluations.empty(),"feature creation retires before first evaluation");gpu.reset();
    record();gpu.submit();need(gpu.evaluations.size()==4,"two NR evaluations per eye");
    for(unsigned i=0;i<4;i+=2){const auto& first=gpu.evaluations[i];const auto& second=gpu.evaluations[i+1];
        need(first.pass==1&&second.pass==2&&first.source==second.source&&first.model!=second.model,"ordered independent pass histories");
        need(first.width==(before?50U:100U)&&first.height==(before?40U:80U)&&first.width==second.width&&first.height==second.height,"50 percent is applied once, never 25 percent on pass two");
        need(first.depth==second.depth&&first.motion==second.motion&&first.x==second.x&&first.y==second.y,"both passes reuse matched guide contract");
        need(first.model->tune==second.model->tune,"both passes retain identical tuning");
        const auto a=gpu.pixel(first.input),b=gpu.pixel(second.input);
        for(unsigned c=0;c<3;++c)need(std::abs(b[c]-std::clamp(a[c]+.125f,0.f,1.f))<.00001f,"second input is the clamped GPU result of first model, not the original colour");
        need(a[3]==b[3],"intermediate clamp preserves alpha");need(first.reset&&second.reset,"fresh histories reset independently");
    }
    nr_witness::verify(gpu,before,2);
    need(xrfg::neural_rendering::status().running,"NR running requires successful second pass");
    gpu.reset();record();gpu.submit();for(auto& e:gpu.evaluations)need(!e.reset,"steady histories retained on both passes");
    gpu.reset();for(auto& eye:eyes)eye.params.Set(NVSDK_NGX_Parameter_Reset,1);record();gpu.submit();for(auto& e:gpu.evaluations)need(e.reset,"source reset reaches both model histories");for(auto& eye:eyes)eye.params.Set(NVSDK_NGX_Parameter_Reset,0);
    // Hold an actual submitted recording on its queue; turning 2x off must
    // park its features/resources without waiting or releasing them early.
    gpu.reset();record();ComPtr<ID3D12Fence> gate;hr(gpu.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));hr(gpu.queue->Wait(gate.Get(),1));gpu.submit(false);
    const auto count_before=released.size();settings.second_pass=false;xrfg::neural_rendering::configure(settings);
    need(released.size()==count_before&&live.size()==4,"optional histories survive until exact GPU receipt retires");
    for(auto& eye:eyes)need(!g_nrByHandle.at(eye.id).second&&!g_nrByHandle.at(eye.id).secondClamp,"disabled 2x detaches optional state immediately");
    hr(gate->Signal(1));gpu.finish();TickNrRetired();need(live.size()==2,"retired optional histories released after their GPU fence");
    gpu.reset();record();gpu.submit();need(gpu.evaluations.size()==2,"checkbox off restores one evaluation per eye");
    // Optional model creation refusal must not poison the original model.
    gpu.reset();refuse_second=true;settings.second_pass=true;xrfg::neural_rendering::configure(settings);record();gpu.submit();
    need(!g_runtime.failed&&!xrfg::neural_rendering::status().running&&!xrfg::neural_rendering::status().reason.empty(),"second model refusal is isolated and visible");
    gpu.reset();settings.second_pass=false;xrfg::neural_rendering::configure(settings);record();gpu.submit();need(gpu.evaluations.size()==2&&xrfg::neural_rendering::status().running,"disable restores first NR pass after optional refusal");
    for(auto& eye:eyes)xrfg::neural_rendering::retire_handle(eye.id);TickNrRetired();xrfg::neural_rendering::shutdown();need(live.empty(),"all model features retired once");
    unsigned errors{};if(gpu.messages)for(UINT64 i=0;i<gpu.messages->GetNumStoredMessages();++i){SIZE_T size{};gpu.messages->GetMessage(i,nullptr,&size);std::vector<char> b(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(b.data());hr(gpu.messages->GetMessage(i,m,&size));if(m->Severity==D3D12_MESSAGE_SEVERITY_ERROR||m->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION){++errors;std::printf("D3D12: %s\n",m->pDescription);}}
    need(!errors,"NR host chain has no D3D12 validation errors");std::filesystem::remove(folder/L"nvngx_dlssnr.dll");std::filesystem::remove(folder);
    std::printf("NR 2x %s: stereo histories, same half resolution, real GPU chaining/clamp, reset, fence retirement, OFF and refusal recovery PASS\n",before?"pre-SR":"post-SR");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"NR host FAIL: %s\n",e.what());return 1;}
