#include "features/lua_compat.h"
#include "config/settings.h"
#include "memory/process.h"
#include "render/model_chams.h"
#include "sdk/game.h"
#include "sdk/offsets.h"
#include "sdk/visibility.h"
#include "state.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <lua.hpp>
#include <pango/pangocairo.h>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

extern char** environ;

namespace lua_compat {

namespace {

cairo_t* s_cr = nullptr;
const render::Camera* s_camera = nullptr;
Settings* s_cfg = nullptr;
uint64_t s_frame_count = 0;
double s_frame_time = 0.0;
int s_clip_depth = 0;
std::vector<GdkPixbuf*> s_textures;

struct SchemaField {
    uint32_t offset;
    std::string type;
    std::string cls;
};

std::unordered_map<std::string, std::vector<SchemaField>> s_schema;
time_t s_schema_mtime = 0;

void LoadSchema() {
    static gint64 next_check = 0;
    gint64 now = g_get_monotonic_time();
    if (now < next_check) return;
    next_check = now + 2000000;
    FILE* f = fopen("/tmp/schema_full.txt", "r");
    if (!f) return;
    int fd = fileno(f);
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_mtime == s_schema_mtime) {
        fclose(f);
        return;
    }
    s_schema_mtime = st.st_mtime;
    s_schema.clear();
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char cls[128], name[128];
        unsigned offset = 0;
        int consumed = 0;
        if (sscanf(line, "%127s %127s %u %n", cls, name, &offset, &consumed) < 3) continue;
        std::string type = line + consumed;
        while (!type.empty() && (type.back() == '\n' || type.back() == '\r')) type.pop_back();
        s_schema[name].push_back({offset, type, cls});
    }
    fclose(f);
}

cairo_t* Cr(lua_State* L) {
    if (!s_cr) luaL_error(L, "render functions are only available inside the \"paint\" callback");
    return s_cr;
}

void SetColor(lua_State* L, int index) {
    double r = luaL_optnumber(L, index, 255) / 255.0, g = luaL_optnumber(L, index + 1, 255) / 255.0;
    double b = luaL_optnumber(L, index + 2, 255) / 255.0, a = luaL_optnumber(L, index + 3, 255) / 255.0;
    cairo_set_source_rgba(s_cr, std::clamp(r, 0.0, 1.0), std::clamp(g, 0.0, 1.0), std::clamp(b, 0.0, 1.0), std::clamp(a, 0.0, 1.0));
}

