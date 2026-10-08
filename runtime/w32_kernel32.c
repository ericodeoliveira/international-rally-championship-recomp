/* KERNEL32 reimplementation: files (through a virtual file system), memory, threads, time. */
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include "runtime.h"

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_(p) _mkdir(p)
#else
#include <dirent.h>
#include <unistd.h>
#define mkdir_(p) mkdir(p, 0755)
#endif

static THREAD_LOCAL uint32_t t_last_error;
#define SET_ERR(e) (t_last_error = (e))

/* ===================================================================== VFS */
static char g_root[1024];          /* host directory that represents the installed game */
char g_cd_drive = 'D';
char g_install_drive = 'C';
static char g_guest_cwd[260] = "C:\\IRC";

const char *vfs_data_dir(void) { return g_root; }

void vfs_init(const char *data_dir, const char *save_dir)
{
    (void)save_dir;
    snprintf(g_root, sizeof g_root, "%s", data_dir);
    size_t n = strlen(g_root);
    while (n && (g_root[n - 1] == '/' || g_root[n - 1] == '\\')) g_root[--n] = 0;
}

static bool host_exists(const char *p, bool *is_dir)
{
    struct stat st;
    if (stat(p, &st)) return false;
    if (is_dir) *is_dir = (st.st_mode & S_IFDIR) != 0;
    return true;
}

#ifndef _WIN32
/* case-insensitive lookup of one path component inside dir */
static bool ci_component(const char *dir, const char *name, char *out, size_t outsz)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool ok = false;
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, name)) {
            snprintf(out, outsz, "%s", e->d_name);
            ok = true;
            break;
        }
    }
    closedir(d);
    return ok;
}
#endif

/* Turn a guest path (absolute with drive letter, or relative to the guest cwd) into a host path. */
bool vfs_resolve(const char *gp, char *out, size_t outsz, bool for_write)
{
    char full[600];
    if (!gp) return false;
    if (gp[0] && gp[1] == ':') {
        snprintf(full, sizeof full, "%s", gp);
    } else if (gp[0] == '\\' || gp[0] == '/') {
        snprintf(full, sizeof full, "%c:%s", g_guest_cwd[0], gp);
    } else {
        snprintf(full, sizeof full, "%s\\%s", g_guest_cwd, gp);
    }
    char drive = (char)SDL_toupper((unsigned char)full[0]);
    const char *rest = full + 2;
    /* both the install directory and the virtual CD map onto the game root */
    if (drive == g_install_drive) {
        if (!SDL_strncasecmp(rest, "\\IRC", 4) && (rest[4] == 0 || rest[4] == '\\' || rest[4] == '/')) rest += 4;
    } else if (drive != g_cd_drive) {
        return false;
    }
    /* normalise components, resolving "." and ".." */
    char comps[64][128];
    int nc = 0;
    const char *p = rest;
    while (*p) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        char c[128];
        int k = 0;
        while (*p && *p != '\\' && *p != '/' && k < 127) c[k++] = *p++;
        c[k] = 0;
        if (!strcmp(c, ".")) continue;
        if (!strcmp(c, "..")) { if (nc) nc--; continue; }
        if (nc < 64) strcpy(comps[nc++], c);
    }
    char host[1024];
    snprintf(host, sizeof host, "%s", g_root);
    for (int i = 0; i < nc; i++) {
#ifndef _WIN32
        char real[256];
        if (ci_component(host, comps[i], real, sizeof real)) {
            strncat(host, "/", sizeof host - strlen(host) - 1);
            strncat(host, real, sizeof host - strlen(host) - 1);
            continue;
        }
#endif
        strncat(host, "/", sizeof host - strlen(host) - 1);
        strncat(host, comps[i], sizeof host - strlen(host) - 1);
    }
    (void)for_write;
    snprintf(out, outsz, "%s", host);
    return true;
}

/* ================================================================== files */
typedef struct { FILE *f; char path[1024]; bool console; } FileObj;

