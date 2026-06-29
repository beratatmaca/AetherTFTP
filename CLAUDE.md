# AetherTFTP — Development & Style Guide

> **Purpose:** This file is the authoritative guide for developers, contributors, and IDE tooling configurations working on the **AetherTFTP open-source project**. Read it fully before making any changes.

---

## 1. Project Identity

AetherTFTP is an open-source, lightweight, cross-platform TFTP client and server application.

### Name: **AetherTFTP**

- **Etymology:** "Aether" (the mythical upper sky; the invisible, frictionless medium) + TFTP — symbolising the lightweight, invisible nature of UDP-based file transfer.
- **Version:** `0.1.0` (Phase 1 — Engine & CLI)
- **Language:** C++17
- **Framework:** Qt 6 (Core + Network; Widgets added in Phase 2)
- **Build system:** CMake ≥ 3.16

### Icon Design Concept

A modern, vector-based minimalist icon:

```text
+---------------------+
|   .-------------.   |
|  /   ▲     ▲     \  |   Outer boundary : Dark Slate hexagon
| |    |     |      | |
| |    |  ▲  |      | |   Dual vertical arrows : bi-directional
| |    ▼  |  ▼      | |   async block transfers (RRQ ↕ WRQ)
|  \      ▼        /  |
|   '-------------'   |   Centre node : lightweight UDP datagram
+---------------------+
```

| Token      | Value                     | Meaning            |
| ---------- | ------------------------- | ------------------ |
| Background | `#1E293B` (Deep Slate)    | Depth & precision  |
| Accent A   | `#06B6D4` (Electric Cyan) | Network activity   |
| Accent B   | `#10B981` (Mint Green)    | Transfer integrity |

- **Geometry:** Rounded hexagon; compatible with GNOME, macOS Squircle, and Windows Fluent design grids.

---

## 2. Repository Layout

```text
AetherTFTP/
├── CMakeLists.txt            # Root build configuration
├── CLAUDE.md                 # ← you are here
├── .gitignore
├── src/
│   ├── main.cpp              # Entry point; dispatches CLI ↔ GUI
│   ├── cli/
│   │   ├── cli_runner.h      # QCommandLineParser-based CLI front-end
│   │   └── cli_runner.cpp
│   └── core/                 # Pure protocol engine (no UI dependencies)
│       ├── tftp_protocol.h   # Packet types, opcodes, (de)serialisation API
│       ├── tftp_protocol.cpp
│       ├── tftp_session.h    # One active server-side transfer (TftpSession)
│       ├── tftp_session.cpp
│       ├── tftp_server.h     # Listener socket; creates TftpSession per request
│       ├── tftp_server.cpp
│       ├── tftp_client.h     # Client-side transfer logic
│       └── tftp_client.cpp
└── tests/
    └── tftp_test.cpp         # Qt Test suite (loopback, RFC compliance)
```

> **Rule:** `src/core/` must **never** import Qt Widgets, Qt Quick, or any GUI module. It must compile cleanly with `Qt6::Core` and `Qt6::Network` only.

---

## 3. Technical Architecture

### 3.1 Dual-Mode Dispatch

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

`src/main.cpp` inspects `argc`/`argv` at startup:

- **Default (No Arguments)**: Instantiates `QApplication` and launches the desktop GUI.
- **Argumented Call (e.g. `--server`, `--get`, `--put`)**: Instantiates `QCoreApplication` and runs headlessly in CLI Mode.
- **Explicit GUI Override**: If `--gui` is explicitly supplied along with other arguments, it overrides the default behavior to launch the GUI window.

### 3.2 Core Layer: `tftp_protocol`

**Files:** [`tftp_protocol.h`](src/core/tftp_protocol.h), [`tftp_protocol.cpp`](src/core/tftp_protocol.cpp)

Pure data layer — **no sockets, no Qt event loop**. Responsible for:

| Function                             | Direction | Description                       |
| ------------------------------------ | --------- | --------------------------------- |
| `buildRequest(op, file, mode, opts)` | → wire    | Serialise RRQ / WRQ datagram      |
| `buildData(block, payload)`          | → wire    | Serialise DATA packet             |
| `buildAck(block)`                    | → wire    | Serialise ACK packet              |
| `buildError(code, msg)`              | → wire    | Serialise ERROR packet            |
| `buildOack(opts)`                    | → wire    | Serialise OACK (RFC 2347)         |
| `peekOpCode(dg, &op)`                | ← wire    | Read opcode without full parse    |
| `parseRequest(dg, &out)`             | ← wire    | Parse RRQ / WRQ + options         |
| `parseData(dg, &blk, &payload)`      | ← wire    | Parse DATA packet                 |
| `parseAck(dg, &blk)`                 | ← wire    | Parse ACK packet                  |
| `parseError(dg, &code, &msg)`        | ← wire    | Parse ERROR packet                |
| `parseOack(dg, &opts)`               | ← wire    | Parse OACK option set             |
| `clampBlockSize(n)`                  | —         | Enforce RFC 2348 range [8, 65464] |

**Key types:**

```cpp
namespace tftp {
    enum class OpCode   : quint16 { RRQ=1, WRQ, DATA, ACK, ERROR, OACK };
    enum class ErrorCode: quint16 { NotDefined=0, FileNotFound, AccessViolation,
                                    DiskFull, IllegalOperation, UnknownTransferId,
                                    FileAlreadyExists, NoSuchUser, OptionRefused };
    using Options = QMap<QString, QString>;  // lowercase keys, insertion-ordered

    struct Request {
        OpCode  op;          // RRQ or WRQ
        QString filename;
        QString mode;        // always lowercased (e.g. "octet")
        Options options;     // blksize / timeout / tsize (may be empty)
    };
}
```

**Constants:**

| Constant            | Value     | Source      |
| ------------------- | --------- | ----------- |
| `kDefaultBlockSize` | 512       | RFC 1350    |
| `kMinBlockSize`     | 8         | RFC 2348    |
| `kMaxBlockSize`     | 65464     | RFC 2348    |
| `kDefaultPort`      | 69        | IANA        |
| `kModeOctet`        | `"octet"` | RFC 1350 §1 |

### 3.3 Core Layer: `TftpSession`

**Files:** [`tftp_session.h`](src/core/tftp_session.h), [`tftp_session.cpp`](src/core/tftp_session.cpp)

One `TftpSession` object represents **one active transfer** (RRQ or WRQ). It owns:

- An **ephemeral `QUdpSocket`** (its Transfer ID, RFC 1350 §4).
- A **`QFile`** for reading (RRQ) or writing (WRQ).
- A **`QTimer`** for retransmission / timeout.

**Lifecycle:**

```text
TftpServer creates TftpSession
        │
        ▼
    session.start()
        │
        ├─ Bind ephemeral socket (random port = TID)
        ├─ negotiateOptions() → build OACK if options present
        │
        ├─ RRQ path:  send OACK (or DATA block 1 directly) → await ACK loop
        └─ WRQ path:  send OACK (or ACK block 0 directly) → await DATA loop
                                           │
                             handleAck() / handleData()
                                           │
                                     finish(ok, msg)
                                           │
                                    emit finished(bool, QString)
```

**Signals emitted by `TftpSession`:**

| Signal     | Args                           | When                         |
| ---------- | ------------------------------ | ---------------------------- |
| `progress` | `(qint64 bytes, qint64 total)` | After each block processed   |
| `finished` | `(bool ok, QString message)`   | Transfer complete or aborted |

**Retransmission policy:**

- Default timeout: **5 000 ms** per attempt.
- Maximum retries: **5** (configurable via RFC 2349 `timeout` option).
- On timeout: resend last packet (OACK / DATA / ACK); increment retry counter.
- Exceeding `m_maxRetries` → `finish(false, "Transfer timed out")`.

**Stray TID handling (RFC 1350 §4):**
Any datagram arriving from an unexpected `(address, port)` pair receives an `ERROR UnknownTransferId` reply but does **not** disturb the active transfer.

### 3.4 Option Negotiation (RFC 2347 / 2348 / 2349)

| Option key | Behaviour                                                                                     |
| ---------- | --------------------------------------------------------------------------------------------- |
| `blksize`  | Clamped to [8, 65464]; stored in `m_blockSize`                                                |
| `timeout`  | Valid range [1, 255] seconds; stored in `m_timeoutMs`                                         |
| `tsize`    | RRQ: server reports actual file size. WRQ: client declares size; echoed for progress tracking |