std::vector<double> Numbers(lua_State* L, int index) {
    luaL_checktype(L, index, LUA_TTABLE);
    std::vector<double> out;
    int count = static_cast<int>(lua_objlen(L, index));
    out.reserve(count);
    for (int i = 1; i <= count; i++) {
        lua_rawgeti(L, index, i);
        out.push_back(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    return out;
}

int Poly(lua_State* L) {
    cairo_t* cr = Cr(L);
    std::vector<double> points = Numbers(L, 1);
    if (points.size() < 4) return 0;
    const char* mode = luaL_optstring(L, 6, "fill");
    bool closed = lua_isnoneornil(L, 8) ? true : lua_toboolean(L, 8);
    cairo_new_path(cr);
    cairo_move_to(cr, points[0], points[1]);
    for (size_t i = 2; i + 1 < points.size(); i += 2) cairo_line_to(cr, points[i], points[i + 1]);
    if (closed) cairo_close_path(cr);
    SetColor(L, 2);
    if (!strcmp(mode, "fill")) {
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
        cairo_fill(cr);
    } else {
        cairo_set_line_width(cr, luaL_optnumber(L, 7, 1.0));
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_stroke(cr);
    }
    return 0;
}

int Arc(lua_State* L) {
    cairo_t* cr = Cr(L);
    double x = luaL_checknumber(L, 1), y = luaL_checknumber(L, 2), radius = luaL_checknumber(L, 3);
    double a0 = luaL_checknumber(L, 4) * M_PI / 180.0, a1 = luaL_checknumber(L, 5) * M_PI / 180.0;
    cairo_new_path(cr);
    cairo_arc(cr, x, y, radius, a0, a1);
    SetColor(L, 6);
    cairo_set_line_width(cr, luaL_optnumber(L, 10, 1.0));
    cairo_stroke(cr);
    return 0;
}

int ClipPush(lua_State* L) {
    cairo_t* cr = Cr(L);
    double x0 = luaL_checknumber(L, 1), y0 = luaL_checknumber(L, 2), x1 = luaL_checknumber(L, 3), y1 = luaL_checknumber(L, 4);
    bool intersect = lua_isnoneornil(L, 5) ? true : lua_toboolean(L, 5);
    cairo_save(cr);
    if (!intersect) cairo_reset_clip(cr);
    cairo_rectangle(cr, std::min(x0, x1), std::min(y0, y1), std::fabs(x1 - x0), std::fabs(y1 - y0));
    cairo_clip(cr);
    s_clip_depth++;
    return 0;
}

int ClipPop(lua_State* L) {
    cairo_t* cr = Cr(L);
    if (s_clip_depth > 0) {
        cairo_restore(cr);
        s_clip_depth--;
    }
    return 0;
}

int GradientRect(lua_State* L) {
    cairo_t* cr = Cr(L);
    double x0 = luaL_checknumber(L, 1), y0 = luaL_checknumber(L, 2), x1 = luaL_checknumber(L, 3), y1 = luaL_checknumber(L, 4);
    std::vector<double> c = Numbers(L, 5);
    if (c.size() < 16) return 0;
    cairo_pattern_t* mesh = cairo_pattern_create_mesh();
    cairo_mesh_pattern_begin_patch(mesh);
    cairo_mesh_pattern_move_to(mesh, x0, y0);
    cairo_mesh_pattern_line_to(mesh, x1, y0);
    cairo_mesh_pattern_line_to(mesh, x1, y1);
    cairo_mesh_pattern_line_to(mesh, x0, y1);
    for (int corner = 0; corner < 4; corner++)
        cairo_mesh_pattern_set_corner_color_rgba(mesh, corner, c[corner * 4] / 255.0, c[corner * 4 + 1] / 255.0,
                                                 c[corner * 4 + 2] / 255.0, c[corner * 4 + 3] / 255.0);
    cairo_mesh_pattern_end_patch(mesh);
    cairo_set_source(cr, mesh);
    cairo_rectangle(cr, std::min(x0, x1), std::min(y0, y1), std::fabs(x1 - x0), std::fabs(y1 - y0));
    cairo_fill(cr);
    cairo_pattern_destroy(mesh);
    return 0;
}

int Radial(lua_State* L) {
    cairo_t* cr = Cr(L);
    double x = luaL_checknumber(L, 1), y = luaL_checknumber(L, 2), radius = luaL_checknumber(L, 3);
    std::vector<double> c = Numbers(L, 4);
    if (c.size() < 8) return 0;
    cairo_pattern_t* pattern = cairo_pattern_create_radial(x, y, 0, x, y, radius);
    cairo_pattern_add_color_stop_rgba(pattern, 0, c[0] / 255.0, c[1] / 255.0, c[2] / 255.0, c[3] / 255.0);
    cairo_pattern_add_color_stop_rgba(pattern, 1, c[4] / 255.0, c[5] / 255.0, c[6] / 255.0, c[7] / 255.0);
    cairo_new_path(cr);
    cairo_arc(cr, x, y, radius, 0, 2 * M_PI);
    cairo_set_source(cr, pattern);
    cairo_fill(cr);
    cairo_pattern_destroy(pattern);
    return 0;
}

PangoLayout* Layout(cairo_t* cr, const char* text, const char* family, double size, bool bold) {
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* desc = pango_font_description_from_string(family && *family ? family : "Roboto");
    pango_font_description_set_absolute_size(desc, std::max(1.0, size) * PANGO_SCALE);
    if (bold) pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_text(layout, text, -1);
    return layout;
}

int Text(lua_State* L) {
    cairo_t* cr = Cr(L);
    const char* text = luaL_checkstring(L, 1);
    PangoLayout* layout = Layout(cr, text, luaL_optstring(L, 2, "Roboto"), luaL_optnumber(L, 3, 14), lua_toboolean(L, 4));
    int w, h;
    pango_layout_get_pixel_size(layout, &w, &h);
    SetColor(L, 7);
    cairo_move_to(cr, std::round(luaL_checknumber(L, 5)), std::round(luaL_checknumber(L, 6)));
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    lua_pushnumber(L, w);
    lua_pushnumber(L, h);
    return 2;
}

int Measure(lua_State* L) {
    static cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    static cairo_t* cr = cairo_create(surface);
    PangoLayout* layout = Layout(cr, luaL_checkstring(L, 1), luaL_optstring(L, 2, "Roboto"), luaL_optnumber(L, 3, 14), lua_toboolean(L, 4));
    int w, h;
    pango_layout_get_pixel_size(layout, &w, &h);
    g_object_unref(layout);
    lua_pushnumber(L, w);
    lua_pushnumber(L, h);
    return 2;
}

int FontFile(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (access(path, R_OK) != 0) {
        lua_pushstring(L, path);
        return 1;
    }
    FcConfigAppFontAddFile(FcConfigGetCurrent(), reinterpret_cast<const FcChar8*>(path));
    int count = 0;
    FcPattern* pattern = FcFreeTypeQuery(reinterpret_cast<const FcChar8*>(path), 0, nullptr, &count);
    FcChar8* family = nullptr;
    if (pattern && FcPatternGetString(pattern, FC_FAMILY, 0, &family) == FcResultMatch && family) {
        lua_pushstring(L, reinterpret_cast<const char*>(family));
    } else {
        lua_pushnil(L);
    }
    if (pattern) FcPatternDestroy(pattern);
    pango_cairo_font_map_set_default(nullptr);
    return 1;
}

int StoreTexture(lua_State* L, GdkPixbuf* pixbuf) {
    if (!pixbuf) {
        lua_pushnil(L);
        return 1;
    }
    s_textures.push_back(pixbuf);
    lua_pushinteger(L, static_cast<lua_Integer>(s_textures.size()));
    lua_pushinteger(L, gdk_pixbuf_get_width(pixbuf));
    lua_pushinteger(L, gdk_pixbuf_get_height(pixbuf));
    return 3;
}

int TextureFile(lua_State* L) {
    return StoreTexture(L, gdk_pixbuf_new_from_file(luaL_checkstring(L, 1), nullptr));
}

int TextureMemory(lua_State* L) {
    size_t size = 0;
    const char* data = luaL_checklstring(L, 1, &size);
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    GdkPixbuf* pixbuf = nullptr;
    if (gdk_pixbuf_loader_write(loader, reinterpret_cast<const guchar*>(data), size, nullptr) && gdk_pixbuf_loader_close(loader, nullptr)) {
        pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
        if (pixbuf) g_object_ref(pixbuf);
    } else {
        gdk_pixbuf_loader_close(loader, nullptr);
    }
    g_object_unref(loader);
    return StoreTexture(L, pixbuf);
}

int TextureRgba(lua_State* L) {
    std::vector<double> values = Numbers(L, 1);
    int w = static_cast<int>(luaL_checkinteger(L, 2)), h = static_cast<int>(luaL_checkinteger(L, 3));
    if (w <= 0 || h <= 0 || values.size() < static_cast<size_t>(w) * h * 4) {
        lua_pushnil(L);
        return 1;
    }
    GdkPixbuf* pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w, h);
    guchar* pixels = gdk_pixbuf_get_pixels(pixbuf);
    int stride = gdk_pixbuf_get_rowstride(pixbuf);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w * 4; x++) pixels[y * stride + x] = static_cast<guchar>(std::clamp(values[(y * w) * 4 + x], 0.0, 255.0));
    return StoreTexture(L, pixbuf);
}

