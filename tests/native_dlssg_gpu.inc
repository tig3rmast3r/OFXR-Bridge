// Native DLSS-G GPU cases derived from Elliot commit
// 135185a033453ae7f844b867c7ef34ae26d40ab3. Real NGX evaluations,
// GPU readbacks and completion fences; no simulated frame-generation path.
#ifdef XRFG_NATIVE_DLSSG
[[nodiscard]] bool native_test_has_supported_nvidia_adapter() noexcept {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const auto result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) return false;
        if (FAILED(result)) return false;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x10deU &&
            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                __uuidof(ID3D12Device), nullptr))) return true;
    }
}

// NVIDIA exposes this driver annotation in production builds too. This
// read-only fixture never changes the global indicator setting. Only a DWORD
// with the documented value and the actual captured monochrome SDK header
// can exclude its 21-row annotation from the interpolation quality sample.
[[nodiscard]] bool ngx_sdk_indicator_enabled() noexcept {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0,
        KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return false;
    DWORD value{}, bytes = sizeof(value), type{};
    const auto result = RegQueryValueExW(key, L"ShowDlssIndicator", nullptr,
        &type, reinterpret_cast<BYTE*>(&value), &bytes);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && type == REG_DWORD && bytes == sizeof(value) && value == 1024;
}
[[nodiscard]] bool native_readback_contains_sdk_header(
    const StereoPattern& actual, UINT width, UINT height, bool cropped) noexcept {
    const UINT x0 = cropped ? 3 : 0, y0 = cropped ? 1 : 0;
    if (height < y0 + 21 || width < x0 + 16) return false;
    const UINT x1 = cropped ? width - 3 : width;
    const auto area = static_cast<std::size_t>(x1 - x0) * 21;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        if (actual[eye].size() != static_cast<std::size_t>(width) * height * 4) return false;
        std::size_t black{}, white{};
        for (UINT y = y0; y < y0 + 21; ++y) for (UINT x = x0; x < x1; ++x) {
            const auto i = (static_cast<std::size_t>(y) * width + x) * 4;
            const auto r = actual[eye][i], g = actual[eye][i + 1], b = actual[eye][i + 2];
            if (r == 0 && g == 0 && b == 0) ++black;
            else if (r == 255 && g == 255 && b == 255) ++white;
        }
        if (black > area / 3 && white > area / 20 && black + white > area * 3 / 5) return true;
    }
    return false;
}

[[nodiscard]] ComPtr<ID3D12Resource> create_native_test_texture(
    D3D12WarpFixture& fixture, UINT width, UINT height,
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET,
    UINT16 array_size = kEyeCount, DXGI_FORMAT format = kFormat) {
    auto desc = stereo_texture_description(width, height);
    desc.DepthOrArraySize = array_size;
    desc.Format = format;
    const auto heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> result;
    require_hresult(fixture.device()->CreateCommittedResource(&heap,
        D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&result)),
        "create native GPU test texture");
    return result;
}

[[nodiscard]] ComPtr<ID3D12Resource> create_native_test_depth(D3D12WarpFixture& fixture, UINT width, UINT height,
    float (*depth_at)(UINT eye, UINT x, UINT y) = nullptr) {
    ComPtr<ID3D12Resource> native_depth;
    auto description = stereo_texture_description(width,height);
    description.Format=DXGI_FORMAT_R32_FLOAT; description.Flags=D3D12_RESOURCE_FLAG_NONE;
    const auto hp=heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    require_hresult(fixture.device()->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,
        &description,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&native_depth)),"native depth creation");
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,kEyeCount> fp{};
    std::array<UINT,kEyeCount> rows{}; std::array<UINT64,kEyeCount> sizes{}; UINT64 total{};
    fixture.device()->GetCopyableFootprints(&description,0,kEyeCount,0,fp.data(),rows.data(),sizes.data(),&total);
    auto upload=create_buffer(fixture,D3D12_HEAP_TYPE_UPLOAD,total,D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped{}; require_hresult(upload->Map(0,nullptr,&mapped),"map native depth");
    for(UINT eye=0;eye<kEyeCount;++eye) for(UINT y=0;y<height;++y) {
        auto* row=reinterpret_cast<float*>(static_cast<std::byte*>(mapped)+fp[eye].Offset+SIZE_T(y)*fp[eye].Footprint.RowPitch);
        for(UINT x=0;x<width;++x) row[x]=depth_at?depth_at(eye,x,y):0.5F;
    }
    upload->Unmap(0,nullptr);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list){
        for(UINT eye=0;eye<kEyeCount;++eye) {
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=native_depth.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex=eye;
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=fp[eye];
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        }
        auto barrier=transition_barrier(native_depth.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(1,&barrier);
    });
    return native_depth;
}

