#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 --device <framework-or-dylib> [--simulator <framework-or-dylib>] [--output <dir>]" >&2
}

DEVICE_ARTIFACT=""
SIMULATOR_ARTIFACT=""
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IOS_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_ROOT="${IOS_ROOT}/EmbeddedCore"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --device)
      DEVICE_ARTIFACT="${2:-}"
      shift 2
      ;;
    --simulator)
      SIMULATOR_ARTIFACT="${2:-}"
      shift 2
      ;;
    --output)
      OUTPUT_ROOT="${2:-}"
      shift 2
      ;;
    *)
      usage
      exit 1
      ;;
  esac
done

if [[ -z "${DEVICE_ARTIFACT}" ]]; then
  usage
  exit 1
fi

if ! command -v xcodebuild >/dev/null 2>&1; then
  echo "xcodebuild is required to create an xcframework." >&2
  exit 1
fi

append_artifact_args() {
  local path="$1"
  if [[ -d "${path}" && "${path}" == *.framework ]]; then
    XCFRAMEWORK_ARGS+=(-framework "${path}")
    return 0
  fi

  if [[ -f "${path}" ]]; then
    XCFRAMEWORK_ARGS+=(-library "${path}")
    return 0
  fi

  echo "Unsupported artifact path: ${path}" >&2
  exit 1
}

XCFRAMEWORK_DIR="${OUTPUT_ROOT}/X1BoxEmbeddedCore.xcframework"
rm -rf "${XCFRAMEWORK_DIR}"
mkdir -p "${OUTPUT_ROOT}"

declare -a XCFRAMEWORK_ARGS
append_artifact_args "${DEVICE_ARTIFACT}"

if [[ -n "${SIMULATOR_ARTIFACT}" ]]; then
  append_artifact_args "${SIMULATOR_ARTIFACT}"
fi

xcodebuild -create-xcframework \
  "${XCFRAMEWORK_ARGS[@]}" \
  -output "${XCFRAMEWORK_DIR}"

echo "Packaged embedded core xcframework at: ${XCFRAMEWORK_DIR}"
echo "Next step: sign the selected framework slice with the same identity as X1BoxiOS when deploying to a real device."
