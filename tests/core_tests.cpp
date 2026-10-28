#include "xrfg/image.hpp"
#include "xrfg/generation_backpressure.hpp"
#include "xrfg/pose.hpp"
#include "xrfg/luke_guide_eye.hpp"
#include "xrfg/luke_guide_window.hpp"
#include "xrfg/luke_physical_projection.hpp"
#include "xrfg/luke_pose_camera.hpp"
#include "xrfg/luke_native_motion_interval.hpp"
#include "xrfg/luke_world_scale.hpp"
#include "xrfg/luke_camera_units.hpp"
#include "xrfg/luke_camera_origin.hpp"
#include "xrfg/luke_ingress_devices.hpp"
#include "xrfg/luke_afw_startup.hpp"
#include "xrfg/native_ingress/RealVrDlssCamera.h"

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

[[nodiscard]] bool near(float left, float right, float epsilon = 1.0e-4F) {
    return std::abs(left - right) <= epsilon;
}

[[nodiscard]] xrfg::Quaternion yaw(float degrees) {
    const float half_angle = degrees * std::numbers::pi_v<float> / 360.0F;
    return {0.0F, std::sin(half_angle), 0.0F, std::cos(half_angle)};
}

}  // namespace

int main() {
    {
        using Key = xrfg::LukeAfwStartupKey;
        xrfg::LukeAfwStartupRendezvous startup;
        const Key key{1000001, 7};
        expect(!startup.pending(), "held/menu-only startup cannot arm a rendezvous");
        startup.observe_fresh(Key{}, 1);
        startup.observe_fresh(key, 0);
        expect(!startup.pending(), "invalid/loading metadata cannot arm startup");
        startup.observe_fresh(key, 100);
        expect(!startup.pending(), "one fresh menu frame cannot arm startup");
        // Held frames do not call observe_fresh: repeated polls preserve the
        // candidate, so alternating fresh/held still arms after three fresh.
        for (unsigned held = 0; held < 100; ++held)
            expect(!startup.pending(), "held submissions cannot count toward startup");
        startup.observe_fresh(key, 101);
        expect(!startup.pending(), "two fresh frames do not arm startup");
        startup.observe_fresh(key, 102);
        expect(startup.pending() == key, "three contiguous fresh frames arm one rendezvous");
        startup.complete(Key{1000001, 8}, true);
        expect(startup.pending() == key, "stale completion cannot consume a different key");
        startup.complete(key, true);
        expect(!startup.pending(), "successful startup reconfiguration closes its request");
        startup.complete(key, false);
        startup.observe_failure();
        for (std::uint64_t publication = 103; publication < 200; ++publication)
            startup.observe_fresh(key, publication);
        expect(!startup.pending(), "same completed key never forces again after frames or failures");

        const Key epoch{1000001, 8};
        startup.observe_fresh(epoch, 200);
        startup.observe_fresh(epoch, 201);
        expect(!startup.pending(), "scene cut cannot rearm a completed producer startup");
        startup.observe_fresh(epoch, 205);
        startup.observe_fresh(epoch, 206);
        startup.observe_fresh(epoch, 207);
        expect(!startup.pending(), "fresh post-cut history must not drain or reset the route again");
        // Retry/gap behaviour still applies to a genuinely new producer.
        const Key retry_stream{1500001, 8};
        startup.observe_fresh(retry_stream, 205);
        startup.observe_fresh(retry_stream, 206);
        expect(!startup.pending(), "new producer requires its own fresh streak");
        startup.observe_fresh(retry_stream, 209);
        startup.observe_fresh(retry_stream, 210);
        expect(!startup.pending(), "publication gaps discard the earlier candidate");
        startup.observe_fresh(retry_stream, 211);
        expect(startup.pending() == retry_stream, "three frames after a gap arm the new producer");
        startup.observe_failure();
        expect(!startup.pending(), "missing/admission failure disarms a pending candidate");
        startup.observe_fresh(retry_stream, 212);
        startup.observe_fresh(retry_stream, 213);
        expect(!startup.pending(), "failure recovery starts a new three-fresh streak");
        startup.observe_fresh(retry_stream, 214);
        startup.complete(retry_stream, false);
        expect(!startup.pending(), "failed reconfiguration cannot immediately retry");
        startup.observe_fresh(retry_stream, 215);
        startup.observe_fresh(retry_stream, 216);
        expect(!startup.pending(), "failed force requires three new fresh frames before retry");
        startup.observe_fresh(retry_stream, 217);
        expect(startup.pending() == retry_stream, "first failure permits one bounded retry");
        startup.complete(retry_stream, false);
        for (std::uint64_t publication = 218; publication < 300; ++publication)
            startup.observe_fresh(retry_stream, publication);
        expect(!startup.pending(), "two failed attempts cannot cause a per-frame retry loop");

        const Key new_stream{2000001, 8};
        startup.observe_fresh(new_stream, 400);
        startup.observe_fresh(new_stream, 399);
        startup.observe_fresh(new_stream, 400);
        expect(!startup.pending(), "nonmonotonic publication resets the new stream candidate");
        startup.observe_fresh(new_stream, 401);
        expect(startup.pending() == new_stream, "new producer stream has independent startup state");
        startup.complete(new_stream, true);

        const Key overflow{2500001, 9};
        startup.observe_fresh(overflow, std::numeric_limits<std::uint64_t>::max() - 1);
        startup.observe_fresh(overflow, std::numeric_limits<std::uint64_t>::max());
        startup.observe_fresh(overflow, 1);
        expect(!startup.pending(), "publication wraparound cannot manufacture continuity");

        xrfg::LukeAfwStartupRendezvous changed_candidate;
        changed_candidate.observe_fresh(key, 10);
        changed_candidate.observe_fresh(key, 11);
        changed_candidate.observe_fresh(epoch, 12);
        changed_candidate.observe_fresh(epoch, 13);
        expect(!changed_candidate.pending(), "two epochs cannot share a startup candidate streak");
        changed_candidate.observe_fresh(epoch, 14);
        changed_candidate.observe_failure(); // AFW Off/OFXR On cancels startup.
        changed_candidate.complete(epoch, true);
        expect(!changed_candidate.pending(), "disable cancels an armed request before it can complete");
        changed_candidate.observe_fresh(epoch, 15);
        changed_candidate.observe_fresh(epoch, 16);
        expect(!changed_candidate.pending(), "re-enable cannot reuse a disabled candidate streak");
        changed_candidate.observe_fresh(epoch, 17);
        expect(changed_candidate.pending() == epoch, "re-enable requires three new fresh submissions");

        xrfg::LukeAfwStartupRendezvous duplicate_candidate;
        duplicate_candidate.observe_fresh(key, 300);
        duplicate_candidate.observe_fresh(key, 300);
        duplicate_candidate.observe_fresh(key, 301);
        expect(!duplicate_candidate.pending(), "duplicate publication cannot count twice");
        duplicate_candidate.observe_fresh(key, 302);
        expect(duplicate_candidate.pending() == key, "three unique contiguous publications qualify startup");
        duplicate_candidate.complete(key, true);
        // F6, threshold and mode changes do not change producer stream/epoch.
        duplicate_candidate.observe_fresh(key, 303);
        duplicate_candidate.observe_fresh(key, 304);
        duplicate_candidate.observe_fresh(key, 305);
        expect(!duplicate_candidate.pending(), "AFW UI changes cannot rearm a completed producer context");

        xrfg::LukeAfwStartupRendezvous ordinary_drain;
        ordinary_drain.satisfy(Key{});
        ordinary_drain.observe_fresh(key, 500);
        ordinary_drain.satisfy(key);
        ordinary_drain.observe_failure();
        for (std::uint64_t publication = 501; publication < 510; ++publication)
            ordinary_drain.observe_fresh(key, publication);
        expect(!ordinary_drain.pending(), "ordinary successful hot On-to-Off drain satisfies startup without a force");
        ordinary_drain.observe_fresh(epoch, 510);
        ordinary_drain.observe_fresh(epoch, 511);
        expect(!ordinary_drain.pending(), "ordinary drain remains satisfied across a scene cut");
        ordinary_drain.observe_fresh(epoch, 512);
        expect(!ordinary_drain.pending(), "three post-cut frames cannot reset the metrics via startup");
    }
    {
        xrfg::LukeCameraOrigin origin;
        const std::array<float, 3> left{795.964600F, 2448.13086F, -4583.30908F};
        const std::array<float, 3> right{795.918091F, 2448.17456F, -4583.30811F};
        const auto first = origin.update(1000001, 7, left);
        expect(first && *first && origin.position_relative(left) ==
            std::array<float, 3>{0, 0, 0},
            "first finite producer camera establishes a local AFW origin");
        const auto anchor = origin.origin();
        const auto peer = origin.update(1000001, 7, right);
        expect(peer && !*peer && origin.origin() == anchor,
            "opposite-eye publication preserves the same common origin");
        const auto relative_right = origin.position_relative(right);
        for (unsigned axis = 0; axis < 3; ++axis)
            expect(relative_right[axis] == static_cast<float>(
                static_cast<double>(right[axis]) - static_cast<double>(left[axis])),
                "local camera translation is subtracted in double and cast once");

        auto moved = left;
        moved[0] += 8.0F;
        moved[1] -= 4.0F;
        moved[2] += 2.0F;
        const auto movement = origin.update(1000001, 7, moved);
        expect(movement && !*movement && origin.origin() == anchor &&
            origin.position_relative(moved) == std::array<float, 3>{8, -4, 2} &&
            origin.position_relative(right) == relative_right,
            "camera motion preserves both the anchor and older eye history coordinates");

        // Rebase the captured DOOM-scale world, not just a zero-origin fixture.
        // Translating cameras and points by one origin leaves both eye view
        // coordinates identical, including an independently rotated camera.
        const xrfg::LukeCameraBasis basis{{
            {0.733587086F, -0.679424882F, -0.0152251674F},
            {0.0191475488F, 0.0430579595F, -0.998889148F},
            {0.6793257F, 0.732480705F, 0.0445960872F}}};
        const auto view_point = [&](const std::array<float, 3>& camera,
                                    const std::array<float, 3>& point) {
            std::array<double, 3> result{};
            for (unsigned row = 0; row < 3; ++row)
                for (unsigned axis = 0; axis < 3; ++axis)
                    result[row] += (static_cast<double>(point[axis]) -
                        static_cast<double>(camera[axis])) * basis[row][axis];
            return result;
        };
        for (const auto& camera : {left, right, moved}) {
            auto point = left;
            point[0] += 1.0F;
            point[1] += 2.0F;
            point[2] -= 5.0F;
            const auto before = view_point(camera, point);
            const auto after = view_point(origin.position_relative(camera),
                origin.position_relative(point));
            for (unsigned row = 0; row < 3; ++row)
                expect(std::abs(before[row] - after[row]) < 1.0e-9,
                    "common-origin rebasing preserves both eyes' world-to-view transforms");
        }

        for (const float invalid : {std::numeric_limits<float>::quiet_NaN(),
                 std::numeric_limits<float>::infinity(),
                 -std::numeric_limits<float>::infinity()}) {
            auto bad = right;
            bad[1] = invalid;
            expect(!origin.update(1000002, 8, bad, true) && origin.origin() == anchor,
                "nonfinite producer position cannot mutate the stream/epoch origin");
            expect(!std::isfinite(origin.position_relative(bad)[0]),
                "nonfinite history position is rejected instead of silently rebased");
        }
        const auto stream_change = origin.update(1000002, 7, right);
        expect(stream_change && *stream_change && origin.position_relative(right) ==
            std::array<float, 3>{0, 0, 0},
            "new producer stream invalidates the previous shared origin");
        const auto epoch_change = origin.update(1000002, 8, moved);
        expect(epoch_change && *epoch_change && origin.position_relative(moved) ==
            std::array<float, 3>{0, 0, 0},
            "new producer epoch invalidates the previous shared origin");
        const auto explicit_reset = origin.update(1000002, 8, left, true);
        expect(explicit_reset && *explicit_reset && origin.position_relative(left) ==
            std::array<float, 3>{0, 0, 0},
            "explicit producer reset reanchors even within an unchanged stream/epoch");
        const auto after_reset = origin.update(1000002, 8, right);
        expect(after_reset && !*after_reset,
            "ordinary publications after reset do not reanchor every frame");
        xrfg::LukeCameraOrigin uninitialized;
        expect(!std::isfinite(uninitialized.position_relative(left)[0]),
            "uninitialized origin does not manufacture usable camera coordinates");
        xrfg::LukeCameraOrigin paired;
        const std::array<std::uint64_t,4> producers{101,7,202,3};
        const auto paired_start=paired.update_producers(producers,left);
        const auto paired_anchor=paired.origin();
        const auto paired_peer=paired.update_producers(producers,right);
        expect(paired_start && *paired_start && paired_peer && !*paired_peer && paired.origin()==paired_anchor,
            "two original feature epochs preserve one AFW world origin across eye alternation");
        auto next_producers=producers;++next_producers[3];
        const auto paired_reset=paired.update_producers(next_producers,moved);
        expect(paired_reset && *paired_reset && paired.position_relative(moved)==std::array<float,3>{},
            "opposite native feature reset invalidates the common paired origin");
    }
    // Every combination of missing devices, optional depth, adapter and strict
    // queue/list identity must preserve the old guard's acceptance exactly.
    for (unsigned bits = 0; bits < 1024; ++bits) {
        const auto b = [&](unsigned i) { return (bits & (1u << i)) != 0; };
        const xrfg::LukeIngressDeviceFacts f{b(0),b(1),b(2),b(3),b(4),b(5),b(6),b(7),b(8),b(9)};
        const bool old_admitted = f.output_device && f.motion_device &&
            f.command_device && f.queue_device && f.output_adapter &&
            f.motion_adapter && f.queue_list_identity &&
            (!f.depth_present || (f.depth_device && f.depth_adapter));
        expect((xrfg::luke_ingress_device_failures(f) == 0) == old_admitted,
            "diagnostic device facts must not change admission");
    }
    expect(xrfg::luke_ingress_device_failures({true,true,true,true,true,true,true,true,true,false}) == 256,
        "queue/list identity is independently reported and still rejects");
    const std::array<float, 16> luke_projection{
        0.70020777F, 0.0F, 0.0F, 0.0F,
        0.0F, 0.70020777F, 0.0F, 0.0F,
        0.0F, 0.0F, 0.0000463724F, -1.0F,
        0.0F, 0.0F, 0.190008804F, 0.0F};
    const auto degrees = [](float value) {
        return value * std::numbers::pi_v<float> / 180.0F;
    };
    const auto left_window = xrfg::luke_guide_window(luke_projection,
        degrees(-54), degrees(40), degrees(44), degrees(-55),
        3344, 3344, 1672, 1672, 2594, 2803);
    const auto right_window = xrfg::luke_guide_window(luke_projection,
        degrees(-40), degrees(54), degrees(44), degrees(-55),
        3344, 3344, 1672, 1672, 2594, 2803);
    expect(left_window && right_window && left_window->x == 30 &&
        right_window->x == 345 && left_window->y == 270 &&
        right_window->y == 270 && left_window->width == 1297 &&
        left_window->height == 1402,
        "Horizon eye guide windows follow projection and physical FOV");
    const auto double_resolution = xrfg::luke_guide_window(luke_projection,
        degrees(-54), degrees(40), degrees(44), degrees(-55),
        6688, 6688, 3344, 3344, 5188, 5606);
    expect(double_resolution && double_resolution->x == 61 &&
        double_resolution->y == 541 && double_resolution->width == 2594 &&
        double_resolution->height == 2803,
        "guide window scales with resolution instead of using pixel offsets");
    const auto resized_eye=xrfg::luke_guide_window(luke_projection,
        degrees(-54), degrees(40), degrees(44), degrees(-55),
        3344, 3344, 1672, 1672, 3000, 2803);
    expect(resized_eye && resized_eye->x==left_window->x && resized_eye->width==left_window->width,
        "resized XR color preserves the native FOV sampling window");
    // Actual historical KCD raster values, accepted by both V33011 and the
    // V30098 endpoint contract: FOV spans2828.267 output pixels while color
    // contains2828. At half-resolution the integer crop is1414, not1415.
    const std::array<float,16> kcd_raster_projection{
        .698464334F,0,0,0,0,.854080677F,0,0,
        -.000132461719F,-.000359815429F,.0000062500385F,-1,
        0,0,.0500003137F,0};
    const float kcd_horizontal=std::atan(1.F/.932515085F);
    const float kcd_vertical=std::atan(1.F/.854080677F);
    const auto kcd_raster=xrfg::luke_guide_window(kcd_raster_projection,
        -kcd_horizontal,kcd_horizontal,kcd_vertical,-kcd_vertical,
        3776,3088,1888,1544,2828,3088);
    expect(kcd_raster&&kcd_raster->width==1414&&kcd_raster->height==1544&&
        1888.F*2828.F/float(kcd_raster->width)==3776.F,
        "accepted KCD raster rounding preserves exact crop and native motion gain");
    for(float focal:{std::nextafter(kcd_raster_projection[0],0.F),kcd_raster_projection[0],
            std::nextafter(kcd_raster_projection[0],1.F)})
        for(float jitter:{-.0005F,0.F,.0005F}) {
            auto varied=kcd_raster_projection;varied[0]=focal;varied[8]=jitter;varied[9]=jitter;
            const auto stable=xrfg::luke_guide_window(varied,
                -kcd_horizontal,kcd_horizontal,kcd_vertical,-kcd_vertical,
                3776,3088,1888,1544,2828,3088);
            expect(stable&&stable->width==1414&&stable->height==1544&&
                stable->x+stable->width<=1888&&stable->y+stable->height<=1544,
                "ordinary camera float noise and jitter do not grow a raster-aligned guide");
        }
    auto raster_unit=kcd_raster_projection;raster_unit[0]=raster_unit[5]=1.F;
    raster_unit[8]=raster_unit[9]=0;
    for(unsigned guide:{1U,511U,1115U,1672U,2229U,3344U})
        for(unsigned color:{256U,2594U,2803U,3344U})
            for(float rounding:{-.25F,0.F,.25F}) {
                const float angle=std::atan((float(color)+rounding)/3344.F);
                const auto exact=xrfg::luke_guide_window(raster_unit,-angle,angle,angle,-angle,
                    3344,3344,guide,guide,color,color);
                const auto accepted=static_cast<unsigned>((std::uint64_t(guide)*color+3343)/3344);
                expect(exact&&exact->width==accepted&&exact->height==accepted,
                    "all accepted raster ratios retain the exact original integer extent");
            }
    // Measured V506 KCD steady-state geometry. The odd color width occupies
    // 1346 guide texels, but that integer padding is not a motion resize.
    auto odd_projection=kcd_raster_projection;
    odd_projection[0]=.7265426F;odd_projection[5]=.8879965F;
    const float odd_horizontal=std::atan((2691.25F/3344.F)/odd_projection[0]);
    const float odd_vertical=std::atan((2254.25F/2736.F)/odd_projection[5]);
    const auto odd_window=[&](unsigned color_width,unsigned color_height) {
        return xrfg::luke_guide_window(odd_projection,-odd_horizontal,odd_horizontal,
            odd_vertical,-odd_vertical,3344,2736,1672,1368,color_width,color_height);
    };
    const auto odd=odd_window(2691,2254);
    expect(odd&&odd->width==1346&&odd->height==1127&&odd->raster_x&&odd->raster_y,
        "measured odd KCD crop records independent raster matches on both axes");
    if(odd) {
        expect(xrfg::luke_guide_motion_gain(1672.F,3344,1672,2691,odd->width,odd->raster_x)==3344.F&&
            xrfg::luke_guide_motion_gain(1368.F,2736,1368,2254,odd->height,odd->raster_y)==2736.F,
            "raster padding preserves accepted native motion units rather than shrinking X to 3342.7578125");
        expect(xrfg::luke_guide_motion_gain(-1672.F,3344,1672,2691,odd->width,odd->raster_x)==-3344.F,
            "accepted raster gain preserves the caller's negative motion sign");
        // These fixed IEEE-754 receipts distinguish V33011's two rounded
        // operations from dividing the extents first, including the 2:1 case.
        expect(std::bit_cast<std::uint32_t>(xrfg::luke_guide_motion_gain(
            std::bit_cast<float>(0x3ca2d41eU),3344,1672,2691,odd->width,true))==0x3d22d41dU,
            "raster gain preserves accepted float multiply-then-divide order");
        expect(std::bit_cast<std::uint32_t>(xrfg::luke_guide_motion_gain(
            -std::bit_cast<float>(0x3ca2d41eU),3344,1672,2691,odd->width,true))==0xbd22d41dU,
            "negative raster gain preserves the same exact float operation order");
        expect(std::bit_cast<std::uint32_t>(xrfg::luke_guide_motion_gain(
            std::bit_cast<float>(0x3c60857dU),3344,1115,2691,898,true))==0x3d28573bU,
            "nonintegral native resolution ratio preserves accepted float operation order");
    }
    const auto resized_x=odd_window(2600,2254),resized_y=odd_window(2691,2200);
    expect(resized_x&&!resized_x->raster_x&&resized_x->raster_y&&
        resized_y&&resized_y->raster_x&&!resized_y->raster_y,
        "one genuinely resized axis cannot change the other axis's raster motion contract");
    if(resized_x&&resized_y) {
        expect(xrfg::luke_guide_motion_gain(-1672.F,3344,1672,2600,resized_x->width,false)==
                -1672.F*2600.F/float(resized_x->width)&&
            xrfg::luke_guide_motion_gain(1368.F,2736,1368,2254,resized_x->height,true)==2736.F&&
            xrfg::luke_guide_motion_gain(1672.F,3344,1672,2691,resized_y->width,true)==3344.F&&
            xrfg::luke_guide_motion_gain(-1368.F,2736,1368,2200,resized_y->height,false)==
                -1368.F*2200.F/float(resized_y->height),
            "genuine per-axis resizing retains the prior color-to-crop gain and raw signs");
    }
    const auto fractional_resize=xrfg::luke_guide_window(kcd_raster_projection,
        -kcd_horizontal,kcd_horizontal,kcd_vertical,-kcd_vertical,
        3776,3088,1888,1544,2800,3088);
    expect(fractional_resize&&fractional_resize->width==1415&&fractional_resize->height==1544,
        "a genuinely resized eye retains its fractional projected footprint");
    xrfg::LukeGuideWindowFailure rejection{};
    expect(xrfg::luke_guide_window(luke_projection,
        degrees(-54), degrees(40), degrees(44), degrees(-55),
        3344, 3344, 1672, 1672, 3063, 3344, &rejection) &&
        rejection == xrfg::LukeGuideWindowFailure::none,
        "color width is independent of native projection sampling");
    expect(xrfg::luke_guide_window(luke_projection,
        degrees(-54), degrees(40), degrees(44), degrees(-55),
        3344, 3344, 1672, 1672, 2594, 3000, &rejection) &&
        rejection == xrfg::LukeGuideWindowFailure::none,
        "resized height does not invalidate physical camera FOV");
    for(unsigned guide_width:{1U,511U,1115U,1672U,2229U,3344U})
        for(unsigned eye_width:{256U,2594U,3056U,6688U})
            for(unsigned eye_height:{128U,2803U,3337U,3344U,6688U}) {
                const auto first=xrfg::luke_guide_window(luke_projection,
                    degrees(-54),degrees(40),degrees(44),degrees(-55),
                    3344,3344,guide_width,guide_width,eye_width,eye_height);
                expect(first && first->x+first->width<=guide_width && first->y+first->height<=guide_width,
                    "all guide ratios and independently resized XR eyes retain bounded physical sampling");
            }
    expect(xrfg::luke_guide_window(luke_projection,
        degrees(-54), degrees(40), degrees(44), degrees(-55),
        3344, 3344, 1672, 1672, 2594, 2803, &rejection) &&
        rejection == xrfg::LukeGuideWindowFailure::none,
        "accepted crop clears prior diagnostic reason and retains original window");
    // Exact V31001 PID36652 projection/FOV, not the Horizon-sized fixture.
    const std::array<float, 16> ghost_projection{
        0.8540806174278259F, 0, 0, 0,
        0, 0.8540806174278259F, 0, 0,
        0, 0, -0.0000010000009069699445F, 0.9999999403953552F,
        0, 0, 10.000008583068848F, 0};
    const auto ghost_window = xrfg::luke_guide_window(ghost_projection,
        -0.8203047513961792F, 0.8203047513961792F,
        0.8639379739761353F, -0.8639379739761353F,
        3344, 3344, 1672, 1672, 3063, 3344, &rejection);
    expect(ghost_window && ghost_window->x == 70 && ghost_window->y == 0 &&
        ghost_window->width == 1532 && ghost_window->height == 1672 &&
        rejection == xrfg::LukeGuideWindowFailure::none,
        "measured positive-Z Ghost projection admits the correct guide crop");
    expect(xrfg::luke_same_eye_native_motion_title(L"Spider-Man.exe") &&
        xrfg::luke_same_eye_native_motion_title(L"SPIDER-MAN.EXE") &&
        xrfg::luke_same_eye_native_motion_title(L"GhostOfTsushima.exe") &&
        xrfg::luke_same_eye_native_motion_title(L"HorizonForbiddenWest.exe") &&
        xrfg::luke_same_eye_native_motion_title(L"GTA5_Enhanced.exe") &&
        xrfg::luke_same_eye_native_motion_title(L"gta5_enhanced.EXE") &&
        !xrfg::luke_same_eye_native_motion_title(L"GTA5.exe") &&
        xrfg::luke_same_eye_native_motion_title(L"Dead Space.exe") &&
        xrfg::luke_same_eye_native_motion_title(L"DEAD SPACE.EXE") &&
        !xrfg::luke_same_eye_native_motion_title(L"DeadSpace.exe") &&
        xrfg::luke_same_eye_native_motion_title(L"Cyberpunk2077.exe") &&
        xrfg::luke_same_eye_native_motion_title(L"CYBERPUNK2077.EXE") &&
        !xrfg::luke_same_eye_native_motion_title(L"Cyberpunk2077Launcher.exe") &&
        xrfg::luke_same_eye_native_motion_title(L"afop.exe") &&
        xrfg::luke_same_eye_native_motion_title(L"AFOP.EXE") &&
        !xrfg::luke_same_eye_native_motion_title(L"Outlaws.exe") &&
        !xrfg::luke_same_eye_native_motion_title(L"Spider-Man2.exe"),
        "measured same-eye interval includes Remastered without guessing other games");
    {
        int game{}, injector{};
        using xrfg::LukeMotionSource;
        expect(xrfg::luke_motion_source(&game, &injector) == LukeMotionSource::replaced &&
            xrfg::luke_motion_source(&game, &game) == LukeMotionSource::original &&
            xrfg::luke_motion_source(nullptr, &game) == LukeMotionSource::unobserved &&
            xrfg::luke_motion_source(&game, nullptr) == LukeMotionSource::unobserved &&
            xrfg::luke_motion_span(LukeMotionSource::replaced) == 2 &&
            xrfg::luke_motion_span(LukeMotionSource::original) == 1 &&
            xrfg::luke_motion_span(LukeMotionSource::unobserved) == 1,
            "unlisted REAL VR motion is N-2 only when the typed call's vectors were replaced");
    }
    // Spider-Man Remastered PID28936: the native view uses down for Y,
    // compensated by a negative projection focal term. Physical XR up still
    // samples the upper raster half; rejecting the sign prevents all AFW.
    auto spidey_projection = ghost_projection;
    spidey_projection[0] = 0.8540806770324707F;
    spidey_projection[5] = -0.8540806770324707F;
    spidey_projection[10] = -1.0000099791795947e-05F;
    spidey_projection[11] = 1.F;
    spidey_projection[14] = 0.10000099241733551F;
    const auto spidey_window = xrfg::luke_guide_window(spidey_projection,
        -0.8203047513961792F, 0.8203047513961792F,
        0.8639379739761353F, -0.8639379739761353F,
        3344, 3344, 1672, 1672, 3063, 3344, &rejection);
    expect(spidey_window && spidey_window->x == 70 && spidey_window->y == 0 &&
        spidey_window->width == 1532 && spidey_window->height == 1672 &&
        rejection == xrfg::LukeGuideWindowFailure::none,
        "captured Spider-Man negative-Y projection must admit the physical crop");
    auto spidey_physical = spidey_projection;
    expect(xrfg::luke_physical_eye_projection(spidey_physical,
        -0.8203047513961792F, 0.8203047513961792F,
        0.8639379739761353F, -0.8639379739761353F) && spidey_physical[5] < 0,
        "physical projection preserves the producer's down-Y view convention");
    for (const float focal_sign : {-1.F, 1.F}) {
        auto p = ghost_projection;
        p[0] *= focal_sign; p[5] *= focal_sign;
        p[8] = .025F; p[9] = -.035F;
        const auto reflected = xrfg::luke_guide_window(p,
            -.7F, .65F, .6F, -.75F,3344,3344,1672,1672,2600,2800);
        auto reference = p; reference[0] = std::abs(p[0]); reference[5] = std::abs(p[5]);
        const auto positive = xrfg::luke_guide_window(reference,
            -.7F,.65F,.6F,-.75F,3344,3344,1672,1672,2600,2800);
        expect(reflected && positive && reflected->x == positive->x && reflected->y == positive->y &&
            reflected->width == positive->width && reflected->height == positive->height,
            "asymmetric FOV and off-axis centers preserve equivalent reflected-view crops");
    }
    auto equivalent_lh = luke_projection;
    for (unsigned column = 0; column < 4; ++column)
        equivalent_lh[8 + column] = -equivalent_lh[8 + column];
    const auto lh_window = xrfg::luke_guide_window(equivalent_lh,
        degrees(-54), degrees(40), degrees(44), degrees(-55),
        3344, 3344, 1672, 1672, 2594, 2803);
    expect(lh_window && left_window && lh_window->x == left_window->x &&
        lh_window->y == left_window->y && lh_window->width == left_window->width &&
        lh_window->height == left_window->height,
        "equivalent handedness preserves asymmetric crop geometry");
    auto ghost_physical = ghost_projection;
    expect(xrfg::luke_physical_eye_projection(ghost_physical,
        -0.8203047513961792F, 0.8203047513961792F,
        0.8639379739761353F, -0.8639379739761353F) &&
        near(static_cast<float>(xrfg::luke_projection_ray_ndc(
            std::tan(-0.8203047513961792F), ghost_physical[0],
            ghost_physical[8], ghost_physical[3], ghost_physical[11])), -1.0F) &&
        near(static_cast<float>(xrfg::luke_projection_ray_ndc(
            std::tan(0.8639379739761353F), ghost_physical[5],
            ghost_physical[9], ghost_physical[7], ghost_physical[11])), 1.0F),
        "positive-Z physical projection maps FOV edges to clip boundaries");
    auto degenerate_projection = ghost_projection;
    degenerate_projection[11] = 0;
    expect(!xrfg::luke_guide_window(degenerate_projection,
        -0.8203047513961792F, 0.8203047513961792F,
        0.8639379739761353F, -0.8639379739761353F,
        3344, 3344, 1672, 1672, 3063, 3344) &&
        !xrfg::luke_physical_eye_projection(degenerate_projection,
            -0.8203047513961792F, 0.8203047513961792F,
            0.8639379739761353F, -0.8639379739761353F),
        "missing perspective direction remains rejected");
    auto left_physical = luke_projection;
    auto right_physical = luke_projection;
    expect(xrfg::luke_physical_eye_projection(left_physical,
        degrees(-54), degrees(40), degrees(44), degrees(-55)) &&
        xrfg::luke_physical_eye_projection(right_physical,
        degrees(-40), degrees(54), degrees(44), degrees(-55)),
        "source and destination physical projections are computable");
    const auto projected_x = [](const std::array<float, 16>& matrix,
        float angle) {
        const double tangent = std::tan(angle);
        return (matrix[0] * tangent - matrix[8]) /
            (matrix[3] * tangent - matrix[11]);
    };
    const auto projected_y = [](const std::array<float, 16>& matrix,
        float angle) {
        const double tangent = std::tan(angle);
        return (matrix[5] * tangent - matrix[9]) /
            (matrix[7] * tangent - matrix[11]);
    };
    expect(near(left_physical[0], right_physical[0]) &&
        left_physical[8] < 0.0F && right_physical[8] > 0.0F &&
        near(static_cast<float>(projected_x(left_physical, degrees(-54))), -1) &&
        near(static_cast<float>(projected_x(left_physical, degrees(40))), 1) &&
        near(static_cast<float>(projected_x(right_physical, degrees(-40))), -1) &&
        near(static_cast<float>(projected_x(right_physical, degrees(54))), 1) &&
        near(static_cast<float>(projected_y(left_physical, degrees(44))), 1) &&
        near(static_cast<float>(projected_y(left_physical, degrees(-55))), -1),
        "physical-eye projections map each eye's angular edges to NDC edges");
    auto invalid_physical = luke_projection;
    expect(!xrfg::luke_physical_eye_projection(invalid_physical,
        degrees(0), degrees(0), degrees(44), degrees(-55)),
        "degenerate physical eye FOV is rejected");
    float game_baseline{};
    expect(xrfg::luke_game_eye_baseline(0.0643343F, 1.1F, 1.0F,
        game_baseline) && near(game_baseline, 0.0584857F),
        "metre-scale calibrated camera preserves the Horizon baseline");
    expect(!xrfg::luke_game_eye_baseline(0.0643343F, 0.0F, 1.0F,
        game_baseline), "missing Luke WorldScale rejects AFW camera");
    expect(xrfg::luke_game_eye_baseline(0.06406F, 1.0F, 100.0F, game_baseline) &&
        near(game_baseline, 6.406F), "centimetre-scale producer camera uses its measured units");
    expect(!xrfg::luke_game_eye_baseline(0.06406F, 1.0F, 0, game_baseline),
        "missing calibration never falls back to metres");
    {
        // REAL VR's DLSS-fix constants: a camera 3 m up, turned 90 degrees
        // (forward = world +x, right = world -z), with 100 degrees horizontal
        // and 70 vertical of field, near 0.25, far 4000, and the shader's uv
        // mapping. Its decode must give that pose and a projection whose
        // z/w is the game's depth and whose x/y come from uv.
        namespace rv = xrfg::native_ingress::real_vr_dlss_camera;
        std::array<unsigned char, rv::buffer_bytes> bytes{};
        const auto put = [&](std::size_t offset, float v) { std::memcpy(bytes.data() + offset, &v, 4); };
        const auto put_u = [&](std::size_t offset, std::uint32_t v) { std::memcpy(bytes.data() + offset, &v, 4); };
        // transf0 rows: world = M * (x, y, z, 1); columns right, up, forward, position.
        const float rows[16]{0, 0, 1, 10,   0, 1, 0, 3,   -1, 0, 0, -5,   0, 0, 0, 1};
        for (unsigned i = 0; i < 16; ++i) put(i * 4, rows[i]);
        const float tan_h = std::tan(50.F * 3.14159265F / 180), tan_v = std::tan(35.F * 3.14159265F / 180);
        const float near_plane = .25F, far_plane = 4000.F;
        // x/z = u * sx + ox over [-tan_h, tan_h]; y/z = v * sy + oy, v down, y up.
        put(304, 2 * tan_h); put(320, -tan_h);
        put(308, -2 * tan_v); put(324, tan_v);
        // z = near / (depth * sz + oz): reversed-Z style, depth 1 at near, 0 at far.
        const float sz = 1 - near_plane / far_plane, oz = near_plane / far_plane;
        put(312, sz); put(328, oz); put(316, near_plane); put(332, far_plane);
        put_u(256, 1920); put_u(260, 1080); put(264, 1920); put(268, 1080); put(272, 1 / 1920.F); put(276, 1 / 1080.F);
        put(288, 1920); put(292, 1080);
        expect(rv::looks_like(bytes.data(), bytes.size(), 1920, 1080) &&
            !rv::looks_like(bytes.data(), bytes.size(), 1280, 720), "REAL VR constants recognised by their field size");
        const auto camera = rv::decode(bytes.data(), bytes.size());
        const auto near3 = [](const std::array<float, 3>& a, float x, float y, float z) {
            return std::fabs(a[0] - x) < 1e-5F && std::fabs(a[1] - y) < 1e-5F && std::fabs(a[2] - z) < 1e-5F;
        };
        // The rotation is read transposed (see decode): rows of transf0 are the axes.
        expect(camera.valid && near3(camera.position, 10, 3, -5) && near3(camera.right, 0, 0, 1) &&
            near3(camera.up, 0, 1, 0) && near3(camera.forward, -1, 0, 0) && camera.near_z == near_plane && camera.far_z == far_plane,
            "REAL VR constants did not give the camera's pose");
        expect(std::fabs(camera.fov_vertical - 70.F * 3.14159265F / 180) < 1e-4F, "REAL VR constants did not give the field");
        // A view point at uv (0.3, 0.8), 12 m deep: through the projection its
        // x/w, y/w are that uv's NDC and its z/w the shader's depth value.
        const float z = 12.F, x = (.3F * 2 * tan_h - tan_h) * z, y = (.8F * -2 * tan_v + tan_v) * z;
        const auto& p = camera.view_to_clip;
        const float cx = x * p[0] + y * p[4] + z * p[8] + p[12], cy = x * p[1] + y * p[5] + z * p[9] + p[13];
        const float cz = x * p[2] + y * p[6] + z * p[10] + p[14], cw = x * p[3] + y * p[7] + z * p[11] + p[15];
        const float depth = (near_plane / z - oz) / sz;
        expect(std::fabs(cw - z) < 1e-4F && std::fabs(cx / cw - (.3F * 2 - 1)) < 1e-4F &&
            std::fabs(cy / cw - (1 - .8F * 2)) < 1e-4F && std::fabs(cz / cw - depth) < 1e-5F,
            "REAL VR projection does not reproduce the shader's uv and depth");
        // x mapped the other way (S.x < 0): the same screen, right negated.
        put(304, -2 * tan_h); put(320, tan_h);
        const auto mirrored = rv::decode(bytes.data(), bytes.size());
        expect(mirrored.valid && near3(mirrored.right, 0, 0, -1) && mirrored.view_to_clip[0] > 0 &&
            std::fabs(mirrored.view_to_clip[0] - p[0]) < 1e-5F && std::fabs(mirrored.view_to_clip[8] - p[8]) < 1e-5F,
            "a left-pointing view x was not normalised");
        // transf0 is the step from the frame REAL VR stepped from, whose own
        // transf0 is this frame's transf_2. Left eye, then the right eye 63 mm
        // to its right, then the left eye again after a 90 degree turn to the
        // right about the head: the chained world puts each camera where those
        // steps say.
        const auto step = [](std::array<double, 16> rows, std::array<double, 16> from, bool flip_x) {
            rv::Camera c{}; c.valid = true; c.transform = rows; c.previous_transform = from; c.flip_x = flip_x; return c;
        };
        const std::array<double, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const std::array<double, 16> to_right{1, 0, 0, .063, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const std::array<double, 16> to_left{1, 0, 0, -.063, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        // Right-eye view to left-eye view after turning: x' = z, z' = -x, 63 mm back left.
        const std::array<double, 16> turned{0, 0, 1, -.063, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 0, 1};
        const std::array<double, 16> unknown{1, 0, 0, 5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        rv::Chain chain; rv::Camera world{};
        expect(chain.advance(step(identity, unknown, false), world) == rv::Chain::kRestarted &&
            near3(world.position, 0, 0, 0), "a chain starts its world at its first camera");
        const auto first_world = chain.world();
        expect(chain.advance(step(to_right, identity, false), world) == 1 && near3(world.position, .063F, 0, 0) &&
            near3(world.right, 1, 0, 0), "the next eye did not land 63 mm to the right");
        expect(chain.advance(step(turned, to_right, false), world) == 1 && near3(world.position, 0, 0, 0) &&
            near3(world.right, 0, 0, -1) && near3(world.forward, 1, 0, 0), "a turn did not rotate the chained camera");
        // REAL VR steps from the first frame again (its transf_2 is the first
        // frame's transf0), not from the turned one just before.
        expect(chain.advance(step(to_left, identity, false), world) == 3 && near3(world.position, -.063F, 0, 0) &&
            near3(world.right, 1, 0, 0), "a step from an earlier frame did not start from that frame");
        // A frame stepping from one never captured: its transf_2 is the step
        // into that frame from the last one held (5 units right here), then
        // its own step. Same world, no restart.
        expect(chain.advance(step(to_right, unknown, true), world) == rv::Chain::kBridged &&
            chain.world() == first_world && near3(world.position, 5, 0, 0) && near3(world.right, -1, 0, 0),
            "a frame after an uncaptured one was not bridged from the last frame, or x was not normalised");
    }
    {
        // The Last of Us Part I F8 (projection x mirrored): right eye's
        // publication 202 against the left eye's 201, 5.7 cm along +right.
        const std::array<float, 3> right_eye{-246.112595F, -18.5818462F, -11.1235952F};
        const std::array<float, 3> left_eye{-246.087296F, -18.5860825F, -11.0724058F};
        const std::array<float, 3> right_axis{-0.442002326F, 0.0729873255F, -0.894039571F};
        expect(xrfg::luke_measured_right_axis(1, right_eye, left_eye, right_axis, .057F) == 1.F &&
            xrfg::luke_measured_right_axis(0, left_eye, right_eye, right_axis, .057F) == 1.F,
            "TLOU1 camera_right points to the right eye");
        expect(xrfg::luke_measured_right_axis(0, right_eye, left_eye, right_axis, .057F) == -1.F,
            "a mirrored camera_right is measured as such");
        expect(xrfg::luke_measured_right_axis(1, right_eye, left_eye, right_axis, .2F) == 0 &&
            xrfg::luke_measured_right_axis(1, right_eye, right_eye, right_axis, .057F) == 0 &&
            xrfg::luke_measured_right_axis(2, right_eye, left_eye, right_axis, .057F) == 0,
            "a separation that is not one eye baseline decides nothing");
        xrfg::LukeRightAxisSign sign;
        expect(sign.observe(1) == 0 && sign.observe(0) == 0 && sign.observe(1) == 0 &&
            sign.observe(1) == 1, "direction adopted only after three agreeing frames");
        for (int i = 0; i < 12; ++i) expect(sign.observe(-1) == 1, "swapped-eye frames flipped an adopted direction");
        xrfg::LukeRightAxisSign interrupted;
        expect(interrupted.observe(1) == 0 && interrupted.observe(-1) == 0 && interrupted.observe(1) == 0 &&
            interrupted.observe(1) == 0 && interrupted.observe(1) == 1, "a disagreeing frame did not restart the count");
    }
    for (float units : {1.0F, 100.0F}) {
        for (float world_scale : {0.5F, 1.0F, 2.0F}) {
            const float baseline = 0.064F * units / world_scale;
            const float velocity = 0.002F * units;
            xrfg::LukeUnitCamera a{{-baseline/2,0,0},{1,0,0},8,true};
            xrfg::LukeUnitCamera b{{baseline/2+8*velocity,0,0},{1,0,0},8,true};
            xrfg::LukeUnitCamera c{{-baseline/2+20*velocity,0,0},{1,0,0},12,true};
            const float measured = xrfg::luke_camera_separation(a,b,c);
            expect(near(measured, baseline), "unequal frame intervals cancel linear head translation in both unit systems");
            xrfg::LukeCameraUnitCalibration calibration;
            for (unsigned i=1;i<=8;++i)
                expect(calibration.observe(1,i,measured)==0, "no admission before independent calibration samples");
            expect(near(calibration.observe(1,9,measured),baseline), "consistent independent observations calibrate camera units");
            expect(near(calibration.observe(1,9,measured),baseline) && calibration.samples()==9,
                "duplicate resolver calls do not add calibration confidence");
            expect(calibration.observe(2,10,measured)==0 && calibration.samples()==1,
                "new NGX epoch requires fresh calibration");
            for(unsigned i=11;i<=18;++i) calibration.observe(2,i,measured);
            expect(near(calibration.observe(2,19,measured*2),baseline),
                "head acceleration does not rescale an established engine unit");
            expect(near(calibration.observe(2,20,0),baseline),
                "frame gap cannot invalidate established unit calibration");
            expect(calibration.observe(3,21,measured*2)==0 && calibration.samples()==1,
                "explicit new epoch permits a fresh unit conversion");
            float physical_baseline{};
            expect(xrfg::luke_game_eye_baseline(.064F, world_scale, units, physical_baseline) &&
                near(physical_baseline, baseline), "WorldScale applied exactly once to stable engine units");
            c.valid=false;
            expect(xrfg::luke_camera_separation(a,b,c)==0,"invalid camera metadata cannot calibrate");
        }
    }
    xrfg::LukeCameraUnitCalibration unstable_units;
    for(unsigned i=1;i<=12;++i)
        expect(unstable_units.observe(1,i,i%2 ? 6.0F : 7.0F)==0,
            "inconsistent acceleration/camera separation is not silently accepted");
    xrfg::LukeSceneCameraUnitCalibration scene_units;
    for(unsigned i=1;i<=8;++i)expect(scene_units.observe(1,i,.11857F)==0,"GTA startup requires a complete native calibration window");
    expect(near(scene_units.observe(1,9,.11857F),.11857F),"GTA scripted-rig calibration established");
    for(unsigned i=10;i<=17;++i)expect(near(scene_units.observe(1,i,.06195F),.11857F),"one or mixed-window rig sample cannot rescale AFW");
    expect(near(scene_units.observe(1,18,.06195F),.06195F),"consistent gameplay rig replaces stale scripted separation");
    expect(near(scene_units.observe(2,19,0),.06195F),"GTA temporal reset preserves proven unit conversion while cameras re-prime");
    expect(near(scene_units.observe(2,19,.2F),.06195F),"duplicate scene publication adds no calibration confidence");
    expect(scene_units.observe(2,18,.2F)==0,"out-of-order scene camera rejected");
    for(unsigned i=20;i<40;++i)expect(near(scene_units.observe(2,i,i%2?.04F:.09F),.06195F),"unstable post-cut cameras cannot poison recovered units");
    constexpr float physical_ipd = 0.06412F;
    const std::array<float, 3> xr_left{-physical_ipd * 0.5F, 0.0F, 0.0F};
    const std::array<float, 3> xr_right{physical_ipd * 0.5F, 0.0F, 0.0F};
    float measured_ipd{};
    expect(xrfg::luke_xr_eye_baseline(xr_left, xr_right, measured_ipd) &&
        near(measured_ipd, physical_ipd, 1.0e-6F),
        "same-call XR eye positions measure physical IPD");
    for (const float angle : {0.0F, 35.0F, 90.0F, 175.0F, 300.0F}) {
        const float cosine = std::cos(degrees(angle));
        const float sine = std::sin(degrees(angle));
        const std::array<float, 3> center{1.25F + angle * 0.001F,
            1.7F, -2.1F - angle * 0.002F};
        const auto moved_eye = [&](float local_x) {
            return std::array<float, 3>{center[0] + local_x * cosine,
                center[1], center[2] - local_x * sine};
        };
        expect(xrfg::luke_xr_eye_baseline(
            moved_eye(xr_left[0]), moved_eye(xr_right[0]), measured_ipd) &&
            near(measured_ipd, physical_ipd, 1.0e-5F),
            "same-call physical IPD is invariant under common headset translation and yaw");
    }
    auto invalid_eye = xr_left;
    invalid_eye[1] = std::numeric_limits<float>::quiet_NaN();
    expect(!xrfg::luke_xr_eye_baseline(invalid_eye, xr_right, measured_ipd),
        "nonfinite XR eye position rejects physical IPD");
    invalid_eye = xr_right;
    invalid_eye[2] = std::numeric_limits<float>::infinity();
    expect(!xrfg::luke_xr_eye_baseline(xr_left, invalid_eye, measured_ipd),
        "infinite XR eye position rejects physical IPD");
    expect(!xrfg::luke_xr_eye_baseline(xr_left, xr_left, measured_ipd),
        "coincident XR eye positions reject physical IPD");
    expect(!xrfg::luke_xr_eye_baseline({0.0F, 0.0F, 0.0F},
        {0.005F, 0.0F, 0.0F}, measured_ipd) &&
        !xrfg::luke_xr_eye_baseline({0.0F, 0.0F, 0.0F},
        {0.25F, 0.0F, 0.0F}, measured_ipd),
        "unreasonable XR eye separation rejects physical IPD");
    auto current_left = xr_left;
    auto current_right = xr_right;
    current_left[0] += 0.008F;
    current_right[0] += 0.008F;
    float shortened_cross_time{}, lengthened_cross_time{};
    expect(xrfg::luke_xr_eye_baseline(current_left, xr_right,
            shortened_cross_time) &&
        xrfg::luke_xr_eye_baseline(xr_left, current_right,
            lengthened_cross_time) &&
        shortened_cross_time < physical_ipd - 0.007F &&
        lengthened_cross_time > physical_ipd + 0.007F &&
        xrfg::luke_xr_eye_baseline(current_left, current_right, measured_ipd) &&
        near(measured_ipd, physical_ipd, 1.0e-6F),
        "retained cross-time AER peer invents alternating IPD while a same-call pair remains constant");
    const xrfg::LukeCameraBasis camera_basis{{
        {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}};
    xrfg::LukeCameraBasis peer_basis{};
    expect(xrfg::luke_destination_camera_basis(yaw(0.0F), yaw(10.0F),
        camera_basis, peer_basis) &&
        near(peer_basis[0][0], std::cos(degrees(10))) &&
        near(peer_basis[0][2], -std::sin(degrees(10))) &&
        near(peer_basis[2][0], std::sin(degrees(10))),
        "submitted headset yaw rotates only the generated peer camera");
    expect(xrfg::luke_destination_camera_basis(yaw(10.0F), yaw(10.0F),
        camera_basis, peer_basis) && near(peer_basis[0][0], 1.0F) &&
        near(peer_basis[0][2], 0.0F),
        "equal submitted orientations preserve stationary stereo");
    expect(!xrfg::luke_destination_camera_basis({0, 0, 0, 0}, yaw(10.0F),
        camera_basis, peer_basis), "invalid XR orientation rejects camera");
    xrfg::LukeCameraBasis lh_view{}, rh_view{}, lh_peer{}, rh_peer{};
    // Horizon V477 PID34904 publication3862: ingress camera rows, projection
    // W=-1. Golden output follows the exact owner-selected V33018 helper.
    const xrfg::LukeCameraBasis horizon_physical{{
        {0.536053598F, -0.844009817F, 0.0171653107F},
        {0.132586151F, 0.10425552F, 0.985673487F},
        {0.833707511F, 0.526097715F, -0.167790502F}}};
    for (float w : {-1.0F, 1.0F}) {
        xrfg::LukeCameraBasis restored{};
        expect(xrfg::luke_view_camera_basis(horizon_physical, w, restored,
                xrfg::LukeProducerCameraAxes::physical_forward),
            "captured Horizon physical camera is admitted");
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned c = 0; c < 3; ++c)
                expect(restored[row][c] == horizon_physical[row][c] *
                    (row == 2 ? w : 1.0F),
                    "Horizon matches V33018 without changing Right or Up");
    }
    const xrfg::LukeCameraBasis lh_producer_basis{{
        {1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    const xrfg::LukeCameraBasis rh_producer_basis{{
        {1, 0, 0}, {0, 1, 0}, {0, 0, -1}}};
    expect(xrfg::luke_view_camera_basis(lh_producer_basis, 1.0F, lh_view,
            xrfg::LukeProducerCameraAxes::view) &&
        xrfg::luke_view_camera_basis(rh_producer_basis, -1.0F, rh_view,
            xrfg::LukeProducerCameraAxes::view) &&
        near(lh_view[2][2], 1.0F) && near(rh_view[2][2], -1.0F),
        "LH and RH producer view rows are preserved without double reflection");
    expect(xrfg::luke_destination_view_camera_basis(yaw(0), yaw(10), lh_view,
        1.0F, lh_peer) && xrfg::luke_destination_view_camera_basis(yaw(0), yaw(10),
        rh_view, -1.0F, rh_peer), "both view conventions accept headset yaw");
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 3; ++column)
            expect(near(lh_peer[row][column], rh_peer[row][column] * (row == 2 ? -1.0F : 1.0F)),
                "LH and RH camera yaw describe the same physical rotation");
    // Equivalent producer view-axis reflections must transport the same
    // physical headset pitch/yaw/roll, not reverse pitch on a down-Y camera.
    const xrfg::Quaternion reflected_source_head{.10F,.08F,-.04F,.99F};
    const xrfg::Quaternion reflected_destination_head{-.05F,.12F,.03F,.98F};
    for (float sign_x : {-1.F,1.F}) for (float sign_y : {-1.F,1.F}) {
        auto reflected_view=lh_view;
        for (auto& v:reflected_view[0]) v*=sign_x;
        for (auto& v:reflected_view[1]) v*=sign_y;
        xrfg::LukeCameraBasis physical_peer{},reflected_peer{};
        expect(xrfg::luke_destination_view_camera_basis(reflected_source_head,
            reflected_destination_head,lh_view,1.F,physical_peer) &&
            xrfg::luke_destination_view_camera_basis(reflected_source_head,
            reflected_destination_head,reflected_view,1.F,reflected_peer,sign_x,sign_y),
            "signed producer view supports full headset rotation");
        for (unsigned row=0;row<3;++row) for(unsigned col=0;col<3;++col)
            expect(near(reflected_peer[row][col],physical_peer[row][col]*
                (row==0?sign_x:row==1?sign_y:1.F)),
                "reflected view axes preserve physical pitch yaw and roll");
    }
    for (float sign : {-1.0F, 1.0F}) {
        xrfg::LukeCameraBasis view{};
        expect(xrfg::luke_view_camera_basis(sign > 0 ? lh_producer_basis :
            rh_producer_basis, sign, view, xrfg::LukeProducerCameraAxes::view),
            "producer view convention is computable");
        // A point five metres ahead and a peer shifted right must remain
        // in front and move left in the destination view for either matrix.
        const float view_z = 5.0F * view[2][2];
        const float w = sign * view_z;
        expect(w > 0 && near((-0.06F * ghost_projection[0]) / w,
            -0.06F * ghost_projection[0] / 5.0F),
            "stereo disparity and positive clip W agree across handedness");
    }
    expect(!xrfg::luke_view_camera_basis(lh_producer_basis, 0, lh_view,
            xrfg::LukeProducerCameraAxes::view) &&
        !xrfg::luke_destination_view_camera_basis(yaw(0), yaw(10), rh_view, 0, rh_peer),
        "degenerate perspective does not select a guessed camera convention");
    // PID23856 publications1708 ->1707. The TSV camera_forward column is
    // the CONSUMED V78 row (written after camera_data), not original Fwd.
    // Undo V78's known m[11]=-1 multiplication to recover producer input.
    // Compare projection of actual world points with independent declared
    // Streamline clipToPrevClip, not with the old helper's assumptions.
    const xrfg::LukeCameraBasis captured_current{{
        {0.939639568F, 0.320974499F, -0.118545517F},
        {0.0473205224F, 0.221225843F, 0.974073887F},
        {0.338878185F, -0.920887947F, 0.192683861F}}};
    const xrfg::LukeCameraBasis captured_previous{{
        {0.947595358F, 0.28590554F, -0.142551824F},
        {0.0787854791F, 0.223294601F, 0.971561909F},
        {0.309605986F, -0.931878567F, 0.189067721F}}};
    const std::array<double, 3> captured_current_position{-277.36972, -115.582558, 7.01657772};
    const std::array<double, 3> captured_previous_position{-277.419067, -115.597961, 7.02474356};
    const std::array<double, 16> captured_projection{
        .854080677,0,0,0, 0,.854080677,0,0,
        0,0,.0000073242727,-1, 0,0,.060000442,0};
    const std::array<double, 16> captured_declared_history{
        .99906534,.0305275153,-.000000262451408,.0358331017,
        -.030765608,.999499798,-.0000000629363655,.00859284867,
        .744880795,-.00862929877,.99999994,.0103368396,
        -.025928691,-.00706644962,.00000000362706487,.999504805};
    const auto project_fixture = [](const std::array<double, 4>& point,
        const std::array<double, 16>& matrix) {
        std::array<double, 4> result{};
        for (unsigned c = 0; c < 4; ++c)
            for (unsigned r = 0; r < 4; ++r) result[c] += point[r] * matrix[r * 4 + c];
        return result;
    };
    xrfg::LukeCameraBasis captured_view{}, captured_prior_view{};
    expect(xrfg::luke_view_camera_basis(captured_current, -1, captured_view,
            xrfg::LukeProducerCameraAxes::view) &&
        xrfg::luke_view_camera_basis(captured_previous, -1, captured_prior_view,
            xrfg::LukeProducerCameraAxes::view),
        "actual captured RH producer rows are admitted");
    for (const std::array<double, 4> point : {
            std::array<double, 4>{.4,.2,-5,1}, {-1,.5,-2,1}, {4,-3,-30,1}}) {
        std::array<double, 3> world = captured_current_position;
        for (unsigned c = 0; c < 3; ++c)
            for (unsigned r = 0; r < 3; ++r) world[c] += point[r] * captured_view[r][c];
        std::array<double, 4> previous_point{0,0,0,1};
        for (unsigned r = 0; r < 3; ++r)
            for (unsigned c = 0; c < 3; ++c)
                previous_point[r] += (world[c] - captured_previous_position[c]) *
                    captured_prior_view[r][c];
        const auto predicted = project_fixture(previous_point, captured_projection);
        const auto declared = project_fixture(project_fixture(point, captured_projection),
            captured_declared_history);
        for (unsigned c = 0; c < 3; ++c)
            expect(std::abs(predicted[c] / predicted[3] - declared[c] / declared[3]) < 5.0e-5,
                "captured moving camera agrees with declared SL history across scene depths");
    }
    auto invalid_basis = captured_current;
    invalid_basis[2][0] = std::numeric_limits<float>::quiet_NaN();
    expect(!xrfg::luke_view_camera_basis(invalid_basis, -1, captured_view,
            xrfg::LukeProducerCameraAxes::view),
        "nonfinite producer basis is rejected rather than guessed");
    expect(!xrfg::luke_view_camera_basis(captured_current, -1, captured_view,
            static_cast<xrfg::LukeProducerCameraAxes>(99)),
        "unknown producer axis contract is rejected instead of guessed");
    {
        // DOOM V30089 PID8952, immutable real colours6162 ->6161. The supplied
        // rows are right/down/forward, whereas the image projection consumes
        // right/up/back. Both changed rows are required; an Fwd-only reflection
        // was rejected in V30088. This is not a declared-SL-history fixture.
        const xrfg::LukeCameraBasis doom_current{{
            {0.833019257F, -0.549696326F, -0.0625534728F},
            {-0.05711063F, 0.02702263F, -0.998002052F},
            {0.550288439F, 0.83492744F, -0.00888313074F}}};
        const xrfg::LukeCameraBasis doom_previous{{
            {0.830590904F, -0.553524435F, -0.0610701889F},
            {-0.0562100895F, 0.0257725492F, -0.998086333F},
            {0.554039061F, 0.832434118F, -0.00970721152F}}};
        const std::array<double, 3> doom_current_position{
            -0.00482177734, 0.0168457031, 0.00244140625};
        const std::array<double, 3> doom_previous_position{
            0.048034668, -0.0185546875, -0.00146484375};
        xrfg::LukeCameraBasis current_view{}, previous_view{}, unchanged_view{};
        expect(xrfg::luke_view_camera_basis(doom_current, -1, current_view,
                xrfg::LukeProducerCameraAxes::right_down_forward) &&
            xrfg::luke_view_camera_basis(doom_previous, -1, previous_view,
                xrfg::LukeProducerCameraAxes::right_down_forward),
            "DOOM complete producer axis contract is decoded for current and history");
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned coordinate = 0; coordinate < 3; ++coordinate)
                expect(current_view[row][coordinate] == doom_current[row][coordinate] *
                    (row == 0 ? 1.0F : -1.0F),
                    "right/down/forward rotates Up and Fwd together while preserving Right");
        expect(xrfg::luke_view_camera_basis(doom_current, -1, unchanged_view,
                xrfg::LukeProducerCameraAxes::view) && unchanged_view == doom_current,
            "accepted ordinary producer view contract remains byte-identical");
        invalid_basis = doom_current;
        invalid_basis[1][1] = std::numeric_limits<float>::infinity();
        expect(!xrfg::luke_view_camera_basis(invalid_basis, -1, unchanged_view,
                xrfg::LukeProducerCameraAxes::right_down_forward),
            "nonfinite right/down/forward producer rows are rejected");

        const xrfg::Quaternion current_head{0.00602520537F, -0.0506943725F,
            0.031026708F, -0.998214066F};
        const xrfg::Quaternion previous_head{0.00633070152F, -0.0484380238F,
            0.0302784983F, -0.998347223F};
        xrfg::LukeCameraBasis destination_view{};
        expect(xrfg::luke_destination_view_camera_basis(current_head, previous_head,
                current_view, -1, destination_view),
            "decoded DOOM view uses the same accepted XR destination rotation");
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned coordinate = 0; coordinate < 3; ++coordinate)
                expect(near(destination_view[row][coordinate],
                    previous_view[row][coordinate], 2.0e-6F),
                    "DOOM captured headset delta reaches the actual opposite rendered camera");

        // x/y are current guide-pixel centres, z is bilinearly sampled native
        // depth, dx/dy are bidirectionally checked LK displacements measured
        // from the two independently captured real colour images (not AFW).
        const std::array<std::array<double, 5>, 8> real_colour_tracks{{
            {642,689,.0013668662,-3.685669,-.38861084},
            {589,850,.00499421,-6.449707,-.4295044},
            {611,776,.0028919429,-4.7799683,-.3961792},
            {370,447,.014618282,-12.943787,-1.4966125},
            {817,753,.001979855,-4.376953,-.2220459},
            {644,654,.0012784565,-3.6744385,-.4039917},
            {658,844,.0044243755,-5.954529,-.32696533},
            {1015,700,.0023304194,-5.1900024,.258667}}};
        constexpr double focal_x = .932515085;
        constexpr double focal_y = .854080677;
        constexpr double depth_a = .00000600003568;
        constexpr double depth_b = .0600003563;
        for (const auto& track : real_colour_tracks) {
            const double u = (track[0] + .5) / 1341;
            const double v = (track[1] + .5) / 1464;
            const double view_z = -depth_b / (track[2] + depth_a);
            const std::array<double, 3> point{
                (2*u-1)*(-view_z)/focal_x,
                (1-2*v)*(-view_z)/focal_y, view_z};
            auto world = doom_current_position;
            for (unsigned coordinate = 0; coordinate < 3; ++coordinate)
                for (unsigned row = 0; row < 3; ++row)
                    world[coordinate] += point[row] * current_view[row][coordinate];
            std::array<double, 3> previous_point{};
            for (unsigned row = 0; row < 3; ++row)
                for (unsigned coordinate = 0; coordinate < 3; ++coordinate)
                    previous_point[row] += (world[coordinate] -
                        doom_previous_position[coordinate]) * previous_view[row][coordinate];
            const double previous_u = focal_x*previous_point[0] /
                (-previous_point[2])*.5+.5;
            const double previous_v = -focal_y*previous_point[1] /
                (-previous_point[2])*.5+.5;
            const double error_x = (previous_u-u)*1341 - track[3];
            const double error_y = (previous_v-v)*1464 - track[4];
            expect(std::hypot(error_x,error_y) < .12,
                "decoded DOOM world motion matches immutable adjacent real colours at varied depths");
        }
    }
    const std::array<float, 3> camera_right{0.27F, 0.96F, 0.01F};
    const std::array<float, 3> camera_left_position{0.0F, 0.0F, 0.0F};
    const std::array<float, 3> camera_right_position{0.0162F, 0.0576F, 0.0006F};
    expect(xrfg::luke_guide_index_for_eye(camera_left_position,
        camera_right, camera_right_position, camera_right, 0) == 0,
        "left-first Luke publication selects older left guide");
    expect(xrfg::luke_guide_index_for_eye(camera_left_position,
        camera_right, camera_right_position, camera_right, 1) == 1,
        "left-first Luke publication selects newer right guide");
    expect(xrfg::luke_guide_index_for_eye(camera_right_position,
        camera_right, camera_left_position, camera_right, 0) == 1,
        "right-first Luke publication selects newer left guide");
    expect(xrfg::luke_guide_index_for_eye(camera_right_position,
        camera_right, camera_left_position, camera_right, 1) == 0,
        "right-first Luke publication selects older right guide");
    expect(!xrfg::luke_guide_index_for_eye(camera_left_position,
        camera_right, camera_left_position, camera_right, 0),
        "same-camera publications are not a stereo guide pair");
    expect(xrfg::luke_guide_index_for_serial_eye(
        3969, 3970, 3968, 1, 0) == 0,
        "camera movement cannot skip the expected opposite-eye guide");
    expect(xrfg::luke_guide_index_for_serial_eye(
        3972, 3973, 3971, 1, 0) == 0,
        "the next physical eye selects 3972 before 3973");
    expect(xrfg::luke_guide_index_for_serial_eye(
        3966, 3967, 3966, 0, 1) == 1,
        "one NGX publication cannot be consumed by both eyes");
    expect(!xrfg::luke_guide_index_for_serial_eye(
        3972, 3973, 3973, 1, 0),
        "stale guide pair holds the previous stereo pair");
    const xrfg::Pose start{yaw(0.0F), {0.0F, 1.0F, 2.0F}};
    const xrfg::Pose end{yaw(90.0F), {2.0F, 3.0F, 6.0F}};
    const xrfg::Pose middle = xrfg::interpolate_pose(start, end, 0.5F);

    expect(near(middle.position.x, 1.0F), "position x midpoint");
    expect(near(middle.position.y, 2.0F), "position y midpoint");
    expect(near(middle.position.z, 4.0F), "position z midpoint");
    expect(near(middle.orientation.y, std::sin(std::numbers::pi_v<float> / 8.0F)), "45-degree yaw sine");
    expect(near(middle.orientation.w, std::cos(std::numbers::pi_v<float> / 8.0F)), "45-degree yaw cosine");

    const auto same_rotation_other_sign = xrfg::slerp_shortest(yaw(30.0F), {
        -yaw(30.0F).x,
        -yaw(30.0F).y,
        -yaw(30.0F).z,
        -yaw(30.0F).w,
    }, 0.5F);
    expect(near(std::abs(same_rotation_other_sign.w), std::cos(std::numbers::pi_v<float> / 12.0F)),
           "quaternion sign uses shortest path");

    // A synthetic's own pose: inside the span it is the interpolation, past
    // it the same turn carries on - 1.5 of a 90-degree yaw is 135 degrees.
    const xrfg::Pose third = xrfg::pose_at_fraction(start, end, 1.0F / 3.0F);
    const xrfg::Pose third_slerp = xrfg::interpolate_pose(start, end, 1.0F / 3.0F);
    expect(near(third.orientation.y, third_slerp.orientation.y) &&
               near(third.orientation.w, third_slerp.orientation.w) &&
               near(third.position.z, third_slerp.position.z),
           "pose_at_fraction matches the interpolation inside the span");
    const xrfg::Pose beyond = xrfg::pose_at_fraction(start, end, 1.5F);
    expect(near(beyond.orientation.y, yaw(135.0F).y) && near(beyond.orientation.w, yaw(135.0F).w),
           "pose_at_fraction carries the turn on past the second pose");
    expect(near(beyond.position.x, 3.0F) && near(beyond.position.y, 4.0F) &&
               near(beyond.position.z, 8.0F),
           "pose_at_fraction carries the translation on past the second pose");
    const xrfg::Pose tilted_start{{0.0F, 0.0F, std::sin(0.2F), std::cos(0.2F)}, {}};
    const xrfg::Pose tilted_end{{std::sin(0.05F) * 0.6F, std::sin(0.05F) * 0.8F, 0.0F,
                                 std::cos(0.05F)}, {}};
    const xrfg::Pose tilted_half = xrfg::pose_at_fraction(tilted_start, tilted_end, 0.5F);
    const xrfg::Pose tilted_slerp = xrfg::interpolate_pose(tilted_start, tilted_end, 0.5F);
    expect(xrfg::rotation_angle(tilted_half.orientation, tilted_slerp.orientation) < 1.0e-4F,
           "pose_at_fraction matches slerp about an arbitrary axis");
    expect(near(xrfg::rotation_angle(yaw(10.0F), yaw(40.0F)),
                30.0F * std::numbers::pi_v<float> / 180.0F),
           "rotation_angle measures a yaw");
    expect(near(xrfg::rotation_angle(yaw(10.0F), {-yaw(40.0F).x, -yaw(40.0F).y,
                                                   -yaw(40.0F).z, -yaw(40.0F).w}),
                30.0F * std::numbers::pi_v<float> / 180.0F),
           "rotation_angle ignores the quaternion's sign");
    expect(near(xrfg::rotation_angle(yaw(10.0F), yaw(10.002F)),
                0.002F * std::numbers::pi_v<float> / 180.0F, 2.0e-6F),
           "rotation_angle resolves thousandths of a degree");

    const xrfg::TimedPose timed_start{100, start};
    const xrfg::TimedPose timed_end{200, end};
    const auto timed_middle = xrfg::midpoint(timed_start, timed_end);
    expect(timed_middle.has_value() && timed_middle->time_ns == 150, "timestamp midpoint");
    expect(!xrfg::midpoint(timed_end, timed_start).has_value(), "reject non-monotonic timestamps");

    const xrfg::Rgba8 black{0, 0, 0, 0};
    const xrfg::Rgba8 white{255, 255, 255, 255};
    const auto gray = xrfg::interpolate_pixel(black, white, 0.5F);
    expect(gray.r == 128 && gray.g == 128 && gray.b == 128 && gray.a == 128, "pixel midpoint rounds correctly");

    using namespace std::chrono_literals;
    expect(!xrfg::should_enter_generation_cooldown(false, 200ms, 8'333'333),
            "FidelityFX ignores compositor end spikes");
    expect(!xrfg::should_enter_generation_cooldown(true, 50ms, 8'333'333),
           "NVIDIA tolerates low-frame-rate compositor pacing");
    expect(!xrfg::should_enter_generation_cooldown(
               true, 75ms, 8'333'333),
           "NVIDIA cooldown has a strict 75 ms minimum threshold");
    expect(xrfg::should_enter_generation_cooldown(true, 75'000'001ns, 8'333'333),
           "NVIDIA retains recovery above the 75 ms minimum threshold");
    expect(!xrfg::should_enter_generation_cooldown(true, 83ms, 13'888'889),
           "NVIDIA also requires more than six display periods");
    expect(xrfg::should_enter_generation_cooldown(true, 84ms, 13'888'889),
           "NVIDIA retains recovery beyond six display periods");
    expect(!xrfg::should_enter_generation_cooldown(true, 68ms, 0),
           "invalid display period cannot start cooldown");

    constexpr std::array uevr_slow_ends{
        68'522us, 18'226us, 22'262us, 19'471us, 19'330us,
        19'863us, 19'893us, 19'440us, 22'551us};
    std::size_t uevr_nvidia_cooldowns = 0;
    for (const auto elapsed : uevr_slow_ends) {
        uevr_nvidia_cooldowns += xrfg::should_enter_generation_cooldown(
            true, elapsed, 8'333'333);
    }
    expect(uevr_nvidia_cooldowns == 0,
           "retained low-rate UEVR timing replay causes no cooldowns");

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All XR Frame Bridge core tests passed\n";
    return EXIT_SUCCESS;
}
