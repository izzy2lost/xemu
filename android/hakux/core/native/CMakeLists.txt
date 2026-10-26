# hakuX native core (QEMU-free): FEXCore fork + reimplemented Xbox kernel.
#
# hakux_native is the core library; xboxrun runs a title from the command
# line on the device (adb shell) for the frontier loop. Cross-compiled with
# the NDK into build/native/core; the steps are in android/README.md.
cmake_minimum_required(VERSION 3.20)
project(hakux_native C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ThinLTO for Release builds of everything but FEX (its own ENABLE_LTO stays
# off, fex.cmake): the decoder and the renderer ~2% lighter (the RP5, DOA2).
# The cache keeps relinking after a change to the files changed.
option(HAKUX_LTO "ThinLTO in Release builds (FEX excepted)" ON)
if(HAKUX_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT nc_ipo LANGUAGES C CXX)
  if(nc_ipo)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    add_link_options("-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/lto-cache")
  endif()
endif()

get_filename_component(NC_REPO "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/fex.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/hle.cmake")

add_library(hakux_native STATIC
  core/log.c
  core/jmp.c
  core/vm.c
  core/xdvdfs.c
  core/xbe_load.c
  core/mods.c
  core/cheats.c
  core/memscan.c
  core/core.c
  core/mmio.c
  core/irq.c
  dev/nv2a.c
  dev/ports.c
  dev/usb.c
  dev/usb_hub.c
  dev/xid.c
  dev/ohci.c
  dev/aci.c
  dev/nvnet.c
  kernel/kernel.c
  kernel/pool.c
  kernel/ob.c
  kernel/ps.c
  kernel/ke.c
  kernel/mm.c
  kernel/paging.c
  kernel/ex.c
  kernel/rtl.c
  kernel/seh.c
  kernel/fixups.c
  kernel/x86mmio.c
  kernel/timer.c
  kernel/intr.c
  kernel/apc.c
  kernel/io.c
  kernel/xc.c
  kernel/xc_crypto.c
  kernel/kstate.c
  state/state.c
  state/vm_state.c
  state/save.c
  state/cache.c
  cpu/cpu.cpp
  "${NC_REPO}/hw/xbox/hle/core/xbe.c"
)
target_include_directories(hakux_native PUBLIC "${NC_REPO}")
# xxhash (single header, FEX's copy): the savestates' blob store (state/cache.c)
target_include_directories(hakux_native PRIVATE "${HAKUX_FEX_DIR}/External/xxhash")
# The HLE layer's pure modules build without QEMU in this mode.
target_compile_definitions(hakux_native PRIVATE HLE_HOST_TEST)
target_link_libraries(hakux_native PUBLIC nc_fex hakux_hle hakux_apu log z)
target_compile_options(hakux_native PRIVATE -Wall -Wno-unused-parameter)

add_executable(xboxrun run/xboxrun.c run/capture.c)
target_link_libraries(xboxrun PRIVATE hakux_native hakux_hle z)
target_link_libraries(hakux_hle PUBLIC hakux_native)

# The hakuX app's native core library (native/app, cmake/app.cmake).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/app.cmake")
