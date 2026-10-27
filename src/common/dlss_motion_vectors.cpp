#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/luke_camera_units.hpp"
#include "xrfg/luke_color_token.hpp"
#include "xrfg/luke_interleaved_history.hpp"
#include "xrfg/luke_motion_format.hpp"
#include "xrfg/two_frame_capture.hpp"
#include "xrfg/bridge_flight_logger.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <deque>

namespace xrfg {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint64_t kMaximumStereoPublicationGap = 4;
constexpr std::size_t kSnapshotSlotCount = 16;
// Leave two protected slots free for producer/consumer overlap. Stored frames
// have no previous-guide chains: those pins are attached only to a resolved join.
// Keep exact delayed colors AND their N-1 guides: OFXR can make the color
// arrive 5-6 publications behind production. Retention is not a pacing delay;
// snapshots remain immutable and CPU/GPU readers still prevent slot reuse.
constexpr std::size_t kLukeOriginHistoryCount = kSnapshotSlotCount - 2;
constexpr D3D12_RESOURCE_STATES kSnapshotReadState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

struct SnapshotSlot {
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12Device> allocation_device;
    D3D12_RESOURCE_DESC description{};
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
    std::shared_ptr<DlssSnapshotRetention> retention;
    // Recording a copy is not submission. The original Execute callback
    // replaces this list with a fence receipt before reuse or retirement.
    ComPtr<ID3D12GraphicsCommandList> producer_command_list;
    IUnknown* producer_identity{}; // canonical weak identity, pinned by producer_command_list
    ComPtr<ID3D12Fence> producer_fence;
    std::uint64_t producer_fence_value{};
    bool producer_canceled{};
    bool writer_unknown{}; // actual Execute observed, but no trustworthy completion receipt
    bool submission_started{}; // pre-Execute quarantine awaiting its first post-Execute receipt
};

bool retain_luke_reset_origins() noexcept {
    // GTA can reset one native eye before XR consumes its preceding color.
    // Preserve only exact protected keys, with their original epochs; this
    // does not authorize joining a new camera across that reset boundary.
    static const bool gta=[] {
        wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);
        const auto* name=wcsrchr(path,L'\\');
        return !_wcsicmp(name?name+1:path,L"GTA5_Enhanced.exe");
    }();
    return gta;
}

struct StreamState {
    bool retain_reset_origins{retain_luke_reset_origins()};
    LukeCameraUnitCalibration camera_units;
    LukeSceneCameraUnitCalibration scene_camera_units;
    std::uint64_t epoch{1};
    std::uint64_t serial{};
    std::uint64_t first_publication{};
    float jitter_x{};
    float jitter_y{};
    std::shared_ptr<const DlssMotionVectorFrame> previous;
    std::shared_ptr<const DlssMotionVectorFrame> previous2;
    std::shared_ptr<const DlssMotionVectorFrame> latest;
    std::deque<std::shared_ptr<const DlssMotionVectorFrame>> luke_origin_history;
    // Used only by an explicitly requested output diagnostic. Normal FSR
    // consumes the bridge-owned final XR color and allocates no output ring.
    std::array<SnapshotSlot, kSnapshotSlotCount> output_snapshots;
    std::size_t next_output_snapshot{};
    std::array<SnapshotSlot, kSnapshotSlotCount> snapshots;
    std::size_t next_snapshot{};
    std::array<SnapshotSlot, kSnapshotSlotCount> depth_snapshots;
    std::size_t next_depth_snapshot{};
    ComPtr<ID3D12Fence> submission_fence;
    ComPtr<ID3D12Device> submission_fence_device;
    ComPtr<IUnknown> submission_fence_queue;
    std::uint64_t next_submission_fence_value{1};
};

struct Registry {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, StreamState> streams;
    // A retired stream must disappear from resolution immediately, but its
    // snapshot resources can still be referenced by command lists that have
    // only been recorded. Actual Execute/fence receipts or successful Reset
    // cancellation, plus consumer retirement, authorize their destruction.
    std::vector<StreamState> retired_streams;
    DlssMotionVectorStatistics statistics;
    // Tracking toggles and stream resets cannot reuse a color-carried key.
    std::uint64_t next_publication{1};
    // Unlike the six-guide history, this sequence survives every clear/epoch.
    LukeColorTokenAllocator color_origin_tokens;
};

Registry& registry() {
    static Registry value;
    return value;
}

std::atomic_bool g_tracking_enabled{false};
std::atomic_bool g_retirement_pending{false};
std::atomic_uint64_t g_cancelable_snapshots{};
std::atomic_bool g_submission_ever_started{};

bool writer_complete(const SnapshotSlot& slot) noexcept {
    if(!slot.resource)return true;
    if(slot.writer_unknown)return false;
    if(slot.producer_canceled)return true;
    if(slot.producer_command_list || !slot.producer_fence || !slot.producer_fence_value)return false;
    const auto completed=slot.producer_fence->GetCompletedValue();
    return completed!=UINT64_MAX && completed>=slot.producer_fence_value;
}

void clear_frame_history(StreamState& stream) noexcept {
    stream.previous.reset();stream.previous2.reset();stream.latest.reset();
    stream.luke_origin_history.clear();
}

// Every retained allocation has its own actual writer proof. Consumer frame
// pins and AFW's GPU-read receipts independently prevent early destruction.
void collect_retired(Registry& registry) noexcept {
    for(auto& stream:registry.retired_streams) {
        clear_frame_history(stream);
        const auto trim=[](auto& slots) {
            for(auto& slot:slots)if(slot.resource && writer_complete(slot) &&
                (!slot.retention || (slot.retention.use_count()==1 && slot.retention->reusable())))
                slot=SnapshotSlot{};
        };
        trim(stream.output_snapshots);trim(stream.snapshots);trim(stream.depth_snapshots);
    }
    std::erase_if(registry.retired_streams,[](const auto& stream) {
        const auto empty=[](const auto& slots) {
            return std::none_of(slots.begin(),slots.end(),[](const auto& slot){return bool(slot.resource);});
        };
        return empty(stream.output_snapshots) && empty(stream.snapshots) && empty(stream.depth_snapshots);
    });
    g_retirement_pending.store(!registry.retired_streams.empty() ||
        (!g_tracking_enabled.load(std::memory_order_acquire) && !registry.streams.empty()),std::memory_order_release);
}

ComPtr<IUnknown> identity(IUnknown* object) noexcept {
    ComPtr<IUnknown> value;
    if (object != nullptr) {
        static_cast<void>(object->QueryInterface(IID_PPV_ARGS(value.GetAddressOf())));
    }
    return value;
}

bool same_identity(IUnknown* left, IUnknown* right) noexcept {
    const auto a = identity(left);
    const auto b = identity(right);
    return a && b && a.Get() == b.Get();
}

std::vector<ComPtr<IUnknown>> command_identities(std::uint32_t count,ID3D12CommandList* const* lists) {
    std::vector<ComPtr<IUnknown>> values;values.reserve(count);
    for(std::uint32_t index=0;index<count;++index)values.push_back(identity(lists[index]));
    return values;
}
bool contains_identity(const std::vector<ComPtr<IUnknown>>& values,IUnknown* value) noexcept {
    return value && std::any_of(values.begin(),values.end(),[&](const auto& candidate){return candidate.Get()==value;});
}

bool same_device(IUnknown* left, IUnknown* right) noexcept {
    if (left == nullptr || right == nullptr) return false;
    ComPtr<ID3D12DeviceChild> left_child;
    ComPtr<ID3D12DeviceChild> right_child;
    ComPtr<ID3D12Device> left_device;
    ComPtr<ID3D12Device> right_device;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(left_child.GetAddressOf()))) &&
        SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(right_child.GetAddressOf()))) &&
        SUCCEEDED(left_child->GetDevice(IID_PPV_ARGS(left_device.GetAddressOf()))) &&
        SUCCEEDED(right_child->GetDevice(IID_PPV_ARGS(right_device.GetAddressOf()))) &&
        same_identity(left_device.Get(), right_device.Get());
}

std::shared_ptr<const DlssMotionVectorFrame> protected_publication(
    const Registry& state, std::uint64_t publication) {
    std::shared_ptr<const DlssMotionVectorFrame> found;
    for (const auto& [id, stream] : state.streams)
        for (const auto& frame : stream.luke_origin_history)
            if (frame->publication == publication) {
                if (found || frame->stream != id ||
                    (!stream.retain_reset_origins && frame->epoch != stream.epoch)) return {};
                found = frame;
            }
    return found;
}

bool interleaved_frames(const DlssMotionVectorFrame& current,
    const std::shared_ptr<const DlssMotionVectorFrame>& middle,
    const std::shared_ptr<const DlssMotionVectorFrame>& older) {
    if (!middle || !older || !luke_interleaved_history_matches(dlss_native_camera_history(current),
            dlss_native_camera_history(*middle), dlss_native_camera_history(*older))) return false;
    // Protected keys and canonical allocation owners also agree. Keep every
    // original feature identity and its independent reset epoch unchanged.
    return older->color_origin_token &&
        std::uint64_t(older->color_origin_token) + 1 == middle->color_origin_token &&
        std::uint64_t(middle->color_origin_token) + 1 == current.color_origin_token &&
        same_identity(current.motion_snapshot_device.Get(), middle->motion_snapshot_device.Get()) &&
        same_identity(current.motion_snapshot_device.Get(), older->motion_snapshot_device.Get()) &&
        same_identity(current.producer_queue.Get(), middle->producer_queue.Get()) &&
        same_identity(current.producer_queue.Get(), older->producer_queue.Get());
}

bool valid(const DlssMotionVectorPublication& publication) noexcept {
    if (publication.stream == 0 || publication.output == nullptr ||
        publication.motion_vectors == nullptr ||
        publication.producer_queue == nullptr ||
        publication.producer_command_list == nullptr || publication.output_width == 0 ||
        publication.output_height == 0 || publication.motion_width == 0 ||
        publication.motion_height == 0 || !std::isfinite(publication.scale_x) ||
        !std::isfinite(publication.scale_y) || !std::isfinite(publication.jitter_x) ||
        !std::isfinite(publication.jitter_y)) {
        return false;
    }
    const auto output = publication.output->GetDesc();
    const auto motion = publication.motion_vectors->GetDesc();
    const bool base_valid = output.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        motion.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        static_cast<std::uint64_t>(publication.output_x) + publication.output_width <=
            output.Width &&
        static_cast<std::uint64_t>(publication.output_y) + publication.output_height <=
            output.Height &&
        static_cast<std::uint64_t>(publication.motion_x) + publication.motion_width <=
            motion.Width &&
        static_cast<std::uint64_t>(publication.motion_y) + publication.motion_height <=
            motion.Height;
    if (!base_valid || publication.depth == nullptr) return base_valid;
    const auto depth = publication.depth->GetDesc();
    return depth.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        publication.depth_width != 0 && publication.depth_height != 0 &&
        static_cast<std::uint64_t>(publication.depth_x) + publication.depth_width <=
            depth.Width &&
        static_cast<std::uint64_t>(publication.depth_y) + publication.depth_height <=
            depth.Height &&
        std::isfinite(publication.frame_time_delta_ms) &&
        publication.frame_time_delta_ms > 0.0F &&
        std::isfinite(publication.camera_near) &&
        std::isfinite(publication.camera_far);
}

bool matching_description(
    const D3D12_RESOURCE_DESC& left,
    const D3D12_RESOURCE_DESC& right) noexcept {
    return left.Dimension == right.Dimension && left.Alignment == right.Alignment &&
        left.Width == right.Width && left.Height == right.Height &&
        left.DepthOrArraySize == right.DepthOrArraySize &&
        left.MipLevels == right.MipLevels && left.Format == right.Format &&
        left.SampleDesc.Count == right.SampleDesc.Count &&
        left.SampleDesc.Quality == right.SampleDesc.Quality &&
        left.Layout == right.Layout && left.Flags == right.Flags;
}

