/*
 * pds-nx: Panzer Dragoon Saga for Nintendo Switch.
 *
 * A single-game Switch frontend for the Yaba Sanshiro Saturn emulator core
 * (libretro "yabasanshiro" branch), linked in statically. It owns the window
 * and GL context, feeds the core input and audio, and draws the core's frame
 * to the screen at the right aspect ratio.
 *
 * SD card layout (all under sdmc:/switch/pds-nx/):
 *   pds-nx.nro                 this program
 *   saturn_bios.bin            your Saturn BIOS (also accepted: sega_101.bin, mpr-17933.bin)
 *   discs/                     your four discs as .cue/.bin or .chd
 *   config.ini                 settings (written with defaults on first run)
 *   saves/                     the Saturn's internal save memory
 *   states/                    save states
 *   pds-nx.log                 log of the last run
 *
 * This file is part of pds-nx and is licensed under the GNU GPL v2 or later.
 */
#include <switch.h>

#include <SDL2/SDL.h>
#include <GLES3/gl3.h>

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <libretro.h>

#include "nx_clock.h"
#include "nx_log.h"
#include "nx_ui.h"

extern "C" {
void pdsnx_open_tray(void);
int pdsnx_close_tray(const char *path);
int pdsnx_backup_flush(void);
void pdsnx_debug_state(unsigned *frame, int *line);
void nx_jit_shutdown(void);
void pdsnx_apply_video_options(void);
extern unsigned long long pdsnx_prof[4];
void pdsnx_resolve_private_gl(void *(*get_proc)(const char *));
}

#define PDSNX_VERSION "1.0.0"
static bool g_debug_log;

/* Progress counters for the watchdog log. */
static volatile u64 g_loop_iter;
static volatile unsigned g_frames_shown;
static volatile int g_phase; /* 1 = in retro_run, 2 = presenting, 3 = between frames */
static volatile bool g_watchdog_run;
/* Timing, summed between watchdog lines (microseconds) */
static volatile u64 g_us_run, g_us_present, g_us_wait;
static volatile unsigned g_underruns;
static u64 ticks_us(u64 t) { return armTicksToNs(t) / 1000; }
static Thread g_watchdog;

static void watchdog_main(void *arg)
{
    (void)arg;
    unsigned last_shown = 0;
    while (g_watchdog_run) {
        svcSleepThread(2000000000LL);
        if (!g_watchdog_run)
            break;
        unsigned frame = 0;
        int line = 0;
        pdsnx_debug_state(&frame, &line);
        if (!g_debug_log) {
            for (int i = 0; i < 4; i++) pdsnx_prof[i] = 0;
            g_us_run = g_us_present = g_us_wait = 0;
            g_underruns = 0;
            last_shown = g_frames_shown;
            continue;
        }
        unsigned n = g_frames_shown - last_shown;
        u64 run = g_us_run, pres = g_us_present, wait = g_us_wait;
        g_us_run = g_us_present = g_us_wait = 0;
        unsigned und = g_underruns;
        g_underruns = 0;
        double d = n ? (double)n : 1.0;
        double p[4];
        for (int i = 0; i < 4; i++) {
            p[i] = pdsnx_prof[i] / 19200.0 / d; /* 19.2 MHz ticks -> ms per frame */
            pdsnx_prof[i] = 0;
        }
        nxlog("watchdog: frames=%u (+%u) emu=%.1fms [sh2=%.1f line=%.1f vbin=%.1f vbout=%.1f] draw=%.1fms "
              "idle=%.1fms audio_underruns=%u%s",
              g_frames_shown, n, (run - pres) / 1000.0 / d, p[0], p[1], p[2], p[3], pres / 1000.0 / d,
              wait / 1000.0 / d, und, n == 0 ? "  (no new frames)" : "");
        (void)frame; (void)line;
        last_shown = g_frames_shown;
        nxlog_flush();
    }
}

#define BASE_DIR "sdmc:/switch/pds-nx"


/* ------------------------------------------------------------------------ */
/* Settings                                                                 */
/* ------------------------------------------------------------------------ */

static std::map<std::string, std::string> g_options;
static bool g_options_dirty;

enum { PAGE_NONE, PAGE_VIDEO, PAGE_PERF, PAGE_CONTROLS };
enum { APPLY_FRONTEND, APPLY_VIDEO, APPLY_CLOCK, APPLY_SYNC, APPLY_RESTART };

struct Choice { const char *value; const char *label; };
struct Setting {
    const char *key;
    const char *label;
    const char *comment;
    int page;
    int apply;
    std::vector<Choice> choices;
};

/* Switch buttons a Saturn button can be assigned to (used by the btn_* keys). */
static const std::vector<Choice> k_switch_buttons = {
    { "A", "A" }, { "B", "B" }, { "X", "X" }, { "Y", "Y" }, { "L", "L" }, { "R", "R" },
    { "ZL", "ZL" }, { "ZR", "ZR" }, { "Plus", "+" }, { "LStick", "Left stick click" },
    { "RStick", "Right stick click" }, { "None", "(none)" },
};

