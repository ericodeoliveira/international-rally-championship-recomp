/* USER32/GDI32/ADVAPI32 reimplementation and the SDL platform layer (window, events, message queue). */
#include <stdlib.h>
#include "runtime.h"

#define HWND_MAIN 0x00010010u
#define WM_DESTROY 0x0002
#define WM_ACTIVATE 0x0006
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ACTIVATEAPP 0x001C
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101

volatile bool g_quit_requested;
uint8_t g_dik_state[256];
SDL_Mutex *g_input_lock;

static SDL_Window *g_window;
static uint32_t g_wndproc;
static bool g_window_alive;

/* ------------------------------------------------------------ message queue */
typedef struct { uint32_t msg, wparam, lparam; } QMsg;
#define QMAX 256
static QMsg g_q[QMAX];
static int g_qhead, g_qtail;
static SDL_Mutex *g_qlock;

void platform_post_message(uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    if (!g_qlock) g_qlock = SDL_CreateMutex();
    SDL_LockMutex(g_qlock);
    int next = (g_qtail + 1) % QMAX;
    if (next != g_qhead) {
        g_q[g_qtail] = (QMsg){msg, wparam, lparam};
        g_qtail = next;
    }
    SDL_UnlockMutex(g_qlock);
}

static bool queue_pop(QMsg *m)
{
    if (!g_qlock) g_qlock = SDL_CreateMutex();
    SDL_LockMutex(g_qlock);
    bool ok = g_qhead != g_qtail;
    if (ok) {
        *m = g_q[g_qhead];
        g_qhead = (g_qhead + 1) % QMAX;
    }
    SDL_UnlockMutex(g_qlock);
    return ok;
}

uint32_t platform_hwnd(void) { return HWND_MAIN; }
SDL_Window *platform_window(void) { return g_window; }

/* ------------------------------------------------------- keyboard mapping */
static uint8_t sdl_to_dik(SDL_Scancode s)
{
    switch (s) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_1: return 0x02; case SDL_SCANCODE_2: return 0x03; case SDL_SCANCODE_3: return 0x04;
    case SDL_SCANCODE_4: return 0x05; case SDL_SCANCODE_5: return 0x06; case SDL_SCANCODE_6: return 0x07;
    case SDL_SCANCODE_7: return 0x08; case SDL_SCANCODE_8: return 0x09; case SDL_SCANCODE_9: return 0x0A;
    case SDL_SCANCODE_0: return 0x0B; case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_Q: return 0x10; case SDL_SCANCODE_W: return 0x11; case SDL_SCANCODE_E: return 0x12;
    case SDL_SCANCODE_R: return 0x13; case SDL_SCANCODE_T: return 0x14; case SDL_SCANCODE_Y: return 0x15;
    case SDL_SCANCODE_U: return 0x16; case SDL_SCANCODE_I: return 0x17; case SDL_SCANCODE_O: return 0x18;
    case SDL_SCANCODE_P: return 0x19; case SDL_SCANCODE_LEFTBRACKET: return 0x1A; case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_RETURN: return 0x1C; case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_A: return 0x1E; case SDL_SCANCODE_S: return 0x1F; case SDL_SCANCODE_D: return 0x20;
    case SDL_SCANCODE_F: return 0x21; case SDL_SCANCODE_G: return 0x22; case SDL_SCANCODE_H: return 0x23;
    case SDL_SCANCODE_J: return 0x24; case SDL_SCANCODE_K: return 0x25; case SDL_SCANCODE_L: return 0x26;
    case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_Z: return 0x2C; case SDL_SCANCODE_X: return 0x2D; case SDL_SCANCODE_C: return 0x2E;
    case SDL_SCANCODE_V: return 0x2F; case SDL_SCANCODE_B: return 0x30; case SDL_SCANCODE_N: return 0x31;
    case SDL_SCANCODE_M: return 0x32; case SDL_SCANCODE_COMMA: return 0x33; case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_RSHIFT: return 0x36; case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
    case SDL_SCANCODE_LALT: return 0x38; case SDL_SCANCODE_SPACE: return 0x39; case SDL_SCANCODE_CAPSLOCK: return 0x3A;
    case SDL_SCANCODE_F1: return 0x3B; case SDL_SCANCODE_F2: return 0x3C; case SDL_SCANCODE_F3: return 0x3D;
    case SDL_SCANCODE_F4: return 0x3E; case SDL_SCANCODE_F5: return 0x3F; case SDL_SCANCODE_F6: return 0x40;
    case SDL_SCANCODE_F7: return 0x41; case SDL_SCANCODE_F8: return 0x42; case SDL_SCANCODE_F9: return 0x43;
    case SDL_SCANCODE_F10: return 0x44; case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45; case SDL_SCANCODE_SCROLLLOCK: return 0x46;
    case SDL_SCANCODE_KP_7: return 0x47; case SDL_SCANCODE_KP_8: return 0x48; case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4A; case SDL_SCANCODE_KP_4: return 0x4B; case SDL_SCANCODE_KP_5: return 0x4C;
    case SDL_SCANCODE_KP_6: return 0x4D; case SDL_SCANCODE_KP_PLUS: return 0x4E; case SDL_SCANCODE_KP_1: return 0x4F;
    case SDL_SCANCODE_KP_2: return 0x50; case SDL_SCANCODE_KP_3: return 0x51; case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_PERIOD: return 0x53; case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_KP_ENTER: return 0x9C; case SDL_SCANCODE_RCTRL: return 0x9D; case SDL_SCANCODE_KP_DIVIDE: return 0xB5;
    case SDL_SCANCODE_PRINTSCREEN: return 0xB7; case SDL_SCANCODE_RALT: return 0xB8; case SDL_SCANCODE_PAUSE: return 0xC5;
    case SDL_SCANCODE_HOME: return 0xC7; case SDL_SCANCODE_UP: return 0xC8; case SDL_SCANCODE_PAGEUP: return 0xC9;
    case SDL_SCANCODE_LEFT: return 0xCB; case SDL_SCANCODE_RIGHT: return 0xCD; case SDL_SCANCODE_END: return 0xCF;
    case SDL_SCANCODE_DOWN: return 0xD0; case SDL_SCANCODE_PAGEDOWN: return 0xD1; case SDL_SCANCODE_INSERT: return 0xD2;
    case SDL_SCANCODE_DELETE: return 0xD3; case SDL_SCANCODE_LGUI: return 0xDB; case SDL_SCANCODE_RGUI: return 0xDC;
    case SDL_SCANCODE_APPLICATION: return 0xDD;
    default: return 0;
    }
}

