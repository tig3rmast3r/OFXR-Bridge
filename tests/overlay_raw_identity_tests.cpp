// Exercise the actual input reducer without creating a window or installing
// hooks. A mod can consume WM_INPUT before our WndProc sees its cleanup.
#include "../src/ui/overlay_menu_input.cpp"
#include <iostream>
#include <stdexcept>

int main() {
    try {
        xrfg::State input;
        input.owners.insert(1);
        const auto handle=reinterpret_cast<HRAWINPUT>(0x100);
        RAWINPUT packet{};packet.header.dwType=RIM_TYPEMOUSE;
        packet.data.mouse.usButtonFlags=RI_MOUSE_LEFT_BUTTON_DOWN;
        xrfg::process_raw_locked(input,handle,packet);
        if(!input.mouse.down[0])throw std::runtime_error("first mouse packet missing");
        packet.data.mouse.usButtonFlags=RI_MOUSE_LEFT_BUTTON_UP;
        xrfg::process_raw_locked(input,handle,packet);
        if(input.mouse.down[0])throw std::runtime_error("recycled raw handle suppressed a distinct release");
        if(input.decision_cursor)throw std::runtime_error("out-of-WndProc packets retained a stale cache entry");

        xrfg::current_raw_message=handle;
        packet.data.mouse.usButtonFlags=RI_MOUSE_LEFT_BUTTON_DOWN;
        xrfg::process_raw_locked(input,handle,packet);
        const auto cursor=input.decision_cursor;
        xrfg::process_raw_locked(input,handle,packet);
        if(input.decision_cursor!=cursor)throw std::runtime_error("same dispatched packet processed twice");
        xrfg::current_raw_message=nullptr;
        packet.data.mouse.usButtonFlags=RI_MOUSE_LEFT_BUTTON_UP;
        xrfg::process_raw_locked(input,handle,packet);
        if(input.mouse.down[0])throw std::runtime_error("completed dispatch polluted a later mod read");
        std::cout<<"Raw input recycled handle, mod-only reads and scoped duplicate forwarding PASS\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