static const std::vector<Setting> k_settings = {
    /* Video */
    { "aspect", "Screen shape", "4:3 (original) or 16:9 (stretched to fill)", PAGE_VIDEO, APPLY_FRONTEND,
      { { "4:3", "4:3 original" }, { "16:9", "16:9 stretched" } } },
    { "filter", "Scaling filter", "linear (smooth) or nearest (sharp pixels)", PAGE_VIDEO, APPLY_FRONTEND,
      { { "linear", "Smooth" }, { "nearest", "Sharp" } } },
    { "yabasanshiro_resolution_mode", "Internal resolution", "How sharp the 3D is drawn", PAGE_VIDEO, APPLY_VIDEO,
      { { "original", "Original" }, { "2x", "2x" }, { "720p", "720p" }, { "1080p", "1080p" } } },
    { "yabasanshiro_polygon_mode", "Polygon mode",
      "Perspective correction fixes warped textures and is the fastest. CPU tessellation is an alternative",
      PAGE_VIDEO, APPLY_VIDEO,
      { { "perspective_correction", "Perspective correction" }, { "cpu_tesselation", "CPU tessellation" } } },
    { "yabasanshiro_rbg_resolution_mode", "Sky/floor resolution",
      "Resolution of rotating backgrounds (sky and ground planes)", PAGE_VIDEO, APPLY_VIDEO,
      { { "original", "Original" }, { "2x", "2x" }, { "720p", "720p" }, { "1080p", "1080p" },
        { "Fit_to_emulation", "Match internal" } } },
    { "yabasanshiro_rbg_use_compute_shader", "Sky/floor on compute shader",
      "Draw rotating backgrounds with compute shaders", PAGE_VIDEO, APPLY_VIDEO,
      { { "disabled", "Off" }, { "enabled", "On" } } },
    /* Performance */
    { "cpu_mhz", "CPU clock", "Switch CPU speed while playing, in MHz (0 = stock 1020; 1785 recommended)",
      PAGE_PERF, APPLY_CLOCK,
      { { "0", "Stock (1020)" }, { "1224", "1224 MHz" }, { "1428", "1428 MHz" }, { "1581", "1581 MHz" },
        { "1785", "1785 MHz" } } },
    { "gpu_mhz", "GPU clock", "Switch GPU speed in MHz (0 = leave alone)", PAGE_PERF, APPLY_CLOCK,
      { { "0", "Stock" }, { "460", "460 MHz" }, { "614", "614 MHz" }, { "768", "768 MHz" } } },
    { "sync", "Timing", "audio = smooth sound (recommended); video = lock to the screen refresh",
      PAGE_PERF, APPLY_SYNC, { { "audio", "Audio sync" }, { "video", "Video sync" } } },
    { "yabasanshiro_frameskip", "Auto frameskip", "Skip frames when the Switch falls behind", PAGE_PERF,
      APPLY_VIDEO, { { "disabled", "Off" }, { "enabled", "On" } } },
    { "show_fps", "Show FPS", "Frames per second in the corner", PAGE_PERF, APPLY_FRONTEND,
      { { "off", "Off" }, { "on", "On" } } },
    /* Controls (Saturn button -> Switch button) */
    { "btn_a", "Saturn A", "Switch button for Saturn A", PAGE_CONTROLS, APPLY_FRONTEND, k_switch_buttons },
    { "btn_b", "Saturn B", "Switch button for Saturn B", PAGE_CONTROLS, APPLY_FRONTEND, k_switch_buttons },
    { "btn_c", "Saturn C", "Switch button for Saturn C", PAGE_CONTROLS, APPLY_FRONTEND, k_switch_buttons },
    { "btn_x", "Saturn X", "Switch button for Saturn X", PAGE_CONTROLS, APPLY_FRONTEND, k_switch_buttons },
    { "btn_y", "Saturn Y", "Switch button for Saturn Y", PAGE_CONTROLS, APPLY_FRONTEND, k_switch_buttons },
    { "btn_z", "Saturn Z", "Switch button for Saturn Z", PAGE_CONTROLS, APPLY_FRONTEND, k_switch_buttons },
    { "btn_l", "Saturn L", "Switch button for Saturn L", PAGE_CONTROLS, APPLY_FRONTEND, k_switch_buttons },
    { "btn_r", "Saturn R", "Switch button for Saturn R", PAGE_CONTROLS, APPLY_FRONTEND, k_switch_buttons },
    { "btn_start", "Saturn Start", "Switch button for Saturn Start", PAGE_CONTROLS, APPLY_FRONTEND,
      k_switch_buttons },
    /* Config file only */
    { "debug_log", "Debug log", "Write timing details to pds-nx.log every 2 seconds (for troubleshooting)",
      PAGE_NONE, APPLY_RESTART, { { "off", "Off" }, { "on", "On" } } },
    { "yabasanshiro_sh2coretype", "SH-2 CPU", "dynarec (fast) or interpreter (slow, for troubleshooting); restart to apply",
      PAGE_NONE, APPLY_RESTART, { { "dynarec", "Dynarec" }, { "interpreter", "Interpreter" } } },
    { "yabasanshiro_system_language", "Saturn language",
      "english, deutsch, french, spanish, italian, japanese; restart to apply", PAGE_NONE, APPLY_RESTART,
      { { "english", "English" }, { "deutsch", "Deutsch" }, { "french", "French" }, { "spanish", "Spanish" },
        { "italian", "Italian" }, { "japanese", "Japanese" } } },
};

static const char *const k_default_buttons[][2] = {
    { "btn_a", "Y" }, { "btn_b", "B" }, { "btn_c", "A" }, { "btn_x", "ZL" }, { "btn_y", "X" },
    { "btn_z", "ZR" }, { "btn_l", "L" }, { "btn_r", "R" }, { "btn_start", "Plus" },
};

static void set_default_buttons(void)
{
    for (auto &d : k_default_buttons)
        g_options[d[0]] = d[1];
}

static void set_default_options(void)
{
    g_options["aspect"] = "4:3";
    g_options["filter"] = "linear";
    g_options["cpu_mhz"] = "1785";
    g_options["gpu_mhz"] = "0";
    g_options["sync"] = "audio";
    g_options["show_fps"] = "off";
    g_options["debug_log"] = "off";
    g_options["yabasanshiro_force_hle_bios"] = "disabled";
    g_options["yabasanshiro_frameskip"] = "enabled";
    g_options["yabasanshiro_addon_cart"] = "4M_extended_ram";
    g_options["yabasanshiro_system_language"] = "english";
    g_options["yabasanshiro_multitap_port1"] = "disabled";
    g_options["yabasanshiro_multitap_port2"] = "disabled";
    g_options["yabasanshiro_sh2coretype"] = "dynarec";
    g_options["yabasanshiro_polygon_mode"] = "perspective_correction";
    g_options["yabasanshiro_resolution_mode"] = "original";
    g_options["yabasanshiro_rbg_resolution_mode"] = "original";
    g_options["yabasanshiro_rbg_use_compute_shader"] = "enabled";
    set_default_buttons();
}

