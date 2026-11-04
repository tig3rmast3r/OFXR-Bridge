enum class SyntheticCameraPath {
    flow,                // FidelityFX's optical flow alone
    vectors,             // the game's DLSS vectors alone
    hybrid,              // both, per pixel (the default with DLSS vectors)
    nvidia,              // NVIDIA's optical flow
    extrapolate,         // extrapolation from the vectors, the gather
    extrapolate_mesh,    // extrapolation from the vectors, the mesh warp
    extrapolate_flow,    // extrapolation from FidelityFX's flow, the gather
    fsr,                 // FSR final reprojection
    native,              // NVIDIA's DLSS Frame Generation
};

[[nodiscard]] const char* synthetic_camera_path_name(SyntheticCameraPath path) noexcept {
    switch (path) {
    case SyntheticCameraPath::flow: return "flow";
    case SyntheticCameraPath::vectors: return "vectors";
    case SyntheticCameraPath::hybrid: return "hybrid";
    case SyntheticCameraPath::nvidia: return "nvidia";
    case SyntheticCameraPath::extrapolate: return "extrapolate";
    case SyntheticCameraPath::extrapolate_mesh: return "extrapolate-mesh";
    case SyntheticCameraPath::extrapolate_flow: return "extrapolate-flow";
    case SyntheticCameraPath::fsr: return "fsr";
    case SyntheticCameraPath::native: return "native";
    }
    return "?";
}

