/* SPDX-License-Identifier: Apache-2.0 */
/* Host check for the pure BC rules (no Vulkan, no device):
 *   cc -std=c11 -Wall -Wextra -Werror tools/tests/hybris_bc_rules_check.c \
 *      <tree>/hybris/vulkan/compat/bc_rules.c -I <tree>/hybris/vulkan -o bc_rules_check && ./bc_rules_check */
#include "compat/bc_rules.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); } } while (0)

#define FULL HYBRIS_BC_REQUIRED
#define SAMPLED_ONLY HYBRIS_BC_SAMPLED
/* Extra feature bits an emulated format must never inherit from its backing. */
#define STORAGE 0x2u
#define COLOR_ATTACHMENT 0x80u

/* Pixel 10 shape: BC1 RGB..BC5 (0..11) fully native, BC6H/BC7 (12..15) absent. */
static void pixel10(uint32_t native[HYBRIS_BC_FORMATS], uint32_t backing[HYBRIS_BC_FORMATS])
{
    for (unsigned i = 0; i < HYBRIS_BC_FORMATS; ++i) {
        native[i] = i < 12 ? FULL : 0;
        backing[i] = FULL | STORAGE | COLOR_ATTACHMENT;
    }
}

static void parsing(void)
{
    CHECK(hybris_bc_parse_policy(NULL) == HYBRIS_BC_MISSING);
    CHECK(hybris_bc_parse_policy("missing") == HYBRIS_BC_MISSING);
    CHECK(hybris_bc_parse_policy("1") == HYBRIS_BC_MISSING);
    CHECK(hybris_bc_parse_policy("force") == HYBRIS_BC_FORCE);
    CHECK(hybris_bc_parse_policy("dxvk") == HYBRIS_BC_DXVK);
    CHECK(hybris_bc_parse_policy("d3d") == HYBRIS_BC_D3D);
    const char *off[] = {"0", "", "off", "D3D", "missing ", "forced", "2"};
    for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); ++i) CHECK(hybris_bc_parse_policy(off[i]) == HYBRIS_BC_OFF);
    CHECK(!hybris_bc_traits(HYBRIS_BC_OFF)->enabled);
    CHECK(!hybris_bc_traits(-1)->enabled);
    CHECK(!hybris_bc_traits(HYBRIS_BC_POLICIES)->enabled);
    CHECK(!hybris_bc_traits(1 << 20)->enabled);
    for (int p = HYBRIS_BC_MISSING; p < HYBRIS_BC_POLICIES; ++p) CHECK(hybris_bc_traits(p)->enabled);
}

static void api_gate(void)
{
    const uint32_t v13 = (1u << 22) | (3u << 12) | 296, v14 = HYBRIS_BC_API_1_4, v14_317 = v14 | 317;
    CHECK(hybris_bc_api_allowed(hybris_bc_traits(HYBRIS_BC_MISSING), v13));
    CHECK(!hybris_bc_api_allowed(hybris_bc_traits(HYBRIS_BC_MISSING), v14));
    CHECK(!hybris_bc_api_allowed(hybris_bc_traits(HYBRIS_BC_FORCE), v14_317));
    CHECK(hybris_bc_api_allowed(hybris_bc_traits(HYBRIS_BC_DXVK), v14_317));
    CHECK(hybris_bc_api_allowed(hybris_bc_traits(HYBRIS_BC_D3D), v14_317));
    CHECK(hybris_bc_api_allowed(hybris_bc_traits(HYBRIS_BC_D3D), v13));
}

static void selection(void)
{
    uint32_t native[HYBRIS_BC_FORMATS], backing[HYBRIS_BC_FORMATS];
    pixel10(native, backing);
    /* Only the formats the driver cannot sample are emulated. */
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_MISSING), native, backing) == 0xF000u);
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_D3D), native, backing) == 0xF000u);
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_FORCE), native, backing) == 0xFFFFu);
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_DXVK), native, backing) == 0xFFFCu);
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_OFF), native, backing) == 0);
    /* Native sampling without filtering/transfer: "missing" trusts it, "d3d" decodes it. */
    native[8] = native[9] = SAMPLED_ONLY;
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_MISSING), native, backing) == 0xF000u);
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_D3D), native, backing) == 0xF300u);
    /* BC1 RGB is never selected by the Direct3D policies, even when absent. */
    native[0] = native[1] = 0;
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_D3D), native, backing) == 0xF300u);
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_MISSING), native, backing) == 0xF003u);
    /* A backing format that cannot be sampled and copied vetoes emulation. */
    backing[12] = FULL & ~HYBRIS_BC_TRANSFER_DST;
    backing[13] = HYBRIS_BC_SAMPLED | HYBRIS_BC_TRANSFER;
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_D3D), native, backing) == 0xE300u);
    backing[14] = 0;
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_D3D), native, backing) == 0xA300u);
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_D3D), NULL, NULL) == 0);
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_D3D), native, NULL) == 0);
    /* No native answers at all: everything with a usable backing is emulated, BC1 RGB still skipped. */
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_D3D), NULL, backing) == 0xAFFCu);
}

static void exposed_features(void)
{
    CHECK(hybris_bc_emulated_features(FULL) == FULL);
    CHECK(hybris_bc_emulated_features(FULL | STORAGE | COLOR_ATTACHMENT) == FULL);
    CHECK(hybris_bc_emulated_features(HYBRIS_BC_SAMPLED | HYBRIS_BC_TRANSFER) ==
        (HYBRIS_BC_SAMPLED | HYBRIS_BC_TRANSFER));
    CHECK(hybris_bc_emulated_features(0) == 0);
}

