# Z# syntax guide

This guide explains how to write Z# and compares its concepts with C#, Java,
and C. The official source extension is `.zsharp`.

Z# 1.0.2.0 implements the Z1 compiler and virtual machine plus the specialized
window, 2D, and 3D script headers in this guide. Window syntax is accepted,
validated, stored in bytecode, and rendered by native Windows, Linux, and macOS
`zsharpwindow` backends. `.zapp` and `.zgame` packaging is implemented.

For the underlying design record, see [LANGUAGE.md](LANGUAGE.md). For bytecode
details, see [BYTECODE.md](BYTECODE.md).

## 1. Files and project layout

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

The specialized script headers are:

```zsharp
zsharp = type.script:window
zsharp = type.script:2D
zsharp = type.script:3D
```

These identify window, 2D game, and 3D game scripts respectively.

### Game projects

Every project containing a 2D or 3D script must enable the official game
runtime in `project.zsettings`:

```zsharp
Dependencies (
 zsharpgame:1.0.0.0
):
```

The room and function syntax inside a game script is the normal Z# syntax. A
non-`DR` `Start[]` runs automatically after the SDL3 window and Vulkan renderer
are ready. Eligible starts across the project run as independent tasks, so an
endless game loop does not block window events or rendering. Closing the window
or pressing Escape cancels and joins those tasks.

The 1.0.2.0 runtime initializes SDL video, audio, keyboard, mouse, and gamepad
support, creates a high-DPI resizable game window, and forces Vulkan for game
drawing. Windows and Linux are the advertised game targets. A bundled MoltenVK
path exists for macOS, but it is experimental until tested on Mac hardware.

Game scenes and objects live in `.zobject` files. A 2D object file starts with:

```zsharp
zsharp = type.object:2D
```

Use `type.object:3D` with a 3D startup script. Mixing 2D and 3D object files in
one running game is a compile error. Coordinates start at the center of the
game view: positive X moves right, negative X moves left, positive Y moves up,
and negative Y moves down.

Define one or more scenes:

```zsharp
noticed scene Main[] (
 background: #08080B:
 gravityX: 0:
 gravityY: -900:
 gravityZ: 0:
 cameraX: 0:
 cameraY: 0:
 cameraZ: 8:
 cameraFov: 70:
)
```

The first scene is active at launch. `cameraZ` and `cameraFov` mainly affect
3D games. A project with no explicit scene receives a default `Main` scene.

Define a 2D object like this:

```zsharp
noticed object Player[] (
 scene: Main:
 shape: rectangle:
 positionX: -250:
 positionY: -200:
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

 controlX: 390:
 jumpSpeed: 560:
)
```

Supported shapes are `rectangle`, `circle`, `triangle`, `sprite`, `cube`,
and `text`. `text` objects use a quoted `text:` field. The first sprite loader
accepts BMP assets through `asset:`; primitive shapes need no external asset.
`cube` is the initial 3D primitive and uses `positionZ`, `depth`, `scaleZ`, and
the active scene camera.

Bodies can be `static`, `dynamic`, or `kinematic`. Dynamic bodies receive
gravity and collision response. Kinematic bodies use velocity but do not
receive gravity. Colliders can be `none`, `box`, or `circle`; the current
collision solver uses their world bounds, exposes grounded/colliding state,
and supports `trigger: alive:` for overlap-only objects. `restitution` controls
bounce and `friction` slows horizontal movement on a surface.

`controlX` maps A/D, Left/Right, and gamepad horizontal input to velocity.
`controlY` does the same for vertical input. `jumpSpeed` maps W, Up, Space, and
the primary gamepad button to jumping while grounded. These automatic controls
are optional; scripts can read input and change properties directly.

Game runtime reads include:

