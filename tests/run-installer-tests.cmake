if(NOT DEFINED INSTALLER_BIN OR NOT DEFINED RUNTIME_BIN OR
   NOT DEFINED TEST_PLATFORM OR NOT DEFINED TEST_ROOT OR NOT DEFINED ZIP_TOOL)
    message(FATAL_ERROR "The Z# installer test paths were not supplied")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(SIZE "${RUNTIME_BIN}" runtime_size)
file(SHA256 "${RUNTIME_BIN}" runtime_sha256)
if(WIN32)
    set(runtime_name "zsharp.exe")
else()
    set(runtime_name "zsharp")
endif()
set(test_archive "${TEST_ROOT}/ZVM-LATEST.zip")
execute_process(
    COMMAND "${ZIP_TOOL}" "${RUNTIME_BIN}" "${test_archive}"
            "runtimes/${TEST_PLATFORM}/${runtime_name}"
    RESULT_VARIABLE archive_result
    ERROR_VARIABLE archive_error
)
if(NOT archive_result EQUAL 0)
    message(FATAL_ERROR "Could not create the installer test archive: ${archive_error}")
endif()
file(SIZE "${test_archive}" archive_size)
file(SHA256 "${test_archive}" archive_sha256)
set(manifest "${TEST_ROOT}/update.js")
set(install_directory "${TEST_ROOT}/install")
file(WRITE "${manifest}"
    "{\n"
    "  \"schema\": 1,\n"
    "  \"latestVersion\": \"1.0.1.1\",\n"
    "  \"download\": {\n"
    "    \"url\": \"https://example.invalid/ZVM-LATEST.zip\",\n"
    "    \"sha256\": \"${archive_sha256}\",\n"
    "    \"size\": ${archive_size}\n"
    "  },\n"
    "  \"platforms\": {\n"
    "   \"${TEST_PLATFORM}\": {\n"
    "    \"path\": \"runtimes/${TEST_PLATFORM}/${runtime_name}\",\n"
    "    \"sha256\": \"${runtime_sha256}\",\n"
    "    \"size\": ${runtime_size}\n"
    "   }\n"
    "  }\n"
    "}\n")

function(run_installer label)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "ZSHARP_INSTALLER_ARTIFACT_FILE=${test_archive}"
                "ZSHARP_INSTALLER_INSTALL_DIR=${install_directory}"
                ZSHARP_INSTALLER_SKIP_INTEGRATION=1
                "${INSTALLER_BIN}" --manifest-file "${manifest}" --yes
        RESULT_VARIABLE installer_result
        OUTPUT_VARIABLE installer_output
        ERROR_VARIABLE installer_error
    )
    if(NOT installer_result EQUAL 0 OR
       NOT installer_output MATCHES "installed successfully")
        message(FATAL_ERROR
            "${label} failed (${installer_result})\n"
            "stdout: ${installer_output}\nstderr: ${installer_error}")
    endif()
endfunction()

if(WIN32)
    set(installed_runtime "${install_directory}/zsharp.exe")
    set(backup_runtime "${install_directory}/zsharp.previous.exe")
    set(installed_updater "${install_directory}/zsharp-installer.exe")
    set(rejected_runtime "${TEST_ROOT}/rejected/zsharp.exe")
else()
    set(installed_runtime "${install_directory}/zsharp")
    set(backup_runtime "${install_directory}/zsharp.previous")
    set(installed_updater "${install_directory}/zsharp-installer")
    set(rejected_runtime "${TEST_ROOT}/rejected/zsharp")
endif()

run_installer("clean bootstrap installation")
if(NOT EXISTS "${installed_runtime}" OR NOT EXISTS "${installed_updater}")
    message(FATAL_ERROR "The bootstrap installer did not create the ZVM and updater")
endif()
file(SHA256 "${installed_runtime}" installed_sha256)
if(NOT installed_sha256 STREQUAL runtime_sha256)
    message(FATAL_ERROR "The installed ZVM checksum does not match")
endif()

run_installer("existing runtime update")
if(NOT EXISTS "${backup_runtime}")
    message(FATAL_ERROR "Updating did not preserve the previous ZVM")
endif()
file(SHA256 "${backup_runtime}" backup_sha256)
if(NOT backup_sha256 STREQUAL runtime_sha256)
    message(FATAL_ERROR "The preserved ZVM backup checksum does not match")
endif()

set(current_manifest "${TEST_ROOT}/current-update.js")
file(WRITE "${current_manifest}"
    "{\"schema\":1,\"latestVersion\":\"1.0.1.1\"}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "ZSHARP_INSTALLER_INSTALL_DIR=${install_directory}"
            ZSHARP_INSTALLER_SKIP_INTEGRATION=1
            "${installed_updater}" --check --current-version 1.0.1.1
            --manifest-file "${current_manifest}"
    RESULT_VARIABLE current_result
    OUTPUT_VARIABLE current_output
    ERROR_VARIABLE current_error
)
if(NOT current_result EQUAL 0 OR
   NOT current_output MATCHES "already current")
    message(FATAL_ERROR
        "A current ZVM was not left unchanged (${current_result})\n"
        "stdout: ${current_output}\nstderr: ${current_error}")
endif()
file(SHA256 "${installed_runtime}" current_runtime_sha256)
if(NOT current_runtime_sha256 STREQUAL runtime_sha256)
    message(FATAL_ERROR "The no-update check changed the installed ZVM")
endif()

set(rejected_directory "${TEST_ROOT}/rejected")
set(rejected_manifest "${TEST_ROOT}/rejected-update.js")
file(WRITE "${rejected_manifest}"
    "{\"schema\":1,\"${TEST_PLATFORM}\":{"
    "\"path\":\"runtimes/${TEST_PLATFORM}/${runtime_name}\","
    "\"sha256\":\"${runtime_sha256}\","
    "\"size\":${runtime_size}},"
    "\"latestVersion\":\"1.0.1.1\","
    "\"download\":{\"url\":\"https://example.invalid/ZVM-LATEST.zip\","
    "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\","
    "\"size\":${archive_size}}}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "ZSHARP_INSTALLER_ARTIFACT_FILE=${test_archive}"
            "ZSHARP_INSTALLER_INSTALL_DIR=${rejected_directory}"
            ZSHARP_INSTALLER_SKIP_INTEGRATION=1
            "${INSTALLER_BIN}" --manifest-file "${rejected_manifest}" --yes
    RESULT_VARIABLE rejected_result
    OUTPUT_VARIABLE rejected_output
    ERROR_VARIABLE rejected_error
)
if(rejected_result EQUAL 0 OR
   EXISTS "${rejected_runtime}" OR
   NOT rejected_error MATCHES "archive failed SHA-256 verification")
    message(FATAL_ERROR
        "The installer accepted a bad runtime checksum (${rejected_result})\n"
        "stdout: ${rejected_output}\nstderr: ${rejected_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ZSHARP_SKIP_UPDATE_CHECK=1
            "${installed_runtime}" --version
    RESULT_VARIABLE version_result
    OUTPUT_VARIABLE version_output
    ERROR_VARIABLE version_error
)
if(NOT version_result EQUAL 0 OR NOT version_output MATCHES "Z# 1.0.1.1")
    message(FATAL_ERROR
        "The installed ZVM did not run (${version_result})\n"
        "stdout: ${version_output}\nstderr: ${version_error}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
