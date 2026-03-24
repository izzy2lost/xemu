#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF' >&2
Usage: build-x1box-ios-deps.sh [--output <dir>] [--min-ios <version>] [--simulator-arch <auto|arm64|x86_64>] [--artifact-name <name>]

This script bootstraps vcpkg on macOS, builds the iOS dependency prefixes used
by the X1 BOX embedded-core pipeline, and stages them as:

  <output>/artifacts/<artifact-name>/device
  <output>/artifacts/<artifact-name>/simulator

The generated prefixes contain pkg-config metadata that is normalized so the
artifact can be downloaded elsewhere and consumed by build-x1box-embedded-core.sh.
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IOS_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${IOS_ROOT}/.." && pwd)"

BUILD_ROOT="${X1BOX_IOS_DEPS_BUILD_ROOT:-${REPO_ROOT}/build/ios-deps}"
MIN_IOS_VERSION="${X1BOX_IOS_MIN_VERSION:-17.0}"
SIMULATOR_ARCH="${X1BOX_IOS_SIMULATOR_ARCH:-auto}"
ARTIFACT_NAME="${X1BOX_IOS_DEPS_ARTIFACT_NAME:-x1box-ios-deps}"
VCPKG_ROOT="${X1BOX_VCPKG_ROOT:-${BUILD_ROOT}/vcpkg}"
OVERLAY_TRIPLETS="${IOS_ROOT}/vcpkg-triplets"
OVERLAY_PORTS="${X1BOX_VCPKG_OVERLAY_PORTS:-${IOS_ROOT}/vcpkg-ports}"

while [[ $# -gt 0 ]]; do
  case "$1" in
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
    --artifact-name)
      ARTIFACT_NAME="${2:-}"
      shift 2
      ;;
    *)
      usage
      exit 1
      ;;
  esac
done

require_cmd() {
  local name="$1"
  if ! command -v "${name}" >/dev/null 2>&1; then
    echo "Missing required command: ${name}" >&2
    exit 1
  fi
}

require_cmd git
require_cmd xcodebuild
require_cmd xcrun
require_cmd cmake
require_cmd python3
require_cmd unzip
require_cmd rsync
require_cmd sed

export X1BOX_IOS_MIN_VERSION="${MIN_IOS_VERSION}"

case "${SIMULATOR_ARCH}" in
  auto)
    case "$(uname -m)" in
      arm64) SIMULATOR_ARCH="arm64" ;;
      *) SIMULATOR_ARCH="x86_64" ;;
    esac
    ;;
  arm64|x86_64)
    ;;
  *)
    echo "Unsupported simulator arch: ${SIMULATOR_ARCH}" >&2
    exit 1
    ;;
esac

DEVICE_TRIPLET="arm64-ios-release"
case "${SIMULATOR_ARCH}" in
  arm64) SIMULATOR_TRIPLET="arm64-ios-sim-release" ;;
  x86_64) SIMULATOR_TRIPLET="x64-ios-sim-release" ;;
esac

HOST_TRIPLET="x64-osx"
if [[ "$(uname -m)" == "arm64" ]]; then
  HOST_TRIPLET="arm64-osx"
fi

INSTALL_ROOT="${BUILD_ROOT}/installed"
LOG_DIR="${BUILD_ROOT}/logs"
ARTIFACT_ROOT="${BUILD_ROOT}/artifacts/${ARTIFACT_NAME}"
DEVICE_PREFIX="${INSTALL_ROOT}/${DEVICE_TRIPLET}"
SIMULATOR_PREFIX="${INSTALL_ROOT}/${SIMULATOR_TRIPLET}"

rm -rf "${BUILD_ROOT}"
mkdir -p "${LOG_DIR}" "${ARTIFACT_ROOT}"

bootstrap_vcpkg() {
  if [[ ! -d "${VCPKG_ROOT}/.git" ]]; then
    git clone --depth 1 https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}" >"${LOG_DIR}/vcpkg-clone.log" 2>&1
  fi

  if [[ ! -x "${VCPKG_ROOT}/vcpkg" ]]; then
    (cd "${VCPKG_ROOT}" && ./bootstrap-vcpkg.sh -disableMetrics) >"${LOG_DIR}/vcpkg-bootstrap.log" 2>&1
  fi
}

