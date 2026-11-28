#pragma once
#include "RealVrEyeLayout.h"
#include "xrfg/luke_render_identity.hpp"
#include <cmath>
#include <limits>

namespace xrfg::native_ingress::daysgone_render_binding {
struct Layout {
    std::uint32_t owner{},vtable{},matcher{},writer{},config{};
    std::uint32_t ring{},stride{},origin{},used{},selected{},adjustment{};
    std::uint32_t renderer{},compositor_owner{};
};
namespace image = real_vr_runtime_image;
template<class Read>
std::optional<Layout> discover(std::uintptr_t base,Read&& read,
    const real_vr_eye_layout::Layout* verified_eye=nullptr) noexcept {
    try {
        using namespace image;
        const auto loaded=load(base,read);if(!loaded)return {};
        const auto& pe=*loaded;
        // The eye observer patches its own verified draw/probe instructions.
        // Reuse that module's pre-hook inspection instead of requiring those
        // original bytes to remain present after our hooks are installed.
        const auto eye=verified_eye?std::optional{*verified_eye}:real_vr_eye_layout::detail::discover(pe);
        const auto vt=vtable(pe,".?AVruntime_DG@@");if(!eye||!vt)return {};
        if(!pe.has(eye->owner,writable)||vtable(pe,".?AV?$OpenXRTextureSwapChain@$0A@@@")!=eye->swapchain_vtable)return {};
        const auto is_method=[&](std::uint32_t rva){
            for(unsigned i=0;i<256;++i) {
                const auto at=pe.at(*vt+i*8,8);if(at.empty())break;
                const auto address=value<std::uint64_t>(at,0);
                if(address<base||address-base>UINT32_MAX||!pe.has(static_cast<std::uint32_t>(address-base),exec))break;
                if(address==base+rva)return true;
            }
            return false;
        };
        std::optional<Layout> result;
        for(const auto& s:pe.sections)if(s.flags&exec)for(std::size_t i=0;i+58<=s.bytes.size();++i) {
            // Native DG matcher: four-entry camera ring, unused record, exact
            // 8+4 byte origin comparison. Derive all object offsets from code.
            if(!match(s.bytes,i,{0x4c,0x69,0xc0,-1,-1,-1,-1,0x4c,0x03,0xc3,
                0x4d,0x8b,0x90,-1,-1,-1,-1,0x4d,0x85,0xd2,0x78,-1,
                0x41,0x80,0xb8,-1,-1,-1,-1,0,0x75,-1,0x48,0x8b,0x44,0x24,0x20,
                0x49,0x3b,0x80,-1,-1,-1,-1,0x75,-1,0x8b,0x44,0x24,0x28,
                0x41,0x3b,0x80,-1,-1,-1,-1,0x75,-1}))continue;
            const auto root=real_vr_eye_layout::detail::function_root(pe,s.rva+static_cast<unsigned>(i));
            if(!root||!is_method(*root))continue;
            Layout candidate;candidate.owner=eye->owner;candidate.vtable=*vt;candidate.matcher=*root;
            candidate.stride=value<std::uint32_t>(s.bytes,i+3);candidate.ring=value<std::uint32_t>(s.bytes,i+13);
            const auto used=value<std::uint32_t>(s.bytes,i+25),origin=value<std::uint32_t>(s.bytes,i+40);
            if(candidate.stride<80||candidate.stride>4096||candidate.ring>64*1024*1024||
                origin<candidate.ring||used<candidate.ring||used-candidate.ring>=candidate.stride||
                origin-candidate.ring>candidate.stride-12||value<std::uint32_t>(s.bytes,i+53)!=origin+8)continue;
            candidate.origin=origin-candidate.ring;candidate.used=used-candidate.ring;
            unsigned commits{};
            // The same native method applies the configured render adjustment
            // before exposing the selected render to the eye compositor.
            for(std::size_t j=i;j<std::min(s.bytes.size()-34,i+256);++j) {
                if(!match(s.bytes,j,{0x48,0x8b,0x05,-1,-1,-1,-1,0x48,0x63,0x88,-1,-1,-1,-1,
                    0x41,0x8d,0x42,1,0x49,0x03,0xca,0x83,0xe0,3,0x48,0x89,0x8b,-1,-1,-1,-1,0x89,0x05}))continue;
                if(real_vr_eye_layout::detail::function_root(pe,s.rva+static_cast<unsigned>(j))!=root)continue;
                const auto config=pe.relative(s.rva+static_cast<unsigned>(j),7,3);
                if(!config||!pe.has(*config,writable))continue;
                candidate.config=*config;candidate.adjustment=value<std::uint32_t>(s.bytes,j+10);
                candidate.selected=value<std::uint32_t>(s.bytes,j+27);++commits;
            }
            const auto fields=real_vr_eye_layout::selected_render_fields(eye->abi);
            if(commits!=1||candidate.adjustment>4096||candidate.selected!=fields.selected||
                candidate.ring!=fields.camera_ring||candidate.selected<candidate.ring+4*candidate.stride)continue;
            // DG submits this selected ID to its XR renderer. Its queue owner
            // is the namespace used by eye-release metadata, not runtime_DG.
            unsigned submissions{};
            for(const auto& submit:pe.sections)if(submit.flags&exec)for(std::size_t j=0;j+14<=submit.bytes.size();++j) {
                if(!match(submit.bytes,j,{0x48,0x8b,0x93,-1,-1,-1,-1,0x48,0x89,0x93,-1,-1,-1,-1})||
                    value<std::uint32_t>(submit.bytes,j+3)!=candidate.selected)continue;
                const auto method=real_vr_eye_layout::detail::function_root(pe,submit.rva+static_cast<unsigned>(j));
                if(!method||!is_method(*method))continue;
                for(std::size_t k=j+14;k+12<=std::min(submit.bytes.size(),j+128);++k) {
                    if(!match(submit.bytes,k,{0x48,0x8b,0x8b,-1,-1,-1,-1,0xe8}))continue;
                    const auto target=pe.relative(submit.rva+static_cast<unsigned>(k)+7,5,1);
                    if(!target||real_vr_eye_layout::detail::function_root(pe,submit.rva+static_cast<unsigned>(k))!=method)continue;
                    const auto fn=image::function(pe,*target);if(!fn||(*fn)[0]!=*target)continue;
                    const auto body=pe.at(*target,(*fn)[1]-*target);unsigned owners{};
                    for(std::size_t n=0;n+12<=std::min(body.size(),std::size_t(512));++n) {
                        // Same renderer passed in RCX, retained in R12, then
                        // dereferenced to obtain the compositor queue object.
                        if(!match(body,n,{0x49,0x8b,0x94,0x24,-1,-1,-1,-1,0x48,0x89,0x55,0x80}))continue;
                        const auto field=value<std::uint32_t>(body,n+4);
                        if(field!=(eye->abi==real_vr_eye_layout::Abi::current?0x15b0U:0x14e8U))continue;
                        candidate.compositor_owner=field;++owners;
                    }
                    if(owners==1){candidate.renderer=value<std::uint32_t>(submit.bytes,k+3);++submissions;}
                }
            }
            if(submissions!=1||candidate.renderer>64*1024*1024)continue;
            // Independent producer witness: copy the game's XYZ, clear the
            // used bit, then atomically commit this same ring record's ID.
            unsigned writers{};
            for(const auto& w:pe.sections)if(w.flags&exec)for(std::size_t j=0;j+45<=w.bytes.size();++j) {
                if(!match(w.bytes,j,{0x8b,7,0x89,0x84,0x19,-1,-1,-1,-1,0x8b,0x47,4,
                    0x89,0x84,0x19,-1,-1,-1,-1,0x8b,0x47,8,0x89,0x84,0x19,-1,-1,-1,-1,
                    0xc6,0x84,0x19,-1,-1,-1,-1,0,0x48,0x87,0xb4,0x19,-1,-1,-1,-1}))continue;
                if(value<std::uint32_t>(w.bytes,j+5)!=origin||value<std::uint32_t>(w.bytes,j+15)!=origin+4||
                    value<std::uint32_t>(w.bytes,j+25)!=origin+8||value<std::uint32_t>(w.bytes,j+32)!=used||
                    value<std::uint32_t>(w.bytes,j+41)!=candidate.ring)continue;
                const auto writer=real_vr_eye_layout::detail::function_root(pe,w.rva+static_cast<unsigned>(j));
                // Several title callbacks share this ring format. The active
                // DG vtable and its matcher identify the title; these stores
                // independently witness the atomic producer commit protocol.
                if(writer){if(!writers)candidate.writer=*writer;++writers;}
            }
            if(!writers)continue;
            if(result)return {};result=candidate;
        }
        return result;
    }catch(...){return {};}
}

// Read-only native DG origin match. No newest/oldest choice: ambiguity remains
// unavailable. The selected field is optional while DLSS is executing.
template<class Read>
std::optional<LukeRenderIdentity> read_identity(std::uintptr_t base,const Layout& l,
    const std::array<float,3>& origin,Read&& read,unsigned* rejection=nullptr) noexcept {
    const auto fail=[&](unsigned reason)->std::optional<LukeRenderIdentity>{if(rejection)*rejection=reason;return {};};
    if(rejection)*rejection=0;
    for(float v:origin)if(!std::isfinite(v))return fail(1);
    const auto get=[&](std::uintptr_t address,auto& value){return read(address,&value,sizeof(value));};
    std::uint64_t owner{},vt{},config{},selected{},renderer{},compositor{};std::int32_t adjustment{};
    if(!get(base+l.owner,owner)||!owner||!get(owner,vt)||vt!=base+l.vtable)return fail(2);
    if(!get(base+l.config,config)||!config||!get(config+l.adjustment,adjustment)||!get(owner+l.selected,selected))return fail(3);
    if(!get(owner+l.renderer,renderer)||!renderer||!get(renderer+l.compositor_owner,compositor)||!compositor)return fail(7);
    struct Entry {std::uint64_t render{};std::array<float,3> origin{};unsigned char used{};};
    const auto entry=[&](unsigned slot,Entry& e){
        const auto at=owner+l.ring+slot*l.stride;
        return get(at,e.render)&&read(at+l.origin,e.origin.data(),sizeof(e.origin))&&get(at+l.used,e.used);
    };
    const auto adjusted=[&](std::uint64_t raw)->std::optional<std::uint64_t>{
        if(raw>std::uint64_t(INT64_MAX))return {};
        if(adjustment<0&&raw<std::uint64_t(-std::int64_t(adjustment)))return {};
        if(adjustment>0&&raw>std::uint64_t(INT64_MAX)-adjustment)return {};
        return raw+std::int64_t(adjustment);
    };
    std::array<Entry,4> records;int candidate=-1;std::uint64_t render{};
    for(unsigned slot=0;slot<4;++slot) {
        if(!entry(slot,records[slot]))return fail(3);
        const auto& e=records[slot];const auto id=adjusted(e.render);
        if(!id||(e.render&3)!=slot||std::memcmp(e.origin.data(),origin.data(),sizeof(origin)))continue;
        if(!e.used) {if(candidate>=0)return fail(5);candidate=slot;render=*id;}
    }
    if(candidate<0&&selected<std::uint64_t(INT64_MAX)) {
        for(unsigned slot=0;slot<4;++slot) {
            const auto& e=records[slot];const auto id=adjusted(e.render);
            if(id&&*id==selected&&(e.render&3)==slot&&!std::memcmp(e.origin.data(),origin.data(),sizeof(origin))) {
                if(candidate>=0)return fail(5);candidate=slot;render=*id;
            }
        }
    }
    if(candidate<0)return fail(4);
    Entry after;std::uint64_t final_owner{},final_vt{},final_config{},final_renderer{},final_compositor{};std::int32_t final_adjustment{};
    const auto& before=records[candidate];
    if(!entry(candidate,after)||after.render!=before.render||after.used!=before.used||
        std::memcmp(after.origin.data(),origin.data(),sizeof(origin))||
        !get(base+l.owner,final_owner)||final_owner!=owner||!get(owner,final_vt)||final_vt!=vt||
        !get(base+l.config,final_config)||final_config!=config||
        !get(config+l.adjustment,final_adjustment)||final_adjustment!=adjustment||
        !get(owner+l.renderer,final_renderer)||final_renderer!=renderer||
        !get(renderer+l.compositor_owner,final_compositor)||final_compositor!=compositor)return fail(6);
    return LukeRenderIdentity{compositor,render};
}
}
