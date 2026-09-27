#include "state.h"
#include "config/settings.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <unordered_map>
#include <algorithm>

namespace features {

struct Cs2Crosshair {
    int  size = 3;
    int  gap = 0;
    int  thickness = 1;
    int  style = 4;
    bool dot = false;
    bool outline_on = false;
    int  outline_thickness = 1;
    int  color_index = 5;
    int  r = 0, g = 255, b = 0, a = 255;
};

static std::string FindCs2ConvarsPath() {
    const char* home = getenv("HOME");
    if (!home) return {};
    const char* bases[] = {
        "/.local/share/Steam/userdata",
        "/.steam/steam/userdata",
        "/.steam/root/userdata",
    };
    std::string newest;
    time_t newest_time = 0;
    for (const char* rel : bases) {
        std::string base = std::string(home) + rel;
        DIR* d = opendir(base.c_str());
        if (!d) continue;
        dirent* e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            std::string p = base + "/" + e->d_name + "/730/local/cfg/cs2_user_convars_0_slot0.vcfg";
            struct stat st;
            if (stat(p.c_str(), &st) == 0 && st.st_mtime >= newest_time) {
                newest = p;
                newest_time = st.st_mtime;
            }
        }
        closedir(d);
    }
    return newest;
}

static bool ParseKV(const std::string& text, std::unordered_map<std::string, std::string>& out) {
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] != '"') i++;
        if (i >= text.size()) break;
        size_t ks = ++i;
        while (i < text.size() && text[i] != '"') i++;
        if (i >= text.size()) break;
        std::string key = text.substr(ks, i - ks);
        i++;
        while (i < text.size() && text[i] != '"' && text[i] != '\n') i++;
        if (i >= text.size() || text[i] == '\n') continue;
        size_t vs = ++i;
        while (i < text.size() && text[i] != '"') i++;
        std::string val = text.substr(vs, i - vs);
        i++;
        out[key] = val;
    }
    return !out.empty();
}

static void ApplyPaletteColor(Cs2Crosshair& c) {
    switch (c.color_index) {
        case 0: c.r=255; c.g=0;   c.b=0;   break;
        case 1: c.r=0;   c.g=255; c.b=0;   break;
        case 2: c.r=255; c.g=255; c.b=0;   break;
        case 3: c.r=0;   c.g=0;   c.b=255; break;
        case 4: c.r=0;   c.g=255; c.b=255; break;
    }
}

bool ReloadCs2Crosshair(Settings* cfg) {
    std::string path = FindCs2ConvarsPath();
    if (path.empty()) return false;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    std::string buf;
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) buf.append(tmp, n);
    fclose(f);

    std::unordered_map<std::string, std::string> kv;
    if (!ParseKV(buf, kv)) return false;

    Cs2Crosshair c;
    auto geti = [&](const char* k, int def){ auto it = kv.find(k); return it != kv.end() ? atoi(it->second.c_str()) : def; };
    c.color_index       = geti("cl_crosshaircolor", 5);
    ApplyPaletteColor(c);
    if (c.color_index == 5) {
        c.r = geti("cl_crosshaircolor_r", 0);
        c.g = geti("cl_crosshaircolor_g", 255);
        c.b = geti("cl_crosshaircolor_b", 0);
    }
    c.a         = geti("cl_crosshairalpha",     255);
    c.gap       = geti("cl_crosshairgap",       0);
    c.size      = geti("cl_crosshairsize",      3);
    c.thickness = geti("cl_crosshairthickness", 1);
    c.style     = geti("cl_crosshairstyle",     4);
    c.dot       = geti("cl_crosshairdot",       0) != 0;
    c.outline_on = geti("cl_crosshair_outlinethickness", 0) > 0
                || geti("cl_crosshair_drawoutline", 0) != 0;
    c.outline_thickness = geti("cl_crosshair_outlinethickness", 1);

    cfg->crosshair_size      = static_cast<uint32_t>(std::clamp(c.size, 1, 40));
    cfg->crosshair_gap       = std::clamp(c.gap, -10, 40);
    cfg->crosshair_thickness = static_cast<uint32_t>(std::clamp(c.thickness, 1, 10));
    settings::SetEnabled(cfg->crosshair_dot, c.dot);
    settings::SetEnabled(cfg->crosshair_outline, c.outline_on);
    uint32_t rgba = (uint32_t(c.r) << 24) | (uint32_t(c.g) << 16) | (uint32_t(c.b) << 8) | uint32_t(c.a);
    cfg->crosshair_color_rgba = rgba;
    return true;
}

}
