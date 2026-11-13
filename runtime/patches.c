/* Optional improvements applied to the guest image after loading. Each patch belongs to one
 * release of the game (its "$VER:" string) and checks the original bytes first, so another
 * version is left untouched: the same addresses hold other data there.
 *
 * Note on controls: in 4.81 the default keys (player 1: Z/X steer, ' accelerate, / brake,
 * ; . gears, C view, P pause; player 2: arrows, PgUp/PgDn gears) are written by code at
 * 0x421205 as instruction immediates, not data, and can be redefined in the game's own
 * options menu. */
#include "runtime.h"

typedef struct {
    const char *name;
    const char *version;          /* prefix of the version string, e.g. "Rally 4.81 " */
    uint32_t addr;
    const uint8_t *orig;
    const uint8_t *repl;
    uint32_t len;
    const bool *enabled;
} Patch;

/* Graphics option (0 = software 8-bit, 2 = 3D card): default to the accelerated mode, which
 * the runtime renders at high resolution. The player can still change it in the game's
 * settings, and a saved var\irc.cfg overrides it. */
static const uint8_t gfx_orig[1] = {0x00};
static const uint8_t gfx_3d[1] = {0x02};

static const Patch g_patches[] = {
    {"default to accelerated 3D mode", "Rally 4.81 ", 0x45DD5A, gfx_orig, gfx_3d, 1, &g_cfg.d3d},   /* English, 9.12.98 */
    {"default to accelerated 3D mode", "Rally 4.57 ", 0x467A72, gfx_orig, gfx_3d, 1, &g_cfg.d3d},   /* Brazilian, 10.9.97 */
    {NULL, NULL, 0, NULL, NULL, 0, NULL},
};

/* The game's own version string, e.g. "Rally 4.81 (9.12.98)" */
static bool game_version(char *out, size_t cap)
{
    static const char tag[] = "$VER: Rally ";
    const uint8_t *img = g_mem + g_image_base;
    size_t n = sizeof tag - 1;
    for (uint32_t i = 0; i + n + 32 <= g_image_size; i++) {
        if (memcmp(img + i, tag, n)) continue;
        size_t k = 0;
        while (k + 1 < cap && img[i + 6 + k] >= 0x20 && img[i + 6 + k] < 0x7F) { out[k] = (char)img[i + 6 + k]; k++; }
        out[k] = 0;
        return true;
    }
    snprintf(out, cap, "unknown");
    return false;
}

void patches_apply(void)
{
    char ver[64];
    game_version(ver, sizeof ver);
    rlog("game version: %s", ver);
    int ok = 0;
    for (const Patch *p = g_patches; p->name; p++) {
        if (strncmp(ver, p->version, strlen(p->version))) continue;
        if (p->enabled && !*p->enabled) continue;
        if (memcmp(g_mem + p->addr, p->orig, p->len)) {
            rlog("patch '%s' skipped: unexpected bytes", p->name);
            continue;
        }
        memcpy(g_mem + p->addr, p->repl, p->len);
        ok++;
    }
    rlog("%d patches applied", ok);
}
