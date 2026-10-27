#include "xrfg/d3d12_native_dlssg.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/two_frame_capture.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <share.h>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
#include <dxgi1_6.h>

#ifdef XRFG_NATIVE_DLSSG
#include "xrfg/ngx_feature_paths.hpp"
#include "native_dlssg_pack_shader.hpp"
#include "native_dlssg_pixel_shader.hpp"
#include "native_dlssg_seed_shader.hpp"
#include "native_dlssg_vertex_shader.hpp"
#include <DirectXMath.h>
// SDK 310.6 and later split the D3D helpers out and deprecate the old name.
#if __has_include(<nvsdk_ngx_helpers_dlssg_d3d.h>)
#include <nvsdk_ngx_helpers_dlssg_d3d.h>
#else
#include <nvsdk_ngx_helpers_dlssg.h>
#endif
#endif

namespace xrfg {

#ifdef XRFG_NATIVE_DLSSG
namespace {
using Microsoft::WRL::ComPtr;
using namespace DirectX;
constexpr UINT kSlots = D3D12SwapchainHistory::kSlotCount;
constexpr UINT kSrvs = 7;
constexpr UINT kBlock = kSrvs + 3; // seven SRVs, three UAVs
constexpr UINT kBlocksPerEye = 4; // A, B, and two outputs
constexpr UINT kMaxOutputs = 2;
constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kPixelRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr auto kAnyRead = kRead | kPixelRead;
constexpr auto kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto kCommon = D3D12_RESOURCE_STATE_COMMON;
// DLSS-G interpolates display-ready colour, so the pack shader encodes sRGB
// views. An 8-bit swapchain's frame is packed at 8 bits, which hold its codes
// exactly and which NGX evaluates 2-4% faster. The reseed's resampled previous
// frame falls between those codes, so it keeps ten bits in a texture of its
// own; that measures less error than ten bits for both. The generated alpha is
// not used, so the two-bit alpha does not matter.
constexpr DXGI_FORMAT kTenBitColorFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
constexpr DXGI_FORMAT kEightBitColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
// A camera change that moves no pixel of the view further than this keeps the
// NGX history instead of reseeding it with the aligned previous frame.
constexpr float kReseedPixels = 0.1F;
// Pairs skipped before another attempt after NGX could not create a feature.
constexpr UINT kCreateRetryPairs = 120;
// Stereo eyes share one NGX feature, side by side: most of an evaluation's
// cost is fixed, so one wide evaluation costs far less than one per eye. The
// seam between them repeats each eye's edge, so neither eye's history reaches
// the other unless its motion crosses half the seam.
constexpr UINT kSeam = 64;
// NGX refuses a feature wider or taller than this. Eyes too wide for both and
// a full seam narrow the seam to fit, down to this; 8 to 128 pixels measured
// alike. Wider still, each eye gets its own feature.
constexpr UINT kMaxFeatureSize = 8192;
constexpr UINT kMinSeam = 16;
// Root constants per dispatch or draw.
constexpr UINT kParams = 52;
// The motion and depth rectangle a reset evaluation reads.
constexpr UINT kResetGuideSize = 64;
// SeedNativeDlssG's group width; its groups are 8 high, like the pack's.
constexpr UINT kSeedGroupWidth = 16;
// The lowest reduced feature resolution, in percent of the eye's.
constexpr UINT kMinScale = 25;
// A reduced-resolution generated frame takes the real frame's detail where the
// two differ by less than half the display range, fading towards it. Of 1/32
// to 1, a half measured the least error at moving edges.
constexpr float kDetailFalloff = 2.0F;

struct Params {
    UINT extent[2], color_slice, guide_slice;
    float output_rect[4], motion_rect[4], depth_rect[4];
    float motion_scale[2], jitter_delta[2], rotation[4];
    float source_tangents[4], target_tangents[4];
    UINT motion_slice, encode_srgb, feature_extent[2];
    UINT eye_x, cell_x, cell_width, cell_height;
    float motion_normal[2], guide_scale[2];
    float detail_falloff, towards_a;
    // synthetic_camera: the compose's output is shown from a camera of its
    // own, synthetic_rotation taking its rays into B's (Output).
    UINT depth_inverted, synthetic_camera;
    float synthetic_rotation[4];
};
static_assert(sizeof(Params) == kParams * sizeof(UINT));
// Where an eye sits in its feature, and the columns its pack writes: the eye
// and its half of any seam beside it.
struct Placement {
    UINT feature, x, cell_x, cell_width;
};

// Batches transitions and remembers each resource's resting state, so a
// record that stops early returns everything it touched to that state.
class Transitions {
  public:
    explicit Transitions(ID3D12GraphicsCommandList *list) noexcept : list_(list) {}
    void to(ID3D12Resource *r, D3D12_RESOURCE_STATES rest, D3D12_RESOURCE_STATES state) {
        if (!r) {
            return;
        }
        Entry *entry = find(r);
        if (!entry) {
            // Two eyes touch at most 28 distinct resources.
            if (tracked_count_ == tracked_.size()) {
                throw std::length_error("native DLSS-G transitions");
            }
            entry = &tracked_[tracked_count_++];
            *entry = {r, rest, rest};
        }
        if (entry->state == state) {
            return;
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, entry->state, state};
        push(b);
        entry->state = state;
    }
    // Orders two NGX evaluations that write the same output.
    void uav(ID3D12Resource *r) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = r;
        push(b);
    }
    void flush() noexcept {
        if (pending_count_) {
            list_->ResourceBarrier(pending_count_, pending_.data());
            pending_count_ = 0;
        }
    }
    void restore() {
        for (UINT i = 0; i < tracked_count_; ++i) {
            to(tracked_[i].resource, tracked_[i].rest, tracked_[i].rest);
        }
        flush();
    }

