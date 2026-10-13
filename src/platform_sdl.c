/* model3recomp -- SDL2 platform layer: the window, its menu, the controls.
 *
 * Settings live in an ini file (m3_config_t.ini_path) and are written back
 * whenever they change. On Windows the window carries a menu bar; elsewhere
 * the same settings are reachable through the ini and the hotkeys.
 *
 *   F5 save state   F6 next slot   F7 load state   F11 / Alt+Enter fullscreen
 *   Tab (held) run flat out        Esc leave fullscreen, or quit
 */
#include "model3recomp/platform.h"
#include "model3recomp/model3recomp.h"
#include "model3recomp/savestate.h"
#include "model3recomp/netplay.h"

#include <SDL.h>
#include <SDL_syswm.h>
#ifdef __ANDROID__
#include "model3recomp/bus.h"
#include "touch.h"
#include <jni.h>
#include <math.h>

static m3_touch g_touch;               /* the touch controls (bottom of the controls section) */
static void touch_event(const SDL_Event *e);
static void touch_draw(void);
static void nvram_keep(void);
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture  *g_tex;
static SDL_GameController *g_pad;
static uint32_t     *g_fb;
static int           g_w, g_h;
static char          g_title[128];
static char          g_net_status[160];
static char          g_note[160];
static uint64_t      g_note_until;

/* ---- settings --------------------------------------------------------- */
enum { CUR_HIDDEN, CUR_CROSSHAIR, CUR_SYSTEM };

static struct {
    int scale;          /* window size, multiples of the board's 496x384 */
    int fullscreen;
    int smooth;         /* linear filtering rather than sharp pixels */
    int scanlines;
    int aspect43;       /* 4:3, as the cabinet's monitor showed it */
    int cursor;         /* CUR_* */
    int mouse;          /* the mouse aims player 1 */
    int pad;            /* gamepad: 0 off, 1 player 1, 2 player 2 */
    int sinden;         /* white border for a Sinden gun: 0 off, 1 thin, 2 thick */
    int slot;           /* save state slot, 1..9 */
    int net_delay;      /* fields of input delay when hosting */
    int net_port;
    char net_join[128]; /* last address joined */
    int volume;         /* percent */
    int mute;
} opt = { 2, 0, 0, 0, 1, CUR_HIDDEN, 1, 2, 0, 1, 3, 7777, "", 100, 0 };

static const struct { const char *key; int *v; } k_int[] = {
    { "scale", &opt.scale }, { "fullscreen", &opt.fullscreen },
    { "smooth", &opt.smooth }, { "scanlines", &opt.scanlines },
    { "aspect43", &opt.aspect43 }, { "cursor", &opt.cursor },
    { "mouse", &opt.mouse }, { "pad", &opt.pad }, { "sinden", &opt.sinden },
    { "slot", &opt.slot }, { "net_delay", &opt.net_delay },
    { "net_port", &opt.net_port }, { "volume", &opt.volume }, { "mute", &opt.mute },
};

static const char *ini_path(void)
{
    const char *p = model3recomp_config()->ini_path;
    return p ? p : "model3recomp.ini";
}

static void settings_load(void)
{
    char line[256];
    FILE *f = fopen(ini_path(), "r");
    unsigned i;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *nl;
        if (!eq) continue;
        *eq++ = 0;
        if ((nl = strpbrk(eq, "\r\n"))) *nl = 0;
        for (i = 0; i < sizeof k_int / sizeof k_int[0]; i++)
            if (!strcmp(line, k_int[i].key)) *k_int[i].v = atoi(eq);
        if (!strcmp(line, "net_join"))
            snprintf(opt.net_join, sizeof opt.net_join, "%s", eq);
        if (!strcmp(line, "game_options"))
            m3_options_set_local_all((uint32_t)strtoul(eq, NULL, 0));
    }
    fclose(f);
    if (opt.scale < 1 || opt.scale > 4) opt.scale = 2;
    if (opt.slot < 1 || opt.slot > 9) opt.slot = 1;
    if (opt.net_delay < 1 || opt.net_delay > 10) opt.net_delay = 3;
    if (opt.volume < 0 || opt.volume > 100) opt.volume = 100;
}

static void settings_save(void)
{
    FILE *f = fopen(ini_path(), "w");
    unsigned i;
    if (!f) return;
    for (i = 0; i < sizeof k_int / sizeof k_int[0]; i++)
        fprintf(f, "%s=%d\n", k_int[i].key, *k_int[i].v);
    fprintf(f, "net_join=%s\n", opt.net_join);
    fprintf(f, "game_options=0x%08X\n", (unsigned)m3_options_local_all());
    fclose(f);
}

/* ---- title bar -------------------------------------------------------- */
static void update_title(void)
{
    char t[512];
    const char *note = SDL_GetTicks64() < g_note_until ? g_note : "";
    snprintf(t, sizeof t, "%s%s%s%s%s", g_title,
             g_net_status[0] ? "  -  " : "", g_net_status,
             note[0] ? "  -  " : "", note);
    if (g_win) SDL_SetWindowTitle(g_win, t);
}

void platform_set_status(const char *s)
{
    /* Netplay's line stays; everything else is a note that fades. */
    if (!strncmp(s, "netplay", 7)) {
        snprintf(g_net_status, sizeof g_net_status, "%s", s);
    } else {
        snprintf(g_note, sizeof g_note, "%s", s);
        g_note_until = SDL_GetTicks64() + 3000;
    }
    update_title();
}

/* ---- where the picture goes ------------------------------------------- */
static SDL_Rect view_rect(void)
{
    SDL_Rect r;
    int ow = 1, oh = 1;
    double aspect = opt.aspect43 ? 4.0 / 3.0 : (double)g_w / g_h;
    SDL_GetRendererOutputSize(g_ren, &ow, &oh);
    if ((double)ow / oh > aspect) { r.h = oh; r.w = (int)(oh * aspect + 0.5); }
    else                          { r.w = ow; r.h = (int)(ow / aspect + 0.5); }
    r.x = (ow - r.w) / 2;
    r.y = (oh - r.h) / 2;
    return r;
}

