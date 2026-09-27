#include "features/lua_engine.h"
#include "features/features.h"
#include "state.h"
#include "config/settings.h"
#include "memory/process.h"
#include "sdk/offsets.h"
#include "input/input.h"
#include "render/camera.h"
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
    printf("[LUA] %s\n", msg);
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

static int LuaMemoryReadI32(lua_State* L) {
    uintptr_t addr = static_cast<uintptr_t>(luaL_checknumber(L, 1));
    lua_pushinteger(L, g_proc.Read<int32_t>(addr));
    return 1;
}

static int LuaMemoryReadU32(lua_State* L) {
    uintptr_t addr = static_cast<uintptr_t>(luaL_checknumber(L, 1));
    lua_pushnumber(L, g_proc.Read<uint32_t>(addr));
    return 1;
}

static int LuaMemoryReadI64(lua_State* L) {
    uintptr_t addr = static_cast<uintptr_t>(luaL_checknumber(L, 1));
    lua_pushnumber(L, static_cast<lua_Number>(g_proc.Read<int64_t>(addr)));
    return 1;
}

static int LuaMemoryReadU64(lua_State* L) {
    uintptr_t addr = static_cast<uintptr_t>(luaL_checknumber(L, 1));
    lua_pushnumber(L, static_cast<lua_Number>(g_proc.Read<uint64_t>(addr)));
    return 1;
}

static int LuaMemoryReadFloat(lua_State* L) {
    uintptr_t addr = static_cast<uintptr_t>(luaL_checknumber(L, 1));
    lua_pushnumber(L, g_proc.Read<float>(addr));
    return 1;
}

static int LuaMemoryReadString(lua_State* L) {
    uintptr_t addr = static_cast<uintptr_t>(luaL_checknumber(L, 1));
    size_t len = static_cast<size_t>(luaL_optinteger(L, 2, 64));
    lua_pushstring(L, g_proc.ReadString(addr, len).c_str());
    return 1;
}

static int LuaMemoryWriteI32(lua_State* L) {
    uintptr_t addr = static_cast<uintptr_t>(luaL_checknumber(L, 1));
    int32_t val = static_cast<int32_t>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, g_proc.Write<int32_t>(addr, val));
    return 1;
}

