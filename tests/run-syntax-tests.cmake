if(NOT DEFINED ZSHARP_BIN OR NOT DEFINED PROJECT_ROOT OR
   NOT DEFINED TEST_OUTPUT OR NOT DEFINED TEST_PACKAGE OR
   NOT DEFINED TEST_SOURCE_PACKAGE OR NOT DEFINED TEST_GAME_PACKAGE OR
   NOT DEFINED TEST_GAME_SOURCE_PACKAGE OR
   NOT DEFINED TEST_PACKAGE_PROJECT OR NOT DEFINED TEST_GAME_PROJECT OR
   NOT DEFINED TEST_PACKAGE_CACHE OR NOT DEFINED TEST_PROJECT_REGISTRY OR
   NOT DEFINED TEST_SHORTCUT_CACHE OR NOT DEFINED TEST_DESKTOP)
    message(FATAL_ERROR "The Z# test paths were not supplied")
endif()

set(WINDOW_DIR "${PROJECT_ROOT}/tests/window")
set(ICON_INVALID_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/icon-invalid-project")
set(ENV{ZSHARP_SKIP_UPDATE_CHECK} "1")
set(ENV{ZSHARP_PACKAGE_REGISTRY} "${TEST_PROJECT_REGISTRY}.packages")
set(ENV{ZSHARP_PLAY_STATS_REGISTRY} "${TEST_PROJECT_REGISTRY}.playtime")
file(REMOVE "${TEST_OUTPUT}" "${TEST_OUTPUT}.mutation"
            "${TEST_PACKAGE}" "${TEST_SOURCE_PACKAGE}"
            "${TEST_GAME_PACKAGE}" "${TEST_GAME_SOURCE_PACKAGE}")
file(REMOVE_RECURSE "${TEST_PACKAGE_CACHE}" "${TEST_SHORTCUT_CACHE}"
                    "${TEST_DESKTOP}" "${TEST_PACKAGE_PROJECT}"
                    "${TEST_GAME_PROJECT}" "${ICON_INVALID_PROJECT}")
file(REMOVE "${TEST_PROJECT_REGISTRY}")
file(REMOVE "${TEST_PROJECT_REGISTRY}.packages")
file(REMOVE "${TEST_PROJECT_REGISTRY}.playtime")
file(MAKE_DIRECTORY "${TEST_PACKAGE_PROJECT}")
file(MAKE_DIRECTORY "${TEST_GAME_PROJECT}")
file(COPY "${PROJECT_ROOT}/tests/package/"
     DESTINATION "${TEST_PACKAGE_PROJECT}")
file(MAKE_DIRECTORY "${TEST_PACKAGE_PROJECT}/assets")
file(WRITE "${TEST_PACKAGE_PROJECT}/assets/project-icon.png" "test icon")
file(APPEND "${TEST_PACKAGE_PROJECT}/project.zsettings"
     "\nIcon: \"assets/project-icon.png\":\n")
file(MAKE_DIRECTORY "${ICON_INVALID_PROJECT}")
file(COPY "${PROJECT_ROOT}/tests/package/"
     DESTINATION "${ICON_INVALID_PROJECT}")
file(APPEND "${ICON_INVALID_PROJECT}/project.zsettings"
     "\nIcon: \"assets/missing.png\":\n")
file(COPY "${PROJECT_ROOT}/tests/game_package/"
     DESTINATION "${TEST_GAME_PROJECT}")

function(expect_success label working_dir)
    execute_process(
        COMMAND "${ZSHARP_BIN}" ${ARGN}
        WORKING_DIRECTORY "${working_dir}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${label} failed (${result})\nstdout: ${output}\nstderr: ${error}")
    endif()
endfunction()

function(expect_failure label expected working_dir)
    execute_process(
        COMMAND "${ZSHARP_BIN}" ${ARGN}
        WORKING_DIRECTORY "${working_dir}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(result EQUAL 0)
        message(FATAL_ERROR "${label} unexpectedly succeeded")
    endif()
    string(CONCAT combined "${output}" "${error}")
    string(FIND "${combined}" "${expected}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR
            "${label} did not report '${expected}'\n${combined}")
    endif()
endfunction()

expect_success("window settings" "${WINDOW_DIR}"
               check project.zsettings)
expect_failure("missing project icon" "project Icon"
               "${ICON_INVALID_PROJECT}" check project.zsettings)
expect_failure("settings cannot run directly"
               "project.zsettings cannot be run directly"
               "${WINDOW_DIR}" run project.zsettings)
expect_success("window source" "${WINDOW_DIR}"
               check Main.zsharp)
expect_success("window bytecode compilation" "${WINDOW_DIR}"
               compile Main.zsharp -o "${TEST_OUTPUT}")
expect_success("window bytecode validation" "${WINDOW_DIR}"
               check-bytecode "${TEST_OUTPUT}")
expect_success("window mutation bytecode compilation" "${WINDOW_DIR}"
               compile Mutations.zsharp -o "${TEST_OUTPUT}.mutation")
expect_success("window mutation bytecode validation" "${WINDOW_DIR}"
               check-bytecode "${TEST_OUTPUT}.mutation")
expect_success("Window namespace wildcard import" "${WINDOW_DIR}"
               check WildcardImport.zsharp)
expect_success("global official wildcard import" "${WINDOW_DIR}"
               check GlobalWildcardImport.zsharp)
if(WIN32)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                ZSHARP_WINDOW_AUTOCLOSE_MS=100
                ZSHARP_DISABLE_PROJECT_STARTS=1
                ZSHARP_WINDOW_AUTORUN_CALLBACK=Mutations:Actions:Theme
                "${ZSHARP_BIN}" run-bytecode "${TEST_OUTPUT}"
        WORKING_DIRECTORY "${WINDOW_DIR}"
        RESULT_VARIABLE window_result
        OUTPUT_VARIABLE window_output
        ERROR_VARIABLE window_error
    )
    if(NOT window_result EQUAL 0)
        message(FATAL_ERROR
            "native window smoke test failed (${window_result})\n"
            "stdout: ${window_output}\nstderr: ${window_error}")
    endif()
    string(FIND "${window_output}" "\n0\n1\n1\n1\n"
           input_metrics_index)
    if(input_metrics_index EQUAL -1)
        message(FATAL_ERROR
            "live text-input metrics returned unexpected values\n"
            "stdout: ${window_output}\nstderr: ${window_error}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                ZSHARP_WINDOW_AUTOCLOSE_MS=100
                "${ZSHARP_BIN}" run
                "${PROJECT_ROOT}/tests/window_autostart/Main.zsharp"
        WORKING_DIRECTORY "${PROJECT_ROOT}/tests/window_autostart"
        RESULT_VARIABLE auto_start_result
        OUTPUT_VARIABLE auto_start_output
        ERROR_VARIABLE auto_start_error
    )
    if(NOT auto_start_result EQUAL 0 OR
       NOT auto_start_output MATCHES "automatic start ran" OR
       auto_start_output MATCHES "DR start ran")
        message(FATAL_ERROR
            "automatic project Start test failed (${auto_start_result})\n"
            "stdout: ${auto_start_output}\nstderr: ${auto_start_error}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                ZSHARP_WINDOW_AUTOCLOSE_MS=200
                "${ZSHARP_BIN}" run
                "${PROJECT_ROOT}/tests/window_multitask/Main.zsharp"
        WORKING_DIRECTORY "${PROJECT_ROOT}/tests/window_multitask"
        RESULT_VARIABLE multitask_result
        OUTPUT_VARIABLE multitask_output
        ERROR_VARIABLE multitask_error
    )
    if(NOT multitask_result EQUAL 0 OR
       NOT multitask_output MATCHES "multitask A ran" OR
       NOT multitask_output MATCHES "multitask B ran")
        message(FATAL_ERROR
            "window multitasking test failed (${multitask_result})\n"
            "stdout: ${multitask_output}\nstderr: ${multitask_error}")
    endif()
endif()
expect_failure("invalid window color" "exactly #RRGGBB"
               "${WINDOW_DIR}" check InvalidColor.zsharp)
expect_failure("gradient requires two colors" "at least two colors"
               "${WINDOW_DIR}" check InvalidGradient.zsharp)
expect_failure("wait/delay duration unit" "expected 'ms' or 's'"
               "${WINDOW_DIR}" check InvalidDelay.zsharp)
expect_failure("statements cannot be direct room members"
               "executable statements must be inside a brain function"
               "${WINDOW_DIR}" check InvalidRoomStatement.zsharp)
expect_failure("loops do not use a closing colon"
               "a loop closes with ')' and does not use ':' after it"
               "${WINDOW_DIR}" check InvalidLoopColon.zsharp)
expect_failure("misspelled UI field" "use 'height'"
               "${WINDOW_DIR}" check InvalidField.zsharp)
expect_failure("image inputs cannot be multiline"
               "multiline and wrap are only valid for a text textInput"
               "${WINDOW_DIR}" check InvalidImageMultiline.zsharp)
expect_failure("wrap requires multiline input"
               "textInput wrap requires multiline: alive"
               "${WINDOW_DIR}" check InvalidWrap.zsharp)
expect_failure("missing window feature import" "ZSharp.Window.Text"
               "${WINDOW_DIR}" check MissingFeatureImport.zsharp)
expect_success("project wildcard import" "${PROJECT_ROOT}"
               check tests/imports/WildcardImport.zsharp)
expect_failure("wildcard import must be final"
               "'*' must be the final name in an import path"
               "${PROJECT_ROOT}"
               check tests/imports/InvalidWildcardPosition.zsharp)
expect_success("2D script header" "${PROJECT_ROOT}"
               check tests/Game2DHeader.zsharp)
expect_success("3D script header" "${PROJECT_ROOT}"
               check tests/Game3DHeader.zsharp)
expect_success("SDL/Vulkan game runtime build" "${PROJECT_ROOT}"
               game-info)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            ZSHARP_WINDOW_FORCE_FAILURE=1
            "ZSHARP_PROJECT_REGISTRY=${TEST_PROJECT_REGISTRY}"
            "${ZSHARP_BIN}" project "${TEST_PACKAGE_PROJECT}/project.zsettings"
    WORKING_DIRECTORY "${PROJECT_ROOT}"
    RESULT_VARIABLE project_result
    OUTPUT_VARIABLE project_output
    ERROR_VARIABLE project_error
)
if(NOT project_result EQUAL 0 OR
   NOT project_output MATCHES "registered Z# project" OR
   NOT EXISTS "${TEST_PROJECT_REGISTRY}")
    message(FATAL_ERROR
        "project registration failed or attempted to launch (${project_result})\n"
        "stdout: ${project_output}\nstderr: ${project_error}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            ZSHARP_WINDOW_FORCE_FAILURE=1
            "ZSHARP_PROJECT_REGISTRY=${TEST_PROJECT_REGISTRY}"
            "${ZSHARP_BIN}" project "${TEST_PACKAGE_PROJECT}"
    WORKING_DIRECTORY "${PROJECT_ROOT}"
    RESULT_VARIABLE reregister_result
    OUTPUT_VARIABLE reregister_output
    ERROR_VARIABLE reregister_error
)
file(READ "${TEST_PROJECT_REGISTRY}" registry_contents)
string(REGEX MATCHALL "package_test" registry_entries
       "${registry_contents}")
