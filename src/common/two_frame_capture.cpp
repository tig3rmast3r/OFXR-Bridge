#include "xrfg/two_frame_capture.hpp"
#include "xrfg/two_frame_capture_budget.hpp"
#include "xrfg/diagnostic_hotkeys.hpp"
#include "xrfg/diagnostic_ring_sink.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include <sstream>
#include "xrfg/luke_host_config.hpp"


#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace xrfg {
namespace {

using Microsoft::WRL::ComPtr;
constexpr std::uint64_t kMaximumBytes = 1280ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumImages = 48;

struct Shot {
    TwoFrameImageInfo info{};
    std::uint32_t frame{};
    D3D12_RESOURCE_DESC description{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    std::uint64_t row_bytes{};
    std::uint64_t bytes{};
    ComPtr<ID3D12Resource> readback;
    std::vector<std::byte> cpu_pixels;
    bool cpu_ready{}; // Session::mutex; publish only after the owner-thread Map/copy.
    ComPtr<ID3D12CommandList> producer_list;
    ComPtr<ID3D12CommandAllocator> owned_allocator;
    ComPtr<ID3D12Fence> fence;
    std::uint64_t fence_value{};
    HRESULT status{S_OK};
};

struct ContractShot {
    TwoFrameGuideContract info{};
    std::uint32_t frame{};
};

struct TimelineShot {
    std::uint32_t frame{};
    const char* stream{};
    std::int64_t code{};
    std::uint64_t serial{};
    std::uint64_t display_time{};
    std::uint64_t detail{};
};

struct Session {
    std::mutex mutex;
    std::vector<std::shared_ptr<Shot>> shots;
    std::vector<ContractShot> contracts;
    std::vector<TwoFrameNativeDlssgEvaluation> native_dlssg_evaluations;
    std::vector<TimelineShot> timeline;
    std::vector<std::uint64_t> dlssg_streams;
    std::vector<std::uint64_t> application_swapchains;
    std::filesystem::path directory;
    std::string settings;
    std::uint64_t allocated{};
    std::uint32_t dropped_images{};
    std::uint32_t suppressed_upstream_images{};
    TwoFrameCaptureStageBudget stage_budget;
    bool sealed{};
    bool automatic_origin_loss{};
    std::atomic_bool write_finished{false};
    std::atomic_bool safe_to_release{true};
};

struct Manager {
    std::mutex mutex;
    std::shared_ptr<Session> session;
    std::atomic_bool has_session{};
    std::atomic_uint32_t active_frame{};
    bool pending_start{};
    bool key_down{};
    bool afw_completed_frame{};
    std::uint32_t observed_end_frames{};
    std::thread writer;

    ~Manager() {
        if (writer.joinable()) writer.join();
    }
};

Manager& manager() {
    static Manager value;
    return value;
}

std::string capture_settings() {
    HMODULE module{}; wchar_t runtime[32768]{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&manager), &module);
    GetModuleFileNameW(module, runtime, 32768);
    const auto root=std::filesystem::path(runtime).parent_path();
    const auto game=luke_host_game_directory();
    const auto provider=luke_host_provider(game,root/L"PDAFWPlugin.dll");
    std::string result="AFW provider selected: "+provider.string()+"\n";
    result+="Actual per-evaluation AFW debug/mode: guide-contracts.tsv\n";
    result+="Odd serial is left: "+std::to_string(luke_host_odd_serial_is_left(game,root/L"ofxr_bridge.ini"))+"\n";
    for(const auto& path:{root/L"ofxr_bridge.ini",game/L"ofxr_bridge.ini",game/L"RealVR.ini"}) {
        std::ifstream file(path,std::ios::binary);
        if(!file)continue;
        std::string contents(65536,'\0');file.read(contents.data(),contents.size());
        contents.resize(static_cast<std::size_t>(file.gcount()));
        result+="\n--- "+path.string()+" ---\n"+contents;
    }
    return result;
}

DXGI_FORMAT typed_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return format;
    }
}

D3D12_RESOURCE_BARRIER barrier(
    ID3D12Resource* resource, UINT subresource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER value{};
    value.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    value.Transition.pResource = resource;
    value.Transition.Subresource = subresource;
    value.Transition.StateBefore = before;
    value.Transition.StateAfter = after;
    return value;
}

ComPtr<IUnknown> identity(IUnknown* object) noexcept {
    ComPtr<IUnknown> result;
    if (object) object->QueryInterface(IID_PPV_ARGS(result.GetAddressOf()));
    return result;
}

std::filesystem::path capture_directory() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
        static_cast<DWORD>(path.size()));
    const auto root = length && length < path.size()
        ? std::filesystem::path(path.data()).parent_path()
        : std::filesystem::temp_directory_path();
    SYSTEMTIME time{};
    GetLocalTime(&time);
    static std::atomic_uint32_t sequence{0};
    wchar_t name[128]{};
    swprintf_s(name, L"ofxr-capture-v23286-%lu-%04u%02u%02u-%02u%02u%02u-%03u-%u",
        GetCurrentProcessId(), time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
        ++sequence);
    return root / name;
}

