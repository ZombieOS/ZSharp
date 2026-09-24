#ifndef ZSHARP_RUST_H
#define ZSHARP_RUST_H

/* Rust cdylib modules use the same stable C value layout as C++ modules. */
#include "zsharp_cpp.h"

#define ZSHARP_RUST_ABI_VERSION ZSHARP_CPP_ABI_VERSION
#define ZSHARP_RUST_ENTRY_NAME "zsharp_rust_call_v1"

typedef ZSharpCppValueType ZSharpRustValueType;
typedef ZSharpCppValue ZSharpRustValue;
typedef ZSharpCppCallV1 ZSharpRustCallV1;

#endif
