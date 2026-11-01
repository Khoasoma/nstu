# Project NSTU Development Guide

[Product overview](../README.md) | [Tiếng Việt](DEVELOPMENT.vi.md)

NSTU is an open-source classroom and computer lab management project for Windows.
Its objective is to provide a centralized teacher administration server,
lightweight student clients running in the background, and low-latency screen
snapshots and broadcast from a teacher workstation to multiple student machines.

Repository: <https://github.com/Khoasoma/nstu>

NSTU is currently an engineering MVP used to develop and test architectural
boundaries, not a finished production release for schools. Practical limitations
are documented in [ROADMAP.md](ROADMAP.md).

## INDEV function status

`INDEV` indicates that the function is merged into `main` for ongoing development
and testing. Its verified implementation remains under development; this label
does not certify the full application for production deployment.

| Function | Status | Main Integration | Verification |
|---|---|---|---|
| [Authenticated control-channel deadline send](../common/include/nstu/control_channel.hpp) (`send_with_deadline`) | **INDEV** | `07af8b6` | Native/public deadline and ordinary-send controls passed; fast/strict gates and MSVC Release CI passed |
| [Native WebView2 Host Kiosk](../client/src/exam_host.cpp) (`ExamHost`) | **INDEV** | `79d1fdf` | Pinned SDK acquisition, fullscreen kiosk window, shortcut suppression hook, foreground re-grab, local virtual host `https://nstu.exam/` |
| [Discrete Timing Telemetry Pipeline](../common/include/nstu/exam_telemetry_collector.hpp) | **INDEV** | `79d1fdf` | Zero-raw-input timing collector, DPAPI retry outbox, 8×8 MiB rotating server journal with ≤262k dedup cache, async worker, 56/56 C++ and 63 Node recovery tests passed |

## Current features

- **Versioned binary protocol**: little-endian, bounded message sizes, and incremental TCP parser.
- **TCP control channel**: connection preamble, mutual HMAC handshake, replay protection, monotonic sequence guard, and rate limiter for unauthenticated connections.
- **`KeyStore`**: enrollment, monotonic rotation, revocation, `key_id` tombstones, and zeroization; machine-scoped DPAPI protection for local secrets.
- **UDP multicast**: room broadcast foundation with fallback to unicast.
- **Packet-loss tracker**: reorder window, duplicate/late/wrong-stream/jump handling. See [PACKET_LOSS.md](PACKET_LOSS.md).
- **Bounded video frame reassembly**: deadlines, duplicate/conflict detection, and memory-pressure accounting. See [VIDEO_REASSEMBLY.md](VIDEO_REASSEMBLY.md).
- **DXGI Desktop Duplication, D3D11 BGRA-to-NV12 converter, and Media Foundation H.264 MFT encoder**.
- **Windows Service and active-session agent**: secure local named pipe, tray icon, lock window, annotation overlay, and teacher-broadcast window.
- **Server UI**: Dear ImGui + Direct3D 11 with thread-safe client registry.
- **Room snapshot wall**: responsive grid with 5-10 second interval, health summary, search/filter, selected-client telemetry, annotation, broadcast, chat, and control actions.
- **Bilingual and theme support**: English and Vietnamese runtime switching, light and dark themes, and a resilient native tray icon with hide/restore/exit without stopping the control plane.
- **Client capture**: GDI + WIC JPEG, bounded to 60 KiB/frame; service retains only the newest pending snapshot to avoid queue backlogs.
- **Client chat**: Native Win32 chat window using lightweight system controls.
- **Modular CMake**: builds cleanly with MSVC or MinGW-w64 with zero compiler warnings under `-Werror`.

## Directory layout

