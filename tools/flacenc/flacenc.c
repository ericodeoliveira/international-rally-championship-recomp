/* Minimal lossless FLAC encoder for the CD music tracks (16-bit stereo 44.1 kHz WAV).
 *
 * Per block: the best stereo decorrelation (independent, left/side, side/right, mid/side)
 * and, per channel, the best FIXED predictor (order 0-4) with partitioned Rice residuals.
 * After writing, the file is decoded again with dr_flac and compared sample by sample;
 * the output is only kept when it is bit-identical to the input.
 *
 * usage: irc_flacenc in.wav out.flac      (exit code 0 = written and verified) */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#include "dr_flac.h"

#define BLOCK 4608
#define MAX_PORDER 6
#define MAX_LPC 12
#define LPC_PRECISION 15

/* ---------------------------------------------------------------- bit writer */
typedef struct { uint8_t *buf; size_t cap, len; uint64_t acc; int nacc; } Bits;

static void bw_flush_bytes(Bits *b)
{
    while (b->nacc >= 8) {
        if (b->len == b->cap) { b->cap = b->cap ? b->cap * 2 : 1 << 20; b->buf = realloc(b->buf, b->cap); }
        b->buf[b->len++] = (uint8_t)(b->acc >> (b->nacc - 8));
        b->nacc -= 8;
    }
}
static void bw_put(Bits *b, uint32_t v, int n)       /* n <= 32 */
{
    if (n == 0) return;
    if (n > 24) { bw_put(b, v >> 16, n - 16); bw_put(b, v & 0xFFFF, 16); return; }
    b->acc = (b->acc << n) | (v & ((1u << n) - 1));
    b->nacc += n;
    bw_flush_bytes(b);
}
static void bw_unary(Bits *b, uint32_t q)             /* q zeros then a one */
{
    while (q >= 24) { bw_put(b, 0, 24); q -= 24; }
    bw_put(b, 1, (int)q + 1);
}
static void bw_align(Bits *b) { if (b->nacc & 7) bw_put(b, 0, 8 - (b->nacc & 7)); }

/* ---------------------------------------------------------------- CRCs */
static uint8_t crc8(const uint8_t *p, size_t n)
{
    uint8_t c = 0;
    while (n--) { c ^= *p++; for (int i = 0; i < 8; i++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1); }
    return c;
}
static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0;
    while (n--) { c ^= (uint16_t)(*p++ << 8); for (int i = 0; i < 8; i++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x8005 : c << 1); }
    return c;
}

/* --------------------------------------------------------------- residuals */
static void residual(const int32_t *x, int n, int order, int32_t *r)
{
    for (int i = order; i < n; i++) {
        switch (order) {
        case 0: r[i] = x[i]; break;
        case 1: r[i] = x[i] - x[i - 1]; break;
        case 2: r[i] = x[i] - 2 * x[i - 1] + x[i - 2]; break;
        case 3: r[i] = x[i] - 3 * x[i - 1] + 3 * x[i - 2] - x[i - 3]; break;
        default: r[i] = x[i] - 4 * x[i - 1] + 6 * x[i - 2] - 4 * x[i - 3] + x[i - 4]; break;
        }
    }
}

static inline uint32_t zz(int32_t v) { return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31); }

/* exact bit count of partitioned Rice coding with the best parameter per partition */
static uint64_t rice_bits(const int32_t *r, int n, int order, int porder, int *params)
{
    int parts = 1 << porder, psize = n >> porder;
    uint64_t total = 0;
    for (int p = 0; p < parts; p++) {
        int s = p == 0 ? order : p * psize, e = (p + 1) * psize;
        uint64_t sum = 0;
        for (int i = s; i < e; i++) sum += zz(r[i]);
        int cnt = e - s, best_k = 0;
        uint64_t best = UINT64_MAX;
        int k0 = 0;
        if (cnt > 0 && sum > (uint64_t)cnt) { uint64_t m = sum / (uint64_t)cnt; while ((1ull << (k0 + 1)) <= m) k0++; }
        int klo = k0 > 1 ? k0 - 1 : 0, khi = k0 + 1;
        if (klo > 14) klo = 14;
        if (khi > 14) khi = 14;
        for (int k = klo; k <= khi; k++) {
            uint64_t bits = (uint64_t)cnt * (uint64_t)(k + 1);
            for (int i = s; i < e; i++) bits += zz(r[i]) >> k;
            if (bits < best) { best = bits; best_k = k; }
        }
        params[p] = best_k;
        total += 4 + best;
    }
    return total + 2 + 4;   /* method + partition order */
}

