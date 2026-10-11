/* hwvulkan_abi.h - the Android Vulkan HAL ABI as an LP64 C header, for tools that call the vendor HAL
 * directly (tools/pixel10/pvr_props_probe.c) or stand in for it (tools/tests/fake_powervr_hal.c).
 * hw_module_t / hw_device_t are copies of AOSP hardware/libhardware include_all/hardware/hardware.h;
 * hwvulkan_device_t is frameworks/native vulkan/include/hardware/hwvulkan.h. The NDK ships neither.
 * Include after <vulkan/vulkan.h>. The static asserts pin the offsets measured on the Pixel 10 HAL
 * (close +0x70, CreateInstance +0x80, GetInstanceProcAddr +0x88, 0x90 bytes). It also carries the
 * VK_ANDROID_native_buffer gralloc-usage entry points (vk_android_native_buffer.h, spec versions 6-11).
 */
#ifndef PIXEL10_HWVULKAN_ABI_H
#define PIXEL10_HWVULKAN_ABI_H
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/* ---- Android hardware ABI (LP64) -------------------------------------------------------- */
typedef struct hw_module_t hw_module_t;
typedef struct hw_device_t hw_device_t;
typedef struct { int (*open)(const hw_module_t *, const char *, hw_device_t **); } hw_module_methods_t;
struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version, hal_api_version;
    const char *id, *name, *author;
    hw_module_methods_t *methods;
    void *dso;
    uint64_t reserved[32 - 7];
};
struct hw_device_t {
    uint32_t tag, version;
    hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(hw_device_t *);
};
typedef struct {
    hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} hwvulkan_device_t;
_Static_assert(sizeof(void *) == 8, "LP64 only: the reserved[] widths above are the __LP64__ ones");
_Static_assert(sizeof(hw_module_t) == 0xf8 && offsetof(hw_module_t, methods) == 0x20,
               "hw_module_t layout");
_Static_assert(offsetof(hw_device_t, close) == 0x70 && sizeof(hwvulkan_device_t) == 0x90 &&
                   offsetof(hwvulkan_device_t, GetInstanceProcAddr) == 0x88,
               "hwvulkan_device_t layout (close +0x70, CreateInstance +0x80, GetInstanceProcAddr +0x88)");
#define HW_MODULE_TAG 0x48574d54u  /* 'HWMT' */
#define HW_DEVICE_TAG 0x48574454u  /* 'HWDT' */
#define HWVK_DEVICE_API_0_1 0x00010000u /* HARDWARE_DEVICE_API_VERSION_2(0, 1, 0): upstream hybris */
#define HW_DEVICE_API_0_1 0x00000001u   /* HARDWARE_DEVICE_API_VERSION(0, 1): what powervr-hal-version.patch adds */

/* ---- VK_ANDROID_native_buffer, only what the gralloc-usage queries need ------------------ */
#define ANB_ENUM(id) ((VkStructureType)(1000000000 + 1000 * (11 - 1) + (id)))
typedef VkFlags VkSwapchainImageUsageFlagsANDROID;
typedef struct { VkStructureType sType; const void *pNext; VkFormat format; VkImageUsageFlags imageUsage; } GrallocInfo3;
typedef struct { VkStructureType sType; const void *pNext; VkFormat format; VkImageUsageFlags imageUsage;
                 VkSwapchainImageUsageFlagsANDROID swapchainImageUsage; } GrallocInfo4;
typedef VkResult (*PFN_gralloc1)(VkDevice, VkFormat, VkImageUsageFlags, int *);
typedef VkResult (*PFN_gralloc2)(VkDevice, VkFormat, VkImageUsageFlags, VkSwapchainImageUsageFlagsANDROID,
                                 uint64_t *, uint64_t *);
typedef VkResult (*PFN_gralloc3)(VkDevice, const GrallocInfo3 *, uint64_t *);
typedef VkResult (*PFN_gralloc4)(VkDevice, const GrallocInfo4 *, uint64_t *);

#endif