list(LENGTH registry_entries registry_entry_count)
if(NOT reregister_result EQUAL 0 OR NOT registry_entry_count EQUAL 1)
    message(FATAL_ERROR
        "project re-registration was not idempotent (${reregister_result}, "
        "${registry_entry_count} entries)\n"
        "stdout: ${reregister_output}\nstderr: ${reregister_error}")
endif()
expect_success("application packaging" "${PROJECT_ROOT}"
               package app "${TEST_PACKAGE_PROJECT}" PackageTest
               --unbytecode)
execute_process(
    COMMAND "${ZSHARP_BIN}" hub add "${TEST_PACKAGE}"
    WORKING_DIRECTORY "${PROJECT_ROOT}"
    RESULT_VARIABLE hub_add_result
    OUTPUT_VARIABLE hub_add_output
    ERROR_VARIABLE hub_add_error
)
execute_process(
    COMMAND "${ZSHARP_BIN}" hub list
    WORKING_DIRECTORY "${PROJECT_ROOT}"
    RESULT_VARIABLE hub_list_result
    OUTPUT_VARIABLE hub_list_output
    ERROR_VARIABLE hub_list_error
)
if(NOT hub_add_result EQUAL 0 OR NOT hub_list_result EQUAL 0 OR
   NOT hub_list_output MATCHES "package_test" OR
   NOT hub_list_output MATCHES "Packaged Window Test")
    message(FATAL_ERROR
        "Hub package registration failed\n"
        "add: ${hub_add_output}${hub_add_error}\n"
        "list: ${hub_list_output}${hub_list_error}")
