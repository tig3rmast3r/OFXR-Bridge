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
