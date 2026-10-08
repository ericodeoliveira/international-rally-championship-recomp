/* Software rasterizer for D3DTLVERTEX triangles into an XRGB8888 buffer.
 *
 * Setup turns every vertex attribute into a screen-space plane equation; each scanline's
 * covered span is solved analytically from the three edges (pixel centres at +0.5, left
 * edges inclusive, right edges exclusive, so shared edges are drawn once). Texture
 * coordinates are interpolated perspective-correctly (u/w, v/w, 1/w).
 *
 * Queued triangles are drawn by a pool of worker threads, each taking horizontal bands
 * and drawing every triangle of the batch, in submission order, clipped to its band. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "d3d_raster.h"

/* ------------------------------------------------------------------ batch */
typedef struct { int state; TLVertex v[3]; } Tri;

static RasterState *g_states;
static int g_nstates, g_cap_states;
static Tri *g_tris;
static int g_ntris, g_cap_tris;

void raster_set_state(const RasterState *st)
{
    if (g_nstates && !memcmp(&g_states[g_nstates - 1], st, sizeof *st)) return;
    if (g_nstates == g_cap_states) {
        g_cap_states = g_cap_states ? g_cap_states * 2 : 256;
        g_states = realloc(g_states, sizeof *g_states * (size_t)g_cap_states);
    }
    g_states[g_nstates++] = *st;
}

void raster_add(const TLVertex *a, const TLVertex *b, const TLVertex *c)
{
    if (!g_nstates) return;
    if (g_ntris == g_cap_tris) {
        g_cap_tris = g_cap_tris ? g_cap_tris * 2 : 4096;
        g_tris = realloc(g_tris, sizeof *g_tris * (size_t)g_cap_tris);
    }
    Tri *t = &g_tris[g_ntris++];
    t->state = g_nstates - 1;
    t->v[0] = *a;
    t->v[1] = *b;
    t->v[2] = *c;
}

bool raster_pending(void) { return g_ntris > 0; }

/* ------------------------------------------------------------ pixel ops */
typedef struct { float r, g, b, a; } Col;

static inline Col unpack(uint32_t c)
{
    return (Col){(float)((c >> 16) & 255), (float)((c >> 8) & 255), (float)(c & 255), (float)(c >> 24)};
}

static inline int wrap_coord(int i, int n, int mode)
{
    if (mode == 3) return i < 0 ? 0 : i >= n ? n - 1 : i;
    if (mode == 2) {
        int p = ((i % (2 * n)) + 2 * n) % (2 * n);
        return p < n ? p : 2 * n - 1 - p;
    }
    if ((n & (n - 1)) == 0) return i & (n - 1);
    return ((i % n) + n) % n;
}

static inline uint32_t texel(const RasterState *st, int x, int y)
{
    return st->tex[wrap_coord(y, st->tex_h, st->address) * st->tex_w + wrap_coord(x, st->tex_w, st->address)];
}

