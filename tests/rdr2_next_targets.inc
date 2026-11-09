void rdr2_phase_cpu() {
    using namespace xrfg;
    // Independent constant-speed scene at alternating 6/16ms. The corrected
    // camera must advance by 11ms worth of rotation in BOTH alternating eyes.
    std::array<OsrSourceTime,20> time{};std::array<double,20> angle{},corrected{};
    std::uint64_t ticks=1000000;
    for(unsigned i=0;i<20;++i){if(i)ticks+=(i%2?6000:16000);time[i]={9,100+i,ticks,1000000};angle[i]=(ticks-1000000)*1e-6;}
    for(unsigned i=2;i<20;++i){auto p=rdr2_camera_phase(time[i],time[i-1],time[i-2],angle[i],angle[i-1],angle[i-2]);check(p.has_value(),"steady alternating yaw valid");corrected[i]=angle[i]+p->yaw;}
    for(unsigned i=3;i<20;++i)check(std::abs(corrected[i]-corrected[i-1]-.011)<1e-7,"alternating real camera steps become uniform without delay");
    auto broken=time[3];broken.owner++;
    check(!rdr2_camera_phase(time[4],broken,time[2],angle[4],angle[3],angle[2]),"camera phase refuses foreign owner");
    check(!rdr2_camera_phase(time[4],time[3],time[2],angle[2],angle[3],angle[2]),"camera phase refuses reversal");
    check(!rdr2_camera_phase(time[4],time[3],time[2],2,angle[3],angle[2]),"camera phase refuses camera cut");
    auto uniform=time[4];uniform.ticks=time[3].ticks+6000;
    const auto equal=rdr2_camera_phase(uniform,time[3],time[2],.012,.006,0);
    check(equal && equal->yaw==0 && equal->seconds==0,"uniform native steps need no warp");
    for(float head:{-.004F,0.F,.004F}) {
        Rdr2Matrix view{};view[8]=std::cos(.3F+head);view[9]=std::sin(.3F+head);
        const auto yaw=rdr2_game_yaw(view,{0,std::sin(head*.5F),0,std::cos(head*.5F)});
        check(yaw && std::abs(*yaw-.3)<1e-6,"head yaw is removed from game rotation");
    }
    check(!rdr2_head_stable({0,0,0,1},{0,.1F,0,.994987F}),"rapid headset rotation bypasses trial");
    check(!rdr2_head_stable({0,0,0,1.002F},{0,.02F,0,1.0018F}),"quaternion scale cannot hide head rotation");
    std::puts("RDR2 phase: alternating cadence, stable cadence, exact identity, cuts/reversal and head separation PASS");
}
void rdr2_phase_gpu(DXGI_FORMAT format) {
    Fixture f;xrfg::Rdr2CameraPhasePass pass;
    ok(pass.prepare(f.device.Get(),width,height,format),"camera phase prepare");
    auto color=f.texture(format),scene=f.texture(DXGI_FORMAT_R32_UINT);
    std::vector<UINT> pixels(width*height),scene_data(width*height,0);scene_data[0]=1;
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)pixels[y*width+x]=0xff000000u|(x*2u)|((y*3u)<<8)|((x+y)<<16);
    const xrfg::Rdr2Matrix P{1,0,0,0,0,1,0,0,0,0,0,-1,0,0,.05F,0},I{1,0,0,0,0,1,0,0,0,0,0,20,0,0,-1,0};
    const xrfg::Rdr2Matrix V{1,0,0,0,0,0,1,0,0,-1,0,0,0,0,0,1},W{1,0,0,0,0,0,-1,0,0,1,0,0,0,0,0,1};
    for(bool cut:{false,true}) {
        begin(f);f.upload(color.Get(),pixels.data());scene_data[2]=cut;f.upload(scene.Get(),scene_data.data());
        for(unsigned eye:{0u,1u})pass.record(f.device.Get(),f.list.Get(),color.Get(),scene.Get(),eye,xrfg::rdr2_phase_mapping(I,V,W,P,eye?-.012F:.012F));
        auto a=f.readback(pass.output(0)),b=f.readback(pass.output(1));submit(f);
        for(unsigned eye:{0u,1u}) {
            auto& r=eye?b:a;void* ptr{};ok(r.buffer->Map(0,nullptr,&ptr),"phase result map");unsigned changed=0;
            const float yaw=eye?-.012F:.012F;
            for(unsigned y=4;y<height-4;++y)for(unsigned x=4;x<width-4;++x) {
                const auto actual=*reinterpret_cast<const UINT*>(static_cast<const char*>(ptr)+r.footprint.Offset+y*r.footprint.Footprint.RowPitch+x*4);
                if(cut)check(actual==pixels[y*width+x],"GPU scene-cut gate exact copy");
                else {
                    const double nx=(x+.5)/width*2-1,ny=1-(y+.5)/height*2;
                    // Independent perspective rotation about camera Y.
                    const double w=std::cos(yaw)+nx*std::sin(yaw);
                    const double sx=((nx*std::cos(yaw)-std::sin(yaw))/w+1)*width*.5-.5;
                    const double sy=(1-ny/w)*height*.5-.5;
                    const int ix=int(std::floor(sx)),iy=int(std::floor(sy));const double fx=sx-ix,fy=sy-iy;
                    for(unsigned k=0;k<3;++k) {
                        const auto channel=[&](int xx,int yy){return double((pixels[yy*width+xx]>>(k*8))&255);};
                        const double want=(channel(ix,iy)*(1-fx)+channel(ix+1,iy)*fx)*(1-fy)+(channel(ix,iy+1)*(1-fx)+channel(ix+1,iy+1)*fx)*fy;
                        check(std::abs(double((actual>>(k*8))&255)-want)<=1.1,"both GPU eyes match independent rotation sampling");
                    }
                    changed+=actual!=pixels[y*width+x];
                }
            }
            if(!cut)check(changed>0,"camera test exercises pixel movement");r.buffer->Unmap(0,nullptr);
        }
    }
    std::printf("RDR2 camera phase GPU both eyes/cut gate format=%u PASS\n",unsigned(format));
}
void rdr2_clean_history_edge(unsigned eye,bool low) {
    Fixture f;std::vector<UINT> old(width*height,grey(80)),current(width*height,grey(150)),zero(width*height,0);
    std::vector<float> old_depth(width*height,.1F),depth(width*height,.1F);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        if(x>=50&&x<=70){current[y*width+x]=grey(10);depth[y*width+x]=.8F;}
        if(x>=63&&x<=83){old[y*width+x]=grey(30);old_depth[y*width+x]=.8F;}
        if(x==61||x==62)old[y*width+x]=grey(30); // background depth, stale foreground colour
    }
    warm(f,old);auto field=f.texture(DXGI_FORMAT_R16G16_FLOAT);
    begin(f);f.upload(field.Get(),zero.data());f.upload(f.color.Get(),old.data());f.upload(f.depth.Get(),old_depth.data());
    auto keep=inputs(f,1-eye);xrfg::OsrFullHistory h{};h.publication=102;
    f.warp.record_history_update(f.list.Get(),keep,h);submit(f);f.warp.commit();
    auto in=inputs(f,eye);in.lower_quality=low;in.background_history_only=true;in.use_history_motion=false;
    in.source_motion=field.Get();in.adjacent_motion=field.Get();in.use_current_history_motion=true;
    in.forward_stereo[8]=.25F;in.backward_source[8]=-.25F;
    for(unsigned m:{0u,1u,2u,4u})for(unsigned i=0;i<4;++i)in.current_history_data[m*16+i*5]=1;
    in.current_history_data[50]=in.current_history_data[54]=1;in.current_history_data[56]=in.current_history_data[57]=1;
    in.current_history_data[62]=1;
    const auto old_result=render_with_inputs(f,in,current,depth);
    in.detect_motion_interval=true;
    const auto repaired=render_with_inputs(f,in,current,depth);unsigned changes=0;
    for(unsigned y=18;y<height-18;++y) {
        // x61 is the history-filled pixel. x62 is the source/inpaint boundary,
        // so demanding that the history repair also replace it is incorrect.
        check(repaired[y*width+61]==grey(80),"edge repair chooses clean occluded background");
        changes+=old_result[y*width+61]!=repaired[y*width+61];
        check(repaired[y*width+55]==grey(80),"clean hidden background retained");
        check(repaired[y*width+74]==grey(10),"current foreground retained");
        check(repaired[y*width+20]==grey(150),"visible current background retained");
    }
    check(changes>0,"history edge fixture reproduces stale-colour artifact");
    std::printf("RDR2 history edge eye=%u lower=%u PASS\n",eye,low);
}
