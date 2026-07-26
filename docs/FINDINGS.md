# Engineering findings

Working notes for `obs-shoutcast`: what the wire protocol actually looks like,
what was measured against a real server, and — most importantly — **what does
not work**, so nobody spends a day re-attempting a dead end.

Everything here was observed on this machine or against the live Radio.co
endpoint. Where a number is a measurement it says so.

Last updated: 2026-07-26.

---

## 1. Wire protocol

The plugin speaks two protocols. In both cases **raw MP3 frames follow the
handshake with no container** — no ADTS, no Ogg, no framing of any kind. This
is exactly what butt does: `snd_stream_thread`
(`butt-1.46.0/src/port_audio.cpp:785-905`) encodes with LAME and hands the
buffer straight to a `xc_send` function pointer (`:799-809`) that writes it to
one TCP socket.

### 1.1 Icecast / Liquidsoap harbor — `SOURCE` (default)

```
SOURCE /<mount> HTTP/1.0\r\n
Authorization: Basic <base64(username ":" password)>\r\n
Host: <server>:<port>\r\n
User-Agent: obs-shoutcast/<version>\r\n
Content-Type: audio/mpeg\r\n
ice-name: <station name>\r\n
ice-genre: <genre>\r\n
ice-url: <url>\r\n            <-- omitted entirely when the URL is empty
ice-public: <0|1>\r\n
ice-bitrate: <kbps>\r\n
ice-audio-info: ice-bitrate=<kbps>;ice-channels=<n>;ice-samplerate=<hz>\r\n
\r\n
```

A `200` means connected. Anything else is mapped to a human-readable error:

| Status | Message shown |
|---|---|
| `401` | `Invalid username or password` |
| `403` | `Mount point <mount> is already in use` |
| `404` | `Mount point <mount> not found on server` |
| other | `Server rejected connection:` plus the first response line |

`SOURCE` is used rather than `PUT`. Both authenticate on this harbor; `SOURCE`
with `HTTP/1.0` avoids the `Expect: 100-continue` round trip.

Captured from the real code path (`src/icy-protocol.c`) against a local
listener:

```
SOURCE / HTTP/1.0
Authorization: Basic <redacted>
Host: 127.0.0.1:9099
User-Agent: obs-shoutcast/1.0.0
Content-Type: audio/mpeg
ice-name: Radyo ÖzÜ
ice-genre: Various
ice-public: 0
ice-bitrate: 128
ice-audio-info: ice-bitrate=128;ice-channels=2;ice-samplerate=48000
```

That `Authorization` value decodes to `source:<password>` and is byte-identical
to the header proven against the live server during planning.

### 1.2 SHOUTcast v1 — legacy ICY

Kept as a second protocol option. Send the bare password, wait for a response
containing `OK`, then the `icy-*` headers:

```
<password>\r\n
        -> server replies, must contain "OK"
icy-name:<name>\r\n
icy-genre:<genre>\r\n
icy-url:<url>\r\n
icy-pub:<0|1>\r\n
icy-br:<kbps>\r\n
content-type:audio/mpeg\r\n
\r\n
```

Note butt connects SHOUTcast v1 on **`port + 1`** for audio
(`butt-1.46.0/src/shoutcast.cpp:66-217`), but metadata goes to the **base
port**. This plugin's metadata call follows that and uses `params->port`.

### 1.3 Metadata

Opens a **fresh short-lived TCP connection each time** and closes it
immediately. The audio socket is never touched, so a title update cannot
interrupt or corrupt the stream. butt does the same
(`butt-1.46.0/src/icecast.cpp:502` opens, `:594` closes).

Icecast form — **one free-text `song=` parameter, nothing else**:

```
GET /admin/metadata?mode=updinfo&mount=<mount>&song=<pct-encoded> HTTP/1.0\r\n
Authorization: Basic <base64>\r\n
Host: <server>:<port>\r\n
User-Agent: obs-shoutcast/<version>\r\n
\r\n
```

SHOUTcast v1 form (mirrors `butt-1.46.0/src/shoutcast.cpp:264-267`):

```
GET /admin.cgi?pass=<pct-encoded>&mode=updinfo&song=<pct-encoded>&url= HTTP/1.0\r\n
User-Agent: ShoutcastDSP (Mozilla Compatible)\r\n
Host: <server>:<port>\r\n
\r\n
```

Percent-encoding is byte-wise over UTF-8: unreserved `A-Za-z0-9-_.~` pass
through, every other byte becomes `%XX` in **uppercase** hex. Verified
round-trip:

