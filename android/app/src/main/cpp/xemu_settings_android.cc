#include "qemu/osdep.h"

#include <SDL_filesystem.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <toml++/toml.h>
#include <android/log.h>

#include <sstream>
#include <string>

#include "xemu-settings.h"

struct config g_config;

static const char *settings_path;
static const char *filename = "xemu.toml";
static std::string error_msg;

static char *xemu_strdup_or_null(const char *value)
{
    if (!value || *value == '\0') {
        return NULL;
    }
    return strdup(value);
}

static void xemu_settings_apply_defaults(void)
{
    memset(&g_config, 0, sizeof(g_config));

    g_config.general.show_welcome = true;
    g_config.general.updates.check = true;
    g_config.general.skip_boot_anim = false;
    g_config.general.last_viewed_menu_index = 0;

    g_config.input.auto_bind = true;
    g_config.input.allow_vibration = true;
    g_config.input.background_input_capture = false;
    g_config.input.keyboard_controller_scancode_map.a = 4;
    g_config.input.keyboard_controller_scancode_map.b = 5;
    g_config.input.keyboard_controller_scancode_map.x = 27;
    g_config.input.keyboard_controller_scancode_map.y = 28;
    g_config.input.keyboard_controller_scancode_map.dpad_left = 80;
    g_config.input.keyboard_controller_scancode_map.dpad_up = 82;
    g_config.input.keyboard_controller_scancode_map.dpad_right = 79;
    g_config.input.keyboard_controller_scancode_map.dpad_down = 81;
    g_config.input.keyboard_controller_scancode_map.back = 42;
    g_config.input.keyboard_controller_scancode_map.start = 40;
    g_config.input.keyboard_controller_scancode_map.white = 30;
    g_config.input.keyboard_controller_scancode_map.black = 31;
    g_config.input.keyboard_controller_scancode_map.lstick_btn = 32;
    g_config.input.keyboard_controller_scancode_map.rstick_btn = 33;
    g_config.input.keyboard_controller_scancode_map.guide = 34;
    g_config.input.keyboard_controller_scancode_map.lstick_up = 8;
    g_config.input.keyboard_controller_scancode_map.lstick_left = 22;
    g_config.input.keyboard_controller_scancode_map.lstick_right = 9;
    g_config.input.keyboard_controller_scancode_map.lstick_down = 7;
    g_config.input.keyboard_controller_scancode_map.ltrigger = 26;
    g_config.input.keyboard_controller_scancode_map.rstick_up = 12;
    g_config.input.keyboard_controller_scancode_map.rstick_left = 13;
    g_config.input.keyboard_controller_scancode_map.rstick_right = 15;
    g_config.input.keyboard_controller_scancode_map.rstick_down = 14;
    g_config.input.keyboard_controller_scancode_map.rtrigger = 18;

    g_config.display.renderer = CONFIG_DISPLAY_RENDERER_VULKAN;
    g_config.display.filtering = CONFIG_DISPLAY_FILTERING_NEAREST;
    g_config.display.quality.surface_scale = 1;
    g_config.display.window.fullscreen_on_startup = false;
    g_config.display.window.fullscreen_exclusive = false;
    g_config.display.window.startup_size =
        CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_1280X960;
    g_config.display.window.last_width = 640;
    g_config.display.window.last_height = 480;
    g_config.display.window.vsync = false;
    g_config.display.ui.show_menubar = true;
    g_config.display.ui.show_notifications = true;
    g_config.display.ui.hide_cursor = true;
    g_config.display.ui.use_animations = true;
    g_config.display.ui.fit = CONFIG_DISPLAY_UI_FIT_SCALE;
    g_config.display.ui.aspect_ratio = CONFIG_DISPLAY_UI_ASPECT_RATIO_AUTO;
    g_config.display.ui.scale = 1;
    g_config.display.ui.auto_scale = true;
    g_config.display.setup_nvidia_profile = true;

    g_config.audio.vp.num_workers = 0;
    g_config.audio.use_dsp = false;
    g_config.audio.hrtf = true;
    g_config.audio.volume_limit = 1.0;

    g_config.net.enable = false;
    g_config.net.backend = CONFIG_NET_BACKEND_NAT;
    g_config.net.udp.bind_addr = xemu_strdup_or_null("0.0.0.0:9368");
    g_config.net.udp.remote_addr = xemu_strdup_or_null("1.2.3.4:9368");

    g_config.sys.mem_limit = CONFIG_SYS_MEM_LIMIT_64;
    g_config.sys.avpack = CONFIG_SYS_AVPACK_HDTV;

    g_config.perf.hard_fpu = true;
    g_config.perf.cache_shaders = true;
}

