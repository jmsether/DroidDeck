#define VK_NO_PROTOTYPES
#include "compat/drm_properties.h"
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PD ((VkPhysicalDevice)(uintptr_t)1)
#define PAYLOAD offsetof(VkPhysicalDeviceDrmPropertiesEXT, hasPrimary)
#define PAYLOAD_SIZE (sizeof(VkPhysicalDeviceDrmPropertiesEXT) - PAYLOAD)
#define SENTINEL 0xA5

/* Fake driver: mode 0 ignores the DRM structure, 1 answers a render node, 2 zero-fills it, 3 writes only the
 * render group, 4 only the primary group, 5 only hasRender. `seen` is the structure as the driver received it. */
static int hal_mode, hal_calls, node_calls;
static VkPhysicalDeviceDrmPropertiesEXT seen;
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
    if (!d) return;
    seen = *d;
    switch (hal_mode) {
    case 1: d->hasRender = VK_TRUE; d->renderMajor = 7; d->renderMinor = 9;
            d->hasPrimary = VK_FALSE; d->primaryMajor = d->primaryMinor = 0; break;
    case 2: d->hasPrimary = d->hasRender = VK_FALSE;
            d->primaryMajor = d->primaryMinor = d->renderMajor = d->renderMinor = 0; break;
    case 3: d->hasRender = VK_TRUE; d->renderMajor = 7; d->renderMinor = 9; break;
    case 4: d->hasPrimary = VK_TRUE; d->primaryMajor = 226; d->primaryMinor = 0; break;
    case 5: d->hasRender = VK_TRUE; break;
    default: break;
    }
}
static void VKAPI_CALL quiet_hal(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out)
{ (void)physical; (void)out; }
static struct hybris_drm_node node_226_128(void)
{ ++node_calls; return (struct hybris_drm_node){.valid = 1, .major = 226, .minor = 128}; }
static struct hybris_drm_node node_missing(void)
{ ++node_calls; return (struct hybris_drm_node){.error = ENOENT}; }
static VkPhysicalDeviceDrmPropertiesEXT fresh(void)
{ return (VkPhysicalDeviceDrmPropertiesEXT){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT}; }
/* The application's pre-initialised structure: no payload byte equals the sentinel. */
static VkPhysicalDeviceDrmPropertiesEXT app(void)
{
    VkPhysicalDeviceDrmPropertiesEXT d = fresh();
    d.hasPrimary = VK_TRUE; d.primaryMajor = 11; d.primaryMinor = 12;
    d.hasRender = VK_FALSE; d.renderMajor = 42; d.renderMinor = 43;
    return d;
}
/* Payload bytes equal to the sentinel: all of them as the driver receives it, none once the application sees it. */
static size_t sentinel_bytes(const VkPhysicalDeviceDrmPropertiesEXT *d)
{
    const unsigned char *p = (const unsigned char *)d + PAYLOAD;
    size_t n = 0;
    for (size_t i = 0; i < PAYLOAD_SIZE; ++i) n += p[i] == SENTINEL;
    return n;
}
static int same_payload(const VkPhysicalDeviceDrmPropertiesEXT *a, const VkPhysicalDeviceDrmPropertiesEXT *b)
{ return !memcmp((const char *)a + PAYLOAD, (const char *)b + PAYLOAD, PAYLOAD_SIZE); }

