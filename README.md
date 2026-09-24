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

Playback interruption is experimental: the current implementation uses AFE
VAD plus an energy threshold, a 1.2-second playback guard, and ten consecutive
speech frames. These settings were added to reduce false interruptions and
still require hardware validation. A successful build does not establish AEC
quality or reliable interruption behavior in a particular acoustic setup.

## Source layout

- `main/`: application, state machine, Wi-Fi, WebSocket, and audio services.
- `components/`: local component dependency declarations.
- `dependencies.lock`: exact managed dependency versions.
- `sdkconfig` and `sdkconfig.defaults`: build configuration.
- `partitions.csv`: flash layout.

Build outputs, downloaded components, local backups, editor settings, and logs
are excluded by `.gitignore`.