  private:
    struct Entry {
        ID3D12Resource *resource;
        D3D12_RESOURCE_STATES rest, state;
    };
    Entry *find(ID3D12Resource *r) noexcept {
        for (UINT i = 0; i < tracked_count_; ++i) {
            if (tracked_[i].resource == r) {
                return &tracked_[i];
            }
        }
        return nullptr;
    }
    void push(const D3D12_RESOURCE_BARRIER &b) noexcept {
        if (pending_count_ == pending_.size()) {
            flush();
        }
        pending_[pending_count_++] = b;
    }
    ID3D12GraphicsCommandList *list_;
    std::array<Entry, 32> tracked_{};
    std::array<D3D12_RESOURCE_BARRIER, 32> pending_{};
    UINT tracked_count_{}, pending_count_{};
};

bool eight_bit(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
bool srgb(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
DXGI_FORMAT motion_format(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R16G16_TYPELESS   ? DXGI_FORMAT_R16G16_FLOAT
           : f == DXGI_FORMAT_R32G32_TYPELESS ? DXGI_FORMAT_R32G32_FLOAT
                                              : f;
}
DXGI_FORMAT depth_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    default:
        return f;
    }
}
bool same_device(ID3D12Device *d, ID3D12DeviceChild *r) {
    if (!r) {
        return false;
    }
    ComPtr<ID3D12Device> rd;
    ComPtr<IUnknown> x, y;
    return SUCCEEDED(r->GetDevice(IID_PPV_ARGS(&rd))) &&
           SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&x))) &&
           SUCCEEDED(rd->QueryInterface(IID_PPV_ARGS(&y))) && x.Get() == y.Get();
}
bool valid_rect(ID3D12Resource *r, UINT x, UINT y, UINT w, UINT h, UINT slice) {
    if (!r || !w || !h) {
        return false;
    }
    const auto d = r->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.SampleDesc.Count == 1 &&
           slice < d.DepthOrArraySize && !(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
           std::uint64_t(x) + w <= d.Width && std::uint64_t(y) + h <= d.Height;
}
D3D12ImageRect rect(const D3D12ReprojectionView &v, UINT w, UINT h) {
    return v.image_rect.width && v.image_rect.height ? v.image_rect : D3D12ImageRect{0, 0, w, h};
}
UINT slice(const D3D12ReprojectionView &v, UINT index, UINT views, UINT arrays) {
    return v.array_slice != std::numeric_limits<UINT>::max() ? v.array_slice
           : arrays == views                                 ? index
                                                             : 0;
}
XMVECTOR orientation(const D3D12ReprojectionView &v) {
    const auto q = normalize(v.pose.orientation);
    return XMVectorSet(q.x, q.y, q.z, q.w);
}
// Rotates a B-camera ray into A's camera, as the pack shader does.
XMVECTOR b_to_a(const D3D12ReprojectionView &a, const D3D12ReprojectionView &b) {
    return XMQuaternionMultiply(orientation(b), XMQuaternionConjugate(orientation(a)));
}
std::array<float, 4> tangents(const D3D12FieldOfView &f) {
    return {std::tan(f.angle_left), std::tan(f.angle_right), std::tan(f.angle_up),
            std::tan(f.angle_down)};
}
// The furthest any pixel of B's view moves when A is aligned into B's camera.
// The alignment is rotational, so a translation alone never moves a pixel.
float alignment_pixels(const D3D12ReprojectionView &a, const D3D12ReprojectionView &b,
                       UINT width, UINT height) {
    const auto q = b_to_a(a, b);
    const auto at = tangents(a.fov), bt = tangents(b.fov);
    float furthest = 0;
    for (const float u : {0.0F, 0.5F, 1.0F}) {
        for (const float v : {0.0F, 0.5F, 1.0F}) {
            const auto ray = XMVector3Rotate(
                XMVectorSet(bt[0] + (bt[1] - bt[0]) * u, bt[2] + (bt[3] - bt[2]) * v, -1, 0), q);
            const float z = -XMVectorGetZ(ray);
            if (z <= 0.00001F) {
                return std::numeric_limits<float>::infinity();
            }
            const float au = (XMVectorGetX(ray) / z - at[0]) / (at[1] - at[0]);
            const float av = (XMVectorGetY(ray) / z - at[2]) / (at[3] - at[2]);
            furthest = std::max({furthest, std::abs(au - u) * width, std::abs(av - v) * height});
        }
    }
    return furthest;
}
XMMATRIX projection(const D3D12FieldOfView &f, const DlssMotionVectorFrame &g) {
    const float l = std::tan(f.angle_left), r = std::tan(f.angle_right);
    const float t = std::tan(f.angle_up), b = std::tan(f.angle_down);
    const float n = g.camera_near, far_plane = g.camera_far;
    const float z = g.depth_infinite   ? (g.depth_inverted ? 0.f : -1.f)
                    : g.depth_inverted ? n / (far_plane - n)
                                       : far_plane / (n - far_plane);
    const float zw = g.depth_infinite   ? (g.depth_inverted ? n : -n)
                     : g.depth_inverted ? n * far_plane / (far_plane - n)
                                        : n * far_plane / (n - far_plane);
    return XMMATRIX(2 / (r - l), 0, 0, 0, 0, 2 / (t - b), 0, 0, (r + l) / (r - l),
                    (t + b) / (t - b), z, -1, 0, 0, zw, 0);
}
XMMATRIX camera(const Pose &p) {
    const auto q = normalize(p.orientation);
    return XMMatrixRotationQuaternion(XMVectorSet(q.x, q.y, q.z, q.w)) *
           XMMatrixTranslation(p.position.x, p.position.y, p.position.z);
}
void matrix(float (&out)[4][4], FXMMATRIX m) {
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4 *>(out), m);
}
std::filesystem::path module_directory() {
    HMODULE m{};
    wchar_t p[32768]{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_directory), &m);
    GetModuleFileNameW(m, p, 32768);
    return std::filesystem::path(p).parent_path();
}
bool environment(const wchar_t *name) {
    return GetEnvironmentVariableW(name, nullptr, 0) != 0;
}
UINT environment(const wchar_t *name, UINT fallback) {
    wchar_t value[16]{};
    const DWORD n = GetEnvironmentVariableW(name, value, 16);
    return n && n < 16 ? UINT(std::wcstoul(value, nullptr, 10)) : fallback;
}
float environment(const wchar_t *name, float fallback) {
    wchar_t value[32]{};
    const DWORD n = GetEnvironmentVariableW(name, value, 32);
    return n && n < 32 ? float(std::wcstod(value, nullptr)) : fallback;
}
// Initializes NGX for the device while an OFXR feature uses it. NGX is never
// shut down here: the driver keeps one NGX instance per adapter for the whole
// process, and NVSDK_NGX_D3D12_Shutdown1 tears it down for every client - it
// shuts down each loaded feature module for the device, including the
// game's own DLSS upscaler that this path captures guides from.
struct DeviceUse {
    ComPtr<ID3D12Device> device;
    UINT users;
};
struct AdapterFrames {
    LUID adapter;
    std::uint32_t frames;
};
std::mutex ngx_mutex;
std::vector<DeviceUse> ngx_devices;
std::vector<AdapterFrames> ngx_frames;
// With XRFG_TEST_NATIVE_DLSSG_VERBOSE, why native generation could not start
// goes to ofxr-native-dlssg.log in the NGX log directory: NGX's own logs do
// not record its capability answers.
void diagnose(const char *format, ...) {
    if (!bridge_flight_logger().enabled() && !bridge_flight_logger().ram_enabled()) return;
    if (!environment(L"XRFG_TEST_NATIVE_DLSSG_VERBOSE")) return;
    wchar_t local[32768]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    if (!*local) return;
    const auto path = std::filesystem::path(local) / L"ofxr-bridge" / L"NGX" / L"ofxr-native-dlssg.log";
    FILE *file = _wfsopen(path.c_str(), L"ab", _SH_DENYNO);
    if (!file) return;
    std::fprintf(file, "%llu pid=%lu ", GetTickCount64(), GetCurrentProcessId());
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(file, format, arguments);
    va_end(arguments);
    std::fputc('\n', file);
    std::fclose(file);
}
HRESULT acquire_ngx(ID3D12Device *d) {
    std::scoped_lock lock(ngx_mutex);
    for (auto &e : ngx_devices) {
        if (e.device.Get() == d) {
            ++e.users;
            return S_OK;
        }
    }
    const auto dir = module_directory();
    NgxFeaturePaths search(nullptr,dir.wstring());
    auto info=*search.get();
    if (environment(L"XRFG_TEST_NATIVE_DLSSG_VERBOSE") &&
        (bridge_flight_logger().enabled() || bridge_flight_logger().ram_enabled())) {
        info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
        info.LoggingInfo.LoggingCallback = [](const char *message, NVSDK_NGX_Logging_Level,
                                              NVSDK_NGX_Feature) {
            std::fprintf(stderr, "NGX: %s\n", message);
        };
    }
    wchar_t local[32768]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    auto logs = *local ? std::filesystem::path(local) / L"ofxr-bridge" / L"NGX" : dir / L"NGX";
    std::filesystem::create_directories(logs);
    const auto result = NVSDK_NGX_D3D12_Init_with_ProjectID("2152e8d8-a3d7-4d4c-88a5-37f740f81036",
                                                            NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                            "OFXR-Bridge", logs.c_str(), d, &info);
    diagnose("NGX initialization result=0x%x", unsigned(result));
    bridge_flight_logger().event(BridgeFlightOperation::native_dlssg_provider,
        static_cast<unsigned>(result),1);
    if (NVSDK_NGX_FAILED(result)) {
        return DXGI_ERROR_UNSUPPORTED;
    }
    ngx_devices.push_back({d, 1});
    return S_OK;
}
// The device stays initialised once its last user goes. A later instance -
// the one a resized swapchain brings, say - would otherwise initialise NGX
// again in the middle of the game's own DLSS work, which recreates its
// features at the same resize.
void release_ngx(ID3D12Device *d) {
    std::scoped_lock lock(ngx_mutex);
    for (auto &e : ngx_devices) {
        if (e.device.Get() == d) {
            if (e.users) --e.users;
            return;
        }
    }
}
// A late caller cannot replace the game's first NGX feature search paths.
// Recover missing discovery or an incompatible core evaluation using the adjacent FG snippet, without touching
// the core's capability block, handles or process-wide lifetime.
struct DirectProvider {
    using Abi = unsigned(*)();
    using Requirements = NVSDK_NGX_Result(*)(IDXGIAdapter*, const NVSDK_NGX_FeatureDiscoveryInfo*, NVSDK_NGX_FeatureRequirement*);
    using Init = NVSDK_NGX_Result(*)(ID3D12Device*, const wchar_t*, NVSDK_NGX_Parameter*);
    using Create = NVSDK_NGX_Result(*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*, NVSDK_NGX_DLSSG_Create_Params*, NVSDK_NGX_Handle**);
    using Evaluate = NVSDK_NGX_Result(*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, NVSDK_NGX_D3D12_DLSSG_Eval_Params*, NVSDK_NGX_DLSSG_Opt_Eval_Params*);
    using Release = NVSDK_NGX_Result(*)(NVSDK_NGX_Handle*);
    Requirements requirements{}; Init init{}; Create create{}; Evaluate evaluate{}; Release release{};
    HMODULE module{};
    std::once_flag once;
    bool load() {
        std::call_once(once, [&] {
            const auto file = module_directory() / L"nvngx.dll_dlssg.dll";
            module = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!module) return;
            const auto abi = reinterpret_cast<Abi>(GetProcAddress(module, "ofxr_dlssg_abi"));
            if (!abi || abi() != 45601) return;
#define ENTRY(field, name) field = reinterpret_cast<decltype(field)>(GetProcAddress(module, name))
            ENTRY(requirements, "ofxr_dlssg_requirements"); ENTRY(init, "ofxr_dlssg_init");
            ENTRY(create, "ofxr_dlssg_create"); ENTRY(evaluate, "ofxr_dlssg_evaluate");
            ENTRY(release, "ofxr_dlssg_release");
#undef ENTRY
        });
        return requirements && init && create && evaluate && release;
    }
};
DirectProvider direct_provider;
struct LifetimeCounters {
    std::atomic<std::uint64_t> create_attempts{},create_successes{},create_failures{};
    std::atomic<std::uint64_t> created{},failed_create_handles{};
    std::atomic<std::uint64_t> release_attempts{},released{},release_failures{},live{};
};
LifetimeCounters lifetime;
void observe_created(NVSDK_NGX_Result result,NVSDK_NGX_Handle* handle,bool direct) noexcept {
    lifetime.create_attempts.fetch_add(1,std::memory_order_relaxed);
    if(NVSDK_NGX_SUCCEED(result))lifetime.create_successes.fetch_add(1,std::memory_order_relaxed);
    else lifetime.create_failures.fetch_add(1,std::memory_order_relaxed);
    if(handle) {
        lifetime.created.fetch_add(1,std::memory_order_relaxed);
        lifetime.live.fetch_add(1,std::memory_order_relaxed);
        if(NVSDK_NGX_FAILED(result))lifetime.failed_create_handles.fetch_add(1,std::memory_order_relaxed);
    }
    auto& logger=bridge_flight_logger();
    if(logger.enabled() || logger.ram_enabled())
        logger.event(BridgeFlightOperation::native_dlssg_provider,static_cast<unsigned>(result),
            9,reinterpret_cast<std::uintptr_t>(handle),direct?1:0);
}
void release_owned_feature(NVSDK_NGX_Handle* handle,bool direct) noexcept {
    if(!handle)return;
    lifetime.release_attempts.fetch_add(1,std::memory_order_relaxed);
    const auto result=direct?direct_provider.release(handle):NVSDK_NGX_D3D12_ReleaseFeature(handle);
    if(NVSDK_NGX_SUCCEED(result)) {
        lifetime.released.fetch_add(1,std::memory_order_relaxed);
        lifetime.live.fetch_sub(1,std::memory_order_relaxed);
    } else lifetime.release_failures.fetch_add(1,std::memory_order_relaxed);
    auto& logger=bridge_flight_logger();
    if(logger.enabled() || logger.ram_enabled())
        logger.event(BridgeFlightOperation::native_dlssg_provider,static_cast<unsigned>(result),
            10,reinterpret_cast<std::uintptr_t>(handle),direct?1:0);
}
struct DirectDevice { ComPtr<ID3D12Device> device; UINT frames{}; };
std::mutex direct_mutex;
std::vector<DirectDevice> direct_devices;
bool uses_direct(ID3D12Device* device) {
    std::scoped_lock lock(direct_mutex);
    for (const auto& item : direct_devices) if (item.device.Get() == device) return item.frames != 0;
    return false;
}
UINT recover_direct(ID3D12Device* device, NVSDK_NGX_Parameter* private_caps) {
    std::scoped_lock lock(direct_mutex);
    for (const auto& item : direct_devices) if (item.device.Get() == device) return item.frames;
    UINT frames{};
    const bool loaded = direct_provider.load();
    diagnose("direct provider module/ABI ready=%u", unsigned(loaded));
    if (loaded) {
        ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x10de &&
            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            const auto dir = module_directory();
            NgxFeaturePaths search(nullptr,dir.wstring());
            wchar_t local[32768]{}; GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
            const auto logs = *local ? std::filesystem::path(local) / L"ofxr-bridge" / L"NGX" : dir / L"NGX";
            NVSDK_NGX_FeatureDiscoveryInfo info{};
            info.SDKVersion = NVSDK_NGX_Version_API; info.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
            info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Application_Id;
            info.Identifier.v.ApplicationId = 0x4f4658524155544full;
            info.ApplicationDataPath = logs.c_str(); info.FeatureInfo = search.get();
            NVSDK_NGX_FeatureRequirement requirements{};
            auto result = direct_provider.requirements(adapter.Get(), &info, &requirements);
            diagnose("direct requirements result=0x%x support=0x%x", unsigned(result), unsigned(requirements.FeatureSupported));
            bridge_flight_logger().event(BridgeFlightOperation::native_dlssg_provider,
                static_cast<unsigned>(result),5,static_cast<unsigned>(requirements.FeatureSupported));
            if (NVSDK_NGX_SUCCEED(result) && requirements.FeatureSupported == 0) {
                result = direct_provider.init(device, logs.c_str(), private_caps);
                diagnose("direct init/population result=0x%x", unsigned(result));
                bridge_flight_logger().event(BridgeFlightOperation::native_dlssg_provider,
                    static_cast<unsigned>(result),6);
                // Require the snippet's actual device answer. Never manufacture
                // availability or assume RTX 40/50 capabilities from the model name.
                if (NVSDK_NGX_SUCCEED(result) && NVSDK_NGX_SUCCEED(private_caps->Get(
                        NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &frames)) && frames > 0 && frames <= 3) {
                    bridge_flight_logger().event(BridgeFlightOperation::native_dlssg_provider,
                        static_cast<unsigned>(result),7,frames,1);
                } else frames = 0;
                diagnose("direct generated frames=%u", frames);
            }
        }
    }
    direct_devices.push_back({device, frames});
    return frames;
}

