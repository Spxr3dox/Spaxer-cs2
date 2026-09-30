#include "state.h"
#include "config/settings.h"
#include "features/features.h"
#include "features/hitmarker.h"
#include "features/sound_esp.h"
#include "memory/process.h"
#include "sdk/offsets.h"
#include "sdk/game.h"
#include "sdk/dumper.h"
#include "sdk/visibility.h"
#include "overlay/weather.h"
#include "overlay/night_sky.h"
#include "overlay/grenade.h"
#include "overlay/saturation.h"
#include "overlay/crosshair_capture.h"
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
#include <X11/extensions/Xfixes.h>
#include <linux/input.h>
#include <cairo.h>
#include <pango/pangocairo.h>
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
#include <sys/stat.h>
#include <string>
#include <ctime>

static std::atomic<bool> s_running{true};
static Settings* g_cfg = nullptr;
static int s_lock_fd = -1;
static GtkWidget* g_area = nullptr;
static GtkWidget* g_overlay_win = nullptr;
static GtkWidget* g_gui_win = nullptr;

static void InstallCss();
static bool IsCs2Active();

struct Rect { double x, y, w, h; };
static Rect s_r_wm{20, 20, 0, 0};
static Rect s_r_bomb{0, 24, 0, 0};
static Rect s_r_keybinds{20, 100, 0, 0};
static Rect s_r_radar{20, 320, 0, 0};
static Rect s_r_spectators{0, 46, 0, 0};
static Rect s_r_media{20, 240, 0, 0};
static Rect s_r_velocity{0, 0, 0, 0};
static Rect s_r_keys{0, 0, 0, 0};
static Rect s_r_notices{0, 0, 0, 0};

static int    s_drag_target = -1;
static double s_drag_off_x = 0, s_drag_off_y = 0;

static int    s_frame_count = 0;
static gint64 s_fps_last_us = 0;

static std::atomic<int> s_screen_w{1920};
static std::atomic<int> s_screen_h{1080};
static std::atomic<bool> s_cursor_active{false};

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

static void UpdateInputShape() {
    if (!g_overlay_win) return;
    GdkWindow* gw = gtk_widget_get_window(g_overlay_win);
    if (!gw) return;

    static cairo_region_t* s_last_reg = nullptr;
    bool allow_input = settings::Enabled(g_cfg->edit_mode) || (IsCs2Active() && s_cursor_active.load());
    if (!allow_input) {
        if (s_last_reg) {
            cairo_region_destroy(s_last_reg);
            s_last_reg = nullptr;
        }
        gdk_window_set_pass_through(gw, TRUE);
        cairo_region_t* empty = cairo_region_create();
        gdk_window_input_shape_combine_region(gw, empty, 0, 0);
        cairo_region_destroy(empty);
        return;
    }

    cairo_region_t* reg = cairo_region_create();
    auto add_rect = [&](const Rect& rc) {
        if (rc.w <= 0 || rc.h <= 0) return;
        cairo_rectangle_int_t r{ (int)std::floor(rc.x), (int)std::floor(rc.y), (int)std::ceil(rc.w), (int)std::ceil(rc.h) };
        cairo_region_union_rectangle(reg, &r);
    };

    if (settings::Enabled(g_cfg->watermark)) add_rect(s_r_wm);
    if (settings::Enabled(g_cfg->bomb_timer)) add_rect(s_r_bomb);
    if (settings::Enabled(g_cfg->keybinds)) add_rect(s_r_keybinds);
    if (settings::Enabled(g_cfg->radar)) add_rect(s_r_radar);
    if (settings::Enabled(g_cfg->velocity_graph)) add_rect(s_r_velocity);
    if (settings::Enabled(g_cfg->spectators)) add_rect(s_r_spectators);
    if (settings::Enabled(g_cfg->media_player)) add_rect(s_r_media);
    if (settings::Enabled(g_cfg->keystrokes)) add_rect(s_r_keys);
    if (settings::Enabled(g_cfg->notifications)) add_rect(s_r_notices);

    if (s_last_reg && cairo_region_equal(reg, s_last_reg)) {
        cairo_region_destroy(reg);
        return;
    }
    if (s_last_reg) cairo_region_destroy(s_last_reg);
    s_last_reg = cairo_region_copy(reg);

    gdk_window_set_pass_through(gw, FALSE);
    gdk_window_input_shape_combine_region(gw, reg, 0, 0);
    cairo_region_destroy(reg);
}

static void ApplyInputMode() {
    UpdateInputShape();
}