static void k32_CreateFileA(CPU *c)
{
    const char *name = gstr(ARG(0));
    uint32_t access = ARG(1), disp = ARG(4);
    char host[1024];
    if (!vfs_resolve(name, host, sizeof host, (access & 0x40000000) != 0)) {
        rlog("CreateFileA(%s): unmapped path", name);
        SET_ERR(3);
        hle_return(c, 0xFFFFFFFF, 7);
        return;
    }
    bool exists = host_exists(host, NULL);
    const char *mode = NULL;
    bool wr = (access & 0x40000000) != 0;
    switch (disp) {
    case 1: if (exists) { SET_ERR(80); break; } mode = "w+b"; break;           /* CREATE_NEW */
    case 2: mode = "w+b"; break;                                              /* CREATE_ALWAYS */
    case 3: if (!exists) { SET_ERR(2); break; } mode = wr ? "r+b" : "rb"; break; /* OPEN_EXISTING */
    case 4: mode = exists ? (wr ? "r+b" : "rb") : "w+b"; break;               /* OPEN_ALWAYS */
    case 5: if (!exists) { SET_ERR(2); break; } mode = "w+b"; break;          /* TRUNCATE_EXISTING */
    }
    FILE *f = mode ? fopen(host, mode) : NULL;
    if (!f && mode && wr && exists) f = fopen(host, "rb");   /* read-only media fallback */
    if (g_cfg.trace) rlog("CreateFileA(%s, %08x, %u) -> %s", name, access, disp, f ? "ok" : "FAIL");
    if (!f) {
        if (!t_last_error) SET_ERR(2);
        hle_return(c, 0xFFFFFFFF, 7);
        return;
    }
    FileObj *fo = calloc(1, sizeof *fo);
    fo->f = f;
    snprintf(fo->path, sizeof fo->path, "%s", host);
    SET_ERR(exists && (disp == 2 || disp == 4) ? 183 : 0);
    hle_return(c, handle_new(H_FILE, fo), 7);
}

static void k32_ReadFile(CPU *c)
{
    FileObj *fo = handle_get(ARG(0), H_FILE);
    uint32_t buf = ARG(1), n = ARG(2), pread = ARG(3);
    if (!fo) { SET_ERR(6); hle_return(c, 0, 5); return; }
    size_t got = n ? fread(g_mem + buf, 1, n, fo->f) : 0;
    if (pread) wr32(pread, (uint32_t)got);
    hle_return(c, 1, 5);
}

static void k32_WriteFile(CPU *c)
{
    FileObj *fo = handle_get(ARG(0), H_FILE);
    uint32_t buf = ARG(1), n = ARG(2), pw = ARG(3);
    if (!fo) { SET_ERR(6); hle_return(c, 0, 5); return; }
    size_t put;
    if (fo->console) {
        rlog("[game] %.*s", (int)n, (const char *)g_mem + buf);
        put = n;
    } else {
        put = fwrite(g_mem + buf, 1, n, fo->f);
        fflush(fo->f);
    }
    if (pw) wr32(pw, (uint32_t)put);
    hle_return(c, 1, 5);
}

static void k32_SetFilePointer(CPU *c)
{
    FileObj *fo = handle_get(ARG(0), H_FILE);
    int32_t dist = (int32_t)ARG(1);
    uint32_t method = ARG(3);
    if (!fo || fo->console) { SET_ERR(6); hle_return(c, 0xFFFFFFFF, 4); return; }
    int whence = method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END;
    fseek(fo->f, dist, whence);
    hle_return(c, (uint32_t)ftell(fo->f), 4);
}

static void k32_GetFileSize(CPU *c)
{
    FileObj *fo = handle_get(ARG(0), H_FILE);
    if (!fo || fo->console) { hle_return(c, 0xFFFFFFFF, 2); return; }
    long cur = ftell(fo->f);
    fseek(fo->f, 0, SEEK_END);
    long sz = ftell(fo->f);
    fseek(fo->f, cur, SEEK_SET);
    if (ARG(1)) wr32(ARG(1), 0);
    hle_return(c, (uint32_t)sz, 2);
}

static void unix_to_filetime(time_t t, uint32_t ft)
{
    uint64_t v = ((uint64_t)t + 11644473600ull) * 10000000ull;
    wr32(ft, (uint32_t)v);
    wr32(ft + 4, (uint32_t)(v >> 32));
}

