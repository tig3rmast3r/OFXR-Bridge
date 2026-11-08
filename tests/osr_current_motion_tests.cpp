// Production shader: current same-eye motion pulls own-eye history. The source
// colour intentionally differs, so passing cannot be a current-warp shortcut.
#define main retained_scene_cut_main
#include "osr_scene_cut_tests.cpp"
#undef main
#include <bit>
#include <cmath>

namespace {
UINT grey(unsigned value) { return 0xff000000u | value | (value << 8) | (value << 16); }
void begin(Fixture& f) {
    ok(f.allocator->Reset(), "selection allocator reset");
    ok(f.list->Reset(f.allocator.Get(), nullptr), "selection list reset");
    f.transfers.clear();
}
void submit(Fixture& f) {
    ok(f.list->Close(), "selection list close");
    ID3D12CommandList* lists[]{f.list.Get()}; f.queue->ExecuteCommandLists(1, lists);
    ok(f.queue->Signal(f.fence.Get(), ++f.serial), "selection signal");
    ok(f.fence->SetEventOnCompletion(f.serial, f.event), "selection event");
    check(WaitForSingleObject(f.event, 15000) == WAIT_OBJECT_0, "selection GPU completion");
    if (f.info) for (UINT64 i=0;i<f.info->GetNumStoredMessages();++i) {
        SIZE_T bytes{}; f.info->GetMessage(i,nullptr,&bytes); std::vector<char> storage(bytes);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); f.info->GetMessage(i,m,&bytes);
        if (m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) {
            std::fprintf(stderr,"%s\n",m->pDescription); check(false,"selection GPU validation");
        }
    }
    if(f.info) f.info->ClearStoredMessages();
}
xrfg::OsrFullWarp::Inputs inputs(Fixture& f, unsigned eye) {
    xrfg::OsrFullWarp::Inputs in{};
    in.source_color=f.color.Get(); in.source_depth=f.depth.Get(); in.source_eye=eye;
    in.use_history=in.use_history_motion=true;
    in.history_motion_scale[0]=in.history_motion_scale[1]=1;
    for(unsigned i=0;i<4;++i) {
        in.forward_stereo[i*5]=in.backward_source[i*5]=in.forward_history[i*5]=1;
        in.backward_history[i*5]=in.forward_history_previous[i*5]=1;
    }
    return in;
}
void warm(Fixture& f,const std::vector<UINT>& color) {
    f.frame(0,color,.2F,false); f.frame(1,color,.2F);
    f.frame(0,color,.2F); f.frame(1,color,.2F);
}
std::vector<UINT> render_with_inputs(Fixture& f,const xrfg::OsrFullWarp::Inputs& in,
    const std::vector<UINT>& color,const std::vector<float>& depth) {
    begin(f); f.upload(f.color.Get(),color.data()); f.upload(f.depth.Get(),depth.data());
    f.warp.record(f.device.Get(),f.list.Get(),in);
    auto r=f.readback(f.warp.output()); submit(f);
    std::vector<UINT> output(width*height); void* ptr{};
    ok(r.buffer->Map(0,nullptr,&ptr),"selection result map");
    for(UINT y=0;y<height;++y) std::memcpy(output.data()+y*width,
        static_cast<char*>(ptr)+r.footprint.Offset+y*r.footprint.Footprint.RowPitch,width*4);
    r.buffer->Unmap(0,nullptr); return output;
}
void save(Fixture& f,unsigned eye,const std::vector<UINT>& color,float z,ID3D12Resource* field, xrfg::OsrFullFrameIdentity identity) {
    begin(f); f.upload(f.color.Get(),color.data());
    const std::vector<float> depths(width*height,z);f.upload(f.depth.Get(),depths.data());
    auto in=inputs(f,eye);in.source_motion=field;
    xrfg::OsrFullHistory kept{};kept.publication=identity.publication;kept.stream=identity.stream;kept.epoch=identity.epoch;
    f.warp.record_history_update(f.list.Get(),in,kept);submit(f);f.warp.commit();
}
void current_vectors(unsigned eye,bool low,float native_z,bool invalid_native=false,bool zero=false,bool hard_near=false,bool reverse=false,bool screen_locked=false,float history_scale=1.F,bool adjacent=false) {
    Fixture f;std::vector<UINT> previous(width*height),current(width*height),motion(width*height,0);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        previous[y*width+x]=grey(35+(x*7+y*11)%90);
    }
    warm(f,previous);auto field=f.texture(DXGI_FORMAT_R16G16_FLOAT);
    begin(f);f.upload(field.Get(),motion.data());submit(f);
    const float current_z=hard_near?.8F:.2F;
    const float middle_z=2.F/(1.F/current_z+1.F/native_z);
    using Id=xrfg::OsrFullFrameIdentity;
    const std::uint64_t streams[2]{2293874049320ULL,2293874039672ULL}, epochs[2]{13,7};
    const auto publication=eye==0?8436ULL:8435ULL;
    const Id current_id{publication,streams[eye],epochs[eye]},
        same_id{publication-2,streams[eye],epochs[eye]},peer_id{publication-1,streams[1-eye],epochs[1-eye]};
    save(f,eye,previous,invalid_native?.9F:native_z,field.Get(),same_id);
    save(f,1-eye,previous,middle_z,field.Get(),peer_id);
    const int dx=zero?0:(adjacent?(reverse?-3:3):(reverse?-2:2)),dy=zero?0:(adjacent?(reverse?-2:2):(reverse?1:-1));
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        const auto sx=std::clamp(int(x)+dx,0,int(width)-1),sy=std::clamp(int(y)+dy,0,int(height)-1);
        current[y*width+x]=previous[sy*width+sx]^0x00100000u;
        // total(+4,-2) spans two native publications; current->previous peer half.
        motion[y*width+x]=zero?0u:(reverse?(0x40000000u|0xc400u):(0xc0000000u|0x4400u));
    }
    begin(f);f.upload(field.Get(),motion.data());submit(f);
    auto in=inputs(f,eye);in.lower_quality=low;in.source_motion=field.Get();
    const auto identity=[](const auto& h){return Id{h.publication,h.stream,h.epoch};};
    in.use_current_history_motion=xrfg::osr_full_current_motion_usable(current_id,
        identity(f.warp.history(1-eye)),identity(f.warp.history(eye)),peer_id,same_id,2,true,false);
    check(in.use_current_history_motion,"production gate activates actual A/B/A native identities BEFORE GPU test");
    if(adjacent)check(xrfg::osr_full_adjacent_motion_usable(current_id,peer_id,peer_id,1,true,true,false),"direct original interval uses independent exact peer gate");
    in.locked_depth=hard_near?.6F:2.F;
    for(unsigned i=0;i<4;++i) for(unsigned m:{0u,1u,2u,4u})in.current_history_data[m*16+i*5]=1;
    if(screen_locked)in.current_history_data[32+12]=24.F/float(width);
    in.current_history_data[0]=in.current_history_data[16]=history_scale;
    in.backward_history[0]=history_scale;in.forward_history[0]=1.F/history_scale;
    // Reversed perspective: clip->view homogeneous W = depth (hard_near=1).
    in.current_history_data[50]=in.current_history_data[54]=1;
    in.current_history_data[56]=in.current_history_data[57]=1;
    ComPtr<ID3D12Resource> original;
    if(adjacent) {
        original=f.texture(DXGI_FORMAT_R16G16_FLOAT);
        std::vector<UINT> raw(width*height,0x7bff7bffu);
        for(UINT y=3;y<height;++y)for(UINT x=7;x<width;++x)
            raw[y*width+x]=zero?0u:(reverse?0xc000c200u:0x40004200u);
        begin(f);f.upload(original.Get(),raw.data());submit(f);
        in.adjacent_motion=original.Get();
        in.adjacent_motion_state=D3D12_RESOURCE_STATE_COMMON;
        in.current_history_data[60]=7;in.current_history_data[61]=3;
        in.current_history_data[62]=1;in.current_history_data[63]=.5F;
    }
    const std::vector<float> depths(width*height,current_z);
    const auto output=render_with_inputs(f,in,current,depths);
    if(invalid_native||zero||hard_near) {
        in.use_current_history_motion=false;
        const auto baseline=render_with_inputs(f,in,current,depths);
        check(output==baseline,"invalid native depth, static and hard-hard_near retain old path");
    } else {
        unsigned matches=0,count=0;
        for(UINT y=10;y<height-10;++y)for(UINT x=24;x<width-24;++x) {
            // Unequal perspective depths change the fraction of screen-space
            // displacement, but these endpoints still round to(+2,-1).
            const int old_x=int(std::floor(history_scale*(float(x)+.5F+float(dx)-float(width)*.5F)+float(width)*.5F));
            const auto expected=previous[(y+dy)*width+old_x];++count;
            if(output[y*width+x]==expected)++matches;
        }
        if(matches!=count)std::fprintf(stderr,"eye=%u low=%u native_z=%f matches=%u/%u\n",eye,low,native_z,matches,count);
        check(matches==count,"current vectors align PREVIOUS colour including temporal depth");
    }
    std::printf("current history eye=%u lower=%u old_z=%.3f invalid=%u static=%u hard_near=%u PASS\n",eye,low,native_z,invalid_native,zero,hard_near);
}
void transported_depth(unsigned eye,bool low) {
    Fixture f;std::vector<UINT> previous(width*height),current(width*height),zero(width*height,0),motion(width*height,0x4400u);
    std::vector<float> now(width*height),native(width*height),peer(width*height);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        previous[y*width+x]=grey(35+(x*7+y*11)%90);current[y*width+x]=previous[y*width+x]^0x00100000u;
        const float z=y<height/2?.2F:.1F;now[y*width+x]=z;native[y*width+x]=z*1.25F;
        peer[y*width+x]=2.F/(1.F/z+1.F/native[y*width+x]);
    }
    warm(f,previous);auto field=f.texture(DXGI_FORMAT_R16G16_FLOAT);
    begin(f);f.upload(field.Get(),zero.data());submit(f);
    const auto seed=[&](unsigned target,const std::vector<float>& depths){
        begin(f);f.upload(f.color.Get(),previous.data());f.upload(f.depth.Get(),depths.data());
        auto in=inputs(f,target);in.source_motion=field.Get();xrfg::OsrFullHistory kept{};
        kept.publication=target==eye?101:102;kept.stream=target==eye?11:22;kept.epoch=target==eye?3:4;
        f.warp.record_history_update(f.list.Get(),in,kept);submit(f);f.warp.commit();
    };
    seed(eye,native);seed(1-eye,peer);
    begin(f);f.upload(field.Get(),motion.data());submit(f);
    auto in=inputs(f,eye);in.lower_quality=low;in.source_motion=field.Get();
    in.use_current_history_motion=xrfg::osr_full_current_motion_usable({103,11,3},{102,22,4},{101,11,3},{102,22,4},{101,11,3},2,true,false);
    check(in.use_current_history_motion,"two-depth transport passes production per-eye gate");
    for(unsigned m:{0u,1u,2u,4u})for(unsigned i=0;i<4;++i)in.current_history_data[m*16+i*5]=1;
    in.current_history_data[32+12]=24.F/float(width); // camera12px vs total4px => inferred screen attachment
    in.current_history_data[50]=in.current_history_data[54]=1;
    in.current_history_data[56]=in.current_history_data[57]=1;
    in.forward_stereo[8]=.5F;in.backward_source[8]=-.5F;
    in.current_history_data[64+8]=.5F; // old native inverse projection -> same peer stereo baseline
    const auto output=render_with_inputs(f,in,current,now);
    unsigned shifts[2]{};
    for(UINT y:{16u,56u})for(UINT x=24;x<width-24;++x) {
        // Source->peer x disparity is0.25*width*z. Halving the DIFFERENCE
        // of old/current endpoint disparities adds0.8px at z.2,0.4px at z.1.
        const float z=now[y*width+x],oldz=native[y*width+x];
        const int old_x=int(std::floor(float(x)+.5F+2.F+.125F*float(width)*(oldz-z)));
        const unsigned band=y<height/2?0:1;shifts[band]=old_x-x;
        if(output[y*width+x]!=previous[y*width+old_x]) {
            std::fprintf(stderr,"depthtransport eye=%u low=%u y=%u x=%u shift=%d actual=%u expected=%u\n",eye,low,y,x,old_x-int(x),output[y*width+x]&255,previous[y*width+old_x]&255);
            check(false,"per-point endpoint depth transport matches previous-colour oracle");
        }
    }
    check(shifts[0]!=shifts[1],"different depths produce different transferred vector shifts");
    std::printf("depth transport eye=%u lower=%u shifts=%u/%u PASS\n",eye,low,shifts[0],shifts[1]);
}