typedef struct { int type; int order; int porder; int params[1 << MAX_PORDER]; uint64_t bits; int shift; int32_t q[MAX_LPC]; } Choice;

/* LPC: windowed autocorrelation + Levinson-Durbin, quantised coefficients */
static void lpc_residual(const int32_t *x, int n, int order, const int32_t *q, int shift, int32_t *r)
{
    for (int i = order; i < n; i++) {
        int64_t sum = 0;
        for (int j = 0; j < order; j++) sum += (int64_t)q[j] * x[i - j - 1];
        r[i] = x[i] - (int32_t)(sum >> shift);
    }
}

static int quantize(const double *lpc, int order, int32_t *q)
{
    double cmax = 0;
    for (int i = 0; i < order; i++) if (fabs(lpc[i]) > cmax) cmax = fabs(lpc[i]);
    if (cmax <= 0) return -1;
    int log2c;
    frexp(cmax, &log2c);
    int shift = LPC_PRECISION - 1 - log2c;
    if (shift > 15) shift = 15;
    if (shift < 0) return -1;
    double err = 0;
    int32_t qmax = (1 << (LPC_PRECISION - 1)) - 1, qmin = -(1 << (LPC_PRECISION - 1));
    for (int i = 0; i < order; i++) {
        err += lpc[i] * (double)(1 << shift);
        long v = lround(err);
        if (v > qmax) v = qmax;
        if (v < qmin) v = qmin;
        err -= (double)v;
        q[i] = (int32_t)v;
    }
    return shift;
}

static void lpc_try(const int32_t *x, int n, int bps, int32_t *r, Choice *best)
{
    if (n < 64) return;
    static double w[8192];
    double R[MAX_LPC + 1] = {0};
    for (int i = 0; i < n; i++) {
        double t = 1.0;   /* Tukey(0.5) window */
        double a = 0.25 * n;
        if (i < a) t = 0.5 * (1 - cos(3.14159265358979 * i / a));
        else if (i > n - 1 - a) t = 0.5 * (1 - cos(3.14159265358979 * (n - 1 - i) / a));
        w[i] = x[i] * t;
    }
    for (int k = 0; k <= MAX_LPC; k++) {
        double sacc = 0;
        for (int i = k; i < n; i++) sacc += w[i] * w[i - k];
        R[k] = sacc;
    }
    if (R[0] <= 0) return;
    double lpc[MAX_LPC + 1][MAX_LPC] = {{0}}, a[MAX_LPC + 1] = {0}, e = R[0];
    for (int i = 1; i <= MAX_LPC; i++) {
        double acc = R[i];
        for (int j = 1; j < i; j++) acc -= a[j] * R[i - j];
        double k = acc / e;
        double na[MAX_LPC + 1];
        memcpy(na, a, sizeof a);
        na[i] = k;
        for (int j = 1; j < i; j++) na[j] = a[j] - k * a[i - j];
        memcpy(a, na, sizeof a);
        e *= (1 - k * k);
        for (int j = 0; j < i; j++) lpc[i][j] = a[j + 1];
        if (e <= 0) break;
    }
    static const int orders[] = {4, 8, 12};
    for (unsigned oi = 0; oi < sizeof orders / sizeof orders[0]; oi++) {
        int order = orders[oi];
        int32_t q[MAX_LPC];
        int shift = quantize(lpc[order], order, q);
        if (shift < 0) continue;
        lpc_residual(x, n, order, q, shift, r);
        for (int po = 0; po <= MAX_PORDER; po++) {
            if (n % (1 << po) || (n >> po) <= order) break;
            int params[1 << MAX_PORDER];
            uint64_t bits = 8 + (uint64_t)bps * (uint64_t)order + 4 + 5 + (uint64_t)LPC_PRECISION * (uint64_t)order
                            + rice_bits(r, n, order, po, params);
            if (bits < best->bits) {
                best->type = 3;
                best->order = order;
                best->porder = po;
                best->shift = shift;
                memcpy(best->q, q, sizeof q);
                memcpy(best->params, params, sizeof(int) << po);
                best->bits = bits;
            }
        }
    }
}

static Choice best_subframe(const int32_t *x, int n, int bps, int32_t *r)
{
    Choice best = {0};
    bool constant = true;
    for (int i = 1; i < n && constant; i++) constant = x[i] == x[0];
    if (constant) { best.type = 0; best.bits = 8 + (uint64_t)bps; return best; }
    best.type = 1;
    best.bits = 8 + (uint64_t)bps * (uint64_t)n;
    for (int order = 0; order <= 4 && order < n; order++) {
        residual(x, n, order, r);
        for (int po = 0; po <= MAX_PORDER; po++) {
            if (n % (1 << po) || (n >> po) <= order) break;
            int params[1 << MAX_PORDER];
            uint64_t bits = 8 + (uint64_t)bps * (uint64_t)order + rice_bits(r, n, order, po, params);
            if (bits < best.bits) {
                best.type = 2;
                best.order = order;
                best.porder = po;
                memcpy(best.params, params, sizeof(int) << po);
                best.bits = bits;
            }
        }
    }
    lpc_try(x, n, bps, r, &best);
    return best;
}

