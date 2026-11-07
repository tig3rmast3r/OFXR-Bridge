// Private UI draw targets feed one real XR atlas. Only this optional UI owner
// sees the virtual tokens; no application/presenter API receives them.
static constexpr unsigned ui_atlas_width=1024,ui_status_y=768,ui_atlas_height=1280;
struct UiSourceImage { ComPtr<ID3D12Resource> d12;ComPtr<ID3D11Texture2D> d11;std::uint64_t read_fence{}; };
struct UiSource {
    bool menu{},held{},valid{};unsigned width{},height{},next{},index{},published{};
    std::uint64_t version{};
    std::array<UiSourceImage,3> images;
};
std::unordered_map<XrSwapchain,std::unique_ptr<UiSource>> ui_sources;
bool ui_atlas_supported{},ui_atlas_held{},ui_atlas_waited{},ui_atlas_failed{};
bool ui_atlas_enabled{},ui_policy_warming{};
NativeSwapchainCount ui_native_count;
std::optional<bool> ui_pair_choice;
unsigned ui_pair_mask{},ui_projected_total{};
XrSwapchain ui_atlas{};unsigned ui_atlas_index{};
std::vector<Image> ui_atlas_images;
std::vector<std::pair<XrSwapchain,std::uint64_t>> ui_published;

