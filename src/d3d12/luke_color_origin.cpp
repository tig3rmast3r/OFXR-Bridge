#include "xrfg/luke_color_origin.hpp"
#include "xrfg/luke_origin_inset.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <mutex>
#include <vector>
#include <cstdio>
#include <thread>
#include <condition_variable>
#include <functional>

namespace xrfg {
struct LukeColorOriginReadResult {
    std::mutex mutex;
    std::function<HRESULT(std::uint32_t*,HRESULT*,bool*)> decode;
    std::array<std::uint32_t,2> tokens{};
    std::array<HRESULT,2> results{};
    std::array<bool,2> nonblack{};
    HRESULT status{E_PENDING};
    bool completed{};
};
namespace {
bool origin_from_bottom() noexcept;
bool gta_color_origin() noexcept;
std::mutex g_crop_mutex;
LukeOriginCropGuard g_crop_guard;
// Black bars measured on the eye images by the reader (left|top|bottom, 16
// bits each) and the eye extent they were measured on. Writer and reader
// place the mark from the same values.
std::atomic<std::uint64_t> g_bars{}, g_bars_extent{};
LukeOriginBars stored_bars(unsigned width, unsigned height) noexcept {
    if (g_bars_extent.load(std::memory_order_acquire) != ((static_cast<std::uint64_t>(width) << 32) | height)) return {};
    const auto bits = g_bars.load(std::memory_order_acquire);
    return {static_cast<unsigned>(bits >> 32) & 0xffffu, static_cast<unsigned>(bits >> 16) & 0xffffu,
        static_cast<unsigned>(bits) & 0xffffu};
}
// Outlaws' final REAL VR color can translate the corner barcode beyond the
// eye boundary; its FPS text also covers that corner. Keep its exact token
// inside the eye, away from that overlay. GTA additionally masks the top of
// realtime scripted scenes; its bounded scan follows the full-size eye height.
// Black bars around the picture move it inside (luke_origin_inset_with_bars).
std::array<unsigned,2> origin_inset(unsigned color_width, unsigned color_height) noexcept {
    static const auto title=[] {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr,path,MAX_PATH);
        const auto* name=wcsrchr(path,L'\\');
        return std::wstring(name?name+1:path);
    }();
    auto inset=luke_origin_inset_for_title(title,color_height);
    if(gta_color_origin()) {
        std::scoped_lock lock(g_crop_mutex);
        inset[0]=g_crop_guard.configure(color_width,color_height);
    }
    return luke_origin_inset_with_bars(inset,
        stored_bars(color_width,color_height),color_width,color_height,origin_from_bottom());
}
bool bright_primary_origin() noexcept {
    static const bool selected=[] {
        wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);
        const auto* name=wcsrchr(path,L'\\');
        return luke_origin_bright_primary_for_title(name?name+1:path);
    }();
    return selected;
}
bool gta_color_origin() noexcept {
    static const bool result=[] {wchar_t p[MAX_PATH]{};GetModuleFileNameW(nullptr,p,MAX_PATH);
        const auto* n=wcsrchr(p,L'\\');return !_wcsicmp(n?n+1:p,L"GTA5_Enhanced.exe");}();
    return result;
}
// Uncharted: bottom-left, below its post-DLSS night sky and rain.
bool origin_from_bottom() noexcept {
    static const bool bottom=[] {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr,path,MAX_PATH);
        const auto* name=wcsrchr(path,L'\\');
        return luke_origin_from_bottom_for_title(name?name+1:path);
    }();
    return bottom;
}
using Microsoft::WRL::ComPtr;
struct Uploads {
    ComPtr<ID3D12Device> device;
    DXGI_FORMAT format{};
    UINT height{};
    std::array<ComPtr<ID3D12Resource>, 2> colors;        // black, dim white
    std::array<ComPtr<ID3D12Resource>, 2> bright_colors; // black, full white (float outputs)
};
struct OriginSearchJob {
    struct Image {
        std::uint64_t geometry{};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        LukeColorOriginDecode decoded{};
        bool rgb_fallback{};
    };
    std::array<Image,2> images{};
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12Fence> fence;
    UINT64 bytes{},value{};
    bool started{};
    std::atomic_bool ready{};
};
void start_origin_search(const std::shared_ptr<OriginSearchJob>& job) noexcept {
    if(!job||job->started||!job->fence||job->fence->GetCompletedValue()<job->value)return;
    try {
        std::thread([job] {
            try {
                // Copy to ordinary cached memory before decoding. In particular
                // do not borrow the frame reader's readback, lock or CPU pool.
                std::array<std::vector<std::byte>,2> pixels;
                void* mapped{};const D3D12_RANGE range{0,static_cast<SIZE_T>(job->bytes)};
                if(SUCCEEDED(job->readback->Map(0,&range,&mapped))) {
                    try {
                        for(unsigned i=0;i<2;++i)if(job->images[i].geometry) {
                            const auto& f=job->images[i].footprint;
                            const auto pitch=f.Footprint.Width*luke_origin_pixel_bytes(f.Footprint.Format);
                            pixels[i].resize(std::size_t(pitch)*f.Footprint.Height);
                            for(unsigned y=0;y<f.Footprint.Height;++y)
                                std::memcpy(pixels[i].data()+std::size_t(y)*pitch,
                                    static_cast<const std::byte*>(mapped)+f.Offset+std::size_t(y)*f.Footprint.RowPitch,pitch);
                        }
                    } catch(...) { const D3D12_RANGE empty{};job->readback->Unmap(0,&empty);throw; }
                    const D3D12_RANGE empty{};job->readback->Unmap(0,&empty);
                    for(unsigned i=0;i<2;++i)if(job->images[i].geometry&&!pixels[i].empty()) {
                        const auto& f=job->images[i].footprint.Footprint;
                        job->images[i].decoded=find_luke_origin_position({pixels[i].data(),f.Format,
                            f.Width,f.Height,f.Width*luke_origin_pixel_bytes(f.Format),job->images[i].rgb_fallback});
                    }
                }
            } catch(...) {} // Allocation/map failure leaves recovery pending.
            job->ready.store(true,std::memory_order_release);
        }).detach();
        job->started=true;
    } catch(...) {} // Keep the owned, already-submitted snapshot and retry.
}
struct Reader {
    std::mutex mutex;
    std::weak_ptr<LukeColorOriginReadResult> pending;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> readback;
    UINT64 bytes{}, last_fence{}, next_fence{1};
    HANDLE event{};
    bool poisoned{};
    std::array<LukeOriginRowWindow, 2> windows{}; // per color slot, under mutex
    std::array<LukeOriginSearchPosition,2> positions{};
    std::shared_ptr<OriginSearchJob> search;
    // Black bars: a measurement is adopted once kBarAgreement consecutive ones
    // agree within kBarTolerance pixels, so a dark scene beside a bar does not
    // move the mark from frame to frame. Under mutex.
    LukeOriginBars bar_candidate{};
    unsigned bar_agree{};
    std::uint64_t bar_extent{};
    std::uint64_t last_used{}; // GetTickCount64 of the last get_reader, under the cache mutex
    ~Reader() { if (event) CloseHandle(event); }
};
// Three sleeping helpers plus the caller: bounded CPU parallelism, no frame
// queue, GPU submission, skipped row or additional history. Process-lifetime
// ownership follows the existing pinned capture cache.
class OriginDecodeWorkers {
    std::mutex call_mutex_,mutex_;
    std::condition_variable work_,done_;
    std::uint64_t epoch_{};
    unsigned pending_{};
    std::function<void(unsigned)> job_;
    bool stopping_{};
public:
    OriginDecodeWorkers() {
        std::array<std::thread,3> threads;
        try { for(unsigned i=1;i<4;++i)threads[i-1]=std::thread([this,i] {
            std::uint64_t observed{};
            for(;;) {
                std::unique_lock lock(mutex_);
                work_.wait(lock,[&]{return stopping_ || epoch_!=observed;});
                if(stopping_)return;
                observed=epoch_;
                lock.unlock();job_(i);lock.lock();if(--pending_==0)done_.notify_one();
            }
        }); } catch(...) {
            {std::scoped_lock lock(mutex_);stopping_=true;}
            work_.notify_all();for(auto& t:threads)if(t.joinable())t.join();throw;
        }
        for(auto& t:threads)t.detach();
    }
    void run(std::function<void(unsigned)> job) {
        std::scoped_lock caller(call_mutex_);
        {std::scoped_lock lock(mutex_);job_=std::move(job);pending_=3;++epoch_;}
        work_.notify_all();job_(0);
        std::unique_lock lock(mutex_);done_.wait(lock,[&]{return pending_==0;});job_={};
    }
};
OriginDecodeWorkers& origin_decode_workers() {
    static auto* workers=new OriginDecodeWorkers;
    return *workers;
}
struct Cache {
    std::mutex mutex;
    std::vector<std::shared_ptr<Uploads>> uploads;
    std::vector<std::shared_ptr<Reader>> readers;
};
Cache& cache() {
    // Immutable uploads must outlive every original game's command-list use.
    // Deliberate bounded process-lifetime ownership, not an unretired frame ring.
    static Cache* value = new Cache();
    return *value;
}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* color, D3D12_RESOURCE_STATES from,
    D3D12_RESOURCE_STATES to) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {color, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    return barrier;
}
HRESULT immutable_upload(ID3D12Device* device, DXGI_FORMAT format, UINT height,
    bool white, ComPtr<ID3D12Resource>& output, bool bright = false) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(256) * height;
    desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(output.GetAddressOf()));
    if (FAILED(result)) return result;
    void* mapped{}; const D3D12_RANGE no_read{0, 0};
    result = output->Map(0, &no_read, &mapped);
    if (FAILED(result)) return result;
    std::memset(mapped, 0, static_cast<SIZE_T>(desc.Width));
    auto* bytes = static_cast<std::byte*>(mapped);
    const auto pixel_bytes = luke_origin_pixel_bytes(format);
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < 2; ++x)
            luke_origin_pixel(bytes + static_cast<UINT64>(y) * 256 + x * pixel_bytes, format, white, bright);
    const D3D12_RANGE written{0, static_cast<SIZE_T>(desc.Width)};
    output->Unmap(0, &written);
    return S_OK;
}
HRESULT get_uploads(ID3D12Device* device, DXGI_FORMAT format, UINT height,
    std::shared_ptr<Uploads>& output) {
    auto& c = cache(); std::scoped_lock lock(c.mutex);
    for (const auto& candidate : c.uploads)
        if (candidate->device.Get() == device && candidate->format == format && candidate->height == height) {
            output = candidate; return S_OK;
        }
    if (c.uploads.size() >= 16) return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
    auto created = std::make_shared<Uploads>();
    created->device = device; created->format = format; created->height = height;
    HRESULT result = immutable_upload(device, format, height, false, created->colors[0]);
    if (SUCCEEDED(result)) result = immutable_upload(device, format, height, true, created->colors[1]);
    if (SUCCEEDED(result)) result = immutable_upload(device, format, height, false, created->bright_colors[0], true);
    if (SUCCEEDED(result)) result = immutable_upload(device, format, height, true, created->bright_colors[1], true);
    if (FAILED(result)) return result;
    c.uploads.push_back(created); output = std::move(created);
    return S_OK;
}
HRESULT get_reader(ID3D12CommandQueue* queue, std::shared_ptr<Reader>& output) {
    auto& c = cache(); std::scoped_lock lock(c.mutex);
    const auto now = GetTickCount64();
    for (const auto& candidate : c.readers)
        if (candidate->queue.Get() == queue) { candidate->last_used = now; output = candidate; return S_OK; }
    if (c.readers.size() >= 8) {
        // Each OpenXR session brings its own queue, and REAL VR recreates the
        // session on every resolution or DLSS mode change. Retire the least
        // recently used reader nobody holds whose last copy has completed;
        // the cache owns its only reference, so no read is in flight.
        auto victim = c.readers.end();
        for (auto it = c.readers.begin(); it != c.readers.end(); ++it) {
            const auto& reader = *it;
            if (reader.use_count() != 1) continue;
            const auto completed = reader->fence->GetCompletedValue();
            if (completed != UINT64_MAX && completed < reader->last_fence) continue;
            if (victim == c.readers.end() || reader->last_used < (*victim)->last_used) victim = it;
        }
        if (victim == c.readers.end()) return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
        // 1194: origin reader retired; b = its queue, c = ms since its last use.
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage, S_OK, 1194,
            reinterpret_cast<std::uint64_t>((*victim)->queue.Get()), now - (*victim)->last_used);
        c.readers.erase(victim);
    }
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
    auto created = std::make_shared<Reader>(); created->queue = queue;
    HRESULT result = queue->GetDevice(IID_PPV_ARGS(created->device.GetAddressOf()));
    if (SUCCEEDED(result)) result = created->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(created->allocator.GetAddressOf()));
    if (SUCCEEDED(result)) result = created->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        created->allocator.Get(), nullptr, IID_PPV_ARGS(created->list.GetAddressOf()));
    if (SUCCEEDED(result)) result = created->list->Close();
    if (SUCCEEDED(result)) result = created->device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(created->fence.GetAddressOf()));
    if (FAILED(result)) return result;
    created->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!created->event) return HRESULT_FROM_WIN32(GetLastError());
    created->last_used = now;
    c.readers.push_back(created); output = std::move(created);
    return S_OK;
}
HRESULT wait_reader(Reader& reader, DWORD timeout_ms) {
    if (reader.poisoned) return E_FAIL;
    if (!reader.last_fence) return S_OK;
    const auto deadline=GetTickCount64()+timeout_ms;
    for(;;) {
        const auto completed = reader.fence->GetCompletedValue();
        if (completed == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
        if (completed >= reader.last_fence) return S_OK;
        const auto now=GetTickCount64();
        if(now>=deadline)return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        ResetEvent(reader.event);
        HRESULT result = reader.fence->SetEventOnCompletion(reader.last_fence, reader.event);
        if (FAILED(result)) return result;
        const DWORD wait = WaitForSingleObject(reader.event,static_cast<DWORD>(deadline-now));
        if (wait == WAIT_TIMEOUT) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        if (wait != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(GetLastError());
        // A registration from an earlier timeout can wake this auto-reset
        // event. Only the requested fence is proof; keep the original deadline.
    }
}


std::atomic<std::uint64_t> g_output_extent{}, g_color_extent{};
std::uint64_t extent_bits(unsigned width, unsigned height) noexcept {
    return (static_cast<std::uint64_t>(width) << 32) | height;
}
struct ReadTelemetry {
    std::mutex mutex;
    HANDLE file{INVALID_HANDLE_VALUE};
    std::uint64_t ok{}, missing{}, errors{}, last_ms{}, written{};
    bool opened{};
    ~ReadTelemetry() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
};
void report_read(HRESULT result, std::uint32_t token, UINT width, UINT height) noexcept {
    try {
        static ReadTelemetry telemetry;
        std::scoped_lock lock(telemetry.mutex);
        if (SUCCEEDED(result) && token) ++telemetry.ok;
        else if (result == E_PENDING || result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            ++telemetry.missing;
        else ++telemetry.errors;
        const auto now = GetTickCount64();
        if (telemetry.last_ms && now - telemetry.last_ms < 1000) return;
        telemetry.last_ms = now;
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage, result, 122, token,
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(telemetry.ok)) << 32) |
            static_cast<std::uint32_t>(telemetry.missing + telemetry.errors));
        if (!telemetry.opened) {
            telemetry.opened = true;
            HMODULE module{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&report_read), &module)) return;
            std::vector<wchar_t> path(32768);
            const auto count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
            if (!count || count >= path.size()) return;
            const auto destination = std::filesystem::path(path.data()).parent_path() /
                (L"ofxr-luke-origin-pid" + std::to_wstring(GetCurrentProcessId()) + L".log");
            telemetry.file = CreateFileW(destination.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ |
                FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (telemetry.file != INVALID_HANDLE_VALUE) {
                constexpr char header[] = "# V33008 compact token32+CRC16, disjoint256x8 coverage bands\n"
                    "# uptime_ms,read_ok,read_missing,read_error,last_hr,token,roi_width,roi_height\n";
                DWORD written{};
                WriteFile(telemetry.file, header, static_cast<DWORD>(sizeof(header)-1), &written, nullptr);
                telemetry.written = written;
            }
        }
        // Bounded tiny normal log, independent of diagnostic profile; no raw pixels,
        // per-frame flush, timing gate, altered admission, or GPU observation work.
        if (telemetry.file == INVALID_HANDLE_VALUE || telemetry.written >= 4*1024*1024) return;
        char row[256]{};
        const auto count = std::snprintf(row, sizeof(row), "%llu,%llu,%llu,%llu,%ld,%u,%u,%u\n",
            static_cast<unsigned long long>(now), static_cast<unsigned long long>(telemetry.ok),
            static_cast<unsigned long long>(telemetry.missing), static_cast<unsigned long long>(telemetry.errors),
            static_cast<long>(result), token, width, height);
        if (count > 0 && static_cast<std::size_t>(count) < sizeof(row)) {
            DWORD written{};
            if (WriteFile(telemetry.file, row, static_cast<DWORD>(count), &written, nullptr))
                telemetry.written += written;
        }
    } catch (...) { } // Observation never changes the decoder result.
}
struct ReadReport {
    std::uint32_t* token{};
    HRESULT result{E_UNEXPECTED};
    UINT width{}, height{};
    ~ReadReport() { if (token) report_read(result, *token, width, height); }
    HRESULT finish(HRESULT value) noexcept { result = value; return value; }
};
} // namespace