void test_native_dlss_nvidia_prime(D3D12WarpFixture& fixture) {
    constexpr UINT width=256, height=128;
    auto source=create_source_texture(fixture,width,height);
    auto current=create_source_texture(fixture,width,height);
    auto synthetic=create_source_texture(fixture,width,height);
    const auto pattern=translated_motion_pattern(width,height,{0,0});
    upload_pattern(fixture,source.Get(),pattern);
    ID3D12Resource* sources[]{source.Get()}, *currents[]{current.Get()}, *synthetics[]{synthetic.Get()};
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    require_hresult(history->initialize(fixture.device(),fixture.queue(),sources,D3D12_RESOURCE_STATE_RENDER_TARGET),"native prime history");
    xrfg::D3D12FrameSynthesizer synthesizer;
    require_hresult(synthesizer.initialize(fixture.device(),fixture.queue(),history,currents,synthetics,
        kFormat,D3D12_RESOURCE_STATE_RENDER_TARGET,xrfg::D3D12OpticalFlowBackend::nvidia,{},false,
        xrfg::D3D12FrameGenerationMode::native_dlss),"native prime NVIDIA selector");
    auto motion=create_and_upload_game_motion(fixture,width/2,height/2,{0,0});
    auto depth=create_native_test_depth(fixture,width,height);
    auto guides=std::make_shared<xrfg::DlssMotionVectorSet>();guides->eye_count=2;
    for(UINT eye=0;eye<2;++eye) {
        auto g=std::make_shared<xrfg::DlssMotionVectorFrame>();g->stream=eye+1;g->epoch=1;g->serial=1;
        g->publication=eye+1;g->motion_vectors=motion;g->depth=depth;g->motion_slice=eye;
        g->motion_width=width/2;g->motion_height=height/2;g->depth_width=g->output_width=width;
        g->depth_height=g->output_height=height;guides->eyes[eye]=std::move(g);
    }
    const auto views=make_reprojection_views();
    for(UINT frame=0;frame<5;++frame) {
        xrfg::D3D12HistoryCaptureTicket capture{};xrfg::D3D12FrameSynthesisTicket ticket{};
        require_hresult(history->capture(0,&capture),"native prime capture");
        require_hresult(history->commit(capture),"native prime commit");
        require(synthesizer.submit_prime(capture,views,0,&ticket)==DXGI_ERROR_WAS_STILL_DRAWING,
            "missing native guides must refuse prime without work or anchor");
        require_hresult(synthesizer.submit_prime(capture,views,0,&ticket,guides),"native NVIDIA prime work-slot reuse");
        require_hresult(synthesizer.wait_for_idle(),"native prime completion");
        require(readback_pattern(fixture,current.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==pattern,
            "native prime must retain exact real stereo color");
    }
    std::cout<<"Native DLSS-G NVIDIA work-slot prime/reuse and missing-guide refusal passed\n";
}

void test_native_dlss_packed_stereo(D3D12WarpFixture& fixture, bool cropped,
                                    DXGI_FORMAT format = kFormat, bool recover_core = false,
                                    StereoPattern* observed = nullptr, bool resized_eye = false) {
    constexpr UINT width = 256, height = 128, margin = 16;
    const auto previous = translated_motion_pattern(width, height, {0, 0});
    const auto current = translated_motion_pattern(width, height, {8, -8});
    const auto expected = translated_motion_pattern(width, height, {4, -4});
    const auto blend = midpoint_pattern(previous, current);
    std::array<ComPtr<ID3D12Resource>, 2> stereo{
        create_native_test_texture(fixture, width, height, D3D12_RESOURCE_STATE_RENDER_TARGET,
                              kEyeCount, format),
        create_native_test_texture(fixture, width, height, D3D12_RESOURCE_STATE_RENDER_TARGET,
                              kEyeCount, format)};
    upload_pattern(fixture, stereo[0].Get(), previous);
    upload_pattern(fixture, stereo[1].Get(), current);
    std::array<ComPtr<ID3D12Resource>, 2> packed{
        create_native_test_texture(fixture, width * 2, height, D3D12_RESOURCE_STATE_COMMON, 1, format),
        create_native_test_texture(fixture, width * 2, height, D3D12_RESOURCE_STATE_COMMON, 1, format)};
    auto output = create_native_test_texture(
        fixture, width * 2, height, D3D12_RESOURCE_STATE_RENDER_TARGET, 1, format);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        for (UINT frame = 0; frame < 2; ++frame) {
            std::array barriers{
                transition_barrier(stereo[frame].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE),
                transition_barrier(packed[frame].Get(), D3D12_RESOURCE_STATE_COMMON,
                                   D3D12_RESOURCE_STATE_COPY_DEST)};
            list->ResourceBarrier(UINT(barriers.size()), barriers.data());
            for (UINT eye = 0; eye < kEyeCount; ++eye) {
                D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
                src.pResource = stereo[frame].Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = eye;
                dst.pResource = packed[frame].Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                list->CopyTextureRegion(&dst, eye * width, 0, 0, &src, nullptr);
            }
            for (auto& barrier : barriers)
                std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(UINT(barriers.size()), barriers.data());
        }
        // Native composition preserves pixels outside each projection rectangle.
        auto source = transition_barrier(packed[1].Get(), D3D12_RESOURCE_STATE_COMMON,
                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto destination = transition_barrier(output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                              D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &source);
        list->ResourceBarrier(1, &destination);
        list->CopyResource(output.Get(), packed[1].Get());
        std::swap(source.Transition.StateBefore, source.Transition.StateAfter);
        std::swap(destination.Transition.StateBefore, destination.Transition.StateAfter);
        list->ResourceBarrier(1, &source);
        list->ResourceBarrier(1, &destination);
    });
    auto motion = create_and_upload_game_motion(fixture, width / 2, height / 2, {-4, 4});
    auto depth = create_native_test_depth(fixture, width, height);
    xrfg::DlssMotionVectorSet a_guides{}, b_guides{};
    a_guides.eye_count = b_guides.eye_count = kEyeCount;
    auto views = make_reprojection_views();
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        auto a = std::make_shared<xrfg::DlssMotionVectorFrame>();
        a->stream = 17 + eye;
        a->epoch = 3;
        a->serial = 1;
        a->motion_vectors = motion;
        a->depth = depth;
        a->producer_queue = fixture.queue();
        a->output_x = eye * width;
        a->output_width = a->depth_width = width;
        a->output_height = a->depth_height = height;
        a->motion_width = width / 2;
        a->motion_height = height / 2;
        a->motion_slice = a->output_slice = eye;
        a->depth_resource_state = D3D12_RESOURCE_STATE_COMMON;
        auto b = std::make_shared<xrfg::DlssMotionVectorFrame>(*a);
        b->serial = 2;
        b->previous_serial = 1;
        a_guides.eyes[eye] = a;
        b_guides.eyes[eye] = b;
        views[eye].array_slice = 0;
        views[eye].image_rect = {
            eye * width + (cropped ? 3U : 0U), cropped ? 1U : 0U,
            cropped ? width - 6 : width, cropped ? height - 2 : height};
    }
    if (resized_eye) for (UINT eye=0; eye<kEyeCount; ++eye) {
        for(auto* set:{&a_guides,&b_guides}) {
            auto g=std::make_shared<xrfg::DlssMotionVectorFrame>(*set->eyes[eye]);
            g->output_x=0;g->output_width=width-4;g->output_height=height-2;
            g->camera_data_valid=g->camera_matrices_valid=true;
            const auto& f=views[eye].fov;
            const float left=std::tan(f.angle_left),right=std::tan(f.angle_right);
            const float up=std::tan(f.angle_up),down=std::tan(f.angle_down);
            g->camera_view_to_clip={};
            g->camera_view_to_clip[0]=2.F/(right-left);
            g->camera_view_to_clip[5]=2.F/(up-down);
            g->camera_view_to_clip[8]=-(right+left)/(right-left);
            g->camera_view_to_clip[9]=-(up+down)/(up-down);
            g->camera_view_to_clip[10]=1.F;g->camera_view_to_clip[11]=1.F;
            g->camera_view_to_clip[14]=-.1F;
            set->eyes[eye]=std::move(g);
        }
    }
    xrfg::D3D12NativeDlssG native;
    require_hresult(native.initialize(fixture.device(), fixture.queue(), packed[0]->GetDesc(),
                                      format), "packed native initialization");
    const std::array<xrfg::D3D12NativeDlssG::Output, 1> outputs{{{output.Get()}}};
    if (resized_eye) {
        // Unknown or narrower cameras must STILL fail bounds admission. The
        // fix may map a proven view; it must not waive the guard or mutate the
        // immutable publication shared with other generators.
        for(bool narrow:{false,true}) {
            auto bad_a=a_guides,bad_b=b_guides;
            for(auto* set:{&bad_a,&bad_b}) for(auto& frame:set->eyes)if(frame) {
                auto bad=std::make_shared<xrfg::DlssMotionVectorFrame>(*frame);
                bad->camera_matrices_valid=narrow;
                bad->camera_view_to_clip[0]*=2.F;frame=std::move(bad);
            }
            fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
                require(native.record(list,0,packed[0].Get(),packed[1].Get(),views,views,
                    &bad_a,&bad_b,outputs,D3D12_RESOURCE_STATE_RENDER_TARGET)==S_FALSE &&
                    native.last_skip()==xrfg::D3D12NativeDlssG::Skip::guides,
                    "resized-eye mapping admitted an unknown/narrow native camera");
            });
        }
    }
    if (recover_core) {
        // The real old provider advertises FG and creates a feature, but
        // rejects evaluation. Both refused pairs must remain non-generated.
        for (UINT attempt = 0; attempt < 2; ++attempt) {
            fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
                require(native.record(list, attempt, packed[0].Get(), packed[1].Get(), views, views,
                    &a_guides, &b_guides, outputs, D3D12_RESOURCE_STATE_RENDER_TARGET) == S_FALSE,
                    "incompatible core/recovery pair must refuse interpolation");
                require(native.last_skip() == xrfg::D3D12NativeDlssG::Skip::evaluate_failed,
                    "compatibility recovery must retain the actual evaluation refusal");
            });
            native.submitted(fixture.completion_fence(), fixture.last_submitted_value());
            auto refused = native.last_result();
            require(refused && !refused->generated(), "a rejected core/recovery pair turned green");
        }
    }
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        require(native.record(list, 0, packed[0].Get(), packed[1].Get(), views, views,
                              &a_guides, &b_guides, outputs,
                              D3D12_RESOURCE_STATE_RENDER_TARGET) == S_OK,
                "packed native generation did not consume both eye guides");
    });
    auto receipt=native.last_result();
    require(receipt && receipt->resolve()==xrfg::D3D12NativeFrameResult::pending,
        "an unsubmitted native decision must not turn the counter green");
    native.submitted(fixture.completion_fence(), fixture.last_submitted_value());
    require(receipt->generated(), "actual packed interpolation must have an enabled GPU decision");
    if(resized_eye)for(UINT eye=0;eye<kEyeCount;++eye)
        require(a_guides.eyes[eye]->output_width==width-4 && b_guides.eyes[eye]->output_x==0 &&
            !a_guides.eyes[eye]->placed && !b_guides.eyes[eye]->placed,
            "native placement modified the original shared guide publication");
    auto unpacked = create_native_test_texture(fixture, width, height,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, kEyeCount, format);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        std::array barriers{
            transition_barrier(output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition_barrier(unpacked.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_COPY_DEST)};
        list->ResourceBarrier(UINT(barriers.size()), barriers.data());
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = output.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = unpacked.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = eye;
            const D3D12_BOX box{eye * width, 0, 0, (eye + 1) * width, height, 1};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        }
        for (auto& barrier : barriers)
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(UINT(barriers.size()), barriers.data());
    });
    const auto actual = readback_pattern(fixture, unpacked.Get(),
                                         D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (observed) *observed = actual;
    const bool sdk_header = ngx_sdk_indicator_enabled() &&
        native_readback_contains_sdk_header(actual, width, height, cropped);
    const UINT quality_margin = sdk_header ? 24 : margin;
    if (sdk_header) std::cout << "Confirmed NVIDIA driver SDK indicator: scoring below its 21-row annotation\n";
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        const double error = mean_absolute_rgb_error(actual, expected, width, height, eye, quality_margin);
        const double blend_error = mean_absolute_rgb_error(blend, expected, width, height, eye, quality_margin);
        std::cout << "Native packed stereo eye=" << eye << " cropped=" << cropped
                  << " srgb=" << (format != kFormat) << " synthetic_mae=" << error
                  << " blend_mae=" << blend_error << '\n';
        if (error >= 0.02) {
            double best_error = std::numeric_limits<double>::max();
            int best_translation = 0;
            for (int shift = -16; shift <= 20; ++shift) {
                std::array<int, kEyeCount> translations{};
                translations[eye] = shift;
                const auto candidate = translated_motion_pattern(width, height, translations);
                const auto candidate_error = mean_absolute_rgb_error(actual, candidate, width, height, eye, quality_margin);
                if (candidate_error < best_error) { best_error = candidate_error; best_translation = shift; }
            }
            std::cout << "Native best-fit horizontal translation=" << best_translation
                      << " MAE=" << best_error << " expected=" << (eye ? -4 : 4) << '\n';
        }
        require(error < 0.02 && error < blend_error,
                "native packed stereo did not interpolate eye " + std::to_string(eye));
        if (cropped) for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
            if (x >= 3 && x < width - 3 && y >= 1 && y < height - 1) continue;
            const SIZE_T offset = (SIZE_T(y) * width + x) * 4;
            require(std::equal(actual[eye].begin() + offset, actual[eye].begin() + offset + 4,
                               current[eye].begin() + offset),
                    "native packed stereo wrote outside the eye viewport");
        }
    }
    fixture.require_no_debug_errors();
}