D3D12_RESOURCE_BARRIER transition_barrier(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

template <std::size_t SnapshotCount>
HRESULT snapshot_resource(
    std::array<SnapshotSlot, SnapshotCount>& snapshots,
    std::size_t& next_snapshot,
    ID3D12Resource* source,
    D3D12_RESOURCE_STATES source_state,
    ID3D12GraphicsCommandList* producer_command_list,
    ID3D12CommandQueue* producer_queue,
    ID3D12Device* verified_producer_device,
    ComPtr<ID3D12Resource>* snapshot,
    D3D12_RESOURCE_STATES snapshot_state = kSnapshotReadState,
    std::shared_ptr<DlssSnapshotRetention>* retention = nullptr,
    ComPtr<ID3D12Device>* allocation_receipt = nullptr,
    std::size_t retained_capacity = SnapshotCount,
    bool dlss_float_motion = false) noexcept {
    if (source == nullptr || producer_command_list == nullptr ||
        producer_queue == nullptr || snapshot == nullptr) return E_POINTER;
    snapshot->Reset();
    if (allocation_receipt) allocation_receipt->Reset();
    const auto list_identity=identity(producer_command_list);
    if(!list_identity)return E_NOINTERFACE;

    ComPtr<ID3D12Device> source_device;
    ComPtr<ID3D12Device> command_device;
    ComPtr<ID3D12Device> queue_device;
    HRESULT result = source->GetDevice(
        IID_PPV_ARGS(source_device.GetAddressOf()));
    if (FAILED(result)) return result;
    result = producer_command_list->GetDevice(
        IID_PPV_ARGS(command_device.GetAddressOf()));
    if (FAILED(result)) return result;
    result = producer_queue->GetDevice(
        IID_PPV_ARGS(queue_device.GetAddressOf()));
    if (FAILED(result)) return result;
    ComPtr<ID3D12Device> allocation_device;
    if (verified_producer_device != nullptr) {
        // The in-process producer has already unwrapped and identity-checked
        // the devices returned by every resource/list/queue. Comparing the
        // reported wrapper identities here would recreate the Streamline false
        // mismatch this field exists to avoid.
        allocation_device = verified_producer_device;
    } else {
        if (!same_identity(source_device.Get(), command_device.Get()) ||
            !same_identity(source_device.Get(), queue_device.Get())) {
            return E_INVALIDARG;
        }
        allocation_device = source_device;
    }

    D3D12_RESOURCE_DESC description = source->GetDesc();
    // The observed NGX SR/RR MotionVectors input has the documented float-XY
    // contract. Dead Space allocates that field as RG16_TYPELESS. Preserve its
    // bits in the existing owned copy using the compatible RG16_FLOAT format;
    // never change the game resource or relax generic typeless SRV rejection.
    if (dlss_float_motion && description.Format == DXGI_FORMAT_R16G16_TYPELESS &&
        description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        description.DepthOrArraySize == 1 && description.SampleDesc.Count == 1 &&
        !(description.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) {
        description.Format = dlss_motion_snapshot_format(description.Format, true);
    }
    // Ordinary producers keep four slots; direct receipts and Luke use their
    // explicit bounds. Never overwrite a pending producer, CPU pin or GPU read.
    // No producer wait/flush is introduced at recording time.
    const std::size_t capacity = retention ? std::min(retained_capacity,SnapshotCount) : 4;
    std::size_t selected = capacity;
    for (std::size_t offset = 0; offset < capacity; ++offset) {
        const auto index = (next_snapshot + offset) % capacity;
        const auto& use = snapshots[index].retention;
        if (writer_complete(snapshots[index]) &&
            (!use || (use.use_count() == 1 && use->reusable()))) { selected = index; break; }
    }
    if (selected == capacity) {
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    std::shared_ptr<DlssSnapshotRetention> lifetime;
    if (retention) {
        try { lifetime = std::make_shared<DlssSnapshotRetention>(); }
        catch (...) { return E_OUTOFMEMORY; }
    }
    SnapshotSlot& slot = snapshots[selected];
    if (!slot.resource || !matching_description(slot.description, description) ||
        !same_identity(slot.allocation_device.Get(), allocation_device.Get())) {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        properties.CreationNodeMask = 1;
        properties.VisibleNodeMask = 1;
        ComPtr<ID3D12Resource> resource;
        result = allocation_device->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(resource.GetAddressOf()));
        if (FAILED(result)) return result;
        slot.resource = std::move(resource);
        slot.allocation_device = allocation_device;
        slot.description = description;
        slot.state = D3D12_RESOURCE_STATE_COMMON;
    }

    std::array<D3D12_RESOURCE_BARRIER, 2> before{};
    UINT before_count = 0;
    if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        before[before_count++] = transition_barrier(
            source,
            source_state,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    if (slot.state != D3D12_RESOURCE_STATE_COPY_DEST) {
        before[before_count++] = transition_barrier(
            slot.resource.Get(), slot.state, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (before_count != 0) {
        producer_command_list->ResourceBarrier(before_count, before.data());
    }
    producer_command_list->CopyResource(slot.resource.Get(), source);

    std::array<D3D12_RESOURCE_BARRIER, 2> after{};
    UINT after_count = 0;
    if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        after[after_count++] = transition_barrier(
            source,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            source_state);
    }
    after[after_count++] = transition_barrier(
        slot.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, snapshot_state);
    producer_command_list->ResourceBarrier(after_count, after.data());
    slot.state = snapshot_state;
    *snapshot = slot.resource;
    if (allocation_receipt) *allocation_receipt = slot.allocation_device;
    slot.retention = std::move(lifetime);
    slot.producer_command_list=producer_command_list;
    slot.producer_identity=list_identity.Get();
    slot.producer_fence.Reset();slot.producer_fence_value=0;
    slot.producer_canceled=false;slot.writer_unknown=false;slot.submission_started=false;
    g_cancelable_snapshots.fetch_add(1,std::memory_order_release);
    if (retention) *retention = slot.retention;
    next_snapshot = (selected + 1) % capacity;
    return S_OK;
}

struct Candidate {
    std::uint64_t first_publication{};
    std::shared_ptr<const DlssMotionVectorFrame> frame;
    std::shared_ptr<const DlssMotionVectorFrame> previous;
    std::shared_ptr<const DlssMotionVectorFrame> older;
};

// Diagnostic-only resolver receipts: at most 64 groups, once per second for
// each refusal. No frame/thread wait or admission policy is changed here.
// 1100 reason/token/sample; 1101 owner/epoch; 1102 retained token bounds;
// 1103 current/previous tokens + history count; 1104 epochs; 1105 serials;
// 1106 publications; 1107 unsubmitted lists; 1108 fence values; 1109 fences;
// 1110 canonical consumer device/queue; 1111 motion owners; 1112 depth owners;
// 1113 fence owners; 1114 producer queues; 1115 Wait HRESULT/failed publication.
void report_luke_token_refusal(unsigned reason,std::uint32_t token,
    const Registry& registry,const StreamState* owner,std::uint64_t stream_id,
    const DlssMotionVectorFrame* current,const DlssMotionVectorFrame* previous,
    ID3D12CommandQueue* queue,ID3D12Device* device,HRESULT wait=S_OK,
    std::uint64_t failed_publication=0) noexcept {
    try {
        auto& logger=bridge_flight_logger();
        if(!logger.enabled()&&!logger.ram_enabled())return;
        static std::atomic_uint reports{};
        static std::array<std::atomic_uint64_t,15> deadlines{};
        if(reason>=deadlines.size()||reports.load(std::memory_order_relaxed)>=64)return;
        const auto now=GetTickCount64();auto before=deadlines[reason].load(std::memory_order_relaxed);
        if(now<before||!deadlines[reason].compare_exchange_strong(before,now+1000,std::memory_order_relaxed))return;
        const auto sample=reports.fetch_add(1,std::memory_order_relaxed)+1;if(sample>64)return;
        std::uint32_t oldest{},newest{};
        const auto bounds=[&](const StreamState& candidate) {
            for(const auto& origin:candidate.luke_origin_history) {
                const auto value=origin->color_origin_token;
                oldest=oldest?std::min(oldest,value):value;newest=std::max(newest,value);
            }
        };
        if(owner)bounds(*owner);else for(const auto& [unused,candidate]:registry.streams)bounds(candidate);
        const auto address=[](IUnknown* object){return reinterpret_cast<std::uint64_t>(object);};
        const auto canonical=[&](IUnknown* object){return address(identity(object).Get());};
        const auto event=[&](unsigned stage,std::uint64_t a,std::uint64_t b) {
            logger.event(BridgeFlightOperation::luke_afw_stage,sample,stage,a,b);
        };
        logger.event(BridgeFlightOperation::luke_afw_stage,reason,1100,token,sample);
        event(1101,stream_id,owner?owner->epoch:0);event(1102,oldest,newest);
        event(1103,(std::uint64_t(current?current->color_origin_token:0)<<32)|
            (previous?previous->color_origin_token:0),owner?owner->luke_origin_history.size():0);
        event(1104,current?current->epoch:0,previous?previous->epoch:0);
        event(1105,current?current->serial:0,previous?previous->serial:0);
        event(1106,current?current->publication:0,previous?previous->publication:0);
        event(1107,current?address(current->producer_command_list.Get()):0,previous?address(previous->producer_command_list.Get()):0);
        event(1108,current?current->producer_fence_value:0,previous?previous->producer_fence_value:0);
        event(1109,current?address(current->producer_fence.Get()):0,previous?address(previous->producer_fence.Get()):0);
        event(1110,canonical(device),canonical(queue));
        event(1111,current?canonical(current->motion_snapshot_device.Get()):0,previous?canonical(previous->motion_snapshot_device.Get()):0);
        event(1112,current?canonical(current->depth_snapshot_device.Get()):0,previous?canonical(previous->depth_snapshot_device.Get()):0);
        event(1113,current?canonical(current->producer_fence_device.Get()):0,previous?canonical(previous->producer_fence_device.Get()):0);
        event(1114,current?canonical(current->producer_queue.Get()):0,previous?canonical(previous->producer_queue.Get()):0);
        logger.event(BridgeFlightOperation::luke_afw_stage,wait,1115,sample,failed_publication);
    }catch(...){}
}

// Bounded F9 receipts retain the actual admitted camera/guide contract without
// requiring another image capture. This observes values only; no GPU work,
// temporal policy or camera/motion conversion is performed here.
void report_luke_ingress_contract(const DlssMotionVectorFrame& frame) noexcept {
    try {
        auto& logger=bridge_flight_logger();
        if(!frame.color_origin_token||(!logger.enabled()&&!logger.ram_enabled()))return;
        static std::atomic_uint reports{};
        static std::atomic_uint64_t deadline{};
        const auto now=GetTickCount64();auto before=deadline.load(std::memory_order_relaxed);
        if(reports.load(std::memory_order_relaxed)>=64||now<before||
            !deadline.compare_exchange_strong(before,now+5000,std::memory_order_relaxed))return;
        const auto sample=reports.fetch_add(1,std::memory_order_relaxed)+1;
        if(sample>64)return;
        const auto pair=[](float a,float b) {
            return (std::uint64_t(std::bit_cast<std::uint32_t>(a))<<32)|std::bit_cast<std::uint32_t>(b);
        };
        const auto extent=[](std::uint32_t a,std::uint32_t b){return (std::uint64_t(a)<<32)|b;};
        const auto event=[&](unsigned stage,std::uint64_t a,std::uint64_t b) {
            logger.event(BridgeFlightOperation::luke_afw_stage,sample,stage,a,b);
        };
        event(1160,frame.stream,frame.native_production_sequence);
        event(1161,extent(frame.camera_frame_token,frame.color_origin_token),frame.publication);
        event(1162,extent(frame.output_width,frame.output_height),extent(frame.motion_width,frame.motion_height));
        event(1163,extent(frame.depth_width,frame.depth_height),pair(frame.scale_x,frame.scale_y));
        event(1164,pair(frame.jitter_x,frame.jitter_y),pair(frame.frame_time_delta_ms,frame.camera_fov_vertical));
        event(1165,pair(frame.camera_near,frame.camera_far),std::uint64_t(frame.reset)|
            (std::uint64_t(frame.jittered)<<1)|(std::uint64_t(frame.depth_inverted)<<2)|
            (std::uint64_t(frame.depth_infinite)<<3)|(std::uint64_t(frame.camera_data_valid)<<4)|
            (std::uint64_t(frame.camera_matrices_valid)<<5)|
            (std::uint64_t(static_cast<std::uint32_t>(frame.raw_camera_motion_included))<<8));
        event(1166,pair(frame.camera_position[0],frame.camera_position[1]),pair(frame.camera_position[2],frame.calibrated_game_eye_baseline));
        event(1167,pair(frame.camera_right[0],frame.camera_right[1]),pair(frame.camera_right[2],frame.camera_up[0]));
        event(1168,pair(frame.camera_up[1],frame.camera_up[2]),pair(frame.camera_forward[0],frame.camera_forward[1]));
        event(1169,pair(frame.camera_forward[2],0),extent(frame.source_motion_x,frame.source_depth_x));
        for(unsigned i=0;i<4;++i) {
            const auto base=i*4;
            event(1170+i,pair(frame.camera_view_to_clip[base],frame.camera_view_to_clip[base+1]),
                pair(frame.camera_view_to_clip[base+2],frame.camera_view_to_clip[base+3]));
            event(1174+i,pair(frame.raw_clip_to_previous_clip[base],frame.raw_clip_to_previous_clip[base+1]),
                pair(frame.raw_clip_to_previous_clip[base+2],frame.raw_clip_to_previous_clip[base+3]));
        }
    }catch(...){}
}

// Where an eye's DLSS inputs sit in their render targets. Both eyes often
// share one double-wide target with the left eye on the left - UEVR's depth,
// for one - so the eye further left is eye 0 whichever evaluates first.
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> input_offset(
    const DlssMotionVectorFrame& frame) noexcept {
    return {frame.depth ? frame.source_depth_x : 0U, frame.source_motion_x};
}

}  // namespace

std::shared_ptr<const DlssMotionVectorFrame> publish_dlss_motion_vectors(
    const DlssMotionVectorPublication& publication) noexcept {
    try {
        if (!g_tracking_enabled.load(std::memory_order_acquire)) return {};
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        // Disable can win after the fast check but before this lock. No late
        // publisher may repopulate a registry whose retirement already ended.
        if(!g_tracking_enabled.load(std::memory_order_acquire))return {};
        if (!valid(publication)) {
            ++state.statistics.invalid_publications;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return {};
        }

        const std::uint32_t color_origin_token = publication.protect_snapshot_lifetime
            ? state.color_origin_tokens.allocate() : 0;
        if (publication.protect_snapshot_lifetime && !color_origin_token) {
            ++state.statistics.invalid_publications;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return {};
        }

        auto& stream = state.streams[publication.stream];
        const auto& last=stream.latest;
        const bool resized=last && (last->output_width!=publication.output_width ||
            last->output_height!=publication.output_height ||
            last->motion_width!=publication.motion_width || last->motion_height!=publication.motion_height ||
            last->depth_width!=publication.depth_width || last->depth_height!=publication.depth_height);
        const bool reset=publication.reset || resized;
        if (reset) {
            ++stream.epoch;
            stream.serial = 0;
            stream.previous.reset();
            stream.previous2.reset();
            stream.latest.reset();
            // These immutable records can still be named by a color already
            // in flight. The same bounded ring/reader ownership prevents reuse.
            if (!stream.retain_reset_origins) stream.luke_origin_history.clear();
        }
        ComPtr<ID3D12Resource> output_snapshot;
        if (publication.snapshot_output) {
            if (FAILED(snapshot_resource(stream.output_snapshots,
                    stream.next_output_snapshot, publication.output,
                    publication.output_resource_state,
                    publication.producer_command_list,
                    publication.producer_queue,
                    publication.verified_producer_device,
                    &output_snapshot,
                    D3D12_RESOURCE_STATE_COMMON))) {
                ++state.statistics.snapshot_failures;
                state.statistics.status = DlssMotionVectorStatus::invalid_input;
                return {};
            }
            ++state.statistics.snapshot_copies;
        }

        ComPtr<ID3D12Resource> motion_snapshot;
        ComPtr<ID3D12Device> motion_snapshot_device, depth_snapshot_device;
        std::shared_ptr<DlssSnapshotRetention> motion_retention, depth_retention;
        const bool retain_snapshots=publication.protect_snapshot_lifetime || publication.direct_output_receipt;
        const std::size_t retained_capacity=publication.direct_output_receipt ? 6 : kSnapshotSlotCount;
        if (FAILED(snapshot_resource(stream.snapshots, stream.next_snapshot,
                publication.motion_vectors, publication.resource_state,
                publication.producer_command_list, publication.producer_queue,
                publication.verified_producer_device,
                &motion_snapshot, kSnapshotReadState,
                retain_snapshots ? &motion_retention : nullptr,
                &motion_snapshot_device,retained_capacity,publication.dlss_float_motion))) {
            ++state.statistics.snapshot_failures;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return {};
        }
        ++state.statistics.snapshot_copies;

        ComPtr<ID3D12Resource> depth_snapshot;
        if (publication.depth != nullptr) {
            if (FAILED(snapshot_resource(stream.depth_snapshots,
                    stream.next_depth_snapshot, publication.depth,
                    publication.depth_resource_state,
                    publication.producer_command_list,
                    publication.producer_queue,
                    publication.verified_producer_device, &depth_snapshot, kSnapshotReadState,
                    retain_snapshots ? &depth_retention : nullptr,
                    &depth_snapshot_device,retained_capacity))) {
                ++state.statistics.snapshot_failures;
                state.statistics.status = DlssMotionVectorStatus::invalid_input;
                return {};
            }
            ++state.statistics.snapshot_copies;
            ++state.statistics.depth_publications;
        }

        auto frame = std::make_shared<DlssMotionVectorFrame>();
        frame->stream = publication.stream;
        frame->epoch = stream.epoch;
        frame->previous_serial = stream.serial;
        frame->serial = ++stream.serial;
        if (state.next_publication == 0) {
            ++state.statistics.invalid_publications;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return {};
        }
        frame->publication = state.next_publication++;
        frame->color_origin_token = color_origin_token;
        ++state.statistics.published;
        frame->output = publication.snapshot_output
            ? std::move(output_snapshot)
            : publication.output;
        frame->motion_vectors = std::move(motion_snapshot);
        frame->motion_snapshot_device = std::move(motion_snapshot_device);
        frame->depth_snapshot_device = std::move(depth_snapshot_device);
        frame->producer_queue = publication.producer_queue;
        frame->verified_producer_device =
            publication.verified_producer_device;
        frame->output_x = publication.output_x;
        frame->output_y = publication.output_y;
        frame->output_width = publication.output_width;
        frame->output_height = publication.output_height;
        frame->output_slice = 0;
        frame->motion_x = publication.motion_x;
        frame->motion_y = publication.motion_y;
        frame->source_motion_x = publication.motion_x;
        frame->motion_width = publication.motion_width;
        frame->motion_height = publication.motion_height;
        frame->motion_slice = 0;
        frame->scale_x = publication.scale_x == 0.0F ? 1.0F : publication.scale_x;
        frame->scale_y = publication.scale_y == 0.0F ? 1.0F : publication.scale_y;
        frame->jitter_x = publication.jitter_x;
        frame->jitter_y = publication.jitter_y;
        frame->previous_jitter_x = stream.jitter_x;
        frame->previous_jitter_y = stream.jitter_y;
        frame->resource_state = kSnapshotReadState;
        frame->jittered = publication.jittered;
        frame->reset = reset;
        frame->depth = std::move(depth_snapshot);
        frame->motion_retention = std::move(motion_retention);
        frame->depth_retention = std::move(depth_retention);
        frame->camera_frame_token = publication.camera_frame_token;
        frame->camera_frame_token_valid = publication.camera_frame_token_valid;
        frame->native_production_sequence = publication.native_production_sequence;
        frame->raw_motion_native_span = publication.raw_motion_native_span;
        frame->compose_native_object_history = publication.compose_native_object_history;
        frame->gta_headlock=publication.gta_headlock;
        frame->depth_x = publication.depth_x;
        frame->depth_y = publication.depth_y;
        frame->source_depth_x = publication.depth_x;
        frame->depth_width = publication.depth_width;
        frame->depth_height = publication.depth_height;
        frame->depth_resource_state = kSnapshotReadState;
        frame->frame_time_delta_ms = publication.frame_time_delta_ms;
        frame->camera_near = publication.camera_near;
        frame->camera_far = publication.camera_far;
        frame->depth_inverted = publication.depth_inverted;
        frame->depth_infinite = publication.depth_infinite;
        frame->camera_position = publication.camera_position;
        frame->camera_up = publication.camera_up;
        frame->camera_right = publication.camera_right;
        frame->camera_forward = publication.camera_forward;
        frame->camera_fov_vertical = publication.camera_fov_vertical;
        frame->camera_data_valid = publication.camera_data_valid;
        frame->camera_view_to_clip = publication.camera_view_to_clip;
        frame->camera_clip_to_view = publication.camera_clip_to_view;
        frame->camera_matrices_valid = publication.camera_matrices_valid;
        frame->raw_camera_motion_included = publication.raw_camera_motion_included;
        frame->raw_clip_to_previous_clip = publication.raw_clip_to_previous_clip;
        frame->raw_previous_clip_to_clip = publication.raw_previous_clip_to_clip;
        frame->producer_command_list = publication.producer_command_list;
        const bool may_interleave = publication.protect_snapshot_lifetime &&
            !frame->native_production_sequence && frame->publication >= 3 && stream.latest &&
            stream.latest->publication + 1 != frame->publication;
        const auto interleaved_middle = may_interleave
            ? protected_publication(state, frame->publication - 1) : nullptr;
        const auto interleaved_older = may_interleave
            ? protected_publication(state, frame->publication - 2) : nullptr;
        const bool interleaved = interleaved_frames(*frame, interleaved_middle, interleaved_older);
        if (publication.protect_snapshot_lifetime) {
            // Calibrate exactly once in immutable production order. Consumer
            // queries may lag, repeat or skip; none may select newer cameras
            // to calibrate an older color-carried identity.
            const auto camera_sample = [](const auto& value) {
                return LukeUnitCamera{value->camera_position, value->camera_right,
                    value->frame_time_delta_ms, value->camera_data_valid && !value->reset};
            };
            const auto& older = interleaved ? interleaved_older : stream.previous;
            const auto& middle = interleaved ? interleaved_middle : stream.latest;
            const bool consecutive = interleaved || (older && middle &&
                older->stream == frame->stream && middle->stream == frame->stream &&
                older->epoch == frame->epoch && middle->epoch == frame->epoch &&
                older->serial + 1 == middle->serial && middle->serial + 1 == frame->serial &&
                older->publication + 1 == middle->publication &&
                middle->publication + 1 == frame->publication &&
                older->color_origin_token != 0 &&
                std::uint64_t(older->color_origin_token) + 1 == middle->color_origin_token &&
                std::uint64_t(middle->color_origin_token) + 1 == frame->color_origin_token);
            const float sample = consecutive
                ? luke_camera_separation(camera_sample(older), camera_sample(middle), camera_sample(frame)) : 0;
            frame->calibrated_game_eye_baseline = stream.retain_reset_origins
                ? stream.scene_camera_units.observe(frame->epoch,frame->publication,sample)
                : stream.camera_units.observe(frame->epoch,frame->publication,sample);
        }
        if (stream.previous && stream.previous->epoch == frame->epoch &&
            stream.previous->serial + 2 == frame->serial &&
            stream.previous->publication + 2 == frame->publication) {
            // Exact N-2 scalar camera survives eviction of that GPU snapshot.
            frame->previous_same_native_camera=dlss_native_camera_history(*stream.previous);
        }
        if (interleaved)
            frame->previous_same_native_camera = dlss_native_camera_history(*interleaved_older);
        report_luke_ingress_contract(*frame);
        if (two_frame_capture_active()) {
            TwoFrameGuideContract contract{};
            contract.stage = "ngx_ingress";
            contract.native_camera_token = frame->camera_frame_token;
            contract.color_origin_token = frame->color_origin_token;
            contract.native_production_sequence = frame->native_production_sequence;
            contract.stream = frame->stream;
            contract.publication = frame->publication;
            contract.serial = frame->serial;
            contract.previous_serial = frame->previous_serial;
            contract.motion_width = frame->motion_width;
            contract.motion_height = frame->motion_height;
            contract.depth_width = frame->depth_width;
            contract.depth_height = frame->depth_height;
            contract.output_width = frame->output_width;
            contract.output_height = frame->output_height;
            contract.scale_x = frame->scale_x;
            contract.scale_y = frame->scale_y;
            contract.jitter_x = frame->jitter_x;
            contract.jitter_y = frame->jitter_y;
            contract.frame_time_delta_ms = frame->frame_time_delta_ms;
            contract.camera_near = frame->camera_near;
            contract.camera_far = frame->camera_far;
            contract.reset = frame->reset;
            contract.jittered = frame->jittered;
            contract.depth_inverted = frame->depth_inverted;
            contract.depth_infinite = frame->depth_infinite;
            contract.camera_position = frame->camera_position;
            contract.camera_right = frame->camera_right;
            contract.camera_up = frame->camera_up;
            contract.camera_forward = frame->camera_forward;
            contract.camera_fov_vertical = frame->camera_fov_vertical;
            contract.view_to_clip = frame->camera_view_to_clip;
            contract.raw_camera_motion_included = frame->raw_camera_motion_included;
            contract.raw_motion_native_span = frame->raw_motion_native_span;
            contract.raw_clip_to_previous_clip = frame->raw_clip_to_previous_clip;
            contract.raw_previous_clip_to_clip = frame->raw_previous_clip_to_clip;
            contract.clip_to_view = frame->camera_clip_to_view;
            two_frame_capture_guide_contract(contract);
        }
        stream.jitter_x = publication.jitter_x;
        stream.jitter_y = publication.jitter_y;
        if (stream.first_publication == 0) {
            stream.first_publication = frame->publication;
        }
        stream.previous2 = std::move(stream.previous);
        stream.previous = std::move(stream.latest);
        stream.latest = std::move(frame);
        if (publication.protect_snapshot_lifetime) {
            stream.luke_origin_history.push_back(stream.latest);
            while (stream.luke_origin_history.size() > kLukeOriginHistoryCount)
                stream.luke_origin_history.pop_front();
        } else {
            stream.luke_origin_history.clear();
        }
        if (state.statistics.used == 0) {
            state.statistics.status = DlssMotionVectorStatus::output_not_direct;
        }
        return stream.latest;
    } catch (...) {
        report_dlss_motion_vector_status(DlssMotionVectorStatus::invalid_input);
        return {};
    }
}

void configure_dlss_motion_vector_tracking(bool enabled) noexcept {
    const bool previous = g_tracking_enabled.exchange(enabled, std::memory_order_acq_rel);
    if (enabled == previous) return;
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        if (!enabled) {
            // The layer's drained consumer boundary requests retirement. The
            // per-slot writer receipt will still protect copies recorded on an
            // application list that has not actually executed yet.
            g_retirement_pending.store(!state.streams.empty() || !state.retired_streams.empty(),
                std::memory_order_release);
            state.statistics.status = DlssMotionVectorStatus::disabled;
            return;
        }
        // Re-enabling without a layer-side retirement must not expose stale
        // vectors. Preserve the slot resources (and therefore any in-flight GPU
        // lifetime), but start fresh stream epochs and temporal ownership.
        for (auto& [unused_stream_id, stream] : state.streams) {
            static_cast<void>(unused_stream_id);
            ++stream.epoch;
            stream.serial = 0;
            stream.first_publication = 0;
            stream.jitter_x = 0.0F;
            stream.jitter_y = 0.0F;
            stream.previous.reset();
            stream.previous2.reset();
            stream.latest.reset();
            stream.luke_origin_history.clear();
        }
        state.statistics = {};
        state.statistics.status = DlssMotionVectorStatus::waiting_for_dlss;
        collect_retired(state);
    } catch (...) {
    }
}

void retire_disabled_dlss_motion_vector_resources() noexcept {
    if(!g_retirement_pending.load(std::memory_order_acquire))return;
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        if (!g_tracking_enabled.load(std::memory_order_acquire)) {
            state.retired_streams.reserve(state.retired_streams.size()+state.streams.size());
            for(auto& [unused,stream]:state.streams)state.retired_streams.push_back(std::move(stream));
            state.streams.clear();
            state.statistics = {};
            state.statistics.status = DlssMotionVectorStatus::disabled;
        }
        collect_retired(state);
    } catch (...) {
    }
}

void retire_dlss_motion_vector_stream(std::uint64_t stream) noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        state.retired_streams.reserve(state.retired_streams.size()+1);
        auto retired = state.streams.extract(stream);
        if (!retired.empty()) {
            state.retired_streams.push_back(std::move(retired.mapped()));
            g_retirement_pending.store(true,std::memory_order_release);
            collect_retired(state);
        }
    } catch (...) {
    }
}