endif()
execute_process(
    COMMAND "${ZSHARP_BIN}" hub remove package_test
    WORKING_DIRECTORY "${PROJECT_ROOT}"
    RESULT_VARIABLE hub_remove_result
    ERROR_VARIABLE hub_remove_error
)
execute_process(
    COMMAND "${ZSHARP_BIN}" hub list
    WORKING_DIRECTORY "${PROJECT_ROOT}"
    RESULT_VARIABLE hub_empty_result
    OUTPUT_VARIABLE hub_empty_output
    ERROR_VARIABLE hub_empty_error
)
if(NOT hub_remove_result EQUAL 0 OR NOT hub_empty_result EQUAL 0 OR
   NOT hub_empty_output MATCHES "0 installed packages")
    message(FATAL_ERROR
        "Hub package removal failed\n${hub_remove_error}\n"
        "${hub_empty_output}${hub_empty_error}")
endif()
expect_success("game packaging" "${PROJECT_ROOT}"
               package game "${TEST_GAME_PROJECT}" PackageTest --unbytecode)
if(NOT EXISTS "${TEST_PACKAGE}" OR NOT EXISTS "${TEST_SOURCE_PACKAGE}")
    message(FATAL_ERROR
        "--unbytecode did not create both application packages")
endif()
if(NOT EXISTS "${TEST_GAME_PACKAGE}" OR
   NOT EXISTS "${TEST_GAME_SOURCE_PACKAGE}")
    message(FATAL_ERROR
        "game --unbytecode did not create both game packages")