void test_native_dlss_reseeds_only_for_camera_motion(D3D12WarpFixture& fixture) {
    constexpr UINT width = 256, height = 128;
    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_native_test_texture(fixture, width, height),
        create_native_test_texture(fixture, width, height)};
    const auto pattern = translated_motion_pattern(width, height, {0, 0});
    for (auto& source : sources) upload_pattern(fixture, source.Get(), pattern);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        for (auto& source : sources) {
            auto ready = transition_barrier(source.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                             D3D12_RESOURCE_STATE_COMMON);
            list->ResourceBarrier(1, &ready);
        }
    });
    auto output = create_native_test_texture(fixture, width, height);
    auto motion = create_and_upload_game_motion(fixture, width / 2, height / 2, {0, 0});
    auto depth = create_native_test_depth(fixture, width, height);
    const std::uint32_t frames = xrfg::native_dlssg_max_generated_frames(fixture.device());
    std::cout << "native max generated frames=" << frames << '\n';
    require(frames >= 1, "native frame generation reported unavailable");
    xrfg::D3D12NativeDlssG native;
    require_hresult(native.initialize(fixture.device(), fixture.queue(), sources[0]->GetDesc(),
                                      kFormat), "reseed native initialization");
    const std::array<xrfg::D3D12NativeDlssG::Output, 1> outputs{{{output.Get()}}};
    std::uint64_t serial = 1;
    const auto pair = [&](const ReprojectionViews& a_views, const ReprojectionViews& b_views) {
        xrfg::DlssMotionVectorSet a_guides{}, b_guides{};
        a_guides.eye_count = b_guides.eye_count = kEyeCount;
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            for (const UINT next : {0U, 1U}) {
                auto f = std::make_shared<xrfg::DlssMotionVectorFrame>();
                f->stream = 61 + eye;
                f->epoch = 1;
                f->serial = serial + next;
                f->previous_serial = serial + next - 1;
                f->motion_vectors = motion;
                f->depth = depth;
                f->producer_queue = fixture.queue();
                f->output_width = f->depth_width = width;
                f->output_height = f->depth_height = height;
                f->motion_width = width / 2;
                f->motion_height = height / 2;
                f->motion_slice = f->output_slice = eye;
                f->depth_resource_state = D3D12_RESOURCE_STATE_COMMON;
                (next ? b_guides : a_guides).eyes[eye] = f;
            }
        }
        ++serial;
        HRESULT recorded = E_FAIL;
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            recorded = native.record(list, 0, sources[0].Get(), sources[1].Get(), a_views,
                                     b_views, &a_guides, &b_guides, outputs,
                                     D3D12_RESOURCE_STATE_RENDER_TARGET);
        });
        native.submitted(fixture.completion_fence(), fixture.last_submitted_value());
        require(recorded == S_OK, "reseed native pair was not generated");
        return native.reseeds();
    };
    const auto still = make_reprojection_views();
    require(pair(still, still) == kEyeCount, "the first native pair did not seed each eye");
    require(pair(still, still) == kEyeCount, "a still camera reseeded the native history");
    auto moved = still;
    for (auto& view : moved) view.pose.position.z += 0.05F;
    require(pair(still, moved) == kEyeCount, "a translation alone reseeded the native history");
    require(pair(still, make_reprojection_views(0.000001F)) == kEyeCount,
            "a sub-pixel rotation reseeded the native history");
    require(pair(still, make_reprojection_views(0.01F)) == 2 * kEyeCount,
            "a turning camera did not reseed the native history");
    fixture.require_no_debug_errors();
}

