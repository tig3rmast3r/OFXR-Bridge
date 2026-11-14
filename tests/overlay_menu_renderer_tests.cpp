#include "xrfg/overlay_menu_renderer.hpp"
#include "xrfg/overlay_menu_input.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <d3d12sdklayers.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <barrier>
#include <unordered_set>
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks{};
void require(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
void hr(HRESULT result,const char* message){require(SUCCEEDED(result),message);}
unsigned width=128,height=96;
float density=1.F;
const DXGI_FORMAT formats[]{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
struct DrawData{ImU32 color{};unsigned calls{};};
struct LifecycleDraw {
    unsigned cycle{},calls{},callbacks{},creates{},updates{},destroys{};
    bool option{},consistent{true},combo_focused{};ImGuiContext* context{};
    std::unordered_set<int> atlas_ids;
};
void lifecycle_callback(const ImDrawList*,const ImDrawCmd* command) {
    auto& state=*static_cast<LifecycleDraw*>(command->UserCallbackData);
    ++state.callbacks;state.consistent&=ImGui::GetCurrentContext()==state.context;
}
void lifecycle_draw(void* opaque) {
    auto& state=*static_cast<LifecycleDraw*>(opaque);++state.calls;
    require(ImGui::GetIO().DisplaySize.x==128.F && ImGui::GetIO().DisplaySize.y==96.F,
        "supersampling preserves logical layout");
    require(ImGui::GetIO().DisplayFramebufferScale.x==density,
        "font rasterization uses physical framebuffer density");
    if(!state.context)state.context=ImGui::GetCurrentContext();
    state.consistent&=ImGui::GetCurrentContext()==state.context;
    auto* background=ImGui::GetBackgroundDrawList();
    background->AddCallback(lifecycle_callback,&state);
    background->AddCallback(ImDrawCallback_ResetRenderState,nullptr);
    background->AddRectFilled({8,8},{48,40},IM_COL32(255,0,0,255));
    background->AddText({8,52},IM_COL32_WHITE,"OFXR");
    auto* font=ImGui::GetFont();const float size=14.0F+float(state.cycle)*4.0F;
    auto* baked=font->GetFontBaked(size);
    for(unsigned glyph=32;glyph<127;++glyph)baked->FindGlyph(ImWchar(glyph));
    require(baked->FindGlyphNoFallback(0x4e2d)!=nullptr,"merged font rasterizes a real Chinese glyph");
    background->AddText(font,size,{8,72},IM_COL32_WHITE,"神经渲染 中文字体");
    ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({128,96});
    ImGui::Begin("Options lifecycle",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings);
    int route=int(state.cycle%3);ImGui::Combo("Runtime",&route,"Reference\0New\0Djules\0");
    ImGui::SetItemDefaultFocus();
    if(state.cycle==1){const auto& context=*ImGui::GetCurrentContext();state.combo_focused=context.NavWindow&&context.NavId==ImGui::GetItemID();}
    state.option=bool(state.cycle&1);ImGui::Checkbox("FG",&state.option);
    if(state.cycle>=2&&state.cycle%3==0)ImGui::OpenPopup("Option popup");
    if(ImGui::BeginPopup("Option popup")){ImGui::Text("Updated options");ImGui::EndPopup();}
    ImGui::End();
    for(const auto* texture:ImGui::GetIO().Fonts->TexList){
        state.atlas_ids.insert(texture->UniqueID);
        state.creates+=texture->Status==ImTextureStatus_WantCreate;
        state.updates+=texture->Status==ImTextureStatus_WantUpdates;
        state.destroys+=texture->Status==ImTextureStatus_WantDestroy;
    }
}
void reset_lifecycle(xrfg::OverlayMenuRenderer& renderer,const LifecycleDraw& draw) {
    auto* foreign=ImGui::GetCurrentContext();renderer.reset_input();
    require(ImGui::GetCurrentContext()==foreign,"close/reset restores foreign context");
    if(!draw.context)return;
    ImGui::SetCurrentContext(draw.context);const auto& state=*draw.context;
    const bool coherent=state.NavWindow||(!state.NavId&&!state.NavAnyRequest);
    ImGui::SetCurrentContext(foreign);
    require(coherent,"hidden menu has no orphan navigation ID or request");
}
struct ConcurrentDraw{std::atomic<unsigned>* active{};std::atomic<bool>* serialized{};};
void concurrent_draw(void* opaque){auto& data=*static_cast<ConcurrentDraw*>(opaque);const auto current=ImGui::GetCurrentContext();
    if(data.active->fetch_add(1)!=0)*data.serialized=false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if(ImGui::GetCurrentContext()!=current)*data.serialized=false;
    ImGui::GetBackgroundDrawList()->AddRectFilled({8,8},{48,40},IM_COL32_WHITE);data.active->fetch_sub(1);}
void draw(void* opaque){auto& data=*static_cast<DrawData*>(opaque);++data.calls;
    ImGui::GetBackgroundDrawList()->AddRectFilled({8,8},{48,40},data.color);
    ImGui::GetBackgroundDrawList()->AddText({8,52},IM_COL32_WHITE,"OFXR");}
xrfg::OverlayMenuInputFrame input(){xrfg::OverlayMenuInputFrame result{};result.open=result.focused=result.mouse_valid=true;
    result.mouse_x=68;result.mouse_y=44;return result;}
void pixels(const std::vector<unsigned char>& data,DXGI_FORMAT format,unsigned channel) {
    const bool bgra=format==DXGI_FORMAT_B8G8R8A8_UNORM||format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const auto actual=bgra&&channel!=1?2-channel:channel;
    const auto at=[&](unsigned x,unsigned y,unsigned c){return data[(unsigned(y*density)*width+unsigned(x*density))*4+c];};
    require(at(20,20,actual)>250&&at(20,20,3)>250,"actual ImGui primary-colour rectangle rendered");
    require(at(124,92,0)==0&&at(124,92,1)==0&&at(124,92,2)==0&&at(124,92,3)==0,"render target cleared transparent");
    unsigned cursor{};for(unsigned y=44;y<70;++y)for(unsigned x=68;x<91;++x)if(at(x,y,3)>0)++cursor;
    require(cursor>20,"actual software cursor rendered into headset texture");
    unsigned text{};for(unsigned y=52;y<68;++y)for(unsigned x=8;x<43;++x)if(at(x,y,3)>0)++text;
    require(text>20,"actual dynamic ImGui font atlas rendered");
}
void wait12(ID3D12Fence* fence,UINT64 value){auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event!=nullptr,"private event");
    hr(fence->SetEventOnCompletion(value,event),"private fence event");const auto waited=WaitForSingleObject(event,10000);CloseHandle(event);
    require(waited==WAIT_OBJECT_0&&fence->GetCompletedValue()!=UINT64_MAX,"private GPU fence retired");}
ComPtr<ID3DBlob> shader(const char* entry,const char* profile) {
    constexpr char code[]=R"(
struct P{float4 p:SV_POSITION;};
P vs(float4 p:POSITION){P o;o.p=p;return o;}
float4 ps():SV_TARGET{return float4(1,0,0,1);}
[maxvertexcount(1)]void gs(point P p[1],inout PointStream<P> outp){outp.Append(p[0]);}
struct C{float edges[3]:SV_TessFactor;float inside:SV_InsideTessFactor;};
C pc(InputPatch<P,3> p){C o;o.edges[0]=o.edges[1]=o.edges[2]=o.inside=1;return o;}
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")][outputcontrolpoints(3)][patchconstantfunc("pc")]
P hs(InputPatch<P,3> p,uint i:SV_OutputControlPointID){return p[i];}
[domain("tri")]P ds(C c,float3 uv:SV_DomainLocation,const OutputPatch<P,3> p){P o;o.p=p[0].p;return o;}
[numthreads(1,1,1)]void cs(){}
)";
    ComPtr<ID3DBlob> result,errors;const auto compiled=D3DCompile(code,sizeof(code),nullptr,nullptr,nullptr,entry,profile,0,0,&result,&errors);
    if(FAILED(compiled)&&errors)fprintf(stderr,"%s\n",static_cast<const char*>(errors->GetBufferPointer()));hr(compiled,"state fixture shader");return result;
}
void d11(bool single_threaded=false) {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
    const UINT flags=D3D11_CREATE_DEVICE_DEBUG|(single_threaded?D3D11_CREATE_DEVICE_SINGLETHREADED:0);
    hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,flags,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context),"WARP D11 debug device");
    require(bool(device->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED)==single_threaded,"D11 fixture exercises the requested real device threading mode");
    ComPtr<ID3D11Device5> device5;ComPtr<ID3D11DeviceContext4> context4;ComPtr<ID3D11Fence> fence;
    hr(device.As(&device5),"D11 device5");hr(context.As(&context4),"D11 context4");hr(device5->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"D11 caller fence");
    ComPtr<ID3D11InfoQueue> debug;device.As(&debug);debug->ClearStoredMessages();
    const auto v=shader("vs","vs_5_0"),p=shader("ps","ps_5_0"),g=shader("gs","gs_5_0"),h=shader("hs","hs_5_0"),d=shader("ds","ds_5_0"),c=shader("cs","cs_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;ComPtr<ID3D11ComputeShader> cs;
    hr(device->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs),"VS");hr(device->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps),"PS");
    hr(device->CreateGeometryShader(g->GetBufferPointer(),g->GetBufferSize(),nullptr,&gs),"GS");hr(device->CreateHullShader(h->GetBufferPointer(),h->GetBufferSize(),nullptr,&hs),"HS");
    hr(device->CreateDomainShader(d->GetBufferPointer(),d->GetBufferSize(),nullptr,&ds),"DS");hr(device->CreateComputeShader(c->GetBufferPointer(),c->GetBufferSize(),nullptr,&cs),"CS");
    context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->GSSetShader(gs.Get(),nullptr,0);
    context->HSSetShader(hs.Get(),nullptr,0);context->DSSetShader(ds.Get(),nullptr,0);context->CSSetShader(cs.Get(),nullptr,0);
    ComPtr<ID3D11Buffer> buffer;D3D11_BUFFER_DESC bd{};bd.ByteWidth=64;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER;
    hr(device->CreateBuffer(&bd,nullptr,&buffer),"game IA buffer");auto* vb=buffer.Get();UINT stride=16,offset=8;
    context->IASetVertexBuffers(0,1,&vb,&stride,&offset);context->IASetIndexBuffer(vb,DXGI_FORMAT_R32_UINT,4);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ComPtr<ID3D11InputLayout> layout;D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    hr(device->CreateInputLayout(&element,1,v->GetBufferPointer(),v->GetBufferSize(),&layout),"game IA layout");context->IASetInputLayout(layout.Get());
    ComPtr<ID3D11Buffer> constant;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;hr(device->CreateBuffer(&bd,nullptr,&constant),"game constant");auto* cb=constant.Get();context->VSSetConstantBuffers(0,1,&cb);
    D3D11_VIEWPORT viewport{1,2,72,51,.2F,.8F};D3D11_RECT scissor{3,4,37,41};context->RSSetViewports(1,&viewport);context->RSSetScissorRects(1,&scissor);
    ComPtr<ID3D11RasterizerState> rasterizer;D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_WIREFRAME;rd.CullMode=D3D11_CULL_FRONT;rd.DepthClipEnable=TRUE;rd.ScissorEnable=TRUE;
    hr(device->CreateRasterizerState(&rd,&rasterizer),"game RS");context->RSSetState(rasterizer.Get());
    ComPtr<ID3D11BlendState> blend;D3D11_BLEND_DESC blend_desc{};blend_desc.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    hr(device->CreateBlendState(&blend_desc,&blend),"game blend");const float blend_factor[]{.1F,.2F,.3F,.4F};context->OMSetBlendState(blend.Get(),blend_factor,0x12345678);
    ComPtr<ID3D11DepthStencilState> depth;D3D11_DEPTH_STENCIL_DESC depth_desc{};depth_desc.DepthEnable=TRUE;depth_desc.DepthFunc=D3D11_COMPARISON_GREATER;depth_desc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    hr(device->CreateDepthStencilState(&depth_desc,&depth),"game depth state");context->OMSetDepthStencilState(depth.Get(),37);
    D3D11_TEXTURE2D_DESC td{};td.Width=width;td.Height=height;td.ArraySize=td.MipLevels=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;ComPtr<ID3D11Texture2D> game_texture;hr(device->CreateTexture2D(&td,nullptr,&game_texture),"game texture");
    ComPtr<ID3D11RenderTargetView> game_rtv;hr(device->CreateRenderTargetView(game_texture.Get(),nullptr,&game_rtv),"game RTV");auto* game_target=game_rtv.Get();context->OMSetRenderTargets(1,&game_target,nullptr);
    ComPtr<ID3D11ShaderResourceView> srv;hr(device->CreateShaderResourceView(game_texture.Get(),nullptr,&srv),"game SRV");auto* sv=srv.Get();context->PSSetShaderResources(3,1,&sv);
    ComPtr<ID3D11SamplerState> sampler;D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    hr(device->CreateSamplerState(&sd,&sampler),"game sampler");auto* sp=sampler.Get();context->PSSetSamplers(0,1,&sp);
    // Clear the intentional initial SRV/RT hazard; use a different input texture.
    sv=nullptr;context->PSSetShaderResources(3,1,&sv);ComPtr<ID3D11Texture2D> input_texture;
    td.BindFlags=D3D11_BIND_SHADER_RESOURCE;hr(device->CreateTexture2D(&td,nullptr,&input_texture),"game input texture");srv.Reset();hr(device->CreateShaderResourceView(input_texture.Get(),nullptr,&srv),"game input SRV");sv=srv.Get();context->PSSetShaderResources(0,1,&sv);debug->ClearStoredMessages();
    UINT64 serial=0;
    for(auto format:formats) {
        xrfg::OverlayMenuRenderer renderer;hr(renderer.initialize(nullptr,nullptr,device.Get(),format,1,width,height,density),"D11 renderer init");
        td.Format=format;td.BindFlags=D3D11_BIND_RENDER_TARGET;ComPtr<ID3D11Texture2D> texture;hr(device->CreateTexture2D(&td,nullptr,&texture),"D11 menu texture");
        const auto previous=ImGui::GetCurrentContext();DrawData drawing{IM_COL32(255,0,0,255)};
        hr(renderer.render_d3d11(texture.Get(),0,input(),draw,&drawing),"D11 real isolated-state draw");require(ImGui::GetCurrentContext()==previous,"D11 restores separate ImGui context");
        ComPtr<ID3D11VertexShader> after_vs;ComPtr<ID3D11PixelShader> after_ps;ComPtr<ID3D11GeometryShader> after_gs;
        ComPtr<ID3D11HullShader> after_hs;ComPtr<ID3D11DomainShader> after_ds;ComPtr<ID3D11ComputeShader> after_cs;
        context->VSGetShader(&after_vs,nullptr,nullptr);context->PSGetShader(&after_ps,nullptr,nullptr);context->GSGetShader(&after_gs,nullptr,nullptr);
        context->HSGetShader(&after_hs,nullptr,nullptr);context->DSGetShader(&after_ds,nullptr,nullptr);context->CSGetShader(&after_cs,nullptr,nullptr);
        require(after_vs==vs&&after_ps==ps&&after_gs==gs&&after_hs==hs&&after_ds==ds&&after_cs==cs,"D11 restores all six shader stages including HS DS CS");
        ComPtr<ID3D11Buffer> after_vb,after_ib,after_cb;UINT after_stride{},after_offset{},index_offset{};DXGI_FORMAT index_format{};D3D11_PRIMITIVE_TOPOLOGY topology{};
        context->IAGetVertexBuffers(0,1,&after_vb,&after_stride,&after_offset);context->IAGetIndexBuffer(&after_ib,&index_format,&index_offset);context->VSGetConstantBuffers(0,1,&after_cb);context->IAGetPrimitiveTopology(&topology);
        ComPtr<ID3D11InputLayout> after_layout;context->IAGetInputLayout(&after_layout);
        require(after_vb==buffer&&after_ib==buffer&&after_cb==constant&&after_layout==layout&&after_stride==16&&after_offset==8&&index_offset==4&&index_format==DXGI_FORMAT_R32_UINT&&topology==D3D11_PRIMITIVE_TOPOLOGY_LINELIST,"D11 restores IA and constant buffer state");
        D3D11_VIEWPORT after_view{};D3D11_RECT after_scissor{};UINT count=1;context->RSGetViewports(&count,&after_view);count=1;context->RSGetScissorRects(&count,&after_scissor);
        ComPtr<ID3D11RasterizerState> after_rs;context->RSGetState(&after_rs);require(!memcmp(&viewport,&after_view,sizeof(viewport))&&!memcmp(&scissor,&after_scissor,sizeof(scissor))&&after_rs==rasterizer,"D11 restores viewport scissor rasterizer");
        ComPtr<ID3D11RenderTargetView> after_rt;ComPtr<ID3D11BlendState> after_blend;ComPtr<ID3D11DepthStencilState> after_depth;FLOAT after_factor[4]{};UINT mask{},stencil{};
        context->OMGetRenderTargets(1,&after_rt,nullptr);context->OMGetBlendState(&after_blend,after_factor,&mask);context->OMGetDepthStencilState(&after_depth,&stencil);
        require(after_rt==game_rtv&&after_blend==blend&&after_depth==depth&&!memcmp(blend_factor,after_factor,sizeof(blend_factor))&&mask==0x12345678&&stencil==37,"D11 restores complete OM target blend depth state");
        ComPtr<ID3D11ShaderResourceView> after_srv;ComPtr<ID3D11SamplerState> after_sampler;context->PSGetShaderResources(0,1,&after_srv);context->PSGetSamplers(0,1,&after_sampler);require(after_srv==srv&&after_sampler==sampler,"D11 restores PS texture and sampler");
        require(renderer.shutdown()==E_PENDING,"unreceipted D11 execution cannot destroy backend GPU objects");
        hr(context4->Signal(fence.Get(),++serial),"caller D11 signal");hr(renderer.submitted_d3d11(0,fence.Get(),serial),"D11 exact receipt");context->Flush();
        auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);hr(fence->SetEventOnCompletion(serial,event),"D11 fence event");require(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"D11 fence completion");CloseHandle(event);
        td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;hr(device->CreateTexture2D(&td,nullptr,&staging),"D11 staging");context->CopyResource(staging.Get(),texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};hr(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"D11 output readback");std::vector<unsigned char> data(width*height*4);
        for(unsigned y=0;y<height;++y)memcpy(data.data()+y*width*4,static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch,width*4);context->Unmap(staging.Get(),0);pixels(data,format,0);
        LifecycleDraw lifecycle;
        for(unsigned cycle=0;cycle<14;++cycle){
            lifecycle.cycle=cycle;if(cycle>=2)reset_lifecycle(renderer,lifecycle);require(ImGui::GetCurrentContext()==previous,"D11 close/reset restores foreign context");
            hr(renderer.render_d3d11(texture.Get(),0,input(),lifecycle_draw,&lifecycle),"D11 reopen with changed options and dynamic atlas");
            if(cycle>=2)reset_lifecycle(renderer,lifecycle);require(renderer.shutdown()==E_PENDING,"D11 close cannot retire an unreceipted reopen draw");
            hr(context4->Signal(fence.Get(),++serial),"D11 reopened caller signal");hr(renderer.submitted_d3d11(0,fence.Get(),serial),"D11 reopened exact receipt");context->Flush();
            auto reopened_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);hr(fence->SetEventOnCompletion(serial,reopened_event),"D11 reopened event");
            require(WaitForSingleObject(reopened_event,10000)==WAIT_OBJECT_0,"D11 reopened GPU retired");CloseHandle(reopened_event);
        }
        require(lifecycle.combo_focused,"D11 reopen regression starts from a real focused Combo widget");
        require(lifecycle.calls==14&&lifecycle.callbacks==14&&lifecycle.consistent,"D11 warm navigation plus twelve close/reset/reopen callbacks retain private context");
        require(lifecycle.creates&&lifecycle.updates&&lifecycle.atlas_ids.size()>1,"D11 reopen covers atlas creation update and replacement");
        hr(renderer.shutdown(),"D11 retired shutdown");td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;
    }
    for(UINT64 i=0;i<debug->GetNumStoredMessages();++i){SIZE_T size{};debug->GetMessage(i,nullptr,&size);std::vector<char> bytes(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());debug->GetMessage(i,message,&size);
        require(message->Severity!=D3D11_MESSAGE_SEVERITY_ERROR&&message->Severity!=D3D11_MESSAGE_SEVERITY_CORRUPTION,"zero D11 validation errors");}
}
void d12() {
    ComPtr<ID3D12Debug> debug;hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"D12 debug");debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"D12 WARP adapter");
    ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"WARP D12 device");
    ComPtr<ID3D12InfoQueue> validation;device.As(&validation);validation->ClearStoredMessages();
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"D12 queue");
    ComPtr<ID3D12Fence> fence,blocker;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"caller D12 fence");hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&blocker)),"private queue blocker");
    UINT64 serial=0;
    for(auto format:formats) {
        xrfg::OverlayMenuRenderer renderer;hr(renderer.initialize(device.Get(),queue.Get(),nullptr,format,3,width,height,density),"D12 renderer init");
        std::array<ComPtr<ID3D12Resource>,3> textures;std::array<ComPtr<ID3D12CommandAllocator>,3> allocators;std::array<ComPtr<ID3D12GraphicsCommandList>,3> lists;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=format;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        for(unsigned i=0;i<3;++i){hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&textures[i])),"D12 menu texture");hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocators[i])),"private allocator");hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[i].Get(),nullptr,IID_PPV_ARGS(&lists[i])),"private list");}
        hr(queue->Wait(blocker.Get(),serial+1),"hold private queue to verify immutable GPU lag");
        const unsigned order[]{2,0,1};const ImU32 colours[]{IM_COL32(255,0,0,255),IM_COL32(0,255,0,255),IM_COL32(0,0,255,255)};
        std::array<unsigned,3> channel{};
        for(unsigned n=0;n<3;++n){const auto i=order[n];DrawData drawing{colours[n]};const auto previous=ImGui::GetCurrentContext();
            hr(renderer.render_d3d12(textures[i].Get(),lists[i].Get(),i,input(),draw,&drawing),"D12 actual immutable draw");require(ImGui::GetCurrentContext()==previous,"D12 restores separate ImGui context");
            hr(lists[i]->Close(),"D12 draw close");ID3D12CommandList* submitted[]{lists[i].Get()};queue->ExecuteCommandLists(1,submitted);hr(queue->Signal(fence.Get(),++serial),"caller exact signal");hr(renderer.submitted_d3d12(i,fence.Get(),serial),"exact image submission receipt");channel[i]=n;
        }
        renderer.reset_input();
        DrawData skipped{IM_COL32_WHITE};require(renderer.render_d3d12(textures[2].Get(),lists[2].Get(),2,input(),draw,&skipped)==E_PENDING&&skipped.calls==0,"GPU-busy image refuses after close/reset before consuming input or draw callback");require(renderer.shutdown()==E_PENDING,"GPU-busy shutdown retains immutable atlas mesh descriptors");
        hr(blocker->Signal(serial),"release private queue blocker");wait12(fence.Get(),serial);
        for(unsigned i=0;i<3;++i){hr(allocators[i]->Reset(),"retired allocator reset");hr(lists[i]->Reset(allocators[i].Get(),nullptr),"retired readback list");
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={textures[i].Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};lists[i]->ResourceBarrier(1,&barrier);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
            D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES readback{};readback.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> result;hr(device->CreateCommittedResource(&readback,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&result)),"D12 readback buffer");
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=textures[i].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=result.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;lists[i]->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);lists[i]->ResourceBarrier(1,&barrier);hr(lists[i]->Close(),"readback close");ID3D12CommandList* submitted[]{lists[i].Get()};queue->ExecuteCommandLists(1,submitted);hr(queue->Signal(fence.Get(),++serial),"readback signal");wait12(fence.Get(),serial);
            void* mapped{};D3D12_RANGE range{0,SIZE_T(bytes)};hr(result->Map(0,&range,&mapped),"D12 readback map");std::vector<unsigned char> data(width*height*4);for(unsigned y=0;y<height;++y)memcpy(data.data()+y*width*4,static_cast<unsigned char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch,width*4);D3D12_RANGE empty{};result->Unmap(0,&empty);pixels(data,format,channel[i]);
        }
        LifecycleDraw lifecycle;
        for(unsigned cycle=0;cycle<14;++cycle){const auto i=order[cycle%3];lifecycle.cycle=cycle;
            hr(allocators[i]->Reset(),"reopen allocator reset");hr(lists[i]->Reset(allocators[i].Get(),nullptr),"reopen private list reset");
            const auto previous=ImGui::GetCurrentContext();if(cycle>=2)reset_lifecycle(renderer,lifecycle);require(ImGui::GetCurrentContext()==previous,"D12 close/reset restores foreign context");
            hr(renderer.render_d3d12(textures[i].Get(),lists[i].Get(),i,input(),lifecycle_draw,&lifecycle),"D12 reopen with changed options and dynamic atlas");
            hr(lists[i]->Close(),"D12 reopened close");ID3D12CommandList* submitted[]{lists[i].Get()};queue->ExecuteCommandLists(1,submitted);
            if(cycle>=2)reset_lifecycle(renderer,lifecycle);require(renderer.shutdown()==E_PENDING,"D12 close cannot retire an unreceipted reopen draw");
            hr(queue->Signal(fence.Get(),++serial),"D12 reopened caller signal");hr(renderer.submitted_d3d12(i,fence.Get(),serial),"D12 reopened exact receipt");wait12(fence.Get(),serial);
        }
        require(lifecycle.combo_focused,"D12 reopen regression starts from a real focused Combo widget");
        require(lifecycle.calls==14&&lifecycle.callbacks==14&&lifecycle.consistent,"D12 warm navigation plus twelve close/reset/reopen callbacks retain private context");
        require(lifecycle.creates&&lifecycle.updates&&lifecycle.atlas_ids.size()>1,"D12 reopen covers atlas creation update and replacement");
        hr(allocators[2]->Reset(),"abandon allocator reset");hr(lists[2]->Reset(allocators[2].Get(),nullptr),"abandon list reset");DrawData abandoned{IM_COL32_WHITE};hr(renderer.render_d3d12(textures[2].Get(),lists[2].Get(),2,input(),draw,&abandoned),"unsubmitted immutable draw");require(renderer.shutdown()==E_PENDING,"unsubmitted draw cannot be implicitly freed");hr(lists[2]->Close(),"discard list close");hr(allocators[2]->Reset(),"discard unused allocator");hr(lists[2]->Reset(allocators[2].Get(),nullptr),"discard unused recording");hr(renderer.abandon_d3d12(2),"explicit never-executed draw abandonment");hr(lists[2]->Close(),"empty list close");hr(renderer.shutdown(),"D12 retired shutdown");
    }
    {
        std::array<xrfg::OverlayMenuRenderer,2> renderers;
        std::array<ComPtr<ID3D12Resource>,2> textures;std::array<ComPtr<ID3D12CommandAllocator>,2> allocators;
        std::array<ComPtr<ID3D12GraphicsCommandList>,2> lists;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;
        desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        for(unsigned i=0;i<2;++i){hr(renderers[i].initialize(device.Get(),queue.Get(),nullptr,desc.Format,1,width,height,density),"concurrent renderer init");
            hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&textures[i])),"concurrent target");
            hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocators[i])),"concurrent allocator");
            hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[i].Get(),nullptr,IID_PPV_ARGS(&lists[i])),"concurrent list");}
        std::atomic<unsigned> active{};std::atomic<bool> serialized{true};ConcurrentDraw drawing{&active,&serialized};
        std::barrier start(2);std::array<HRESULT,2> results{};std::array<std::thread,2> workers;
        for(unsigned i=0;i<2;++i)workers[i]=std::thread([&,i]{start.arrive_and_wait();results[i]=renderers[i].render_d3d12(textures[i].Get(),lists[i].Get(),0,input(),concurrent_draw,&drawing);});
        for(auto& worker:workers)worker.join();
        require(results[0]==S_OK&&results[1]==S_OK&&serialized&&active==0,"two XR sessions serialize private contexts across different application threads");
        for(unsigned i=0;i<2;++i){hr(lists[i]->Close(),"concurrent discard close");hr(allocators[i]->Reset(),"concurrent discarded allocator reset");
            hr(lists[i]->Reset(allocators[i].Get(),nullptr),"concurrent discard recording");hr(renderers[i].abandon_d3d12(0),"concurrent exact abandon");
            hr(lists[i]->Close(),"concurrent empty close");renderers[i].reset_input();hr(renderers[i].shutdown(),"concurrent retired shutdown");}
    }
    for(UINT64 i=0;i<validation->GetNumStoredMessages();++i){SIZE_T size{};validation->GetMessage(i,nullptr,&size);std::vector<char> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());validation->GetMessage(i,message,&size);
        require(message->Severity!=D3D12_MESSAGE_SEVERITY_ERROR&&message->Severity!=D3D12_MESSAGE_SEVERITY_CORRUPTION,"zero D12 validation errors");}
}
}
int main(int argc,char** argv){
    if(argc>3||(argc>=2&&strcmp(argv[1],"d11")&&strcmp(argv[1],"d12"))){fprintf(stderr,"Expected d11 or d12 (or no argument for both)\n");return 2;}
    if(argc==3){density=2.F;width*=2;height*=2;}
    auto* foreign=ImGui::CreateContext();ImGui::GetIO().IniFilename=nullptr;try{
    const bool both=argc<2;
    if(both||!strcmp(argv[1],"d11")){
        auto before=checks;d11();printf("D3D11 WARP draw/state/retirement: %u checks passed\n",checks-before);
        before=checks;d11(true);printf("D3D11 SINGLETHREADED WARP draw/state/retirement: %u checks passed\n",checks-before);
    }
    if(both||!strcmp(argv[1],"d12")){const auto before=checks;d12();printf("D3D12 WARP draw/immutable lifetime/concurrency: %u checks passed\n",checks-before);}
    require(ImGui::GetCurrentContext()==foreign,"foreign ImGui context survives both renderer APIs");
    ImGui::DestroyContext(foreign);printf("Overlay menu renderer WARP: %u checks passed\n",checks);return 0;
}catch(const std::exception& error){fprintf(stderr,"FAIL after %u checks: %s\n",checks,error.what());return 1;}}
