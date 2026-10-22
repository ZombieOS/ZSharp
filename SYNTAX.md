# Z# syntax guide

## Desktop speech, microphone, speaker and application control (1.4.0.0)

These APIs are for the native desktop ZVM, not bZVM. Declare
`ZSharp: [1.4.0.0]:` or newer in project settings and import each API in every
room that uses it; importing another script does not grant that script's
feature imports to the caller:

```zsharp
noticed room Assistant[] (
 import ZSharp.Speech():
 import ZSharp.Microphone():
 import ZSharp.Speaker():
 import ZSharp.Desktop():

 noticed brain Greet[] (
  Speech.speak("Hello from Z#!"):
 )
 noticed brain Listen[] (
  text Heard = Microphone.getInput:
  Print(Heard):
 )
)
```

The canonical names are `Speech`, `Microphone`, `Speaker` and `Desktop`;
`Mic` is not an alias. `Speech`/`Desktop` actions are statements, not returned
values. Microphone/device/level reads below are properties without `()`.

### Speech and `.zvoice`

```zsharp
Speech.speak("Use the system's default voice"):
Speech.speak("Use a saved voice", "Voices/Assistant.zvoice"):
Speech.createVoice("Voices/Reference.mp3", "Voices/Assistant.zvoice"):
Speech.prepare("Voices/Assistant.zvoice"):
```

`Speech.speak` accepts one or two text expressions. With one argument it uses
the native system voice; the second argument selects a project-relative
`.zvoice` file. Speech is queued without blocking the UI/script thread,
with at most 32 queued utterances per speech service and 16,384 UTF-8 bytes
per utterance. Playback uses this process's selected `Speaker` device.
Failures are reported by the runtime; ending the app cancels queued speech.

`Speech.prepare(voice)` validates a project-relative `.zvoice` and warms a cloned
voice's local model before returning, without playing audio. Put it at startup,
before displaying a listening/ready indicator, to move the first-use model load
out of the conversation. It requires `ZSharp.Speech` like other speech actions;
preparing a system preset only validates it. Preparation blocks its calling
task, not the UI, and is cancellable. Cloned models stay in a dedicated worker
between utterances, independently of Python language imports and recognition.

At the default clone pitch/speed (100), playback begins as PCM chunks arrive
instead of waiting for the entire reply or writing a full temporary WAV. Output
queues are bounded and use the selected speaker and volume. Non-default cloned
pitch/speed uses the complete signal for its existing audio adjustments, so it
cannot provide the same streaming latency. First loading and text generation
still take time; neither preparation nor streaming promises instant speech.

A manually authored preset `.zvoice` is UTF-8 JSON:

```json
{
  "format": "ZVOICE",
  "version": 1,
  "type": "preset",
  "backend": "system",
  "voice": "",
  "pitch": 100,
  "speed": 100,
  "volume": 100
}
```

These are the default values when fields are omitted. An empty `voice`
chooses the system default; a nonempty value must identify an installed
native voice. `pitch` is 50..200%, `speed` is 25..400%, and `volume` is
0..100%; 100 keeps normal pitch/speed/full volume. Engines approximate rate
and pitch differently. Windows uses SAPI; Linux requires a supported local
`espeak-ng`/`espeak` executable. macOS uses `say`, which explicitly rejects
nondefault preset pitch instead of silently ignoring it.

`Speech.createVoice` is genuine, optional **local voice conditioning**, not
a system-voice preset or a pitch estimate. It accepts MP3/WAV references
up to 50 MiB, uses up to the first 30 seconds, and writes a new `.zvoice`.
Reference/output paths must stay within the project; an existing output is
not overwritten. Call this explicitly when creating a voice, not on every
startup. Use reference recordings only with the speaker's permission.

Cloning requires optional `pocket-tts` and `soundfile` packages and a genuine
local model, installed through the managed setup below or configured explicitly
with `ZSHARP_VOICE_MODEL_CONFIG` pointing to an absolute local model YAML with
local safetensors weights and a local tokenizer. Speech calls do not download
models or silently fall back to a different voice. The output embeds
speaker conditioning, checksums and model/backend fingerprints, not the full
model. Matching model data and library versions are still needed to speak
with it. Clone pitch/speed changes additionally need optional `librosa`.
Model preparation/creation helpers are cancellable and time out after 120
seconds. Streamed synthesis additionally allows its bounded five-minute audio
playback interval; cancellation also stops queued output. Models and
optional packages are not bundled automatically. Real-model conditioning,
synthesis and native source/bytecode execution have been checked on Windows
using synthetic WAV/MP3 references and dummy audio. Output quality has not been
listening-tested; Linux/macOS backends still need live testing.

### Optional local speech setup

From the source checkout, use a full Python 3.10..3.14 interpreter:

```text
python tools/install-speech-components.py
```

This explicit setup downloads pinned Python dependencies into a private virtual
environment and provisions the Vosk recognition model and genuine Pocket TTS
model where publisher access permits. It never installs packages globally,
records the microphone or plays audio. Pocket model access may require accepting
the publisher's model terms and supplying your own authorized `HF_TOKEN`; setup
does not log into an account, bypass model gates or substitute a non-cloning
model. A model failure is reported even if the other component installed.
Use `--models-only` to retry model setup in an existing managed environment,
or `--home ABSOLUTE_DIRECTORY` for a dedicated alternate directory.

If you already downloaded the genuine model through an authorized browser
session, you can install that file without copying an account token:

```text
python tools/install-speech-components.py --models-only --voice-weights "C:/Downloads/model.safetensors"
```

The file must match the exact revision required by the installed Pocket TTS
package. Setup checks its size and SHA-256 against the publisher's metadata
before importing it. This does not read browser cookies or create access tokens.

The default managed speech directory is:

- Windows: `%LOCALAPPDATA%/ZombieOS/ZSharp/speech`.
- Linux: `$XDG_DATA_HOME/zsharp/speech`, or `~/.local/share/zsharp/speech`.
- macOS: `~/Library/Application Support/ZSharp/speech`.

The runtime discovers its `venv`, `models/pocket-tts/model.yaml` and
`models/vosk` automatically. Set `ZSHARP_SPEECH_HOME` to an absolute alternate
root when using `--home`; it is not a project setting. Explicit
`ZSHARP_SPEECH_PYTHON`, `ZSHARP_VOICE_MODEL_CONFIG` and `ZSHARP_SPEECH_MODEL`
override their corresponding managed interpreter/model paths. Empty explicit
values are errors. Native system/preset speech does not require these optional
models. Setup success does not verify real-device permissions, voice quality
or microphone behavior.

### Microphone capture, recognition and device selection

```zsharp
Print(Microphone.getInputDevice):
Microphone.setInputDevice("Exact microphone device name"):
Microphone.setInputDevice(): // Reset this process to the system default.

Print(Speaker.getOutputDevice):
Speaker.setOutputDevice("Exact speaker device name"):
Speaker.setOutputDevice():

number Level = Microphone.level:
Microphone.record("sample.wav", 5s):
Microphone.record("short.wav", 250ms):
text Recognized = Microphone.getInput:
```

`Microphone.getInputDevice` and `Speaker.getOutputDevice` return the selected
device's name, or `null` if no corresponding device exists. Selection accepts
a unique exact name (or `id:NUMBER` for a known SDL physical device ID);
ambiguous/missing devices produce an error. Zero-argument setters reset the
selection. This changes only the ZVM process, never the OS default. Future
streams use the selection; already-open streams are not rerouted.

Use `if[Microphone.getInputDevice != null] (...)` to check device availability.
For equality/inequality, `null` equals only `null`; every non-null value compares
unequal to it, in either operand order. This does not make text and numbers
interchangeable, or allow ordering comparisons with `null`.

`Microphone.level` is current recording RMS amplitude in 0..1, and is zero
when not recording. Merely reading a level/device or importing the API does
not open the microphone. Capture requires OS microphone permission and an
available device; denied permission, mute/no audio and disconnection report
errors rather than pretending recording succeeded.

`Microphone.record(path, duration)` writes mono 16 kHz PCM16 WAV to a
project-relative path. Use literal `s`/`ms` durations, like `wait`: minimum
50ms, maximum 30s. Explicit recording captures the requested duration unless
cancelled or an error occurs. Capture blocks its calling task, not the native
UI loop, and cancellation does not write a new recording.

`Microphone.getInput` returns recognized **text**, not raw samples or a file
name. It checks the offline recognizer before opening the microphone, waits
up to five seconds for speech, then captures one utterance of up to 30 seconds
including a short pre-roll. It ends after approximately 400 ms of silence,
transcribes the temporary WAV and removes temporary files. End-of-speech uses
20 ms sample windows and an adaptive energy/noise threshold, not the timing of
device reads; changing background noise can still resemble speech. Short pauses
below the silence threshold stay within the utterance. Explicit
`Microphone.record` keeps its exact-duration behavior. Recognition
requires optional `vosk` and the managed recognition model, or
`ZSHARP_SPEECH_MODEL` naming an extracted local Vosk model directory containing
`am/final.mdl` (small models up to 512 MiB). There are separate 30-second
preflight and transcription limits, plus the capture interval; these steps
are cancellable. No model/audio is automatically downloaded or sent to a
network recognition service. Missing recognition support fails before capture.
The ZVM keeps one dedicated local recognition worker and a cached Vosk model
for the current run; preflight and subsequent utterances reuse it instead of
reloading the model on every read. Each utterance uses a fresh recognizer, so
previous words are not retained. The worker is separate from Python language
imports and is stopped when the run ends. Recognition failures remain errors
(not empty input) and include bounded underlying recognizer diagnostics.

### Desktop application/window actions

```zsharp
Desktop.openApp("notepad.exe"):
Desktop.minimizeApp("notepad.exe"):
Desktop.maximizeApp("notepad.exe"):
Desktop.scaleApp("notepad.exe", 1000, 700):
Desktop.moveApp("notepad.exe", 0):
Desktop.focusWindow("notepad.exe"):
Desktop.typeText("Hello from Z#!", "center"):
Desktop.sendKeys("CTRL+S"):
Desktop.closeApp("notepad.exe"):
```

Import `ZSharp.Desktop` in the using room. These are all statements. App/window
selectors are an exact process/application name or numeric PID (text or number);
`openApp` instead takes an executable/application name or path. Ambiguous
targets are errors, not an arbitrary first match. Window operations other than
`closeApp` require one matching window.

On Windows, `closeApp` groups matching helper processes into one application
family using verified process ancestry, rather than treating each helper as a
separate app. It requests closure of all visible, unowned top-level windows in
that family, including windows belonging to descendant processes with different
executable names. A PID selects that process and its descendants. Two unrelated
instances with the same executable name remain ambiguous; use a PID to select
one. A family with no eligible window produces an error.

`closeApp` requests normal graceful closure and does not force-kill processes or
bypass unsaved-document prompts. Returning successfully means the close requests
were sent, not that every process has exited. Save/cancel dialogs, applications
that ignore closure and background workers may keep the application running.
Resolve any save prompts normally; this API does not forcibly clear them.

`focusWindow(selector):` restores a minimized target when needed and asks the
OS to bring its one selected window to the foreground. On Windows it waits for
the selected window to process the activation request before checking focus.
If a responsive window is still blocked by foreground policy, one OK/Cancel
confirmation offers an explicit user-approved switch and one retry. Cancel
stops the call without typing or sending a shortcut. An unresponsive window or
a denied retry produces an error. Focus is verified, not guaranteed: this does
not bypass Windows restrictions or request elevation.

`typeText(text, region):` clicks a region of the currently focused window, then
types the provided text. Both arguments must be text; expressions and variables
are allowed. Regions are case-insensitive and select the centers of a 3-by-3
grid inside the window:

```text
topleft       topmiddle       topright
middleleft    center          middleright
bottomleft    bottommiddle    bottomright
```

This is coordinate-based input, not text-box detection. The selected point must
actually contain the intended editable field. Windows uses the client area so
the title bar and frame are excluded. The pointer moves to that point. Text is
strict UTF-8, limited to 65536 bytes; newline and tab are allowed, and an empty
string does nothing. Text goes through native input, not the clipboard.
On Windows, recognized Unicode `Edit`/`RichEdit` text controls in another
process receive an acknowledged native click followed by one acknowledged
text insertion (`EM_REPLACESEL`, with undo enabled). Text is inserted at the
caret or replaces the selection left by that click; the whole document is not
replaced unless it is the selected text. The click itself may reposition the
caret or change the selection. CRLF, LF and standalone CR are normalized to
Windows paragraph endings, preserving Unicode text and multiline input.
Custom/unrecognized controls and controls in the ZVM's own process retain
paced Unicode keyboard input. The same-process fallback avoids retaining a
borrowed text pointer after a cross-thread timeout. Neither route uses the
clipboard.

Windows rechecks the selected app and control around the native click and
insertion, and throughout paced keyboard entry. Changed focus, held modifiers
or failed input stop the call without replaying text already sent. Native
insertion waits up to 1000 ms for acknowledgment. A timeout may leave partial
or complete text, so it never retries the insertion or falls back to keyboard
input. Longer keyboard-entry strings take time; other Z# tasks can continue
while this call waits. Real Windows Notepad was validated with the reported
black-hole and Minecraft paragraphs, Unicode and 80 numbered lines; the saved
text matched after newline normalization. Linux/macOS desktop input still
requires live platform validation.

