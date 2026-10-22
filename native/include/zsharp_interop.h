#ifndef ZSHARP_INTEROP_H
#define ZSHARP_INTEROP_H

/* Additive native interop ABI. The existing C/C++/Rust/Kotlin v1 scalar ABI
 * remains supported and unchanged. New modules may export a *_call_v2 entry.
 * All structures are caller-owned. Pointers are borrowed only for the duration
 * of the current synchronous call; never retain a host, request, or result sink.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define ZSHARP_INTEROP_EXPORT __declspec(dllexport)
#define ZSHARP_INTEROP_CALL __cdecl
#else
#define ZSHARP_INTEROP_EXPORT __attribute__((visibility("default")))
#define ZSHARP_INTEROP_CALL
#endif

#define ZSHARP_INTEROP_ABI_V2 2u
#define ZSHARP_INTEROP_MAX_JSON_BYTES (1024u * 1024u)
#define ZSHARP_INTEROP_CALL_OPERATION "call"
#define ZSHARP_INTEROP_NEW_OPERATION "new"
#define ZSHARP_INTEROP_GET_OPERATION "get"
#define ZSHARP_INTEROP_SET_OPERATION "set"
#define ZSHARP_INTEROP_INVOKE_OPERATION "invoke"
#define ZSHARP_INTEROP_RELEASE_OPERATION "release"

#ifdef __cplusplus
extern "C" {
#endif

/* Arguments are one UTF-8 JSON array in the shared tagged wire format. Results
 * are one tagged wire value. Numeric values carry their exact decimal text,
 * not a lossy double. Arrays/maps and explicit host/foreign object references
 * use that same format in both directions. The VM validates the wire format:
 *   text/status/null: JSON string / boolean / null
 *   exact number:    {"$number":"123.456"}
 *   array:           [WIRE, ...]
 *   plain record:    {"$record":{"field":WIRE, ...}}
 *   native object:   {"$ref":"OPAQUE_ID","$type":"ExportedClass"}
 *   Z# object:       {"$zref":"OPAQUE_ID","$type":"File.Room"}
 * A naked JSON number is not the exact-number representation. A whole wire
 * payload is limited to ZSHARP_INTEROP_MAX_JSON_BYTES bytes, excluding NUL.
 * operation, target, member, handle and arguments_json are non-NULL, NUL-ended
 * strings; unused fields are "", and no-argument calls use "[]".
 *
 * call: target is an exported function or a File:Room:Brain host callback.
 * new: target is an exported class/factory.
 * get/set/invoke/release: handle identifies one retained object; member is the
 * field/method name (empty for release). set has exactly one argument.
 * Handles are opaque, module-owned identifiers, not raw pointers. A released
 * handle must be rejected, never rebound to an unrelated later allocation.
 */
typedef struct ZSharpInteropRequestV2 {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *operation;
    const char *target;
    const char *member;
    const char *handle;
    const char *arguments_json;
} ZSharpInteropRequestV2;

/* Emit exactly one complete JSON value on success. The receiver copies bytes
 * before write returns, so stack buffers and local std::string values are safe.
 * Never probe/retry a mutating operation to determine its result length.
 * A failed write must be propagated as failure; it may not be repeated.
 */
typedef int (ZSHARP_INTEROP_CALL *ZSharpInteropWriteV2)(
    void *context, const char *json, size_t length,
    char *error, size_t error_size);

typedef struct ZSharpInteropResultV2 {
    void *context;
    ZSharpInteropWriteV2 write;
} ZSharpInteropResultV2;

typedef int (ZSHARP_INTEROP_CALL *ZSharpInteropHostInvokeV2)(
    void *context, const ZSharpInteropRequestV2 *request,
    const ZSharpInteropResultV2 *result, char *error, size_t error_size);

typedef struct ZSharpInteropHostV2 {
    uint32_t abi_version;
    uint32_t struct_size;
    void *context;
    ZSharpInteropHostInvokeV2 invoke;
} ZSharpInteropHostV2;

/* 1 means success, 0 means failure with a bounded NUL-ended UTF-8 error.
 * Exceptions/panics must not cross the ABI. Host callbacks are supported only
 * synchronously, on the calling ZVM task thread and inside the current entry.
 * Background threads must use an existing explicit service/event API instead;
 * retaining host/context and calling it later is invalid, not asynchronous IPC.
 */
typedef int (ZSHARP_INTEROP_CALL *ZSharpInteropCallV2)(
    const ZSharpInteropHostV2 *host, const ZSharpInteropRequestV2 *request,
    const ZSharpInteropResultV2 *result, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