```
Güneş - Kayıp Şarkı
  -> G%C3%BCne%C5%9F%20-%20Kay%C4%B1p%20%C5%9Eark%C4%B1
  -> Güneş - Kayıp Şarkı          (exact match)
escapes: C3 BC C5 9F 20 20 C4 B1 20 C5 9E C4 B1   (all uppercase, space is %20 not +)
```

No `charset` parameter is sent — plain percent-encoded UTF-8 round-tripped
correctly against the live server. butt's optional one is
`butt-1.46.0/src/icecast.cpp:558-564` if it is ever needed.

Updates are de-duplicated: an identical title applied twice opens only one
socket.

---

## 2. The Radio.co endpoint

Ingest is **`s3ab6bdcb9.dj.radio.co:80`** — the newer "DJ" endpoint. This is
*not* the `maple.radio.co:4192` entry still sitting in `~/.buttrc`, which is
the older SHOUTcast v1 endpoint.

It is a **Liquidsoap source harbor**, not stock Icecast: `GET /` returns
`HTTP/1.0 404 Not found` with `<title>Liquidsoap source harbor</title>`.

**The only mount is the bare `/`.** Probed live:

| Request | Response |
|---|---|
| `SOURCE /` | `401 Unauthorized`, `WWW-Authenticate: Basic realm="Liquidsoap harbor"` |
| `SOURCE /stream` | `404` |
| `SOURCE /live` | `404` |
| `SOURCE /source` | `404` |
| `SOURCE /radio` | `404` |
| `SOURCE /mount` | `404` |
| `SOURCE /s3ab6bdcb9` | `404` |
| `GET /` | `404`, Liquidsoap source harbor page |

A `401` means the mount exists and only the credentials were missing; a `404`
means it does not. That is the quickest way to re-probe if the endpoint ever
moves.

Credentials are username `source` plus the broadcast password from the
Radio.co dashboard. The dashboard displays it URL-encoded (trailing `%3D`);
the harbor accepts either form, so use the decoded `=` form.

The metadata endpoint is a **real handler, not a catch-all** — verified by
probing neighbours: `/nonsense` → `404`, `/admin/bogus` → `400`,
`/admin/metadata` with no query → `400`, `mode=bogus` → `500`.

Live status for checking any of this:
`https://public.radio.co/stations/s3ab6bdcb9/status` → `source.type` is
`automated` or `live`, and `current_track.title` is the now-playing string.

Measured latencies against the live station: `automated` → `live` in **~11-16
s** after connecting, metadata visible in **~18 s**, and back to `automated`
within **~30 s** of disconnecting. `mount` is accepted but not required on the
metadata call; it is sent anyway to match butt.

Metadata sent while not streaming returns `200` and does nothing — the source
must be live for a title to land.

---

## 3. Negative results

The most valuable section. Each of these looked reasonable and is wrong.

### 3.1 FFmpeg's `icecast://` protocol cannot be used

It refuses a bare `/` mount outright:

```
No mountpoint (path) specified!
```

Radio.co's harbor has *only* the bare `/` mount (§2), so the two are
fundamentally incompatible. This rules out the obvious "just point ffmpeg at
it" approach.

### 3.2 OBS's built-in Custom Output (FFmpeg) cannot carry this

Two independent blockers:

- The `mp3` muxer's **default video codec is `png`** (`ffmpeg -h muxer=mp3`).
  OBS creates a video stream whenever the container has one, so it would
  PNG-encode the entire canvas alongside the audio.
- That muxer buffers attached-picture packets for a **trailer that never runs
  on a live stream**, so those frames are never flushed.

A WAV-over-TCP → external `ffmpeg` relay was prototyped and did work, but it
means two artifacts and a babysat external process, which defeats the point of
a single installable plugin.

### 3.3 The station name cannot be changed on a live connection

- `ice-name` travels **only in the handshake** and is never re-sent.
- A `name=` metadata parameter returns `200` and **changes nothing** —
  verified live, the title was untouched.
- Radio.co's public status payload has no station-name field at all (only
  `status`, `source`, `current_track`, `history`, `collaborators`, `relays`);
  the name listeners see comes from the dashboard.

So renaming genuinely requires a reconnect. The dock exposes this honestly as
an **Apply Name** button that reconnects, rather than pretending the field is
live-editable.

### 3.4 `artist=` + `title=` work but are deliberately unused

The harbor accepts them and composes the result **server-side** as
`"<artist> - <title>"` — verified live: `artist=BBB%20artist&title=CCC%20title`
came back as `"BBB artist - CCC title"`.

