// Actual shipping OSR passes: equal-depth motion overlaps must select one
// stable origin, and visibility must test the history sample actually chosen.
#define main retained_scene_cut_main
#include "osr_scene_cut_tests.cpp"
#undef main
#include <limits>

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
void seed_history(Fixture& f,unsigned eye,const std::vector<UINT>& color,
    const std::vector<float>& depth,const std::vector<UINT>& motion,
    ID3D12Resource* field) {
    begin(f); f.upload(f.color.Get(),color.data()); f.upload(f.depth.Get(),depth.data());
    f.upload(field,motion.data()); auto in=inputs(f,1-eye); in.source_motion=field;
    xrfg::OsrFullHistory kept{}; kept.publication=20;
    f.warp.record_history_update(f.list.Get(),in,kept); submit(f); f.warp.commit();
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
std::vector<UINT> render(Fixture& f,unsigned eye,const std::vector<UINT>& color,
    const std::vector<float>& depth) {
    return render_with_inputs(f,inputs(f,eye),color,depth);
}
void stable_overlap(unsigned eye,bool converging) {
    Fixture f; std::vector<UINT> previous(width*height),current(width*height),motion(width*height);
    const std::vector<float> depths(width*height,.2F);
    for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x) {
        previous[y*width+x]=grey(40+x);
        // Same-eye motion spans two publications. Opposite +/-4 vectors move
        // adjacent samples by -/+2 pixels, making flat-depth footprints overlap.
        // The deterministic greatest packed origin is x+2.5 (nearest pixel x+2).
        // Uniform -2 motion is the no-alias control: its origin is x-.5.
        motion[y*width+x]=converging ? ((x&1)?0xc400u:0x4400u) : 0xc000u;
        current[y*width+x]=grey(40+std::clamp(int(x)+(converging?2:-1),0,int(width)-1));
    }
    warm(f,previous); auto field=f.texture(DXGI_FORMAT_R16G16_FLOAT);
    seed_history(f,eye,previous,depths,motion,field.Get());
    // Missing current depth makes this a genuine history-only region. This
    // retains coverage of equal-depth historical origins after V890 removed
    // replacement of otherwise-valid current surfaces.
    const std::vector<float> missing(width*height,std::numeric_limits<float>::quiet_NaN());
    for(unsigned iteration=0;iteration<32;++iteration) {
        const auto output=render(f,eye,current,missing);
        for(UINT y=4;y<height-4;++y) for(UINT x=8;x<width-8;++x) {
            const auto i=y*width+x;
            if(output[i]!=current[i]) {
                std::fprintf(stderr,"eye=%u convergence=%u repeat=%u x=%u y=%u expected=%u actual=%u\n",
                    eye,converging,iteration,x,y,current[i]&255,output[i]&255);
                check(false,"flat-depth motion ties choose the stable winning origin");
            }
        }
    }
    std::printf("history selection eye=%u convergence=%u repeats=32 exact PASS\n",eye,converging);
}
void selected_origin_visibility(unsigned eye) {
    Fixture f; auto previous=solid(grey(20)); warm(f,previous);
    std::vector<float> depths(width*height,.2F); std::vector<UINT> motion(width*height,0);
    // An old near patch moves onto a background visible in the fresh source.
    // It is stale: the source would see it if it existed. A camera-only history
    // fallback finds background at the destination, but its location must not
    // validate the different, selected near patch's visibility.
    for(UINT y=20;y<52;++y) for(UINT x=48;x<52;++x) {
        previous[y*width+x]=grey(220); depths[y*width+x]=.4F;
        motion[y*width+x]=0xd000u; // half -32: advance the old patch by16
    }
    auto field=f.texture(DXGI_FORMAT_R16G16_FLOAT);
    seed_history(f,eye,previous,depths,motion,field.Get());
    std::fill(depths.begin(),depths.end(),.2F); auto current=solid(grey(200));
    const auto output=render(f,eye,current,depths);
    for(UINT y=24;y<48;++y) for(UINT x:{65u,66u}) {
        check(output[y*width+x]==current[y*width+x],
            "valid current background cannot be replaced by stale foreground");
    }
    std::printf("history selection eye=%u selected-origin visibility PASS\n",eye);
}
void screen_locked_surface(unsigned eye,float history_depth,float current_depth,
    bool retain_history) {
    Fixture f; std::vector<UINT> previous(width*height);
    for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x)
        previous[y*width+x]=grey(40+(x%4)*30);
    warm(f,previous); auto field=f.texture(DXGI_FORMAT_R16G16_FLOAT);
    std::vector<float> depths(width*height,history_depth);
    const std::vector<UINT> motion(width*height,0);
    seed_history(f,eye,previous,depths,motion,field.Get());
    auto in=inputs(f,eye); in.locked_depth=.1666667F;
    // A follow-camera character has the camera's world movement, but no screen
    // movement. The motion heuristic marks it screen locked despite its far
    // depth; this says where to place its samples, not that it is a near gun.
    in.forward_history_previous[12]=8.F/width;
    in.forward_history[12]=3.F/width;
    in.backward_history[12]=-3.F/width;
    std::fill(depths.begin(),depths.end(),current_depth);
    const auto current=solid(grey(128));
    const auto output=render_with_inputs(f,in,current,depths);
    const auto& expected=retain_history?previous:current;
    for(UINT y=4;y<height-4;++y) for(UINT x=8;x<width-8;++x) {
        if(output[y*width+x]!=expected[y*width+x]) {
            std::fprintf(stderr,"eye=%u history_z=%f source_z=%f keep=%u x=%u expected=%u actual=%u\n",
                eye,history_depth,current_depth,retain_history,x,expected[y*width+x]&255,output[y*width+x]&255);
            check(false,"screen lock is placement, while depth determines visibility");
        }
    }
    std::printf("history selection eye=%u screen-locked old_z=%.3f fresh_z=%.3f keep=%u PASS\n",
        eye,history_depth,current_depth,retain_history);
}
}
int main() {
    for(unsigned eye:{0u,1u}) {
        stable_overlap(eye,false); stable_overlap(eye,true); selected_origin_visibility(eye);
        screen_locked_surface(eye,.04F,.04F,false);
        screen_locked_surface(eye,.4F,.4F,false);
        screen_locked_surface(eye,.4F,.04F,false);
        screen_locked_surface(eye,.08F,.04F,false);
    }
    std::puts("OSR history selection: both eyes, uniform control, converging flat surfaces and stale-surface rejection PASS");
    return 0;
}