endif()
set(source_zip "${CMAKE_CURRENT_BINARY_DIR}/PackageTest-unbytecoded.zip")
file(REMOVE "${source_zip}")
file(RENAME "${TEST_SOURCE_PACKAGE}" "${source_zip}")
file(READ "${source_zip}" source_zip_hex HEX)
file(RENAME "${source_zip}" "${TEST_SOURCE_PACKAGE}")
string(FIND "${source_zip_hex}" "504b0304" source_zip_signature)
string(FIND "${source_zip_hex}" "4d61696e2e7a7368617270" source_main_name)
string(FIND "${source_zip_hex}"
       "7a7368617270203d20747970652e7363726970743a77696e646f77"
       source_main_contents)
string(FIND "${source_zip_hex}" "2e7a73686172702d62797465636f6465"
       source_bytecode_name)
if(NOT source_zip_signature EQUAL 0 OR source_main_name EQUAL -1 OR
   source_main_contents EQUAL -1 OR NOT source_bytecode_name EQUAL -1)
    message(FATAL_ERROR
        "renamed unbytecoded package did not expose the original source tree")
endif()
expect_failure("package filename extension guard"
               "use the filename without .zapp or .zgame"
               "${PROJECT_ROOT}"
               package app "${TEST_PACKAGE_PROJECT}" Invalid.zapp)