static void DrawText(cairo_t* cr, double x, double y, const char* s,
                     double r, double g, double b, double a, double size, bool bold) {
    cairo_select_font_face(cr, "Roboto", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    cairo_move_to(cr, std::round(x), std::round(y + fe.ascent));
    cairo_show_text(cr, s);
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

double kAccentR = 0.30, kAccentG = 0.55, kAccentB = 1.0;

static void UpdateAccentFromTheme() {
    if (!g_cfg) return;
    uint32_t rgba = g_cfg->hud_accent_rgba;
    if (rgba == 0) {
        switch (g_cfg->hud_theme) {
            case 1: rgba = 0xB47CFFFF; break;
            case 2: rgba = 0x60D68CFF; break;
            case 3: rgba = 0xFF6B6BFF; break;
            case 4: rgba = 0xFFA24DFF; break;
            case 5: rgba = 0xFF7BC8FF; break;
            case 6: rgba = 0x50E3F2FF; break;
            default: rgba = 0x4C8DFFFF; break;
        }
    }
    kAccentR = ((rgba >> 24) & 0xFF) / 255.0;
    kAccentG = ((rgba >> 16) & 0xFF) / 255.0;
    kAccentB = ((rgba >>  8) & 0xFF) / 255.0;
}

static double s_hud_scale = 1.0;
static double s_hud_dt = 0.0;

static double U(double value) { return std::round(value * s_hud_scale); }

static void HudBeginFrame(int height) {
    static gint64 last_us = 0;
    gint64 now = g_get_monotonic_time();
    s_hud_dt = last_us ? std::min(0.1, (now - last_us) / 1e6) : 0.0;
    last_us = now;
    s_hud_scale = std::clamp(height / 1440.0, 0.8, 2.0);
}

static double Approach(double current, double target, double speed = 14.0) {
    double next = current + (target - current) * (1.0 - std::exp(-speed * s_hud_dt));
    return std::abs(next - target) < 0.002 ? target : next;
}

static double EaseOut(double t) {
    return 1.0 - std::pow(1.0 - std::clamp(t, 0.0, 1.0), 3.0);
}

static PangoLayout* HudLayout(cairo_t* cr, const char* text, double px, bool bold) {
    static cairo_font_options_t* options = [] {
        cairo_font_options_t* o = cairo_font_options_create();
        cairo_font_options_set_antialias(o, CAIRO_ANTIALIAS_GRAY);
        cairo_font_options_set_hint_style(o, CAIRO_HINT_STYLE_SLIGHT);
        cairo_font_options_set_hint_metrics(o, CAIRO_HINT_METRICS_ON);
        return o;
    }();
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_cairo_context_set_font_options(pango_layout_get_context(layout), options);
    pango_layout_context_changed(layout);
    PangoFontDescription* desc = pango_font_description_from_string("Roboto");
    pango_font_description_set_weight(desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_MEDIUM);
    pango_font_description_set_absolute_size(desc, px * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    const char* safe_text = text ? text : "";
    if (!g_utf8_validate(safe_text, -1, nullptr)) safe_text = "";
    pango_layout_set_text(layout, safe_text, -1);
    return layout;
}

static double HudTextWidth(cairo_t* cr, const char* text, double px, bool bold) {
    PangoLayout* layout = HudLayout(cr, text, px, bold);
    int w, h;
    pango_layout_get_pixel_size(layout, &w, &h);
    g_object_unref(layout);
    return w;
}

static double HudText(cairo_t* cr, double x, double center_y, const char* text, double px, bool bold,
                      double r, double g, double b, double a) {
    PangoLayout* layout = HudLayout(cr, text, px, bold);
    int w, h;
    pango_layout_get_pixel_size(layout, &w, &h);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_move_to(cr, std::round(x), std::round(center_y - h / 2.0));
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    return w;
}

static void HudGlass(cairo_t* cr, double x, double y, double w, double h, double radius, double alpha) {
    for (int i = 3; i >= 1; i--) {
        RoundedRect(cr, x - i, y - i + 1, w + i * 2, h + i * 2, radius + i);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.05 * alpha);
        cairo_fill(cr);
    }
    cairo_pattern_t* fill = cairo_pattern_create_linear(0, y, 0, y + h);
    cairo_pattern_add_color_stop_rgba(fill, 0.0, 0.07, 0.08, 0.11, 0.6 * alpha);
    cairo_pattern_add_color_stop_rgba(fill, 1.0, 0.03, 0.035, 0.05, 0.62 * alpha);
    RoundedRect(cr, x, y, w, h, radius);
    cairo_set_source(cr, fill);
    cairo_fill(cr);
    cairo_pattern_destroy(fill);
    cairo_pattern_t* rim = cairo_pattern_create_linear(0, y, 0, y + h);
    cairo_pattern_add_color_stop_rgba(rim, 0.0, 1, 1, 1, 0.12 * alpha);
    cairo_pattern_add_color_stop_rgba(rim, 1.0, 1, 1, 1, 0.03 * alpha);
    RoundedRect(cr, x + 0.5, y + 0.5, w - 1, h - 1, radius);
    cairo_set_source(cr, rim);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_pattern_destroy(rim);
}

static void HudPanel(cairo_t* cr, double x, double y, double w, double h, double alpha = 1.0) {
    HudGlass(cr, x, y, w, h, U(7), alpha);
}

static void HudRowBackground(cairo_t* cr, double x, double y, double w, double h, double alpha) {
    HudGlass(cr, x, y, w, h, U(6), alpha);
}

static void HudAccent(cairo_t* cr, double alpha = 1.0) {
    cairo_set_source_rgba(cr, kAccentR, kAccentG, kAccentB, alpha);
}

enum class HudIcon { Fps, Ping, Clock, User, List, Toggle, Hold, Eye, Bomb, Defuse, Music, Prev, Next, Play, Pause };

static void DrawHudIcon(cairo_t* cr, HudIcon icon, double cx, double cy, double s, double alpha = 1.0) {
    const double pi = 3.14159265358979;
    cairo_save(cr);
    cairo_new_path(cr);
    HudAccent(cr, alpha);
    cairo_set_line_width(cr, std::max(1.2, s * 0.13));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    double h = s / 2;
    switch (icon) {
        case HudIcon::Fps:
            for (int i = 0; i < 3; i++) {
                double bx = cx - h + i * s * 0.38, bh = s * (0.4 + i * 0.3);
                cairo_rectangle(cr, bx, cy + h - bh, s * 0.24, bh);
            }
            cairo_fill(cr);
            break;
        case HudIcon::Ping:
            for (int i = 0; i < 4; i++) {
                double bx = cx - h + i * s * 0.28, bh = s * (0.25 + i * 0.25);
                cairo_rectangle(cr, bx, cy + h - bh, s * 0.16, bh);
            }
            cairo_fill(cr);
            break;
        case HudIcon::Clock:
            cairo_arc(cr, cx, cy, h, 0, 2 * pi);
            cairo_stroke(cr);
            cairo_move_to(cr, cx, cy - h * 0.55);
            cairo_line_to(cr, cx, cy);
            cairo_line_to(cr, cx + h * 0.45, cy + h * 0.2);
            cairo_stroke(cr);
            break;
        case HudIcon::User:
            cairo_arc(cr, cx, cy - h * 0.35, h * 0.42, 0, 2 * pi);
            cairo_fill(cr);
            cairo_arc(cr, cx, cy + h * 1.05, h * 0.85, pi, 2 * pi);
            cairo_fill(cr);
            break;
        case HudIcon::List:
            for (int i = 0; i < 3; i++) {
                double ly = cy - h * 0.7 + i * h * 0.7;
                cairo_arc(cr, cx - h * 0.8, ly, s * 0.08, 0, 2 * pi);
                cairo_fill(cr);
                cairo_move_to(cr, cx - h * 0.4, ly);
                cairo_line_to(cr, cx + h, ly);
                cairo_stroke(cr);
            }
            break;
        case HudIcon::Toggle:
            RoundedRect(cr, cx - h, cy - h * 0.55, s, h * 1.1, h * 0.55);
            cairo_stroke(cr);
            cairo_arc(cr, cx + h * 0.42, cy, h * 0.3, 0, 2 * pi);
            cairo_fill(cr);
            break;
        case HudIcon::Hold:
            cairo_arc(cr, cx, cy, h * 0.9, 0, 2 * pi);
            cairo_stroke(cr);
            cairo_arc(cr, cx, cy, h * 0.4, 0, 2 * pi);
            cairo_fill(cr);
            break;
        case HudIcon::Eye:
            cairo_move_to(cr, cx - h, cy);
            cairo_curve_to(cr, cx - h * 0.5, cy - h * 0.8, cx + h * 0.5, cy - h * 0.8, cx + h, cy);
            cairo_curve_to(cr, cx + h * 0.5, cy + h * 0.8, cx - h * 0.5, cy + h * 0.8, cx - h, cy);
            cairo_stroke(cr);
            cairo_arc(cr, cx, cy, h * 0.3, 0, 2 * pi);
            cairo_fill(cr);
            break;
        case HudIcon::Bomb:
            cairo_arc(cr, cx - h * 0.1, cy + h * 0.15, h * 0.72, 0, 2 * pi);
            cairo_fill(cr);
            cairo_move_to(cr, cx + h * 0.35, cy - h * 0.4);
            cairo_curve_to(cr, cx + h * 0.6, cy - h * 0.9, cx + h * 0.9, cy - h * 0.7, cx + h, cy - h);
            cairo_stroke(cr);
            break;
        case HudIcon::Defuse:
            cairo_move_to(cr, cx - h, cy + h);
            cairo_line_to(cr, cx + h * 0.2, cy - h * 0.2);
            cairo_stroke(cr);
            cairo_arc(cr, cx + h * 0.45, cy - h * 0.45, h * 0.45, 0, 2 * pi);
            cairo_stroke(cr);
            break;
        case HudIcon::Music:
            cairo_arc(cr, cx - h * 0.4, cy + h * 0.4, h * 0.35, 0, 2 * pi);
            cairo_fill(cr);
            cairo_arc(cr, cx + h * 0.5, cy + h * 0.1, h * 0.35, 0, 2 * pi);
            cairo_fill(cr);
            cairo_move_to(cr, cx - h * 0.05, cy + h * 0.4);
            cairo_line_to(cr, cx - h * 0.05, cy - h * 0.6);
            cairo_line_to(cr, cx + h * 0.85, cy - h * 0.9);
            cairo_line_to(cr, cx + h * 0.85, cy + h * 0.1);
            cairo_stroke(cr);
            cairo_rectangle(cr, cx - h * 0.05, cy - h * 0.9, h * 0.9, h * 0.35);
            cairo_fill(cr);
            break;
        case HudIcon::Prev:
            cairo_rectangle(cr, cx - h, cy - h * 0.7, s * 0.15, s * 0.7);
            cairo_fill(cr);
            cairo_move_to(cr, cx + h * 0.8, cy - h * 0.7);
            cairo_line_to(cr, cx - h * 0.5, cy);
            cairo_line_to(cr, cx + h * 0.8, cy + h * 0.7);
            cairo_close_path(cr);
            cairo_fill(cr);
            break;
        case HudIcon::Next:
            cairo_rectangle(cr, cx + h - s * 0.15, cy - h * 0.7, s * 0.15, s * 0.7);
            cairo_fill(cr);
            cairo_move_to(cr, cx - h * 0.8, cy - h * 0.7);
            cairo_line_to(cr, cx + h * 0.5, cy);
            cairo_line_to(cr, cx - h * 0.8, cy + h * 0.7);
            cairo_close_path(cr);
            cairo_fill(cr);
            break;
        case HudIcon::Play:
            cairo_move_to(cr, cx - h * 0.5, cy - h * 0.7);
            cairo_line_to(cr, cx + h * 0.8, cy);
            cairo_line_to(cr, cx - h * 0.5, cy + h * 0.7);
            cairo_close_path(cr);
            cairo_fill(cr);
            break;
        case HudIcon::Pause:
            cairo_rectangle(cr, cx - h * 0.65, cy - h * 0.7, s * 0.25, s * 0.7);
            cairo_rectangle(cr, cx + h * 0.15, cy - h * 0.7, s * 0.25, s * 0.7);
            cairo_fill(cr);
            break;
    }
    cairo_restore(cr);
}

static std::string SteamPersona() {
    const char* home = getenv("HOME");
    std::string path = std::string(home ? home : "") + "/.local/share/Steam/config/loginusers.vdf";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return {};
    std::string name, recent;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char key[256]{}, value[256]{};
        if (sscanf(line, " \"%255[^\"]\" \"%255[^\"]\"", key, value) != 2) continue;
        if (!strcmp(key, "PersonaName")) name = value;
        if (!strcmp(key, "MostRecent") && !strcmp(value, "1") && recent.empty()) recent = name;
    }
    fclose(f);
    return recent.empty() ? name : recent;
}

static void DrawWatermark(cairo_t* cr) {
    static double appear = 0.0;
    appear = Approach(appear, settings::Enabled(g_cfg->watermark) ? 1.0 : 0.0, 10.0);
    if (appear <= 0.0) return;

    int fps  = g_hud.overlay_fps.load();
    int ping = g_hud.local_ping.load();
    char sfps[16];  snprintf(sfps, sizeof(sfps), "%d FPS", fps);
    char sping[16]; if (ping >= 0) snprintf(sping, sizeof(sping), "%d MS", ping);
                    else           snprintf(sping, sizeof(sping), "0 MS");
    char stime[16];
    time_t t = time(nullptr);
    strftime(stime, sizeof(stime), "%H:%M", localtime(&t));
    static const std::string persona = SteamPersona();
    const char* user_env = getenv("USER");
    std::string user = !persona.empty() ? persona : user_env ? user_env : "user";

    struct Segment { HudIcon icon; const char* text; };
    Segment segments[] = {{HudIcon::Fps, sfps}, {HudIcon::Ping, sping}, {HudIcon::Clock, stime}, {HudIcon::User, user.c_str()}};

    double font = U(11), icon = U(10), gap = U(5), spacing = U(12), pad = U(10), h = U(26);
    double logo_w = HudTextWidth(cr, "SPAXER", U(12), true);
    double target_w = pad + logo_w + spacing;
    double widths[4];
    for (int i = 0; i < 4; i++) {
        widths[i] = HudTextWidth(cr, segments[i].text, font, true);
        target_w += icon + gap + widths[i] + (i < 3 ? spacing : 0);
    }
    target_w += pad;
    static double w = 0;
    w = w == 0 ? target_w : Approach(w, target_w, 16.0);

    int W = s_screen_w.load();
    if (g_cfg->hud_wm_x >= 0) {
        s_r_wm.x = g_cfg->hud_wm_x;
        s_r_wm.y = g_cfg->hud_wm_y;
    } else {
        s_r_wm.x = W - target_w - U(14);
        s_r_wm.y = U(10);
    }
    s_r_wm.w = w; s_r_wm.h = h;
    double x = std::round(s_r_wm.x), y = std::round(s_r_wm.y - (1.0 - EaseOut(appear)) * U(8));
    double cy = y + h / 2;

    HudPanel(cr, x, y, w, h, appear);
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
    double cx = x + pad;
    HudText(cr, cx, cy, "SPAXER", U(12), true, kAccentR, kAccentG, kAccentB, appear);
    cx = x + pad + logo_w + spacing;
    for (int i = 0; i < 4; i++) {
        DrawHudIcon(cr, segments[i].icon, cx + icon / 2, cy, icon, appear);
        cx += icon + gap;
        double r = 0.9, g = 0.91, b = 0.94;
        if (i == 1 && ping > 80) { r = 0.35; g = 0.55; b = 1.0; }
        else if (i == 1 && ping > 45) { r = 0.6; g = 0.75; b = 1.0; }
        HudText(cr, cx, cy, segments[i].text, font, true, r, g, b, appear);
        cx += widths[i] + spacing;
    }
    cairo_restore(cr);
}

static void DrawBomb(cairo_t* cr, int W) {
    bool planted = g_hud.bomb_visible.load();
    float blow = g_hud.bomb_blow_secs.load();
    int   site = g_hud.bomb_site.load();
    bool  bd   = g_hud.bomb_being_defused.load();
    float df   = g_hud.bomb_defuse_secs.load();
    bool edit = settings::Enabled(g_cfg->edit_mode);
    bool cursor = s_cursor_active.load();
    bool live = planted && blow > 0.f;

    static float s_sim_blow = 38.0f;
    static int s_sim_site = 0;
    static gint64 s_sim_last_us = 0;
    gint64 now_us = g_get_monotonic_time();
    if (s_sim_last_us == 0) s_sim_last_us = now_us;
    float dt = (now_us - s_sim_last_us) / 1000000.0f;
    s_sim_last_us = now_us;
    s_sim_blow -= dt;
    if (s_sim_blow <= 0.0f) {
        s_sim_blow = 35.0f + (rand() % 8);
        s_sim_site = rand() % 2;
    }

    if (!live && (edit || cursor)) {
        blow = s_sim_blow;
        site = s_sim_site;
        live = true;
    }

    static double appear = 0.0, defuse_appear = 0.0, shown_fraction = 1.0;
    appear = Approach(appear, settings::Enabled(g_cfg->bomb_timer) && (live || edit || cursor) ? 1.0 : 0.0, 10.0);
    defuse_appear = Approach(defuse_appear, live && bd && df > 0.f ? 1.0 : 0.0, 12.0);
    if (appear <= 0.0) return;

    char main_txt[64];
    if (live) snprintf(main_txt, sizeof(main_txt), "Site %s", site == 0 ? "A" : "B");
    else snprintf(main_txt, sizeof(main_txt), "Bomb timer");
    char time_txt[16] = {0};
    if (live) snprintf(time_txt, sizeof(time_txt), "%.1fs", blow);
    char sub_txt[48] = {0};
    if (df > 0.f) snprintf(sub_txt, sizeof(sub_txt), "Defusing  %.1fs", df);

    double font = U(12), row = U(28);
    double w = std::max(U(170), U(34) + HudTextWidth(cr, main_txt, font, true) + U(16) + HudTextWidth(cr, "00.0s", font, true) + U(12));
    double h = row + U(4) + defuse_appear * U(22);

    if (g_cfg->hud_bomb_x >= 0) {
        s_r_bomb.x = g_cfg->hud_bomb_x;
        s_r_bomb.y = g_cfg->hud_bomb_y;
    } else {
        s_r_bomb.x = (W - w) * 0.5;
        s_r_bomb.y = U(175);
    }
    s_r_bomb.w = w; s_r_bomb.h = h;
    double x = std::round(s_r_bomb.x), y = std::round(s_r_bomb.y - (1.0 - EaseOut(appear)) * U(8));

    double tr = 0.92, tg = 0.93, tb = 0.95;
    double br = kAccentR, bg = kAccentG, bb = kAccentB;
    if (live && blow < 5.f) { tr = br = 0.62; tg = bg = 0.8; tb = bb = 1.0; }
    else if (live && blow < 10.f) { tr = br = 0.45; tg = bg = 0.66; tb = bb = 1.0; }

    HudPanel(cr, x, y, w, h, appear);
    DrawHudIcon(cr, HudIcon::Bomb, x + U(16), y + row / 2, U(12), appear);
    HudText(cr, x + U(30), y + row / 2, main_txt, font, true, 0.92, 0.93, 0.95, appear);
    if (time_txt[0]) {
        double tw = HudTextWidth(cr, time_txt, font, true);
        HudText(cr, x + w - U(12) - tw, y + row / 2, time_txt, font, true, tr, tg, tb, appear);
    }

    shown_fraction = Approach(shown_fraction, live ? std::clamp(blow / 40.f, 0.f, 1.f) : 1.0, 8.0);
    double bar_x = x + U(10), bar_w = w - U(20), bar_y = y + row, bar_h = U(3);
    RoundedRect(cr, bar_x, bar_y, bar_w, bar_h, bar_h / 2);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08 * appear);
    cairo_fill(cr);
    if (shown_fraction > 0.01) {
        RoundedRect(cr, bar_x, bar_y, bar_w * shown_fraction, bar_h, bar_h / 2);
        cairo_set_source_rgba(cr, br, bg, bb, appear);
        cairo_fill(cr);
    }

    if (defuse_appear > 0.01 && sub_txt[0]) {
        double a = appear * defuse_appear;
        double sy = bar_y + bar_h + U(11);
        bool late = df >= blow;
        DrawHudIcon(cr, HudIcon::Defuse, x + U(16), sy, U(10), a);
        HudText(cr, x + U(30), sy, sub_txt, U(11), true, late ? 1.0 : 0.6, late ? 0.35 : 0.78, late ? 0.38 : 1.0, a);
    }
}

