#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF' >&2
Usage: build-x1box-embedded-core.sh [--deps-root <dir>] [--device-deps <dir>] [--simulator-deps <dir>] [--output <dir>] [--min-ios <version>] [--simulator-arch <auto|arm64|x86_64>]

Dependency layout:
  <deps-root>/device/lib/pkgconfig
  <deps-root>/simulator/lib/pkgconfig

The dependency prefixes are expected to contain the cross-compiled libraries
and pkg-config metadata needed by the upstream xemu Meson build, such as SDL2,
epoxy, pixman, glib, and libslirp for iphoneos and iphonesimulator.
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IOS_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${IOS_ROOT}/.." && pwd)"

DEPS_ROOT="${X1BOX_IOS_DEPS_ROOT:-}"
DEVICE_DEPS="${X1BOX_IOS_DEVICE_DEP_PREFIX:-}"
SIMULATOR_DEPS="${X1BOX_IOS_SIMULATOR_DEP_PREFIX:-}"
BUILD_ROOT="${X1BOX_IOS_EMBEDDED_CORE_BUILD_ROOT:-${REPO_ROOT}/build/ios-embedded-core}"
MIN_IOS_VERSION="${X1BOX_IOS_MIN_VERSION:-17.0}"
SIMULATOR_ARCH="${X1BOX_IOS_SIMULATOR_ARCH:-auto}"

resolve_deps_root() {
  local candidate_root="$1"
  local nested_root

  if [[ -d "${candidate_root}/device" && -d "${candidate_root}/simulator" ]]; then
    printf '%s\n' "${candidate_root}"
    return 0
  fi

  if [[ -d "${candidate_root}/artifacts" ]]; then
    while IFS= read -r -d '' nested_root; do
      if [[ -d "${nested_root}/device" && -d "${nested_root}/simulator" ]]; then
        printf '%s\n' "${nested_root}"
        return 0
      fi
    done < <(find "${candidate_root}/artifacts" -mindepth 1 -maxdepth 1 -type d -print0)
  fi

  printf '%s\n' "${candidate_root}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --deps-root)
      DEPS_ROOT="${2:-}"
      shift 2
      ;;
    --device-deps)
      DEVICE_DEPS="${2:-}"
      shift 2
      ;;
    --simulator-deps)
      SIMULATOR_DEPS="${2:-}"
      shift 2
      ;;
    --output)
      BUILD_ROOT="${2:-}"
      shift 2
      ;;
    --min-ios)
      MIN_IOS_VERSION="${2:-}"
      shift 2
      ;;
    --simulator-arch)
      SIMULATOR_ARCH="${2:-}"
      shift 2
      ;;
    *)
      usage
      exit 1
      ;;
  esac
done

if [[ -n "${DEPS_ROOT}" ]]; then
  DEPS_ROOT="$(resolve_deps_root "${DEPS_ROOT}")"
  : "${DEVICE_DEPS:=${DEPS_ROOT}/device}"
  : "${SIMULATOR_DEPS:=${DEPS_ROOT}/simulator}"
fi

if [[ -z "${DEVICE_DEPS}" || -z "${SIMULATOR_DEPS}" ]]; then
  usage
  exit 1
fi

require_cmd() {
  local name="$1"
  if ! command -v "${name}" >/dev/null 2>&1; then
    echo "Missing required command: ${name}" >&2
    exit 1
  fi
}

require_path() {
  local path="$1"
  local label="$2"
  if [[ ! -d "${path}" ]]; then
    echo "Missing ${label}: ${path}" >&2
    exit 1
  fi
  if [[ ! -d "${path}/lib/pkgconfig" ]]; then
    echo "Missing ${label} pkg-config directory: ${path}/lib/pkgconfig" >&2
    exit 1
  fi
}

require_cmd xcrun
require_cmd meson
require_cmd ninja
require_cmd pkg-config

require_path "${DEVICE_DEPS}" "device dependency prefix"
require_path "${SIMULATOR_DEPS}" "simulator dependency prefix"

if [[ "${SIMULATOR_ARCH}" == "auto" ]]; then
  case "$(uname -m)" in
    arm64) SIMULATOR_ARCH="arm64" ;;
    *) SIMULATOR_ARCH="x86_64" ;;
  esac
fi

case "${SIMULATOR_ARCH}" in
  arm64) SIMULATOR_TRIPLE="arm64-apple-ios${MIN_IOS_VERSION}-simulator" ;;
  x86_64) SIMULATOR_TRIPLE="x86_64-apple-ios${MIN_IOS_VERSION}-simulator" ;;
  *)
    echo "Unsupported simulator arch: ${SIMULATOR_ARCH}" >&2
    exit 1
    ;;
esac

DEVICE_TRIPLE="arm64-apple-ios${MIN_IOS_VERSION}"
ARTIFACT_ROOT="${BUILD_ROOT}/artifacts"
DEVICE_BUILD_DIR="${BUILD_ROOT}/iphoneos"
SIMULATOR_BUILD_DIR="${BUILD_ROOT}/iphonesimulator"
LOG_DIR="${BUILD_ROOT}/logs"
PKG_CONFIG_BIN="$(command -v pkg-config)"
PKG_CONFIG_WRAPPER="${BUILD_ROOT}/pkg-config-static.sh"

rm -rf "${BUILD_ROOT}"
mkdir -p "${ARTIFACT_ROOT}" "${LOG_DIR}"

cat > "${PKG_CONFIG_WRAPPER}" <<EOF
#!/usr/bin/env bash
exec "${PKG_CONFIG_BIN}" --static "\$@"
EOF
chmod +x "${PKG_CONFIG_WRAPPER}"

configure_cpu() {
  case "$1" in
    arm64) printf 'aarch64\n' ;;
    *) printf '%s\n' "$1" ;;
  esac
}

