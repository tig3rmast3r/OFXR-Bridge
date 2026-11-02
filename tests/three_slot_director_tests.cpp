// Compile the actual layer implementation into this test: no test-only switch
// or alternate director implementation is added to the distributed DLL.
#include "../src/layer/openxr_layer.cpp"
#include <iostream>
#include <stdexcept>
#include <fstream>
#include "ram_timing_fixture.hpp"
#include "autonomous_layer_fixture.hpp"
#include "xrfg/native_vulkan_transport_test.hpp"
namespace xrfg {int run_luke_ngx_dispatch_test(const wchar_t*,bool);}
namespace xrfg {int run_dead_space_ngx_capture_test(const wchar_t*,const wchar_t*);}
namespace xrfg {int run_gta_enhanced_ngx_capture_test(const wchar_t*,const wchar_t*);}
namespace xrfg {int run_dead_space_ingress_diagnostics_test(const wchar_t*,const wchar_t*);}
namespace xrfg {int run_ghost_ngx_capture_test(const wchar_t*,const wchar_t*,bool);}
namespace xrfg {int run_unlisted_motion_capture_test(const wchar_t*,const wchar_t*,bool);}
namespace xrfg {int run_horizon_ngx_capture_test(const wchar_t*,const wchar_t*,bool);}
namespace xrfg {int run_actual_ngx_capture_test(const wchar_t*,const wchar_t*); int run_crimson_ngx_capture_test(const wchar_t*,const wchar_t*);}
namespace xrfg {int run_kcd_logical_capture_test(const wchar_t*,const wchar_t*);}
namespace xrfg {int run_kcd_scene_capture_test(const wchar_t*,const wchar_t*,
    HRESULT (*)(bool,ID3D12Device*,ID3D12CommandQueue*,Microsoft::WRL::ComPtr<ID3D12CommandQueue>&));}
namespace xrfg {int run_luke_private_capture_test(const wchar_t*,const wchar_t*,
    HRESULT (*)(bool,ID3D12Device*,ID3D12CommandQueue*,Microsoft::WRL::ComPtr<ID3D12CommandQueue>&));}
namespace xrfg {int run_outlaws_logical_capture_test(const wchar_t*,const wchar_t*,
    HRESULT (*)(bool,ID3D12Device*,ID3D12CommandQueue*,Microsoft::WRL::ComPtr<ID3D12CommandQueue>&));}

namespace {
#ifdef OFXR_TEST_DJULES_ROUTE
constexpr bool kTestDjules=true;
#else
constexpr bool kTestDjules=false;
#endif
std::mutex test_mutex;
std::vector<std::pair<unsigned, bool>> presented;
std::atomic<unsigned> waits{}, begins{}, ends{};
std::atomic<unsigned> reference_calls{}, action_calls{}, locate_calls{};
std::atomic<bool> fail_runtime{}, blocked_end{}, entered_end{};
std::atomic<XrResult> refuse_next_end{XR_SUCCESS};
std::condition_variable test_condition;
XrTime next_time = 1000000000;
std::unordered_map<XrSwapchain, ID3D11Texture2D*> gpu_outputs;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> readback_context;
Microsoft::WRL::ComPtr<ID3D11Texture2D> readback;
Microsoft::WRL::ComPtr<ID3D11Texture2D> auxiliary_readback;
bool gpu_pixels_correct = true;
bool terminal_optical_test = false;
unsigned terminal_real_reads{}, terminal_synthetic_reads{};
std::shared_ptr<xrfg::D3D11D3D12SwapchainInterop> inline_publication_bridge;
std::uint64_t inline_publication_before{};
std::atomic<bool> inline_early_release{};
bool three_layer_test{};
Microsoft::WRL::ComPtr<ID3D11Device> test_device;
std::unordered_map<XrSwapchain, Microsoft::WRL::ComPtr<ID3D11Texture2D>> auxiliary_outputs;
std::atomic<unsigned> auxiliary_creates{}, auxiliary_destroys{};
bool auxiliary_pixels_correct = true;

std::uint32_t terminal_optical_pixel(unsigned frame, unsigned eye, unsigned x, unsigned y) {
    const unsigned shifted_x = (x + 256 - frame * 4) % 256;
    const unsigned blue = 32u + ((((shifted_x / 8) ^ (y / 8)) & 1u) ? 96u : 0u);
    return 0xff000000u | (blue << 16) | ((eye ? 160u : 80u) << 8) | (frame * 24u);
}

XrResult XRAPI_CALL fake_wait(XrSession, const XrFrameWaitInfo*, XrFrameState* out) {
    ++waits;
    next_time += 2000000;
    out->predictedDisplayTime = next_time;
    out->predictedDisplayPeriod = 2000000;
    out->shouldRender = XR_TRUE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_begin(XrSession, const XrFrameBeginInfo*) { ++begins; return XR_SUCCESS; }
XrResult XRAPI_CALL fake_end(XrSession, const XrFrameEndInfo* info) {
    ++ends;
    std::unique_lock lock(test_mutex);
    entered_end = true;
    test_condition.notify_all();
    test_condition.wait(lock, [] { return !blocked_end.load(); });
    if (fail_runtime) return XR_ERROR_RUNTIME_FAILURE;
    // The legacy presenter legally closes an already begun frame with an
    // empty EndFrame during stop. It has no projection to inspect/refuse.
    if (info->layerCount == 0) return XR_SUCCESS;
    const auto refusal = refuse_next_end.exchange(XR_SUCCESS);
    if (XR_FAILED(refusal)) return refusal;
    const auto* layer = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
    const unsigned packed = gpu_outputs.empty()
        ? static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(layer->space))
        : static_cast<unsigned>(layer->views[0].pose.position.y * 2);
    bool synthetic = (packed & 1) != 0;
    if (!gpu_outputs.empty() && layer->viewCount) {
        const auto handle = layer->views[0].subImage.swapchain;
        synthetic = reinterpret_cast<std::uintptr_t>(handle) >= 0x910;
        if (!synthetic || terminal_optical_test) {
            for (unsigned eye = 0; eye != 2; ++eye) {
                const auto& sub = layer->views[eye].subImage;
                readback_context->CopyResource(readback.Get(), gpu_outputs.at(sub.swapchain));
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (FAILED(readback_context->Map(readback.Get(), sub.imageArrayIndex, D3D11_MAP_READ, 0, &mapped))) {
                    gpu_pixels_correct = false;
                    continue;
                }
                const auto* pixel = static_cast<const unsigned char*>(mapped.pData);
                if (terminal_optical_test) {
                    const auto sequence = packed / 2;
                    if (!synthetic) {
                        for (unsigned y = 0; y < 128; ++y)
                            for (unsigned x = 0; x < 256; ++x) {
                                std::uint32_t actual{};
                                std::memcpy(&actual, pixel + y * mapped.RowPitch + x * 4, sizeof(actual));
                                gpu_pixels_correct &= actual == terminal_optical_pixel(sequence, eye, x, y);
                            }
                        ++terminal_real_reads;
                    } else {
                        // R is spatially constant but changes by 24 each frame;
                        // B is a checker translated four pixels per production.
                        // A copied endpoint cannot satisfy this midpoint check.
                        unsigned interior{}, computed{}; std::uint64_t red_sum{};
                        gpu_pixels_correct &= sequence > 1;
                        for (unsigned y = 16; y < 112; ++y)
                            for (unsigned x = 16; x < 240; ++x) {
                                const auto* p = pixel + y * mapped.RowPitch + x * 4;
                                ++interior; red_sum += p[0];
                                computed += p[0] > (sequence - 1) * 24 && p[0] < sequence * 24;
                                gpu_pixels_correct &= std::abs(static_cast<int>(p[1]) - (eye ? 160 : 80)) <= 1 &&
                                    p[3] == 255;
                            }
                        const double midpoint_red = sequence * 24.0 - 12.0;
                        gpu_pixels_correct &= computed > interior * 3 / 4 &&
                            std::abs(static_cast<double>(red_sum) / interior - midpoint_red) <= 2.0;
                        ++terminal_synthetic_reads;
                    }
                } else gpu_pixels_correct &= pixel[0] == packed / 2 && pixel[1] == (eye ? 160 : 80);
                readback_context->Unmap(readback.Get(), sub.imageArrayIndex);
            }
        }
    }
    if (three_layer_test && !info->layers[1]->next) {
        auxiliary_pixels_correct &= info->layerCount == 3;
        for (unsigned i = 1; i < info->layerCount; ++i) {
            const auto* quad = reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[i]);
            auxiliary_pixels_correct &= quad->type == XR_TYPE_COMPOSITION_LAYER_QUAD &&
                quad->eyeVisibility == (i == 1 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT) &&
                quad->subImage.imageArrayIndex == i-1 &&
                quad->subImage.imageRect.offset.x == 3 && quad->subImage.imageRect.extent.width == 240 &&
                quad->layerFlags == XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT &&
                quad->pose.position.x == static_cast<float>(i);
            const auto found = auxiliary_outputs.find(quad->subImage.swapchain);
            if (found == auxiliary_outputs.end()) { auxiliary_pixels_correct = false; continue; }
            readback_context->CopyResource(auxiliary_readback.Get(), found->second.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(readback_context->Map(auxiliary_readback.Get(), i-1, D3D11_MAP_READ, 0, &mapped))) {
                auxiliary_pixels_correct = false; continue;
            }
            const auto* pixel = static_cast<const unsigned char*>(mapped.pData);
            auxiliary_pixels_correct &= pixel[0] == (i == 1 ? packed/2 : 55) && pixel[1] == 60*i;
            readback_context->Unmap(auxiliary_readback.Get(), i-1);
        }
    }
    presented.emplace_back(packed / 2, synthetic);
    return XR_SUCCESS;
}

void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }

std::shared_ptr<ThreeSlotFrame> frame(unsigned sequence, bool synthetic = false) {
    auto entry = std::make_shared<ThreeSlotFrame>();
    entry->source_sequence = sequence;
    entry->previous_sequence = synthetic ? sequence - 1 : 0;
    auto owned = std::make_shared<GeneratedFrameEndInfo>();
    owned->synthetic = synthetic;
    owned->projections.resize(1);
    auto& p = owned->projections[0];
    p.layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    p.layer.space = reinterpret_cast<XrSpace>(std::uintptr_t(sequence * 2 + synthetic));
    owned->layer_pointers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&p.layer));
    owned->info.layers = owned->layer_pointers.data();
    owned->info.layerCount = 1;
    owned->info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    entry->frame = owned;
    return entry;
}

std::shared_ptr<SessionState> session() {
    auto dispatch = std::make_shared<Dispatch>();
    dispatch->wait_frame = fake_wait; dispatch->begin_frame = fake_begin; dispatch->end_frame = fake_end;
    auto state = std::make_shared<SessionState>(dispatch);
    state->handle = reinterpret_cast<XrSession>(0x207);
    state->graphics_binding = SessionGraphicsBinding::d3d11;
    state->optical_flow_backend = xrfg::D3D12OpticalFlowBackend::nvidia;
    state->nvidia_new_route = true; state->djules_route=kTestDjules;
    state->presenter_active = true;
    state->three_slot_director = true;
    state->presenter_frame_state_valid = true;
    state->presenter_frame_state.predictedDisplayPeriod = 2000000;
    state->presenter_frame_state.predictedDisplayTime = next_time;
    state->presenter_frame_state.shouldRender = XR_TRUE;
    return state;
}

// The real content EndFrame entry resolves the exact session by handle. Pure
// policy fixtures need no registration, but a live test consumer does.
struct RegisteredFixtureSession {
    std::shared_ptr<SessionState> current,previous;
    explicit RegisteredFixtureSession(std::shared_ptr<SessionState> value):current(std::move(value)) {
        std::scoped_lock lock(g_state_mutex);
        if(const auto found=g_sessions.find(current->handle);found!=g_sessions.end())previous=found->second;
        g_sessions[current->handle]=current;
    }
    ~RegisteredFixtureSession() {
        std::scoped_lock lock(g_state_mutex);
        const auto found=g_sessions.find(current->handle);
        if(found!=g_sessions.end() && found->second==current) {
            if(previous)found->second=std::move(previous);else g_sessions.erase(found);
        }
    }
};