expect_failure("package filename path guard"
               "contains an invalid character"
               "${PROJECT_ROOT}"
               package app "${TEST_PACKAGE_PROJECT}" ../Invalid)
expect_failure("invalid game object field"
               "unknown game object field"
               "${PROJECT_ROOT}/tests/settings/game_invalid_object"
               package game
               "${PROJECT_ROOT}/tests/settings/game_invalid_object" Invalid)
if(WIN32)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                ZSHARP_WINDOW_AUTOCLOSE_MS=100
                ZSHARP_DISABLE_PROJECT_STARTS=1
                ZSHARP_SKIP_DESKTOP_INTEGRATION=1
                "ZSHARP_PACKAGE_CACHE=${TEST_PACKAGE_CACHE}"
                "${ZSHARP_BIN}" run "${TEST_PACKAGE}"
        WORKING_DIRECTORY "${PROJECT_ROOT}"
        RESULT_VARIABLE package_result
        OUTPUT_VARIABLE package_output
        ERROR_VARIABLE package_error
    )
    if(NOT package_result EQUAL 0)
        message(FATAL_ERROR
            "package run smoke test failed (${package_result})\n"
            "stdout: ${package_output}\nstderr: ${package_error}")
    endif()
    if(NOT package_output MATCHES "running bytecoded startup")
        message(FATAL_ERROR
            "normal application package did not use bytecode\n${package_output}")
    endif()
    if(NOT EXISTS "${TEST_PROJECT_REGISTRY}.playtime")
        message(FATAL_ERROR "package launch did not create Hub playtime data")
    endif()
    file(READ "${TEST_PROJECT_REGISTRY}.playtime" playtime_contents)
    if(NOT playtime_contents MATCHES "package_test")
        message(FATAL_ERROR "Hub playtime data did not contain package_test")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                ZSHARP_WINDOW_AUTOCLOSE_MS=100
                ZSHARP_DISABLE_PROJECT_STARTS=1
                ZSHARP_SKIP_DESKTOP_INTEGRATION=1
                "ZSHARP_PACKAGE_CACHE=${TEST_PACKAGE_CACHE}"
                "${ZSHARP_BIN}" open-desktop "${TEST_PACKAGE}"
        WORKING_DIRECTORY "${PROJECT_ROOT}"
        RESULT_VARIABLE desktop_open_result
        ERROR_VARIABLE desktop_open_error
    )
    if(NOT desktop_open_result EQUAL 0)
        message(FATAL_ERROR
            "desktop package launch failed (${desktop_open_result})\n"
            "stderr: ${desktop_open_error}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                ZSHARP_WINDOW_AUTOCLOSE_MS=100
                ZSHARP_DISABLE_PROJECT_STARTS=1
                ZSHARP_SKIP_DESKTOP_INTEGRATION=1
                "ZSHARP_PACKAGE_CACHE=${TEST_PACKAGE_CACHE}"
                "${ZSHARP_BIN}" run "${TEST_SOURCE_PACKAGE}"
        WORKING_DIRECTORY "${PROJECT_ROOT}"
        RESULT_VARIABLE source_package_result
        OUTPUT_VARIABLE source_package_output
        ERROR_VARIABLE source_package_error
    )
    if(NOT source_package_result EQUAL 0 OR
       NOT source_package_output MATCHES "running unbytecoded source startup")
        message(FATAL_ERROR
            "unbytecoded package run failed (${source_package_result})\n"
            "stdout: ${source_package_output}\n"
            "stderr: ${source_package_error}")
    endif()
    file(MAKE_DIRECTORY "${TEST_DESKTOP}")
    set(shortcut_answer "${CMAKE_CURRENT_BINARY_DIR}/shortcut-answer.txt")
    file(WRITE "${shortcut_answer}" "yes\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                ZSHARP_WINDOW_AUTOCLOSE_MS=100
                ZSHARP_DISABLE_PROJECT_STARTS=1
                ZSHARP_SKIP_ASSOCIATION_INSTALL=1
                "ZSHARP_DESKTOP_DIRECTORY=${TEST_DESKTOP}"
                "ZSHARP_PACKAGE_CACHE=${TEST_SHORTCUT_CACHE}"
                "${ZSHARP_BIN}" open "${TEST_PACKAGE}"
        WORKING_DIRECTORY "${PROJECT_ROOT}"
        INPUT_FILE "${shortcut_answer}"
        RESULT_VARIABLE shortcut_result
        OUTPUT_VARIABLE shortcut_output
        ERROR_VARIABLE shortcut_error
    )
    if(NOT shortcut_result EQUAL 0 OR
       NOT EXISTS "${TEST_DESKTOP}/Packaged Window Test.lnk" OR
       NOT shortcut_output MATCHES "Desktop shortcut created")
        message(FATAL_ERROR
            "desktop shortcut prompt failed (${shortcut_result})\n"
            "stdout: ${shortcut_output}\nstderr: ${shortcut_error}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                ZSHARP_WINDOW_AUTOCLOSE_MS=100
                ZSHARP_DISABLE_PROJECT_STARTS=1
                ZSHARP_SKIP_ASSOCIATION_INSTALL=1
                "ZSHARP_DESKTOP_DIRECTORY=${TEST_DESKTOP}"
                "ZSHARP_PACKAGE_CACHE=${TEST_SHORTCUT_CACHE}"
                "${ZSHARP_BIN}" open "${TEST_PACKAGE}"
        WORKING_DIRECTORY "${PROJECT_ROOT}"
        RESULT_VARIABLE shortcut_refresh_result
        ERROR_VARIABLE shortcut_refresh_error
    )
    if(NOT shortcut_refresh_result EQUAL 0 OR
       NOT EXISTS "${TEST_DESKTOP}/Packaged Window Test.lnk")
        message(FATAL_ERROR
            "desktop shortcut refresh failed (${shortcut_refresh_result})\n"
            "stderr: ${shortcut_refresh_error}")
    endif()
    set(uninstall_package "${CMAKE_CURRENT_BINARY_DIR}/UninstallTest.zapp")
    file(COPY_FILE "${TEST_PACKAGE}" "${uninstall_package}" ONLY_IF_DIFFERENT)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "ZSHARP_DESKTOP_DIRECTORY=${TEST_DESKTOP}"
                "ZSHARP_PACKAGE_CACHE=${TEST_SHORTCUT_CACHE}"
                "${ZSHARP_BIN}" uninstall "${uninstall_package}"
        WORKING_DIRECTORY "${PROJECT_ROOT}"
        INPUT_FILE "${shortcut_answer}"
        RESULT_VARIABLE uninstall_result
        OUTPUT_VARIABLE uninstall_output
        ERROR_VARIABLE uninstall_error
    )
    if(NOT uninstall_result EQUAL 0 OR EXISTS "${uninstall_package}" OR
       EXISTS "${TEST_DESKTOP}/Packaged Window Test.lnk" OR
       NOT uninstall_output MATCHES "Uninstall completed")
        message(FATAL_ERROR
            "desktop shortcut uninstall cleanup failed (${uninstall_result})\n"
            "stdout: ${uninstall_output}\nstderr: ${uninstall_error}")
    endif()
