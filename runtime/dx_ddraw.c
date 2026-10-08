/* DirectDraw (DX5: IDirectDraw, IDirectDraw2, IDirectDrawSurface/2, IDirectDrawPalette)
 * Surfaces live in guest memory; the primary surface is shown by the main thread
 * through an SDL renderer (8-bit palette or RGB565 converted to XRGB8888). */
#include <stdlib.h>
#include "runtime.h"

#include "ddraw_int.h"

/* host objects referenced from guest COM objects (obj+8 holds index) */
#define OBJ_MAX 4096
static void *g_objs[OBJ_MAX];
static int g_nobjs = 1;

uint32_t dd_new_obj(uint32_t vtable, void *host)
{
    uint32_t o = com_object(vtable, 16);
    if (g_nobjs >= OBJ_MAX) fatal("too many DirectDraw objects");
    g_objs[g_nobjs] = host;
    wr32(o + 8, (uint32_t)g_nobjs++);
    return o;
}
void *dd_host_of(uint32_t obj) { uint32_t i = obj ? rd32(obj + 8) : 0; return i < OBJ_MAX ? g_objs[i] : NULL; }
#define new_obj dd_new_obj
#define host_of dd_host_of

static uint32_t vt_dd, vt_dd2, vt_surf, vt_surf2, vt_pal, vt_clip;
static uint32_t g_dd_obj, g_dd2_obj;
static Surface *g_primary;
static SDL_Mutex *g_video_lock;
static volatile uint32_t g_frame_serial;
static uint32_t g_shown_serial;
static DisplayMode g_mode = {640, 480, 8};

/* -------------------------------------------------------------- video out */
static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static int g_tex_w, g_tex_h;
static uint32_t *g_conv;

void video_init_main_thread(SDL_Window *win)
{
    g_win = win;
    g_ren = SDL_CreateRenderer(win, NULL);
    if (!g_ren) fatal("SDL_CreateRenderer: %s", SDL_GetError());
    SDL_SetRenderVSync(g_ren, 1);
    if (g_cfg.fps_limit < 0) {
        const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
        g_cfg.fps_limit = dm && dm->refresh_rate > 1 ? (int)(dm->refresh_rate + 0.5f) : 60;
        rlog("frame limit: %d fps", g_cfg.fps_limit);
    }
    rlog("renderer: %s", SDL_GetRendererName(g_ren));
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    SDL_RenderPresent(g_ren);
}

void video_set_mode(int w, int h, int bpp)
{
    SDL_LockMutex(g_video_lock);
    g_mode.width = w;
    g_mode.height = h;
    g_mode.bpp = bpp;
    g_frame_serial++;
    SDL_UnlockMutex(g_video_lock);
    rlog("display mode %dx%dx%d", w, h, bpp);
}

static void count_frame(void)
{
    static uint64_t t0;
    static int frames;
    frames++;
    uint64_t now = SDL_GetTicks();
    if (now - t0 >= 5000) {
        if (t0 && g_cfg.trace) rlog("video: %.1f frames/s (%dx%dx%d)", frames * 1000.0 / (double)(now - t0), g_mode.width, g_mode.height, g_mode.bpp);
        t0 = now;
        frames = 0;
    }
}

/* Real DirectDraw blocked in Flip until the vertical blank; the game renders as fast as it
 * can, so pace frames to the display refresh (fps_limit, 0 = unlimited). */
static void frame_throttle(void)
{
    static uint64_t next_ns;
    int fps = g_cfg.fps_limit;
    if (fps <= 0) return;
    uint64_t period = 1000000000ull / (uint64_t)fps;
    uint64_t now = SDL_GetTicksNS();
    if (next_ns > now && next_ns - now < period * 2) SDL_DelayPrecise(next_ns - now);
    now = SDL_GetTicksNS();
    next_ns = (next_ns + period > now && next_ns + period < now + period * 2) ? next_ns + period : now + period;
}

static void mark_dirty(Surface *s)
{
    if (!s) return;
    s->version++;
    if (s->primary) {
        g_frame_serial++;
        count_frame();
    }
}
void dd_mark_dirty(Surface *s) { mark_dirty(s); }
void dd_lock_video(void) { SDL_LockMutex(g_video_lock); }
void dd_unlock_video(void) { SDL_UnlockMutex(g_video_lock); }

static int g_out_w, g_out_h;

/* destination rectangle for a w x h image in the current output (follows window/monitor) */
static SDL_FRect present_rect(int w, int h, int ow, int oh)
{
    SDL_FRect r = {0, 0, (float)ow, (float)oh};
    if (g_cfg.stretch || w <= 0 || h <= 0) return r;
    float s = SDL_min((float)ow / (float)w, (float)oh / (float)h);
    r.w = (float)w * s;
    r.h = (float)h * s;
    r.x = ((float)ow - r.w) * 0.5f;
    r.y = ((float)oh - r.h) * 0.5f;
    return r;
}

int video_output_height(void)
{
    int ow = 0, oh = 0;
    if (g_ren) SDL_GetCurrentRenderOutputSize(g_ren, &ow, &oh);
    return oh;
}

