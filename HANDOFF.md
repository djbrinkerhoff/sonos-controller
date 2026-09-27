# Handoff: standalone Tab5 Sonos controller

Updated September 26, 2026. The user explicitly asked to finish the current build/test step, stop, and write this handoff. Do not continue implementation automatically after delivering it.

## User decisions and instructions

- Build a dedicated Sonos controller on an M5Stack Tab5. It should boot into this one purpose.
- Selected architecture: **standalone, favorites-first**, with no always-on helper required. Full Apple Music library browsing is not required for the initial product.
- Only Apple Music and, if workable, Sonos Radio should be launchable. Do not remove other services from the user's Sonos account.
- Control rooms, grouping, saved areas, playback and room/group volume.
- Only used on the same network as the speakers. This Mac is on that network.
- User has the Tab5. We asked them to connect it by USB data cable and identify its rear display label; no reply to that hardware question yet. No USB serial device has appeared so far.
- User's stated inventory: Beam Gen 2, Play:1, Era 100, Era 300, Beam Gen 1; all use the current Sonos app, not S1.
- User explicitly requested **Luna subagents for suitable tasks**. Two Luna agents implemented the read-only probe and board research/native tests. No new chats were created.
- AGENTS instruction: always use Context7 for library/API documentation, code generation and setup. Context7 was used for SoCo, ESP-IDF and LVGL; exact vendor source was also inspected.

## Current implementation

Firmware is a native C++ ESP-IDF/LVGL application, with a transport-independent Sonos core. A Python tool is a development-only read-only probe, not a runtime server.

Important files:

- `README.md`: build/use instructions and explicit prototype limitations.
- `PLAN.md`: chosen architecture plus future alternatives.
- `firmware/main/main.cpp`: LVGL Favorites / Now Playing / Rooms / Settings screens, NVS Wi-Fi and area storage, FreeRTOS worker and command queue.
- `firmware/main/network.cpp`: Wi-Fi via C6, SSDP discovery and bounded SOAP HTTP transport.
- `firmware/components/sonos/sonos.cpp` and `include/sonos.hpp`: room topology, providers, favorites pagination, playback, room/group volume, mute, grouping and applying saved areas.
- `firmware/components/sonos/vendor/`: TinyXML2 10.0.0, with upstream license.
- `firmware/main/idf_component.yml`, `firmware/dependencies.lock`: pinned components.
- `firmware/sdkconfig.defaults`: Tab5 hardware settings, SDIO pins, legacy P4 revision target.
- `firmware/sdkconfig.rev3`: alternate P4 >=3.0 target settings.
- `tools/bootstrap.sh`, `tools/idf.sh`: project-local toolchain setup/build wrappers.
- `tools/sonos_probe.py`, `docs/probe.md`: read-only LAN compatibility probe.
- `tests/core_test.cpp`, `tools/test_core.sh`, `tests/test_probe.py`: synthetic regression tests.
- `docs/tab5-board.md`: source-verified board setup.

The project was an empty directory before this work. There was no Git repository, remote, or existing application to preserve. No commits, PRs or deployment were made.

## What the live network check established

The read-only probe discovered five visible Sonos room devices and successfully fetched topology, service descriptors and favorites. Observed room models included a Sonos One, so do not assume the user's remembered Play:1 model is the exact inventory; use the live topology. Private IP addresses and device identifiers are in the ignored report.

- Twenty favorites returned.
- Sixteen are Apple Music favorites with playback resources; one is a Sonos Radio station with a playback resource (“Set The Table”).
- Three additional entries identify as Sonos Radio through nested metadata but are browse/promotional entries with no playable URI, so they are excluded from launchable favorites.
- Household service descriptors identify Apple Music as service Id/sid **204**, Sonos Radio as **303**. These values are discovered dynamically in code, not hardcoded as the allowlist.
- Favorite URIs carry `sid`; escaped `resMD` DIDL contains service descriptors such as `SA_RINCON52231_...`. The service type relationship is `serviceType = serviceId * 256 + 7`.
- `GroupRenderingControl` uses **urn:schemas-upnp-org:service:GroupRenderingControl:1**, not the rincon namespace.
- Probe report: `artifacts/sonos-probe.json`. It contains private URI/account metadata; keep it out of commits, public docs and chat output.

**No playback, volume or grouping mutations were sent to the user's speakers.** Listing a supported favorite type is not proof of successful playback.

## Build/toolchain findings

Project-local tools are installed under `.tools/`, which is ignored. ESP-IDF tag **v5.5.3**, exact commit `2c211b236707889e8400c4dc5644dd5c4ee071e0`. The local environment currently uses Python 3.9.6; its private venv also has CMake <4 and Ninja.

Pinned components:

- `espressif/m5stack_tab5` 1.3.1
- `lvgl/lvgl` 9.4.0
- `espressif/esp_lvgl_port` **2.6.2**
- `espressif/esp_hosted` 1.4.0
- `espressif/esp_wifi_remote` 0.8.5

Do not casually unpin LVGL port: initially the BSP's `^2` dependency resolved to **2.9.0**, which does not compile against this IDF's DPI callback type (`on_frame_buf_complete` mismatch). Pinning 2.6.2 fixed it.

The BSP supports ILI9881C/GT911, ST7123 and ST7121 through touch-controller detection, despite incomplete registry table wording. LCD's native resolution is 720×1280; application requests 90-degree rotation for landscape. Touch alignment and visual layout are not hardware-verified.