static void apply_video(void)
{
    if (!g_win) return;
    SDL_SetWindowFullscreen(g_win, opt.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (!opt.fullscreen) {
        int h = g_h * opt.scale;
        int w = opt.aspect43 ? h * 4 / 3 : g_w * opt.scale;
        SDL_SetWindowSize(g_win, w, h);
    }
    SDL_SetTextureScaleMode(g_tex, opt.smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    SDL_ShowCursor(opt.cursor == CUR_SYSTEM ? SDL_ENABLE : SDL_DISABLE);
}

/* ---- menu (Windows) ---------------------------------------------------- */
#ifdef _WIN32
enum {
    ID_SAVE = 100, ID_LOAD, ID_RESET, ID_QUIT,
    ID_SLOT = 110,                      /* +1..9 */
    ID_SCALE = 130,                     /* +1..4 */
    ID_FULL = 140, ID_SHARP, ID_SMOOTH, ID_SCAN, ID_ASPECT43, ID_ASPECTSQ,
    ID_MOUSE = 160, ID_PAD = 170, ID_CURSOR = 180, ID_SINDEN = 190,
    ID_CHEAT = 200,                     /* +bit */
    ID_OPTION = 240,                    /* +slot*16+choice */
    ID_MUTE = 380, ID_VOL = 381,        /* +0..4: 100, 75, 50, 25 */
    ID_HOST = 400, ID_JOIN, ID_DISCONNECT,
    ID_DELAY = 410                      /* +1..10 */
};

static HWND hwnd(void)
{
    SDL_SysWMinfo i;
    SDL_VERSION(&i.version);
    return SDL_GetWindowWMInfo(g_win, &i) ? i.info.win.window : NULL;
}

static void item(HMENU m, UINT id, const char *label, int checked)
{
    AppendMenuA(m, MF_STRING | (checked ? MF_CHECKED : 0), id, label);
}

static void note_item(HMENU m, const char *label)
{
    AppendMenuA(m, MF_STRING | MF_GRAYED, 0, label);
}

static void build_menu(void)
{
    HWND h = hwnd();
    HMENU bar, file, slot, video, size, sound, ctl, pad, cur, sind, dbg, mp, dly;
    HMENU old;
    char b[64];
    int i, any = 0;

    if (!h) return;
    bar = CreateMenu();

    file = CreatePopupMenu(); slot = CreatePopupMenu();
    item(file, ID_SAVE, "&Save state\tF5", 0);
    item(file, ID_LOAD, "&Load state\tF7", 0);
    for (i = 1; i <= 9; i++) {
        snprintf(b, sizeof b, "Slot %d%s", i, m3_state_exists(i) ? "" : "  (empty)");
        item(slot, ID_SLOT + i, b, opt.slot == i);
    }
    AppendMenuA(file, MF_POPUP, (UINT_PTR)slot, "Sl&ot\tF6");
    AppendMenuA(file, MF_SEPARATOR, 0, NULL);
    item(file, ID_RESET, "&Reset", 0);
    item(file, ID_QUIT, "&Quit\tEsc", 0);
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)file, "&File");

    video = CreatePopupMenu(); size = CreatePopupMenu();
    for (i = 1; i <= 4; i++) {
        snprintf(b, sizeof b, "%dx  (%d x %d)", i, opt.aspect43 ? g_h * i * 4 / 3 : g_w * i, g_h * i);
        item(size, ID_SCALE + i, b, opt.scale == i);
    }
    AppendMenuA(video, MF_POPUP, (UINT_PTR)size, "&Window size");
    item(video, ID_FULL, "&Fullscreen\tF11", opt.fullscreen);
    AppendMenuA(video, MF_SEPARATOR, 0, NULL);
    item(video, ID_SHARP, "Sharp pixels", !opt.smooth);
    item(video, ID_SMOOTH, "Smooth (bilinear)", opt.smooth);
    item(video, ID_SCAN, "Scanlines", opt.scanlines);
    AppendMenuA(video, MF_SEPARATOR, 0, NULL);
    item(video, ID_ASPECT43, "4:3, as on the cabinet", opt.aspect43);
    item(video, ID_ASPECTSQ, "Square pixels (496 x 384)", !opt.aspect43);
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)video, "&Video");

    sound = CreatePopupMenu();
    item(sound, ID_MUTE, "&Mute\tF9", opt.mute);
    AppendMenuA(sound, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < 4; i++) {
        static const int lv[4] = { 100, 75, 50, 25 };
        snprintf(b, sizeof b, "Volume %d%%", lv[i]);
        item(sound, ID_VOL + i, b, opt.volume == lv[i]);
    }
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)sound, "&Sound");

    ctl = CreatePopupMenu(); pad = CreatePopupMenu(); cur = CreatePopupMenu(); sind = CreatePopupMenu();
    item(ctl, ID_MOUSE, "&Mouse aims player 1", opt.mouse);
    item(pad, ID_PAD + 0, "Off", opt.pad == 0);
    item(pad, ID_PAD + 1, "Player 1", opt.pad == 1);
    item(pad, ID_PAD + 2, "Player 2", opt.pad == 2);
    AppendMenuA(ctl, MF_POPUP, (UINT_PTR)pad, g_pad ? "&Gamepad" : "&Gamepad  (none connected)");
    item(cur, ID_CURSOR + CUR_HIDDEN, "Hidden, as on the cabinet", opt.cursor == CUR_HIDDEN);
    item(cur, ID_CURSOR + CUR_CROSSHAIR, "Crosshair", opt.cursor == CUR_CROSSHAIR);
    item(cur, ID_CURSOR + CUR_SYSTEM, "Mouse pointer", opt.cursor == CUR_SYSTEM);
    AppendMenuA(ctl, MF_POPUP, (UINT_PTR)cur, "&Cursor");
    item(sind, ID_SINDEN + 0, "Off", opt.sinden == 0);
    item(sind, ID_SINDEN + 1, "Thin", opt.sinden == 1);
    item(sind, ID_SINDEN + 2, "Thick", opt.sinden == 2);
    AppendMenuA(ctl, MF_POPUP, (UINT_PTR)sind, "&Sinden light gun border");
    AppendMenuA(ctl, MF_SEPARATOR, 0, NULL);
    note_item(ctl, "Mouse: left fires, right reloads (points off screen)");
    note_item(ctl, "Gamepad: left stick aims, A/RT fire, B/LT reload, Start, Back = coin");
    note_item(ctl, "Keys: 5/6 coin, 1/2 start, Space fire, Shift reload, F2 test, F3 service");
    note_item(ctl, "Sinden: run its software in mouse mode, off-screen reload = right click");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)ctl, "&Controls");

    dbg = CreatePopupMenu();
    for (i = 0; i < 32; i++) {
        const char *l = m3_cheat_label((unsigned)i);
        if (!l) continue;
        item(dbg, ID_CHEAT + i, l, !m3_cheat_is_action((unsigned)i) && ((m3_cheats_local() >> i) & 1u));
        any = 1;
    }
    if (!any) note_item(dbg, "This game offers no cheats");
    if (netplay_role() == 2) note_item(dbg, "(during netplay the host's cheats apply)");
    {
        unsigned sl, c;
        for (sl = 0; sl < 8u; sl++) {
            HMENU sub;
            char lbl[96];
            if (!m3_option_label(sl)) continue;
            sub = CreatePopupMenu();
            for (c = 0; c < m3_option_count(sl); c++)
                AppendMenuA(sub, MF_STRING | (m3_option_local(sl) == c ? MF_CHECKED : 0)
                                 | (netplay_active() ? MF_GRAYED : 0),
                            ID_OPTION + sl * 16u + c, m3_option_choice(sl, c));
            if (netplay_active()) note_item(sub, "(not during netplay: changing it restarts the game)");
            snprintf(lbl, sizeof lbl, "%s  (restarts the game)", m3_option_label(sl));
            if (sl == 0) AppendMenuA(dbg, MF_SEPARATOR, 0, NULL);
            AppendMenuA(dbg, MF_POPUP, (UINT_PTR)sub, lbl);
        }
    }
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)dbg, "&Debug");

    mp = CreatePopupMenu(); dly = CreatePopupMenu();
    AppendMenuA(mp, MF_STRING | (netplay_active() ? MF_GRAYED : 0), ID_HOST, "&Host a game...");
    AppendMenuA(mp, MF_STRING | (netplay_active() ? MF_GRAYED : 0), ID_JOIN, "&Join a game...");
    AppendMenuA(mp, MF_STRING | (netplay_active() ? 0 : MF_GRAYED), ID_DISCONNECT, "&Disconnect");
    for (i = 1; i <= 10; i++) {
        snprintf(b, sizeof b, "%d field%s%s", i, i == 1 ? "" : "s", i <= 2 ? "  (LAN)" : i >= 5 ? "  (far away)" : "");
        item(dly, ID_DELAY + i, b, opt.net_delay == i);
    }
    AppendMenuA(mp, MF_POPUP, (UINT_PTR)dly, "Input &delay when hosting");
    AppendMenuA(mp, MF_SEPARATOR, 0, NULL);
    note_item(mp, netplay_active() ? netplay_status() : "Not connected");
    note_item(mp, "Host plays player 1; the game restarts for both when you connect");
    note_item(mp, "LAN, a Tailscale address, or a forwarded TCP port all work");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)mp, "&Multiplayer");

    old = GetMenu(h);
    SetMenu(h, bar);
    if (old) DestroyMenu(old);
}

