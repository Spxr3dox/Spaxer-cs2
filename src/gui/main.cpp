#include "config/settings.h"
#include "features/hit_sound.h"
#include "render/esp_icons.h"
#include "state.h"
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <atomic>
#include <algorithm>
#include <thread>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <array>
#include <fstream>
#include <locale>
#include <cctype>
#include <string>
#include <vector>
#include <filesystem>
#include <map>
#include <spawn.h>
#include <sys/stat.h>

static Settings* g_cfg = nullptr;
static GtkWidget* g_win = nullptr;
static GtkWidget* g_stack = nullptr;
static std::atomic<bool> g_running{true};
static GtkWidget* BuildGui();

struct BindCtx { GtkButton* btn; uint32_t* field; };
struct SwitchBinding { GtkSwitch* sw; uint32_t* field; };

static std::vector<SwitchBinding> g_switches;
struct ColorBinding { GtkColorChooser* chooser; uint32_t* field; };
struct SliderBinding { GtkRange* range; int32_t* field; double scale; };
struct CheckBinding { GtkToggleButton* check; uint32_t* field; GtkWidget* dependent; };
static std::vector<CheckBinding> g_checks;
static std::vector<ColorBinding> g_colors;
static std::vector<SliderBinding> g_sliders;
static std::vector<BindCtx*> g_bind_buttons;
static bool g_capturing_bind = false;
struct SearchRow { GtkWidget* row; std::string text; };
static std::vector<SearchRow> g_search_rows;
static GtkWidget* g_search_entry = nullptr;

static std::string Lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

static void RegisterSearchRow(GtkWidget* row, const char* label) {
    g_search_rows.push_back({row, Lowercase(label)});
}

static void SetBindLabel(GtkButton* btn, uint32_t kv) {
    if (kv == 0 || kv == 0xFFFFFFFFu || kv >= 0xFFFF00u) { gtk_button_set_label(btn, "bind"); return; }
    const char* n = gdk_keyval_name(kv);
    gtk_button_set_label(btn, n ? n : "bind");
}

static void InstallCss();

static gboolean SyncWidgetsWithSettings(gpointer) {
    if (g_win) {
        bool visible = gtk_widget_get_visible(g_win);
        g_cfg->gui_open = visible ? 1u : 0u;
        if (visible) {
            gint x = 0, y = 0, w = 0, h = 0;
            gtk_window_get_position(GTK_WINDOW(g_win), &x, &y);
            gtk_window_get_size(GTK_WINDOW(g_win), &w, &h);
            g_cfg->gui_x = x; g_cfg->gui_y = y; g_cfg->gui_w = w; g_cfg->gui_h = h;
        }
    }
    for (const SwitchBinding& binding : g_switches) {
        bool enabled = settings::Enabled(*binding.field);
        if (gtk_switch_get_active(binding.sw) != enabled) gtk_switch_set_active(binding.sw, enabled);
    }
    if (!g_capturing_bind) {
        for (BindCtx* ctx : g_bind_buttons) {
            if (ctx && ctx->btn && ctx->field) {
                const char* cur = gtk_button_get_label(ctx->btn);
                if (cur && std::string(cur) == "…") continue;
                SetBindLabel(ctx->btn, *ctx->field);
            }
        }
    }
    for (const ColorBinding& binding : g_colors) {
        GdkRGBA shown;
        gtk_color_chooser_get_rgba(binding.chooser, &shown);
        uint32_t rgba = *binding.field;
        GdkRGBA wanted{((rgba >> 24) & 0xFF) / 255.0, ((rgba >> 16) & 0xFF) / 255.0, ((rgba >> 8) & 0xFF) / 255.0, (rgba & 0xFF) / 255.0};
        if (std::fabs(shown.red - wanted.red) + std::fabs(shown.green - wanted.green) + std::fabs(shown.blue - wanted.blue) +
            std::fabs(shown.alpha - wanted.alpha) > 0.01)
            gtk_color_chooser_set_rgba(binding.chooser, &wanted);
    }
    for (const CheckBinding& binding : g_checks) {
        bool enabled = settings::Enabled(*binding.field);
        if (gtk_toggle_button_get_active(binding.check) != enabled) gtk_toggle_button_set_active(binding.check, enabled);
        if (binding.dependent && gtk_widget_get_sensitive(binding.dependent) != enabled) gtk_widget_set_sensitive(binding.dependent, enabled);
    }
    for (const SliderBinding& binding : g_sliders) {
        double wanted = *binding.field / binding.scale;
        if (std::fabs(gtk_range_get_value(binding.range) - wanted) > 1e-6) gtk_range_set_value(binding.range, wanted);
    }
    static uint32_t last_theme = 999, last_accent = 999;
    if (g_cfg && (g_cfg->hud_theme != last_theme || g_cfg->hud_accent_rgba != last_accent)) {
        last_theme = g_cfg->hud_theme;
        last_accent = g_cfg->hud_accent_rgba;
        InstallCss();
    }
    return G_SOURCE_CONTINUE;
}

static gboolean OnBindKey(GtkWidget* w, GdkEventKey* e, gpointer d) {
    BindCtx* b = static_cast<BindCtx*>(d);
    guint kv = e->keyval;
    guint base_layout_kv = 0;
    if (gdk_keymap_translate_keyboard_state(gdk_keymap_get_for_display(gdk_display_get_default()),
                                            e->hardware_keycode, GdkModifierType(0), 0,
                                            &base_layout_kv, nullptr, nullptr, nullptr) && base_layout_kv)
        kv = gdk_keyval_to_lower(base_layout_kv);
    if (kv == GDK_KEY_Escape || kv == GDK_KEY_VoidSymbol || kv >= 0xFFFF00u || !gdk_keyval_name(kv)) kv = 0;
    *b->field = kv;
    SetBindLabel(b->btn, kv);
    g_signal_handlers_disconnect_by_func(w, (gpointer)OnBindKey, d);
    g_capturing_bind = false;
    return TRUE;
}

static void OnBindClicked(GtkButton* btn, gpointer d) {
    BindCtx* b = static_cast<BindCtx*>(d);
    gtk_button_set_label(btn, "…");
    g_capturing_bind = true;
    GtkWidget* top = gtk_widget_get_toplevel(GTK_WIDGET(btn));
    g_signal_connect(top, "key-press-event", G_CALLBACK(OnBindKey), b);
}

static GtkWidget* MakeBindBtn(uint32_t* field) {
    const char* n = gdk_keyval_name(*field);
    GtkWidget* btn = gtk_button_new_with_label(*field ? (n ? n : "?") : "bind");
    gtk_style_context_add_class(gtk_widget_get_style_context(btn), "bind");
    gtk_widget_set_size_request(btn, 78, 26);
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    BindCtx* ctx = new BindCtx{GTK_BUTTON(btn), field};
    g_bind_buttons.push_back(ctx);
    g_signal_connect(btn, "clicked", G_CALLBACK(OnBindClicked), ctx);
    return btn;
}

static GtkWidget* MakeRow(const char* label, uint32_t* toggle, uint32_t* bind_field) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "row");
    GtkWidget* lbl = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.f);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);
    if (bind_field) {
        gtk_box_pack_start(GTK_BOX(row), MakeBindBtn(bind_field), FALSE, FALSE, 0);
    }
    if (toggle) {
        GtkWidget* sw = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(sw), settings::Enabled(*toggle));
        g_signal_connect(sw, "state-set", G_CALLBACK(+[](GtkSwitch*, gboolean s, gpointer d) -> gboolean {
            settings::SetEnabled(*static_cast<uint32_t*>(d), s);
            return FALSE;
        }), toggle);
        gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(row), sw, FALSE, FALSE, 0);
        g_switches.push_back({GTK_SWITCH(sw), toggle});
    }
    RegisterSearchRow(row, label);
    return row;
}

static GtkWidget* MakeSliderRow(const char* label, int32_t* field, int min, int max, int step) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "row");
    GtkWidget* lbl = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.f);
    gtk_widget_set_size_request(lbl, 130, -1);
    GtkAdjustment* adj = gtk_adjustment_new(*field, min, max, step, step * 5, 0);
    GtkWidget* scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, adj);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
    gtk_scale_set_digits(GTK_SCALE(scale), 0);
    g_signal_connect(scale, "value-changed", G_CALLBACK(+[](GtkRange* r, gpointer d) {
        *static_cast<int32_t*>(d) = (int32_t)gtk_range_get_value(r);
    }), field);
    g_sliders.push_back({GTK_RANGE(scale), field, 1.0});
    gtk_box_pack_start(GTK_BOX(row), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), scale, TRUE, TRUE, 0);
    RegisterSearchRow(row, label);
    return row;
}

static GtkWidget* MakeCheckSliderRow(const char* label, uint32_t* toggle, int32_t* field, int min, int max, int step) {
    GtkWidget* row = MakeSliderRow(label, field, min, max, step);
    GList* children = gtk_container_get_children(GTK_CONTAINER(row));
    GtkWidget* label_widget = GTK_WIDGET(g_list_nth_data(children, 0));
    GtkWidget* scale = GTK_WIDGET(g_list_nth_data(children, 1));
    g_list_free(children);
    gtk_widget_set_size_request(label_widget, 104, -1);
    GtkWidget* check = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), settings::Enabled(*toggle));
    gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive(scale, settings::Enabled(*toggle));
    g_signal_connect(check, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer d) {
        settings::SetEnabled(*static_cast<uint32_t*>(d), gtk_toggle_button_get_active(b));
    }), toggle);
    gtk_box_pack_start(GTK_BOX(row), check, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(row), check, 0);
    g_checks.push_back({GTK_TOGGLE_BUTTON(check), toggle, scale});
    return row;
}

static GtkWidget* MakeFovSliderRow(const char* label, int32_t* field_x100) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "row");
    GtkWidget* lbl = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.f);
    gtk_widget_set_size_request(lbl, 130, -1);
    double degrees = std::clamp(*field_x100 / 100.0, 1.0, 180.0);
    GtkAdjustment* adj = gtk_adjustment_new(degrees, 1.0, 180.0, 0.5, 5.0, 0);
    GtkWidget* scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, adj);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
    gtk_scale_set_digits(GTK_SCALE(scale), 1);
    g_signal_connect(scale, "format-value", G_CALLBACK(+[](GtkScale*, gdouble value, gpointer) -> gchar* {
        return g_strdup_printf("%.1f°", value);
    }), nullptr);
    g_signal_connect(scale, "value-changed", G_CALLBACK(+[](GtkRange* r, gpointer d) {
        *static_cast<int32_t*>(d) = static_cast<int32_t>(gtk_range_get_value(r) * 100.0 + 0.5);
    }), field_x100);
    g_sliders.push_back({GTK_RANGE(scale), field_x100, 100.0});
    gtk_box_pack_start(GTK_BOX(row), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), scale, TRUE, TRUE, 0);
    RegisterSearchRow(row, label);
    return row;
}

static GtkWidget* MakeComboRow(const char* label, uint32_t* field, std::initializer_list<const char*> options) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "row");
    GtkWidget* lbl = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.f);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);
    GtkWidget* combo = gtk_combo_box_text_new();
    for (const char* option : options) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), option);
    uint32_t count = static_cast<uint32_t>(options.size());
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), *field < count ? static_cast<gint>(*field) : 0);
    gtk_widget_set_valign(combo, GTK_ALIGN_CENTER);
    g_signal_connect(combo, "changed", G_CALLBACK(+[](GtkComboBox* c, gpointer d) {
        gint active = gtk_combo_box_get_active(c);
        if (active >= 0) *static_cast<uint32_t*>(d) = static_cast<uint32_t>(active);
    }), field);
    gtk_box_pack_start(GTK_BOX(row), combo, FALSE, FALSE, 0);
    RegisterSearchRow(row, label);
    return row;
}

static GtkWidget* MakeColorRow(const char* label, uint32_t* field) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "row");
    GtkWidget* lbl = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.f);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);

    GtkWidget* btn = gtk_color_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(btn), "color-btn");
    GdkRGBA c;
    uint32_t rgba = *field;
    c.red   = ((rgba >> 24) & 0xFF) / 255.0;
    c.green = ((rgba >> 16) & 0xFF) / 255.0;
    c.blue  = ((rgba >>  8) & 0xFF) / 255.0;
    c.alpha = ( rgba        & 0xFF) / 255.0;
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(btn), &c);
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(btn), TRUE);
    g_colors.push_back({GTK_COLOR_CHOOSER(btn), field});

    g_signal_connect(btn, "color-set", G_CALLBACK(+[](GtkColorButton* b, gpointer d) {
        GdkRGBA col;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(b), &col);
        uint32_t r = static_cast<uint8_t>(col.red * 255.f);
        uint32_t g = static_cast<uint8_t>(col.green * 255.f);
        uint32_t bv = static_cast<uint8_t>(col.blue * 255.f);
        uint32_t a = static_cast<uint8_t>(col.alpha * 255.f);
        *static_cast<uint32_t*>(d) = (r << 24) | (g << 16) | (bv << 8) | a;
    }), field);

    gtk_box_pack_start(GTK_BOX(row), btn, FALSE, FALSE, 0);
    RegisterSearchRow(row, label);
    return row;
}

