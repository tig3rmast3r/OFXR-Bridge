#include "xrfg/luke_afw.hpp"
#include "xrfg/afw_provider_device.hpp"
#include "xrfg/luke_depth_format.hpp"
#include "xrfg/luke_motion_format.hpp"
#include "xrfg/luke_native_motion.hpp"
#include "xrfg/luke_afw_continuity.hpp"
#include "xrfg/luke_recording_scope.hpp"
#include "xrfg/luke_guide_window.hpp"
#include "xrfg/luke_physical_projection.hpp"
#include "xrfg/luke_pose_camera.hpp"
#include "xrfg/luke_gta_eye_reference.hpp"
#include "xrfg/luke_camera_origin.hpp"
#include "xrfg/luke_world_scale.hpp"
#include "xrfg/luke_world_scale_config.hpp"
#include "xrfg/luke_host_config.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/d3d12_ram_timing.hpp"
#include "xrfg/two_frame_capture.hpp"
#include "luke_depth_convert_shader.hpp"
#include "luke_depth_calibrate_shader.hpp"
#include "luke_compose_motion_shader.hpp"
#include "luke_afw_color_scale.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <limits>
#include <new>
#include <utility>

namespace xrfg {
namespace {

struct Matrix4x4 { float values[16]{}; };

struct DeviceParams {
    ID3D11Device* d3d11_device{};
    ID3D11DeviceContext* d3d11_context{};
    ID3D12Device* d3d12_device{};
    ID3D12CommandQueue* d3d12_queue{};
    IDXGIAdapter* dxgi_adapter{};
};

struct FrameWarpInitParams {
    int hmd_width{};
    int hmd_height{};
    DXGI_FORMAT eye_format{DXGI_FORMAT_R8G8B8A8_UNORM};
    DXGI_FORMAT backbuffer_format{DXGI_FORMAT_R8G8B8A8_UNORM};
};

enum EyeIndex { EyeLeft = 0, EyeRight = 1 };
enum FrameWarpMode { None = 0, AlternateEyeWarping = 1, PreviousFrameWarping = 2, CombinedWarping = 3 };
enum ImageType { Image = 0, Depth = 1 };
enum MotionVectorType { Normal = 0, FromOtherEye = 1, ObjectOnly = 2 };

struct TextureDesc {
    ImageType type{Image};
    ID3D12Resource* texture{};
    int srv_position{-1};
    int uav_position{-1};
    D3D12_GPU_DESCRIPTOR_HANDLE shader_resource_view{};
    D3D12_GPU_DESCRIPTOR_HANDLE unordered_access_view{};
    union {
        D3D12_CPU_DESCRIPTOR_HANDLE render_target_view{};
        D3D12_CPU_DESCRIPTOR_HANDLE depth_stencil_view;
    };
    D3D12_RESOURCE_STATES initial_state{D3D12_RESOURCE_STATE_COMMON};
};

struct FrameBufferDesc { TextureDesc color; TextureDesc depth; TextureDesc motion_vectors; };
struct EyeFrameBuffers { FrameBufferDesc eye_frame_buffers[2]; };

struct CameraData {
    Matrix4x4 destination_world_to_view;
    Matrix4x4 destination_view_to_world;
    Matrix4x4 destination_view_to_clip;
    Matrix4x4 destination_clip_to_view;
    Matrix4x4 source_world_to_view;
    Matrix4x4 source_view_to_world;
    Matrix4x4 source_view_to_clip;
    Matrix4x4 source_clip_to_view;
    Matrix4x4 camera_world_to_view;
    Matrix4x4 camera_view_to_world;
    Matrix4x4 camera_view_to_clip;
    Matrix4x4 camera_clip_to_view;
};

struct FrameWarpEvaluateParams {
    void* command_list{};
    FrameBufferDesc* input_eye_frame_buffer{};
    FrameBufferDesc* output_eye_frame_buffer{};
    TextureDesc* ui_color_alpha{};
    float ui_scale[2]{1.0F, 1.0F};
    float ui_position[3]{0.0F, 0.0F, -1.0F};
    float motion_scale[2]{};
    FrameWarpMode mode{};
    EyeIndex eye_index{};
    CameraData* camera_data{};
    bool clear_before_warping{};
    float ignore_motion_threshold{2.5F};
    bool is_hudless_color{true};
    MotionVectorType motion_vectors_type{Normal};
    bool debug{};
    bool is_foveated{};
    RECT foveated_area{};
    TextureDesc* ue_velocity_buffer{};
    bool use_uint64{};
    std::uint32_t shading_rate{9}; // beta6 ShadingRate_1X1
    float reserved[12]{};
};

static_assert(sizeof(TextureDesc) == 56);
static_assert(sizeof(FrameBufferDesc) == 168);
static_assert(sizeof(EyeFrameBuffers) == 336);
static_assert(sizeof(CameraData) == 768);
static_assert(sizeof(FrameWarpEvaluateParams) == 184);
static_assert(offsetof(FrameWarpEvaluateParams, shading_rate) == 132);
static_assert(offsetof(FrameWarpEvaluateParams, reserved) == 136);

struct RendererApi;
using InitDeviceFn = RendererApi* (__stdcall*)(DeviceParams);
using InitFrameWarpFn = EyeFrameBuffers (__stdcall*)(FrameWarpInitParams);
using EvaluateFrameWarpFn = void (__stdcall*)(FrameWarpEvaluateParams&);
using SetupTextureDescFn = void (__fastcall*)(RendererApi*, TextureDesc&);
using CreateTextureFn = bool (__fastcall*)(RendererApi*, int, int, DXGI_FORMAT,
    D3D12_RESOURCE_STATES, TextureDesc&, bool);
using CopyTextureFn = void (__fastcall*)(RendererApi*, ID3D12GraphicsCommandList*,
    TextureDesc&, TextureDesc&, D3D12_BOX, UINT, UINT);

constexpr std::size_t kSetupTextureDescSlot = 14;
constexpr std::size_t kCreateTextureSlot = 16;
constexpr std::size_t kCopyTextureSlot = 20;

// The beta6 renderer allocates its full-rate color history at (W+1,H+1),
// then asks its Copy method for an unboxed CopyResource against (W,H).
// D3D12 rejects that operation. Adapt only this documented one-pixel color
// padding inside our synchronous provider call; never patch code or a shared
// command-list vtable. Every other provider method/copy is forwarded unchanged.
class AfwProviderCopyScope {
    static inline std::mutex provider_mutex_;
    static inline thread_local CopyTextureFn original_copy_{};
    static inline thread_local unsigned repaired_{};
    std::unique_lock<std::mutex> lock_{provider_mutex_};
    void*** instance_{};
    void** original_table_{};
    std::array<void*,32> table_{}; // pinned beta6 D3D12RendererAPI
    CopyTextureFn previous_copy_{};
    unsigned previous_repaired_{};
    static void __fastcall copy(RendererApi* renderer,ID3D12GraphicsCommandList* list,
        TextureDesc& destination,TextureDesc& source,D3D12_BOX box,UINT x,UINT y) {
        if(destination.texture && source.texture && !box.right && !box.bottom && !x && !y) {
            const auto a=destination.texture->GetDesc(),b=source.texture->GetDesc();
            const auto width=std::min(a.Width,b.Width);
            const auto height=std::min(a.Height,b.Height);
            const bool padding=a.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                b.Dimension==a.Dimension && a.Format==b.Format &&
                a.DepthOrArraySize==1 && b.DepthOrArraySize==1 && a.MipLevels==1 && b.MipLevels==1 &&
                a.SampleDesc.Count==1 && b.SampleDesc.Count==1 && width && height &&
                std::max(a.Width,b.Width)-width<=1 && std::max(a.Height,b.Height)-height<=1 &&
                (a.Width!=b.Width || a.Height!=b.Height);
            if(padding) {
                ++repaired_;
                box={0,0,0,static_cast<UINT>(width),height,1};
                original_copy_(renderer,list,destination,source,box,0,0);
                // Clamp the optional history border; do not leave undefined texels.
                if(a.Width>width) {
                    box={static_cast<UINT>(width-1),0,0,static_cast<UINT>(width),height,1};
                    original_copy_(renderer,list,destination,source,box,static_cast<UINT>(width),0);
                }
                if(a.Height>height) {
                    box={0,height-1,0,static_cast<UINT>(width),height,1};
                    original_copy_(renderer,list,destination,source,box,0,height);
                    if(a.Width>width) {
                        box={static_cast<UINT>(width-1),height-1,0,static_cast<UINT>(width),height,1};
                        original_copy_(renderer,list,destination,source,box,static_cast<UINT>(width),height);
                    }
                }
                return;
            }
        }
        original_copy_(renderer,list,destination,source,box,x,y);
    }
public:
    explicit AfwProviderCopyScope(RendererApi* renderer) :
        instance_(reinterpret_cast<void***>(renderer)),original_table_(*instance_),
        previous_copy_(original_copy_),previous_repaired_(repaired_) {
        std::copy_n(original_table_,table_.size(),table_.begin());
        original_copy_=reinterpret_cast<CopyTextureFn>(table_[kCopyTextureSlot]);
        repaired_=0;
        table_[kCopyTextureSlot]=reinterpret_cast<void*>(&copy);
        *instance_=table_.data();
    }
    unsigned repaired() const noexcept {return repaired_;}
    ~AfwProviderCopyScope() {
        *instance_=original_table_;
        original_copy_=previous_copy_;repaired_=previous_repaired_;
    }
};


enum class AfwStage : std::uint64_t {
    enter = 1,
    arguments_valid,
    descriptions_ready,
    camera_ready,
    frame_warp_init_begin,
    frame_warp_init_end,
    depth_create_begin,
    depth_create_end,
    motion_allocate_begin,
    motion_resource_created,
    motion_descriptor_ready,
    resources_ready,
    producer_waited,
    command_list_reset,
    color_copied,
    depth_converted,
    motion_copied,
    evaluate_begin,
    evaluate_end,
    output_copied,
    command_list_closed,
    command_list_executed,
    fence_signaled,
    debug_toggle,
    configuration,
    allocator_wait_begin,
    allocator_wait_end,
    temporal_camera_ready = 37,
};

void report_stage(AfwStage stage, std::int64_t result, std::uint64_t detail,
    std::uint64_t publication) noexcept {
    bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
        result, static_cast<std::uint64_t>(stage), detail, publication);
}

void identity(Matrix4x4& value) noexcept {
    value = {};
    value.values[0] = value.values[5] = value.values[10] = value.values[15] = 1.0F;
}

bool invert(const Matrix4x4& source, Matrix4x4& output) noexcept {
    float a[4][8]{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column)
            a[row][column] = source.values[row * 4 + column];
        a[row][4 + row] = 1.0F;
    }
    for (std::size_t column = 0; column < 4; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < 4; ++row)
            if (std::fabs(a[row][column]) > std::fabs(a[pivot][column])) pivot = row;
        if (std::fabs(a[pivot][column]) < 1.0e-7F) return false;
        if (pivot != column) std::swap(a[pivot], a[column]);
        const float divisor = a[column][column];
        for (float& item : a[column]) item /= divisor;
        for (std::size_t row = 0; row < 4; ++row) {
            if (row == column) continue;
            const float factor = a[row][column];
            for (std::size_t item = 0; item < 8; ++item)
                a[row][item] -= factor * a[column][item];
        }
    }
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            output.values[row * 4 + column] = a[row][4 + column];
    return true;
}

Matrix4x4 multiply(const Matrix4x4& left,
    const Matrix4x4& right) noexcept {
    Matrix4x4 product{};
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            for (std::size_t term = 0; term < 4; ++term)
                product.values[row * 4 + column] +=
                    left.values[row * 4 + term] *
                    right.values[term * 4 + column];
    return product;
}

void view_to_world(const std::array<float, 3>& right,
    const std::array<float, 3>& up, const std::array<float, 3>& forward,
    const std::array<float, 3>& position, Matrix4x4& output) noexcept {
    output = {};
    output.values[0] = right[0];
    output.values[1] = right[1];
    output.values[2] = right[2];
    output.values[4] = up[0];
    output.values[5] = up[1];
    output.values[6] = up[2];
    output.values[8] = forward[0];
    output.values[9] = forward[1];
    output.values[10] = forward[2];
    output.values[12] = position[0];
    output.values[13] = position[1];
    output.values[14] = position[2];
    output.values[15] = 1.0F;
}

