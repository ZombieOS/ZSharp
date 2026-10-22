@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class,
    kotlin.experimental.ExperimentalNativeApi::class)

import kotlinx.cinterop.*
import kotlin.native.CName
import zsharp.interop.*

// Kotlin/Native scaffold: build with the generated native/kotlin cinterop
// binding. Retained handles identify instances, never a StableRef address.
private val counters = mutableMapOf<String, String>()
private var nextHandle = 0L

@CName("zsharp_kotlin_call_v2")
fun callZSharpV2(
    host: CPointer<ZSharpInteropHostV2>?,
    request: CPointer<ZSharpInteropRequestV2>?,
    result: CPointer<ZSharpInteropResultV2>?,
    error: CPointer<ByteVar>?,
    errorSize: ULong,
): Int {
    fun fail(message: String): Int {
        val bytes = message.encodeToByteArray()
        if (error != null && errorSize > 0uL) {
            val count = minOf(bytes.size.toULong(), errorSize - 1uL).toInt()
            for (i in 0 until count) error[i] = bytes[i]
            error[count] = 0
        }
        return 0
    }
    fun emit(json: String): Int = memScoped {
        val sink = result?.pointed ?: return@memScoped fail("missing result sink")
        val write = sink.write ?: return@memScoped fail("missing result writer")
        val bytes = json.encodeToByteArray()
        write(sink.context, json.cstr.ptr, bytes.size.toULong(), error, errorSize)
    }
    try {
        val current = request?.pointed ?: return fail("missing interop request")
        if (current.abi_version != 2u || current.struct_size.toULong() < sizeOf<ZSharpInteropRequestV2>().toULong())
            return fail("unsupported interop request ABI")
        val operation = current.operation?.toKString() ?: return fail("missing operation")
        val target = current.target?.toKString() ?: return fail("missing target")
        val member = current.member?.toKString() ?: return fail("missing member")
        val handle = current.handle?.toKString() ?: return fail("missing handle")
        val args = current.arguments_json?.toKString() ?: return fail("missing arguments")
        if (operation == "call" && target == "echo") return emit(args)
        if (operation == "call" && target == "callback") return memScoped {
            val bridge = host?.pointed ?: return@memScoped fail("missing Z# callback host")
            if (bridge.abi_version != 2u || bridge.struct_size.toULong() < sizeOf<ZSharpInteropHostV2>().toULong())
                return@memScoped fail("unsupported host ABI")
            val invoke = bridge.invoke ?: return@memScoped fail("missing host dispatcher")
            val callback = alloc<ZSharpInteropRequestV2>()
            callback.abi_version = 2u
            callback.struct_size = sizeOf<ZSharpInteropRequestV2>().toUInt()
            callback.operation = "call".cstr.ptr
            callback.target = "Main:Main:Callback".cstr.ptr
            callback.member = "".cstr.ptr
            callback.handle = "".cstr.ptr
            callback.arguments_json = args.cstr.ptr
            invoke(bridge.context, callback.ptr, result, error, errorSize)
        }
        if (operation == "new" && target == "Counter") {
            require(args.length >= 3 && args.first() == '[' && args.last() == ']')
            val id = (++nextHandle).toString()
            counters[id] = args.substring(1, args.lastIndex)
            return emit("{\"\$ref\":\"$id\",\"\$type\":\"Counter\"}")
        }
        val value = counters[handle] ?: return fail("released or unknown object handle")
        return when {
            operation == "get" && member == "value" -> emit(value)
            operation == "set" && member == "value" -> {
                require(args.length >= 3 && args.first() == '[' && args.last() == ']')
                counters[handle] = args.substring(1, args.lastIndex)
                emit("null")
            }
            operation == "invoke" && member == "echo" -> emit(args)
            operation == "release" -> { counters.remove(handle); emit("null") }
            else -> fail("unknown Kotlin object member or operation")
        }
    } catch (failure: Throwable) {
        return fail(failure.message ?: "Kotlin interop failure")
    }
}
