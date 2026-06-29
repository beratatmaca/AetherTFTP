# AetherTFTP

[![Build Status](https://github.com/beratatmaca/AetherTFTP/actions/workflows/release.yml/badge.svg)](https://github.com/beratatmaca/AetherTFTP/actions)
[![codecov](https://codecov.io/gh/beratatmaca/AetherTFTP/graph/badge.svg)](https://codecov.io/gh/beratatmaca/AetherTFTP)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C++-17-blue.svg?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/compiler_support/17)
[![Qt 6](https://img.shields.io/badge/Qt-6-green.svg?logo=qt)](https://www.qt.io/)

AetherTFTP is a modern, lightweight, open-source cross-platform TFTP (Trivial File Transfer Protocol) client and server application. Written in C++17 and utilizing the Qt 6 framework, it is designed for speed, reliability, and high-block-size network throughput while providing both a headless Command Line Interface (CLI) and an intuitive Graphical User Interface (GUI).

## Technical Architecture

The architecture separates the core protocol logic from the presentation layer, allowing the engine to be compiled as a static library with zero graphical dependencies.

```text
                  ┌─────────────────────────────┐
                  │      Core TFTP Engine        │
                  │  (RFC 1350 / 2347 / 2348 / 2349) │
                  └──────────────┬──────────────┘
                                 │
               ┌─────────────────┴─────────────────┐
               │                                   │
               ▼                                   ▼
 ┌─────────────────────────┐       ┌─────────────────────────┐
 │      CLI Interface       │       │     Qt6 GUI Interface    │
 │  (QCoreApplication)      │       │  (QApplication)          │
 │  QCommandLineParser      │       │  QThread + QTreeView     │
 └─────────────────────────┘       └─────────────────────────┘
```

- **`tftp_protocol`**: A pure data serialization and deserialization layer, operating independently of sockets and threads.
- **`TftpSession`**: Manages the lifecycle of an individual transfer thread-safely, controlling block counters, window flow, timeout retransmissions, and file stream states.
- **`TftpServer`**: A listener socket that accepts incoming requests and spawns isolated, transaction-specific sessions.

---

## Getting Started

### Prerequisites

To build AetherTFTP, you will need:

- **CMake** (v3.16 or higher)
- **Qt6 SDK** (specifically the `Core`, `Network`, and `Test` modules)
- **C++17 compliant compiler** (GCC 10+, Clang 12+, or MSVC 2019+)

### Build Instructions

To compile the project:

```bash
# Clone the repository
git clone https://github.com/your-org/AetherTFTP.git
cd AetherTFTP

# Configure and compile (Release mode)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

To run the unit test suite:

```bash
cmake -B build -DBUILD_TESTING=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

---

## Usage Reference

By default, calling `aethertftp` without arguments launches the Graphical User Interface (GUI). When arguments (such as `--server`, `--get`, or `--put`) are supplied, the application automatically launches in headless Command Line Interface (CLI) mode.

To explicitly force GUI mode when launching with arguments, use the `--gui` flag.

### Headless Server Mode

Spawn a standalone TFTP server listening on port 6969, serving files out of a specified directory:

```bash
./build/aethertftp --server --port 6969 --dir /var/tftp
```

### Client File Download (Get)

Download a file from a remote server using a custom blocksize of 8192 bytes for faster transfer rates:

```bash
./build/aethertftp --get --host 192.168.1.10 --file firmware.bin --output ./firmware.bin --blocksize 8192
```

### Client File Upload (Put)

Upload a local file to a remote TFTP server:

```bash
./build/aethertftp --put --host 192.168.1.10 --file firmware.bin --timeout 3
```

### Graphical Interface Mode

To open the graphical interface:

```bash
./build/aethertftp --gui
```

---

## License

This project is open-source and available under the MIT License.
