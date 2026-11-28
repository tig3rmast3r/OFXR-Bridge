// Included after uevr_api(). Only the sealed upstream binary is eligible.
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

bool module_hash(HMODULE module) {
    wchar_t path[32768]{};
    const auto n=GetModuleFileNameW(module,path,std::size(path));
    if(!n || n==std::size(path))return false;
    std::ifstream file(std::filesystem::path(path),std::ios::binary);
    if(!file)return false;
    struct Crypto {
        BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};std::vector<UCHAR> object;
        ~Crypto(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
    } crypto;
    if(BCryptOpenAlgorithmProvider(&crypto.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    DWORD size{},bytes{};
    if(BCryptGetProperty(crypto.algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&bytes,0)<0)return false;
    crypto.object.resize(size);
    if(BCryptCreateHash(crypto.algorithm,&crypto.hash,crypto.object.data(),size,nullptr,0,0)<0)return false;
    std::array<char,65536> buffer{};
    while(file) {
        file.read(buffer.data(),buffer.size());const auto count=file.gcount();
        if(count && BCryptHashData(crypto.hash,reinterpret_cast<PUCHAR>(buffer.data()),ULONG(count),0)<0)return false;
    }
    if(!file.eof())return false;
    std::array<UCHAR,32> digest{};
    if(BCryptFinishHash(crypto.hash,digest.data(),digest.size(),0)<0)return false;
    std::array<char,64> hex{};constexpr char digits[]="0123456789abcdef";
    for(unsigned i=0;i<digest.size();++i){hex[i*2]=digits[digest[i]>>4];hex[i*2+1]=digits[digest[i]&15];}
    return std::string_view(hex.data(),hex.size())==xrfg::uevr_stock_profile::sha256;
}
template<std::size_t N>
bool code_matches(std::uintptr_t address,const std::array<std::uint8_t,N>& expected) noexcept {
    std::array<std::uint8_t,N> bytes{};SIZE_T read{};
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),bytes.data(),N,&read) &&
        read==N && bytes==expected;
}
void install() noexcept {
#if defined(_WIN64)
    try {
        const auto* api=uevr_api();
        // Reject fork/unknown SDKs before file IO or any hook creation.
        if(!api || api->version->minor!=39 || !api->functions || !api->functions->get_commit_hash)return;
        const auto* commit=api->functions->get_commit_hash();
        if(!commit || !xrfg::uevr_stock_profile::supports_sdk(api->version->major,api->version->minor,commit))return;
        const auto module=GetModuleHandleW(L"UEVRBackend.dll");if(!module)return;
        auto& s=state();std::scoped_lock lock(s.mutex);
        if(s.attempted==module)return;s.attempted=module;
        const auto report=[](int reason){xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,reason,1462);};
        if(!module_hash(module)){report(2);return;}
        const auto base=reinterpret_cast<std::uintptr_t>(module);
        namespace p=xrfg::uevr_stock_profile;
        std::array<std::uint8_t,p::function_size> body{};SIZE_T read{};
        if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(base+p::entry),body.data(),body.size(),&read) ||
            read!=body.size() || p::digest(body.data(),body.size())!=p::function_digest){report(3);return;}
        if(!code_matches(base+p::entry,p::entry_bytes) || !code_matches(base+p::guard_start,p::guard_bytes) ||
            !code_matches(base+p::accepted,p::accepted_bytes)){report(3);return;}
        auto a=safetyhook::create_mid(reinterpret_cast<void*>(base+p::gate),gate,SafetyHookMid::StartDisabled);
        auto b=safetyhook::create_mid(reinterpret_cast<void*>(base+p::accepted),accepted,SafetyHookMid::StartDisabled);
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
