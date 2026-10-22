# C# interoperability SDK

Build `ZSharp.Interop.Worker.csproj` using the .NET 8 SDK or a compatible newer
SDK with the .NET 8 targeting pack. Reference the resulting `ZSharp.Interop.dll`
from a user class library and name its output `Module.zcs.dll`. Keep the SDK
assembly, worker assembly, dependency manifest and runtime configuration together
in the ZVM distribution. User modules may have adjacent managed dependency DLLs.

```csharp
using ZSharp;

[ZSharpExport]
public class Counter
{
    public int Value { get; set; }

    public Counter(int initial)
    {
        Value = initial;
    }

    public int Increase(int amount)
    {
        Value += amount;
        return Value;
    }

    public static object? AskZSharp(int value)
    {
        return Runtime.Call("Main:Main:Calculate", value);
    }
}
```

`[ZSharpExport]` exports public constructors, methods, fields and properties
declared by that type. Individual public members may be annotated instead.
Private members, private property accessors and inherited `System.Object`
methods are not implicitly exported. Read-only fields and init-only properties
remain read-only after construction.
Ambiguous overloads fail explicitly. Generic, `ref`/`out` and `params` methods
are not currently supported; use ordinary typed parameters and arrays.

Instances and module static state persist until release/shutdown. Arrays/lists
and string-keyed dictionaries cross by value; other objects cross as live
handles. `ExactNumber` preserves the original arbitrary-precision decimal text;
native numeric parameters are range checked. Conversion to `decimal` rejects
rounding, and non-finite floating-point results are rejected.

Z# object arguments arrive as `ZSharpObject`, exposing `Get`, `Set` and `Invoke`
against the original VM object. `Runtime.Call("File:Room:Brain", arguments...)`
calls an imported Z# brain. Callbacks are synchronous on the worker's current
call thread; re-entering the same active worker is rejected. Asynchronous
`Task`-returning methods are not automatically awaited. Use synchronous exported
entry points. `Console.WriteLine` goes to stderr, not the protocol stream.

Run `node tests/interop-managed/run.mjs` from the repository root to build the
SDKs and run standalone Java/C# worker regression tests.