void video_present_if_needed(void)
{
    if (!g_ren) return;
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(g_ren, &ow, &oh);
    bool resized = ow != g_out_w || oh != g_out_h;
    if (g_shown_serial == g_frame_serial && !resized) return;
    g_out_w = ow;
    g_out_h = oh;
    if (g_shown_serial == g_frame_serial && g_tex) {
        SDL_FRect dr = present_rect(g_tex_w, g_tex_h, ow, oh);
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
        SDL_RenderClear(g_ren);
        SDL_RenderTexture(g_ren, g_tex, NULL, &dr);
        SDL_RenderPresent(g_ren);
        return;
    }
    SDL_LockMutex(g_video_lock);
    g_shown_serial = g_frame_serial;
    Surface *p = g_primary;
    if (!p || !p->mem) {
        SDL_UnlockMutex(g_video_lock);
        return;
    }
    HDBuf *hd = p->hd;
    int w = hd ? p->w * hd->scale : p->w, h = hd ? p->h * hd->scale : p->h;
    if (w != g_tex_w || h != g_tex_h || !g_tex) {
        if (g_tex) SDL_DestroyTexture(g_tex);
        g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        /* pixel-art scaling keeps 2D crisp at non-integer factors; linear when asked for */
        SDL_SetTextureScaleMode(g_tex, g_cfg.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_PIXELART);
        g_tex_w = w;
        g_tex_h = h;
        free(g_conv);
        g_conv = malloc((size_t)w * h * 4);
    }
    const uint8_t *src = g_mem + p->mem;
    uint32_t *dst = g_conv;
    if (hd) {
        memcpy(dst, hd->px, (size_t)w * h * 4);
    } else if (p->bpp == 8) {
        uint32_t lut[256];
        Palette *pal = p->pal;
        for (int i = 0; i < 256; i++)
            lut[i] = pal ? ((uint32_t)pal->rgb[i][0] << 16) | ((uint32_t)pal->rgb[i][1] << 8) | pal->rgb[i][2] : (uint32_t)(i * 0x010101);
        for (int y = 0; y < h; y++) {
            const uint8_t *row = src + (size_t)y * p->pitch;
            for (int x = 0; x < w; x++) *dst++ = lut[row[x]];
        }
    } else if (p->bpp == 16) {
        for (int y = 0; y < h; y++) {
            const uint16_t *row = (const uint16_t *)(src + (size_t)y * p->pitch);
            for (int x = 0; x < w; x++) {
                uint32_t v = row[x];
                uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                *dst++ = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
            }
        }
    } else {
        for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * w, src + (size_t)y * p->pitch, (size_t)w * 4);
    }
    SDL_UnlockMutex(g_video_lock);
    SDL_UpdateTexture(g_tex, NULL, g_conv, w * 4);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    SDL_FRect dr = present_rect(w, h, ow, oh);
    SDL_RenderTexture(g_ren, g_tex, NULL, &dr);
    if (g_cfg.shot_dir[0]) {
        /* debug: one screenshot per second of what is really on screen (bars included) */
        static uint64_t last;
        static int n;
        uint64_t now = SDL_GetTicks();
        if (now - last >= (uint64_t)(g_cfg.shot_ms > 0 ? g_cfg.shot_ms : 1000)) {
            last = now;
            SDL_Surface *s = SDL_RenderReadPixels(g_ren, NULL);
            if (s) {
                char path[1200];
                snprintf(path, sizeof path, "%s/shot_%04d_%06llu.bmp", g_cfg.shot_dir, n++, (unsigned long long)now);
                SDL_SaveBMP(s, path);
                SDL_DestroySurface(s);
            }
        }
    }
    SDL_RenderPresent(g_ren);
}

/* -------------------------------------------------------------- helpers */
static int bpp_of(int format) { return format == PF_PAL8 ? 8 : format == PF_XRGB8888 ? 32 : 16; }

void dd_fill_pixelformat(uint32_t pf, int format)
{
    memset(g_mem + pf, 0, 32);
    wr32(pf, 32);
    switch (format) {
    case PF_PAL8:
        wr32(pf + 4, 0x20 | 0x40);                 /* DDPF_PALETTEINDEXED8 | DDPF_RGB */
        wr32(pf + 12, 8);
        break;
    case PF_RGB565:
        wr32(pf + 4, 0x40);
        wr32(pf + 12, 16);
        wr32(pf + 16, 0xF800); wr32(pf + 20, 0x07E0); wr32(pf + 24, 0x001F);
        break;
    case PF_ARGB1555:
        wr32(pf + 4, 0x40 | 0x1);                  /* DDPF_RGB | DDPF_ALPHAPIXELS */
        wr32(pf + 12, 16);
        wr32(pf + 16, 0x7C00); wr32(pf + 20, 0x03E0); wr32(pf + 24, 0x001F); wr32(pf + 28, 0x8000);
        break;
    case PF_RGB555:
        wr32(pf + 4, 0x40);
        wr32(pf + 12, 16);
        wr32(pf + 16, 0x7C00); wr32(pf + 20, 0x03E0); wr32(pf + 24, 0x001F);
        break;
    case PF_ARGB4444:
        wr32(pf + 4, 0x40 | 0x1);
        wr32(pf + 12, 16);
        wr32(pf + 16, 0x0F00); wr32(pf + 20, 0x00F0); wr32(pf + 24, 0x000F); wr32(pf + 28, 0xF000);
        break;
    default:
        wr32(pf + 4, 0x40);
        wr32(pf + 12, 32);
        wr32(pf + 16, 0xFF0000); wr32(pf + 20, 0xFF00); wr32(pf + 24, 0xFF);
        break;
    }
}
#define fill_pixelformat dd_fill_pixelformat