// Requires acquire_ngx. Zero when frame generation is unavailable.
std::uint32_t query_generated_frames(ID3D12Device* device) {
    NVSDK_NGX_Parameter *caps{};
    const auto queried = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
    bridge_flight_logger().event(BridgeFlightOperation::native_dlssg_provider,
        static_cast<unsigned>(queried),2);
    if (NVSDK_NGX_FAILED(queried) || !caps) {
        diagnose("capability query result=0x%x", unsigned(queried));
        return 0;
    }
    int available = 0, init_result = 0, needs_driver = 0;
    unsigned int frames = 1, driver_major = 0, driver_minor = 0;
    caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
    caps->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &frames);
    caps->Get(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &init_result);
    caps->Get(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &needs_driver);
    caps->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, &driver_major);
    caps->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, &driver_minor);
    bridge_flight_logger().event(BridgeFlightOperation::native_dlssg_provider,
        static_cast<unsigned>(init_result),3,available,available ? std::max(frames,1U) : 0);
    diagnose("frame generation available=%d frames=%u feature_init=0x%x needs_driver=%d "
             "min_driver=%u.%u", available, frames, unsigned(init_result), needs_driver,
             driver_major, driver_minor);
    const auto recovered = !available && static_cast<NVSDK_NGX_Result>(init_result) == NVSDK_NGX_Result_FAIL_FeatureNotFound
        && !needs_driver ? recover_direct(device, caps) : 0U;
    NVSDK_NGX_D3D12_DestroyParameters(caps);
    return available ? std::max(frames, 1U) : recovered;
}
bool same_adapter(const LUID &x, const LUID &y) {
    return x.LowPart == y.LowPart && x.HighPart == y.HighPart;
}
void remember_frames(ID3D12Device *d, std::uint32_t frames) {
    const LUID adapter = d->GetAdapterLuid();
    std::scoped_lock lock(ngx_mutex);
    for (const auto &e : ngx_frames) {
        if (same_adapter(e.adapter, adapter)) {
            return;
        }
    }
    ngx_frames.push_back({adapter, frames});
}
} // namespace

