# Cancel boundary v2 — 2026-09-24

## Observed behavior in the supplied log

- Boot at uptime 0; WebSocket authenticated at 5872 ms; WakeWord detected at
  73853 ms. This captured boot did require a wake word before upload.
- First barge-in: cancelled at 87037 ms, listening at 87047 ms. The delayed
  inactive-response message at 87221 ms did not disconnect the session.
- Second barge-in: cancellation began at 104849 ms. At 107860 ms the application
  initiated recovery because its three-second confirmation deadline expired.
  Authentication and automatic conversation resumption completed at 115442 ms.
  The log cannot distinguish absent, late or unrecognized server confirmation.
- At 127559 ms the transport read failed. A connection attempt then timed out
  at 135647 ms. Authentication succeeded at 144132 ms without a voice trigger.
  This is a separate network/transport issue; the log does not identify whether
  the cause is the gateway, Internet path, AP or client transport implementation.
- A later barge-in at 158223 ms is recorded; whether this was intended speech
  or residual speaker echo cannot be established from energy alone.
- The log ends with ClearCommError and the monitor waiting for USB reconnection.
  It contains no second boot sequence. The user switched the board power while
  leaving USB connected; that alone is insufficient to establish CPU reset.

## Implementation

- Record wire response completion independently from speaker progress. Tag
  queued frames with a local response turn number and reject stale turn data.
- If a barge-in interrupts an already-received complete reply, stop local
  playback and discard its remaining frames without sending response.cancel.
- For a live server response, retain the cancellation boundary requirement.
  Accept response.audio.done and explicit terminal forms (response.done with
  completed/cancelled/canceled status, response.cancelled, response.canceled).
  Their presence on this gateway remains to be verified with new logs.
- At three seconds print cancellation diagnostics instead of disconnecting.
  Recovery remains at eight seconds if no safe boundary is received. Silence
  or absence of audio is not treated as successful cancellation.
- Increase the PSRAM microphone ring to about ten seconds to cover that wait.
- Increase the per-operation WebSocket network timeout from 3000 to 10000 ms;
  increase the application's initial connect deadline to 20000 ms.
- Add boot identifier, reset reason, firmware label, transport error details,
  explicit WAIT_WAKE/upload-off log and the reason each listen period begins.
- Conversation continuation is RAM-only and explicitly reset on startup.
  Only a previously awakened conversation within its recovery window can
  resume after reconnect. Automatic connection remains independent of speech.

No AEC thresholds, gain, credentials, service URL or Wi-Fi settings were changed.

## Validation performed

All main C files compiled using the project's existing ESP-IDF 5.4.4 compiler
arguments. Generated archive, linker generation, link and image commands were
replayed directly because the normal build runner encounters Windows subprocess
pipe restrictions in this session. Application and bootloader size checks pass.

- Application size: 0x279d90 bytes; 17% application partition free.
- ELF SHA256: 9638e3e115c37e460bf9e35580d8280b49c4487ea47649615c0ffdf0dcc982e3
- Binary SHA256: d4120cf36f8c69549ce16a7811bf197afc382c11f0b6d4103d214b588b882a15
- reply_boundary_test.c: compiler constant evaluation reduces main to ret i32 0.
- proto_boundary_test.c: cross-compiles successfully; not runtime-executed.
- No board flashing, hardware testing or cloud protocol capture was performed.

## Board verification

1. Flash this project. Confirm `build=cancel-boundary-v2` and the ELF hash prefix
   `9638e3e115`. After initial authentication, expect `Online; WAIT_WAKE` and no
   audio upload until a wake word.
2. Test early, middle and late interruptions. Completed incoming replies should
   print `server already finished`; live replies should print `cancelling active
   server response`. Both should return to listening when the boundary is known.
3. If cancellation is delayed, retain `Cancel pending`, `RX response boundary`
   and `Transport detail` lines to identify what remains missing.
4. On transport recovery, listening may resume without a wake word within the
   existing active-conversation window. The log explicitly labels this source.
5. To check a fresh boot, disconnect all external power including USB and any
   battery, then reconnect and capture a new ESP-ROM/boot identifier sequence.
   Confirm WAIT_WAKE again; do not infer a CPU reset from LED changes alone.

Backup before installation: work/before_cancel_boundary_20260924_141650.
