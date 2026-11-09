file(MAKE_DIRECTORY "${TEST_DIR}")
file(COPY_FILE "${TEST_EXE}" "${TEST_DIR}/RDR2.exe" ONLY_IF_DIFFERENT)
execute_process(COMMAND "${TEST_DIR}/RDR2.exe" rdr2-hdr
    WORKING_DIRECTORY "${TEST_DIR}" RESULT_VARIABLE result TIMEOUT 20)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "RDR2 native-HDR marker GPU failed (${result})")
endif()