static GtkWidget* MakeCard(const char* title) {
    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "card");
    if (title) {
        GtkWidget* head = gtk_label_new(title);
        gtk_label_set_xalign(GTK_LABEL(head), 0.f);
        gtk_style_context_add_class(gtk_widget_get_style_context(head), "card-title");
        gtk_box_pack_start(GTK_BOX(card), head, FALSE, FALSE, 0);
    }
    return card;
}

static GtkWidget* MakePage() {
    GtkWidget* scr = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(scr, TRUE);
    gtk_widget_set_vexpand(scr, TRUE);
    GtkWidget* columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_box_set_homogeneous(GTK_BOX(columns), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(columns), "page");
    GtkWidget* left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    GtkWidget* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_box_pack_start(GTK_BOX(columns), left, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(columns), right, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(scr), columns);
    g_object_set_data(G_OBJECT(scr), "left", left);
    g_object_set_data(G_OBJECT(scr), "right", right);
    return scr;
}

static GtkWidget* PageBox(GtkWidget* page) {
    return GTK_WIDGET(g_object_get_data(G_OBJECT(page), "left"));
}

static void Place(GtkWidget* page, GtkWidget* card, bool right) {
    GtkWidget* column = GTK_WIDGET(g_object_get_data(G_OBJECT(page), right ? "right" : "left"));
    gtk_box_pack_start(GTK_BOX(column), card, FALSE, FALSE, 0);
}

static GtkWidget* MakeSvgWidget(const char* svg_data, int size = 16) {
    GInputStream* stream = g_memory_input_stream_new_from_data(svg_data, -1, nullptr);
    GdkPixbuf* pb = gdk_pixbuf_new_from_stream_at_scale(stream, size, size, TRUE, nullptr, nullptr);
    g_object_unref(stream);
    if (!pb) return gtk_label_new("</>");
    GtkWidget* img = gtk_image_new_from_pixbuf(pb);
    g_object_unref(pb);
    return img;
}

struct SidebarItem { GtkWidget* button; std::string prefix; };
static std::vector<SidebarItem> g_sidebar_items;

static void RefreshSidebarHighlight() {
    const char* visible = gtk_stack_get_visible_child_name(GTK_STACK(g_stack));
    std::string current = visible ? visible : "";
    for (const SidebarItem& item : g_sidebar_items) {
        GtkStyleContext* style = gtk_widget_get_style_context(item.button);
        bool active = current == item.prefix;
        if (active) gtk_style_context_add_class(style, "active");
        else gtk_style_context_remove_class(style, "active");
    }
}

static GtkWidget* SidebarButton(const char* svg_data, const char* label, int icon_size, const char* extra_class) {
    GtkWidget* btn = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(btn), "sidebar-item");
    if (extra_class) gtk_style_context_add_class(gtk_widget_get_style_context(btn), extra_class);
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    if (svg_data) {
        GtkWidget* ic = MakeSvgWidget(svg_data, icon_size);
        gtk_style_context_add_class(gtk_widget_get_style_context(ic), "sidebar-icon");
        gtk_box_pack_start(GTK_BOX(row), ic, FALSE, FALSE, 0);
    }
    GtkWidget* lbl = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.f);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(btn), row);
    return btn;
}

static void ShowPage(GtkButton*, gpointer name) {
    gtk_stack_set_visible_child_name(GTK_STACK(g_stack), static_cast<const char*>(name));
}

static void AddSidebarItemSvg(GtkWidget* sidebar, const char* svg_data, const char* name, const char* label) {
    GtkWidget* btn = SidebarButton(svg_data, label, 22, nullptr);
    g_signal_connect(btn, "clicked", G_CALLBACK(ShowPage), (gpointer)name);
    gtk_box_pack_start(GTK_BOX(sidebar), btn, FALSE, FALSE, 0);
    g_sidebar_items.push_back({btn, name});
}

static void AddSidebarHeader(GtkWidget* sidebar, const char* title) {
    GtkWidget* label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0.f);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "sidebar-header");
    gtk_box_pack_start(GTK_BOX(sidebar), label, FALSE, FALSE, 0);
}

static void ReadSteamProfile(std::string& name, std::string& avatar_path) {
    const char* home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.local/share/Steam/config/loginusers.vdf", home);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    std::string current_id;
    bool inside = false;
    std::string best_name;
    while (fgets(line, sizeof(line), f)) {
        char a[256]{}, b[256]{};
        if (sscanf(line, " \"%255[^\"]\" \"%255[^\"]\"", a, b) == 2) {
            std::string k = a, v = b;
            if (k == "PersonaName") best_name = v;
            if (k == "AutoLogin" && v == "1" && !current_id.empty()) {
                name = best_name;
                char av[512];
                snprintf(av, sizeof(av), "%s/.local/share/Steam/config/avatarcache/%s.png", home, current_id.c_str());
                avatar_path = av;
                fclose(f);
                return;
            }
        } else if (sscanf(line, " \"%255[^\"]\"", a) == 1) {
            if (strlen(a) > 8 && a[0] == '7' && a[1] == '6') {
                current_id = a;
                best_name.clear();
                inside = true;
            }
        }
        (void)inside;
    }
    fclose(f);
    if (!best_name.empty() && name.empty()) name = best_name;
}

static GtkWidget* SteamProfileWidget() {
    std::string name, avatar;
    ReadSteamProfile(name, avatar);
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "profile");
    GtkWidget* av = nullptr;
    if (!avatar.empty()) {
        GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(avatar.c_str(), 36, 36, TRUE, nullptr);
        if (pb) {
            av = gtk_image_new_from_pixbuf(pb);
            g_object_unref(pb);
        }
    }
    if (!av) av = gtk_image_new_from_icon_name("avatar-default", GTK_ICON_SIZE_DND);
    gtk_style_context_add_class(gtk_widget_get_style_context(av), "avatar");
    gtk_box_pack_start(GTK_BOX(row), av, FALSE, FALSE, 0);

    GtkWidget* txt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* lbl = gtk_label_new(name.empty() ? "Not signed in" : name.c_str());
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.f);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "profile-name");
    GtkWidget* sub = gtk_label_new("Steam");
    gtk_label_set_xalign(GTK_LABEL(sub), 0.f);
    gtk_style_context_add_class(gtk_widget_get_style_context(sub), "profile-sub");
    gtk_box_pack_start(GTK_BOX(txt), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(txt), sub, FALSE, FALSE, 0);
    gtk_widget_set_hexpand(txt, TRUE);
    gtk_widget_set_valign(txt, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(row), txt, TRUE, TRUE, 0);
    return row;
}

static void RefreshConfigCombo(GtkComboBoxText* combo) {
    gtk_combo_box_text_remove_all(combo);
    for (auto& n : settings::ListConfigs())
        gtk_combo_box_text_append_text(combo, n.c_str());
    std::string last = settings::LastConfig();
    if (!last.empty()) {
        int i = 0;
        for (auto& n : settings::ListConfigs()) {
            if (n == last) { gtk_combo_box_set_active(GTK_COMBO_BOX(combo), i); break; }
            i++;
        }
    }
}

static GtkWidget* ConfigsWidget() {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "configs");
    GtkWidget* title = gtk_label_new("CONFIGS");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "configs-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.f);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

    GtkWidget* combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(combo, TRUE);
    RefreshConfigCombo(GTK_COMBO_BOX_TEXT(combo));
    gtk_box_pack_start(GTK_BOX(box), combo, FALSE, FALSE, 0);

    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "name");
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget* save = gtk_button_new_with_label("Save");
    GtkWidget* load = gtk_button_new_with_label("Load");
    GtkWidget* del  = gtk_button_new_with_label("Del");
    gtk_widget_set_hexpand(save, TRUE);
    gtk_widget_set_hexpand(load, TRUE);
    gtk_widget_set_hexpand(del,  TRUE);
    struct CfgCtx { GtkComboBoxText* combo; GtkEntry* entry; };
    static CfgCtx ctx;
    ctx = {GTK_COMBO_BOX_TEXT(combo), GTK_ENTRY(entry)};
    g_signal_connect(save, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        CfgCtx* c = static_cast<CfgCtx*>(d);
        const char* n = gtk_entry_get_text(c->entry);
        if (n && *n) { settings::SaveConfig(n); RefreshConfigCombo(c->combo); }
    }), &ctx);
    g_signal_connect(load, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        CfgCtx* c = static_cast<CfgCtx*>(d);
        gchar* n = gtk_combo_box_text_get_active_text(c->combo);
        if (n) {
            if (settings::LoadConfig(n)) {
                g_idle_add(+[](gpointer) -> gboolean {
                    if (g_win) {
                        GtkWidget* old = g_win;
                        g_win = BuildGui();
                        gtk_widget_show_all(g_win);
                        gtk_widget_destroy(old);
                    }
                    return G_SOURCE_REMOVE;
                }, nullptr);
            }
            g_free(n);
        }
    }), &ctx);
    g_signal_connect(del, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        CfgCtx* c = static_cast<CfgCtx*>(d);
        gchar* n = gtk_combo_box_text_get_active_text(c->combo);
        if (n) { settings::DeleteConfig(n); g_free(n); RefreshConfigCombo(c->combo); }
    }), &ctx);
    gtk_box_pack_start(GTK_BOX(row), save, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), load, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), del,  TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return box;
}

struct PreviewColor { double r, g, b, a; };

static PreviewColor Unpack(uint32_t rgba) {
    return {((rgba >> 24) & 0xFF) / 255.0, ((rgba >> 16) & 0xFF) / 255.0, ((rgba >> 8) & 0xFF) / 255.0, (rgba & 0xFF) / 255.0};
}

static void PreviewText(cairo_t* cr, const char* text, double center_x, double baseline, double size, bool bold,
                        PreviewColor color) {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_text_extents_t extents;
    cairo_text_extents(cr, text, &extents);
    cairo_move_to(cr, std::round(center_x - extents.width * 0.5 - extents.x_bearing), std::round(baseline));
    cairo_text_path(cr, text);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
    cairo_set_line_width(cr, 3.0);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
    cairo_fill(cr);
}

struct AgentPreview {
    cairo_surface_t* color = nullptr;
    cairo_surface_t* shade = nullptr;
    double width = 0, height = 0;
    double bounds[4]{};
    double head[2]{};
    std::vector<std::array<double, 4>> bones;
    bool loaded = false;
};

static const AgentPreview& Agent() {
    static AgentPreview agent = [] {
        AgentPreview result;
        const char* home = getenv("HOME");
        std::string dir = std::string(home ? home : "/tmp") + "/.config/spaxer/preview/";
        result.color = cairo_image_surface_create_from_png((dir + "agent.png").c_str());
        result.shade = cairo_image_surface_create_from_png((dir + "agent_shade.png").c_str());
        std::ifstream meta(dir + "agent.txt");
        meta.imbue(std::locale::classic());
        std::string key;
        while (meta >> key) {
            if (key == "size") meta >> result.width >> result.height;
            else if (key == "bounds") meta >> result.bounds[0] >> result.bounds[1] >> result.bounds[2] >> result.bounds[3];
            else if (key == "head") meta >> result.head[0] >> result.head[1];
            else if (key == "bone") {
                std::array<double, 4> bone{};
                meta >> bone[0] >> bone[1] >> bone[2] >> bone[3];
                result.bones.push_back(bone);
            }
        }
        result.loaded = cairo_surface_status(result.color) == CAIRO_STATUS_SUCCESS &&
                        cairo_surface_status(result.shade) == CAIRO_STATUS_SUCCESS && result.width > 0;
        return result;
    }();
    return agent;
}

static void PaintTinted(cairo_t* cr, cairo_surface_t* mask, PreviewColor color, double alpha) {
    cairo_set_source_rgba(cr, color.r, color.g, color.b, alpha);
    cairo_mask_surface(cr, mask, 0, 0);
}