/* A one-line text prompt, built in memory so there is no resource file. */
static WCHAR g_prompt_buf[256];
static const WCHAR *g_prompt_label;

static INT_PTR CALLBACK prompt_proc(HWND d, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_INITDIALOG:
        SetDlgItemTextW(d, 10, g_prompt_label);
        SetDlgItemTextW(d, 11, g_prompt_buf);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            GetDlgItemTextW(d, 11, g_prompt_buf, 256);
            EndDialog(d, 1);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) { EndDialog(d, 0); return TRUE; }
        break;
    }
    return FALSE;
}

static WORD *dlg_str(WORD *p, const WCHAR *s)
{
    while ((*p++ = (WORD)*s++) != 0) {}
    return p;
}

static WORD *dlg_item(WORD *p, DWORD style, short x, short y, short cx, short cy,
                      WORD id, WORD cls, const WCHAR *text)
{
    DLGITEMTEMPLATE *it;
    p = (WORD *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
    it = (DLGITEMTEMPLATE *)p;
    it->style = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    p = (WORD *)(it + 1);
    *p++ = 0xFFFF; *p++ = cls;
    p = dlg_str(p, text);
    *p++ = 0;
    return p;
}

static int prompt(const WCHAR *title, const WCHAR *label, char *io, size_t n)
{
    static WORD buf[1024];
    DLGTEMPLATE *t = (DLGTEMPLATE *)buf;
    WORD *p;
    INT_PTR r;

    memset(buf, 0, sizeof buf);
    t->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    t->cdit = 4;
    t->x = 0; t->y = 0; t->cx = 240; t->cy = 64;
    p = (WORD *)(t + 1);
    *p++ = 0; *p++ = 0;                     /* no menu, default class */
    p = dlg_str(p, title);
    *p++ = 9;                               /* font size */
    p = dlg_str(p, L"Segoe UI");
    p = dlg_item(p, SS_LEFT, 8, 6, 224, 10, 10, 0x0082, L"");
    p = dlg_item(p, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 20, 224, 13, 11, 0x0081, L"");
    p = dlg_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, 128, 42, 50, 14, IDOK, 0x0080, L"OK");
    (void)dlg_item(p, BS_PUSHBUTTON | WS_TABSTOP, 182, 42, 50, 14, IDCANCEL, 0x0080, L"Cancel");

    g_prompt_label = label;
    MultiByteToWideChar(CP_UTF8, 0, io, -1, g_prompt_buf, 256);
    r = DialogBoxIndirectW(GetModuleHandleW(NULL), t, hwnd(), prompt_proc);
    if (r != 1) return 0;
    WideCharToMultiByte(CP_UTF8, 0, g_prompt_buf, -1, io, (int)n, NULL, NULL);
    return 1;
}

/* Start over as a new process: the guest cannot be reset in place, and a
 * netplay session has to begin at power-on on both sides. */
static void relaunch(const char *netplay, int delay)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR *cmd = _wcsdup(GetCommandLineW());
    char d[16];

    SetEnvironmentVariableA("M3_NETPLAY", netplay && *netplay ? netplay : NULL);
    SetEnvironmentVariableA("M3_RELAUNCHED", "1");   /* ignore --host/--join now */
    snprintf(d, sizeof d, "%d", delay);
    SetEnvironmentVariableA("M3_NET_DELAY", d);
    settings_save();
    netplay_shutdown();
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    /* Battery RAM first, so the new process boots with it. */
    model3recomp_save_nvram();
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        model3recomp_quit(0);
    }
    free(cmd);
    platform_set_status("could not restart");
}

static int on_command(unsigned id)
{
    if (id == ID_MUTE) opt.mute = !opt.mute;
    else if (id >= ID_VOL && id < ID_VOL + 4) {
        static const int lv[4] = { 100, 75, 50, 25 };
        opt.volume = lv[id - ID_VOL]; opt.mute = 0;
    }
    else if (id == ID_SAVE) m3_state_request_save(opt.slot);
    else if (id == ID_LOAD) m3_state_request_load(opt.slot);
    else if (id > ID_SLOT && id <= ID_SLOT + 9) opt.slot = (int)(id - ID_SLOT);
    else if (id == ID_RESET) relaunch(NULL, opt.net_delay);
    else if (id == ID_QUIT) return 0;
    else if (id > ID_SCALE && id <= ID_SCALE + 4) { opt.scale = (int)(id - ID_SCALE); opt.fullscreen = 0; }
    else if (id == ID_FULL) opt.fullscreen = !opt.fullscreen;
    else if (id == ID_SHARP) opt.smooth = 0;
    else if (id == ID_SMOOTH) opt.smooth = 1;
    else if (id == ID_SCAN) opt.scanlines = !opt.scanlines;
    else if (id == ID_ASPECT43) opt.aspect43 = 1;
    else if (id == ID_ASPECTSQ) opt.aspect43 = 0;
    else if (id == ID_MOUSE) opt.mouse = !opt.mouse;
    else if (id >= ID_PAD && id <= ID_PAD + 2) opt.pad = (int)(id - ID_PAD);
    else if (id >= ID_CURSOR && id <= ID_CURSOR + 2) opt.cursor = (int)(id - ID_CURSOR);
    else if (id >= ID_SINDEN && id <= ID_SINDEN + 2) opt.sinden = (int)(id - ID_SINDEN);
    else if (id >= ID_CHEAT && id < ID_CHEAT + 32) {
        unsigned bit = id - ID_CHEAT;
        if (m3_cheat_is_action(bit)) m3_cheat_pulse(bit);
        else m3_cheats_set_local(m3_cheats_local() ^ (1u << bit));
    }
    else if (id >= ID_OPTION && id < ID_OPTION + 128 && !netplay_active()) {
        unsigned sl = (id - ID_OPTION) / 16u, v = (id - ID_OPTION) % 16u;
        /* A game option is the board's configuration, so it is applied the
         * way an operator would: change it and power the machine back on.
         * relaunch() saves the settings first. */
        if (m3_option_local(sl) != v) {
            m3_option_set_local(sl, v);
            relaunch(NULL, opt.net_delay);
        }
    }
    else if (id > ID_DELAY && id <= ID_DELAY + 10) opt.net_delay = (int)(id - ID_DELAY);
    else if (id == ID_DISCONNECT) { netplay_shutdown(); platform_set_status("netplay: disconnected"); }
    else if (id == ID_HOST) {
        char port[32], spec[64];
        snprintf(port, sizeof port, "%d", opt.net_port);
        if (prompt(L"Host a game", L"Port to listen on (TCP). The game restarts and waits for player 2.", port, sizeof port)) {
            opt.net_port = atoi(port);
            snprintf(spec, sizeof spec, "host:%d", opt.net_port);
            relaunch(spec, opt.net_delay);
        }
    } else if (id == ID_JOIN) {
        char addr[128], spec[160];
        snprintf(addr, sizeof addr, "%s", opt.net_join[0] ? opt.net_join : "127.0.0.1:7777");
        if (prompt(L"Join a game", L"Host address and port, e.g. 192.168.1.20:7777 or a Tailscale name.", addr, sizeof addr)) {
            if (!strchr(addr, ':')) strncat(addr, ":7777", sizeof addr - strlen(addr) - 1);
            snprintf(opt.net_join, sizeof opt.net_join, "%s", addr);
            snprintf(spec, sizeof spec, "join:%s", addr);
            relaunch(spec, opt.net_delay);
        }
    }
    settings_save();
    apply_video();
    build_menu();
    return 1;
}
#endif /* _WIN32 */

