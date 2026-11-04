enum class HybridVectorScene { occlusion, overlay, fade };
// Which optical flow the hybrid composes with: FidelityFX, or NVIDIA's on the
// 0.2.1 (legacy) or 0.3.0 (pose-compensated) route.
enum class HybridFlow { fidelity_fx, nvidia_legacy, nvidia_new };
// hybrid composes with that flow as well as the game's vectors.
void test_hybrid_motion_vector_occlusion_edges(D3D12WarpFixture& fixture, HybridVectorScene kind,
                                             bool hybrid = false, bool quarter = false,
                                             HybridFlow flow = HybridFlow::fidelity_fx) {
    constexpr UINT width = 256, height = 128;
    constexpr int object_left = 72, object_width = 80, object_top = 32, object_bottom = 96;
    constexpr int background_motion = 6;
    const bool overlay = kind == HybridVectorScene::overlay, fade = kind == HybridVectorScene::fade;
    const int object_motion = overlay ? 0 : fade ? background_motion : 20;
    const auto make_scene = [&](int background_shift, int object_shift, float brightness = 1.0F) {
        StereoPattern pattern;
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            auto& bytes = pattern[eye];
            bytes.resize(static_cast<std::size_t>(width) * height * kBytesPerPixel);
            for (UINT y = 0; y < height; ++y) {
                for (UINT x = 0; x < width; ++x) {
                    const int ox = static_cast<int>(x) - object_shift;
                    const bool object = static_cast<int>(y) >= object_top &&
                        static_cast<int>(y) < object_bottom && ox >= object_left &&
                        ox < object_left + object_width;
                    const int bx = static_cast<int>(x) - background_shift + 64;
                    const RgbaBytes color = object
                        ? ((ox / 5) % 2 ? RgbaBytes{240, 210, 40, 255} : RgbaBytes{30, 60, 200, 255})
                        : RgbaBytes{static_cast<std::uint8_t>(40 + (bx * 13 + static_cast<int>(y) * 5) % 160),
                                    static_cast<std::uint8_t>(60 + ((bx / 3) % 7) * 20 + eye * 9),
                                    static_cast<std::uint8_t>(90 + (bx * 7) % 120), 255};
                    const RgbaBytes lit{static_cast<std::uint8_t>(std::lround(color[0] * brightness)),
                                        static_cast<std::uint8_t>(std::lround(color[1] * brightness)),
                                        static_cast<std::uint8_t>(std::lround(color[2] * brightness)), 255};
                    std::copy(lit.begin(), lit.end(), bytes.begin() +
                        (static_cast<std::size_t>(y) * width + x) * kBytesPerPixel);
                }
            }
        }
        return pattern;
    };
    const StereoPattern previous = make_scene(0, 0);
    const StereoPattern current = make_scene(background_motion, object_motion, fade ? 0.6F : 1.0F);
    const StereoPattern expected =
        make_scene(background_motion / 2, object_motion / 2, fade ? 0.8F : 1.0F);
    auto game_motion = create_and_upload_game_motion_field(
        fixture, width, height, [&](UINT, UINT x, UINT y) {
            const int ox = static_cast<int>(x) - object_motion;
            const bool object = static_cast<int>(y) >= object_top &&
                static_cast<int>(y) < object_bottom && ox >= object_left &&
                ox < object_left + object_width;
            return std::array<float, 2>{
                static_cast<float>(-(object && !overlay ? object_motion : background_motion)), 0.0F};
        });
    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height)};
    std::array<ComPtr<ID3D12Resource>, 2> current_destinations{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height)};
    std::array<ComPtr<ID3D12Resource>, 1> synthetic_destinations{
        create_source_texture(fixture, width, height)};
    upload_pattern(fixture, sources[0].Get(), previous);
    upload_pattern(fixture, sources[1].Get(), current);
    std::array<ID3D12Resource*, 2> source_pointers{sources[0].Get(), sources[1].Get()};
    std::array<ID3D12Resource*, 2> current_pointers{
        current_destinations[0].Get(), current_destinations[1].Get()};
    std::array<ID3D12Resource*, 1> synthetic_pointers{synthetic_destinations[0].Get()};
    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(operation_succeeded(history->initialize(fixture.device(), fixture.queue(),
                source_pointers, D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "occlusion history initialization failed");
    xrfg::D3D12FrameSynthesizer synthesizer;
    xrfg::D3D12NvidiaOpticalFlowOptions options;
    options.hybrid = hybrid;
    if (hybrid && !quarter) options.input_scale = xrfg::D3D12NvidiaInputScale::half;
    if (quarter) options.input_scale = xrfg::D3D12NvidiaInputScale::quarter;
    const bool nvidia = flow != HybridFlow::fidelity_fx;
    if (flow == HybridFlow::nvidia_new) {
        options.grayscale_input = options.pose_compensated_input = true;
        options.input_encoding = xrfg::flow_encoding_for_route(true);
    }
    require(operation_succeeded(synthesizer.initialize(fixture.device(), fixture.queue(),
                history, current_pointers, synthetic_pointers, kFormat,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                nvidia ? xrfg::D3D12OpticalFlowBackend::nvidia : xrfg::D3D12OpticalFlowBackend::fidelity_fx,
                options)),
        "occlusion synthesizer initialization failed");
    const auto guides = [&](std::uint64_t serial) {
        auto set = std::make_shared<xrfg::DlssMotionVectorSet>();
        set->eye_count = kEyeCount;
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            auto f = std::make_shared<xrfg::DlssMotionVectorFrame>();
            f->stream = 311 + eye;
            f->epoch = 3;
            f->serial = serial;
            f->previous_serial = serial - 1;
            f->motion_vectors = game_motion;
            f->producer_queue = fixture.queue();
            f->output_width = f->motion_width = width;
            f->output_height = f->motion_height = height;
            f->motion_slice = f->output_slice = eye;
            f->resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            set->eyes[eye] = f;
        }
        return set;
    };
    const ReprojectionViews views = make_reprojection_views();
    xrfg::D3D12HistoryCaptureTicket capture_a{}, capture_b{};
    require(operation_succeeded(history->capture(0, &capture_a)) &&
            operation_succeeded(history->commit(capture_a)),
        "occlusion capture A failed");
    xrfg::D3D12FrameSynthesisTicket prime{}, pair{};
    require(operation_succeeded(synthesizer.submit_prime(capture_a, views, 0, &prime, guides(1))),
        "occlusion prime failed");
    require_frame_start_gate(synthesizer, "occlusion frame-start gate");
    require(operation_succeeded(history->capture(1, &capture_b)) &&
            operation_succeeded(history->commit(capture_b)),
        "occlusion capture B failed");
    require(operation_succeeded(synthesizer.submit_pair(capture_b, views, views, 0, 1, &pair,
                std::nullopt, guides(2))),
        "occlusion pair failed");
    fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
    const StereoPattern actual = readback_pattern(fixture, synthetic_destinations[0].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto blend = midpoint_pattern(previous, current);
    // The bands either side of each edge of the object at the midpoint.
    const int left = object_left + object_motion / 2, right = left + object_width;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        double total = 0, blend_total = 0;
        std::size_t count = 0;
        for (int y = object_top + 2; y < object_bottom - 2; ++y) {
            for (int x = 0; x < static_cast<int>(width); ++x) {
                // The overlay is measured whole, the fade everywhere.
                if (fade ? x < 16 || x >= static_cast<int>(width) - 16
                    : overlay ? x < left - 2 || x >= right + 2
                              : std::abs(x - left) >= 12 && std::abs(x - right) >= 12) continue;
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * width + static_cast<UINT>(x)) * kBytesPerPixel;
                for (UINT c = 0; c < 3; ++c, ++count) {
                    blend_total += std::abs(static_cast<int>(blend[eye][offset + c]) - static_cast<int>(expected[eye][offset + c]));
                    total += std::abs(static_cast<int>(actual[eye][offset + c]) -
                                      static_cast<int>(expected[eye][offset + c]));
                }
            }
        }
        const double edge_error = total / static_cast<double>(count);
        const double blend_error = blend_total / static_cast<double>(count);
        std::cout << "quarter=" << quarter << " blend_mae=" << blend_error << " " << (hybrid ? "hybrid " : "")
                  << (flow == HybridFlow::nvidia_new ? "nvidia-0.3.0 " : flow == HybridFlow::nvidia_legacy ? "nvidia-0.2.1 " : "")
                  << "DLSS "
                  << (fade ? "fade" : overlay ? "overlay" : "occlusion") << " eye=" << eye
                  << " edge_mae=" << edge_error << '\n';
        // Preserve the existing vectors-only thresholds. Hybrid includes the
        // accepted R15 composition and R9 coarse packing: require an actual
        // improvement over same-pixel blending on these identical edge pixels.
        require(hybrid && !overlay ? edge_error < blend_error : edge_error <= (fade ? 1.0 : overlay ? 12.0 : hybrid ? 5.0 : 2.0),
            "DLSS vectors showed the wrong frame around a moving edge for eye " +
                std::to_string(eye) + ": " + std::to_string(edge_error));
    }
    require(operation_succeeded(synthesizer.wait_for_idle()), "occlusion final drain failed");
    require(operation_succeeded(history->invalidate()), "occlusion history invalidate failed");
}

