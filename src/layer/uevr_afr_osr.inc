// OSR for stock UEVR's Alternating/AFR rendering method (xrfg/uevr_afr_osr.hpp).
// Included by openxr_layer.cpp inside its anonymous namespace, before the
// exported entry points that call these hooks. Its headers are included at
// the top of openxr_layer.cpp, outside that namespace.
//
// UEVR, Alternating/AFR, default synchronisation (Very Late): on its present
// thread it waits and begins one OpenXR frame, renders the left eye and
// releases it into the left eye's swapchain, renders the right eye on the next
// game frame, releases it, and ends the frame. Here, on each eye's release:
//   1. the released image is copied while UEVR still owns it;
//   2. the other eye is reconstructed from it with Full OSR, from that frame's
//      DLSS depth and vectors and the other eye's own last frame, and written
//      into the other eye's swapchain through this layer's own acquire/release;
//   3. after the first eye of a pair, the frame UEVR began is ended here with
//      the two eyes as they now are, and a new one is waited and begun, so the
//      second eye gets a frame of its own. UEVR's xrEndFrame then ends that one.
// Each game frame thus reaches the runtime with one real and one
// reconstructed eye, and the runtime paces UEVR at one game frame per display
// slot, as Luke Ross's alternate-eye rendering is paced.

// ---- UEVR's public plugin API: the eye cameras and the VR settings ----

struct UevrCameraFeed {
    std::mutex mutex;
    xrfg::UevrEyePhase phase;
    xrfg::UevrEyeCameraQueue queue;
    std::uint64_t sequence{};
    std::uint64_t views{}, unknown_eye{};
    std::atomic_bool registered{};
    // UEVR's latest stage-space pose query; each view computed after it was
    // rendered with it.
    std::array<xrfg::UevrXrPose, 2> last_xr{};
    std::int64_t last_xr_time{};
    bool last_xr_valid{};
    // The last stage-space pose queries with their display times.
    struct Located { std::int64_t time{}; std::array<XrPosef, 2> poses{}; };
    std::array<Located, 8> located{};
    unsigned located_next{};
};
UevrCameraFeed& uevr_camera_feed() {
    static UevrCameraFeed feed;
    return feed;
}

thread_local std::array<double, 3> t_uevr_base_location{};
thread_local std::array<double, 3> t_uevr_base_rotation{};
thread_local bool t_uevr_base_valid{};

const UEVR_PluginInitializeParam* uevr_api() noexcept {
    const auto module = GetModuleHandleW(L"UEVRBackend.dll");
    if (!module) return nullptr;
    const auto* api = reinterpret_cast<const UEVR_PluginInitializeParam*>(
        GetProcAddress(module, "g_plugin_initialize_param"));
    if (!api || !api->version || api->version->major != 2) return nullptr;
    return api;
}

std::array<double, 3> uevr_location(const UEVR_Vector3f* position, bool is_double) noexcept {
    if (is_double) {
        const auto* d = reinterpret_cast<const UEVR_Vector3d*>(position);
        return {d->x, d->y, d->z};
    }
    return {position->x, position->y, position->z};
}

void uevr_pre_view_offset(UEVR_StereoRenderingDeviceHandle, int, float, UEVR_Vector3f* position,
    UEVR_Rotatorf* rotation, bool is_double) {
    if (!position || !rotation) return;
    t_uevr_base_location = uevr_location(position, is_double);
    if (is_double) {
        const auto* r = reinterpret_cast<const UEVR_Rotatord*>(rotation);
        t_uevr_base_rotation = {r->pitch, r->yaw, r->roll};
    } else {
        t_uevr_base_rotation = {rotation->pitch, rotation->yaw, rotation->roll};
    }
    t_uevr_base_valid = true;
}

void uevr_post_view_offset(UEVR_StereoRenderingDeviceHandle, int, float world_to_meters,
    UEVR_Vector3f* position, UEVR_Rotatorf* rotation, bool is_double) {
    if (!position || !rotation || !t_uevr_base_valid) return;
    t_uevr_base_valid = false;
    try {
        xrfg::UevrEyeCamera camera{};
        camera.location = uevr_location(position, is_double);
        camera.base_location = t_uevr_base_location;
        camera.base_rotation = t_uevr_base_rotation;
        if (is_double) {
            const auto* r = reinterpret_cast<const UEVR_Rotatord*>(rotation);
            camera.rotation = {r->pitch, r->yaw, r->roll};
        } else {
            camera.rotation = {rotation->pitch, rotation->yaw, rotation->roll};
        }
        camera.world_to_meters = world_to_meters;
        camera.ticks = GetTickCount64();
        for (const double v : camera.location) if (!std::isfinite(v)) return;
        for (const double v : camera.rotation) if (!std::isfinite(v)) return;
        auto& feed = uevr_camera_feed();
        std::scoped_lock lock(feed.mutex);
        camera.sequence = ++feed.sequence;
        camera.xr_eyes = feed.last_xr;
        camera.xr_time = feed.last_xr_time;
        camera.xr_valid = feed.last_xr_valid;
        ++feed.views;
        // Half the smallest plausible separation (50 mm at 1 unit per metre
        // is 0.05; UE titles use about 100 units per metre).
        const float min_separation = std::max(0.02F, world_to_meters * 0.02F);
        const auto eye = feed.phase.observe(camera, min_separation);
        feed.queue.push(camera, eye.value_or(2));
        if (!eye) ++feed.unknown_eye;
    } catch (...) {
    }
}

// Registers the two stereo view callbacks once. They are UEVR's documented
// plugin callbacks; registering from outside a plugin DLL is the same call.
bool uevr_register_camera_feed() noexcept {
    auto& feed = uevr_camera_feed();
    if (feed.registered.load(std::memory_order_acquire)) return true;
    const auto* api = uevr_api();
    if (!api || !api->sdk || !api->sdk->callbacks ||
        !api->sdk->callbacks->on_pre_calculate_stereo_view_offset ||
        !api->sdk->callbacks->on_post_calculate_stereo_view_offset) return false;
    bool expected = false;
    if (!feed.registered.compare_exchange_strong(expected, true)) return true;
    const bool pre = api->sdk->callbacks->on_pre_calculate_stereo_view_offset(&uevr_pre_view_offset);
    const bool post = api->sdk->callbacks->on_post_calculate_stereo_view_offset(&uevr_post_view_offset);
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
        (pre ? 1 : 0) | (post ? 2 : 0), 1405, api->version->minor, 0);
    return pre && post;
}

// Rounding bias, in display periods, when snapping a frame's predicted
// display time to the native slot grid (see uevr_afr_reconstruct): 0.4 made
// the snapped slot the frame's real one for 98.6% of 2489 frames (0.0: 95.3%,
// 0.5: 97.6%, 0.7: 55.7%; Dead Island 2, 2026-11-02).
constexpr double kUevrSlotSnapBias = 0.4;
// Largest turn of a pair's second frame onto its first frame's world; more is
// a cut or a teleport, not UEVR's per-frame head turn.
constexpr double kUevrPairTurnLimitDegrees = 10.0;

struct UevrModSettings {
    bool valid{};
    int rendering_method{-1};
    int synchronization{-1};
    bool ghosting_fix{};
    float world_scale{1.0F};
};

