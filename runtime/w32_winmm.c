/* WINMM: MCI "cdaudio" device and aux volume, backed by the extracted CD tracks
 * (music/trackNN.flac, lossless, or trackNN.wav; 44.1 kHz 16-bit stereo, CD frames of 2352 bytes). */
#include <stdlib.h>
#include "runtime.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#include "dr_flac.h"

#define MCI_OPEN 0x0803
#define MCI_CLOSE 0x0804
#define MCI_PLAY 0x0806
#define MCI_SEEK 0x0807
#define MCI_STOP 0x0808
#define MCI_PAUSE 0x0809
#define MCI_SET 0x080D
#define MCI_STATUS 0x0814
#define MCI_RESUME 0x0855

#define MCI_FROM 0x4
#define MCI_TO 0x8
#define MCI_TRACK 0x10
#define MCI_STATUS_ITEM 0x100
#define MCI_SET_TIME_FORMAT 0x400

#define MCI_STATUS_LENGTH 1
#define MCI_STATUS_POSITION 2
#define MCI_STATUS_NUMBER_OF_TRACKS 3
#define MCI_STATUS_MODE 4
#define MCI_STATUS_MEDIA_PRESENT 5
#define MCI_STATUS_TIME_FORMAT 6
#define MCI_STATUS_READY 7
#define MCI_CDA_STATUS_TYPE_TRACK 0x4001

#define MCI_FORMAT_MSF 2
#define MCI_FORMAT_TMSF 10
#define MCI_MODE_STOP 525
#define MCI_MODE_PLAY 526
#define CD_DEVICE_ID 0x4C

#define MAX_TRACKS 99
typedef struct { bool audio, flac; uint32_t frames; char path[1024]; uint32_t data_off; } Track;

static Track g_tracks[MAX_TRACKS + 1];
static int g_ntracks;
static uint32_t g_time_format = MCI_FORMAT_MSF;
static SDL_Mutex *g_cd_lock;
static SDL_AudioStream *g_cd_stream;

/* playback state, in absolute frame units: (track, frame offset within track) */
static int g_play_track, g_end_track;
static uint32_t g_play_frame, g_end_frame;   /* g_end_frame exclusive */
static uint32_t g_play_byte;                  /* byte offset inside the current frame */
static bool g_playing;
static FILE *g_cd_file;
static drflac *g_cd_flac;
static uint64_t g_cd_flac_pos;
static int g_cd_file_track;
static float g_cd_gain = 1.0f;

void cdaudio_init(const char *music_dir)
{
    g_cd_lock = SDL_CreateMutex();
    g_tracks[1].audio = false;
    g_tracks[1].frames = 53932;    /* data track */
    g_ntracks = 1;
    for (int t = 2; t <= MAX_TRACKS; t++) {
        char p[1024];
        snprintf(p, sizeof p, "%s/track%02d.flac", music_dir, t);
        drflac *fl = drflac_open_file(p, NULL);
        if (fl) {
            g_tracks[t].audio = true;
            g_tracks[t].flac = true;
            g_tracks[t].frames = (uint32_t)(fl->totalPCMFrameCount / 588);
            snprintf(g_tracks[t].path, sizeof g_tracks[t].path, "%s", p);
            drflac_close(fl);
            g_ntracks = t;
            continue;
        }
        snprintf(p, sizeof p, "%s/track%02d.wav", music_dir, t);
        FILE *f = fopen(p, "rb");
        if (!f) break;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        g_tracks[t].audio = true;
        g_tracks[t].data_off = 44;
        g_tracks[t].frames = (uint32_t)((sz - 44) / 2352);
        snprintf(g_tracks[t].path, sizeof g_tracks[t].path, "%s", p);
        g_ntracks = t;
    }
    rlog("cdaudio: %d tracks (%d audio)", g_ntracks, g_ntracks - 1);
}

static uint32_t msf_pack(uint32_t frames) { return (frames / 4500) | ((frames / 75 % 60) << 8) | ((frames % 75) << 16); }
static uint32_t tmsf_pack(int t, uint32_t frames) { return (uint32_t)t | (msf_pack(frames) << 8); }
static uint32_t msf_frames(uint32_t m, uint32_t s, uint32_t f) { return (m * 60 + s) * 75 + f; }