void pipelined_route_handoff_test(bool steamvr = false, bool budget_demotion = false) {
    auto state = session();
    state->presenter_active = false;
    state->graphics_binding = SessionGraphicsBinding::d3d12;
    state->dispatch->steamvr_runtime = steamvr;
    state->pipelined_presenter_mode = true;
    state->last_inline_frame_state_valid = true;
    state->last_inline_frame_state = state->presenter_frame_state;
    std::mutex physical_mutex;
    unsigned physical_waits{}, physical_begins{}, physical_ends{}, order_errors{};
    bool physical_waited{}, physical_begun{};
    // Dispatch has C function pointers, so keep this fixture's strict runtime
    // journal separate from the deliberately permissive other director tests.
    struct Journal {
        std::mutex* mutex;
        unsigned *waits, *begins, *ends, *errors;
        bool *waited, *begun;
        XrTime time{1'000'000'000};
        SessionState* state{};
        bool latch_budget{};
    } journal{&physical_mutex, &physical_waits, &physical_begins, &physical_ends,
              &order_errors, &physical_waited, &physical_begun};
    static Journal* runtime;
    runtime = &journal;
    journal.state=state.get();
    state->dispatch->wait_frame = +[](XrSession, const XrFrameWaitInfo*, XrFrameState* out) -> XrResult {
        std::scoped_lock lock(*runtime->mutex);
        if (*runtime->waited || *runtime->begun) {
            ++*runtime->errors; return XR_ERROR_CALL_ORDER_INVALID;
        }
        ++*runtime->waits; *runtime->waited = true;
        runtime->time += 2'000'000;
        out->predictedDisplayTime = runtime->time;
        out->predictedDisplayPeriod = 2'000'000;
        out->shouldRender = XR_TRUE;
        return XR_SUCCESS;
    };
    state->dispatch->begin_frame = +[](XrSession, const XrFrameBeginInfo*) -> XrResult {
        std::scoped_lock lock(*runtime->mutex);
        if (!*runtime->waited || *runtime->begun) {
            ++*runtime->errors; return XR_ERROR_CALL_ORDER_INVALID;
        }
        ++*runtime->begins; *runtime->waited = false; *runtime->begun = true;
        return XR_SUCCESS;
    };
    state->dispatch->end_frame = +[](XrSession, const XrFrameEndInfo*) -> XrResult {
        std::scoped_lock lock(*runtime->mutex);
        if (!*runtime->begun) {
            ++*runtime->errors; return XR_ERROR_CALL_ORDER_INVALID;
        }
        ++*runtime->ends; *runtime->begun = false;
        if(runtime->latch_budget) {
            runtime->state->luke_aux_budget_inline=true;
            runtime->latch_budget=false;
        }
        return XR_SUCCESS;
    };
    const auto original = xrfg::embedded::snapshot().desired;
    auto choice = original;
    choice.enabled = true; choice.backend = 0; choice.frame_generation = 1;
    check(xrfg_fixture::request_control(choice), "handoff initial FSR setting");
    apply_embedded_control(state);
    { std::scoped_lock lock(g_state_mutex); g_sessions[state->handle] = state; }
    check(start_continuous_presenter(state, nullptr, true), "handoff initial owner");
    try {
        XrFrameState prediction{XR_TYPE_FRAME_STATE};
        check(XR_SUCCEEDED(layer_wait_frame_impl(state->handle, nullptr, &prediction)), "initial virtual Wait");
        check(XR_SUCCEEDED(layer_begin_frame_impl(state->handle, nullptr)), "initial virtual Begin");
        // Every change occurs after the *next* virtual Wait has already returned
        // on another thread. A stop-only switch passes End yet strands that Begin.
        for (const auto target : std::array<std::pair<int,int>, 12>{{
                {1,0}, {2,0}, {3,0}, {2,0}, {3,0}, {1,0},
                {0,0}, {-1,0}, {1,0}, {0,1}, {1,0}, {0,1}}}) {
            XrFrameState next{XR_TYPE_FRAME_STATE};
            XrResult wait_result = XR_ERROR_RUNTIME_FAILURE;
            std::thread application_wait([&] {
                wait_result = layer_wait_frame_impl(state->handle, nullptr, &next);
            });
            application_wait.join();
            check(XR_SUCCEEDED(wait_result), "overlapping virtual Wait");
            choice.enabled = target.first >= 0;
            choice.backend = (std::max)(target.first, 0);
            choice.frame_generation = target.second;
            check(xrfg_fixture::request_control(choice), "handoff menu setting");
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            end.displayTime = prediction.predictedDisplayTime;
            check(XR_SUCCEEDED(layer_end_frame_impl(state->handle, &end)), "old owner boundary End");
            check(XR_SUCCEEDED(layer_begin_frame_impl(state->handle, nullptr)), "Begin after changed owner");
            check(continuous_presenter_active(state), "virtual tickets lost their physical owner");
            check(state->fsr_two_slot_presenter == fsr_two_slot_session(state), "FSR owner mismatch");
            check(state->queued_nvidia_presenter == queued_nvidia_session(state), "NVIDIA owner mismatch");
            check(state->djules_route == (target.first == 3), "DJ route was not physically selected");
            check(state->nvidia_new_route == (target.first >= 2), "advanced route selection mismatch");
            prediction = next;
        }
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.displayTime = prediction.predictedDisplayTime;
        journal.latch_budget=budget_demotion;
        check(XR_SUCCEEDED(layer_end_frame_impl(state->handle, &end)), "last virtual End");
        if(budget_demotion) {
            check(!continuous_presenter_active(state),"mid-End budget refusal kept virtual owner alive");
            for(unsigned i=0;i<4;++i) {
                XrFrameState direct{XR_TYPE_FRAME_STATE};
                check(XR_SUCCEEDED(layer_wait_frame_impl(state->handle,nullptr,&direct)),"budget direct Wait");
                check(XR_SUCCEEDED(layer_begin_frame_impl(state->handle,nullptr)),"budget direct Begin");
                end.displayTime=direct.predictedDisplayTime;
                check(XR_SUCCEEDED(layer_end_frame_impl(state->handle,&end)),"budget direct End");
                check(!continuous_presenter_active(state),"budget fallback promoted again");
            }
        }
        stop_continuous_presenter(state);
        check(order_errors == 0 && physical_waits > 12 &&
              physical_waits == physical_begins && physical_begins == physical_ends &&
              !physical_waited && !physical_begun, "physical Wait/Begin/End journal imbalance");
    } catch (...) {
        stop_continuous_presenter(state);
        static_cast<void>(xrfg_fixture::request_control(original));
        { std::scoped_lock lock(g_state_mutex); g_sessions.erase(state->handle); }
        throw;
    }
    check(xrfg_fixture::request_control(original), "restore handoff fixture configuration");
    { std::scoped_lock lock(g_state_mutex); g_sessions.erase(state->handle); }
    std::cout << "Twelve live route changes preserve outstanding virtual tickets and balanced physical cycles\n";
}

void own_time_pairing_recovery_test() {
    auto live = session();
    live->pending_frames={{100,10,1},{110,10,2}};
    check(!latest_pending_application_frame(live,999,false,false), "ambiguous custom time was paired");
    consume_application_frame(live,999,false,false);
    check(live->pending_frames.empty(), "unmatchable wait survived its End");
    live->pending_frames.push_back({120,10,3});
    const auto recovered=latest_pending_application_frame(live,1001,false,false);
    check(recovered && recovered->serial==3, "custom-time game did not recover on next wait");
    consume_application_frame(live,1001,false,false);
    check(live->pending_frames.empty(), "sole custom-time admission did not retire");
    live->pending_frames={{130,10,4},{140,10,5}};
    live->application_wait_pending_begin=true;
    consume_application_frame(live,1002,false,false);
    check(live->pending_frames.size()==1 && live->pending_frames.front().serial==5,
        "recovery erased already returned next virtual ticket");
    live->application_wait_pending_begin=false;
    consume_application_frame(live,140,false,false);
    check(live->pending_frames.empty(), "following ticket was not consumable");
    std::cout << "Custom-time orphan recovery preserves next outstanding virtual Wait\n";
}

void unified_route_policy_test() {
    {
        auto reference = std::make_shared<SessionState>(std::make_shared<Dispatch>());
        reference->graphics_binding = SessionGraphicsBinding::d3d12;
        reference->optical_flow_backend = xrfg::D3D12OpticalFlowBackend::nvidia;
        reference->frame_generation_mode = xrfg::D3D12FrameGenerationMode::ofxr;
        reference->nvidia_new_route = false;
        reference->dispatch->steamvr_runtime = true;
        reference->vdxr_legacy = false;
        check(steamvr_reference_capture_overlap(*reference), "native SteamVR Reference transport not selected");
        reference->dispatch->steamvr_runtime = false;
        reference->dispatch->virtual_desktop_runtime = true;
        reference->vdxr_legacy = true;
        check(!steamvr_reference_capture_overlap(*reference), "Reference overlap leaked into Virtual Desktop");
        reference->dispatch->steamvr_runtime = true;
        reference->vdxr_legacy = false;
        reference->nvidia_new_route = true;
        check(!steamvr_reference_capture_overlap(*reference), "Reference overlap changed New/Djules");
        reference->nvidia_new_route = false;
        reference->luke_afw_enabled = true;
        check(!steamvr_reference_capture_overlap(*reference), "Reference overlap changed Luke AFW");
        reference->luke_afw_enabled = false;
        reference->frame_generation_mode = xrfg::D3D12FrameGenerationMode::fsr3;
        check(!steamvr_reference_capture_overlap(*reference), "Reference overlap changed FSR");
        reference->frame_generation_mode = xrfg::D3D12FrameGenerationMode::ofxr;
        reference->lower_vram = 1;
        check(steamvr_reference_capture_overlap(*reference), "allocation flag changed Reference capture overlap");
    }
    const auto ini = current_layer_directory() / L"ofxr_bridge.ini";
    wchar_t old_deep[64]{}, old_triple[64]{};
    GetPrivateProfileStringW(L"ofxr", L"deep_pipeline", L"absent", old_deep, 64, ini.c_str());
    GetPrivateProfileStringW(L"ofxr", L"triple_frame_gen", L"absent", old_triple, 64, ini.c_str());
    const auto original = xrfg::embedded::snapshot().desired;
    auto live = std::make_shared<SessionState>(std::make_shared<Dispatch>());
    live->graphics_binding = SessionGraphicsBinding::d3d12;
    live->dispatch->steamvr_runtime = true;
    live->vdxr_legacy = false;
    live->dispatch->inline_unless_pipelined = true;
    live->three_slot_director = true;
    live->djules_pipeline_switchable = true;
    const auto restore = [&] {
        static_cast<void>(xrfg_fixture::request_control(original));
        WritePrivateProfileStringW(L"ofxr", L"deep_pipeline", std::wcscmp(old_deep,L"absent") ? old_deep : nullptr, ini.c_str());
        WritePrivateProfileStringW(L"ofxr", L"triple_frame_gen", std::wcscmp(old_triple,L"absent") ? old_triple : nullptr, ini.c_str());
    };
    try {
        check(xrfg::embedded::set_djules_triple_frame_gen(true), "request 3X from embedded menu producer");
        check(xrfg::embedded::set_djules_prefer_fps(true), "save deep policy");
        for (const int route : {3,2,1,2,3}) {
            auto desired = original;
            desired.enabled = true; desired.backend = route; desired.frame_generation = 0;
            check(xrfg_fixture::request_control(desired), "request actual runtime route");
            apply_embedded_control(live);
            live->djules_pipeline_poll = {};
            check(XR_SUCCEEDED(poll_djules_pipeline(live)), "first policy observation");
            live->djules_pipeline_poll = {};
            check(XR_SUCCEEDED(poll_djules_pipeline(live)), "second policy observation");
            check(live->djules_route == (route==3), "route did not select DJ runtime policy");
            check(djules_triple_active(*live) == (route==3), "3X leaked outside DJ");
            check(djules_inline_runtime(*live) == (route==3), "inline pacing leaked outside DJ");
            check(live->three_slots.capacity() == (route==3 ? 4U : 3U), "wrong physical output shape");
            check(live->nvidia_options.pose_compensated_input == (route>=2), "non-pacing input fix missing or on reference");
            live->presenter_display_period = 2'000'000;
            check(adopt_djules_display_period(*live,std::chrono::nanoseconds(8'333'333)) == (route==3), "DJ clock leaked outside route");
        }
        auto desired = original; desired.enabled=true; desired.backend=3; desired.frame_generation=1;
        check(xrfg_fixture::request_control(desired), "request FSR with saved DJ flags");
        apply_embedded_control(live);
        check(!djules_triple_active(*live) && !djules_inline_runtime(*live), "FSR inherited DJ policy");
        desired.frame_generation=0;
        check(xrfg_fixture::request_control(desired), "return to DJ");
        apply_embedded_control(live);
        live->luke_afw_enabled=true;
        check(!djules_triple_active(*live) && !djules_deep_active(*live), "Luke AFW inherited DJ policy");
    } catch (...) { restore(); throw; }
    restore();
    std::cout << "One runtime selects all three routes; saved 3X/deep/clock/inline policy stays DJ-only\n";
}

void djules_pipeline_preference_test() {
    const auto ini = current_layer_directory() / L"ofxr_bridge.ini";
    wchar_t old_deep[64]{}, old_triple[64]{};
    GetPrivateProfileStringW(L"ofxr", L"deep_pipeline", L"absent", old_deep, 64, ini.c_str());
    GetPrivateProfileStringW(L"ofxr", L"triple_frame_gen", L"absent", old_triple, 64, ini.c_str());
    const auto id = xrfg::embedded::attach();
    const auto restore = [&] {
        xrfg::embedded::detach(id);
        WritePrivateProfileStringW(L"ofxr", L"deep_pipeline",
            std::wcscmp(old_deep,L"absent") ? old_deep : nullptr, ini.c_str());
        WritePrivateProfileStringW(L"ofxr", L"triple_frame_gen",
            std::wcscmp(old_triple,L"absent") ? old_triple : nullptr, ini.c_str());
    };
    try {
        check(WritePrivateProfileStringW(L"ofxr", L"triple_frame_gen", L"0", ini.c_str()), "preference fixture 2X");
        check(xrfg::embedded::set_djules_prefer_fps(true), "save initial deep preference");
        xrfg::embedded::register_djules_pipeline(id, true);
        const auto before = xrfg::embedded::snapshot();
        check(xrfg::embedded::set_djules_prefer_fps(false), "save shallow preference");
        auto pipeline = xrfg::embedded::djules_pipeline_snapshot();
        check(!pipeline.prefer_fps && pipeline.pending, "preference must await the frame-boundary receipt");
        // Ordinary route receipts must not erase the pending policy indication.
        xrfg::embedded::applied(id, before.revision, true, 0);
        check(xrfg::embedded::djules_pipeline_snapshot().pending, "route apply erased policy receipt");
        xrfg::embedded::register_djules_pipeline(id, false);
        check(!xrfg::embedded::djules_pipeline_snapshot().pending, "applied live receipt must clear pending");
        check(xrfg::embedded::set_djules_prefer_fps(true), "restore deep preference");
        check(xrfg::embedded::djules_pipeline_snapshot().pending, "reverse edit must await application");
        xrfg::embedded::register_djules_pipeline(id, true);
        check(!xrfg::embedded::djules_pipeline_snapshot().pending, "matching preference is already applied");
        check(WritePrivateProfileStringW(L"ofxr", L"triple_frame_gen", L"1", ini.c_str()), "preference fixture 3X");
        check(!xrfg::embedded::set_djules_prefer_fps(false), "3X must prevent disabling its deep setup");
        pipeline = xrfg::embedded::djules_pipeline_snapshot();
        check(pipeline.prefer_fps && pipeline.triple_frame_gen, "3X effective preference");
        const auto after = xrfg::embedded::snapshot();
        check(after.revision == before.revision && after.desired == before.desired,
              "preference click reconfigured the active renderer");
#ifdef OFXR_TEST_DJULES_ROUTE
        auto live = std::make_shared<SessionState>(std::make_shared<Dispatch>());
        live->graphics_binding = SessionGraphicsBinding::d3d12;
        live->nvidia_new_route = true; live->djules_route=kTestDjules;
        live->optical_flow_backend = xrfg::D3D12OpticalFlowBackend::nvidia;
        live->frame_generation_mode = xrfg::D3D12FrameGenerationMode::ofxr;
        live->djules_pipeline_switchable = true;
        check(WritePrivateProfileStringW(L"ofxr", L"triple_frame_gen", L"0", ini.c_str()), "live 2X");
        check(xrfg::embedded::set_djules_prefer_fps(true), "request live deep");
        const auto poll = [&] {
            live->djules_pipeline_poll = {};
            check(XR_SUCCEEDED(poll_djules_pipeline(live)), "live policy poll");
        };
        poll();
        check(!live->djules_deep_pipeline, "single observation applied partially written policy");
        poll();
        check(live->djules_deep_pipeline && live->djules_configuration_epoch == 1, "INI policy did not apply live");
        live->nvidia_new_route = false;
        check(xrfg::embedded::set_djules_prefer_fps(false), "request shallow outside DJ");
        poll(); poll();
        check(live->djules_deep_pipeline && live->djules_configuration_epoch == 1, "reference applied DJ policy");
        live->nvidia_new_route = true; live->djules_route=kTestDjules;
        poll(); poll();
        check(!live->djules_deep_pipeline && live->djules_configuration_epoch == 2, "return to DJ did not apply preference");
        check(WritePrivateProfileStringW(L"ofxr", L"triple_frame_gen", L"1", ini.c_str()), "request live 3X");
        poll(); poll();
        check(live->djules_triple_requested && live->djules_deep_pipeline, "3X did not force effective preference");
        check(WritePrivateProfileStringW(L"ofxr", L"triple_frame_gen", L"0", ini.c_str()), "return live 2X");
        poll(); poll();
        check(!live->djules_triple_requested && !live->djules_deep_pipeline, "2X did not restore saved shallow preference");
#endif
    } catch (...) { restore(); throw; }
    restore();
    std::cout << "DJ preference persists independently, preserves pending receipts and honors 3X\n";
}

void publication_receipt_independence_test(const std::shared_ptr<SessionState>& state,
    const std::shared_ptr<xrfg::D3D11D3D12SwapchainInterop>& bridge) {
    Microsoft::WRL::ComPtr<ID3D12Fence> gate;
    check(SUCCEEDED(state->d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&gate))), "publication test fence");
    check(SUCCEEDED(state->d3d12_queue->Wait(gate.Get(), 1)), "hold worker publication");
    xrfg::D3D11PublicationReceipt worker_receipt, producer_receipt;
    const auto queued = bridge->publish(0, std::nullopt, &worker_receipt);
    if (FAILED(queued)) {
        static_cast<void>(gate->Signal(2));
        check(false, "queue worker receipt");
    }
    // A subsequent producer transaction is allowed to queue while A waits.
    // Keep its completion B blocked even after the worker's A becomes ready.
    const auto hold_next = state->d3d12_queue->Wait(gate.Get(), 2);
    if (FAILED(hold_next)) {
        static_cast<void>(gate->Signal(2));
        check(false, "hold later producer publication");
    }
    std::atomic<bool> worker_finished{false}, producer_finished{false};
    HRESULT worker_result = E_PENDING, producer_result = E_PENDING;
    std::thread waiter([&] {
        worker_result = xrfg::wait_for_d3d11_publication(worker_receipt);
        worker_finished.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    std::thread producer([&] {
        producer_result = bridge->prepare_capture(0);
        if (SUCCEEDED(producer_result)) producer_result = bridge->finish_capture();
        if (SUCCEEDED(producer_result))
            producer_result = bridge->publish(kAfwScratchSlot, std::nullopt, &producer_receipt);
        producer_finished.store(true);
    });
    for (unsigned i = 0; i < 200 && !producer_finished.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool producer_not_blocked = producer_finished.load() && !worker_finished.load();
    static_cast<void>(gate->Signal(1));
    for (unsigned i = 0; i < 200 && !worker_finished.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool exact_target = worker_finished.load() && producer_finished.load() &&
        producer_receipt.copy_complete_value > worker_receipt.copy_complete_value &&
        producer_receipt.fence->GetCompletedValue() < producer_receipt.copy_complete_value;
    static_cast<void>(gate->Signal(2));
    waiter.join();
    producer.join();
    check(producer_not_blocked && exact_target && SUCCEEDED(worker_result) && SUCCEEDED(producer_result),
        "worker publication wait blocked producer or followed mutable next-frame completion");
    check(SUCCEEDED(xrfg::wait_for_d3d11_publication(producer_receipt)), "finish later producer receipt");
    check(SUCCEEDED(bridge->wait_for_idle()), "publication regression cleanup");
    std::cout << "Publication receipt: producer queues while worker waits; worker completes before later producer GPU work\n";
}

void completed_publication_independence_test(const std::shared_ptr<SessionState>& state,
    const std::shared_ptr<FrameGenerationSwapchainState>& generation,
    const std::shared_ptr<xrfg::D3D12SwapchainHistory>& history, ID3D11Texture2D* source) {
    const auto bridge=generation->d3d11_interop;
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    constexpr std::uint32_t pixel=0xff6c3b19;
    std::vector<std::uint32_t> pixels(desc.Width*desc.Height,pixel);
    for(UINT eye=0;eye<desc.ArraySize;++eye)
        state->d3d11_context->UpdateSubresource(source,eye,nullptr,pixels.data(),desc.Width*4,0);
    check(SUCCEEDED(bridge->prepare_capture(0)),"completed output capture");
    xrfg::D3D12HistoryCaptureTicket capture;
    check(SUCCEEDED(history->capture(0,&capture)),"completed output history");
    check(SUCCEEDED(history->commit(capture)),"completed output history commit");
    check(SUCCEEDED(bridge->finish_capture()),"completed output source retirement");
    xrfg::D3D12ReprojectionView view{};
    view.fov={-.7F,.7F,.7F,-.7F};view.image_rect={0,0,desc.Width,desc.Height};
    std::vector<xrfg::D3D12ReprojectionView> views(desc.ArraySize,view);
    for(unsigned eye=0;eye<views.size();++eye)views[eye].array_slice=eye;
    xrfg::D3D12FrameSynthesisTicket ticket;
    check(SUCCEEDED(bridge->prepare_synthesis(generation->isolated_luke_synthesis)),"completed output destination access");
    check(SUCCEEDED(generation->synthesizer->submit_copy(capture,views,0,&ticket)),"completed output copy");
    const auto fence=generation->synthesizer->submission_fence(ticket);
    check(fence && SUCCEEDED(wait_composition_fence(fence.Get(),ticket.fence_value)),"exact output completed");
    Microsoft::WRL::ComPtr<ID3D12Fence> gate;
    check(SUCCEEDED(state->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate))),"later native gate");
    xrfg::D3D11PublicationReceipt invalid;
    const auto before=bridge->publication_snapshot().copy_complete_value;
    check(bridge->publish_completed(0,std::nullopt,gate.Get(),1,&invalid)==HRESULT_FROM_WIN32(ERROR_BUSY) &&
        !invalid.fence && bridge->publication_snapshot().copy_complete_value==before,"unfinished producer cannot publish");
    check(SUCCEEDED(state->d3d12_queue->Wait(gate.Get(),1)),"park later native work");
    // Legacy publication appends its ready Signal behind this parked work,
    // despite the worker pixels already being complete. Verify that condition
    // first, then release it before testing the production completed route.
    xrfg::D3D11PublicationReceipt legacy;
    check(SUCCEEDED(bridge->publish(0,std::nullopt,&legacy)),"queue legacy dependency");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const bool legacy_blocked=legacy.fence->GetCompletedValue()<legacy.copy_complete_value;
    static_cast<void>(gate->Signal(1));
    check(SUCCEEDED(xrfg::wait_for_d3d11_publication(legacy)),"legacy cleanup");
    check(SUCCEEDED(state->d3d12_queue->Wait(gate.Get(),2)),"park next native work");
    PreparedProjectionFrame prepared;prepared.kind=PreparedGenerationKind::prime;
    PreparedGeneration output;output.kind=PreparedGenerationKind::prime;
    output.worker_generation=generation;output.synthesizer=generation->synthesizer;
    output.worker_fence=fence;output.worker_fence_value=ticket.fence_value;
    output.current_destination=0;output.current_slot=0;
    prepared.resources.push_back({reinterpret_cast<XrSwapchain>(0x488),output});
    std::atomic<bool> finished{false};bool successful=false;
    std::thread worker([&]{successful=finish_worker_outputs(state,prepared,488);finished=true;});
    for(unsigned i=0;i<250 && !finished.load();++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool independent=finished.load();
    static_cast<void>(gate->Signal(2));worker.join();
    check(legacy_blocked && independent && successful,"completed worker publication followed later native queue work");
    auto image=bridge->published_current_image(0);
    auto staging_desc=desc;staging_desc.Usage=D3D11_USAGE_STAGING;
    staging_desc.BindFlags=staging_desc.MiscFlags=0;staging_desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    check(SUCCEEDED(state->d3d11_device->CreateTexture2D(&staging_desc,nullptr,&staging)),"completed publication readback");
    state->d3d11_context->CopyResource(staging.Get(),image.Get());
    for(UINT eye=0;eye<desc.ArraySize;++eye) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(SUCCEEDED(state->d3d11_context->Map(staging.Get(),eye,D3D11_MAP_READ,0,&mapped)),"completed publication pixels");
        bool identical=true;
        for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x)
            identical &= reinterpret_cast<const std::uint32_t*>(static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch)[x]==pixel;
        state->d3d11_context->Unmap(staging.Get(),eye);
        check(identical,"completed publication changed stereo pixels");
    }
    // A later native capture retirement may still be queued when already
    // completed worker pixels are published. It must not rewind the D11
    // publication receipt: that is a different producer timeline.
    check(SUCCEEDED(state->d3d12_queue->Wait(gate.Get(),3)),"park native retirement");
    check(SUCCEEDED(bridge->finish_capture()),"queue later native capture retirement");
    Microsoft::WRL::ComPtr<ID3D12Fence> native_retired;
    check(SUCCEEDED(state->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&native_retired))),"native retirement receipt");
    check(SUCCEEDED(state->d3d12_queue->Signal(native_retired.Get(),1)),"exact native retirement marker");
    xrfg::D3D11PublicationReceipt publication;
    check(SUCCEEDED(bridge->publish_completed(0,std::nullopt,fence.Get(),ticket.fence_value,&publication)),"publish before later native retirement");
    check(SUCCEEDED(xrfg::wait_for_d3d11_publication(publication)),"D11 copy completion before native retirement");
    static_cast<void>(gate->Signal(3));
    check(SUCCEEDED(wait_composition_fence(native_retired.Get(),1,1000)),"native retirement completed");
    const bool monotonic=publication.fence->GetCompletedValue()>=publication.copy_complete_value;
    // Advance an ordinary source handshake before checking the result, so
    // the old single-timeline implementation can fail without stranding GPU
    // queues during fixture destruction.
    check(SUCCEEDED(bridge->prepare_capture(0)),"post-retirement capture");
    xrfg::D3D12HistoryCaptureTicket later_capture;
    check(SUCCEEDED(history->capture(0,&later_capture)) && SUCCEEDED(history->commit(later_capture)),"post-retirement history");
    check(SUCCEEDED(bridge->finish_capture()),"post-retirement capture completion");
    check(SUCCEEDED(bridge->wait_for_idle()),"completed publication cleanup");
    // Reproduce the live single-eye backpressure with actual unfinished GPU
    // work; the worker must retry without holding either producer lock.
    check(SUCCEEDED((generation->isolated_luke_synthesis?state->luke_synthesis_queue:state->d3d12_queue)->Wait(gate.Get(),4)),"park busy worker destination");
    xrfg::D3D12FrameSynthesisTicket blocked_ticket,retried_ticket;
    check(SUCCEEDED(generation->synthesizer->submit_copy(later_capture,views,0,&blocked_ticket)),"queue busy worker owner");
    const auto legacy_result=submit_luke_worker_generation(state,generation,generation->synthesizer,false,[&] {
        return generation->synthesizer->submit_copy(later_capture,views,0,&retried_ticket);});
    check(legacy_result==HRESULT_FROM_WIN32(ERROR_BUSY),"legacy busy resource not reproduced");
    const bool saved_luke=state->luke_afw_enabled;state->luke_afw_enabled=true;
    std::mutex producer_call;std::atomic<unsigned> attempts{};std::atomic_bool retry_finished{};
    HRESULT retry_result=E_PENDING;
    std::thread retry([&] {
        std::unique_lock call(producer_call);
        retry_result=submit_luke_worker_generation(state,generation,generation->synthesizer,true,[&] {
            ++attempts;return generation->synthesizer->submit_copy(later_capture,views,0,&retried_ticket);
        },&call);retry_finished=true;
    });
    for(unsigned i=0;i<100&&!attempts.load();++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool gpu_free=state->gpu_mutex.try_lock();if(gpu_free)state->gpu_mutex.unlock();
    const bool call_free=producer_call.try_lock();if(call_free)producer_call.unlock();
    const bool waited=attempts.load()==1&&!retry_finished.load();
    static_cast<void>(gate->Signal(4));retry.join();state->luke_afw_enabled=saved_luke;
    check(gpu_free&&call_free&&waited&&SUCCEEDED(retry_result)&&attempts.load()==2,
        "worker busy killed stereo transaction or retained producer locks");
    check(SUCCEEDED(wait_composition_fence(generation->synthesizer->submission_fence(retried_ticket).Get(),retried_ticket.fence_value,1000)),"retried real copy completion");
    // A completed native production must remain usable while later game work
    // waits for CPU/XR progress. Exercise real NVIDIA OFA and D11 publication
    // on the production-selected queue, with the native queue still parked.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> execution, unchanged;
    check(SUCCEEDED(select_luke_execution_queue(false,state->d3d12_device.Get(),
        state->d3d12_queue.Get(),unchanged))&&unchanged==state->d3d12_queue,"non-Luke queue changed");
    check(SUCCEEDED(select_luke_execution_queue(true,state->d3d12_device.Get(),
        state->d3d12_queue.Get(),execution))&&execution!=state->d3d12_queue,"Luke execution queue not isolated");
    auto independent_history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    check(SUCCEEDED(independent_history->initialize(state->d3d12_device.Get(),
        state->d3d12_queue.Get(),bridge->source_images(),D3D12_RESOURCE_STATE_COMMON)),"isolated history");
    xrfg::D3D12HistoryCaptureTicket first_native,second_native;
    check(SUCCEEDED(bridge->prepare_capture(0)),"isolated native color ready");
    check(SUCCEEDED(independent_history->capture(0,&first_native))&&
        SUCCEEDED(independent_history->commit(first_native)),"first native production");
    check(SUCCEEDED(independent_history->capture(0,&second_native))&&
        SUCCEEDED(independent_history->commit(second_native))&&
        SUCCEEDED(bridge->finish_capture())&&SUCCEEDED(bridge->wait_for_idle()),"second native production");
    auto private_synth=std::make_shared<xrfg::D3D12FrameSynthesizer>();
    check(SUCCEEDED(private_synth->initialize(state->d3d12_device.Get(),execution.Get(),
        independent_history,bridge->current_destination_images(),bridge->synthetic_destination_images(),
        desc.Format,D3D12_RESOURCE_STATE_COMMON,xrfg::D3D12OpticalFlowBackend::nvidia,
        state->nvidia_options,false,xrfg::D3D12FrameGenerationMode::ofxr,true)),"isolated actual OFA provider");
    xrfg::D3D12FrameSynthesisTicket isolated_prime,isolated_pair;
    check(SUCCEEDED(private_synth->submit_prime(first_native,views,0,&isolated_prime))&&
        SUCCEEDED(wait_composition_fence(private_synth->submission_fence(isolated_prime).Get(),
            isolated_prime.fence_value,1000)),"isolated prime");
    Microsoft::WRL::ComPtr<ID3D12Fence> legacy_progress;
    check(SUCCEEDED(state->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&legacy_progress))),"legacy native progress fence");
    struct Unpark { ID3D12Fence* fence; ~Unpark(){static_cast<void>(fence->Signal(6));} } unpark{gate.Get()};
    check(SUCCEEDED(state->d3d12_queue->Wait(gate.Get(),5)),"park legacy scratch copy");
    xrfg::D3D12FrameSynthesisTicket legacy_copy;
    const auto legacy_synth=generation->isolated_luke_synthesis?generation->afw_source_synthesizer:generation->synthesizer;
    check(SUCCEEDED(legacy_synth->submit_copy(later_capture,views,0,&legacy_copy)),"legacy actual scratch copy");
    auto legacy_fence=legacy_synth->submission_fence(legacy_copy);
    const bool legacy_starved=FAILED(wait_composition_fence(legacy_fence.Get(),legacy_copy.fence_value,40));
    static_cast<void>(gate->Signal(5));
    check(SUCCEEDED(wait_composition_fence(legacy_fence.Get(),legacy_copy.fence_value,1000)),"legacy copy retirement");
    check(SUCCEEDED(state->d3d12_queue->Wait(gate.Get(),6))&&
        SUCCEEDED(state->d3d12_queue->Signal(legacy_progress.Get(),1)),"park future game rendering");
    check(SUCCEEDED(private_synth->submit_pair(second_native,views,views,0,0,&isolated_pair)),"isolated OFA pair");
    auto pair_fence=private_synth->submission_fence(isolated_pair);
    check(SUCCEEDED(wait_composition_fence(pair_fence.Get(),isolated_pair.fence_value,1000)),"isolated OFA completion");
    xrfg::D3D11PublicationReceipt isolated_publication;
    check(SUCCEEDED(bridge->publish_completed(0,0,pair_fence.Get(),isolated_pair.fence_value,
        &isolated_publication))&&SUCCEEDED(xrfg::wait_for_d3d11_publication(isolated_publication)),"isolated stereo publication");
    const bool game_still_parked=legacy_progress->GetCompletedValue()==0;
    static_cast<void>(gate->Signal(6));
    check(legacy_starved&&game_still_parked&&SUCCEEDED(wait_composition_fence(legacy_progress.Get(),1,1000)),
        "OFA inherited later native queue dependency");
    check(SUCCEEDED(private_synth->retire_previous())&&SUCCEEDED(private_synth->wait_for_idle())&&
        SUCCEEDED(independent_history->invalidate()),"isolated history retirement");
    auto isolated_image=bridge->published_current_image(0);
    state->d3d11_context->CopyResource(staging.Get(),isolated_image.Get());
    for(UINT eye=0;eye<desc.ArraySize;++eye) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(SUCCEEDED(state->d3d11_context->Map(staging.Get(),eye,D3D11_MAP_READ,0,&mapped)),"isolated B readback");
        bool identical=true;
        for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x)
            identical &= reinterpret_cast<const std::uint32_t*>(static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch)[x]==pixel;
        state->d3d11_context->Unmap(staging.Get(),eye);
        check(identical,"isolated current pixels changed");
    }
    std::cout<<"Actual NVIDIA OFA + D11 publication: legacy native queue starves, private queue completes while game stays parked PASS\n";
    check(SUCCEEDED(history->invalidate()),"busy worker history cleanup");
    std::cout<<"Actual GPU worker: legacy ERROR_BUSY, bounded retry succeeds with both producer locks released PASS\n";
    check(monotonic,"later D12 capture retirement rewound a completed D11 publication fence");
    std::cout<<"Publication timeline: later D12 retirement cannot rewind completed D11 copy receipt PASS\n";

    std::cout<<"Completed worker: legacy blocks behind later native work; exact-fence publication finishes with identical stereo pixels\n";
}