std::shared_ptr<Shot> prepare_shot(
    const std::shared_ptr<Session>& session, std::uint32_t frame,
    const TwoFrameImageInfo& info, ID3D12Device* device,
    ID3D12Resource* source, const D3D12_RESOURCE_DESC* cpu_description=nullptr) {
    if (!session || !device || (!source&&!cpu_description) || !info.stage) return {};
    {
        std::scoped_lock lock(session->mutex);
        if (session->sealed) return {};
        if (!session->stage_budget.accept(info.stage)) {
            ++session->suppressed_upstream_images;
            return {};
        }
    }
    auto shot = std::make_shared<Shot>();
    shot->info = info;
    shot->frame = frame;
    shot->description = cpu_description?*cpu_description:source->GetDesc();
    const auto& desc = shot->description;
    const auto format = typed_format(desc.Format);
    const bool supported_format =
        format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
        format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
        format == DXGI_FORMAT_R32G32_FLOAT ||
        format == DXGI_FORMAT_R10G10B10A2_UNORM ||
        format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        format == DXGI_FORMAT_R16G16_FLOAT ||
        format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
        format == DXGI_FORMAT_R24G8_TYPELESS ||
        format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
        format == DXGI_FORMAT_D32_FLOAT ||
        format == DXGI_FORMAT_R32_FLOAT ||
        format == DXGI_FORMAT_R8G8_UNORM ||
        format == DXGI_FORMAT_R16_FLOAT ||
        format == DXGI_FORMAT_R16_UNORM ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.SampleDesc.Count != 1 || desc.MipLevels == 0 ||
        info.slice >= desc.DepthOrArraySize || !supported_format) {
        shot->status = E_INVALIDARG;
    } else {
        D3D12_RESOURCE_DESC typed = desc;
        typed.Format = format;
        const UINT subresource = info.slice * desc.MipLevels;
        device->GetCopyableFootprints(&typed, subresource, 1, 0,
            &shot->footprint, nullptr, &shot->row_bytes, &shot->bytes);
        if (shot->bytes == 0 || shot->bytes > 128ULL * 1024ULL * 1024ULL) {
            shot->status = E_OUTOFMEMORY;
        } else {
            std::scoped_lock lock(session->mutex);
            if (session->allocated + shot->bytes > kMaximumBytes) {
                shot->status = E_OUTOFMEMORY;
            } else {
                session->allocated += shot->bytes;
            }
        }
        if (SUCCEEDED(shot->status) && cpu_description) {
            shot->cpu_pixels.resize(static_cast<std::size_t>(shot->bytes));
        } else if (SUCCEEDED(shot->status)) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = shot->bytes;
            buffer.Height = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            shot->status = device->CreateCommittedResource(&heap,
                D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(shot->readback.GetAddressOf()));
        }
    }
    {
        std::scoped_lock lock(session->mutex);
        if (!session->sealed && session->shots.size() < kMaximumImages)
            session->shots.push_back(shot);
        else {
            ++session->dropped_images;
            return {};
        }
    }
    return shot;
}

void record_copy(
    Shot& shot, ID3D12GraphicsCommandList* list,
    ID3D12Resource* source, D3D12_RESOURCE_STATES source_state) noexcept {
    if (FAILED(shot.status) || !list || !source) return;
    const UINT subresource = shot.info.slice * shot.description.MipLevels;
    const UINT barrier_subresource =
        (shot.description.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
         shot.description.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
         shot.description.Format == DXGI_FORMAT_R24G8_TYPELESS)
            ? D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES : subresource;
    if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        const auto before = barrier(source, barrier_subresource, source_state,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &before);
    }
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = shot.readback.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint = shot.footprint;
    D3D12_TEXTURE_COPY_LOCATION origin{};
    origin.pResource = source;
    origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    origin.SubresourceIndex = subresource;
    list->CopyTextureRegion(&target, 0, 0, 0, &origin, nullptr);
    if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        const auto after = barrier(source, barrier_subresource,
            D3D12_RESOURCE_STATE_COPY_SOURCE, source_state);
        list->ResourceBarrier(1, &after);
    }
}

bool completed(const Shot& shot) noexcept {
    if (!shot.cpu_pixels.empty()) return shot.cpu_ready;
    return shot.fence && shot.fence_value &&
        shot.fence->GetCompletedValue() >= shot.fence_value;
}