void invalidate_dlss_motion_vector_stream_history(std::uint64_t stream_id) noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        const auto found = state.streams.find(stream_id);
        if (found == state.streams.end()) return;
        auto& stream = found->second;
        ++stream.epoch;
        stream.serial = 0;
        stream.first_publication = 0;
        stream.jitter_x = stream.jitter_y = 0;
        stream.previous.reset();
        stream.previous2.reset();
        stream.latest.reset();
        stream.luke_origin_history.clear();
        state.statistics.status = DlssMotionVectorStatus::temporal_mismatch;
    } catch (...) {
    }
}

void begin_dlss_motion_vector_submission(std::uint32_t count,ID3D12CommandList* const* lists) noexcept {
    if(!count || !lists || !g_cancelable_snapshots.load(std::memory_order_acquire))return;
    try {
        auto& state=registry();std::scoped_lock lock(state.mutex);
        bool marked=false;
        const auto visit=[&](const auto& operation) {
            const auto stream=[&](StreamState& value) {
                for(auto& slot:value.output_snapshots)operation(slot);
                for(auto& slot:value.snapshots)operation(slot);
                for(auto& slot:value.depth_snapshots)operation(slot);
            };
            for(auto& [unused,value]:state.streams)stream(value);
            for(auto& value:state.retired_streams)stream(value);
        };
        // No temporary vectors/allocations may fail before the original Execute.
        // Each supplied identity is queried once and compared to cached slot IDs.
        for(std::uint32_t index=0;index<count;++index) {
            const auto canonical=identity(lists[index]);
            if(!canonical) {
                // An unidentified input might own any pending copy. Keep every
                // such resource unknown, never cancelable or receipt-admissible.
                visit([&](SnapshotSlot& slot) {
                    if(slot.resource && slot.producer_command_list) {
                        if(!slot.writer_unknown) {
                            slot.writer_unknown=true;g_cancelable_snapshots.fetch_sub(1,std::memory_order_release);
                        }
                        slot.submission_started=false;
                    }
                });
                return;
            }
            visit([&](SnapshotSlot& slot) {
                if(slot.resource && !slot.writer_unknown && slot.producer_command_list && slot.producer_identity==canonical.Get()) {
                    slot.writer_unknown=true;slot.submission_started=true;
                    g_cancelable_snapshots.fetch_sub(1,std::memory_order_release);
                    marked=true;
                }
            });
        }
        if(marked)g_submission_ever_started.store(true,std::memory_order_release);
    }catch(...){}
}