static const char* KeysymName(uint32_t k);

static std::string HudKeyName(uint32_t key) {
    gunichar ch = gdk_keyval_to_unicode(gdk_keyval_to_upper(key));
    if (ch > 32 && ch != 127) {
        char buf[8] = {0};
        g_unichar_to_utf8(ch, buf);
        return buf;
    }
    std::string name = KeysymName(key);
    static const std::pair<const char*, const char*> kShort[] = {
        {"space", "SPACE"}, {"Shift_L", "SHIFT"}, {"Shift_R", "SHIFT"}, {"Control_L", "CTRL"}, {"Control_R", "CTRL"},
        {"Alt_L", "ALT"}, {"Alt_R", "ALT"}, {"Caps_Lock", "CAPS"}, {"Tab", "TAB"}, {"Return", "ENTER"},
        {"Insert", "INS"}, {"Delete", "DEL"}, {"Home", "HOME"}, {"End", "END"}, {"Prior", "PGUP"}, {"Next", "PGDN"},
    };
    for (const auto& [from, to] : kShort) if (name == from) return to;
    std::transform(name.begin(), name.end(), name.begin(), ::toupper);
    return name;
}

struct HudListRow {
    std::string name;
    std::string value;
    HudIcon icon;
    bool accent_dot;
};

struct HudListEntry {
    HudListRow row;
    double t = 0.0;
};

struct HudListAnim {
    std::vector<HudListEntry> order;
    double appear = 0.0;
    double width = 0.0;
};

static double DrawHudList(cairo_t* cr, HudListAnim& anim, double x, double y, bool anchor_right, double min_w,
                          HudIcon header_icon, const char* title, const std::vector<HudListRow>& rows, bool force) {
    double font = U(12), row_h = U(26), gap = U(3);
    anim.appear = Approach(anim.appear, !rows.empty() || force ? 1.0 : 0.0, 12.0);
    for (const auto& row : rows) {
        bool known = false;
        for (auto& entry : anim.order) {
            if (entry.row.name != row.name) continue;
            entry.row = row;
            known = true;
        }
        if (!known) anim.order.push_back({row, 0.0});
    }
    for (auto& entry : anim.order) {
        bool present = false;
        for (const auto& row : rows) present |= row.name == entry.row.name;
        entry.t = Approach(entry.t, present ? 1.0 : 0.0, 14.0);
    }
    std::erase_if(anim.order, [](const HudListEntry& entry) { return entry.t <= 0.0; });
    if (anim.appear <= 0.0) return 0.0;

    double target_w = std::max(min_w, U(40) + HudTextWidth(cr, title, font, true));
    for (const auto& row : rows) {
        double value_w = row.value.empty() ? 0 : std::max(HudTextWidth(cr, row.value.c_str(), U(10.5), true) + U(12), U(22)) + U(16);
        target_w = std::max(target_w, U(28) + HudTextWidth(cr, row.name.c_str(), font, false) + value_w + U(14));
    }
    anim.width = anim.width == 0 ? target_w : Approach(anim.width, target_w, 14.0);
    double w = std::round(anim.width);
    if (anchor_right) x -= w;

    double header_alpha = anim.appear;
    double top = std::round(y - (1.0 - EaseOut(anim.appear)) * U(8));
    HudRowBackground(cr, x, top, w, row_h, header_alpha);
    cairo_rectangle(cr, x, top + row_h - 1, w * 0.55, 1);
    cairo_pattern_t* line = cairo_pattern_create_linear(x, 0, x + w * 0.55, 0);
    cairo_pattern_add_color_stop_rgba(line, 0.0, kAccentR, kAccentG, kAccentB, 0.9 * header_alpha);
    cairo_pattern_add_color_stop_rgba(line, 1.0, kAccentR, kAccentG, kAccentB, 0.0);
    cairo_set_source(cr, line);
    cairo_fill(cr);
    cairo_pattern_destroy(line);
    DrawHudIcon(cr, header_icon, x + U(14), top + row_h / 2, U(11), header_alpha);
    HudText(cr, x + U(28), top + row_h / 2, title, font, true, 0.95, 0.96, 0.98, header_alpha);

    double cur_y = top + row_h + gap;
    for (const auto& entry : anim.order) {
        const HudListRow& row = entry.row;
        double t = EaseOut(entry.t);
        double a = t * header_alpha;
        if (a > 0.01) {
            double ox = x - (1.0 - t) * U(14);
            double cy = cur_y + row_h / 2;
            HudRowBackground(cr, ox, cur_y, w, row_h, a * 0.9);
            if (row.accent_dot) {
                cairo_arc(cr, ox + U(14), cy, U(6.5), 0, 2 * 3.14159265358979);
                HudAccent(cr, 0.9 * a);
                cairo_fill(cr);
                char initial[2] = {static_cast<char>(toupper(static_cast<unsigned char>(row.name.empty() ? '?' : row.name[0]))), 0};
                double iw = HudTextWidth(cr, initial, U(9), true);
                HudText(cr, ox + U(14) - iw / 2, cy, initial, U(9), true, 1, 1, 1, a);
            } else {
                DrawHudIcon(cr, row.icon, ox + U(14), cy, U(11), a);
            }
            HudText(cr, ox + U(28), cy, row.name.c_str(), font, false, 0.9, 0.91, 0.94, a);
            if (!row.value.empty()) {
                double vw = HudTextWidth(cr, row.value.c_str(), U(10.5), true);
                double chip_w = std::max(vw + U(12), U(22)), chip_h = U(17);
                double chip_x = ox + w - U(6) - chip_w;
                RoundedRect(cr, chip_x, cy - chip_h / 2, chip_w, chip_h, U(4));
                HudAccent(cr, 0.16 * a);
                cairo_fill(cr);
                HudText(cr, chip_x + (chip_w - vw) / 2, cy, row.value.c_str(), U(10.5), true, 0.62, 0.75, 1.0, a);
            }
        }
        cur_y += (row_h + gap) * t;
    }
    return cur_y - top;
}

struct KeybindEntry {
    const char* name;
    uint32_t* bind;
    uint32_t* feature;
};

static std::vector<KeybindEntry> KeybindEntries() {
    return {
        {"Aimbot", &g_cfg->bind_aimbot, &g_cfg->aimbot_enabled},
        {"Triggerbot", &g_cfg->bind_trigger, &g_cfg->trigger_enabled},
        {"ESP", &g_cfg->bind_esp, &g_cfg->esp},
        {"Glow", &g_cfg->bind_glow, &g_cfg->glow},
        {"Chams", &g_cfg->bind_chams, &g_cfg->chams},
        {"Bunny hop", &g_cfg->bind_bunnyhop, &g_cfg->bunnyhop},
        {"Auto strafe", &g_cfg->bind_auto_strafe, &g_cfg->auto_strafe},
        {"Bomb timer", &g_cfg->bind_bomb, &g_cfg->bomb_timer},
        {"Watermark", &g_cfg->bind_watermark, &g_cfg->watermark},
        {"Keybinds", &g_cfg->bind_keybinds, &g_cfg->keybinds},
        {"Crosshair", &g_cfg->bind_crosshair, &g_cfg->crosshair},
        {"Thirdperson", &g_cfg->bind_thirdperson, &g_cfg->thirdperson},
        {"Arrows", &g_cfg->bind_arrows, &g_cfg->arrows},
        {"Night mode", &g_cfg->bind_night_mode, &g_cfg->night_mode},
        {"Fast stop", &g_cfg->bind_fast_stop, &g_cfg->fast_stop_enabled},
        {"Force shot", &g_cfg->bind_force_shot, &g_cfg->trigger_force_shot},
        {"Spread trigger", &g_cfg->bind_spread_trigger, &g_cfg->trigger_spread},
        {"Min damage", &g_cfg->bind_md_override, &g_cfg->trigger_md_override},
        {"Sound ESP", &g_cfg->bind_sound_esp, &g_cfg->sound_esp},
        {"Weapon ESP", &g_cfg->bind_weapon_esp, &g_cfg->esp_dropped_weapons},
        {"Edge bug", &g_cfg->bind_edge_bug, &g_cfg->edge_bug},
        {"Edge jump", &g_cfg->bind_edge_jump, &g_cfg->edge_jump},
        {"Silent aim", &g_cfg->bind_silent_aim, &g_cfg->silent_aim},
        {"Grenade aim", &g_cfg->bind_grenade_helper_aim, &g_cfg->grenade_helper_aim},
        {"Edit HUD", &g_cfg->bind_edit_hud, &g_cfg->edit_mode},
    };
}

