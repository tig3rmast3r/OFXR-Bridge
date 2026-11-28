// Included after uevr_api(). Discover and validate the stock code contract.
// UEVR retains ownership of view creation, its readiness gates and routing.
namespace uevr_stock_bootstrap {
struct Draw {
    bool pulse{},overrode{},accepted{},camera_kept{};
};
thread_local Draw draw;
struct State {
    std::mutex mutex;
    xrfg::UevrStockBootstrap policy;
    SafetyHookMid gate,accepted;
    std::atomic_bool installed{},requested{},callbacks_ready{};
    HMODULE attempted{};
    unsigned last_phase{~0u},last_settings_reasons{~0u},accepted_frames{},observations{};
};
State& state() {static auto* s=new State;return *s;}

bool active() noexcept {
    auto& s=state();return s.installed.load(std::memory_order_acquire) &&
        s.callbacks_ready.load(std::memory_order_acquire) && s.requested.load(std::memory_order_acquire);
}
bool ready() noexcept {
    if(!active())return true;
    auto& s=state();std::scoped_lock lock(s.mutex);return s.policy.ready;
}
void gate(SafetyHookContext& ctx) noexcept {
#if defined(_WIN64)
    if(draw.pulse) {ctx.rflags=xrfg::uevr_stock_bootstrap_flags(ctx.rflags,true);draw.overrode=true;}
#endif
}
void accepted(SafetyHookContext& ctx) noexcept {
#if defined(_WIN64)
    if(draw.pulse && draw.overrode && ctx.rax==2)draw.accepted=true;
#endif
}

#if defined(_WIN64)
std::vector<std::uint8_t> module_snapshot(HMODULE module) {
    MODULEINFO info{};
    if(!GetModuleInformation(GetCurrentProcess(),module,&info,sizeof(info)) ||
        info.SizeOfImage<4096 || info.SizeOfImage>128*1024*1024)return {};
    std::vector<std::uint8_t> image(info.SizeOfImage);
    const auto base=reinterpret_cast<std::uintptr_t>(module);
    for(std::size_t offset=0;offset<image.size();) {
        MEMORY_BASIC_INFORMATION memory{};
        if(!VirtualQuery(reinterpret_cast<void*>(base+offset),&memory,sizeof(memory)))return {};
        const auto end=std::min(base+image.size(),reinterpret_cast<std::uintptr_t>(memory.BaseAddress)+memory.RegionSize);
        if(end<=base+offset)return {};
        const auto size=end-base-offset;
        if(memory.State==MEM_COMMIT && !(memory.Protect&(PAGE_NOACCESS|PAGE_GUARD))) {
            SIZE_T count{};
            if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(base+offset),image.data()+offset,size,&count) || count!=size)return {};
        }
        offset+=size;
    }
    return image;
}
#endif
void install() noexcept {
#if defined(_WIN64)
    try {
        const auto* api=uevr_api();
        if(!api || !xrfg::uevr_stock_discovery::supports_sdk(api->version->major,api->version->minor) ||
            !api->vr || !api->vr->get_mod_value)return;
        // A fork that owns bootstrap must retain its native behavior. Check
        // capability rather than guessing from a commit or SDK minor number.
        char bootstrap[64]{};
        api->vr->get_mod_value("VR_GhostingFixBootstrapViewStates",bootstrap,sizeof(bootstrap)-1);
        if(bootstrap[0])return;
        const auto module=GetModuleHandleW(L"UEVRBackend.dll");if(!module)return;
        auto& s=state();std::scoped_lock lock(s.mutex);
        if(s.attempted==module)return;s.attempted=module;
        const auto report=[](int reason){xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,reason,1462);};
        const auto image=module_snapshot(module);
        const auto sites=xrfg::uevr_stock_discovery::discover(image);
        if(!sites){report(3);return;}
        const auto base=reinterpret_cast<std::uintptr_t>(module);
        // Reject code changed between discovery and hook creation, including
        // existing hooks. Both hook points came from decoded instructions.
        std::vector<std::uint8_t> live(sites->end-sites->entry);SIZE_T read{};
        if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(base+sites->entry),live.data(),live.size(),&read) ||
            read!=live.size() || !std::equal(live.begin(),live.end(),image.begin()+sites->entry)){report(3);return;}
        auto a=safetyhook::create_mid(reinterpret_cast<void*>(base+sites->gate),gate,SafetyHookMid::StartDisabled);
        auto b=safetyhook::create_mid(reinterpret_cast<void*>(base+sites->accepted),accepted,SafetyHookMid::StartDisabled);
        if(!a || !b){report(4);return;}
        HMODULE backend{},layer{};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(module),&backend) ||
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&gate),&layer)){report(5);return;}
        // TLS starts disabled. Install the observation first; failed enable
        // leaves no partial active override. No GPU or presentation calls.
        if(!b.enable() || !a.enable()){report(6);return;}
        s.gate=std::move(a);s.accepted=std::move(b);
        s.installed.store(true,std::memory_order_release);report(1);
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
            0,1466,(std::uint64_t(sites->entry)<<32)|sites->gate,sites->accepted);
    } catch(...) {}