/* buffered keyboard events for IDirectInputDevice::GetDeviceData */
#define KEVT_MAX 256
typedef struct { uint8_t dik; uint8_t down; uint32_t time; } KeyEvent;
static KeyEvent g_kevt[KEVT_MAX];
static int g_kevt_head, g_kevt_tail;
static uint32_t g_kevt_seq;

bool input_pop_key_event(uint8_t *dik, uint8_t *down, uint32_t *time, uint32_t *seq)
{
    SDL_LockMutex(g_input_lock);
    bool ok = g_kevt_head != g_kevt_tail;
    if (ok) {
        *dik = g_kevt[g_kevt_head].dik;
        *down = g_kevt[g_kevt_head].down;
        *time = g_kevt[g_kevt_head].time;
        *seq = g_kevt_seq++;
        g_kevt_head = (g_kevt_head + 1) % KEVT_MAX;
    }
    SDL_UnlockMutex(g_input_lock);
    return ok;
}

/* mouse state for DirectInput */
int32_t g_mouse_dx, g_mouse_dy, g_mouse_dz;
uint8_t g_mouse_buttons[4];

static void handle_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (!g_quit_requested) {
            g_quit_requested = true;
            platform_post_message(WM_CLOSE, 0, 0);
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        platform_post_message(WM_ACTIVATEAPP, 1, 0);
        platform_post_message(WM_ACTIVATE, 1, 0);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        /* keep the game running: many players alt-tab; only release held keys */
        SDL_LockMutex(g_input_lock);
        memset(g_dik_state, 0, sizeof g_dik_state);
        SDL_UnlockMutex(g_input_lock);
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (down && e->key.key == SDLK_RETURN && (e->key.mod & SDL_KMOD_ALT)) {
            g_cfg.fullscreen = !g_cfg.fullscreen;
            SDL_SetWindowFullscreen(g_window, g_cfg.fullscreen);
            break;
        }
        uint8_t dik = sdl_to_dik(e->key.scancode);
        if (!dik || (down && e->key.repeat)) break;
        SDL_LockMutex(g_input_lock);
        g_dik_state[dik] = down ? 0x80 : 0;
        int next = (g_kevt_tail + 1) % KEVT_MAX;
        if (next != g_kevt_head) {
            g_kevt[g_kevt_tail] = (KeyEvent){dik, down, (uint32_t)SDL_GetTicks()};
            g_kevt_tail = next;
        }
        SDL_UnlockMutex(g_input_lock);
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        SDL_LockMutex(g_input_lock);
        g_mouse_dx += (int32_t)e->motion.xrel;
        g_mouse_dy += (int32_t)e->motion.yrel;
        SDL_UnlockMutex(g_input_lock);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button >= 1 && e->button.button <= 3) {
            static const int map[4] = {0, 0, 2, 1};
            g_mouse_buttons[map[e->button.button]] = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 0x80 : 0;
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        g_mouse_dz += (int32_t)(e->wheel.y * 120);
        break;
    default:
        break;
    }
    extern void joystick_handle_event(const SDL_Event *e);
    joystick_handle_event(e);
}

