#include "state.h"
#include "config/settings.h"
#include "features/features.h"
#include "features/hitmarker.h"
#include "features/sound_esp.h"
#include "memory/process.h"
#include "sdk/offsets.h"
#include "sdk/game.h"
#include "sdk/dumper.h"
#include "render/camera.h"
#include "overlay/world.h"
#include <cmath>
#include "input/input.h"
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gtk-layer-shell/gtk-layer-shell.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <linux/input.h>
#include <cairo.h>
#include <atomic>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <string>

static std::atomic<bool> s_running{true};
static Settings* g_cfg = nullptr;
static int s_lock_fd = -1;
static GtkWidget* g_area = nullptr;
static GtkWidget* g_overlay_win = nullptr;
static GtkWidget* g_gui_win = nullptr;

static void InstallCss();

struct Rect { double x, y, w, h; };
static Rect s_r_wm{20, 20, 0, 0};
static Rect s_r_bomb{0, 24, 0, 0};
static Rect s_r_keybinds{20, 100, 0, 0};
static Rect s_r_radar{20, 320, 0, 0};

static int    s_drag_target = -1;
static double s_drag_off_x = 0, s_drag_off_y = 0;

static int    s_frame_count = 0;
static gint64 s_fps_last_us = 0;

static std::atomic<int> s_screen_w{1920};
static std::atomic<int> s_screen_h{1080};

static bool TakeInstanceLock() {
    s_lock_fd = open("/tmp/spaxer.lock", O_RDWR | O_CREAT, 0644);
    if (s_lock_fd < 0) return false;
    if (flock(s_lock_fd, LOCK_EX | LOCK_NB) != 0) return false;
    return true;
}

static void EnsurePtraceScope() {
    FILE* f = fopen("/proc/sys/kernel/yama/ptrace_scope", "r");
    if (!f) return;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    if (v == 0) return;
    if (system("pkexec sysctl -w kernel.yama.ptrace_scope=0 >/dev/null 2>&1") == 0) return;
    system("sudo -n sysctl -w kernel.yama.ptrace_scope=0 >/dev/null 2>&1");
}

static void ApplyInputMode() {
    GdkWindow* gw = gtk_widget_get_window(g_overlay_win);
    if (!gw) return;
    gboolean pass_through = !g_cfg->edit_mode;
    gdk_window_set_pass_through(gw, pass_through);
    if (pass_through) {
        cairo_region_t* empty = cairo_region_create();
        gdk_window_input_shape_combine_region(gw, empty, 0, 0);
        cairo_region_destroy(empty);
    } else {
        gdk_window_input_shape_combine_region(gw, nullptr, 0, 0);
    }
}

static void DrawText(cairo_t* cr, double x, double y, const char* s,
                     double r, double g, double b, double a, double size, bool bold) {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    cairo_move_to(cr, std::round(x), std::round(y + fe.ascent));
    cairo_show_text(cr, s);
}

static void MeasureText(cairo_t* cr, const char* s, double size, bool bold, cairo_text_extents_t& e) {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_text_extents(cr, s, &e);
}

static void RoundedRect(cairo_t* cr, double x, double y, double w, double h, double rad) {
    double d = 3.1415926535 / 180.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - rad, y + rad,     rad, -90 * d,   0 * d);
    cairo_arc(cr, x + w - rad, y + h - rad, rad,   0 * d,  90 * d);
    cairo_arc(cr, x + rad,     y + h - rad, rad,  90 * d, 180 * d);
    cairo_arc(cr, x + rad,     y + rad,     rad, 180 * d, 270 * d);
    cairo_close_path(cr);
}

static void DrawWatermark(cairo_t* cr) {
    if (!settings::Enabled(g_cfg->watermark)) return;

    int fps  = g_hud.overlay_fps.load();
    int ping = g_hud.local_ping.load();

    char sfps[16];  snprintf(sfps,  sizeof(sfps),  "%d fps",  fps);
    char sping[16]; if (ping >= 0) snprintf(sping, sizeof(sping), "%d ms", ping);
                    else           snprintf(sping, sizeof(sping), "-- ms");
    const char* brand = "SPAXER";

    double sz_brand = 13, sz_stat = 11;
    cairo_text_extents_t eb, ef, ep;
    MeasureText(cr, brand, sz_brand, true, eb);
    MeasureText(cr, sfps,  sz_stat,  false, ef);
    MeasureText(cr, sping, sz_stat,  false, ep);

    double pad_x = 12, gap = 8, sep_w = 1.0;
    double w = pad_x + eb.width + gap + sep_w + gap + ef.width + gap + sep_w + gap + ep.width + pad_x;
    double h = 28;

    if (g_cfg->hud_wm_x >= 0) {
        s_r_wm.x = g_cfg->hud_wm_x;
        s_r_wm.y = g_cfg->hud_wm_y;
    } else {
        if (s_r_wm.x < 0) s_r_wm.x = 20;
        if (s_r_wm.y < 0) s_r_wm.y = 20;
        g_cfg->hud_wm_x = (int)s_r_wm.x;
        g_cfg->hud_wm_y = (int)s_r_wm.y;
    }
    s_r_wm.w = w; s_r_wm.h = h;

    double x = s_r_wm.x, y = s_r_wm.y;

    RoundedRect(cr, x, y, w, h, 8);
    cairo_set_source_rgba(cr, 0.08, 0.08, 0.10, 0.78);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    double cx = x + pad_x;
    double ty = y + (h - sz_brand) * 0.5 - 1.0;

    DrawText(cr, cx, ty, brand, 1.0, 1.0, 1.0, 1.0, sz_brand, true);
    cx += eb.width + gap;

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
    cairo_set_line_width(cr, sep_w);
    cairo_move_to(cr, cx, y + 6); cairo_line_to(cr, cx, y + h - 6); cairo_stroke(cr);
    cx += gap;

    double stat_y = y + (h - sz_stat) * 0.5 - 1.0;
    DrawText(cr, cx, stat_y, sfps, 0.20, 0.78, 0.35, 1.0, sz_stat, false);
    cx += ef.width + gap;

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
    cairo_move_to(cr, cx, y + 6); cairo_line_to(cr, cx, y + h - 6); cairo_stroke(cr);
    cx += gap;

    double pr = 0.20, pg = 0.78, pb = 0.35;
    if (ping > 80)       { pr = 1.0; pg = 0.27; pb = 0.27; }
    else if (ping > 45)  { pr = 1.0; pg = 0.75; pb = 0.25; }
    DrawText(cr, cx, stat_y, sping, pr, pg, pb, 1.0, sz_stat, false);
}

static void DrawBomb(cairo_t* cr, int W) {
    if (!settings::Enabled(g_cfg->bomb_timer)) return;

    bool planted = g_hud.bomb_visible.load();
    float blow = g_hud.bomb_blow_secs.load();
    int   site = g_hud.bomb_site.load();
    bool  bd   = g_hud.bomb_being_defused.load();
    float df   = g_hud.bomb_defuse_secs.load();

    const char* title = "BOMB TIMER";
    char main_txt[128];
    char sub_txt[128] = {0};

    if (planted && blow > 0.f) {
        snprintf(main_txt, sizeof(main_txt), "SITE %s — %.1fs", site == 0 ? "A" : "B", blow);
        if (bd && df > 0.f) {
            snprintf(sub_txt, sizeof(sub_txt), "DEFUSING — %.1fs", df);
        }
    } else {
        snprintf(main_txt, sizeof(main_txt), "NOT PLANTED");
    }

    double sz_title = 10, sz_main = 13, sz_sub = 11;
    cairo_text_extents_t et, em, es = {};
    MeasureText(cr, title, sz_title, true, et);
    MeasureText(cr, main_txt, sz_main, true, em);
    if (sub_txt[0]) MeasureText(cr, sub_txt, sz_sub, false, es);

    double tw = std::max({et.width, em.width, es.width});
    double w = tw + 28;
    double h = 16 + sz_title + 6 + sz_main + (sub_txt[0] ? 4 + sz_sub : 0) + 10;

    if (g_cfg->hud_bomb_x >= 0) {
        s_r_bomb.x = g_cfg->hud_bomb_x;
        s_r_bomb.y = g_cfg->hud_bomb_y;
    } else {
        if (s_r_bomb.x < 0) s_r_bomb.x = (W - w) * 0.5;
        if (s_r_bomb.y < 0) s_r_bomb.y = 20;
        g_cfg->hud_bomb_x = (int)s_r_bomb.x;
        g_cfg->hud_bomb_y = (int)s_r_bomb.y;
    }
    s_r_bomb.w = w; s_r_bomb.h = h;

    double x = s_r_bomb.x, y = s_r_bomb.y;

    RoundedRect(cr, x, y, w, h, 8);
    cairo_set_source_rgba(cr, 0.08, 0.08, 0.10, 0.78);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    double cur_y = y + 8;
    DrawText(cr, x + 14, cur_y, title, 0.55, 0.55, 0.60, 1.0, sz_title, true);

    double tr = 0.55, tg = 0.55, tb = 0.60;
    if (planted) {
        if (blow < 5.f)      { tr = 1.00; tg = 0.27; tb = 0.27; }
        else if (blow < 15.f){ tr = 1.00; tg = 0.75; tb = 0.25; }
        else                 { tr = 0.20; tg = 0.78; tb = 0.35; }
    }

    cur_y += sz_title + 6;
    DrawText(cr, x + 14, cur_y, main_txt, tr, tg, tb, 1.0, sz_main, true);

    if (sub_txt[0]) {
        cur_y += sz_main + 4;
        DrawText(cr, x + 14, cur_y, sub_txt, 0.0, 0.48, 1.0, 1.0, sz_sub, false);
    }
}

static const char* KeysymName(uint32_t k);

