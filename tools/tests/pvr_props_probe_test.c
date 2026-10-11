/* Host test for the pure helpers of tools/pixel10/pvr_props_probe.c: the driver-version decode, the
 * driverInfo parse, the DRM "did the HAL touch it" classifier and the BC format rules. The probe is
 * included whole, minus its main, so the test sees exactly the code that ships. */
#define PVR_PROBE_NO_MAIN
#include "../pixel10/pvr_props_probe.c"
#include <assert.h>

static void test_driver_version(void) {
    char text[96];
    fmt_driver_version(6908880u, text, sizeof text); /* DDK 25.3 on the Pixel 10 */
    assert(!strcmp(text, "6908880 (0x696bd0) vk_decoded=1.662.3024"));
    fmt_driver_version(0, text, sizeof text);
    assert(!strcmp(text, "0 (0x0) vk_decoded=0.0.0"));
    fmt_driver_version(UINT32_MAX, text, sizeof text);
    assert(!strcmp(text, "4294967295 (0xffffffff) vk_decoded=1023.1023.4095"));
}

static void test_driver_info(void) {
    char release[32];
    uint32_t build = 7;
    assert(parse_driver_info("25.3@6908880", release, sizeof release, &build));
    assert(!strcmp(release, "25.3") && build == 6908880u);
    assert(parse_driver_info("25.1@6794074", release, sizeof release, &build) && build == 6794074u);
    assert(parse_driver_info("26.1@4294967295", release, sizeof release, &build) && build == UINT32_MAX);
    build = 7;
    assert(!parse_driver_info(NULL, release, sizeof release, &build));
    assert(!parse_driver_info("", release, sizeof release, &build));
    assert(!parse_driver_info("25.3", release, sizeof release, &build));
    assert(!parse_driver_info("@6908880", release, sizeof release, &build));
    assert(!parse_driver_info("25.3@", release, sizeof release, &build));
    assert(!parse_driver_info("25.3@x", release, sizeof release, &build));
    assert(!parse_driver_info("25.3@4294967296", release, sizeof release, &build)); /* overflows a u32 */
    assert(!parse_driver_info("25.3@6908880", release, 4, &build));                 /* "25.3" does not fit in 4 bytes */
    assert(build == 7); /* a rejected string leaves the output alone */
}

static void test_drm_classifier(void) {
    VkPhysicalDeviceDrmPropertiesEXT d;
    drm_poison(&d);
    assert(d.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT && d.pNext == NULL);
    assert(drm_classify(&d) == DRM_UNTOUCHED);

    memset((unsigned char *)&d + offsetof(VkPhysicalDeviceDrmPropertiesEXT, hasPrimary), 0,
           sizeof d - offsetof(VkPhysicalDeviceDrmPropertiesEXT, hasPrimary));
    assert(drm_classify(&d) == DRM_ZEROED);

    d.hasPrimary = d.hasRender = 1;
    d.primaryMajor = d.renderMajor = 226;
    d.primaryMinor = 2;
    d.renderMinor = 128;
    assert(drm_classify(&d) == DRM_FILLED);

    d.hasPrimary = 0; /* a render-only HAL answer is still a filled one */
    d.primaryMajor = d.primaryMinor = 0;
    assert(drm_classify(&d) == DRM_FILLED);

    d.hasRender = 2; /* not a VkBool32 anyone wrote on purpose */
    assert(drm_classify(&d) == DRM_PARTIAL);

    drm_poison(&d);
    d.hasRender = 1; /* one word written, the rest still poison */
    assert(drm_classify(&d) == DRM_PARTIAL);
}

static void test_bc_rules(void) {
    assert(bc_complete(0xd401u) && bc_sampled(0xd401u)); /* BC1-BC5 as the Pixel 10 reports them */
    assert(!bc_complete(0x1u) && bc_sampled(0x1u));      /* sampled but no blit/filter/transfer */
    assert(!bc_complete(0) && !bc_sampled(0));           /* BC6H / BC7: nothing */
    assert(!bc_complete(0xd401u & ~VK_FORMAT_FEATURE_TRANSFER_DST_BIT));

    unsigned d3d = 0;
    for (size_t i = 0; i < NFORMATS; i++) d3d += (unsigned)FORMATS[i].d3d_bc;
    assert(d3d == 14);
    assert(!FORMATS[4].d3d_bc && !FORMATS[5].d3d_bc && !strcmp(FORMATS[4].name, "BC1_RGB_UNORM_BLOCK"));
    assert(FORMATS[NFORMATS - 1].id == VK_FORMAT_BC7_SRGB_BLOCK);

    char list[16] = "";
    list_add(list, sizeof list, "AB");
    list_add(list, sizeof list, "CD");
    list_add(list, sizeof list, "this name does not fit");
    assert(!strcmp(list, "AB,CD"));
}

int main(void) {
    test_driver_version();
    test_driver_info();
    test_drm_classifier();
    test_bc_rules();
    puts("pvr_props_probe helpers: ok");
    return 0;
}
