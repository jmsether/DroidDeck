/* pvr_props_probe.c - what the phone's Vulkan stack says about its PowerVR GPU, asked twice:
 *   loader  Android's own libvulkan.so (what an Android app sees)
 *   hal     the vendor HAL called directly through HMI / open("vk0") (what libhybris sees)
 * Both go through the one probe_source() chain; they differ only in where the entry points come from.
 *
 * Run as the adb shell user from /data/local/tmp. No app, no root, no link-time Vulkan dependency.
 *   usage: pvr_props_probe [HAL_PATH]        default /vendor/lib64/hw/vulkan.powervr.so
 * Output: one "scope.key=value" fact per line on stdout. Pipe it to a file and diff runs.
 *
 * Rail contract: strings from the driver are sanitized and length-capped before printing; every
 * enumeration is bounded (MAX_EXT, MAX_PD, MAX_NODES); a missing entry point or a failing call is
 * a printed fact, never a crash. Each section runs in a forked child with a 60 s alarm, so a driver
 * crash or hang costs that section only and is reported as "section.<name>=killed signal=N".
 * Exit status: 0 when every section ran to its end, 1 when one could not run (no libvulkan.so, no HAL,
 * no instance) or died. What the facts say never changes the status.
 *
 * ABI: hw_module_t / hw_device_t / hwvulkan_device_t are LP64 copies of AOSP hardware.h and
 * hwvulkan.h (the NDK ships neither); static asserts pin the offsets the 25.3 HAL was measured at.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#ifdef __ANDROID__
#include <android/dlext.h>
#endif

#define DEFAULT_HAL "/vendor/lib64/hw/vulkan.powervr.so"
#define MAX_EXT 1024
#define MAX_PD 4
#define MAX_NODES 64
#ifndef SECTION_TIMEOUT_S
#define SECTION_TIMEOUT_S 60
#endif

#include "hwvulkan_abi.h"

/* ---- output (the rail for everything the driver can hand us) ------------------------------ */
static void P(const char *scope, const char *key, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void P(const char *scope, const char *key, const char *fmt, ...) {
    va_list ap;
    printf("%s.%s=", scope, key);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}
/* Driver text: printable ASCII only, at most `max` bytes, NUL-terminated by us, never by trust. */
static void Pstr(const char *scope, const char *key, const char *s, size_t max) {
    char out[300];
    size_t n = 0;
    if (!s) s = "(null)";
    if (max >= sizeof out) max = sizeof out - 1;
    for (; n < max && s[n]; n++) out[n] = (s[n] >= 0x20 && s[n] < 0x7f) ? s[n] : '?';
    out[n] = 0;
    P(scope, key, "%s", out);
}

/* ---- pure helpers (host-testable: tools/tests/pvr_props_probe_test.c includes this file) --- */
#define DRM_POISON 0xCD

/* Imagination's driverVersion is the DDK build (changelist) number: 6908880 for DDK "25.3@6908880".
 * The Vulkan version macros decode the same number as 1.662.3024, which is what Android and
 * gpuinfo.org print. The DDK release ("25.3") exists only in driverInfo. */
static void fmt_driver_version(uint32_t v, char *out, size_t n) {
    snprintf(out, n, "%u (0x%x) vk_decoded=%u.%u.%u", v, v, v >> 22, (v >> 12) & 0x3ffu, v & 0xfffu);
}

/* "25.3@6908880" -> release "25.3", build 6908880. Returns 0 when the string is not that shape. */
static int parse_driver_info(const char *info, char *release, size_t n, uint32_t *build) {
    const char *at = info ? strchr(info, '@') : NULL;
    if (!at || at == info || (size_t)(at - info) >= n || at[1] < '0' || at[1] > '9') return 0;
    uint64_t v = 0;
    const char *p = at + 1;
    for (; *p >= '0' && *p <= '9'; p++) {
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > UINT32_MAX) return 0;
    }
    memcpy(release, info, (size_t)(at - info));
    release[at - info] = 0;
    *build = (uint32_t)v;
    return 1;
}

enum drm_state { DRM_UNTOUCHED, DRM_ZEROED, DRM_FILLED, DRM_PARTIAL };
static const char *const DRM_STATE_NAME[] = {"untouched", "zeroed", "filled", "partial"};

