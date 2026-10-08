/* Optional improvements applied to the guest image after loading. Every patch checks the
 * original bytes first, so an unexpected game version is left untouched.
 *
 * Note on controls: the default keys (player 1: Z/X steer, ' accelerate, / brake, ; . gears,
 * C view, P pause; player 2: arrows, PgUp/PgDn gears) are written by code at 0x421205 as
 * instruction immediates, not data, and can be redefined in the game's own options menu. */
#include "runtime.h"

typedef struct {
    const char *name;
    uint32_t addr;
    const uint8_t *orig;
    const uint8_t *repl;
    uint32_t len;
    const bool *enabled;
} Patch;

/* Graphics option (0 = software 8-bit, 2 = 3D card): default to the accelerated mode, which
 * the runtime renders at high resolution. The player can still change it in Settings, and a
 * saved var\irc.cfg overrides it. */
static const uint8_t gfx_orig[1] = {0x00};
static const uint8_t gfx_3d[1] = {0x02};

static const Patch g_patches[] = {
    {"default to accelerated 3D mode", 0x45DD5A, gfx_orig, gfx_3d, 1, &g_cfg.d3d},
    {NULL, 0, NULL, NULL, 0, NULL},
};

void patches_apply(void)
{
    int ok = 0;
    for (const Patch *p = g_patches; p->name; p++) {
        if (p->enabled && !*p->enabled) continue;
        if (memcmp(g_mem + p->addr, p->orig, p->len)) {
            rlog("patch '%s' skipped: unexpected bytes", p->name);
            continue;
        }
        memcpy(g_mem + p->addr, p->repl, p->len);
        ok++;
    }
    if (ok) rlog("%d patches applied", ok);
}