static void k32_GetFileTime(CPU *c)
{
    FileObj *fo = handle_get(ARG(0), H_FILE);
    struct stat st;
    time_t t = time(NULL);
    if (fo && !stat(fo->path, &st)) t = st.st_mtime;
    for (int i = 1; i <= 3; i++)
        if (ARG(i)) unix_to_filetime(t, ARG(i));
    hle_return(c, fo ? 1 : 0, 4);
}

static void k32_SetFileTime(CPU *c) { hle_return(c, 1, 4); }

static void k32_FileTimeToDosDateTime(CPU *c)
{
    uint64_t v = rd32(ARG(0)) | ((uint64_t)rd32(ARG(0) + 4) << 32);
    time_t t = (time_t)(v / 10000000ull - 11644473600ull);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    if (ARG(1)) wr16(ARG(1), (uint32_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday));
    if (ARG(2)) wr16(ARG(2), (uint32_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2)));
    hle_return(c, 1, 3);
}

static void k32_DosDateTimeToFileTime(CPU *c)
{
    uint32_t d = ARG(0) & 0xFFFF, tt = ARG(1) & 0xFFFF;
    struct tm tm = {0};
    tm.tm_year = (int)(d >> 9) + 80;
    tm.tm_mon = (int)((d >> 5) & 15) - 1;
    tm.tm_mday = (int)(d & 31);
    tm.tm_hour = (int)(tt >> 11);
    tm.tm_min = (int)((tt >> 5) & 63);
    tm.tm_sec = (int)(tt & 31) * 2;
    tm.tm_isdst = -1;
    unix_to_filetime(mktime(&tm), ARG(2));
    hle_return(c, 1, 3);
}

static void k32_DeleteFileA(CPU *c)
{
    char host[1024];
    bool ok = vfs_resolve(gstr(ARG(0)), host, sizeof host, true) && remove(host) == 0;
    if (!ok) SET_ERR(2);
    hle_return(c, ok, 1);
}

static void k32_CreateDirectoryA(CPU *c)
{
    char host[1024];
    bool dir;
    bool ok = vfs_resolve(gstr(ARG(0)), host, sizeof host, true);
    if (ok && host_exists(host, &dir)) { SET_ERR(183); ok = false; }
    else if (ok) ok = mkdir_(host) == 0;
    hle_return(c, ok, 2);
}

static void k32_RemoveDirectoryA(CPU *c)
{
    char host[1024];
    bool ok = vfs_resolve(gstr(ARG(0)), host, sizeof host, true) && rmdir(host) == 0;
    hle_return(c, ok, 1);
}

static void k32_SetCurrentDirectoryA(CPU *c)
{
    const char *p = gstr(ARG(0));
    char host[1024];
    bool dir = false;
    bool ok = vfs_resolve(p, host, sizeof host, false) && host_exists(host, &dir) && dir;
    if (ok) {
        char full[260];
        if (p[0] && p[1] == ':') snprintf(full, sizeof full, "%s", p);
        else snprintf(full, sizeof full, "%s\\%s", g_guest_cwd, p);
        size_t n = strlen(full);
        while (n > 3 && (full[n - 1] == '\\' || full[n - 1] == '/')) full[--n] = 0;
        snprintf(g_guest_cwd, sizeof g_guest_cwd, "%s", full);
    }
    rlog("SetCurrentDirectoryA(%s) -> %d", p, ok);
    hle_return(c, ok, 1);
}

static void k32_GetCurrentDirectoryA(CPU *c)
{
    uint32_t n = ARG(0), buf = ARG(1);
    uint32_t len = (uint32_t)strlen(g_guest_cwd);
    if (buf && n > len) memcpy(g_mem + buf, g_guest_cwd, len + 1);
    hle_return(c, n > len ? len : len + 1, 2);
}

/* ------------------------------------------------------------- find files */
typedef struct {
    char dir[1024];
    char pattern[260];
    char **names;
    int count, pos;
} FindObj;

static bool wild_match(const char *pat, const char *s)
{
    if (!strcmp(pat, "*.*")) return true;
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return true;
            for (; *s; s++)
                if (wild_match(pat, s)) return true;
            return wild_match(pat, s);
        }
        if (!*s) return false;
        if (*pat != '?' && SDL_toupper((unsigned char)*pat) != SDL_toupper((unsigned char)*s)) return false;
        pat++;
        s++;
    }
    return *s == 0;
}