int dd_format_from_ddpf(uint32_t pf)
{
    uint32_t flags = rd32(pf + 4), bits = rd32(pf + 12);
    if (flags & 0x20) return PF_PAL8;
    if (bits == 32 || bits == 24) return PF_XRGB8888;
    if (flags & 0x1) return rd32(pf + 28) == 0xF000 ? PF_ARGB4444 : PF_ARGB1555;
    if (bits == 8) return PF_PAL8;
    if (rd32(pf + 16) == 0x7C00) return PF_RGB555;
    return PF_RGB565;
}

static void fill_desc(uint32_t d, Surface *s)
{
    uint32_t size = rd32(d);
    if (size < 108) size = 108;
    memset(g_mem + d, 0, size);
    wr32(d, size);
    wr32(d + 4, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_LPSURFACE);
    wr32(d + 8, (uint32_t)s->h);
    wr32(d + 12, (uint32_t)s->w);
    wr32(d + 16, (uint32_t)s->pitch);
    wr32(d + 20, (uint32_t)(s->chain_len > 1 ? s->chain_len - 1 : 0));
    wr32(d + 0x24, s->mem);
    fill_pixelformat(d + 0x48, s->format);
    wr32(d + 0x68, s->caps);
}

static Surface *surface_new(int w, int h, int format, uint32_t caps)
{
    Surface *s = calloc(1, sizeof *s);
    s->w = w;
    s->h = h;
    s->format = format;
    s->bpp = bpp_of(format);
    s->pitch = ((w * (s->bpp / 8)) + 3) & ~3;
    s->caps = caps;
    s->mem = gmem_alloc((uint32_t)(s->pitch * h) + 64);
    s->obj = new_obj(vt_surf, s);
    s->obj2 = new_obj(vt_surf2, s);
    return s;
}

static Surface *surf(uint32_t obj) { return (Surface *)host_of(obj); }
Surface *dd_surface(uint32_t obj) { return surf(obj); }

/* ======================================================== IDirectDraw(2) */
static uint32_t dd_create_surface(uint32_t desc, bool v2)
{
    uint32_t flags = rd32(desc + 4);
    uint32_t caps = (flags & DDSD_CAPS) ? rd32(desc + 0x68) : 0;
    int w = (flags & DDSD_WIDTH) ? (int)rd32(desc + 12) : g_mode.width;
    int h = (flags & DDSD_HEIGHT) ? (int)rd32(desc + 8) : g_mode.height;
    int format = g_mode.bpp == 8 ? PF_PAL8 : g_mode.bpp == 16 ? PF_RGB565 : PF_XRGB8888;
    if (flags & DDSD_PIXELFORMAT) format = dd_format_from_ddpf(desc + 0x48);
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        w = g_mode.width;
        h = g_mode.height;
        format = g_mode.bpp == 8 ? PF_PAL8 : g_mode.bpp == 16 ? PF_RGB565 : PF_XRGB8888;
    }
    if (caps & DDSCAPS_ZBUFFER) format = PF_RGB565;
    Surface *s = surface_new(w, h, format, caps | (caps & DDSCAPS_PRIMARYSURFACE ? DDSCAPS_VISIBLE | DDSCAPS_FRONTBUFFER : 0));
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        s->primary = true;
        int backs = (flags & DDSD_BACKBUFFERCOUNT) ? (int)rd32(desc + 20) : 0;
        if (backs > 3) backs = 3;
        s->chain[0] = s;
        s->chain_len = 1 + backs;
        for (int i = 1; i <= backs; i++) {
            Surface *b = surface_new(w, h, format, (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER)) | DDSCAPS_FLIP | (i == 1 ? DDSCAPS_BACKBUFFER : 0));
            s->chain[i] = b;
            b->chain_len = 0;
        }
        SDL_LockMutex(g_video_lock);
        g_primary = s;
        g_frame_serial++;
        SDL_UnlockMutex(g_video_lock);
    }
    if (g_cfg.trace || !(caps & DDSCAPS_TEXTURE))
        rlog("CreateSurface %dx%d fmt=%d caps=%08x backs=%d -> %08x", w, h, format, caps, s->chain_len ? s->chain_len - 1 : 0, s->obj);
    return v2 ? s->obj2 : s->obj;
}

