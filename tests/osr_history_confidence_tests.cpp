// Actual shipping OSR passes. A one-level view-colour change must not switch
// a moving pattern by its whole temporal displacement. No game/headset.
#define main retained_scene_cut_main
#include "osr_scene_cut_tests.cpp"
#undef main

namespace {
UINT grey(unsigned value) { return 0xff000000u | value | (value << 8) | (value << 16); }
void begin(Fixture& f) {
    ok(f.allocator->Reset(), "confidence allocator reset");
    ok(f.list->Reset(f.allocator.Get(), nullptr), "confidence list reset");
    f.transfers.clear();
}
void submit(Fixture& f) {
    ok(f.list->Close(), "confidence list close");
    ID3D12CommandList* lists[]{f.list.Get()}; f.queue->ExecuteCommandLists(1, lists);
    ok(f.queue->Signal(f.fence.Get(), ++f.serial), "confidence signal");
    ok(f.fence->SetEventOnCompletion(f.serial, f.event), "confidence event");
    check(WaitForSingleObject(f.event, 15000) == WAIT_OBJECT_0, "confidence GPU completion");
}
xrfg::OsrFullWarp::Inputs inputs(Fixture& f, unsigned eye) {
    xrfg::OsrFullWarp::Inputs in{};
    in.source_color=f.color.Get();in.source_depth=f.depth.Get();in.source_eye=eye;
    in.use_history=in.use_history_motion=true;
    in.history_motion_scale[0]=in.history_motion_scale[1]=1;
    for(unsigned j=0;j<4;++j) {
        in.forward_stereo[j*5]=in.backward_source[j*5]=in.forward_history[j*5]=1;
        in.backward_history[j*5]=in.forward_history_previous[j*5]=1;
    }
    return in;
}
std::vector<UINT> render(Fixture& f,unsigned eye,const std::vector<UINT>& pixels) {
    begin(f);f.upload(f.color.Get(),pixels.data());auto in=inputs(f,eye);
    f.warp.record(f.device.Get(),f.list.Get(),in);
    const auto read=f.readback(f.warp.output());submit(f);
    std::vector<UINT> result(width*height);void* mapped{};
    ok(read.buffer->Map(0,nullptr,&mapped),"confidence result map");
    for(UINT y=0;y<height;++y)std::memcpy(result.data()+y*width,
        static_cast<char*>(mapped)+read.footprint.Offset+y*read.footprint.Footprint.RowPitch,width*4);
    read.buffer->Unmap(0,nullptr);return result;
}
void test(unsigned eye,int shift) {
    Fixture f;
    std::vector<UINT> pattern(width*height);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)pattern[y*width+x]=grey((x&1)?220:20);
    f.frame(0,pattern,.2F,false);f.frame(1,pattern,.2F);f.frame(0,pattern,.2F);f.frame(1,pattern,.2F);
    auto motion=f.texture(DXGI_FORMAT_R16G16_FLOAT);
    // Native same-eye span2; a +/-2 vector predicts the next eye tick by1.
    const UINT half=shift>0?0xc000u:shift<0?0x4000u:0u;
    std::vector<UINT> velocities(width*height,half);
    begin(f);f.upload(f.color.Get(),pattern.data());f.upload(motion.Get(),velocities.data());
    auto seed=inputs(f,1-eye);seed.source_motion=motion.Get();
    xrfg::OsrFullHistory kept{};kept.publication=20;
    f.warp.record_history_update(f.list.Get(),seed,kept);submit(f);f.warp.commit();

    const auto low=render(f,eye,solid(grey(119)));
    const auto middle=render(f,eye,solid(grey(120)));
    const auto high=render(f,eye,solid(grey(121)));
    unsigned worst=0,large=0;
    for(UINT y=4;y<height-4;++y)for(UINT x=4;x<width-4;++x) {
        auto index=y*width+x;
        for(auto pair:{std::pair{low[index],middle[index]},std::pair{middle[index],high[index]}}) {
            const auto delta=static_cast<unsigned>(std::abs(int(pair.first&255)-int(pair.second&255)));
            worst=std::max(worst,delta);large+=delta>8;
        }
        if(!shift)check(low[index]==pattern[index] && high[index]==pattern[index],"static own-eye detail stays bit-exact");
    }
    std::printf("history confidence eye=%u shift=%d one-level worst=%u jumps>8=%u\n",eye,shift,worst,large);
    check(worst<=8 && !large,"one-level colour perturbation cannot flip moving history by 200 levels");
    const auto still=render(f,eye,pattern);
    auto advanced=pattern;
    if(shift)for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
        advanced[y*width+x]=pattern[y*width+std::clamp(int(x)-shift,0,int(width)-1)];
    const auto moved=render(f,eye,advanced);
    for(UINT y=4;y<height-4;++y)for(UINT x=4;x<width-4;++x) {
        const auto index=y*width+x;
        check(still[index]==pattern[index],"clearly wrong motion still recovers the camera-only history");
        check(moved[index]==advanced[index],"clearly correct motion keeps the compensated history");
    }
    if(f.info)for(UINT64 j=0;j<f.info->GetNumStoredMessages();++j){
        SIZE_T bytes{};f.info->GetMessage(j,nullptr,&bytes);std::vector<char> data(bytes);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());f.info->GetMessage(j,message,&bytes);
        if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::fprintf(stderr,"%s\n",message->pDescription);check(false,"confidence GPU validation");}
    }
}
}
int main(){for(unsigned eye:{0u,1u})for(int shift:{-1,0,1})test(eye,shift);
    std::puts("OSR history confidence: both eyes/directions, static detail, ambiguous colour stability and clear wrong/correct-motion recovery PASS");return 0;}