If **no** options are requested, no OACK is sent and the classic RFC 1350 flow begins immediately.

---

## 4. Build Instructions

### Prerequisites

| Tool         | Minimum version                                |
| ------------ | ---------------------------------------------- |
| CMake        | 3.16                                           |
| Qt           | 6.4 (Core, Network; Test for tests)            |
| C++ compiler | GCC 10 / Clang 12 / MSVC 2019 (C++17 required) |

### Debug build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

### Release build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Run tests

```bash
cmake -B build -DBUILD_TESTING=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

> Tests are **on by default** (`BUILD_TESTING=ON`). Pass `-DBUILD_TESTING=OFF` to skip them in CI if needed.

---

## 5. CLI Command Reference

The binary `aethertftp` acts as a drop-in replacement for standard TFTP command-line clients when arguments are supplied.

```bash
# Start headless server
aethertftp --server --port 6969 --dir /var/tftp

# Download a file (RRQ)
aethertftp --get --host 192.168.1.10 --file firmware.bin --output ./firmware.bin

# Upload a file (WRQ)
aethertftp --put --host 192.168.1.10 --file firmware.bin --blocksize 8192

# Launch GUI (Phase 2)
aethertftp --gui
```

| Flag          | Type   | Default | Description                                      |
| ------------- | ------ | ------- | ------------------------------------------------ |
| `--server`    | bool   | false   | Run as a headless TFTP server                    |
| `--port`      | int    | 69      | UDP port to bind (server) or connect to (client) |
| `--dir`       | path   | `.`     | Root directory served (server mode)              |
| `--host`      | string | —       | Remote server address (client mode)              |
| `--get`       | bool   | false   | Perform an RRQ (download)                        |
| `--put`       | bool   | false   | Perform a WRQ (upload)                           |
| `--file`      | string | —       | Filename to transfer                             |
| `--output`    | path   | `.`     | Local save path (get mode)                       |
| `--blocksize` | int    | 512     | Request RFC 2348 blksize negotiation             |
| `--timeout`   | int    | 5       | Per-block timeout in seconds (RFC 2349)          |
| `--gui`       | bool   | false   | Open graphical interface (Phase 2)               |

---

## 6. RFC Compliance Matrix

| RFC      | Title                            | Status                                                              |
| -------- | -------------------------------- | ------------------------------------------------------------------- |
| RFC 1350 | TFTP Protocol (Rev. 2)           | ✅ Implemented — octet mode; full DATA/ACK loop; stray TID rejection |
| RFC 2347 | TFTP Option Extension            | ✅ Implemented — OACK generation and parsing                         |
| RFC 2348 | TFTP Blocksize Option            | ✅ Implemented — clamp to [8, 65464]                                 |
| RFC 2349 | Timeout Interval & Transfer Size | ✅ Implemented — `timeout` and `tsize` options                       |
| netascii | Transfer mode                    | 🚫 Out of scope — `IllegalOperation` error returned                  |

---

## 7. Testing

### Framework

Qt Test (`Qt6::Test`) with a loopback network infrastructure. No physical hardware required.

### Test binary

```bash
./build/tftp_test           # or: ctest --test-dir build
```

### Reference test pattern

```cpp
#include <QtTest>
#include "core/tftp_server.h"
#include "core/tftp_client.h"

class TFTPProtocolTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void testBlockSizeNegotiation();   // RFC 2348
    void testIntegrityOnPacketLoss();  // retransmit / timeout
    void testWrqReceive();             // WRQ end-to-end
    void cleanupTestCase();
private:
    TftpServer   *m_server = nullptr;
    QTemporaryDir m_testDir;
};

void TFTPProtocolTest::initTestCase() {
    m_server = new TftpServer(this);
    QVERIFY(m_server->listen(QHostAddress::LocalHost, 10069, m_testDir.path()));
}

void TFTPProtocolTest::testBlockSizeNegotiation() {
    TftpClient client;
    client.setBlockSize(8192);  // RFC 2348 — request 8 KiB blocks

    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile("127.0.0.1", 10069, "test_image.bin",
                        m_testDir.path() + "/out.bin");

    QVERIFY(spy.wait(5000));
    QCOMPARE(client.negotiatedBlockSize(), 8192);
}

