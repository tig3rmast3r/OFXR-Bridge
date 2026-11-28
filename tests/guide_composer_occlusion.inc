#include "fixtures/great_circle_hybrid_wall.hpp"
#include <bit>

void test_guide_composer_occlusion(D3D12WarpFixture& fixture) {
    constexpr UINT width=160;
    const auto texture=[&](DXGI_FORMAT format,const std::vector<std::uint32_t>& row) {
        auto desc=stereo_texture_description(width,1);desc.DepthOrArraySize=1;desc.Format=format;desc.Flags=D3D12_RESOURCE_FLAG_NONE;
        const auto heap=heap_properties(D3D12_HEAP_TYPE_DEFAULT);ComPtr<ID3D12Resource> image;
        require_hresult(fixture.device()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&image)),"occlusion fixture texture");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows{};UINT64 bytes{},total{};
        fixture.device()->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,&bytes,&total);
        auto upload=create_buffer(fixture,D3D12_HEAP_TYPE_UPLOAD,total,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped{};require_hresult(upload->Map(0,nullptr,&mapped),"occlusion fixture upload");
        std::memcpy(static_cast<std::byte*>(mapped)+fp.Offset,row.data(),width*4);upload->Unmap(0,nullptr);
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list){
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=fp;
            to.pResource=image.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            const auto b=transition_barrier(image.Get(),D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&b);
        });return image;
    };
    const auto read=[&](const xrfg::DlssMotionVectorFrame& frame) {
        auto desc=frame.motion_vectors->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows{};UINT64 bytes{},total{};
        fixture.device()->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,&bytes,&total);
        auto buffer=create_buffer(fixture,D3D12_HEAP_TYPE_READBACK,total,D3D12_RESOURCE_STATE_COPY_DEST);
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list){
            auto b=transition_barrier(frame.motion_vectors.Get(),frame.resource_state,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&b);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=frame.motion_vectors.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=buffer.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=fp;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);
        });
        void* mapped{};require_hresult(buffer->Map(0,nullptr,&mapped),"occlusion output map");std::vector<float> values;
        const auto* row=static_cast<const std::byte*>(mapped)+fp.Offset;
        for(UINT x=0;x<width;++x){std::uint16_t h;std::memcpy(&h,row+x*4,2);
            const auto exponent=(h>>10)&31, mantissa=h&1023;
            float value=exponent==31?std::numeric_limits<float>::quiet_NaN():
                exponent==0?std::ldexp(float(mantissa),-24):std::ldexp(1.F+float(mantissa)/1024,int(exponent)-15);
            values.push_back(h&0x8000?-value:value);}
        buffer->Unmap(0,nullptr);return values;
    };
    for(bool colour_aligned:{false,true})for(bool mirrored:{false,true})for(DXGI_FORMAT depth_fmt:{DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32G8X24_TYPELESS}) {
        if(colour_aligned && depth_fmt!=DXGI_FORMAT_R32_FLOAT)continue;
        std::vector<std::uint32_t> mb(width),mr(width),db(width),dr(width);
        for(UINT x=0;x<width;++x){const auto i=mirrored?width-1-x:x;
            mb[x]=great_circle_wall::motion_b[i]^(mirrored?0x8000U:0);
            mr[x]=great_circle_wall::motion_r[i]^(mirrored?0x8000U:0);
            db[x]=great_circle_wall::depth_b[i];dr[x]=great_circle_wall::depth_r[i];}
        if(colour_aligned)for(auto* row:{&db,&dr})for(auto& value:*row) {
            const float depth=std::bit_cast<float>(value);
            value=std::bit_cast<std::uint32_t>(depth>0?.012F:.01F);
        }
        auto b=std::make_shared<xrfg::DlssMotionVectorFrame>();
        b->motion_vectors=texture(DXGI_FORMAT_R16G16_FLOAT,mb);b->depth=texture(depth_fmt,db);
        b->motion_width=b->depth_width=width;b->motion_height=b->depth_height=1;
        b->output_width=width*2;b->output_height=2;b->scale_x=-1880;b->scale_y=1;
        b->raw_motion_native_span=1;b->raw_motion_native_span_confirmed=true;
        b->depth_inverted=true;b->camera_near=.06F;b->camera_far=8192;
        b->resource_state=b->depth_resource_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        auto r=std::make_shared<xrfg::DlssMotionVectorFrame>(*b);
        if(colour_aligned) {
            b->raw_motion_color_aligned=r->raw_motion_color_aligned=true;
            for(auto* frame:{b.get(),r.get()}) {
                frame->depth_infinite=true;frame->camera_near=10;frame->camera_far=0;
                frame->camera_data_valid=frame->camera_matrices_valid=true;
                frame->camera_right={1,0,0};frame->camera_up={0,1,0};frame->camera_forward={0,0,1};
                frame->camera_view_to_clip={.815872F,0,0,0,0,1,0,0,0,0,0,1,0,0,10,0};
                frame->camera_clip_to_view={1/.815872F,0,0,0,0,1,0,0,0,0,0,.1F,0,0,1,0};
                frame->output_width=3584;
            }
            r->camera_position[0]=mirrored?-5.726F:5.726F;
        }
        r->motion_vectors=texture(DXGI_FORMAT_R16G16_FLOAT,mr);r->depth=texture(depth_fmt,dr);b->bridge=r;
        auto set=std::make_shared<xrfg::DlssMotionVectorSet>();set->eye_count=1;set->eyes[0]=b;
        xrfg::D3D12GuideComposer composer;require_hresult(composer.initialize(fixture.device()),"occlusion composer");
        xrfg::D3D12ReprojectionView view{};view.fov={-.7F,.7F,.7F,-.7F};
        const auto run=[&](bool reject){std::shared_ptr<const xrfg::DlssMotionVectorSet> result;
            fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list){result=composer.compose(list,0,set,{&view,1},reject);});
            require(result!=set,"fixture did not enter actual GPU composition");return read(*result->eyes[0]);};
        const UINT bad=mirrored?width-1-71:71,wall=mirrored?width-1-40:40,sky=mirrored?width-1-140:140;
        if(colour_aligned) {
            b->raw_motion_color_aligned=r->raw_motion_color_aligned=false;
            const auto old_guard=run(true);
            require(std::isfinite(old_guard[bad]) && std::abs(old_guard[bad]*-3760)>40,
                "old 25-percent guard must reproduce the close-layer ghost");
            b->raw_motion_color_aligned=r->raw_motion_color_aligned=true;
        }
        const auto legacy=run(false),guarded=run(true);
        require(std::abs(legacy[bad]*-3760)>40,"captured row did not reproduce the stereo wall ghost vector");
        require(!std::isfinite(guarded[bad]),"occluded sky pixel retained the wall's motion");
        for(UINT x:{wall,sky})require(std::isfinite(guarded[x]) && std::abs(guarded[x]-legacy[x])<.00001F,
            "depth validation changed a visible wall or background vector");
    }
    std::cout<<"Great Circle wall and Days Gone close depth layers: stereo ghost reproduced, opposite occlusion edges invalidated, valid surfaces unchanged PASS\n";
}
