#include "xrfg/vulkan_native_ingress.hpp"
#include "xrfg/luke_vulkan_scope.hpp"
#include "../src/vulkan_layer/vulkan_queue_layer.cpp"
#include <windows.h>
#include <iostream>
#include <stdexcept>

int main(int argc,char** argv) {
    using xrfg::native_vulkan::luke_vulkan_process_name;
    if(!luke_vulkan_process_name(L"DOOMTheDarkAges.exe")||!luke_vulkan_process_name(L"TheGreatCircle.exe")||
        !luke_vulkan_process_name(L"RDR2.exe")||!luke_vulkan_process_name(L"rdr2.EXE")||
        luke_vulkan_process_name(L"NMS.exe")||luke_vulkan_process_name(L"DOOMEternalx64vk.exe")||
        luke_vulkan_process_name(L"TheGreatCircle.exe.other")||luke_vulkan_process_name(L"RDR2.exe.other")||
        luke_vulkan_process_name(nullptr))return 4;
    HMODULE loader=LoadLibraryW(L"vulkan-1.dll");
    if(!loader){std::cerr<<"Vulkan loader unavailable\n";return 1;}
    // Copy actual loader entry bytes before polling repeatedly with valid API
    // addresses available. The shipping stubs must neither patch nor call them.
    const char* names[]{"vkGetDeviceProcAddr","vkGetInstanceProcAddr","vkCreateImage",
        "vkDestroyImage","vkCreateDevice","vkDestroyDevice","vkCmdDraw","vkQueueSubmit"};
    unsigned char before[8][16]{};
    FARPROC addresses[8]{};
    for(unsigned i=0;i<8;++i){addresses[i]=GetProcAddress(loader,names[i]);if(!addresses[i])return 2;memcpy(before[i],addresses[i],16);}
    for(unsigned i=0;i<100;++i)xrfg::native_vulkan::poll_capture();
    HMODULE shipping{};
    const bool selected=argc>2 && !strcmp(argv[2],"selected");
    HMODULE marker=selected?LoadLibraryW(L"luke-runtime-fixture.dll"):nullptr;
    if(selected&&!marker)return 8;
    if(argc>1)start_luke_vulkan_ingress();
    if(argc>1){shipping=LoadLibraryA(argv[1]);if(!shipping)return 5;
        using Start=void(*)() noexcept;
        const auto start=reinterpret_cast<Start>(GetProcAddress(shipping,"OFXR_StartLukeVulkanIngressV1"));
        if(!start)return 6;
        for(unsigned i=0;i<100;++i)start();}
    // Invalid opaque owner pointers deliberately detect any attempted transport
    // setup. A suspended ingress has no ownership and never dereferences them.
    xrfg::VulkanSessionBinding binding{};binding.device=reinterpret_cast<VkDevice>(1);
    xrfg::native_vulkan::configure_owner(binding,reinterpret_cast<ID3D12Device*>(2),reinterpret_cast<ID3D12CommandQueue*>(3));
    xrfg::native_vulkan::stop_owner(binding.device);
    unsigned changed{};
    for(unsigned i=0;i<8;++i)if(memcmp(before[i],addresses[i],16)){
        ++changed;if(!selected){std::cerr<<"Loader hook installed: "<<names[i]<<'\n';return 3;}}
    if(selected&&changed!=8){std::cerr<<"Selected early Vulkan hooks: "<<changed<<" expected 8\n";return 7;}
    FreeLibrary(loader);
    if(shipping)FreeLibrary(shipping);
    std::cout<<(selected?"Selected shipping runtime early-load PASS: native loader and command export hooks\n":
        "Shipping Vulkan capture stays suspended: loader entries unchanged, no owner dereference or private transport\n");
    return 0;
}
