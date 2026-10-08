/* Macros used by generated code only. Registers/flags are function locals,
 * synced with the CPU struct (SAVE/LOAD) around calls and exits. */
#pragma once
#include "cpu.h"

#ifdef _MSC_VER
#pragma warning(disable : 4102 4189 4100 4146 4244 4701 4702 4065)
#else
#pragma GCC diagnostic ignored "-Wunused-label"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif

#define RD8(a) mrd8(m_, (a))
#define RD16(a) mrd16(m_, (a))
#define RD32(a) mrd32(m_, (a))
#define RD64(a) mrd64(m_, (a))
#define WR8(a, v) mwr8(m_, (a), (v))
#define WR16(a, v) mwr16(m_, (a), (v))
#define WR32(a, v) mwr32(m_, (a), (v))
#define WR64(a, v) mwr64(m_, (a), (v))
#define XCHG8(a, v) mxchg8(m_, (a), (v))
#define XCHG16(a, v) mxchg16(m_, (a), (v))
#define XCHG32(a, v) mxchg32(m_, (a), (v))

#define DECL()                                                                    \
    uint8_t *const m_ = g_mem;                                                    \
    uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esp = c->esp, \
             ebp = c->ebp, esi = c->esi, edi = c->edi;                            \
    uint8_t cf = c->cf, zf = c->zf, sf = c->sf, of = c->of, df = c->df

#define SAVE()                                                                       \
    (c->eax = eax, c->ecx = ecx, c->edx = edx, c->ebx = ebx, c->esp = esp, c->ebp = ebp, \
     c->esi = esi, c->edi = edi, c->cf = cf, c->zf = zf, c->sf = sf, c->of = of, c->df = df)
#define LOAD()                                                                       \
    (eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esp = c->esp, ebp = c->ebp, \
     esi = c->esi, edi = c->edi, cf = c->cf, zf = c->zf, sf = c->sf, of = c->of, df = c->df)

#define SHADOW_PUSH(r) (c->shadow[c->shadow_sp++ & (SHADOW_MAX - 1)] = (r))

#define CALL(f, ret)                 \
    do {                             \
        esp -= 4;                    \
        WR32(esp, (ret));            \
        SHADOW_PUSH(ret);            \
        SAVE();                      \
        f(c);                        \
        LOAD();                      \
    } while (0)

#define ICALL(t, ret)                \
    do {                             \
        esp -= 4;                    \
        WR32(esp, (ret));            \
        SHADOW_PUSH(ret);            \
        SAVE();                      \
        dispatch(c, (t));            \
        LOAD();                      \
    } while (0)

#define RET(n)                                                              \
    do {                                                                    \
        uint32_t ra_ = RD32(esp);                                           \
        esp += 4 + (n);                                                     \
        SAVE();                                                             \
        if (c->shadow[--c->shadow_sp & (SHADOW_MAX - 1)] != ra_)            \
            shadow_mismatch(c, ra_);                                        \
        return;                                                             \
    } while (0)

/* loop back-edges: force memory reloads so cross-thread polling loops work */
#define POLL() COMPILER_BARRIER()

#define EFLAGS_REST_MASK 0x240014u   /* ID | AC | AF | PF */
#define MAKE_EFLAGS()                                                                                 \
    ((uint32_t)cf | 2u | ((uint32_t)zf << 6) | ((uint32_t)sf << 7) | 0x200u | ((uint32_t)df << 10) | \
     ((uint32_t)of << 11) | c->eflags_rest)
#define SET_EFLAGS(f)                                                                                \
    (cf = (f) & 1, zf = ((f) >> 6) & 1, sf = ((f) >> 7) & 1, df = ((f) >> 10) & 1, of = ((f) >> 11) & 1, \
     c->eflags_rest = (f) & EFLAGS_REST_MASK)

/* rep movs: memmove fast path when forward copy semantics equal memmove */
#define REP_MOVS(n)                                                                     \
    do {                                                                                \
        uint32_t cnt_ = ecx * (n);                                                      \
        if (!df && (edi <= esi || edi - esi >= cnt_)) {                                 \
            memmove(m_ + edi, m_ + esi, cnt_);                                          \
            esi += cnt_; edi += cnt_; ecx = 0;                                          \
        } else {                                                                        \
            while (ecx) {                                                               \
                for (uint32_t k_ = 0; k_ < (n); k_++) m_[edi + k_] = m_[esi + k_];      \
                esi += df ? -(n) : (n); edi += df ? -(n) : (n); ecx--;                  \
            }                                                                           \
        }                                                                               \
    } while (0)

#define REP_STOS(n, v)                                                                  \
    do {                                                                                \
        uint32_t v_ = (v);                                                              \
        if ((n) == 1 && !df) { memset(m_ + edi, (int)v_, ecx); edi += ecx; ecx = 0; }   \
        else while (ecx) {                                                              \
            if ((n) == 1) WR8(edi, v_); else if ((n) == 2) WR16(edi, v_); else WR32(edi, v_); \
            edi += df ? -(n) : (n); ecx--;                                              \
        }                                                                               \
    } while (0)

/* x87 */
#define ST(i) c->st[(c->ftop + (i)) & 7]
#define FPUSH(v) do { double v_ = (v); c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = v_; } while (0)
#define FPOP() (c->ftop = (c->ftop + 1) & 7)
#define FPU_INIT() (c->ftop = 0, c->fcw = 0x37F, c->fsw = 0)
#define FPU_SW() ((uint32_t)((c->fsw & ~0x3800u) | ((c->ftop & 7u) << 11)))
static inline float lf_f32(const uint8_t *m, uint32_t a) { float f; memcpy(&f, m + a, 4); return f; }
static inline double lf_f64(const uint8_t *m, uint32_t a) { double f; memcpy(&f, m + a, 8); return f; }
#define F32(a) lf_f32(m_, (a))
#define F64(a) lf_f64(m_, (a))
#define F80(a) fpu_read80(a)
#define SETF32(a, v) do { float f_ = (float)(v); memcpy(m_ + (a), &f_, 4); } while (0)
#define SETF64(a, v) do { double f_ = (v); memcpy(m_ + (a), &f_, 8); } while (0)
#define SETF80(a, v) fpu_write80((a), (v))
#define FROUND(v) fpu_round(c, (v))
