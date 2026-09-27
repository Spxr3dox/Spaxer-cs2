#include "config/settings.h"
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
#include <cstring>
#include <string>

static Settings* g_cfg = nullptr;
static GtkWidget* g_win = nullptr;
static GtkWidget* g_stack = nullptr;
static std::atomic<bool> g_running{true};
static GtkWidget* BuildGui();

struct BindCtx { GtkButton* btn; uint32_t* field; };

static gboolean OnBindKey(GtkWidget* w, GdkEventKey* e, gpointer d) {
    BindCtx* b = static_cast<BindCtx*>(d);
    guint kv = e->keyval;
    if (kv == GDK_KEY_Escape) kv = 0;
    *b->field = kv;
    const char* n = gdk_keyval_name(kv);
    gtk_button_set_label(b->btn, kv ? (n ? n : "?") : "None");
    g_signal_handlers_disconnect_by_func(w, (gpointer)OnBindKey, d);
    return TRUE;
}

static void OnBindClicked(GtkButton* btn, gpointer d) {
    BindCtx* b = static_cast<BindCtx*>(d);
    gtk_button_set_label(btn, "…");
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
    g_signal_connect(btn, "clicked", G_CALLBACK(OnBindClicked), ctx);
    g_signal_connect(btn, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer d) { delete static_cast<BindCtx*>(d); }), ctx);
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
    }
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
    gtk_box_pack_start(GTK_BOX(row), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), scale, TRUE, TRUE, 0);
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
    gtk_box_pack_start(GTK_BOX(row), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), scale, TRUE, TRUE, 0);
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
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "page");
    gtk_widget_set_hexpand(box, TRUE);
    gtk_container_add(GTK_CONTAINER(scr), box);
    g_object_set_data(G_OBJECT(scr), "content", box);
    return scr;
}

static GtkWidget* PageBox(GtkWidget* page) {
    return GTK_WIDGET(g_object_get_data(G_OBJECT(page), "content"));
}

static void AddSidebarItem(GtkWidget* sidebar, const char* icon, const char* name, const char* label) {
    GtkWidget* btn = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(btn), "sidebar-item");
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* ic = gtk_label_new(icon);
    gtk_style_context_add_class(gtk_widget_get_style_context(ic), "sidebar-icon");
    GtkWidget* lbl = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.f);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_box_pack_start(GTK_BOX(row), ic, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(btn), row);
    g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer name) {
        gtk_stack_set_visible_child_name(GTK_STACK(g_stack), (const char*)name);
    }), (gpointer)name);
    gtk_box_pack_start(GTK_BOX(sidebar), btn, FALSE, FALSE, 0);
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

