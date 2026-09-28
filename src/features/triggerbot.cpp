#include "features.h"
#include "sdk/game.h"
#include "sdk/visibility.h"
#include "config/settings.h"
#include "input/input.h"
#include "state.h"
#include <atomic>
#include <thread>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <linux/input.h>
#include <vector>

namespace features {

static std::atomic<bool> s_running{false};

constexpr float kAccurateSpeedFraction = 0.34f;
constexpr float kUnscopedSniperPenalty = 0.08f;
constexpr int kHitChanceSamples = 64;
constexpr float kGoldenAngle = 2.39996323f;
constexpr float kCounterThreshold = 15.f;
constexpr auto kAutoStopLimit = std::chrono::milliseconds(700);
static std::thread s_thread;

static inline float NormAngle(float a) {
    while (a >  180.f) a -= 360.f;
    while (a < -180.f) a += 360.f;
    return a;
}

static inline Vec3 CalcAngle(const Vec3& src, const Vec3& dst) {
    Vec3 d{dst.x - src.x, dst.y - src.y, dst.z - src.z};
    float dist = sqrtf(d.x*d.x + d.y*d.y);
    Vec3 ang;
    ang.x = -atan2f(d.z, dist) * (180.f / 3.14159265f);
    ang.y =  atan2f(d.y, d.x) * (180.f / 3.14159265f);
    ang.z = 0.f;
    return ang;
}

struct AimState {
    float degPerPixel = 0.044f;
    int   aimFrames = 0;

    bool AimAtHead(uintptr_t localPawn, uintptr_t target, float smooth) {
        Vec3 lo = game::Origin(localPawn);
        Vec3 to = game::Origin(target);
        if ((lo.x == 0 && lo.y == 0) || (to.x == 0 && to.y == 0)) return true;

        float headH = 65.0f;
        if (off::m_fFlags) {
            uint32_t flags = g_proc.Read<uint32_t>(target + off::m_fFlags);
            if (flags & 2) headH = 48.0f;
        }
        Vec3 head{to.x, to.y, to.z + headH};

        float eyeH = 64.0f;
        if (off::m_fFlags) {
            uint32_t myFlags = g_proc.Read<uint32_t>(localPawn + off::m_fFlags);
            if (myFlags & 2) eyeH = 46.0f;
        }
        Vec3 eye{lo.x, lo.y, lo.z + eyeH};
        Vec3 va = g_proc.Read<Vec3>(localPawn + off::m_angEyeAngles);
        Vec3 desired = CalcAngle(eye, head);

        float dYaw   = NormAngle(desired.y - va.y);
        float dPitch = NormAngle(desired.x - va.x);
        float total  = sqrtf(dYaw*dYaw + dPitch*dPitch);

        if (fabsf(dPitch) > 8.0f && fabsf(dPitch) > fabsf(dYaw) * 2.0f) return true;
        if (total > 12.0f) return true;
        if (total < 1.5f)  return true;

        if (smooth < 0.1f) smooth = 0.1f;
        if (smooth > 1.0f) smooth = 1.0f;
        int dx = (int)(-dYaw  / degPerPixel * smooth);
        int dy = (int)(dPitch / degPerPixel * smooth);
        if (dx >  250) dx =  250;
        if (dx < -250) dx = -250;
        if (dy >   80) dy =   80;
        if (dy <  -80) dy =  -80;
        if (dx || dy) { g_input.MouseMove(dx, dy); std::this_thread::sleep_for(std::chrono::microseconds(5000)); }
        return false;
    }
};

struct HitCapsule { int from, to; float radius; float damage_scale; };

static constexpr HitCapsule kHitCapsules[] = {
    {6, 7, 4.2f, 4.f}, {5, 6, 3.6f, 1.f}, {1, 2, 6.5f, 1.25f}, {2, 4, 6.8f, 1.f}, {4, 5, 6.2f, 1.f},
    {9, 10, 3.0f, 1.f}, {10, 11, 2.6f, 1.f}, {13, 14, 3.0f, 1.f}, {14, 15, 2.6f, 1.f},
    {17, 18, 4.0f, 0.75f}, {18, 19, 3.2f, 0.75f}, {20, 21, 4.0f, 0.75f}, {21, 22, 3.2f, 0.75f},
};

struct CrosshairHit {
    bool hit = false;
    Vec3 point{};
    float damage_scale = 1.f;
};

struct ViewTangent {
    float row[3][4];
    float right_len, up_len, depth_len;