/* ---- lifecycle -------------------------------------------------------- */
int platform_init(int w, int h, const char *title)
{
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");       /* fingers are the touch controls, not a gun */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1"); /* an edge swipe must not end the race */
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[model3recomp] SDL_Init: %s\n", SDL_GetError());
        return 0;
    }
    settings_load();
#ifdef __ANDROID__
    opt.fullscreen = 1; /* a phone shows nothing else */
#endif
    g_w = w; g_h = h;
    snprintf(g_title, sizeof g_title, "%s", title);
    g_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             w * opt.scale, h * opt.scale, SDL_WINDOW_RESIZABLE);
    if (!g_win) return 0;
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_ren) return 0;
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!g_tex) return 0;
    g_fb = calloc((size_t)w * h, sizeof(uint32_t));
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
#ifdef _WIN32
    build_menu();
#endif
    apply_video();
    return g_fb != NULL;
}

void platform_shutdown(void)
{
    settings_save();
    free(g_fb); g_fb = NULL;
    if (g_pad) SDL_GameControllerClose(g_pad);
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    SDL_Quit();
}

uint32_t *platform_framebuffer(int *w, int *h)
{
    if (w) *w = g_w;
    if (h) *h = g_h;
    return g_fb;
}

int platform_poll(void)
{
    SDL_Event e;
    int changed = 0;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            return 0;
        case SDL_KEYDOWN:
            if (e.key.repeat) break;
            switch (e.key.keysym.sym) {
            case SDLK_ESCAPE:
                if (!opt.fullscreen) return 0;
                opt.fullscreen = 0; changed = 1; break;
            case SDLK_F11:
                opt.fullscreen = !opt.fullscreen; changed = 1; break;
            case SDLK_RETURN:
                if (e.key.keysym.mod & KMOD_ALT) { opt.fullscreen = !opt.fullscreen; changed = 1; }
                break;
            case SDLK_F5: m3_state_request_save(opt.slot); break;
            case SDLK_F7: m3_state_request_load(opt.slot); break;
            case SDLK_F9: opt.mute = !opt.mute; changed = 1;
                          platform_set_status(opt.mute ? "sound muted" : "sound on"); break;
            case SDLK_F6: {
                char b[64];
                opt.slot = opt.slot % 9 + 1;
                snprintf(b, sizeof b, "slot %d%s", opt.slot, m3_state_exists(opt.slot) ? "" : " (empty)");
                platform_set_status(b);
                changed = 1;
                break;
            }
            default: break;
            }
            break;
        case SDL_CONTROLLERDEVICEADDED:
            if (!g_pad) { g_pad = SDL_GameControllerOpen(e.cdevice.which); changed = 1; }
            break;
#ifdef __ANDROID__
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
        case SDL_FINGERUP:
            touch_event(&e);
            break;
        case SDL_APP_WILLENTERBACKGROUND:
            m3_touch_clear(&g_touch); /* nothing stays held while away */
            break;
#endif
        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_pad && e.cdevice.which ==
                SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
                SDL_GameControllerClose(g_pad); g_pad = NULL; changed = 1;
            }
            break;
#ifdef _WIN32
        case SDL_SYSWMEVENT:
            if (e.syswm.msg->msg.win.msg == WM_COMMAND &&
                !on_command(LOWORD(e.syswm.msg->msg.win.wParam)))
                return 0;
            break;
#endif
        default: break;
        }
    }
    if (changed) {
        settings_save();
        apply_video();
#ifdef _WIN32
        build_menu();
#endif
    }
    if (g_note_until && SDL_GetTicks64() >= g_note_until) { g_note_until = 0; update_title(); }
    return 1;
}

/* ---- controls --------------------------------------------------------- */

/* Reload is pointing off screen and pulling the trigger. One press does
 * both, Supermodel's way: off screen for ten fields, the trigger held for
 * the last five of them, so the game sees the gun leave the screen first. */
static unsigned reload(int player, int held)
{
    static int was[2];
    static uint64_t start[2] = { (uint64_t)-1, (uint64_t)-1 };
    uint64_t f = model3recomp_frame_count();
    unsigned b = 0;
    if (held && !was[player]) start[player] = f;
    was[player] = held;
    if (start[player] != (uint64_t)-1 && f - start[player] < 10u) {
        b |= player ? M3_BTN_OFFSCR2 : M3_BTN_OFFSCR1;
        if (f - start[player] >= 5u) b |= player ? M3_BTN_TRIG2 : M3_BTN_TRIG1;
    }
    return b;
}

static float g_pad_x = -1, g_pad_y = -1;    /* the gamepad's aim, framebuffer px */

static float axis(SDL_GameControllerAxis a)
{
    float v = SDL_GameControllerGetAxis(g_pad, a) / 32767.0f;
    if (v > -0.15f && v < 0.15f) return 0;
    return v;
}

#ifdef __ANDROID__
/* ---- touch controls (Android) -------------------------------------------
 *
 * touch.c lays them out and turns fingers into a pad; here they are fed SDL's
 * finger events and drawn in the bars beside the picture, translucent and lit
 * while held. Shapes are triangles (SDL_RenderGeometry) with a one-pixel edge
 * fading to nothing, so curves are smooth without multisampling; labels come
 * from the system's font (MainActivity.renderText), or from a 5x7 font of our
 * own if that fails. A button clicks under the finger (MainActivity.tick). */
static int g_touch_w, g_touch_h, g_touch_side;
static void touch_labels_drop(void);

static void touch_layout(void)
{
    int w = 0, h = 0;
    float ddpi = 0;
    const SDL_Rect pic = view_rect();
    SDL_GetRendererOutputSize(g_ren, &w, &h);
    if (w == g_touch_w && h == g_touch_h && pic.x == g_touch_side) return;
    g_touch_w = w; g_touch_h = h; g_touch_side = pic.x;
    if (SDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(g_win), &ddpi, NULL, NULL) != 0) ddpi = 0;
    m3_touch_layout(&g_touch, 0, 0, (float)w, (float)h, ddpi / 160.f, 70.f, (float)pic.x);
    touch_labels_drop();                /* their sizes follow the layout's */
}

/* MainActivity's static helpers, looked up once from the activity's class
 * (FindClass on this thread would search the system's class loader). */
static jclass g_main_class;
static jmethodID g_render_text, g_tick;

static JNIEnv *main_activity(void)
{
    static int tried;
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    if (!env) return NULL;
    if (!tried) {
        jobject act = (jobject)SDL_AndroidGetActivity();
        tried = 1;
        if (act) {
            jclass cls = (*env)->GetObjectClass(env, act);
            g_main_class = (jclass)(*env)->NewGlobalRef(env, cls);
            g_render_text = (*env)->GetStaticMethodID(env, g_main_class, "renderText", "(Ljava/lang/String;F)[I");
            if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); g_render_text = NULL; }
            g_tick = (*env)->GetStaticMethodID(env, g_main_class, "tick", "()V");
            if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); g_tick = NULL; }
            (*env)->DeleteLocalRef(env, cls);
            (*env)->DeleteLocalRef(env, act);
        }
    }
    return g_main_class ? env : NULL;
}