static void drm_poison(VkPhysicalDeviceDrmPropertiesEXT *d) {
    memset(d, DRM_POISON, sizeof *d);
    d->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
    d->pNext = NULL;
}
/* Classify the payload (everything after pNext). UNTOUCHED = still all poison: the HAL ignored the
 * struct, so a caller that zero-initialises reads hasRender=0. ZEROED = the HAL wrote only zeros. */
static enum drm_state drm_classify(const VkPhysicalDeviceDrmPropertiesEXT *d) {
    const unsigned char *p = (const unsigned char *)d + offsetof(VkPhysicalDeviceDrmPropertiesEXT, hasPrimary);
    size_t n = sizeof *d - offsetof(VkPhysicalDeviceDrmPropertiesEXT, hasPrimary), poison = 0, zero = 0;
    for (size_t i = 0; i < n; i++) {
        poison += p[i] == DRM_POISON;
        zero += p[i] == 0;
    }
    if (poison == n) return DRM_UNTOUCHED;
    if (zero == n) return DRM_ZEROED;
    if (poison) return DRM_PARTIAL;
    return d->hasPrimary <= 1 && d->hasRender <= 1 ? DRM_FILLED : DRM_PARTIAL;
}

/* What the Vulkan spec ("Required Format Support") requires of every BC format in optimalTilingFeatures
 * once textureCompressionBC is true. */
#define BC_MANDATED (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |      \
                     VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |                          \
                     VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)
static int bc_complete(VkFormatFeatureFlags f) { return (f & BC_MANDATED) == BC_MANDATED; }
static int bc_sampled(VkFormatFeatureFlags f) { return (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0; }

struct fmt { VkFormat id; const char *name; int d3d_bc; };
#define F(x, bc) {VK_FORMAT_##x, #x, bc}
static const struct fmt FORMATS[] = {
    F(R8G8B8A8_UNORM, 0), F(R8G8B8A8_SRGB, 0), F(B8G8R8A8_UNORM, 0), F(B8G8R8A8_SRGB, 0),
    F(BC1_RGB_UNORM_BLOCK, 0), F(BC1_RGB_SRGB_BLOCK, 0), /* not a D3D format */
    F(BC1_RGBA_UNORM_BLOCK, 1), F(BC1_RGBA_SRGB_BLOCK, 1), F(BC2_UNORM_BLOCK, 1), F(BC2_SRGB_BLOCK, 1),
    F(BC3_UNORM_BLOCK, 1), F(BC3_SRGB_BLOCK, 1), F(BC4_UNORM_BLOCK, 1), F(BC4_SNORM_BLOCK, 1),
    F(BC5_UNORM_BLOCK, 1), F(BC5_SNORM_BLOCK, 1), F(BC6H_UFLOAT_BLOCK, 1), F(BC6H_SFLOAT_BLOCK, 1),
    F(BC7_UNORM_BLOCK, 1), F(BC7_SRGB_BLOCK, 1),
};
#define NFORMATS (sizeof FORMATS / sizeof *FORMATS)

static void list_add(char *buf, size_t size, const char *name) {
    size_t used = strlen(buf);
    if (used + strlen(name) + 2 < size) snprintf(buf + used, size - used, "%s%s", used ? "," : "", name);
}

/* ---- device nodes ------------------------------------------------------------------------ */
static void report_nodes(void) {
    DIR *dir = opendir("/dev/dri");
    if (!dir) {
        P("nodes", "dri_dir", "unreadable errno=%d", errno);
    } else {
        struct dirent *e;
        for (int n = 0; n < MAX_NODES && (e = readdir(dir)); n++) {
            struct stat st;
            char path[300];
            if (e->d_name[0] == '.') continue;
            snprintf(path, sizeof path, "/dev/dri/%.200s", e->d_name);
            if (stat(path, &st)) {
                P("nodes", "dri", "%.200s stat_errno=%d", e->d_name, errno);
                continue;
            }
            P("nodes", "dri", "%.200s %s %u:%u mode=%04o uid=%u gid=%u", e->d_name, S_ISCHR(st.st_mode) ? "chr" : "other",
              major(st.st_rdev), minor(st.st_rdev), (unsigned)(st.st_mode & 07777), (unsigned)st.st_uid, (unsigned)st.st_gid);
        }
        closedir(dir);
    }
    struct stat st;
    if (!stat("/dev/dri/renderD128", &st) && S_ISCHR(st.st_mode))
        P("nodes", "renderD128", "chr %u:%u", major(st.st_rdev), minor(st.st_rdev));
    else
        P("nodes", "renderD128", "absent_or_not_chr errno=%d", errno);
}

/* ---- one source, one chain ---------------------------------------------------------------- */
struct source {
    const char *label;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkCreateInstance create;
    int is_hal;
};

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name, uint32_t *spec) {
    for (uint32_t i = 0; i < n; i++)
        if (!strncmp(e[i].extensionName, name, VK_MAX_EXTENSION_NAME_SIZE)) {
            if (spec) *spec = e[i].specVersion;
            return 1;
        }
    return 0;
}

