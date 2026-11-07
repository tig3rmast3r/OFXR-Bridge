// Production shader: current same-eye motion pulls own-eye history. The source
// colour intentionally differs, so passing cannot be a current-warp shortcut.
#define main retained_scene_cut_main
#include "osr_scene_cut_tests.cpp"
#undef main

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
void current_vectors(unsigned eye,bool low,float native_z,bool invalid_native=false,bool zero=false,bool hard_near=false,bool reverse=false) {
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
    const int dx=zero?0:(reverse?-2:2),dy=zero?0:(reverse?1:-1);
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
    in.locked_depth=hard_near?.6F:2.F;
    for(unsigned i=0;i<4;++i) for(unsigned m=0;m<3;++m)in.current_history_data[m*16+i*5]=1;
    // Reversed perspective: clip->view homogeneous W = depth (hard_near=1).
    in.current_history_data[50]=in.current_history_data[54]=1;
    in.current_history_data[56]=in.current_history_data[57]=1;
    const std::vector<float> depths(width*height,current_z);
    const auto output=render_with_inputs(f,in,current,depths);
    if(invalid_native||zero||hard_near) {
        in.use_current_history_motion=false;
        const auto baseline=render_with_inputs(f,in,current,depths);
        check(output==baseline,"invalid native depth, static and hard-hard_near retain old path");
    } else {
        unsigned matches=0,count=0;
        for(UINT y=10;y<height-10;++y)for(UINT x=12;x<width-12;++x) {
            // Unequal perspective depths change the fraction of screen-space
            // displacement, but these endpoints still round to(+2,-1).
            const auto expected=previous[(y+dy)*width+x+dx];++count;
            if(output[y*width+x]==expected)++matches;
        }
        if(matches!=count)std::fprintf(stderr,"eye=%u low=%u native_z=%f matches=%u/%u\n",eye,low,native_z,matches,count);
        check(matches==count,"current vectors align PREVIOUS colour including temporal depth");
    }
    std::printf("current history eye=%u lower=%u old_z=%.3f invalid=%u static=%u hard_near=%u PASS\n",eye,low,native_z,invalid_native,zero,hard_near);
}
}
int main() {
    for(unsigned eye:{0u,1u})for(bool lower:{false,true}) {
        current_vectors(eye,lower,.2F);
        current_vectors(eye,lower,.25F);
        current_vectors(eye,lower,.2F,false,false,false,true);
        current_vectors(eye,lower,.25F,true);
        current_vectors(eye,lower,.2F,false,true);
        current_vectors(eye,lower,.8F,false,false,true);
    }
    std::puts("OSR current native vectors: both eyes/qualities, own-eye colour, temporal depth and conservative fallbacks PASS");return 0;
}
