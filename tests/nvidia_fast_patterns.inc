// Deterministic high-contrast controls for the Fast/Medium headset comparison.
// Included inside the history test namespace to reuse its GPU ownership helpers.
StereoPattern fast_control_pattern(UINT width, UINT height, int shift, int kind) {
    StereoPattern pattern;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        auto& bytes = pattern[eye];
        bytes.resize(static_cast<std::size_t>(width) * height * 4);
        for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
            const int shifted = static_cast<int>(x) - (eye ? -shift : shift);
            const auto sx = static_cast<UINT>(std::max(0, shifted));
            unsigned value = 8;
            if (kind == 0) {
                // Identical high-contrast structure on each advancing capture.
                value = ((x / 13 + y / 17) % 2) ? 210 : 12;
                if (x > width / 3 && x < width / 3 + 50) value = 60 + eye * 100;
            } else if (kind == 1) {
                value = 20 + ((sx / 7 + y / 9) % 2) * 45;
                // Static HUD over moving background, with repeated white strokes.
                if (x > width / 6 && x < width * 5 / 6 && y > height / 3 && y < height * 2 / 3)
                    value = (x % 25 < 3 || y % 31 < 3) ? 245 : 4;
            } else if (kind == 2) {
                // Sparse moving objects surrounded by a large unchanged dark area.
                if ((sx > width / 3 && sx < width / 3 + 56 && y > height / 3 && y < height / 3 + 160) ||
                    (sx > width * 2 / 3 && sx < width * 2 / 3 + 100 && y > height / 2 && y < height / 2 + 64)) value = 235;
            } else {
                // Repeated narrow high-contrast structures, translated as a group.
                value = (sx % 97 < 5 || (sx % 191 < 8 && y % 103 < 64)) ? 230 : 5;
            }
            const auto i = (static_cast<std::size_t>(y) * width + x) * 4;
            bytes[i] = bytes[i + 1] = bytes[i + 2] = static_cast<std::uint8_t>(value);
            bytes[i + 3] = 255;
        }
    }
    return pattern;
}

void run_nvidia_fast_controls() {
    D3D12WarpFixture fixture(true);
    constexpr UINT width = 1342, height = 1464;
    for (int kind = 0; kind < 4; ++kind) {
        const auto a = fast_control_pattern(width, height, 0, kind);
        const auto b = fast_control_pattern(width, height, 24, kind);
        const auto expected = fast_control_pattern(width, height, 12, kind);
        for (auto preset : {xrfg::D3D12NvidiaPerformancePreset::fast,
                            xrfg::D3D12NvidiaPerformancePreset::medium}) {
            std::array<ComPtr<ID3D12Resource>, 2> sources{
                create_source_texture(fixture, width, height), create_source_texture(fixture, width, height)};
            auto current = create_source_texture(fixture, width, height);
            auto synthetic = create_source_texture(fixture, width, height);
            std::array<ID3D12Resource*, 2> input{sources[0].Get(), sources[1].Get()};
            std::array<ID3D12Resource*, 1> current_out{current.Get()}, synthetic_out{synthetic.Get()};
            upload_pattern(fixture, sources[0].Get(), a);
            upload_pattern(fixture, sources[1].Get(), b);
            auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
            require_hresult(history->initialize(fixture.device(), fixture.queue(), input,
                D3D12_RESOURCE_STATE_RENDER_TARGET), "fast control history");
            xrfg::D3D12FrameSynthesizer synth;
            xrfg::D3D12NvidiaOpticalFlowOptions options;
            options.preset = preset;
            options.input_scale = xrfg::D3D12NvidiaInputScale::half;
            require_hresult(synth.initialize(fixture.device(), fixture.queue(), history,
                current_out, synthetic_out, kFormat, D3D12_RESOURCE_STATE_RENDER_TARGET,
                xrfg::D3D12OpticalFlowBackend::nvidia, options), "fast control initialize");
            const auto views = make_reprojection_views();
            xrfg::D3D12HistoryCaptureTicket ca{}, cb{};
            xrfg::D3D12FrameSynthesisTicket output{};
            require_hresult(history->capture(0, &ca), "fast control A");
            require_hresult(history->commit(ca), "fast control commit A");
            require_hresult(synth.submit_prime(ca, views, 0, &output), "fast control prime");
            fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
            require_hresult(history->capture(1, &cb), "fast control B");
            require_hresult(history->commit(cb), "fast control commit B");
            require_hresult(synth.submit_pair(cb, views, views, 0, 0, &output), "fast control pair");
            const auto image = readback_pattern(fixture, synthetic.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            std::size_t stationary{}, changed{}, strong{}, impossible{};
            for (UINT eye = 0; eye < 2; ++eye) for (UINT y = 32; y + 32 < height; ++y)
                for (UINT x = 32; x + 32 < width; ++x) {
                    const auto i = (static_cast<std::size_t>(y) * width + x) * 4;
                    if (a[eye][i] != b[eye][i] || b[eye][i] != expected[eye][i]) continue;
                    ++stationary;
                    const int error = std::abs(int(image[eye][i]) - int(expected[eye][i]));
                    changed += error > 2;
                    strong += error > 32;
                    if (kind == 2 && expected[eye][i] == 8 && error > 32 &&
                        (x < width / 3 - 64 || x > width * 2 / 3 + 164)) ++impossible;
                }
            std::cout << "quality kind=" << kind << " preset=" << (preset == xrfg::D3D12NvidiaPerformancePreset::fast ? "fast" : "medium")
                << " mae=" << mean_absolute_rgb_error(image, expected, width, height, 0, 32)
                << " stationary=" << stationary << " changed=" << changed << " strong=" << strong
                << " distant_ghost=" << impossible << std::endl;
            if (preset == xrfg::D3D12NvidiaPerformancePreset::fast && kind == 0) {
                require(stationary > 3000000 && changed == 0,
                    "Fast must not invent motion in identical high-contrast captures");
            }
            require_hresult(synth.wait_for_idle(), "fast control drain");
            fixture.require_no_debug_errors();
        }
    }
}
