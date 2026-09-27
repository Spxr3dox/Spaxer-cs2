#include "features/lua_engine.h"
#include "features/features.h"
#include "state.h"
#include "config/settings.h"
#include "memory/process.h"
#include "sdk/offsets.h"
#include "input/input.h"
#include "render/camera.h"
#include "sdk/game.h"
#include <X11/Xlib.h>
#include <lua.hpp>
#include <vector>
#include <string>
#include <filesystem>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <cstdlib>

static cairo_t* s_current_cr = nullptr;
static lua_State* s_L = nullptr;
static Settings* s_cfg = nullptr;
static render::Camera s_camera;
static bool s_reload_requested = false;
static uint32_t s_seen_reload_token = 0;
static std::chrono::steady_clock::time_point s_call_deadline;

static constexpr auto kCallBudget = std::chrono::milliseconds(50);
static constexpr int kHookInstructionCount = 10000;

static std::string ScriptsDir() {
    const char* home = getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.config/spaxer/scripts";
}

static void BudgetHook(lua_State* L, lua_Debug*) {
    if (std::chrono::steady_clock::now() > s_call_deadline)
        luaL_error(L, "script exceeded %d ms time budget", static_cast<int>(kCallBudget.count()));
}

static bool ProtectedCall(lua_State* L, int args, const char* context) {
    s_call_deadline = std::chrono::steady_clock::now() + kCallBudget;
    if (lua_pcall(L, args, 0, 0) == 0) return true;
    fprintf(stderr, "[LUA] %s: %s\n", context, lua_tostring(L, -1));
    lua_pop(L, 1);
    return false;
}

static void DispatchEvent(const char* event) {
    if (!s_L) return;
    lua_getfield(s_L, LUA_REGISTRYINDEX, "_SPX_CALLBACKS");
    if (!lua_istable(s_L, -1)) {
        lua_pop(s_L, 1);
        return;
    }
    lua_getfield(s_L, -1, event);
    if (lua_istable(s_L, -1)) {
        int len = static_cast<int>(lua_objlen(s_L, -1));
        for (int i = 1; i <= len; ++i) {
            lua_rawgeti(s_L, -1, i);
            if (!lua_isfunction(s_L, -1)) {
                lua_pop(s_L, 1);
                continue;
            }
            if (!ProtectedCall(s_L, 0, event)) {
                fprintf(stderr, "[LUA] %s callback #%d disabled after error\n", event, i);
                lua_pushboolean(s_L, 0);
                lua_rawseti(s_L, -2, i);
            }
        }
    }
    lua_pop(s_L, 2);
}

static int LuaClientRegisterCallback(lua_State* L) {
    const char* event = luaL_checkstring(L, 1);
    if (!lua_isfunction(L, 2)) return luaL_error(L, "callback must be a function");
    lua_getfield(L, LUA_REGISTRYINDEX, "_SPX_CALLBACKS");
    lua_getfield(L, -1, event);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, event);
    }
    int len = static_cast<int>(lua_objlen(L, -1));
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, len + 1);
    lua_pop(L, 2);
    return 0;
}

static int LuaClientLog(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    fprintf(stderr, "[LUA] %s\n", msg);
    return 0;
}