// Optimized parsers - avoid string allocations
static bool parse_renderer(const std::string &value, CONFIG_DISPLAY_RENDERER *out)
{
    if (value.size() == 6 && (value == "opengl" || value == "OpenGL" || value == "OPENGL")) {
        *out = CONFIG_DISPLAY_RENDERER_OPENGL;
        return true;
    }
    if (value.size() == 2 && (value == "gl" || value == "GL")) {
        *out = CONFIG_DISPLAY_RENDERER_OPENGL;
        return true;
    }
    if (value.size() == 6 && (value == "vulkan" || value == "Vulkan" || value == "VULKAN")) {
        *out = CONFIG_DISPLAY_RENDERER_VULKAN;
        return true;
    }
    if (value.size() == 2 && (value == "vk" || value == "VK")) {
        *out = CONFIG_DISPLAY_RENDERER_VULKAN;
        return true;
    }
    if ((value.size() == 4 && (value == "null" || value == "NULL" || value == "Null" || value == "none" || value == "NONE" || value == "None"))) {
        *out = CONFIG_DISPLAY_RENDERER_NULL;
        return true;
    }
    return false;
}

static bool parse_filtering(const std::string &value, CONFIG_DISPLAY_FILTERING *out)
{
    if (value.size() == 6 && (value == "linear" || value == "Linear" || value == "LINEAR")) {
        *out = CONFIG_DISPLAY_FILTERING_LINEAR;
        return true;
    }
    if (value.size() == 7 && (value == "nearest" || value == "Nearest" || value == "NEAREST")) {
        *out = CONFIG_DISPLAY_FILTERING_NEAREST;
        return true;
    }
    return false;
}

const char *xemu_settings_get_error_message(void)
{
    return error_msg.empty() ? NULL : error_msg.c_str();
}

void xemu_settings_set_path(const char *path)
{
    if (settings_path) {
        return;
    }
    settings_path = path;
}

const char *xemu_settings_get_base_path(void)
{
    static const char *base_path = NULL;
    if (base_path) {
        return base_path;
    }

    char *base = SDL_GetPrefPath("xemu", "xemu");
    if (!base) {
        base = SDL_GetBasePath();
    }
    base_path = base ? strdup(base) : strdup("");
    SDL_free(base);
    return base_path;
}

const char *xemu_settings_get_path(void)
{
    if (settings_path != NULL) {
        return settings_path;
    }

    const char *base = xemu_settings_get_base_path();
    settings_path = g_strdup_printf("%s%s", base, filename);
    return settings_path;
}

const char *xemu_settings_get_default_eeprom_path(void)
{
    static char *eeprom_path = NULL;
    if (eeprom_path != NULL) {
        return eeprom_path;
    }

    const char *base = xemu_settings_get_base_path();
    eeprom_path = g_strdup_printf("%s%s", base, "eeprom.bin");
    return eeprom_path;
}