struct D3D12NativeDlssG::Impl {
    struct DecisionReadback {
        ComPtr<ID3D12Resource> buffer;
        ComPtr<ID3D12Fence> fence;
        std::uint64_t value{}, publication{};
        UINT eyes{}, outputs{};
        std::shared_ptr<D3D12NativeFrameResult> receipt;
    };
    // One NGX DLSS-G instance and its private textures, holding one eye or
    // both side by side.
    struct Feature {
        bool direct{};
        NVSDK_NGX_Handle *handle{};
        NVSDK_NGX_Parameter *params{};
        ComPtr<ID3D12Resource> color, motion, depth;
        // The reseed's colour, when it needs a format of its own.
        ComPtr<ID3D12Resource> seed_color;
        std::array<ComPtr<ID3D12Resource>, kMaxOutputs> generated, disable;
        UINT width{}, height{}, outputs{}, guide_width{}, guide_height{};
    };
    struct RetiredFeature {
        Feature feature;
        ComPtr<ID3D12Fence> fence;
        std::uint64_t value{};
        bool awaiting_submission{true};
    };
    // The guide frame whose history an eye's feature continues.
    struct EyeHistory {
        std::uint64_t stream{}, epoch{}, serial{};
    };
    // What each descriptor block last described. Holding the resources means
    // an unchanged pointer is the same resource, never a reused address.
    struct Block {
        std::array<ComPtr<ID3D12Resource>, kBlock> resources;
        std::array<DXGI_FORMAT, 3> formats{};
    };
    struct Target {
        ComPtr<ID3D12Resource> image;
        UINT slice{};
    };
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pack, seed, compose;
    std::array<ComPtr<ID3D12DescriptorHeap>, kSlots> heaps;
    std::array<ComPtr<ID3D12DescriptorHeap>, kSlots> rtvs;
    std::array<std::array<Block, 2 * kBlocksPerEye>, kSlots> blocks;
    std::array<std::array<Target, 2 * kMaxOutputs>, kSlots> targets;
    std::array<Feature, 2> features;
    std::vector<RetiredFeature> retired_features;
    std::array<EyeHistory, 2> eyes;
    bool shared_stereo{true};
    UINT seam{kSeam};
    // The feature's resolution, in percent of each eye's, and how the
    // composed frame restores detail when that is below 100.
    UINT scale{100};
    float detail_falloff{kDetailFalloff};
    // NGX's depth heuristics. NGX linearises depth as one over it, which for
    // a reversed projection is the distance in near-plane units. NVIDIA
    // suggests scaling that by a tenth where depth is compressed; on 42
    // recorded Galactic Racer triplets it cut native generation's error by
    // 0.09-0.12 at 67% and 100%, better on 31 of them. Scales from 0.03 to
    // 0.2 measured alike; 10 and the other heuristics' settings were worse.
    // The rest are NVIDIA's defaults. Tests may set each.
    // R23 (1e2f9aec): avoid depth-based object separation when the bridge's
    // identity camera matrices cannot describe the game's third-person turn.
    // NVIDIA's 40 smears the character; 1000 keeps full scene motion together.
    float depth_scale{0.1F}, near_far_partition{600.0F}, object_separation{1000.0F};
    bool vectors_dilated{};
    D3D12_RESOURCE_DESC source{};
    DXGI_FORMAT view_format{}, color_format{kTenBitColorFormat}, seed_format{kTenBitColorFormat};
    UINT increment{}, rtv_increment{}, max_outputs{1};
    bool acquired{}, encode_srgb{};
    ComPtr<ID3D12Fence> completion;
    std::uint64_t completion_value{};
    std::array<DecisionReadback, kSlots> decisions;
    UINT recorded_slot{kSlots};
    ComPtr<ID3D12GraphicsCommandList> capture_list;
    bool diagnostic_decisions{}, capture_only{}, test_output{}, reseed_every_pair{};
    bool direct_route{}, recover_requested{}, recovery_attempted{};
    FILE *decision_log{};
    Skip skip{Skip::none};
    std::shared_ptr<D3D12NativeFrameResult> last_result;
    UINT create_retry{};
    std::uint64_t reseeds{}, failures{};
    void report(const char *what, unsigned code) noexcept {
        // The first failures and then one in 300: a persistent NGX rejection
        // must not flood the debugger once per pair.
        if (++failures > 8 && failures % 300 != 0) {
            return;
        }
        char message[160]{};
        std::snprintf(message, sizeof(message), "OFXR native DLSS FG %s: 0x%x (failure %llu)\n",
                      what, code, static_cast<unsigned long long>(failures));
        if (bridge_flight_logger().enabled() || bridge_flight_logger().ram_enabled()) OutputDebugStringA(message);
        if (test_output) {
            std::fputs(message, stderr);
        }
    }
    void poll_decisions() noexcept {
        try {
        const bool logging = bridge_flight_logger().enabled() || bridge_flight_logger().ram_enabled();
        if (logging && diagnostic_decisions && !decision_log) {
            const auto path = module_directory() /
                (L"ofxr-native-decisions-pid" + std::to_wstring(GetCurrentProcessId()) +
                 L"-instance" + std::to_wstring(reinterpret_cast<std::uintptr_t>(this)) + L".log");
            decision_log = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
        }
        for (auto &readback : decisions) {
            if (!readback.fence || !readback.value) continue;
            if (readback.receipt && readback.receipt->resolve() == D3D12NativeFrameResult::pending) continue;
            const auto completed = readback.fence->GetCompletedValue();
            if (completed == std::numeric_limits<std::uint64_t>::max() ||
                completed < readback.value) continue;
            const D3D12_RANGE range{0, 4 * sizeof(UINT)};
            void *mapped{};
            if (SUCCEEDED(readback.buffer->Map(0, &range, &mapped))) {
                const auto *flags = static_cast<const UINT *>(mapped);
                if (logging && decision_log) {
                    std::fprintf(decision_log,
                        "%llu publication=%llu eyes=%u outputs=%u disable=%u,%u,%u,%u\n",
                        GetTickCount64(), readback.publication, readback.eyes, readback.outputs,
                        flags[0], readback.outputs > 1 ? flags[1] : 0,
                        readback.eyes > 1 ? flags[2] : 0,
                        readback.eyes > 1 && readback.outputs > 1 ? flags[3] : 0);
                    std::fflush(decision_log);
                }
                const D3D12_RANGE no_write{0, 0};
                readback.buffer->Unmap(0, &no_write);
            }
            readback.value = 0;
            readback.fence.Reset();
        }
        } catch (...) {}
    }
    ~Impl() {
        poll_decisions();
        if (decision_log) std::fclose(decision_log);
        for (auto &f : features) {
            release_feature(f);
        }
        // The owner drains the synthesis queue before destruction.
        for (auto &retired : retired_features) release_feature(retired.feature);
        if (acquired) {
            release_ngx(device.Get());
        }
    }
    HRESULT texture(DXGI_FORMAT format, ComPtr<ID3D12Resource> &out, bool buffer, UINT width,
                    UINT height) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = buffer ? D3D12_RESOURCE_DIMENSION_BUFFER : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = buffer ? 4 : width;
        d.Height = buffer ? 1 : height;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Format = buffer ? DXGI_FORMAT_UNKNOWN : format;
        d.Layout = buffer ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR : D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = hp.VisibleNodeMask = 1;
        return device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&out));
    }
    void release_feature(Feature &f) noexcept {
        release_owned_feature(f.handle,f.direct);
        if (f.params) {
            NVSDK_NGX_D3D12_DestroyParameters(f.params);
        }
        f = {};
    }
    void retire_failed_feature(Feature &f) {
        // A rejected vendor create can still have recorded GPU work. Its
        // parameters/textures belong to this list until actual submission and
        // completion, even though the caller will present the current frame.
        retired_features.push_back({std::move(f), {}, 0, true});
        f = {};
    }
    void poll_retired_features() noexcept {
        for (auto iterator = retired_features.begin(); iterator != retired_features.end();) {
            if (iterator->awaiting_submission || !iterator->fence || !iterator->value) {
                ++iterator;
                continue;
            }
            const auto completed = iterator->fence->GetCompletedValue();
            if (completed == std::numeric_limits<std::uint64_t>::max() || completed < iterator->value) {
                ++iterator;
                continue;
            }
            release_feature(iterator->feature);
            iterator = retired_features.erase(iterator);
        }
    }
    HRESULT create_feature(ID3D12GraphicsCommandList *list, Feature &f, UINT width, UINT height,
                           UINT gw, UINT gh) {
        release_feature(f);
        HRESULT hr = texture(color_format, f.color, false, width, height);
        if (FAILED(hr)) {
            return hr;
        }
        hr = texture(DXGI_FORMAT_R16G16_FLOAT, f.motion, false, gw, gh);
        if (FAILED(hr)) {
            return hr;
        }
        hr = texture(DXGI_FORMAT_R32_FLOAT, f.depth, false, gw, gh);
        if (FAILED(hr)) {
            return hr;
        }
        if (seed_format != color_format) {
            hr = texture(seed_format, f.seed_color, false, width, height);
            if (FAILED(hr)) {
                return hr;
            }
        }
        f.width = width;
        f.height = height;
        f.guide_width = gw;
        f.guide_height = gh;
        hr = ensure_outputs(f, 1);
        if (FAILED(hr)) {
            return hr;
        }
        f.direct = direct_route;
        auto result = f.direct ? NVSDK_NGX_D3D12_GetCapabilityParameters(&f.params)
                               : NVSDK_NGX_D3D12_AllocateParameters(&f.params);
        if (NVSDK_NGX_FAILED(result)) {
            return E_FAIL;
        }
        NVSDK_NGX_DLSSG_Create_Params create{};
        create.Width = width; create.RenderWidth = gw;
        create.Height = height; create.RenderHeight = gh;
        create.NativeBackbufferFormat = color_format;
        result = f.direct ? direct_provider.create(list, f.params, &create, &f.handle)
                          : NGX_D3D12_CREATE_DLSSG(list, 1, 1, &f.handle, f.params, &create);
        observe_created(result,f.handle,f.direct);
        bridge_flight_logger().event(BridgeFlightOperation::native_dlssg_provider,
            static_cast<unsigned>(result),4,width,height);
        if (NVSDK_NGX_FAILED(result)) {
            diagnose("frame generation feature %ux%u creation result=0x%x", width, height,
                     unsigned(result));
            f.handle = nullptr;
            return DXGI_ERROR_UNSUPPORTED;
        }
        return S_OK;
    }
    // The second generated image exists only once 3X asks for it.
    HRESULT ensure_outputs(Feature &f, UINT count) {
        for (; f.outputs < count; ++f.outputs) {
            HRESULT hr = texture(color_format, f.generated[f.outputs], false, f.width, f.height);
            if (FAILED(hr)) {
                return hr;
            }
            hr = texture(DXGI_FORMAT_UNKNOWN, f.disable[f.outputs], true, 0, 0);
            if (FAILED(hr)) {
                return hr;
            }
        }
        return S_OK;
    }
    HRESULT pipelines() {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvs, 0, 0, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0};
        D3D12_ROOT_PARAMETER rp[3]{};
        for (UINT i = 0; i < 2; ++i) {
            rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[i].DescriptorTable = {1, &ranges[i]};
        }
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[2].Constants = {0, 0, kParams};
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 3;
        rd.pParameters = rp;
        ComPtr<ID3DBlob> blob, error;
        HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (FAILED(hr)) {
            return hr;
        }
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root));
        if (FAILED(hr)) {
            return hr;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC cs{};
        cs.pRootSignature = root.Get();
        cs.CS = {g_xrfg_native_dlssg_pack_shader, sizeof(g_xrfg_native_dlssg_pack_shader)};
        hr = device->CreateComputePipelineState(&cs, IID_PPV_ARGS(&pack));
        if (FAILED(hr)) {
            return hr;
        }
        cs.CS = {g_xrfg_native_dlssg_seed_shader, sizeof(g_xrfg_native_dlssg_seed_shader)};
        hr = device->CreateComputePipelineState(&cs, IID_PPV_ARGS(&seed));
        if (FAILED(hr)) {
            return hr;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC ps{};
        ps.pRootSignature = root.Get();
        ps.VS = {g_xrfg_native_dlssg_vertex_shader, sizeof(g_xrfg_native_dlssg_vertex_shader)};
        ps.PS = {g_xrfg_native_dlssg_pixel_shader, sizeof(g_xrfg_native_dlssg_pixel_shader)};
        ps.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        ps.SampleMask = UINT_MAX;
        ps.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        ps.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        ps.RasterizerState.DepthClipEnable = TRUE;
        ps.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        ps.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        ps.NumRenderTargets = 1;
        ps.RTVFormats[0] = view_format;
        ps.SampleDesc.Count = 1;
        return device->CreateGraphicsPipelineState(&ps, IID_PPV_ARGS(&compose));
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT slot, UINT base) {
        auto h = heaps[slot]->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(base) * increment;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT slot, UINT base) {
        auto h = heaps[slot]->GetGPUDescriptorHandleForHeapStart();
        h.ptr += UINT64(base) * increment;
        return h;
    }
    void srv(UINT slot, UINT base, ID3D12Resource *r, DXGI_FORMAT format, bool array) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = format;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.ViewDimension =
            array ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
        if (array) {
            d.Texture2DArray.MipLevels = 1;
            d.Texture2DArray.ArraySize = r->GetDesc().DepthOrArraySize;
        } else {
            d.Texture2D.MipLevels = 1;
        }
        device->CreateShaderResourceView(r, &d, cpu(slot, base));
    }
    // A work slot is reused only after its previous submission completed, so
    // a block can be rewritten here; one that still matches is left alone.
    void descriptors(UINT slot, UINT base, Feature &f, ID3D12Resource *color,
                     const DlssMotionVectorFrame &g, UINT output, ID3D12Resource *fallback) {
        // The feature's packed colour is both read, by a reduced-resolution
        // compose, and written, by the pack.
        const std::array<ID3D12Resource *, kBlock> resources{
            color, g.motion_vectors.Get(), g.depth.Get(), f.generated[output].Get(),
            f.disable[output].Get(), fallback, f.color.Get(), f.color.Get(), f.motion.Get(),
            f.depth.Get()};
        const std::array<DXGI_FORMAT, 3> formats{view_format,
                                                 motion_format(g.motion_vectors->GetDesc().Format),
                                                 depth_format(g.depth->GetDesc().Format)};
        auto &written = blocks[slot][base / kBlock];
        bool current = written.formats == formats;
        for (UINT i = 0; current && i < kBlock; ++i) {
            current = written.resources[i].Get() == resources[i];
        }
        if (current) {
            return;
        }
        srv(slot, base, color, formats[0], true);
        srv(slot, base + 1, resources[1], formats[1], true);
        srv(slot, base + 2, resources[2], formats[2], true);
        srv(slot, base + 3, resources[3], color_format, false);
        D3D12_SHADER_RESOURCE_VIEW_DESC raw{};
        raw.Format = DXGI_FORMAT_R32_TYPELESS;
        raw.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        raw.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        raw.Buffer.NumElements = 1;
        raw.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(resources[4], &raw, cpu(slot, base + 4));
        srv(slot, base + 5, fallback, formats[0], true);
        srv(slot, base + 6, resources[6], resources[6]->GetDesc().Format, false);
        for (UINT i = 0; i < 3; ++i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = resources[kSrvs + i]->GetDesc().Format;
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(resources[kSrvs + i], nullptr, &u,
                                              cpu(slot, base + kSrvs + i));
        }
        for (UINT i = 0; i < kBlock; ++i) {
            written.resources[i] = resources[i];
        }
        written.formats = formats;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE target(UINT slot, UINT index, ID3D12Resource *image,
                                       UINT array_slice) {
        auto h = rtvs[slot]->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(index) * rtv_increment;
        auto &written = targets[slot][index];
        if (written.image.Get() != image || written.slice != array_slice) {
            D3D12_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = view_format;
            rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rd.Texture2DArray.FirstArraySlice = array_slice;
            rd.Texture2DArray.ArraySize = 1;
            device->CreateRenderTargetView(image, &rd, h);
            written = {image, array_slice};
        }
        return h;
    }
    Params parameters(const D3D12ReprojectionView &view, const DlssMotionVectorFrame &g,
                      UINT view_index, UINT views) {
        Params p{};
        const auto r = rect(view, UINT(source.Width), source.Height);
        p.extent[0] = r.width;
        p.extent[1] = r.height;
        p.color_slice = slice(view, view_index, views, source.DepthOrArraySize);
        p.guide_slice = g.depth->GetDesc().DepthOrArraySize == 1 ? 0 : g.output_slice;
        p.motion_slice = g.motion_slice;
        p.encode_srgb = encode_srgb;
        p.depth_inverted = g.depth_inverted;
        p.output_rect[0] = float(r.offset_x);
        p.output_rect[1] = float(r.offset_y);
        p.output_rect[2] = float(r.width);
        p.output_rect[3] = float(r.height);
        const float ux = float(r.offset_x - g.output_x) / g.output_width,
                    uy = float(r.offset_y - g.output_y) / g.output_height;
        const float uw = float(r.width) / g.output_width, uh = float(r.height) / g.output_height;
        // A guide placed by its camera (MSFS 2024's foveated periphery) maps
        // the fractional part of its vectors and depth under the view.
        const auto mr = guide_motion_rect(g), dr = guide_depth_rect(g);
        p.motion_rect[0] = mr[0] + ux * mr[2];
        p.motion_rect[1] = mr[1] + uy * mr[3];
        p.motion_rect[2] = uw * mr[2];
        p.motion_rect[3] = uh * mr[3];
        p.depth_rect[0] = dr[0] + ux * dr[2];
        p.depth_rect[1] = dr[1] + uy * dr[3];
        p.depth_rect[2] = uw * dr[2];
        p.depth_rect[3] = uh * dr[3];
        p.motion_scale[0] = g.scale_x * float(g.output_width) / mr[2];
        p.motion_scale[1] = g.scale_y * float(g.output_height) / mr[3];
        if (g.jittered) {
            p.jitter_delta[0] =
                (g.jitter_x - g.previous_jitter_x) * float(g.output_width) / mr[2];
            p.jitter_delta[1] =
                (g.jitter_y - g.previous_jitter_y) * float(g.output_height) / mr[3];
        }
        return p;
    }
    // The eyes of a feature pack disjoint cells, so no barrier separates
    // their dispatches. NGX reads the inputs as non-pixel shader resources.
    void begin_pack(Transitions &t, Feature &f, bool colour_only = false) {
        for (auto *r : {f.color.Get(), f.motion.Get(), f.depth.Get()}) {
            if (!colour_only || r == f.color.Get()) t.to(r, kCommon, kWrite);
        }
    }
    void end_pack(Transitions &t, Feature &f) {
        for (auto *r : {f.color.Get(), f.motion.Get(), f.depth.Get()}) {
            t.to(r, kCommon, kRead);
        }
    }
    // A seed writes only the aligned colour and reads no guides.
    void pack_input(ID3D12GraphicsCommandList *list, Transitions &t, UINT slot, UINT base,
                    Feature &f, ID3D12Resource *color, const DlssMotionVectorFrame &g,
                    const Params &p, ID3D12Resource *fallback, bool seeding = false) {
        descriptors(slot, base, f, color, g, 0, fallback);
        if (!seeding) {
            t.to(g.motion_vectors.Get(), g.resource_state, kRead);
            t.to(g.depth.Get(), g.depth_resource_state, kRead);
        }
        t.flush();
        ID3D12DescriptorHeap *hh[]{heaps[slot].Get()};
        list->SetDescriptorHeaps(1, hh);
        list->SetComputeRootSignature(root.Get());
        list->SetPipelineState(seeding ? seed.Get() : pack.Get());
        list->SetComputeRootDescriptorTable(0, gpu(slot, base));
        list->SetComputeRootDescriptorTable(1, gpu(slot, base + kSrvs));
        list->SetComputeRoot32BitConstants(2, kParams, &p, 0);
        const UINT width = seeding ? kSeedGroupWidth : 8;
        list->Dispatch((p.cell_width + width - 1) / width, (p.cell_height + 7) / 8, 1);
    }
    // One evaluation covers every eye of the feature. The camera describes
    // the first of them; motion carries everything else.
    HRESULT evaluate(ID3D12GraphicsCommandList *list, Transitions &t, Feature &e,
                     const D3D12ReprojectionView &b, const DlssMotionVectorFrame &gb,
                     const Params &p, UINT count, UINT index, bool reset) {
        const UINT output = index - 1;
        t.to(e.generated[output].Get(), kCommon, kWrite);
        t.to(e.disable[output].Get(), kCommon, kWrite);
        t.flush();
        NVSDK_NGX_D3D12_DLSSG_Eval_Params in{};
        in.pBackbuffer = e.color.Get();
        in.pMVecs = e.motion.Get();
        in.pDepth = e.depth.Get();
        in.pOutputInterpFrame = e.generated[output].Get();
        in.pOutputDisableInterpolation = e.disable[output].Get();
        NVSDK_NGX_DLSSG_Opt_Eval_Params o{};
        // Both packed frames are in B's camera plane, so the camera matrices
        // describe no rotation between them; the engine vectors carry the rest.
        const auto bp = projection(b.fov, gb);
        matrix(o.cameraViewToClip, bp);
        matrix(o.clipToCameraView, XMMatrixInverse(nullptr, bp));
        matrix(o.clipToLensClip, XMMatrixIdentity());
        matrix(o.clipToPrevClip, XMMatrixIdentity());
        matrix(o.prevClipToClip, XMMatrixIdentity());
        const auto c = camera(b.pose);
        XMFLOAT3 right, up, fwd;
        XMStoreFloat3(&right, XMVector3TransformNormal(XMVectorSet(1, 0, 0, 0), c));
        XMStoreFloat3(&up, XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), c));
        XMStoreFloat3(&fwd, XMVector3TransformNormal(XMVectorSet(0, 0, -1, 0), c));
        o.cameraRight[0] = right.x;
        o.cameraRight[1] = right.y;
        o.cameraRight[2] = right.z;
        o.cameraUp[0] = up.x;
        o.cameraUp[1] = up.y;
        o.cameraUp[2] = up.z;
        o.cameraFwd[0] = fwd.x;
        o.cameraFwd[1] = fwd.y;
        o.cameraFwd[2] = fwd.z;
        o.cameraPos[0] = b.pose.position.x;
        o.cameraPos[1] = b.pose.position.y;
        o.cameraPos[2] = b.pose.position.z;
        o.cameraNear = gb.camera_near;
        o.cameraFar = gb.camera_far;
        o.cameraFOV = b.fov.angle_up - b.fov.angle_down;
        o.cameraAspectRatio = float(p.extent[0]) / p.extent[1];
        o.depthInverted = gb.depth_inverted;
        o.cameraMotionIncluded = true;
        o.motionVectorsDilated = vectors_dilated; // false: NGX dilates them at depth edges
        o.motionVectorsInvalidValue = std::numeric_limits<float>::max();
        // The pack writes motion as a fraction of the feature. NGX scales it
        // to pixels of the motion grid itself: handing it pixels with a unit
        // scale instead costs measurable quality.
        o.mvecScale[0] = float(e.guide_width);
        o.mvecScale[1] = float(e.guide_height);
        o.reset = reset;
        // Colour and motion are unjittered by now, and NGX's result does not
        // depend on this offset for them. Depth still carries the render
        // jitter, resampled onto the guide grid, so express it in that grid.
        o.jitterOffset[0] = gb.jitter_x * float(gb.output_width) / gb.depth_width *
                            p.feature_extent[0] / p.extent[0] * e.guide_width / e.width;
        o.jitterOffset[1] = gb.jitter_y * float(gb.output_height) / gb.depth_height *
                            p.feature_extent[1] / p.extent[1] * e.guide_height / e.height;
        o.multiFrameCount = count;
        o.multiFrameIndex = index;
        o.backbufferSubrectSize = {e.width, e.height};
        o.mvecsSubrectSize = o.depthSubrectSize = {e.guide_width, e.guide_height};
        // A reset's output is the same whatever motion and depth it gets, so
        // the seed packs only colour and NGX reads a token rectangle of them.
        // The colour must still cover the whole feature.
        if (reset) {
            o.mvecsSubrectSize = o.depthSubrectSize = {std::min(e.guide_width, kResetGuideSize),
                                                       std::min(e.guide_height, kResetGuideSize)};
        }
        o.minRelativeLinearDepthObjectSeparation = object_separation;
        NVSDK_NGX_Parameter_SetF(e.params, NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_Scale, depth_scale);
        NVSDK_NGX_Parameter_SetF(e.params, NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_NearFarPartition,
                                 near_far_partition);
        const auto result = e.direct ? direct_provider.evaluate(list, e.handle, e.params, &in, &o)
                                     : NGX_D3D12_EVALUATE_DLSSG(list, e.handle, e.params, &in, &o);
        if (capture_list) {
            TwoFrameNativeDlssgEvaluation c{};
            c.stream=gb.stream;c.publication=gb.publication;c.serial=gb.serial;c.previous_serial=gb.previous_serial;
            c.reset=reset;c.direct=e.direct;c.result=unsigned(result);c.count=count;c.index=index;
            c.flags=(o.depthInverted?1U:0U)|(o.cameraMotionIncluded?2U:0U)|(o.motionVectorsDilated?4U:0U)|
                (o.colorBuffersHDR?8U:0U)|(o.orthoProjection?16U:0U)|(o.notRenderingGameFrames?32U:0U);
            c.color=reinterpret_cast<std::uint64_t>(in.pBackbuffer);c.motion=reinterpret_cast<std::uint64_t>(in.pMVecs);
            c.depth=reinterpret_cast<std::uint64_t>(in.pDepth);c.generated=reinterpret_cast<std::uint64_t>(in.pOutputInterpFrame);
            c.disable=reinterpret_cast<std::uint64_t>(in.pOutputDisableInterpolation);
            c.width=e.width;c.height=e.height;c.color_format=color_format;c.input_color_format=e.color->GetDesc().Format;
            c.backbuffer_rect={o.backbufferSubrectBase.X,o.backbufferSubrectBase.Y,o.backbufferSubrectSize.Width,o.backbufferSubrectSize.Height};
            c.motion_rect={o.mvecsSubrectBase.X,o.mvecsSubrectBase.Y,o.mvecsSubrectSize.Width,o.mvecsSubrectSize.Height};
            c.depth_rect={o.depthSubrectBase.X,o.depthSubrectBase.Y,o.depthSubrectSize.Width,o.depthSubrectSize.Height};
            c.motion_scale={o.mvecScale[0],o.mvecScale[1]};c.jitter={o.jitterOffset[0],o.jitterOffset[1]};
            c.depth_scale=depth_scale;c.near_far_partition=near_far_partition;c.object_separation=object_separation;c.invalid_motion=o.motionVectorsInvalidValue;
            c.near_plane=o.cameraNear;c.far_plane=o.cameraFar;c.fov=o.cameraFOV;c.aspect=o.cameraAspectRatio;
            std::copy_n(o.cameraPos,3,c.position.begin());std::copy_n(o.cameraRight,3,c.right.begin());
            std::copy_n(o.cameraUp,3,c.up.begin());std::copy_n(o.cameraFwd,3,c.forward.begin());
            std::copy_n(&o.cameraViewToClip[0][0],16,c.view_to_clip.begin());std::copy_n(&o.clipToCameraView[0][0],16,c.clip_to_view.begin());
            std::copy_n(&o.clipToLensClip[0][0],16,c.clip_to_lens.begin());std::copy_n(&o.clipToPrevClip[0][0],16,c.clip_to_previous.begin());
            std::copy_n(&o.prevClipToClip[0][0],16,c.previous_to_clip.begin());
            two_frame_capture_native_dlssg_evaluation(c);
        }
        bridge_flight_logger().event(BridgeFlightOperation::native_dlssg_evaluate,
            unsigned(result), reset, index, e.direct);
        if (NVSDK_NGX_FAILED(result)) {
            if (last_result) last_result->provider_error = unsigned(result);
            // The game's older FG snippet can advertise/create FG yet reject
            // the current SDK's evaluation. Retire only our own feature after
            // its submission fence; never retry on this partially recorded list.
            if (!e.direct && !recovery_attempted &&
                result == NVSDK_NGX_Result_FAIL_InvalidParameter) {
                recover_requested = true;
            }
            report("evaluate failed", unsigned(result));
            return E_FAIL;
        }
        return S_OK;
    }
};
#else
struct D3D12NativeDlssG::Impl {};
#endif

