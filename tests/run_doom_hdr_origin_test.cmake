file(MAKE_DIRECTORY "${TEST_DIR}")
file(COPY_FILE "${TEST_EXE}" "${TEST_DIR}/DOOMTheDarkAges.exe" ONLY_IF_DIFFERENT)
execute_process(COMMAND "${TEST_DIR}/DOOMTheDarkAges.exe" doom-hdr
    WORKING_DIRECTORY "${TEST_DIR}" RESULT_VARIABLE result TIMEOUT 20)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "DOOM HDR marker GPU failed (${result})")
endif()