static gboolean DrawEspPreview(GtkWidget* area, cairo_t* cr, gpointer) {
    double w = gtk_widget_get_allocated_width(area), h = gtk_widget_get_allocated_height(area);
    cairo_pattern_t* bg = cairo_pattern_create_linear(0, 0, 0, h);
    cairo_pattern_add_color_stop_rgb(bg, 0, 0.16, 0.17, 0.21);
    cairo_pattern_add_color_stop_rgb(bg, 0.8, 0.1, 0.1, 0.13);
    cairo_pattern_add_color_stop_rgb(bg, 1, 0.07, 0.07, 0.09);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);

    const AgentPreview& agent = Agent();
    if (!agent.loaded) {
        PreviewText(cr, "Run tools/preview_model.py", w * 0.5, h * 0.5, 12, false, {0.7, 0.72, 0.78, 1});
        return FALSE;
    }
    double scale = std::min(w * 0.72 / agent.width, h * 0.86 / agent.height);
    double ox = w * 0.5 - agent.width * scale * 0.5, oy = h * 0.075;
    auto X = [&](double x) { return ox + x * scale; };
    auto Y = [&](double y) { return oy + y * scale; };

    cairo_save(cr);
    cairo_translate(cr, X((agent.bounds[0] + agent.bounds[2]) * 0.5), Y(agent.bounds[3]));
    cairo_scale(cr, 1.0, 0.18);
    cairo_arc(cr, 0, 0, (agent.bounds[2] - agent.bounds[0]) * scale * 0.42, 0, 2 * M_PI);
    cairo_restore(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
    cairo_fill(cr);

    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, scale, scale);
    if (settings::Enabled(g_cfg->glow)) {
        PreviewColor glow = Unpack(g_cfg->glow_enemy_rgba);
        for (double radius : {9.0, 6.0, 3.0})
            for (int step = 0; step < 12; step++) {
                double angle = step * M_PI / 6;
                cairo_save(cr);
                cairo_translate(cr, std::cos(angle) * radius, std::sin(angle) * radius);
                PaintTinted(cr, agent.shade, glow, 0.07);
                cairo_restore(cr);
            }
    }
    if (settings::Enabled(g_cfg->chams)) {
        PreviewColor chams = Unpack(g_cfg->chams_visible_rgba);
        cairo_push_group(cr);
        cairo_set_source_surface(cr, agent.shade, 0, 0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_MULTIPLY);
        PaintTinted(cr, agent.shade, chams, 1.0);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, std::max(0.55, chams.a));
    } else {
        cairo_set_source_surface(cr, agent.color, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
    }
    cairo_restore(cr);

    if (!settings::Enabled(g_cfg->esp)) return FALSE;
    double left = X(agent.bounds[0]) - 4, right = X(agent.bounds[2]) + 4;
    double top = Y(agent.bounds[1]) - 4, bottom = Y(agent.bounds[3]) + 2;
    if (settings::Enabled(g_cfg->esp_skeleton)) {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.92);
        cairo_set_line_width(cr, 1.5);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        for (const auto& bone : agent.bones) {
            cairo_move_to(cr, X(bone[0]), Y(bone[1]));
            cairo_line_to(cr, X(bone[2]), Y(bone[3]));
        }
        cairo_stroke(cr);
    }
    if (settings::Enabled(g_cfg->esp_head_circle)) {
        cairo_arc(cr, X(agent.head[0]), Y(agent.head[1]), 0.07 * (bottom - top) * 0.5, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.92);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
    }
    if (settings::Enabled(g_cfg->esp_box)) {
        for (int pass = 0; pass < 2; pass++) {
            cairo_rectangle(cr, std::round(left) + 0.5, std::round(top) + 0.5, std::round(right - left), std::round(bottom - top));
            if (pass == 0) { cairo_set_source_rgba(cr, 0, 0, 0, 0.75); cairo_set_line_width(cr, 3.0); }
            else { cairo_set_source_rgba(cr, 1, 1, 1, 0.95); cairo_set_line_width(cr, 1.0); }
            cairo_stroke(cr);
        }
    }
    if (settings::Enabled(g_cfg->esp_health)) {
        double bar_x = std::round(left) - 6, bar_h = bottom - top, fill = bar_h * 0.76;
        cairo_rectangle(cr, bar_x - 1, top - 1, 5, bar_h + 2);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.75);
        cairo_fill(cr);
        cairo_pattern_t* hp = cairo_pattern_create_linear(0, bottom - fill, 0, bottom);
        cairo_pattern_add_color_stop_rgb(hp, 0, 0.45, 0.95, 0.4);
        cairo_pattern_add_color_stop_rgb(hp, 1, 0.12, 0.62, 0.3);
        cairo_set_source(cr, hp);
        cairo_rectangle(cr, bar_x, bottom - fill, 3, fill);
        cairo_fill(cr);
        cairo_pattern_destroy(hp);
        PreviewText(cr, "76", bar_x + 1.5, bottom - fill + 3.0, 9.0, true, {1, 1, 1, 1});
    }
    if (settings::Enabled(g_cfg->esp_name)) PreviewText(cr, "Enemy", (left + right) * 0.5, top - 6, 12, true, {1, 1, 1, 1});
    double below = bottom;
    bool ammo = settings::Enabled(g_cfg->esp_ammo);
    if (ammo) {
        cairo_rectangle(cr, left - 1, bottom + 3, right - left + 2, 5);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.75);
        cairo_fill(cr);
        cairo_rectangle(cr, left, bottom + 4, (right - left) * 22.0 / 30.0, 3);
        cairo_set_source_rgba(cr, 0.3, 0.55, 1.0, 1);
        cairo_fill(cr);
        below = bottom + 8;
    }
    if (settings::Enabled(g_cfg->esp_weapon))
        PreviewText(cr, ammo ? "AK-47  22/30" : "AK-47", (left + right) * 0.5, below + 12, 10, false, {0.85, 0.87, 0.9, 1});
    if (settings::Enabled(g_cfg->esp_flags))
        icons::DrawFlagColumn(cr, kFlagHelmet | kFlagBomb | kFlagFlashed | kFlagScoped, right + 5, top, 16, 1.0);
    return FALSE;
}

static GtkWidget* MakeEspPreview() {
    GtkWidget* card = MakeCard("PREVIEW");
    GtkWidget* area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, -1, 400);
    gtk_style_context_add_class(gtk_widget_get_style_context(area), "preview");
    g_signal_connect(area, "draw", G_CALLBACK(DrawEspPreview), nullptr);
    g_timeout_add(50, +[](gpointer widget) -> gboolean {
        if (gtk_widget_get_mapped(GTK_WIDGET(widget))) gtk_widget_queue_draw(GTK_WIDGET(widget));
        return G_SOURCE_CONTINUE;
    }, area);
    gtk_box_pack_start(GTK_BOX(card), area, FALSE, FALSE, 0);
    return card;
}

static GtkCssProvider* s_css_provider = nullptr;

static void InstallCss() {
    if (!s_css_provider) {
        s_css_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(s_css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    uint32_t rgba = g_cfg ? g_cfg->hud_accent_rgba : 0;
    if (rgba == 0 && g_cfg) {
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
    if (rgba == 0) rgba = 0x4C8DFFFF;
    int r = (rgba >> 24) & 0xFF;
    int g = (rgba >> 16) & 0xFF;
    int b = (rgba >>  8) & 0xFF;
    char hex[8], hex_hover[8], hex_dark[8], rgba_16[32], rgba_02[32], rgba_12[32], rgba_28[32], rgba_24[32];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", r, g, b);
    snprintf(hex_hover, sizeof(hex_hover), "#%02x%02x%02x", std::min(255, r + 25), std::min(255, g + 25), std::min(255, b + 25));
    snprintf(hex_dark, sizeof(hex_dark), "#%02x%02x%02x", std::max(0, r - 30), std::max(0, g - 30), std::max(0, b - 30));
    snprintf(rgba_16, sizeof(rgba_16), "rgba(%d,%d,%d,0.16)", r, g, b);
    snprintf(rgba_02, sizeof(rgba_02), "rgba(%d,%d,%d,0.02)", r, g, b);
    snprintf(rgba_12, sizeof(rgba_12), "rgba(%d,%d,%d,0.12)", r, g, b);
    snprintf(rgba_28, sizeof(rgba_28), "rgba(%d,%d,%d,0.28)", r, g, b);
    snprintf(rgba_24, sizeof(rgba_24), "rgba(%d,%d,%d,0.24)", r, g, b);

    std::string css =
        "* { font-family: 'Inter', 'SF Pro Text', 'Segoe UI', sans-serif; font-size: 12px; }"
        "window { background: transparent; }"
        "#root { background: #0b0c11; border-radius: 10px; border: 1px solid #1d2029; }"
        "#titlebar { background: #0e0f15; border-bottom: 1px solid #1a1c25; border-radius: 10px 10px 0 0; padding: 10px 12px 10px 18px; }"
        ".logo { font-size: 15px; font-weight: 800; letter-spacing: 2px; color: " + std::string(hex) + "; }"
        "#search { background: #0b0c11; color: #e4e6ed; border: 1px solid #1d2029; border-radius: 6px; min-height: 26px; padding: 0 8px; margin-left: 16px; }"
        "#search:focus { border-color: " + std::string(hex) + "; }"
        ".close-button { background: transparent; color: #5c6070; border: none; box-shadow: none; padding: 2px 8px; font-size: 13px; }"
        ".close-button:hover { color: #ffffff; background: #1a1d27; }"
        "#sidebar { background: #0e0f15; border-right: 1px solid #1a1c25; padding: 8px 10px 10px 10px; border-radius: 0 0 0 10px; }"
        ".sidebar-header { color: #4a4e5c; font-size: 10px; font-weight: 800; letter-spacing: 1.4px; margin: 14px 10px 4px 10px; }"
        ".sidebar-item { background: transparent; color: #9a9eab; border: none; border-radius: 6px; padding: 7px 10px; margin: 1px 0; box-shadow: none; font-size: 13px; font-weight: 600; }"
        ".sidebar-item:hover { background: #141620; color: #e4e6ed; }"
        ".sidebar-item.active { background: linear-gradient(90deg, " + std::string(rgba_16) + ", " + std::string(rgba_02) + "); color: #ffffff; box-shadow: inset 2px 0 0 " + std::string(hex) + "; }"
        ".sidebar-icon { min-width: 18px; margin-right: 2px; }"
        ".configs { padding: 10px; margin: 8px 0; background: #11131a; border-radius: 8px; border: 1px solid #1c1f29; }"
        ".configs-title { color: #4a4e5c; font-size: 10px; font-weight: 800; letter-spacing: 1.4px; margin-bottom: 4px; }"
        ".configs entry { min-height: 26px; padding: 4px 10px; font-size: 11px; background: #0b0c11; color: #e4e6ed; border-radius: 6px; border: 1px solid #1d2029; }"
        ".configs button { min-height: 26px; padding: 4px 10px; font-size: 11px; }"
        ".configs combobox box { background: #0b0c11; border-radius: 6px; border: 1px solid #1d2029; }"
        ".configs combobox button { min-height: 26px; padding: 4px 10px; background: transparent; border: none; box-shadow: none; }"
        "combobox window menu { background: #11131a; border-radius: 6px; padding: 4px; border: 1px solid #1d2029; }"
        "combobox window menu menuitem { color: #e4e6ed; border-radius: 4px; padding: 6px 10px; }"
        "combobox window menu menuitem:hover { background: " + std::string(hex) + "; color: white; }"
        ".profile { padding: 8px 10px; margin: 4px 0 0 0; background: #11131a; border-radius: 8px; border: 1px solid #1c1f29; }"
        ".profile .avatar { border-radius: 16px; }"
        ".profile-name { color: #ffffff; font-weight: 700; font-size: 12px; }"
        ".profile-sub  { color: " + std::string(hex) + "; font-size: 10px; font-weight: 600; }"
        "stack { background: transparent; }"
        ".page { padding: 14px 16px 18px 16px; }"
        ".card { background: #11131a; border-radius: 8px; border: 1px solid #1c1f29; }"
        ".loader-status { color: #7a7f8e; font-weight: 700; }"
        ".loader-status.loader-on { color: " + std::string(hex) + "; }"
        "button.suggested-action { background: " + std::string(hex) + "; color: #ffffff; border-color: " + std::string(hex) + "; }"
        "button.suggested-action:hover { background: " + std::string(hex_hover) + "; }"
        ".dim-label { color: #7a7f8e; padding: 6px 14px; }"
        "entry { background: #151823; border: 1px solid #262a3a; color: #e4e6ed; border-radius: 6px; padding: 4px 8px; min-height: 22px; }"
        "entry:focus { border-color: " + std::string(hex) + "; }"
        "popover { background: #11131a; border: 1px solid #1c1f29; border-radius: 8px; }"
        "popover check { min-width: 14px; min-height: 14px; }"
        ".card > button { margin: 4px 14px; }"
        ".card-title { color: #e4e6ed; font-size: 11px; font-weight: 800; letter-spacing: 1px; padding: 10px 14px 9px 14px; border-bottom: 1px solid #1a1c25; margin-bottom: 4px; }"
        ".preview { margin: 6px 10px 10px 10px; border-radius: 6px; }"
        ".row { padding: 6px 14px; }"
        ".row:hover { background: #141620; }"
        ".row label { color: #c9ccd6; font-size: 12px; }"
        "checkbutton { padding: 0; min-height: 0; }"
        "checkbutton check { min-width: 14px; min-height: 14px; margin: 0; border-radius: 4px; background: #1d2030; border: 1px solid #2c3142; color: transparent; }"
        "checkbutton check:checked { background: " + std::string(hex) + "; border-color: " + std::string(hex) + "; color: #ffffff; -gtk-icon-source: -gtk-icontheme('object-select-symbolic'); }"
        "scale:disabled { opacity: 0.4; }"
        "switch { min-width: 34px; min-height: 18px; background: #1d2030; border-radius: 9px; border: 1px solid #262a3a; padding: 0; }"
        "switch:checked { background: " + std::string(hex) + "; border-color: " + std::string(hex) + "; }"
        "switch slider { background: #8a8fa0; border-radius: 50%; min-width: 14px; min-height: 14px; margin: 2px; border: none; box-shadow: none; }"
        "switch:checked slider { background: #ffffff; }"
        "scale { padding: 6px 0; }"
        "scale trough { background: #1d2030; border-radius: 2px; min-height: 4px; border: none; }"
        "scale highlight { background: linear-gradient(90deg, " + std::string(hex_dark) + ", " + std::string(hex) + "); border-radius: 2px; }"
        "scale slider { background: #ffffff; border-radius: 50%; min-width: 12px; min-height: 12px; margin: -5px; border: 2px solid " + std::string(hex) + "; box-shadow: none; }"
        "scale value { color: " + std::string(hex) + "; font-size: 11px; font-weight: 700; margin-left: 6px; }"
        "button { background: #171a24; color: #e4e6ed; border: 1px solid #232736; border-radius: 6px; padding: 4px 10px; font-weight: 600; font-size: 11px; box-shadow: none; }"
        "button:hover { background: #1d2130; border-color: #2d3346; }"
        "button:active { background: " + std::string(hex) + "; border-color: " + std::string(hex) + "; }"
        ".bind-badge { background: " + std::string(rgba_12) + "; color: #7fa7ff; border: 1px solid " + std::string(rgba_28) + "; border-radius: 4px; padding: 2px 7px; font-family: monospace; font-size: 10px; font-weight: 700; margin-right: 4px; }"
        ".bind-badge:hover { background: " + std::string(rgba_24) + "; color: #ffffff; }"
        ".bind-badge.capturing { background: rgba(255, 159, 10, 0.22); color: #ff9f0a; border-color: #ff9f0a; }"
        "menu { background: #11131a; border-radius: 6px; padding: 4px; border: 1px solid #1d2029; }"
        "menu menuitem { color: #e4e6ed; border-radius: 4px; padding: 5px 12px; font-size: 11px; }"
        "menu menuitem:hover { background: " + std::string(hex) + "; color: white; }"
        ".color-btn { padding: 2px; border-radius: 5px; background: #171a24; }"
        "combobox button { background: #171a24; border: 1px solid #232736; }"
        "scrollbar { background: transparent; }"
        "scrollbar slider { background: #232736; border-radius: 3px; min-width: 5px; min-height: 30px; }"
        "scrollbar slider:hover { background: " + std::string(hex) + "; }";

    gtk_css_provider_load_from_data(s_css_provider, css.c_str(), -1, nullptr);
}

static const char* CardTitle(GtkWidget* card) {
    GList* children = gtk_container_get_children(GTK_CONTAINER(card));
    const char* title = children && GTK_IS_LABEL(children->data) ? gtk_label_get_text(GTK_LABEL(children->data)) : "";
    g_list_free(children);
    return title;
}

struct CardHome { GtkWidget* card; GtkWidget* parent; int position; };
static std::vector<CardHome> g_card_homes;
static GtkWidget* g_search_page = nullptr;
static std::string g_page_before_search;

static void MoveCard(GtkWidget* card, GtkWidget* target) {
    g_object_ref(card);
    gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(card)), card);
    gtk_box_pack_start(GTK_BOX(target), card, FALSE, FALSE, 0);
    g_object_unref(card);
}

static void RestoreCards() {
    std::vector<CardHome> homes = g_card_homes;
    std::sort(homes.begin(), homes.end(), [](const CardHome& l, const CardHome& r) { return l.position < r.position; });
    for (const CardHome& home : homes) {
        if (gtk_widget_get_parent(home.card) != home.parent) MoveCard(home.card, home.parent);
        gtk_box_reorder_child(GTK_BOX(home.parent), home.card, home.position);
    }
    g_card_homes.clear();
}

static void ApplySearch(const std::string& raw) {
    std::string query = Lowercase(raw);
    bool searching = !query.empty();
    const char* visible = gtk_stack_get_visible_child_name(GTK_STACK(g_stack));
    if (searching && g_card_homes.empty() && visible && std::string(visible) != "search") g_page_before_search = visible;
    RestoreCards();
    for (const SearchRow& entry : g_search_rows) gtk_widget_set_visible(entry.row, TRUE);
    if (!searching) {
        if (!g_page_before_search.empty()) gtk_stack_set_visible_child_name(GTK_STACK(g_stack), g_page_before_search.c_str());
        return;
    }
    std::vector<GtkWidget*> cards;
    for (const SearchRow& entry : g_search_rows) {
        GtkWidget* card = gtk_widget_get_parent(entry.row);
        if (std::find(cards.begin(), cards.end(), card) == cards.end()) cards.push_back(card);
    }
    GtkWidget* columns[2] = {GTK_WIDGET(g_object_get_data(G_OBJECT(g_search_page), "left")),
                             GTK_WIDGET(g_object_get_data(G_OBJECT(g_search_page), "right"))};
    int placed = 0;
    for (GtkWidget* card : cards) {
        bool card_match = Lowercase(CardTitle(card)).find(query) != std::string::npos;
        bool any = false;
        for (const SearchRow& entry : g_search_rows) {
            if (gtk_widget_get_parent(entry.row) != card) continue;
            bool show = card_match || entry.text.find(query) != std::string::npos;
            gtk_widget_set_visible(entry.row, show);
            any = any || show;
        }
        if (!any) continue;
        GtkWidget* parent = gtk_widget_get_parent(card);
        int position = 0;
        gtk_container_child_get(GTK_CONTAINER(parent), card, "position", &position, nullptr);
        g_card_homes.push_back({card, parent, position});
        MoveCard(card, columns[placed++ % 2]);
    }
    gtk_widget_show_all(g_search_page);
    for (const SearchRow& entry : g_search_rows) {
        GtkWidget* card = gtk_widget_get_parent(entry.row);
        bool in_results = std::any_of(g_card_homes.begin(), g_card_homes.end(), [&](const CardHome& h) { return h.card == card; });
        if (!in_results) continue;
        bool card_match = Lowercase(CardTitle(card)).find(query) != std::string::npos;
        gtk_widget_set_visible(entry.row, card_match || entry.text.find(query) != std::string::npos);
    }
    gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "search");
}

