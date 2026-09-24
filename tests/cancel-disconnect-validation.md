# Cancel / Receive Backlog Fix (2026-09-24)

## Evidence

The supplied log shows successful cancel completion at 177332 ms and 197023 ms,
followed by `Conversation has none active response` at 177479 ms and 197149 ms.
The application then explicitly invokes session recovery. These two disconnects
are caused by error classification, not evidence of Wi-Fi loss.

At 150561 ms, 217257 ms and 232576 ms, the 96-entry receive queue fills while the
worker is playing buffered audio. This also invokes application-driven recovery.

## Changes

- Recognize the exact ACOS message as well as `response_cancel_not_active`.
  Complete a pending cancellation once; ignore a delayed duplicate without
  changing the next response or reconnecting. Other errors retain recovery.
- Buffer compressed Opus in PSRAM with 4096 FIFO slots, a 1 MiB audio payload
  budget, 32 reserved control slots and 128 KiB control headroom.
- Keep receive callbacks nonblocking and preserve control/audio FIFO ordering.
  At the audio limit, drop incoming audio with a throttled warning rather than
  removing queued control messages or reconnecting. This can cause missing sound
  under exceptional overload; buffering is intentionally bounded.

## Validation

The changed service compiled using the existing ESP-IDF 5.4.4 project compiler
arguments. Generated archive/link/image steps and application/bootloader size
checks passed. Application size: 0x279330 bytes, 18% application partition free.
Ninja's Windows subprocess pipe limitation was bypassed using the existing
generated commands; a normal `idf.py build` was not claimed to have completed.

`ws_rx_policy_test.c` covers exact and length-bounded error classification, the
reported delayed cancel error, a 30-second incoming burst and queue/byte limits.
The test source cross-compiled successfully; it has not been executed on a host
or board. No flashing or hardware validation was performed.

## Hardware Acceptance

1. Confirm startup prints `RX buffer: 4096 frames, 1024 KiB Opus budget in PSRAM`.
2. Interrupt at the beginning, middle and end of replies, including replies that
   the server may already have fully sent but the speaker is still playing.
3. `Cancel already completed; keeping WebSocket connected` may appear. It must
   not be followed by protocol-error recovery for that same inactive-response
   message. Ask another question immediately after listening resumes.
4. Request a longer reply. Ordinary bursts should not emit the old `RX queue
   full; recovering session` message. Record any `Audio backlog limit` warning.
5. Real network errors and cancellation acknowledgement timeouts still use the
   existing recovery path. AEC false triggers are outside this change.
