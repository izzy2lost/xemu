# X1 BOX iOS

This folder contains the iOS and iPadOS shell for **X1 BOX**, kept separate from the Android app.

## What is included

- SwiftUI app shell with:
  - launcher
  - setup wizard
  - library
  - settings
  - emulator container
- Objective-C++ native bridge target named `X1BoxNativeCore`
- sandbox staging for MCPX, flash, HDD, EEPROM, and staged DVD files
- security-scoped bookmark persistence for the games folder
- config writer that mirrors the Android `xemu.toml` structure closely
- touch overlay model and game controller presence monitoring
- basic unit tests for settings persistence, library scanning, and config generation
- an embedded-core bootstrap path that will call official xemu entry points (`qemu_init` / `qemu_main`) when they are linked into the framework
- an iOS-focused embedded host path that can boot and pump frames through exported helpers from `ui/xemu.c`

## Important scope note

The native bridge now does two things:

- it keeps the iOS shell working even with no embedded core linked
- it prefers a dedicated embedded iOS host API exported from `ui/xemu.c`, and falls back to raw `qemu_init` / `qemu_main` only when that API is not linked
- it now also attempts to `dlopen()` a bundled `X1BoxEmbeddedCore.framework/X1BoxEmbeddedCore` image before resolving symbols, so a signed embedded core can light up the real iOS boot path without changing the SwiftUI shell

The remaining platform-specific handoff still lives inside:

- `ios-app/NativeCore/X1BoxNativeBridge.mm`
- `include/ui/xemu-embedded.h`
- `ui/xemu.c`

Those files now expose the embedded boot and frame-pump seam for iPhone and iPad. The next platform-specific step is finishing the SDL/UIKit presentation details in a real Xcode/macOS build.

Because `qemu_init()` is not safely repeatable in the same process, once the embedded core path has been initialized the app should be relaunched before starting a second full embedded session.

### Dynamic embedded core contract

The iOS bridge will look for these developer/runtime integration points:

- a signed bundled framework at `X1BoxEmbeddedCore.framework/X1BoxEmbeddedCore` inside the app's `Frameworks` area
- a signed bundled xcframework at `X1BoxEmbeddedCore.xcframework`, from which the app picks the matching device or simulator slice during the build embed step
- a developer fallback image under `Library/Application Support/X1Box/EmbeddedCore/`
- a packaging drop-in at `ios-app/EmbeddedCore/`, which the Xcode app target now copies into `Frameworks/` through `scripts/embed-x1box-core.sh`
- a framework or dylib imported from the Settings screen and staged into `Application Support/X1Box/EmbeddedCore/`

When that image exports the same symbols already referenced by the bridge (`xemu_embedded_*`, `qemu_init`, `qemu_main`, `xemu_settings_*`, snapshot helpers), the app can move from shell fallback into the real embedded core path on iPhone and iPad.

If you already have an upstream-built binary, `ios-app/scripts/package-x1box-embedded-core.sh <path-to-binary>` wraps it into `ios-app/EmbeddedCore/X1BoxEmbeddedCore.framework` so it can be signed and bundled without changing the SwiftUI shell.
If you have separate device and simulator builds, `ios-app/scripts/package-x1box-embedded-core-xcframework.sh --device <path> --simulator <path>` creates `ios-app/EmbeddedCore/X1BoxEmbeddedCore.xcframework` so the embed step can choose the right slice automatically.
If you download the output of the embedded-core CI workflow, `ios-app/scripts/prepare-embedded-core-dropin.sh <artifact-download-dir>` stages the framework, xcframework, or dylib into `ios-app/EmbeddedCore/` automatically.

## Build setup

1. Open `ios-app/X1BoxiOS.xcodeproj` in Xcode on macOS.
2. Set your Apple development team for `X1BoxiOS`.
3. Build for an `arm64` iPhone or iPad device.
4. The project now also supports `iphonesimulator`, so you can validate the SwiftUI shell and tests without a signed device build first.
5. Use your preferred sideload workflow and external JIT-enabling workflow if needed by the core.

