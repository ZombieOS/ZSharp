# Z# for Python

Call a Z# function from a regular Python program using the installed ZVM
1.4.1.1 or later. The package has no Python runtime dependencies and does not
download or install ZVM. Install the matching ZVM separately from the
[official Z# website](https://zsharp.zombieos.com).

The PyPI distribution is named `zombieos-zsharp`. Once it has been published,
install it into your Python environment with:

```sh
python -m pip install zombieos-zsharp
```

The Python import remains `from zsharp import call`. The unrelated PyPI
distribution named `zsharp` is not ZombieOS's Z# bridge.

Install this local package into your Python environment:

```sh
python -m pip install /path/to/Z\#/python
```

When the environment already has setuptools 77.0.3 or later, an offline editable
installation is also available:

```sh
python -m pip install --no-index --no-build-isolation --no-deps -e /path/to/Z\#/python
```

For example, `/path/to/project/Math.zsharp` can contain:

```zsharp
zsharp = type.script

noticed room Calculator[] (
 noticed number Add[number Left, number Right] (
  feed(Left + Right):
 )
)
```

The project must also have its normal `project.zsettings`. From Python:

```python
from zsharp import call

answer = call("Math:Calculator:Add", 20, 22, project="/path/to/project")
print(answer)  # 42
```

Reuse a project and runtime configuration with `ZSharp`:

```python
from zsharp import ZSharp

project = ZSharp(
    project="/path/to/project",
    executable="/path/to/zsharp",  # optional
    timeout=30,                    # seconds; None disables the limit
)
answer = project.call("Math:Calculator:Add", 20, 22)
```

`call(target, *arguments, project=None, executable=None, timeout=30)` defaults
to the current directory. Targets have exactly three parts:
`File:Room:Function`. The file may be `Math`, `Math.zsharp`, or a relative path
such as `tools/Math`; it must remain inside the project. The room and function
must be `noticed`. Ordinary brains and typed functions are supported.

Runtime discovery checks an explicit `executable`, `ZSHARP_EXECUTABLE`, the
current PATH, and the default user installation. That includes
`~/.local/bin/zsharp`, and on Windows
`%LOCALAPPDATA%\ZombieOS\ZSharp\bin\zsharp.exe`.

Each call starts a fresh VM, initializes the project, invokes the selected
function, and exits. `Start` and `Shutdown` are not called automatically;
globals and live objects do not persist between calls. No source wrapper is
generated. Native `Print` output and errors go to Python's stdout and stderr;
the return value travels in a separate result file.

Python `str`, `bool`, `None`, `int`, finite `float`, `Decimal`, and string-keyed
records use the existing Z# value bridge. Return values include plain records
and supported Z# arrays; integer numbers become `int`, while numbers with a
decimal point or exponent become `decimal.Decimal` so precision is preserved.
The VM currently accepts homogeneous text/number/object arrays, including
arrays nested in records. Z# does not yet declare direct array parameters;
put array data in an `object` record when passing it into a function. Mixed,
boolean, null, or nested arrays are not supported by the VM. Live room or
foreign object handles cannot be returned from a one-shot call. Calls have a
1 MiB request/result limit, a nesting limit of 32, and at most 1024 arguments.

```python
from zsharp import ZSharpError

try:
    result = project.call("Math:Calculator:Add", 20, 22, timeout=5)
except ZSharpError as error:
    print(error)
    # error.stdout, error.stderr, error.returncode, error.timed_out
```

Invalid Python argument types, non-finite numbers, and malformed targets raise
`TypeError` or `ValueError`. Runtime discovery, native diagnostics, invalid
responses, and timeouts raise `ZSharpError`. A timeout terminates the ZVM
subprocess and returns promptly. A foreign worker already executing Python,
Java, or another language may finish its current call before noticing that
the VM has exited. The package launches an argument list without a shell.

Inside a Python module already running under ZVM, the worker continues to
provide its own `from zsharp import export, call` callback API. That callback
uses the active VM synchronously; installing this standalone package does not
change it.

Run the package tests without installing it:

```sh
python -m unittest discover -s python/tests -v
```

## Publishing from GitHub

The repository workflow is **Publish Python package**, stored in
`.github/workflows/publish-python-package.yml`. It runs manually. With the
`publish` box unchecked, it tests the package on Python 3.9 and 3.14, checks
that Python/ZVM versions match, builds and validates both distribution files,
and saves them as a workflow artifact. Checking `publish` also uploads those
files to PyPI through a separate job using Trusted Publishing.

For the first release, configure a pending publisher on PyPI with these values:

| Field | Value |
| --- | --- |
| PyPI project name | `zombieos-zsharp` |
| GitHub owner | `ZombieOS` |
| Repository | `ZSharp` |
| Workflow name | `publish-python-package.yml` |
| Environment name | `pypi` |

The workflow file must be pushed to GitHub's default branch before it appears
under Actions.
The `pypi` environment in PyPI must match the workflow's environment name.
This workflow uses GitHub's identity token; no PyPI API token secret is needed.

Release the matching native ZVM (1.4.1.1 for this first Python package) before
running **Publish Python package** with `publish` checked. Older ZVM releases
do not implement the required `call` command. Adding the workflow and
configuring the publisher can happen beforehand; neither step publishes the
package. A normal commit or GitHub release does not start this workflow.

See [PyPI's Trusted Publishing guide](https://docs.pypi.org/trusted-publishers/using-a-publisher/)
and [creating a project with a pending publisher](https://docs.pypi.org/trusted-publishers/creating-a-project-through-oidc/).
