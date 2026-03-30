/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "ui/xemu-settings.h"
#include "renderer.h"
#include "xemu-version.h"

#ifdef __ANDROID__
#include <android/log.h>
#include <dlfcn.h>
#endif
#include <volk.h>

#ifdef __ANDROID__
static void *g_custom_vk_handle = NULL;

static bool android_try_load_custom_vulkan(void)
{
    const char *path = getenv("XEMU_VULKAN_DRIVER");
    if (!path || path[0] == '\0') {
        return false;
    }
    g_custom_vk_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!g_custom_vk_handle) {
        fprintf(stderr, "Custom Vulkan driver dlopen failed: %s\n", dlerror());
        return false;
    }
    PFN_vkGetInstanceProcAddr proc =
        (PFN_vkGetInstanceProcAddr)dlsym(g_custom_vk_handle, "vkGetInstanceProcAddr");
    if (!proc) {
        fprintf(stderr, "Custom Vulkan driver: vkGetInstanceProcAddr not found\n");
        dlclose(g_custom_vk_handle);
        g_custom_vk_handle = NULL;
        return false;
    }
    volkInitializeCustom(proc);
    fprintf(stderr, "Custom Vulkan driver loaded: %s\n", path);
    return true;
}
#endif

#define VkExtensionPropertiesArray GArray
#define StringArray GArray

static bool enable_validation = false;

static char const *const validation_layers[] = {
    "VK_LAYER_KHRONOS_validation",
};

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData, void *pUserData)
{
    fprintf(stderr, "[vk] %s\n", pCallbackData->pMessage);

    if ((messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) &&
        (messageSeverity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT))) {
        assert(!g_config.display.vulkan.assert_on_validation_msg);
    }
    return VK_FALSE;
}

static bool check_validation_layer_support(void)
{
    uint32_t num_available_layers;
    vkEnumerateInstanceLayerProperties(&num_available_layers, NULL);

    g_autofree VkLayerProperties *available_layers =
        g_malloc_n(num_available_layers, sizeof(VkLayerProperties));
    vkEnumerateInstanceLayerProperties(&num_available_layers, available_layers);

    for (int i = 0; i < ARRAY_SIZE(validation_layers); i++) {
        bool found = false;
        for (int j = 0; j < num_available_layers; j++) {
            if (!strcmp(validation_layers[i], available_layers[j].layerName)) {
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "desired validation layer not found: %s\n",
                    validation_layers[i]);
            return false;
        }
    }

    return true;
}

static VkExtensionPropertiesArray *
get_available_instance_extensions(PGRAPHState *pg)
{
    uint32_t num_extensions = 0;

    VK_CHECK(
        vkEnumerateInstanceExtensionProperties(NULL, &num_extensions, NULL));

    VkExtensionPropertiesArray *extensions = g_array_sized_new(
        FALSE, FALSE, sizeof(VkExtensionProperties), num_extensions);

    g_array_set_size(extensions, num_extensions);
    VK_CHECK(vkEnumerateInstanceExtensionProperties(
        NULL, &num_extensions, (VkExtensionProperties *)extensions->data));

    return extensions;
}

static bool
is_extension_available(VkExtensionPropertiesArray *available_extensions,
                       const char *extension_name)
{
    for (int i = 0; i < available_extensions->len; i++) {
        VkExtensionProperties *e =
            &g_array_index(available_extensions, VkExtensionProperties, i);
        if (!strcmp(e->extensionName, extension_name)) {
            return true;
        }
    }

    return false;
}

static bool
add_extension_if_available(VkExtensionPropertiesArray *available_extensions,
                           StringArray *enabled_extension_names,
                           const char *desired_extension_name)
{
    if (is_extension_available(available_extensions, desired_extension_name)) {
        g_array_append_val(enabled_extension_names, desired_extension_name);
        return true;
    }

    fprintf(stderr, "Warning: extension not available: %s\n",
            desired_extension_name);
    return false;
}

static void
add_optional_instance_extension_names(PGRAPHState *pg,
                                      VkExtensionPropertiesArray *available_extensions,
                                      StringArray *enabled_extension_names)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->debug_utils_extension_enabled =
        g_config.display.vulkan.validation_layers &&
        add_extension_if_available(available_extensions, enabled_extension_names,
                                   VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
}

