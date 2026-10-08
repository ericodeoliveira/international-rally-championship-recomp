/* DirectSound (DX5). The game writes its own mix straight into the primary buffer
 * (DSSCL_WRITEPRIMARY) and paces itself with GetCurrentPosition. The primary buffer
 * lives in guest memory and an SDL audio stream consumes it in real time, so the play
 * cursor follows what the sound card actually played. Secondary buffers are mixed in too. */
#include <stdlib.h>
#include "runtime.h"

#define DS_OK 0
#define DSBCAPS_PRIMARYBUFFER 0x1
#define DSBPLAY_LOOPING 0x1
#define PRIMARY_BYTES 0x8000

typedef struct SBuf {
    uint32_t obj;
    bool primary;
    uint32_t mem, size;
    int rate, channels, bits;
    volatile bool playing, looping;
    double pos;                 /* byte position (secondary: fractional for resampling) */
    int32_t volume, pan;        /* hundredths of dB */
    uint32_t freq;
    struct SBuf *next;
} SBuf;

static uint32_t vt_ds, vt_dsb;
static uint32_t g_ds_obj;
static SBuf *g_primary_buf;
static SBuf *g_bufs;
static SDL_Mutex *g_ds_lock;
static SDL_AudioStream *g_stream;
static int g_out_rate = 22050;

static SBuf *sbuf(uint32_t o) { return (SBuf *)(uintptr_t)(((uint64_t)rd32(o + 8)) | ((uint64_t)rd32(o + 12) << 32)); }
static void sbuf_bind(uint32_t o, SBuf *b) { uint64_t p = (uint64_t)(uintptr_t)b; wr32(o + 8, (uint32_t)p); wr32(o + 12, (uint32_t)(p >> 32)); }

static float db_to_gain(int32_t hdb) { return hdb <= -10000 ? 0.f : SDL_powf(10.f, (float)hdb / 2000.f); }