static void touch_tick(void)
{
    JNIEnv *env = main_activity();
    if (!env || !g_tick) return;
    (*env)->CallStaticVoidMethod(env, g_main_class, g_tick);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

static void touch_event(const SDL_Event *e)
{
    float x, y;
    touch_layout();
    x = e->tfinger.x * g_touch_w;
    y = e->tfinger.y * g_touch_h;
    if (e->type == SDL_FINGERDOWN) {
        const int b = m3_touch_down(&g_touch, (int64_t)e->tfinger.fingerId, x, y);
        if (b >= 0 && b != M3_TOUCH_STEER) touch_tick();
    } else if (e->type == SDL_FINGERMOTION) {
        m3_touch_move(&g_touch, (int64_t)e->tfinger.fingerId, x, y);
    } else {
        m3_touch_up(&g_touch, (int64_t)e->tfinger.fingerId);
    }
}

/* ---- labels: a texture each, made at the size drawn */
typedef struct { char text[8]; float px; SDL_Texture *tex; int w, h; } touch_label_t;
static touch_label_t g_labels[24];
static int g_nlabels;

static void touch_labels_drop(void)
{
    int k;
    for (k = 0; k < g_nlabels; k++)
        if (g_labels[k].tex) SDL_DestroyTexture(g_labels[k].tex);
    g_nlabels = 0;
}

static const touch_label_t *touch_label(const char *s, float px)
{
    touch_label_t *l;
    JNIEnv *env;
    int k;
    for (k = 0; k < g_nlabels; k++)
        if (g_labels[k].px == px && !strcmp(g_labels[k].text, s)) return &g_labels[k];
    if (g_nlabels == (int)(sizeof g_labels / sizeof g_labels[0])) touch_labels_drop();
    l = &g_labels[g_nlabels++];
    memset(l, 0, sizeof *l);
    snprintf(l->text, sizeof l->text, "%s", s);
    l->px = px;
    if ((env = main_activity()) != NULL && g_render_text) {
        jstring js = (*env)->NewStringUTF(env, s);
        jintArray a = js ? (jintArray)(*env)->CallStaticObjectMethod(env, g_main_class, g_render_text, js, (jfloat)px) : NULL;
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); a = NULL; }
        if (a) {
            const jsize n = (*env)->GetArrayLength(env, a);
            jint *d = (*env)->GetIntArrayElements(env, a, NULL);
            if (d && n >= 2 && d[0] > 0 && d[1] > 0 && n >= 2 + d[0] * d[1]) {
                /* ARGB words, as SDL_PIXELFORMAT_ARGB8888 packs them */
                l->tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, d[0], d[1]);
                if (l->tex) {
                    l->w = d[0]; l->h = d[1];
                    SDL_UpdateTexture(l->tex, NULL, d + 2, d[0] * 4);
                    SDL_SetTextureBlendMode(l->tex, SDL_BLENDMODE_BLEND);
                }
            }
            if (d) (*env)->ReleaseIntArrayElements(env, a, d, JNI_ABORT);
            (*env)->DeleteLocalRef(env, a);
        }
        if (js) (*env)->DeleteLocalRef(env, js);
    }
    return l;
}

static const struct { char c; uint8_t row[7]; } k_font[] = {
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } }, { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F } }, { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { 'G', { 0x0F, 0x10, 0x10, 0x13, 0x11, 0x11, 0x0F } }, { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } }, { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } }, { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } }, { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } }, { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } },
    { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } }, { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
    { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } }, { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
};

/* Text centred on (cx, cy): px is the font's size (capitals about 0.7 of it). */
static void touch_text(float cx, float cy, float px, const char *s, SDL_Color col)
{
    const touch_label_t *l = touch_label(s, px);
    float x, dot;
    if (l->tex) {
        const SDL_FRect d = { floorf(cx - l->w * .5f + .5f), floorf(cy - l->h * .5f + .5f), (float)l->w, (float)l->h };
        SDL_SetTextureColorMod(l->tex, col.r, col.g, col.b);
        SDL_SetTextureAlphaMod(l->tex, col.a);
        SDL_RenderCopyF(g_ren, l->tex, NULL, &d);
        return;
    }
    dot = px * .1f;
    x = cx - (6.f * (float)strlen(s) - 1.f) * dot / 2;
    SDL_SetRenderDrawColor(g_ren, col.r, col.g, col.b, col.a);
    for (; *s; s++, x += 6 * dot) {
        size_t g;
        int r, c;
        for (g = 0; g < sizeof k_font / sizeof k_font[0] && k_font[g].c != *s; g++) {}
        if (g == sizeof k_font / sizeof k_font[0]) continue;
        for (r = 0; r < 7; r++)
            for (c = 0; c < 5; c++)
                if (k_font[g].row[r] & (0x10 >> c)) {
                    SDL_FRect p = { x + c * dot, cy - 3.5f * dot + r * dot, dot, dot };
                    SDL_RenderFillRectF(g_ren, &p);
                }
    }
}

/* ---- shapes: gathered into one mesh, drawn by touch_flush() */
#define TOUCH_VERTS 6144
#define ARC   10                         /* segments a rounded corner */
#define PATH  (4 * (ARC + 1))            /* points round a rounded rectangle */
static SDL_Vertex g_tv[TOUCH_VERTS];
static int g_ti[TOUCH_VERTS * 3];
static int g_ntv, g_nti;

static void touch_flush(void)
{
    if (g_nti) SDL_RenderGeometry(g_ren, NULL, g_tv, g_ntv, g_ti, g_nti);
    g_ntv = g_nti = 0;
}

static void room(int verts)
{
    if (g_ntv + verts > TOUCH_VERTS) touch_flush();
}

static int vert(float x, float y, SDL_Color c)
{
    g_tv[g_ntv].position.x = x;
    g_tv[g_ntv].position.y = y;
    g_tv[g_ntv].color = c;
    g_tv[g_ntv].tex_coord.x = g_tv[g_ntv].tex_coord.y = 0;
    return g_ntv++;
}

static void tri(int a, int b, int c) { g_ti[g_nti++] = a; g_ti[g_nti++] = b; g_ti[g_nti++] = c; }
static void quad(int a, int b, int c, int d) { tri(a, b, c); tri(a, c, d); }
static SDL_Color clear(SDL_Color c) { c.a = 0; return c; }

/* The outline of a rounded rectangle moved d inwards (negative: outwards),
 * corner by corner clockwise from the top left. */
static void rr_path(float x, float y, float w, float h, float r, float d, SDL_FPoint *p)
{
    const float c = r > d ? r : d, rad = r > d ? r - d : 0;
    int k, i, n = 0;
    for (k = 0; k < 4; k++) {
        const float ox = (k == 1 || k == 2) ? x + w - c : x + c, oy = k >= 2 ? y + h - c : y + c;
        for (i = 0; i <= ARC; i++, n++) {
            const float a = 3.14159265f * (1.f + .5f * (float)k + .5f * (float)i / ARC);
            p[n].x = ox + rad * cosf(a);
            p[n].y = oy + rad * sinf(a);
        }
    }
}

/* The band between two outlines, each in its own colour. */
static void band(const SDL_FPoint *a, SDL_Color ca, const SDL_FPoint *b, SDL_Color cb)
{
    const int base = g_ntv;
    int i;
    for (i = 0; i < PATH; i++) vert(a[i].x, a[i].y, ca);
    for (i = 0; i < PATH; i++) vert(b[i].x, b[i].y, cb);
    for (i = 0; i < PATH; i++) {
        const int j = (i + 1) % PATH;
        quad(base + i, base + j, base + PATH + j, base + PATH + i);
    }
}