void write_session(const std::shared_ptr<Session>& session) {
    std::error_code ec;
    std::filesystem::create_directories(session->directory, ec);
    std::ofstream manifest(session->directory / L"manifest.tsv", std::ios::binary);
    manifest << "index\tframe\tstage\tstatus\tstream\tdisplay_time\tswapchain\tserial\teye\tslice\tx\ty\twidth\theight\tresource_width\tresource_height\tformat\tsource_format\trow_pitch\trow_bytes\tbytes\tscale_x\tscale_y\tjitter_x\tjitter_y\tfile\n";
    std::vector<std::shared_ptr<Shot>> shots;
    std::vector<ContractShot> contracts;
    std::vector<TwoFrameNativeDlssgEvaluation> native_dlssg_evaluations;
    std::vector<TimelineShot> timeline;
    {
        std::scoped_lock lock(session->mutex);
        shots = session->shots;
        contracts = session->contracts;
        native_dlssg_evaluations = session->native_dlssg_evaluations;
        timeline = session->timeline;
    }
    std::ofstream native_manifest(session->directory / L"native-dlssg-evaluations.tsv", std::ios::binary);
    native_manifest << "frame\tstream\tpublication\tserial\tprevious_serial\treset\tdirect\tresult\tcount\tindex\tflags"
        "\tcolor\tmotion\tdepth\tgenerated\tdisable\twidth\theight\tcolor_format\tinput_color_format"
        "\tbackbuffer_rect\tmotion_rect\tdepth_rect\tmotion_scale\tjitter\tdepth_scale\tnear_far_partition"
        "\tobject_separation\tinvalid_motion\tnear\tfar\tfov\taspect\tposition\tright\tup\tforward"
        "\tview_to_clip\tclip_to_view\tclip_to_lens\tclip_to_previous\tprevious_to_clip\n";
    native_manifest << std::setprecision(9);
    const auto native_array = [&](const auto& values) {
        native_manifest << '\t';
        for (std::size_t i=0;i<values.size();++i) { if(i)native_manifest << ',';native_manifest << values[i]; }
    };
    for (const auto& c : native_dlssg_evaluations) {
        native_manifest << c.frame << '\t' << c.stream << '\t' << c.publication << '\t' << c.serial
            << '\t' << c.previous_serial << '\t' << c.reset << '\t' << c.direct << '\t' << c.result
            << '\t' << c.count << '\t' << c.index << '\t' << c.flags << '\t' << c.color << '\t' << c.motion
            << '\t' << c.depth << '\t' << c.generated << '\t' << c.disable << '\t' << c.width << '\t' << c.height
            << '\t' << c.color_format << '\t' << c.input_color_format;
        native_array(c.backbuffer_rect);native_array(c.motion_rect);native_array(c.depth_rect);
        native_array(c.motion_scale);native_array(c.jitter);
        native_manifest << '\t' << c.depth_scale << '\t' << c.near_far_partition << '\t' << c.object_separation
            << '\t' << c.invalid_motion << '\t' << c.near_plane << '\t' << c.far_plane << '\t' << c.fov << '\t' << c.aspect;
        native_array(c.position);native_array(c.right);native_array(c.up);native_array(c.forward);
        native_array(c.view_to_clip);native_array(c.clip_to_view);native_array(c.clip_to_lens);
        native_array(c.clip_to_previous);native_array(c.previous_to_clip);native_manifest << '\n';
    }
    std::ofstream timeline_manifest(
        session->directory / L"xr-timeline.tsv", std::ios::binary);
    timeline_manifest << "frame\tstream\tcode\tserial\tdisplay_time\tdetail\n";
    for (const TimelineShot& shot : timeline) {
        timeline_manifest << shot.frame << '\t' << shot.stream << '\t'
            << shot.code << '\t' << shot.serial << '\t'
            << shot.display_time << '\t' << shot.detail << '\n';
    }
    std::ofstream guide_manifest(session->directory / L"guide-contracts.tsv",
        std::ios::binary);
    guide_manifest << "frame\tstage\tstream\tpublication\tserial\tprevious_serial"
        "\tsource_eye\tdestination_eye\tmode"
        "\tmotion_vectors_type\tignore_motion_threshold"
        "\tcamera_fov_vertical\tdebug\tclear_before_warping"
        "\tis_hudless_color\tdepth_dilate_mode"
        "\tmotion_width\tmotion_height"
        "\tdepth_width\tdepth_height\toutput_width\toutput_height"
        "\tinput_color_width\tinput_color_height\tinput_depth_width"
        "\tinput_depth_height\tinput_motion_width\tinput_motion_height"
        "\toutput_color_width\toutput_color_height\tdepth_crop_x"
        "\tdepth_crop_y\tmotion_crop_x\tmotion_crop_y"
        "\tscale_x\tscale_y\tjitter_x\tjitter_y\tmotion_scale_x"
        "\tmotion_scale_y\tframe_time_delta_ms\tcamera_near\tcamera_far"
        "\treset\tjittered\tdepth_inverted\tdepth_infinite"
        "\tcamera_position\tcamera_right\tcamera_up\tcamera_forward"
        "\tsource_pose\tdestination_pose"
        "\tsource_orientation\tdestination_orientation"
        "\tview_to_clip\tclip_to_view"
        "\tsource_world_to_view\tsource_view_to_world"
        "\tsource_view_to_clip\tsource_clip_to_view"
        "\tdestination_world_to_view\tdestination_view_to_world"
        "\tdestination_view_to_clip\tdestination_clip_to_view"
        "\tcamera_world_to_view\tcamera_view_to_world"
        "\tcamera_view_to_clip\tcamera_clip_to_view"
        "\traw_camera_motion_included\traw_clip_to_previous_clip\traw_previous_clip_to_clip\tnative_previous_publication\tnative_same_publication\tnative_clip_to_opposite\tnative_clip_to_same\tnative_history_jitter_x\tnative_history_jitter_y"
        "\tmotion_temporal_valid\tmotion_previous_publication\tmotion_middle_publication"
        "\tmotion_current_to_middle_same\tmotion_current_to_previous_same\tmotion_current_to_middle_opposite"
        "\tdepth_native_history_valid\tdepth_native_previous_publication\tdepth_current_to_native_previous"
        "\tcamera_forward_sign\tcamera_up_sign\traw_motion_native_span"
        "\tnative_camera_token\tcolor_origin_token\tnative_production_sequence\tinterleaved_history"
        "\tgta_current_render\tgta_current_mode\tgta_current_yaw\tgta_current_tracking_valid\tgta_current_tracking"
        "\tgta_peer_render\tgta_peer_mode\tgta_peer_yaw\tgta_peer_tracking_valid\tgta_peer_tracking"
        "\tgta_same_render\tgta_same_mode\tgta_same_yaw\tgta_same_tracking_valid\tgta_same_tracking\n";
    const auto write_array = [&guide_manifest](const auto& values) {
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i) guide_manifest << ',';
            guide_manifest << values[i];
        }
    };
    guide_manifest << std::setprecision(9);
    for (const auto& shot : contracts) {
        const auto& c = shot.info;
        guide_manifest << shot.frame << '\t' << c.stage << '\t'
            << c.stream << '\t' << c.publication << '\t' << c.serial
            << '\t' << c.previous_serial << '\t' << c.source_eye << '\t'
            << c.destination_eye << '\t' << c.mode << '\t'
            << c.motion_vectors_type << '\t'
            << c.ignore_motion_threshold << '\t'
            << c.camera_fov_vertical << '\t' << c.debug << '\t'
            << c.clear_before_warping << '\t' << c.is_hudless_color << '\t'
            << c.depth_dilate_mode << '\t'
            << c.motion_width << '\t' << c.motion_height << '\t'
            << c.depth_width << '\t' << c.depth_height << '\t'
            << c.output_width << '\t' << c.output_height << '\t'
            << c.input_color_width << '\t' << c.input_color_height << '\t'
            << c.input_depth_width << '\t' << c.input_depth_height << '\t'
            << c.input_motion_width << '\t' << c.input_motion_height << '\t'
            << c.output_color_width << '\t' << c.output_color_height << '\t'
            << c.depth_crop_x << '\t' << c.depth_crop_y << '\t'
            << c.motion_crop_x << '\t' << c.motion_crop_y << '\t'
            << c.scale_x << '\t' << c.scale_y << '\t'
            << c.jitter_x << '\t' << c.jitter_y << '\t'
            << c.motion_scale_x << '\t' << c.motion_scale_y << '\t'
            << c.frame_time_delta_ms << '\t' << c.camera_near << '\t'
            << c.camera_far << '\t' << c.reset << '\t' << c.jittered
            << '\t' << c.depth_inverted << '\t' << c.depth_infinite << '\t';
        write_array(c.camera_position); guide_manifest << '\t';
        write_array(c.camera_right); guide_manifest << '\t';
        write_array(c.camera_up); guide_manifest << '\t';
        write_array(c.camera_forward); guide_manifest << '\t';
        write_array(c.source_pose); guide_manifest << '\t';
        write_array(c.destination_pose); guide_manifest << '\t';
        write_array(c.source_orientation); guide_manifest << '\t';
        write_array(c.destination_orientation); guide_manifest << '\t';
        write_array(c.view_to_clip); guide_manifest << '\t';
        write_array(c.clip_to_view); guide_manifest << '\t';
        write_array(c.source_world_to_view); guide_manifest << '\t';
        write_array(c.source_view_to_world); guide_manifest << '\t';
        write_array(c.source_view_to_clip); guide_manifest << '\t';
        write_array(c.source_clip_to_view); guide_manifest << '\t';
        write_array(c.destination_world_to_view); guide_manifest << '\t';
        write_array(c.destination_view_to_world); guide_manifest << '\t';
        write_array(c.destination_view_to_clip); guide_manifest << '\t';
        write_array(c.destination_clip_to_view); guide_manifest << '\t';
        write_array(c.camera_world_to_view); guide_manifest << '\t';
        write_array(c.camera_view_to_world); guide_manifest << '\t';
        write_array(c.camera_view_to_clip); guide_manifest << '\t';
        write_array(c.camera_clip_to_view); guide_manifest << '\t';
        guide_manifest << c.raw_camera_motion_included << '\t';
        write_array(c.raw_clip_to_previous_clip); guide_manifest << '\t';
        write_array(c.raw_previous_clip_to_clip); guide_manifest << '\t';
        guide_manifest << c.native_previous_publication << '\t' << c.native_same_publication << '\t';
        write_array(c.native_clip_to_opposite); guide_manifest << '\t';
        write_array(c.native_clip_to_same); guide_manifest << '\t' << c.native_history_jitter_x << '\t' << c.native_history_jitter_y << '\t';
        guide_manifest << c.motion_temporal_valid << '\t' << c.motion_previous_publication << '\t'
            << c.motion_middle_publication << '\t';
        write_array(c.motion_current_to_middle_same); guide_manifest << '\t';
        write_array(c.motion_current_to_previous_same); guide_manifest << '\t';
        write_array(c.motion_current_to_middle_opposite); guide_manifest << '\t';
        guide_manifest << c.depth_native_history_valid << '\t'
            << c.depth_native_previous_publication << '\t';
        write_array(c.depth_current_to_native_previous); guide_manifest << '\t';
        guide_manifest << c.camera_forward_sign << '\t' << c.camera_up_sign << '\t' << c.raw_motion_native_span
            << '\t' << c.native_camera_token << '\t' << c.color_origin_token
            << '\t' << c.native_production_sequence << '\t' << c.interleaved_history;
        for(unsigned i=0;i<3;++i) {
            guide_manifest << '\t' << c.gta_tracking_render[i] << '\t' << c.gta_tracking_mode[i]
                << '\t' << c.gta_tracking_yaw[i] << '\t' << c.gta_tracking_valid[i] << '\t';
            write_array(c.gta_tracking_poses[i]);
        }
        guide_manifest << '\n';
    }
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(15);
    for (std::size_t index = 0; index < shots.size(); ++index) {
        const auto& shot = *shots[index];
        HRESULT status{};
        {
            std::scoped_lock lock(session->mutex);
            status = shot.status;
        }
        if (SUCCEEDED(status)) {
            bool ready = false;
            while (std::chrono::steady_clock::now() < deadline) {
                {
                    std::scoped_lock lock(session->mutex);
                    status = shot.status;
                    ready = completed(shot);
                }
                if (ready || FAILED(status)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!ready && SUCCEEDED(status)) {
                status = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
                session->safe_to_release.store(false);
            }
        }
        std::string file;
        if (SUCCEEDED(status)) {
            void* data = nullptr;
            const D3D12_RANGE read_range{0, static_cast<SIZE_T>(shot.bytes)};
            if (!shot.cpu_pixels.empty()) data=const_cast<std::byte*>(shot.cpu_pixels.data());
            else status = shot.readback->Map(0, &read_range, &data);
            if (SUCCEEDED(status) && data) {
                file = "image-" + std::to_string(index) + ".raw";
                std::ofstream raw(session->directory / file, std::ios::binary);
                raw.write(static_cast<const char*>(data),
                    static_cast<std::streamsize>(shot.bytes));
                if (!raw) status = E_FAIL;
                const D3D12_RANGE written{0, 0};
                if (shot.readback) shot.readback->Unmap(0, &written);
            } else if (SUCCEEDED(status)) {
                status = E_FAIL;
            }
        }
        manifest << index << '\t' << shot.frame << '\t' << shot.info.stage
            << '\t' << std::hex << static_cast<std::uint32_t>(status) << std::dec
            << '\t' << shot.info.stream << '\t' << shot.info.display_time
            << '\t' << shot.info.swapchain << '\t' << shot.info.capture_serial
            << '\t' << shot.info.eye << '\t' << shot.info.slice
            << '\t' << shot.info.x << '\t' << shot.info.y
            << '\t' << shot.info.width << '\t' << shot.info.height
            << '\t' << shot.description.Width << '\t' << shot.description.Height
            << '\t' << static_cast<unsigned>(shot.footprint.Footprint.Format)
            << '\t' << static_cast<unsigned>(shot.description.Format)
            << '\t' << shot.footprint.Footprint.RowPitch
            << '\t' << shot.row_bytes << '\t' << shot.bytes
            << '\t' << shot.info.scale_x << '\t' << shot.info.scale_y
            << '\t' << shot.info.jitter_x << '\t' << shot.info.jitter_y
            << '\t' << file << '\n';
    }
    const std::string message = "OFXR V23286 capture: " +
        session->directory.string() + "\n";
    std::ofstream summary(session->directory / L"README.txt", std::ios::binary);
    summary << "V23286 AFW/OFXR frame-slot and projection-pose audit.\n"
        << "Trigger: " << (session->automatic_origin_loss ? "automatic GTA origin loss (F9)" : "F8")
        << "; capture ends after two AFW evaluations or eight XR frames.\n"
        << "NGX source, AFW inputs, OpenXR eyes, camera matrices and xr-timeline.tsv.\n"
        << "V601: afw_plugin_returned_eye -> afw_generated_eye -> afw_xr_terminal_eye.\n"
        << "xr_final_eye is the application source release BEFORE AFW.\n"
        << "V690 luke_origin_loss_color: exact frozen pair at the CRC failure, recorded immediately before release/reuse; not a guessed guide. origin_loss_crc stores original HRESULT/token per eye.\n"
        << "V685 xr_application_original: ordered D11 application-side shared image, independent of AFW history/runtime output. serial is acquired image index.\n"
        << "layer_type/layer_image/rect/quad timeline records identify the application's actual submitted layers. Original snapshots: at most one per swapchain.\n"
        << "Terminal eyes carry original publication, XR swapchain, pose interval, acquired index and fence receipts.\n"
        << "Raw images include D3D12 row padding; see manifest.tsv.\n"
        << "Max images=48, max total readback=1.25 GiB. Dropped images="
        << session->dropped_images << "\n";
    summary << "AFW capture focus: later upstream NGX images omitted="
        << session->suppressed_upstream_images
        << "; exact consumed guides remain in afw_ngx_*_before_crop.\n";
    summary << "V576 DLSS-G stages: aligned_previous / packed_current / packed_motion / packed_depth -> ngx_generated -> composed_output.\n"
        << "One pair per stream, at most two streams. Native stage swapchain is resource identity, not XR handle.\n"
        << "native_dlssg_pair: original A/B pose/projection. native_dlssg_output_camera: B -> output-to-B quaternion, mode = output index.\n";
    std::ofstream settings(session->directory / L"settings.txt", std::ios::binary);
    settings << session->settings;
    OutputDebugStringA(message.c_str());
    session->write_finished.store(true);
}

}  // namespace

