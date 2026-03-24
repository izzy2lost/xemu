#!/bin/bash
set -euo pipefail

CORE_ROOT="${SRCROOT}/EmbeddedCore"
FRAMEWORK_NAME="X1BoxEmbeddedCore.framework"
XCFRAMEWORK_NAME="X1BoxEmbeddedCore.xcframework"
SOURCE_FRAMEWORK="${CORE_ROOT}/${FRAMEWORK_NAME}"
SOURCE_XCFRAMEWORK="${CORE_ROOT}/${XCFRAMEWORK_NAME}"
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

resolve_xcframework_source() {
  local slice_candidates=()

  case "${PLATFORM_NAME:-}" in
    iphonesimulator)
      slice_candidates=(
        "ios-arm64_x86_64-simulator"
        "ios-arm64-simulator"
        "ios-x86_64-simulator"
      )
      ;;
    *)
      slice_candidates=("ios-arm64")
      ;;
  esac

  for slice in "${slice_candidates[@]}"; do
    local framework_candidate="${SOURCE_XCFRAMEWORK}/${slice}/${FRAMEWORK_NAME}"
    local dylib_candidate="${SOURCE_XCFRAMEWORK}/${slice}/libxemu-ios-core.dylib"
    if [[ -d "${framework_candidate}" ]]; then
      printf '%s\n' "${framework_candidate}"
      return 0
    fi
    if [[ -f "${dylib_candidate}" ]]; then
      printf '%s\n' "${dylib_candidate}"
      return 0
    fi
  done

  return 1
}

mkdir -p "${DEST_ROOT}"

if [[ -d "${SOURCE_FRAMEWORK}" ]]; then
  copy_and_sign "${SOURCE_FRAMEWORK}" "${DEST_ROOT}/${FRAMEWORK_NAME}"
  exit 0
fi

if [[ -d "${SOURCE_XCFRAMEWORK}" ]]; then
  if XCFRAMEWORK_SOURCE="$(resolve_xcframework_source)"; then
    if [[ -d "${XCFRAMEWORK_SOURCE}" ]]; then
      copy_and_sign "${XCFRAMEWORK_SOURCE}" "${DEST_ROOT}/${FRAMEWORK_NAME}"
    else
      copy_and_sign "${XCFRAMEWORK_SOURCE}" "${DEST_ROOT}/libxemu-ios-core.dylib"
    fi
    exit 0
  fi

  echo "Found ${XCFRAMEWORK_NAME}, but no matching slice was available for ${PLATFORM_NAME:-unknown}."
  exit 1
fi

if [[ -f "${SOURCE_DYLIB}" ]]; then
  copy_and_sign "${SOURCE_DYLIB}" "${DEST_ROOT}/libxemu-ios-core.dylib"
  exit 0
fi

echo "No optional embedded core artifact was found under ${CORE_ROOT}; skipping bundle embed."
