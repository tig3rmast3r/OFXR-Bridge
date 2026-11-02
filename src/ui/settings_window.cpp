#include "xrfg/autonomous_ui.hpp"
#include "xrfg/settings_widgets.hpp"
#include "xrfg/benchmark_suite.hpp"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <memory>
#include <fstream>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace xrfg::autonomous {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t kClassName[] = L"OFXRBridgeSettingsV449";
constexpr UINT_PTR kRenderTimer = 1;

struct Window {
    HWND handle{};
    ImGuiContext* imgui{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapchain;
    ComPtr<ID3D11RenderTargetView> target;
    bool win32_ready{};
    bool dx11_ready{};
    SettingsWidgetState widgets{};

    ~Window() {
        set_interface_visible(InterfaceSurface::desktop, false);
        if (imgui) {
            ImGui::SetCurrentContext(imgui);
            if (dx11_ready) ImGui_ImplDX11_Shutdown();
            if (win32_ready) ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(imgui);
        }
    }
    bool render_target() {
        ComPtr<ID3D11Texture2D> buffer;
        return SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer))) &&
            SUCCEEDED(device->CreateRenderTargetView(buffer.Get(), nullptr, &target));
    }
    bool initialize() {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 2;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = handle;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL actual{};
        auto result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
            nullptr, 0, levels, 1, D3D11_SDK_VERSION, &desc, &swapchain, &device, &actual, &context);
        if (FAILED(result)) result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP,
            nullptr, 0, levels, 1, D3D11_SDK_VERSION, &desc, &swapchain, &device, &actual, &context);
        if (FAILED(result) || !render_target()) return false;
        IMGUI_CHECKVERSION();
        imgui = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        apply_settings_style();
        auto& style = ImGui::GetStyle();
        style.WindowRounding = 0;
        style.FrameRounding = 4;
        style.ItemSpacing = ImVec2(10, 9);
        style.WindowPadding = ImVec2(18, 16);
        win32_ready = ImGui_ImplWin32_Init(handle);
        dx11_ready = win32_ready && ImGui_ImplDX11_Init(device.Get(), context.Get());
        return dx11_ready;
    }
    void render(bool present = true) {
        if (!dx11_ready || !target || (present && !IsWindowVisible(handle)) || IsIconic(handle)) {
            set_interface_visible(InterfaceSurface::desktop, false); return;
        }
        ImGui::SetCurrentContext(imgui);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        const auto size = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(size);
        ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings);
        render_settings_widgets(widgets, snapshot());
        ImGui::End();
        ImGui::Render();
        const float clear[]{.06F, .07F, .09F, 1};
        context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
        context->ClearRenderTargetView(target.Get(), clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (present) swapchain->Present(0, 0);
    }
    bool preview(const std::filesystem::path& path) {
        // Tab bars measure newly submitted labels on their first frame. Draw a
        // second frame before capture so a cold preview includes the tab strip.
        render(false);
        render(false);
        ComPtr<ID3D11Texture2D> source;
        if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&source)))) return false;
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;
        context->CopyResource(staging.Get(), source.Get());
        D3D11_MAPPED_SUBRESOURCE mapping{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping))) return false;
        const std::size_t bytes = static_cast<std::size_t>(desc.Width) * desc.Height * 4;
        std::vector<unsigned char> pixels(bytes);
        for (UINT y = 0; y < desc.Height; ++y) {
            const auto* row = static_cast<const unsigned char*>(mapping.pData) + y * mapping.RowPitch;
            auto* destination = pixels.data() + static_cast<std::size_t>(y) * desc.Width * 4;
            for (UINT x = 0; x < desc.Width; ++x) {
                destination[x * 4] = row[x * 4 + 2];
                destination[x * 4 + 1] = row[x * 4 + 1];
                destination[x * 4 + 2] = row[x * 4];
                destination[x * 4 + 3] = 255;
            }
        }
        context->Unmap(staging.Get(), 0);
        BITMAPFILEHEADER file{};
        file.bfType = 0x4D42;
        file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + static_cast<DWORD>(bytes);
        BITMAPINFOHEADER info{sizeof(info)};
        info.biWidth = static_cast<LONG>(desc.Width);
        info.biHeight = -static_cast<LONG>(desc.Height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        info.biSizeImage = static_cast<DWORD>(bytes);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(&file), sizeof(file));
        output.write(reinterpret_cast<const char*>(&info), sizeof(info));
        output.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
        swapchain->Present(0, 0);
        return output.good();
    }
};
std::unique_ptr<Window> window;

LRESULT CALLBACK procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<Window*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->handle = handle;
    }
    if (state && state->imgui) {
        ImGui::SetCurrentContext(state->imgui);
        if (ImGui_ImplWin32_WndProcHandler(handle, message, wparam, lparam)) return TRUE;
    }
    switch (message) {
    case WM_TIMER:
        if (state && wparam == kRenderTimer) { tick_benchmark();state->render(); return 0; }
        break;
    case WM_SIZE:
        if (state && state->swapchain && wparam != SIZE_MINIMIZED) {
            state->context->OMSetRenderTargets(0, nullptr, nullptr);
            state->target.Reset();
            state->swapchain->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
            state->render_target();
        }
        return 0;
    case WM_CLOSE:
        set_interface_visible(InterfaceSurface::desktop, false);
        if (state) finish_settings_widget_edits(state->widgets);
        ShowWindow(handle, SW_HIDE);
        if (!GetWindow(handle, GW_OWNER)) PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND: return TRUE;
    }
    return DefWindowProcW(handle, message, wparam, lparam);
}
} // namespace

bool show_settings_window(HWND owner) noexcept {
    try {
        if (!window) {
            WNDCLASSEXW cls{sizeof(cls)};
            cls.lpfnWndProc = procedure;
            cls.hInstance = GetModuleHandleW(nullptr);
            cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            cls.lpszClassName = kClassName;
            if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
            auto candidate = std::make_unique<Window>();
            const auto title=L"OFXR-BRIDGE 0.3.2 (v"+std::to_wstring(XRFG_IMPLEMENTATION_VERSION)+L") - Settings";
            const HWND handle = CreateWindowExW(WS_EX_APPWINDOW, kClassName,
                title.c_str(), WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT, CW_USEDEFAULT, 690, 900, owner, nullptr,
                cls.hInstance, candidate.get());
            if (!handle || !candidate->initialize()) {
                if (handle) DestroyWindow(handle);
                return false;
            }
            SetTimer(handle, kRenderTimer, 33, nullptr);
            window = std::move(candidate);
        }
        window->widgets.dlssg_profile_loaded=false; // Reopening after another game restores the new remembered target.
        ShowWindow(window->handle, SW_SHOW);
        SetForegroundWindow(window->handle);
        window->render();
        return true;
    } catch (...) { return false; }
}
void close_settings_window() noexcept {
    if (window) {
        finish_settings_widget_edits(window->widgets);
        KillTimer(window->handle, kRenderTimer);
        DestroyWindow(window->handle);
        window.reset();
    }
}
bool save_settings_preview(const std::filesystem::path& output) noexcept {
    try { return window && window->preview(output); }
    catch (...) { return false; }
}
} // namespace xrfg::autonomous