    bool Load() {
        if (!off::dwViewMatrix || !off::g_ClientBase) return false;
        float m[16];
        if (!g_proc.ReadBytes(off::g_ClientBase + off::dwViewMatrix, m, sizeof(m))) return false;
        for (float value : m)
            if (!std::isfinite(value)) return false;
        for (int c = 0; c < 4; c++) {
            row[0][c] = m[c];
            row[1][c] = m[4 + c];
            row[2][c] = m[12 + c];
        }
        right_len = Length(row[0]);
        up_len = Length(row[1]);
        depth_len = Length(row[2]);
        return right_len > 1e-3f && up_len > 1e-3f && depth_len > 1e-3f;
    }

    bool Project(const Vec3& p, float& x, float& y, float& depth) const {
        depth = (Dot(row[2], p) + row[2][3]) / depth_len;
        if (depth < 1.f) return false;
        x = (Dot(row[0], p) + row[0][3]) / right_len / depth;
        y = (Dot(row[1], p) + row[1][3]) / up_len / depth;
        return true;
    }

    void AimPoint(const Vec3& punch, float& x, float& y) const {
        constexpr float rad = 3.14159265f / 180.f;
        Vec3 forward{row[2][0] / depth_len, row[2][1] / depth_len, row[2][2] / depth_len};
        float pitch = -asinf(std::clamp(forward.z, -1.f, 1.f)) + punch.x * 2.f * rad;
        float yaw = atan2f(forward.y, forward.x) + punch.y * 2.f * rad;
        Vec3 dir{cosf(pitch) * cosf(yaw), cosf(pitch) * sinf(yaw), -sinf(pitch)};
        float along = Dot(row[2], dir) / depth_len;
        x = Dot(row[0], dir) / right_len / along;
        y = Dot(row[1], dir) / up_len / along;
    }

private:
    static float Dot(const float* r, const Vec3& v) { return r[0] * v.x + r[1] * v.y + r[2] * v.z; }
    static float Length(const float* r) { return sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]); }
};

static float ClosestOnSegment(float px, float py, float ax, float ay, float bx, float by, float& distance) {
    float dx = bx - ax, dy = by - ay;
    float length_sq = dx * dx + dy * dy;
    float t = length_sq > 0.f ? std::clamp(((px - ax) * dx + (py - ay) * dy) / length_sq, 0.f, 1.f) : 0.f;
    float cx = ax + dx * t - px, cy = ay + dy * t - py;
    distance = sqrtf(cx * cx + cy * cy);
    return t;
}

struct ProjectedCapsule { float ax, ay, bx, by, radius; bool head; };

static bool ProjectHitboxes(uintptr_t local_pawn, uintptr_t target, float tolerance, std::vector<ProjectedCapsule>& out,
                            float& aim_x, float& aim_y) {
    ViewTangent view;
    if (!off::m_modelState || !view.Load()) return false;
    uintptr_t node = g_proc.Read<uintptr_t>(target + off::m_pGameSceneNode);
    uintptr_t bones_array = node ? g_proc.Read<uintptr_t>(node + off::m_modelState + 0x80) : 0;
    if (!bones_array) return false;
    Vec3 bones[23];
    Vec3 origin = game::Origin(target);
    for (int i = 0; i < 23; i++) bones[i] = g_proc.Read<Vec3>(bones_array + static_cast<uintptr_t>(i) * 32);
    if (!game::BoneNearOrigin(bones[7], origin)) return false;
    Vec3 punch = off::m_aimPunchAngle ? g_proc.Read<Vec3>(local_pawn + off::m_aimPunchAngle) : Vec3{};
    view.AimPoint(punch, aim_x, aim_y);
    for (const HitCapsule& capsule : kHitCapsules) {
        const Vec3& a = bones[capsule.from];
        const Vec3& b = bones[capsule.to];
        if (!game::BoneNearOrigin(a, origin) || !game::BoneNearOrigin(b, origin)) continue;
        float ax, ay, a_depth, bx, by, b_depth;
        if (!view.Project(a, ax, ay, a_depth) || !view.Project(b, bx, by, b_depth)) continue;
        out.push_back({ax, ay, bx, by, capsule.radius * tolerance / std::max(a_depth, b_depth), capsule.damage_scale >= 4.f});
    }
    return !out.empty();
}