/* Everything that needs the fill enabled. */
static void enabled_cases(void)
{
    /* 1. Driver ignores the structure: it receives the painted payload; the node fills the render group, hasPrimary
     *    is false even where the application had set it, the rest of the chain is intact. */
    VkPhysicalDeviceDrmPropertiesEXT drm = app();
    VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, .pNext = &drm};
    VkPhysicalDeviceProperties2 out = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
    hal_mode = 0;
    assert(hybris_drm_query(fake_hal, PD, &out, node_226_128) == HYBRIS_DRM_FILLED);
    assert(sentinel_bytes(&seen) == PAYLOAD_SIZE);
    assert(drm.hasRender == VK_TRUE && drm.renderMajor == 226 && drm.renderMinor == 128);
    assert(drm.hasPrimary == VK_FALSE && drm.primaryMajor == 0 && drm.primaryMinor == 0);
    assert(drm.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT && drm.pNext == NULL);
    assert(out.pNext == &id && id.pNext == &drm && out.properties.vendorID == 0x1010);
    assert(node_calls == 1 && sentinel_bytes(&drm) == 0);

    /* 2. Driver fills it: passed through (its primary zeros win over the application's), node never stat'ed. */
    drm = app(); hal_mode = 1; node_calls = 0;
    assert(hybris_drm_query(fake_hal, PD, &out, node_226_128) == HYBRIS_DRM_DRIVER_ANSWERED);
    assert(drm.hasRender == VK_TRUE && drm.renderMajor == 7 && drm.renderMinor == 9);
    assert(drm.hasPrimary == VK_FALSE && drm.primaryMajor == 0 && drm.primaryMinor == 0);
    assert(node_calls == 0 && sentinel_bytes(&drm) == 0);

    /* 3. Driver explicitly answers "no render" with zeros: still the driver's answer (design: pass through). */
    drm = app(); hal_mode = 2;
    assert(hybris_drm_query(fake_hal, PD, &out, node_226_128) == HYBRIS_DRM_DRIVER_ANSWERED);
    assert(drm.hasRender == VK_FALSE && drm.renderMajor == 0 && drm.renderMinor == 0);
    assert(drm.hasPrimary == VK_FALSE && drm.primaryMajor == 0 && drm.primaryMinor == 0);
    assert(node_calls == 0 && sentinel_bytes(&drm) == 0);

    /* 4. Driver ignores it and there is no node: the application's own bytes come back. */
    drm = app(); hal_mode = 0;
    VkPhysicalDeviceDrmPropertiesEXT before = drm;
    assert(hybris_drm_query(fake_hal, PD, &out, node_missing) == HYBRIS_DRM_NO_NODE);
    assert(same_payload(&before, &drm));
    assert(node_calls == 1);

    /* 5. No DRM structure in the chain: driver called once, node never stat'ed, nothing touched. */
    node_calls = 0;
    VkPhysicalDeviceIDProperties id2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 no_drm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id2};
    int calls = hal_calls;
    assert(hybris_drm_query(fake_hal, PD, &no_drm, node_226_128) == HYBRIS_DRM_NOT_REQUESTED);
    assert(hal_calls == calls + 1 && node_calls == 0);
    VkPhysicalDeviceProperties2 bare = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    assert(hybris_drm_query(fake_hal, PD, &bare, node_226_128) == HYBRIS_DRM_NOT_REQUESTED);

    /* 6. A cyclic chain without the DRM structure terminates (bounded walk). */
    VkPhysicalDeviceIDProperties loop = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    loop.pNext = &loop;
    VkPhysicalDeviceProperties2 cyc = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &loop};
    assert(hybris_drm_query(quiet_hal, PD, &cyc, node_226_128) == HYBRIS_DRM_NOT_REQUESTED);

    /* 7. The two groups are judged separately and no painted byte reaches the application.
     * 7a. Driver writes only the render group: its answer passes through, the primary group is restored. */
    drm = app(); hal_mode = 3; node_calls = 0;
    assert(hybris_drm_query(fake_hal, PD, &out, node_226_128) == HYBRIS_DRM_DRIVER_ANSWERED);
    assert(drm.hasRender == VK_TRUE && drm.renderMajor == 7 && drm.renderMinor == 9);
    assert(drm.hasPrimary == VK_TRUE && drm.primaryMajor == 11 && drm.primaryMinor == 12);
    assert(node_calls == 0 && sentinel_bytes(&drm) == 0);

    /* 7b. Driver writes only the primary group: the render group is filled from the node, primary stays the driver's. */
    drm = app(); hal_mode = 4; node_calls = 0;
    assert(hybris_drm_query(fake_hal, PD, &out, node_226_128) == HYBRIS_DRM_FILLED);
    assert(drm.hasRender == VK_TRUE && drm.renderMajor == 226 && drm.renderMinor == 128);
    assert(drm.hasPrimary == VK_TRUE && drm.primaryMajor == 226 && drm.primaryMinor == 0);
    assert(node_calls == 1 && sentinel_bytes(&drm) == 0);

    /* 7c. The same without a node: the driver's primary group stays, the untouched render group is restored. */
    drm = app(); hal_mode = 4; node_calls = 0;
    assert(hybris_drm_query(fake_hal, PD, &out, node_missing) == HYBRIS_DRM_NO_NODE);
    assert(drm.hasPrimary == VK_TRUE && drm.primaryMajor == 226 && drm.primaryMinor == 0);
    assert(drm.hasRender == VK_FALSE && drm.renderMajor == 42 && drm.renderMinor == 43);
    assert(node_calls == 1 && sentinel_bytes(&drm) == 0);

    /* 7d. Driver writes only hasRender: answered; the rest of the render group and the primary group are restored. */
    drm = app(); hal_mode = 5; node_calls = 0;
    assert(hybris_drm_query(fake_hal, PD, &out, node_226_128) == HYBRIS_DRM_DRIVER_ANSWERED);
    assert(drm.hasRender == VK_TRUE && drm.renderMajor == 42 && drm.renderMinor == 43);
    assert(drm.hasPrimary == VK_TRUE && drm.primaryMajor == 11 && drm.primaryMinor == 12);
    assert(node_calls == 0 && sentinel_bytes(&drm) == 0);

    /* 8. The walk covers 64 chained structures: the DRM structure as the 64th is found, as the 65th it is not. */
    for (unsigned position = 64; position <= 65; ++position) {
        VkPhysicalDeviceIDProperties ids[64];
        VkPhysicalDeviceDrmPropertiesEXT last = app();
        for (unsigned i = 0; i < position - 1; ++i)
            ids[i] = (VkPhysicalDeviceIDProperties){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
                                                    .pNext = i + 1 < position - 1 ? (void *)&ids[i + 1] : (void *)&last};
        VkPhysicalDeviceProperties2 deep = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &ids[0]};
        int found = position == 64;
        hal_mode = 0; node_calls = 0;
        assert(hybris_drm_query(fake_hal, PD, &deep, node_226_128) == (found ? HYBRIS_DRM_FILLED : HYBRIS_DRM_NOT_REQUESTED));
        assert(node_calls == found && last.renderMajor == (found ? 226 : 42));
    }

    /* 9. Missing arguments pass straight through to the driver, never a dereference or a paint. */
    drm = app(); hal_mode = 0; node_calls = 0; calls = hal_calls;
    before = drm;
    assert(hybris_drm_query(fake_hal, PD, &out, NULL) == HYBRIS_DRM_NOT_REQUESTED);
    assert(hal_calls == calls + 1 && same_payload(&seen, &before) && same_payload(&drm, &before));
    assert(hybris_drm_query(quiet_hal, PD, NULL, node_226_128) == HYBRIS_DRM_NOT_REQUESTED);
    assert(hybris_drm_query(NULL, PD, &out, node_226_128) == HYBRIS_DRM_NOT_REQUESTED);
    assert(node_calls == 0 && hal_calls == calls + 1 && same_payload(&drm, &before));
}