template<class Predicate>
void until(const std::shared_ptr<SessionState>& state, Predicate condition) {
    std::unique_lock lock(state->presenter_mutex);
    check(state->presenter_condition.wait_for(lock, std::chrono::seconds(3), condition), "timed out");
}

// Delay the real publication GPU queue. The production inline consumer must
// keep its XR image acquired until its own copy receipt completes.
void inline_afw_publication_gpu_test(const std::shared_ptr<SessionState>& state,
    const PostAfwFrameInput& input, unsigned pixel_serial) {
    const auto gen=input.generation;
    const auto active=gen->afw_temporal_synthesizer;
    const auto current_slot=gen->current_slot,synthetic_slot=gen->synthetic_slot;
    static std::shared_ptr<xrfg::D3D12FrameSynthesizer> probe;
    static std::shared_ptr<xrfg::D3D12SwapchainHistory> probe_history;
    if(pixel_serial==1) {
    probe_history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    check(SUCCEEDED(probe_history->initialize(state->d3d12_device.Get(),state->d3d12_queue.Get(),
        gen->d3d11_interop->current_destination_images(),D3D12_RESOURCE_STATE_COMMON)),"inline independent history");
    probe=std::make_shared<xrfg::D3D12FrameSynthesizer>();
    check(SUCCEEDED(probe->initialize(state->d3d12_device.Get(),
        gen->isolated_luke_synthesis?state->luke_synthesis_queue.Get():state->d3d12_queue.Get(),
        probe_history,gen->d3d11_interop->current_destination_images(),
        gen->d3d11_interop->synthetic_destination_images(),DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_STATE_COMMON,xrfg::D3D12OpticalFlowBackend::nvidia,{},true)), "inline probe synth");
    }
    auto probe_input=input;
    check(SUCCEEDED(probe_history->capture(input.capture.source_index,&probe_input.capture)),"inline independent capture");
    check(SUCCEEDED(probe_history->commit(probe_input.capture)),"inline independent commit");
    gen->afw_temporal_synthesizer=probe;
    Microsoft::WRL::ComPtr<ID3D12Fence> gate;
    check(SUCCEEDED(state->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate))),"inline publication fence");
    const auto before=gen->d3d11_interop->publication_snapshot().copy_complete_value;
    check(SUCCEEDED((gen->isolated_luke_synthesis?state->luke_synthesis_queue:state->d3d12_queue)->Wait(gate.Get(),1)),"delay inline output copy");
    std::atomic<bool> finished{false};
    const auto release=state->dispatch->release_swapchain_image;
    inline_publication_bridge=gen->d3d11_interop;inline_publication_before=before;inline_early_release=false;
    state->dispatch->release_swapchain_image=[](XrSwapchain,const XrSwapchainImageReleaseInfo*) -> XrResult {
        const auto receipt=inline_publication_bridge->publication_snapshot();
        if(receipt.copy_complete_value>inline_publication_before && receipt.completed_value<receipt.copy_complete_value)
            inline_early_release.store(true);
        return XR_SUCCESS;
    };
    PreparedGeneration prepared;
    std::thread consumer([&] {
        prepared=prepare_frame_generation(probe_input.application_swapchain,pixel_serial>1,probe_input.views,.5F,
            false,nullptr,false,&probe_input);
        finished.store(true);
    });
    const bool private_output=state->luke_afw_enabled && state->separate_synthesis>0;
    bool queued=false;
    for(unsigned i=0;i<500;++i) {
        if(gen->d3d11_interop->publication_snapshot().copy_complete_value>before){queued=true;break;}
        if(finished.load())break;
        if(private_output && i>=30)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    // A waits for exact synthesis completion before queuing the D11 copy;
    // the old path already queues that copy with a GPU-side producer wait.
    const bool blocked=(queued || private_output) && !finished.load() && !inline_early_release.load();
    const bool outside_gpu_lock=state->gpu_mutex.try_lock();
    if(outside_gpu_lock)state->gpu_mutex.unlock();
    static_cast<void>(gate->Signal(1));consumer.join();
    state->dispatch->release_swapchain_image=release;
    inline_publication_bridge.reset();
    // Keep the prime's temporal anchor for the subsequent pair; wait_for_idle
    // deliberately retires that anchor and is only appropriate after the pair.
    const HRESULT probe_idle=pixel_serial==1?probe->wait_for_previous_submission(5000):probe->wait_for_idle();
    if(FAILED(probe_idle))std::cerr<<"inline probe idle HRESULT "<<probe_idle<<" kind "<<int(prepared.kind)
        <<" reason "<<int(prepared.reason)<<" queued "<<queued<<" blocked "<<blocked
        <<" device "<<state->d3d12_device->GetDeviceRemovedReason()<<'\n';
    check(SUCCEEDED(probe_idle),"inline probe completion");
    gen->afw_temporal_synthesizer=active;
    gen->current_slot=current_slot;gen->synthetic_slot=synthetic_slot;
    check(blocked && outside_gpu_lock && prepared.kind==(pixel_serial==1?
            PreparedGenerationKind::prime:PreparedGenerationKind::pair),
        "inline AFW released XR output before exact D3D11 publication completion");
    state->d3d11_context->CopyResource(readback.Get(),gpu_outputs.at(prepared.current_handle));
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(SUCCEEDED(state->d3d11_context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped)),"inline fresh pixels");
    const auto actual=*static_cast<const std::uint32_t*>(mapped.pData);
    state->d3d11_context->Unmap(readback.Get(),0);
    check(actual==terminal_optical_pixel(pixel_serial,0,0,0),"inline AFW returned stale pixels");
    std::cout<<"Inline AFW "<<(pixel_serial==1?"prime":"S/B pair")<<": delayed D3D11 copy blocks XR release, waits outside GPU lock, returns fresh pixels\n";
    if(pixel_serial==2){probe.reset();probe_history.reset();}
}

void origin_history_preflight_gpu_test(const std::shared_ptr<SessionState>& state) {
    using Microsoft::WRL::ComPtr;
    std::array<ComPtr<ID3D12Resource>,2> colors;
    std::array<LukeOriginHistoryProbe,2> inputs;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    check(SUCCEEDED(state->d3d12_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))),"probe allocator");
    check(SUCCEEDED(state->d3d12_device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))),"probe list");
    xrfg::configure_luke_color_origin_extent(512,128);
    for(unsigned i=0;i<2;++i) {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=512;d.Height=128;
        d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        check(SUCCEEDED(state->d3d12_device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,
            D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&colors[i]))),"probe color");
        check(SUCCEEDED(xrfg::mark_luke_color_origin(list.Get(),colors[i].Get(),111+i,D3D12_RESOURCE_STATE_COMMON)),"probe actual marker");
    }
    check(SUCCEEDED(list->Close()),"probe close");ID3D12CommandList* lists[]{list.Get()};state->d3d12_queue->ExecuteCommandLists(1,lists);
    for(unsigned i=0;i<2;++i) {
        inputs[i].history=std::make_shared<xrfg::D3D12SwapchainHistory>();
        ID3D12Resource* sources[]{colors[i].Get()};
        check(SUCCEEDED(inputs[i].history->initialize(state->d3d12_device.Get(),state->d3d12_queue.Get(),sources,D3D12_RESOURCE_STATE_COMMON)),"probe history init");
        check(SUCCEEDED(inputs[i].history->capture(0,&inputs[i].capture))&&SUCCEEDED(inputs[i].history->commit(inputs[i].capture)),"probe frozen capture");
    }
    std::array<std::uint32_t,2> tokens{};std::array<HRESULT,2> results{};bool poisoned{};
    check(SUCCEEDED(read_luke_capture_origins(state->d3d12_queue.Get(),inputs,tokens,results,poisoned))&&
        !poisoned&&tokens[0]==111&&tokens[1]==112,"preflight did not read exact leased source tokens");
    for(unsigned i=0;i<2;++i) {
        xrfg::D3D12HistoryConsumerLease lease;ComPtr<ID3D12Resource> image;ComPtr<ID3D12Fence> fence;
        check(SUCCEEDED(inputs[i].history->acquire_consumer(inputs[i].capture,&lease,image.GetAddressOf(),fence.GetAddressOf())),
            "completed CRC probe left a lease busy before ordinary AFW source copy");
        inputs[i].history->cancel_consumer(lease);
        check(SUCCEEDED(inputs[i].history->wait_for_idle()),"probe retirement");
    }
    xrfg::configure_luke_color_origin_extent(256,128);
    std::cout<<"Native reset preflight: exact two source CRCs, producer waits and reusable retired history leases PASS\n";
}