static void dd_QueryInterface(CPU *c)
{
    uint32_t riid = ARG(1), out = ARG(2);
    const uint8_t *g = g_mem + riid;
    static const uint8_t IID_DD2[16] = {0xE0, 0xF3, 0xA6, 0xB3, 0x43, 0x2B, 0xCF, 0x11, 0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56};
    static const uint8_t IID_DD[16] = {0x80, 0xDB, 0x14, 0x6C, 0x33, 0xA7, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60};
    if (!memcmp(g, IID_DD2, 16)) { wr32(out, g_dd2_obj); hle_return(c, DD_OK, 3); return; }
    if (!memcmp(g, IID_DD, 16)) { wr32(out, g_dd_obj); hle_return(c, DD_OK, 3); return; }
    extern uint32_t d3d_query_interface(const uint8_t *iid);
    uint32_t o = d3d_query_interface(g);
    if (o) { wr32(out, o); hle_return(c, DD_OK, 3); return; }
    rlog("IDirectDraw::QueryInterface: unknown IID %08x-...", rd32(riid));
    wr32(out, 0);
    hle_return(c, E_NOINTERFACE, 3);
}

void com_AddRef(CPU *c) { uint32_t o = COM_THIS(); wr32(o + 4, rd32(o + 4) + 1); hle_return(c, rd32(o + 4), 1); }
void com_Release(CPU *c) { uint32_t o = COM_THIS(); uint32_t r = rd32(o + 4); if (r) wr32(o + 4, --r); hle_return(c, r, 1); }

static void dd_CreateSurface(CPU *c, bool v2)
{
    uint32_t desc = ARG(1), out = ARG(2);
    wr32(out, dd_create_surface(desc, v2));
    hle_return(c, DD_OK, 4);
}
static void dd1_CreateSurface(CPU *c) { dd_CreateSurface(c, false); }
static void dd2_CreateSurface(CPU *c) { dd_CreateSurface(c, false); }

static void dd_CreatePalette(CPU *c)
{
    uint32_t entries = ARG(2), out = ARG(3);
    Palette *p = calloc(1, sizeof *p);
    if (entries) memcpy(p->rgb, g_mem + entries, 1024);
    p->version = 1;
    p->obj = new_obj(vt_pal, p);
    wr32(out, p->obj);
    hle_return(c, DD_OK, 5);
}

static void dd_CreateClipper(CPU *c)
{
    wr32(ARG(2), new_obj(vt_clip, NULL));
    hle_return(c, DD_OK, 4);
}

static const int g_modes[][3] = {
    {320, 240, 8}, {512, 384, 8}, {640, 480, 8}, {800, 600, 8},
    {320, 240, 16}, {512, 384, 16}, {640, 480, 16}, {800, 600, 16},
};

static void dd_EnumDisplayModes(CPU *c)
{
    uint32_t ctx = ARG(3), cb = ARG(4);
    hle_return(c, DD_OK, 5);
    uint32_t d = garena_alloc(108);
    for (unsigned i = 0; i < sizeof g_modes / sizeof g_modes[0]; i++) {
        memset(g_mem + d, 0, 108);
        wr32(d, 108);
        wr32(d + 4, DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE);
        wr32(d + 8, (uint32_t)g_modes[i][1]);
        wr32(d + 12, (uint32_t)g_modes[i][0]);
        wr32(d + 16, (uint32_t)(g_modes[i][0] * g_modes[i][2] / 8));
        wr32(d + 0x18, 60);
        fill_pixelformat(d + 0x48, g_modes[i][2] == 8 ? PF_PAL8 : PF_RGB565);
        if (guest_call(c, cb, 2, d, ctx) == 0) break;
    }
    c->eax = DD_OK;
}

static void dd_GetCaps(CPU *c)
{
    for (int i = 1; i <= 2; i++) {
        uint32_t p = ARG(i);
        if (!p) continue;
        uint32_t size = rd32(p);
        if (size < 4 || size > 1024) size = 316;
        memset(g_mem + p, 0, size);
        wr32(p, size);
        /* DDCAPS_BLT | DDCAPS_PALETTE | DDCAPS_BLTCOLORFILL, plus DDCAPS_3D when Direct3D is offered */
        wr32(p + 4, 0x40 | 0x200000 | 0x4000000 | (g_cfg.d3d ? 0x1u : 0u));
        wr32(p + 0x3C, 8u << 20);                   /* dwVidMemTotal */
        wr32(p + 0x40, 8u << 20);                   /* dwVidMemFree */
    }
    hle_return(c, DD_OK, 3);
}

static void dd_SetCooperativeLevel(CPU *c) { rlog("SetCooperativeLevel(%x)", ARG(2)); hle_return(c, DD_OK, 3); }

static void dd1_SetDisplayMode(CPU *c)
{
    video_set_mode((int)ARG(1), (int)ARG(2), (int)ARG(3));
    hle_return(c, DD_OK, 4);
}
static void dd2_SetDisplayMode(CPU *c)
{
    video_set_mode((int)ARG(1), (int)ARG(2), (int)ARG(3));
    hle_return(c, DD_OK, 6);
}
static void dd_RestoreDisplayMode(CPU *c) { hle_return(c, DD_OK, 1); }
static void dd_FlipToGDISurface(CPU *c) { hle_return(c, DD_OK, 1); }
static void dd_WaitForVerticalBlank(CPU *c) { SDL_Delay(1); hle_return(c, DD_OK, 3); }
static void dd_GetVerticalBlankStatus(CPU *c) { if (ARG(1)) wr32(ARG(1), 1); hle_return(c, DD_OK, 2); }

