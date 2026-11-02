if(NOT EXISTS "${TEST_EXE}" OR NOT EXISTS "${LUKE_FIXTURE}")
  message(FATAL_ERROR "Missing compiled fixture")
endif()
file(MAKE_DIRECTORY "${DESTINATION}")
file(COPY_FILE "${TEST_EXE}" "${DESTINATION}/${GAME_EXE}" ONLY_IF_DIFFERENT)
file(COPY_FILE "${LUKE_FIXTURE}" "${DESTINATION}/luke-runtime-fixture.dll" ONLY_IF_DIFFERENT)
# No layer registration, live tray, game directory or active game INI is changed.
execute_process(COMMAND "${CMAKE_COMMAND}" -E env OFXR_DISABLE_VULKAN_QUEUE_LAYER=1 "VK_LOADER_LAYERS_DISABLE=~implicit~" "${DESTINATION}/${GAME_EXE}" native-vulkan-bootstrap
  WORKING_DIRECTORY "${DESTINATION}" RESULT_VARIABLE result TIMEOUT 25)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Native Luke Vulkan bootstrap failed: ${result}")
endif()