static GtkWidget* Titlebar() {
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_name(bar, "titlebar");
    GtkWidget* logo = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(logo), "<span foreground=\"#4c8dff\">SPAXER</span><span foreground=\"#5c6070\">.CS2</span>");
    gtk_style_context_add_class(gtk_widget_get_style_context(logo), "logo");
    gtk_widget_set_size_request(logo, 230, -1);
    gtk_label_set_xalign(GTK_LABEL(logo), 0.f);
    GtkWidget* spacer = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(spacer), "Search settings...");
    gtk_widget_set_name(spacer, "search");
    g_search_entry = spacer;
    gtk_widget_set_size_request(spacer, 320, -1);
    gtk_widget_set_halign(spacer, GTK_ALIGN_START);
    g_signal_connect(spacer, "search-changed", G_CALLBACK(+[](GtkSearchEntry* entry, gpointer) {
        ApplySearch(gtk_entry_get_text(GTK_ENTRY(entry)));
    }), nullptr);
    GtkWidget* close = gtk_button_new_with_label("\u2715");
    gtk_style_context_add_class(gtk_widget_get_style_context(close), "close-button");
    g_signal_connect(close, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { gtk_widget_hide(g_win); }), nullptr);
    gtk_box_pack_start(GTK_BOX(bar), logo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), spacer, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(bar), close, FALSE, FALSE, 0);
    return bar;
}

static std::string LineIconSvg(const char* glyph) {
    return std::string("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\"><g fill=\"none\" stroke=\"#4c8dff\" "
                       "stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\">") + glyph + "</g></svg>";
}

static const char* kGlyphCrosshair = "<circle cx=\"12\" cy=\"12\" r=\"8\"/><line x1=\"12\" y1=\"1.5\" x2=\"12\" y2=\"6\"/><line x1=\"12\" y1=\"18\" x2=\"12\" y2=\"22.5\"/><line x1=\"1.5\" y1=\"12\" x2=\"6\" y2=\"12\"/><line x1=\"18\" y1=\"12\" x2=\"22.5\" y2=\"12\"/><circle cx=\"12\" cy=\"12\" r=\"1.2\"/>";
static const char* kGlyphRage = "<circle cx=\"12\" cy=\"12\" r=\"9\"/><path d=\"M8 15h8\"/><path d=\"M9 9h.01\"/><path d=\"M15 9h.01\"/><path d=\"M10 13l2-2 2 2\"/>";
static const char* kGlyphMovement = "<polyline points=\"5 9 2 12 5 15\"/><polyline points=\"9 5 12 2 15 5\"/><polyline points=\"15 19 12 22 9 19\"/><polyline points=\"19 9 22 12 19 15\"/><line x1=\"2\" y1=\"12\" x2=\"22\" y2=\"12\"/><line x1=\"12\" y1=\"2\" x2=\"12\" y2=\"22\"/>";
static const char* kGlyphEsp = "<circle cx=\"12\" cy=\"7\" r=\"4\"/><path d=\"M4 21v-1a6 6 0 0 1 6-6h4a6 6 0 0 1 6 6v1\"/><rect x=\"2\" y=\"1.5\" width=\"20\" height=\"21\" rx=\"2\" stroke-dasharray=\"3 3\"/>";
static const char* kGlyphHud = "<rect x=\"3\" y=\"3\" width=\"18\" height=\"18\" rx=\"2\"/><line x1=\"3\" y1=\"9\" x2=\"21\" y2=\"9\"/><line x1=\"9\" y1=\"21\" x2=\"9\" y2=\"9\"/>";
static const char* kGlyphWorld = "<circle cx=\"12\" cy=\"12\" r=\"10\"/><line x1=\"2\" y1=\"12\" x2=\"22\" y2=\"12\"/><path d=\"M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z\"/>";
static const char* kGlyphMisc = "<line x1=\"4\" y1=\"21\" x2=\"4\" y2=\"14\"/><line x1=\"4\" y1=\"10\" x2=\"4\" y2=\"3\"/><line x1=\"12\" y1=\"21\" x2=\"12\" y2=\"12\"/><line x1=\"12\" y1=\"8\" x2=\"12\" y2=\"3\"/><line x1=\"20\" y1=\"21\" x2=\"20\" y2=\"16\"/><line x1=\"20\" y1=\"12\" x2=\"20\" y2=\"3\"/><line x1=\"1\" y1=\"14\" x2=\"7\" y2=\"14\"/><line x1=\"9\" y1=\"8\" x2=\"15\" y2=\"8\"/><line x1=\"17\" y1=\"16\" x2=\"23\" y2=\"16\"/>";
static const char* kGlyphScripts = "<polyline points=\"16 18 22 12 16 6\"/><polyline points=\"8 6 2 12 8 18\"/><line x1=\"14\" y1=\"4\" x2=\"10\" y2=\"20\"/>";
static const char* kGlyphFile = "<path d=\"M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z\"/><polyline points=\"14 2 14 8 20 8\"/><line x1=\"8\" y1=\"13\" x2=\"16\" y2=\"13\"/><line x1=\"8\" y1=\"17\" x2=\"13\" y2=\"17\"/>";

static const std::string s_svg_legit = LineIconSvg(kGlyphCrosshair);
static const std::string s_svg_rage = LineIconSvg(kGlyphRage);
static const std::string s_svg_movement = LineIconSvg(kGlyphMovement);
static const std::string s_svg_esp = LineIconSvg(kGlyphEsp);
static const std::string s_svg_hud = LineIconSvg(kGlyphHud);
static const std::string s_svg_world = LineIconSvg(kGlyphWorld);
static const std::string s_svg_misc = LineIconSvg(kGlyphMisc);
static const std::string s_svg_scripts = LineIconSvg(kGlyphScripts);
static const std::string s_svg_lua_file = LineIconSvg(kGlyphFile);

static std::string ScriptsDir() {
    const char* home = getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.config/spaxer/scripts";
}

static void OpenPath(const std::string& path) {
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    if (!uri) return;
    g_app_info_launch_default_for_uri(uri, nullptr, nullptr);
    g_free(uri);
}

static void RequestScriptReload() {
    __atomic_fetch_add(&g_cfg->lua_reload_token, 1u, __ATOMIC_RELAXED);
}

struct ScriptEntry {
    std::string path;
    bool enabled;
};

static std::vector<ScriptEntry> ListScripts() {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::create_directories(ScriptsDir(), error);
    std::vector<ScriptEntry> scripts;
    for (const auto& entry : fs::directory_iterator(ScriptsDir(), error)) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        if (entry.path().extension() == ".lua") scripts.push_back({entry.path().string(), true});
        else if (name.size() > 13 && name.compare(name.size() - 13, 13, ".lua.disabled") == 0)
            scripts.push_back({entry.path().string(), false});
    }
    std::sort(scripts.begin(), scripts.end(), [](const ScriptEntry& a, const ScriptEntry& b) { return a.path < b.path; });
    return scripts;
}

static std::string ScriptDisplayName(const ScriptEntry& script) {
    std::string name = std::filesystem::path(script.path).filename().string();
    if (!script.enabled) name.resize(name.size() - 9);
    return name;
}

static void PopulateScriptsList(GtkBox* target);

static void OnScriptToggled(GtkSwitch* toggle, gboolean state, gpointer data) {
    const std::string& path = *static_cast<std::string*>(data);
    std::string renamed = state ? path.substr(0, path.size() - 9) : path + ".disabled";
    std::error_code error;
    std::filesystem::rename(path, renamed, error);
    if (error) return;
    RequestScriptReload();
    gpointer list = g_object_get_data(G_OBJECT(toggle), "scripts-list");
    if (list) g_idle_add(+[](gpointer box) -> gboolean { PopulateScriptsList(GTK_BOX(box)); return G_SOURCE_REMOVE; }, list);
}

