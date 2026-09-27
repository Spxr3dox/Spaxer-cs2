#include "features.h"
#include "sdk/game.h"
#include "config/settings.h"
#include "input/input.h"
#include "state.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <vector>

namespace features {

static std::atomic<bool> s_running{false};
static std::thread s_thread;

static inline float NormAngle(float a) {
    while (a >  180.f) a -= 360.f;
    while (a < -180.f) a += 360.f;
    return a;
}

static inline Vec3 CalcAngle(const Vec3& src, const Vec3& dst) {
    Vec3 d{dst.x - src.x, dst.y - src.y, dst.z - src.z};
    float dist = sqrtf(d.x*d.x + d.y*d.y);
    Vec3 a;
    a.x = -atan2f(d.z, dist) * (180.f / 3.14159265f);
    a.y =  atan2f(d.y, d.x)  * (180.f / 3.14159265f);
    a.z = 0.f;
    return a;
}

struct AimPoint { Vec3 world; };

static void CollectPoints(Settings* cfg, uintptr_t pawn, std::vector<AimPoint>& out) {
    if (settings::Enabled(cfg->aimbot_point_head))   out.push_back({game::BonePosition(pawn, game::bones::head, 64.f)});
    if (settings::Enabled(cfg->aimbot_point_neck))   out.push_back({game::BonePosition(pawn, game::bones::neck, 58.f)});
    if (settings::Enabled(cfg->aimbot_point_chest))  out.push_back({game::BonePosition(pawn, game::bones::chest, 50.f)});
    if (settings::Enabled(cfg->aimbot_point_pelvis)) out.push_back({game::BonePosition(pawn, game::bones::pelvis, 36.f)});
    if (out.empty()) out.push_back({game::BonePosition(pawn, game::bones::head, 64.f)});
}

static bool EnemyAlive(uintptr_t pawn, int my_team) {
    if (!pawn || game::IsDormant(pawn)) return false;
    int hp = g_proc.Read<int>(pawn + off::m_iHealth);
    int team = game::Team(pawn);
    return hp > 0 && hp <= 100 && (team == 2 || team == 3) && team != my_team;
}

static bool SpottedByLocal(uintptr_t pawn) {
    if (!off::m_entitySpottedState || !off::m_bSpottedByMask) return true;
    int local_index = off::g_LocalControllerIdx - 1;
    if (local_index < 0 || local_index >= 64) return true;
    uint32_t mask[2] = {};
    g_proc.ReadBytes(pawn + off::m_entitySpottedState + off::m_bSpottedByMask, mask, sizeof(mask));
    return (mask[local_index / 32] >> (local_index % 32)) & 1u;
}

static bool AimKeyHeld(uint32_t key_mode) {
    switch (key_mode) {
        case 1: return g_input.IsMouseDown(1);
        case 2: return g_input.IsMouseDown(3);
        case 3: return g_input.IsMouseDown(4) || g_input.IsMouseDown(5);
        default: return true;
    }
}

static float TargetScore(uint32_t target_mode, float angle_delta, uintptr_t enemy, const Vec3& eye) {
    switch (target_mode) {
        case 1: {
            Vec3 o = game::Origin(enemy);
            float dx = o.x - eye.x, dy = o.y - eye.y, dz = o.z - eye.z;
            return sqrtf(dx * dx + dy * dy + dz * dz);
        }
        case 2: return static_cast<float>(g_proc.Read<int>(enemy + off::m_iHealth)) * 100.f + angle_delta;
        default: return angle_delta;
    }
}

static float AngleDelta(const Vec3& eye, const Vec3& va, const Vec3& point, Vec3& desired) {
    desired = CalcAngle(eye, point);
    float d_yaw = NormAngle(desired.y - va.y);
    float d_pitch = NormAngle(desired.x - va.x);
    return sqrtf(d_yaw * d_yaw + d_pitch * d_pitch);
}

static void Loop() {
    Settings* cfg = settings::Attach();
    if (!cfg) return;
    uintptr_t locked = 0;
    uintptr_t last_target = 0;
    float rest_x = 0.f, rest_y = 0.f;
    auto switch_ready = std::chrono::steady_clock::now();

    while (s_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));

        uintptr_t pawn = game::LocalPawn();
        if (!pawn) { locked = 0; continue; }
        WeaponSettings* weapon = settings::WeaponFor(*cfg, game::ActiveWeaponDefinitionIndex(pawn));
        bool aimbot_enabled = weapon ? settings::Enabled(weapon->aimbot_enabled) : settings::Enabled(cfg->aimbot_enabled);
        if (!aimbot_enabled || !g_hud.cs2_focused.load() || !g_proc.IsAlive()) { locked = 0; continue; }
        uintptr_t list = game::EntityList();
        if (!list) continue;
        int my_hp = g_proc.Read<int>(pawn + off::m_iHealth);
        if (my_hp <= 0) { locked = 0; continue; }
        int my_team = game::Team(pawn);
        if (!AimKeyHeld(cfg->aimbot_key_mode)) { locked = 0; rest_x = rest_y = 0.f; continue; }
        bool need_visible = !settings::Enabled(cfg->aimbot_thru_walls);