static SDL_EnumerationResult SDLCALL find_cb(void *ud, const char *dirname, const char *fname)
{
    FindObj *fo = ud;
    (void)dirname;
    if (wild_match(fo->pattern, fname)) {
        fo->names = realloc(fo->names, sizeof(char *) * (size_t)(fo->count + 1));
        fo->names[fo->count++] = SDL_strdup(fname);
    }
    return SDL_ENUM_CONTINUE;
}

static bool find_fill(FindObj *fo, uint32_t data)
{
    while (fo->pos < fo->count) {
        const char *name = fo->names[fo->pos++];
        char path[1300];
        snprintf(path, sizeof path, "%s/%s", fo->dir, name);
        struct stat st;
        if (stat(path, &st)) continue;
        memset(g_mem + data, 0, 320);
        wr32(data, (st.st_mode & S_IFDIR) ? 0x10 : 0x80);
        unix_to_filetime(st.st_mtime, data + 4);
        unix_to_filetime(st.st_mtime, data + 12);
        unix_to_filetime(st.st_mtime, data + 20);
        wr32(data + 32, (uint32_t)st.st_size);
        /* DOS-era games expect upper-case 8.3 names from the CD */
        char up[260];
        snprintf(up, sizeof up, "%s", name);
        snprintf((char *)g_mem + data + 44, 260, "%s", up);
        return true;
    }
    return false;
}

static void k32_FindFirstFileA(CPU *c)
{
    const char *pat = gstr(ARG(0));
    uint32_t data = ARG(1);
    char host[1024];
    if (!vfs_resolve(pat, host, sizeof host, false)) { SET_ERR(3); hle_return(c, 0xFFFFFFFF, 2); return; }
    FindObj *fo = calloc(1, sizeof *fo);
    char *slash = strrchr(host, '/');
    if (slash) { *slash = 0; snprintf(fo->dir, sizeof fo->dir, "%s", host); snprintf(fo->pattern, sizeof fo->pattern, "%s", slash + 1); }
    else { snprintf(fo->dir, sizeof fo->dir, "."); snprintf(fo->pattern, sizeof fo->pattern, "%s", host); }
    SDL_EnumerateDirectory(fo->dir, find_cb, fo);
    if (!find_fill(fo, data)) {
        if (g_cfg.trace) rlog("FindFirstFileA(%s): none", pat);
        free(fo);
        SET_ERR(2);
        hle_return(c, 0xFFFFFFFF, 2);
        return;
    }
    hle_return(c, handle_new(H_FIND, fo), 2);
}

static void k32_FindNextFileA(CPU *c)
{
    FindObj *fo = handle_get(ARG(0), H_FIND);
    bool ok = fo && find_fill(fo, ARG(1));
    if (!ok) SET_ERR(18);
    hle_return(c, ok, 2);
}

static void k32_FindClose(CPU *c)
{
    FindObj *fo = handle_get(ARG(0), H_FIND);
    if (fo) {
        for (int i = 0; i < fo->count; i++) SDL_free(fo->names[i]);
        free(fo->names);
        free(fo);
        handle_close(ARG(0));
    }
    hle_return(c, fo != NULL, 1);
}

static void k32_CloseHandle(CPU *c)
{
    uint32_t h = ARG(0);
    int t = handle_type(h);
    if (t == H_FILE) {
        FileObj *fo = handle_get(h, H_FILE);
        if (fo->f) fclose(fo->f);
        free(fo);
        handle_close(h);
    } else if (t == H_FIND) {
        handle_close(h);
    }
    hle_return(c, 1, 1);
}

static void k32_GetStdHandle(CPU *c)
{
    static uint32_t h;
    if (!h) {
        FileObj *fo = calloc(1, sizeof *fo);
        fo->console = true;
        h = handle_new(H_FILE, fo);
    }
    hle_return(c, h, 1);
}

/* --------------------------------------------------------------- volumes */
static void k32_GetDriveTypeA(CPU *c)
{
    const char *root = gstr(ARG(0));
    char d = root ? (char)SDL_toupper((unsigned char)root[0]) : g_guest_cwd[0];
    uint32_t r = d == g_cd_drive ? 5 : d == g_install_drive ? 3 : 1;
    hle_return(c, r, 1);
}

