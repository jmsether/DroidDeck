/* The layer's properties2 handlers (layer/physical.c) against stubbed policies: a device handle the layer
 * does not know gets no DRM fill, even when a policy handler returns without calling the driver. */
#define VK_NO_PROTOTYPES
#include "layer/layer.h"
#include "compat/bc_policy.h"
#include "compat/clip_distance.h"
#include "compat/drm_properties.h"
#include "compat/shader_dispatch.h"
#include "compat/shader_policy.h"
#include "compat/vertex_stores.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define REGISTERED ((VkPhysicalDevice)(uintptr_t)2)
#define UNREGISTERED ((VkPhysicalDevice)(uintptr_t)1)
#define PAYLOAD offsetof(VkPhysicalDeviceDrmPropertiesEXT, hasPrimary)

static int driver_calls, clip_present;

/* The driver never touches the DRM structure. */
static void VKAPI_CALL driver_properties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out)
{ (void)physical; (void)out; ++driver_calls; }
static PFN_vkVoidFunction VKAPI_CALL resolver(VkInstance instance, const char *name)
{ (void)instance; return strstr(name, "vkGetPhysicalDeviceProperties2") ? (PFN_vkVoidFunction)driver_properties2 : NULL; }

int hybris_layer_lookup_physical(VkPhysicalDevice physical, struct hybris_layer_physical *out)
{
    *out = (struct hybris_layer_physical){0};
    if (physical != REGISTERED) return 0;
    out->resolver = resolver;
    return 1;
}
/* Like the clip handler: for an unknown device it returns without calling down. */
static void VKAPI_CALL clip_properties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out)
{
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return;
    driver_properties2(physical, out);
}
PFN_vkVoidFunction hybris_clip_policy_proc(const char *name)
{ return clip_present && strstr(name, "vkGetPhysicalDeviceProperties2") ? (PFN_vkVoidFunction)clip_properties2 : NULL; }

PFN_vkVoidFunction hybris_shader_policy_proc(const char *name) { (void)name; return NULL; }
PFN_vkVoidFunction hybris_bc_policy_proc(const char *name) { (void)name; return NULL; }
int hybris_bc_enabled(void) { return 0; }
int hybris_bc_extension_allowed(const char *name) { (void)name; return 1; }
int hybris_bc_format_properties(VkPhysicalDevice physical, VkFormat format, VkFormatProperties *properties)
{ (void)physical; (void)format; (void)properties; return 0; }
unsigned hybris_bc_physical_mask(VkPhysicalDevice physical) { (void)physical; return 0; }
int hybris_scaled_enabled(void) { return 0; }
void hybris_scaled_format(PFN_vkGetPhysicalDeviceFormatProperties query, VkPhysicalDevice physical,
                          VkFormat format, VkFormatProperties *properties)
{ (void)query; (void)physical; (void)format; (void)properties; }
int hybris_shader_extension_allowed(const char *name) { (void)name; return 1; }
unsigned hybris_shader_physical_mask(VkPhysicalDevice physical) { (void)physical; return 0; }
int hybris_vertex_stores_active(VkPhysicalDevice physical) { (void)physical; return 0; }

/* Stderr is redirected to `path`: counts the HYBRIS_DRM_PROPERTIES lines written so far. */
static unsigned drm_lines(const char *path)
{
    fflush(stderr);
    FILE *log = fopen(path, "r");
    assert(log);
    char line[512];
    unsigned lines = 0;
    while (fgets(line, sizeof line, log)) lines += !!strstr(line, "HYBRIS_DRM_PROPERTIES outcome=");
    fclose(log);
    return lines;
}

int main(void)
{
    static const char *const names[] = {"vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceProperties2KHR"};
    const char *path = "drm_test_stderr.txt";
    if (!freopen(path, "w", stderr)) return 1;

    /* The application's own values; no payload byte is the sentinel. */
    VkPhysicalDeviceDrmPropertiesEXT drm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
        .hasPrimary = VK_TRUE, .primaryMajor = 11, .primaryMinor = 12, .hasRender = VK_FALSE, .renderMajor = 42, .renderMinor = 43};
    const VkPhysicalDeviceDrmPropertiesEXT before = drm;
    VkPhysicalDeviceProperties2 out = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drm};

    /* 1. An unknown device handle: the driver is not reached, the structure is untouched, nothing is reported.
     * It must hold with and without a clip handler that returns early for an unknown device. */
    for (clip_present = 0; clip_present <= 1; ++clip_present)
        for (unsigned i = 0; i < 2; ++i) {
            PFN_vkGetPhysicalDeviceProperties2 handler = (PFN_vkGetPhysicalDeviceProperties2)hybris_layer_physical_proc(names[i]);
            assert(handler);
            handler(UNREGISTERED, &out);
            assert(driver_calls == 0 && !memcmp((char *)&drm + PAYLOAD, (const char *)&before + PAYLOAD, sizeof drm - PAYLOAD));
            assert(drm_lines(path) == 0);
        }

    /* 2. Control: a registered device does reach the driver through every route, and the DRM protocol runs
     * (one line for its outcome, which depends on the host's render node). */
    for (clip_present = 0; clip_present <= 1; ++clip_present)
        for (unsigned i = 0; i < 2; ++i) {
            PFN_vkGetPhysicalDeviceProperties2 handler = (PFN_vkGetPhysicalDeviceProperties2)hybris_layer_physical_proc(names[i]);
            int calls = driver_calls;
            handler(REGISTERED, &out);
            assert(driver_calls == calls + 1);
        }
    assert(drm_lines(path) == 1);
    printf("drm_layer: all checks passed\n");
    return 0;
}
