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
- [x] Bounded unauthenticated handshake rate limiter with temporary blocking.
- [x] CPack/NSIS server and client installers with automatic service
      registration, recovery policy, reboot activation, and package-owned
      uninstall handling.
- [x] Teacher-focused server UI with room screen wall, responsive 5-15 FPS
      target, health summary, filtering, focused telemetry, chat and controls;
      lightweight Native Win32 client chat shell.
- [x] Bounded frame reassembly, deadlines, duplicate/conflict handling.
- [x] Desktop Duplication capture and device-loss reporting.
- [x] GPU BGRA-to-NV12 conversion.
- [x] Hardware H.264 MFT configuration and asynchronous event handling.
- [x] Service/session-agent separation, DACL helpers, named pipe, tray, overlay.
- [x] Dear ImGui/D3D11 server shell and thread-safe client state registry.
- [x] Empty-by-default server registry with no demonstration client injection.
- [x] IOCP completion-port create, socket association, wait, and post wrappers.
- [x] Unit/integration tests and Windows CI.
- [x] Bilingual deployment-facing README with installation, network, licensing,
      Deep Freeze, and honest MVP-status guidance.

## Completed production engineering

- [x] Persisted, versioned DPAPI keyring with active keys and revoked-ID
      tombstones, plus replay-resistant authenticated bootstrap enrollment.
- [x] Live IOCP `AcceptEx`/`WSARecv`/`WSASend` dispatcher with bounded connection
      capacity, source rate limiting, audit events, and a 64-client integration
      test.
- [x] Authenticated server control plane and reconnecting service client with
      status, heartbeat, lock/unlock, chat, stream, stop, and keyframe commands.
- [x] Live service-agent named-pipe routing and status reporting.
- [x] Server dashboard actions connected to authenticated client sessions.
- [x] Authenticated video packetizer, jitter buffer, bounded NACK policy, and
      keyframe scheduler with deterministic reordering/loss tests.
- [x] Duplication/converter/encoder recovery orchestration with bounded
      exponential retry and hardware keyframe control.
- [x] Authenticode hooks, a certificate-gated production workflow, protected
      configurable data roots for Deep Freeze thaw spaces, and deployment
      validation scripts.
- [x] Reproducible benchmark and multicast-matrix tooling plus a production
      evidence record.

## Remaining production release gates

- [ ] Connect encoded UDP send/receive, authenticated group-key rotation, H.264
      decode, and D3D11 preview textures to the room screen wall. Transport
      primitives exist, but the current preview still has no decoded live frame.
- [ ] Run the production workflow with the real code-signing certificate and
      verify signatures/timestamps on both installers and installed binaries.
- [ ] Validate the conservative Deep Freeze install/uninstall/data-root behavior
      against every edition and version the project claims to support.
- [ ] Execute and attach evidence for the 50-client soak, multicast switch
      matrix, forced unicast fallback, Windows build matrix, Intel driver matrix,
      and CPU/RAM/network benchmarks.
- [ ] Complete independent protocol review and fuzzing. Where the LAN threat
      model requires screen confidentiality, add authenticated encryption before
      deployment; the current video format authenticates but does not encrypt.

## Context note

The repository now contains the control, enrollment, persistence, packetization,
and recovery implementations that were previously roadmap stubs. A green build
still does not establish production reliability, decoded live-screen delivery,
certificate trust, or compatibility with heterogeneous school hardware. The
evidence gates above remain release-blocking.