static void PopulateScriptsList(GtkBox* target) {
    GList* children = gtk_container_get_children(GTK_CONTAINER(target));
    for (GList* iter = children; iter != nullptr; iter = g_list_next(iter)) gtk_widget_destroy(GTK_WIDGET(iter->data));
    g_list_free(children);

    std::vector<ScriptEntry> scripts = ListScripts();
    for (const ScriptEntry& script : scripts) {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_style_context_add_class(gtk_widget_get_style_context(row), "row");
        GtkWidget* icon = MakeSvgWidget(s_svg_lua_file.c_str(), 20);
        GtkWidget* label = gtk_label_new(ScriptDisplayName(script).c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.f);
        gtk_widget_set_hexpand(label, TRUE);

        GtkWidget* edit = gtk_button_new_with_label("Edit");
        gtk_widget_set_valign(edit, GTK_ALIGN_CENTER);
        std::string* edit_path = new std::string(script.path);
        g_signal_connect(edit, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { OpenPath(*static_cast<std::string*>(p)); }), edit_path);
        g_signal_connect(edit, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer p) { delete static_cast<std::string*>(p); }), edit_path);

        GtkWidget* toggle = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(toggle), script.enabled);
        g_object_set_data(G_OBJECT(toggle), "scripts-list", target);
        gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
        std::string* toggle_path = new std::string(script.path);
        g_signal_connect(toggle, "state-set", G_CALLBACK(+[](GtkSwitch* sw, gboolean state, gpointer p) -> gboolean {
            OnScriptToggled(sw, state, p);
            return FALSE;
        }), toggle_path);
        g_signal_connect(toggle, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer p) { delete static_cast<std::string*>(p); }), toggle_path);

        gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(row), edit, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), toggle, FALSE, FALSE, 0);
        gtk_box_pack_start(target, row, FALSE, FALSE, 0);
    }
    if (scripts.empty()) {
        std::string text = "No scripts yet. Put .lua files into " + ScriptsDir();
        GtkWidget* empty = gtk_label_new(text.c_str());
        gtk_label_set_xalign(GTK_LABEL(empty), 0.f);
        gtk_label_set_line_wrap(GTK_LABEL(empty), TRUE);
        gtk_box_pack_start(target, empty, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(GTK_WIDGET(target));
}

extern char** environ;

static std::string SpaxerConfigPath(const char* file) {
    const char* home = getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.config/spaxer/" + file;
}

struct LuaUiElement {
    std::string script;
    std::string kind;
    std::string name;
    bool visible = true;
    std::vector<std::string> args;
};

static std::vector<std::string> SplitString(const std::string& text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t end = text.find(separator, start);
        parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return parts;
}

static std::vector<LuaUiElement> ReadLuaSchema() {
    std::vector<LuaUiElement> elements;
    std::ifstream file(SpaxerConfigPath("lua_ui.txt"));
    std::string line;
    while (std::getline(file, line)) {
        std::vector<std::string> fields = SplitString(line, '\t');
        if (fields.size() < 4) continue;
        LuaUiElement element{fields[0], fields[1], fields[2], fields[3] == "1", {}};
        element.args.assign(fields.begin() + 4, fields.end());
        elements.push_back(std::move(element));
    }
    return elements;
}

static std::map<std::string, std::string> ReadLuaValues() {
    std::map<std::string, std::string> values;
    std::ifstream file(SpaxerConfigPath("lua_values.txt"));
    std::string line;
    while (std::getline(file, line)) {
        size_t first = line.find('\t');
        size_t second = first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
        if (second == std::string::npos) continue;
        values[line.substr(0, second)] = line.substr(second + 1);
    }
    return values;
}

static void WriteLuaValue(const std::string& key, const std::string& value) {
    std::map<std::string, std::string> values = ReadLuaValues();
    values[key] = value;
    std::string path = SpaxerConfigPath("lua_values.txt");
    std::string tmp = path + ".gui";
    {
        std::ofstream out(tmp, std::ios::trunc);
        for (const auto& [k, v] : values) out << k << '\t' << v << '\n';
    }
    std::rename(tmp.c_str(), path.c_str());
}

struct LuaWidget {
    std::string key;
    std::string kind;
    GtkWidget* widget;
    std::vector<std::string> items;
    std::vector<GtkWidget*> checks;
};

static std::vector<LuaWidget> g_lua_widgets;
static GtkWidget* g_lua_panel = nullptr;
static time_t g_lua_schema_mtime = 0;
static time_t g_lua_values_mtime = 0;
static bool g_lua_updating = false;

static time_t FileMtime(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? st.st_mtime : 0;
}

static std::string* LuaKey(GtkWidget* widget, const std::string& key) {
    std::string* stored = new std::string(key);
    g_signal_connect(widget, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer p) { delete static_cast<std::string*>(p); }), stored);
    return stored;
}

static GtkWidget* LuaRow(const std::string& label) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "row");
    GtkWidget* lbl = gtk_label_new(label.c_str());
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.f);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);
    return row;
}

static std::string LuaKeyName(guint keyval) {
    const char* name = gdk_keyval_name(keyval);
    std::string upper = name ? name : "";
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
    return upper;
}

static std::string LuaBindLabel(const std::string& value) {
    size_t bar = value.find('|');
    std::string key = value.substr(0, bar);
    std::string mode = bar == std::string::npos ? "hold" : value.substr(bar + 1);
    if (mode == "always") return "always";
    return (key.empty() ? std::string("bind") : key) + (mode == "toggle" ? " · toggle" : "");
}

static void FinishLuaBind(GtkWidget* button, const std::string& key_name) {
    std::string* key = static_cast<std::string*>(g_object_get_data(G_OBJECT(button), "lua-key"));
    std::string current = ReadLuaValues()[*key];
    size_t bar = current.find('|');
    std::string mode = bar == std::string::npos ? "hold" : current.substr(bar + 1);
    std::string value = key_name + "|" + mode;
    WriteLuaValue(*key, value);
    gtk_button_set_label(GTK_BUTTON(button), LuaBindLabel(value).c_str());
    g_capturing_bind = false;
}

static gboolean OnLuaBindKey(GtkWidget* top, GdkEventKey* event, gpointer button) {
    guint kv = event->keyval, base = 0;
    if (gdk_keymap_translate_keyboard_state(gdk_keymap_get_for_display(gdk_display_get_default()), event->hardware_keycode,
                                            GdkModifierType(0), 0, &base, nullptr, nullptr, nullptr) && base)
        kv = gdk_keyval_to_lower(base);
    g_signal_handlers_disconnect_matched(top, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, button);
    FinishLuaBind(GTK_WIDGET(button), kv == GDK_KEY_Escape ? "" : LuaKeyName(kv));
    return TRUE;
}

static gboolean OnLuaBindMouse(GtkWidget* top, GdkEventButton* event, gpointer button) {
    static const std::pair<guint, const char*> kButtons[] = {{2, "MOUSE2"}, {3, "MOUSE3"}, {8, "MOUSE4"}, {9, "MOUSE5"}};
    for (const auto& [gtk_button, name] : kButtons) {
        if (event->button != gtk_button) continue;
        g_signal_handlers_disconnect_matched(top, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, button);
        FinishLuaBind(GTK_WIDGET(button), name);
        return TRUE;
    }
    return FALSE;
}

static GtkWidget* MakeLuaBindButton(const std::string& key) {
    GtkWidget* button = gtk_button_new_with_label(LuaBindLabel(ReadLuaValues()[key]).c_str());
    gtk_style_context_add_class(gtk_widget_get_style_context(button), "bind");
    gtk_widget_set_size_request(button, 78, 26);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(button), "lua-key", LuaKey(button, key));
    g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer) {
        gtk_button_set_label(b, "…");
        g_capturing_bind = true;
        GtkWidget* top = gtk_widget_get_toplevel(GTK_WIDGET(b));
        g_signal_connect(top, "key-press-event", G_CALLBACK(OnLuaBindKey), b);
        g_signal_connect(top, "button-press-event", G_CALLBACK(OnLuaBindMouse), b);
    }), nullptr);
    g_signal_connect(button, "button-press-event", G_CALLBACK(+[](GtkWidget* b, GdkEventButton* event, gpointer) -> gboolean {
        if (event->button != 3 || g_capturing_bind) return FALSE;
        GtkWidget* menu = gtk_menu_new();
        for (const char* mode : {"hold", "toggle", "always"}) {
            GtkWidget* item = gtk_menu_item_new_with_label(mode);
            g_object_set_data(G_OBJECT(item), "lua-bind-button", b);
            g_signal_connect(item, "activate", G_CALLBACK(+[](GtkMenuItem* menu_item, gpointer) {
                GtkWidget* bind_button = GTK_WIDGET(g_object_get_data(G_OBJECT(menu_item), "lua-bind-button"));
                std::string* key = static_cast<std::string*>(g_object_get_data(G_OBJECT(bind_button), "lua-key"));
                std::string current = ReadLuaValues()[*key];
                std::string value = current.substr(0, current.find('|')) + "|" + gtk_menu_item_get_label(menu_item);
                WriteLuaValue(*key, value);
                gtk_button_set_label(GTK_BUTTON(bind_button), LuaBindLabel(value).c_str());
            }), nullptr);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        }
        gtk_widget_show_all(menu);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), reinterpret_cast<GdkEvent*>(event));
        return TRUE;
    }), nullptr);
    return button;
}

static std::string MultiLabel(const std::vector<std::string>& items, const std::string& value) {
    std::string label;
    for (const std::string& index : SplitString(value, ',')) {
        if (index.empty()) continue;
        size_t i = static_cast<size_t>(std::atoi(index.c_str()));
        if (i >= items.size()) continue;
        if (!label.empty()) label += ", ";
        label += items[i];
    }
    return label.empty() ? "none" : label;
}

static void RefreshLuaValues() {
    std::map<std::string, std::string> values = ReadLuaValues();
    g_lua_updating = true;
    for (LuaWidget& w : g_lua_widgets) {
        auto it = values.find(w.key);
        if (it == values.end()) continue;
        const std::string& value = it->second;
        if (w.kind == "checkbox") gtk_switch_set_active(GTK_SWITCH(w.widget), value == "1");
        else if (w.kind == "slider") gtk_range_set_value(GTK_RANGE(w.widget), std::atof(value.c_str()));
        else if (w.kind == "combo") gtk_combo_box_set_active(GTK_COMBO_BOX(w.widget), std::atoi(value.c_str()));
        else if (w.kind == "color") {
            int r = 255, g = 255, b = 255, a = 255;
            sscanf(value.c_str(), "%d,%d,%d,%d", &r, &g, &b, &a);
            GdkRGBA rgba{r / 255.0, g / 255.0, b / 255.0, a / 255.0};
            gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(w.widget), &rgba);
        } else if (w.kind == "bind" && !g_capturing_bind) gtk_button_set_label(GTK_BUTTON(w.widget), LuaBindLabel(value).c_str());
        else if (w.kind == "input" && !gtk_widget_has_focus(w.widget) && value != gtk_entry_get_text(GTK_ENTRY(w.widget)))
            gtk_entry_set_text(GTK_ENTRY(w.widget), value.c_str());
        else if (w.kind == "multi") {
            std::vector<std::string> selected = SplitString(value, ',');
            for (size_t i = 0; i < w.checks.size(); i++)
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w.checks[i]),
                                             std::find(selected.begin(), selected.end(), std::to_string(i)) != selected.end());
            gtk_button_set_label(GTK_BUTTON(w.widget), MultiLabel(w.items, value).c_str());
        }
    }
    g_lua_updating = false;
}