static bool create_instance(PGRAPHState *pg, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    VkResult result;

#ifdef __ANDROID__
    if (!android_try_load_custom_vulkan()) {
        result = volkInitialize();
    } else {
        result = VK_SUCCESS;
    }
#else
    result = volkInitialize();
#endif
    if (result != VK_SUCCESS) {
        error_setg(errp, "volkInitialize failed");
        return false;
    }

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "xemu",
        .applicationVersion = VK_MAKE_VERSION(
            xemu_version_major, xemu_version_minor, xemu_version_patch),
        .pEngineName = "No Engine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_1,
    };

    g_autoptr(VkExtensionPropertiesArray) available_extensions =
        get_available_instance_extensions(pg);

    g_autoptr(StringArray) enabled_extension_names =
        g_array_new(FALSE, FALSE, sizeof(char *));

    add_optional_instance_extension_names(pg, available_extensions,
                                          enabled_extension_names);

    const char *const *enabled_instance_extension_names = NULL;
    if (enabled_extension_names->len > 0) {
        enabled_instance_extension_names =
            &g_array_index(enabled_extension_names, const char *, 0);
    }

    fprintf(stderr, "Enabled instance extensions:\n");
    for (int i = 0; i < enabled_extension_names->len; i++) {
        fprintf(stderr, "- %s\n",
                g_array_index(enabled_extension_names, char *, i));
    }

    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = enabled_extension_names->len,
        .ppEnabledExtensionNames = enabled_instance_extension_names,
    };

    enable_validation = g_config.display.vulkan.validation_layers;

    VkValidationFeatureEnableEXT enables[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
        // VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
    };

    VkValidationFeaturesEXT validationFeatures = {
        .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .enabledValidationFeatureCount = ARRAY_SIZE(enables),
        .pEnabledValidationFeatures = enables,
    };

    if (enable_validation) {
        if (check_validation_layer_support()) {
            fprintf(stderr, "Warning: Validation layers enabled. Expect "
                            "performance impact.\n");
            create_info.enabledLayerCount = ARRAY_SIZE(validation_layers);
            create_info.ppEnabledLayerNames = validation_layers;
            create_info.pNext = &validationFeatures;
        } else {
            fprintf(stderr, "Warning: validation layers not available\n");
            enable_validation = false;
        }
    }

    result = vkCreateInstance(&create_info, NULL, &r->instance);
    if (result != VK_SUCCESS) {
        error_setg(errp, "Failed to create instance (%d)", result);
        return false;
    }

    volkLoadInstance(r->instance);

    if (r->debug_utils_extension_enabled) {
        VkDebugUtilsMessengerCreateInfoEXT messenger_info = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debugCallback,
        };
        VK_CHECK(vkCreateDebugUtilsMessengerEXT(r->instance, &messenger_info,
                                                NULL, &r->debug_messenger));
    }

    return true;
}

static bool is_queue_family_indicies_complete(QueueFamilyIndices indices)
{
    return indices.queue_family >= 0;
}

QueueFamilyIndices pgraph_vk_find_queue_families(VkPhysicalDevice device)
{
    QueueFamilyIndices indices = {
        .queue_family = -1,
    };

    uint32_t num_queue_families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &num_queue_families, NULL);

    g_autofree VkQueueFamilyProperties *queue_families =
        g_malloc_n(num_queue_families, sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(device, &num_queue_families,
                                             queue_families);

    for (int i = 0; i < num_queue_families; i++) {
        VkQueueFamilyProperties queueFamily = queue_families[i];
        // FIXME: Support independent graphics, compute queues
        int required_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((queueFamily.queueFlags & required_flags) == required_flags) {
            indices.queue_family = i;
        }
        if (is_queue_family_indicies_complete(indices)) {
            break;
        }
    }

    return indices;
}

static VkExtensionPropertiesArray *
get_available_device_extensions(VkPhysicalDevice device)
{
    uint32_t num_extensions = 0;

    VK_CHECK(vkEnumerateDeviceExtensionProperties(device, NULL, &num_extensions,
                                                  NULL));

    VkExtensionPropertiesArray *extensions = g_array_sized_new(
        FALSE, FALSE, sizeof(VkExtensionProperties), num_extensions);

    g_array_set_size(extensions, num_extensions);
    VK_CHECK(vkEnumerateDeviceExtensionProperties(
        device, NULL, &num_extensions,
        (VkExtensionProperties *)extensions->data));

    return extensions;
}

static StringArray *get_required_device_extension_names(void)
{
    StringArray *extensions = g_array_sized_new(FALSE, FALSE, sizeof(char *), 2);

#ifdef WIN32
    static char const *const required_device_extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
    };
    g_array_append_vals(extensions, required_device_extensions,
                        ARRAY_SIZE(required_device_extensions));
#elif HAVE_EXTERNAL_MEMORY
    static char const *const required_device_extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    };
    g_array_append_vals(extensions, required_device_extensions,
                        ARRAY_SIZE(required_device_extensions));
#endif

    return extensions;
}

