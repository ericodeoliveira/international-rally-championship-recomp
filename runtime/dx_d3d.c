/* Direct3D 5 immediate mode: IDirect3D2, IDirect3DDevice2, IDirect3DViewport2,
 * IDirect3DMaterial2 and IDirect3DTexture2. The game sends pre-transformed vertices
 * (D3DTLVERTEX) with DrawPrimitive; they are rasterized by d3d_raster.c into the
 * device's render target surface (RGB565), which then goes through the normal
 * DirectDraw presentation path. */
#include <stdlib.h>
#include "ddraw_int.h"
#include "d3d_raster.h"

#define D3D_OK 0
#define DDERR_INVALIDOBJECT 0x88760082u

/* ---------------------------------------------------------------- GUIDs */
static bool guid_is(const uint8_t *g, uint32_t d1, uint16_t d2, uint16_t d3, uint64_t d4be)
{
    uint8_t ref[16];
    memcpy(ref, &d1, 4);
    memcpy(ref + 4, &d2, 2);
    memcpy(ref + 6, &d3, 2);
    for (int i = 0; i < 8; i++) ref[8 + i] = (uint8_t)(d4be >> (56 - 8 * i));
    return !memcmp(g, ref, 16);
}
#define IS_IID_D3D2(g) guid_is(g, 0x6AAE1EC1, 0x662A, 0x11D0, 0x889D00AA00BBB76Aull)
#define IS_IID_D3D(g) guid_is(g, 0x3BBA0080, 0x2421, 0x11CF, 0xA31A00AA00B93356ull)
#define IS_IID_TEX2(g) guid_is(g, 0x93281502, 0x8CF8, 0x11D0, 0x89AB00A0C9054129ull)
#define IS_IID_TEX(g) guid_is(g, 0x2CDCD9E0, 0x25A0, 0x11CF, 0xA31A00AA00B93356ull)
static const uint8_t GUID_HAL[16] = {0xE0, 0x3D, 0xE6, 0x84, 0xAA, 0x46, 0xCF, 0x11, 0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E};

/* --------------------------------------------------------------- objects */
typedef struct { uint32_t obj; float diffuse[4]; uint32_t handle; } Material;
typedef struct {
    uint32_t obj;
    int x, y, w, h;
    uint32_t background;        /* material handle */
    struct Device *dev;
} Viewport;

typedef struct Device {
    uint32_t obj;
    Surface *target;
    Viewport *viewport;
    uint32_t rs[256];
    uint32_t ls[16];
    int prims, tris;
} Device;

static uint32_t vt_d3d, vt_dev, vt_vp, vt_mat, vt_tex;
static uint32_t g_d3d_obj;
static Device *g_dev;

#define MAX_HANDLES 4096
static Surface *g_tex_handles[MAX_HANDLES];
static int g_ntex = 1;
static Material *g_mat_handles[MAX_HANDLES];
static int g_nmat = 1;

static void *host(uint32_t obj) { return dd_host_of(obj); }