/* bilinear: colour weighted by alpha so transparent (colour-keyed) texels do not bleed */
static Col sample(const RasterState *st, float u, float v)
{
    float fu = u * (float)st->tex_w, fv = v * (float)st->tex_h;
    if (!st->linear) return unpack(texel(st, (int)floorf(fu), (int)floorf(fv)));
    fu -= 0.5f;
    fv -= 0.5f;
    float x0f = floorf(fu), y0f = floorf(fv);
    float ax = fu - x0f, ay = fv - y0f;
    int x0 = (int)x0f, y0 = (int)y0f;
    uint32_t t[4] = {texel(st, x0, y0), texel(st, x0 + 1, y0), texel(st, x0, y0 + 1), texel(st, x0 + 1, y0 + 1)};
    float w[4] = {(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay};
    float r = 0, g = 0, b = 0, a = 0;
    for (int i = 0; i < 4; i++) {
        float wa = w[i] * (float)(t[i] >> 24);
        r += wa * (float)((t[i] >> 16) & 255);
        g += wa * (float)((t[i] >> 8) & 255);
        b += wa * (float)(t[i] & 255);
        a += wa;
    }
    if (a <= 0.f) return (Col){0, 0, 0, 0};
    float inv = 1.f / a;
    return (Col){r * inv, g * inv, b * inv, a};
}

static inline float blend_factor(int mode, Col s, Col d, int comp)
{
    float sc = comp == 0 ? s.r : comp == 1 ? s.g : s.b;
    float dc = comp == 0 ? d.r : comp == 1 ? d.g : d.b;
    switch (mode) {
    case 1: return 0.f;
    case 2: return 1.f;
    case 3: return sc / 255.f;
    case 4: return 1.f - sc / 255.f;
    case 5: return s.a / 255.f;
    case 6: return 1.f - s.a / 255.f;
    case 7: return 1.f;                      /* DESTALPHA: the target has no alpha */
    case 8: return 0.f;
    case 9: return dc / 255.f;
    case 10: return 1.f - dc / 255.f;
    case 11: return s.a / 255.f;
    default: return 1.f;
    }
}

static inline bool alpha_pass(int func, float a, int ref)
{
    int v = (int)(a + 0.5f);
    switch (func) {
    case 1: return false;
    case 2: return v < ref;
    case 3: return v == ref;
    case 4: return v <= ref;
    case 5: return v > ref;
    case 6: return v != ref;
    case 7: return v >= ref;
    default: return true;
    }
}

static inline float clamp255(float v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* ----------------------------------------------------------- triangle */
enum { A_RHW, A_U, A_V, A_R, A_G, A_B, A_A, A_SR, A_SG, A_SB, A_SA, A_N };

static inline uint32_t lerp_px(uint32_t a, uint32_t b, uint32_t w)   /* w in 0..256 */
{
    uint32_t rb = (((a & 0x00FF00FFu) * (256 - w) + (b & 0x00FF00FFu) * w) >> 8) & 0x00FF00FFu;
    uint32_t ag = (((a >> 8) & 0x00FF00FFu) * (256 - w) + ((b >> 8) & 0x00FF00FFu) * w) & 0xFF00FF00u;
    return rb | ag;
}

/* Fast span for the common case: textured, wrap addressing on a power-of-two texture,
 * no blending/alpha test/specular/fog. Perspective is exact every 8 pixels and affine
 * in between; bilinear filtering in integer SWAR, falling back to the alpha-weighted float
 * sampler only next to colour-keyed texels. */
static void span_fast(const RasterState *st, uint32_t *row, int x0, int x1, float px, float py,
                      const float *fx, const float *fy, const float *fc)
{
    const int tw = st->tex_w, th = st->tex_h, wm = tw - 1, hm = th - 1;
    (void)wm; (void)hm;
    const uint32_t *tex = st->tex;
    float rhw = fx[A_RHW] * px + fy[A_RHW] * py + fc[A_RHW];
    float uw = fx[A_U] * px + fy[A_U] * py + fc[A_U];
    float vw = fx[A_V] * px + fy[A_V] * py + fc[A_V];
    const float su = (float)tw * 256.f, sv = (float)th * 256.f;
    const bool modulate = st->tex_blend != 1 && st->tex_blend != 7;
    /* Gouraud colour in 16.16 fixed point */
    int32_t cr = (int32_t)((fx[A_R] * px + fy[A_R] * py + fc[A_R]) * 65536.f), dcr = (int32_t)(fx[A_R] * 65536.f);
    int32_t cg = (int32_t)((fx[A_G] * px + fy[A_G] * py + fc[A_G]) * 65536.f), dcg = (int32_t)(fx[A_G] * 65536.f);
    int32_t cb = (int32_t)((fx[A_B] * px + fy[A_B] * py + fc[A_B]) * 65536.f), dcb = (int32_t)(fx[A_B] * 65536.f);
    /* specular (added) and fog factor (specular alpha) */
    const bool spec = st->specular, fog = st->fog;
    int32_t sr = 0, dsr = 0, sg = 0, dsg = 0, sb = 0, dsb = 0, sa = 0, dsa = 0;
    if (spec) {
        sr = (int32_t)((fx[A_SR] * px + fy[A_SR] * py + fc[A_SR]) * 65536.f); dsr = (int32_t)(fx[A_SR] * 65536.f);
        sg = (int32_t)((fx[A_SG] * px + fy[A_SG] * py + fc[A_SG]) * 65536.f); dsg = (int32_t)(fx[A_SG] * 65536.f);
        sb = (int32_t)((fx[A_SB] * px + fy[A_SB] * py + fc[A_SB]) * 65536.f); dsb = (int32_t)(fx[A_SB] * 65536.f);
    }
    if (fog) { sa = (int32_t)((fx[A_SA] * px + fy[A_SA] * py + fc[A_SA]) * 65536.f); dsa = (int32_t)(fx[A_SA] * 65536.f); }
    const uint32_t fr = (st->fog_color >> 16) & 255, fgc = (st->fog_color >> 8) & 255, fb = st->fog_color & 255;
    if (!tex) {
        for (int x = x0; x < x1; x++, cr += dcr, cg += dcg, cb += dcb, sr += dsr, sg += dsg, sb += dsb, sa += dsa) {
            int32_t r = cr >> 16, g = cg >> 16, b = cb >> 16;
            if (spec) { r += sr >> 16; g += sg >> 16; b += sb >> 16; }
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            if (fog) {
                int32_t f = sa >> 16;
                f = f < 0 ? 0 : f > 255 ? 255 : f;
                r = (r * f + (int32_t)fr * (255 - f) + 127) / 255;
                g = (g * f + (int32_t)fgc * (255 - f) + 127) / 255;
                b = (b * f + (int32_t)fb * (255 - f) + 127) / 255;
            }
            row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
        return;
    }
    float w = 1.f / rhw;
    float u0 = uw * w * su, v0 = vw * w * sv;
    for (int x = x0; x < x1;) {
        int n = x1 - x < 8 ? x1 - x : 8;
        float rhw1 = rhw + fx[A_RHW] * (float)n, uw1 = uw + fx[A_U] * (float)n, vw1 = vw + fx[A_V] * (float)n;
        float w1 = 1.f / rhw1;
        float u1 = uw1 * w1 * su, v1 = vw1 * w1 * sv;
        float du = (u1 - u0) / (float)n, dv = (v1 - v0) / (float)n;
        float uu = u0, vv = v0;
        for (int i = 0; i < n; i++, x++, uu += du, vv += dv, cr += dcr, cg += dcg, cb += dcb, sr += dsr, sg += dsg, sb += dsb, sa += dsa) {
            uint32_t t;
            if (st->linear) {
                int fu = (int)floorf(uu) - 128, fv = (int)floorf(vv) - 128;
                int tx = fu >> 8, ty = fv >> 8;
                uint32_t ax = (uint32_t)(fu & 255), ay = (uint32_t)(fv & 255);
                const uint32_t *r0 = tex + (size_t)(ty & hm) * tw, *r1 = tex + (size_t)((ty + 1) & hm) * tw;
                uint32_t t00 = r0[tx & wm], t10 = r0[(tx + 1) & wm], t01 = r1[tx & wm], t11 = r1[(tx + 1) & wm];
                if ((t00 & t10 & t01 & t11) >> 24 == 0xFF) {
                    t = lerp_px(lerp_px(t00, t10, ax), lerp_px(t01, t11, ax), ay);
                } else {
                    Col c = sample(st, uu / su, vv / sv);
                    if (st->colorkey && c.a < 127.5f) continue;
                    t = ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
                }
            } else {
                int tx = (int)floorf(uu) >> 8, ty = (int)floorf(vv) >> 8;
                t = tex[(size_t)(ty & hm) * tw + (tx & wm)];
                if (st->colorkey && (t >> 24) < 128) continue;
            }
            if (modulate) {
                int32_t r = cr >> 16, g = cg >> 16, b = cb >> 16;
                r = r < 0 ? 0 : r > 255 ? 255 : r;
                g = g < 0 ? 0 : g > 255 ? 255 : g;
                b = b < 0 ? 0 : b > 255 ? 255 : b;
                t = (((((t >> 16) & 255) * (uint32_t)r + 255) >> 8) << 16) | (((((t >> 8) & 255) * (uint32_t)g + 255) >> 8) << 8) |
                    (((t & 255) * (uint32_t)b + 255) >> 8);
            }
            if (spec || fog) {
                int32_t r = (int32_t)((t >> 16) & 255), g = (int32_t)((t >> 8) & 255), b = (int32_t)(t & 255);
                if (spec) {
                    r += sr >> 16; g += sg >> 16; b += sb >> 16;
                    r = r < 0 ? 0 : r > 255 ? 255 : r;
                    g = g < 0 ? 0 : g > 255 ? 255 : g;
                    b = b < 0 ? 0 : b > 255 ? 255 : b;
                }
                if (fog) {
                    int32_t f = sa >> 16;
                    f = f < 0 ? 0 : f > 255 ? 255 : f;
                    r = (r * f + (int32_t)fr * (255 - f) + 127) / 255;
                    g = (g * f + (int32_t)fgc * (255 - f) + 127) / 255;
                    b = (b * f + (int32_t)fb * (255 - f) + 127) / 255;
                }
                t = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
            row[x] = t & 0x00FFFFFFu;
        }
        rhw = rhw1; uw = uw1; vw = vw1;
        u0 = u1; v0 = v1;
    }
}

static void draw_tri(const RasterState *st, const TLVertex *va, const TLVertex *vb, const TLVertex *vc, int band_y0, int band_y1)
{
    const TLVertex *v[3] = {va, vb, vc};
    float area = (vb->x - va->x) * (vc->y - va->y) - (vc->x - va->x) * (vb->y - va->y);
    if (area == 0.f || !isfinite(area)) return;
    if (st->cull == 2 && area > 0) return;
    if (st->cull == 3 && area < 0) return;
    if (area < 0) { v[1] = vc; v[2] = vb; area = -area; }
    float miny = fminf(v[0]->y, fminf(v[1]->y, v[2]->y)), maxy = fmaxf(v[0]->y, fmaxf(v[1]->y, v[2]->y));
    int y0 = (int)ceilf(miny - 0.5f), y1 = (int)ceilf(maxy - 0.5f);
    int cy0 = st->clip_y0 > band_y0 ? st->clip_y0 : band_y0, cy1 = st->clip_y1 < band_y1 ? st->clip_y1 : band_y1;
    if (y0 < cy0) y0 = cy0;
    if (y1 > cy1) y1 = cy1;
    if (y0 >= y1) return;

    /* edge i is opposite vertex i: E = A x + B y + C, inside >= 0 */
    float A[3], B[3], C[3];
    for (int i = 0; i < 3; i++) {
        const TLVertex *p = v[(i + 1) % 3], *q = v[(i + 2) % 3];
        A[i] = p->y - q->y;
        B[i] = q->x - p->x;
        C[i] = p->x * q->y - p->y * q->x;
    }
    float inv = 1.f / area;
    /* attribute planes: f(x,y) = fx*x + fy*y + fc */
    float val[3][A_N];
    for (int i = 0; i < 3; i++) {
        float rhw = v[i]->rhw > 0 ? v[i]->rhw : 1e-6f;
        const TLVertex *cv = st->gouraud ? v[i] : va;
        Col c = unpack(cv->color), s = unpack(cv->specular);
        val[i][A_RHW] = rhw;
        val[i][A_U] = v[i]->u * rhw;
        val[i][A_V] = v[i]->v * rhw;
        val[i][A_R] = c.r; val[i][A_G] = c.g; val[i][A_B] = c.b; val[i][A_A] = c.a;
        val[i][A_SR] = s.r; val[i][A_SG] = s.g; val[i][A_SB] = s.b; val[i][A_SA] = s.a;
    }
    float fx[A_N], fy[A_N], fc[A_N];
    for (int k = 0; k < A_N; k++) {
        fx[k] = (val[0][k] * A[0] + val[1][k] * A[1] + val[2][k] * A[2]) * inv;
        fy[k] = (val[0][k] * B[0] + val[1][k] * B[1] + val[2][k] * B[2]) * inv;
        fc[k] = (val[0][k] * C[0] + val[1][k] * C[1] + val[2][k] * C[2]) * inv;
    }
    bool need_spec = st->specular || st->fog;
    Col fogc = unpack(st->fog_color);
    bool pow2 = st->tex && (st->tex_w & (st->tex_w - 1)) == 0 && (st->tex_h & (st->tex_h - 1)) == 0;
    bool fast = !st->alpha_blend && !st->alpha_test &&
                (!st->tex || (pow2 && st->address == 1 &&
                              (st->tex_blend == 1 || st->tex_blend == 2 || st->tex_blend == 4 || st->tex_blend == 7)));

    for (int y = y0; y < y1; y++) {
        float py = (float)y + 0.5f;
        /* span from the edges, in pixel-centre coordinates */
        float xl = (float)st->clip_x0 - 0.5f, xr = (float)st->clip_x1 - 0.5f;   /* [xl, xr) */
        bool empty = false;
        for (int i = 0; i < 3; i++) {
            float k = B[i] * py + C[i];
            if (A[i] > 0) { float b = -k / A[i]; if (b > xl) xl = b; }
            else if (A[i] < 0) { float b = -k / A[i]; if (b < xr) xr = b; }
            else if (k < 0) { empty = true; break; }
        }
        if (empty) continue;
        int x0 = (int)ceilf(xl - 0.5f), x1 = (int)ceilf(xr - 0.5f);
        if (x0 < st->clip_x0) x0 = st->clip_x0;
        if (x1 > st->clip_x1) x1 = st->clip_x1;
        if (x0 >= x1) continue;
        uint32_t *row = st->dst + (size_t)y * st->dst_pitch;
        float px = (float)x0 + 0.5f;
        if (fast) {
            span_fast(st, row, x0, x1, px, py, fx, fy, fc);
            continue;
        }
        float a[A_N];
        for (int k = 0; k < A_N; k++) a[k] = fx[k] * px + fy[k] * py + fc[k];
        for (int x = x0; x < x1; x++) {
            Col col = {a[A_R], a[A_G], a[A_B], a[A_A]};
            Col out = col;
            bool skip = false;
            if (st->tex) {
                float w = 1.f / a[A_RHW];
                Col t = sample(st, a[A_U] * w, a[A_V] * w);
                if (st->colorkey && t.a < 127.5f) skip = true;
                switch (st->tex_blend) {
                case 1: case 7: out = t; break;
                case 3:
                    out.r = col.r + (t.r - col.r) * t.a / 255.f;
                    out.g = col.g + (t.g - col.g) * t.a / 255.f;
                    out.b = col.b + (t.b - col.b) * t.a / 255.f;
                    break;
                case 4:
                    out.r = t.r * col.r / 255.f; out.g = t.g * col.g / 255.f; out.b = t.b * col.b / 255.f;
                    out.a = t.a * col.a / 255.f;
                    break;
                case 8:
                    out.r = t.r + col.r; out.g = t.g + col.g; out.b = t.b + col.b;
                    break;
                default:
                    out.r = t.r * col.r / 255.f; out.g = t.g * col.g / 255.f; out.b = t.b * col.b / 255.f;
                    out.a = st->tex_has_alpha ? t.a : col.a;
                    break;
                }
            }
            if (!skip) {
                if (need_spec) {
                    if (st->specular) { out.r += a[A_SR]; out.g += a[A_SG]; out.b += a[A_SB]; }
                    if (st->fog) {
                        float f = clamp255(a[A_SA]) / 255.f;
                        out.r = out.r * f + fogc.r * (1 - f);
                        out.g = out.g * f + fogc.g * (1 - f);
                        out.b = out.b * f + fogc.b * (1 - f);
                    }
                }
                if (!st->alpha_test || alpha_pass(st->alpha_func, out.a, st->alpha_ref)) {
                    out.r = clamp255(out.r);
                    out.g = clamp255(out.g);
                    out.b = clamp255(out.b);
                    out.a = clamp255(out.a);
                    if (st->alpha_blend) {
                        uint32_t dv = row[x];
                        Col d = {(float)((dv >> 16) & 255), (float)((dv >> 8) & 255), (float)(dv & 255), 255.f};
                        Col s = out;
                        out.r = clamp255(s.r * blend_factor(st->src_blend, s, d, 0) + d.r * blend_factor(st->dst_blend, s, d, 0));
                        out.g = clamp255(s.g * blend_factor(st->src_blend, s, d, 1) + d.g * blend_factor(st->dst_blend, s, d, 1));
                        out.b = clamp255(s.b * blend_factor(st->src_blend, s, d, 2) + d.b * blend_factor(st->dst_blend, s, d, 2));
                    }
                    row[x] = ((uint32_t)(out.r + 0.5f) << 16) | ((uint32_t)(out.g + 0.5f) << 8) | (uint32_t)(out.b + 0.5f);
                }
            }
            for (int k = 0; k < A_N; k++) a[k] += fx[k];
        }
    }
}

/* ---------------------------------------------------------- thread pool */
#define BAND_H 16
static SDL_Thread *g_workers[32];
static int g_nworkers;
static SDL_Mutex *g_pool_lock;
static SDL_Condition *g_pool_start, *g_pool_done;
static uint32_t g_job_id;
static int g_busy;
static SDL_AtomicInt g_next_band;
static int g_nbands, g_band_rows;

static void run_bands(void)
{
    for (;;) {
        int b = SDL_AddAtomicInt(&g_next_band, 1);
        if (b >= g_nbands) break;
        int by0 = b * g_band_rows, by1 = by0 + g_band_rows;
        for (int i = 0; i < g_ntris; i++) {
            const Tri *t = &g_tris[i];
            draw_tri(&g_states[t->state], &t->v[0], &t->v[1], &t->v[2], by0, by1);
        }
    }
}

static int SDLCALL worker(void *arg)
{
    (void)arg;
    uint32_t seen = 0;
    for (;;) {
        SDL_LockMutex(g_pool_lock);
        while (g_job_id == seen) SDL_WaitCondition(g_pool_start, g_pool_lock);
        seen = g_job_id;
        SDL_UnlockMutex(g_pool_lock);
        run_bands();
        SDL_LockMutex(g_pool_lock);
        if (--g_busy == 0) SDL_SignalCondition(g_pool_done);
        SDL_UnlockMutex(g_pool_lock);
    }
    return 0;
}

void raster_init(int threads)
{
    if (g_pool_lock) return;
    g_pool_lock = SDL_CreateMutex();
    g_pool_start = SDL_CreateCondition();
    g_pool_done = SDL_CreateCondition();
    if (threads > 32) threads = 32;
    for (int i = 0; i < threads; i++) {
        g_workers[i] = SDL_CreateThread(worker, "raster", NULL);
        if (g_workers[i]) g_nworkers++;
    }
}

extern uint64_t g_prof_ns[4];

void raster_flush(void)
{
    uint64_t t0 = SDL_GetTicksNS();
    g_prof_ns[3] += (uint64_t)g_ntris;
    if (!g_ntris) {
        g_nstates = 0;
        return;
    }
    int h = 0;
    for (int i = 0; i < g_nstates; i++)
        if (g_states[i].clip_y1 > h) h = g_states[i].clip_y1;
    g_band_rows = BAND_H;
    g_nbands = (h + BAND_H - 1) / BAND_H;
    SDL_SetAtomicInt(&g_next_band, 0);
    if (g_nworkers && g_ntris > 8) {
        SDL_LockMutex(g_pool_lock);
        g_busy = g_nworkers;
        g_job_id++;
        SDL_BroadcastCondition(g_pool_start);
        SDL_UnlockMutex(g_pool_lock);
        run_bands();
        SDL_LockMutex(g_pool_lock);
        while (g_busy) SDL_WaitCondition(g_pool_done, g_pool_lock);
        SDL_UnlockMutex(g_pool_lock);
    } else {
        run_bands();
    }
    g_ntris = 0;
    g_nstates = 0;
    g_prof_ns[0] += SDL_GetTicksNS() - t0;
}
