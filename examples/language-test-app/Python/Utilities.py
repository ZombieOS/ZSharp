from zsharp import export


@export
def greet(name: str) -> str:
    return f"Hello from Python, {name or 'Z# developer'}!"


@export
def add(left: int, right: int) -> int:
    return left + right


@export
def is_ready() -> bool:
    return True