static void dd_GetDisplayMode(CPU *c)
{
    uint32_t d = ARG(1);
    Surface tmp = {.w = g_mode.width, .h = g_mode.height, .bpp = g_mode.bpp, .pitch = g_mode.width * g_mode.bpp / 8};
    fill_desc(d, &tmp);
    hle_return(c, DD_OK, 2);
}

static void dd_GetAvailableVidMem(CPU *c)
{
    if (ARG(2)) wr32(ARG(2), 8u << 20);
    if (ARG(3)) wr32(ARG(3), 8u << 20);
    hle_return(c, DD_OK, 4);
}

static void dd_GetMonitorFrequency(CPU *c) { if (ARG(1)) wr32(ARG(1), 60); hle_return(c, DD_OK, 2); }

#define DD_COMMON(createsurf, setmode, setmode_n)                                                     \
    {"QueryInterface", 3, dd_QueryInterface}, {"AddRef", 1, com_AddRef}, {"Release", 1, com_Release},  \
    {"Compact", 1, NULL}, {"CreateClipper", 4, dd_CreateClipper}, {"CreatePalette", 5, dd_CreatePalette}, \
    {"CreateSurface", 4, createsurf}, {"DuplicateSurface", 3, NULL},                                   \
    {"EnumDisplayModes", 5, dd_EnumDisplayModes}, {"EnumSurfaces", 5, NULL},                           \
    {"FlipToGDISurface", 1, dd_FlipToGDISurface}, {"GetCaps", 3, dd_GetCaps},                          \
    {"GetDisplayMode", 2, dd_GetDisplayMode}, {"GetFourCCCodes", 3, NULL}, {"GetGDISurface", 2, NULL},  \
    {"GetMonitorFrequency", 2, dd_GetMonitorFrequency}, {"GetScanLine", 2, NULL},                      \
    {"GetVerticalBlankStatus", 2, dd_GetVerticalBlankStatus}, {"Initialize", 2, NULL},                 \
    {"RestoreDisplayMode", 1, dd_RestoreDisplayMode}, {"SetCooperativeLevel", 3, dd_SetCooperativeLevel}, \
    {"SetDisplayMode", setmode_n, setmode}, {"WaitForVerticalBlank", 3, dd_WaitForVerticalBlank}

static const ComMethod m_dd[] = {DD_COMMON(dd1_CreateSurface, dd1_SetDisplayMode, 4)};
static const ComMethod m_dd2[] = {DD_COMMON(dd2_CreateSurface, dd2_SetDisplayMode, 6),
                                  {"GetAvailableVidMem", 4, dd_GetAvailableVidMem}};

/* ===================================================== IDirectDrawSurface */
static void surf_QueryInterface(CPU *c)
{
    Surface *s = surf(COM_THIS());
    uint32_t riid = ARG(1), out = ARG(2);
    static const uint8_t IID_S2[16] = {0x85, 0x58, 0x80, 0x57, 0xEC, 0x6E, 0xCF, 0x11, 0x94, 0x41, 0xA8, 0x23, 0x03, 0xC1, 0x0E, 0x27};
    static const uint8_t IID_S1[16] = {0x81, 0xDB, 0x14, 0x6C, 0x33, 0xA7, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60};
    if (!memcmp(g_mem + riid, IID_S2, 16)) { wr32(out, s->obj2); hle_return(c, DD_OK, 3); return; }
    if (!memcmp(g_mem + riid, IID_S1, 16)) { wr32(out, s->obj); hle_return(c, DD_OK, 3); return; }
    extern uint32_t d3d_surface_query_interface(void *surface, const uint8_t *iid);
    uint32_t o = d3d_surface_query_interface(s, g_mem + riid);
    if (o) { wr32(out, o); hle_return(c, DD_OK, 3); return; }
    rlog("IDirectDrawSurface::QueryInterface: unknown IID %08x", rd32(riid));
    wr32(out, 0);
    hle_return(c, E_NOINTERFACE, 3);
}

static void surf_Lock(CPU *c)
{
    Surface *s = surf(COM_THIS());
    uint32_t rect = ARG(1), desc = ARG(2);
    d3d_before_cpu_access(s);
    fill_desc(desc, s);
    if (rect) {
        int x = (int)rd32(rect), y = (int)rd32(rect + 4);
        wr32(desc + 0x24, s->mem + (uint32_t)(y * s->pitch + x * (s->bpp / 8)));
    }
    hle_return(c, DD_OK, 5);
}

static void surf_Unlock(CPU *c)
{
    Surface *s = surf(COM_THIS());
    d3d_after_cpu_write(s);
    mark_dirty(s);
    hle_return(c, DD_OK, 2);
    if (s && s->primary && s->chain_len <= 1) frame_throttle();
}

