# Contributing to Ext4Windows

Thanks for your interest in contributing! This project is actively developed and we welcome contributions of all kinds.

## Getting Started

1. **Fork** the repository
2. **Clone** with submodules: `git clone --recursive https://github.com/YOUR_USER/Ext4Windows.git`
3. **Build** — see [Building from Source](README.md#building-from-source) in the README
4. **Create a branch** for your changes: `git checkout -b feature/my-feature`

## What Can I Work On?

- Check the [Roadmap](README.md#roadmap) for planned features
- Look at [open issues](https://github.com/Mateuscruz19/Ext4Windows/issues) for bugs and feature requests
- Improve test coverage (see `tests/` directory)
- Fix typos or improve documentation

## Development Setup

### Prerequisites

| Tool | Version |
|:-----|:--------|
| Windows | 10 or 11 (64-bit) |
| Visual Studio 2022 | Build Tools or full IDE ("Desktop development with C++" workload) |
| CMake | 3.16+ |
| Git | Any |
| [WinFsp](https://winfsp.dev/rel/) | Latest |

### Build

Open a **Developer Command Prompt for VS 2022**, then:

```bash
cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -B build -S .
cmake --build build --config Release
```

### Run Tests

```bash
cd build/tests
ctest --output-on-failure
```

Note: `test_blockdev` requires a `test.img` ext4 image in the project root. Other tests run without external files.

## Submitting Changes

1. Make sure the project **builds without errors**
2. Run the **test suite** and make sure all tests pass
3. Write clear, concise **commit messages**
4. **Open a Pull Request** with a description of what you changed and why

## Code Style

- C++17 standard
- 4-space indentation
- Braces on the same line for functions and control flow
- Use `snake_case` for functions and variables
- Use `PascalCase` for classes and types
- Keep lines under 120 characters when practical

## Reporting Bugs

Open an [issue](https://github.com/Mateuscruz19/Ext4Windows/issues) with:

1. What you expected to happen
2. What actually happened
3. Steps to reproduce
4. Windows version and any relevant system info
5. Debug log output (run with `--debug` flag)

## License

By contributing, you agree that your contributions will be licensed under the [GPL-2.0 License](LICENSE).
