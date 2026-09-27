# Local Sonos probe

`tools/sonos_probe.py` is a read-only Python 3.9+ diagnostic for the
favorites-first Tab5 proof. It uses the Python standard library, discovers a
speaker with SSDP, reads its device description and group topology, requests
the installed music-service descriptors, and pages through the `FV:2` Sonos
Favorites container. It does not issue playback, queue, volume, grouping, or
other state-changing SOAP actions.

Run it from the project root:

```sh
python3 tools/sonos_probe.py
```

If multicast discovery is blocked, pass a speaker address:

```sh
python3 tools/sonos_probe.py --speaker 192.168.1.42
```

To retain complete item URIs and DIDL metadata locally for protocol analysis:

```sh
python3 tools/sonos_probe.py --report artifacts/sonos-probe.json
```

Treat that report as private household data. Favorite names, artwork and
playable service URIs can identify listening habits or expose account/service
identifiers. Keep it local and remove it when it is no longer needed. The
report includes raw provider descriptors and raw DIDL item XML so an unknown
provider mapping can be diagnosed without silently guessing from a title.

Provider classification is fail-closed. A favorite is offered only when a
service identifier carried in its DIDL metadata matches a `ListAvailableServices`
descriptor whose name is exactly Apple Music or Sonos Radio (case-insensitive).
An item with no explicit match is marked `unmapped`; title, artwork, URI text,
and similarity of names are not used to infer its provider. This behavior can
be extended after the actual household's metadata establishes a reliable
identifier relationship. Provider identity and playability are reported
separately: a favorite needs a nonempty resource URI to appear in
`allowed_favorites`; service-backed promotional entries without one remain in
the full inventory but have `queueable: false`.

The script intentionally tolerates partial read failures: discovery results
and any successful sections are reported alongside an `errors` object. A
nonzero exit status means no speaker description was available. A successful
command does not imply every service endpoint succeeded; check `errors`,
`available_services`, and the favorite `provider_status` fields.

The included standard-library tests can be run with:

```sh
python3 -m unittest discover -s tests -v
```
