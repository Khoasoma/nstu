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

The service loads its client identity and PSK from a machine-scoped DPAPI
configuration, reconnects to the teacher server, completes the mutual HMAC
handshake, and forwards authenticated commands to the active-session agent.
Agent status is returned over the same pipe and then reported to the server.

The server shell presents two operational modes. `Room screens` uses a
responsive grid for all visible clients, health summaries, search/filter, and a
5-15 FPS screen-refresh target. `Selected client` provides focused telemetry, a
16:9 preview, control actions, and chat. Preview surfaces are deliberately
stateful and show no-frame status until authenticated video packets are
connected. The registry starts empty and does not inject demonstration client
records. The client agent uses standard Win32 LISTBOX/EDIT/BUTTON controls for
a small chat window; no UI framework is added to the client.

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

The server uses a bounded IOCP dispatcher with posted `AcceptEx`, one outstanding
`WSARecv` per connection, serialized queued `WSASend`, source admission limits,
and structured audit callbacks. The raw receive path feeds an asynchronous
preamble/handshake state machine before authenticated frames can update the
registry or execute dashboard commands.

## Packet loss

Video protocol v2 includes a monotonic packet sequence independent of frame and
fragment identifiers. Receivers use a bounded reorder window and only confirm a
loss after the missing sequence leaves that window. See `docs/PACKET_LOSS.md`
for reporting, hysteresis, stream-reset, and fallback rules.

## Known limitations

- The authenticated packetizer, reassembler, jitter buffer, NACK policy, and
  recovery pipeline are implemented, but the application does not yet connect
  encoded UDP sockets, group-key rotation, H.264 decoding, and ImGui preview
  textures end to end. Screen surfaces therefore still show no-frame state.
- Video group-key payload codecs exist, but membership-driven key generation and
  distribution are not yet wired to stream startup.
- Video HMAC provides integrity and source authentication, not confidentiality.
- Long-duration rate control, repeated device loss, multicast/unicast behavior,
  and 50-client resource use still require the hardware validation matrix in
  `PRODUCTION_VALIDATION.md`.
