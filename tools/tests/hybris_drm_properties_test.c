#define VK_NO_PROTOTYPES
#include "compat/drm_properties.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fake driver: mode 0 ignores the DRM structure, 1 answers hasRender, 2 answers "no render node". */
static int hal_mode, hal_calls, node_calls;
static VkPhysicalDeviceDrmPropertiesEXT *drm_in(VkPhysicalDeviceProperties2 *out)
{
    for (VkBaseOutStructure *n = out->pNext; n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT) return (void *)n;
    return NULL;
}
static void VKAPI_CALL fake_hal(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out)
{
    (void)physical;
    ++hal_calls;
    out->properties.vendorID = 0x1010; out->properties.deviceID = 0x1234; out->properties.driverVersion = 0x19030000;
    VkPhysicalDeviceDrmPropertiesEXT *d = drm_in(out);
    if (!d || !hal_mode) return;
    if (hal_mode == 1) { d->hasPrimary = VK_FALSE; d->hasRender = VK_TRUE; d->renderMajor = 7; d->renderMinor = 9;
                         d->primaryMajor = d->primaryMinor = 0; }
    else { d->hasPrimary = d->hasRender = VK_FALSE; d->primaryMajor = d->primaryMinor = d->renderMajor = d->renderMinor = 0; }
}
static void VKAPI_CALL quiet_hal(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out)
{ (void)physical; (void)out; }
static struct hybris_drm_node node_226_128(void)
{ ++node_calls; return (struct hybris_drm_node){.valid = 1, .major = 226, .minor = 128}; }
static struct hybris_drm_node node_missing(void)
{ ++node_calls; return (struct hybris_drm_node){.error = ENOENT}; }
static VkPhysicalDeviceDrmPropertiesEXT fresh(void)
{ return (VkPhysicalDeviceDrmPropertiesEXT){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT}; }

int main(void)
{
    VkPhysicalDevice pd = (VkPhysicalDevice)(uintptr_t)1;
    const char *path = "drm_test_stderr.txt";
    assert(freopen(path, "w", stderr));

    /* 1. Driver ignores the structure: filled from the node, hasPrimary false, rest of chain intact. */
    VkPhysicalDeviceDrmPropertiesEXT drm = fresh();
    VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, .pNext = &drm};
    VkPhysicalDeviceProperties2 out = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
    hal_mode = 0;
    assert(hybris_drm_query(fake_hal, pd, &out, node_226_128) == HYBRIS_DRM_FILLED);
    assert(drm.hasRender == VK_TRUE && drm.renderMajor == 226 && drm.renderMinor == 128);
    assert(drm.hasPrimary == VK_FALSE && drm.primaryMajor == 0 && drm.primaryMinor == 0);
    assert(drm.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT && drm.pNext == NULL);
    assert(out.pNext == &id && id.pNext == &drm && out.properties.vendorID == 0x1010);
    assert(node_calls == 1);

    /* 2. Driver fills it: passed through, and the node is never stat'ed. */
    drm = fresh(); hal_mode = 1; node_calls = 0;
    assert(hybris_drm_query(fake_hal, pd, &out, node_226_128) == HYBRIS_DRM_DRIVER_ANSWERED);
    assert(drm.hasRender == VK_TRUE && drm.renderMajor == 7 && drm.renderMinor == 9);
    assert(node_calls == 0);

    /* 3. Driver explicitly answers "no render": still the driver's answer (design: pass through). */
    drm = fresh(); hal_mode = 2;
    assert(hybris_drm_query(fake_hal, pd, &out, node_226_128) == HYBRIS_DRM_DRIVER_ANSWERED);
    assert(drm.hasRender == VK_FALSE && drm.renderMajor == 0);
    assert(node_calls == 0);

    /* 4. Driver ignores it and there is no node: the application's own bytes come back. */
    drm = fresh(); hal_mode = 0;
    drm.hasRender = 0; drm.renderMajor = 42;                      /* application pre-initialised */
    VkPhysicalDeviceDrmPropertiesEXT before = drm;
    assert(hybris_drm_query(fake_hal, pd, &out, node_missing) == HYBRIS_DRM_NO_NODE);
    assert(!memcmp(&before, &drm, sizeof drm));
    assert(node_calls == 1);

    /* 5. No DRM structure in the chain: driver called once, node never stat'ed, nothing touched. */
    node_calls = 0;
    VkPhysicalDeviceIDProperties id2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 no_drm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id2};
    int calls = hal_calls;
    assert(hybris_drm_query(fake_hal, pd, &no_drm, node_226_128) == HYBRIS_DRM_NOT_REQUESTED);
    assert(hal_calls == calls + 1 && node_calls == 0);
    VkPhysicalDeviceProperties2 bare = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    assert(hybris_drm_query(fake_hal, pd, &bare, node_226_128) == HYBRIS_DRM_NOT_REQUESTED);

    /* 6. A cyclic chain without the DRM structure terminates (bounded walk). */
    VkPhysicalDeviceIDProperties loop = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    loop.pNext = &loop;
    VkPhysicalDeviceProperties2 cyc = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &loop};
    assert(hybris_drm_query(quiet_hal, pd, &cyc, node_226_128) == HYBRIS_DRM_NOT_REQUESTED);

    /* 7. stat adapter: null, missing path and a directory are errors; a character device is a node. */
    struct hybris_drm_node n = hybris_drm_stat(NULL);
    assert(!n.valid && n.error == EINVAL);
    n = hybris_drm_stat("definitely/not/here");
    assert(!n.valid && n.error == ENOENT);
    n = hybris_drm_stat(".");
    assert(!n.valid && n.error == ENOTSUP);
#ifdef _WIN32
    n = hybris_drm_stat("NUL");
    assert(n.valid && n.error == 0);
#else
    n = hybris_drm_stat("/dev/null");
    assert(n.valid && n.major == 1 && n.minor == 3);
#endif

    /* 7b. The escape hatch collapses into the missing-node case. */
    n = hybris_drm_render_node();
    if (getenv("EXPECT_DISABLED")) assert(!n.valid && n.error == ECANCELED);
    else assert(n.valid || n.error != ECANCELED);   /* CI hosts have no renderD128: ENOENT is fine */

    /* 8. One diagnostic line per outcome per process (3 outcomes, each reported more than once). */
    fflush(stderr);
    FILE *log = fopen(path, "r");
    assert(log);
    char line[512];
    unsigned lines = 0, filled = 0, answered = 0, nonode = 0;
    while (fgets(line, sizeof line, log)) {
        ++lines;
        filled += !!strstr(line, "outcome=filled");
        answered += !!strstr(line, "outcome=driver-answered");
        nonode += !!strstr(line, "outcome=no-node");
    }
    fclose(log);
    assert(lines == 3 && filled == 1 && answered == 1 && nonode == 1);
    printf("drm_properties: all checks passed\n");
    return 0;
}
