#include <climits>
#if !FIXTURE_LEGACY
#define SL_INTERPOSER
#include <sl.h>
#endif
#include "xrfg/native_ingress/streamline1/sl1.h"
#include <cstdint>
using Observer=void(*)(void*,uint32_t,uint32_t,uint32_t,const void*);
namespace {Observer observer{};int outcome=1;const void* values{};
#if !FIXTURE_LEGACY
struct Token:sl::FrameToken {uint32_t value{};operator uint32_t() const override{return value;}} token;
#endif
}
extern "C" __declspec(dllexport) void TestObserver(Observer next){observer=next;}
extern "C" __declspec(dllexport) void TestOutcome(int next){outcome=next;}
#if FIXTURE_LEGACY
extern "C" __declspec(dllexport) __declspec(noinline) bool slSetConstants(const sl1::Constants& v,uint32_t frame,uint32_t viewport){
    values=&v;volatile uint32_t keep=frame^viewport;(void)keep;return outcome!=0;
}
extern "C" __declspec(dllexport) __declspec(noinline) bool slEvaluateFeature(void* list,sl1::Feature feature,uint32_t frame,uint32_t viewport){
    if(observer)observer(list,feature,frame,viewport,values);return outcome!=0;
}
#else
extern "C" __declspec(dllexport) __declspec(noinline) sl::Result slSetConstants(const sl::Constants& v,const sl::FrameToken& frame,const sl::ViewportHandle& viewport){
    values=&v;volatile uint32_t keep=uint32_t(frame)^uint32_t(viewport);(void)keep;return outcome?sl::Result::eOk:sl::Result::eErrorInvalidParameter;
}
extern "C" __declspec(dllexport) __declspec(noinline) sl::Result slEvaluateFeature(sl::Feature feature,const sl::FrameToken& frame,const sl::BaseStructure** inputs,uint32_t count,sl::CommandBuffer* list){
    uint32_t viewport=UINT32_MAX;
    if(inputs)for(uint32_t i=0;i<count&&viewport==UINT32_MAX;++i) {
        const auto* input=inputs[i];
        for(unsigned depth=0;input&&depth<16;++depth,input=input->next)
            if(input->structType==sl::ViewportHandle::s_structType) {
                viewport=uint32_t(*static_cast<const sl::ViewportHandle*>(input));break;
            }
    }
    if(observer)observer(list,feature,uint32_t(frame),viewport,values);return outcome?sl::Result::eOk:sl::Result::eErrorInvalidParameter;
}
extern "C" __declspec(dllexport) sl::Result slGetNewFrameToken(sl::FrameToken*& output,const uint32_t* frame){token.value=frame?*frame:0;output=&token;return sl::Result::eOk;}
#endif