static void DrawKeybinds(cairo_t* cr) {
    if (!settings::Enabled(g_cfg->keybinds)) return;

    struct BindItem {
        const char* name;
        uint32_t key;
        bool active;
    };

    BindItem items[] = {
        {"Aimbot",     g_cfg->bind_aimbot,      settings::Enabled(g_cfg->aimbot_enabled)},
        {"TriggerBot", g_cfg->bind_trigger,     settings::Enabled(g_cfg->trigger_enabled)},
        {"ESP",        g_cfg->bind_esp,         settings::Enabled(g_cfg->esp)},
        {"Glow",       g_cfg->bind_glow,        settings::Enabled(g_cfg->glow)},
        {"Chams",      g_cfg->bind_chams,       settings::Enabled(g_cfg->chams)},
        {"Bunny Hop",  g_cfg->bind_bunnyhop,    settings::Enabled(g_cfg->bunnyhop)},
        {"Auto Strafe",g_cfg->bind_auto_strafe, settings::Enabled(g_cfg->auto_strafe)},
        {"Bomb Timer", g_cfg->bind_bomb,        settings::Enabled(g_cfg->bomb_timer)},
        {"Watermark",  g_cfg->bind_watermark,   settings::Enabled(g_cfg->watermark)},
        {"Keybinds",   g_cfg->bind_keybinds,    settings::Enabled(g_cfg->keybinds)},
        {"Crosshair",  g_cfg->bind_crosshair,  settings::Enabled(g_cfg->crosshair)},
        {"Thirdperson",g_cfg->bind_thirdperson, settings::Enabled(g_cfg->thirdperson)},
        {"Arrows",     g_cfg->bind_arrows,      settings::Enabled(g_cfg->arrows)},
        {"Sound ESP",  g_cfg->bind_sound_esp,   settings::Enabled(g_cfg->sound_esp)},
        {"Weapon ESP", g_cfg->bind_weapon_esp,  settings::Enabled(g_cfg->esp_dropped_weapons)},
        {"Edit HUD",   g_cfg->bind_edit_hud,    settings::Enabled(g_cfg->edit_mode)},
    };

    std::vector<std::pair<std::string, std::string>> active_list;
    for (const auto& item : items) {
        if (settings::Enabled(g_cfg->edit_mode)) {
            char val_buf[32];
            if (item.key != 0) snprintf(val_buf, sizeof(val_buf), "[%s]", KeysymName(item.key));
            else snprintf(val_buf, sizeof(val_buf), "[ON]");
            active_list.push_back({item.name, val_buf});
        } else if (item.key != 0 && item.active) {
            char val_buf[32];
            snprintf(val_buf, sizeof(val_buf), "[%s]", KeysymName(item.key));
            active_list.push_back({item.name, val_buf});
        }
    }

    if (active_list.empty() && !settings::Enabled(g_cfg->edit_mode)) return;

    double sz_title = 10, sz_item = 12;
    cairo_text_extents_t et;
    MeasureText(cr, "KEYBINDS", sz_title, true, et);

    double max_namew = et.width;
    double max_valw = 0;
    for (const auto& kv : active_list) {
        cairo_text_extents_t en, ev;
        MeasureText(cr, kv.first.c_str(), sz_item, false, en);
        MeasureText(cr, kv.second.c_str(), sz_item, true, ev);
        if (en.width > max_namew) max_namew = en.width;
        if (ev.width > max_valw) max_valw = ev.width;
    }

    double w = std::max(170.0, max_namew + max_valw + 36.0);
    double item_h = 18.0;
    double h = 26.0 + active_list.size() * item_h + 8.0;

    if (g_cfg->hud_keybinds_x >= 0) {
        s_r_keybinds.x = g_cfg->hud_keybinds_x;
        s_r_keybinds.y = g_cfg->hud_keybinds_y;
    } else {
        if (s_r_keybinds.x < 0) s_r_keybinds.x = 20;
        if (s_r_keybinds.y < 0) s_r_keybinds.y = 60;
        g_cfg->hud_keybinds_x = (int)s_r_keybinds.x;
        g_cfg->hud_keybinds_y = (int)s_r_keybinds.y;
    }
    if (s_r_keybinds.x <= 25 && s_r_keybinds.y < 56) {
        s_r_keybinds.y = 60;
        g_cfg->hud_keybinds_y = 60;
    }
    s_r_keybinds.w = w; s_r_keybinds.h = h;

    double x = s_r_keybinds.x, y = s_r_keybinds.y;

    RoundedRect(cr, x, y, w, h, 8);
    cairo_set_source_rgba(cr, 0.08, 0.08, 0.10, 0.78);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    DrawText(cr, x + 12, y + 8, "KEYBINDS", 0.55, 0.55, 0.60, 1.0, sz_title, true);

    double cur_y = y + 8 + sz_title + 8;
    for (const auto& kv : active_list) {
        DrawText(cr, x + 12, cur_y, kv.first.c_str(), 0.95, 0.95, 0.97, 1.0, sz_item, false);
        bool is_on = kv.second != "OFF";
        double vr = is_on ? 0.20 : 0.55;
        double vg = is_on ? 0.78 : 0.55;
        double vb = is_on ? 0.35 : 0.60;
        cairo_text_extents_t ev;
        MeasureText(cr, kv.second.c_str(), sz_item, true, ev);
        DrawText(cr, x + w - 12 - ev.width, cur_y, kv.second.c_str(), vr, vg, vb, 1.0, sz_item, true);
        cur_y += item_h;
    }
}

static void DrawSpectators(cairo_t* cr, int W) {
    if (!settings::Enabled(g_cfg->spectators)) return;

    std::vector<SpectatorEntry> specs;
    {
        std::lock_guard<std::mutex> lock(g_hud.spectators_mtx);
        specs = g_hud.spectators;
    }

    if (specs.empty() && !settings::Enabled(g_cfg->edit_mode)) return;

    double sz_title = 10, sz_item = 12;
    cairo_text_extents_t et;
    MeasureText(cr, "SPECTATORS", sz_title, true, et);

    double max_w = et.width;
    for (const auto& s : specs) {
        cairo_text_extents_t es;
        MeasureText(cr, s.name, sz_item, false, es);
        if (es.width > max_w) max_w = es.width;
    }

    double w = std::max(150.0, max_w + 28.0);
    double h = 26.0 + (specs.empty() ? 18.0 : specs.size() * 18.0) + 6.0;

    double x = W - w - 20;
    double y = 60;

    RoundedRect(cr, x, y, w, h, 8);
    cairo_set_source_rgba(cr, 0.08, 0.08, 0.10, 0.78);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    DrawText(cr, x + 12, y + 14, "SPECTATORS", 0.55, 0.55, 0.60, 1.0, sz_title, true);

    double cur_y = y + 14 + et.height + 8;
    if (specs.empty()) {
        DrawText(cr, x + 12, cur_y, "None", 0.55, 0.55, 0.60, 1.0, sz_item, false);
    } else {
        int my_team = g_hud.local_team.load();
        for (const auto& s : specs) {
            double r = 0.95, g = 0.95, b = 0.97;
            if (s.team == my_team) { r = 0.30; g = 0.85; b = 1.0; }
            else { r = 1.0; g = 0.35; b = 0.35; }
            DrawText(cr, x + 12, cur_y, s.name, r, g, b, 1.0, sz_item, false);
            cur_y += 18.0;
        }
    }
}

static render::Camera s_camera;

static void DrawCrosshair(cairo_t* cr, int W, int H) {
    if (!g_hud.in_game.load()) return;

    uintptr_t pawn = game::LocalPawn();
    int weap_idx = pawn ? game::ActiveWeaponDefinitionIndex(pawn) : 0;
    bool is_sniper = (weap_idx == 9 || weap_idx == 11 || weap_idx == 38 || weap_idx == 40);
    bool is_scoped = pawn && off::m_bIsScoped && g_proc.Read<bool>(pawn + off::m_bIsScoped);

    if (!settings::Enabled(g_cfg->crosshair) && !(settings::Enabled(g_cfg->sniper_crosshair) && is_sniper))
        return;

    bool force_sniper_style = settings::Enabled(g_cfg->sniper_crosshair) && is_sniper && is_scoped;
    double cx = W * 0.5, cy = H * 0.5;
    double gap = force_sniper_style ? 5.0 : g_cfg->crosshair_gap;
    double len = force_sniper_style ? 5.0 : g_cfg->crosshair_size;
    double th  = force_sniper_style ? 1.5 : g_cfg->crosshair_thickness;
    uint32_t rgba = g_cfg->crosshair_color_rgba;
    double r = ((rgba >> 24) & 0xFF) / 255.0;
    double g = ((rgba >> 16) & 0xFF) / 255.0;
    double b = ((rgba >>  8) & 0xFF) / 255.0;
    double a = ( rgba        & 0xFF) / 255.0;

    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    if (settings::Enabled(g_cfg->crosshair_outline) || force_sniper_style) {
        cairo_set_source_rgba(cr, 0, 0, 0, 0.75);
        cairo_set_line_width(cr, th + 1.5);
        cairo_move_to(cr, cx - gap - len, cy); cairo_line_to(cr, cx - gap, cy);
        cairo_move_to(cr, cx + gap,       cy); cairo_line_to(cr, cx + gap + len, cy);
        cairo_move_to(cr, cx, cy - gap - len); cairo_line_to(cr, cx, cy - gap);
        cairo_move_to(cr, cx, cy + gap);       cairo_line_to(cr, cx, cy + gap + len);
        cairo_stroke(cr);
    }

    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_set_line_width(cr, th);
    cairo_move_to(cr, cx - gap - len, cy); cairo_line_to(cr, cx - gap, cy);
    cairo_move_to(cr, cx + gap,       cy); cairo_line_to(cr, cx + gap + len, cy);
    cairo_move_to(cr, cx, cy - gap - len); cairo_line_to(cr, cx, cy - gap);
    cairo_move_to(cr, cx, cy + gap);       cairo_line_to(cr, cx, cy + gap + len);
    cairo_stroke(cr);

    if (settings::Enabled(g_cfg->crosshair_dot) || force_sniper_style) {
        cairo_arc(cr, cx, cy, th * 0.5, 0, 6.2831853);
        cairo_fill(cr);
    }
}