bool two_frame_capture_request_origin_loss() noexcept {
    try {
        if (!bridge_flight_logger().enabled() || diagnostic_ring_enabled) return false;
        auto& state=manager();
        std::scoped_lock lock(state.mutex);
        if (state.session || state.active_frame || state.has_session) return false;
        if (state.writer.joinable()) state.writer.join();
        auto session=std::make_shared<Session>();
        session->directory=capture_directory();
        session->settings=capture_settings();
        session->automatic_origin_loss=true;
        state.session=std::move(session);
        state.has_session.store(true,std::memory_order_relaxed);
        state.pending_start=false;
        state.afw_completed_frame=false;
        state.observed_end_frames=0;
        state.active_frame.store(1,std::memory_order_release);
        return true;
    } catch (...) { return false; }
}

std::uint32_t two_frame_capture_begin_end_frame() noexcept {
    try {
        auto& state = manager();
        if (!bridge_flight_logger().enabled() && !bridge_flight_logger().ram_enabled() &&
            !state.has_session.load(std::memory_order_relaxed) && !state.active_frame.load(std::memory_order_relaxed)) return 0;
        std::scoped_lock lock(state.mutex);
        const bool down = (GetAsyncKeyState(kImageCaptureKey) & 0x8000) != 0;
        const bool pressed = down && !state.key_down;
        state.key_down = down;
        if (state.session && state.session->write_finished.load() &&
            state.session->safe_to_release.load()) {
            if (state.writer.joinable()) state.writer.join();
            state.session.reset();
            state.has_session.store(false,std::memory_order_relaxed);
        }
        if (!state.session && pressed && (bridge_flight_logger().enabled() || bridge_flight_logger().ram_enabled()) &&
            !diagnostic_ring_enabled && !(GetAsyncKeyState(VK_CONTROL)&0x8000)) {
            if (state.writer.joinable()) state.writer.join();
            state.session = std::make_shared<Session>();
            state.has_session.store(true,std::memory_order_relaxed);
            state.session->directory = capture_directory();
            state.session->settings = capture_settings();
            state.pending_start = true;
            state.afw_completed_frame = false;
            state.observed_end_frames = 0;
            return 0;
        }
        return state.active_frame;
    } catch (...) {
        return 0;
    }
}