UevrModSettings uevr_mod_settings() noexcept {
    static std::mutex mutex;
    static UevrModSettings cached{};
    static ULONGLONG next{};
    std::scoped_lock lock(mutex);
    const auto now = GetTickCount64();
    if (now < next) return cached;
    next = now + 1000;
    const auto* api = uevr_api();
    UevrModSettings s{};
    if (api && api->vr && api->vr->get_mod_value) {
        const auto read = [&](const char* key) {
            char value[64]{};
            api->vr->get_mod_value(key, value, sizeof(value) - 1);
            return std::string(value);
        };
        try {
            const auto method = read("VR_RenderingMethod");
            const auto ghosting = read("VR_GhostingFix");
            const auto synchronization = read("VR_SynchronizationMode");
            const auto scale = read("VR_WorldScale");
            s.valid = !method.empty();
            s.rendering_method = method.empty() ? -1 : std::atoi(method.c_str());
            s.ghosting_fix = ghosting == "true" || ghosting == "1";
            s.synchronization = synchronization.empty() ? -1 : std::atoi(synchronization.c_str());
            const float parsed = scale.empty() ? 1.0F : static_cast<float>(std::atof(scale.c_str()));
            s.world_scale = std::isfinite(parsed) && parsed > 0.0F ? parsed : 1.0F;
        } catch (...) {
            s = {};
        }
    }
    cached = s;
    return cached;
}

// ---- Per session ----

// Marks this layer's own frame and swapchain calls for the hooks above.
struct UevrInsideScope {
    bool& inside;
    explicit UevrInsideScope(bool& flag) noexcept : inside(flag) { inside = true; }
    ~UevrInsideScope() { inside = false; }
};

struct UevrCachedLayer {
    XrStructureType type{};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerProjectionView> views;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
};