static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

#define CONFIG_PATH BASE_DIR "/config.ini"

static void save_config(void)
{
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        nxlog("config: cannot write %s", CONFIG_PATH);
        return;
    }
    fputs("# pds-nx settings. Most of these can be changed in game: tap - (Minus).\n"
          "# Lines starting with # are ignored.\n\n", f);
    for (const Setting &st : k_settings) {
        fprintf(f, "# %s\n# options:", st.comment);
        for (size_t i = 0; i < st.choices.size(); i++)
            fprintf(f, "%s %s", i ? "," : "", st.choices[i].value);
        fprintf(f, "\n%s=%s\n\n", st.key, g_options[st.key].c_str());
    }
    fclose(f);
}

static void load_config(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        save_config();
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string l = trim(line);
        if (l.empty() || l[0] == '#')
            continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
        g_options[k] = v;
        nxlog("config: %s=%s", k.c_str(), v.c_str());
    }
    fclose(f);
    /* GPU tessellation is far too slow on Switch and hard to get out of. */
    if (g_options["yabasanshiro_polygon_mode"] == "gpu_tesselation")
        g_options["yabasanshiro_polygon_mode"] = "perspective_correction";
    /* Rewrite so the file picks up any settings added since it was created. */
    save_config();
}

/* ------------------------------------------------------------------------ */
/* Files                                                                    */
/* ------------------------------------------------------------------------ */

static bool file_exists(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static bool ends_with(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

/* Disc images, indexed by disc number - 1. */
static std::string g_discs[4];
static int g_disc_count;
static int g_current_disc;

static void scan_dir_for_discs(const std::string &dir, std::vector<std::string> &found)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        std::string name = e->d_name;
        std::string l = lower(name);
        if (ends_with(l, ".cue") || ends_with(l, ".chd"))
            found.push_back(dir + "/" + name);
    }
    closedir(d);
}

static void find_discs(void)
{
    std::vector<std::string> found;
    scan_dir_for_discs(BASE_DIR "/discs", found);
    scan_dir_for_discs(BASE_DIR, found);
    std::sort(found.begin(), found.end());

    std::vector<std::string> unnumbered;
    for (const std::string &p : found) {
        std::string l = lower(p.substr(p.find_last_of('/') + 1));
        int n = 0;
        size_t at = l.find("disc");
        if (at != std::string::npos) {
            for (size_t i = at + 4; i < l.size(); i++) {
                if (l[i] >= '1' && l[i] <= '4') { n = l[i] - '0'; break; }
                if (l[i] != ' ' && l[i] != '_' && l[i] != '-') break;
            }
        }
        if (n >= 1 && n <= 4 && g_discs[n - 1].empty())
            g_discs[n - 1] = p;
        else
            unnumbered.push_back(p);
    }
    /* Fill any gaps with unnumbered images in name order. */
    for (int i = 0; i < 4 && !unnumbered.empty(); i++) {
        if (g_discs[i].empty()) {
            g_discs[i] = unnumbered.front();
            unnumbered.erase(unnumbered.begin());
        }
    }
    g_disc_count = 0;
    for (int i = 0; i < 4; i++) {
        if (!g_discs[i].empty()) {
            g_disc_count = i + 1;
            nxlog("disc %d: %s", i + 1, g_discs[i].c_str());
        }
    }
}

static std::string find_bios(void)
{
    static const char *names[] = { "saturn_bios.bin", "sega_101.bin", "mpr-17933.bin" };
    for (const char *n : names) {
        std::string p = std::string(BASE_DIR) + "/" + n;
        if (file_exists(p))
            return p;
    }
    return std::string();
}

/* ------------------------------------------------------------------------ */
/* Startup problems: plain text screen, wait for + to exit                  */
/* ------------------------------------------------------------------------ */

static void fatal_screen(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    nxlog("fatal: %s", msg);
    nxlog_flush();

    consoleInit(NULL);
    PadState pad;
    padInitializeDefault(&pad);
    printf("\n  Panzer Dragoon Saga for Switch (pds-nx)\n\n");
    printf("%s\n\n  Press + to exit.\n", msg);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
}

/* ------------------------------------------------------------------------ */
/* Video                                                                    */
/* ------------------------------------------------------------------------ */

static SDL_Window *g_window;
static SDL_GLContext g_gl;
static struct retro_hw_render_callback g_hw;
static GLuint g_fbo, g_fbo_color, g_fbo_depth;
static unsigned g_fbo_w, g_fbo_h;
static unsigned g_frame_w = 320, g_frame_h = 224;

static void destroy_fbo(void)
{
    if (g_fbo) glDeleteFramebuffers(1, &g_fbo);
    if (g_fbo_color) glDeleteTextures(1, &g_fbo_color);
    if (g_fbo_depth) glDeleteRenderbuffers(1, &g_fbo_depth);
    g_fbo = g_fbo_color = g_fbo_depth = 0;
}

