# obs-shoutcast

An OBS Studio plugin that streams **audio-only MP3** to Icecast / Liquidsoap
and SHOUTcast v1 servers, with a native **Radio.co** dock for connecting,
updating the now-playing line, and setting the station name.

OBS cannot do this on its own: its Custom Output (FFmpeg) creates a video
stream whenever the container has a default video codec — and the `mp3`
muxer's is `png`, so it would PNG-encode the whole canvas — while FFmpeg's
`icecast://` protocol refuses the bare `/` mount that Liquidsoap harbors such
as Radio.co use. See [`docs/FINDINGS.md`](docs/FINDINGS.md) for the evidence.

This replaces [butt](https://danielnoethen.de/butt/) for a station that also
streams video, letting one OBS instance drive both an audio broadcast and a
video platform at the same time.

---

## Requirements

| | |
|---|---|
| macOS | 13.0 or newer |
| Hardware | Apple Silicon (arm64) |
| OBS Studio | 32.2.x — must bundle libavcodec major **62** |

macOS 13 is the floor because OBS's own bundled Qt frameworks are built for it,
not an arbitrary choice. The plugin ships no FFmpeg or LAME of its own; it uses
the copies already inside `OBS.app`, which is why the OBS major matters.

---

## Install

```bash
mkdir -p ~/Library/Application\ Support/obs-studio/plugins

unzip -o ~/Downloads/obs-shoutcast-1.0.0-arm64.zip \
      -d ~/Library/Application\ Support/obs-studio/plugins/

xattr -dr com.apple.quarantine \
      ~/Library/Application\ Support/obs-studio/plugins/obs-shoutcast.plugin
```

Then relaunch OBS.

**The `xattr` step is not optional.** Anything that arrives by browser,
AirDrop, or email is quarantined, and Gatekeeper silently blocks an
ad-hoc-signed plugin until the flag is cleared. A plugin that "doesn't show up"
is almost always this.

---

## Usage

Open **View → Docks → Radio.co**.

| Field | Notes |
|---|---|
| Protocol | `SHOUTcast v1 (legacy ICY)` for Radio.co; `Icecast (HTTP SOURCE)` for a normal Icecast server |
| Server | Ingest hostname, e.g. `maple.radio.co` |
| Port | The port your host lists, e.g. `4192`. **SHOUTcast v1 streams on port + 1** — the plugin adds it, so enter the number as given |
| Username | `source` (Icecast only; ignored by SHOUTcast v1) |
| Mount | `/` (Icecast only; ignored by SHOUTcast v1) |
| Password | The broadcast password from your dashboard. SHOUTcast v1 wants the short token; Icecast wants the long base64 blob |
| Station | Sent as `ice-name` when connecting |
| Bitrate | 64 – 320 kbps |

Press **Connect**. The status line shows `Idle`, `Connecting…`,
`Reconnecting…`, `● Live hh:mm:ss`, or the server's error text. Settings
persist across restarts, and auto-reconnect is on.

**Now Playing** is a single free-text field. Type the whole line, press
**Update**, and it reaches listeners in roughly 15–20 seconds without
interrupting audio, appearing exactly as typed. There is deliberately no
artist/title split: the server would rejoin the pair as `"artist - title"` and
reformat what you wrote.

**Station name** is editable at any time, but it travels in the connection
handshake and cannot be changed on a live connection. Editing it while
connected enables **Apply Name**, which reconnects to apply it — a brief
dropout. When not connected it is simply picked up on the next Connect. If a
dropout is unacceptable, change the name in the Radio.co dashboard instead;
that is what listeners actually see.

**Connect with OBS "Start Streaming"** (off by default) ties the audio feed to
OBS's own stream button, so one click goes live on both the video platform and
the radio. A feed you connected manually is never torn down by stopping the
video stream.

---

## Running alongside a video stream

Radio.co runs as its own output on the dock. The video platform (Kick,
Twitch, YouTube…) goes in **Settings → Stream** and is driven by OBS's
**Start Streaming** button.

The built-in Stream output, the built-in Recording output, and this plugin's
output are three independent outputs and run concurrently. Radio.co cannot be
put in the Settings → Stream slot — that slot holds exactly one service, and
it is not an audio-only one.

---

## Build from source

```bash
brew install qt simde

cd obs-shoutcast
cmake -S . -B build
cmake --build build

codesign --force --sign - --identifier com.obsproject.obs-shoutcast \
         --timestamp=none build/obs-shoutcast.plugin

ditto -c -k --keepParent build/obs-shoutcast.plugin \
      obs-shoutcast-1.0.0-arm64.zip
```

`simde` is needed because `libobs`'s SSE-intrinsics header pulls in
`<simde/x86/sse2.h>` on arm64, and simde is a git submodule that the OBS
release tarball does not contain.

Homebrew provides headers and `moc` only — Qt, FFmpeg, libobs and the frontend
API are all linked out of `OBS.app` through `@rpath`, so nothing is vendored
and no Homebrew paths end up in the bundle. `otool -L` on the result should
show only `@rpath/…`, `/usr/lib/libc++.1.dylib` and `/usr/lib/libSystem.B.dylib`.

**Delete `build/` whenever the target OBS version changes.** The OBS headers
are fetched once and cached under `build/_deps`, so an incremental reconfigure
keeps using the old ones.

---

## Troubleshooting

| Symptom | Cause |
|---|---|
| Plugin never appears in OBS | Quarantine flag not cleared — re-run the `xattr` command. Failing that, the bundle's minimum macOS is above the host's; check `vtool -show-build` against `sw_vers`. |
| `Invalid username or password` | HTTP 401. Username is `source`; re-copy the broadcast password from the dashboard. |
| `Mount point … not found on server` | HTTP 404 — wrong mount. Radio.co uses the bare `/`; anything else 404s. |
| `Mount point … is already in use` | HTTP 403 — another source (butt, a phone app, another OBS) is still connected. Disconnect it first. |
| Connects, shows `● Live`, then `send failed: Resource temporarily unavailable` after ~20 s | **Wrong endpoint.** The server authenticated you but is not consuming audio, so the socket buffer fills and `send()` times out. Radio.co's `.dj.radio.co` Icecast harbor does exactly this. Switch to SHOUTcast v1 against the host in your dashboard (`maple.radio.co`-style). Details in [`docs/FINDINGS.md`](docs/FINDINGS.md) §3.6. |
| SHOUTcast v1 never completes the handshake | You are on the admin port. SHOUTcast v1 sources connect on **port + 1**; the plugin applies this automatically, so enter the base port (e.g. `4192`), not `4193`. |
| Connects and shows `● Live`, but the station stays `automated` | The stream is arriving but the station is not accepting it: "Live Anytime" is disabled, or no event is scheduled in the dashboard. butt reports the same condition at `src/shoutcast.cpp:189-191`. |
| Metadata returns `200` but the title never changes | Titles only land while the source is actually live — which makes this a handy liveness check. Confirm `source.type` is `live` first. |
| Build fails with `FFmpeg major mismatch` | Homebrew's FFmpeg major differs from the one in `OBS.app`. Install the matching formula, or upgrade OBS. |
| Build fails with `simde headers not found` | `brew install simde`. |

Live station status, useful for all of the above:

```bash
curl -s https://public.radio.co/stations/<station-id>/status \
  | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d["source"]["type"], "|", d["current_track"]["title"])'
```

---

## Configuration file

```
~/Library/Application Support/obs-studio/plugin_config/obs-shoutcast/settings.json
```

Dock settings are written here on every change. **The password is stored in
plaintext** — the same exposure butt had in `~/.buttrc`. There is no keychain
integration.

---

## Documentation

[`docs/FINDINGS.md`](docs/FINDINGS.md) — the wire protocol byte for byte, what
was measured against the live endpoint, the approaches that do **not** work and
why, the build/ABI constraints, and the verification log.
