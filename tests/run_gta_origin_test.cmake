# Exercise the actual title selector on an isolated test copy only.
if(NOT EXISTS "${TEST_EXE}" OR NOT TEST_DIR)
    message(FATAL_ERROR "Missing GTA origin test executable/directory")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
configure_file("${TEST_EXE}" "${TEST_DIR}/GTA5_Enhanced.exe" COPYONLY)
if(NOT TEST_MODE)
    set(TEST_MODE gta-protected)
endif()
execute_process(COMMAND "${TEST_DIR}/GTA5_Enhanced.exe" ${TEST_MODE}
    WORKING_DIRECTORY "${TEST_DIR}" RESULT_VARIABLE result
    OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
message("${output}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "GTA protected origin failed (${result}): ${error}")
endif()