void held_afw_pair_gpu_test(const std::shared_ptr<SessionState>& state,
    const XrFrameEndInfo& end, const ProjectionSnapshot& snapshot,
    const std::vector<ProjectionResourceMapping>& mappings, unsigned pixel_serial) {
    auto pair = std::make_shared<HeldAfwPair>();
    pair->projection = snapshot;
    pair->revision = xrfg::embedded::afw_snapshot().revision;
    pair->production_serial = pixel_serial;
    std::array<XrSwapchain, 2> handles{};
    for (std::size_t index = 0; index < mappings.size(); ++index) {
        auto chain = find_swapchain(mappings[index].application_swapchain);
        chain->dlss_logical_eye_index = static_cast<std::uint32_t>(index);
        handles[index] = chain->frame_generation->current[kAfwScratchSlot].handle;
        pair->resources[index] = {chain->handle, handles[index], static_cast<std::uint32_t>(index)};
    }
    state->luke_afw_enabled = true;
    state->graphics_binding_capabilities = 8;
    state->luke_afw = std::make_shared<xrfg::LukeAfwSynthesizer>();
    state->luke_afw_last_pair = pair;
    if(pixel_serial==1)origin_history_preflight_gpu_test(state);
    std::vector<ProjectionResourceDestination> identities;
    for (const auto& mapping : mappings)
        identities.push_back({mapping.application_swapchain, mapping.application_swapchain});
    GeneratedFrameEndInfo newer;
    check(build_generated_frame_end_info(&end, snapshot, false, identities, &newer), "held test new input");
    for (unsigned interval = 1; interval <= 4; ++interval) {
        newer.info.displayTime = end.displayTime + interval * 11'111'111;
        for (auto& projection : newer.projections)
            for (auto& view : projection.views) {
                view.pose.position = {float(interval), float(interval + 10), -float(interval)};
                view.pose.orientation = {0.0f, 0.6f, 0.0f, 0.8f};
                view.fov = {-0.5f, 0.9f, 0.8f, -0.6f};
            }
        ProjectionSnapshot current;
        check(capture_projection_snapshot(&newer.info, &current), "held test current snapshot");
        auto reordered = mappings;
        std::reverse(reordered.begin(), reordered.end());
        std::vector<PostAfwFrameInput> temporal_inputs;
        const auto held = prepare_luke_afw_frame(state, current, reordered, std::nullopt,
            100 + interval, false, temporal_inputs);
        check(held.kind == PreparedGenerationKind::prime && held.held_afw_pair == pair &&
            temporal_inputs.empty(), "actual held preparation lost immutable packet");
        GeneratedFrameEndInfo terminal;
        ProjectionSnapshot terminal_snapshot;
        check(build_afw_terminal_submission(&newer.info, current, held, terminal, terminal_snapshot),
            "held terminal construction");
        check(terminal.interface_stamp.scene_serial==pixel_serial &&
            terminal_snapshot.interface_stamp.scene_serial==pixel_serial &&
            terminal.interface_stamp.scene_owner!=0,
            "held AFW image acquired a new scene identity from an XR interval");
        // This downstream call inspects what the runtime receives, and reads
        // BOTH retained D3D11 images. A metadata-only cache cannot pass it.
        const auto runtime_end = [&](const XrFrameEndInfo* frame) {
            check(frame->displayTime == newer.info.displayTime, "held presentation time regressed");
            const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(frame->layers[0]);
            check(projection->space == snapshot.layers[0].space, "held camera space changed");
            for (unsigned eye = 0; eye != 2; ++eye) {
                const auto& received = projection->views[eye];
                const auto& saved = snapshot.layers[0].views[eye];
                check(std::memcmp(&received.pose, &saved.pose, sizeof(XrPosef)) == 0 &&
                    std::memcmp(&received.fov, &saved.fov, sizeof(XrFovf)) == 0,
                    "runtime received a newer pose/FOV for retained pixels");
                check(received.subImage.swapchain == handles[eye], "held eye resource swapped");
                state->d3d11_context->CopyResource(readback.Get(), gpu_outputs.at(received.subImage.swapchain));
                D3D11_MAPPED_SUBRESOURCE mapped{};
                check(SUCCEEDED(state->d3d11_context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
                    "held image readback");
                const auto* pixel = static_cast<const unsigned char*>(mapped.pData);
                const bool correct = pixel[0] == pixel_serial && pixel[1] == (eye ? 160 : 80);
                state->d3d11_context->Unmap(readback.Get(), 0);
                check(correct, "held packet paired with stale/wrong-eye pixels");
            }
            return XR_SUCCESS;
        };
        check(runtime_end(&terminal.info) == XR_SUCCESS, "held runtime submission");
        check(newer.projections[0].views[0].pose.position.y == float(interval + 10),
            "held builder overwrote caller metadata");
    }
    PreparedProjectionFrame rejected;
    auto changed = snapshot;
    check(!prepare_held_luke_afw_frame(pair, changed, mappings, handles, {0, 1}, pair->revision + 1, rejected),
        "held pair survived menu revision");
    auto wrong_handles = handles; wrong_handles[0] = reinterpret_cast<XrSwapchain>(0xbad);
    check(!prepare_held_luke_afw_frame(pair, changed, mappings, wrong_handles, {0, 1}, pair->revision, rejected),
        "held pair survived private resource replacement");
    check(!prepare_held_luke_afw_frame(pair, changed, mappings, handles, {1, 0}, pair->revision, rejected),
        "held pair survived logical-eye reassignment");
    changed.layers[0].space = reinterpret_cast<XrSpace>(0xbad);
    check(!prepare_held_luke_afw_frame(pair, changed, mappings, handles, {0, 1}, pair->revision, rejected),
        "held pair survived recenter/space change");
    changed = snapshot; changed.layers[0].layer_flags ^= XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    check(!prepare_held_luke_afw_frame(pair, changed, mappings, handles, {0, 1}, pair->revision, rejected),
        "held pair survived layer flag change");
    changed = snapshot; changed.layers[0].views[0].subImage.imageRect.offset.x++;
    check(!prepare_held_luke_afw_frame(pair, changed, mappings, handles, {0, 1}, pair->revision, rejected),
        "held pair survived crop change");
    check(!prepare_held_luke_afw_frame({}, snapshot, mappings, handles, {0, 1}, pair->revision, rejected),
        "invalidated pair revived");
    state->luke_afw_last_pair.reset(); state->luke_afw.reset();
    state->luke_afw_enabled = false; state->graphics_binding_capabilities = 0;
    std::cout << "Held AFW: actual preparation + terminal submission, four moved camera/FOV intervals, reversed mappings, both GPU eyes; ownership transitions rejected\n";
}

#include "luke_private_publication_tests.inc"
#include "luke_fidelity_busy_fixture.inc"
void gpu_pipeline(bool with_layers, bool transition = false, bool diagnostic = false, bool luke = false, bool gap = false,
                  bool afw_slot_probe = false, bool post_afw_packet = false, bool missing_afw_guides = false, int private_mode = 0,
                  bool isolated_lane = false) {
    terminal_optical_test = post_afw_packet;
    terminal_real_reads = terminal_synthetic_reads = 0;
    three_layer_test = with_layers;
    auto state = session();
    auto generation = std::make_shared<FrameGenerationSwapchainState>();
    if(private_mode==5)state->optical_flow_backend=xrfg::D3D12OpticalFlowBackend::fidelity_fx;
    try {
        check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &state->d3d11_device, nullptr, &state->d3d11_context)), "D3D11 hardware");
        check(SUCCEEDED(xrfg::create_d3d12_device_for_d3d11(state->d3d11_device.Get(),
            &state->d3d12_device, &state->d3d12_queue)), "D3D12 bridge");
        if(isolated_lane) {
            const auto desc=state->d3d12_queue->GetDesc();
            check(SUCCEEDED(state->d3d12_device->CreateCommandQueue(&desc,
                IID_PPV_ARGS(&state->luke_synthesis_queue))),"isolated Luke OFXR lane");
        }
        const auto diagnostic_root=std::filesystem::current_path()/("capture-integration-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        if(diagnostic) {
            std::filesystem::create_directories(diagnostic_root);
            {std::ofstream ini(diagnostic_root/"ofxr_bridge.ini");ini<<"[diagnostics]\nlogging_enabled=1\nmax_file_mb=8\n";}
            xrfg::bridge_flight_logger().initialize(diagnostic_root);
            state->frame_capture=std::make_unique<xrfg::FrameCapture>(state->d3d11_device.Get(),state->d3d11_context.Get(),diagnostic_root/"captures");
            check(state->frame_capture->start(true,4),"integration capture start");
        }
        state->dispatch->acquire_swapchain_image = [](XrSwapchain,
            const XrSwapchainImageAcquireInfo*, std::uint32_t* index) -> XrResult {
            *index = 0; return XR_SUCCESS;
        };
        state->dispatch->wait_swapchain_image = [](XrSwapchain,
            const XrSwapchainImageWaitInfo*) -> XrResult { return XR_SUCCESS; };
        state->dispatch->release_swapchain_image = [](XrSwapchain,
            const XrSwapchainImageReleaseInfo*) -> XrResult { return XR_SUCCESS; };
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 256; desc.Height = 128; desc.ArraySize = with_layers ? 1 : 2; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 6> images;
        for (auto& image : images)
            check(SUCCEEDED(state->d3d11_device->CreateTexture2D(&desc, nullptr, &image)), "texture");
        test_device = state->d3d11_device;
        state->dispatch->create_swapchain = [](XrSession, const XrSwapchainCreateInfo* info,
            XrSwapchain* handle) -> XrResult {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = info->width; d.Height = info->height; d.ArraySize = info->arraySize;
            d.MipLevels = info->mipCount; d.Format = static_cast<DXGI_FORMAT>(info->format);
            d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> image;
            if (FAILED(test_device->CreateTexture2D(&d, nullptr, &image))) return XR_ERROR_RUNTIME_FAILURE;
            *handle = reinterpret_cast<XrSwapchain>(std::uintptr_t(0xa00 + ++auxiliary_creates));
            std::scoped_lock lock(test_mutex);
            auxiliary_outputs[*handle] = image;
            return XR_SUCCESS;
        };
        state->dispatch->enumerate_swapchain_images = [](XrSwapchain handle, std::uint32_t capacity,
            std::uint32_t* count, XrSwapchainImageBaseHeader* images) -> XrResult {
            *count = 1;
            if (capacity) reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)->texture = auxiliary_outputs.at(handle).Get();
            return XR_SUCCESS;
        };
        state->dispatch->destroy_swapchain = [](XrSwapchain handle) -> XrResult {
            std::scoped_lock lock(test_mutex);
            auxiliary_destroys += static_cast<unsigned>(auxiliary_outputs.erase(handle));
            return XR_SUCCESS;
        };
        const auto initialize_generation = [&](const auto& generation, const auto& images, unsigned base) {
        ID3D11Texture2D* sources[] = {images[0].Get()};
        ID3D11Texture2D* reals[] = {images[1].Get(), images[2].Get(), images[3].Get()};
        ID3D11Texture2D* synthetics[] = {images[4].Get(), images[5].Get()};
        generation->d3d11_interop = std::make_shared<xrfg::D3D11D3D12SwapchainInterop>();
        auto& interop = generation->d3d11_interop;
        check(SUCCEEDED(interop->initialize(state->d3d11_device.Get(), state->d3d11_context.Get(),
            state->d3d12_device.Get(), state->d3d12_queue.Get(), sources, reals, synthetics)), "interop");
        if(isolated_lane) {
            check(SUCCEEDED(interop->configure_synthesis_queue(state->luke_synthesis_queue.Get())),"isolated publication timeline");
            generation->isolated_luke_synthesis=true;
        }
        auto* synthesis_queue=isolated_lane?state->luke_synthesis_queue.Get():state->d3d12_queue.Get();
        auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
        check(SUCCEEDED(history->initialize(state->d3d12_device.Get(), state->d3d12_queue.Get(),
            interop->source_images(), D3D12_RESOURCE_STATE_COMMON)), "history");
        generation->synthesizer = std::make_shared<xrfg::D3D12FrameSynthesizer>();
        check(SUCCEEDED(generation->synthesizer->initialize(state->d3d12_device.Get(),
            synthesis_queue, history, interop->current_destination_images(),
            interop->synthetic_destination_images(), desc.Format, D3D12_RESOURCE_STATE_COMMON,
            state->optical_flow_backend,{},private_mode>0,
            xrfg::D3D12FrameGenerationMode::ofxr,false,post_afw_packet)), "NVIDIA synthesis");
        if(isolated_lane) {
            generation->afw_source_synthesizer=std::make_shared<xrfg::D3D12FrameSynthesizer>();
            check(SUCCEEDED(generation->afw_source_synthesizer->initialize(state->d3d12_device.Get(),
                state->d3d12_queue.Get(),history,interop->current_destination_images(),interop->synthetic_destination_images(),
                desc.Format,D3D12_RESOURCE_STATE_COMMON,state->optical_flow_backend,{},false,
                xrfg::D3D12FrameGenerationMode::ofxr,true,true)),"isolated AFW ingress copy");
        }
        generation->current_images_per_slot = generation->synthetic_images_per_slot = 1;
        generation->current[kAfwScratchSlot].handle = reinterpret_cast<XrSwapchain>(std::uintptr_t(base+kAfwScratchSlot));
        gpu_outputs[generation->current[kAfwScratchSlot].handle] = reals[kAfwScratchSlot];
        if (post_afw_packet) {
            generation->afw_final_history = std::make_shared<xrfg::D3D12SwapchainHistory>();
            check(SUCCEEDED(generation->afw_final_history->initialize(state->d3d12_device.Get(),
                state->d3d12_queue.Get(), interop->current_destination_images(), D3D12_RESOURCE_STATE_COMMON)), "post AFW history");
            generation->afw_temporal_synthesizer = std::make_shared<xrfg::D3D12FrameSynthesizer>();
            check(SUCCEEDED(generation->afw_temporal_synthesizer->initialize(state->d3d12_device.Get(),
                synthesis_queue, generation->afw_final_history, interop->current_destination_images(),
                interop->synthetic_destination_images(), desc.Format, D3D12_RESOURCE_STATE_COMMON,
                xrfg::D3D12OpticalFlowBackend::nvidia, {}, true,
                xrfg::D3D12FrameGenerationMode::ofxr,false,true)), "post AFW OFA");
        }
        for (unsigned i = 0; i != 2; ++i) {
            generation->current[i].handle = reinterpret_cast<XrSwapchain>(std::uintptr_t(base+i));
            generation->synthetic[i].handle = reinterpret_cast<XrSwapchain>(std::uintptr_t(base+0x10+i));
            gpu_outputs[generation->current[i].handle] = reals[i];
            gpu_outputs[generation->synthetic[i].handle] = synthetics[i];
        }
        return history;
        };
        const auto history = initialize_generation(generation, images, 0x900);
        const auto interop = generation->d3d11_interop;
        if (post_afw_packet) {
            publication_receipt_independence_test(state, interop);
            completed_publication_independence_test(state,generation,history,images[0].Get());
        }
        std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 6> right_images;
        auto right_generation = std::make_shared<FrameGenerationSwapchainState>();
        std::shared_ptr<xrfg::D3D12SwapchainHistory> right_history;
        if (with_layers) {
            for (auto& image : right_images)
                check(SUCCEEDED(state->d3d11_device->CreateTexture2D(&desc, nullptr, &image)), "right texture");
            right_history = initialize_generation(right_generation, right_images, 0x920);
        }
        readback_context = state->d3d11_context;
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(SUCCEEDED(state->d3d11_device->CreateTexture2D(&desc, nullptr, &readback)), "readback");
        auto aux_desc = desc; aux_desc.ArraySize = 2;
        check(SUCCEEDED(state->d3d11_device->CreateTexture2D(&aux_desc, nullptr, &auxiliary_readback)), "aux readback");
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.width = 256; info.height = 128; info.arraySize = desc.ArraySize; info.mipCount = 1;
        info.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        auto chain = std::make_shared<SwapchainState>(state, info);
        chain->handle = reinterpret_cast<XrSwapchain>(0x800);
        gpu_outputs[chain->handle] = images[0].Get();
        chain->d3d12_history = history; chain->frame_generation = generation;
        chain->enumerated_d3d11_images.push_back(images[0]);
        auto right_chain = std::make_shared<SwapchainState>(state, info);
        right_chain->handle = reinterpret_cast<XrSwapchain>(0x801);
        right_chain->d3d12_history = right_history; right_chain->frame_generation = right_generation;
        if (with_layers) gpu_outputs[right_chain->handle] = right_images[0].Get();
        if (with_layers) right_chain->enumerated_d3d11_images.push_back(right_images[0]);
        std::array<std::shared_ptr<SwapchainState>, 2> auxiliary;
        if (with_layers) {
            for (unsigned i = 0; i != 2; ++i) {
                auto auxiliary_info = info; auxiliary_info.arraySize = 2;
                auxiliary[i] = std::make_shared<SwapchainState>(state, auxiliary_info);
                auxiliary[i]->handle = reinterpret_cast<XrSwapchain>(std::uintptr_t(0x820+i));
                if (i == 1) auxiliary[i]->create_info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
                // Use the same dimensions/array layout as the projection for
                // readback, but distinct textures and per-layer eye markers.
                D3D11_TEXTURE2D_DESC d{}; images[0]->GetDesc(&d);
                d.ArraySize = 2;
                Microsoft::WRL::ComPtr<ID3D11Texture2D> image;
                check(SUCCEEDED(state->d3d11_device->CreateTexture2D(&d, nullptr, &image)), "aux source");
                auxiliary[i]->enumerated_d3d11_images.push_back(image);
            }
        }
        state->control_revision = xrfg::embedded::snapshot().revision;
        if (private_mode == 6) {
            auto settings = xrfg::autonomous::snapshot().desired;
            settings.afw_mode = 0;
            check(xrfg::autonomous::request(settings), "markerless AFW off");
            xrfg::autonomous::apply_runtime_settings();
            state->control_revision = xrfg::embedded::snapshot().revision;
            state->luke_afw_enabled = true;
            state->graphics_binding_capabilities |= 8ULL;
            state->dlss_motion_vectors = true; // legacy Luke capability bit
            xrfg::configure_dlss_motion_vector_tracking(true);
            configure_session_guide_capture(*state);
            check(xrfg::dlss_motion_vector_statistics().status == xrfg::DlssMotionVectorStatus::disabled,
                "plain Luke OFXR still captures native guides/markers with AFW off");
        }
        if (missing_afw_guides) {
            check(xrfg::embedded::afw_snapshot().desired.mode != 0, "AFW must remain requested");
            state->luke_afw_enabled = true;
            state->graphics_binding_capabilities |= 8ULL;
            state->luke_afw = std::make_shared<xrfg::LukeAfwSynthesizer>();
            state->luke_xr_eye_baseline = .064F;
            state->luke_ofxr_previous_afw_input = true; // exercise loss->raw anchor transition
        }
        { std::scoped_lock lock(g_state_mutex);
          g_swapchains[chain->handle] = chain; g_sessions[state->handle] = state;
          if (with_layers) g_swapchains[right_chain->handle] = right_chain;
          for (const auto& a : auxiliary) if (a) g_swapchains[a->handle] = a; }
        if(private_mode==1 || private_mode==2 || private_mode==5) {
            if(private_mode==5)luke_fidelity_busy_probe(state,chain);
            else luke_private_inline_probe(state,{chain,right_chain},private_mode==1);
            for(auto c:{chain,right_chain}) {
                if(!c || !c->frame_generation || !c->frame_generation->synthesizer)continue;
                check(SUCCEEDED(c->frame_generation->synthesizer->retire_previous()),"inline probe retire");
                check(SUCCEEDED(c->frame_generation->d3d11_interop->wait_for_idle()),"inline probe drain");
            }
            {std::scoped_lock lock(g_state_mutex);g_swapchains.clear();g_sessions.clear();}
            gpu_outputs.clear();readback.Reset();auxiliary_readback.Reset();readback_context.Reset();return;
        }
        if(private_mode==3) {state->luke_afw_enabled=true;state->separate_synthesis=1;}
        if(post_afw_packet) {
            // Model the actual one-second menu stall: game queue waits for
            // CPU/XR progress while an OFXR scratch submission is pending.
            check(SUCCEEDED(interop->prepare_capture(0)),"prediction gate capture");
            xrfg::D3D12HistoryCaptureTicket capture;
            check(SUCCEEDED(history->capture(0,&capture))&&SUCCEEDED(history->commit(capture))&&
                SUCCEEDED(interop->finish_capture())&&SUCCEEDED(interop->wait_for_idle()),"prediction source ready");
            Microsoft::WRL::ComPtr<ID3D12Fence> gate;
            check(SUCCEEDED(state->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate))),"prediction gate");
            check(SUCCEEDED((isolated_lane?state->luke_synthesis_queue:state->d3d12_queue)->Wait(gate.Get(),1)),"park native work until prediction");
            xrfg::D3D12ReprojectionView v{};v.fov={-.7F,.7F,.7F,-.7F};v.image_rect={0,0,desc.Width,desc.Height};
            xrfg::D3D12FrameSynthesisTicket ticket;
            check(SUCCEEDED(generation->synthesizer->submit_copy(capture,std::span(&v,1),0,&ticket)),"pending prediction scratch");
            const bool old_luke=state->luke_afw_enabled;
            const auto old_binding=state->graphics_binding;
            state->luke_afw_enabled=true;state->graphics_binding=SessionGraphicsBinding::d3d11;
            const auto legacy=wait_for_previous_session_synthesis(state);
            const auto started=std::chrono::steady_clock::now();
            const auto prediction=wait_for_previous_session_synthesis(state,true);
            const auto elapsed=std::chrono::steady_clock::now()-started;
            const bool still_pending=generation->synthesizer->ticket_available(ticket)==S_FALSE;
            static_cast<void>(gate->Signal(1));
            state->luke_afw_enabled=old_luke;state->graphics_binding=old_binding;
            check(legacy==HRESULT_FROM_WIN32(ERROR_BUSY)&&SUCCEEDED(prediction)&&
                elapsed<std::chrono::milliseconds(100)&&still_pending,"prediction waits for game work or fakes completion");
            check(SUCCEEDED(generation->synthesizer->retire_previous())&&SUCCEEDED(history->invalidate()),"prediction reuse still retires GPU");
            std::cout<<"Actual native GPU gate: legacy prediction blocks 1s; Luke prediction returns before work, reuse still retires PASS\n";
        }
        state->synthesis_thread = std::thread(synthesis_worker_main, state);
        state->presenter_thread = std::thread(three_slot_director_main, state);
        presented.clear(); waits = begins = ends = 0;
        for (unsigned n = 1; n <= 8; ++n) {
            if (transition && n == 6)
                check(start_continuous_presenter(state, nullptr, true), "re-promotion");
            XrFrameState luke_prediction{XR_TYPE_FRAME_STATE};
            if (luke) {
                check(XR_SUCCEEDED(layer_wait_frame_impl(state->handle,nullptr,&luke_prediction)), "GPU Luke Wait");
                check(XR_SUCCEEDED(layer_begin_frame_impl(state->handle,nullptr)), "GPU Luke Begin");
            }
            // This is the producer handoff, not a frame-start pacing gate.
            const auto handoff_result = wait_for_synthesis_handoff(state);
            if (XR_FAILED(handoff_result)) std::cerr << "frame " << n << " handoff " << handoff_result << '\n';
            check(XR_SUCCEEDED(handoff_result), "GPU worker rendezvous");
            std::vector<std::uint32_t> pixels(256 * 128);
            for (unsigned eye = 0; eye != 2; ++eye) {
                if (post_afw_packet) {
                    for (unsigned y = 0; y < 128; ++y)
                        for (unsigned x = 0; x < 256; ++x)
                            pixels[y * 256 + x] = terminal_optical_pixel(n, eye, x, y);
                } else std::fill(pixels.begin(), pixels.end(), 0xff200000u | ((eye ? 160u : 80u) << 8) | n);
                if (!with_layers || eye == 0) state->d3d11_context->UpdateSubresource(images[0].Get(), eye, nullptr,
                    pixels.data(), 256 * 4, 0);
                if (with_layers && eye == 1) state->d3d11_context->UpdateSubresource(right_images[0].Get(), 0, nullptr,
                    pixels.data(), 256*4, 0);
            }
            xrfg::D3D12HistoryCaptureTicket capture{};
            if(diagnostic || luke || gap) {
                for(const auto& c:{chain,right_chain}) {
                    c->acquired_indices.push_back(0);c->waited_count=1;
                    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    check(XR_SUCCEEDED(luke ? release_swapchain_with_identity(c,&release,xrfg::LukeRenderIdentity{0x12340000,n})
                        : layer_release_swapchain_image_impl(c->handle,&release)),"projection release");
                    if(diagnostic) check(c->last_diagnostic_capture!=0,"release hook missed diagnostic sample");
                }
            } else {
            check(SUCCEEDED(interop->prepare_capture(0)), "capture transfer");
            check(SUCCEEDED(history->capture(0, &capture)), "capture ring");
            check(SUCCEEDED(interop->finish_capture()), "capture finish");
            check(SUCCEEDED(history->commit(capture)), "capture commit");
            { std::scoped_lock lock(chain->mutex); chain->last_released_capture = capture; }
            if (with_layers) {
                const auto& bridge = right_generation->d3d11_interop;
                check(SUCCEEDED(bridge->prepare_capture(0)), "right capture transfer");
                check(SUCCEEDED(right_history->capture(0, &capture)), "right capture ring");
                check(SUCCEEDED(bridge->finish_capture()), "right capture finish");
                check(SUCCEEDED(right_history->commit(capture)), "right capture commit");
                std::scoped_lock lock(right_chain->mutex); right_chain->last_released_capture = capture;
            }
            }
            if(gap && (n==4 || n==6)) {
                auto c=n==4 ? chain : right_chain;
                c->acquired_indices.push_back(0);c->waited_count=1;
                XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                check(XR_SUCCEEDED(layer_release_swapchain_image_impl(c->handle,&release)),"extra projection release");
            }
            if (private_mode == 6) {
                // Crimson gameplay releases alternating-eye AER sources with
                // different render counters. Both exact XR captures exist;
                // the raw optical path must not require one shared counter.
                chain->luke_released_identity = xrfg::LukeRenderIdentity{0x12340000, n * 2};
                right_chain->luke_released_identity = xrfg::LukeRenderIdentity{0x12340000, n * 2 - 1};
            }
            std::array<XrCompositionLayerQuad, 2> quads{};
            XrBaseInStructure unsupported_extension{XR_TYPE_UNKNOWN};
            if (with_layers) {
                for (unsigned i = 0; i != 2; ++i) {
                    auto& aux = auxiliary[i];
                    if (i == 0 || n == 1) { // static second layer releases ONLY once
                        std::fill(pixels.begin(), pixels.end(), 0xff200000u | (60u*(i+1) << 8) | (i ? 55 : n));
                        for (unsigned eye = 0; eye != 2; ++eye)
                            state->d3d11_context->UpdateSubresource(aux->enumerated_d3d11_images[0].Get(), eye,
                                nullptr, pixels.data(), 256*4, 0);
                        aux->acquired_indices.push_back(0); aux->waited_count = 1;
                        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                        check(XR_SUCCEEDED(layer_release_swapchain_image_impl(aux->handle, &release)), "aux release");
                        // Poison the app texture BEFORE EndFrame. Any capture
                        // at EndFrame instead of intercepted release fails.
                        std::fill(pixels.begin(), pixels.end(), 0xffeeffffu);
                        for (unsigned eye = 0; eye != 2; ++eye)
                            state->d3d11_context->UpdateSubresource(aux->enumerated_d3d11_images[0].Get(), eye,
                                nullptr, pixels.data(), 256*4, 0);
                    }
                    auto& q = quads[i]; q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                    q.eyeVisibility = i ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
                    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    q.subImage.swapchain = aux->handle; q.subImage.imageArrayIndex = i;
                    q.subImage.imageRect = {{3, 0}, {240, 128}};
                    q.pose.orientation.w = 1; q.pose.position.x = static_cast<float>(i+1);
                }
                if (transition && n == 5) quads[0].next = &unsupported_extension;
            }
            std::array<XrCompositionLayerProjectionView, 2> views;
            for (unsigned eye = 0; eye != 2; ++eye) {
                auto& view = views[eye]; view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                view.pose.orientation.w = 1; view.pose.position.x = eye ? 0.032F : -0.032F;
                view.pose.position.y = static_cast<float>(n);
                view.fov = {-0.7F, 0.7F, 0.7F, -0.7F};
                view.subImage.swapchain = chain->handle; view.subImage.imageArrayIndex = with_layers ? 0 : eye;
                if (with_layers && eye == 1) view.subImage.swapchain = right_chain->handle;
                view.subImage.imageRect.extent = {256, 128};
            }
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection.space = reinterpret_cast<XrSpace>(0x100);
            projection.viewCount = 2; projection.views = views.data();
            const XrCompositionLayerBaseHeader* layers[] = {
                reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection),
                reinterpret_cast<XrCompositionLayerBaseHeader*>(&quads[0]),
                reinterpret_cast<XrCompositionLayerBaseHeader*>(&quads[1])};
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = 1000000000 + n * 4000000;
            if (luke) end.displayTime = luke_prediction.predictedDisplayTime;
            end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            end.layerCount = with_layers ? 3 : 1; end.layers = layers;
            ProjectionSnapshot snapshot;
            check(capture_projection_snapshot(&end, &snapshot), "snapshot");
            auto mappings = build_projection_resource_mappings(snapshot);
            check(mappings.ready(), "stereo mappings");
            check(mappings.mappings.size() == (with_layers ? 2 : 1), "split-eye mapping count");
            if (afw_slot_probe && n == 1) {
                // Reproduce an OFXR -> AFW transition with independent eye
                // cursors, including the asymmetric 0/1 state in PID12328.
                for (unsigned cursors = 0; cursors < 4; ++cursors) {
                    for (std::size_t eye = 0; eye < mappings.mappings.size(); ++eye) {
                        const auto& mapping = mappings.mappings[eye];
                        const auto chain = find_swapchain(mapping.application_swapchain);
                        const auto gen = chain->frame_generation;
                        const auto cursor = (cursors >> eye) & 1U;
                        gen->current_slot = cursor;
                        gen->synthetic_slot = 1U - cursor;
                        const auto views = build_reprojection_views(snapshot, mapping);
                        check(views.has_value(), "AFW slot probe reprojection inputs");
                        AfwSourceCapture frozen;
                        {
                            std::scoped_lock lock(chain->mutex);
                            check(chain->last_released_capture.has_value(), "AFW admission color missing");
                            frozen = {gen, *chain->last_released_capture};
                            // Late producer metadata must not reselect the
                            // admitted source after preparation starts.
                            chain->last_released_capture.reset();
                        }
                        const auto prepared = prepare_frame_generation(mapping.application_swapchain,
                            false, *views, 0.5f, false, nullptr, true, nullptr, &frozen);
                        {
                            std::scoped_lock lock(chain->mutex);
                            chain->last_released_capture = frozen.capture;
                        }
                        check(prepared.kind == PreparedGenerationKind::prime && prepared.publication_deferred,
                            "AFW scratch prime not prepared");
                        check(prepared.current_handle == gen->current[kAfwScratchSlot].handle,
                            "AFW scratch current handle wrong");
                        check(prepared.synthetic_handle == gen->synthetic[0].handle,
                            "AFW synthetic handle wrong");
                        check(prepared.current_destination_index / gen->current_images_per_slot == kAfwScratchSlot,
                            "AFW scratch flat destination wrong");
                        check(gen->current[kAfwScratchSlot].phase == PrivateOwnershipPhase::waited,
                            "AFW scratch ownership not waited");
                        check(gen->current[1].phase == PrivateOwnershipPhase::idle,
                            "AFW scratch modified director output ownership");
                        check(gen->current_slot == cursor && gen->synthetic_slot == 1U - cursor,
                            "AFW source changed standalone OFXR ring cursors");
                        const auto bridge = gen->d3d11_interop;
                        check(SUCCEEDED(bridge->publish(prepared.current_destination_index, std::nullopt)),
                            "AFW slot probe publish");
                        check(bridge->publication_snapshot().current_index == prepared.current_destination_index,
                            "AFW copy targeted a different slot from release");
                        check(release_private_image(state.get(), state->dispatch, gen->current[kAfwScratchSlot]),
                            "AFW slot probe release");
                        check(gen->current[kAfwScratchSlot].phase == PrivateOwnershipPhase::idle &&
                              gen->current[1].phase == PrivateOwnershipPhase::idle,
                            "AFW source left an output image owned");
                        check(SUCCEEDED(bridge->wait_for_idle()), "AFW slot probe GPU completion");
                        state->d3d11_context->CopyResource(readback.Get(), gpu_outputs.at(prepared.current_handle));
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        check(SUCCEEDED(state->d3d11_context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
                            "AFW slot probe pixel readback");
                        const auto* pixel = static_cast<const unsigned char*>(mapped.pData);
                        const bool fresh = pixel[0] == n && pixel[1] == (eye ? 160 : 80);
                        state->d3d11_context->Unmap(readback.Get(), 0);
                        check(fresh, "AFW scratch slot metadata references stale/wrong-eye pixels");
                    }
                }
                held_afw_pair_gpu_test(state, end, snapshot, mappings.mappings, n);
                generation->current_slot = generation->synthetic_slot = 0;
                right_generation->current_slot = right_generation->synthetic_slot = 0;
                std::cout << "AFW GPU source/publication/release: frozen admission color despite late metadata change, both eyes/four cursor states; fresh pixels verified\n";
            }
            if (!luke) { std::scoped_lock lock(state->mutex); state->pending_frames.push_back({end.displayTime, 2000000}); }
            if (missing_afw_guides) state->luke_afw_released_eye = 0;
            if (post_afw_packet) {
                PreparedProjectionFrame terminal_prepared;
                terminal_prepared.kind = PreparedGenerationKind::prime;
                std::vector<PostAfwFrameInput> packet;
                for (const auto& mapping : mappings.mappings) {
                    const auto c = find_swapchain(mapping.application_swapchain);
                    auto gen = c->frame_generation;
                    const auto input_views = build_reprojection_views(snapshot, mapping);
                    check(input_views.has_value(), "post AFW source views");
                    const AfwSourceCapture source{gen,*c->last_released_capture};
                    const auto scratch = prepare_frame_generation(c->handle,
                        false, *input_views, 0.5f, false, nullptr, true,nullptr,isolated_lane?&source:nullptr);
                    check(scratch.kind == PreparedGenerationKind::prime && scratch.publication_deferred,
                        "post AFW scratch preparation");
                    xrfg::D3D12HistoryCaptureTicket final_capture{};
                    xrfg::D3D11PublicationReceipt receipt;
                    check(SUCCEEDED(gen->d3d11_interop->publish(scratch.current_destination_index,
                        std::nullopt, &receipt)), "post AFW scratch publication");
                    check(SUCCEEDED(xrfg::wait_for_d3d11_publication(receipt)), "terminal XR output readiness");
                    // Deliberately poison the intermediate D3D12 mirror AFTER
                    // terminal publication. Only a read of the actual XR D3D11
                    // image can recover the correct pixels for this packet.
                    HANDLE shared{};
                    const auto mirror = gen->d3d11_interop->current_destination_images()[scratch.current_destination_index];
                    check(SUCCEEDED(state->d3d12_device->CreateSharedHandle(mirror, nullptr,
                        GENERIC_ALL, nullptr, &shared)), "terminal mirror poison handle");
                    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
                    Microsoft::WRL::ComPtr<ID3D11Texture2D> alias;
                    check(SUCCEEDED(state->d3d11_device.As(&device1)), "terminal mirror poison device");
                    const auto opened = device1->OpenSharedResource1(shared, IID_PPV_ARGS(&alias));
                    CloseHandle(shared);
                    check(SUCCEEDED(opened), "terminal mirror poison alias");
                    std::fill(pixels.begin(), pixels.end(), 0xffeeffffu);
                    state->d3d11_context->UpdateSubresource(alias.Get(), 0, nullptr, pixels.data(), 256*4, 0);
                    check(SUCCEEDED(gen->d3d11_interop->capture_published_current(receipt)),
                        "exact terminal D3D11 snapshot");
                    check(SUCCEEDED(gen->afw_final_history->capture(scratch.current_destination_index,
                        &final_capture)), "post AFW capture");
                    check(SUCCEEDED(gen->d3d11_interop->finish_capture()), "terminal snapshot completion");
                    check(SUCCEEDED(gen->afw_final_history->commit(final_capture)), "post AFW commit");
                    const auto identity=make_color_identity(n,final_capture.serial,
                        gen->current[kAfwScratchSlot].handle,scratch.current_destination_index,
                        static_cast<std::uint32_t>(packet.size()),0,input_views->front());
                    check(release_private_image(state.get(), state->dispatch, gen->current[kAfwScratchSlot]),
                        "post AFW scratch release");
                    packet.push_back({c->handle, gen->current[kAfwScratchSlot].handle,
                        gen, final_capture, *input_views,identity});
                    terminal_prepared.resources.push_back({c->handle, scratch});
                    // Deliberately remove the AER ticket. The worker must not
                    // fetch another capture/camera after packet admission.
                    c->last_released_capture.reset();
                }
                GeneratedFrameEndInfo terminal;
                ProjectionSnapshot terminal_snapshot;
                terminal_prepared.scene_serial=n;
                check(build_afw_terminal_submission(&end, snapshot, terminal_prepared,
                    terminal, terminal_snapshot), "single terminal submission builder");
                check(terminal_snapshot.interface_stamp.scene_serial==n,
                    "post AFW director packet lost its source scene identity");
                if (n == 1) {
                    auto& terminal_view = terminal.projections.front().views.front();
                    terminal_view.next = reinterpret_cast<const void*>(0x123);
                    check(!freeze_completed_afw_frame(state, &terminal.info, terminal_snapshot, packet, n),
                        "exact-output probe silently stripped projection extension");
                    terminal_view.next = nullptr;
                }
                if (n == 1) {
                    // A color chain can be generation-eligible at enumeration,
                    // then appear as a HUD quad. Reproduce the skipped release
                    // and prove AFW admission classifies it before cold failure,
                    // allowing the NEXT release to populate its owned image.
                    auto& aux = auxiliary[0];
                    aux->frame_generation = std::make_shared<FrameGenerationSwapchainState>();
                    aux->composition_referenced = false;
                    check(capture_composition_image(aux, 0) == S_FALSE,
                        "unclassified generated HUD capture must be skipped");
                    check(!freeze_completed_afw_frame(state, &terminal.info, terminal_snapshot, packet, n),
                        "AFW accepted HUD without released capture");
                    check(aux->composition_referenced,
                        "AFW ownership cold start never classified auxiliary chain");
                    std::fill(pixels.begin(), pixels.end(), 0xff203c00u | n);
                    for (unsigned eye = 0; eye != 2; ++eye)
                        state->d3d11_context->UpdateSubresource(aux->enumerated_d3d11_images[0].Get(), eye,
                            nullptr, pixels.data(), 256*4, 0);
                    check(capture_composition_image(aux, 0) == S_OK,
                        "classified HUD next release capture");
                    aux->composition_capture->valid = true;
                    std::fill(pixels.begin(), pixels.end(), 0xffeeffffu);
                    for (unsigned eye = 0; eye != 2; ++eye)
                        state->d3d11_context->UpdateSubresource(aux->enumerated_d3d11_images[0].Get(), eye,
                            nullptr, pixels.data(), 256*4, 0);
                    aux->frame_generation.reset();
                }
                if (n == 1) {
                    const auto fill_auxiliary_capture = [&](std::uint32_t value) {
                        std::fill(pixels.begin(), pixels.end(), value);
                        for (const auto& aux : auxiliary)
                            for (unsigned eye = 0; eye != 2; ++eye)
                                state->d3d11_context->UpdateSubresource(aux->composition_capture->texture.Get(),
                                    eye, nullptr, pixels.data(), 256*4, 0);
                    };
                    const auto read_auxiliary_pixel = [&](const auto& owner) {
                        const auto& image = owner->submission->composition_images.front();
                        check(SUCCEEDED(wait_composition_fence(image->fence.Get(), image->ready_value)),
                            "auxiliary lease pixel readiness");
                        const auto& texture = image->textures[image->image.acquired_index];
                        D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
                        desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        desc.BindFlags = 0; desc.MiscFlags = 0;
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
                        check(SUCCEEDED(state->d3d11_device->CreateTexture2D(&desc, nullptr, &staging)),
                            "auxiliary lease pixel staging");
                        state->d3d11_context->CopyResource(staging.Get(), texture.Get());
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        check(SUCCEEDED(state->d3d11_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
                            "auxiliary lease pixel readback");
                        const auto value = *static_cast<const std::uint32_t*>(mapped.pData);
                        state->d3d11_context->Unmap(staging.Get(), 0);
                        return value;
                    };
                    // A and B remain retained by the director/worker while C
                    // is frozen BEFORE the normal capacity gate. No old frame
                    // may be reused to make room for this producer snapshot.
                    fill_auxiliary_capture(0xff112201u);
                    auto a = freeze_completed_afw_frame(state, &terminal.info, terminal_snapshot, packet, n);
                    fill_auxiliary_capture(0xff334402u);
                    auto b = freeze_completed_afw_frame(state, &terminal.info, terminal_snapshot, packet, n);
                    fill_auxiliary_capture(0xff556603u);
                    auto c = freeze_completed_afw_frame(state, &terminal.info, terminal_snapshot, packet, n);
                    check(a && b && c, "third pre-admission AFW auxiliary lease missing");
                    const auto handle_a = a->submission->composition_images.front()->image.handle;
                    const auto handle_b = b->submission->composition_images.front()->image.handle;
                    const auto handle_c = c->submission->composition_images.front()->image.handle;
                    check(handle_a != handle_b && handle_a != handle_c && handle_b != handle_c,
                        "pre-admission auxiliary overwrote a retained endpoint handle");
                    check(read_auxiliary_pixel(a) == 0xff112201u &&
                          read_auxiliary_pixel(b) == 0xff334402u &&
                          read_auxiliary_pixel(c) == 0xff556603u,
                        "pre-admission auxiliary overwrote retained endpoint pixels");
                    check(!freeze_completed_afw_frame(state, &terminal.info, terminal_snapshot, packet, n),
                        "auxiliary pool grew beyond its bounded ownership budget");
                    a.reset();
                    fill_auxiliary_capture(0xff778804u);
                    auto recycled = freeze_completed_afw_frame(state, &terminal.info, terminal_snapshot, packet, n);
                    check(recycled && recycled->submission->composition_images.front()->image.handle == handle_a,
                        "released auxiliary lease was not reusable");
                    check(read_auxiliary_pixel(b) == 0xff334402u &&
                          read_auxiliary_pixel(c) == 0xff556603u &&
                          read_auxiliary_pixel(recycled) == 0xff778804u,
                        "auxiliary recycling contaminated still-retained endpoints");
                    // Restore the real fixture's released dynamic/static HUD
                    // colors; application textures remain deliberately poisoned.
                    for (unsigned i = 0; i != auxiliary.size(); ++i) {
                        std::fill(pixels.begin(), pixels.end(),
                            0xff200000u | (60u*(i+1) << 8) | (i ? 55u : n));
                        for (unsigned eye = 0; eye != 2; ++eye)
                            state->d3d11_context->UpdateSubresource(auxiliary[i]->composition_capture->texture.Get(),
                                eye, nullptr, pixels.data(), 256*4, 0);
                    }
                    std::cout << "AFW auxiliary pool: three distinct retained colors, bounded fourth rejection, release-only recycling\n";
                }
                auto completed = freeze_completed_afw_frame(state, &terminal.info, terminal_snapshot, std::move(packet), n);
                check(completed && completed->application_serial == n, "completed direct-XR packet freeze");
                if(n<=2) inline_afw_publication_gpu_test(state,completed->inputs.front(),n);
                if (n == 1) {
                    // Reproduce promotion with one eye's inline post-AFW
                    // prime still on the GPU. The ordinary synthesizer is
                    // idle, so waiting only for it cannot protect admission.
                    Microsoft::WRL::ComPtr<ID3D12Fence> promotion_gate;
                    check(SUCCEEDED(state->d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                        IID_PPV_ARGS(&promotion_gate))), "AFW promotion fence");
                    check(SUCCEEDED((isolated_lane?state->luke_synthesis_queue:state->d3d12_queue)->Wait(promotion_gate.Get(), 1)),
                        "AFW promotion GPU hold");
                    const auto& right_input = completed->inputs.back();
                    xrfg::D3D12FrameSynthesisTicket pending_prime{};
                    const HRESULT prime_result = right_input.generation->afw_temporal_synthesizer->submit_prime(
                        right_input.capture, right_input.views, 0, &pending_prime);
                    if (FAILED(prime_result)) {
                        static_cast<void>(promotion_gate->Signal(1));
                        check(false, "AFW promotion pending prime");
                    }
                    std::atomic<bool> promotion_finished{false};
                    HRESULT promotion_result = E_PENDING;
                    std::thread promotion([&] {
                        promotion_result = wait_for_previous_session_synthesis(state);
                        promotion_finished.store(true);
                    });
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    const bool waited_for_afw = !promotion_finished.load();
                    static_cast<void>(promotion_gate->Signal(1));
                    promotion.join();
                    check(waited_for_afw && SUCCEEDED(promotion_result),
                        "director promotion ignored outstanding post-AFW GPU work");
                    check(SUCCEEDED(right_input.generation->afw_temporal_synthesizer->retire_previous()),
                        "AFW promotion prime retirement");
                }
                const auto exact_time = completed->submission->info.displayTime;
                const auto exact_pose = completed->snapshot.layers.front().views.front().pose;
                // Poison BEFORE admission: the same direct-XR AFW packet must
                // reach the worker, not reconstructed caller/session metadata.
                for (auto& v : views) v.pose.position.y = 99.0f;
                snapshot.layers.front().views.front().pose.position.y = 88.0f;
                end.displayTime += 900000000;
                if(private_mode==4) {state->luke_afw_enabled=true;state->separate_synthesis=1;}
                check(XR_SUCCEEDED(handoff_three_slot_frame(state, &end, snapshot,
                    mappings.mappings, n > 1, completed)), "post AFW immutable handoff");
                check(completed->submission->info.displayTime == exact_time &&
                    completed->snapshot.layers.front().views.front().pose.position.y == exact_pose.position.y,
                    "consumer changed direct-XR AFW packet");
            } else {
                check(XR_SUCCEEDED(layer_end_frame_impl(state->handle, &end)), "actual layer EndFrame");
            }
            if(luke) {
                const auto sequence = state->next_source_sequence;
                const auto anchor = state->previous_projection->display_time;
                check(state->luke_clock_active && !state->luke_duplicate_cycle, "GPU accepted source clock");
                check(XR_SUCCEEDED(layer_wait_frame_impl(state->handle,nullptr,&luke_prediction)), "GPU duplicate Wait");
                check(XR_SUCCEEDED(layer_begin_frame_impl(state->handle,nullptr)), "GPU duplicate Begin");
                for (const auto& c : {chain,right_chain}) {
                    const auto serial = c->last_released_capture->serial;
                    c->acquired_indices.push_back(0); c->waited_count=1;
                    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    check(XR_SUCCEEDED(release_swapchain_with_identity(c,&release,
                        xrfg::LukeRenderIdentity{0x12340000,n})),"duplicate Release");
                    check(c->luke_released_duplicate && c->last_released_capture->serial==serial,
                          "duplicate changed worker private source");
                }
                end.displayTime = luke_prediction.predictedDisplayTime;
                check(XR_SUCCEEDED(layer_end_frame_impl(state->handle,&end)),"duplicate End");
                check(state->next_source_sequence==sequence && state->previous_projection->display_time==anchor,
                    "duplicate reached director or changed anchor");
                check(state->pending_frames.empty(),"duplicate virtual frame not consumed");
                check(state->luke_duplicate_cycle, "duplicate did not arm bounded cycle wait");
            }
            if (transition && n == 5) {
                check(!continuous_presenter_active(state), "unsupported extension not demoted");
                check(!state->previous_projection, "bypass retained stale metadata anchor");
                check(state->generation_resume_wall_time == std::chrono::steady_clock::time_point{}, "bypass quarantined");
            } else check(continuous_presenter_active(state), "EndFrame demoted owned composition");
        }
        check(XR_SUCCEEDED(wait_for_presenter_idle(state)), "GPU pipeline drain");
        if (private_mode == 6) {
            const auto guides = xrfg::dlss_motion_vector_statistics();
            check(guides.status == xrfg::DlssMotionVectorStatus::disabled && guides.published == 0 &&
                guides.snapshot_copies == 0 && !chain->last_released_motion_vectors &&
                !right_chain->last_released_motion_vectors,
                "markerless OFXR published native guides or depended on their ownership");
        }
        if (missing_afw_guides) {
            const auto afw = xrfg::embedded::afw_snapshot();
            check(afw.missing_capture_frames == 8 && afw.evaluations == 0 &&
                state->luke_ofxr_previous_afw_input == false,
                "missing AFW guides must select raw OFXR without faking AFW output");
        }
        stop_continuous_presenter(state);
        if (post_afw_packet) {
            for (const auto& gen : {generation, right_generation}) {
                check(SUCCEEDED(gen->afw_temporal_synthesizer->wait_for_idle()), "post AFW optical drain");
                bool measured_final_pair = false;
                for (;;) {
                    xrfg::D3D12NvidiaGpuTiming optical{};
                    const HRESULT result = gen->afw_temporal_synthesizer->consume_nvidia_gpu_timing(&optical);
                    if (result == S_FALSE) break;
                    check(result == S_OK && optical.previous_serial + 1 == optical.current_serial &&
                        optical.current_serial >= 2 && optical.current_serial <= 8 &&
                        optical.eye_count == 1 && optical.eye0_microseconds > 0 &&
                        optical.total_microseconds >= optical.pack_microseconds + optical.composition_microseconds,
                        "post AFW synthetic did not execute a measured NVIDIA optical pair");
                    measured_final_pair |= optical.previous_serial == 7 && optical.current_serial == 8;
                }
                check(measured_final_pair, "post AFW final 7/8 optical pair was not measured");
            }
            check(terminal_real_reads == 16 && terminal_synthetic_reads == 14,
                "post AFW optical fixture missed real/synthetic image readback");
        }
        if(diagnostic) {
            state->frame_capture->shutdown();
            unsigned samples=0,views=0,handoffs=0,submissions=0,consumers=0;
            for(const auto& directory:std::filesystem::directory_iterator(diagnostic_root/"captures")) {
                std::ifstream trace(directory.path()/"trace.jsonl");std::string line;
                while(std::getline(trace,line)) {
                    if(line.find("\"kind\":\"sample\"")!=std::string::npos)++samples;
                    if(line.find("\"kind\":\"view\"")!=std::string::npos){++views;check(line.find("\"capture\":0")==std::string::npos,"view lost capture correlation");}
                    if(line.find("\"kind\":\"handoff\"")!=std::string::npos)++handoffs;
                    if(line.find("\"kind\":\"present\"")!=std::string::npos)++submissions;
                    if(line.find("\"kind\":\"consume\"")!=std::string::npos)++consumers;
                }
            }
            check(samples==16&&views==16&&handoffs==8&&submissions==15&&consumers==16,"incomplete correlated trace");
            xrfg::bridge_flight_logger().shutdown();
        }
        std::vector<std::pair<unsigned, bool>> expected;
        for (unsigned i = 1; i <= 8; ++i) {
            if (i > 1 && !(transition && (i == 5 || i == 6)) && !(gap && (i==4 || i==6))) expected.emplace_back(i, true);
            expected.emplace_back(i, false);
        }
        check(presented == expected && waits == expected.size() && begins == waits && ends == waits, "GPU order/count");
        check(presented.back() == std::pair<unsigned,bool>{8,false}, "GPU final real");
        check(gpu_pixels_correct, "real image stale, synthetic copied an endpoint, or eyes crossed");
        check(auxiliary_pixels_correct, "auxiliary ownership, pixels or metadata changed");
        for (auto& aux : auxiliary) if (aux) destroy_composition_capture(aux);
        check(auxiliary_creates == auxiliary_destroys, "auxiliary swapchain leak");
        if (with_layers) check(auxiliary_creates == (post_afw_packet ? 6u : 4u),
            "auxiliary allocation exceeded endpoint/producer ownership budget");
        check(SUCCEEDED(generation->synthesizer->wait_for_idle()), "GPU shutdown");
        check(SUCCEEDED(interop->wait_for_idle()), "publication shutdown");
        const auto publication = interop->publication_snapshot();
        check(publication.fence_identity != 0 && publication.ready_value != 0 &&
              publication.copy_complete_value > publication.ready_value &&
              publication.completed_value >= publication.copy_complete_value &&
              publication.current_index < 2,
              "V30013 publication snapshot lacks completed copy identity");
        if (with_layers) check(SUCCEEDED(right_generation->synthesizer->wait_for_idle()), "right GPU shutdown");
        { std::scoped_lock lock(g_state_mutex); g_swapchains.clear(); g_sessions.clear(); }
        gpu_outputs.clear(); readback.Reset(); auxiliary_readback.Reset(); readback_context.Reset();
        std::cout << "Actual EndFrame + NVIDIA/D3D11 worker/director: ordered unique real/synthetic; stereo pixels verified; "
                  << (with_layers ? "3 layers / 2 split-eye mappings, static/dynamic source poisoned before EndFrame, metadata and 4-handle pool verified" : "projection only")
                  << (transition ? "; unsupported-layer bypass + re-prime without serial-gap failure" : "") << '\n';
    } catch (...) {
        stop_continuous_presenter(state);
        for (const auto& c : find_swapchains(state)) if (c->composition_capture) destroy_composition_capture(c);
        { std::scoped_lock lock(g_state_mutex); g_swapchains.clear(); g_sessions.clear(); }
        gpu_outputs.clear(); readback.Reset(); auxiliary_readback.Reset(); readback_context.Reset();
        throw;
    }
}
}

