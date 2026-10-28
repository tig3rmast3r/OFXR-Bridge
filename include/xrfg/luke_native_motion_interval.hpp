#pragma once
#include <string_view>

namespace xrfg {
// Captured native motion in these titles references the prior same-eye frame.
// This identifies a measured interval; it does not alter the motion samples.
// Miles Morales uses Remastered's profile at the owner's explicit request;
// it has not yet supplied an independent capture or headset validation.
// Cyberpunk 2077 runs the same REAL VR 26.3.0 build as Tsushima with
// [CP2077] FixDLSS=1, whose rewritten field is same-eye N-2; as N-1, AFW
// debug turned most of the moving scene green. With N-2 the owner's headset
// shows green only on independently moving objects. No F8 capture yet.
// Avatar: Frontiers of Pandora sets [AFOP] FixDLSS=1 too: green over the
// moving scene as N-1, AFW correct as N-2 on legacy AER. No F8 capture yet.
[[nodiscard]] constexpr bool luke_same_eye_native_motion_title(std::wstring_view name) noexcept {
    const auto equal = [](std::wstring_view a, std::wstring_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i=0;i<a.size();++i) {
            const auto lower=[](wchar_t c){return c>=L'A' && c<=L'Z' ? c+(L'a'-L'A') : c;};
            if (lower(a[i]) != lower(b[i])) return false;
        }
        return true;
    };
    return equal(name,L"GhostOfTsushima.exe") || equal(name,L"HorizonForbiddenWest.exe") ||
        equal(name,L"Spider-Man.exe") || equal(name,L"MilesMorales.exe") ||
        equal(name,L"GTA5_Enhanced.exe") || equal(name,L"Dead Space.exe") ||
        equal(name,L"Cyberpunk2077.exe") || equal(name,L"afop.exe");
}
} // namespace xrfg
