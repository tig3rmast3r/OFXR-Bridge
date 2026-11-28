#include "uevr_stock_code_fixture.hpp"
static int uevr_stock_bootstrap_test() {
    using Policy=xrfg::UevrStockBootstrap;
    {
        // Exact failed run: AFR/Very Late/Ghosting Fix, saved native fix ON.
        // UEVR ignores the native toggle in AFR; it must not block bootstrap.
        using Pair=std::pair<std::string_view,std::string_view>;
        std::array<Pair,7> settings{{{"VR_RenderingMethod","2"},{"VR_SynchronizationMode","2"},
            {"VR_GhostingFix","true"},{"VR_NativeStereoFix","true"},
            {"VR_Compatibility_SplitScreen","false"},{"VR_Compatibility_SceneView","false"},
            {"VR_GhostingFixBootstrapViewStates",""}}};
        const auto read=[&](std::string_view key) {
            check(key!="VR_NativeStereoFix","dormant native stereo toggle consulted in AFR");
            for(const auto& [name,value]:settings)if(name==key)return value;
            check(false,"bootstrap read an unknown stock UEVR setting");return std::string_view{};
        };
        check(!xrfg::uevr_stock_bootstrap_setting_reasons(read),"reported AFR settings blocked bootstrap");
        for(const auto& [index,value,reason]:std::array<std::tuple<unsigned,std::string_view,unsigned>,6>{{
            {0,"0",1},{1,"1",2},{2,"false",4},{4,"true",8},{5,"true",16},{6,"false",32}}}) {
            const auto saved=settings[index].second;settings[index].second=value;
            check(xrfg::uevr_stock_bootstrap_setting_reasons(read)==reason,"stock setting guard or blocker code changed");
            settings[index].second=saved;
        }
    }
    #if defined(_WIN64)
    check(xrfg::uevr_stock_discovery::supports_sdk(2,39) && xrfg::uevr_stock_discovery::supports_sdk(2,41),"compatible stock SDK minor rejected");
    check(xrfg::uevr_stock_discovery::supports_sdk(2,34) && !xrfg::uevr_stock_discovery::supports_sdk(3,39) &&
        !xrfg::uevr_stock_discovery::supports_sdk(2,33),"SDK compatibility floor changed");
#endif
    const std::array<std::uintptr_t,3> scene{1,2,3};
    Policy p;
    for(unsigned i=1;i<=11;++i)check(!p.begin(scene,true),"bootstrap ran before twelve stable frames");
    check(p.begin(scene,true) && p.attempts==1,"stable scene did not request first pulse");
    check(p.begin(scene,true),"bootstrap pulse must include two engine frames");
    for(unsigned i=0;i<30;++i)check(!p.begin(scene,true),"bootstrap retried before cooldown");
    check(p.begin(scene,true) && p.attempts==2,"bounded retry was not rearmed");
    p.begin(scene,true);
    for(unsigned i=0;i<30;++i)p.begin(scene,true);
    check(p.begin(scene,true) && p.attempts==3,"third attempt missing");p.begin(scene,true);
    for(unsigned i=0;i<100;++i)check(!p.begin(scene,true),"bootstrap exceeded attempt budget");
    check(p.phase==Policy::Phase::exhausted,"exhaustion not observable");
    check(!p.begin({4,5,6},true) && !p.attempts,"new scene inherited exhausted budget");
    p.observe(0,10,1,1,0,1,false);p.observe(1,10,1,2,1,2,false);
    p.observe(0,10,1,3,2,3,false);p.observe(1,10,1,4,3,4,false);
    check(!p.ready,"shared feature was mistaken for separated eye histories");
    p.observe(0,10,1,5,4,5,false);p.observe(1,20,1,1,0,6,false);
    p.observe(0,10,1,6,5,7,false);p.observe(1,20,1,2,1,8,false);
    check(p.ready && !p.begin({4,5,6},true),"verified same-eye chains did not stop pulses");
    p.observe(0,10,2,1,0,9,true);
    check(!p.ready && !p.attempts,"stream reset kept old readiness or exhausted budget");
    check(!p.begin(scene,false) && !p.enabled,"disabled reconstruction still bootstrapped");
    Policy warming;
    for(unsigned i=0;i<12;++i)warming.begin(scene,true);
    for(unsigned i=0;i<90;++i) {
        warming.finish(false);check(warming.begin(scene,true),"native readiness probe stopped before hooks became ready");
        check(warming.attempts==1,"unready native checks exhausted construction attempts");
    }
    warming.finish(true);check(warming.begin(scene,true),"ready first view did not retain second construction frame");
    warming.finish(true);
    for(unsigned i=0;i<30;++i)check(!warming.begin(scene,true),"late readiness lost retry cooldown");
    check(warming.begin(scene,true) && warming.attempts==2,"late readiness did not resume bounded construction attempts");
    Policy never_ready;
    for(unsigned i=0;i<650;++i){const bool pulse=never_ready.begin(scene,true);if(pulse)never_ready.finish(false);}
    check(never_ready.phase==Policy::Phase::exhausted,"unavailable native hooks produced an endless probe loop");
    for(std::uintptr_t flags:{0x202u,0x246u,0xa92u}) {
        check(xrfg::uevr_stock_bootstrap_flags(flags,false)==flags,"inactive hook changed flags");
        check((xrfg::uevr_stock_bootstrap_flags(flags,true)^flags)<=1,"hook changed more than CF");
    }
#if defined(_WIN64)
    // Execute the ORIGINAL bytes from the official binary's CMP/JAE and
    // readiness/return tail. Only the entry prologue is a test harness. This
    // proves the real mid-hook preserves every subsequent readiness gate.
    auto* code=static_cast<std::uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    check(code!=nullptr,"CPU hook fixture allocation");
    struct Release {void* p;~Release(){VirtualFree(p,0,MEM_RELEASE);}} release{code};
    std::memset(code,0xcc,4096);
    constexpr std::uint8_t prologue[]{0x53,0x41,0x56,0x48,0x81,0xec,0xe8,0,0,0,0xe9,0x99,0,0,0};
    std::memcpy(code,prologue,sizeof(prologue)); // jmp to copied CMP at+a8
    std::memcpy(code+0x75,xrfg::uevr_stock_fixture::test_tail.data(),xrfg::uevr_stock_fixture::test_tail.size());
    DWORD old{};check(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old)!=0,"protect CPU fixture");
    FlushInstructionCache(GetCurrentProcess(),code,4096);
    using Function=unsigned(*)(void*);const auto fn=reinterpret_cast<Function>(code);
    alignas(16) std::array<std::byte,0x2000> state{};
    const auto write=[&](unsigned offset,std::uint64_t v){std::memcpy(state.data()+offset,&v,sizeof(v));};
    const auto fixed=[&](bool value){state[0x1d26]=std::byte(value);};
    const auto extensions=[&](bool value){state[0x1d51]=std::byte(value);};
    write(0x170,3);fixed(true);write(0xb8,1);write(0xc0,1);extensions(true);
    check(fn(state.data())==1,"unmodified official code must reproduce global-count refusal");
    auto accepted=safetyhook::create_mid(code+0xdc,uevr_stock_bootstrap::accepted);
    auto gate=safetyhook::create_mid(code+0xb0,uevr_stock_bootstrap::gate);
    check(bool(accepted) && bool(gate),"real mid-hook creation failed");
    auto& draw=uevr_stock_bootstrap::draw;
    draw={};check(fn(state.data())==1,"inactive hook changed stock behavior");
    draw={true};check(fn(state.data())==2 && draw.overrode && draw.accepted,
        "active stock hook did not request two views through the original readiness gates");
    for(unsigned absent=0;absent<4;++absent) {
        fixed(absent!=0);write(0xb8,absent==1?0:1);write(0xc0,absent==2?0:1);extensions(absent!=3);
        draw={true};check(fn(state.data())==1 && !draw.accepted,"hook bypassed UEVR readiness validation");
    }
    fixed(true);write(0xb8,1);write(0xc0,1);extensions(true);write(0x170,1);
    draw={};check(fn(state.data())==2,"already eligible stock behavior changed");
    draw={};
