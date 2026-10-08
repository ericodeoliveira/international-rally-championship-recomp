/* DirectInput 5: keyboard, mouse and joysticks (SDL gamepads/joysticks), with basic
 * force feedback mapped to rumble. Device state layout follows the DIDATAFORMAT the
 * game passes to SetDataFormat, so any format (c_dfDIJoystick, custom) works. */
#include <stdlib.h>
#include "runtime.h"

#define DI_OK 0
#define DIERR_NOTFOUND 0x80070002u
#define DIERR_UNSUPPORTED 0x80004001u
#define DIDFT_AXIS 0x03
#define DIDFT_BUTTON 0x0C
#define DIDFT_POV 0x10
#define DIDFT_ANYINSTANCE 0xFFFF00
#define DIDEVTYPE_MOUSE 2
#define DIDEVTYPE_KEYBOARD 3
#define DIDEVTYPE_JOYSTICK 4

extern int32_t g_mouse_dx, g_mouse_dy, g_mouse_dz;
extern uint8_t g_mouse_buttons[4];
bool input_pop_key_event(uint8_t *dik, uint8_t *down, uint32_t *time, uint32_t *seq);

enum { DEV_KEYBOARD, DEV_MOUSE, DEV_JOY };
enum { OBJ_X, OBJ_Y, OBJ_Z, OBJ_RX, OBJ_RY, OBJ_RZ, OBJ_SLIDER0, OBJ_SLIDER1, OBJ_POV, OBJ_BUTTON, OBJ_NONE };

typedef struct { int kind, index; uint32_t ofs; bool dword; } Mapping;

typedef struct Joy {
    SDL_JoystickID id;
    SDL_Gamepad *pad;
    SDL_Joystick *joy;
    char name[128];
    bool rumble;
} Joy;

typedef struct Device {
    uint32_t obj, obj2;
    int type;
    Joy *joy;
    Mapping map[64];
    int nmap;
    uint32_t data_size;
    int32_t range_min[8], range_max[8];
    uint32_t deadzone[8];
    float ff_gain;
} Device;

static uint32_t vt_di, vt_dev, vt_dev2, vt_eff;
static uint32_t g_di_obj;
#define MAX_JOYS 4
static Joy g_joys[MAX_JOYS];
static int g_njoys;

