#!/bin/bash
set -euo pipefail

CORE_ROOT="${SRCROOT}/EmbeddedCore"
FRAMEWORK_NAME="X1BoxEmbeddedCore.framework"
SOURCE_FRAMEWORK="${CORE_ROOT}/${FRAMEWORK_NAME}"
SOURCE_DYLIB="${CORE_ROOT}/libxemu-ios-core.dylib"
DEST_ROOT="${TARGET_BUILD_DIR}/${FRAMEWORKS_FOLDER_PATH}"

copy_and_sign() {
  local source_path="$1"
  local destination_path="$2"

  rm -rf "${destination_path}"
  mkdir -p "$(dirname "${destination_path}")"
  /usr/bin/ditto "${source_path}" "${destination_path}"

  if [[ "${CODE_SIGNING_ALLOWED:-NO}" == "YES" ]] && [[ -n "${EXPANDED_CODE_SIGN_IDENTITY:-}" ]] && [[ "${EXPANDED_CODE_SIGN_IDENTITY}" != "-" ]]; then
    /usr/bin/codesign --force --sign "${EXPANDED_CODE_SIGN_IDENTITY}" --preserve-metadata=identifier,entitlements "${destination_path}"
  fi

  echo "Embedded optional core artifact: ${destination_path}"
}

mkdir -p "${DEST_ROOT}"

if [[ -d "${SOURCE_FRAMEWORK}" ]]; then
  copy_and_sign "${SOURCE_FRAMEWORK}" "${DEST_ROOT}/${FRAMEWORK_NAME}"
  exit 0
fi

if [[ -f "${SOURCE_DYLIB}" ]]; then
  copy_and_sign "${SOURCE_DYLIB}" "${DEST_ROOT}/libxemu-ios-core.dylib"
  exit 0
fi

echo "No optional embedded core artifact was found under ${CORE_ROOT}; skipping bundle embed."