/* A rounded rectangle: filled, then outlined t thick inside its edge. */
static void touch_round(float x, float y, float w, float h, float r, SDL_Color fill, SDL_Color line, float t)
{
    SDL_FPoint p0[PATH], p1[PATH], p2[PATH], p3[PATH];
    room(5 * PATH + 1);
    if (fill.a) {
        const int c = vert(x + w / 2, y + h / 2, fill), first = g_ntv;
        int i;
        rr_path(x, y, w, h, r, .5f, p1);
        rr_path(x, y, w, h, r, -.5f, p0);
        for (i = 0; i < PATH; i++) vert(p1[i].x, p1[i].y, fill);
        for (i = 0; i < PATH; i++) tri(c, first + i, first + (i + 1) % PATH);
        band(p1, fill, p0, clear(fill));
    }
    if (line.a && t > 0) {
        rr_path(x, y, w, h, r, -.5f, p0);
        rr_path(x, y, w, h, r, .5f, p1);
        rr_path(x, y, w, h, r, t - .5f, p2);
        rr_path(x, y, w, h, r, t + .5f, p3);
        room(6 * PATH);
        band(p0, clear(line), p1, line);
        band(p1, line, p2, line);
        band(p2, line, p3, clear(line));
    }
}

static void touch_circle(float cx, float cy, float r, SDL_Color fill, SDL_Color line, float t)
{
    touch_round(cx - r, cy - r, 2 * r, 2 * r, r, fill, line, t);
}

/* A straight stroke t thick, its long edges soft. */
static void touch_seg(float x0, float y0, float x1, float y1, float t, SDL_Color c)
{
    const float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    const float off[4] = { -t / 2 - .5f, -t / 2 + .5f, t / 2 - .5f, t / 2 + .5f };
    float nx, ny;
    int k, base;
    if (len <= 0) return;
    nx = -dy / len; ny = dx / len;
    room(8);
    base = g_ntv;
    for (k = 0; k < 4; k++) {
        const SDL_Color ck = (k == 0 || k == 3) ? clear(c) : c;
        vert(x0 + nx * off[k], y0 + ny * off[k], ck);
        vert(x1 + nx * off[k], y1 + ny * off[k], ck);
    }
    for (k = 0; k < 3; k++) quad(base + 2 * k, base + 2 * k + 1, base + 2 * k + 3, base + 2 * k + 2);
}

/* A steering wheel: rim, hub, three spokes, and a mark at the top of the
 * rim, all turned by `turn` radians (clockwise). */
static void touch_wheel(float cx, float cy, float r, float turn, SDL_Color back, SDL_Color ink, SDL_Color mark)
{
    static const float spokes[3] = { 0.f, 1.5707963f, 3.1415927f };
    const float t = .2f * r;
    int k;
    touch_circle(cx, cy, r, back, ink, t);
    for (k = 0; k < 3; k++) {
        const float a = spokes[k] + turn, c = cosf(a), s = sinf(a);
        touch_seg(cx + .2f * r * c, cy + .2f * r * s, cx + (r - t * .5f) * c, cy + (r - t * .5f) * s, .17f * r, ink);
    }
    touch_circle(cx, cy, .3f * r, ink, ink, 0);
    {
        const float a = -1.5707963f + turn, c = cosf(a), s = sinf(a);
        touch_seg(cx + (r - t * 1.1f) * c, cy + (r - t * 1.1f) * s, cx + (r + .5f) * c, cy + (r + .5f) * s, t * 1.2f, mark);
    }
}

static SDL_Color rgba(int r, int g, int b, float a)
{
    SDL_Color c;
    c.r = (Uint8)r; c.g = (Uint8)g; c.b = (Uint8)b;
    c.a = (Uint8)(a < 0 ? 0 : a > 255 ? 255 : a);
    return c;
}

static void touch_draw(void)
{
    static const char *const labels[M3_TOUCH_STEER] = { "COIN", "START", "VIEW", "", "", "BRAKE", "GAS" };
    const float u = g_touch.unit, op = 1.f;   /* op: an opacity setting, one day */
    const SDL_Color fill = rgba(255, 255, 255, 30 * op), edge = rgba(255, 255, 255, 120 * op);
    const SDL_Color ink = rgba(255, 255, 255, 235 * op), dim = rgba(255, 255, 255, 110 * op);
    const SDL_Color dark = rgba(16, 16, 20, 255 * op), amber = rgba(255, 190, 0, 225 * op);
    const int gear = m3_touch_gear(m3_input()->in08);
    struct { float cx, cy, px; const char *s; SDL_Color col; } text[16];
    int b, k, ntext = 0;

    touch_layout();
    for (b = 0; b < M3_TOUCH_STEER; b++) {
        const m3_touch_box *x = &g_touch.box[b];
        const int lit = g_touch.held[b];
        const float cx = x->x + x->w / 2, cy = x->y + x->h / 2;
        SDL_Color on = amber;
        if (b == M3_TOUCH_GAS)   on = rgba(70, 205, 95, 225 * op);
        if (b == M3_TOUCH_BRAKE) on = rgba(235, 70, 55, 225 * op);
        touch_round(x->x, x->y, x->w, x->h, 10 * u, lit ? on : fill, lit ? ink : edge, 2 * u);
        if (b == M3_TOUCH_GEAR_DOWN || b == M3_TOUCH_GEAR_UP) {   /* - and + */
            const float s = 11 * u, t = 4.5f * u;
            const SDL_Color c = lit ? dark : ink;
            touch_round(cx - s, cy - t / 2, 2 * s, t, t / 2, c, c, 0);
            if (b == M3_TOUCH_GEAR_UP) touch_round(cx - t / 2, cy - s, t, 2 * s, t / 2, c, c, 0);
        } else if (b == M3_TOUCH_GAS || b == M3_TOUCH_BRAKE) {   /* a pedal's ribs over its name */
            const float rw = x->w - 26 * u, rh = 5 * u;
            for (k = 0; k < 4; k++)
                touch_round(cx - rw / 2, x->y + (18 + 15 * k) * u, rw, rh, rh / 2, lit ? dark : dim, dim, 0);
        }
        if (*labels[b] && ntext < 16) {
            text[ntext].cx = cx;
            text[ntext].cy = b >= M3_TOUCH_BRAKE ? x->y + x->h - 20 * u : cy;
            text[ntext].px = (b == M3_TOUCH_COIN || b == M3_TOUCH_START) && x->w < 74 * u ? 14 * u : 16 * u;
            text[ntext].s = labels[b];
            text[ntext].col = lit ? dark : ink;
            ntext++;
        }
    }
    {   /* the gear lever's four positions, the engaged one lit */
        const m3_touch_box *g = &g_touch.gears;
        const float gap = 6 * u, cw = (g->w - 3 * gap) / 4;
        static const char *const digits[4] = { "1", "2", "3", "4" };
        for (k = 0; k < 4; k++) {
            const float x = g->x + k * (cw + gap);
            const int on = gear == k + 1;
            touch_round(x, g->y, cw, g->h, 7 * u, on ? amber : fill, on ? amber : edge, 1.5f * u);
            if (ntext < 16) {
                text[ntext].cx = x + cw / 2; text[ntext].cy = g->y + g->h / 2; text[ntext].px = 14 * u;
                text[ntext].s = digits[k]; text[ntext].col = on ? dark : dim;
                ntext++;
            }
        }
    }
    {   /* steering: a track centred where the thumb landed, the wheel above
         * the thumb turning with it (at rest, both low in the left column) */
        const float travel = g_touch.range * u, r = 28 * u;
        const int on = g_touch.steering;
        const float cx = on ? g_touch.origin_x : g_touch.rest_x;
        const float cy = on ? g_touch.origin_y : g_touch.rest_y;
        float kx = on ? g_touch.finger_x : cx, wy = cy - 66 * u;
        if (kx < cx - travel) kx = cx - travel;
        if (kx > cx + travel) kx = cx + travel;
        if (wy < r + 6 * u) wy = r + 6 * u;
        touch_round(cx - travel, cy - 4 * u, 2 * travel, 8 * u, 4 * u, fill, edge, 1.5f * u);
        touch_seg(cx, cy - 10 * u, cx, cy + 10 * u, 2 * u, edge);
        if (on) touch_circle(kx, cy, 13 * u, amber, ink, 2 * u);
        else {
            for (k = -1; k <= 1; k += 2) {      /* chevrons either side of the wheel */
                const float ax = cx + k * (r + 22 * u), s = 7 * u;
                touch_seg(ax - k * s * .5f, wy - s, ax + k * s * .5f, wy, 3 * u, dim);
                touch_seg(ax + k * s * .5f, wy, ax - k * s * .5f, wy + s, 3 * u, dim);
            }
        }
        touch_wheel(cx, wy, r, g_touch.steer * 1.5707963f, rgba(0, 0, 0, 90 * op), on ? amber : ink,
                    on ? ink : amber);
    }
    touch_flush();
    for (k = 0; k < ntext; k++) touch_text(text[k].cx, text[k].cy, text[k].px, text[k].s, text[k].col);
}