static void report_native_buffer(const struct source *s, VkInstance inst, VkPhysicalDevice pd) {
    const char *l = s->label;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties qfp =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)s->gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)s->gipa(inst, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)s->gipa(inst, "vkGetDeviceProcAddr");
    if (!qfp || !create || !gdpa) {
        P(l, "native_buffer.device", "skipped missing_entry_point");
        return;
    }
    uint32_t nq = 0, family = UINT32_MAX;
    VkQueueFamilyProperties q[16];
    qfp(pd, &nq, NULL);
    if (nq > 16) nq = 16;
    qfp(pd, &nq, q);
    for (uint32_t i = 0; i < nq && family == UINT32_MAX; i++)
        if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = i;
    if (family == UINT32_MAX) {
        P(l, "native_buffer.device", "skipped no_graphics_queue");
        return;
    }
    float prio = 1.0f;
    const char *names[] = {"VK_ANDROID_native_buffer"};
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = names};
    VkDevice dev = VK_NULL_HANDLE;
    VkResult r = create(pd, &dci, NULL, &dev);
    P(l, "native_buffer.device", "vkCreateDevice=%d family=%u", r, family);
    if (r != VK_SUCCESS || !dev) return;
    PFN_gralloc1 g1 = (PFN_gralloc1)gdpa(dev, "vkGetSwapchainGrallocUsageANDROID");
    PFN_gralloc2 g2 = (PFN_gralloc2)gdpa(dev, "vkGetSwapchainGrallocUsage2ANDROID");
    PFN_gralloc3 g3 = (PFN_gralloc3)gdpa(dev, "vkGetSwapchainGrallocUsage3ANDROID");
    PFN_gralloc4 g4 = (PFN_gralloc4)gdpa(dev, "vkGetSwapchainGrallocUsage4ANDROID");
    P(l, "native_buffer.gralloc_fns", "v1=%d v2=%d v3=%d v4=%d (icd.c calls v2)", g1 != NULL, g2 != NULL, g3 != NULL, g4 != NULL);
    const VkFormat fmts[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    const char *const fmt_names[] = {"RGBA8", "BGRA8"};
    const VkImageUsageFlags uses[] = {
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT};
    for (size_t f = 0; f < 2; f++)
        for (size_t u = 0; u < 3; u++) {
            char key[48];
            snprintf(key, sizeof key, "native_buffer.usage.%s.use0x%x", fmt_names[f], (unsigned)uses[u]);
            if (g2) {
                uint64_t consumer = 0, producer = 0;
                VkResult rr = g2(dev, fmts[f], uses[u], 0, &consumer, &producer);
                P(l, key, "v2 result=%d consumer=0x%llx producer=0x%llx exceeds_32_bits=%d", rr, (unsigned long long)consumer,
                  (unsigned long long)producer, ((consumer | producer) >> 32) != 0);
            }
            if (g1) {
                int usage = 0;
                VkResult rr = g1(dev, fmts[f], uses[u], &usage);
                P(l, key, "v1 result=%d usage=0x%x", rr, (unsigned)usage);
            }
            if (g3) {
                GrallocInfo3 gi = {ANB_ENUM(3), NULL, fmts[f], uses[u]};
                uint64_t usage = 0;
                VkResult rr = g3(dev, &gi, &usage);
                P(l, key, "v3 result=%d usage=0x%llx exceeds_32_bits=%d", rr, (unsigned long long)usage, (usage >> 32) != 0);
            }
            if (g4) {
                GrallocInfo4 gi = {ANB_ENUM(4), NULL, fmts[f], uses[u], 0};
                uint64_t usage = 0;
                VkResult rr = g4(dev, &gi, &usage);
                P(l, key, "v4 result=%d usage=0x%llx exceeds_32_bits=%d", rr, (unsigned long long)usage, (usage >> 32) != 0);
            }
        }
    PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)gdpa(dev, "vkDestroyDevice");
    if (destroy) destroy(dev, NULL);
}

