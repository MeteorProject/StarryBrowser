# Development & Building

All custom Starry Browser code lives in the standalone `starry/` directory (symlinked to `chromium/src/starry` on macOS, or synced to the same location via `scripts/windows_overlay.py` on Windows). Modifications to upstream Chromium are kept minimal as patches under `patches/`.

Hardware requirements match Chromium: at least 16 GB of RAM (32 GB recommended) and 100 GB+ of free disk space. The first full build takes several hours.

## Build Steps

### macOS

Requires Xcode and the Metal toolchain (starting with Xcode 26, install it separately with `xcodebuild -downloadComponent MetalToolchain`):

```sh
scripts/bootstrap.sh          # Fetch depot_tools into vendor/depot_tools
scripts/sync.sh               # Sync Chromium source, link starry/, and apply patches/
scripts/build.sh Release      # Build chrome (runs scripts/gen.sh automatically if needed)
scripts/run.sh Release        # Launch with a temporary user data directory
```

For day-to-day incremental development, use the `Dev` configuration (component build for faster linking; cannot be packaged):

```sh
scripts/build.sh Dev
scripts/run.sh Dev
```

### Windows

Requires Visual Studio (with the **Desktop development with C++** workload and MFC/ATL components), the matching Windows 11 SDK, and Debugging Tools for Windows. Prepend `depot_tools` to `PATH`, set `DEPOT_TOOLS_WIN_TOOLCHAIN=0`, and run `git config --global core.autocrlf false`.

From `cmd` (assuming the repository is at a short path such as `C:\src\starry-browser-chromium`):

```bat
cd /d C:\src\starry-browser-chromium
mkdir chromium
```

Create `chromium\.gclient` (replace the tag with the version in `chromium.version`):

```python
solutions = [
  {
    "name": "src",
    "url": "https://chromium.googlesource.com/chromium/src.git@refs/tags/158.0.8127.37",
    "managed": False,
    "custom_deps": {},
    "custom_vars": {},
  },
]
target_os = ["win"]
```

Sync the source tree, overlay `starry/`, apply patches, and build:

```bat
cd chromium
set /p CHROMIUM_VERSION=<..\chromium.version
gclient sync --nohooks --no-history -D --force --revision src@refs/tags/%CHROMIUM_VERSION%
gclient runhooks

python3 ..\scripts\windows_overlay.py ..\starry src\starry

cd src
for /r "..\..\patches" %f in (*.patch) do git apply --whitespace=nowarn "%f"

mkdir out\Release
echo import("//starry/build/args/release.gn") > out\Release\args.gn
gn gen out\Release
autoninja -C out\Release chrome
```

Run:

```bat
out\Release\starry-browser.exe --user-data-dir=%TEMP%\starry-dev --no-first-run --no-default-browser-check
```

After editing files under `starry\` on Windows, re-run `windows_overlay.py` and run an incremental build.

## Build Configuration

`scripts/gen.sh [Release|Dev]` generates `chromium/src/out/<Config>/args.gn`, importing the build arg files under `starry/build/args/` (`common.gni`, `release.gn`, `dev.gn`).

To add local GN arguments, create `args.local.gn` in the repository root (ignored by `.gitignore`). `scripts/gen.sh` appends its contents automatically; pass `--no-local` to ignore local overrides.

## Patch Management

Keep new code in `starry/` whenever possible. Only modify upstream files in `chromium/src` when necessary, and keep changes minimal:

```sh
scripts/patches.sh apply      # Link starry/ and apply patches/
scripts/patches.sh export     # Re-export current changes in chromium/src to patches/
scripts/patches.sh check      # Verify chromium/src matches patches/
scripts/patches.sh revert     # Revert upstream files modified by patches
scripts/patches.sh sync       # Update only files that differ from patches/ (preserves timestamps on unchanged files; discards unexported changes; for CI)
```

## UI Localization

Locale files live under `starry/browser/resources/l10n/` (`zh-Hans`, `zh-Hant`, `en`, `ja`, `ko`). Use `L("key")` in C++, `L('key')` in page JavaScript, and `$L{key}` in HTML.

Run the following checks before committing:

```sh
scripts/patches.sh check      # Requires synced Chromium source
scripts/l10n.py check
scripts/l10n.py hardcoded
git diff --check
```