// A newly visible moving foreground edge must follow the CURRENT field rather
// than reuse stale history motion. Background/ambiguous/nearer-own cases retain
// established handling. Distinct current colour catches a current-warp shortcut.
void trusted_edge(unsigned eye,bool low,unsigned refusal) {
    Fixture f;std::vector<UINT> old(width*height),current(width*height),zero(width*height,0),field_data(width*height,0);
    std::vector<float> now(width*height,.1F),native(width*height,.1F),peer(width*height,.1F);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        old[y*width+x]=grey(25+(x*3+y*7)%35);
        if(x>=62&&x<70){old[y*width+x]=grey(160+(x*3+y*5)%70);peer[y*width+x]=.2F;}
        if(x>=64&&x<72)native[y*width+x]=.2F;
        if(x>=60&&x<68){now[y*width+x]=.2F;field_data[y*width+x]=0x4400u;}
    }
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        current[y*width+x]=old[y*width+std::min(x+2,width-1)]^0x00100000u;
        if(refusal==1&&x==63)peer[y*width+x]=.1F; // one neighbour contradicts the fetched history surface
        if(refusal==2&&x==60){peer[y*width+x]=.4F;old[y*width+x]=grey(230);} // protect nearer own-eye geometry
    }
    warm(f,old);auto field=f.texture(DXGI_FORMAT_R16G16_FLOAT);
    begin(f);f.upload(field.Get(),zero.data());submit(f);
    const auto seed=[&](unsigned target,const std::vector<float>& depths){
        begin(f);f.upload(f.color.Get(),old.data());f.upload(f.depth.Get(),depths.data());
        auto in=inputs(f,target);in.source_motion=field.Get();xrfg::OsrFullHistory kept{};
        kept.publication=target==eye?101:102;kept.stream=target==eye?11:22;kept.epoch=target==eye?3:4;
        f.warp.record_history_update(f.list.Get(),in,kept);submit(f);f.warp.commit();
    };
    seed(eye,native);seed(1-eye,peer);begin(f);f.upload(field.Get(),field_data.data());submit(f);
    auto in=inputs(f,eye);in.lower_quality=low;in.source_motion=field.Get();
    in.use_current_history_motion=xrfg::osr_full_current_motion_usable({103,11,3},{102,22,4},{101,11,3},{102,22,4},{101,11,3},2,true,false);
    check(in.use_current_history_motion,"trusted edge uses production gate");
    for(unsigned m:{0u,1u,2u,4u})for(unsigned i=0;i<4;++i)in.current_history_data[m*16+i*5]=1;
    in.current_history_data[50]=in.current_history_data[54]=1;in.current_history_data[56]=in.current_history_data[57]=1;
    const auto output=render_with_inputs(f,in,current,now);
    in.use_current_history_motion=false;const auto legacy=render_with_inputs(f,in,current,now);
    unsigned differences=0;
    for(UINT y=12;y<height-12;++y) {
        const auto at=y*width+60;
        if(refusal)check(output[at]==legacy[at],"ambiguous edge / nearer own history must retain legacy result");
        else {
            check(output[at]==old[y*width+62],"visible foreground edge uses current motion and PREVIOUS own-eye colour");
            if(output[at]!=legacy[at])++differences;
        }
    }
    if(!refusal)check(differences>0,"edge fixture would fail the V906 blanket fringe veto");
    std::printf("trusted edge eye=%u lower=%u refusal=%u changed=%u PASS\n",eye,low,refusal,differences);
}

