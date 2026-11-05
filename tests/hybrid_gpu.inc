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

// 0.2 V559-V573: Hybrid's HUD fix. A translucent HUD over a flat world, whose
// motion only the vectors tell: they claim the HUD moved with it. Judged after
// the pairs the still mask needs, against where the HUD belongs and where
// something other than the mask would put it.
enum class HudScene {
    // Bright strokes, two pixels every eight, as HUD text is.
    strokes,
    // The same with the world's light changing every frame, so no HUD pixel
    // is unchanged and the static overlay test cannot hold it.
    pulsing,
    // A box: its borders are steps, not strokes. _fast: the world four times
    // as fast, so the warped samples reach well past the border's mask.
    panel, panel_fast,
    // The strokes dark on the lighter world, and dim, their edges about
    // 14/255 (Star Citizen's dim HUD elements).
    dark, dim,
    // Fixed in the cockpit: the head turns a step a frame, which moves the
    // HUD across the image as it moves the struts, while the world turns past
    // on the stick. It belongs at its place in the synthetic's camera.
    cockpit,
    // Head-locked under the same head and stick turns: at its image place in
    // a synthetic camera; in B's, placed by the pose shift for the compositor
    // to turn back.
    helmet_turn
};
struct HudError { double to_true, to_other; };

HudError test_hybrid_hud(D3D12WarpFixture& fixture, HudScene kind,
    xrfg::D3D12OpticalFlowBackend backend = xrfg::D3D12OpticalFlowBackend::fidelity_fx,
    bool synthetic_camera = false, UINT outputs = 1) {
    constexpr UINT width = 256, height = 128;
    constexpr int hud_left = 72, hud_width = 80, hud_top = 32, hud_bottom = 96;
    constexpr int warm_up_pairs = 4;
    constexpr float yaw_step = 0.02F, stick_step = 0.035F;
    const bool cockpit = kind == HudScene::cockpit, helmet_turn = kind == HudScene::helmet_turn;
    const bool turning = cockpit || helmet_turn;
    const bool panel = kind == HudScene::panel || kind == HudScene::panel_fast;
    const int world_motion = kind == HudScene::panel_fast ? 24 : 6;
    const float frame_yaw = turning ? yaw_step : 0.0F;
    const RgbaBytes hud_color = panel || kind == HudScene::dark ? RgbaBytes{20, 40, 70, 255}
        : kind == HudScene::dim ? RgbaBytes{92, 112, 130, 255} : RgbaBytes{240, 210, 40, 255};
    const auto yaw_rotation = [](float angle) {
        xrfg::Quaternion rotation{};
        rotation.y = std::sin(angle * 0.5F);
        rotation.w = std::cos(angle * 0.5F);
        return rotation;
    };
    struct Tangents { float left, right, up, down; };
    const auto tangents = [](const ReprojectionViews& views, UINT eye) {
        const auto& fov = views[eye].fov;
        return Tangents{std::tan(fov.angle_left), std::tan(fov.angle_right),
                        std::tan(fov.angle_up), std::tan(fov.angle_down)};
    };
    // 65% of the HUD over the world where `inside` says, 4 x 4 samples a
    // pixel giving its edges their sub-pixel place.
    const auto paint = [&](float world_light, auto&& inside) {
        StereoPattern pattern;
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            auto& bytes = pattern[eye];
            bytes.resize(static_cast<std::size_t>(width) * height * kBytesPerPixel);
            for (UINT y = 0; y < height; ++y) {
                for (UINT x = 0; x < width; ++x) {
                    int covered = 0;
                    for (int sy = 0; sy < 4; ++sy) {
                        for (int sx = 0; sx < 4; ++sx) {
                            if (inside(eye, static_cast<float>(x) + (sx + 0.5F) / 4.0F,
                                    static_cast<float>(y) + (sy + 0.5F) / 4.0F)) ++covered;
                        }
                    }
                    const RgbaBytes world{70, 90, 110, 255};
                    const double alpha = 0.65 * covered / 16.0;
                    RgbaBytes color{0, 0, 0, 255};
                    for (UINT c = 0; c < 3; ++c) {
                        color[c] = static_cast<std::uint8_t>(std::lround(
                            alpha * hud_color[c] + (1.0 - alpha) * (world[c] + world_light)));
                    }
                    std::copy(color.begin(), color.end(), bytes.begin() +
                        (static_cast<std::size_t>(y) * width + x) * kBytesPerPixel);
                }
            }
        }
        return pattern;
    };
    const auto in_hud = [&](float px, float py) {
        const float ox = px - static_cast<float>(hud_left);
        return py >= hud_top && py < hud_bottom && ox >= 0.0F && ox < static_cast<float>(hud_width) &&
            (panel || std::fmod(ox, 8.0F) < 2.0F);
    };
    // The HUD at its image place plus a shift, per eye.
    const auto image_hud = [&](const std::array<float, kEyeCount>& shift, float world_light = 0.0F) {
        return paint(world_light, [&](UINT eye, float px, float py) { return in_hud(px - shift[eye], py); });
    };
    // The HUD fixed in the room, seen from a head at `yaw`; `hud_turn` turns
    // it with the world.
    const auto room_hud = [&](float yaw, float hud_turn) {
        const ReprojectionViews views_at = make_reprojection_views(yaw);
        return paint(0.0F, [&, views_at](UINT eye, float px, float py) {
            const Tangents t = tangents(views_at, eye);
            const xrfg::Vec3 ray = rotate_vector(yaw_rotation(-hud_turn),
                rotate_vector(views_at[eye].pose.orientation,
                    {t.left + (t.right - t.left) * px / width, t.up + (t.down - t.up) * py / height, -1.0F}));
            if (ray.z >= -1.0e-4F) return false;
            return in_hud((ray.x / -ray.z - t.left) / (t.right - t.left) * width,
                (ray.y / -ray.z - t.up) / (t.down - t.up) * height);
        });
    };
    const std::array<float, kEyeCount> no_shift{};
    const auto frame_light = [&](int frame) {
        return kind == HudScene::pulsing ? static_cast<float>(frame % 2) * 6.0F : 0.0F;
    };
    const auto frame_scene = [&](int frame) {
        return cockpit ? room_hud(yaw_step * static_cast<float>(frame), 0.0F)
                       : image_hud(no_shift, frame_light(frame));
    };
    // The pixels a world point moves from A's image to B's on the head's step,
    // at the HUD's middle.
    const auto pose_shift = [&]() {
        std::array<float, kEyeCount> shift{};
        const ReprojectionViews views_a = make_reprojection_views(0.0F);
        const ReprojectionViews views_b = make_reprojection_views(yaw_step);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            const Tangents t = tangents(views_a, eye);
            const float mx = static_cast<float>(hud_left) + hud_width * 0.5F;
            const xrfg::Vec3 world = rotate_vector(views_a[eye].pose.orientation,
                {t.left + (t.right - t.left) * mx / width, 0.0F, -1.0F});
            xrfg::Quaternion inverse_b = views_b[eye].pose.orientation;
            inverse_b.x = -inverse_b.x; inverse_b.y = -inverse_b.y; inverse_b.z = -inverse_b.z;
            const xrfg::Vec3 seen = rotate_vector(inverse_b, world);
            shift[eye] = (seen.x / -seen.z - t.left) / (t.right - t.left) * width - mx;
        }
        return shift;
    }();
    const auto scaled_shift = [&](float scale) {
        std::array<float, kEyeCount> shift{};
        for (UINT eye = 0; eye < kEyeCount; ++eye) shift[eye] = pose_shift[eye] * scale;
        return shift;
    };
    const int judged = warm_up_pairs + 1;
    // Where the HUD belongs in the output at a fraction, and where it lands
    // without the mask: dragged with the world, or for a turned head left
    // where the pose alone puts it.
    const auto expected_at = [&](float f) {
        if (cockpit) return synthetic_camera
            ? room_hud(yaw_step * (static_cast<float>(warm_up_pairs) + f), 0.0F) : frame_scene(judged);
        if (helmet_turn) return synthetic_camera ? image_hud(no_shift) : image_hud(scaled_shift(1.0F - f));
        return image_hud(no_shift, f * (frame_light(warm_up_pairs) + frame_light(judged)));
    };
    const auto other_at = [&](float f) {
        if (cockpit) return room_hud(yaw_step * static_cast<float>(judged), 0.5F * stick_step);
        if (helmet_turn) return synthetic_camera ? image_hud(scaled_shift(f - 1.0F)) : image_hud(no_shift);
        const float dragged = static_cast<float>(world_motion) * (1.0F - f);
        return image_hud({dragged, dragged}, f * (frame_light(warm_up_pairs) + frame_light(judged)));
    };
    auto game_motion = create_and_upload_game_motion_field(
        fixture, width, height, [&](UINT eye, UINT mx, UINT my) {
            if (!turning) return std::array<float, 2>{-static_cast<float>(world_motion), 0.0F};
            // Where the world at this B pixel was in A: turned back by the
            // stick's step, seen from A's head, a yaw step before B's.
            const ReprojectionViews views_a = make_reprojection_views(0.0F);
            const ReprojectionViews views_b = make_reprojection_views(yaw_step);
            const Tangents t = tangents(views_b, eye);
            const float bx = static_cast<float>(mx) + 0.5F, by = static_cast<float>(my) + 0.5F;
            const xrfg::Vec3 world = rotate_vector(yaw_rotation(-stick_step),
                rotate_vector(views_b[eye].pose.orientation,
                    {t.left + (t.right - t.left) * bx / width, t.up + (t.down - t.up) * by / height, -1.0F}));
            xrfg::Quaternion inverse_a = views_a[eye].pose.orientation;
            inverse_a.x = -inverse_a.x; inverse_a.y = -inverse_a.y; inverse_a.z = -inverse_a.z;
            const xrfg::Vec3 seen = rotate_vector(inverse_a, world);
            return std::array<float, 2>{(seen.x / -seen.z - t.left) / (t.right - t.left) * width - bx,
                                        (seen.y / -seen.z - t.up) / (t.down - t.up) * height - by};
        });
    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_source_texture(fixture, width, height), create_source_texture(fixture, width, height)};
    std::array<ComPtr<ID3D12Resource>, 2> current_destinations{
        create_source_texture(fixture, width, height), create_source_texture(fixture, width, height)};
    std::array<ComPtr<ID3D12Resource>, 3> synthetic_destinations{
        create_source_texture(fixture, width, height), create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height)};
    upload_pattern(fixture, sources[0].Get(), frame_scene(0));
    upload_pattern(fixture, sources[1].Get(), frame_scene(1));
    std::array<ID3D12Resource*, 2> source_pointers{sources[0].Get(), sources[1].Get()};
    std::array<ID3D12Resource*, 2> current_pointers{
        current_destinations[0].Get(), current_destinations[1].Get()};
    std::array<ID3D12Resource*, 3> synthetic_pointers{synthetic_destinations[0].Get(),
        synthetic_destinations[1].Get(), synthetic_destinations[2].Get()};
    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(operation_succeeded(history->initialize(fixture.device(), fixture.queue(),
                source_pointers, D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "HUD history initialization failed");
    xrfg::D3D12FrameSynthesizer synthesizer;
    xrfg::D3D12NvidiaOpticalFlowOptions options;
    options.hybrid = true;
    require(operation_succeeded(synthesizer.initialize(fixture.device(), fixture.queue(),
                history, current_pointers, synthetic_pointers, kFormat,
                D3D12_RESOURCE_STATE_RENDER_TARGET, backend, options)),
        "HUD synthesizer initialization failed");
    const auto guides = [&](std::uint64_t serial) {
        auto set = std::make_shared<xrfg::DlssMotionVectorSet>();
        set->eye_count = kEyeCount;
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            auto frame = std::make_shared<xrfg::DlssMotionVectorFrame>();
            frame->stream = 611 + eye;
            frame->epoch = 3;
            frame->serial = serial;
            frame->previous_serial = serial - 1;
            frame->motion_vectors = game_motion;
            frame->producer_queue = fixture.queue();
            frame->output_width = frame->motion_width = width;
            frame->output_height = frame->motion_height = height;
            frame->motion_slice = frame->output_slice = eye;
            frame->resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            set->eyes[eye] = frame;
        }
        return set;
    };
    const auto frame_views = [&](int frame) {
        return make_reprojection_views(frame_yaw * static_cast<float>(frame));
    };
    const ReprojectionViews first_views = frame_views(0);
    xrfg::D3D12HistoryCaptureTicket capture_a{};
    xrfg::D3D12FrameSynthesisTicket prime{};
    require(operation_succeeded(history->capture(0, &capture_a)) &&
            operation_succeeded(history->commit(capture_a)),
        "HUD capture A failed");
    require(operation_succeeded(synthesizer.submit_prime(capture_a, first_views, 0, &prime, guides(1))),
        "HUD prime failed");
    require_frame_start_gate(synthesizer, "HUD frame-start gate");
    const std::array<float, 3> fractions = outputs == 3
        ? std::array<float, 3>{0.25F, 0.5F, 0.75F} : std::array<float, 3>{0.5F, 0.5F, 0.5F};
    // Each pair a frame further on, captured into the source the pair before
    // it left free; the warm-up pairs age the mask, the last is judged.
    for (int frame = 1; frame <= judged; ++frame) {
        const UINT source = static_cast<UINT>(frame % 2);
        if (frame > 1) upload_pattern(fixture, sources[source].Get(), frame_scene(frame));
        xrfg::D3D12HistoryCaptureTicket capture{};
        require(operation_succeeded(history->capture(source, &capture)) &&
                operation_succeeded(history->commit(capture)),
            "HUD capture failed");
        const ReprojectionViews views = frame_views(frame);
        const ReprojectionViews target = synthetic_camera
            ? make_reprojection_views(frame_yaw * (static_cast<float>(frame - 1) + fractions[0])) : views;
        const auto extra = outputs >= 2
            ? std::make_optional(xrfg::D3D12ExtraSynthetic{1U, fractions[1], {},
                  outputs == 3 ? 2U : UINT32_MAX, fractions[2]})
            : std::nullopt;
        xrfg::D3D12FrameSynthesisTicket pair{};
        require(operation_succeeded(synthesizer.submit_pair(capture, views, target, 0, 1, &pair,
                    std::nullopt, guides(static_cast<std::uint64_t>(frame) + 1U), false, fractions[0],
                    {}, extra, synthetic_camera)),
            "HUD pair failed");
        fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
    }
    HudError error{};
    std::size_t count = 0;
    for (UINT output = 0; output < outputs; ++output) {
        const StereoPattern actual = readback_pattern(fixture, synthetic_destinations[output].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        const StereoPattern expected = expected_at(fractions[output]);
        const StereoPattern other = other_at(fractions[output]);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            for (int y = hud_top + 2; y < hud_bottom - 2; ++y) {
                for (int x = hud_left - 8; x < hud_left + hud_width + 8; ++x) {
                    const std::size_t offset =
                        (static_cast<std::size_t>(y) * width + static_cast<UINT>(x)) * kBytesPerPixel;
                    for (UINT c = 0; c < 3; ++c, ++count) {
                        error.to_true += std::abs(static_cast<int>(actual[eye][offset + c]) -
                            static_cast<int>(expected[eye][offset + c]));
                        error.to_other += std::abs(static_cast<int>(actual[eye][offset + c]) -
                            static_cast<int>(other[eye][offset + c]));
                    }
                }
            }
        }
    }
    error.to_true /= static_cast<double>(count);
    error.to_other /= static_cast<double>(count);
    std::cout << "hybrid HUD scene=" << static_cast<int>(kind)
              << (backend == xrfg::D3D12OpticalFlowBackend::nvidia ? " nvidia" : " fidelityfx")
              << (synthetic_camera ? " synthetic-camera" : "") << " outputs=" << outputs
              << " fix=" << (xrfg::hybrid_hud_fix_enabled() ? "on" : "off")
              << " vs_true=" << error.to_true << " vs_other=" << error.to_other << '\n';
    require(operation_succeeded(synthesizer.wait_for_idle()), "HUD final drain failed");
    require(operation_succeeded(history->invalidate()), "HUD history invalidate failed");
    return error;
}