```text
nstu/
|-- common/                  Protocol, auth, TCP/UDP, IOCP, key, rate limit, telemetry
|   |-- include/nstu/        Public C++ headers
|   `-- src/                 Shared implementations
|-- video/                   DXGI, D3D11 converter, Media Foundation encoder
|-- client/
|   |-- service/             Windows Service entry point
|   |-- agent/               Interactive-session tray/overlay/kiosk entry point
|   `-- src/                 Named pipe, exam host, and session integration
|-- server/
|   |-- app/                 Server UI entry point (Dear ImGui)
|   |-- include/nstu/        Client state registry and server telemetry headers
|   `-- src/                 Client registry, control plane, telemetry journal/worker
|-- exam/
|   |-- web/                 Web-based exam client UI (app.js, index.html, styles.css)
|   |-- studio/              Browser-native visual authoring tool (NSTU Exam Studio)
|   |-- schema/              manifest.schema.json contract
|   `-- examples/            Worked sample manifests
|-- tests/                   Protocol, auth, packet-loss, reassembly, IPC, telemetry tests
|-- docs/                    Architecture, security, packet-loss, roadmap, assessment
|-- packaging/               NSIS installer scripts and package staging helpers
|-- cmake/                   Compiler warning policy and WebView2 SDK acquisition
|-- CMakeLists.txt           Root build configuration
|-- CMakePresets.json        Windows Debug/Release presets
|-- LICENSE                  MIT license
`-- THIRD_PARTY_NOTICES.md   Dependency and license notices
```

## System requirements

- Windows 10 or Windows 11 x64.
- Visual Studio 2022 (with "Desktop development with C++" and Windows SDK), or MinGW-w64 (GCC 13+ with C++20 support).
- CMake 3.25 or later, Git.
- Ninja (recommended when using CMake presets).

Video APIs and Windows services are Windows-only. Full builds and integration tests require Windows.

## Clone and build

```powershell
git clone https://github.com/Khoasoma/nstu.git
cd nstu
```

### MSVC / Visual Studio

```powershell
cmake -S . -B build-msvc -A x64 -DNSTU_ENABLE_WERROR=ON
cmake --build build-msvc --config Release --parallel
ctest --test-dir build-msvc -C Release --output-on-failure
```

### MinGW-w64

```powershell
cmake -S . -B build-mingw -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release -DNSTU_ENABLE_WERROR=ON
cmake --build build-mingw --parallel 4
ctest --test-dir build-mingw --output-on-failure
```

### CMake presets

Presets require Ninja and a compatible C++20 toolchain:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

For release builds, use `--preset windows-release`.

## CMake configuration options

| Option | Default | Purpose |
|---|---|---|
| `NSTU_BUILD_CLIENT=ON\|OFF` | `ON` | Build client service (`nstu-service`), agent (`nstu-agent`), and provisioning |
| `NSTU_BUILD_SERVER=ON\|OFF` | `ON` | Build teacher server and administration UI |
| `NSTU_BUILD_SETUP=ON\|OFF` | `ON` on Windows | Build the `nstu-diagnostics` helper used by installer and boot checks |
| `NSTU_BUILD_VIDEO=ON\|OFF` | `ON` | Build DXGI Desktop Duplication and Media Foundation components |
| `NSTU_BUILD_TESTS=ON\|OFF` | `ON` | Build unit and integration test executables |
| `NSTU_SERVER_USE_IMGUI=ON\|OFF` | `ON` | Enable Dear ImGui + D3D11 server UI executable |
| `NSTU_ENABLE_WERROR=ON\|OFF` | `OFF` | Treat compiler warnings as errors (required for CI and release gates) |
| `NSTU_ENABLE_PACKAGING=ON\|OFF` | `ON` | Enable unified NSIS packaging target (`nstu-package`) |
| `NSTU_ENABLE_WEBVIEW2_HOST=ON\|OFF`| `ON` on Windows | Build native WebView2 exam kiosk host (`ExamHost`) |
| `NSTU_WEBVIEW2_SDK_DIR=<path>` | Auto-download | Path to pinned Microsoft.Web.WebView2 SDK directory |
| `NSTU_INTERNAL_TEST_BUILD=ON\|OFF` | `OFF` | Build `*-internal-vm-setup.exe` permitting low-RAM development VMs |

Example: building core protocol and test suite only:

```powershell
cmake -S . -B build-core -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release -DNSTU_BUILD_VIDEO=OFF `
  -DNSTU_BUILD_CLIENT=OFF -DNSTU_BUILD_SERVER=OFF
cmake --build build-core --parallel 4
ctest --test-dir build-core --output-on-failure
```

## Build artifacts

- `nstu-server.exe`: Teacher server administration UI.
- `nstu-service.exe`: Privileged Windows Service (`LocalSystem`, Session 0).
- `nstu-agent.exe`: Interactive user session agent (tray, chat, overlay, and WebView2 kiosk).
- `nstu_video_probe.exe`: Hardware capability probe for DXGI adapters and Media Foundation MFTs.
- `tests/`: 56 unit and integration test executables covering protocol, crypto, IPC, and telemetry.

## Running the UI

After building on Windows, execute `nstu-server.exe` to launch the server shell.
The dashboard opens to `Room screens`: health summary at the top, adjustable 5-10 second snapshot intervals, search/filtering, and responsive snapshot wall.
`Selected client` provides a focused view with telemetry, 16:9 snapshot preview, snapshot controls, lock/unlock, click-through overlay drawing, and chat.

