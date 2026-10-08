/* Entry point: set up SDL, guest memory and the recompiled image, then run the game's
 * own entry point on the main thread (it becomes the window/message thread). */
#include <setjmp.h>
#include <stdlib.h>
#include <SDL3/SDL_main.h>
#include "runtime.h"

extern const HleDef hle_kernel32[], hle_user32[], hle_ddraw[], hle_dsound[], hle_dinput[], hle_winmm[], hle_dplay[];
static const HleDef *const g_modules[] = {hle_kernel32, hle_user32, hle_ddraw, hle_dsound, hle_dinput, hle_winmm, hle_dplay};

static void missing_import(CPU *c)
{
    (void)c;
    fatal("The game called a Windows function that is not implemented yet.\nSee irc_runtime.log.");
}

static void bind_imports(void)
{
    int missing = 0;
    for (unsigned i = 0; i < g_import_count; i++) {
        const ImportEntry *im = &g_import_table[i];
        HleFn fn = NULL;
        for (unsigned m = 0; m < sizeof g_modules / sizeof g_modules[0] && !fn; m++)
            for (const HleDef *d = g_modules[m]; d->dll; d++)
                if (!SDL_strcasecmp(d->dll, im->dll) && !strcmp(d->name, im->name)) { fn = d->fn; break; }
        char *name = SDL_malloc(96);
        snprintf(name, 96, "%s!%s", im->dll, im->name);
        if (!fn) {
            rlog("import not implemented: %s", name);
            fn = missing_import;
            missing++;
        }
        wr32(im->iat, hle_slot(name, fn));
    }
    rlog("bound %u imports (%d missing)", g_import_count, missing);
}

static void load_config(int argc, char **argv)
{
    const char *base = SDL_GetBasePath();
    snprintf(g_cfg.data_dir, sizeof g_cfg.data_dir, "%sgame", base ? base : "");
    g_cfg.window_scale = 0;
    g_cfg.fullscreen = true;
    g_cfg.fps_limit = -1;
    g_cfg.d3d = true;
    g_cfg.render_scale = 0;
    g_cfg.smooth3d = true;
    char ini[1100];
    snprintf(ini, sizeof ini, "%sirc_native.ini", base ? base : "");
    FILE *f = fopen(ini, "r");
    if (f) {
        char line[1200];
        while (fgets(line, sizeof line, f)) {
            char key[64], val[1100];
            if (line[0] == ';' || line[0] == '#' || sscanf(line, " %63[^= ] = %1099[^\r\n]", key, val) != 2) continue;
            if (!SDL_strcasecmp(key, "data_dir")) {
                bool abs = val[0] == '/' || val[0] == '\\' || (val[0] && val[1] == ':');
                snprintf(g_cfg.data_dir, sizeof g_cfg.data_dir, "%s%s", abs ? "" : (base ? base : ""), val);
            }
            else if (!SDL_strcasecmp(key, "scale")) g_cfg.window_scale = atoi(val);
            else if (!SDL_strcasecmp(key, "fps_limit")) g_cfg.fps_limit = atoi(val);
            else if (!SDL_strcasecmp(key, "direct3d")) g_cfg.d3d = atoi(val) != 0;
            else if (!SDL_strcasecmp(key, "render_scale")) g_cfg.render_scale = atoi(val);
            else if (!SDL_strcasecmp(key, "texture_filter")) g_cfg.smooth3d = atoi(val) != 0;
            else if (!SDL_strcasecmp(key, "fullscreen")) g_cfg.fullscreen = atoi(val) != 0;
            else if (!SDL_strcasecmp(key, "smooth")) g_cfg.smooth = atoi(val) != 0;
            else if (!SDL_strcasecmp(key, "aspect")) g_cfg.stretch = !SDL_strcasecmp(val, "stretch");
            else if (!SDL_strcasecmp(key, "trace")) g_cfg.trace = atoi(val) != 0;
        }
        fclose(f);
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) snprintf(g_cfg.data_dir, sizeof g_cfg.data_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--fullscreen")) g_cfg.fullscreen = true;
        else if (!strcmp(argv[i], "--window")) g_cfg.fullscreen = false;
        else if (!strcmp(argv[i], "--stretch")) g_cfg.stretch = true;
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) g_cfg.fps_limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-d3d")) g_cfg.d3d = false;
        else if (!strcmp(argv[i], "--render-scale") && i + 1 < argc) g_cfg.render_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--smooth")) g_cfg.smooth = true;
        else if (!strcmp(argv[i], "--trace")) g_cfg.trace = true;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) g_cfg.window_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) snprintf(g_cfg.shot_dir, sizeof g_cfg.shot_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) snprintf(g_cfg.autokeys, sizeof g_cfg.autokeys, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--shot-ms") && i + 1 < argc) g_cfg.shot_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--exit-after") && i + 1 < argc) g_cfg.exit_after_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) snprintf(g_cfg.record_dir, sizeof g_cfg.record_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc) snprintf(g_cfg.poke, sizeof g_cfg.poke, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--peek") && i + 1 < argc) snprintf(g_cfg.peek, sizeof g_cfg.peek, "%s", argv[++i]);
    }
}

int main(int argc, char **argv)
{
    load_config(argc, argv);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetAppMetadata("International Rally Championship", "1.0", "org.ircrecomp.irc");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK))
        fatal("SDL_Init: %s", SDL_GetError());
    if (!gmem_init()) fatal("cannot reserve guest address space (a 64-bit system is required)");
    rlog("International Rally Championship - native build (ircrecomp)");
    rlog("data dir: %s", g_cfg.data_dir);

    char exe[1100];
    snprintf(exe, sizeof exe, "%s/RAL.EXE", g_cfg.data_dir);
    if (!image_load(exe)) fatal("Cannot load %s.\nRun the ircrecomp installer first.", exe);
    bind_imports();
    patches_apply();
    vfs_init(g_cfg.data_dir, NULL);
    char music[1100];
    snprintf(music, sizeof music, "%s/music", g_cfg.data_dir);
    cdaudio_init(music);

    GuestThread *t = thread_main_init();
    CPU *c = &t->cpu;
    if (!setjmp(*(jmp_buf *)t->jb)) {
        guest_call(c, g_entry_point, 0);
        rlog("game entry point returned");
    }
    SDL_Quit();
    return 0;
}
