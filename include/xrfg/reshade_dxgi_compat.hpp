#pragma once
#include <cstdint>

namespace xrfg {
// Only enabled by the verified stock-OptiScaler SteamVR path. Does not load
// ReShade, change its registration or affect factories requested by the game.
bool ensure_reshade_dxgi_compat() noexcept;
std::uint64_t reshade_dxgi_unwrapped_factories() noexcept;
}