void two_frame_capture_finish_end_frame(std::uint32_t frame) noexcept {
    try {
        auto& state = manager();
        if (!frame && !state.has_session.load(std::memory_order_relaxed)) return;
        std::scoped_lock lock(state.mutex);
        if (frame == 0) {
            if (state.pending_start) {
                state.active_frame = 1;
                state.pending_start = false;
            }
            return;
        }
        if (!state.session || state.active_frame != frame) return;
        ++state.observed_end_frames;
        const bool completed = state.afw_completed_frame;
        state.afw_completed_frame = false;
        if (completed && frame == 1) {
            state.active_frame = frame + 1;
            return;
        }
        if (!completed && state.observed_end_frames < 8) return;
        state.active_frame = 0;
        const auto session = state.session;
        {
            std::scoped_lock session_lock(session->mutex);
            session->sealed = true;
        }
        state.writer = std::thread([session] {
            try {
                write_session(session);
            } catch (...) {
                session->safe_to_release.store(false);
                session->write_finished.store(true);
            }
        });
        // Keep the session available to a late producer submission. It is
        // retired on a later xrEndFrame only after the writer has finished.
    } catch (...) {
    }
}

void two_frame_capture_mark_afw_evaluation() noexcept {
    try {
        auto& state = manager();
        std::scoped_lock lock(state.mutex);
        if (state.session && state.active_frame != 0)
            state.afw_completed_frame = true;
    } catch (...) {
    }
}