static void create_fbo(unsigned w, unsigned h)
{
    destroy_fbo();
    g_fbo_w = w;
    g_fbo_h = h;

    glGenTextures(1, &g_fbo_color);
    glBindTexture(GL_TEXTURE_2D, g_fbo_color);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenRenderbuffers(1, &g_fbo_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_fbo_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbo_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_fbo_depth);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    nxlog("video: frame buffer %ux%u, status 0x%x", w, h, status);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

static uintptr_t RETRO_CALLCONV hw_get_current_framebuffer(void)
{
    return g_fbo;
}

static retro_proc_address_t RETRO_CALLCONV hw_get_proc_address(const char *sym)
{
    return (retro_proc_address_t)SDL_GL_GetProcAddress(sym);
}

static void draw_overlay(void);
static void present_inner(void);
static void present(void)
{
    u64 t0 = armGetSystemTick();
    present_inner();
    g_us_present += ticks_us(armGetSystemTick() - t0);
}

static void present_inner(void)
{
    int sw = 0, sh = 0;
    SDL_GL_GetDrawableSize(g_window, &sw, &sh);

    int dw = sw, dh = sh, dx = 0, dy = 0;
    if (g_options["aspect"] != "16:9") {
        dw = sh * 4 / 3;
        if (dw > sw) dw = sw;
        dx = (sw - dw) / 2;
    }

    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, sw, sh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned fw = std::min(g_frame_w, g_fbo_w), fh = std::min(g_frame_h, g_fbo_h);
    glBlitFramebuffer(0, 0, fw, fh, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT,
                      g_options["filter"] == "nearest" ? GL_NEAREST : GL_LINEAR);
    draw_overlay();
    ui_flush(sw, sh);
    SDL_GL_SwapWindow(g_window);

    /* Hand the core back its own frame buffer and state. */
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
}

static void RETRO_CALLCONV video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
    (void)pitch;
    g_frames_shown++;
    if (g_debug_log && (g_frames_shown <= 5 || g_frames_shown % 600 == 0)) {
        nxlog("frame %u (%ux%u%s)", g_frames_shown, w, h, data == RETRO_HW_FRAME_BUFFER_VALID ? "" : ", dupe");
        nxlog_flush();
    }
    g_phase = 2;
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
        g_frame_w = w;
        g_frame_h = h;
    }
    present();
}

/* ------------------------------------------------------------------------ */
/* Audio                                                                    */
/* ------------------------------------------------------------------------ */

static SDL_AudioDeviceID g_audio;

/*
 * Audio sync: the sound queue sets the pace. We keep about AUDIO_TARGET of
 * sound queued; the main loop runs the next frame only once the queue has
 * drained below that. If the queue ever runs dry (the Switch fell behind),
 * playback pauses until it has refilled, so a slow moment becomes one short
 * pause instead of a stream of crackles.
 */
#define AUDIO_RATE    44100
#define AUDIO_BYTES   4                                   /* 16-bit stereo */
#define AUDIO_TARGET  (AUDIO_RATE * AUDIO_BYTES * 60 / 1000) /* 60 ms */
#define AUDIO_MAX     (AUDIO_TARGET * 4)                  /* drop beyond 240 ms */

static bool g_audio_sync = true;
static bool g_audio_playing;

static size_t RETRO_CALLCONV audio_sample_batch(const int16_t *data, size_t frames)
{
    if (!g_audio)
        return frames;
    Uint32 queued = SDL_GetQueuedAudioSize(g_audio);
    if (g_audio_playing && queued == 0) {
        g_underruns++;
        SDL_PauseAudioDevice(g_audio, 1);
        g_audio_playing = false;
    }
    if (queued < AUDIO_MAX)
        SDL_QueueAudio(g_audio, data, (Uint32)(frames * AUDIO_BYTES));
    if (!g_audio_playing && SDL_GetQueuedAudioSize(g_audio) >= AUDIO_TARGET) {
        SDL_PauseAudioDevice(g_audio, 0);
        g_audio_playing = true;
    }
    return frames;
}

/* Called after each emulated frame in audio-sync mode. */
static void audio_wait(void)
{
    if (!g_audio || !g_audio_sync || !g_audio_playing)
        return;
    u64 t0 = armGetSystemTick();
    while (SDL_GetQueuedAudioSize(g_audio) > AUDIO_TARGET && g_audio_playing)
        svcSleepThread(500000); /* 0.5 ms */
    g_us_wait += ticks_us(armGetSystemTick() - t0);
}

static void RETRO_CALLCONV audio_sample(int16_t l, int16_t r)
{
    int16_t s[2] = { l, r };
    audio_sample_batch(s, 1);
}

static void open_audio(void)
{
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_audio) {
        nxlog("audio: open failed: %s", SDL_GetError());
        return;
    }
    nxlog("audio: %d Hz, %d channels, %d samples, sync=%s", have.freq, have.channels, have.samples,
          g_audio_sync ? "audio" : "video");
    /* Stays paused until the first 60 ms are queued; see audio_sample_batch(). */
}

/* ------------------------------------------------------------------------ */
/* Input                                                                    */
/* ------------------------------------------------------------------------ */

static PadState g_pad;
static uint16_t g_retropad;  /* RETRO_DEVICE_ID_JOYPAD_* bits for player 1 */

static u64 switch_button_mask(const std::string &name)
{
    if (name == "A") return HidNpadButton_A;
    if (name == "B") return HidNpadButton_B;
    if (name == "X") return HidNpadButton_X;
    if (name == "Y") return HidNpadButton_Y;
    if (name == "L") return HidNpadButton_L;
    if (name == "R") return HidNpadButton_R;
    if (name == "ZL") return HidNpadButton_ZL;
    if (name == "ZR") return HidNpadButton_ZR;
    if (name == "Plus") return HidNpadButton_Plus;
    if (name == "LStick") return HidNpadButton_StickL;
    if (name == "RStick") return HidNpadButton_StickR;
    return 0;
}

/*
 * Saturn buttons in btn_* order, with the RetroPad id the core turns into
 * each one (the core routes B->A, A->B, L->C, Y->X, X->Y, R->Z, L2->L, R2->R).
 * Default layout: Saturn A B C on Switch Y B A, X Y Z on ZL X ZR, L R on L R.
 */
static const struct { const char *key; unsigned retro; } k_saturn_buttons[] = {
    { "btn_a", RETRO_DEVICE_ID_JOYPAD_B },  { "btn_b", RETRO_DEVICE_ID_JOYPAD_A },
    { "btn_c", RETRO_DEVICE_ID_JOYPAD_L },  { "btn_x", RETRO_DEVICE_ID_JOYPAD_Y },
    { "btn_y", RETRO_DEVICE_ID_JOYPAD_X },  { "btn_z", RETRO_DEVICE_ID_JOYPAD_R },
    { "btn_l", RETRO_DEVICE_ID_JOYPAD_L2 }, { "btn_r", RETRO_DEVICE_ID_JOYPAD_R2 },
    { "btn_start", RETRO_DEVICE_ID_JOYPAD_START },
};
static u64 g_bind[9];