- `EN`/`VI` and `Light`/`Dark` buttons toggle language and theme dynamically.
- Command-line flags `--language=vi`, `--language=en`, and `--dark` select startup defaults.
- Minimizing or closing the window minimizes to the Windows notification area; double-click the tray icon to restore, or use the context menu to hide/show and exit.

Launch `nstu-agent.exe` to open the Native Win32 client tray icon and chat window.
Double-click the tray icon to toggle the chat window. The agent manages separate windows for screen lock, annotation click-through, and teacher-screen broadcast.

## Local verification gates

Before pushing commits, run the local verification gates in Bash (e.g. Git Bash):

```bash
# Fast gate: hygiene, syntax check, web recovery tests, build, and ctest
./scripts/verify-gate.sh

# Strict gate: mirrors CI release posture with -Werror
./scripts/verify-gate.sh --strict
```

The script verifies:
1. Naming and attribution rules.
2. Never-stage exclusions (artifacts, private roadmaps, logs).
3. Node.js web syntax check (`node --check exam/web/app.js`).
4. Web recovery test suite (`node tests/exam_web_recovery_tests.js`).
5. Zero compiler warnings with `-Werror`.
6. 100% test pass rate under `ctest`.

## Packaging the installer

On Windows with [NSIS](https://nsis.sourceforge.io/) installed (`makensis.exe` in `PATH`):

```powershell
cmake --build build-msvc --config Release --target nstu-package
```

The output is `nstu-<version>-setup.exe` in the build directory.
The installer presents an explicit choice between Client and Server, verifies that the opposite role is not already installed, and invokes `nstu-diagnostics` sequentially.

## Secrets, memory, and Deep Freeze

- Never commit PSKs, DPAPI entropy, certificates, crash dumps, memory snapshots, screen captures, logs, or runtime state.
- Local directories ignored by Git include `local/`, `memory/`, `dumps/`, `logs/`, `runtime/`, and all build trees.
- Runtime secrets must be stored using machine-scoped DPAPI or Windows certificate store.
- Named pipes and memory-mapped files reduce temporary disk activity; they do not replace ACLs, authentication, key rotation, or Deep Freeze persistence requirements.

## Security and protocol

- Connection preamble is a quick filter for magic, version, role, `client_id`, and `key_id`; it does not prove client identity.
- Client identity is accepted only after mutual HMAC handshake succeeds.
- Video streams authenticate HMAC before packet-loss accounting and frame reassembly.
- Screen contents are authenticated but not encrypted; sensitive screens require an encrypted physical LAN or VPN.

See [ARCHITECTURE.md](ARCHITECTURE.md), [SECURITY.md](SECURITY.md), [PACKET_LOSS.md](PACKET_LOSS.md), [VIDEO_REASSEMBLY.md](VIDEO_REASSEMBLY.md), and [ROADMAP.md](ROADMAP.md) for architectural details.

## Fork and development workflow

1. Open the repository on GitHub and click **Fork**.
2. Clone your fork and configure the upstream repository:

   ```powershell
   git clone https://github.com/<your-account>/nstu.git
   cd nstu
   git remote add upstream https://github.com/Khoasoma/nstu.git
   ```

3. Create a feature branch:

   ```powershell
   git fetch upstream
   git switch -c feature/<short-name> upstream/main
   ```

4. Build with `NSTU_ENABLE_WERROR=ON`, run tests, and check diff:

   ```powershell
   ./scripts/verify-gate.sh --strict
   git diff --check
   ```

5. Commit with clear, purposeful messages and push to your fork:

   ```powershell
   git add <files>
   git commit -m "scope: concise description of changes"
   git push -u origin feature/<short-name>
   ```

6. Open a Pull Request from your fork to `Khoasoma/nstu:main`. Include test evidence, protocol changes, and compatibility considerations.

## License and third-party dependencies

Project NSTU is released under the [MIT License](../LICENSE).
All runtime dependencies must be compatible with MIT, Apache-2.0, or equivalent permissive licenses; GPL dependencies are not accepted.
Dear ImGui is fetched at CMake configure time under the MIT License. Windows SDK, Winsock, DXGI, D3D11, and Media Foundation are system components of Windows and are not redistributed as source.
See [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Production validation status

IOCP dispatcher, persisted keyring, enrollment transport, snapshot routing, annotation/broadcast, packetizer, jitter buffer, NACK, keyframe scheduling, service-agent routing, signing automation, native WebView2 kiosk, and discrete timing telemetry are implemented and tested.
Passing CI proves compilation and automated correctness. Prior to real school deployments, production code signing with genuine certificates, protocol fuzzing, a 50-client physical lab soak test, and the validation checklist in [PRODUCTION_VALIDATION.md](PRODUCTION_VALIDATION.md) must be completed.