/* ------------------------------------------------- high resolution shadow */
static inline uint32_t xrgb_of_565(uint32_t v)
{
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

static void hd_put_block(Surface *s, int x, int y, uint32_t c)
{
    HDBuf *hd = s->hd;
    int S = hd->scale, W = s->w * S;
    uint32_t *p = hd->px + (size_t)(y * S) * W + (size_t)x * S;
    for (int j = 0; j < S; j++, p += W)
        for (int i = 0; i < S; i++) p[i] = c;
}

static HDBuf *hd_get(Surface *s)
{
    if (s->hd) return s->hd;
    int S = g_cfg.render_scale;
    if (S <= 0) {
        /* auto: match the screen; e.g. 1080p -> 2x (1280x960), 1440p and above -> 3x (1920x1440) */
        int oh = video_output_height();
        S = oh > 0 ? (oh + s->h / 2) / s->h : 2;
        if (S > 3) S = 3;
    }
    if (S < 1) S = 1;
    if (S > 8) S = 8;
    HDBuf *hd = calloc(1, sizeof *hd);
    hd->scale = S;
    hd->px = malloc((size_t)s->w * S * s->h * S * 4);
    hd->snap = malloc((size_t)s->w * s->h * 2);
    s->hd = hd;
    for (int y = 0; y < s->h; y++) {
        const uint16_t *row = (const uint16_t *)(g_mem + s->mem + (uint32_t)(y * s->pitch));
        memcpy(hd->snap + (size_t)y * s->w, row, (size_t)s->w * 2);
        for (int x = 0; x < s->w; x++) hd_put_block(s, x, y, xrgb_of_565(row[x]));
    }
    rlog("3D render target %dx%d rendered at %dx%d", s->w, s->h, s->w * S, s->h * S);
    return hd;
}

uint64_t g_prof_ns[4];   /* raster, downsample, merge, tris */

static void prof_report(void)
{
    static uint64_t last;
    uint64_t now = SDL_GetTicks();
    if (!g_cfg.trace || now - last < 5000) return;
    if (last) rlog("d3d time/5s: raster %.0f ms, downsample %.0f ms, merge %.0f ms, %llu tris",
                   g_prof_ns[0] / 1e6, g_prof_ns[1] / 1e6, g_prof_ns[2] / 1e6, (unsigned long long)g_prof_ns[3]);
    memset(g_prof_ns, 0, sizeof g_prof_ns);
    last = now;
}

void d3d_before_cpu_access(Surface *s)
{
    uint64_t t0 = SDL_GetTicksNS();
    if (!s || !s->hd || !s->hd->ahead) return;
    raster_flush();
    HDBuf *hd = s->hd;
    int S = hd->scale, W = s->w * S;
    for (int y = 0; y < s->h; y++) {
        uint16_t *row = (uint16_t *)(g_mem + s->mem + (uint32_t)(y * s->pitch));
        for (int x = 0; x < s->w; x++) {
            uint32_t p = hd->px[(size_t)(y * S + S / 2) * W + (size_t)x * S + S / 2];
            row[x] = (uint16_t)((((p >> 19) & 31) << 11) | (((p >> 10) & 63) << 5) | ((p >> 3) & 31));
        }
        memcpy(hd->snap + (size_t)y * s->w, row, (size_t)s->w * 2);
    }
    hd->ahead = false;
    g_prof_ns[1] += SDL_GetTicksNS() - t0;
}

void d3d_after_cpu_write(Surface *s)
{
    if (!s || !s->hd) return;
    uint64_t t0 = SDL_GetTicksNS();
    HDBuf *hd = s->hd;
    for (int y = 0; y < s->h; y++) {
        const uint16_t *row = (const uint16_t *)(g_mem + s->mem + (uint32_t)(y * s->pitch));
        uint16_t *snap = hd->snap + (size_t)y * s->w;
        if (!memcmp(row, snap, (size_t)s->w * 2)) continue;
        for (int x = 0; x < s->w; x++)
            if (row[x] != snap[x]) {
                hd_put_block(s, x, y, xrgb_of_565(row[x]));
                snap[x] = row[x];
            }
    }
    g_prof_ns[2] += SDL_GetTicksNS() - t0;
    prof_report();
}

/* Before a surface is shown: the game may write 2D (e.g. the pause menu text) through a
 * pointer kept from an earlier Lock, after Unlock, so merge whatever changed since. */
void d3d_before_present(Surface *s)
{
    if (!s || !s->hd) return;
    if (s->hd->ahead) raster_flush();
    d3d_after_cpu_write(s);
}

static void d3d_init(void);

uint32_t d3d_query_interface(const uint8_t *iid)
{
    if (!g_cfg.d3d) return 0;
    if (IS_IID_D3D2(iid) || IS_IID_D3D(iid)) {
        d3d_init();
        return g_d3d_obj;
    }
    return 0;
}

uint32_t d3d_surface_query_interface(void *surface, const uint8_t *iid)
{
    Surface *s = surface;
    if (!g_cfg.d3d || !(IS_IID_TEX2(iid) || IS_IID_TEX(iid))) return 0;
    d3d_init();
    if (!s->tex_obj) s->tex_obj = dd_new_obj(vt_tex, s);
    return s->tex_obj;
}

/* ------------------------------------------------------------ caps */
static void fill_primcaps(uint32_t p)
{
    wr32(p + 0x00, 0x38);
    wr32(p + 0x04, 0x7F);           /* misc: mask z, cull none/cw/ccw, conformant */
    wr32(p + 0x08, 0x000703FF);     /* raster: dither, subpixel, fog vertex/table, ... */
    wr32(p + 0x0C, 0xFF);           /* z compare */
    wr32(p + 0x10, 0x1FFF);         /* src blend */
    wr32(p + 0x14, 0x1FFF);         /* dest blend */
    wr32(p + 0x18, 0xFF);           /* alpha compare */
    wr32(p + 0x1C, 0x000FFFFF);     /* shade: flat/gouraud colour, specular, alpha blend/stipple, fog */
    wr32(p + 0x20, 0x1F);           /* texture: perspective, pow2, alpha, transparency, border */
    wr32(p + 0x24, 0x3F);           /* texture filter */
    wr32(p + 0x28, 0xFF);           /* texture blend */
    wr32(p + 0x2C, 0x7);            /* texture address: wrap, mirror, clamp */
    wr32(p + 0x30, 32);
    wr32(p + 0x34, 32);
}

static void fill_devdesc(uint32_t d, bool hal)
{
    uint32_t size = 0xCC;
    memset(g_mem + d, 0, size);
    wr32(d, size);
    if (!hal) return;
    wr32(d + 0x04, 0xFFF);          /* all fields valid */
    wr32(d + 0x08, 2);              /* D3DCOLOR_RGB */
    wr32(d + 0x0C, 0x0FF1);         /* devcaps: float TL vertices, textures in system/video memory, DrawPrimitive */
    wr32(d + 0x10, 8);
    wr32(d + 0x14, 1);
    wr32(d + 0x18, 1);
    wr32(d + 0x1C, 16);
    wr32(d + 0x20, 7);
    wr32(d + 0x24, 1);
    wr32(d + 0x28, 8);
    fill_primcaps(d + 0x2C);
    fill_primcaps(d + 0x64);
    wr32(d + 0x9C, 0x400 | 0x800);  /* render depth: 16 and 8 bit */
    wr32(d + 0xA0, 0x400);          /* z-buffer 16 bit */
    wr32(d + 0xA4, 0);
    wr32(d + 0xA8, 65535);
    wr32(d + 0xAC, 1);
    wr32(d + 0xB0, 1);
    wr32(d + 0xB4, 2048);
    wr32(d + 0xB8, 2048);
}

/* ======================================================== IDirect3D2 */
static void d3d_QueryInterface(CPU *c)
{
    wr32(ARG(2), g_d3d_obj);
    hle_return(c, D3D_OK, 3);
}

static void d3d_EnumDevices(CPU *c)
{
    uint32_t cb = ARG(1), ctx = ARG(2);
    hle_return(c, D3D_OK, 3);
    uint32_t guid = garena_alloc(16), hal = garena_alloc(0xCC), hel = garena_alloc(0xCC);
    static uint32_t name, desc;
    if (!name) { name = garena_strdup("Direct3D HAL"); desc = garena_strdup("ircrecomp hardware renderer"); }
    memcpy(g_mem + guid, GUID_HAL, 16);
    fill_devdesc(hal, true);
    fill_devdesc(hel, false);
    guest_call(c, cb, 6, guid, desc, name, hal, hel, ctx);
    c->eax = D3D_OK;
}

static void dev_new_viewport(uint32_t out);

static void d3d_CreateViewport(CPU *c) { dev_new_viewport(ARG(1)); hle_return(c, D3D_OK, 3); }

static void d3d_CreateMaterial(CPU *c)
{
    Material *m = calloc(1, sizeof *m);
    m->obj = dd_new_obj(vt_mat, m);
    wr32(ARG(1), m->obj);
    hle_return(c, D3D_OK, 3);
}

static void d3d_CreateLight(CPU *c) { wr32(ARG(1), 0); hle_return(c, 0x80004001u, 3); }

static void d3d_FindDevice(CPU *c)
{
    uint32_t res = ARG(2);
    memcpy(g_mem + res + 4, GUID_HAL, 16);
    fill_devdesc(res + 20, true);
    fill_devdesc(res + 20 + 0xCC, false);
    hle_return(c, D3D_OK, 3);
}

static void d3d_CreateDevice(CPU *c)
{
    uint32_t surf = ARG(2), out = ARG(3);
    if (!g_dev) {
        g_dev = calloc(1, sizeof *g_dev);
        g_dev->obj = dd_new_obj(vt_dev, g_dev);
    }
    g_dev->target = dd_surface(surf);
    /* D3D defaults */
    memset(g_dev->rs, 0, sizeof g_dev->rs);
    g_dev->rs[3] = 1;    /* TEXTUREADDRESS wrap */
    g_dev->rs[4] = 0;    /* TEXTUREPERSPECTIVE */
    g_dev->rs[9] = 2;    /* SHADEMODE gouraud */
    g_dev->rs[17] = 1;   /* TEXTUREMAG nearest */
    g_dev->rs[18] = 1;   /* TEXTUREMIN nearest */
    g_dev->rs[19] = 2;   /* SRCBLEND one */
    g_dev->rs[20] = 1;   /* DESTBLEND zero */
    g_dev->rs[21] = 2;   /* TEXTUREMAPBLEND modulate */
    g_dev->rs[22] = 3;   /* CULLMODE ccw */
    g_dev->rs[25] = 8;   /* ALPHAFUNC always */
    rlog("Direct3D CreateDevice: target %dx%d fmt=%d", g_dev->target ? g_dev->target->w : 0,
         g_dev->target ? g_dev->target->h : 0, g_dev->target ? g_dev->target->format : -1);
    wr32(out, g_dev->obj);
    hle_return(c, D3D_OK, 4);
}

static const ComMethod m_d3d[] = {
    {"QueryInterface", 3, d3d_QueryInterface}, {"AddRef", 1, com_AddRef}, {"Release", 1, com_Release},
    {"EnumDevices", 3, d3d_EnumDevices}, {"CreateLight", 3, d3d_CreateLight}, {"CreateMaterial", 3, d3d_CreateMaterial},
    {"CreateViewport", 3, d3d_CreateViewport}, {"FindDevice", 3, d3d_FindDevice}, {"CreateDevice", 4, d3d_CreateDevice},
};

/* =================================================== texture conversion */
static uint32_t rgba_of_565(uint32_t v)
{
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

/* RGBA cache of a texture surface (alpha 0 for colour-keyed texels) */
static const uint32_t *texture_rgba(Surface *s, bool *has_alpha)
{
    uint32_t pv = s->pal ? s->pal->version : 0;
    *has_alpha = s->format == PF_ARGB1555 || s->format == PF_ARGB4444;
    if (s->tex_rgba && s->tex_version == s->version && s->tex_pal_version == pv) return s->tex_rgba;
    if (raster_pending()) raster_flush();
    if (!s->tex_rgba) s->tex_rgba = malloc((size_t)s->w * s->h * 4);
    for (int y = 0; y < s->h; y++) {
        uint32_t row = s->mem + (uint32_t)(y * s->pitch);
        uint32_t *o = s->tex_rgba + (size_t)y * s->w;
        for (int x = 0; x < s->w; x++) {
            uint32_t v, px;
            switch (s->format) {
            case PF_PAL8:
                v = rd8(row + (uint32_t)x);
                px = s->pal ? 0xFF000000u | ((uint32_t)s->pal->rgb[v][0] << 16) | ((uint32_t)s->pal->rgb[v][1] << 8) | s->pal->rgb[v][2]
                            : 0xFF000000u | v * 0x010101u;
                break;
            case PF_ARGB1555:
            case PF_RGB555:
                v = rd16(row + 2u * (uint32_t)x);
                px = ((v & 0x8000) || s->format == PF_RGB555 ? 0xFF000000u : 0) | ((((v >> 10) & 31) * 255 / 31) << 16) | ((((v >> 5) & 31) * 255 / 31) << 8) | ((v & 31) * 255 / 31);
                break;
            case PF_ARGB4444:
                v = rd16(row + 2u * (uint32_t)x);
                px = (((v >> 12) & 15) * 17u << 24) | (((v >> 8) & 15) * 17u << 16) | (((v >> 4) & 15) * 17u << 8) | ((v & 15) * 17u);
                break;
            case PF_XRGB8888:
                v = rd32(row + 4u * (uint32_t)x);
                px = 0xFF000000u | v;
                break;
            default:
                v = rd16(row + 2u * (uint32_t)x);
                px = rgba_of_565(v);
                break;
            }
            if (s->has_ck && v >= s->ck_lo && v <= s->ck_hi) px &= 0x00FFFFFFu;
            o[x] = px;
        }
    }
    s->tex_version = s->version;
    s->tex_pal_version = pv;
    return s->tex_rgba;
}

/* ======================================================= IDirect3DDevice2 */
static Device *dev_of(CPU *c) { return (Device *)host(COM_THIS()); }

static void dev_GetCaps(CPU *c)
{
    if (ARG(1)) fill_devdesc(ARG(1), true);
    if (ARG(2)) fill_devdesc(ARG(2), false);
    hle_return(c, D3D_OK, 3);
}

static void dev_EnumTextureFormats(CPU *c)
{
    uint32_t cb = ARG(1), ctx = ARG(2);
    hle_return(c, D3D_OK, 3);
    static const int formats[] = {PF_PAL8, PF_RGB565, PF_RGB555, PF_ARGB1555, PF_ARGB4444};
    uint32_t d = garena_alloc(108);
    for (unsigned i = 0; i < sizeof formats / sizeof formats[0]; i++) {
        memset(g_mem + d, 0, 108);
        wr32(d, 108);
        wr32(d + 4, DDSD_CAPS | DDSD_PIXELFORMAT);
        dd_fill_pixelformat(d + 0x48, formats[i]);
        wr32(d + 0x68, DDSCAPS_TEXTURE);
        if (guest_call(c, cb, 2, d, ctx) == 0) break;
    }
    c->eax = D3D_OK;
}

static void dev_AddViewport(CPU *c)
{
    Viewport *v = host(ARG(1));
    if (v) v->dev = dev_of(c);
    hle_return(c, D3D_OK, 2);
}
static void dev_DeleteViewport(CPU *c) { Device *d = dev_of(c); if (d->viewport == host(ARG(1))) d->viewport = NULL; hle_return(c, D3D_OK, 2); }
static void dev_SetCurrentViewport(CPU *c) { dev_of(c)->viewport = host(ARG(1)); hle_return(c, D3D_OK, 2); }
static void dev_GetCurrentViewport(CPU *c) { Device *d = dev_of(c); wr32(ARG(1), d->viewport ? d->viewport->obj : 0); hle_return(c, D3D_OK, 2); }
static void dev_BeginScene(CPU *c) { hle_return(c, D3D_OK, 1); }

static void dev_EndScene(CPU *c)
{
    Device *d = dev_of(c);
    raster_flush();
    if (d->target) dd_mark_dirty(d->target);
    hle_return(c, D3D_OK, 1);
}

static void dev_GetDirect3D(CPU *c) { wr32(ARG(1), g_d3d_obj); hle_return(c, D3D_OK, 2); }
static void dev_SetRenderTarget(CPU *c) { raster_flush(); dev_of(c)->target = dd_surface(ARG(1)); hle_return(c, D3D_OK, 3); }
static void dev_GetRenderTarget(CPU *c) { Device *d = dev_of(c); wr32(ARG(1), d->target ? d->target->obj : 0); hle_return(c, D3D_OK, 2); }

static void dev_GetRenderState(CPU *c) { wr32(ARG(2), dev_of(c)->rs[ARG(1) & 255]); hle_return(c, D3D_OK, 3); }
static void dev_SetRenderState(CPU *c)
{
    uint32_t s = ARG(1), v = ARG(2);
    if (g_cfg.trace && dev_of(c)->rs[s & 255] != v) rlog("SetRenderState(%u, %u)", s, v);
    dev_of(c)->rs[s & 255] = v;
    hle_return(c, D3D_OK, 3);
}
static void dev_GetLightState(CPU *c) { wr32(ARG(2), dev_of(c)->ls[ARG(1) & 15]); hle_return(c, D3D_OK, 3); }
static void dev_SetLightState(CPU *c) { dev_of(c)->ls[ARG(1) & 15] = ARG(2); hle_return(c, D3D_OK, 3); }
static void dev_Transform(CPU *c) { hle_return(c, D3D_OK, 3); }
static void dev_GetStats(CPU *c) { hle_return(c, D3D_OK, 2); }
static void dev_ClipStatus(CPU *c) { hle_return(c, D3D_OK, 2); }

static void dev_SwapTextureHandles(CPU *c)
{
    Surface *a = host(ARG(1)), *b = host(ARG(2));
    for (int i = 1; i < g_ntex; i++) {
        if (g_tex_handles[i] == a) g_tex_handles[i] = b;
        else if (g_tex_handles[i] == b) g_tex_handles[i] = a;
    }
    hle_return(c, D3D_OK, 3);
}

static bool setup_state(Device *d, RasterState *st)
{
    Surface *t = d->target;
    if (!t || t->format != PF_RGB565) return false;
    HDBuf *hd = hd_get(t);
    int S = hd->scale;
    memset(st, 0, sizeof *st);
    st->dst = hd->px;
    st->dst_pitch = t->w * S;
    st->dst_h = t->h * S;
    int cx0 = 0, cy0 = 0, cx1 = t->w, cy1 = t->h;
    if (d->viewport && d->viewport->w > 0) {
        Viewport *v = d->viewport;
        if (v->x > cx0) cx0 = v->x;
        if (v->y > cy0) cy0 = v->y;
        if (v->x + v->w < cx1) cx1 = v->x + v->w;
        if (v->y + v->h < cy1) cy1 = v->y + v->h;
    }
    st->clip_x0 = cx0 * S;
    st->clip_y0 = cy0 * S;
    st->clip_x1 = cx1 * S;
    st->clip_y1 = cy1 * S;
    uint32_t th = d->rs[1];
    Surface *tex = th > 0 && th < (uint32_t)g_ntex ? g_tex_handles[th] : NULL;
    if (tex) {
        st->tex = texture_rgba(tex, &st->tex_has_alpha);
        st->tex_w = tex->w;
        st->tex_h = tex->h;
        st->colorkey = d->rs[41] != 0 || tex->has_ck;
    }
    st->linear = g_cfg.smooth3d || d->rs[17] == 2 || d->rs[18] == 2;
    st->address = d->rs[44] ? (int)d->rs[44] : d->rs[3] ? (int)d->rs[3] : 1;
    st->tex_blend = d->rs[21] ? (int)d->rs[21] : 2;
    st->gouraud = d->rs[9] != 1;
    st->alpha_blend = d->rs[27] != 0;
    st->src_blend = (int)d->rs[19];
    st->dst_blend = (int)d->rs[20];
    st->alpha_test = d->rs[15] != 0;
    st->alpha_func = (int)d->rs[25];
    st->alpha_ref = (int)d->rs[24];
    st->specular = d->rs[29] != 0;
    st->fog = d->rs[28] != 0;
    st->fog_color = d->rs[34];
    st->cull = d->rs[22] ? (int)d->rs[22] : 3;
    return true;
}

static float g_vscale = 1.f;

/* D3D5 pixel centres are at integer coordinates: shift by half a pixel and scale to the shadow */
static void read_vertex(uint32_t p, TLVertex *v)
{
    memcpy(v, g_mem + p, 32);
    v->x = (v->x + 0.5f) * g_vscale;
    v->y = (v->y + 0.5f) * g_vscale;
}

static void draw(Device *d, uint32_t ptype, uint32_t vtype, uint32_t verts, uint32_t nverts, uint32_t indices, uint32_t nidx)
{
    if (vtype != 3) {
        rlog_once("d3dvtype", "DrawPrimitive with vertex type %u not supported", vtype);
        return;
    }
    RasterState st;
    if (!setup_state(d, &st)) return;
    if (g_cfg.trace) {
        char key[160];
        snprintf(key, sizeof key, "rs tex=%d fmt=%d ck=%d blend=%u %u/%u tb=%u at=%u/%u/%u fog=%u spec=%u stip=%u/%u shade=%u",
                 st.tex != NULL, (int)(d->rs[1] && d->rs[1] < (uint32_t)g_ntex ? g_tex_handles[d->rs[1]]->format : -1),
                 st.colorkey, d->rs[27], d->rs[19], d->rs[20], d->rs[21], d->rs[15], d->rs[24], d->rs[25], d->rs[28],
                 d->rs[29], d->rs[33], d->rs[39], d->rs[9]);
        rlog_once(key, "%s ptype=%u", key, ptype);
    }
    g_vscale = (float)d->target->hd->scale;
    d->target->hd->ahead = true;
    raster_set_state(&st);
    uint32_t n = indices ? nidx : nverts;
    TLVertex v[3];
#define VTX(i) (verts + 32u * (indices ? rd16(indices + 2u * (i)) : (i)))
    d->prims++;
    switch (ptype) {
    case 4: /* TRIANGLELIST */
        for (uint32_t i = 0; i + 2 < n; i += 3) {
            read_vertex(VTX(i), &v[0]); read_vertex(VTX(i + 1), &v[1]); read_vertex(VTX(i + 2), &v[2]);
            raster_add(&v[0], &v[1], &v[2]);
            d->tris++;
        }
        break;
    case 5: /* TRIANGLESTRIP */
        for (uint32_t i = 0; i + 2 < n; i++) {
            read_vertex(VTX(i), &v[0]); read_vertex(VTX(i + 1), &v[1]); read_vertex(VTX(i + 2), &v[2]);
            if (i & 1) raster_add(&v[1], &v[0], &v[2]);
            else raster_add(&v[0], &v[1], &v[2]);
            d->tris++;
        }
        break;
    case 6: /* TRIANGLEFAN */
        read_vertex(VTX(0), &v[0]);
        for (uint32_t i = 1; i + 1 < n; i++) {
            read_vertex(VTX(i), &v[1]); read_vertex(VTX(i + 1), &v[2]);
            raster_add(&v[0], &v[1], &v[2]);
            d->tris++;
        }
        break;
    default:
        rlog_once("d3dptype", "DrawPrimitive type %u (points/lines) ignored", ptype);
        break;
    }
#undef VTX
}

static void dev_DrawPrimitive(CPU *c)
{
    draw(dev_of(c), ARG(1), ARG(2), ARG(3), ARG(4), 0, 0);
    hle_return(c, D3D_OK, 6);
}

static void dev_DrawIndexedPrimitive(CPU *c)
{
    draw(dev_of(c), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6));
    hle_return(c, D3D_OK, 8);
}

static const ComMethod m_dev[] = {
    {"QueryInterface", 3, NULL}, {"AddRef", 1, com_AddRef}, {"Release", 1, com_Release},
    {"GetCaps", 3, dev_GetCaps}, {"SwapTextureHandles", 3, dev_SwapTextureHandles}, {"GetStats", 2, dev_GetStats},
    {"AddViewport", 2, dev_AddViewport}, {"DeleteViewport", 2, dev_DeleteViewport}, {"NextViewport", 4, NULL},
    {"EnumTextureFormats", 3, dev_EnumTextureFormats}, {"BeginScene", 1, dev_BeginScene}, {"EndScene", 1, dev_EndScene},
    {"GetDirect3D", 2, dev_GetDirect3D}, {"SetCurrentViewport", 2, dev_SetCurrentViewport},
    {"GetCurrentViewport", 2, dev_GetCurrentViewport}, {"SetRenderTarget", 3, dev_SetRenderTarget},
    {"GetRenderTarget", 2, dev_GetRenderTarget}, {"Begin", 4, NULL}, {"BeginIndexed", 6, NULL}, {"Vertex", 2, NULL},
    {"Index", 2, NULL}, {"End", 2, NULL}, {"GetRenderState", 3, dev_GetRenderState}, {"SetRenderState", 3, dev_SetRenderState},
    {"GetLightState", 3, dev_GetLightState}, {"SetLightState", 3, dev_SetLightState}, {"SetTransform", 3, dev_Transform},
    {"GetTransform", 3, dev_Transform}, {"MultiplyTransform", 3, dev_Transform}, {"DrawPrimitive", 6, dev_DrawPrimitive},
    {"DrawIndexedPrimitive", 8, dev_DrawIndexedPrimitive}, {"SetClipStatus", 2, dev_ClipStatus}, {"GetClipStatus", 2, dev_ClipStatus},
};

/* ===================================================== IDirect3DViewport2 */
static void dev_new_viewport(uint32_t out)
{
    Viewport *v = calloc(1, sizeof *v);
    v->obj = dd_new_obj(vt_vp, v);
    wr32(out, v->obj);
}

static Viewport *vp_of(CPU *c) { return (Viewport *)host(COM_THIS()); }

static void vp_Initialize(CPU *c) { hle_return(c, D3D_OK, 2); }

static void vp_SetViewport(CPU *c)
{
    Viewport *v = vp_of(c);
    uint32_t p = ARG(1);
    v->x = (int)rd32(p + 4);
    v->y = (int)rd32(p + 8);
    v->w = (int)rd32(p + 12);
    v->h = (int)rd32(p + 16);
    rlog("viewport %d,%d %dx%d", v->x, v->y, v->w, v->h);
    hle_return(c, D3D_OK, 2);
}

static void vp_GetViewport(CPU *c)
{
    Viewport *v = vp_of(c);
    uint32_t p = ARG(1);
    wr32(p + 4, (uint32_t)v->x);
    wr32(p + 8, (uint32_t)v->y);
    wr32(p + 12, (uint32_t)v->w);
    wr32(p + 16, (uint32_t)v->h);
    hle_return(c, D3D_OK, 2);
}

static void vp_SetBackground(CPU *c) { vp_of(c)->background = ARG(1); hle_return(c, D3D_OK, 2); }
static void vp_GetBackground(CPU *c) { wr32(ARG(1), vp_of(c)->background); if (ARG(2)) wr32(ARG(2), 1); hle_return(c, D3D_OK, 3); }
static void vp_Light(CPU *c) { hle_return(c, D3D_OK, 2); }
static void vp_BgDepth(CPU *c) { hle_return(c, D3D_OK, 2); }

static void vp_Clear(CPU *c)
{
    Viewport *v = vp_of(c);
    uint32_t n = ARG(1), rects = ARG(2), flags = ARG(3);
    Device *d = v->dev ? v->dev : g_dev;
    hle_return(c, D3D_OK, 4);
    if (!(flags & 1) || !d || !d->target || d->target->format != PF_RGB565) return;
    uint32_t color = 0;
    Material *m = v->background > 0 && v->background < (uint32_t)g_nmat ? g_mat_handles[v->background] : NULL;
    if (m) {
        float f[3];
        for (int k = 0; k < 3; k++) f[k] = m->diffuse[k] < 0 ? 0 : m->diffuse[k] > 1 ? 255.f : m->diffuse[k] * 255.f;
        color = ((uint32_t)f[0] << 16) | ((uint32_t)f[1] << 8) | (uint32_t)f[2];
    }
    raster_flush();
    Surface *t = d->target;
    HDBuf *hd = hd_get(t);
    int S = hd->scale, W = t->w * S;
    for (uint32_t i = 0; i < n; i++) {
        int x0 = (int)rd32(rects + 16 * i), y0 = (int)rd32(rects + 16 * i + 4), x1 = (int)rd32(rects + 16 * i + 8), y1 = (int)rd32(rects + 16 * i + 12);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > t->w) x1 = t->w;
        if (y1 > t->h) y1 = t->h;
        for (int y = y0 * S; y < y1 * S; y++)
            for (int x = x0 * S; x < x1 * S; x++) hd->px[(size_t)y * W + x] = color;
    }
    hd->ahead = true;
}

