#ifndef ZSHARP_KOTLIN_H
#define ZSHARP_KOTLIN_H

/* Kotlin/Native exports a stable C entry through @CName. */
#include "zsharp_c.h"

#define ZSHARP_KOTLIN_ABI_VERSION ZSHARP_C_ABI_VERSION
#define ZSHARP_KOTLIN_ENTRY_NAME "zsharp_kotlin_call_v1"
#define ZSHARP_KOTLIN_ENTRY_V2_NAME "zsharp_kotlin_call_v2"
typedef ZSharpCValue ZSharpKotlinValue;
typedef ZSharpCCallV1 ZSharpKotlinCallV1;
typedef ZSharpInteropCallV2 ZSharpKotlinCallV2;

#endif