void configure_luke_color_origin_extent(unsigned width, unsigned height) noexcept {
    g_color_extent.store(extent_bits(width, height), std::memory_order_release);
}

// Bounded changed-geometry evidence only. Admission and marker layout unchanged.
static void report_origin_layout(bool writer, unsigned reason,
    std::uint64_t output, std::uint64_t eye) noexcept {
    try {
        struct State { std::uint64_t output{},eye{}; unsigned reason{},count{}; };
        static std::mutex mutex;
        static std::array<State,2> states{};
        std::scoped_lock lock(mutex);
        auto& state=states[writer?0:1];
        if(state.count>=16||(state.count&&state.output==output&&state.eye==eye&&state.reason==reason))return;
        state.output=output;state.eye=eye;state.reason=reason;++state.count;
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
            reason,writer?948:949,output,eye);
    } catch (...) { }
}

bool luke_color_origin_patch_layout(unsigned texture_width, unsigned texture_height,
    LukeColorOriginRect rect, LukeColorOriginPatch* patch) noexcept {
    if (!patch) return false;
    *patch = {};
    try {
        if (!rect.width) rect.width = texture_width;
        if (!rect.height) rect.height = texture_height;
        if (rect.width < kLukeOriginWidth || rect.height > 8192 ||
            static_cast<UINT64>(rect.x) + rect.width > texture_width ||
            static_cast<UINT64>(rect.y) + rect.height > texture_height) return false;
        g_output_extent.store(extent_bits(rect.width, rect.height), std::memory_order_release);
        const auto color_extent = g_color_extent.load(std::memory_order_acquire);
        if (!color_extent) return false;
        LukeColorOriginLayout layout;
        const auto inset=origin_inset(static_cast<UINT>(color_extent >> 32),static_cast<UINT>(color_extent));
        if (!compact_luke_origin_layout(rect.width, rect.height,
                static_cast<UINT>(color_extent >> 32), static_cast<UINT>(color_extent), layout,inset[0],inset[1],
                origin_from_bottom())) return false;
        const bool primary_bright=bright_primary_origin();
        for(unsigned band=0;band<layout.band_count;++band) {
            auto area=layout.bands[band];area.x+=rect.x;area.y+=rect.y;
            if(static_cast<UINT64>(area.x)+area.width>texture_width ||
                static_cast<UINT64>(area.y)+area.height>texture_height)return false;
            patch->bands[band]=area;patch->bright[band]=layout.bright[band]||primary_bright;
        }
        patch->band_count=layout.band_count;
        return patch->band_count!=0;
    } catch (...) { return false; }
}
void luke_color_origin_patch_pixels(const LukeColorOriginPatch& patch, DXGI_FORMAT format,
    std::uint32_t token, std::byte* pixels) noexcept {
    const auto bytes=luke_origin_pixel_bytes(format);
    if(!pixels||!bytes)return;
    for(unsigned band=0;band<patch.band_count;++band)
        for(unsigned y=0;y<kLukeOriginHeight;++y)
            for(unsigned x=0;x<kLukeOriginWidth;++x,pixels+=bytes)
                luke_origin_pixel(pixels,format,luke_origin_white(token,(x/2)*2),patch.bright[band]);
}
HRESULT mark_luke_color_origin(ID3D12GraphicsCommandList* list, ID3D12Resource* color,
    std::uint32_t token, D3D12_RESOURCE_STATES state, ID3D12Device* native_device,
    LukeColorOriginRect rect, DXGI_FORMAT proven_color_view, LukeColorOriginRect* written_rectangle) noexcept {
    if (written_rectangle) *written_rectangle = {};
    try {
        if (!list || !color || !token) return E_INVALIDARG;
        const auto desc = color->GetDesc();
        const auto format = luke_origin_upload_format(desc.Format, proven_color_view);
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
            desc.DepthOrArraySize != 1 || desc.Width > 16384 || !luke_origin_pixel_bytes(format))
            return DXGI_ERROR_UNSUPPORTED;
        if (!rect.width) rect.width = static_cast<UINT>(desc.Width);
        if (!rect.height) rect.height = desc.Height;
        if (rect.width < kLukeOriginWidth || rect.height > 8192 ||
            static_cast<UINT64>(rect.x) + rect.width > desc.Width ||
            static_cast<UINT64>(rect.y) + rect.height > desc.Height) return E_INVALIDARG;
        g_output_extent.store(extent_bits(rect.width, rect.height), std::memory_order_release);
        const auto color_extent = g_color_extent.load(std::memory_order_acquire);
        if (!color_extent) {
            report_origin_layout(true,1,extent_bits(rect.width,rect.height),0);
            return E_PENDING; // Actual eye viewport is not known during startup.
        }
        LukeColorOriginLayout layout;
        const auto inset=origin_inset(static_cast<UINT>(color_extent >> 32),static_cast<UINT>(color_extent));
        if (!compact_luke_origin_layout(rect.width, rect.height,
                static_cast<UINT>(color_extent >> 32), static_cast<UINT>(color_extent), layout,inset[0],inset[1],
                origin_from_bottom()))
        {
            report_origin_layout(true,2,extent_bits(rect.width,rect.height),color_extent);
            return E_PENDING;
        }
        ComPtr<ID3D12Device> device = native_device;
        if (!device) {
            const HRESULT result = list->GetDevice(IID_PPV_ARGS(device.GetAddressOf()));
            if (FAILED(result)) return result;
        }
        std::shared_ptr<Uploads> patterns;
        HRESULT result = get_uploads(device.Get(), format, kLukeOriginHeight, patterns);
        if (FAILED(result)) return result;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = color; destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        if (state != D3D12_RESOURCE_STATE_COPY_DEST) {
            auto barrier = transition(color, state, D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1, &barrier);
        }
        const bool primary_bright=bright_primary_origin();
        for(unsigned band=0;band<layout.band_count;++band) {
            const UINT x=rect.x+layout.bands[band].x,y=rect.y+layout.bands[band].y;
            for (unsigned cell = 0; cell < kLukeOriginBits * 2; ++cell) {
            const bool white = luke_origin_white(token, cell * 2);
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = ((layout.bright[band]||primary_bright) ? patterns->bright_colors : patterns->colors)[white ? 1 : 0].Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint.Footprint = {format, 2, kLukeOriginHeight, 1, 256};
                list->CopyTextureRegion(&destination, x + cell * 2, y, 0, &source, nullptr);
            }
        }
        if (written_rectangle) {
            UINT left=desc.Width,top=desc.Height,right=0,bottom=0;
            for(unsigned band=0;band<layout.band_count;++band) {
                const auto& area=layout.bands[band];
                left=std::min(left,rect.x+area.x);top=std::min(top,rect.y+area.y);
                right=std::max(right,rect.x+area.x+area.width);
                bottom=std::max(bottom,rect.y+area.y+area.height);
            }
            *written_rectangle={left,top,right-left,bottom-top};
        }
        if (state != D3D12_RESOURCE_STATE_COPY_DEST) {
            auto barrier = transition(color, D3D12_RESOURCE_STATE_COPY_DEST, state);
            list->ResourceBarrier(1, &barrier);
        }
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

namespace {
HRESULT read_origins(ID3D12CommandQueue* queue, ID3D12Resource* const* colors,
    std::uint32_t* tokens, HRESULT* results, unsigned count,
    D3D12_RESOURCE_STATES state, DWORD timeout_ms, bool* nonblack=nullptr,
    LukeColorOriginReadReceipt* receipt=nullptr) noexcept {
    if(receipt)*receipt={};
    struct Plan {
        D3D12_RESOURCE_DESC original{};
        LukeColorOriginLayout layout{};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        bool valid{};
    };
    std::array<Plan, 2> plans{};
    std::array<ReadReport, 2> reports{};
    for (unsigned i=0; i<count; ++i) {
        tokens[i]=0; results[i]=E_UNEXPECTED; reports[i].token=colors[i]?&tokens[i]:nullptr;
        if(nonblack)nonblack[i]=false;
    }
    const auto finish = [&](HRESULT hr) {
        for (unsigned i=0; i<count; ++i) {
            if (results[i]==E_UNEXPECTED) results[i]=hr;
            if(colors[i])reports[i].finish(results[i]);
        }
        return hr;
    };
    try {
        if (!queue) return finish(E_POINTER);
        std::shared_ptr<Reader> reader;
        HRESULT result=get_reader(queue,reader);
        if(FAILED(result))return finish(result);
        std::scoped_lock lock(reader->mutex);
        if(!reader->pending.expired())return finish(DXGI_ERROR_WAS_STILL_DRAWING);
        if(reader->search) {
            start_origin_search(reader->search);
            if(reader->search->ready.load(std::memory_order_acquire)) {
                for(unsigned i=0;i<2;++i) {
                    const auto& found=reader->search->images[i];auto& position=reader->positions[i];
                    if(found.geometry&&found.geometry==position.geometry&&position.lost&&
                        position.locate(found.decoded,0,0,found.footprint.Footprint.Width,found.footprint.Footprint.Height))
                        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,0,1323,
                            (std::uint64_t(position.region.x)<<32)|position.region.y,
                            (std::uint64_t(position.region.width)<<32)|position.region.height);
                }
                reader->search.reset();
            }
        }
        const auto output_extent=g_output_extent.load(std::memory_order_acquire);
        unsigned valid_count{};
        for (unsigned i=0; i<count; ++i) {
            auto& plan=plans[i];
            if (!colors[i]) { results[i]=E_POINTER; continue; }
            plan.original=colors[i]->GetDesc();
            const auto& original=plan.original;
            if (original.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || original.SampleDesc.Count!=1 ||
                original.DepthOrArraySize!=1 || original.Width>16384 || original.Width<kLukeOriginWidth ||
                !original.Height || !luke_origin_pixel_bytes(original.Format)) {
                results[i]=DXGI_ERROR_UNSUPPORTED; continue;
            }
            configure_luke_color_origin_extent(static_cast<UINT>(original.Width),original.Height);
            if (!output_extent) {
                report_origin_layout(false,1,0,extent_bits(static_cast<UINT>(original.Width),original.Height));
                results[i]=E_PENDING; continue;
            }
            const auto inset=origin_inset(static_cast<UINT>(original.Width),original.Height);
            if (!compact_luke_origin_layout(static_cast<UINT>(output_extent>>32),static_cast<UINT>(output_extent),
                    static_cast<UINT>(original.Width),original.Height,plan.layout,inset[0],inset[1],origin_from_bottom())) {
                report_origin_layout(false,2,output_extent,extent_bits(static_cast<UINT>(original.Width),original.Height));
                results[i]=E_PENDING; continue;
            }
            if(gta_color_origin()) {
                // Include earlier marker positions during feedback changes.
                // A moving scan origin could otherwise hide the previous frame.
                plan.layout.scan.width+=plan.layout.scan.x;
                plan.layout.scan.x=0;
            }
            std::uint64_t key=1469598103934665603ULL;
            for(const std::uint64_t value:{original.Width,std::uint64_t(original.Height),std::uint64_t(original.Format)})
                {key^=value;key*=1099511628211ULL;}
            auto& position=reader->positions[i];position.configure(key|1);
            if(position.region.width&&position.region.height)plan.layout.scan=position.region;
            plan.valid=true; ++valid_count;
            reports[i].width=plan.layout.scan.width; reports[i].height=plan.layout.scan.height;
        }
        if (!valid_count) return finish(results[0]);
        const bool timing=bridge_flight_logger().ram_enabled() || bridge_flight_logger().enabled();
        const auto phase=[&](unsigned stage,HRESULT hr) {
            if (timing) bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
                hr,stage,reinterpret_cast<std::uint64_t>(reader.get()),reader->last_fence);
        };
        phase(120,S_OK);
        result=wait_reader(*reader,timeout_ms); phase(121,result);
        if (FAILED(result)) return finish(result); // Retire before any allocator/readback reuse.
        UINT64 bytes{};
        for (auto& plan:plans) if (plan.valid) {
            auto roi=plan.original; roi.Width=plan.layout.scan.width;
            roi.Height=plan.layout.scan.height; roi.MipLevels=1;
            const UINT64 offset=(bytes+D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1) &
                ~(UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT)-1);
            reader->device->GetCopyableFootprints(&roi,0,1,offset,&plan.footprint,nullptr,nullptr,nullptr);
            bytes=plan.footprint.Offset+UINT64(plan.footprint.Footprint.RowPitch)*roi.Height;
        }
        std::shared_ptr<OriginSearchJob> search;
        const auto now=GetTickCount64();
        if(!reader->search) {
            for(unsigned i=0;i<count;++i)if(plans[i].valid&&reader->positions[i].scan_due(now)) {
                reader->positions[i].scanned(now);
                if(!search)search=std::make_shared<OriginSearchJob>();
                auto& image=search->images[i];image.geometry=reader->positions[i].geometry;
                image.rgb_fallback=gta_color_origin();
                auto desc=plans[i].original;desc.MipLevels=1;
                const auto offset=(search->bytes+D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1)&
                    ~(UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT)-1);
                reader->device->GetCopyableFootprints(&desc,0,1,offset,&image.footprint,nullptr,nullptr,nullptr);
                search->bytes=image.footprint.Offset+UINT64(image.footprint.Footprint.RowPitch)*desc.Height;
            }
            if(search) {
                D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
                desc.Width=search->bytes;desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
                desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                const auto hr=reader->device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
                    D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&search->readback));
                bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,hr,1322,search->bytes,now);
                if(FAILED(hr))search.reset(); // Compact reads continue independently.
            }
        }
        // Black bars: the middle column (top and bottom) and middle row (left)
        // of the first valid eye.
        int bars_from=-1;
        for (unsigned i=0;i<count;++i) if (plans[i].valid) { bars_from=static_cast<int>(i); break; }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT column_footprint{}, row_footprint{};
        if (bars_from>=0) {
            const auto& original=plans[bars_from].original;
            const auto place=[&](UINT64 width, UINT height, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint) {
                auto strip=original; strip.Width=width; strip.Height=height; strip.MipLevels=1;
                const UINT64 offset=(bytes+D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1) &
                    ~(UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT)-1);
                reader->device->GetCopyableFootprints(&strip,0,1,offset,&footprint,nullptr,nullptr,nullptr);
                bytes=footprint.Offset+UINT64(footprint.Footprint.RowPitch)*height;
            };
            place(1,original.Height,column_footprint);
            place(original.Width,1,row_footprint);
        }
        if (!reader->readback || reader->bytes!=bytes) {
            D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_READBACK;
            heap.CreationNodeMask=heap.VisibleNodeMask=1;
            D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width=bytes; desc.Height=1; desc.DepthOrArraySize=desc.MipLevels=1;
            desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> buffer;
            result=reader->device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
                D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(buffer.GetAddressOf()));
            if (FAILED(result)) return finish(result);
            reader->readback=std::move(buffer); reader->bytes=bytes;
        }
        // The batch decoder is noexcept and fails closed, so every Map below
        // reaches Unmap even when worker or scratch allocation fails.

        result=reader->allocator->Reset();
        if (SUCCEEDED(result)) result=reader->list->Reset(reader->allocator.Get(),nullptr);
        if (FAILED(result)) return finish(result);
        for (unsigned i=0; i<count; ++i) if (plans[i].valid) {
            const auto& plan=plans[i];
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource=colors[i]; source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource=reader->readback.Get(); destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint=plan.footprint;
            if (state!=D3D12_RESOURCE_STATE_COPY_SOURCE) {
                auto barrier=transition(colors[i],state,D3D12_RESOURCE_STATE_COPY_SOURCE);
                reader->list->ResourceBarrier(1,&barrier);
            }
            const auto& scan=plan.layout.scan;
            const D3D12_BOX box{scan.x,scan.y,0,scan.x+scan.width,scan.y+scan.height,1};
            reader->list->CopyTextureRegion(&destination,0,0,0,&source,&box);
            if(search&&search->images[i].geometry) {
                destination.pResource=search->readback.Get();
                destination.PlacedFootprint=search->images[i].footprint;
                reader->list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
                destination.pResource=reader->readback.Get();
            }
            if (static_cast<int>(i)==bars_from) {
                const UINT width=static_cast<UINT>(plan.original.Width),height=plan.original.Height;
                destination.PlacedFootprint=column_footprint;
                const D3D12_BOX column{width/2,0,0,width/2+1,height,1};
                reader->list->CopyTextureRegion(&destination,0,0,0,&source,&column);
                destination.PlacedFootprint=row_footprint;
                const D3D12_BOX row{0,height/2,0,width,height/2+1,1};
                reader->list->CopyTextureRegion(&destination,0,0,0,&source,&row);
            }
            if (state!=D3D12_RESOURCE_STATE_COPY_SOURCE) {
                auto barrier=transition(colors[i],D3D12_RESOURCE_STATE_COPY_SOURCE,state);
                reader->list->ResourceBarrier(1,&barrier);
            }
        }
        result=reader->list->Close();
        if (FAILED(result)) return finish(result);
        ID3D12CommandList* lists[]{reader->list.Get()};
        if(receipt)receipt->submitted=true;
        if(search)reader->search=search; // Own the snapshot even if Signal fails.
        queue->ExecuteCommandLists(1,lists);
        const auto value=reader->next_fence++;
        result=queue->Signal(reader->fence.Get(),value);
        if (FAILED(result)) { reader->poisoned=true; return finish(result); }
        if(search) {search->fence=reader->fence;search->value=value;}
        if(receipt) {receipt->fence=reader->fence;receipt->value=value;}
        reader->last_fence=value;
        phase(126,static_cast<HRESULT>(valid_count)); // Number of colors in this submission.
        phase(123,result);
        const auto decode=[reader,plans,count,bytes,bars_from,column_footprint,row_footprint](
            std::uint32_t* tokens,HRESULT* results,bool* nonblack) -> HRESULT {
            HRESULT result{};
            const auto phase=[&](unsigned stage,HRESULT hr) {
                bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
                    hr,stage,reinterpret_cast<std::uint64_t>(reader.get()),reader->last_fence);
            };
        start_origin_search(reader->search);
        void* mapped{}; const D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};
        result=reader->readback->Map(0,&range,&mapped);
        if (FAILED(result)) return result;
        if (bars_from>=0) {
            const auto& original=plans[bars_from].original;
            const auto bpp=luke_origin_pixel_bytes(original.Format);
            const auto* base=static_cast<const std::byte*>(mapped);
            std::vector<float> column(original.Height),row(static_cast<std::size_t>(original.Width));
            for (UINT y=0;y<original.Height;++y)
                column[y]=luke_origin_luma(base+column_footprint.Offset+UINT64(column_footprint.Footprint.RowPitch)*y,original.Format);
            for (UINT64 x=0;x<original.Width;++x)
                row[static_cast<std::size_t>(x)]=luke_origin_luma(base+row_footprint.Offset+x*bpp,original.Format);
            LukeOriginBars measured{};
            if (luke_origin_black_run(column,false,measured.top) && luke_origin_black_run(column,true,measured.bottom) &&
                luke_origin_black_run(row,false,measured.left)) {
                constexpr unsigned kBarAgreement=8, kBarTolerance=2;
                const auto extent=extent_bits(static_cast<UINT>(original.Width),original.Height);
                const auto within=[](unsigned a,unsigned b){return (a>b?a-b:b-a)<=kBarTolerance;};
                auto& candidate=reader->bar_candidate;
                if (reader->bar_extent==extent && within(candidate.left,measured.left) &&
                    within(candidate.top,measured.top) && within(candidate.bottom,measured.bottom)) ++reader->bar_agree;
                else { candidate=measured; reader->bar_agree=1; reader->bar_extent=extent; }
                if (reader->bar_agree==kBarAgreement) {
                    const auto clamp16=[](unsigned v){return static_cast<std::uint64_t>(std::min(v,0xffffu));};
                    const auto bits=(clamp16(candidate.left)<<32)|(clamp16(candidate.top)<<16)|clamp16(candidate.bottom);
                    if (g_bars.load(std::memory_order_acquire)!=bits || g_bars_extent.load(std::memory_order_acquire)!=extent) {
                        g_bars.store(bits,std::memory_order_release);
                        g_bars_extent.store(extent,std::memory_order_release);
                        // 1250: black bars adopted; b = left<<32 | top<<16 | bottom, c = eye extent.
                        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,S_OK,1250,bits,extent);
                    }
                }
            }
        }
        std::array<LukeColorOriginImage,2> image_rows{};
        std::array<LukeColorOriginDecode,2> decoded{};
        std::array<std::uint64_t,2> geometry{};
        for(unsigned i=0;i<count;++i)if(plans[i].valid) {
            const auto& p=plans[i];
            image_rows[i]={static_cast<const std::byte*>(mapped)+p.footprint.Offset,
                p.original.Format,p.layout.scan.width,p.layout.scan.height,p.footprint.Footprint.RowPitch};
            // FNV-1a over everything that places the band inside this scan.
            std::uint64_t key=1469598103934665603ULL;
            for(const std::uint64_t value:{p.original.Width,std::uint64_t(p.original.Height),
                    std::uint64_t(p.original.Format),std::uint64_t(p.layout.scan.x),std::uint64_t(p.layout.scan.y),
                    std::uint64_t(p.layout.scan.width),std::uint64_t(p.layout.scan.height)})
                {key^=value;key*=1099511628211ULL;}
            geometry[i]=key|1;
        }
        for(unsigned i=0;i<count;++i)image_rows[i].rgb_fallback=gta_color_origin();
        std::array<bool,2> local_nonblack{};
        auto* observed_nonblack=nonblack?nonblack:local_nonblack.data();
        const auto narrowed=decode_luke_origin_images_windowed(std::span(image_rows.data(),count),
            std::span(geometry.data(),count),std::span(reader->windows.data(),count),
            std::span(decoded.data(),count),(nonblack||gta_color_origin())?std::span<bool>(observed_nonblack,count):std::span<bool>{});
        phase(127,static_cast<HRESULT>(narrowed)); // Bit per color decoded from its learned rows.
        HRESULT decode_status=S_OK;
        for(unsigned i=0;i<count;++i)if(plans[i].valid) {
            results[i]=decoded[i].ambiguous ? HRESULT_FROM_WIN32(ERROR_INVALID_DATA) :
                decoded[i].token ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            if(SUCCEEDED(results[i]))tokens[i]=decoded[i].token;else decode_status=results[i];
            const auto& p=plans[i];
            reader->positions[i].observed(decoded[i],p.layout.scan.x,p.layout.scan.y,
                static_cast<unsigned>(p.original.Width),p.original.Height,GetTickCount64());
        }
        if(gta_color_origin()) {
            bool valid=false,color=false,ambiguous=false;int left=-1;
            for(unsigned i=0;i<count;++i)if(plans[i].valid) {
                color|=observed_nonblack[i];ambiguous|=decoded[i].ambiguous;
                if(decoded[i].token&&!decoded[i].ambiguous) {
                    valid=true;
                    const auto x=decoded[i].first_column;
                    if(x>=0)left=left<0?x:std::min(left,x);
                }
            }
            // A single readable eye must not mask a consistently clipped peer.
            bool any_missing=false;
            for(unsigned i=0;i<count;++i)if(plans[i].valid&&!decoded[i].token&&observed_nonblack[i])any_missing=true;
            unsigned inset{};bool changed=false;
            {
                std::scoped_lock crop_lock(g_crop_mutex);
                changed=g_crop_guard.observe(valid&&!any_missing,color,ambiguous,left);
                inset=g_crop_guard.inset;
            }
            if(changed)bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
                valid?1:0,1320,inset,left<0?0:static_cast<unsigned>(left));
        }
        const D3D12_RANGE no_write{0,0}; reader->readback->Unmap(0,&no_write);
        phase(125,decode_status);
        return S_OK;
        };
        result=wait_reader(*reader,timeout_ms); phase(124,result);
        if (FAILED(result)) {
            if(receipt && (result==HRESULT_FROM_WIN32(WAIT_TIMEOUT) || result==DXGI_ERROR_WAS_STILL_DRAWING)) {
                auto pending=std::make_shared<LukeColorOriginReadResult>();
                for(unsigned i=0;i<count;++i)pending->results[i]=results[i];
                pending->decode=[reader,decode,plans,count](std::uint32_t* tokens,HRESULT* results,bool* nonblack) {
                    std::scoped_lock lock(reader->mutex);
                    const auto hr=decode(tokens,results,nonblack);
                    if(SUCCEEDED(hr))for(unsigned i=0;i<count;++i)if(plans[i].valid)
                        report_read(results[i],tokens[i],plans[i].layout.scan.width,plans[i].layout.scan.height);
                    reader->pending.reset();return hr;
                };
                reader->pending=pending;receipt->pending=std::move(pending);
            }
            bridge_flight_logger().event(BridgeFlightOperation::gpu_fence_snapshot,49305,
                reinterpret_cast<std::uintptr_t>(reader->fence.Get()),value,reader->fence->GetCompletedValue());
            bridge_flight_logger().event(BridgeFlightOperation::gpu_device_status,
                reader->device->GetDeviceRemovedReason(),reinterpret_cast<std::uintptr_t>(queue),
                reinterpret_cast<std::uintptr_t>(reader->device.Get()),49305);
            return finish(result);
        }
        return finish(decode(tokens,results,nonblack));
    } catch (const std::bad_alloc&) { return finish(E_OUTOFMEMORY); }
      catch (...) { return finish(E_FAIL); }
}
} // namespace

