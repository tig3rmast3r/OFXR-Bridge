// Production GPU passes: stored history may fill missing surfaces, but cannot
// replace or move a surface already reconstructed from the current source.
#define main retained_scene_cut_main
#include "osr_scene_cut_tests.cpp"
#undef main

namespace {
UINT grey(unsigned v) { return 0xff000000u | v | (v<<8) | (v<<16); }
void begin(Fixture& f) {
    ok(f.allocator->Reset(),"visible allocator");
    ok(f.list->Reset(f.allocator.Get(),nullptr),"visible list");f.transfers.clear();
}
void submit(Fixture& f) {
    ok(f.list->Close(),"visible close");ID3D12CommandList* lists[]{f.list.Get()};
    f.queue->ExecuteCommandLists(1,lists);ok(f.queue->Signal(f.fence.Get(),++f.serial),"visible signal");
    ok(f.fence->SetEventOnCompletion(f.serial,f.event),"visible event");
    check(WaitForSingleObject(f.event,15000)==WAIT_OBJECT_0,"visible GPU completion");
    if(f.info)for(UINT64 j=0;j<f.info->GetNumStoredMessages();++j) {
        SIZE_T bytes{};f.info->GetMessage(j,nullptr,&bytes);std::vector<char> data(bytes);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());f.info->GetMessage(j,m,&bytes);
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) {
            std::fprintf(stderr,"%s\n",m->pDescription);check(false,"visible GPU validation");
        }
    }
    if(f.info)f.info->ClearStoredMessages();
}
xrfg::OsrFullWarp::Inputs inputs(Fixture& f,unsigned eye,bool history,bool lower=false) {
    xrfg::OsrFullWarp::Inputs in{};in.source_color=f.color.Get();in.source_depth=f.depth.Get();
    in.source_eye=eye;in.use_history=in.use_history_motion=history;in.lower_quality=lower;
    in.locked_depth=.1666667F;in.history_motion_scale[0]=in.history_motion_scale[1]=1;
    for(unsigned j=0;j<4;++j)in.forward_stereo[j*5]=in.backward_source[j*5]=
        in.forward_history[j*5]=in.backward_history[j*5]=in.forward_history_previous[j*5]=1;
    return in;
}
std::vector<UINT> read(Fixture& f,const Fixture::Readback& r) {
    std::vector<UINT> pixels(width*height);void* p{};ok(r.buffer->Map(0,nullptr,&p),"visible map");
    for(UINT y=0;y<height;++y)std::memcpy(pixels.data()+y*width,
        static_cast<char*>(p)+r.footprint.Offset+y*r.footprint.Footprint.RowPitch,width*4);
    r.buffer->Unmap(0,nullptr);return pixels;
}
struct Result {std::vector<UINT> color,mask;UINT history_allowed{};};
Result render(Fixture& f,const xrfg::OsrFullWarp::Inputs& in,
    const std::vector<UINT>& color,const std::vector<float>& depth,bool remember=false) {
    begin(f);f.upload(f.color.Get(),color.data());f.upload(f.depth.Get(),depth.data());
    f.warp.record(f.device.Get(),f.list.Get(),in);
    if(remember) {xrfg::OsrFullHistory kept{};kept.publication=100+f.serial;
        f.warp.record_history_update(f.list.Get(),in,kept);}
    auto a=f.readback(f.warp.output()),b=f.readback(f.warp.resolved()),s=f.readback(f.warp.scene_state());
    submit(f);if(remember)f.warp.commit();
    void* mapped{};ok(s.buffer->Map(0,nullptr,&mapped),"visible state");
    const UINT allowed=*reinterpret_cast<const UINT*>(static_cast<char*>(mapped)+s.footprint.Offset);
    s.buffer->Unmap(0,nullptr);return {read(f,a),read(f,b),allowed};
}
void warm(Fixture& f,const std::vector<UINT>& old) {
    f.frame(0,old,.2F,false);f.frame(1,old,.2F);f.frame(0,old,.2F);f.frame(1,old,.2F);
}
void seed(Fixture& f,unsigned eye,const std::vector<UINT>& old,
    const std::vector<float>& depth,int motion=0) {
    auto field=f.texture(DXGI_FORMAT_R16G16_FLOAT);
    const UINT half=motion>0?0xc000u:motion<0?0x4000u:0u;
    std::vector<UINT> values(width*height,half);
    begin(f);f.upload(f.color.Get(),old.data());f.upload(f.depth.Get(),depth.data());f.upload(field.Get(),values.data());
    auto in=inputs(f,1-eye,true);in.source_motion=field.Get();
    xrfg::OsrFullHistory kept{};kept.publication=20;
    f.warp.record_history_update(f.list.Get(),in,kept);submit(f);f.warp.commit();
}
std::vector<UINT> pattern(unsigned base,unsigned step) {
    std::vector<UINT> result(width*height);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)result[y*width+x]=grey(base+((x+2*y)%7)*step);
    return result;
}
void visible(unsigned eye,bool lower,float z) {
    Fixture f;const auto old=pattern(128,2),current=pattern(168,2);
    const std::vector<float> depths(width*height,z);warm(f,old);seed(f,eye,old,depths,1);
    auto in=inputs(f,eye,false,lower);
    const auto pure=render(f,in,current,depths);
    in.use_history=in.use_history_motion=true;
    // A past camera and nonzero field must not shift any current valid pixel.
    in.forward_history_previous[12]=20.F/width;
    in.forward_history[12]=-10.F/width;in.backward_history[12]=10.F/width;
    const auto combined=render(f,in,current,depths);
    check(combined.history_allowed==1,"test exercises valid retained history, not a cut");
    unsigned compared=0;
    for(UINT y=4;y<height-4;++y)for(UINT x=12;x<width-12;++x) {
        const auto i=y*width+x;if((pure.mask[i]&3)!=1)continue;++compared;
        if(combined.color[i]!=pure.color[i] || (combined.mask[i]&3)!=1) {
            std::fprintf(stderr,"eye=%u lower=%u z=%f x=%u y=%u current=%u combined=%u\n",
                eye,lower,z,x,y,pure.color[i]&255,combined.color[i]&255);
            check(false,"visible current warp stays bit-exact with history enabled");
        }
    }
    check(compared>5000,"substantial current-surface coverage");
    std::printf("visible eye=%u lower=%u z=%.6f current pixels=%u exact PASS\n",eye,lower,z,compared);
}
void holes(unsigned eye,bool lower,int side,int advance) {
    Fixture f;const auto old=pattern(128,2),current=solid(grey(168));
    const std::vector<float> depths(width*height,.04F);warm(f,old);seed(f,eye,old,depths,advance);
    auto in=inputs(f,eye,false,lower);in.forward_stereo[12]=side*32.F/width;in.backward_source[12]=-side*32.F/width;
    const auto pure=render(f,in,current,depths);
    in.use_history=in.use_history_motion=true;in.forward_history_previous[12]=20.F/width;
    const auto combined=render(f,in,current,depths);
    check(combined.history_allowed==1,"occlusion history remains available");
    unsigned filled=0;
    for(UINT y=6;y<height-6;++y)for(UINT x=4;x<width-4;++x) {
        const auto i=y*width+x;
        if((pure.mask[i]&3)==1)check(combined.color[i]==pure.color[i] && (combined.mask[i]&3)==1,
            "filling occlusions cannot alter adjacent valid warp");
        if((combined.mask[i]&3)==2) {
            ++filled;const int origin=int(x)-advance;
            check(origin>=0 && origin<int(width),"bounded history sample");
            check(combined.color[i]==old[y*width+origin],"occluded pixel uses motion-compensated own-eye history");
        }
    }
    check(filled>100,"real missing region still uses retained history");
    std::printf("holes eye=%u lower=%u side=%d motion=%d filled=%u PASS\n",eye,lower,side,advance,filled);
}
void refresh(unsigned eye) {
    Fixture f;const auto old=pattern(128,2);warm(f,old);const std::vector<float> depths(width*height,.04F);
    const auto current=solid(grey(168));auto in=inputs(f,eye,true);
    render(f,in,current,depths,true); // Keep storing the source even on fully visible frames.
    in=inputs(f,1-eye,true);in.forward_stereo[12]=32.F/width;in.backward_source[12]=-32.F/width;
    const auto next=render(f,in,solid(grey(172)),depths);unsigned filled=0;
    for(UINT y=6;y<height-6;++y)for(UINT x=4;x<12;++x) {
        const auto i=y*width+x;if((next.mask[i]&3)==2) {
            ++filled;check(next.color[i]==current[i],"future hole uses the freshly stored source, not old history");
        }
    }
    check(filled>100,"freshly retained eye fills next occlusion");
    std::printf("history continues recording eye=%u filled=%u PASS\n",eye,filled);
}
}
int main() {
    for(unsigned eye:{0u,1u}) {
        for(bool lower:{false,true}) {
            for(float depth:{.04F,.4F,0.F})visible(eye,lower,depth);
            for(int side:{-1,1})for(int advance:{-1,0,1})holes(eye,lower,side,advance);
        }
        refresh(eye);
    }
    std::puts("OSR visible warp: both eyes/qualities/near/sky, valid-current identity, occluded residual motion and ongoing history recording PASS");
}