static bool RunFile(lua_State* L, const std::string& path) {
    if (luaL_loadfile(L, path.c_str()) != 0) {
        fprintf(stderr, "[LUA] %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return ProtectedCall(L, 0, path.c_str());
}

static int LuaClientLoadScript(lua_State* L) {
    lua_pushboolean(L, RunFile(L, luaL_checkstring(L, 1)));
    return 1;
}

static int LuaClientReload(lua_State*) {
    s_reload_requested = true;
    return 0;
}

static int LuaClientGetTime(lua_State* L) {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    lua_pushnumber(L, std::chrono::duration<double>(now).count());
    return 1;
}

static void GetColor(lua_State* L, int idx, double& r, double& g, double& b, double& a) {
    r = std::clamp(luaL_optnumber(L, idx, 255.0), 0.0, 255.0) / 255.0;
    g = std::clamp(luaL_optnumber(L, idx + 1, 255.0), 0.0, 255.0) / 255.0;
    b = std::clamp(luaL_optnumber(L, idx + 2, 255.0), 0.0, 255.0) / 255.0;
    a = std::clamp(luaL_optnumber(L, idx + 3, 255.0), 0.0, 255.0) / 255.0;
}

static int LuaRenderScreenSize(lua_State* L) {
    lua_pushnumber(L, g_hud.screen_w.load());
    lua_pushnumber(L, g_hud.screen_h.load());
    return 2;
}

static int LuaRenderText(lua_State* L) {
    if (!s_current_cr) return 0;
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);
    const char* text = luaL_checkstring(L, 3);
    double r, g, b, a;
    GetColor(L, 4, r, g, b, a);
    double size = luaL_optnumber(L, 8, 12.0);
    bool bold = lua_toboolean(L, 9);
    cairo_select_font_face(s_current_cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(s_current_cr, size);
    cairo_set_source_rgba(s_current_cr, r, g, b, a);
    cairo_font_extents_t fe;
    cairo_font_extents(s_current_cr, &fe);
    cairo_move_to(s_current_cr, std::round(x), std::round(y + fe.ascent));
    cairo_show_text(s_current_cr, text);
    return 0;
}

static int LuaRenderLine(lua_State* L) {
    if (!s_current_cr) return 0;
    double x1 = luaL_checknumber(L, 1);
    double y1 = luaL_checknumber(L, 2);
    double x2 = luaL_checknumber(L, 3);
    double y2 = luaL_checknumber(L, 4);
    double r, g, b, a;
    GetColor(L, 5, r, g, b, a);
    double width = luaL_optnumber(L, 9, 1.0);
    cairo_set_source_rgba(s_current_cr, r, g, b, a);
    cairo_set_line_width(s_current_cr, width);
    cairo_move_to(s_current_cr, x1, y1);
    cairo_line_to(s_current_cr, x2, y2);
    cairo_stroke(s_current_cr);
    return 0;
}

static int LuaRenderRect(lua_State* L) {
    if (!s_current_cr) return 0;
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);
    double w = luaL_checknumber(L, 3);
    double h = luaL_checknumber(L, 4);
    double r, g, b, a;
    GetColor(L, 5, r, g, b, a);
    double width = luaL_optnumber(L, 9, 1.0);
    double radius = luaL_optnumber(L, 10, 0.0);
    cairo_set_source_rgba(s_current_cr, r, g, b, a);
    cairo_set_line_width(s_current_cr, width);
    if (radius > 0.0) {
        double d = 3.14159265358979323846 / 180.0;
        cairo_new_sub_path(s_current_cr);
        cairo_arc(s_current_cr, x + w - radius, y + radius, radius, -90 * d, 0 * d);
        cairo_arc(s_current_cr, x + w - radius, y + h - radius, radius, 0 * d, 90 * d);
        cairo_arc(s_current_cr, x + radius, y + h - radius, radius, 90 * d, 180 * d);
        cairo_arc(s_current_cr, x + radius, y + radius, radius, 180 * d, 270 * d);
        cairo_close_path(s_current_cr);
    } else {
        cairo_rectangle(s_current_cr, x, y, w, h);
    }
    cairo_stroke(s_current_cr);
    return 0;
}

static int LuaRenderRectFilled(lua_State* L) {
    if (!s_current_cr) return 0;
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);
    double w = luaL_checknumber(L, 3);
    double h = luaL_checknumber(L, 4);
    double r, g, b, a;
    GetColor(L, 5, r, g, b, a);
    double radius = luaL_optnumber(L, 9, 0.0);
    cairo_set_source_rgba(s_current_cr, r, g, b, a);
    if (radius > 0.0) {
        double d = 3.14159265358979323846 / 180.0;
        cairo_new_sub_path(s_current_cr);
        cairo_arc(s_current_cr, x + w - radius, y + radius, radius, -90 * d, 0 * d);
        cairo_arc(s_current_cr, x + w - radius, y + h - radius, radius, 0 * d, 90 * d);
        cairo_arc(s_current_cr, x + radius, y + h - radius, radius, 90 * d, 180 * d);
        cairo_arc(s_current_cr, x + radius, y + radius, radius, 180 * d, 270 * d);
        cairo_close_path(s_current_cr);
    } else {
        cairo_rectangle(s_current_cr, x, y, w, h);
    }
    cairo_fill(s_current_cr);
    return 0;
}

static int LuaRenderCircle(lua_State* L) {
    if (!s_current_cr) return 0;
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);
    double rad = luaL_checknumber(L, 3);
    double r, g, b, a;
    GetColor(L, 4, r, g, b, a);
    double width = luaL_optnumber(L, 8, 1.0);
    cairo_set_source_rgba(s_current_cr, r, g, b, a);
    cairo_set_line_width(s_current_cr, width);
    cairo_arc(s_current_cr, x, y, rad, 0.0, 2.0 * 3.14159265358979323846);
    cairo_stroke(s_current_cr);
    return 0;
}

static int LuaRenderCircleFilled(lua_State* L) {
    if (!s_current_cr) return 0;
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);
    double rad = luaL_checknumber(L, 3);
    double r, g, b, a;
    GetColor(L, 4, r, g, b, a);
    cairo_set_source_rgba(s_current_cr, r, g, b, a);
    cairo_arc(s_current_cr, x, y, rad, 0.0, 2.0 * 3.14159265358979323846);
    cairo_fill(s_current_cr);
    return 0;
}

static int LuaRenderTriangle(lua_State* L) {
    if (!s_current_cr) return 0;
    double x1 = luaL_checknumber(L, 1);
    double y1 = luaL_checknumber(L, 2);
    double x2 = luaL_checknumber(L, 3);
    double y2 = luaL_checknumber(L, 4);
    double x3 = luaL_checknumber(L, 5);
    double y3 = luaL_checknumber(L, 6);
    double r, g, b, a;
    GetColor(L, 7, r, g, b, a);
    double width = luaL_optnumber(L, 11, 1.0);
    cairo_set_source_rgba(s_current_cr, r, g, b, a);
    cairo_set_line_width(s_current_cr, width);
    cairo_move_to(s_current_cr, x1, y1);
    cairo_line_to(s_current_cr, x2, y2);
    cairo_line_to(s_current_cr, x3, y3);
    cairo_close_path(s_current_cr);
    cairo_stroke(s_current_cr);
    return 0;
}

static int LuaRenderTriangleFilled(lua_State* L) {
    if (!s_current_cr) return 0;
    double x1 = luaL_checknumber(L, 1);
    double y1 = luaL_checknumber(L, 2);
    double x2 = luaL_checknumber(L, 3);
    double y2 = luaL_checknumber(L, 4);
    double x3 = luaL_checknumber(L, 5);
    double y3 = luaL_checknumber(L, 6);
    double r, g, b, a;
    GetColor(L, 7, r, g, b, a);
    cairo_set_source_rgba(s_current_cr, r, g, b, a);
    cairo_move_to(s_current_cr, x1, y1);
    cairo_line_to(s_current_cr, x2, y2);
    cairo_line_to(s_current_cr, x3, y3);
    cairo_close_path(s_current_cr);
    cairo_fill(s_current_cr);
    return 0;
}