```zsharp
Input.left
Input.right
Input.up
Input.down
Input.space
Input.action
Input.mouseLeft
Input.mouseRight
Input.mouseX
Input.mouseY

Game.scene
Game.delta
Game.elapsed
Game.fps

Player.positionX
Player.velocityY
Player.grounded
Player.colliding
```

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
```

Calculated number and text changes use the normal typed assignment forms:

```zsharp
number.set:Player.positionX = Player.positionX + 5:
number.set:Player.rotation = Player.rotation + Game.delta * 90:
text.set.Player.text = "Score: " + Score:
number.set:Main.cameraX = Player.positionX:
```

An object can be qualified with its scene as
`Main.Player.positionX`. Input and timing properties are read-only, as are
`grounded` and `colliding`. Scene changes, rendering, physics, and script loops
run concurrently, so `wait(...)` does not freeze the game window.

WAV audio uses a safe project-relative path:

```zsharp
noticed object BounceSound[] (
 scene: Main:
 visible: dead:
 audio: "assets/audio/bounce.wav":
 audioVolume: 0.4:
 audioLoop: dead:
 audioAutoplay: dead:
 audioOnCollision: alive:
 collider: box:
)
```

For small effects that need no asset, generate a tone with `tone:` (frequency
in hertz) and `toneDuration:` (seconds). `audioAutoplay` starts it when the game
loads, `audioOnCollision` starts it on a new collision, and
`Object.audioPlay.set: alive:` starts it from a script.

The complete playable example is in `examples/test-game`. Package both the
bytecoded and source forms with:

```text
zsharp package game "path/to/examples/test-game" ZSharpGameTest --unbytecode
```

### ZSS game styling

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
load, so a later ZSS rule overrides an earlier object value. In 1.0.2.0 ZSS
styles the game fields supported by `.zobject`; web-only layout and browser DOM
properties do not apply to native game objects.

## 2. Project settings

A complete basic settings file is:

```zsharp
zsharp = type.settings

Project: "Project Display Name":
PID: "project_id":
Version: [1.0.0.0]:
Authors: ["Author1", "Author2"]:
Description: "This is a Z# Project!":
ZSharp: [1.0.2.0]:

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

## 3. Comments and terminators

`//` begins a comment that continues to the end of its line:

```zsharp
noticed text Message = "Hello": // This is a comment.
```

Statements end with `:` rather than `;`:

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

## 4. Rooms

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

## 5. Values and types

### Text

`text` stores text values:

```zsharp
noticed text Name = "Zombie":
text LocalMessage = "Hello":
```

It is closest to C# `string` and Java `String`. C normally represents text
with a character array or pointer.

Text concatenation uses `+`:

```zsharp
text Message = "Hello, " + Name:
```

### Numbers

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

### Status

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

### Null

The missing-value literal is `null`:

```zsharp
noticed text Missing = null:
```

### Default field values

A room field may omit its initial value:

```zsharp
noticed room Player[] (
 noticed text Name:
 noticed number X:
)
```

## 6. Visibility

`noticed` means code outside the current scope may access the declaration:

```zsharp
noticed text PublicMessage = "Visible":
```

`silent` restricts access:

```zsharp
silent text Secret = "Hidden":
```

A declaration with no visibility word follows its enclosing file or room
visibility rules.

## 7. Functions

### Brain functions

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

### Number-returning functions

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

### Text-returning functions

Use `text` in place of `brain`:

```zsharp
noticed text Greeting[text Name] (
 feed("Hello, " + Name):
)
```

### Returning without a value

`feed:` ends the current function without a value:

```zsharp
noticed brain StopEarly[] (
 feed:
 Print("This does not run"):
)
```

### Start functions

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

### Timing statements

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

### Function calls

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

## 8. Local values and assignment

Local values omit visibility:

```zsharp
noticed brain Start[] (
 number Score = 10:
 text Name = "Zombie":
 status Alive = alive:
)
```

A number field is changed with `number.set:`:

```zsharp
noticed number Score = 10:
number.set:Score = Score + 5:
```

A text field on the current object is changed with `text.set`:

```zsharp
text.set.Name = Name + "!":
```

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

## 9. Conditions

Conditions use square brackets:

```zsharp
if[Visits >= 10] (
 Print("Returning visitor"):
) else (
 Print("New visitor"):
)
```

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

## 10. Named brain outcomes

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

## 11. Loops

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

## 12. Arrays

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

## 13. Objects and constructors

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

## 14. Horde members

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

## 15. Imports

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

## 16. Qualified names

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

## 17. External projects and native providers

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

## 18. Errors and warnings

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

## 19. Compilation and bytecode

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

## 20. Java and Z# virtual machines

The current Java integration uses two execution systems:

