# CONTROL-CHANNEL-AEAD-ENCRYPTION-1 Task Plan — November 2, 2026

## 1. Context and Objective

In [tasks/task-plans/phase5-exam-security-delivery-plan.md](phase5-exam-security-delivery-plan.md), Task 20 delivers Authenticated Encryption with Associated Data (AEAD) across the control channel.

Currently, the control protocol wraps frames in a 20-byte `CommandEnvelope`, an 8-byte sequence counter, and a 16-byte HMAC-SHA256 authentication tag. While this provides authenticity, payloads (screen JPEG frames, student answers, chat messages, and telemetry samples) are transmitted in plaintext over the local network.

Task 20 adds confidentiality and authenticated encryption using AES-256-GCM via Windows CNG (`BCryptEncrypt` / `BCryptDecrypt`):
1. **Capability Negotiation**: Adds `kPreambleFlagAead = 0x01` to `protocol::ConnectionPreamble` so that client and server negotiate AEAD capability during handshake, maintaining complete backward compatibility with non-AEAD peers.
2. **Directional Key & Nonce Derivation**: Derives independent directional 256-bit AES keys (`client_to_server_key`, `server_to_client_key`) and 96-bit base nonces (`c2s_iv`, `s2c_iv`) from the master session secret using domain-separated HMAC-SHA256 transcripts.
3. **Deterministic Monotonic Nonce Construction**: Nonces are formed by XORing the 64-bit monotonic frame sequence counter into the 12-byte IV base (`iv_base ^ sequence`), strictly preventing nonce reuse.
4. **Associated Data (AAD) Binding**: Authenticates the 20-byte `CommandEnvelope` header and the 8-byte sequence counter as Associated Authenticated Data, ensuring any tampering with headers or sequences causes immediate AEAD tag verification rejection.
5. **Zero Payload Invariants**: Correctly handles 0-byte payloads (e.g. state requests, simple commands) while authenticating AAD.
6. **Graceful Fallback & Strict Testing**: Retains support for legacy HMAC framing when AEAD is not negotiated, with comprehensive test coverage for unreadable ciphertext, tamper detection, sequence rejection, and replay protection.

---

## 2. Technical Architecture & Cryptographic Design

### 2.1 Capability Flag
In `common/include/nstu/protocol.hpp`:
```cpp
inline constexpr std::uint8_t kPreambleFlagAead = 0x01;
```

When client and server both set `kPreambleFlagAead` in their `ConnectionPreamble.flags`, the connection enters AEAD mode. If either side does not set the flag, the connection falls back to legacy HMAC-SHA256 authentication.

### 2.2 Directional Key Derivation
From the 32-byte `session_key` derived during handshake:
- `c2s_key` = HMAC-SHA256(session_key, "NSTU-AEAD-C2S-KEY-V1") [32 bytes]
- `s2c_key` = HMAC-SHA256(session_key, "NSTU-AEAD-S2C-KEY-V1") [32 bytes]
- `c2s_iv`  = first 12 bytes of HMAC-SHA256(session_key, "NSTU-AEAD-C2S-IV-V1") [12 bytes]
- `s2c_iv`  = first 12 bytes of HMAC-SHA256(session_key, "NSTU-AEAD-S2C-IV-V1") [12 bytes]

### 2.3 Nonce Construction
Given 12-byte `iv_base` and 64-bit `sequence`:
```cpp
std::array<std::byte, 12> nonce = iv_base;
for (std::size_t i = 0; i < 8; ++i) {
    nonce[4 + i] ^= static_cast<std::byte>((sequence >> (i * 8u)) & 0xffu);
}
```
Every frame sequence increments monotonically, guaranteeing unique nonces per key.

### 2.4 Wire Frame Format
The wire format retains the same framing overhead as legacy:
- 4-byte length prefix
- 20-byte `CommandEnvelope` header (with `payload_bytes = 24 + payload.size()`)
- 8-byte `sequence` (little-endian)
- 16-byte AES-GCM authentication tag
- `payload.size()` bytes of AES-256-GCM ciphertext

AAD:
- 20-byte normalized `CommandEnvelope` header
- 8-byte `sequence`
Total AAD = 28 bytes.

### 2.5 CNG AES-GCM Implementation
Uses Windows CNG `BCRYPT_AES_ALGORITHM` with `BCRYPT_CHAIN_MODE_GCM` and `BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO`. All key objects and secrets are securely wiped with `security::secure_zero`.

---

## 3. Allowlisted Product Files (6)

1. `common/include/nstu/protocol.hpp`
2. `common/include/nstu/control_channel.hpp`
3. `common/src/control_channel.cpp`
4. `server/include/nstu/control_plane.hpp`
5. `server/src/control_plane.cpp`
6. `tests/control_channel_tests.cpp`

---

## 4. Verification & Testing

1. **Unit Tests (`tests/control_channel_tests.cpp`)**:
   - `test_aead_roundtrip_and_tamper`: Verifies plaintext encryption, ciphertext unreadability on wire, clean decryption, and rejection of tampered ciphertext, sequence, or header AAD.
   - `test_aead_zero_byte_payload`: Verifies 0-byte commands authenticate headers and detect tampering.
   - `test_aead_channel_end_to_end`: Tests full `client_handshake` and `server_handshake` with AEAD negotiation, bidirectional message exchange, and clean teardown.
   - `test_aead_backward_compatibility`: Tests client/server handshake when one peer does not advertise `kPreambleFlagAead`, ensuring transparent fallback to legacy framing.
2. **Local Gates**:
   - Fast gate: `./scripts/verify-gate.sh` (zero compiler warnings, 100% tests pass).
   - Strict gate: `./scripts/verify-gate.sh --strict` (Release build, `-Werror`, 100% tests pass).
3. **CI Validation**:
   - Feature branch CI passes 4/4 Windows jobs (`gh run watch`).
   - Main branch CI passes 5/5 jobs on fast-forward merge.

---

## 5. Execution Sequence

1. Author task plan `tasks/task-plans/control-channel-aead-encryption-1.md`.
2. Add `kPreambleFlagAead` to `common/include/nstu/protocol.hpp`.
3. Update `common/include/nstu/control_channel.hpp` and `common/src/control_channel.cpp` with AEAD key derivation, AES-GCM encryption/decryption, and handshake negotiation.
4. Update `server/src/control_plane.cpp` and `server/include/nstu/control_plane.hpp` to negotiate and process AEAD control frames.
5. Expand `tests/control_channel_tests.cpp` with comprehensive AEAD tests.
6. Run fast and strict verification gates.
7. Commit with author `khoasoma <songtu438@gmail.com>` and push to feature branch.
8. Validate feature CI, merge to `main`, validate main CI, and record results.