static int LuaRenderMeasureText(lua_State* L) {
    if (!s_current_cr) return 0;
    const char* text = luaL_checkstring(L, 1);
    double size = luaL_optnumber(L, 2, 12.0);
    bool bold = lua_toboolean(L, 3);
    cairo_select_font_face(s_current_cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(s_current_cr, size);
    cairo_text_extents_t te;
    cairo_text_extents(s_current_cr, text, &te);
    lua_pushnumber(L, te.width);
    lua_pushnumber(L, te.height);
    return 2;
}

static int LuaRenderWorldToScreen(lua_State* L) {
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);
    double z = luaL_checknumber(L, 3);
    float sx = 0.f, sy = 0.f;
    if (!s_camera.valid || !s_camera.Project(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), sx, sy)) {
        lua_pushnil(L);
        lua_pushnil(L);
        lua_pushboolean(L, false);
        return 3;
    }
    bool on_screen = sx >= 0.f && sy >= 0.f && sx <= s_camera.width && sy <= s_camera.height;
    lua_pushnumber(L, sx);
    lua_pushnumber(L, sy);
    lua_pushboolean(L, on_screen);
    return 3;
}

static int LuaEngineIsInGame(lua_State* L) {
    lua_pushboolean(L, g_hud.in_game.load());
    return 1;
}

static int LuaEngineIsFocused(lua_State* L) {
    lua_pushboolean(L, g_hud.cs2_focused.load());
    return 1;
}

static int LuaEngineGetPing(lua_State* L) {
    lua_pushinteger(L, g_hud.local_ping.load());
    return 1;
}

static int LuaEngineGetFps(lua_State* L) {
    lua_pushinteger(L, g_hud.overlay_fps.load());
    return 1;
}

static int LuaEngineGetLocalTeam(lua_State* L) {
    lua_pushinteger(L, g_hud.local_team.load());
    return 1;
}

static int LuaEngineGetScreenSize(lua_State* L) {
    lua_pushinteger(L, g_hud.screen_w.load());
    lua_pushinteger(L, g_hud.screen_h.load());
    return 2;
}

static int LuaEntitiesGetPlayers(lua_State* L) {
    lua_newtable(L);
    std::vector<EspEntry> players;
    {
        std::lock_guard<std::mutex> lk(g_hud.esp_mtx);
        players = g_hud.esp_players;
    }
    for (size_t i = 0; i < players.size(); ++i) {
        const auto& p = players[i];
        lua_newtable(L);
        lua_pushnumber(L, static_cast<lua_Number>(p.pawn));
        lua_setfield(L, -2, "pawn");
        lua_pushstring(L, p.name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, p.weapon);
        lua_setfield(L, -2, "weapon");
        lua_pushstring(L, p.model);
        lua_setfield(L, -2, "model");
        lua_pushinteger(L, p.hp);
        lua_setfield(L, -2, "hp");
        lua_pushinteger(L, p.team);
        lua_setfield(L, -2, "team");
        lua_pushboolean(L, p.visible);
        lua_setfield(L, -2, "visible");
        lua_pushboolean(L, p.spotted_valid);
        lua_setfield(L, -2, "spotted");

        lua_newtable(L);
        lua_pushnumber(L, p.world_head_x);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, p.world_head_y);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, p.world_head_z);
        lua_setfield(L, -2, "z");
        lua_setfield(L, -2, "head_pos");

        lua_newtable(L);
        lua_pushnumber(L, p.world_feet_x);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, p.world_feet_y);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, p.world_feet_z);
        lua_setfield(L, -2, "z");
        lua_setfield(L, -2, "feet_pos");

        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

static int LuaEntitiesGetBomb(lua_State* L) {
    lua_newtable(L);
    lua_pushboolean(L, g_hud.bomb_visible.load());
    lua_setfield(L, -2, "visible");
    lua_pushnumber(L, g_hud.bomb_blow_secs.load());
    lua_setfield(L, -2, "blow_secs");
    lua_pushinteger(L, g_hud.bomb_site.load());
    lua_setfield(L, -2, "site");
    lua_pushboolean(L, g_hud.bomb_being_defused.load());
    lua_setfield(L, -2, "being_defused");
    lua_pushnumber(L, g_hud.bomb_defuse_secs.load());
    lua_setfield(L, -2, "defuse_secs");
    return 1;
}

static int LuaEntitiesGetSpectators(lua_State* L) {
    lua_newtable(L);
    std::vector<SpectatorEntry> specs;
    {
        std::lock_guard<std::mutex> lk(g_hud.spectators_mtx);
        specs = g_hud.spectators;
    }
    for (size_t i = 0; i < specs.size(); ++i) {
        lua_newtable(L);
        lua_pushstring(L, specs[i].name);
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, specs[i].team);
        lua_setfield(L, -2, "team");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

static int LuaEntitiesGetDroppedItems(lua_State* L) {
    lua_newtable(L);
    std::vector<DroppedItemEntry> items;
    {
        std::lock_guard<std::mutex> lk(g_hud.items_mtx);
        items = g_hud.dropped_items;
    }
    for (size_t i = 0; i < items.size(); ++i) {
        lua_newtable(L);
        lua_pushstring(L, items[i].name);
        lua_setfield(L, -2, "name");
        lua_pushnumber(L, items[i].world_x);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, items[i].world_y);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, items[i].world_z);
        lua_setfield(L, -2, "z");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

static int LuaMemoryReadString(lua_State* L) {
    uintptr_t addr = static_cast<uintptr_t>(luaL_checknumber(L, 1));
    size_t len = static_cast<size_t>(luaL_optinteger(L, 2, 64));
    lua_pushstring(L, g_proc.ReadString(addr, len).c_str());
    return 1;
}

static int LuaMemoryGetClientBase(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(off::g_ClientBase));
    return 1;
}