// The object follows the gameplay camera while the head rolls in either
// direction. Native N2 endpoint is unavailable; original N1 and this eye's N1
// depth are valid. Distinct source colour prevents a pure-stereo shortcut.
void adjacent_head_rotation(unsigned eye,bool low,bool reverse,unsigned refusal,bool near_rig=false) {
    Fixture f;std::vector<UINT> old(width*height),current(width*height),zero(width*height),raw(width*height);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        old[y*width+x]=grey(35+(x*7+y*11)%90);current[y*width+x]=old[y*width+x]^0x00100000u;
    }
    warm(f,old);auto composed=f.texture(DXGI_FORMAT_R16G16_FLOAT),original=f.texture(DXGI_FORMAT_R16G16_FLOAT);
    begin(f);f.upload(composed.Get(),zero.data());submit(f);
    save(f,eye,old,.9F,composed.Get(),{101,11,3});
    const float surface_z=near_rig?.8F:.2F;
    save(f,1-eye,old,refusal==1?.4F:surface_z,composed.Get(),{102,22,4});
    auto in=inputs(f,eye);in.lower_quality=low;in.source_motion=composed.Get();in.adjacent_motion=original.Get();
    in.adjacent_motion_state=D3D12_RESOURCE_STATE_COMMON;in.locked_depth=.6F;
    in.use_current_history_motion=xrfg::osr_full_adjacent_motion_usable({103,11,3},{102,22,4},{102,22,4},1,true,true,false);
    const float angle=reverse?-.12F:.12F,c=std::cos(angle),s=std::sin(angle);
    in.forward_stereo[0]=c;in.forward_stereo[1]=s;in.forward_stereo[4]=-s;in.forward_stereo[5]=c;
    in.backward_source[0]=c;in.backward_source[1]=-s;in.backward_source[4]=s;in.backward_source[5]=c;
    for(unsigned m:{0u,1u,2u,4u})for(unsigned i=0;i<4;++i)in.current_history_data[m*16+i*5]=1;
    std::copy_n(in.forward_stereo,16,in.current_history_data);
    in.current_history_data[13]=.4F; // full N1 camera motion AFTER factoring the head rotation
    in.current_history_data[50]=in.current_history_data[54]=1;
    in.current_history_data[56]=in.current_history_data[57]=1;in.current_history_data[62]=1;in.current_history_data[63]=.5F;
    const auto half=[](float v)->UINT {
        const auto b=std::bit_cast<UINT>(v);const int exp=int((b>>23)&255)-127+15;
        if(exp<=0)return (b>>16)&0x8000;
        return ((b>>16)&0x8000)|UINT(exp<<10)+(((b&0x7fffff)+0x1000)>>13);
    };
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        const float px=(x+.5F)/width*2-1,py=1-(y+.5F)/height*2;
        const float tx=(px*c-py*s+1)*.5F*width,ty=(1-px*s-py*c)*.5F*height;
        raw[y*width+x]=half(tx-(x+.5F)+2)|half(ty-(y+.5F)-1)<<16;
    }
    if(refusal==2)std::fill(raw.begin(),raw.end(),0x7bff7bffu);
    begin(f);f.upload(original.Get(),raw.data());submit(f);
    const std::vector<float> depths(width*height,refusal==3?.8F:surface_z);
    const auto output=render_with_inputs(f,in,current,depths);
    in.use_current_history_motion=false;const auto legacy=render_with_inputs(f,in,current,depths);
    if(refusal)check(output==legacy,"nearer N1 surface, invalid original flow and hard-near retain guarded fallback");
    else {
        unsigned changed=0;
        for(UINT y=16;y<height-16;++y)for(UINT x=28;x<width-28;++x) {
            check(output[y*width+x]==old[(y-1)*width+x+2],"head-compensated N1 reads exact own-eye colour");
            changed+=output[y*width+x]!=legacy[y*width+x];
        }
        check(changed>0,"head rotation fixture must reject old N2-dependent selection");
    }
    std::printf("N1 head rotation eye=%u lower=%u reverse=%u refusal=%u near-rig=%u PASS\n",eye,low,reverse,refusal,near_rig);
}

