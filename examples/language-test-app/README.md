# Z# Language Test

This is the shared interoperability test app for languages embedded in Z#.
The current version tests Python, JavaScript, and Lua from one window.

Enter a name and select a language. Each button calls that language's local
module, exchanges text, numbers, and a status value with Z#, then displays the
result in the window.

Build it with Z# 1.1.2.0 or newer:

```text
zsharp package app "path/to/language-test-app" ZSharpLanguageTest
```
