# Handoff: standalone Tab5 Sonos controller

Updated September 26, 2026 (second pass, same day). The device is now flashed, booting, on Wi-Fi, and talking to the real household speakers. The board bring-up blockers are resolved; the unproven product milestone is now audible playback.

## Session 3: review fixes (not yet flashed)

Code review changes, built and host-tested but **not yet run on the Tab5**:

- **Speaker fallback.** Every visible room IP from the last good topology is saved to NVS (`speakers` key). `refresh()` tries the current seed, the manual seed, then each saved IP (each behind a bounded 1.5 s TCP check), and only then SSDP. A stale or powered-off seed no longer strands the controller.
- **SSDP** now listens against one 4 s deadline with `MX: 2` and three spaced sends, instead of quitting at the first quiet second. Wi-Fi power save is disabled (`WIFI_PS_NONE`) as a suspected cause of missed replies; revisit with battery/sleep work. Discovery logs the time and number of sends it took.
- **TCP reachability probe** now runs only after a failed SOAP call (previously before every call, ~7 extra connects per 4 s poll), and uses a non-blocking connect so it is actually bounded.
- **SOAP timeout** reduced from 30 s to 5 s. `esp_http_client` applies it per connect/read, not per transfer.
- **Wi-Fi reconnect** moved off the event loop into a task with 1 s→60 s exponential backoff. A boot-time connect that fails (router down) now recovers, and the worker loads the catalog once Wi-Fi returns (retried at most every 30 s).
- **NVS init failure** now boots the UI without persistence and says so, instead of a black screen.
- **Favorite playback replaces the queue** (`RemoveAllTracksFromQueue` first), per user decision.
- **`apply_area`** no longer re-sends a join to rooms already in the kept group.
- **Tests:** 67 native assertions (was 40), adding join/ungroup, saved areas against a simulated household, group volume, radio playback and `state()`.

To verify on hardware: cold boot with the `seed` key cleared, SSDP timing in the log, Wi-Fi recovery after an access-point restart, replace-queue behavior, and saved areas.

## Session 2: hardware bring-up (what changed)

The Tab5 appeared on USB as `/dev/cu.usbmodem2101` once the user attached it with a data cable.

**Flash safety.** P4 revision is **v1.3**, so the default legacy build is correct and `sdkconfig.rev3` must **not** be used on this unit. A full 16 MB factory backup was taken before the first write, at `artifacts/tab5-original.bin` (`sha256 a260eda93e4e5ddc1d3abddd361b1eb326c07d4248b67d1b493feba057fde231`). Factory `human_face_det` and `storage` SPIFFS partitions were never in any erase window.

**Boot loop, diagnosed and fixed.** `bsp_display_start()` hard-asserted and rebooted:

```
E M5Stack Tab5: Unsupported board version!
assert failed: bsp_get_board_version bsp_display.c:189 (NULL)
```

A purpose-built I2C scan diagnostic (`CONFIG_TAB5_I2C_SCAN`, `firmware/sdkconfig.scan`) showed the touch controller *is* present at `0x55` and the bus is healthy, but it drops off I2C around the reset release and returns. Root cause is an **ordering** bug, not a timing one: the BSP probes for the panel before releasing the shared TDDI reset. On ST712x units the device at `0x55` *is* the TDDI, so it cannot ACK while held in reset. `BSP_LCD_EN` is a misnomer for that pin — it is `LCD_RST`.

Fixed in `main.cpp` with `await_touch_controller()`, which releases the resets and then polls for a genuine ACK (up to ~2 s) instead of guessing a fixed delay. It now answers on attempt 1, so it costs nothing. A hard-coded 1500 ms delay was tried first and also worked, but was arbitrary — no published settle time exists.

