/* Software rasterizer for Direct3D transformed-and-lit vertices (D3DTLVERTEX).
 * Triangles are queued and drawn in parallel by horizontal bands at flush time;
 * order is preserved inside every band, so painter's-algorithm blending is exact. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    float x, y, z, rhw;
    uint32_t color, specular;   /* D3DCOLOR: 0xAARRGGBB */
    float u, v;
} TLVertex;

typedef struct {
    /* target: XRGB8888 */
    uint32_t *dst;
    int dst_pitch;              /* in pixels */
    int dst_h;
    int clip_x0, clip_y0, clip_x1, clip_y1;
    /* texture (RGBA8 as 0xAARRGGBB), NULL = untextured */
    const uint32_t *tex;
    int tex_w, tex_h;
    bool tex_has_alpha;
    bool linear;                /* bilinear filtering */
    int address;                /* 1 wrap, 2 mirror, 3 clamp */
    int tex_blend;              /* D3DTBLEND_* */
    bool gouraud;
    bool alpha_blend;
    int src_blend, dst_blend;   /* D3DBLEND_* */
    bool alpha_test;
    int alpha_func;             /* D3DCMP_* */
    int alpha_ref;
    bool colorkey;              /* discard texels whose alpha is 0 */
    bool specular;
    bool fog;
    uint32_t fog_color;
    int cull;                   /* 1 none, 2 cw, 3 ccw */
} RasterState;

void raster_init(int threads);
/* queue a triangle; the state is copied once per call to raster_set_state */
void raster_set_state(const RasterState *st);
void raster_add(const TLVertex *a, const TLVertex *b, const TLVertex *c);
/* draw everything queued (blocks until done) */
void raster_flush(void);
bool raster_pending(void);