struct UevrAfrOsrState {
    std::mutex mutex;
    // UEVR's last xrEndFrame: the eye swapchains and every layer, so an
    // inserted frame can submit the same composition.
    bool layout{};
    std::array<XrSwapchain, 2> eye_swapchain{};
    std::size_t eye_layer{};
    XrSpace space{XR_NULL_HANDLE};
    std::array<XrFovf, 2> fov{};
    std::array<XrPosef, 2> submitted_pose{};
    std::array<XrRect2Di, 2> rect{};
    // The pose the last reconstructed source eye was rendered with: the
    // inserted frame carries it.
    std::array<XrPosef, 2> frame_pose{};
    bool frame_pose_valid{};
    XrTime frame_time{};  // the display time frame_pose was located for
    XrTime second_time{}; // the same for the pair's second frame
    // The pose the pair's second eye was rendered with: UEVR's own frame carries it.
    std::array<XrPosef, 2> second_pose{};
    bool second_pose_valid{};
    std::array<std::uint32_t, 2> image_width{}, image_height{};
    std::uint32_t off_reasons{0xffffffffu};
    XrEnvironmentBlendMode blend{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
    std::vector<UevrCachedLayer> layers;
    // UEVR's own pose query for the eyes it renders, from any thread.
    std::mutex pose_mutex;
    std::array<XrPosef, 2> located_pose{};
    bool located{};
    std::atomic<XrSpace> located_space{XR_NULL_HANDLE};
    // UEVR's display time from its own xrWaitFrame; and, once a frame was
    // inserted, the time of the frame UEVR's next xrEndFrame closes.
    XrTime application_display_time{};
    XrDuration display_period{};
    std::optional<XrTime> rewrite_display_time;
    unsigned eye_releases{};
    bool inside{}; // our own frame calls and swapchain cycles
    bool active{};
    std::uint64_t last_publication{};
    // OSR's guide for the real eye of the frame being released, bound once
    // UEVR's own release has bound the game's.
    std::shared_ptr<const xrfg::DlssMotionVectorSet> source_guide;
    unsigned last_physical{2}; // physical eye of the last reconstructed source
    unsigned reconstructed_eye{2}; // the eye whose frame was built before its release
    // UEVR's engine yaw per pair (held for the pair's second frame) and its
    // last step a stick turned; the head's and UEVR's added yaw at the pair's
    // first frame, to tell the two apart.
    double pair_yaw{}, pair_yaw_step{};
    bool pair_yaw_valid{};
    double pair_head_yaw{}, pair_added_yaw{};
    bool pair_head_valid{};
    // The pair's first frame: its camera and the head pose it was rendered
    // for, which the second frame must agree with.
    xrfg::UevrMatrix pair_view{};
    xrfg::UevrXrPose pair_pose{};
    std::int64_t pair_pose_time{};
    bool pair_view_valid{};
    // Source capture taken before UEVR's release.
    std::optional<unsigned> captured_eye;
    std::array<double, 3> origin{};
    bool origin_valid{};
    bool reset_pending{true};
    ULONGLONG reconstructed_at{};
    ULONGLONG next_report{};
    std::uint64_t reconstructions{}, refusals{}, inserted{};
    xrfg::UevrOsrRenderer renderer;
};

std::shared_ptr<UevrAfrOsrState> make_uevr_afr_osr_state() noexcept {
    try { return std::make_shared<UevrAfrOsrState>(); } catch (...) { return {}; }
}

UevrAfrOsrState* uevr_afr_state(SessionState& session) { return session.uevr_afr.get(); }

bool uevr_afr_osr_overlay_active(const SessionState& session) noexcept {
    const auto* afr = session.uevr_afr.get();
    return afr && afr->active && afr->reconstructed_at && GetTickCount64() - afr->reconstructed_at < 500;
}

bool uevr_afr_osr_wanted(const SessionState& session) noexcept {
    if (!session.uevr_afr || session.luke_afw_enabled ||
        session.graphics_binding != SessionGraphicsBinding::d3d12 ||
        session.d3d11_bridge || session.vulkan_bridge || !session.d3d12_device || !session.d3d12_queue) return false;
    const auto afw = xrfg::embedded::afw_snapshot().desired;
    return afw.mode != 0 && afw.osr == static_cast<int>(xrfg::OsrReconstruction::full);
}

// UEVR's xrEndFrame as it must reach the layer: owned copies of what changed.
struct UevrEndFrameRewrite {
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    std::deque<XrCompositionLayerProjection> projections;
    std::deque<std::vector<XrCompositionLayerProjectionView>> views;
};

float uevr_angle_degrees(const XrQuaternionf& a, const XrQuaternionf& b) noexcept {
    const float dot = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.0F * std::acos(std::min(1.0F, dot)) * 57.29578F;
}

// Called with UEVR's xrEndFrame before the layer handles it. Records the
// composition and decides whether OSR runs. When a frame was inserted for the
// pair's first eye, this frame carries the display time of the frame begun for
// it; when its second eye was reconstructed, both views carry the pose that eye
// was rendered with (UEVR submits the pose of its submit state, which need not
// be that one). Returns true when out replaces UEVR's frame.
bool uevr_afr_observe_end_frame(SessionState& session, const XrFrameEndInfo* info, UevrEndFrameRewrite& out) {
    auto* afr = uevr_afr_state(session);
    if (!afr) return false;
    std::scoped_lock lock(afr->mutex);
    if (afr->inside) return false;
    const auto rewrite = afr->rewrite_display_time;
    afr->rewrite_display_time.reset();
    afr->eye_releases = 0;
    const auto second_pose = afr->second_pose_valid ? std::optional<std::array<XrPosef, 2>>(afr->second_pose) : std::nullopt;
    const XrTime second_time = afr->second_pose_valid ? afr->second_time : 0;
    afr->second_pose_valid = false;
    bool layout = false;
    std::array<XrSwapchain, 2> eyes{};
    std::vector<UevrCachedLayer> layers;
    std::size_t eye_layer{};
    XrSpace space{XR_NULL_HANDLE};
    std::array<XrFovf, 2> fov{};
    std::array<XrPosef, 2> poses{};
    std::array<XrRect2Di, 2> rects{};
    std::array<std::uint32_t, 2> widths{}, heights{};
    if (info && info->type == XR_TYPE_FRAME_END_INFO && info->layers) {
        try {
            for (std::uint32_t i = 0; i < info->layerCount; ++i) {
                const auto* base = info->layers[i];
                if (!base) continue;
                UevrCachedLayer cached{};
                cached.type = base->type;
                if (base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    const auto& p = *reinterpret_cast<const XrCompositionLayerProjection*>(base);
                    if (!p.views || !p.viewCount) continue;
                    cached.projection = p;
                    cached.projection.next = nullptr;
                    cached.views.assign(p.views, p.views + p.viewCount);
                    for (auto& view : cached.views) view.next = nullptr; // depth belongs to UEVR's frame only
                    // UEVR's AFR projection: two eye swapchains. Each eye is a
                    // rectangle of its image (UEVR crops the asymmetric field).
                    if (p.viewCount == 2 && p.views[0].subImage.swapchain != p.views[1].subImage.swapchain) {
                        bool inside_images = true;
                        for (unsigned eye = 0; eye < 2; ++eye) {
                            const auto& sub = p.views[eye].subImage;
                            const auto chain = find_swapchain(sub.swapchain);
                            inside_images &= chain && sub.imageArrayIndex == 0 && sub.imageRect.offset.x >= 0 &&
                                sub.imageRect.offset.y >= 0 && sub.imageRect.extent.width > 0 && sub.imageRect.extent.height > 0 &&
                                std::uint32_t(sub.imageRect.offset.x + sub.imageRect.extent.width) <= chain->create_info.width &&
                                std::uint32_t(sub.imageRect.offset.y + sub.imageRect.extent.height) <= chain->create_info.height &&
                                chain->create_info.arraySize == 1 && chain->create_info.mipCount == 1;
                            if (chain) { rects[eye] = sub.imageRect; widths[eye] = chain->create_info.width; heights[eye] = chain->create_info.height; }
                        }
                        if (inside_images) {
                            layout = true;
                            eyes = {p.views[0].subImage.swapchain, p.views[1].subImage.swapchain};
                            eye_layer = layers.size();
                            space = p.space;
                            fov = {p.views[0].fov, p.views[1].fov};
                            poses = {p.views[0].pose, p.views[1].pose};
                        }
                    }
                } else if (base->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                    cached.quad = *reinterpret_cast<const XrCompositionLayerQuad*>(base);
                    cached.quad.next = nullptr;
                } else if (base->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
                    cached.cylinder = *reinterpret_cast<const XrCompositionLayerCylinderKHR*>(base);
                    cached.cylinder.next = nullptr;
                } else {
                    continue; // an inserted frame leaves out what it cannot copy
                }
                layers.push_back(std::move(cached));
            }
        } catch (...) {
            layout = false;
        }
    }
    const auto settings = uevr_mod_settings();
    // Very Late synchronisation (UEVR's default) waits and begins the frame
    // before the first eye is rendered; with Late the first eye is released
    // before the frame is begun, so no frame can be ended there.
    const std::uint32_t off_reasons = (uevr_afr_osr_wanted(session) ? 0u : 1u) | (settings.valid ? 0u : 2u) |
        (settings.rendering_method == xrfg::kUevrRenderingMethodAlternating ? 0u : 4u) |
        (settings.synchronization == xrfg::kUevrSynchronizationVeryLate ? 0u : 8u) | (layout ? 0u : 16u);
    const bool wanted = off_reasons == 0;
    if (off_reasons != afr->off_reasons) {
        // 1407: why OSR is off (0 = on): 1 session (binding, Luke, Reconstruction
        // setting, ini), 2 UEVR settings unreadable, 4 not Alternating/AFR,
        // 8 not Very Late, 16 no two-eye projection; b = rendering method,
        // c = synchronisation mode.
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage, off_reasons, 1407,
            static_cast<std::uint64_t>(static_cast<std::int64_t>(settings.rendering_method)),
            static_cast<std::uint64_t>(static_cast<std::int64_t>(settings.synchronization)));
        afr->off_reasons = off_reasons;
    }
    if (wanted) uevr_register_camera_feed();
    if (wanted != afr->active) {
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
            wanted ? 1 : 0, 1400, static_cast<std::uint64_t>(settings.rendering_method),
            (layout ? 1u : 0u) | (settings.valid ? 2u : 0u) | (settings.ghosting_fix ? 4u : 0u) |
            (static_cast<std::uint64_t>(settings.synchronization & 0xff) << 8));
        afr->active = wanted;
        afr->reset_pending = true;
        afr->captured_eye.reset();
        {std::scoped_lock pose_lock(afr->pose_mutex); afr->located = false;}
        session.uevr_osr_active.store(wanted, std::memory_order_release);
        xrfg::autonomous::uevr_osr_attached.store(wanted, std::memory_order_release);
    }
    if (layout) {
        if (afr->eye_swapchain != eyes) afr->reset_pending = true;
        afr->layout = true;
        afr->eye_swapchain = eyes;
        afr->eye_layer = eye_layer;
        afr->space = space;
        afr->located_space.store(wanted ? space : XR_NULL_HANDLE, std::memory_order_release);
        afr->fov = fov;
        afr->submitted_pose = poses;
        afr->rect = rects;
        afr->image_width = widths;
        afr->image_height = heights;
        afr->blend = info->environmentBlendMode;
        afr->layers = std::move(layers);
    } else {
        afr->layout = false;
    }
    // UEVR's frame: the runtime reprojects it from the pose its second eye
    // was relocated for (set before the layer forwards this End).
    if (wanted && second_pose && second_time && session.private_runtime)
        session.private_runtime->set_next_end_content_time(second_time);
    // 1416: a frame without the two-eye AFR layout while OSR ran: result =
    // layer count; b = type of each of the first four layers (16 bits each);
    // c = views of the first projection << 32 | 1 if its two swapchains differ.
    if (!layout && afr->active && info && info->layers) {
        std::uint64_t types = 0, views = 0;
        for (std::uint32_t l = 0; l < info->layerCount && l < 4; ++l)
            if (info->layers[l]) types |= (static_cast<std::uint64_t>(info->layers[l]->type) & 0xffff) << (16 * l);
        for (std::uint32_t l = 0; l < info->layerCount; ++l) {
            const auto* base = info->layers[l];
            if (!base || base->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
            const auto& p = *reinterpret_cast<const XrCompositionLayerProjection*>(base);
            views = (static_cast<std::uint64_t>(p.viewCount) << 32) |
                (p.viewCount == 2 && p.views && p.views[0].subImage.swapchain != p.views[1].subImage.swapchain ? 1u : 0u);
            break;
        }
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage, info->layerCount, 1416, types, views);
    }
    if (!info || info->type != XR_TYPE_FRAME_END_INFO || (!rewrite && !(wanted && second_pose))) return false;
    out.info = *info;
    if (rewrite) out.info.displayTime = *rewrite;
    if (wanted && second_pose && info->layers) {
        try {
            out.layers.assign(info->layers, info->layers + info->layerCount);
            for (auto& header : out.layers) {
                if (!header || header->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
                const auto& p = *reinterpret_cast<const XrCompositionLayerProjection*>(header);
                if (p.viewCount != 2 || !p.views) continue;
                const bool eyes = p.views[0].subImage.swapchain == afr->eye_swapchain[0] &&
                    p.views[1].subImage.swapchain == afr->eye_swapchain[1];
                if (eyes) {
                    // 1410: what UEVR submitted, before it is replaced. result =
                    // millidegrees between UEVR's right view and the right eye's
                    // render pose; b = display time of the pose query UEVR's
                    // right view equals, minus this frame's display time, in
                    // microseconds (INT32_MIN: none of the last eight);
                    // c = this frame's display time.
                    std::int64_t offset = INT32_MIN;
                    {
                        auto& feed = uevr_camera_feed();
                        std::scoped_lock feed_lock(feed.mutex);
                        for (const auto& located : feed.located) {
                            const auto& v = located.poses[1];
                            const auto& s = p.views[1].pose;
                            if (located.time && v.orientation.x == s.orientation.x && v.orientation.y == s.orientation.y &&
                                v.orientation.z == s.orientation.z && v.orientation.w == s.orientation.w &&
                                v.position.x == s.position.x && v.position.y == s.position.y && v.position.z == s.position.z)
                                offset = (located.time - out.info.displayTime) / 1000;
                        }
                    }
                    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                        static_cast<std::int64_t>(uevr_angle_degrees(p.views[1].pose.orientation, (*second_pose)[1].orientation) * 1000.0F),
                        1410, static_cast<std::uint64_t>(offset), static_cast<std::uint64_t>(out.info.displayTime));
                }
                auto& projection = out.projections.emplace_back(p);
                auto& views = out.views.emplace_back(p.views, p.views + p.viewCount);
                for (unsigned eye = 0; eye < 2; ++eye) views[eye].pose = (*second_pose)[eye];
                projection.views = views.data();
                header = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
            }
            out.info.layers = out.layers.data();
        } catch (...) {
            out.info = *info;
            if (rewrite) out.info.displayTime = *rewrite;
        }
    }
    return true;
}