bool xemu_settings_load(void)
{
    xemu_settings_apply_defaults();
    error_msg.clear();
    setenv("XEMU_ANDROID_FORCE_CPU_BLIT", "0", 1);

    const char *path = xemu_settings_get_path();
    if (!path || *path == '\0') {
        return true;
    }

    if (qemu_access(path, F_OK) == -1) {
        return true;
    }

    try {
        toml::table tbl = toml::parse_file(path);

        // Cache table lookups to avoid repeated traversal
        auto general = tbl["general"];
        auto display = tbl["display"];
        auto display_window = display["window"];
        auto perf = tbl["perf"];
        auto android_cfg = tbl["android"];
        auto sys = tbl["sys"];
        auto sys_files = sys["files"];

        // General settings
        if (auto show_welcome = general["show_welcome"].value<bool>()) {
            g_config.general.show_welcome = *show_welcome;
        }

        // Display settings - force Vulkan on Android
        g_config.display.renderer = CONFIG_DISPLAY_RENDERER_VULKAN;
        if (auto renderer = display["renderer"].value<std::string>()) {
            CONFIG_DISPLAY_RENDERER parsed;
            if (parse_renderer(*renderer, &parsed) && parsed != CONFIG_DISPLAY_RENDERER_VULKAN) {
                __android_log_print(ANDROID_LOG_WARN, "xemu-android",
                                    "Config display.renderer=%s requested, but Android forces Vulkan",
                                    renderer->c_str());
            }
        }

        if (auto filtering = display["filtering"].value<std::string>()) {
            CONFIG_DISPLAY_FILTERING parsed;
            if (parse_filtering(*filtering, &parsed)) {
                g_config.display.filtering = parsed;
            }
        }

        if (auto vsync = display_window["vsync"].value<bool>()) {
            g_config.display.window.vsync = *vsync;
        }

        // Performance settings
        if (auto hard_fpu = perf["hard_fpu"].value<bool>()) {
            g_config.perf.hard_fpu = *hard_fpu;
        }
        if (auto cache_shaders = perf["cache_shaders"].value<bool>()) {
            g_config.perf.cache_shaders = *cache_shaders;
        }

        // Android-specific settings
        if (auto egl_offscreen = android_cfg["egl_offscreen"].value<bool>()) {
            if (!*egl_offscreen) {
                setenv("XEMU_ANDROID_EGL_OFFSCREEN", "0", 1);
            }
        }

        // System file paths
        if (auto bootrom = sys_files["bootrom_path"].value<std::string>()) {
            xemu_settings_set_string(&g_config.sys.files.bootrom_path, bootrom->c_str());
        }
        if (auto flashrom = sys_files["flashrom_path"].value<std::string>()) {
            xemu_settings_set_string(&g_config.sys.files.flashrom_path, flashrom->c_str());
        }
        if (auto hdd = sys_files["hdd_path"].value<std::string>()) {
            xemu_settings_set_string(&g_config.sys.files.hdd_path, hdd->c_str());
        }
        if (auto dvd = sys_files["dvd_path"].value<std::string>()) {
            xemu_settings_set_string(&g_config.sys.files.dvd_path, dvd->c_str());
        }
        if (auto eeprom = sys_files["eeprom_path"].value<std::string>()) {
            xemu_settings_set_string(&g_config.sys.files.eeprom_path, eeprom->c_str());
        }
    } catch (const toml::parse_error &err) {
        std::ostringstream oss;
        oss << "Error parsing config file at " << err.source().begin << ":\n"
            << "    " << err.description() << "\n";
        error_msg = oss.str();
        return false;
    }
    return true;
}

void xemu_settings_save(void)
{
}

void add_net_nat_forward_ports(int host, int guest,
                               CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL protocol)
{
    (void)host;
    (void)guest;
    (void)protocol;
}

void remove_net_nat_forward_ports(unsigned int index)
{
    (void)index;
}

bool xemu_settings_load_gamepad_mapping(const char *guid,
                                        GamepadMappings **mapping)
{
    (void)guid;
    *mapping = NULL;
    return true;
}

void xemu_settings_reset_controller_mapping(const char *guid)
{
    (void)guid;
}

void xemu_settings_reset_keyboard_mapping(void)
{
}
