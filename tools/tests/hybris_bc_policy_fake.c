/* SPDX-License-Identifier: Apache-2.0 */
/* Host check of bc_policy.c against a fake Vulkan 1.4 driver (no device, no loader).
 * The policy is read once per process, so run it once per policy:
 *   for p in unset missing force dxvk d3d 0; do ... HYBRIS_BC_TEXTURES=$p ./bc_policy_fake $p; done
 * (unset: leave the variable out). Build:
 *   cc -std=gnu11 -Wall -Wextra tools/tests/hybris_bc_policy_fake.c hybris/vulkan/compat/bc_policy.c \
 *      hybris/vulkan/compat/bc_rules.c hybris/vulkan/compat/bc_image.c -pthread -o bc_policy_fake */
#define VK_NO_PROTOTYPES
#include "compat/bc_policy.h"
#include "compat/bc_context.h"
#include "layer/layer.h"
#include <stdio.h>
#include <string.h>

#define FULL (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | \
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
    VK_FORMAT_FEATURE_TRANSFER_DST_BIT)
static unsigned checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); } } while (0)

/* The fake driver: Pixel 10 shape. BC1 RGB..BC5 native, BC6H/BC7 absent. */
static VkFormatFeatureFlags bc_native[16], decoded_features;
static VkFormatFeatureFlags rgba16f_features, rgba8_features;
static uint32_t device_api;
static VkBool32 native_texture_compression;
static VkQueueFamilyProperties family;

static void reset_driver(void)
{
    for (unsigned i = 0; i < 16; ++i) bc_native[i] = i < 12 ? FULL : 0;
    rgba8_features = rgba16f_features = decoded_features = FULL | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    device_api = VK_MAKE_API_VERSION(0, 1, 4, 317);
    native_texture_compression = VK_FALSE;
    family = (VkQueueFamilyProperties){.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
        VK_QUEUE_TRANSFER_BIT, .queueCount = 1, .minImageTransferGranularity = {1, 1, 1}};
}
static void fake_properties(VkPhysicalDevice p, VkPhysicalDeviceProperties *out)
{
    (void)p;
    memset(out, 0, sizeof(*out));
    out->apiVersion = device_api;
    out->driverVersion = 0x1903;
    out->limits.maxStorageBufferRange = 0x7fffffffu;
    out->limits.minStorageBufferOffsetAlignment = 64;
}
static void fake_queues(VkPhysicalDevice p, uint32_t *count, VkQueueFamilyProperties *out)
{
    (void)p;
    if (out && *count) out[0] = family;
    *count = 1;
}
static void fake_format(VkPhysicalDevice p, VkFormat format, VkFormatProperties *out)
{
    (void)p;
    memset(out, 0, sizeof(*out));
    if (format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK)
        out->optimalTilingFeatures = bc_native[format - VK_FORMAT_BC1_RGB_UNORM_BLOCK];
    else if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB) out->optimalTilingFeatures = rgba8_features;
    else if (format == VK_FORMAT_R16G16B16A16_SFLOAT) out->optimalTilingFeatures = rgba16f_features;
    else if (format == VK_FORMAT_R16_UNORM || format == VK_FORMAT_R16_SNORM || format == VK_FORMAT_R16G16_UNORM ||
        format == VK_FORMAT_R16G16_SNORM) out->optimalTilingFeatures = decoded_features;
}
static void fake_features(VkPhysicalDevice p, VkPhysicalDeviceFeatures *out)
{
    (void)p;
    memset(out, 0, sizeof(*out));
    out->textureCompressionBC = native_texture_compression;
    out->pipelineStatisticsQuery = VK_TRUE;
}
static void fake_features2(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *out)
{ fake_features(p, &out->features); }
static PFN_vkVoidFunction VKAPI_CALL fake_resolver(VkInstance instance, const char *name)
{
    (void)instance;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties")) return (PFN_vkVoidFunction)fake_properties;
    if (!strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties")) return (PFN_vkVoidFunction)fake_queues;
    if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties")) return (PFN_vkVoidFunction)fake_format;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures")) return (PFN_vkVoidFunction)fake_features;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2")) return (PFN_vkVoidFunction)fake_features2;
    return NULL;
}
/* Stubs for everything outside bc_policy.c / bc_rules.c / bc_image.c. */
int hybris_layer_lookup_physical(VkPhysicalDevice physical, struct hybris_layer_physical *out)
{
    (void)physical;
    *out = (struct hybris_layer_physical){.instance = (VkInstance)(uintptr_t)1, .generation = 1,
        .api_version = VK_API_VERSION_1_3, .resolver = fake_resolver};
    return 1;
}
VkResult hybris_bc_device_add(VkDevice h, PFN_vkGetDeviceProcAddr r, const VkPhysicalDeviceMemoryProperties *m,
    const VkPhysicalDeviceProperties *p, unsigned f, unsigned g, const VkAllocationCallbacks *a)
{ (void)h; (void)r; (void)m; (void)p; (void)f; (void)g; (void)a; return VK_SUCCESS; }
PFN_vkVoidFunction hybris_bc_commands_proc(const char *n) { (void)n; return NULL; }
PFN_vkVoidFunction hybris_bc_resources_proc(const char *n) { (void)n; return NULL; }
PFN_vkVoidFunction hybris_bc_record_proc(const char *n) { (void)n; return NULL; }
PFN_vkVoidFunction hybris_bc_barriers_proc(const char *n) { (void)n; return NULL; }
PFN_vkVoidFunction hybris_bc_image_copy_proc(const char *n) { (void)n; return NULL; }