static void feature(void)
{
    uint32_t native[HYBRIS_BC_FORMATS], backing[HYBRIS_BC_FORMATS], lacking = 99;
    const struct hybris_bc_traits *d3d = hybris_bc_traits(HYBRIS_BC_D3D);
    pixel10(native, backing);
    unsigned mask = hybris_bc_select(d3d, native, backing);
    CHECK(hybris_bc_first_gap(d3d, mask, native, backing, &lacking) == HYBRIS_BC_FORMATS && lacking == 0);
    CHECK(hybris_bc_feature_complete(d3d, mask, native, backing));
    /* Without emulation BC6H/BC7 are the gap. */
    CHECK(hybris_bc_first_gap(d3d, 0, native, backing, &lacking) == 12 && lacking == FULL);
    CHECK(!hybris_bc_feature_complete(d3d, 0, native, backing));
    /* Emulated BC7 whose backing cannot blit or filter: not complete. */
    backing[14] = backing[15] = HYBRIS_BC_SAMPLED | HYBRIS_BC_TRANSFER;
    mask = hybris_bc_select(d3d, native, backing);
    CHECK(mask == 0xF000u);
    CHECK(hybris_bc_first_gap(d3d, mask, native, backing, &lacking) == 14 &&
        lacking == (HYBRIS_BC_LINEAR | HYBRIS_BC_BLIT_SRC));
    CHECK(!hybris_bc_feature_complete(d3d, mask, native, backing));
    /* Native BC4 missing blit: "d3d" decodes it and recovers; "missing" never reports. */
    pixel10(native, backing);
    native[8] = FULL & ~HYBRIS_BC_BLIT_SRC;
    mask = hybris_bc_select(d3d, native, backing);
    CHECK(mask == 0xF100u && hybris_bc_feature_complete(d3d, mask, native, backing));
    CHECK(!hybris_bc_feature_complete(hybris_bc_traits(HYBRIS_BC_MISSING), 0xF000u, native, backing));
    CHECK(!hybris_bc_feature_complete(hybris_bc_traits(HYBRIS_BC_FORCE), 0xFFFFu, native, backing));
    CHECK(!hybris_bc_feature_complete(hybris_bc_traits(HYBRIS_BC_OFF), 0xFFFFu, native, backing));
    /* BC1 RGB is outside the Direct3D set but inside Vulkan's: a spec-strict mask sees it. */
    pixel10(native, backing);
    native[0] = native[1] = 0;
    mask = hybris_bc_select(d3d, native, backing);
    CHECK(hybris_bc_feature_complete(d3d, mask, native, backing));
    struct hybris_bc_traits strict = *d3d;
    strict.feature_formats = 0xFFFFu;
    CHECK(!hybris_bc_feature_complete(&strict, mask, native, backing));
    CHECK(hybris_bc_first_gap(&strict, mask, native, backing, NULL) == 0);
    /* Forced emulation of everything (dxvk) is complete only through the backing formats. */
    pixel10(native, backing);
    const struct hybris_bc_traits *dxvk = hybris_bc_traits(HYBRIS_BC_DXVK);
    mask = hybris_bc_select(dxvk, native, backing);
    CHECK(mask == 0xFFFCu && hybris_bc_feature_complete(dxvk, mask, native, backing));
    backing[8] = FULL & ~HYBRIS_BC_LINEAR;
    CHECK(!hybris_bc_feature_complete(dxvk, mask, native, backing));
    CHECK(hybris_bc_first_gap(dxvk, mask, native, backing, &lacking) == 8 && lacking == HYBRIS_BC_LINEAR);
    CHECK(!hybris_bc_feature_complete(d3d, 0, NULL, NULL));
}

/* The pre-existing missing/force/dxvk selection, restated per format, must not change. */
static unsigned legacy_select(int policy, const uint32_t native[HYBRIS_BC_FORMATS], const uint32_t backing[HYBRIS_BC_FORMATS])
{
    const uint32_t required = HYBRIS_BC_SAMPLED | HYBRIS_BC_TRANSFER;
    unsigned mask = 0;
    for (unsigned i = 0; i < HYBRIS_BC_FORMATS; ++i) {
        if (policy == HYBRIS_BC_DXVK && i < 2) continue;
        if (policy == HYBRIS_BC_MISSING && (native[i] & HYBRIS_BC_SAMPLED)) continue;
        if ((backing[i] & required) == required) mask |= 1u << i;
    }
    return mask;
}

static void legacy_equivalence(void)
{
    uint32_t seed = 0x2545F491u, native[HYBRIS_BC_FORMATS], backing[HYBRIS_BC_FORMATS];
    for (unsigned round = 0; round < 20000; ++round) {
        for (unsigned i = 0; i < HYBRIS_BC_FORMATS; ++i) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            native[i] = seed & 0xFFFFu;
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            backing[i] = seed & 0xFFFFu;
        }
        for (int policy = HYBRIS_BC_MISSING; policy <= HYBRIS_BC_DXVK; ++policy)
            CHECK(hybris_bc_select(hybris_bc_traits(policy), native, backing) == legacy_select(policy, native, backing));
    }
}

int main(void)
{
    parsing();
    api_gate();
    selection();
    exposed_features();
    feature();
    legacy_equivalence();
    printf("BC_RULES_CHECK checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