static void surf_Flip(CPU *c)
{
    Surface *s = surf(COM_THIS());
    if (s && s->primary && s->chain_len > 1) {
        d3d_before_present(s->chain[1]);
        SDL_LockMutex(g_video_lock);
        /* rotate surface memory: front <- back1 <- back2 ... <- front */
        uint32_t front = s->chain[0]->mem;
        HDBuf *fhd = s->chain[0]->hd;
        for (int i = 0; i < s->chain_len - 1; i++) {
            s->chain[i]->mem = s->chain[i + 1]->mem;
            s->chain[i]->hd = s->chain[i + 1]->hd;
        }
        s->chain[s->chain_len - 1]->mem = front;
        s->chain[s->chain_len - 1]->hd = fhd;
        g_frame_serial++;
        count_frame();
        SDL_UnlockMutex(g_video_lock);
    }
    hle_return(c, DD_OK, 3);
    frame_throttle();
}

static void surf_GetAttachedSurface(CPU *c)
{
    Surface *s = surf(COM_THIS());
    uint32_t caps = rd32(ARG(1)), out = ARG(2);
    if (s->chain_len > 1 && (caps & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP))) {
        wr32(out, s->chain[1]->obj);
        hle_return(c, DD_OK, 3);
        return;
    }
    rlog("GetAttachedSurface(caps=%08x): not found", caps);
    wr32(out, 0);
    hle_return(c, DDERR_NOTFOUND, 3);
}

static void surf_GetSurfaceDesc(CPU *c) { fill_desc(ARG(1), surf(COM_THIS())); hle_return(c, DD_OK, 2); }
static void surf_GetPixelFormat(CPU *c) { fill_pixelformat(ARG(1), surf(COM_THIS())->format); hle_return(c, DD_OK, 2); }
static void surf_GetCaps(CPU *c) { wr32(ARG(1), surf(COM_THIS())->caps); hle_return(c, DD_OK, 2); }
static void surf_IsLost(CPU *c) { hle_return(c, DD_OK, 1); }
static void surf_Restore(CPU *c) { hle_return(c, DD_OK, 1); }
static void surf_GetBltStatus(CPU *c) { hle_return(c, DD_OK, 2); }
static void surf_GetFlipStatus(CPU *c) { hle_return(c, DD_OK, 2); }
static void surf_SetClipper(CPU *c) { hle_return(c, DD_OK, 2); }

static void surf_SetPalette(CPU *c)
{
    Surface *s = surf(COM_THIS());
    s->pal = (Palette *)host_of(ARG(1));
    if (s->primary)
        for (int i = 1; i < s->chain_len; i++) s->chain[i]->pal = s->pal;
    mark_dirty(s);
    hle_return(c, DD_OK, 2);
}

static void surf_GetPalette(CPU *c)
{
    Surface *s = surf(COM_THIS());
    wr32(ARG(1), s->pal ? s->pal->obj : 0);
    hle_return(c, s->pal ? DD_OK : 0x88760083u, 2);
}

static void surf_SetColorKey(CPU *c)
{
    Surface *s = surf(COM_THIS());
    uint32_t flags = ARG(1), ck = ARG(2);
    if (flags & 8) {   /* DDCKEY_SRCBLT */
        s->has_ck = ck != 0;
        if (ck) { s->ck_lo = rd32(ck); s->ck_hi = rd32(ck + 4); }
        s->version++;
    }
    hle_return(c, DD_OK, 3);
}

static uint32_t px_get(Surface *s, int x, int y)
{
    uint32_t a = s->mem + (uint32_t)(y * s->pitch) + (uint32_t)(x * (s->bpp / 8));
    return s->bpp == 8 ? rd8(a) : s->bpp == 16 ? rd16(a) : rd32(a);
}
static void px_set(Surface *s, int x, int y, uint32_t v)
{
    uint32_t a = s->mem + (uint32_t)(y * s->pitch) + (uint32_t)(x * (s->bpp / 8));
    if (s->bpp == 8) wr8(a, v); else if (s->bpp == 16) wr16(a, v); else wr32(a, v);
}

static void read_rect(uint32_t r, Surface *s, int *x0, int *y0, int *x1, int *y1)
{
    if (r) { *x0 = (int)rd32(r); *y0 = (int)rd32(r + 4); *x1 = (int)rd32(r + 8); *y1 = (int)rd32(r + 12); }
    else { *x0 = 0; *y0 = 0; *x1 = s->w; *y1 = s->h; }
}