static void SDLCALL cd_cb(void *ud, SDL_AudioStream *stream, int additional, int total)
{
    (void)ud; (void)total;
    if (additional <= 0) return;
    uint8_t *buf = SDL_malloc((size_t)additional);
    int filled = 0;
    SDL_LockMutex(g_cd_lock);
    while (filled < additional && g_playing) {
        if (g_play_track > g_end_track || (g_play_track == g_end_track && g_play_frame >= g_end_frame)) {
            g_playing = false;
            break;
        }
        Track *t = &g_tracks[g_play_track];
        if (!t->audio || g_play_frame >= t->frames) {
            g_play_track++;
            g_play_frame = 0;
            g_play_byte = 0;
            continue;
        }
        if (g_cd_file_track != g_play_track) {
            if (g_cd_file) { fclose(g_cd_file); g_cd_file = NULL; }
            if (g_cd_flac) { drflac_close(g_cd_flac); g_cd_flac = NULL; }
            if (t->flac) g_cd_flac = drflac_open_file(t->path, NULL);
            else g_cd_file = fopen(t->path, "rb");
            g_cd_flac_pos = 0;
            g_cd_file_track = g_play_track;
            if (!g_cd_file && !g_cd_flac) { g_playing = false; break; }
        }
        uint32_t end = g_play_track == g_end_track ? g_end_frame : t->frames;
        uint32_t avail = (end - g_play_frame) * 2352 - g_play_byte;
        uint32_t want = (uint32_t)(additional - filled) & ~3u;
        if (want > avail) want = avail;
        if (!want) break;
        size_t got;
        if (g_cd_flac) {
            uint64_t pcm = ((uint64_t)g_play_frame * 2352 + g_play_byte) / 4;
            if (pcm != g_cd_flac_pos) {
                if (!drflac_seek_to_pcm_frame(g_cd_flac, pcm)) { g_playing = false; break; }
                g_cd_flac_pos = pcm;
            }
            uint64_t n = drflac_read_pcm_frames_s16(g_cd_flac, want / 4, (drflac_int16 *)(buf + filled));
            g_cd_flac_pos += n;
            got = (size_t)n * 4;
        } else {
            fseek(g_cd_file, (long)(t->data_off + g_play_frame * 2352 + g_play_byte), SEEK_SET);
            got = fread(buf + filled, 1, want, g_cd_file);
        }
        if (!got) { g_playing = false; break; }
        filled += (int)got;
        uint32_t adv = g_play_byte + (uint32_t)got;
        g_play_frame += adv / 2352;
        g_play_byte = adv % 2352;
    }
    float gain = g_cd_gain;
    SDL_UnlockMutex(g_cd_lock);
    if (filled < additional) memset(buf + filled, 0, (size_t)(additional - filled));
    wav_record("cdtrack", buf, additional, 44100);   /* debug: track data before the volume */
    if (gain < 0.999f) {
        int16_t *s = (int16_t *)buf;
        for (int i = 0; i < additional / 2; i++) s[i] = (int16_t)(s[i] * gain);
    }
    SDL_PutAudioStreamData(stream, buf, additional);
    wav_record("cdmusic", buf, additional, 44100);
    SDL_free(buf);
}

static void cd_open_stream(void)
{
    if (g_cd_stream) return;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 44100};
    g_cd_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, cd_cb, NULL);
    if (g_cd_stream) SDL_ResumeAudioStreamDevice(g_cd_stream);
    else rlog("cdaudio stream: %s", SDL_GetError());
}