static void DrawEditMarker(cairo_t* cr, const Rect& r) {
    RoundedRect(cr, r.x - 3, r.y - 3, r.w + 6, r.h + 6, 8);
    cairo_set_source_rgba(cr, 0.35, 0.75, 1.0, 0.7);
    cairo_set_line_width(cr, 1.2);
    cairo_set_dash(cr, (double[]){5,4}, 2, 0);
    cairo_stroke(cr);
    cairo_set_dash(cr, nullptr, 0, 0);
}

static bool IsCs2Active() {
    static Display* display = XOpenDisplay(nullptr);
    if (!display || g_proc.pid() <= 0) return false;
    static Atom active_atom = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    static Atom pid_atom = XInternAtom(display, "_NET_WM_PID", False);
    Window active = 0;
    Atom type;
    int format;
    unsigned long count, bytes_after;
    unsigned char* data = nullptr;
    int status = XGetWindowProperty(display, DefaultRootWindow(display), active_atom, 0, 1, False,
                                    XA_WINDOW, &type, &format, &count, &bytes_after, &data);
    if (status != Success || !data || count != 1) {
        if (data) XFree(data);
        return false;
    }
    active = *reinterpret_cast<Window*>(data);
    XFree(data);
    data = nullptr;
    if (!active) return false;
    status = XGetWindowProperty(display, active, pid_atom, 0, 1, False, XA_CARDINAL,
                                &type, &format, &count, &bytes_after, &data);
    if (status != Success || !data || count != 1) {
        if (data) XFree(data);
        return false;
    }
    unsigned long pid = *reinterpret_cast<unsigned long*>(data);
    XFree(data);
    return pid == static_cast<unsigned long>(g_proc.pid());
}

static void DrawHitmarker(cairo_t* cr, int width, int height) {
    if (!g_cfg || !settings::Enabled(g_cfg->hitmarker)) return;
    std::vector<features::HitMark> marks;
    {
        std::lock_guard<std::mutex> lock(features::g_hitmarks_mtx);
        auto now = std::chrono::steady_clock::now();
        std::erase_if(features::g_hitmarks, [&](const features::HitMark& m) {
            return now - m.time > std::chrono::milliseconds(800);
        });
        marks = features::g_hitmarks;
    }
    if (marks.empty()) return;
    const render::Camera& cam = s_camera;
    if (!cam.valid) return;
    auto now = std::chrono::steady_clock::now();
    for (const auto& mark : marks) {
        float sx, sy;
        if (!cam.Project(mark.position.x, mark.position.y, mark.position.z, sx, sy)) continue;
        double age = std::chrono::duration<double, std::milli>(now - mark.time).count();
        double alpha = 1.0 - age / 800.0;
        double len = 6.0;
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 0) { cairo_set_source_rgba(cr, 0, 0, 0, alpha * 0.8); cairo_set_line_width(cr, 3.5); }
            else           { cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha); cairo_set_line_width(cr, 1.8); }
            cairo_move_to(cr, sx - len, sy - len); cairo_line_to(cr, sx + len, sy + len);
            cairo_move_to(cr, sx + len, sy - len); cairo_line_to(cr, sx - len, sy + len);
            cairo_stroke(cr);
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "-%d", mark.damage);
        DrawText(cr, sx + 9, sy - 18 - age * 0.02, buf, 1.0, 0.36, 0.36, alpha, 13, true);
    }
}

static void DrawSoundEsp(cairo_t* cr, const render::Camera& cam) {
    if (!g_cfg || !settings::Enabled(g_cfg->sound_esp) || !cam.valid) return;
    std::vector<features::SoundStep> steps;
    {
        std::lock_guard<std::mutex> lock(features::g_sound_steps_mtx);
        steps = features::g_sound_steps;
    }
    constexpr int kSegments = 40;
    uint32_t rgba = g_cfg->sound_esp_rgba;
    double r = ((rgba >> 24) & 0xFF) / 255.0, g = ((rgba >> 16) & 0xFF) / 255.0, b = ((rgba >> 8) & 0xFF) / 255.0;
    double base_alpha = (rgba & 0xFF) / 255.0;
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    for (const auto& step : steps) {
        if (step.alpha <= 0.01f || step.radius < 1.f) continue;
        cairo_new_path(cr);
        bool pen_down = false;
        for (int i = 0; i <= kSegments; i++) {
            float angle = static_cast<float>(i) / kSegments * 2.f * static_cast<float>(M_PI);
            Vec3 point{step.x + step.radius * std::cos(angle), step.y + step.radius * std::sin(angle), step.z + 1.f};
            float sx, sy;
            if (!cam.Project(point, sx, sy)) { pen_down = false; continue; }
            if (pen_down) cairo_line_to(cr, sx, sy);
            else cairo_move_to(cr, sx, sy);
            pen_down = true;
        }
        double alpha = base_alpha * step.alpha / 0.75;
        cairo_set_line_width(cr, 3.5);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.35 * alpha);
        cairo_stroke_preserve(cr);
        cairo_set_line_width(cr, 1.8);
        cairo_set_source_rgba(cr, r, g, b, alpha);
        cairo_stroke(cr);
    }
}

static void DrawDroppedItems(cairo_t* cr, int width, int height) {
    if (!g_cfg || !settings::Enabled(g_cfg->esp_dropped_weapons)) return;
    const render::Camera& cam = s_camera;
    if (!cam.valid) return;
    std::vector<DroppedItemEntry> items;
    {
        std::lock_guard<std::mutex> lk(g_hud.items_mtx);
        items = g_hud.dropped_items;
    }
    float max_distance = std::max(1, g_cfg->weapon_esp_distance_m) / 0.01905f;
    uint32_t rgba = g_cfg->weapon_esp_rgba;
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11);
    for (const auto& item : items) {
        float dx = item.world_x - cam.eye.x, dy = item.world_y - cam.eye.y, dz = item.world_z - cam.eye.z;
        float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance > max_distance) continue;
        float sx, sy;
        if (!cam.Project(item.world_x, item.world_y, item.world_z, sx, sy)) continue;
        if (sx < 0.f || sx > width || sy < 0.f || sy > height) continue;
        char label[48];
        snprintf(label, sizeof(label), "%s %dm", item.name, static_cast<int>(distance * 0.01905f));
        cairo_text_extents_t te;
        cairo_text_extents(cr, label, &te);
        double tx = sx - te.width * 0.5;
        double ty = sy;
        cairo_set_source_rgba(cr, 0, 0, 0, 0.85);
        cairo_move_to(cr, tx + 1, ty + 1); cairo_show_text(cr, label);
        cairo_set_source_rgba(cr, ((rgba >> 24) & 0xFF) / 255.0, ((rgba >> 16) & 0xFF) / 255.0,
                              ((rgba >> 8) & 0xFF) / 255.0, (rgba & 0xFF) / 255.0);
        cairo_move_to(cr, tx, ty); cairo_show_text(cr, label);
    }
}

static void DrawRadar(cairo_t* cr) {
    if (!g_cfg || !settings::Enabled(g_cfg->radar) || !g_hud.in_game.load()) return;
    uintptr_t pawn = game::LocalPawn();
    if (!pawn || !off::m_angEyeAngles) return;
    constexpr double size = 190.0, range = 1800.0;
    if (g_cfg->hud_radar_x >= 0) { s_r_radar.x = g_cfg->hud_radar_x; s_r_radar.y = g_cfg->hud_radar_y; }
    s_r_radar.w = size; s_r_radar.h = size;
    double x = s_r_radar.x, y = s_r_radar.y, cx = x + size / 2, cy = y + size / 2, radius = size / 2 - 8;

    Vec3 origin = game::Origin(pawn);
    float yaw = g_proc.Read<Vec3>(pawn + off::m_angEyeAngles).y * 3.14159265f / 180.f;
    float cs = cosf(yaw), sn = sinf(yaw);
    auto to_radar = [&](float wx, float wy, double& px, double& py) {
        float dx = wx - origin.x, dy = wy - origin.y;
        double right = dx * sn - dy * cs, forward = dx * cs + dy * sn;
        double scale = radius / range;
        px = cx + right * scale;
        py = cy - forward * scale;
        return (px - cx) * (px - cx) + (py - cy) * (py - cy) <= radius * radius;
    };

    RoundedRect(cr, x, y, size, size, 12);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.55); cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08); cairo_set_line_width(cr, 1.0); cairo_stroke(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
    cairo_move_to(cr, cx, y + 8); cairo_line_to(cr, cx, y + size - 8);
    cairo_move_to(cr, x + 8, cy); cairo_line_to(cr, x + size - 8, cy);
    cairo_stroke(cr);

    std::vector<DroppedItemEntry> items;
    if (settings::Enabled(g_cfg->esp_dropped_weapons)) {
        std::lock_guard<std::mutex> lock(g_hud.items_mtx);
        items = g_hud.dropped_items;
    }
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 8);
    for (const auto& item : items) {
        double px, py;
        if (!to_radar(item.world_x, item.world_y, px, py)) continue;
        cairo_set_source_rgba(cr, 0.35, 0.85, 1.0, 0.95);
        cairo_rectangle(cr, px - 2.5, py - 2.5, 5, 5); cairo_fill(cr);
        cairo_move_to(cr, px + 4, py + 3); cairo_show_text(cr, item.name);
    }

    int my_team = g_hud.local_team.load();
    {
        std::lock_guard<std::mutex> lock(g_hud.esp_mtx);
        for (const auto& e : g_hud.esp_players) {
            if (my_team && e.team == my_team) continue;
            double px, py;
            if (!to_radar(e.world_feet_x, e.world_feet_y, px, py)) continue;
            cairo_set_source_rgba(cr, 1.0, 0.27, 0.23, 1.0);
            cairo_arc(cr, px, py, 3.5, 0, 2 * M_PI); cairo_fill(cr);
        }
    }

    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    cairo_move_to(cr, cx, cy - 6); cairo_line_to(cr, cx + 4.5, cy + 5); cairo_line_to(cr, cx - 4.5, cy + 5);
    cairo_close_path(cr); cairo_fill(cr);
}

