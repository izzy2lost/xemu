#!/usr/bin/env bash
set -euo pipefail

PROJECT="ios-app/X1BoxiOS.xcodeproj"
SCHEME="X1BoxiOS"
SIM_DESTINATION="${SIM_DESTINATION:-platform=iOS Simulator,name=iPhone 16,OS=latest}"
BUILD_ROOT="${BUILD_ROOT:-build/ios-ci}"
RUN_DEVICE_BUILD="${RUN_DEVICE_BUILD:-true}"

mkdir -p "${BUILD_ROOT}/logs" "${BUILD_ROOT}/results"

echo "Using project: ${PROJECT}"
echo "Using scheme: ${SCHEME}"
echo "Using simulator destination: ${SIM_DESTINATION}"
echo "Run generic iOS device build: ${RUN_DEVICE_BUILD}"

run_step() {
  local name="$1"
  shift

  echo
  echo "==> ${name}"
  set -o pipefail
  "$@" | tee "${BUILD_ROOT}/logs/${name}.log"
}

run_step show-build-settings \
  xcodebuild \
    -project "${PROJECT}" \
    -scheme "${SCHEME}" \
    -showBuildSettings

run_step build-simulator \
  xcodebuild \
    -project "${PROJECT}" \
    -scheme "${SCHEME}" \
    -configuration Debug \
    -destination "${SIM_DESTINATION}" \
    -derivedDataPath "${BUILD_ROOT}/DerivedData-simulator" \
    CODE_SIGNING_ALLOWED=NO \
    build

run_step test-simulator \
  xcodebuild \
    -project "${PROJECT}" \
    -scheme "${SCHEME}" \
    -configuration Debug \
    -destination "${SIM_DESTINATION}" \
    -derivedDataPath "${BUILD_ROOT}/DerivedData-tests" \
    -resultBundlePath "${BUILD_ROOT}/results/X1BoxiOS-SimulatorTests.xcresult" \
    CODE_SIGNING_ALLOWED=NO \
    test

if [[ "${RUN_DEVICE_BUILD}" == "true" ]]; then
  run_step build-device \
    xcodebuild \
      -project "${PROJECT}" \
      -scheme "${SCHEME}" \
      -configuration Debug \
      -destination "generic/platform=iOS" \
      -derivedDataPath "${BUILD_ROOT}/DerivedData-device" \
      CODE_SIGNING_ALLOWED=NO \
      CODE_SIGNING_REQUIRED=NO \
      build
fi