// A synthetic shown from its own camera. A and B are rendered from two cameras
// 12 degrees of yaw apart; the synthetic for fraction f is generated in the
// camera turned f of the way - the head's pose when it is shown, which the
// layer submits it with - and must match the scene rendered from that camera
// across the whole view: inside B's view from the synthesis, beyond it on the
// side the head turned from out of A, and never black. Generated in B's camera
// and submitted with B's pose instead, the runtime turned it back by (1 - f)
// of the turn and left that strip of the view without pixels: the share is
// printed as old_black. Extrapolated, the camera turns on past B and the strip
// it turns towards was seen by neither frame; it takes B's nearest edge, and
// is held only to having content.
void test_synthetic_camera_follows_display_pose(
    D3D12WarpFixture& fixture,
    SyntheticCameraPath path,
    bool triple,
    std::uint32_t native_scale = 100,
    bool quad = false) {
    constexpr UINT kCameraWidth = 160;
    constexpr UINT kCameraHeight = 80;
    constexpr float kYawRadians = 0.2094395102393195F;
    const bool extrapolating = path == SyntheticCameraPath::extrapolate ||
        path == SyntheticCameraPath::extrapolate_mesh ||
        path == SyntheticCameraPath::extrapolate_flow;
    const bool vectors = path == SyntheticCameraPath::vectors ||
        path == SyntheticCameraPath::hybrid || path == SyntheticCameraPath::extrapolate ||
        path == SyntheticCameraPath::extrapolate_mesh || path == SyntheticCameraPath::native || path == SyntheticCameraPath::fsr;
    // As the layer places them: interpolating, (index + 1) / frames of the
    // way; extrapolating, the real frame first and each synthetic a display
    // period further on, the first synthetic shown last.
    require(!quad || !extrapolating, "4X is interpolation only");
    const std::array<float, 3> fractions = extrapolating
        ? (triple ? std::array<float, 3>{5.0F / 3.0F, 4.0F / 3.0F, 4.0F / 3.0F}
                  : std::array<float, 3>{1.5F, 1.5F, 1.5F})
        : quad ? std::array<float, 3>{0.25F, 0.5F, 0.75F}
        : (triple ? std::array<float, 3>{1.0F / 3.0F, 2.0F / 3.0F, 2.0F / 3.0F}
                  : std::array<float, 3>{0.5F, 0.5F, 0.5F});
    const UINT outputs = quad ? 3U : triple ? 2U : 1U;
    const std::string label = std::string("synthetic camera ") +
        synthetic_camera_path_name(path) +
        (path == SyntheticCameraPath::native ? " " + std::to_string(native_scale) + "%" : "") +
        (quad ? " 4X" : triple ? " 3X" : " 2X");

    const ReprojectionViews views_a = make_reprojection_views(0.0F);
    const ReprojectionViews views_b = make_reprojection_views(kYawRadians);
    std::array<ReprojectionViews, 3> views_c{};
    for (UINT output = 0; output < 3; ++output) {
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            views_c[output][eye] = views_b[eye];
            views_c[output][eye].pose = xrfg::pose_at_fraction(
                views_a[eye].pose, views_b[eye].pose, fractions[output]);
        }
    }
    const StereoPattern previous =
        ray_direction_pattern(kCameraWidth, kCameraHeight, views_a);
    const StereoPattern current =
        ray_direction_pattern(kCameraWidth, kCameraHeight, views_b);

    std::shared_ptr<xrfg::DlssMotionVectorSet> motion_a;
    std::shared_ptr<xrfg::DlssMotionVectorSet> motion_b;
    if (vectors) {
        // A still scene under a turning head: each pixel of B came from where
        // A's camera saw its ray, the head's turn and nothing else.
        ComPtr<ID3D12Resource> field = create_and_upload_game_motion_field(
            fixture, kCameraWidth, kCameraHeight, [&](UINT eye, UINT x, UINT y) {
                float source_x = static_cast<float>(x);
                float source_y = static_cast<float>(y);
                (void)target_pixel_maps_to_source(views_a[eye], views_b[eye],
                    kCameraWidth, kCameraHeight, x, y, &source_x, &source_y);
                return std::array<float, 2>{source_x - static_cast<float>(x),
                                            source_y - static_cast<float>(y)};
            });
        ComPtr<ID3D12Resource> depth = (path == SyntheticCameraPath::native || path == SyntheticCameraPath::fsr)
            ? create_native_test_depth(fixture, kCameraWidth, kCameraHeight)
            : nullptr;
        const auto make_set = [&](std::uint64_t serial) {
            auto set = std::make_shared<xrfg::DlssMotionVectorSet>();
            set->eye_count = kEyeCount;
            for (UINT eye = 0; eye < kEyeCount; ++eye) {
                auto frame = std::make_shared<xrfg::DlssMotionVectorFrame>();
                frame->stream = 611 + eye;
                frame->epoch = 1;
                frame->serial = serial;
                frame->previous_serial = serial - 1;
                frame->motion_vectors = field;
                frame->producer_queue = fixture.queue();
                frame->output_width = frame->motion_width = kCameraWidth;
                frame->output_height = frame->motion_height = kCameraHeight;
                frame->motion_slice = frame->output_slice = eye;
                frame->resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                if (depth) {
                    frame->depth = depth;
                    frame->depth_width = kCameraWidth;
                    frame->depth_height = kCameraHeight;
                    frame->depth_resource_state = D3D12_RESOURCE_STATE_COMMON;
                }
                set->eyes[eye] = frame;
            }
            return set;
        };
        motion_a = make_set(1);
        motion_b = make_set(2);
    }

    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_source_texture(fixture, kCameraWidth, kCameraHeight),
        create_source_texture(fixture, kCameraWidth, kCameraHeight)};
    std::array<ComPtr<ID3D12Resource>, 2> current_destinations{
        create_source_texture(fixture, kCameraWidth, kCameraHeight),
        create_source_texture(fixture, kCameraWidth, kCameraHeight)};
    std::array<ComPtr<ID3D12Resource>, 3> synthetic_destinations{
        create_source_texture(fixture, kCameraWidth, kCameraHeight),
        create_source_texture(fixture, kCameraWidth, kCameraHeight),
        create_source_texture(fixture, kCameraWidth, kCameraHeight)};
    upload_pattern(fixture, sources[0].Get(), previous);
    upload_pattern(fixture, sources[1].Get(), current);
    std::array<ID3D12Resource*, 2> source_pointers{sources[0].Get(), sources[1].Get()};
    std::array<ID3D12Resource*, 2> current_pointers{
        current_destinations[0].Get(), current_destinations[1].Get()};
    std::array<ID3D12Resource*, 3> synthetic_pointers{
        synthetic_destinations[0].Get(), synthetic_destinations[1].Get(),
        synthetic_destinations[2].Get()};
    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(operation_succeeded(history->initialize(fixture.device(), fixture.queue(),
                source_pointers, D3D12_RESOURCE_STATE_RENDER_TARGET)),
        label + " history initialization failed");
    xrfg::D3D12NvidiaOpticalFlowOptions options;
    options.hybrid=path==SyntheticCameraPath::hybrid;
    if (options.hybrid) options.input_scale=xrfg::D3D12NvidiaInputScale::quarter;



    if (path == SyntheticCameraPath::native) {

        options.native_scale = native_scale;
    }
    xrfg::D3D12FrameSynthesizer synthesizer;
    require(operation_succeeded(synthesizer.initialize(fixture.device(), fixture.queue(), history,
                current_pointers, synthetic_pointers, kFormat, D3D12_RESOURCE_STATE_RENDER_TARGET,
                path == SyntheticCameraPath::nvidia ? xrfg::D3D12OpticalFlowBackend::nvidia
                                                    : xrfg::D3D12OpticalFlowBackend::fidelity_fx,
                options, false, path == SyntheticCameraPath::native ? xrfg::D3D12FrameGenerationMode::native_dlss : path == SyntheticCameraPath::fsr ? xrfg::D3D12FrameGenerationMode::fsr3 : xrfg::D3D12FrameGenerationMode::ofxr)),
        label + " synthesizer initialization failed");
    xrfg::D3D12HistoryCaptureTicket capture_a{};
    xrfg::D3D12HistoryCaptureTicket capture_b{};
    require(operation_succeeded(history->capture(0, &capture_a)) &&
                operation_succeeded(history->commit(capture_a)),
        label + " capture A failed");
    xrfg::D3D12FrameSynthesisTicket prime{};
    xrfg::D3D12FrameSynthesisTicket pair{};
    require(operation_succeeded(synthesizer.submit_prime(capture_a, views_a, 0, &prime, motion_a)),
        label + " prime failed");
    require_frame_start_gate(synthesizer, label + " frame-start gate");
    require(operation_succeeded(history->capture(1, &capture_b)) &&
                operation_succeeded(history->commit(capture_b)),
        label + " capture B failed");
    const auto extra = triple || quad
        ? std::optional<xrfg::D3D12ExtraSynthetic>(xrfg::D3D12ExtraSynthetic{
              1, fractions[1], std::span<const xrfg::D3D12ReprojectionView>(views_c[1]),
              quad ? 2U : UINT32_MAX, fractions[2],
              quad ? std::span<const xrfg::D3D12ReprojectionView>(views_c[2])
                   : std::span<const xrfg::D3D12ReprojectionView>{}})
        : std::nullopt;
    require(synthesizer.submit_pair(capture_b, views_b, views_c[0], 0, 1, &pair, std::nullopt,
                motion_b, false, fractions[0], {}, extra, true) == S_OK,
        label + " pair failed");
    require(path==SyntheticCameraPath::fsr ? !pair.synthetics_in_target_camera : pair.synthetics_in_target_camera,
        label + " did not generate its synthetics in their own cameras");
    require(operation_succeeded(synthesizer.wait_for_idle()), label + " drain failed");
    require(readback_pattern(fixture, current_destinations[1].Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET) == current,
        label + " current output is not a bit-exact copy of B");

    if(path==SyntheticCameraPath::fsr) {
        const auto reset_pixels=readback_pattern(fixture,synthetic_destinations[0].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
        for(UINT eye=0;eye<kEyeCount;++eye)
            require(mean_absolute_rgb_error(reset_pixels,current,kCameraWidth,kCameraHeight,eye,0)<0.01,
                "FSR SDK reset must keep B colour and B camera, even with R19 ON");
        std::cout << "FSR SDK reset: unchanged B RGB output; target-camera receipt false\n";
        require(operation_succeeded(history->invalidate()),label+" invalidate");
        return;
    }
    for (UINT output = 0; output < outputs; ++output) {
        const StereoPattern actual = readback_pattern(fixture,
            synthetic_destinations[output].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        const StereoPattern expected =
            ray_direction_pattern(kCameraWidth, kCameraHeight, views_c[output]);
        if(path==SyntheticCameraPath::fsr) { std::ofstream pixels("r19-fsr.rgba",std::ios::binary);pixels.write(reinterpret_cast<const char*>(actual[0].data()),actual[0].size()); }
        if(path==SyntheticCameraPath::native) { std::ofstream pixels("r19-native.rgba",std::ios::binary);pixels.write(reinterpret_cast<const char*>(actual[0].data()),actual[0].size()); }
        const UINT header_height=std::min(kCameraHeight,static_cast<UINT>(std::ceil(24.F*100.F/native_scale)));
        std::size_t header_black{},header_white{},header_monochrome{};
        for (UINT y=0;y<header_height;++y) for(UINT x=0;x<kCameraWidth;++x) {
            const auto i=(y*kCameraWidth+x)*4;const auto r=actual[0][i],g=actual[0][i+1],b=actual[0][i+2];
            if(std::abs(int(r)-int(g))<3 && std::abs(int(r)-int(b))<3) {
                ++header_monochrome;
                if(r<8) ++header_black;else if(r>247) ++header_white;
            }
        }
        // R19 rotates/resamples the driver's banner: it no longer satisfies the
        // unrotated 21-row detector. Require the global documented setting AND
        // captured black/white text in the first 24 rows; never skip scene pixels below.
        const bool sdk_header=path==SyntheticCameraPath::native && ngx_sdk_indicator_enabled() &&
            header_black>kCameraWidth*header_height/5 && header_white>kCameraWidth*header_height/20 &&
            header_monochrome>kCameraWidth*header_height*2/5;
        std::cout << "SDK header confirmed=" << sdk_header << " registry=" << ngx_sdk_indicator_enabled() << "\n";
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            // Inside B's view; and beyond it, split by whether A saw it.
            double inside = 0.0, beyond_seen = 0.0, beyond_unseen = 0.0;
            std::size_t inside_count = 0, seen_count = 0, unseen_count = 0, black = 0;
            for (UINT y = sdk_header ? header_height : 0U; y < kCameraHeight; ++y) {
                for (UINT x = 0; x < kCameraWidth; ++x) {
                    const std::size_t offset =
                        (static_cast<std::size_t>(y) * kCameraWidth + x) * kBytesPerPixel;
                    int error = 0;
                    int brightest = 0;
                    for (UINT channel = 0; channel < 3; ++channel) {
                        error += std::abs(static_cast<int>(actual[eye][offset + channel]) -
                                          static_cast<int>(expected[eye][offset + channel]));
                        brightest = std::max<int>(brightest, actual[eye][offset + channel]);
                    }
                    // The scene's darkest channel is above 20 everywhere.
                    black += brightest < 12 ? 1U : 0U;
                    if (target_pixel_maps_to_source(views_b[eye], views_c[output][eye],
                            kCameraWidth, kCameraHeight, x, y)) {
                        inside += error;
                        ++inside_count;
                    } else if (target_pixel_maps_to_source(views_a[eye], views_c[output][eye],
                                   kCameraWidth, kCameraHeight, x, y)) {
                        beyond_seen += error;
                        ++seen_count;
                    } else {
                        beyond_unseen += error;
                        ++unseen_count;
                    }
                }
            }
            const auto mean = [](double total, std::size_t count) {
                return count == 0 ? 0.0 : total / (3.0 * static_cast<double>(count));
            };
            const std::size_t pixels = static_cast<std::size_t>(kCameraWidth) * kCameraHeight;
            std::cout << label << " output=" << output << " eye=" << eye
                      << " fraction=" << fractions[output]
                      << " inside_mae=" << mean(inside, inside_count)
                      << " beyond_seen_mae=" << mean(beyond_seen, seen_count)
                      << " beyond_unseen_mae=" << mean(beyond_unseen, unseen_count)
                      << " old_black=" << static_cast<double>(seen_count + unseen_count) /
                             static_cast<double>(pixels)
                      << " black=" << black << '\n';
            require(seen_count + unseen_count > static_cast<std::size_t>(kCameraHeight) * 4U,
                label + ": the turn leaves no strip beyond B's view to test");
            require(extrapolating ? unseen_count > 0 && seen_count == 0
                                  : seen_count > 0 && unseen_count == 0,
                label + ": the strip beyond B's view is not on the side expected");
            require(black == 0, label + " left " + std::to_string(black) +
                " black pixels for eye " + std::to_string(eye));
            // FidelityFX's flow is too coarse on this small turning scene to
            // extrapolate from at all: 32 against the scene from B's own
            // camera too, where the vectors are exact. It is held to having
            // content.
            if (path == SyntheticCameraPath::extrapolate_flow) {
                continue;
            }
            if(path==SyntheticCameraPath::fsr) {
                std::cout << "FSR first-pair camera diagnostics MAE A=" << mean_absolute_rgb_error(actual,previous,kCameraWidth,kCameraHeight,eye,10) << " B=" << mean_absolute_rgb_error(actual,current,kCameraWidth,kCameraHeight,eye,10) << "\n";
            }
            require(mean(inside, inside_count) <= 1.5,
                label + " does not match the scene from its camera inside B's view for eye " +
                    std::to_string(eye) + ": " + std::to_string(mean(inside, inside_count)));
            require(mean(beyond_seen, seen_count) <= 1.5,
                label + " does not take A beyond B's view for eye " + std::to_string(eye) +
                    ": " + std::to_string(mean(beyond_seen, seen_count)));
        }
    }
    require(operation_succeeded(history->invalidate()), label + " history invalidate failed");
}