static void add_optional_device_extension_names(
    PGRAPHState *pg, VkExtensionPropertiesArray *available_extensions,
    StringArray *enabled_extension_names)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->custom_border_color_extension_enabled =
        add_extension_if_available(available_extensions, enabled_extension_names,
                                   VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);

    r->memory_budget_extension_enabled = add_extension_if_available(
        available_extensions, enabled_extension_names,
        VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
}

static bool check_device_support_required_extensions(VkPhysicalDevice device)
{
    g_autoptr(VkExtensionPropertiesArray) available_extensions =
        get_available_device_extensions(device);

#if !(defined(WIN32) || HAVE_EXTERNAL_MEMORY)
    return true;
#else
#ifdef WIN32
    static char const *const required_device_extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
    };
#else
    static char const *const required_device_extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    };
#endif
    const int required_device_extensions_len =
        ARRAY_SIZE(required_device_extensions);

    for (int i = 0; i < required_device_extensions_len; i++) {
        if (!is_extension_available(available_extensions,
                                    required_device_extensions[i])) {
            fprintf(stderr, "required device extension not found: %s\n",
                    required_device_extensions[i]);
            return false;
        }
    }
#endif

    return true;
}

static void report_device_incompatibility(VkPhysicalDevice device,
                                          const char *reason)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);

    fprintf(stderr, "Vulkan device rejected (%s): %s\n", props.deviceName,
            reason);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_WARN, "xemu-android",
                        "Vulkan device rejected (%s): %s", props.deviceName,
                        reason);
#endif
}

#ifdef __ANDROID__
static void log_android_format_support_failure(const char *label,
                                               VkFormat format,
                                               VkImageUsageFlags usage,
                                               VkFormatFeatureFlags required,
                                               VkFormatProperties props,
                                               VkResult image_result)
{
    fprintf(stderr,
            "Android Vulkan format issue [%s]: format=%d usage=0x%x "
            "required_optimal=0x%x optimal=0x%x linear=0x%x buffer=0x%x "
            "imageFormatResult=%d\n",
            label, (int)format, (unsigned int)usage, (unsigned int)required,
            (unsigned int)props.optimalTilingFeatures,
            (unsigned int)props.linearTilingFeatures,
            (unsigned int)props.bufferFeatures, (int)image_result);
    __android_log_print(
        ANDROID_LOG_WARN, "xemu-android",
        "vk format issue [%s]: format=%d usage=0x%x required_optimal=0x%x "
        "optimal=0x%x linear=0x%x buffer=0x%x imageFormatResult=%d",
        label, (int)format, (unsigned int)usage, (unsigned int)required,
        (unsigned int)props.optimalTilingFeatures,
        (unsigned int)props.linearTilingFeatures,
        (unsigned int)props.bufferFeatures, (int)image_result);
}
#endif

static bool format_already_checked(const VkFormat *formats, size_t count,
                                   VkFormat format)
{
    for (size_t i = 0; i < count; i++) {
        if (formats[i] == format) {
            return true;
        }
    }

    return false;
}

static bool check_format_supports_features(VkPhysicalDevice device,
                                           VkFormat format,
                                           VkFormatFeatureFlags features)
{
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(device, format, &props);

    return (props.optimalTilingFeatures & features) == features;
}

static bool check_image_format_usage_supported(VkPhysicalDevice device,
                                               VkFormat format,
                                               VkImageUsageFlags usage)
{
    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
    };
    VkImageFormatProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
    };

    return vkGetPhysicalDeviceImageFormatProperties2(device, &info, &props) ==
           VK_SUCCESS;
}