static const char* FeatureName(const uint32_t* feature) {
    for (const KeybindEntry& entry : KeybindEntries())
        if (entry.feature == feature) return entry.name;
    return nullptr;
}

static void DrawKeybinds(cairo_t* cr) {
    struct BindItem {
        const char* name;
        const uint32_t* bind;
        bool active;
    };

    std::vector<BindItem> items;
    for (const KeybindEntry& entry : KeybindEntries()) items.push_back({entry.name, entry.bind, settings::Enabled(*entry.feature)});

    bool enabled = settings::Enabled(g_cfg->keybinds);
    bool edit = settings::Enabled(g_cfg->edit_mode);
    bool cursor = s_cursor_active.load();
    std::vector<HudListRow> rows;
    if (enabled) {
        for (const auto& item : items) {
            uint32_t key = *item.bind;
            if (key == 0 || !(item.active || edit)) continue;
            bool toggle = settings::GetBindMode(*g_cfg, item.bind) == settings::BindMode::Toggle;
            std::string value = item.bind == &g_cfg->bind_md_override && item.active ? std::to_string(g_cfg->md_override_value) : HudKeyName(key);
            rows.push_back({item.name, value, toggle ? HudIcon::Toggle : HudIcon::Hold, false});
        }
        if (rows.empty() && (edit || cursor)) {
            rows.push_back({"Thirdperson", "[HOLD]", HudIcon::Hold, false});
            rows.push_back({"Aimbot", "[HOLD]", HudIcon::Hold, false});
        }
    }

    if (g_cfg->hud_keybinds_x >= 0) {
        s_r_keybinds.x = g_cfg->hud_keybinds_x;
        s_r_keybinds.y = g_cfg->hud_keybinds_y;
    } else {
        s_r_keybinds.x = U(20);
        s_r_keybinds.y = s_screen_h.load() * 0.42;
    }
    static HudListAnim anim;
    double h = DrawHudList(cr, anim, s_r_keybinds.x, s_r_keybinds.y, false, U(170), HudIcon::List, "Hotkeys", rows, enabled && (edit || cursor));
    s_r_keybinds.w = anim.width;
    s_r_keybinds.h = std::max(h, U(26));
}

static void DrawSpectators(cairo_t* cr, int W) {
    std::vector<HudListRow> rows;
    bool enabled = settings::Enabled(g_cfg->spectators);
    bool edit = settings::Enabled(g_cfg->edit_mode);
    bool cursor = s_cursor_active.load();
    if (enabled) {
        std::lock_guard<std::mutex> lock(g_hud.spectators_mtx);
        for (const auto& s : g_hud.spectators) rows.push_back({s.name, "", HudIcon::Eye, true});
    }
    if (enabled && rows.empty() && (edit || cursor)) {
        rows.push_back({"Example player", "", HudIcon::Eye, true});
    }
    if (g_cfg->hud_spectators_x >= 0) {
        s_r_spectators.x = g_cfg->hud_spectators_x;
        s_r_spectators.y = g_cfg->hud_spectators_y;
    } else {
        s_r_spectators.x = W - U(174);
        s_r_spectators.y = U(46);
    }
    static HudListAnim anim;
    double h = DrawHudList(cr, anim, s_r_spectators.x, s_r_spectators.y, false, U(160), HudIcon::Eye, "Spectators", rows, enabled && (edit || cursor));
    s_r_spectators.w = anim.width;
    s_r_spectators.h = std::max(h, U(26));
}

static bool HitMediaPrev(double mx, double my) {
    double w = s_r_media.w;
    double bx = s_r_media.x + w / 2 - U(36);
    double by = s_r_media.y + U(22) + U(3) + U(36);
    return mx >= bx - U(15) && mx <= bx + U(15) && my >= by - U(12) && my <= by + U(12);
}

static bool HitMediaPlay(double mx, double my) {
    double w = s_r_media.w;
    double bx = s_r_media.x + w / 2;
    double by = s_r_media.y + U(22) + U(3) + U(36);
    return mx >= bx - U(15) && mx <= bx + U(15) && my >= by - U(12) && my <= by + U(12);
}

static bool HitMediaNext(double mx, double my) {
    double w = s_r_media.w;
    double bx = s_r_media.x + w / 2 + U(36);
    double by = s_r_media.y + U(22) + U(3) + U(36);
    return mx >= bx - U(15) && mx <= bx + U(15) && my >= by - U(12) && my <= by + U(12);
}

static void DrawMediaPlayer(cairo_t* cr, int W, int H) {
    bool enabled = settings::Enabled(g_cfg->media_player);
    bool edit = settings::Enabled(g_cfg->edit_mode);
    bool cursor = s_cursor_active.load();
    bool active = false;
    char title[128]{};
    char artist[128]{};
    char status[32]{};
    {
        std::lock_guard<std::mutex> lk(g_hud.media_mtx);
        active = g_hud.media_active;
        snprintf(title, sizeof(title), "%s", g_hud.media_title);
        snprintf(artist, sizeof(artist), "%s", g_hud.media_artist);
        snprintf(status, sizeof(status), "%s", g_hud.media_status);
    }
    static double s_media_appear = 0.0;
    bool should_show = enabled && (active || edit || cursor);
    s_media_appear = Approach(s_media_appear, should_show ? 1.0 : 0.0, 12.0);
    if (s_media_appear <= 0.01) return;

    if (g_cfg->hud_media_x >= 0) {
        s_r_media.x = g_cfg->hud_media_x;
        s_r_media.y = g_cfg->hud_media_y;
    } else {
        s_r_media.x = U(20);
        s_r_media.y = H * 0.70;
    }

    double alpha = EaseOut(s_media_appear);
    double w = U(220);
    double row_h = U(22);
    double gap = U(3);
    double body_h = U(48);
    double total_h = row_h + gap + body_h;
    s_r_media.w = w;
    s_r_media.h = total_h;

    double x = s_r_media.x;
    double y = s_r_media.y;

    HudRowBackground(cr, x, y, w, row_h, alpha);
    cairo_rectangle(cr, x, y + row_h - 1, w * 0.55, 1);
    cairo_pattern_t* line = cairo_pattern_create_linear(x, 0, x + w * 0.55, 0);
    cairo_pattern_add_color_stop_rgba(line, 0.0, kAccentR, kAccentG, kAccentB, 0.9 * alpha);
    cairo_pattern_add_color_stop_rgba(line, 1.0, kAccentR, kAccentG, kAccentB, 0.0);
    cairo_set_source(cr, line);
    cairo_fill(cr);
    cairo_pattern_destroy(line);

    DrawHudIcon(cr, HudIcon::Music, x + U(14), y + row_h / 2, U(11), alpha);
    HudText(cr, x + U(28), y + row_h / 2, "Media", U(11), true, 0.95, 0.96, 0.98, alpha);

    double body_y = y + row_h + gap;
    HudRowBackground(cr, x, body_y, w, body_h, alpha * 0.9);

    const char* display_title = (title[0] != '\0') ? title : ((edit || cursor) ? "Example Track" : "");
    const char* display_artist = (artist[0] != '\0') ? artist : ((edit || cursor) ? "playerctl" : "");

    double max_txt_w = w - U(24);
    double title_w = HudTextWidth(cr, display_title, U(10), true);
    cairo_save(cr);
    cairo_rectangle(cr, x + U(10), body_y, max_txt_w + U(4), body_h);
    cairo_clip(cr);
    if (title_w <= max_txt_w) {
        HudText(cr, x + U(12), body_y + U(11), display_title, U(10), true, 0.92, 0.93, 0.95, alpha);
    } else {
        double period = title_w + U(40);
        double offset = std::fmod(g_get_monotonic_time() / 1e6 * U(28), period);
        HudText(cr, x + U(12) - offset, body_y + U(11), display_title, U(10), true, 0.92, 0.93, 0.95, alpha);
        HudText(cr, x + U(12) - offset + period, body_y + U(11), display_title, U(10), true, 0.92, 0.93, 0.95, alpha);
    }
    cairo_restore(cr);
    if (display_artist[0] != '\0') {
        PangoLayout* layout = HudLayout(cr, display_artist, U(8.5), false);
        pango_layout_set_width(layout, static_cast<int>(max_txt_w * PANGO_SCALE));
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        int lw, lh;
        pango_layout_get_pixel_size(layout, &lw, &lh);
        cairo_set_source_rgba(cr, 0.60, 0.65, 0.75, alpha * 0.85);
        cairo_move_to(cr, std::round(x + U(12)), std::round(body_y + U(23) - lh / 2.0));
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }

    double ctrl_y = body_y + U(36);
    double btn_w = U(24), btn_h = U(15);
    double b_prev_x = x + w / 2 - U(36);
    double b_play_x = x + w / 2;
    double b_next_x = x + w / 2 + U(36);

    bool is_playing = (strcasecmp(status, "Playing") == 0);

    RoundedRect(cr, b_prev_x - btn_w / 2, ctrl_y - btn_h / 2, btn_w, btn_h, U(3.5));
    HudAccent(cr, 0.16 * alpha);
    cairo_fill(cr);
    DrawHudIcon(cr, HudIcon::Prev, b_prev_x, ctrl_y, U(8.5), alpha);

    RoundedRect(cr, b_play_x - btn_w / 2, ctrl_y - btn_h / 2, btn_w, btn_h, U(3.5));
    HudAccent(cr, (is_playing ? 0.32 : 0.20) * alpha);
    cairo_fill(cr);
    DrawHudIcon(cr, is_playing ? HudIcon::Pause : HudIcon::Play, b_play_x, ctrl_y, U(8.5), alpha);

    RoundedRect(cr, b_next_x - btn_w / 2, ctrl_y - btn_h / 2, btn_w, btn_h, U(3.5));
    HudAccent(cr, 0.16 * alpha);
    cairo_fill(cr);
    DrawHudIcon(cr, HudIcon::Next, b_next_x, ctrl_y, U(8.5), alpha);
}