struct WeaponAccuracy { float spread, crouch, stand, move, max_speed; };

static WeaponAccuracy AccuracyFor(int definition) {
    switch (definition) {
        case 1: case 2: case 3: case 4: case 30: case 32: case 36: case 61: case 63: case 64:
            return {2.0f, 4.0f, 6.0f, 30.f, 240.f};
        case 17: case 19: case 23: case 24: case 26: case 33: case 34:
            return {1.0f, 8.0f, 11.0f, 40.f, 230.f};
        case 7: case 8: case 10: case 13: case 16: case 39: case 60:
            return {0.6f, 4.8f, 6.4f, 150.f, 220.f};
        case 9: case 11: case 38: case 40:
            return {0.2f, 1.5f, 2.2f, 150.f, 200.f};
        case 14: case 28:
            return {2.0f, 6.0f, 8.0f, 150.f, 200.f};
        case 25: case 27: case 29: case 35:
            return {40.f, 8.0f, 10.0f, 20.f, 220.f};
        default:
            return {1.0f, 6.0f, 8.0f, 100.f, 220.f};
    }
}

static float InaccuracyCone(uintptr_t pawn, int definition, bool scoped, bool sniper) {
    WeaponAccuracy accuracy = AccuracyFor(definition);
    bool crouched = off::m_fFlags && (g_proc.Read<uint32_t>(pawn + off::m_fFlags) & 2u);
    Vec3 velocity = off::m_vecVelocity ? g_proc.Read<Vec3>(pawn + off::m_vecVelocity) : Vec3{};
    float speed = std::hypot(velocity.x, velocity.y);
    float move = std::clamp((speed - accuracy.max_speed * kAccurateSpeedFraction) / (accuracy.max_speed * (1.f - kAccurateSpeedFraction)), 0.f, 1.f);
    float cone = (accuracy.spread + (crouched ? accuracy.crouch : accuracy.stand) + accuracy.move * move) / 1000.f;
    if (sniper && !scoped) cone += kUnscopedSniperPenalty;
    if (off::m_fAccuracyPenalty) {
        uintptr_t weapon = game::ActiveWeapon(pawn);
        float penalty = weapon ? g_proc.Read<float>(weapon + off::m_fAccuracyPenalty) : 0.f;
        if (std::isfinite(penalty) && penalty > 0.f) cone += penalty;
    }
    return cone;
}

static int HitChancePercent(uintptr_t local_pawn, uintptr_t target, float cone, float tolerance) {
    std::vector<ProjectedCapsule> capsules;
    float aim_x, aim_y;
    if (!ProjectHitboxes(local_pawn, target, tolerance, capsules, aim_x, aim_y)) return 100;
    float spread = std::tan(cone);
    int hits = 0;
    for (int i = 0; i < kHitChanceSamples; i++) {
        float radius = spread * ((i + 0.5f) / kHitChanceSamples);
        float angle = i * kGoldenAngle;
        float px = aim_x + std::cos(angle) * radius, py = aim_y + std::sin(angle) * radius;
        for (const ProjectedCapsule& capsule : capsules) {
            float distance;
            ClosestOnSegment(px, py, capsule.ax, capsule.ay, capsule.bx, capsule.by, distance);
            if (distance < capsule.radius) {
                hits++;
                break;
            }
        }
    }
    return hits * 100 / kHitChanceSamples;
}

static bool SpreadCovered(uintptr_t local_pawn, uintptr_t target, float cone, float tolerance, int coverage, bool head_only) {
    std::vector<ProjectedCapsule> capsules;
    float aim_x, aim_y;
    if (!ProjectHitboxes(local_pawn, target, tolerance, capsules, aim_x, aim_y)) return false;
    float spread = std::tan(cone);
    auto inside = [&](float px, float py) {
        for (const ProjectedCapsule& capsule : capsules) {
            if (head_only && !capsule.head) continue;
            float distance;
            ClosestOnSegment(px, py, capsule.ax, capsule.ay, capsule.bx, capsule.by, distance);
            if (distance < capsule.radius) return true;
        }
        return false;
    };
    if (!inside(aim_x, aim_y)) return false;
    constexpr int kRing = 24;
    int total = 0, hits = 0;
    for (float scale : {0.5f, 1.0f}) {
        for (int i = 0; i < kRing; i++) {
            float angle = i * 6.2831853f / kRing;
            total++;
            if (inside(aim_x + std::cos(angle) * spread * scale, aim_y + std::sin(angle) * spread * scale)) hits++;
        }
    }
    return hits * 100 >= std::clamp(coverage, 1, 100) * total;
}

