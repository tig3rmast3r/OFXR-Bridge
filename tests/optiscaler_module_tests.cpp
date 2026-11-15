#define NOMINMAX
#include "xrfg/optiscaler_module.hpp"
#include "xrfg/ngx_evaluation_boundary.hpp"
#include <iostream>
#include <stdexcept>

int wmain(int argc,wchar_t** argv) {
    const auto require=[](bool b,const char* message){if(!b)throw std::runtime_error(message);};
    try {
        require(argc==2,"pass official OptiScaler DLL path");
        require(!xrfg::optiscaler_module_valid(nullptr),"null module admitted");
        require(!xrfg::optiscaler_module_valid(GetModuleHandleW(L"kernel32.dll")),"Windows proxy admitted");
        // Map without running the upstream proxy's DllMain or touching any game.
        auto module=LoadLibraryExW(argv[1],nullptr,DONT_RESOLVE_DLL_REFERENCES);
        require(module!=nullptr,"official PE map");
        require(xrfg::optiscaler_module_valid(module),"official stock OptiScaler identity refused");
        FreeLibrary(module);
        {
            xrfg::NgxEvaluationBoundary public_call(3,false,false,true);
            xrfg::OptiscalerEvaluationScope replacement(true);
            xrfg::NgxEvaluationBoundary producer(5,true,false);
            require(!producer.observes()&&producer.public_guide_parent()&&replacement.outermost(),"public guide ownership");
            {
                xrfg::OptiscalerEvaluationScope alias(true);
                xrfg::NgxEvaluationBoundary nested(5,true,false);
                require(!alias.outermost()&&alias.active()&&!nested.observes(),"alias duplicate witness");
            }
            {
                xrfg::OptiscalerEvaluationScope native(false);
                xrfg::NgxEvaluationBoundary nested(0,true,false);
                require(native.active()&&!native.outermost()&&!nested.observes(),"DLSS passthrough duplicate witness");
            }
            require(!replacement.completed(),"public success without FSR dispatch accepted");
            replacement.expect(reinterpret_cast<void*>(1),reinterpret_cast<void*>(2));
            xrfg::OptiscalerEvaluationScope::dispatched(reinterpret_cast<void*>(1),reinterpret_cast<void*>(2),true);
            require(replacement.completed(),"exact FSR dispatch refused");
            xrfg::OptiscalerEvaluationScope::dispatched(reinterpret_cast<void*>(1),reinterpret_cast<void*>(3),true);
            require(!replacement.completed(),"different FSR output admitted");
            require(public_call.can_complete(),"replacement suppressed final caller");
        }
        {
            xrfg::OptiscalerEvaluationScope inactive(false);
            require(!inactive.active(),"replacement scope leaked");
            xrfg::NgxEvaluationBoundary generic(2,false,true);
            xrfg::OptiscalerEvaluationScope replacement(true);
            xrfg::NgxEvaluationBoundary producer(5,true,false);
            require(!generic.observes()&&producer.observes()&&producer.can_complete(),"generic did not defer to FSR producer");
        }
        {
            xrfg::OptiscalerEvaluationScope public_caller(false);
            {xrfg::OptiscalerEvaluationScope skipped_fsr(true);}
            require(!public_caller.completed(),"caller accepted replacement initialization as completed color");
        }
        {
            xrfg::OptiscalerEvaluationScope public_caller(false);
            {xrfg::OptiscalerEvaluationScope fsr(true);
             fsr.expect(reinterpret_cast<void*>(1),reinterpret_cast<void*>(2));
             xrfg::OptiscalerEvaluationScope::dispatched(reinterpret_cast<void*>(1),reinterpret_cast<void*>(2),true);}
            require(public_caller.completed(),"completed replacement did not reach final caller");
        }
        std::cout<<"Official OptiScaler identity, proxy refusal, FSR producer and nested ownership PASS\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