static void report_device(const struct source *s, VkInstance inst, VkPhysicalDevice pd, unsigned index) {
    const char *l = s->label;
    PFN_vkGetPhysicalDeviceProperties gpp = (PFN_vkGetPhysicalDeviceProperties)s->gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceProperties2 gpp2 = (PFN_vkGetPhysicalDeviceProperties2)s->gipa(inst, "vkGetPhysicalDeviceProperties2");
    if (!gpp2) gpp2 = (PFN_vkGetPhysicalDeviceProperties2)s->gipa(inst, "vkGetPhysicalDeviceProperties2KHR");
    PFN_vkGetPhysicalDeviceFeatures gpf = (PFN_vkGetPhysicalDeviceFeatures)s->gipa(inst, "vkGetPhysicalDeviceFeatures");
    PFN_vkGetPhysicalDeviceFormatProperties gfp =
        (PFN_vkGetPhysicalDeviceFormatProperties)s->gipa(inst, "vkGetPhysicalDeviceFormatProperties");
    PFN_vkEnumerateDeviceExtensionProperties edep =
        (PFN_vkEnumerateDeviceExtensionProperties)s->gipa(inst, "vkEnumerateDeviceExtensionProperties");
    if (!gpp || !edep) {
        P(l, "device", "%u skipped missing_entry_point", index);
        return;
    }

    VkPhysicalDeviceProperties p;
    memset(&p, 0, sizeof p);
    gpp(pd, &p);
    char ver[96];
    P(l, "device.index", "%u", index);
    Pstr(l, "device.name", p.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE);
    P(l, "device.vendorID", "0x%x", p.vendorID);
    P(l, "device.deviceID", "0x%x", p.deviceID);
    P(l, "device.type", "%d", (int)p.deviceType);
    P(l, "device.apiVersion", "%u.%u.%u", VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
      VK_API_VERSION_PATCH(p.apiVersion));
    fmt_driver_version(p.driverVersion, ver, sizeof ver);
    P(l, "device.driverVersion", "%s", ver);

    static VkExtensionProperties ext[MAX_EXT];
    uint32_t ne = 0;
    VkResult er = edep(pd, NULL, &ne, NULL);
    if (ne > MAX_EXT) ne = MAX_EXT;
    if (er == VK_SUCCESS || er == VK_INCOMPLETE) er = edep(pd, NULL, &ne, ext);
    if (er != VK_SUCCESS && er != VK_INCOMPLETE) ne = 0;
    P(l, "ext.count", "%u (enumerate result=%d)", ne, er);
    for (uint32_t i = 0; i < ne; i++) {
        ext[i].extensionName[VK_MAX_EXTENSION_NAME_SIZE - 1] = 0;
        Pstr(l, "ext", ext[i].extensionName, VK_MAX_EXTENSION_NAME_SIZE);
    }
    static const char *const WATCH[] = {"VK_EXT_physical_device_drm", "VK_ANDROID_native_buffer", "VK_KHR_driver_properties",
                                        "VK_EXT_mutable_descriptor_type", "VK_EXT_memory_budget", "VK_EXT_external_memory_host",
                                        "VK_EXT_map_memory_placed", "VK_KHR_dynamic_rendering", "VK_EXT_image_drm_format_modifier",
                                        "VK_EXT_external_memory_dma_buf", "VK_KHR_external_memory_fd"};
    for (size_t w = 0; w < sizeof WATCH / sizeof *WATCH; w++) {
        uint32_t spec = 0;
        int has = has_ext(ext, ne, WATCH[w], &spec);
        P(l, "has", "%s=%d spec=%u", WATCH[w], has, has ? spec : 0);
    }

    if (gpp2 && (p.apiVersion >= VK_API_VERSION_1_2 || has_ext(ext, ne, "VK_KHR_driver_properties", NULL))) {
        VkPhysicalDeviceDriverProperties drv;
        memset(&drv, 0, sizeof drv);
        drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
        gpp2(pd, &p2);
        drv.driverName[VK_MAX_DRIVER_NAME_SIZE - 1] = 0;
        drv.driverInfo[VK_MAX_DRIVER_INFO_SIZE - 1] = 0;
        P(l, "driver.id", "%d", (int)drv.driverID);
        Pstr(l, "driver.name", drv.driverName, VK_MAX_DRIVER_NAME_SIZE);
        Pstr(l, "driver.info", drv.driverInfo, VK_MAX_DRIVER_INFO_SIZE);
        char release[32];
        uint32_t build = 0;
        if (parse_driver_info(drv.driverInfo, release, sizeof release, &build))
            P(l, "driver.release", "%s build=%u build_matches_driverVersion=%d", release, build, build == p.driverVersion);
        else
            P(l, "driver.release", "unparsed");
    } else {
        P(l, "driver.info", "unavailable");
    }

    if (gpp2 && has_ext(ext, ne, "VK_EXT_physical_device_drm", NULL)) {
        VkPhysicalDeviceDrmPropertiesEXT drm;
        drm_poison(&drm);
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drm};
        gpp2(pd, &p2);
        P(l, "drm.state", "%s", DRM_STATE_NAME[drm_classify(&drm)]);
        P(l, "drm.fields", "hasPrimary=0x%x hasRender=0x%x primary=%lld:%lld render=%lld:%lld", drm.hasPrimary, drm.hasRender,
          (long long)drm.primaryMajor, (long long)drm.primaryMinor, (long long)drm.renderMajor, (long long)drm.renderMinor);
    } else {
        P(l, "drm.state", "not_advertised");
    }

    if (gpf) {
        VkPhysicalDeviceFeatures f;
        memset(&f, 0, sizeof f);
        gpf(pd, &f);
        P(l, "features", "textureCompressionBC=%u textureCompressionETC2=%u textureCompressionASTC_LDR=%u", f.textureCompressionBC,
          f.textureCompressionETC2, f.textureCompressionASTC_LDR);
    }

    if (gfp) {
        char incomplete[320] = "", unsampled[320] = "";
        unsigned complete = 0, d3d = 0;
        for (size_t i = 0; i < NFORMATS; i++) {
            VkFormatProperties fp;
            memset(&fp, 0, sizeof fp);
            gfp(pd, FORMATS[i].id, &fp);
            P(l, "fmt", "%s optimal=0x%08x linear=0x%08x buffer=0x%08x sampled=%d bc_mandated_complete=%d", FORMATS[i].name,
              fp.optimalTilingFeatures, fp.linearTilingFeatures, fp.bufferFeatures, bc_sampled(fp.optimalTilingFeatures),
              bc_complete(fp.optimalTilingFeatures));
            if (!FORMATS[i].d3d_bc) continue;
            d3d++;
            if (bc_complete(fp.optimalTilingFeatures)) complete++;
            else if (bc_sampled(fp.optimalTilingFeatures)) list_add(incomplete, sizeof incomplete, FORMATS[i].name);
            else list_add(unsampled, sizeof unsampled, FORMATS[i].name);
        }
        P(l, "bc.d3d_native_complete", "%u/%u", complete, d3d);
        P(l, "bc.d3d_sampled_but_incomplete", "%s", incomplete);
        P(l, "bc.d3d_not_sampleable", "%s", unsampled);
    }

    /* What the BC policy checks: it refuses emulation when any graphics/compute/transfer family lacks
     * compute or reports a transfer granularity other than 1x1x1. */
    PFN_vkGetPhysicalDeviceQueueFamilyProperties gqf =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)s->gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (gqf) {
        VkQueueFamilyProperties qf[16];
        uint32_t nq = 0;
        memset(qf, 0, sizeof qf);
        gqf(pd, &nq, NULL);
        if (nq > 16) nq = 16;
        gqf(pd, &nq, qf);
        P(l, "queue.count", "%u", nq);
        for (uint32_t i = 0; i < nq; i++)
            P(l, "queue.family", "%u flags=0x%x count=%u granularity=%ux%ux%u", i, qf[i].queueFlags, qf[i].queueCount,
              qf[i].minImageTransferGranularity.width, qf[i].minImageTransferGranularity.height,
              qf[i].minImageTransferGranularity.depth);
    }

    uint32_t anb_spec = 0;
    if (has_ext(ext, ne, "VK_ANDROID_native_buffer", &anb_spec)) {
        P(l, "native_buffer.spec_version", "%u (icd needs >= 8)", anb_spec);
        report_native_buffer(s, inst, pd);
    }
}