static void DrawVelocityGraph(cairo_t* cr, int W, int H) {
    struct Sample { double t; float speed; };
    struct Takeoff { double t; float speed; };
    static std::vector<Sample> samples;
    static std::vector<Takeoff> takeoffs;
    static bool was_grounded = true;
    static float last_takeoff = 0.f;
    static double appear = 0.0, scale_max = 300.0, trend = 0.0;
    constexpr double kWindow = 3.0;

    bool enabled = settings::Enabled(g_cfg->velocity_graph);
    bool edit = settings::Enabled(g_cfg->edit_mode);
    bool cursor = s_cursor_active.load();
    uintptr_t pawn = enabled && g_hud.in_game.load() ? game::LocalPawn() : 0;
    double now = g_get_monotonic_time() / 1e6;

    float speed = 0.f;
    if (pawn && off::m_vecVelocity) {
        float vx = g_proc.Read<float>(pawn + off::m_vecVelocity);
        float vy = g_proc.Read<float>(pawn + off::m_vecVelocity + 4);
        speed = std::sqrt(vx * vx + vy * vy);
        if (!std::isfinite(speed) || speed > 5000.f) speed = 0.f;
        bool grounded = off::m_fFlags && (g_proc.Read<uint32_t>(pawn + off::m_fFlags) & 1u);
        if (was_grounded && !grounded && speed > 1.f) {
            last_takeoff = speed;
            takeoffs.push_back({now, speed});
        }
        was_grounded = grounded;
        samples.push_back({now, speed});
    } else if (edit || cursor) {
        speed = 250.0f;
        if (samples.empty()) {
            for (int i = 0; i < 30; i++) samples.push_back({now - (30 - i) * 0.1, 240.f + (i % 5) * 5.f});
        }
    }
    std::erase_if(samples, [&](const Sample& s) { return now - s.t > kWindow + 0.2; });
    std::erase_if(takeoffs, [&](const Takeoff& s) { return now - s.t > kWindow; });

    appear = Approach(appear, enabled && (pawn || edit || cursor) ? 1.0 : 0.0, 10.0);
    if (appear <= 0.0) {
        samples.clear();
        takeoffs.clear();
        return;
    }

    double w = U(260), graph_h = U(56), h = graph_h + U(40);
    if (g_cfg->hud_velocity_x >= 0) {
        s_r_velocity.x = g_cfg->hud_velocity_x;
        s_r_velocity.y = g_cfg->hud_velocity_y;
    } else {
        s_r_velocity.x = (W - w) * 0.5;
        s_r_velocity.y = H * 0.74;
    }
    s_r_velocity.w = w; s_r_velocity.h = h;
    double x = std::round(s_r_velocity.x), y = std::round(s_r_velocity.y + (1.0 - EaseOut(appear)) * U(8));

    float peak = 0.f;
    for (const auto& s : samples) peak = std::max(peak, s.speed);
    scale_max = Approach(scale_max, std::max(300.0, peak * 1.15), 4.0);

    if (samples.size() >= 2) {
        double before = samples[samples.size() - 2].speed;
        double delta = std::clamp((speed - before) / std::max(s_hud_dt, 0.001) / 300.0, -1.0, 1.0);
        trend = Approach(trend, delta, 6.0);
    }

    auto px = [&](double t) { return x + w - (now - t) / kWindow * w; };
    auto py = [&](double v) { return y + graph_h - std::min(v / scale_max, 1.0) * (graph_h - U(4)); };

    cairo_save(cr);
    cairo_rectangle(cr, x, y - U(4), w, graph_h + U(8));
    cairo_clip(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.06 * appear);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, x, std::round(y + graph_h) + 0.5);
    cairo_line_to(cr, x + w, std::round(y + graph_h) + 0.5);
    cairo_stroke(cr);

    if (samples.size() >= 2) {
        cairo_new_path(cr);
        cairo_move_to(cr, px(samples.front().t), py(samples.front().speed));
        for (size_t i = 1; i < samples.size(); i++) {
            double mx = (px(samples[i - 1].t) + px(samples[i].t)) / 2;
            double my = (py(samples[i - 1].speed) + py(samples[i].speed)) / 2;
            cairo_curve_to(cr, px(samples[i - 1].t), py(samples[i - 1].speed), px(samples[i - 1].t), py(samples[i - 1].speed), mx, my);
        }
        cairo_line_to(cr, px(samples.back().t), py(samples.back().speed));
        cairo_path_t* line = cairo_copy_path(cr);

        cairo_line_to(cr, px(samples.back().t), y + graph_h);
        cairo_line_to(cr, px(samples.front().t), y + graph_h);
        cairo_close_path(cr);
        cairo_pattern_t* fill = cairo_pattern_create_linear(0, y, 0, y + graph_h);
        cairo_pattern_add_color_stop_rgba(fill, 0.0, kAccentR, kAccentG, kAccentB, 0.28 * appear);
        cairo_pattern_add_color_stop_rgba(fill, 1.0, kAccentR, kAccentG, kAccentB, 0.0);
        cairo_set_source(cr, fill);
        cairo_fill(cr);
        cairo_pattern_destroy(fill);

        cairo_new_path(cr);
        cairo_append_path(cr, line);
        cairo_path_destroy(line);
        cairo_pattern_t* stroke = cairo_pattern_create_linear(x, 0, x + w, 0);
        cairo_pattern_add_color_stop_rgba(stroke, 0.0, kAccentR, kAccentG, kAccentB, 0.0);
        cairo_pattern_add_color_stop_rgba(stroke, 0.25, kAccentR, kAccentG, kAccentB, 0.9 * appear);
        cairo_pattern_add_color_stop_rgba(stroke, 1.0, 0.75, 0.85, 1.0, appear);
        cairo_set_source(cr, stroke);
        cairo_set_line_width(cr, U(2));
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_stroke(cr);
        cairo_pattern_destroy(stroke);
    }

    for (const auto& jump : takeoffs) {
        double jx = px(jump.t), jy = py(jump.speed);
        double fade = std::clamp((jx - x) / (w * 0.25), 0.0, 1.0) * appear;
        cairo_arc(cr, jx, jy, U(2.5), 0, 2 * 3.14159265358979);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.9 * fade);
        cairo_fill(cr);
        char label[16];
        snprintf(label, sizeof(label), "%d", static_cast<int>(std::lround(jump.speed)));
        double lw = HudTextWidth(cr, label, U(9), true);
        HudText(cr, jx - lw / 2, jy - U(9), label, U(9), true, 0.8, 0.83, 0.9, 0.8 * fade);
    }
    cairo_restore(cr);

    static double shown_speed = 0.0;
    shown_speed = Approach(shown_speed, speed, 20.0);
    char value[16];
    snprintf(value, sizeof(value), "%d", static_cast<int>(std::lround(shown_speed)));
    double r = 0.95, g = 0.96, b = 0.98;
    if (trend > 0.05) { double k = std::min(trend * 2, 1.0); r -= 0.4 * k; g -= 0.15 * k; b = std::min(1.0, b + 0.02 * k); }
    else if (trend < -0.05) { double k = std::min(-trend * 2, 1.0); r -= 0.7 * k; g -= 0.55 * k; b -= 0.05 * k; }
    double big = U(22);
    double vw = HudTextWidth(cr, value, big, true);
    char sub[24] = {0};
    if (last_takeoff > 0.f) snprintf(sub, sizeof(sub), "(%d)", static_cast<int>(std::lround(last_takeoff)));
    double sw = sub[0] ? HudTextWidth(cr, sub, U(12), true) + U(6) : 0;
    double tx = x + (w - vw - sw) / 2, ty = y + graph_h + U(20);
    HudText(cr, tx, ty, value, big, true, r, g, b, appear);
    if (sub[0]) HudText(cr, tx + vw + U(6), ty + U(2), sub, U(12), true, 0.55, 0.58, 0.66, appear);
}

static void DrawKeystrokes(cairo_t* cr, int W, int H) {
    struct Key { const char* label; int code; bool mouse; };
    static const Key kKeys[] = {
        {"W", KEY_W, false}, {"A", KEY_A, false}, {"S", KEY_S, false}, {"D", KEY_D, false},
        {"SPACE", KEY_SPACE, false}, {"LMB", 1, true}, {"RMB", 3, true},
    };
    static double press[7] = {};
    static bool was_down[7] = {};
    static std::vector<std::chrono::steady_clock::time_point> clicks[2];
    static double appear = 0.0;
    bool edit = settings::Enabled(g_cfg->edit_mode);
    appear = Approach(appear, settings::Enabled(g_cfg->keystrokes) && (g_hud.in_game.load() || edit) ? 1.0 : 0.0, 10.0);
    if (appear <= 0.0) return;

    double key = U(48), gap = U(6), bar = U(34);
    double w = key * 3 + gap * 2, h = key * 2 + bar * 2 + gap * 3;
    if (g_cfg->hud_keys_x >= 0) {
        s_r_keys.x = g_cfg->hud_keys_x;
        s_r_keys.y = g_cfg->hud_keys_y;
    } else {
        s_r_keys.x = U(20);
        s_r_keys.y = H * 0.64;
    }
    s_r_keys.w = w; s_r_keys.h = h;
    double x0 = std::round(s_r_keys.x), y0 = std::round(s_r_keys.y + (1.0 - EaseOut(appear)) * U(8));
    double half = (w - gap) / 2;
    struct Slot { double x, y, w, h; };
    const Slot slots[] = {
        {x0 + key + gap, y0, key, key},
        {x0, y0 + key + gap, key, key},
        {x0 + key + gap, y0 + key + gap, key, key},
        {x0 + (key + gap) * 2, y0 + key + gap, key, key},
        {x0, y0 + (key + gap) * 2, w, bar},
        {x0, y0 + (key + gap) * 2 + bar + gap, half, bar},
        {x0 + half + gap, y0 + (key + gap) * 2 + bar + gap, half, bar},
    };
    auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < std::size(kKeys); i++) {
        const Key& k = kKeys[i];
        const Slot& slot = slots[i];
        bool down = k.mouse ? g_input.IsMouseDown(k.code) : g_input.IsPhysicalKeyDown(k.code);
        if (k.mouse) {
            auto& list = clicks[k.code == 1 ? 0 : 1];
            if (down && !was_down[i]) list.push_back(now);
            std::erase_if(list, [&](const auto& t) { return now - t > std::chrono::seconds(1); });
        }
        was_down[i] = down;
        press[i] = Approach(press[i], down ? 1.0 : 0.0, down ? 35.0 : 10.0);
        double t = press[i];
        double inset = t * U(1.5);
        double kx = slot.x + inset, ky = slot.y + inset, kw = slot.w - inset * 2, kh = slot.h - inset * 2;
        HudGlass(cr, kx, ky, kw, kh, U(8), appear);
        if (t > 0.01) {
            RoundedRect(cr, kx, ky, kw, kh, U(8));
            cairo_pattern_t* fill = cairo_pattern_create_linear(0, ky, 0, ky + kh);
            cairo_pattern_add_color_stop_rgba(fill, 0.0, kAccentR * 1.1, kAccentG * 1.1, std::min(1.0, kAccentB * 1.1), 0.95 * t * appear);
            cairo_pattern_add_color_stop_rgba(fill, 1.0, kAccentR * 0.75, kAccentG * 0.75, kAccentB * 0.85, 0.9 * t * appear);
            cairo_set_source(cr, fill);
            cairo_fill(cr);
            cairo_pattern_destroy(fill);
            for (int g = 3; g >= 1; g--) {
                RoundedRect(cr, kx - g * 2, ky - g * 2, kw + g * 4, kh + g * 4, U(8) + g * 2);
                cairo_set_source_rgba(cr, kAccentR, kAccentG, kAccentB, 0.06 * t * appear);
                cairo_fill(cr);
            }
        }
        double shade = 0.7 + 0.3 * t;
        double font = slot.h < key ? U(12) : U(17);
        if (k.mouse) {
            char cps[32];
            snprintf(cps, sizeof(cps), "%zu CPS", clicks[k.code == 1 ? 0 : 1].size());
            double lw = HudTextWidth(cr, k.label, U(11), true), cw = HudTextWidth(cr, cps, U(10), false);
            double total = lw + U(6) + cw;
            HudText(cr, kx + (kw - total) / 2, ky + kh / 2, k.label, U(11), true, shade, shade, 1.0, appear);
            HudText(cr, kx + (kw - total) / 2 + lw + U(6), ky + kh / 2, cps, U(10), false, 0.75, 0.8, 0.95, appear * (0.6 + 0.4 * t));
        } else {
            double tw = HudTextWidth(cr, k.label, font, true);
            HudText(cr, kx + (kw - tw) / 2, ky + kh / 2, k.label, font, true, shade, shade, 1.0, appear);
        }
    }
}