static void InstallCss() {
    const char* css =
        "* { font-family: -apple-system, 'SF Pro Text', 'SF Pro Display', 'Inter', 'Helvetica Neue', sans-serif; font-size: 13px; }"
        "window { background: transparent; }"
        "#root { background: rgba(30, 30, 34, 0.75); border-radius: 12px; border: 1px solid rgba(255,255,255,0.08); }"
        "#titlebar { background: transparent; padding: 12px 14px 6px 14px; }"
        "#titlebar label { color: #d1d1d6; font-weight: 600; font-size: 12px; }"

        ".traffic { min-width: 14px; min-height: 14px; padding: 0; margin: 0 4px; border: none; border-radius: 50%; box-shadow: inset 0 0 0 0.5px rgba(0,0,0,0.35); }"
        ".traffic.close  { background: #ff5f57; }"
        ".traffic.close:hover  { background: #ff746e; }"
        ".traffic.hide   { background: #28c840; }"
        ".traffic.hide:hover   { background: #4dd865; }"

        "#sidebar { background: rgba(20, 20, 22, 0.35); border-right: 1px solid rgba(255,255,255,0.06); padding: 12px 8px; }"
        ".sidebar-item { background: transparent; color: #c8c8ce; border: none; border-radius: 8px; padding: 8px 12px; margin: 2px 4px; box-shadow: none; }"
        ".sidebar-item:hover { background: rgba(255,255,255,0.06); }"
        ".sidebar-item:active { background: rgba(255,255,255,0.14); color: #ffffff; }"
        ".sidebar-icon { color: #a0a0a6; font-size: 15px; min-width: 18px; }"

        ".configs { padding: 8px 10px; margin: 8px 4px; background: rgba(255,255,255,0.04); border-radius: 8px; border: 1px solid rgba(255,255,255,0.06); }"
        ".configs-title { color: #8e8e93; font-size: 10px; font-weight: 700; letter-spacing: 0.6px; margin-bottom: 2px; }"
        ".configs entry { min-height: 24px; padding: 4px 10px; font-size: 11px; background: rgba(0,0,0,0.35); border-radius: 8px; }"
        ".configs button { min-height: 24px; padding: 4px 10px; font-size: 11px; background: rgba(120,120,128,0.24); border-radius: 8px; }"
        ".configs button:hover { background: rgba(120,120,128,0.38); }"
        ".configs combobox { background: transparent; }"
        ".configs combobox box { background: rgba(0,0,0,0.35); border-radius: 8px; padding: 0; border: none; }"
        ".configs combobox button { min-height: 24px; padding: 4px 10px; font-size: 11px; background: transparent; box-shadow: none; border: none; }"
        ".configs combobox arrow { color: #8e8e93; min-width: 10px; min-height: 10px; padding-right: 6px; }"
        ".configs combobox window menu { background: rgba(40,40,45,0.98); border-radius: 8px; padding: 4px; border: 1px solid rgba(255,255,255,0.08); }"
        ".configs combobox window menu menuitem { color: #f2f2f7; border-radius: 6px; padding: 6px 10px; }"
        ".configs combobox window menu menuitem:hover { background: #007aff; color: white; }"

        ".profile { padding: 10px 12px; margin: 6px 4px 4px 4px; background: rgba(255,255,255,0.06); border-radius: 10px; border: 1px solid rgba(255,255,255,0.06); }"
        ".profile .avatar { border-radius: 18px; background-color: rgba(0,0,0,0.3); }"
        ".profile-name { color: #ffffff; font-weight: 600; font-size: 12px; }"
        ".profile-sub  { color: #8e8e93; font-size: 10px; }"

        "stack { background: transparent; }"
        ".page { padding: 18px 24px 24px 24px; }"

        ".card { background: rgba(255,255,255,0.06); border-radius: 10px; border: 1px solid rgba(255,255,255,0.05); }"
        ".card-title { color: #8e8e93; font-size: 10px; font-weight: 700; letter-spacing: 0.6px; margin: 10px 14px 6px 14px; }"

        ".row { padding: 8px 14px; border-top: 1px solid rgba(255,255,255,0.05); }"
        ".row:first-child { border-top: none; }"
        ".row label { color: #ffffff; font-size: 13px; }"

        "switch { min-width: 44px; min-height: 26px; background: rgba(120,120,128,0.32); border-radius: 13px; border: none; padding: 0; }"
        "switch:checked { background: #34c759; }"
        "switch slider { background: white; border-radius: 50%; min-width: 22px; min-height: 22px; margin: 2px; box-shadow: 0 2px 4px rgba(0,0,0,0.35); border: none; }"

        "scale { padding: 8px 0; }"
        "scale trough { background: rgba(120,120,128,0.32); border-radius: 3px; min-height: 4px; border: none; }"
        "scale highlight { background: #007aff; border-radius: 3px; }"
        "scale slider { background: white; border-radius: 50%; min-width: 20px; min-height: 20px; margin: -8px; box-shadow: 0 1px 3px rgba(0,0,0,0.35); border: none; }"
        "scale value { color: #8e8e93; font-size: 11px; margin-left: 6px; }"

        "button { background: rgba(120,120,128,0.24); color: #ffffff; border: none; border-radius: 6px; padding: 4px 10px; font-weight: 500; font-size: 11px; }"
        "button:hover { background: rgba(120,120,128,0.36); }"
        "button:active { background: rgba(0,122,255,0.7); }"
        ".bind { background: rgba(0,122,255,0.18); color: #5ac8fa; font-family: monospace; }"
        ".bind:hover { background: rgba(0,122,255,0.32); color: #ffffff; }"

        "scrollbar { background: transparent; }"
        "scrollbar slider { background: rgba(255,255,255,0.24); border-radius: 3px; min-width: 6px; min-height: 30px; }"
        "scrollbar slider:hover { background: rgba(255,255,255,0.4); }";

    GtkCssProvider* p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, css, -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

static GtkWidget* Titlebar() {
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_name(bar, "titlebar");
    GtkWidget* close = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(close), "traffic");
    gtk_style_context_add_class(gtk_widget_get_style_context(close), "close");
    g_signal_connect(close, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { gtk_widget_hide(g_win); }), nullptr);
    GtkWidget* hide = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(hide), "traffic");
    gtk_style_context_add_class(gtk_widget_get_style_context(hide), "hide");
    g_signal_connect(hide, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { gtk_widget_hide(g_win); }), nullptr);
    GtkWidget* title = gtk_label_new("spaxer");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_label_set_xalign(GTK_LABEL(title), 0.5f);
    gtk_box_pack_start(GTK_BOX(bar), close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), hide, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), title, TRUE, TRUE, 0);
    return bar;
}

