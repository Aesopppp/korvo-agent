# barge-guard-v3 verification — 2026-09-24

Project: `D:\esp32s3v1.0\korvo_agent`

## Incident and diagnosis

Input log: attachment `c4570894-2d81-4e38-b37f-0322a0640e2e`.
The user confirmed they were not speaking or intentionally interrupting the final answer.
At 61.851 seconds the firmware reported a playback barge-in with mean-square PCM
energy 2950; at 61.871 seconds it reported a normal reply completion. There was
no WebSocket disconnection during this response. The preceding response also
triggered at energy 3435.

Code review found that the former cancel_requested flag was set before obtaining
playback_lock, while the receive worker used that flag to discard queued audio.
The worker could discard the tail, consume its terminal boundary and report normal
completion before the main task obtained the mutex. This race is consistent with
the observed 20 ms transition; the log does not contain per-frame evidence of the
exact number of discarded frames.

## Changes

- Playback cancellation changes response_phase only while holding playback_lock.
  Until cancellation is committed, the worker continues to admit valid audio.
- cancel_waiting is a scheduling hint only: the worker yields one tick between
  packets so the cancelling task can obtain the mutex. It never discards audio.
- Playback-only speech detection now requires VAD speech, energy >=12000 and
  7680 continuous valid mono samples (480 ms at 16 kHz), after a 1200 ms startup
  guard. Low energy, silence, missing data or a failed fetch reset accumulation.
- Use returned data_size in bytes for the energy/sample calculation; duration
  is independent of AFE fetch chunk size.
- A barge-in is latched only after successful event delivery.
- Add weak-VAD, cancellation-commit and playback-boundary diagnostics, including
  counts of played and discarded Opus packets.
- Boot marker: build=barge-guard-v3.

Speaker volume remains 40. The software playback reference and AEC configuration
were not changed. No credentials were added to logs or this record.

## Verification completed

- Compiled all 14 application C sources with the existing ESP-IDF compile database.
- Cross-compiled the five portable test sources. Final barge guard, reply boundary
  and response-flow tests were also compiled with ESP Clang -O2 to LLVM IR; each
  main folded completely to `ret i32 0`.
- Guard cases: sustained quiet residuals 2950/3435, startup protection, sample-count
  duration with 512/1024 sample chunks, interrupted bursts, low-energy resets,
  missing samples and counter saturation.
- Response-flow cases include both mutex orderings: completion before delayed
  cancel, and cancellation before remaining audio. These are pure transition
  checks, not a runtime FreeRTOS scheduling test.
- Rebuilt libmain.a, regenerated linker sections, linked the ELF, created the ESP32-S3
  image and ran application/bootloader partition checks using generated build steps.
  Standard idf.py/Ninja was not used for this build because subprocess pipes were
  unavailable in this environment.
- Application size: 2597088 bytes (0x27a0e0), 0x85f20 bytes / 17% of the 3 MiB
  application partition remain free. Bootloader partition check passed.
- Verified installed source hashes against staging and inspected firmware strings
  for the new marker and diagnostics.

Build output: `build\korvo_agent.bin`

ELF SHA256: `32DAE67801C31CB261FEA79FDF1504384E14C32D61C1A9B38A4979730E059980`

BIN SHA256: `F97EE744C965B806DE0EB8BA8E3929C22C7B62503B39FE417FFA700E66A8182A`

Original files backed up at:
`work\before_barge_guard_v3_20260924_145025`

## Hardware validation still required

This firmware has not been flashed or tested acoustically. Increasing a speech
threshold reduces the recorded weak false triggers but does not establish that
echo cancellation is effective; louder residual echo may still cross it and soft
or distant human speech may no longer interrupt reliably.

After flashing this project, confirm build=barge-guard-v3 and ELF prefix 32dae6780.
Test several long answers while silent. Expected: no Barge-in confirmed and a
normal playback boundary without cancellation discards. Weak playback VAD ignored
is diagnostic and must not stop playback.

Then deliberately interrupt after the first 1.2 seconds with sustained speech;
expect Barge-in confirmed, Cancel committed, a local or server cancellation
acknowledgement, resumed listening and a response to the next question. Repeat
with long answers. If a silent stop persists, collect the full log around the
new diagnostics before further acoustic/reference tuning.