bool camera_data(const D3D12ReprojectionView& source,
    const D3D12ReprojectionView& destination,
    const DlssMotionVectorFrame& guide, std::uint32_t source_eye,
    float world_scale, float xr_eye_baseline, float units_per_metre,
    const LukeCameraOrigin& origin, LukeProducerCameraAxes axes,
    CameraData& output,bool native_peer_camera=false,float measured_right_axis=0) noexcept {
    output = {};
    if (!guide.camera_data_valid || !guide.camera_matrices_valid ||
        source_eye > 1) return false;
    float game_baseline{};
    // AER submits cross-time eye poses. Their distance includes head motion,
    // not just IPD. Use ONE simultaneous XR pair, including the middle peer.
    if (!luke_game_eye_baseline(xr_eye_baseline, world_scale,
            units_per_metre, game_baseline))
        return false;

    auto source_projection = guide.camera_view_to_clip;
    auto destination_projection = guide.camera_view_to_clip;
    if (!luke_physical_eye_projection(source_projection,
            source.fov.angle_left, source.fov.angle_right,
            source.fov.angle_up, source.fov.angle_down) ||
        !luke_physical_eye_projection(destination_projection,
            destination.fov.angle_left, destination.fov.angle_right,
            destination.fov.angle_up, destination.fov.angle_down))
        return false;
    std::copy(source_projection.begin(), source_projection.end(),
        output.source_view_to_clip.values);
    std::copy(destination_projection.begin(), destination_projection.end(),
        output.destination_view_to_clip.values);
    if (!invert(output.source_view_to_clip, output.source_clip_to_view) ||
        !invert(output.destination_view_to_clip,
            output.destination_clip_to_view)) return false;

    LukeCameraBasis source_basis{};
    if (!luke_view_camera_basis({guide.camera_right, guide.camera_up,
            guide.camera_forward}, guide.camera_view_to_clip[11], source_basis, axes))
        return false;
    const auto source_position = origin.position_relative(guide.camera_position);
    if (!std::all_of(source_position.begin(), source_position.end(),
            [](float coordinate) { return std::isfinite(coordinate); }))
        return false;
    view_to_world(source_basis[0], source_basis[1], source_basis[2],
        source_position, output.source_view_to_world);
    if (!invert(output.source_view_to_world, output.source_world_to_view))
        return false;
    const float view_x_sign = std::copysign(1.F, guide.camera_view_to_clip[0]);
    const float view_y_sign = std::copysign(1.F, guide.camera_view_to_clip[5]);
    // Whether camera_right points to the viewer's right, as measured from the
    // producer's alternating cameras; the projection's x sign only until that
    // is known. It places the peer eye and maps the XR rotation into these
    // axes: a mirrored guess there turns the generated eye against the head.
    const float right_axis = !native_peer_camera && measured_right_axis != 0 ? measured_right_axis : view_x_sign;
    const float direction = (source_eye == 0 ? 1.0F : -1.0F) * right_axis;
    std::array<float, 3> destination_position{
        source_position[0] + direction * guide.camera_right[0] * game_baseline,
        source_position[1] + direction * guide.camera_right[1] * game_baseline,
        source_position[2] + direction * guide.camera_right[2] * game_baseline};
    LukeCameraBasis destination_basis{};
    if(native_peer_camera) {
        // The peer receipt owns its projection and submitted AER head pose,
        // not the gameplay-camera time of the generated view. Keep the scene
        // at this source production; retaining the old scene camera makes the
        // static generated background effectively repeat the old real peer.
        const auto& peer=guide.previous_opposite_guide;
        if(!guide.gta_headlock || !guide.gta_headlock->active() || !peer ||
            peer->stream!=guide.stream || peer->epoch!=guide.epoch ||
            peer->publication+1!=guide.publication || !peer->camera_data_valid ||
            !peer->camera_matrices_valid || peer->output_width!=guide.output_width ||
            peer->output_height!=guide.output_height ||
            !luke_view_camera_basis({peer->camera_right,peer->camera_up,peer->camera_forward},
                peer->camera_view_to_clip[11],destination_basis,axes))return false;
        if(!std::all_of(peer->camera_position.begin(),peer->camera_position.end(),
            [](float v){return std::isfinite(v);}))return false;
        destination_projection=peer->camera_view_to_clip;
        if(!luke_physical_eye_projection(destination_projection,
            destination.fov.angle_left,destination.fov.angle_right,
            destination.fov.angle_up,destination.fov.angle_down))return false;
        std::copy(destination_projection.begin(),destination_projection.end(),
            output.destination_view_to_clip.values);
        if(!invert(output.destination_view_to_clip,output.destination_clip_to_view))return false;
    }
    if(!luke_destination_view_camera_basis(source.pose.orientation,
            destination.pose.orientation,source_basis,guide.camera_view_to_clip[11],
            destination_basis,right_axis,view_y_sign))return false;
    if(native_peer_camera && source_eye==1) {
        // GTA's locked producer anchors the right eye at zero. Its left-eye
        // offset follows the left head orientation, not the newer right one.
        // Keep the existing measured distance, with that observed rig direction.
        for(unsigned axis=0;axis<3;++axis)
            destination_position[axis]=source_position[axis]+
                direction*destination_basis[0][axis]*game_baseline;
    }
    view_to_world(destination_basis[0], destination_basis[1],
        destination_basis[2], destination_position,
        output.destination_view_to_world);
    if (!invert(output.destination_view_to_world,
                output.destination_world_to_view)) return false;
    // PureDark reserves these four matrices for optional UI rendering;
    // source/destination matrices above drive the scene warp.
    identity(output.camera_world_to_view);
    identity(output.camera_view_to_world);
    identity(output.camera_view_to_clip);
    identity(output.camera_clip_to_view);
    return true;
}

bool compatible_color(const D3D12_RESOURCE_DESC& left,
    const D3D12_RESOURCE_DESC& right) noexcept {
    return left.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        right.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        left.Width == right.Width && left.Height == right.Height &&
        left.Format == right.Format && left.DepthOrArraySize == 1 &&
        right.DepthOrArraySize == 1 && left.SampleDesc.Count == 1 &&
        right.SampleDesc.Count == 1;
}

enum class ColorFormatGroup {
    none,
    rgba8,
    bgra8,
};

ColorFormatGroup color_format_group(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return ColorFormatGroup::rgba8;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return ColorFormatGroup::bgra8;
    default:
        return ColorFormatGroup::none;
    }
}

DXGI_FORMAT linear_color_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return format;
    }
}

bool copy_compatible_color(const D3D12_RESOURCE_DESC& left,
    const D3D12_RESOURCE_DESC& right) noexcept {
    const ColorFormatGroup left_group = color_format_group(left.Format);
    const bool compatible_format = left.Format == right.Format ||
        (left_group != ColorFormatGroup::none &&
            left_group == color_format_group(right.Format));
    return left.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        right.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        left.Width == right.Width && left.Height == right.Height &&
        compatible_format && left.DepthOrArraySize == 1 &&
        right.DepthOrArraySize == 1 && left.SampleDesc.Count == 1 &&
        right.SampleDesc.Count == 1;
}

}  // namespace

struct LukeAfwSynthesizer::Impl {
    AfwProviderAllocations provider_allocations;
    std::unique_ptr<AfwProviderDevice> provider_device;
    HMODULE module{};
    std::filesystem::path provider_path;
    std::filesystem::path private_provider_file;
    InitDeviceFn init_device{};
    InitFrameWarpFn init_frame_warp{};
    EvaluateFrameWarpFn evaluate{};
    RendererApi* renderer{};
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> command_allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list;
    Microsoft::WRL::ComPtr<ID3D12Fence> submission_fence;
    HANDLE submission_event{};
    std::uint64_t last_submission_fence{};
    std::uint64_t next_submission_fence{1};
    D3D12RamTiming<4> gpu_timing;
    bool gpu_timing_initialized{};
    EyeFrameBuffers eye_buffers{};
    AfwColorScale color_scale;
    UINT work_width{},work_height{},quality_divisor{1};
    DXGI_FORMAT checked_quality_format{DXGI_FORMAT_UNKNOWN};
    bool quality_can_scale{};
    int active_shading_rate{-1}; // changes replace retired provider state, never mutate it
    std::array<CameraData, 2> previous_source_camera{};
    std::array<Quaternion,2> previous_source_xr_orientation{};
    std::array<bool,2> previous_source_headlocked{};
    std::uint64_t last_evaluated_publication{};
    std::array<std::uint64_t,4> paired_unit_identity{};
    unsigned last_evaluated_eye{2};
    std::array<std::uint64_t, 2> previous_source_stream{};
    std::array<std::uint64_t, 2> previous_source_epoch{};
    std::array<std::uint64_t, 2> previous_source_publication{};
    TextureDesc depth;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_staging;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> depth_descriptor_heap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> depth_root_signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> depth_pipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> depth_calibration_pipeline;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_remap_state;
    struct DepthCalibrationContract {
        std::uint32_t raw_motion_native_span{1};
        bool valid{};
        std::uint64_t stream{}, epoch{};
        std::array<float, 16> projection{};
        std::array<UINT, 14> window{};
        DXGI_FORMAT output_format{};
        bool inverted{};
    };
    std::array<DepthCalibrationContract, 2> depth_calibration_contracts{};
    bool depth_calibration_reset_pending{true};
    D3D12_RESOURCE_STATES depth_staging_state{D3D12_RESOURCE_STATE_COMMON};
    UINT depth_descriptor_size{};
    DXGI_FORMAT depth_source_format{};
    UINT depth_source_width{}, depth_source_height{};
    UINT depth_width{};
    UINT depth_height{};
    Microsoft::WRL::ComPtr<ID3D12Resource> motion_resource;
    TextureDesc motion;
    Microsoft::WRL::ComPtr<ID3D12Resource> composed_motion_staging;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> composed_motion_descriptors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> composed_motion_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> composed_motion_pipeline;
    UINT composed_motion_descriptor_size{};
    UINT composed_motion_width{};
    UINT composed_motion_height{};
    D3D12_RESOURCE_DESC color_description{};
    D3D12_RESOURCE_DESC motion_description{};
    float world_scale{};
    LukeCameraOrigin camera_origin;
    LukeProducerCameraAxes camera_axes{LukeProducerCameraAxes::view};
    float calibrated_units_per_metre{};
    LukeRightAxisSign right_axis;
    std::uint64_t unit_epoch{};
    std::uint64_t unit_stream{};
    std::array<ULONGLONG, 64> input_rejection_report_ticks{};
    bool frame_buffers_ready{};
    bool clean_frame_buffers{true};

    ~Impl() {
        // Normal live resets already retired last_submission_fence above.
        // Cold/failure teardown also proves queue completion before releasing
        // surviving provider factory references. A timeout leaves them alive.
        bool retired=false;
        if(queue && submission_fence && submission_event) {
            auto receipt=last_submission_fence;
            if(!receipt) {
                receipt=next_submission_fence++;
                if(FAILED(queue->Signal(submission_fence.Get(),receipt)))receipt=0;
            }
            if(receipt) {
                if(submission_fence->GetCompletedValue()<receipt &&
                    SUCCEEDED(submission_fence->SetEventOnCompletion(receipt,submission_event)))
                    WaitForSingleObject(submission_event,250);
                retired=submission_fence->GetCompletedValue()>=receipt;
            }
        }
        if (module != nullptr && FreeLibrary(module) && retired)
            provider_allocations.retire();
        if (submission_event != nullptr) CloseHandle(submission_event);
        if (!private_provider_file.empty()) DeleteFileW(private_provider_file.c_str());
    }

    template <typename Function>
    Function method(std::size_t slot) const noexcept {
        if (renderer == nullptr) return nullptr;
        void** table = *reinterpret_cast<void***>(renderer);
        return table == nullptr ? nullptr : reinterpret_cast<Function>(table[slot]);
    }


    HRESULT ensure_float_depth(UINT width, UINT height,
        const D3D12_RESOURCE_DESC& source) noexcept {
        const auto format = luke_depth_format(source.Format);
        if (format.resource == DXGI_FORMAT_UNKNOWN ||
            source.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            source.Width > INT_MAX || source.DepthOrArraySize != 1 ||
            source.SampleDesc.Count != 1) return DXGI_ERROR_UNSUPPORTED;
        if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX)
            return E_INVALIDARG;
        if (depth.texture != nullptr && depth_staging != nullptr &&
            depth_remap_state != nullptr && depth_calibration_pipeline != nullptr &&
            depth_width == width && depth_height == height &&
            depth_source_format == source.Format && depth_source_width == source.Width &&
            depth_source_height == source.Height) return S_OK;

        const auto create = method<CreateTextureFn>(kCreateTextureSlot);
        if (create == nullptr) return E_NOINTERFACE;
        TextureDesc created{};
        created.type = Depth;
        if (!create(renderer, static_cast<int>(width), static_cast<int>(height),
                DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                created, true) ||
            created.texture == nullptr) return E_FAIL;
        created.type = Depth;
        created.initial_state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

        D3D12_HEAP_PROPERTIES heap_properties{};
        heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_properties.CreationNodeMask = 1;
        heap_properties.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC staging_description{};
        staging_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        staging_description.Width = source.Width;
        staging_description.Height = source.Height;
        staging_description.DepthOrArraySize = 1;
        staging_description.MipLevels = 1;
        staging_description.Format = format.resource;
        staging_description.SampleDesc.Count = 1;
        staging_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        staging_description.Flags = D3D12_RESOURCE_FLAG_NONE;
        Microsoft::WRL::ComPtr<ID3D12Resource> staging;
        HRESULT result = device->CreateCommittedResource(&heap_properties,
            D3D12_HEAP_FLAG_NONE, &staging_description,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(staging.GetAddressOf()));
        if (FAILED(result)) return result;

        D3D12_RESOURCE_DESC remap_description{};
        remap_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        remap_description.Width = 1;
        remap_description.Height = 2;
        remap_description.DepthOrArraySize = 1;
        remap_description.MipLevels = 1;
        remap_description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        remap_description.SampleDesc.Count = 1;
        remap_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        remap_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Microsoft::WRL::ComPtr<ID3D12Resource> remap_state;
        result = device->CreateCommittedResource(&heap_properties,
            D3D12_HEAP_FLAG_NONE, &remap_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(remap_state.GetAddressOf()));
        if (FAILED(result)) return result;

        D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
        heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_description.NumDescriptors = 5;
        heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
        result = device->CreateDescriptorHeap(
            &heap_description, IID_PPV_ARGS(descriptors.GetAddressOf()));
        if (FAILED(result)) return result;