        if (settings::Enabled(cfg->aimbot_flash_check) && off::m_flFlashDuration &&
            g_proc.Read<float>(pawn + off::m_flFlashDuration) > 0.3f) { locked = 0; continue; }

        Vec3 eye = game::EyePosition(pawn);
        Vec3 va = g_proc.Read<Vec3>(pawn + off::m_angEyeAngles);
        if (off::m_aimPunchAngle && off::m_iShotsFired && g_proc.Read<int>(pawn + off::m_iShotsFired) > 1) {
            Vec3 punch = g_proc.Read<Vec3>(pawn + off::m_aimPunchAngle);
            va.x += punch.x * 2.f;
            va.y += punch.y * 2.f;
        }
        float deg_per_pixel = cfg->aimbot_sens_x1000 / 1000.f;
        if (deg_per_pixel < 0.005f) deg_per_pixel = 0.005f;

        float fov_deg = (weapon ? weapon->aimbot_fov_x100 : cfg->aimbot_fov_x100) / 100.f;
        if (fov_deg < 1.f) fov_deg = 1.f;
        if (fov_deg > 180.f) fov_deg = 180.f;

        auto now = std::chrono::steady_clock::now();
        uintptr_t target = 0;
        bool aim_lock = settings::Enabled(cfg->aimbot_lock);
        if (aim_lock && EnemyAlive(locked, my_team)) target = locked;
        if (!target) {
            float best_score = 1e9f;
            for (int i = 1; i <= 64; i++) {
                if (i == off::g_LocalControllerIdx) continue;
                uintptr_t ctrl = game::EntityFromList(list, i);
                if (!ctrl) continue;
                uint32_t handle = g_proc.Read<uint32_t>(ctrl + off::m_hPlayerPawn);
                if (!handle || handle == 0xFFFFFFFF) continue;
                uintptr_t enemy = game::EntityFromList(list, handle & 0x7FFF);
                if (enemy == pawn || !EnemyAlive(enemy, my_team)) continue;
                if (need_visible && !SpottedByLocal(enemy)) continue;
                std::vector<AimPoint> pts;
                CollectPoints(cfg, enemy, pts);
                float enemy_delta = 1e9f;
                for (const auto& p : pts) {
                    Vec3 desired;
                    float delta = AngleDelta(eye, va, p.world, desired);
                    if (delta < enemy_delta) enemy_delta = delta;
                }
                if (enemy_delta >= fov_deg) continue;
                float score = TargetScore(cfg->aimbot_target_mode, enemy_delta, enemy, eye);
                if (score < best_score) { best_score = score; target = enemy; }
            }
        }
        int switch_delay_ms = cfg->aimbot_switch_delay_ms < 0 ? 0 : cfg->aimbot_switch_delay_ms;
        if (last_target && target != last_target && !(aim_lock && target && !EnemyAlive(last_target, my_team))) {
            switch_ready = now + std::chrono::milliseconds(switch_delay_ms);
            last_target = 0;
            locked = 0;
            rest_x = rest_y = 0.f;
            continue;
        }
        if (target && !last_target && now < switch_ready) {
            rest_x = rest_y = 0.f;
            continue;
        }
        last_target = target;
        locked = target;
        if (!target) { rest_x = rest_y = 0.f; continue; }

        Vec3 aim_point;
        if (settings::Enabled(cfg->aimbot_point_head)) {
            aim_point = game::BonePosition(target, game::bones::head, 64.f);
        } else {
            std::vector<AimPoint> pts;
            CollectPoints(cfg, target, pts);
            aim_point = pts.front().world;
            float nearest = 1e9f;
            for (const auto& p : pts) {
                Vec3 desired;
                float delta = AngleDelta(eye, va, p.world, desired);
                if (delta < nearest) { nearest = delta; aim_point = p.world; }
            }
        }

        Vec3 desired;
        if (AngleDelta(eye, va, aim_point, desired) < 0.05f) { rest_x = rest_y = 0.f; continue; }
        float d_yaw = NormAngle(desired.y - va.y);
        float d_pitch = NormAngle(desired.x - va.x);
        float smooth = (weapon ? weapon->aimbot_smooth_x100 : cfg->aimbot_smooth_x100) / 100.f;
        if (smooth < 0.05f) smooth = 0.05f;
        if (smooth > 1.0f)  smooth = 1.0f;

        float move_x = -d_yaw / deg_per_pixel * smooth + rest_x;
        float move_y = d_pitch / deg_per_pixel * smooth + rest_y;
        int dx = static_cast<int>(move_x);
        int dy = static_cast<int>(move_y);
        rest_x = move_x - dx;
        rest_y = move_y - dy;
        if (dx >  200) dx =  200;
        if (dx < -200) dx = -200;
        if (dy >  150) dy =  150;
        if (dy < -150) dy = -150;
        if (dx || dy) g_input.MouseMove(dx, dy);
    }
}

void StartAimbot() {
    if (s_running.exchange(true)) return;
    s_thread = std::thread(Loop);
}

void StopAimbot() {
    if (!s_running.exchange(false)) return;
    if (s_thread.joinable()) s_thread.join();
}

}
