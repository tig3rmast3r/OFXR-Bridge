#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

namespace xrfg {
enum class KeybindAction : unsigned { open_interface, nr, framegen, logging, dump, count };
struct KeybindChord {
    unsigned modifiers{},key{}; // RegisterHotKey MOD_* flags and Win32 VK code
    bool operator==(const KeybindChord&) const = default;
};
struct Keybinds {
    std::array<KeybindChord,5> keys{{{0,0x79},{0,0x7a},{0,0x76},{0,0x78},{0,0x77}}};
    bool operator==(const Keybinds&) const = default;
    const KeybindChord& operator[](KeybindAction action) const {return keys[static_cast<unsigned>(action)];}
};
struct KeybindSnapshot {
    Keybinds bindings{};
    std::uint64_t revision{},suspended_until_ms{};
    bool suspended{};
};
inline bool keybind_assigned(const Keybinds& bindings,KeybindChord chord) {
    if(!chord.key)return false;
    for(const auto key:bindings.keys)if(key==chord)return true;
    return false;
}
// Exact modifiers prevent a bare F8/F10 binding also firing a debug chord.
template<class Down> bool keybind_matches(KeybindChord chord,Down&& down) {
    if(!chord.key || chord.key>=256 || !down(chord.key))return false;
    const unsigned modifiers=(down(0x12)?1U:0U)|(down(0x11)?2U:0U)|
        (down(0x10)?4U:0U)|((down(0x5b)||down(0x5c))?8U:0U);
    return modifiers==chord.modifiers;
}
}