        std::array<D3D12_DESCRIPTOR_RANGE, 2> ranges{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 3;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 0;
        std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
        for (UINT index = 0; index < ranges.size(); ++index) {
            parameters[index].ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[index].DescriptorTable.NumDescriptorRanges = 1;
            parameters[index].DescriptorTable.pDescriptorRanges = &ranges[index];
            parameters[index].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        parameters[2].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants.ShaderRegister = 0;
        parameters[2].Constants.Num32BitValues = 52;
        parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.NumParameters = static_cast<UINT>(parameters.size());
        root_description.pParameters = parameters.data();
        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        result = D3D12SerializeRootSignature(&root_description,
            D3D_ROOT_SIGNATURE_VERSION_1, serialized.GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(result)) return result;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
        result = device->CreateRootSignature(queue->GetDesc().NodeMask,
            serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(root_signature.GetAddressOf()));
        if (FAILED(result)) return result;

        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description{};
        pipeline_description.pRootSignature = root_signature.Get();
        pipeline_description.CS = {g_xrfg_luke_depth_convert_shader,
            sizeof(g_xrfg_luke_depth_convert_shader)};
        pipeline_description.NodeMask = queue->GetDesc().NodeMask;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
        result = device->CreateComputePipelineState(&pipeline_description,
            IID_PPV_ARGS(pipeline.GetAddressOf()));
        if (FAILED(result)) return result;
        pipeline_description.CS = {g_xrfg_luke_depth_calibrate_shader,
            sizeof(g_xrfg_luke_depth_calibrate_shader)};
        Microsoft::WRL::ComPtr<ID3D12PipelineState> calibration_pipeline;
        result = device->CreateComputePipelineState(&pipeline_description,
            IID_PPV_ARGS(calibration_pipeline.GetAddressOf()));
        if (FAILED(result)) return result;

        const UINT descriptor_size = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            descriptors->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = format.srv;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(staging.Get(), &srv, cpu);
        cpu.ptr += 3ULL * descriptor_size;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(created.texture, nullptr, &uav, cpu);
        cpu.ptr += descriptor_size;
        uav.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        device->CreateUnorderedAccessView(remap_state.Get(), nullptr, &uav, cpu);
        result = device->GetDeviceRemovedReason();
        if (FAILED(result)) return result;

        depth = created;
        depth_staging = std::move(staging);
        depth_descriptor_heap = std::move(descriptors);
        depth_root_signature = std::move(root_signature);
        depth_pipeline = std::move(pipeline);
        depth_calibration_pipeline = std::move(calibration_pipeline);
        depth_remap_state = std::move(remap_state);
        depth_calibration_contracts = {};
        depth_calibration_reset_pending = true;
        depth_staging_state = D3D12_RESOURCE_STATE_COMMON;
        depth_descriptor_size = descriptor_size;
        depth_source_format = source.Format;
        depth_source_width = static_cast<UINT>(source.Width);
        depth_source_height = source.Height;
        depth_width = width;
        depth_height = height;
        return S_OK;
    }

    HRESULT convert_depth(ID3D12GraphicsCommandList* list,
        ID3D12Resource* source, D3D12_RESOURCE_STATES source_state,
        UINT source_x, UINT source_y, bool depth_inverted,
        int depth_dilate_mode, const DlssMotionVectorFrame& guide,
        const Matrix4x4& middle_opposite_transform,
        const Matrix4x4& previous_same_transform,
        UINT motion_crop_x, UINT motion_crop_y,
        UINT middle_crop_x, UINT middle_crop_y, bool temporal_valid,
        UINT source_eye) noexcept {
        if (list == nullptr || source == nullptr || depth.texture == nullptr ||
            depth_staging == nullptr || depth_descriptor_heap == nullptr ||
            depth_root_signature == nullptr || depth_pipeline == nullptr ||
            depth_calibration_pipeline == nullptr || depth_remap_state == nullptr)
            return E_POINTER;
        if (source_eye >= depth_calibration_contracts.size()) return E_INVALIDARG;
        const auto source_description = source->GetDesc();
        if ((source_description.Format != depth_source_format ||
                source_description.Width != depth_source_width ||
                source_description.Height != depth_source_height) ||
            static_cast<std::uint64_t>(source_x) + depth_width >
                source_description.Width ||
            static_cast<std::uint64_t>(source_y) + depth_height >
                source_description.Height)
            return DXGI_ERROR_UNSUPPORTED;

        const auto transition = [](ID3D12Resource* resource,
            D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            return barrier;
        };
        const auto& middle = guide.previous_opposite_guide;
        // This calibration fits consecutive alternating-eye disparity. An
        // already same-eye native field cannot observe that stereo remap.
        const bool stereo_depth_valid = temporal_valid && middle &&
            guide.raw_motion_native_span==1&&middle->raw_motion_native_span==1&&
            middle->motion_vectors && guide.motion_vectors &&
            middle->motion_vectors.Get() != guide.motion_vectors.Get() &&
            middle->motion_width == guide.motion_width &&
            middle->motion_height == guide.motion_height &&
            middle->output_width == guide.output_width &&
            middle->output_height == guide.output_height &&
            middle->motion_x + middle_crop_x + depth_width <= middle->motion_vectors->GetDesc().Width &&
            middle->motion_y + middle_crop_y + depth_height <= middle->motion_vectors->GetDesc().Height;
        // All producer guides are retained and GPU-ordered. Never infer an
        // earlier motion field from today's resource or a guessed publication.
        if (stereo_depth_valid && middle->producer_fence && middle->producer_fence_value) {
            const HRESULT wait = queue->Wait(middle->producer_fence.Get(), middle->producer_fence_value);
            if (FAILED(wait)) return wait;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE motion_cpu = depth_descriptor_heap->GetCPUDescriptorHandleForHeapStart();
        motion_cpu.ptr += depth_descriptor_size;
        for (UINT i = 0; i < 2; ++i) {
            ID3D12Resource* resource = i && stereo_depth_valid
                ? middle->motion_vectors.Get() : guide.motion_vectors.Get();
            D3D12_SHADER_RESOURCE_VIEW_DESC motion_srv{};
            motion_srv.Format = luke_motion_srv_format(resource->GetDesc().Format);
            if (motion_srv.Format == DXGI_FORMAT_UNKNOWN) return DXGI_ERROR_UNSUPPORTED;
            motion_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            motion_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            motion_srv.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(resource, &motion_srv, motion_cpu);
            motion_cpu.ptr += depth_descriptor_size;
        }
        std::array<D3D12_RESOURCE_BARRIER, 2> motion_before{};
        UINT motion_barrier_count = 0;
        if (guide.resource_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            motion_before[motion_barrier_count++] = transition(guide.motion_vectors.Get(),
                guide.resource_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (stereo_depth_valid && middle->resource_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            motion_before[motion_barrier_count++] = transition(middle->motion_vectors.Get(),
                middle->resource_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (motion_barrier_count) list->ResourceBarrier(motion_barrier_count, motion_before.data());
        std::array<D3D12_RESOURCE_BARRIER, 2> before_copy{};
        UINT before_count = 0;
        if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE)
            before_copy[before_count++] = transition(
                source, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (depth_staging_state != D3D12_RESOURCE_STATE_COPY_DEST)
            before_copy[before_count++] = transition(depth_staging.Get(),
                depth_staging_state, D3D12_RESOURCE_STATE_COPY_DEST);
        if (before_count != 0) list->ResourceBarrier(before_count, before_copy.data());

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = depth_staging.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION source_location{};
        source_location.pResource = source;
        source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source_location.SubresourceIndex = 0;
        // Depth/stencil planes require a full-subresource copy. Crop only
        // when sampling the private shader-readable depth plane.
        list->CopyTextureRegion(&destination, 0, 0, 0,
            &source_location, nullptr);

        std::array<D3D12_RESOURCE_BARRIER, 3> before_dispatch{};
        UINT dispatch_count = 0;
        if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE)
            before_dispatch[dispatch_count++] = transition(source,
                D3D12_RESOURCE_STATE_COPY_SOURCE, source_state);
        before_dispatch[dispatch_count++] = transition(depth_staging.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        before_dispatch[dispatch_count++] = transition(depth.texture,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(dispatch_count, before_dispatch.data());
        depth_staging_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        ID3D12DescriptorHeap* heaps[] = {depth_descriptor_heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(depth_root_signature.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE gpu =
            depth_descriptor_heap->GetGPUDescriptorHandleForHeapStart();
        list->SetComputeRootDescriptorTable(0, gpu);
        gpu.ptr += 3ULL * depth_descriptor_size;
        list->SetComputeRootDescriptorTable(1, gpu);
        struct DepthConstants {
            UINT inverted, dilate, x, y;
            UINT current_x, current_y, middle_x, middle_y;
            float scale_x, scale_y, middle_scale_x, middle_scale_y;
            float width, height;
            UINT valid, reset_calibration;
            float crop_delta[4];
            float middle[16], previous[16];
        };
        static_assert(sizeof(DepthConstants) == 52 * sizeof(UINT));
        DepthConstants depth_options{};
        depth_options.inverted = depth_inverted ? 1U : 0U;
        // Bit 8: the provider receives reversed depth (see provider_camera).
        depth_options.dilate = static_cast<UINT>(depth_dilate_mode) | (depth_inverted ? 0U : 0x100U);
        depth_options.x = source_x; depth_options.y = source_y;
        depth_options.current_x = guide.motion_x + motion_crop_x;
        depth_options.current_y = guide.motion_y + motion_crop_y;
        depth_options.middle_x = middle ? middle->motion_x + middle_crop_x : depth_options.current_x;
        depth_options.middle_y = middle ? middle->motion_y + middle_crop_y : depth_options.current_y;
        depth_options.scale_x = guide.scale_x; depth_options.scale_y = guide.scale_y;
        depth_options.middle_scale_x = middle ? middle->scale_x : guide.scale_x;
        depth_options.middle_scale_y = middle ? middle->scale_y : guide.scale_y;
        depth_options.width = static_cast<float>(depth_width);
        depth_options.height = static_cast<float>(depth_height);
        depth_options.valid = stereo_depth_valid ? 1U : 0U;
        // AER may use distinct projections/origins in its two eyes. Compare
        // each eye with its own last contract, not with the adjacent eye.
        auto& contract = depth_calibration_contracts[source_eye];
        const std::array<UINT, 14> window{
            source_x, source_y, depth_options.current_x, depth_options.current_y,
            depth_options.middle_x, depth_options.middle_y,
            guide.output_x, guide.output_y, guide.output_width, guide.output_height,
            guide.motion_width, guide.motion_height,
            static_cast<UINT>(color_description.Width), color_description.Height};
        if (!contract.valid || contract.stream != guide.stream ||
            contract.raw_motion_native_span != guide.raw_motion_native_span ||
            contract.epoch != guide.epoch || contract.inverted != depth_inverted ||
            contract.output_format != color_description.Format || contract.window != window ||
            std::memcmp(contract.projection.data(), guide.camera_view_to_clip.data(),
                sizeof(contract.projection)) != 0) {
            depth_calibration_reset_pending = true;
            contract.valid = true;
            contract.stream = guide.stream;
            contract.raw_motion_native_span = guide.raw_motion_native_span;
            contract.epoch = guide.epoch;
            contract.projection = guide.camera_view_to_clip;
            contract.window = window;
            contract.output_format = color_description.Format;
            contract.inverted = depth_inverted;
        }
        depth_options.reset_calibration = depth_calibration_reset_pending ? 1U : 0U;
        depth_options.crop_delta[0] = static_cast<float>(motion_crop_x) - static_cast<float>(middle_crop_x);
        depth_options.crop_delta[1] = static_cast<float>(motion_crop_y) - static_cast<float>(middle_crop_y);
        std::copy(std::begin(middle_opposite_transform.values), std::end(middle_opposite_transform.values), depth_options.middle);
        std::copy(std::begin(previous_same_transform.values), std::end(previous_same_transform.values), depth_options.previous);
        list->SetComputeRoot32BitConstants(2, 52, &depth_options, 0);
        list->SetPipelineState(depth_calibration_pipeline.Get());
        list->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER calibration_ready{};
        calibration_ready.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        calibration_ready.UAV.pResource = depth_remap_state.Get();
        list->ResourceBarrier(1, &calibration_ready);
        list->SetPipelineState(depth_pipeline.Get());
        list->Dispatch((depth_width + 7) / 8, (depth_height + 7) / 8, 1);

        std::array<D3D12_RESOURCE_BARRIER, 2> after_dispatch{};
        after_dispatch[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        after_dispatch[0].UAV.pResource = depth.texture;
        after_dispatch[1] = transition(depth.texture,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        list->ResourceBarrier(static_cast<UINT>(after_dispatch.size()),
            after_dispatch.data());
        for (UINT i = 0; i < motion_barrier_count; ++i)
            std::swap(motion_before[i].Transition.StateBefore, motion_before[i].Transition.StateAfter);
        if (motion_barrier_count) list->ResourceBarrier(motion_barrier_count, motion_before.data());
        return S_OK;
    }

    HRESULT ensure_motion(const D3D12_RESOURCE_DESC& description,
        UINT roi_width, UINT roi_height,
        std::uint64_t publication) noexcept {
        if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            description.Width == 0 || description.Width > INT_MAX ||
            description.Height == 0 || description.Height > INT_MAX ||
            roi_width == 0 || roi_height == 0 ||
            roi_width > description.Width || roi_height > description.Height ||
            description.DepthOrArraySize != 1 || description.SampleDesc.Count != 1 ||
            luke_motion_srv_format(description.Format) == DXGI_FORMAT_UNKNOWN ||
            (description.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
            return E_INVALIDARG;
        }
        if (motion_resource != nullptr &&
            motion_description.Width == roi_width &&
            motion_description.Height == roi_height &&
            motion_description.Format == kLukeAfwMotionFormat &&
            motion_description.Flags == description.Flags) {
            report_stage(AfwStage::motion_resource_created, S_FALSE,
                reinterpret_cast<std::uint64_t>(motion_resource.Get()), publication);
            report_stage(AfwStage::motion_descriptor_ready, S_FALSE,
                static_cast<std::uint64_t>(motion.srv_position), publication);
            return S_OK;
        }
        const auto setup = method<SetupTextureDescFn>(kSetupTextureDescSlot);
        if (setup == nullptr) return E_NOINTERFACE;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        // Composition reads the producer's typed float2 SRV and writes the
        // standard AFW RG16F texture. Preserve compatible producer flags, not
        // its precision; expose only the populated/cropped motion window.
        D3D12_RESOURCE_DESC clean = description;
        clean.Width = roi_width;
        clean.Height = roi_height;
        clean.Format = kLukeAfwMotionFormat;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        const HRESULT result = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &clean,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(resource.GetAddressOf()));
        report_stage(AfwStage::motion_resource_created, result,
            reinterpret_cast<std::uint64_t>(resource.Get()), publication);
        if (FAILED(result)) return result;

        TextureDesc descriptor{};
        descriptor.texture = resource.Get();
        descriptor.initial_state = D3D12_RESOURCE_STATE_COMMON;
        setup(renderer, descriptor);
        const HRESULT device_status = device->GetDeviceRemovedReason();
        const HRESULT descriptor_status = FAILED(device_status)
            ? device_status
            : descriptor.srv_position < 0 ||
                    descriptor.shader_resource_view.ptr == 0
                ? E_FAIL
                : S_OK;
        report_stage(AfwStage::motion_descriptor_ready, descriptor_status,
            static_cast<std::uint64_t>(descriptor.srv_position), publication);
        if (FAILED(descriptor_status)) return descriptor_status;
        motion_resource = std::move(resource);
        descriptor.texture = motion_resource.Get();
        motion = descriptor;
        motion_description = clean;
        return S_OK;
    }

    HRESULT ensure_composed_motion(UINT width, UINT height) noexcept {
        if (width == 0 || height == 0) return E_INVALIDARG;
        if (composed_motion_staging &&
            composed_motion_width == width &&
            composed_motion_height == height) return S_OK;

        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
        support.Format = kLukeAfwMotionFormat;
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                &support, sizeof(support))) ||
            (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0)
            return DXGI_ERROR_UNSUPPORTED;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC texture{};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = width;
        texture.Height = height;
        texture.DepthOrArraySize = 1;
        texture.MipLevels = 1;
        texture.Format = kLukeAfwMotionFormat;
        texture.SampleDesc.Count = 1;
        texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Microsoft::WRL::ComPtr<ID3D12Resource> staging;
        HRESULT result = device->CreateCommittedResource(&heap,
            D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(staging.GetAddressOf()));
        if (FAILED(result)) return result;

        D3D12_DESCRIPTOR_HEAP_DESC descriptor_description{};
        descriptor_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        descriptor_description.NumDescriptors = 5;
        descriptor_description.Flags =
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
        result = device->CreateDescriptorHeap(&descriptor_description,
            IID_PPV_ARGS(descriptors.GetAddressOf()));
        if (FAILED(result)) return result;

        std::array<D3D12_DESCRIPTOR_RANGE,2> ranges{};
        std::array<D3D12_ROOT_PARAMETER,3> parameters{};
        for(UINT index=0;index<2;++index) {
            ranges[index].RangeType=index?D3D12_DESCRIPTOR_RANGE_TYPE_UAV:D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[index].NumDescriptors=index?1:4;
            parameters[index].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[index].DescriptorTable={1,&ranges[index]};
        }
        parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants.ShaderRegister=0;
        parameters[2].Constants.Num32BitValues=60;
        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.NumParameters =
            static_cast<UINT>(parameters.size());
        root_description.pParameters = parameters.data();
        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        result = D3D12SerializeRootSignature(&root_description,
            D3D_ROOT_SIGNATURE_VERSION_1, serialized.GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(result)) return result;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        result = device->CreateRootSignature(queue->GetDesc().NodeMask,
            serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(root.GetAddressOf()));
        if (FAILED(result)) return result;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description{};
        pipeline_description.pRootSignature = root.Get();
        pipeline_description.CS = {g_xrfg_luke_compose_motion_shader,
            sizeof(g_xrfg_luke_compose_motion_shader)};
        pipeline_description.NodeMask = queue->GetDesc().NodeMask;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
        result = device->CreateComputePipelineState(&pipeline_description,
            IID_PPV_ARGS(pipeline.GetAddressOf()));
        if (FAILED(result)) return result;

        const UINT descriptor_size = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            descriptors->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += 4 * descriptor_size;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = kLukeAfwMotionFormat;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(staging.Get(), nullptr, &uav, cpu);
        result = device->GetDeviceRemovedReason();
        if (FAILED(result)) return result;
        composed_motion_staging = std::move(staging);
        composed_motion_descriptors = std::move(descriptors);
        composed_motion_root = std::move(root);
        composed_motion_pipeline = std::move(pipeline);
        composed_motion_descriptor_size = descriptor_size;
        composed_motion_width = width;
        composed_motion_height = height;
        return S_OK;
    }

    HRESULT compose_motion(ID3D12GraphicsCommandList* list,
        const DlssMotionVectorFrame& guide,
        const LukeNativeMotionConstants& constants, UINT crop_x,
        UINT crop_y, UINT width, UINT height) noexcept {
        if (!list || !guide.motion_vectors || !depth.texture ||
            !motion_resource ||
            !composed_motion_staging || !composed_motion_descriptors ||
            !composed_motion_root || !composed_motion_pipeline ||
            crop_x + width > guide.motion_width ||
            crop_y + height > guide.motion_height ||
            width != depth_width || height != depth_height)
            return E_INVALIDARG;

        const bool extend=constants.padding[1]==1.F;
        const auto* middle=extend?guide.previous_opposite_guide.get():nullptr;
        if(extend && (!middle || !middle->motion_vectors || !middle->depth))return E_INVALIDARG;
        for(unsigned index=0;index<4;++index) {
            ID3D12Resource* field=index==0?guide.motion_vectors.Get():index==1?depth.texture:
                index==2?(extend?middle->motion_vectors.Get():guide.motion_vectors.Get()):
                (extend?middle->depth.Get():depth.texture);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format=index==0||index==2?luke_motion_srv_format(field->GetDesc().Format):
                index==3&&extend?luke_depth_format(field->GetDesc().Format).srv:DXGI_FORMAT_R32_FLOAT;
            if(srv.Format==DXGI_FORMAT_UNKNOWN)return DXGI_ERROR_UNSUPPORTED;
            srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels=1;
            auto cpu=composed_motion_descriptors->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr+=index*composed_motion_descriptor_size;
            device->CreateShaderResourceView(field,&srv,cpu);
        }
        if(extend && middle->producer_fence && middle->producer_fence_value &&
            middle->producer_fence->GetCompletedValue()<middle->producer_fence_value) {
            const auto waited=queue->Wait(middle->producer_fence.Get(),middle->producer_fence_value);
            if(FAILED(waited))return waited;
        }
        const HRESULT descriptor_status=device->GetDeviceRemovedReason();
        if(FAILED(descriptor_status)) return descriptor_status;

        const auto transition = [](ID3D12Resource* resource,
            D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            return barrier;
        };
        std::array<D3D12_RESOURCE_BARRIER,2> history_barriers{};
        UINT history_barrier_count{};
        if(extend) {
            if(middle->resource_state!=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                history_barriers[history_barrier_count++]=transition(middle->motion_vectors.Get(),middle->resource_state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if(middle->depth_resource_state!=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                history_barriers[history_barrier_count++]=transition(middle->depth.Get(),middle->depth_resource_state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if(history_barrier_count)list->ResourceBarrier(history_barrier_count,history_barriers.data());
        }
        auto to_uav = transition(composed_motion_staging.Get(),
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &to_uav);
        ID3D12DescriptorHeap* heaps[] =
            {composed_motion_descriptors.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(composed_motion_root.Get());
        list->SetPipelineState(composed_motion_pipeline.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE gpu =
            composed_motion_descriptors->GetGPUDescriptorHandleForHeapStart();
        list->SetComputeRootDescriptorTable(0,gpu);
        gpu.ptr+=4*composed_motion_descriptor_size;
        list->SetComputeRootDescriptorTable(1,gpu);
        list->SetComputeRoot32BitConstants(2,60,&constants,0);
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        for(UINT i=0;i<history_barrier_count;++i)
            std::swap(history_barriers[i].Transition.StateBefore,history_barriers[i].Transition.StateAfter);
        if(history_barrier_count)list->ResourceBarrier(history_barrier_count,history_barriers.data());


        std::array<D3D12_RESOURCE_BARRIER, 3> before_copy{};
        before_copy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        before_copy[0].UAV.pResource = composed_motion_staging.Get();
        before_copy[1] = transition(composed_motion_staging.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        before_copy[2] = transition(motion_resource.Get(),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(static_cast<UINT>(before_copy.size()),
            before_copy.data());
        list->CopyResource(motion_resource.Get(),
            composed_motion_staging.Get());
        std::array<D3D12_RESOURCE_BARRIER, 2> after_copy{
            transition(composed_motion_staging.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COMMON),
            transition(motion_resource.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_COMMON)};
        list->ResourceBarrier(static_cast<UINT>(after_copy.size()),
            after_copy.data());
        return S_OK;
    }
};

LukeAfwSynthesizer::LukeAfwSynthesizer() noexcept = default;
LukeAfwSynthesizer::~LukeAfwSynthesizer() = default;

HRESULT LukeAfwSynthesizer::initialize(ID3D12Device* device,
    ID3D12CommandQueue* queue, const std::filesystem::path& plugin_path) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return initialize_locked(device,queue,plugin_path,true);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT LukeAfwSynthesizer::create_impl(ID3D12Device* device,
    ID3D12CommandQueue* queue, const std::filesystem::path& plugin_path,
    bool isolated_provider,std::unique_ptr<Impl>& output) noexcept {
    try {
        if (device == nullptr || queue == nullptr || plugin_path.empty()) return E_INVALIDARG;
        auto candidate = std::make_unique<Impl>();
        wchar_t executable[32768]{};
        if (!GetModuleFileNameW(nullptr, executable, 32768)) return HRESULT_FROM_WIN32(GetLastError());
        const auto camera_config = luke_game_camera_config_from_ini(
            std::filesystem::path(executable).parent_path() / L"RealVR.ini");
        if (camera_config.world_scale == 0.0F) return E_INVALIDARG;
        candidate->world_scale = camera_config.world_scale;
        candidate->camera_axes = camera_config.axes;
        const auto selected_provider = luke_host_provider(std::filesystem::path(executable).parent_path(), plugin_path);
        candidate->provider_path=selected_provider;
        auto load_path=selected_provider;
        if (isolated_provider) {
            // beta6 has module-global renderer state. InitDevice/InitFrameWarp
            // on that same loaded module cannot reset every lookup state.
            // A private byte-identical module gets fresh globals; only one new
            // context is kept, and the old context has already retired on GPU.
            wchar_t directory[MAX_PATH+1]{},file[MAX_PATH+1]{};
            const auto length=GetTempPathW(MAX_PATH+1,directory);
            if (!length || length>MAX_PATH) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
            if (!GetTempFileNameW(directory,L"OFX",0,file)) return HRESULT_FROM_WIN32(GetLastError());
            candidate->private_provider_file=file; // exact owned file, removed after unload
            if (!CopyFileW(selected_provider.c_str(),file,FALSE)) return HRESULT_FROM_WIN32(GetLastError());
            load_path=file;
        }
        candidate->module = LoadLibraryExW(load_path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (candidate->module == nullptr) return HRESULT_FROM_WIN32(GetLastError());
        candidate->init_device = reinterpret_cast<InitDeviceFn>(
            GetProcAddress(candidate->module, "InitDevice"));
        candidate->init_frame_warp = reinterpret_cast<InitFrameWarpFn>(
            GetProcAddress(candidate->module, "InitFrameWarp"));
        candidate->evaluate = reinterpret_cast<EvaluateFrameWarpFn>(
            GetProcAddress(candidate->module, "EvaluateFrameWarp"));
        if (candidate->init_device == nullptr || candidate->init_frame_warp == nullptr ||
            candidate->evaluate == nullptr) return E_NOINTERFACE;
        const D3D12_COMMAND_QUEUE_DESC queue_description = queue->GetDesc();
        HRESULT result = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(candidate->command_allocator.GetAddressOf()));
        if (FAILED(result)) return result;
        result = device->CreateCommandList(queue_description.NodeMask,
            D3D12_COMMAND_LIST_TYPE_DIRECT, candidate->command_allocator.Get(),
            nullptr, IID_PPV_ARGS(candidate->command_list.GetAddressOf()));
        if (FAILED(result)) return result;
        result = candidate->command_list->Close();
        if (FAILED(result)) return result;
        result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(candidate->submission_fence.GetAddressOf()));
        if (FAILED(result)) return result;
        candidate->submission_event = CreateEventW(
            nullptr, FALSE, FALSE, nullptr);
        if (candidate->submission_event == nullptr)
            return HRESULT_FROM_WIN32(GetLastError());
        candidate->device = device;
        candidate->queue = queue;
        DeviceParams params{};
        candidate->provider_device=std::make_unique<AfwProviderDevice>(device,candidate->provider_allocations);
        params.d3d12_device = candidate->provider_device.get();
        params.d3d12_queue = queue;
        candidate->renderer = candidate->init_device(params);
        if (candidate->renderer == nullptr) return E_FAIL;
        output = std::move(candidate);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT LukeAfwSynthesizer::initialize_locked(ID3D12Device* device,ID3D12CommandQueue* queue,
    const std::filesystem::path& path,bool isolated) noexcept {
    std::unique_ptr<Impl> candidate;
    const auto hr=create_impl(device,queue,path,isolated,candidate);
    if(SUCCEEDED(hr))impl_=std::move(candidate);
    return hr;
}
void LukeAfwSynthesizer::queue_standby_locked() noexcept {
    try {
        if(!impl_||standby_.valid())return;
        if(retirement_.valid()) {
            if(retirement_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
            retirement_.get();
        }
        auto device=impl_->device;auto queue=impl_->queue;const auto path=impl_->provider_path;
        standby_=std::async(std::launch::async,[device,queue,path] {
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
            std::unique_ptr<Impl> candidate;
            if(FAILED(create_impl(device.Get(),queue.Get(),path,true,candidate)))return std::unique_ptr<Impl>{};
            return candidate;
        });
        standby_geometry_=false;
    }catch(...){}
}
void LukeAfwSynthesizer::prepare_history_reset() noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if(reset_prepared_||standby_geometry_||!impl_||!impl_->frame_buffers_ready)return;
        queue_standby_locked();
        if(!standby_.valid()||standby_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
        auto candidate=standby_.get();if(!candidate)return;
        const auto color=impl_->color_description;const auto divisor=impl_->quality_divisor;
        standby_=std::async(std::launch::async,[candidate=std::move(candidate),color,divisor]() mutable {
            FrameWarpInitParams params{};
            candidate->quality_divisor=divisor;
            candidate->work_width=static_cast<UINT>((color.Width+divisor-1)/divisor);
            candidate->work_height=(color.Height+divisor-1)/divisor;
            params.hmd_width=candidate->work_width;params.hmd_height=candidate->work_height;
            params.eye_format=params.backbuffer_format=linear_color_format(color.Format);
            candidate->eye_buffers=candidate->init_frame_warp(params);
            if(!candidate->eye_buffers.eye_frame_buffers[0].color.texture||!candidate->eye_buffers.eye_frame_buffers[1].color.texture)
                return std::unique_ptr<Impl>{};
            if(divisor>1&&FAILED(candidate->color_scale.prepare(candidate->device.Get(),static_cast<UINT>(color.Width),color.Height,params.eye_format)))
                return std::unique_ptr<Impl>{};
            candidate->color_description=color;candidate->frame_buffers_ready=true;
            return std::move(candidate);
        });
        standby_geometry_=true;
    }catch(...){}
}
bool LukeAfwSynthesizer::history_reset_ready() noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if(reset_prepared_)return true;
        if(!standby_geometry_||!standby_.valid()||standby_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return false;
        // Only one retired provider can exist. Never replace a pending future
        // (whose destructor could join on the game's thread).
        if(retirement_.valid()) {
            if(retirement_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return false;
            retirement_.get();
        }
        auto candidate=standby_.get();standby_geometry_=false;
        if(!candidate)return false;
        if(!impl_||!compatible_color(candidate->color_description,impl_->color_description)||
            candidate->quality_divisor!=impl_->quality_divisor) {
            retirement_=std::async(std::launch::async,[unused=std::move(candidate)]() mutable {unused.reset();});
            return false;
        }
        candidate->camera_origin=impl_->camera_origin;
        candidate->right_axis=impl_->right_axis;
        candidate->calibrated_units_per_metre=impl_->calibrated_units_per_metre;
        candidate->unit_epoch=impl_->unit_epoch;candidate->unit_stream=impl_->unit_stream;
        candidate->paired_unit_identity=impl_->paired_unit_identity;
        auto old=std::move(impl_);impl_=std::move(candidate);
        retirement_=std::async(std::launch::async,[old=std::move(old)]() mutable {old.reset();});
        reset_prepared_=true;
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,0,1230,0,0);
        return true;
    }catch(...){return false;}
}

HRESULT LukeAfwSynthesizer::synthesize(ID3D12Resource* source_color,
    ID3D12Resource* destination_color, const DlssMotionVectorFrame& guide,
    const D3D12ReprojectionView& source_view,
    const D3D12ReprojectionView& destination_view,
    std::uint32_t source_eye,
    float xr_eye_baseline,
    const embedded::AfwSettings& settings,bool native_peer_camera) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        report_stage(AfwStage::enter, S_OK, source_eye, guide.publication);
        if (!impl_ || source_color == nullptr || destination_color == nullptr ||
            source_eye > 1 || guide.motion_vectors == nullptr ||
            guide.depth == nullptr ||
            settings.mode < 1 || settings.mode > 3)
            return E_INVALIDARG;
        report_stage(AfwStage::arguments_valid, S_OK, source_eye,
            guide.publication);
        const auto color_desc = source_color->GetDesc();
        const auto destination_desc = destination_color->GetDesc();
        const auto depth_desc = guide.depth->GetDesc();
        const auto motion_desc = guide.motion_vectors->GetDesc();
        const std::uint64_t color_extent =
            (static_cast<std::uint64_t>(color_desc.Width) << 32) |
            color_desc.Height;
        report_stage(AfwStage::descriptions_ready,
            static_cast<std::int64_t>(color_desc.Format), color_extent,
            guide.publication);
        const auto reject_input = [&](std::uint32_t reason) {
            // At most one complete diagnostic per reason/second, so the
            // circular flight log retains the cause. No change
            // to admission, images, synchronization or plugin arguments.
            auto& logger = bridge_flight_logger();
            if (logger.enabled() || logger.ram_enabled()) {
                const auto now = GetTickCount64();
                auto& last = impl_->input_rejection_report_ticks[reason];
                if (last && now - last < 1000) return DXGI_ERROR_UNSUPPORTED;
                last = now;
                logger.event(BridgeFlightOperation::luke_afw_stage,
                    reason, 95, guide.publication, source_eye);
                const auto extent = [](std::uint64_t w, std::uint32_t h) {
                    return (w << 32) | h;
                };
                const D3D12_RESOURCE_DESC descriptors[]{color_desc, destination_desc, depth_desc, motion_desc};
                for (std::uint32_t slot = 0; slot < 4; ++slot) {
                    const auto& d = descriptors[slot];
                    logger.event(BridgeFlightOperation::luke_afw_stage,
                        slot, 96, extent(d.Width, d.Height),
                        static_cast<std::uint64_t>(d.Format) |
                        (static_cast<std::uint64_t>(d.Dimension) << 16) |
                        (static_cast<std::uint64_t>(d.DepthOrArraySize) << 24) |
                        (static_cast<std::uint64_t>(d.SampleDesc.Count) << 40));
                }
                logger.event(BridgeFlightOperation::luke_afw_stage, 0, 97,
                    extent(guide.output_width, guide.output_height), extent(guide.output_x, guide.output_y));
                logger.event(BridgeFlightOperation::luke_afw_stage, 1, 97,
                    extent(guide.motion_width, guide.motion_height), extent(guide.motion_x, guide.motion_y));
                logger.event(BridgeFlightOperation::luke_afw_stage, 2, 97,
                    extent(guide.depth_width, guide.depth_height), extent(guide.depth_x, guide.depth_y));
                const auto floats = [](float x, float y) {
                    std::uint32_t a{}, b{};
                    std::memcpy(&a, &x, sizeof(a)); std::memcpy(&b, &y, sizeof(b));
                    return (static_cast<std::uint64_t>(a) << 32) | b;
                };
                for (std::uint32_t row = 0; row < 8; ++row)
                    logger.event(BridgeFlightOperation::luke_afw_stage, row, 98,
                        floats(guide.camera_view_to_clip[row * 2], guide.camera_view_to_clip[row * 2 + 1]),
                        guide.publication);
                logger.event(BridgeFlightOperation::luke_afw_stage, 0, 99,
                    floats(source_view.fov.angle_left, source_view.fov.angle_right),
                    floats(source_view.fov.angle_up, source_view.fov.angle_down));
            }
            return DXGI_ERROR_UNSUPPORTED;
        };
        if (!compatible_color(color_desc, destination_desc)) return reject_input(1);
        if (color_desc.Width > INT_MAX || color_desc.Height > INT_MAX) return reject_input(2);
        if (!guide.motion_width || !guide.motion_height || !guide.output_width || !guide.output_height)
            return reject_input(3);
        if (luke_motion_srv_format(motion_desc.Format) == DXGI_FORMAT_UNKNOWN) return reject_input(5);
        if (depth_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            motion_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return reject_input(6);
        if (depth_desc.DepthOrArraySize != 1 || motion_desc.DepthOrArraySize != 1) return reject_input(7);
        if (guide.depth_x + guide.depth_width > depth_desc.Width ||
            guide.depth_y + guide.depth_height > depth_desc.Height) return reject_input(8);
        if (guide.motion_x + guide.motion_width > motion_desc.Width ||
            guide.motion_y + guide.motion_height > motion_desc.Height) return reject_input(9);

        // The final OpenXR eye FOV is slightly inset from the apparent edge
        // crop of Luke's square DLSS output. Project that physical FOV through
        // the producer's exact matrix and take the same viewport from depth
        // and motion. At 3344/1672 this is about X30/X345, not X0/X375;
        // the numbers are derived, never stored as calibration offsets.
        LukeGuideWindowFailure depth_failure{}, motion_failure{};
        const auto depth_window = luke_guide_window(
            guide.camera_view_to_clip,
            source_view.fov.angle_left, source_view.fov.angle_right,
            source_view.fov.angle_up, source_view.fov.angle_down,
            guide.output_width, guide.output_height,
            guide.depth_width, guide.depth_height,
            static_cast<UINT>(color_desc.Width), color_desc.Height, &depth_failure);
        const auto motion_window = luke_guide_window(
            guide.camera_view_to_clip,
            source_view.fov.angle_left, source_view.fov.angle_right,
            source_view.fov.angle_up, source_view.fov.angle_down,
            guide.output_width, guide.output_height,
            guide.motion_width, guide.motion_height,
            static_cast<UINT>(color_desc.Width), color_desc.Height);
        if (!depth_window) return reject_input(16 + static_cast<std::uint32_t>(depth_failure));
        if (!motion_window) return reject_input(32 + static_cast<std::uint32_t>(motion_failure));
        const UINT cropped_depth_width = depth_window->width;
        const UINT cropped_depth_height = depth_window->height;
        const UINT cropped_motion_width = motion_window->width;
        const UINT cropped_motion_height = motion_window->height;
        const UINT depth_crop_x = depth_window->x;
        const UINT depth_crop_y = depth_window->y;
        const UINT motion_crop_x = motion_window->x;
        const UINT motion_crop_y = motion_window->y;

        if(cropped_depth_width!=cropped_motion_width || cropped_depth_height!=cropped_motion_height ||
            depth_crop_x!=motion_crop_x || depth_crop_y!=motion_crop_y) return reject_input(42);
        LukeNativeMotionConstants native_motion{};unsigned native_failure{};
        // A current-list camera token identifies the exact native contract.
        // A failed native history must never borrow the producer/XR contract.
        const bool native_camera_contract = guide.camera_frame_token_valid;
        const bool have_native_motion = native_camera_contract &&
            luke_native_motion_constants(guide,motion_crop_x,motion_crop_y,native_motion,native_failure,impl_->camera_axes);
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,native_failure,950,guide.publication,
            guide.previous_same_native_camera?guide.previous_same_native_camera->publication:0);
        if(native_camera_contract && !have_native_motion) {
            const auto now=GetTickCount64();
            auto& last=impl_->input_rejection_report_ticks[63];
            if(!last || now-last>=1000) {
                last=now;
                for(unsigned i=0;i<3;++i) {
                    const auto value=i==0?dlss_native_camera_history(guide):
                        i==1 && guide.previous_opposite_guide?dlss_native_camera_history(*guide.previous_opposite_guide):
                        guide.previous_same_native_camera.value_or(DlssNativeCameraHistory{});
                    std::uint32_t w{};std::memcpy(&w,&value.projection[11],sizeof(w));
                    const std::uint64_t flags=(value.valid?1:0)|(value.reset?2:0)|(value.jittered?4:0)|
                        (static_cast<std::uint64_t>(value.token)<<32);
                    bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,i,953,w,flags);
                    if(guide.native_production_sequence)
                        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,i,958,
                            guide.publication,value.native_production_sequence);
                }
            }
            return HRESULT_FROM_WIN32(ERROR_NOT_READY);
        }

        // The allocator, plugin eye buffers and private depth/motion targets
        // are reused by the next evaluation. V23181 skipped that evaluation
        // whenever the previous GPU submission had not yet retired, visibly
        // exposing the unwarped AER peer. Wait before touching any of them.
        if (impl_->last_submission_fence != 0 &&
            impl_->submission_fence->GetCompletedValue() <
                impl_->last_submission_fence) {
            report_stage(AfwStage::allocator_wait_begin, S_OK,
                impl_->last_submission_fence, guide.publication);
            const HRESULT event_result =
                impl_->submission_fence->SetEventOnCompletion(
                    impl_->last_submission_fence, impl_->submission_event);
            if (FAILED(event_result)) return event_result;
            constexpr DWORD kAllocatorWaitMilliseconds = 250;
            const DWORD wait_result = WaitForSingleObject(
                impl_->submission_event, kAllocatorWaitMilliseconds);
            const HRESULT completion = wait_result == WAIT_OBJECT_0
                ? S_OK
                : wait_result == WAIT_TIMEOUT
                    ? DXGI_ERROR_WAS_STILL_DRAWING
                    : HRESULT_FROM_WIN32(GetLastError());
            report_stage(AfwStage::allocator_wait_end, completion,
                wait_result, guide.publication);
            if (FAILED(completion)) return completion;
        }

        // The existing allocator fence is the sole completion proof. Harvest
        // before a quality change can retire this context; never add a wait.
        impl_->gpu_timing.harvest();
        if (!std::isfinite(xr_eye_baseline) || xr_eye_baseline < .01F || xr_eye_baseline > .20F)
            return E_INVALIDARG;
        const bool interleaved = guide.luke_interleaved_history && guide.previous_opposite_guide &&
            guide.previous_same_native_camera && luke_interleaved_history_matches(
                dlss_native_camera_history(guide), dlss_native_camera_history(*guide.previous_opposite_guide),
                *guide.previous_same_native_camera);
        // A camera source with one world for all its frames (camera_world)
        // keys origin and calibration on that world: its producer may switch
        // between one NGX handle and one per eye without any discontinuity.
        const auto paired_key = guide.camera_world
            ? std::array<std::uint64_t,4>{0, guide.camera_world, 0, 0}
            : interleaved ? luke_interleaved_producer_key(
            dlss_native_camera_history(guide), dlss_native_camera_history(*guide.previous_opposite_guide))
            : std::array<std::uint64_t,4>{};
        if (impl_->paired_unit_identity != paired_key || (!interleaved && !guide.camera_world &&
            (impl_->unit_epoch != guide.epoch||impl_->unit_stream!=guide.stream))) {
            const bool same_domain=guide.camera_world ? impl_->paired_unit_identity==paired_key : interleaved ? impl_->paired_unit_identity[0]==paired_key[0]&&
                impl_->paired_unit_identity[2]==paired_key[2]&&paired_key[0]&&paired_key[2]
                : impl_->unit_stream==guide.stream&&guide.stream;
            const bool keep_units=same_domain&&compatible_color(impl_->color_description,color_desc);
            impl_->unit_epoch = guide.epoch;
            impl_->unit_stream=guide.stream;
            impl_->paired_unit_identity = paired_key;
            if(!keep_units)impl_->calibrated_units_per_metre = 0;
        }
        const auto quality_format=linear_color_format(color_desc.Format);
        if(impl_->checked_quality_format!=quality_format) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};support.Format=quality_format;
            impl_->quality_can_scale=SUCCEEDED(impl_->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support))) &&
                (support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)!=0;
            impl_->checked_quality_format=quality_format;
        }
        // Formats without typed UAV stores retain their original provider route.
        const UINT quality_divisor=settings.mode==3 && impl_->quality_can_scale ? (settings.shading_rate==4 ? 2u : settings.shading_rate==8 ? 4u : 1u) : 1u;
        if ((settings.mode==3 && impl_->active_shading_rate>=0 &&
            settings.shading_rate!=impl_->active_shading_rate) ||
            (impl_->frame_buffers_ready && impl_->quality_divisor!=quality_divisor)) {
            // Previous provider submission is proven complete above. Create
            // isolated renderer state before replacing the active context;
            // a failed replacement leaves the old one intact for recovery.
            const auto provider=impl_->provider_path;
            const auto camera_origin=impl_->camera_origin;
            const auto calibrated_units=impl_->calibrated_units_per_metre;
            const auto saved_right_axis=impl_->right_axis;
            const auto unit_epoch=impl_->unit_epoch;const auto unit_stream=impl_->unit_stream;
            const auto paired_unit_identity=impl_->paired_unit_identity;
            const auto result=initialize_locked(impl_->device.Get(),impl_->queue.Get(),provider,true);
            report_stage(AfwStage::frame_warp_init_begin,result,1044,settings.shading_rate);
            if (FAILED(result)) return result;
            impl_->camera_origin=camera_origin;
            impl_->calibrated_units_per_metre=calibrated_units;
            impl_->right_axis=saved_right_axis;
            impl_->unit_epoch=unit_epoch;impl_->unit_stream=unit_stream;
            impl_->paired_unit_identity=paired_unit_identity;
        }
        bool gta_calibration_changed=false;
        if(std::isfinite(guide.calibrated_game_eye_baseline) &&
            guide.calibrated_game_eye_baseline>0) {
            const float updated=guide.calibrated_game_eye_baseline*impl_->world_scale/xr_eye_baseline;
            gta_calibration_changed=impl_->calibrated_units_per_metre>0 &&
                std::abs(updated-impl_->calibrated_units_per_metre)>impl_->calibrated_units_per_metre*.03F;
            if(impl_->calibrated_units_per_metre==0||gta_calibration_changed)
                impl_->calibrated_units_per_metre=updated;
        } else if (impl_->calibrated_units_per_metre == 0 && guide.calibrated_game_eye_baseline > 0)
            impl_->calibrated_units_per_metre = guide.calibrated_game_eye_baseline * impl_->world_scale / xr_eye_baseline;
        const float units = impl_->calibrated_units_per_metre;
        float game_baseline{};
        const bool units_ready = luke_game_eye_baseline(xr_eye_baseline, impl_->world_scale, units, game_baseline);
        std::uint32_t baseline_bits_units{}, units_bits{};
        std::memcpy(&baseline_bits_units, &game_baseline, sizeof(baseline_bits_units));
        std::memcpy(&units_bits, &units, sizeof(units_bits));
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
            units_ready ? S_OK : S_FALSE, 104, guide.publication,
            (std::uint64_t(baseline_bits_units) << 32) | units_bits);
        if (!units_ready) return HRESULT_FROM_WIN32(ERROR_NOT_READY);
        if (const auto& other = guide.previous_opposite_guide;
            other && other->camera_data_valid && guide.camera_data_valid && !guide.reset) {
            const float before = impl_->right_axis.sign();
            const float after = impl_->right_axis.observe(luke_measured_right_axis(source_eye,
                guide.camera_position, other->camera_position, guide.camera_right, game_baseline));
            // 105: the eye direction along camera_right was adopted or changed;
            // b = the projection's x sign it replaces (1/-1), c = publication.
            if (after != before)
                bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
                    static_cast<std::int64_t>(after), 105,
                    guide.camera_view_to_clip[0] < 0 ? std::uint64_t(-1) : 1, guide.publication);
        }
        const float right_axis = impl_->right_axis.sign();
        const auto origin_changed = interleaved || guide.camera_world
            ? impl_->camera_origin.update_producers(paired_key,guide.camera_position,guide.reset)
            : impl_->camera_origin.update(guide.stream, guide.epoch, guide.camera_position, guide.reset);
        if (!origin_changed) return E_INVALIDARG;
        // InitFrameWarp on a warmed beta6 renderer does not retire all reduced-
        // rate lookup resources. Reusing it after a producer reset can leave
        // m_otherEyeLookupDesc in shader-read state when the next Combined
        // dispatch clears it as a render target. The existing fence above has
        // already retired the old provider; use a fresh isolated renderer at
        // this lifetime/geometry boundary, exactly as for a quality change.
        const bool provider_geometry_changed=impl_->frame_buffers_ready &&
            !compatible_color(impl_->color_description,color_desc);
        // A backend switch may skip an AFW color without changing the producer
        // epoch or geometry. Combined must then fall back to Alternate. Beta6
        // changes to full-rate work there but retains its reduced lookup state;
        // re-entering Combined otherwise clears a shader-read lookup as an RTV.
        // Retire that warm state at the discontinuity, after the existing fence.
        // Missing same-eye history during the second prime is not another
        // discontinuity: recreating here would discard the first primed eye.
        const bool reduced_continuity_lost=settings.mode==3 &&
            impl_->active_shading_rate>0 && impl_->last_evaluated_publication &&
            !luke_afw_previous_opposite(guide.publication,
                impl_->last_evaluated_publication,source_eye,impl_->last_evaluated_eye);
        if(impl_->frame_buffers_ready && (provider_geometry_changed ||
            (!impl_->clean_frame_buffers&&(*origin_changed || reduced_continuity_lost || gta_calibration_changed)))) {
            const auto provider=impl_->provider_path;
            const auto camera_origin=impl_->camera_origin;
            const auto calibrated_units=impl_->calibrated_units_per_metre;
            const auto saved_right_axis=impl_->right_axis;
            const auto unit_epoch=impl_->unit_epoch;const auto unit_stream=impl_->unit_stream;
            const auto paired_unit_identity=impl_->paired_unit_identity;
            const auto result=initialize_locked(impl_->device.Get(),impl_->queue.Get(),provider,true);
            report_stage(AfwStage::frame_warp_init_begin,result,
                *origin_changed ? 1045 : gta_calibration_changed ? 1203 : provider_geometry_changed ? 1046 : 1047,
                guide.publication);
            if(FAILED(result))return result;
            impl_->camera_origin=camera_origin;
            impl_->calibrated_units_per_metre=calibrated_units;
            impl_->right_axis=saved_right_axis;
            impl_->unit_epoch=unit_epoch;impl_->unit_stream=unit_stream;
            impl_->paired_unit_identity=paired_unit_identity;
        }
        if (*origin_changed) {
            // AFW's private history and ours must never span two origins.
            // Reinitialize both eyes only at a producer lifetime/reset boundary.
            if(!impl_->clean_frame_buffers)impl_->frame_buffers_ready = false;
            impl_->previous_source_publication = {};
            impl_->previous_source_camera = {};
            impl_->last_evaluated_publication = 0;
            impl_->last_evaluated_eye = 2;
            impl_->depth_calibration_reset_pending = true;
        }
        CameraData camera{};
        if (!camera_data(source_view, destination_view, guide, source_eye,
                         impl_->world_scale, xr_eye_baseline, units, impl_->camera_origin,
                         impl_->camera_axes, camera,native_peer_camera,native_peer_camera?0.F:right_axis)) return E_INVALIDARG;
        if(native_peer_camera) {
            const unsigned peer_eye=1-source_eye;
            bool corrected=false;
            auto cached_peer_head=impl_->previous_source_xr_orientation[peer_eye];
            auto submitted_peer_head=destination_view.pose.orientation;
            const bool same_peer_reference=stereo_pose_detail::unit(cached_peer_head) &&
                stereo_pose_detail::unit(submitted_peer_head) &&
                std::abs(cached_peer_head.x*submitted_peer_head.x+cached_peer_head.y*submitted_peer_head.y+
                    cached_peer_head.z*submitted_peer_head.z+cached_peer_head.w*submitted_peer_head.w)>1.F-1e-5F;
            if(!guide.reset && same_peer_reference && impl_->previous_source_headlocked[source_eye] &&
                impl_->previous_source_headlocked[peer_eye] &&
                impl_->previous_source_stream[source_eye]==guide.stream &&
                impl_->previous_source_stream[peer_eye]==guide.stream &&
                impl_->previous_source_epoch[source_eye]==guide.epoch &&
                impl_->previous_source_epoch[peer_eye]==guide.epoch &&
                impl_->previous_source_publication[source_eye]+2==guide.publication &&
                impl_->previous_source_publication[peer_eye]+1==guide.publication &&
                guide.previous_opposite_guide && guide.previous_same_guide &&
                guide.previous_same_guide->publication==impl_->previous_source_publication[source_eye] &&
                guide.previous_opposite_guide->publication==impl_->previous_source_publication[peer_eye]) {
                const auto input=[](const Matrix4x4& matrix,Quaternion head,unsigned eye) {
                    LukeGtaEyeCamera result;std::copy_n(matrix.values,16,result.view_to_world.begin());
                    result.xr_orientation=head;result.eye=eye;return result;
                };
                float baseline{};
                const float current_ms=guide.frame_time_delta_ms;
                const float peer_ms=guide.previous_opposite_guide->frame_time_delta_ms;
                if(std::isfinite(current_ms)&&std::isfinite(peer_ms)&&current_ms>0&&peer_ms>0&&
                    luke_game_eye_baseline(xr_eye_baseline,impl_->world_scale,units,baseline)) {
                    const auto target=luke_gta_peer_eye_reference(
                        input(camera.source_view_to_world,source_view.pose.orientation,source_eye),
                        input(impl_->previous_source_camera[source_eye].source_view_to_world,
                            impl_->previous_source_xr_orientation[source_eye],source_eye),
                        input(impl_->previous_source_camera[peer_eye].source_view_to_world,
                            impl_->previous_source_xr_orientation[peer_eye],peer_eye),
                        baseline,current_ms/(current_ms+peer_ms));
                    if(target) {
                        Matrix4x4 destination;std::copy(target->begin(),target->end(),destination.values);
                        Matrix4x4 inverse_destination;
                        if(invert(destination,inverse_destination)) {
                            camera.destination_view_to_world=destination;
                            camera.destination_world_to_view=inverse_destination;corrected=true;
                        }
                    }
                }
            }
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
                corrected,1202,guide.publication,impl_->previous_source_publication[source_eye]);
        }
        std::uint32_t baseline_bits{};
        std::memcpy(&baseline_bits, &xr_eye_baseline, sizeof(baseline_bits));
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
            source_eye, 88, guide.publication, baseline_bits);
        if (bridge_flight_logger().ram_enabled()) {
            const auto pair = [](float x, float y) noexcept {
                std::uint32_t a{}, b{};
                std::memcpy(&a, &x, sizeof(a)); std::memcpy(&b, &y, sizeof(b));
                return (static_cast<std::uint64_t>(a) << 32) | b;
            };
            const Matrix4x4* matrices[]{&camera.source_view_to_world,
                &camera.destination_view_to_world, &camera.source_view_to_clip,
                &camera.destination_view_to_clip};
            for (int matrix = 0; matrix < 4; ++matrix)
                for (int row = 0; row < 4; ++row) {
                    const auto* v = matrices[matrix]->values + row * 4;
                    bridge_flight_logger().event(BridgeFlightOperation::luke_render_camera,
                        source_eye * 100 + matrix * 10 + row, guide.publication,
                        pair(v[0], v[1]), pair(v[2], v[3]));
                }
            bridge_flight_logger().event(BridgeFlightOperation::luke_render_camera,
                source_eye * 100 + 40, guide.publication,
                pair(impl_->world_scale, guide.camera_fov_vertical), 0);
        }
        if (guide.reset) impl_->previous_source_publication = {};
        report_stage(AfwStage::camera_ready, S_OK, source_eye,
            guide.publication);

        if (!impl_->frame_buffers_ready ||
            !compatible_color(impl_->color_description, color_desc)) {
            FrameWarpInitParams params{};
            impl_->quality_divisor=quality_divisor;
            impl_->work_width=static_cast<UINT>((color_desc.Width+quality_divisor-1)/quality_divisor);
            impl_->work_height=(color_desc.Height+quality_divisor-1)/quality_divisor;
            params.hmd_width = static_cast<int>(impl_->work_width);
            params.hmd_height = static_cast<int>(impl_->work_height);
            // PureDark allocates typed UAV-capable internal eye textures.
            // Horizon exposes the backing resource as RGBA8 typeless (DXGI 27)
            // and its OpenXR view as sRGB. Use the copy-compatible typed linear
            // sibling only inside AFW; application resources and presentation
            // remain unchanged.
            params.eye_format = linear_color_format(color_desc.Format);
            params.backbuffer_format = params.eye_format;
            report_stage(AfwStage::frame_warp_init_begin,
                static_cast<std::int64_t>(params.eye_format), color_extent,
                guide.publication);
            impl_->eye_buffers = impl_->init_frame_warp(params);
            report_stage(AfwStage::frame_warp_init_end, S_OK, source_eye,
                guide.publication);
            if (impl_->eye_buffers.eye_frame_buffers[0].color.texture == nullptr ||
                impl_->eye_buffers.eye_frame_buffers[1].color.texture == nullptr)
                return E_FAIL;
            if(quality_divisor>1) {
                const auto prepared=impl_->color_scale.prepare(impl_->device.Get(),static_cast<UINT>(color_desc.Width),color_desc.Height,params.eye_format);
                if(FAILED(prepared))return prepared;
            }
            impl_->color_description = color_desc;
            impl_->frame_buffers_ready = true;
            impl_->previous_source_publication = {};
            impl_->last_evaluated_publication = 0;
            impl_->last_evaluated_eye = 2;
        }
        if (impl_->depth.texture == nullptr ||
            impl_->depth_width != cropped_depth_width ||
            impl_->depth_height != cropped_depth_height ||
            impl_->depth_source_format != depth_desc.Format ||
            impl_->depth_source_width != depth_desc.Width ||
            impl_->depth_source_height != depth_desc.Height) {
            report_stage(AfwStage::depth_create_begin,
                static_cast<std::int64_t>(depth_desc.Format), source_eye,
                guide.publication);
            HRESULT result = impl_->ensure_float_depth(
                cropped_depth_width, cropped_depth_height, depth_desc);
            const HRESULT device_status = impl_->device->GetDeviceRemovedReason();
            if (SUCCEEDED(result) && FAILED(device_status)) result = device_status;
            report_stage(AfwStage::depth_create_end, result, source_eye,
                guide.publication);
            if (FAILED(result)) return result;
        }
        // PureDark's CreateTexture helper removes Horizon's wrapped D3D12
        // device for RG16F, while binding the producer snapshot directly is
        // rejected inside EvaluateFrameWarp. Allocate an exact private clone
        // of the working producer contract, then register it with PureDark.
        report_stage(AfwStage::motion_allocate_begin,
            static_cast<std::int64_t>(motion_desc.Format),
            reinterpret_cast<std::uint64_t>(guide.motion_vectors.Get()),
            guide.publication);
        HRESULT result = impl_->ensure_motion(motion_desc,
            cropped_motion_width, cropped_motion_height, guide.publication);
        if (FAILED(result)) return result;
        result = impl_->ensure_composed_motion(cropped_motion_width,
            cropped_motion_height);
        if (FAILED(result)) return result;
        report_stage(AfwStage::resources_ready, S_OK, source_eye,
            guide.publication);
        if(guide.producer_fence && guide.producer_fence_value) {
            const HRESULT waited=impl_->queue->Wait(guide.producer_fence.Get(),guide.producer_fence_value);
            if(FAILED(waited)) return waited;
        }
        report_stage(AfwStage::producer_waited, S_OK, source_eye,
            guide.publication);
        const auto copy = impl_->method<CopyTextureFn>(kCopyTextureSlot);
        if (copy == nullptr) return E_NOINTERFACE;
        result = impl_->command_allocator->Reset();
        if (FAILED(result)) {
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage, result, 100, source_eye, guide.publication);
            return result;
        }
        result = impl_->command_list->Reset(
            impl_->command_allocator.Get(), nullptr);
        if (FAILED(result)) {
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage, result, 101, source_eye, guide.publication);
            return result;
        }
        const auto initial_depth_state = impl_->depth_staging_state;
        const auto initial_calibration_contracts = impl_->depth_calibration_contracts;
        const bool initial_calibration_reset = impl_->depth_calibration_reset_pending;
        LukeRecordingScope recording(impl_->command_list.Get(), [&](HRESULT aborted_close) noexcept {
            impl_->depth_staging_state = initial_depth_state;
            impl_->depth_calibration_contracts = initial_calibration_contracts;
            impl_->depth_calibration_reset_pending = initial_calibration_reset;
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage, result, 102, source_eye, guide.publication);
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage, aborted_close, 103, source_eye, guide.publication);
        });
        ID3D12GraphicsCommandList* command_list = impl_->command_list.Get();
        if (!impl_->gpu_timing_initialized &&
            (bridge_flight_logger().enabled() || bridge_flight_logger().ram_enabled())) {
            impl_->gpu_timing.initialize(impl_->device.Get(),impl_->queue.Get(),
                BridgeFlightOperation::gpu_afw,bridge_flight_logger().enabled());
            impl_->gpu_timing_initialized=true;
            LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
                S_OK,1102,frequency.QuadPart,reinterpret_cast<std::uintptr_t>(impl_.get()));
        }
        impl_->gpu_timing.mark(command_list,0);
        report_stage(AfwStage::command_list_reset, S_OK, source_eye,
            guide.publication);
        if (two_frame_capture_active()) {
            // Interleaved native output is a reused public SR allocation,
            // not this retained production's colour. Keep the exact XR source
            // and completion-stage colour; reserve F8 budget for pre-dispatch
            // motion instead of two redundant full-size mutable SR images.
            if (guide.output != nullptr && !interleaved) {
                TwoFrameImageInfo output{};
                output.stage = "afw_ngx_output_at_consume";
                output.stream = guide.stream;
                output.capture_serial = guide.publication;
                output.eye = static_cast<std::int32_t>(source_eye);
                output.x = guide.output_x;
                output.y = guide.output_y;
                output.width = guide.output_width;
                output.height = guide.output_height;
                two_frame_capture_record_ng_x(output, impl_->device.Get(),
                    command_list, guide.output.Get(),
                    D3D12_RESOURCE_STATE_COMMON);
            }
            TwoFrameImageInfo mv{};
            mv.stage = "afw_ngx_mv_before_crop";
            mv.stream = guide.stream;
            mv.capture_serial = guide.publication;
            mv.eye = static_cast<std::int32_t>(source_eye);
            mv.x = guide.motion_x;
            mv.y = guide.motion_y;
            mv.width = guide.motion_width;
            mv.height = guide.motion_height;
            mv.scale_x = guide.scale_x;
            mv.scale_y = guide.scale_y;
            mv.jitter_x = guide.jitter_x;
            mv.jitter_y = guide.jitter_y;
            two_frame_capture_record_ng_x(mv, impl_->device.Get(),
                command_list, guide.motion_vectors.Get(), guide.resource_state);
            TwoFrameImageInfo native_depth{};
            native_depth.stage = "afw_ngx_depth_before_crop";
            native_depth.stream = guide.stream;
            native_depth.capture_serial = guide.publication;
            native_depth.eye = static_cast<std::int32_t>(source_eye);
            native_depth.x = guide.depth_x;
            native_depth.y = guide.depth_y;
            native_depth.width = guide.depth_width;
            native_depth.height = guide.depth_height;
            two_frame_capture_record_ng_x(native_depth, impl_->device.Get(),
                command_list, guide.depth.Get(), guide.resource_state);
            TwoFrameImageInfo color{};
            color.stage = "afw_source_color";
            color.stream = guide.stream;
            color.capture_serial = guide.publication;
            color.eye = static_cast<std::int32_t>(source_eye);
            color.width = static_cast<std::uint32_t>(color_desc.Width);
            color.height = color_desc.Height;
            two_frame_capture_record_ng_x(color, impl_->device.Get(),
                command_list, source_color, D3D12_RESOURCE_STATE_COMMON);
        }