static void blit(Surface *d, int dx0, int dy0, int dx1, int dy1, Surface *s, int sx0, int sy0, int sx1, int sy1, bool key)
{
    int dw = dx1 - dx0, dh = dy1 - dy0, sw = sx1 - sx0, sh = sy1 - sy0;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    bool same = dw == sw && dh == sh && !key && s->bpp == d->bpp;
    for (int y = 0; y < dh; y++) {
        int ty = dy0 + y;
        if (ty < 0 || ty >= d->h) continue;
        int sy = sy0 + (same ? y : y * sh / dh);
        if (sy < 0 || sy >= s->h) continue;
        if (same && dx0 >= 0 && dx1 <= d->w && sx0 >= 0 && sx1 <= s->w) {
            memmove(g_mem + d->mem + (uint32_t)(ty * d->pitch + dx0 * (d->bpp / 8)),
                    g_mem + s->mem + (uint32_t)(sy * s->pitch + sx0 * (s->bpp / 8)), (size_t)dw * (d->bpp / 8));
            continue;
        }
        for (int x = 0; x < dw; x++) {
            int tx = dx0 + x, sx = sx0 + x * sw / dw;
            if (tx < 0 || tx >= d->w || sx < 0 || sx >= s->w) continue;
            uint32_t v = px_get(s, sx, sy);
            if (key && v >= s->ck_lo && v <= s->ck_hi) continue;
            px_set(d, tx, ty, v);
        }
    }
}

static void surf_Blt(CPU *c)
{
    Surface *d = surf(COM_THIS());
    uint32_t drect = ARG(1), src = ARG(2), srect = ARG(3), flags = ARG(4), fx = ARG(5);
    int dx0, dy0, dx1, dy1;
    read_rect(drect, d, &dx0, &dy0, &dx1, &dy1);
    d3d_before_cpu_access(d);
    if (src) d3d_before_cpu_access(surf(src));
    SDL_LockMutex(g_video_lock);
    if (flags & 0x400) {   /* DDBLT_COLORFILL */
        uint32_t color = rd32(fx + 0x50);
        for (int y = dy0 < 0 ? 0 : dy0; y < dy1 && y < d->h; y++)
            for (int x = dx0 < 0 ? 0 : dx0; x < dx1 && x < d->w; x++) px_set(d, x, y, color);
    } else if (src) {
        Surface *s = surf(src);
        int sx0, sy0, sx1, sy1;
        read_rect(srect, s, &sx0, &sy0, &sx1, &sy1);
        blit(d, dx0, dy0, dx1, dy1, s, sx0, sy0, sx1, sy1, (flags & 0x8000) && s->has_ck);
    }
    mark_dirty(d);
    SDL_UnlockMutex(g_video_lock);
    d3d_after_cpu_write(d);
    hle_return(c, DD_OK, 6);
}

static void surf_BltFast(CPU *c)
{
    Surface *d = surf(COM_THIS());
    int x = (int)ARG(1), y = (int)ARG(2);
    Surface *s = surf(ARG(3));
    uint32_t srect = ARG(4), trans = ARG(5);
    int sx0, sy0, sx1, sy1;
    read_rect(srect, s, &sx0, &sy0, &sx1, &sy1);
    d3d_before_cpu_access(d);
    d3d_before_cpu_access(s);
    SDL_LockMutex(g_video_lock);
    blit(d, x, y, x + sx1 - sx0, y + sy1 - sy0, s, sx0, sy0, sx1, sy1, (trans & 1) && s->has_ck);
    mark_dirty(d);
    SDL_UnlockMutex(g_video_lock);
    d3d_after_cpu_write(d);
    hle_return(c, DD_OK, 6);
}

static void surf_AddAttachedSurface(CPU *c) { hle_return(c, DD_OK, 2); }
static void surf_DeleteAttachedSurface(CPU *c) { hle_return(c, DD_OK, 3); }
static void surf_GetDDInterface(CPU *c) { wr32(ARG(1), g_dd_obj); hle_return(c, DD_OK, 2); }
static void surf_PageLock(CPU *c) { hle_return(c, DD_OK, 2); }

#define SURF_METHODS                                                                                       \
    {"QueryInterface", 3, surf_QueryInterface}, {"AddRef", 1, com_AddRef}, {"Release", 1, com_Release},      \
    {"AddAttachedSurface", 2, surf_AddAttachedSurface}, {"AddOverlayDirtyRect", 2, NULL},                   \
    {"Blt", 6, surf_Blt}, {"BltBatch", 4, NULL}, {"BltFast", 6, surf_BltFast},                               \
    {"DeleteAttachedSurface", 3, surf_DeleteAttachedSurface}, {"EnumAttachedSurfaces", 3, NULL},            \
    {"EnumOverlayZOrders", 4, NULL}, {"Flip", 3, surf_Flip}, {"GetAttachedSurface", 3, surf_GetAttachedSurface}, \
    {"GetBltStatus", 2, surf_GetBltStatus}, {"GetCaps", 2, surf_GetCaps}, {"GetClipper", 2, NULL},          \
    {"GetColorKey", 3, NULL}, {"GetDC", 2, NULL}, {"GetFlipStatus", 2, surf_GetFlipStatus},                 \
    {"GetOverlayPosition", 3, NULL}, {"GetPalette", 2, surf_GetPalette}, {"GetPixelFormat", 2, surf_GetPixelFormat}, \
    {"GetSurfaceDesc", 2, surf_GetSurfaceDesc}, {"Initialize", 3, NULL}, {"IsLost", 1, surf_IsLost},         \
    {"Lock", 5, surf_Lock}, {"ReleaseDC", 2, NULL}, {"Restore", 1, surf_Restore},                           \
    {"SetClipper", 2, surf_SetClipper}, {"SetColorKey", 3, surf_SetColorKey}, {"SetOverlayPosition", 3, NULL}, \
    {"SetPalette", 2, surf_SetPalette}, {"Unlock", 2, surf_Unlock}, {"UpdateOverlay", 6, NULL},             \
    {"UpdateOverlayDisplay", 2, NULL}, {"UpdateOverlayZOrder", 3, NULL}