// The old composed field can carry foreground motion into zero-depth sky.
// Confirmed original N1 keeps camera-reprojected own-eye sky instead of moving
// a windshield tint across it; non-N1 retains its established path.
void adjacent_sky(unsigned eye,bool low) {
    Fixture f;std::vector<UINT> old(width*height),current(width*height);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        old[y*width+x]=grey(35+(x*7+y*11)%90);
        current[y*width+x]=old[y*width+x]^0x00100000u;
    }
    warm(f,old);auto field=f.texture(DXGI_FORMAT_R16G16_FLOAT),original=f.texture(DXGI_FORMAT_R16G16_FLOAT);
    const std::vector<UINT> residue(width*height,0x4400u),zero(width*height,0);
    begin(f);f.upload(field.Get(),residue.data());f.upload(original.Get(),zero.data());submit(f);
    save(f,eye,old,0,field.Get(),{101,11,3});
    save(f,1-eye,old,0,field.Get(),{102,22,4});
    auto in=inputs(f,eye);in.lower_quality=low;in.source_motion=field.Get();
    in.adjacent_motion=original.Get();in.adjacent_motion_state=D3D12_RESOURCE_STATE_COMMON;
    in.use_current_history_motion=xrfg::osr_full_adjacent_motion_usable({103,11,3},{102,22,4},{102,22,4},1,true,true,false);
    in.forward_history_previous[12]=.5F;
    in.current_history_data[62]=1;
    const std::vector<float> depths(width*height,0);
    const auto output=render_with_inputs(f,in,current,depths);
    in.adjacent_motion=nullptr;in.use_current_history_motion=false;
    const auto legacy=render_with_inputs(f,in,current,depths);
    unsigned changed=0;
    for(UINT y=16;y<height-16;++y)for(UINT x=28;x<width-28;++x) {
        check(output[y*width+x]==old[y*width+x],"N1 sky must not inherit old composed foreground motion");
        changed+=output[y*width+x]!=legacy[y*width+x];
    }
    check(changed>0,"sky fixture must expose the previous composed-motion error");
    std::printf("N1 sky eye=%u lower=%u PASS\n",eye,low);
}