/* debug: scripted key presses "ms:NAME,ms:NAME" (press 120 ms) using SDL key names */
static void autokeys_tick(void)
{
    static int idx;
    static uint64_t release_at;
    static uint8_t held;
    uint64_t now = SDL_GetTicks();
    if (g_cfg.exit_after_ms && now > (uint64_t)g_cfg.exit_after_ms) {
        rlog("exit-after reached");
        exit(0);
    }
    if (held && now >= release_at) {
        SDL_LockMutex(g_input_lock);
        g_dik_state[held] = 0;
        int next = (g_kevt_tail + 1) % KEVT_MAX;
        if (next != g_kevt_head) { g_kevt[g_kevt_tail] = (KeyEvent){held, 0, (uint32_t)now}; g_kevt_tail = next; }
        SDL_UnlockMutex(g_input_lock);
        held = 0;
    }
    if (held || !g_cfg.autokeys[0]) return;
    const char *p = g_cfg.autokeys;
    for (int i = 0; i < idx && p; i++) { p = strchr(p, ','); if (p) p++; }
    if (!p || !*p) return;
    unsigned ms = 0, hold = 120;
    char name[64] = {0};
    if (sscanf(p, "%u:%63[^,/]/%u", &ms, name, &hold) < 2) { idx++; return; }
    if (now < ms) return;
    idx++;
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    uint8_t dik = sdl_to_dik(sc);
    rlog("autokey %s (dik %02x) at %llu", name, dik, (unsigned long long)now);
    if (!dik) return;
    SDL_LockMutex(g_input_lock);
    g_dik_state[dik] = 0x80;
    int next = (g_kevt_tail + 1) % KEVT_MAX;
    if (next != g_kevt_head) { g_kevt[g_kevt_tail] = (KeyEvent){dik, 1, (uint32_t)now}; g_kevt_tail = next; }
    SDL_UnlockMutex(g_input_lock);
    held = dik;
    release_at = now + hold;
}

static void peek_tick(void)
{
    static uint64_t last;
    uint64_t now = SDL_GetTicks();
    if (!g_cfg.peek[0] || now - last < 2000) return;
    last = now;
    const char *p = g_cfg.peek;
    while (p && *p) {
        unsigned addr = 0, len = 16;
        if (sscanf(p, "%x:%u", &addr, &len) >= 1 && addr) {
            char line[1024];
            int n = 0;
            for (unsigned i = 0; i < len && n < 1000; i++) n += snprintf(line + n, sizeof line - n, "%02x ", rd8(addr + i));
            rlog("peek %08x: %s", addr, line);
        }
        p = strchr(p, ',');
        if (p) p++;
    }
}

static void poke_tick(void)
{
    static int done;
    const char *p = g_cfg.poke;
    int idx = 0;
    while (p && *p) {
        unsigned addr = 0, val = 0, ms = 0;
        if (sscanf(p, "%x=%x@%u", &addr, &val, &ms) == 3 && !(done & (1 << idx)) && SDL_GetTicks() >= ms) {
            wr8(addr, val);
            done |= 1 << idx;
            rlog("poke %08x = %02x", addr, val);
        }
        idx++;
        p = strchr(p, ',');
        if (p) p++;
    }
}

void platform_pump(void)
{
    autokeys_tick();
    poke_tick();
    peek_tick();
    SDL_Event e;
    while (SDL_PollEvent(&e)) handle_event(&e);
    video_present_if_needed();
}