void uevr_afr_observe_wait(SessionState& session, const XrFrameState* frame_state) {
    auto* afr = session.uevr_afr.get();
    if (!afr || !frame_state) return;
    std::scoped_lock lock(afr->mutex);
    if (afr->inside) return;
    afr->application_display_time = frame_state->predictedDisplayTime;
    if (frame_state->predictedDisplayPeriod > 0) afr->display_period = frame_state->predictedDisplayPeriod;
}

xrfg::UevrXrPose uevr_xr_pose(const XrPosef& pose) noexcept;

void uevr_afr_observe_locate(SessionState& session, const XrViewLocateInfo& info,
    const XrViewState& view_state, std::uint32_t count, const XrView* views) {
    auto* afr = session.uevr_afr.get();
    if (!afr || !views || count != 2) return;
    constexpr XrViewStateFlags kValid = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if ((view_state.viewStateFlags & kValid) != kValid) return;
    if (info.space == XR_NULL_HANDLE || info.space != afr->located_space.load(std::memory_order_acquire)) return;
    {
        std::scoped_lock lock(afr->pose_mutex);
        afr->located_pose = {views[0].pose, views[1].pose};
        afr->located = true;
    }
    auto& feed = uevr_camera_feed();
    std::scoped_lock feed_lock(feed.mutex);
    feed.last_xr = {uevr_xr_pose(views[0].pose), uevr_xr_pose(views[1].pose)};
    feed.last_xr_time = info.displayTime;
    feed.last_xr_valid = true;
    feed.located[feed.located_next++ % feed.located.size()] = {info.displayTime, {views[0].pose, views[1].pose}};
}

xrfg::UevrXrPose uevr_xr_pose(const XrPosef& pose) noexcept {
    return {{pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w},
        {pose.position.x, pose.position.y, pose.position.z}};
}

bool uevr_afr_reconstruct(SessionState& session, UevrAfrOsrState& afr, unsigned source_eye,
    ID3D12Resource* same_image);

// Before the application's release of an eye image: copy it while UEVR owns
// it, then build both eyes of this frame.
void uevr_afr_before_release(const std::shared_ptr<SwapchainState>& chain) {
    if (!chain || !chain->session) return;
    auto* afr = chain->session->uevr_afr.get();
    if (!afr) return;
    std::scoped_lock lock(afr->mutex);
    if (!afr->active || afr->inside || !afr->layout) return;
    unsigned eye = 2;
    for (unsigned i = 0; i < 2; ++i) if (afr->eye_swapchain[i] == chain->handle) eye = i;
    if (eye > 1) return;
    afr->captured_eye.reset();
    Microsoft::WRL::ComPtr<ID3D12Resource> image;
    {
        std::scoped_lock chain_lock(chain->mutex);
        if (chain->acquired_indices.empty()) return;
        const auto index = chain->acquired_indices.front();
        if (index >= chain->enumerated_d3d12_images.size()) return;
        image = chain->enumerated_d3d12_images[index];
    }
    if (!image) return;
    auto& session = *chain->session;
    HRESULT hr;
    {
        std::scoped_lock gpu_lock(session.gpu_mutex);
        hr = afr->renderer.capture_source(session.d3d12_device.Get(), session.d3d12_queue.Get(),
            image.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, eye);
    }
    if (FAILED(hr)) {
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage, hr, 1406, eye, 0);
        return;
    }
    afr->captured_eye = eye;
    // Both eyes of this frame are built now, while UEVR still owns the image:
    // the other eye into its own swapchain, this eye relocated in place.
    afr->reconstructed_eye = uevr_afr_reconstruct(session, *afr, eye, image.Get()) ? eye : 2;
}

