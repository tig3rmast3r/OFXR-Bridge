file(MAKE_DIRECTORY "${TEST_DIR}")
file(COPY_FILE "${TEST_EXE}" "${TEST_DIR}/DaysGone.exe" ONLY_IF_DIFFERENT)
execute_process(COMMAND "${TEST_DIR}/DaysGone.exe" daysgone-eye-metadata
    WORKING_DIRECTORY "${TEST_DIR}" RESULT_VARIABLE result TIMEOUT 30)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Days Gone native eye metadata failed (${result})")
endif()