/* The bypass is a plain call to the driver: nothing painted, nothing stat'ed, nothing reported. */
static void disabled_cases(void)
{
    VkPhysicalDeviceDrmPropertiesEXT drm = app(), before = drm;
    VkPhysicalDeviceProperties2 out = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drm};
    hal_mode = 0;
    assert(hybris_drm_query(fake_hal, PD, &out, node_226_128) == HYBRIS_DRM_NOT_REQUESTED);
    assert(hal_calls == 1 && node_calls == 0);
    assert(same_payload(&seen, &before) && same_payload(&drm, &before));
}

int main(void)
{
    const char *path = "drm_test_stderr.txt";
    const int disabled = getenv("EXPECT_DISABLED") != NULL;
    if (!freopen(path, "w", stderr)) return 1;
    if (disabled) disabled_cases(); else enabled_cases();

    /* 10. stat adapter: null, missing path and a directory are errors; a character device is a node. */
    struct hybris_drm_node n = hybris_drm_stat(NULL);
    assert(!n.valid && n.error == EINVAL);
    n = hybris_drm_stat("definitely/not/here");
    assert(!n.valid && n.error == ENOENT);
    n = hybris_drm_stat(".");
    assert(!n.valid && n.error == ENOTSUP);
    n = hybris_drm_stat("/dev/null");
    assert(n.valid && n.major == 1 && n.minor == 3);

    /* 10b. The escape hatch also collapses into the missing-node case for direct callers. */
    n = hybris_drm_render_node();
    if (disabled) assert(!n.valid && n.error == ECANCELED);
    else assert(n.valid || n.error != ECANCELED);   /* CI hosts have no renderD128: ENOENT is fine */

    /* 11. One diagnostic line per outcome per process, showing what the application finally sees: three outcomes,
     * each reported more than once when enabled; none at all when disabled. */
    fflush(stderr);
    FILE *log = fopen(path, "r");
    assert(log);
    char line[512];
    unsigned lines = 0, filled = 0, answered = 0, nonode = 0;
    while (fgets(line, sizeof line, log)) {
        ++lines;
        filled += strstr(line, "outcome=filled") && strstr(line, "hasRender=1 render=226:128 hasPrimary=0 primary=0:0");
        answered += strstr(line, "outcome=driver-answered") && strstr(line, "hasRender=1 render=7:9 hasPrimary=0 primary=0:0");
        nonode += strstr(line, "outcome=no-node") && strstr(line, "hasRender=0 render=42:43 hasPrimary=1 primary=11:12");
    }
    fclose(log);
    if (disabled) assert(lines == 0);
    else assert(lines == 3 && filled == 1 && answered == 1 && nonode == 1);
    printf("drm_properties: all checks passed\n");
    return 0;
}