// A per-eye depth field (R32_FLOAT, two slices), for the guides' depth.
template <typename DepthForPixel>
[[nodiscard]] ComPtr<ID3D12Resource> create_and_upload_game_depth_field(
    D3D12WarpFixture& fixture, UINT width, UINT height, DepthForPixel&& depth_for_pixel) {
    D3D12_RESOURCE_DESC description = stereo_texture_description(width, height);
    description.Format = DXGI_FORMAT_R32_FLOAT;
    description.Flags = D3D12_RESOURCE_FLAG_NONE;
    const D3D12_HEAP_PROPERTIES properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> texture;
    require_hresult(fixture.device()->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
                        &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                        IID_PPV_ARGS(texture.GetAddressOf())),
        "ID3D12Device::CreateCommittedResource(game depth)");
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount> layouts{};
    std::array<UINT, kEyeCount> row_counts{};
    std::array<UINT64, kEyeCount> row_sizes{};
    UINT64 total_size = 0;
    get_copy_layouts(fixture.device(), description, &layouts, &row_counts, &row_sizes, &total_size);
    ComPtr<ID3D12Resource> upload = create_buffer(
        fixture, D3D12_HEAP_TYPE_UPLOAD, total_size, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    require_hresult(upload->Map(0, &no_read, &mapped), "ID3D12Resource::Map(game depth upload)");
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        for (UINT y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<float*>(static_cast<std::byte*>(mapped) +
                layouts[eye].Offset + static_cast<std::size_t>(y) * layouts[eye].Footprint.RowPitch);
            for (UINT x = 0; x < width; ++x) row[x] = depth_for_pixel(eye, x, y);
        }
    }
    upload->Unmap(0, nullptr);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = layouts[eye];
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = texture.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = eye;
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        const D3D12_RESOURCE_BARRIER ready = transition_barrier(texture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &ready);
    });
    return texture;
}