int TextureDraw(lua_State* L) {
    cairo_t* cr = Cr(L);
    lua_Integer id = luaL_checkinteger(L, 1);
    if (id < 1 || id > static_cast<lua_Integer>(s_textures.size())) return 0;
    GdkPixbuf* pixbuf = s_textures[id - 1];
    double x0 = luaL_checknumber(L, 2), y0 = luaL_checknumber(L, 3), x1 = luaL_checknumber(L, 4), y1 = luaL_checknumber(L, 5);
    double alpha = luaL_optnumber(L, 9, 255) / 255.0;
    double rounding = luaL_optnumber(L, 10, 0);
    double w = x1 - x0, h = y1 - y0;
    if (std::fabs(w) < 1 || std::fabs(h) < 1) return 0;
    cairo_save(cr);
    cairo_new_path(cr);
    if (rounding > 0) {
        double r = std::min({rounding, std::fabs(w) / 2, std::fabs(h) / 2});
        cairo_arc(cr, x0 + w - r, y0 + r, r, -M_PI / 2, 0);
        cairo_arc(cr, x0 + w - r, y0 + h - r, r, 0, M_PI / 2);
        cairo_arc(cr, x0 + r, y0 + h - r, r, M_PI / 2, M_PI);
        cairo_arc(cr, x0 + r, y0 + r, r, M_PI, 1.5 * M_PI);
        cairo_close_path(cr);
    } else {
        cairo_rectangle(cr, x0, y0, w, h);
    }
    cairo_clip(cr);
    cairo_translate(cr, x0, y0);
    cairo_scale(cr, w / gdk_pixbuf_get_width(pixbuf), h / gdk_pixbuf_get_height(pixbuf));
    gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint_with_alpha(cr, std::clamp(alpha, 0.0, 1.0));
    cairo_restore(cr);
    return 0;
}

