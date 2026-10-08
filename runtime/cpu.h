/* Guest CPU model and guest memory access for recompiled x86-32 code.
 *
 * Guest memory is a 4 GB window (g_mem) reserved at startup; a guest address
 * is a 32-bit offset into it, so generated code is independent of the host
 * pointer size and runs on any little-endian 64-bit host (x64, ARM64).
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef _MSC_VER
#include <intrin.h>
#define COMPILER_BARRIER() _ReadWriteBarrier()
#define NORETURN __declspec(noreturn)
#define THREAD_LOCAL __declspec(thread)
#else
#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")
#define NORETURN __attribute__((noreturn))
#define THREAD_LOCAL _Thread_local
#endif

#define SHADOW_MAX 4096

typedef struct GuestThread GuestThread;

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint8_t cf, zf, sf, of, df;
    /* EFLAGS bits not modelled as flags (PF AF AC ID), as last loaded by popfd: keeps the
     * "toggle bit 21" CPUID-detection idiom and plain pushfd/popfd round trips exact */
    uint32_t eflags_rest;
    /* x87: plain doubles; precision differences are irrelevant for this game */
    double st[8];
    uint32_t ftop;
    uint16_t fcw, fsw;
    uint64_t mm[8];
    /* expected return addresses, to detect stack tricks the C call model cannot follow */
    uint32_t shadow[SHADOW_MAX];
    uint32_t shadow_sp;
    GuestThread *thread;
} CPU;

typedef void (*GuestFn)(CPU *c);
typedef struct { uint32_t va; GuestFn fn; } FuncEntry;
typedef struct { uint32_t iat; const char *dll; const char *name; } ImportEntry;

extern uint8_t *g_mem;

/* generated tables (table.c) */
extern const FuncEntry g_func_table[];
extern const unsigned g_func_count;
extern const ImportEntry g_import_table[];
extern const unsigned g_import_count;
extern const uint32_t g_image_base, g_image_size, g_entry_point;

/* ---- memory access (unaligned-safe, little-endian host) ---- */
static inline uint32_t mrd8(const uint8_t *m, uint32_t a) { return m[a]; }
static inline uint32_t mrd16(const uint8_t *m, uint32_t a) { uint16_t v; memcpy(&v, m + a, 2); return v; }
static inline uint32_t mrd32(const uint8_t *m, uint32_t a) { uint32_t v; memcpy(&v, m + a, 4); return v; }
static inline uint64_t mrd64(const uint8_t *m, uint32_t a) { uint64_t v; memcpy(&v, m + a, 8); return v; }
static inline void mwr8(uint8_t *m, uint32_t a, uint32_t v) { m[a] = (uint8_t)v; }
static inline void mwr16(uint8_t *m, uint32_t a, uint32_t v) { uint16_t x = (uint16_t)v; memcpy(m + a, &x, 2); }
static inline void mwr32(uint8_t *m, uint32_t a, uint32_t v) { memcpy(m + a, &v, 4); }
static inline void mwr64(uint8_t *m, uint32_t a, uint64_t v) { memcpy(m + a, &v, 8); }

/* convenience accessors for runtime (host) code */
static inline uint32_t rd8(uint32_t a) { return mrd8(g_mem, a); }
static inline uint32_t rd16(uint32_t a) { return mrd16(g_mem, a); }
static inline uint32_t rd32(uint32_t a) { return mrd32(g_mem, a); }
static inline void wr8(uint32_t a, uint32_t v) { mwr8(g_mem, a, v); }
static inline void wr16(uint32_t a, uint32_t v) { mwr16(g_mem, a, v); }
static inline void wr32(uint32_t a, uint32_t v) { mwr32(g_mem, a, v); }
static inline void *gptr(uint32_t a) { return a ? (void *)(g_mem + a) : 0; }
static inline const char *gstr(uint32_t a) { return a ? (const char *)(g_mem + a) : 0; }

/* atomic exchange (x86 xchg with memory is implicitly locked) */
static inline uint32_t mxchg8(uint8_t *m, uint32_t a, uint32_t v) {
#ifdef _MSC_VER
    return (uint8_t)_InterlockedExchange8((volatile char *)(m + a), (char)v);
#else
    return __atomic_exchange_n((uint8_t *)(m + a), (uint8_t)v, __ATOMIC_SEQ_CST);
#endif
}
static inline uint32_t mxchg16(uint8_t *m, uint32_t a, uint32_t v) {
#ifdef _MSC_VER
    return (uint16_t)_InterlockedExchange16((volatile short *)(m + a), (short)v);
#else
    return __atomic_exchange_n((uint16_t *)(m + a), (uint16_t)v, __ATOMIC_SEQ_CST);
#endif
}
static inline uint32_t mxchg32(uint8_t *m, uint32_t a, uint32_t v) {
#ifdef _MSC_VER
    return (uint32_t)_InterlockedExchange((volatile long *)(m + a), (long)v);
#else
    return __atomic_exchange_n((uint32_t *)(m + a), v, __ATOMIC_SEQ_CST);
#endif
}

/* ---- runtime services used by generated code ---- */
void dispatch(CPU *c, uint32_t target);
NORETURN void trap(CPU *c, uint32_t va, const char *why);
NORETURN void div_error(CPU *c, uint32_t va);
NORETURN void noreturn_returned(CPU *c, uint32_t va);
void shadow_mismatch(CPU *c, uint32_t got);
void hle_cpuid(CPU *c);
double fpu_read80(uint32_t a);
void fpu_write80(uint32_t a, double v);
double fpu_round(CPU *c, double v);

/* ---- host <-> guest calling helpers ---- */
#define ARG(n) rd32(c->esp + 4 + 4 * (n))
void hle_return(CPU *c, uint32_t value, int nargs);     /* stdcall return */
uint32_t guest_call(CPU *c, uint32_t fn, int nargs, ...);
