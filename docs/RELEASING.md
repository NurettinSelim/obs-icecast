# Releasing obs-icecast

## Scope and prerequisites

Releases are built locally on the supported Apple Silicon Mac. There is no CI
release pipeline or release script.

Before starting, install or verify:

- `/Applications/OBS.app` at the OBS version targeted by `CMakeLists.txt`.
- Homebrew `qt`, `simde`, and FFmpeg headers. The Homebrew libavcodec major
  must match the major bundled by OBS.
- CMake and the Xcode command-line tools.
- `gh`, authenticated to the GitHub repository.

Public release builds are currently ad-hoc signed. Users must therefore run
the documented `xattr` command after installing the downloaded bundle.

## Protect production state

Quit OBS before changing or backing up:

```
~/Library/Application Support/obs-studio/plugin_config/obs-icecast/settings.json
```

The file contains the broadcast password in plaintext. Never print it, copy it
into the repository, or publish it. OBS rewrites this file on exit, so make
all test-configuration changes only after OBS has quit. Back it up before
local testing and restore it after the final test run has exited.

## Choose and set the version

Use one `X.Y.Z` value everywhere:

1. Set `project(obs-icecast VERSION X.Y.Z ...)` in `CMakeLists.txt`.
2. Set `version` to `X.Y.Z` in `buildspec.json`.
3. Update versioned zip names in `README.md`.
4. Use the Git tag `vX.Y.Z`.

## Build locally

From the repository root:

```bash
cmake -S . -B build && cmake --build build
```

When the targeted OBS version changes, delete `build/` before configuring.
Headers cached under `build/_deps` otherwise remain from the old version.
Never bypass the configure-time architecture, deployment-target, OBS, simde,
or FFmpeg-major checks.

## Sign and package

Set the version chosen above, then sign and package the bundle:

```bash
VERSION=X.Y.Z
codesign --force --sign - \
  --identifier io.github.nurettinselim.obs-icecast \
  --timestamp=none build/obs-icecast.plugin
ditto -c -k --keepParent --norsrc --noextattr --noqtn \
  build/obs-icecast.plugin "obs-icecast-${VERSION}-arm64.zip"
shasum -a 256 "obs-icecast-${VERSION}-arm64.zip"
```

The zip is intentionally gitignored. Put the generated SHA-256 checksum in the
GitHub release body; do not commit the archive.

## Verify the exact artifact

Follow the local-sink procedure in `AGENTS.md` and `docs/FINDINGS.md` §7.
Install the signed `build/obs-icecast.plugin`, then exercise Connect, sustained
MP3 transfer, metadata, reconnect, and clean shutdown. Restore the production
settings only after OBS exits.

Validate the packaged bundle rather than assuming it matches the build tree:

```bash
rm -rf /tmp/obs-icecast-release
mkdir -p /tmp/obs-icecast-release
ditto -x -k "obs-icecast-${VERSION}-arm64.zip" \
  /tmp/obs-icecast-release
codesign -dv \
  /tmp/obs-icecast-release/obs-icecast.plugin
otool -L \
  /tmp/obs-icecast-release/obs-icecast.plugin/Contents/MacOS/obs-icecast
```

The signature must report identifier `io.github.nurettinselim.obs-icecast`.
The binary may link only `@rpath/…`, `/usr/lib/libc++.1.dylib`, and
`/usr/lib/libSystem.B.dylib`. Install this extracted bundle and repeat the
local-sink Connect check so the exact packaged artifact is exercised.

## Commit and publish

Commit the version and documentation changes, then push `main` before creating
the tag:

```bash
git add CMakeLists.txt buildspec.json README.md docs/RELEASING.md
git commit -m "Document local releases and bump version to X.Y.Z"
git push origin main
git tag "v${VERSION}"
git push origin "v${VERSION}"
gh release create "v${VERSION}" "obs-icecast-${VERSION}-arm64.zip" \
  --title "obs-icecast ${VERSION}" \
  --notes-file /tmp/obs-icecast-release-notes.md
```

The release body must contain:

- **What's new**.
- **Requirements**, clearly identifying what changed or remained unchanged.
- **Install**, using the commands from `README.md`.
- **Verification**, with the exact zip filename and SHA-256.

## Post-publication check

Run `gh release view "v${VERSION}"` and confirm that it lists the versioned zip
asset. Download the asset from GitHub, calculate its SHA-256, and compare it
with the checksum in the release body. Confirm that `README.md` points to the
same version.

If publication fails before a release exists, fix the problem locally and
recreate or move the tag as needed. Never overwrite an existing public release
artifact under the same version.