void platform_create_window(uint32_t wndproc, const char *title)
{
    g_wndproc = wndproc;
    if (!g_input_lock) g_input_lock = SDL_CreateMutex();
    /* windowed size: an explicit scale, or the largest 4:3 that fits 85% of the usable desktop */
    int ww = 640 * 2, wh = 480 * 2;
    SDL_Rect usable;
    if (g_cfg.window_scale > 0) {
        ww = 640 * g_cfg.window_scale;
        wh = 480 * g_cfg.window_scale;
    } else if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable)) {
        wh = usable.h * 85 / 100;
        ww = wh * 4 / 3;
        if (ww > usable.w * 85 / 100) { ww = usable.w * 85 / 100; wh = ww * 3 / 4; }
    }
    g_window = SDL_CreateWindow(title, ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_window) fatal("SDL_CreateWindow: %s", SDL_GetError());
    SDL_SetWindowMinimumSize(g_window, 320, 240);
    /* borderless fullscreen at the monitor's own resolution (Alt+Enter toggles) */
    if (g_cfg.fullscreen) SDL_SetWindowFullscreen(g_window, true);
    video_init_main_thread(g_window);
    g_window_alive = true;
    /* the game's main thread waits for activation before it starts */
    platform_post_message(WM_ACTIVATEAPP, 1, 0);
    platform_post_message(WM_ACTIVATE, 1, 0);
}

bool platform_get_message(CPU *c, uint32_t msgp, bool wait)
{
    for (;;) {
        platform_pump();
        QMsg m;
        if (queue_pop(&m)) {
            wr32(msgp, HWND_MAIN);
            wr32(msgp + 4, m.msg);
            wr32(msgp + 8, m.wparam);
            wr32(msgp + 12, m.lparam);
            wr32(msgp + 16, (uint32_t)SDL_GetTicks());
            wr32(msgp + 20, 0);
            wr32(msgp + 24, 0);
            return m.msg != WM_QUIT;
        }
        if (!wait) return false;
        SDL_WaitEventTimeout(NULL, 4);
    }
}

static uint32_t call_wndproc(CPU *c, uint32_t msg, uint32_t wp, uint32_t lp)
{
    if (!g_wndproc) return 0;
    return guest_call(c, g_wndproc, 4, HWND_MAIN, msg, wp, lp);
}

/* ------------------------------------------------------------------ USER32 */
static void u32_RegisterClassA(CPU *c)
{
    uint32_t wc = ARG(0);
    g_wndproc = rd32(wc + 4);
    rlog("RegisterClassA(%s) wndproc=%08x", gstr(rd32(wc + 36)), g_wndproc);
    hle_return(c, 0xC001, 1);
}

static void u32_CreateWindowExA(CPU *c)
{
    const char *title = gstr(ARG(2));
    platform_create_window(g_wndproc, title && *title ? title : "International Rally Championship");
    hle_return(c, HWND_MAIN, 12);
}

static void u32_GetMessageA(CPU *c)
{
    uint32_t msgp = ARG(0);
    hle_return(c, 0, 4);
    c->eax = platform_get_message(c, msgp, true) ? 1 : 0;
}

static void u32_DispatchMessageA(CPU *c)
{
    uint32_t m = ARG(0);
    hle_return(c, 0, 1);
    c->eax = call_wndproc(c, rd32(m + 4), rd32(m + 8), rd32(m + 12));
}

static void u32_PostMessageA(CPU *c)
{
    platform_post_message(ARG(1), ARG(2), ARG(3));
    hle_return(c, 1, 4);
}

static void u32_PostQuitMessage(CPU *c)
{
    platform_post_message(WM_QUIT, ARG(0), 0);
    hle_return(c, 0, 1);
}

static void u32_DestroyWindow(CPU *c)
{
    hle_return(c, 1, 1);
    if (g_window_alive) {
        g_window_alive = false;
        uint32_t eax = c->eax;
        call_wndproc(c, WM_DESTROY, 0, 0);
        c->eax = eax;
    }
}

static void u32_DefWindowProcA(CPU *c)
{
    uint32_t msg = ARG(1);
    hle_return(c, 0, 4);
    if (msg == WM_CLOSE && g_window_alive) {
        g_window_alive = false;
        call_wndproc(c, WM_DESTROY, 0, 0);
        c->eax = 0;
    }
}

