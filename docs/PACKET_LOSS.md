# Packet Loss Detection

Packet loss is measured per authenticated video stream using the monotonic
`packet_sequence` field introduced by video protocol version 2. `frame_id` and
`fragment_index` must not be used as a substitute: frame sizes vary, frames may
be skipped by the encoder, and fragments may arrive out of order.

## Sender requirements

- Generate a random `stream_id` whenever the sender pipeline restarts.
- Start `packet_sequence` at any value and increment it exactly once for every
  emitted UDP datagram, including retransmissions sent as new datagrams.
- Never reset a sequence counter while retaining the same `stream_id`.
- Authenticate stream setup over the TCP control channel. A receiver must not
  reset its tracker merely because an unexpected UDP stream appears.
- Include the first expected packet sequence in that authenticated setup and
  call `reset(stream_id, initial_packet_sequence)` before joining the stream.
  Without this baseline, loss or reordering before the first observed datagram
  is fundamentally unobservable.

At 625 packets/second, a 64-bit sequence does not wrap in any practical system
lifetime. Wraparound is therefore treated as a new stream, not special modular
ordering logic.

## Receiver algorithm

`PacketLossTracker` maintains a bounded bitmap over a reorder window, 256
packets by default.

1. A gap is initially pending, not lost.
2. A missing packet becomes `confirmed_lost` only after its sequence leaves the
   reorder window.
3. A packet filling a pending gap is `reordered` and prevents false loss.
4. A sequence already present in the window is `duplicate`.
5. A packet arriving after its sequence was finalized is `too_late`. It does
   not decrement confirmed loss, because historical reports must remain stable.
6. A packet from another stream is `wrong_stream` and cannot reset state.
7. An implausibly large forward jump is rejected without advancing the window.
   This prevents one corrupted or spoofed datagram from manufacturing a huge
   loss event.

`flush()` may finalize the remaining window only when a stream is explicitly
ended. Do not flush on a timer or temporary silence; doing so converts normal
jitter into false loss.

## Reported metrics

The loss denominator is:

```text
finalized_received + confirmed_lost
```

Packets still inside the reorder window are excluded. `unique_received` is a
diagnostic counter and is not an appropriate denominator because it includes
unfinalized packets and would bias the loss percentage downward.

Telemetry should report counter deltas over fixed intervals, not the lifetime
cumulative ratio. Use `packet_loss_delta(newer, older)` and discard the interval
if it returns no value. Delta snapshots include both stream ID and tracker
generation, so resetting even with an accidentally reused stream ID cannot
produce a plausible but invalid interval.

Recommended report interval:

- One-second counter snapshots.
- Minimum 200 finalized packets before making a health decision.
- Retain raw counts alongside the per-mille percentage.
- Report reorder, duplicate, too-late, wrong-stream, and invalid-jump counters
  separately.

## Health and fallback policy

The initial server policy uses hysteresis:

- Degraded: at least 5% confirmed loss for three qualifying windows.
- Recovered: at most 2% confirmed loss for five qualifying windows.
- Samples below 200 finalized packets do not change health state.
- Values between 2% and 5% reset both streaks and preserve the current state.

Do not switch from multicast to unicast solely because one client reports one
bad interval. Delivery selection should combine:

- multicast probe success;
- several packet-loss intervals;
- whether failures affect one client, one switch segment, or most clients;
- TCP heartbeat health;
- server NIC and switch capacity for unicast fallback.

Use randomized backoff and a minimum residence time before switching delivery
mode again. Otherwise multiple clients can oscillate between multicast and
unicast simultaneously. The current `DeliveryModeSelector` also requires five
successful multicast probes before recovery from unicast by default.

## Packet loss versus frame loss

Transport loss and visible frame loss are different metrics.

- A missing UDP datagram is transport packet loss.
- A frame is incomplete when its fragment deadline expires with one or more
  missing fragments.
- A recovered retransmission does not restore the historical transport-loss
  counter, but it can prevent effective frame loss.
- Dropping an entire late frame intentionally is a playout decision, not packet
  loss.
- Encoder frame skipping is not network loss.

The future reassembler must track incomplete frames and deadline drops
separately from `PacketLossTracker`.

## Operational validation

Before deployment, replay captures containing:

- in-order traffic;
- bounded and extreme reordering;
- duplicates;
- burst loss and isolated loss;
- delayed packets arriving after finalization;
- sender restart with a new stream ID;
- stale packets from the previous stream;
- corrupted large sequence jumps;
- sequence gaps during multicast-to-unicast transition.

Packet capture analysis should compare receiver counters against an independent
sequence-number script. Windows receive-buffer overflow, NIC drops, switch
drops, and radio loss all appear as sequence gaps, so root cause requires ETW,
adapter counters, and switch telemetry in addition to application metrics.