static bool check_texture_formats_supported_internal(VkPhysicalDevice device,
                                                     bool report_failures)
{
    VkFormat checked_formats[ARRAY_SIZE(kelvin_color_format_vk_map)];
    size_t num_checked_formats = 0;
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    memset(checked_formats, 0, sizeof(checked_formats));

    for (int i = 0; i < ARRAY_SIZE(kelvin_color_format_vk_map); i++) {
        VkFormat format = kelvin_color_format_vk_map[i].vk_format;

        if (format == VK_FORMAT_UNDEFINED ||
            format_already_checked(checked_formats, num_checked_formats,
                                   format)) {
            continue;
        }

        checked_formats[num_checked_formats++] = format;

        if (!check_format_supports_features(device, format,
                                            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
            !check_image_format_usage_supported(device, format, usage)) {
            if (report_failures) {
                char reason[160];

                snprintf(reason, sizeof(reason),
                         "sampled texture uploads need VkFormat %d to support "
                         "optimal sampled images and transfer-dst usage",
                         format);
                report_device_incompatibility(device, reason);
            }
            return false;
        }
    }

    return true;
}

static bool check_texture_formats_supported(VkPhysicalDevice device)
{
    return check_texture_formats_supported_internal(device, true);
}

static bool check_surface_format_supported_internal(VkPhysicalDevice device,
                                                    const SurfaceFormatInfo *format,
                                                    bool report_failures,
                                                    const char *reason)
{
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              format->usage;

    bool supported =
        check_image_format_usage_supported(device, format->vk_format, usage) &&
        check_format_supports_features(device, format->vk_format,
                                       VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                       VK_FORMAT_FEATURE_BLIT_DST_BIT);
    if (!supported && report_failures) {
        report_device_incompatibility(device, reason);
    }
    return supported;
}

static bool check_surface_formats_supported_internal(VkPhysicalDevice device,
                                                     bool report_failures)
{
    VkFormat checked_formats[ARRAY_SIZE(kelvin_surface_color_format_vk_map)];
    size_t num_checked_formats = 0;

    memset(checked_formats, 0, sizeof(checked_formats));

    for (int i = 0; i < ARRAY_SIZE(kelvin_surface_color_format_vk_map); i++) {
        const SurfaceFormatInfo *format = &kelvin_surface_color_format_vk_map[i];

        if (!format->host_bytes_per_pixel ||
            format_already_checked(checked_formats, num_checked_formats,
                                   format->vk_format)) {
            continue;
        }

        checked_formats[num_checked_formats++] = format->vk_format;

        if (!check_surface_format_supported_internal(
                device, format, report_failures,
                "surface format needs sampled, transfer, attachment, and "
                "blit support")) {
            return false;
        }
    }

    if (!check_surface_format_supported_internal(
            device, &zeta_d16, report_failures,
            "Z16 surfaces need sampled, transfer, attachment, and blit support")) {
        return false;
    }

    if (!check_surface_format_supported_internal(
            device, &zeta_d24_unorm_s8_uint, false,
            "Z24S8 surfaces need either D24_UNORM_S8_UINT or "
            "D32_SFLOAT_S8_UINT with sampled, transfer, attachment, and blit "
            "support") &&
        !check_surface_format_supported_internal(
            device, &zeta_d32_sfloat_s8_uint, false,
            "Z24S8 surfaces need either D24_UNORM_S8_UINT or "
            "D32_SFLOAT_S8_UINT with sampled, transfer, attachment, and blit "
            "support")) {
        if (report_failures) {
            report_device_incompatibility(
                device,
                "Z24S8 surfaces need either D24_UNORM_S8_UINT or "
                "D32_SFLOAT_S8_UINT with sampled, transfer, attachment, and "
                "blit support");
        }
        return false;
    }

    return true;
}

static bool check_surface_formats_supported(VkPhysicalDevice device)
{
    return check_surface_formats_supported_internal(device, true);
}

#ifdef __ANDROID__
static void log_android_texture_format_diagnostics(VkPhysicalDevice device)
{
    VkFormat checked_formats[ARRAY_SIZE(kelvin_color_format_vk_map)];
    size_t num_checked_formats = 0;
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    memset(checked_formats, 0, sizeof(checked_formats));

    for (int i = 0; i < ARRAY_SIZE(kelvin_color_format_vk_map); i++) {
        VkFormat format = kelvin_color_format_vk_map[i].vk_format;
        VkFormatProperties props;
        VkResult image_result;
        bool features_ok;
        bool usage_ok;

        if (format == VK_FORMAT_UNDEFINED ||
            format_already_checked(checked_formats, num_checked_formats,
                                   format)) {
            continue;
        }

        checked_formats[num_checked_formats++] = format;
        vkGetPhysicalDeviceFormatProperties(device, format, &props);
        features_ok = (props.optimalTilingFeatures &
                       VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ==
                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

        VkPhysicalDeviceImageFormatInfo2 info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .format = format,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage,
        };
        VkImageFormatProperties2 image_props = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        };
        image_result = vkGetPhysicalDeviceImageFormatProperties2(
            device, &info, &image_props);
        usage_ok = (image_result == VK_SUCCESS);

        if (!features_ok || !usage_ok) {
            log_android_format_support_failure(
                "texture", format, usage, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
                props, image_result);
        }
    }
}

