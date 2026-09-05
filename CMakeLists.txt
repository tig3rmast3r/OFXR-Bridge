cmake_minimum_required(VERSION 3.24)

project(XRFrameGenerationBridge VERSION 0.1.0 LANGUAGES CXX)

option(XRFG_BUILD_LAYER "Build the Windows OpenXR API layer" ON)
option(XRFG_BUILD_TESTS "Build the core test executable" ON)
option(XRFG_BUILD_STANDALONE "Build the Windows standalone tray launcher" ON)

set(XRFG_OPENXR_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/external/OpenXR-SDK"
    CACHE PATH
    "Path to Khronos OpenXR-SDK containing include/openxr/openxr.h")

set(XRFG_FIDELITYFX_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/external/FidelityFX-SDK-v1.1.4/sdk"
    CACHE PATH
    "Path to the pinned AMD FidelityFX SDK v1.1.4 source tree")

set(XRFG_NVIDIA_OPTICAL_FLOW_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/external/NVIDIA-Optical-Flow-SDK-5.0.7/Optical_Flow_SDK_5.0.7"
    CACHE PATH
    "Path to the NVIDIA Optical Flow SDK 5.0.7 root")

add_library(xrfg_core STATIC
    src/core/image.cpp
    src/core/optical_flow.cpp
    src/core/pose.cpp)
target_include_directories(xrfg_core PUBLIC include)
target_compile_features(xrfg_core PUBLIC cxx_std_20)

add_executable(xrfg_reference src/reference/main.cpp)
target_link_libraries(xrfg_reference PRIVATE xrfg_core)

if(MSVC)
    foreach(target xrfg_core xrfg_reference)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endforeach()
endif()