static void k32_GetVolumeInformationA(CPU *c)
{
    const char *root = gstr(ARG(0));
    char d = root ? (char)SDL_toupper((unsigned char)root[0]) : g_guest_cwd[0];
    uint32_t name = ARG(1), namesz = ARG(2), serial = ARG(3), maxlen = ARG(4), flags = ARG(5), fs = ARG(6), fssz = ARG(7);
    bool cd = d == g_cd_drive;
    if (!cd && d != g_install_drive) { SET_ERR(21); hle_return(c, 0, 8); return; }
    const char *label = cd ? "IRC" : "SYSTEM";
    if (name && namesz) snprintf((char *)g_mem + name, namesz, "%s", label);
    if (serial) wr32(serial, cd ? 0x1997092Au : 0x12345678u);
    if (maxlen) wr32(maxlen, cd ? 110 : 255);
    if (flags) wr32(flags, cd ? 0x80005 : 0x3);
    if (fs && fssz) snprintf((char *)g_mem + fs, fssz, "%s", cd ? "CDFS" : "FAT32");
    hle_return(c, 1, 8);
}

static void k32_GetDiskFreeSpaceA(CPU *c)
{
    if (ARG(1)) wr32(ARG(1), 64);
    if (ARG(2)) wr32(ARG(2), 512);
    if (ARG(3)) wr32(ARG(3), 60000);
    if (ARG(4)) wr32(ARG(4), 65000);
    hle_return(c, 1, 5);
}

/* ============================================================== memory */
static void k32_VirtualAlloc(CPU *c)
{
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    uint32_t r;
    if (addr && gmem_size_of(addr)) r = addr;          /* commit inside a reservation */
    else r = gmem_alloc(size);
    rlog("VirtualAlloc(%08x, %u, %x) -> %08x", addr, size, type, r);
    hle_return(c, r, 4);
}

static void k32_VirtualFree(CPU *c)
{
    uint32_t addr = ARG(0), type = ARG(2);
    if (type & 0x8000) gmem_free(addr);
    hle_return(c, 1, 3);
}

static void k32_GlobalMemoryStatus(CPU *c)
{
    uint32_t p = ARG(0);
    wr32(p, 32);
    wr32(p + 4, 20);
    wr32(p + 8, 256u << 20);
    wr32(p + 12, 200u << 20);
    wr32(p + 16, 512u << 20);
    wr32(p + 20, 400u << 20);
    wr32(p + 24, 2047u << 20);
    wr32(p + 28, 1800u << 20);
    hle_return(c, 0, 1);
}

/* ============================================================== threads */
static void k32_CreateThread(CPU *c)
{
    uint32_t start = ARG(2), param = ARG(3), flags = ARG(4), ptid = ARG(5);
    GuestThread *t = thread_create(start, param, (flags & 4) != 0);
    if (ptid) wr32(ptid, t->tid);
    rlog("CreateThread(entry=%08x) -> tid %x", start, t->tid);
    hle_return(c, t->handle, 6);
}

static void k32_ExitThread(CPU *c)
{
    uint32_t code = ARG(0);
    hle_return(c, 0, 1);
    thread_exit(c, code);
}

static void k32_ExitProcess(CPU *c)
{
    rlog("ExitProcess(%u)", ARG(0));
    SDL_Quit();
    exit((int)ARG(0));
}

static GuestThread *thread_from(uint32_t h)
{
    if (h == 0xFFFFFFFE) return thread_current();
    return thread_by_handle(h);
}

static void k32_SuspendThread(CPU *c)
{
    GuestThread *t = thread_from(ARG(0));
    if (!t) { hle_return(c, 0xFFFFFFFF, 1); return; }
    SDL_LockMutex(t->lock);
    int prev = t->suspend_count++;
    SDL_UnlockMutex(t->lock);
    hle_return(c, (uint32_t)prev, 1);
    if (t == thread_current()) thread_suspend_point(t);
}

static void k32_ResumeThread(CPU *c)
{
    GuestThread *t = thread_from(ARG(0));
    if (!t) { hle_return(c, 0xFFFFFFFF, 1); return; }
    SDL_LockMutex(t->lock);
    int prev = t->suspend_count;
    if (t->suspend_count > 0) t->suspend_count--;
    SDL_BroadcastCondition(t->cond);
    SDL_UnlockMutex(t->lock);
    hle_return(c, (uint32_t)prev, 1);
}