static GtkWidget* BuildLuaElement(const LuaUiElement& element, const std::string& key) {
    const auto& a = element.args;
    if (element.kind == "label") {
        GtkWidget* label = gtk_label_new(element.name.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.f);
        gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(label), "dim-label");
        return label;
    }
    if (element.kind == "button") {
        GtkWidget* button = gtk_button_new_with_label(element.name.c_str());
        g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
            const std::string& k = *static_cast<std::string*>(p);
            WriteLuaValue(k, std::to_string(std::atol(ReadLuaValues()[k].c_str()) + 1));
        }), LuaKey(button, key));
        return button;
    }
    GtkWidget* row = LuaRow(element.name);
    GtkWidget* control = nullptr;
    LuaWidget record{key, element.kind, nullptr, {}, {}};
    if (element.kind == "checkbox") {
        control = gtk_switch_new();
        g_signal_connect(control, "state-set", G_CALLBACK(+[](GtkSwitch*, gboolean state, gpointer p) -> gboolean {
            if (!g_lua_updating) WriteLuaValue(*static_cast<std::string*>(p), state ? "1" : "0");
            return FALSE;
        }), LuaKey(control, key));
    } else if (element.kind == "slider" && a.size() >= 3) {
        double min = std::atof(a[0].c_str()), max = std::atof(a[1].c_str()), step = std::max(1e-6, std::atof(a[2].c_str()));
        control = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, step);
        gtk_scale_set_digits(GTK_SCALE(control), step >= 1 ? 0 : (step >= 0.1 ? 1 : 2));
        gtk_scale_set_value_pos(GTK_SCALE(control), GTK_POS_RIGHT);
        gtk_widget_set_size_request(control, 170, -1);
        if (a.size() >= 4 && !a[3].empty()) {
            g_object_set_data_full(G_OBJECT(control), "suffix", g_strdup(a[3].c_str()), g_free);
            g_signal_connect(control, "format-value", G_CALLBACK(+[](GtkScale* scale, gdouble value, gpointer) -> gchar* {
                const char* suffix = static_cast<const char*>(g_object_get_data(G_OBJECT(scale), "suffix"));
                return g_strdup_printf("%.*f%s", gtk_scale_get_digits(scale), value, suffix ? suffix : "");
            }), nullptr);
        }
        g_signal_connect(control, "value-changed", G_CALLBACK(+[](GtkRange* range, gpointer p) {
            if (g_lua_updating) return;
            char text[64];
            snprintf(text, sizeof(text), "%.4g", gtk_range_get_value(range));
            for (char* c = text; *c; ++c) if (*c == ',') *c = '.';
            WriteLuaValue(*static_cast<std::string*>(p), text);
        }), LuaKey(control, key));
    } else if (element.kind == "combo" && !a.empty()) {
        control = gtk_combo_box_text_new();
        for (const std::string& item : SplitString(a[0], '|')) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(control), item.c_str());
        gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
        g_signal_connect(control, "changed", G_CALLBACK(+[](GtkComboBox* combo, gpointer p) {
            if (!g_lua_updating && gtk_combo_box_get_active(combo) >= 0)
                WriteLuaValue(*static_cast<std::string*>(p), std::to_string(gtk_combo_box_get_active(combo)));
        }), LuaKey(control, key));
    } else if (element.kind == "multi" && !a.empty()) {
        record.items = SplitString(a[0], '|');
        control = gtk_menu_button_new();
        gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
        GtkWidget* popover = gtk_popover_new(control);
        GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_container_set_border_width(GTK_CONTAINER(list), 8);
        std::string* stored_key = LuaKey(control, key);
        for (size_t i = 0; i < record.items.size(); i++) {
            GtkWidget* check = gtk_check_button_new_with_label(record.items[i].c_str());
            g_object_set_data(G_OBJECT(check), "lua-list", list);
            g_object_set_data(G_OBJECT(check), "lua-key", stored_key);
            g_signal_connect(check, "toggled", G_CALLBACK(+[](GtkToggleButton* toggled, gpointer) {
                if (g_lua_updating) return;
                GtkWidget* container = GTK_WIDGET(g_object_get_data(G_OBJECT(toggled), "lua-list"));
                std::string value;
                GList* children = gtk_container_get_children(GTK_CONTAINER(container));
                int index = 0;
                for (GList* it = children; it; it = it->next, index++) {
                    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(it->data))) continue;
                    if (!value.empty()) value += ",";
                    value += std::to_string(index);
                }
                g_list_free(children);
                WriteLuaValue(*static_cast<std::string*>(g_object_get_data(G_OBJECT(toggled), "lua-key")), value);
            }), nullptr);
            gtk_box_pack_start(GTK_BOX(list), check, FALSE, FALSE, 0);
            record.checks.push_back(check);
        }
        gtk_widget_show_all(list);
        gtk_container_add(GTK_CONTAINER(popover), list);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(control), popover);
    } else if (element.kind == "color") {
        control = gtk_color_button_new();
        gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(control), TRUE);
        gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
        g_signal_connect(control, "color-set", G_CALLBACK(+[](GtkColorButton* button, gpointer p) {
            GdkRGBA rgba;
            gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &rgba);
            char text[48];
            snprintf(text, sizeof(text), "%d,%d,%d,%d", static_cast<int>(std::lround(rgba.red * 255)), static_cast<int>(std::lround(rgba.green * 255)),
                     static_cast<int>(std::lround(rgba.blue * 255)), static_cast<int>(std::lround(rgba.alpha * 255)));
            WriteLuaValue(*static_cast<std::string*>(p), text);
        }), LuaKey(control, key));
    } else if (element.kind == "bind") {
        control = MakeLuaBindButton(key);
    } else if (element.kind == "input") {
        control = gtk_entry_new();
        gtk_widget_set_size_request(control, 150, -1);
        gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
        g_signal_connect(control, "changed", G_CALLBACK(+[](GtkEditable* editable, gpointer p) {
            if (!g_lua_updating) WriteLuaValue(*static_cast<std::string*>(p), gtk_entry_get_text(GTK_ENTRY(editable)));
        }), LuaKey(control, key));
    }
    if (!control) return row;
    gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(row), control, element.kind == "slider", element.kind == "slider", 0);
    record.widget = control;
    g_lua_widgets.push_back(std::move(record));
    return row;
}

static void BuildLuaPanel() {
    if (!g_lua_panel) return;
    GList* children = gtk_container_get_children(GTK_CONTAINER(g_lua_panel));
    for (GList* it = children; it; it = it->next) gtk_widget_destroy(GTK_WIDGET(it->data));
    g_list_free(children);
    g_lua_widgets.clear();

    std::vector<LuaUiElement> elements = ReadLuaSchema();
    std::vector<std::string> scripts;
    for (const LuaUiElement& element : elements)
        if (element.visible && std::find(scripts.begin(), scripts.end(), element.script) == scripts.end()) scripts.push_back(element.script);

    for (const std::string& script : scripts) {
        std::string title = script;
        std::transform(title.begin(), title.end(), title.begin(), [](unsigned char c) { return std::toupper(c); });
        GtkWidget* card = MakeCard(title.c_str());
        for (const LuaUiElement& element : elements)
            if (element.script == script && element.visible)
                gtk_box_pack_start(GTK_BOX(card), BuildLuaElement(element, element.script + "\t" + element.name), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(g_lua_panel), card, FALSE, FALSE, 0);
    }
    if (scripts.empty()) {
        GtkWidget* card = MakeCard("SCRIPT SETTINGS");
        GtkWidget* hint = gtk_label_new("Scripts can add their own settings here with ui.checkbox, ui.slider, ui.combo, ui.color, ui.bind and ui.button.");
        gtk_label_set_xalign(GTK_LABEL(hint), 0.f);
        gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dim-label");
        gtk_box_pack_start(GTK_BOX(card), hint, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(g_lua_panel), card, FALSE, FALSE, 0);
    }
    RefreshLuaValues();
    gtk_widget_show_all(g_lua_panel);
}

static gboolean LuaPanelTick(gpointer) {
    time_t schema = FileMtime(SpaxerConfigPath("lua_ui.txt"));
    time_t values = FileMtime(SpaxerConfigPath("lua_values.txt"));
    if (schema != g_lua_schema_mtime) {
        g_lua_schema_mtime = schema;
        g_lua_values_mtime = values;
        BuildLuaPanel();
    } else if (values != g_lua_values_mtime) {
        g_lua_values_mtime = values;
        RefreshLuaValues();
    }
    return G_SOURCE_CONTINUE;
}

static pid_t FindOverlayPid() {
    namespace fs = std::filesystem;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator("/proc", error)) {
        std::string pid = entry.path().filename().string();
        if (pid.empty() || !std::isdigit(static_cast<unsigned char>(pid[0]))) continue;
        std::ifstream comm(entry.path() / "comm");
        std::string name;
        if (std::getline(comm, name) && name == "spaxer") return static_cast<pid_t>(std::atoi(pid.c_str()));
    }
    return 0;
}

static std::string OverlayBinary() {
    char self[4096] = {};
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) return "spaxer";
    return (std::filesystem::path(std::string(self, n)).parent_path() / "spaxer").string();
}

static void InjectOverlay() {
    if (FindOverlayPid()) return;
    std::string binary = OverlayBinary();
    std::string dir = std::filesystem::path(binary).parent_path().string();
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addchdir_np(&actions, dir.c_str());
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/tmp/spaxer_overlay.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    char* argv[] = {binary.data(), nullptr};
    pid_t child = 0;
    posix_spawn(&child, binary.c_str(), &actions, &attributes, argv, environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
}

static void UnloadOverlay() {
    if (pid_t pid = FindOverlayPid()) kill(pid, SIGTERM);
}

static void RestartOverlay() {
    pid_t pid = FindOverlayPid();
    if (!pid) {
        InjectOverlay();
        return;
    }
    kill(pid, SIGTERM);
    struct RestartState { pid_t pid; int ticks; };
    g_timeout_add(100, +[](gpointer data) -> gboolean {
        auto* state = static_cast<RestartState*>(data);
        bool alive = kill(state->pid, 0) == 0 && FindOverlayPid() == state->pid;
        if (alive && ++state->ticks == 40) kill(state->pid, SIGKILL);
        if (alive && state->ticks < 60) return G_SOURCE_CONTINUE;
        delete state;
        InjectOverlay();
        return G_SOURCE_REMOVE;
    }, new RestartState{pid, 0});
}

static GtkWidget* g_loader_status = nullptr;

static void UpdateLoaderStatus() {
    if (!g_loader_status) return;
    pid_t pid = FindOverlayPid();
    std::string text = pid ? "Injected · pid " + std::to_string(pid) : std::string("Not injected");
    if (text != gtk_label_get_text(GTK_LABEL(g_loader_status))) gtk_label_set_text(GTK_LABEL(g_loader_status), text.c_str());
    GtkStyleContext* style = gtk_widget_get_style_context(g_loader_status);
    if (pid) gtk_style_context_add_class(style, "loader-on");
    else gtk_style_context_remove_class(style, "loader-on");
}

static GtkWidget* MakeLoaderCard() {
    GtkWidget* card = MakeCard("LOADER");
    GtkWidget* status_row = LuaRow("Spaxer");
    g_loader_status = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_loader_status), "loader-status");
    g_signal_connect(g_loader_status, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) { g_loader_status = nullptr; }), nullptr);
    gtk_box_pack_start(GTK_BOX(status_row), g_loader_status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), status_row, FALSE, FALSE, 0);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(buttons), "row");
    GtkWidget* inject = gtk_button_new_with_label("Inject");
    GtkWidget* unload = gtk_button_new_with_label("Unload");
    GtkWidget* restart = gtk_button_new_with_label("Restart");
    gtk_style_context_add_class(gtk_widget_get_style_context(inject), "suggested-action");
    g_signal_connect(inject, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { InjectOverlay(); }), nullptr);
    g_signal_connect(unload, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { UnloadOverlay(); }), nullptr);
    g_signal_connect(restart, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { RestartOverlay(); }), nullptr);
    for (GtkWidget* button : {inject, unload, restart}) gtk_box_pack_start(GTK_BOX(buttons), button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(card), buttons, FALSE, FALSE, 0);

    GtkWidget* launch = gtk_button_new_with_label("Launch CS2");
    g_signal_connect(launch, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
        g_app_info_launch_default_for_uri("steam://rungameid/730", nullptr, nullptr);
    }), nullptr);
    GtkWidget* launch_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(launch_row), "row");
    gtk_box_pack_start(GTK_BOX(launch_row), launch, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(card), launch_row, FALSE, FALSE, 0);
    UpdateLoaderStatus();
    return card;
}