static void u32_MessageBoxA(CPU *c)
{
    const char *text = gstr(ARG(1)), *cap = gstr(ARG(2));
    rlog("MessageBoxA(%s): %s", cap ? cap : "", text ? text : "");
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, cap ? cap : "", text ? text : "", g_window);
    hle_return(c, 1, 4);
}

static void u32_GetSystemMetrics(CPU *c)
{
    uint32_t i = ARG(0);
    hle_return(c, i == 0 ? 640 : i == 1 ? 480 : 0, 1);
}

static void u32_GetKeyboardType(CPU *c) { hle_return(c, ARG(0) == 0 ? 4 : 0, 1); }
static void u32_ShowWindow(CPU *c) { hle_return(c, 1, 2); }
static void u32_UpdateWindow(CPU *c) { hle_return(c, 1, 1); }
static void u32_SetFocus(CPU *c) { hle_return(c, HWND_MAIN, 1); }
static void u32_SetCursor(CPU *c) { if (ARG(0)) SDL_ShowCursor(); else SDL_HideCursor(); hle_return(c, 0, 1); }
static void u32_MessageBeep(CPU *c) { hle_return(c, 1, 1); }
static void u32_IsIconic(CPU *c) { hle_return(c, (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) ? 1 : 0, 1); }
static void u32_LoadIconA(CPU *c) { hle_return(c, 0x20001, 2); }
static void u32_FindWindowA(CPU *c) { hle_return(c, 0, 2); }  /* no other instance running */

/* ------------------------------------------------------------------ GDI32 */
static void gdi_GetStockObject(CPU *c) { hle_return(c, 0x30000 + ARG(0), 1); }

/* ---------------------------------------------------------------- ADVAPI32 */
static void adv_GetUserNameA(CPU *c)
{
    uint32_t buf = ARG(0), psz = ARG(1);
    const char *name = "Player";
    uint32_t n = (uint32_t)strlen(name) + 1;
    if (buf && psz && rd32(psz) >= n) memcpy(g_mem + buf, name, n);
    if (psz) wr32(psz, n);
    hle_return(c, 1, 2);
}

static void adv_RegOpenKeyA(CPU *c)
{
    rlog("RegOpenKeyA(%08x, %s)", ARG(0), gstr(ARG(1)) ? gstr(ARG(1)) : "");
    if (ARG(2)) wr32(ARG(2), 0x5000);
    hle_return(c, 0, 3);
}

static void adv_RegSetValueExA(CPU *c)
{
    rlog("RegSetValueExA(%s)", gstr(ARG(1)) ? gstr(ARG(1)) : "");
    hle_return(c, 0, 6);
}

static void adv_RegCloseKey(CPU *c) { hle_return(c, 0, 1); }

const HleDef hle_user32[] = {
    {"USER32", "RegisterClassA", u32_RegisterClassA},
    {"USER32", "CreateWindowExA", u32_CreateWindowExA},
    {"USER32", "GetMessageA", u32_GetMessageA},
    {"USER32", "DispatchMessageA", u32_DispatchMessageA},
    {"USER32", "PostMessageA", u32_PostMessageA},
    {"USER32", "PostQuitMessage", u32_PostQuitMessage},
    {"USER32", "DestroyWindow", u32_DestroyWindow},
    {"USER32", "DefWindowProcA", u32_DefWindowProcA},
    {"USER32", "MessageBoxA", u32_MessageBoxA},
    {"USER32", "GetSystemMetrics", u32_GetSystemMetrics},
    {"USER32", "GetKeyboardType", u32_GetKeyboardType},
    {"USER32", "ShowWindow", u32_ShowWindow},
    {"USER32", "UpdateWindow", u32_UpdateWindow},
    {"USER32", "SetFocus", u32_SetFocus},
    {"USER32", "SetCursor", u32_SetCursor},
    {"USER32", "MessageBeep", u32_MessageBeep},
    {"USER32", "IsIconic", u32_IsIconic},
    {"USER32", "LoadIconA", u32_LoadIconA},
    {"USER32", "FindWindowA", u32_FindWindowA},
    {"GDI32", "GetStockObject", gdi_GetStockObject},
    {"ADVAPI32", "GetUserNameA", adv_GetUserNameA},
    {"ADVAPI32", "RegOpenKeyA", adv_RegOpenKeyA},
    {"ADVAPI32", "RegSetValueExA", adv_RegSetValueExA},
    {"ADVAPI32", "RegCloseKey", adv_RegCloseKey},
    {0, 0, 0},
};