static bool handle_signaled(uint32_t h)
{
    GuestThread *t = thread_by_handle(h);
    if (t) return t->done != 0;
    return true;
}

static void k32_WaitForSingleObject(CPU *c)
{
    uint32_t h = ARG(0), ms = ARG(1);
    uint64_t start = SDL_GetTicks();
    while (!handle_signaled(h)) {
        if (ms != 0xFFFFFFFF && SDL_GetTicks() - start >= ms) { hle_return(c, 0x102, 2); return; }
        SDL_Delay(1);
    }
    hle_return(c, 0, 2);
}

static void k32_WaitForMultipleObjects(CPU *c)
{
    uint32_t n = ARG(0), arr = ARG(1), all = ARG(2), ms = ARG(3);
    uint64_t start = SDL_GetTicks();
    for (;;) {
        uint32_t ready = 0, first = 0xFFFFFFFF;
        for (uint32_t i = 0; i < n; i++) {
            if (handle_signaled(rd32(arr + 4 * i))) {
                ready++;
                if (first == 0xFFFFFFFF) first = i;
            }
        }
        if (all ? ready == n : ready > 0) { hle_return(c, all ? 0 : first, 4); return; }
        if (ms != 0xFFFFFFFF && SDL_GetTicks() - start >= ms) { hle_return(c, 0x102, 4); return; }
        SDL_Delay(1);
    }
}

static void k32_Sleep(CPU *c)
{
    uint32_t ms = ARG(0);
    hle_return(c, 0, 1);
    SDL_Delay(ms ? ms : 0);
    if (!ms) SDL_Delay(0);
    thread_suspend_point(thread_current());
}

static void k32_GetTickCount(CPU *c) { hle_return(c, (uint32_t)SDL_GetTicks(), 0); }
static void k32_GetCurrentThreadId(CPU *c) { hle_return(c, thread_current()->tid, 0); }
static void k32_GetCurrentThread(CPU *c) { hle_return(c, 0xFFFFFFFE, 0); }
static void k32_GetCurrentProcess(CPU *c) { hle_return(c, 0xFFFFFFFF, 0); }
static void k32_SetThreadPriority(CPU *c) { hle_return(c, 1, 2); }
static void k32_SetPriorityClass(CPU *c) { hle_return(c, 1, 2); }
static void k32_SetErrorMode(CPU *c) { hle_return(c, 0, 1); }
static void k32_GetModuleHandleA(CPU *c) { hle_return(c, ARG(0) ? 0 : g_image_base, 1); }
static void k32_GetLastError(CPU *c) { hle_return(c, t_last_error, 0); }
static void k32_GetSystemDefaultLCID(CPU *c) { hle_return(c, 0x0809, 0); }

static void k32_GetLocalTime(CPU *c)
{
    uint32_t p = ARG(0);
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    wr16(p, (uint32_t)tm.tm_year + 1900);
    wr16(p + 2, (uint32_t)tm.tm_mon + 1);
    wr16(p + 4, (uint32_t)tm.tm_wday);
    wr16(p + 6, (uint32_t)tm.tm_mday);
    wr16(p + 8, (uint32_t)tm.tm_hour);
    wr16(p + 10, (uint32_t)tm.tm_min);
    wr16(p + 12, (uint32_t)tm.tm_sec);
    wr16(p + 14, (uint32_t)(SDL_GetTicks() % 1000));
    hle_return(c, 0, 1);
}

static void k32_CreateProcessA(CPU *c)
{
    rlog("CreateProcessA(%s, %s) refused", gstr(ARG(0)) ? gstr(ARG(0)) : "", gstr(ARG(1)) ? gstr(ARG(1)) : "");
    SET_ERR(2);
    hle_return(c, 0, 10);
}

static void k32_GetExitCodeProcess(CPU *c)
{
    if (ARG(1)) wr32(ARG(1), 0);
    hle_return(c, 1, 2);
}

static void k32_LoadLibraryA(CPU *c)
{
    rlog("LoadLibraryA(%s) -> not available", gstr(ARG(0)));
    SET_ERR(126);
    hle_return(c, 0, 1);
}