        FrameBufferDesc input = impl_->eye_buffers.eye_frame_buffers[source_eye];
        if (two_frame_capture_active()) {
            const auto capture_prior_eye = [&](std::uint32_t eye,
                const char* stage) {
                const TextureDesc& prior =
                    impl_->eye_buffers.eye_frame_buffers[eye].color;
                if (prior.texture == nullptr) return;
                const auto extent = prior.texture->GetDesc();
                TwoFrameImageInfo image{};
                image.stage = stage;
                image.stream = guide.stream;
                image.capture_serial = guide.publication;
                image.eye = static_cast<std::int32_t>(eye);
                image.width = static_cast<std::uint32_t>(extent.Width);
                image.height = extent.Height;
                two_frame_capture_record_ng_x(image, impl_->device.Get(),
                    command_list, prior.texture, prior.initial_state);
            };
            capture_prior_eye(source_eye, "afw_prior_source_eye_color");
            capture_prior_eye(1U - source_eye, "afw_prior_peer_eye_color");
        }
        TextureDesc color_source{};
        color_source.texture = source_color;
        color_source.initial_state = D3D12_RESOURCE_STATE_COMMON;
        D3D12_BOX color_box{0, 0, 0, static_cast<UINT>(color_desc.Width),
            color_desc.Height, 1};
        if(quality_divisor>1)
            impl_->color_scale.record(impl_->device.Get(),command_list,source_color,D3D12_RESOURCE_STATE_COMMON,
                static_cast<UINT>(color_desc.Width),color_desc.Height,input.color.texture,input.color.initial_state,impl_->work_width,impl_->work_height,0);
        else copy(impl_->renderer, command_list, input.color, color_source,
            color_box, 0, 0);
        report_stage(AfwStage::color_copied, S_OK, source_eye,
            guide.publication);