static void log_android_surface_format_diagnostics(VkPhysicalDevice device)
{
    VkFormat checked_formats[ARRAY_SIZE(kelvin_surface_color_format_vk_map)];
    size_t num_checked_formats = 0;
    const VkFormatFeatureFlags required_features =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;

    memset(checked_formats, 0, sizeof(checked_formats));

    for (int i = 0; i < ARRAY_SIZE(kelvin_surface_color_format_vk_map); i++) {
        const SurfaceFormatInfo *format = &kelvin_surface_color_format_vk_map[i];
        VkImageUsageFlags usage;
        VkFormatProperties props;
        VkResult image_result;
        bool features_ok;
        bool usage_ok;

        if (!format->host_bytes_per_pixel ||
            format_already_checked(checked_formats, num_checked_formats,
                                   format->vk_format)) {
            continue;
        }

        checked_formats[num_checked_formats++] = format->vk_format;
        usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | format->usage;
        vkGetPhysicalDeviceFormatProperties(device, format->vk_format, &props);
        features_ok =
            (props.optimalTilingFeatures & required_features) ==
            required_features;

        VkPhysicalDeviceImageFormatInfo2 info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .format = format->vk_format,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage,
        };
        VkImageFormatProperties2 image_props = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        };
        image_result = vkGetPhysicalDeviceImageFormatProperties2(
            device, &info, &image_props);
        usage_ok = (image_result == VK_SUCCESS);

        if (!features_ok || !usage_ok) {
            log_android_format_support_failure("surface-color",
                                               format->vk_format, usage,
                                               required_features, props,
                                               image_result);
        }
    }

    const struct {
        const char *label;
        const SurfaceFormatInfo *format;
    } zeta_formats[] = {
        { "surface-zeta-d16", &zeta_d16 },
        { "surface-zeta-d24s8", &zeta_d24_unorm_s8_uint },
        { "surface-zeta-d32s8", &zeta_d32_sfloat_s8_uint },
    };

    for (int i = 0; i < ARRAY_SIZE(zeta_formats); i++) {
        const SurfaceFormatInfo *format = zeta_formats[i].format;
        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  format->usage;
        VkFormatProperties props;
        VkResult image_result;
        bool features_ok;
        bool usage_ok;

        vkGetPhysicalDeviceFormatProperties(device, format->vk_format, &props);
        features_ok =
            (props.optimalTilingFeatures & required_features) ==
            required_features;

        VkPhysicalDeviceImageFormatInfo2 info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .format = format->vk_format,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage,
        };
        VkImageFormatProperties2 image_props = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        };
        image_result = vkGetPhysicalDeviceImageFormatProperties2(
            device, &info, &image_props);
        usage_ok = (image_result == VK_SUCCESS);

        if (!features_ok || !usage_ok) {
            log_android_format_support_failure(zeta_formats[i].label,
                                               format->vk_format, usage,
                                               required_features, props,
                                               image_result);
        }
    }
}

static void log_android_device_format_diagnostics(VkPhysicalDevice device)
{
    bool textures_ok = check_texture_formats_supported_internal(device, false);
    bool surfaces_ok = check_surface_formats_supported_internal(device, false);

    if (textures_ok && surfaces_ok) {
        fprintf(stderr,
                "Android Vulkan device passed full xemu texture/surface "
                "format compatibility checks\n");
        __android_log_print(
            ANDROID_LOG_INFO, "xemu-android",
            "Selected Vulkan device passed full xemu texture/surface format "
            "compatibility checks");
        return;
    }

    fprintf(stderr,
            "Warning: Android Vulkan device is running in best-effort mode; "
            "full xemu format compatibility checks failed\n");
    __android_log_print(
        ANDROID_LOG_WARN, "xemu-android",
        "Selected Vulkan device is running in best-effort mode; full xemu "
        "format compatibility checks failed");

    log_android_texture_format_diagnostics(device);
    log_android_surface_format_diagnostics(device);
}
#endif

static bool is_device_minimally_compatible(VkPhysicalDevice device)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    if (props.apiVersion < VK_API_VERSION_1_1) {
        return false;
    }

    QueueFamilyIndices indices = pgraph_vk_find_queue_families(device);

    return is_queue_family_indicies_complete(indices) &&
           check_device_support_required_extensions(device);
}

static bool is_device_fully_compatible(VkPhysicalDevice device)
{
#ifdef __ANDROID__
    return is_device_minimally_compatible(device) &&
           check_texture_formats_supported_internal(device, false) &&
           check_surface_formats_supported_internal(device, false);
#else
    return is_device_minimally_compatible(device) &&
           check_texture_formats_supported(device) &&
           check_surface_formats_supported(device);
#endif
    // FIXME: Check vram
}