That reformatting is exactly what is not wanted here. The requirement is that
the typed line reaches listeners verbatim, so the plugin sends a single
free-text `song=` and never splits. Do not "improve" this by re-adding the
split.

### 3.5 Radio.co cannot be driven from Settings → Stream

That slot holds exactly one service, and it has to be Kick. An RTMP
multistream plugin does not help either: those create `rtmp_output` targets and
have no audio-only mode, so an HTTP/Icecast audio stream is outside what they
can express.

The plugin therefore runs its own output. The built-in Stream output, the
built-in Recording output, and this `shoutcast_output` are three independent
outputs and run concurrently.

What *is* possible — and is implemented — is subscribing to the frontend's
stream lifecycle events (`OBS_FRONTEND_EVENT_STREAMING_STARTED` / `_STOPPED`),
which is what the optional *Connect with OBS "Start Streaming"* checkbox uses.

---

## 4. Two bugs that only a real run exposed

### 4.1 No audio on the wire

**Symptom.** The handshake succeeded, the log said `[shoutcast] streaming
started`, the dock showed `● Live`, metadata updates landed — and the server
received **0 bytes of audio**. A station would have gone "live" and broadcast
pure silence.

**Cause.** `shoutcast_start()` called `obs_output_begin_data_capture()` without
first calling `obs_output_initialize_encoders()`. Without that call the audio
encoder is never started, so no packets are ever produced and
`encoded_packet` is never invoked. Every in-tree encoded output does both, e.g.
`plugins/obs-outputs/flv-output.c`:

```c
if (!obs_output_can_begin_data_capture(stream->output, 0))
        return false;
if (!obs_output_initialize_encoders(stream->output, 0))
        return false;
```

**Fix.** Both calls now guard `shoutcast_start()` in
`src/shoutcast-output.c`.

**Why it hid.** Nothing upstream fails. The socket connects, the protocol is
correct, the UI is correct. Only counting bytes at the far end reveals it —
which is why the verification harness parses MP3 frames instead of just
checking that a connection happened.

**Measured after the fix** (~16 s of streaming into a local sink):

```
bytes received : 254976
mp3 frames     : 664
first frame hdr: fffb9464     -> MPEG1 Layer III, 128 kbps, 48 kHz
bitrates seen  : [128]
samplerates    : [48000]
```

254,976 bytes over ~16 s is 128 kbps, and 664 frames × 1152 samples ÷ 48000 Hz
= 15.94 s. Both cross-check.

### 4.2 Double free of the dock widget on quit

**Symptom.** Quitting OBS gracefully, the log stopped dead at:

```
Tried to call obs_frontend_remove_event_callback with no callbacks!
Tried to call obs_frontend_remove_dock with no callbacks!
```

No `[obs-shoutcast] plugin unloaded`, no profiler summary, no
`Number of memory leaks:` line — OBS was dying part-way through shutdown. Easy
to miss, because the process still "exits" and nothing is obviously broken
until you compare against a healthy shutdown.

**Cause.** `radioco_dock_free()` was calling `obs_frontend_remove_dock()` and
then `delete dock` from `obs_module_unload`. Both are wrong, for the same
underlying reason: **OBS tears the frontend down before it unloads modules.**

Looking at what registration actually does
(`frontend/OBSStudioAPI.cpp:336-357`):

```cpp
OBSDock *dock = new OBSDock(main);
dock->setWidget((QWidget *)widget);
```

The `OBSDock` is parented to the main window, and `setWidget()` **reparents our
widget into it**. So when the main window is destroyed, Qt destroys the
`OBSDock` and our widget with it. By `obs_module_unload` the pointer is
dangling and `delete` double-frees. The frontend callback list is likewise
already gone, which is what the two warnings were saying.

**Fix.** Two parts:

- The static pointer is a **`QPointer<RadioCoDock>`**, which Qt nulls
  automatically when the widget is destroyed, so the stale pointer can never be
  dereferenced.
- `radioco_dock_free()` no longer calls any frontend API and never deletes the
  widget; all real teardown happens in the `OBS_FRONTEND_EVENT_EXIT` handler,
  which is the last point at which the frontend is still alive. This is exactly
  what `plugins/decklink-output-ui/decklink-ui-main.cpp` does — it stops its
  outputs on `EXIT` and from `obs_module_unload`, and never calls
  `obs_frontend_remove_dock` at all.

**After the fix**, shutdown runs to completion:

```
[obs-shoutcast] plugin unloaded
Number of memory leaks: 0
```

with no `remove_dock with no callbacks` warning. (Two
`remove_event_callback with no callbacks!` lines remain in the log — those come
from other bundled plugins, not this one, and were present before this plugin
was installed.)

