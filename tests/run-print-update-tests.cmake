get_filename_component(output_directory "${OUT}" DIRECTORY)
set(output_directory "${output_directory}/print-update-project")
file(MAKE_DIRECTORY "${output_directory}")
configure_file("${ROOT}/tests/print-update/project.zsettings"
               "${output_directory}/project.zsettings" COPYONLY)
set(OUT "${output_directory}/Main.zbc")
execute_process(COMMAND "${BIN}" run "${ROOT}/tests/print-update/Main.zsharp"
                RESULT_VARIABLE result OUTPUT_VARIABLE direct ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT direct MATCHES "Header" OR NOT direct MATCHES "Done" OR NOT direct MATCHES "Next")
 message(FATAL_ERROR "Print update failed: ${error}")
endif()
execute_process(COMMAND "${BIN}" compile "${ROOT}/tests/print-update/Main.zsharp" -o "${OUT}"
                RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
 message(FATAL_ERROR "Print update compile failed: ${error}")
endif()
execute_process(COMMAND "${BIN}" run-bytecode "${OUT}"
                RESULT_VARIABLE result OUTPUT_VARIABLE bytecode ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT direct STREQUAL bytecode)
 message(FATAL_ERROR "Print update bytecode output differs: ${error}")
endif()
