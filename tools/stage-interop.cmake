if(NOT DEFINED SOURCE OR NOT DEFINED BUILD OR NOT DEFINED DESTINATION)
    message(FATAL_ERROR "SOURCE, BUILD, and DESTINATION are required")
endif()
file(MAKE_DIRECTORY "${DESTINATION}")
file(COPY "${SOURCE}/" DESTINATION "${DESTINATION}"
    PATTERN "obj" EXCLUDE
    PATTERN "bin" EXCLUDE
    PATTERN "classes" EXCLUDE
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
    PATTERN "*_test.go" EXCLUDE
    PATTERN "*_test.rb" EXCLUDE)
get_filename_component(NATIVE_SOURCE "${SOURCE}" DIRECTORY)
file(MAKE_DIRECTORY "${DESTINATION}/include" "${DESTINATION}/rust" "${DESTINATION}/kotlin")
foreach(header IN ITEMS zsharp_interop.h zsharp_c.h zsharp_cpp.h zsharp_rust.h zsharp_kotlin.h)
    file(COPY "${NATIVE_SOURCE}/include/${header}" DESTINATION "${DESTINATION}/include")
endforeach()
file(COPY "${NATIVE_SOURCE}/rust/zsharp_ffi.rs" DESTINATION "${DESTINATION}/rust")
file(COPY "${NATIVE_SOURCE}/kotlin/zsharp_interop.def" "${NATIVE_SOURCE}/kotlin/README.md"
    DESTINATION "${DESTINATION}/kotlin")
if(EXISTS "${BUILD}/java/zsharp-java-worker.jar")
    file(MAKE_DIRECTORY "${DESTINATION}/java")
    file(COPY "${BUILD}/java/zsharp-java-worker.jar" DESTINATION "${DESTINATION}/java")
endif()
foreach(filename IN ITEMS ZSharp.Interop.Worker.dll ZSharp.Interop.Worker.deps.json
        ZSharp.Interop.Worker.runtimeconfig.json ZSharp.Interop.dll)
    if(EXISTS "${BUILD}/csharp/${filename}")
        file(MAKE_DIRECTORY "${DESTINATION}/csharp")
        file(COPY "${BUILD}/csharp/${filename}" DESTINATION "${DESTINATION}/csharp")
    endif()
endforeach()
