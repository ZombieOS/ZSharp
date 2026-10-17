# Z# PCVR Lab — experimental 1.3.0.0

This is an experimental hardware test, not a validated VR release. Initial
Quest PCVR testing confirmed head/controller tracking, trigger color changes,
vibration and both controller pointers activating the world-space buttons.
The optional bare-hand checks below still need hardware testing.
Use a clear play area
and stay stationary for the first test. Stop if the image is unstable or
uncomfortable. No artificial locomotion is enabled in this example.

## Run

Use Z# 1.3.0.0 or the development `build/Release/zsharp.exe`, not an installed
older ZVM. `Launch.cmd` prefers the repository's development build, otherwise
uses `zsharp` from PATH, and packages the lab before opening it. Set
`ZSHARP_LAB_VM` to select another executable explicitly.

```powershell
& "D:\VS Code\Projects\Z#\build\Release\zsharp.exe" package game "D:\VS Code\Projects\Z#\examples\pcvr-test" PCVR-Lab
& "D:\VS Code\Projects\Z#\build\Release\zsharp.exe" open "D:\VS Code\Projects\Z#\examples\pcvr-test\Packages\PCVR-Lab.zgame"
```

Connect the headset to your PC using your normal PCVR setup and make sure your
chosen OpenXR runtime detects it. ZVM uses the system's active OpenXR runtime;
it does not change that setting. A separate ZVM install on Quest is not needed.
Choose **Play in VR** in the launch menu. **Play in Desktop Mode** is also
available. With no usable runtime/headset, the optional-mode prompt offers
**Close** or **Play in Desktop Mode**.

## Controls/checks

- Look and lean: the world should remain stable, with different left/right views.
- Translucent blue and coral boxes should follow the left and right controller
  grip poses. Their 35% opacity keeps the visual markers from blocking their
  own aim rays under the normal 50% solid-surface occlusion threshold.
- Right trigger: change the target cube's color and play a short 25% pulse.
- A/B (right) and X/Y (left): short 25% pulse. Either grip: short 50% pulse.
- Either stick click: short 25% pulse. Left trigger: blue while pressed;
  releasing it restores green, including a quick tap.
- Tracking/capability status prints only when it changes. Press **P** on the
  PC keyboard for stick and pinch details. There is no per-frame console output.
- Physical controller presses print `[Input] ...`. A successful world-space
  button callback prints `[UI CLICK] Blue feedback` or `[UI CLICK] Coral feedback`.
  These are separate from physical input.
  `Launch.cmd` also enables `[XR UI]` trigger diagnostics showing focus, aim
  tracking, hit target, release-to-arm state, and whether activation fired.
- The console reports `XR.hands.available`. This is a capability of the active
  **PCVR runtime**, not a promise based on the headset model. A Quest headset
  connected to a PC may not expose hands through that connection/runtime.
- Optional bare-hand check: put controllers down, enable hand tracking in the
  headset/runtime if supported, and open both hands. Four 35%-opaque, 2.5 cm
  cubes follow the left/right index and thumb tips only while each joint is
  tracked. These markers do not block controller UI rays. No full hand mesh is
  drawn. If hand tracking is unavailable, the cubes remain hidden.
- Open the hand before the first pinch, then bring thumb/index tips together.
  The lower small **hand feedback** cube becomes blue for left pinch, coral for
  right pinch, green for both, and white when neither is held. Pinch press/release
  transitions are logged; press **P** to inspect normalized pinch values.
  Losing tracking clears held input; reopen the hand before pinching again.
  Pinching does **not** click UI buttons: controller trigger interaction remains
  the only UI activation mechanism in this example.
- World-space UI: aim either controller's laser at **Blue feedback** or
  **Coral feedback**, then squeeze that controller's trigger. Either controller
  can click either button. Hover changes the button background. Clicking changes
  the separate small feedback cube and prints the callback's running click count;
  the existing main target/trigger test remains independent.
- Hold a trigger before aiming at a button: it must not click just because the
  pointer enters. Release and squeeze again to click. Check both hands separately
  and together. After focus/tracking loss or a scene switch, release the trigger
  before the next UI click. A held trigger must not repeatedly invoke callbacks.
- The blue/coral buttons are world-space XY panels at 1.1 m high, 1.6 m ahead. Their existing
  160 x 32 logical size uses scale 0.005, giving a 0.8 x 0.16 m panel with scaled
  text. Both faces are interactive, and XYZ rotations use normal scene transforms.
  The desktop fallback retains its existing screen-space button behavior; this
  tiny-scaled layout is intended for the headset test, not a desktop menu.
- Left: a checker-textured cube behind a blue translucent panel. Right: an
  imported, spinning FBX prism with the same checker texture. Point and
  directional lights illuminate the objects and produce floor shadows.
- Escape on the PC keyboard exits. In desktop mode, Space changes cube color.
- Shutdown prints `PCVR Lab shutdown completed.` and must return control to
  the launcher/terminal without leaving a game process running.

The script keeps the rig at the origin so its hand-marker positions can use
the raw tracked-space coordinates directly. If you move/turn the rig in your
own game, apply the rig transform to scripted controller objects too.

## Current limitations

Windows PCVR only. Touch-controller bindings. The headset renderer supports
cubes and FBX meshes with existing mesh-part/bone poses, PNG/BMP textures,
material slots, transparency/cutouts, lighting and shadows. The GPU depth,
texture and blend paths have offscreen tests; the new rendering additions still
need headset retesting. Existing game buttons now render as world-space panels
in VR, with independent left/right controller aim pointers and trigger clicks.
The rays extend up to 10 m and are shown only in scenes with visible buttons.
The nearest button wins; solid rendered surfaces with effective alpha at least
50% block the pointer, while texture cutout holes do not. Controller poses in
`XR.left`/`XR.right` remain controller grip poses, not pointer/aim poses or bare
hands. Experimental hand joints and pinch input use the separate
`XR.left.hand`/`XR.right.hand` paths and require runtime hand-tracking support.
Hand tracking is not guaranteed through Quest Link/SteamVR or every other PCVR
connection, and its new hardware tests remain outstanding. Mixed-reality
passthrough is deferred pending compatible-runtime hardware testing and is not
included in 1.3.0.0. Direct-touch UI, automatic hand/pinch UI activation and
headset-locked UI are not implemented.
Unsupported visible 2D geometry reports an error instead of being silently
misplaced in VR. The desktop window is a scene preview, not an exact eye mirror.
Projection is currently limited to 0.05–100 m. Transparent triangles use
back-to-front sorting, not order-independent transparency. CPU mesh posing,
lighting and shadow work is shared with the desktop renderer; GPU work handles
rasterization, depth, texture sampling and blending. Stereo culling currently
applies to cubes; offset imported meshes are conservatively retained.
The stage/floor reference space is preferred; if unavailable, the local-space
fallback assumes a 1.6 m eye height. Extended button coverage and different
headset/runtime combinations still need hardware testing.

For automated desktop smoke tests only, set `ZSHARP_XR_MODE=desktop` to skip
the mode dialog. `ZSHARP_XR_MODE=vr` requests VR without the first dialog, but
still shows the headset-error dialog on failure. This does not bypass a game's
required-VR setting or OS restrictions.