/* decode a position in the current time format into (track, frame-in-track) */
static void decode_pos(uint32_t v, int *track, uint32_t *frame)
{
    if (g_time_format == MCI_FORMAT_TMSF) {
        *track = (int)(v & 0xFF);
        *frame = msf_frames((v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF);
    } else {
        /* absolute MSF over the whole disc (2 s pregap ignored) */
        uint32_t abs = msf_frames(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF);
        int t = 1;
        while (t < g_ntracks && abs >= g_tracks[t].frames) abs -= g_tracks[t++].frames;
        *track = t;
        *frame = abs;
    }
}

static void wmm_mciSendCommandA(CPU *c)
{
    uint32_t dev = ARG(0), msg = ARG(1), flags = ARG(2), p = ARG(3);
    uint32_t r = 0;
    SDL_LockMutex(g_cd_lock);
    switch (msg) {
    case MCI_OPEN:
        if (p) wr32(p + 4, CD_DEVICE_ID);
        rlog("MCI_OPEN cdaudio (%d tracks)", g_ntracks);
        break;
    case MCI_CLOSE:
        g_playing = false;
        break;
    case MCI_SET:
        if (flags & MCI_SET_TIME_FORMAT) g_time_format = rd32(p + 4);
        break;
    case MCI_PLAY: {
        int ft = g_play_track, tt = g_ntracks;
        uint32_t ff = g_play_frame, tf = g_tracks[g_ntracks].frames;
        if (flags & MCI_FROM) decode_pos(rd32(p + 4), &ft, &ff);
        if (flags & MCI_TO) decode_pos(rd32(p + 8), &tt, &tf);
        if (ft < 1) ft = 1;
        g_play_track = ft;
        g_play_frame = ff;
        g_play_byte = 0;
        g_end_track = tt;
        g_end_frame = tf;
        g_playing = true;
        rlog("MCI_PLAY track %d+%u -> track %d+%u", ft, ff, tt, tf);
        SDL_UnlockMutex(g_cd_lock);
        cd_open_stream();
        SDL_LockMutex(g_cd_lock);
        break;
    }
    case MCI_STOP:
    case MCI_PAUSE:
        g_playing = false;
        break;
    case MCI_RESUME:
        g_playing = true;
        break;
    case MCI_SEEK:
        g_playing = false;
        break;
    case MCI_STATUS: {
        uint32_t item = rd32(p + 8), track = rd32(p + 12);
        uint32_t ret = 0;
        switch (item) {
        case MCI_STATUS_LENGTH:
            if ((flags & MCI_TRACK) && track >= 1 && (int)track <= g_ntracks) ret = msf_pack(g_tracks[track].frames);
            else { uint32_t tot = 0; for (int t = 1; t <= g_ntracks; t++) tot += g_tracks[t].frames; ret = msf_pack(tot); }
            break;
        case MCI_STATUS_POSITION:
            if (flags & MCI_TRACK) ret = g_time_format == MCI_FORMAT_TMSF ? tmsf_pack((int)track, 0) : 0;
            else ret = tmsf_pack(g_play_track ? g_play_track : 1, g_play_frame);
            break;
        case MCI_STATUS_NUMBER_OF_TRACKS: ret = (uint32_t)g_ntracks; break;
        case MCI_STATUS_MODE: ret = g_playing ? MCI_MODE_PLAY : MCI_MODE_STOP; break;
        case MCI_STATUS_MEDIA_PRESENT: case MCI_STATUS_READY: ret = 1; break;
        case MCI_STATUS_TIME_FORMAT: ret = g_time_format; break;
        case MCI_CDA_STATUS_TYPE_TRACK: ret = (track >= 1 && (int)track <= g_ntracks && g_tracks[track].audio) ? 1028 : 1029; break;
        default: rlog("MCI_STATUS item %u not handled", item); break;
        }
        wr32(p + 4, ret);
        break;
    }
    default:
        rlog("mciSendCommand(dev=%x msg=%x flags=%x) not handled", dev, msg, flags);
        r = 0;
    }
    SDL_UnlockMutex(g_cd_lock);
    hle_return(c, r, 4);
}

static uint32_t g_aux_volume = 0xFFFFFFFF;

static void wmm_auxGetNumDevs(CPU *c) { hle_return(c, 1, 0); }

static void wmm_auxGetDevCapsA(CPU *c)
{
    uint32_t p = ARG(1), n = ARG(2);
    memset(g_mem + p, 0, n);
    wr16(p, 1);
    wr16(p + 2, 1);
    snprintf((char *)g_mem + p + 8, 32, "CD Audio");
    wr16(p + 40, 1);          /* AUXCAPS_CDAUDIO */
    wr32(p + 44, 3);          /* AUXCAPS_VOLUME | AUXCAPS_LRVOLUME */
    hle_return(c, 0, 3);
}

static void wmm_auxGetVolume(CPU *c) { wr32(ARG(1), g_aux_volume); hle_return(c, 0, 2); }

static void wmm_auxSetVolume(CPU *c)
{
    g_aux_volume = ARG(1);
    float l = (float)(g_aux_volume & 0xFFFF) / 65535.f, r = (float)(g_aux_volume >> 16) / 65535.f;
    g_cd_gain = (l + r) * 0.5f;
    hle_return(c, 0, 2);
}

const HleDef hle_winmm[] = {
    {"WINMM", "mciSendCommandA", wmm_mciSendCommandA},
    {"WINMM", "auxGetNumDevs", wmm_auxGetNumDevs},
    {"WINMM", "auxGetDevCapsA", wmm_auxGetDevCapsA},
    {"WINMM", "auxGetVolume", wmm_auxGetVolume},
    {"WINMM", "auxSetVolume", wmm_auxSetVolume},
    {0, 0, 0},
};
