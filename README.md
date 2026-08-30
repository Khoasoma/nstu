# nstu

`nstu` is an experimental Windows classroom/lab management system focused on
low-overhead control and one-to-many screen broadcast.

## Current MVP

- Versioned, bounded binary command and video headers.
- Incremental TCP framing plus Windows TCP/IOCP primitives.
- UDP multicast sender/receiver and automatic unicast fallback policy.
- Reorder-aware packet-loss accounting with duplicate/late/jump protection.
- Mutual HMAC handshake, replay protection, and authenticated control/video
  packet primitives.
- Bounded UDP frame reassembly with deadline and memory-pressure accounting.
- DXGI Desktop Duplication capture.
- GPU BGRA-to-NV12 conversion using the D3D11 video processor.
- Media Foundation hardware H.264 encoding with asynchronous MFT events.
- Windows service, active-session agent launcher, service DACL helper, secure
  named-pipe IPC, tray icon, and fullscreen overlay.
- Dear ImGui + D3D11 server shell with a thread-safe client registry.

This is an engineering MVP, not production-ready classroom software. Network
authentication, a complete server/client control loop, retransmission policy,
installer/updater, and fleet soak testing remain mandatory before deployment.

## Build

Requirements: Windows 10/11, CMake 3.25+, a Windows SDK, and MSVC or MinGW-w64.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Dear ImGui 1.92.0 is fetched by CMake and is MIT licensed. Disable the server UI
dependency with `-DNSTU_SERVER_USE_IMGUI=OFF`.

## Local-only data

Build trees, logs, crash dumps, ETW traces, captures, recordings, local config,
secrets, and memory/heap artifacts are ignored by Git. Use `local/`, `memory/`,
or `dumps/` for developer-only runtime output.

## License

MIT. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
