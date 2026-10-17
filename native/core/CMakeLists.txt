add_library(zsharp_language_core STATIC
    ../src/bytecode.c
    ../src/decimal.c
    ../src/hash.c
    ../src/lexer.c
    ../src/parser.c
    ../src/time_api.c
    ../src/xr_input.c
    ../src/xr_hands.c
    ../src/xr_ui.c
    ../src/gamepad_input.c
)
target_include_directories(zsharp_language_core
    PUBLIC ../include ../src)
target_compile_features(zsharp_language_core PUBLIC c_std_17)
set_target_properties(zsharp_language_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