/* Battery RAM and EEPROM written whenever they have changed, looked at every
 * 300 fields (5 s): an Android app is never quit, only sent away and killed
 * there, so the save at quit (model3recomp_quit) never came and records and
 * settings were lost with each launch. */
static void nvram_keep(void)
{
    static uint64_t last;
    static uint8_t *copy;
    static size_t copy_n;
    const uint64_t f = model3recomp_frame_count();
    size_t n = 0;
    const uint8_t *b = bus_backup(&n);
    const uint8_t *e = (const uint8_t *)bus_eeprom();
    if (!b || !e || f - last < 300) return;
    last = f;
    if (copy_n != n + 128) {
        free(copy);
        copy = (uint8_t *)calloc(1, n + 128);
        copy_n = copy ? n + 128 : 0;
        if (!copy) return;
    }
    if (!memcmp(copy, b, n) && !memcmp(copy + n, e, 128)) return;
    memcpy(copy, b, n);
    memcpy(copy + n, e, 128);
    model3recomp_save_nvram();
}
#endif /* __ANDROID__ */

void platform_sample(m3_input_t *in)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    int mx, my;
    Uint32 m = SDL_GetMouseState(&mx, &my);

    if (k) {
        if (k[SDL_SCANCODE_5]) in->buttons |= M3_BTN_COIN1;
        if (k[SDL_SCANCODE_6]) in->buttons |= M3_BTN_COIN2;
        if (k[SDL_SCANCODE_1]) in->buttons |= M3_BTN_START1;
        if (k[SDL_SCANCODE_2]) in->buttons |= M3_BTN_START2;
        if (k[SDL_SCANCODE_F2]) in->buttons |= M3_BTN_TEST;
        if (k[SDL_SCANCODE_F3]) in->buttons |= M3_BTN_SERVICE;
        if (k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_LCTRL]) in->buttons |= M3_BTN_TRIG1;
        if (k[SDL_SCANCODE_RCTRL]) in->buttons |= M3_BTN_TRIG2;
        /* The keyboard as a pad, for a game's input_map (a driving game):
         * arrows the d-pad, Z / X / C / V the A / B / X / Y buttons. */
        {
            m3_pad_t *p = &in->pad[0];
            p->present = 1;
            if (k[SDL_SCANCODE_UP])    p->buttons |= M3_PAD_UP;
            if (k[SDL_SCANCODE_DOWN])  p->buttons |= M3_PAD_DOWN;
            if (k[SDL_SCANCODE_LEFT])  p->buttons |= M3_PAD_LEFT;
            if (k[SDL_SCANCODE_RIGHT]) p->buttons |= M3_PAD_RIGHT;
            if (k[SDL_SCANCODE_Z])     p->buttons |= M3_PAD_A;
            if (k[SDL_SCANCODE_X])     p->buttons |= M3_PAD_B;
            if (k[SDL_SCANCODE_C])     p->buttons |= M3_PAD_X;
            if (k[SDL_SCANCODE_V])     p->buttons |= M3_PAD_Y;
        }
    }
    if (g_pad) {
        m3_pad_t *p = &in->pad[0];
        float ax = axis(SDL_CONTROLLER_AXIS_LEFTX);
        p->present = 1;
        p->sx = (int8_t)(ax * 127.0f);
        p->lt = (uint8_t)(axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) * 255.0f);
        p->rt = (uint8_t)(axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) * 255.0f);
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_UP))   p->buttons |= M3_PAD_UP;
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) p->buttons |= M3_PAD_DOWN;
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_X))         p->buttons |= M3_PAD_X;
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_START))     p->buttons |= M3_PAD_START;
    }
#ifdef __ANDROID__
    m3_touch_pad(&g_touch, &in->pad[0]);
#endif

    if (opt.mouse) {
        SDL_Rect r = view_rect();
        int px = (int)((double)(mx - r.x) * g_w / (r.w > 0 ? r.w : 1));
        int py = (int)((double)(my - r.y) * g_h / (r.h > 0 ? r.h : 1));
        m3_gun_from_screen(px, py, g_w, g_h, &in->gun_x[0], &in->gun_y[0]);
        if (m & SDL_BUTTON(SDL_BUTTON_LEFT)) in->buttons |= M3_BTN_TRIG1;
    }
    in->buttons |= reload(0, (opt.mouse && (m & SDL_BUTTON(SDL_BUTTON_RIGHT))) ||
                             (k && k[SDL_SCANCODE_LSHIFT]));

    if (g_pad && opt.pad) {
        int p = opt.pad - 1;
        float speed = 6.0f;                 /* pixels a field at full tilt */
        if (g_pad_x < 0) { g_pad_x = g_w / 2.0f; g_pad_y = g_h / 2.0f; }
        g_pad_x += axis(SDL_CONTROLLER_AXIS_LEFTX) * speed;
        g_pad_y += axis(SDL_CONTROLLER_AXIS_LEFTY) * speed;
        if (g_pad_x < 0) g_pad_x = 0;
        if (g_pad_y < 0) g_pad_y = 0;
        if (g_pad_x > g_w - 1) g_pad_x = (float)(g_w - 1);
        if (g_pad_y > g_h - 1) g_pad_y = (float)(g_h - 1);
        m3_gun_from_screen((int)g_pad_x, (int)g_pad_y, g_w, g_h, &in->gun_x[p], &in->gun_y[p]);
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_A) ||
            axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.5f)
            in->buttons |= p ? M3_BTN_TRIG2 : M3_BTN_TRIG1;
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_START))
            in->buttons |= p ? M3_BTN_START2 : M3_BTN_START1;
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_BACK))
            in->buttons |= p ? M3_BTN_COIN2 : M3_BTN_COIN1;
        in->buttons |= reload(p, SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_B) ||
                                 axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 0.5f);
    }
}

/* ---- presenting ------------------------------------------------------- */
static void crosshair(const SDL_Rect *r, int gx, int gy, Uint8 cr, Uint8 cg, Uint8 cb)
{
    int x = r->x + (gx - M3_GUN_X0) * r->w / (M3_GUN_X1 - M3_GUN_X0);
    int y = r->y + (gy - M3_GUN_Y0) * r->h / (M3_GUN_Y1 - M3_GUN_Y0);
    int s = r->h / 40 + 4, i;
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 160);
    for (i = -1; i <= 1; i += 2) {
        SDL_RenderDrawLine(g_ren, x - s, y + i, x + s, y + i);
        SDL_RenderDrawLine(g_ren, x + i, y - s, x + i, y + s);
    }
    SDL_SetRenderDrawColor(g_ren, cr, cg, cb, 255);
    SDL_RenderDrawLine(g_ren, x - s, y, x - 2, y);
    SDL_RenderDrawLine(g_ren, x + 2, y, x + s, y);
    SDL_RenderDrawLine(g_ren, x, y - s, x, y - 2);
    SDL_RenderDrawLine(g_ren, x, y + 2, x, y + s);
}