void TFTPProtocolTest::cleanupTestCase() {
    m_server->close();
    delete m_server;
}

QTEST_MAIN(TFTPProtocolTest)
#include "tftp_test.moc"
```

### What to test

- Block-size negotiation (powers of two; min/max clamping).
- Transfer integrity on simulated packet loss (retransmit path).
- Timeout exhaustion → `finished(false, …)`.
- Stray TID rejection (unrelated sender mid-transfer).
- `tsize` progress reporting accuracy.
- WRQ file overwrite rejection (`FileAlreadyExists`).
- Non-octet mode rejection (`IllegalOperation`).

---

## 8. Code Conventions

### General

- **C++17** everywhere. Use structured bindings, `if constexpr`, `std::optional`, range-for freely.
- **`nullptr`** over `NULL` or `0`.
- **`Q_UNUSED(x)`** to suppress unused-parameter warnings explicitly.
- **`const`-correctness**: mark every parameter and method `const` where possible.
- **Doxygen Documentation**: All public APIs, classes, methods, and important member variables must be fully documented using Doxygen comments (`/** ... */` or `///` syntax with `@brief`, `@param`, and `@return` tags).
- **Paths & Privacy**: Never hardcode absolute system paths (e.g. `/home/username/...` or specific local directories) in source code, build scripts, test suites, or package configurations. Use relative paths, C++ filesystem APIs, or CMake variables. Do not reference private developer names, emails, or personal usernames within code, configurations, or documentation.
- Include guards: use `#pragma once` (already adopted throughout).
- Header order: own headers first → Qt headers → stdlib headers (each group alphabetical).

### Qt specifics

- Use `Qt6::Core`'s `qDebug()` / `qWarning()` / `qCritical()` for diagnostics.
- Prefer **signal/slot** (new pointer-to-member syntax) over `SIGNAL()`/`SLOT()` macros.
- `QObject` ownership: parent-child tree is the default; use `std::unique_ptr` only when explicit RAII is clearer (see `TftpSession::m_socket`, `m_file`).
- Do **not** use `QThread::sleep` or blocking waits in the event loop thread.

### Naming

| Construct               | Convention                | Example                        |
| ----------------------- | ------------------------- | ------------------------------ |
| Classes                 | `PascalCase`              | `TftpSession`                  |
| Member variables        | `m_camelCase`             | `m_blockSize`                  |
| Local variables         | `camelCase`               | `peerPort`                     |
| Constants / `constexpr` | `kCamelCase`              | `kDefaultBlockSize`            |
| Free functions          | `camelCase`               | `buildRequest()`               |
| Signals                 | `camelCase`, verb-present | `progress()`, `finished()`     |
| Slots (private)         | `on<Source><Event>`       | `onReadyRead()`, `onTimeout()` |

### Namespaces

All protocol code lives in `namespace tftp`. Do not `using namespace tftp;` in headers.

---

## 9. Cross-Platform Packaging

Packages are compiled, signed, and shipped automatically via the GitHub Actions release workflow (`.github/workflows/release.yml`) directly to the GitHub releases page upon tagging a new version (`v*`).

```text
[ GitHub Release Tag ]
         │
┌────────┼────────┐
│        │        │
▼        ▼        ▼
Linux   macOS   Windows
│        │        │
├─ .deb  │ .dmg   └─ .msi
└─ snap
```

| Platform                      | Package | Toolchain                                                            |
| ----------------------------- | ------- | -------------------------------------------------------------------- |
| Linux (Ubuntu/Debian)         | `.deb`  | `cpack -G DEB`; ships a `systemd` unit file for server mode          |
| Linux (Universal)             | `snap`  | `snapcraft.yaml` on `core24`; `network-bind` plug required           |
| macOS (Intel + Apple Silicon) | `.dmg`  | `macdeployqt` + `hdiutil`; signed with Apple Developer ID; notarised |
| Windows 10/11                 | `.msi`  | `windeployqt` + WiX Toolset V4 via CMake integration                 |

---

## 10. Implementation Roadmap

### Phase 1 — Engine & CLI Base *(current)*

