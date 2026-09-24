# Korvo Agent

ESP-IDF firmware for the ESP32-S3-Korvo-2 V3.1 voice assistant.

## Hardware and toolchain

- ESP32-S3-Korvo-2 V3.1 with ES7210 microphones and ES8311 playback.
- ESP-IDF 5.4.4, 16 MB flash, 8 MB octal PSRAM.
- WakeNet model: `wn9s_nihaoxiaozhi`.
- 16 kHz mono Opus audio over an authenticated ACOS WebSocket connection.
- Component versions are recorded in `dependencies.lock`.

## Configuration

Wi-Fi credentials and the ACOS token have been removed from this source copy.
Before building, fill in these definitions in `main/board_config.h` locally:

- `DEFAULT_WIFI_SSID`
- `DEFAULT_WIFI_PASSWORD`
- `DEFAULT_ACOS_TOKEN`

Do not commit real credentials. This version reads saved NVS settings first;
changing defaults does not overwrite credentials already stored on a board.
It does not include the web provisioning module from other project versions.

The checked-in `sdkconfig` preserves the current flash, PSRAM, and speech model
settings. `partitions.csv` includes the speech model partition. Keep both files.

## Build and flash

Open an ESP-IDF 5.4.4 terminal in this directory. On Windows, install Git for
Windows with `patch.exe`; CMake searches its usual `usr/bin` location because
the micro-opus component needs that tool.

```powershell
idf.py build
idf.py -p COM3 flash monitor
```

Replace COM3 with the board's port. Exit the monitor with Ctrl+]. The initial
build requires network access to download managed components. Flash the full
project so that the bootloader, partition table, application, and speech model
are all written; the application binary alone is not a complete first install.

## Runtime behavior and status

The firmware supports wake-word activation and continued conversation, with a
60-second inactivity timeout. The AFE uses two microphone channels and a
digital playback reference for echo cancellation.

Current firmware marker: `build=pcm-playback-v1`.

- CPU runs at 240 MHz with performance compilation. A dedicated output task
  plays from a 500 ms PCM buffer after a 150 ms prefill, using 10 ms I2S blocks.
- Normal completion waits for the PCM and DMA tail. Interruption clears queued
  audio and transitions to silence while keeping the shared microphone clock running.
- WebSocket recovery retains an already awakened conversation within its timeout;
  a fresh boot requires the wake word. Cancellation observes response boundaries
  before resuming microphone upload.
- Playback interruption uses AFE VAD, energy >=12000, a 1.2-second startup guard,
  and 480 ms of sustained speech. AEC uses a software playback reference; acoustic
  alignment still requires board validation.
- `Playback stats` and `AFE stats` summarize buffering and processing time.

The owner reported that slow/choppy playback and popping were resolved in the
latest board test. This is not a guarantee across all networks/acoustic setups.
Known limitation: while playing, only about 0.5 seconds of microphone history is
retained. Detection and cancellation delays can truncate the beginning of a short
interruption. Longer pre-roll and protected speech-onset buffering have been
discussed but are **not implemented** in this snapshot.

Full ESP-IDF build and partition checks passed for the local source before its
credentials were removed for upload. Pure logic tests under `tests/` were checked
at compile time; they are not hardware integration tests. Validation notes record
the build-time status, so older notes may predate subsequent user board tests.

With Xtensa GCC 14.2.0, the root CMake configuration applies `-O1` only to the
ESP-DSP image-convolution fallback that crashes that compiler at `-O2`.

## Source layout

- `main/`: application, state machine, Wi-Fi, WebSocket, and audio services.
- `components/`: local component dependency declarations.
- `dependencies.lock`: exact managed dependency versions.
- `sdkconfig` and `sdkconfig.defaults`: build configuration.
- `partitions.csv`: flash layout.
- `tests/`: regression source files and validation records.

Build outputs, downloaded components, local backups, editor settings, and logs
are excluded by `.gitignore`.
