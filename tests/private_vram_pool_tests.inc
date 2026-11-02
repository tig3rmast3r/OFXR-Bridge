// Exercise real GPU resources and their destruction, rather than extrapolating
// a native-handle count. The runtime is bounded, but its three images are real.
// D12 bytes use GetResourceAllocationInfo; D11 bytes are exact texture payload
// bytes (not a claim about opaque driver residency/alignment).
enum class VramStereoLayout { separate, packed, array };
struct VramResourceCounters {
    std::atomic<unsigned> private_images{},native_images{};
    std::atomic<std::uint64_t> live_bytes{};
};
constexpr GUID vram_resource_lifetime_id{
    0xc7f2b947,0x3341,0x4b6a,{0x89,0x06,0x6d,0x48,0x9b,0x2d,0x88,0xa3}};
class VramResourceLifetime final : public IUnknown {
    std::atomic<ULONG> references_{1};
    VramResourceCounters& counters_;
    const bool native_;
    const std::uint64_t bytes_;
public:
    VramResourceLifetime(VramResourceCounters& counters,bool native,std::uint64_t bytes)
        :counters_(counters),native_(native),bytes_(bytes) {
        ++(native_?counters_.native_images:counters_.private_images);
        counters_.live_bytes+=bytes_;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;
        if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
        *output=static_cast<IUnknown*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left=--references_;
        if(!left) {
            --(native_?counters_.native_images:counters_.private_images);
            counters_.live_bytes-=bytes_;delete this;
        }
        return left;
    }
};
template<class Object>
void observe_vram_resource(Object* object,VramResourceCounters& counters,
                           bool native,std::uint64_t bytes) {
    check(object && bytes,"tracked GPU texture has a nonzero allocation");
    unsigned size{};
    check(object->GetPrivateData(vram_resource_lifetime_id,&size,nullptr)==DXGI_ERROR_NOT_FOUND,
        "one texture allocation must not masquerade as several output images");
    auto* witness=new VramResourceLifetime(counters,native,bytes);
    const auto result=object->SetPrivateDataInterface(vram_resource_lifetime_id,witness);
    witness->Release();ok(result,"attach non-owning GPU resource destruction witness");
}
std::uint64_t observe_vram_allocations(Fixture& f,
    const std::array<std::array<XrSwapchain,4>,2>& outputs,unsigned domains,
    VramResourceCounters& counters) {
    std::uint64_t image_bytes{};
    const auto d12_bytes=[&](ID3D12Resource* image) {
        const auto desc=image->GetDesc();
        return f.device12->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
    };
    const auto d11_bytes=[](ID3D11Texture2D* image) {
        D3D11_TEXTURE2D_DESC desc{};image->GetDesc(&desc);
        check(desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM && desc.MipLevels==1 && desc.SampleDesc.Count==1,
            "D11 payload accounting uses actual tested format/mip/sample contract");
        return std::uint64_t(desc.Width)*desc.Height*desc.ArraySize*4;
    };
    for(unsigned domain=0;domain<domains;++domain)for(auto handle:outputs[domain]) {
        unsigned count{};
        if(f.native12) {
            XrSwapchainImageD3D12KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            xr_ok(f.transport->enumerate_images(handle,1,&count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(&image)),"enumerate allocated private D12 texture");
            check(count==1,"generated virtual output contains one independent image");
            const auto bytes=d12_bytes(image.texture);
            if(image_bytes)check(bytes==image_bytes,"all compared images have identical allocation size");
            image_bytes=bytes;observe_vram_resource(image.texture,counters,false,bytes);
        } else {
            XrSwapchainImageD3D11KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
            xr_ok(f.transport->enumerate_images(handle,1,&count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(&image)),"enumerate allocated private D11 texture");
            check(count==1,"generated virtual D11 output contains one independent image");
            const auto bytes=d11_bytes(image.texture);
            if(image_bytes)check(bytes==image_bytes,"D11 payload shapes match");
            image_bytes=bytes;observe_vram_resource(image.texture,counters,false,bytes);
        }
    }
    for(auto& [handle,chain]:f.chains) {
        if(f.native12)for(auto& image:chain.d12) {
            const auto bytes=d12_bytes(image.Get());check(bytes==image_bytes,"native D12 allocation matches source shape");
            observe_vram_resource(image.Get(),counters,true,bytes);
        } else for(auto& image:chain.d11) {
            const auto bytes=d11_bytes(image.Get());check(bytes==image_bytes,"native D11 payload matches source shape");
            observe_vram_resource(image.Get(),counters,true,bytes);
        }
    }
    return image_bytes;
}
void write_vram_stereo(Fixture& f,XrSwapchain handle,VramStereoLayout layout,
                      unsigned left,unsigned right) {
    check(layout!=VramStereoLayout::separate,"one-resource stereo writer requires packed or array layout");
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
    unsigned index{};xr_ok(f.transport->acquire_image(handle,&acquire,&index),"acquire stereo output");
    xr_ok(f.transport->wait_image(handle,&wait),"wait stereo output");
    std::scoped_lock lock(f.gpu);
    unsigned count{};
    if(f.native12) {
        XrSwapchainImageD3D12KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
        xr_ok(f.transport->enumerate_images(handle,1,&count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(&image)),"stereo D12 resource");
        const auto desc=image.texture->GetDesc();
        ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap_desc.NumDescriptors=2;
        ok(f.device12->CreateDescriptorHeap(&heap_desc,IID_PPV_ARGS(&heap)),"stereo RTV heap");
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE,2> views{};
        const auto step=f.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for(unsigned eye=0;eye<2;++eye) {
            views[eye]=heap->GetCPUDescriptorHandleForHeapStart();views[eye].ptr+=eye*step;
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};rtv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            if(layout==VramStereoLayout::array) {
                rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtv.Texture2DArray.FirstArraySlice=eye;rtv.Texture2DArray.ArraySize=1;
            } else rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
            f.device12->CreateRenderTargetView(image.texture,&rtv,views[eye]);
        }
        f.commands([&](ID3D12GraphicsCommandList* list) {
            for(unsigned eye=0;eye<2;++eye) {
                float color[]{float(eye?right:left)/255.0F,0,0,1};
                D3D12_RECT rect{LONG(eye*desc.Width/2),0,LONG((eye+1)*desc.Width/2),LONG(desc.Height)};
                list->ClearRenderTargetView(views[eye],color,
                    layout==VramStereoLayout::packed?1:0,
                    layout==VramStereoLayout::packed?&rect:nullptr);
            }
        });
    } else {
        XrSwapchainImageD3D11KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        xr_ok(f.transport->enumerate_images(handle,1,&count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(&image)),"stereo D11 resource");
        D3D11_TEXTURE2D_DESC desc{};image.texture->GetDesc(&desc);
        std::vector<std::uint32_t> data(std::size_t(desc.Width)*desc.Height);
        for(unsigned slice=0;slice<desc.ArraySize;++slice) {
            for(unsigned y=0;y<desc.Height;++y)for(unsigned x=0;x<desc.Width;++x) {
                const auto value=layout==VramStereoLayout::array?(slice?right:left):(x>=desc.Width/2?right:left);
                data[std::size_t(y)*desc.Width+x]=value|0xff000000U;
            }
            f.context->UpdateSubresource(image.texture,slice,nullptr,data.data(),desc.Width*4,0);
        }
    }
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xr_ok(f.transport->release_image(handle,&release),"release complete stereo output");
}
void vram_publication_layout(bool d12,VramStereoLayout layout,bool reuse) {
    const unsigned domains=layout==VramStereoLayout::separate?2:1;
    const auto* layout_name=layout==VramStereoLayout::separate?"separate":layout==VramStereoLayout::packed?"packed":"array2";
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.format=DXGI_FORMAT_R8G8B8A8_UNORM;info.width=layout==VramStereoLayout::packed?1024:512;
    info.height=256;info.arraySize=layout==VramStereoLayout::array?2:1;
    info.faceCount=info.mipCount=info.sampleCount=1;
    info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    std::uint64_t baseline_bytes{};
    // First measure the old unpooled topology with actual GPU allocations,
    // then release every allocation before creating the pooled topology.
    for(bool pooled:{false,true}) {
        VramResourceCounters counters;
        Fixture f(d12);f.native_image_count=3;f.expected_views=2;f.throttled=false;
        f.reuse_current_real_image=reuse;
        check(f.chains.empty(),"cold transport has no allocated generation publication sink");
        std::array<std::array<XrSwapchain,4>,2> outputs{};
        for(unsigned domain=0;domain<domains;++domain)for(auto& handle:outputs[domain])
            xr_ok(f.transport->create_swapchain(&info,&handle,D3D12_RESOURCE_STATE_RENDER_TARGET,1,
                pooled?reinterpret_cast<XrSwapchain>(400+domain):XR_NULL_HANDLE),
                "create bounded full-size color generation outputs");
        const auto bytes=observe_vram_allocations(f,outputs,domains,counters);
        check(counters.private_images==4*domains,"sharing preserves all four independent private work images");
        check(counters.native_images==(pooled?3:12)*domains,"native images follow shared-sink count, not private ring count");
        const auto live=counters.live_bytes.load();
        check(live==(pooled?7:16)*domains*bytes,"measured image allocations account for entire output topology");
        check(f.chains.size()==(pooled?1:4)*domains,"native swapchain allocation follows source domains");
        if(!pooled)baseline_bytes=live;
        else {
            check(live*16==baseline_bytes*7,"pooled topology removes 56.25 percent of output allocation bytes");
            // Reject contracts which cannot share a plain color sink. Invalid
            // input must not allocate a partial native owner or corrupt a pool.
            const auto expect_refused=[&](XrSwapchainCreateInfo invalid) {
                const auto before=f.chains.size();XrSwapchain handle{};
                check(f.transport->create_swapchain(&invalid,&handle,D3D12_RESOURCE_STATE_RENDER_TARGET,1,
                    reinterpret_cast<XrSwapchain>(400))==XR_ERROR_VALIDATION_FAILURE,
                    "unsafe publication contract refused before allocation");
                check(f.chains.size()==before && !handle,"refusal leaves no partial native swapchain");
            };
            auto invalid=info;invalid.createFlags=XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;expect_refused(invalid);
            invalid=info;invalid.createFlags=XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT;expect_refused(invalid);
            invalid=info;invalid.usageFlags|=XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;expect_refused(invalid);
            invalid=info;invalid.faceCount=6;expect_refused(invalid);
            invalid=info;invalid.arraySize=0;expect_refused(invalid);
            XrBaseInStructure extension{};invalid=info;invalid.next=&extension;expect_refused(invalid);
            f.native_chain_limit=f.chains.size();
            const auto expect_distinct=[&](XrSwapchainCreateInfo incompatible,
                D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_RENDER_TARGET,
                XrSwapchain source=reinterpret_cast<XrSwapchain>(400)) {
                XrSwapchain handle{};
                check(f.transport->create_swapchain(&incompatible,&handle,state,1,source)==XR_ERROR_LIMIT_REACHED && !handle,
                    "incompatible source/shape/state needs its own native sink rather than aliasing an existing pool");
            };
            invalid=info;invalid.arraySize=info.arraySize==1?2:1;expect_distinct(invalid);
            invalid=info;++invalid.width;expect_distinct(invalid);
            invalid=info;invalid.format=DXGI_FORMAT_R16G16B16A16_FLOAT;expect_distinct(invalid);
            invalid=info;++invalid.mipCount;expect_distinct(invalid);
            invalid=info;invalid.sampleCount=2;expect_distinct(invalid);
            invalid=info;invalid.usageFlags|=XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;expect_distinct(invalid);
            expect_distinct(info,D3D12_RESOURCE_STATE_COMMON);
            expect_distinct(info,D3D12_RESOURCE_STATE_RENDER_TARGET,reinterpret_cast<XrSwapchain>(9876));
            // These views deliberately share the virtual handle only for
            // packed/array stereo. Their independent rectangles/slices survive
            // metadata ownership and the final native-handle substitution.
            auto submit=[&](unsigned slot,unsigned identity) {
                std::array<XrCompositionLayerProjectionView,2> views{};
                for(unsigned eye=0;eye<2;++eye) {
                    auto& view=views[eye];view={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                    view.pose.orientation.w=1;view.pose.position.x=float(identity);
                    view.subImage.swapchain=outputs[layout==VramStereoLayout::separate?eye:0][slot];
                    view.subImage.imageArrayIndex=layout==VramStereoLayout::array?eye:0;
                    view.subImage.imageRect.offset={layout==VramStereoLayout::packed?int(eye*info.width/2):0,0};
                    view.subImage.imageRect.extent={int(layout==VramStereoLayout::packed?info.width/2:info.width),int(info.height)};
                }
                XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
                projection.space=reinterpret_cast<XrSpace>(9);projection.viewCount=2;projection.views=views.data();
                const auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
                XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=epoch+ns();
                end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=1;end.layers=&layer;
                f.begin_virtual();const auto result=f.transport->end_frame(&end);
                views[0].subImage.imageArrayIndex=99;views[1].subImage.imageRect.offset.x=9999;
                return result;
            };
            for(unsigned slot=0;slot<4;++slot) {
                if(layout==VramStereoLayout::separate) {
                    f.write(outputs[0][slot],20+slot*30);f.write(outputs[1][slot],130+slot*30);
                } else write_vram_stereo(f,outputs[0][slot],layout,20+slot*30,130+slot*30);
            }
            xr_ok(submit(0,1),"first stereo output");f.wait_entered();
            xr_ok(submit(1,2),"first synthetic output with native End held");
            xr_ok(submit(2,3),"second synthetic output with native End held");
            auto blocked=std::async(std::launch::async,[&]{return submit(3,4);});
            check(blocked.wait_for(30ms)==std::future_status::timeout,"shared sink preserves bounded producer backpressure");
            f.unblock();check(blocked.wait_for(3s)==std::future_status::ready,"native retirement releases next publication");
            xr_ok(blocked.get(),"next real output after two synthetics");
            f.wait_count(4);xr_ok(f.transport->drain(),"first full-size stereo drain");
            unsigned identity=4;
            for(auto slot:{2U,0U,3U,1U,1U})xr_ok(submit(slot,++identity),"old-slot and repeated-generation publication");
            f.wait_count(identity);xr_ok(f.transport->drain(),"all stereo publications retired");
            check(f.pixels==std::vector<unsigned>({20,50,80,110,80,20,110,50,50}),
                "full-size left eye pixels and real/synthetic FIFO order are exact");
            check(f.right_pixels==std::vector<unsigned>({130,160,190,220,190,130,220,160,160}),
                "full-size right eye never aliases the left eye, next source or retained generation");
            for(unsigned i=0;i<f.positions.size();++i)check(f.positions[i]==float(i+1),"owned pose identity follows exact output order");
            for(auto handles:f.stereo_handles) {
                check((handles[0]!=handles[1])==(layout==VramStereoLayout::separate),
                    "native handle sharing follows source domain and preserves stereo layout");
                check(handles==f.stereo_handles.front(),"each source keeps one stable native sink");
            }
            for(unsigned i=1;i<f.handoffs.size();++i)check(f.handoffs[i]-f.handoffs[i-1]>=period,
                "pool savings do not change physical director cadence");
            std::printf("%s %s native3%s: output bytes %llu -> %llu, independent private images %u, native images %u\n",
                d12?"D12 allocation":"D11 payload",layout_name,reuse?" reused-index":"",
                static_cast<unsigned long long>(baseline_bytes),static_cast<unsigned long long>(live),
                counters.private_images.load(),counters.native_images.load());
        }
        // One remaining virtual output pins exactly one native sink for that
        // source, while destroyed virtual outputs release their own GPU image.
        for(unsigned domain=0;domain<domains;++domain) {
            for(unsigned slot=0;slot<3;++slot)
                xr_ok(f.transport->destroy_swapchain(outputs[domain][slot]),"destroy nonfinal output owner");
            if(pooled)check(counters.native_images==3*(domains-domain),"native sink survives until final private owner");
            xr_ok(f.transport->destroy_swapchain(outputs[domain][3]),"destroy final output owner");
        }
        check(f.chains.empty() && !counters.private_images && !counters.native_images && !counters.live_bytes,
            "all actual private/native generation allocations destroyed after ordered retirement");
    }
}
void vram_publication_tests() {
    for(bool d12:{true,false})for(auto layout:{VramStereoLayout::separate,VramStereoLayout::packed,VramStereoLayout::array}) {
        vram_publication_layout(d12,layout,false);
        vram_publication_layout(d12,layout,true);
    }
    std::puts("Private VRAM: native3 allocation reduction, split/packed/array stereo, real GPU pixels, bounded FIFO, exact-index reuse and zero retained output allocations PASS");
}

// Allocation-only HDR transport coverage. This intentionally does not call an
// interpolation provider or reinterpret RGBA16F readback as the UNORM8 pixel
// oracle above: provider HDR admission is a separate, unchanged contract.
void vram_hdr_publication_tests() {
    for(bool d12:{true,false})for(auto layout:{VramStereoLayout::separate,VramStereoLayout::packed,VramStereoLayout::array}) {
        const unsigned domains=layout==VramStereoLayout::separate?2:1;
        const auto* shape=layout==VramStereoLayout::separate?"separate":layout==VramStereoLayout::packed?"packed":"array2";
        std::uint64_t baseline{};
        for(bool pooled:{false,true}) {
            VramResourceCounters counters;
            Fixture f(d12);f.native_image_count=3;
            XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            info.format=DXGI_FORMAT_R16G16B16A16_FLOAT;
            info.width=layout==VramStereoLayout::packed?1024:512;
            info.height=256;info.arraySize=layout==VramStereoLayout::array?2:1;
            info.faceCount=info.mipCount=info.sampleCount=1;
            info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            const auto payload=std::uint64_t(info.width)*info.height*info.arraySize*8;
            std::vector<XrSwapchain> outputs;
            std::uint64_t allocation_bytes{};
            for(unsigned domain=0;domain<domains;++domain)for(unsigned slot=0;slot<4;++slot) {
                XrSwapchain output{};
                xr_ok(f.transport->create_swapchain(&info,&output,D3D12_RESOURCE_STATE_RENDER_TARGET,1,
                    pooled?reinterpret_cast<XrSwapchain>(std::uintptr_t{400}+domain):XR_NULL_HANDLE),
                    "allocate RGBA16F private generation output");
                outputs.push_back(output);unsigned count{};
                if(d12) {
                    XrSwapchainImageD3D12KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
                    xr_ok(f.transport->enumerate_images(output,1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&image)),
                        "enumerate RGBA16F private D12 resource");
                    const auto desc=image.texture->GetDesc();
                    check(desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT && desc.DepthOrArraySize==info.arraySize,
                        "actual HDR texture preserves format and stereo domains");
                    const auto bytes=f.device12->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
                    if(allocation_bytes)check(allocation_bytes==bytes,"compared HDR allocations have identical shape");
                    allocation_bytes=bytes;observe_vram_resource(image.texture,counters,false,bytes);
                } else {
                    XrSwapchainImageD3D11KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
                    xr_ok(f.transport->enumerate_images(output,1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&image)),
                        "enumerate RGBA16F private D11 resource");
                    D3D11_TEXTURE2D_DESC desc{};image.texture->GetDesc(&desc);
                    check(desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT && desc.ArraySize==info.arraySize && desc.MipLevels==1,
                        "actual D11 HDR resource preserves exact payload shape");
                    allocation_bytes=payload;observe_vram_resource(image.texture,counters,false,payload);
                }
                check(count==1,"HDR generation output keeps one private work image");
            }
            for(auto& [handle,chain]:f.chains) {
                if(d12)for(auto& image:chain.d12) {
                    const auto desc=image->GetDesc();
                    const auto bytes=f.device12->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
                    check(bytes==allocation_bytes,"native HDR allocation matches private shape");
                    observe_vram_resource(image.Get(),counters,true,bytes);
                } else for(auto& image:chain.d11)observe_vram_resource(image.Get(),counters,true,payload);
            }
            const auto live=counters.live_bytes.load();
            check(counters.private_images==4*domains && counters.native_images==(pooled?3:12)*domains,
                "HDR topology keeps private work capacity while removing duplicated native image sets");
            check(live==(pooled?7:16)*domains*allocation_bytes,"actual HDR allocation/payload byte budget closes");
            if(!pooled)baseline=live;
            else {
                check(live*16==baseline*7,"HDR outputs receive identical 56.25 percent topology reduction");
                std::printf("%s RGBA16F %s native3: bytes %llu -> %llu (transport allocation only)\n",
                    d12?"D12 allocation":"D11 payload",shape,
                    static_cast<unsigned long long>(baseline),static_cast<unsigned long long>(live));
            }
            for(auto handle:outputs)xr_ok(f.transport->destroy_swapchain(handle),"retire unused HDR output allocation");
            check(!counters.private_images && !counters.native_images && !counters.live_bytes && f.chains.empty(),
                "all RGBA16F private/native output allocations released");
        }
    }
    std::puts("RGBA16F transport allocation: D11/D12 split/packed/array2 native3 topology and retirement PASS; provider HDR admission unchanged");
}
