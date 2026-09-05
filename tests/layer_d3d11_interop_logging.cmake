foreach(required_variable IN ITEMS LAYER_DLL CALL_CHAIN ENABLED_INI WORK_DIR)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} was not provided")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(COPY "${LAYER_DLL}" DESTINATION "${WORK_DIR}")
file(COPY "${ENABLED_INI}" DESTINATION "${WORK_DIR}")
file(RENAME
    "${WORK_DIR}/ofxr_bridge_logging_enabled.ini"
    "${WORK_DIR}/ofxr_bridge.ini")

get_filename_component(layer_name "${LAYER_DLL}" NAME)
execute_process(
    COMMAND "${CALL_CHAIN}"
        "${WORK_DIR}/${layer_name}"
        "${WORK_DIR}/fake-runtime.log"
        d3d11-interop
    RESULT_VARIABLE call_chain_result
    OUTPUT_VARIABLE call_chain_output
    ERROR_VARIABLE call_chain_error)
if(NOT call_chain_result EQUAL 0)
    message(FATAL_ERROR
        "D3D11 interop call chain failed (${call_chain_result}):\n"
        "${call_chain_output}\n${call_chain_error}")
endif()

file(GLOB flight_logs "${WORK_DIR}/ofxr-bridge-flight-*.log")
list(LENGTH flight_logs flight_log_count)
if(NOT flight_log_count EQUAL 1)
    message(FATAL_ERROR
        "Expected one OFXR flight log, found ${flight_log_count}")
endif()
list(GET flight_logs 0 flight_log)
file(READ "${flight_log}" contents)

foreach(required IN ITEMS
        "phase=I op=session_binding result=1"
        "phase=I op=swapchain_image result=27"
        "phase=I op=swapchain_eligibility result=0"
        "phase=B op=synthesis_initialize"
        "phase=E op=synthesis_initialize result=0"
        "phase=B op=d3d11_capture"
        "phase=E op=d3d11_capture result=0"
        "phase=I op=d3d11_capture result=0"
        "phase=B op=d3d11_publish"
        "phase=E op=d3d11_publish result=0"
        "phase=B op=synthesis_pair"
        "phase=E op=synthesis_pair result=0"
        "phase=I op=generation_prepare result=0"
        "phase=E op=internal_end_frame result=0")
    string(FIND "${contents}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "D3D11 interop log is missing: ${required}")
    endif()
endforeach()

foreach(forbidden IN ITEMS
        "op=swapchain_eligibility result=15"
        "op=d3d11_capture result=-"
        "op=d3d11_publish result=-")
    string(FIND "${contents}" "${forbidden}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR "D3D11 interop log contains: ${forbidden}")
    endif()
endforeach()

message(STATUS "OFXR ReShade/D3D11 active interop logging contract verified")
