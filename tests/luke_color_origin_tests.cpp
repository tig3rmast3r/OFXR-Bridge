
#include "xrfg/luke_color_origin.hpp"
#include "xrfg/luke_origin_inset.hpp"
#include "xrfg/luke_afw_source.hpp"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <limits>

namespace {
using Microsoft::WRL::ComPtr;
std::atomic_uint checks{};
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void check(HRESULT result, const char* message) { require(SUCCEEDED(result), message); }
void stamp(std::vector<float>& row, double start, double scale, std::uint32_t token) {
    const auto original = [&](int x) {
        return x >= 0 && x < static_cast<int>(xrfg::kLukeOriginWidth)
            ? (xrfg::luke_origin_white(token, static_cast<unsigned>(x)) ? 1. : 0.) : .5;
    };
    for (std::size_t x=0;x<row.size();++x) {
        const double source=(x+.5-start)/scale-.5;
        const auto a=static_cast<int>(std::floor(source));
        row[x]=static_cast<float>(original(a)+(original(a+1)-original(a))*(source-a));
    }
}
void codec_tests() {
    // V634 Horizon: every observed AFW publication gap follows a committed
    // opposite-eye capture rejected because the cross-eye counter stayed equal.
    // First gap: AFW 557 -> 559, lost color token 558, Luke render 5316.
    const std::optional<xrfg::LukeRenderIdentity> hfw_source{{0x1234,5316}};
    require(!xrfg::luke_hfw_source_is_duplicate(hfw_source,hfw_source,true,1),
        "fresh opposite-eye AFW capture rejected by equal HFW counter");
    require(!xrfg::luke_hfw_source_is_duplicate(hfw_source,hfw_source,true,0),
        "fresh left-eye AFW capture rejected by equal HFW counter");
    require(xrfg::luke_hfw_source_is_duplicate(hfw_source,hfw_source,false,1),
        "raw HFW duplicate filter changed");
    require(xrfg::luke_hfw_source_is_duplicate(hfw_source,hfw_source,true,{}),
        "held AFW cycle treated as a new release");
    require(xrfg::luke_hfw_source_is_duplicate(hfw_source,hfw_source,true,2),
        "invalid captured eye admitted");
    require(xrfg::luke_hfw_source_is_duplicate({},hfw_source,true,1),
        "missing HFW identity dereferenced/admitted");
    require(!xrfg::luke_hfw_source_is_duplicate(
        xrfg::LukeRenderIdentity{0x1234,5317},hfw_source,false,{}),
        "ordinary new raw source rejected");
    require(xrfg::kLukeOriginBits==64 && xrfg::kLukeOriginWidth==256 &&
        xrfg::kLukeOriginHeight==8, "compact copy dimensions");
    for (const auto token : {1u,17u,0x12345678u,0xfedcba98u,UINT32_MAX})
        for (const double scale : {.5,.75,.9,1.,1.1,1.25,1.5,2.})
            for (const double offset : {.0,.2,.5,.8}) {
                std::vector<float> row(1024,.5f);
                stamp(row,43+offset,scale,token);
                auto decoded=xrfg::decode_luke_origin_row(row);
                if (scale == .5 && offset == .5) {
                    require(!decoded.token, "erased Manchester cells invented identity");
                    continue;
                }
                if (decoded.token!=token || decoded.ambiguous) {
                    std::cerr << "token=" << token << " scale=" << scale << " offset=" << offset
                        << " decoded=" << decoded.token << " ambiguous=" << decoded.ambiguous << '\n';
                }
                require(decoded.token==token && !decoded.ambiguous,"resampled compact token");
                for (auto& pixel:row) pixel=static_cast<float>(.4+pixel*(3./255));
                decoded=xrfg::decode_luke_origin_row(row);
                require(decoded.token==token && !decoded.ambiguous,"dimmed compact token");
            }
    // A changing overlay/background can merge the final sync midpoint with
    // the next transition without erasing either Manchester half-cell. The
    // old bit0/bit15-only pitch and all-16 edge fit reject this valid token.
    // Synthetic analogue of the Outlaws F8; no captured pixels are embedded.
    for (float first_black : {.64f, .61f}) {
        std::vector<float> dimmed(263, .4f);
        for (unsigned x = 0; x < xrfg::kLukeOriginWidth; ++x)
            dimmed[4 + x] = xrfg::luke_origin_white(6098, x) ? .65f : .35f;
        dimmed[66] = first_black; dimmed[67] = .6f; dimmed[68] = .35f;
        const auto decoded = xrfg::decode_luke_origin_row(dimmed);
        require(decoded.token == 6098 && !decoded.ambiguous,
            "merged final preamble edge lost CRC-valid token");
        std::vector<float> mixed(1024, .4f);
        std::copy(dimmed.begin(), dimmed.end(), mixed.begin());
        for (unsigned x = 0; x < xrfg::kLukeOriginWidth; ++x)
            mixed[512 + x] = xrfg::luke_origin_white(6100, x) ? 1.f : 0.f;
        require(xrfg::decode_luke_origin_row(mixed).ambiguous,
            "recovered marker silently chose a conflicting publication");
        // A recovered preamble never authorizes a changed payload or CRC.
        for (unsigned x = 4 + 27 * 4; x < 4 + 28 * 4; ++x)
            dimmed[x] = 1.f - dimmed[x];
        require(!xrfg::decode_luke_origin_row(dimmed).token,
            "merged-edge recovery accepted corrupted payload");
    }
    std::vector<float> row(1024,.5f);
    require(!xrfg::decode_luke_origin_row(row).token,"blank token invented");
    stamp(row,32,1,0x12345678u);
    for (unsigned x=32+27*4;x<32+28*4;++x) row[x]=1.f-row[x];
    require(!xrfg::decode_luke_origin_row(row).token,"CRC corruption accepted");
    row.assign(1024,.5f);
    for (unsigned x=0;x<256;++x) row[32+x]=xrfg::luke_origin_white(1,x)?1.f:0.f;
    for (unsigned x=0;x<256;++x) row[512+x]=xrfg::luke_origin_white(2,x)?1.f:0.f;
    require(xrfg::decode_luke_origin_row(row).ambiguous,"conflicting tokens accepted");
    stamp(row,32,1,0);
    require(!xrfg::decode_luke_origin_row(row).token,"zero token accepted");
    xrfg::LukeColorOriginLayout layout;
    require(xrfg::luke_origin_inset_for_title(L"gta5_enhanced.EXE")==std::array<unsigned,2>{0,32},"GTA Enhanced case-insensitive protected policy");
    require(xrfg::luke_origin_inset_for_title(L"Outlaws.exe")==std::array<unsigned,2>{256,32},"Outlaws protected policy changed");
    for(const auto* name:{L"GTA5.exe",L"Dead Space.exe",L"KingdomCome.exe",L"HorizonForbiddenWest.exe"})
        require(xrfg::luke_origin_inset_for_title(name)==std::array<unsigned,2>{0,0},"unrelated title got GTA inset");
    require(xrfg::compact_luke_origin_layout(2128,1696,1553,1696,layout,0,32)&&layout.band_count==1&&
        layout.bands[0].x==577&&layout.bands[0].y==34&&layout.scan.y==16&&layout.scan.height==44,
        "GTA measured final-color protected geometry");
    for(unsigned height:{1696U,2736U,3344U}) {
        const auto gta=xrfg::luke_origin_inset_for_title(L"GTA5_Enhanced.exe",height);
        require(gta[1]==height/6,"GTA scripted inset did not follow full-size eye height");
        require(xrfg::compact_luke_origin_layout(3424,height,2987,height,layout,gta[0],gta[1]),
            "GTA scripted full-size layout rejected");
        require(layout.bands[0].y>height/8+16&&layout.scan.height==44&&layout.scan.width==697,
            "GTA scripted mask clips band or increases readback area");
        for(const auto* name:{L"Outlaws.exe",L"GTA5.exe",L"DeadSpace.exe",L"KingdomCome.exe",L"HorizonForbiddenWest.exe"})
            require(xrfg::luke_origin_inset_for_title(name,height)==xrfg::luke_origin_inset_for_title(name),
                "scripted inset changed another title");
    }
    require(xrfg::compact_luke_origin_layout(3344,3344,3063,3344,layout),"Horizon crop layout");
    require(layout.band_count==1 && layout.bands[0].x==283 && layout.bands[0].y==2 && layout.bands[0].width==256 &&
        layout.bands[0].height==8 && layout.scan.width==541 && layout.scan.height==12,"derived corner crop");
    for (unsigned crop=0;crop<=281;++crop)
        require(layout.bands[0].x>=crop && layout.bands[0].x+256<=crop+3063,"either-eye crop erases marker");
    require(xrfg::compact_luke_origin_layout(2048,1400,1700,1200,layout),"vertical crop layout");
    for (unsigned crop=0;crop<=200;++crop)
        require(layout.bands[0].y>=crop && layout.bands[0].y+8<=crop+1200,"vertical crop erases band");
    require(xrfg::compact_luke_origin_layout(2560,1440,1319,1440,layout),"KCD2 measured crop layout");
    require(layout.band_count==2&&layout.scan.width==1319&&layout.scan.height==12,"KCD2 bounded two-band ROI");
    for(unsigned i=0;i<layout.band_count;++i) {
        const auto& b=layout.bands[i];
        require(b.x+b.width<=2560&&b.y+b.height<=1440,"band outside producer");
        if(i)require(layout.bands[i-1].x+layout.bands[i-1].width<=b.x,"copies overwrite Manchester cells");
    }
    constexpr std::uint32_t coverage_token=0xa5c31236u;
    std::vector<float> producer(2560,.5F);
    for(unsigned i=0;i<layout.band_count;++i)for(unsigned x=0;x<xrfg::kLukeOriginWidth;++x)
        producer[layout.bands[i].x+x]=xrfg::luke_origin_white(coverage_token,x)?1.F:0.F;
    // Decode real cell patterns through EVERY possible KCD2 crop origin, not
    // merely the left/right endpoints or a matching implementation formula.
    for(unsigned crop=0;crop<=2560-1319;++crop) {
        const auto decoded=xrfg::decode_luke_origin_row(std::span<const float>(producer).subspan(crop,1319));
        require(decoded.token==coverage_token&&!decoded.ambiguous,"KCD2 crop lost/corrupted token");
    }
    // Both copies present in one eye must still reject conflicting identities.
    for(unsigned x=0;x<xrfg::kLukeOriginWidth;++x)
        producer[layout.bands[1].x+x]=xrfg::luke_origin_white(coverage_token+1,x)?1.F:0.F;
    require(xrfg::decode_luke_origin_row(std::span<const float>(producer).subspan(620,1319)).ambiguous,
        "distinct copied tokens selected silently");
    require(!xrfg::compact_luke_origin_layout(1024,1024,400,1024,layout),"uncoverable disjoint crop admitted");
    require(!xrfg::compact_luke_origin_layout(UINT32_MAX,1440,1319,1440,layout),"oversized/overflow layout admitted");
    require(!xrfg::compact_luke_origin_layout(512,8,512,8,layout),"insufficient band margin accepted");
    require(!xrfg::compact_luke_origin_layout(512,32,1024,32,layout),"upscaled/crop mismatch accepted");
    // V641 Outlaws F8: 3056x3344 -> 3056x3337; final-color translation can
    // clip the corner sync, while REAL VR's FPS block covers its left side.
    // Protect the token, not the decoder: all bits/CRC remain mandatory.
    xrfg::LukeColorOriginLayout protected_layout;
    require(xrfg::compact_luke_origin_layout(3056,3344,3056,3337,protected_layout,256,32),
        "Outlaws protected origin layout");
    require(protected_layout.band_count==1 && protected_layout.bands[0].x==258 &&
        protected_layout.bands[0].y==41 && protected_layout.scan.x==240 && protected_layout.scan.y==16 &&
        protected_layout.scan.width==292 && protected_layout.scan.height==51,"Outlaws bounded protected scan");
    for(int shift=-16;shift<=16;++shift) {
        std::vector<float> projected(3056,.15F);
        const int x=static_cast<int>(protected_layout.bands[0].x)+shift;
        for(unsigned cell=0;cell<256;++cell)
            projected[x+cell]=xrfg::luke_origin_white(coverage_token,cell)?1.F:0.F;
        std::fill_n(projected.begin(),192,.0F); // Independent final FPS overlay.
        const auto decoded=xrfg::decode_luke_origin_row(std::span<const float>(projected).subspan(
            protected_layout.scan.x,protected_layout.scan.width));
        require(decoded.token==coverage_token&&!decoded.ambiguous,"translated token / FPS overlay lost identity");
        for(unsigned crop=0;crop<=7;++crop) {
            const int y=static_cast<int>(protected_layout.bands[0].y)-crop+shift-
                static_cast<int>(protected_layout.scan.y);
            require(y>=0 && y+8<=static_cast<int>(protected_layout.scan.height),
                "Outlaws vertical crop/translation clipped protected token");
        }
    }
    require(xrfg::compact_luke_origin_layout(3344,3344,3056,3337,protected_layout,256,32),
        "protected inset rejected cropped stereo source");
    for(unsigned crop=0;crop<=288;++crop) {
        const auto& band=protected_layout.bands[0];
        require(band.x>=crop+256 && band.x+256+256<=crop+3056,
            "protected marker lost either-eye crop margin");
    }
    require(!xrfg::compact_luke_origin_layout(512,32,512,32,protected_layout,256,32),
        "unrepresentable protected marker silently lost its inset");
}
void luma_tests() {
    const auto close=[](float actual,float expected,float tolerance=1.e-6F) {
        return std::isfinite(actual) && std::abs(actual-expected)<tolerance;
    };
    constexpr float red=.2126F,green=.7152F,blue=.0722F;
    for(auto format:{DXGI_FORMAT_R8G8B8A8_TYPELESS,DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_TYPELESS,
        DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
        const bool bgra=format==DXGI_FORMAT_B8G8R8A8_TYPELESS ||
            format==DXGI_FORMAT_B8G8R8A8_UNORM || format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        for(unsigned alpha:{0U,255U}) {
            const std::array<unsigned char,4> pixel{static_cast<unsigned char>(bgra?233:17),119,
                static_cast<unsigned char>(bgra?17:233),static_cast<unsigned char>(alpha)};
            require(close(xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(pixel.data()),format),
                (red*17+green*119+blue*233)/255),"encoded RGB/BGRA luma ordering or alpha");
        }
    }
    for(unsigned alpha:{0U,1U,2U,3U}) {
        const std::uint32_t packed=37U|(511U<<10)|(900U<<20)|(alpha<<30);
        require(close(xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(&packed),DXGI_FORMAT_R10G10B10A2_UNORM),
            (red*37+green*511+blue*900)/1023),"packed10 RGB luma ordering or alpha");
    }
    std::array<float,4> floating{-.5F,.25F,.75F,std::numeric_limits<float>::quiet_NaN()};
    require(close(xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(floating.data()),DXGI_FORMAT_R32G32B32A32_FLOAT),
        -.5F*red+.25F*green+.75F*blue),"float RGB signed luma or NaN alpha");
    for(unsigned channel=0;channel<3;++channel) for(float invalid:{std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}) {
        auto broken=floating;broken[channel]=invalid;
        require(!std::isfinite(xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(broken.data()),DXGI_FORMAT_R32G32B32A32_FLOAT)),
            "invalid float RGB component accepted");
    }
    std::array<std::uint16_t,4> half{0xb800,0x3400,0x3a00,0x7e00}; // -.5,.25,.75,NaN alpha
    require(close(xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(half.data()),DXGI_FORMAT_R16G16B16A16_FLOAT),
        -.5F*red+.25F*green+.75F*blue),"half RGB signed luma or NaN alpha");
    const std::array<std::uint16_t,4> subnormal{0x8001,0x0001,0x0000,0x7c00};
    require(close(xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(subnormal.data()),DXGI_FORMAT_R16G16B16A16_FLOAT),
        (green-red)*std::ldexp(1.F,-24),1.e-12F),"half signed subnormals or infinite alpha");
    for(unsigned channel=0;channel<3;++channel) for(std::uint16_t invalid:{std::uint16_t(0x7e00),std::uint16_t(0x7c00),std::uint16_t(0xfc00)}) {
        auto broken=half;broken[channel]=invalid;
        require(std::isnan(xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(broken.data()),DXGI_FORMAT_R16G16B16A16_FLOAT)),
            "invalid half RGB component accepted");
    }
    for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT,
        DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R16G16B16A16_UNORM}) {
        std::array<std::byte,16> pixel{};
        // Float outputs are scene-referred: their white stays under exposure/bloom.
        const bool scene_referred=format==DXGI_FORMAT_R16G16B16A16_FLOAT||
            format==DXGI_FORMAT_R32G32B32A32_FLOAT||format==DXGI_FORMAT_R11G11B10_FLOAT;
        for(bool white:{false,true}) {
            xrfg::luke_origin_pixel(pixel.data(),format,white);
            require(close(xrfg::luke_origin_luma(pixel.data(),format),
                white?(scene_referred?xrfg::kLukeOriginHdrWhite:1.F):0.F),"black/white stamp luminance changed");
        }
    }
    const std::uint32_t packed_hdr=0x3c0U|(0x380U<<11)|(0x1a0U<<22); // 1,.5,.25
    require(close(xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(&packed_hdr),DXGI_FORMAT_R11G11B10_FLOAT),
        red+.5F*green+.25F*blue),"packed HDR channels / exponent decode");
    const std::uint32_t subnormal_hdr=1U|(1U<<11)|(1U<<22);
    require(close(xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(&subnormal_hdr),DXGI_FORMAT_R11G11B10_FLOAT),
        (red+green)*std::ldexp(1.F,-20)+blue*std::ldexp(1.F,-19),1.e-12F),"packed HDR subnormals");
    for(unsigned shift:{0U,11U,22U}) {
        const std::uint32_t invalid=(shift==22?0x3e0U:0x7c0U)<<shift;
        require(!std::isfinite(xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(&invalid),DXGI_FORMAT_R11G11B10_FLOAT)),
            "packed HDR infinite channel accepted");
    }
    // Dim chromatic overlay: B quantizes to the SAME103 in both Manchester
    // cells, while R/G retain only three R8 steps. First-byte BGRA cannot decode.
    constexpr std::uint32_t token=0x12345678U;
    std::vector<std::array<unsigned char,4>> chromatic(1024,{103,103,103,255});
    for(unsigned x=0;x<xrfg::kLukeOriginWidth;++x) {
        const unsigned char level=xrfg::luke_origin_white(token,x)?105:102;
        chromatic[32+x]={103,level,level,static_cast<unsigned char>(x&255)};
    }
    std::vector<float> rgb_row(chromatic.size()),erased_blue(chromatic.size(),103.F/255);
    for(auto format:{DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_TYPELESS,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
        for(std::size_t x=0;x<chromatic.size();++x)
            rgb_row[x]=xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(chromatic[x].data()),format);
        const auto decoded=xrfg::decode_luke_origin_row(rgb_row);
        require(decoded.token==token && !decoded.ambiguous,"dim RGB Manchester token lost despite R/G contrast");
    }
    require(!xrfg::decode_luke_origin_row(erased_blue).token,"erased blue pairs invented identity");
    for(unsigned x=32+27*4;x<32+28*4;++x) {
        chromatic[x][1]=static_cast<unsigned char>(207-chromatic[x][1]);
        chromatic[x][2]=static_cast<unsigned char>(207-chromatic[x][2]);
    }
    for(std::size_t x=0;x<chromatic.size();++x)
        rgb_row[x]=xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(chromatic[x].data()),DXGI_FORMAT_B8G8R8A8_UNORM);
    require(!xrfg::decode_luke_origin_row(rgb_row).token,"RGB luminance bypassed CRC corruption");
}
struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    Gpu(bool hardware=false) {
        ComPtr<ID3D12Debug> debug;
        check(D3D12GetDebugInterface(IID_PPV_ARGS(debug.GetAddressOf())),"debug layer");
        debug->EnableDebugLayer();
        ComPtr<IDXGIFactory6> factory; ComPtr<IDXGIAdapter> adapter;
        check(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())),"DXGI");
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.GetAddressOf())),"WARP");
        check(D3D12CreateDevice(hardware?nullptr:adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(device.GetAddressOf())),"device");
        const D3D12_COMMAND_QUEUE_DESC desc{};
        check(device->CreateCommandQueue(&desc,IID_PPV_ARGS(queue.GetAddressOf())),"queue");
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(allocator.GetAddressOf())),"allocator");
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(list.GetAddressOf())),"list");
    }
    ComPtr<ID3D12Resource> texture(unsigned width,unsigned height,DXGI_FORMAT format=DXGI_FORMAT_R8G8B8A8_UNORM) {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask=heap.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=width;desc.Height=height;desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
        desc.Format=format;
        ComPtr<ID3D12Resource> result;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,
            nullptr,IID_PPV_ARGS(result.GetAddressOf())),"texture");
        return result;
    }
    void submit() {
        check(list->Close(),"close");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
    }
    void reset() {check(allocator->Reset(),"reset allocator");check(list->Reset(allocator.Get(),nullptr),"reset list");}
    void transition(ID3D12Resource* color,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={color,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);
    }
    void crop(ID3D12Resource* source,ID3D12Resource* destination,unsigned x,unsigned y,unsigned width,unsigned height,
        unsigned destination_x=0,unsigned destination_y=0) {
        transition(source,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(destination,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION s{},d{};s.pResource=source;s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.pResource=destination;d.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{x,y,0,x+width,y+height,1};
        list->CopyTextureRegion(&d,destination_x,destination_y,0,&s,&box);
        transition(source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        transition(destination,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    }
    ComPtr<ID3D12Resource> black_rows(ID3D12Resource* destination,unsigned y,unsigned height) {
        const auto desc=destination->GetDesc();const UINT width=static_cast<UINT>(desc.Width);
        const UINT pitch=(width*4+255)&~255U;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width=UINT64(pitch)*height;buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;
        buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,IID_PPV_ARGS(&upload)),"scripted black-mask upload");
        void* data{};check(upload->Map(0,nullptr,&data),"scripted mask map");
        std::memset(data,0,static_cast<std::size_t>(buffer.Width));upload->Unmap(0,nullptr);
        D3D12_TEXTURE_COPY_LOCATION s{},d{};s.pResource=upload.Get();s.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,width,height,1,pitch};
        d.pResource=destination;d.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        transition(destination,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyTextureRegion(&d,0,y,0,&s,nullptr);
        transition(destination,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        return upload; // Caller retains until the submitted readback fence completes.
    }
};
void gpu_tests() {
    Gpu gpu;
    auto source=gpu.texture(1024,64),left=gpu.texture(800,64),right=gpu.texture(800,64);
    xrfg::configure_luke_color_origin_extent(0,0);
    require(xrfg::mark_luke_color_origin(gpu.list.Get(),source.Get(),1,D3D12_RESOURCE_STATE_COMMON,
        gpu.device.Get())==E_PENDING,"startup fabricated unknown viewport");
    // The first consumer supplies the true color extent; no legacy center/fullheight path.
    std::uint32_t token{};
    require(xrfg::read_luke_color_origin(gpu.queue.Get(),left.Get(),&token,D3D12_RESOURCE_STATE_COMMON,5000)
        ==HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && token==0,"bootstrap invented token");
    constexpr std::uint32_t first=0x12340000u;
    for (unsigned frame=1;frame<=75;++frame)
        check(xrfg::mark_luke_color_origin(gpu.list.Get(),source.Get(),first+frame,
            D3D12_RESOURCE_STATE_COMMON,gpu.device.Get()),"immutable mark after64");
    gpu.crop(source.Get(),left.Get(),0,0,800,64);
    gpu.crop(source.Get(),right.Get(),224,0,800,64);gpu.submit();
    for (auto* eye : {left.Get(),right.Get()}) {
        check(xrfg::read_luke_color_origin(gpu.queue.Get(),eye,&token,D3D12_RESOURCE_STATE_COMMON,5000),"cropped eye read");
        require(token==first+75,"cropped eye token drift");
    }
    gpu.reset();
    check(xrfg::mark_luke_color_origin(gpu.list.Get(),source.Get(),UINT32_MAX,D3D12_RESOURCE_STATE_COMMON,
        gpu.device.Get()),"full32bit marker");
    gpu.crop(source.Get(),left.Get(),224,0,800,64);gpu.submit();
    check(xrfg::read_luke_color_origin(gpu.queue.Get(),left.Get(),&token,D3D12_RESOURCE_STATE_COMMON,5000),"max token read");
    require(token==UINT32_MAX,"32bit token truncated");
    ComPtr<ID3D12Fence> gate;
    check(gpu.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.GetAddressOf())),"gate fence");
    check(gpu.queue->Wait(gate.Get(),1),"gate reader");
    token=99;
    const auto stalled=xrfg::read_luke_color_origin(gpu.queue.Get(),left.Get(),&token,D3D12_RESOURCE_STATE_COMMON,5);
    check(gate->Signal(1),"release reader gate");
    require(stalled==HRESULT_FROM_WIN32(WAIT_TIMEOUT) && !token,"stalled read fabricated token");
    check(xrfg::read_luke_color_origin(gpu.queue.Get(),left.Get(),&token,D3D12_RESOURCE_STATE_COMMON,5000),"read after timeout");
    require(token==UINT32_MAX,"readback/allocator prematurely reused");
    // One transport, independent tokens/formats and no identity borrowing.
    gpu.reset();
    auto pair_a=gpu.texture(800,64),pair_b=gpu.texture(800,64,DXGI_FORMAT_R16G16B16A16_FLOAT);
    xrfg::configure_luke_color_origin_extent(800,64);
    constexpr std::uint32_t token_a=0xf1020304u,token_b=0x708090a0u;
    check(xrfg::mark_luke_color_origin(gpu.list.Get(),pair_a.Get(),token_a,D3D12_RESOURCE_STATE_COMMON,
        gpu.device.Get()),"pair first marker");
    check(xrfg::mark_luke_color_origin(gpu.list.Get(),pair_b.Get(),token_b,D3D12_RESOURCE_STATE_COMMON,
        gpu.device.Get()),"pair second HDR marker");
    gpu.submit();
    std::array<std::uint32_t,2> pair_tokens{};
    std::array<HRESULT,2> pair_results{};
    xrfg::LukeColorOriginReadReceipt pair_receipt;
    const auto read_pair=[&](std::array<ID3D12Resource*,2> colors,DWORD timeout=5000) {
        return xrfg::read_luke_color_origin_pair(gpu.queue.Get(),colors,pair_tokens,pair_results,
            D3D12_RESOURCE_STATE_COMMON,timeout,nullptr,&pair_receipt);
    };
    check(read_pair({pair_a.Get(),pair_b.Get()}),"paired transport with different row pitches");
    require(pair_receipt.submitted&&pair_receipt.fence&&pair_receipt.value&&
        pair_receipt.fence->GetCompletedValue()>=pair_receipt.value,
        "completed origin read did not return its exact GPU lease-retirement receipt");
    require(pair_results[0]==S_OK && pair_results[1]==S_OK &&
        pair_tokens[0]==token_a && pair_tokens[1]==token_b,"paired eyes merged tokens/footprints");
    check(read_pair({pair_b.Get(),pair_a.Get()}),"reversed paired transport");
    require(pair_tokens[0]==token_b && pair_tokens[1]==token_a,"paired order inferred eye identity");
    check(read_pair({nullptr,pair_b.Get()}),"valid peer survives unavailable color");
    require(pair_results[0]==E_POINTER && !pair_tokens[0] && pair_results[1]==S_OK &&
        pair_tokens[1]==token_b,"paired reader borrowed token from peer");
    // Force invalid CRC in only one eye by overwriting the first half of one cell.
    gpu.reset();
    xrfg::LukeColorOriginLayout pair_layout{};
    require(xrfg::compact_luke_origin_layout(800,64,800,64,pair_layout),"pair corruption layout");
    const auto cell_x=pair_layout.bands[0].x+27*4;
    // Adjacent Manchester halves always have opposite values; copy the second
    // half over the first across all barcode rows, making this eye undecodable.
    auto corrupt=gpu.texture(800,64);
    gpu.crop(pair_a.Get(),corrupt.Get(),0,0,800,64);
    gpu.crop(corrupt.Get(),pair_a.Get(),cell_x+2,pair_layout.bands[0].y,2,8,cell_x,pair_layout.bands[0].y);
    gpu.submit();
    check(read_pair({pair_a.Get(),pair_b.Get()}),"paired invalid-marker transport");
    require(FAILED(pair_results[0]) && !pair_tokens[0] && pair_results[1]==S_OK &&
        pair_tokens[1]==token_b,"invalid peer suppressed/contaminated valid eye");
    check(gpu.queue->Wait(gate.Get(),2),"gate pair reader");
    const auto pair_stalled=read_pair({pair_b.Get(),pair_b.Get()},5);
    require(pair_receipt.submitted&&pair_receipt.fence&&pair_receipt.value&&
        pair_receipt.fence->GetCompletedValue()<pair_receipt.value,
        "timed-out origin read lost its pending fence or reused its source lease early");
    check(gate->Signal(2),"release pair reader gate");
    require(pair_stalled==HRESULT_FROM_WIN32(WAIT_TIMEOUT) &&
        pair_results[0]==pair_stalled && pair_results[1]==pair_stalled &&
        !pair_tokens[0] && !pair_tokens[1],"stalled pair returned stale tokens");
    check(read_pair({pair_b.Get(),pair_b.Get()}),"paired reuse after timeout");
    require(pair_tokens[0]==token_b && pair_tokens[1]==token_b,"paired readback reused before retirement");
    // Final Output differs from the intermediate and has an explicit active rectangle.
    gpu.reset();
    auto final=gpu.texture(1024,64),active=gpu.texture(800,32);
    xrfg::configure_luke_color_origin_extent(800,32);
    check(xrfg::mark_luke_color_origin(gpu.list.Get(),final.Get(),0xfedcba98u,D3D12_RESOURCE_STATE_COMMON,
        gpu.device.Get(),{64,16,896,32}),"final active rectangle");
    gpu.crop(final.Get(),active.Get(),160,16,800,32);gpu.submit();
    check(xrfg::read_luke_color_origin(gpu.queue.Get(),active.Get(),&token,D3D12_RESOURCE_STATE_COMMON,5000),"final active read");
    require(token==0xfedcba98u,"marker attached to intermediate/incorrect active rectangle");
    // Actual KCD2 producer and eye dimensions; endpoint and interior crops.
    gpu.reset();auto kcd_source=gpu.texture(2560,1440);
    const std::array<unsigned,8> crops{0,1,257,258,620,1059,1060,1241};
    std::array<ComPtr<ID3D12Resource>,8> eyes;
    xrfg::configure_luke_color_origin_extent(1319,1440);
    constexpr std::uint32_t kcd_token=0x50520731u;
    check(xrfg::mark_luke_color_origin(gpu.list.Get(),kcd_source.Get(),kcd_token,
        D3D12_RESOURCE_STATE_COMMON,gpu.device.Get()),"KCD2 measured producer marker");
    for(unsigned i=0;i<eyes.size();++i) {
        eyes[i]=gpu.texture(1319,1440);
        gpu.crop(kcd_source.Get(),eyes[i].Get(),crops[i],0,1319,1440);
    }
    gpu.submit();
    for(auto& eye:eyes) {
        check(xrfg::read_luke_color_origin(gpu.queue.Get(),eye.Get(),&token,D3D12_RESOURCE_STATE_COMMON,5000),
            "KCD2 actual GPU endpoint/interior crop read");
        require(token==kcd_token,"KCD2 GPU crop changed publication identity");
    }

    // The shipping writer must carry the same full token through real HDR
    // resources and compatible typeless storage, not just CPU stamp helpers.
    for(const auto format:{DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R16G16B16A16_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT,
        DXGI_FORMAT_R16G16B16A16_TYPELESS,DXGI_FORMAT_R32G32B32A32_TYPELESS}) {
        gpu.reset();
        const auto typed=format==DXGI_FORMAT_R16G16B16A16_TYPELESS?DXGI_FORMAT_R16G16B16A16_FLOAT:
            format==DXGI_FORMAT_R32G32B32A32_TYPELESS?DXGI_FORMAT_R32G32B32A32_FLOAT:format;
        auto hdr_source=gpu.texture(1024,64,format),hdr_eye=gpu.texture(800,64,typed);
        xrfg::configure_luke_color_origin_extent(800,64);
        if(format!=typed) {
            require(xrfg::mark_luke_color_origin(gpu.list.Get(),hdr_source.Get(),1,
                D3D12_RESOURCE_STATE_COMMON,gpu.device.Get())==DXGI_ERROR_UNSUPPORTED,
                "unproven typeless numeric view invented");
            require(xrfg::mark_luke_color_origin(gpu.list.Get(),hdr_source.Get(),1,
                D3D12_RESOURCE_STATE_COMMON,gpu.device.Get(),{},DXGI_FORMAT_R8G8B8A8_UNORM)==DXGI_ERROR_UNSUPPORTED,
                "incompatible typed copy accepted");
        }
        constexpr std::uint32_t hdr_token=0xf83a7241U;
        check(xrfg::mark_luke_color_origin(gpu.list.Get(),hdr_source.Get(),hdr_token,
            D3D12_RESOURCE_STATE_COMMON,gpu.device.Get(),{},typed),"HDR/typeless original marker");
        gpu.crop(hdr_source.Get(),hdr_eye.Get(),224,0,800,64);gpu.submit();
        check(xrfg::read_luke_color_origin(gpu.queue.Get(),hdr_eye.Get(),&token,
            D3D12_RESOURCE_STATE_COMMON,5000),"HDR/typeless cropped marker read");
        require(token==hdr_token,"HDR copy changed token / CRC");
    }
    // Both eyes are released in a fixed order while actual AER production
    // alternates. Retained right color must not hide the new left token.
    // Exercise two render/output shapes across a simulated DLSS preset reset.
    std::uint64_t last=0;
    for(const unsigned width:{1024u,768u}) {
        gpu.reset();
        auto a=gpu.texture(width,64),b=gpu.texture(width,64);
        xrfg::configure_luke_color_origin_extent(width,64);
        const auto current=static_cast<std::uint32_t>(++last+100);
        const auto prior=current-1;
        check(xrfg::mark_luke_color_origin(gpu.list.Get(),a.Get(),current,D3D12_RESOURCE_STATE_COMMON,gpu.device.Get()),"new left color");
        check(xrfg::mark_luke_color_origin(gpu.list.Get(),b.Get(),prior,D3D12_RESOURCE_STATE_COMMON,gpu.device.Get()),"retained right color");
        gpu.submit();
        std::array<std::uint64_t,2> publications{};
        for(unsigned eye=0;eye<2;++eye) {
            std::uint32_t decoded{};
            check(xrfg::read_luke_color_origin(gpu.queue.Get(),eye?b.Get():a.Get(),&decoded,D3D12_RESOURCE_STATE_COMMON,5000),"stereo token read");
            publications[eye]=decoded;
        }
        require(publications[0]==current&&publications[1]==prior,"stereo marker identity changed");
        require(publications[1]<=prior,"last-release-right counterfactual not reproduced");
        const std::optional<xrfg::LukeRenderIdentity> equal_counter{{0x1234,5316}};
        require(!xrfg::luke_hfw_source_is_duplicate(equal_counter,equal_counter,true,0),
            "color-carried fresh AFW source rejected before token selection");
        require(xrfg::select_luke_afw_color_source(publications,prior)==0,"last release hid current left source");
        require(!xrfg::select_luke_afw_color_source(publications,current),"duplicate source reused");
        require(xrfg::select_luke_afw_color_source({prior,current},prior)==1,"right source selection drift");
        require(!xrfg::select_luke_afw_color_source({current,current},prior),"ambiguous two-eye token admitted");
        require(!xrfg::select_luke_afw_color_source({0,0},prior),"missing tokens admitted");
    }

    // Outlaws' two probes are synchronous XR work. A second eye shares the
    // queue's reader and must retire its first unfinished readback before
    // reusing it; another probe therefore repeats the whole timeout. Freeze
    // both original colors before parking later native work, then exercise
    // the shipping reader on that queue and on an actual separate queue.
    {
        gpu.reset();
        const std::array<std::uint32_t,2> frozen_tokens{0x3827a519U,0xc57418efU};
        std::array<ComPtr<ID3D12Resource>,2> frozen{
            gpu.texture(1024,64),gpu.texture(1024,64)};
        xrfg::configure_luke_color_origin_extent(1024,64);
        for(unsigned eye=0;eye<frozen.size();++eye)
            check(xrfg::mark_luke_color_origin(gpu.list.Get(),frozen[eye].Get(),frozen_tokens[eye],
                D3D12_RESOURCE_STATE_COMMON,gpu.device.Get()),"frozen original stereo marker");
        gpu.submit();

        ComPtr<ID3D12Fence> native_retirement,gate;
        check(gpu.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(native_retirement.GetAddressOf())),
            "native reader retirement fence");
        check(gpu.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.GetAddressOf())),
            "later native-work gate");
        const HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        require(event!=nullptr,"native reader retirement event");
        struct UnparkNative {
            ComPtr<ID3D12CommandQueue> queue;
            ComPtr<ID3D12Fence> gate,retirement;
            HANDLE event;
            ~UnparkNative() {
                // Every exceptional path releases the CPU gate and retires
                // the queued readback before local color resources disappear.
                static_cast<void>(gate->Signal(1));
                if(SUCCEEDED(queue->Signal(retirement.Get(),4))&&
                    SUCCEEDED(retirement->SetEventOnCompletion(4,event)))
                    static_cast<void>(WaitForSingleObject(event,5000));
                CloseHandle(event);
            }
        } unpark{gpu.queue,gate,native_retirement,event};
        check(gpu.queue->Signal(native_retirement.Get(),1),"freeze native colors");
        check(native_retirement->SetEventOnCompletion(1,event),"freeze native completion event");
        require(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"original stereo colors not completed");
        check(gpu.queue->Wait(gate.Get(),1),"park later native work");
        check(gpu.queue->Signal(native_retirement.Get(),2),"parked native marker");

        const auto begin=std::chrono::steady_clock::now();
        token=99;
        const auto first=xrfg::read_luke_color_origin(gpu.queue.Get(),frozen[0].Get(),&token,
            D3D12_RESOURCE_STATE_COMMON,5);
        const auto first_done=std::chrono::steady_clock::now();
        require(first==HRESULT_FROM_WIN32(WAIT_TIMEOUT)&&token==0,"parked first probe fabricated a token");
        token=99;
        const auto second=xrfg::read_luke_color_origin(gpu.queue.Get(),frozen[1].Get(),&token,
            D3D12_RESOURCE_STATE_COMMON,5);
        const auto second_done=std::chrono::steady_clock::now();
        require(second==HRESULT_FROM_WIN32(WAIT_TIMEOUT)&&token==0,"pending reader reused for second eye");
        require(first_done-begin>=std::chrono::milliseconds(4)&&
            second_done-first_done>=std::chrono::milliseconds(4),"second probe did not reproduce repeated GPU wait");

        D3D12_COMMAND_QUEUE_DESC execution_desc{};
        execution_desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        execution_desc.Priority=D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        execution_desc.NodeMask=gpu.queue->GetDesc().NodeMask;
        ComPtr<ID3D12CommandQueue> execution;
        check(gpu.device->CreateCommandQueue(&execution_desc,IID_PPV_ARGS(execution.GetAddressOf())),
            "separate token execution queue");
        ComPtr<ID3D12Device> execution_owner;
        check(execution->GetDevice(IID_PPV_ARGS(execution_owner.GetAddressOf())),"execution queue owner");
        require(execution.Get()!=gpu.queue.Get()&&execution_owner.Get()==gpu.device.Get()&&
            execution->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT,"private reader changed native owner");
        for(unsigned eye=0;eye<frozen.size();++eye) {
            token=0;
            check(xrfg::read_luke_color_origin(execution.Get(),frozen[eye].Get(),&token,
                D3D12_RESOURCE_STATE_COMMON,5000),"private probe blocked behind later native work");
            require(token==frozen_tokens[eye],"private probe changed exact token / CRC");
            require(gate->GetCompletedValue()==0&&native_retirement->GetCompletedValue()==1,
                "private probe secretly unparked the original producer");
        }
        check(gate->Signal(1),"release native reader gate");
        check(gpu.queue->Signal(native_retirement.Get(),3),"retire original pending readback");
        check(native_retirement->SetEventOnCompletion(3,event),"native readback retirement event");
        require(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"old native reader did not retire after gate release");
        check(xrfg::read_luke_color_origin(gpu.queue.Get(),frozen[0].Get(),&token,
            D3D12_RESOURCE_STATE_COMMON,5000),"native reader recovery after unpark");
        require(token==frozen_tokens[0],"unfinished native readback corrupted recovery");
    }
    ComPtr<ID3D12InfoQueue> info;
    check(gpu.device.As(&info),"debug queue");
    for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T bytes{};info->GetMessage(i,nullptr,&bytes);std::vector<std::byte> storage(bytes);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        check(info->GetMessage(i,message,&bytes),"debug message");
        if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) {
            std::cerr<<message->pDescription<<'\n';require(false,"GPU resource/copy validation failed");
        }
    }

}
}