void upload_single_slice(D3D12WarpFixture& fixture, ID3D12Resource* texture,
    const std::vector<std::uint8_t>& pixels, UINT width, UINT height) {
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT rows = 0;
    UINT64 row_size = 0, total_size = 0;
    fixture.device()->GetCopyableFootprints(&description, 0, 1, 0, &layout, &rows, &row_size, &total_size);
    ComPtr<ID3D12Resource> upload = create_buffer(fixture, D3D12_HEAP_TYPE_UPLOAD, total_size,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    require_hresult(upload->Map(0, &no_read, &mapped), "ID3D12Resource::Map(upload)");
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(static_cast<std::byte*>(mapped) + layout.Offset + y * layout.Footprint.RowPitch,
            pixels.data() + static_cast<std::size_t>(y) * width * kBytesPerPixel, width * kBytesPerPixel);
    }
    upload->Unmap(0, nullptr);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        const auto to_copy = transition_barrier(texture, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &to_copy);
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = upload.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = layout;
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = texture;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        const auto back = transition_barrier(texture, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &back);
    });
}

// The HUD fix on a double-wide image: the pulsing strokes in both eyes' halves
// of one slice, which share the still mask's slices. Error at each eye's HUD
// against where it belongs and against it dragged with the world.
HudError test_hybrid_hud_double_wide(D3D12WarpFixture& fixture,
    xrfg::D3D12OpticalFlowBackend backend = xrfg::D3D12OpticalFlowBackend::fidelity_fx) {
    constexpr UINT half_width = 256, width = half_width * 2U, height = 128;
    constexpr int hud_left = 72, hud_width = 80, hud_top = 32, hud_bottom = 96;
    constexpr int world_motion = 6, warm_up_pairs = 4, judged = warm_up_pairs + 1;
    const auto make_scene = [&](int hud_shift, double world_light) {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * kBytesPerPixel);
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                const int local = static_cast<int>(x % half_width) - hud_left - hud_shift;
                const bool hud = static_cast<int>(y) >= hud_top && static_cast<int>(y) < hud_bottom &&
                    local >= 0 && local < hud_width && local % 8 < 2;
                const RgbaBytes world{70, 90, 110, 255}, stroke{240, 210, 40, 255};
                const double alpha = hud ? 0.65 : 0.0;
                for (UINT c = 0; c < 4; ++c) {
                    pixels[(static_cast<std::size_t>(y) * width + x) * kBytesPerPixel + c] = c == 3 ? 255
                        : static_cast<std::uint8_t>(std::lround(
                              alpha * stroke[c] + (1.0 - alpha) * (world[c] + world_light)));
                }
            }
        }
        return pixels;
    };
    const auto frame_scene = [&](int frame) { return make_scene(0, frame % 2 ? 6.0 : 0.0); };
    auto game_motion = create_and_upload_game_motion_field(fixture, half_width, height,
        [&](UINT, UINT, UINT) { return std::array<float, 2>{-static_cast<float>(world_motion), 0.0F}; });
    std::array<ComPtr<ID3D12Resource>, 2> sources{create_single_slice_texture(fixture, width, height),
        create_single_slice_texture(fixture, width, height)};
    std::array<ComPtr<ID3D12Resource>, 2> current_destinations{create_single_slice_texture(fixture, width, height),
        create_single_slice_texture(fixture, width, height)};
    std::array<ComPtr<ID3D12Resource>, 1> synthetic_destinations{create_single_slice_texture(fixture, width, height)};
    upload_single_slice(fixture, sources[0].Get(), frame_scene(0), width, height);
    upload_single_slice(fixture, sources[1].Get(), frame_scene(1), width, height);
    std::array<ID3D12Resource*, 2> source_pointers{sources[0].Get(), sources[1].Get()};
    std::array<ID3D12Resource*, 2> current_pointers{current_destinations[0].Get(), current_destinations[1].Get()};
    std::array<ID3D12Resource*, 1> synthetic_pointers{synthetic_destinations[0].Get()};
    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(operation_succeeded(history->initialize(fixture.device(), fixture.queue(),
                source_pointers, D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "double-wide HUD history initialization failed");
    xrfg::D3D12FrameSynthesizer synthesizer;
    xrfg::D3D12NvidiaOpticalFlowOptions options;
    options.hybrid = true;
    require(operation_succeeded(synthesizer.initialize(fixture.device(), fixture.queue(),
                history, current_pointers, synthetic_pointers, kFormat,
                D3D12_RESOURCE_STATE_RENDER_TARGET, backend, options)),
        "double-wide HUD synthesizer initialization failed");
    ReprojectionViews views = make_reprojection_views();
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        views[eye].image_rect = {eye * half_width, 0, half_width, height};
        views[eye].array_slice = 0;
    }
    const auto guides = [&](std::uint64_t serial) {
        auto set = std::make_shared<xrfg::DlssMotionVectorSet>();
        set->eye_count = kEyeCount;
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            auto frame = std::make_shared<xrfg::DlssMotionVectorFrame>();
            frame->stream = 711 + eye;
            frame->epoch = 3;
            frame->serial = serial;
            frame->previous_serial = serial - 1;
            frame->motion_vectors = game_motion;
            frame->producer_queue = fixture.queue();
            frame->output_x = eye * half_width;
            frame->output_width = frame->motion_width = half_width;
            frame->output_height = frame->motion_height = height;
            frame->motion_slice = frame->output_slice = eye;
            frame->resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            set->eyes[eye] = frame;
        }
        return set;
    };
    xrfg::D3D12HistoryCaptureTicket capture_a{};
    xrfg::D3D12FrameSynthesisTicket prime{};
    require(operation_succeeded(history->capture(0, &capture_a)) &&
            operation_succeeded(history->commit(capture_a)),
        "double-wide HUD capture A failed");
    require(operation_succeeded(synthesizer.submit_prime(capture_a, views, 0, &prime, guides(1))),
        "double-wide HUD prime failed");
    require_frame_start_gate(synthesizer, "double-wide HUD frame-start gate");
    const auto statistics_before = xrfg::dlss_motion_vector_statistics();
    for (int frame = 1; frame <= judged; ++frame) {
        const UINT source = static_cast<UINT>(frame % 2);
        if (frame > 1) upload_single_slice(fixture, sources[source].Get(), frame_scene(frame), width, height);
        xrfg::D3D12HistoryCaptureTicket capture{};
        require(operation_succeeded(history->capture(source, &capture)) &&
                operation_succeeded(history->commit(capture)),
            "double-wide HUD capture failed");
        xrfg::D3D12FrameSynthesisTicket pair{};
        require(operation_succeeded(synthesizer.submit_pair(capture, views, views, 0, 1, &pair,
                    std::nullopt, guides(static_cast<std::uint64_t>(frame) + 1U))),
            "double-wide HUD pair failed");
        fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
    }
    require(xrfg::dlss_motion_vector_statistics().used ==
                statistics_before.used + static_cast<std::uint64_t>(judged),
        "the double-wide pairs were not composed by the hybrid");
    const std::vector<std::uint8_t> actual = readback_single_slice(
        fixture, synthetic_destinations[0].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto expected = make_scene(0, 3.0);
    const auto dragged = make_scene(world_motion / 2, 3.0);
    HudError error{};
    std::array<double, kEyeCount> eye_error{};
    std::size_t count = 0;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        for (int y = hud_top + 2; y < hud_bottom - 2; ++y) {
            for (int x = hud_left - 8; x < hud_left + hud_width + 8; ++x) {
                const std::size_t offset = (static_cast<std::size_t>(y) * width + eye * half_width +
                    static_cast<UINT>(x)) * kBytesPerPixel;
                for (UINT c = 0; c < 3; ++c, ++count) {
                    const double to_true = std::abs(static_cast<int>(actual[offset + c]) -
                        static_cast<int>(expected[offset + c]));
                    error.to_true += to_true;
                    eye_error[eye] += to_true;
                    error.to_other += std::abs(static_cast<int>(actual[offset + c]) -
                        static_cast<int>(dragged[offset + c]));
                }
            }
        }
    }
    error.to_true /= static_cast<double>(count);
    error.to_other /= static_cast<double>(count);
    std::cout << "hybrid HUD double-wide " << (backend == xrfg::D3D12OpticalFlowBackend::nvidia ? "nvidia" : "fidelityfx")
              << " fix=" << (xrfg::hybrid_hud_fix_enabled() ? "on" : "off")
              << " vs_true=" << error.to_true << " vs_other=" << error.to_other
              << " eye_error=" << eye_error[0] / (count / 2.0) << "/" << eye_error[1] / (count / 2.0) << '\n';
    require(operation_succeeded(synthesizer.wait_for_idle()), "double-wide HUD final drain failed");
    require(operation_succeeded(history->invalidate()), "double-wide HUD history invalidate failed");
    return error;
}