**Lesson for both bugs.** Neither is visible from the UI, the log's happy path,
or a code read. One needed byte counting at the far end of the socket; the
other needed a diff against a known-good shutdown log. Verify the ends, not the
middle.

---

## 5. Build and ABI constraints

- **FFmpeg major must match.** The plugin compiles against Homebrew headers but
  links `@rpath/libavcodec.dylib` out of `OBS.app`. If the majors diverge,
  `AVCodecContext`/`AVFrame` offsets disagree and memory is silently corrupted.
  `CMakeLists.txt` asserts this at configure time and fails with instructions.
  Both sides are **62** today (OBS bundles libavcodec 62.28.102 / libavutil
  60.26.102 — FFmpeg 8).
- **LAME is already inside OBS.** OBS's `libavcodec.dylib` is built
  `--enable-libmp3lame` with LAME statically linked (zero external `lame_*`
  symbols), so `avcodec_find_encoder_by_name("libmp3lame")` resolves at
  runtime and the plugin ships no encoder of its own.
- **arm64 only.** `libobs` and `obs-frontend-api.dylib` in OBS.app are
  arm64-only (the bundled FFmpeg and Qt are universal). A universal build
  cannot link them.
- **Deployment target is 13.0, not 11.0.** Measured with `vtool`: OBS.app
  declares `LSMinimumSystemVersion 12.0` and `libobs` / `obs-frontend-api` are
  `minos 12.0`, but the bundled **QtCore/QtGui/QtWidgets are `minos 13.0`**.
  Targeting anything lower only produces `built for newer version 13.0` linker
  warnings and a bundle that still cannot load Qt below 13. `Info.plist.in`
  now inherits `${CMAKE_OSX_DEPLOYMENT_TARGET}` so the two can never drift.
- **`CMAKE_OSX_DEPLOYMENT_TARGET` and `CMAKE_OSX_ARCHITECTURES` must be set
  before `project()`.** `project()` initialises those cache entries and
  configures the toolchain; a later `set(... CACHE ...)` is a silent no-op. Set
  after `project()`, the build inherited the host SDK and stamped `minos 26.0`,
  which would refuse to load on any older Mac.
- **rpath must be relative.** The default baked an absolute
  `/Applications/OBS.app/Contents/Frameworks`. Now
  `@executable_path/../Frameworks`, matching OBS's own plugins.
- **Qt linkage.** Homebrew supplies headers and `moc` only; the libraries
  linked are OBS's own frameworks, which all carry `@rpath/…` install names
  (`otool -D`). Linking Homebrew Qt instead would bake `/opt/homebrew` paths
  into the bundle and break it on any machine without Homebrew. Homebrew Qt is
  6.11.1, an exact match for OBS's bundled Qt — normally the blocker for
  plugin docks, and absent here.
- **simde is required and is not in the release tarball.** On arm64
  `libobs/util/sse-intrin.h` includes `<simde/x86/sse2.h>`. simde is a git
  submodule of the OBS repo, so the GitHub *release tarball* CMake fetches does
  not contain it. `brew install simde` supplies it; CMake resolves the prefix
  and fails with that instruction if missing.
- **`libc++` is an expected new dependency.** Adding the C++ dock introduces
  `/usr/lib/libc++.1.dylib`. That is a system library, present everywhere,
  alongside `/usr/lib/libSystem.B.dylib`. The thing to watch for is
  `/opt/homebrew` — there must be zero such references.

Final binary, verified:

```
arch     : arm64
minos    : 13.0
rpath    : @executable_path/../Frameworks
homebrew : 0 references
codesign : adhoc, Info.plist bound (no "not bound")
```

---

## 6. OBS 32.2.1 API notes

Taken from the real headers at tag `32.2.1`, because several of these changed.

- **The frontend header moved.** `UI/obs-frontend-api/obs-frontend-api.h` is
  gone at 32.2.1; it is `frontend/api/obs-frontend-api.h`. CMake locates it
  with a `GLOB_RECURSE` rather than hardcoding either path, so a future move
  cannot silently break the include.
- **`obs_frontend_add_dock()` (the legacy `void *` form) is absent** from the
  32.2.1 header — removed, not deprecated.
  `obs_frontend_add_dock_by_id(id, title, widget)` is the replacement and is
  the only variant that also creates a Docks-menu entry. It takes a
  **`QWidget*`**, not a `QDockWidget*` (the header says so verbatim), and it
  does **not** take ownership.
- **Register the dock from `obs_module_post_load()`**, not `obs_module_load()`.
  The latter races the main window's construction. OBS's own
  `plugins/decklink-output-ui/decklink-ui-main.cpp` does exactly this.
