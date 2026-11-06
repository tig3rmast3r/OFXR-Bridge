// Shipping OSR passes: a camera-followed distant surface may have small real
// screen motion. Screen locking must not freeze that residual animation.
#define main retained_scene_cut_main
#include "osr_scene_cut_tests.cpp"
#undef main

namespace {
UINT grey(unsigned value) { return 0xff000000u | value | (value << 8) | (value << 16); }
void begin(Fixture& f) {
    ok(f.allocator->Reset(), "screen-motion allocator reset");
    ok(f.list->Reset(f.allocator.Get(),nullptr), "screen-motion list reset");
    f.transfers.clear();
}
void submit(Fixture& f) {
    ok(f.list->Close(), "screen-motion close"); ID3D12CommandList* lists[]{f.list.Get()};
    f.queue->ExecuteCommandLists(1,lists); ok(f.queue->Signal(f.fence.Get(),++f.serial),"screen-motion signal");
    ok(f.fence->SetEventOnCompletion(f.serial,f.event),"screen-motion event");
    check(WaitForSingleObject(f.event,15000)==WAIT_OBJECT_0,"screen-motion GPU completion");
    if(f.info)for(UINT64 i=0;i<f.info->GetNumStoredMessages();++i) {
        SIZE_T bytes{}; f.info->GetMessage(i,nullptr,&bytes); std::vector<char> storage(bytes);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); f.info->GetMessage(i,m,&bytes);
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) {
            std::fprintf(stderr,"%s\n",m->pDescription);check(false,"screen-motion GPU validation");
        }
    }
    if(f.info)f.info->ClearStoredMessages();
}
xrfg::OsrFullWarp::Inputs inputs(Fixture& f,unsigned eye) {
    xrfg::OsrFullWarp::Inputs in{};in.source_color=f.color.Get();in.source_depth=f.depth.Get();in.source_eye=eye;
    in.use_history=in.use_history_motion=true;in.history_motion_scale[0]=in.history_motion_scale[1]=1;
    in.locked_depth=.1666667F;
    for(unsigned i=0;i<4;++i)in.forward_stereo[i*5]=in.backward_source[i*5]=in.forward_history[i*5]=
        in.backward_history[i*5]=in.forward_history_previous[i*5]=1;
    return in;
}
void test(unsigned eye,int shift,bool near_weapon) {
    Fixture f;std::vector<UINT> previous(width*height),current(width*height);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)previous[y*width+x]=grey((x/3)%2?220:20);
    f.frame(0,previous,.2F,false);f.frame(1,previous,.2F);f.frame(0,previous,.2F);f.frame(1,previous,.2F);
    const float z=near_weapon?.4F:.04F;
    const std::vector<float> depth(width*height,z);
    auto field=f.texture(DXGI_FORMAT_R16G16_FLOAT);
    // Backward same-eye displacement is +/-2 pixels across two publications.
    // Small compared with ten pixels of camera movement, it triggers inferred
    // screen lock; the next eye publication must still advance by +/-1 pixel.
    const UINT half=shift>0?0xc000u:shift<0?0x4000u:0u;
    const std::vector<UINT> motion(width*height,half);
    begin(f);f.upload(f.color.Get(),previous.data());f.upload(f.depth.Get(),depth.data());f.upload(field.Get(),motion.data());
    auto seed=inputs(f,1-eye);seed.source_motion=field.Get();xrfg::OsrFullHistory kept{};kept.publication=20;
    f.warp.record_history_update(f.list.Get(),seed,kept);submit(f);f.warp.commit();
    const int advance=near_weapon?0:shift;
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
        current[y*width+x]=previous[y*width+std::clamp(int(x)-advance,0,int(width)-1)];
    begin(f);f.upload(f.color.Get(),current.data());
    auto in=inputs(f,eye);
    in.forward_history_previous[12]=20.F/width;
    in.forward_history[12]=-10.F/width;
    in.backward_history[12]=10.F/width;
    f.warp.record(f.device.Get(),f.list.Get(),in);auto r=f.readback(f.warp.output());submit(f);
    void* ptr{};ok(r.buffer->Map(0,nullptr,&ptr),"screen-motion result map");unsigned wrong=0;
    for(UINT y=6;y<height-6;++y)for(UINT x=10;x<width-10;++x) {
        const auto* row=reinterpret_cast<const UINT*>(static_cast<char*>(ptr)+r.footprint.Offset+y*r.footprint.Footprint.RowPitch);
        wrong+=row[x]!=current[y*width+x];
    }
    r.buffer->Unmap(0,nullptr);
    std::printf("screen-motion eye=%u shift=%d hard-near=%u mismatching_pixels=%u\n",eye,shift,near_weapon,wrong);
    check(!wrong,near_weapon?"hard-near weapon still ignores unreliable composed residual":"far followed surface advances its real screen motion");
}
}
int main() {
    // Controls first so a baseline failure proves the new moving-far case.
    for(unsigned eye:{0u,1u})for(int shift:{-1,0,1})test(eye,shift,true);
    for(unsigned eye:{0u,1u})test(eye,0,false);
    for(unsigned eye:{0u,1u})for(int shift:{-1,1})test(eye,shift,false);
    std::puts("OSR screen motion: both eyes/directions, far residual animation, zero residual and hard-near controls PASS");
    return 0;
}