// The history splat holds an old hard-near mirror at x47..65, but the old
// eye's actual camera reveals background at x-16. Current stereo has a gap
// there. Recover that real background instead of copying the old mirror.
void cabin_background(unsigned eye,bool low) {
    Fixture f;std::vector<UINT> old(width*height),current(width*height),zero(width*height,0);
    std::vector<float> old_depth(width*height,.1F),depth(width*height,.1F);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        old[y*width+x]=grey(50+x%70);current[y*width+x]=grey(150);
        if(x>=47&&x<=65){old_depth[y*width+x]=.8F;old[y*width+x]=grey(10);}
        if(x>=40&&x<=60){depth[y*width+x]=.8F;current[y*width+x]=grey(10);}
    }
    warm(f,old);auto field=f.texture(DXGI_FORMAT_R16G16_FLOAT),original=f.texture(DXGI_FORMAT_R16G16_FLOAT);
    begin(f);f.upload(field.Get(),zero.data());f.upload(original.Get(),zero.data());f.upload(f.color.Get(),old.data());f.upload(f.depth.Get(),old_depth.data());
    auto keep=inputs(f,1-eye);keep.source_motion=field.Get();xrfg::OsrFullHistory h{};h.publication=102;
    f.warp.record_history_update(f.list.Get(),keep,h);submit(f);f.warp.commit();
    auto in=inputs(f,eye);in.lower_quality=low;in.source_motion=field.Get();in.adjacent_motion=original.Get();
    in.adjacent_motion_state=D3D12_RESOURCE_STATE_COMMON;in.use_current_history_motion=true;
    in.conservative_cabin_history=true;in.locked_depth=.6F;
    in.forward_stereo[8]=1;in.backward_source[8]=-1;
    in.forward_history[12]=.25F;in.backward_history[12]=-.25F;
    const auto output=render_with_inputs(f,in,current,depth);
    in.conservative_cabin_history=false;const auto legacy=render_with_inputs(f,in,current,depth);
    unsigned changed=0;
    for(UINT y=16;y<height-16;++y)for(UINT x=51;x<=59;++x) {
        check(output[y*width+x]==old[y*width+x-16],"cabin gap uses depth-validated real background, not old mirror");
        changed+=output[y*width+x]!=legacy[y*width+x];
    }
    check(changed>0,"cabin fixture must expose the old displaced mirror");
    in.conservative_cabin_history=true;in.adjacent_motion=nullptr;
    const auto unavailable=render_with_inputs(f,in,current,depth);
    in.conservative_cabin_history=false;
    check(unavailable==render_with_inputs(f,in,current,depth),"cabin policy requires confirmed original N1");
    std::printf("cabin background eye=%u lower=%u PASS\n",eye,low);
}