D3D12NativeDlssGLifetime native_dlssg_lifetime_snapshot() noexcept {
#ifdef XRFG_NATIVE_DLSSG
    return {
        lifetime.create_attempts.load(std::memory_order_relaxed),
        lifetime.create_successes.load(std::memory_order_relaxed),
        lifetime.create_failures.load(std::memory_order_relaxed),
        lifetime.created.load(std::memory_order_relaxed),
        lifetime.failed_create_handles.load(std::memory_order_relaxed),
        lifetime.release_attempts.load(std::memory_order_relaxed),
        lifetime.released.load(std::memory_order_relaxed),
        lifetime.release_failures.load(std::memory_order_relaxed),
        lifetime.live.load(std::memory_order_relaxed)};
#else
    return {};
#endif
}

D3D12NativeDlssG::D3D12NativeDlssG() = default;
D3D12NativeDlssG::~D3D12NativeDlssG() = default;

HRESULT D3D12NativeDlssG::initialize(ID3D12Device *device, ID3D12CommandQueue *queue,
                                     const D3D12_RESOURCE_DESC &source,
                                     DXGI_FORMAT format, UINT scale) noexcept {
#ifdef XRFG_NATIVE_DLSSG
    try {
        auto p = std::make_unique<Impl>();
        p->device = device;
        p->queue = queue;
        p->source = source;
        p->view_format = format;
        p->encode_srgb = srgb(format);
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{kTenBitColorFormat};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                               sizeof(support))) ||
            !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
            p->seed_format = kEightBitColorFormat;
        }
        p->color_format = eight_bit(format) ? kEightBitColorFormat : p->seed_format;
        p->diagnostic_decisions = environment(L"XRFG_TEST_NATIVE_DLSSG_DECISIONS");
        p->capture_only = environment(L"XRFG_TEST_NATIVE_DLSSG_CAPTURE_ONLY");
        p->test_output = environment(L"XRFG_TEST_NATIVE_DLSSG");
        // Measures a turning head's cost in a game while the head is still.
        p->reseed_every_pair = environment(L"XRFG_TEST_NATIVE_DLSSG_RESEED_EVERY_PAIR");
        p->shared_stereo = !environment(L"XRFG_NATIVE_DLSSG_PER_EYE");
        p->seam = environment(L"XRFG_NATIVE_DLSSG_SEAM", kSeam);
        p->scale = std::clamp(environment(L"XRFG_NATIVE_DLSSG_SCALE", scale), kMinScale, 100U);
        p->detail_falloff = float(environment(L"XRFG_NATIVE_DLSSG_DETAIL", UINT(kDetailFalloff)));
        p->depth_scale = environment(L"XRFG_TEST_NATIVE_DLSSG_DEPTH_SCALE", p->depth_scale);
        p->near_far_partition = environment(L"XRFG_TEST_NATIVE_DLSSG_NEAR_FAR", p->near_far_partition);
        p->object_separation = environment(L"XRFG_TEST_NATIVE_DLSSG_SEPARATION", p->object_separation);
        p->vectors_dilated = environment(L"XRFG_TEST_NATIVE_DLSSG_DILATED");
        HRESULT hr = acquire_ngx(device);
        if (FAILED(hr)) {
            return hr;
        }
        p->acquired = true;
        const std::uint32_t frames = query_generated_frames(device);
        remember_frames(device, frames);
        if (!frames) {
            return DXGI_ERROR_UNSUPPORTED;
        }
        p->max_outputs = frames;
        p->direct_route = uses_direct(device);
        hr = p->pipelines();
        if (FAILED(hr)) {
            return hr;
        }
        p->increment =
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        p->rtv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT slot = 0; slot < kSlots; ++slot) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = 2 * kBlocksPerEye * kBlock;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p->heaps[slot]));
            if (FAILED(hr)) {
                return hr;
            }
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = 2 * kMaxOutputs;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p->rtvs[slot]));
            if (FAILED(hr)) {
                return hr;
            }
            { // Tiny permanent readbacks also support logging enabled mid-session.
                D3D12_HEAP_PROPERTIES hp{};
                hp.Type = D3D12_HEAP_TYPE_READBACK;
                hp.CreationNodeMask = hp.VisibleNodeMask = 1;
                D3D12_RESOURCE_DESC bd{};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bd.Width = 4 * sizeof(UINT);
                bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
                bd.SampleDesc.Count = 1;
                bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&p->decisions[slot].buffer));
                if (FAILED(hr)) return hr;
            }
        }
        impl_ = std::move(p);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