/* Hold the display to the board's 57.524 Hz. Fields are paced by guest
 * work, not the clock, so without this the game runs as fast as the host
 * can draw it. M3_NOTHROTTLE turns it off for scripted captures; holding
 * Tab does the same for a moment. */
static SDL_AudioDeviceID g_audio;
#define AUDIO_BYTES_PER_MS 176u          /* 44.1 kHz, stereo, 16-bit */
#define AUDIO_TARGET_MS    100u

static void throttle(void)
{
    static int probed, off;
    static uint64_t next;
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint64_t now;
    if (!probed) { probed = 1; off = getenv("M3_NOTHROTTLE") != NULL; }
    if (off || (k && k[SDL_SCANCODE_TAB])) { next = 0; return; }
    now = platform_ticks_us();
    if (!next || now > next + 100000u   /* first field, or fell far behind */
        || next > now + 1000000u)       /* or the clock went back */
        next = now;
    /* 1e6 / 57.524 us a field, nudged by up to 2% to hold the audio queue
     * at its target: the sound card's clock and this one never quite
     * agree, and a queue left to drift either runs dry or lags. */
    {
        int64_t period = 17384;
        if (g_audio && SDL_GetAudioDeviceStatus(g_audio) == SDL_AUDIO_PLAYING) {
            int64_t q = (int64_t)(SDL_GetQueuedAudioSize(g_audio) / AUDIO_BYTES_PER_MS);
            int64_t adj = (q - (int64_t)AUDIO_TARGET_MS) * 17384 / 2000;   /* 1% per 20 ms off */
            if (adj > 348) adj = 348;
            if (adj < -348) adj = -348;
            period += adj;
        }
        next += (uint64_t)period;
    }
    while ((now = platform_ticks_us()) < next) {
        if (next - now > 2000u)
            SDL_Delay((Uint32)((next - now) / 1000u - 1u));
    }
}

void platform_present(void)
{
    SDL_Rect r = view_rect();
    const m3_input_t *in = m3_input();

    SDL_UpdateTexture(g_tex, NULL, g_fb, g_w * (int)sizeof(uint32_t));
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, NULL, &r);
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);

    /* Scanlines: darken the lower part of every source line, once each
     * line is at least two pixels tall on screen. */
    if (opt.scanlines && r.h >= g_h * 2) {
        int y;
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 96);
        for (y = 0; y < g_h; y++) {
            SDL_Rect s;
            int y0 = r.y + y * r.h / g_h, y1 = r.y + (y + 1) * r.h / g_h;
            s.x = r.x; s.w = r.w;
            s.h = (y1 - y0) / 2; s.y = y1 - s.h;
            SDL_RenderFillRect(g_ren, &s);
        }
    }

    /* A Sinden gun finds the screen by its white border. */
    if (opt.sinden) {
        int t = r.h * (opt.sinden == 1 ? 15 : 30) / 1000 + 1;
        SDL_Rect s[4];
        s[0].x = r.x; s[0].y = r.y; s[0].w = r.w; s[0].h = t;
        s[1].x = r.x; s[1].y = r.y + r.h - t; s[1].w = r.w; s[1].h = t;
        s[2].x = r.x; s[2].y = r.y; s[2].w = t; s[2].h = r.h;
        s[3].x = r.x + r.w - t; s[3].y = r.y; s[3].w = t; s[3].h = r.h;
        SDL_SetRenderDrawColor(g_ren, 255, 255, 255, 255);
        SDL_RenderFillRects(g_ren, s, 4);
    }

    /* Crosshairs: the cabinet had none, so they are an option for the
     * mouse, and always there for a gamepad or a netplay partner, who
     * have nothing else to aim by. */
    if (opt.cursor == CUR_CROSSHAIR || (g_pad && opt.pad == 1))
        crosshair(&r, in->gun_x[0], in->gun_y[0], 255, 64, 64);
    if ((g_pad && opt.pad == 2) || netplay_active())
        crosshair(&r, in->gun_x[1], in->gun_y[1], 64, 160, 255);

#ifdef __ANDROID__
    touch_draw();
    nvram_keep();
#endif
    SDL_RenderPresent(g_ren);
    throttle();
}

/* ---- audio -------------------------------------------------------------
 *
 * The sound board hands over a field's samples at a time, made in step with
 * the guest, not pulled by the audio device -- so the device is fed a queue.
 * Running at 57.524 Hz the queue holds steady; a little latency is kept in
 * hand (100 ms, held there by the frame limiter), anything beyond 300 ms
 * (running flat out) is dropped rather than heard late, and a queue that
 * runs dry pauses to refill rather than crackling.
 */
static int g_audio_failed;
static unsigned g_underruns, g_audio_fields;

void platform_audio(const int16_t *lr, int frames)
{
    static int16_t tmp[2 * 1024];
    Uint32 queued;
    int i, gain;

    if (g_audio_failed || frames <= 0 || frames > 1024) return;
    if (!g_audio) {
        SDL_AudioSpec want, have;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { g_audio_failed = 1; return; }
        memset(&want, 0, sizeof want);
        want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
        g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!g_audio) {
            fprintf(stderr, "[model3recomp] no audio device: %s\n", SDL_GetError());
            g_audio_failed = 1;
            return;
        }
    }
    queued = SDL_GetQueuedAudioSize(g_audio);
    if (queued > AUDIO_BYTES_PER_MS * 300u)     /* over 300 ms behind: drop */
        return;
    /* Ran dry (the host stalled): stop and refill rather than crackle. */
    if (queued < AUDIO_BYTES_PER_MS * 4u &&
        SDL_GetAudioDeviceStatus(g_audio) == SDL_AUDIO_PLAYING) {
        SDL_PauseAudioDevice(g_audio, 1);
        g_underruns++;
    }
    /* M3_AUDIO_TRACE: the queue's depth once a second, and how often it
     * ran dry -- the measure of whether the host keeps up. */
    if (getenv("M3_AUDIO_TRACE") && ++g_audio_fields % 58 == 0)
        fprintf(stderr, "[audio] queued %u ms  underruns %u\n",
                (unsigned)(queued / AUDIO_BYTES_PER_MS), g_underruns);
    gain = opt.mute ? 0 : opt.volume * 256 / 100;
    for (i = 0; i < 2 * frames; i++) tmp[i] = (int16_t)((lr[i] * gain) >> 8);
    SDL_QueueAudio(g_audio, tmp, (Uint32)(frames * 4));
    /* Play once the target is in hand, so a hitch has room to hide in. */
    if (SDL_GetAudioDeviceStatus(g_audio) != SDL_AUDIO_PLAYING &&
        SDL_GetQueuedAudioSize(g_audio) >= AUDIO_BYTES_PER_MS * AUDIO_TARGET_MS)
        SDL_PauseAudioDevice(g_audio, 0);
}

uint64_t platform_ticks_us(void)
{
    /* SDL_GetPerformanceCounter, not SDL_GetTicks: a 17.4 ms field needs
     * better than millisecond resolution or the pacing judders. Seconds and
     * the rest scaled apart: counter * 1e6 passes 64 bits once a nanosecond
     * counter (Linux, Android) reaches 5.1 hours of uptime, and the clock
     * then jumped back 5.1 hours -- throttle() slept that long. */
    static uint64_t freq;
    uint64_t c;
    if (!freq) freq = SDL_GetPerformanceFrequency();
    c = SDL_GetPerformanceCounter();
    return c / freq * 1000000ull + c % freq * 1000000ull / freq;
}
