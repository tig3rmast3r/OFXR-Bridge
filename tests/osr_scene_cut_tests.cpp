// Real OSR shaders and production pass, without OpenXR, a headset or a game.
// All waits/readbacks in this file are fixture-only.
#include "osr_full_warp_pass.hpp"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void ok(HRESULT result, const char* message) { check(SUCCEEDED(result), message); }
constexpr UINT width = 128, height = 72;
struct Fixture {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> info;
    ComPtr<ID3D12Resource> color, depth;
    std::vector<ComPtr<ID3D12Resource>> transfers;
    HANDLE event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    UINT64 serial{};
    xrfg::OsrFullWarp warp;
    bool validation{};
    Fixture() {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer(); validation = true;
        }
        ok(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
        device.As(&info);
        D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ok(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "queue");
        ok(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocator)), "allocator");
        ok(device->CreateCommandList(0, desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
        ok(list->Close(), "initial close");
        ok(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        color = texture(DXGI_FORMAT_R8G8B8A8_UNORM);
        depth = texture(DXGI_FORMAT_R32_FLOAT);
        ok(warp.prepare(device.Get(), width, height, width, height, DXGI_FORMAT_R8G8B8A8_UNORM), "production OSR prepare");
    }
    ~Fixture() { CloseHandle(event); }
    ComPtr<ID3D12Resource> texture(DXGI_FORMAT format) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = width; d.Height = height; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = format; d.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> resource;
        ok(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&resource)), "texture");
        return resource;
    }
    ComPtr<ID3D12Resource> buffer(UINT64 bytes, D3D12_HEAP_TYPE type) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> resource;
        ok(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&resource)), "transfer buffer");
        return resource;
    }
    void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        list->ResourceBarrier(1, &b);
    }
    void upload(ID3D12Resource* resource, const void* pixels) {
        const auto desc = resource->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 bytes{};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
        auto transfer = buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
        void* mapped{}; ok(transfer->Map(0, nullptr, &mapped), "upload map");
        for (UINT y = 0; y < height; ++y)
            std::memcpy(static_cast<char*>(mapped) + fp.Offset + y * fp.Footprint.RowPitch,
                static_cast<const char*>(pixels) + y * width * 4, width * 4);
        transfer->Unmap(0, nullptr);
        transition(resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = resource; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = transfer.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        transfers.push_back(transfer);
    }
    struct Readback { ComPtr<ID3D12Resource> buffer; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint; };
    Readback readback(ID3D12Resource* resource) {
        const auto desc = resource->GetDesc(); Readback r{}; UINT64 bytes{};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &r.footprint, nullptr, nullptr, &bytes);
        r.buffer = buffer(bytes, D3D12_HEAP_TYPE_READBACK);
        transition(resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = r.buffer.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = r.footprint;
        src.pResource = resource; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        return r;
    }
    struct Result { std::array<UINT, 5> state; unsigned history_pixels; UINT center; };
    Result frame(unsigned eye, const std::vector<UINT>& pixels, float z, bool history = true,
        bool reset = false, float history_shift = 0) {
        ok(allocator->Reset(), "reset allocator"); ok(list->Reset(allocator.Get(), nullptr), "reset list");
        transfers.clear();
        upload(color.Get(), pixels.data()); std::vector<float> depths(width * height, z); upload(depth.Get(), depths.data());
        xrfg::OsrFullWarp::Inputs in{};
        in.source_color = color.Get(); in.source_depth = depth.Get(); in.source_eye = eye;
        in.use_history = history; in.reset_history = reset; in.use_history_motion = true;
        in.history_motion_scale[0] = in.history_motion_scale[1] = 1;
        for (unsigned i = 0; i < 4; ++i) {
            in.forward_stereo[i * 5] = in.backward_source[i * 5] = in.backward_history[i * 5] = 1;
            in.forward_history[i * 5] = in.forward_history_previous[i * 5] = 1;
        }
        in.backward_history[12] = history_shift;
        warp.record(device.Get(), list.Get(), in);
        xrfg::OsrFullHistory kept{}; kept.publication = serial + 1;
        warp.record_history_update(list.Get(), in, kept);
        const auto state = readback(warp.scene_state());
        const auto resolved = readback(warp.resolved());
        const auto output = readback(warp.output());
        ok(list->Close(), "close"); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
        ok(queue->Signal(fence.Get(), ++serial), "signal");
        ok(fence->SetEventOnCompletion(serial, event), "completion event");
        check(WaitForSingleObject(event, 15000) == WAIT_OBJECT_0, "fixture GPU completion");
        warp.commit();
        Result result{}; void* mapped{};
        ok(state.buffer->Map(0, nullptr, &mapped), "state map");
        std::memcpy(result.state.data(), static_cast<char*>(mapped) + state.footprint.Offset, sizeof(result.state));
        state.buffer->Unmap(0, nullptr);
        ok(resolved.buffer->Map(0, nullptr, &mapped), "resolved map");
        for (UINT y = 0; y < height; ++y) {
            const auto* row = reinterpret_cast<const UINT*>(static_cast<char*>(mapped) + resolved.footprint.Offset + y * resolved.footprint.Footprint.RowPitch);
            for (UINT x = 0; x < width; ++x) if ((row[x] & 3u) >= 2u) ++result.history_pixels;
        }
        resolved.buffer->Unmap(0, nullptr);
        ok(output.buffer->Map(0, nullptr, &mapped), "output map");
        result.center = *reinterpret_cast<const UINT*>(static_cast<char*>(mapped) + output.footprint.Offset + height / 2 * output.footprint.Footprint.RowPitch + width / 2 * 4);
        output.buffer->Unmap(0, nullptr);
        if (info) for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T bytes{}; info->GetMessage(i, nullptr, &bytes); std::vector<char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data()); info->GetMessage(i, message, &bytes);
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::fprintf(stderr, "D3D12: %s\n", message->pDescription); check(false, "no GPU validation errors");
            }
        }
        if (info) info->ClearStoredMessages();
        std::printf("eye=%u allowed=%u mask=%u cut=%u samples=%u changed=%u motion=%u history_pixels=%u\n",
            eye, result.state[0], result.state[1], result.state[2], result.state[3] & 0xffffu,
            result.state[3] >> 16, result.state[4], result.history_pixels);
        return result;
    }
};
std::vector<UINT> solid(UINT color) { return std::vector<UINT>(width * height, color); }
void no_cut(const Fixture::Result& r, const char* message) { check(r.state[0] == 1 && r.state[2] == 0, message); }
}
int main() {
    Fixture f;
    auto red = solid(0xff1010e0u), blue = solid(0xffe01010u);
    auto r = f.frame(0, red, 0.2F, false);
    check(r.state[0] == 0 && r.state[1] == 1, "first frame initialized without history");
    no_cut(f.frame(1, red, 0.2F), "static frame retains history");
    no_cut(f.frame(0, red, 0.2F), "both eyes retain static history");
    auto exposure = solid(0xff080870u);
    r = f.frame(1, exposure, 0.2F);
    no_cut(r, "exposure-only flash is not a cut");
    check((r.center & 0xffffffu) == (exposure[0] & 0xffffffu), "history stays recorded without replacing current exposure");
    no_cut(f.frame(0, red, 0.2F), "exposure return is not a cut");
    auto local = red;
    std::fill(local.begin(), local.begin() + width * 7, 0xffffffffu);
    no_cut(f.frame(1, local, 0.2F), "localized foreground change is not a cut");
    r = f.frame(0, blue, 0.7F);
    check(r.state[2] == 1 && r.state[0] == 0 && r.state[1] == 1, "hard cut drops both old eyes on first new frame");
    check(r.history_pixels == 0 && (r.center & 0xffffffu) == (blue[0] & 0xffffffu), "cut frame uses only new-scene pixels");
    r = f.frame(0, blue, 0.7F);
    check(r.state[0] == 0 && r.history_pixels == 0, "repeated source eye cannot resurrect old peer");
    r = f.frame(1, blue, 0.7F);
    no_cut(r, "first opposite-eye new scene can reuse new source");
    check(r.state[4] == 1, "motion from two new-scene source frames can resume");
    r = f.frame(0, blue, 0.7F);
    no_cut(r, "new scene warms normally");
    r = f.frame(1, red, 0.7F);
    check(r.state[2] == 1 && r.state[0] == 0 && r.history_pixels == 0, "obvious colour cut with unchanged geometry");
    check((r.center & 0xffffffu) == (red[0] & 0xffffffu), "same-depth cut cannot take old history colour");
    r = f.frame(0, red, 0.7F);
    no_cut(r, "opposite eye starts from new scene after immediate cut");
    check(r.state[4] == 0, "motion spanning cut is suppressed");
    r = f.frame(0, red, 0.7F, true, true);
    check(r.state[0] == 0 && r.state[1] == 1, "provider reset drops OSR state");
    // Camera-only image translation: inverse camera maps samples to the same
    // surfaces in the previous eye. Contrasting stripes catch a wrong matrix.
    std::vector<UINT> pattern(width * height), shifted(width * height);
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
        pattern[y * width + x] = (x / 8) % 2 ? 0xff1010e0u : 0xffe01010u;
        shifted[y * width + x] = ((std::min(x + 8, width - 1)) / 8) % 2 ? 0xff1010e0u : 0xffe01010u;
    }
    f.frame(0, pattern, 0.3F, false, true);
    no_cut(f.frame(1, shifted, 0.3F, true, false, 0.125F), "camera-reprojected translation is not a cut");
    r = f.frame(0, blue, 0.3F, true, false, 4.0F);
    check(r.state[2] == 1 && r.state[0] == 0, "camera cut with virtually no overlap discards old scene");
    std::printf("osr_scene_cut_tests: PASS (D3D12 validation %s)\n", f.validation ? "enabled" : "unavailable");
}
