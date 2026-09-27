#include "sound_esp.h"
#include "features.h"
#include "config/settings.h"
#include "sdk/game.h"
#include <cmath>
#include <array>

namespace features {

struct RawStep {
    Vec3 origin;
    std::chrono::steady_clock::time_point time;
    float max_radius = 50.f;
};

std::mutex g_sound_steps_mtx;
std::vector<SoundStep> g_sound_steps;
static std::vector<RawStep> s_raw_steps;
static std::array<std::chrono::steady_clock::time_point, 65> s_last_step{};

static void PublishSteps(std::vector<SoundStep> steps) {
    std::lock_guard<std::mutex> lock(g_sound_steps_mtx);
    g_sound_steps.swap(steps);
}

void UpdateSoundEsp() {
    Settings* cfg = settings::Attach();
    if (!cfg || !settings::Enabled(cfg->sound_esp) || !g_proc.IsAlive()) {
        s_raw_steps.clear();
        PublishSteps({});
        return;
    }
    uintptr_t list = game::EntityList();
    if (!list) return;
    uintptr_t local_controller = game::LocalController();
    if (!local_controller) return;
    int local_team = game::Team(local_controller);

    auto now = std::chrono::steady_clock::now();

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

        if (game::IsDormant(pawn)) continue;
        Vec3 pos = game::Origin(pawn);
        Vec3 vel = g_proc.Read<Vec3>(pawn + off::m_vecVelocity);
        float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y);

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_last_step[i]).count();
        if (speed > 135.f && elapsed > 320) {
            s_last_step[i] = now;
            s_raw_steps.push_back(RawStep{pos, now, 50.f});
        }
    }

    std::vector<RawStep> valid_raw;
    valid_raw.reserve(s_raw_steps.size());
    for (const auto& step : s_raw_steps) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - step.time).count();
        if (ms < 1200) valid_raw.push_back(step);
    }
    s_raw_steps = std::move(valid_raw);

    std::vector<SoundStep> out;
    out.reserve(s_raw_steps.size());
    for (const auto& step : s_raw_steps) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - step.time).count();
        float progress = static_cast<float>(ms) / 1200.f;
        out.push_back(SoundStep{step.origin.x, step.origin.y, step.origin.z,
                                progress * step.max_radius, (1.f - progress) * 0.75f});
    }
    PublishSteps(std::move(out));
}

}
