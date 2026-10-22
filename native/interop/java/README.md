# Java interoperability SDK

This is the desktop ZVM's persistent Java worker, separate from the existing
Java-to-ZVM launcher library. It requires Java 17 or newer. User modules are JARs
named `Module.zjava.jar`; compile them against `zsharp-java-worker.jar` without
bundling a second copy of the SDK classes in the module.

```java
import ZSharp.ZSharp;
import ZSharp.ZSharp_Export;

@ZSharp_Export
public class Counter {
    public int value;

    public Counter(int initial) {
        value = initial;
    }

    public int increase(int amount) {
        value += amount;
        return value;
    }

    public static Object askZSharp(int value) {
        return ZSharp.call("Main:Main:Calculate", value);
    }
}
```

The annotation exports public constructors, methods and fields declared by that
class. Alternatively, annotate individual public members. Private members and
inherited `Object` methods are never exported implicitly. Final fields are
read-only. Overloads are selected by argument compatibility; ambiguous overloads
produce an error rather than selecting an arbitrary method. Java varargs methods
are not currently bridge-callable; use a typed array parameter instead.

Instances stay alive across calls. Releasing a handle invalidates that handle,
not unrelated instances. Returned arrays/lists and string-keyed maps travel by
value; other objects travel as live handles. `BigDecimal` preserves arbitrary
decimal input without first converting it to `double`. Primitive numeric
parameters are range-checked. Non-finite floating-point results are rejected.

Z# objects arrive as `ZSharpObject`, whose `get`, `set` and `invoke` methods call
back to the original VM object. Callbacks are synchronous and supported only on
the thread handling the current foreign call. A callback cannot re-enter the
same active Java worker. User `System.out` output is forwarded to stderr so it
cannot corrupt the JSON-lines transport. Module classes and static state are
cached until worker shutdown; restart the ZVM to reload a rebuilt module.

Standalone validation, from the repository root:

```powershell
node tests/interop-managed/run.mjs
```

That script compiles both SDKs and fixture modules into an isolated build
directory and exercises the real persistent workers. It requires a JDK, Node.js
and the .NET 8 SDK or a compatible newer SDK with the .NET 8 targeting pack.
