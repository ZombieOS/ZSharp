cmake_minimum_required(VERSION 3.20)

project(zsharp VERSION 1.4.0.0 LANGUAGES C)
if(MSVC)
    # The bootstrap installs one native executable, not a machine-wide VC++
    # redistributable. Keep the C and C++ runtimes inside that executable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(ANDROID)
    enable_language(CXX)
endif()

option(ZSHARP_BUILD_EXAMPLES "Build Z# integration examples" OFF)
option(ZSHARP_BUILD_GAME_RUNTIME
       "Build the SDL3 and Vulkan Z# game runtime" ON)
option(ZSHARP_CORE_ONLY
       "Build only the portable Z# language core for platform ports" OFF)
option(ZSHARP_BUNDLE_MOLTENVK
       "Bundle the official MoltenVK runtime in macOS builds" ON)

add_subdirectory(native/core)
if(ZSHARP_CORE_ONLY)
    return()
endif()

include(CTest)
include(FetchContent)
find_package(Threads REQUIRED)

set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    QuickJS
    URL https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.16.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(QuickJS)

FetchContent_Declare(
    Lua
    URL https://www.lua.org/ftp/lua-5.5.1.tar.gz
    URL_HASH SHA256=1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(Lua)
FetchContent_Declare(
    ufbx
    URL https://codeload.github.com/ufbx/ufbx/tar.gz/26a482ae66871d7de36eb722aa060bce95bce274
    URL_HASH SHA256=18d5a7849378e3fe247b0df67cf845cb593c73172cbac0de99b5aba130542667
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_GetProperties(ufbx)
if(NOT ufbx_POPULATED)
    FetchContent_Populate(ufbx)
endif()
add_library(zsharp_ufbx STATIC "${ufbx_SOURCE_DIR}/ufbx.c")
target_include_directories(zsharp_ufbx PUBLIC "${ufbx_SOURCE_DIR}")
target_compile_features(zsharp_ufbx PRIVATE c_std_17)
set(LUA_SOURCE_DIR "${lua_SOURCE_DIR}/src")
add_library(zsharp_lua STATIC
    ${LUA_SOURCE_DIR}/lapi.c ${LUA_SOURCE_DIR}/lauxlib.c
    ${LUA_SOURCE_DIR}/lbaselib.c ${LUA_SOURCE_DIR}/lcode.c
    ${LUA_SOURCE_DIR}/lcorolib.c ${LUA_SOURCE_DIR}/lctype.c
    ${LUA_SOURCE_DIR}/ldblib.c ${LUA_SOURCE_DIR}/ldebug.c
    ${LUA_SOURCE_DIR}/ldo.c ${LUA_SOURCE_DIR}/ldump.c
    ${LUA_SOURCE_DIR}/lfunc.c ${LUA_SOURCE_DIR}/lgc.c
    ${LUA_SOURCE_DIR}/linit.c ${LUA_SOURCE_DIR}/liolib.c
    ${LUA_SOURCE_DIR}/llex.c ${LUA_SOURCE_DIR}/lmathlib.c
    ${LUA_SOURCE_DIR}/lmem.c ${LUA_SOURCE_DIR}/loadlib.c
    ${LUA_SOURCE_DIR}/lobject.c ${LUA_SOURCE_DIR}/lopcodes.c
    ${LUA_SOURCE_DIR}/loslib.c ${LUA_SOURCE_DIR}/lparser.c
    ${LUA_SOURCE_DIR}/lstate.c ${LUA_SOURCE_DIR}/lstring.c
    ${LUA_SOURCE_DIR}/lstrlib.c ${LUA_SOURCE_DIR}/ltable.c
    ${LUA_SOURCE_DIR}/ltablib.c ${LUA_SOURCE_DIR}/ltm.c
    ${LUA_SOURCE_DIR}/lundump.c ${LUA_SOURCE_DIR}/lutf8lib.c
    ${LUA_SOURCE_DIR}/lvm.c ${LUA_SOURCE_DIR}/lzio.c)
target_include_directories(zsharp_lua PUBLIC ${LUA_SOURCE_DIR})
set_target_properties(zsharp_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(UNIX)
    target_compile_definitions(zsharp_lua PRIVATE LUA_USE_POSIX LUA_USE_DLOPEN)
    target_link_libraries(zsharp_lua PRIVATE ${CMAKE_DL_LIBS} m)
endif()

add_executable(zsharp
    native/src/c_syntax.c
    native/src/interop_process.c
    native/src/interop_embedded.c
    native/src/interop_script.c
    native/src/interop_script_lua.c
    native/src/achievement.c
    native/src/desktop.c
    native/src/desktop_api.c
    native/src/audio_api.c
    native/src/speech_api.c
    native/src/speech_input.c
    native/src/hub.c
    native/src/game_runtime.c
    native/src/game_animation.c
    native/src/game_navigation.c
    native/src/game_model.c
    native/src/game_instances.c
    native/src/game_projection.c
    native/src/game_vulkan.c
    native/src/main.c
    native/src/package.c
    native/src/package_ignore.c
    native/src/paint.c
    native/src/style_transition.c
    native/src/project.c
    native/src/provider_loader.c
    native/src/publisher.c
    native/src/registry.c
    native/src/settings.c
    native/src/terminal.c
    native/src/updater.c
    native/src/vm.c
    native/src/window.c
    native/src/window_runtime.c
    native/src/window_style.c
    native/src/window_layout.c
    native/src/window_linux.c
    native/src/window_macos.c
)

if(ANDROID)
    # Android packages a shared library, not the desktop CLI executable.
    # Keep this target separate until the desktop and Android launchers share
    # a common runtime-library target.
    add_library(zsharp_android_vm SHARED
        native/src/c_syntax.c
        native/src/interop_process.c
        native/src/interop_embedded.c
        native/src/interop_script.c
        native/src/interop_script_lua.c
        native/src/android_vm_bridge.c
        native/src/achievement.c
        native/src/desktop.c
        native/src/desktop_api.c
        native/src/audio_api.c
        native/src/speech_api.c
        native/src/speech_input.c
        native/src/hub.c
        native/src/game_runtime.c
        native/src/game_animation.c
        native/src/game_navigation.c
        native/src/game_model.c
        native/src/game_instances.c
        native/src/game_projection.c
        native/src/game_vulkan.c
        native/src/package.c
        native/src/package_ignore.c
        native/src/paint.c
        native/src/style_transition.c
        native/src/project.c
        native/src/provider_loader.c
        native/src/publisher.c
        native/src/registry.c
        native/src/settings.c
        native/src/terminal.c
        native/src/updater.c
        native/src/vm.c
        native/src/window.c
        native/src/window_runtime.c
        native/src/window_style.c
        native/src/window_layout.c
        native/src/window_linux.c
        native/src/window_macos.c
    )
    target_include_directories(zsharp_android_vm PRIVATE native/include native/src)
    target_compile_features(zsharp_android_vm PRIVATE c_std_17)
    set_target_properties(zsharp_android_vm PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(zsharp_android_vm PRIVATE
        zsharp_language_core qjs zsharp_lua zsharp_ufbx Threads::Threads ${CMAKE_DL_LIBS} m log)
endif()

add_custom_target(zsharp_python_helpers
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:zsharp>/python"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/native/python/zsharp_bridge.py"
            "$<TARGET_FILE_DIR:zsharp>/python/zsharp_bridge.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/native/python/zsharp_network.py"
            "$<TARGET_FILE_DIR:zsharp>/python/zsharp_network.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/native/python/zsharp_voice.py"
            "$<TARGET_FILE_DIR:zsharp>/python/zsharp_voice.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/native/python/zsharp_speech_input.py"
            "$<TARGET_FILE_DIR:zsharp>/python/zsharp_speech_input.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/install-speech-components.py"
            "$<TARGET_FILE_DIR:zsharp>/python/install-speech-components.py"
    VERBATIM)
add_dependencies(zsharp zsharp_python_helpers)

# Managed helpers are portable assets. Toolchains are optional for development
# builds, while release CI installs both so every desktop archive ships them.
find_package(Java 17 COMPONENTS Development QUIET)
find_program(ZSHARP_DOTNET_EXECUTABLE NAMES dotnet)
set(ZSHARP_DOTNET_SDK_AVAILABLE FALSE)
if(ZSHARP_DOTNET_EXECUTABLE)
    execute_process(COMMAND "${ZSHARP_DOTNET_EXECUTABLE}" --list-sdks
        OUTPUT_VARIABLE ZSHARP_DOTNET_SDKS RESULT_VARIABLE ZSHARP_DOTNET_SDK_RESULT
        ERROR_QUIET)
    if(ZSHARP_DOTNET_SDK_RESULT EQUAL 0 AND
       ZSHARP_DOTNET_SDKS MATCHES "(^|\n)(8|9|[1-9][0-9]+)\\.")
        set(ZSHARP_DOTNET_SDK_AVAILABLE TRUE)
    endif()
endif()
find_program(ZSHARP_GO_EXECUTABLE NAMES go)
find_program(ZSHARP_RUBY_EXECUTABLE NAMES ruby)
set(ZSHARP_INTEROP_BUILD "${CMAKE_CURRENT_BINARY_DIR}/interop-build")
set(ZSHARP_INTEROP_DEPENDENCIES)
if(Java_FOUND AND NOT ANDROID)
    file(GLOB ZSHARP_JAVA_INTEROP_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/native/interop/java/src/ZSharp/*.java")
    add_custom_command(OUTPUT "${ZSHARP_INTEROP_BUILD}/java/zsharp-java-worker.jar"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ZSHARP_INTEROP_BUILD}/java/classes"
        COMMAND "${Java_JAVAC_EXECUTABLE}" --release 17 -encoding UTF-8
            -d "${ZSHARP_INTEROP_BUILD}/java/classes" ${ZSHARP_JAVA_INTEROP_SOURCES}
        COMMAND "${Java_JAR_EXECUTABLE}" --create
            --file "${ZSHARP_INTEROP_BUILD}/java/zsharp-java-worker.jar"
            -C "${ZSHARP_INTEROP_BUILD}/java/classes" .
        DEPENDS ${ZSHARP_JAVA_INTEROP_SOURCES}
        VERBATIM)
    add_custom_target(zsharp_java_interop_worker
        DEPENDS "${ZSHARP_INTEROP_BUILD}/java/zsharp-java-worker.jar")
    list(APPEND ZSHARP_INTEROP_DEPENDENCIES zsharp_java_interop_worker)
else()
    message(STATUS "Java 17+ development tools not found: shipping Java SDK sources without a rebuilt worker JAR")
endif()
if(ZSHARP_DOTNET_SDK_AVAILABLE AND NOT ANDROID)
    file(GLOB ZSHARP_CSHARP_INTEROP_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/native/interop/csharp/*.cs"
        "${CMAKE_CURRENT_SOURCE_DIR}/native/interop/csharp/*.csproj")
    add_custom_command(OUTPUT "${ZSHARP_INTEROP_BUILD}/csharp/ZSharp.Interop.Worker.dll"
        COMMAND "${ZSHARP_DOTNET_EXECUTABLE}" build
            "${CMAKE_CURRENT_SOURCE_DIR}/native/interop/csharp/ZSharp.Interop.Worker.csproj"
            --configuration Release --nologo
            --artifacts-path "${ZSHARP_INTEROP_BUILD}/dotnet"
            --output "${ZSHARP_INTEROP_BUILD}/csharp"
        DEPENDS ${ZSHARP_CSHARP_INTEROP_SOURCES}
        VERBATIM)
    add_custom_target(zsharp_csharp_interop_worker
        DEPENDS "${ZSHARP_INTEROP_BUILD}/csharp/ZSharp.Interop.Worker.dll")
    list(APPEND ZSHARP_INTEROP_DEPENDENCIES zsharp_csharp_interop_worker)
else()
    message(STATUS ".NET SDK not found: shipping C# SDK sources without a rebuilt managed worker")
endif()
add_custom_target(zsharp_interop_helpers ALL
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/native/interop"
        "-DBUILD=${ZSHARP_INTEROP_BUILD}"
        "-DDESTINATION=$<TARGET_FILE_DIR:zsharp>/interop"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/stage-interop.cmake"
    DEPENDS ${ZSHARP_INTEROP_DEPENDENCIES}
    VERBATIM)
add_dependencies(zsharp zsharp_interop_helpers)

if(MSVC)
    get_filename_component(ZSHARP_MSVC_TOOL_DIRECTORY "${CMAKE_LINKER}" DIRECTORY)
    find_program(ZSHARP_DUMPBIN_EXECUTABLE NAMES dumpbin
        HINTS "${ZSHARP_MSVC_TOOL_DIRECTORY}" REQUIRED)
    # Also runs in release CI, where BUILD_TESTING is off. A developer machine
    # already has the redistributable and cannot otherwise expose this omission.
    add_custom_command(TARGET zsharp POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DRUNTIME=$<TARGET_FILE:zsharp>"
            "-DDUMPBIN=${ZSHARP_DUMPBIN_EXECUTABLE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows-runtime-dependencies.cmake"
        VERBATIM)
endif()

add_executable(zsharp_installer
    installer/main.c
    native/src/hash.c
)
set_target_properties(zsharp_installer PROPERTIES OUTPUT_NAME "zsharp-installer")
if(WIN32)
    add_executable(zsharp_setup WIN32
        installer/main.c
        native/src/hash.c
    )
    target_compile_definitions(zsharp_setup PRIVATE ZSHARP_INSTALLER_GUI=1)
    target_include_directories(zsharp_setup PRIVATE native/src)
    target_compile_features(zsharp_setup PRIVATE c_std_17)
    target_link_libraries(zsharp_setup PRIVATE
        winhttp advapi32 user32 gdi32 shell32 comctl32)
    set_target_properties(zsharp_setup PROPERTIES OUTPUT_NAME "zsharp-setup")
endif()

target_include_directories(zsharp PRIVATE native/include)
target_include_directories(zsharp_installer PRIVATE native/src)
target_compile_features(zsharp PRIVATE c_std_17)
target_compile_features(zsharp_installer PRIVATE c_std_17)
target_link_libraries(zsharp PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
target_link_libraries(zsharp PRIVATE zsharp_language_core)
target_link_libraries(zsharp PRIVATE qjs)
target_link_libraries(zsharp PRIVATE zsharp_lua)
target_link_libraries(zsharp PRIVATE zsharp_ufbx)

if(ZSHARP_BUILD_GAME_RUNTIME)
    include(FetchContent)

    if(ANDROID)
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    else()
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
    endif()
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.10/SDL3-3.4.10.tar.gz
        URL_HASH SHA256=12b34280415ec8418c864408b93d008a20a6530687ee613d60bfbd20411f2785
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_Declare(
        VulkanHeaders
        URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.350.tar.gz
        URL_HASH SHA256=6dd105e5cc7ddab6e7b611ae2c1872740d1727557cc8bf9daf13d6de1e4b3999
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(SDL3 VulkanHeaders)
    if(WIN32)
        enable_language(CXX)
        set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
        set(BUILD_LOADER ON CACHE BOOL "" FORCE)
        set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(OpenXRSDK
            URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.53.tar.gz
            URL_HASH SHA256=3ccd6835e213e3226e828660b80b8e2178f1571ba743f16088c942ebce84f6ef
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE EXCLUDE_FROM_ALL)
        FetchContent_MakeAvailable(OpenXRSDK)
        if(MSVC)
            # The SDK overrides the parent CRT selection for its static loader.
            # Match the VM and the other static libraries to avoid /MD mixing.
            set_property(TARGET openxr_loader PROPERTY MSVC_RUNTIME_LIBRARY
                "${CMAKE_MSVC_RUNTIME_LIBRARY}")
        endif()
        target_sources(zsharp PRIVATE native/src/game_xr.cpp)
        target_compile_features(zsharp PRIVATE cxx_std_17)
        target_compile_definitions(zsharp PRIVATE ZSHARP_HAS_OPENXR=1)
        target_link_libraries(zsharp PRIVATE OpenXR::openxr_loader d3d11 dxgi d3dcompiler)
        install(FILES "${openxrsdk_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME OPENXR-LICENSE.txt)
    endif()
    target_compile_definitions(zsharp PRIVATE ZSHARP_HAS_GAME_RUNTIME=1)
    if(ANDROID)
        target_link_libraries(zsharp PRIVATE SDL3::SDL3-shared Vulkan::Headers)
    else()
        target_link_libraries(zsharp PRIVATE SDL3::SDL3-static Vulkan::Headers)
    endif()
    if(ANDROID)
        target_compile_definitions(zsharp_android_vm PRIVATE ZSHARP_HAS_GAME_RUNTIME=1)
        target_link_libraries(zsharp_android_vm PRIVATE SDL3::SDL3-shared Vulkan::Headers)
    endif()

    if(APPLE AND ZSHARP_BUNDLE_MOLTENVK)
        FetchContent_Declare(
            MoltenVKPackage
            URL https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-macos.tar
            URL_HASH SHA256=f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(MoltenVKPackage)
        set(ZSHARP_MOLTENVK_DYLIB
            "${moltenvkpackage_SOURCE_DIR}/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib")
        if(NOT EXISTS "${ZSHARP_MOLTENVK_DYLIB}")
            message(FATAL_ERROR
                "The pinned MoltenVK package did not contain the expected runtime: "
                "${ZSHARP_MOLTENVK_DYLIB}")
        endif()
        target_compile_definitions(
            zsharp PRIVATE
            ZSHARP_BUNDLED_MOLTENVK_NAME="libMoltenVK.dylib")
        add_custom_command(
            TARGET zsharp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ZSHARP_MOLTENVK_DYLIB}"
                    "$<TARGET_FILE_DIR:zsharp>/libMoltenVK.dylib"
            VERBATIM)
        install(FILES "${ZSHARP_MOLTENVK_DYLIB}" DESTINATION bin)
    endif()
endif()

if(NOT WIN32)
    target_link_libraries(zsharp PRIVATE m)
endif()

if(APPLE)
    # The command-line ZVM also needs a usage description for macOS TCC; it is
    # not necessarily launched from an .app bundle with an external Info.plist.
    configure_file(native/macos/Info.plist.in
        "${CMAKE_CURRENT_BINARY_DIR}/zsharp-runtime-Info.plist" @ONLY)
    target_link_options(zsharp PRIVATE
        "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_BINARY_DIR}/zsharp-runtime-Info.plist")
endif()

if(WIN32)
    target_link_libraries(zsharp PRIVATE user32 gdi32 msimg32 comdlg32 ole32 windowscodecs
                                            shell32 advapi32 uuid winhttp sapi oleaut32)
    target_link_libraries(zsharp_installer PRIVATE winhttp advapi32 user32 shell32)
endif()

if(MSVC)
    target_compile_options(zsharp PRIVATE /W4 /permissive-)
    target_compile_options(zsharp_installer PRIVATE /W4 /permissive-)
else()
    target_compile_options(zsharp PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(zsharp_installer PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS zsharp zsharp_installer RUNTIME DESTINATION bin)
if(WIN32)
    install(TARGETS zsharp_setup RUNTIME DESTINATION bin)
endif()
install(FILES native/python/zsharp_bridge.py DESTINATION bin/python)
install(FILES native/python/zsharp_network.py DESTINATION bin/python)
install(FILES native/python/zsharp_voice.py native/python/zsharp_speech_input.py
        DESTINATION bin/python)
install(FILES tools/install-speech-components.py DESTINATION bin/python)
install(DIRECTORY "$<TARGET_FILE_DIR:zsharp>/interop/" DESTINATION bin/interop
        OPTIONAL)
install(FILES native/rust/zsharp_ffi.rs DESTINATION share/zsharp/rust)
install(FILES
    native/include/zsharp.h
    native/include/zsharp_cpp.h
    native/include/zsharp_c.h
    native/include/zsharp_rust.h
    native/include/zsharp_provider.h
    native/include/zsharp_interop.h
    native/include/zsharp_kotlin.h
    DESTINATION include
)
install(FILES
    LICENSE
    README.md
    LANGUAGE.md
    SYNTAX.md
    BYTECODE.md
    CHANGELOG.md
    PROVIDERS.md
    UNINSTALL.md
    INSTALLING.md
    DESTINATION share/doc/zsharp
)

if(ZSHARP_BUILD_EXAMPLES)
    add_library(zsharp_mock_provider SHARED
        examples/providers/mock_provider.c
    )
    target_include_directories(zsharp_mock_provider PRIVATE native/include)
    target_compile_features(zsharp_mock_provider PRIVATE c_std_17)
endif()

set(CPACK_PACKAGE_NAME "zsharp")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "zsharp-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_GENERATOR "ZIP")
include(CPack)

if(BUILD_TESTING)
    add_executable(zsharp_test_package_executable tests/package-executable-tests.c)
    target_include_directories(zsharp_test_package_executable PRIVATE native/src)
    target_compile_features(zsharp_test_package_executable PRIVATE c_std_17)
    add_test(NAME zsharp_package_executable COMMAND zsharp_test_package_executable)
    enable_language(CXX)
    add_executable(zsharp_test_interop_process tests/interop-process-tests.c
        native/src/interop_process.c)
    target_include_directories(zsharp_test_interop_process PRIVATE native/src)
    target_compile_features(zsharp_test_interop_process PRIVATE c_std_17)
    add_test(NAME zsharp_interop_process COMMAND zsharp_test_interop_process
        "${CMAKE_CURRENT_BINARY_DIR}/owned interop process fixture")
    set_tests_properties(zsharp_interop_process PROPERTIES TIMEOUT 25)
    add_library(zsharp_test_interop_c_v2 SHARED tests/native-interop-v2/fixture.c)
    add_library(zsharp_test_interop_cpp_v2 SHARED tests/native-interop-v2/fixture.cpp)
    target_include_directories(zsharp_test_interop_c_v2 PRIVATE native/include)
    target_include_directories(zsharp_test_interop_cpp_v2 PRIVATE native/include)
    target_compile_features(zsharp_test_interop_c_v2 PRIVATE c_std_17)
    target_compile_features(zsharp_test_interop_cpp_v2 PRIVATE cxx_std_17)
    add_executable(zsharp_test_interop_native_abi tests/native-interop-v2/abi-tests.c)
    target_include_directories(zsharp_test_interop_native_abi PRIVATE native/include)
    target_compile_features(zsharp_test_interop_native_abi PRIVATE c_std_17)
    target_link_libraries(zsharp_test_interop_native_abi PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME zsharp_interop_native_abi COMMAND zsharp_test_interop_native_abi
        "$<TARGET_FILE:zsharp_test_interop_c_v2>" "$<TARGET_FILE:zsharp_test_interop_cpp_v2>")
    add_executable(zsharp_test_interop_native_backend tests/native-interop-v2/backend-tests.c
        native/src/interop_embedded.c)
    target_include_directories(zsharp_test_interop_native_backend PRIVATE native/include native/src)
    target_compile_features(zsharp_test_interop_native_backend PRIVATE c_std_17)
    target_link_libraries(zsharp_test_interop_native_backend PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME zsharp_interop_native_backend COMMAND zsharp_test_interop_native_backend
        "$<TARGET_FILE:zsharp_test_interop_c_v2>" "$<TARGET_FILE:zsharp_test_c_module>")
    add_executable(zsharp_test_foreign_imports tests/foreign-import-tests.c)
    target_link_libraries(zsharp_test_foreign_imports PRIVATE zsharp_language_core)
    add_test(NAME zsharp_foreign_imports COMMAND zsharp_test_foreign_imports
        "${CMAKE_CURRENT_BINARY_DIR}/foreign-import-tests.zbc")
    add_executable(zsharp_test_interop_script tests/interop-managed/script-tests.c
        native/src/interop_script.c native/src/interop_script_lua.c)
    target_include_directories(zsharp_test_interop_script PRIVATE native/src)
    target_compile_features(zsharp_test_interop_script PRIVATE c_std_17)
    target_link_libraries(zsharp_test_interop_script PRIVATE qjs zsharp_lua Threads::Threads)
    add_test(NAME zsharp_interop_javascript COMMAND zsharp_test_interop_script
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop-managed/ScriptSample.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop-managed/ScriptLegacy.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop-managed/ScriptDefault.js")
    set_tests_properties(zsharp_interop_javascript PROPERTIES TIMEOUT 20)
    add_executable(zsharp_test_interop_lua tests/interop-managed/lua-tests.c
        native/src/interop_script_lua.c)
    target_include_directories(zsharp_test_interop_lua PRIVATE native/src)
    target_compile_features(zsharp_test_interop_lua PRIVATE c_std_17)
    target_link_libraries(zsharp_test_interop_lua PRIVATE qjs zsharp_lua Threads::Threads)
    add_test(NAME zsharp_interop_lua COMMAND zsharp_test_interop_lua
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop-managed/LuaSample.lua")
    set_tests_properties(zsharp_interop_lua PROPERTIES TIMEOUT 20)
    add_executable(zsharp_test_foreign_object_syntax tests/foreign-object-syntax-tests.c)
    target_link_libraries(zsharp_test_foreign_object_syntax PRIVATE zsharp_language_core)
    add_test(NAME zsharp_foreign_object_syntax COMMAND zsharp_test_foreign_object_syntax
        "${CMAKE_CURRENT_BINARY_DIR}/foreign-object-syntax-tests.zbc")
    if(ZSHARP_GO_EXECUTABLE)
        add_test(NAME zsharp_interop_go COMMAND ${CMAKE_COMMAND} -E env GOWORK=off
            "${ZSHARP_GO_EXECUTABLE}" test ./...)
        set_tests_properties(zsharp_interop_go PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/native/interop/go" TIMEOUT 90)
    endif()
    if(ZSHARP_RUBY_EXECUTABLE)
        add_test(NAME zsharp_interop_ruby COMMAND "${ZSHARP_RUBY_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/native/interop/ruby/zsharp_worker_test.rb")
        set_tests_properties(zsharp_interop_ruby PROPERTIES TIMEOUT 30)
    endif()
    if(WIN32 AND TARGET openxr_loader)
        add_executable(zsharp_test_xr_d3d11 tests/xr-d3d11-tests.cpp)
        target_include_directories(zsharp_test_xr_d3d11 PRIVATE native/src)
        target_link_libraries(zsharp_test_xr_d3d11 PRIVATE d3d11 d3dcompiler)
        add_test(NAME zsharp_xr_d3d11 COMMAND zsharp_test_xr_d3d11)
        # MR/passthrough is deferred; retain the experiment but do not build it.
        # add_executable(zsharp_test_xr_passthrough tests/xr-passthrough-tests.cpp)
        # target_include_directories(zsharp_test_xr_passthrough PRIVATE native/src)
        # target_compile_features(zsharp_test_xr_passthrough PRIVATE cxx_std_17)
        # target_link_libraries(zsharp_test_xr_passthrough PRIVATE openxr_loader)
        # add_test(NAME zsharp_xr_passthrough COMMAND zsharp_test_xr_passthrough)
        add_executable(zsharp_test_xr_hand_backend tests/xr-hand-backend-tests.cpp)
        target_include_directories(zsharp_test_xr_hand_backend PRIVATE native/src)
        target_compile_features(zsharp_test_xr_hand_backend PRIVATE cxx_std_17)
        target_link_libraries(zsharp_test_xr_hand_backend PRIVATE openxr_loader zsharp_language_core)
        add_test(NAME zsharp_xr_hand_backend COMMAND zsharp_test_xr_hand_backend)
    endif()
    add_executable(zsharp_test_xr tests/xr-tests.c)
    target_link_libraries(zsharp_test_xr PRIVATE zsharp_language_core)
    if(NOT WIN32)
        target_link_libraries(zsharp_test_xr PRIVATE m)
    endif()
    add_test(NAME zsharp_xr COMMAND zsharp_test_xr)
    add_executable(zsharp_test_gamepad tests/gamepad-tests.c)
    target_link_libraries(zsharp_test_gamepad PRIVATE zsharp_language_core)
    if(NOT WIN32)
        target_link_libraries(zsharp_test_gamepad PRIVATE m)
    endif()
    add_test(NAME zsharp_gamepad COMMAND zsharp_test_gamepad)
    add_executable(zsharp_test_xr_hands tests/xr-hand-tests.c)
    target_link_libraries(zsharp_test_xr_hands PRIVATE zsharp_language_core)
    if(NOT WIN32)
        target_link_libraries(zsharp_test_xr_hands PRIVATE m)
    endif()
    add_test(NAME zsharp_xr_hands COMMAND zsharp_test_xr_hands)
    add_executable(zsharp_test_xr_ui tests/xr-ui-tests.c)
    target_link_libraries(zsharp_test_xr_ui PRIVATE zsharp_language_core)
    if(NOT WIN32)
        target_link_libraries(zsharp_test_xr_ui PRIVATE m)
    endif()
    add_test(NAME zsharp_xr_ui COMMAND zsharp_test_xr_ui)
    enable_language(CXX)
    add_executable(zsharp_test_xr_ui_world tests/xr-ui-world-tests.cpp)
    target_compile_features(zsharp_test_xr_ui_world PRIVATE cxx_std_17)
    target_link_libraries(zsharp_test_xr_ui_world PRIVATE zsharp_language_core)
    add_test(NAME zsharp_xr_ui_world COMMAND zsharp_test_xr_ui_world)
    set(ZSHARP_C_TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/c-project")
    file(REMOVE_RECURSE "${ZSHARP_C_TEST_DIR}")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/tests/c_project/"
         DESTINATION "${ZSHARP_C_TEST_DIR}")
    add_library(zsharp_test_c_module SHARED
        tests/c_project/C/Utilities.c)
    target_include_directories(zsharp_test_c_module PRIVATE native/include)
    target_compile_features(zsharp_test_c_module PRIVATE c_std_17)
    set_target_properties(zsharp_test_c_module PROPERTIES
        PREFIX ""
        OUTPUT_NAME "Utilities.zc"
        RUNTIME_OUTPUT_DIRECTORY "${ZSHARP_C_TEST_DIR}/C"
        LIBRARY_OUTPUT_DIRECTORY "${ZSHARP_C_TEST_DIR}/C"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${ZSHARP_C_TEST_DIR}/C"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${ZSHARP_C_TEST_DIR}/C"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${ZSHARP_C_TEST_DIR}/C"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${ZSHARP_C_TEST_DIR}/C")
    set(ZSHARP_C_CONFLICT_DIR "${CMAKE_CURRENT_BINARY_DIR}/c-syntax-conflict")
    file(REMOVE_RECURSE "${ZSHARP_C_CONFLICT_DIR}")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/tests/c_syntax_conflict/"
         DESTINATION "${ZSHARP_C_CONFLICT_DIR}")
    add_library(zsharp_test_c_conflict SHARED
        tests/c_syntax_conflict/C/Bad.c)
    target_include_directories(zsharp_test_c_conflict PRIVATE native/include)
    target_compile_features(zsharp_test_c_conflict PRIVATE c_std_17)
    set_target_properties(zsharp_test_c_conflict PROPERTIES
        PREFIX ""
        OUTPUT_NAME "Bad.zc"
        RUNTIME_OUTPUT_DIRECTORY "${ZSHARP_C_CONFLICT_DIR}/C"
        LIBRARY_OUTPUT_DIRECTORY "${ZSHARP_C_CONFLICT_DIR}/C"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${ZSHARP_C_CONFLICT_DIR}/C"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${ZSHARP_C_CONFLICT_DIR}/C"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${ZSHARP_C_CONFLICT_DIR}/C"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${ZSHARP_C_CONFLICT_DIR}/C")
    set(ZSHARP_CPP_TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/cpp-project")
    file(REMOVE_RECURSE "${ZSHARP_CPP_TEST_DIR}")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/tests/cpp_project/"
         DESTINATION "${ZSHARP_CPP_TEST_DIR}")
    add_library(zsharp_test_cpp_module SHARED
        tests/cpp_project/Cpp/Utilities.cpp)
    target_include_directories(zsharp_test_cpp_module PRIVATE native/include)
    target_compile_features(zsharp_test_cpp_module PRIVATE cxx_std_17)
    set_target_properties(zsharp_test_cpp_module PROPERTIES
        PREFIX ""
        OUTPUT_NAME "Utilities.zcpp"
        RUNTIME_OUTPUT_DIRECTORY "${ZSHARP_CPP_TEST_DIR}/Cpp"
        LIBRARY_OUTPUT_DIRECTORY "${ZSHARP_CPP_TEST_DIR}/Cpp"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${ZSHARP_CPP_TEST_DIR}/Cpp"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${ZSHARP_CPP_TEST_DIR}/Cpp"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${ZSHARP_CPP_TEST_DIR}/Cpp"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${ZSHARP_CPP_TEST_DIR}/Cpp")
    find_program(ZSHARP_RUSTC rustc)
    if(ZSHARP_RUSTC)
        set(ZSHARP_RUST_TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/rust-project")
        file(REMOVE_RECURSE "${ZSHARP_RUST_TEST_DIR}")
        file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/tests/rust_project/"
             DESTINATION "${ZSHARP_RUST_TEST_DIR}")
        file(COPY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/native/rust/zsharp_ffi.rs"
             "${ZSHARP_RUST_TEST_DIR}/Rust/zsharp_ffi.rs")
        if(WIN32)
            set(ZSHARP_RUST_TEST_SUFFIX dll)
        elseif(APPLE)
            set(ZSHARP_RUST_TEST_SUFFIX dylib)
        else()
            set(ZSHARP_RUST_TEST_SUFFIX so)
        endif()
        set(ZSHARP_RUST_TEST_LIBRARY
            "${ZSHARP_RUST_TEST_DIR}/Rust/Utilities.zrust.${ZSHARP_RUST_TEST_SUFFIX}")
        add_custom_command(OUTPUT "${ZSHARP_RUST_TEST_LIBRARY}"
            COMMAND "${ZSHARP_RUSTC}" --edition 2021 --crate-type cdylib
                    "${ZSHARP_RUST_TEST_DIR}/Rust/Utilities.rs"
                    -o "${ZSHARP_RUST_TEST_LIBRARY}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/rust_project/Rust/Utilities.rs"
                    "${CMAKE_CURRENT_SOURCE_DIR}/native/rust/zsharp_ffi.rs"
            VERBATIM)
        add_custom_target(zsharp_test_rust_module ALL
                          DEPENDS "${ZSHARP_RUST_TEST_LIBRARY}")
    endif()
    add_executable(zsharp_test_zip tests/create-stored-zip.c)
    add_executable(zsharp_test_time tests/time-tests.c)
    target_link_libraries(zsharp_test_time PRIVATE zsharp_language_core)
    add_test(NAME zsharp_time COMMAND zsharp_test_time)
    add_executable(zsharp_test_application_quit tests/application-quit-tests.c)
    target_link_libraries(zsharp_test_application_quit PRIVATE zsharp_language_core)
    add_test(NAME zsharp_application_quit
        COMMAND zsharp_test_application_quit
            "${CMAKE_CURRENT_BINARY_DIR}/application-quit-roundtrip.zbc")
    add_executable(zsharp_test_member_visibility_status
        tests/member-visibility-status-tests.c)
    target_link_libraries(zsharp_test_member_visibility_status
        PRIVATE zsharp_language_core)
    add_test(NAME zsharp_member_visibility_status
        COMMAND zsharp_test_member_visibility_status
            "${CMAKE_CURRENT_BINARY_DIR}/member-visibility-status-roundtrip.zbc")
    add_executable(zsharp_test_system_api tests/system-api-tests.c)
    target_link_libraries(zsharp_test_system_api PRIVATE zsharp_language_core)
    add_test(NAME zsharp_system_api COMMAND zsharp_test_system_api
        "${CMAKE_CURRENT_BINARY_DIR}/system-api-roundtrip.zbc")
    add_executable(zsharp_test_audio_api tests/audio-api-tests.c native/src/audio_api.c)
    target_include_directories(zsharp_test_audio_api PRIVATE native/src)
    target_compile_features(zsharp_test_audio_api PRIVATE c_std_17)
    if(NOT WIN32)
        target_link_libraries(zsharp_test_audio_api PRIVATE m)
    endif()
    add_test(NAME zsharp_audio_api COMMAND zsharp_test_audio_api
        "${CMAKE_CURRENT_BINARY_DIR}/audio-api-fixture.wav")
    if(TARGET SDL3::SDL3-static)
        add_executable(zsharp_test_audio_stream tests/audio-stream-tests.c native/src/audio_api.c)
        target_include_directories(zsharp_test_audio_stream PRIVATE native/src)
        target_compile_features(zsharp_test_audio_stream PRIVATE c_std_17)
        target_compile_definitions(zsharp_test_audio_stream PRIVATE ZSHARP_HAS_SDL_AUDIO=1)
        target_link_libraries(zsharp_test_audio_stream PRIVATE SDL3::SDL3-static)
        if(NOT WIN32)
            target_link_libraries(zsharp_test_audio_stream PRIVATE m)
        endif()
        add_test(NAME zsharp_audio_stream COMMAND zsharp_test_audio_stream)
        set_tests_properties(zsharp_audio_stream PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy" TIMEOUT 30)
    endif()
    add_executable(zsharp_test_desktop_api tests/desktop-api-tests.c native/src/desktop_api.c)
    target_include_directories(zsharp_test_desktop_api PRIVATE native/src)
    target_compile_features(zsharp_test_desktop_api PRIVATE c_std_17)
    if(WIN32)
        target_link_libraries(zsharp_test_desktop_api PRIVATE user32 advapi32)
    else()
        target_link_libraries(zsharp_test_desktop_api PRIVATE m ${CMAKE_DL_LIBS} Threads::Threads)
    endif()
    add_test(NAME zsharp_desktop_api COMMAND zsharp_test_desktop_api
        "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(zsharp_test_desktop_input tests/desktop-input-tests.c)
    target_include_directories(zsharp_test_desktop_input PRIVATE native/src)
    target_compile_features(zsharp_test_desktop_input PRIVATE c_std_17)
    add_test(NAME zsharp_desktop_input COMMAND zsharp_test_desktop_input)
    if(WIN32)
        add_executable(zsharp_test_desktop_close tests/desktop-close-tests.c
            native/src/desktop_api.c)
        target_include_directories(zsharp_test_desktop_close PRIVATE native/src)
        target_compile_features(zsharp_test_desktop_close PRIVATE c_std_17)
        target_link_libraries(zsharp_test_desktop_close PRIVATE user32 advapi32)
        add_test(NAME zsharp_desktop_close COMMAND zsharp_test_desktop_close)
        set_tests_properties(zsharp_desktop_close PROPERTIES TIMEOUT 60)
        add_executable(zsharp_test_desktop_input_windows
            tests/desktop-input-windows-tests.c)
        target_include_directories(zsharp_test_desktop_input_windows PRIVATE native/src)
        target_compile_features(zsharp_test_desktop_input_windows PRIVATE c_std_17)
        target_link_libraries(zsharp_test_desktop_input_windows PRIVATE user32 advapi32)
        add_test(NAME zsharp_desktop_input_windows COMMAND zsharp_test_desktop_input_windows)
        set_tests_properties(zsharp_desktop_input_windows PROPERTIES TIMEOUT 30)
        # Real message transport into hidden, process-owned edit controls only:
        # this regression never activates a window or sends keyboard/mouse input.
        add_executable(zsharp_test_desktop_input_ack
            tests/desktop-input-ack-tests.c)
        target_include_directories(zsharp_test_desktop_input_ack PRIVATE native/src)
        target_compile_features(zsharp_test_desktop_input_ack PRIVATE c_std_17)
        target_link_libraries(zsharp_test_desktop_input_ack PRIVATE user32 advapi32)
        add_test(NAME zsharp_desktop_input_ack COMMAND zsharp_test_desktop_input_ack)
        set_tests_properties(zsharp_desktop_input_ack PROPERTIES TIMEOUT 30)
        # Manual only: waits for its Run button, then types into its own visible
        # receiver. Never register foreground/input integration as unattended CTest.
        add_executable(zsharp_test_desktop_input_live WIN32 EXCLUDE_FROM_ALL
            tests/desktop-input-live-tests.c)
        target_include_directories(zsharp_test_desktop_input_live PRIVATE native/src)
        target_compile_features(zsharp_test_desktop_input_live PRIVATE c_std_17)
        target_link_libraries(zsharp_test_desktop_input_live PRIVATE user32 advapi32 gdi32)
        # Manual opt-in test against a separate, uniquely named Notepad fixture.
        # Never register real application input as an unattended CTest.
        add_executable(zsharp_test_desktop_input_notepad WIN32 EXCLUDE_FROM_ALL
            tests/desktop-input-notepad-tests.c)
        target_include_directories(zsharp_test_desktop_input_notepad PRIVATE native/src)
        target_compile_features(zsharp_test_desktop_input_notepad PRIVATE c_std_17)
        target_link_libraries(zsharp_test_desktop_input_notepad PRIVATE user32 advapi32 gdi32)
    endif()
    add_executable(zsharp_test_speech_api tests/speech-api-tests.c
        native/src/speech_api.c native/src/speech_input.c)
    target_include_directories(zsharp_test_speech_api PRIVATE native/src)
    target_compile_features(zsharp_test_speech_api PRIVATE c_std_17)
    target_link_libraries(zsharp_test_speech_api PRIVATE qjs Threads::Threads)
    if(WIN32)
        target_link_libraries(zsharp_test_speech_api PRIVATE sapi ole32 oleaut32 uuid)
    else()
        target_link_libraries(zsharp_test_speech_api PRIVATE m)
    endif()
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME zsharp_interop_python_worker
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/python-interop-worker-tests.py")
        add_executable(zsharp_test_speech_stream_api tests/speech-stream-api-tests.c
            native/src/speech_api.c native/src/speech_input.c)
        target_include_directories(zsharp_test_speech_stream_api PRIVATE native/src)
        target_compile_features(zsharp_test_speech_stream_api PRIVATE c_std_17)
        target_link_libraries(zsharp_test_speech_stream_api PRIVATE qjs Threads::Threads)
        if(WIN32)
            target_link_libraries(zsharp_test_speech_stream_api PRIVATE sapi ole32 oleaut32 uuid)
        else()
            target_link_libraries(zsharp_test_speech_stream_api PRIVATE m)
        endif()
        add_test(NAME zsharp_speech_stream_api COMMAND zsharp_test_speech_stream_api
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/speech-voice-stream.py")
        set_tests_properties(zsharp_speech_stream_api PROPERTIES TIMEOUT 45)
        add_executable(zsharp_test_speech_input_worker
            tests/speech-input-worker-tests.c native/src/speech_input.c)
        target_include_directories(zsharp_test_speech_input_worker PRIVATE native/src)
        target_compile_features(zsharp_test_speech_input_worker PRIVATE c_std_17)
        target_link_libraries(zsharp_test_speech_input_worker PRIVATE Threads::Threads)
        add_test(NAME zsharp_speech_input_worker
            COMMAND zsharp_test_speech_input_worker "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/speech-input-worker.py")
        add_test(NAME zsharp_speech_input_helper
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/speech-input-helper-tests.py")
        add_test(NAME zsharp_speech_api COMMAND zsharp_test_speech_api
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/native/python/zsharp_voice.py")
        add_test(NAME zsharp_speech_voice_helper
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/speech-voice-helper-tests.py")
        add_test(NAME zsharp_speech_voice_worker
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/speech-voice-worker-tests.py")
        add_test(NAME zsharp_speech_installer
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/speech-installer-tests.py")
    else()
        add_test(NAME zsharp_speech_api COMMAND zsharp_test_speech_api)
    endif()
    find_program(ZSHARP_NODE_TEST_EXECUTABLE NAMES node)
    if(ZSHARP_NODE_TEST_EXECUTABLE)
        if(Java_FOUND AND ZSHARP_DOTNET_SDK_AVAILABLE)
            add_test(NAME zsharp_interop_managed_workers
                COMMAND "${ZSHARP_NODE_TEST_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop-managed/run.mjs"
                    "${CMAKE_CURRENT_BINARY_DIR}/managed-worker-tests")
            set_tests_properties(zsharp_interop_managed_workers PROPERTIES TIMEOUT 150)
        endif()
        add_test(NAME zsharp_interop_installer
            COMMAND "${ZSHARP_NODE_TEST_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop-managed/installer-tests.mjs"
                "$<TARGET_FILE:zsharp_installer>")
        add_test(NAME zsharp_foreign_import_validation
            COMMAND "${ZSHARP_NODE_TEST_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/foreign-import-validation-tests.mjs"
                "$<TARGET_FILE:zsharp>")
        add_test(NAME zsharp_desktop_input_posix
            COMMAND "${ZSHARP_NODE_TEST_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/desktop-input-posix-tests.mjs")
        add_test(NAME zsharp_system_api_runtime
            COMMAND "${ZSHARP_NODE_TEST_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/system-api-runtime-tests.mjs"
                "$<TARGET_FILE:zsharp>" "${Python3_EXECUTABLE}")
        add_test(NAME zsharp_member_visibility_status_runtime
            COMMAND "${ZSHARP_NODE_TEST_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/member-visibility-status-runtime-tests.mjs"
                "$<TARGET_FILE:zsharp>")
        add_test(NAME zsharp_application_quit_runtime
            COMMAND "${ZSHARP_NODE_TEST_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/application-quit-runtime-tests.mjs"
                "$<TARGET_FILE:zsharp>")
    endif()
    add_executable(zsharp_test_game_projection
        tests/game-projection-tests.c
        native/src/game_projection.c
    )
    add_executable(zsharp_test_game_skinning tests/game-skinning-tests.c)
    add_executable(zsharp_test_game_collision tests/game-collision-tests.c)
    add_executable(zsharp_test_game_input tests/game-input-tests.c)
    target_include_directories(zsharp_test_game_input PRIVATE native/src)
    target_compile_features(zsharp_test_game_input PRIVATE c_std_17)
    add_test(NAME zsharp_game_input COMMAND zsharp_test_game_input)
    add_executable(zsharp_test_terminal_print tests/terminal-print-tests.c)
    target_compile_features(zsharp_test_terminal_print PRIVATE c_std_17)
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(zsharp_test_terminal_print PRIVATE Threads::Threads)
    endif()
    add_test(NAME zsharp_terminal_print COMMAND zsharp_test_terminal_print)
    add_test(NAME zsharp_print_update
        COMMAND "${CMAKE_COMMAND}" "-DBIN=$<TARGET_FILE:zsharp>"
        "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/print-update.zbc"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-print-update-tests.cmake")
    add_test(NAME zsharp_game_buttons
        COMMAND "${CMAKE_COMMAND}" "-DBIN=$<TARGET_FILE:zsharp>"
        "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUT=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-game-button-tests.cmake")
    add_executable(zsharp_test_zss_layout tests/zss-layout-tests.c native/src/paint.c native/src/window_layout.c native/src/style_transition.c)
    target_include_directories(zsharp_test_zss_layout PRIVATE native/src native/include)
    target_compile_features(zsharp_test_zss_layout PRIVATE c_std_17)
    if(NOT WIN32)
        target_link_libraries(zsharp_test_zss_layout PRIVATE m)
    endif()
    add_test(NAME zsharp_zss_layout COMMAND zsharp_test_zss_layout)
    add_test(NAME zsharp_zss_styles COMMAND "${CMAKE_COMMAND}" "-DBIN=$<TARGET_FILE:zsharp>"
        "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUT=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-zss-tests.cmake")
    target_include_directories(zsharp_test_game_collision PRIVATE
        native/include native/src "${ufbx_SOURCE_DIR}")
    target_compile_features(zsharp_test_game_collision PRIVATE c_std_17)
    if(MSVC)
        target_compile_options(zsharp_test_game_collision PRIVATE /Gy)
        target_link_options(zsharp_test_game_collision PRIVATE /OPT:REF)
    else()
        target_compile_options(zsharp_test_game_collision PRIVATE
            -ffunction-sections -fdata-sections)
        if(APPLE)
            target_link_options(zsharp_test_game_collision PRIVATE -Wl,-dead_strip)
        else()
            target_link_options(zsharp_test_game_collision PRIVATE -Wl,--gc-sections)
        endif()
        target_link_libraries(zsharp_test_game_collision PRIVATE m)
    endif()
    target_link_libraries(zsharp_test_game_skinning PRIVATE zsharp_ufbx)
    target_compile_features(zsharp_test_game_skinning PRIVATE c_std_17)
    if(NOT WIN32)
        target_link_libraries(zsharp_test_game_skinning PRIVATE m)
    endif()
    target_include_directories(zsharp_test_game_projection PRIVATE native/src)
    target_compile_features(zsharp_test_game_projection PRIVATE c_std_17)
    if(NOT WIN32)
        target_link_libraries(zsharp_test_game_projection PRIVATE m)
    endif()
    target_compile_features(zsharp_test_zip PRIVATE c_std_17)
    if(MSVC)
        target_compile_options(zsharp_test_zip PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zsharp_test_zip PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(
        NAME zsharp_syntax_regressions
        COMMAND ${CMAKE_COMMAND}
            "-DZSHARP_BIN=$<TARGET_FILE:zsharp>"
            "-DPROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DTEST_OUTPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/window/Main.zbc"
            "-DTEST_PACKAGE_PROJECT=${CMAKE_CURRENT_BINARY_DIR}/package-project"
            "-DTEST_GAME_PROJECT=${CMAKE_CURRENT_BINARY_DIR}/game-project"
            "-DTEST_3D_GAME_PROJECT=${CMAKE_CURRENT_BINARY_DIR}/game-3d-project"
            "-DTEST_PACKAGE=${CMAKE_CURRENT_BINARY_DIR}/package-project/Packages/PackageTest.zapp"
            "-DTEST_SOURCE_PACKAGE=${CMAKE_CURRENT_BINARY_DIR}/package-project/Packages/PackageTest-unbytecoded.zapp"
            "-DTEST_GAME_PACKAGE=${CMAKE_CURRENT_BINARY_DIR}/game-project/Packages/PackageTest.zgame"
            "-DTEST_GAME_SOURCE_PACKAGE=${CMAKE_CURRENT_BINARY_DIR}/game-project/Packages/PackageTest-unbytecoded.zgame"
            "-DTEST_PACKAGE_CACHE=${CMAKE_CURRENT_BINARY_DIR}/package-cache"
            "-DTEST_PROJECT_REGISTRY=${CMAKE_CURRENT_BINARY_DIR}/projects.registry"
            "-DTEST_SHORTCUT_CACHE=${CMAKE_CURRENT_BINARY_DIR}/shortcut-cache"
            "-DTEST_DESKTOP=${CMAKE_CURRENT_BINARY_DIR}/test-desktop"
            "-DTEST_CPP_PROJECT=${ZSHARP_CPP_TEST_DIR}"
            "-DTEST_C_PROJECT=${ZSHARP_C_TEST_DIR}"
            "-DTEST_C_CONFLICT_PROJECT=${ZSHARP_C_CONFLICT_DIR}"
            "-DTEST_RUST_PROJECT=${ZSHARP_RUST_TEST_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-syntax-tests.cmake"
    )
    add_test(NAME zsharp_game_projection
             COMMAND zsharp_test_game_projection)
    add_test(NAME zsharp_file_search COMMAND "${CMAKE_COMMAND}"
        "-DBIN=$<TARGET_FILE:zsharp>" "-DOUT=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-file-search-tests.cmake")
    add_test(NAME zsharp_game_skinning
             COMMAND zsharp_test_game_skinning
               "${ufbx_SOURCE_DIR}/data/max_transformed_skin_7500_ascii.fbx")
    add_test(NAME zsharp_game_collision
             COMMAND zsharp_test_game_collision)
    add_executable(zsharp_test_game_instances tests/game-instance-tests.c)
    target_include_directories(zsharp_test_game_instances PRIVATE native/include native/src)
    target_compile_features(zsharp_test_game_instances PRIVATE c_std_17)
    add_test(NAME zsharp_game_instances COMMAND zsharp_test_game_instances)
    add_test(NAME zsharp_game_instance_runtime COMMAND "${CMAKE_COMMAND}"
        "-DBIN=$<TARGET_FILE:zsharp>" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-game-instance-tests.cmake")
    add_test(NAME zsharp_callbacks COMMAND "${CMAKE_COMMAND}"
        "-DBIN=$<TARGET_FILE:zsharp>" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-callback-tests.cmake")
    add_test(NAME zsharp_c_declarations COMMAND "${CMAKE_COMMAND}"
        "-DBIN=$<TARGET_FILE:zsharp>" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-c-declaration-tests.cmake")
    add_test(NAME zsharp_package_validation_cache COMMAND "${CMAKE_COMMAND}"
        "-DBIN=$<TARGET_FILE:zsharp>" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-package-performance-tests.cmake")
    add_executable(zsharp_test_package_ignore tests/package-ignore-tests.c native/src/package_ignore.c)
    target_include_directories(zsharp_test_package_ignore PRIVATE native/src)
    add_test(NAME zsharp_package_ignore_match COMMAND zsharp_test_package_ignore)
    add_test(NAME zsharp_package_ignore COMMAND "${CMAKE_COMMAND}"
        "-DBIN=$<TARGET_FILE:zsharp>" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-package-ignore-tests.cmake")
    add_test(NAME zsharp_source_dependencies COMMAND "${CMAKE_COMMAND}"
        "-DBIN=$<TARGET_FILE:zsharp>" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-source-dependency-tests.cmake")
    add_executable(zsharp_test_vm_blocking_state tests/vm-blocking-state-tests.c)
    target_include_directories(zsharp_test_vm_blocking_state PRIVATE native/src)
    target_compile_features(zsharp_test_vm_blocking_state PRIVATE c_std_17)
    target_link_libraries(zsharp_test_vm_blocking_state PRIVATE Threads::Threads)
    add_test(NAME zsharp_vm_blocking_state COMMAND zsharp_test_vm_blocking_state)
    set_tests_properties(zsharp_vm_blocking_state PROPERTIES TIMEOUT 15)
    if(WIN32)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
            set(ZSHARP_TEST_PLATFORM "windows-aarch64")
        else()
            set(ZSHARP_TEST_PLATFORM "windows-x86_64")
        endif()
    elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
            set(ZSHARP_TEST_PLATFORM "macos-aarch64")
        else()
            set(ZSHARP_TEST_PLATFORM "macos-x86_64")
        endif()
    else()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
            set(ZSHARP_TEST_PLATFORM "linux-aarch64")
        else()
            set(ZSHARP_TEST_PLATFORM "linux-x86_64")
        endif()
    endif()
    add_test(
        NAME zsharp_installer_regressions
        COMMAND ${CMAKE_COMMAND}
            "-DINSTALLER_BIN=$<TARGET_FILE:zsharp_installer>"
            "-DRUNTIME_BIN=$<TARGET_FILE:zsharp>"
            "-DTEST_PLATFORM=${ZSHARP_TEST_PLATFORM}"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/installer-test"
            "-DZIP_TOOL=$<TARGET_FILE:zsharp_test_zip>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-installer-tests.cmake"
    )
    if(WIN32)
        add_test(
            NAME zsharp_setup_regressions
            COMMAND ${CMAKE_COMMAND}
                "-DINSTALLER_BIN=$<TARGET_FILE:zsharp_setup>"
                "-DRUNTIME_BIN=$<TARGET_FILE:zsharp>"
                "-DTEST_PLATFORM=${ZSHARP_TEST_PLATFORM}"
                "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/setup-installer-test"
                "-DZIP_TOOL=$<TARGET_FILE:zsharp_test_zip>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-installer-tests.cmake"
        )
    endif()
endif()
