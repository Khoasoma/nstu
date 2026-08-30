# Current Task Log and Roadmap

## Completed in the experimental MVP

- [x] Modular CMake build for common, video, client, server, and tests.
- [x] Local-only memory, dump, trace, capture, and build exclusions.
- [x] Versioned binary protocol and bounded incremental TCP parser.
- [x] Winsock TCP/IOCP and UDP multicast foundations.
- [x] Multicast-to-unicast fallback state machine.
- [x] Reorder-aware packet-loss estimator and server health hysteresis.
- [x] Mutual HMAC handshake, replay cache, control/video MAC primitives.
- [x] Live blocking TCP handshake and authenticated control channel.
- [x] Fixed connection preamble for early role/version/identity filtering.
- [x] DPAPI machine-scoped secret storage with restrictive ACL and atomic replace.
- [x] In-process key enrollment, monotonic rotation, revocation, and zeroization.
- [x] Bounded frame reassembly, deadlines, duplicate/conflict handling.
- [x] Desktop Duplication capture and device-loss reporting.
- [x] GPU BGRA-to-NV12 conversion.
- [x] Hardware H.264 MFT configuration and asynchronous event handling.
- [x] Service/session-agent separation, DACL helpers, named pipe, tray, overlay.
- [x] Dear ImGui/D3D11 server shell and thread-safe client state registry.
- [x] IOCP completion-port create, socket association, wait, and post wrappers.
- [x] Unit/integration tests and Windows CI.

## Next engineering milestones

- [ ] Add persisted keyring loading/saving and authenticated enrollment transport
      around the in-process key lifecycle manager.
- [ ] Complete IOCP accept/receive/send dispatcher for 50+ clients.
- [ ] Packetizer, jitter buffer, NACK policy, and keyframe scheduling.
- [ ] Wire live service-agent commands for lock/unlock and status reporting.
- [ ] Connect server UI actions to the control plane.
- [ ] Device-loss recovery loop for duplication, converter, and encoder.
- [ ] Installer, signed binaries, service recovery configuration, and thaw-space
      configuration for Deep Freeze deployments.
- [ ] 50-client soak, switch multicast matrix, Windows/Intel driver matrix, and
      memory/CPU benchmark capture.

## Context note

The repository is intentionally an MVP foundation. A green build demonstrates
API integration and local correctness; it does not establish production
reliability on unmanaged switches or heterogeneous school hardware.
