#include "NrSecondPassChain.h"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

// Exercise the orchestration called by EvaluateSecondPass, including the
// native barriers' lifetime on optional-stage failure; no duplicated chain.
struct Operations {
    std::vector<std::string> calls;
    bool clamp_ok{true}, evaluate_ok{true}, throw_on_evaluate{};
    bool first_is_readable{}, clamp_is_readable{};
    void first_readable() { calls.push_back("first readable"); first_is_readable=true; }
    bool clamp() { calls.push_back("clamp same dimensions"); return clamp_ok; }
    void clamp_readable() { calls.push_back("clamp readable"); clamp_is_readable=true; }
    bool evaluate() {
        calls.push_back("second independent model");
        if (throw_on_evaluate) throw std::runtime_error("fixture refusal");
        return evaluate_ok;
    }
    void restore_clamp() { calls.push_back("restore clamp"); clamp_is_readable=false; }
    void restore_first() { calls.push_back("restore first"); first_is_readable=false; }
};

int main() {
    unsigned checks{}, failures{};
    const auto check=[&](bool value, const char* name) {
        ++checks; if (!value) { ++failures; std::printf("FAIL %s\n",name); }
    };
    Operations off;
    check(DlssNr::RunOptionalSecondPass(false,off) && off.calls.empty(),
        "disabled stage records no commands or callbacks");
    Operations good;
    check(DlssNr::RunOptionalSecondPass(true,good),"successful second stage accepted");
    check(good.calls==std::vector<std::string>{"first readable","clamp same dimensions",
        "clamp readable","second independent model","restore clamp","restore first"},
        "clamp and model ordered with exact resource restoration");
    check(!good.first_is_readable && !good.clamp_is_readable,"successful state reusable");
    Operations clamp_failure; clamp_failure.clamp_ok=false;
    check(!DlssNr::RunOptionalSecondPass(true,clamp_failure) &&
        clamp_failure.calls==std::vector<std::string>{"first readable","clamp same dimensions","restore first"},
        "clamp refusal never invokes second model or restores untransitioned clamp");
    Operations model_failure; model_failure.evaluate_ok=false;
    check(!DlssNr::RunOptionalSecondPass(true,model_failure) &&
        !model_failure.first_is_readable && !model_failure.clamp_is_readable,
        "model refusal restores both temporary read states");
    Operations exception; exception.throw_on_evaluate=true;
    try { DlssNr::RunOptionalSecondPass(true,exception); check(false,"exception propagated"); }
    catch (const std::runtime_error&) {
        check(!exception.first_is_readable && !exception.clamp_is_readable,
            "exception retains resource-state restoration");
    }
    Operations left,right;
    DlssNr::RunOptionalSecondPass(true,left);
    check(right.calls.empty(),"another eye's independent stage is untouched");
    std::printf("Neural second-pass native callback orchestration: %u/%u passed\n",checks-failures,checks);
    return failures?1:0;
}
