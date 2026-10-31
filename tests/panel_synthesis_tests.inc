void upload_single_slice(
    D3D12WarpFixture& fixture,
    ID3D12Resource* texture,
    const std::vector<std::uint8_t>& bytes) {
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT row_count = 0;
    UINT64 row_size = 0, total_size = 0;
    fixture.device()->GetCopyableFootprints(
        &description, 0, 1, 0, &layout, &row_count, &row_size, &total_size);
    const UINT width = static_cast<UINT>(description.Width);
    require(bytes.size() == static_cast<std::size_t>(width) * description.Height * kBytesPerPixel,
        "single-slice pattern dimensions do not match the texture");
    ComPtr<ID3D12Resource> upload = create_buffer(
        fixture, D3D12_HEAP_TYPE_UPLOAD, total_size, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    require_hresult(upload->Map(0, &no_read, &mapped), "ID3D12Resource::Map(single-slice upload)");
    for (UINT y = 0; y < row_count; ++y) {
        std::memcpy(static_cast<std::byte*>(mapped) + layout.Offset +
                static_cast<std::size_t>(y) * layout.Footprint.RowPitch,
            bytes.data() + static_cast<std::size_t>(y) * width * kBytesPerPixel,
            static_cast<std::size_t>(row_size));
    }
    upload->Unmap(0, nullptr);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        const auto to_copy = transition_barrier(
            texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &to_copy);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = layout;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = texture;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        const auto back = transition_barrier(
            texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &back);
    });
}
void test_flat_panel_marker_synthesis(
    D3D12WarpFixture& fixture,
    xrfg::D3D12OpticalFlowBackend backend,
    bool hybrid,
    bool require_motion) {
    constexpr UINT kPanelWidth = 1024;
    constexpr UINT kPanelHeight = 512;
    constexpr UINT kMarker = 64;
    constexpr UINT kMarkerY = kPanelHeight / 2 - kMarker / 2;
    constexpr UINT kPreviousX = 256;
    constexpr UINT kCurrentX = 272;
    const auto panel_image = [&](UINT marker_x) {
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(kPanelWidth) * kPanelHeight * kBytesPerPixel, 0);
        for (UINT y = kMarkerY; y < kMarkerY + kMarker; ++y) {
            for (UINT x = marker_x; x < marker_x + kMarker; ++x) {
                const RgbaBytes texel = motion_texel((x - marker_x) * 3U, (y - kMarkerY) * 3U, 0);
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * kPanelWidth + x) * kBytesPerPixel;
                bytes[offset + 0] = texel[0];
                bytes[offset + 1] = texel[1];
                bytes[offset + 2] = texel[2];
                bytes[offset + 3] = 255;
            }
        }
        return bytes;
    };
    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_single_slice_texture(fixture, kPanelWidth, kPanelHeight),
        create_single_slice_texture(fixture, kPanelWidth, kPanelHeight),
    };
    std::array<ComPtr<ID3D12Resource>, 2> current_destinations{
        create_single_slice_texture(fixture, kPanelWidth, kPanelHeight),
        create_single_slice_texture(fixture, kPanelWidth, kPanelHeight),
    };
    std::array<ComPtr<ID3D12Resource>, 1> synthetic_destinations{
        create_single_slice_texture(fixture, kPanelWidth, kPanelHeight),
    };
    const std::vector<std::uint8_t> previous_image = panel_image(kPreviousX);
    const std::vector<std::uint8_t> current_image = panel_image(kCurrentX);
    const std::vector<std::uint8_t> midpoint_image = panel_image((kPreviousX + kCurrentX) / 2U);
    upload_single_slice(fixture, sources[0].Get(), previous_image);
    upload_single_slice(fixture, sources[1].Get(), current_image);
    std::array<ID3D12Resource*, 2> source_pointers{sources[0].Get(), sources[1].Get()};
    std::array<ID3D12Resource*, 2> current_pointers{
        current_destinations[0].Get(), current_destinations[1].Get()};
    std::array<ID3D12Resource*, 1> synthetic_pointers{synthetic_destinations[0].Get()};
    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(operation_succeeded(history->initialize(fixture.device(), fixture.queue(),
                std::span<ID3D12Resource* const>(source_pointers.data(), source_pointers.size()),
                D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "flat panel history initialization failed");
    xrfg::D3D12NvidiaOpticalFlowOptions options{};
    options.hybrid = hybrid;
    options.input_scale=xrfg::D3D12NvidiaInputScale::full;
    xrfg::D3D12FrameSynthesizer synthesizer;
    require(operation_succeeded(synthesizer.initialize(fixture.device(), fixture.queue(), history,
                std::span<ID3D12Resource* const>(current_pointers.data(), current_pointers.size()),
                std::span<ID3D12Resource* const>(synthetic_pointers.data(), synthetic_pointers.size()),
                kFormat, D3D12_RESOURCE_STATE_RENDER_TARGET, backend, options)),
        "flat panel synthesizer initialization failed");
    // The layer's panel view (prepare_panel_generation).
    std::array<xrfg::D3D12ReprojectionView, 1> view{};
    view[0].fov = {-0.785398F, 0.785398F, 0.785398F, -0.785398F};
    view[0].image_rect = {0, 0, kPanelWidth, kPanelHeight};
    view[0].array_slice = 0;
    xrfg::D3D12HistoryCaptureTicket previous{};
    require(operation_succeeded(history->capture(0, &previous)) &&
            operation_succeeded(history->commit(previous)),
        "flat panel previous capture failed");
    xrfg::D3D12FrameSynthesisTicket prime{};
    require(operation_succeeded(synthesizer.submit_prime(previous, view, 0, &prime)),
        "flat panel prime failed");
    require(operation_succeeded(synthesizer.wait_for_previous_submission(5000)),"flat panel prime drain failed");
    // Re-prime an already seeded engine, as a panel returning after a missing
    // capture or hidden frame does. The D3D debug layer checks its barriers.
    require(operation_succeeded(synthesizer.retire_previous()),"panel retire before reprime");
    require(operation_succeeded(synthesizer.submit_prime(previous,view,0,&prime)),"panel reprime");
    require(operation_succeeded(synthesizer.wait_for_previous_submission(5000)),"panel reprime drain");
    xrfg::D3D12HistoryCaptureTicket current{};
    require(operation_succeeded(history->capture(1, &current)) &&
            operation_succeeded(history->commit(current)),
        "flat panel current capture failed");
    xrfg::D3D12FrameSynthesisTicket pair{};
    const auto pair_result=synthesizer.submit_pair(current, view, view, 0, 1, &pair);
    if(FAILED(pair_result))std::cerr<<"Panel pair HRESULT="<<std::hex<<uint32_t(pair_result)<<std::dec<<'\n';
    require(operation_succeeded(pair_result),
        "flat panel pair failed");
    require(operation_succeeded(synthesizer.wait_for_idle()), "flat panel drain failed");
    const std::vector<std::uint8_t> pixels = readback_single_slice(
        fixture, synthetic_destinations[0].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    std::size_t opaque = 0;
    std::size_t stray = 0;
    double alpha_sum = 0.0;
    double alpha_x = 0.0;
    double synthetic_error = 0.0;
    double blend_error = 0.0;
    for (UINT y = 0; y < kPanelHeight; ++y) {
        for (UINT x = 0; x < kPanelWidth; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * kPanelWidth + x) * kBytesPerPixel;
            const std::uint8_t alpha = pixels[offset + 3];
            if (alpha > 200) ++opaque;
            alpha_sum += alpha;
            alpha_x += static_cast<double>(alpha) * (x + 0.5);
            // Well away from either marker: the background stays clear.
            const bool distant = y + 8 < kMarkerY || y > kMarkerY + kMarker + 8 ||
                x + 8 < kPreviousX || x > kCurrentX + kMarker + 8;
            if (distant && alpha > 8) ++stray;
            for (UINT channel = 0; channel < kBytesPerPixel; ++channel) {
                synthetic_error += std::abs(static_cast<int>(pixels[offset + channel]) -
                    static_cast<int>(midpoint_image[offset + channel]));
                blend_error += std::abs((static_cast<int>(previous_image[offset + channel]) +
                    static_cast<int>(current_image[offset + channel])) / 2 -
                    static_cast<int>(midpoint_image[offset + channel]));
            }
        }
    }
    const double samples = static_cast<double>(pixels.size());
    synthetic_error /= samples;
    blend_error /= samples;
    const double centroid = alpha_sum > 0.0 ? alpha_x / alpha_sum : 0.0;
    const double expected = (kPreviousX + kCurrentX) * 0.5 + kMarker * 0.5;
    const double area = static_cast<double>(kMarker) * kMarker;
    std::cout << "flat panel marker backend=" << static_cast<int>(backend)
              << (hybrid ? " hybrid" : "") << ": opaque=" << opaque << "/" << area
              << " centroid=" << centroid << " expected=" << expected << " stray=" << stray
              << " synthetic_mae=" << synthetic_error << " blend_mae=" << blend_error << '\n';
    require(stray == 0, "flat panel background is no longer transparent");
    require(std::abs(centroid - expected) <= 3.0,
        "flat panel marker is not between the frames");
    if (require_motion) {
        require(static_cast<double>(opaque) >= 0.8 * area,
            "flat panel marker is doubled or faded, not moved");
        require(synthetic_error < blend_error * 0.6,
            "flat panel synthetic is no closer to the midpoint than a blend");
    }
    require(operation_succeeded(history->invalidate()), "flat panel history invalidate failed");
}