void retire_luke_color_origin_reader(ID3D12CommandQueue* queue) noexcept {
    try {
        auto& c=cache();std::scoped_lock lock(c.mutex);
        std::erase_if(c.readers,[&](const auto& reader) {
            if(reader->queue.Get()!=queue || reader.use_count()!=1)return false;
            const auto completed=reader->fence->GetCompletedValue();
            return completed==UINT64_MAX || completed>=reader->last_fence;
        });
    }catch(...) {}
}

HRESULT poll_luke_color_origin_pair(const LukeColorOriginReadReceipt& receipt,
    std::array<std::uint32_t,2>& tokens,std::array<HRESULT,2>& results,
    std::array<bool,2>* nonblack) noexcept {
    tokens={};results={E_PENDING,E_PENDING};
    if(!receipt.pending || !receipt.fence || !receipt.value)return E_INVALIDARG;
    const auto completed=receipt.fence->GetCompletedValue();
    if(completed==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
    if(completed<receipt.value)return E_PENDING;
    try {
        auto& read=*receipt.pending;std::scoped_lock lock(read.mutex);
        if(!read.completed) {
            read.status=read.decode(read.tokens.data(),read.results.data(),read.nonblack.data());
            read.decode={};read.completed=true;
        }
        tokens=read.tokens;results=read.results;if(nonblack)*nonblack=read.nonblack;
        return read.status;
    }catch(...){return E_FAIL;}
}

LukeColorOriginDecode find_luke_origin_position(const LukeColorOriginImage& image) noexcept {
    // One row cannot enter the parallel decoder pool. A slow recovery search
    // must never hold that pool's call mutex against the live frame decoder.
    if(!image.pixels||image.height>8192)return {};
    for(unsigned y=0;y<image.height;++y) {
        auto row=image;row.pixels+=std::size_t(y)*image.row_pitch;row.height=1;
        LukeColorOriginDecode decoded{};
        decode_luke_origin_images(std::span(&row,1),std::span(&decoded,1));
        if(decoded.token&&!decoded.ambiguous&&decoded.first_column>=0) {
            decoded.first_row=decoded.last_row=static_cast<int>(y);
            return decoded;
        }
    }
    return {};
}

void decode_luke_origin_images(std::span<const LukeColorOriginImage> images,
    std::span<LukeColorOriginDecode> results, std::span<bool> nonblack) noexcept {
    if(results.size()!=images.size())return;
    std::fill(results.begin(),results.end(),LukeColorOriginDecode{});
    const bool observe_color=nonblack.size()==images.size()&&!nonblack.empty();
    if(observe_color)std::fill(nonblack.begin(),nonblack.end(),false);
    try {
        std::size_t total{};
        std::vector<std::size_t> offsets{0};
        for(const auto& image:images) {
            if(image.pixels && luke_origin_pixel_bytes(image.format) && image.width<=16384 &&
                image.height<=8192 && image.row_pitch>=image.width*luke_origin_pixel_bytes(image.format))total+=image.height;
            offsets.push_back(total);
        }
        std::vector<LukeColorOriginDecode> rows(total);
        std::vector<unsigned char> color_rows(observe_color?total:0);
        const auto decode=[&](unsigned worker,unsigned step) noexcept {
            try {
                std::vector<float> row;
                for(unsigned i=0;i<images.size();++i) {
                    const auto& image=images[i];const auto bpp=luke_origin_pixel_bytes(image.format);
                    if(offsets[i]==offsets[i+1])continue;
                    row.resize(image.width);
                    for(std::size_t index=offsets[i]+((worker+step-offsets[i]%step)%step);index<offsets[i+1];index+=step) {
                        const auto* pixels=image.pixels+(index-offsets[i])*image.row_pitch;
                        for(unsigned x=0;x<image.width;++x)row[x]=luke_origin_luma(pixels+x*bpp,image.format);
                        rows[index]=decode_luke_origin_row(row);
                        if(observe_color)color_rows[index]=std::any_of(row.begin(),row.end(),
                            [](float v){return std::isfinite(v)&&v!=0.F;});
                    }
                }
            } catch(...) {for(std::size_t i=worker;i<rows.size();i+=step)rows[i].ambiguous=true;}
        };
        if(total>=64) {
            origin_decode_workers().run([&](unsigned worker) noexcept {decode(worker,4);});
        } else decode(0,1);
        for(unsigned i=0;i<images.size();++i) {
            // The rows reported are one band: the longest run of decoded rows
            // (later on a tie). A layout with a bright copy 64 rows above the
            // band (Uncharted at night) otherwise learned both bands and the
            // gap, so every narrowed read decoded 104 rows for 8 that matter.
            int run_first=-1,run_last=-1,best_first=-1,best_last=-1;
            auto& result=results[i];
            for(std::size_t y=offsets[i];y<offsets[i+1];++y) {
                const auto row=rows[y];result.ambiguous|=row.ambiguous;
                if(observe_color)nonblack[i]=nonblack[i]||color_rows[y]!=0;
                if(!row.token)continue;
                if(result.token&&result.token!=row.token)result.ambiguous=true;result.token=row.token;
                if(row.first_column>=0)result.first_column=result.first_column<0?row.first_column:std::min(result.first_column,row.first_column);
                result.last_column=std::max(result.last_column,row.last_column);
                const auto scan_row=static_cast<int>(y-offsets[i]);
                if(run_first<0||scan_row>run_last+2)run_first=scan_row;
                run_last=scan_row;
                if(best_first<0||run_last-run_first>=best_last-best_first) {best_first=run_first;best_last=run_last;}
            }
            result.first_row=best_first;result.last_row=best_last;
        }
        for(unsigned i=0;i<images.size();++i) {
            const auto& image=images[i];auto& result=results[i];
            if(result.token||result.ambiguous||!image.rgb_fallback||offsets[i]==offsets[i+1])continue;
            const bool rgba=image.format==DXGI_FORMAT_R8G8B8A8_UNORM||image.format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||image.format==DXGI_FORMAT_R8G8B8A8_TYPELESS;
            const bool bgra=image.format==DXGI_FORMAT_B8G8R8A8_UNORM||image.format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB||image.format==DXGI_FORMAT_B8G8R8A8_TYPELESS;
            if(!rgba&&!bgra)continue;
            // RGB-only recovery must report its band just like luminance.
            // Otherwise a valid token never learns a row window and every
            // frame repeats the expensive full scan. Keep all three channels
            // and all rows in that scan, including conflicting CRC identities.
            std::vector<LukeColorOriginDecode> rgb_rows(image.height);
            const auto rgb_decode=[&](unsigned worker,unsigned step) noexcept {
                try {
                    std::vector<float> row(image.width);
                    for(unsigned y=worker;y<image.height;y+=step) {
                        const auto* pixels=image.pixels+y*image.row_pitch;
                        auto& found=rgb_rows[y];
                        for(unsigned channel=0;channel<3;++channel) {
                            for(unsigned x=0;x<image.width;++x)
                                row[x]=std::to_integer<unsigned char>(pixels[x*4+channel])/255.F;
                            const auto decoded=decode_luke_origin_row(row);
                            found.ambiguous|=decoded.ambiguous;
                            if(decoded.token) {
                                if(decoded.first_column>=0)found.first_column=found.first_column<0?decoded.first_column:std::min(found.first_column,decoded.first_column);
                                found.last_column=std::max(found.last_column,decoded.last_column);
                                if(found.token&&found.token!=decoded.token)found.ambiguous=true;
                                found.token=decoded.token;
                            }
                        }
                    }
                } catch(...) {for(unsigned y=worker;y<image.height;y+=step)rgb_rows[y].ambiguous=true;}
            };
            if(image.height>=32)origin_decode_workers().run([&](unsigned worker) noexcept {rgb_decode(worker,4);});
            else rgb_decode(0,1);
            int run_first=-1,run_last=-1,best_first=-1,best_last=-1;
            for(unsigned y=0;y<image.height;++y) {
                const auto& decoded=rgb_rows[y];result.ambiguous|=decoded.ambiguous;
                if(!decoded.token)continue;
                if(result.token&&result.token!=decoded.token)result.ambiguous=true;
                result.token=decoded.token;
                if(decoded.first_column>=0)result.first_column=result.first_column<0?decoded.first_column:std::min(result.first_column,decoded.first_column);
                result.last_column=std::max(result.last_column,decoded.last_column);
                const auto scan_row=static_cast<int>(y);
                if(run_first<0||scan_row>run_last+2)run_first=scan_row;
                run_last=scan_row;
                if(best_first<0||run_last-run_first>=best_last-best_first) {best_first=run_first;best_last=run_last;}
            }
            result.first_row=best_first;result.last_row=best_last;
        }
    } catch(...) {for(auto& result:results)result={0,true};}
}
unsigned decode_luke_origin_images_windowed(std::span<const LukeColorOriginImage> images,
    std::span<const std::uint64_t> geometry, std::span<LukeOriginRowWindow> windows,
    std::span<LukeColorOriginDecode> results,std::span<bool> nonblack) noexcept {
    constexpr std::size_t capacity=8;
    const auto count=images.size();
    if(geometry.size()!=count||windows.size()!=count||results.size()!=count||count>capacity) {
        decode_luke_origin_images(images,results,nonblack);return 0;
    }
    std::array<LukeColorOriginImage,capacity> first{};
    std::array<unsigned,capacity> top{};
    unsigned narrowed{};
    for(std::size_t i=0;i<count;++i) {
        first[i]=images[i];
        auto& window=windows[i];
        if(!geometry[i]||window.geometry!=geometry[i]) {window={};continue;}
        const auto& image=images[i];
        if(!image.pixels||window.first>window.last||window.last>=image.height)continue;
        const unsigned begin=window.first>kLukeOriginWindowMargin?window.first-kLukeOriginWindowMargin:0;
        const unsigned end=std::min(image.height-1,window.last+kLukeOriginWindowMargin);
        if(end-begin+1>=image.height)continue;
        first[i].pixels=image.pixels+static_cast<std::size_t>(begin)*image.row_pitch;
        first[i].height=end-begin+1;
        top[i]=begin;narrowed|=1U<<i;
    }
    decode_luke_origin_images(std::span(first.data(),count),results,nonblack);
    std::array<LukeColorOriginImage,capacity> retry{};
    std::array<std::size_t,capacity> retry_index{};
    std::size_t retries{};
    for(std::size_t i=0;i<count;++i) {
        if(!(narrowed&(1U<<i)))continue;
        auto& result=results[i];
        if(result.token&&!result.ambiguous&&result.first_row>=0) {
            // Follow small drift; the margin still bounds what is skipped.
            result.first_row+=static_cast<int>(top[i]);result.last_row+=static_cast<int>(top[i]);
            auto& window=windows[i];
            window.first=static_cast<unsigned>(result.first_row);
            window.last=static_cast<unsigned>(result.last_row);
            continue;
        }
        narrowed&=~(1U<<i);
        retry[retries]=images[i];retry_index[retries++]=i;
    }
    if(retries) {
        std::array<LukeColorOriginDecode,capacity> whole{};
        std::array<bool,capacity> whole_nonblack{};
        decode_luke_origin_images(std::span(retry.data(),retries),std::span(whole.data(),retries),nonblack.empty()?std::span<bool>{}:std::span<bool>(whole_nonblack.data(),retries));
        for(std::size_t k=0;k<retries;++k){results[retry_index[k]]=whole[k];if(!nonblack.empty())nonblack[retry_index[k]]=whole_nonblack[k];}
    }
    for(std::size_t i=0;i<count;++i) {
        if(narrowed&(1U<<i))continue;
        // Whole-scan result: learn its rows, or forget a stale window.
        const auto& result=results[i];
        windows[i]=geometry[i]&&result.token&&!result.ambiguous&&result.first_row>=0
            ? LukeOriginRowWindow{geometry[i],static_cast<unsigned>(result.first_row),
                static_cast<unsigned>(result.last_row)}
            : LukeOriginRowWindow{};
    }
    return narrowed;
}
HRESULT read_luke_color_origin(ID3D12CommandQueue* queue, ID3D12Resource* color,
    std::uint32_t* token, D3D12_RESOURCE_STATES state, DWORD timeout_ms) noexcept {
    if (!token) return E_POINTER;
    HRESULT result{};
    read_origins(queue,&color,token,&result,1,state,timeout_ms);
    return result;
}
HRESULT read_luke_color_origin_pair(ID3D12CommandQueue* queue,
    const std::array<ID3D12Resource*,2>& colors, std::array<std::uint32_t,2>& tokens,
    std::array<HRESULT,2>& results, D3D12_RESOURCE_STATES state, DWORD timeout_ms,
    std::array<bool,2>* nonblack, LukeColorOriginReadReceipt* receipt) noexcept {
    return read_origins(queue,colors.data(),tokens.data(),results.data(),2,state,timeout_ms,
        nonblack?nonblack->data():nullptr,receipt);
}
} // namespace xrfg