void abandon_dlss_motion_vector_submission(std::uint32_t count,ID3D12CommandList* const* lists) noexcept {
    // Do not even lazily construct the registry before the first actual marker.
    if(!count || !lists || !g_submission_ever_started.load(std::memory_order_acquire))return;
    try {
        auto& state=registry();std::scoped_lock lock(state.mutex);
        const auto visit=[&](const auto& operation) {
            const auto stream=[&](StreamState& value) {
                for(auto& slot:value.output_snapshots)operation(slot);
                for(auto& slot:value.snapshots)operation(slot);
                for(auto& slot:value.depth_snapshots)operation(slot);
            };
            for(auto& [unused,value]:state.streams)stream(value);
            for(auto& value:state.retired_streams)stream(value);
        };
        for(std::uint32_t index=0;index<count;++index) {
            const auto canonical=identity(lists[index]);
            visit([&](SnapshotSlot& slot) {
                if(slot.writer_unknown && slot.submission_started && slot.producer_command_list &&
                    (!canonical || slot.producer_identity==canonical.Get()))slot.submission_started=false;
            });
            if(!canonical)return; // unidentified failure conservatively seals every started attempt
        }
    }catch(...){}
}

void report_dlss_motion_vector_reset(ID3D12GraphicsCommandList* list) noexcept {
    if(!list || !g_cancelable_snapshots.load(std::memory_order_acquire))return;
    try {
        const auto reset_identity=identity(list);if(!reset_identity)return;
        auto& state=registry();std::scoped_lock lock(state.mutex);
        const auto contains=[&](const auto& slots) {
            return std::any_of(slots.begin(),slots.end(),[&](const auto& slot) {
                return slot.resource && !slot.writer_unknown && slot.producer_command_list &&
                    slot.producer_identity==reset_identity.Get();
            });
        };
        const auto affected=[&](const StreamState& stream) {
            return contains(stream.output_snapshots) || contains(stream.snapshots) || contains(stream.depth_snapshots);
        };
        const auto cancel=[&](StreamState& stream) {
            const auto slots=[&](auto& values) {
                for(auto& slot:values)if(slot.resource && !slot.writer_unknown && slot.producer_command_list &&
                    slot.producer_identity==reset_identity.Get()) {
                    slot.producer_identity=nullptr;slot.producer_command_list.Reset();slot.producer_fence.Reset();slot.producer_fence_value=0;
                    slot.producer_canceled=true;g_cancelable_snapshots.fetch_sub(1,std::memory_order_release);
                }
            };
            slots(stream.output_snapshots);slots(stream.snapshots);slots(stream.depth_snapshots);
        };
        const auto affected_count=std::count_if(state.streams.begin(),state.streams.end(),
            [&](const auto& entry){return affected(entry.second);});
        if(!affected_count && std::none_of(state.retired_streams.begin(),state.retired_streams.end(),affected))return;
        state.retired_streams.reserve(state.retired_streams.size()+affected_count);
        for(auto& [unused,stream]:state.streams)if(affected(stream)) {
            const auto next_epoch=stream.epoch+1;
            cancel(stream);
            state.retired_streams.push_back(std::move(stream));
            stream=StreamState{};stream.epoch=next_epoch;
        }
        for(auto& stream:state.retired_streams)cancel(stream);
        collect_retired(state);
    }catch(...){}
}

void report_dlss_motion_vector_submission(
    ID3D12CommandQueue* queue,
    std::uint32_t command_list_count,
    ID3D12CommandList* const* command_lists,bool began_before_execute) noexcept {
    if(!command_list_count || !command_lists)return;
    // Legacy/manual reporters have already executed their lists. Mark them
    // without allocation before any receipt bookkeeping can throw. Native
    // reporters consume ONLY the exact pre-call markers, never copies recorded
    // reentrantly on a reset list before the outer Execute returns.
    if(!began_before_execute)begin_dlss_motion_vector_submission(command_list_count,command_lists);
    if(!queue){abandon_dlss_motion_vector_submission(command_list_count,command_lists);return;}
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        std::vector<ComPtr<IUnknown>> identities;
        bool identities_ready=false;
        const auto list_submitted = [&](IUnknown* canonical) {
            if(!canonical)return false;
            if(!identities_ready){identities=command_identities(command_list_count,command_lists);identities_ready=true;}
            return contains_identity(identities,canonical);
        };
        const auto process_stream = [&](StreamState& stream) {
            std::vector<SnapshotSlot*> submitted_slots;
            const auto inspect_slots=[&](auto& slots) {
                for(auto& slot:slots)if(slot.resource && slot.writer_unknown && slot.submission_started &&
                    slot.producer_command_list && list_submitted(slot.producer_identity))
                    submitted_slots.push_back(&slot);
            };
            inspect_slots(stream.output_snapshots);inspect_slots(stream.snapshots);inspect_slots(stream.depth_snapshots);
            for(auto* slot:submitted_slots) {
                // Any error after this point leaves a completed-attempt
                // quarantine. A later Reset or unrelated receipt cannot clear it.
                slot->submission_started=false;
            }
            const auto was_submitted = [&](const auto& frame) {
                if(!frame || !frame->producer_command_list || !list_submitted(identity(frame->producer_command_list.Get()).Get()))return false;
                const auto owns=[&](ID3D12Resource* resource) {
                    return std::any_of(submitted_slots.begin(),submitted_slots.end(),
                        [&](const auto* slot){return slot->resource.Get()==resource;});
                };
                return owns(frame->motion_vectors.Get()) && (!frame->depth || owns(frame->depth.Get()));
            };
            const bool latest_submitted = was_submitted(stream.latest);
            const bool previous_submitted = was_submitted(stream.previous);
            const bool previous2_submitted = was_submitted(stream.previous2);
            const bool origin_submitted = std::any_of(
                stream.luke_origin_history.begin(), stream.luke_origin_history.end(),
                was_submitted);
            if (!latest_submitted && !previous_submitted &&
                !previous2_submitted && !origin_submitted && submitted_slots.empty()) return;

            // The callback follows actual ExecuteCommandLists. Take the
            // allocation receipt from those submitted lists' snapshots;
            // queue.GetDevice may report a Streamline device proxy.
            ComPtr<ID3D12Device> submission_device;
            bool incompatible_devices = false;
            for(const auto* slot:submitted_slots) {
                if(!slot->allocation_device || (submission_device &&
                    !same_identity(submission_device.Get(),slot->allocation_device.Get())))incompatible_devices=true;
                else submission_device=slot->allocation_device;
            }
            const auto inspect_owner = [&](const auto& frame, bool submitted) {
                if (!submitted) return;
                if (!frame->motion_snapshot_device ||
                    (submission_device && !same_identity(submission_device.Get(),
                        frame->motion_snapshot_device.Get())) ||
                    (frame->depth && !same_identity(frame->motion_snapshot_device.Get(),
                        frame->depth_snapshot_device.Get()))) {
                    incompatible_devices = true;
                    return;
                }
                submission_device = frame->motion_snapshot_device;
            };
            inspect_owner(stream.latest, latest_submitted);
            inspect_owner(stream.previous, previous_submitted);
            inspect_owner(stream.previous2, previous2_submitted);
            for (const auto& origin : stream.luke_origin_history)
                inspect_owner(origin, was_submitted(origin));
            if (!submission_device || incompatible_devices) {
                ++state.statistics.invalid_rejections;
                state.statistics.status = DlssMotionVectorStatus::invalid_input;
                return;
            }
            const auto queue_identity=identity(queue);
            if (!stream.submission_fence || !same_identity(
                    stream.submission_fence_device.Get(), submission_device.Get()) ||
                stream.submission_fence_queue.Get()!=queue_identity.Get()) {
                ComPtr<ID3D12Fence> fence;
                if (FAILED(submission_device->CreateFence(
                        0, D3D12_FENCE_FLAG_NONE,
                        IID_PPV_ARGS(fence.GetAddressOf())))) {
                    ++state.statistics.invalid_rejections;
                    state.statistics.status = DlssMotionVectorStatus::invalid_input;
                    return;
                }
                stream.submission_fence = std::move(fence);
                stream.submission_fence_device = submission_device;
                stream.submission_fence_queue = queue_identity;
                stream.next_submission_fence_value = 1;
            }

            const std::uint64_t fence_value =
                stream.next_submission_fence_value++;
            if (FAILED(queue->Signal(stream.submission_fence.Get(), fence_value))) {
                ++state.statistics.invalid_rejections;
                state.statistics.status = DlssMotionVectorStatus::invalid_input;
                return;
            }

            for(auto* slot:submitted_slots) {
                slot->producer_fence=stream.submission_fence;
                slot->producer_fence_value=fence_value;
                slot->producer_identity=nullptr;
                slot->producer_command_list.Reset();
                slot->writer_unknown=false;
            }

            std::unordered_map<std::uint64_t,
                std::shared_ptr<const DlssMotionVectorFrame>> updated_frames;
            const auto stamp_submission = [&](auto& stored, bool submitted) {
                if (!submitted) return;
                const auto existing = updated_frames.find(stored->publication);
                if (existing != updated_frames.end()) {
                    stored = existing->second;
                    return;
                }
                auto updated = std::make_shared<DlssMotionVectorFrame>(*stored);
                updated->producer_queue = queue;
                updated->producer_command_list.Reset();
                updated->producer_fence = stream.submission_fence;
                updated->producer_fence_device = stream.submission_fence_device;
                updated->producer_fence_value = fence_value;
                updated_frames.emplace(updated->publication, updated);
                stored = std::move(updated);
            };
            stamp_submission(stream.latest, latest_submitted);
            stamp_submission(stream.previous, previous_submitted);
            stamp_submission(stream.previous2, previous2_submitted);
            for (auto& origin : stream.luke_origin_history)
                stamp_submission(origin, was_submitted(origin));
        };
        for(auto& [unused_stream_id,stream]:state.streams)process_stream(stream);
        for(auto& stream:state.retired_streams)process_stream(stream);
        if(g_retirement_pending.load(std::memory_order_acquire))collect_retired(state);
    } catch (...) {
        // The registry lock has unwound. This path allocates nothing and seals
        // pre-call markers even if receipt vectors/frame clones ran out of RAM.
        abandon_dlss_motion_vector_submission(command_list_count,command_lists);
        report_dlss_motion_vector_status(DlssMotionVectorStatus::invalid_input);
    }
}