static bool select_physical_device(PGRAPHState *pg, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    VkResult result;

    uint32_t num_physical_devices = 0;

    result =
        vkEnumeratePhysicalDevices(r->instance, &num_physical_devices, NULL);
    if (result != VK_SUCCESS || num_physical_devices == 0) {
        error_setg(errp, "Failed to find GPUs with Vulkan support");
        return false;
    }

    g_autofree VkPhysicalDevice *devices =
        g_malloc_n(num_physical_devices, sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(r->instance, &num_physical_devices, devices);

    const char *preferred_device = g_config.display.vulkan.preferred_physical_device;
    int preferred_device_index = -1;

    fprintf(stderr, "Available physical devices:\n");
    for (int i = 0; i < num_physical_devices; i++) {
        vkGetPhysicalDeviceProperties(devices[i], &r->device_props);
        bool is_preferred =
            preferred_device &&
            !strcmp(r->device_props.deviceName, preferred_device);
        if (is_preferred) {
            preferred_device_index = i;
        }
        fprintf(stderr, "- %s%s\n", r->device_props.deviceName,
                is_preferred ? " *" : "");
    }

    r->physical_device = VK_NULL_HANDLE;

    bool selected_device_fully_compatible = false;

    if (preferred_device_index >= 0 &&
        is_device_fully_compatible(devices[preferred_device_index])) {
        r->physical_device = devices[preferred_device_index];
        selected_device_fully_compatible = true;
    } else {
        for (int i = 0; i < num_physical_devices; i++) {
            if (is_device_fully_compatible(devices[i])) {
                r->physical_device = devices[i];
                selected_device_fully_compatible = true;
                break;
            }
        }
    }
    if (r->physical_device == VK_NULL_HANDLE) {
#ifdef __ANDROID__
        if (preferred_device_index >= 0 &&
            is_device_minimally_compatible(devices[preferred_device_index])) {
            r->physical_device = devices[preferred_device_index];
        } else {
            for (int i = 0; i < num_physical_devices; i++) {
                if (is_device_minimally_compatible(devices[i])) {
                    r->physical_device = devices[i];
                    break;
                }
            }
        }
        if (r->physical_device == VK_NULL_HANDLE) {
            int fallback_index =
                preferred_device_index >= 0 ? preferred_device_index : 0;
            r->physical_device = devices[fallback_index];
            vkGetPhysicalDeviceProperties(r->physical_device, &r->device_props);
            fprintf(stderr,
                    "Warning: No minimally compatible Vulkan GPU found; "
                    "trying %s anyway\n",
                    r->device_props.deviceName);
        }
        vkGetPhysicalDeviceProperties(r->physical_device, &r->device_props);
        if (!selected_device_fully_compatible) {
            fprintf(stderr,
                    "Warning: No fully compatible Vulkan GPU found; trying %s "
                    "in best-effort mode\n",
                    r->device_props.deviceName);
        }
#else
        error_setg(errp, "Failed to find a suitable GPU");
        return false;
#endif
    }

    vkGetPhysicalDeviceProperties(r->physical_device, &r->device_props);
    xemu_settings_set_string(&g_config.display.vulkan.preferred_physical_device,
                             r->device_props.deviceName);

    fprintf(stderr,
            "Selected physical device: %s\n"
            "- Vendor: %x, Device: %x\n"
            "- Driver Version: %d.%d.%d\n",
            r->device_props.deviceName,
            r->device_props.vendorID,
            r->device_props.deviceID,
            VK_VERSION_MAJOR(r->device_props.driverVersion),
            VK_VERSION_MINOR(r->device_props.driverVersion),
            VK_VERSION_PATCH(r->device_props.driverVersion));

#ifdef __ANDROID__
    if (!selected_device_fully_compatible) {
        log_android_device_format_diagnostics(r->physical_device);
    }
#endif

    return true;
}

static bool create_logical_device(PGRAPHState *pg, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    VkResult result;

    QueueFamilyIndices indices =
        pgraph_vk_find_queue_families(r->physical_device);

    g_autoptr(VkExtensionPropertiesArray) available_extensions =
        get_available_device_extensions(r->physical_device);

    g_autoptr(StringArray) enabled_extension_names =
        get_required_device_extension_names();

    add_optional_device_extension_names(pg, available_extensions,
                                        enabled_extension_names);

    const char *const *enabled_device_extension_names = NULL;
    if (enabled_extension_names->len > 0) {
        enabled_device_extension_names =
            &g_array_index(enabled_extension_names, const char *, 0);
    }

    fprintf(stderr, "Enabled device extensions:\n");
    for (int i = 0; i < enabled_extension_names->len; i++) {
        fprintf(stderr, "- %s\n",
                g_array_index(enabled_extension_names, char *, i));
    }

    float queuePriority = 1.0f;

    VkDeviceQueueCreateInfo queue_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = indices.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };

    // Check device features
    VkPhysicalDeviceFeatures physical_device_features;
    vkGetPhysicalDeviceFeatures(r->physical_device, &physical_device_features);
    memset(&r->enabled_physical_device_features, 0,
           sizeof(r->enabled_physical_device_features));

    struct {
        const char *name;
        VkBool32 available, *enabled;
        bool required;
    } desired_features[] = {
        // clang-format off
        #define F(n, req) { \
            .name = #n, \
            .available = physical_device_features.n, \
            .enabled = &r->enabled_physical_device_features.n, \
            .required = req, \
        }
        F(depthClamp, false),
        F(fillModeNonSolid, false),
        F(geometryShader, false),
        F(occlusionQueryPrecise, false),
        F(samplerAnisotropy, false),
        F(shaderClipDistance, false),
        F(shaderTessellationAndGeometryPointSize, false),
        F(wideLines, false),
        #undef F
        // clang-format on
    };

    bool all_required_features_available = true;
    char missing_required_features[256] = { 0 };
    size_t missing_required_len = 0;
    for (int i = 0; i < ARRAY_SIZE(desired_features); i++) {
        fprintf(stderr, "Vulkan feature %-36s : %s%s\n",
                desired_features[i].name,
                desired_features[i].available == VK_TRUE ? "available" : "missing",
                desired_features[i].required ? " (required)" : "");
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "vk feature %s: %s%s",
                            desired_features[i].name,
                            desired_features[i].available == VK_TRUE ? "available" : "missing",
                            desired_features[i].required ? " (required)" : "");