void luke_inline_diagnostic_test() {
    const auto root = std::filesystem::temp_directory_path() /
        (L"ofxr-v30013-inline-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    { std::ofstream ini(root / "ofxr_bridge.ini");
      ini << "[diagnostics]\nlogging_enabled=1\ntiming_capture=1\npose_cadence_capture=1\n"; }
    auto& logger = xrfg::bridge_flight_logger();
    logger.shutdown(); logger.initialize(root);
    check(logger.ram_enabled() && !logger.enabled(), "inline diagnostic must be RAM only");
    auto s = session();
    s->graphics_binding_capabilities = 8;
    s->next_application_frame_serial = 43;
    s->dispatch->instance = reinterpret_cast<XrInstance>(0x456);
    s->dispatch->convert_time_to_qpc = [](XrInstance, XrTime time, LARGE_INTEGER* qpc) -> XrResult {
        qpc->QuadPart = time / 100 + 1234567; return XR_SUCCESS;
    };
    s->dispatch->locate_views = [](XrSession, const XrViewLocateInfo*, XrViewState* state,
        std::uint32_t capacity, std::uint32_t* count, XrView* views) -> XrResult {
        *count = 1;
        state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        if (capacity) {
            views[0].pose.orientation.w = 1.0f;
            views[0].pose.position.x = -0.032f;
            views[0].fov = {-0.7f, 0.7f, 0.8f, -0.8f};
        }
        return XR_SUCCESS;
    };
    { std::scoped_lock lock(g_state_mutex); g_sessions[s->handle] = s; }
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.space = reinterpret_cast<XrSpace>(0x123); locate.displayTime = 1000000000;
    XrView view{XR_TYPE_VIEW}; XrViewState located{XR_TYPE_VIEW_STATE}; std::uint32_t count{};
    check(layer_locate_views(s->handle, &locate, &located, 1, &count, &view) == XR_SUCCESS &&
        count == 1 && view.pose.position.x == -0.032f && view.fov.angleUp == 0.8f,
        "LocateViews observation altered downstream output");
    XrCompositionLayerProjectionView projection_view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    projection_view.pose = view.pose; projection_view.fov = view.fov;
    projection_view.subImage.swapchain = reinterpret_cast<XrSwapchain>(0xabc);
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space = locate.space; projection.viewCount = 1; projection.views = &projection_view;
    const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
    XrFrameEndInfo frame_info{XR_TYPE_FRAME_END_INFO};
    frame_info.displayTime = locate.displayTime; frame_info.layerCount = 1; frame_info.layers = layers;
    log_luke_physical_boundary(s, &frame_info, 42, 4, nullptr);
    s->dispatch->wait_frame = [](XrSession, const XrFrameWaitInfo*, XrFrameState* frame) -> XrResult {
        frame->predictedDisplayTime = 1011111111; frame->predictedDisplayPeriod = 11111111;
        frame->shouldRender = XR_TRUE; return XR_SUCCESS;
    };
    s->dispatch->begin_frame = [](XrSession, const XrFrameBeginInfo*) -> XrResult { return XR_SUCCESS; };
    s->dispatch->end_frame = [](XrSession, const XrFrameEndInfo* frame) -> XrResult {
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(frame->layers[0]);
        return frame->displayTime == 1011111111 && projection->views[0].pose.position.x == -0.032f &&
            projection->views[0].subImage.swapchain == reinterpret_cast<XrSwapchain>(0xabc)
            ? XR_SUCCESS : XR_ERROR_VALIDATION_FAILURE;
    };
    const auto cycle = submit_current_cycle(s, frame_info, 42);
    check(cycle.completed && cycle.result == XR_SUCCESS && frame_info.displayTime == locate.displayTime,
          "physical observation changed scheduling, projection pose or caller metadata");
    reference_calls = action_calls = locate_calls = 0;
    s->dispatch->create_reference_space = [](XrSession, const XrReferenceSpaceCreateInfo* info,
        XrSpace* space) -> XrResult {
        ++reference_calls;
        *space = reinterpret_cast<XrSpace>(0xdef);
        return info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW &&
            info->poseInReferenceSpace.orientation.w == 1.0f ? XR_SUCCESS : XR_ERROR_VALIDATION_FAILURE;
    };
    s->dispatch->create_action_space = [](XrSession, const XrActionSpaceCreateInfo*,
        XrSpace* space) -> XrResult { ++action_calls; *space = reinterpret_cast<XrSpace>(0xddd); return XR_SUCCESS; };
    s->dispatch->locate_space = [](XrSpace space, XrSpace base, XrTime time,
        XrSpaceLocation* location) -> XrResult {
        ++locate_calls;
        if (time == 999) return XR_ERROR_TIME_INVALID;
        if (space != reinterpret_cast<XrSpace>(0xdef) || base != reinterpret_cast<XrSpace>(0x123) ||
            time != 1000000000) return XR_ERROR_VALIDATION_FAILURE;
        location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        location->pose.orientation = {0.0f, 0.0f, 0.6f, 0.8f};
        location->pose.position = {0.125f, 0.25f, -0.5f};
        return XR_SUCCESS;
    };
    XrReferenceSpaceCreateInfo reference_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    reference_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    reference_info.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace reference{};
    check(layer_create_reference_space(s->handle, &reference_info, &reference) == XR_SUCCESS &&
        reference == reinterpret_cast<XrSpace>(0xdef), "reference space passthrough/ownership");
    XrActionSpaceCreateInfo action_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    action_info.poseInActionSpace.orientation.w = 1.0f;
    XrSpace action{};
    check(layer_create_action_space(s->handle, &action_info, &action) == XR_SUCCESS &&
        action == reinterpret_cast<XrSpace>(0xddd), "action space passthrough/ownership");
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    const auto sample_id = g_space_sample_id.load();
    check(layer_locate_space(reference, locate.space, locate.displayTime, &location) == XR_SUCCESS &&
        location.pose.position.x == 0.125f && location.pose.orientation.z == 0.6f &&
        location.pose.orientation.w == 0.8f, "absolute space sample changed runtime pose");
    const auto retained_pose = location.pose;
    check(layer_locate_space(reference, locate.space, 999, &location) == XR_ERROR_TIME_INVALID &&
        location.pose.position.x == retained_pose.position.x,
        "invalid space sample changed result/output or manufactured valid pose");
    check(reference_calls == 1 && action_calls == 1 && locate_calls == 2,
        "space observation issued extra runtime calls");
    xrfg::DlssMotionVectorFrame guide{};
    guide.publication = 777; guide.camera_data_valid = true;
    guide.camera_position = {0.125f, 0.25f, -0.5f};
    guide.camera_right = {1.0f, 0.0f, 0.0f};
    guide.camera_up = {0.0f, 1.0f, 0.0f};
    guide.camera_forward = {0.0f, 0.0f, 1.0f};
    log_luke_guide_basis(guide);
    s->dispatch->convert_time_to_qpc = nullptr;
    log_luke_physical_boundary(s, &frame_info, 43, 4, nullptr);
    std::filesystem::path saved; std::wstring error;
    check(xrfg::save_ram_timing(GetCurrentProcessId(), root, &saved, &error), "inline diagnostic save");
    std::ifstream input(saved); const std::string csv(std::istreambuf_iterator<char>{input}, {});
    check(csv.find("luke_inline_clock,0,42,1000000000,11234567") != std::string::npos &&
          csv.find("luke_inline_clock,0,42,1011111111,11345678") != std::string::npos &&
          csv.find("luke_physical_submit,600,42,1011111111,") != std::string::npos &&
          csv.find("luke_physical_submit,700,42,1011111111,") != std::string::npos &&
          csv.find("luke_locate_context,0,42,1000000000,291") != std::string::npos &&
          csv.find("luke_inline_pose_bits,6000,42,") != std::string::npos,
          "missing converted clocks, actual second slot, locate identity or raw pose");
    check(csv.find("luke_inline_clock," + std::to_string(XR_ERROR_FUNCTION_UNSUPPORTED) +
          ",43,1000000000,0") != std::string::npos, "unsupported clock was silently guessed");
    check(csv.find("luke_space_created,1,3567,") != std::string::npos &&
        csv.find("luke_space_created,-1,3549,") != std::string::npos &&
        csv.find("luke_space_locate,0," + std::to_string(sample_id) + ",3567,291") != std::string::npos &&
        csv.find("luke_space_time,3," + std::to_string(sample_id) + ",1000000000,42") != std::string::npos &&
        csv.find("luke_inline_pose_bits,9500," + std::to_string(sample_id) + ",") != std::string::npos &&
        csv.find("luke_inline_pose_bits,9500," + std::to_string(sample_id + 1) + ",") == std::string::npos &&
        csv.find("luke_render_camera,200,777,4467570831400108032,13763000462309588992") != std::string::npos,
        "missing numerical camera/absolute head identity or invalid pose logged as valid");
    logger.shutdown();
    { std::scoped_lock lock(g_state_mutex); g_sessions.erase(s->handle);
      g_observed_spaces.erase(reference); g_observed_spaces.erase(action); }
    std::cout << "V30013: unchanged LocateViews output; actual second slot, raw poses, calibrated clock and unsupported-clock records verified\n";
}