if(XRFG_BUILD_TESTS)
    enable_testing()
    add_executable(xrfg_core_tests tests/core_tests.cpp)
    target_link_libraries(xrfg_core_tests PRIVATE xrfg_core)
    if(MSVC)
        target_compile_options(xrfg_core_tests PRIVATE /W4 /permissive- /EHsc)
        target_compile_definitions(xrfg_core_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    add_test(NAME xrfg_core_tests COMMAND xrfg_core_tests)

    add_executable(xrfg_optical_flow_tests tests/optical_flow_tests.cpp)
    target_link_libraries(xrfg_optical_flow_tests PRIVATE xrfg_core)
    if(MSVC)
        target_compile_options(xrfg_optical_flow_tests PRIVATE /W4 /permissive- /EHsc)
        target_compile_definitions(xrfg_optical_flow_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    add_test(NAME xrfg_optical_flow_tests COMMAND xrfg_optical_flow_tests)
endif()

if(WIN32 AND XRFG_BUILD_LAYER)
    if(NOT EXISTS "${XRFG_OPENXR_SDK_ROOT}/include/openxr/openxr.h" OR
       NOT EXISTS "${XRFG_OPENXR_SDK_ROOT}/include/openxr/openxr_platform_defines.h" OR
       NOT EXISTS "${XRFG_OPENXR_SDK_ROOT}/include/openxr/openxr_loader_negotiation.h" OR
       NOT EXISTS "${XRFG_OPENXR_SDK_ROOT}/include/openxr/openxr_platform.h")
        message(FATAL_ERROR
            "XRFG_OPENXR_SDK_ROOT must point to the required Khronos OpenXR-SDK generated headers")
    endif()

    find_program(XRFG_FXC_EXECUTABLE fxc.exe
        HINTS
            "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64")
    if(NOT XRFG_FXC_EXECUTABLE)
        message(FATAL_ERROR "fxc.exe from the Windows SDK is required for frame-synthesis shaders")
    endif()

    set(XRFG_FIDELITYFX_INCLUDE_DIR
        "${XRFG_FIDELITYFX_SDK_ROOT}/include")
    set(XRFG_FIDELITYFX_LIBRARY_DIR
        "${XRFG_FIDELITYFX_SDK_ROOT}/bin/ffx_sdk")
    set(XRFG_FIDELITYFX_OPTICALFLOW_LIBRARY
        "${XRFG_FIDELITYFX_LIBRARY_DIR}/ffx_opticalflow_x64.lib")
    set(XRFG_FIDELITYFX_DX12_LIBRARY
        "${XRFG_FIDELITYFX_LIBRARY_DIR}/ffx_backend_dx12_x64.lib")
    if(NOT EXISTS "${XRFG_FIDELITYFX_INCLUDE_DIR}/FidelityFX/host/ffx_opticalflow.h" OR
       NOT EXISTS "${XRFG_FIDELITYFX_INCLUDE_DIR}/FidelityFX/host/backends/dx12/ffx_dx12.h" OR
       NOT EXISTS "${XRFG_FIDELITYFX_OPTICALFLOW_LIBRARY}" OR
       NOT EXISTS "${XRFG_FIDELITYFX_DX12_LIBRARY}")
        message(FATAL_ERROR
            "Pinned FidelityFX SDK v1.1.4 headers and Release DX12/Optical Flow libraries are required")
    endif()

    set(XRFG_NVIDIA_OPTICAL_FLOW_INCLUDE_DIR
        "${XRFG_NVIDIA_OPTICAL_FLOW_SDK_ROOT}/NvOFInterface")
    if(NOT EXISTS "${XRFG_NVIDIA_OPTICAL_FLOW_INCLUDE_DIR}/nvOpticalFlowCommon.h" OR
       NOT EXISTS "${XRFG_NVIDIA_OPTICAL_FLOW_INCLUDE_DIR}/nvOpticalFlowD3D12.h")
        message(FATAL_ERROR
            "NVIDIA Optical Flow SDK 5.0.7 interface headers are required")
    endif()

    set(XRFG_GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
    set(XRFG_PACK_INPUT_HEADER "${XRFG_GENERATED_SHADER_DIR}/pack_flow_input_shader.hpp")
    set(XRFG_FULLSCREEN_VERTEX_HEADER "${XRFG_GENERATED_SHADER_DIR}/fullscreen_vertex_shader.hpp")
    set(XRFG_SYNTHESIZE_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/synthesize_midpoint_pixel_shader.hpp")
    set(XRFG_NVIDIA_PACK_INPUT_HEADER "${XRFG_GENERATED_SHADER_DIR}/nvidia_pack_flow_input_shader.hpp")
    set(XRFG_NVIDIA_SYNTHESIZE_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/nvidia_synthesize_midpoint_pixel_shader.hpp")
    set(XRFG_NVIDIA_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/nvidia_bidirectional_synthesize_midpoint_pixel_shader.hpp")
    add_custom_command(
        OUTPUT
            "${XRFG_PACK_INPUT_HEADER}"
            "${XRFG_FULLSCREEN_VERTEX_HEADER}"
            "${XRFG_SYNTHESIZE_PIXEL_HEADER}"
            "${XRFG_NVIDIA_PACK_INPUT_HEADER}"
            "${XRFG_NVIDIA_SYNTHESIZE_PIXEL_HEADER}"
            "${XRFG_NVIDIA_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${XRFG_GENERATED_SHADER_DIR}"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T cs_5_1 /E PackFlowInput
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_PACK_INPUT_HEADER}" /Vn g_xrfg_pack_flow_input_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T vs_5_1 /E FullscreenTriangleVS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_FULLSCREEN_VERTEX_HEADER}" /Vn g_xrfg_fullscreen_vertex_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeMidpointPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_SYNTHESIZE_PIXEL_HEADER}" /Vn g_xrfg_synthesize_midpoint_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T cs_5_1 /E PackNvidiaFlowInput
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_NVIDIA_PACK_INPUT_HEADER}" /Vn g_xrfg_nvidia_pack_flow_input_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeNvidiaMidpointPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_NVIDIA_SYNTHESIZE_PIXEL_HEADER}" /Vn g_xrfg_nvidia_synthesize_midpoint_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeNvidiaBidirectionalMidpointPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_NVIDIA_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER}" /Vn g_xrfg_nvidia_bidirectional_synthesize_midpoint_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        VERBATIM)

    add_library(xrfg_d3d12_history STATIC
        src/d3d11/d3d11_d3d12_interop.cpp
        src/d3d12/d3d12_history.cpp
        src/d3d12/d3d12_frame_synthesizer.cpp
        "${XRFG_PACK_INPUT_HEADER}"
        "${XRFG_FULLSCREEN_VERTEX_HEADER}"
        "${XRFG_SYNTHESIZE_PIXEL_HEADER}"
        "${XRFG_NVIDIA_PACK_INPUT_HEADER}"
        "${XRFG_NVIDIA_SYNTHESIZE_PIXEL_HEADER}"
        "${XRFG_NVIDIA_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER}")
    target_include_directories(xrfg_d3d12_history
        PUBLIC include
        PRIVATE
            "${XRFG_GENERATED_SHADER_DIR}"
            "${XRFG_FIDELITYFX_INCLUDE_DIR}"
            "${XRFG_NVIDIA_OPTICAL_FLOW_INCLUDE_DIR}")
    target_compile_features(xrfg_d3d12_history PUBLIC cxx_std_20)
    target_link_libraries(xrfg_d3d12_history PUBLIC
        xrfg_core
        d3d11
        d3d12
        dxgi
        "${XRFG_FIDELITYFX_OPTICALFLOW_LIBRARY}"
        "${XRFG_FIDELITYFX_DX12_LIBRARY}")
    target_compile_definitions(xrfg_d3d12_history PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(xrfg_d3d12_history PRIVATE
            /W4 /permissive- /EHsc)
    endif()

    add_library(xrfg_bridge_flight_logger STATIC
        src/layer/bridge_flight_logger.cpp
        include/xrfg/bridge_flight_logger.hpp)
    target_include_directories(xrfg_bridge_flight_logger PUBLIC include)
    target_compile_features(xrfg_bridge_flight_logger PUBLIC cxx_std_20)
    target_compile_definitions(xrfg_bridge_flight_logger PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        XRFG_IMPLEMENTATION_VERSION=59)
    if(MSVC)
        target_compile_options(xrfg_bridge_flight_logger PRIVATE
            /W4 /permissive- /EHsc)
    endif()

    add_library(xrfg_implicit_layer STATIC
        src/common/implicit_layer.cpp
        include/xrfg/implicit_layer.hpp)
    target_include_directories(xrfg_implicit_layer PUBLIC include)
    target_compile_features(xrfg_implicit_layer PUBLIC cxx_std_20)
    target_compile_definitions(xrfg_implicit_layer PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE)
    target_link_libraries(xrfg_implicit_layer PUBLIC advapi32)
    if(MSVC)
        target_compile_options(xrfg_implicit_layer PRIVATE
            /W4 /permissive- /EHsc)
    endif()

    add_library(XR_APILAYER_XRFrameBridge_diagnostic SHARED
        src/layer/openxr_layer.cpp)
    target_include_directories(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE
        include
        "${XRFG_OPENXR_SDK_ROOT}/include")
    target_link_libraries(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE
        xrfg_core
        xrfg_d3d12_history
        xrfg_bridge_flight_logger
        xrfg_implicit_layer)
    target_compile_features(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE cxx_std_20)
    target_compile_definitions(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        XR_USE_GRAPHICS_API_D3D11
        XR_USE_GRAPHICS_API_D3D12
        XR_USE_PLATFORM_WIN32)
    if(MSVC)
        target_compile_options(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE
            /W4 /permissive- /EHsc)
    endif()

    add_custom_command(TARGET XR_APILAYER_XRFrameBridge_diagnostic POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/layer/ofxr_bridge.ini"
            "$<TARGET_FILE_DIR:XR_APILAYER_XRFrameBridge_diagnostic>/ofxr_bridge.ini")

    if(XRFG_BUILD_STANDALONE)
        add_library(xrfg_standalone_core STATIC
            src/tray/standalone_launcher.cpp
            include/xrfg/standalone_launcher.hpp)
        target_include_directories(xrfg_standalone_core PUBLIC include)
        target_compile_features(xrfg_standalone_core PUBLIC cxx_std_20)
        target_link_libraries(xrfg_standalone_core PUBLIC xrfg_implicit_layer)
        target_compile_definitions(xrfg_standalone_core PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            UNICODE
            _UNICODE)
        if(MSVC)
            target_compile_options(xrfg_standalone_core PRIVATE
                /W4 /permissive- /EHsc)
        endif()

        enable_language(RC)
        add_executable(OFXRBridgeTray WIN32
            src/tray/main.cpp
            src/tray/resources.rc)
        target_link_libraries(OFXRBridgeTray PRIVATE
            xrfg_standalone_core
            comctl32
            ole32
            shell32
            user32)
        target_compile_features(OFXRBridgeTray PRIVATE cxx_std_20)
        target_compile_definitions(OFXRBridgeTray PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            UNICODE
            _UNICODE)
        if(MSVC)
            target_compile_options(OFXRBridgeTray PRIVATE
                /W4 /permissive- /EHsc)
        endif()
        add_dependencies(OFXRBridgeTray
            XR_APILAYER_XRFrameBridge_diagnostic)
        add_custom_command(TARGET OFXRBridgeTray POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:OFXRBridgeTray>/ofxr"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "$<TARGET_FILE_DIR:OFXRBridgeTray>/ofxr/XR_APILAYER_XRFrameBridge_diagnostic.dll"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE_DIR:XR_APILAYER_XRFrameBridge_diagnostic>/ofxr_bridge.ini"
                "$<TARGET_FILE_DIR:OFXRBridgeTray>/ofxr/ofxr_bridge.ini")

        if(XRFG_BUILD_TESTS)
            add_executable(xrfg_standalone_launcher_tests
                tests/standalone_launcher_tests.cpp)
            target_link_libraries(xrfg_standalone_launcher_tests PRIVATE
                xrfg_standalone_core)
            target_compile_features(xrfg_standalone_launcher_tests PRIVATE
                cxx_std_20)
            if(MSVC)
                target_compile_options(xrfg_standalone_launcher_tests PRIVATE
                    /W4 /permissive- /EHsc)
                target_compile_definitions(xrfg_standalone_launcher_tests PRIVATE
                    NOMINMAX
                    WIN32_LEAN_AND_MEAN
                    UNICODE
                    _UNICODE)
            endif()
            add_test(NAME xrfg_standalone_launcher_tests
                COMMAND xrfg_standalone_launcher_tests)
        endif()
    endif()

    if(XRFG_BUILD_TESTS)
        add_executable(xrfg_bridge_flight_logger_tests
            tests/bridge_flight_logger_tests.cpp)
        target_link_libraries(xrfg_bridge_flight_logger_tests PRIVATE
            xrfg_bridge_flight_logger)
        target_compile_features(xrfg_bridge_flight_logger_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_bridge_flight_logger_tests PRIVATE
                /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_bridge_flight_logger_tests PRIVATE
                NOMINMAX
                WIN32_LEAN_AND_MEAN)
        endif()
        add_test(NAME xrfg_bridge_flight_logger_tests
            COMMAND xrfg_bridge_flight_logger_tests)

        add_executable(xrfg_layer_smoke tests/layer_smoke.cpp)
        target_include_directories(xrfg_layer_smoke PRIVATE
            "${XRFG_OPENXR_SDK_ROOT}/include")
        target_compile_features(xrfg_layer_smoke PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_layer_smoke PRIVATE /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_layer_smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        endif()
        add_test(
            NAME xrfg_layer_negotiation
            COMMAND xrfg_layer_smoke $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>)

        add_executable(xrfg_layer_call_chain tests/layer_call_chain.cpp)
        target_include_directories(xrfg_layer_call_chain PRIVATE
            "${XRFG_OPENXR_SDK_ROOT}/include")
        target_compile_features(xrfg_layer_call_chain PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_layer_call_chain PRIVATE /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_layer_call_chain PRIVATE
                NOMINMAX
                WIN32_LEAN_AND_MEAN
                XR_USE_GRAPHICS_API_D3D11
                XR_USE_GRAPHICS_API_D3D12
                XR_USE_PLATFORM_WIN32)
        endif()
        target_link_libraries(xrfg_layer_call_chain PRIVATE d3d11 d3d12 dxgi)
        add_dependencies(xrfg_layer_call_chain XR_APILAYER_XRFrameBridge_diagnostic)
        add_test(
            NAME xrfg_layer_call_chain
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-call-chain-test.log")

        add_test(
            NAME xrfg_layer_split_eye_call_chain
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-split-eye-call-chain-test.log"
                split-eye)

        add_test(
            NAME xrfg_layer_cropped_split_eye_call_chain
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-cropped-split-eye-call-chain-test.log"
                cropped-split-eye)

        add_test(
            NAME xrfg_layer_double_wide_call_chain
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-double-wide-call-chain-test.log"
                double-wide)

        add_test(
            NAME xrfg_layer_d3d11_interop
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-d3d11-interop-test.log"
                d3d11-interop)

        add_test(
            NAME xrfg_layer_d3d11_double_wide
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-d3d11-double-wide-test.log"
                d3d11-double-wide)

        add_test(
            NAME xrfg_layer_steamvr_inline
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-steamvr-inline-test.log"
                steamvr-inline)

        add_test(
            NAME xrfg_layer_steamvr_presenter_transition
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-steamvr-presenter-test.log"
                steamvr-presenter)

        add_test(
            NAME xrfg_layer_pipelined_presenter
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-pipelined-presenter-test.log"
                flight-simulator)

        add_test(
            NAME xrfg_layer_flight_logging
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DENABLED_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_logging_enabled.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/flight-logging-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_flight_logging.cmake")

        add_test(
            NAME xrfg_layer_d3d11_interop_logging
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DENABLED_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_logging_enabled.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/d3d11-interop-logging-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_d3d11_interop_logging.cmake")

        add_executable(xrfg_d3d12_history_tests tests/d3d12_history_tests.cpp)
        target_include_directories(xrfg_d3d12_history_tests PRIVATE include)
        target_link_libraries(xrfg_d3d12_history_tests PRIVATE
            xrfg_d3d12_history
            d3d12
            dxgi)
        target_compile_features(xrfg_d3d12_history_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_d3d12_history_tests PRIVATE
                /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_d3d12_history_tests PRIVATE
                NOMINMAX
                WIN32_LEAN_AND_MEAN)
        endif()
        add_test(NAME xrfg_d3d12_history_tests COMMAND xrfg_d3d12_history_tests)

        add_executable(xrfg_nvidia_optical_flow_probe
            tests/nvidia_optical_flow_probe.cpp)
        target_include_directories(xrfg_nvidia_optical_flow_probe PRIVATE
            "${XRFG_NVIDIA_OPTICAL_FLOW_INCLUDE_DIR}")
        target_link_libraries(xrfg_nvidia_optical_flow_probe PRIVATE d3d12 dxgi)
        target_compile_features(xrfg_nvidia_optical_flow_probe PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_nvidia_optical_flow_probe PRIVATE
                /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_nvidia_optical_flow_probe PRIVATE
                NOMINMAX
                WIN32_LEAN_AND_MEAN)
        endif()
    endif()
endif()
