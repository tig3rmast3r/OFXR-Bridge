#include "xrfg/native_ingress/DaysGoneCameraSource.h"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
namespace source=xrfg::daysgone_camera_source;
namespace camera=xrfg::daysgone_camera;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class T,std::size_t N>void put(std::array<unsigned char,N>& data,std::size_t offset,const T& value) {
    check(offset<=N&&sizeof(value)<=N-offset,"fixture field overflow");std::memcpy(data.data()+offset,&value,sizeof(value));
}
std::vector<std::unique_ptr<unsigned char[]>> allocations;
void* allocate(source::Address,std::int32_t bytes) {
    auto p=std::make_unique<unsigned char[]>(bytes);auto* out=p.get();allocations.push_back(std::move(p));return out;
}
struct Queue {
    std::array<unsigned char,0x100> data{};
    Queue(){put(data,8,reinterpret_cast<source::Address>(data.data()));}
    source::Address address(){return reinterpret_cast<source::Address>(data.data());}
    source::Command* head(){return source::detail::field<source::Command*>(data,0);}
};
struct Scene {
    std::array<unsigned char,0x90> renderer{};
    std::array<unsigned char,0x2d50*2> view{};
    std::array<unsigned char,0x50> family{};
    Scene(){put(renderer,0x80,reinterpret_cast<source::Address>(view.data()));put(renderer,0x88,std::int32_t{1});}
    source::Address address(){return reinterpret_cast<source::Address>(renderer.data());}
    void frame(std::uint32_t number,float x,bool cut=false) {
        put(family,0x48,number);put(view,0,reinterpret_cast<source::Address>(family.data()));
        put(view,source::layout113.aa,std::uint8_t{3});put(view,source::layout113.state,source::Address{1});
        put(view,source::layout113.cut,std::uint8_t(cut));
        put(view,source::layout113.rect,std::array<std::int32_t,4>{0,0,960,720});
        put(view,source::layout113.unscaled,std::array<std::int32_t,4>{0,0,1920,1440});
        put(view,source::layout113.jitter,std::array<float,2>{.125f,-.25f});
        std::array<float,16> p{};p[0]=1.5f;p[5]=2;p[8]=.001f;p[9]=-.002f;p[11]=1;p[14]=10;
        const auto v=camera::store(DirectX::XMMatrixIdentity());
        put(view,source::layout113.projection,p);put(view,source::layout113.projection+0x80,v);
        put(view,source::layout113.projection+0xc0,p);
        put(view,source::layout113.projection+0x160,std::array<float,3>{x,0,0});
        put(view,source::layout113.projection+0x174,std::array<float,2>{.001f,-.002f});
    }
};
camera::Binding binding(){return {1,2,3,4,5,6,GetCurrentThreadId(),960,720,1920,1440,.125f,.25f,true};}
camera::Packet take(std::uint64_t last=0) {
    camera::Packet p;camera::Rejection reason;
    check(source::take(binding(),last,p,reason),"native RHI camera unavailable");
    xrfg::native_ingress::streamline_camera::Camera decoded;
    check(camera::decode(p,binding(),last,decoded,reason),"native receipt did not pass camera validation");return p;
}
void queue_tests() {
    source::executing={};Scene scene;Queue queue;
    scene.frame(101,1);check(source::detail::capture(scene.address(),queue.address(),&allocate),"first view did not enqueue");
    auto* first=queue.head();check(first&&first->execute,"native RHI command missing");
    scene.frame(102,2);check(source::detail::capture(scene.address(),queue.address(),&allocate),"render-ahead view did not enqueue");
    auto* second=first->next;check(second&&!second->next,"RHI linkage changed");
    first->execute(queue.address(),first);const auto a=take();
    check(a.frame==101&&a.camera.origin[0]==1&&a.flags==0,"used latest render-thread view instead of executing RHI view");
    camera::Packet duplicate;camera::Rejection reason;
    check(!source::take(binding(),0,duplicate,reason),"same view reused by another evaluation");
    second->execute(queue.address(),second);const auto b=take(a.sequence);
    check(b.frame==102&&b.camera.origin[0]==2&&(b.flags&1)&&b.previous_frame==101&&b.previous_camera.origin[0]==1,
        "exact adjacent game history lost");
    Queue cut;scene.frame(103,3,true);check(source::detail::capture(scene.address(),cut.address(),&allocate),"camera cut enqueue");
    cut.head()->execute(cut.address(),cut.head());check(take(b.sequence).flags==0,"camera cut retained old motion history");
    Queue mismatch;scene.frame(104,4);check(source::detail::capture(scene.address(),mismatch.address(),&allocate),"mismatch enqueue");
    mismatch.head()->execute(mismatch.address(),mismatch.head());auto bad=binding();bad.jitter_x=.333f;
    check(!source::take(bad,0,duplicate,reason)&&reason==camera::Rejection::jitter,"foreign jitter phase admitted");
    check(!source::take(binding(),0,duplicate,reason),"rejected evaluation left a reusable stale view");
    Queue threaded;scene.frame(105,5);check(source::detail::capture(scene.address(),threaded.address(),&allocate),"threaded view enqueue");
    bool worker_ok{};
    std::thread worker([&]{threaded.head()->execute(threaded.address(),threaded.head());camera::Packet p;camera::Rejection r;
        worker_ok=source::take(binding(),0,p,r)&&p.frame==105&&p.thread==GetCurrentThreadId();});worker.join();
    check(worker_ok,"camera did not follow the RHI execution thread");
    check(!source::take(binding(),0,duplicate,reason),"camera crossed to a different thread");
    Queue invalid;scene.frame(106,6);put(scene.view,source::layout113.projection+0xc0,std::array<float,16>{});
    check(source::detail::capture(scene.address(),invalid.address(),&allocate),"invalid-view barrier enqueue");
    invalid.head()->execute(invalid.address(),invalid.head());
    check(!source::take(binding(),0,duplicate,reason),"camera whose matrices do not match the game view admitted");
    Queue ambiguous;scene.frame(107,7);std::memcpy(scene.view.data()+source::layout113.size,scene.view.data(),source::layout113.size);
    put(scene.renderer,0x88,std::int32_t{2});check(source::detail::capture(scene.address(),ambiguous.address(),&allocate),"ambiguous barrier enqueue");
    ambiguous.head()->execute(ambiguous.address(),ambiguous.head());
    check(!source::take(binding(),0,duplicate,reason),"picked one of several eligible game views");
    Queue executing;put(executing.data,0x10,true);put(scene.renderer,0x88,std::int32_t{1});
    check(!source::detail::capture(scene.address(),executing.address(),&allocate),"modified an executing RHI list");
    check(!source::detail::disk_prefix(0x20e6920,source::visibility_prefix),"accepted an unrelated executable");
}
volatile unsigned original_calls{};
__declspec(noinline) void original_setup(source::Address,source::Address){original_calls=original_calls+1;}
void hook_chain_test(const wchar_t* path) {
    const auto module=LoadLibraryW(path);check(bool(module),"addon hook fixture failed to load");
    const auto detour=GetProcAddress(module,"DaysGoneTestDetour");
    const auto set=reinterpret_cast<void(*)(source::detail::Setup)>(GetProcAddress(module,"DaysGoneTestOriginal"));
    check(detour&&set,"addon hook fixture missing exports");
    auto existing=safetyhook::create_inline(reinterpret_cast<void*>(&original_setup),reinterpret_cast<void*>(detour));
    check(bool(existing),"existing addon hook could not be installed");set(existing.original<source::detail::Setup>());
    check(source::detail::addon_detour(reinterpret_cast<source::Address>(&original_setup)),"verified existing addon detour rejected");
    auto& h=source::detail::hooks();h.allocate=&allocate;
    h.hook=safetyhook::create_inline(reinterpret_cast<void*>(&original_setup),reinterpret_cast<void*>(&source::detail::setup),SafetyHookInline::StartDisabled);
    check(bool(h.hook),"OFXR could not chain an existing camera hook");h.original=h.hook.original<source::detail::Setup>();
    check(bool(h.hook.enable()),"OFXR camera hook enable failed");
    Scene scene;Queue queue;scene.frame(108,8);
    original_setup(scene.address(),queue.address());
    check(original_calls==1&&queue.head(),"camera hook changed or skipped the original game/addon call");
    queue.head()->execute(queue.address(),queue.head());check(take().frame==108,"chained native camera unavailable");
    h.hook={};h.original=nullptr;h.allocate=nullptr;existing={};FreeLibrary(module);
}
}
int wmain(int argc,wchar_t** argv) {
    try{check(argc==2,"expected hook fixture path");queue_tests();hook_chain_test(argv[1]);
        std::cout<<checks<<" native Days Gone source checks PASS (unmodified addon hook chain, RHI render-ahead, exact camera)\n";return 0;}
    catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