class StopHold {
public:
    void Engage(uintptr_t pawn) {
        auto now = std::chrono::steady_clock::now();
        if (m_expired) return;
        if (!m_engaged) m_since = now;
        if (now - m_since > kAutoStopLimit) {
            ReleaseKeys();
            m_expired = true;
            return;
        }
        m_engaged = true;
        bool forward = g_input.IsPhysicalKeyDown(KEY_W), back = g_input.IsPhysicalKeyDown(KEY_S);
        bool left = g_input.IsPhysicalKeyDown(KEY_A), right = g_input.IsPhysicalKeyDown(KEY_D);
        bool want_w = back && !forward, want_s = forward && !back;
        bool want_a = right && !left, want_d = left && !right;
        ViewTangent view;
        if (view.Load() && off::m_vecVelocity) {
            float fx = view.row[2][0], fy = view.row[2][1];
            float flat = std::hypot(fx, fy);
            if (flat > 1e-3f) {
                fx /= flat; fy /= flat;
                Vec3 velocity = g_proc.Read<Vec3>(pawn + off::m_vecVelocity);
                float forward_speed = velocity.x * fx + velocity.y * fy;
                float side_speed = velocity.x * fy - velocity.y * fx;
                if (!forward && !back) {
                    want_s = forward_speed > kCounterThreshold;
                    want_w = forward_speed < -kCounterThreshold;
                }
                if (!left && !right) {
                    want_a = side_speed > kCounterThreshold;
                    want_d = side_speed < -kCounterThreshold;
                }
            }
        }
        Set(KEY_W, want_w, m_w);
        Set(KEY_S, want_s, m_s);
        Set(KEY_A, want_a, m_a);
        Set(KEY_D, want_d, m_d);
    }

    void Release() {
        m_expired = false;
        ReleaseKeys();
    }

private:
    void ReleaseKeys() {
        m_engaged = false;
        Set(KEY_W, false, m_w);
        Set(KEY_S, false, m_s);
        Set(KEY_A, false, m_a);
        Set(KEY_D, false, m_d);
    }

    static void Set(int key, bool down, bool& state) {
        if (state == down) return;
        state = down;
        g_input.SetVirtualKey(key, down);
    }

    bool m_engaged = false;
    bool m_expired = false;
    bool m_w = false, m_s = false, m_a = false, m_d = false;
    std::chrono::steady_clock::time_point m_since{};
};

static CrosshairHit HitboxUnderCrosshair(uintptr_t local_pawn, uintptr_t target, float tolerance) {
    CrosshairHit result;
    ViewTangent view;
    if (!off::m_modelState || !view.Load()) return result;
    uintptr_t node = g_proc.Read<uintptr_t>(target + off::m_pGameSceneNode);
    uintptr_t bones_array = node ? g_proc.Read<uintptr_t>(node + off::m_modelState + 0x80) : 0;
    if (!bones_array) return result;
    Vec3 bones[23];
    Vec3 origin = game::Origin(target);
    for (int i = 0; i < 23; i++) bones[i] = g_proc.Read<Vec3>(bones_array + static_cast<uintptr_t>(i) * 32);
    if (!game::BoneNearOrigin(bones[7], origin)) return result;

    Vec3 punch = off::m_aimPunchAngle ? g_proc.Read<Vec3>(local_pawn + off::m_aimPunchAngle) : Vec3{};
    float aim_x, aim_y;
    view.AimPoint(punch, aim_x, aim_y);
    float best_margin = 0.f;
    for (const HitCapsule& capsule : kHitCapsules) {
        const Vec3& a = bones[capsule.from];
        const Vec3& b = bones[capsule.to];
        if (!game::BoneNearOrigin(a, origin) || !game::BoneNearOrigin(b, origin)) continue;
        float ax, ay, a_depth, bx, by, b_depth;
        if (!view.Project(a, ax, ay, a_depth) || !view.Project(b, bx, by, b_depth)) continue;
        float radius = capsule.radius * tolerance / std::max(a_depth, b_depth);
        float distance;
        float t = ClosestOnSegment(aim_x, aim_y, ax, ay, bx, by, distance);
        float margin = radius - distance;
        if (margin <= 0.f || (result.hit && capsule.damage_scale <= result.damage_scale && margin <= best_margin)) continue;
        result.hit = true;
        result.point = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
        result.damage_scale = capsule.damage_scale;
        best_margin = margin;
    }
    return result;
}