#endif
    // Native AER learning must not retain the initial shared-stream identity
    // hints. Exercise the shipping release hook; it performs no GPU work here.
    {
        auto& stock=uevr_stock_bootstrap::state();
        const auto saved=stock.policy;
        const bool installed=stock.installed.load(),requested=stock.requested.load(),callbacks=stock.callbacks_ready.load();
        std::array<XrSwapchain,2> handles{reinterpret_cast<XrSwapchain>(0xafb0),reinterpret_cast<XrSwapchain>(0xafb1)};
        struct Cleanup {
            uevr_stock_bootstrap::State& stock;Policy policy;bool installed,requested,callbacks;std::array<XrSwapchain,2> handles;
            ~Cleanup(){stock.policy=policy;stock.installed=installed;stock.requested=requested;stock.callbacks_ready=callbacks;
                std::scoped_lock lock(g_state_mutex);for(auto h:handles)g_swapchains.erase(h);}
        } cleanup{stock,saved,installed,requested,callbacks,handles};
        stock.installed=true;stock.requested=true;stock.callbacks_ready=true;stock.policy={};stock.policy.begin(scene,true);
        auto session=std::make_shared<SessionState>(std::make_shared<Dispatch>());
        session->uevr_afr=std::make_shared<UevrAfrOsrState>();auto& afr=*session->uevr_afr;
        session->graphics_binding=SessionGraphicsBinding::d3d12;session->menu_enabled=true;
        session->nvidia_options.ofr=true;session->hybrid_requested=true;
        XrFrameEndInfo loading{XR_TYPE_FRAME_END_INFO};UevrEndFrameRewrite rewritten;
        uevr_afr_observe_end_frame(*session,&loading,rewritten);
        check(!stock.requested && !stock.policy.enabled,"loading without a stereo layout consumed bootstrap attempts");
        stock.requested=true;stock.policy.begin(scene,true);
        afr.layout=true;afr.eye_swapchain=handles;
        std::array<std::shared_ptr<SwapchainState>,2> chains;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};info.width=info.height=info.arraySize=1;
        for(unsigned eye=0;eye<2;++eye){chains[eye]=std::make_shared<SwapchainState>(session,info);chains[eye]->handle=handles[eye];
            chains[eye]->fsr_eye_identity={true,99,1};std::scoped_lock lock(g_state_mutex);g_swapchains[handles[eye]]=chains[eye];}
        check(!uevr_stock_bootstrap_ready(),"unverified stock histories were admitted");
        uevr_afr_before_release(chains[0]);
        check(!chains[0]->fsr_eye_identity.stream && !chains[1]->fsr_eye_identity.stream,
            "native AER pinned its initial shared feature across bootstrap");
        for(unsigned i=0;i<4;++i)stock.policy.observe(i%2,10+i%2,1,1+i/2,i/2,1+i,false);
        check(uevr_stock_bootstrap_ready(),"distinct native chains not admitted");
        chains[0]->fsr_eye_identity={true,10,1};chains[1]->fsr_eye_identity={true,11,1};
        uevr_afr_before_release(chains[0]);
        check(chains[0]->fsr_eye_identity.stream==10 && chains[1]->fsr_eye_identity.stream==11,
            "ready stock AER kept clearing verified eye identities");
    }
    std::puts("STOCK_UEVR_BOOTSTRAP official-code rejection/control, real mid-hooks, all readiness gates, bounded lifecycle and temporal identity PASS");
    return 0;
}
