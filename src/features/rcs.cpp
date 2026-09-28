#include "features.h"
#include "config/settings.h"
#include "input/input.h"
#include "sdk/game.h"
#include "state.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

namespace features {

static std::atomic<bool> s_running{false};
static std::thread s_thread;

static bool IsRcsWeapon(int definition) {
    switch (definition) {
        case 7:
        case 8:
        case 10:
        case 13:
        case 16:
        case 28:
        case 39:
        case 60:
            return true;
        default:
            return false;
    }
}

constexpr float kViewPunchScale = 2.f;

static void Loop() {
    Settings* cfg = settings::Attach();
    if (!cfg) return;

    Vec3 previous{};
    float remainder_x = 0.f, remainder_y = 0.f;
    while (s_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        uintptr_t pawn = game::LocalPawn();
        WeaponSettings* weapon = pawn ? settings::WeaponFor(*cfg, game::ActiveWeaponDefinitionIndex(pawn)) : nullptr;
        bool rcs_enabled = weapon ? settings::Enabled(weapon->rcs_enabled) : settings::Enabled(cfg->rcs_enabled);
        if (!rcs_enabled || !g_hud.cs2_focused.load() || !g_proc.IsAlive()) {
            previous = {};
            continue;
        }
        if (!pawn || !off::m_iShotsFired || !off::m_aimPunchAngle ||
            g_proc.Read<int>(pawn + off::m_iHealth) <= 0 ||
            !IsRcsWeapon(game::ActiveWeaponDefinitionIndex(pawn))) {
            previous = {};
            continue;
        }
        int shots = g_proc.Read<int>(pawn + off::m_iShotsFired);
        Vec3 punch = g_proc.Read<Vec3>(pawn + off::m_aimPunchAngle);
        if (shots <= 1 || !std::isfinite(punch.x) || !std::isfinite(punch.y)) {
            previous = punch;
            remainder_x = remainder_y = 0.f;
            continue;
        }
        float sensitivity = cfg->aimbot_sens_x1000 / 1000.f;
        if (sensitivity < 0.005f) sensitivity = 0.005f;
        float strength = (weapon ? weapon->rcs_strength_x100 : cfg->rcs_strength_x100) / 100.f;
        if (strength < 0.f) strength = 0.f;
        if (strength > 1.f) strength = 1.f;
        remainder_x += (punch.y - previous.y) * kViewPunchScale / sensitivity * strength;
        remainder_y -= (punch.x - previous.x) * kViewPunchScale / sensitivity * strength;
        previous = punch;
        int dx = static_cast<int>(remainder_x), dy = static_cast<int>(remainder_y);
        remainder_x -= dx;
        remainder_y -= dy;
        if (dx || dy) g_input.MouseMove(dx, dy);
    }
}

void StartRcs() {
    if (s_running.exchange(true)) return;
    s_thread = std::thread(Loop);
}

void StopRcs() {
    if (!s_running.exchange(false)) return;
    if (s_thread.joinable()) s_thread.join();
}

}