// Reconstructs the other eye from the eye just released and writes it into
// the other eye's swapchain. Locked by the caller.
bool uevr_afr_reconstruct(SessionState& session, UevrAfrOsrState& afr, unsigned source_eye,
    ID3D12Resource* same_image) {
    const unsigned destination_eye = 1 - source_eye;
    const auto refuse = [&](std::uint32_t reason, std::uint64_t detail = 0) {
        ++afr.refusals;
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
            static_cast<std::int64_t>(reason), 1404, source_eye, detail);
        return false;
    };
    const auto destination = find_swapchain(afr.eye_swapchain[destination_eye]);
    const auto source = find_swapchain(afr.eye_swapchain[source_eye]);
    if (!destination || !source) return refuse(1);
    if (destination->create_info.width != source->create_info.width ||
        destination->create_info.height != source->create_info.height) return refuse(2);

    // This frame's DLSS depth and vectors: the newest evaluation, which must
    // be newer than the one the previous eye used.
    const auto set = xrfg::resolve_dlss_motion_vectors(1, session.d3d12_queue.Get());
    const auto guide = set && set->eye_count ? set->eyes[0] : nullptr;
    if (!guide) return refuse(3);
    if (guide->publication <= afr.last_publication) return refuse(4, guide->publication);

    // The eye's world camera from UEVR, and the units of its world.
    const auto now = GetTickCount64();
    std::optional<xrfg::UevrEyeCamera> camera;
    // The eye the released image was rendered from, by where UEVR placed its
    // viewpoint; it decides the baseline. The swapchain index keys the history.
    unsigned physical_source = afr.last_physical < 2 ? 1 - afr.last_physical : source_eye;
    std::size_t queued_before{};
    std::int64_t front_offset_us{INT32_MIN};
    // The pose query for this pair's own display time (UEVR's xrWaitFrame):
    // the one this pair's eyes were rendered with when the camera carries none.
    std::optional<std::pair<std::int64_t, std::array<XrPosef, 2>>> pair_pose;
    xrfg::UevrXrPose newest_xr{};
    std::int64_t newest_xr_time{};
    {
        auto& feed = uevr_camera_feed();
        std::scoped_lock feed_lock(feed.mutex);
        newest_xr = feed.last_xr[0];
        newest_xr_time = feed.last_xr_time;
        queued_before = feed.queue.size();
        if (const auto* front = feed.queue.front()) front_offset_us = front->xr_valid
            ? (front->xr_time - afr.application_display_time) / 1000 : INT32_MIN;
        // The view of the eye this swapchain shows. In Dead Island 2 every
        // release's view had this eye while the pairing held (3,500 of 3,500).
        if (auto taken = feed.queue.take_eye(source_eye, now, 200)) {
            camera = taken->camera;
            if (taken->eye < 2) physical_source = taken->eye;
        }
        for (const auto& located : feed.located) {
            if (!located.time) continue;
            const auto distance = std::llabs(located.time - afr.application_display_time);
            if (!pair_pose || distance < std::llabs(pair_pose->first - afr.application_display_time))
                pair_pose = std::make_pair(located.time, located.poses);
        }
    }
    afr.last_physical = physical_source;
    // 1409: result = released eye | physical eye << 8; a = stage; b = camera sequence (0 none);
    // c = views queued before taking << 32 | (pose time of the oldest queued
    // view - this frame's display time, microseconds, int32).
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage, source_eye | (physical_source << 8), 1409,
        camera ? camera->sequence : 0,
        (static_cast<std::uint64_t>(queued_before) << 32) | static_cast<std::uint32_t>(static_cast<std::int32_t>(front_offset_us)));
    const auto settings = uevr_mod_settings();
    const float world_to_meters = camera && camera->world_to_meters > 0 ? camera->world_to_meters : 100.0F;
    const float units = world_to_meters * settings.world_scale;
    const float near_plane = guide->camera_near > 0 ? guide->camera_near : 10.0F;

    // The two eyes of the instant this eye was rendered for: the pose query
    // UEVR made for its view. Its newest query can already belong to the
    // next frame, a display period further on (UE's game thread runs ahead).
    std::array<XrPosef, 2> poses = afr.submitted_pose;
    unsigned pose_source = 0; // 0 last submitted, 1 this pair's display time, 2 the camera's own query
    std::int64_t pose_time = 0;
    if (camera && camera->xr_valid) {
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto& p = camera->xr_eyes[eye];
            poses[eye].orientation = {p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]};
            poses[eye].position = {p.position[0], p.position[1], p.position[2]};
        }
        pose_source = 2;
        pose_time = camera->xr_time;
    } else if (pair_pose) {
        poses = pair_pose->second;
        pose_source = 1;
        pose_time = pair_pose->first;
    }
    // The pose of this frame's display slot. UEVR samples the head once per
    // pair and renders both frames with it; here both eyes of the frame are
    // relocated to the slot pose, so content, submitted pose and slot agree
    // whatever the runtime keys its reprojection on, and head tracking runs
    // at the frame rate again. Without a slot pose the frame keeps UEVR's.
    std::array<XrPosef, 2> slot = poses;
    bool relocated = false;
    // The slot this frame is shown in. The display time UEVR's frame (or the
    // inserted one) was waited for is the private runtime's continuous
    // prediction: 2.6 ms ahead of the frame's real native slot on average,
    // 1.9 ms sd, with 5-9 ms drift episodes (r20 log matched by packet
    // sequence). Relocated there, the head pose wandered against the display
    // grid: a jitter a couple of times a second while turning. Snapped to the
    // grid of native slots it is the frame's real slot for 98.6% of frames:
    // the last native slot plus whole periods, rounded with a 0.4-period
    // bias. Not one period after that last slot (r15-r18): the queue is two
    // frames deep, so that slot belongs to an earlier frame.
    XrTime slot_time = afr.application_display_time;
    bool snapped = false;
    if (session.private_runtime && afr.display_period > 0 && slot_time) {
        const XrTime last = session.private_runtime->last_native_display_time();
        if (last > 0) {
            const double periods = static_cast<double>(slot_time - last) / static_cast<double>(afr.display_period);
            const auto k = static_cast<std::int64_t>(std::floor(periods + kUevrSlotSnapBias + 0.5));
            const XrTime grid = last + k * afr.display_period;
            if (k >= 1 && std::llabs(grid - slot_time) <= afr.display_period) { slot_time = grid; snapped = true; }
        }
    }
    if (camera && camera->xr_valid && afr.space != XR_NULL_HANDLE && slot_time &&
        session.dispatch && session.dispatch->locate_views) {
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = slot_time;
        locate.space = afr.space;
        XrViewState view_state{XR_TYPE_VIEW_STATE};
        XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        std::uint32_t count{};
        constexpr XrViewStateFlags kValid = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        if (XR_SUCCEEDED(session.dispatch->locate_views(session.handle, &locate, &view_state, 2, &count, views)) &&
            count == 2 && (view_state.viewStateFlags & kValid) == kValid) {
            slot = {views[0].pose, views[1].pose};
            relocated = true;
            pose_source = snapped ? 4 : 3;
            pose_time = slot_time;
        }
    }
    const auto& submitted = relocated ? slot : poses;
    // 1408: result = source eye | pose source << 8 (0 last submitted, 1 the
    // query nearest this pair's display time, 2 the camera's own query, 3 the
    // slot pose both eyes are relocated to, 4 the same snapped to the native
    // slot grid); a = stage; b = display time of the pose used; c = UEVR's
    // display time.
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage, source_eye | (pose_source << 8), 1408,
        static_cast<std::uint64_t>(pose_time), static_cast<std::uint64_t>(afr.application_display_time));
    // 1412: yaws, degrees as float bits. result = source eye; b = the HMD yaw
    // UEVR added to this view (its rotation after the callback minus the
    // engine's before it) << 32 | yaw of the locate stamped on the view;
    // c = yaw of the newest locate << 32 | yaw of the pose submitted for this
    // frame (view 0). With a still head all four are constant; turning, the
    // stamped yaw must follow UEVR's and the submitted yaw must equal the stamped.
    if (camera) {
        const auto bits = [](float v) { std::uint32_t b{}; std::memcpy(&b, &v, sizeof(b)); return static_cast<std::uint64_t>(b); };
        const float used = static_cast<float>(xrfg::uevr_wrap_degrees(camera->rotation[1] - camera->base_rotation[1]));
        const float stamped = camera->xr_valid ? xrfg::uevr_xr_yaw_degrees(camera->xr_eyes[0].orientation) : 0.0F;
        const float newest = xrfg::uevr_xr_yaw_degrees(newest_xr.orientation);
        const float submitted_yaw = xrfg::uevr_xr_yaw_degrees(uevr_xr_pose(submitted[0]).orientation);
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage, source_eye, 1412,
            (bits(used) << 32) | bits(stamped), (bits(newest) << 32) | bits(submitted_yaw));
    }
    // From the source eye as rendered (its own locate) to each eye at the slot pose.
    const auto eye_to_eye = xrfg::uevr_eye_to_eye(uevr_xr_pose(poses[physical_source]),
        uevr_xr_pose(slot[1 - physical_source]), units);
    const auto eye_to_same = xrfg::uevr_eye_to_eye(uevr_xr_pose(poses[physical_source]),
        uevr_xr_pose(slot[physical_source]), units);

    xrfg::UevrOsrFrame frame{};
    frame.guide = guide.get();
    frame.source_eye = source_eye;
    // Whole images: the field of each eye's image, from its submitted field and rectangle.
    const auto image_projection = [&](unsigned eye) {
        const auto& f = afr.fov[eye];
        const auto& r = afr.rect[eye];
        const auto t = xrfg::uevr_image_tangents(f.angleLeft, f.angleRight, f.angleUp, f.angleDown,
            r.offset.x, r.offset.y, r.extent.width, r.extent.height, afr.image_width[eye], afr.image_height[eye]);
        return xrfg::uevr_projection_tangents(t.left, t.right, t.up, t.down, near_plane);
    };
    frame.source_view_to_clip = image_projection(source_eye);
    frame.destination_view_to_clip = image_projection(destination_eye);
    frame.cameras_valid = camera.has_value();
    frame.reset = afr.reset_pending || guide->reset;
    if (camera) {
        // A floating origin keeps world positions within float precision;
        // moving it invalidates the history kept in the old frame.
        double distance = 0;
        for (int i = 0; i < 3; ++i) distance = std::max(distance, std::abs(camera->location[i] - afr.origin[i]));
        if (!afr.origin_valid || distance > 1000.0 * units) {
            afr.origin = camera->location;
            afr.origin_valid = true;
            frame.reset = true;
        }
        frame.source_view_to_world = xrfg::uevr_view_to_world(*camera, afr.origin);
        frame.source_engine_to_world = xrfg::uevr_engine_to_world(*camera, afr.origin);
        frame.destination_engine_to_world = frame.source_engine_to_world;
        frame.engine_valid = true;
    } else {
        frame.source_view_to_world = xrfg::uevr_identity();
    }
    frame.destination_view_to_world = xrfg::uevr_multiply(eye_to_eye, frame.source_view_to_world);
    frame.same_view_to_world = xrfg::uevr_multiply(eye_to_same, frame.source_view_to_world);
    frame.relocate_same = relocated && same_image != nullptr;
    // UEVR holds the engine rotation for the pair (calculate_stereo_view_offset,
    // m_last_afr_rotation): a stick turn advances only on the pair's first
    // frame. At a steady turn the second frame lies halfway, so both its eyes
    // are turned by half the last pair's yaw step, in place, when its engine
    // yaw is indeed the held one. Yaw only; the engine's pitch and roll are
    // rare on a stick and the HMD's are per frame already. Only the stick's
    // share of the step: where the engine camera follows the head (Atomic
    // Heart), its step is the head's turn, which the slot pose already has;
    // turned by half of it again, every second frame overshot while the head
    // turned and the view doubled.
    double extra_yaw = 0;
    double carried = 0;
    if (camera) {
        const double base_yaw = camera->base_rotation[1];
        if (afr.eye_releases == 0) {
            const double step = afr.pair_yaw_valid ? xrfg::uevr_wrap_degrees(base_yaw - afr.pair_yaw) : 0;
            // UE yaw turns right; the OpenXR yaw turns left.
            const double head_yaw = camera->xr_valid ? -xrfg::uevr_xr_yaw_degrees(camera->xr_eyes[0].orientation) : 0;
            const double added_yaw = xrfg::uevr_wrap_degrees(camera->rotation[1] - base_yaw);
            double stick = step;
            if (camera->xr_valid && afr.pair_head_valid) {
                const double head_step = xrfg::uevr_wrap_degrees(head_yaw - afr.pair_head_yaw);
                const double added_step = xrfg::uevr_wrap_degrees(added_yaw - afr.pair_added_yaw);
                stick = xrfg::uevr_stick_yaw_step(step, head_step, added_step);
                carried = step - stick;
            }
            afr.pair_yaw_step = std::abs(stick) < 30 ? stick : 0;
            afr.pair_yaw = base_yaw;
            afr.pair_yaw_valid = true;
            afr.pair_head_yaw = head_yaw;
            afr.pair_added_yaw = added_yaw;
            afr.pair_head_valid = camera->xr_valid;
        } else if (afr.pair_yaw_valid && std::abs(xrfg::uevr_wrap_degrees(base_yaw - afr.pair_yaw)) < 1e-3 && relocated) {
            extra_yaw = afr.pair_yaw_step * 0.5;
        }
        if (extra_yaw != 0) {
            frame.destination_view_to_world = xrfg::uevr_yaw_about_up(frame.destination_view_to_world, extra_yaw);
            frame.same_view_to_world = xrfg::uevr_yaw_about_up(frame.same_view_to_world, extra_yaw);
            frame.destination_engine_to_world = xrfg::uevr_yaw_about_up(frame.destination_engine_to_world, extra_yaw);
        }
        // Both frames of a pair are rendered for one head pose and should sit
        // on one world: the second frame's camera where the first frame's
        // would be from its eye. In Atomic Heart (aim from a controller, the
        // pawn's control rotation) UEVR adds the head's turn on the first
        // frame only and the second keeps the engine's, 1-1.3 deg behind
        // while turning (r40 log); every frame of OSR shows both eyes from
        // one source, so the view alternated and doubled sideways (F8: 20-52
        // px back on each second frame). Turned onto the first frame's world
        // here; where both agree (Dead Island 2) the turn is nil.
        double pair_turn = 0, pair_turn_yaw = 0;
        if (afr.eye_releases == 0) {
            afr.pair_view = frame.source_view_to_world;
            afr.pair_pose = uevr_xr_pose(poses[physical_source]);
            afr.pair_pose_time = camera->xr_valid ? camera->xr_time : 0;
            afr.pair_view_valid = camera->xr_valid && !frame.reset;
        } else if (afr.pair_view_valid && camera->xr_valid && camera->xr_time == afr.pair_pose_time && !frame.reset) {
            const auto expected = xrfg::uevr_multiply(
                xrfg::uevr_eye_to_eye(uevr_xr_pose(poses[physical_source]), afr.pair_pose, units), frame.source_view_to_world);
            const auto turn = xrfg::uevr_turn_between(expected, afr.pair_view);
            pair_turn = xrfg::uevr_turn_degrees(turn);
            pair_turn_yaw = xrfg::uevr_turn_yaw_degrees(turn);
            if (pair_turn > 0.01 && pair_turn < kUevrPairTurnLimitDegrees) {
                frame.destination_view_to_world = xrfg::uevr_turn_in_place(frame.destination_view_to_world, turn);
                frame.same_view_to_world = xrfg::uevr_turn_in_place(frame.same_view_to_world, turn);
            }
        }
        if (afr.eye_releases != 0) {
            // 1417: the pair's second frame. result = source eye; b = float
            // bits of the turn onto the first frame's world (degrees) << 32 |
            // float bits of its yaw part (UE yaw); c = 1 applied.
            const auto bits = [](float v) { std::uint32_t b{}; std::memcpy(&b, &v, sizeof(b)); return static_cast<std::uint64_t>(b); };
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage, source_eye, 1417,
                (bits(static_cast<float>(pair_turn)) << 32) | bits(static_cast<float>(pair_turn_yaw)),
                pair_turn > 0.01 && pair_turn < kUevrPairTurnLimitDegrees ? 1 : 0);
        }
        // 1413: result = source eye; b = float bits of the last pair's stick
        // yaw step << 32 | float bits of the yaw added to this frame;
        // c = float bits of the share of the engine's step the head carried
        // (pair's first frame, else 0) << 32 | float bits of this view's engine yaw.
        const auto bits = [](float v) { std::uint32_t b{}; std::memcpy(&b, &v, sizeof(b)); return static_cast<std::uint64_t>(b); };
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage, source_eye, 1413,
            (bits(static_cast<float>(afr.pair_yaw_step)) << 32) | bits(static_cast<float>(extra_yaw)),
            (bits(static_cast<float>(carried)) << 32) | bits(static_cast<float>(base_yaw)));
    }
    frame.history_motion = settings.ghosting_fix;
    const auto afw = xrfg::embedded::afw_snapshot().desired;
    frame.mode = afw.mode;
    frame.debug = afw.debug;
    frame.lower_quality = afw.osr_lower_quality;
    frame.units_per_metre = units;
    if (afr.display_period > 0) frame.display_period_ms = static_cast<float>(afr.display_period) / 1.0e6F;

    // The destination eye's next image, through this layer's own swapchain
    // calls so its history and bookkeeping see it like any release.
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    std::uint32_t index{};
    std::optional<UevrInsideScope> inside(std::in_place, afr.inside);
    XrResult xr = layer_acquire_swapchain_image_impl(destination->handle, &acquire, &index);
    if (XR_FAILED(xr)) return refuse(5, static_cast<std::uint64_t>(xr));
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = 100'000'000;
    xr = layer_wait_swapchain_image_impl(destination->handle, &wait);
    Microsoft::WRL::ComPtr<ID3D12Resource> image;
    if (XR_SUCCEEDED(xr)) {
        std::scoped_lock chain_lock(destination->mutex);
        if (index < destination->enumerated_d3d12_images.size()) image = destination->enumerated_d3d12_images[index];
    }
    xrfg::UevrOsrResult result{};
    if (image) {
        std::scoped_lock gpu_lock(session.gpu_mutex);
        result = afr.renderer.render(session.d3d12_device.Get(), session.d3d12_queue.Get(), frame,
            image.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, same_image, D3D12_RESOURCE_STATE_RENDER_TARGET);
        // F8: the same stage names as Luke Ross OSR, so the same tools read them.
        if (SUCCEEDED(result.result) && xrfg::two_frame_capture_images_active()) {
            const auto capture = [&](const char* stage, ID3D12Resource* resource, unsigned eye) {
                if (!resource) return;
                xrfg::TwoFrameImageInfo info{};
                info.stage = stage;
                info.stream = guide->stream;
                info.capture_serial = guide->publication;
                info.display_time = static_cast<std::uint64_t>(afr.application_display_time);
                info.eye = static_cast<std::int32_t>(eye);
                const auto desc = resource->GetDesc();
                info.width = static_cast<std::uint32_t>(desc.Width);
                info.height = desc.Height;
                static_cast<void>(xrfg::two_frame_capture_record_queue(info, session.d3d12_device.Get(),
                    session.d3d12_queue.Get(), resource, D3D12_RESOURCE_STATE_COMMON));
            };
            capture("afw_source_color", afr.renderer.source_image(source_eye), source_eye);
            capture("afw_generated_eye", afr.renderer.output(), destination_eye);
            if (result.same_rendered) capture("osr_relocated_eye", afr.renderer.same_output(), source_eye);
        }
    }
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const XrResult released = XR_SUCCEEDED(xr) ? layer_release_swapchain_image_impl(destination->handle, &release) : xr;
    // The release bound the newest DLSS evaluation to the reconstructed eye:
    // this frame's, the real eye's own; frame generation then saw one
    // evaluation for both eyes and DLSS-G refused every frame (generation
    // reason 27). Each eye carries OSR's guide instead: its shown depth and
    // its motion since its previous frame, one unbroken serial chain per eye
    // (DLSS-G refused an eye's guide repeated from the frame before, detail 5).
    // The real eye's is bound after UEVR's release (uevr_afr_after_release).
    if (XR_SUCCEEDED(released) && result.guides[destination_eye]) {
        std::scoped_lock chain_lock(destination->mutex);
        destination->last_released_motion_vectors = result.guides[destination_eye];
    }
    afr.source_guide = result.guides[source_eye];
    inside.reset();
    // 1401: result = HRESULT; a = stage; b = source eye | history used << 8 |
    // vectors used << 9 | camera known << 10 | reset << 11 | refusal << 16;
    // c = DLSS publication.
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage, result.result, 1401,
        source_eye | (result.history_used ? 0x100u : 0u) | (result.history_motion_used ? 0x200u : 0u) |
        (frame.cameras_valid ? 0x400u : 0u) | (frame.reset ? 0x800u : 0u) | (result.same_rendered ? 0x1000u : 0u) |
        (static_cast<std::uint64_t>(result.reason) << 16), guide->publication);
    // 1411: geometry receipts. result = source eye; b = float bits of the
    // destination's offset along the source's right axis (world units) << 32
    // | float bits of the destination camera's distance from the history
    // camera (NaN: no history); c = float bits of UEVR's world_to_meters << 32
    // | float bits of the units per metre used.
    {
        const auto bits = [](float v) { std::uint32_t b{}; std::memcpy(&b, &v, sizeof(b)); return static_cast<std::uint64_t>(b); };
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage, source_eye, 1411,
            (bits(result.baseline_right) << 32) | bits(result.history_distance),
            (bits(camera ? camera->world_to_meters : 0.0F) << 32) | bits(units));
    }
    // 1415: GPU microseconds of the last completed capture/reconstruction
    // (xrfg/uevr_osr.hpp UevrOsrResult::gpu_us), 16 bits each. result = eye
    // copy out; b = conversion | other-eye OSR << 16 | copy into its swapchain
    // << 32 | relocation << 48; c = history copy.
    if (SUCCEEDED(result.result)) {
        const auto& g = result.gpu_us;
        const auto c16 = [](std::uint32_t v) { return static_cast<std::uint64_t>(std::min<std::uint32_t>(v, 0xffff)); };
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage, static_cast<std::int64_t>(g[0]), 1415,
            c16(g[1]) | (c16(g[2]) << 16) | (c16(g[3]) << 32) | (c16(g[4]) << 48), g[5]);
    }
    if (XR_FAILED(xr) || !image) return refuse(6, static_cast<std::uint64_t>(xr));
    if (XR_FAILED(released)) return refuse(7, static_cast<std::uint64_t>(released));
    if (FAILED(result.result)) return refuse(8, result.reason);
    afr.last_publication = guide->publication;
    afr.frame_pose = submitted;
    afr.frame_pose_valid = true;
    afr.frame_time = relocated ? slot_time : 0;
    afr.reset_pending = false;
    afr.reconstructed_at = now;
    ++afr.reconstructions;
    // The tray and the headset meter show OSR running as for Luke Ross.
    xrfg::embedded::report_osr_configuration(2, result.lower_quality);
    xrfg::embedded::report_afw_evaluation(guide->publication, source_eye, guide->motion_width,
        guide->motion_height, guide->output_width, guide->output_height, guide->scale_x, guide->scale_y, S_OK);
    return true;
}

