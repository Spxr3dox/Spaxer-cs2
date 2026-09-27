#include "features.h"
#include "config/settings.h"
#include "sdk/game.h"

namespace features {

void ApplyRadarHack() {
    Settings* cfg = settings::Attach();
    if (!cfg || !settings::Enabled(cfg->radar_hack) || !g_proc.IsAlive()) return;
    if (!off::m_entitySpottedState || !off::m_bSpotted) return;
    uintptr_t list = game::EntityList();
    if (!list) return;
    uintptr_t local_controller = game::LocalController();
    if (!local_controller) return;
    int local_team = game::Team(local_controller);
    for (int i = 1; i <= 64; i++) {
        uintptr_t controller = game::EntityFromList(list, i);
        if (!controller) continue;
        int team = game::Team(controller);
        if (team == local_team || (team != 2 && team != 3)) continue;
        uint32_t pawn_h = g_proc.Read<uint32_t>(controller + off::m_hPlayerPawn);
        if (!pawn_h || pawn_h == 0xFFFFFFFF) continue;
        uintptr_t pawn = game::EntityFromList(list, pawn_h & 0x7FFF);
        if (!pawn) continue;
        int hp = g_proc.Read<int>(pawn + off::m_iHealth);
        if (hp <= 0 || hp > 100) continue;
        g_proc.Write<uint8_t>(pawn + off::m_entitySpottedState + off::m_bSpotted, 1);
    }
}

}
