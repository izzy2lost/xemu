# Embedded Core Drop-In

This folder is the packaging hook for a real iOS embedded core image.

Supported drop-in names:

- `X1BoxEmbeddedCore.framework`
- `X1BoxEmbeddedCore.xcframework`
- `libxemu-ios-core.dylib`

How it works:

1. Place a signed framework or dynamic library here before building in Xcode.
2. The `X1BoxiOS` app target runs `scripts/embed-x1box-core.sh`.
3. If a matching artifact exists, the build copies it into the app `Frameworks/` folder.
4. If the drop-in is an `.xcframework`, the script picks the `iphoneos` or `iphonesimulator` slice automatically.
4. `X1BoxNativeBridge` then tries to load it at runtime and resolve:
   - `xemu_embedded_*`
   - `qemu_init`
   - `qemu_main`
   - `xemu_settings_*`
   - snapshot helpers

Recommended path for real device testing:

- Use `X1BoxEmbeddedCore.framework` signed with the same development identity as the app.
- You can also import a framework or dylib from the iOS app Settings screen, which stages it into `Application Support/X1Box/EmbeddedCore/` for runtime loading.

Packaging helper:

- If you already have an upstream-built core binary, run `scripts/package-x1box-embedded-core.sh <path-to-binary>` to wrap it into `EmbeddedCore/X1BoxEmbeddedCore.framework`.
- If you already have separate device and simulator artifacts, run `scripts/package-x1box-embedded-core-xcframework.sh --device <path> --simulator <path>` to build `EmbeddedCore/X1BoxEmbeddedCore.xcframework`.
- If you downloaded the `x1box-ios-embedded-core` CI artifact, run `scripts/prepare-embedded-core-dropin.sh <artifact-download-dir>` to stage the correct drop-in into this folder.
- The generated framework is runtime-only. Sign it with the same identity as the app before testing on a real device.

Development fallback:

- `libxemu-ios-core.dylib` is also supported by the bridge for experimentation, but a signed framework is the safer route for iPhone/iPad deployment.