void embedded_eligibility_test() {
    check(xrfg::embedded::snapshot().desired.enabled, "control defaults");
    for (auto binding : {SessionGraphicsBinding::d3d11, SessionGraphicsBinding::d3d12}) {
        for (unsigned mode = 0; mode < 4; ++mode) {
            auto s = std::make_shared<SessionState>(std::make_shared<Dispatch>());
            s->handle = reinterpret_cast<XrSession>(0x213);
            s->graphics_binding = binding;
            s->dispatch->create_swapchain=+[](XrSession,const XrSwapchainCreateInfo*,XrSwapchain* output)->XrResult {
                if(output)*output=XR_NULL_HANDLE;
                return XR_ERROR_VALIDATION_FAILURE;
            };
            s->dispatch->destroy_swapchain=+[](XrSwapchain)->XrResult{return XR_SUCCESS;};
            s->dispatch->enumerate_swapchain_images=+[](XrSwapchain,std::uint32_t,std::uint32_t* count,XrSwapchainImageBaseHeader*)->XrResult {
                if(count)*count=0;
                return XR_ERROR_VALIDATION_FAILURE;
            };
            check(SUCCEEDED(apply_embedded_control(s)),"establish actual initial control tuple before testing admission");
            s->control_revision = 0;
            s->generation_budget_exhausted = mode == 2;
            XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            info.width=info.height=16;info.arraySize=info.faceCount=info.sampleCount=info.mipCount=1;
            info.format=DXGI_FORMAT_R8G8B8A8_UNORM;info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            auto chain = std::make_shared<SwapchainState>(s, info);
            chain->handle = reinterpret_cast<XrSwapchain>(0x213);
            chain->session = s;
            chain->generation_eligible_pending = mode != 0;
            chain->generation_declined = mode == 1;
            { std::scoped_lock lock(g_state_mutex); g_swapchains[chain->handle] = chain; }
            struct Remove {
                XrSwapchain handle;
                ~Remove(){std::scoped_lock lock(g_state_mutex);g_swapchains.erase(handle);}
            } cleanup{chain->handle};
            check(SUCCEEDED(apply_embedded_control(s)),"apply equivalent control revision before projection admission");
            check(chain->generation_declined == (mode == 1) && !chain->frame_generation,
                "control revision spent an unsubmitted source's generation budget");
            ProjectionResourceMapping mapping{};mapping.application_swapchain=chain->handle;
            check(ensure_projection_frame_generation(s,std::span<const ProjectionResourceMapping>(&mapping,1)),
                "incompatible graphics binding incorrectly exhausted the session swapchain budget");
            // Eligible attempt fails without a graphics binding: refusal must
            // latch. Ineligible/budget-exhausted chains must remain untouched.
            check(chain->generation_declined == (mode == 1 || mode == 3),
                "control eligibility/refusal gate");
            if(mode==3) {
                check(chain->generation_refusal==(binding==SessionGraphicsBinding::d3d11
                    ?SwapchainEligibilityReason::invalid_d3d11_image:SwapchainEligibilityReason::missing_dispatch_or_history),
                    "explicit projection admission returned an unrelated refusal reason");
                const auto detail=binding==SessionGraphicsBinding::d3d11
                    ?static_cast<std::uint64_t>(static_cast<std::int64_t>(XR_ERROR_VALIDATION_FAILURE)):0ULL;
                check(chain->generation_refusal_detail==detail,"explicit projection admission lost the exact runtime/binding failure");
            }
            check(s->generation_budget_exhausted==(mode==2),"local graphics refusal changed the session budget gate");
            check(!chain->frame_generation, "unexpected control allocation");
        }
    }
}

void luke_admission_test() {
    auto s=session();
    // No live Luke DLL or injected hook is required: the production Release
    // transaction receives the same typed identity its real reader supplies.
    s->dispatch->release_swapchain_image=[](XrSwapchain,const XrSwapchainImageReleaseInfo*) { return XR_SUCCESS; };
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    std::array<std::shared_ptr<SwapchainState>,2> chains;
    std::array<ProjectionResourceMapping,2> mappings;
    const xrfg::LukeRenderIdentity id{0x12340000,10};
    for(unsigned i=0;i<2;++i) {
        auto c=std::make_shared<SwapchainState>(s,info);
        c->handle=reinterpret_cast<XrSwapchain>(std::uintptr_t{0x990}+i);
        c->frame_generation=std::make_shared<FrameGenerationSwapchainState>();
        c->last_released_capture=xrfg::D3D12HistoryCaptureTicket{};
        c->last_released_capture->serial=7;
        c->luke_released_identity=id;
        chains[i]=c; mappings[i].application_swapchain=c->handle;
        std::scoped_lock lock(g_state_mutex);g_swapchains[c->handle]=c;
    }
    using A=LukePairDecision::Action;
    check(luke_pair_decision(mappings).action==A::accept,"new identity rejected");
    commit_luke_pair(mappings,id);
    s->synthesis_busy=true; // Would block forever if duplicate entered worker gate.
    for (const auto& c:chains) {
        c->acquired_indices.push_back(2); c->waited_count=1;
        const auto start=std::chrono::steady_clock::now();
        check(XR_SUCCEEDED(release_swapchain_with_identity(c,nullptr,id)),"fast duplicate release failed");
        check(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(50),"duplicate waited");
        check(c->acquired_indices.empty() && !c->waited_count && c->last_released_index==2,
              "duplicate ownership bookkeeping");
        check(c->last_released_capture->serial==7,"private source overwritten");
    }
    auto d=luke_pair_decision(mappings);
    check(d.action==A::drop && !d.reset,"duplicate reset valid history");
    s->presenter_failure=XR_ERROR_RUNTIME_FAILURE;
    check(release_swapchain_with_identity(chains[0],nullptr,id)==XR_ERROR_RUNTIME_FAILURE,
          "duplicate hid director failure");
    s->presenter_failure=XR_SUCCESS;
    chains[0]->acquired_indices.push_back(1);chains[0]->waited_count=1;
    s->dispatch->release_swapchain_image=[](XrSwapchain,const XrSwapchainImageReleaseInfo*) { return XR_ERROR_RUNTIME_FAILURE; };
    check(release_swapchain_with_identity(chains[0],nullptr,id)==XR_ERROR_RUNTIME_FAILURE &&
        chains[0]->acquired_indices.size()==1 && chains[0]->waited_count,"failed release lost ownership");
    chains[0]->acquired_indices.clear();chains[0]->waited_count=0;
    chains[1]->luke_released_identity=xrfg::LukeRenderIdentity{id.object,11};
    chains[1]->luke_released_duplicate=false;
    check(luke_pair_decision(mappings).action==A::drop && luke_pair_decision(mappings).reset,
          "mixed pair admitted");
    for(auto& c:chains) {c->luke_released_identity=xrfg::LukeRenderIdentity{id.object,30};c->luke_released_duplicate=false;}
    check(luke_pair_decision(mappings).action==A::accept,"jump incorrectly filled or rejected");
    for(auto& c:chains)c->luke_released_identity=xrfg::LukeRenderIdentity{id.object,1};
    check(luke_pair_decision(mappings).action==A::accept && luke_pair_decision(mappings).reset,"reset not reprime");
    for(auto& c:chains) {c->luke_released_identity.reset();c->luke_admitted_identity.reset();}
    check(luke_pair_decision(mappings).action==A::normal,"non-Luke route changed");
    s->synthesis_busy=false;s->presenter_active=false;
    { std::scoped_lock lock(g_state_mutex);for(auto& c:chains)g_swapchains.erase(c->handle); }
}

void luke_same_time_baseline_test() {
    auto& logger = xrfg::bridge_flight_logger();
    logger.shutdown();
    check(!logger.ram_enabled(), "baseline regression must run without RAM logging");
    auto s = session();
    s->graphics_binding_capabilities = 8;
    static std::array<XrView, 2> runtime_views;
    static XrViewStateFlags runtime_flags;
    static XrResult runtime_result;
    runtime_views = {XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
    for (auto& view : runtime_views) {
        view.pose.orientation.w = 1.0f;
        view.fov = {-0.7f, 0.7f, 0.8f, -0.8f};
    }
    runtime_views[0].pose.position.x = -0.03206f;
    runtime_views[1].pose.position.x = 0.03206f;
    runtime_flags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    runtime_result = XR_SUCCESS;
    s->dispatch->locate_views = [](XrSession, const XrViewLocateInfo*, XrViewState* out,
        std::uint32_t capacity, std::uint32_t* count, XrView* views) -> XrResult {
        *count = 2;
        out->viewStateFlags = runtime_flags;
        for (std::uint32_t eye = 0; eye < std::min(capacity, 2u); ++eye)
            views[eye] = runtime_views[eye];
        return runtime_result;
    };
    { std::scoped_lock lock(g_state_mutex); g_sessions[s->handle] = s; }
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = 1000000000;
    const auto sample = [&] {
        std::array<XrView, 2> output{XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
        XrViewState view_state{XR_TYPE_VIEW_STATE};
        std::uint32_t count{};
        check(layer_locate_views(s->handle, &locate, &view_state, 2, &count,
            output.data()) == runtime_result && count == 2 && view_state.viewStateFlags == runtime_flags,
            "baseline latch changed runtime result/count/flags");
        for (std::size_t eye = 0; eye < output.size(); ++eye)
            check(std::memcmp(&output[eye].pose, &runtime_views[eye].pose, sizeof(XrPosef)) == 0 &&
                std::memcmp(&output[eye].fov, &runtime_views[eye].fov, sizeof(XrFovf)) == 0,
                "baseline latch modified the application's located pose/FOV");
    };
    sample();
    check(s->luke_xr_eye_baseline && std::abs(*s->luke_xr_eye_baseline - 0.06412f) < 1e-6f,
        "valid stereo locate did not latch physical IPD without logging");
    for (auto& view : runtime_views) {
        view.pose.position.x += 0.17f;
        view.pose.position.y = -0.3f;
        view.pose.position.z = 0.42f;
    }
    sample();
    check(s->luke_xr_eye_baseline && std::abs(*s->luke_xr_eye_baseline - 0.06412f) < 1e-6f,
        "common headset translation contaminated same-time physical IPD");
    s->luke_xr_eye_baseline.reset();
    runtime_flags = XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    sample();
    check(!s->luke_xr_eye_baseline, "invalid position flags manufactured a baseline");
    runtime_flags |= XR_VIEW_STATE_POSITION_VALID_BIT;
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO;
    sample();
    check(!s->luke_xr_eye_baseline, "non-stereo configuration manufactured a baseline");
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    runtime_result = XR_ERROR_RUNTIME_FAILURE;
    sample();
    check(!s->luke_xr_eye_baseline, "failed locate manufactured a baseline");
    runtime_result = XR_SUCCESS;
    s->graphics_binding_capabilities = 0;
    sample();
    check(!s->luke_xr_eye_baseline, "non-Luke locate acquired an AFW baseline");
    s->presenter_active = false;
    { std::scoped_lock lock(g_state_mutex); g_sessions.erase(s->handle); }
    std::cout << "Luke baseline latch: unchanged poses, translation-invariant IPD, invalid samples rejected without logging\n";
}

void luke_cycle_clock_test() {
    using Clock = std::chrono::steady_clock;
    auto s = session();
    { std::scoped_lock lock(g_state_mutex); g_sessions[s->handle] = s; }
    s->synthesis_busy = true; // No worker/director thread: neither can make progress.
    const auto cycle = [&] {
        XrFrameState prediction{XR_TYPE_FRAME_STATE};
        check(XR_SUCCEEDED(layer_wait_frame_impl(s->handle, nullptr, &prediction)), "Luke clock Wait");
        check(XR_SUCCEEDED(layer_begin_frame_impl(s->handle, nullptr)), "Luke clock Begin");
        consume_application_frame(s, prediction.predictedDisplayTime, false, true);
        s->application_frame_in_progress = false;
        return prediction;
    };
    cycle();
    update_luke_application_clock(*s, true, false);
    check(s->luke_clock_active, "recognized route failed to enable clock");
    const auto origin = s->luke_clock_origin;
    const auto origin_wall = s->luke_clock_wall_origin;
    auto last = s->last_virtual_display_time;
    for (const XrDuration period : {8333333LL, 11111111LL, 13888889LL}) {
        s->presenter_frame_state.predictedDisplayPeriod = period;
        for (unsigned i = 0; i < 8; ++i) {
            update_luke_application_clock(*s, true, true);
            const auto deadline = s->application_wait_returned_at + std::chrono::nanoseconds(period);
            const auto prediction = cycle();
            check(Clock::now() >= deadline, "duplicate escaped native-period deadline");
            check(prediction.predictedDisplayTime - last >= period, "duplicate nanosecond clock survived");
            check(prediction.predictedDisplayPeriod == period, "Luke period doubled");
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - origin_wall).count();
            const auto clock_elapsed = prediction.predictedDisplayTime - origin;
            check(clock_elapsed <= elapsed && elapsed - clock_elapsed < 5'000'000,
                  "Luke prediction follows calls/director instead of elapsed time");
            last = prediction.predictedDisplayTime;
        }
    }
    check(waits == 0 && begins == 0 && ends == 0, "duplicate called physical runtime");
    // A fresh admission must not inherit the duplicate deadline, even with
    // synthesis held busy and a long headset period in the test fixture.
    s->presenter_frame_state.predictedDisplayPeriod = 50'000'000;
    update_luke_application_clock(*s, true, false);
    auto start = Clock::now(); cycle();
    check(Clock::now() - start < std::chrono::milliseconds(25), "fresh input paced on duplicate timer");
    // Already spent a period producing: no additional full-period sleep.
    update_luke_application_clock(*s, true, true);
    s->application_wait_returned_at -= std::chrono::milliseconds(100);
    start = Clock::now(); cycle();
    check(Clock::now() - start < std::chrono::milliseconds(25), "slow cycle got additive wait");
    // Error/stop cancellation releases the CV, rather than waiting for a tick.
    for (bool stop : {false, true}) {
        update_luke_application_clock(*s, true, true);
        std::thread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            { std::scoped_lock lock(s->presenter_mutex);
              if (stop) s->presenter_stop_requested = true;
              else s->presenter_failure = XR_ERROR_RUNTIME_FAILURE; }
            s->presenter_condition.notify_all();
        });
        XrFrameState prediction{XR_TYPE_FRAME_STATE};
        const auto result = layer_wait_frame_impl(s->handle, nullptr, &prediction);
        cancel.join();
        check(result == (stop ? XR_ERROR_SESSION_NOT_RUNNING : XR_ERROR_RUNTIME_FAILURE),
              "Luke duplicate hid stop/error");
        s->presenter_failure = XR_SUCCESS; s->presenter_stop_requested = false;
    }
    update_luke_application_clock(*s, false, false);
    check(!s->luke_clock_active && !s->luke_duplicate_cycle, "unsupported route kept Luke clock");
    s->synthesis_busy = false; s->presenter_active = false;
    { std::scoped_lock lock(g_state_mutex); g_sessions.erase(s->handle); }
}