// Original N1 distinguishes a camera-following cabin from a static world
// surface. The composed field and the two-publication camera are unsuitable.
void cabin_native_history(unsigned eye,bool low,unsigned kind) {
    Fixture f;std::vector<UINT> previous(width*height),current(width*height),zero(width*height,0);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        previous[y*width+x]=grey(45+(x*7+y*11)%80);current[y*width+x]=previous[y*width+x]^0x00100000u;
    }
    warm(f,previous);auto field=f.texture(DXGI_FORMAT_R16G16_FLOAT),original=f.texture(DXGI_FORMAT_R16G16_FLOAT);
    const std::vector<UINT> native(width*height,(kind==1||kind==3)?0x5100u:kind==4?0x4900u:0x4800u); // 40 world; 8 stereo; 10 = stereo + 2 temporal
    begin(f);f.upload(field.Get(),zero.data());f.upload(original.Get(),native.data());submit(f);
    save(f,eye,previous,kind==2?.8F:.2F,field.Get(),{101,11,3});
    if(kind==3) {
        begin(f);f.upload(f.color.Get(),previous.data());const std::vector<float> depths(width*height,.2F);f.upload(f.depth.Get(),depths.data());
        auto store=inputs(f,1-eye);store.source_motion=field.Get();store.adjacent_motion=original.Get();
        store.adjacent_motion_state=D3D12_RESOURCE_STATE_COMMON;store.conservative_cabin_history=true;
        store.use_current_history_motion=true;store.current_history_data[62]=1;store.forward_stereo[12]=.125F;
        xrfg::OsrFullHistory kept{};kept.publication=102;kept.motion_scale[0]=kept.motion_scale[1]=1;
        f.warp.record_history_update(f.list.Get(),store,kept);submit(f);f.warp.commit();
        const auto& saved=f.warp.history(1-eye);
        check(saved.native_motion&&saved.previous_publication==101&&saved.native_stereo[12]==.125F,
            "native snapshot retains one-publication camera, stereo reference and original motion");
    } else save(f,1-eye,previous,.2F,original.Get(),{102,22,4});
    auto in=inputs(f,eye);in.lower_quality=low;in.source_motion=field.Get();in.adjacent_motion=original.Get();
    in.adjacent_motion_state=D3D12_RESOURCE_STATE_COMMON;in.use_current_history_motion=true;
    in.conservative_cabin_history=true;in.native_history_motion=true;in.current_history_data[62]=1;
    in.locked_depth=.6F;in.forward_history[12]=.25F;in.backward_history[12]=-.25F;
    in.forward_history_previous[12]=.625F;
    for(unsigned i=0;i<4;++i)in.native_history_stereo[i*5]=1;
    in.native_history_stereo[12]=.125F;
    const std::vector<float> depths(width*height,.2F);
    const auto output=render_with_inputs(f,in,current,depths);
    for(UINT y=16;y<height-16;++y)for(UINT x=48;x<80;++x) {
        const UINT sample_x=kind==4?x+2:kind==0?x:x-16;
        check(output[y*width+x]==previous[y*width+sample_x],
            "original N1 holds attached cabin only; static world and mismatched native depth keep camera projection");
    }
    if(kind==0) {
        in.adjacent_motion=nullptr;
        const auto unavailable=render_with_inputs(f,in,current,depths);
        for(UINT y=16;y<height-16;++y)for(UINT x=48;x<80;++x)
            check(unavailable[y*width+x]==previous[y*width+x-16],
                "unavailable current N1 must not reinterpret stored native N1 as composed N2");
    }
    std::printf("native cabin eye=%u lower=%u kind=%u PASS\n",eye,low,kind);
}

