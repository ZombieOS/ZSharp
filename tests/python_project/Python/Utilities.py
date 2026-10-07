from zsharp import export
import os
import atexit
import pathlib
import threading

marker = os.environ.get("ZSHARP_PYTHON_TEST_MARKER")
if marker:
    pathlib.Path(marker).write_text(str(os.getpid()), encoding="ascii")
    atexit.register(lambda: pathlib.Path(marker + ".closed").write_text("closed"))

counter = 0
original_pid = os.getpid()


@export
def next_count():
    global counter
    counter += 1
    return counter


@export
def same_process():
    return original_pid == os.getpid() and counter == 2


@export
def greet(name: str) -> str:
    return f"Hello, {name}!"


@export
def add(left: int, right: int) -> int:
    return left + right


@export
def is_ready() -> bool:
    if os.environ.get("ZSHARP_PYTHON_TEST_BLOCKING"):
        threading.Thread(target=threading.Event().wait, daemon=False).start()
    # Both ordinary output and raw fd output must not corrupt acknowledgements.
    print("Python worker print")
    os.write(1, b"Python worker raw print\n")
    return True