static GtkWidget* BuildGui() {
    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "spaxer");
    gtk_window_set_role(GTK_WINDOW(win), "spaxer-gui");
    gtk_window_set_default_size(GTK_WINDOW(win), 720, 520);
    gtk_widget_set_size_request(win, 720, 520);
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
        if (e->button == 1 && e->y < 44 && e->x > 60) {
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
    gtk_widget_set_size_request(sidebar_outer, 190, -1);
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
        GtkWidget* box = PageBox(page);

        GtkWidget* aim = MakeCard("AIMBOT");
        gtk_box_pack_start(GTK_BOX(aim), MakeRow("Enabled",       &g_cfg->aimbot_enabled,      &g_cfg->bind_aimbot), FALSE, FALSE, 0);
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
        gtk_box_pack_start(GTK_BOX(box), aim, FALSE, FALSE, 0);

        GtkWidget* tb = MakeCard("TRIGGER BOT");
        gtk_box_pack_start(GTK_BOX(tb), MakeRow("Enabled",        &g_cfg->trigger_enabled,         &g_cfg->bind_trigger), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeRow("Aim correction", &g_cfg->trigger_aim_correction,  nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeRow("Crouch fire",    &g_cfg->trigger_shift_fire,      nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeRow("Flash check",    &g_cfg->trigger_flash_check,     nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeSliderRow("Hitchance",  &g_cfg->trigger_hitchance, 0, 100, 1), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeSliderRow("Delay (ms)", &g_cfg->trigger_delay_ms, 0, 500, 5), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tb), MakeFovSliderRow("FOV", &g_cfg->trigger_fov_x100), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), tb, FALSE, FALSE, 0);

        GtkWidget* rcs = MakeCard("RCS");
        gtk_box_pack_start(GTK_BOX(rcs), MakeRow("Enabled", &g_cfg->rcs_enabled, nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(rcs), MakeSliderRow("Strength", &g_cfg->rcs_strength_x100, 0, 100, 1), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), rcs, FALSE, FALSE, 0);

        gtk_stack_add_named(GTK_STACK(g_stack), page, "legit");
        AddSidebarItem(sidebar, "◐", "legit", "Legit bot");
    }
    {
        GtkWidget* page = MakePage();
        GtkWidget* box = PageBox(page);
        GtkWidget* c = MakeCard("MOVEMENT");
        gtk_box_pack_start(GTK_BOX(c), MakeRow("Bunny hop",   &g_cfg->bunnyhop,    &g_cfg->bind_bunnyhop),    FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(c), MakeRow("Auto jump",   &g_cfg->bhop_auto_jump, nullptr),               FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(c), MakeRow("Auto strafe", &g_cfg->auto_strafe, &g_cfg->bind_auto_strafe), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(g_stack), page, "movement");
        AddSidebarItem(sidebar, "◒", "movement", "Movement");
    }
    {
        GtkWidget* page = MakePage();
        GtkWidget* box = PageBox(page);

        GtkWidget* esp = MakeCard("ESP");
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Enabled",            &g_cfg->esp,                 &g_cfg->bind_esp),     FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Enemies only",       &g_cfg->esp_team_check,      nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Box",                &g_cfg->esp_box,             nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Health bar",         &g_cfg->esp_health,          nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Name",               &g_cfg->esp_name,            nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Player weapon",      &g_cfg->esp_weapon,          nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Skeleton",           &g_cfg->esp_skeleton,        nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Head circle",        &g_cfg->esp_head_circle,     nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Sound ESP",          &g_cfg->sound_esp,           nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Arrows (Cursor)",    &g_cfg->arrows,              &g_cfg->bind_arrows),  FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Weapon ESP (floor)", &g_cfg->esp_dropped_weapons, nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(esp), MakeRow("Grenade Trajectory", &g_cfg->grenade_trajectory,  nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), esp, FALSE, FALSE, 0);

        GtkWidget* glow = MakeCard("GLOW");
        gtk_box_pack_start(GTK_BOX(glow), MakeRow("Enabled",    &g_cfg->glow,      &g_cfg->bind_glow), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(glow), MakeRow("Teammates",  &g_cfg->glow_team, nullptr),           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(glow), MakeColorRow("Enemy color",    &g_cfg->glow_enemy_rgba), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(glow), MakeColorRow("Teammate color", &g_cfg->glow_team_rgba),  FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), glow, FALSE, FALSE, 0);

        GtkWidget* chams = MakeCard("CHAMS");
        gtk_box_pack_start(GTK_BOX(chams), MakeRow("Enabled",          &g_cfg->chams,      &g_cfg->bind_chams), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(chams), MakeRow("Teammates",        &g_cfg->chams_team, nullptr),            FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(chams), MakeComboRow("Material", &g_cfg->chams_material, {"Metallic", "Flat"}), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(chams), MakeComboRow("Hide game model", &g_cfg->chams_hide_model, {"Off", "Transparent", "No draw"}), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(chams), MakeRow("Model tint (game)", &g_cfg->chams_tint, nullptr),           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(chams), MakeColorRow("Visible color",  &g_cfg->chams_visible_rgba), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(chams), MakeColorRow("Hidden color",   &g_cfg->chams_hidden_rgba),  FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(chams), MakeColorRow("Teammate color", &g_cfg->chams_team_rgba),    FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), chams, FALSE, FALSE, 0);

        GtkWidget* hud = MakeCard("HUD");
        gtk_box_pack_start(GTK_BOX(hud), MakeRow("Watermark",       &g_cfg->watermark,  &g_cfg->bind_watermark), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hud), MakeRow("Bomb timer",      &g_cfg->bomb_timer, &g_cfg->bind_bomb),      FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hud), MakeRow("Keybinds panel",  &g_cfg->keybinds,   &g_cfg->bind_keybinds),  FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hud), MakeRow("Spectators list", &g_cfg->spectators, nullptr),                FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hud), MakeRow("Hitmarker",       &g_cfg->hitmarker,  nullptr),                FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hud), MakeRow("Radar",           &g_cfg->radar,      nullptr),                FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), hud, FALSE, FALSE, 0);

        GtkWidget* cross = MakeCard("CROSSHAIR");
        gtk_box_pack_start(GTK_BOX(cross), MakeRow("Custom crosshair", &g_cfg->crosshair,        &g_cfg->bind_crosshair), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(cross), MakeRow("Sniper crosshair", &g_cfg->sniper_crosshair, nullptr),                FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), cross, FALSE, FALSE, 0);

        gtk_stack_add_named(GTK_STACK(g_stack), page, "render");
        AddSidebarItem(sidebar, "◈", "render", "Render");
    }
    {
        GtkWidget* page = MakePage();
        GtkWidget* box = PageBox(page);
        GtkWidget* uns = MakeCard("UNSAFE (memory writes)");
        gtk_box_pack_start(GTK_BOX(uns), MakeRow("Thirdperson",   &g_cfg->thirdperson,          &g_cfg->bind_thirdperson), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(uns), MakeRow("No flash",      &g_cfg->no_flash,             nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(uns), MakeRow("No smoke",      &g_cfg->no_smoke,             nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(uns), MakeRow("Smoke recolor", &g_cfg->smoke_color_enabled,  nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(uns), MakeColorRow("Smoke color", &g_cfg->smoke_color_rgba),            FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), uns, FALSE, FALSE, 0);
        GtkWidget* radar = MakeCard("RADAR HACK");
        gtk_box_pack_start(GTK_BOX(radar), MakeRow("Enabled", &g_cfg->radar_hack, nullptr), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), radar, FALSE, FALSE, 0);
        GtkWidget* misc = MakeCard("MISC");
        gtk_box_pack_start(GTK_BOX(misc), MakeRow("Toggle GUI (bind)", nullptr, &g_cfg->bind_toggle_gui), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(misc), MakeRow("Edit HUD",  &g_cfg->edit_mode, &g_cfg->bind_edit_hud), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), misc, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(g_stack), page, "misc");
        AddSidebarItem(sidebar, "◓", "misc", "Misc");
    }

    gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "legit");
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
    InstallCss();
    g_win = BuildGui();
    gtk_widget_show_all(g_win);
    std::thread ht(HotkeyThread);
    gtk_main();
    g_running.store(false);
    if (ht.joinable()) ht.join();
    return 0;
}
