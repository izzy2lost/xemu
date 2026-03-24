# X1 BOX iOS vcpkg Overlay Ports

This directory contains targeted port overrides used only by the iOS dependency
pipeline.

Current overrides:

- `libslirp`: disables upstream test executables for Apple mobile builds, where
  the static library is useful but the test binaries fail to link against iOS
  resolver symbols we do not need in the shipped app.
