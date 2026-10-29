#!/usr/bin/env bash
# Builds the hakuX core (android/hakux/core, GPLv3) into libhakuxcore.so for
# the app's second, QEMU-free core (HakuxCoreActivity, process :hakux).
#
# The core is its own CMake project, not part of src/main/cpp: it brings its
# own glslang, volk and FEX, whose targets would clash with libxemu's. Gradle
# runs this before it merges native libraries (task buildHakuxCore) and packs
# the result from build/hakux-jniLibs.
#
#   android/hakux/build-core.sh [Release|Debug]
#
# Environment:
#   ANDROID_NDK_HOME  the NDK (default: the app's ndkVersion under ~/Android/Sdk)
#   X1BOX_FEX_DIR     a FEX checkout to build instead of the pinned fork (a
#                     working copy of x1box-haku, while changing FEX itself)
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
android=$(cd "$here/.." && pwd)
build_type=${1:-Release}
ndk=${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/30.0.15729638}
deps=$here/.deps
build=$here/.build/$build_type
out=$android/app/build/hakux-jniLibs/arm64-v8a

# Third-party sources at the revisions hakuX v1.0.0 pins (android/README.md
# there); SDL is the app's own version, so libhakuxcore.so runs against the
# libSDL3.so libxemu ships with and the one Java glue layer.
fetch() { # <dir> <url> <rev>
  local dir=$deps/$1
  if [ ! -d "$dir/.git" ]; then
    git clone -q "$2" "$dir"
  fi
  if [ "$(git -C "$dir" rev-parse HEAD)" != "$(git -C "$dir" rev-parse "$3^{commit}" 2>/dev/null)" ]; then
    git -C "$dir" fetch -q --tags origin || true
    git -C "$dir" checkout -q "$3"
  fi
  git -C "$dir" submodule update -q --init --recursive
}
mkdir -p "$deps/root/subprojects"
fetch volk https://github.com/zeux/volk.git 0b17a763ba5643e32da1b2152f8140461b3b7345
fetch glslang https://github.com/KhronosGroup/glslang.git 16.1.0
fetch spirv_reflect https://github.com/KhronosGroup/SPIRV-Reflect.git vulkan-sdk-1.4.328.0
fetch vma https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git v3.3.0
fetch SDL3 https://github.com/libsdl-org/SDL.git release-3.4.14
fetch adrenotools https://github.com/bylaws/libadrenotools.git 8fae8ce
# hakuX's kgsl draw context priority hook (Turnip above the compositor's
# GPU composition); libxemu's copy gets the same patch, and its hook
# libraries are the ones the app ships
kgsl_patch=$here/patches/adrenotools-kgsl-context-priority.patch
if ! git -C "$deps/adrenotools" apply --reverse --check "$kgsl_patch" 2>/dev/null; then
  git -C "$deps/adrenotools" apply "$kgsl_patch"
fi
# CHD disc images (x1box_chd.c), the revision libxemu's CMakeLists.txt pins
fetch libchdr https://github.com/rtissera/libchdr.git 47d794151b2b64f6aba60304e6480f4f5a2bc24b
fetch root/subprojects/nv2a_vsh_cpu https://github.com/xemu-project/nv2a_vsh_cpu.git 1115255708c10c4841b65dcd2223262e7a316598

# FEX: our fork with the system-mode API (branch x1box-haku), pinned. Fetched
# shallow (vixl's full history alone is hundreds of MB) and retried, since a
# dropped connection on a big fetch is common; its submodules but the test
# binaries, which are large and not built here.
fex_url=https://github.com/izzy2lost/FEX.git
fex_rev=5a1e724ba0ea97f1e549e6ac0842e9507529101f
retry() {
  local i
  for i in 1 2 3; do
    "$@" && return 0
    echo "build-core.sh: '$*' failed (try $i of 3)" >&2
    sleep 5
  done
  return 1
}
if [ -n "${X1BOX_FEX_DIR:-}" ]; then
  fex=$X1BOX_FEX_DIR
else
  fex=$deps/FEX
  if [ ! -d "$fex/.git" ]; then
    git init -q "$fex"
    git -C "$fex" remote add origin "$fex_url"
  fi
  if [ "$(git -C "$fex" rev-parse HEAD 2>/dev/null)" != "$fex_rev" ]; then
    retry git -C "$fex" fetch -q --depth 1 origin "$fex_rev"
    git -C "$fex" checkout -q "$fex_rev"
  fi
  retry git -C "$fex" submodule update -q --init --recursive --depth 1 -- \
    $(git -C "$fex" config -f .gitmodules --get-regexp '^submodule\..*\.path$' |
      awk '$2 !~ /^External\/fex-.*-bins$/ { print $2 }')
  # a fetch cut off mid-checkout leaves a submodule at the right commit with
  # its files missing, which the update above does not repair
  git -C "$fex" submodule foreach -q --recursive git reset -q --hard
fi
if [ ! -f "$fex/FEXCore/include/FEXCore/Core/Context.h" ] ||
   ! grep -q EnableSystemOps "$fex/FEXCore/include/FEXCore/Core/Context.h"; then
  echo "build-core.sh: no FEX fork with the system-mode API at $fex" >&2
  exit 1
fi

# configured once, and again when it is pointed at another FEX
if [ ! -f "$build/build.ninja" ] ||
   [ "$(sed -n 's/^HAKUX_FEX_DIR:[A-Z]*=//p' "$build/CMakeCache.txt")" != "$fex" ]; then
  cmake -S "$here/core/native" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
    -DCMAKE_BUILD_TYPE="$build_type" \
    -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DHAKUX_FEX_DIR="$fex" -DNC_DEPS="$deps"
fi
ninja -C "$build" hakuxcore

mkdir -p "$out"
strip=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
# Only the core: libSDL3.so and adrenotools' hook libraries come from libxemu's build.
"$strip" -o "$out/libhakuxcore.so.tmp" "$build/libhakuxcore.so"
mv "$out/libhakuxcore.so.tmp" "$out/libhakuxcore.so"
ls -la "$out/libhakuxcore.so"
