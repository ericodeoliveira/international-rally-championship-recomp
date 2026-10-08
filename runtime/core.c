/* Core runtime: guest memory, image loader, dispatch, host<->guest calls, threads, handles. */
#include <setjmp.h>
#include <stdlib.h>
#include "runtime.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

uint8_t *g_mem;
RuntimeConfig g_cfg;

/* ------------------------------------------------------------------ logging */
static FILE *g_logf;
static SDL_Mutex *g_log_lock;

void rlog(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log_lock) SDL_LockMutex(g_log_lock);
    if (!g_logf) g_logf = fopen("irc_runtime.log", "w");
    if (g_logf) { fprintf(g_logf, "[%7llu] %s\n", (unsigned long long)SDL_GetTicks(), buf); fflush(g_logf); }
    fprintf(stderr, "%s\n", buf);
    if (g_log_lock) SDL_UnlockMutex(g_log_lock);
}

#define ONCE_MAX 512
static char *g_once_keys[ONCE_MAX];
static int g_once_n;

void rlog_once(const char *key, const char *fmt, ...)
{
    for (int i = 0; i < g_once_n; i++)
        if (!strcmp(g_once_keys[i], key)) return;
    if (g_once_n < ONCE_MAX) g_once_keys[g_once_n++] = SDL_strdup(key);
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rlog("%s", buf);
}

void fatal(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rlog("FATAL: %s", buf);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "International Rally Championship", buf, NULL);
    exit(1);
}

/* ------------------------------------------------------------- guest memory */
#define GUEST_SPACE (0x100000000ull + 0x10000ull)