static void vp_SetViewport2(CPU *c) { vp_SetViewport(c); }
static void vp_GetViewport2(CPU *c) { vp_GetViewport(c); }

static const ComMethod m_vp[] = {
    {"QueryInterface", 3, NULL}, {"AddRef", 1, com_AddRef}, {"Release", 1, com_Release},
    {"Initialize", 2, vp_Initialize}, {"GetViewport", 2, vp_GetViewport}, {"SetViewport", 2, vp_SetViewport},
    {"TransformVertices", 5, NULL}, {"LightElements", 3, NULL}, {"SetBackground", 2, vp_SetBackground},
    {"GetBackground", 3, vp_GetBackground}, {"SetBackgroundDepth", 2, vp_BgDepth}, {"GetBackgroundDepth", 3, NULL},
    {"Clear", 4, vp_Clear}, {"AddLight", 2, vp_Light}, {"DeleteLight", 2, vp_Light}, {"NextLight", 4, NULL},
    {"GetViewport2", 2, vp_GetViewport2}, {"SetViewport2", 2, vp_SetViewport2},
};

/* ===================================================== IDirect3DMaterial2 */
static void mat_SetMaterial(CPU *c)
{
    Material *m = host(COM_THIS());
    uint32_t p = ARG(1);
    for (int i = 0; i < 4; i++) memcpy(&m->diffuse[i], g_mem + p + 4 + 4 * (uint32_t)i, 4);
    hle_return(c, D3D_OK, 2);
}