// An old foreground projects outside the source FOV, while current stereo
// already sees road. Unavailable source evidence cannot certify that old car.
void cabin_outside_history(unsigned eye,bool low) {
    Fixture f;const std::vector<UINT> old(width*height,grey(60)),current(width*height,grey(72)),zero(width*height,0);
    warm(f,old);auto field=f.texture(DXGI_FORMAT_R16G16_FLOAT),original=f.texture(DXGI_FORMAT_R16G16_FLOAT);
    begin(f);f.upload(field.Get(),zero.data());f.upload(original.Get(),zero.data());submit(f);
    save(f,eye,old,.3F,field.Get(),{101,11,3});save(f,1-eye,old,.3F,field.Get(),{102,22,4});
    auto in=inputs(f,eye);in.lower_quality=low;in.source_motion=field.Get();in.adjacent_motion=original.Get();
    in.adjacent_motion_state=D3D12_RESOURCE_STATE_COMMON;in.use_current_history_motion=true;
    in.conservative_cabin_history=true;in.current_history_data[62]=1;in.locked_depth=.6F;
    in.forward_stereo[8]=4;in.backward_source[8]=-4;
    const std::vector<float> depths(width*height,.1F);
    const auto output=render_with_inputs(f,in,current,depths);
    in.conservative_cabin_history=false;const auto legacy=render_with_inputs(f,in,current,depths);
    unsigned changed=0;
    for(UINT y=16;y<height-16;++y)for(UINT x=40;x<=60;++x) {
        check(output[y*width+x]==grey(72),"out-of-FOV old foreground cannot cover visible current road");
        changed+=output[y*width+x]!=legacy[y*width+x];
    }
    check(changed>0,"outside-history negative must expose the old background patch");
}