int FrameInfo(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(s_frame_count));
    lua_pushnumber(L, s_frame_time);
    return 2;
}

int WorldToScreen(lua_State* L) {
    float sx, sy;
    if (!s_camera || !s_camera->valid ||
        !s_camera->Project(static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)),
                           static_cast<float>(luaL_checknumber(L, 3)), sx, sy))
        return 0;
    lua_pushnumber(L, sx);
    lua_pushnumber(L, sy);
    return 2;
}

int LevelName(lua_State* L) {
    lua_pushstring(L, vis::CurrentMap().c_str());
    return 1;
}

int Thirdperson(lua_State* L) {
    lua_pushboolean(L, s_cfg && settings::Enabled(s_cfg->thirdperson));
    return 1;
}

std::string GameDirectory() {
    const char* home = getenv("HOME");
    return std::string(home ? home : "") + "/.local/share/Steam/steamapps/common/Counter-Strike Global Offensive/game";
}

int Version(lua_State* L) {
    FILE* f = fopen((GameDirectory() + "/csgo/steam.inf").c_str(), "r");
    std::string version;
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "PatchVersion=", 13)) {
                version = line + 13;
                while (!version.empty() && (version.back() == '\n' || version.back() == '\r')) version.pop_back();
                break;
            }
        }
        fclose(f);
    }
    lua_pushstring(L, version.c_str());
    return 1;
}

int Sound(lua_State* L) {
    std::string path = luaL_checkstring(L, 1);
    double volume = std::clamp(luaL_optnumber(L, 2, 1.0), 0.0, 1.0);
    if (access(path.c_str(), R_OK) != 0) {
        std::string game_sound = GameDirectory() + "/csgo/sounds/" + path;
        if (access(game_sound.c_str(), R_OK) != 0) return 0;
        path = game_sound;
    }
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}
    std::string vol = std::to_string(volume);
    for (char& c : vol) if (c == ',') c = '.';
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    char* argv[] = {const_cast<char*>("pw-play"), const_cast<char*>("--volume"), vol.data(), path.data(), nullptr};
    pid_t child;
    posix_spawnp(&child, "pw-play", &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    return 0;
}

int Notice(lua_State* L) {
    PushNotice(luaL_checkstring(L, 1), NoticeKind::Info);
    return 0;
}

int Bones(lua_State* L) {
    uintptr_t pawn = static_cast<uintptr_t>(luaL_checknumber(L, 1));
    lua_newtable(L);
    if (!pawn || !off::m_pGameSceneNode || !off::m_modelState) return 1;
    uintptr_t node = g_proc.Read<uintptr_t>(pawn + off::m_pGameSceneNode);
    uintptr_t array = node ? g_proc.Read<uintptr_t>(node + off::m_modelState + 0x80) : 0;
    if (!array) return 1;
    chams::BoneTransform bones[32];
    if (!g_proc.ReadBytes(array, bones, sizeof(bones))) return 1;
    for (int i = 0; i < 32; i++) {
        lua_newtable(L);
        lua_pushnumber(L, bones[i].x); lua_rawseti(L, -2, 1);
        lua_pushnumber(L, bones[i].y); lua_rawseti(L, -2, 2);
        lua_pushnumber(L, bones[i].z); lua_rawseti(L, -2, 3);
        lua_rawseti(L, -2, i);
    }
    return 1;
}