bool two_frame_capture_active() noexcept {
    return diagnostic_ring_enabled.load(std::memory_order_relaxed) || manager().active_frame.load() != 0;
}

bool two_frame_capture_images_active() noexcept {
    return manager().active_frame.load() != 0;
}

bool two_frame_capture_claim_application(std::uint64_t swapchain) noexcept {
    if (!swapchain || !two_frame_capture_images_active()) return false;
    try {
        auto& state=manager();std::scoped_lock lock(state.mutex);
        if (!state.session || !state.active_frame) return false;
        std::scoped_lock session_lock(state.session->mutex);
        auto& handles=state.session->application_swapchains;
        if(state.session->sealed || handles.size()>=8 ||
            std::find(handles.begin(),handles.end(),swapchain)!=handles.end())return false;
        handles.push_back(swapchain);return true;
    } catch (...) { return false; }
}

bool two_frame_capture_claim_dlssg_pair(std::uint64_t stream) noexcept {
    if (!two_frame_capture_images_active()) return false;
    try {
        auto& state = manager();
        std::scoped_lock lock(state.mutex);
        if (!state.active_frame || !state.session) return false;
        std::scoped_lock session_lock(state.session->mutex);
        auto& streams = state.session->dlssg_streams;
        if (state.session->sealed || streams.size() >= 2 ||
            std::find(streams.begin(), streams.end(), stream) != streams.end()) return false;
        streams.push_back(stream);
        return true;
    } catch (...) { return false; }
}

void two_frame_capture_timeline(
    const char* stream,
    std::int64_t code,
    std::uint64_t serial,
    std::uint64_t display_time,
    std::uint64_t detail) noexcept {
    if (stream == nullptr) return;
    if(diagnostic_ring_enabled) {
        diagnostic_ring_record("{\"kind\":\"afw_timeline\",\"qpc\":"+std::to_string(diagnostic_ring_qpc())+
            ",\"stream\":\""+stream+"\",\"code\":"+std::to_string(code)+",\"serial\":"+std::to_string(serial)+
            ",\"display_time\":"+std::to_string(display_time)+",\"detail\":"+std::to_string(detail)+"}");
    }
    try {
        auto& state = manager();
        std::scoped_lock lock(state.mutex);
        const std::uint32_t frame = state.active_frame.load();
        if (!frame || !state.session) return;
        std::scoped_lock session_lock(state.session->mutex);
        if (!state.session->sealed && state.session->timeline.size() < 512) {
            state.session->timeline.push_back({
                frame, stream, code, serial, display_time, detail});
        }
    } catch (...) {
    }
}

void two_frame_capture_native_dlssg_evaluation(const TwoFrameNativeDlssgEvaluation& info) noexcept {
    auto& value=manager();const auto frame=value.active_frame.load(std::memory_order_relaxed);
    if(!frame)return;
    std::shared_ptr<Session> session;
    {std::scoped_lock lock(value.mutex);session=value.session;}
    if(!session)return;
    try {
        std::scoped_lock lock(session->mutex);
        if(session->sealed || session->native_dlssg_evaluations.size()>=16)return;
        auto copy=info;copy.frame=frame;session->native_dlssg_evaluations.push_back(copy);
    } catch (...) {} // An optional F8 observer cannot fail generation.
}