static int LuaMemoryGetEngineBase(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(off::g_EngineBase));
    return 1;
}

static int LuaInputIsKeyDown(lua_State* L) {
    int key = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, g_input.IsKeyDown(key));
    return 1;
}

static int LuaInputIsMouseDown(lua_State* L) {
    int btn = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, g_input.IsMouseDown(btn));
    return 1;
}

static int LuaInputMouseMove(lua_State* L) {
    int dx = static_cast<int>(luaL_checkinteger(L, 1));
    int dy = static_cast<int>(luaL_checkinteger(L, 2));
    g_input.MouseMove(dx, dy);
    return 0;
}

static int LuaInputClickLeft(lua_State*) {
    g_input.ClickLeft();
    return 0;
}

struct ScaledAlias {
    const char* name;
    const char* field;
    double scale;
};

static constexpr ScaledAlias kScaledAliases[] = {
    {"aimbot_fov", "aimbot_fov_x100", 100.0},
    {"aimbot_smooth", "aimbot_smooth_x100", 100.0},
    {"rcs_strength", "rcs_strength_x100", 100.0},
    {"trigger_fov", "trigger_fov_x100", 100.0},
};

static const ScaledAlias* FindAlias(const std::string& name) {
    for (const ScaledAlias& alias : kScaledAliases)
        if (name == alias.name) return &alias;
    return nullptr;
}

static uint32_t* FieldPointer(const settings::FieldInfo& field) {
    return reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(s_cfg) + field.offset);
}

static const char* KindName(settings::FieldKind kind) {
    switch (kind) {
        case settings::FieldKind::Toggle: return "toggle";
        case settings::FieldKind::Number: return "number";
        case settings::FieldKind::Color: return "color";
        case settings::FieldKind::Bind: return "bind";
    }
    return "unknown";
}

static const settings::FieldInfo& CheckField(lua_State* L, int index) {
    const char* name = luaL_checkstring(L, index);
    const settings::FieldInfo* field = settings::FindField(name);
    if (!field) luaL_error(L, "unknown setting '%s'", name);
    return *field;
}

static uint32_t ColorFromLua(lua_State* L, int index) {
    if (lua_istable(L, index)) {
        uint32_t channels[4] = {255, 255, 255, 255};
        for (int i = 0; i < 4; i++) {
            lua_rawgeti(L, index, i + 1);
            if (lua_isnumber(L, -1)) channels[i] = static_cast<uint32_t>(std::clamp<lua_Number>(lua_tonumber(L, -1), 0, 255));
            lua_pop(L, 1);
        }
        return (channels[0] << 24) | (channels[1] << 16) | (channels[2] << 8) | channels[3];
    }
    return static_cast<uint32_t>(luaL_checknumber(L, index));
}

static uint32_t BindFromLua(lua_State* L, int index) {
    if (lua_isnoneornil(L, index)) return 0;
    if (lua_isnumber(L, index)) return static_cast<uint32_t>(lua_tonumber(L, index));
    const char* name = luaL_checkstring(L, index);
    if (!*name) return 0;
    KeySym keysym = XStringToKeysym(name);
    if (keysym == NoSymbol) luaL_error(L, "unknown key name '%s' (use X11 names like F5, Insert, a)", name);
    return static_cast<uint32_t>(keysym);
}

static int LuaConfigGet(lua_State* L) {
    if (!s_cfg) return 0;
    std::string name = luaL_checkstring(L, 1);
    if (const ScaledAlias* alias = FindAlias(name)) {
        const settings::FieldInfo* field = settings::FindField(alias->field);
        lua_pushnumber(L, static_cast<int32_t>(*FieldPointer(*field)) / alias->scale);
        return 1;
    }
    const settings::FieldInfo& field = CheckField(L, 1);
    uint32_t value = __atomic_load_n(FieldPointer(field), __ATOMIC_RELAXED);
    switch (field.kind) {
        case settings::FieldKind::Toggle: lua_pushboolean(L, value != 0); break;
        case settings::FieldKind::Number: lua_pushnumber(L, static_cast<int32_t>(value)); break;
        case settings::FieldKind::Color: lua_pushnumber(L, value); break;
        case settings::FieldKind::Bind: {
            const char* name_text = value ? XKeysymToString(static_cast<KeySym>(value)) : nullptr;
            if (name_text) lua_pushstring(L, name_text);
            else lua_pushnil(L);
            break;
        }
    }
    return 1;
}

static int LuaConfigSet(lua_State* L) {
    if (!s_cfg) return 0;
    std::string name = luaL_checkstring(L, 1);
    if (const ScaledAlias* alias = FindAlias(name)) {
        const settings::FieldInfo* field = settings::FindField(alias->field);
        int32_t value = static_cast<int32_t>(std::lround(luaL_checknumber(L, 2) * alias->scale));
        __atomic_store_n(FieldPointer(*field), static_cast<uint32_t>(value), __ATOMIC_RELAXED);
        return 0;
    }
    const settings::FieldInfo& field = CheckField(L, 1);
    uint32_t value = 0;
    switch (field.kind) {
        case settings::FieldKind::Toggle: value = lua_toboolean(L, 2) ? 1u : 0u; break;
        case settings::FieldKind::Number: value = static_cast<uint32_t>(static_cast<int32_t>(std::lround(luaL_checknumber(L, 2)))); break;
        case settings::FieldKind::Color: value = ColorFromLua(L, 2); break;
        case settings::FieldKind::Bind: value = BindFromLua(L, 2); break;
    }
    __atomic_store_n(FieldPointer(field), value, __ATOMIC_RELAXED);
    return 0;
}