static void mat_GetMaterial(CPU *c)
{
    Material *m = host(COM_THIS());
    uint32_t p = ARG(1);
    for (int i = 0; i < 4; i++) memcpy(g_mem + p + 4 + 4 * (uint32_t)i, &m->diffuse[i], 4);
    hle_return(c, D3D_OK, 2);
}

static void mat_GetHandle(CPU *c)
{
    Material *m = host(COM_THIS());
    if (!m->handle && g_nmat < MAX_HANDLES) {
        m->handle = (uint32_t)g_nmat;
        g_mat_handles[g_nmat++] = m;
    }
    wr32(ARG(2), m->handle);
    hle_return(c, D3D_OK, 3);
}

static const ComMethod m_mat[] = {
    {"QueryInterface", 3, NULL}, {"AddRef", 1, com_AddRef}, {"Release", 1, com_Release},
    {"SetMaterial", 2, mat_SetMaterial}, {"GetMaterial", 2, mat_GetMaterial}, {"GetHandle", 3, mat_GetHandle}, {"Reserve", 1, NULL},
};

/* ===================================================== IDirect3DTexture2 */
static void tex_GetHandle(CPU *c)
{
    Surface *s = host(COM_THIS());
    uint32_t h = 0;
    for (int i = 1; i < g_ntex; i++)
        if (g_tex_handles[i] == s) h = (uint32_t)i;
    if (!h && g_ntex < MAX_HANDLES) {
        h = (uint32_t)g_ntex;
        g_tex_handles[g_ntex++] = s;
    }
    wr32(ARG(2), h);
    hle_return(c, D3D_OK, 3);
}