static void write_subframe(Bits *b, const int32_t *x, int n, int bps, const Choice *c, int32_t *r)
{
    if (c->type == 0) {
        bw_put(b, 0, 8);
        bw_put(b, (uint32_t)x[0] & ((1u << bps) - 1), bps);
        return;
    }
    if (c->type == 1) {
        bw_put(b, 1 << 1, 8);
        for (int i = 0; i < n; i++) bw_put(b, (uint32_t)x[i] & ((1u << bps) - 1), bps);
        return;
    }
    if (c->type == 3) {
        bw_put(b, (uint32_t)((0x20 | (c->order - 1)) << 1), 8);
        for (int i = 0; i < c->order; i++) bw_put(b, (uint32_t)x[i] & ((1u << bps) - 1), bps);
        bw_put(b, LPC_PRECISION - 1, 4);
        bw_put(b, (uint32_t)c->shift & 31, 5);
        for (int i = 0; i < c->order; i++) bw_put(b, (uint32_t)c->q[i] & ((1u << LPC_PRECISION) - 1), LPC_PRECISION);
        lpc_residual(x, n, c->order, c->q, c->shift, r);
    } else {
        bw_put(b, (uint32_t)((0x08 | c->order) << 1), 8);
        for (int i = 0; i < c->order; i++) bw_put(b, (uint32_t)x[i] & ((1u << bps) - 1), bps);
        residual(x, n, c->order, r);
    }
    bw_put(b, 0, 2);                    /* Rice, 4-bit parameters */
    bw_put(b, (uint32_t)c->porder, 4);
    int parts = 1 << c->porder, psize = n >> c->porder;
    for (int p = 0; p < parts; p++) {
        int k = c->params[p];
        bw_put(b, (uint32_t)k, 4);
        int s = p == 0 ? c->order : p * psize, e = (p + 1) * psize;
        for (int i = s; i < e; i++) {
            uint32_t u = zz(r[i]);
            bw_unary(b, u >> k);
            bw_put(b, u & ((1u << k) - 1), k);
        }
    }
}