        Matrix4x4 middle_same_transform{};
        Matrix4x4 previous_same_transform{};
        Matrix4x4 audit_middle_opposite_transform{};
        Matrix4x4 native_previous_same_transform{};
        identity(middle_same_transform);
        identity(previous_same_transform);
        identity(audit_middle_opposite_transform);
        identity(native_previous_same_transform);
        bool temporal_camera_valid = false;
        bool native_depth_camera_valid = false;
        CameraData native_previous_camera{};
        const auto& middle = guide.previous_opposite_guide;
        const auto& native_previous = guide.previous_same_guide;
        const bool have_previous_camera =
            impl_->previous_source_publication[source_eye] != 0 &&
            impl_->previous_source_publication[source_eye] < guide.publication &&
            impl_->previous_source_epoch[source_eye] == guide.epoch &&
            impl_->previous_source_stream[source_eye] == guide.stream;
        if (middle &&
            (interleaved || (middle->stream == guide.stream && middle->epoch == guide.epoch)) &&
            middle->publication + 1 == guide.publication) {
            CameraData middle_camera{};
            // The middle guide is an NGX camera at its own publication. Its
            // virtual same-time peer needs stereo translation, not the XR
            // rotation between today's submitted eyes: the peer pose can be
            // several AER publications older than this middle guide.
            D3D12ReprojectionView middle_peer_view = source_view;
            middle_peer_view.pose.orientation =
                destination_view.pose.orientation;
            if (camera_data(destination_view, middle_peer_view, *middle,
                    1U - source_eye, impl_->world_scale, xr_eye_baseline, units, impl_->camera_origin,
                    impl_->camera_axes, middle_camera, false, right_axis)) {
                const Matrix4x4 current_clip_to_world = multiply(
                    camera.source_clip_to_view, camera.source_view_to_world);
                audit_middle_opposite_transform = multiply(multiply(
                    current_clip_to_world, middle_camera.source_world_to_view),
                    middle_camera.source_view_to_clip);
                if (have_previous_camera) {
                    middle_same_transform = multiply(multiply(
                        current_clip_to_world,
                        middle_camera.destination_world_to_view),
                        middle_camera.destination_view_to_clip);
                    const CameraData& previous =
                        impl_->previous_source_camera[source_eye];
                    previous_same_transform = multiply(multiply(
                        current_clip_to_world, previous.source_world_to_view),
                        previous.source_view_to_clip);
                    temporal_camera_valid = true;
                }
                // Native motion N-1 points to producer N-2, not the last
                // successful AFW input (which may be N-4 after a skipped frame).
                if (native_previous && native_previous->stream == guide.stream &&
                    native_previous->epoch == guide.epoch &&
                    native_previous->serial + (interleaved ? 1 : 2) == guide.serial &&
                    native_previous->publication + 2 == guide.publication &&
                    camera_data(source_view, source_view, *native_previous,
                        source_eye, impl_->world_scale, xr_eye_baseline, units,
                        impl_->camera_origin, impl_->camera_axes, native_previous_camera, false, right_axis)) {
                    native_previous_same_transform = multiply(multiply(
                        current_clip_to_world, native_previous_camera.source_world_to_view),
                        native_previous_camera.source_view_to_clip);
                    native_depth_camera_valid = true;
                    if (!have_previous_camera) {
                        // Bootstrap only from the immutable producer N-2;
                        // identity is never a missing-history motion substitute.
                        previous_same_transform = native_previous_same_transform;
                        middle_same_transform = multiply(multiply(
                            current_clip_to_world,
                            middle_camera.destination_world_to_view),
                            middle_camera.destination_view_to_clip);
                        temporal_camera_valid = true;
                    }
                }
            }
        }
        report_stage(AfwStage::temporal_camera_ready,
            temporal_camera_valid ? S_OK : S_FALSE,
            impl_->previous_source_publication[source_eye],
            guide.publication);
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
            source_eye, 80, guide.publication, impl_->previous_source_publication[source_eye]);
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
            temporal_camera_valid ? 1 : 0, 81, guide.publication,
            middle ? middle->publication : 0);
        const auto middle_motion_window = middle ? luke_guide_window(
            middle->camera_view_to_clip,
            destination_view.fov.angle_left, destination_view.fov.angle_right,
            destination_view.fov.angle_up, destination_view.fov.angle_down,
            middle->output_width, middle->output_height,
            middle->motion_width, middle->motion_height,
            static_cast<UINT>(color_desc.Width), color_desc.Height)
            : std::optional<LukeGuideWindow>{};
        const bool contiguous_depth_history = native_depth_camera_valid && middle_motion_window &&
            middle_motion_window->width == cropped_motion_width &&
            middle_motion_window->height == cropped_motion_height &&
            middle->output_width == guide.output_width &&
            middle->output_height == guide.output_height &&
            native_previous->output_width == guide.output_width &&
            native_previous->output_height == guide.output_height &&
            std::equal(std::begin(guide.camera_view_to_clip), std::end(guide.camera_view_to_clip),
                std::begin(middle->camera_view_to_clip)) &&
            std::equal(std::begin(guide.camera_view_to_clip), std::end(guide.camera_view_to_clip),
                std::begin(native_previous->camera_view_to_clip)) &&
            std::equal(std::begin(camera.source_view_to_clip.values), std::end(camera.source_view_to_clip.values),
                std::begin(native_previous_camera.source_view_to_clip.values));
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
            contiguous_depth_history ? 1 : 0, 82, guide.publication,
            native_previous ? native_previous->publication : 0);
        const auto to_window = native_motion_detail::guide_to_window(
            guide.motion_width, guide.motion_height, motion_crop_x, motion_crop_y,
            cropped_motion_width, cropped_motion_height);
        if (native_camera_contract) {
            // Calibration operates in the physical crop; use the same exact
            // native cameras as motion, transformed into that coordinate domain.
            const auto opposite_window = native_motion_detail::guide_to_window_transform(
                native_motion.opposite, to_window);
            const auto same_window = native_motion_detail::guide_to_window_transform(
                native_motion.same, to_window);
            std::copy(opposite_window.begin(), opposite_window.end(), audit_middle_opposite_transform.values);
            std::copy(same_window.begin(), same_window.end(), native_previous_same_transform.values);
        } else {
            if (!temporal_camera_valid) return HRESULT_FROM_WIN32(ERROR_NOT_READY);
            native_motion.source_base = {guide.motion_x, guide.motion_y};
            native_motion.crop = {motion_crop_x, motion_crop_y};
            native_motion.extent = {float(guide.motion_width), float(guide.motion_height)};
            native_motion.scale = {guide.scale_x, guide.scale_y};
            const auto as_array = [](const Matrix4x4& m) {
                std::array<float,16> values{};
                std::copy(std::begin(m.values),std::end(m.values),values.begin());
                return values;
            };
            const Matrix4x4 stereo_transform = multiply(multiply(multiply(
                camera.source_clip_to_view, camera.source_view_to_world),
                camera.destination_world_to_view), camera.destination_view_to_clip);
            native_motion.opposite = native_motion_detail::window_to_guide_transform(
                as_array(middle_same_transform), to_window);
            native_motion.same = native_motion_detail::window_to_guide_transform(
                as_array(previous_same_transform), to_window);
            native_motion.stereo = native_motion_detail::window_to_guide_transform(
                as_array(stereo_transform), to_window);
        }
        result = impl_->convert_depth(command_list, guide.depth.Get(),
            guide.depth_resource_state,
            guide.depth_x + depth_crop_x, guide.depth_y + depth_crop_y,
            guide.depth_inverted, settings.depth_dilate_mode, guide,
            audit_middle_opposite_transform, native_previous_same_transform,
            motion_crop_x, motion_crop_y,
            middle_motion_window ? middle_motion_window->x : motion_crop_x,
            middle_motion_window ? middle_motion_window->y : motion_crop_y,
            contiguous_depth_history, source_eye);
        report_stage(AfwStage::depth_converted, result, source_eye,
            guide.publication);
        if (FAILED(result)) return result;
        // One depth-aware correction for both measured camera contracts:
        // remove native stereo/camera and reconstruct the missing same-eye span.
        result = impl_->compose_motion(command_list, guide, native_motion,
            motion_crop_x, motion_crop_y, cropped_motion_width,
            cropped_motion_height);
        report_stage(AfwStage::motion_copied, result, source_eye,
            guide.publication);
        if (FAILED(result)) return result;
        if(native_motion.padding[1]==1.F) {
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,1,1198,
                guide.publication,guide.previous_opposite_guide->publication);
            if(two_frame_capture_active())two_frame_capture_timeline("native_object_history",1,
                guide.stream,guide.publication,guide.previous_opposite_guide->publication);
        }

        input.depth = impl_->depth;
        input.motion_vectors = impl_->motion;
        if (two_frame_capture_active()) {
            const auto capture_plugin_input = [&](const TextureDesc& texture,
                const char* stage) {
                if (texture.texture == nullptr) return;
                const auto extent = texture.texture->GetDesc();
                TwoFrameImageInfo image{};
                image.stage = stage;
                image.stream = guide.stream;
                image.capture_serial = guide.publication;
                image.eye = static_cast<std::int32_t>(source_eye);
                image.width = static_cast<std::uint32_t>(extent.Width);
                image.height = extent.Height;
                two_frame_capture_record_ng_x(image, impl_->device.Get(),
                    command_list, texture.texture, texture.initial_state);
            };
            capture_plugin_input(input.color, "afw_plugin_input_color");
            capture_plugin_input(input.depth, "afw_plugin_input_depth");
            capture_plugin_input(input.motion_vectors, "afw_plugin_input_mv");
            TextureDesc calibration{};
            calibration.texture = impl_->depth_remap_state.Get();
            calibration.initial_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            capture_plugin_input(calibration, "afw_depth_remap_calibration");
        }

        // The provider gets no depth convention, only the depth and these
        // matrices. Every other REAL VR title here renders reversed-Z; The
        // Last of Us Part I does not (DLSS flags 67, no DepthInverted), and
        // its AFW output parallaxes wrongly while the layer's own fit of its
        // depth against stereo disparity is exact. So the provider receives
        // its depth reversed (convert_depth, bit 8) with the projection that
        // produces that reversed depth: z' = w - z, i.e. rows [10] = 1 - [10],
        // [14] = -[14] for a w = z projection. The layer's own transforms
        // keep the native convention.
        CameraData provider_camera = camera;
        if (!guide.depth_inverted) {
            const auto reverse = [](Matrix4x4& view_to_clip, Matrix4x4& clip_to_view) {
                view_to_clip.values[10] = view_to_clip.values[11] - view_to_clip.values[10];
                view_to_clip.values[14] = view_to_clip.values[15] - view_to_clip.values[14];
                view_to_clip.values[2] = view_to_clip.values[3] - view_to_clip.values[2];
                view_to_clip.values[6] = view_to_clip.values[7] - view_to_clip.values[6];
                return invert(view_to_clip, clip_to_view);
            };
            if (!reverse(provider_camera.source_view_to_clip, provider_camera.source_clip_to_view) ||
                !reverse(provider_camera.destination_view_to_clip, provider_camera.destination_clip_to_view))
                return E_INVALIDARG;
        }
        FrameWarpEvaluateParams params{};
        params.command_list = command_list;
        params.input_eye_frame_buffer = &input;
        params.motion_scale[0] = luke_guide_motion_gain(guide.scale_x,
            guide.output_width, guide.motion_width, static_cast<UINT>(color_desc.Width),
            cropped_motion_width, motion_window->raster_x);
        params.motion_scale[1] = luke_guide_motion_gain(guide.scale_y,
            guide.output_height, guide.motion_height, color_desc.Height,
            cropped_motion_height, motion_window->raster_y);
        const int effective_mode = luke_afw_effective_mode(settings.mode,
            guide.publication, impl_->last_evaluated_publication,
            source_eye, impl_->last_evaluated_eye,
            impl_->previous_source_publication[source_eye], settings.clear_before_warping);
        params.mode = static_cast<FrameWarpMode>(effective_mode);
        if(impl_->active_shading_rate<0)impl_->active_shading_rate=settings.shading_rate;
        // Reduced qualities use an actual colour raster. Keep the provider
        // resolve at 1x1 rather than its block-phase colour lookup. Native
        // depth/Normal correction above remains in its accepted guide domain.
        params.shading_rate = embedded::afw_shading_rate(
            quality_divisor>1 ? 0 : (effective_mode == 3 ? impl_->active_shading_rate : 0));
        params.motion_scale[0] *= float(impl_->work_width)/float(color_desc.Width);
        params.motion_scale[1] *= float(impl_->work_height)/float(color_desc.Height);
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
            effective_mode, 952, guide.publication, impl_->last_evaluated_publication);
        const std::uint32_t interpreted_eye = source_eye;
        params.eye_index = source_eye == 0 ? EyeLeft : EyeRight;
        params.camera_data = &provider_camera;
        params.clear_before_warping = settings.clear_before_warping;
        params.ignore_motion_threshold = settings.ignore_motion_threshold;
        params.is_hudless_color = true;
        params.motion_vectors_type = Normal;
        params.debug = settings.debug;
        if (two_frame_capture_active()) {
            TwoFrameGuideContract contract{};
            contract.stage = "afw_evaluate";
            contract.native_camera_token = guide.camera_frame_token;
            contract.color_origin_token = guide.color_origin_token;
            contract.native_production_sequence = guide.native_production_sequence;
            contract.interleaved_history = guide.luke_interleaved_history;
            contract.stream = guide.stream;
            contract.publication = guide.publication;
            contract.serial = guide.serial;
            contract.previous_serial = guide.previous_serial;
            contract.source_eye = static_cast<std::int32_t>(source_eye);
            contract.destination_eye = static_cast<std::int32_t>(1U - source_eye);
            contract.mode = effective_mode;
            contract.motion_vectors_type = params.motion_vectors_type;
            contract.ignore_motion_threshold =
                params.ignore_motion_threshold;
            contract.camera_fov_vertical = guide.camera_fov_vertical;
            contract.debug = params.debug;
            contract.depth_dilate_mode = settings.depth_dilate_mode;
            contract.clear_before_warping =
                params.clear_before_warping;
            contract.is_hudless_color = params.is_hudless_color;
            contract.motion_width = guide.motion_width;
            contract.motion_height = guide.motion_height;
            contract.depth_width = guide.depth_width;
            contract.depth_height = guide.depth_height;
            contract.output_width = guide.output_width;
            contract.output_height = guide.output_height;
            const auto input_color_desc = input.color.texture->GetDesc();
            const auto input_depth_desc = input.depth.texture->GetDesc();
            const auto input_motion_desc =
                input.motion_vectors.texture->GetDesc();
            contract.input_color_width =
                static_cast<std::uint32_t>(input_color_desc.Width);
            contract.input_color_height = input_color_desc.Height;
            contract.input_depth_width =
                static_cast<std::uint32_t>(input_depth_desc.Width);
            contract.input_depth_height = input_depth_desc.Height;
            contract.input_motion_width =
                static_cast<std::uint32_t>(input_motion_desc.Width);
            contract.input_motion_height = input_motion_desc.Height;
            contract.output_color_width =
                static_cast<std::uint32_t>(destination_desc.Width);
            contract.output_color_height = destination_desc.Height;
            contract.depth_crop_x = depth_crop_x;
            contract.depth_crop_y = depth_crop_y;
            contract.motion_crop_x = motion_crop_x;
            contract.motion_crop_y = motion_crop_y;
            contract.scale_x = guide.scale_x;
            contract.scale_y = guide.scale_y;
            contract.jitter_x = guide.jitter_x;
            contract.jitter_y = guide.jitter_y;
            contract.motion_scale_x = params.motion_scale[0];
            contract.motion_scale_y = params.motion_scale[1];
            contract.frame_time_delta_ms = guide.frame_time_delta_ms;
            contract.camera_near = guide.camera_near;
            contract.camera_far = guide.camera_far;
            contract.reset = guide.reset;
            contract.jittered = guide.jittered;
            contract.depth_inverted = guide.depth_inverted;
            contract.depth_infinite = guide.depth_infinite;
            contract.native_previous_publication=guide.previous_opposite_guide->publication;
            contract.native_same_publication=guide.previous_same_native_camera->publication;
            contract.native_clip_to_opposite=native_motion.opposite;
            contract.native_clip_to_same=native_motion.same;
            contract.native_history_jitter_x=native_motion.jitter_difference[0];
            contract.native_history_jitter_y=native_motion.jitter_difference[1];
            contract.raw_camera_motion_included = guide.raw_camera_motion_included;
            contract.raw_motion_native_span = guide.raw_motion_native_span;
            contract.raw_clip_to_previous_clip = guide.raw_clip_to_previous_clip;
            contract.motion_temporal_valid = temporal_camera_valid;
            contract.motion_previous_publication = impl_->previous_source_publication[source_eye];
            contract.motion_middle_publication = middle ? middle->publication : 0;
            contract.depth_native_history_valid = contiguous_depth_history;
            contract.camera_up_sign =
                impl_->camera_axes == LukeProducerCameraAxes::right_down_forward ? -1 : 1;
            contract.camera_forward_sign =
                impl_->camera_axes == LukeProducerCameraAxes::right_down_forward ? -1 :
                (impl_->camera_axes == LukeProducerCameraAxes::physical_forward
                    ? (guide.camera_view_to_clip[11] < 0 ? -1 : 1) : 1);
            contract.depth_native_previous_publication = native_previous ? native_previous->publication : 0;
            std::copy(std::begin(native_previous_same_transform.values), std::end(native_previous_same_transform.values),
                contract.depth_current_to_native_previous.begin());
            std::copy(std::begin(middle_same_transform.values), std::end(middle_same_transform.values),
                contract.motion_current_to_middle_same.begin());
            std::copy(std::begin(previous_same_transform.values), std::end(previous_same_transform.values),
                contract.motion_current_to_previous_same.begin());
            std::copy(std::begin(audit_middle_opposite_transform.values), std::end(audit_middle_opposite_transform.values),
                contract.motion_current_to_middle_opposite.begin());
            contract.raw_previous_clip_to_clip = guide.raw_previous_clip_to_clip;
            contract.camera_position = guide.camera_position;
            const DlssMotionVectorFrame* tracked[]{&guide,
                guide.previous_opposite_guide.get(),guide.previous_same_guide.get()};
            for(unsigned i=0;i<3;++i)if(tracked[i]&&tracked[i]->gta_headlock) {
                const auto& receipt=*tracked[i]->gta_headlock;
                contract.gta_tracking_render[i]=receipt.render;
                contract.gta_tracking_mode[i]=receipt.mode;
                contract.gta_tracking_yaw[i]=receipt.yaw_offset;
                contract.gta_tracking_valid[i]=receipt.tracking_valid;
                contract.gta_tracking_poses[i]=receipt.tracking;
            }
            contract.camera_right = guide.camera_right;
            contract.camera_up = guide.camera_up;
            std::copy(camera.source_view_to_world.values + 8,
                camera.source_view_to_world.values + 11, contract.camera_forward.begin());
            contract.source_pose = {source_view.pose.position.x,
                source_view.pose.position.y, source_view.pose.position.z};
            contract.destination_pose = {destination_view.pose.position.x,
                destination_view.pose.position.y, destination_view.pose.position.z};
            contract.source_orientation = {source_view.pose.orientation.x,
                source_view.pose.orientation.y, source_view.pose.orientation.z,
                source_view.pose.orientation.w};
            contract.destination_orientation = {
                destination_view.pose.orientation.x,
                destination_view.pose.orientation.y,
                destination_view.pose.orientation.z,
                destination_view.pose.orientation.w};
            contract.view_to_clip = guide.camera_view_to_clip;
            contract.clip_to_view = guide.camera_clip_to_view;
            const auto copy_matrix = [](const Matrix4x4& source,
                std::array<float, 16>& destination) {
                std::copy(std::begin(source.values), std::end(source.values),
                    destination.begin());
            };
            copy_matrix(camera.source_world_to_view,
                contract.source_world_to_view);
            copy_matrix(camera.source_view_to_world,
                contract.source_view_to_world);
            copy_matrix(camera.source_view_to_clip,
                contract.source_view_to_clip);
            copy_matrix(camera.source_clip_to_view,
                contract.source_clip_to_view);
            copy_matrix(camera.destination_world_to_view,
                contract.destination_world_to_view);
            copy_matrix(camera.destination_view_to_world,
                contract.destination_view_to_world);
            copy_matrix(camera.destination_view_to_clip,
                contract.destination_view_to_clip);
            copy_matrix(camera.destination_clip_to_view,
                contract.destination_clip_to_view);
            copy_matrix(camera.camera_world_to_view,
                contract.camera_world_to_view);
            copy_matrix(camera.camera_view_to_world,
                contract.camera_view_to_world);
            copy_matrix(camera.camera_view_to_clip,
                contract.camera_view_to_clip);
            copy_matrix(camera.camera_clip_to_view,
                contract.camera_clip_to_view);
            two_frame_capture_guide_contract(contract);
        }
        const std::uint64_t configuration =
            (static_cast<std::uint64_t>(settings.mode) << 0) |
            (static_cast<std::uint64_t>(settings.debug) << 12) |
            (static_cast<std::uint64_t>(settings.clear_before_warping) << 13) |
            (static_cast<std::uint64_t>(params.shading_rate) << 16);
        report_stage(AfwStage::configuration,
            static_cast<std::int64_t>(configuration), interpreted_eye,
            guide.publication);
        report_stage(AfwStage::evaluate_begin, S_OK, source_eye,
            guide.publication);
        impl_->gpu_timing.mark(command_list,1);
        {
        AfwProviderCopyScope provider_copy_scope(impl_->renderer);
        impl_->clean_frame_buffers=false;
        impl_->evaluate(params);
        // Beta6 returns no output on a shading-rate change, before recording
        // warping work. Its next call rebuilds the internal shading resources.
        // Complete that bounded configuration handshake in this same list.
        if (!params.output_eye_frame_buffer) {
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
                S_FALSE, 1004, params.shading_rate, guide.publication);
            impl_->evaluate(params);
        }
        if(provider_copy_scope.repaired())
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
                S_OK,1005,provider_copy_scope.repaired(),guide.publication);
        }
        report_stage(AfwStage::evaluate_end, S_OK, source_eye,
            guide.publication);
        impl_->gpu_timing.mark(command_list,2);
        result = S_OK;
        if (params.output_eye_frame_buffer == nullptr ||
            params.output_eye_frame_buffer->color.texture == nullptr) {
            result = E_FAIL;
        } else {
            TextureDesc destination{};
            destination.texture = destination_color;
            destination.initial_state = D3D12_RESOURCE_STATE_COMMON;
            TextureDesc generated = params.output_eye_frame_buffer->color;
            const auto generated_desc = generated.texture->GetDesc();
            const auto& expected_generated=quality_divisor>1 ? input.color.texture->GetDesc() : destination_desc;
            if (!copy_compatible_color(generated_desc, expected_generated)) {
                result = E_INVALIDARG;
            } else {
                if (two_frame_capture_images_active()) {
                    TwoFrameImageInfo returned{};
                    returned.stage = "afw_plugin_returned_eye";
                    returned.stream = guide.stream;
                    returned.capture_serial = guide.publication;
                    returned.eye = static_cast<std::int32_t>(1U - source_eye);
                    returned.width = impl_->work_width;
                    returned.height = impl_->work_height;
                    two_frame_capture_record_ng_x(returned, impl_->device.Get(),
                        command_list, generated.texture, generated.initial_state);
                }
                if(quality_divisor>1) {
                    auto* expanded=impl_->color_scale.expanded();
                    impl_->color_scale.record(impl_->device.Get(),command_list,generated.texture,generated.initial_state,
                        impl_->work_width,impl_->work_height,expanded,D3D12_RESOURCE_STATE_COMMON,
                        static_cast<UINT>(color_desc.Width),color_desc.Height,1);
                    TextureDesc scaled{};scaled.texture=expanded;scaled.initial_state=D3D12_RESOURCE_STATE_COMMON;
                    copy(impl_->renderer,command_list,destination,scaled,color_box,0,0);
                } else copy(impl_->renderer, command_list, destination, generated,
                    color_box, 0, 0);
                report_stage(AfwStage::output_copied, S_OK, source_eye,
                    guide.publication);
                if (two_frame_capture_active()) {
                    TwoFrameImageInfo output{};
                    output.stage = "afw_generated_eye";
                    output.stream = guide.stream;
                    output.capture_serial = guide.publication;
                    output.eye = static_cast<std::int32_t>(1U - source_eye);
                    output.width = static_cast<std::uint32_t>(color_desc.Width);
                    output.height = color_desc.Height;
                    two_frame_capture_record_ng_x(output, impl_->device.Get(),
                        command_list, destination_color,
                        D3D12_RESOURCE_STATE_COMMON);
                }
            }
        }
        impl_->gpu_timing.mark(command_list,3);
        impl_->gpu_timing.resolve(command_list);
        const HRESULT close_result = recording.close();
        report_stage(AfwStage::command_list_closed, close_result, source_eye,
            guide.publication);
        if (FAILED(close_result) || FAILED(result)) {
            impl_->depth_staging_state = initial_depth_state;
            impl_->depth_calibration_contracts = initial_calibration_contracts;
            impl_->depth_calibration_reset_pending = initial_calibration_reset;
            return FAILED(close_result) ? close_result : result;
        }
        ID3D12CommandList* lists[] = {command_list};
        guide.motion_retention->begin_gpu_read();
        if (guide.depth_retention) guide.depth_retention->begin_gpu_read();
        if (impl_->gpu_timing_initialized) {
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
                settings.shading_rate,1100,guide.publication,(color_desc.Width<<32)|color_desc.Height);
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
                settings.mode,1101,guide.publication,source_eye);
        }
        impl_->gpu_timing.submitted(reinterpret_cast<std::uintptr_t>(impl_.get()),guide.publication);
        impl_->queue->ExecuteCommandLists(1, lists);
        impl_->depth_calibration_reset_pending = false;
        if (two_frame_capture_active())
            two_frame_capture_submission(impl_->queue.Get(), 1, lists);
        report_stage(AfwStage::command_list_executed, S_OK, source_eye,
            guide.publication);
        const std::uint64_t fence_value = impl_->next_submission_fence++;
        result = impl_->queue->Signal(impl_->submission_fence.Get(), fence_value);
        report_stage(AfwStage::fence_signaled, result, source_eye,
            guide.publication);
        if (SUCCEEDED(result)) {
            for(const auto& retention:{guide.motion_retention,guide.depth_retention}) {
                if(!retention) continue;
                const HRESULT retired=retention->retire_gpu_read(impl_->submission_fence.Get(),fence_value);
                if(FAILED(retired) && SUCCEEDED(result)) result=retired;
            }
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
                result, 87, guide.publication, fence_value);
        }
        if (SUCCEEDED(result)) {
            embedded::report_afw_shading_rate(impl_->active_shading_rate);
            impl_->last_submission_fence = fence_value;
            impl_->last_evaluated_publication = guide.publication;
            impl_->last_evaluated_eye = source_eye;
            impl_->previous_source_camera[source_eye] = camera;
            impl_->previous_source_xr_orientation[source_eye] = source_view.pose.orientation;
            impl_->previous_source_headlocked[source_eye] = native_peer_camera;
            impl_->previous_source_stream[source_eye] = guide.stream;
            impl_->previous_source_epoch[source_eye] = guide.epoch;
            impl_->previous_source_publication[source_eye] = guide.publication;
            reset_prepared_=false;
            queue_standby_locked();
        }
        return result;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

bool LukeAfwSynthesizer::initialized() const noexcept {
    std::scoped_lock lock(mutex_);
    return impl_ != nullptr;
}

}  // namespace xrfg