static void tex_PaletteChanged(CPU *c) { raster_flush(); Surface *s = host(COM_THIS()); s->version++; hle_return(c, D3D_OK, 3); }

static void tex_Load(CPU *c)
{
    Surface *d = host(COM_THIS()), *s = host(ARG(1));
    raster_flush();
    if (s && d && s->pitch == d->pitch && s->h == d->h) {
        memcpy(g_mem + d->mem, g_mem + s->mem, (size_t)s->pitch * s->h);
        if (s->pal) d->pal = s->pal;
        d->has_ck = s->has_ck;
        d->ck_lo = s->ck_lo;
        d->ck_hi = s->ck_hi;
        d->version++;
    }
    hle_return(c, D3D_OK, 2);
}

static const ComMethod m_tex[] = {
    {"QueryInterface", 3, NULL}, {"AddRef", 1, com_AddRef}, {"Release", 1, com_Release},
    {"GetHandle", 3, tex_GetHandle}, {"PaletteChanged", 3, tex_PaletteChanged}, {"Load", 2, tex_Load},
};

static void d3d_init(void)
{
    if (vt_d3d) return;
    vt_d3d = com_vtable("IDirect3D2", m_d3d, (int)(sizeof m_d3d / sizeof m_d3d[0]));
    vt_dev = com_vtable("IDirect3DDevice2", m_dev, (int)(sizeof m_dev / sizeof m_dev[0]));
    vt_vp = com_vtable("IDirect3DViewport2", m_vp, (int)(sizeof m_vp / sizeof m_vp[0]));
    vt_mat = com_vtable("IDirect3DMaterial2", m_mat, (int)(sizeof m_mat / sizeof m_mat[0]));
    vt_tex = com_vtable("IDirect3DTexture2", m_tex, (int)(sizeof m_tex / sizeof m_tex[0]));
    g_d3d_obj = dd_new_obj(vt_d3d, NULL);
    int cpus = SDL_GetNumLogicalCPUCores();
    if (SDL_getenv("IRC_RASTER_THREADS")) cpus = SDL_atoi(SDL_getenv("IRC_RASTER_THREADS")) + 1;
    raster_init(cpus > 1 ? cpus - 1 : 0);
    rlog("Direct3D available (%d rasterizer threads)", cpus > 1 ? cpus - 1 : 0);
}
