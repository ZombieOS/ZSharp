# Z# language specification notes

This file records syntax provided by the language designer. Unexplained syntax
is intentionally left uninterpreted until its behavior is defined.

## Official source extension

Z# source files use `.zsharp`.

## Project settings

Every project has one `project.zsettings` file in its root. It is a settings
script, so it has no room:

```zsharp
zsharp = type.settings

Project: "Project Display Name":
PID: "project_id":
Version: [1.0.0.0]:
Authors: ["Author1", "Author2"]:
Description: "This is a Z# Project!":
ZSharp: [1.0.0.0]:

Dependencies (
 playfab:1.0.0.0
):
```

- `Project` is the display name.
- `PID` is the identifier used for imports and qualified project paths. It must
  be lowercase and may contain numbers and underscores. Spaces are changed to
  underscores during compilation.
- `Version` is the project's four-part version used by dependent projects.
- `Authors` may contain any number of author names.
- `Description` uses `\n` for a new line.
- `ZSharp` is the four-part language version. Its first number selects the
  generation, such as `1.0.0.0` for Z1.
- A dependency is written as `projectId:projectVersion`.
- Dependency resolution is intended to locate a published project online by
  PID, verify that the requested version exists, and prevent two published
  projects from owning the same PID.
- Compilation derives a permanent project identity from the normalized PID and
  embeds it in bytecode. It is not an editable settings field. The registry is
  responsible for ensuring that only one project owns each PID.
- Every compiled bytecode file also receives a SHA-256 integrity hash. It
  changes with compiled content, and the VM refuses to run a bytecode file when
  the stored hash no longer matches.

## First syntax sample

The first supplied Hello World example is preserved as provided:

```zsharp
zsharp = type.script

noticed room Test[] (
 noticed text HelloWorld = "Hello World": // this is a text variable
 noticed number Visits = 1: // this is a number variable
  noticed brain Start[] ( // this is declaring a function. ill explain what noticed and brain mean
  Print(HelloWorld): // this is printing Hello World to console
  Function.call(FIILE:ROOM:FUNCTION): // example being Function.call(Home:Main:
 )
)
```

## Confirmed observations

- A script begins with `zsharp = type.script`.
- `//` begins a line comment.
- `noticed` has the same visibility meaning as C# `public`.
- `silent` has the same visibility meaning as C# `private`.
- `text` is the Z# equivalent of C# `string`.
- `number` is the Z# equivalent of C# `int`.
- `room` is the Z# equivalent of C# `class`.
- `brain` is the Z# equivalent of C# `void` and declares a function.
- `Print(HelloWorld):` prints the value stored in `HelloWorld`.
- Statements shown inside a room end with `:`.
- `Function.call` uses a colon-separated location shaped like
  `FIILE:ROOM:FUNCTION`.
- The completed call example is
  `Function.call(Home:Main:Start_Outcome):`.
- The file portion of `Function.call` searches the entire project recursively
  for a Z# file with that name. `Home` therefore resolves to `Home.zsharp`, even
  when it is in another project folder.
- A brain named `Start` runs automatically when its file or room is loaded.
- `Start[DR]` disables that automatic run behavior. The brain may still be
  called explicitly.
- `number.set:Score = Score + 5:` assigns a new number value.
- `+` performs number addition.
- `-`, `*`, `/`, and `%` perform subtraction, multiplication, division, and
  remainder. Unary `-` negates a number.
- A number-returning function uses the return type in place of `brain`, as in
  `noticed number Add[number Left, number Right] (...)`.
- A text-returning function likewise uses `text` in place of `brain` and feeds
  a text value.
- Function parameters are declared inside `[]`.
- `feed(number.Left + number.Right):` supplies the returned number.
- A local number can be declared without visibility inside a function, as in
  `number Result = ...:`.
- Conditions use `if[condition] (...) else (...)`.
- `>=` compares two numbers.
- `Print` accepts direct text values as well as named values.
- A text array uses `text()` and an array value uses square brackets:
  `noticed text() Names = ["Alex", "Sam", "Robin"]:`.
- Array indexing uses square brackets, as in `Names[1]`.
- Array elements are changed with `Names.set[1] = "Zombie":`, and array length
  is read as `Names.Length`.
- Other array types use the same `()` marker, such as
  `number() Scores` and `Player() Players`.
- `loop (...)` repeats its body without a built-in condition. Z# loops do not
  accept conditions in the `loop` declaration.
