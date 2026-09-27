#include "features.h"
#include "config/settings.h"
#include "sdk/game.h"

namespace features {

static bool s_tint_written = false;

static uint32_t ToRenderColor(uint32_t rgba) {
    uint32_t r = (rgba >> 24) & 0xFF, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF;
    return r | (g << 8) | (b << 16) | (0xFFu << 24);
}

void ApplyChams() {
    Settings* cfg = settings::Attach();
    if (!cfg || !g_proc.IsAlive() || !off::m_clrRender) return;
    bool enabled = settings::Enabled(cfg->chams) && settings::Enabled(cfg->chams_tint);
    if (!enabled && !s_tint_written) return;
    uintptr_t list = game::EntityList();
    if (!list) return;
    uintptr_t local = game::LocalPawn();
    int my_team = game::Team(local);
    uint32_t enemy_color = ToRenderColor(cfg->chams_visible_rgba);
    uint32_t team_color = ToRenderColor(cfg->chams_team_rgba);
    bool show_team = settings::Enabled(cfg->chams_team);
    for (int i = 1; i <= 64; i++) {
        uintptr_t controller = game::EntityFromList(list, i);
        if (!controller) continue;
        uint32_t handle = g_proc.Read<uint32_t>(controller + off::m_hPlayerPawn);
        if (!handle || handle == 0xFFFFFFFF) continue;
        uintptr_t pawn = game::EntityFromList(list, handle & 0x7FFF);
        if (!pawn || pawn == local) continue;
        bool teammate = my_team && game::Team(pawn) == my_team;
        bool on = enabled && g_proc.Read<int>(pawn + off::m_iHealth) > 0 && (!teammate || show_team);
        uint32_t wanted = on ? (teammate ? team_color : enemy_color) : 0xFFFFFFFFu;
        if (g_proc.Read<uint32_t>(pawn + off::m_clrRender) != wanted)
            g_proc.Write<uint32_t>(pawn + off::m_clrRender, wanted);
    }
    s_tint_written = enabled;
}

}