`sendKeys("CTRL+SHIFT+S"):` sends one shortcut to the currently focused window.
Modifiers precede exactly one key and are joined by `+`. Names are
case-insensitive; spaces around tokens are optional. Supported modifiers are
`CTRL`/`CONTROL`, `ALT`/`OPTION`, `SHIFT`, and
`WIN`/`SUPER`/`META`/`CMD`/`COMMAND` (the platform's Windows/Super/Command key).
Keys are `A`..`Z`, `0`..`9`, `F1`..`F24`, `ENTER`/`RETURN`, `TAB`, `SPACE`,
`ESC`/`ESCAPE`, `BACKSPACE`, `DELETE`/`DEL`, `INSERT`/`INS`, `HOME`, `END`,
`PAGEUP`/`PGUP`, `PAGEDOWN`/`PGDN`, and the four arrow names `LEFT`, `RIGHT`,
`UP`, `DOWN`. Some platform/key combinations may be unavailable and produce an
error. For a sequence, use separate calls. Typing a literal `+` belongs in
`typeText`, not in shortcut syntax.

Three standalone media keys are also supported:

```zsharp
Desktop.sendKeys("MEDIA_PLAY_PAUSE"):
Desktop.sendKeys("MEDIA_NEXT"):
Desktop.sendKeys("MEDIA_PREVIOUS"):
```

These names are case-insensitive and cannot be combined with modifiers such as
`CTRL+MEDIA_NEXT`. They send system media controls, not literal text or ordinary
application shortcuts. The OS and active media player decide which media
session responds; a successful send does not guarantee playback or a track
change. Media-key tests use mocks and do not start or change real playback.

Regions, text and entire shortcuts are validated before any input is sent.
Avoid pressing keys or changing focus while automation runs: it controls the
real desktop, and shortcuts can save, close or otherwise change the active app.
Windows refuses conflicting held modifiers and preserves already-held requested
modifiers. Input may be denied for elevated apps, secure desktops or missing
OS permissions. If partial delivery fails, it is reported rather than silently
replaying the text or shortcut. These APIs are desktop-only, not bZVM APIs.

Linux typing/shortcuts also require the optional `libX11.so.6` runtime for
read-only input-state checks. Release all physically held keys/buttons and turn
Caps Lock off first; this backend does not clear or release your held inputs.
macOS requires Accessibility access (and System Events Automation permission),
uses the focused AX window's full frame for regions, and currently does not
support Insert or F21..F24. Its shortcut letter/digit key codes use standard
ANSI positions; keyboard layouts and application-specific bindings may differ.
Linux/macOS input has compile/mock coverage but still requires live desktop
validation. Automated input is best-effort: a successful send, or a native
control's message acknowledgment, does not prove that application validation
accepted the requested text or that a shortcut performed its intended action.

`scaleApp` sets window width/height in whole pixels (1..32768); window-manager
decorations and display scaling follow the host platform.
`moveApp` centers it on a zero-based display index or exact display name;
Windows index 0 is primary, with subsequent displays ordered by device name.
Windows display names use OS device IDs such as `\\.\DISPLAY1`; Linux uses
XRandR output names; macOS uses localized screen names. Native permissions,
application constraints and window-manager policies still apply.

Windows uses native process/window APIs. Linux window control requires X11
and optional `wmctrl`; focusing, typing, shortcuts and minimizing also need
`xdotool`, and display selection
needs `xrandr`. Native Wayland windows are not controllable by this backend.
macOS uses fixed `osascript`/System Events automation and may require the
user's Automation/Accessibility permission. Native macOS builds include
microphone and Apple Events purpose descriptions; OS permission must still
be granted, and custom embedding applications need equivalent declarations.
Its implementation still needs
live Mac validation. No action interpolates an app name into a shell command
or requests elevation. Opening an executable does not accept a shell command
line or additional launch arguments.

### Launching a project at login

Add this optional root setting to `project.zsettings`:

```zsharp
LaunchAtStart: alive:
```

Use `dead` to remove that project's registration on its next actual launch;
omitting the setting makes no registration changes. This is per-user login
startup, not boot-time execution before login. Registration occurs only when
running a validated source entry, bytecode entry or app/game package, never
while checking, compiling or packaging. The exact canonical executable and
entry are saved with `run`, `run-bytecode` or `open` as appropriate under a
stable project-PID key; a later launch after a move
or reinstall updates that same entry instead of adding duplicates. Package
uninstall removes its own entry. Arguments/paths are escaped as data, not shell
code. Other projects and the ZVM updater's separate startup entry are untouched.

Backends are Windows HKCU `Run`, Linux user XDG autostart `.desktop` files,
and macOS user `LaunchAgents` plists. macOS registration takes effect at a
subsequent login; it does not immediately launch another copy. Windows login
commands exceeding 259 characters report a warning. Startup failures do not
prevent the current application from running, and OS policy can disable startup.

Development/test launchers may set `ZSHARP_SKIP_DESKTOP_INTEGRATION=1` to
suppress registration changes. Isolated tests use an existing absolute
`ZSHARP_STARTUP_DIRECTORY`; this redirects registrations to fixture files
without touching the real registry/login folders. These are test controls,
not Z# syntax. `ZSHARP_DESKTOP_DISABLE_ACTIONS=1` also rejects actual Desktop
actions in an isolated test process. Never test startup persistence against an
unrelated user's setup.

## Experimental PCVR and hand tracking (1.3.0.0, desktop only)

```javascript
AllowedDevices (
 Windows: true:
 Linux: true:
 MacOS: true:
 XR: true:
):
XR (
 required: false:
):
```

These blocks belong in `project.zsettings`. Omitting AllowedDevices preserves
existing OS support and disables XR. When the block is present, omitted OS
fields are disabled. Values are literal `true`/`false`; duplicate/unknown
fields are errors. XR required true requires AllowedDevices XR true. OS
restriction checks happen at execution, not packaging, so cross-platform
packaging is still possible. These settings require ZSharp 1.3.0.0 or newer.
Android/iOS entries are not accepted by this desktop prototype.

Optional XR shows a VR/Desktop launch menu. VR checks for a usable OpenXR
runtime/headset; failure offers Close (cancel launch) or Play in Desktop Mode.
Required XR skips mode selection and has no desktop fallback. Desktop-only
projects never initialize OpenXR. The initial PCVR backend is Windows-only;
enabling Linux/MacOS permits desktop execution there, not PCVR support.

All XR script properties/commands require this import in their using room:

```javascript
import ZSharp.XR():

XR.available
XR.active
XR.focused
XR.head.tracked
XR.head.positionX
XR.head.positionY
XR.head.positionZ
XR.head.rotationX
XR.head.rotationY
XR.head.rotationZ

number.set:XR.rig.positionX = 0:
number.set:XR.rig.positionY = 0:
number.set:XR.rig.positionZ = 0:
number.set:XR.rig.rotationY = 90:

XR.left.tracked
XR.left.positionX
XR.left.rotationY
XR.right.trigger.value
XR.right.trigger.held
XR.right.trigger.pressed
XR.right.trigger.released
XR.left.grip.value
XR.left.grip.held
XR.left.grip.pressed
XR.left.grip.released
XR.right.stick.x
XR.right.stick.y
XR.right.stick.angle
XR.right.stick.magnitude
XR.right.stick.held
XR.right.stick.pressed
XR.right.stick.released
XR.right.a.pressed
XR.right.b.pressed
XR.left.x.pressed
XR.left.y.pressed
XR.right.vibrate(75, 200ms):
```

Both hands expose positionX/Y/Z and rotationX/Y/Z. Poses are read-only,
tracked-space coordinates relative to the rig, measured in meters and degrees.
Tracked flags indicate whether the pose is currently tracked; a lost pose can
retain its last valid coordinates, so check tracked before using it. Rig
translation and yaw are writable and transform the player's tracked space,
without overwriting physical head/controller tracking. The floor/stage origin
is preferred; local-space fallback assumes 1.6 m eye height. Scale and rig
pitch/roll are not writable in this prototype.

Trigger/grip values are decimals 0..1; held uses a 0.5 threshold. Stick axes are
-1..1, with positive Y forward and a radial 0.15 center deadzone. Angle is
clockwise 0..360 degrees (0 forward, 90 right, 180 back, 270 left); centered
angle is 0. Magnitude is a decimal 0..1, clamped to the circular range. Stick
held/pressed/released refer to pressing the stick button, not tilting it.
A/B belong to the right hand and X/Y to the left; each supports held, pressed
and released. Button edges are retained until read once by each Z# task;
multiple queued edges are consumed on successive reads. New tasks ignore
earlier edges. Focus loss clears held input and generates releases.

Vibration strength is 0..100, with a duration literal in ms/s (0..10 seconds).
Strength 0 or duration 0 stops that controller's vibration. Calls queue a
request for the XR frame thread; the latest queued request wins. No haptics
are sent without XR focus. Unavailable/desktop mode returns dead/zero for XR
reads and does not vibrate controllers. `available` currently describes the
successfully initialized XR session, not a separate desktop hardware probe.

The first backend uses OpenXR stereo projection and Direct3D 11 on the
runtime-selected GPU, without replacing the desktop SDL/Vulkan renderer.
Headset rendering supports cubes and imported FBX meshes, including existing
mesh-part/bone poses, material textures, opacity, cutouts, point/spot/directional
lights and shadows. Use the same `.zobject`, `.zmodel`, scene `texture`,
`surface`, `rotations`, light attributes and `.zanimation` syntax as desktop.
World preparation shares the desktop mesh caches, lighting and shadow index;
texture sampling, depth tests, alpha blending and rasterization run on the GPU.
Opaque triangles batch by texture; translucent triangles sort back-to-front
per eye. Cube bounds outside both eye frusta skip scene submission while
remaining in the shadow index. Meshes remain conservatively unculled because
their imported origin may be outside their bounds. The projection currently
uses a 0.05 m near plane and a 100 m far plane. Touch bindings are the initial
controller profile. Existing game buttons support world-space panels and
left/right controller pointers as described below. Optional bare-hand
joint/pinch input depends on extensions exposed by the active PC
OpenXR runtime; a headset supporting a feature in standalone mode does not
guarantee that feature over a PC connection. Direct-touch UI, automatic hand
meshes, hand physics, headset-locked UI and standalone Quest deployment are
not implemented. See
`examples/pcvr-test` for the test game. Successful packaging does not verify
headset visuals, performance, tracking alignment or comfort.

Mixed-reality passthrough is deferred pending compatible-runtime hardware
testing and is not included in 1.3.0.0.

### Bare-hand joints and pinch input

Controller grip poses remain `XR.left` and `XR.right`; bare-hand data is
separate and never replaces them. Hand tracking is optional and uses
`XR_EXT_hand_tracking` when the PC runtime supports it. All hand reads require
`import ZSharp.XR():`.

```zsharp
XR.hands.available
XR.left.hand.tracked
XR.right.hand.tracked
XR.left.hand.indexTip.tracked
XR.left.hand.indexTip.positionX
XR.left.hand.indexTip.positionY
XR.left.hand.indexTip.positionZ
XR.left.hand.indexTip.rotationX
XR.left.hand.indexTip.rotationY
XR.left.hand.indexTip.rotationZ
XR.left.hand.pinch.value
XR.left.hand.pinch.held
XR.left.hand.pinch.pressed
XR.left.hand.pinch.released
```

Both hands expose the following 26 joint names. Every joint has read-only
`tracked`, `positionX/Y/Z` and `rotationX/Y/Z` properties:

```text
palm, wrist
thumbMetacarpal, thumbProximal, thumbDistal, thumbTip
indexMetacarpal, indexProximal, indexIntermediate, indexDistal, indexTip
middleMetacarpal, middleProximal, middleIntermediate, middleDistal, middleTip
ringMetacarpal, ringProximal, ringIntermediate, ringDistal, ringTip
littleMetacarpal, littleProximal, littleIntermediate, littleDistal, littleTip
```

Positions use tracked-space meters and rotations use degrees, like the
controller/head poses. Check each joint's `tracked` status before using its
coordinates. `XR.hands.available` reports runtime hand-tracking capability;
`hand.tracked` reports the live, focused tracked hand. No public joint radius
is exposed.

Pinch strength is derived from thumb-tip/index-tip distance: `value` is 1 at
contact and falls to 0 at 0.06 meters apart. The status becomes held at a
distance of 0.02 meters or less and releases at 0.04 meters or more, avoiding
flicker near a single threshold. `pressed` and `released` are retained per-task
edges like controller buttons; new tasks ignore earlier edges. Losing focus,
hand tracking or either required fingertip clears held input, generates a
release and discards pending stale presses. After acquisition or tracking loss,
open the fingers at least 0.04 meters before a new pinch can press.

```zsharp
noticed brain Start[] (
 loop (
  if[XR.left.hand.pinch.pressed] (
   Print("Left pinch at X: " + XR.left.hand.indexTip.positionX):
  )
  wait(16ms):
 )
)
```

The prototype provides joint/input data, not rendered skeletons, hand collision
or automatic pinch activation of world-space buttons. Controller aim/trigger
interaction remains unchanged. Hand tracking and pinch behavior still need
compatible-runtime hardware testing; controller-only VR continues to work when
bare-hand tracking is unavailable.

### World-space VR buttons

In VR mode, existing `shape: button` placements become world-space XY panels.
No separate ray/event syntax is required. Place and rotate a button like any
other scene instance; XYZ rotation and the legacy `rotation` yaw follow normal
3D object transforms. Position is measured in meters. Keep the usual logical
button width/height and font size, then scale the panel to a suitable physical
size. For example, 160 x 32 at scale 0.005 is 0.8 x 0.16 meters; its 16-unit
label is scaled with the panel.

```zsharp
zsharp = type.object
noticed object StartButton[] (
 shape: button:
 text: "Start":
 width: 160:
 height: 32:
 fontSize: 16:
 scaleX: 0.005:
 scaleY: 0.005:
 color: #205281:
 hoverColor: #347FB5:
 event(
  left: "Main:Main:Begin":
 )
)
```

Place it in a scene using the existing JSON layout:

```zsharp
objects[JSON] (
 [
  {"id":"StartButton","name":"StartMenuButton",
   "location":{"x":0,"y":1.1,"z":-1.6},
   "rotations":{"x":0,"y":0,"z":0}}
 ]
)
```

Either tracked controller projects an aim ray up to 10 meters. Point at the
button for `hoverColor`; squeeze that same controller's trigger through the
existing 0.5 threshold to invoke the existing `event.left` callback. Both
controllers activate the primary/left action; the left controller does not map
to a right-click action. Controller pointers use OpenXR aim poses internally;
`XR.left` and `XR.right` positions/rotations still expose their unchanged grip
poses. Normal XR trigger properties remain readable by scripts independently
of button callbacks.

Both faces are rendered and interactive. The closest button along each ray is
selected, rather than desktop overlay `layer` priority. Solid scene geometry
with effective alpha at least 0.5 blocks a farther button; texture cutout holes
do not block it. Rays are displayed only when the active scene has visible
buttons. Trigger presses are tracked independently for both controllers and
do not repeatedly fire while held. Holding a trigger and moving onto a button
does not click it; after scene changes, focus loss or tracking loss, release
the trigger before clicking again. Hidden/fully transparent buttons do not
receive pointer interaction. Desktop mode keeps the existing screen-space
button behavior; world-space panels do not introduce desktop UI layout changes.

## Desktop gamepad input (1.3.0.0)

Gamepad properties and vibration require `import ZSharp.Gamepad():` in their
using room and `ZSharp: [1.3.0.0]:` or newer in `project.zsettings`. This is a
desktop game API, not native-window or bZVM input. It uses the selected
SDL-recognized controller, including mapped Xbox, Nintendo and PlayStation
controllers; availability depends on the controller, connection and driver.
One controller is selected at a time, with automatic disconnect/reconnect
handling. No XR settings are required for ordinary gamepads.

```zsharp
import ZSharp.Gamepad():

Gamepad.connected
Gamepad.name
Gamepad.type
Gamepad.rumbleAvailable
Gamepad.south.label
Gamepad.south.held
Gamepad.south.pressed
Gamepad.south.released
Gamepad.dpad.up.held
Gamepad.left.stick.x
Gamepad.left.stick.y
Gamepad.left.stick.angle
Gamepad.left.stick.magnitude
Gamepad.left.stick.pressed
Gamepad.right.stick.x
Gamepad.right.stick.y
Gamepad.right.trigger.value
Gamepad.right.trigger.held
Gamepad.right.trigger.pressed
Gamepad.right.trigger.released
Gamepad.left.bumper.held
Gamepad.vibrate(75, 200ms):
```

Face-button names describe physical position, not the printed letter:

| Position | Xbox | Nintendo Switch | PlayStation |
| --- | --- | --- | --- |
| `south` | A | B | Cross |
| `east` | B | A | Circle |
| `west` | X | Y | Square |
| `north` | Y | X | Triangle |

Each face has a text `.label` for on-screen prompts. The backend uses the
controller's mapped label when available; generic controllers use physical
position names. `name` is the device name; `type` is `xbox`, `nintendo`,
`playstation` or `generic`, and `none` when disconnected. `connected` and
`rumbleAvailable` are statuses.

All of these controls expose `.held`, `.pressed` and `.released` statuses:

```text
south, east, west, north
dpad.up, dpad.down, dpad.left, dpad.right
left.stick, right.stick, left.trigger, right.trigger
left.bumper, right.bumper, start, back, guide
misc1, misc2, misc3, misc4, misc5, misc6
left.paddle1, right.paddle1, left.paddle2, right.paddle2, touchpad
```

Controls absent from the selected hardware remain dead. Stick button statuses
refer to clicking the stick, not tilting it. Stick `.x` and `.y` range from -1
to 1, positive X is right and positive Y is forward/up. A radial 0.15 center
deadzone is removed and the remaining range rescaled. `.magnitude` is 0..1;
`.angle` is clockwise degrees, with 0 forward, 90 right, 180 back and 270 left.
A centered stick has zero magnitude and angle. Trigger `.value` is 0..1;
trigger statuses use a 0.5 threshold.

Button edges survive short presses between script polls. Each Z# task reads
its own queue, consuming one pending edge per read; another task can read the
same transition independently. New tasks ignore historical edges. Read edges
once into a variable when several conditions need the same result. Focus loss
or disconnect clears held/analogue input, generates needed releases, discards
stale queued presses and stops vibration. Regaining focus or connecting while
a button is already down does not synthesize a new press.

All input/identity properties are read-only. Vibration takes a numeric strength
expression from 0 to 100 and a literal duration in `ms` or `s`, up to 10 seconds:

```zsharp
number Strength = 50:
Gamepad.vibrate(Strength + 25, 0.2s):
Gamepad.vibrate(0, 0ms):
```

Zero strength or duration stops rumble. Unsupported/disconnected hardware does
not rumble, and the latest queued request wins. `rumbleAvailable` lets games
offer feedback without assuming every controller or connection supports it.
A Nintendo controller has been hardware-tested; Xbox and PlayStation devices
have not yet been physically verified. These tests and mappings do not certify
every hardware model or driver.

## Browser events and networking (1.3.0.0)

The loader registers source maps for loaded `.zsharp` files, including imports.
With JavaScript source maps enabled in DevTools, the original `.zsharp` entries
show the exact Z# source without JavaScript wrappers. The `.bzvm-view.js`
entries are empty viewer anchors, not transpiled Z#. Breakpoints, stepping and
live editing of Z# are not implemented. Set `data-sources="false"` on the `bzvm.js` script
tag to disable source registration. Restrictive CSP can also disable this
optional viewer without stopping the WebAssembly runtime; do not loosen CSP
just to enable it. Source served to the browser is already publicly readable.

Networking requires these imports on both desktop ZVM and bZVM:

```javascript
import ZSharp.HTTP():
import ZSharp.Websocket():
```

### Browser DOM and input

```javascript
Browser.save.clicked: ( Browser.result.content.set: "Saved": ):
Browser.name.inputted: ( Browser.result.content.set: Browser.name.getInput: ):
Browser.choice.changed: ( Browser.result.content.set: Browser.choice.getOption: ):
Browser.form.submitted: ( Function.call(Main:Main:Submit): ):

text Link = Browser.link.getattr[href]: // readattr is an alias
Browser.link.setattr[class]: "selected":
Browser.item[2].html.set: '''<strong>First two matching IDs</strong>''':
text() Classes = Browser.item[*].getattr[class]:
text Title = Browser.page.title:
Browser.page.title.set: "New title":
text Platform = "" + Browser.platform:
```

Inline events and the existing `Function.call` event form are supported.
Submitted events prevent the browser's default form reload. `getInput` and
`getOption` read an element's value. `[2]` means the first two elements with
that exact ID in document order, regardless of tag; `[*]` means all. Unqualified
IDs use the first match. Multi-element reads return text arrays. Missing
multi-element matches return an empty array or make writes a no-op; missing
single elements are errors. Repeated IDs are supported for this API, although
unique IDs remain recommended for ordinary HTML. Attribute names currently use
identifier characters. `Browser.index.readelement[title]` reads the current
`index.html` title, not another unopened page.

```javascript
loop (
 if[Browser.input.e.pressed] ( Print("E pressed"): ) else ()
 if[Browser.input.e.released] ( Print("E released"): ) else ()
 // Browser.input.e.held is the current held state.
 // Browser.pointer.xpos / ypos are viewport coordinates.
 wait(100ms):
)
```

Press/release counters are consumed once per task and property-read instruction.
A quick down/up between reads is retained; multiple queued edges are consumed
over successive reads. New tasks do not receive edges predating their creation.
Blur releases held keys. Use an empty `else` when a false condition should
continue execution: Z#'s existing no-else conditional is a function guard.

`Browser.platform` is best effort and returns null when unknown. Examples are
`win11x8664`, `win10arm64`, `linuxx8664`, `macarm64`, `iosarm64`,
`androidarm64`, `zosx8664`; without available details it returns `windows`,
`linux`, `mac`, `ios`, `android` or `zos`. Version/architecture suffixes are
included only when available; reduced user-agent information is not a reliable
way to distinguish Windows 10 from 11. Platform is informational, not a security
or capability check.

### HTTP / HTTPS

```javascript
HTTP.request(
 name: "News":
 method: "GET":
 url: "https://example.com/news":
 timeout: 10s:
 headers[JSON] ( {"Accept":"application/json"} )
 onResponse: [Main:Main:Loaded]:
 onError: [Main:Main:Failed]:
):

noticed brain Loaded[number Status, text Body] (
 Print("HTTP " + Status + " " + Body):
 text Headers = HTTP.responseHeaders("News"):
)
noticed brain Failed[text Message] ( Print(Message): )

HTTP.requestCancel("News"):
```

`url`, `onResponse` and `onError` are required. Other fields are optional.
Default method is GET, timeout is 30 seconds, and omitted names are generated.
Use a name for cancellation/header lookup. POST/PUT/PATCH/DELETE may include
`body: "text":`; GET/HEAD cannot have a nonempty body. Headers may also be a
JSON-text expression. Supported methods: GET, POST, PUT, PATCH, DELETE, HEAD,
OPTIONS. Timeouts use duration literals, between 1 ms and 300 seconds.
HTTP status errors such as 404 still invoke onResponse; transport failures,
timeouts and cancellation invoke onError. `requestionCancel` is a compatibility
alias for `requestCancel`. Response headers are returned as JSON text.

Requests are asynchronous. Callbacks use normal file/room/function visibility
rules and the signatures shown above. Native callbacks run on the owning Z#
task thread, not I/O threads. Active network services keep console scripts alive
after Start returns. They are cancelled during task/window shutdown. Long
blocking native calls delay callback delivery. Desktop cancellation suppresses
the request result immediately; the background socket may remain until timeout.

Both runtimes allow at most 32 active requests, 1 MiB text request/response bodies,
and no credentials embedded in URLs. Browser requests obey CORS, mixed-content
and browser TLS rules and omit cookies. Desktop uses its bundled CPython standard
library with certificate verification; relative URLs are browser-only.

### Named WebSockets

```javascript
Websocket.connect("wss://example.com/socket", "Game"):
Websocket.connected("Game", [Main:Main:Opened]):
Websocket.callback("Game", [Main:Main:Received]):
Websocket.error("Game", [Main:Main:SocketError]):
Websocket.closed("Game", [Main:Main:Closed]):

noticed brain Opened[] ( Websocket.send("Game", "Hello"): )
noticed brain Received[text Message] ( Print(Message): )
noticed brain SocketError[text Message] ( Print(Message): )
noticed brain Closed[number Code, text Reason] ( Print("Closed " + Code): )

text State = Websocket.state("Game"):
Websocket.close("Game"):
```

States are `connecting`, `open`, `closing`, `closed`. Register callbacks directly
after connecting; use connected before sending. Connections accept ws/wss and
text messages only, with a 1 MiB message/send-buffer limit and at most 32 active
sockets. Duplicate active names and sending before open are errors. Names can
be reused after closure. Browser sockets obey browser security restrictions.
Connections/request names belong to the running task's networking context;
they are not process-global or shared between independent Start tasks.

HTTP response bodies remain text. Browser project-asset JSON schema loading
is described in [Browser JSON and project asset reads](#browser-json-and-project-asset-reads-1300).
Existing desktop JSON syntax is unchanged.

## Time (1.3.0.0, desktop and browser)

Time methods return expression values; they do not print automatically. No import
is required. Projects using them must declare `ZSharp: [1.3.0.0]:` or newer.

```javascript
number Stamp = Time.convert("1/1/26", "00:00", UTC):
text Elapsed = Time.since(Stamp):
text Remaining = Time.until("1790000000"):
text FromMilliseconds = Time.since(1767225600000, "ms"):
Print(Time.since(Time.convert("2026-01-01", "00:00:00", UTC))):
```

`since` and `until` are intent-based aliases: either returns `"... ago"` for a
past timestamp or `"in ..."` for a future timestamp. Durations use days, hours,
minutes and seconds, with singular/plural names and zero units omitted. For
example: `"1 day, 2 hours, 3 minutes, 50 seconds ago"`. Days are elapsed 24-hour
periods, not calendar months/years. Equal timestamps return `"0 seconds ago"`;
sub-second differences round down to `"0 seconds ago"` or `"in 0 seconds"`.
Seconds are the default unit; optional `"s"` or `"ms"` explicitly selects units.
Timestamps may be text or numbers. Second timestamps support millisecond
fractions; millisecond timestamps must be whole milliseconds. Invalid timestamps
and values outside Gregorian years 1..9999 are errors, not guessed units.

`Time.convert(Date, Clock, Zone)` returns a numeric Unix timestamp in seconds.
Dates accept **month/day/year** (`M/D/YY`, `M/D/YYYY`) or `YYYY-MM-DD`.
Two-digit years always mean 2000..2099. Clock text uses 24-hour `HH:MM` or
`HH:MM:SS`. Leap years are checked; invalid dates, times and leap seconds are
rejected rather than normalized. Zones are `UTC`, `LOCAL`, or quoted fixed
offsets such as `"UTC+05:30"` and `"UTC-04:00"` (maximum offset 14 hours).
`UTC` and `LOCAL` may be bare identifiers in this argument position; zone text
variables also work. Abbreviations such as EST and named IANA zones are not
supported. Nonexistent/ambiguous daylight-saving local times are rejected;
use UTC/an explicit offset to disambiguate. Historical local dates may also be
limited by the host OS's timezone facilities. UTC conversion is independent of
the machine's timezone.

All getters use the machine/browser's **local timezone**:

| Expression | Returned value |
| --- | --- |
| `Time.getYear()` | Number, e.g. 2026 |
| `Time.getMonth()` | Number, 1..12 |
| `Time.getDay()` | Number, day of month 1..31 |
| `Time.getWeek()` | English weekday text, e.g. `"Monday"` |
| `Time.getHour()` | Number, 0..23 |
| `Time.getHour().formatted` | 12-hour text, e.g. `"2 PM"` or `"12 AM"` |
| `Time.getMinute()` | Number, 0..59 |
| `Time.getSecond()` | Number, 0..59 |
| `Time.getMS()` / `Time.getMS(1)` | Number, tenths digit 0..9 |
| `Time.getMS(2)` | Number, hundredths 0..99 |
| `Time.getMS(3)` | Number, milliseconds 0..999 |

At `.427` seconds the fractional values are `4`, `42`, and `427`. They are
numbers, not zero-padded strings, and do not round up. Precision must be 1..3;
no extra clock precision is invented. Each call reads the current system clock,
so multiple calls can straddle a second/day boundary. Wall-clock adjustments
affect relative results; these APIs are not monotonic performance timers.

## Development preview: input submission (1.2.2.0)

Native text inputs may declare `Submit[Chat:Chat:Send]:`, using the same callback
path rules as button clicks. On the Windows development backend, Enter invokes
this callback without inserting a newline; Shift+Enter inserts a newline when
`multiline: alive:` is enabled. Without Submit, existing Enter behavior is
unchanged. Holding Enter does not repeatedly submit. Linux/macOS submission
handling is not implemented yet.

`ZSHARP_C_FUNCTION_DURATION_OPTION` (kind 4) is a C-extension function option
accepting exactly one nonnegative duration literal, such as `Demo.Cooldown(5s):`
or `Demo.Cooldown(5ms):`. Like text options, it appears before executable
statements in a custom function declaration. Its binder receives the callback
target followed by a NUMBER measured in milliseconds, once during initialization.
Seconds are converted using the same decimal arithmetic as `wait`.

C extensions may also register `ZSHARP_C_FUNCTION_OPTION` (kind 3) through
`add_declaration`. Its name is a qualified identifier, for example `Demo.Option`.
Inside a custom function declaration, `Demo.Option("one", "two"):` must precede
executable statements and takes one or more literal texts. The option binder
runs after the function binder during initialization, receiving the function's
`File:Room:Function` target followed by the texts. Options are not executed as
part of the callback body and are preserved in bytecode. Extensions validate
their own option semantics; ordinary brains do not accept these declarations.

## C-defined declarations and background services (1.2.1.0)

C modules may export `zsharp_c_register_v2(ZSharpCSyntaxRegistryV2 *, char *, size_t)`.
Its `v1` member retains the existing statement, block and expression registry.
`add_declaration(context, name, kind, binder, error, error_size)` adds a new
function declaration type (`ZSHARP_C_FUNCTION_DECLARATION`) or a named room-level
configuration block (`ZSHARP_C_NAMED_BLOCK_DECLARATION`). Existing built-in types
cannot be replaced. Modules with only the v1 hook continue to work.

```javascript
noticed custom_event Handler[text Message, number Count] (
 Print(Message):
)

CustomConfig Settings (
 label: File.read("label.txt"):
 nested ( value: Math.pow(2, 3): )
):
```

The module must register `custom_event` and `CustomConfig`; they are not built-ins.
Function declarations retain normal Z# parameters and bodies, and do not run
automatically even if named `Start`. Before startup, their binder receives the
declaration name, `File:Room:Function` target, then alternating parameter name/type
texts. Named configuration binders receive their name followed by alternating
dotted field paths and evaluated scalar values. Nested blocks flatten to paths
such as `nested.value`. Hex colors and literal text/number lists are passed as text.
Lists use JSON text. Configuration expressions are evaluated at runtime, not while
the compiler checks source. They are preserved in bytecode.

An optional `zsharp_c_service_v1` hook supports queued callbacks and host lifetime.
See `native/include/zsharp_c.h` for ACTIVE/POLL/COMPLETE/SHUTDOWN operations.
POLL receives the owning source name in `event.target` and must only return events
for that owner. The returned target must belong to an owned custom function
declaration; parameter counts and types are checked by ZVM. Worker threads queue
events; ZVM task threads execute the bodies. Callbacks are serviced at instruction
boundaries and after `Start[]` returns. Active services keep console scripts alive;
closing a game/window cancels its tasks and shuts down their services. Pending
callbacks are not serviced while a Z# task is blocked inside a native call.

DiscordZ uses this extension for `discord_command_hybrid`, `discord_command_slash`,
`discord_command_msg`, named `Webhook` blocks and `Discord.Print`. These are
dependency-provided syntax, not built-in Z# features. Import `c:discordz.C.Main()`
and rebuild DiscordZ against the 1.2.1 development C header before using them.

This guide explains how to write Z# and compares its concepts with C#, Java, and C. The official source extension is `.zsharp`.

This edition is organized for Z# 1.2.0.0 around five areas: getting started, the Z# language, apps, games, and language interoperability. Features that were previously documented as version-specific additions are now placed with the part of Z# they belong to.

For the underlying design record, see [LANGUAGE.md](LANGUAGE.md). For bytecode details, see [BYTECODE.md](BYTECODE.md).

## Contents

- [Getting Started](#getting-started)
- [Z# Language](#z-language)
- [Apps](#apps)
- [Games](#games)
- [Languages](#languages)

---
# Getting Started

## Files and project layout

Every Z# project has one `project.zsettings` file in its root. Normal source
files end in `.zsharp`.

A small project can look like this:

```text
MyProject/
 project.zsettings
 Main.zsharp
 Math.zsharp
 assets/
```

Normal scripts begin with:

```zsharp
zsharp = type.script
```

The settings file begins with:

```zsharp
zsharp = type.settings
```

Project metadata may include an optional Hub icon:

```zsharp
Project: "My App":
PID: "my_app":
Version: [1.0.0.0]:
Authors: ["Author"]:
Description: "A Z# app":
Icon: "assets/icon.png":
ZSharp: [1.0.2.1]:
```

`Icon` accepts a project-relative PNG. Paths use `/`
and cannot leave the project. If `Icon` is omitted, the Hub downloads and
caches the official Z# logo from `https://www.zsharp.zombieos.com/zsharp.png`.
The same project icon is used for Desktop shortcuts created by Z#; a window's
design icon remains the fallback for older projects without `Icon`.

## Project settings

A complete basic settings file is:

```zsharp
zsharp = type.settings

Project: "Project Display Name":
PID: "project_id":
Version: [1.0.0.0]:
Authors: ["Author1", "Author2"]:
Description: "This is a Z# Project!":
ZSharp: [1.0.2.1]:

Dependencies (
 playfab:1.0.0.0
):
```

The fields mean:

- `Project` is the human-readable display name.
- `PID` is the unique project identifier.
- `Version` is the project's four-part version.
- `Authors` lists any number of authors.
- `Description` describes the project and uses `\n` for new lines.
- `ZSharp` selects the language version and generation.
- `Dependencies` lists `projectId:projectVersion` pairs.

Normal PIDs are lowercase and may contain numbers and underscores. Spaces in a
PID are normalized to underscores during compilation.

Reserved official projects are the exception to lowercase import names. Their
names use official capitalization, such as:

```zsharp
import ZSharp.Window.Button():
import ZOS.Cloud.Saves():
```

Third-party projects cannot claim reserved official names such as `ZSharp` or
`ZOS`.

## Application startup

A native Z# window application declares its startup window in settings:

```zsharp
Window (
 Startup: "Windows/Startup.zsharp":
 Uninstall: "Windows/Uninstall.zsharp":
):
```

`Startup` is required when a `.zapp` uses native Z# windows. `Uninstall` is
optional; omit it when the application does not need a custom uninstall UI.

An application backed by an existing native executable may use platform
targets instead of a Z# startup window:

```zsharp
Native[JSON] (
 [
  {
   "platform": "windows-x86_64",
   "start": "Engine/ZSharpEngine.exe"
  },
  {
   "platform": "linux-x86_64",
   "start": "Engine/zsharp-engine"
  }
 ]
):
```

Supported target IDs are `windows-x86_64`, `windows-aarch64`,
`linux-x86_64`, `linux-aarch64`, `macos-x86_64`, and `macos-aarch64`. Paths
are project-relative and must name existing files. ZVM selects the current
platform, starts the executable with its own directory as the working
directory, forwards launch arguments, waits for it to exit, and records its
playtime. A package with no matching target reports the missing platform
instead of attempting to run a different build.

The supported file headers are:

```zsharp
zsharp = type.script:window
zsharp = type.script:audio
zsharp = type.scene
zsharp = type.object
zsharp = type.settings
```

Normal game logic remains `type.script`. Each `.zscene` file uses `type.scene`,
each `.zobject` file uses `type.object`, and each `.zaudio` source uses
`type.script:audio`. The old `type.script:2D`,
`type.script:3D`, `type.object:2D`, and `type.object:3D` headers are no longer
valid source syntax.

## Application and game packages

### Package exclusions: `.zignore`

From Z# 1.2.1.0, place an optional UTF-8 `.zignore` in the project root.
It filters files before hashing and source validation for `.zapp`, `.zgame`,
their source companions, and `.zpackage` dependencies. It never deletes source
files or affects `zsharp run`/imports in your working project. `.gitignore` does
not control packaging. `.zignore` itself is always omitted from archives.

```text
# Secrets and source-only examples
.env
.env.*
/Z# Examples/
*.obj
__pycache__/
Assets/**/*.cache
!Assets/important.cache
```

Rules are ordered; the last matching rule wins. Blank lines and lines beginning
with `#` are ignored. `!pattern` re-includes a matching path; `\#` and `\!` match
literal leading characters. `/` anchors to the root, and patterns containing
`/` are root-relative. Names without `/` match at any depth. A trailing `/`
matches directories only. `*` and `?` do not cross `/`; `**` can cross directories
and `**/` also matches zero directory levels. Bracket classes/ranges such as
`[a-z]` and `[!0-9]` are supported. Backslashes escape literal characters;
use forward slashes for path separators. Unescaped trailing spaces are ignored.
Matching is case-sensitive on all systems; wildcard matching is byte-oriented.

This supports common Git-style patterns, not every Git ignore feature: only
the root `.zignore` is read, and existing built-in exclusions cannot be undone.
Excluded directories are not traversed, so re-include a parent before trying
to re-include its children. Settings and configured startup/native targets may
not be excluded; packaging fails with a clear error. Do not exclude runtime
imports, objects or assets your application needs. Rules are limited to 1 MiB,
10000 entries and 4096 bytes per pattern; oversized, NUL-containing or unreadable ignore files fail
packaging rather than silently including their contents.

### Source dependency packages

From Z# 1.2.1.0, reusable project source can be distributed as a `.zpackage`:

```text
zsharp package pack path/to/library DiscordZ
```

This creates `path/to/library/Packages/DiscordZ.zpackage`. Packs are always
source-only: no compiled Z# bytecode and no `--unbytecode`/`--unbytecoded`
option. A library needs `project.zsettings`, but does not need an application
startup window or a game scene. Native C/C++/Rust/Kotlin modules still need
their compiled platform libraries alongside their source; a pack does not
automatically compile those languages.

Declare a Store project ID and four-part release version in the consuming
project's settings:

```zsharp
Dependencies (
 discordz:1.0.0.0:
):
```

The trailing `:` after each release is optional for compatibility with older
settings. Both `zsharp run path/to/Bot.zsharp` and app/game packaging prepare
dependencies **before parsing source**, including imported custom C syntax:

- First look for a matching `.zpackage` directly inside `Dependencies/`.
  Its internal PID/version must match; the filename can be anything.
- If absent, download
  `https://zos-store-api.zos-store-api.workers.dev/?action=download&project=PROJECT_ID&release=RELEASE_ID`
  to `Dependencies/PROJECT_ID-RELEASE_ID.zpackage`.
- Validate the source archive and install it under
  `Dependencies/PROJECT_ID/RELEASE_ID/`. Existing matching installations are
  reused. Concurrent runs coordinate installation rather than sharing a
  partially downloaded file.

Corrupt local archives and multiple archives matching the same PID/release
are errors, not reasons to silently download a replacement. Releases are
immutable: replacing an already-installed release with different archive
contents reports an error; move its installed folder aside first. Downloads
are limited to 1 GiB and use HTTPS. Transitive dependencies are prepared too;
cycles and excessively deep/large dependency graphs are rejected.

Use normal project-qualified imports and calls, for example:

```zsharp
import discordz.Scripts.Client():
import c:discordz.C.Main():

Function.call(discordz:Client:Client:Initialize):
Function.call(c:discordz.C.Main:initialize []):
```

These are examples of library API names, not built-in DiscordZ functions.
Within the library, its own project-relative paths and imports stay relative
to that library. Dependency files are isolated from the consuming project's
source lookup, so both projects may have a `Main.zsharp`. Dependency `Start[]`
functions do not automatically run as application/game startup tasks; call
initialization explicitly.

App/game packages include the installed dependency source/assets, without
duplicating the `.zpackage` archives. Packaged applications therefore do not
need a Store download for those included dependencies. Built-in runtime
dependencies (`zsharp`, `zsharpwindow`, `zsharpgame`, `zos`) and explicitly
supplied native `--provider` projects keep their existing behavior.

Dependencies are executable code, including any native libraries or custom
syntax modules they contain. Archive validation checks identity, paths and
integrity; it is not a safety review or a publisher signature. Only use trusted
dependencies. A `.zpackage` is not a runnable `.zapp` or `.zgame`.

Z# applications use `.zapp` and games use `.zgame`.

These are cross-platform Z# container formats. The normal 1.0.2.1 container
stores the validated project plus its compiled startup, with a SHA-256 hash for
each entry. The unbytecoded companion uses the standard ZIP container and ZIP
CRC checks. The runtime rejects corrupt data, absolute paths, `..` traversal,
and symbolic-link/reparse-point content.

A package can contain:

```text
project.zsettings
assets/
source files
```

Build packages with:

From 1.2.1.0, game packaging shares one freshly loaded scene/object validation
model across script checks and startup compilation for each output. All scene,
object, script, and package integrity checks still run. This is build-local
reuse, not a persistent cache: changes are checked again on the next build.
Set the environment variable `ZSHARP_PACKAGE_PROFILE=1` to print the validation
model-load count and placement count while diagnosing packaging performance.

```text
zsharp package app path/to/project Application
zsharp package game path/to/project Game
zsharp package app path/to/project Application --unbytecode
zsharp package game path/to/project Game --unbytecode
```

Z# adds the extension and puts the result in the project's `Packages` folder:

```text
path/to/project/Packages/Application.zapp
path/to/project/Packages/Game.zgame
path/to/project/Packages/Application-unbytecoded.zapp
path/to/project/Packages/Game-unbytecoded.zgame
```

The last argument is a filename, not a path, and must not include `.zapp` or
`.zgame`. Without `--unbytecode`, one normal bytecoded package is created. With
the option, Z# creates both the normal package and the
`-unbytecoded` companion. The companion is a standard ZIP-compatible source
archive. Rename `Application-unbytecoded.zapp` to
`Application-unbytecoded.zip` to browse the original project files. Renaming
does not change their contents.

The packager checks `project.zsettings` and validates every included `.zsharp`,
`.zscene`, `.zobject`, and `.zaudio` file. An app requires a configured `Window Startup`;
a game requires `zsharpgame:1.0.0.1`, at least one normal `type.script` file,
and at least one `type.scene` `.zscene` file. The first source and scene in
sorted path order are used as the startup script and initial scene. Renaming an
ordinary ZIP file is not enough; Z# source packages carry a format marker and
must be produced by `zsharp package --unbytecode`.

The normal package launches an embedded, integrity-checked startup bytecode.
The unbytecoded companion launches its validated `.zsharp` startup. Both forms
use the same project metadata, package cache, application window behavior, and
uninstall flow.

Open or uninstall one with:

```text
zsharp run path/to/project/Packages/Application.zapp
zsharp run path/to/project/Packages/Game.zgame
zsharp uninstall path/to/project/Packages/Application.zapp
zsharp uninstall path/to/project/Packages/Game.zgame
```

File associations can pass a package directly to the runtime, so
`zsharp Application.zapp` and the older `zsharp open Application.zapp` alias
are equivalent to `zsharp run Application.zapp`.
Install or refresh the current user's associations with:

```text
zsharp associate
```

On Windows, association also registers and starts the single-instance Z# tray
updater for the current user. It checks once after sign-in and then hourly,
notifies before a newer verified ZVM is installed, and provides **Check for
updates** and **Exit** tray-menu actions. Version comparison is numeric across
all four parts, so an older manifest never downgrades a newer installation.
Linux and macOS keep the quiet launch-time update check.

Opening an associated package or a Z#-created Desktop shortcut launches the app
without opening a terminal. On Windows, Explorer launches are detached from the
console; Linux desktop entries use `Terminal=false`; and macOS launcher apps run
Z# in the background. Explicit `zsharp open` and `zsharp run` commands continue
using the terminal in which they were entered. Existing Z#-created shortcuts
are upgraded to the silent launch form the next time their package runs.

Opening an application verifies every entry and extracts it to a private,
content-addressed cache before running the configured startup window. On its
first successful installation, the runtime asks whether to create a Desktop
shortcut. The shortcut launches that package through Z# and uses the startup
window's design icon when the platform can convert or use it.

If the startup window cannot launch, the Z# Hub opens with:

```text
APPNAME failed to launch!
EXACT FAILURE REASON
```

Uninstall asks the user to type `yes`, then permanently removes that verified
package cache, the selected package file, and any Z#-created Desktop shortcut
without using Recycle Bin or Trash.
The future app-data ledger and optional ZOS Cloud backup are not part of
1.0.2.1. See [UNINSTALL.md](UNINSTALL.md) for current behavior and the planned
full safety model.

`.zgame` uses the same secure package, cache, association, shortcut, and
uninstall foundation as `.zapp`. Opening one launches its normal game startup
script, loads and validates the project's separate `.zscene`, `.zobject`, and
`.zaudio` files, starts every
eligible non-`DR` `Start[]` task, and enters the SDL3/Vulkan game loop. Windows
and Linux are the supported game targets for 1.0.2.1. The macOS/MoltenVK game
path is experimental and is not advertised as supported until hardware tests
are completed.


---

# Z# Language

## Comments and terminators

`//` begins a comment that continues to the end of its line:

```zsharp
noticed text Message = "Hello": // This is a comment.
```

Statements end with `:` rather than `;`:

From Z# 1.2.0.1, `Print(value).update:` replaces the current unfinished console
line without adding a newline. Use it for progress or live input displays:

```zsharp
Print("Loading: 10%").update:
Print("Loading: 100%").update:
Print("Loaded!"):
```

Shorter replacements erase leftover text. Ordinary `Print(value):` finishes
the current line with a newline; completed earlier lines are not editable.
Updates flush immediately and are replayed by `zsharp terminal` connections.
Use single-line values that fit the console width; this is not a multiline
terminal UI. Output from concurrent tasks shares one current line, with each
print operation synchronized to avoid interleaving its bytes.

```zsharp
Print("Hello"):
```

Comparison:

```csharp
Console.WriteLine("Hello"); // C#
```

```java
System.out.println("Hello"); // Java
```

```c
printf("Hello\n"); /* C */
```

```zsharp
Print("Hello"): // Z#
```

Parenthesized declaration bodies use `(` and `)`:

```zsharp
noticed room Main[] (
 // Members go here.
)
```

## Rooms

A `room` is Z#'s class-like container:

```zsharp
noticed room Player[] (
 noticed text Name = "Zombie":
)
```

The rough equivalents are:

| Z# | C# | Java | C |
|---|---|---|---|
| `room` | `class` | `class` | No direct equivalent; often a `struct` plus functions |
| `noticed` | `public` | `public` | External linkage/API declaration |
| `silent` | `private` | `private` | File-local or hidden implementation |
| `horde` | `static` | `static` | `static` has related but context-dependent meanings |

A room can have one of three visibility levels:

```zsharp
noticed room PublicRoom[] (
)

room FileOnlyRoom[] (
)

silent room PrivateRoom[] (
)
```

- `noticed room` is accessible throughout the project when imported.
- A bare `room` is accessible only to rooms in the same file.
- A nested `silent room` is accessible only to itself and its parent.
- A top-level `silent room` cannot be called by another room.

Rooms can be nested:

```zsharp
noticed room Parent[] (
 silent room Child[] (
  noticed text Message = "Only Parent can reach this room":
 )
)
```

## Values and types

#### Text

`text` stores text values:

```zsharp
noticed text Name = "Zombie":
text LocalMessage = "Hello":
```

It is closest to C# `string` and Java `String`. C normally represents text
with a character array or pointer.

Text concatenation uses `+`. Numbers and statuses are converted to their
printable text when either side is text:

```zsharp
text Message = "Hello, " + Name:
text Position = "L" + CurrentLine + ":C" + CurrentColumn:
```

#### Numbers

`number` stores an arbitrarily sized ordinary decimal:

```zsharp
noticed number Visits = 10:
noticed number Price = 99.25:
```

Unlike C# `int`, Java `int`, or C `int`, a Z# number is not limited to a fixed
32-bit range and may contain a decimal point. It is conceptually closer to an
arbitrary-precision decimal type.

Scientific notation is not valid:

```zsharp
number Valid = 10000000000:
number Invalid = 1e+10: // Compile error.
```

Arithmetic operators are:

```zsharp
number Add = 4 + 6:
number Subtract = 10 - 3:
number Multiply = 4 * 5:
number Divide = 8 / 2:
number Remainder = 10 % 4:
number Negative = -5:
```

Terminating division preserves its needed digits. Repeating division keeps
only the first fractional digit:

```zsharp
Print(3 / 1): // 3
Print(1 / 3): // 0.3
```

`addition(...)` is the confirmed built-in addition form:

```zsharp
number Result = addition(4 + 6):
```

#### Status

`status` is the two-state type:

```zsharp
noticed status Alive = alive:
Alive = dead:
```

| Z# | C# | Java | C |
|---|---|---|---|
| `status` | `bool` | `boolean` | `_Bool`/`bool` |
| `alive` | `true` | `true` | `true`/nonzero |
| `dead` | `false` | `false` | `false`/zero |

#### Null

The missing-value literal is `null`:

```zsharp
noticed text Missing = null:
```

#### Default field values

A room field may omit its initial value:

```zsharp
noticed room Player[] (
 noticed text Name:
 noticed number X:
)
```

## Visibility

`noticed` means code outside the current scope may access the declaration:

```zsharp
noticed text PublicMessage = "Visible":
```

`silent` restricts access:

```zsharp
silent text Secret = "Hidden":
```

From 1.4.0.0, a room member without `noticed` or `silent` inherits its
containing room's visibility. This applies to variables and functions,
including `brain`, number-returning and text-returning functions:

```zsharp
noticed room Settings[] (
 text Title = "My application": // Public through this noticed room.
 silent text Secret = "Hidden": // Explicitly private instead.

 brain Start[] (
  Print(Title):
 )
)
```

Members in a `noticed room` default to public; members in a `silent room`
default to private. Members in a bare, file-only `room` follow that room's
same-file accessibility. Explicit `noticed` or `silent` overrides the member
default, but does not bypass the containing room's visibility or required
imports. Bare room declarations keep their existing file-only visibility.

Variables declared inside a function are local instead: they do not inherit
the room's public/private visibility. Automatic `Start[]` execution is
independent of member visibility; use `[DR]` to disable it.

## Functions

#### Brain functions

`brain` is Z#'s no-declared-return-type function form, similar to `void`:

```zsharp
noticed brain SayHello[] (
 Print("Hello"):
)
```

Comparison:

```csharp
public void SayHello() { }
```

```java
public void sayHello() { }
```

```c
void say_hello(void) { }
```

```zsharp
noticed brain SayHello[] (
)
```

Parameters go inside `[]`:

```zsharp
noticed brain Move[number x, number y] (
 Print(x):
 Print(y):
)
```

#### Number-returning functions

Use `number` in place of `brain`:

```zsharp
noticed number Add[number Left, number Right] (
 feed(number.Left + number.Right):
)
```

`feed(value):` supplies the result:

```zsharp
feed(number.Left + number.Right):
```

#### Text-returning functions

Use `text` in place of `brain`:

```zsharp
noticed text Greeting[text Name] (
 feed("Hello, " + Name):
)
```

#### Returning without a value

`feed:` ends the current function without a value:

```zsharp
noticed brain StopEarly[] (
 feed:
 Print("This does not run"):
)
```

#### Start functions

A `brain` named `Start` runs automatically:

```zsharp
noticed brain Start[] (
 Print("Started"):
)
```

`Start[DR]` disables automatic execution:

```zsharp
noticed brain Start[DR] (
 Print("Runs only when explicitly called"):
)
```

For a window application, the ZVM creates the startup window and then runs the
non-`DR` `Start` brain in every normal project script. This is application
startup behavior, not a call made by the window file. A window invokes an
event function only for a configured button click.

Each automatic start runs as an independent ZVM task. An endless `loop` in one
startup brain therefore does not prevent the other scripts from starting.
Internal button callbacks are tasks too, so a long-running callback does not
freeze the native window event loop. Closing the window cancels and joins its
tasks before the ZVM releases their memory.

Primitive room fields keep their current values between button presses. Calls
from the same source room run in order, so a counter used by a click handler is
not reset or updated by two overlapping presses. Different rooms can still run
at the same time.

#### Timing statements

`wait` pauses the current brain. Durations use `ms` for milliseconds or `s`
for seconds and may be ordinary Z# decimals:

```zsharp
wait(5ms):
wait(1.5s):
```

`delay` is an exact alias:

```zsharp
delay(0.25s):
```

Durations cannot be negative. The current runtime accepts up to seven days in
one statement. During a window callback, native window events and redraws keep
running while the callback waits.

#### Function calls

Ordinary declared functions are called with `Function.call(...)`:

```zsharp
Function.call(Home:Main:Start_Outcome):
```

Arguments appear in `[]` after the target:

```zsharp
number Result = Function.call(Math:Calculator:Add [4, 6]):
```

Call paths expand by location:

```zsharp
Function.call(File:Room:Function):
Function.call(Project.File.Room.Function):
```

The file search is recursive and considers only `.zsharp` files. Ambiguous
duplicate filenames produce an error.

Instance methods use the object method form:

```zsharp
User.Move[10, 20]:
Room.User.Move[10, 20]:
File.Room.User.Move[10, 20]:
Project.File.Room.User.Move[10, 20]:
```

#### Callback variables and function references (1.2.1.0)

`Function.call` also accepts a variable name. Room fields, local values, and
function parameters may supply either `text` or the dedicated `function` type:

```zsharp
noticed text TextCallback = "Actions:Actions:Move":
noticed function MoveCallback = Actions:Actions:Move:

noticed brain Forward[text CallBack] (
 Function.call(CallBack [10, "hi"]):
)

noticed brain ForwardTyped[function CallBack] (
 Function.call(CallBack [10, "hi"]):
)

noticed brain Start[] (
 text LocalText = "Actions:Actions:Move":
 function LocalFunction = Actions:Actions:Move:
 function Copy = LocalFunction:
 Function.call(LocalText [10, "hi"]):
 Function.call(Copy [10, "hi"]):
)
```

These declarations belong inside a room; import the target file normally in
every room that binds or invokes its callbacks. Defining a reference does not
execute it. Dedicated references bind qualified targets and are checked at
build time for existence, imports, and visibility; their parameter count/types
are checked when invoked. Local `function` declarations can copy an existing
function reference. Text targets are resolved at runtime and accept
`File:Room:Function`, `File.Room.Function`, a four-part project-qualified target,
or an existing foreign target such as `lua:Lua.Utilities:add`. Unquoted dedicated
references use `File:Room:Function`, `Project.File.Room.Function`, or the normal
`LANGUAGE:Path.To.File:function` spelling.

Omit the argument brackets for zero arguments: `Function.call(CallBack):`.
Returning functions also work in expressions, for example
`number Total = Function.call(CallBack [4, 6]):`, and existing named brain
outcome selectors still work. Invalid/empty targets, incorrect argument types
or counts, missing imports, and inaccessible targets report errors; callback
variables do not bypass visibility rules. A `function` parameter requires an
actual function reference, not ordinary text. This is a named-function
reference, not a closure capturing local variables or an object method.

`noticed brain Start[text CallBack]` is valid, but requires an explicit call
with its argument. Only zero-parameter `Start[]` brains automatically run.
The canonical spelling remains `Function.call`, with lowercase `call`.

## Local values and assignment

Local values omit visibility and exist only during that brain call. Room
fields are declared outside functions, may include `noticed` or `silent`
(or inherit their room's visibility), and keep their value for later calls:

```zsharp
noticed brain Start[] (
 number Score = 10:
 text Name = "Zombie":
 status Alive = alive:
)

noticed number SharedScore = 10:
```

Direct assignment works for a local value:

```zsharp
Score = 15:
Name = "Updated":
Alive = dead:
```

Local `status` declarations accept `alive`, `dead`, or an expression returning
a status, such as `status Ready = Score >= 10:`. Later direct assignments must
also supply a status; text and numbers are not implicitly converted to
`alive`/`dead`. Like other locals, a local status is private to that call and
retains its value across `wait(...)` in the same task.
Projects using local status declarations must set `ZSharp: [1.4.0.0]:` or
newer, because their typed local stores use the newer bytecode instruction.

Typed assignment works for both locals and room fields. If a local shadows a
room field with the same name, the local is changed:

```zsharp
noticed number Score = 10:
number.set:Score = Score + 5:
```

A text field on the current object is changed with `text.set`:

```zsharp
text.set.Name = Name + "!":
```

#### Random values

Z# provides random integers, decimals, and percentage chances without an
import:

```zsharp
number Die = random.number(1, 6):
number Offset = random.decimal(-1, 1):
status Critical = random.chance(15):
```

`random.number` includes both endpoints. `random.decimal` uses the supplied
bounds, and `random.chance` accepts a percentage from `0` through `100`.

A named object's field uses `.set`:

```zsharp
User.set.X = User.X + x:
User.set.Y = User.Y + y:
```

Qualified writes are allowed when visibility, imports, and provider permissions
allow them:

```zsharp
File.Room.User.set.Name = "Zombie":
Project.File.Room.User.set.X = 25:
```

### Persistent room fields

Room-level `text` and `number` values persist between repeated brain calls.
Use `text.set.Name = ...:` and `number.set:Name = ...:` to update them. Local
values remain local to the brain call.

In app/game tasks, `wait(...)` also lets callbacks in the same room run. Scalar
room fields (`text`, `number`, `status`, and `function`) are saved before the
wait and refreshed when that task resumes. Local values and function parameters
retain their own values across the wait. Include waits in long-running polling
loops; a task that never yields can still delay other callbacks in its room.

## Conditions

### Shutdown cleanup (1.2.1.0)

An optional `noticed brain Shutdown[] (...)` runs during graceful shutdown.
For apps and games, ZVM stops and joins ordinary tasks first, then calls every
eligible `Shutdown[]` in project script files, including files without a
`Start[]`. Hooks run sequentially once, retaining the room's saved values.
`Shutdown[DR]` disables automatic cleanup. Console scripts run their shutdown
brains when execution finishes. No hooks are required.

```zsharp
noticed brain Shutdown[] (
 Function.call(cpp:Cpp.Discord.Discord:shutdown []):
)
```

Cleanup may print, update ordinary variables, and call native modules. It must
finish promptly; do not run endless loops or access closing window/game
properties. Cleanup does not receive the cancelled window runtime. A failed
hook is reported, but other hooks are still attempted. Force-ending the
process, power loss, or a native crash cannot guarantee cleanup. A native call
that never returns can prevent tasks from joining and reaching cleanup.

Conditions use square brackets:

```zsharp
if[Visits >= 10] (
 Print("Returning visitor"):
) else (
 Print("New visitor"):
)
```

Chain additional conditions with `else if`. Conditions are checked in order;
only the first matching branch runs. Spaces before `[` are optional.

```zsharp
if [Visits >= 10] (
 Print("Returning visitor"):
) else if [Visits >= 1] (
 Print("Welcome back"):
) else (
 Print("New visitor"):
)
```

Any number of `else if` branches can precede the optional final `else`.
As with a plain `if`, if no condition matches and the chain has no final
`else`, the current function returns early. Use `else ()` to continue instead.

Comparison operators include:

```text
==  !=  >  >=  <  <=
```

Logical operators are words:

```zsharp
if[Alive and Health > 0] (
)

if[Health == 0 or Health <= 20] (
)

if[not dead] (
)
```

An unusual Z# rule is that an unmatched `if` with no `else` ends the current
function early:

```zsharp
noticed brain OnlyWhenAlive[] (
 if[Alive] (
  Print("Alive"):
 )
 Print("This runs only when Alive was met"):
)
```

Add an `else`, even an empty one, when normal execution should continue:

```zsharp
if[Alive] (
 Print("Alive"):
) else (
)
Print("Continues either way"):
```

## Named brain outcomes

A brain can expose named conditional outcomes:

```zsharp
noticed brain Choose[number Value] (
 if(Score)[Value > 0] (
  feed(Value + 1):
 ) else (
  feed("not positive"):
 )
)
```

The caller selects the outcome after the call:

```zsharp
number Score =
 Function.call(Decisions:Choice:Choose [4])[Score]:
```

Only brains accept named-outcome selectors. The selected condition is checked
when the brain is called. If its path reaches no `feed(value):`, the result is
`null`.

Named outcomes are dynamically typed. Assigning a number to text converts it
to text. Assigning text to a number prints a recoverable runtime warning and
skips the assignment.

## Loops

Z# loops are unconditional:

```zsharp
loop (
 // Repeated code.
)
```

Loops do not contain a condition in their declaration. Use `if` and
`loop.end:` to stop:

```zsharp
noticed number Count = 0:

loop (
 number.set:Count = Count + 1:
 if[Count >= 10] (
  loop.end:
 ) else (
 )
)
```

`continue:` starts the next iteration:

```zsharp
loop (
 number.set:Count = Count + 1:
 if[Count == 1] (
  continue:
 ) else (
 )
 Print(Count):
)
```

Comparison:

| Z# | C#/Java/C |
|---|---|
| `loop (...)` | `while (true) { ... }` |
| `loop.end:` | `break;` |
| `continue:` | `continue;` |

## Arrays

An array type adds `()` after the element type:

```zsharp
noticed text() Names = ["Alex", "Sam", "Robin"]:
noticed number() Scores = [10, 20.5, 30]:
noticed Player() Players = [new Player["Zombie"]]:
```

Read an element with its zero-based index:

```zsharp
Print(Names[1]): // Sam
```

Change an element with `.set[index]`:

```zsharp
Names.set[1] = "Zombie":
Scores.set[0] = 99.25:
```

Read the length with `.Length`:

```zsharp
Print(Names.Length):
```

Add an object with `.add(...)`:

```zsharp
Players.add(new Player["Alex"]):
```

Comparison:

| Operation | Z# | Java/C# style |
|---|---|---|
| Declare text array | `text() Names = [...]` | `String[] names = {...}` / `string[] names = {...}` |
| Read | `Names[1]` | `names[1]` |
| Replace | `Names.set[1] = value:` | `names[1] = value;` |
| Length | `Names.Length` | `names.length` / `names.Length` |

## Objects and constructors

A constructor has the same name as its room:

```zsharp
room Player[] (
 noticed text Name:
 noticed number X = 0:
 noticed number Y = 0:

 noticed Player[text name] (
  text.set.Name = name:
 )

 noticed brain Move[number x, number y] (
  number.set:X = X + x:
  number.set:Y = Y + y:
 )
)
```

Create an object with `new` and constructor arguments in `[]`:

```zsharp
noticed Player User = new Player["Zombie"]:
```

Read fields with `.`:

```zsharp
Print(User.Name):
```

Call an instance method with `[]`:

```zsharp
User.Move[10, 20]:
```

## Horde members

`horde` is Z#'s static/shared modifier:

```zsharp
room Counter[] (
 noticed horde number Shared = 0:

 noticed horde brain Increase[] (
  number.set:Shared = Shared + 1:
 )
)
```

A horde field is shared by all objects of its room. A horde function cannot
directly access instance fields and cannot be invoked through an object.

Call it through its room path:

```zsharp
Function.call(Counters:Counter:Increase):
Print(Counter.Shared):
```

A horde room cannot be constructed, and every member must also be horde:

```zsharp
noticed horde room Tools[] (
 noticed horde number Uses = 0:
 noticed horde brain Use[] (
  number.set:Uses = Uses + 1:
 )
)
```

## Imports

Imports are placed inside a room and apply only to that room:

```zsharp
noticed room Launcher[] (
 import zsharp.Home():

 noticed brain Start[] (
  Function.call(Home:Main:Start_Outcome):
 )
)
```

Import an ordinary file with:

```zsharp
import Project.File():
```

Import a file inside folders with:

```zsharp
import Project.Folder.File():
```

Import every eligible endpoint from a project or namespace with a final
wildcard:

```zsharp
import Project.*():
import Project.Folder.*():
import ZSharp.*():
import ZSharp.Window.*():
```

`*` must be the last name in the import. Wildcards work for ordinary projects,
dependencies, and official namespaces, but do not bypass dependency or
visibility rules.

Importing a file exposes its eligible rooms and members. It does not bypass
visibility rules.

Using another file or project without importing it is a compile error.

## Qualified names

Value paths grow according to location:

```zsharp
Var
Room.Var
File.Room.Var
Project.File.Room.Var
```

Object paths use the same ladder:

```zsharp
User.Name
Room.User.Name
File.Room.User.Name
Project.File.Room.User.Name
```

Object method paths are:

```zsharp
User.Move[10, 20]:
Room.User.Move[10, 20]:
File.Room.User.Move[10, 20]:
Project.File.Room.User.Move[10, 20]:
```

Cross-project values may be read and written when their provider supports the
operation.

## External projects and native providers

Another project does not need to be written in Z#. A native provider can expose
C, C++, Java, C#, SDK, hardware, or service behavior as Z# project paths.

Example imports:

```zsharp
import playfab.Player():
import playfab.Entities():
```

Example access:

```zsharp
Print(playfab.Player.Data.PlayerId):
text.set.playfab.Player.Data.PlayerId = "updated-player-id":
playfab.Entities.Players.User.Move[3, 4]:
```

Provider ABI v1 currently supports the confirmed number, text, and status
operations described in [PROVIDERS.md](PROVIDERS.md).

## Typed JSON

Z# 1.0.2.4 can define a schema for flat JSON objects. Import the built-in JSON
feature and declare the expected keys and their Z# types:

```zsharp
noticed room JSONCustom[] (
 import ZSharp.JSON():

 noticed JSON PersonSchema[] (
  name: text:
  age: number:
  phone-number: text:
  enabled: status:
 )
)
```

Load a project-relative JSON file into a local value:

```zsharp
JSON Person = JSON.load(PersonSchema, "Data/Person.json"):
Print(Person.name):
Print(Person.age):
Print(Person["phone-number"]):
Print(Person.enabled):
```

Hyphenated keys use bracket access because `-` is also the subtraction
operator. A room-level loaded value may be `noticed`, bare/file-only, or
`silent`:

```zsharp
noticed JSON CurrentUser = JSON.load(PersonSchema, "Data/Person.json"):
JSON CachedUser = JSON.load(PersonSchema, "Data/Cached.json"):
silent JSON PrivateUser = JSON.load(PersonSchema, "Data/Private.json"):
```

Loading fails with a runtime error if the file is missing, leaves the project,
contains undeclared keys, omits required keys, or uses a value with the wrong
type. Schemas currently support `text`, `number`, and `status`; JSON booleans
become Z# `alive` and `dead` values.

## Text file I/O

Z# 1.1.2.0 provides project-relative text file operations:

```zsharp
File.write("Data/Save.txt", "Level=1"):
File.append("Data/Save.txt", "\nScore=250"):

status HasSave = File.exists("Data/Save.txt"):
text Save = File.read("Data/Save.txt"):
```

`File.write` creates or replaces a text file, while `File.append` creates it
or adds text to its end. `File.read` returns the full file as text and
`File.exists` returns `alive` or `dead`. Paths are relative to the project;
absolute paths and paths containing `..` are rejected. A single read is
limited to 16 MiB. Parent folders must already exist. These operations do not
open native file dialogs or grant access outside the project.

From Z# 1.2.1.0, recursively find files by extension:

```zsharp
text() Files = File.searchExtension(".zscene"):
Print(Files.Length):
text First = File.searchExtension(".zscene")[0]:
```

The extension must be nonempty and begin with `.`. Matching is ASCII
case-insensitive. Results are text paths relative to the project root with
`/` separators, sorted in case-sensitive path order. The search includes
all regular files under the project, including build/output/hidden folders;
symbolic links and junctions are not followed. No import is required.
No matches returns an empty array. Search-result arrays use ordinary zero-based
Z# indexing: `[0]` is the first file. Invalid indexes produce the usual
array bounds errors, so check `Files.Length` before reading `Files[0]`.
Results are a snapshot; another call searches again. Searches exceeding 256
nested directory levels fail instead of recursing indefinitely.

## Regular expressions

From Z# 1.2.1.0, no import is required for these expressions:

```javascript
status Valid = Regex.test("ABC123", "^[A-Z]+[0-9]+$"):
status Found = Regex.test("Hello", "hello", "i"):
text() Matches = Regex.matches("item12 item34", "\\d+"):
text First = Matches[0]:
text Updated = Regex.replace("a12 b34", "[0-9]+", "#"):
```

All arguments must be text. The optional final flags argument accepts `g`
(global), `i` (ignore case), `m` (multiline anchors), `s` (dot matches newlines),
and `u` (Unicode mode). Patterns follow the bundled QuickJS JavaScript RegExp
engine, not PCRE. Pattern and replacement strings are passed as data, never
executed as JavaScript source. Double backslashes in Z# strings: `"\\d+"`
passes `\d+` to the regex engine.

`Regex.test` returns alive/dead for any match; use `^` and `$` anchors when you
need a whole-string match. `Regex.matches` returns all full matches, not capture
groups, as a zero-based text array; no matches returns an empty array. You may
also index directly: `Regex.matches("a12", "\\d+")[0]`. Zero-length matches
advance safely instead of looping forever.

`Regex.replace` replaces all matches and returns the original text if none match.
Replacement templates support `$&` (the whole match), `$1`, `$2`, etc. (capture
groups), and `$$` (a literal dollar sign). `matches` and `replace` add `g`
automatically when it is absent. Invalid patterns, flags, or argument types
produce runtime errors. Each call uses an isolated engine context with a
16 MiB engine allocation limit, an approximately two-second execution deadline,
and at most 100000 returned matches. Returned text cannot contain NUL characters.

## Native math

Z# provides native scalar math expressions without requiring an imported
language:

```javascript
number Radians = Math.radians(InputDegrees):
number Sine = Math.sin(Radians):
number Cosine = Math.cos(Radians):
number Tangent = Math.tan(Radians):
number Root = Math.sqrt(Value):
number Positive = Math.abs(Value):
number Smaller = Math.min(First, Second):
number Larger = Math.max(First, Second):
number ConvertedDegrees = Math.degrees(Radians):
number Power = Math.pow(10, 5): // 100000; available from 1.2.1.0
```

`Math.pow(base, exponent)` accepts nested numeric expressions, for example
`number Power = Math.pow(2, Math.pow(3, 2)):` produces 512 (2 raised to 9).
It supports integer, fractional, zero, and negative exponents within the finite
double-precision range. Negative bases require integer exponents; zero cannot
have a negative exponent; `Math.pow(0, 0)` returns 1. Overflow produces a clear
runtime error, not infinity or invalid number text. Underflow may round to zero.
Thus `Math.pow(10, Math.pow(10, 123))` parses, but cannot be represented and
reports overflow. Use the result in an assignment, condition, or `Print(...)`;
math calls are expressions, not standalone colon-ended statements.

Trigonometric functions accept radians. `Math.radians` converts degrees to
radians, and `Math.degrees` performs the reverse conversion. `Math.sqrt`
reports a runtime error for a negative value. All Math arguments must be
numbers, and non-finite results are rejected. Native Math requires
`ZSharp: [1.1.2.2]:` or newer.

As of Z# 1.1.2.3, native Math results are always returned using ordinary
decimal notation. Tiny floating-point residue produced by trigonometry near
zero is normalized to `0`, so results never introduce unsupported scientific
notation into later Z# expressions.

Camera-relative X/Z movement can be calculated from the scene yaw:

```javascript
number Yaw = Math.radians(Start.cameraRotationY):
number ForwardX = Math.sin(Yaw):
number ForwardZ = 0 - Math.cos(Yaw):
number RightX = Math.cos(Yaw):
number RightZ = Math.sin(Yaw):

number MoveX = (ForwardX * ForwardInput) + (RightX * StrafeInput):
number MoveZ = (ForwardZ * ForwardInput) + (RightZ * StrafeInput):
number Length = Math.sqrt((MoveX * MoveX) + (MoveZ * MoveZ)):

if[Length > 0] (
 MoveX = MoveX / Length:
 MoveZ = MoveZ / Length:
) else (
)
```

Normalizing the X/Z movement vector prevents diagonal movement from being
faster than movement along one axis. Because repeating Z# division retains
one fractional digit, the result is intentionally approximate under the
current `number` rules.

## Errors and warnings

Compile errors include:

- misspelled or unknown language words;
- missing `:` terminators;
- unmatched room or function delimiters;
- invalid scientific number notation;
- missing imports;
- invisible rooms or members;
- unknown function outcome names;
- invalid object construction; and
- unresolved or ambiguous file names.

Recoverable runtime mistakes print a warning, skip the failed operation, and
continue. For example, assigning text output to a number does not crash the
program.

Failures that prevent meaningful execution stop the program. Examples include
division by zero, a missing required dependency, and corrupt bytecode.
When an installed app or game fails, its window closes, the Hub displays the
source error, and Z# writes a timestamped log under the installation data
folder's `logs` directory (`%LOCALAPPDATA%\\ZombieOS\\ZSharp\\logs` on
Windows).

#### Attaching a terminal

An installed app or game exposes its `Print(...)` output while it is running.
Connect by using the package filename recorded by the Hub:

```text
zsharp terminal "Z# IDE.zapp"
zsharp terminal ZSharpGameTest.zgame
```

The connection is output-only and does not start another copy of the project.
Type `zsharp terminal exit` inside the connected session to detach without
closing the app or game.

## Compilation and bytecode

Compile source to Z# bytecode:

```text
zsharp compile Main.zsharp -o Main.zbc
```

Compile and immediately run source:

```text
zsharp run Main.zsharp
```

Run compiled bytecode:

```text
zsharp run-bytecode Main.zbc
```

`.zbc` is currently provisional. `.zsharp` is the official source extension.

Compiled bytecode contains:

- a stable PID-derived project identity; and
- a SHA-256 hash of the compiled content.

The ZVM verifies these values before executing the bytecode.

## Java and Z# virtual machines

The current Java integration uses two execution systems:

```text
Java/Kotlin code -> JVM (`java`)
Z# bytecode      -> ZVM (`zsharp`)
```

A Java application starts on the JVM. When it uses
`com.zombieos:zsharp:1.0.2.1`, the library locates or extracts the bundled
native Z# runtime and starts it as a child process. The ZVM then compiles or
runs the requested Z# file.

JarJar, Shadow, or another dependency bundler can place the Z# runtime inside
the Java application's JAR. That makes distribution self-contained, but it
does not make Z# execute as JVM bytecode.

Therefore, a mixed Java and Z# application currently uses both:

- the JVM executes Java and Kotlin classes;
- the ZVM executes Z# bytecode; and
- the Java integration communicates with the Z# child process.

A future same-process integration could embed the ZVM as a native library
through JNI or Java's native-function facilities. Z# would still run on the
ZVM. A separate future compiler backend would be required for Z# itself to run
as JVM bytecode.

## Complete current console example

```zsharp
zsharp = type.script

noticed room Player[] (
 noticed text Name:
 noticed number X = 0:
 noticed number Y = 0:

 noticed Player[text name] (
  text.set.Name = name:
 )

 noticed brain Move[number x, number y] (
  number.set:X = X + x:
  number.set:Y = Y + y:
 )
)

noticed room Main[] (
 noticed Player User = new Player["Zombie"]:
 noticed text() Names = ["Alex", "Sam", "Robin"]:
 noticed status Alive = alive:

 noticed brain Start[] (
  User.Move[10, 20]:
  Names.set[1] = User.Name:

  if[Alive and User.X > 0] (
   Print(Names[1]):
   Print(User.X):
   Print(User.Y):
  ) else (
   Print("Player is unavailable"):
  )
 )
)
```

## Keyword comparison summary

| Purpose | Z# | C# | Java | C |
|---|---|---|---|---|
| Public visibility | `noticed` | `public` | `public` | Exported declaration |
| Private visibility | `silent` | `private` | `private` | `static`/hidden implementation |
| Class-like container | `room` | `class` | `class` | `struct` plus functions |
| No-value function | `brain` | `void` | `void` | `void` |
| Text | `text` | `string` | `String` | `char *`/`char[]` |
| Boolean | `status` | `bool` | `boolean` | `bool` |
| True | `alive` | `true` | `true` | `true`/nonzero |
| False | `dead` | `false` | `false` | `false`/zero |
| Shared/static | `horde` | `static` | `static` | `static` depending on context |
| Return a value | `feed(value):` | `return value;` | `return value;` | `return value;` |
| Early return | `feed:` | `return;` | `return;` | `return;` |
| Infinite loop | `loop (...)` | `while (true)` | `while (true)` | `while (1)` |
| Leave loop | `loop.end:` | `break;` | `break;` | `break;` |
| Next iteration | `continue:` | `continue;` | `continue;` | `continue;` |
| Missing value | `null` | `null` | `null` | `NULL` |

Z# deliberately does not copy the exact grammar of these languages. The table
compares intent, not necessarily implementation or memory behavior.

## Versioning and generations

Z# versions have four parts:

```text
generation.majorFeature.patch.revision
```

The first part selects the language generation:

```text
1.x.x.x -> Z1
2.x.x.x -> Z2
3.x.x.x -> Z3
```

The installed ZVM supplies runtime and client behavior. A 1.0.1.0 application
therefore continues to run on ZVM 1.0.2.1 and automatically receives runtime
fixes such as smoother window painting and silent Desktop launches; its package
does not need to be rebuilt. The `ZSharp` version in `project.zsettings`
describes the source version the project targets. New source fields and syntax
must be added to the project's code before the application can use them, while
runtime-only fixes apply automatically. Source may target a future Z# version
so development can begin early, but an installed runtime refuses to open an app
or game whose required version is newer than that runtime.

For the current release plan:

- `1.0.0.1` is the first Z1 release.
- `1.0.1.0` adds specialized script headers, native Windows/Linux/macOS window
  renderers, `.zapp`/`.zgame` packaging, file associations, Desktop shortcuts,
  and Hub launch-failure messages. Game objects remain a later runtime layer.
- A revision such as `1.0.0.2` would be a smaller patch without that feature
  scope.

When later generations replace older syntax, the compiler may emit migration
warnings that name both the old form and its recommended replacement. Z1 does
not invent warnings for generations that do not exist yet.


---

# Apps

## Window scripts

Window files begin with:

```zsharp
zsharp = type.script:window
```

There is exactly one window per `.zsharp` file. A window file does not contain
normal brain, number, or text functions. Event handlers live in an imported
normal script file.

The top-level window form is:

```zsharp
noticed Window Start[] (
 // Window imports and elements.
)
```

`Start` is only an example window name. The actual startup window is selected
by `project.zsettings`.

Window projects require:

```zsharp
Dependencies (
 zsharpwindow:1.0.0.0
):
```

Official UI features are imported individually:

```zsharp
import ZSharp.Window.Design():
import ZSharp.Window.Text():
import ZSharp.Window.Button():
import ZSharp.Window.Image():
import ZSharp.Window.TextInput():
```

Or import every Window feature at once:

```zsharp
import ZSharp.Window.*():
```

#### Window design

```zsharp
noticed design Design[] (
 title: "Window Title":
 icon: "assets/images/icon.png":
 width: 320zu:
 height: 180zu:
 scalable: alive:
 background: #FFFFFF:
)
```

A design background may also be a gradient:

```zsharp
background: linear-gradient(45:#FFFFFF:#336699):
background: radial-gradient(0:#101820:#336699:#FFFFFF):
```

Both gradient forms require a degree value and at least two `#RRGGBB` colors.
They accept any number of colors that fits in available memory, and the stops
are spaced evenly. Zero degrees points upward. For a radial gradient, degrees
rotate its focal point around the center.

If width and height are omitted, the window defaults to 25% of the current
screen. If its screen position is omitted, it starts in the screen center.

#### Responsive layout and scrolling

The full display is the window layout's reference canvas. At full width,
elements use their normal sizes and positions. As the user narrows the window,
images, buttons, and text inputs scale down horizontally with the available
width. Text keeps a readable font size and wraps onto additional lines instead
of disappearing outside the window.

Reducing only the window height does not shrink the layout. Content below the
visible area remains at its normal size and the window gains vertical
scrolling. Horizontal scrolling is not used.

#### Coordinates

Window and UI element coordinates use their center as `(0, 0)`:

```text
positive X -> right
negative X -> left
positive Y -> up
negative Y -> down
```

#### UI measurements

One `zu` is four pixels multiplied by the operating system display scale:

```text
pixels = zu * 4 * displayScale
```

An unsuffixed value defaults to `zu`:

```zsharp
width: 15:   // 15zu
width: 15zu: // 15zu
width: 15px: // exactly 15 pixels
```

#### Window text

Confirmed variants are:

```zsharp
text[title]
text[subtitle]
text[header]
text[subheader]
text[paragraph]
```

Plain `text` defaults to paragraph text.

```zsharp
noticed text[header] Header1[] (
 content: "This is a header!":
 color: #000000:
 locationX: 100:
 locationY: 100:
)
```

On Windows, text may begin with a project-relative inline PNG marker:

```zsharp
content: "<img:Assets/icon.png> Icon":
```

The image is drawn before the remaining text. The path cannot be absolute or
leave the project directory. Other desktop renderers will gain the same visual
behavior after their native implementations are tested.

#### Buttons

```zsharp
noticed button Button1[] (
 text: "Click Me!":
 textColor: #FFFFFF:
 buttonColor: #000000:
 width: 15:
 height: 15:
 locationX: 14:
 locationY: -12:
 Click[
  left: [File:Room:Function]:
  right: []:
 ]:
)
```

An empty click target means that mouse button has no handler. A target stores
a function reference; it does not call the function while the window is being
created. When a `Click` block is present, both `left` and `right` labels are
written; use `[]` for either unused target. The target script must be imported
and must satisfy visibility rules.

All UI `width` and `height` values must be greater than zero. Coordinates may
be positive, negative, or zero.

#### Images

```zsharp
noticed image Logo[] (
 file: "assets/images/ZSharp.png":
 width: 30:
 height: 35:
)
```

PNG, JPG/JPEG, BMP, and GIF files are scaled to the element's declared width
and height by the platform's native image facilities. The same applies to
`design.icon`. Windows uses WIC, Linux uses GdkPixbuf through GTK 3, and macOS
uses AppKit.

#### Text and image input

```zsharp
noticed textInput Input1[] (
 display: "This is an input box.":
 type: text:
 multiline: alive:
 wrap: alive:
 fontSize: 18px:
 maxLength: 120:
 textAlign: center:
 textTransform: uppercase:
 allowedCharacters: ["a", "b", "c", "1", "2", "3", "-", " "]:
 locationX: 10:
 locationY: 15:
 width: 15:
 height: 10:
 contents: []:
)
```

For image selection:

```zsharp
noticed textInput ImageInput[] (
 display: "Choose an image":
 type: image:
 supportedTypes: [png, jpg]:
 contents: []:
)
```

`contents` starts empty and is exposed to imported script functions as the full
live text or selected image path:

```zsharp
text EnteredText = Startup.Input1.contents:
text SelectedImage = Startup.ImageInput.contents:
```

The runtime changes UI state rather than rewriting packaged source code.
Functions in imported script files can read these live text-input values:

```zsharp
text FullText = Startup.Input1.contents:
number Characters = Startup.Input1.totalcharacters:
number Column = Startup.Input1.currentcolumn:
number Lines = Startup.Input1.totallines:
number Line = Startup.Input1.currentline:
```

The shorter `Input1.property` form also works. `currentline` and
`currentcolumn` are 1-based. An empty text input has zero characters, one line,
and a cursor at line 1, column 1. Line breaks count as characters, with a
Windows CRLF pair counted as one line-break character. The four numeric fields
are read-only and apply to text inputs; image inputs expose `contents` only.

#### Dropdown/select input

Use a native dropdown when one value must be selected from a fixed list:

```zsharp
noticed dropdown ModelSelector[] (
 options: ["ZOSAI-1 - Astroid", "Test Model"]:
 selected: "ZOSAI-1 - Astroid":
 textColor: #FFFFFF:
 dropdownColor: #181822:
 fontSize: 16px:
 width: 220px:
 height: 32px:
 locationX: 16px:
 locationY: 16px:
 anchorX: right:
 anchorY: top:
 Change[Handlers:Models:Changed]:
)
```

`dropdown` and `select` name the same element. `options` contains quoted
display values, and `selected` must equal one of them. Read or change the live
value with `Startup.ModelSelector.selected` and
`Startup.ModelSelector.selected.set: "Test Model":`. `Change` is optional and
uses the same imported `File:Room:Function` callback form as button clicks.

Every non-design element may use `anchorX: left:`, `center`, or `right` and
`anchorY: top:`, `center`, or `bottom`. Center is the compatibility default.
With an edge anchor, `locationX`/`locationY` becomes the inward distance from
that edge, allowing a top header, growing middle area, and bottom input bar to
stay placed when a scalable window is resized.

Text inputs are single-line by default. Add `multiline: alive:` to allow line
breaks. A multiline input wraps long lines by default; use `wrap: dead:` when
long lines should remain on one line and scroll horizontally instead. `wrap`
cannot be set unless multiline mode is alive, and neither field is valid for
an image input.

Text inputs can also set `fontSize`, limit input with a positive whole-number
`maxLength`, and align text with `textAlign: left:`, `center`, or `right`.
`textTransform` accepts `none`, `uppercase`, or `lowercase`. An optional
`allowedCharacters` array accepts quoted single characters; letters match
case-insensitively, so allowing `"b"` permits both `b` and `B`. Characters not
listed are ignored when typed or pasted. These fields require Z# 1.0.2.5.

Native window scrollbars remain hidden while all elements fit within the
visible window. A vertical scrollbar appears automatically when content
extends below the viewport. Multiline input scrollbars are handled inside the
input itself.

#### ZSS window styling

Window projects load every `.zss` file under the project directory. Use
`.Window Element` to target one element in a named window. `.Element` is the
short form when the element name is enough:

```css
.Startup CodeEditor {
 background: #0B0B10;
 color: #E8E8F0;
 border: 1px solid #272734;
 border-radius: 6px;
 font-family: Consolas;
 font-size: 15px;
 font-weight: normal;
 padding: 12px;
 caret-color: #FF2A42;
 outline: none;
 selection-background: #3A153D;
 selection-color: #FFFFFF;
}

.Startup Save:hover {
 background: #242431;
 border-color: #505064;
}

.Startup CodeEditor:focus {
 border-color: #606078;
}
```

The native window style pass supports `background`/`background-color`, `color`, solid
`border`, `border-color`, `border-radius`, `font-family`, `font-size`,
`font-weight`, `padding` and its four directional forms, `caret-color`,
`outline: none`, `selection-background`, `selection-color`, `width`, `height`,
`text-align`, `text-transform`, and `max-length`. `:hover`
targets buttons and `:focus` targets text inputs. ZSS rules are applied in
sorted file order and override matching values written in the window script.
Selectors and declarations are checked while the project compiles. ZSS is
CSS-shaped, but it styles native controls rather than a browser DOM, so
unsupported web-only properties produce a compile error.

#### Changing live window attributes

An imported normal-script callback can change the active window immediately:

```zsharp
noticed brain ToggleTheme[] (
 Startup.Design.title.set: "Dark mode":
 Startup.Design.background.set: linear-gradient(1:#101820:#000000):
 wait(1ms):
 Startup.Design.background.set: linear-gradient(2:#101820:#000000):
)
```

`Startup` is the window filename without `.zsharp`, `Design` is the declared
element name, and `background` is its field. The shorter form also works when
the active window is already unambiguous:

```zsharp
Design.background.set: #FFFFFF:
```

Wrap an expression in parentheses on the following line when a live property
needs a calculated value:

```zsharp
Startup.CursorPosition.content.set:
 ("L" + CurrentLine + ":C" + CurrentColumn):
```

From 1.1.0.1, text properties also accept a text variable directly:

```zsharp
text Response = "Hello":
Startup.ChatOutput.content.set: Response:
```

A text variable can serve as a reusable property-path alias. Its value is
resolved when the setter runs, so changing the text can retarget later writes:

```zsharp
noticed text Background = "Startup.Design.background":

noticed brain Animate[] (
 Background.set: linear-gradient(1:#FFFFFF:#000000):
 wait(1ms):
 Background.set: linear-gradient(2:#FFFFFF:#000000):
)
```

The alias must contain `Element.property` or `File.Element.property`. It must
refer to the active window when the callback runs.

The setter supports these live fields in 1.0.2.1:

- design: `title`, `icon`, `scalable`, `background`, `width`, `height`,
  `locationX`, and `locationY`;
- text: `content`, `color`, size, and position fields;
- button: `text`, `textColor`, `buttonColor`, size, and position fields;
- image: `file`, size, and position fields; and
- text input: `display`, size, and position fields.

Solid colors work in every color field. Gradients currently target the design
`background`. Input `contents` is owned by the person using the app at runtime;
input `type`, `supportedTypes`, and `Click` targets are not live-mutable.
Changing a property redraws it before the next statement, so a sequence of
gradient updates and short waits can animate a background.

### Dynamic window text and inputs

Normal text accepts `textAlign: left:`, `center`, or `right`. Wrapped text
recalculates its rendered height after `.content.set`, and window scroll bounds
are recalculated when that content grows or shrinks.

Text input contents can be replaced or cleared and keyboard focus can be
returned to an input:

```javascript
Startup.Prompt.contents.set: "":
Startup.Prompt.focus.set: alive:
```

Set `focus` to `dead` to release focus from that input. Multiline inputs treat
Enter as a newline. Z# 1.1.1.0 does not yet expose a portable Shift+Enter
modifier callback, so applications should use a Send button rather than
overriding Enter with application-specific behavior.

Window text and input use UTF-8 end-to-end, including non-Latin scripts and
emoji. Native runtime failures now include the source, room, brain, and Z# call
chain where available.

## Window project settings

```zsharp
zsharp = type.settings

Project: "My Application":
PID: "my_application":
Version: [1.0.0.0]:
Authors: ["Author"]:
Description: "A Z# application":
ZSharp: [1.0.2.1]:

Dependencies (
 zsharpwindow:1.0.0.0
):

Window (
 Startup: "window/Main.zsharp":
 Uninstall: "window/Uninstall.zsharp":
):
```

`Window (...)` is a settings section, not a function.

- `Startup` identifies the first window opened at launch.
- `Uninstall` identifies the window opened during `zsharp uninstall`.
- Both paths are relative to the project root.
- Both paths use `/` on every operating system.
- Neither path may escape the project with `../`.
- Both files must exist and use the `zsharp = type.script:window` header; the
  compiler validates their complete window syntax and imports.

Validate and register the project for the current user with:

```text
zsharp project path/to/project.zsettings
zsharp project path/to/project-folder
```

The tool walks upward from supplied files and folders to find the nearest
`project.zsettings` automatically. Registration does not launch the startup
window. Repeating the command updates the existing entry for that PID or path
instead of adding a duplicate.

The registry is stored in the current user's application-data location:

```text
Windows: %LOCALAPPDATA%\ZombieOS\ZSharp\projects.registry
Linux:   $XDG_DATA_HOME/zsharp/projects.registry
         (or ~/.local/share/zsharp/projects.registry)
macOS:   ~/Library/Application Support/ZSharp/projects.registry
```


---

# Games

## Game projects

Every game project must enable the official game
runtime in `project.zsettings`:

```zsharp
Dependencies (
 zsharpgame:1.0.0.1
):

Window (
 StartScene: "Game/Scenes/Main.zscene":
)

Splash[JSON] (
 [
  {
   "path": "assets/images/Studio.png",
   "duration": 2
  },
  {
   "path": "assets/images/Game.png",
   "duration": 2
  }
 ]
)
```

`Window.StartScene` selects the scene shown when the game starts; it replaces
the old file-order behavior. It requires `zsharpgame:1.0.0.1` but does not
require `zsharpwindow`. If a game also depends on `zsharpwindow`, its existing
`Startup` and `Uninstall` entries may share the same `Window` block, and
`Startup` remains optional whenever `StartScene` is present.

`Splash[JSON]` is optional. Each project-relative image is displayed in array
order for its positive `duration` in seconds before game scripts start. PNG and
BMP images are supported. Closing the game during a splash cancels launch.

The room and function syntax inside a game script is the normal Z# syntax. A
non-`DR` `Start[]` runs automatically after the SDL3 window and Vulkan renderer
are ready. Eligible starts across the project run as independent tasks, so an
endless game loop does not block window events or rendering. Closing the window
or pressing Escape cancels and joins those tasks.

Input paths such as `input.key.a` are global Z# runtime values and do not need
an import. A script must import another script-owned API before using it. For
example, a project with PID `zsharp_adhd_game` imports scene control with:

```zsharp
import zsharp_adhd_game.game.scene():
```

After that import, the room may read or write `Game.scene`. The wildcard form
`import zsharp_adhd_game.game.*():` is also accepted.

The 1.0.2.1 runtime initializes SDL video, audio, keyboard, mouse, and gamepad
support, creates a high-DPI resizable game window, and forces Vulkan for game
drawing. Windows and Linux are the advertised game targets. A bundled MoltenVK
path exists for macOS, but it is experimental until tested on Mac hardware.

Every scene lives in a separate `.zscene` file:

```zsharp
zsharp = type.scene

noticed scene Main[] (
 scene Main (
  title: "Main Scene":
  icon: "assets/icon.png":
  background: #08080B:
  gravityX: 0:
  gravityY: -900:
 )

 objects[JSON] (
  [
   {
    "id": "Player",
    "name": "Player One",
    "location": {
     "x": "-250",
     "y": "-200"
    }
   }
  ]
 )
)
```

Scene `icon` paths are project-relative PNG files. The active scene's icon is
used for the native game window. If a scene omits it, ZVM uses the project
`Icon`; if neither is supplied, it keeps the default Z# application icon.

Every object lives in a separate `.zobject` file and starts with
`zsharp = type.object`. ZVM treats the model as 3D when it uses a 3D-only
field such as a scene placement's `z`, `length`, `scaleZ`, `velocityZ`,
`gravityZ`, or
`cameraZ`, or the `cube` shape. Otherwise it renders as 2D. Coordinates start
at the center of the game view: positive X moves right, negative X moves left,
positive Y moves up, and negative Y moves down.

For example, a complete 3D scene file can contain:

```zsharp
noticed scene Main[] (
 scene Main (
  title: "3D Scene":
  background: #08080B:
  gravityX: 0:
  gravityY: -900:
  gravityZ: 0:
  cameraX: 0:
  cameraY: 0:
  cameraZ: 8:
  cameraRotationX: 0:
  cameraRotationY: 0:
  cameraRotationZ: 0:
  cameraFov: 70:
 )
 objects[JSON] (
  [
   { "id": "Cube", "name": "Main Cube", "location": { "x": "0", "y": "0", "z": "0" } }
  ]
 )
)
```

The configured `Window.StartScene` is active at launch. Without that setting,
the first discovered scene is used for compatibility. `cameraZ` and
`cameraFov` mainly affect 3D games. `cameraRotationX`, `cameraRotationY`, and
`cameraRotationZ` control pitch, yaw, and roll in degrees and can be changed
while the game is running. A game project must contain at least one
`.zscene` file.

Define a 2D object in its own `.zobject` file like this:

```zsharp
zsharp = type.object

noticed object Player[] (
 shape: rectangle:
 width: 48:
 height: 72:
 rotation: 0:
 scaleX: 1:
 scaleY: 1:
 color: #FF2A72:
 visible: alive:
 layer: 10:

 body: dynamic:
 collider: box:
 mass: 1:
 gravityScale: 1:
 restitution: 0:
 friction: 0.15:
 velocityX: 0:
 velocityY: 0:

 attributes[JSON] (
  [
   {
    "id": "COLLIDER2D",
    "active": true
   }
  ]
 )
)
```

The object declaration's ID is referenced by `id` in a scene's `objects[JSON]`
array. `name` is that placed instance's display name, while `location` owns its
X/Y position and optional 3D Z position. This lets an object definition remain
independent from the scene that places it. Starting in 1.1.4.0, a scene
placement may add optional `width`, `height`, `length` (or `depth`), `color`,
and `texture` fields after `location`. They override only that instance; omitted
fields inherit the `.zobject` defaults. Size values must be positive, colors
use quoted `"#RRGGBB"` text in the JSON, and texture paths are quoted safe
project-relative paths. For example, both placements below reuse `Cube.zobject`:

```json
[
  {
    "id": "Cube",
    "name": "Blue Cube",
    "location": { "x": "-10", "y": "0", "z": "0" },
    "width": "12",
    "height": "8",
    "length": "6",
    "color": "#3366FF"
  },
  {
    "id": "Cube",
    "name": "Black Cube",
    "location": { "x": "10", "y": "0", "z": "0" },
    "color": "#000000"
  }
]
```

The scene-level `texture` override uses the existing sprite/image asset path.
In 1.1.4.0, cubes also map a PNG or BMP texture onto each face; `color`
multiplies the texture, so use `#FFFFFF` for its unmodified colors.
`attributes[JSON]` accepts official
IDs such as `COLLIDER2D` and `COLLIDER3D`, plus project-defined IDs prefixed
with `CUSTOM:`. Each attribute has an `active` boolean. Object textures use
`texture: "path/to/texture.png":`; a 3D object's third size is `length:`.

Supported shapes are `rectangle`, `circle`, `triangle`, `sprite`, `cube`,
and `text`. `text` objects use a quoted `text:` field. The first sprite loader
accepts BMP assets through `asset:`; primitive shapes need no external asset.
`cube` is the initial 3D primitive and uses `positionZ`, `depth`, `scaleZ`, and
the active scene camera. Cubes support `rotationX`, `rotationY`, and
`rotationZ`; the older `rotation` property remains a Y-axis rotation for cube
compatibility and keeps its existing 2D behavior. A cube whose width, height,
or depth is omitted uses
`1` world unit for that dimension; 2D shapes retain their existing 64-pixel
defaults. The camera looks toward negative Z. Cubes crossing the camera's near
plane are clipped rather than discarded.

Bodies can be `static`, `dynamic`, or `kinematic`. Dynamic bodies receive
gravity and collision response. Kinematic bodies use `velocityX`, `velocityY`,
and `velocityZ` but do not receive gravity, making them suitable for moving
platforms. A grounded dynamic body is carried along X and Z by a kinematic
platform. Static bodies do not move. Colliders can be `none`, `box`, or `circle`;
the collision solver exposes grounded/colliding state, supports XYZ collision
for `COLLIDER3D`, and supports `trigger: alive:` for overlap-only objects. In
3D, box colliders follow the object's X/Y/Z rotation (including legacy
`rotation` as yaw), and spheres/capsules contact rotated boxes. Fast-moving
bodies are still resolved at discrete frame positions rather than by swept
collision. `restitution` controls
bounce and `friction` slows horizontal movement on a surface.

Z# does not provide a pre-made player movement controller. Game scripts read
input and change object position or velocity explicitly. A reusable movement
system can instead be distributed as a project dependency.

Game key reads use `input.key.KEY`. Supported names include `a` through `z`,
`0` through `9`, `larrow`, `rarrow`, `uarrow`, `darrow`, `fn1` through
`fn24`, `space`, `enter`, `escape`, `tab`, `backspace`, modifier keys, and
mouse properties:

```zsharp
input.key.a
input.key.rarrow
input.key.space
input.key.fn1
input.mouse.left
input.mouse.right
input.mouse.leftPressed
input.mouse.leftReleased
input.mouse.rightPressed
input.mouse.rightReleased
input.mouse.x
input.mouse.y
input.mouse.deltaX
input.mouse.deltaY
input.mouse.captured

Game.scene
Game.delta
Game.elapsed
Game.fps

Player.positionX
Player.velocityY
Player.grounded
Player.colliding
```

`Game.fps` measures the reciprocal of the most recent rendered-frame loop
interval (including presentation and pacing), not the fixed physics rate.
It can fluctuate; `Game.delta` still reports the physics timestep.
For performance investigation, set `ZSHARP_GAME_PROFILE=1` before launching
the development runtime. It reports frame intervals and physics/input,
snapshot, and render/present times to stderr. Profiling output itself adds
overhead; disable it for normal play.

The development 1.2.1.0 runtime uses SDL_GPU/Vulkan for 3D triangle filling,
texture sampling, blending, and depth testing where available. It retains
software depth rendering when GPU initialization is unavailable. Scripts,
physics, mesh transformation, and current lighting/shadow calculations remain
CPU work. No source changes are needed to select the GPU pass. For comparison
or troubleshooting, set `ZSHARP_GAME_SOFTWARE=1` before launch to force the
software renderer. Profiling prints the selected renderer at startup.
The new GPU path has been tested locally on Windows; Linux/macOS hardware
verification is still required before claiming support there.

### Game buttons (Z# 1.2.0.1)

```zsharp
zsharp = type.object
noticed object NewGame[] (
 shape: button:
 text: "New Game":
 width: 160:
 height: 32:
 color: #FFFFFF:
 hoverColor: #990000:
 event(
  left: "Menu:Menu:NewGame":
  right: "Menu:Menu:Options":
 )
)
```

Place the button in the scene's `objects[JSON]` like any other object.
`event(...)` is required, and must contain at least one nonempty `left` or
`right` callback. Both are optional individually. Targets use the existing
`File:Room:Function` window callback format (or `Project:File:Room:Function`).
Callbacks must take no arguments and execute as Z# tasks on button-down, not
by polling held-state. Hidden buttons do not receive clicks. In desktop mode,
overlapping buttons use the highest `layer`, with later placements winning ties.
`color` is the background; `hoverColor` is optional. The label is centered
with automatically contrasting text. Desktop buttons are axis-aligned,
respect size and scale, and overlay the scene. In 2D they follow the scene
camera; in desktop 3D their X/Y placement is screen-space (center = 0,0).
Release mouse capture before using desktop menu buttons. Their press events
remain available to scripts. In VR, see [World-space VR buttons](#world-space-vr-buttons)
for scene-space placement and either-controller trigger activation using these
same object/event declarations.

`input.mouse.left` and `input.mouse.right` are held-state statuses. A complete
click between script reads can leave both held-state reads `dead`.
Use `leftPressed`, `leftReleased`, `rightPressed`, and `rightReleased` for
reliable transitions. Each read consumes one pending transition of that kind
for the current script task and returns `alive`; when none remain it returns
`dead`. These are read-only statuses, not one-frame pulses: pending events
survive waits and render frames. Tasks consume independently, so one task
cannot steal another's clicks. Tracking starts when each task thread begins;
new tasks do not replay earlier clicks. Multiple queued clicks produce
multiple `alive` reads. Press/release queues are independent, not a combined
chronological event stream. Read once and save the result if multiple actions
need to use the same transition. Losing focus releases held mouse buttons.

```zsharp
loop (
 if[input.mouse.leftPressed == alive] (
  Print("Left button pressed"):
 ) else ()
 wait(16ms):
)
```

`input.mouse.deltaX` and `input.mouse.deltaY` contain the accumulated mouse
movement for the current frame. Capture the mouse for a normal 3D camera with
`input.mouse.captured.set: alive:` and release it with
`input.mouse.captured.set: dead:`. Capture is never enabled automatically.

Escape is only an input value (`input.key.escape`); it does not close a game
automatically. A game may assign it to pause, a side menu, or its own quit
logic.

Input and collision values are statuses (`alive` or `dead`). Positions,
velocities, dimensions, timing, and FPS are numbers. `Game.scene` and object
text are text values.

Use the normal property setter for literal updates:

```zsharp
Player.color.set: #00E5FF:
Player.velocityY.set: 500:
Player.visible.set: alive:
Game.scene.set: Gallery:
Main.background.set: #101820:
Player.texture.set: "Assets/Player2.png":
```

Calculated number and text changes use the normal typed assignment forms:

```zsharp
number.set:Player.positionX = Player.positionX + 5:
number.set:Player.rotation = Player.rotation + Game.delta * 90:
text.set.Player.text = "Score: " + Score:
number.set:Main.cameraX = Player.positionX:
number.set:Main.cameraRotationY = Main.cameraRotationY + input.mouse.deltaX:
number.set:Player.rotationZ = Player.rotationZ + Game.delta * 90:
```

For 3D objects, `positionX/Y/Z`, `velocityX/Y/Z`, `rotationX/Y/Z`, and
`scaleX/Y/Z` are all readable and writable. `Player.grounded` becomes alive
when a dynamic `COLLIDER3D` is supported from below; `Player.colliding`
indicates any current collider contact. Python, JavaScript, and Lua may help
with vector or camera calculations, but none is required for native 3D input,
rendering, or physics.

An object can be qualified with its scene as
`Main.Player.positionX`. Input and timing properties are read-only, as are
`grounded` and `colliding`. Scene changes, rendering, physics, and script loops
run concurrently, so `wait(...)` does not freeze the game window.

### Controlling a specific scene placement

In 1.2.1.0, `Scene.Instance.property` addresses one live placement, not its
shared `.zobject` definition. The scene prefix is the declared scene name,
not its folder path. Instance identifiers are case-sensitive.

```zsharp
objects[JSON] (
 [
  {"id": "Cube", "name": "ElevatorLeftDoor",
   "location": {"x": -1, "y": 0, "z": 0}},
  {"id": "Cube", "name": "Elevator Right Door",
   "location": {"x": 1, "y": 0, "z": 0},
   "instanceId": "ElevatorRightDoor"}
 ]
)
```

An identifier-style `name` is the default instance identifier. Use optional
`instanceId` after `location` when a hierarchy label contains spaces, or when
several placements share a display label. Projects using `instanceId` must
target `ZSharp: [1.2.1.0]:` or newer. Explicit IDs use at most 127 ASCII
letters, digits, or underscores, starting with a letter or underscore, and
must be unique within that scene. Names and IDs may repeat in different scenes.
An explicit ID replaces the display-name alias. Duplicate explicit IDs and
invalid IDs are rejected during packaging/loading. Repeated legacy display
labels are still allowed, but referencing an ambiguous label is an error.

```zsharp
number.set:Chapter1Floor1.ElevatorLeftDoor.positionX =
 Chapter1Floor1.ElevatorLeftDoor.positionX - (Game.delta * 2):
number.set:Chapter1Floor1.ElevatorRightDoor.positionX =
 Chapter1Floor1.ElevatorRightDoor.positionX + (Game.delta * 2):
Chapter1Floor1.ElevatorLeftDoor.visible.set: dead:
```

Readable/writable transforms are `positionX/Y/Z`, `rotationX/Y/Z`,
`scaleX/Y/Z`, and `velocityX/Y/Z`. Existing writable properties such as
`visible`, `width`, `height`, `depth`, `color`, `trigger`, and the supported
surface/physics/light fields use the same instance path. Collision flags
`grounded` and `colliding` remain read-only; this does not add a writable `body`
or `collider` field. Moving one Cube placement does not move the other Cubes.

Unqualified `Instance.property` addresses only the active scene. The old
object-definition name (`Player.positionX`, for example) remains an alias
when it uniquely identifies a placement; it is never an arbitrary first match.
A unique placement name/ID takes precedence over a definition-name alias.
Qualified paths address the named scene's own instance, including inactive
scene state, and do not follow an identically named instance into another scene.
Scene instances are indexed at load time; property access does not scan all
placements. Live reads and writes are synchronized with physics snapshots.

Position changes immediately affect the rendered transform and collider.
Kinematic velocity moves platforms normally; script-positioned static or
kinematic platforms also record movement for rider carrying. Grounded dynamic
bodies are carried horizontally and vertically by their supporting platform
where contact is maintained; jumping releases support. This is translation
carrying, not rotation/scale-driven rider attachment or swept collision for
large teleports. No scene rebuild/reload is necessary.

`Game.scene.set: "Chapter1Floor2":` keeps the existing scene-entry behavior:
positions return to their configured spawn locations and velocities reset.
Set arrival positions after switching scenes. Explicit transforms on an
inactive scene are not a substitute for configuring its spawn placements.
Scene switching itself still requires `import project_id.game.scene():`.

## Achievements and audio

Game achievements use ZSON in a `type.script:achievement` file:

```zsharp
zsharp = type.script:achievement

achievement CLIMBER[JSON] (
 [
  {
   "display": "Climber",
   "description": "Reach level 5!",
   "rarity": "HARD"
  }
 ]
)
```

Rarity must be `EASY`, `MEDIUM`, `HARD`, or `IMPOSSIBLE`. A normal game script
imports the official achievement API and awards the ID once its condition is
met:

```zsharp
import ZSharp.Achievements():
ZSharp.Achievement.Award.CLIMBER:
```

Awards are idempotent: awarding an earned ID again does nothing. New awards
are saved locally by project PID, retained across reinstall, counted in the
Z# Hub, and displayed in-game for five seconds with a short sound.

Reusable scene audio is defined in a `.zaudio` source file. The source points
to the real WAV asset; `.zaudio` does not contain encoded sound data:

```zsharp
zsharp = type.script:audio

noticed audio Bounce[] (
 source: "Assets/Audio/Bounce.wav":
 volume: 35:
 pitch: 100:
 loop: false:
)
```

Place it in a scene's existing `objects[JSON]` list by its audio ID:

```json
{
 "id": "Bounce",
 "name": "Ball Bounce",
 "location": {
  "x": "0",
  "y": "0"
 }
}
```

`volume` and `pitch` use 100 as their normal value. `loop` accepts `true` or
`false` (and the equivalent Z# statuses `alive` or `dead`). Audio remains idle
until a script in its active scene starts it with
`Bounce.audioPlay.set: alive:`. ZVM currently accepts WAV assets for `.zaudio`
sources.

Object-owned WAV audio and generated tones remain supported for compatibility.
For small effects that need no asset, use `tone:` (frequency in hertz) and
`toneDuration:` (seconds).

The complete playable example is in `examples/test-game`. Package both the
bytecoded and source forms with:

```text
zsharp package game "path/to/examples/test-game" ZSharpGameTest --unbytecode
```

## ZSS game styling

### Button states (1.2.1.0)

Game text and buttons accept a base `font-size` in pixels:

```css
.NewGame { font-size: 24px; }
```

Bare numbers are also pixels. Values must be greater than zero and at most
4096. The default is the existing 8-pixel bitmap font; increasing font size
scales that bitmap, rather than selecting a smooth/vector font. Button text
remains centered and clipped to the button area. Font-size state overrides
and font-size transitions are not implemented.

A quit button's callback can close the app/game with this 1.4.0.0 syntax:

```zsharp
noticed room Menu[] (
 import ZSharp.Application():
 noticed brain Quit[] (
  Application.Quit("regular"):
 )
 noticed brain Hide[] (
  Application.Quit("tray"):
  // Background tasks and this callback keep running.
 )
)
```

The using room must import `ZSharp.Application`, and the project must declare
`ZSharp: [1.4.0.0]:` or newer. The argument is a text expression whose value
must be exactly `"regular"` or `"tray"`; the slash in `"regular/tray"` is not
part of a valid mode. Missing imports, missing/extra arguments and invalid
mode values produce diagnostics.

`"regular"` requests graceful native app/game closure, returns from the current
callback, cancels other tasks, and uses the usual `Shutdown[]` cleanup path.
It is not a force-kill operation.

`"tray"` hides a native app window without ending the process, cancelling
tasks, returning early from the callback, or running `Shutdown[]`. The app's
tray/menu-bar item provides Show and Exit; Exit performs regular shutdown.
The window's normal X button remains regular close. Tray support depends on
the desktop environment. If it is unavailable or icon creation fails, a
runtime warning is reported and the window stays reachable; nothing is
silently hidden. The current game runtime does not provide a tray item and
reports the same recoverable warning for tray mode. bZVM does not support
`ZSharp.Application` or either quit mode.

Migration: bare `Application.Quit:` no longer compiles as source. Replace it
with `Application.Quit("regular"):` and add the room import. Existing bytecode
using the older quit instruction retains its regular-close behavior.
Packages also contain source that startup/callback discovery can reparse;
older packages containing bare quit statements therefore currently require
migration and rebuilding, even if their embedded bytecode remains readable.

Game buttons support `:hover` (pointer over the button) and `:focus`
(selected for keyboard activation):

```css
.NewGame:hover {
 color: #990000;
 opacity: 0.8;
 scale-x: 1.1;
 scale-y: 1.1;
}
.NewGame:focus {
 color: #3366FF;
 opacity: 1;
}
.NewGame {
 transition: all 150ms ease-in-out;
}
```

State declarations support `color`, `opacity`, `scale-x`, and `scale-y`.
The base button rule also accepts `background: transparent;` to hide only
the background while retaining the label and clickable area. A `#RRGGBB`
background restores a solid fill. Background transparency remains in effect
during hover/focus; use a solid background if those states should show a fill.
ZSS opacity is between 0 and 1; scene/runtime opacity uses its existing
0–100 range. Hover takes precedence when both states supply the same property.
These visual scale changes do not resize the physics collider or click area.
Tab/Shift+Tab cycles visible buttons; Enter/Space invokes the focused button's
left action. Clicking a button focuses it. Losing window focus clears selection;
mouse capture suppresses button focus/hover interaction.

Button transitions accept `all <duration> [timing]` or `none`. Durations use
`ms` or `s`; timing is `linear`, `ease`, `ease-in`, `ease-out`, or
`ease-in-out`. Transitions interpolate color, opacity, and visual scale.
Property lists, delays, and CSS keyframe animations are not implemented.


Z# Style Sheets use the `.zss` extension and CSS declaration syntax. They are
parsed directly by ZVM and do not require a browser. Use `.Object` when an
object name is enough, or `.File Object` to name the `.zobject` file too:

```css
.Player {
 color: #FF2A72;
 width: 48;
 height: 72;
}

.World Bouncer {
 color: #FFE66D;
 scale-x: 1.15;
 scale-y: 1.15;
 rotation: 8;
}
```

Kebab-case ZSS names such as `position-x`, `gravity-scale`, and
`audio-volume` map to the matching Z# fields (`positionX`, `gravityScale`, and
`audioVolume`). Rules are applied in sorted file order after `.zobject` files
load, so a later ZSS rule overrides an earlier object value. In 1.0.2.1 ZSS
styles the game fields supported by `.zobject`; web-only layout and browser DOM
properties do not apply to native game objects.

## Animation clips

`.zanimation` files are JSON containing named clips. A clip has `models`
(project-relative `.zobject`, `.zmodel`, or `.zai` references),
positive `fps`, `type` (`tween` or `static`), `loop`, and `parts` with ordered
keyframes. Each keyframe has `frame` and optional `posX/posY/posZ` and
`rotX/rotY/rotZ` values. These are offsets from the placed object's transform.
The optional keyframe `models` map uses one-based indexes into the clip's
`models` array, for example `"models": {"1": true, "2": false}`.

```zsharp
import my_project.GemBounce():
GemBounce.playClip.Pulse:
GemBounce.pauseClip.Pulse:
GemBounce.playClip.Pulse[2]: // resume or target the second instance
GemBounce.stopClip.Pulse: // restore the starting transform
```

Tracks named `root` or `part_1` animate the placed object's transform.
Other `parts` names can match FBX mesh-node names and animate those mesh
parts around their imported pivots. A track matching an FBX bone name instead
poses that bone and deforms its weighted vertices, including child-bone
hierarchy. This requires a skinned FBX and is tested with a multi-bone asset.
Clips are exclusive per placed instance: starting a different clip stops the
previous clip on that instance. Animation blending and direct playback of an
FBX's embedded animation stacks are not yet supported.

## Imported models and AI

An imported model uses a project-relative FBX source. Scene placements can
override its size, color, texture, rotation, and surface properties just as
they can for `.zobject` definitions. The packager validates and bundles the
source FBX; the renderer draws its triangulated meshes and UV textures.
Scene `width`, `height`, and `length` set the imported mesh's bounding size
on each axis, while the FBX origin remains its transform pivot. Runtime
`scaleX/Y/Z` multiply those dimensions.

```zsharp
zsharp = type.script:model
noticed model Suzanne[] (
 parent: "Models/Suzanne.fbx":
 collider: mesh:
 body: static:
 textures[JSON] (
  "BodyMaterial": "Textures/body.png":
 )
)
```

An AI definition uses an imported model and aliases for scene navigation
points:

```zsharp
zsharp = type.ai
noticed ai Patrol[] (
 model: "Models/Suzanne.zmodel":
 visible: alive:
 body: dynamic:
 collider: box:
 mass: 1:
 gravityScale: 0:
 Navigation[] (
  first: "patrolA":
  second: "patrolB":
 )
)
```

Place it by `"id": "Patrol"` in the scene. A built-in, invisible point is
`{"id": "nav:patrolA", "name": "A", "location": {"x": 0, "y": 0, "z": 0}}`.
Then a game script can use:

```zsharp
Patrol.toPoint.glide(first, 6):
Patrol.toPoint.teleport(second):
if[Patrol.navStatus == "notReachable"] (
 Print("No route"):
) else (
)
```

`navStatus` is read-only: `moving`, `reachable`, or `notReachable`. Glide
uses authored X/Z navigation points when available and falls back to a
bounded automatic grid route around static geometry. An AI can query line of sight using
`Patrol.canSee.Player`, which returns `alive` or `dead` based on blockers'
3D bounds. Model definitions may use `collider: sphere:` or
`collider: capsule:` for radial 3D contacts; the existing `box` and 2D
`circle` collider behavior is unchanged. A **static** `.zmodel` may use
`collider: mesh:` for triangle contacts with dynamic spheres or capsules;
moving mesh colliders and exact box-to-mesh contact are not supported.
Navigation does not yet use polygon navmeshes or rotated obstacle hulls. A
`.zmodel` or `.zai` may be referenced by a `.zanimation` clip, including
root, FBX mesh-node, and FBX bone tracks.

## Solid 3D cubes

`shape: cube` now renders solid faces instead of only an edge outline. Faces
are clipped against the camera near plane, ordered by depth, and shaded so
their orientation remains visible while preserving the object's chosen color.



## Scene appearance, surfaces, and lighting

Scene object placements may set per-instance XYZ rotation without changing
their `.zobject` definition:

```zsharp
"rotations": {"x": 10, "y": 20, "z": 30}
```

This uses the existing `rotationX`, `rotationY`, and `rotationZ` runtime
properties. Scene placements can also specify `surface` values from 0 to
100: `opacity`, `roughness`, `emissive`, and `metallic`. Color and texture
remain independent optional appearance overrides. A built-in `light` entry
can provide a point, spot, or directional light:

```zsharp
{"id": "light", "name": "Lamp", "location": {"x": 0, "y": 20, "z": 0},
 "rotations": {"x": -45, "y": 0, "z": 0},
 "attributes": {"light_type": "spot", "intensity": 100,
   "color": "#FFFFFF", "castShadows": true, "range": 80, "angle": 45}}
```

Use a unique identifier-style scene `name` such as `Flashlight` to address
one placed light from a script (the built-in `id` remains `light`):

```zsharp
number.set:Flashlight.positionX = Player.positionX:
number.set:Flashlight.positionY = Player.positionY + 8:
number.set:Flashlight.positionZ = Player.positionZ:
Flashlight.visible.set: alive:
number.set:Flashlight.lightIntensity = 240:
Flashlight.castShadows.set: alive:
```

With a placement `instanceId` of `Flashlight`, scene-qualified access is:

```zsharp
number Brightness = Chapter1Floor1.Flashlight.lightIntensity:
number.set:Chapter1Floor1.Flashlight.lightIntensity = 100:
number.set:Chapter1Floor1.Flashlight.rotationY = 45:
```

The script must import its project's game scene feature, for example
`import the_pallet.game.scene():`. JSON placement attributes use `intensity`,
`range`, and `angle`; runtime paths use `lightIntensity`, `lightRange`, and
`lightAngle`, not `intensity`/`range`/`angle`.

Lights also expose `rotationX/Y/Z`, `color`, `lightType` (`point`, `spot`, or
`directional`), `lightRange`, and `lightAngle` at runtime. Scene objects expose
`opacity`, `roughness`, `emissive`, and `metallic` as 0–100 number properties.

The current renderer applies per-face lighting with an ambient floor and
hard shadows from opaque cubes, including rotated cubes, and coarse bounding-volume shadows
from models. Opaque 3D triangles use a per-pixel depth buffer and
perspective-correct texture coordinates. Transparency ordering, exact
triangle-cast model shadows and transparency sorting remain approximate.
FBX models, skinned bone tracks, root/mesh-node tracks, and grid-backed AI
are available in this development build, subject to the limitations above.

---

# Languages

Z# can interoperate with several other languages. C is also the extension path for trusted modules that register new Z# statement, block, and expression syntax.

In Z# 1.2.1.0, C modules can register value-producing expressions:

```c
registry->add_expression(registry->context, "Discord.Client",
                         "client", error, error_size);
```

The module's `zsharp_c_call_v1` handler receives function `client` with no
arguments and returns a `ZSharpCValue`. Return `ZSHARP_C_STATUS` with
`number = 1` for `alive` or `number = 0` for `dead` to use it directly:

```javascript
import c:my_project.C.Console():
// Inside a brain, after importing the module that registers the expression:
if[Discord.Client] (
 Print("Client available"):
)
```

Expressions may also return numbers or text for assignments, arithmetic,
comparisons, and function arguments. For example, register
`NativeSum({a}, {b})` and use `number Total = NativeSum(10, 20) + 1:`.
Placeholders accept expressions, including nested calls, following the custom
statement rules. Expression patterns omit the statement-ending colon.
Registrations cannot replace built-in syntax or overlap another registered
pattern. Distinct patterns may share a namespace, including `Discord.Init(...)`,
`Discord.client`, and `Discord.client({name})`. Handlers execute at runtime on each evaluation;
registration itself must remain side-effect-free. Existing C modules remain
compatible; modules using the appended `add_expression` API require a
1.2.1.0-or-newer compiler and should declare that project version requirement.

## Desktop language bridges and live objects (1.4.0.0 development)

This unreleased version includes the previous unreleased 1.3.1.0 desktop work.
C++ was already supported; the new import prefixes are `cs:`, `java:`, `go:` and
`rb:`. All language imports remain project-qualified, and calls within that
project may omit its PID, just like existing Python/C/C++ imports:

```zsharp
import cs:my_project.Models.Person():
import java:my_project.Models.Person():
import go:my_project.Go.Utilities():
import rb:my_project.Ruby.Utilities():

text Greeting = Function.call(java:Models.Person:greeting ["Ada"]):
```

Import only the languages/modules actually used by a room. These features are
desktop-only, not bZVM language connections. Importing a module does not install
a compiler/runtime or compile that module's source automatically.
Only load trusted foreign code: export annotations and import rules control
bridge visibility, not OS sandboxing. A foreign module still runs with the
application user's native filesystem/network/process permissions.

### Live foreign instances

Use the existing `object` value type and ordinary object-member syntax:

```zsharp
object Person = Foreign.new(java:Models.Person ["Ada"]):
text Name = Person.name:
text.set:Person.name = "Grace":
Person.greet[]:
Foreign.release(Person):
```

`Foreign.new` names the imported module and its exported class/factory. The
module's last path component supplies the constructor name (`Person` above).
Constructor arguments use the same square-bracket list as `Function.call`.
Factories may also return instances:

```zsharp
object Person = Function.call(java:Models.Person:make ["Ada"]):
```

The object is a live handle to one retained foreign instance, not a JSON copy.
Two independently created instances do not share their fields. Returning the
same instance preserves its identity. Field/property writes use the existing
typed setter matching the value being assigned. Instance methods currently use
the statement form (`Person.greet[]:`); assigning an instance-method result as
an expression is not included in this first implementation. Export a module
function/factory when a returned value is needed instead.
Foreign handles are currently owned by their originating language/module: they
cannot be passed directly to a different foreign module or another language.
Use ordinary values/records to transfer data, or pass a VM-owned Z# object proxy
when several integrations need to act on the same Z# state.

Releasing invalidates that handle and its aliases; later reads, writes and calls
fail clearly rather than accessing another allocation. Other instances remain
valid. Foreign module globals/instances are application-scoped, including calls
from separate window tasks; remaining handles are discarded when the ZVM exits.
Rebuild/restart the
ZVM application to reload edited foreign modules; source edits are not hot-loaded.

### Calls from another language into Z#

The SDKs support synchronous callbacks into Z# brains, using the calling room's
imports and normal visibility rules. For example, Java can call:

```java
Object result = ZSharp.call("Main:Main:Calculate", 20, 22);
```

The target Z# file/room/brain must be accessible to the room that entered the
foreign bridge. Callbacks do not bypass `silent` visibility, imports or project
dependency checks. A Z# object passed as an argument becomes a live SDK proxy,
not an unrestricted pointer. Its `get`/`set`/`invoke` operations act on the
original object's visible fields/methods. C# spells these `Get`/`Set`/`Invoke`.
VM-owned object proxies expire when their owning task/heap ends, even though
foreign module state may remain alive. A later use reports an expired handle
instead of dereferencing a freed Z# object; do not retain such a proxy beyond
its owner's lifetime.

Callbacks must happen synchronously on the thread currently executing the
foreign entry point. Saving a callback context for a background thread is not
supported. Callbacks can enter another idle runtime/module through Z#, but cannot
re-enter the same active worker: that fails instead of deadlocking. This also
includes another Java/C#/Python/Ruby file sharing that language's active worker.

Strings, booleans/statuses, null and homogeneous text/number arrays continue to
cross the bridge; the new wire bridge also supports homogeneous object arrays.
Numbers use tagged decimal text on the wire rather than first
being narrowed to a C `double`. Native language arithmetic is still limited by
its chosen numeric type: Java `BigDecimal`, C# `ExactNumber` and SDK exact-number
wrappers preserve values that do not fit native scalars. Native numeric
parameters are range checked; non-finite results are rejected. Explicit
string-keyed records travel by value where supported. Other exported objects
travel as live handles. Protocol frames are limited to 1 MiB, wire values to
32 nested levels in the VM, and normalized foreign numbers to 8,190 expanded
decimal digits. Scientific numeric results expand only within that bound;
an enormous exponent fails before allocating an enormous decimal string.
If a worker dies, times out or loses protocol synchronization, its session is
poisoned and later requests fail. The VM does not silently restart it or replay
a call that might already have changed application state. Restart the
application after correcting the underlying failure.

Python uses `from zsharp import export, call`; Java uses `ZSharp.call`; C# uses
`ZSharp.Runtime.Call`; JavaScript/Lua/Ruby use `ZSharp.call`; Go uses
`zsharp.Call`. Z# object proxies expose field reads/writes and method invocation.
Native C/C++/Rust/Kotlin modules opt into `zsharp_c_call_v2`,
`zsharp_cpp_call_v2`, `zsharp_rust_call_v2` or `zsharp_kotlin_call_v2` and the
request/host/result definitions in `zsharp_interop.h`. Existing scalar v1
entry points remain unchanged and continue to work, but do not acquire object
operations/callbacks automatically. The host pointer/context is borrowed only
for the current entry call; never retain it for later use.

### Java exports

```java
import ZSharp.ZSharp;
import ZSharp.ZSharp_Export;

@ZSharp_Export
public class Person {
    public String name;

    public Person(String name) {
        this.name = name;
    }

    public void greet() {
        System.out.println("Hello, " + name);
    }

    public static Person make(String name) {
        return new Person(name);
    }

    public static String greeting(String name) {
        return "Hello, " + name;
    }

    public static Object calculateInZSharp(int value) {
        return ZSharp.call("Main:Main:Calculate", value);
    }
}
```

Compile against `zsharp-java-worker.jar` and package the classes as
`Models/Person.zjava.jar`, beside `Models/Person.java`. Java 17+ is required;
users running a precompiled module do not need `javac`. `@ZSharp_Export` on a
public class opts its public constructors/methods/fields into the bridge.
Individual public members may be annotated instead. Private members and
inherited `Object` methods are not implicitly exported; `final` fields are
read-only. Ambiguous overloads fail explicitly. Varargs methods are not currently
bridge-callable; use a typed array parameter instead. Module classes and static
state persist in the worker until process exit. This Java-to-Z# callback SDK is
separate from the existing Java/Gradle library that launches a ZVM process.

### C# exports

```csharp
using ZSharp;

[ZSharpExport]
public class Person
{
    public string name { get; set; }

    public Person(string name)
    {
        this.name = name;
    }

    public void greet()
    {
        Console.WriteLine("Hello, " + name);
    }

    public static Person make(string name)
    {
        return new Person(name);
    }

    public static object? calculateInZSharp(int value)
    {
        return Runtime.Call("Main:Main:Calculate", value);
    }
}
```

Reference `ZSharp.Interop.dll` when building the user class library, then place
`Models/Person.zcs.dll` beside `Models/Person.cs`, with its managed dependencies.
The worker targets .NET 8; the matching .NET runtime must be installed.
`[ZSharpExport]` supports public classes/structs, constructors, methods,
properties and fields. A type attribute grants its declared public members;
private members/accessors remain inaccessible, and readonly fields/init-only
properties remain read-only after construction. Generic, `ref`/`out`, `params` and automatically awaited `Task`
methods are not included; export ordinary synchronous wrappers instead.
Overloads are resolved by compatible argument types, with clear errors for
ambiguous calls. Worker/module state persists across calls.

### Go and Ruby exports

Go modules register functions with `zsharp.Export("name", function)` and
constructors with `zsharp.ExportType("Person", constructor)`, then call
`zsharp.Serve()` from `main`. Only exported Go fields/methods are accessible.
The local SDK is `native/interop/go` (module name `zsharp`). Build the module as
`Utilities.zgo.exe` on Windows or `Utilities.zgo` on Linux/macOS, beside the
imported `Utilities.go`. Distribute the executable for each supported platform;
the end user's system does not need the Go compiler. Callbacks use
`zsharp.Call("File:Room:Brain", arguments...)` and return `(value, error)`.

Ruby modules use `require "zsharp"` and explicitly register functions or classes
with `ZSharp.export`, for example:

```ruby
require "zsharp"

ZSharp.export("greeting") { |name| "Hello, #{name}" }

class Person
  attr_accessor :name

  def initialize(name)
    @name = name
  end

  def greet
    puts "Hello, #{@name}"
  end
end

ZSharp.export(Person)
```

The `.rb` file is loaded by a persistent worker using the installed Ruby runtime.
Explicitly exported classes expose their public accessors and methods, not
private methods or inherited built-in reflection APIs. `ZSharp.call` invokes a
Z# brain; VM-owned objects expose proxy `get`, `set` and `invoke` operations.
Normal user output from Java/C#/Ruby/Go/Python workers is forwarded separately
from the result protocol. These workers are retained between calls rather than
starting and terminating once per function call.

## Python

Python imports always name the language and project ID. Import one module or
all Python modules in a project with:

```zsharp
import py:my_project.Python.Utilities():
import py:my_project.*():
```

Inside the same project, a call omits the project ID because the import has
already identified it:

```zsharp
text Greeting = Function.call(py:Python.Utilities:greet ["Z#"]):
number Total = Function.call(py:Python.Utilities:add [20, 22]):
```

The corresponding `Python/Utilities.py` file explicitly exports callable
functions:

```python
from zsharp import export

@export
def greet(name: str) -> str:
    return f"Hello, {name}!"

@export
def add(left: int, right: int) -> int:
    return left + right
```

Python interoperability supports Z# text, number, status, and null values,
mapped to Python `str`, numeric values, `bool`, and `None`. The 1.4.0.0 bridge
also supports explicitly exported classes/live objects, arrays/records and
synchronous callbacks through `from zsharp import call`. A Python
exception fails the Z# call and includes its Python traceback in the runtime
error. The official ZVM bundles Python; developers may set
`ZSHARP_PYTHON_RUNTIME` while testing a custom interpreter.

Python calls share one persistent interpreter process for the lifetime of the
ZVM process. Modules are cached by their resolved file path: globals and imported
modules survive subsequent calls, including calls from other Z# tasks. Calls
are serialized across tasks. Python remains available during `Shutdown[]`;
ZVM closes the worker at process exit, allowing up to two seconds for normal
Python cleanup before terminating a worker that remains alive. On Windows,
the worker is also owned by a kill-on-close job to prevent orphaned workers.
Restart the ZVM to reload edited Python modules. User Python output is forwarded
to standard error so it cannot corrupt the worker's result protocol.

## JavaScript

JavaScript is embedded in the ZVM; users do not need Node.js. Import a local
JavaScript module with its project PID and call exported functions through the
same value bridge used by other languages:

```javascript
import js:my_project.JavaScript.Utilities():

text Greeting = Function.call(js:JavaScript.Utilities:greeting ["Z#"]):
```

```javascript
export function greeting(name) {
  return `Hello, ${name}!`;
}
```

Arguments and return values support text, number, status/boolean, and null.
Homogeneous `text[]` and `number[]` values cross the bridge as JavaScript
arrays, and JavaScript may return arrays containing only strings or only
numbers. Empty JavaScript arrays become empty `text[]` values. Mixed arrays
and arrays of booleans are rejected because Z# does not have matching
general-purpose array value types. In 1.4.0.0, objects/classes can cross as live
handles; use `ZSharp.record({...})` for an explicit by-value record.
Named default exports and immediately-resolving `async` functions are also
supported. JavaScript exceptions retain their message and stack in Z# runtime
reports. Each call has a 64 MiB heap limit, a 1 MiB stack limit, and a five
second execution guard; promises that require an external browser/Node event
loop fail with a clear diagnostic because ZVM intentionally embeds no DOM or
Node APIs. In 1.4.0.0 these limits apply to each retained module's runtime;
globals and live instances survive later calls. `ZSharp.call` and Z# object
proxy `get`/`set`/`invoke` are available during synchronous foreign calls.
Very large/excess-precision numeric arguments are exact-number wrappers; use
`value.toString()` to retain their decimal text or explicitly convert with
`Number(...)` when losing precision is acceptable.

## Lua

Lua 5.5.1 is embedded in the ZVM, so users do not need a separate Lua
installation. Import a Lua module with the project PID, then call it with its
project-relative path:

```javascript
import lua:my_project.Lua.Utilities():

text Greeting = Function.call(lua:Lua.Utilities:greeting["Z#"]):
```

A Lua file can return a module table:

```lua
local utilities = {}

function utilities.greeting(name)
  return "Hello, " .. name .. "!"
end

return utilities
```

Global Lua functions are also supported when a file does not return a module
table. Arguments and results may be text, number, status/boolean, null/nil,
or homogeneous text and number arrays. Z# arrays use zero-based indexes while
Lua tables use their usual one-based indexes. Empty Lua arrays become empty
`text[]` values. In 1.4.0.0, non-array object tables cross as live handles and
`ZSharp.record(table)` explicitly copies a string-keyed record. Modules retain
their globals and instance state between calls. Constructor exports are ordinary
functions returning a new table/object; live instance methods use Lua's usual
self argument. `ZSharp.call` and Z# object proxy `get`/`set`/`invoke` support
synchronous calls back into the VM.

Lua errors retain a Lua traceback in the Z# runtime failure report. Each retained
module has a combined 64 MiB runtime/codec memory limit and calls have a
five-second execution guard. Lua support
requires `ZSharp: [1.1.2.0]:` or newer.

## C++

C++ modules use the same project-qualified import and short same-project call
syntax as the other embedded-language bridges:

```zsharp
import cpp:my_project.Cpp.Utilities():

text Greeting = Function.call(cpp:Cpp.Utilities:greeting["Z#"]):
number Total = Function.call(cpp:Cpp.Utilities:add[20, 22]):
```

The import corresponds to `Cpp/Utilities.cpp`. Its compiled bridge must sit
beside it as `Cpp/Utilities.zcpp.dll` on Windows,
`Cpp/Utilities.zcpp.so` on Linux, or `Cpp/Utilities.zcpp.dylib` on macOS.
Applications distribute that compiled module inside their package, so users
do not need a C++ compiler or development tools.

Include the installed `zsharp_cpp.h` header and export one bridge entry:

```cpp
#include <cstring>
#include "zsharp_cpp.h"

extern "C" ZSHARP_CPP_EXPORT int zsharp_cpp_call_v1(
    uint32_t abi, const char *function,
    const ZSharpCppValue *arguments, size_t argument_count,
    ZSharpCppValue *result, char *error, size_t error_size) {
    if (abi != ZSHARP_CPP_ABI_VERSION) return 0;
    if (std::strcmp(function, "answer") == 0 && argument_count == 0) {
        result->type = ZSHARP_CPP_NUMBER;
        result->number = 42;
        return 1;
    }
    return 0;
}
```

Bridge arguments and results support text, number, status, and null. Returned
text is copied by ZVM immediately after the call. C++ modules are cached by
full library path and remain loaded until process exit, preserving global
state and SDK callbacks between calls. The cache is shared across script
tasks; loading is synchronized, but module functions must synchronize their
own shared state if called concurrently. Call an SDK's explicit shutdown
function when appropriate; ZVM does not invent or automatically invoke one.
Restart the app/game after rebuilding a module to load the changed library.
C++ calls require
`ZSharp: [1.1.3.0]:` or newer.

## Rust

Rust modules use project-qualified imports and short same-project calls:

```zsharp
import rust:my_project.Rust.Utilities():

text Greeting = Function.call(rust:Rust.Utilities:greeting["Z#"]):
number Total = Function.call(rust:Rust.Utilities:add[20, 22]):
```

The import corresponds to `Rust/Utilities.rs`. Build it as a Rust `cdylib`
with the bridge entry `zsharp_rust_call_v1`, then place the compiled library
beside the source as `Rust/Utilities.zrust.dll` on Windows,
`Rust/Utilities.zrust.so` on Linux, or `Rust/Utilities.zrust.dylib` on macOS.
Package the module for each supported platform; the user's device does not
need Rust or Cargo installed. Rust source alone cannot be called by the ZVM.
For example, from the project root on Windows (with `zsharp_ffi.rs` copied
beside the Rust source):

```powershell
rustc --edition 2021 --crate-type cdylib Rust/Utilities.rs -o Rust/Utilities.zrust.dll
```

The installed `share/zsharp/rust/zsharp_ffi.rs` file defines the bridge's
`#[repr(C)]` value layout and constants. A module exports this function:

```rust
#[no_mangle]
pub unsafe extern "C" fn zsharp_rust_call_v1(
    abi_version: u32,
    function: *const std::ffi::c_char,
    arguments: *const ZSharpRustValue,
    argument_count: usize,
    result: *mut ZSharpRustValue,
    error: *mut std::ffi::c_char,
    error_size: usize,
) -> i32 {
    // Return 1 for success, 0 after writing an error message for failure.
    0
}
```

Include the value definitions from `zsharp_ffi.rs` in the Rust module. The
module must check the ABI version before reading arguments. Arguments and
results support text, number, status, and null. Returned text must remain
valid until the ZVM copies it, immediately after the call. The full example
is in `tests/rust_project/Rust/Utilities.rs`. Rust calls require
`ZSharp: [1.1.4.0]:` or newer. As with C++, the native desktop bridge is not
the Android VM-executed module format planned for a later release.

## Kotlin/Native

Kotlin modules use `kt:` imports and same-project calls:

```zsharp
import kt:my_project.Kotlin.Rules():
number Speed = Function.call(kt:Kotlin.Rules:platformSpeed[8]):
```

The corresponding native library is `Kotlin/Rules.zkt.dll` on Windows
(`.zkt.so` on Linux or `.zkt.dylib` on macOS). It exports
`zsharp_kotlin_call_v1` with the `zsharp_c.h` value ABI. The ZVM retains the
library for the process lifetime because unloading a Kotlin/Native runtime
after a call can crash later. This is a native bridge, not a JVM requirement.
The current package workflow requires compiling each Kotlin library ahead of
time for its target platform; it does **not** compile `.kt` source automatically.

## C

C modules use project-qualified imports and same-project calls:

```zsharp
import c:my_project.C.Utilities():

number Total = Function.call(c:C.Utilities:add[20, 22]):
```

The import corresponds to `C/Utilities.c`. Compile it as a shared library
beside the source: `C/Utilities.zc.dll` on Windows,
`C/Utilities.zc.so` on Linux, or `C/Utilities.zc.dylib` on macOS. The
installed `zsharp_c.h` header defines the ABI and value types. A module
exports `zsharp_c_call_v1` and receives the function name, arguments,
result, and error buffer. Text, number, status, and null can cross the bridge.
This bridge is version-gated to `ZSharp: [1.2.0.0]:` or newer. C source
alone is not executable by the ZVM; distribute compiled modules for each
supported platform.

### Extending Z# with C

An imported C module may also export `zsharp_c_register_v1` to add statement
patterns. The compiler loads that compiled module while checking and
packaging source, so only use trusted C modules. For example:

```c
ZSHARP_C_EXPORT int zsharp_c_register_v1(
    ZSharpCSyntaxRegistry *registry, char *error, size_t error_size) {
    return registry->add_statement(registry->context,
        "move {who} to {where}", "move", error, error_size) &&
        registry->add_statement(registry->context,
        "Movement.move({who}, {where})", "move", error, error_size) &&
        registry->add_block(registry->context,
        "Detections", "detections", error, error_size);
}
```

Both statements call the module's `zsharp_c_call_v1` with two arguments:

```zsharp
move "Player" to "Spawn":
Movement.move("Player", "Spawn"):
Detections(
 Windows: true:
 Linux: false:
 MacOS: true:
)
```

Patterns use literal Z# tokens and `{name}` placeholders. Each placeholder
accepts an expression, including variables, qualified properties, arithmetic,
and nested calls such as `Discord.Bot.Login(File.read(".env")):`. Arguments
are evaluated at runtime, not during registration. Commas and closing
parentheses inside nested calls do not split the outer arguments. A literal
operator in a registered pattern remains a separator (use parentheses around
a complex operand in that uncommon form). Adjacent placeholders without a
separator keep their original single-atom interpretation. The first word of a
custom pattern cannot be a built-in Z# keyword. Distinct patterns may share
their first word; overlapping token patterns are rejected. Assignment and bracket syntax are
also reserved. A pattern is available only in a script that imports its C
module. Registration must have no side effects because the compiler may call
it repeatedly.
Registered blocks pass alternating field-name text and evaluated values to
the C handler. `true` and `false` work inside these blocks as status values;
normal Z# code still uses `alive` and `dead`.

## Browser prototype (1.2.2.0, development)

The bZVM prototype executes Z# bytecode in WebAssembly. From 1.4.0.0, HTML may
select multiple entry `.zsharp` scripts in one shared VM. All entry files and
their explicitly imported project files are loaded and checked before startup;
a load or parse error prevents any entry's startup from running. The loader
automatically finds `project.zsettings` in the HTML document's
directory or nearest parent directory, up to the server root (32 levels maximum).
That settings file's directory is the project root; `data-project` is not needed.
Its old explicit URL override remains supported for compatibility. If automatic
discovery receives only HTTP 404 responses, standalone scripts without imports
still work. Other HTTP errors or invalid settings stop startup.
Settings are parsed inside WebAssembly by the same settings
parser as the native ZVM, not by a separate JavaScript grammar. Use the normal
`Project`, `PID`, `Version`, `Authors`, `Description`, `ZSharp`, and `Dependencies`
fields. The prototype currently requires `Dependencies ():` and rejects native
window/game configuration and unsupported newer runtime versions. Malformed or
missing explicitly linked settings stop startup with a clear error. The
`bzvm:ready` event's `detail.project` contains the project name, ID and root URL
when settings are enabled.
Load `bzvm.js` once with `defer` and include one or more
`type="text/zsharp"` script tags.
Place `bzvm.wasm` beside the loader and serve the page over HTTP, not `file:`.

```html
<script src="bzvm/bzvm.js" defer></script>
<script type="text/zsharp" src="Main.zsharp"></script>
<input id="input">
<button id="button">Greet</button>
<div id="greeting"></div>
```

```zsharp
zsharp = type.script
noticed room Main[] (
 import ZSharp.Browser():
 noticed brain Start[] (
  Browser.button.clicked: (
   text Name = Browser.input.value:
   if[Name != ""] (
    Browser.greeting.content.set: "Hello, " + Name + "! Count: " + 10:
    wait(5s):
    Browser.greeting.content.set: "Five seconds later, " + Name:
   ) else (
    Browser.greeting.content.set: "Enter your name first.":
   )
  ):
 )
)
```

`if`, `else if`, `else` and text concatenation work in this subset. Joining text
with numbers/status values converts those fields to text. `.value` reads an
input's text; `.content` maps to `textContent`; `.html` maps to `innerHTML` and
preserves the outer element. Paths require a unique HTML ID that is also a Z#
identifier: use `bar_title`, not `bar-title`. Input `.contents` and `.length`
are not implemented. `.style.PROPERTY.set:` accepts CSS strings.

Click handlers may be inline blocks ending in `):`, or use
`Browser.button.clicked: Function.call(Main:Main:Greet):`. Registration does not
execute the body. Each click starts an independent task. Inline handlers have
fresh local variables, not captured locals from the registering function.

`wait(5s):`, `wait(16ms):` and `wait(0ms):` yield to browser timers and resume
the same task, preserving its locals and nested call stack. Other clicks remain
responsive. Wait durations are literal nonnegative values, limited to 2147483647
milliseconds; fractional milliseconds round up in the loader. Browser throttling
can delay timers, especially in background tabs. Loops should include a wait;
each uninterrupted task slice has a 100000-instruction safety limit. Leaving
the page cancels pending tasks.

The browser runtime supports typed function parameters, `feed` return values
(including calls suspended by waits), function-reference parameters, persistent
room variables, visibility checks, indexed text/number arrays, `Math` and
`Regex`. Arithmetic uses the desktop VM's decimal routines. Explicit project
imports and cross-file calls are supported as described below.

This is not yet full desktop language parity: object/class instance execution,
file writes/directory searches and additional browser events still need browser
runtime implementations. Native apps/games and foreign-language integrations
are outside the browser target. Unsupported instructions report an error rather
than silently executing with different semantics.

### Multiple HTML entry scripts (1.4.0.0)

Several `type="text/zsharp"` tags may share one loader and VM. For example,
with PID `my_site` in `project.zsettings`:

```html
<nav id="navbar"></nav>
<div id="greeting"></div>
<script src="bzvm/bzvm.js" defer></script>
<script type="text/zsharp" src="Global.zsharp"></script>
<script type="text/zsharp" src="Main.zsharp"></script>
```

```zsharp
// Global.zsharp
zsharp = type.script
noticed room Global[] (
 import ZSharp.Browser():
 noticed brain Start[] (
  Browser.navbar.html.set: '''<a href="/">Home</a>''':
 )
 noticed brain ShowHome[] (
  Browser.greeting.content.set: "Welcome!":
 )
)
```

```zsharp
// Main.zsharp
zsharp = type.script
noticed room Main[] (
 import my_site.Global():
 noticed brain Start[] (
  Function.call(Global:Global:ShowHome):
 )
)
```

All entries and imports load before any automatic startup. Each distinct
explicit entry's zero-parameter automatic `Start[]` brains begin once in HTML
tag order. A `wait(...)` suspends only that task: later entries start without
waiting for it to finish. The VM, room fields, callbacks and project settings
are shared; the entries are not isolated applications.

Listing a file in HTML does not import it into other rooms. Cross-file reads
and calls still require the normal PID-qualified import and visibility rules.
Files reached only through imports do not automatically run `Start[]`. If
`Global.zsharp` is imported but not explicitly listed in HTML, call
`Function.call(Global:Global:Start):` when initialization is needed. Do not also
make that call when Global is an explicit entry unless intentionally running
its initialization again.

Repeating the same external source URL loads and starts that file only once.
Cache-busting query strings and fragments do not create a second instance;
the first explicit tag's URL is used. Distinct files must still have unique
filenames across the loaded project; ambiguous duplicates produce an error.

Inline `text/zsharp` entries are supported too. A sole inline entry keeps the
default filename `Main`. On a page with several Z# entry tags, unnamed inline
entries use `Inline1`, `Inline2`, and so on, according to their one-based Z# tag
position, including external tags. Set `data-name="Utilities"` on an inline tag
to give it a stable filename for imports/call targets; that name must also be
unique. External entries always use their actual source filename.

### Browser JSON and project asset reads (1.3.0.0)

Use the same JSON schemas and `import ZSharp.JSON():` as on desktop:

```javascript
noticed room Data[] (
 import ZSharp.JSON():
 noticed JSON PersonSchema[] ( name: text: score: number: enabled: status: )
 noticed brain Load[] (
  text Path = "Data/Person.json":
  JSON Person = JSON.load(PersonSchema, Path):
  Print(Person.name + " " + Person.score + " " + Person.enabled):
  status Exists = File.exists(Path):
  text Source = File.read(Path):
 )
)
```

Room-level JSON values, bracket keys and typed field reads/writes use the
desktop syntax too. Field changes affect the loaded value, not its source file.
Required keys and text/number/status types must match the schema; undeclared
keys, malformed JSON and wrong types fail. Decimal tokens preserve their digits.

The browser fetches public assets from the same-origin project root (or page
directory without settings). Paths cannot escape that root. Downloads yield
without blocking other tasks, time out after 15 seconds, and are cached for the
page lifetime: at most 128 files, each smaller than 64 KiB. Reload to fetch edited
assets. `File.exists` returns `dead` for HTTP 404; network/access errors are not
silently treated as missing files. This is not access to a visitor's disk or a
way to write/delete server files. Do not place secrets in public project assets.

### Browser project imports and cross-file calls

Use the settings PID and explicit project-relative paths. Given PID `my_site`,
`import my_site.Scripts.Messages():` loads `Scripts/Messages.zsharp` from the
project root. URL/file capitalization must match the actual files. The browser
does not recursively search directories or support wildcard imports yet.

```zsharp
// Main.zsharp
zsharp = type.script
noticed room Main[] (
 import ZSharp.Browser():
 import my_site.Scripts.Messages():
 noticed brain Start[] (
  Function.call(Messages:Messages:Show):
  Browser.button.clicked: Function.call(Messages:Messages:Show):
 )
)
```

```zsharp
// Scripts/Messages.zsharp
zsharp = type.script
noticed room Messages[] (
 import ZSharp.Browser():
 noticed brain Show[] (
  Browser.greeting.content.set: "Hello from another file":
  wait(1s):
  Browser.greeting.content.set: "The same task resumed":
 )
)
```

The call target uses `File:Room:Brain`; a same-project PID prefix is also accepted
by normal `Function.call`. The calling room must import a foreign file, and the
target room/brain must be `noticed`. Script filenames must be unique across the
loaded project. Each file is loaded once; circular imports are supported.
Only explicitly listed HTML entry scripts' automatic `Start[]` brains run, not
imported-only files' startup brains. Multiple HTML entries require bZVM 1.4.0.0
or newer. Calls and click targets can cross files and preserve waits.
The first subset supports zero-argument, void brains only (no arguments or return
values yet), at most 64 files and 8 MiB total source. Missing/duplicate files,
missing imports, hidden targets and external project dependencies produce errors.

Raw multiline strings use `'''...'''`. Embedded browser-only blocks support
`JavaScript[( ... )]:` and `CSS[ClassName, ( ... )]:`. CSS bodies contain
declarations applied to `.ClassName`; they do not create elements. Embedded
JavaScript executes with normal page privileges and may require CSP permission.
