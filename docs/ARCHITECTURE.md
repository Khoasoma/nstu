# Architecture

## Process model

```text
nstu-server.exe       ImGui/D3D11 administration UI and client state
nstu-service.exe      Session 0 service, policy and privileged lifecycle
nstu-agent.exe        Interactive tray and fullscreen overlay
```

The service and agent are separate because Windows services cannot interact
directly with the logged-in user's desktop. IPC uses a local named pipe whose
DACL permits SYSTEM, Administrators, and the interactive user and rejects
remote clients.

## Video path

```text
DXGI Desktop Duplication (BGRA texture)
  -> D3D11 Video Processor (NV12 texture)
  -> Media Foundation hardware H.264 MFT
  -> application packetization
  -> UDP multicast or UDP unicast fallback
```

Raw frames remain in GPU memory. The compressed H.264 access unit becomes
CPU-visible for Winsock packetization; therefore the architecture avoids raw
frame copies but is not literally copy-free through the NIC.

## Control path

TCP frames are length-prefixed and contain explicitly serialized little-endian
headers. No C++ struct memory layout is placed directly on the wire. Parsers
enforce payload and buffered-byte limits before allocating.

Control authentication uses a nonce-based mutual HMAC handshake, derived
session keys, strict per-direction command sequences, and replay protection.
See `docs/SECURITY.md`.

Each TCP connection begins with a fixed-size role/version preamble carrying the
client identity hint and key ID. It is a cheap admission filter and does not
replace cryptographic authentication.

## Packet loss

Video protocol v2 includes a monotonic packet sequence independent of frame and
fragment identifiers. Receivers use a bounded reorder window and only confirm a
loss after the missing sequence leaves that window. See `docs/PACKET_LOSS.md`
for reporting, hysteresis, stream-reset, and fallback rules.

## Known limitations

- Authentication primitives are implemented, but protected key provisioning
  and live connection integration remain.
- IOCP create/associate/dequeue/post primitives exist, but the complete
  multi-client AcceptEx/WSARecv/WSASend dispatch loop is not connected to the
  server UI.
- Bounded UDP frame reassembly is implemented; jitter buffering, NACK policy,
  and keyframe scheduling remain.
- The service/agent IPC transport exists, but command routing from server to
  overlay is not wired end to end.
- The encoder accepts NV12 textures and handles asynchronous MFT events, but
  long-duration rate-control and device-loss recovery need soak testing.
- The UI currently seeds demonstration clients instead of live discovery.
