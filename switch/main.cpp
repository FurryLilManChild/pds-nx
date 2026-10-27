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

#include "nx_log.h"

extern "C" {
void pdsnx_open_tray(void);
int pdsnx_close_tray(const char *path);
int pdsnx_backup_flush(void);
}

#define BASE_DIR "sdmc:/switch/pds-nx"

/* ------------------------------------------------------------------------ */
/* Settings                                                                 */
/* ------------------------------------------------------------------------ */

static std::map<std::string, std::string> g_options;
static bool g_options_dirty;

static const char *const k_default_config =
    "# pds-nx settings. Edit on your PC or with a Switch text editor.\n"
    "# Lines starting with # are ignored. Changes apply the next time you launch.\n"
    "\n"
    "# Screen shape: 4:3 (original) or 16:9 (stretched to fill)\n"
    "aspect=4:3\n"
    "\n"
    "# Scaling filter when drawing to the screen: linear or nearest\n"
    "filter=linear\n"
    "\n"
    "# Internal rendering resolution: original, 2x, 720p, 1080p\n"
    "yabasanshiro_resolution_mode=original\n"
    "\n"
    "# Rotating background (sky and floor planes) resolution: original, 2x, 720p, 1080p, Fit_to_emulation\n"
    "yabasanshiro_rbg_resolution_mode=original\n"
    "\n"
    "# Use compute shaders for rotating backgrounds: disabled or enabled\n"
    "yabasanshiro_rbg_use_compute_shader=disabled\n"
    "\n"
    "# perspective_correction removes the Saturn's warped textures; gpu_tesselation and cpu_tesselation are alternatives\n"
    "yabasanshiro_polygon_mode=perspective_correction\n"
    "\n"
    "# Skip frames automatically when the Switch can't keep up: disabled or enabled\n"
    "yabasanshiro_frameskip=disabled\n"
    "\n"
    "# SH-2 CPU emulation: dynarec (fast) or interpreter (slow, for troubleshooting)\n"
    "yabasanshiro_sh2coretype=dynarec\n"
    "\n"
    "# Saturn system language: english, deutsch, french, spanish, italian, japanese\n"
    "yabasanshiro_system_language=english\n";

static void set_default_options(void)
{
    g_options["aspect"] = "4:3";
    g_options["filter"] = "linear";
    g_options["yabasanshiro_force_hle_bios"] = "disabled";
    g_options["yabasanshiro_frameskip"] = "disabled";
    g_options["yabasanshiro_addon_cart"] = "4M_extended_ram";
    g_options["yabasanshiro_system_language"] = "english";
    g_options["yabasanshiro_multitap_port1"] = "disabled";
    g_options["yabasanshiro_multitap_port2"] = "disabled";
    g_options["yabasanshiro_sh2coretype"] = "dynarec";
    g_options["yabasanshiro_polygon_mode"] = "perspective_correction";
    g_options["yabasanshiro_resolution_mode"] = "original";
    g_options["yabasanshiro_rbg_resolution_mode"] = "original";
    g_options["yabasanshiro_rbg_use_compute_shader"] = "disabled";
}

static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static void load_config(void)
{
    const char *path = BASE_DIR "/config.ini";
    FILE *f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (f) {
            fputs(k_default_config, f);
            fclose(f);
        }
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

static void present(void)
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
    SDL_GL_SwapWindow(g_window);

    /* Hand the core back its own frame buffer and state. */
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
}

static void RETRO_CALLCONV video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
    (void)pitch;
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

static size_t RETRO_CALLCONV audio_sample_batch(const int16_t *data, size_t frames)
{
    if (!g_audio)
        return frames;
    /* Keep latency bounded: drop audio if more than ~100 ms is waiting. */
    if (SDL_GetQueuedAudioSize(g_audio) < 44100 * 4 / 10)
        SDL_QueueAudio(g_audio, data, (Uint32)(frames * 4));
    return frames;
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
    nxlog("audio: %d Hz, %d channels, %d samples", have.freq, have.channels, have.samples);
    SDL_PauseAudioDevice(g_audio, 0);
}

/* ------------------------------------------------------------------------ */
/* Input                                                                    */
/* ------------------------------------------------------------------------ */

static PadState g_pad;
static uint16_t g_retropad;  /* RETRO_DEVICE_ID_JOYPAD_* bits for player 1 */

/*
 * Default layout. The Saturn pad has A B C on the bottom row and X Y Z above.
 *   Saturn A <- Switch Y      Saturn X <- ZL     Saturn L <- L
 *   Saturn B <- Switch B      Saturn Y <- X      Saturn R <- R
 *   Saturn C <- Switch A      Saturn Z <- ZR     Start    <- +
 * The core routes RetroPad buttons to Saturn ones as: B->A, A->B, L->C,
 * Y->X, X->Y, R->Z, L2->L, R2->R.
 */