static int LuaMemoryWriteFloat(lua_State* L) {
    uintptr_t addr = static_cast<uintptr_t>(luaL_checknumber(L, 1));
    float val = static_cast<float>(luaL_checknumber(L, 2));
    lua_pushboolean(L, g_proc.Write<float>(addr, val));
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

static int LuaConfigGet(lua_State* L) {
    if (!s_cfg) return 0;
    std::string key = luaL_checkstring(L, 1);
    if (key == "esp") lua_pushboolean(L, settings::Enabled(s_cfg->esp));
    else if (key == "esp_box") lua_pushboolean(L, settings::Enabled(s_cfg->esp_box));
    else if (key == "esp_health") lua_pushboolean(L, settings::Enabled(s_cfg->esp_health));
    else if (key == "esp_name") lua_pushboolean(L, settings::Enabled(s_cfg->esp_name));
    else if (key == "esp_weapon") lua_pushboolean(L, settings::Enabled(s_cfg->esp_weapon));
    else if (key == "esp_skeleton") lua_pushboolean(L, settings::Enabled(s_cfg->esp_skeleton));
    else if (key == "esp_head_circle") lua_pushboolean(L, settings::Enabled(s_cfg->esp_head_circle));
    else if (key == "aimbot_enabled") lua_pushboolean(L, settings::Enabled(s_cfg->aimbot_enabled));
    else if (key == "aimbot_fov") lua_pushnumber(L, s_cfg->aimbot_fov_x100 / 100.0);
    else if (key == "aimbot_smooth") lua_pushnumber(L, s_cfg->aimbot_smooth_x100 / 100.0);
    else if (key == "trigger_enabled") lua_pushboolean(L, settings::Enabled(s_cfg->trigger_enabled));
    else if (key == "trigger_delay_ms") lua_pushinteger(L, s_cfg->trigger_delay_ms);
    else if (key == "rcs_enabled") lua_pushboolean(L, settings::Enabled(s_cfg->rcs_enabled));
    else if (key == "rcs_strength") lua_pushnumber(L, s_cfg->rcs_strength_x100 / 100.0);
    else if (key == "bunnyhop") lua_pushboolean(L, settings::Enabled(s_cfg->bunnyhop));
    else if (key == "auto_strafe") lua_pushboolean(L, settings::Enabled(s_cfg->auto_strafe));
    else if (key == "snap_tap") lua_pushboolean(L, settings::Enabled(s_cfg->snap_tap));
    else if (key == "watermark") lua_pushboolean(L, settings::Enabled(s_cfg->watermark));
    else if (key == "bomb_timer") lua_pushboolean(L, settings::Enabled(s_cfg->bomb_timer));
    else if (key == "crosshair") lua_pushboolean(L, settings::Enabled(s_cfg->crosshair));
    else if (key == "chams") lua_pushboolean(L, settings::Enabled(s_cfg->chams));
    else if (key == "glow") lua_pushboolean(L, settings::Enabled(s_cfg->glow));
    else if (key == "sound_esp") lua_pushboolean(L, settings::Enabled(s_cfg->sound_esp));
    else if (key == "hitmarker") lua_pushboolean(L, settings::Enabled(s_cfg->hitmarker));
    else if (key == "radar_hack") lua_pushboolean(L, settings::Enabled(s_cfg->radar_hack));
    else lua_pushnil(L);
    return 1;
}

static int LuaConfigSet(lua_State* L) {
    if (!s_cfg) return 0;
    std::string key = luaL_checkstring(L, 1);
    if (key == "esp") settings::SetEnabled(s_cfg->esp, lua_toboolean(L, 2));
    else if (key == "esp_box") settings::SetEnabled(s_cfg->esp_box, lua_toboolean(L, 2));
    else if (key == "esp_health") settings::SetEnabled(s_cfg->esp_health, lua_toboolean(L, 2));
    else if (key == "esp_name") settings::SetEnabled(s_cfg->esp_name, lua_toboolean(L, 2));
    else if (key == "esp_weapon") settings::SetEnabled(s_cfg->esp_weapon, lua_toboolean(L, 2));
    else if (key == "esp_skeleton") settings::SetEnabled(s_cfg->esp_skeleton, lua_toboolean(L, 2));
    else if (key == "esp_head_circle") settings::SetEnabled(s_cfg->esp_head_circle, lua_toboolean(L, 2));
    else if (key == "aimbot_enabled") settings::SetEnabled(s_cfg->aimbot_enabled, lua_toboolean(L, 2));
    else if (key == "aimbot_fov") s_cfg->aimbot_fov_x100 = static_cast<int32_t>(luaL_checknumber(L, 2) * 100.0);
    else if (key == "aimbot_smooth") s_cfg->aimbot_smooth_x100 = static_cast<int32_t>(luaL_checknumber(L, 2) * 100.0);
    else if (key == "trigger_enabled") settings::SetEnabled(s_cfg->trigger_enabled, lua_toboolean(L, 2));
    else if (key == "trigger_delay_ms") s_cfg->trigger_delay_ms = static_cast<int32_t>(luaL_checkinteger(L, 2));
    else if (key == "rcs_enabled") settings::SetEnabled(s_cfg->rcs_enabled, lua_toboolean(L, 2));
    else if (key == "rcs_strength") s_cfg->rcs_strength_x100 = static_cast<int32_t>(luaL_checknumber(L, 2) * 100.0);
    else if (key == "bunnyhop") settings::SetEnabled(s_cfg->bunnyhop, lua_toboolean(L, 2));
    else if (key == "auto_strafe") settings::SetEnabled(s_cfg->auto_strafe, lua_toboolean(L, 2));
    else if (key == "snap_tap") settings::SetEnabled(s_cfg->snap_tap, lua_toboolean(L, 2));
    else if (key == "watermark") settings::SetEnabled(s_cfg->watermark, lua_toboolean(L, 2));
    else if (key == "bomb_timer") settings::SetEnabled(s_cfg->bomb_timer, lua_toboolean(L, 2));
    else if (key == "crosshair") settings::SetEnabled(s_cfg->crosshair, lua_toboolean(L, 2));
    else if (key == "chams") settings::SetEnabled(s_cfg->chams, lua_toboolean(L, 2));
    else if (key == "glow") settings::SetEnabled(s_cfg->glow, lua_toboolean(L, 2));
    else if (key == "sound_esp") settings::SetEnabled(s_cfg->sound_esp, lua_toboolean(L, 2));
    else if (key == "hitmarker") settings::SetEnabled(s_cfg->hitmarker, lua_toboolean(L, 2));
    else if (key == "radar_hack") settings::SetEnabled(s_cfg->radar_hack, lua_toboolean(L, 2));
    return 0;
}

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
    lua_setglobal(s_L, "entities");

    lua_newtable(s_L);
    lua_pushcfunction(s_L, LuaMemoryReadI32);          lua_setfield(s_L, -2, "read_i32");
    lua_pushcfunction(s_L, LuaMemoryReadU32);          lua_setfield(s_L, -2, "read_u32");
    lua_pushcfunction(s_L, LuaMemoryReadI64);          lua_setfield(s_L, -2, "read_i64");
    lua_pushcfunction(s_L, LuaMemoryReadU64);          lua_setfield(s_L, -2, "read_u64");
    lua_pushcfunction(s_L, LuaMemoryReadFloat);        lua_setfield(s_L, -2, "read_float");
    lua_pushcfunction(s_L, LuaMemoryReadString);       lua_setfield(s_L, -2, "read_string");
    lua_pushcfunction(s_L, LuaMemoryWriteI32);         lua_setfield(s_L, -2, "write_i32");
    lua_pushcfunction(s_L, LuaMemoryWriteFloat);       lua_setfield(s_L, -2, "write_float");
    lua_pushcfunction(s_L, LuaMemoryGetClientBase);    lua_setfield(s_L, -2, "get_client_base");
    lua_pushcfunction(s_L, LuaMemoryGetEngineBase);    lua_setfield(s_L, -2, "get_engine_base");
    lua_setglobal(s_L, "memory");

    lua_newtable(s_L);
    lua_pushcfunction(s_L, LuaInputIsKeyDown);         lua_setfield(s_L, -2, "is_key_down");
    lua_pushcfunction(s_L, LuaInputIsMouseDown);       lua_setfield(s_L, -2, "is_mouse_down");
    lua_pushcfunction(s_L, LuaInputMouseMove);         lua_setfield(s_L, -2, "mouse_move");
    lua_pushcfunction(s_L, LuaInputClickLeft);         lua_setfield(s_L, -2, "click_left");
    lua_setglobal(s_L, "input");

    lua_newtable(s_L);
    lua_pushcfunction(s_L, LuaConfigGet);              lua_setfield(s_L, -2, "get");
    lua_pushcfunction(s_L, LuaConfigSet);              lua_setfield(s_L, -2, "set");
    lua_setglobal(s_L, "config");

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

}