Vec3 CheckVec(lua_State* L, int index) {
    return {static_cast<float>(luaL_checknumber(L, index)), static_cast<float>(luaL_checknumber(L, index + 1)),
            static_cast<float>(luaL_checknumber(L, index + 2))};
}

int Raycast(lua_State* L) {
    Vec3 from = CheckVec(L, 1), to = CheckVec(L, 4);
    Vec3 hit{}, normal{};
    bool blocked = vis::Ready() && vis::Raycast(from, to, hit, &normal, lua_toboolean(L, 7) ? vis::Blocks::Grenades : vis::Blocks::Sight);
    if (!blocked) hit = to;
    float total = std::sqrt((to.x - from.x) * (to.x - from.x) + (to.y - from.y) * (to.y - from.y) + (to.z - from.z) * (to.z - from.z));
    float done = std::sqrt((hit.x - from.x) * (hit.x - from.x) + (hit.y - from.y) * (hit.y - from.y) + (hit.z - from.z) * (hit.z - from.z));
    lua_pushboolean(L, blocked);
    lua_pushnumber(L, total > 0 ? done / total : 1.0);
    lua_pushnumber(L, hit.x); lua_pushnumber(L, hit.y); lua_pushnumber(L, hit.z);
    lua_pushnumber(L, normal.x); lua_pushnumber(L, normal.y); lua_pushnumber(L, normal.z);
    lua_pushboolean(L, vis::Ready());
    return 9;
}

int Damage(lua_State* L) {
    uintptr_t pawn = static_cast<uintptr_t>(luaL_checknumber(L, 1));
    Vec3 from = CheckVec(L, 2), to = CheckVec(L, 5);
    vis::Ballistics ballistics{};
    if (!pawn || !vis::Ready() || !vis::WeaponBallistics(game::ActiveWeaponDefinitionIndex(pawn), ballistics)) return 0;
    float damage = vis::DamageAt(from, to, ballistics);
    if (damage <= 0.f) return 0;
    lua_pushnumber(L, damage);
    return 1;
}

int Entities(lua_State* L) {
    int from = static_cast<int>(luaL_optinteger(L, 1, 1)), to = static_cast<int>(luaL_optinteger(L, 2, 2048));
    lua_newtable(L);
    int n = 0;
    for (const game::EntitySlot& slot : game::EntitySnapshot(game::EntityList(), std::max(1, from), std::min(to, 32768))) {
        lua_newtable(L);
        lua_pushinteger(L, slot.index); lua_setfield(L, -2, "index");
        lua_pushnumber(L, static_cast<double>(slot.entity)); lua_setfield(L, -2, "address");
        lua_pushstring(L, slot.designer); lua_setfield(L, -2, "designer");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int LocalEntities(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(game::LocalController()));
    lua_pushnumber(L, static_cast<double>(game::LocalPawn()));
    lua_pushinteger(L, off::g_LocalControllerIdx);
    return 3;
}

int EntityByIndex(lua_State* L) {
    uintptr_t entity = game::EntityFromList(game::EntityList(), static_cast<int>(luaL_checkinteger(L, 1)));
    if (!entity) return 0;
    lua_pushnumber(L, static_cast<double>(entity));
    return 1;
}

int EntityInfo(lua_State* L) {
    uintptr_t entity = static_cast<uintptr_t>(luaL_checknumber(L, 1));
    if (!entity) return 0;
    uintptr_t identity = g_proc.Read<uintptr_t>(entity + game::kEntityIdentity);
    if (!identity) return 0;
    uintptr_t name_ptr = g_proc.Read<uintptr_t>(identity + game::kIdentityDesignerName);
    uint32_t handle = g_proc.Read<uint32_t>(identity + 0x10);
    lua_pushstring(L, game::DesignerNameAt(name_ptr));
    lua_pushnumber(L, handle);
    return 2;
}

int Netvar(lua_State* L) {
    LoadSchema();
    const char* name = luaL_checkstring(L, 1);
    const char* cls = luaL_optstring(L, 2, nullptr);
    auto it = s_schema.find(name);
    if (it == s_schema.end()) {
        uintptr_t fallback = 0;
        if (!off::Get(name, fallback) || !fallback) return 0;
        lua_pushnumber(L, static_cast<double>(fallback));
        lua_pushstring(L, "?");
        return 2;
    }
    const SchemaField* chosen = &it->second.front();
    if (cls)
        for (const SchemaField& field : it->second)
            if (field.cls == cls) chosen = &field;
    lua_pushnumber(L, chosen->offset);
    lua_pushstring(L, chosen->type.c_str());
    lua_pushstring(L, chosen->cls.c_str());
    return 3;
}

struct Module {
    uintptr_t base = 0;
    std::string path;
    std::vector<std::pair<uintptr_t, uintptr_t>> readable;
};

Module FindModule(const std::string& name) {
    Module module;
    char maps[64];
    snprintf(maps, sizeof(maps), "/proc/%d/maps", g_proc.pid());
    FILE* f = fopen(maps, "r");
    if (!f) return module;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        uintptr_t start, end, file_offset;
        char perms[8] = {}, path[768] = {};
        if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %767[^\n]", &start, &end, perms, &file_offset, path) < 4) continue;
        std::string p = path;
        size_t slash = p.rfind('/');
        std::string file = slash == std::string::npos ? p : p.substr(slash + 1);
        if (file != name && file != name + ".so" && file != "lib" + name + ".so") continue;
        if (!module.base || file_offset == 0) module.base = module.base ? std::min(module.base, start) : start;
        module.path = p;
        if (perms[0] == 'r') module.readable.push_back({start, end});
    }
    fclose(f);
    return module;
}