static GtkWidget* BuildGui() {
    for (BindCtx* ctx : g_bind_buttons) delete ctx;
    g_bind_buttons.clear();
    g_sidebar_items.clear();
    g_switches.clear();
    g_search_rows.clear();
    g_card_homes.clear();
    g_colors.clear();
    g_sliders.clear();
    g_checks.clear();
    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "spaxer");
    gtk_window_set_role(GTK_WINDOW(win), "spaxer-gui");
    gtk_window_set_default_size(GTK_WINDOW(win), 1120, 740);
    gtk_widget_set_size_request(win, 1120, 740);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER_ALWAYS);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_widget_set_app_paintable(win, TRUE);
    GdkScreen* scr = gtk_widget_get_screen(win);
    GdkVisual* v = gdk_screen_get_rgba_visual(scr);
    if (v) gtk_widget_set_visual(win, v);
    g_signal_connect(win, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer) -> gboolean {
        gtk_widget_hide(g_win); return TRUE;
    }), nullptr);
    g_signal_connect(win, "button-press-event", G_CALLBACK(+[](GtkWidget* w, GdkEventButton* e, gpointer) -> gboolean {
        if (e->button == 1 && e->y < 46) {
            gtk_window_begin_move_drag(GTK_WINDOW(w), e->button, e->x_root, e->y_root, e->time);
            return TRUE;
        }
        return FALSE;
    }), nullptr);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(root, "root");
    gtk_container_add(GTK_CONTAINER(win), root);
    gtk_box_pack_start(GTK_BOX(root), Titlebar(), FALSE, FALSE, 0);

    GtkWidget* body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(body, TRUE);
    gtk_widget_set_hexpand(body, TRUE);
    gtk_box_pack_start(GTK_BOX(root), body, TRUE, TRUE, 0);

    GtkWidget* sidebar_outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(sidebar_outer, "sidebar");
    gtk_widget_set_size_request(sidebar_outer, 230, -1);
    gtk_box_pack_start(GTK_BOX(body), sidebar_outer, FALSE, TRUE, 0);
    GtkWidget* sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(sidebar_outer), sidebar, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(sidebar_outer), SteamProfileWidget(), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(sidebar_outer), ConfigsWidget(),      FALSE, FALSE, 0);

    g_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(g_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(g_stack), 160);
    gtk_widget_set_hexpand(g_stack, TRUE);
    gtk_widget_set_vexpand(g_stack, TRUE);
    gtk_box_pack_start(GTK_BOX(body), g_stack, TRUE, TRUE, 0);

    {
        GtkWidget* page = MakePage();

        GtkWidget* aim = MakeCard("AIMBOT");
        gtk_box_pack_start(GTK_BOX(aim), MakeRow("Enabled",       &g_cfg->aimbot_enabled,      &g_cfg->bind_aimbot), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(aim), MakeRow("Silent aim",    &g_cfg->silent_aim,          &g_cfg->bind_silent_aim), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(aim), MakeRow("Auto scope",    &g_cfg->auto_scope,          &g_cfg->bind_auto_scope), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(aim), MakeRow("Auto pistol",   &g_cfg->auto_pistol,         nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(aim), MakeComboRow("Aim key", &g_cfg->aimbot_key_mode, {"Always", "Left mouse", "Right mouse", "Mouse side"}), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(aim), MakeComboRow("Target priority", &g_cfg->aimbot_target_mode, {"Crosshair", "Distance", "Lowest HP"}), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(aim), MakeRow("AimLock",       &g_cfg->aimbot_lock,         nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(aim), MakeSliderRow("Switch delay (ms)", &g_cfg->aimbot_switch_delay_ms, 0, 1000, 10), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(aim), MakeRow("Through walls", &g_cfg->aimbot_thru_walls,   nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(aim), MakeRow("Flash check",   &g_cfg->aimbot_flash_check,  nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(aim), MakeFovSliderRow("FOV", &g_cfg->aimbot_fov_x100), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(aim), MakeSliderRow("Speed",        &g_cfg->aimbot_smooth_x100, 5, 100, 1), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(aim), MakeSliderRow("Sensitivity",  &g_cfg->aimbot_sens_x1000, 10, 500, 5), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(aim), MakeRow("Head",   &g_cfg->aimbot_point_head,   nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(aim), MakeRow("Neck",   &g_cfg->aimbot_point_neck,   nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(aim), MakeRow("Chest",  &g_cfg->aimbot_point_chest,  nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(aim), MakeRow("Pelvis", &g_cfg->aimbot_point_pelvis, nullptr), FALSE, FALSE, 0);
        Place(page, aim, false);

        GtkWidget* human = MakeCard("HUMANIZATION");
        gtk_box_pack_start(GTK_BOX(human), MakeRow("Enabled", &g_cfg->aimbot_humanize, nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(human), MakeSliderRow("Speed min", &g_cfg->aimbot_speed_min_x100, 1, 100, 1), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(human), MakeSliderRow("Speed max", &g_cfg->aimbot_speed_max_x100, 1, 100, 1), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(human), MakeSliderRow("Shake", &g_cfg->aimbot_shake_x100, 0, 100, 1), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(human), MakeSliderRow("Release speed", &g_cfg->aimbot_release_x100, 5, 100, 1), FALSE, FALSE, 0);
        Place(page, human, true);

        GtkWidget* tb = MakeCard("TRIGGER BOT");
        gtk_box_pack_start(GTK_BOX(tb), MakeRow("Enabled",        &g_cfg->trigger_enabled,         &g_cfg->bind_trigger), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeRow("Aim correction", &g_cfg->trigger_aim_correction,  nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeRow("Crouch fire",    &g_cfg->trigger_shift_fire,      nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeRow("Flash check",    &g_cfg->trigger_flash_check,     nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeRow("Autowall",       &g_cfg->autowall,                nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeRow("Auto stop",      &g_cfg->trigger_autostop,        nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeCheckSliderRow("Min damage", &g_cfg->min_damage_enabled, &g_cfg->autowall_min_damage, 1, 100, 1), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeSliderRow("Hitchance",  &g_cfg->trigger_hitchance, 0, 100, 1), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeRow("Force shot", &g_cfg->trigger_force_shot, &g_cfg->bind_force_shot), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeRow("Min damage override", &g_cfg->trigger_md_override, &g_cfg->bind_md_override), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeSliderRow("Override damage", &g_cfg->md_override_value, 1, 100, 1), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeSliderRow("Delay (ms)", &g_cfg->trigger_delay_ms, 0, 500, 5), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeFovSliderRow("FOV", &g_cfg->trigger_fov_x100), FALSE, FALSE, 0);
        Place(page, tb, true);

        GtkWidget* spread = MakeCard("SPREAD TRIGGER");
        gtk_box_pack_start(GTK_BOX(spread), MakeRow("Enabled", &g_cfg->trigger_spread, &g_cfg->bind_spread_trigger), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(spread), MakeSliderRow("Coverage %", &g_cfg->spread_coverage, 50, 100, 1), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(spread), MakeRow("Head only", &g_cfg->spread_head_only, nullptr), FALSE, FALSE, 0);
        Place(page, spread, true);

        GtkWidget* rcs = MakeCard("RCS");
        gtk_box_pack_start(GTK_BOX(rcs), MakeRow("Enabled", &g_cfg->rcs_enabled, nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(rcs), MakeSliderRow("Strength", &g_cfg->rcs_strength_x100, 0, 100, 1), FALSE, FALSE, 0);
        Place(page, rcs, false);

        gtk_stack_add_named(GTK_STACK(g_stack), page, "legit");
        AddSidebarHeader(sidebar, "COMBAT");
        AddSidebarItemSvg(sidebar, s_svg_legit.c_str(), "legit", "Legit bot");
    }

    {
        GtkWidget* page = MakePage();
        GtkWidget* bhop = MakeCard("BUNNY HOP");
        gtk_box_pack_start(GTK_BOX(bhop), MakeRow("Enabled", &g_cfg->bunnyhop, &g_cfg->bind_bunnyhop), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bhop), MakeRow("Auto jump", &g_cfg->bhop_auto_jump, nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bhop), MakeComboRow("Jump method", &g_cfg->bhop_method, {"Space", "Scroll wheel (bind mwheeldown +jump)"}), FALSE, FALSE, 0);
        Place(page, bhop, false);

        GtkWidget* strafe = MakeCard("AUTO STRAFE");
        gtk_box_pack_start(GTK_BOX(strafe), MakeRow("Enabled", &g_cfg->auto_strafe, &g_cfg->bind_auto_strafe), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(strafe), MakeComboRow("Strafe mode", &g_cfg->auto_strafe_mode, {"Follow mouse", "Full auto"}), FALSE, FALSE, 0);
        Place(page, strafe, false);

        GtkWidget* stop = MakeCard("FAST STOP");
        gtk_box_pack_start(GTK_BOX(stop), MakeRow("Enabled", &g_cfg->fast_stop_enabled, &g_cfg->bind_fast_stop), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(stop), MakeComboRow("Fast stop mode", &g_cfg->fast_stop_mode, {"In air", "Always"}), FALSE, FALSE, 0);
        Place(page, stop, true);

        GtkWidget* ladder = MakeCard("LADDER");
        gtk_box_pack_start(GTK_BOX(ladder), MakeRow("Fast ladder", &g_cfg->fast_ladder, nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(ladder), MakeRow("Ladder jump assist", &g_cfg->ladder_jump, nullptr), FALSE, FALSE, 0);
        Place(page, ladder, true);

        GtkWidget* edge = MakeCard("EDGE & ADVANCED");
        gtk_box_pack_start(GTK_BOX(edge), MakeRow("Edge jump", &g_cfg->edge_jump, &g_cfg->bind_edge_jump), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(edge), MakeRow("Edge bug",  &g_cfg->edge_bug,  &g_cfg->bind_edge_bug),  FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(edge), MakeRow("Long jump", &g_cfg->long_jump, &g_cfg->bind_long_jump), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(edge), MakeRow("Slide hop", &g_cfg->slide_hop, nullptr),                FALSE, FALSE, 0);
        Place(page, edge, true);
        gtk_stack_add_named(GTK_STACK(g_stack), page, "movement");
        AddSidebarHeader(sidebar, "MOVEMENT");
        AddSidebarItemSvg(sidebar, s_svg_movement.c_str(), "movement", "Movement");
    }
    {
        GtkWidget* page = MakePage();
        GtkWidget* esp = MakeCard("ESP");
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Enabled",            &g_cfg->esp,                 &g_cfg->bind_esp),     FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Enemies only",       &g_cfg->esp_team_check,      nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Box",                &g_cfg->esp_box,             nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Health bar",         &g_cfg->esp_health,          nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Name",               &g_cfg->esp_name,            nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Player weapon",      &g_cfg->esp_weapon,          nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Ammo",               &g_cfg->esp_ammo,            nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Skeleton",           &g_cfg->esp_skeleton,        nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Head circle",        &g_cfg->esp_head_circle,     nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Flags (flashed, bomb, kit…)", &g_cfg->esp_flags, nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("My grenade prediction", &g_cfg->grenade_trajectory,  nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Thrown grenades (all)", &g_cfg->grenade_world,  nullptr), FALSE, FALSE, 0);
        Place(page, esp, false);

        GtkWidget* sound = MakeCard("SOUND ESP");
        gtk_box_pack_start(GTK_BOX(sound), MakeRow("Enabled", &g_cfg->sound_esp, &g_cfg->bind_sound_esp), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(sound), MakeColorRow("Color", &g_cfg->sound_esp_rgba), FALSE, FALSE, 0);
        Place(page, sound, false);

        GtkWidget* weapons = MakeCard("WEAPON ESP");
        gtk_box_pack_start(GTK_BOX(weapons), MakeRow("Enabled", &g_cfg->esp_dropped_weapons, &g_cfg->bind_weapon_esp), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(weapons), MakeSliderRow("Max distance (m)", &g_cfg->weapon_esp_distance_m, 5, 150, 1), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(weapons), MakeColorRow("Color", &g_cfg->weapon_esp_rgba), FALSE, FALSE, 0);
        Place(page, weapons, false);

        GtkWidget* arrows = MakeCard("ARROWS");
        gtk_box_pack_start(GTK_BOX(arrows), MakeRow("Enabled", &g_cfg->arrows, &g_cfg->bind_arrows), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(arrows), MakeSliderRow("Radius", &g_cfg->arrows_radius, 40, 500, 5), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(arrows), MakeSliderRow("Size", &g_cfg->arrows_size, 6, 32, 1), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(arrows), MakeColorRow("Color", &g_cfg->arrows_rgba), FALSE, FALSE, 0);
        Place(page, arrows, false);

        Place(page, MakeEspPreview(), true);
        GtkWidget* glow = MakeCard("GLOW");
        gtk_box_pack_start(GTK_BOX(glow), MakeRow("Enabled",    &g_cfg->glow,      &g_cfg->bind_glow), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(glow), MakeRow("Teammates",  &g_cfg->glow_team, nullptr),           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(glow), MakeColorRow("Enemy color",    &g_cfg->glow_enemy_rgba), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(glow), MakeColorRow("Teammate color", &g_cfg->glow_team_rgba),  FALSE, FALSE, 0);
        Place(page, glow, true);

        GtkWidget* chams = MakeCard("CHAMS");
        gtk_box_pack_start(GTK_BOX(chams), MakeRow("Enabled",          &g_cfg->chams,      &g_cfg->bind_chams), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(chams), MakeRow("Teammates",        &g_cfg->chams_team, nullptr),            FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(chams), MakeComboRow("Material", &g_cfg->chams_material, {"Metallic", "Flat"}), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(chams), MakeSliderRow("Latency comp (ms)", &g_cfg->render_lead_ms, 0, 100, 1), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(chams), MakeComboRow("Hide game model", &g_cfg->chams_hide_model, {"Off", "Transparent", "No draw"}), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(chams), MakeRow("Model tint (game)", &g_cfg->chams_tint, nullptr),           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(chams), MakeColorRow("Visible color",  &g_cfg->chams_visible_rgba), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(chams), MakeColorRow("Hidden color",   &g_cfg->chams_hidden_rgba),  FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(chams), MakeColorRow("Teammate color", &g_cfg->chams_team_rgba),    FALSE, FALSE, 0);
        Place(page, chams, true);

        gtk_stack_add_named(GTK_STACK(g_stack), page, "render-esp");
    }
    {
        GtkWidget* page = MakePage();
        GtkWidget* hud = MakeCard("HUD");
        gtk_box_pack_start(GTK_BOX(hud), MakeRow("Watermark",       &g_cfg->watermark,  &g_cfg->bind_watermark), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hud), MakeRow("Bomb timer",      &g_cfg->bomb_timer, &g_cfg->bind_bomb),      FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hud), MakeRow("Keybinds panel",  &g_cfg->keybinds,   &g_cfg->bind_keybinds),  FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hud), MakeRow("Spectators list", &g_cfg->spectators, nullptr),                FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hud), MakeRow("Media player",    &g_cfg->media_player, nullptr),             FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hud), MakeRow("Velocity graph",  &g_cfg->velocity_graph, nullptr),            FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hud), MakeRow("Keystrokes",      &g_cfg->keystrokes, nullptr),                FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hud), MakeRow("Notifications",   &g_cfg->notifications, nullptr),             FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hud), MakeRow("Hitmarker",       &g_cfg->hitmarker,  nullptr),                FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hud), MakeRow("Radar",           &g_cfg->radar,      nullptr),                FALSE, FALSE, 0);

        GtkWidget* theme_card = MakeCard("THEME");
        gtk_box_pack_start(GTK_BOX(theme_card), MakeComboRow("Preset",
            &g_cfg->hud_theme, {"Blue", "Purple", "Green", "Red", "Orange", "Pink", "Cyan"}), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(theme_card), MakeColorRow("Custom accent (0 = use preset)", &g_cfg->hud_accent_rgba), FALSE, FALSE, 0);
        Place(page, theme_card, true);
        Place(page, hud, false);

        GtkWidget* cross = MakeCard("CROSSHAIR");
        gtk_box_pack_start(GTK_BOX(cross), MakeRow("Custom crosshair", &g_cfg->crosshair,        &g_cfg->bind_crosshair), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(cross), MakeRow("Sniper crosshair", &g_cfg->sniper_crosshair, nullptr),                FALSE, FALSE, 0);
        Place(page, cross, true);


        gtk_stack_add_named(GTK_STACK(g_stack), page, "render-hud");
    }
    {
        GtkWidget* page = MakePage();
        GtkWidget* world_card = MakeCard("WORLD");
        gtk_box_pack_start(GTK_BOX(world_card), MakeSliderRow("Brightness %", &g_cfg->world_brightness, 10, 400, 5), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(world_card), MakeRow("Night mode", &g_cfg->night_mode, &g_cfg->bind_night_mode), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(world_card), MakeSliderRow("Night strength", &g_cfg->night_mode_strength, 0, 100, 1), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(world_card), MakeRow("Night sky (stars, moon)", &g_cfg->night_sky, nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(world_card), MakeRow("Ambient tint", &g_cfg->ambient_tint, nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(world_card), MakeColorRow("Tint color", &g_cfg->ambient_tint_rgba), FALSE, FALSE, 0);
        Place(page, world_card, false);

        GtkWidget* saturation = MakeCard("SATURATION");
        gtk_box_pack_start(GTK_BOX(saturation), MakeRow("Saturation", &g_cfg->saturation, nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(saturation), MakeSliderRow("Saturation %", &g_cfg->saturation_value, 0, 300, 5), FALSE, FALSE, 0);
        Place(page, saturation, false);

        GtkWidget* weather = MakeCard("WEATHER");
        gtk_box_pack_start(GTK_BOX(weather), MakeComboRow("Effect", &g_cfg->weather_mode, {"Off", "Rain", "Snow", "Ash", "Embers"}), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(weather), MakeSliderRow("Density", &g_cfg->weather_density, 5, 100, 1), FALSE, FALSE, 0);
        Place(page, weather, true);

        gtk_stack_add_named(GTK_STACK(g_stack), page, "render-world");
    }
    AddSidebarHeader(sidebar, "VISUALS");
    AddSidebarItemSvg(sidebar, s_svg_esp.c_str(), "render-esp", "ESP");
    AddSidebarItemSvg(sidebar, s_svg_hud.c_str(), "render-hud", "HUD");
    AddSidebarItemSvg(sidebar, s_svg_world.c_str(), "render-world", "World");
    {
        GtkWidget* page = MakePage();
        Place(page, MakeLoaderCard(), false);
        GtkWidget* uns = MakeCard("UNSAFE (memory writes)");
        gtk_box_pack_start(GTK_BOX(uns), MakeRow("Thirdperson",   &g_cfg->thirdperson,          &g_cfg->bind_thirdperson), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(uns), MakeRow("No flash",      &g_cfg->no_flash,             nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(uns), MakeRow("No smoke",      &g_cfg->no_smoke,             nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(uns), MakeRow("Smoke recolor", &g_cfg->smoke_color_enabled,  nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(uns), MakeColorRow("Smoke color", &g_cfg->smoke_color_rgba),            FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(uns), MakeSliderRow("Smoke strength %", &g_cfg->smoke_color_strength, 10, 300, 5), FALSE, FALSE, 0);
        Place(page, uns, false);
        GtkWidget* radar = MakeCard("RADAR HACK");
        gtk_box_pack_start(GTK_BOX(radar), MakeRow("Enabled", &g_cfg->radar_hack, nullptr), FALSE, FALSE, 0);
        Place(page, radar, true);
        GtkWidget* misc = MakeCard("MISC");
        gtk_box_pack_start(GTK_BOX(misc), MakeRow("Toggle GUI (bind)", nullptr, &g_cfg->bind_toggle_gui), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(misc), MakeRow("Edit HUD",  &g_cfg->edit_mode, &g_cfg->bind_edit_hud), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(misc), MakeRow("Auto accept", &g_cfg->auto_accept, nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(misc), MakeRow("Clan tag spin", &g_cfg->clan_tag_spin, nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(misc), MakeRow("Auto pickup", &g_cfg->auto_pickup, nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(misc), MakeRow("Damage log", &g_cfg->damage_log, nullptr), FALSE, FALSE, 0);
        Place(page, misc, true);


        GtkWidget* sound = MakeCard("HIT SOUND");
        gtk_box_pack_start(GTK_BOX(sound), MakeComboRow("Hit sound", &g_cfg->hit_sound, {"Off", "Click", "Ding", "Bell", "Pop"}), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(sound), MakeSliderRow("Hit sound volume", &g_cfg->hit_sound_volume, 0, 100, 1), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(sound), MakeRow("Kills only", &g_cfg->hit_sound_kills_only, nullptr), FALSE, FALSE, 0);
        GtkWidget* preview_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_style_context_add_class(gtk_widget_get_style_context(preview_row), "row");
        GtkWidget* preview_hit = gtk_button_new_with_label("Play hit");
        GtkWidget* preview_kill = gtk_button_new_with_label("Play kill");
        auto play = +[](GtkButton* button, gpointer kill) {
            hitsound::Play(static_cast<hitsound::Style>(g_cfg->hit_sound), g_cfg->hit_sound_volume, kill != nullptr);
        };
        g_signal_connect(preview_hit, "clicked", G_CALLBACK(play), nullptr);
        g_signal_connect(preview_kill, "clicked", G_CALLBACK(play), GINT_TO_POINTER(1));
        gtk_box_pack_start(GTK_BOX(preview_row), preview_hit, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(preview_row), preview_kill, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(sound), preview_row, FALSE, FALSE, 0);
        Place(page, sound, false);
        gtk_stack_add_named(GTK_STACK(g_stack), page, "misc");
        AddSidebarHeader(sidebar, "OTHER");
        AddSidebarItemSvg(sidebar, s_svg_misc.c_str(), "misc", "Misc");
    }
    {
        GtkWidget* page = MakePage();
        GtkWidget* box = PageBox(page);
        GtkWidget* card = MakeCard("LUA SCRIPTS");
        GtkWidget* btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget* btn_folder = gtk_button_new_with_label("Open folder");
        GtkWidget* btn_refresh = gtk_button_new_with_label("Reload scripts");
        gtk_box_pack_start(GTK_BOX(btn_row), btn_folder, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(btn_row), btn_refresh, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(card), btn_row, FALSE, FALSE, 0);

        GtkWidget* list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_box_pack_start(GTK_BOX(card), list_box, FALSE, FALSE, 0);

        PopulateScriptsList(GTK_BOX(list_box));

        g_signal_connect(btn_folder, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
            OpenPath(ScriptsDir());
        }), nullptr);
        g_signal_connect(btn_refresh, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
            RequestScriptReload();
            PopulateScriptsList(GTK_BOX(d));
        }), list_box);

        gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);
        g_lua_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
        g_signal_connect(g_lua_panel, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) {
            g_lua_panel = nullptr;
            g_lua_widgets.clear();
        }), nullptr);
        Place(page, g_lua_panel, true);
        g_lua_schema_mtime = 0;
        LuaPanelTick(nullptr);
        gtk_stack_add_named(GTK_STACK(g_stack), page, "scripts");
        AddSidebarItemSvg(sidebar, s_svg_scripts.c_str(), "scripts", "Scripts");
    }

    g_search_page = MakePage();
    gtk_stack_add_named(GTK_STACK(g_stack), g_search_page, "search");
    g_signal_connect(g_stack, "notify::visible-child-name", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer) {
        const char* current = gtk_stack_get_visible_child_name(GTK_STACK(g_stack));
        if (current && std::string(current) != "search" && !g_card_homes.empty() && g_search_entry) {
            g_page_before_search = current;
            gtk_entry_set_text(GTK_ENTRY(g_search_entry), "");
            ApplySearch("");
        }
        RefreshSidebarHighlight();
    }), nullptr);
    gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "legit");
    RefreshSidebarHighlight();
    return win;
}

static void HotkeyThread() {
    Display* d = XOpenDisplay(nullptr);
    int keyboards[16]{};
    int kbd_count = 0;
    for (int i = 0; i < 32 && kbd_count < 16; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned char event_bits[(EV_CNT + 7) / 8] = {};
        if (ioctl(fd, EVIOCGBIT(0, sizeof(event_bits)), event_bits) >= 0) {
            bool has_key = (event_bits[EV_KEY / 8] & (1 << (EV_KEY % 8))) != 0;
            bool has_rel = (event_bits[EV_REL / 8] & (1 << (EV_REL % 8))) != 0;
            bool has_abs = (event_bits[EV_ABS / 8] & (1 << (EV_ABS % 8))) != 0;
            if (has_key && !has_rel && !has_abs) {
                keyboards[kbd_count++] = fd;
                continue;
            }
        }
        close(fd);
    }

    bool prev = false;
    while (g_running.load()) {
        if (system("pgrep -x spaxer >/dev/null 2>&1") == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        uint32_t kv = g_cfg ? g_cfg->bind_toggle_gui : GDK_KEY_Insert;
        if (!kv) kv = GDK_KEY_Insert;
        bool now = false;

        if (kbd_count > 0 && (kv == GDK_KEY_Insert || kv == 0)) {
            unsigned char keys[(KEY_CNT + 7) / 8] = {};
            for (int i = 0; i < kbd_count; i++) {
                if (ioctl(keyboards[i], EVIOCGKEY(sizeof(keys)), keys) >= 0 &&
                    (keys[KEY_INSERT / 8] & (1 << (KEY_INSERT % 8)))) {
                    now = true;
                    break;
                }
            }
        }

        if (!now && d) {
            char keys[32]{};
            XQueryKeymap(d, keys);
            KeyCode kc = XKeysymToKeycode(d, (KeySym)kv);
            if (!kc && kv == GDK_KEY_Insert) kc = XKeysymToKeycode(d, XK_Insert);
            if (!kc) kc = XKeysymToKeycode(d, XK_KP_Insert);
            now = kc && (keys[kc / 8] & (1 << (kc & 7)));
        }

        if (now && !prev) {
            g_idle_add(+[](gpointer) -> gboolean {
                if (!g_win) return G_SOURCE_REMOVE;
                if (gtk_widget_get_visible(g_win)) {
                    gtk_widget_hide(g_win);
                } else {
                    gtk_widget_show_all(g_win);
                    gtk_window_present(GTK_WINDOW(g_win));
                }
                return G_SOURCE_REMOVE;
            }, nullptr);
        }
        prev = now;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    for (int i = 0; i < kbd_count; i++) close(keyboards[i]);
    if (d) XCloseDisplay(d);
}

static std::atomic<int> g_toggle_flag{0};

static gboolean CheckToggleFlag(gpointer) {
    if (g_toggle_flag.exchange(0) && g_win) {
        if (gtk_widget_get_visible(g_win)) {
            gtk_widget_hide(g_win);
        } else {
            gtk_widget_show_all(g_win);
            if (g_search_entry) ApplySearch(gtk_entry_get_text(GTK_ENTRY(g_search_entry)));
            gtk_window_present(GTK_WINDOW(g_win));
        }
    }
    return G_SOURCE_CONTINUE;
}

int main(int argc, char** argv) {
    g_set_prgname("spaxer-gui");
    g_set_application_name("spaxer");
    gtk_init(&argc, &argv);
    gdk_set_program_class("spaxer-gui");
    g_cfg = settings::Attach();
    if (!g_cfg) { fprintf(stderr, "spaxer-gui: cannot attach settings\n"); return 1; }
    struct sigaction sa{};
    sa.sa_handler = +[](int) { g_toggle_flag.store(1); };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, nullptr);
    g_timeout_add(50, CheckToggleFlag, nullptr);
    g_timeout_add(200, SyncWidgetsWithSettings, nullptr);
    g_timeout_add(300, LuaPanelTick, nullptr);
    g_timeout_add(500, +[](gpointer) -> gboolean { UpdateLoaderStatus(); return G_SOURCE_CONTINUE; }, nullptr);
    InstallCss();
    g_win = BuildGui();
    gtk_widget_show_all(g_win);
    std::thread ht(HotkeyThread);
    gtk_main();
    g_running.store(false);
    if (ht.joinable()) ht.join();
    return 0;
}
