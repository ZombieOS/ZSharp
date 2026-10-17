cmake_minimum_required(VERSION 3.20)

foreach(required_path RUNTIME DUMPBIN)
    if(NOT DEFINED ${required_path} OR "${${required_path}}" STREQUAL "")
        message(FATAL_ERROR "${required_path} must specify an existing file")
    endif()
    if(NOT EXISTS "${${required_path}}" OR IS_DIRECTORY "${${required_path}}")
        message(FATAL_ERROR "${required_path} file does not exist: ${${required_path}}")
    endif()
endforeach()

execute_process(
    COMMAND "${DUMPBIN}" /dependents "${RUNTIME}"
    RESULT_VARIABLE inspection_result
    OUTPUT_VARIABLE inspection_output
    ERROR_VARIABLE inspection_error
)
if(NOT "${inspection_result}" STREQUAL "0")
    message(FATAL_ERROR
        "Could not inspect Windows runtime dependencies (${inspection_result}):\n"
        "${inspection_output}\n${inspection_error}")
endif()

# Inspect complete DLL-import lines, not incidental filenames in the header.
# Windows API-set/UCRT imports are deliberately allowed: they are OS components.
# Require a numbered VC family so the OS component msvcp_win.dll is allowed;
# include debug and STL support variants such as msvcp140d/140_atomic_wait.
string(TOLOWER "${inspection_output}" dependency_output)
string(REPLACE "\r\n" "\n" dependency_output "${dependency_output}")
string(REPLACE "\r" "\n" dependency_output "${dependency_output}")
if(dependency_output MATCHES
   "(^|\n)[ \t]*((msvcp|vcruntime|concrt)[0-9]+[a-z0-9_]*)\\.dll[ \t]*(\n|$)")
    message(FATAL_ERROR
        "Windows runtime still imports a Visual C++ runtime DLL: ${CMAKE_MATCH_2}.dll\n"
        "The release must not require an unbundled Visual C++ redistributable.\n"
        "${inspection_output}")
endif()

message(STATUS "PASS: Windows runtime has no Visual C++ runtime DLL imports: ${RUNTIME}")