int FindPattern(lua_State* L) {
    Module module = FindModule(luaL_checkstring(L, 1));
    const char* pattern_text = luaL_checkstring(L, 2);
    lua_Integer extra = luaL_optinteger(L, 3, 0);
    std::vector<int> pattern;
    for (const char* p = pattern_text; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '?') {
            pattern.push_back(-1);
            while (*p == '?') p++;
        } else {
            pattern.push_back(static_cast<int>(strtol(p, const_cast<char**>(&p), 16)));
        }
    }
    if (pattern.empty() || module.readable.empty()) return 0;
    std::vector<uint8_t> buffer;
    for (auto [start, end] : module.readable) {
        buffer.resize(end - start);
        if (!g_proc.ReadBytes(start, buffer.data(), buffer.size())) continue;
        for (size_t i = 0; i + pattern.size() <= buffer.size(); i++) {
            size_t k = 0;
            while (k < pattern.size() && (pattern[k] < 0 || buffer[i + k] == pattern[k])) k++;
            if (k == pattern.size()) {
                lua_pushnumber(L, static_cast<double>(start + i + extra));
                return 1;
            }
        }
    }
    return 0;
}

int FindExport(lua_State* L) {
    Module module = FindModule(luaL_checkstring(L, 1));
    const char* wanted = luaL_checkstring(L, 2);
    if (module.path.empty()) return 0;
    FILE* f = fopen(module.path.c_str(), "rb");
    if (!f) return 0;
    std::vector<uint8_t> file;
    fseek(f, 0, SEEK_END);
    file.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    size_t got = fread(file.data(), 1, file.size(), f);
    fclose(f);
    if (got < sizeof(Elf64_Ehdr)) return 0;
    auto* header = reinterpret_cast<const Elf64_Ehdr*>(file.data());
    if (header->e_shoff + static_cast<size_t>(header->e_shnum) * sizeof(Elf64_Shdr) > file.size()) return 0;
    auto* sections = reinterpret_cast<const Elf64_Shdr*>(file.data() + header->e_shoff);
    for (int i = 0; i < header->e_shnum; i++) {
        if (sections[i].sh_type != SHT_DYNSYM) continue;
        const Elf64_Shdr& strings = sections[sections[i].sh_link];
        size_t count = sections[i].sh_size / sizeof(Elf64_Sym);
        auto* symbols = reinterpret_cast<const Elf64_Sym*>(file.data() + sections[i].sh_offset);
        for (size_t s = 0; s < count; s++) {
            if (!symbols[s].st_value) continue;
            const char* name = reinterpret_cast<const char*>(file.data() + strings.sh_offset + symbols[s].st_name);
            if (strcmp(name, wanted) != 0) continue;
            lua_pushnumber(L, static_cast<double>(module.base + symbols[s].st_value));
            return 1;
        }
    }
    return 0;
}

int GameDir(lua_State* L) {
    lua_pushstring(L, GameDirectory().c_str());
    return 1;
}

