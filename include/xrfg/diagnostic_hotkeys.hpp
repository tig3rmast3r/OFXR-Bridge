#pragma once
#include <Windows.h>
#include <atomic>
#include "xrfg/autonomous_control.hpp"
namespace xrfg {
inline constexpr int kImageCaptureKey = VK_F8;
inline constexpr int kLoggingKey = VK_F9;
inline bool physical_key_down(unsigned key) noexcept {
    using Read=SHORT(WINAPI*)(int);
    static auto read=[] {const auto module=GetModuleHandleW(L"win32u.dll");return module?reinterpret_cast<Read>(GetProcAddress(module,"NtUserGetAsyncKeyState")):nullptr;}();
    return ((read?read(int(key)):GetAsyncKeyState(int(key)))&0x8000)!=0;
}
struct DiagnosticKeyEdge {
    std::atomic_bool down{};
    std::atomic_uint64_t configured{};
    bool pressed(KeybindChord binding,bool value,bool suspended=false) noexcept {
        constexpr std::uint64_t held=1ULL<<48;
        const auto identity=(1ULL<<32)|(std::uint64_t(binding.modifiers)<<16)|binding.key;
        const auto previous=configured.exchange(identity|(value?held:0),std::memory_order_relaxed);
        return !suspended && value && !(previous&held) && (!previous || (previous&~held)==identity);
    }
    bool pressed(bool value) noexcept {
        const bool previous = down.exchange(value, std::memory_order_relaxed);
        return value && !previous;
    }
    bool poll(int key) noexcept {
        return pressed((GetAsyncKeyState(key) & 0x8000) != 0);
    }
    bool poll(KeybindAction action) noexcept {
        const auto config=autonomous::keybinds();
        const auto binding=config.bindings[action];
        return pressed(binding,keybind_matches(binding,physical_key_down),config.suspended);
    }
};
}