static void DrawGrenadeTrajectory(cairo_t* cr, int width, int height) {
    if (!g_cfg || !settings::Enabled(g_cfg->grenade_trajectory)) return;
    std::vector<GrenadePoint> path;
    {
        std::lock_guard<std::mutex> lk(g_hud.grenade_mtx);
        path = g_hud.grenade_path;
    }
    if (path.size() < 2) return;
    const render::Camera& cam = s_camera;
    if (!cam.valid) return;
    std::vector<std::pair<float, float>> screen;
    std::vector<std::pair<float, float>> bounces;
    screen.reserve(path.size());
    for (size_t i = 0; i < path.size(); i++) {
        const auto& p = path[i];
        float sx, sy;
        if (!cam.Project(p.x, p.y, p.z, sx, sy)) continue;
        screen.push_back({sx, sy});
        if (p.bounce && i + 1 < path.size()) bounces.push_back({sx, sy});
    }
    if (screen.size() < 2) return;
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) { cairo_set_source_rgba(cr, 0, 0, 0, 0.6); cairo_set_line_width(cr, 4.0); }
        else           { cairo_set_source_rgba(cr, 1.0, 0.62, 0.2, 0.95); cairo_set_line_width(cr, 2.0); }
        cairo_move_to(cr, screen[0].first, screen[0].second);
        for (size_t i = 1; i < screen.size(); i++) cairo_line_to(cr, screen[i].first, screen[i].second);
        cairo_stroke(cr);
    }
    for (const auto& bounce : bounces) {
        cairo_arc(cr, bounce.first, bounce.second, 3.5, 0, 6.2831853);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
        cairo_fill(cr);
    }
    const auto& last = screen.back();
    cairo_arc(cr, last.first, last.second, 5.0, 0, 6.2831853);
    cairo_set_source_rgba(cr, 1.0, 0.3, 0.2, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
}

static gboolean OnDraw(GtkWidget* w, cairo_t* cr, gpointer) {
    GtkAllocation a; gtk_widget_get_allocation(w, &a);
    s_screen_w.store(a.width);
    s_screen_h.store(a.height);
    g_hud.screen_w.store(a.width);
    g_hud.screen_h.store(a.height);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    bool focused = IsCs2Active();
    g_hud.cs2_focused.store(focused);
    if (!focused && !settings::Enabled(g_cfg->edit_mode)) {
        return FALSE;
    }

    s_camera = g_hud.in_game.load() ? render::ReadCamera(a.width, a.height, std::clamp(g_cfg->render_lead_ms, 0, 200) / 1000.f) : render::Camera{};
    std::vector<world::LivePlayer> players;
    if (s_camera.valid) players = world::CapturePlayers(*g_cfg);
    world::DrawChams(cr, s_camera, players, *g_cfg);
    world::DrawEsp(cr, s_camera, players, *g_cfg);
    DrawDroppedItems(cr, a.width, a.height);
    DrawRadar(cr);
    DrawGrenadeTrajectory(cr, a.width, a.height);
    world::DrawArrows(cr, s_camera, players, *g_cfg);
    DrawSoundEsp(cr, s_camera);
    DrawWatermark(cr);
    DrawKeybinds(cr);
    DrawBomb(cr, a.width);
    DrawCrosshair(cr, a.width, a.height);
    DrawHitmarker(cr, a.width, a.height);
    DrawSpectators(cr, a.width);

    if (settings::Enabled(g_cfg->edit_mode)) {
        DrawEditMarker(cr, s_r_wm);
        DrawEditMarker(cr, s_r_bomb);
        DrawEditMarker(cr, s_r_keybinds);
        DrawEditMarker(cr, s_r_radar);
        DrawText(cr, 20, a.height - 34,
                 "Edit mode — drag HUD elements. Press F8 to finish.",
                 0.9, 0.9, 0.9, 1.0, 14, true);
    }

    s_frame_count++;
    gint64 now = g_get_monotonic_time();
    if (s_fps_last_us == 0) s_fps_last_us = now;
    if (now - s_fps_last_us >= 1000000) {
        g_hud.overlay_fps.store(s_frame_count);
        s_frame_count = 0;
        s_fps_last_us = now;
    }
    return FALSE;
}

static gboolean Tick(gpointer) {
    if (!s_running.load()) { gtk_main_quit(); return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

static gboolean OnFrameClock(GtkWidget* widget, GdkFrameClock*, gpointer) {
    gtk_widget_queue_draw(widget);
    return G_SOURCE_CONTINUE;
}

static bool HitRect(const Rect& r, double x, double y) {
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

static gboolean OnButton(GtkWidget*, GdkEventButton* e, gpointer) {
    if (!settings::Enabled(g_cfg->edit_mode)) return FALSE;
    if (e->type == GDK_BUTTON_PRESS && e->button == 1) {
        if (HitRect(s_r_wm, e->x, e->y)) {
            s_drag_target = 0;
            s_drag_off_x = e->x - s_r_wm.x;
            s_drag_off_y = e->y - s_r_wm.y;
        } else if (HitRect(s_r_bomb, e->x, e->y)) {
            s_drag_target = 1;
            s_drag_off_x = e->x - s_r_bomb.x;
            s_drag_off_y = e->y - s_r_bomb.y;
        } else if (HitRect(s_r_keybinds, e->x, e->y)) {
            s_drag_target = 2;
            s_drag_off_x = e->x - s_r_keybinds.x;
            s_drag_off_y = e->y - s_r_keybinds.y;
        } else if (HitRect(s_r_radar, e->x, e->y)) {
            s_drag_target = 3;
            s_drag_off_x = e->x - s_r_radar.x;
            s_drag_off_y = e->y - s_r_radar.y;
        }
    } else if (e->type == GDK_BUTTON_RELEASE) {
        if (s_drag_target == 0) {
            g_cfg->hud_wm_x = (int)s_r_wm.x;
            g_cfg->hud_wm_y = (int)s_r_wm.y;
        } else if (s_drag_target == 1) {
            g_cfg->hud_bomb_x = (int)s_r_bomb.x;
            g_cfg->hud_bomb_y = (int)s_r_bomb.y;
        } else if (s_drag_target == 2) {
            g_cfg->hud_keybinds_x = (int)s_r_keybinds.x;
            g_cfg->hud_keybinds_y = (int)s_r_keybinds.y;
        } else if (s_drag_target == 3) {
            g_cfg->hud_radar_x = (int)s_r_radar.x;
            g_cfg->hud_radar_y = (int)s_r_radar.y;
        }
        s_drag_target = -1;
    }
    return TRUE;
}

static gboolean OnMotion(GtkWidget*, GdkEventMotion* e, gpointer) {
    if (!settings::Enabled(g_cfg->edit_mode) || s_drag_target < 0) return FALSE;
    if (s_drag_target == 0) {
        s_r_wm.x = e->x - s_drag_off_x;
        s_r_wm.y = e->y - s_drag_off_y;
        g_cfg->hud_wm_x = (int)s_r_wm.x;
        g_cfg->hud_wm_y = (int)s_r_wm.y;
    } else if (s_drag_target == 1) {
        s_r_bomb.x = e->x - s_drag_off_x;
        s_r_bomb.y = e->y - s_drag_off_y;
        g_cfg->hud_bomb_x = (int)s_r_bomb.x;
        g_cfg->hud_bomb_y = (int)s_r_bomb.y;
    } else if (s_drag_target == 2) {
        s_r_keybinds.x = e->x - s_drag_off_x;
        s_r_keybinds.y = e->y - s_drag_off_y;
        g_cfg->hud_keybinds_x = (int)s_r_keybinds.x;
        g_cfg->hud_keybinds_y = (int)s_r_keybinds.y;
    } else if (s_drag_target == 3) {
        s_r_radar.x = e->x - s_drag_off_x;
        s_r_radar.y = e->y - s_drag_off_y;
        g_cfg->hud_radar_x = (int)s_r_radar.x;
        g_cfg->hud_radar_y = (int)s_r_radar.y;
    }
    return TRUE;
}

static gboolean OnRealize(GtkWidget*, gpointer) {
    ApplyInputMode();
    return FALSE;
}

static void SetupOverlayWindow(GtkWidget* win) {
    GdkScreen* scr = gtk_widget_get_screen(win);
    GdkVisual* v = gdk_screen_get_rgba_visual(scr);
    if (v) gtk_widget_set_visual(win, v);
    gtk_widget_set_app_paintable(win, TRUE);

    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP,    TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT,   TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT,  TRUE);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);
}

static const char* KeysymName(uint32_t k) {
    if (!k) return "None";
    const char* s = gdk_keyval_name(k);
    return s ? s : "?";
}

struct Rebind { uint32_t* field; GtkWidget* btn; GtkWidget* win; };

static gboolean RebindKey(GtkWidget*, GdkEventKey* e, gpointer data) {
    Rebind* rb = static_cast<Rebind*>(data);
    guint kv = e->keyval;
    if (kv == GDK_KEY_Escape) { *rb->field = 0; }
    else if (kv == GDK_KEY_Shift_L || kv == GDK_KEY_Shift_R ||
             kv == GDK_KEY_Control_L || kv == GDK_KEY_Control_R ||
             kv == GDK_KEY_Alt_L || kv == GDK_KEY_Alt_R ||
             kv == GDK_KEY_Super_L || kv == GDK_KEY_Super_R) {
        return TRUE;
    } else {
        *rb->field = kv;
    }
    gtk_button_set_label(GTK_BUTTON(rb->btn), KeysymName(*rb->field));
    gtk_widget_destroy(rb->win);
    g_free(rb);
    return TRUE;
}

static void OpenRebind(GtkButton* b, gpointer data) {
    uint32_t* field = static_cast<uint32_t*>(data);
    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Press a key");
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(win),
        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(b))));
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER_ON_PARENT);
    gtk_window_set_default_size(GTK_WINDOW(win), 300, 90);
    gtk_container_set_border_width(GTK_CONTAINER(win), 16);

    GtkWidget* lbl = gtk_label_new("Press any key…\n(Esc = clear bind)");
    gtk_container_add(GTK_CONTAINER(win), lbl);

    Rebind* rb = g_new0(Rebind, 1);
    rb->field = field;
    rb->btn = GTK_WIDGET(b);
    rb->win = win;

    gtk_widget_set_can_focus(win, TRUE);
    gtk_widget_add_events(win, GDK_KEY_PRESS_MASK);
    g_signal_connect(win, "key-press-event", G_CALLBACK(RebindKey), rb);
    g_signal_connect(win, "destroy",         G_CALLBACK(gtk_widget_destroyed), &rb->win);

    gtk_widget_show_all(win);
    gtk_widget_grab_focus(win);
}

