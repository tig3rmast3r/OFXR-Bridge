#pragma once
// Observes the original native RAGE camera without changing game state.
// Discover the RDR2 runtime through RTTI and linked camera code, then verify
// the live owner and exact GPU-selected camera at every observation.
#include <windows.h>
#include <safetyhook.hpp>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include "xrfg/native_ingress/Rdr2CameraPacket.h"
#include "xrfg/native_ingress/Rdr2RuntimeBinding.h"

namespace xrfg::native_vulkan::rdr2_camera_probe {
using Report=void(*)(unsigned,std::int64_t,std::uint64_t,std::uint64_t) noexcept;
struct Sample {std::uint64_t render{},owner{},sequence{};std::array<unsigned char,256> camera{};
    DWORD thread{};ULONGLONG captured_ms{};bool valid{};
    std::uint64_t camera_qpc{},camera_frequency{};};
inline thread_local Sample main_camera;
struct State {
    std::mutex mutex;
    std::array<Sample,2> samples{};
    std::array<std::array<Sample,2>,32> history{};
    SafetyHookInline hook;
    using Call=void(*)(void*,bool,void*);
    std::atomic<Call> original{};
    std::uintptr_t module{};
    std::uintptr_t attempted_module{};
    native_ingress::rdr2_runtime_binding::Layout layout{};
    std::atomic_uint64_t calls{},next_report{};
    std::atomic_uint reports{};
    std::atomic_uint refusal{};
    std::atomic_uint64_t selected_render{};
};
inline State& state(){static auto* s=new State;return *s;}
inline bool read(std::uintptr_t address,void* destination,std::size_t size) noexcept {
    SIZE_T got{};return address>=0x10000&&ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const void*>(address),destination,size,&got)&&got==size;
}
template<class T>bool read(std::uintptr_t address,T& value) noexcept {return read(address,&value,sizeof(value));}
inline void camera_call(void* owner,bool gun,void* camera) {
    auto& s=state();s.original.load(std::memory_order_acquire)(owner,gun,camera);
    LARGE_INTEGER stamp{};QueryPerformanceCounter(&stamp);
    static const auto frequency=[] {LARGE_INTEGER v{};QueryPerformanceFrequency(&v);return std::uint64_t(v.QuadPart);}();
    const auto sequence=s.calls.fetch_add(1,std::memory_order_relaxed)+1;
    const auto object=reinterpret_cast<std::uintptr_t>(owner);
    std::uint64_t selected{},vtable{},after_owner{},after_vtable{},after_render{};
    Sample sample{};sample.owner=object;sample.sequence=sequence;
    sample.thread=GetCurrentThreadId();sample.captured_ms=GetTickCount64();
    sample.camera_qpc=std::uint64_t(stamp.QuadPart);sample.camera_frequency=frequency;
    std::array<unsigned char,256> verified{};
    sample.valid=read(s.module+s.layout.owner,selected)&&selected==object&&
        read(object,vtable)&&vtable==s.module+s.layout.vtable&&
        read(object+s.layout.render,sample.render)&&sample.render<=INT64_MAX&&
        read(reinterpret_cast<std::uintptr_t>(camera),sample.camera)&&
        read(reinterpret_cast<std::uintptr_t>(camera),verified)&&verified==sample.camera&&
        read(object+s.layout.render,after_render)&&after_render==sample.render&&
        read(object,after_vtable)&&after_vtable==vtable&&
        read(s.module+s.layout.owner,after_owner)&&after_owner==object;
    if(!gun)main_camera=sample;
    std::scoped_lock lock(s.mutex);s.samples[gun?1:0]=sample;
    if(sample.valid)s.history[sample.render%s.history.size()][gun?1:0]=sample;
}
// REAL VR matches a GPU-uploaded native RAGE view matrix against its four
// camera records, then stores the selected render. That identity, rather than
// the advancing game-thread counter, identifies the worker's source. Retain packets
// by identity; never substitute a nearest or globally latest camera.
inline Sample take() noexcept {
    auto& s=state();
    const auto fail=[&](unsigned why){s.refusal.store(why,std::memory_order_relaxed);return Sample{};};
    if(!s.original.load(std::memory_order_acquire))return fail(1);
    std::uint64_t owner{},vtable{},render{},after_owner{},after_render{},ring_render{},after_ring_render{};
    if(!read(s.module+s.layout.owner,owner)||!read(owner,vtable)||vtable!=s.module+s.layout.vtable||
        !read(owner+s.layout.selected,render)||render>UINT32_MAX)return fail(4);
    s.selected_render.store(render,std::memory_order_relaxed);
    const auto slot=owner+s.layout.ring+(render&3)*s.layout.stride;
    std::array<unsigned char,48> matrix{},verified{};
    if(!read(slot,ring_render)||ring_render!=render||!read(slot+s.layout.matrix,matrix)||
        !read(slot+s.layout.matrix,verified)||verified!=matrix)return fail(5);
    std::array<Sample,2> candidates;
    {std::scoped_lock lock(s.mutex);candidates=s.history[render%s.history.size()];}
    Sample selected;
    for(const auto& sample:candidates) {
        if(!sample.valid||sample.owner!=owner||sample.render!=render)continue;
        if(GetTickCount64()-sample.captured_ms>250)continue;
        if(!native_ingress::rdr2_camera_packet::matches_native_view(sample.camera.data(),sample.camera.size(),matrix))continue;
        if(selected.valid)for(unsigned at=96;at<112;at+=4) {
            const auto a=native_ingress::real_vr_dlss_camera::read_float(selected.camera.data(),at);
            const auto b=native_ingress::real_vr_dlss_camera::read_float(sample.camera.data(),at);
            if(!std::isfinite(a)||!std::isfinite(b)||std::abs(a-b)>1e-5F*std::max(1.F,std::abs(a)))return fail(6);
        }
        // Prefer the matching main camera over a later identical gun callback.
        if(!selected.valid)selected=sample;
    }
    if(!selected.valid)return fail(2);
    if(!read(slot,after_ring_render)||after_ring_render!=render||
        !read(owner+s.layout.selected,after_render)||after_render!=render||
        !read(s.module+s.layout.owner,after_owner)||after_owner!=owner)return fail(4);
    s.refusal.store(0,std::memory_order_relaxed);return selected;
}
inline void poll(Report report) noexcept {
    try {
        auto& s=state();if(s.original)return;
        wchar_t path[32768]{};
        if(!GetModuleFileNameW(nullptr,path,32768))return;
        auto* slash=wcsrchr(path,L'\\');if(!slash||_wcsicmp(slash+1,L"RDR2.exe"))return;
        // RDR2's launcher injects the shared ProgramData module directly.
        // Unlike GTA, it has no game-local dxgi.dll alias. Select an already
        // loaded module; exports, RTTI and linked code witnesses prove it.
        const auto module=GetModuleHandleW(L"RealVR64.dll");
        if(!module||!GetProcAddress(module,"RealVRVersion")||!GetProcAddress(module,"g_rvrShared"))return;
        const auto base=reinterpret_cast<std::uintptr_t>(module);
        std::scoped_lock lock(s.mutex);if(s.original||s.attempted_module==base)return;
        s.attempted_module=base;unsigned refusal{};
        const auto layout=native_ingress::rdr2_runtime_binding::discover(base,
            [](auto address,void* bytes,std::size_t size){return read(address,bytes,size);},&refusal);
        if(!layout){if(report)report(1368,-static_cast<std::int64_t>(refusal),base,0);return;}
        if(report){report(1368,1,layout->owner,layout->vtable);
            report(1369,1,layout->callback,layout->matcher);}
        auto hook=safetyhook::create_inline(reinterpret_cast<void*>(base+layout->callback),
            reinterpret_cast<void*>(&camera_call),SafetyHookInline::StartDisabled);
        if(!hook){if(report)report(1342,-1,base,0);return;}
        HMODULE retained{};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(base),&retained))return;
        s.module=base;s.layout=*layout;s.original=hook.original<State::Call>();s.hook=std::move(hook);
        if(!s.hook.enable()){s.original=nullptr;s.hook={};if(report)report(1342,-2,base,0);return;}
        if(report)report(1342,1,base,base+layout->callback);
    }catch(...) {}
}
inline void report_entry(Report report) noexcept {
    if(!report)return;
    auto& s=state();const auto now=GetTickCount64();auto due=s.next_report.load(std::memory_order_relaxed);
    if(now<due||
        !s.next_report.compare_exchange_strong(due,now+1000,std::memory_order_relaxed))return;
    ++s.reports;
    std::array<Sample,2> samples;
    {std::scoped_lock lock(s.mutex);samples=s.samples;}
    report(1343,s.original?1:0,s.calls.load(std::memory_order_relaxed),
        (std::uint64_t(samples[0].thread)<<32)|samples[1].thread);
    report(1344,s.refusal.load(std::memory_order_relaxed),GetCurrentThreadId(),s.selected_render.load(std::memory_order_relaxed));
    for(unsigned role=0;role<samples.size();++role) {
        const auto& sample=samples[role];report(1340,role|(sample.valid?256:0),sample.render,sample.owner);
        if(!sample.valid)continue;
        for(unsigned at=0;at<sample.camera.size();at+=16) {
            std::uint64_t first{},second{};std::memcpy(&first,sample.camera.data()+at,8);
            std::memcpy(&second,sample.camera.data()+at+8,8);
            report(1341,role*64+at/16,first,second);
        }
    }
}
}
