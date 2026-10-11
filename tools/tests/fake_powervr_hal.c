/* fake_powervr_hal.c - a stand-in for /vendor/lib64/hw/vulkan.powervr.so that host tests can load.
 *
 * It exports the HMI module and answers the handful of Vulkan entry points tools/pixel10/pvr_props_probe.c
 * calls, shaped like the Pixel 10 (driver 25.3@6908880) as measured by gpuinfo.org and the Fizzolas probes:
 * Vulkan 1.4.317, BC1-BC5 sampleable with the spec-mandated features, BC6H/BC7 absent,
 * textureCompressionBC false, VK_EXT_physical_device_drm advertised, HAL device version 0x00000001.
 *
 * The behaviours the tests need to vary come from the environment:
 *   FAKE_HAL_DRM=untouched|zeroed|filled   what the HAL does with VkPhysicalDeviceDrmPropertiesEXT
 *   FAKE_HAL_WIDE_USAGE=1                  gralloc producer usage needs more than 32 bits
 *   FAKE_HAL_VERSION=0x00010000            the hw_device_t version (default 0x00000001)
 *   FAKE_HAL_CRASH=1                       vkGetPhysicalDeviceProperties2 dies with SIGSEGV
 *   FAKE_HAL_SPIN=1                        vkGetPhysicalDeviceProperties2 never returns
 *   FAKE_HAL_DRIVER_INFO=text              VkPhysicalDeviceDriverProperties::driverInfo (default 25.3@6908880)
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "../pixel10/hwvulkan_abi.h"

#define EXPORT __attribute__((visibility("default")))
static int g_instance, g_physical, g_device;
static const VkExtensionProperties EXTENSIONS[] = {
    {"VK_KHR_driver_properties", 1}, {"VK_EXT_physical_device_drm", 1}, {"VK_ANDROID_native_buffer", 11},
    {"VK_KHR_dynamic_rendering", 1},
};
#define NEXT (sizeof EXTENSIONS / sizeof *EXTENSIONS)

static const char *env(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return v && v[0] ? v : fallback;
}

static VkResult enumerate_extensions(uint32_t *count, VkExtensionProperties *out) {
    if (!out) {
        *count = NEXT;
        return VK_SUCCESS;
    }
    uint32_t n = *count < NEXT ? *count : (uint32_t)NEXT;
    memcpy(out, EXTENSIONS, n * sizeof *out);
    *count = n;
    return n < NEXT ? VK_INCOMPLETE : VK_SUCCESS;
}
static VkResult enumerate_instance_extensions(const char *layer, uint32_t *count, VkExtensionProperties *out) {
    (void)layer;
    *count = 0;
    (void)out;
    return VK_SUCCESS;
}
static VkResult create_instance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    (void)ci;
    (void)a;
    *out = (VkInstance)&g_instance;
    return VK_SUCCESS;
}
static void destroy_instance(VkInstance i, const VkAllocationCallbacks *a) { (void)i; (void)a; }
static VkResult enumerate_physical(VkInstance i, uint32_t *count, VkPhysicalDevice *out) {
    (void)i;
    if (out && *count) out[0] = (VkPhysicalDevice)&g_physical;
    *count = 1;
    return VK_SUCCESS;
}
static void get_properties(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p) {
    (void)pd;
    memset(p, 0, sizeof *p);
    p->apiVersion = VK_MAKE_API_VERSION(0, 1, 4, 317);
    p->driverVersion = 6908880;
    p->vendorID = 0x1010;
    p->deviceID = 0x71061212;
    p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    strcpy(p->deviceName, "PowerVR D-Series DXT-48-1536 MC1");
}
static void get_properties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p2) {
    if (!strcmp(env("FAKE_HAL_CRASH", "0"), "1")) raise(SIGSEGV);
    if (!strcmp(env("FAKE_HAL_SPIN", "0"), "1"))
        for (volatile unsigned long n = 0;; n++) {}
    get_properties(pd, &p2->properties);
    const char *drm = env("FAKE_HAL_DRM", "untouched");
    for (VkBaseOutStructure *n = p2->pNext; n; n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) {
            VkPhysicalDeviceDriverProperties *d = (VkPhysicalDeviceDriverProperties *)n;
            d->driverID = VK_DRIVER_ID_IMAGINATION_PROPRIETARY;
            strcpy(d->driverName, "Imagination Technologies");
            strncpy(d->driverInfo, env("FAKE_HAL_DRIVER_INFO", "25.3@6908880"), sizeof d->driverInfo - 1);
        } else if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT) {
            VkPhysicalDeviceDrmPropertiesEXT *d = (VkPhysicalDeviceDrmPropertiesEXT *)n;
            if (!strcmp(drm, "zeroed")) {
                d->hasPrimary = d->hasRender = 0;
                d->primaryMajor = d->primaryMinor = d->renderMajor = d->renderMinor = 0;
            } else if (!strcmp(drm, "filled")) {
                d->hasPrimary = d->hasRender = 1;
                d->primaryMajor = d->renderMajor = 226;
                d->primaryMinor = 2;
                d->renderMinor = 128;
            }
        }
    }
}
static void get_features(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) {
    (void)pd;
    memset(f, 0, sizeof *f);
}
static void get_format_properties(VkPhysicalDevice pd, VkFormat f, VkFormatProperties *out) {
    (void)pd;
    memset(out, 0, sizeof *out);
    int bc = f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK;
    int missing = f >= VK_FORMAT_BC6H_UFLOAT_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK;
    if (bc && !missing)
        out->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                     VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                     VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    else if (!bc)
        out->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
}
static void get_queue_families(VkPhysicalDevice pd, uint32_t *count, VkQueueFamilyProperties *out) {
    (void)pd;
    if (out && *count) {
        memset(out, 0, sizeof *out);
        out[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        out[0].queueCount = 2;
        out[0].minImageTransferGranularity = (VkExtent3D){1, 1, 1};
    }
    *count = 1;
}
static VkResult enumerate_device_extensions(VkPhysicalDevice pd, const char *layer, uint32_t *count, VkExtensionProperties *out) {
    (void)pd;
    (void)layer;
    return enumerate_extensions(count, out);
}
/* Like the real HAL, a device only exists when VK_ANDROID_native_buffer is asked for. */
static VkResult create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    (void)pd;
    (void)a;
    if (ci->enabledExtensionCount != 1 || strcmp(ci->ppEnabledExtensionNames[0], "VK_ANDROID_native_buffer"))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    *out = (VkDevice)&g_device;
    return VK_SUCCESS;
}
static void destroy_device(VkDevice d, const VkAllocationCallbacks *a) { (void)d; (void)a; }
static VkResult gralloc_usage2(VkDevice d, VkFormat f, VkImageUsageFlags u, VkSwapchainImageUsageFlagsANDROID s, uint64_t *consumer,
                               uint64_t *producer) {
    (void)d;
    (void)f;
    (void)u;
    (void)s;
    *consumer = 0x100;
    *producer = !strcmp(env("FAKE_HAL_WIDE_USAGE", "0"), "1") ? 0x100000000ull | 0x200 : 0x200;
    return VK_SUCCESS;
}