- [x] `tftp_protocol`: full packet (de)serialisation layer
- [x] `TftpSession`: server-side RRQ/WRQ with option negotiation, retransmit, stray TID protection
- [x] `TftpServer`: listening socket → creates `TftpSession` per accepted request
- [x] `TftpClient`: client-side RRQ/WRQ with option negotiation and progress signals
- [x] `CliRunner`: `QCommandLineParser`-based front-end wiring server + client
- [x] `src/main.cpp`: dispatch logic (CLI vs. GUI)
- [x] Qt Test suite integrated into CMake (`tests/tftp_test.cpp`)

### Phase 2 — Modern Qt6 GUI

- [x] Responsive UI layout: `QTreeView` + file-transfer model (Model/View decoupled from core)
- [x] `QSS` styling with native OS Dark / Light mode support
- [x] Drag-and-drop upload via GUI
- [x] Per-transfer progress bars and transfer log panel
- [x] Server configuration dialog (port, root directory, max sessions)

### Phase 3 — Deployment Engineering

- [x] GitHub Actions cross-compilation matrix (Linux / macOS / Windows)
- [x] Windows code-signing certificate integration (MSI packaging configured via WiX/CPack)
- [x] macOS notarisation scripts (DMG packaging configured via macdeployqt)
- [x] `snapcraft.yaml` configuration (Snap packages targeting core24)
- [x] `cpack` DEB package with `systemd` service unit (Wired in CMakeLists.txt and dist/)

### Phase 4 — Advanced Security & Networking

- [x] **Access Control Lists (ACLs)**: Define IP-based access permissions, separate read-only/read-write folders, and block specific subnets.
- [x] **Path Traversal Protection**: Implement strict sandboxing to block request paths containing `../` or absolute symlinks pointing outside the server root directory.
- [x] **IPv6 Dual-Stack Support**: Upgrade listener socket creation to support both IPv4 and IPv6 transparently.
- [x] **Single-Port Multiplexing**: Implement firewall-friendly transfer handling by routing active DATA/ACK traffic through a single UDP port rather than allocating random dynamic ports (TIDs).

### Phase 5 — QoS & System Monitoring

- [x] **Bandwidth Rate Limiting**: Allow configuring maximum global or per-session speed limits to prevent network starvation. (Implemented via token-bucket system)
- [x] **Structured Logging**: Output logs in JSON format to support integration with systemlog utilities, syslog (Linux), and Event Viewer (Windows). (JSON logging format implemented in server)
- [x] **Prometheus Exporter**: Add a lightweight HTTP endpoint to expose real-time metrics (throughput, active sessions, packet loss rates, errors). (Exposed via metrics_exporter server)
- [ ] **Real-time GUI Metrics Dashboard**: Add live throughput graphs, transaction histories, and active session performance gauges to the GUI.

---

## 11. Core Implementation Rules

When modifying this codebase, adhere to the following:

1. **Never add GUI dependencies to `src/core/`.** The core engine must remain UI-agnostic and link only against `Qt6::Core` and `Qt6::Network`.

2. **Preserve RFC compliance.** Any change to packet construction or parsing must cite the relevant RFC section in a comment. Do not introduce behaviour that deviates from RFC 1350, 2347, 2348, or 2349 without an explicit discussion.

3. **Maintain `finished()` exactly-once semantics.** `TftpSession::finish()` is guarded by `m_emitted`. Never call `emit finished(…)` directly — always go through `finish()`.

4. **Retransmit safety.** Always cancel the timer (`m_timer->stop()`) before destroying or resetting session state. This is done in `finish()`.

5. **Error propagation.** On any network or file error, call `sendError()` (which internally calls `finish(false, …)`). Do not silently swallow errors.

6. **Option keys are case-insensitive on the wire** (RFC 2347 §2). Always `.toLower()` keys when inserting into `Options` maps.

7. **Tests first.** New protocol behaviour must be accompanied by a corresponding test case in `tests/tftp_test.cpp`.

8. **CMake targets.** `aether_core` is a static library. The executable `aethertftp` links against it. Do not add source files to the executable target directly — add them to `aether_core` or a new library target.

9. **No blocking calls in the event loop.** All I/O goes through Qt's async `readyRead` signal. File I/O in `TftpSession` is synchronous but kept to small, bounded reads/writes per datagram event.

10. **Commit hygiene.** One logical change per commit; prefix messages with the component (`core:`, `cli:`, `gui:`, `tests:`, `ci:`, `docs:`).