// 0.2 V505/V555: a thin near bar sweeps across background both frames show.
// Without the depth search the solve finds the background and the bars go
// missing from the generated frame; with it they stay whole. outputs 1-3 is
// 2X-4X, each output checked at its own instant.
double test_hybrid_bars(D3D12WarpFixture& fixture, bool with_depth,
    xrfg::D3D12OpticalFlowBackend backend = xrfg::D3D12OpticalFlowBackend::fidelity_fx,
    UINT length = 256, bool vertical = false, UINT outputs = 1) {
    const UINT width = vertical ? 128U : length;
    const UINT height = vertical ? length : 128U;
    // Along the motion (u) and across it (v), in image pixels.
    const auto along = [&](UINT x, UINT y) { return static_cast<int>(vertical ? y : x); };
    const auto across = [&](UINT x, UINT y) { return static_cast<int>(vertical ? x : y); };
    constexpr int bar_width = 3, bar_spacing = 28, bar_first = 20, bar_count = 7;
    constexpr int bar_top = 16, bar_bottom = 112;
    constexpr int background_motion = 4, bar_motion = 16;
    const auto bar_at = [&](int x, int y, int shift) {
        if (y < bar_top || y >= bar_bottom) return false;
        const int local = x - shift - bar_first;
        return local >= 0 && local < bar_spacing * bar_count && local % bar_spacing < bar_width;
    };
    const auto make_scene = [&](int background_shift, int bar_shift) {
        StereoPattern pattern;
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            auto& bytes = pattern[eye];
            bytes.resize(static_cast<std::size_t>(width) * height * kBytesPerPixel);
            for (UINT y = 0; y < height; ++y) {
                for (UINT x = 0; x < width; ++x) {
                    const int bx = along(x, y) - background_shift + 64;
                    const RgbaBytes color = bar_at(along(x, y), across(x, y), bar_shift)
                        ? RgbaBytes{20, 20, 24, 255}
                        : RgbaBytes{static_cast<std::uint8_t>(80 + (bx * 13 + across(x, y) * 5) % 160),
                                    static_cast<std::uint8_t>(90 + ((bx / 3) % 7) * 20 + eye * 9),
                                    static_cast<std::uint8_t>(100 + (bx * 7) % 120), 255};
                    std::copy(color.begin(), color.end(), bytes.begin() +
                        (static_cast<std::size_t>(y) * width + x) * kBytesPerPixel);
                }
            }
        }
        return pattern;
    };
    const StereoPattern previous = make_scene(0, 0);
    const StereoPattern current = make_scene(background_motion, bar_motion);
    auto game_motion = create_and_upload_game_motion_field(
        fixture, width, height, [&](UINT, UINT x, UINT y) {
            const bool bar = bar_at(along(x, y), across(x, y), bar_motion);
            const float motion = static_cast<float>(-(bar ? bar_motion : background_motion));
            return vertical ? std::array<float, 2>{0.0F, motion} : std::array<float, 2>{motion, 0.0F};
        });
    // Inverted depth, as DI2's and MSFS's DLSS: larger is nearer.
    auto game_depth = create_and_upload_game_depth_field(
        fixture, width, height, [&](UINT, UINT x, UINT y) {
            return bar_at(along(x, y), across(x, y), bar_motion) ? 0.5F : 0.01F;
        });
    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_source_texture(fixture, width, height), create_source_texture(fixture, width, height)};
    std::array<ComPtr<ID3D12Resource>, 2> current_destinations{
        create_source_texture(fixture, width, height), create_source_texture(fixture, width, height)};
    std::array<ComPtr<ID3D12Resource>, 3> synthetic_destinations{
        create_source_texture(fixture, width, height), create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height)};
    upload_pattern(fixture, sources[0].Get(), previous);
    upload_pattern(fixture, sources[1].Get(), current);
    std::array<ID3D12Resource*, 2> source_pointers{sources[0].Get(), sources[1].Get()};
    std::array<ID3D12Resource*, 2> current_pointers{
        current_destinations[0].Get(), current_destinations[1].Get()};
    std::array<ID3D12Resource*, 3> synthetic_pointers{synthetic_destinations[0].Get(),
        synthetic_destinations[1].Get(), synthetic_destinations[2].Get()};
    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(operation_succeeded(history->initialize(fixture.device(), fixture.queue(),
                source_pointers, D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "bars history initialization failed");
    xrfg::D3D12FrameSynthesizer synthesizer;
    xrfg::D3D12NvidiaOpticalFlowOptions options;
    options.hybrid = true;
    require(operation_succeeded(synthesizer.initialize(fixture.device(), fixture.queue(),
                history, current_pointers, synthetic_pointers, kFormat,
                D3D12_RESOURCE_STATE_RENDER_TARGET, backend, options)),
        "bars synthesizer initialization failed");
    const auto guides = [&](std::uint64_t serial) {
        auto set = std::make_shared<xrfg::DlssMotionVectorSet>();
        set->eye_count = kEyeCount;
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            auto frame = std::make_shared<xrfg::DlssMotionVectorFrame>();
            frame->stream = 511 + eye;
            frame->epoch = 3;
            frame->serial = serial;
            frame->previous_serial = serial - 1;
            frame->motion_vectors = game_motion;
            frame->producer_queue = fixture.queue();
            frame->output_width = frame->motion_width = width;
            frame->output_height = frame->motion_height = height;
            frame->motion_slice = frame->output_slice = eye;
            frame->resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if (with_depth) {
                frame->depth = game_depth;
                frame->depth_width = width;
                frame->depth_height = height;
                frame->depth_inverted = true;
                frame->depth_resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
            set->eyes[eye] = frame;
        }
        return set;
    };
    const ReprojectionViews views = make_reprojection_views();
    xrfg::D3D12HistoryCaptureTicket capture_a{}, capture_b{};
    require(operation_succeeded(history->capture(0, &capture_a)) &&
            operation_succeeded(history->commit(capture_a)),
        "bars capture A failed");
    xrfg::D3D12FrameSynthesisTicket prime{}, pair{};
    require(operation_succeeded(synthesizer.submit_prime(capture_a, views, 0, &prime, guides(1))),
        "bars prime failed");
    require_frame_start_gate(synthesizer, "bars frame-start gate");
    require(operation_succeeded(history->capture(1, &capture_b)) &&
            operation_succeeded(history->commit(capture_b)),
        "bars capture B failed");
    // 2X the midpoint; 3X two outputs at the midpoint, as 0.2's test; 4X a
    // quarter, half and three quarters of the way.
    const std::array<float, 3> fractions = outputs == 3
        ? std::array<float, 3>{0.25F, 0.5F, 0.75F} : std::array<float, 3>{0.5F, 0.5F, 0.5F};
    const auto extra = outputs >= 2
        ? std::make_optional(xrfg::D3D12ExtraSynthetic{1U, fractions[1], {},
              outputs == 3 ? 2U : UINT32_MAX, fractions[2]})
        : std::nullopt;
    require(operation_succeeded(synthesizer.submit_pair(capture_b, views, views, 0, 1, &pair,
                std::nullopt, guides(2), false, fractions[0], {}, extra)),
        "bars pair failed");
    fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
    double coverage_total = 0;
    for (UINT output = 0; output < outputs; ++output) {
        const StereoPattern actual = readback_pattern(fixture, synthetic_destinations[output].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        const int shift = static_cast<int>(std::lround(bar_motion * fractions[output]));
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            std::size_t bar_pixels = 0, shown = 0;
            for (int v = bar_top + 4; v < bar_bottom - 4; ++v) {
                for (int u = 0; u < static_cast<int>(length); ++u) {
                    if (!bar_at(u, v, shift)) continue;
                    const UINT x = static_cast<UINT>(vertical ? v : u);
                    const UINT y = static_cast<UINT>(vertical ? u : v);
                    const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * kBytesPerPixel;
                    ++bar_pixels;
                    // Bar is near black, the background at least 80 a channel.
                    const int brightness = (actual[eye][offset] + actual[eye][offset + 1] +
                        actual[eye][offset + 2]) / 3;
                    if (brightness < 50) ++shown;
                }
            }
            const double coverage = static_cast<double>(shown) / static_cast<double>(bar_pixels);
            std::cout << "hybrid bars " << (with_depth ? "with" : "without") << " depth"
                      << (backend == xrfg::D3D12OpticalFlowBackend::nvidia ? " nvidia" : "")
                      << " eye=" << eye << " length=" << length << (vertical ? " vertical" : "")
                      << " outputs=" << outputs << " output=" << output
                      << " bar_coverage=" << coverage << '\n';
            coverage_total += coverage;
        }
    }
    require(operation_succeeded(synthesizer.wait_for_idle()), "bars final drain failed");
    require(operation_succeeded(history->invalidate()), "bars history invalidate failed");
    return coverage_total / static_cast<double>(kEyeCount * outputs);
}