#endif
}

unsigned settings_reasons(const UEVR_PluginInitializeParam* api) {
    struct Cached {ULONGLONG next{};unsigned reasons{64};};static thread_local Cached cached;
    const auto now=GetTickCount64();if(now<cached.next)return cached.reasons;
    cached.next=now+1000;cached.reasons=64;
    if(!api || !api->vr || !api->vr->get_mod_value)return cached.reasons;
    const auto read=[&](const char* key) {char text[64]{};api->vr->get_mod_value(key,text,63);return std::string(text);};
    // Preserve the fork's bootstrap and all incompatible rendering modes.
    cached.reasons=xrfg::uevr_stock_bootstrap_setting_reasons(read);
    return cached.reasons;
}
void before_draw(UEVR_UGameViewportClientHandle viewport) noexcept {
    draw={};if(!active())return;
    try {
        const auto* api=uevr_api();std::array<std::uintptr_t,3> key{};
        auto reasons=settings_reasons(api);
        if(!api || !api->vr || !api->vr->is_runtime_ready || !api->vr->is_runtime_ready())reasons|=128;
        bool allowed=!reasons;
        if(allowed && api->sdk && api->sdk->functions && api->sdk->functions->get_player_controller &&
            api->sdk->uobject && api->sdk->uobject->get_outer) {
            const auto player=api->sdk->functions->get_player_controller(0);
            const auto outer=player?api->sdk->uobject->get_outer(player):nullptr;
            key={reinterpret_cast<std::uintptr_t>(viewport),reinterpret_cast<std::uintptr_t>(player),
                reinterpret_cast<std::uintptr_t>(outer)};
        }
        if(allowed && (!key[0] || !key[1] || !key[2]))reasons|=256;
        auto& s=state();std::scoped_lock lock(s.mutex);
        if(reasons!=s.last_settings_reasons) {
            s.last_settings_reasons=reasons;
            // 1461 blocker bits: method1, sync2, Ghosting Fix4, splitscreen8,
            // SceneView16, fork32, missing SDK64, runtime128, scene key256.
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                reasons,1461,s.policy.frame,0);
        }
        draw.pulse=s.policy.begin(key,allowed);
        const auto phase=unsigned(s.policy.phase);
        if(phase!=s.last_phase) {
            s.last_phase=phase;
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                phase,1463,s.policy.frame,(std::uint64_t(s.policy.attempts)<<32)|s.accepted_frames);
        }
    } catch(...) {draw={};}
}
void after_draw(UEVR_UGameViewportClientHandle,UEVR_FViewportHandle,UEVR_FCanvasHandle) noexcept {
    if(draw.pulse) {
        auto& s=state();std::scoped_lock lock(s.mutex);
        s.policy.finish(draw.accepted);
        if(draw.accepted)++s.accepted_frames;
        if(draw.overrode)xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
            draw.accepted?1:0,1465,s.policy.frame,s.accepted_frames);
    }
    draw={};
}
void observe(unsigned eye,const xrfg::DlssMotionVectorIdentity& id) noexcept {
    if(!active())return;
    auto& s=state();std::scoped_lock lock(s.mutex);const bool was=s.policy.ready;
    s.policy.observe(eye,id.stream,id.epoch,id.serial,id.previous,id.publication,id.reset);
    if(was!=s.policy.ready || s.observations++<8)
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
            eye|(s.policy.ready?256u:0u),1464,id.stream,id.serial);
}
} // namespace uevr_stock_bootstrap
bool uevr_stock_bootstrap_ready() noexcept {return uevr_stock_bootstrap::ready();}
