// Immutable D3D12 draw extraction from accepted OptiScaler V33034
// 6c36e598b9027a852ce2b759a1851378468a0938, InUpscalerDx12Renderer.h.
// Original licence: src/neural/licenses/OptiScaler-LICENSE. Dear ImGui sources
// and D11 backend retain their own MIT notice in src/ui/vendor/imgui/LICENSE.txt.
// V441 replaces borrowed game-list state/hooks with a caller-private RT list
// and exact per-image submission receipts. No sequential upload ring is used.
#include "xrfg/overlay_menu_renderer.hpp"
#include "xrfg/settings_widgets.hpp"
#include "xrfg/overlay_menu_input.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dx11.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cfloat>
#include <mutex>
#include <chrono>
#include <cmath>

namespace xrfg {
namespace {
// Vendored ImGui's GImGui is process-global within this module, not TLS.
// Different OpenXR sessions may draw on different application threads.
std::recursive_mutex& imgui_context_mutex(){static auto* mutex=new std::recursive_mutex;return *mutex;}
bool compatible_color_format(DXGI_FORMAT requested,DXGI_FORMAT backing) noexcept {
    if(requested==DXGI_FORMAT_R8G8B8A8_UNORM||requested==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        return backing==DXGI_FORMAT_R8G8B8A8_UNORM||backing==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||backing==DXGI_FORMAT_R8G8B8A8_TYPELESS;
    return (requested==DXGI_FORMAT_B8G8R8A8_UNORM||requested==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
        (backing==DXGI_FORMAT_B8G8R8A8_UNORM||backing==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB||backing==DXGI_FORMAT_B8G8R8A8_TYPELESS);
}
class ImmutableMenuDraw12
{
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    struct Image { int width{}, height{}; std::vector<unsigned char> rgba; };
    struct Texture { ComPtr<ID3D12Resource> image, upload; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; };
    struct Draw
    {
        ComPtr<ID3D12Resource> vertices, indices, output;
        ComPtr<ID3D12DescriptorHeap> srv, rtv;
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> pipeline;
        std::vector<Texture> textures;
        std::unordered_map<ImTextureID, UINT> textureIndices;
    };
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3DBlob> vs_, ps_;
    std::unordered_map<DXGI_FORMAT, ComPtr<ID3D12PipelineState>> pipelines_;
    std::unordered_map<ImTextureID, Image> images_;
    ImTextureID nextTexture_{1};
    bool initialized_{};

    static ComPtr<IUnknown> Identity(IUnknown* object)
    {
        // Streamline forwards resource/list methods but can return a proxy device.
        // Compare native COM identity, not the address of two different wrappers.
        constexpr GUID nativeGuid{0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};
        ComPtr<IUnknown> native, identity;
        if (SUCCEEDED(object->QueryInterface(nativeGuid, reinterpret_cast<void**>(native.GetAddressOf()))))
            object=native.Get();
        object->QueryInterface(IID_PPV_ARGS(&identity));
        return identity;
    }
    bool Owns(ID3D12DeviceChild* child) const
    {
        ComPtr<ID3D12Device> device;
        if (FAILED(child->GetDevice(IID_PPV_ARGS(&device)))) return false;
        const auto expected=Identity(device_.Get()), actual=Identity(device.Get());
        return expected && actual && expected.Get()==actual.Get();
    }
    static D3D12_HEAP_PROPERTIES Heap(D3D12_HEAP_TYPE type)
    {
        D3D12_HEAP_PROPERTIES h{}; h.Type=type; h.CreationNodeMask=h.VisibleNodeMask=1; return h;
    }
    bool Buffer(UINT64 bytes, ComPtr<ID3D12Resource>& resource)
    {
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width=bytes; d.Height=1; d.DepthOrArraySize=1; d.MipLevels=1;
        d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto h=Heap(D3D12_HEAP_TYPE_UPLOAD);
        return SUCCEEDED(device_->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,
            D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&resource)));
    }
    bool TextureResource(UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                         D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& resource)
    {
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=width; d.Height=height; d.DepthOrArraySize=1; d.MipLevels=1;
        d.Format=format; d.SampleDesc.Count=1; d.Flags=flags;
        const auto h=Heap(D3D12_HEAP_TYPE_DEFAULT);
        return SUCCEEDED(device_->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,state,
            nullptr,IID_PPV_ARGS(&resource)));
    }
    bool Pipeline(DXGI_FORMAT format, Draw& draw)
    {
        if (!root_)
        {
            constexpr char shader[]=R"(
cbuffer Projection : register(b0) { float4x4 projectionMatrix; };
struct V { float2 pos:POSITION; float2 uv:TEXCOORD0; float4 color:COLOR0; };
struct P { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; float4 color:COLOR0; };
P VS(V v) { P p; p.pos=mul(projectionMatrix,float4(v.pos,0,1)); p.uv=v.uv; p.color=v.color; return p; }
Texture2D atlas:register(t0); SamplerState sampleAtlas:register(s0);
float4 PS(P p):SV_Target { return p.color*atlas.Sample(sampleAtlas,p.uv); }
)";
            if (FAILED(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"VS","vs_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs_,nullptr)) ||
                FAILED(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"PS","ps_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps_,nullptr))) return false;
            D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
            D3D12_ROOT_PARAMETER p[2]{};
            p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            p[0].Constants={0,0,16}; p[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
            p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[1].DescriptorTable={1,&range}; p[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxLOD=FLT_MAX;
            sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC desc{2,p,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
            ComPtr<ID3DBlob> blob;
            if (FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr)) ||
                FAILED(device_->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),
                    IID_PPV_ARGS(&root_)))) return false;
        }
        auto& pipeline=pipelines_[format];
        if (!pipeline)
        {
            D3D12_INPUT_ELEMENT_DESC layout[]={
                {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,(UINT)offsetof(ImDrawVert,pos),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
                {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,(UINT)offsetof(ImDrawVert,uv),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
                {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,(UINT)offsetof(ImDrawVert,col),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
            D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
            p.pRootSignature=root_.Get(); p.VS={vs_->GetBufferPointer(),vs_->GetBufferSize()};
            p.PS={ps_->GetBufferPointer(),ps_->GetBufferSize()}; p.InputLayout={layout,3};
            p.SampleMask=UINT_MAX; p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            p.NumRenderTargets=1; p.RTVFormats[0]=format; p.SampleDesc.Count=1;
            auto& blend=p.BlendState.RenderTarget[0]; blend.BlendEnable=TRUE;
            blend.SrcBlend=D3D12_BLEND_SRC_ALPHA; blend.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;
            blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha=D3D12_BLEND_ONE; blend.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;
            blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
            p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
            p.RasterizerState.DepthClipEnable=TRUE;
            p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
            p.DepthStencilState.FrontFace.StencilFunc=p.DepthStencilState.BackFace.StencilFunc=D3D12_COMPARISON_FUNC_ALWAYS;
            p.DepthStencilState.FrontFace.StencilFailOp=p.DepthStencilState.FrontFace.StencilDepthFailOp=
                p.DepthStencilState.FrontFace.StencilPassOp=D3D12_STENCIL_OP_KEEP;
            p.DepthStencilState.BackFace=p.DepthStencilState.FrontFace;
            if (FAILED(device_->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&pipeline)))) return false;
        }
        draw.root=root_; draw.pipeline=pipeline; return true;
    }
    bool UpdateImages(ImDrawData* data)
    {
        if (!data->Textures) return true;
        for (auto* tex:*data->Textures)
        {
            if (tex->Status==ImTextureStatus_WantDestroy)
            {
                images_.erase(tex->GetTexID()); tex->SetTexID(ImTextureID_Invalid);
                tex->BackendUserData=nullptr; tex->SetStatus(ImTextureStatus_Destroyed); continue;
            }
            if (tex->Status!=ImTextureStatus_WantCreate && tex->Status!=ImTextureStatus_WantUpdates) continue;
            if (!tex->Pixels || tex->Width<=0 || tex->Height<=0) return false;
            if (tex->Status==ImTextureStatus_WantCreate)
            {
                tex->SetTexID(nextTexture_++);
                tex->BackendUserData=this;
            }
            Image image; image.width=tex->Width; image.height=tex->Height;
            const auto pixels=(size_t)image.width*image.height;
            image.rgba.resize(pixels*4);
            if (tex->Format==ImTextureFormat_RGBA32) std::memcpy(image.rgba.data(),tex->Pixels,pixels*4);
            else if (tex->Format==ImTextureFormat_Alpha8)
                for(size_t i=0;i<pixels;++i) {
                    image.rgba[i*4]=image.rgba[i*4+1]=image.rgba[i*4+2]=255; image.rgba[i*4+3]=tex->Pixels[i];
                }
            else return false;
            images_[tex->GetTexID()]=std::move(image); tex->SetStatus(ImTextureStatus_OK);
        }
        return true;
    }
    static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to}; list->ResourceBarrier(1,&b);
    }
public:
    explicit ImmutableMenuDraw12(ID3D12Device* device) : device_(device) {}
    bool Init()
    {
        if (initialized_) return true;
        auto& io=ImGui::GetIO();
        if (io.BackendRendererUserData) return false;
        io.BackendRendererUserData=this; io.BackendRendererName="OFXR_Bridge_immutable_DX12";
        io.BackendFlags|=ImGuiBackendFlags_RendererHasVtxOffset|ImGuiBackendFlags_RendererHasTextures;
        initialized_=true; return true;
    }
    ~ImmutableMenuDraw12()
    {
        if (!initialized_ || !ImGui::GetCurrentContext()) return;
        auto& io=ImGui::GetIO();
        if(io.BackendRendererUserData!=this) return;
        for (auto* tex:ImGui::GetPlatformIO().Textures)
            if(tex->BackendUserData==this) {
                tex->SetTexID(ImTextureID_Invalid); tex->BackendUserData=nullptr; tex->SetStatus(ImTextureStatus_Destroyed);
            }
        io.BackendRendererUserData=nullptr; io.BackendRendererName=nullptr;
        io.BackendFlags&=~(ImGuiBackendFlags_RendererHasVtxOffset|ImGuiBackendFlags_RendererHasTextures);
        // Draw snapshots hold all GPU references beyond the UI/context's lifetime.
    }
    bool Render(ImDrawData* data, ID3D12GraphicsCommandList* list, ID3D12Resource* output, DXGI_FORMAT format,
                std::shared_ptr<void>& retained)
    {
        if (!data || !list || !output || !Owns(list) || !Owns(output)) return false;

        const auto desc=output->GetDesc();
        if (desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize!=1 || desc.SampleDesc.Count!=1 ||
            data->DisplaySize.x<=0 || data->DisplaySize.y<=0) return false;
        if (!(desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || !UpdateImages(data)) return false;
        if (data->TotalVtxCount<=0 || data->TotalIdxCount<=0) {
            auto draw=std::make_shared<Draw>(); draw->output=output;
            D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=1;
            if(FAILED(device_->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&draw->rtv))))return false;
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};rtv.Format=format;rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
            device_->CreateRenderTargetView(output,&rtv,draw->rtv->GetCPUDescriptorHandleForHeapStart());
            retained=draw;const float transparent[4]{};list->ClearRenderTargetView(draw->rtv->GetCPUDescriptorHandleForHeapStart(),transparent,0,nullptr);return true;
        }
        auto draw=std::make_shared<Draw>(); draw->output=output;
        if (!Pipeline(format,*draw) || !Buffer((UINT64)data->TotalVtxCount*sizeof(ImDrawVert),draw->vertices) ||
            !Buffer((UINT64)data->TotalIdxCount*sizeof(ImDrawIdx),draw->indices)) return false;
        draw->vertices->SetName(L"OFXR-BRIDGE V449 immutable menu vertices");
        draw->indices->SetName(L"OFXR-BRIDGE V449 immutable menu indices");
        void* vertices=nullptr; void* indices=nullptr; D3D12_RANGE empty{0,0};
        if (FAILED(draw->vertices->Map(0,&empty,&vertices))) return false;
        if (FAILED(draw->indices->Map(0,&empty,&indices))) { draw->vertices->Unmap(0,nullptr); return false; }
        auto* v=(ImDrawVert*)vertices; auto* i=(ImDrawIdx*)indices;
        for (const auto* commands:data->CmdLists) {
            std::memcpy(v,commands->VtxBuffer.Data,commands->VtxBuffer.Size*sizeof(ImDrawVert)); v+=commands->VtxBuffer.Size;
            std::memcpy(i,commands->IdxBuffer.Data,commands->IdxBuffer.Size*sizeof(ImDrawIdx)); i+=commands->IdxBuffer.Size;
            for (const auto& cmd:commands->CmdBuffer) if (!cmd.UserCallback && cmd.ElemCount)
                draw->textureIndices.emplace(cmd.GetTexID(),(UINT)draw->textureIndices.size());
        }
        draw->vertices->Unmap(0,nullptr); draw->indices->Unmap(0,nullptr);
        if (draw->textureIndices.empty()) return true;
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors=(UINT)draw->textureIndices.size(); heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&draw->srv)))) return false;
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors=1; heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device_->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&draw->rtv)))) return false;
        D3D12_RENDER_TARGET_VIEW_DESC rtv{}; rtv.Format=format; rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
        device_->CreateRenderTargetView(output,&rtv,draw->rtv->GetCPUDescriptorHandleForHeapStart());
        draw->textures.resize(draw->textureIndices.size());
        const auto increment=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for(const auto& [id,index]:draw->textureIndices)
        {
            const auto found=images_.find(id); if(found==images_.end()) return false;
            const auto& image=found->second; auto& texture=draw->textures[index];
            if(!TextureResource(image.width,image.height,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_COMMON,texture.image)) return false;
            texture.image->SetName(L"OFXR-BRIDGE V449 immutable menu atlas");
            const auto td=texture.image->GetDesc(); UINT64 bytes=0;
            device_->GetCopyableFootprints(&td,0,1,0,&texture.footprint,nullptr,nullptr,&bytes);
            if(!Buffer(bytes,texture.upload)) return false;
            void* pixels=nullptr; if(FAILED(texture.upload->Map(0,&empty,&pixels))) return false;
            for(int row=0;row<image.height;++row)
                std::memcpy((char*)pixels+texture.footprint.Offset+(size_t)row*texture.footprint.Footprint.RowPitch,
                            image.rgba.data()+(size_t)row*image.width*4,(size_t)image.width*4);
            texture.upload->Unmap(0,nullptr);
            auto cpu=draw->srv->GetCPUDescriptorHandleForHeapStart(); cpu.ptr+=(SIZE_T)index*increment;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels=1; device_->CreateShaderResourceView(texture.image.Get(),&srv,cpu);
        }
        // All allocations/maps succeed before the first command is recorded.
        retained=draw;
        const float transparent[4]{};
        list->ClearRenderTargetView(draw->rtv->GetCPUDescriptorHandleForHeapStart(),transparent,0,nullptr);
        for(auto& texture:draw->textures)
        {
            Barrier(list,texture.image.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=texture.upload.Get();
            src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=texture.footprint;
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=texture.image.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            Barrier(list,texture.image.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        const auto setup=[&] {
            list->SetPipelineState(draw->pipeline.Get()); list->SetGraphicsRootSignature(draw->root.Get());
            ID3D12DescriptorHeap* heaps[]={draw->srv.Get()}; list->SetDescriptorHeaps(1,heaps);
            const auto r=draw->rtv->GetCPUDescriptorHandleForHeapStart(); list->OMSetRenderTargets(1,&r,FALSE,nullptr);
            D3D12_VIEWPORT viewport{0,0,data->DisplaySize.x*data->FramebufferScale.x,data->DisplaySize.y*data->FramebufferScale.y,0,1};
            list->RSSetViewports(1,&viewport); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            D3D12_VERTEX_BUFFER_VIEW vb{draw->vertices->GetGPUVirtualAddress(),(UINT)(data->TotalVtxCount*sizeof(ImDrawVert)),sizeof(ImDrawVert)};
            D3D12_INDEX_BUFFER_VIEW ib{draw->indices->GetGPUVirtualAddress(),(UINT)(data->TotalIdxCount*sizeof(ImDrawIdx)),sizeof(ImDrawIdx)==2?DXGI_FORMAT_R16_UINT:DXGI_FORMAT_R32_UINT};
            list->IASetVertexBuffers(0,1,&vb); list->IASetIndexBuffer(&ib);
            const float l=data->DisplayPos.x, rpos=l+data->DisplaySize.x, t=data->DisplayPos.y, b=t+data->DisplaySize.y;
            const float projection[16]={2/(rpos-l),0,0,0,0,2/(t-b),0,0,0,0,0.5f,0,(rpos+l)/(l-rpos),(t+b)/(b-t),0.5f,1};
            list->SetGraphicsRoot32BitConstants(0,16,projection,0);
            const float blend[4]={0,0,0,0}; list->OMSetBlendFactor(blend);
        };
        setup(); int vertexOffset=0, indexOffset=0;
        for(const auto* commands:data->CmdLists)
        {
            for(const auto& cmd:commands->CmdBuffer)
            {
                if(cmd.UserCallback) {
                    if(cmd.UserCallback==ImDrawCallback_ResetRenderState) setup(); else cmd.UserCallback(commands,&cmd);
                    continue;
                }
                if(!cmd.ElemCount) continue;
                D3D12_RECT clip{
                    (LONG)(std::max)(0.0f,(cmd.ClipRect.x-data->DisplayPos.x)*data->FramebufferScale.x),
                    (LONG)(std::max)(0.0f,(cmd.ClipRect.y-data->DisplayPos.y)*data->FramebufferScale.y),
                    (LONG)(std::min)((float)desc.Width,(cmd.ClipRect.z-data->DisplayPos.x)*data->FramebufferScale.x),
                    (LONG)(std::min)((float)desc.Height,(cmd.ClipRect.w-data->DisplayPos.y)*data->FramebufferScale.y)};
                if(clip.right<=clip.left || clip.bottom<=clip.top) continue;
                list->RSSetScissorRects(1,&clip);
                auto gpu=draw->srv->GetGPUDescriptorHandleForHeapStart(); gpu.ptr+=(UINT64)draw->textureIndices.at(cmd.GetTexID())*increment;
                list->SetGraphicsRootDescriptorTable(1,gpu);
                list->DrawIndexedInstanced(cmd.ElemCount,1,cmd.IdxOffset+indexOffset,(INT)cmd.VtxOffset+vertexOffset,0);
            }
            vertexOffset+=commands->VtxBuffer.Size; indexOffset+=commands->IdxBuffer.Size;
        }
        for(auto& texture:draw->textures) Barrier(list,texture.image.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        return true;
    }
};

} // namespace

struct OverlayMenuRenderer::Impl {
    using ComPtr=Microsoft::WRL::ComPtr<IUnknown>;
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Image {
        std::shared_ptr<void> draw;
        Ptr<ID3D11Texture2D> texture11;
        Ptr<ID3D12Fence> fence12;
        Ptr<ID3D11Fence> fence11;
        std::uint64_t value{};
        bool recorded{};
        HRESULT retired() const noexcept {
            if(recorded)return E_PENDING;
            if(!value)return S_OK;
            const auto completed=fence12?fence12->GetCompletedValue():
                fence11?fence11->GetCompletedValue():UINT64_MAX;
            if(completed==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
            return completed>=value?S_OK:E_PENDING;
        }
        void clear(){draw.reset();texture11.Reset();fence12.Reset();fence11.Reset();value=0;recorded=false;}
    };
    struct ContextScope {
        std::unique_lock<std::recursive_mutex> global{imgui_context_mutex()};
        ImGuiContext* previous=ImGui::GetCurrentContext();
        explicit ContextScope(ImGuiContext* current){ImGui::SetCurrentContext(current);}
        ~ContextScope(){ImGui::SetCurrentContext(previous);}
    };
    struct DeviceStateScope11 {
        ID3D11DeviceContext1* context{};
        Ptr<ID3DDeviceContextState> previous;
        DeviceStateScope11(ID3D11DeviceContext1* current,ID3DDeviceContextState* owned):context(current) {
            context->SwapDeviceContextState(owned,&previous);
        }
        ~DeviceStateScope11(){
            // Only our isolated state is cleared. Do not retain an XR image as
            // a stale private binding after its exact per-image receipt retires.
            context->ClearState();context->SwapDeviceContextState(previous.Get(),nullptr);
        }
    };
    Ptr<ID3D12Device> device12;
    Ptr<ID3D12CommandQueue> queue12;
    Ptr<ID3D11Device> device11;
    Ptr<ID3D11DeviceContext1> immediate11;
    Ptr<ID3DDeviceContextState> state11;
    std::unique_ptr<ImmutableMenuDraw12> renderer12;
    ImGuiContext* imgui{};
    DXGI_FORMAT format{};
    std::uint32_t width{},height{};
    float density=1.F;
    std::vector<Image> images;
    std::chrono::steady_clock::time_point previous_frame{};
    bool dx11_ready{},initialized{},frame_open{};
    std::recursive_mutex mutex;

    ~Impl(){destroy_context();}
    void destroy_context() noexcept {
        if(!imgui)return;
        ContextScope scope(imgui);if(scope.previous==imgui)scope.previous=nullptr;
        renderer12.reset();
        if(dx11_ready){ImGui_ImplDX11_Shutdown();dx11_ready=false;}
        ImGui::DestroyContext(imgui);imgui=nullptr;
    }
    HRESULT image_ready(std::uint32_t index) noexcept {
        if(!initialized||index>=images.size())return E_INVALIDARG;
        const auto ready=images[index].retired();if(FAILED(ready))return ready;
        images[index].clear();return S_OK;
    }
    void begin_frame(const OverlayMenuInputFrame& input) {
        const auto now=std::chrono::steady_clock::now();
        auto& io=ImGui::GetIO();
        io.DisplaySize=ImVec2(float(width)/density,float(height)/density);io.DisplayFramebufferScale=ImVec2(density,density);
        const auto delta=previous_frame.time_since_epoch().count()?std::chrono::duration<float>(now-previous_frame).count():1.0F/60.0F;
        io.DeltaTime=std::clamp(delta,1.0F/1000.0F,0.25F);previous_frame=now;
        apply_imgui_input(input);
        if(dx11_ready)ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();frame_open=true;
    }
    void end_failed_frame() noexcept {
        if(frame_open){ImGui::EndFrame();frame_open=false;}
    }
    static bool same_identity(IUnknown* a,IUnknown* b) noexcept {
        if(!a||!b)return false;
        constexpr GUID nativeGuid{0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};
        auto identity=[&](IUnknown* value) {
            Ptr<IUnknown> native, result;
            if(SUCCEEDED(value->QueryInterface(nativeGuid,reinterpret_cast<void**>(native.GetAddressOf()))))value=native.Get();
            value->QueryInterface(IID_PPV_ARGS(&result));return result;
        };
        const auto first=identity(a),second=identity(b);return first&&second&&first.Get()==second.Get();
    }
    bool owns(ID3D12DeviceChild* object)const noexcept {
        Ptr<ID3D12Device> owner;return object&&SUCCEEDED(object->GetDevice(IID_PPV_ARGS(&owner)))&&same_identity(device12.Get(),owner.Get());
    }
    bool owns(ID3D11DeviceChild* object)const noexcept {
        Ptr<ID3D11Device> owner;if(object)object->GetDevice(&owner);return same_identity(device11.Get(),owner.Get());
    }
};

OverlayMenuRenderer::OverlayMenuRenderer() noexcept=default;
OverlayMenuRenderer::~OverlayMenuRenderer() {
    if(FAILED(shutdown())) {
        // Missing/failed submission receipts cannot justify releasing resources
        // still referenced by a GPU list. Normal caller teardown drains first.
        // A failed owner stays bounded to this overlay's <=64 immutable images.
        (void)impl_.release();
    }
}
HRESULT OverlayMenuRenderer::initialize(ID3D12Device* device12,ID3D12CommandQueue* queue12,
    ID3D11Device* device11,DXGI_FORMAT format,std::uint32_t image_count,std::uint32_t width,std::uint32_t height,float density) noexcept {
    if(!std::isfinite(density)||density<1.F||density>4.F)return E_INVALIDARG;
    if(impl_||!image_count||image_count>64||!width||!height||width>16384||height>16384||
        (!device11&&(!device12||!queue12)))return E_INVALIDARG;
    switch(format){case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:break;default:return DXGI_ERROR_UNSUPPORTED;}
    try {
        auto candidate=std::make_unique<Impl>();candidate->format=format;candidate->width=width;candidate->height=height;candidate->density=density;
        candidate->images.resize(image_count);
        if(device11) {
            candidate->device11=device11;Impl::Ptr<ID3D11DeviceContext> immediate;
            device11->GetImmediateContext(&immediate);if(!immediate)return E_NOINTERFACE;
            auto created=immediate.As(&candidate->immediate11);if(FAILED(created))return created;
            Impl::Ptr<ID3D11Device1> state_device;created=device11->QueryInterface(IID_PPV_ARGS(&state_device));if(FAILED(created))return created;
            const UINT flags=(device11->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED)?
                D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED:0;
            // The state API's documented level list ends at 11_1. Our shaders
            // need at most 11_0; the exact original game state is restored.
            const auto level=std::min(device11->GetFeatureLevel(),D3D_FEATURE_LEVEL_11_1);D3D_FEATURE_LEVEL selected{};
            created=state_device->CreateDeviceContextState(flags,&level,1,D3D11_SDK_VERSION,
                __uuidof(ID3D11Device),&selected,&candidate->state11);if(FAILED(created))return created;
        } else {
            candidate->device12=device12;candidate->queue12=queue12;
            if(!candidate->owns(queue12)||queue12->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return E_INVALIDARG;
        }
        IMGUI_CHECKVERSION();
        {
            std::scoped_lock context_lock(imgui_context_mutex());const auto previous=ImGui::GetCurrentContext();
            candidate->imgui=ImGui::CreateContext();ImGui::SetCurrentContext(previous);
        }
        if(!candidate->imgui)return E_OUTOFMEMORY;
        Impl::ContextScope scope(candidate->imgui);auto& io=ImGui::GetIO();
        io.IniFilename=nullptr;io.LogFilename=nullptr;io.DisplaySize=ImVec2(float(width),float(height));
        io.ConfigFlags=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NoMouseCursorChange;
        autonomous::apply_settings_style(true);
        if(device11) {
            candidate->dx11_ready=ImGui_ImplDX11_Init(device11,candidate->immediate11.Get());
            if(!candidate->dx11_ready)return E_FAIL;
        } else {
            candidate->renderer12=std::make_unique<ImmutableMenuDraw12>(device12);
            if(!candidate->renderer12->Init())return E_FAIL;
        }
        candidate->initialized=true;impl_=std::move(candidate);return S_OK;
    }catch(...){return E_OUTOFMEMORY;}
}
HRESULT OverlayMenuRenderer::render_d3d12(ID3D12Resource* texture,ID3D12GraphicsCommandList* list,
    std::uint32_t image_index,const OverlayMenuInputFrame& input,OverlayMenuDraw draw,void* user) noexcept {
    if(!impl_||!impl_->renderer12||!texture||!list||!draw)return E_INVALIDARG;
    std::scoped_lock lock(impl_->mutex);
    if(!impl_->owns(texture)||!impl_->owns(list)||list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return E_INVALIDARG;
    const auto desc=texture->GetDesc();
    if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.Width!=impl_->width||desc.Height!=impl_->height||
        desc.DepthOrArraySize!=1||desc.SampleDesc.Count!=1||!(desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)||
        !compatible_color_format(impl_->format,desc.Format))return E_INVALIDARG;
    const auto ready=impl_->image_ready(image_index);if(FAILED(ready))return ready;
    auto& image=impl_->images[image_index];Impl::ContextScope scope(impl_->imgui);
    try {
        impl_->begin_frame(input);draw(user);ImGui::Render();impl_->frame_open=false;
        const auto rendered=impl_->renderer12->Render(ImGui::GetDrawData(),list,texture,impl_->format,image.draw);
        image.recorded=bool(image.draw);return rendered?S_OK:E_FAIL;
    }catch(...) {
        impl_->end_failed_frame();image.recorded=bool(image.draw);return E_FAIL;
    }
}
HRESULT OverlayMenuRenderer::render_d3d11(ID3D11Texture2D* texture,std::uint32_t image_index,
    const OverlayMenuInputFrame& input,OverlayMenuDraw draw,void* user) noexcept {
    if(!impl_||!impl_->dx11_ready||!texture||!draw)return E_INVALIDARG;
    std::scoped_lock lock(impl_->mutex);if(!impl_->owns(texture))return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    if(desc.Width!=impl_->width||desc.Height!=impl_->height||desc.ArraySize!=1||desc.SampleDesc.Count!=1||
        !(desc.BindFlags&D3D11_BIND_RENDER_TARGET)||!compatible_color_format(impl_->format,desc.Format))return E_INVALIDARG;
    const auto ready=impl_->image_ready(image_index);if(FAILED(ready))return ready;
    auto& image=impl_->images[image_index];Impl::ContextScope scope(impl_->imgui);
    try {
        Impl::Ptr<ID3D11RenderTargetView> target;D3D11_RENDER_TARGET_VIEW_DESC view{};
        view.Format=impl_->format;view.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        const auto created=impl_->device11->CreateRenderTargetView(texture,&view,&target);if(FAILED(created))return created;
        Impl::DeviceStateScope11 state_scope(impl_->immediate11.Get(),impl_->state11.Get());
        impl_->begin_frame(input);draw(user);ImGui::Render();impl_->frame_open=false;
        const float transparent[4]{};auto* rtv=target.Get();
        image.texture11=texture;image.recorded=true;
        impl_->immediate11->OMSetRenderTargets(1,&rtv,nullptr);
        impl_->immediate11->ClearRenderTargetView(rtv,transparent);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        return S_OK;
    }catch(...) {
        impl_->end_failed_frame();
        // An immediate draw that already emitted GPU work must still receive
        // the caller's fence even if a custom draw command aborted the frame.
        return image.recorded?S_OK:E_FAIL;
    }
}
HRESULT OverlayMenuRenderer::submitted_d3d12(std::uint32_t image_index,ID3D12Fence* fence,std::uint64_t value) noexcept {
    if(!impl_||!impl_->renderer12||!fence||!value||value==UINT64_MAX)return E_INVALIDARG;
    std::scoped_lock lock(impl_->mutex);
    if(image_index>=impl_->images.size()||!impl_->images[image_index].recorded||!impl_->owns(fence))return E_INVALIDARG;
    auto& image=impl_->images[image_index];image.fence12=fence;image.value=value;image.recorded=false;return S_OK;
}
HRESULT OverlayMenuRenderer::submitted_d3d11(std::uint32_t image_index,ID3D11Fence* fence,std::uint64_t value) noexcept {
    if(!impl_||!impl_->dx11_ready||!fence||!value||value==UINT64_MAX)return E_INVALIDARG;
    std::scoped_lock lock(impl_->mutex);
    if(image_index>=impl_->images.size()||!impl_->images[image_index].recorded||!impl_->owns(fence))return E_INVALIDARG;
    auto& image=impl_->images[image_index];image.fence11=fence;image.value=value;image.recorded=false;return S_OK;
}
HRESULT OverlayMenuRenderer::abandon_d3d12(std::uint32_t image_index) noexcept {
    if(!impl_||!impl_->renderer12)return E_INVALIDARG;
    std::scoped_lock lock(impl_->mutex);if(image_index>=impl_->images.size())return E_INVALIDARG;
    auto& image=impl_->images[image_index];if(!image.recorded)return E_INVALIDARG;
    image.clear();return S_OK;
}
void OverlayMenuRenderer::reset_input() noexcept {
    if(!impl_||!impl_->imgui)return;
    std::scoped_lock lock(impl_->mutex);Impl::ContextScope scope(impl_->imgui);
    auto& io=ImGui::GetIO();io.ClearEventsQueue();io.ClearInputKeys();io.ClearInputMouse();
    io.AddFocusEvent(false);io.MouseDrawCursor=false;
    // SetNavWindow(nullptr) leaves the focused widget ID alive. With no UI
    // frames while hidden, reopening reuses that ID and ItemAdd dereferences a
    // null NavWindow. Use ImGui's complete focus transition instead.
    ImGui::ClearActiveID();ImGui::ClosePopupsOverWindow(nullptr,false);ImGui::FocusWindow(nullptr);
}
HRESULT OverlayMenuRenderer::shutdown() noexcept {
    if(!impl_)return S_OK;
    {
        std::scoped_lock lock(impl_->mutex);
        for(const auto& image:impl_->images){const auto result=image.retired();if(FAILED(result))return result;}
        for(auto& image:impl_->images)image.clear();impl_->destroy_context();impl_->initialized=false;
    }
    impl_.reset();return S_OK;
}
bool OverlayMenuRenderer::ready()const noexcept{return impl_&&impl_->initialized;}
} // namespace xrfg