std::shared_ptr<const DlssMotionVectorFrame> resolve_dlss_guide_publication(
    std::uint64_t stream_id, std::uint64_t publication,
    ID3D12CommandQueue* consumer) noexcept {
    try {
        if (!g_tracking_enabled.load(std::memory_order_acquire) || !publication || !consumer) return {};
        auto& state=registry(); std::scoped_lock lock(state.mutex);
        const auto found=state.streams.find(stream_id);
        if (found==state.streams.end()) return {};
        const auto& frame=found->second.latest;
        if (!frame || frame->publication!=publication || !frame->producer_fence || !frame->producer_fence_value ||
            !same_device(frame->producer_fence.Get(),consumer)) return {};
        if (!same_identity(frame->producer_queue.Get(),consumer) &&
            FAILED(consumer->Wait(frame->producer_fence.Get(),frame->producer_fence_value))) return {};
        return frame;
    } catch (...) { return {}; }
}

// The tangents of a view's image edges from its projection: x's left edge
// at NDC -1, its right at +1, the top row at y +1, the bottom at -1. Either
// vector convention: off-centre terms sit in the third row for row vectors
// (D3D, w = z * m[2][3]) and in the third column for column vectors (w =
// m[3][2] * z); w's sign is the handedness. A view direction with tangent t
// and depth d > 0 has z = sign(w) d, so NDC = (t m00 + sign(w) offset) /
// |w|, and t = (NDC |w| - sign(w) offset) / m00.
DlssViewGeometry dlss_view_geometry(const DlssMotionVectorFrame& frame) noexcept {
    if (!frame.camera_matrices_valid) return {};
    const auto m = [&](std::size_t row, std::size_t column) {
        return frame.camera_view_to_clip[row * 4U + column];
    };
    for (const float value : frame.camera_view_to_clip)
        if (!std::isfinite(value)) return {};
    constexpr float kZero = 1e-6F;
    if (std::fabs(m(3, 3)) > kZero || std::fabs(m(0, 0)) < kZero || std::fabs(m(1, 1)) < kZero)
        return {};
    const bool row_offsets = std::fabs(m(2, 0)) > kZero || std::fabs(m(2, 1)) > kZero;
    const bool column_offsets = std::fabs(m(0, 2)) > kZero || std::fabs(m(1, 2)) > kZero;
    float w = 0.0F, offset_x = 0.0F, offset_y = 0.0F;
    if (row_offsets && !column_offsets) {
        w = m(2, 3); offset_x = m(2, 0); offset_y = m(2, 1);
    } else if (column_offsets && !row_offsets) {
        w = m(3, 2); offset_x = m(0, 2); offset_y = m(1, 2);
    } else if (!row_offsets && !column_offsets) {
        // Symmetric: only |w| matters, the term of magnitude one.
        w = std::fabs(std::fabs(m(2, 3)) - 1.0F) < 1e-4F ? m(2, 3)
            : std::fabs(std::fabs(m(3, 2)) - 1.0F) < 1e-4F ? m(3, 2) : 0.0F;
    }
    if (std::fabs(w) < kZero) return {};
    const float sign = w > 0.0F ? 1.0F : -1.0F;
    const float magnitude = std::fabs(w);
    DlssViewGeometry view;
    view.known = true;
    view.tan_left = (-magnitude - sign * offset_x) / m(0, 0);
    view.tan_right = (magnitude - sign * offset_x) / m(0, 0);
    view.tan_up = (magnitude - sign * offset_y) / m(1, 1);
    view.tan_down = (-magnitude - sign * offset_y) / m(1, 1);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        view.position[axis] = std::isfinite(frame.camera_position[axis]) ? frame.camera_position[axis] : 0.0F;
        view.right[axis] = std::isfinite(frame.camera_right[axis]) ? frame.camera_right[axis] : 0.0F;
    }
    return view;
}

namespace {

// How far a guide's camera may fall short of an eye's field and still be
// taken to cover it: a hundredth of the field each side.
constexpr double kViewCoverTolerance = 0.01;
// A stream with a camera is recent while its newest evaluation is within
// this many publications of the newest of all: MSFS 2024's foveated
// rendering publishes four a frame, and a stream the game stopped
// evaluating (a menu's, the desktop mirror's) keeps its last frame.
constexpr std::uint64_t kMaximumViewPublicationGap = 8;

[[nodiscard]] double view_fit_error(const DlssViewGeometry& g, const DlssEyeView& eye) noexcept {
    return std::fabs(static_cast<double>(g.tan_left) - eye.tan_left) +
        std::fabs(static_cast<double>(g.tan_right) - eye.tan_right) +
        std::fabs(static_cast<double>(g.tan_up) - eye.tan_up) +
        std::fabs(static_cast<double>(g.tan_down) - eye.tan_down);
}

// Along the two cameras' mean right vector: the smaller is the left eye.
[[nodiscard]] double camera_side(const DlssViewGeometry& g, const std::array<float, 3>& right) noexcept {
    return static_cast<double>(g.position[0]) * right[0] +
        static_cast<double>(g.position[1]) * right[1] +
        static_cast<double>(g.position[2]) * right[2];
}

}  // namespace

std::shared_ptr<const DlssMotionVectorFrame> place_guide_in_view(
    const DlssMotionVectorFrame& frame,
    const DlssEyeView& view) noexcept {
    try {
        const DlssViewGeometry g = dlss_view_geometry(frame);
        const double eye_width = static_cast<double>(view.tan_right) - view.tan_left;
        const double eye_height = static_cast<double>(view.tan_up) - view.tan_down;
        if (!g.known || view.x < 0 || view.y < 0 || view.width == 0 || view.height == 0 ||
            !(eye_width > 0.0) || !(eye_height > 0.0) || frame.motion_width == 0 ||
            frame.motion_height == 0) {
            return {};
        }
        // The guide's edges in the image's pixels.
        const double x0 = view.x, y0 = view.y;
        const double x1 = x0 + view.width, y1 = y0 + view.height;
        const double left = x0 + (g.tan_left - view.tan_left) / eye_width * view.width;
        const double right = x0 + (g.tan_right - view.tan_left) / eye_width * view.width;
        const double top = y0 + (view.tan_up - g.tan_up) / eye_height * view.height;
        const double bottom = y0 + (view.tan_up - g.tan_down) / eye_height * view.height;
        const double slack_x = kViewCoverTolerance * view.width;
        const double slack_y = kViewCoverTolerance * view.height;
        // Written so that a NaN refuses.
        if (!(right - left > 1.0) || !(bottom - top > 1.0) || !(left <= x0 + slack_x) ||
            !(right >= x1 - slack_x) || !(top <= y0 + slack_y) || !(bottom >= y1 - slack_y)) {
            return {};
        }
        // The vectors under the view: texel = origin + (pixel - edge) x
        // texels per pixel, kept inside the vectors where the camera falls
        // short within the tolerance.
        const double per_x = frame.motion_width / (right - left);
        const double per_y = frame.motion_height / (bottom - top);
        const double motion_left = frame.motion_x, motion_top = frame.motion_y;
        const double motion_right = motion_left + frame.motion_width;
        const double motion_bottom = motion_top + frame.motion_height;
        const double mx0 = std::clamp(motion_left + (x0 - left) * per_x, motion_left, motion_right);
        const double mx1 = std::clamp(motion_left + (x1 - left) * per_x, motion_left, motion_right);
        const double my0 = std::clamp(motion_top + (y0 - top) * per_y, motion_top, motion_bottom);
        const double my1 = std::clamp(motion_top + (y1 - top) * per_y, motion_top, motion_bottom);
        if (!(mx1 - mx0 > 0.5) || !(my1 - my0 > 0.5)) return {};
        auto placed = std::make_shared<DlssMotionVectorFrame>(frame);
        placed->output_x = static_cast<std::uint32_t>(view.x);
        placed->output_y = static_cast<std::uint32_t>(view.y);
        placed->output_width = view.width;
        placed->output_height = view.height;
        placed->placed = true;
        placed->placed_motion_rect = {static_cast<float>(mx0), static_cast<float>(my0),
            static_cast<float>(mx1 - mx0), static_cast<float>(my1 - my0)};
        return placed;
    } catch (...) {
        return {};
    }
}