static void DrawNoticeIcon(cairo_t* cr, NoticeKind kind, double cx, double cy, double r, double a) {
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, a);
    cairo_set_line_width(cr, std::max(1.4, r * 0.28));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    const double pi = 3.14159265358979;
    switch (kind) {
        case NoticeKind::On:
            cairo_move_to(cr, cx - r * 0.5, cy);
            cairo_line_to(cr, cx - r * 0.1, cy + r * 0.4);
            cairo_line_to(cr, cx + r * 0.55, cy - r * 0.4);
            cairo_stroke(cr);
            break;
        case NoticeKind::Off:
            cairo_move_to(cr, cx - r * 0.4, cy - r * 0.4); cairo_line_to(cr, cx + r * 0.4, cy + r * 0.4);
            cairo_move_to(cr, cx + r * 0.4, cy - r * 0.4); cairo_line_to(cr, cx - r * 0.4, cy + r * 0.4);
            cairo_stroke(cr);
            break;
        case NoticeKind::Miss:
            cairo_move_to(cr, cx - r * 0.45, cy - r * 0.45); cairo_line_to(cr, cx + r * 0.45, cy + r * 0.45);
            cairo_move_to(cr, cx + r * 0.45, cy - r * 0.45); cairo_line_to(cr, cx - r * 0.45, cy + r * 0.45);
            cairo_stroke(cr);
            break;
        case NoticeKind::Hit:
            cairo_arc(cr, cx, cy, r * 0.35, 0, 2 * pi);
            cairo_stroke(cr);
            cairo_move_to(cr, cx - r * 0.6, cy); cairo_line_to(cr, cx + r * 0.6, cy);
            cairo_move_to(cr, cx, cy - r * 0.6); cairo_line_to(cr, cx, cy + r * 0.6);
            cairo_stroke(cr);
            break;
        case NoticeKind::Kill:
            cairo_arc(cr, cx, cy - r * 0.12, r * 0.52, pi, 2 * pi);
            cairo_line_to(cr, cx + r * 0.52, cy + r * 0.2);
            cairo_line_to(cr, cx + r * 0.3, cy + r * 0.2);
            cairo_line_to(cr, cx + r * 0.3, cy + r * 0.55);
            cairo_line_to(cr, cx - r * 0.3, cy + r * 0.55);
            cairo_line_to(cr, cx - r * 0.3, cy + r * 0.2);
            cairo_line_to(cr, cx - r * 0.52, cy + r * 0.2);
            cairo_close_path(cr);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 0.15, 0.3, 0.8, a);
            cairo_arc(cr, cx - r * 0.2, cy - r * 0.1, r * 0.13, 0, 2 * pi);
            cairo_arc(cr, cx + r * 0.2, cy - r * 0.1, r * 0.13, 0, 2 * pi);
            cairo_fill(cr);
            break;
        case NoticeKind::Bomb:
            cairo_arc(cr, cx - r * 0.08, cy + r * 0.1, r * 0.42, 0, 2 * pi);
            cairo_fill(cr);
            cairo_move_to(cr, cx + r * 0.2, cy - r * 0.25);
            cairo_line_to(cr, cx + r * 0.5, cy - r * 0.55);
            cairo_stroke(cr);
            break;
        default:
            cairo_arc(cr, cx, cy, r * 0.45, 0, 2 * pi);
            cairo_stroke(cr);
            cairo_move_to(cr, cx, cy - r * 0.75); cairo_line_to(cr, cx, cy - r * 0.2);
            cairo_move_to(cr, cx, cy + r * 0.2); cairo_line_to(cr, cx, cy + r * 0.75);
            cairo_move_to(cr, cx - r * 0.75, cy); cairo_line_to(cr, cx - r * 0.2, cy);
            cairo_move_to(cr, cx + r * 0.2, cy); cairo_line_to(cr, cx + r * 0.75, cy);
            cairo_stroke(cr);
            break;
    }
    cairo_restore(cr);
}

static void DrawNotices(cairo_t* cr, int W, int H) {
    constexpr double kLife = 4.5;
    std::vector<Notice> notices;
    {
        std::lock_guard<std::mutex> lock(g_hud.notices_mtx);
        auto now = std::chrono::steady_clock::now();
        std::erase_if(g_hud.notices, [&](const Notice& n) { return std::chrono::duration<double>(now - n.time).count() > kLife; });
        notices = g_hud.notices;
    }
    bool enabled = settings::Enabled(g_cfg->notifications);
    bool edit = settings::Enabled(g_cfg->edit_mode) || s_cursor_active.load();
    if (enabled && edit && notices.empty()) notices.push_back({"Notifications", NoticeKind::Info, std::chrono::steady_clock::now()});
    double font = U(13), row_h = U(36), width = U(320);
    if (g_cfg->hud_notif_x >= 0) {
        s_r_notices.x = g_cfg->hud_notif_x;
        s_r_notices.y = g_cfg->hud_notif_y;
    } else {
        s_r_notices.x = (W - width) / 2;
        s_r_notices.y = H * 0.2;
    }
    s_r_notices.w = width;
    s_r_notices.h = row_h;
    if (!enabled || notices.empty()) return;
    auto now = std::chrono::steady_clock::now();
    double y = s_r_notices.y;
    double center = s_r_notices.x + width / 2;
    for (auto it = notices.rbegin(); it != notices.rend(); ++it) {
        double age = std::chrono::duration<double>(now - it->time).count();
        double in = edit && it->text == "Notifications" ? 1.0 : EaseOut(age / 0.28);
        double out = edit && it->text == "Notifications" ? 1.0 : std::clamp((kLife - age) / 0.4, 0.0, 1.0);
        double a = std::min(in, out);
        double r = kAccentR, g = kAccentG, b = kAccentB;
        switch (it->kind) {
            case NoticeKind::Kill: r = 0.95; g = 0.25; b = 0.25; break;
            case NoticeKind::Hit: r = kAccentR; g = kAccentG; b = kAccentB; break;
            case NoticeKind::Miss: r = 0.95; g = 0.55; b = 0.2; break;
            case NoticeKind::On: r = 0.38; g = 0.85; b = 0.45; break;
            case NoticeKind::Off: r = 0.55; g = 0.6; b = 0.72; break;
            case NoticeKind::Bomb: r = 1.0; g = 0.45; b = 0.2; break;
            default: break;
        }
        double tw = HudTextWidth(cr, it->text.c_str(), font, true);
        double w = std::max(U(180), row_h + tw + U(18));
        double x = std::round(center - w / 2);
        double ty = std::round(y - (1.0 - in) * U(12));
        HudGlass(cr, x, ty, w, row_h, U(9), a);
        RoundedRect(cr, x, ty, w, row_h, U(9));
        cairo_pattern_t* tint = cairo_pattern_create_linear(x, 0, x + w, 0);
        cairo_pattern_add_color_stop_rgba(tint, 0.0, r, g, b, 0.38 * a);
        cairo_pattern_add_color_stop_rgba(tint, 0.45, r, g, b, 0.12 * a);
        cairo_pattern_add_color_stop_rgba(tint, 1.0, r, g, b, 0.04 * a);
        cairo_set_source(cr, tint);
        cairo_fill(cr);
        cairo_pattern_destroy(tint);
        double icon_r = row_h * 0.32;
        double icx = x + row_h / 2 + U(1), icy = ty + row_h / 2;
        cairo_arc(cr, icx, icy, icon_r, 0, 2 * 3.14159265358979);
        cairo_set_source_rgba(cr, r, g, b, 0.95 * a);
        cairo_fill(cr);
        DrawNoticeIcon(cr, it->kind, icx, icy, icon_r * 0.85, a);
        HudText(cr, x + row_h + U(6), icy, it->text.c_str(), font, true, 1, 1, 1, a);
        double life = edit && it->text == "Notifications" ? 1.0 : std::clamp(1.0 - age / kLife, 0.0, 1.0);
        RoundedRect(cr, x + U(10), ty + row_h - U(3), (w - U(20)) * life, U(2), U(1));
        cairo_set_source_rgba(cr, r, g, b, 0.9 * a);
        cairo_fill(cr);
        y += (row_h + U(6)) * in;
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
        DrawText(cr, sx + 9, sy - 18 - age * 0.02, buf, 0.55, 0.75, 1.0, alpha, 13, true);
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
    cairo_select_font_face(cr, "Roboto", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
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
    cairo_select_font_face(cr, "Roboto", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
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
            cairo_set_source_rgba(cr, 0.3, 0.55, 1.0, 1.0);
            cairo_arc(cr, px, py, 3.5, 0, 2 * M_PI); cairo_fill(cr);
        }
    }

    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    cairo_move_to(cr, cx, cy - 6); cairo_line_to(cr, cx + 4.5, cy + 5); cairo_line_to(cr, cx - 4.5, cy + 5);
    cairo_close_path(cr); cairo_fill(cr);
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
        UpdateInputShape();
        return FALSE;
    }

    s_camera = g_hud.in_game.load() ? render::ReadCamera(a.width, a.height, std::clamp(g_cfg->render_lead_ms, 0, 200) / 1000.f) : render::Camera{};
    std::vector<world::LivePlayer> players;
    if (s_camera.valid) players = world::CapturePlayers(*g_cfg);
    nightsky::Draw(cr, s_camera, *g_cfg);
    weather::Draw(cr, s_camera, *g_cfg);
    world::DrawChams(cr, s_camera, players, *g_cfg);
    world::DrawEsp(cr, s_camera, players, *g_cfg);
    DrawDroppedItems(cr, a.width, a.height);
    DrawRadar(cr);
    grenade::Draw(cr, s_camera, *g_cfg);
    world::DrawArrows(cr, s_camera, players, *g_cfg);
    DrawSoundEsp(cr, s_camera);
    HudBeginFrame(a.height);
    UpdateAccentFromTheme();
    DrawWatermark(cr);
    DrawKeybinds(cr);
    DrawBomb(cr, a.width);
    DrawCrosshair(cr, a.width, a.height);
    DrawHitmarker(cr, a.width, a.height);
    DrawSpectators(cr, a.width);
    DrawVelocityGraph(cr, a.width, a.height);
    DrawKeystrokes(cr, a.width, a.height);
    DrawNotices(cr, a.width, a.height);
    DrawMediaPlayer(cr, a.width, a.height);
    features::PaintLua(cr, s_camera);

    if (settings::Enabled(g_cfg->edit_mode)) {
        DrawEditMarker(cr, s_r_wm);
        DrawEditMarker(cr, s_r_bomb);
        DrawEditMarker(cr, s_r_keybinds);
        DrawEditMarker(cr, s_r_radar);
        if (settings::Enabled(g_cfg->spectators)) DrawEditMarker(cr, s_r_spectators);
        if (settings::Enabled(g_cfg->media_player)) DrawEditMarker(cr, s_r_media);
        if (settings::Enabled(g_cfg->velocity_graph)) DrawEditMarker(cr, s_r_velocity);
        if (settings::Enabled(g_cfg->keystrokes)) DrawEditMarker(cr, s_r_keys);
        if (settings::Enabled(g_cfg->notifications)) DrawEditMarker(cr, s_r_notices);
        DrawText(cr, 20, a.height - 34,
                 "Edit mode — drag HUD elements. Press F8 to finish.",
                 0.9, 0.9, 0.9, 1.0, 14, true);
    }

    UpdateInputShape();

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
    features::TickLua();
    return G_SOURCE_CONTINUE;
}

