/* Differential-test harness: runs lifted snippets on random CPU states and reports the final
 * state. Guest pages are materialised on first touch with deterministic pseudo-random content
 * (same generator as semtest.py), so the reference emulator sees identical memory.
 *
 * stdin : records { u32 entry, u32 seed, u32 regs[8], u8 flags[5] (cf zf sf of df), u8 pad[3] }
 * stdout: records { u32 status, u32 regs[8], u8 flags[5], u8 pad[3], u32 npages, {u32 page, u64 hash}[npages] }
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <io.h>
#include "cpu.h"

uint8_t *g_mem;
static uint32_t g_seed;
static uint32_t g_pages[4096];
static int g_npages;
static jmp_buf g_jb;

enum { ST_OK = 0, ST_DIVIDE = 1, ST_TRAP = 2, ST_UNKNOWN = 3, ST_OVERFLOW = 4 };

static uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

static void fill_page(uint32_t page)
{
    uint32_t s = (g_seed ^ (page * 2654435761u)) | 1u;
    uint32_t *p = (uint32_t *)(g_mem + page);
    for (int i = 0; i < 1024; i++) p[i] = xorshift(&s);
}

static LONG CALLBACK veh(PEXCEPTION_POINTERS e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *a = (uint8_t *)e->ExceptionRecord->ExceptionInformation[1];
    if (a < g_mem || a >= g_mem + 0x100010000ull) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t off = (uint64_t)(a - g_mem) & ~0xFFFull;
    if (g_npages >= 4096) return EXCEPTION_CONTINUE_SEARCH;
    /* accesses straddling 4 GB land in the guard area: materialise them too (x86 would wrap) */
    VirtualAlloc(g_mem + off, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    fill_page((uint32_t)off);
    g_pages[g_npages++] = (uint32_t)off;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void dispatch(CPU *c, uint32_t target)
{
    for (unsigned i = 0; i < g_func_count; i++)
        if (g_func_table[i].va == target) { g_func_table[i].fn(c); return; }
    longjmp(g_jb, ST_UNKNOWN);
}
void trap(CPU *c, uint32_t va, const char *why) { (void)c; (void)va; (void)why; longjmp(g_jb, ST_TRAP); }
void div_error(CPU *c, uint32_t va) { (void)c; (void)va; longjmp(g_jb, ST_DIVIDE); }
void noreturn_returned(CPU *c, uint32_t va) { (void)c; (void)va; longjmp(g_jb, ST_TRAP); }
void shadow_mismatch(CPU *c, uint32_t got) { (void)c; (void)got; }
void hle_cpuid(CPU *c) { (void)c; longjmp(g_jb, ST_TRAP); }
double fpu_read80(uint32_t a) { (void)a; return 0; }
void fpu_write80(uint32_t a, double v) { (void)a; (void)v; }
double fpu_round(CPU *c, double v) { (void)c; return nearbyint(v); }
void hle_return(CPU *c, uint32_t value, int nargs) { (void)c; (void)value; (void)nargs; }

static int g_fault_info(PEXCEPTION_POINTERS e)
{
    fprintf(stderr, "exception %08lx at %p addr %p (g_mem %p)\n", e->ExceptionRecord->ExceptionCode,
            e->ExceptionRecord->ExceptionAddress, (void *)e->ExceptionRecord->ExceptionInformation[1], (void *)g_mem);
    return 1;
}

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

#pragma pack(push, 1)
typedef struct { uint32_t entry, seed, regs[8]; uint8_t flags[5], pad[3]; } In;
typedef struct { uint32_t status, regs[8]; uint8_t flags[5], pad[3]; uint32_t npages; } OutHdr;
#pragma pack(pop)

int main(void)
{
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    g_mem = VirtualAlloc(NULL, 0x100010000ull, MEM_RESERVE, PAGE_NOACCESS);
    AddVectoredExceptionHandler(1, veh);
    static CPU cpu;
    In in;
    while (fread(&in, sizeof in, 1, stdin) == 1) {
        for (int i = 0; i < g_npages; i++) VirtualFree(g_mem + g_pages[i], 0x1000, MEM_DECOMMIT);
        g_npages = 0;
        g_seed = in.seed;
        CPU *c = &cpu;
        memset(c, 0, sizeof *c);
        c->fcw = 0x37F;
        uint32_t *r = &c->eax;
        for (int i = 0; i < 8; i++) r[i] = in.regs[i];
        c->cf = in.flags[0]; c->zf = in.flags[1]; c->sf = in.flags[2]; c->of = in.flags[3]; c->df = in.flags[4];
        /* esp already points at the (unwritten) return slot; the snippet's final ret pops it */
        c->shadow[c->shadow_sp++] = 0xDEAD0000u;
        int st = setjmp(g_jb);
        if (st == 0) {
            __try {
                dispatch(c, in.entry);
            } __except (g_fault_info(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER) {
                st = 7;
            }
        }
        OutHdr o = {0};
        o.status = (uint32_t)st;
        for (int i = 0; i < 8; i++) o.regs[i] = r[i];
        o.flags[0] = c->cf; o.flags[1] = c->zf; o.flags[2] = c->sf; o.flags[3] = c->of; o.flags[4] = c->df;
        o.npages = (uint32_t)g_npages;
        fwrite(&o, sizeof o, 1, stdout);
        for (int i = 0; i < g_npages; i++) {
            uint64_t hsh = fnv(g_mem + g_pages[i], 0x1000);
            fwrite(&g_pages[i], 4, 1, stdout);
            fwrite(&hsh, 8, 1, stdout);
        }
    }
    return 0;
}