std::array<std::shared_ptr<const DlssMotionVectorSet>, kDlssMotionVectorEyeCount>
resolve_view_placed_dlss_guides(
    ID3D12CommandQueue* consumer_queue,
    const std::array<DlssEyeView, kDlssMotionVectorEyeCount>& eyes,
    const std::array<std::uint64_t, kDlssMotionVectorEyeCount>& released_at) noexcept {
    std::array<std::shared_ptr<const DlssMotionVectorSet>, kDlssMotionVectorEyeCount> none{};
    try {
        if (!g_tracking_enabled.load(std::memory_order_acquire) || consumer_queue == nullptr) {
            return none;
        }
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        ++state.statistics.resolve_calls;
        std::vector<Candidate> candidates;
        std::vector<DlssViewGeometry> cameras;
        std::uint64_t newest = 0;
        for (const auto& [unused_stream_id, stream] : state.streams) {
            static_cast<void>(unused_stream_id);
            if (!stream.latest) continue;
            const auto camera = dlss_view_geometry(*stream.latest);
            if (!camera.known) continue;
            if (!same_identity(stream.latest->producer_queue.Get(), consumer_queue) &&
                !(stream.latest->producer_fence && stream.latest->producer_fence_value != 0 &&
                  same_device(stream.latest->producer_fence.Get(), consumer_queue))) continue;
            candidates.push_back({stream.first_publication, stream.latest, stream.previous, stream.previous2});
            cameras.push_back(camera);
            newest = std::max(newest, stream.latest->publication);
        }
        for (std::size_t index = candidates.size(); index-- > 0;) {
            if (newest - candidates[index].frame->publication > kMaximumViewPublicationGap) {
                candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(index));
                cameras.erase(cameras.begin() + static_cast<std::ptrdiff_t>(index));
            }
        }
        if (candidates.empty()) return none;

        // The streams whose camera covers each eye, and how well each fits.
        constexpr double kNoFit = 1e30;
        std::vector<std::array<double, kDlssMotionVectorEyeCount>> fit(
            candidates.size(), {kNoFit, kNoFit});
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            for (std::uint32_t eye = 0; eye < kDlssMotionVectorEyeCount; ++eye) {
                if (place_guide_in_view(*candidates[index].frame, eyes[eye]))
                    fit[index][eye] = view_fit_error(cameras[index], eyes[eye]);
            }
        }
        // The pair of streams, one an eye, that fits best.
        std::size_t best_left = 0, best_right = 0;
        double best = kNoFit;
        for (std::size_t left = 0; left < candidates.size(); ++left) {
            for (std::size_t right = 0; right < candidates.size(); ++right) {
                if (left == right || fit[left][0] >= kNoFit || fit[right][1] >= kNoFit) continue;
                const double cost = fit[left][0] + fit[right][1];
                if (cost < best) {
                    best = cost;
                    best_left = left;
                    best_right = right;
                }
            }
        }
        if (best >= kNoFit) {
            state.statistics.status = DlssMotionVectorStatus::output_not_direct;
            return none;
        }
        // Two cameras that fit both eyes alike (a symmetric field): the
        // camera further left is the left eye.
        if (fit[best_right][0] < kNoFit && fit[best_left][1] < kNoFit &&
            std::fabs((fit[best_right][0] + fit[best_left][1]) - best) < 1e-4) {
            const auto& a = cameras[best_left];
            const auto& b = cameras[best_right];
            const std::array<float, 3> right{
                a.right[0] + b.right[0], a.right[1] + b.right[1], a.right[2] + b.right[2]};
            const double side_a = camera_side(a, right), side_b = camera_side(b, right);
            if (!(std::fabs(side_a - side_b) > 1e-6)) {
                ++state.statistics.resolve_stale_pairs;
                state.statistics.status = DlssMotionVectorStatus::temporal_mismatch;
                return none;
            }
            if (side_a > side_b) std::swap(best_left, best_right);
        }

        // Each eye's evaluation: its stream's newest by the eye's release.
        std::array<std::shared_ptr<const DlssMotionVectorSet>, kDlssMotionVectorEyeCount> result{};
        const std::array<std::size_t, kDlssMotionVectorEyeCount> chosen_streams{best_left, best_right};
        for (std::uint32_t eye = 0; eye < kDlssMotionVectorEyeCount; ++eye) {
            const Candidate& candidate = candidates[chosen_streams[eye]];
            std::shared_ptr<const DlssMotionVectorFrame> chosen = candidate.frame;
            if (released_at[eye] != 0 && chosen->publication > released_at[eye]) {
                chosen = candidate.previous && candidate.previous->epoch == candidate.frame->epoch &&
                        candidate.previous->publication <= released_at[eye]
                    ? candidate.previous : nullptr;
            }
            auto placed = chosen ? place_guide_in_view(*chosen, eyes[eye]) : nullptr;
            if (!placed) {
                ++state.statistics.resolve_stale_pairs;
                state.statistics.status = DlssMotionVectorStatus::temporal_mismatch;
                return none;
            }
            // In OFXR-BRIDGE a CPU publication is not a cross-queue receipt.
            if (!same_identity(chosen->producer_queue.Get(), consumer_queue) &&
                !(chosen->producer_fence && chosen->producer_fence_value != 0 &&
                  same_device(chosen->producer_fence.Get(), consumer_queue) &&
                  SUCCEEDED(consumer_queue->Wait(chosen->producer_fence.Get(),
                      chosen->producer_fence_value)))) {
                ++state.statistics.resolve_queue_mismatches;
                state.statistics.status = DlssMotionVectorStatus::queue_mismatch;
                return none;
            }
            auto set = std::make_shared<DlssMotionVectorSet>();
            set->eye_count = 1;
            set->eyes[0] = std::move(placed);
            result[eye] = std::move(set);
        }
        state.statistics.matched += kDlssMotionVectorEyeCount;
        return result;
    } catch (...) {
        return none;
    }
}

std::uint64_t dlss_motion_vector_publications() noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        return state.next_publication - 1;
    } catch (...) {
        return 0;
    }
}

std::shared_ptr<const DlssMotionVectorSet> resolve_dlss_motion_vectors(
    ID3D12Resource* output,
    ID3D12CommandQueue* consumer_queue,
    int eye,
    std::uint64_t released_at,
    std::uint64_t other_released_at,
    DlssReleasedEyeIdentity identity) noexcept {
    try {
        if (!g_tracking_enabled.load(std::memory_order_acquire)) {
            report_dlss_motion_vector_status(DlssMotionVectorStatus::disabled);
            return {};
        }
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        ++state.statistics.resolve_calls;
        if (output == nullptr || consumer_queue == nullptr) {
            ++state.statistics.invalid_rejections;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return {};
        }

        const auto description = output->GetDesc();
        if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            ++state.statistics.invalid_rejections;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return {};
        }
        std::uint32_t eye_count = std::min<std::uint32_t>(
            std::max<std::uint32_t>(description.DepthOrArraySize, 1U),
            kDlssMotionVectorEyeCount);

        std::vector<Candidate> candidates;
        std::uint32_t streams_with_frames = 0;
        candidates.reserve(state.streams.size());
        for (const auto& [unused_stream_id, stream] : state.streams) {
            static_cast<void>(unused_stream_id);
            if (!stream.latest) continue;
            ++streams_with_frames;
            if (same_identity(stream.latest->producer_queue.Get(), consumer_queue) ||
                (stream.latest->producer_fence &&
                 stream.latest->producer_fence_value != 0 &&
                 same_device(stream.latest->producer_fence.Get(), consumer_queue))) {
                candidates.push_back(
                    {stream.first_publication, stream.latest, stream.previous, stream.previous2});
            }
        }
        const auto synchronize = [&](const auto& frame) {
            if (same_identity(frame->producer_queue.Get(), consumer_queue))
                return true;
            return frame->producer_fence && frame->producer_fence_value != 0 &&
                same_device(frame->producer_fence.Get(), consumer_queue) &&
                SUCCEEDED(consumer_queue->Wait(frame->producer_fence.Get(),
                    frame->producer_fence_value));
        };

        // An evaluation whose output is this very image belongs to it,
        // whatever else matches its size.
        bool direct_match = false;
        {
            std::vector<Candidate> direct;
            for (const auto& candidate : candidates) {
                if (same_identity(candidate.frame->output.Get(), output)) {
                    direct.push_back(candidate);
                }
            }
            if (!direct.empty()) {
                candidates = std::move(direct);
                direct_match = true;
            }
        }

        // Native Stereo can place two eye viewports side by side in one
        // array-size-one XR image while DLSS keeps one persistent stream per
        // eye. Restrict the candidate set to streams whose published output is
        // exactly one horizontal half of the XR image. This excludes desktop
        // DLSS streams and leaves ordinary mono/Sync Sequential resources on
        // the existing array-size-one path.
        bool packed_stereo = false;
        if (eye_count == 1) {
            std::vector<Candidate> horizontal_eye_candidates;
            horizontal_eye_candidates.reserve(candidates.size());
            for (const auto& candidate : candidates) {
                if (candidate.frame->output_width != 0 &&
                    candidate.frame->output_height != 0 &&
                    description.Width ==
                        static_cast<std::uint64_t>(
                            candidate.frame->output_width) * 2U &&
                    description.Height == candidate.frame->output_height) {
                    horizontal_eye_candidates.push_back(candidate);
                }
            }
            if (horizontal_eye_candidates.size() >=
                    kDlssMotionVectorEyeCount) {
                candidates = std::move(horizontal_eye_candidates);
                eye_count = kDlssMotionVectorEyeCount;
                packed_stereo = true;
            }
        }

        if (eye_count == 1) {
            // Composition/UI swapchains share the scene queue but are not the
            // upscaler output. Never attach unrelated scene guides to them.
            std::erase_if(candidates, [&](const Candidate& c) {
                return c.frame->output_x != 0 || c.frame->output_y != 0 ||
                    c.frame->output_width != description.Width ||
                    c.frame->output_height != description.Height;
            });
        }

        // A swapchain per eye, each the size of a DLSS output: every eye's
        // evaluation matches each eye's image by size, and the newest went to
        // both - with the capture at xrEndFrame, after both eyes, always the
        // eye DLSS ran last - so the other eye followed its motion, a double
        // image wherever the two eyes' motion differs. The eye this image is
        // submitted as now takes its own evaluation, in the order the stereo
        // path gives two: two streams, or the game's one stream alternating
        // eyes.
        if (!direct_match && eye_count == 1 && released_at != 0 &&
            (eye == 0 || eye == 1)) {
            // R15: evaluate an eye, then release its XR color. Resolve by
            // that release, never by an odd/even serial or discovery order.
            // Strict FSR opts into this join; AFW and packed color stay below.
            const auto stale = [&]() -> std::shared_ptr<const DlssMotionVectorSet> {
                ++state.statistics.resolve_stale_pairs;
                state.statistics.status = DlssMotionVectorStatus::temporal_mismatch;
                return {};
            };
            std::uint64_t newest_publication = 0;
            for (const auto& candidate : candidates)
                newest_publication = std::max(newest_publication, candidate.frame->publication);
            std::erase_if(candidates, [&](const Candidate& candidate) {
                return newest_publication - candidate.frame->publication >
                    kMaximumStereoPublicationGap;
            });
            if (candidates.empty()) {
                // Absence is not a temporally invalid pair. Preserve the
                // existing precise GUI/fallback reason after route switches.
                if (streams_with_frames >= eye_count) {
                    ++state.statistics.resolve_queue_mismatches;
                    state.statistics.status = DlssMotionVectorStatus::queue_mismatch;
                } else {
                    ++state.statistics.resolve_missing_streams;
                    state.statistics.status = state.statistics.published == 0
                        ? DlssMotionVectorStatus::waiting_for_dlss
                        : DlssMotionVectorStatus::output_not_direct;
                }
                return {};
            }
            std::vector<std::shared_ptr<const DlssMotionVectorFrame>> frames;
            for (const auto& candidate : candidates) {
                frames.push_back(candidate.frame);
                if (candidate.previous && candidate.previous->epoch == candidate.frame->epoch)
                    frames.push_back(candidate.previous);
            }
            std::sort(frames.begin(), frames.end(), [](const auto& a, const auto& b) {
                return a->publication > b->publication;
            });
            const auto newest_by = [&](std::uint64_t limit) {
                for (const auto& frame : frames)
                    if (frame->publication <= limit) return frame;
                return std::shared_ptr<const DlssMotionVectorFrame>{};
            };
            auto chosen = newest_by(released_at);
            // One evaluation for both eyes: each eye's own release bounds it,
            // and its serials chain frame to frame as published.
            const auto other = other_released_at && !identity.both_eyes
                ? newest_by(other_released_at) : nullptr;
            bool shared_stream = candidates.size() == 1 && !identity.both_eyes;
            if (chosen && other && chosen == other) {
                if (candidates.size() >= 2) {
                    // Both evaluated before either release. Exactly two
                    // active streams retain the legacy ordering unless strict identity is requested.
                    // A third same-size stream is ambiguous; do not guess.
                    if (candidates.size() != 2) return stale();
                    std::sort(candidates.begin(), candidates.end(),
                        [](const Candidate& a, const Candidate& b) {
                            const auto ao = input_offset(*a.frame), bo = input_offset(*b.frame);
                            return ao != bo ? ao < bo : a.first_publication < b.first_publication;
                        });
                    std::size_t selected = static_cast<std::size_t>(eye);
                    if (identity.strict && input_offset(*candidates[0].frame) ==
                            input_offset(*candidates[1].frame)) {
                        const auto known = std::find_if(candidates.begin(), candidates.end(),
                            [&](const Candidate& c) { return identity.stream != 0 &&
                                c.frame->stream == identity.stream && c.frame->epoch == identity.epoch; });
                        if (known == candidates.end()) return stale();
                        selected = static_cast<std::size_t>(known - candidates.begin());
                    }
                    const auto& candidate = candidates[selected];
                    chosen = candidate.frame->publication <= released_at
                        ? candidate.frame : candidate.previous;
                } else {
                    // One shared stream: left first when both eyes were
                    // evaluated before either release, as in upstream R15.
                    const auto later = std::find(frames.begin(), frames.end(), chosen);
                    const auto earlier = later + 1;
                    if (identity.strict) {
                        if (earlier == frames.end() || (*earlier)->stream != (*later)->stream ||
                            (*earlier)->epoch != (*later)->epoch ||
                            input_offset(**earlier) == input_offset(**later)) return stale();
                        const bool earlier_left = input_offset(**earlier) < input_offset(**later);
                        chosen = (eye == 0) == earlier_left ? *earlier : *later;
                    } else {
                        chosen = eye == 1 ? *later
                            : earlier != frames.end() ? *earlier : nullptr;
                    }
                    if (!identity.strict && chosen && eye == 0 && (chosen->stream != other->stream ||
                        chosen->epoch != other->epoch ||
                        chosen->serial + 1 != other->serial)) return stale();
                }
            }
            if (!chosen || frames.empty() || chosen->publication > released_at ||
                frames.front()->publication - chosen->publication > kMaximumStereoPublicationGap)
                return stale();
            if (other && other != chosen)
                shared_stream = other->stream == chosen->stream;
            // In OFXR-BRIDGE a CPU publication is not a cross-queue receipt.
            // Apply the existing synchronization check to the actual chosen
            // frame, including older snapshots selected by the watermark.
            if (!synchronize(chosen)) {
                ++state.statistics.resolve_queue_mismatches;
                state.statistics.status = DlssMotionVectorStatus::queue_mismatch;
                return {};
            }
            auto frame = std::make_shared<DlssMotionVectorFrame>(*chosen);
            if (identity.both_eyes) {
                // Each eye reads its own half, so the two eyes' guides never
                // cover the same vectors (classify_fsr_stereo_guides).
                const std::uint32_t view_width = frame->output_width / 2U;
                if (frame->output_x != 0 || frame->output_width != description.Width ||
                    frame->output_width % 2U != 0 || identity.view_half > 1 ||
                    frame->motion_width < 2 || (frame->depth && frame->depth_width < 2))
                    return stale();
                const auto half = static_cast<std::uint32_t>(eye);
                frame->output_x = identity.view_half * view_width;
                frame->output_width = view_width;
                const std::uint32_t motion_half = frame->motion_width / 2U;
                frame->motion_x += half * motion_half;
                frame->motion_width = motion_half;
                if (frame->depth) {
                    const std::uint32_t depth_half = frame->depth_width / 2U;
                    frame->depth_x += half * depth_half;
                    frame->depth_width = depth_half;
                }
            }
            if (shared_stream) {
                frame->previous_serial = frame->serial > 2 ? frame->serial - 2 : 0;
                // Strict FSR retains its existing guides and queue waits.
                if (!identity.strict) for (const auto& candidate : candidates) {
                    for (const auto& bridge : {candidate.frame, candidate.previous, candidate.older}) {
                        if (bridge && bridge->stream == chosen->stream &&
                            bridge->epoch == chosen->epoch && bridge->serial + 1 == chosen->serial &&
                            synchronize(bridge)) frame->bridge = bridge;
                    }
                }
            }
            auto result = std::make_shared<DlssMotionVectorSet>();
            result->eye_count = 1;
            result->eyes[0] = std::move(frame);
            ++state.statistics.matched;
            return result;
        }

        if (!direct_match && eye_count == 1 &&
            (eye == 0 || eye == 1)) {
            if (candidates.size() >= 2) {
                std::sort(candidates.begin(), candidates.end(),
                    [](const Candidate& a, const Candidate& b) {
                        return a.frame->publication > b.frame->publication;
                    });
                candidates.resize(2);
                const auto newest = candidates[0].frame->publication;
                const auto oldest = candidates[1].frame->publication;
                if (newest < oldest || newest - oldest > kMaximumStereoPublicationGap) {
                    ++state.statistics.resolve_stale_pairs;
                    state.statistics.status = DlssMotionVectorStatus::temporal_mismatch;
                    return {};
                }
                for (const auto& candidate : candidates) {
                    if (!synchronize(candidate.frame)) {
                        ++state.statistics.resolve_queue_mismatches;
                        state.statistics.status = DlssMotionVectorStatus::queue_mismatch;
                        return {};
                    }
                }
                std::sort(candidates.begin(), candidates.end(),
                    [](const Candidate& a, const Candidate& b) {
                        const auto ao = input_offset(*a.frame), bo = input_offset(*b.frame);
                        if (ao != bo) return ao < bo;
                        return a.first_publication < b.first_publication;
                    });
                auto result = std::make_shared<DlssMotionVectorSet>();
                result->eye_count = 1;
                result->eyes[0] = candidates[static_cast<std::size_t>(eye)].frame;
                ++state.statistics.matched;
                return result;
            }
            if (candidates.size() == 1 && streams_with_frames == 1) {
                const auto& current = candidates.front().frame;
                const auto& previous = candidates.front().previous;
                if (!previous && eye == 1) {
                    ++state.statistics.resolve_stale_pairs;
                    state.statistics.status = DlssMotionVectorStatus::temporal_mismatch;
                    return {};
                }
                if (previous && current->stream == previous->stream &&
                    current->epoch == previous->epoch &&
                    current->serial == previous->serial + 1 &&
                    current->publication > previous->publication &&
                    current->publication - previous->publication <=
                        kMaximumStereoPublicationGap) {
                    // One stream for both eyes: the odd serial is the left
                    // eye, as for alternating-eye renderers, unless the
                    // inputs say otherwise. Each eye sees every other serial.
                    const auto& odd = (current->serial & 1U) != 0 ? current : previous;
                    const auto& even = (current->serial & 1U) != 0 ? previous : current;
                    const bool swapped = input_offset(*even) < input_offset(*odd);
                    const auto& chosen = (eye == 0) != swapped ? odd : even;
                    // The chosen eye's evaluation of this frame has to be the
                    // newest or the one just before it: a left eye evaluated
                    // with the right still to come is this frame's.
                    if (!synchronize(chosen)) {
                        ++state.statistics.resolve_queue_mismatches;
                        state.statistics.status = DlssMotionVectorStatus::queue_mismatch;
                        return {};
                    }
                    if (chosen == current || (current->serial & 1U) == 0) {
                        auto frame = std::make_shared<DlssMotionVectorFrame>(*chosen);
                        frame->previous_serial = frame->serial > 2 ? frame->serial - 2 : 0;
                        auto result = std::make_shared<DlssMotionVectorSet>();
                        result->eye_count = 1;
                        result->eyes[0] = std::move(frame);
                        ++state.statistics.matched;
                        return result;
                    }
                    ++state.statistics.resolve_stale_pairs;
                    state.statistics.status = DlssMotionVectorStatus::temporal_mismatch;
                    return {};
                }
                // A reset/gap cannot turn a requested eye into whichever
                // evaluation happened last. Only a first left publication
                // may continue through the ordinary synchronized path.
                if (previous) {
                    ++state.statistics.resolve_stale_pairs;
                    state.statistics.status = DlssMotionVectorStatus::temporal_mismatch;
                    return {};
                }
            }
        }

        // Alternating-eye renderers such as Ghost of Tsushima's AER path reuse
        // one DLSS handle for both eyes. A complete stereo guide set therefore
        // consists of two consecutive publications from that single stream.
        // Even local serials close the stable eye-0/eye-1 pair; odd serials are
        // deliberately left pending until the second eye arrives.
        if (eye_count == 2 && candidates.size() == 1) {
            const auto& current = candidates.front().frame;
            const auto& previous = candidates.front().previous;
            if (previous && current->stream == previous->stream &&
                current->epoch == previous->epoch &&
                current->serial == previous->serial + 1 &&
                (current->serial & 1U) == 0 &&
                current->publication > previous->publication &&
                current->publication - previous->publication <=
                    kMaximumStereoPublicationGap &&
                synchronize(previous) && synchronize(current)) {
                auto left = std::make_shared<DlssMotionVectorFrame>(*previous);
                auto right = std::make_shared<DlssMotionVectorFrame>(*current);
                // The first of the pair is the left eye unless the inputs
                // say otherwise.
                if (input_offset(*right) < input_offset(*left)) std::swap(left, right);
                left->previous_serial = left->serial > 2 ? left->serial - 2 : 0;
                right->previous_serial = right->serial > 2 ? right->serial - 2 : 0;
                auto result = std::make_shared<DlssMotionVectorSet>();
                result->eye_count = 2;
                result->eyes[0] = std::move(left);
                result->eyes[1] = std::move(right);
                ++state.statistics.matched;
                return result;
            }
        }

        if (candidates.size() < eye_count) {
            if (streams_with_frames >= eye_count) {
                ++state.statistics.resolve_queue_mismatches;
                state.statistics.status = DlssMotionVectorStatus::queue_mismatch;
            } else {
                ++state.statistics.resolve_missing_streams;
                state.statistics.status = state.statistics.published == 0
                    ? DlssMotionVectorStatus::waiting_for_dlss
                    : DlssMotionVectorStatus::output_not_direct;
            }
            return {};
        }

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a,
                                                           const Candidate& b) {
            return a.frame->publication > b.frame->publication;
        });
        candidates.resize(eye_count);
        for (const auto& candidate : candidates) {
            if (!synchronize(candidate.frame)) {
                ++state.statistics.invalid_rejections;
                state.statistics.status = DlssMotionVectorStatus::invalid_input;
                return {};
            }
        }
        const auto newest = candidates.front().frame->publication;
        const auto oldest = candidates.back().frame->publication;
        if (newest < oldest || newest - oldest > kMaximumStereoPublicationGap) {
            ++state.statistics.resolve_stale_pairs;
            state.statistics.status = DlssMotionVectorStatus::temporal_mismatch;
            return {};
        }

        // Eye order follows the two DLSS evaluation streams rather than the
        // final colour resource, which survives any post-DLSS shader passes:
        // by where each stream's inputs sit, then by first-seen order. First
        // seen alone gave Galactic Racer under UEVR each eye the other's
        // guides whenever the right eye evaluated first.
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a,
                                                           const Candidate& b) {
            const auto ao = input_offset(*a.frame), bo = input_offset(*b.frame);
            if (ao != bo) return ao < bo;
            return a.first_publication < b.first_publication;
        });

        auto result = std::make_shared<DlssMotionVectorSet>();
        result->eye_count = eye_count;
        for (std::uint32_t eye = 0; eye < eye_count; ++eye) {
            if (packed_stereo) {
                auto frame = std::make_shared<DlssMotionVectorFrame>(*candidates[eye].frame);
                // Each mono producer uses local coordinates. Only the resolved
                // XR join maps color to its half; native input rects stay intact.
                frame->output_x = eye * frame->output_width;
                result->eyes[eye] = std::move(frame);
            } else {
                result->eyes[eye] = candidates[eye].frame;
            }
        }
        ++state.statistics.matched;
        return result;
    } catch (...) {
        report_dlss_motion_vector_status(DlssMotionVectorStatus::invalid_input);
        return {};
    }
}

