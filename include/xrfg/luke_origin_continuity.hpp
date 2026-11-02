#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <numeric>
namespace xrfg {
struct LukeOriginSequenceFrame {
    std::uint64_t stream{},epoch{},publication{},native{},owner{};
    std::uint32_t token{},eye{};
    std::array<std::uint64_t,2> captures{},contexts{};
    std::array<std::uint32_t,4> shape{};
};
struct LukeOriginSequencePrediction {
    LukeOriginSequenceFrame frame;
    std::uint64_t age_ms{};
};
// GTA experiment explicitly requested by the owner. This is NOT a decoded CRC.
// Learn only from fresh CRC-confirmed source productions. Continue at most one
// second using actual NEW eye captures and exact registered native guide keys.
class LukeOriginContinuity {
    LukeOriginSequenceFrame anchor_{};
    std::uint64_t confirmed_ms_{};
    unsigned confirmations_{};
    bool inferred_{};
    struct Step {std::uint64_t numerator{1},denominator{1};};
    Step publication_step_,token_step_,native_step_;
    static Step step(std::uint64_t amount,std::uint64_t captures) noexcept {
        const auto divisor=std::gcd(amount,captures);return {amount/divisor,captures/divisor};
    }
    static bool advance(std::uint64_t& value,std::uint64_t delta,Step step,std::uint64_t maximum) noexcept {
        if(!step.denominator||delta%step.denominator)return false;
        delta/=step.denominator;
        if(!step.numerator||delta>(maximum-value)/step.numerator)return false;
        value+=delta*step.numerator;return true;
    }
    static bool valid(const LukeOriginSequenceFrame& f) noexcept {
        return f.stream&&f.epoch&&f.owner&&f.publication>1&&f.native>1&&f.token>1&&f.eye<2&&
            f.contexts[0]&&f.contexts[1]&&f.captures[0]&&f.captures[1]&&
            f.shape[0]&&f.shape[1]&&f.shape[2]&&f.shape[3];
    }
    std::optional<LukeOriginSequenceFrame> project(unsigned eye,
        std::array<std::uint64_t,2> captures,std::array<std::uint64_t,2> contexts) const noexcept {
        if(!confirmations_||eye>1||contexts!=anchor_.contexts||captures[0]<anchor_.captures[0]||
            captures[1]<anchor_.captures[1])return {};
        const auto a=captures[anchor_.eye]-anchor_.captures[anchor_.eye];
        const auto b=captures[1-anchor_.eye]-anchor_.captures[1-anchor_.eye];
        if(a>b||b-a>1||a>10000||b>10000)return {};
        const auto delta=a+b;
        if(!delta||eye!=(anchor_.eye^(delta&1)))return {};
        auto f=anchor_;f.eye=eye;f.captures=captures;
        std::uint64_t token=f.token;
        if(!advance(token,delta,token_step_,std::numeric_limits<std::uint32_t>::max())||
            !advance(f.publication,delta,publication_step_,UINT64_MAX)||
            !advance(f.native,delta,native_step_,UINT64_MAX))return {};
        f.token=static_cast<std::uint32_t>(token);return f;
    }
public:
    static bool matches(const LukeOriginSequenceFrame& expected,const LukeOriginSequenceFrame& actual) noexcept {
        // A temporal NGX reset may change epoch; its OWN fresh history must be
        // resolved normally. Never borrow old-epoch cameras/vectors here.
        return valid(actual)&&actual.stream==expected.stream&&actual.owner==expected.owner&&
            actual.epoch>=expected.epoch&&actual.publication==expected.publication&&
            actual.native==expected.native&&actual.token==expected.token&&actual.eye==expected.eye&&
            actual.contexts==expected.contexts&&actual.captures==expected.captures&&actual.shape==expected.shape;
    }
    // Returns true only when speculative AFW history must be retired on CRC
    // resynchronization. Duplicate/old CRCs cannot renew the one-second lease.
    bool confirm(const LukeOriginSequenceFrame& actual,bool complete_pair,std::uint64_t now) noexcept {
        if(!valid(actual))return false;
        if(confirmations_&&actual.stream==anchor_.stream&&actual.publication<=anchor_.publication)return false;
        const auto expected=project(actual.eye,actual.captures,actual.contexts);
        bool same=expected&&matches(*expected,actual)&&now>=confirmed_ms_;
        // Per-eye SR streams can advance native IDs at a different rate from
        // the global publication/token counters. Learn only from real CRCs.
        // A bank below requires a further confirmation before any inference.
        if(!same&&confirmations_==1&&complete_pair&&expected&&now>=confirmed_ms_&&
            actual.stream==anchor_.stream&&actual.owner==anchor_.owner&&actual.epoch>=anchor_.epoch&&
            actual.eye==expected->eye&&actual.contexts==anchor_.contexts&&actual.shape==anchor_.shape&&
            actual.publication>anchor_.publication&&actual.token>anchor_.token&&actual.native>anchor_.native) {
            const auto delta=(actual.captures[0]-anchor_.captures[0])+(actual.captures[1]-anchor_.captures[1]);
            const auto p=actual.publication-anchor_.publication,t=std::uint64_t(actual.token-anchor_.token),n=actual.native-anchor_.native;
            if(delta&&p<=delta*10000&&t<=delta*10000&&n<=delta*10000) {
                publication_step_=step(p,delta);token_step_=step(t,delta);native_step_=step(n,delta);same=true;
            }
        }
        const bool reset=inferred_&&(!same||now<confirmed_ms_||now-confirmed_ms_>1000);
        if(!complete_pair&&!same){invalidate();return reset;}
        if(!same)publication_step_=token_step_=native_step_={};
        confirmations_=same?(confirmations_<4?confirmations_+1:4):1;
        anchor_=actual;confirmed_ms_=now;inferred_=false;return reset;
    }
    std::optional<LukeOriginSequencePrediction> predict(std::uint64_t now,unsigned eye,
        std::array<std::uint64_t,2> captures,std::array<std::uint64_t,2> contexts,
        unsigned* refusal=nullptr) const noexcept {
        if(refusal)*refusal=0;
        const auto reject=[&](unsigned reason)->std::optional<LukeOriginSequencePrediction>{if(refusal)*refusal=reason;return {};};
        if(confirmations_<2)return reject(1);
        if(now<confirmed_ms_)return reject(3);
        if(now-confirmed_ms_>1000)return reject(2);
        const auto f=project(eye,captures,contexts);if(!f)return reject(4);
        return LukeOriginSequencePrediction{*f,now-confirmed_ms_};
    }
    unsigned confirmations() const noexcept {return confirmations_;}
    std::uint64_t age(std::uint64_t now) const noexcept {return now>=confirmed_ms_?now-confirmed_ms_:0;}
    void used() noexcept {inferred_=true;} // never refreshes confirmed_ms_
    bool invalidate() noexcept {const bool used=inferred_;*this={};return used;}
};
// One independently learned producer phase per logical eye. This supports a
// shared SR feature and two alternating features without assigning one eye's
// IDs to the other. Three real confirmations are required per model.
class LukeOriginContinuityBank {
    std::array<LukeOriginContinuity,2> eyes_;
public:
    bool confirm(const LukeOriginSequenceFrame& frame,bool complete,std::uint64_t now) noexcept {
        return frame.eye<2?eyes_[frame.eye].confirm(frame,complete,now):false;
    }
    std::optional<LukeOriginSequencePrediction> predict(std::uint64_t now,unsigned eye,
        std::array<std::uint64_t,2> captures,std::array<std::uint64_t,2> contexts,unsigned* refusal=nullptr) const noexcept {
        if(eye>1||eyes_[eye].confirmations()<3) {if(refusal)*refusal=1;return {};}
        return eyes_[eye].predict(now,eye,captures,contexts,refusal);
    }
    unsigned confirmations(unsigned eye) const noexcept {return eye<2?eyes_[eye].confirmations():0;}
    std::uint64_t age(unsigned eye,std::uint64_t now) const noexcept {return eye<2?eyes_[eye].age(now):0;}
    void used(unsigned eye) noexcept {if(eye<2)eyes_[eye].used();}
    bool invalidate() noexcept {const bool a=eyes_[0].invalidate(),b=eyes_[1].invalidate();return a||b;}
};
} // namespace xrfg
