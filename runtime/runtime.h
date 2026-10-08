/* Runtime-internal API shared by the HLE (high-level emulation) modules. */
#pragma once
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <SDL3/SDL.h>
#include "cpu.h"

/* ---- logging ---- */
void rlog(const char *fmt, ...);
void rlog_once(const char *key, const char *fmt, ...);
NORETURN void fatal(const char *fmt, ...);

/* ---- guest memory layout ---- */
#define GUEST_ARENA_BASE 0x01000000u   /* small runtime objects (COM objects, structs) */
#define GUEST_ARENA_END 0x04000000u
#define GUEST_HEAP_BASE 0x10000000u    /* VirtualAlloc, surfaces, sound buffers, stacks */
#define GUEST_HEAP_END 0x80000000u
#define HLE_BASE 0xFFF00000u           /* sentinel "addresses" of host functions */
#define GUEST_RET_MAGIC 0xFFEFFFF0u    /* return address used by host->guest calls */

bool gmem_init(void);
void gmem_commit(uint32_t addr, uint32_t size);
uint32_t gmem_alloc(uint32_t size);           /* zeroed, 64 KB aligned, from the heap */
void gmem_free(uint32_t addr);
uint32_t gmem_size_of(uint32_t addr);
uint32_t garena_alloc(uint32_t size);         /* zeroed, 16-byte aligned, never freed */
uint32_t garena_strdup(const char *s);

/* ---- host functions callable from guest code ---- */
typedef void (*HleFn)(CPU *c);
typedef struct { const char *dll; const char *name; HleFn fn; } HleDef;
uint32_t hle_slot(const char *name, HleFn fn);          /* returns a callable guest address */
const char *hle_slot_name(uint32_t addr);
/* COM helper: build a vtable of host methods and an object pointing to it */
typedef struct { const char *name; int nargs; HleFn fn; } ComMethod;   /* nargs includes `this` */
uint32_t com_vtable(const char *iface, const ComMethod *methods, int count);
uint32_t com_object(uint32_t vtable, uint32_t extra_bytes);   /* returns guest object ptr */
#define COM_THIS() ARG(0)

/* ---- guest image ---- */
bool image_load(const char *exe_path);

/* ---- threads ---- */
struct GuestThread {
    CPU cpu;
    void *jb;                 /* jmp_buf for ExitThread */
    SDL_Thread *thread;
    SDL_Mutex *lock;
    SDL_Condition *cond;
    uint32_t entry, param, handle, tid;
    uint32_t stack;
    int suspend_count;
    uint32_t exit_code;
    volatile int done;
};
GuestThread *thread_current(void);
GuestThread *thread_main_init(void);
GuestThread *thread_create(uint32_t entry, uint32_t param, bool suspended);
GuestThread *thread_by_handle(uint32_t h);
NORETURN void thread_exit(CPU *c, uint32_t code);
void thread_suspend_point(GuestThread *t);

/* ---- handles ---- */
enum { H_FREE, H_FILE, H_FIND, H_THREAD, H_PROCESS, H_EVENT, H_MISC };
uint32_t handle_new(int type, void *obj);
void *handle_get(uint32_t h, int type);
int handle_type(uint32_t h);
void handle_close(uint32_t h);

/* ---- virtual file system ---- */
void vfs_init(const char *data_dir, const char *save_dir);
bool vfs_resolve(const char *guest_path, char *out, size_t outsz, bool for_write);
const char *vfs_data_dir(void);
extern char g_cd_drive;          /* virtual CD-ROM drive letter, e.g. 'D' */
extern char g_install_drive;     /* drive letter the game is "installed" on */

/* ---- platform (main thread) ---- */
typedef struct {
    int width, height, bpp;     /* current guest display mode */
} DisplayMode;
void platform_pump(void);                       /* main thread: events + present */
uint32_t platform_hwnd(void);
void platform_create_window(uint32_t wndproc, const char *title);
bool platform_get_message(CPU *c, uint32_t msg_ptr, bool wait);
void platform_post_message(uint32_t msg, uint32_t wparam, uint32_t lparam);
extern volatile bool g_quit_requested;
extern uint8_t g_dik_state[256];                /* DirectInput keyboard state */
extern SDL_Mutex *g_input_lock;

/* video (dx_ddraw.c) */
void video_set_mode(int w, int h, int bpp);
void video_present_if_needed(void);
int video_output_height(void);          /* pixels of the drawable area, for auto render scale */
void video_init_main_thread(SDL_Window *win);
SDL_Window *platform_window(void);

void patches_apply(void);
void wav_record(const char *name, const void *pcm, int bytes, int rate);

/* audio */
void audio_init(void);
void cdaudio_init(const char *music_dir);

/* config */
typedef struct {
    int window_scale;
    int render_scale;           /* 3D (accelerated mode) internal resolution multiplier */
    int fps_limit;              /* -1 = display refresh, 0 = unlimited */
    bool fullscreen;
    bool smooth;
    bool stretch;               /* fill the whole screen instead of keeping 4:3 */
    bool smooth3d;              /* bilinear filtering for 3D textures (accelerated mode) */
    bool trace;
    bool d3d;                   /* offer the Direct3D (accelerated) mode to the game */
    char shot_dir[1024];        /* debug: save a frame per second as BMP */
    char autokeys[2048];        /* debug: "ms:KEY[+],..." scripted key presses */
    int shot_ms;                /* debug: interval between saved frames (default 1000) */
    int exit_after_ms;          /* debug: quit after N ms */
    char record_dir[1024];      /* debug: write the mixed sound effects / CD music to WAV files */
    char poke[256];             /* debug: "addr=byte@ms,..." writes guest memory at a given time */
    char peek[256];             /* debug: "addr:len,..." hex-dumped to the log every 2 s */
    char data_dir[1024];
    char save_dir[1024];
} RuntimeConfig;
extern RuntimeConfig g_cfg;

static inline void hle_ret0(CPU *c, uint32_t v, int nargs) { hle_return(c, v, nargs); }