static void update_bindings(void)
{
    for (int i = 0; i < 9; i++)
        g_bind[i] = switch_button_mask(g_options[k_saturn_buttons[i].key]);
}

static uint16_t map_buttons(u64 b, HidAnalogStickState stick)
{
    uint16_t m = 0;
    const int dz = 0x4000;
    if ((b & HidNpadButton_Up)    || stick.y >  dz) m |= 1 << RETRO_DEVICE_ID_JOYPAD_UP;
    if ((b & HidNpadButton_Down)  || stick.y < -dz) m |= 1 << RETRO_DEVICE_ID_JOYPAD_DOWN;
    if ((b & HidNpadButton_Left)  || stick.x < -dz) m |= 1 << RETRO_DEVICE_ID_JOYPAD_LEFT;
    if ((b & HidNpadButton_Right) || stick.x >  dz) m |= 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    for (int i = 0; i < 9; i++)
        if (g_bind[i] && (b & g_bind[i]))
            m |= 1 << k_saturn_buttons[i].retro;
    return m;
}

static void RETRO_CALLCONV input_poll(void) {}

static int16_t RETRO_CALLCONV input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return (int16_t)g_retropad;
    return (g_retropad >> id) & 1;
}

/* ------------------------------------------------------------------------ */
/* Save states and discs                                                    */
/* ------------------------------------------------------------------------ */

/* Short on-screen message (bottom left). */
static char g_toast[128];
static u64 g_toast_until;

static void toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_toast, sizeof(g_toast), fmt, ap);
    va_end(ap);
    g_toast_until = armGetSystemTick() + armNsToTicks(2500000000ULL);
    nxlog("msg: %s", g_toast);
}

#define STATE_SLOTS 10

static std::string state_path(int slot)
{
    char path[256];
    snprintf(path, sizeof(path), BASE_DIR "/states/slot%d.state", slot);
    return path;
}

static void save_state(int slot)
{
    size_t size = retro_serialize_size();
    std::vector<uint8_t> buf(size);
    if (!size || !retro_serialize(buf.data(), size)) {
        toast("Could not save state");
        return;
    }
    FILE *f = fopen(state_path(slot).c_str(), "wb");
    if (!f) {
        toast("Could not write slot %d", slot);
        return;
    }
    fwrite(buf.data(), 1, size, f);
    fclose(f);
    toast("Saved state to slot %d", slot);
}

