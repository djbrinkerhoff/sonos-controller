# Dedicated Sonos controller on M5Stack Tab5

Status: standalone favorites-first architecture selected September 26, 2026, and validated on the purchased hardware. The firmware runs on the Tab5, joins Wi-Fi, discovers a household speaker, and plays an Apple Music favorite with working transport and volume. The central risk in this plan — that Sonos may not play Apple Music content from a preserved favorite URI — is retired. SSDP auto-discovery is still unreliable, so a stored manual IP is currently required, and the Radio favorite, grouping, saved areas, queue, and external-change reflection remain untested. See HANDOFF.md for current state and README.md for build and recovery instructions. Full-library/helper sections below are retained as future alternatives, not the selected implementation.

## Product and assumptions

Build a device that boots directly into a Sonos controller. Apple Music is the primary source; Sonos Radio is optional. Start music on selected Sonos rooms, manage temporary room groups and saved areas, control playback and volume, and view the queue and artwork. Full Apple Music library/catalog browsing is an optional scope choice; a favorites-first product is acceptable to the user.

Confirmed: the controller will only be used on the same network as the speakers. All speakers use the current Sonos app: Beam (Gen 2), Play:1, Era 100, Era 300, and Beam (Gen 1). The user wants both standalone and helper-based options and is open to favorites-only if the tradeoffs are clear. Personal household use is the planning assumption. Confirm actual room/bonded configurations, Apple Music account/storefront, and any available helper host before implementation. “Library” means the user's Apple Music library; a NAS/local Sonos library would be additional scope. S1 is out of initial scope.

Only Apple Music and Sonos Radio appear as selectable sources. Enforce this in the service as well as the UI, including favorites and playback requests. Unknown sources are not launchable; music started elsewhere can still show neutral now-playing information and stop/volume controls. Do not remove other services from the household's Sonos account.

## Hardware fit