/* Request Vulkan 1.4 first (what the system loader gives an app), then 1.1 (what the HAL accepted
 * from Fizzolas's probe). The ladder is data, not two code paths. */
static int probe_source(const struct source *s) {
    const char *l = s->label;
    static const uint32_t LADDER[] = {VK_MAKE_API_VERSION(0, 1, 4, 0), VK_MAKE_API_VERSION(0, 1, 1, 0)};
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (!s->gipa || !s->create) {
        P(l, "instance", "no_entry_points");
        return 2;
    }
    for (size_t i = 0; i < 2 && inst == VK_NULL_HANDLE; i++) {
        VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "pvr_props_probe",
                                 .apiVersion = LADDER[i]};
        VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
        r = s->create(&ici, NULL, &inst);
        P(l, "instance", "vkCreateInstance(api %u.%u)=%d", VK_API_VERSION_MAJOR(LADDER[i]), VK_API_VERSION_MINOR(LADDER[i]), r);
        if (r != VK_SUCCESS) inst = VK_NULL_HANDLE;
    }
    if (inst == VK_NULL_HANDLE) return 2;
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)s->gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)s->gipa(inst, "vkDestroyInstance");
    VkPhysicalDevice pds[MAX_PD];
    uint32_t n = MAX_PD;
    int rc = 0;
    if (!enumerate) {
        P(l, "devices", "no vkEnumeratePhysicalDevices");
        rc = 2;
    } else {
        r = enumerate(inst, &n, pds);
        P(l, "devices", "count=%u result=%d", n, r);
        for (uint32_t i = 0; i < n && i < MAX_PD; i++) report_device(s, inst, pds[i], i);
    }
    if (destroy) destroy(inst, NULL);
    return rc;
}