static const VkPhysicalDevice physical = (VkPhysicalDevice)(uintptr_t)8;
static VkBool32 reported_bc(void)
{
    PFN_vkGetPhysicalDeviceFeatures2 query =
        (PFN_vkGetPhysicalDeviceFeatures2)hybris_bc_policy_proc("vkGetPhysicalDeviceFeatures2");
    VkPhysicalDeviceFeatures2 out = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if (!query) return native_texture_compression;
    query(physical, &out);
    return out.features.textureCompressionBC;
}
static VkResult create_with_bc(void)
{
    VkPhysicalDeviceFeatures enabled = {.textureCompressionBC = VK_TRUE};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pEnabledFeatures = &enabled};
    return hybris_bc_prepare_device(physical, &info);
}
static uint32_t reported_api(void)
{
    PFN_vkGetPhysicalDeviceProperties query =
        (PFN_vkGetPhysicalDeviceProperties)hybris_bc_policy_proc("vkGetPhysicalDeviceProperties");
    VkPhysicalDeviceProperties out;
    if (!query) { fake_properties(physical, &out); return out.apiVersion; }
    query(physical, &out);
    return out.apiVersion;
}

static void pixel10_d3d(unsigned expected_mask)   /* d3d and dxvk profiles on the Pixel 10 shape */
{
    reset_driver();
    CHECK(hybris_bc_physical_mask(physical) == expected_mask);
    CHECK(hybris_bc_texture_compression(physical) == 1);
    CHECK(reported_bc() == VK_TRUE);
    CHECK(reported_api() == VK_API_VERSION_1_3);
    CHECK(create_with_bc() == VK_SUCCESS);
    CHECK(hybris_bc_strip_texture_compression(physical) == 1);
    VkFormatProperties bc7;
    CHECK(hybris_bc_format_properties(physical, VK_FORMAT_BC7_UNORM_BLOCK, &bc7) == 1 && bc7.optimalTilingFeatures == FULL &&
        !bc7.linearTilingFeatures && !bc7.bufferFeatures);
    CHECK(hybris_bc_extension_allowed("VK_KHR_maintenance6") && !hybris_bc_extension_allowed("VK_KHR_push_descriptor"));
    /* Emulated BC7 backing without BLIT_SRC: the claim must be withdrawn and enabling refused. */
    rgba8_features = FULL & ~VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    CHECK(hybris_bc_texture_compression(physical) == 0 && reported_bc() == VK_FALSE);
    CHECK(create_with_bc() == VK_ERROR_FEATURE_NOT_PRESENT && hybris_bc_strip_texture_compression(physical) == 0);
    /* A driver that already reports the feature keeps it and sees it enabled. */
    reset_driver();
    native_texture_compression = VK_TRUE;
    CHECK(reported_bc() == VK_TRUE && hybris_bc_strip_texture_compression(physical) == 0);
    /* Compute-less queue family: emulation cannot run, nothing is touched. */
    reset_driver();
    family.queueFlags = VK_QUEUE_TRANSFER_BIT;
    CHECK(hybris_bc_physical_mask(physical) == 0 && reported_bc() == VK_FALSE && reported_api() == device_api);
}
static void check_policy(const char *name)
{
    reset_driver();
    if (!strcmp(name, "d3d")) {
        CHECK(hybris_bc_enabled());
        pixel10_d3d(0xF000u);
        /* Natively sampleable but incomplete BC4: d3d decodes it, so the claim survives. */
        reset_driver();
        bc_native[8] = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        CHECK(hybris_bc_physical_mask(physical) == 0xF100u && hybris_bc_texture_compression(physical) == 1);
    } else if (!strcmp(name, "dxvk")) {
        pixel10_d3d(0xFFFCu);
    } else if (!strcmp(name, "missing") || !strcmp(name, "unset") || !strcmp(name, "force")) {
        /* Unchanged behavior: Vulkan 1.4 stays excluded, and the feature is never raised. */
        CHECK(hybris_bc_enabled() && hybris_bc_physical_mask(physical) == 0);
        CHECK(hybris_bc_texture_compression(physical) == 0 && reported_bc() == VK_FALSE);
        CHECK(reported_api() == device_api && hybris_bc_strip_texture_compression(physical) == 0);
        device_api = VK_API_VERSION_1_3;
        const unsigned want = !strcmp(name, "force") ? 0xFFFFu : 0xF000u;
        CHECK(hybris_bc_physical_mask(physical) == want);
        CHECK(hybris_bc_texture_compression(physical) == 0 && reported_bc() == VK_FALSE);
        CHECK(create_with_bc() == VK_ERROR_FEATURE_NOT_PRESENT);
    } else {
        CHECK(!hybris_bc_enabled() && hybris_bc_physical_mask(physical) == 0);
        CHECK(!hybris_bc_policy_proc("vkGetPhysicalDeviceFeatures2"));
    }
}

int main(int argc, char **argv)
{
    check_policy(argc > 1 ? argv[1] : "unset");
    printf("BC_POLICY_FAKE policy=%s checks=%u failures=%u\n", argc > 1 ? argv[1] : "unset", checks, failures);
    return failures ? 1 : 0;
}