// A background point reappears where the previous eye still held the car.
// N2 depth interpolation alone aliases that old foreground to valid background.
void adjacent_disocclusion(unsigned eye,bool low,bool moving,bool hidden=false) {
    Fixture f;const std::vector<UINT> old(width*height,grey(60)),current(width*height,grey(72));
    const std::vector<UINT> zero(width*height,0),composed(width*height,0x4400u),raw(width*height,moving?0x4200u:0u);
    warm(f,old);auto field=f.texture(DXGI_FORMAT_R16G16_FLOAT),original=f.texture(DXGI_FORMAT_R16G16_FLOAT);
    begin(f);f.upload(field.Get(),zero.data());f.upload(original.Get(),raw.data());submit(f);
    save(f,eye,old,.3F,field.Get(),{101,11,3});
    save(f,1-eye,old,.15F,field.Get(),{102,22,4});
    begin(f);f.upload(field.Get(),composed.data());submit(f);
    auto in=inputs(f,eye);in.lower_quality=low;in.source_motion=field.Get();
    in.adjacent_motion=original.Get();in.adjacent_motion_state=D3D12_RESOURCE_STATE_COMMON;
    in.use_current_history_motion=xrfg::osr_full_adjacent_motion_usable({103,11,3},{102,22,4},{102,22,4},1,true,true,false);
    for(unsigned m:{0u,1u,2u,4u})for(unsigned i=0;i<4;++i)in.current_history_data[m*16+i*5]=1;
    in.current_history_data[50]=in.current_history_data[54]=1;
    in.current_history_data[56]=in.current_history_data[57]=1;in.current_history_data[62]=1;in.current_history_data[63]=.5F;
    in.locked_depth=.6F;
    std::vector<float> depths(width*height,.1F);
    if(hidden) {
        in.forward_stereo[8]=2;in.backward_source[8]=-2;
        // At destination x64, the old near surface projects to source x44.8;
        // a current source occluder there hides genuine own-eye geometry.
        for(UINT y=0;y<height;++y)for(UINT x=43;x<=46;++x)depths[y*width+x]=.3F;
    }
    const auto output=render_with_inputs(f,in,current,depths);
    if(hidden) {
        for(UINT y=16;y<height-16;++y)check(output[y*width+64]==grey(60),
            "genuine own-eye foreground hidden behind source occluder stays protected");
        return;
    }
    for(UINT y=16;y<height-16;++y)for(UINT x=28;x<width-28;++x)
        check(output[y*width+x]==(moving?grey(60):grey(72)),
            "newly visible static background rejects old foreground; moving own-eye history stays primary");
    std::printf("N1 disocclusion eye=%u lower=%u moving=%u PASS\n",eye,low,moving);
}

}
int main() {
    for(unsigned eye:{0u,1u})for(bool lower:{false,true}) {
        cabin_background(eye,lower);
        cabin_outside_history(eye,lower);
        for(unsigned kind:{0u,1u,2u,3u,4u})cabin_native_history(eye,lower,kind);
        adjacent_sky(eye,lower);
        current_vectors(eye,lower,.2F);
        current_vectors(eye,lower,.25F);
        current_vectors(eye,lower,.2F,false,false,false,true);
        current_vectors(eye,lower,.25F,true);
        current_vectors(eye,lower,.2F,false,true);
        current_vectors(eye,lower,.8F,false,false,true);
        current_vectors(eye,lower,.2F,false,false,false,false,true);
        current_vectors(eye,lower,.25F,false,false,false,false,true);
        current_vectors(eye,lower,.2F,false,false,false,true,true);
        current_vectors(eye,lower,.2F,false,true,false,false,true);
        current_vectors(eye,lower,.2F,false,false,false,false,false,1.125F);
        current_vectors(eye,lower,.2F,false,false,false,false,false,1.F,true);
        current_vectors(eye,lower,.25F,false,false,false,false,false,1.F,true);
        current_vectors(eye,lower,.2F,false,false,false,true,false,1.F,true);
        current_vectors(eye,lower,.25F,true,false,false,false,false,1.F,true);
        current_vectors(eye,lower,.8F,false,false,true,false,false,1.F,true);
        adjacent_disocclusion(eye,lower,false);adjacent_disocclusion(eye,lower,true);adjacent_disocclusion(eye,lower,false,true);
        transported_depth(eye,lower);
        trusted_edge(eye,lower,0);trusted_edge(eye,lower,1);trusted_edge(eye,lower,2);
        adjacent_head_rotation(eye,lower,false,0);adjacent_head_rotation(eye,lower,true,0);
        for(unsigned refusal:{1u,2u,3u})adjacent_head_rotation(eye,lower,false,refusal);
        // A near dashboard/hand has confirmed original N1 flow and matching
        // peer depth, despite the unreliable N2 endpoint. Preserve its exact
        // own-eye sample through head roll; mismatched depth/invalid flow refuse.
        adjacent_head_rotation(eye,lower,false,0,true);
        adjacent_head_rotation(eye,lower,true,0,true);
        for(unsigned refusal:{1u,2u})adjacent_head_rotation(eye,lower,false,refusal,true);
    }
    std::puts("OSR current native vectors: both eyes/qualities, own-eye colour, temporal depth and conservative fallbacks PASS");return 0;
}