struct ToggleBinding { GtkToggleButton* w; uint32_t* field; gulong handler; };
static std::vector<ToggleBinding> s_toggle_bindings;

static void OnBool(GtkToggleButton* b, gpointer field) {
    settings::SetEnabled(*static_cast<uint32_t*>(field), gtk_toggle_button_get_active(b));
    if (field == &g_cfg->edit_mode) ApplyInputMode();
}

static gboolean SyncToggles(gpointer) {
    static bool prev_edit_mode = false;
    bool curr_edit_mode = settings::Enabled(g_cfg->edit_mode);
    if (curr_edit_mode != prev_edit_mode) {
        prev_edit_mode = curr_edit_mode;
        ApplyInputMode();
    }
    if (!g_gui_win || !gtk_widget_get_visible(g_gui_win)) return G_SOURCE_CONTINUE;
    for (auto& b : s_toggle_bindings) {
        bool want = settings::Enabled(*b.field);
        bool have = gtk_toggle_button_get_active(b.w);
        if (want != have) {
            g_signal_handler_block(b.w, b.handler);
            gtk_toggle_button_set_active(b.w, want);
            g_signal_handler_unblock(b.w, b.handler);
        }
    }
    return G_SOURCE_CONTINUE;
}
static void OnInt(GtkSpinButton* sp, gpointer field) {
    *static_cast<int32_t*>(field) = gtk_spin_button_get_value_as_int(sp);
}
static void OnFov(GtkSpinButton* sp, gpointer field) {
    *static_cast<int32_t*>(field) = static_cast<int32_t>(gtk_spin_button_get_value(sp) * 100.0);
}
static void OnColor(GtkColorButton* button, gpointer field) {
    GdkRGBA color{};
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &color);
    uint32_t r = static_cast<uint32_t>(std::round(color.red * 255.0));
    uint32_t g = static_cast<uint32_t>(std::round(color.green * 255.0));
    uint32_t b = static_cast<uint32_t>(std::round(color.blue * 255.0));
    *static_cast<uint32_t*>(field) = (r << 24) | (g << 16) | (b << 8) | 0xFFu;
}

static GtkWidget* MakeIntRow(const char* label, int32_t* field, int lo, int hi, int step) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* lbl = gtk_label_new(label);
    gtk_widget_set_size_request(lbl, 140, -1);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.f);
    GtkWidget* sp  = gtk_spin_button_new_with_range(lo, hi, step);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp), *field);
    g_signal_connect(sp, "value-changed", G_CALLBACK(OnInt), field);
    gtk_box_pack_start(GTK_BOX(row), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), sp,  TRUE,  TRUE,  0);
    return row;
}
static GtkWidget* MakeFovRow(const char* label, int32_t* field_x100) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* lbl = gtk_label_new(label);
    gtk_widget_set_size_request(lbl, 140, -1);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.f);
    GtkWidget* sp  = gtk_spin_button_new_with_range(1.0, 180.0, 0.5);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(sp), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp), *field_x100 / 100.0);
    g_signal_connect(sp, "value-changed", G_CALLBACK(OnFov), field_x100);
    gtk_box_pack_start(GTK_BOX(row), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), sp,  TRUE,  TRUE,  0);
    return row;
}
static GtkWidget* MakeColorRow(const char* label, uint32_t* field) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* lbl = gtk_label_new(label);
    gtk_widget_set_size_request(lbl, 140, -1);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.f);
    uint32_t rgba = *field;
    GdkRGBA color{
        ((rgba >> 24) & 0xFF) / 255.0,
        ((rgba >> 16) & 0xFF) / 255.0,
        ((rgba >> 8) & 0xFF) / 255.0,
        1.0
    };
    GtkWidget* button = gtk_color_button_new_with_rgba(&color);
    g_signal_connect(button, "color-set", G_CALLBACK(OnColor), field);
    gtk_box_pack_start(GTK_BOX(row), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    return row;
}
static GtkWidget* MakeBindBtn(uint32_t* field) {
    GtkWidget* btn = gtk_button_new_with_label(KeysymName(*field));
    gtk_widget_set_size_request(btn, 90, -1);
    g_signal_connect(btn, "clicked", G_CALLBACK(OpenRebind), field);
    return btn;
}

static GtkWidget* MakeToggleBind(const char* label, uint32_t* field, uint32_t* bind) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* tog = gtk_check_button_new_with_label(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tog), settings::Enabled(*field));
    gulong h = g_signal_connect(tog, "toggled", G_CALLBACK(OnBool), field);
    s_toggle_bindings.push_back({GTK_TOGGLE_BUTTON(tog), field, h});
    gtk_widget_set_hexpand(tog, TRUE);
    gtk_box_pack_start(GTK_BOX(row), tog, TRUE,  TRUE,  0);
    if (bind) gtk_box_pack_end(GTK_BOX(row), MakeBindBtn(bind), FALSE, FALSE, 0);
    return row;
}
static GtkWidget* Section(const char* title) {
    GtkWidget* lbl = gtk_label_new(nullptr);
    char m[128]; snprintf(m, sizeof(m), "<b>%s</b>", title);
    gtk_label_set_markup(GTK_LABEL(lbl), m);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.f);
    return lbl;
}
static void OnQuit(GtkButton*, gpointer);
static GtkWidget* BuildGuiWindow();

static gboolean OnGuiClose(GtkWidget* widget, GdkEvent*, gpointer) {
    gtk_widget_hide(widget);
    return TRUE;
}

static GtkWidget* s_cfg_entry = nullptr;
static GtkWidget* s_cfg_combo = nullptr;
static GtkWidget* s_cfg_status = nullptr;
static int s_weapon_definition = 7;

struct WeaponOption { int definition; const char* name; };
static const WeaponOption s_weapon_options[] = {
    {1, "Desert Eagle"}, {4, "Glock-18"}, {7, "AK-47"}, {8, "AUG"},
    {9, "AWP"}, {10, "FAMAS"}, {11, "G3SG1"}, {13, "Galil AR"},
    {16, "M4A4"}, {17, "MAC-10"}, {19, "P90"}, {23, "MP5-SD"},
    {24, "UMP-45"}, {26, "PP-Bizon"}, {28, "Negev"}, {31, "Zeus"},
    {33, "MP7"}, {34, "MP9"}, {38, "SCAR-20"}, {39, "SG 553"},
    {40, "SSG 08"}, {60, "M4A1-S"}, {61, "USP-S"}, {64, "R8 Revolver"}
};

static GtkWidget* AddPage(GtkWidget* nb, const char* title) {
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 4);
    gtk_container_add(GTK_CONTAINER(scroll), box);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scroll, gtk_label_new(title));
    return box;
}

static void RefreshConfigList(const char* select) {
    if (!s_cfg_combo) return;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(s_cfg_combo));
    int active = -1;
    int i = 0;
    for (const auto& n : settings::ListConfigs()) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_cfg_combo), n.c_str());
        if (select && n == select) active = i;
        i++;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(s_cfg_combo), active);
}

static void SetCfgStatus(const char* m) {
    if (s_cfg_status) gtk_label_set_text(GTK_LABEL(s_cfg_status), m);
}

static void OnCfgSave(GtkButton*, gpointer) {
    if (!s_cfg_entry) return;
    const char* name = gtk_entry_get_text(GTK_ENTRY(s_cfg_entry));
    if (!name || !name[0]) { SetCfgStatus("Enter a name first"); return; }
    if (settings::SaveConfig(name)) {
        RefreshConfigList(name);
        SetCfgStatus("Saved");
    } else {
        SetCfgStatus("Bad name (a-z, 0-9, -, _)");
    }
}

static gboolean RebuildGuiIdle(gpointer) {
    s_toggle_bindings.clear();
    s_cfg_combo = nullptr;
    if (g_gui_win) gtk_widget_destroy(g_gui_win);
    g_gui_win = BuildGuiWindow();
    gtk_widget_show_all(g_gui_win);
    return G_SOURCE_REMOVE;
}

static void OnWeaponSelect(GtkComboBox* box, gpointer) {
    int index = gtk_combo_box_get_active(box);
    if (index < 0 || index >= static_cast<int>(sizeof(s_weapon_options) / sizeof(s_weapon_options[0]))) return;
    s_weapon_definition = s_weapon_options[index].definition;
    g_idle_add(RebuildGuiIdle, nullptr);
}

static void OnCfgLoad(GtkButton*, gpointer) {
    if (!s_cfg_combo) return;
    char* name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(s_cfg_combo));
    if (!name) { SetCfgStatus("Select a config first"); return; }
    bool ok = settings::LoadConfig(name);
    g_free(name);
    if (ok) {
        SetCfgStatus("Loaded");
        g_idle_add(RebuildGuiIdle, nullptr);
    } else {
        SetCfgStatus("Load failed");
    }
}

static void OnCfgDelete(GtkButton*, gpointer) {
    if (!s_cfg_combo) return;
    char* name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(s_cfg_combo));
    if (!name) { SetCfgStatus("Select a config first"); return; }
    bool ok = settings::DeleteConfig(name);
    g_free(name);
    RefreshConfigList(nullptr);
    SetCfgStatus(ok ? "Deleted" : "Delete failed");
}