// Real rolling synthesis, native guide refusal, copied pixels and recovery.
void test_native_counter_bypass(D3D12WarpFixture& fixture) {
    constexpr UINT width=256,height=128;
    auto source=create_source_texture(fixture,width,height);
    auto real=create_source_texture(fixture,width,height);
    auto synthetic=create_source_texture(fixture,width,height);
    auto motion=create_and_upload_game_motion(fixture,width/2,height/2,{0,0});
    auto depth=create_native_test_depth(fixture,width,height);
    ID3D12Resource* sources[]{source.Get()},*reals[]{real.Get()},*synthetics[]{synthetic.Get()};
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    require_hresult(history->initialize(fixture.device(),fixture.queue(),sources,
        D3D12_RESOURCE_STATE_RENDER_TARGET),"native counter history");
    xrfg::D3D12FrameSynthesizer synthesis;
    require_hresult(synthesis.initialize(fixture.device(),fixture.queue(),history,reals,synthetics,
        kFormat,D3D12_RESOURCE_STATE_RENDER_TARGET,xrfg::D3D12OpticalFlowBackend::nvidia,{},false,
        xrfg::D3D12FrameGenerationMode::native_dlss),"native counter synthesis");
    const auto guides=[&](UINT serial,UINT previous) {
        auto set=std::make_shared<xrfg::DlssMotionVectorSet>();set->eye_count=2;
        for(UINT eye=0;eye<2;++eye) {
            auto g=std::make_shared<xrfg::DlssMotionVectorFrame>();
            g->stream=eye+1;g->epoch=1;g->serial=serial;g->previous_serial=previous;
            g->publication=serial*2+eye;g->motion_vectors=motion;g->depth=depth;
            g->producer_queue=fixture.queue();g->motion_slice=g->output_slice=eye;
            g->motion_width=width/2;g->motion_height=height/2;
            g->depth_width=g->output_width=width;g->depth_height=g->output_height=height;
            set->eyes[eye]=g;
        }
        return set;
    };
    const auto capture=[&](const StereoPattern& pixels) {
        upload_pattern(fixture,source.Get(),pixels);
        xrfg::D3D12HistoryCaptureTicket ticket;
        require_hresult(history->capture(0,&ticket),"native counter capture");
        require_hresult(history->commit(ticket),"native counter commit");
        return ticket;
    };
    auto views=make_reprojection_views();
    xrfg::D3D12FrameSynthesisTicket prime,bypassed,recovered;
    const auto a=translated_motion_pattern(width,height,{0,0});
    const auto b=translated_motion_pattern(width,height,{6,-6});
    const auto c=translated_motion_pattern(width,height,{6,-6});
    require_hresult(synthesis.submit_prime(capture(a),views,0,&prime,guides(1,0)),"native counter prime");
    require_hresult(synthesis.wait_for_previous_submission(5000),"native counter prime GPU");
    // B's backward motion references serial 2, which is absent from this history.
    require_hresult(synthesis.submit_pair(capture(b),views,views,0,0,&bypassed,
        std::nullopt,guides(3,2)),"native counter rejected pair remains safe");
    require_hresult(synthesis.wait_for_previous_submission(5000),"native counter bypass GPU");
    require(bypassed.native_result && !bypassed.native_result->generated() &&
        bypassed.native_result->skip==1 && bypassed.native_result->detail==5,
        "native refusal must retain the exact temporal reason in its ticket");
    require(readback_pattern(fixture,synthetic.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==b,
        "native bypass must contain B, not interpolated pixels");
    const std::array skipped{bypassed.native_result};
    auto metrics=xrfg::classify_native_frame_content(skipped,0,true,true);
    require(!metrics.synthetic && !metrics.new_content && metrics.native_bypass,
        "a copied native S must neither count twice nor turn green");
    metrics=xrfg::classify_native_frame_content(skipped,0,false,true);
    require(!metrics.synthetic && metrics.new_content && metrics.native_bypass,
        "the real B after a skipped S must count once");
    const auto capture_c=capture(c);
    require_hresult(synthesis.submit_pair(capture_c,views,views,0,0,&recovered,
        std::nullopt,guides(4,3)),"native counter resumes on continuous guides");
    require_hresult(synthesis.wait_for_previous_submission(5000),"native counter recovered GPU");
    require(recovered.native_result && recovered.native_result->generated(),
        "native interpolation must resume after a rejected pair");
    const std::array valid{recovered.native_result};
    metrics=xrfg::classify_native_frame_content(valid,0,true,true);
    require(metrics.synthetic && metrics.new_content && !metrics.native_bypass,
        "actual native generation must count and turn green");
    metrics=xrfg::classify_native_frame_content(valid,0,true,false);
    require(!metrics.synthetic && !metrics.new_content,
        "holding a generated frame must not count it again");
    xrfg::D3D12FrameSynthesisTicket repeated;
    require_hresult(synthesis.submit_pair(capture_c,views,views,0,0,&repeated,
        std::nullopt,guides(4,3)),"native counter repeated capture");
    require_hresult(synthesis.wait_for_previous_submission(5000),"native counter repeated GPU");
    require(repeated.native_result && !repeated.native_result->generated() &&
        repeated.native_result->detail==20,"repeated captures must not inherit a prior native success");
    // Gate only B's copy: S's completed NGX decision must resolve before that
    // deferred list is submitted, without waiting or misclassifying it as bypass.
    xrfg::D3D12FrameSynthesisTicket deferred;
    require_hresult(synthesis.submit_pair(capture(c),views,views,0,0,&deferred,
        std::nullopt,guides(5,4),true),"native counter deferred real copy");
    fixture.execute_and_wait([](ID3D12GraphicsCommandList*){});
    require(deferred.native_result && deferred.native_result->generated(),
        "completed S decision must not wait for B's unsubmitted fence");
    require_hresult(synthesis.flush_current_copy(),"native counter submit deferred B");
    require_hresult(synthesis.wait_for_previous_submission(5000),"native counter deferred B completes");
    // Recycle every GPU work slot; the old bypass receipt must remain immutable.
    for(UINT serial=6;serial<15;++serial) {
        xrfg::D3D12FrameSynthesisTicket ticket;
        require_hresult(synthesis.submit_pair(capture(c),views,views,0,0,&ticket,
            std::nullopt,guides(serial,serial-1)),"native counter work-slot reuse");
        require_hresult(synthesis.wait_for_previous_submission(5000),"native counter reuse GPU");
        require(ticket.native_result && ticket.native_result->generated(),"reused native work slot decision");
    }
    require(!bypassed.native_result->generated() && recovered.native_result->generated(),
        "work-slot reuse must not rewrite an already presented decision");
    fixture.require_no_debug_errors();
    std::cout<<"Native real-copy counter refusal, exact reason, recovery and receipt reuse passed\n";
}
#endif
