vcpkg_from_gitlab(
    GITLAB_URL https://gitlab.freedesktop.org/
    OUT_SOURCE_PATH SOURCE_PATH
    REPO slirp/libslirp
    REF "v${VERSION}"
    SHA512 cdb66f6280a9982de3c32269aee352bdf225db918590255abaed9bcd0aee4e996d2d8c2c3f62473f57485603ec29fd35723b0649d3ec3c41cc28b22ce913f63b
    HEAD_REF master
)

if(VCPKG_TARGET_IS_IOS)
    set(x1box_ios_test_block [=[
pingtest = executable('pingtest', 'test/pingtest.c',
link_with: [ lib ],
c_args : cargs,
link_args : vflag,
include_directories: [ 'src' ],
dependencies : libslirp_deps
)
test('ping', pingtest)
ncsitest = executable('ncsitest', 'test/ncsitest.c',
link_with: [lib],
c_args : cargs,
link_args : vflag,
include_directories: ['src'],
dependencies: libslirp_deps
)
test('ncsi', ncsitest)]=])

    set(x1box_ios_wrapped_test_block [=[
if host_system != 'ios'
pingtest = executable('pingtest', 'test/pingtest.c',
link_with: [ lib ],
c_args : cargs,
link_args : vflag,
include_directories: [ 'src' ],
dependencies : libslirp_deps
)
test('ping', pingtest)
ncsitest = executable('ncsitest', 'test/ncsitest.c',
link_with: [lib],
c_args : cargs,
link_args : vflag,
include_directories: ['src'],
dependencies: libslirp_deps
)
test('ncsi', ncsitest)
endif]=])

    set(x1box_libslirp_meson "${SOURCE_PATH}/meson.build")
    file(READ "${x1box_libslirp_meson}" x1box_libslirp_meson_contents)

    if(x1box_libslirp_meson_contents MATCHES "host_system != 'ios'")
        message(STATUS "libslirp iOS test binaries already disabled")
    else()
        string(REPLACE "${x1box_ios_test_block}" "${x1box_ios_wrapped_test_block}" x1box_libslirp_meson_contents "${x1box_libslirp_meson_contents}")

        if(x1box_libslirp_meson_contents STREQUAL "")
            message(FATAL_ERROR "libslirp meson.build replacement unexpectedly produced empty output")
        endif()

        file(WRITE "${x1box_libslirp_meson}" "${x1box_libslirp_meson_contents}")
        file(READ "${x1box_libslirp_meson}" x1box_libslirp_meson_verify)
        if(NOT x1box_libslirp_meson_verify MATCHES "if host_system != 'ios'")
            message(FATAL_ERROR "Failed to disable libslirp test executables for iOS")
        endif()
    endif()
endif()

if(VCPKG_HOST_IS_WINDOWS)
    vcpkg_acquire_msys(MSYS_ROOT)
    vcpkg_add_to_path("${MSYS_ROOT}/usr/bin")
endif()

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_install_meson(ADD_BIN_TO_PATH)

vcpkg_fixup_pkgconfig()

vcpkg_copy_pdbs()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYRIGHT")