#endif
        if (desired_features[i].required &&
            desired_features[i].available != VK_TRUE) {
            fprintf(stderr,
                    "Error: Device does not support required feature %s\n",
                    desired_features[i].name);
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                                "vk required feature missing: %s",
                                desired_features[i].name);
#endif
            int n = snprintf(missing_required_features + missing_required_len,
                             sizeof(missing_required_features) - missing_required_len,
                             "%s%s",
                             missing_required_len ? ", " : "",
                             desired_features[i].name);
            if (n > 0) {
                size_t remaining = sizeof(missing_required_features) -
                                   missing_required_len - 1;
                size_t consumed = (size_t)n > remaining ? remaining : (size_t)n;
                missing_required_len += consumed;
            }
            all_required_features_available = false;
        }
        *desired_features[i].enabled = desired_features[i].available;
    }

    if (!all_required_features_available) {
        error_setg(errp, "Device does not support required features: %s",
                   missing_required_features[0] ?
                   missing_required_features : "(unknown)");
        return false;
    }

    void *next_struct = NULL;

    VkPhysicalDeviceCustomBorderColorFeaturesEXT custom_border_features;
    if (r->custom_border_color_extension_enabled) {
        custom_border_features = (VkPhysicalDeviceCustomBorderColorFeaturesEXT){
            .sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
            .customBorderColors = VK_TRUE,
            .pNext = next_struct,
        };
        next_struct = &custom_border_features;
    }

    VkDeviceCreateInfo device_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_create_info,
        .pEnabledFeatures = &r->enabled_physical_device_features,
        .enabledExtensionCount = enabled_extension_names->len,
        .ppEnabledExtensionNames = enabled_device_extension_names,
        .pNext = next_struct,
    };

    if (enable_validation) {
        device_create_info.enabledLayerCount = ARRAY_SIZE(validation_layers);
        device_create_info.ppEnabledLayerNames = validation_layers;
    }

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "vk init stage: vkCreateDevice");
#endif
    result = vkCreateDevice(r->physical_device, &device_create_info, NULL,
                            &r->device);
    if (result != VK_SUCCESS) {
        error_setg(errp, "Failed to create logical device (%d)", result);
        return false;
    }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "vk init stage: vkCreateDevice done");
#endif

    vkGetDeviceQueue(r->device, indices.queue_family, 0, &r->queue);
    return true;
}

uint32_t pgraph_vk_get_memory_type(PGRAPHState *pg, uint32_t type_bits,
                                   VkMemoryPropertyFlags properties)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkPhysicalDeviceMemoryProperties prop;
    vkGetPhysicalDeviceMemoryProperties(r->physical_device, &prop);
    for (uint32_t i = 0; i < prop.memoryTypeCount; i++) {
        if ((prop.memoryTypes[i].propertyFlags & properties) == properties &&
            type_bits & (1 << i)) {
            return i;
        }
    }
    return 0xFFFFFFFF; // Unable to find memoryType
}