void two_frame_capture_guide_contract(
    const TwoFrameGuideContract& info) noexcept {
    if (!info.stage) return;
    if(diagnostic_ring_enabled) {
        try {
            const auto& c=info;std::ostringstream row;row<<std::setprecision(9);
            row<<"{\"kind\":\"afw_contract\",\"qpc\":"<<diagnostic_ring_qpc()<<",\"stage\":\""<<c.stage
               <<"\",\"stream\":"<<c.stream<<",\"publication\":"<<c.publication<<",\"serial\":"<<c.serial
               <<",\"previous_serial\":"<<c.previous_serial<<",\"source_eye\":"<<c.source_eye<<",\"destination_eye\":"<<c.destination_eye
               <<",\"mode\":"<<c.mode<<",\"debug\":"<<c.debug<<",\"motion_vectors_type\":"<<c.motion_vectors_type;
            const auto array=[&](const char* key,const auto& values){row<<",\""<<key<<"\":[";bool first=true;for(auto value:values){if(!first)row<<',';first=false;row<<value;}row<<']';};
            array("source_pose",c.source_pose);array("destination_pose",c.destination_pose);
            array("source_orientation",c.source_orientation);array("destination_orientation",c.destination_orientation);
            array("source_world_to_view",c.source_world_to_view);array("destination_world_to_view",c.destination_world_to_view);
            array("source_view_to_clip",c.source_view_to_clip);array("destination_view_to_clip",c.destination_view_to_clip);
            array("native_clip_to_opposite",c.native_clip_to_opposite);array("native_clip_to_same",c.native_clip_to_same);
            row<<",\"motion_temporal_valid\":"<<c.motion_temporal_valid<<",\"camera_forward_sign\":"<<c.camera_forward_sign<<'}';
            diagnostic_ring_record(row.str());
        }catch(...){}
    }
    try {
        auto& state = manager();
        std::scoped_lock lock(state.mutex);
        const auto frame = state.active_frame.load();
        if (!frame || !state.session) return;
        std::scoped_lock session_lock(state.session->mutex);
        if (!state.session->sealed && state.session->contracts.size() < 32)
            state.session->contracts.push_back({info, frame});
    } catch (...) {
    }
}

void two_frame_capture_note(
    const TwoFrameImageInfo& info, HRESULT status) noexcept {
    if (!info.stage || SUCCEEDED(status)) return;
    try {
        std::shared_ptr<Session> session;
        std::uint32_t frame{};
        {
            auto& state = manager();
            std::scoped_lock lock(state.mutex);
            session = state.session;
            frame = state.active_frame;
        }
        if (!session || !frame) return;
        auto shot = std::make_shared<Shot>();
        shot->info = info;
        shot->frame = frame;
        shot->status = status;
        std::scoped_lock lock(session->mutex);
        if (!session->sealed && session->shots.size() < kMaximumImages)
            session->shots.push_back(std::move(shot));
        else
            ++session->dropped_images;
    } catch (...) {
    }
}

void two_frame_capture_record_ng_x(
    const TwoFrameImageInfo& info, ID3D12Device* device,
    ID3D12GraphicsCommandList* list, ID3D12Resource* source,
    D3D12_RESOURCE_STATES source_state) noexcept {
    try {
        std::shared_ptr<Session> session;
        std::uint32_t frame{};
        {
            auto& state = manager();
            std::scoped_lock lock(state.mutex);
            session = state.session;
            frame = state.active_frame;
        }
        if (!session || !frame || !list) return;
        auto shot = prepare_shot(session, frame, info, device, source);
        if (!shot) return;
        record_copy(*shot, list, source, source_state);
        if (SUCCEEDED(shot->status)) shot->producer_list = list;
    } catch (...) {
    }
}

HRESULT two_frame_capture_record_d3d11(const TwoFrameImageInfo& info,
    ID3D12Device* footprint_device,ID3D11DeviceContext* context,ID3D11Texture2D* source) noexcept {
    if (!two_frame_capture_images_active()) return S_FALSE;
    try {
        if (!footprint_device||!context||!source) return E_INVALIDARG;
        std::shared_ptr<Session> session;std::uint32_t frame{};
        {auto& state=manager();std::scoped_lock lock(state.mutex);session=state.session;frame=state.active_frame;}
        if(!session||!frame)return S_FALSE;
        ComPtr<ID3D11Device> owner,context_device;source->GetDevice(&owner);context->GetDevice(&context_device);
        ComPtr<IUnknown> oi,ci;
        if(!owner||!context_device||FAILED(owner.As(&oi))||FAILED(context_device.As(&ci))||oi.Get()!=ci.Get())return E_INVALIDARG;
        D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
        if(desc.SampleDesc.Count!=1||info.slice>=desc.ArraySize||!desc.MipLevels)return E_INVALIDARG;
        D3D12_RESOURCE_DESC converted{};converted.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        converted.Width=desc.Width;converted.Height=desc.Height;converted.DepthOrArraySize=static_cast<UINT16>(desc.ArraySize);
        converted.MipLevels=static_cast<UINT16>(desc.MipLevels);converted.Format=desc.Format;converted.SampleDesc.Count=1;
        auto shot=prepare_shot(session,frame,info,footprint_device,nullptr,&converted);
        if(!shot)return E_OUTOFMEMORY;if(FAILED(shot->status))return shot->status;
        auto staging_desc=desc;staging_desc.MipLevels=staging_desc.ArraySize=1;
        staging_desc.Usage=D3D11_USAGE_STAGING;staging_desc.BindFlags=staging_desc.MiscFlags=0;
        staging_desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;HRESULT result=owner->CreateTexture2D(&staging_desc,nullptr,&staging);
        if(SUCCEEDED(result)) {
            context->CopySubresourceRegion(staging.Get(),0,0,0,0,source,info.slice*desc.MipLevels,nullptr);
            D3D11_MAPPED_SUBRESOURCE mapped{};result=context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped);
            if(SUCCEEDED(result)) {
                if(!mapped.pData||mapped.RowPitch<shot->row_bytes)result=E_INVALIDARG;
                else for(UINT y=0;y<desc.Height;++y)
                    std::memcpy(shot->cpu_pixels.data()+shot->footprint.Offset+y*shot->footprint.Footprint.RowPitch,
                        static_cast<const std::byte*>(mapped.pData)+y*mapped.RowPitch,static_cast<std::size_t>(shot->row_bytes));
                context->Unmap(staging.Get(),0);
            }
        }
        {std::scoped_lock lock(session->mutex);shot->status=result;shot->cpu_ready=SUCCEEDED(result);}
        return result;
    } catch(...) {return E_FAIL;}
}

