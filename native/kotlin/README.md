# Kotlin/Native interop

The existing `zsharp_kotlin_call_v1` scalar ABI is unchanged. New modules can
export `zsharp_kotlin_call_v2` to exchange arrays, exact decimal values, records,
retained objects and synchronous calls back to Z#.

Generate the Kotlin C interop binding using the platform's Kotlin/Native tools:

```text
cinterop -def native/kotlin/zsharp_interop.def -compiler-option -Inative/include -o zsharp_interop
```

Build a dynamic library with `-library zsharp_interop` and an entry annotated
`@CName("zsharp_kotlin_call_v2")`. See
`tests/native-interop-v2/Fixture.kt` for the complete C entry scaffold. Native
imports remain `kt:` and the library suffix remains `.zkt.dll`, `.zkt.so` or
`.zkt.dylib`, depending on the operating system.

The authoritative ABI and tagged JSON protocol are documented in
`native/include/zsharp_interop.h`. JSON argument/result strings are UTF-8.
Do not convert exact `{"$number":"..."}` values to `Double` when exactness
matters. A native object handle identifies one retained instance, not a class
definition or a raw pointer.

The result sink copies its input before returning, so the entry can emit a
local buffer and then dispose of it. Call the sink exactly once on success and
propagate failure. Do not repeat a mutating operation after a sink error.

Host callbacks and host object operations must occur synchronously inside the
entry on the calling ZVM thread. Never save the borrowed host/context to a
global, worker or coroutine. Existing explicit C service events remain the
mechanism for asynchronous work. Catch Kotlin exceptions before they reach the
C boundary and return a bounded error rather than unwinding into the ZVM.
