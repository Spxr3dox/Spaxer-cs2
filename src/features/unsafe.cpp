#include "features.h"
#include "config/settings.h"
#include "sdk/game.h"
#include <cmath>

namespace features {

void ApplyUnsafe() {
    Settings* cfg = settings::Attach();
    if (!cfg || !g_proc.IsAlive()) return;
    uintptr_t pawn = game::LocalPawn();
    if (settings::Enabled(cfg->no_flash) && pawn && off::m_flFlashMaxAlpha)
        g_proc.Write(pawn + off::m_flFlashMaxAlpha, 0.f);

    bool thirdperson = settings::Enabled(cfg->thirdperson);
    if (pawn && off::m_bIsThirdPersonView) {
        bool current = g_proc.Read<bool>(pawn + off::m_bIsThirdPersonView);
        if (current != thirdperson) {
            g_proc.Write<bool>(pawn + off::m_bIsThirdPersonView, thirdperson);
        }
    }

    bool no_smoke = settings::Enabled(cfg->no_smoke);
    bool color_smoke = settings::Enabled(cfg->smoke_color_enabled);
    if (!no_smoke && !color_smoke) return;
    if (!off::m_bDidSmokeEffect || !off::m_vSmokeColor) return;

    uint32_t rgba = cfg->smoke_color_rgba;
    Vec3 wanted_color{
        static_cast<float>((rgba >> 24) & 0xFF) / 255.0f,
        static_cast<float>((rgba >> 16) & 0xFF) / 255.0f,
        static_cast<float>((rgba >> 8) & 0xFF) / 255.0f
    };

    uintptr_t list = game::EntityList();
    if (!list) return;
    for (int i = 64; i < 2048; i++) {
        uintptr_t entity = game::EntityFromList(list, i);
        if (!entity) continue;

        if (no_smoke) {
            uint8_t disabled = g_proc.Read<uint8_t>(entity + off::m_bDidSmokeEffect);
            if (!disabled) g_proc.Write<uint8_t>(entity + off::m_bDidSmokeEffect, 1);
        }

        if (color_smoke) {
            Vec3 current_color = g_proc.Read<Vec3>(entity + off::m_vSmokeColor);
            if (std::fabs(current_color.x - wanted_color.x) > 0.005f ||
                std::fabs(current_color.y - wanted_color.y) > 0.005f ||
                std::fabs(current_color.z - wanted_color.z) > 0.005f) {
                g_proc.Write<Vec3>(entity + off::m_vSmokeColor, wanted_color);
            }
        }
    }
}

}
