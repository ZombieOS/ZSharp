# External project providers

Z# can read a value from another project with:

```zsharp
Print(playfab.Player.Data.PlayerId):
```

It can call a function supplied by that project with:

```zsharp
Function.call(playfab.Client.Player.Refresh):
```

The first name (`playfab`) selects a provider by dependency PID. The remaining
names are passed unchanged to that provider. A provider is a native shared library, so its
callbacks may forward the operation to code written in another language, an
SDK, hardware library, or a remote service.

Run a source or bytecode file with a provider registered under that project
name:

```text
zsharp run Game.zsharp --provider playfab=path/to/playfab_provider.dll
zsharp run-bytecode Game.zbc --provider playfab=path/to/playfab_provider.dll
```

On Linux and macOS, use the platform's shared-library extension instead of
`.dll`. Multiple `--provider` options may be supplied.

## C ABI

Providers include `native/include/zsharp_provider.h` and export this entry
point:

```c
ZSHARP_PROVIDER_EXPORT const ZSharpProviderV1 *zsharp_provider_v1(void);
```

`ZSharpProviderV1.get_variable` receives the file, room, and value names for a
four-part value path. It returns a `number`, `text`, or `status` value.
`ZSharpProviderV1.call_function` receives the three names after the project in
a four-part `Function.call` target.

Providers can also implement `set_variable`, `get_member`, `set_member`, and
`call_method`. These callbacks support qualified forms such as:

```zsharp
text.set.playfab.Player.Data.PlayerId = "new-id":
Print(playfab.Entities.Players.User.X):
playfab.Entities.Players.User.set.X = 20:
playfab.Entities.Players.User.Move[10, 20]:
```

Every referenced provider file must be imported inside the calling room, for
example `import playfab.Player():` and `import playfab.Entities():`. The PID and
required version are declared under `Dependencies` in `project.zsettings`.

Z#-implemented `Function.call` targets can accept arguments and return values.
Provider ABI v1 external functions still have no arguments and no returned
value. Qualified provider object methods accept `number`, `text`, and `status`
arguments. The next provider ABI will carry function arguments and results.

The working example at `examples/providers/mock_provider.c` can be built with:

```text
cmake -S . -B build -DZSHARP_BUILD_EXAMPLES=ON
cmake --build build
```

A real PlayFab provider would use PlayFab's SDK inside these callbacks and own
the login/session state. Z# itself intentionally does not embed PlayFab
credentials or assume which PlayFab SDK a project uses.