configure_and_build() {
  local sdk="$1"
  local build_dir="$2"
  local dep_prefix="$3"
  local triple="$4"
  local dylib_output_name="$5"
  local min_flag="$6"
  local configure_cpu_name

  local clang
  local clangxx
  local ar
  local nm
  local ranlib
  local strip
  local sdkroot
  local cflags
  local ldflags
  local dylib_path
  local native_clang
  local native_clangxx
  local native_sdkroot
  local native_cflags

  configure_cpu_name="$(configure_cpu "$(echo "${triple}" | cut -d- -f1)")"
  sdkroot="$(xcrun --sdk "${sdk}" --show-sdk-path)"
  clang="$(xcrun --sdk "${sdk}" --find clang)"
  clangxx="$(xcrun --sdk "${sdk}" --find clang++)"
  native_clang="$(xcrun --sdk macosx --find clang)"
  native_clangxx="$(xcrun --sdk macosx --find clang++)"
  native_sdkroot="$(xcrun --sdk macosx --show-sdk-path)"
  ar="$(xcrun --sdk "${sdk}" --find ar)"
  nm="$(xcrun --sdk "${sdk}" --find nm)"
  ranlib="$(xcrun --sdk "${sdk}" --find ranlib)"
  strip="$(xcrun --sdk "${sdk}" --find strip)"

  cflags="--target=${triple} -isysroot ${sdkroot} ${min_flag}"
  ldflags="--target=${triple} -isysroot ${sdkroot} ${min_flag}"
  native_cflags="-isysroot ${native_sdkroot}"

  mkdir -p "${build_dir}"
  pushd "${build_dir}" >/dev/null

  export CC="${clang}"
  export CXX="${clangxx}"
  export OBJC="${clang}"
  export AR="${ar}"
  export NM="${nm}"
  export RANLIB="${ranlib}"
  export STRIP="${strip}"
  export PKG_CONFIG="${PKG_CONFIG_WRAPPER}"
  export PKG_CONFIG_ALLOW_CROSS=1
  export PKG_CONFIG_PATH="${dep_prefix}/lib/pkgconfig"
  export PKG_CONFIG_LIBDIR="${dep_prefix}/lib/pkgconfig"
  unset PKG_CONFIG_SYSROOT_DIR

  "${REPO_ROOT}/configure" \
    --cross-prefix= \
    --cpu="${configure_cpu_name}" \
    --host-cc="${native_clang}" \
    --host-cxx="${native_clangxx}" \
    --host-cflags="${native_cflags}" \
    --host-cxxflags="${native_cflags}" \
    --host-ldflags="${native_cflags}" \
    --target-list=i386-softmmu \
    --disable-docs \
    --disable-tools \
    --disable-guest-agent \
    --disable-install-blobs \
    --disable-cocoa \
    --disable-coreaudio \
    --disable-gettext \
    --disable-linux-user \
    --disable-bsd-user \
    --disable-guest-agent-msi \
    --disable-hvf \
    --disable-pvg \
    --disable-vmnet \
    --disable-vnc \
    --disable-spice \
    --disable-spice-protocol \
    --disable-smartcard \
    --disable-usb-redir \
    --disable-libiscsi \
    --disable-libnfs \
    --disable-libssh \
    --disable-rbd \
    --disable-vde \
    --disable-passt \
    --disable-virglrenderer \
    --disable-rutabaga-gfx \
    --disable-vte \
    --without-default-features \
    --audio-drv-list=sdl \
    --enable-opengl \
    --enable-pixman \
    --enable-png \
    --enable-sdl \
    --enable-slirp \
    --extra-cflags="${cflags}" \
    --extra-cxxflags="${cflags}" \
    --extra-objcflags="${cflags}" \
    --extra-ldflags="${ldflags}" \
    -Dx1box_ios_embedded_core=true \
    >"${LOG_DIR}/${sdk}-configure.log" 2>&1

  ninja x1box-ios-embedded-core >"${LOG_DIR}/${sdk}-build.log" 2>&1

  dylib_path="$(find . -name 'libxemu-ios-core*.dylib' | head -n 1)"
  if [[ -z "${dylib_path}" ]]; then
    echo "Failed to locate libxemu-ios-core.dylib in ${build_dir}" >&2
    exit 1
  fi

  cp "${dylib_path}" "${ARTIFACT_ROOT}/${dylib_output_name}"
  popd >/dev/null
}

configure_and_build \
  iphoneos \
  "${DEVICE_BUILD_DIR}" \
  "${DEVICE_DEPS}" \
  "${DEVICE_TRIPLE}" \
  "libxemu-ios-core-device.dylib" \
  "-miphoneos-version-min=${MIN_IOS_VERSION}"

configure_and_build \
  iphonesimulator \
  "${SIMULATOR_BUILD_DIR}" \
  "${SIMULATOR_DEPS}" \
  "${SIMULATOR_TRIPLE}" \
  "libxemu-ios-core-simulator.dylib" \
  "-mios-simulator-version-min=${MIN_IOS_VERSION}"

bash "${SCRIPT_DIR}/package-x1box-embedded-core.sh" \
  "${ARTIFACT_ROOT}/libxemu-ios-core-device.dylib" \
  "${ARTIFACT_ROOT}"

bash "${SCRIPT_DIR}/package-x1box-embedded-core-xcframework.sh" \
  --device "${ARTIFACT_ROOT}/libxemu-ios-core-device.dylib" \
  --simulator "${ARTIFACT_ROOT}/libxemu-ios-core-simulator.dylib" \
  --output "${ARTIFACT_ROOT}"

echo "Embedded core artifacts prepared in: ${ARTIFACT_ROOT}"
