# Security Protocol

## Scope

The current security layer provides protocol primitives for authenticated
control sessions and authenticated UDP video packets. It is not yet a complete
key-provisioning system and it does not encrypt screen content.

## Control handshake

Each installed client has a 128-bit client ID and references a provisioned key
using `key_id`. The pre-shared key itself must come from protected local machine
storage and must never be committed to the repository.
Protocol keys shorter than 256 bits are rejected.

```text
Client -> Server: AuthHello(client_id, client_nonce, unix_time, key_id)
Server -> Client: AuthChallenge(server_nonce, unix_time)
Client -> Server: HMAC(PSK, labelled transcript)
Server -> Client: HMAC(session_key, labelled transcript)
```

Both nonces are 256-bit values generated with Windows CNG. Client and server
proofs use different domain labels. The session key is also derived under a
separate label, preventing the same HMAC input from being reused across roles.

The server must verify the client MAC before inserting the hello into
`ReplayProtector`. Replay insertion is atomic and bounded. Capacity must be
sized for the maximum accepted handshake rate over the clock-skew interval, and
the TCP listener must separately rate-limit unauthenticated connections.

## Authenticated control frames

After the handshake, every command carries:

- a strictly monotonic 64-bit sequence;
- a 128-bit truncated HMAC-SHA256 tag;
- the normal command envelope and payload.

The MAC covers a domain label, serialized command envelope, sequence, and
payload. Because TCP preserves order, `ControlSequenceGuard` requires the exact
next sequence. A reconnect creates a new session key and resets both directions
to independently negotiated initial sequences.

Verify the MAC before applying the sequence guard. Only advance the guard after
successful verification. Do not execute, log as trusted, or acknowledge an
unauthenticated command.

## UDP video authentication

The datagram layout is:

```text
[44-byte video header][16-byte authentication tag][H.264 fragment]
```

The tag is a truncated HMAC-SHA256 over a video-specific domain label, the
serialized header, and payload. Verify it before packet-loss accounting or
frame reassembly; otherwise spoofed packets can corrupt both metrics and video.

One client-derived session key cannot authenticate a shared multicast stream.
The server therefore needs a random video group key, distributed individually
over each authenticated control session. Rotate the group key when membership
changes or according to policy, and allocate a new `stream_id` and sequence
baseline for the rotated stream.

## Reassembly ordering

The required receive order is:

```text
validate datagram length
  -> decode fixed header
  -> select authenticated stream/key
  -> verify video HMAC
  -> packet-loss tracker
  -> frame reassembler
  -> decoder/jitter buffer
```

Neither `PacketLossTracker` nor `FrameReassembler` auto-selects a stream from an
untrusted first datagram. Both must be reset from authenticated control-plane
metadata before UDP reception begins.

## Remaining blockers

- Protected PSK enrollment and storage using DPAPI or machine certificates.
- Automated key rotation and revocation.
- Secure group-key distribution messages wired into the control connection.
- Connection-level rate limiting and audit events.
- Confidentiality: HMAC authenticates but does not encrypt screen content.
  AES-GCM group encryption or an equivalent design is required where LAN users
  must not be able to view captured traffic.
- Independent protocol review and fuzzing before deployment.