- `loop.end:` exits the innermost loop.
- `continue:` immediately begins the next iteration of the innermost loop.
- `status` is a two-state type. `alive` is true and `dead` is false.
- A status can be changed directly, as in `Alive = dead:`.
- Logical conjunction is written with `and`.
- Logical disjunction is written with `or`.
- Logical negation is written with `not`.
- `>` compares two numbers and produces a status.
- `==` and `!=` test equality and inequality. Comparing different value types
  is an error. `<=` compares two numbers.
- A room may be declared without a visibility word, as in `room Player[] (...)`.
- A field may omit an initial value, as in `noticed text Name:`.
- A constructor is a function named after its room:
  `noticed Player[text name] (...)`.
- `text.set.Name = name:` assigns a text field on the current object.
- `+` concatenates two text values, as in
  `text.set.Name = Name + "!":`.
- Object construction uses `new` and square-bracketed arguments:
  `noticed Player User = new Player["Zombie"]:`.
- Instance methods use square-bracketed arguments:
  `User.Move[10, 20]:`.
- Member access uses `.`, as in `User.Name`.
- A field on a named object is assigned with `set`, as in
  `User.set.X = User.X + x:`.
- `addition(4 + 6)` is the confirmed built-in addition form and evaluates to
  the resulting number.
- Every declared function type is invoked through `Function.call(...)`.
  Direct invocation such as `Add(1, 2)` is not valid Z#.
- Function arguments are placed in `[]` after the colon-separated target but
  inside the call parentheses. A returned value can be assigned directly:
  `number Result = Function.call(Math:Calculator:Add [4, 6]):`.
- Assigning a number result to text converts the number to text. Assigning a
  text result to number prints a recoverable runtime warning, skips the
  assignment, and leaves a newly declared number at `0`.
- A brain can expose a named result block with
  `if(Result)[condition] (...)`. Its value is supplied by `feed(value):`.
- A caller selects that result after the closing call parenthesis, as in
  `number Value = Function.call(File:Room:Brain [arguments])[Result]:`.
  Only brains accept named-result selectors. Using a brain as a value without
  a selector, or requesting a name that it does not declare, is a compile
  error.
- The selected named `if` condition is checked when the brain is called. Its
  `else`, when present, may feed a different value. If the selected path ends
  without `feed(value):`, its result is `null`.
- A named brain result has the type of the value reached by `feed(...)`.
  Assignment conversion and error behavior therefore depend on the actual
  path taken at runtime.
- `feed:` ends the current function without supplying a value.
- When an `if` condition is not met and the `if` has no `else`, the current
  function ends immediately. An explicit `else`, including an empty one,
  continues normal control flow.
- A value in another room in the same file is read as `Room.Var`.
- A value in another Z# file in the same project is read as `File.Room.Var`.
  The file search is recursive and considers only `.zsharp` files.
- A value supplied by another project is read as `Project.File.Room.Var`.
- A function in another project is called as
  `Function.call(Project.File.Room.Function):`.
- The other project does not have to be written in Z#. A native provider maps
  its variables and functions into Z# and may call a C, C++, Java, C#, SDK, or
  service implementation internally.
- Object field access and same-file room access both have two names. For
  `First.Second`, a visible object named `First` takes priority; otherwise
  `First` is treated as a room.
- Object methods use the same location ladder:
  `User.Move[...]`, `Room.User.Move[...]`,
  `File.Room.User.Move[...]`, or
  `Project.File.Room.User.Move[...]`.
- Object fields use the same ladder, including
  `Project.File.Room.User.Name`. Field writes retain `set`, such as
  `File.Room.User.set.Name = "Zombie":`.
- Qualified variables can be written as well as read. This includes values in
  another project when its provider permits writes.
- The missing-value literal is `null`.
- New objects can be supplied when an object array is declared and added later:

  ```zsharp
  noticed Player() Players = [new Player["Zombie"]]:
  Players.add(new Player["Alex"]):
  ```
- `horde` is the Z# word for the concept called `static` in C# and Java. It is
  placed after visibility, as in
  `noticed horde number Score = 10:`. A horde field belongs to its room type
  and is shared by every object of that room.
- A horde function belongs to its room, is called with `Function.call`, and
  cannot access instance fields directly. A horde function cannot be invoked
  through an object.
- A horde room is declared as `noticed horde room Tools[] (...)`. It cannot be
  created with `new`, and all of its fields and functions must be horde.
