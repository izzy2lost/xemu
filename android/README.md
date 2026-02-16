# xemu Android bootstrap

This directory mirrors Super3's Android setup (AGP, NDK, SDK levels) and wires
up SDL2 for an Android-native entry point. It currently builds a minimal SDL2
bootstrap (see `app/src/main/cpp/xemu_android.cpp`) that opens an ES context and
runs an event loop. The xemu core is not yet wired.

## Toolchain expectations
- Android SDK 36
- Build Tools 36.1.0
- NDK r29+ (configured to 29.0.14206865 in Gradle)
- CMake 3.30.3
- JDK 21
- Rust toolchain (`cargo`) for ISO->XISO converter
  - On Windows, this project uses `stable-x86_64-pc-windows-gnu` (to avoid MSVC `link.exe`)
  - Install once:
    - `rustup toolchain install stable-x86_64-pc-windows-gnu`
    - `rustup target add aarch64-linux-android --toolchain stable-x86_64-pc-windows-gnu`

If Rust is not available, configure CMake with `-DXEMU_ENABLE_XISO_CONVERTER=OFF`.

## Build
From this directory:

```
./gradlew assembleDebug
```

## SDL2
SDL2 is fetched via CMake (default `release-2.32.10`). To use a local checkout:

```
./gradlew assembleDebug -Pandroid.experimental.cmake.arguments=-DSDL2_LOCAL_DIR=/path/to/SDL
```

## Core integration note
Mainline xemu moved to SDL3 on 2026-01-21. The last SDL2-based tag is `v0.8.133`.
If you plan to wire the core into Android with SDL2, start from that tag or
cherry-pick the SDL2-based frontend changes.