Upstream: espressif/esp-bsp [issue #829](https://github.com/espressif/esp-bsp/issues/829) and [PR #830](https://github.com/espressif/esp-bsp/pull/830) (one-line ordering fix, unmerged). 1.3.1 is still the latest release. Note `CONFIG_BSP_ERROR_CHECK=n` does **not** help: that branch is a raw libc `assert(NULL)`, so the fix must land before `bsp_display_start()`. The research suggests issue #829 may be the user's own filing — worth confirming.

**Panel identified: ST7121** (`Discovered board version 3 (LCD ST7121, Touch ST712x, FW 1)`), the post-2026-04-28 revision. Detected by the firmware, not read from a label. M5Stack documents three revisions: ILI9881C+GT911, ST7123, ST7121.

**Wi-Fi connected.** Credentials were written directly into the device's NVS partition (`controller` namespace, keys `ssid`/`password`/`seed`) using `nvs_partition_gen.py`, and the scratch CSV was deleted immediately. **No credential was ever compiled into a firmware binary.** The user was asked not to paste passwords into chat and did anyway; the value is in the session transcript and should be considered exposed. The app auto-connects on boot when `ssid` is non-empty.

**SDIO throughput bug, found by measurement.** The original `sdkconfig.defaults` pinned the C6 link to **25 MHz with receive streaming disabled**. The component's own defaults are 40 MHz plus streaming mode, and its Kconfig help states P4-as-host is `<= 40MHz`. That over-conservatism was silently truncating SOAP replies — speakers returned `200 OK` and the transfer died partway:

```
ESP_ERR_HTTP_INCOMPLETE_DATA (received 1440 bytes, status 200)
```

Restoring the vendor defaults fixed it completely. This would have been misdiagnosed as flaky Wi-Fi without the byte-count logging.

**Diagnostics added (keep these).** `catch` blocks now log `e.what()` instead of only painting it on screen — the first failure was unreadable without this. `speaker_reachable()` does a raw-socket probe after a failed SOAP call (before every call until session 3), which is what separated "network cannot reach the speaker" from "HTTP client misbehaved". SOAP failures now log action, error, bytes received, and HTTP status.

## Validation status after session 2

- P4 revision v1.3 confirmed on real hardware; factory flash backed up and verified.
- Cold boot to LVGL UI: **passed**, no assert, single boot, backlight on.
- Touch detection and input: **passed** (user confirmed landscape, legible, touch registers).
- Panel revision auto-detected as ST7121: **passed**.
- Wi-Fi association and DHCP on the C6: **passed**.
- SSDP auto-discovery: **intermittent** — found a speaker on one boot, none on the next. Manual-IP path works reliably.
- SOAP against a real speaker (`ListAvailableServices`, topology, favorites): **passed** with zero errors over a 110 s run at the raised timeout.
- Audible playback: **passed.** User confirmed audio plays, pauses, and responds to volume up/down on a real Apple Music favorite. This retires the central architectural risk in `PLAN.md` — Sonos does play Apple Music content from a preserved favorite URI, so the favorites-first scope is viable.
- Room/group volume, grouping/ungrouping, saved areas, queue viewing, external changes from the official app, and the Sonos Radio favorite ("Set The Table"): **still not performed.**

## Open items

- **SSDP discovery was unreliable.** Session 3 changed the listen window and power save and added a saved-IP fallback; hardware confirmation is pending.
- The seeded `seed` IP is still present in device NVS and should be cleared once discovery is fixed.
- SOAP `timeout_ms` is 5 s per network operation (session 3); confirm on hardware that large favorites/topology replies stay comfortably inside it.
- Artwork is parsed but not rendered. Queue browser/editing, battery/charging, dim/sleep/wake, OTA signing/rollback, and soak testing remain.
- Wi-Fi credentials live in ordinary NVS. Production credential protection remains.
- State is polled every four seconds, not event-subscribed. Reconnect/IP-change recovery needs hardening.
- A failed action is not retried; pending actions are dropped on error, and partial queue/grouping operations may already have happened.
- Address `0x28` appears on the I²C bus but is in no official M5Stack I²C map. Unexplained; possibly worth reporting upstream.
- The `CONFIG_TAB5_I2C_SCAN` diagnostic is retained; the `lvgl_port_init` and per-step probing it also does are what isolated the ordering bug.

## User decisions and instructions

- Build a dedicated Sonos controller on an M5Stack Tab5. It should boot into this one purpose.
- Selected architecture: **standalone, favorites-first**, with no always-on helper required. Full Apple Music library browsing is not required for the initial product.
- Only Apple Music and, if workable, Sonos Radio should be launchable. Do not remove other services from the user's Sonos account.
- Control rooms, grouping, saved areas, playback and room/group volume.
- Only used on the same network as the speakers. This Mac is on that network.
- User has the Tab5, connected by USB data cable as `/dev/cu.usbmodem2101`. Hardware question is now answered by the firmware: this unit is an **ST7121** panel revision.
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

The project was an empty directory before this work. There was a single initial commit (`7ae7fcc`) covering the prototype; an earlier version of this handoff incorrectly stated no repository or commits existed. No PRs or deployments were made.

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

The BSP supports ILI9881C/GT911, ST7123 and ST7121 through touch-controller detection, despite incomplete registry table wording. LCD's native resolution is 720×1280; the application requests 90-degree rotation for landscape. Touch alignment and visual layout are now **hardware-verified** (user confirmed landscape, legible, touch registering), though the type is small.

C6 power: `bsp_feature_enable(BSP_FEATURE_WIFI, true)` before remote Wi-Fi initialization. Hosted SDIO pins: reset 15, CLK 12, CMD 13, D0 11, D1 10, D2 9, D3 8. Host clock is **40 MHz with receive streaming mode enabled** — these are the component's own defaults and must not be reduced. An earlier 25 MHz / `SDIO_OPTIMIZATION_RX_NONE` setting silently truncated SOAP responses; see session 2 above. Do not overwrite the factory C6 firmware routinely.

**P4 chip revision is a mandatory pre-flash check.** IDF 5.5.3 defaults to revision 3.1, which cannot run on earlier chips. The default build deliberately targets P4 **0.x/1.x**, using `ESP32P4_SELECTS_REV_LESS_V3=y` and minimum revision 0.0. This unit is **v1.3**, verified on hardware, so the default build is correct. Revision 3.x needs a separate build using `sdkconfig.rev3`. Chip revision and LCD driver revision are different facts.

The current default firmware build produces `firmware/build/sonos_controller.bin`, size `0x19be60` bytes (about 1.62 MiB), with 77% of the 7 MiB application partition free. Associated bootloader, partition table and flash arguments are in the same build directory. This is a prototype factory partition layout, without OTA slots. A separate scan build lives in `firmware/build-scan/` via `firmware/sdkconfig.scan`.

Note: changing `sdkconfig.defaults` requires deleting `firmware/sdkconfig` so the values regenerate; the cached file wins otherwise. This is how the SDIO clock fix was applied.

Build log: `artifacts/firmware-build.log`. Some local incremental builds can run sandboxed, but CMake reconfiguration uses `psutil` process inspection; macOS sandbox denies that. Run `bash tools/idf.sh build` with tool escalation if it fails on `sysctl()` in Component Manager. This was a sandbox restriction, not a firmware error. Network access is also needed when resolving components for the first time.

## Validation status

- ESP-IDF cross-compilation: **passed**, including application, bootloader and partition-size check for the default legacy P4 target.
- Live read-only Sonos discovery/metadata: **passed**.
- Native C++ tests: **passed, 67 assertions** (session 3), via `bash tools/test_core.sh`.
- Python probe tests: **passed, 9 tests**, via `python3 -m unittest discover -s tests -p 'test_*.py'`.
- Tab5 flash, cold boot, touch, Wi-Fi association, and SOAP exchange with a real speaker: **passed** (see session 2).
- Audible playback and grouping/volume mutations: **not performed**. This is the remaining unproven milestone.

## Current limitations and review points

- This is a bring-up prototype, not a finished appliance. Artwork is parsed but not rendered. Queue browser/editing, battery/charging management, dim/sleep/wake, OTA signing/rollback, and soak testing remain.
- Wi-Fi credentials live in ordinary NVS for now. Source/report secrets are excluded from Git; production credential protection remains.
- State is polled every four seconds, not event-subscribed. Reconnect/IP-change recovery needs hardening. If an existing seed IP becomes stale, discovery/manual IP refresh may be necessary.
- Playback targets the selected room's current coordinator, resolved fresh. Favorite playback clears the queue, adds the favorite, and plays from its first track. Radio uses preserved URI/metadata.
- A failed action is not retried automatically; pending queued actions are dropped on error. Partial queue/grouping operations can still have happened before a timeout. The next step should verify actual state before another mutation.
- Areas store stable room IDs. The app saves the selected room's current group; applying an area verifies all saved rooms exist before grouping and excludes outsiders from the restored group. Hardware tests must confirm expected coordinator/music behavior.
- Source filtering fails closed. Unknown/conflicting identities and non-playable promotional favorites are excluded. The controller can display neutral state and stop/adjust volume for content started by another app.
- A computer-based probe or successful build does not establish that favorite playback works on the physical Tab5.

## Exact next steps when the user resumes

1. Read this handoff. Inspect current files before changing anything; agents share this directory.
2. **Finish the playback validation matrix.** Apple Music favorites are proven; still untested are the Sonos Radio favorite ("Set The Table"), room/group volume, grouping/ungrouping, saved areas, queue viewing, and whether external changes made in the official app are reflected. Test every speaker model actually present, not just the one used so far.
3. **Confirm SSDP auto-discovery on hardware** after the session 3 changes. It was intermittent. Playback currently depends on a seeded manual IP, so the product does not yet work from a cold boot with no stored address; that address also goes stale if the router reassigns it. Clear the `seed` key from NVS once discovery is reliable.
4. Fix any board/protocol issues observed before expanding UI/features. Then finish the appliance work listed above.

Commands from project root:

```sh
python3 -m unittest discover -s tests -p 'test_*.py'
bash tools/test_core.sh
bash tools/idf.sh build
```

Flash and capture logs (this unit is P4 v1.3, so use the default build):

```sh
cd firmware/build
python -m esptool --chip esp32p4 --port /dev/cu.usbmodem2101 write_flash \
  --flash_mode dio --flash_freq 40m --flash_size 16MB --verify \
  0x2000 bootloader/bootloader.bin 0x10000 sonos_controller.bin 0x8000 partition_table/partition-table.bin
```

esptool 4.12 wants the flash-mode flags on the `write_flash` subcommand, not before it.

The I2C scan diagnostic builds separately and does not touch the default build:

```sh
bash tools/idf.sh -B build-scan -D SDKCONFIG=sdkconfig.scan.local -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.scan' build
```

For a verified P4 3.x chip (not this unit):

```sh
bash tools/idf.sh -B build-rev3 -D SDKCONFIG=sdkconfig.rev3.local -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.rev3' build
```

## Final checkpoint

Verified results: the legacy-P4 firmware build, 40 passing native assertions, 9 passing Python tests, read-only household metadata access, and — as of session 2 — a verified factory flash backup, a clean single-boot to the LVGL UI on real hardware, ST7121 panel detection, Wi-Fi association with a DHCP lease, a SOAP exchange with a real speaker at zero errors, and **audible playback of an Apple Music favorite with working transport and volume.**

That last item is the significant one. `PLAN.md` selected the favorites-first standalone architecture on the condition that Apple Music playback could be proven, and warned against assuming any Apple Music item is directly playable by Sonos. It is proven for favorites. The saved private report contains 17 `allowed_favorites`.

Still unproven: the Sonos Radio favorite, grouping, saved areas, queue viewing, reflection of external changes, and every speaker model in the household. Also note all playback so far was driven by a **seeded manual IP**; SSDP discovery remains unreliable, so the product does not yet work from a cold boot with no stored address.

A backup of the device's original flash exists at `artifacts/tab5-original.bin` and should be preserved. To return the Tab5 to factory firmware, write that image back and reset.

No model- or agent-owned background work should continue after this handoff; wait for the user to resume.