static bool CrosshairOnHitbox(uintptr_t local_pawn, uintptr_t target, float tolerance) {
    if (!off::dwViewMatrix) return true;
    return HitboxUnderCrosshair(local_pawn, target, tolerance).hit;
}

static bool PenetratesTo(uintptr_t local_pawn, uintptr_t target, const vis::Ballistics& weapon, float min_damage, float tolerance) {
    CrosshairHit hit = HitboxUnderCrosshair(local_pawn, target, tolerance);
    if (!hit.hit) return false;
    Vec3 eye = game::EyePosition(local_pawn);
    return vis::DamageAt(eye, hit.point, weapon) * hit.damage_scale >= min_damage;
}

static bool IsSniper(int def_idx) {
    return def_idx == 9 || def_idx == 40 || def_idx == 38 || def_idx == 11;
}

static void Loop() {
    using clock = std::chrono::steady_clock;
    Settings* cfg = settings::Attach();
    if (!cfg) return;

    AimState aim;
    auto lastFire = clock::now() - std::chrono::seconds(1);
    StopHold stop_hold;
    uintptr_t wall_candidate = 0;
    int wall_confirmations = 0;
    auto scoped_since = clock::now();
    bool was_scoped = false;
    const auto kCooldown = std::chrono::milliseconds(100);

    while (s_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));

        uintptr_t pawn = game::LocalPawn();
        WeaponSettings* weapon = pawn ? settings::WeaponFor(*cfg, game::ActiveWeaponDefinitionIndex(pawn)) : nullptr;
        bool trigger_enabled = weapon ? settings::Enabled(weapon->trigger_enabled) : settings::Enabled(cfg->trigger_enabled);
        bool spread_trigger = settings::Enabled(cfg->trigger_spread);
        if ((!trigger_enabled && !spread_trigger) || !g_hud.cs2_focused.load() || !g_proc.IsAlive()) {
            stop_hold.Release();
            g_input.HoldCrouch(false);
            aim.aimFrames = 0;
            continue;
        }
        uintptr_t list = game::EntityList();
        if (!list) { aim.aimFrames = 0; continue; }
        if (!pawn) { aim.aimFrames = 0; continue; }
        int myHp = g_proc.Read<int>(pawn + off::m_iHealth);
        if (myHp <= 0) { aim.aimFrames = 0; continue; }
        bool scoped_now = off::m_bIsScoped && g_proc.Read<bool>(pawn + off::m_bIsScoped);
        if (scoped_now && !was_scoped) scoped_since = clock::now();
        was_scoped = scoped_now;
        int myTeam = game::Team(pawn);

        if (settings::Enabled(cfg->trigger_flash_check) && off::m_flFlashDuration) {
            float flash = g_proc.Read<float>(pawn + off::m_flFlashDuration);
            if (flash > 0.3f) continue;
        }

        if (clock::now() - lastFire < kCooldown) continue;

        uintptr_t target = 0;
        bool directTarget = false;
        int entIdx = off::m_iIDEntIndex ? g_proc.Read<int>(pawn + off::m_iIDEntIndex) : -1;
        if (entIdx > 0 && entIdx < 2048) {
            uintptr_t cand = game::EntityFromList(list, entIdx);
            if (cand && cand != pawn && !game::IsDormant(cand)) {
                bool valid_pawn = false;
                for (int i = 1; i <= 64; i++) {
                    uintptr_t ctrl = game::EntityFromList(list, i);
                    if (!ctrl) continue;
                    uint32_t pawnH = g_proc.Read<uint32_t>(ctrl + off::m_hPlayerPawn);
                    if (pawnH == 0 || pawnH == 0xFFFFFFFF) continue;
                    uintptr_t p = game::EntityFromList(list, pawnH & 0x7FFF);
                    if (p == cand) { valid_pawn = true; break; }
                }
                if (valid_pawn) {
                    int hp = g_proc.Read<int>(cand + off::m_iHealth);
                    int tm = game::Team(cand);
                    if (hp > 0 && hp <= 100 && (tm == 2 || tm == 3) && tm != myTeam) {
                        target = cand; directTarget = true;
                    }
                }
            }
        }

        bool hitbox_target = false;
        if (!target && off::dwViewMatrix && vis::Ready()) {
            Vec3 eye = game::EyePosition(pawn);
            for (int i = 1; i <= 64 && !target; i++) {
                uintptr_t ctrl = game::EntityFromList(list, i);
                if (!ctrl) continue;
                uint32_t handle = g_proc.Read<uint32_t>(ctrl + off::m_hPlayerPawn);
                if (!handle || handle == 0xFFFFFFFF) continue;
                uintptr_t enemy = game::EntityFromList(list, handle & 0x7FFF);
                if (!enemy || enemy == pawn || game::IsDormant(enemy)) continue;
                int hp = g_proc.Read<int>(enemy + off::m_iHealth);
                int tm = game::Team(enemy);
                if (hp <= 0 || hp > 100 || (tm != 2 && tm != 3) || tm == myTeam) continue;
                CrosshairHit hit = HitboxUnderCrosshair(pawn, enemy, 1.f);
                if (hit.hit && vis::LineOfSight(eye, hit.point)) {
                    target = enemy;
                    hitbox_target = true;
                }
            }
        }

        vis::Ballistics ballistics{};
        bool autowall = trigger_enabled && settings::Enabled(cfg->autowall) && vis::Ready() && off::dwViewMatrix &&
                        vis::WeaponBallistics(game::ActiveWeaponDefinitionIndex(pawn), ballistics);
        bool force_shot = settings::Enabled(cfg->trigger_force_shot);
        int damage_setting = settings::Enabled(cfg->trigger_md_override) ? cfg->md_override_value : cfg->autowall_min_damage;
        bool damage_limited = !force_shot && (settings::Enabled(cfg->min_damage_enabled) || settings::Enabled(cfg->trigger_md_override));
        float min_damage = damage_limited ? static_cast<float>(std::max(1, damage_setting)) : 1.f;
        bool wall_target = false;
        if (!target && autowall) {
            for (int i = 1; i <= 64 && !target; i++) {
                uintptr_t ctrl = game::EntityFromList(list, i);
                if (!ctrl) continue;
                uint32_t handle = g_proc.Read<uint32_t>(ctrl + off::m_hPlayerPawn);
                if (!handle || handle == 0xFFFFFFFF) continue;
                uintptr_t enemy = game::EntityFromList(list, handle & 0x7FFF);
                if (!enemy || enemy == pawn || game::IsDormant(enemy)) continue;
                int hp = g_proc.Read<int>(enemy + off::m_iHealth);
                int tm = game::Team(enemy);
                if (hp <= 0 || hp > 100 || (tm != 2 && tm != 3) || tm == myTeam) continue;
                if (PenetratesTo(pawn, enemy, ballistics, min_damage, 1.f)) {
                    target = enemy;
                    wall_target = true;
                }
            }
            wall_confirmations = target && target == wall_candidate ? wall_confirmations + 1 : (target ? 1 : 0);
            wall_candidate = target;
            if (target && wall_confirmations < 2) target = 0;
        }

        if (!target) { stop_hold.Release(); aim.aimFrames = 0; g_input.HoldCrouch(false); continue; }

        int def_idx = 0;
        if (off::m_pWeaponServices && off::m_hActiveWeapon && off::m_AttributeManager && off::m_Item && off::m_iItemDefinitionIndex) {
            uintptr_t services = g_proc.Read<uintptr_t>(pawn + off::m_pWeaponServices);
            uint32_t handle = services ? g_proc.Read<uint32_t>(services + off::m_hActiveWeapon) : 0;
            uintptr_t entity = game::EntityFromList(list, handle & 0x7FFF);
            if (entity)
                def_idx = g_proc.Read<int>(entity + off::m_AttributeManager + off::m_Item + off::m_iItemDefinitionIndex);
        }
        bool is_scoped = off::m_bIsScoped && g_proc.Read<bool>(pawn + off::m_bIsScoped);
        bool is_sniper = is_scoped || IsSniper(def_idx);

        if (!wall_target && settings::Enabled(cfg->trigger_aim_correction) && aim.aimFrames < 10) {
            aim.aimFrames++;
            if (!aim.AimAtHead(pawn, target, 0.5f)) continue;
        }

        if (directTarget && off::m_iIDEntIndex) {
            int nowIdx = g_proc.Read<int>(pawn + off::m_iIDEntIndex);
            if (nowIdx != entIdx) { aim.aimFrames = 0; continue; }
        }

        bool on_ground = !off::m_fFlags || (g_proc.Read<uint32_t>(pawn + off::m_fFlags) & 1u);
        if (!on_ground && !force_shot) { aim.aimFrames = 0; continue; }
        if (off::m_vecVelocity && !force_shot) {
            Vec3 v = g_proc.Read<Vec3>(pawn + off::m_vecVelocity);
            float stop_speed = AccuracyFor(def_idx).max_speed * kAccurateSpeedFraction;
            if (std::hypot(v.x, v.y) > stop_speed) {
                if (settings::Enabled(cfg->trigger_autostop)) stop_hold.Engage(pawn);
                aim.aimFrames = 0;
                continue;
            }
        }

        if (is_sniper) {
            int shots = off::m_iShotsFired ? g_proc.Read<int>(pawn + off::m_iShotsFired) : 0;
            if (shots != 0) { aim.aimFrames = 0; continue; }
            if (IsSniper(def_idx) && !is_scoped) { aim.aimFrames = 0; continue; }
            if (clock::now() - scoped_since < std::chrono::milliseconds(120)) { aim.aimFrames = 0; continue; }
        }
        int hitchance = weapon ? weapon->trigger_hitchance : cfg->trigger_hitchance;
        if (spread_trigger && !force_shot && !wall_target) {
            float cone = InaccuracyCone(pawn, def_idx, is_scoped, IsSniper(def_idx));
            if (!SpreadCovered(pawn, target, cone, 1.0f, cfg->spread_coverage, settings::Enabled(cfg->spread_head_only))) { aim.aimFrames = 0; continue; }
        } else if (hitchance > 0 && !force_shot) {
            float cone = InaccuracyCone(pawn, def_idx, is_scoped, IsSniper(def_idx));
            if (HitChancePercent(pawn, target, cone, is_sniper ? 0.85f : 1.0f) < hitchance) { aim.aimFrames = 0; continue; }
        }

        if (off::m_aimPunchAngle) {
            Vec3 punch = g_proc.Read<Vec3>(pawn + off::m_aimPunchAngle);
            if (sqrtf(punch.x * punch.x + punch.y * punch.y) > (is_sniper ? 0.3f : 1.2f)) { aim.aimFrames = 0; continue; }
        }

        bool do_shift = !is_sniper && settings::Enabled(cfg->trigger_shift_fire);
        if (do_shift) { g_input.HoldShift(true); std::this_thread::sleep_for(std::chrono::milliseconds(30)); }

        int delay = weapon ? weapon->trigger_delay_ms : cfg->trigger_delay_ms;
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));

        bool still_on_target = g_proc.Read<int>(target + off::m_iHealth) > 0;
        float tolerance = is_sniper ? 0.85f : 1.0f;
        if (still_on_target && wall_target) {
            still_on_target = PenetratesTo(pawn, target, ballistics, min_damage, tolerance);
        } else if (still_on_target && hitbox_target) {
            CrosshairHit hit = HitboxUnderCrosshair(pawn, target, tolerance);
            still_on_target = hit.hit && vis::LineOfSight(game::EyePosition(pawn), hit.point);
        } else if (still_on_target) {
            if (off::m_iIDEntIndex) still_on_target = g_proc.Read<int>(pawn + off::m_iIDEntIndex) == entIdx;
            if (still_on_target) still_on_target = CrosshairOnHitbox(pawn, target, tolerance);
        }
        if (!still_on_target) {
            if (do_shift) g_input.HoldShift(false);
            aim.aimFrames = 0;
            continue;
        }

        g_input.ClickLeft();
        stop_hold.Release();
        if (do_shift) { std::this_thread::sleep_for(std::chrono::milliseconds(30)); g_input.HoldShift(false); }

        aim.aimFrames = 0;
        lastFire = clock::now();
    }
    g_input.HoldCrouch(false);
    g_input.HoldShift(false);
}

void StartTriggerBot() {
    if (s_running.exchange(true)) return;
    s_thread = std::thread(Loop);
}

void StopTriggerBot() {
    if (!s_running.exchange(false)) return;
    if (s_thread.joinable()) s_thread.join();
}

}