std::shared_ptr<const DlssMotionVectorSet> resolve_dlss_motion_vectors(
    std::uint32_t eye_count,
    ID3D12CommandQueue* consumer_queue) noexcept {
    try {
        if (!g_tracking_enabled.load(std::memory_order_acquire)) {
            report_dlss_motion_vector_status(DlssMotionVectorStatus::disabled);
            return {};
        }
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        ++state.statistics.resolve_calls;
        if (eye_count == 0 || eye_count > kDlssMotionVectorEyeCount ||
            consumer_queue == nullptr) {
            ++state.statistics.invalid_rejections;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return {};
        }

        std::vector<Candidate> candidates;
        std::uint32_t streams_with_frames = 0;
        candidates.reserve(state.streams.size());
        for (const auto& [unused_stream_id, stream] : state.streams) {
            static_cast<void>(unused_stream_id);
            if (!stream.latest) continue;
            ++streams_with_frames;
            if (same_identity(stream.latest->producer_queue.Get(), consumer_queue) ||
                (stream.latest->producer_fence &&
                 stream.latest->producer_fence_value != 0 &&
                 same_device(stream.latest->producer_fence.Get(), consumer_queue))) {
                candidates.push_back(
                    {stream.first_publication, stream.latest, stream.previous, stream.previous2});
            }
        }

        const auto synchronize = [&](const auto& frame) {
            if (!frame ||
                same_identity(frame->producer_queue.Get(), consumer_queue))
                return true;
            return frame->producer_fence && frame->producer_fence_value != 0 &&
                SUCCEEDED(consumer_queue->Wait(
                    frame->producer_fence.Get(), frame->producer_fence_value));
        };

        // Alternating-eye renderers such as Ghost of Tsushima's AER path reuse
        // one DLSS handle for both eyes. A complete stereo guide set therefore
        // consists of two consecutive publications from that single stream.
        // Even local serials close the stable eye-0/eye-1 pair; odd serials are
        // deliberately left pending until the second eye arrives.
        if (eye_count == 2 && candidates.size() == 1) {
            const auto& current = candidates.front().frame;
            const auto& previous = candidates.front().previous;
            if (previous && current->stream == previous->stream &&
                current->epoch == previous->epoch &&
                current->serial == previous->serial + 1 &&
                (current->serial & 1U) == 0 &&
                current->publication > previous->publication &&
                current->publication - previous->publication <=
                    kMaximumStereoPublicationGap &&
                (same_identity(previous->producer_queue.Get(), consumer_queue) ||
                 (previous->producer_fence &&
                  previous->producer_fence_value != 0 &&
                  same_device(previous->producer_fence.Get(), consumer_queue))) &&
                synchronize(previous) && synchronize(current)) {
                auto left = std::make_shared<DlssMotionVectorFrame>(*previous);
                auto right = std::make_shared<DlssMotionVectorFrame>(*current);
                left->previous_serial = left->serial > 2 ? left->serial - 2 : 0;
                right->previous_serial = right->serial > 2 ? right->serial - 2 : 0;
                auto result = std::make_shared<DlssMotionVectorSet>();
                result->eye_count = 2;
                result->eyes[0] = std::move(left);
                result->eyes[1] = std::move(right);
                ++state.statistics.matched;
                return result;
            }
        }

        if (candidates.size() < eye_count) {
            if (streams_with_frames >= eye_count) {
                ++state.statistics.resolve_queue_mismatches;
                state.statistics.status = DlssMotionVectorStatus::queue_mismatch;
            } else {
                ++state.statistics.resolve_missing_streams;
                state.statistics.status = state.statistics.published == 0
                    ? DlssMotionVectorStatus::waiting_for_dlss
                    : DlssMotionVectorStatus::output_not_direct;
            }
            return {};
        }

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a,
                                                           const Candidate& b) {
            return a.frame->publication > b.frame->publication;
        });
        candidates.resize(eye_count);
        for (const auto& candidate : candidates) {
            if (!synchronize(candidate.frame)) {
                ++state.statistics.invalid_rejections;
                state.statistics.status = DlssMotionVectorStatus::invalid_input;
                return {};
            }
        }
        const auto newest = candidates.front().frame->publication;
        const auto oldest = candidates.back().frame->publication;
        if (newest < oldest || newest - oldest > kMaximumStereoPublicationGap) {
            ++state.statistics.resolve_stale_pairs;
            state.statistics.status = DlssMotionVectorStatus::temporal_mismatch;
            return {};
        }

        // Stable first-seen stream order defines eye order. This survives any
        // post-DLSS shader passes because the association follows the two DLSS
        // evaluation streams rather than the final color resource identity.
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a,
                                                           const Candidate& b) {
            return a.first_publication < b.first_publication;
        });

        auto result = std::make_shared<DlssMotionVectorSet>();
        result->eye_count = eye_count;
        for (std::uint32_t eye = 0; eye < eye_count; ++eye) {
            result->eyes[eye] = candidates[eye].frame;
        }
        ++state.statistics.matched;
        return result;
    } catch (...) {
        report_dlss_motion_vector_status(DlssMotionVectorStatus::invalid_input);
        return {};
    }
}

