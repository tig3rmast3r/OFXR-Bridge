#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include <windows.h>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
int main() {
    WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"OFXRDpiInputTest";
    RegisterClassW(&cls);
    const auto window=CreateWindowW(cls.lpszClassName,L"",WS_POPUP,0,0,300,200,nullptr,nullptr,cls.hInstance,nullptr);
    auto* context=ImGui::CreateContext();
    try {
        if(!window || !ImGui_ImplWin32_Init(window))throw std::runtime_error("native backend initialization");
        const auto send=[&](int x,int y){ImGui_ImplWin32_WndProcHandler(window,WM_MOUSEMOVE,0,MAKELPARAM(x,y));};
        ImGui_ImplWin32_SetMouseCoordinateScale(1.F/1.5F);
        send(150,90); // 100,60 logical
        send(100,60); // raw x equals the old logical x: must not be deduplicated
        send(100,60); // true duplicate must still be removed
        std::vector<ImVec2> points;
        for(const auto& event:context->InputEventsQueue)
            if(event.Type==ImGuiInputEventType_MousePos)points.emplace_back(event.MousePos.PosX,event.MousePos.PosY);
        if(points.size()!=2 || std::abs(points[0].x-100.F)>.001F || std::abs(points[1].x-66.F)>.001F || std::abs(points[1].y-40.F)>.001F)
            throw std::runtime_error("150% DPI movement must be converted before event deduplication");
        ImGui_ImplWin32_SetMouseCoordinateScale(.5F);send(100,60);
        if(std::abs(context->InputEventsQueue.back().MousePos.PosX-50.F)>.001F)
            throw std::runtime_error("monitor DPI change must use the new coordinate scale");
        ImGui_ImplWin32_SetMouseCoordinateScale(1.F);send(100,60);
        if(context->InputEventsQueue.back().MousePos.PosX!=100.F)
            throw std::runtime_error("default physical coordinates must remain unchanged");
        ImGui_ImplWin32_Shutdown();ImGui::DestroyContext(context);DestroyWindow(window);
        std::cout<<"Native mouse input: 150%, 200%, duplicate filtering and default scale passed\n";return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';return 1;
    }
}

