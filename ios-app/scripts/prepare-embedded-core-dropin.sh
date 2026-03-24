#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF' >&2
Usage: prepare-embedded-core-dropin.sh <artifact-root> [destination-root]

The source directory can be the raw download location from the embedded-core
workflow artifact. The script searches recursively for one of:

  - X1BoxEmbeddedCore.xcframework
  - X1BoxEmbeddedCore.framework
  - libxemu-ios-core.dylib

and stages the first match into ios-app/EmbeddedCore/ so the Xcode embed step
can pick it up automatically.
EOF
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
  usage
  exit 1
fi

SOURCE_ROOT="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IOS_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEST_ROOT="${2:-${IOS_ROOT}/EmbeddedCore}"

if [[ ! -d "${SOURCE_ROOT}" ]]; then
  echo "Artifact root not found: ${SOURCE_ROOT}" >&2
  exit 1
fi

find_first_match() {
  local pattern="$1"
  find "${SOURCE_ROOT}" -mindepth 1 \( -name "${pattern}" \) -print | head -n 1
}

stage_artifact() {
  local source_path="$1"
  local destination_path="$2"

  rm -rf "${DEST_ROOT}/X1BoxEmbeddedCore.framework" \
         "${DEST_ROOT}/X1BoxEmbeddedCore.xcframework" \
         "${DEST_ROOT}/libxemu-ios-core.dylib"

  mkdir -p "${DEST_ROOT}"
  if [[ -d "${source_path}" ]]; then
    /usr/bin/ditto "${source_path}" "${destination_path}"
  else
    cp "${source_path}" "${destination_path}"
  fi
}

selected_path="$(find_first_match "X1BoxEmbeddedCore.xcframework")"
selected_name="X1BoxEmbeddedCore.xcframework"

if [[ -z "${selected_path}" ]]; then
  selected_path="$(find_first_match "X1BoxEmbeddedCore.framework")"
  selected_name="X1BoxEmbeddedCore.framework"
fi

if [[ -z "${selected_path}" ]]; then
  selected_path="$(find_first_match "libxemu-ios-core.dylib")"
  selected_name="libxemu-ios-core.dylib"
fi

if [[ -z "${selected_path}" ]]; then
  echo "No embedded core drop-in was found under ${SOURCE_ROOT}." >&2
  exit 1
fi

stage_artifact "${selected_path}" "${DEST_ROOT}/${selected_name}"
echo "Staged embedded core drop-in: ${DEST_ROOT}/${selected_name}"