XrResult ensure_ui_atlas(const XrSwapchainCreateInfo& source) {
    if(ui_atlas)return XR_SUCCESS;
    auto info=source;info.width=ui_atlas_width;info.height=ui_atlas_height;
    info.usageFlags|=XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    const auto result=runtime_create_swapchain(session,&info,&ui_atlas);
    if(result!=XR_SUCCESS)return result;
    unsigned count{};
    auto fail=[&](XrResult r) {
        runtime_destroy_swapchain(ui_atlas);ui_atlas={};ui_atlas_images.clear();return r;
    };
    if(runtime_enumerate_images(ui_atlas,0,&count,nullptr)!=XR_SUCCESS || !count || count>64)
        return fail(XR_ERROR_RUNTIME_FAILURE);
    ui_atlas_images.resize(count);
    if(device11) {
        std::vector<XrSwapchainImageD3D11KHR> textures(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if(runtime_enumerate_images(ui_atlas,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(textures.data()))!=XR_SUCCESS)
            return fail(XR_ERROR_RUNTIME_FAILURE);
        for(unsigned i=0;i<count;++i) {
            ui_atlas_images[i].texture11=textures[i].texture;
            if(!ui_atlas_images[i].texture11)return fail(XR_ERROR_RUNTIME_FAILURE);
        }
    } else {
        std::vector<XrSwapchainImageD3D12KHR> textures(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        if(runtime_enumerate_images(ui_atlas,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(textures.data()))!=XR_SUCCESS)
            return fail(XR_ERROR_RUNTIME_FAILURE);
        for(unsigned i=0;i<count;++i) {
            auto& image=ui_atlas_images[i];image.texture12=textures[i].texture;
            if(!image.texture12 || FAILED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&image.allocator))) ||
                FAILED(device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,image.allocator.Get(),nullptr,IID_PPV_ARGS(&image.list))) ||
                FAILED(image.list->Close()))return fail(XR_ERROR_RUNTIME_FAILURE);
        }
    }
    return XR_SUCCESS;
}
XrResult create_swapchain(XrSession owner,const XrSwapchainCreateInfo* info,XrSwapchain* out,bool menu_source=false) {
    if(!ui_atlas_supported || !ui_atlas_enabled)return runtime_create_swapchain(owner,info,out);
    if(!out || !info)return XR_ERROR_VALIDATION_FAILURE;
    *out=XR_NULL_HANDLE;
    if(info->width>ui_atlas_width || info->height>(menu_source?ui_status_y:ui_atlas_height-ui_status_y))
        return XR_ERROR_VALIDATION_FAILURE;
    // Allocate private images first: a failure cannot leave an empty native
    // reservation behind. These dimensions do not identify a surface's role.
    auto source=std::make_unique<UiSource>();source->menu=menu_source;
    source->width=info->width;source->height=info->height;
    for(auto& image:source->images) {
        HRESULT result{};
        if(device11) {
            D3D11_TEXTURE2D_DESC d{};d.Width=info->width;d.Height=info->height;
            d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=static_cast<DXGI_FORMAT>(info->format);
            d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            result=device11->CreateTexture2D(&d,nullptr,&image.d11);
        } else {
            D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width=info->width;d.Height=info->height;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;
            d.Format=static_cast<DXGI_FORMAT>(info->format);d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            result=device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&image.d12));
        }
        if(FAILED(result))return XR_ERROR_OUT_OF_MEMORY;
    }
    const auto result=ensure_ui_atlas(*info);if(result!=XR_SUCCESS)return result;
    const auto token=xr_handle_from_pointer<XrSwapchain>(source.get());
    ui_sources.emplace(token,std::move(source));*out=token;return XR_SUCCESS;
}
XrResult enumerate_images(XrSwapchain chain,unsigned capacity,unsigned* count,XrSwapchainImageBaseHeader* output) {
    const auto found=ui_sources.find(chain);
    if(found==ui_sources.end())return runtime_enumerate_images(chain,capacity,count,output);
    *count=3;if(!capacity)return XR_SUCCESS;if(capacity<3)return XR_ERROR_SIZE_INSUFFICIENT;
    for(unsigned i=0;i<3;++i) {
        if(device11)reinterpret_cast<XrSwapchainImageD3D11KHR*>(output)[i].texture=found->second->images[i].d11.Get();
        else reinterpret_cast<XrSwapchainImageD3D12KHR*>(output)[i].texture=found->second->images[i].d12.Get();
    }
    return XR_SUCCESS;
}
XrResult acquire(XrSwapchain chain,const XrSwapchainImageAcquireInfo* info,unsigned* index) {
    const auto found=ui_sources.find(chain);
    if(found==ui_sources.end())return runtime_acquire(chain,info,index);
    if(ui_atlas_failed)return XR_TIMEOUT_EXPIRED;
    auto& source=*found->second;if(source.held)return XR_ERROR_CALL_ORDER_INVALID;
    const auto completed=fence12?fence12->GetCompletedValue():fence11->GetCompletedValue();
    if(completed==UINT64_MAX)return XR_ERROR_RUNTIME_FAILURE;
    for(unsigned n=0;n<3;++n) {
        const auto candidate=(source.next+n)%3;
        if(source.images[candidate].read_fence>completed)continue;
        source.index=candidate;source.next=(candidate+1)%3;source.held=true;*index=candidate;return XR_SUCCESS;
    }
    return XR_TIMEOUT_EXPIRED;
}
XrResult wait(XrSwapchain chain,const XrSwapchainImageWaitInfo* info) {
    const auto found=ui_sources.find(chain);
    if(found==ui_sources.end())return runtime_wait(chain,info);
    return found->second->held?XR_SUCCESS:XR_ERROR_CALL_ORDER_INVALID;
}
XrResult release(XrSwapchain chain,const XrSwapchainImageReleaseInfo* info) {
    const auto found=ui_sources.find(chain);
    if(found==ui_sources.end())return runtime_release(chain,info);
    auto& source=*found->second;if(!source.held)return XR_ERROR_CALL_ORDER_INVALID;
    source.held=false;source.valid=true;source.published=source.index;++source.version;return XR_SUCCESS;
}
XrResult destroy_swapchain(XrSwapchain chain) {
    const auto found=ui_sources.find(chain);
    if(found==ui_sources.end())return runtime_destroy_swapchain(chain);
    const auto completed=fence12?fence12->GetCompletedValue():fence11->GetCompletedValue();
    if(completed==UINT64_MAX)return XR_ERROR_RUNTIME_FAILURE;
    for(const auto& image:found->second->images)if(image.read_fence>completed)return XR_ERROR_CALL_ORDER_INVALID;
    if(ui_sources.size()==1 && ui_atlas) {
        for(const auto& image:ui_atlas_images)if(image.fence_value>completed)return XR_ERROR_CALL_ORDER_INVALID;
        if(ui_atlas_held) {
            if(!ui_atlas_waited) {
                XrSwapchainImageWaitInfo info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};info.timeout=0;
                if(runtime_wait(ui_atlas,&info)!=XR_SUCCESS)return XR_ERROR_CALL_ORDER_INVALID;
                ui_atlas_waited=true;
            }
            XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            if(runtime_release(ui_atlas,&info)!=XR_SUCCESS)return XR_ERROR_CALL_ORDER_INVALID;
            ui_atlas_held=ui_atlas_waited=false;
        }
        const auto result=runtime_destroy_swapchain(ui_atlas);if(result!=XR_SUCCESS)return result;
        ui_atlas={};ui_atlas_images.clear();ui_published.clear();ui_atlas_failed=false;
    }
    std::erase_if(ui_published,[&](const auto& p){return p.first==chain;});
    ui_sources.erase(found);return XR_SUCCESS;
}
XrSwapchain public_ui_handle(XrSwapchain source) const {
    return ui_sources.contains(source)?ui_atlas:source;
}
bool public_ui_quad(XrSwapchain source,const XrCompositionLayerQuad& quad,XrCompositionLayerQuad& out) const {
    out=quad;const auto found=ui_sources.find(source);if(found==ui_sources.end())return true;
    if(std::none_of(ui_published.begin(),ui_published.end(),[&](const auto& p){return p.first==source;}))return false;
    out.subImage.swapchain=ui_atlas;out.subImage.imageRect.offset.y+=found->second->menu?0:ui_status_y;
    return true;
}
// Called only on the producer's existing UI refresh path. Native image waits
// remain zero-timeout; the delivery thread only reads already-published quads.
void compose_ui_atlas(const XrFrameEndInfo* info) {
    if(!ui_atlas || ui_atlas_failed || disabled || !info)return;
    std::array<std::pair<XrSwapchain,std::uint64_t>,2> pending{};unsigned pending_count{};
    const auto add=[&](XrSwapchain source) {
        const auto found=ui_sources.find(source);
        if(found!=ui_sources.end() && found->second->valid)pending[pending_count++]={source,found->second->version};
    };
    if(menu_open && menu_valid)add(menu_swapchain);
    if(elliott_mode()) {if(place_meter(info->displayTime,now_ns()))add(meter.swapchain);}
    else if(position!=FpsOverlayPosition::off && image_valid)add(swapchain);
    const std::span desired(pending.data(),pending_count);
    if(desired.empty() || (desired.size()==ui_published.size() && std::equal(desired.begin(),desired.end(),ui_published.begin())))return;
    if(!ui_atlas_held) {
        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        const auto result=runtime_acquire(ui_atlas,&acquire_info,&ui_atlas_index);
        if(result==XR_TIMEOUT_EXPIRED)return;
        if(result!=XR_SUCCESS){ui_atlas_failed=true;return;}
        ui_atlas_held=true;ui_atlas_waited=false;
    }
    if(!ui_atlas_waited) {
        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait_info.timeout=0;
        const auto result=runtime_wait(ui_atlas,&wait_info);
        if(result==XR_TIMEOUT_EXPIRED)return;
        if(result!=XR_SUCCESS){ui_atlas_failed=true;return;}
        ui_atlas_waited=true;
    }
    if(ui_atlas_index>=ui_atlas_images.size()){ui_atlas_failed=true;return;}
    auto& target=ui_atlas_images[ui_atlas_index];
    const auto completed=fence12?fence12->GetCompletedValue():fence11->GetCompletedValue();
    if(completed==UINT64_MAX){ui_atlas_failed=true;return;}
    if(completed<target.fence_value)return;
    const auto value=next_fence++;
    HRESULT result{};
    if(device11) {
        if(multithread11)multithread11->Enter();
        for(const auto& [token,version]:desired) {
            auto& source=*ui_sources.at(token);
            context11->CopySubresourceRegion(target.texture11.Get(),0,0,source.menu?0:ui_status_y,0,
                source.images[source.published].d11.Get(),0,nullptr);
        }
        fps_unfenced_submission=true;
        result=context11->Signal(fence11.Get(),value);context11->Flush();
        if(multithread11)multithread11->Leave();
    } else {
        if(FAILED(target.allocator->Reset()) || FAILED(target.list->Reset(target.allocator.Get(),nullptr)))return;
        const auto barrier=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
            D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource=resource;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore=from;b.Transition.StateAfter=to;target.list->ResourceBarrier(1,&b);
        };
        barrier(target.texture12.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
        for(const auto& [token,version]:desired) {
            auto& source=*ui_sources.at(token);auto* texture=source.images[source.published].d12.Get();
            barrier(texture,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};
            from.pResource=texture;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=target.texture12.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            target.list->CopyTextureRegion(&to,0,source.menu?0:ui_status_y,0,&from,nullptr);
            barrier(texture,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        barrier(target.texture12.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
        if(FAILED(target.list->Close()))return;
        ID3D12CommandList* lists[]{target.list.Get()};queue12->ExecuteCommandLists(1,lists);
        fps_unfenced_submission=true;result=queue12->Signal(fence12.Get(),value);
    }
    if(FAILED(result)){ui_atlas_failed=true;return;}
    fps_unfenced_submission=false;last_fence=target.fence_value=value;
    for(const auto& [token,version]:desired) {
        auto& source=*ui_sources.at(token);source.images[source.published].read_fence=value;
    }
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if(runtime_release(ui_atlas,&release_info)!=XR_SUCCESS){ui_atlas_failed=true;return;}
    ui_atlas_held=ui_atlas_waited=false;ui_published.assign(desired.begin(),desired.end());
}

void observe_ui_membership() {
    const unsigned mask=(menu_open?1u:0u)|(position!=FpsOverlayPosition::off?2u:0u);
    if(mask!=ui_pair_mask){ui_pair_mask=mask;ui_pair_choice.reset();}
}
bool retire_ui_for_policy() {
    image_valid=menu_valid=meter.image_valid=false;meter_paint.reset();
    if(swapchain) {
        if(!release_unused(swapchain,images,acquired,waited) || destroy_swapchain(swapchain)!=XR_SUCCESS)return false;
        swapchain={};images.clear();quad.subImage.swapchain={};fps_attempted=fps_initialized=false;
    }
    if(meter.swapchain) {
        if(!release_unused(meter.swapchain,meter.images,meter.acquired,meter.waited) || destroy_swapchain(meter.swapchain)!=XR_SUCCESS)return false;
        meter=MeterSurface{};
    }
    if(menu_swapchain) {
        if(!release_unused(menu_swapchain,menu_images,menu_acquired,menu_waited) || FAILED(menu_renderer.shutdown()) ||
            destroy_swapchain(menu_swapchain)!=XR_SUCCESS)return false;
        menu_swapchain={};menu_images.clear();menu_quad.subImage.swapchain={};
        menu_attempted=menu_initialized=false;
    }
    return true;
}
bool update_ui_policy(bool& refresh,std::int64_t now) {
    observe_ui_membership();
    if(ui_pair_mask==3 && !ui_pair_choice) {
        if(ui_native_count && ui_atlas_supported) {
            const auto total=ui_native_count();if(!total)return false;
            std::array<XrSwapchain,3> own{};unsigned owned{};
            for(const auto source:{swapchain,meter.swapchain,menu_swapchain}) {
                const auto native=public_ui_handle(source);
                if(native && std::find(own.begin(),own.begin()+owned,native)==own.begin()+owned)own[owned++]=native;
            }
            ui_projected_total=*total-std::min(*total,owned)+2;
            ui_pair_choice=ui_projected_total>=14;
        } else {ui_pair_choice=false;ui_projected_total=0;}
    }
    const bool desired=ui_pair_mask==3 && ui_pair_choice.value_or(false);
    if(desired==ui_atlas_enabled)return true;
    // Finish a mouse gesture before rebuilding its ImGui owner. Keep polling
    // and buffering input; no game image or producer/presenter schedule waits.
    if(std::any_of(pending_menu_input.mouse_down.begin(),pending_menu_input.mouse_down.end(),[](bool down){return down;}))return false;
    if(!retire_ui_for_policy())return false;
    // All old handles are returned first, so conversion never needs a spare
    // third UI allocation at the very native limit it is meant to relieve.
    ui_atlas_enabled=desired;ui_policy_warming=true;refresh=true;next_refresh=now+250'000'000;
    bridge_flight_logger().event(BridgeFlightOperation::menu_status,desired?1:0,12,ui_projected_total,0);
    return true;
}
void finish_ui_policy_refresh() {
    if(!ui_policy_warming)return;
    const auto published=[&](XrSwapchain source) {
        return !ui_sources.contains(source) || std::any_of(ui_published.begin(),ui_published.end(),[&](const auto& p){return p.first==source;});
    };
    const bool menu_ready=!menu_open || (menu_valid&&published(menu_swapchain));
    const bool counter_ready=position==FpsOverlayPosition::off ||
        (elliott_mode()?(!meter.initialized || meter.failed || (meter.image_valid&&published(meter.swapchain))):
            (!fps_initialized || (image_valid&&published(swapchain))));
    if(disabled || (menu_ready&&counter_ready))ui_policy_warming=false;
}
