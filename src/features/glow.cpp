#include "features.h"
#include "config/settings.h"
#include "sdk/game.h"

namespace features {

static bool s_glow_written = false;

static uint32_t ToGameColor(uint32_t rgba) {
    uint32_t r = (rgba >> 24) & 0xFF, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF, a = rgba & 0xFF;
    return r | (g << 8) | (b << 16) | (a << 24);
}

static void WriteGlow(uintptr_t pawn, bool on, uint32_t color) {
    uintptr_t glow = pawn + off::m_Glow;
    if (on) {
        g_proc.Write<uint32_t>(glow + off::m_glowColorOverride, color);
        if (off::m_iGlowType) g_proc.Write<int32_t>(glow + off::m_iGlowType, 3);
    }
    if (g_proc.Read<bool>(glow + off::m_bGlowing) != on)
        g_proc.Write<bool>(glow + off::m_bGlowing, on);
}

void ApplyGlow() {
    Settings* cfg = settings::Attach();
    if (!cfg || !g_proc.IsAlive() || !off::m_Glow || !off::m_bGlowing || !off::m_glowColorOverride) return;
    bool enabled = settings::Enabled(cfg->glow);
    if (!enabled && !s_glow_written) return;
    uintptr_t list = game::EntityList();
    if (!list) return;
    uintptr_t local = game::LocalPawn();
    uint8_t my_team = local ? g_proc.Read<uint8_t>(local + off::m_iTeamNum) : 0;
    uint32_t enemy_color = ToGameColor(cfg->glow_enemy_rgba);
    uint32_t team_color = ToGameColor(cfg->glow_team_rgba);
    bool show_team = settings::Enabled(cfg->glow_team);
    for (int i = 1; i <= 64; i++) {
        uintptr_t controller = game::EntityFromList(list, i);
        if (!controller) continue;
        uint32_t handle = g_proc.Read<uint32_t>(controller + off::m_hPlayerPawn);
        if (!handle || handle == 0xFFFFFFFF) continue;
        uintptr_t pawn = game::EntityFromList(list, handle & 0x7FFF);
        if (!pawn || pawn == local) continue;
        int hp = g_proc.Read<int>(pawn + off::m_iHealth);
        uint8_t team = g_proc.Read<uint8_t>(pawn + off::m_iTeamNum);
        bool teammate = my_team && team == my_team;
        bool on = enabled && hp > 0 && (!teammate || show_team);
        WriteGlow(pawn, on, teammate ? team_color : enemy_color);
    }
    s_glow_written = enabled;
}

}
