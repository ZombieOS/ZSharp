from decimal import Decimal
import atexit
import os
import pathlib
import threading
from zsharp import export, call


count = 0


@export
def echo(value):
    return value


@export
def next_value():
    global count
    count += 1
    return count


@export
def exact():
    return [10 ** 100, Decimal("0.0000000000000000000000000000000000000000000000001")]


@export
def callback(value):
    return call("Main:Main:Callback", value)


@export
def host_object(value):
    before = value.get("Count")
    value.set("Count", before + 1)
    result = value.invoke("Describe", "Python")
    return {"before": before, "result": result, "proxy": value}


@export
def background_callback():
    messages = []
    def work():
        try:
            call("Main:Main:Callback")
        except RuntimeError as error:
            messages.append(str(error))
    thread = threading.Thread(target=work)
    thread.start()
    thread.join()
    return len(messages) == 1 and "current foreign call" in messages[0]


@export
def logs():
    print("Python user print is a log, not protocol")
    os.write(1, b"Python native stdout is a log, not protocol\n")
    return "logged"


@export
def fail():
    raise RuntimeError("expected fixture failure")


@export
def too_large():
    return "x" * (1024 * 1024)


def hidden():
    return "not exported"


@export
class Person:
    def __init__(self, name):
        self.name = name
        self._secret = "private"

    def greet(self, suffix):
        return self.name + suffix

    def identity(self):
        return self

    @staticmethod
    def static_greeting():
        return "static export"


marker = os.environ.get("ZSHARP_PYTHON_EXIT_MARKER")
if marker:
    atexit.register(lambda: pathlib.Path(marker).write_text("clean exit", encoding="utf-8"))