#else
    (void)device;
    (void)queue;
    (void)source;
    (void)format;
    (void)scale;
    return DXGI_ERROR_UNSUPPORTED;
#endif
}
void D3D12NativeDlssG::reset() noexcept {
#ifdef XRFG_NATIVE_DLSSG
    if (impl_) {
        for (auto &e : impl_->eyes) {
            e.serial = 0;
        }
    }
#endif
}
void D3D12NativeDlssG::submitted(ID3D12Fence *fence, std::uint64_t value, std::uint64_t decision_value) noexcept {
#ifdef XRFG_NATIVE_DLSSG
    if (impl_) {
        impl_->completion = fence;
        impl_->completion_value = value;
        if (impl_->capture_list) {
            ID3D12CommandList* lists[]{impl_->capture_list.Get()};
            two_frame_capture_submission(impl_->queue.Get(), 1, lists);
            impl_->capture_list.Reset();
        }
        for (auto &retired : impl_->retired_features) {
            if (!retired.awaiting_submission) continue;
            retired.fence = fence;
            retired.value = value;
            retired.awaiting_submission = false;
        }
        if (impl_->recorded_slot < kSlots) {
            auto &readback = impl_->decisions[impl_->recorded_slot];
            readback.fence = fence;
            readback.value = decision_value ? decision_value : value;
            if (readback.receipt) { readback.receipt->fence=fence; readback.receipt->value=readback.value; }
            impl_->recorded_slot = kSlots;
        }
    }
#else
    (void)fence;
    (void)value;
    (void)decision_value;
#endif
}
std::shared_ptr<D3D12NativeFrameResult> D3D12NativeDlssG::last_result() const noexcept {
#ifdef XRFG_NATIVE_DLSSG
    return impl_ ? impl_->last_result : nullptr;
#else
    return {};
#endif
}
D3D12NativeDlssG::Skip D3D12NativeDlssG::last_skip() const noexcept {
#ifdef XRFG_NATIVE_DLSSG
    return impl_ ? impl_->skip : Skip::none;
#else
    return Skip::none;
#endif
}
std::uint64_t D3D12NativeDlssG::reseeds() const noexcept {
#ifdef XRFG_NATIVE_DLSSG
    return impl_ ? impl_->reseeds : 0;
#else
    return 0;
#endif
}

