cmake_minimum_required(VERSION 3.20)

project(zsharp VERSION 1.0.0.1 LANGUAGES C)

option(ZSHARP_BUILD_EXAMPLES "Build Z# integration examples" OFF)

add_executable(zsharp
    native/src/bytecode.c
    native/src/decimal.c
    native/src/hash.c
    native/src/lexer.c
    native/src/main.c
    native/src/parser.c
    native/src/project.c
    native/src/provider_loader.c
    native/src/settings.c
    native/src/vm.c
)

target_include_directories(zsharp PRIVATE native/include)
target_compile_features(zsharp PRIVATE c_std_17)
target_link_libraries(zsharp PRIVATE ${CMAKE_DL_LIBS})

if(MSVC)
    target_compile_options(zsharp PRIVATE /W4 /permissive-)
else()
    target_compile_options(zsharp PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS zsharp RUNTIME DESTINATION bin)
install(FILES
    native/include/zsharp.h
    native/include/zsharp_provider.h
    DESTINATION include
)
install(FILES
    LICENSE
    README.md
    LANGUAGE.md
    BYTECODE.md
    CHANGELOG.md
    PROVIDERS.md
    PUBLISHING.md
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