std::shared_ptr<const DlssMotionVectorSet> resolve_luke_alternating_guides(
    ID3D12CommandQueue* consumer_queue) noexcept {
    try {
        if (!g_tracking_enabled.load(std::memory_order_acquire) ||
            consumer_queue == nullptr) return {};
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        ++state.statistics.resolve_calls;
        // The Luke route is admitted only with exactly one live NGX stream.
        // Never silently choose a pair from a different producer.
        if (state.streams.size() != 1) return {};
        const auto& stream = state.streams.begin()->second;
        const auto& current = stream.latest;
        const auto& previous = stream.previous;
        const auto& previous2 = stream.previous2;
        if (!previous2 || !previous || !current ||
            previous2->stream != previous->stream ||
            previous2->epoch != previous->epoch ||
            previous2->serial + 1 != previous->serial ||
            previous2->publication + 1 != previous->publication ||
            current->stream != previous->stream ||
            current->epoch != previous->epoch ||
            current->serial != previous->serial + 1 ||
            current->publication <= previous->publication ||
            current->publication - previous->publication >
                kMaximumStereoPublicationGap) return {};
        const auto compatible = [&](const auto& frame) {
            if (same_identity(frame->producer_queue.Get(), consumer_queue))
                return true;
            return frame->producer_fence && frame->producer_fence_value != 0 &&
                same_device(frame->producer_fence.Get(), consumer_queue) &&
                SUCCEEDED(consumer_queue->Wait(frame->producer_fence.Get(),
                                              frame->producer_fence_value));
        };
        if (!compatible(previous2) || !compatible(previous) ||
            !compatible(current)) return {};
        auto older = std::make_shared<DlssMotionVectorFrame>(*previous);
        auto newer = std::make_shared<DlssMotionVectorFrame>(*current);
        older->previous_serial = older->serial > 2 ? older->serial - 2 : 0;
        newer->previous_serial = newer->serial > 2 ? newer->serial - 2 : 0;
        older->previous_opposite_guide = previous2;
        newer->previous_opposite_guide = previous;
        newer->previous_same_guide = previous2;
        auto result = std::make_shared<DlssMotionVectorSet>();
        result->eye_count = 2;
        result->eyes[0] = std::move(older);
        result->eyes[1] = std::move(newer);
        ++state.statistics.matched;
        return result;
    } catch (...) {
        report_dlss_motion_vector_status(DlssMotionVectorStatus::invalid_input);
        return {};
    }
}


std::shared_ptr<const DlssMotionVectorFrame> resolve_luke_guide_token(
    std::uint32_t color_origin_token,
    ID3D12CommandQueue* consumer_queue,
    ID3D12Device* canonical_consumer_device) noexcept {
    try {
        const bool tracking = g_tracking_enabled.load(std::memory_order_acquire);
        if (!tracking || color_origin_token == 0 || consumer_queue == nullptr ||
            canonical_consumer_device == nullptr) {
            auto& state=registry();std::scoped_lock lock(state.mutex);
            report_luke_token_refusal(1,color_origin_token,state,nullptr,0,nullptr,nullptr,
                consumer_queue,canonical_consumer_device);
            return {};
        }
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        ++state.statistics.resolve_calls;
        // The protected color token is process-unique, not local to a native
        // feature handle. REAL VR can own several SR handles; an unrelated
        // startup/multipass stream must not invalidate this exact color key.
        // Resolve its one owner, then join only that owner's temporal history.
        const StreamState* owner{};
        std::uint64_t owner_stream{};
        std::size_t origin_index{};
        for (const auto& [stream_id, candidate] : state.streams) {
            for (std::size_t index = 0; index < candidate.luke_origin_history.size(); ++index) {
                if (candidate.luke_origin_history[index]->color_origin_token != color_origin_token)
                    continue;
                if (owner) {
                    ++state.statistics.invalid_rejections;
                    state.statistics.status = DlssMotionVectorStatus::invalid_input;
                    report_luke_token_refusal(2,color_origin_token,state,owner,owner_stream,
                        owner->luke_origin_history[origin_index].get(),nullptr,consumer_queue,canonical_consumer_device);
                    return {};
                }
                owner = &candidate;
                owner_stream = stream_id;
                origin_index = index;
            }
        }
        if (!owner) {
            ++state.statistics.resolve_missing_streams;
            report_luke_token_refusal(3,color_origin_token,state,nullptr,0,nullptr,nullptr,
                consumer_queue,canonical_consumer_device);
            return {};
        }
        const auto& stream = *owner;
        const auto origin = stream.luke_origin_history.begin() + origin_index;
        if (origin == stream.luke_origin_history.begin()) {
            ++state.statistics.resolve_stale_pairs;
            report_luke_token_refusal(4,color_origin_token,state,owner,owner_stream,origin->get(),nullptr,
                consumer_queue,canonical_consumer_device);
            return {};
        }
        const auto& current = *origin;
        auto previous = *std::prev(origin);
        const bool may_interleave = !current->native_production_sequence && current->publication >= 3 &&
            previous->publication + 1 != current->publication;
        const auto interleaved_middle = may_interleave
            ? protected_publication(state, current->publication - 1) : nullptr;
        const auto interleaved_older = may_interleave
            ? protected_publication(state, current->publication - 2) : nullptr;
        const bool interleaved = interleaved_frames(*current, interleaved_middle, interleaved_older);
        if (interleaved) previous = interleaved_middle;
        if (!interleaved && (current->stream != owner_stream || current->stream != previous->stream ||
            current->epoch != previous->epoch ||
            (!stream.retain_reset_origins && current->epoch != stream.epoch) ||
            previous->serial + 1 != current->serial ||
            previous->publication + 1 != current->publication)) {
            ++state.statistics.resolve_stale_pairs;
            const unsigned reason=current->stream!=owner_stream||current->stream!=previous->stream?5:
                current->epoch!=previous->epoch||(!stream.retain_reset_origins&&current->epoch!=stream.epoch)?6:
                previous->serial+1!=current->serial?7:8;
            report_luke_token_refusal(reason,color_origin_token,state,owner,owner_stream,current.get(),previous.get(),
                consumer_queue,canonical_consumer_device);
            return {};
        }
        const auto compatible = [&](const auto& frame) {
            // Only the owned snapshots enter AFW. The original SR output is
            // metadata for its color key, not a resource sampled by AFW.
            if (!same_identity(frame->motion_snapshot_device.Get(),
                    canonical_consumer_device)) {
                report_luke_token_refusal(frame==current?9:10,color_origin_token,state,owner,owner_stream,
                    current.get(),previous.get(),consumer_queue,canonical_consumer_device,S_OK,frame->publication);
                return false;
            }
            if (frame->depth && !same_identity(frame->depth_snapshot_device.Get(),
                    canonical_consumer_device)) {
                report_luke_token_refusal(11,color_origin_token,state,owner,owner_stream,current.get(),previous.get(),
                    consumer_queue,canonical_consumer_device,S_OK,frame->publication);
                return false;
            }
            if (same_identity(frame->producer_queue.Get(), consumer_queue))
                return true;
            if (!frame->producer_fence || frame->producer_fence_value == 0) {
                report_luke_token_refusal(12,color_origin_token,state,owner,owner_stream,current.get(),previous.get(),
                    consumer_queue,canonical_consumer_device,S_OK,frame->publication);
                return false;
            }
            if (!same_identity(frame->producer_fence_device.Get(),
                    canonical_consumer_device)) {
                report_luke_token_refusal(13,color_origin_token,state,owner,owner_stream,current.get(),previous.get(),
                    consumer_queue,canonical_consumer_device,S_OK,frame->publication);
                return false;
            }
            const HRESULT wait = consumer_queue->Wait(frame->producer_fence.Get(),
                frame->producer_fence_value);
            if (FAILED(wait)) {
                report_luke_token_refusal(14,color_origin_token,state,owner,owner_stream,current.get(),previous.get(),
                    consumer_queue,canonical_consumer_device,wait,frame->publication);
                return false;
            }
            return true;
        };
        if (!compatible(previous) || !compatible(current)) {
            ++state.statistics.resolve_queue_mismatches;
            state.statistics.status = DlssMotionVectorStatus::queue_mismatch;
            return {};
        }
        auto resolved = std::make_shared<DlssMotionVectorFrame>(*current);
        resolved->previous_serial = resolved->serial > 2 ? resolved->serial - 2 : 0;
        resolved->previous_opposite_guide = previous;
        if (interleaved) {
            resolved->luke_interleaved_history = true;
            resolved->previous_serial = interleaved_older->serial;
            resolved->previous_same_native_camera = dlss_native_camera_history(*interleaved_older);
            resolved->previous_same_guide = interleaved_older;
        }
        if (std::distance(stream.luke_origin_history.begin(), origin) >= 2) {
            const auto& native_previous = *std::prev(origin, 2);
            if (native_previous->stream == current->stream &&
                native_previous->epoch == current->epoch &&
                native_previous->serial + 2 == current->serial &&
                native_previous->publication + 2 == current->publication) {
                // Only its immutable CPU camera is used. Current and N-1
                // snapshots already passed the GPU compatibility checks above.
                resolved->previous_same_guide = native_previous;
            }
        }
        ++state.statistics.matched;
        return resolved;
    } catch (...) {
        report_dlss_motion_vector_status(DlssMotionVectorStatus::invalid_input);
        return {};
    }
}

bool luke_native_stream_reset(std::uint64_t stream,std::uint64_t accepted_epoch,
    ID3D12Device* device) noexcept {
    try {
        if(!g_tracking_enabled.load(std::memory_order_acquire)||!stream||!accepted_epoch||!device)return false;
        auto& state=registry();std::scoped_lock lock(state.mutex);
        const auto found=state.streams.find(stream);
        if(found==state.streams.end())return false;
        const auto& s=found->second;
        return s.latest&&s.latest->camera_frame_token_valid&&s.latest->epoch>accepted_epoch&&
            same_identity(s.latest->motion_snapshot_device.Get(),device);
    }catch(...){return false;}
}
std::optional<LukeNativeOriginMetadata> luke_native_origin_metadata(
    std::uint32_t token,ID3D12Device* device) noexcept {
    try {
        if(!g_tracking_enabled.load(std::memory_order_acquire)||!token||!device)return {};
        auto& state=registry();std::scoped_lock lock(state.mutex);
        std::optional<LukeNativeOriginMetadata> result;
        for(const auto& [id,s]:state.streams)
            for(const auto& f:s.luke_origin_history)if(f->color_origin_token==token) {
                if(result||!f->camera_frame_token_valid||!same_identity(f->motion_snapshot_device.Get(),device))return {};
                result=LukeNativeOriginMetadata{id,f->epoch,f->publication,f->serial};
            }
        return result;
    }catch(...){return {};}
}
void report_dlss_motion_vector_status(DlssMotionVectorStatus status) noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        if (status == DlssMotionVectorStatus::temporal_mismatch) {
            ++state.statistics.temporal_rejections;
        } else if (status == DlssMotionVectorStatus::invalid_input) {
            ++state.statistics.invalid_rejections;
        }
        state.statistics.status = status;
    } catch (...) {
    }
}

void report_dlss_motion_vector_use() noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        ++state.statistics.used;
        state.statistics.last_used_publication = state.next_publication - 1;
        state.statistics.status = DlssMotionVectorStatus::used;
    } catch (...) {
    }
}

DlssMotionVectorStatistics dlss_motion_vector_statistics() noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        return state.statistics;
    } catch (...) {
        return {};
    }
}

}  // namespace xrfg
