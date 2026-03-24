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

    if(x1box_libslirp_meson_raw MATCHES "if host_system != 'ios'")
        message(STATUS "libslirp iOS test binaries already disabled")
    else()
    file(STRINGS "${x1box_libslirp_meson}" x1box_libslirp_meson_lines)

    set(x1box_pingtest_start "pingtest = executable('pingtest', 'test/pingtest.c',")
    set(x1box_ncsi_end "test('ncsi', ncsitest)")
    set(x1box_injected_guard FALSE)
    set(x1box_inserted_end FALSE)
    set(x1box_rewritten_lines)

    foreach(x1box_line IN LISTS x1box_libslirp_meson_lines)
        if(x1box_line STREQUAL "if host_system != 'ios'")
            set(x1box_injected_guard TRUE)
        endif()
        if(x1box_line STREQUAL "${x1box_pingtest_start}" AND NOT x1box_injected_guard)
            list(APPEND x1box_rewritten_lines "if host_system != 'ios'")
            set(x1box_injected_guard TRUE)
        endif()

        list(APPEND x1box_rewritten_lines "${x1box_line}")

        if(x1box_line STREQUAL "${x1box_ncsi_end}" AND x1box_injected_guard AND NOT x1box_inserted_end)
            list(APPEND x1box_rewritten_lines "endif")
            set(x1box_inserted_end TRUE)
        endif()
    endforeach()

    if(x1box_injected_guard AND x1box_inserted_end)
        string(JOIN "\n" x1box_libslirp_meson_contents ${x1box_rewritten_lines})
        string(APPEND x1box_libslirp_meson_contents "\n")
        file(WRITE "${x1box_libslirp_meson}" "${x1box_libslirp_meson_contents}")
    else()
        message(FATAL_ERROR "Failed to locate libslirp pingtest block for iOS guard insertion")
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