static gboolean OnFrameClock(GtkWidget* widget, GdkFrameClock*, gpointer) {
    features::FrameLua();
    gtk_widget_queue_draw(widget);
    return G_SOURCE_CONTINUE;
}

static bool HitRect(const Rect& r, double x, double y) {
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

static gboolean OnButton(GtkWidget*, GdkEventButton* e, gpointer) {
    if (e->type == GDK_BUTTON_PRESS && e->button == 1) {
        if (settings::Enabled(g_cfg->media_player) && HitRect(s_r_media, e->x, e->y)) {
            char player_opt[96]{};
            {
                std::lock_guard<std::mutex> lk(g_hud.media_mtx);
                if (g_hud.media_player_name[0] != '\0')
                    snprintf(player_opt, sizeof(player_opt), "-p %s ", g_hud.media_player_name);
            }
            char cmd[256]{};
            if (HitMediaPrev(e->x, e->y)) {
                snprintf(cmd, sizeof(cmd), "playerctl %sprevious >/dev/null 2>&1 &", player_opt);
                system(cmd);
                return TRUE;
            } else if (HitMediaPlay(e->x, e->y)) {
                snprintf(cmd, sizeof(cmd), "playerctl %splay-pause >/dev/null 2>&1 &", player_opt);
                system(cmd);
                return TRUE;
            } else if (HitMediaNext(e->x, e->y)) {
                snprintf(cmd, sizeof(cmd), "playerctl %snext >/dev/null 2>&1 &", player_opt);
                system(cmd);
                return TRUE;
            } else {
                s_drag_target = 6;
                s_drag_off_x = e->x - s_r_media.x;
                s_drag_off_y = e->y - s_r_media.y;
                return TRUE;
            }
        } else if (settings::Enabled(g_cfg->watermark) && HitRect(s_r_wm, e->x, e->y)) {
            s_drag_target = 0;
            s_drag_off_x = e->x - s_r_wm.x;
            s_drag_off_y = e->y - s_r_wm.y;
            return TRUE;
        } else if (settings::Enabled(g_cfg->bomb_timer) && HitRect(s_r_bomb, e->x, e->y)) {
            s_drag_target = 1;
            s_drag_off_x = e->x - s_r_bomb.x;
            s_drag_off_y = e->y - s_r_bomb.y;
            return TRUE;
        } else if (settings::Enabled(g_cfg->keybinds) && HitRect(s_r_keybinds, e->x, e->y)) {
            s_drag_target = 2;
            s_drag_off_x = e->x - s_r_keybinds.x;
            s_drag_off_y = e->y - s_r_keybinds.y;
            return TRUE;
        } else if (settings::Enabled(g_cfg->radar) && HitRect(s_r_radar, e->x, e->y)) {
            s_drag_target = 3;
            s_drag_off_x = e->x - s_r_radar.x;
            s_drag_off_y = e->y - s_r_radar.y;
            return TRUE;
        } else if (settings::Enabled(g_cfg->velocity_graph) && HitRect(s_r_velocity, e->x, e->y)) {
            s_drag_target = 4;
            s_drag_off_x = e->x - s_r_velocity.x;
            s_drag_off_y = e->y - s_r_velocity.y;
            return TRUE;
        } else if (settings::Enabled(g_cfg->notifications) && HitRect(s_r_notices, e->x, e->y)) {
            s_drag_target = 8;
            s_drag_off_x = e->x - s_r_notices.x;
            s_drag_off_y = e->y - s_r_notices.y;
            return TRUE;
        } else if (settings::Enabled(g_cfg->keystrokes) && HitRect(s_r_keys, e->x, e->y)) {
            s_drag_target = 7;
            s_drag_off_x = e->x - s_r_keys.x;
            s_drag_off_y = e->y - s_r_keys.y;
            return TRUE;
        } else if (settings::Enabled(g_cfg->spectators) && HitRect(s_r_spectators, e->x, e->y)) {
            s_drag_target = 5;
            s_drag_off_x = e->x - s_r_spectators.x;
            s_drag_off_y = e->y - s_r_spectators.y;
            return TRUE;
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
        } else if (s_drag_target == 4) {
            g_cfg->hud_velocity_x = (int)s_r_velocity.x;
            g_cfg->hud_velocity_y = (int)s_r_velocity.y;
        } else if (s_drag_target == 5) {
            g_cfg->hud_spectators_x = (int)s_r_spectators.x;
            g_cfg->hud_spectators_y = (int)s_r_spectators.y;
        } else if (s_drag_target == 6) {
            g_cfg->hud_media_x = (int)s_r_media.x;
            g_cfg->hud_media_y = (int)s_r_media.y;
        } else if (s_drag_target == 7) {
            g_cfg->hud_keys_x = (int)s_r_keys.x;
            g_cfg->hud_keys_y = (int)s_r_keys.y;
        } else if (s_drag_target == 8) {
            g_cfg->hud_notif_x = (int)s_r_notices.x;
            g_cfg->hud_notif_y = (int)s_r_notices.y;
        }
        s_drag_target = -1;
        UpdateInputShape();
    }
    return TRUE;
}

static gboolean OnMotion(GtkWidget*, GdkEventMotion* e, gpointer) {
    if (s_drag_target < 0) return FALSE;
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
    } else if (s_drag_target == 4) {
        s_r_velocity.x = e->x - s_drag_off_x;
        s_r_velocity.y = e->y - s_drag_off_y;
        g_cfg->hud_velocity_x = (int)s_r_velocity.x;
        g_cfg->hud_velocity_y = (int)s_r_velocity.y;
    } else if (s_drag_target == 5) {
        s_r_spectators.x = e->x - s_drag_off_x;
        s_r_spectators.y = e->y - s_drag_off_y;
        g_cfg->hud_spectators_x = (int)s_r_spectators.x;
        g_cfg->hud_spectators_y = (int)s_r_spectators.y;
    } else if (s_drag_target == 6) {
        s_r_media.x = e->x - s_drag_off_x;
        s_r_media.y = e->y - s_drag_off_y;
        g_cfg->hud_media_x = (int)s_r_media.x;
        g_cfg->hud_media_y = (int)s_r_media.y;
    } else if (s_drag_target == 7) {
        s_r_keys.x = e->x - s_drag_off_x;
        s_r_keys.y = e->y - s_drag_off_y;
        g_cfg->hud_keys_x = (int)s_r_keys.x;
        g_cfg->hud_keys_y = (int)s_r_keys.y;
    } else if (s_drag_target == 8) {
        s_r_notices.x = e->x - s_drag_off_x;
        s_r_notices.y = e->y - s_drag_off_y;
        g_cfg->hud_notif_x = (int)s_r_notices.x;
        g_cfg->hud_notif_y = (int)s_r_notices.y;
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
    gtk_layer_set_namespace(GTK_WINDOW(win), "spaxer");
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

static void AutosaveConfig() {
    static std::string s_name;
    static std::vector<char> s_snapshot;
    std::string name = settings::LastConfig();
    const char* bytes = reinterpret_cast<const char*>(g_cfg);
    std::vector<char> current(bytes, bytes + sizeof(Settings));
    Settings* view = reinterpret_cast<Settings*>(current.data());
    view->edit_mode = 0;
    view->gui_open = 0;
    view->gui_x = view->gui_y = view->gui_w = view->gui_h = 0;
    if (name != s_name) {
        s_name = name;
        s_snapshot.swap(current);
        return;
    }
    static time_t s_lua_values_mtime = 0;
    struct stat values_stat;
    const char* home = getenv("HOME");
    std::string values_path = std::string(home ? home : "/tmp") + "/.config/spaxer/lua_values.txt";
    time_t lua_values_mtime = ::stat(values_path.c_str(), &values_stat) == 0 ? values_stat.st_mtime : 0;
    bool lua_changed = s_lua_values_mtime != 0 && lua_values_mtime != s_lua_values_mtime;
    s_lua_values_mtime = lua_values_mtime;
    if (name.empty() || (current == s_snapshot && !lua_changed)) return;
    if (settings::SaveConfig(name)) s_snapshot.swap(current);
}

static gboolean AutosaveTick(gpointer) {
    AutosaveConfig();
    return G_SOURCE_CONTINUE;
}

static gboolean SyncToggles(gpointer) {
    saturation::Update(*g_cfg, g_hud.cs2_focused.load() && g_hud.in_game.load());
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

    GtkWidget* sc_page = AddPage(nb, "Scripts");
    gtk_box_pack_start(GTK_BOX(sc_page), Section("Lua Scripts"), FALSE, FALSE, 0);
    GtkWidget* btn_reload = gtk_button_new_with_label("Reload all scripts");
    g_signal_connect(btn_reload, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
        features::ReloadLua();
    }), nullptr);
    gtk_box_pack_start(GTK_BOX(sc_page), btn_reload, FALSE, FALSE, 0);
    GtkWidget* btn_sc_dir = gtk_button_new_with_label("Open scripts folder");
    g_signal_connect(btn_sc_dir, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
        system("xdg-open scripts >/dev/null 2>&1 &");
    }), nullptr);
    gtk_box_pack_start(GTK_BOX(sc_page), btn_sc_dir, FALSE, FALSE, 0);
    return win;
}

static void OnQuit(GtkButton*, gpointer) {
    s_running.store(false);
    if (g_gui_win)     gtk_widget_hide(g_gui_win);
    if (g_overlay_win) gtk_widget_hide(g_overlay_win);
    gtk_main_quit();
}

static void CrashShutdown(int sig) {
    g_input.Shutdown();
    saturation::Reset();
    std::signal(sig, SIG_DFL);
    raise(sig);
}

static void PanicShutdown(int) {
    std::signal(SIGSEGV, CrashShutdown);
    std::signal(SIGABRT, CrashShutdown);
    AutosaveConfig();
    features::StopTriggerBot();
    features::StopAimbot();
    features::StopRcs();
    features::StopMovement();
    features::StopChams();
    features::StopEffects();
    nightsky::Shutdown();
    g_input.Shutdown();
    saturation::Reset();
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
        vis::Update();
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

static bool CheckCursorVisible(Display* d) {
    if (!d) return false;
    XFixesCursorImage* img = XFixesGetCursorImage(d);
    if (!img) return false;
    bool visible = true;
    if (img->width <= 1 && img->height <= 1) {
        if (!img->pixels || img->pixels[0] == 0) visible = false;
    }
    XFree(img);
    return visible;
}


static void WriteInternalFlags() {
    if (!g_cfg) return;
    const char* tmp = "/tmp/spx_internal.flags.tmp";
    const char* dst = "/tmp/spx_internal.flags";
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "silent_aim %u\n",   settings::Enabled(g_cfg->silent_aim) ? 1u : 0u);
    fprintf(f, "thirdperson %u\n",  settings::Enabled(g_cfg->thirdperson_internal) ? 1u : 0u);
    fprintf(f, "night_mode %u\n",   settings::Enabled(g_cfg->night_mode_internal) ? 1u : 0u);
    fprintf(f, "anti_aim %u\n",     settings::Enabled(g_cfg->anti_aim) ? 1u : 0u);
    fclose(f);
    rename(tmp, dst);
}