- A Z# `number` is an arbitrarily sized ordinary decimal number. Decimal points
  are valid, but exponent/scientific notation such as `1e+10` is not.
- Division preserves the complete result when its decimal terminates. When it
  repeats forever, it keeps only the first digit after the decimal point. Thus
  `1 / 3` is `0.3`, while `3 / 1` is `3`.

## Imports

- Code in another file or project must be imported before a room can read it,
  write it, or call it.
- Imports are declared inside a room and apply only to that room.
- Importing a file makes that file available; rooms are not imported
  individually.
- A direct file import is written as `import Project.File():`.
- A file inside folders is written as
  `import Project.Folder.File():`. Additional folder names are allowed.
- Visibility rules still apply after import.
- Missing imports and resolvable visibility/name errors are compile errors.
- For the current project, `Project` is its `PID`. For an external project it
  must match a PID listed under `Dependencies`.

## Nested rooms

Rooms may be declared inside rooms:

```zsharp
noticed room RoomA[] (
 silent room RoomB[] (
 )
)
```

A nested `silent room` is visible to its direct parent and to itself. It is not
visible to unrelated rooms. A top-level `silent room` remains inaccessible to
other rooms.

## Compile diagnostics and language generations

- Syntax errors include unknown or misspelled language words, unmatched room
  or function delimiters, and missing `:` statement terminators.
- Using code from a different file/project without a room-scoped import is a
  compile error.
- Recoverable runtime type mistakes print a warning, skip the failed
  assignment, and continue. For example, assigning text output to a number
  declaration leaves that number at its default value of `0`.
- Runtime failures that prevent meaningful continuation terminate the program.
  Examples include division by zero, missing dependencies, and invalid object
  construction.
- Z# generations will be named `Z1`, `Z2`, `Z3`, and so on, similarly to HTML
  generations.
- The generation is selected by the first part of `ZSharp` in
  `project.zsettings`. The current compiler implements Z1 and rejects a project
  requesting a newer generation.
- Older-generation syntax may produce a migration warning shaped like:
  `Your code: '____' is from Z1 and will not work in Z3. It is recommended to
  use '____' instead`.
- Z1 does not produce future-generation migration warnings. These warnings
  begin only after Z2 exists and defines actual Z1-to-Z2 replacements.
- Migration warnings can be implemented once the first old-syntax to
  replacement-syntax mappings are defined.

## Room visibility

- `noticed room` is public throughout the project.
- A room with no visibility word is file-only. Other rooms in the same file can
  call it, but rooms in other files cannot.
- A nested `silent room` can be called only by its parent room.
- A top-level `silent room` cannot be called by another room.
- A room can still execute its own functions internally.

## Awaiting definition

- Whether instance calls such as `User.Move[...]` remain the special method-call
  form or also change to `Function.call`.
- What the letters `DR` stand for and whether other `[]` options exist.
- Whether capitalization is significant.
- Whether files can contain brains outside a room.
- Whether loading another file for `Function.call` also auto-runs that file's
  `Start[]` functions. The runtime currently invokes only the requested target.
- How tools should discover the project root when invoked from a subfolder. The
  toolchain currently uses its working directory and expects
  `project.zsettings` there.
- What should happen if multiple project folders contain a Z# file with the
  same name. The toolchain currently reports that as an ambiguous call.
- How a dependency PID is resolved to a Z# project folder or a provider
  library. Dependencies are permanent in `project.zsettings`; native provider
  locations are currently supplied at run time.
- Which authoritative online registry owns PID uniqueness and provides trusted
  version metadata and downloads.
- How the future registry signs project identities and builds to prove
  publisher ownership. The current hashes detect accidental or unsophisticated
  bytecode changes but are not a substitute for a registry signature.
- How arguments and return values cross an external-project function call.
  Provider ABI v1 supports the currently defined no-argument, value-less
  `Function.call` form.
- Which additional external variable and method-argument types are supported.
  Provider ABI v1 supports `number`, `text`, and `status`.
- How service authentication and asynchronous results work. For example, a
  PlayFab provider must own the PlayFab login/session and translate its SDK
  result into a Z# value.
- How named brain results map into engine-specific 2D/3D output types.
- The syntax for a one-time horde-room initializer, if Z# needs the equivalent
  of a C# static constructor.
- The list of Z1 syntax that is replaced in Z2, including each recommended
  replacement used in migration warnings.
