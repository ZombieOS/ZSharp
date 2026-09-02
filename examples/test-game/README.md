# Z# Game Runtime Test

This small 2D project exercises the first complete `zsharpgame` runtime:
Vulkan drawing, scenes, Z# property reads and writes, keyboard input, gravity,
dynamic/static bodies, collisions, jumping, concurrent `Start` tasks, and
generated audio.

Package both versions with:

```powershell
zsharp package game "path/to/examples/test-game" ZSharpGameTest --unbytecode
```

Run `Packages/ZSharpGameTest.zgame`. Use A/D or the arrow keys to move, Space
to jump, E to switch scenes, and Escape to quit. A gamepad can move and jump
with its left stick/D-pad and primary button.
