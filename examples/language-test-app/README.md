# Z# language interoperability test

This is the permanent test app for languages embedded in Z#. Python is its
first test section; later language updates should add their tests to this same
app. Enter a name and select **Run Python**. The callback passes the name to
Python, calls three exported functions, and writes their text, number, and
status results into the window.

Package it from the repository root with:

```text
zsharp package app examples/language-test-app ZSharpLanguageTest
```

The generated package includes `Python/Utilities.py`. An official ZVM install
also includes the Python runtime, so the computer opening the package does not
need Python installed separately.
