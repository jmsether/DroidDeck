/* SPDX-License-Identifier: Apache-2.0 */
/* Host check of layer/device_features.c: the layer reports textureCompressionBC without the driver having it, so
 * vkCreateDevice must hand the driver a copy with the bit cleared and leave the application's structures alone.
 * The policies are stubbed. Build (TREE is the prepared libhybris tree, VULKAN_INCLUDE the Vulkan headers):
 *   cc -std=gnu11 -Wall -Wextra -Werror -I $VULKAN_INCLUDE -I $TREE/hybris/vulkan \
 *      tools/tests/hybris_bc_device_create_test.c $TREE/hybris/vulkan/layer/device_features.c -o bc_device_create */
#define VK_NO_PROTOTYPES
#include "layer/device_features.h"
#include "compat/bc_policy.h"
#include "compat/clip_distance.h"
#include "compat/vertex_stores.h"
#include <vulkan/vk_layer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); } } while (0)

/* What the policies answer. strip: the layer reports textureCompressionBC and the driver lacks it. clip and
 * stores: the other two filters that copy the create info (off unless a case switches them on). */
static int strip, clip, stores;
int hybris_clip_active(VkPhysicalDevice physical) { (void)physical; return clip; }
int hybris_vertex_stores_active(VkPhysicalDevice physical) { (void)physical; return stores; }
int hybris_bc_strip_texture_compression(VkPhysicalDevice physical) { (void)physical; return strip; }

/* Every allocation the layer makes goes through this, so a leak or a double free shows without a sanitizer. */
static int live, allocations;
static void *VKAPI_CALL counted_allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope)
{ (void)user; (void)alignment; (void)scope; ++live; ++allocations; return malloc(size); }
static void VKAPI_CALL counted_free(void *user, void *memory)
{ (void)user; if (memory) { --live; free(memory); } }
static const VkAllocationCallbacks allocator = {.pfnAllocation = counted_allocate, .pfnFree = counted_free};

static const VkPhysicalDevice physical = (VkPhysicalDevice)(uintptr_t)8;
static VkResult prepare(const VkDeviceCreateInfo *info, struct hybris_device_features *out)
{
    allocations = 0;
    return hybris_device_features_prepare(physical, info, &allocator, out);
}
static void finish(struct hybris_device_features *out)
{
    hybris_device_features_release(out, &allocator);
    CHECK(live == 0 && !out->copies);
}

static const char *const extension_names[] = {"VK_KHR_swapchain", "VK_KHR_maintenance5"};
static const VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
static VkDeviceCreateInfo device_info(const void *next, const VkPhysicalDeviceFeatures *enabled)
{
    return (VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = next, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue, .enabledExtensionCount = 2, .ppEnabledExtensionNames = extension_names,
        .pEnabledFeatures = enabled};
}
/* Everything in a VkDeviceCreateInfo except its two feature routes (pNext, pEnabledFeatures). */
static int same_device_info(const VkDeviceCreateInfo *a, const VkDeviceCreateInfo *b)
{
    return a->sType == b->sType && a->flags == b->flags && a->queueCreateInfoCount == b->queueCreateInfoCount &&
        a->pQueueCreateInfos == b->pQueueCreateInfos && a->enabledLayerCount == b->enabledLayerCount &&
        a->ppEnabledLayerNames == b->ppEnabledLayerNames && a->enabledExtensionCount == b->enabledExtensionCount &&
        a->ppEnabledExtensionNames == b->ppEnabledExtensionNames;
}
/* All 55 features on: any bit the layer touches besides textureCompressionBC shows as a change. */
static VkPhysicalDeviceFeatures all_features(void)
{
    VkPhysicalDeviceFeatures features;
    const VkBool32 on = VK_TRUE;
    for (unsigned i = 0; i < sizeof features / sizeof on; ++i) memcpy((char *)&features + i * sizeof on, &on, sizeof on);
    return features;
}
static int equals_but_bc(const VkPhysicalDeviceFeatures *seen, const VkPhysicalDeviceFeatures *app)
{
    VkPhysicalDeviceFeatures expected = *app;
    expected.textureCompressionBC = VK_FALSE;
    return !memcmp(seen, &expected, sizeof expected);
}
static unsigned sequence(const void *head, VkStructureType *types, unsigned capacity)
{
    unsigned count = 0;
    for (const VkBaseInStructure *node = head; node && count < capacity; node = node->pNext) types[count++] = node->sType;
    return count;
}
static const VkBaseInStructure *find(const void *head, VkStructureType type)
{
    const VkBaseInStructure *node = head;
    while (node && node->sType != type) node = node->pNext;
    return node;
}

