vcpkg_from_gitlab(
    GITLAB_URL https://gitlab.freedesktop.org/
    OUT_SOURCE_PATH SOURCE_PATH
    REPO slirp/libslirp
    REF "v${VERSION}"
    SHA512 cdb66f6280a9982de3c32269aee352bdf225db918590255abaed9bcd0aee4e996d2d8c2c3f62473f57485603ec29fd35723b0649d3ec3c41cc28b22ce913f63b
    HEAD_REF master
)

if(VCPKG_TARGET_IS_IOS)
    set(x1box_libslirp_meson "${SOURCE_PATH}/meson.build")
    file(READ "${x1box_libslirp_meson}" x1box_libslirp_meson_raw)
    set(x1box_pingtest_start "pingtest = executable('pingtest', 'test/pingtest.c',")
    set(x1box_ncsi_end "test('ncsi', ncsitest)")

    if(x1box_libslirp_meson_raw MATCHES "if host_system != 'ios'")
        message(STATUS "libslirp iOS test binaries already disabled")
    else()
        if(NOT x1box_libslirp_meson_raw MATCHES "pingtest = executable\\('pingtest', 'test/pingtest\\.c',")
            message(FATAL_ERROR "Failed to locate libslirp pingtest block for iOS guard insertion")
        endif()
        if(NOT x1box_libslirp_meson_raw MATCHES "test\\('ncsi', ncsitest\\)")
            message(FATAL_ERROR "Failed to locate libslirp ncsi test terminator for iOS guard insertion")
        endif()

        string(REPLACE "${x1box_pingtest_start}" "if host_system != 'ios'\n${x1box_pingtest_start}" x1box_libslirp_meson_raw "${x1box_libslirp_meson_raw}")
        string(REPLACE "${x1box_ncsi_end}" "${x1box_ncsi_end}\nendif" x1box_libslirp_meson_raw "${x1box_libslirp_meson_raw}")
        file(WRITE "${x1box_libslirp_meson}" "${x1box_libslirp_meson_raw}")
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
