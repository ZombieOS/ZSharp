# Z# Game Runtime Test

This small project exercises Vulkan drawing, separate scene/object files,
named-key input, gravity, dynamic/static bodies, collisions, jumping,
concurrent `Start` tasks, generated audio, and scene switching.

Package both forms with:

```powershell
zsharp package game "path/to/examples/test-game" ZSharpGameTest --unbytecode
```

Run `Packages/ZSharpGameTest.zgame`. Use A/D or the arrow keys to move, Space
to jump, E to switch scenes, and Escape to quit.
