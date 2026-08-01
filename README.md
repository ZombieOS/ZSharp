# Z#

The official programming language for ZombieOS.

Z# is a modern programming language designed to compile into multiple programming languages through a package-based architecture. Instead of targeting a single language, Z# allows developers to write code once and compile it into one or more supported languages.

---

# Current Status

Current Version

```
Pre-Alpha
```

This project is currently under active development.

---

# Features

- Multi-language compilation
- Package-based compiler
- Modular architecture
- Cross-platform
- Workspace support
- VS Code Extension
- Visual Studio Extension
- Build system
- Package Manager
- Language Server (Planned)

---

# Repository Structure

```
ZSharp/
│
├── CLI/
│   Command Line Interface
│
├── Compiler/
│   Z# Compiler
│
├── Language/
│   Language Specification
│
├── VSCode/
│   Visual Studio Code Extension
│
├── VisualStudio/
│   Visual Studio Extension
│
├── Examples/
│   Example Projects
│
└── README.md
```

---

# Installing

Clone the repository.

```
git clone https://github.com/ZombieOS/ZSharp.git
```

Install the required dependencies.

Currently Required

- Python 3.13+
- .NET 10 SDK

---

# Building

Once the CLI has been installed:

```
zsharp build
```

The CLI will:

- Parse the workspace
- Load installed packages
- Build the project
- Generate output

---

# CLI Commands

Current

```
zsharp build
```

Planned

```
zsharp install <package>

zsharp uninstall <package>

zsharp update

zsharp doctor

zsharp version

zsharp list

zsharp search <package>

zsharp info <package>

zsharp help
```

---

# Package System

Language compilation is performed through packages.

Packages are installed into the workspace.

```
Project/

├── Main.zsharp

└── Z# Packages/
```

Packages are downloaded from

```
packages.zsharp.zombieos.com
```

Official packages include

- Web
- JavaScript
- TypeScript
- C#
- Java
- Kotlin
- Python
- Rust
- C
- CPP
- Lua
- Go

Community packages can also be installed.

---

# Workspace

Example

```
Website/

├── Main.zsharp

├── Pages/
│   Home.zsharp
│   Downloads.zsharp
│   Dashboard.zsharp

└── Z# Packages/
    Web/
    JavaScript/
```

---

# Philosophy

Write once.

Compile anywhere.

Rather than writing multiple versions of the same application, Z# allows projects to target multiple languages using the same source code.

---

# Roadmap

Phase 1

- CLI
- Compiler
- Parser
- Package System

Phase 2

- JavaScript Package
- Web Package
- Go Package

Phase 3

- Additional Official Packages

Phase 4

- Stable Release

---

# Official Packages

- Web
- JavaScript
- TypeScript
- C#
- Java
- Kotlin
- Python
- Rust
- C
- CPP
- Lua
- Go

---

# Contributing

Contributions are welcome.

Areas include

- Compiler
- CLI
- Documentation
- VS Code Extension
- Visual Studio Extension
- Official Packages
- Testing

See

```
CONTRIBUTING.md
```

for more information.

---

# License

This project is licensed under the **Apache License 2.0**.

You are free to use, modify, and distribute this software in accordance with the terms of the Apache License 2.0.

See the `LICENSE` file in this repository for the full license text.

Copyright © ZombieOS.