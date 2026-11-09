#pragma once
#include "RealVrRuntimeImage.h"

namespace xrfg::native_ingress::gta_runtime_binding {
struct Layout {
    std::uint32_t runtime_pointer{},runtime_vtable{};
    std::uint32_t render_offset{},yaw_offset{},mode_offset{};
    bool operator==(const Layout&) const = default;
};
// A GTA RTTI/vtable witness identifies the owner. Its camera wrapper calls
// both the per-render mode snapshot and yaw correction; those independently
// agree on the live/cached mode fields. All addresses and fields are decoded.
template<class Read>
std::optional<Layout> discover(std::uintptr_t base,Read&& read,unsigned* refusal=nullptr) noexcept {
    const auto fail=[&](unsigned why)->std::optional<Layout>{if(refusal)*refusal=why;return {};};
    try {
        using namespace real_vr_runtime_image;
        const auto loaded=load(base,read);if(!loaded)return fail(1);
        const auto& image=*loaded;
        const auto vt=vtable(image,".?AVruntime_GTAVE@@");if(!vt)return fail(2);
        std::vector<std::uint32_t> owners,constructors;
        for(const auto& s:image.sections)if(s.flags&exec)for(std::size_t i=0;i+42<=s.bytes.size();++i) {
            if(match(s.bytes,i,{0x48,0x8b,-1,-1,-1,-1,-1}) && (s.bytes[i+2]&0xc7)==5) {
                const unsigned reg=(s.bytes[i+2]>>3)&7;
                if(reg==4||reg==5)continue;
                // The loaded object's vtable is compared with the independently
                // named complete GTA type before dispatching its camera hook.
                if(!match(s.bytes,i+13,{0x48,0x85,int(0xc0|(reg<<3)|reg),0x0f,0x84,-1,-1,-1,-1,
                    0x48,0x8b,int(reg),0x48,0x8d,0x0d,-1,-1,-1,-1,
                    0x48,0x8b,0x90,-1,-1,-1,-1,0x48,0x3b,0xc1}))continue;
                const auto at=s.rva+static_cast<std::uint32_t>(i);
                const auto owner=image.relative(at,7,3),type=image.relative(at+25,7,3);
                const auto fn=function(image,at);
                if(owner&&type&&*type==*vt&&image.has(*owner,writable)&&fn&&at-(*fn)[0]<96)
                    owners.push_back(*owner);
            }
            if(match(s.bytes,i,{0x48,0x8d,0x05,-1,-1,-1,-1,0x48,0x89,-1}) &&
                (s.bytes[i+9]&0xf8)==0 && (s.bytes[i+9]&7)!=4 && (s.bytes[i+9]&7)!=5) {
                const auto at=s.rva+static_cast<std::uint32_t>(i);
                const auto type=image.relative(at,7,3);
                const auto fn=function(image,at);
                if(type&&*type==*vt&&fn)constructors.push_back(at);
            }
        }
        std::sort(owners.begin(),owners.end());owners.erase(std::unique(owners.begin(),owners.end()),owners.end());
        if(owners.size()!=1)return fail(3);
        const auto valid_field=[](std::uint32_t field,unsigned width=4){return field>=8 && field<=64*1024*1024-width && field%4==0;};
        std::vector<Layout> candidates;
        for(const auto& s:image.sections)if(s.flags&exec)for(std::size_t i=0;i+20<=s.bytes.size();++i) {
            // The native camera argument is preserved in RDX, while RCX becomes
            // this exact owner. A null owner returns without invoking it.
            if(!match(s.bytes,i,{0x48,0x8b,0xd1,0x48,0x8b,0x0d,-1,-1,-1,-1,
                0x48,0x85,0xc9,0x0f,0x85,-1,-1,-1,-1,0xc3}))continue;
            const auto at=s.rva+static_cast<std::uint32_t>(i);
            const auto owner=image.relative(at+3,7,3),handler=image.relative(at+13,6,2);
            if(!owner||*owner!=owners[0]||!handler)continue;
            const auto fn=function(image,*handler);if(!fn||(*fn)[0]!=*handler)continue;
            const auto body=image.at((*fn)[0],(*fn)[1]-(*fn)[0]);
            std::vector<std::uint32_t> callees;
            for(std::size_t j=0;j+5<=body.size();++j)if(body[j]==0xe8) {
                const auto target=image.relative((*fn)[0]+static_cast<std::uint32_t>(j),5,1);
                if(target&&image.has(*target,exec))callees.push_back(*target);
            }
            std::sort(callees.begin(),callees.end());callees.erase(std::unique(callees.begin(),callees.end()),callees.end());
            for(const auto state:callees) {
                const auto sf=function(image,state);if(!sf||(*sf)[0]!=state)continue;
                const auto sb=image.at(state,(*sf)[1]-state);
                for(std::size_t j=0;j+12<=sb.size();++j) {
                    // Snapshot effective mode into GTA's cached mode field.
                    if(!match(sb,j,{0x8b,-1,-1,-1,-1,-1,0x89,-1,-1,-1,-1,-1}) ||
                        (sb[j+1]&0xf8)!=0x80 || sb[j+7]!=sb[j+1])continue;
                    const unsigned reg=sb[j+1]&7;if(reg==4||reg==5)continue;
                    const auto mode=value<std::uint32_t>(sb,j+2),cached=value<std::uint32_t>(sb,j+8);
                    if(!valid_field(mode)||!valid_field(cached,8)||mode==cached)continue;
                    std::vector<std::uint32_t> renders;
                    for(std::size_t k=0;k+10<=std::min<std::size_t>(sb.size(),96);++k)
                        if(match(sb,k,{0x48,0x8b,0x81,-1,-1,-1,-1,0x48,0x8b,int(0xc1|(reg<<3))}))
                            renders.push_back(value<std::uint32_t>(sb,k+3));
                    if(renders.size()!=1||!valid_field(renders[0],8)||renders[0]==mode)continue;
                    // GTA constructor must initialize this exact cached pair;
                    // this links the field witness back to the named runtime.
                    unsigned initialized{};
                    for(const auto ctor:constructors) {
                        const auto cf=function(image,ctor);if(!cf)continue;
                        const auto cb=image.at(ctor,(*cf)[1]-ctor);if(cb.size()<10)continue;
                        const unsigned cr=cb[9]&7;
                        for(std::size_t k=10;k+7<=cb.size();++k)
                            if(match(cb,k,{0x48,0x89,int(0x80|cr),-1,-1,-1,-1}) && value<std::uint32_t>(cb,k+3)==cached)
                                ++initialized;
                    }
                    if(initialized!=1)continue;
                    for(const auto yaw:callees) {
                        const auto yf=function(image,yaw);if(!yf||(*yf)[0]!=yaw)continue;
                        const auto yb=image.at(yaw,(*yf)[1]-yaw);
                        for(std::size_t k=0;k+14<=yb.size();++k) {
                            // subss yaw followed by its mode flag store, using
                            // the same this-register as both mode reads below.
                            if(!match(yb,k,{0xf3,0x0f,0x5c,-1,-1,-1,-1,-1,0x88,-1,-1,-1,-1,-1}) ||
                                (yb[k+3]&0xc0)!=0x80)continue;
                            const unsigned yr=yb[k+3]&7;if(yr==4||yr==5||yb[k+9]!=(0x80|yr))continue;
                            const auto field=value<std::uint32_t>(yb,k+4);
                            if(!valid_field(field)||value<std::uint32_t>(yb,k+10)!=field+17)continue;
                            bool this_argument{},mode_read{},cached_read{};
                            for(std::size_t n=0;n+6<=yb.size();++n) {
                                if(n<128&&match(yb,n,{0x48,0x8b,int(0xc1|(yr<<3))}))this_argument=true;
                                if(yb[n]==0x8b&&(yb[n+1]&0xc7)==(0x80|yr)) {
                                    const auto offset=value<std::uint32_t>(yb,n+2);
                                    mode_read|=offset==mode;cached_read|=offset==cached;
                                }
                            }
                            if(this_argument&&mode_read&&cached_read&&field!=mode&&field!=renders[0])
                                candidates.push_back({owners[0],*vt,renders[0],field,mode});
                        }
                    }
                }
            }
        }
        if(candidates.size()!=1)return fail(candidates.empty()?4:5);
        if(refusal)*refusal=0;return candidates.front();
    }catch(...){return fail(6);}
}
}