int UserName(lua_State* L) {
    const char* home = getenv("HOME");
    std::string path = std::string(home ? home : "") + "/.local/share/Steam/config/loginusers.vdf";
    std::string name, recent;
    if (FILE* f = fopen(path.c_str(), "r")) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            char key[256]{}, value[256]{};
            if (sscanf(line, " \"%255[^\"]\" \"%255[^\"]\"", key, value) != 2) continue;
            if (!strcmp(key, "PersonaName")) name = value;
            if (!strcmp(key, "MostRecent") && !strcmp(value, "1") && recent.empty()) recent = name;
        }
        fclose(f);
    }
    if (!recent.empty()) name = recent;
    if (name.empty()) {
        const char* user = getenv("USER");
        name = user ? user : "";
    }
    lua_pushstring(L, name.c_str());
    return 1;
}

int Menu(lua_State* L) {
    if (!s_cfg) return 0;
    lua_pushboolean(L, s_cfg->gui_open != 0);
    lua_pushnumber(L, s_cfg->gui_x);
    lua_pushnumber(L, s_cfg->gui_y);
    lua_pushnumber(L, s_cfg->gui_w);
    lua_pushnumber(L, s_cfg->gui_h);
    return 5;
}

const luaL_Reg kFunctions[] = {
    {"poly", Poly}, {"arc", Arc}, {"clip_push", ClipPush}, {"clip_pop", ClipPop}, {"gradient_rect", GradientRect},
    {"radial", Radial}, {"text", Text}, {"measure", Measure}, {"font_file", FontFile}, {"texture_file", TextureFile},
    {"texture_memory", TextureMemory}, {"texture_rgba", TextureRgba}, {"texture_draw", TextureDraw}, {"frame_info", FrameInfo},
    {"world_to_screen", WorldToScreen}, {"level_name", LevelName}, {"thirdperson", Thirdperson}, {"version", Version},
    {"sound", Sound}, {"notice", Notice}, {"bones", Bones}, {"raycast", Raycast}, {"damage", Damage}, {"entities", Entities},
    {"local_entities", LocalEntities}, {"entity_by_index", EntityByIndex}, {"entity_info", EntityInfo}, {"netvar", Netvar},
    {"find_pattern", FindPattern}, {"find_export", FindExport}, {"game_dir", GameDir}, {"user_name", UserName}, {"menu", Menu},
    {nullptr, nullptr},
};

}

void Register(lua_State* L, Settings* cfg) {
    s_cfg = cfg;
    s_clip_depth = 0;
    lua_newtable(L);
    for (const luaL_Reg* entry = kFunctions; entry->name; entry++) {
        lua_pushcfunction(L, entry->func);
        lua_setfield(L, -2, entry->name);
    }
    lua_setglobal(L, "_spx");
}

void BeginPaint(cairo_t* cr, const render::Camera& camera) {
    static gint64 last = 0;
    gint64 now = g_get_monotonic_time();
    s_frame_time = last ? (now - last) / 1e6 : 0.0;
    last = now;
    s_frame_count++;
    s_cr = cr;
    s_camera = &camera;
    s_clip_depth = 0;
}

void EndPaint() {
    while (s_clip_depth > 0 && s_cr) {
        cairo_restore(s_cr);
        s_clip_depth--;
    }
    s_cr = nullptr;
}

void DispatchGameEvents(lua_State* L) {
    std::vector<GameEventRecord> events;
    {
        std::lock_guard<std::mutex> lock(g_hud.game_events_mtx);
        events.swap(g_hud.game_events);
    }
    if (events.empty()) return;
    for (const GameEventRecord& event : events) {
        lua_getglobal(L, "_spx_dispatch_game_event");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            return;
        }
        lua_newtable(L);
        lua_pushstring(L, event.name.c_str());
        lua_setfield(L, -2, "_name");
        for (const auto& [key, value] : event.numbers) {
            lua_pushnumber(L, value);
            lua_setfield(L, -2, key.c_str());
        }
        for (const auto& [key, value] : event.strings) {
            lua_pushstring(L, value.c_str());
            lua_setfield(L, -2, key.c_str());
        }
        if (lua_pcall(L, 1, 0, 0) != 0) {
            fprintf(stderr, "[LUA] game_event: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
}

}