bool gmem_init(void)
{
#ifdef _WIN32
    g_mem = (uint8_t *)VirtualAlloc(NULL, GUEST_SPACE, MEM_RESERVE, PAGE_NOACCESS);
#else
    void *p = mmap(NULL, GUEST_SPACE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    g_mem = p == MAP_FAILED ? NULL : (uint8_t *)p;
#endif
    if (!g_mem) return false;
    g_log_lock = SDL_CreateMutex();
    gmem_commit(GUEST_ARENA_BASE, GUEST_ARENA_END - GUEST_ARENA_BASE);
    return true;
}

void gmem_commit(uint32_t addr, uint32_t size)
{
    uint64_t lo = addr & ~0xFFFull, hi = ((uint64_t)addr + size + 0xFFF) & ~0xFFFull;
#ifdef _WIN32
    if (!VirtualAlloc(g_mem + lo, (SIZE_T)(hi - lo), MEM_COMMIT, PAGE_READWRITE))
        fatal("cannot commit guest memory %08x+%x", addr, size);
#else
    if (mprotect(g_mem + lo, hi - lo, PROT_READ | PROT_WRITE))
        fatal("cannot commit guest memory %08x+%x", addr, size);
#endif
}

static void gmem_decommit(uint32_t addr, uint32_t size)
{
#ifdef _WIN32
    VirtualFree(g_mem + addr, size, MEM_DECOMMIT);
#else
    mmap(g_mem + addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
#endif
}

/* first-fit heap over [GUEST_HEAP_BASE, GUEST_HEAP_END) in 64 KB granules */
typedef struct Block { uint32_t addr, size; bool used; struct Block *next; } Block;
static Block *g_blocks;
static SDL_Mutex *g_heap_lock;
static uint32_t g_arena_top = GUEST_ARENA_BASE;

uint32_t gmem_alloc(uint32_t size)
{
    if (!g_heap_lock) g_heap_lock = SDL_CreateMutex();
    SDL_LockMutex(g_heap_lock);
    if (!g_blocks) {
        g_blocks = calloc(1, sizeof(Block));
        g_blocks->addr = GUEST_HEAP_BASE;
        g_blocks->size = GUEST_HEAP_END - GUEST_HEAP_BASE;
    }
    uint32_t need = (size + 0xFFFF) & ~0xFFFFu;
    if (!need) need = 0x10000;
    uint32_t result = 0;
    for (Block *b = g_blocks; b; b = b->next) {
        if (b->used || b->size < need) continue;
        if (b->size > need) {
            Block *rest = calloc(1, sizeof(Block));
            rest->addr = b->addr + need;
            rest->size = b->size - need;
            rest->next = b->next;
            b->next = rest;
            b->size = need;
        }
        b->used = true;
        result = b->addr;
        break;
    }
    SDL_UnlockMutex(g_heap_lock);
    if (result) gmem_commit(result, need);
    return result;
}

uint32_t gmem_size_of(uint32_t addr)
{
    for (Block *b = g_blocks; b; b = b->next)
        if (b->used && b->addr == addr) return b->size;
    return 0;
}

void gmem_free(uint32_t addr)
{
    SDL_LockMutex(g_heap_lock);
    for (Block *b = g_blocks; b; b = b->next) {
        if (b->used && b->addr == addr) {
            gmem_decommit(b->addr, b->size);
            b->used = false;
            /* merge with following free blocks */
            while (b->next && !b->next->used) {
                Block *n = b->next;
                b->size += n->size;
                b->next = n->next;
                free(n);
            }
            break;
        }
    }
    SDL_UnlockMutex(g_heap_lock);
}

uint32_t garena_alloc(uint32_t size)
{
    if (!g_heap_lock) g_heap_lock = SDL_CreateMutex();
    SDL_LockMutex(g_heap_lock);
    uint32_t a = (g_arena_top + 15) & ~15u;
    g_arena_top = a + size;
    SDL_UnlockMutex(g_heap_lock);
    if (g_arena_top > GUEST_ARENA_END) fatal("runtime arena exhausted");
    memset(g_mem + a, 0, size);
    return a;
}

uint32_t garena_strdup(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t a = garena_alloc(n);
    memcpy(g_mem + a, s, n);
    return a;
}

/* ------------------------------------------------------------- HLE registry */
#define HLE_MAX 8192
static HleFn g_hle_fn[HLE_MAX];
static const char *g_hle_name[HLE_MAX];
static int g_hle_nargs[HLE_MAX];
static int g_hle_n;
static THREAD_LOCAL int t_cur_slot;

uint32_t hle_slot(const char *name, HleFn fn)
{
    if (g_hle_n >= HLE_MAX) fatal("too many HLE slots");
    g_hle_fn[g_hle_n] = fn;
    g_hle_name[g_hle_n] = name;
    return HLE_BASE + 16u * (uint32_t)g_hle_n++;
}

const char *hle_slot_name(uint32_t addr)
{
    if (addr < HLE_BASE) return NULL;
    uint32_t i = (addr - HLE_BASE) / 16;
    return i < (uint32_t)g_hle_n ? g_hle_name[i] : NULL;
}

void hle_return(CPU *c, uint32_t value, int nargs)
{
    c->eax = value;
    c->esp += 4 + 4u * (uint32_t)nargs;
    c->shadow_sp--;
}

/* generic body for COM methods without an implementation: log once, pop args, E_NOTIMPL */
static void com_unimplemented(CPU *c)
{
    int s = t_cur_slot;
    rlog_once(g_hle_name[s], "COM method not implemented: %s (%d args)", g_hle_name[s], g_hle_nargs[s]);
    hle_return(c, 0x80004001u, g_hle_nargs[s]);
}

uint32_t com_vtable(const char *iface, const ComMethod *methods, int count)
{
    uint32_t vt = garena_alloc(4u * (uint32_t)count);
    for (int i = 0; i < count; i++) {
        char *name = malloc(128);
        snprintf(name, 128, "%s::%s", iface, methods[i].name);
        uint32_t a = hle_slot(name, methods[i].fn ? methods[i].fn : com_unimplemented);
        g_hle_nargs[(a - HLE_BASE) / 16] = methods[i].nargs;
        wr32(vt + 4u * (uint32_t)i, a);
    }
    return vt;
}

uint32_t com_object(uint32_t vtable, uint32_t extra_bytes)
{
    uint32_t obj = garena_alloc(16 + extra_bytes);
    wr32(obj, vtable);
    wr32(obj + 4, 1);   /* refcount */
    return obj;
}

/* --------------------------------------------------------------- dispatch */
static GuestFn find_func(uint32_t va)
{
    unsigned lo = 0, hi = g_func_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        uint32_t v = g_func_table[mid].va;
        if (v == va) return g_func_table[mid].fn;
        if (v < va) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

static SDL_AtomicInt g_hle_count[HLE_MAX];

static void hle_profile(uint32_t i)
{
    static SDL_AtomicU32 last;
    SDL_AddAtomicInt(&g_hle_count[i], 1);
    uint32_t now = (uint32_t)SDL_GetTicks(), prev = SDL_GetAtomicU32(&last);
    if (now - prev >= 5000 && SDL_CompareAndSwapAtomicU32(&last, prev, now)) {
        char buf[1500];
        int n = 0;
        for (int k = 0; k < g_hle_n && n < 1400; k++) {
            int v = SDL_SetAtomicInt(&g_hle_count[k], 0);
            if (v >= 50) n += snprintf(buf + n, sizeof buf - n, " %s=%d", g_hle_name[k], v);
        }
        rlog("calls/5s:%s", buf);
    }
}

void dispatch(CPU *c, uint32_t target)
{
    if (target >= HLE_BASE) {
        uint32_t i = (target - HLE_BASE) / 16;
        if (i >= (uint32_t)g_hle_n) trap(c, target, "call to invalid host slot");
        t_cur_slot = (int)i;
        if (g_cfg.trace) hle_profile(i);
        g_hle_fn[i](c);
        return;
    }
    GuestFn f = find_func(target);
    if (!f) trap(c, target, "indirect transfer to unknown code address");
    f(c);
}

uint32_t guest_call(CPU *c, uint32_t fn, int nargs, ...)
{
    uint32_t save[8] = {c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi};
    uint32_t args[16];
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    for (int i = nargs - 1; i >= 0; i--) { c->esp -= 4; wr32(c->esp, args[i]); }
    c->esp -= 4;
    wr32(c->esp, GUEST_RET_MAGIC);
    c->shadow[c->shadow_sp++ & (SHADOW_MAX - 1)] = GUEST_RET_MAGIC;
    dispatch(c, fn);
    uint32_t r = c->eax;
    c->ecx = save[1]; c->edx = save[2]; c->ebx = save[3]; c->esp = save[4];
    c->ebp = save[5]; c->esi = save[6]; c->edi = save[7];
    return r;
}

/* ------------------------------------------------------------------ errors */
static void dump(CPU *c, char *buf, size_t n)
{
    snprintf(buf, n,
             "EAX=%08X EBX=%08X ECX=%08X EDX=%08X\nESI=%08X EDI=%08X EBP=%08X ESP=%08X\n"
             "CF=%d ZF=%d SF=%d OF=%d DF=%d",
             c->eax, c->ebx, c->ecx, c->edx, c->esi, c->edi, c->ebp, c->esp, c->cf, c->zf, c->sf, c->of, c->df);
}

void trap(CPU *c, uint32_t va, const char *why)
{
    char regs[512];
    dump(c, regs, sizeof regs);
    fatal("Guest error at %08X: %s\n%s", va, why, regs);
}

void div_error(CPU *c, uint32_t va) { trap(c, va, "divide error"); }
void noreturn_returned(CPU *c, uint32_t va) { trap(c, va, "no-return function returned"); }

void shadow_mismatch(CPU *c, uint32_t got)
{
    char key[64];
    snprintf(key, sizeof key, "shadow%08x", got);
    rlog_once(key, "warning: return address mismatch (got %08X, expected %08X)", got,
              c->shadow[c->shadow_sp & (SHADOW_MAX - 1)]);
}

void hle_cpuid(CPU *c)
{
    /* Pentium MMX */
    switch (c->eax) {
    case 0: c->eax = 1; c->ebx = 0x756E6547; c->edx = 0x49656E69; c->ecx = 0x6C65746E; break;
    default: c->eax = 0x543; c->ebx = 0; c->ecx = 0; c->edx = 0x008001BF; break;
    }
}

double fpu_read80(uint32_t a)
{
    uint64_t mant = mrd64(g_mem, a);
    uint32_t se = rd16(a + 8);
    int sign = se >> 15, exp = se & 0x7FFF;
    if (!exp && !mant) return sign ? -0.0 : 0.0;
    double v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void fpu_write80(uint32_t a, double v)
{
    int sign = signbit(v) ? 1 : 0;
    v = fabs(v);
    uint64_t mant = 0;
    int exp = 0;
    if (v != 0) {
        int e;
        double f = frexp(v, &e);          /* v = f * 2^e, f in [0.5,1) */
        mant = (uint64_t)ldexp(f, 64);
        exp = e - 1 + 16383;
    }
    mwr64(g_mem, a, mant);
    wr16(a + 8, (uint32_t)((sign << 15) | exp));
}

double fpu_round(CPU *c, double v)
{
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

/* ------------------------------------------------------------------ image */
bool image_load(const char *exe_path)
{
    size_t len;
    uint8_t *f = SDL_LoadFile(exe_path, &len);
    if (!f) return false;
    uint32_t pe = *(uint32_t *)(f + 0x3C);
    uint16_t nsec = *(uint16_t *)(f + pe + 6);
    uint16_t optsz = *(uint16_t *)(f + pe + 20);
    uint32_t base = *(uint32_t *)(f + pe + 24 + 28);
    uint32_t imgsize = *(uint32_t *)(f + pe + 24 + 56);
    uint32_t hdrsize = *(uint32_t *)(f + pe + 24 + 60);
    if (base != g_image_base || imgsize != g_image_size) {
        SDL_free(f);
        fatal("RAL.EXE does not match the recompiled build (base %08x size %08x)", base, imgsize);
    }
    gmem_commit(base, imgsize);
    memcpy(g_mem + base, f, hdrsize);
    uint8_t *sec = f + pe + 24 + optsz;
    for (int i = 0; i < nsec; i++, sec += 40) {
        uint32_t va = *(uint32_t *)(sec + 12), rawsz = *(uint32_t *)(sec + 16), rawoff = *(uint32_t *)(sec + 20);
        uint32_t vsz = *(uint32_t *)(sec + 8);
        uint32_t n = rawsz < vsz ? rawsz : vsz;
        if (rawoff + n > len) n = rawoff < len ? (uint32_t)(len - rawoff) : 0;
        memcpy(g_mem + base + va, f + rawoff, n);
    }
    SDL_free(f);
    return true;
}

/* ---------------------------------------------------------------- handles */
#define HANDLE_MAX 1024
static struct { int type; void *obj; } g_handles[HANDLE_MAX];
static SDL_Mutex *g_handle_lock;

uint32_t handle_new(int type, void *obj)
{
    if (!g_handle_lock) g_handle_lock = SDL_CreateMutex();
    SDL_LockMutex(g_handle_lock);
    for (int i = 1; i < HANDLE_MAX; i++) {
        if (g_handles[i].type == H_FREE) {
            g_handles[i].type = type;
            g_handles[i].obj = obj;
            SDL_UnlockMutex(g_handle_lock);
            return 0x1000u + 4u * (uint32_t)i;
        }
    }
    SDL_UnlockMutex(g_handle_lock);
    fatal("out of handles");
}

static int handle_index(uint32_t h)
{
    if (h < 0x1004 || (h & 3)) return -1;
    uint32_t i = (h - 0x1000) / 4;
    return i < HANDLE_MAX ? (int)i : -1;
}

void *handle_get(uint32_t h, int type)
{
    int i = handle_index(h);
    if (i < 0 || g_handles[i].type != type) return NULL;
    return g_handles[i].obj;
}

int handle_type(uint32_t h)
{
    int i = handle_index(h);
    return i < 0 ? H_FREE : g_handles[i].type;
}

void handle_close(uint32_t h)
{
    int i = handle_index(h);
    if (i >= 0) g_handles[i].type = H_FREE;
}

/* ---------------------------------------------------------------- threads */
static SDL_TLSID g_tls_thread;
static SDL_AtomicInt g_next_tid;

GuestThread *thread_current(void) { return (GuestThread *)SDL_GetTLS(&g_tls_thread); }

static GuestThread *thread_alloc(void)
{
    GuestThread *t = calloc(1, sizeof *t);
    t->lock = SDL_CreateMutex();
    t->cond = SDL_CreateCondition();
    t->tid = 0x100 + (uint32_t)SDL_AddAtomicInt(&g_next_tid, 4);
    t->stack = gmem_alloc(0x100000);
    t->cpu.esp = t->stack + 0x100000 - 64;
    t->cpu.thread = t;
    t->cpu.fcw = 0x37F;
    t->jb = malloc(sizeof(jmp_buf));
    t->handle = handle_new(H_THREAD, t);
    return t;
}

GuestThread *thread_main_init(void)
{
    GuestThread *t = thread_alloc();
    SDL_SetTLS(&g_tls_thread, t, NULL);
    return t;
}

GuestThread *thread_by_handle(uint32_t h) { return (GuestThread *)handle_get(h, H_THREAD); }

static int SDLCALL thread_main(void *arg)
{
    GuestThread *t = arg;
    SDL_SetTLS(&g_tls_thread, t, NULL);
    thread_suspend_point(t);
    if (!setjmp(*(jmp_buf *)t->jb)) {
        CPU *c = &t->cpu;
        uint32_t r = guest_call(c, t->entry, 1, t->param);
        t->exit_code = r;
    }
    t->done = 1;
    rlog("thread %x finished (code %u)", t->tid, t->exit_code);
    return 0;
}

GuestThread *thread_create(uint32_t entry, uint32_t param, bool suspended)
{
    GuestThread *t = thread_alloc();
    t->entry = entry;
    t->param = param;
    t->suspend_count = suspended ? 1 : 0;
    char name[32];
    snprintf(name, sizeof name, "guest-%08x", entry);
    t->thread = SDL_CreateThread(thread_main, name, t);
    return t;
}

void thread_exit(CPU *c, uint32_t code)
{
    GuestThread *t = c->thread;
    t->exit_code = code;
    longjmp(*(jmp_buf *)t->jb, 1);
}

void thread_suspend_point(GuestThread *t)
{
    SDL_LockMutex(t->lock);
    while (t->suspend_count > 0) SDL_WaitCondition(t->cond, t->lock);
    SDL_UnlockMutex(t->lock);
}

/* ------------------------------------------------------- debug WAV capture */
typedef struct { char name[32]; FILE *f; uint32_t bytes; int rate; } WavRec;
static WavRec g_recs[4];

static void wav_header(FILE *f, uint32_t bytes, int rate)
{
    uint32_t v;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    uint16_t fmt[2] = {1, 2}; fwrite(fmt, 2, 2, f);
    v = (uint32_t)rate; fwrite(&v, 4, 1, f); v = (uint32_t)rate * 4; fwrite(&v, 4, 1, f);
    uint16_t ba[2] = {4, 16}; fwrite(ba, 2, 2, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    fseek(f, 0, SEEK_END);
}

void wav_record(const char *name, const void *pcm, int bytes, int rate)
{
    if (!g_cfg.record_dir[0]) return;
    WavRec *r = NULL;
    for (int i = 0; i < 4 && !r; i++)
        if (g_recs[i].f && !strcmp(g_recs[i].name, name)) r = &g_recs[i];
    for (int i = 0; i < 4 && !r; i++)
        if (!g_recs[i].f) {
            r = &g_recs[i];
            snprintf(r->name, sizeof r->name, "%s", name);
            char path[1200];
            snprintf(path, sizeof path, "%s/%s.wav", g_cfg.record_dir, name);
            r->f = fopen(path, "w+b");
            r->rate = rate;
            if (r->f) wav_header(r->f, 0, rate);
        }
    if (!r || !r->f) return;
    fwrite(pcm, 1, (size_t)bytes, r->f);
    r->bytes += (uint32_t)bytes;
    wav_header(r->f, r->bytes, r->rate);
}
