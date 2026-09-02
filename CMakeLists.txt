cmake_minimum_required(VERSION 3.20)

project(zsharp VERSION 1.0.2.0 LANGUAGES C)

option(ZSHARP_BUILD_EXAMPLES "Build Z# integration examples" OFF)
option(ZSHARP_BUILD_GAME_RUNTIME
       "Build the SDL3 and Vulkan Z# game runtime" ON)
option(ZSHARP_BUNDLE_MOLTENVK
       "Bundle the official MoltenVK runtime in macOS builds" ON)

include(CTest)
find_package(Threads REQUIRED)

add_executable(zsharp
    native/src/bytecode.c
    native/src/decimal.c
    native/src/desktop.c
    native/src/hash.c
    native/src/game_runtime.c
    native/src/game_model.c
    native/src/game_vulkan.c
    native/src/lexer.c
    native/src/main.c
    native/src/package.c
    native/src/paint.c
    native/src/parser.c
    native/src/project.c
    native/src/provider_loader.c
    native/src/registry.c
    native/src/settings.c
    native/src/updater.c
    native/src/vm.c
    native/src/window.c
    native/src/window_runtime.c
    native/src/window_style.c
    native/src/window_linux.c
    native/src/window_macos.c
)

add_executable(zsharp_installer
    installer/main.c
    native/src/hash.c
)
set_target_properties(zsharp_installer PROPERTIES OUTPUT_NAME "zsharp-installer")

target_include_directories(zsharp PRIVATE native/include)
target_include_directories(zsharp_installer PRIVATE native/src)
target_compile_features(zsharp PRIVATE c_std_17)
target_compile_features(zsharp_installer PRIVATE c_std_17)
target_link_libraries(zsharp PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)

if(ZSHARP_BUILD_GAME_RUNTIME)
    include(FetchContent)

    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
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
    target_compile_definitions(zsharp PRIVATE ZSHARP_HAS_GAME_RUNTIME=1)
    target_link_libraries(zsharp PRIVATE SDL3::SDL3-static Vulkan::Headers)

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
            "${moltenvkpackage_SOURCE_DIR}/MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib")
        if(NOT EXISTS "${ZSHARP_MOLTENVK_DYLIB}")
            message(FATAL_ERROR
                "The pinned MoltenVK package did not contain libMoltenVK.dylib")
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

if(WIN32)
    target_link_libraries(zsharp PRIVATE user32 gdi32 comdlg32 ole32 windowscodecs
                                            shell32 advapi32 uuid winhttp)
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
install(FILES
    native/include/zsharp.h
    native/include/zsharp_provider.h
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
    add_executable(zsharp_test_zip tests/create-stored-zip.c)
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
            "-DTEST_PACKAGE=${CMAKE_CURRENT_BINARY_DIR}/package-project/Packages/PackageTest.zapp"
            "-DTEST_SOURCE_PACKAGE=${CMAKE_CURRENT_BINARY_DIR}/package-project/Packages/PackageTest-unbytecoded.zapp"
            "-DTEST_GAME_PACKAGE=${CMAKE_CURRENT_BINARY_DIR}/game-project/Packages/PackageTest.zgame"
            "-DTEST_GAME_SOURCE_PACKAGE=${CMAKE_CURRENT_BINARY_DIR}/game-project/Packages/PackageTest-unbytecoded.zgame"
            "-DTEST_PACKAGE_CACHE=${CMAKE_CURRENT_BINARY_DIR}/package-cache"
            "-DTEST_PROJECT_REGISTRY=${CMAKE_CURRENT_BINARY_DIR}/projects.registry"
            "-DTEST_SHORTCUT_CACHE=${CMAKE_CURRENT_BINARY_DIR}/shortcut-cache"
            "-DTEST_DESKTOP=${CMAKE_CURRENT_BINARY_DIR}/test-desktop"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-syntax-tests.cmake"
    )
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
endif()
