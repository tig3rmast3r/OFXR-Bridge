file(MAKE_DIRECTORY "${DESTINATION}")
file(COPY_FILE "${TEST_EXE}" "${DESTINATION}/${GAME_EXE}" ONLY_IF_DIFFERENT)
execute_process(COMMAND "${DESTINATION}/${GAME_EXE}" "${CUSTOM_MARKER}" "${NAMED_MARKER}" --legacy-vulkan
  WORKING_DIRECTORY "${DESTINATION}" RESULT_VARIABLE result TIMEOUT 15)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "V666 selected Vulkan detection failed: ${result}")
endif()