void native_stereo_time_match_test() {
    auto state = session();
    state->pending_frames.push_back({100, 11});
    check(latest_pending_application_period(state, 100, false, true) == 11,
          "exact Native frame time failed");
    check(latest_pending_application_period(state, 105, false, true) == 11,
          "Native Stereo Fix time rewrite rejected");
    consume_application_frame(state, 105, false, true);
    check(state->pending_frames.empty(), "rewritten Native frame was not consumed");

    state->pending_frames.push_back({100, 11});
    state->pending_frames.push_back({200, 12});
    check(latest_pending_application_period(state, 205, false, true) == 12,
          "Native Stereo Fix selected a stale pending frame");
    check(!latest_pending_application_period(state, 205, false, false),
          "ambiguous frame time was accepted without begin ownership");
    check(latest_pending_application_period(state, 205, true, false) == 11,
          "overlapping wait lost submission order");
    consume_application_frame(state, 205, false, true);
    check(state->pending_frames.empty(), "stale pending Native frames survived");
}

void afw_producer_release_independence_test() {
    auto s = session();
    s->luke_afw_enabled = true;
    s->three_slot_director = true;
    s->synthesis_busy = true;
    s->dispatch->release_swapchain_image = [](XrSwapchain,
        const XrSwapchainImageReleaseInfo*) -> XrResult { return XR_SUCCESS; };
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    auto chain = std::make_shared<SwapchainState>(s, info);
    chain->frame_generation = std::make_shared<FrameGenerationSwapchainState>();
    std::mutex mutex;
    std::condition_variable condition;
    bool finished = false;
    XrResult result = XR_ERROR_RUNTIME_FAILURE;
    std::thread producer([&] {
        result = release_swapchain_with_identity(chain, nullptr, std::nullopt);
        { std::scoped_lock lock(mutex); finished = true; }
        condition.notify_all();
    });
    bool independent;
    { std::unique_lock lock(mutex);
      independent = condition.wait_for(lock, std::chrono::milliseconds(100), [&] { return finished; }); }
    // Release a regressed wait before asserting, so a failing test cannot hang.
    { std::scoped_lock lock(s->presenter_mutex); s->synthesis_busy = false; }
    s->presenter_condition.notify_all();
    producer.join();
    check(independent && XR_SUCCEEDED(result), "AFW producer Release waited for OFXR worker");
}

void refused_frame_recovery_test() {
    for (bool director : {false, true}) {
        for (XrResult error : {XR_ERROR_LAYER_INVALID, XR_ERROR_LAYER_LIMIT_EXCEEDED,
                              XR_ERROR_SWAPCHAIN_RECT_INVALID, XR_ERROR_POSE_INVALID}) {
            auto s = session();
            s->three_slot_director = director;
            s->nvidia_new_route = director;
            RegisteredFixtureSession registration(s);
            try {
                auto rejected_frame = frame(61);
                if (!director) s->presenter_last_frame = frame(60)->frame;
                auto rejected = enqueue_presenter_submission(s, nullptr, &rejected_frame->frame->info);
                refuse_next_end = error;
                s->presenter_thread = std::thread(
                    director ? three_slot_director_main : continuous_presenter_main, s);
                until(s, [&] { return rejected->completed; });
                {
                    std::scoped_lock lock(s->presenter_mutex);
                    check(rejected->result == error && XR_SUCCEEDED(s->presenter_failure),
                        "one refused frame poisoned the session");
                    check(!s->presenter_last_frame && !s->presenter_inflight_frame &&
                          !s->presenter_inflight_submission && s->outstanding_presenter_submissions == 0,
                        "refused frame ownership/repeat retained");
                }
                auto next_frame = frame(62);
                auto next = enqueue_presenter_submission(s, nullptr, &next_frame->frame->info);
                until(s, [&] { return next->completed; });
                {
                    std::scoped_lock lock(s->presenter_mutex);
                    check(XR_SUCCEEDED(next->result) && XR_SUCCEEDED(s->presenter_failure),
                        "session did not recover after content refusal");
                }
                stop_continuous_presenter(s);
            } catch (...) {
                std::cerr << "content-refusal probe failed: director=" << director
                          << " error=" << error << '\n';
                stop_continuous_presenter(s);
                refuse_next_end = XR_SUCCESS;
                throw;
            }
        }
    }
    // Exercise an owned director slot as well as borrowed submissions. The
    // refused A must be consumed so that B can drain; no slot/failure is stuck.
    auto s = session();
    RegisteredFixtureSession registration(s);
    try {
        auto rejected = frame(71);
        s->three_slots.push_real(rejected);
        s->three_slots.shift_empty(); s->three_slots.shift_empty();
        s->three_slots.push_real(frame(72));
        refuse_next_end = XR_ERROR_LAYER_INVALID;
        s->presenter_thread = std::thread(three_slot_director_main, s);
        until(s, [&] { return s->three_slots.can_insert_synthetic(); });
        {
            std::scoped_lock lock(s->presenter_mutex);
            check(s->three_slots.push_synthetic(frame(72, true)), "synthetic insertion after refusal");
        }
        s->presenter_condition.notify_all();
        until(s, [&] { return s->three_slots.can_accept_real(); });
        {
            std::scoped_lock lock(s->presenter_mutex);
            check(XR_SUCCEEDED(s->presenter_failure) &&
                  s->presenter_inflight_frame != rejected->frame,
                "owned content refusal poisoned director/retained frame");
            s->director_draining = true;
        }
        s->presenter_condition.notify_all();
        until(s, [&] { return s->three_slots.empty(); });
        stop_continuous_presenter(s);
    } catch (...) {
        stop_continuous_presenter(s);
        refuse_next_end = XR_SUCCESS;
        throw;
    }
    std::cout << "Content refusal: four errors on legacy/director borrowed frames and owned-slot recovery\n";
}

#include "depth_history_tests.inc"
#include "vulkan_bridge_contract_tests.inc"
#include "djules_binding_queue_tests.inc"
#include "djules_clock_tests.inc"
#include "native_prediction_clock_tests.inc"
#include "luke_route_prediction_clock_tests.inc"
#include "native_dx11_guide_session_tests.inc"
#include "fsr_release_identity_tests.inc"
#include "interface_cycle_layer_tests.inc"
#include "elliott_controller_layer_tests.inc"
#include "blackbar_timing_layer_tests.inc"
#include "djules_v474_layer_tests.inc"

#include "d3d12_reported_device_alias.hpp"
#include "luke_stale_queue_fixture.hpp"
#include "v477_luke_context.inc"
#include "native_reset_lifetime_test.inc"
#include "../src/layer/outlaws_startup_test.inc"

