#pragma once
#include <cstdint>
#include <type_traits>

// OFXR's internal camera receipt. The native game reader carries this through
// the RHI command queue; no addon parameter or external publisher is required.
namespace xrfg::daysgone_camera {
struct NativeCamera {
    float projection[16]{};
    float projection_jitter[2]{};
    float translated_view[16]{};
    float origin[3]{};
};
struct Packet {
    std::uint32_t bytes{sizeof(Packet)},version{1},provider{1},game_build{};
    std::uint64_t sequence{},previous_sequence{};
    std::uint32_t frame{},previous_frame{},thread{},flags{}; // bit 0: exact previous camera
    std::uint64_t context{},feature{},color{},output{},depth{},motion{};
    std::uint32_t render_width{},render_height{},output_width{},output_height{};
    float jitter_x{},jitter_y{};
    NativeCamera camera{},previous_camera{};
};
static_assert(std::is_trivially_copyable_v<Packet>);
static_assert(sizeof(NativeCamera)==148);
static_assert(sizeof(Packet)==416);
}
