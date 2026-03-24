# X1 BOX iOS vcpkg Triplets

These overlay triplets define the iOS/iPadOS dependency prefixes used by the
embedded-core pipeline.

Included triplets:

- `arm64-ios-release`
- `arm64-ios-sim-release`
- `x64-ios-sim-release`

Each triplet:

- targets `iOS`
- uses static libraries
- builds release-only packages
- pins the Apple SDK selection through `VCPKG_OSX_SYSROOT`

The workflow [build-ios-deps.yml](C:\Users\AlejandroMazabuel\Documents\New project\.github\workflows\build-ios-deps.yml) and script [build-x1box-ios-deps.sh](C:\Users\AlejandroMazabuel\Documents\New project\ios-app\scripts\build-x1box-ios-deps.sh) consume these triplets when generating the `x1box-ios-deps` artifact.
