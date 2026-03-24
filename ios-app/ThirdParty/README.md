# iOS Embedded Core Dependencies

The upstream xemu core cannot be built for iPhone/iPad from this repo alone.
The embedded core build expects a dependency bundle with cross-compiled
libraries for both Apple SDKs:

- `device/lib/pkgconfig`
- `simulator/lib/pkgconfig`

Each prefix should contain the pkg-config metadata and libraries needed by the
upstream Meson build when `-Dx1box_ios_embedded_core=true`, such as:

- SDL2
- libepoxy
- pixman-1
- glib-2.0
- gio-2.0
- gobject-2.0
- libslirp
- libpng

The script [build-x1box-embedded-core.sh](C:\Users\AlejandroMazabuel\Documents\New project\ios-app\scripts\build-x1box-embedded-core.sh) accepts either:

- `--deps-root <dir>` where `device/` and `simulator/` live underneath, or
- direct `--device-deps <dir>` and `--simulator-deps <dir>` prefixes

Typical artifact layout after a macOS dependency job:

```text
deps/
  device/
    include/
    lib/
      pkgconfig/
  simulator/
    include/
    lib/
      pkgconfig/
```