// The inserted frame: what UEVR's xrEndFrame would carry, frozen at the first
// eye's release, with the pose that eye was rendered for.
struct UevrInsertJob {
    std::vector<UevrCachedLayer> layers;
    std::size_t eye_layer{};
    std::array<XrPosef, 2> pose{};
    bool pose_valid{};
    XrTime display_time{};
    XrEnvironmentBlendMode blend{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
};

// Ends the frame UEVR began, with both eyes as they now are, and begins the
// one UEVR's coming xrEndFrame will close, on UEVR's render thread. Locked by
// the caller.
//
// The inserted end blocks until the private runtime frees a slot; that block
// is what spaces the pair's two game frames (eye releases 11.2 ms apart on
// average, Dead Island 2 r15 log). Run on another thread (r16), UEVR rendered
// the second frame right after the first and the world moved at 45 Hz. Held
// to one period after UEVR's xrBeginFrame (r17), the releases alternated
// 17/6 ms: that begin comes ~1 ms before the first eye's release, not when
// its rendering starts.
void uevr_afr_insert_frame(SessionState& session, UevrAfrOsrState& afr) {
    std::vector<XrCompositionLayerBaseHeader*> headers;
    std::vector<UevrCachedLayer> layers = afr.layers;
    for (auto& layer : layers) {
        if (layer.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            // UEVR's own projection and its Virtual Desktop placeholder both carry the eyes' poses.
            if (afr.frame_pose_valid && layer.views.size() == 2)
                for (unsigned eye = 0; eye < 2; ++eye) layer.views[eye].pose = afr.frame_pose[eye];
            layer.projection.views = layer.views.data();
            layer.projection.viewCount = static_cast<std::uint32_t>(layer.views.size());
            headers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer.projection));
        } else if (layer.type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            headers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer.quad));
        } else if (layer.type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
            headers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer.cylinder));
        }
    }
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = afr.application_display_time;
    end.environmentBlendMode = afr.blend;
    end.layerCount = static_cast<std::uint32_t>(headers.size());
    end.layers = headers.data();
    bool presenter_was_active{};
    {std::scoped_lock presenter_lock(session.presenter_mutex);
     presenter_was_active = session.presenter_active && !session.presenter_stop_requested;}
    std::optional<UevrInsideScope> inside(std::in_place, afr.inside);
    // The runtime reprojects the frame from the pose it was relocated for.
    if (session.private_runtime && afr.frame_time) session.private_runtime->set_next_end_content_time(afr.frame_time);
    const XrResult ended = layer_end_frame_impl(session.handle, &end);
    XrResult waited = XR_ERROR_CALL_ORDER_INVALID, begun = XR_ERROR_CALL_ORDER_INVALID;
    XrFrameState state{XR_TYPE_FRAME_STATE};
    if (XR_SUCCEEDED(ended)) {
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
        waited = layer_wait_frame_impl(session.handle, &wait, &state);
        if (XR_SUCCEEDED(waited)) {
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
            begun = layer_begin_frame_impl(session.handle, &begin);
        }
    }
    inside.reset();
    if (XR_SUCCEEDED(begun)) {
        if (state.predictedDisplayPeriod > 0) afr.display_period = state.predictedDisplayPeriod;
        // With frame generation's presenter the Wait is virtual and returns at
        // once; when the inserted frame was taken at once too, it can hand out
        // the inserted frame's own slot again (+1 ns, Dead Island 2 r45: one
        // in 30 pairs, the one before the freeze). The pair's second frame is
        // a frame of its own: never the same display time.
        XrTime next = state.predictedDisplayTime;
        if (afr.display_period > 0 && next < end.displayTime + afr.display_period / 2)
            next = end.displayTime + afr.display_period;
        afr.rewrite_display_time = next;
        afr.application_display_time = next;
        ++afr.inserted;
    }
    // 1418: who owned the frame loop when the frame was inserted. result =
    // inserted End; b = frame generation's presenter active | pipelined
    // presenter mode << 1 | generation enabled << 2; c = UEVR's display time.
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage, ended, 1418,
        (presenter_was_active ? 1u : 0u) | (session.pipelined_presenter_mode ? 2u : 0u) | (session.menu_enabled ? 4u : 0u),
        static_cast<std::uint64_t>(end.displayTime));
    // 1402: result = xrEndFrame of the inserted frame; a = stage; b = wait
    // result << 32 | begin result (low 32 bits each); c = new display time.
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage, ended, 1402,
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(waited)) << 32) | static_cast<std::uint32_t>(begun),
        static_cast<std::uint64_t>(state.predictedDisplayTime));
}