- **`obs_data_save_json_safe` takes four arguments** — `(data, file, temp_ext,
  backup_ext)`.
- **`obs_module_config_path` is a macro** expanding to
  `obs_module_get_config_path(obs_current_module(), file)`, so it only compiles
  inside the module's own translation units. It does **not** create the
  directory — `os_mkdirs()` first or the save silently fails.
- **Threading.** Calling `obs_output_start`/`obs_output_stop` from a Qt slot is
  safe and is what OBS's own UI does. The hazard is the reverse: output signals
  (`"void stop(ptr output, int code)"` etc., declared in
  `libobs/obs-output.c` `output_signals[]`) fire on **libobs threads** and must
  not touch widgets. They are marshalled with
  `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`.
- **Teardown must happen on `OBS_FRONTEND_EVENT_EXIT`, not in
  `obs_module_unload`.** The frontend — including the dock widget and the
  callback list — is already destroyed by the time a module is unloaded.
  Calling `obs_frontend_remove_dock()` there logs `with no callbacks!` and
  deleting the widget double-frees. See §4.2.

---

## 7. Verification log

Run on 2026-07-26 against OBS Studio 32.2.1 on macOS (Apple Silicon).

| # | Check | Result |
|---|---|---|
| 1 | Build attributes portable | **pass** — arm64, minos 13.0, relative rpath, 0 Homebrew refs, Info.plist bound |
| 2 | New code present in binary | **pass** — `SOURCE`, `Authorization`, `ice-audio-info`, `admin/metadata`, `radioco_dock` all found |
| 3 | Plugin loads, dock registers | **pass** — `[obs-shoutcast] plugin loaded (version 1.0.0)`, `Radio.co dock registered`, `shoutcast_mp3 (MP3 Encoder (LAME))`, no `incompatible` |
| 3b | Dock renders correctly | **pass** — all fields present with correct defaults (`source`, `/`, port 80, 128 kbps); **Apply Name** and **Update** correctly disabled while idle |
| 4 | Handshake + metadata bytes | **pass** — see §1.1 and §1.3; both protocol branches produce correct bytes, auth header byte-identical to the live-proven value |
| 4b | UTF-8 percent-encoding | **pass** — exact round-trip, uppercase hex, `%20` not `+` |
| 5 | Audio actually reaches the server | **initially FAILED — 0 bytes.** Root cause and fix in §4. **Pass after fix**: 254,976 bytes / 664 frames / 128 kbps / 48 kHz |
| 6 | Metadata during a live stream | **pass** — `[icy] metadata updated: Güneş - Kayıp Şarkı`, server received one `song=` parameter and no `artist=`/`title=` |
| 7 | Clean start/stop cycle | **pass** — `streaming started` → `streaming stopped`, `obs_output_active` false at release, no leaked socket |
| 8 | Clean cutover from the Lua script | **pass** — `tools/shoutcast-control.lua` deleted and its registration removed from the scene collection; no `[Lua: …]` lines remain |
| 9 | Audio still flows after the teardown fix | **pass** — 254,592 bytes / 663 frames / 128 kbps / 48 kHz, no regression |
| 10 | Graceful quit is clean | **pass** — `[obs-shoutcast] plugin unloaded` then `Number of memory leaks: 0`, full profiler summary printed, no `remove_dock with no callbacks`. Before the §4.2 fix the log stopped dead at that warning. |
| 11 | Redistributable round-trips | **pass** — zip extracts, `codesign --verify` reports *valid on disk* and *satisfies its Designated Requirement*, attributes unchanged |

Checks that require the production station or the second machine — going live
on Radio.co, metadata on air, the station-name reconnect, both destinations at
once, the follow-OBS checkbox end to end, and quitting while live — are steps
8-10 of the rollout and are performed on the streaming Mac. The protocol,
encoder, output and metadata paths they depend on are all verified above
against a local server.

### How to re-run the local end-to-end test

The harness is not shipped in the repo. To reproduce:

1. Run a listener on `127.0.0.1:9099` that accepts the `SOURCE` handshake,
   replies `HTTP/1.0 200 OK\r\n\r\n`, then counts the bytes and MPEG frame
   headers (`0xFF 0xEx`) that follow.
2. Point the dock at `127.0.0.1` / port `9099` / mount `/` and press
   **Connect**.
3. Expect roughly `bitrate_kbps × seconds / 8` KB and a first frame header of
   `fffb…` for 128 kbps 48 kHz.

Counting frames — not merely observing that a connection succeeded — is the
part that catches the §4 class of bug.