static GtkWidget* BuildGuiWindow() {
    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Spaxer");
    gtk_window_set_default_size(GTK_WINDOW(win), 540, 720);
    gtk_container_set_border_width(GTK_CONTAINER(win), 12);
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_widget_set_app_paintable(win, TRUE);
    GdkScreen* screen = gtk_widget_get_screen(win);
    GdkVisual* visual = gdk_screen_get_rgba_visual(screen);
    if (visual) gtk_widget_set_visual(win, visual);
    g_signal_connect(win, "delete-event", G_CALLBACK(OnGuiClose), nullptr);

    GtkWidget* nb = gtk_notebook_new();
    gtk_container_add(GTK_CONTAINER(win), nb);

    GtkWidget* cfg = AddPage(nb, "Config");
    gtk_box_pack_start(GTK_BOX(cfg), Section("Config"), FALSE, FALSE, 0);
    GtkWidget* save_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    s_cfg_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(s_cfg_entry), "config name");
    gtk_entry_set_max_length(GTK_ENTRY(s_cfg_entry), 32);
    gtk_widget_set_hexpand(s_cfg_entry, TRUE);
    gtk_box_pack_start(GTK_BOX(save_row), s_cfg_entry, TRUE, TRUE, 0);
    GtkWidget* save_btn = gtk_button_new_with_label("Save");
    g_signal_connect(save_btn, "clicked", G_CALLBACK(OnCfgSave), nullptr);
    gtk_box_pack_end(GTK_BOX(save_row), save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cfg), save_row, FALSE, FALSE, 0);

    GtkWidget* load_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    s_cfg_combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(s_cfg_combo, TRUE);
    gtk_box_pack_start(GTK_BOX(load_row), s_cfg_combo, TRUE, TRUE, 0);
    GtkWidget* load_btn = gtk_button_new_with_label("Load");
    g_signal_connect(load_btn, "clicked", G_CALLBACK(OnCfgLoad), nullptr);
    gtk_box_pack_start(GTK_BOX(load_row), load_btn, FALSE, FALSE, 0);
    GtkWidget* del_btn = gtk_button_new_with_label("Delete");
    g_signal_connect(del_btn, "clicked", G_CALLBACK(OnCfgDelete), nullptr);
    gtk_box_pack_start(GTK_BOX(load_row), del_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cfg), load_row, FALSE, FALSE, 0);

    s_cfg_status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(s_cfg_status), 0.f);
    gtk_box_pack_start(GTK_BOX(cfg), s_cfg_status, FALSE, FALSE, 0);
    RefreshConfigList(settings::LastConfig().c_str());

    gtk_box_pack_start(GTK_BOX(cfg), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
    GtkWidget* quit = gtk_button_new_with_label("Quit / Uninject");
    GtkStyleContext* quit_ctx = gtk_widget_get_style_context(quit);
    gtk_style_context_add_class(quit_ctx, "destructive-action");
    g_signal_connect(quit, "clicked", G_CALLBACK(OnQuit), nullptr);
    gtk_box_pack_start(GTK_BOX(cfg), quit, FALSE, FALSE, 0);

    GtkWidget* vis = AddPage(nb, "Visuals");
    GtkWidget* gui_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* gui_lbl = gtk_label_new("Toggle GUI");
    gtk_label_set_xalign(GTK_LABEL(gui_lbl), 0.f);
    gtk_widget_set_hexpand(gui_lbl, TRUE);
    gtk_box_pack_start(GTK_BOX(gui_row), gui_lbl, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(gui_row), MakeBindBtn(&g_cfg->bind_toggle_gui), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vis), gui_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vis), Section("Visuals"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vis), MakeToggleBind("Watermark",  &g_cfg->watermark,  &g_cfg->bind_watermark), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vis), MakeToggleBind("Bomb timer", &g_cfg->bomb_timer, &g_cfg->bind_bomb),      FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vis), MakeToggleBind("Keybinds panel", &g_cfg->keybinds, &g_cfg->bind_keybinds), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vis), MakeToggleBind("Edit HUD (drag with mouse)", &g_cfg->edit_mode, &g_cfg->bind_edit_hud), FALSE, FALSE, 0);

    GtkWidget* esp = AddPage(nb, "ESP");
    gtk_box_pack_start(GTK_BOX(esp), Section("ESP"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(esp), MakeToggleBind("Enabled",     &g_cfg->esp,            &g_cfg->bind_esp), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(esp), MakeToggleBind("Box",         &g_cfg->esp_box,        nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(esp), MakeToggleBind("Health bar",  &g_cfg->esp_health,     nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(esp), MakeToggleBind("Name",        &g_cfg->esp_name,       nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(esp), MakeToggleBind("Weapon",      &g_cfg->esp_weapon,     nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(esp), MakeToggleBind("Skeleton",    &g_cfg->esp_skeleton,   nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(esp), MakeToggleBind("Head circle", &g_cfg->esp_head_circle,nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(esp), MakeToggleBind("Arrows (Cursor)", &g_cfg->arrows,     &g_cfg->bind_arrows), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(esp), MakeToggleBind("Item / Weapon ESP", &g_cfg->esp_dropped_weapons, nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(esp), MakeToggleBind("Grenade Trajectory", &g_cfg->grenade_trajectory, nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(esp), MakeToggleBind("Bone debug (all indices)", &g_cfg->esp_bone_debug, nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(esp), MakeToggleBind("Enemies only",&g_cfg->esp_team_check, nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(esp), MakeToggleBind("Spectators",  &g_cfg->spectators,     nullptr), FALSE, FALSE, 0);

    GtkWidget* ch = AddPage(nb, "Crosshair");
    gtk_box_pack_start(GTK_BOX(ch), Section("Crosshair"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ch), MakeToggleBind("Enabled", &g_cfg->crosshair, &g_cfg->bind_crosshair), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ch), MakeToggleBind("Sniper crosshair", &g_cfg->sniper_crosshair, nullptr), FALSE, FALSE, 0);
    GtkWidget* copy = gtk_button_new_with_label("Copy from CS2 (cl_crosshair*)");
    g_signal_connect(copy, "clicked", G_CALLBACK(+[](GtkButton*, gpointer){
        if (features::ReloadCs2Crosshair(g_cfg))
            fprintf(stderr, "[crosshair] loaded from cs2 user convars\n");
        else
            fprintf(stderr, "[crosshair] cs2_user_convars_0_slot0.vcfg not found\n");
    }), nullptr);
    gtk_box_pack_start(GTK_BOX(ch), copy, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ch), MakeIntRow("Size",      (int32_t*)&g_cfg->crosshair_size,      1, 40, 1), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ch), MakeIntRow("Gap",       (int32_t*)&g_cfg->crosshair_gap,     -10, 40, 1), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ch), MakeIntRow("Thickness", (int32_t*)&g_cfg->crosshair_thickness, 1, 10, 1), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ch), MakeToggleBind("Dot", &g_cfg->crosshair_dot, nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ch), MakeToggleBind("Outline", &g_cfg->crosshair_outline, nullptr), FALSE, FALSE, 0);

    GtkWidget* tb = AddPage(nb, "Trigger");
    gtk_box_pack_start(GTK_BOX(tb), Section("TriggerBot"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), MakeToggleBind("Enabled",        &g_cfg->trigger_enabled,         &g_cfg->bind_trigger), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), MakeToggleBind("Flash check",    &g_cfg->trigger_flash_check,     nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), MakeToggleBind("Aim correction", &g_cfg->trigger_aim_correction,  nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), MakeToggleBind("Crouch fire",    &g_cfg->trigger_shift_fire,      nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), MakeIntRow("Hitchance",  &g_cfg->trigger_hitchance, 0, 100, 1), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), MakeIntRow("Delay (ms)", &g_cfg->trigger_delay_ms,  0, 500, 5), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), MakeFovRow("Aim FOV",    &g_cfg->trigger_fov_x100), FALSE, FALSE, 0);

    GtkWidget* ab = AddPage(nb, "Aimbot");
    gtk_box_pack_start(GTK_BOX(ab), Section("Aimbot"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ab), MakeToggleBind("Enabled",     &g_cfg->aimbot_enabled,     &g_cfg->bind_aimbot), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ab), MakeToggleBind("Flash check", &g_cfg->aimbot_flash_check, nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ab), MakeToggleBind("Through walls", &g_cfg->aimbot_thru_walls, nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ab), MakeFovRow("Aim FOV",           &g_cfg->aimbot_fov_x100), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ab), MakeIntRow("Speed (1-100)",     &g_cfg->aimbot_smooth_x100, 5, 100, 1), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ab), MakeIntRow("Sensitivity (x1000 deg/px)", &g_cfg->aimbot_sens_x1000, 10, 500, 5), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ab), Section("Aim points"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ab), MakeToggleBind("Head",   &g_cfg->aimbot_point_head,   nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ab), MakeToggleBind("Neck",   &g_cfg->aimbot_point_neck,   nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ab), MakeToggleBind("Chest",  &g_cfg->aimbot_point_chest,  nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ab), MakeToggleBind("Pelvis", &g_cfg->aimbot_point_pelvis, nullptr), FALSE, FALSE, 0);

    GtkWidget* rcs = AddPage(nb, "RCS");
    gtk_box_pack_start(GTK_BOX(rcs), Section("Recoil control"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rcs), MakeToggleBind("Enabled", &g_cfg->rcs_enabled, nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rcs), MakeIntRow("Strength (%)", &g_cfg->rcs_strength_x100, 0, 100, 1), FALSE, FALSE, 0);

    GtkWidget* weapon = AddPage(nb, "Weapon");
    gtk_box_pack_start(GTK_BOX(weapon), Section("Per-weapon override"), FALSE, FALSE, 0);
    GtkWidget* selector = gtk_combo_box_text_new();
    int selected = 0;
    for (size_t i = 0; i < sizeof(s_weapon_options) / sizeof(s_weapon_options[0]); i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(selector), s_weapon_options[i].name);
        if (s_weapon_options[i].definition == s_weapon_definition) selected = static_cast<int>(i);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(selector), selected);
    g_signal_connect(selector, "changed", G_CALLBACK(OnWeaponSelect), nullptr);
    gtk_box_pack_start(GTK_BOX(weapon), selector, FALSE, FALSE, 0);
    WeaponSettings& selected_weapon = g_cfg->weapon_settings[s_weapon_definition];
    gtk_box_pack_start(GTK_BOX(weapon), MakeToggleBind("Use override", &selected_weapon.enabled, nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(weapon), MakeToggleBind("Aimbot", &selected_weapon.aimbot_enabled, nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(weapon), MakeFovRow("Aim FOV", &selected_weapon.aimbot_fov_x100), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(weapon), MakeIntRow("Aim speed", &selected_weapon.aimbot_smooth_x100, 5, 100, 1), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(weapon), MakeToggleBind("TriggerBot", &selected_weapon.trigger_enabled, nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(weapon), MakeIntRow("Hitchance", &selected_weapon.trigger_hitchance, 0, 100, 1), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(weapon), MakeIntRow("Delay (ms)", &selected_weapon.trigger_delay_ms, 0, 500, 5), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(weapon), MakeToggleBind("RCS", &selected_weapon.rcs_enabled, nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(weapon), MakeIntRow("RCS strength (%)", &selected_weapon.rcs_strength_x100, 0, 100, 1), FALSE, FALSE, 0);

    GtkWidget* unsafe = AddPage(nb, "Unsafe");
    gtk_box_pack_start(GTK_BOX(unsafe), Section("Unsafe"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(unsafe), MakeToggleBind("Thirdperson", &g_cfg->thirdperson, &g_cfg->bind_thirdperson), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(unsafe), MakeToggleBind("No flash", &g_cfg->no_flash, nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(unsafe), MakeToggleBind("No smoke", &g_cfg->no_smoke, nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(unsafe), MakeToggleBind("Smoke color", &g_cfg->smoke_color_enabled, nullptr), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(unsafe), MakeColorRow("Color", &g_cfg->smoke_color_rgba), FALSE, FALSE, 0);

    GtkWidget* radar = AddPage(nb, "Radar");
    gtk_box_pack_start(GTK_BOX(radar), Section("Radar Hack"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(radar), MakeToggleBind("Enabled", &g_cfg->radar_hack, nullptr), FALSE, FALSE, 0);

    GtkWidget* mv = AddPage(nb, "Movement");
    gtk_box_pack_start(GTK_BOX(mv), Section("Movement"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mv), MakeToggleBind("Bunny hop", &g_cfg->bunnyhop, &g_cfg->bind_bunnyhop), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mv), MakeToggleBind("Auto strafe", &g_cfg->auto_strafe, &g_cfg->bind_auto_strafe), FALSE, FALSE, 0);
    return win;
}

static void OnQuit(GtkButton*, gpointer) {
    s_running.store(false);
    if (g_gui_win)     gtk_widget_hide(g_gui_win);
    if (g_overlay_win) gtk_widget_hide(g_overlay_win);
    gtk_main_quit();
}

static void PanicShutdown(int) {
    features::StopTriggerBot();
    features::StopAimbot();
    features::StopRcs();
    features::StopMovement();
    g_input.Shutdown();
    _exit(1);
}

static gboolean HideGuiIfCs2Blurred(gpointer) {
    if (g_gui_win && gtk_widget_get_visible(g_gui_win) && !g_hud.cs2_focused.load()) {
        gtk_widget_hide(g_gui_win);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean ToggleGui(gpointer) {
    if (system("pgrep -x spaxer-gui >/dev/null 2>&1") == 0) {
        system("pkill -SIGUSR1 spaxer-gui 2>/dev/null");
    } else if (g_gui_win) {
        if (gtk_widget_get_visible(g_gui_win)) gtk_widget_hide(g_gui_win);
        else if (g_hud.cs2_focused.load())     gtk_widget_show_all(g_gui_win);
    }
    return G_SOURCE_REMOVE;
}
static gboolean ToggleEdit(gpointer) {
    settings::ToggleEnabled(g_cfg->edit_mode);
    ApplyInputMode();
    return G_SOURCE_REMOVE;
}
static gboolean ToggleTrigger(gpointer) {
    settings::ToggleEnabled(g_cfg->trigger_enabled);
    return G_SOURCE_REMOVE;
}
static gboolean ToggleWm(gpointer) {
    settings::ToggleEnabled(g_cfg->watermark); return G_SOURCE_REMOVE;
}
static gboolean ToggleBomb(gpointer) {
    settings::ToggleEnabled(g_cfg->bomb_timer); return G_SOURCE_REMOVE;
}
static gboolean ToggleCross(gpointer) {
    settings::ToggleEnabled(g_cfg->crosshair); return G_SOURCE_REMOVE;
}
static gboolean ToggleEsp(gpointer) {
    settings::ToggleEnabled(g_cfg->esp); return G_SOURCE_REMOVE;
}
static gboolean ToggleBunnyHop(gpointer) {
    settings::ToggleEnabled(g_cfg->bunnyhop); return G_SOURCE_REMOVE;
}
static gboolean OnAutoStrafeToggle(gpointer) {
    settings::ToggleEnabled(g_cfg->auto_strafe); return G_SOURCE_REMOVE;
}
static gboolean ToggleKeybinds(gpointer) {
    settings::ToggleEnabled(g_cfg->keybinds); return G_SOURCE_REMOVE;
}
static gboolean ToggleAimbot(gpointer) {
    settings::ToggleEnabled(g_cfg->aimbot_enabled); return G_SOURCE_REMOVE;
}
static gboolean ToggleChams(gpointer) {
    settings::ToggleEnabled(g_cfg->chams); return G_SOURCE_REMOVE;
}
static gboolean ToggleSoundEsp(gpointer) {
    settings::ToggleEnabled(g_cfg->sound_esp); return G_SOURCE_REMOVE;
}
static gboolean ToggleWeaponEsp(gpointer) {
    settings::ToggleEnabled(g_cfg->esp_dropped_weapons); return G_SOURCE_REMOVE;
}
static gboolean ToggleArrows(gpointer) {
    settings::ToggleEnabled(g_cfg->arrows); return G_SOURCE_REMOVE;
}
static gboolean ToggleGlow(gpointer) {
    settings::ToggleEnabled(g_cfg->glow); return G_SOURCE_REMOVE;
}

static void FetchThread() {
    while (s_running.load()) {
        if (!g_proc.IsAlive()) {
            dumper::Reset();
            g_hud.attached.store(false);
            g_hud.in_game.store(false);
            g_hud.local_ping.store(-1);
            g_hud.local_team.store(0);
            g_hud.bomb_visible.store(false);
            {
                std::lock_guard<std::mutex> lk(g_hud.esp_mtx);
                g_hud.esp_players.clear();
            }
            if (g_proc.Attach("cs2")) g_hud.attached.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        dumper::Run();
        features::UpdatePlayer();
        features::UpdateBomb();
        features::UpdateEsp();
        features::ApplyUnsafe();
        features::ApplyGlow();
        features::ApplyChams();
        features::ApplyRadarHack();
        features::UpdateHitmarker();
        features::UpdateSoundEsp();
        if (!dumper::ReadyForGameplay()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

static bool XkbToGdk(Display* d, uint32_t gdk_keyval, KeyCode& kc_out) {
    if (!gdk_keyval) return false;
    KeyCode kc = XKeysymToKeycode(d, (KeySym)gdk_keyval);
    if (!kc) return false;
    kc_out = kc;
    return true;
}

static int LinuxKeyFromGdk(uint32_t keyval) {
    switch (keyval) {
        case GDK_KEY_F1: return KEY_F1;
        case GDK_KEY_F2: return KEY_F2;
        case GDK_KEY_F3: return KEY_F3;
        case GDK_KEY_F4: return KEY_F4;
        case GDK_KEY_F5: return KEY_F5;
        case GDK_KEY_F6: return KEY_F6;
        case GDK_KEY_F7: return KEY_F7;
        case GDK_KEY_F8: return KEY_F8;
        case GDK_KEY_F9: return KEY_F9;
        case GDK_KEY_F10: return KEY_F10;
        case GDK_KEY_F11: return KEY_F11;
        case GDK_KEY_F12: return KEY_F12;
        case GDK_KEY_Insert: return KEY_INSERT;
        case GDK_KEY_Delete: return KEY_DELETE;
        case GDK_KEY_Home: return KEY_HOME;
        case GDK_KEY_End: return KEY_END;
        case GDK_KEY_Page_Up: return KEY_PAGEUP;
        case GDK_KEY_Page_Down: return KEY_PAGEDOWN;
        case GDK_KEY_Left: return KEY_LEFT;
        case GDK_KEY_Right: return KEY_RIGHT;
        case GDK_KEY_Up: return KEY_UP;
        case GDK_KEY_Down: return KEY_DOWN;
        case GDK_KEY_space: return KEY_SPACE;
        case GDK_KEY_Shift_L:
        case GDK_KEY_Shift_R: return KEY_LEFTSHIFT;
        case GDK_KEY_a: case GDK_KEY_A: return KEY_A;
        case GDK_KEY_b: case GDK_KEY_B: return KEY_B;
        case GDK_KEY_c: case GDK_KEY_C: return KEY_C;
        case GDK_KEY_d: case GDK_KEY_D: return KEY_D;
        case GDK_KEY_e: case GDK_KEY_E: return KEY_E;
        case GDK_KEY_f: case GDK_KEY_F: return KEY_F;
        case GDK_KEY_g: case GDK_KEY_G: return KEY_G;
        case GDK_KEY_h: case GDK_KEY_H: return KEY_H;
        case GDK_KEY_i: case GDK_KEY_I: return KEY_I;
        case GDK_KEY_j: case GDK_KEY_J: return KEY_J;
        case GDK_KEY_k: case GDK_KEY_K: return KEY_K;
        case GDK_KEY_l: case GDK_KEY_L: return KEY_L;
        case GDK_KEY_m: case GDK_KEY_M: return KEY_M;
        case GDK_KEY_n: case GDK_KEY_N: return KEY_N;
        case GDK_KEY_o: case GDK_KEY_O: return KEY_O;
        case GDK_KEY_p: case GDK_KEY_P: return KEY_P;
        case GDK_KEY_q: case GDK_KEY_Q: return KEY_Q;
        case GDK_KEY_r: case GDK_KEY_R: return KEY_R;
        case GDK_KEY_s: case GDK_KEY_S: return KEY_S;
        case GDK_KEY_t: case GDK_KEY_T: return KEY_T;
        case GDK_KEY_u: case GDK_KEY_U: return KEY_U;
        case GDK_KEY_v: case GDK_KEY_V: return KEY_V;
        case GDK_KEY_w: case GDK_KEY_W: return KEY_W;
        case GDK_KEY_x: case GDK_KEY_X: return KEY_X;
        case GDK_KEY_y: case GDK_KEY_Y: return KEY_Y;
        case GDK_KEY_z: case GDK_KEY_Z: return KEY_Z;
        default: return -1;
    }
}

static void HotkeyThread() {
    Display* d = XOpenDisplay(nullptr);
    bool prev[16] = {};
    while (s_running.load()) {
        struct { uint32_t kv; GSourceFunc fn; } binds[] = {
            { g_cfg->bind_toggle_gui, ToggleGui     },
            { g_cfg->bind_edit_hud,   ToggleEdit    },
            { g_cfg->bind_trigger,    ToggleTrigger },
            { g_cfg->bind_watermark,  ToggleWm      },
            { g_cfg->bind_bomb,       ToggleBomb    },
            { g_cfg->bind_crosshair,  ToggleCross   },
            { g_cfg->bind_esp,        ToggleEsp     },
            { g_cfg->bind_bunnyhop,   ToggleBunnyHop },
            { g_cfg->bind_auto_strafe, OnAutoStrafeToggle },
            { g_cfg->bind_keybinds,    ToggleKeybinds },
            { g_cfg->bind_aimbot,      ToggleAimbot   },
            { g_cfg->bind_glow,        ToggleGlow     },
            { g_cfg->bind_chams,       ToggleChams    },
            { g_cfg->bind_sound_esp,   ToggleSoundEsp },
            { g_cfg->bind_weapon_esp,  ToggleWeaponEsp },
            { g_cfg->bind_arrows,      ToggleArrows   },
        };
        char keys[32]{};
        if (d) XQueryKeymap(d, keys);
        for (size_t i = 0; i < sizeof(binds)/sizeof(binds[0]); i++) {
            bool now = false;
            int linux_key = LinuxKeyFromGdk(binds[i].kv);
            if (linux_key >= 0)
                now = g_input.IsKeyDown(linux_key);
            if (!now && d) {
                KeyCode kc;
                if (XkbToGdk(d, binds[i].kv, kc))
                    now = (keys[kc / 8] & (1 << (kc & 7))) != 0;
            }
            if (now && !prev[i]) g_idle_add(binds[i].fn, nullptr);
            prev[i] = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    if (d) XCloseDisplay(d);
}

static void InstallCss() {
    static const char* css =
        "* { font-family: 'SF Pro Display', 'Inter', 'Segoe UI', -apple-system, sans-serif; }"
        "window { background: rgba(28, 28, 32, 0.72); color: #f2f2f7; border-radius: 14px; }"
        "#click-gui { background: rgba(28, 28, 32, 0.94); border-radius: 14px; box-shadow: 0 20px 50px rgba(0,0,0,0.5); }"
        "window decoration { border-radius: 14px; box-shadow: 0 20px 50px rgba(0,0,0,0.5); }"
        "notebook { background: transparent; padding: 4px; }"
        "notebook header { background: rgba(255,255,255,0.04); border: none; padding: 4px; border-radius: 10px; margin-bottom: 8px; }"
        "notebook tab { padding: 8px 16px; color: #8e8e93; background: transparent; border: none; border-radius: 8px; margin: 0 2px; font-weight: 500; }"
        "notebook tab:checked { color: #ffffff; background: rgba(255,255,255,0.12); }"
        "notebook tab:hover:not(:checked) { color: #d1d1d6; background: rgba(255,255,255,0.05); }"
        "scrolledwindow, viewport { background: transparent; }"
        "label { color: #f2f2f7; font-size: 12px; }"
        "label.section { color: #ffffff; font-weight: 600; font-size: 13px; margin: 8px 0 4px 0; }"
        "checkbutton { padding: 6px 8px; border-radius: 8px; color: #f2f2f7; }"
        "checkbutton:hover { background: rgba(255,255,255,0.06); }"
        "checkbutton check { min-width: 18px; min-height: 18px; margin-right: 8px; "
        "  background: rgba(120,120,128,0.32); border: none; border-radius: 9px; -gtk-icon-source: none; }"
        "checkbutton check:checked { background: linear-gradient(135deg, #007aff, #5ac8fa); "
        "  border: none; -gtk-icon-source: none; box-shadow: 0 0 8px rgba(0,122,255,0.4); }"
        "button { background: rgba(120,120,128,0.24); color: #f2f2f7; border: none; "
        "  border-radius: 8px; padding: 7px 14px; font-weight: 500; }"
        "button:hover { background: rgba(120,120,128,0.36); }"
        "button:active, button:checked { background: #007aff; color: white; }"
        "button.destructive-action { background: rgba(255,59,48,0.22); color: #ff6b6b; }"
        "button.destructive-action:hover { background: rgba(255,59,48,0.35); }"
        "spinbutton { background: rgba(0,0,0,0.24); color: #f2f2f7; border-radius: 8px; border: none; }"
        "spinbutton entry { background: transparent; color: #f2f2f7; padding: 6px 8px; caret-color: #007aff; }"
        "spinbutton button { border-radius: 0; padding: 0 8px; background: transparent; }"
        "spinbutton button:hover { background: rgba(255,255,255,0.06); }"
        "entry { background: rgba(0,0,0,0.24); color: #f2f2f7; border: none; border-radius: 8px; padding: 6px 10px; caret-color: #007aff; }"
        "entry:focus { background: rgba(0,0,0,0.32); box-shadow: 0 0 0 2px rgba(0,122,255,0.4); }"
        "combobox button { padding: 6px 10px; background: rgba(0,0,0,0.24); border-radius: 8px; }"
        "combobox arrow { color: #8e8e93; }"
        "separator { background: rgba(255,255,255,0.08); min-height: 1px; margin: 8px 0; }"
        "scrollbar { background: transparent; }"
        "scrollbar slider { min-width: 6px; min-height: 6px; background: rgba(255,255,255,0.24); border-radius: 3px; }"
        "scrollbar slider:hover { background: rgba(255,255,255,0.4); }"
        "scale trough { background: rgba(0,0,0,0.24); border-radius: 3px; min-height: 4px; }"
        "scale highlight { background: linear-gradient(90deg, #007aff, #5ac8fa); border-radius: 3px; }"
        "scale slider { background: white; border-radius: 50%; min-width: 16px; min-height: 16px; box-shadow: 0 2px 4px rgba(0,0,0,0.3); }";
    GtkCssProvider* p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, css, -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

int main(int argc, char** argv) {
    std::signal(SIGINT,  PanicShutdown);
    std::signal(SIGTERM, PanicShutdown);
    std::signal(SIGSEGV, PanicShutdown);
    std::signal(SIGABRT, PanicShutdown);

    if (!TakeInstanceLock()) {
        fprintf(stderr, "spaxer: another instance is already running\n");
        return 1;
    }

    EnsurePtraceScope();
    gtk_init(&argc, &argv);
    InstallCss();

    g_cfg = settings::Attach();
    if (!g_cfg) { fprintf(stderr, "settings mmap failed\n"); return 1; }
    std::string last = settings::LastConfig();
    if (!last.empty()) settings::LoadConfig(last);
    g_cfg->bind_toggle_gui = GDK_KEY_Insert;
    if (!g_cfg->bind_edit_hud) g_cfg->bind_edit_hud = GDK_KEY_F8;
    g_cfg->edit_mode = 0;

    if (g_cfg->hud_wm_x >= 0)   { s_r_wm.x   = g_cfg->hud_wm_x;   s_r_wm.y   = g_cfg->hud_wm_y; }
    if (g_cfg->hud_bomb_x >= 0) { s_r_bomb.x = g_cfg->hud_bomb_x; s_r_bomb.y = g_cfg->hud_bomb_y; }
    if (g_cfg->hud_keybinds_x >= 0) { s_r_keybinds.x = g_cfg->hud_keybinds_x; s_r_keybinds.y = g_cfg->hud_keybinds_y; }

    const char* offsets_path = "offsets.json";
    if (argc >= 2) offsets_path = argv[1];
    off::SetJsonPath(offsets_path);

    g_input.Init();

    g_overlay_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_overlay_win), "spaxer-overlay");
    gtk_window_set_decorated(GTK_WINDOW(g_overlay_win), FALSE);
    gtk_window_set_accept_focus(GTK_WINDOW(g_overlay_win), FALSE);
    SetupOverlayWindow(g_overlay_win);

    g_area = gtk_drawing_area_new();
    gtk_widget_add_events(g_area,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    gtk_container_add(GTK_CONTAINER(g_overlay_win), g_area);
    g_signal_connect(g_area,        "draw",                 G_CALLBACK(OnDraw),    nullptr);
    g_signal_connect(g_area,        "button-press-event",   G_CALLBACK(OnButton),  nullptr);
    g_signal_connect(g_area,        "button-release-event", G_CALLBACK(OnButton),  nullptr);
    g_signal_connect(g_area,        "motion-notify-event",  G_CALLBACK(OnMotion),  nullptr);
    g_signal_connect(g_overlay_win, "realize",              G_CALLBACK(OnRealize), nullptr);
    g_signal_connect(g_overlay_win, "destroy",              G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(g_overlay_win);
    g_gui_win = BuildGuiWindow();
    ApplyInputMode();

    features::StartTriggerBot();
    features::StartAimbot();
    features::StartRcs();
    features::StartMovement();
    std::thread ft(FetchThread);
    std::thread ht(HotkeyThread);

    g_timeout_add(100, Tick, nullptr);
    gtk_widget_add_tick_callback(g_area, OnFrameClock, nullptr, nullptr);
    g_timeout_add(150, SyncToggles, nullptr);
    g_timeout_add(200, HideGuiIfCs2Blurred, nullptr);
    gtk_main();

    s_running.store(false);
    if (ft.joinable()) ft.join();
    if (ht.joinable()) ht.join();
    features::StopTriggerBot();
    features::StopAimbot();
    features::StopRcs();
    features::StopMovement();
    g_input.Shutdown();
    return 0;
}