HRESULT two_frame_capture_record_queue(
    const TwoFrameImageInfo& info, ID3D12Device* device,
    ID3D12CommandQueue* queue, ID3D12Resource* source,
    D3D12_RESOURCE_STATES source_state) noexcept {
    try {
        std::shared_ptr<Session> session;
        std::uint32_t frame{};
        {
            auto& state = manager();
            std::scoped_lock lock(state.mutex);
            session = state.session;
            frame = state.active_frame;
        }
        if (!session || !frame) return S_FALSE;
        if (!device || !queue || !source) return E_INVALIDARG;
        auto shot = prepare_shot(session, frame, info, device, source);
        if (!shot) return E_OUTOFMEMORY;
        if (FAILED(shot->status)) return shot->status;
        shot->status = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(shot->owned_allocator.GetAddressOf()));
        ComPtr<ID3D12GraphicsCommandList> list;
        if (SUCCEEDED(shot->status))
            shot->status = device->CreateCommandList(queue->GetDesc().NodeMask,
                D3D12_COMMAND_LIST_TYPE_DIRECT, shot->owned_allocator.Get(), nullptr,
                IID_PPV_ARGS(list.GetAddressOf()));
        if (SUCCEEDED(shot->status)) {
            record_copy(*shot, list.Get(), source, source_state);
            const HRESULT closed = list->Close();
            if (SUCCEEDED(shot->status)) shot->status = closed;
        }
        // Provision completion before submitting; a failed allocation must
        // never strand a diagnostic list whose allocator is released early.
        if (SUCCEEDED(shot->status))
            shot->status = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(shot->fence.GetAddressOf()));
        if (FAILED(shot->status)) return shot->status;
        shot->producer_list = list;
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        shot->status = queue->Signal(shot->fence.Get(), 1);
        if (SUCCEEDED(shot->status)) shot->fence_value = 1;
        else session->safe_to_release.store(false);
        // The retained list is owned, not awaiting an intercepted submission.
        // Keep it until session retirement alongside its allocator.
        return shot->status;
    } catch (...) {
        return E_FAIL;
    }
}

void two_frame_capture_submission(
    ID3D12CommandQueue* queue, std::uint32_t count,
    ID3D12CommandList* const* lists) noexcept {
    if (!queue || !lists || count == 0) return;
    if (!manager().has_session.load(std::memory_order_relaxed) && !manager().active_frame.load(std::memory_order_relaxed)) return;
    try {
        std::shared_ptr<Session> session;
        {
            auto& state = manager();
            std::scoped_lock lock(state.mutex);
            session = state.session;
        }
        if (!session) return;
        std::scoped_lock lock(session->mutex);
        for (const auto& shot : session->shots) {
            if (!shot->producer_list || FAILED(shot->status) || shot->fence) continue;
            const auto producer = identity(shot->producer_list.Get());
            bool match = false;
            for (std::uint32_t index = 0; index < count; ++index) {
                if (producer && producer.Get() == identity(lists[index]).Get()) {
                    match = true;
                    break;
                }
            }
            if (!match) continue;
            ComPtr<ID3D12Device> device;
            shot->status = queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf()));
            if (SUCCEEDED(shot->status))
                shot->status = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                    IID_PPV_ARGS(shot->fence.GetAddressOf()));
            if (SUCCEEDED(shot->status))
                shot->status = queue->Signal(shot->fence.Get(), 1);
            if (SUCCEEDED(shot->status)) shot->fence_value = 1;
            else session->safe_to_release.store(false);
            shot->producer_list.Reset();
        }
    } catch (...) {
    }
}

void two_frame_capture_annotate_projection(
    std::uint32_t frame, std::uint64_t swapchain,
    std::uint64_t capture_serial, std::uint64_t display_time,
    std::int32_t eye, std::uint32_t slice,
    std::uint32_t x, std::uint32_t y,
    std::uint32_t width, std::uint32_t height) noexcept {
    try {
        std::shared_ptr<Session> session;
        {
            auto& state = manager();
            std::scoped_lock lock(state.mutex);
            session = state.session;
        }
        if (!session) return;
        std::scoped_lock lock(session->mutex);
        for (const auto& shot : session->shots) {
            if (shot->frame != frame ||
                shot->info.swapchain != swapchain ||
                shot->info.capture_serial != capture_serial ||
                std::string_view(shot->info.stage) != "xr_final_eye") continue;
            shot->info.display_time = display_time;
            shot->info.eye = eye;
            shot->info.slice = slice;
            shot->info.x = x;
            shot->info.y = y;
            shot->info.width = width;
            shot->info.height = height;
        }
    } catch (...) {
    }
}
}  // namespace xrfg