endif()
if(WIN32)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                ZSHARP_GAME_AUTOCLOSE_MS=250
                ZSHARP_SKIP_DESKTOP_INTEGRATION=1
                "ZSHARP_PACKAGE_CACHE=${TEST_PACKAGE_CACHE}"
                "${ZSHARP_BIN}" open "${TEST_GAME_PACKAGE}"
        WORKING_DIRECTORY "${PROJECT_ROOT}"
        RESULT_VARIABLE game_result
        OUTPUT_VARIABLE game_output
        ERROR_VARIABLE game_error
    )
    if(NOT game_result EQUAL 0 OR
       NOT game_output MATCHES "running bytecoded startup" OR
       NOT game_output MATCHES "game start ran")
        message(FATAL_ERROR
            "Vulkan game package smoke test failed (${game_result})\n"
            "stdout: ${game_output}\nstderr: ${game_error}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                ZSHARP_GAME_AUTOCLOSE_MS=250
                ZSHARP_SKIP_DESKTOP_INTEGRATION=1
                "ZSHARP_PACKAGE_CACHE=${TEST_PACKAGE_CACHE}"
                "${ZSHARP_BIN}" open "${TEST_GAME_SOURCE_PACKAGE}"
        WORKING_DIRECTORY "${PROJECT_ROOT}"
        RESULT_VARIABLE game_source_result
        OUTPUT_VARIABLE game_source_output
        ERROR_VARIABLE game_source_error
    )
    if(NOT game_source_result EQUAL 0 OR
       NOT game_source_output MATCHES "running unbytecoded source startup" OR
       NOT game_source_output MATCHES "game start ran")
        message(FATAL_ERROR
            "source game package smoke test failed (${game_source_result})\n"
            "stdout: ${game_source_output}\n"
            "stderr: ${game_source_error}")
    endif()
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            ZSHARP_HUB_CONSOLE_ONLY=1
            ZSHARP_SKIP_DESKTOP_INTEGRATION=1
            ZSHARP_WINDOW_FORCE_FAILURE=1
            "ZSHARP_PACKAGE_CACHE=${TEST_PACKAGE_CACHE}"
            "${ZSHARP_BIN}" open "${TEST_PACKAGE}"
    WORKING_DIRECTORY "${PROJECT_ROOT}"
    RESULT_VARIABLE failure_result
    OUTPUT_VARIABLE failure_output
    ERROR_VARIABLE failure_error
)
string(CONCAT failure_combined "${failure_output}" "${failure_error}")
if(failure_result EQUAL 0 OR
   NOT failure_combined MATCHES "Packaged Window Test failed to launch!" OR
   NOT failure_combined MATCHES "forced window launch failure")
    message(FATAL_ERROR
        "failed app Hub routing did not preserve its reason (${failure_result})\n"
        "${failure_combined}")