```text
Java/Kotlin code -> JVM (`java`)
Z# bytecode      -> ZVM (`zsharp`)
```

A Java application starts on the JVM. When it uses
`com.zombieos:zsharp:1.0.2.0`, the library locates or extracts the bundled
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

## 21. Window scripts

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

### Window design

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

### Responsive layout and scrolling

The full display is the window layout's reference canvas. At full width,
elements use their normal sizes and positions. As the user narrows the window,
images, buttons, and text inputs scale down horizontally with the available
width. Text keeps a readable font size and wraps onto additional lines instead
of disappearing outside the window.

Reducing only the window height does not shrink the layout. Content below the
visible area remains at its normal size and the window gains vertical
scrolling. Horizontal scrolling is not used.

### Coordinates

Window and UI element coordinates use their center as `(0, 0)`:

```text
positive X -> right
negative X -> left
positive Y -> up
negative Y -> down
```

### UI measurements

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

### Window text

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

### Buttons

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

### Images

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

### Text and image input

```zsharp
noticed textInput Input1[] (
 display: "This is an input box.":
 type: text:
 multiline: alive:
 wrap: alive:
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

Text inputs are single-line by default. Add `multiline: alive:` to allow line
breaks. A multiline input wraps long lines by default; use `wrap: dead:` when
long lines should remain on one line and scroll horizontally instead. `wrap`
cannot be set unless multiline mode is alive, and neither field is valid for
an image input.

Native window scrollbars remain hidden while all elements fit within the
visible window. A vertical scrollbar appears automatically when content
extends below the viewport. Multiline input scrollbars are handled inside the
input itself.

### ZSS window styling

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

The 1.0.2.0 native window style pass supports `background`, `color`, solid
`border`, `border-color`, `border-radius`, `font-family`, `font-size`,
`font-weight`, `padding` and its four directional forms, `caret-color`,
`outline: none`, `selection-background`, and `selection-color`. `:hover`
targets buttons and `:focus` targets text inputs. ZSS rules are applied in
sorted file order and override matching values written in the window script.
Selectors and declarations are checked while the project compiles. ZSS is
CSS-shaped, but it styles native controls rather than a browser DOM, so
unsupported web-only properties produce a compile error.

### Changing live window attributes

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

The setter supports these live fields in 1.0.2.0:

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

## 22. Window project settings

```zsharp
zsharp = type.settings

Project: "My Application":
PID: "my_application":
Version: [1.0.0.0]:
Authors: ["Author"]:
Description: "A Z# application":
ZSharp: [1.0.2.0]:

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

## 23. Application and game packages

Z# applications use `.zapp` and games use `.zgame`.

These are cross-platform Z# container formats. The normal 1.0.2.0 container
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

The packager checks `project.zsettings` and validates every included `.zsharp`
and `.zobject` file. An app requires a configured `Window Startup`; a game requires
`zsharpgame:1.0.0.0` and at least one `type.script:2D` or `type.script:3D`
file. A game prefers a 3D startup when both types exist. Renaming an
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
The future hub, app-data ledger, and optional ZOS Cloud backup are not part of
1.0.2.0. See [UNINSTALL.md](UNINSTALL.md) for current behavior and the planned
full safety model.

`.zgame` uses the same secure package, cache, association, shortcut, and
uninstall foundation as `.zapp`. Opening one launches its selected 2D or 3D
startup, loads and validates the project's `.zobject` scenes, starts every
eligible non-`DR` `Start[]` task, and enters the SDL3/Vulkan game loop. Windows
and Linux are the supported game targets for 1.0.2.0. The macOS/MoltenVK game
path is experimental and is not advertised as supported until hardware tests
are completed.

## 24. Complete current console example

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

## 25. Keyword comparison summary

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

## 26. Versioning and generations

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
therefore continues to run on ZVM 1.0.2.0 and automatically receives runtime
fixes such as smoother window painting and silent Desktop launches; its package
does not need to be rebuilt. The `ZSharp` version in `project.zsettings`
describes the source version the project targets. New source fields and syntax
must be added to the project's code before the application can use them, while
runtime-only fixes apply automatically. A current Z1 runtime accepts older Z1
projects but still rejects projects that require an unreleased future
generation.

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