// After the application's release of an eye image.
void uevr_afr_after_release(const std::shared_ptr<SwapchainState>& chain, XrResult release_result) {
    if (!chain || !chain->session) return;
    auto& session = *chain->session;
    auto* afr = session.uevr_afr.get();
    if (!afr) return;
    std::scoped_lock lock(afr->mutex);
    if (!afr->active || afr->inside || !afr->layout) return;
    unsigned eye = 2;
    for (unsigned i = 0; i < 2; ++i) if (afr->eye_swapchain[i] == chain->handle) eye = i;
    if (eye > 1) return;
    const bool reconstructed = afr->reconstructed_eye == eye;
    afr->reconstructed_eye = 2;
    afr->captured_eye.reset();
    if (reconstructed && afr->source_guide && XR_SUCCEEDED(release_result)) {
        std::scoped_lock chain_lock(chain->mutex);
        chain->last_released_motion_vectors = afr->source_guide;
    }
    afr->source_guide.reset();
    const bool first_eye = afr->eye_releases++ == 0;
    if (XR_FAILED(release_result)) return;
    // Only a pair whose first eye was completed gets a frame of its own;
    // otherwise UEVR's frame goes out as UEVR made it.
    if (reconstructed && first_eye && session.application_frame_in_progress) uevr_afr_insert_frame(session, *afr);
    if (reconstructed && !first_eye) {
        afr->second_pose = afr->frame_pose;
        afr->second_pose_valid = true;
        afr->second_time = afr->frame_time;
    }
    const auto now = GetTickCount64();
    if (now >= afr->next_report) {
        afr->next_report = now + 1000;
        std::size_t queued{};
        std::uint64_t views{}, unknown{};
        std::array<std::uint64_t, 2> decided{};
        {
            auto& feed = uevr_camera_feed();
            std::scoped_lock feed_lock(feed.mutex);
            queued = feed.queue.size(); views = feed.views; unknown = feed.unknown_eye; decided = feed.phase.decided();
        }
        // 1403: a = stage; b = reconstructions << 32 | refusals; c = inserted
        // frames << 40 | camera views << 16 | (views of unknown eye & 0xff) << 8
        // | queued; result = views told left << 32 | views told right.
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
            static_cast<std::int64_t>((decided[0] << 32) | (decided[1] & 0xffffffffu)), 1403,
            (afr->reconstructions << 32) | (afr->refusals & 0xffffffffu),
            (afr->inserted << 40) | ((views & 0xffffff) << 16) | ((unknown & 0xff) << 8) | (queued & 0xff));
    }
}
