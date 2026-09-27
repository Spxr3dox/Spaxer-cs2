#include "features.h"
#include "sdk/game.h"
#include "sdk/visibility.h"
#include "config/settings.h"
#include "input/input.h"
#include "state.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <vector>
#include <algorithm>
#include <random>

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

struct AimPoint { Vec3 world; float damage_scale; };

static void CollectPoints(Settings* cfg, uintptr_t pawn, std::vector<AimPoint>& out) {
    if (settings::Enabled(cfg->aimbot_point_head))   out.push_back({game::BonePosition(pawn, game::bones::head, 64.f), 4.f});
    if (settings::Enabled(cfg->aimbot_point_neck))   out.push_back({game::BonePosition(pawn, game::bones::neck, 58.f), 1.f});
    if (settings::Enabled(cfg->aimbot_point_chest))  out.push_back({game::BonePosition(pawn, game::bones::chest, 50.f), 1.f});
    if (settings::Enabled(cfg->aimbot_point_pelvis)) out.push_back({game::BonePosition(pawn, game::bones::pelvis, 36.f), 1.25f});
    if (out.empty()) out.push_back({game::BonePosition(pawn, game::bones::head, 64.f), 4.f});
}

struct Reachability {
    bool rays = false;
    bool autowall = false;
    vis::Ballistics weapon{};
    float min_damage = 0.f;
    Vec3 eye{};

    bool Hits(const AimPoint& point) const {
        if (vis::LineOfSight(eye, point.world)) return true;
        return autowall && vis::DamageAt(eye, point.world, weapon) * point.damage_scale >= min_damage;
    }
};

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

class Humanizer {
public:
    Humanizer() : rng_(std::random_device{}()) { Retarget(); }

    float Speed(const Settings& cfg, float base, std::chrono::steady_clock::time_point now) {
        if (!settings::Enabled(cfg.aimbot_humanize)) return base;
        float low = std::clamp(cfg.aimbot_speed_min_x100, 1, 100) / 100.f;
        float high = std::clamp(cfg.aimbot_speed_max_x100, 1, 100) / 100.f;
        if (low > high) std::swap(low, high);
        if (now >= next_speed_change_ || speed_goal_ < low || speed_goal_ > high) {
            speed_goal_ = std::uniform_real_distribution<float>(low, high)(rng_);
            next_speed_change_ = now + std::chrono::milliseconds(std::uniform_int_distribution<int>(120, 380)(rng_));
            if (speed_current_ <= 0.f) speed_current_ = speed_goal_;
        }
        speed_current_ += (speed_goal_ - speed_current_) * 0.15f;
        return speed_current_;
    }

    void Shake(const Settings& cfg, std::chrono::steady_clock::time_point now, float& move_x, float& move_y) {
        float amplitude = settings::Enabled(cfg.aimbot_humanize) ? std::clamp(cfg.aimbot_shake_x100, 0, 100) / 100.f * 4.f : 0.f;
        float t = std::chrono::duration<float>(now.time_since_epoch()).count();
        constexpr float tau = 6.2831853f;
        float offset_x = amplitude * (0.6f * std::sin(tau * 3.1f * t + phase_[0]) + 0.4f * std::sin(tau * 7.3f * t + phase_[1]));
        float offset_y = amplitude * 0.7f * (0.6f * std::sin(tau * 2.7f * t + phase_[2]) + 0.4f * std::sin(tau * 6.1f * t + phase_[3]));
        move_x += offset_x - shake_x_;
        move_y += offset_y - shake_y_;
        shake_x_ = offset_x;
        shake_y_ = offset_y;
    }

    void Track(float move_x, float move_y) {
        velocity_x_ = velocity_x_ * 0.5f + move_x * 0.5f;
        velocity_y_ = velocity_y_ * 0.5f + move_y * 0.5f;
    }

    bool Release(const Settings& cfg, float& move_x, float& move_y) {
        if (!settings::Enabled(cfg.aimbot_humanize)) return false;
        float decay = std::clamp(cfg.aimbot_release_x100, 5, 100) / 100.f;
        velocity_x_ *= 1.f - decay;
        velocity_y_ *= 1.f - decay;
        shake_x_ = shake_y_ = 0.f;
        if (std::fabs(velocity_x_) < 0.3f && std::fabs(velocity_y_) < 0.3f) return false;
        move_x = velocity_x_;
        move_y = velocity_y_;
        return true;
    }

    void Stop() {
        velocity_x_ = velocity_y_ = 0.f;
        shake_x_ = shake_y_ = 0.f;
    }

    void Retarget() {
        std::uniform_real_distribution<float> phase(0.f, 6.2831853f);
        for (float& value : phase_) value = phase(rng_);
        speed_current_ = 0.f;
        next_speed_change_ = {};
    }

private:
    std::mt19937 rng_;
    float speed_goal_ = 0.f, speed_current_ = 0.f;
    std::chrono::steady_clock::time_point next_speed_change_{};
    float phase_[4]{};
    float shake_x_ = 0.f, shake_y_ = 0.f;
    float velocity_x_ = 0.f, velocity_y_ = 0.f;
};

enum class AimResult { Blocked, Idle, Aim };

struct AimState {
    uintptr_t locked = 0;
    uintptr_t last_target = 0;
    std::chrono::steady_clock::time_point switch_ready = std::chrono::steady_clock::now();
};

