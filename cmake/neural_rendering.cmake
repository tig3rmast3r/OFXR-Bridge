# Autonomous extraction of the accepted OptiScaler V33034 NR component only.
# No OptiScaler target, proxy runtime, menu renderer or generated compiler tree.
add_library(xrfg_neural_rendering STATIC
    src/neural/nr_feature.cpp
    src/neural/nr_host.cpp
    src/neural/nr_codec.cpp)
target_include_directories(xrfg_neural_rendering PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include" "${XRFG_DLSS_SDK_ROOT}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/neural")
target_compile_features(xrfg_neural_rendering PUBLIC cxx_std_20)
target_compile_definitions(xrfg_neural_rendering PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
    _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
target_link_libraries(xrfg_neural_rendering PUBLIC d3d12 dxgi d3dcompiler advapi32)
if(MSVC)
    target_compile_options(xrfg_neural_rendering PRIVATE /W4 /permissive- /EHsc)
endif()

add_library(xrfg_neural_forwarder SHARED src/neural/forwarder/dlssnr_forwarder.cpp)
target_compile_features(xrfg_neural_forwarder PRIVATE cxx_std_17)
target_link_libraries(xrfg_neural_forwarder PRIVATE d3d12)
set_target_properties(xrfg_neural_forwarder PROPERTIES
    OUTPUT_NAME "nvngx.dll_dlssnr" PREFIX "" SUFFIX ".dll")

if(XRFG_BUILD_TESTS)
    add_executable(xrfg_neural_device_removal_tests tests/neural_device_removal_tests.cpp tests/neural_capture_stubs.cpp)
    target_include_directories(xrfg_neural_device_removal_tests PRIVATE src/neural)
    target_compile_features(xrfg_neural_device_removal_tests PRIVATE cxx_std_20)
    target_compile_definitions(xrfg_neural_device_removal_tests PRIVATE WIN32_LEAN_AND_MEAN _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    target_link_libraries(xrfg_neural_device_removal_tests PRIVATE xrfg_neural_rendering d3d12 dxgi)
    add_test(NAME xrfg_neural_device_removal_tests COMMAND xrfg_neural_device_removal_tests)
    add_executable(xrfg_neural_contract_tests tests/neural_contract_tests.cpp)
    target_include_directories(xrfg_neural_contract_tests PRIVATE src/neural)
    target_compile_features(xrfg_neural_contract_tests PRIVATE cxx_std_20)
    add_test(NAME xrfg_neural_contract_tests COMMAND xrfg_neural_contract_tests)
    add_executable(xrfg_neural_codec_gpu_tests tests/neural_codec_gpu_tests.cpp)
    target_include_directories(xrfg_neural_codec_gpu_tests PRIVATE src/neural)
    target_compile_definitions(xrfg_neural_codec_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(xrfg_neural_codec_gpu_tests PRIVATE xrfg_neural_rendering)
    add_test(NAME xrfg_neural_codec_gpu_tests COMMAND xrfg_neural_codec_gpu_tests)
    add_executable(xrfg_neural_multipass_tests tests/neural_multipass_tests.cpp)
    target_include_directories(xrfg_neural_multipass_tests PRIVATE src/neural)
    target_compile_features(xrfg_neural_multipass_tests PRIVATE cxx_std_20)
    add_test(NAME xrfg_neural_multipass_tests COMMAND xrfg_neural_multipass_tests)
    add_executable(xrfg_neural_multipass_host_tests
        tests/neural_multipass_host_tests.cpp src/neural/nr_codec.cpp)
    target_include_directories(xrfg_neural_multipass_host_tests PRIVATE
        include src/neural "${XRFG_DLSS_SDK_ROOT}/include")
    target_compile_features(xrfg_neural_multipass_host_tests PRIVATE cxx_std_20)
    target_compile_definitions(xrfg_neural_multipass_host_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    target_link_libraries(xrfg_neural_multipass_host_tests PRIVATE d3d12 dxgi d3dcompiler advapi32)
    add_test(NAME xrfg_neural_multipass_host_pre COMMAND xrfg_neural_multipass_host_tests pre)
    add_test(NAME xrfg_neural_multipass_host_post COMMAND xrfg_neural_multipass_host_tests post)
    set_tests_properties(xrfg_neural_multipass_host_pre xrfg_neural_multipass_host_post
        PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    add_executable(xrfg_neural_clamp_gpu_tests tests/neural_clamp_gpu_tests.cpp)
    target_include_directories(xrfg_neural_clamp_gpu_tests PRIVATE src/neural)
    target_compile_definitions(xrfg_neural_clamp_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(xrfg_neural_clamp_gpu_tests PRIVATE xrfg_neural_rendering)
    add_test(NAME xrfg_neural_clamp_warp COMMAND xrfg_neural_clamp_gpu_tests)
    set_tests_properties(xrfg_neural_clamp_warp PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
    if(XRFG_TEST_RIFE_GPU)
        add_test(NAME xrfg_neural_clamp_nvidia COMMAND xrfg_neural_clamp_gpu_tests --nvidia)
        set_tests_properties(xrfg_neural_clamp_nvidia PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
    endif()
endif()

if(TARGET xrfg_neural_multipass_host_tests)
    add_test(NAME xrfg_neural_quad_full COMMAND xrfg_neural_multipass_host_tests quad)
    add_test(NAME xrfg_neural_quad_copy COMMAND xrfg_neural_multipass_host_tests quad-copy)
    add_test(NAME xrfg_neural_quad_half COMMAND xrfg_neural_multipass_host_tests quad-half)
    set_tests_properties(xrfg_neural_quad_full xrfg_neural_quad_half xrfg_neural_quad_copy PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endif()

if(TARGET xrfg_neural_multipass_host_tests)
 add_test(NAME xrfg_neural_quad_depth_array COMMAND xrfg_neural_multipass_host_tests quad-array)
 add_test(NAME xrfg_neural_quad_depth_array_half COMMAND xrfg_neural_multipass_host_tests quad-array-half)
 set_tests_properties(xrfg_neural_quad_depth_array xrfg_neural_quad_depth_array_half PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endif()