/* ---- the two sources ----------------------------------------------------------------------- */
static int run_loader(const char *unused) {
    (void)unused;
    void *lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        Pstr("loader", "load_error", dlerror(), 200);
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    struct source s = {"loader", gipa, gipa ? (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance") : NULL, 0};
    PFN_vkEnumerateInstanceVersion eiv = gipa ? (PFN_vkEnumerateInstanceVersion)gipa(NULL, "vkEnumerateInstanceVersion") : NULL;
    uint32_t v = 0;
    if (eiv && eiv(&v) == VK_SUCCESS)
        P("loader", "instance_version", "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
    return probe_source(&s);
}

/* Plain dlopen first. If the shell domain's namespace refuses the vendor path, take the exported
 * "sphal" namespace the way libhybris's ICD and Android's own loader do. */
static void *load_hal(const char *path, const char **how) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h) {
        *how = "plain";
        return h;
    }
    Pstr("hal", "load_plain_error", dlerror(), 250);
#ifdef __ANDROID__
    typedef struct android_namespace_t *(*get_ns_fn)(const char *);
    void *dla = dlopen("libdl_android.so", RTLD_NOW);
    get_ns_fn get_ns = dla ? (get_ns_fn)dlsym(dla, "__loader_android_get_exported_namespace") : NULL;
    struct android_namespace_t *ns = get_ns ? get_ns("sphal") : NULL;
    P("hal", "sphal_namespace", "%s", ns ? "found" : "unavailable");
    if (ns) {
        android_dlextinfo ext = {.flags = ANDROID_DLEXT_USE_NAMESPACE, .library_namespace = ns};
        h = android_dlopen_ext(path, RTLD_NOW | RTLD_LOCAL, &ext);
        if (h) {
            *how = "sphal";
            return h;
        }
        Pstr("hal", "load_sphal_error", dlerror(), 250);
    }
#endif
    *how = "failed";
    return NULL;
}

static int run_hal(const char *path) {
    const char *how = "failed";
    P("hal", "path", "%s", path);
    void *h = load_hal(path, &how);
    P("hal", "load", "%s", how);
    if (!h) return 2;
    hw_module_t *m = (hw_module_t *)dlsym(h, "HMI");
    if (!m) {
        P("hal", "module", "no HMI symbol");
        return 2;
    }
    P("hal", "module.tag", "0x%08x (%s)", m->tag, m->tag == HW_MODULE_TAG ? "HWMT ok" : "unexpected");
    Pstr("hal", "module.id", m->id, 64);
    Pstr("hal", "module.name", m->name, 128);
    Pstr("hal", "module.author", m->author, 128);
    P("hal", "module.api", "module_api_version=0x%04x hal_api_version=0x%04x", m->module_api_version, m->hal_api_version);
    if (!m->methods || !m->methods->open) {
        P("hal", "module", "no methods->open");
        return 2;
    }
    hwvulkan_device_t *dev = NULL;
    int rc = m->methods->open(m, "vk0", (hw_device_t **)&dev);
    P("hal", "open_vk0", "rc=%d dev=%s", rc, dev ? "non-null" : "null");
    if (rc || !dev) return 2;
    uint32_t v = dev->common.version;
    P("hal", "device.tag", "0x%08x (%s)", dev->common.tag, dev->common.tag == HW_DEVICE_TAG ? "HWDT ok" : "unexpected");
    P("hal", "device.version", "0x%08x accepted_by_upstream_hybris=%d accepted_by_powervr_patch=%d", v, v == HWVK_DEVICE_API_0_1,
      v == HWVK_DEVICE_API_0_1 || v == HW_DEVICE_API_0_1);
    P("hal", "device.entry_points", "CreateInstance=%d GetInstanceProcAddr=%d EnumerateInstanceExtensionProperties=%d",
      dev->CreateInstance != NULL, dev->GetInstanceProcAddr != NULL, dev->EnumerateInstanceExtensionProperties != NULL);
    if (!dev->CreateInstance || !dev->GetInstanceProcAddr) return 2;
    if (dev->EnumerateInstanceExtensionProperties) {
        static VkExtensionProperties ie[MAX_EXT];
        uint32_t n = MAX_EXT;
        VkResult r = dev->EnumerateInstanceExtensionProperties(NULL, &n, ie);
        if (n > MAX_EXT) n = MAX_EXT;
        P("hal", "instance_ext.count", "%u (result=%d)", n, r);
        for (uint32_t i = 0; i < n && r == VK_SUCCESS; i++) {
            ie[i].extensionName[VK_MAX_EXTENSION_NAME_SIZE - 1] = 0;
            Pstr("hal", "instance_ext", ie[i].extensionName, VK_MAX_EXTENSION_NAME_SIZE);
        }
    }
    struct source s = {"hal", dev->GetInstanceProcAddr, dev->CreateInstance, 1};
    return probe_source(&s);
}

/* ---- orchestration -------------------------------------------------------------------------- */
static int isolated(const char *name, int (*fn)(const char *), const char *arg) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        P("section", name, "fork_failed errno=%d", errno);
        return 1;
    }
    if (pid == 0) {
        alarm(SECTION_TIMEOUT_S);
        int rc = fn(arg);
        fflush(stdout);
        _exit(rc);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (WIFSIGNALED(st)) {
        P("section", name, "killed signal=%d", WTERMSIG(st));
        return 1;
    }
    P("section", name, "exit=%d", WEXITSTATUS(st));
    return WEXITSTATUS(st) != 0;
}

#ifndef PVR_PROBE_NO_MAIN
int main(int argc, char **argv) {
    const char *hal = argc > 1 && argv[1][0] ? argv[1] : DEFAULT_HAL;
    setvbuf(stdout, NULL, _IOLBF, 0);
    P("probe", "version", "1");
    P("probe", "uid", "%u", (unsigned)getuid());
    FILE *f = fopen("/proc/self/attr/current", "r");
    char ctx[128] = "";
    if (f) {
        if (!fgets(ctx, sizeof ctx, f)) ctx[0] = 0;
        fclose(f);
    }
    Pstr("probe", "selinux_context", ctx, 100);
    P("probe", "page_size", "%ld", sysconf(_SC_PAGESIZE));
    report_nodes();
    int bad = isolated("loader", run_loader, NULL);
    bad |= isolated("hal", run_hal, hal);
    return bad;
}
#endif