/* SDL pulls output here: primary buffer bytes in order, secondary buffers mixed on top */
static void SDLCALL audio_cb(void *ud, SDL_AudioStream *stream, int additional, int total)
{
    (void)ud; (void)total;
    if (additional <= 0) return;
    if (g_cfg.trace) {
        static int n;
        if (n++ < 12) rlog("dsound callback: %d bytes requested (queued total %d)", additional, total);
    }
    int frames = additional / 4;
    int16_t *out = SDL_malloc((size_t)frames * 4);
    SDL_LockMutex(g_ds_lock);
    SBuf *p = g_primary_buf;
    for (int i = 0; i < frames; i++) {
        int32_t l = 0, r = 0;
        if (p && p->playing && p->mem) {
            uint32_t pos = (uint32_t)p->pos;
            if (p->bits == 16 && p->channels == 2) {
                l = (int16_t)rd16(p->mem + pos);
                r = (int16_t)rd16(p->mem + pos + 2);
                pos += 4;
            } else if (p->bits == 16) {
                l = r = (int16_t)rd16(p->mem + pos);
                pos += 2;
            } else if (p->channels == 2) {
                l = ((int)rd8(p->mem + pos) - 128) << 8;
                r = ((int)rd8(p->mem + pos + 1) - 128) << 8;
                pos += 2;
            } else {
                l = r = ((int)rd8(p->mem + pos) - 128) << 8;
                pos += 1;
            }
            if (pos >= p->size) pos = 0;
            p->pos = pos;
        }
        for (SBuf *b = g_bufs; b; b = b->next) {
            if (b->primary || !b->playing || !b->mem || !b->size) continue;
            int bps = b->bits / 8 * b->channels;
            uint32_t pos = (uint32_t)b->pos;
            pos -= pos % (uint32_t)bps;
            int32_t sl, sr;
            if (b->bits == 16) {
                sl = (int16_t)rd16(b->mem + pos);
                sr = b->channels == 2 ? (int16_t)rd16(b->mem + pos + 2) : sl;
            } else {
                sl = ((int)rd8(b->mem + pos) - 128) << 8;
                sr = b->channels == 2 ? ((int)rd8(b->mem + pos + 1) - 128) << 8 : sl;
            }
            float g = db_to_gain(b->volume);
            float gl = g * (b->pan > 0 ? db_to_gain(-b->pan) : 1.f);
            float gr = g * (b->pan < 0 ? db_to_gain(b->pan) : 1.f);
            l += (int32_t)(sl * gl);
            r += (int32_t)(sr * gr);
            double step = (double)(b->freq ? b->freq : (uint32_t)b->rate) / g_out_rate * bps;
            b->pos += step;
            if (b->pos >= b->size) {
                if (b->looping) b->pos -= b->size;
                else { b->pos = 0; b->playing = false; }
            }
        }
        out[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    SDL_UnlockMutex(g_ds_lock);
    SDL_PutAudioStreamData(stream, out, frames * 4);
    wav_record("effects", out, frames * 4, g_out_rate);
    SDL_free(out);
}

static void open_stream(int rate)
{
    if (g_stream && rate == g_out_rate) return;
    if (g_stream) SDL_DestroyAudioStream(g_stream);
    g_out_rate = rate;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, rate};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, NULL);
    if (!g_stream) { rlog("audio: %s", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(g_stream);
    rlog("audio stream opened at %d Hz", rate);
}

/* ---------------------------------------------------------- IDirectSound */
static void com_AddRef(CPU *c) { uint32_t o = COM_THIS(); wr32(o + 4, rd32(o + 4) + 1); hle_return(c, rd32(o + 4), 1); }
static void com_Release(CPU *c) { uint32_t o = COM_THIS(); uint32_t r = rd32(o + 4); if (r) wr32(o + 4, --r); hle_return(c, r, 1); }

static void ds_CreateSoundBuffer(CPU *c)
{
    uint32_t desc = ARG(1), out = ARG(2);
    uint32_t flags = rd32(desc + 4), bytes = rd32(desc + 8), wfx = rd32(desc + 16);
    SBuf *b = calloc(1, sizeof *b);
    b->primary = (flags & DSBCAPS_PRIMARYBUFFER) != 0;
    b->rate = 22050; b->channels = 2; b->bits = 16;
    if (wfx) {
        b->channels = (int)rd16(wfx + 2);
        b->rate = (int)rd32(wfx + 4);
        b->bits = (int)rd16(wfx + 14);
    }
    b->size = b->primary ? PRIMARY_BYTES : bytes;
    b->mem = gmem_alloc(b->size + 16);
    if (b->primary && b->bits == 8) memset(g_mem + b->mem, 0x80, b->size);
    b->obj = com_object(vt_dsb, 16);
    sbuf_bind(b->obj, b);
    SDL_LockMutex(g_ds_lock);
    b->next = g_bufs;
    g_bufs = b;
    if (b->primary) g_primary_buf = b;
    SDL_UnlockMutex(g_ds_lock);
    rlog("CreateSoundBuffer flags=%x bytes=%u primary=%d", flags, b->size, b->primary);
    wr32(out, b->obj);
    hle_return(c, DS_OK, 4);
}

static void ds_GetCaps(CPU *c) { uint32_t p = ARG(1); uint32_t n = rd32(p); memset(g_mem + p + 4, 0, n > 4 && n < 512 ? n - 4 : 0); wr32(p + 4, 0xF0F); hle_return(c, DS_OK, 2); }
static void ds_SetCooperativeLevel(CPU *c) { hle_return(c, DS_OK, 3); }
static void ds_Compact(CPU *c) { hle_return(c, DS_OK, 1); }
static void ds_GetSpeakerConfig(CPU *c) { wr32(ARG(1), 4); hle_return(c, DS_OK, 2); }
static void ds_SetSpeakerConfig(CPU *c) { hle_return(c, DS_OK, 2); }

static const ComMethod m_ds[] = {
    {"QueryInterface", 3, NULL}, {"AddRef", 1, com_AddRef}, {"Release", 1, com_Release},
    {"CreateSoundBuffer", 4, ds_CreateSoundBuffer}, {"GetCaps", 2, ds_GetCaps}, {"DuplicateSoundBuffer", 3, NULL},
    {"SetCooperativeLevel", 3, ds_SetCooperativeLevel}, {"Compact", 1, ds_Compact},
    {"GetSpeakerConfig", 2, ds_GetSpeakerConfig}, {"SetSpeakerConfig", 2, ds_SetSpeakerConfig}, {"Initialize", 2, NULL},
};

/* ---------------------------------------------------- IDirectSoundBuffer */
static void dsb_GetCaps(CPU *c)
{
    SBuf *b = sbuf(COM_THIS());
    uint32_t p = ARG(1);
    wr32(p + 4, b->primary ? DSBCAPS_PRIMARYBUFFER : 0);
    wr32(p + 8, b->size);
    wr32(p + 12, 0);
    wr32(p + 16, 0);
    hle_return(c, DS_OK, 2);
}

static void dsb_GetCurrentPosition(CPU *c)
{
    SBuf *b = sbuf(COM_THIS());
    SDL_LockMutex(g_ds_lock);
    uint32_t play = (uint32_t)b->pos;
    SDL_UnlockMutex(g_ds_lock);
    int bps = b->bits / 8 * b->channels;
    play -= play % (uint32_t)bps;
    uint32_t write = (play + (uint32_t)bps * 256) % b->size;
    if (ARG(1)) wr32(ARG(1), play);
    if (ARG(2)) wr32(ARG(2), write);
    hle_return(c, DS_OK, 3);
}

static void dsb_GetFormat(CPU *c)
{
    SBuf *b = sbuf(COM_THIS());
    uint32_t p = ARG(1);
    if (p) {
        wr16(p, 1);
        wr16(p + 2, (uint32_t)b->channels);
        wr32(p + 4, (uint32_t)b->rate);
        wr32(p + 8, (uint32_t)(b->rate * b->channels * b->bits / 8));
        wr16(p + 12, (uint32_t)(b->channels * b->bits / 8));
        wr16(p + 14, (uint32_t)b->bits);
    }
    if (ARG(3)) wr32(ARG(3), 16);
    hle_return(c, DS_OK, 4);
}

static void dsb_SetFormat(CPU *c)
{
    SBuf *b = sbuf(COM_THIS());
    uint32_t w = ARG(1);
    SDL_LockMutex(g_ds_lock);
    b->channels = (int)rd16(w + 2);
    b->rate = (int)rd32(w + 4);
    b->bits = (int)rd16(w + 14);
    b->pos = 0;
    SDL_UnlockMutex(g_ds_lock);
    rlog("primary format %d Hz %d bit %d ch", b->rate, b->bits, b->channels);
    if (b->primary) open_stream(b->rate);
    hle_return(c, DS_OK, 2);
}

static void dsb_Lock(CPU *c)
{
    SBuf *b = sbuf(COM_THIS());
    uint32_t off = ARG(1), bytes = ARG(2), p1 = ARG(3), l1 = ARG(4), p2 = ARG(5), l2 = ARG(6), flags = ARG(7);
    if (flags & 2) { off = 0; bytes = b->size; }
    if (flags & 1) { uint32_t play = (uint32_t)b->pos; off = (play + 1024) % b->size; }
    off %= b->size;
    if (bytes > b->size) bytes = b->size;
    uint32_t first = bytes <= b->size - off ? bytes : b->size - off;
    wr32(p1, b->mem + off);
    wr32(l1, first);
    if (p2) wr32(p2, first < bytes ? b->mem : 0);
    if (l2) wr32(l2, bytes - first);
    hle_return(c, DS_OK, 8);
}

static void dsb_Unlock(CPU *c) { hle_return(c, DS_OK, 5); }

static void dsb_Play(CPU *c)
{
    SBuf *b = sbuf(COM_THIS());
    if (!g_stream) open_stream(b->primary ? b->rate : 22050);
    b->looping = (ARG(3) & DSBPLAY_LOOPING) != 0 || b->primary;
    b->playing = true;
    hle_return(c, DS_OK, 4);
}

static void dsb_Stop(CPU *c) { sbuf(COM_THIS())->playing = false; hle_return(c, DS_OK, 1); }
static void dsb_SetCurrentPosition(CPU *c) { SBuf *b = sbuf(COM_THIS()); SDL_LockMutex(g_ds_lock); b->pos = ARG(1) % b->size; SDL_UnlockMutex(g_ds_lock); hle_return(c, DS_OK, 2); }
static void dsb_GetStatus(CPU *c) { SBuf *b = sbuf(COM_THIS()); wr32(ARG(1), (b->playing ? 1 : 0) | (b->looping ? 4 : 0)); hle_return(c, DS_OK, 2); }
static void dsb_SetVolume(CPU *c) { sbuf(COM_THIS())->volume = (int32_t)ARG(1); hle_return(c, DS_OK, 2); }
static void dsb_GetVolume(CPU *c) { wr32(ARG(1), (uint32_t)sbuf(COM_THIS())->volume); hle_return(c, DS_OK, 2); }
static void dsb_SetPan(CPU *c) { sbuf(COM_THIS())->pan = (int32_t)ARG(1); hle_return(c, DS_OK, 2); }
static void dsb_GetPan(CPU *c) { wr32(ARG(1), (uint32_t)sbuf(COM_THIS())->pan); hle_return(c, DS_OK, 2); }
static void dsb_SetFrequency(CPU *c) { sbuf(COM_THIS())->freq = ARG(1); hle_return(c, DS_OK, 2); }
static void dsb_GetFrequency(CPU *c) { SBuf *b = sbuf(COM_THIS()); wr32(ARG(1), b->freq ? b->freq : (uint32_t)b->rate); hle_return(c, DS_OK, 2); }
static void dsb_Restore(CPU *c) { hle_return(c, DS_OK, 1); }

static const ComMethod m_dsb[] = {
    {"QueryInterface", 3, NULL}, {"AddRef", 1, com_AddRef}, {"Release", 1, com_Release},
    {"GetCaps", 2, dsb_GetCaps}, {"GetCurrentPosition", 3, dsb_GetCurrentPosition}, {"GetFormat", 4, dsb_GetFormat},
    {"GetVolume", 2, dsb_GetVolume}, {"GetPan", 2, dsb_GetPan}, {"GetFrequency", 2, dsb_GetFrequency},
    {"GetStatus", 2, dsb_GetStatus}, {"Initialize", 3, NULL}, {"Lock", 8, dsb_Lock}, {"Play", 4, dsb_Play},
    {"SetCurrentPosition", 2, dsb_SetCurrentPosition}, {"SetFormat", 2, dsb_SetFormat},
    {"SetVolume", 2, dsb_SetVolume}, {"SetPan", 2, dsb_SetPan}, {"SetFrequency", 2, dsb_SetFrequency},
    {"Stop", 1, dsb_Stop}, {"Unlock", 5, dsb_Unlock}, {"Restore", 1, dsb_Restore},
};

static void dsound_DirectSoundCreate(CPU *c)
{
    if (!vt_ds) {
        g_ds_lock = SDL_CreateMutex();
        vt_ds = com_vtable("IDirectSound", m_ds, (int)(sizeof m_ds / sizeof m_ds[0]));
        vt_dsb = com_vtable("IDirectSoundBuffer", m_dsb, (int)(sizeof m_dsb / sizeof m_dsb[0]));
        g_ds_obj = com_object(vt_ds, 16);
    }
    wr32(ARG(1), g_ds_obj);
    hle_return(c, DS_OK, 3);
}

const HleDef hle_dsound[] = {
    {"DSOUND", "DirectSoundCreate", dsound_DirectSoundCreate},
    {0, 0, 0},
};