static const uint8_t GUID_SysKeyboard[16] = {0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
static const uint8_t GUID_SysMouse[16] = {0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
/* GUID_XAxis..: A36D02E0-C9F3-11CF-BFC7-444553540000 with first dword varying */
static int axis_from_guid(const uint8_t *g)
{
    static const uint8_t tail[12] = {0xC9, 0xF3, 0x11, 0xCF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
    uint8_t t[12] = {g[6], g[7], g[4], g[5], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]};
    (void)tail;
    uint32_t d1 = (uint32_t)g[0] | ((uint32_t)g[1] << 8) | ((uint32_t)g[2] << 16) | ((uint32_t)g[3] << 24);
    if (memcmp(g + 4, "\xF3\xC9\xCF\x11\xBF\xC7\x44\x45\x53\x54\x00\x00", 12)) return -1;
    (void)t;
    switch (d1) {
    case 0xA36D02E0: return OBJ_X;
    case 0xA36D02E1: return OBJ_Y;
    case 0xA36D02E2: return OBJ_Z;
    case 0xA36D02F4: return OBJ_RX;
    case 0xA36D02F5: return OBJ_RY;
    case 0xA36D02E3: return OBJ_RZ;
    case 0xA36D02E4: return OBJ_SLIDER0;
    case 0xA36D02F0: return OBJ_BUTTON;
    case 0xA36D02F2: return OBJ_POV;
    default: return -1;
    }
}

static Device *dev_of(uint32_t o) { return (Device *)(uintptr_t)(((uint64_t)rd32(o + 8)) | ((uint64_t)rd32(o + 12) << 32)); }
static void dev_bind(uint32_t o, Device *d) { uint64_t p = (uint64_t)(uintptr_t)d; wr32(o + 8, (uint32_t)p); wr32(o + 12, (uint32_t)(p >> 32)); }

/* ----------------------------------------------------------- joysticks */
static void joys_refresh(void)
{
    int n = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&n);
    for (int i = 0; ids && i < n && g_njoys < MAX_JOYS; i++) {
        bool known = false;
        for (int k = 0; k < g_njoys; k++) known |= g_joys[k].id == ids[i];
        if (known) continue;
        Joy *j = &g_joys[g_njoys];
        memset(j, 0, sizeof *j);
        j->id = ids[i];
        if (SDL_IsGamepad(ids[i])) j->pad = SDL_OpenGamepad(ids[i]);
        if (!j->pad) j->joy = SDL_OpenJoystick(ids[i]);
        const char *nm = j->pad ? SDL_GetGamepadName(j->pad) : j->joy ? SDL_GetJoystickName(j->joy) : NULL;
        snprintf(j->name, sizeof j->name, "%s", nm ? nm : "Game Controller");
        SDL_PropertiesID props = j->pad ? SDL_GetGamepadProperties(j->pad) : j->joy ? SDL_GetJoystickProperties(j->joy) : 0;
        j->rumble = props && SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        rlog("controller %d: %s%s", g_njoys, j->name, j->rumble ? " (rumble)" : "");
        g_njoys++;
    }
    SDL_free(ids);
}

void joystick_handle_event(const SDL_Event *e) { (void)e; }

/* raw axis value in [-32768, 32767] */
static int32_t joy_axis(Joy *j, int which)
{
    if (j->pad) {
        int32_t lx = SDL_GetGamepadAxis(j->pad, SDL_GAMEPAD_AXIS_LEFTX);
        int32_t ly = SDL_GetGamepadAxis(j->pad, SDL_GAMEPAD_AXIS_LEFTY);
        int32_t lt = SDL_GetGamepadAxis(j->pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        int32_t rt = SDL_GetGamepadAxis(j->pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        switch (which) {
        case OBJ_X: return lx;
        case OBJ_Y: {   /* pedals on one axis: up = accelerate (right trigger), down = brake (left trigger) */
            int32_t v = ly + lt - rt;
            return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
        }
        case OBJ_Z: return rt * 2 - 32768;
        case OBJ_RZ: return lt * 2 - 32768;
        case OBJ_RX: return SDL_GetGamepadAxis(j->pad, SDL_GAMEPAD_AXIS_RIGHTX);
        case OBJ_RY: return SDL_GetGamepadAxis(j->pad, SDL_GAMEPAD_AXIS_RIGHTY);
        default: return 0;
        }
    }
    if (j->joy) {
        static const int axmap[] = {0, 1, 2, 3, 4, 5, 6, 7};
        if (which <= OBJ_SLIDER1 && axmap[which] < SDL_GetNumJoystickAxes(j->joy)) return SDL_GetJoystickAxis(j->joy, axmap[which]);
    }
    return 0;
}

static bool joy_button(Joy *j, int b)
{
    if (j->pad) {
        static const SDL_GamepadButton map[] = {
            SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
            SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_BACK,
            SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
        if (b < (int)(sizeof map / sizeof map[0])) return SDL_GetGamepadButton(j->pad, map[b]);
        return false;
    }
    return j->joy && b < SDL_GetNumJoystickButtons(j->joy) && SDL_GetJoystickButton(j->joy, b);
}

static uint32_t joy_pov(Joy *j)
{
    bool u, d, l, r;
    if (j->pad) {
        u = SDL_GetGamepadButton(j->pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
        d = SDL_GetGamepadButton(j->pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        l = SDL_GetGamepadButton(j->pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        r = SDL_GetGamepadButton(j->pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    } else if (j->joy && SDL_GetNumJoystickHats(j->joy) > 0) {
        uint8_t h = SDL_GetJoystickHat(j->joy, 0);
        u = h & SDL_HAT_UP; d = h & SDL_HAT_DOWN; l = h & SDL_HAT_LEFT; r = h & SDL_HAT_RIGHT;
    } else {
        return 0xFFFFFFFF;
    }
    if (u && r) return 4500; if (r && d) return 13500; if (d && l) return 22500; if (l && u) return 31500;
    if (u) return 0; if (r) return 9000; if (d) return 18000; if (l) return 27000;
    return 0xFFFFFFFF;
}

/* -------------------------------------------------------- IDirectInput */
static void com_AddRef(CPU *c) { uint32_t o = COM_THIS(); wr32(o + 4, rd32(o + 4) + 1); hle_return(c, rd32(o + 4), 1); }
static void com_Release(CPU *c) { uint32_t o = COM_THIS(); uint32_t r = rd32(o + 4); if (r) wr32(o + 4, --r); hle_return(c, r, 1); }

static void fill_joy_guid(uint32_t p, int idx, bool product)
{
    /* stable fake GUIDs: {4A4F5900+idx-...} */
    memset(g_mem + p, 0, 16);
    wr32(p, (product ? 0x50524400u : 0x4A4F5900u) + (uint32_t)idx);
    wr32(p + 4, 0x11D10000u);
    wr32(p + 8, 0x0000F0BAu);
    wr32(p + 12, 0x12345678u);
}

static int joy_index_from_guid(const uint8_t *g)
{
    uint32_t d = (uint32_t)g[0] | ((uint32_t)g[1] << 8) | ((uint32_t)g[2] << 16) | ((uint32_t)g[3] << 24);
    if ((d & 0xFFFFFF00u) == 0x4A4F5900u && (int)(d & 0xFF) < g_njoys) return (int)(d & 0xFF);
    return -1;
}

static void fill_devinst(uint32_t p, int type, int joy)
{
    uint32_t size = 580;
    memset(g_mem + p, 0, size);
    wr32(p, size);
    if (type == DEV_JOY) {
        Joy *j = &g_joys[joy];
        fill_joy_guid(p + 4, joy, false);
        fill_joy_guid(p + 20, joy, true);
        wr32(p + 36, DIDEVTYPE_JOYSTICK | ((j->pad ? 5u : 2u) << 8) | 0x10000u);
        snprintf((char *)g_mem + p + 40, 260, "%s", j->name);
        snprintf((char *)g_mem + p + 300, 260, "%s", j->name);
        if (j->rumble) { wr32(p + 560, 0x4646u); }
    } else if (type == DEV_KEYBOARD) {
        memcpy(g_mem + p + 4, GUID_SysKeyboard, 16);
        memcpy(g_mem + p + 20, GUID_SysKeyboard, 16);
        wr32(p + 36, DIDEVTYPE_KEYBOARD | (4u << 8));
        snprintf((char *)g_mem + p + 40, 260, "Keyboard");
        snprintf((char *)g_mem + p + 300, 260, "Keyboard");
    } else {
        memcpy(g_mem + p + 4, GUID_SysMouse, 16);
        memcpy(g_mem + p + 20, GUID_SysMouse, 16);
        wr32(p + 36, DIDEVTYPE_MOUSE | (2u << 8));
        snprintf((char *)g_mem + p + 40, 260, "Mouse");
        snprintf((char *)g_mem + p + 300, 260, "Mouse");
    }
}

static void di_EnumDevices(CPU *c)
{
    uint32_t type = ARG(1), cb = ARG(2), ref = ARG(3);
    hle_return(c, DI_OK, 5);
    joys_refresh();
    uint32_t inst = garena_alloc(580);
    if (type == 0 || type == DIDEVTYPE_KEYBOARD) {
        fill_devinst(inst, DEV_KEYBOARD, 0);
        if (!guest_call(c, cb, 2, inst, ref)) goto done;
    }
    if (type == 0 || type == DIDEVTYPE_MOUSE) {
        fill_devinst(inst, DEV_MOUSE, 0);
        if (!guest_call(c, cb, 2, inst, ref)) goto done;
    }
    if (type == 0 || type == DIDEVTYPE_JOYSTICK) {
        for (int i = 0; i < g_njoys; i++) {
            fill_devinst(inst, DEV_JOY, i);
            if (!guest_call(c, cb, 2, inst, ref)) break;
        }
    }
done:
    c->eax = DI_OK;
}

static void di_CreateDevice(CPU *c)
{
    const uint8_t *g = g_mem + ARG(1);
    uint32_t out = ARG(2);
    Device *d = calloc(1, sizeof *d);
    if (!memcmp(g, GUID_SysKeyboard, 16)) d->type = DEV_KEYBOARD;
    else if (!memcmp(g, GUID_SysMouse, 16)) d->type = DEV_MOUSE;
    else {
        int j = joy_index_from_guid(g);
        if (j < 0) { free(d); wr32(out, 0); hle_return(c, DIERR_NOTFOUND, 4); return; }
        d->type = DEV_JOY;
        d->joy = &g_joys[j];
    }
    for (int i = 0; i < 8; i++) { d->range_min[i] = 0; d->range_max[i] = 65535; }
    d->ff_gain = 1.0f;
    d->obj = com_object(vt_dev, 16);
    dev_bind(d->obj, d);
    d->obj2 = com_object(vt_dev2, 16);
    dev_bind(d->obj2, d);
    wr32(out, d->obj);
    rlog("DirectInput CreateDevice type=%d", d->type);
    hle_return(c, DI_OK, 4);
}

static void di_GetDeviceStatus(CPU *c) { hle_return(c, DI_OK, 2); }
static void di_RunControlPanel(CPU *c) { hle_return(c, DI_OK, 3); }

static const ComMethod m_di[] = {
    {"QueryInterface", 3, NULL}, {"AddRef", 1, com_AddRef}, {"Release", 1, com_Release},
    {"CreateDevice", 4, di_CreateDevice}, {"EnumDevices", 5, di_EnumDevices},
    {"GetDeviceStatus", 2, di_GetDeviceStatus}, {"RunControlPanel", 3, di_RunControlPanel}, {"Initialize", 3, NULL},
};

/* ------------------------------------------------- IDirectInputDevice(2) */
static void dev_QueryInterface(CPU *c)
{
    Device *d = dev_of(COM_THIS());
    uint32_t out = ARG(2);
    /* IDirectInputDevice2A 5944E682-C92E-11CF-BFC7-444553540000 (and anything else device-like) */
    wr32(out, d->obj2);
    hle_return(c, DI_OK, 3);
}

static void dev_GetCapabilities(CPU *c)
{
    Device *d = dev_of(COM_THIS());
    uint32_t p = ARG(1);
    uint32_t size = rd32(p);
    if (size < 24 || size > 64) size = 44;
    memset(g_mem + p + 4, 0, size - 4);
    wr32(p + 4, 1 | (d->type == DEV_JOY && d->joy->rumble ? 0x100u : 0));
    if (d->type == DEV_JOY) {
        wr32(p + 8, DIDEVTYPE_JOYSTICK | ((d->joy->pad ? 5u : 2u) << 8) | 0x10000u);
        wr32(p + 12, d->joy->pad ? 6 : (uint32_t)SDL_GetNumJoystickAxes(d->joy->joy));
        wr32(p + 16, d->joy->pad ? 10 : (uint32_t)SDL_GetNumJoystickButtons(d->joy->joy));
        wr32(p + 20, 1);
    } else if (d->type == DEV_KEYBOARD) {
        wr32(p + 8, DIDEVTYPE_KEYBOARD | (4u << 8));
        wr32(p + 16, 128);
    } else {
        wr32(p + 8, DIDEVTYPE_MOUSE | (2u << 8));
        wr32(p + 12, 3);
        wr32(p + 16, 3);
    }
    hle_return(c, DI_OK, 2);
}

static void dev_SetDataFormat(CPU *c)
{
    Device *d = dev_of(COM_THIS());
    uint32_t f = ARG(1);
    uint32_t objsize = rd32(f + 4), datasize = rd32(f + 12), n = rd32(f + 16), arr = rd32(f + 20);
    d->data_size = datasize;
    d->nmap = 0;
    int next_axis = 0, next_button = 0;
    for (uint32_t i = 0; i < n && d->nmap < 64; i++) {
        uint32_t o = arr + i * objsize;
        uint32_t pg = rd32(o), ofs = rd32(o + 4), type = rd32(o + 8);
        int kind = pg ? axis_from_guid(g_mem + pg) : -1;
        int inst = (type & DIDFT_ANYINSTANCE) == DIDFT_ANYINSTANCE ? -1 : (int)((type >> 8) & 0xFFFF);
        Mapping *m = &d->map[d->nmap];
        m->ofs = ofs;
        if (kind == OBJ_BUTTON || (kind < 0 && (type & DIDFT_BUTTON))) {
            m->kind = OBJ_BUTTON;
            m->index = inst >= 0 ? inst : next_button;
            next_button = m->index + 1;
        } else if (kind == OBJ_POV || (kind < 0 && (type & DIDFT_POV))) {
            m->kind = OBJ_POV;
            m->index = 0;
            m->dword = true;
        } else if (kind >= 0 || (type & DIDFT_AXIS)) {
            m->kind = kind >= 0 ? kind : (next_axis < 8 ? next_axis : OBJ_NONE);
            if (kind == OBJ_SLIDER0 && next_axis > OBJ_SLIDER0) m->kind = OBJ_SLIDER1;
            m->index = m->kind;
            next_axis = m->kind + 1;
            m->dword = true;
        } else {
            m->kind = OBJ_NONE;
        }
        d->nmap++;
    }
    rlog("SetDataFormat type=%d size=%u objs=%u", d->type, datasize, n);
    hle_return(c, DI_OK, 2);
}

static int32_t scale_axis(Device *d, int axis, int32_t raw)
{
    int a = axis < 8 ? axis : 0;
    int32_t dz = (int32_t)(d->deadzone[a] * 32768 / 10000);
    if (raw > -dz && raw < dz) raw = 0;
    int64_t lo = d->range_min[a], hi = d->range_max[a];
    return (int32_t)(lo + ((int64_t)raw + 32768) * (hi - lo) / 65535);
}

static void dev_GetDeviceState(CPU *c)
{
    Device *d = dev_of(COM_THIS());
    uint32_t n = ARG(1), p = ARG(2);
    memset(g_mem + p, 0, n);
    if (d->type == DEV_KEYBOARD) {
        SDL_LockMutex(g_input_lock);
        memcpy(g_mem + p, g_dik_state, n < 256 ? n : 256);
        SDL_UnlockMutex(g_input_lock);
        if (g_cfg.trace) {
            static uint64_t calls;
            calls++;
            for (int k = 0; k < 256; k++)
                if (g_dik_state[k]) { rlog("GetDeviceState(kbd) #%llu key %02x down (thread %x)", (unsigned long long)calls, k, thread_current()->tid); break; }
            if ((calls & 1023) == 1) rlog("GetDeviceState(kbd) calls=%llu", (unsigned long long)calls);
        }
    } else if (d->type == DEV_MOUSE) {
        SDL_LockMutex(g_input_lock);
        if (n >= 12) { wr32(p, (uint32_t)g_mouse_dx); wr32(p + 4, (uint32_t)g_mouse_dy); wr32(p + 8, (uint32_t)g_mouse_dz); }
        for (int i = 0; i < 4 && 12 + i < (int)n; i++) wr8(p + 12 + (uint32_t)i, g_mouse_buttons[i]);
        g_mouse_dx = g_mouse_dy = g_mouse_dz = 0;
        SDL_UnlockMutex(g_input_lock);
    } else {

        for (int i = 0; i < d->nmap; i++) {
            Mapping *m = &d->map[i];
            if (m->ofs >= n) continue;
            switch (m->kind) {
            case OBJ_BUTTON: wr8(p + m->ofs, joy_button(d->joy, m->index) ? 0x80 : 0); break;
            case OBJ_POV: wr32(p + m->ofs, joy_pov(d->joy)); break;
            case OBJ_NONE: break;
            default: wr32(p + m->ofs, (uint32_t)scale_axis(d, m->kind, joy_axis(d->joy, m->kind))); break;
            }
        }
    }
    hle_return(c, DI_OK, 3);
}

static void dev_GetDeviceData(CPU *c)
{
    Device *d = dev_of(COM_THIS());
    uint32_t objsz = ARG(1), buf = ARG(2), pin = ARG(3), flags = ARG(4);
    uint32_t max = rd32(pin), got = 0;
    if (d->type == DEV_KEYBOARD) {
        uint8_t dik, down;
        uint32_t time, seq;
        while (got < max && (flags & 1) == 0 && input_pop_key_event(&dik, &down, &time, &seq)) {
            if (buf) {
                uint32_t o = buf + got * objsz;
                wr32(o, dik);
                wr32(o + 4, down ? 0x80 : 0);
                wr32(o + 8, time);
                wr32(o + 12, seq);
            }
            got++;
        }
    }
    wr32(pin, got);
    hle_return(c, DI_OK, 5);
}

static void dev_SetProperty(CPU *c)
{
    Device *d = dev_of(COM_THIS());
    uint32_t prop = ARG(1), hdr = ARG(2);
    uint32_t how = rd32(hdr + 12), obj = rd32(hdr + 8);
    int axis = -1;
    if (how == 1) {   /* DIPH_BYOFFSET */
        for (int i = 0; i < d->nmap; i++)
            if (d->map[i].ofs == obj && d->map[i].kind < OBJ_POV) axis = d->map[i].kind;
    }
    switch (prop) {
    case 4:   /* DIPROP_RANGE */
        for (int a = 0; a < 8; a++)
            if (axis < 0 || axis == a) { d->range_min[a] = (int32_t)rd32(hdr + 16); d->range_max[a] = (int32_t)rd32(hdr + 20); }
        break;
    case 5:   /* DIPROP_DEADZONE */
        for (int a = 0; a < 8; a++)
            if (axis < 0 || axis == a) d->deadzone[a] = rd32(hdr + 16);
        break;
    case 7:   /* DIPROP_FFGAIN */
        d->ff_gain = (float)rd32(hdr + 16) / 10000.f;
        break;
    default:
        break;
    }
    hle_return(c, DI_OK, 3);
}

static void dev_GetProperty(CPU *c)
{
    Device *d = dev_of(COM_THIS());
    uint32_t prop = ARG(1), hdr = ARG(2);
    if (prop == 4) { wr32(hdr + 16, (uint32_t)d->range_min[0]); wr32(hdr + 20, (uint32_t)d->range_max[0]); }
    else wr32(hdr + 16, 0);
    hle_return(c, DI_OK, 3);
}

static void dev_Acquire(CPU *c) { hle_return(c, DI_OK, 1); }
static void dev_Unacquire(CPU *c) { hle_return(c, DI_OK, 1); }
static void dev_SetCooperativeLevel(CPU *c) { hle_return(c, DI_OK, 3); }
static void dev_SetEventNotification(CPU *c) { hle_return(c, DI_OK, 2); }
static void dev_Poll(CPU *c) { hle_return(c, DI_OK, 1); }

static void dev_GetDeviceInfo(CPU *c)
{
    Device *d = dev_of(COM_THIS());
    int idx = d->type == DEV_JOY ? (int)(d->joy - g_joys) : 0;
    fill_devinst(ARG(1), d->type, idx);
    hle_return(c, DI_OK, 2);
}

static void dev_EnumObjects(CPU *c)
{
    Device *d = dev_of(COM_THIS());
    uint32_t cb = ARG(1), ref = ARG(2), flags = ARG(3);
    hle_return(c, DI_OK, 4);
    if (d->type != DEV_JOY) { c->eax = DI_OK; return; }
    uint32_t inst = garena_alloc(316);
    static const uint32_t axis_guid[] = {0xA36D02E0, 0xA36D02E1, 0xA36D02E2, 0xA36D02F4, 0xA36D02F5, 0xA36D02E3};
    int naxes = d->joy->pad ? 6 : SDL_GetNumJoystickAxes(d->joy->joy);
    int nbtn = d->joy->pad ? 10 : SDL_GetNumJoystickButtons(d->joy->joy);
    if (flags == 0 || (flags & DIDFT_AXIS)) {
        for (int a = 0; a < naxes && a < 6; a++) {
            memset(g_mem + inst, 0, 316);
            wr32(inst, 316);
            wr32(inst + 4, axis_guid[a]);
            memcpy(g_mem + inst + 8, "\xF3\xC9\xCF\x11\xBF\xC7\x44\x45\x53\x54\x00\x00", 12);
            wr32(inst + 20, (uint32_t)a * 4);
            wr32(inst + 24, 0x02 | ((uint32_t)a << 8) | (a < 2 && d->joy->rumble ? 0x01000000u : 0));
            wr32(inst + 28, a < 2 && d->joy->rumble ? 1u : 0u);
            snprintf((char *)g_mem + inst + 32, 260, "Axis %d", a);
            if (!guest_call(c, cb, 2, inst, ref)) { c->eax = DI_OK; return; }
        }
    }
    if (flags == 0 || (flags & DIDFT_BUTTON)) {
        for (int b = 0; b < nbtn; b++) {
            memset(g_mem + inst, 0, 316);
            wr32(inst, 316);
            wr32(inst + 4, 0xA36D02F0);
            memcpy(g_mem + inst + 8, "\xF3\xC9\xCF\x11\xBF\xC7\x44\x45\x53\x54\x00\x00", 12);
            wr32(inst + 20, 48 + (uint32_t)b);
            wr32(inst + 24, 0x0C | ((uint32_t)b << 8));
            snprintf((char *)g_mem + inst + 32, 260, "Button %d", b);
            if (!guest_call(c, cb, 2, inst, ref)) { c->eax = DI_OK; return; }
        }
    }
    c->eax = DI_OK;
}

/* ---- force feedback: effects become rumble on gamepads ---- */
typedef struct { Device *dev; int kind; float magnitude; bool playing; } Effect;
static Effect *eff_of(uint32_t o) { return (Effect *)(uintptr_t)(((uint64_t)rd32(o + 8)) | ((uint64_t)rd32(o + 12) << 32)); }

static void eff_apply(Effect *e)
{
    Joy *j = e->dev->joy;
    if (!j || !j->rumble || !j->pad) return;
    float m = e->playing ? e->magnitude * e->dev->ff_gain : 0.f;
    if (m > 1.f) m = 1.f;
    uint16_t v = (uint16_t)(m * 0xFFFF);
    SDL_RumbleGamepad(j->pad, v, (uint16_t)(v / 2), e->playing ? 1000 : 0);
}

static void eff_read_params(Effect *e, uint32_t p)
{
    /* DIEFFECT: dwSize, dwFlags, dwDuration, dwSamplePeriod, dwGain, dwTriggerButton, dwTriggerRepeatInterval,
       cAxes, rgdwAxes, rglDirection, lpEnvelope, cbTypeSpecificParams, lpvTypeSpecificParams */
    if (!p) return;
    uint32_t gain = rd32(p + 16), tsp = rd32(p + 48), tspsz = rd32(p + 44);
    float mag = 0.f;
    if (tsp && tspsz >= 4) {
        if (e->kind == 0) mag = SDL_fabsf((float)(int32_t)rd32(tsp)) / 10000.f;                 /* DICONSTANTFORCE */
        else if (tspsz >= 24) mag = (SDL_fabsf((float)(int32_t)rd32(tsp + 4)) + SDL_fabsf((float)(int32_t)rd32(tsp + 8))) / 40000.f; /* DICONDITION */
    }
    e->magnitude = mag * (gain ? (float)gain / 10000.f : 1.f);
}

static void eff_SetParameters(CPU *c) { Effect *e = eff_of(COM_THIS()); eff_read_params(e, ARG(1)); eff_apply(e); hle_return(c, DI_OK, 3); }
static void eff_Start(CPU *c) { Effect *e = eff_of(COM_THIS()); e->playing = true; eff_apply(e); hle_return(c, DI_OK, 3); }
static void eff_Stop(CPU *c) { Effect *e = eff_of(COM_THIS()); e->playing = false; eff_apply(e); hle_return(c, DI_OK, 1); }
static void eff_Download(CPU *c) { hle_return(c, DI_OK, 1); }
static void eff_Unload(CPU *c) { hle_return(c, DI_OK, 1); }
static void eff_GetEffectStatus(CPU *c) { wr32(ARG(1), eff_of(COM_THIS())->playing ? 1 : 0); hle_return(c, DI_OK, 2); }

static const ComMethod m_eff[] = {
    {"QueryInterface", 3, NULL}, {"AddRef", 1, com_AddRef}, {"Release", 1, com_Release},
    {"Initialize", 4, NULL}, {"GetEffectGuid", 2, NULL}, {"GetParameters", 3, NULL},
    {"SetParameters", 3, eff_SetParameters}, {"Start", 3, eff_Start}, {"Stop", 1, eff_Stop},
    {"GetEffectStatus", 2, eff_GetEffectStatus}, {"Download", 1, eff_Download}, {"Unload", 1, eff_Unload},
    {"Escape", 2, NULL},
};

static void dev_CreateEffect(CPU *c)
{
    Device *d = dev_of(COM_THIS());
    const uint8_t *g = g_mem + ARG(1);
    uint32_t params = ARG(2), out = ARG(3);
    Effect *e = calloc(1, sizeof *e);
    e->dev = d;
    uint32_t d1 = (uint32_t)g[0] | ((uint32_t)g[1] << 8) | ((uint32_t)g[2] << 16) | ((uint32_t)g[3] << 24);
    e->kind = d1 == 0x13541C20u ? 0 : 1;
    eff_read_params(e, params);
    uint32_t o = com_object(vt_eff, 16);
    uint64_t pp = (uint64_t)(uintptr_t)e;
    wr32(o + 8, (uint32_t)pp);
    wr32(o + 12, (uint32_t)(pp >> 32));
    wr32(out, o);
    hle_return(c, DI_OK, 5);
}

static void dev_EnumEffects(CPU *c)
{
    Device *d = dev_of(COM_THIS());
    uint32_t cb = ARG(1), ref = ARG(2);
    hle_return(c, DI_OK, 4);
    if (d->type != DEV_JOY || !d->joy->rumble) { c->eax = DI_OK; return; }
    /* DIEFFECTINFOA: dwSize, guid, dwEffType, dwStaticParams, dwDynamicParams, tszName[260] */
    static const uint32_t guids[][4] = {
        {0x13541C20, 0x11D08E33, 0xA0009AD0, 0x356EA0C9},   /* ConstantForce */
        {0x13541C27, 0x11D08E33, 0xA0009AD0, 0x356EA0C9},   /* Spring */
        {0x13541C28, 0x11D08E33, 0xA0009AD0, 0x356EA0C9},   /* Damper */
    };
    uint32_t info = garena_alloc(300);
    for (int i = 0; i < 3; i++) {
        memset(g_mem + info, 0, 300);
        wr32(info, 300);
        for (int k = 0; k < 4; k++) wr32(info + 4 + 4u * (uint32_t)k, guids[i][k]);
        wr32(info + 20, i == 0 ? 5u : 4u);
        wr32(info + 24, 0x3FF);
        wr32(info + 28, 0x3FF);
        snprintf((char *)g_mem + info + 32, 260, "%s", i == 0 ? "Constant Force" : i == 1 ? "Spring" : "Damper");
        if (!guest_call(c, cb, 2, info, ref)) break;
    }
    c->eax = DI_OK;
}

static void dev_GetForceFeedbackState(CPU *c) { wr32(ARG(1), 0x200); hle_return(c, DI_OK, 2); }
static void dev_SendForceFeedbackCommand(CPU *c) { hle_return(c, DI_OK, 2); }

#define DEV_METHODS                                                                                  \
    {"QueryInterface", 3, dev_QueryInterface}, {"AddRef", 1, com_AddRef}, {"Release", 1, com_Release}, \
    {"GetCapabilities", 2, dev_GetCapabilities}, {"EnumObjects", 4, dev_EnumObjects},                 \
    {"GetProperty", 3, dev_GetProperty}, {"SetProperty", 3, dev_SetProperty},                         \
    {"Acquire", 1, dev_Acquire}, {"Unacquire", 1, dev_Unacquire},                                     \
    {"GetDeviceState", 3, dev_GetDeviceState}, {"GetDeviceData", 5, dev_GetDeviceData},               \
    {"SetDataFormat", 2, dev_SetDataFormat}, {"SetEventNotification", 2, dev_SetEventNotification},   \
    {"SetCooperativeLevel", 3, dev_SetCooperativeLevel}, {"GetObjectInfo", 4, NULL},                  \
    {"GetDeviceInfo", 2, dev_GetDeviceInfo}, {"RunControlPanel", 3, NULL}, {"Initialize", 4, NULL}

static const ComMethod m_dev[] = {DEV_METHODS};
static const ComMethod m_dev2[] = {DEV_METHODS,
    {"CreateEffect", 5, dev_CreateEffect}, {"EnumEffects", 4, dev_EnumEffects}, {"GetEffectInfo", 3, NULL},
    {"GetForceFeedbackState", 2, dev_GetForceFeedbackState}, {"SendForceFeedbackCommand", 2, dev_SendForceFeedbackCommand},
    {"EnumCreatedEffectObjects", 4, NULL}, {"Escape", 2, NULL}, {"Poll", 1, dev_Poll}, {"SendDeviceData", 5, NULL}};

static void dinput_DirectInputCreateA(CPU *c)
{
    if (!vt_di) {
        vt_di = com_vtable("IDirectInputA", m_di, (int)(sizeof m_di / sizeof m_di[0]));
        vt_dev = com_vtable("IDirectInputDeviceA", m_dev, (int)(sizeof m_dev / sizeof m_dev[0]));
        vt_dev2 = com_vtable("IDirectInputDevice2A", m_dev2, (int)(sizeof m_dev2 / sizeof m_dev2[0]));
        vt_eff = com_vtable("IDirectInputEffect", m_eff, (int)(sizeof m_eff / sizeof m_eff[0]));
        g_di_obj = com_object(vt_di, 16);
        joys_refresh();
    }
    wr32(ARG(2), g_di_obj);
    hle_return(c, DI_OK, 4);
}

const HleDef hle_dinput[] = {
    {"DINPUT", "DirectInputCreateA", dinput_DirectInputCreateA},
    {0, 0, 0},
};