C6 power: `bsp_feature_enable(BSP_FEATURE_WIFI, true)` before remote Wi-Fi initialization. Hosted SDIO pins: reset 15, CLK 12, CMD 13, D0 11, D1 10, D2 9, D3 8. Host clock is conservatively set to 25 MHz and receive streaming optimization disabled. Factory C6 firmware compatibility still needs validation; do not overwrite it routinely.

**P4 chip revision is a mandatory pre-flash check.** IDF 5.5.3 defaults to revision 3.1, which cannot run on earlier chips. The final default build deliberately targets P4 **0.x/1.x**, using `ESP32P4_SELECTS_REV_LESS_V3=y` and minimum revision 0.0. Revision 3.x needs a separate build using `sdkconfig.rev3`. Chip revision and LCD driver revision are different facts.

The latest successful default firmware build produced `firmware/build/sonos_controller.bin`, size `0x19b8c0` bytes (about 1.61 MiB), with 77% of the 7 MiB application partition free. Associated bootloader, partition table and flash arguments are in the same build directory. This is a prototype factory partition layout, without OTA slots.

Build log: `artifacts/firmware-build.log`. Some local incremental builds can run sandboxed, but CMake reconfiguration uses `psutil` process inspection; macOS sandbox denies that. Run `bash tools/idf.sh build` with tool escalation if it fails on `sysctl()` in Component Manager. This was a sandbox restriction, not a firmware error. Network access is also needed when resolving components for the first time.

## Validation status

- ESP-IDF cross-compilation: **passed**, including application, bootloader and partition-size check for the default legacy P4 target.
- Live read-only Sonos discovery/metadata: **passed**.
- Native C++ tests: **passed, 40 assertions**, via `bash tools/test_core.sh`.
- Python probe tests: **passed, 9 tests**, via `python3 -m unittest discover -s tests -p 'test_*.py'`.
- Tab5 flash/boot, touch layout, Wi-Fi connection, audible playback, and grouping/volume mutation checks: **not performed**.

## Current limitations and review points

- This is a bring-up prototype, not a finished appliance. Artwork is parsed but not rendered. Queue browser/editing, battery/charging management, dim/sleep/wake, OTA signing/rollback, and soak testing remain.
- Wi-Fi credentials live in ordinary NVS for now. Source/report secrets are excluded from Git; production credential protection remains.
- State is polled every four seconds, not event-subscribed. Reconnect/IP-change recovery needs hardening. If an existing seed IP becomes stale, discovery/manual IP refresh may be necessary.
- Playback targets the selected room's current coordinator, resolved fresh. Favorite queue content is appended, then the newly added item is selected; the prior queue is not cleared. Radio uses preserved URI/metadata.
- A failed action is not retried automatically; pending queued actions are dropped on error. Partial queue/grouping operations can still have happened before a timeout. The next step should verify actual state before another mutation.
- Areas store stable room IDs. The app saves the selected room's current group; applying an area verifies all saved rooms exist before grouping and excludes outsiders from the restored group. Hardware tests must confirm expected coordinator/music behavior.
- Source filtering fails closed. Unknown/conflicting identities and non-playable promotional favorites are excluded. The controller can display neutral state and stop/adjust volume for content started by another app.
- A computer-based probe or successful build does not establish that the C6 factory firmware, selected P4 revision, touch rotation, or favorite playback path works on the physical Tab5.

## Exact next steps when the user resumes

1. Read this handoff and the final validation note. Inspect current files before changing anything; agents share this directory.
2. Confirm the Tab5 is connected through a USB data cable and inspect `/dev/cu.*`. Only Bluetooth/audio/debug-console ports were visible at the last check.
3. Use the project's Python/esptool environment to read chip info (`--chip esp32p4 --port PORT chip_id`). Select the matching legacy or revision-3 firmware configuration. Do not use `--force` to bypass a revision mismatch.
4. Back up the original full flash to an ignored artifact **before the first flash**. README contains the read-flash command. No backup exists yet because no device was visible.
5. Flash the matching build and watch serial boot logs. Verify screen/touch, then enter Wi-Fi details on-device; don't ask the user to paste passwords into chat.
6. Validate one Apple Music favorite and the Radio favorite at a modest existing volume, then transport, room/group volume, groups, saved areas and external changes from the official app. This is the main unproven product milestone.
7. Fix observed board/protocol issues before expanding UI/features. Then finish the appliance work listed above.

Commands from project root:

```sh
python3 -m unittest discover -s tests -p 'test_*.py'
bash tools/test_core.sh
bash tools/idf.sh build
```

For a verified P4 3.x chip:

```sh
bash tools/idf.sh -B build-rev3 -D SDKCONFIG=sdkconfig.rev3.local -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.rev3' build
```

Do not resume further work in this turn: the user asked to stop after this checkpoint.

## Final checkpoint

All build/test work for this checkpoint is finished. Final verified results are the successful legacy-P4 firmware build, 40 passing native assertions, 9 passing Python tests, and successful read-only household metadata access. The saved private report was reclassified locally using the corrected provider/launchability logic; it now contains 17 `allowed_favorites`, with no additional network or speaker mutation calls.

The Tab5 still does not appear on USB. No flash backup, flash write, audible playback test, or UI visual inspection has occurred. No model- or agent-owned background work should continue after this handoff; wait for the user to resume.