static PFN_vkVoidFunction device_proc(VkDevice d, const char *name) {
    (void)d;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)destroy_device;
    if (!strcmp(name, "vkGetSwapchainGrallocUsage2ANDROID")) return (PFN_vkVoidFunction)gralloc_usage2;
    return NULL;
}
static PFN_vkVoidFunction instance_proc(VkInstance i, const char *name) {
    static const struct { const char *name; PFN_vkVoidFunction fn; } TABLE[] = {
        {"vkDestroyInstance", (PFN_vkVoidFunction)destroy_instance},
        {"vkEnumeratePhysicalDevices", (PFN_vkVoidFunction)enumerate_physical},
        {"vkGetPhysicalDeviceProperties", (PFN_vkVoidFunction)get_properties},
        {"vkGetPhysicalDeviceProperties2", (PFN_vkVoidFunction)get_properties2},
        {"vkGetPhysicalDeviceFeatures", (PFN_vkVoidFunction)get_features},
        {"vkGetPhysicalDeviceFormatProperties", (PFN_vkVoidFunction)get_format_properties},
        {"vkGetPhysicalDeviceQueueFamilyProperties", (PFN_vkVoidFunction)get_queue_families},
        {"vkEnumerateDeviceExtensionProperties", (PFN_vkVoidFunction)enumerate_device_extensions},
        {"vkCreateDevice", (PFN_vkVoidFunction)create_device},
        {"vkGetDeviceProcAddr", (PFN_vkVoidFunction)device_proc},
        {"vkCreateInstance", (PFN_vkVoidFunction)create_instance},
    };
    (void)i;
    for (size_t k = 0; k < sizeof TABLE / sizeof *TABLE; k++)
        if (!strcmp(name, TABLE[k].name)) return TABLE[k].fn;
    return NULL;
}

static int hal_open(const hw_module_t *m, const char *id, hw_device_t **out) {
    static hwvulkan_device_t dev;
    if (strcmp(id, "vk0")) return -22;
    memset(&dev, 0, sizeof dev);
    dev.common.tag = HW_DEVICE_TAG;
    dev.common.version = (uint32_t)strtoul(env("FAKE_HAL_VERSION", "0x00000001"), NULL, 0);
    dev.common.module = (hw_module_t *)m;
    dev.EnumerateInstanceExtensionProperties = enumerate_instance_extensions;
    dev.CreateInstance = create_instance;
    dev.GetInstanceProcAddr = instance_proc;
    *out = &dev.common;
    return 0;
}
static hw_module_methods_t g_methods = {hal_open};
EXPORT hw_module_t HMI = {.tag = HW_MODULE_TAG, .module_api_version = 1, .hal_api_version = 0, .id = "vulkan",
                          .name = "PowerVR D-Series Vulkan HAL (fake)", .author = "tests", .methods = &g_methods};