// 0.2 V546: the eye-order measurement on the GPU, for a swapchain of one eye
// whose two DLSS streams nothing tells apart. A textured object moves 16 pixels
// over a background moving 4; this eye's stream has the object where the image
// shows it, the other eye's 40 pixels to the side, as a disparity puts it.
// Whichever of the two the resolver assigned, the eye's own images have to vote
// this eye's stream in, and the pair's order follow. native: on the DLSS-G
// route as well, which measures through the same pass; a real adapter.
void test_eye_order_measurement_on_gpu(D3D12WarpFixture& fixture, bool native = false) {
    constexpr UINT width = 512, height = 256;
    constexpr int object_left = 128, object_width = 128, object_top = 64, object_bottom = 192;
    constexpr int object_motion = 16, background_motion = 4, disparity = 40;
    const auto byte = [](int value) { return static_cast<std::uint8_t>(value); };
    const auto image = [&](int background_shift, int object_shift) {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) * height * kBytesPerPixel, 0);
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                const int left = object_left + object_shift, v = static_cast<int>(y);
                const bool object = v >= object_top && v < object_bottom &&
                    static_cast<int>(x) >= left && static_cast<int>(x) < left + object_width;
                const int u = object ? static_cast<int>(x) - left
                                     : static_cast<int>(x) - background_shift + 64;
                const RgbaBytes color = object
                    ? RgbaBytes{byte(200 - (u * 29 + v * 17) % 120), byte(40 + (u * 11 + v * 23) % 160),
                          byte(60 + ((u / 3) * 19 + v * 5) % 150), 255}
                    : RgbaBytes{byte(60 + (u * 37 + v * 11) % 150), byte(70 + ((u / 2) * 13 + v * 7) % 140),
                          byte(80 + (u * 5 + v * 3) % 120), 255};
                std::copy(color.begin(), color.end(),
                    bytes.begin() + (static_cast<std::size_t>(y) * width + x) * kBytesPerPixel);
            }
        }
        return bytes;
    };
    const auto image_a = image(0, 0);
    const auto image_b = image(background_motion, object_motion);
    // Each pixel's vector back to the other image: from B to A (forward) or
    // from A to B, the object where this eye (offset 0) or the other eye
    // (offset disparity) sees it.
    const auto field = [&](bool forward, int offset) {
        return create_and_upload_game_motion_field(
            fixture, width, height, [&, forward, offset](UINT, UINT x, UINT y) {
                const int left = object_left + offset + (forward ? object_motion : 0);
                const int v = static_cast<int>(y);
                const bool object = v >= object_top && v < object_bottom &&
                    static_cast<int>(x) >= left && static_cast<int>(x) < left + object_width;
                const float motion = static_cast<float>(object ? object_motion : background_motion);
                return std::array<float, 2>{forward ? -motion : motion, 0.0F};
            });
    };
    const auto own_forward = field(true, 0), own_backward = field(false, 0);
    const auto other_forward = field(true, disparity), other_backward = field(false, disparity);
    const auto run = [&](std::uint64_t own_stream, std::uint64_t other_stream, bool assign_own,
                         xrfg::D3D12FrameGenerationMode mode = xrfg::D3D12FrameGenerationMode::ofxr) {
        std::array<ComPtr<ID3D12Resource>, 2> sources{create_single_slice_texture(fixture, width, height),
            create_single_slice_texture(fixture, width, height)};
        std::array<ComPtr<ID3D12Resource>, 2> current_destinations{create_single_slice_texture(fixture, width, height),
            create_single_slice_texture(fixture, width, height)};
        std::array<ComPtr<ID3D12Resource>, 1> synthetic_destinations{create_single_slice_texture(fixture, width, height)};
        upload_single_slice(fixture, sources[0].Get(), image_a, width, height);
        upload_single_slice(fixture, sources[1].Get(), image_b, width, height);
        std::array<ID3D12Resource*, 2> source_pointers{sources[0].Get(), sources[1].Get()};
        std::array<ID3D12Resource*, 2> current_pointers{current_destinations[0].Get(), current_destinations[1].Get()};
        std::array<ID3D12Resource*, 1> synthetic_pointers{synthetic_destinations[0].Get()};
        auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
        require(operation_succeeded(history->initialize(fixture.device(), fixture.queue(),
                    source_pointers, D3D12_RESOURCE_STATE_RENDER_TARGET)),
            "eye-order history initialization failed");
        xrfg::D3D12NvidiaOpticalFlowOptions options{};
        options.hybrid = mode == xrfg::D3D12FrameGenerationMode::ofxr;
        xrfg::D3D12FrameSynthesizer synthesizer;
        require(operation_succeeded(synthesizer.initialize(fixture.device(), fixture.queue(), history,
                    current_pointers, synthetic_pointers, kFormat, D3D12_RESOURCE_STATE_RENDER_TARGET,
                    xrfg::D3D12OpticalFlowBackend::fidelity_fx, options, false, mode)),
            "eye-order synthesizer initialization failed");
        std::array<xrfg::D3D12ReprojectionView, 1> view{};
        view[0].fov = {-0.785398F, 0.785398F, 0.785398F, -0.785398F};
        view[0].image_rect = {0, 0, width, height};
        view[0].array_slice = 0;
        const auto frame_for = [&](std::uint64_t stream, std::uint64_t serial, const ComPtr<ID3D12Resource>& vectors) {
            auto frame = std::make_shared<xrfg::DlssMotionVectorFrame>();
            frame->stream = stream;
            frame->epoch = 5;
            frame->serial = serial;
            frame->previous_serial = serial - 1;
            frame->motion_vectors = vectors;
            frame->producer_queue = fixture.queue();
            frame->output_width = frame->motion_width = width;
            frame->output_height = frame->motion_height = height;
            frame->resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            return frame;
        };
        const auto set_for = [&](std::uint64_t serial, bool forward) {
            auto own = frame_for(own_stream, serial, forward ? own_forward : own_backward);
            auto other = frame_for(other_stream, serial, forward ? other_forward : other_backward);
            auto set = std::make_shared<xrfg::DlssMotionVectorSet>();
            set->eye_count = 1;
            set->eyes[0] = assign_own ? own : other;
            set->alternate = assign_own ? other : own;
            set->resolved_eye = 0;
            return set;
        };
        xrfg::D3D12HistoryCaptureTicket capture{};
        require(operation_succeeded(history->capture(0, &capture)) &&
                operation_succeeded(history->commit(capture)),
            "eye-order capture A failed");
        xrfg::D3D12FrameSynthesisTicket ticket{};
        require(operation_succeeded(synthesizer.submit_prime(capture, view, 0, &ticket, set_for(1, false))),
            "eye-order prime failed");
        require_frame_start_gate(synthesizer, "eye-order frame-start gate");
        for (std::uint64_t pair = 1; pair <= 32; ++pair) {
            const std::uint32_t index = static_cast<std::uint32_t>(pair % 2U);
            require(operation_succeeded(history->capture(index, &capture)) &&
                    operation_succeeded(history->commit(capture)),
                "eye-order capture failed");
            require(operation_succeeded(synthesizer.submit_pair(capture, view, view, 0, index,
                        &ticket, std::nullopt, set_for(pair + 1, index == 1))),
                "eye-order pair failed");
            fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
        }
        require(operation_succeeded(synthesizer.wait_for_idle()), "eye-order drain failed");
        require(operation_succeeded(history->invalidate()), "eye-order history invalidate failed");
        return xrfg::dlss_eye_order_left(own_stream, other_stream);
    };
    const std::uint64_t kept = run(901, 902, true);
    const std::uint64_t swapped = run(911, 912, false);
    std::cout << "eye order: assigned own -> left " << kept << ", assigned other -> left " << swapped << '\n';
    require(kept == 901, "the eye images did not vote in the eye's own stream when it was assigned");
    require(swapped == 911, "the eye images did not vote in the eye's own stream when the other was assigned");
    if (native) {
        const std::uint64_t native_kept = run(921, 922, true, xrfg::D3D12FrameGenerationMode::native_dlss);
        const std::uint64_t native_swapped = run(931, 932, false, xrfg::D3D12FrameGenerationMode::native_dlss);
        std::cout << "eye order DLSS-G: assigned own -> left " << native_kept
                  << ", assigned other -> left " << native_swapped << '\n';
        require(native_kept == 921 && native_swapped == 931,
            "the DLSS-G route did not measure the eye's own stream");
    }
}
