from zsharp import export


@export
def greet(name: str) -> str:
    return f"Hello, {name}!"


@export
def add(left: int, right: int) -> int:
    return left + right


@export
def is_ready() -> bool:
    return True