endif()
expect_failure("Window settings dependency guard"
               "require the zsharpwindow dependency"
               "${PROJECT_ROOT}/tests/settings/window_missing_dependency"
               check project.zsettings)
expect_failure("Window settings script-type guard"
               "must use zsharp = type.script:window"
               "${PROJECT_ROOT}/tests/settings/window_wrong_type"
               check project.zsettings)
expect_failure("window ZSS declaration guard"
               "unsupported or invalid ZSS declaration"
               "${PROJECT_ROOT}/tests/settings/window_invalid_zss"
               check project.zsettings)
expect_failure("game dependency guard"
               "require zsharpgame:1.0.0.0"
               "${PROJECT_ROOT}/tests/settings/game_missing_dependency"
               check Game.zsharp)

file(REMOVE "${TEST_OUTPUT}")
file(REMOVE "${TEST_PACKAGE}" "${TEST_SOURCE_PACKAGE}"
            "${TEST_GAME_PACKAGE}" "${TEST_GAME_SOURCE_PACKAGE}")
file(REMOVE_RECURSE "${TEST_PACKAGE_CACHE}")
file(REMOVE_RECURSE "${TEST_SHORTCUT_CACHE}" "${TEST_DESKTOP}")
file(REMOVE_RECURSE "${TEST_PACKAGE_PROJECT}")
file(REMOVE_RECURSE "${TEST_GAME_PROJECT}")
file(REMOVE_RECURSE "${ICON_INVALID_PROJECT}")
file(REMOVE "${TEST_PROJECT_REGISTRY}")
file(REMOVE "${TEST_PROJECT_REGISTRY}.playtime")
if(DEFINED shortcut_answer)
    file(REMOVE "${shortcut_answer}")
endif()
