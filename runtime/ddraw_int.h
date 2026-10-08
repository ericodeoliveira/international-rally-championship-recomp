/* DirectDraw objects shared between dx_ddraw.c and dx_d3d.c. */
#pragma once
#include "runtime.h"

#define DD_OK 0
#define DDERR_GENERIC 0x80004005u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_NOTFOUND 0x887600FFu
#define E_NOINTERFACE 0x80004002u

#define DDSD_CAPS 0x1
#define DDSD_HEIGHT 0x2
#define DDSD_WIDTH 0x4
#define DDSD_PITCH 0x8
#define DDSD_BACKBUFFERCOUNT 0x20
#define DDSD_LPSURFACE 0x800
#define DDSD_PIXELFORMAT 0x1000
#define DDSD_REFRESHRATE 0x40000

#define DDSCAPS_BACKBUFFER 0x4
#define DDSCAPS_COMPLEX 0x8
#define DDSCAPS_FLIP 0x10
#define DDSCAPS_FRONTBUFFER 0x20
#define DDSCAPS_OFFSCREENPLAIN 0x40
#define DDSCAPS_PRIMARYSURFACE 0x200
#define DDSCAPS_SYSTEMMEMORY 0x800
#define DDSCAPS_TEXTURE 0x1000
#define DDSCAPS_3DDEVICE 0x2000
#define DDSCAPS_VIDEOMEMORY 0x4000
#define DDSCAPS_VISIBLE 0x8000
#define DDSCAPS_ZBUFFER 0x20000

/* pixel formats a surface can have */
enum { PF_PAL8, PF_RGB565, PF_ARGB1555, PF_ARGB4444, PF_XRGB8888, PF_RGB555 };

typedef struct Palette {
    uint32_t obj;
    uint8_t rgb[256][4];
    uint32_t version;
} Palette;

/* High-resolution shadow of a 3D render target: 3D is drawn here at `scale` x the
 * surface size in XRGB8888; 2D writes the game makes to the surface are merged in by
 * comparing the surface memory against `snap` (what the shadow currently reflects). */
typedef struct HDBuf {
    uint32_t *px;
    uint16_t *snap;
    int scale;
    bool ahead;                   /* 3D drawn since the surface memory was last refreshed */
} HDBuf;

typedef struct Surface {
    uint32_t obj, obj2;           /* IDirectDrawSurface and IDirectDrawSurface2 views */
    uint32_t tex_obj;             /* IDirect3DTexture2 view (created on demand) */
    int w, h, bpp, pitch;
    int format;                   /* PF_* */
    uint32_t mem;
    uint32_t caps;
    struct Surface *chain[4];     /* flip chain: [0]=front, then back buffers */
    int chain_len;
    bool primary;
    Palette *pal;
    bool has_ck;
    uint32_t ck_lo, ck_hi;
    uint32_t version;             /* bumped whenever the pixels may have changed */
    HDBuf *hd;                    /* follows `mem` through flips */
    /* rasterizer texture cache: RGBA8, rebuilt when version/palette version changes */
    uint32_t *tex_rgba;
    uint32_t tex_version, tex_pal_version;
    void *tex_owner;
} Surface;

uint32_t dd_new_obj(uint32_t vtable, void *host);
void *dd_host_of(uint32_t obj);
Surface *dd_surface(uint32_t obj);           /* any surface view -> Surface */
void dd_fill_pixelformat(uint32_t pf, int format);
void dd_mark_dirty(Surface *s);
void dd_lock_video(void);
void dd_unlock_video(void);
int dd_format_from_ddpf(uint32_t pf);
void d3d_before_cpu_access(Surface *s);   /* refresh surface memory from the HD shadow */
void d3d_after_cpu_write(Surface *s);     /* merge 2D changes into the HD shadow */
void d3d_before_present(Surface *s);      /* same, for writes made outside Lock/Unlock */
void com_AddRef(CPU *c);
void com_Release(CPU *c);
