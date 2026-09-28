#include "hitmarker.h"
#include "features.h"
#include "config/settings.h"
#include "features/hit_sound.h"
#include "state.h"
#include <array>

namespace features {

std::mutex g_hitmarks_mtx;
std::vector<HitMark> g_hitmarks;
static std::array<int, 65> s_prev_hp{};

static Vec3 HitPosition(uintptr_t local_pawn, uintptr_t pawn) {
    static constexpr int kBones[] = {7, 6, 5, 4, 3, 2, 1, 9, 10, 11, 13, 14, 15, 17, 18, 19, 20, 21, 22};
    constexpr float rad = 3.14159265f / 180.f;
    Vec3 eye = game::EyePosition(local_pawn);
    Vec3 angles = g_proc.Read<Vec3>(local_pawn + off::m_angEyeAngles);
    float cp = cosf(angles.x * rad);
    Vec3 dir{cp * cosf(angles.y * rad), cp * sinf(angles.y * rad), -sinf(angles.x * rad)};
    Vec3 best = game::BonePosition(pawn, game::bones::chest, 45.f);
    float best_distance = 1e9f;
    for (int bone : kBones) {
        Vec3 p = game::BonePosition(pawn, bone, -1000.f);
        Vec3 d{p.x - eye.x, p.y - eye.y, p.z - eye.z};
        float along = d.x * dir.x + d.y * dir.y + d.z * dir.z;
        if (along <= 0.f) continue;
        Vec3 perp{d.x - dir.x * along, d.y - dir.y * along, d.z - dir.z * along};
        float distance = perp.x * perp.x + perp.y * perp.y + perp.z * perp.z;
        if (distance < best_distance) { best_distance = distance; best = p; }
    }
    return best;
}

void UpdateHitmarker() {
    Settings* cfg = settings::Attach();
    bool markers = cfg && settings::Enabled(cfg->hitmarker);
    auto sound = cfg ? static_cast<hitsound::Style>(cfg->hit_sound) : hitsound::Style::Off;
    bool notices = cfg && settings::Enabled(cfg->notifications);
    if (!cfg || (!markers && sound == hitsound::Style::Off && !notices) || !g_proc.IsAlive()) return;
    uintptr_t list = game::EntityList();
    uintptr_t local_pawn = game::LocalPawn();
    if (!list || !local_pawn) return;
    uint8_t local_team = g_proc.Read<uint8_t>(local_pawn + off::m_iTeamNum);

    for (int i = 1; i <= 64; i++) {
        uintptr_t controller = game::EntityFromList(list, i);
        uint32_t handle = controller ? g_proc.Read<uint32_t>(controller + off::m_hPlayerPawn) : 0;
        uintptr_t pawn = handle && handle != 0xFFFFFFFF ? game::EntityFromList(list, handle & 0x7FFF) : 0;
        if (!pawn || pawn == local_pawn) { s_prev_hp[i] = 0; continue; }
        uint8_t team = g_proc.Read<uint8_t>(pawn + off::m_iTeamNum);
        int hp = g_proc.Read<int>(pawn + off::m_iHealth);
        if (team == local_team || (team != 2 && team != 3) || hp < 0 || hp > 100) { s_prev_hp[i] = 0; continue; }

        int prev = s_prev_hp[i];
        if (prev > 0 && hp < prev) {
            if (hp == 0 || !settings::Enabled(cfg->hit_sound_kills_only)) hitsound::Play(sound, cfg->hit_sound_volume, hp == 0);
            if (notices) {
                std::string name = "enemy";
                {
                    std::lock_guard<std::mutex> lock(g_hud.esp_mtx);
                    for (const EspEntry& entry : g_hud.esp_players)
                        if (entry.pawn == pawn && entry.name[0]) name = entry.name;
                }
                if (hp == 0) PushNotice("Killed " + name, NoticeKind::Kill);
                else PushNotice("Hit " + name + " for " + std::to_string(prev - hp) + " (" + std::to_string(hp) + " hp left)", NoticeKind::Hit);
            }
            if (markers) {
                std::lock_guard<std::mutex> lock(g_hitmarks_mtx);
                g_hitmarks.push_back({HitPosition(local_pawn, pawn), prev - hp, std::chrono::steady_clock::now()});
                if (g_hitmarks.size() > 16) g_hitmarks.erase(g_hitmarks.begin());
            }
        }
        s_prev_hp[i] = hp;
    }
}

}
