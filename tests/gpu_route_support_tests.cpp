#include "xrfg/gpu_route_support.hpp"
#include "xrfg/benchmark_suite.hpp"
#include <iostream>
#include <stdexcept>
int main() {
    using namespace xrfg;using namespace xrfg::autonomous;
    const auto check=[](bool ok){if(!ok)throw std::runtime_error("GPU route policy failed");};
    try {
        for(int route=0;route<=4;++route)for(int backend=0;backend<=3;++backend) {
            Settings s;s.frame_generation=route;s.backend=backend;s.hybrid_backend=2;
            const auto original=s;constrain_radeon_routes(s,0x1002);
            check(s.frame_generation!=2&&s.hybrid_backend==0);
            if(route==0)check(s.backend==0);
            if(route==1||route==3||route==4)check(s.frame_generation==route&&s.backend==backend);
            auto nv=original;check(!constrain_radeon_routes(nv,0x10de)&&nv==original);
            auto unknown=original;check(!constrain_radeon_routes(unknown,0)&&unknown==original);
            check(!constrain_radeon_routes(s,0x1002));
        }
        for(int route=0;route<=2;++route)for(int backend=0;backend<=3;++backend) {
            embedded::Settings s;s.frame_generation=route;s.backend=backend;
            constrain_radeon_routes(s,0x1002);check(s.frame_generation!=2&&s.backend==0);
            s.rife_model=3;s.backend=2;check(!constrain_radeon_routes(s,0x1002)&&s.backend==2);
        }
        for(bool full:{false,true})for(bool luke:{false,true})for(bool steamvr:{false,true}) {
            auto plan=benchmark_execution_plan(full,luke,steamvr,true,3000,4,18);
            filter_radeon_benchmark_plan(plan,0x1002);
            for(const auto& s:plan)check(s.disabled||!(s.generation==2||((s.generation==0||s.generation==3)&&s.backend!=0)));
            check(!plan.empty());
        }
        std::cout<<"Radeon saved-route constraints, NVIDIA/unknown unchanged, supported benchmark matrix PASS; physical vendor="<<std::hex<<rendering_gpu_vendor()<<'\n';
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