static void k32_GetProcAddress(CPU *c) { hle_return(c, 0, 2); }
static void k32_FreeLibrary(CPU *c) { hle_return(c, 1, 1); }

const HleDef hle_kernel32[] = {
    {"KERNEL32", "CreateFileA", k32_CreateFileA},
    {"KERNEL32", "ReadFile", k32_ReadFile},
    {"KERNEL32", "WriteFile", k32_WriteFile},
    {"KERNEL32", "SetFilePointer", k32_SetFilePointer},
    {"KERNEL32", "GetFileSize", k32_GetFileSize},
    {"KERNEL32", "GetFileTime", k32_GetFileTime},
    {"KERNEL32", "SetFileTime", k32_SetFileTime},
    {"KERNEL32", "FileTimeToDosDateTime", k32_FileTimeToDosDateTime},
    {"KERNEL32", "DosDateTimeToFileTime", k32_DosDateTimeToFileTime},
    {"KERNEL32", "DeleteFileA", k32_DeleteFileA},
    {"KERNEL32", "CreateDirectoryA", k32_CreateDirectoryA},
    {"KERNEL32", "RemoveDirectoryA", k32_RemoveDirectoryA},
    {"KERNEL32", "SetCurrentDirectoryA", k32_SetCurrentDirectoryA},
    {"KERNEL32", "GetCurrentDirectoryA", k32_GetCurrentDirectoryA},
    {"KERNEL32", "FindFirstFileA", k32_FindFirstFileA},
    {"KERNEL32", "FindNextFileA", k32_FindNextFileA},
    {"KERNEL32", "FindClose", k32_FindClose},
    {"KERNEL32", "CloseHandle", k32_CloseHandle},
    {"KERNEL32", "GetStdHandle", k32_GetStdHandle},
    {"KERNEL32", "GetDriveTypeA", k32_GetDriveTypeA},
    {"KERNEL32", "GetVolumeInformationA", k32_GetVolumeInformationA},
    {"KERNEL32", "GetDiskFreeSpaceA", k32_GetDiskFreeSpaceA},
    {"KERNEL32", "VirtualAlloc", k32_VirtualAlloc},
    {"KERNEL32", "VirtualFree", k32_VirtualFree},
    {"KERNEL32", "GlobalMemoryStatus", k32_GlobalMemoryStatus},
    {"KERNEL32", "CreateThread", k32_CreateThread},
    {"KERNEL32", "ExitThread", k32_ExitThread},
    {"KERNEL32", "ExitProcess", k32_ExitProcess},
    {"KERNEL32", "SuspendThread", k32_SuspendThread},
    {"KERNEL32", "ResumeThread", k32_ResumeThread},
    {"KERNEL32", "WaitForSingleObject", k32_WaitForSingleObject},
    {"KERNEL32", "WaitForMultipleObjects", k32_WaitForMultipleObjects},
    {"KERNEL32", "Sleep", k32_Sleep},
    {"KERNEL32", "GetTickCount", k32_GetTickCount},
    {"KERNEL32", "GetCurrentThreadId", k32_GetCurrentThreadId},
    {"KERNEL32", "GetCurrentThread", k32_GetCurrentThread},
    {"KERNEL32", "GetCurrentProcess", k32_GetCurrentProcess},
    {"KERNEL32", "SetThreadPriority", k32_SetThreadPriority},
    {"KERNEL32", "SetPriorityClass", k32_SetPriorityClass},
    {"KERNEL32", "SetErrorMode", k32_SetErrorMode},
    {"KERNEL32", "GetModuleHandleA", k32_GetModuleHandleA},
    {"KERNEL32", "GetLastError", k32_GetLastError},
    {"KERNEL32", "GetSystemDefaultLCID", k32_GetSystemDefaultLCID},
    {"KERNEL32", "GetLocalTime", k32_GetLocalTime},
    {"KERNEL32", "CreateProcessA", k32_CreateProcessA},
    {"KERNEL32", "GetExitCodeProcess", k32_GetExitCodeProcess},
    {"KERNEL32", "LoadLibraryA", k32_LoadLibraryA},
    {"KERNEL32", "GetProcAddress", k32_GetProcAddress},
    {"KERNEL32", "FreeLibrary", k32_FreeLibrary},
    {0, 0, 0},
};