/* (a) pEnabledFeatures: the driver gets a copy without the bit; the application's structure is untouched. */
static void enabled_features(void)
{
    VkPhysicalDeviceFeatures app = all_features(), before = app;
    VkDeviceCreateInfo info = device_info(NULL, &app);
    struct hybris_device_features out;
    strip = 1;
    CHECK(prepare(&info, &out) == VK_SUCCESS);
    CHECK(out.info.pEnabledFeatures && out.info.pEnabledFeatures != &app);
    CHECK(out.info.pEnabledFeatures && equals_but_bc(out.info.pEnabledFeatures, &before));
    CHECK(!memcmp(&app, &before, sizeof app) && app.textureCompressionBC == VK_TRUE);
    CHECK(same_device_info(&out.info, &info) && out.info.pNext == info.pNext && !out.copies && allocations == 0);
    finish(&out);
}

/* (b) Features2 in DXVK's shape: Vulkan 1.1/1.2/1.3 and maintenance5/6 feature structs follow it. The
 * layer copies the prefix up to Features2 and links the application's own nodes after the copy. */
struct dxvk_chain {
    VkLayerDeviceCreateInfo loader;
    VkPhysicalDeviceFeatures2 features2;
    VkPhysicalDeviceVulkan11Features v11;
    VkPhysicalDeviceVulkan12Features v12;
    VkPhysicalDeviceVulkan13Features v13;
    VkPhysicalDeviceMaintenance5Features maintenance5;
    VkPhysicalDeviceMaintenance6Features maintenance6;
};
static const VkBaseInStructure *dxvk_chain(struct dxvk_chain *c, int with_loader)
{
    *c = (struct dxvk_chain){
        .loader = {.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, .function = VK_LOADER_DATA_CALLBACK, .pNext = &c->features2},
        .features2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &c->v11, .features = all_features()},
        .v11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &c->v12, .multiview = VK_TRUE},
        .v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &c->v13, .bufferDeviceAddress = VK_TRUE},
        .v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &c->maintenance5, .dynamicRendering = VK_TRUE},
        .maintenance5 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES, .pNext = &c->maintenance6, .maintenance5 = VK_TRUE},
        .maintenance6 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES, .maintenance6 = VK_TRUE},
    };
    return with_loader ? (const VkBaseInStructure *)&c->loader : (const VkBaseInStructure *)&c->features2;
}
static void features2_chain(int with_loader)
{
    struct dxvk_chain app, before;
    VkStructureType want[8], got[8];
    VkDeviceCreateInfo info = device_info(dxvk_chain(&app, with_loader), NULL);
    memcpy(&before, &app, sizeof app);
    unsigned count = sequence(info.pNext, want, 8);
    struct hybris_device_features out;
    strip = 1;
    CHECK(prepare(&info, &out) == VK_SUCCESS);
    CHECK(count == (with_loader ? 7u : 6u) && sequence(out.info.pNext, got, 8) == count && !memcmp(got, want, count * sizeof *got));
    const VkPhysicalDeviceFeatures2 *copy = (const void *)find(out.info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    CHECK(copy && copy != &app.features2 && equals_but_bc(&copy->features, &before.features2.features));
    CHECK(copy && copy->pNext == (const void *)&app.v11 && out.suffix == &app.v11);   /* the suffix is shared, not copied */
    CHECK(allocations == (with_loader ? 2 : 1) && !out.info.pEnabledFeatures && same_device_info(&out.info, &info));
    if (with_loader) {
        VkLayerDeviceCreateInfo loader;
        memcpy(&loader, out.info.pNext, sizeof loader);
        CHECK(out.info.pNext != (const void *)&app.loader && loader.pNext == (const void *)copy);
        loader.pNext = app.loader.pNext;
        CHECK(!memcmp(&loader, &app.loader, sizeof loader));
    } else CHECK(out.info.pNext == (const void *)copy);
    CHECK(!memcmp(&app, &before, sizeof app) && app.features2.features.textureCompressionBC == VK_TRUE);
    finish(&out);
    CHECK(!memcmp(&app, &before, sizeof app));
}

/* (c) Nothing to strip: the driver sees the application's own pointers, with no copy made. */
static void nothing_to_strip(void)
{
    struct dxvk_chain app;
    VkPhysicalDeviceFeatures enabled = all_features();
    struct hybris_device_features out;
    VkDeviceCreateInfo features2 = device_info(dxvk_chain(&app, 1), NULL), plain = device_info(NULL, &enabled);
    strip = 0;   /* the driver reports the feature itself */
    CHECK(prepare(&features2, &out) == VK_SUCCESS && out.info.pNext == features2.pNext && !out.copies && allocations == 0);
    CHECK(same_device_info(&out.info, &features2) && out.info.pEnabledFeatures == NULL);
    finish(&out);
    CHECK(prepare(&plain, &out) == VK_SUCCESS && out.info.pEnabledFeatures == &enabled && out.info.pNext == NULL && allocations == 0);
    finish(&out);
    strip = 1;   /* stripping is on, but the application never asked for the feature */
    app.features2.features.textureCompressionBC = VK_FALSE;
    enabled.textureCompressionBC = VK_FALSE;
    CHECK(prepare(&features2, &out) == VK_SUCCESS && out.info.pNext == features2.pNext && !out.copies && allocations == 0);
    finish(&out);
    /* pEnabledFeatures is always copied while stripping is on; the copy equals the application's. */
    CHECK(prepare(&plain, &out) == VK_SUCCESS && out.info.pEnabledFeatures && !memcmp(out.info.pEnabledFeatures, &enabled, sizeof enabled));
    CHECK(out.info.pNext == NULL && allocations == 0);
    finish(&out);
}

/* (d) Existing behavior, pinned: a node with a payload size the layer does not know in front of Features2 fails
 * the create and frees what was copied so far; behind Features2 it is never read, so it passes through. */
static void unknown_structure(void)
{
    struct dxvk_chain app;
    VkBaseInStructure unknown = {.sType = (VkStructureType)0x7ffffff0u};
    struct hybris_device_features out;
    VkDeviceCreateInfo info = device_info(dxvk_chain(&app, 1), NULL);
    strip = 1;
    app.loader.pNext = &unknown;
    unknown.pNext = (const void *)&app.features2;
    CHECK(prepare(&info, &out) == VK_ERROR_EXTENSION_NOT_PRESENT);
    CHECK(allocations == 1 && live == 0 && !out.copies);
    finish(&out);

    unknown.pNext = NULL;
    app.loader.pNext = &app.features2;
    app.maintenance6.pNext = &unknown;
    CHECK(prepare(&info, &out) == VK_SUCCESS && out.suffix == &app.v11 && allocations == 2);
    const VkBaseInStructure *tail = find(out.info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES);
    CHECK(tail == (const void *)&app.maintenance6 && app.maintenance6.pNext == &unknown);
    finish(&out);
}

/* (e) The copy another filter makes must not take the bit along: when the driver has textureCompressionBC itself
 * (strip off), the application's request reaches the driver while that filter's own bits are cleared. */
static void bc_survives_the_other_filters(void)
{
    strip = 0;
    for (unsigned mode = 1; mode <= 3; ++mode) {   /* clip alone, stores alone, both */
        clip = mode & 1;
        stores = mode >> 1;
        VkPhysicalDeviceFeatures app = all_features(), before = app, want = app;
        if (clip) want.shaderClipDistance = VK_FALSE;
        if (stores) want.vertexPipelineStoresAndAtomics = VK_FALSE;
        struct hybris_device_features out;
        VkDeviceCreateInfo plain = device_info(NULL, &app);
        CHECK(prepare(&plain, &out) == VK_SUCCESS && out.info.pEnabledFeatures && out.info.pEnabledFeatures != &app);
        CHECK(out.info.pEnabledFeatures && !memcmp(out.info.pEnabledFeatures, &want, sizeof want) && !memcmp(&app, &before, sizeof app));
        finish(&out);

        struct dxvk_chain chain, chain_before;
        VkDeviceCreateInfo features2 = device_info(dxvk_chain(&chain, 1), NULL);
        memcpy(&chain_before, &chain, sizeof chain);
        CHECK(prepare(&features2, &out) == VK_SUCCESS && allocations == 2);
        const VkPhysicalDeviceFeatures2 *copy = (const void *)find(out.info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        CHECK(copy && copy != &chain.features2 && !memcmp(&copy->features, &want, sizeof want) && out.suffix == &chain.v11);
        CHECK(!memcmp(&chain, &chain_before, sizeof chain));
        finish(&out);
    }
    clip = stores = 0;
}

int main(void)
{
    enabled_features();
    features2_chain(1);
    features2_chain(0);
    nothing_to_strip();
    unknown_structure();
    bc_survives_the_other_filters();
    printf("BC_DEVICE_CREATE checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