static bool init_allocator(PGRAPHState *pg, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    VkResult result;

    VmaVulkanFunctions vulkanFunctions = {
        /// Required when using VMA_DYNAMIC_VULKAN_FUNCTIONS.
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
        /// Required when using VMA_DYNAMIC_VULKAN_FUNCTIONS.
        .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
        .vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties,
        .vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties,
        .vkAllocateMemory = vkAllocateMemory,
        .vkFreeMemory = vkFreeMemory,
        .vkMapMemory = vkMapMemory,
        .vkUnmapMemory = vkUnmapMemory,
        .vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges,
        .vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges,
        .vkBindBufferMemory = vkBindBufferMemory,
        .vkBindImageMemory = vkBindImageMemory,
        .vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements,
        .vkGetImageMemoryRequirements = vkGetImageMemoryRequirements,
        .vkCreateBuffer = vkCreateBuffer,
        .vkDestroyBuffer = vkDestroyBuffer,
        .vkCreateImage = vkCreateImage,
        .vkDestroyImage = vkDestroyImage,
        .vkCmdCopyBuffer = vkCmdCopyBuffer,
        .vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2,
        .vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2,
        .vkBindBufferMemory2KHR = vkBindBufferMemory2,
        .vkBindImageMemory2KHR = vkBindImageMemory2,
        .vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2,
    };

    const uint32_t device_api_version = r->device_props.apiVersion;
    const uint32_t vma_compiled_api_version = VK_MAKE_VERSION(
        (uint32_t)(VMA_VULKAN_VERSION / 1000000),
        (uint32_t)((VMA_VULKAN_VERSION / 1000) % 1000),
        (uint32_t)(VMA_VULKAN_VERSION % 1000));
    uint32_t vma_api_version = device_api_version;

    if (vma_api_version > vma_compiled_api_version) {
        vma_api_version = vma_compiled_api_version;
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_WARN, "xemu-android",
                            "Clamping VMA API version %u.%u.%u to %u.%u.%u",
                            VK_API_VERSION_MAJOR(device_api_version),
                            VK_API_VERSION_MINOR(device_api_version),
                            VK_API_VERSION_PATCH(device_api_version),
                            VK_API_VERSION_MAJOR(vma_api_version),
                            VK_API_VERSION_MINOR(vma_api_version),
                            VK_API_VERSION_PATCH(vma_api_version));
#endif
    }

    if (device_api_version >= VK_API_VERSION_1_3) {
        vulkanFunctions.vkGetDeviceBufferMemoryRequirements =
            vkGetDeviceBufferMemoryRequirements;
        vulkanFunctions.vkGetDeviceImageMemoryRequirements =
            vkGetDeviceImageMemoryRequirements;
    }

    VmaAllocatorCreateInfo create_info = {
        .flags = (r->memory_budget_extension_enabled ?
                      VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT :
                      0),
        .vulkanApiVersion = vma_api_version,
        .instance = r->instance,
        .physicalDevice = r->physical_device,
        .device = r->device,
        .pVulkanFunctions = &vulkanFunctions,
    };

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "vk init stage: vmaCreateAllocator");
#endif
    result = vmaCreateAllocator(&create_info, &r->allocator);
    if (result != VK_SUCCESS) {
        error_setg(errp, "vmaCreateAllocator failed");
        return false;
    }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "vk init stage: vmaCreateAllocator done");
#endif

    return true;
}

void pgraph_vk_init_instance(PGRAPHState *pg, Error **errp)
{
    bool ok = false;

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "vk init stage: create_instance");
#endif
    if (!create_instance(pg, errp)) {
        goto done;
    }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "vk init stage: select_physical_device");
#endif
    if (!select_physical_device(pg, errp)) {
        goto done;
    }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "vk init stage: create_logical_device");
#endif
    if (!create_logical_device(pg, errp)) {
        goto done;
    }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "vk init stage: init_allocator");
#endif
    if (!init_allocator(pg, errp)) {
        goto done;
    }

    ok = true;

done:
    if (ok) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "vk init stage: complete");
#endif
        return;
    }

    pgraph_vk_finalize_instance(pg);

    const char *msg = "Failed to initialize Vulkan renderer";
    if (*errp) {
        error_prepend(errp, "%s: ", msg);
    } else {
        error_setg(errp, "%s", msg);
    }
}

void pgraph_vk_finalize_instance(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(r->allocator);
        r->allocator = VK_NULL_HANDLE;
    }

    if (r->device != VK_NULL_HANDLE) {
        vkDestroyDevice(r->device, NULL);
        r->device = VK_NULL_HANDLE;
    }

    if (r->debug_messenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(r->instance, r->debug_messenger, NULL);
        r->debug_messenger = VK_NULL_HANDLE;
    }

    if (r->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(r->instance, NULL);
        r->instance = VK_NULL_HANDLE;
    }

    volkFinalize();
}