## Remote validation from Windows

From this Windows workspace, the most reliable way to validate the iOS project is through GitHub Actions on macOS.

- The repository now includes `.github/workflows/build-ios-app.yml`.
- The repository now includes `.github/workflows/build-ios-embedded-core.yml` for the macOS-side `libxemu-ios-core.dylib` / `X1BoxEmbeddedCore.xcframework` packaging path.
- The repository also includes `.github/workflows/ios-workflow-followup.yml` for default-branch `workflow_run` follow-up, while feature branches publish the same summary/comment artifact inline from `build-ios-app.yml`.
- That workflow builds the app for iOS Simulator, runs the `X1BoxiOSTests` suite, and also performs a generic iOS device build with signing disabled.
- The helper script `ios-app/scripts/ci-build-ios.sh` is the single source of truth for those `xcodebuild` commands, so the same flow can be reused locally on macOS.
- `build-ios-app.yml` can now optionally download a prior `x1box-ios-embedded-core` artifact and stage it into `ios-app/EmbeddedCore/` before the Xcode build starts.
- After this lands on a default branch, `build-ios-app.yml` can also react to `Build iOS Embedded Core` via `workflow_run`, download that run's default `x1box-ios-embedded-core` artifact, and validate the shell against the packaged core automatically.
- The bridge script `ios-app/scripts/fork-workflow-bridge.ps1` can dispatch the workflow on your fork, wait for completion, and download both the main iOS CI artifact and the follow-up summary artifact back into this workspace.
- The follow-up workflow publishes a compact summary artifact for every run, includes failed jobs and failing steps, and updates the related pull request comment when the run belongs to a PR.

This is the practical answer to "can we program this correctly from here?": yes, by pairing this Windows editing environment with remote macOS CI for every iOS iteration, and by letting GitHub itself react on every workflow use.

### Fork workflow bridge

Set `GITHUB_TOKEN` or `GH_TOKEN` with permissions to run Actions on your fork, then use:

```powershell
$env:GITHUB_TOKEN = "YOUR_TOKEN"
.\ios-app\scripts\fork-workflow-bridge.ps1 -Mode Full -Repo "alejomazabuel/xemu" -Ref "your-branch"
```

Useful modes:

- `dispatch`: trigger the workflow only
- `watch`: wait on an existing run
- `download`: fetch artifacts from an existing run
- `full`: dispatch, wait, and download artifacts in one pass
- `status`: inspect the latest run and write `latest-status.json`
- `sync`: inspect the latest run, write `latest-status.json`, and download artifacts only when a new successful run appears

Examples:

```powershell
.\ios-app\scripts\fork-workflow-bridge.ps1 -Mode Status -Repo "alejomazabuel/xemu" -Ref "ios-branch"
.\ios-app\scripts\fork-workflow-bridge.ps1 -Mode Sync -Repo "alejomazabuel/xemu" -Ref "ios-branch"
```

That makes the script suitable for automatic periodic checks: a scheduler can run `-Mode Sync` every hour, and the bridge will keep the latest JSON summary updated while only downloading artifacts for new successful runs.

## Official xemu linkage target

This project is meant to align with the upstream xemu Apple/SDL architecture from the main repo:

- `ui/xemu.c`
- `include/ui/xemu-embedded.h`
- `ui/sdl2.c`
- `ui/sdl2-gl.c`
- `ui/xemu-settings.cc`
- `system/*` bootstrap around `qemu_init` / `qemu_main`

Once those sources or a static library exposing the same symbols are linked into `X1BoxNativeCore`, the iOS bridge will use the embedded boot path and drive rendering through `CADisplayLink`.

## Project structure

- `App/`: SwiftUI shell, state, models, and services
- `NativeCore/`: Objective-C++ bridge target
- `Support/`: plist, entitlements, asset catalog
- `X1BoxiOSTests/`: unit tests