install_triplet() {
  local triplet="$1"
  local log_name="$2"
  local manifest_dir="${BUILD_ROOT}/manifest-${triplet}"
  local effective_keep_env_vars="${VCPKG_KEEP_ENV_VARS:-}"
  local effective_ldflags="${LDFLAGS:-}"

  if [[ -n "${effective_keep_env_vars}" ]]; then
    effective_keep_env_vars="${effective_keep_env_vars};LDFLAGS"
  else
    effective_keep_env_vars="LDFLAGS"
  fi

  if [[ -n "${effective_ldflags}" ]]; then
    effective_ldflags="${effective_ldflags} -framework CoreFoundation"
  else
    effective_ldflags="-framework CoreFoundation"
  fi

  mkdir -p "${manifest_dir}"
  cat > "${manifest_dir}/vcpkg.json" <<'EOF'
{
  "name": "x1box-ios-deps",
  "version-string": "1.0.0",
  "dependencies": [
    "glib",
    "libffi",
    "libpng",
    "libslirp",
    "pcre2",
    "pixman",
    "sdl2",
    "zlib"
  ]
}
EOF

  VCPKG_OVERLAY_TRIPLETS="${OVERLAY_TRIPLETS}" \
  VCPKG_FORCE_SYSTEM_BINARIES=1 \
  VCPKG_KEEP_ENV_VARS="${effective_keep_env_vars}" \
  LDFLAGS="${effective_ldflags}" \
  "${VCPKG_ROOT}/vcpkg" install \
    --x-manifest-root="${manifest_dir}" \
    --triplet="${triplet}" \
    --host-triplet="${HOST_TRIPLET}" \
    --overlay-triplets="${OVERLAY_TRIPLETS}" \
    --overlay-ports="${OVERLAY_PORTS}" \
    --x-install-root="${INSTALL_ROOT}" \
    --clean-after-build \
    >"${LOG_DIR}/${log_name}.log" 2>&1
}

normalize_pkgconfig_prefixes() {
  local prefix_root="$1"
  local pkgconfig_dir
  pkgconfig_dir="${prefix_root}/lib/pkgconfig"

  if [[ ! -d "${pkgconfig_dir}" ]]; then
    return 0
  fi

  find "${pkgconfig_dir}" -name '*.pc' -print0 | while IFS= read -r -d '' pcfile; do
    python3 - "$pcfile" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
lines = text.splitlines()
rewritten = []
replaced = False
for line in lines:
    if line.startswith("prefix="):
        rewritten.append("prefix=${pcfiledir}/../..")
        replaced = True
    else:
        rewritten.append(line)
if not replaced:
    rewritten.insert(0, "prefix=${pcfiledir}/../..")
path.write_text("\n".join(rewritten) + "\n", encoding="utf-8")
PY
  done
}

stage_prefix() {
  local source_prefix="$1"
  local destination_prefix="$2"

  rm -rf "${destination_prefix}"
  mkdir -p "${destination_prefix}"
  rsync -a "${source_prefix}/" "${destination_prefix}/"
  normalize_pkgconfig_prefixes "${destination_prefix}"
}

write_metadata() {
  cat > "${ARTIFACT_ROOT}/x1box-ios-deps.json" <<EOF
{
  "artifact_name": "${ARTIFACT_NAME}",
  "minimum_ios_version": "${MIN_IOS_VERSION}",
  "host_triplet": "${HOST_TRIPLET}",
  "device_triplet": "${DEVICE_TRIPLET}",
  "simulator_triplet": "${SIMULATOR_TRIPLET}",
  "simulator_arch": "${SIMULATOR_ARCH}",
  "generated_at_utc": "$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
}
EOF
}

bootstrap_vcpkg
install_triplet "${DEVICE_TRIPLET}" "device-install"
install_triplet "${SIMULATOR_TRIPLET}" "simulator-install"

stage_prefix "${DEVICE_PREFIX}" "${ARTIFACT_ROOT}/device"
stage_prefix "${SIMULATOR_PREFIX}" "${ARTIFACT_ROOT}/simulator"
write_metadata

echo "Prepared X1 BOX iOS dependency artifact at: ${ARTIFACT_ROOT}"
