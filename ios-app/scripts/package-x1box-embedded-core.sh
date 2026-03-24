#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 <path-to-upstream-core-binary> [output-dir]" >&2
  exit 1
fi

SOURCE_BINARY="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IOS_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_ROOT="${2:-${IOS_ROOT}/EmbeddedCore}"
FRAMEWORK_DIR="${OUTPUT_ROOT}/X1BoxEmbeddedCore.framework"
FRAMEWORK_BINARY="${FRAMEWORK_DIR}/X1BoxEmbeddedCore"
INFO_PLIST="${FRAMEWORK_DIR}/Info.plist"

if [[ ! -f "${SOURCE_BINARY}" ]]; then
  echo "Source binary not found: ${SOURCE_BINARY}" >&2
  exit 1
fi

rm -rf "${FRAMEWORK_DIR}"
mkdir -p "${FRAMEWORK_DIR}"

cp "${SOURCE_BINARY}" "${FRAMEWORK_BINARY}"
chmod +x "${FRAMEWORK_BINARY}"

cat > "${INFO_PLIST}" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>en</string>
  <key>CFBundleExecutable</key>
  <string>X1BoxEmbeddedCore</string>
  <key>CFBundleIdentifier</key>
  <string>com.x1box.embeddedcore</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleName</key>
  <string>X1BoxEmbeddedCore</string>
  <key>CFBundlePackageType</key>
  <string>FMWK</string>
  <key>CFBundleShortVersionString</key>
  <string>1.0</string>
  <key>CFBundleVersion</key>
  <string>1</string>
  <key>MinimumOSVersion</key>
  <string>17.0</string>
</dict>
</plist>
PLIST

echo "Packaged embedded core framework at: ${FRAMEWORK_DIR}"
echo "Next step: codesign it with the same identity you use for X1BoxiOS before deploying to a device."