void cpu_batch_tests() {
 constexpr unsigned w=260,pitch=w*4;
 std::array<unsigned,2> heights{67,303};std::array<std::vector<std::byte>,2> bytes;
 std::array<xrfg::LukeColorOriginImage,2> images;std::array<xrfg::LukeColorOriginDecode,2> results;
 for(unsigned i=0;i<2;++i){bytes[i].resize(pitch*heights[i],std::byte{128});images[i]={bytes[i].data(),DXGI_FORMAT_R8G8B8A8_UNORM,w,heights[i],pitch};
  for(unsigned row:{1u,heights[i]-1})for(unsigned x=0;x<256;++x)xrfg::luke_origin_pixel(bytes[i].data()+row*pitch+(x+2)*4,images[i].format,xrfg::luke_origin_white(100+i,x));}
 for(unsigned n=0;n<20;++n){xrfg::decode_luke_origin_images(images,results);require(results[0].token==100&&!results[0].ambiguous&&results[1].token==101&&!results[1].ambiguous,"parallel rows mixed or lost exact independent identities");}
 for(unsigned x=0;x<256;++x)xrfg::luke_origin_pixel(bytes[0].data()+33*pitch+(x+2)*4,images[0].format,xrfg::luke_origin_white(103,x));
 xrfg::decode_luke_origin_images(images,results);require(results[0].ambiguous&&results[1].token==101&&!results[1].ambiguous,"parallel reader lost conflicting-row rejection");
 std::array<std::thread,4> callers;
 for(auto& t:callers)t=std::thread([&]{for(unsigned n=0;n<5;++n){std::array<xrfg::LukeColorOriginDecode,2> out;xrfg::decode_luke_origin_images(images,out);require(out[0].ambiguous&&out[1].token==101&&!out[1].ambiguous,"concurrent decode batches mixed independent results");}});
 for(auto& t:callers)t.join();
 images[0].row_pitch=3;xrfg::decode_luke_origin_images(images,results);require(!results[0].token&&results[1].token==101,"invalid stride consumed or poisoned peer");
 // Diagnostic evidence must ignore alpha-only black and remain independent
 // of CRC admission. Observing content never permits a missing token.
 std::array<std::vector<std::byte>,2> scene;
 for(unsigned i=0;i<2;++i){scene[i].resize(pitch*8);for(unsigned p=3;p<scene[i].size();p+=4)scene[i][p]=std::byte{255};
  images[i]={scene[i].data(),DXGI_FORMAT_B8G8R8A8_UNORM,w,8,pitch};}
 std::array<bool,2> nonblack{true,true};xrfg::decode_luke_origin_images(images,results,nonblack);
 require(!nonblack[0]&&!nonblack[1]&&!results[0].token&&!results[1].token,"opaque black source invented color or CRC");
 scene[0][0]=std::byte{1};xrfg::decode_luke_origin_images(images,results,nonblack);
 require(nonblack[0]&&!nonblack[1]&&!results[0].token,"content probe admitted markerless scene");
 for(unsigned x=0;x<256;++x)xrfg::luke_origin_pixel(scene[0].data()+2*pitch+(x+2)*4,images[0].format,xrfg::luke_origin_white(104,x));
 xrfg::decode_luke_origin_images(images,results,nonblack);
 require(nonblack[0]&&!nonblack[1]&&results[0].token==104&&!results[0].ambiguous,"content probe changed valid CRC");
 auto observed=results;xrfg::decode_luke_origin_images(images,results);
 require(results[0].token==observed[0].token&&results[0].ambiguous==observed[0].ambiguous&&results[1].token==observed[1].token,
  "diagnostic and ordinary decoder admission differ");
}
// Pimax/REAL VR Ghost of Tsushima: 3680x3680 producer, 3680x3089 eye, so a
// 260x603 scan whose band sits at one fixed crop row. Only rows near the
// learned band are decoded until a miss, a conflict or a geometry change.
void windowed_decode_tests() {
    constexpr unsigned w=260,h=603,pitch=w*4;
    std::array<std::vector<std::byte>,2> bytes;
    std::array<xrfg::LukeColorOriginImage,2> images;
    for(unsigned i=0;i<2;++i) {
        bytes[i].assign(std::size_t(pitch)*h,std::byte{128});
        images[i]={bytes[i].data(),DXGI_FORMAT_R8G8B8A8_UNORM,w,h,pitch};
    }
    const auto band=[&](unsigned image,unsigned top,std::uint32_t token) {
        for(unsigned y=top;y<top+xrfg::kLukeOriginHeight;++y)for(unsigned x=0;x<256;++x)
            xrfg::luke_origin_pixel(bytes[image].data()+std::size_t(y)*pitch+(x+2)*4,images[image].format,
                xrfg::luke_origin_white(token,x));
    };
    const auto clear=[&](unsigned image){std::fill(bytes[image].begin(),bytes[image].end(),std::byte{128});};
    std::array<std::uint64_t,2> geometry{7,9};
    std::array<xrfg::LukeOriginRowWindow,2> windows{};
    std::array<xrfg::LukeColorOriginDecode,2> results{};
    const auto decode=[&](unsigned count=1){
        return xrfg::decode_luke_origin_images_windowed(std::span(images.data(),count),
            std::span(geometry.data(),count),std::span(windows.data(),count),std::span(results.data(),count));
    };
    band(0,297,0x51a7e3);
    require(decode()==0&&results[0].token==0x51a7e3&&!results[0].ambiguous,"first read is a whole scan");
    require(windows[0].geometry==7&&windows[0].first==297&&windows[0].last==304,"whole scan learned band rows");
    require(decode()==1&&results[0].token==0x51a7e3&&results[0].first_row==297&&results[0].last_row==304,
        "learned rows decoded alone, in scan coordinates");
    clear(0);band(0,297+xrfg::kLukeOriginWindowMargin,0x51a7e4);
    require(decode()==1&&results[0].token==0x51a7e4&&windows[0].first==297+xrfg::kLukeOriginWindowMargin,
        "drift inside the margin followed without a whole scan");
    clear(0);band(0,100,0x51a7e5);
    require(decode()==0&&results[0].token==0x51a7e5&&windows[0].first==100,
        "band outside the window recovered by the whole scan in the same read");
    clear(0);
    require(decode()==0&&!results[0].token&&windows[0].geometry==0,"missing token forgets the window");
    // A conflicting copy outside the window is not seen by narrowed reads
    // (no periodic whole scan); the next whole scan, here after the learned
    // band goes missing, rejects the image and forgets the rows.
    band(0,297,0x51a7e6);
    require(decode()==0&&results[0].token==0x51a7e6,"relearn after a miss");
    band(0,10,0x51a7e7);
    for(unsigned n=0;n<64;++n)
        require(decode()==1&&results[0].token==0x51a7e6&&!results[0].ambiguous,"narrowed reads scanned whole");
    clear(0);band(0,10,0x51a7e7);
    require(decode()==0&&results[0].token==0x51a7e7&&windows[0].first==10,"whole scan after a miss did not relearn");
    clear(0);band(0,297,0x51a7e6);band(0,150,0x51a7e7);
    for(unsigned n=0;n<2;++n)require(decode()==0&&results[0].ambiguous&&windows[0].geometry==0,
        "whole scan lost conflicting-token rejection, or an ambiguous read learned rows");
    // New geometry discards rows learned for another layout.
    clear(0);band(0,297,0x51a7e8);
    require(decode()==0&&windows[0].geometry==7,"relearn");
    geometry[0]=8;
    require(decode()==0&&windows[0].geometry==8,"geometry change reused stale rows");
    // Colors keep independent windows and results.
    band(1,40,0x51a7e9);
    require(decode(2)==1&&results[0].token==0x51a7e8&&results[1].token==0x51a7e9&&
        windows[1].first==40,"second color borrowed the first color's window");
    require(decode(2)==3&&results[1].token==0x51a7e9,"both colors narrowed independently");
    images[1].row_pitch=3;
    require((decode(2)&2)==0&&!results[1].token&&results[0].token==0x51a7e8,"invalid peer stride poisoned narrowed color");
}
void outlaws_protected_gpu_test() {
    wchar_t executable[MAX_PATH]{};GetModuleFileNameW(nullptr,executable,MAX_PATH);
    const auto* name=wcsrchr(executable,L'\\');
    require(_wcsicmp(name?name+1:executable,L"Outlaws.exe")==0,
        "protected GPU test requires the real Outlaws process-name routing");
    Gpu gpu;
    auto source=gpu.texture(1024,256);
    xrfg::configure_luke_color_origin_extent(1024,249);
    xrfg::LukeColorOriginRect written{};
    constexpr std::uint32_t expected=0x64231292;
    check(xrfg::mark_luke_color_origin(gpu.list.Get(),source.Get(),expected,
        D3D12_RESOURCE_STATE_COMMON,gpu.device.Get(),{},DXGI_FORMAT_UNKNOWN,&written),
        "Outlaws real scoped writer");
    require(written.x==258 && written.y==41 && written.width==256 && written.height==8,
        "Outlaws process failed to select protected marker");
    std::vector<ComPtr<ID3D12Resource>> eyes;
    for(int shift:{-16,-8,0,8,16}) {
        auto eye=gpu.texture(1024,249);
        const unsigned sx=shift<0?unsigned(-shift):0,dx=shift>0?unsigned(shift):0;
        const int origin_y=3-shift;
        const unsigned sy=origin_y>0?unsigned(origin_y):0,dy=origin_y<0?unsigned(-origin_y):0;
        gpu.crop(source.Get(),eye.Get(),sx,sy,1024-std::abs(shift),
            std::min(256-sy,249-dy),dx,dy);
        eyes.push_back(std::move(eye));
    }
    gpu.submit();
    for(const auto& eye:eyes) {
        std::uint32_t token{};
        check(xrfg::read_luke_color_origin(gpu.queue.Get(),eye.Get(),&token,
            D3D12_RESOURCE_STATE_COMMON,5000),"Outlaws shifted writer/reader GPU roundtrip");
        require(token==expected,"Outlaws translation changed exact token identity");
    }
    std::cout<<"Outlaws protected GPU: "<<checks<<" checks PASS\n";
}
void gta_protected_gpu_test() {
    wchar_t executable[MAX_PATH]{};GetModuleFileNameW(nullptr,executable,MAX_PATH);
    const auto* name=wcsrchr(executable,L'\\');
    require(_wcsicmp(name?name+1:executable,L"GTA5_Enhanced.exe")==0,"GTA GPU test requires shipping process-name routing");
    // Captured V691 failure class: chromatic channels each retain a valid CRC
    // while their spatially offset weighted luminance loses the sync/payload.
    std::vector<std::byte> chromatic(800*4*4,std::byte{102});
    const auto channel_marker=[&](unsigned channel,unsigned shift,std::uint32_t token) {
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<256;++x)
            chromatic[(y*800+100+shift+x)*4+channel]=std::byte(xrfg::luke_origin_white(token,x)?255:0);
    };
    channel_marker(0,0,8118);channel_marker(1,2,8118);channel_marker(2,1,8118);
    xrfg::LukeColorOriginImage chromatic_image{chromatic.data(),DXGI_FORMAT_R8G8B8A8_UNORM,800,4,3200};
    xrfg::LukeColorOriginDecode decoded{};
    xrfg::decode_luke_origin_images(std::span(&chromatic_image,1),std::span(&decoded,1));
    require(!decoded.token&&!decoded.ambiguous,"chromatic fixture unexpectedly retained a luminance CRC");
    chromatic_image.rgb_fallback=true;
    xrfg::decode_luke_origin_images(std::span(&chromatic_image,1),std::span(&decoded,1));
    require(decoded.token==8118&&!decoded.ambiguous,"RGB recovery did not preserve exact full token/CRC");
    channel_marker(0,0,8119);
    xrfg::decode_luke_origin_images(std::span(&chromatic_image,1),std::span(&decoded,1));
    require(decoded.ambiguous,"different valid channel tokens were silently combined");
    std::fill(chromatic.begin(),chromatic.end(),std::byte{64});
    xrfg::decode_luke_origin_images(std::span(&chromatic_image,1),std::span(&decoded,1));
    require(!decoded.token&&!decoded.ambiguous,"erased RGB invented an identity");
    Gpu gpu;auto source=gpu.texture(2128,1696);
    xrfg::configure_luke_color_origin_extent(1553,1696);
    xrfg::LukeColorOriginRect written{};constexpr std::uint32_t expected=223;
    check(xrfg::mark_luke_color_origin(gpu.list.Get(),source.Get(),expected,D3D12_RESOURCE_STATE_COMMON,gpu.device.Get(),{},DXGI_FORMAT_UNKNOWN,&written),"GTA original writer");
    require(written.x==577&&written.y==284&&written.width==256&&written.height==8,"GTA actual process did not select scripted-safe inset");
    std::vector<ComPtr<ID3D12Resource>> eyes;
    std::vector<ComPtr<ID3D12Resource>> masks;
    for(unsigned crop:{0U,288U,575U})for(int shift:{-16,-8,0,8,16}) {
        auto eye=gpu.texture(1553,1696);
        const unsigned sy=shift<0?unsigned(-shift):0,dy=shift>0?unsigned(shift):0;
        gpu.crop(source.Get(),eye.Get(),crop,sy,1553,1696-std::abs(shift),0,dy);
        masks.push_back(gpu.black_rows(eye.Get(),0,1696/8));
        masks.push_back(gpu.black_rows(eye.Get(),1696-1696/8,1696/8));
        eyes.push_back(std::move(eye));
    }
    gpu.submit();
    for(const auto& eye:eyes){std::uint32_t token{};check(xrfg::read_luke_color_origin(gpu.queue.Get(),eye.Get(),&token,D3D12_RESOURCE_STATE_COMMON,5000),"GTA cropped/shifted actual GPU reader");
        require(token==expected,"GTA crop or reprojection margin lost exact CRC token");}
    // Exact new captured geometry: gameplay -> scripted mask -> gameplay.
    // Copies are real D12 commands, while the closed REAL VR transform is not
    // simulated here. Capture-transform replay is a separate offline receipt.
    gpu.reset();auto wide=gpu.texture(3424,2736);
    xrfg::configure_luke_color_origin_extent(2987,2736);
    check(xrfg::mark_luke_color_origin(gpu.list.Get(),wide.Get(),expected+1,D3D12_RESOURCE_STATE_COMMON,gpu.device.Get(),{},DXGI_FORMAT_UNKNOWN,&written),"GTA large scripted writer");
    require(written.x==439&&written.y==458,"GTA large source marker misplaced");
    eyes.clear();masks.clear();
    for(unsigned crop:{0U,219U,437U})for(bool scripted:{false,true}) {
        auto eye=gpu.texture(2987,2736);gpu.crop(wide.Get(),eye.Get(),crop,0,2987,2736);
        if(scripted) {
            masks.push_back(gpu.black_rows(eye.Get(),0,344));
            masks.push_back(gpu.black_rows(eye.Get(),2736-344,344));
        }
        eyes.push_back(std::move(eye));
    }
    gpu.submit();
    for(const auto& eye:eyes){std::uint32_t token{};check(xrfg::read_luke_color_origin(gpu.queue.Get(),eye.Get(),&token,D3D12_RESOURCE_STATE_COMMON,5000),"GTA scripted/gameplay transition reader");
        require(token==expected+1,"GTA scripted transition kept an old token or lost its marker");}
    std::array<std::uint32_t,2> observed_tokens{};std::array<HRESULT,2> observed_results{};std::array<bool,2> observed_color{};
    check(xrfg::read_luke_color_origin_pair(gpu.queue.Get(),{eyes[0].Get(),eyes[1].Get()},observed_tokens,observed_results,
        D3D12_RESOURCE_STATE_COMMON,5000,&observed_color),"GTA GPU diagnostic color receipt");
    require(observed_color[0]&&observed_color[1]&&observed_tokens[0]==expected+1&&observed_tokens[1]==expected+1,
        "GTA color receipt changed exact tokens");
    const auto raw_scene=xrfg::luke_original_scene_identity(observed_tokens,{0,1});
    require(bool(raw_scene)&&raw_scene.output==0,"actual GPU CRC pair could not identify the original real scene");
    gpu.reset();auto erased=gpu.texture(2987,2736);gpu.crop(wide.Get(),erased.Get(),219,0,2987,2736);
    masks.push_back(gpu.black_rows(erased.Get(),440,44));gpu.submit();std::uint32_t absent{};
    require(xrfg::read_luke_color_origin(gpu.queue.Get(),erased.Get(),&absent,D3D12_RESOURCE_STATE_COMMON,5000)==HRESULT_FROM_WIN32(ERROR_NOT_FOUND)&&!absent,
        "GTA missing marker guessed an earlier publication");
    check(xrfg::read_luke_color_origin_pair(gpu.queue.Get(),{erased.Get(),erased.Get()},observed_tokens,observed_results,
        D3D12_RESOURCE_STATE_COMMON,5000,&observed_color),"GTA black GPU diagnostic receipt");
    require(!observed_color[0]&&!observed_color[1]&&!observed_tokens[0]&&!observed_tokens[1],"GTA black readback not classified exactly");
    require(!xrfg::luke_original_scene_identity(observed_tokens,{0,1}),"erased GPU markers invented a real scene identity");
    std::cout<<"GTA protected GPU: "<<checks<<" checks PASS\n";
}
int captured_decode_benchmark(char** argv) {
 constexpr unsigned width=260,height=303,pitch=14080;
 std::array<std::vector<std::byte>,2> bytes;
 std::array<xrfg::LukeColorOriginImage,2> images;
 for(unsigned i=0;i<2;++i){std::ifstream in(argv[1+i],std::ios::binary);require(bool(in),"decode benchmark capture missing");bytes[i].resize(std::size_t(pitch)*height);in.read(reinterpret_cast<char*>(bytes[i].data()),bytes[i].size());require(in.gcount()==bytes[i].size(),"decode benchmark image truncated");images[i]={bytes[i].data(),DXGI_FORMAT_R8G8B8A8_UNORM,width,height,pitch};}

 Gpu gpu(true);std::array<ComPtr<ID3D12Resource>,2> readback,uploads;
 for(unsigned i=0;i<2;++i){for(unsigned j=0;j<2;++j){D3D12_HEAP_PROPERTIES heap{};heap.Type=j?D3D12_HEAP_TYPE_READBACK:D3D12_HEAP_TYPE_UPLOAD;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes[i].size();d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;auto& r=j?readback[i]:uploads[i];check(gpu.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,j?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&r)),"benchmark hardware buffer");if(!j){void* data{};check(r->Map(0,nullptr,&data),"benchmark upload map");std::memcpy(data,bytes[i].data(),bytes[i].size());r->Unmap(0,nullptr);}}gpu.list->CopyResource(readback[i].Get(),uploads[i].Get());}
 gpu.submit();ComPtr<ID3D12Fence> fence;check(gpu.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"benchmark fence");HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(gpu.queue->Signal(fence.Get(),1),"benchmark signal");check(fence->SetEventOnCompletion(1,event),"benchmark event");require(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"benchmark GPU timeout");CloseHandle(event);
 for(unsigned i=0;i<2;++i){void* data{};check(readback[i]->Map(0,nullptr,&data),"benchmark readback map");images[i].pixels=static_cast<const std::byte*>(data);}
 const auto serial=[&](){std::array<xrfg::LukeColorOriginDecode,2> out{};std::vector<float> row(width);
  for(unsigned i=0;i<2;++i)for(unsigned y=0;y<height;++y){for(unsigned x=0;x<width;++x)row[x]=xrfg::luke_origin_luma(images[i].pixels+y*pitch+x*4,images[i].format);const auto d=xrfg::decode_luke_origin_row(row);out[i].ambiguous|=d.ambiguous;if(d.token){out[i].ambiguous|=out[i].token&&out[i].token!=d.token;out[i].token=d.token;}}return out;};
 const auto expected=serial();std::array<xrfg::LukeColorOriginDecode,2> results;std::vector<double> serial_ms,parallel_ms;
 for(unsigned n=0;n<15;++n){auto a=std::chrono::steady_clock::now();const auto reference=serial();auto b=std::chrono::steady_clock::now();xrfg::decode_luke_origin_images(images,results);auto c=std::chrono::steady_clock::now();for(unsigned i=0;i<2;++i)require(results[i].token==reference[i].token&&results[i].ambiguous==reference[i].ambiguous,"captured parallel decode differs from original exhaustive search");if(n){serial_ms.push_back(std::chrono::duration<double,std::milli>(b-a).count());parallel_ms.push_back(std::chrono::duration<double,std::milli>(c-b).count());}}
 std::sort(serial_ms.begin(),serial_ms.end());std::sort(parallel_ms.begin(),parallel_ms.end());std::cout<<"CAPTURED_DECODE serial_ms="<<serial_ms[7]<<" parallel_ms="<<parallel_ms[7]<<" token0="<<expected[0].token<<" token1="<<expected[1].token<<" ambiguous="<<expected[0].ambiguous<<','<<expected[1].ambiguous<<'\n';return 0;
}
int main(int argc,char** argv) {
    try { if(argc==2 && std::string_view(argv[1])=="outlaws-protected") {outlaws_protected_gpu_test();return 0;}
        if(argc==2 && std::string_view(argv[1])=="gta-protected") {gta_protected_gpu_test();return 0;}
        if(argc==3)return captured_decode_benchmark(argv);cpu_batch_tests();windowed_decode_tests();codec_tests();luma_tests();gpu_tests();std::cout<<"Compact origin: "<<checks<<" checks PASS\n";return 0; }
    catch(const std::exception& e) {std::cerr<<"Compact origin FAIL: "<<e.what()<<"\n";return 1;}
}