static AimResult ComputeAim(Settings* cfg, AimState& state, Humanizer& humanizer, float& move_x, float& move_y) {
    uintptr_t& locked = state.locked;
    uintptr_t& last_target = state.last_target;
    auto& switch_ready = state.switch_ready;
    {
        uintptr_t pawn = game::LocalPawn();
        if (!pawn) { locked = 0; return AimResult::Blocked; }
        WeaponSettings* weapon = settings::WeaponFor(*cfg, game::ActiveWeaponDefinitionIndex(pawn));
        bool aimbot_enabled = weapon ? settings::Enabled(weapon->aimbot_enabled) : settings::Enabled(cfg->aimbot_enabled);
        if (!aimbot_enabled || !g_hud.cs2_focused.load() || !g_proc.IsAlive()) { locked = 0; return AimResult::Blocked; }
        uintptr_t list = game::EntityList();
        if (!list) return AimResult::Blocked;
        int my_hp = g_proc.Read<int>(pawn + off::m_iHealth);
        if (my_hp <= 0) { locked = 0; return AimResult::Blocked; }
        int my_team = game::Team(pawn);
        if (!AimKeyHeld(cfg->aimbot_key_mode)) { locked = 0; return AimResult::Idle; }
        bool need_visible = !settings::Enabled(cfg->aimbot_thru_walls);

        if (settings::Enabled(cfg->aimbot_flash_check) && off::m_flFlashDuration &&
            g_proc.Read<float>(pawn + off::m_flFlashDuration) > 0.3f) { locked = 0; return AimResult::Idle; }

        Vec3 eye = game::EyePosition(pawn);
        Vec3 va = g_proc.Read<Vec3>(pawn + off::m_angEyeAngles);
        if (off::m_aimPunchAngle && off::m_iShotsFired && g_proc.Read<int>(pawn + off::m_iShotsFired) > 1) {
            Vec3 punch = g_proc.Read<Vec3>(pawn + off::m_aimPunchAngle);
            va.x += punch.x * 2.f;
            va.y += punch.y * 2.f;
        }
        Reachability reach;
        reach.rays = need_visible && vis::Ready();
        reach.eye = eye;
        reach.autowall = settings::Enabled(cfg->autowall) &&
                         vis::WeaponBallistics(game::ActiveWeaponDefinitionIndex(pawn), reach.weapon);
        reach.min_damage = static_cast<float>(std::max(1, cfg->autowall_min_damage));
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
                if (need_visible && !reach.rays && !SpottedByLocal(enemy)) continue;
                std::vector<AimPoint> pts;
                CollectPoints(cfg, enemy, pts);
                if (reach.rays) std::erase_if(pts, [&](const AimPoint& p) { return !reach.Hits(p); });
                if (pts.empty()) continue;
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
            return AimResult::Idle;
        }
        if (target && !last_target && now < switch_ready) return AimResult::Idle;
        if (target != last_target) humanizer.Retarget();
        last_target = target;
        locked = target;
        if (!target) return AimResult::Idle;

        Vec3 aim_point;
        if (settings::Enabled(cfg->aimbot_point_head)) {
            aim_point = game::BonePosition(target, game::bones::head, 64.f);
        } else {
            std::vector<AimPoint> pts;
            CollectPoints(cfg, target, pts);
            if (reach.rays) {
                std::vector<AimPoint> reachable = pts;
                std::erase_if(reachable, [&](const AimPoint& p) { return !reach.Hits(p); });
                if (!reachable.empty()) pts = std::move(reachable);
            }
            aim_point = pts.front().world;
            float nearest = 1e9f;
            for (const auto& p : pts) {
                Vec3 desired;
                float delta = AngleDelta(eye, va, p.world, desired);
                if (delta < nearest) { nearest = delta; aim_point = p.world; }
            }
        }

        Vec3 desired;
        if (AngleDelta(eye, va, aim_point, desired) < 0.05f) { move_x = move_y = 0.f; return AimResult::Aim; }
        float d_yaw = NormAngle(desired.y - va.y);
        float d_pitch = NormAngle(desired.x - va.x);
        float base_smooth = (weapon ? weapon->aimbot_smooth_x100 : cfg->aimbot_smooth_x100) / 100.f;
        float smooth = std::clamp(humanizer.Speed(*cfg, base_smooth, now), 0.02f, 1.f);
        move_x = -d_yaw / deg_per_pixel * smooth;
        move_y = d_pitch / deg_per_pixel * smooth;
        return AimResult::Aim;
    }
}

static void Loop() {
    Settings* cfg = settings::Attach();
    if (!cfg) return;
    AimState state;
    Humanizer humanizer;
    float rest_x = 0.f, rest_y = 0.f;

    while (s_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        float move_x = 0.f, move_y = 0.f;
        AimResult result = ComputeAim(cfg, state, humanizer, move_x, move_y);
        if (result == AimResult::Aim) {
            humanizer.Shake(*cfg, std::chrono::steady_clock::now(), move_x, move_y);
            humanizer.Track(move_x, move_y);
        } else if (result == AimResult::Blocked || !humanizer.Release(*cfg, move_x, move_y)) {
            humanizer.Stop();
            rest_x = rest_y = 0.f;
            continue;
        }
        move_x += rest_x;
        move_y += rest_y;
        int dx = static_cast<int>(move_x);
        int dy = static_cast<int>(move_y);
        rest_x = move_x - dx;
        rest_y = move_y - dy;
        dx = std::clamp(dx, -200, 200);
        dy = std::clamp(dy, -150, 150);
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
