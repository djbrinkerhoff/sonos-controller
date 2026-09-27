# Sonos controller for M5Stack Tab5

A standalone, favorites-first Sonos remote. The Tab5 talks directly to speakers on the home LAN. No permanent server, Apple developer key, audio relay, or separate provider login is needed. Link Apple Music and save favorites using the official Sonos app.

## Current build

This is a **hardware bring-up prototype**, not yet a finished appliance. The ESP32-P4 firmware compiles with pinned ESP-IDF 5.5.3, Tab5 BSP 1.3.1, LVGL 9.4.0 and LVGL port 2.6.2. Hardware flashing, touchscreen QA, Wi-Fi coprocessor compatibility and audible playback still need validation on the Tab5.

Running on the Tab5: Apple Music and Sonos Radio favorites, queue view, artwork, room/group volume, instant updates from the speakers, screen dim/sleep with touch and motion wake, battery indicator with charging fixed, and signed over-the-air updates with rollback. See [HANDOFF.md](HANDOFF.md) for what is verified and what still needs testing.

Implemented:

- Favorites screen restricted to Apple Music and Sonos Radio using service IDs supplied by the household; unknown/conflicting providers are excluded.
- Room selection, now-playing text, play/pause/stop, capability-gated previous/next, room mute, room and group volume.
- Group/ungroup rooms and up to eight saved areas. Group the desired rooms, name the current group, and save it; apply a saved area to restore its membership.
- Speaker discovery by SSDP and mDNS together, remembered room IPs as a fallback, and an optional manual speaker IP.
- On-device Wi-Fi setup, persistent settings and areas.
- Network work on a separate FreeRTOS task, bounded XML responses and paginated favorites, periodic state refresh, fresh coordinator lookup before group playback actions.
- Favorite playback replaces the queue (like the official app's "Replace Queue") and starts it. Radio favorites use their supplied stream URI/metadata. A failed operation is not automatically replayed.
- Read-only Python compatibility probe and host tests for the shared C++ Sonos protocol code.

Still to implement/validate before daily-use release: queue editing, stronger credential storage (needs an eFuse decision), removing or gating the debug HTTP endpoints, and a multi-day soak test. Wi-Fi credentials currently live in ordinary device NVS; no secrets belong in source control. The initial factory partition layout has no OTA slots.

## Household compatibility check

The development probe found five visible Sonos room devices and read 20 favorites without changing playback, volume, or groups. Sixteen Apple Music favorites and one Sonos Radio favorite were classified as supported source types; three browse/promotional entries were excluded. This verifies discovery and metadata access, not that each item starts playback.

```sh
python3 tools/sonos_probe.py --report artifacts/sonos-probe.json
# If discovery is blocked:
python3 tools/sonos_probe.py --speaker 192.168.1.100 --report artifacts/sonos-probe.json
python3 -m unittest discover -s tests -p 'test_*.py'
bash tools/test_core.sh
```

Reports contain private household details and resource metadata. They are excluded from Git by `artifacts/`. The probe requires only Python's standard library and never sends playback or volume commands. See [probe documentation](docs/probe.md).

## Build

```sh
bash tools/bootstrap.sh
bash tools/idf.sh build
```

The toolchain stays in `.tools/`. The firmware binary is `firmware/build/sonos_controller.bin`; use the generated flash arguments, including its matching bootloader and partition table. ESP-IDF component versions are locked in `firmware/dependencies.lock`. The build may need network access on first run; on sandboxed macOS, Component Manager also needs process-inspection permission.

**Detect the P4 chip revision before flashing.** IDF 5.5.3 builds for P4 revisions 0.x/1.x and 3.x are mutually incompatible. The default configuration targets 0.x/1.x. It is not a universal binary. The display driver revision (ST7121/ST7123/ILI9881C) does not identify the P4 chip revision.

For a verified P4 3.x device, create a separate build/configuration:

```sh
bash tools/idf.sh -B build-rev3 -D SDKCONFIG=sdkconfig.rev3.local -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.rev3' build
```

The BSP auto-detects the supported display variants. Wi-Fi uses the internal C6 over SDIO with the Tab5-specific GPIO assignments in `sdkconfig.defaults`. The C6 factory firmware remains separate and must be compatible with the selected Hosted host library.

## First hardware run

1. Connect the Tab5 USB-C device port with a data cable. Identify its serial port and P4 revision with the project environment's `python -m esptool --chip esp32p4 --port PORT chip_id`.
2. Back up its current flash before the first write: `python -m esptool --chip esp32p4 --port PORT read_flash 0 0x1000000 artifacts/tab5-original.bin`. Run from the project root with the `.tools/espressif/python_env/.../bin` environment active. Keep that private backup.
3. Select the matching chip-revision build, then use `bash tools/idf.sh -p PORT flash` for the default build (it writes the bootloader, partition table, OTA data and the app at 0x20000). After that, prefer `bash tools/ota.sh <tab5-ip>` (add the same `-B`/`-D` arguments for revision 3). Follow M5Stack's download-mode instructions if needed. Do not erase flash or replace the C6 firmware as a routine step.
4. Open Settings, enter the 2.4 GHz Wi-Fi name/password, optionally enter one Sonos speaker's IP, and connect. Select a room before tapping a favorite. The selected room's current group is the playback destination.
5. Verify one Apple Music favorite, one Radio favorite, transport controls, room/group volume, external Sonos-app changes, saved areas, reconnect behavior, and correct touch alignment. Use a low existing speaker volume for the first playback test.

Build success alone does not verify board initialization or playback.

Diagnostics over Wi-Fi (opening the USB serial port resets the device): `http://<tab5-ip>:3400/log`, `/tasks` and `/screenshot`. These are unauthenticated and meant for development.

## Over-the-air updates

The firmware now uses an OTA partition layout (`ota_0`/`ota_1` plus `otadata`) with signed app images and automatic rollback. Signing is software-only (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`, RSA-3072); hardware secure boot and flash encryption stay off and no eFuses are burned.

```sh
bash tools/ota_key.sh          # generates firmware/keys/ota_signing_key.pem once; never overwrites
bash tools/ota.sh <tab5-ip>    # builds, then POSTs the signed image to http://<tab5-ip>:3400/ota
```

The device must be flashed over USB **once** with a build that contains the new partition table and the rollback-aware bootloader; before that first flash `tools/ota.sh` has no endpoint to talk to. `esp_ota_end` verifies the signature on the device, so an unsigned or wrongly signed upload is rejected, and an image that fails to call `ota_mark_healthy()` on its first boot is rolled back automatically.

**Back up `firmware/keys/ota_signing_key.pem` somewhere safe.** It is gitignored; losing it means no new image can be signed and updates go back to USB.

## Layout

- `firmware/components/sonos/`: transport-independent C++ Sonos client and TinyXML2 parser.
- `firmware/main/`: native LVGL UI, NVS settings, Wi-Fi, SSDP, HTTP and background worker.
- `tools/`: read-only probe and local build/test scripts.
- `tests/`: synthetic protocol fixtures and regression tests; no private household data.
- `docs/tab5-board.md`: source-verified board details and references.
- `PLAN.md`: architectural choices and longer-term delivery plan.

TinyXML2 10.0.0 is vendored with its upstream license in `firmware/components/sonos/vendor/LICENSE.txt`. ESP-IDF managed components retain their upstream licenses.