HRESULT D3D12NativeDlssG::record(ID3D12GraphicsCommandList *list, UINT slot, ID3D12Resource *a,
                                 ID3D12Resource *b, std::span<const D3D12ReprojectionView> av,
                                 std::span<const D3D12ReprojectionView> bv,
                                 const DlssMotionVectorSet *ag, const DlssMotionVectorSet *bg,
                                 std::span<const Output> outputs,
                                 D3D12_RESOURCE_STATES release) noexcept {
#ifdef XRFG_NATIVE_DLSSG
    try {
        if (!impl_ || slot >= kSlots || !list || !a || !b || av.empty() || av.size() != bv.size() ||
            bv.size() > 2 || outputs.empty() || outputs.size() > kMaxOutputs) {
            return E_INVALIDARG;
        }
        auto &p = *impl_;
        p.skip = Skip::none;
        p.poll_retired_features();
        p.poll_decisions();
        p.last_result = std::make_shared<D3D12NativeFrameResult>();
        p.last_result->outputs = UINT(outputs.size());
        p.last_result->publication = bg && bg->eye_count && bg->eyes[0] ? bg->eyes[0]->publication : 0;
        const auto skip = [&](Skip reason, unsigned detail=0, unsigned eye=0) {
            reset();
            p.skip = reason;
            p.last_result->skip = unsigned(reason);
            p.last_result->detail = detail;
            p.last_result->mask.store(0, std::memory_order_release);
            bridge_flight_logger().event(BridgeFlightOperation::native_dlssg_record,
                unsigned(reason), p.last_result->publication, detail, eye);
            return S_FALSE;
        };
        // A slot may not overwrite an outstanding readback or its receipt.
        if (p.decisions[slot].receipt &&
            p.decisions[slot].receipt->resolve()==D3D12NativeFrameResult::pending)
            return skip(Skip::resizing, 90);
        if (p.capture_only) {
            return skip(Skip::guides, 1);
        }
        if (outputs.size() > p.max_outputs) {
            return skip(Skip::multi_frame_unsupported);
        }
        if (!ag || !bg || ag->eye_count != bg->eye_count || !bg->eye_count ||
            bg->eye_count > 2) {
            return skip(Skip::guides, 2);
        }
        // Validate the entire stereo pair before writing a command. No partial
        // eye, guessed depth, or cross-stream history is handed to NGX.
        const auto guide_index = [&](UINT i) {
            return p.source.DepthOrArraySize == 1 && bg->eye_count == bv.size()
                ? i : slice(bv[i], i, UINT(bv.size()), p.source.DepthOrArraySize);
        };
        for (UINT i = 0; i < bv.size(); ++i) {
            const UINT sl = guide_index(i);
            if (sl >= bg->eye_count || !ag->eyes[sl] || !bg->eyes[sl]) {
                return skip(Skip::guides, 3, i);
            }
            const auto &ga = *ag->eyes[sl];
            const auto &gb = *bg->eyes[sl];
            if (gb.reset) return skip(Skip::guides, 4, i);
            if (!ga.serial || gb.previous_serial != ga.serial) return skip(Skip::guides, 5, i);
            if (ga.stream != gb.stream) return skip(Skip::guides, 6, i);
            if (ga.epoch != gb.epoch) return skip(Skip::guides, 7, i);
            if (ga.depth_inverted != gb.depth_inverted || ga.depth_infinite != gb.depth_infinite ||
                ga.camera_near != gb.camera_near || ga.camera_far != gb.camera_far) {
                return skip(Skip::guides, 8, i);
            }
            // The aligned A is written into textures sized for B's viewport.
            const auto ar = rect(av[i], UINT(p.source.Width), p.source.Height);
            const auto br = rect(bv[i], UINT(p.source.Width), p.source.Height);
            if (ar.width != br.width || ar.height != br.height) {
                return skip(Skip::guides, 9, i);
            }
            for (const auto *g : {&ga, &gb}) {
                const auto r = g == &ga ? ar : br;
                const auto mf = g->motion_vectors
                                    ? motion_format(g->motion_vectors->GetDesc().Format)
                                    : DXGI_FORMAT_UNKNOWN;
                const auto df =
                    g->depth ? depth_format(g->depth->GetDesc().Format) : DXGI_FORMAT_UNKNOWN;
                if (!same_device(p.device.Get(), g->motion_vectors.Get()) ||
                    !same_device(p.device.Get(), g->depth.Get())) return skip(Skip::guides, 11, i);
                if (!valid_rect(g->motion_vectors.Get(),g->motion_x,g->motion_y,
                    g->motion_width,g->motion_height,g->motion_slice)) return skip(Skip::guides,12,i);
                if (!valid_rect(g->depth.Get(),g->depth_x,g->depth_y,g->depth_width,g->depth_height,
                    g->depth && g->depth->GetDesc().DepthOrArraySize==1 ? 0 : g->output_slice))
                    return skip(Skip::guides,13,i);
                if (mf!=DXGI_FORMAT_R16G16_FLOAT && mf!=DXGI_FORMAT_R32G32_FLOAT)
                    return skip(Skip::guides,14,i);
                if (df!=DXGI_FORMAT_R32_FLOAT && df!=DXGI_FORMAT_R16_UNORM &&
                    df!=DXGI_FORMAT_R24_UNORM_X8_TYPELESS && df!=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS)
                    return skip(Skip::guides,15,i);
                if (!std::isfinite(g->camera_near) || g->camera_near<=0 ||
                    !std::isfinite(g->camera_far) || g->camera_far<=g->camera_near)
                    return skip(Skip::guides,16,i);
                if (!std::isfinite(g->scale_x) || !std::isfinite(g->scale_y))
                    return skip(Skip::guides,17,i);
                if (!std::isfinite(g->jitter_x) || !std::isfinite(g->jitter_y) ||
                    !std::isfinite(g->previous_jitter_x) || !std::isfinite(g->previous_jitter_y))
                    return skip(Skip::guides,18,i);
                if (!g->output_width || !g->output_height || r.offset_x<g->output_x ||
                    r.offset_y<g->output_y || std::uint64_t(r.offset_x)+r.width>
                    std::uint64_t(g->output_x)+g->output_width || std::uint64_t(r.offset_y)+r.height>
                    std::uint64_t(g->output_y)+g->output_height) return skip(Skip::guides,19,i);
            }
        }
        // Two eyes share one feature side by side, each eye's pack cell taking
        // the half of the seam beside it; otherwise each eye has its own.
        // Each eye's size in the feature: its viewport, or that scaled down when
        // the feature runs at a reduced resolution.
        std::array<std::array<UINT, 2>, 2> eye_size{};
        for (UINT i = 0; i < bv.size(); ++i) {
            const auto r = rect(bv[i], UINT(p.source.Width), p.source.Height);
            eye_size[i] = {std::max(1U, (r.width * p.scale + 50) / 100),
                           std::max(1U, (r.height * p.scale + 50) / 100)};
        }
        bool shared = p.shared_stereo && bv.size() == 2;
        UINT seam = p.seam;
        if (shared) {
            const std::uint64_t eyes = std::uint64_t(eye_size[0][0]) + eye_size[1][0];
            seam = UINT(std::min<std::uint64_t>(seam, kMaxFeatureSize - std::min<std::uint64_t>(eyes, kMaxFeatureSize)));
            shared = seam >= std::min(p.seam, kMinSeam) &&
                     std::max(eye_size[0][1], eye_size[1][1]) <= kMaxFeatureSize;
        }
        const UINT feature_count = shared ? 1 : UINT(bv.size());
        std::array<Placement, 2> place{};
        // Width and height, then the motion and depth grid's.
        std::array<std::array<UINT, 4>, 2> feature_size{};
        if (shared) {
            const UINT w0 = eye_size[0][0], w1 = eye_size[1][0], half = seam / 2;
            feature_size[0] = {w0 + seam + w1, std::max(eye_size[0][1], eye_size[1][1])};
            place[0] = {0, 0, 0, w0 + half};
            place[1] = {0, w0 + seam, w0 + half, w1 + seam - half};
        } else {
            for (UINT i = 0; i < bv.size(); ++i) {
                feature_size[i] = {eye_size[i][0], eye_size[i][1]};
                place[i] = {i, 0, 0, eye_size[i][0]};
            }
        }
        // Guides the game renders at two thirds of the feature's resolution or
        // less (DLSS Quality and below, at full resolution) are packed to a
        // grid two thirds the feature's size, which loses none of their detail.
        // NGX then measures 1-2% faster at 3004x3004 and the native quality
        // tests the same or better; packed to a half-size grid, depth edges
        // measured worse.
        bool coarse = true;
        const std::uint64_t limit = 67ULL * p.scale;
        for (UINT i = 0; i < bv.size(); ++i) {
            const auto &gb = *bg->eyes[guide_index(i)];
            coarse = coarse && 10000ULL * gb.motion_width <= limit * gb.output_width &&
                     10000ULL * gb.motion_height <= limit * gb.output_height &&
                     10000ULL * gb.depth_width <= limit * gb.output_width &&
                     10000ULL * gb.depth_height <= limit * gb.output_height;
        }
        for (UINT f = 0; f < feature_count; ++f) {
            feature_size[f][2] = coarse ? (2 * feature_size[f][0] + 2) / 3 : feature_size[f][0];
            feature_size[f][3] = coarse ? (2 * feature_size[f][1] + 2) / 3 : feature_size[f][1];
        }
        const auto resized = [&](const Impl::Feature &feature, UINT f) {
            return feature.width != feature_size[f][0] || feature.height != feature_size[f][1] ||
                   feature.guide_width != feature_size[f][2] ||
                   feature.guide_height != feature_size[f][3] || feature.direct != p.direct_route;
        };
        if (p.recover_requested) {
            p.recover_requested = false;
            p.recovery_attempted = true;
            NVSDK_NGX_Parameter* caps{};
            UINT frames{};
            if (NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps)) && caps) {
                frames = recover_direct(p.device.Get(), caps);
                NVSDK_NGX_D3D12_DestroyParameters(caps);
            }
            bridge_flight_logger().event(BridgeFlightOperation::native_dlssg_provider,
                frames ? 1 : 0, 8, frames, 1);
            if (frames) {
                p.direct_route = true;
                p.max_outputs = frames;
            }
            // Keep this pair in real-copy bypass. The next pair validates the
            // actual private capacity and retires old histories behind their
            // existing GPU fence before creating private-owned handles.
            return skip(Skip::evaluate_failed);
        }
        // Changing a feature's dimensions destroys its neural history. A
        // previous queue submission can still own it: poll its fence, never
        // CPU-wait.
        for (UINT f = 0; f < feature_count; ++f) {
            const auto &feature = p.features[f];
            if (feature.handle && resized(feature, f) && p.completion) {
                const auto completed = p.completion->GetCompletedValue();
                if (completed == std::numeric_limits<std::uint64_t>::max() ||
                    completed < p.completion_value) return skip(Skip::resizing);
            }
        }
        // A feature NGX refused is retried after a pause rather than every
        // pair; the pairs in between show the current frame.
        if (p.create_retry) {
            --p.create_retry;
            return skip(Skip::feature_unavailable);
        }
        for (UINT f = 0; f < feature_count; ++f) {
            auto &feature = p.features[f];
            HRESULT hr = S_OK;
            const bool creating = !feature.handle || resized(feature, f);
            if (creating) {
                hr = p.create_feature(list, feature, feature_size[f][0], feature_size[f][1],
                                      feature_size[f][2], feature_size[f][3]);
                // A new feature holds no eye's history.
                for (UINT i = 0; i < bv.size(); ++i) {
                    if (place[i].feature == f) {
                        p.eyes[i] = {};
                    }
                }
            }
            if (SUCCEEDED(hr)) {
                hr = p.ensure_outputs(feature, UINT(outputs.size()));
            }
            if (FAILED(hr)) {
                p.report("feature creation failed", unsigned(hr));
                if (creating) p.retire_failed_feature(feature);
                // NGX may refuse the double-width feature; fall back to one per
                // eye on the next pair instead of pausing.
                if (shared) {
                    p.shared_stereo = false;
                } else {
                    p.create_retry = kCreateRetryPairs;
                }
                return skip(Skip::feature_unavailable);
            }
        }
        Transitions t(list);
        t.to(a, kCommon, kAnyRead);
        t.to(b, kCommon, kAnyRead);
        const bool capture = two_frame_capture_images_active() &&
            two_frame_capture_claim_dlssg_pair(bg->eyes[0]->stream);
        if (capture) p.capture_list = list;
        const auto snapshot = [&](const char* stage, ID3D12Resource* image,
                                  D3D12_RESOURCE_STATES state, UINT output = 0) {
            if (!capture || !image) return;
            TwoFrameImageInfo info{};
            info.stage = stage;
            info.stream = bg->eyes[0]->stream;
            info.capture_serial = bg->eyes[0]->publication;
            info.swapchain = reinterpret_cast<std::uint64_t>(image); // resource identity, not an XR handle
            info.width = UINT(image->GetDesc().Width);
            info.height = image->GetDesc().Height;
            two_frame_capture_timeline(stage, output, info.capture_serial, info.swapchain, 0);
            two_frame_capture_record_ng_x(info, p.device.Get(), list, image, state);
        };
        if (capture) for (UINT i = 0; i < bv.size(); ++i) {
            const auto& ga = *ag->eyes[guide_index(i)];
            const auto& gb = *bg->eyes[guide_index(i)];
            TwoFrameGuideContract c{};
            c.stage = "native_dlssg_pair";
            c.stream = gb.stream; c.publication = gb.publication;
            c.serial = gb.serial; c.previous_serial = ga.serial;
            c.source_eye = c.destination_eye = int(i);
            const auto pose = [](const Pose& v, auto& position, auto& orientation) {
                position = {v.position.x, v.position.y, v.position.z};
                orientation = {v.orientation.x, v.orientation.y, v.orientation.z, v.orientation.w};
            };
            pose(av[i].pose, c.source_pose, c.source_orientation);
            pose(bv[i].pose, c.destination_pose, c.destination_orientation);
            XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(c.source_view_to_clip.data()), projection(av[i].fov, ga));
            XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(c.destination_view_to_clip.data()), projection(bv[i].fov, gb));
            c.motion_width = gb.motion_width; c.motion_height = gb.motion_height;
            c.depth_width = gb.depth_width; c.depth_height = gb.depth_height;
            c.output_width = gb.output_width; c.output_height = gb.output_height;
            c.scale_x = gb.scale_x; c.scale_y = gb.scale_y;
            c.camera_near = gb.camera_near; c.camera_far = gb.camera_far;
            c.depth_inverted = gb.depth_inverted; c.depth_infinite = gb.depth_infinite;
            two_frame_capture_guide_contract(c);
            for (UINT output = 0; output < outputs.size(); ++output) {
                c.stage = "native_dlssg_output_camera";
                c.mode = int(output);
                pose(bv[i].pose, c.source_pose, c.source_orientation);
                c.destination_orientation = outputs[output].camera_to_current
                    ? (*outputs[output].camera_to_current)[i] : std::array<float,4>{0,0,0,1};
                two_frame_capture_guide_contract(c);
            }
        }
        // Align A and the engine backward motion into B's camera plane. The
        // NGX outputs can then keep the existing OpenXR B pose/FOV contract,
        // instead of presenting midpoint-camera pixels as B.
        const auto set_mapping = [&](Params &v, UINT i) {
            XMStoreFloat4(reinterpret_cast<XMFLOAT4 *>(v.rotation), b_to_a(av[i], bv[i]));
            const auto at = tangents(av[i].fov), bt = tangents(bv[i].fov);
            std::copy(at.begin(), at.end(), v.source_tangents);
            std::copy(bt.begin(), bt.end(), v.target_tangents);
            v.eye_x = place[i].x;
            v.cell_x = place[i].cell_x;
            v.cell_width = place[i].cell_width;
            v.cell_height = p.features[place[i].feature].height;
            v.motion_normal[0] = 1.0F / p.features[place[i].feature].width;
            v.motion_normal[1] = 1.0F / p.features[place[i].feature].height;
            const auto &f = p.features[place[i].feature];
            v.guide_scale[0] = float(f.guide_width) / f.width;
            v.guide_scale[1] = float(f.guide_height) / f.height;
            v.feature_extent[0] = eye_size[i][0];
            v.feature_extent[1] = eye_size[i][1];
            v.detail_falloff = p.detail_falloff;
        };
        std::array<Params, 2> eye_params{};
        for (UINT f = 0; f < feature_count; ++f) {
            auto &feature = p.features[f];
            const UINT first = shared ? 0 : f, last = shared ? 1 : f;
            bool reseed = p.reseed_every_pair;
            for (UINT i = first; i <= last; ++i) {
                const auto &ga = *ag->eyes[guide_index(i)];
                const auto &gb = *bg->eyes[guide_index(i)];
                auto &params = eye_params[i];
                params = p.parameters(bv[i], gb, i, UINT(bv.size()));
                set_mapping(params, i);
                const auto &e = p.eyes[i];
                reseed = reseed || e.serial != ga.serial || e.stream != ga.stream ||
                         e.epoch != ga.epoch ||
                         alignment_pixels(av[i], bv[i], params.extent[0], params.extent[1]) >
                             kReseedPixels;
            }
            // The seed's colour may be a texture of its own; its motion and depth
            // are the feature's, which the reset does not read.
            Impl::Feature seed = feature;
            if (feature.seed_color) {
                seed.color = feature.seed_color;
            }
            // A reset clears the whole feature's history, so a reseed packs the
            // aligned A of every eye in it.
            if (reseed) {
                p.begin_pack(t, seed, true);
                Params first_params{};
                for (UINT i = first; i <= last; ++i) {
                    const auto &ga = *ag->eyes[guide_index(i)];
                    auto pa = p.parameters(av[i], ga, i, UINT(av.size()));
                    set_mapping(pa, i);
                    p.pack_input(list, t, slot, i * kBlocksPerEye * kBlock, seed, a, ga, pa, b,
                                 true);
                    if (i == first) {
                        first_params = pa;
                    }
                }
                p.end_pack(t, seed);
                if (capture) t.flush();
                snapshot("dlssg_aligned_previous", seed.color.Get(), kRead);
                snapshot("dlssg_seed_motion", seed.motion.Get(), kRead);
                snapshot("dlssg_seed_depth", seed.depth.Get(), kRead);
                if (FAILED(p.evaluate(list, t, seed, bv[first], *ag->eyes[guide_index(first)],
                                      first_params, 1, 1, true))) {
                    t.restore();
                    return skip(Skip::evaluate_failed);
                }
                t.uav(feature.generated[0].Get());
                t.uav(feature.disable[0].Get());
                p.reseeds += last - first + 1;
            }
            p.begin_pack(t, feature);
            for (UINT i = first; i <= last; ++i) {
                p.pack_input(list, t, slot, i * kBlocksPerEye * kBlock + kBlock, feature, b,
                             *bg->eyes[guide_index(i)], eye_params[i], b);
            }
            p.end_pack(t, feature);
            if (capture) t.flush();
            snapshot("dlssg_packed_current", feature.color.Get(), kRead);
            snapshot("dlssg_packed_motion", feature.motion.Get(), kRead);
            snapshot("dlssg_packed_depth", feature.depth.Get(), kRead);
            for (UINT output = 0; output < outputs.size(); ++output) {
                if (FAILED(p.evaluate(list, t, feature, bv[first], *bg->eyes[guide_index(first)],
                                      eye_params[first], UINT(outputs.size()), output + 1,
                                      false))) {
                    t.restore();
                    return skip(Skip::evaluate_failed);
                }
            }
        }
        bridge_flight_logger().event(BridgeFlightOperation::native_dlssg_record,
            0, p.last_result->publication, 0, bv.size());
        // Every evaluation succeeded; only now are the outputs written.
        for (UINT f = 0; f < feature_count; ++f) {
            for (UINT output = 0; output < outputs.size(); ++output) {
                t.to(p.features[f].generated[output].Get(), kCommon, kPixelRead);
                t.to(p.features[f].disable[output].Get(), kCommon, kPixelRead);
            }
            // A reduced-resolution compose compares with the packed real frame.
            if (p.scale < 100) {
                t.to(p.features[f].color.Get(), kCommon, kAnyRead);
            }
        }
        // It also follows B's engine motion, at the nearest surface.
        if (p.scale < 100) {
            for (UINT i = 0; i < bv.size(); ++i) {
                const auto &gb = *bg->eyes[guide_index(i)];
                t.to(gb.motion_vectors.Get(), gb.resource_state, kAnyRead);
                t.to(gb.depth.Get(), gb.depth_resource_state, kAnyRead);
            }
        }
        for (const auto &output : outputs) {
            t.to(output.image, release, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        t.flush();
        ID3D12DescriptorHeap *hh[]{p.heaps[slot].Get()};
        if (capture) for (UINT f = 0; f < feature_count; ++f)
            for (UINT output = 0; output < outputs.size(); ++output)
                snapshot("dlssg_ngx_generated", p.features[f].generated[output].Get(), kPixelRead, output);
        list->SetDescriptorHeaps(1, hh);
        list->SetGraphicsRootSignature(p.root.Get());
        list->SetPipelineState(p.compose.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (UINT output = 0; output < outputs.size(); ++output) {
            for (UINT i = 0; i < bv.size(); ++i) {
                auto &feature = p.features[place[i].feature];
                const auto &gb = *bg->eyes[guide_index(i)];
                auto params = eye_params[i];
                params.towards_a = 1.0F - float(output + 1) / float(outputs.size() + 1);
                // An output shown from a camera of its own samples NGX's
                // frame along each pixel's ray, and A, which the compose has
                // in the seed's fallback slot, beyond B's view.
                if (outputs[output].camera_to_current) {
                    const auto &rotation = (*outputs[output].camera_to_current)[std::min(i, 1U)];
                    std::copy(rotation.begin(), rotation.end(), params.synthetic_rotation);
                    params.synthetic_camera = 1;
                }
                const UINT outbase = i * kBlocksPerEye * kBlock + (output + 2) * kBlock;
                p.descriptors(slot, outbase, feature, b, gb, output, a);
                list->SetGraphicsRootDescriptorTable(0, p.gpu(slot, outbase));
                list->SetGraphicsRootDescriptorTable(1, p.gpu(slot, outbase + kSrvs));
                list->SetGraphicsRoot32BitConstants(2, kParams, &params, 0);
                const auto rh = p.target(slot, i * kMaxOutputs + output, outputs[output].image,
                                         params.color_slice);
                list->OMSetRenderTargets(1, &rh, FALSE, nullptr);
                const D3D12_VIEWPORT vp{params.output_rect[0],
                                        params.output_rect[1],
                                        float(params.extent[0]),
                                        float(params.extent[1]),
                                        0,
                                        1};
                const D3D12_RECT sc{LONG(vp.TopLeftX), LONG(vp.TopLeftY),
                                    LONG(vp.TopLeftX + vp.Width), LONG(vp.TopLeftY + vp.Height)};
                list->RSSetViewports(1, &vp);
                list->RSSetScissorRects(1, &sc);
                list->DrawInstanced(3, 1, 0, 0);
            }
        }
        { // Read the provider decision after composition on the same queue.
            for (UINT f = 0; f < feature_count; ++f) {
                for (UINT output = 0; output < outputs.size(); ++output) {
                    t.to(p.features[f].disable[output].Get(), kCommon,
                         D3D12_RESOURCE_STATE_COPY_SOURCE);
                }
            }
            t.flush();
            // Eyes of a shared feature report its one decision.
            for (UINT i = 0; i < bv.size(); ++i) {
                for (UINT output = 0; output < outputs.size(); ++output) {
                    list->CopyBufferRegion(p.decisions[slot].buffer.Get(),
                        (i * 2 + output) * sizeof(UINT),
                        p.features[place[i].feature].disable[output].Get(), 0, sizeof(UINT));
                }
            }
            p.recorded_slot = slot;
            auto &readback = p.decisions[slot];
            readback.publication = bg->eyes[0]->publication;
            readback.eyes = UINT(bv.size());
            readback.outputs = UINT(outputs.size());
            readback.receipt = p.last_result;
            readback.receipt->readback=readback.buffer;
            readback.receipt->eyes=readback.eyes;
        }
        t.restore();
        for (UINT i = 0; i < bv.size(); ++i) {
            const auto &gb = *bg->eyes[guide_index(i)];
            p.eyes[i].serial = gb.serial;
            p.eyes[i].stream = gb.stream;
            p.eyes[i].epoch = gb.epoch;
        }
        if (capture) for (UINT output = 0; output < outputs.size(); ++output)
            snapshot("dlssg_composed_output", outputs[output].image, release, output);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
#else
    (void)list;
    (void)slot;
    (void)a;
    (void)b;
    (void)av;
    (void)bv;
    (void)ag;
    (void)bg;
    (void)outputs;
    (void)release;
    return DXGI_ERROR_UNSUPPORTED;
#endif
}

std::uint32_t native_dlssg_max_generated_frames(ID3D12Device *device) noexcept {
#ifdef XRFG_NATIVE_DLSSG
    try {
        if (!device) {
            return 0;
        }
        const LUID adapter = device->GetAdapterLuid();
        {
            std::scoped_lock lock(ngx_mutex);
            for (const auto &e : ngx_frames) {
                if (same_adapter(e.adapter, adapter)) {
                    return e.frames;
                }
            }
        }
        if (FAILED(acquire_ngx(device))) {
            return 0;
        }
        const std::uint32_t frames = query_generated_frames(device);
        release_ngx(device);
        remember_frames(device, frames);
        return frames;
    } catch (...) {
        return 0;
    }
#else
    (void)device;
    return 0;
#endif
}
} // namespace xrfg