static bool load_state(int slot)
{
    FILE *f = fopen(state_path(slot).c_str(), "rb");
    if (!f) {
        toast("Slot %d is empty", slot);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(size > 0 ? size : 0);
    size_t got = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (got == buf.size() && retro_unserialize(buf.data(), buf.size())) {
        toast("Loaded slot %d", slot);
        return true;
    }
    toast("Could not load slot %d", slot);
    return false;
}

static void change_disc(int index)
{
    if (index < 0 || index >= g_disc_count || g_discs[index].empty() || index == g_current_disc)
        return;
    nxlog("disc: switching to disc %d", index + 1);
    pdsnx_open_tray();
    int rc = pdsnx_close_tray(g_discs[index].c_str());
    if (rc == 0) {
        g_current_disc = index;
        toast("Disc %d inserted", index + 1);
    } else {
        toast("Could not insert disc %d", index + 1);
    }
}

/* ------------------------------------------------------------------------ */
/* In-game menu (tap - to open)                                             */
/* ------------------------------------------------------------------------ */

enum { MENU_MAIN, MENU_VIDEO, MENU_PERF, MENU_CONTROLS };
enum { MAIN_RESUME, MAIN_SAVE, MAIN_LOAD, MAIN_DISC, MAIN_VIDEO, MAIN_PERF, MAIN_CONTROLS, MAIN_RESET,
       MAIN_QUIT, MAIN_COUNT };

static bool g_menu_open;
static int g_menu_page = MENU_MAIN;
static int g_menu_cursor[4];
static int g_slot = 1;
static int g_disc_sel;
static bool g_quit_confirm;
static bool g_quit_requested;
static unsigned g_fps;
static u64 g_nav_prev, g_nav_next_repeat;

static void open_menu(void)
{
    g_menu_open = true;
    g_menu_page = MENU_MAIN;
    g_quit_confirm = false;
    g_disc_sel = g_current_disc;
    g_retropad = 0;
    if (g_audio) {
        SDL_PauseAudioDevice(g_audio, 1);
        SDL_ClearQueuedAudio(g_audio);
        g_audio_playing = false; /* resumes once the queue refills */
    }
}

static void close_menu(void)
{
    g_menu_open = false;
}

/* Settings shown on a menu page, in table order. */
static std::vector<const Setting *> page_settings(int page)
{
    int want = page == MENU_VIDEO ? PAGE_VIDEO : page == MENU_PERF ? PAGE_PERF : PAGE_CONTROLS;
    std::vector<const Setting *> v;
    for (const Setting &st : k_settings)
        if (st.page == want)
            v.push_back(&st);
    return v;
}

static int choice_index(const Setting &st)
{
    const std::string &cur = g_options[st.key];
    for (size_t i = 0; i < st.choices.size(); i++)
        if (cur == st.choices[i].value)
            return (int)i;
    return -1;
}

static void apply_setting(const Setting &st)
{
    switch (st.apply) {
    case APPLY_FRONTEND:
        update_bindings();
        break;
    case APPLY_VIDEO:
        pdsnx_apply_video_options();
        break;
    case APPLY_CLOCK:
        nx_clock_apply(atoi(g_options["cpu_mhz"].c_str()), atoi(g_options["gpu_mhz"].c_str()));
        break;
    case APPLY_SYNC:
        g_audio_sync = g_options["sync"] != "video";
        SDL_GL_SetSwapInterval(g_audio_sync ? 0 : 1);
        break;
    default:
        break;
    }
    save_config();
}

static void cycle_setting(const Setting &st, int dir)
{
    int n = (int)st.choices.size();
    int i = choice_index(st);
    i = i < 0 ? 0 : (i + dir + n) % n;
    g_options[st.key] = st.choices[i].value;
    nxlog("menu: %s=%s", st.key, st.choices[i].value);
    apply_setting(st);
}

static int page_rows(void)
{
    if (g_menu_page == MENU_MAIN)
        return MAIN_COUNT;
    int n = (int)page_settings(g_menu_page).size() + 1; /* + Back */
    if (g_menu_page == MENU_CONTROLS)
        n++; /* + Reset to default */
    return n;
}

static void menu_activate(int row, int dir)
{
    /* dir: 0 = A pressed, -1/+1 = left/right */
    if (g_menu_page == MENU_MAIN) {
        if (row != MAIN_QUIT)
            g_quit_confirm = false;
        switch (row) {
        case MAIN_RESUME:
            if (!dir) close_menu();
            break;
        case MAIN_SAVE:
        case MAIN_LOAD:
            if (dir) {
                g_slot = (g_slot - 1 + dir + STATE_SLOTS) % STATE_SLOTS + 1;
            } else if (row == MAIN_SAVE) {
                save_state(g_slot);
                close_menu();
            } else if (load_state(g_slot)) {
                close_menu();
            }
            break;
        case MAIN_DISC:
            if (dir && g_disc_count > 0) {
                g_disc_sel = (g_disc_sel + dir + g_disc_count) % g_disc_count;
            } else if (!dir) {
                if (g_disc_sel == g_current_disc)
                    toast("Disc %d is already in", g_disc_sel + 1);
                else {
                    change_disc(g_disc_sel);
                    close_menu();
                }
            }
            break;
        case MAIN_VIDEO:
        case MAIN_PERF:
        case MAIN_CONTROLS:
            if (!dir) {
                g_menu_page = row == MAIN_VIDEO ? MENU_VIDEO : row == MAIN_PERF ? MENU_PERF : MENU_CONTROLS;
                g_menu_cursor[g_menu_page] = 0;
            }
            break;
        case MAIN_RESET:
            if (!dir) {
                retro_reset();
                toast("Game reset");
                close_menu();
            }
            break;
        case MAIN_QUIT:
            if (!dir) {
                if (g_quit_confirm)
                    g_quit_requested = true;
                else
                    g_quit_confirm = true;
            }
            break;
        }
        return;
    }

    std::vector<const Setting *> items = page_settings(g_menu_page);
    int n = (int)items.size();
    if (row < n) {
        cycle_setting(*items[row], dir ? dir : 1);
    } else if (g_menu_page == MENU_CONTROLS && row == n) {
        if (!dir) {
            set_default_buttons();
            update_bindings();
            save_config();
            toast("Controls reset to default");
        }
    } else if (!dir) {
        g_menu_page = MENU_MAIN; /* Back */
    }
}

static void menu_input(u64 held, u64 down, HidAnalogStickState stick)
{
    const int dz = 0x5000;
    u64 nav = held & (HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right);
    if (stick.y > dz) nav |= HidNpadButton_Up;
    if (stick.y < -dz) nav |= HidNpadButton_Down;
    if (stick.x < -dz) nav |= HidNpadButton_Left;
    if (stick.x > dz) nav |= HidNpadButton_Right;

    /* Directions fire on press, then repeat while held. */
    u64 now = armGetSystemTick();
    u64 fire = nav & ~g_nav_prev;
    if (fire)
        g_nav_next_repeat = now + armNsToTicks(400000000ULL);
    else if (nav && nav == g_nav_prev && now >= g_nav_next_repeat) {
        fire = nav;
        g_nav_next_repeat = now + armNsToTicks(90000000ULL);
    }
    g_nav_prev = nav;

    int rows = page_rows();
    int &cur = g_menu_cursor[g_menu_page];
    if (fire & HidNpadButton_Up)   cur = (cur - 1 + rows) % rows;
    if (fire & HidNpadButton_Down) cur = (cur + 1) % rows;
    if (fire & HidNpadButton_Left)  menu_activate(cur, -1);
    if (fire & HidNpadButton_Right) menu_activate(cur, +1);
    if (down & HidNpadButton_A)     menu_activate(cur, 0);

    if (down & HidNpadButton_Minus) {
        close_menu();
    } else if (down & HidNpadButton_B) {
        if (g_menu_page == MENU_MAIN)
            close_menu();
        else
            g_menu_page = MENU_MAIN;
    }
}

#define COL_TEXT   0xE8E8E8FF
#define COL_DIM    0x9AA4B0FF
#define COL_TITLE  0xF2C35BFF
#define COL_SEL    0x2E5E8EFF
#define COL_PANEL  0x101820E8

static void draw_row(float x, float y, float w, bool sel, const char *label, const char *value, bool arrows)
{
    if (sel)
        ui_rect(x, y - 8, w, 32, COL_SEL);
    ui_text(x + 20, y, 2, COL_TEXT, label);
    if (value && *value) {
        char buf[96];
        snprintf(buf, sizeof(buf), (sel && arrows) ? "< %s >" : "%s", value);
        ui_text(x + w - 20 - ui_text_width(2, buf), y, 2, sel ? COL_TEXT : COL_DIM, buf);
    }
}

static void draw_menu(void)
{
    const float px = 250, pw = 780, py = 50, ph = 620;
    ui_rect(0, 0, UI_W, UI_H, 0x00000090);
    ui_rect(px, py, pw, ph, COL_PANEL);
    ui_rect(px, py, pw, 4, COL_TITLE);

    static const char *titles[] = { "Panzer Dragoon Saga", "Video", "Performance", "Controls" };
    ui_text(px + 20, py + 24, 3, COL_TITLE, titles[g_menu_page]);
    ui_text(px + pw - 20 - ui_text_width(1.5f, "v" PDSNX_VERSION), py + 30, 1.5f, COL_DIM, "v" PDSNX_VERSION);

    float x = px + 10, w = pw - 20, y = py + 84, rh = 36;
    int cur = g_menu_cursor[g_menu_page];
    const char *help = "";
    char v[96];

    if (g_menu_page == MENU_MAIN) {
        for (int r = 0; r < MAIN_COUNT; r++, y += rh) {
            bool sel = r == cur;
            switch (r) {
            case MAIN_RESUME: draw_row(x, y, w, sel, "Resume", "", false); break;
            case MAIN_SAVE:
                snprintf(v, sizeof(v), "Slot %d%s", g_slot, file_exists(state_path(g_slot)) ? " (used)" : "");
                draw_row(x, y, w, sel, "Save state", v, true);
                if (sel) help = "Left/Right picks the slot, A saves";
                break;
            case MAIN_LOAD:
                snprintf(v, sizeof(v), "Slot %d%s", g_slot, file_exists(state_path(g_slot)) ? "" : " (empty)");
                draw_row(x, y, w, sel, "Load state", v, true);
                if (sel) help = "Left/Right picks the slot, A loads";
                break;
            case MAIN_DISC:
                snprintf(v, sizeof(v), "Disc %d%s", g_disc_sel + 1, g_disc_sel == g_current_disc ? " (in)" : "");
                draw_row(x, y, w, sel, "Change disc", v, true);
                if (sel) help = "Pick a disc with Left/Right, A inserts it when the game asks";
                break;
            case MAIN_VIDEO: draw_row(x, y, w, sel, "Video", ">", false); break;
            case MAIN_PERF: draw_row(x, y, w, sel, "Performance", ">", false); break;
            case MAIN_CONTROLS: draw_row(x, y, w, sel, "Controls", ">", false); break;
            case MAIN_RESET:
                draw_row(x, y, w, sel, "Reset game", "", false);
                if (sel) help = "Restarts the Saturn (unsaved progress is lost)";
                break;
            case MAIN_QUIT:
                draw_row(x, y, w, sel, "Quit", g_quit_confirm ? "Press A again" : "", false);
                if (sel) help = "Back to the homebrew menu";
                break;
            }
        }
    } else {
        std::vector<const Setting *> items = page_settings(g_menu_page);
        int n = (int)items.size();
        for (int r = 0; r < n; r++, y += rh) {
            const Setting &st = *items[r];
            int ci = choice_index(st);
            const char *val = ci >= 0 ? st.choices[ci].label : g_options[st.key].c_str();
            draw_row(x, y, w, r == cur, st.label, val, true);
            if (r == cur)
                help = st.comment;
        }
        if (g_menu_page == MENU_CONTROLS) {
            draw_row(x, y, w, cur == n, "Reset to default", "", false);
            y += rh;
        }
        draw_row(x, y, w, cur == page_rows() - 1, "< Back", "", false);
    }

    if (*help) {
        /* word-wrap to the panel, at most two lines */
        const size_t maxc = (size_t)((pw - 60) / 12);
        std::string rest = help, lines[2];
        for (int l = 0; l < 2 && !rest.empty(); l++) {
            if (rest.size() <= maxc) { lines[l] = rest; rest.clear(); break; }
            size_t cut = rest.rfind(' ', maxc);
            if (cut == std::string::npos || cut == 0) cut = maxc;
            lines[l] = rest.substr(0, cut);
            rest = trim(rest.substr(cut));
        }
        ui_text(px + 30, py + ph - 100, 1.5f, COL_DIM, lines[0].c_str());
        ui_text(px + 30, py + ph - 80, 1.5f, COL_DIM, lines[1].c_str());
    }
    ui_text(px + 30, py + ph - 40, 1.5f, COL_DIM,
            g_menu_page == MENU_MAIN ? "A select   Left/Right change   B or - close"
                                     : "A or Left/Right change   B back   - close");
}

static void draw_overlay(void)
{
    if (g_options["show_fps"] == "on" && !g_menu_open) {
        char b[32];
        snprintf(b, sizeof(b), "%u FPS", g_fps);
        float tw = ui_text_width(2, b);
        ui_rect(UI_W - tw - 28, 12, tw + 16, 28, 0x00000090);
        ui_text(UI_W - tw - 20, 18, 2, COL_TEXT, b);
    }
    if (g_menu_open)
        draw_menu();
    if (g_toast[0] && armGetSystemTick() < g_toast_until) {
        float tw = ui_text_width(2, g_toast);
        ui_rect(24, UI_H - 64, tw + 32, 40, 0x000000C0);
        ui_text(40, UI_H - 52, 2, COL_TEXT, g_toast);
    }
}

/* ------------------------------------------------------------------------ */
/* Core environment                                                         */
/* ------------------------------------------------------------------------ */

static void RETRO_CALLCONV core_log(enum retro_log_level level, const char *fmt, ...)
{
    if (level < RETRO_LOG_INFO)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    nxlog("core: %s", buf);
}

static bool RETRO_CALLCONV environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = (struct retro_variable *)data;
        auto it = g_options.find(v->key);
        v->value = it != g_options.end() ? it->second.c_str() : NULL;
        return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = g_options_dirty;
        g_options_dirty = false;
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
        struct retro_hw_render_callback *hw = (struct retro_hw_render_callback *)data;
        nxlog("core: wants GL context type %d, version %u.%u", hw->context_type,
              hw->version_major, hw->version_minor);
        hw->get_current_framebuffer = hw_get_current_framebuffer;
        hw->get_proc_address = hw_get_proc_address;
        g_hw = *hw;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
        const struct retro_game_geometry *g = (const struct retro_game_geometry *)data;
        nxlog("video: core output %ux%u", g->base_width, g->base_height);
        if (g_fbo && (g->base_width > g_fbo_w || g->base_height > g_fbo_h))
            create_fbo(std::max(g->base_width, g_fbo_w), std::max(g->base_height, g_fbo_h));
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = BASE_DIR;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = BASE_DIR "/saves";
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
        *(bool *)data = false;
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------------ */
/* Main                                                                     */
/* ------------------------------------------------------------------------ */

/* Runs on every way out of main: give the clocks back and unmap code memory
 * (the homebrew loader crashes if code memory is left mapped). */
static void at_exit_cleanup(void)
{
    nx_clock_restore();
    nx_jit_shutdown();
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    mkdir(BASE_DIR, 0777);
    mkdir(BASE_DIR "/discs", 0777);
    mkdir(BASE_DIR "/saves", 0777);
    mkdir(BASE_DIR "/states", 0777);
    nxlog_open(BASE_DIR "/pds-nx.log");
    nxlog("pds-nx " PDSNX_VERSION " starting");

    /* Keep the main (emulation) thread on core 0; see thr-rthreads.c. */
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 1u << 0);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    AppletType at = appletGetAppletType();
    nxlog("applet type %d", (int)at);
    if (at != AppletType_Application && at != AppletType_SystemApplication) {
        fatal_screen("  This needs full RAM access.\n\n"
                     "  Launch it by holding R while starting any game,\n"
                     "  then pick pds-nx from the homebrew menu.\n"
                     "  (Opening it from the Album does not work.)");
        nxlog_close();
        return 0;
    }

    set_default_options();
    load_config();
    update_bindings();
    g_debug_log = g_options["debug_log"] == "on";

    nx_clock_apply(atoi(g_options["cpu_mhz"].c_str()), atoi(g_options["gpu_mhz"].c_str()));
    atexit(at_exit_cleanup);

    std::string bios = find_bios();
    if (bios.empty()) {
        fatal_screen("  Saturn BIOS not found.\n\n"
                     "  Copy your Saturn BIOS to:\n"
                     "    /switch/pds-nx/saturn_bios.bin\n"
                     "  (sega_101.bin or mpr-17933.bin also work)");
        nxlog_close();
        return 0;
    }
    nxlog("bios: %s", bios.c_str());

    find_discs();
    if (g_disc_count == 0) {
        fatal_screen("  No discs found.\n\n"
                     "  Copy your Panzer Dragoon Saga discs (.cue + .bin, or .chd)\n"
                     "  into /switch/pds-nx/discs/");
        nxlog_close();
        return 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fatal_screen("  SDL_Init failed: %s", SDL_GetError());
        nxlog_close();
        return 0;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    g_window = SDL_CreateWindow("pds-nx", 0, 0, 1920, 1080, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
    if (!g_window) {
        fatal_screen("  Could not create the window: %s", SDL_GetError());
        SDL_Quit();
        nxlog_close();
        return 0;
    }
    g_gl = SDL_GL_CreateContext(g_window);
    if (!g_gl) {
        fatal_screen("  Could not create the GL context: %s", SDL_GetError());
        SDL_Quit();
        nxlog_close();
        return 0;
    }
    SDL_GL_MakeCurrent(g_window, g_gl);
    g_audio_sync = g_options["sync"] != "video";
    SDL_GL_SetSwapInterval(g_audio_sync ? 0 : 1);
    nxlog("gl: %s | %s | %s", glGetString(GL_VERSION), glGetString(GL_RENDERER),
          glGetString(GL_SHADING_LANGUAGE_VERSION));
    create_fbo(1920, 1080);
    ui_init();

    open_audio();

    retro_set_environment(environment);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_sample_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();

    struct retro_game_info info;
    memset(&info, 0, sizeof(info));
    info.path = g_discs[0].c_str();
    g_current_disc = 0;
    if (!retro_load_game(&info)) {
        nxlog_flush();
        fatal_screen("  The emulator could not load disc 1.\n\n  Details are in /switch/pds-nx/pds-nx.log");
        SDL_Quit();
        nxlog_close();
        return 0;
    }
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    /* The core sets up its renderer when the GL context is handed over. */
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    pdsnx_resolve_private_gl([](const char *n) -> void * { return SDL_GL_GetProcAddress(n); });
    nxlog("gl: compute entry points %s", SDL_GL_GetProcAddress("glDispatchCompute") ? "found" : "missing");
    if (g_hw.context_reset)
        g_hw.context_reset();
    toast("Tap - for the menu");
    g_toast_until = armGetSystemTick() + armNsToTicks(6000000000ULL);
    nxlog("running");
    nxlog_flush();

    /* The watchdog lives on core 2 at a higher priority, so it keeps logging
     * even if the emulator stops making progress. */
    g_watchdog_run = true;
    if (R_SUCCEEDED(threadCreate(&g_watchdog, watchdog_main, NULL, NULL, 0x10000, 0x2B, 2)))
        threadStart(&g_watchdog);

    bool running = true;
    u64 last_flush = armGetSystemTick();
    unsigned fps_frames = 0;
    while (running && appletMainLoop()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                running = false;
        }

        padUpdate(&g_pad);
        u64 held = padGetButtons(&g_pad);
        u64 down = padGetButtonsDown(&g_pad);
        HidAnalogStickState stick = padGetStickPos(&g_pad, 0);

        /* Once a second: write any new in-game saves to the SD card, keep the
         * clocks where we set them, update the FPS counter, flush the log. */
        u64 now = armGetSystemTick();
        if (armTicksToNs(now - last_flush) > 1000000000ULL) {
            if (pdsnx_backup_flush())
                nxlog("saves: wrote save memory");
            nx_clock_maintain();
            g_fps = g_frames_shown - fps_frames;
            fps_frames = g_frames_shown;
            nxlog_flush();
            last_flush = now;
        }

        if (g_menu_open) {
            menu_input(held, down, stick);
            if (g_quit_requested)
                break;
            present_inner();
            if (g_audio_sync)
                svcSleepThread(16000000LL);
            continue;
        }
        if (down & HidNpadButton_Minus) {
            open_menu();
            continue;
        }
        g_retropad = map_buttons(held, stick);

        g_phase = 1;
        u64 t_run = armGetSystemTick();
        retro_run();
        g_us_run += ticks_us(armGetSystemTick() - t_run);
        g_phase = 3;
        audio_wait();
        g_loop_iter++;
    }

    nxlog("exiting");
    g_watchdog_run = false;
    threadWaitForExit(&g_watchdog);
    threadClose(&g_watchdog);
    retro_unload_game();
    retro_deinit();
    if (g_audio)
        SDL_CloseAudioDevice(g_audio);
    destroy_fbo();
    SDL_GL_DeleteContext(g_gl);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    nxlog_close();
    return 0;
}