static uint16_t map_buttons(u64 b, HidAnalogStickState stick)
{
    uint16_t m = 0;
    const int dz = 0x4000;
    if ((b & HidNpadButton_Up)    || stick.y >  dz) m |= 1 << RETRO_DEVICE_ID_JOYPAD_UP;
    if ((b & HidNpadButton_Down)  || stick.y < -dz) m |= 1 << RETRO_DEVICE_ID_JOYPAD_DOWN;
    if ((b & HidNpadButton_Left)  || stick.x < -dz) m |= 1 << RETRO_DEVICE_ID_JOYPAD_LEFT;
    if ((b & HidNpadButton_Right) || stick.x >  dz) m |= 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if (b & HidNpadButton_Y)  m |= 1 << RETRO_DEVICE_ID_JOYPAD_B;  /* Saturn A */
    if (b & HidNpadButton_B)  m |= 1 << RETRO_DEVICE_ID_JOYPAD_A;  /* Saturn B */
    if (b & HidNpadButton_A)  m |= 1 << RETRO_DEVICE_ID_JOYPAD_L;  /* Saturn C */
    if (b & HidNpadButton_ZL) m |= 1 << RETRO_DEVICE_ID_JOYPAD_Y;  /* Saturn X */
    if (b & HidNpadButton_X)  m |= 1 << RETRO_DEVICE_ID_JOYPAD_X;  /* Saturn Y */
    if (b & HidNpadButton_ZR) m |= 1 << RETRO_DEVICE_ID_JOYPAD_R;  /* Saturn Z */
    if (b & HidNpadButton_L)  m |= 1 << RETRO_DEVICE_ID_JOYPAD_L2; /* Saturn L */
    if (b & HidNpadButton_R)  m |= 1 << RETRO_DEVICE_ID_JOYPAD_R2; /* Saturn R */
    if (b & HidNpadButton_Plus) m |= 1 << RETRO_DEVICE_ID_JOYPAD_START;
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

static void save_state(int slot)
{
    size_t size = retro_serialize_size();
    if (!size) {
        nxlog("state: not available yet");
        return;
    }
    std::vector<uint8_t> buf(size);
    if (!retro_serialize(buf.data(), size)) {
        nxlog("state: serialize failed");
        return;
    }
    char path[256];
    snprintf(path, sizeof(path), BASE_DIR "/states/slot%d.state", slot);
    FILE *f = fopen(path, "wb");
    if (!f) {
        nxlog("state: cannot write %s", path);
        return;
    }
    fwrite(buf.data(), 1, size, f);
    fclose(f);
    nxlog("state: saved slot %d (%zu bytes)", slot, size);
}

static void load_state(int slot)
{
    char path[256];
    snprintf(path, sizeof(path), BASE_DIR "/states/slot%d.state", slot);
    FILE *f = fopen(path, "rb");
    if (!f) {
        nxlog("state: no %s", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(size > 0 ? size : 0);
    size_t got = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (got == buf.size() && retro_unserialize(buf.data(), buf.size()))
        nxlog("state: loaded slot %d", slot);
    else
        nxlog("state: load failed for slot %d", slot);
}

static void change_disc(int index)
{
    if (index < 0 || index >= g_disc_count || g_discs[index].empty() || index == g_current_disc)
        return;
    nxlog("disc: switching to disc %d", index + 1);
    pdsnx_open_tray();
    int rc = pdsnx_close_tray(g_discs[index].c_str());
    if (rc == 0)
        g_current_disc = index;
    else
        nxlog("disc: change failed (%d)", rc);
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

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    mkdir(BASE_DIR, 0777);
    mkdir(BASE_DIR "/discs", 0777);
    mkdir(BASE_DIR "/saves", 0777);
    mkdir(BASE_DIR "/states", 0777);
    nxlog_open(BASE_DIR "/pds-nx.log");
    nxlog("pds-nx starting");

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
    SDL_GL_SetSwapInterval(1);
    nxlog("gl: %s | %s | %s", glGetString(GL_VERSION), glGetString(GL_RENDERER),
          glGetString(GL_SHADING_LANGUAGE_VERSION));
    create_fbo(1920, 1080);

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
    if (g_hw.context_reset)
        g_hw.context_reset();
    nxlog("running");
    nxlog_flush();

    bool running = true;
    u64 last_flush = armGetSystemTick();
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

        /* Hold - (Minus) for shortcuts; the game doesn't see buttons meanwhile. */
        if (held & HidNpadButton_Minus) {
            g_retropad = 0;
            if (down & HidNpadButton_R) save_state(0);
            if (down & HidNpadButton_L) load_state(0);
            if (down & HidNpadButton_Right) change_disc(g_current_disc + 1);
            if (down & HidNpadButton_Left) change_disc(g_current_disc - 1);
            if (down & HidNpadButton_Plus) running = false;
        } else {
            g_retropad = map_buttons(held, stick);
        }

        retro_run();

        /* Once a second: write any new in-game saves to the SD card, flush the log. */
        u64 now = armGetSystemTick();
        if (armTicksToNs(now - last_flush) > 1000000000ULL) {
            if (pdsnx_backup_flush())
                nxlog("saves: wrote save memory");
            nxlog_flush();
            last_flush = now;
        }
    }

    nxlog("exiting");
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