static const ComMethod m_surf[] = {SURF_METHODS};
static const ComMethod m_surf2[] = {SURF_METHODS, {"GetDDInterface", 2, surf_GetDDInterface},
                                    {"PageLock", 2, surf_PageLock}, {"PageUnlock", 2, surf_PageLock}};

/* ===================================================== IDirectDrawPalette */
static void pal_GetEntries(CPU *c)
{
    Palette *p = host_of(COM_THIS());
    uint32_t start = ARG(2), n = ARG(3), out = ARG(4);
    for (uint32_t i = 0; i < n && start + i < 256; i++) memcpy(g_mem + out + 4 * i, p->rgb[start + i], 4);
    hle_return(c, DD_OK, 5);
}

static void pal_SetEntries(CPU *c)
{
    Palette *p = host_of(COM_THIS());
    uint32_t start = ARG(2), n = ARG(3), in = ARG(4);
    SDL_LockMutex(g_video_lock);
    for (uint32_t i = 0; i < n && start + i < 256; i++) memcpy(p->rgb[start + i], g_mem + in + 4 * i, 4);
    p->version++;
    if (g_primary && g_primary->pal == p) g_frame_serial++;
    SDL_UnlockMutex(g_video_lock);
    hle_return(c, DD_OK, 5);
}

static void pal_GetCaps(CPU *c) { wr32(ARG(1), 0x4 | 0x40); hle_return(c, DD_OK, 2); }

static const ComMethod m_pal[] = {
    {"QueryInterface", 3, NULL}, {"AddRef", 1, com_AddRef}, {"Release", 1, com_Release},
    {"GetCaps", 2, pal_GetCaps}, {"GetEntries", 5, pal_GetEntries}, {"Initialize", 4, NULL},
    {"SetEntries", 5, pal_SetEntries},
};

static const ComMethod m_clip[] = {
    {"QueryInterface", 3, NULL}, {"AddRef", 1, com_AddRef}, {"Release", 1, com_Release},
    {"GetClipList", 4, NULL}, {"GetHWnd", 2, NULL}, {"Initialize", 3, NULL}, {"IsClipListChanged", 2, NULL},
    {"SetClipList", 3, NULL}, {"SetHWnd", 3, NULL},
};

/* ================================================================ exports */
static void ddraw_init(void)
{
    if (vt_dd) return;
    g_video_lock = SDL_CreateMutex();
    vt_dd = com_vtable("IDirectDraw", m_dd, (int)(sizeof m_dd / sizeof m_dd[0]));
    vt_dd2 = com_vtable("IDirectDraw2", m_dd2, (int)(sizeof m_dd2 / sizeof m_dd2[0]));
    vt_surf = com_vtable("IDirectDrawSurface", m_surf, (int)(sizeof m_surf / sizeof m_surf[0]));
    vt_surf2 = com_vtable("IDirectDrawSurface2", m_surf2, (int)(sizeof m_surf2 / sizeof m_surf2[0]));
    vt_pal = com_vtable("IDirectDrawPalette", m_pal, (int)(sizeof m_pal / sizeof m_pal[0]));
    vt_clip = com_vtable("IDirectDrawClipper", m_clip, (int)(sizeof m_clip / sizeof m_clip[0]));
}

uint32_t ddraw_object(void) { return g_dd_obj; }

static void ddraw_DirectDrawCreate(CPU *c)
{
    ddraw_init();
    uint32_t guid = ARG(0), out = ARG(1);
    if (guid > 2) {
        rlog("DirectDrawCreate for secondary device refused");
        hle_return(c, DDERR_GENERIC, 3);
        return;
    }
    if (!g_dd_obj) {
        g_dd_obj = new_obj(vt_dd, NULL);
        g_dd2_obj = new_obj(vt_dd2, NULL);
    }
    wr32(out, g_dd_obj);
    rlog("DirectDrawCreate -> %08x", g_dd_obj);
    hle_return(c, DD_OK, 3);
}

static void ddraw_DirectDrawEnumerateA(CPU *c)
{
    uint32_t cb = ARG(0), ctx = ARG(1);
    hle_return(c, DD_OK, 2);
    static uint32_t desc, name;
    if (!desc) { desc = garena_strdup("Primary Display Driver"); name = garena_strdup("display"); }
    guest_call(c, cb, 4, 0u, desc, name, ctx);
    c->eax = DD_OK;
}

const HleDef hle_ddraw[] = {
    {"DDRAW", "DirectDrawCreate", ddraw_DirectDrawCreate},
    {"DDRAW", "DirectDrawEnumerateA", ddraw_DirectDrawEnumerateA},
    {0, 0, 0},
};