The Tab5 has an ESP32-P4, 32 MB PSRAM, 16 MB flash, a 5-inch 1280×720 touch display, and an ESP32-C6 for 2.4 GHz Wi-Fi. This is a reasonable embedded touch-controller platform. Use native firmware rather than assuming an Android app or desktop browser will run on it. Confirm the purchased battery variant and display/touch revision: M5Stack lists multiple display-controller revisions. [Hardware and revision history](https://shop.m5stack.com/products/m5stack-tab5-iot-development-kit-esp32-p4).

## Architecture options and recommendation

There are two separate choices: where integration code runs, and whether the device offers favorites or full library browsing.

| Option | Favorites and daily controls | Full Apple Music library | Operational tradeoff |
| --- | --- | --- | --- |
| Standalone Tab5 | Recommended v1 candidate, subject to real-system validation | Highest complexity and unproven playback coverage | One device; all protocol fixes ship as firmware |
| Tab5 + local helper | Fastest integration prototype using existing libraries | Recommended foundation if full browsing is selected | Requires an always-on Pi/NAS/PC; integration fixes ship independently |

Current recommendation: prove favorites-based control on the actual system, then build a standalone favorites-first Tab5 if those shortcuts cover daily listening. A computer-based test harness is useful for this proof and does not imply a permanent helper. Keep the firmware UI behind a controller interface so a helper can be added later without redesigning the screens. If full browsing becomes a priority, select the helper from the start.

### Favorites-only scope and tradeoffs

Use Sonos Favorites saved in the official Sonos app, filtered to Apple Music and Sonos Radio. This is different from Apple Music's own Favorites or Favorite Songs library collection. Test favorite albums, playlists, individual tracks, and stations as supported by the household. Existing Sonos account linking remains in the official app; this variant should not need a separate Apple Music developer integration.

Favorites-only still includes room selection, grouping, saved areas, now-playing artwork, volume/mute, play/pause, and skips where supported. Queue viewing and edits are separate capabilities to validate and can be added without full provider browsing. The loss is music discovery: no arbitrary Apple Music catalog search, complete artist/album/library browser, or playlist editing on the device. Add and organize favorite shortcuts in the Sonos app. A favorite playlist should be loaded through its service-backed Sonos metadata; verify that later playlist changes appear rather than copying a frozen track list or promising refresh behavior.

This is an accepted possible final scope, not merely an interim prototype. Sonos Radio favorites remain conditional on a successful playback test; full Radio browsing is optional in either architecture. Speaker access to online services is still needed even though control is local. Favorites do not make Apple Music offline.

### Standalone implementation

The Tab5 runs both the LVGL UI and a small local Sonos client: SSDP discovery/manual-IP fallback, required UPnP/SOAP operations, favorites metadata parsing, topology/coordinator handling, and state updates via subscriptions with periodic reconciliation. Preserve each favorite's playable URI and metadata; do not guess Apple Music URLs. Reuse the vendor network stack and keep XML/network work off the UI thread. Existing Python SoCo code can inform tests but does not run directly in native ESP-IDF firmware.

Prototype on the purchased board early to measure discovery, XML parsing, artwork memory, event renewals, Wi-Fi recovery, and responsiveness. Same-network placement simplifies reachability but Wi-Fi client isolation or multicast filtering can still prevent discovery. An asleep or powered-off Tab5 should not stop speaker playback. On wake, fetch fresh topology and state before accepting commands.

### Helper implementation

```text
Tab5 native touch UI
    | local authenticated API + live state events
Always-on local helper (Pi / NAS / mini PC)
    |-- Apple Music API: account library, catalog, search, artwork
    |-- Sonos adapter: discovery, playback, queue, rooms, volume
    |-- Sonos Radio adapter: validated favorites; browsing if proven
    |-- saved areas, credentials, cache, setup page
Sonos speakers retrieve and play the music themselves
```

The helper carries service authentication and integration complexity. A Sonos or Apple integration fix can then ship without reflashing the touchscreen. Neither the Tab5 nor helper needs to decode or relay Apple Music audio. With this architecture the helper must be running to issue controls; stopping it should not interrupt music already playing on Sonos.

Adding full Apple Music browsing to the standalone variant would also require provider authentication, content resolution, and caching, plus a phone/computer for browser sign-in. It does not remove the Apple-to-Sonos playback uncertainty. That additional complexity is why the helper is preferred for the full-library variant.

## Integration decisions and limits

### Apple Music full-library variant

Use Apple's documented Apple Music API for browsing and search. MusicKit authorization runs in a phone/computer browser through a small setup page; the Tab5 displays a pairing link/QR code. The helper holds the developer signing key and user authorization token. This path requires Apple Developer Program access and MusicKit credentials, plus an active subscription and Apple Music linked in the official Sonos app. [Apple Music API](https://developer.apple.com/documentation/AppleMusicAPI), [MusicKit](https://developer.apple.com/musickit/), [developer tokens](https://developer.apple.com/documentation/applemusicapi/generating-developer-tokens).

Do not assume an Apple Music item ID or URL is directly playable by Sonos. Build an explicit resolution layer from Apple library items to catalog identities/share links, then to a Sonos queue item. SoCo's share-link implementation is a candidate, not a compatibility guarantee. Private playlists may require paginated track expansion; uploaded or unmatched tracks may have no usable catalog equivalent. Report unsupported items clearly and never silently substitute a different recording. [SoCo share-link implementation](https://github.com/SoCo/SoCo/blob/master/soco/plugins/sharelink.py), [SoCo CLI usage](https://github.com/avantrec/soco-cli).

Do not base the plan on SoCo providing complete Apple Music browsing: its current README explicitly warns about authentication issues affecting Apple Music. [SoCo warning](https://github.com/SoCo/SoCo/blob/master/README.rst).

### Sonos control

Prototype local control with Python and SoCo behind our own small adapter interface. Validate discovery, transport, queue access, volume, grouping, and events against the actual household. Local Sonos integration is community-supported and can break after speaker updates; keep it replaceable.

The official cloud Control API is an alternative for documented control operations and favorites. It brings Sonos authorization and cloud dependencies. The reviewed documentation does not establish a general Apple Music/Sonos Radio catalog-browsing API for third-party controllers. SMAPI documentation describes the interface music-service providers implement for Sonos; it is not blanket access to existing providers' authenticated catalogs. [Control API](https://docs.sonos.com/reference/about-control-api), [favorites playback](https://docs.sonos.com/reference/favorites-loadfavorite-groupid), [SMAPI](https://docs.sonos.com/docs/smapi).

### Sonos Radio

First test playback of Sonos Radio stations saved as Sonos Favorites. Preserve service identity so only allowed sources are offered. Test a free station and any HD content the user subscribes to; entitlements and available transport controls can differ. Full station browsing is optional and needs its own successful authentication/browse/play experiment. Do not substitute another radio provider or assume a generic stream URL exposes Sonos Radio.

### Rooms and areas

Treat existing Sonos rooms and bonded stereo/home-theater setups as the control units, with current group topology supplied by Sonos. A custom area such as “Downstairs” is a saved set of room IDs that the helper applies as a temporary group. Show the destination and group changes before starting playback. Re-resolve group coordinators after changes; do not treat transient group IDs as permanent identities. Subs and surround speakers are not independent music destinations.

## Firmware and optional companion software

Firmware: C/C++ with ESP-IDF, the M5Stack Tab5 board support package, and LVGL. Start from the vendor's working board configuration and pin compatible IDF/BSP/LVGL/C6 firmware versions. M5Stack's factory build guide currently recommends IDF 5.4.2; verify support for the purchased display revision before locking versions. Use the vendor-supported C6 connectivity path. [Tab5 documentation](https://docs.m5stack.com/en/core/Tab5), [factory build guide](https://docs.m5stack.com/en/esp_idf/m5tab5/userdemo).

Firmware responsibilities: display/touch, Wi-Fi provisioning, helper discovery/pairing, UI state, reconnect, bounded artwork cache, battery status, dim/sleep/wake, and signed OTA with rollback. Use paginated lists and appropriately sized images; never load the entire library or full-resolution artwork into RAM. Keep network operations off the UI loop. Validate flash partitions and update size against the 16 MB flash budget. Leave unused audio/camera features inactive.

Helper responsibilities: Sonos adapter, Apple Music adapter, optional Radio adapter, API/events, token renewal/re-authorization, cache, saved areas, and a minimal browser setup/diagnostics page. Proposed stack: Python, SoCo, an HTTP/WebSocket service, and SQLite for non-secret state. Store secrets separately with restricted access; require device pairing, protect setup/auth traffic, redact tokens from logs, and keep control endpoints LAN-only. Run natively during development, then package for the selected always-on host. Validate multicast discovery and inbound speaker events on that host; Docker networking is not interchangeable across platforms.

Suggested project structure: `firmware/`, `bridge/`, `setup-ui/`, `contracts/`, `tests/`, and `docs/`. The setup page supports the appliance; it is not a separate everyday music app.

## Touchscreen experience

- Now Playing: artwork, track/artist, destination, play/pause, skip, volume, and queue. Show seek/shuffle/repeat only when the current content supports them.
- Library: Apple Music playlists, albums, artists, songs, and recently added content; paginated and cached.
- Search: Apple Music catalog and library where supported, with a touch keyboard.
- Rooms: room selection, group/ungroup, individual and group volume, mute, saved areas.
- Radio/Favorites: only validated Apple Music and Sonos Radio items.
- Settings: Wi-Fi, pairing, brightness/sleep, account status, diagnostics, and updates.

Keep the current destination and playback controls accessible while browsing. Reflect changes made in the official Sonos app. Distinguish pending actions from confirmed speaker state and show recoverable connection/account errors.

## Delivery sequence and acceptance gates

For the recommended favorites-first standalone build, use this sequence:

1. Prove on the household: enumerate rooms and favorites; play Apple Music track/album/playlist favorites and a Sonos Radio favorite; check metadata, volume, group/ungroup, and state changes made from the Sonos app. Confirm provider filtering and behavior of edited playlists. Record any unavailable favorite types.
2. Bring up the actual Tab5 display/touch/Wi-Fi with ESP-IDF and LVGL, then implement the limited local Sonos client. Deliver select-room, favorite-playback, now-playing, and volume as a complete vertical slice.
3. Add Favorites, Now Playing, Rooms, saved areas, and supported queue controls. Persist preferences and stable room identifiers. Test every listed speaker model and bonded/grouped configurations actually present.
4. Finish provisioning, reconnect, sleep/wake, battery UI, signed OTA/rollback, flash/recovery instructions, and a multi-day soak test. Verify music continues while the controller sleeps and external changes appear after wake.

Acceptance for this variant: from cold boot, choose an allowed favorite, select a room or saved area, play it, adjust room/group volume, and reflect external changes. No permanent helper or Apple developer credentials are needed. Radio is enabled only for tested content; failures and unsupported controls are explicit. Favorites-only is sufficient if the user selects this scope.

If full browsing and the helper architecture are selected, use the expanded sequence below:

1. **Prove the difficult integration on a computer first.** Inventory the system; discover rooms; authorize Apple Music; browse a real user playlist; resolve and play a track, album, public playlist, and private library playlist on an actual Sonos room. Test regional/unavailable items, unmatched uploads, pagination, correct recording selection, and re-authorization. Test a saved Sonos Radio station. Record supported and unsupported cases. No full-browsing commitment until browse-to-play succeeds. Favorites-only is an interim prototype, not completion of the requested product.
2. **Build the local service.** Add the versioned API and event model, provider allowlist, discovery/recovery, now-playing state, volume/mute, queue actions, grouping, and saved areas. Validate simultaneous use with the Sonos app, coordinator changes, and unavailable rooms. Reconcile state after timeouts rather than blindly replaying non-idempotent queue operations.
3. **Bring up the Tab5.** Verify the actual display/touch variant, Wi-Fi, battery readings, and sustained memory use. Deliver a real-device vertical slice: select a room, show its track, pause/play, change volume, and recover after Wi-Fi interruption.
4. **Complete browsing and daily controls.** Add the Apple Music library/search screens, artwork, play-now/play-next/add-to-queue, supported queue edits, room grouping, and saved areas. Add Radio favorites and enable full Radio browsing only if proven. Test large libraries without UI stalls.
5. **Make it an appliance.** Finish provisioning and account setup, boot-to-controller, sleep/wake, signed updates/rollback, diagnostics, reconnect behavior, and a multi-day soak test. Package a reproducible firmware build, flash/recovery instructions, helper installation, and a short user guide.

Acceptance: from a cold boot, the user can browse an Apple Music library item, select a room or saved area, play it, adjust group/room volume, and see externally initiated changes. Playback continues while the Tab5 sleeps. Recover from helper restart, Wi-Fi loss, speaker restart, topology changes, and expired authorization without duplicate queue additions. No other provider can be launched through this controller. Unsupported library items and Radio limitations are documented explicitly.

## Decisions before implementation

- Always-on helper versus standalone firmware; available host if using a helper.
- Exact room, stereo-pair, and home-theater configurations; LAN discovery reachability.
- Favorites-only versus full library browsing. Apple Developer/MusicKit credentials are required only for the proposed direct Apple API browsing path.
- Whether uploaded/unmatched Apple library tracks must work, and whether multiple Apple accounts are required.
- Desired saved areas and battery-powered versus primarily docked use.

Recommended first milestone: a favorites-to-Sonos-playback proof using the user's real system, followed by a standalone Tab5 vertical slice. If full browsing is selected, add an Apple Music browse-to-playback proof before investing in that integration or its screens.
