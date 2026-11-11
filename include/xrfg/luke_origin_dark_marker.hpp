#pragma once
#include "xrfg/luke_color_origin.hpp"
#include <limits>
namespace xrfg {
struct LukeDarkMarker {bool recognized{};unsigned x{},y{},width{},height{};};
inline float luke_origin_peak(const std::byte* p,DXGI_FORMAT f) noexcept {
    const auto byte=[&](unsigned i){return std::to_integer<unsigned char>(p[i])/255.f;};
    switch(f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return std::max({byte(0),byte(1),byte(2)});
    case DXGI_FORMAT_R10G10B10A2_UNORM: {
        std::uint32_t v{};std::memcpy(&v,p,4);return std::max({v&1023,(v>>10)&1023,v>>20&1023})/1023.f;
    }
    case DXGI_FORMAT_R16G16B16A16_UNORM: {
        std::array<std::uint16_t,4> v{};std::memcpy(v.data(),p,8);return std::max({v[0],v[1],v[2]})/65535.f;
    }
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        std::array<std::uint16_t,4> v{};std::memcpy(v.data(),p,8);
        if((v[0]|v[1]|v[2])&0x8000)return INFINITY;
        const auto m=std::max({v[0],v[1],v[2]});v[0]=v[1]=v[2]=m;
        return luke_origin_luma(reinterpret_cast<const std::byte*>(v.data()),f);
    }
    case DXGI_FORMAT_R32G32B32A32_FLOAT: {
        std::array<float,4> v{};std::memcpy(v.data(),p,16);
        for(unsigned i=0;i<3;++i)if(!std::isfinite(v[i])||v[i]<0)return INFINITY;
        return std::max({v[0],v[1],v[2]});
    }
    default:return INFINITY; // Unknown formats cannot qualify initial adaptation.
    }
}
// Recognize a very dark, complete barcode footprint, rather than treating the
// whole read ROI/scene being black as marker evidence. Conservative bootstrap:
// localized rectangle + intact weak Manchester sync on >=3 adjacent rows.
// A fully erased initial rectangle has no identity evidence and stays unknown.
inline LukeDarkMarker classify_luke_dark_marker(const LukeColorOriginImage& image,
    const LukeColorOriginDecode* known=nullptr) noexcept {
    const auto bytes=luke_origin_pixel_bytes(image.format);
    if(!image.pixels||!bytes||image.width<kLukeOriginWidth||image.height<8||
       image.width>16384||image.height>8192||image.row_pitch<std::uint64_t(image.width)*bytes)return {};
    constexpr float dark=2.f/255.f, surround=8.f/255.f;
    const auto pixel=[&](unsigned x,unsigned y){return image.pixels+std::size_t(y)*image.row_pitch+x*bytes;};
    const auto peak=[&](unsigned x,unsigned y){return luke_origin_peak(pixel(x,y),image.format);};
    const auto sync=[&](unsigned left,unsigned width,unsigned y) {
        // Ignore the resampled fringe. Cells are sampled at their centres;
        // lower contrast is accepted for CLASSIFICATION ONLY, never identity.
        const double cell=double(width)/128.;
        for(unsigned bit=0;bit<16;++bit) {
            const unsigned a=left+unsigned((2*bit+.5)*cell),b=left+unsigned((2*bit+1.5)*cell);
            if(b>=image.width)return false;
            const auto* pa=pixel(a,y);const auto* pb=pixel(b,y);
            float d=luke_origin_luma(pa,image.format)-luke_origin_luma(pb,image.format);
            // Blue-only residual can be invisible to the normal luma threshold.
            const bool expected=((0xd3a5U>>(15-bit))&1)!=0;
            if(!std::isfinite(d)||std::abs(d)<1.f/65536.f||(d>0)!=expected)return false;
        }
        return true;
    };
    for(unsigned y=1;y+1<image.height;++y) {
        unsigned x=1;
        while(x+1<image.width) {
            if(!(peak(x,y)<=dark)){++x;continue;}
            const unsigned left=x;while(x+1<image.width&&peak(x,y)<=dark)++x;
            const unsigned width=x-left;
            // A clipped/wrongly scaled/large black patch is not this case.
            if(width<kLukeOriginWidth-4||width>kLukeOriginWidth+4||left<2||x+2>=image.width)continue;
            const unsigned middle=left+width/2;
            unsigned top=y,bottom=y+1;
            while(top>0&&peak(middle,top-1)<=dark)--top;
            while(bottom<image.height&&peak(middle,bottom)<=dark)++bottom;
            const unsigned height=bottom-top;
            if(top<2||bottom+1>=image.height||height<5||height>10)continue;
            // Require surrounding scene on all four sides, not a black bar,
            // all-black frame, a cut-off band or a partial occlusion.
            bool frame=true;unsigned good_sync=0;
            for(unsigned yy=top+1;yy+1<bottom;++yy) {
                if(!(peak(left-2,yy)>=surround)||!(peak(x+2,yy)>=surround)){frame=false;break;}
                bool inside=true;
                for(unsigned xx=left+2;xx+2<x;++xx)if(!(peak(xx,yy)<=dark)){inside=false;break;}
                if(!inside){frame=false;break;}
                // One fringe pixel may be excluded from each end of the run.
                good_sync+=sync(left-1,width+1,yy)||sync(left,width,yy)||sync(left-1,width+2,yy);
            }
            if(!frame)continue;
            for(unsigned xx=left+3;xx+3<x;xx+=8)
                if(!(peak(xx,top-2)>=surround)||!(peak(xx,bottom+1)>=surround)){frame=false;break;}
            bool at_known=false;
            if(known && known->token && !known->ambiguous && known->first_column>=0&&known->first_row>=0)
                at_known=std::abs(int(left)-known->first_column)<=4 &&
                    std::abs(int(top)-known->first_row)<=3 &&
                    std::abs(int(x)-known->last_column)<=4 && std::abs(int(bottom)-known->last_row-1)<=3;
            if(frame&&(good_sync>=3||at_known))return {true,left,top,width,height};
        }
    }
    return {};
}
} // namespace xrfg