static int LuaConfigToggle(lua_State* L) {
    if (!s_cfg) return 0;
    const settings::FieldInfo& field = CheckField(L, 1);
    if (field.kind != settings::FieldKind::Toggle) return luaL_error(L, "'%s' is not a toggle", field.name);
    settings::ToggleEnabled(*FieldPointer(field));
    lua_pushboolean(L, settings::Enabled(*FieldPointer(field)));
    return 1;
}

static int LuaConfigList(lua_State* L) {
    lua_newtable(L);
    int index = 1;
    for (const settings::FieldInfo& field : settings::Fields()) {
        lua_newtable(L);
        lua_pushstring(L, field.name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, KindName(field.kind));
        lua_setfield(L, -2, "type");
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

static int LuaConfigSave(lua_State* L) {
    lua_pushboolean(L, settings::SaveConfig(luaL_checkstring(L, 1)));
    return 1;
}

static int LuaConfigLoad(lua_State* L) {
    lua_pushboolean(L, settings::LoadConfig(luaL_checkstring(L, 1)));
    return 1;
}

static int LuaConfigConfigs(lua_State* L) {
    lua_newtable(L);
    int index = 1;
    for (const std::string& name : settings::ListConfigs()) {
        lua_pushstring(L, name.c_str());
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

static uintptr_t CheckAddress(lua_State* L, int index) {
    return static_cast<uintptr_t>(luaL_checknumber(L, index));
}

template <typename T>
static int LuaReadNumber(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(g_proc.Read<T>(CheckAddress(L, 1))));
    return 1;
}

template <typename T>
static int LuaWriteNumber(lua_State* L) {
    T value = static_cast<T>(luaL_checknumber(L, 2));
    lua_pushboolean(L, g_proc.Write<T>(CheckAddress(L, 1), value));
    return 1;
}

static int LuaMemoryReadBool(lua_State* L) {
    lua_pushboolean(L, g_proc.Read<uint8_t>(CheckAddress(L, 1)) != 0);
    return 1;
}

static int LuaMemoryWriteBool(lua_State* L) {
    lua_pushboolean(L, g_proc.Write<uint8_t>(CheckAddress(L, 1), lua_toboolean(L, 2) ? 1 : 0));
    return 1;
}

static void PushVec3(lua_State* L, const Vec3& v) {
    lua_newtable(L);
    lua_pushnumber(L, v.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.z); lua_setfield(L, -2, "z");
}

static int LuaMemoryReadVec3(lua_State* L) {
    PushVec3(L, g_proc.Read<Vec3>(CheckAddress(L, 1)));
    return 1;
}

static int LuaMemoryWriteVec3(lua_State* L) {
    luaL_checktype(L, 2, LUA_TTABLE);
    Vec3 v{};
    lua_getfield(L, 2, "x"); v.x = static_cast<float>(luaL_optnumber(L, -1, 0)); lua_pop(L, 1);
    lua_getfield(L, 2, "y"); v.y = static_cast<float>(luaL_optnumber(L, -1, 0)); lua_pop(L, 1);
    lua_getfield(L, 2, "z"); v.z = static_cast<float>(luaL_optnumber(L, -1, 0)); lua_pop(L, 1);
    lua_pushboolean(L, g_proc.Write<Vec3>(CheckAddress(L, 1), v));
    return 1;
}

static int LuaOffsetsGet(lua_State* L) {
    uintptr_t value = 0;
    if (off::Get(luaL_checkstring(L, 1), value) && value) lua_pushnumber(L, static_cast<lua_Number>(value));
    else lua_pushnil(L);
    return 1;
}

static int LuaOffsetsList(lua_State* L) {
    lua_newtable(L);
    int index = 1;
    for (const std::string& name : off::Names()) {
        lua_pushstring(L, name.c_str());
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

static int LuaEntitiesGetLocal(lua_State* L) {
    uintptr_t pawn = game::LocalPawn();
    if (!pawn) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_pushnumber(L, static_cast<lua_Number>(pawn)); lua_setfield(L, -2, "pawn");
    lua_pushnumber(L, static_cast<lua_Number>(game::LocalController())); lua_setfield(L, -2, "controller");
    lua_pushinteger(L, g_proc.Read<int>(pawn + off::m_iHealth)); lua_setfield(L, -2, "hp");
    lua_pushinteger(L, game::Team(pawn)); lua_setfield(L, -2, "team");
    lua_pushinteger(L, game::ActiveWeaponDefinitionIndex(pawn)); lua_setfield(L, -2, "weapon_id");
    lua_pushboolean(L, off::m_bIsScoped && g_proc.Read<bool>(pawn + off::m_bIsScoped)); lua_setfield(L, -2, "scoped");
    uint32_t flags = off::m_fFlags ? g_proc.Read<uint32_t>(pawn + off::m_fFlags) : 0;
    lua_pushboolean(L, (flags & 1u) != 0); lua_setfield(L, -2, "on_ground");
    lua_pushboolean(L, (flags & 2u) != 0); lua_setfield(L, -2, "crouching");
    PushVec3(L, game::Origin(pawn)); lua_setfield(L, -2, "origin");
    PushVec3(L, game::EyePosition(pawn)); lua_setfield(L, -2, "eye");
    PushVec3(L, off::m_angEyeAngles ? g_proc.Read<Vec3>(pawn + off::m_angEyeAngles) : Vec3{}); lua_setfield(L, -2, "angles");
    PushVec3(L, off::m_vecVelocity ? g_proc.Read<Vec3>(pawn + off::m_vecVelocity) : Vec3{}); lua_setfield(L, -2, "velocity");
    return 1;
}

static int LuaEntitiesGetEntity(lua_State* L) {
    int index = static_cast<int>(luaL_checkinteger(L, 1));
    uintptr_t entity = game::EntityFromList(game::EntityList(), index);
    if (entity) lua_pushnumber(L, static_cast<lua_Number>(entity));
    else lua_pushnil(L);
    return 1;
}

static int LuaInputSetKey(lua_State* L) {
    g_input.SetKey(static_cast<int>(luaL_checkinteger(L, 1)), lua_toboolean(L, 2));
    return 0;
}

static int LuaInputPressKey(lua_State* L) {
    int code = static_cast<int>(luaL_checkinteger(L, 1));
    g_input.SetKey(code, true);
    g_input.SetKey(code, false);
    return 0;
}

static int LuaInputSetMouseButton(lua_State* L) {
    g_input.SetMouseButton(static_cast<int>(luaL_checkinteger(L, 1)), lua_toboolean(L, 2));
    return 0;
}

static int LuaInputClick(lua_State* L) {
    int button = static_cast<int>(luaL_optinteger(L, 1, 1));
    g_input.SetMouseButton(button, true);
    g_input.SetMouseButton(button, false);
    return 0;
}

static int LuaClientScriptDir(lua_State* L) {
    lua_pushstring(L, ScriptsDir().c_str());
    return 1;
}

struct KeyName {
    const char* name;
    int code;
};

static constexpr KeyName kKeyNames[] = {
#include "features/lua_keys.inc"
};

static void RegisterKeys(lua_State* L) {
    lua_newtable(L);
    for (const KeyName& key : kKeyNames) {
        lua_pushinteger(L, key.code);
        lua_setfield(L, -2, key.name);
    }
    lua_setglobal(L, "keys");
}

static const char* kPrelude = R"lua(
local timers, binds, next_id = {}, {}, 0

local function new_id()
    next_id = next_id + 1
    return next_id
end

local function schedule(seconds, fn, interval)
    assert(type(fn) == "function", "callback must be a function")
    local id = new_id()
    timers[id] = { at = client.get_time() + seconds, fn = fn, interval = interval }
    return id
end

function client.delay(seconds, fn) return schedule(seconds, fn, nil) end
function client.every(seconds, fn) return schedule(seconds, fn, math.max(seconds, 0.001)) end
function client.cancel(id) timers[id] = nil end

local function resolve_key(key)
    if type(key) == "number" then return "key", key end
    assert(type(key) == "string", "key must be a name or a code")
    local upper = key:upper()
    local mouse = upper:match("^MOUSE(%d)$")
    if mouse then return "mouse", tonumber(mouse) end
    local code = keys[upper]
    assert(code, "unknown key '" .. key .. "'")
    return "key", code
end

local function is_down(bind)
    if bind.kind == "mouse" then return input.is_mouse_down(bind.code) end
    return input.is_key_down(bind.code)
end

function input.bind(key, fn, mode, global)
    assert(type(fn) == "function", "callback must be a function")
    mode = mode or "press"
    assert(mode == "press" or mode == "release" or mode == "hold" or mode == "toggle", "mode must be press, release, hold or toggle")
    local kind, code = resolve_key(key)
    local id = new_id()
    binds[id] = { kind = kind, code = code, fn = fn, mode = mode, global = global == true, down = false, state = false }
    return id
end

function input.unbind(id) binds[id] = nil end

function input.is_pressed(key)
    local kind, code = resolve_key(key)
    return is_down({ kind = kind, code = code })
end

local function run(fn, what, ...)
    local ok, err = pcall(fn, ...)
    if not ok then client.log(what .. " error: " .. tostring(err)) end
    return ok
end

client.register_callback("frame", function()
    local now = client.get_time()
    local due = {}
    for id, timer in pairs(timers) do
        if now >= timer.at then due[#due + 1] = id end
    end
    for _, id in ipairs(due) do
        local timer = timers[id]
        if timer then
            if timer.interval then timer.at = now + timer.interval else timers[id] = nil end
            if not run(timer.fn, "timer") then timers[id] = nil end
        end
    end

    local focused = engine.is_focused()
    local fired = {}
    for id, bind in pairs(binds) do
        local down = (bind.global or focused) and is_down(bind)
        if down ~= bind.down then
            bind.down = down
            fired[#fired + 1] = { id = id, down = down }
        end
    end
    for _, event in ipairs(fired) do
        local bind = binds[event.id]
        if bind then
            local ok = true
            if bind.mode == "press" and event.down then ok = run(bind.fn, "bind")
            elseif bind.mode == "release" and not event.down then ok = run(bind.fn, "bind")
            elseif bind.mode == "hold" then ok = run(bind.fn, "bind", event.down)
            elseif bind.mode == "toggle" and event.down then
                bind.state = not bind.state
                ok = run(bind.fn, "bind", bind.state)
            end
            if not ok then binds[event.id] = nil end
        end
    end
end)
)lua";

static void LoadScriptsFromDir(const std::string& dir) {
    std::error_code error;
    std::vector<std::string> paths;
    for (const auto& entry : std::filesystem::directory_iterator(dir, error))
        if (entry.is_regular_file() && entry.path().extension() == ".lua") paths.push_back(entry.path().string());
    std::sort(paths.begin(), paths.end());
    for (const std::string& path : paths) RunFile(s_L, path);
}

namespace lua_engine {

void Init(Settings* cfg) {
    s_cfg = cfg;
    if (cfg) s_seen_reload_token = __atomic_load_n(&cfg->lua_reload_token, __ATOMIC_RELAXED);
    if (s_L) Shutdown();
    s_L = luaL_newstate();
    if (!s_L) return;
    luaL_openlibs(s_L);
    luaJIT_setmode(s_L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
    lua_sethook(s_L, BudgetHook, LUA_MASKCOUNT, kHookInstructionCount);

    lua_newtable(s_L);
    lua_setfield(s_L, LUA_REGISTRYINDEX, "_SPX_CALLBACKS");

    lua_newtable(s_L);
    lua_pushcfunction(s_L, LuaClientRegisterCallback); lua_setfield(s_L, -2, "register_callback");
    lua_pushcfunction(s_L, LuaClientLog);              lua_setfield(s_L, -2, "log");
    lua_pushcfunction(s_L, LuaClientLoadScript);       lua_setfield(s_L, -2, "load_script");
    lua_pushcfunction(s_L, LuaClientReload);           lua_setfield(s_L, -2, "reload");
    lua_pushcfunction(s_L, LuaClientGetTime);          lua_setfield(s_L, -2, "get_time");
    lua_pushcfunction(s_L, LuaClientScriptDir);        lua_setfield(s_L, -2, "script_dir");
    lua_setglobal(s_L, "client");

    lua_newtable(s_L);
    lua_pushcfunction(s_L, LuaRenderScreenSize);       lua_setfield(s_L, -2, "screen_size");
    lua_pushcfunction(s_L, LuaRenderText);             lua_setfield(s_L, -2, "text");
    lua_pushcfunction(s_L, LuaRenderLine);             lua_setfield(s_L, -2, "line");
    lua_pushcfunction(s_L, LuaRenderRect);             lua_setfield(s_L, -2, "rect");
    lua_pushcfunction(s_L, LuaRenderRectFilled);       lua_setfield(s_L, -2, "rect_filled");
    lua_pushcfunction(s_L, LuaRenderCircle);           lua_setfield(s_L, -2, "circle");
    lua_pushcfunction(s_L, LuaRenderCircleFilled);     lua_setfield(s_L, -2, "circle_filled");
    lua_pushcfunction(s_L, LuaRenderTriangle);         lua_setfield(s_L, -2, "triangle");
    lua_pushcfunction(s_L, LuaRenderTriangleFilled);   lua_setfield(s_L, -2, "triangle_filled");
    lua_pushcfunction(s_L, LuaRenderMeasureText);      lua_setfield(s_L, -2, "measure_text");
    lua_pushcfunction(s_L, LuaRenderWorldToScreen);    lua_setfield(s_L, -2, "world_to_screen");
    lua_setglobal(s_L, "render");

    lua_newtable(s_L);
    lua_pushcfunction(s_L, LuaEngineIsInGame);         lua_setfield(s_L, -2, "is_in_game");
    lua_pushcfunction(s_L, LuaEngineIsFocused);        lua_setfield(s_L, -2, "is_focused");
    lua_pushcfunction(s_L, LuaEngineGetPing);          lua_setfield(s_L, -2, "get_ping");
    lua_pushcfunction(s_L, LuaEngineGetFps);           lua_setfield(s_L, -2, "get_fps");
    lua_pushcfunction(s_L, LuaEngineGetLocalTeam);     lua_setfield(s_L, -2, "get_local_team");
    lua_pushcfunction(s_L, LuaEngineGetScreenSize);    lua_setfield(s_L, -2, "get_screen_size");
    lua_setglobal(s_L, "engine");

    lua_newtable(s_L);
    lua_pushcfunction(s_L, LuaEntitiesGetPlayers);      lua_setfield(s_L, -2, "get_players");
    lua_pushcfunction(s_L, LuaEntitiesGetBomb);         lua_setfield(s_L, -2, "get_bomb");
    lua_pushcfunction(s_L, LuaEntitiesGetSpectators);   lua_setfield(s_L, -2, "get_spectators");
    lua_pushcfunction(s_L, LuaEntitiesGetDroppedItems); lua_setfield(s_L, -2, "get_dropped_items");
    lua_pushcfunction(s_L, LuaEntitiesGetLocal);        lua_setfield(s_L, -2, "get_local");
    lua_pushcfunction(s_L, LuaEntitiesGetEntity);       lua_setfield(s_L, -2, "get_entity");
    lua_setglobal(s_L, "entities");

    lua_newtable(s_L);
    lua_pushcfunction(s_L, LuaReadNumber<uint8_t>);    lua_setfield(s_L, -2, "read_u8");
    lua_pushcfunction(s_L, LuaReadNumber<int32_t>);    lua_setfield(s_L, -2, "read_i32");
    lua_pushcfunction(s_L, LuaReadNumber<uint32_t>);   lua_setfield(s_L, -2, "read_u32");
    lua_pushcfunction(s_L, LuaReadNumber<int64_t>);    lua_setfield(s_L, -2, "read_i64");
    lua_pushcfunction(s_L, LuaReadNumber<uint64_t>);   lua_setfield(s_L, -2, "read_u64");
    lua_pushcfunction(s_L, LuaReadNumber<uint64_t>);   lua_setfield(s_L, -2, "read_ptr");
    lua_pushcfunction(s_L, LuaReadNumber<float>);      lua_setfield(s_L, -2, "read_float");
    lua_pushcfunction(s_L, LuaMemoryReadBool);         lua_setfield(s_L, -2, "read_bool");
    lua_pushcfunction(s_L, LuaMemoryReadVec3);         lua_setfield(s_L, -2, "read_vec3");
    lua_pushcfunction(s_L, LuaMemoryReadString);       lua_setfield(s_L, -2, "read_string");
    lua_pushcfunction(s_L, LuaWriteNumber<uint8_t>);   lua_setfield(s_L, -2, "write_u8");
    lua_pushcfunction(s_L, LuaWriteNumber<int32_t>);   lua_setfield(s_L, -2, "write_i32");
    lua_pushcfunction(s_L, LuaWriteNumber<uint32_t>);  lua_setfield(s_L, -2, "write_u32");
    lua_pushcfunction(s_L, LuaWriteNumber<uint64_t>);  lua_setfield(s_L, -2, "write_u64");
    lua_pushcfunction(s_L, LuaWriteNumber<float>);     lua_setfield(s_L, -2, "write_float");
    lua_pushcfunction(s_L, LuaMemoryWriteBool);        lua_setfield(s_L, -2, "write_bool");
    lua_pushcfunction(s_L, LuaMemoryWriteVec3);        lua_setfield(s_L, -2, "write_vec3");
    lua_pushcfunction(s_L, LuaMemoryGetClientBase);    lua_setfield(s_L, -2, "get_client_base");
    lua_pushcfunction(s_L, LuaMemoryGetEngineBase);    lua_setfield(s_L, -2, "get_engine_base");
    lua_setglobal(s_L, "memory");

    lua_newtable(s_L);
    lua_pushcfunction(s_L, LuaInputIsKeyDown);         lua_setfield(s_L, -2, "is_key_down");
    lua_pushcfunction(s_L, LuaInputIsMouseDown);       lua_setfield(s_L, -2, "is_mouse_down");
    lua_pushcfunction(s_L, LuaInputMouseMove);         lua_setfield(s_L, -2, "mouse_move");
    lua_pushcfunction(s_L, LuaInputClickLeft);         lua_setfield(s_L, -2, "click_left");
    lua_pushcfunction(s_L, LuaInputSetKey);            lua_setfield(s_L, -2, "set_key");
    lua_pushcfunction(s_L, LuaInputPressKey);          lua_setfield(s_L, -2, "press_key");
    lua_pushcfunction(s_L, LuaInputSetMouseButton);    lua_setfield(s_L, -2, "set_mouse_button");
    lua_pushcfunction(s_L, LuaInputClick);             lua_setfield(s_L, -2, "click");
    lua_setglobal(s_L, "input");

    lua_newtable(s_L);
    lua_pushcfunction(s_L, LuaConfigGet);              lua_setfield(s_L, -2, "get");
    lua_pushcfunction(s_L, LuaConfigSet);              lua_setfield(s_L, -2, "set");
    lua_pushcfunction(s_L, LuaConfigToggle);           lua_setfield(s_L, -2, "toggle");
    lua_pushcfunction(s_L, LuaConfigList);             lua_setfield(s_L, -2, "list");
    lua_pushcfunction(s_L, LuaConfigSave);             lua_setfield(s_L, -2, "save");
    lua_pushcfunction(s_L, LuaConfigLoad);             lua_setfield(s_L, -2, "load");
    lua_pushcfunction(s_L, LuaConfigConfigs);          lua_setfield(s_L, -2, "configs");
    lua_setglobal(s_L, "config");

    lua_newtable(s_L);
    lua_pushcfunction(s_L, LuaOffsetsGet);             lua_setfield(s_L, -2, "get");
    lua_pushcfunction(s_L, LuaOffsetsList);            lua_setfield(s_L, -2, "list");
    lua_setglobal(s_L, "offsets");

    RegisterKeys(s_L);
    if (luaL_loadstring(s_L, kPrelude) != 0) {
        fprintf(stderr, "[LUA] prelude: %s\n", lua_tostring(s_L, -1));
        lua_pop(s_L, 1);
    } else {
        ProtectedCall(s_L, 0, "prelude");
    }

    std::error_code error;
    std::filesystem::create_directories(ScriptsDir(), error);
    LoadScriptsFromDir(ScriptsDir());
}

void Shutdown() {
    if (!s_L) return;
    DispatchEvent("unload");
    lua_close(s_L);
    s_L = nullptr;
}

void Reload() {
    Settings* cfg = s_cfg;
    Shutdown();
    Init(cfg);
}

static void ApplyPendingReload() {
    if (!s_reload_requested) return;
    s_reload_requested = false;
    Reload();
}

void DispatchPaint(cairo_t* cr, const render::Camera& camera) {
    ApplyPendingReload();
    s_camera = camera;
    if (!s_L || !cr) return;
    s_current_cr = cr;
    DispatchEvent("paint");
    s_current_cr = nullptr;
}

void DispatchFrame() {
    if (s_cfg) {
        uint32_t token = __atomic_load_n(&s_cfg->lua_reload_token, __ATOMIC_RELAXED);
        if (token != s_seen_reload_token) {
            s_seen_reload_token = token;
            s_reload_requested = true;
        }
    }
    ApplyPendingReload();
    if (!s_L) return;
    DispatchEvent("frame");
}

void DispatchTick() {
    ApplyPendingReload();
    if (!s_L) return;
    DispatchEvent("tick");
}

}

namespace features {

void InitLua(Settings* cfg) {
    lua_engine::Init(cfg);
}

void ShutdownLua() {
    lua_engine::Shutdown();
}

void ReloadLua() {
    lua_engine::Reload();
}

void PaintLua(cairo_t* cr, const render::Camera& camera) {
    lua_engine::DispatchPaint(cr, camera);
}

void TickLua() {
    lua_engine::DispatchTick();
}

void FrameLua() {
    lua_engine::DispatchFrame();
}

}