#include "luke_fresh_pacing_fixture.inc"
#include "luke_clock_continuity_fixture.inc"
#include "luke_aux_budget_fixture.inc"
#include "luke_aux_private_fixture.inc"
#include "luke_ngx_startup_fixture.inc"
void luke_queue_policy_test() {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&device,nullptr,&context)),"queue policy D11 owner");
    for (bool steamvr : {true,false}) for (bool legacy : {true,false}) for (bool only : {true,false}) {
        auto state=session();state->luke_afw_enabled=true;state->afw_only=only;
        state->graphics_binding=SessionGraphicsBinding::d3d11;
        state->dispatch->steamvr_runtime=steamvr;state->dispatch->virtual_desktop_runtime=!steamvr;
        state->vdxr_legacy=legacy;state->graphics_binding_capabilities=8;
        check(SUCCEEDED(xrfg::create_d3d12_device_for_d3d11(device.Get(),&state->d3d12_device,
            &state->d3d12_queue)),"queue policy capture lane");
        check(session_uses_shared_queue(*state)==!only,"actual unsplit queue state");
        state->graphics_binding_capabilities=4; // real loading-screen order: no native SR/RR yet
        initialize_luke_synthesis_queue(*state);
        check(bool(state->luke_synthesis_queue)==!only,
            "OFXR must isolate before AFW provenance becomes available");
        auto cold_queue=state->luke_synthesis_queue;
        state->graphics_binding_capabilities=8;initialize_luke_synthesis_queue(*state);
        check(state->luke_synthesis_queue==cold_queue,
            "native AFW confirmation must preserve the already-isolated OFXR lane");
        check(bool(state->luke_synthesis_queue)==!only && !session_uses_shared_queue(*state),
            "SteamVR/VDXR/Legacy isolate OFXR; AFW-only allocates no OFXR lane");
        auto before=state->luke_synthesis_queue;initialize_luke_synthesis_queue(*state);
        check(before==state->luke_synthesis_queue,"queue remains stable across pacing switches");
        if(!only) {
            auto fallback=std::make_shared<FrameGenerationSwapchainState>();
            fallback->shared_queue_owner=state;++state->luke_shared_generation_count;
            check(session_uses_shared_queue(*state),"one shared eye must warn even with a dedicated session lane");
            fallback.reset();check(!session_uses_shared_queue(*state),"retiring last shared eye clears warning");
        }
        state->dispatch->steamvr_runtime=true;state->luke_afw_enabled=false;
        state->graphics_binding=SessionGraphicsBinding::d3d12;
        check(session_uses_shared_queue(*state)==!only,"native D12 failed synthesis allocation warns");
        state->d3d12_synthesis_queue=state->d3d12_queue;
        check(session_uses_shared_queue(*state)==!only,"a shared queue alias still warns");
        state->d3d12_synthesis_queue=state->luke_synthesis_queue;
        check(!session_uses_shared_queue(*state),"separate native synthesis queue clears warning");
        state->d3d12_synthesis_queue.Reset();
    }
    std::cout<<"Queue policy: SteamVR/VDXR, legacy/comparison, AFW-only, per-eye fallback lifetime PASS\n";
}
void gta_flat_scene_test() {
    auto session=std::make_shared<SessionState>(std::make_shared<Dispatch>());
    session->luke_afw_enabled=true;
    session->luke_afw=std::make_shared<xrfg::LukeAfwSynthesizer>();
    session->graphics_binding_capabilities=8;
    XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create.width=1024;create.height=128;create.arraySize=create.sampleCount=1;
    auto chain=std::make_shared<SwapchainState>(session,create);
    auto other=std::make_shared<SwapchainState>(session,create);
    const auto handle=reinterpret_cast<XrSwapchain>(std::uintptr_t(0x69201));
    const auto other_handle=reinterpret_cast<XrSwapchain>(std::uintptr_t(0x69202));
    {std::scoped_lock lock(g_state_mutex);g_swapchains[handle]=chain;g_swapchains[other_handle]=other;}
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.eyeVisibility=XR_EYE_VISIBILITY_BOTH;quad.subImage.swapchain=handle;
    quad.subImage.imageRect.extent={1024,128};
    XrCompositionLayerQuad second=quad;second.subImage.swapchain=other_handle;
    const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&second)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.layerCount=1;end.layers=layers;
    std::vector<PostAfwFrameInput> completed;
    const auto prepare=[&] {return prepare_luke_afw_frame(session,{}, {},std::nullopt,1,false,completed,&end);};
    const auto flat=prepare();
    check(flat.kind==PreparedGenerationKind::none&&flat.reason==GenerationPrepareReason::luke_flat_scene,
        "full application flat quad must bypass AFW on absent stereo");
    check(!flat.original_scene&&completed.empty(),"flat quad must not invent stereo FPS identity");
    quad.eyeVisibility=XR_EYE_VISIBILITY_LEFT;
    check(prepare().reason==GenerationPrepareReason::missing_generation,"single-eye overlay is not a flat scene");
    quad.eyeVisibility=XR_EYE_VISIBILITY_BOTH;quad.subImage.imageRect.extent.width=1023;
    check(prepare().reason==GenerationPrepareReason::missing_generation,"partial overlay is not a full scene");
    quad.subImage.imageRect.extent.width=1024;
    const auto before=xrfg::embedded::afw_snapshot();
    xrfg::embedded::report_afw_prepare(static_cast<std::int64_t>(prepare().reason));
    const auto suspended=xrfg::embedded::afw_snapshot();
    check(suspended.flat_scene&&suspended.failed_frames==before.failed_frames,"flat scene is a suspension, not failed AFW");
    end.layerCount=0;
    check(prepare().reason==GenerationPrepareReason::missing_generation,"removed quad must not remain latched");
    xrfg::embedded::report_afw_prepare(0);
    const auto resumed=xrfg::embedded::afw_snapshot();
    check(!resumed.flat_scene&&resumed.ready_frames==suspended.ready_frames+1,"valid stereo resumes ordinary status");
    {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(handle);g_swapchains.erase(other_handle);}
    std::cout<<"GTA flat scene: full application BOTH-eye quad despite erased CRC, no staging read or fake FPS; normal status resumes PASS\n";
}
#include "quarantine_history_recovery_test.inc"
int main(int argc, char** argv) {
    if(argc>1 && std::string_view(argv[1])=="quarantine-history-recovery") {
        try {quarantine_history_recovery_test();return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="gta-flat-scene") {
        try {xrfg_fixture::ControlProfile profile;gta_flat_scene_test();return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="luke-queue-policy") {
        try {luke_queue_policy_test();return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    }
    if(argc>1 && std::string_view(argv[1]).starts_with("luke-aux-private")) {
        try {luke_aux_private_test(std::string_view(argv[1])!="luke-aux-private-vdxr",
            std::string_view(argv[1])!="luke-aux-private-afw-off");return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="luke-ngx-startup") {
        try {luke_ngx_startup_test();return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="luke-aux-budget-handoff") {
        try {xrfg_fixture::ControlProfile profile;pipelined_route_handoff_test(true,true);return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="luke-aux-budget") {
        try {luke_aux_budget_test();return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    xrfg_fixture::ControlProfile autonomous_profile;
    if(argc>1 && std::string_view(argv[1])=="native-reset-lifetime") {
        try {native_reset_lifetime_test();return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc>3 && std::string_view(argv[1])=="outlaws-startup") {
        const std::string marker=argv[2],provider=argv[3];
        const std::wstring wide_marker(marker.begin(),marker.end());
        const std::wstring wide_provider(provider.begin(),provider.end());
        try {outlaws_startup_test(wide_marker.c_str(),wide_provider.c_str());return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="luke-startup-pacing") {
        try {luke_fresh_pacing_test(true);return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="luke-clock-continuity") {
        try {luke_clock_continuity_test();return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="luke-fresh-pacing") {
        try {luke_fresh_pacing_test();return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="luke-stale-synthesis-context") {
        try {luke_standalone_context_test(false,false,true);return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="luke-no-helper-context") {
        try {luke_standalone_context_test(false,true);return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="luke-standalone-context") {
        try {luke_standalone_context_test();return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="luke-late-alias-context") {
        try {luke_standalone_context_test(true);return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="luke-afw-resource-lifecycle") {
        try {luke_standalone_context_test(true,false,false,true);return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="v474-vd") {
        try {djules_v474_vd_restore_test();return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="v474-policy") {
        try {djules_v474_policy_lifecycle_test();return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="vulkan-deferred-release") {
        try {vulkan_deferred_release_test();return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="v474-bridge") {
        try {djules_v474_bridge_test();return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc>1 && std::string_view(argv[1])=="blackbar-timing") {
        try {blackbar_timing_layer_test();return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if (argc>1 && std::string_view(argv[1])=="elliott-controller") {
        try { elliott_controller_layer_test();return 0; }
        catch(const std::exception& e) { std::cerr<<e.what()<<"\n";return 1; }
    }
    if (argc>1 && std::string_view(argv[1])=="cycle-metrics") {
        try { interface_cycle_layer_test();return 0; }
        catch(const std::exception& e) { std::cerr<<e.what()<<"\n";return 1; }
    }
    if (argc>1 && std::string_view(argv[1])=="fsr-release-identity") {
        try { fsr_release_identity_test();return 0; }
        catch(const std::exception& e) { std::cerr<<e.what()<<"\n";return 1; }
    }
    {
        ProjectionSnapshot a{},b{}; a.layers.resize(1);b.layers.resize(1);
        a.layers[0].views.resize(2);b.layers[0].views.resize(2);
        for(unsigned eye=0;eye<2;++eye) {
            a.layers[0].views[eye].pose.orientation.w=1;
            b.layers[0].views[eye].pose={{0,std::sin(.1F),0,std::cos(.1F)},{.1F,0,0}};
        }
        const auto camera=synthetic_camera_snapshot(a,b,.5F);
        check(camera && std::abs(camera->layers[0].views[0].pose.position.x-.05F)<1e-5F,"R19 midpoint camera");
        XrCompositionLayerProjectionView views[2]{};
        auto swap=reinterpret_cast<XrSwapchain>(0x7510);
        for(unsigned eye=0;eye<2;++eye) { views[eye].type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;views[eye].pose=b.layers[0].views[eye].pose;views[eye].subImage.swapchain=swap; }
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.viewCount=2;projection.views=views;
        auto header=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};info.layerCount=1;info.layers=&header;
        std::array destinations{ProjectionResourceDestination{swap,swap,false}};
        GeneratedFrameEndInfo off,on,skipped;
        check(build_generated_frame_end_info(&info,b,true,destinations,&off,&*camera),"R19 off metadata");
        check(off.projections[0].views[0].pose.position.x==.1F,"R19 off must keep B metadata");
        destinations[0].synthetic_camera=true;
        check(build_generated_frame_end_info(&info,b,true,destinations,&on,&*camera),"R19 on metadata");
        check(on.projections[0].views[0].pose.position.x==.05F,"R19 metadata must follow generated camera");
        destinations[0].synthetic_camera=false;
        check(build_generated_frame_end_info(&info,b,true,destinations,&skipped,&*camera) && skipped.projections[0].views[0].pose.position.x==.1F,"Skipped native pair keeps B");
        SynthesisJob frozen;frozen.synthetic_camera=camera;
        xrfg::autonomous::r19_camera_enabled=false;
        check(frozen.synthetic_camera->layers[0].views[0].pose.position.x==.05F,"R19 queued camera remains immutable");
        b.layers[0].views[1].pose.position.x=1;
        check(!synthetic_camera_snapshot(a,b,.5F),"R19 teleport rejects entire stereo camera");
        b.layers[0].views[1].pose.position.x=.1F;
        b.layers[0].views[1].pose.orientation={0,std::sin(.6F),0,std::cos(.6F)};
        check(!synthetic_camera_snapshot(a,b,.5F),"R19 recentre rejects entire stereo camera");
    }
    if(argc>3 && (std::string_view(argv[1])=="horizon-steamvr-capture"||std::string_view(argv[1])=="horizon-vdxr-capture")) {
        std::filesystem::path wr(argv[2]),ws(argv[3]);
        return xrfg::run_horizon_ngx_capture_test(wr.c_str(),ws.c_str(),std::string_view(argv[1])=="horizon-steamvr-capture");
    }
    if(argc>3 && (std::string_view(argv[1])=="ghost-steamvr-capture"||std::string_view(argv[1])=="ghost-vdxr-capture")) {
        const std::string realvr=argv[2],streamline=argv[3];
        const std::wstring wr(realvr.begin(),realvr.end()),ws(streamline.begin(),streamline.end());
        return xrfg::run_ghost_ngx_capture_test(wr.c_str(),ws.c_str(),std::string_view(argv[1])=="ghost-steamvr-capture");
    }
    if(argc>3 && (std::string_view(argv[1])=="luke-ngx-actual-capture" || std::string_view(argv[1])=="crimson-ngx-actual-capture" || std::string_view(argv[1])=="dead-space-ngx-actual-capture" || std::string_view(argv[1])=="dead-space-ingress-diagnostics" || std::string_view(argv[1])=="gta-enhanced-ngx-actual-capture")) {
        const std::string realvr=argv[2],streamline=argv[3];
        const std::wstring wide_realvr(realvr.begin(),realvr.end());
        const std::wstring wide_streamline(streamline.begin(),streamline.end());
        if(std::string_view(argv[1])=="gta-enhanced-ngx-actual-capture")
            return xrfg::run_gta_enhanced_ngx_capture_test(wide_realvr.c_str(),wide_streamline.c_str());
        if(std::string_view(argv[1])=="dead-space-ingress-diagnostics")
            return xrfg::run_dead_space_ingress_diagnostics_test(wide_realvr.c_str(),wide_streamline.c_str());
        if(std::string_view(argv[1])=="dead-space-ngx-actual-capture")
            return xrfg::run_dead_space_ngx_capture_test(wide_realvr.c_str(),wide_streamline.c_str());
        return std::string_view(argv[1])=="crimson-ngx-actual-capture" ?
            xrfg::run_crimson_ngx_capture_test(wide_realvr.c_str(),wide_streamline.c_str()) :
            xrfg::run_actual_ngx_capture_test(wide_realvr.c_str(),wide_streamline.c_str());
    }
    if(argc>3 && (std::string_view(argv[1])=="unlisted-motion-swap-capture" ||
        std::string_view(argv[1])=="unlisted-motion-original-capture")) {
        const std::string realvr=argv[2],streamline=argv[3];
        const std::wstring wide_realvr(realvr.begin(),realvr.end());
        const std::wstring wide_streamline(streamline.begin(),streamline.end());
        return xrfg::run_unlisted_motion_capture_test(wide_realvr.c_str(),wide_streamline.c_str(),
            std::string_view(argv[1])=="unlisted-motion-swap-capture");
    }
    if(argc>3 && std::string_view(argv[1])=="kcd-ngx-logical-capture") {
        const std::string realvr=argv[2],streamline=argv[3];
        const std::wstring wide_realvr(realvr.begin(),realvr.end());
        const std::wstring wide_streamline(streamline.begin(),streamline.end());
        return xrfg::run_kcd_logical_capture_test(wide_realvr.c_str(),wide_streamline.c_str());
    }
    if(argc>3 && std::string_view(argv[1])=="kcd-ngx-scene-capture") {
        const std::string realvr=argv[2],streamline=argv[3];
        const std::wstring wide_realvr(realvr.begin(),realvr.end());
        const std::wstring wide_streamline(streamline.begin(),streamline.end());
        return xrfg::run_kcd_scene_capture_test(wide_realvr.c_str(),wide_streamline.c_str(),
            &select_luke_execution_queue);
    }
    if(argc>3 && std::string_view(argv[1])=="luke-ngx-private-capture") {
        const std::string realvr=argv[2],streamline=argv[3];
        const std::wstring wide_realvr(realvr.begin(),realvr.end());
        const std::wstring wide_streamline(streamline.begin(),streamline.end());
        return xrfg::run_luke_private_capture_test(wide_realvr.c_str(),wide_streamline.c_str(),
            &select_luke_execution_queue);
    }
    if(argc>3 && std::string_view(argv[1])=="outlaws-ngx-logical-capture") {
        const std::string realvr=argv[2],streamline=argv[3];
        const std::wstring wide_realvr(realvr.begin(),realvr.end());
        const std::wstring wide_streamline(streamline.begin(),streamline.end());
        return xrfg::run_outlaws_logical_capture_test(wide_realvr.c_str(),wide_streamline.c_str(),
            &select_luke_execution_queue);
    }
    if (argc>2 && (std::string_view(argv[1])=="luke-ngx-dispatch" ||
                  std::string_view(argv[1])=="kcd-engine-dispatch")) {
        const std::string path=argv[2];const std::wstring wide(path.begin(),path.end());
        return xrfg::run_luke_ngx_dispatch_test(wide.c_str(),std::string_view(argv[1])=="kcd-engine-dispatch");
    }
    if (argc>1 && std::string_view(argv[1])=="native-vulkan-transport")
        return xrfg::native_vulkan::run_buffer_transport_test();
    if (argc>1 && std::string_view(argv[1])=="native-vulkan-bootstrap")
        return xrfg::native_vulkan::run_buffer_transport_test(true);
#ifdef OFXR_TEST_DJULES_ROUTE
    {
        auto timing=std::make_shared<SessionState>(std::make_shared<Dispatch>());
        timing->graphics_binding=SessionGraphicsBinding::d3d12;
        timing->nvidia_new_route=true; timing->djules_route=kTestDjules; timing->menu_enabled=true;
        timing->optical_flow_backend=xrfg::D3D12OpticalFlowBackend::nvidia;
        timing->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;
        timing->minimum_runtime_display_period=10;
        ProjectionSnapshot a{},b{};a.display_time=100;b.display_time=140;
        check(std::abs(synthetic_interpolation_fraction(timing,a,b)-0.75F)<0.0001F,"DJ 2X interval fraction");
        timing->djules_triple_requested=true;
        check(std::abs(synthetic_interpolation_fraction(timing,a,b)-0.5F)<0.0001F,"DJ 3X first fraction");
        check(std::abs(synthetic_interpolation_fraction(timing,a,b,1)-0.75F)<0.0001F,"DJ 3X second fraction");
        b.display_time=120;
        check(std::abs(synthetic_interpolation_fraction(timing,a,b)-1.0F/3.0F)<0.0001F,"invalid cadence fallback");
        timing->nvidia_new_route=false;
        check(synthetic_interpolation_fraction(timing,a,b)==0.5F,"reference fraction changed");
        timing->nvidia_new_route=true; timing->djules_route=kTestDjules; timing->three_slot_director=true;
        check(XR_SUCCEEDED(switch_djules_pipeline(timing,false,false)),"switch 2X");
        check(!timing->djules_triple_requested && timing->three_slots.empty(),"2X shape reset");
        check(XR_SUCCEEDED(switch_djules_pipeline(timing,true,true)),"switch 3X");
        check(timing->djules_triple_requested && timing->three_slots.empty(),"3X shape reset");
        xrfg::DjulesTripleRelease release;
        const auto window=[&](unsigned late) {
            for(unsigned i=0;i<30;++i) release.observe(i>=late,std::chrono::milliseconds(10));
        };
        window(30);
        check(release.delay.count()==0 && release.ceiling==std::chrono::milliseconds(20),"startup clamped ceiling");
        for(unsigned i=0;i<5;++i) window(0);
        check(release.delay==std::chrono::milliseconds(10),"clean-window raise");
        window(2);
        check(release.delay==std::chrono::milliseconds(7) && release.hold==10,"late-window retreat");
        for(unsigned i=0;i<10;++i) window(0);
        check(release.delay==std::chrono::milliseconds(7),"ceiling hold");
        window(0);
        check(release.delay==std::chrono::milliseconds(9),"ceiling recovery");
        timing->djules_triple_release=release;
        check(XR_SUCCEEDED(switch_djules_pipeline(timing,false,false)),"controller switch");
        check(timing->djules_triple_release.delay.count()==0,"controller survived shape switch");
    }
#endif
    auto state = session();
    try {
        native_stereo_time_match_test();
        afw_producer_release_independence_test();
        check(three_slot_generation_session(state), "Nvidia (new) must select director");
        state->nvidia_new_route = false;
        check(!three_slot_generation_session(state), "Nvidia legacy must exclude director");
        state->nvidia_new_route = true; state->djules_route=kTestDjules;
        state->frame_generation_mode = xrfg::D3D12FrameGenerationMode::fsr3;
        check(!three_slot_generation_session(state), "FSR must remain inline");
        state->frame_generation_mode = xrfg::D3D12FrameGenerationMode::ofxr;
        state->optical_flow_backend = xrfg::D3D12OpticalFlowBackend::fidelity_fx;
        check(!three_slot_generation_session(state), "FidelityFX must exclude NVIDIA director");
        state->optical_flow_backend = xrfg::D3D12OpticalFlowBackend::nvidia;
        state->luke_afw_enabled = true;
        check(three_slot_generation_session(state),
              "Luke AFW must enter the new director after its completed packet");
        state->nvidia_new_route = false;
        check(!three_slot_generation_session(state), "Luke legacy must stay inline");
        check(!queued_nvidia_session(state),
              "Luke AFW must not enter the NVIDIA queued presenter");
        state->nvidia_new_route = true; state->djules_route=kTestDjules;
        state->luke_afw_enabled = false;
        if (argc > 1) {
            const auto mode = std::string_view(argv[1]);
            if (mode == "native-dx11-guides") { native_dx11_guide_session_test(); return 0; }
            if (mode == "native-prediction-clock") { native_prediction_clock_test(); return 0; }
            if (mode == "luke-route-prediction-clock") { luke_route_prediction_clock_test(); return 0; }
            if (mode == "route-handoff") { pipelined_route_handoff_test(); return 0; }
            if (mode == "route-handoff-steamvr") { pipelined_route_handoff_test(true); return 0; }
            if (mode == "pipeline-preference") { djules_pipeline_preference_test(); return 0; }
            if (mode == "pairing-recovery") { own_time_pairing_recovery_test(); return 0; }
            if (mode == "unified-routes") { unified_route_policy_test(); return 0; }
            if (mode == "vulkan-contract") { vulkan_bridge_contract_test(); return 0; }
#ifdef OFXR_TEST_DJULES_ROUTE
            if (mode == "binding-queue") { djules_binding_queue_test(); return 0; }
            if (mode == "display-clock") { djules_display_clock_test(); return 0; }
            if (mode == "absolute-grid") { djules_absolute_grid_test(); return 0; }
            if (mode == "triple-buffer") { djules_triple_buffer_test(); return 0; }
            if (mode == "deep-readiness") { djules_readiness_test(); return 0; }
            if (mode == "inline-policy") { djules_inline_policy_test(); return 0; }
            if (mode == "phase-acquisition") { djules_phase_acquisition_test(); return 0; }
#endif
            if(mode == "depth-history") { depth_history_test(); return 0; }
            if(mode == "luke-afw-slot") { gpu_pipeline(true, false, false, false, false, true); return 0; }
            if(mode == "post-afw-packet") { gpu_pipeline(true, false, false, false, false, false, true); return 0; }
            if(mode == "luke-isolated-post-afw") { gpu_pipeline(true,false,false,false,false,false,true,false,0,true); return 0; }

            if(mode == "luke-isolated-raw") { gpu_pipeline(true,false,false,true,false,false,false,false,0,true); return 0; }
            if(mode == "luke-missing-afw-guides") { gpu_pipeline(true, false, false, false, false, false, false, true); return 0; }
            if(mode == "luke-markerless-ofxr") { gpu_pipeline(true,false,false,false,false,false,false,false,6); return 0; }
            if(mode == "luke-private-inline-on") {gpu_pipeline(true,false,false,false,false,false,false,false,1);return 0;}
            if(mode == "luke-fidelity-inline-busy") {gpu_pipeline(false,false,false,false,false,false,false,false,5);return 0;}
            if(mode == "luke-private-inline-off") {gpu_pipeline(true,false,false,false,false,false,false,false,2);return 0;}
            if(mode == "luke-private-worker") {gpu_pipeline(true,false,false,false,false,false,false,false,3);return 0;}
            if(mode == "luke-private-post-afw") {gpu_pipeline(true,false,false,false,false,false,true,false,4);return 0;}
            if(mode == "luke-inline-diagnostic") { luke_inline_diagnostic_test(); return 0; }
            if(mode == "luke-unit") { luke_admission_test(); luke_same_time_baseline_test(); return 0; }
            if(mode == "luke-clock") { luke_cycle_clock_test(); return 0; }
            if(mode == "luke-gpu") { gpu_pipeline(true,false,false,true); return 0; }
            if(mode == "gap") { gpu_pipeline(true,false,false,false,true); return 0; }
            if(mode == "luke-ram") {
                RamTimingFixture capture(true);gpu_pipeline(true,false,false,true);capture.verify(true);return 0;
            }
            if(mode == "ram") {
                RamTimingFixture capture(true);
                gpu_pipeline(false,false,false);capture.verify(true);return 0;
            }
            gpu_pipeline(mode == "composition" || mode == "transition" || mode == "capture", mode == "transition", mode == "capture"); return 0;
        }
        std::cerr<<"Director fixture: explicit eligibility\n"<<std::flush;
        embedded_eligibility_test();
        // Apply both menu revisions to the real session implementation. The
        // GPU backend stays NVIDIA: only presenter ownership is switched.
        std::cerr<<"Director fixture: control route revisions\n"<<std::flush;
        {
            auto selected = session();
            selected->presenter_active = false;
            const auto original = xrfg::embedded::snapshot().desired;
            auto choice = original;
            choice.backend = 1;
            choice.frame_generation = 0;
            check(xrfg_fixture::request_control(choice), "request Nvidia legacy");
            apply_embedded_control(selected);
            check(!three_slot_generation_session(selected), "legacy menu revision applied");
            choice.backend = kTestDjules ? 3 : 2;
            check(xrfg_fixture::request_control(choice), "request Nvidia new");
            apply_embedded_control(selected);
            check(three_slot_generation_session(selected), "new menu revision applied");
            choice.backend = 1;
            check(xrfg_fixture::request_control(choice), "request legacy again");
            apply_embedded_control(selected);
            check(!three_slot_generation_session(selected), "reverse menu revision applied");
            check(xrfg_fixture::request_control(original), "restore test configuration");
        }
        // Virtual WaitFrame must return even while synthesis is deliberately
        // marked busy and no director is running. No tick/fence gate survives.
        std::cerr<<"Director fixture: virtual prediction\n"<<std::flush;
        { std::scoped_lock lock(g_state_mutex); g_sessions[state->handle] = state; }
        state->synthesis_busy = true;
        XrFrameState prediction{XR_TYPE_FRAME_STATE};
        const auto start = std::chrono::steady_clock::now();
        check(XR_SUCCEEDED(layer_wait_frame_impl(state->handle, nullptr, &prediction)), "virtual wait");
        check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(50), "producer paced");
        check(waits == 0, "producer entered runtime");
        check(prediction.predictedDisplayPeriod == 2000000, "director advertised doubled period");
        check(prediction.predictedDisplayTime == next_time + 4000000, "prediction anchor changed");
        // Exercise the real WaitFrame hook at several headset rates. Only
        // advertised period changes: the two-period anchor/ceiling survives.
        for (const XrDuration period : {8333333LL, 11111111LL, 13888889LL}) {
            state->presenter_frame_state.predictedDisplayPeriod = period;
            state->last_virtual_display_time = 0;
            state->application_wait_pending_begin = false;
            check(XR_SUCCEEDED(layer_wait_frame_impl(state->handle, nullptr, &prediction)), "native-period wait");
            check(prediction.predictedDisplayPeriod == period, "headset period not preserved");
            check(prediction.predictedDisplayTime == next_time + 2*period, "pair lookahead changed");
            state->application_wait_pending_begin = false;
            check(XR_SUCCEEDED(layer_wait_frame_impl(state->handle, nullptr, &prediction)), "second virtual wait");
            check(prediction.predictedDisplayPeriod == period, "second period changed");
            check(prediction.predictedDisplayTime == next_time + 4*period, "prediction step changed");
        }
        state->three_slot_director = false;
        state->last_virtual_display_time = 0;
        state->application_wait_pending_begin = false;
        check(XR_SUCCEEDED(layer_wait_frame_impl(state->handle, nullptr, &prediction)), "other presenter wait");
        check(prediction.predictedDisplayPeriod == 2*13888889LL, "non-director route changed");
        state->three_slot_director = true;
        state->menu_enabled = false;
        state->last_virtual_display_time = 0;
        state->application_wait_pending_begin = false;
        check(XR_SUCCEEDED(layer_wait_frame_impl(state->handle, nullptr, &prediction)), "disabled wait");
        check(prediction.predictedDisplayPeriod == 13888889LL, "disabled period changed");
        check(prediction.predictedDisplayTime == next_time + 13888889LL, "disabled anchor changed");
        state->menu_enabled = true;
        state->presenter_frame_state.predictedDisplayPeriod = 2000000;
        state->last_virtual_display_time = 0;
        check(waits == 0, "metadata test entered runtime");
        state->synthesis_busy = false;
        state->application_wait_pending_begin = false;
        { std::scoped_lock lock(g_state_mutex); g_sessions.clear(); }

        // Real director, mock pixels. Verify downstream blocking keeps A's
        // slot owned and prevents an early synthetic from overwriting it.
        std::cerr<<"Director fixture: actual consumer 49 cycles\n"<<std::flush;
        RegisteredFixtureSession director_registration(state);
        state->three_slots.push_real(frame(1));
        state->three_slots.shift_empty(); state->three_slots.shift_empty();
        state->presenter_thread = std::thread(three_slot_director_main, state);
        for (unsigned n = 2; n <= 25; ++n) {
            std::cerr<<"Director fixture: admit real "<<n<<'\n'<<std::flush;
            until(state, [&] { return state->three_slots.can_accept_real(); });
            {
                std::scoped_lock lock(state->presenter_mutex);
                blocked_end = (n % 3 == 0); entered_end = false;
                check(state->three_slots.push_real(frame(n)), "real admission");
            }
            state->presenter_condition.notify_all();
            if (blocked_end) {
                std::unique_lock lock(test_mutex);
                check(test_condition.wait_for(lock, std::chrono::seconds(3), [] { return entered_end.load(); }), "end enter");
                lock.unlock();
                { std::scoped_lock state_lock(state->presenter_mutex);
                  check(!state->three_slots.can_insert_synthetic(), "inflight overwrite"); }
                blocked_end = false; test_condition.notify_all();
            }
            until(state, [&] { return state->three_slots.can_insert_synthetic(); });
            // Model delayed completion without changing the director's code.
            if (n % 4 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            { std::scoped_lock lock(state->presenter_mutex);
              check(state->three_slots.push_synthetic(frame(n, true)), "synthetic insertion"); }
            state->presenter_condition.notify_all();
        }
        until(state, [&] { return state->three_slots.can_accept_real(); });
        check(XR_SUCCEEDED(wait_for_presenter_idle(state)), "drain");
        stop_continuous_presenter(state);
        check(presented.size() == 49, "lost or repeated frame");
        for (unsigned i = 0; i < 24; ++i) {
            check(presented[2*i] == std::pair<unsigned,bool>{i+1,false}, "real sequence");
            check(presented[2*i+1] == std::pair<unsigned,bool>{i+2,true}, "synthetic sequence");
        }
        check(presented.back() == std::pair<unsigned,bool>{25,false}, "last real drain");
        check(waits == begins && begins == ends && ends == 49, "frame cycle mismatch");

        std::cerr<<"Director fixture: refused output recovery\n"<<std::flush;
        refused_frame_recovery_test();

        auto failing = session();
        RegisteredFixtureSession failing_registration(failing);
        failing->three_slots.push_real(frame(1));
        failing->three_slots.shift_empty(); failing->three_slots.shift_empty();
        failing->three_slots.push_real(frame(2));
        fail_runtime = true;
        failing->presenter_thread = std::thread(three_slot_director_main, failing);
        until(failing, [&] { return XR_FAILED(failing->presenter_failure); });
        check(XR_FAILED(wait_for_synthesis_handoff(failing)), "runtime failure not propagated");
        stop_continuous_presenter(failing);
        fail_runtime = false;

        auto held = session();
        RegisteredFixtureSession held_registration(held);
        held->three_slots.push_real(frame(1));
        held->three_slots.shift_empty(); held->three_slots.shift_empty();
        const auto before_stop = ends.load();
        held->presenter_thread = std::thread(three_slot_director_main, held);
        stop_continuous_presenter(held);
        check(ends == before_stop, "stop repeated a retained frame");

        // The actual worker error path must wake both producer rendezvous and
        // director. A missing mapping cannot silently strand a busy worker.
        auto failed = session();
        RegisteredFixtureSession failed_registration(failed);
        auto job = std::make_shared<SynthesisJob>();
        job->current = frame(1); failed->synthesis_job = job; failed->synthesis_busy = true;
        failed->synthesis_thread = std::thread(synthesis_worker_main, failed);
        check(XR_FAILED(wait_for_synthesis_handoff(failed)), "worker error not propagated");
        stop_continuous_presenter(failed);
        std::cout << "Actual director: 49 legal ordered cycles, slow synthesis/runtime, drain, unpaced producer, worker failure\n";
        return 0;
    } catch (const std::exception& e) {
        blocked_end = false; test_condition.notify_all(); stop_continuous_presenter(state);
        std::cerr << e.what() << '\n'; return 1;
    }
}
