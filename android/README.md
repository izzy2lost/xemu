# xemu Android

This directory contains the Android frontend and native build for xemu. The
Android entry point in `app/src/main/cpp/xemu_android.cpp` integrates the xemu
core with SDL2 and the Android Vulkan/OpenGL rendering paths.

## Toolchain expectations
- Android SDK 36
- Build Tools 36.1.0
- NDK r30+ (configured to 30.0.15729638 in Gradle)
- CMake 3.22.1
- Meson
- Ninja
- JDK 21
- Rust toolchain (`cargo`) for the ISO->XISO converter and DSP JIT backend
  - On Windows, this project uses `stable-x86_64-pc-windows-gnu` (to avoid MSVC `link.exe`)
  - Install once:
    - `rustup toolchain install stable-x86_64-pc-windows-gnu`
    - `rustup target add aarch64-linux-android --toolchain stable-x86_64-pc-windows-gnu`

Rust is now required for Android builds because the DSP JIT backend is built
from source. `-DXEMU_ENABLE_XISO_CONVERTER=OFF` only disables the ISO->XISO
converter; it does not remove the DSP JIT Rust dependency.

## Build
From this directory:

```
./gradlew assembleDebug
```

For a release-optimized APK that Android Studio can profile without making the
app debuggable:

```
./gradlew assembleProfile
```

The resulting APK is written to
`app/build/outputs/apk/profile/app-profile.apk`. Open it with Android Studio's
**Profile or Debug APK** action. A connected arm64 Android device is required
to record CPU, memory, power, or system traces.

Host tools installed outside the normal `PATH` can be supplied in the ignored
`local.properties` file:

```
xemu.cargoExecutable=/absolute/path/to/cargo
xemu.mesonExecutable=/absolute/path/to/meson
xemu.cargoHome=/absolute/path/to/cargo-home
xemu.rustupHome=/absolute/path/to/rustup-home
xemu.hostRustLinker=/absolute/path/to/a/host-linker
```

## SDL2
SDL2 is fetched via CMake (default `release-2.32.10`). To use a local checkout:

```
./gradlew assembleDebug -Pandroid.experimental.cmake.arguments=-DSDL2_LOCAL_DIR=/path/to/SDL
```
