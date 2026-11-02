#pragma once
#include "xrfg/autonomous_control.hpp"
#include <algorithm>
#include <vector>
#include <string>
#include <cmath>
#include <limits>
#include <optional>
#include <tuple>

namespace xrfg::autonomous {
inline constexpr std::uint64_t kBenchmarkSettleMs=2500;
inline constexpr std::uint64_t kBenchmarkMeasureMs=5000;
inline constexpr std::uint64_t kBenchmarkDurationMs=kBenchmarkSettleMs+kBenchmarkMeasureMs;
inline constexpr std::uint64_t kBenchmarkMemoryRequestMs=kBenchmarkDurationMs-500;
struct BenchmarkStep {
    int generation{}, backend{}, a{}, b{};
    bool triple{}, prefer{}, disabled{};
    std::string label;
    int afw{-1},shading{-1};
    int pacing{-1}; // -1 preserves the pre-V614 runtime's global choice.
    int buffer{-1}; // -1 keeps preference; 0=two, 1=three deliveries (private transport only).
};
inline std::vector<BenchmarkStep> legacy_benchmark_plan(bool full,bool luke=false) {
    std::vector<BenchmarkStep> steps;
    for(const int quality : (luke ? std::vector<int>{0,4,8} : std::vector<int>{-1})) {
        BenchmarkStep off{0,0,0,0,false,false,true,luke?"AFW only":"Disabled"};
        off.pacing=0;if(luke){off.afw=3;off.shading=quality;}steps.push_back(off);
        const auto route=[&](const char* name,int generation,int backend,bool triple,bool fast) {
            BenchmarkStep s{generation,backend,luke?0:int(fast),luke?0:int(fast),triple,triple,false,
                std::string(name)+(luke?" 00":fast?" Fast":" Normal")};
            s.pacing=0;if(luke){s.afw=3;s.shading=quality;}steps.push_back(std::move(s));
        };
        for(auto [name,backend]:{std::pair{"0.2.1",1},std::pair{"Djules 2X",3}}) {
            route(name,0,backend,false,false);
            if(!luke)route(name,0,backend,false,true);
        }
        if(!luke){route("Djules 3X",0,3,true,false);route("Djules 3X",0,3,true,true);}
        route("0.3.0",0,2,false,false);if(!luke)route("0.3.0",0,2,false,true);
        if(full&&!luke)for(auto [name,mode]:{std::pair{"Hybrid",3},std::pair{"FSR",1},std::pair{"DLSS-G",2}}) {
            route(name,mode,0,false,false);route(name,mode,0,false,true);
        }
    }
    return steps;
}
inline const char* benchmark_pacing_label(int mode) {
    return mode==0 ? "Framework" : mode==1 ? "Director" : "Framework+director";
}
inline std::vector<BenchmarkStep> benchmark_plan(bool full,bool luke=false,bool steamvr=true,bool advanced=false) {
    // The session-latched transport selects the new matrix or Legacy VDXR plan.
    if(!steamvr)return legacy_benchmark_plan(full,luke);
    std::vector<BenchmarkStep> steps;
    const auto route=[&](const char* name,int generation,int backend,bool triple,int quality) {
        for(int pacing=0;pacing<(advanced?3:2);++pacing) {
          for(int buffer=0;buffer<(advanced?1:2);++buffer) {
            for(int fast=0;fast<(luke ? 1 : 2);++fast) {
                BenchmarkStep step{generation,backend,fast,fast,triple,triple,false,
                    std::string(name)+" "+benchmark_pacing_label(pacing)+(luke ? "" : fast ? " Fast" : " Normal")+
                        (buffer ? " Buffer" : "")};
                step.pacing=pacing;
                if(!advanced)step.buffer=buffer;
                if(luke) {step.afw=3;step.shading=quality;}
                steps.push_back(std::move(step));
            }
          }
        }
    };
    for(const int quality : (luke ? std::vector<int>{0,4,8} : std::vector<int>{-1})) {
        BenchmarkStep off{0,0,0,0,false,false,true,luke ? "AFW only" : "Disabled"};
        off.pacing=0;
        if(!advanced)off.buffer=0; // one reference, never repeat it for buffer/fast/pacing
        if(luke) {off.afw=3;off.shading=quality;}
        steps.push_back(std::move(off));
        route("0.2.1",0,1,false,quality);
        route("Djules 2X",0,3,false,quality);
        if(!luke)route("Djules 3X",0,3,true,quality);
        route("0.3.0",0,2,false,quality);
        if(full && !luke) {route("Hybrid",3,0,false,quality);route("FSR",1,0,false,quality);route("DLSS-G",2,0,false,quality);}
    }
    return steps;
}
inline bool benchmark_afw_quality_allowed(int shading,std::uint32_t eye_long_side) {
    return shading==0 || (shading==4 && eye_long_side>=3000) || (shading==8 && eye_long_side>=4000);
}
inline std::vector<BenchmarkStep> benchmark_execution_plan(bool full,bool luke,bool steamvr,bool advanced,
    std::uint32_t eye_long_side) {
    auto steps=benchmark_plan(full,luke,steamvr,advanced);
    if(luke)std::erase_if(steps,[&](const auto& step) {
        return !benchmark_afw_quality_allowed(step.shading,eye_long_side);
    });
    return steps;
}
inline std::vector<BenchmarkStep> benchmark_export_plan(bool full,bool luke,bool steamvr,bool advanced=false) {
    auto steps=benchmark_plan(full,luke,steamvr,advanced);
    if(luke) {
        // Execution minimizes AFW resets with whole-quality passes. Export groups
        // every route/pacing's three qualities without relying on a fixed route count.
        const auto stride=steps.size()/3;
        std::vector<BenchmarkStep> ordered;
        for(std::size_t trial=0;trial<stride;++trial)
            for(std::size_t quality=0;quality<3;++quality)ordered.push_back(steps[quality*stride+trial]);
        return ordered;
    }
    return steps;
}
inline Settings benchmark_settings(Settings saved,const BenchmarkStep& step) {
    select_generation(saved,step.generation,step.backend);
    saved.enabled=!step.disabled;
    saved.motion_vectors=step.generation ? 1 : 0;
    saved.separate_synthesis=step.a;saved.lower_vram=step.b;
    saved.triple_frame_gen=step.triple;saved.prefer_fps=step.prefer;
    if(step.pacing>=0)saved.pacing_mode=step.pacing;
    if(step.buffer>=0)saved.increase_buffer=step.buffer!=0;
    if(step.afw>=0) {saved.afw_mode=step.afw;saved.afw_shading_rate=step.shading;}
    return sanitize(saved);
}
inline double benchmark_median(std::vector<double> values) {
    if(values.empty()) return 0;
    std::sort(values.begin(),values.end());
    const auto middle=values.size()/2;
    return values.size()%2 ? values[middle] : (values[middle-1]+values[middle])/2;
}
struct BenchmarkSamples {
    std::vector<double> fps,latency,refresh_hz;
    std::uint64_t last_ms{};
    bool generated{};
    void observe(const InterfaceTelemetry& m,std::uint64_t started,std::uint64_t end) {
        if(!m.updated_ms || m.updated_ms==last_ms || m.updated_ms<started+kBenchmarkSettleMs || m.updated_ms>=end) return;
        last_ms=m.updated_ms;
        if((m.output_source==InterfaceSource::steamvr_delivery ||
            m.output_source==InterfaceSource::steamvr_scene_delivery ||
            m.output_source==InterfaceSource::accepted_scene_submissions ||
            m.output_source==InterfaceSource::runtime_accepted_delivery) && m.rates_valid &&
            std::isfinite(m.output_fps) && m.output_fps>=0) fps.push_back(m.output_fps);
        if(m.cycle_estimated_valid && std::isfinite(m.cycle_estimated_ms) && m.cycle_estimated_ms>=0)
            latency.push_back(m.cycle_estimated_ms);
        generated |= m.generating;
        if(std::isfinite(m.display_hz) && m.display_hz>0)refresh_hz.push_back(m.display_hz);
    }
};
struct BenchmarkScore {
    double fps{}, latency{}, fps_range{}, latency_range{};
    bool valid{};
    bool vectors{}, afw_only{};
    int shading{};
    std::uint32_t eye_long_side{};
    double display_hz{};
    bool triple{};
};
inline BenchmarkScore benchmark_score(const BenchmarkSamples& samples,bool complete) {
    // A partial/failed trial, unknown timing or a single sample cannot establish
    // both performance and stability. Never turn missing measurements into zero.
    const auto measured=[](const auto& values) {
        return values.size()>=2 && std::all_of(values.begin(),values.end(),[](double x) {
            return std::isfinite(x) && x>0;
        });
    };
    if(!complete || !measured(samples.fps) || !measured(samples.latency))return {};
    const auto fps=std::minmax_element(samples.fps.begin(),samples.fps.end());
    const auto latency=std::minmax_element(samples.latency.begin(),samples.latency.end());
    return {benchmark_median(samples.fps),benchmark_median(samples.latency),
        *fps.second-*fps.first,*latency.second-*latency.first,true};
}
inline std::optional<std::size_t> benchmark_best(const std::vector<BenchmarkScore>& scores,bool luke=false) {
    // Anchor tolerances to the global maxima/minima, not pairwise comparisons:
    // a chain of near-ties must not select a materially slower configuration.
    constexpr double epsilon=1e-6;
    std::vector<std::size_t> pool;
    for(std::size_t i=0;i<scores.size();++i) {
        const auto& s=scores[i];if(!s.valid)continue;
        if(luke && !benchmark_afw_quality_allowed(s.shading,s.eye_long_side))continue;
        pool.push_back(i);
    }
    // AFW-only is an explicit quality/latency preference, before the ordinary
    // full-refresh lock. Both qualities must independently reach 90% of Hz.
    if(luke) {
        std::vector<std::size_t> afw;
        for(const auto i:pool)if(scores[i].afw_only && scores[i].display_hz>0 &&
            scores[i].fps+1e-3>=scores[i].display_hz*.9)afw.push_back(i);
        if(!afw.empty()) {
            const bool full=std::any_of(afw.begin(),afw.end(),[&](auto i){return scores[i].shading==0;});
            if(full)std::erase_if(afw,[&](auto i){return scores[i].shading!=0;});
            pool=std::move(afw);
        }
    }
    const auto at_refresh=[&](const auto& score) {
        return score.display_hz>0 && score.fps+1e-3>=score.display_hz;
    };
    const bool capped_pool=std::any_of(pool.begin(),pool.end(),[&](auto i) {return at_refresh(scores[i]);});
    if(capped_pool)std::erase_if(pool,[&](auto i) {return !at_refresh(scores[i]);});
    const bool normal_sufficient=std::any_of(pool.begin(),pool.end(),[&](auto i) {
        const auto& s=scores[i];return !s.triple && (s.display_hz<=0 || s.fps+1e-3>=s.display_hz*.66);
    });
    std::erase_if(pool,[&](auto i){return scores[i].triple && (normal_sufficient || scores[i].display_hz<=0);});
    if(luke) {
        const bool full_quality=std::any_of(pool.begin(),pool.end(),[&](auto i) {
            return scores[i].shading==0 && scores[i].display_hz>0 && scores[i].fps+1e-3>=scores[i].display_hz*.9;
        });
        if(full_quality)std::erase_if(pool,[&](auto i){return scores[i].shading!=0;});
    }
    const bool optical_at_refresh=std::any_of(pool.begin(),pool.end(),[&](auto i) {
        return !scores[i].vectors && at_refresh(scores[i]);
    });
    // A measured full-refresh optical route cannot lose to a sub-refresh
    // vector result, even inside the ordinary one-FPS tie group.
    if(optical_at_refresh)std::erase_if(pool,[&](auto i) {
        return scores[i].vectors && !at_refresh(scores[i]);
    });
    const auto keep_fps=[&](auto& candidates) {
        if(capped_pool)return; // all remaining candidates deliver the display cap
        double highest=0;
        for(const auto i:candidates)highest=std::max(highest,scores[i].fps);
        std::erase_if(candidates,[&](auto i){return highest-scores[i].fps>1+epsilon;});
    };
    // Preserve the ordinary 1 FPS group. Only the optical-to-vector comparison
    // may widen to 3 FPS; vector-to-vector comparisons still use 1 FPS.
    auto ordinary=pool;keep_fps(ordinary);
    double optical_latency=std::numeric_limits<double>::infinity();
    double optical_fps=0;
    const auto& optical_candidates=optical_at_refresh?pool:ordinary;
    for(const auto i:optical_candidates)if(!scores[i].vectors && (!optical_at_refresh || at_refresh(scores[i]))) {
        optical_latency=std::min(optical_latency,scores[i].latency);
        optical_fps=std::max(optical_fps,scores[i].fps);
    }
    std::vector<std::size_t> vectors;
    if(optical_fps>0)for(const auto i:pool)if(scores[i].vectors &&
        (optical_at_refresh ? at_refresh(scores[i]) : optical_fps-scores[i].fps<=3+epsilon) &&
        scores[i].latency<=optical_latency+12+epsilon)vectors.push_back(i);
    if(!vectors.empty()) {pool=std::move(vectors);keep_fps(pool);}
    else pool=std::move(ordinary);
    double lowest=std::numeric_limits<double>::infinity();
    for(const auto i:pool)lowest=std::min(lowest,scores[i].latency);
    std::optional<std::size_t> best;
    for(const auto i:pool) {
        const auto& score=scores[i];
        if(score.latency-lowest>2+epsilon)continue;
        const auto order=[](const auto& s) {return std::tuple{s.fps_range,s.latency_range,s.latency,-s.fps};};
        if(!best || order(score)<order(scores[*best]))best=i;
    }
    return best;
}
// Skip only after a completed, measured 2X trial of one of the two early OFXR routes.
inline bool benchmark_2x_sufficient(const Settings& settings,const BenchmarkScore& score) {
    return score.valid && settings.enabled && settings.frame_generation==0 &&
        (settings.backend==1 || settings.backend==3) && !settings.triple_frame_gen &&
        score.display_hz>0 && score.fps+1e-3>=score.display_hz*.66;
}
inline bool benchmark_luke_early_complete(const std::vector<BenchmarkScore>& scores) {
    const auto winner=benchmark_best(scores,true);
    if(!winner)return false;
    const auto& score=scores[*winner];
    return score.display_hz>0 && (score.afw_only ? score.fps+1e-3>=score.display_hz*.9 :
        score.fps+1e-3>=score.display_hz-1.0);
}
// UI commands; the tray/desktop timer drives the suite while menus are hidden.
bool start_benchmark(bool full,bool luke=false) noexcept;
void stop_benchmark() noexcept;
void tick_benchmark(std::uint64_t now_ms=0) noexcept;
} // namespace xrfg::autonomous