static void InternalFlagsThread() {
    while (s_running.load()) {
        WriteInternalFlags();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}


static void WriteBridgeFlags() {
    if (!off::g_EntityListPtr) return;
    const char* tmp = "/tmp/spx_bridge.txt.tmp";
    const char* dst = "/tmp/spx_bridge.txt";
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "entity_list 0x%lx\n", (unsigned long)off::g_EntityListPtr);
    fprintf(f, "local_controller_idx %d\n", off::g_LocalControllerIdx);
    fprintf(f, "client_base 0x%lx\n", (unsigned long)off::g_ClientBase);
    fclose(f);
    rename(tmp, dst);
}

static void CheckGrenadeToken() {
    static int32_t last_token = -1;
    if (!g_cfg) return;
    int32_t cur_token = g_cfg->grenade_helper_save_token;
    if (last_token < 0) {
        last_token = cur_token;
        return;
    }
    if (cur_token != last_token) {
        last_token = cur_token;
        grenade::AddCurrentSpot();
    }
}

static void BridgeThread() {
    while (s_running.load()) {
        WriteBridgeFlags();
        CheckGrenadeToken();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

static gboolean OnGrenadeSave(gpointer) {
    grenade::AddCurrentSpot();
    return G_SOURCE_REMOVE;
}

static gboolean OnGrenadeRemove(gpointer) {
    grenade::RemoveNearestSpot();
    return G_SOURCE_REMOVE;
}

static void HotkeyThread() {
    struct HotkeyBinding { uint32_t* bind; uint32_t* feature; GSourceFunc action; };
    const HotkeyBinding bindings[] = {
        {&g_cfg->bind_toggle_gui, nullptr, ToggleGui},
        {&g_cfg->bind_edit_hud, nullptr, ToggleEdit},
        {&g_cfg->bind_trigger, &g_cfg->trigger_enabled, nullptr},
        {&g_cfg->bind_watermark, &g_cfg->watermark, nullptr},
        {&g_cfg->bind_bomb, &g_cfg->bomb_timer, nullptr},
        {&g_cfg->bind_crosshair, &g_cfg->crosshair, nullptr},
        {&g_cfg->bind_esp, &g_cfg->esp, nullptr},
        {&g_cfg->bind_bunnyhop, &g_cfg->bunnyhop, nullptr},
        {&g_cfg->bind_auto_strafe, &g_cfg->auto_strafe, nullptr},
        {&g_cfg->bind_keybinds, &g_cfg->keybinds, nullptr},
        {&g_cfg->bind_aimbot, &g_cfg->aimbot_enabled, nullptr},
        {&g_cfg->bind_glow, &g_cfg->glow, nullptr},
        {&g_cfg->bind_chams, &g_cfg->chams, nullptr},
        {&g_cfg->bind_sound_esp, &g_cfg->sound_esp, nullptr},
        {&g_cfg->bind_weapon_esp, &g_cfg->esp_dropped_weapons, nullptr},
        {&g_cfg->bind_arrows, &g_cfg->arrows, nullptr},
        {&g_cfg->bind_night_mode, &g_cfg->night_mode, nullptr},
        {&g_cfg->bind_thirdperson, &g_cfg->thirdperson, nullptr},
        {&g_cfg->bind_force_shot, &g_cfg->trigger_force_shot, nullptr},
        {&g_cfg->bind_spread_trigger, &g_cfg->trigger_spread, nullptr},
        {&g_cfg->bind_fast_stop, &g_cfg->fast_stop_enabled, nullptr},
        {&g_cfg->bind_md_override, &g_cfg->trigger_md_override, nullptr},
        {&g_cfg->bind_edge_bug, &g_cfg->edge_bug, nullptr},
        {&g_cfg->bind_edge_jump, &g_cfg->edge_jump, nullptr},
        {&g_cfg->bind_silent_aim, &g_cfg->silent_aim, nullptr},
        {&g_cfg->bind_grenade_helper_aim, &g_cfg->grenade_helper_aim, nullptr},
        {&g_cfg->bind_grenade_helper_save, nullptr, OnGrenadeSave},
        {&g_cfg->bind_grenade_helper_remove, nullptr, OnGrenadeRemove},
    };
    constexpr size_t kCount = sizeof(bindings) / sizeof(bindings[0]);
    Display* d = XOpenDisplay(nullptr);
    bool prev[kCount] = {};
    int applied_mode[kCount];
    std::fill(std::begin(applied_mode), std::end(applied_mode), -1);
    while (s_running.load()) {
        if (!IsCs2Active()) {
            s_cursor_active.store(false);
            for (size_t i = 0; i < kCount; i++) prev[i] = false;
            CheckGrenadeToken();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        CheckGrenadeToken();
        s_cursor_active.store(CheckCursorVisible(d));

        char keys[32]{};
        if (d) XQueryKeymap(d, keys);
        for (size_t i = 0; i < kCount; i++) {
            const HotkeyBinding& binding = bindings[i];
            uint32_t kv = *binding.bind;
            bool now = false;
            int linux_key = kv ? LinuxKeyFromGdk(kv) : -1;
            if (linux_key >= 0) now = g_input.IsKeyDown(linux_key);
            if (!now && d && kv) {
                KeyCode kc;
                if (XkbToGdk(d, kv, kc)) now = (keys[kc / 8] & (1 << (kc & 7))) != 0;
            }
            auto mode = binding.feature ? settings::GetBindMode(*g_cfg, binding.bind) : settings::BindMode::Toggle;
            bool mode_changed = applied_mode[i] != static_cast<int>(mode);
            applied_mode[i] = static_cast<int>(mode);
            if (!kv || kv == 0xFFFFFFFFu || kv >= 0xFFFF00u) { prev[i] = false; continue; }
            if (mode == settings::BindMode::Toggle) {
                if (now && !prev[i]) {
                    if (binding.action) g_idle_add(binding.action, nullptr);
                    else {
                        settings::ToggleEnabled(*binding.feature);
                        const char* name = FeatureName(binding.feature);
                        if (name && settings::Enabled(g_cfg->notifications)) {
                            bool on = settings::Enabled(*binding.feature);
                            PushNotice(std::string(name) + (on ? " enabled" : " disabled"), on ? NoticeKind::On : NoticeKind::Off);
                        }
                    }
                }
            } else if (now != prev[i] || mode_changed) {
                if (binding.feature) settings::SetEnabled(*binding.feature, (mode == settings::BindMode::Hold) == now);
            }
            prev[i] = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
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

static void StartMediaThread() {
    static std::thread t([]() {
        while (s_running.load()) {
            char buf[512]{};
            FILE* p = popen("playerctl -a metadata --format '{{status}}||{{artist}}||{{title}}||{{playerName}}' 2>/dev/null", "r");
            if (p) {
                char best_status[32]{};
                char best_artist[128]{};
                char best_title[128]{};
                char best_player[64]{};
                bool found_any = false;
                bool found_playing = false;

                while (fgets(buf, sizeof(buf), p)) {
                    char* s1 = strstr(buf, "||");
                    if (!s1) continue;
                    *s1 = '\0';
                    char* artist = s1 + 2;
                    char* s2 = strstr(artist, "||");
                    if (!s2) continue;
                    *s2 = '\0';
                    char* title = s2 + 2;
                    char* s3 = strstr(title, "||");
                    char player[64]{};
                    if (s3) {
                        *s3 = '\0';
                        char* pl = s3 + 2;
                        size_t plen = strlen(pl);
                        while (plen > 0 && (pl[plen - 1] == '\n' || pl[plen - 1] == '\r')) pl[--plen] = '\0';
                        snprintf(player, sizeof(player), "%s", pl);
                    }
                    size_t tlen = strlen(title);
                    while (tlen > 0 && (title[tlen - 1] == '\n' || title[tlen - 1] == '\r')) title[--tlen] = '\0';

                    if (title[0] == '\0') continue;

                    bool is_playing = (strcasecmp(buf, "Playing") == 0);
                    if (!found_any || (!found_playing && is_playing)) {
                        snprintf(best_status, sizeof(best_status), "%s", buf);
                        snprintf(best_artist, sizeof(best_artist), "%s", artist);
                        snprintf(best_title, sizeof(best_title), "%s", title);
                        snprintf(best_player, sizeof(best_player), "%s", player);
                        found_any = true;
                        if (is_playing) found_playing = true;
                    }
                }
                pclose(p);

                std::lock_guard<std::mutex> lk(g_hud.media_mtx);
                if (found_any) {
                    snprintf(g_hud.media_status, sizeof(g_hud.media_status), "%s", best_status);
                    snprintf(g_hud.media_artist, sizeof(g_hud.media_artist), "%s", best_artist);
                    snprintf(g_hud.media_title, sizeof(g_hud.media_title), "%s", best_title);
                    snprintf(g_hud.media_player_name, sizeof(g_hud.media_player_name), "%s", best_player);
                    g_hud.media_active = true;
                } else {
                    g_hud.media_active = false;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
    });
    t.detach();
}

int main(int argc, char** argv) {
    setenv("OMP_WAIT_POLICY", "PASSIVE", 0);
    XInitThreads();
    std::signal(SIGINT,  PanicShutdown);
    std::signal(SIGTERM, PanicShutdown);
    std::signal(SIGSEGV, CrashShutdown);
    std::signal(SIGABRT, CrashShutdown);

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
    if (g_cfg->hud_radar_x >= 0) { s_r_radar.x = g_cfg->hud_radar_x; s_r_radar.y = g_cfg->hud_radar_y; }
    if (g_cfg->hud_spectators_x >= 0) { s_r_spectators.x = g_cfg->hud_spectators_x; s_r_spectators.y = g_cfg->hud_spectators_y; }
    if (g_cfg->hud_media_x >= 0) { s_r_media.x = g_cfg->hud_media_x; s_r_media.y = g_cfg->hud_media_y; }

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
    features::StartChams();
    xhair::Start();
    features::StartEffects();
    features::InitLua(g_cfg);
    StartMediaThread();
    std::thread ft(FetchThread);
    std::thread ht(HotkeyThread);
    std::thread ift(InternalFlagsThread); ift.detach();
    std::thread btb(BridgeThread); btb.detach();

    g_timeout_add(100, Tick, nullptr);
    gtk_widget_add_tick_callback(g_area, OnFrameClock, nullptr, nullptr);
    g_timeout_add(150, SyncToggles, nullptr);
    g_timeout_add(2000, AutosaveTick, nullptr);
    g_timeout_add(200, HideGuiIfCs2Blurred, nullptr);
    gtk_main();
    AutosaveConfig();
    saturation::Reset();

    s_running.store(false);
    if (ft.joinable()) ft.join();
    if (ht.joinable()) ht.join();
    features::StopTriggerBot();
    features::StopAimbot();
    features::StopRcs();
    features::StopMovement();
    features::StopChams();
    features::StopEffects();
    nightsky::Shutdown();
    features::ShutdownLua();
    g_input.Shutdown();
    return 0;
}