static void put_utf8(Bits *b, uint32_t v)
{
    if (v < 0x80) { bw_put(b, v, 8); return; }
    if (v < 0x800) { bw_put(b, 0xC0 | (v >> 6), 8); bw_put(b, 0x80 | (v & 63), 8); return; }
    if (v < 0x10000) { bw_put(b, 0xE0 | (v >> 12), 8); bw_put(b, 0x80 | ((v >> 6) & 63), 8); bw_put(b, 0x80 | (v & 63), 8); return; }
    bw_put(b, 0xF0 | (v >> 18), 8); bw_put(b, 0x80 | ((v >> 12) & 63), 8);
    bw_put(b, 0x80 | ((v >> 6) & 63), 8); bw_put(b, 0x80 | (v & 63), 8);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s in.wav out.flac\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *wav = malloc((size_t)fsz);
    if (fread(wav, 1, (size_t)fsz, f) != (size_t)fsz) return 1;
    fclose(f);
    /* find the "data" chunk of a 16-bit stereo 44.1 kHz WAV */
    if (fsz < 44 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4)) { fprintf(stderr, "not a WAV\n"); return 1; }
    long pos = 12, data = -1, dlen = 0;
    while (pos + 8 <= fsz) {
        uint32_t len;
        memcpy(&len, wav + pos + 4, 4);
        if (!memcmp(wav + pos, "fmt ", 4)) {
            uint16_t fmt, ch, bits; uint32_t rate;
            memcpy(&fmt, wav + pos + 8, 2); memcpy(&ch, wav + pos + 10, 2); memcpy(&rate, wav + pos + 12, 4); memcpy(&bits, wav + pos + 22, 2);
            if (fmt != 1 || ch != 2 || bits != 16 || rate != 44100) { fprintf(stderr, "unsupported WAV format\n"); return 1; }
        }
        if (!memcmp(wav + pos, "data", 4)) { data = pos + 8; dlen = len; break; }
        pos += 8 + len + (len & 1);
    }
    if (data < 0) { fprintf(stderr, "no data chunk\n"); return 1; }
    if (data + dlen > fsz) dlen = fsz - data;
    uint32_t frames = (uint32_t)(dlen / 4);
    const int16_t *pcm = (const int16_t *)(wav + data);

    Bits out = {0};
    bw_put(&out, 'f', 8); bw_put(&out, 'L', 8); bw_put(&out, 'a', 8); bw_put(&out, 'C', 8);
    bw_put(&out, 1, 1); bw_put(&out, 0, 7); bw_put(&out, 34, 24);            /* last block, STREAMINFO */
    bw_put(&out, BLOCK, 16); bw_put(&out, BLOCK, 16);
    bw_put(&out, 0, 24); bw_put(&out, 0, 24);                                 /* frame sizes unknown */
    bw_put(&out, 44100, 20); bw_put(&out, 1, 3); bw_put(&out, 15, 5);
    bw_put(&out, 0, 4); bw_put(&out, frames, 32);                             /* 36-bit sample count */
    for (int i = 0; i < 16; i++) bw_put(&out, 0, 8);                          /* MD5 not computed */

    int32_t *L = malloc(BLOCK * 4), *R = malloc(BLOCK * 4), *S = malloc(BLOCK * 4), *M = malloc(BLOCK * 4), *res = malloc(BLOCK * 4);
    uint32_t frame_no = 0;
    for (uint32_t base = 0; base < frames; base += BLOCK, frame_no++) {
        int n = (int)(frames - base < BLOCK ? frames - base : BLOCK);
        for (int i = 0; i < n; i++) {
            L[i] = pcm[2 * (base + i)];
            R[i] = pcm[2 * (base + i) + 1];
            S[i] = L[i] - R[i];
            M[i] = (L[i] + R[i]) >> 1;
        }
        Choice cl = best_subframe(L, n, 16, res), cr = best_subframe(R, n, 16, res);
        Choice cs = best_subframe(S, n, 17, res), cm = best_subframe(M, n, 16, res);
        uint64_t opt[4] = {cl.bits + cr.bits, cl.bits + cs.bits, cs.bits + cr.bits, cm.bits + cs.bits};
        int mode = 0;
        for (int k = 1; k < 4; k++) if (opt[k] < opt[mode]) mode = k;

        size_t start = out.len;
        bw_put(&out, 0x3FFE, 14); bw_put(&out, 0, 1); bw_put(&out, 0, 1);    /* sync, reserved, fixed blocking */
        bw_put(&out, n == BLOCK ? 7 : 7, 4);                                  /* block size: 16 bit at end */
        bw_put(&out, 9, 4);                                                   /* 44.1 kHz */
        static const int chan_code[4] = {1, 8, 9, 10};
        bw_put(&out, (uint32_t)chan_code[mode], 4);
        bw_put(&out, 4, 3); bw_put(&out, 0, 1);                               /* 16 bit */
        put_utf8(&out, frame_no);
        bw_put(&out, (uint32_t)(n - 1), 16);
        bw_put(&out, crc8(out.buf + start, out.len - start), 8);
        switch (mode) {
        case 0: write_subframe(&out, L, n, 16, &cl, res); write_subframe(&out, R, n, 16, &cr, res); break;
        case 1: write_subframe(&out, L, n, 16, &cl, res); write_subframe(&out, S, n, 17, &cs, res); break;
        case 2: write_subframe(&out, S, n, 17, &cs, res); write_subframe(&out, R, n, 16, &cr, res); break;
        default: write_subframe(&out, M, n, 16, &cm, res); write_subframe(&out, S, n, 17, &cs, res); break;
        }
        bw_align(&out);
        bw_put(&out, crc16(out.buf + start, out.len - start), 16);
    }

    /* verify: decode and compare every sample before keeping the file */
    drflac *d = drflac_open_memory(out.buf, out.len, NULL);
    if (!d) { fprintf(stderr, "verification: cannot decode\n"); return 1; }
    int16_t *dec = malloc((size_t)frames * 4 + 16);
    drflac_uint64 got = drflac_read_pcm_frames_s16(d, frames, dec);
    drflac_close(d);
    if (got != frames || memcmp(dec, pcm, (size_t)frames * 4)) { fprintf(stderr, "verification FAILED: output differs\n"); return 1; }

    FILE *o = fopen(argv[2], "wb");
    if (!o || fwrite(out.buf, 1, out.len, o) != out.len) { perror(argv[2]); return 1; }
    fclose(o);
    printf("%s: %u frames, %ld -> %zu bytes (%.1f%%), verified bit-exact\n", argv[2], frames, dlen, out.len, 100.0 * (double)out.len / (double)dlen);
    return 0;
}
