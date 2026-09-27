#include "features.h"
#include "sdk/game.h"
#include "config/settings.h"
#include "input/input.h"
#include "state.h"
#include <atomic>
#include <thread>
#include <cmath>
#include <chrono>
#include <algorithm>

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

struct HitCapsule { int from, to; float radius; };

static constexpr HitCapsule kHitCapsules[] = {
    {6, 7, 4.2f}, {5, 6, 3.6f}, {1, 2, 6.5f}, {2, 4, 6.8f}, {4, 5, 6.2f},
    {9, 10, 3.0f}, {10, 11, 2.6f}, {13, 14, 3.0f}, {14, 15, 2.6f},
    {17, 18, 4.0f}, {18, 19, 3.2f}, {20, 21, 4.0f}, {21, 22, 3.2f},
};

static float RaySegmentDistance(const Vec3& origin, const Vec3& dir, const Vec3& a, const Vec3& b) {
    float best = 1e9f;
    for (int step = 0; step <= 8; step++) {
        float t = step / 8.f;
        Vec3 p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
        Vec3 d{p.x - origin.x, p.y - origin.y, p.z - origin.z};
        float along = d.x * dir.x + d.y * dir.y + d.z * dir.z;
        if (along <= 0.f) continue;
        Vec3 c{d.x - dir.x * along, d.y - dir.y * along, d.z - dir.z * along};
        best = std::min(best, sqrtf(c.x * c.x + c.y * c.y + c.z * c.z));
    }
    return best;
}

static bool CrosshairOnHitbox(uintptr_t local_pawn, uintptr_t target, float tolerance) {
    if (!off::m_modelState || !off::m_angEyeAngles) return true;
    uintptr_t node = g_proc.Read<uintptr_t>(target + off::m_pGameSceneNode);
    uintptr_t bones_array = node ? g_proc.Read<uintptr_t>(node + off::m_modelState + 0x80) : 0;
    if (!bones_array) return true;
    Vec3 bones[23];
    Vec3 origin = game::Origin(target);
    for (int i = 0; i < 23; i++) bones[i] = g_proc.Read<Vec3>(bones_array + static_cast<uintptr_t>(i) * 32);
    if (!game::BoneNearOrigin(bones[7], origin)) return true;

    Vec3 angles = g_proc.Read<Vec3>(local_pawn + off::m_angEyeAngles);
    if (off::m_aimPunchAngle) {
        Vec3 punch = g_proc.Read<Vec3>(local_pawn + off::m_aimPunchAngle);
        angles.x += punch.x * 2.f;
        angles.y += punch.y * 2.f;
    }
    constexpr float rad = 3.14159265f / 180.f;
    float cp = cosf(angles.x * rad), sp = sinf(angles.x * rad);
    float cy = cosf(angles.y * rad), sy = sinf(angles.y * rad);
    Vec3 dir{cp * cy, cp * sy, -sp};
    Vec3 eye = game::EyePosition(local_pawn);
    for (const HitCapsule& capsule : kHitCapsules) {
        const Vec3& a = bones[capsule.from];
        const Vec3& b = bones[capsule.to];
        if (!game::BoneNearOrigin(a, origin) || !game::BoneNearOrigin(b, origin)) continue;
        if (RaySegmentDistance(eye, dir, a, b) < capsule.radius * tolerance) return true;
    }
    return false;
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
    auto scoped_since = clock::now();
    bool was_scoped = false;
    const auto kCooldown = std::chrono::milliseconds(100);

    while (s_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));

        uintptr_t pawn = game::LocalPawn();
        WeaponSettings* weapon = pawn ? settings::WeaponFor(*cfg, game::ActiveWeaponDefinitionIndex(pawn)) : nullptr;
        bool trigger_enabled = weapon ? settings::Enabled(weapon->trigger_enabled) : settings::Enabled(cfg->trigger_enabled);
        if (!trigger_enabled || !g_hud.cs2_focused.load() || !g_proc.IsAlive()) {
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

        if (!target) { aim.aimFrames = 0; g_input.HoldCrouch(false); continue; }

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

        if (settings::Enabled(cfg->trigger_aim_correction) && aim.aimFrames < 10) {
            aim.aimFrames++;
            if (!aim.AimAtHead(pawn, target, 0.5f)) continue;
        }

        if (directTarget && off::m_iIDEntIndex) {
            int nowIdx = g_proc.Read<int>(pawn + off::m_iIDEntIndex);
            if (nowIdx != entIdx) { aim.aimFrames = 0; continue; }
        }

        if (off::m_vecVelocity) {
            Vec3 v = g_proc.Read<Vec3>(pawn + off::m_vecVelocity);
            float speed = sqrtf(v.x*v.x + v.y*v.y);
            bool onGround = true;
            if (off::m_fFlags) onGround = (g_proc.Read<uint32_t>(pawn + off::m_fFlags) & 1) != 0;
            float max_speed = is_sniper ? 5.0f : 34.0f;
            if (speed > max_speed || !onGround) {
                aim.aimFrames = 0;
                continue;
            }
        }

        int shots = off::m_iShotsFired ? g_proc.Read<int>(pawn + off::m_iShotsFired) : 0;
        if (is_sniper) {
            if (shots != 0) { aim.aimFrames = 0; continue; }
            if (IsSniper(def_idx) && !is_scoped) { aim.aimFrames = 0; continue; }
            if (clock::now() - scoped_since < std::chrono::milliseconds(120)) { aim.aimFrames = 0; continue; }
        } else {
            float recoil_score = shots <= 1 ? 1.f : 1.f - (shots - 1) * 0.25f;
            if (recoil_score < 0.f) recoil_score = 0.f;
            int chance = (int)(recoil_score * 100.f);
            int hitchance = weapon ? weapon->trigger_hitchance : cfg->trigger_hitchance;
            if (chance < hitchance) { aim.aimFrames = 0; continue; }
        }

        if (off::m_aimPunchAngle) {
            Vec3 punch = g_proc.Read<Vec3>(pawn + off::m_aimPunchAngle);
            if (sqrtf(punch.x * punch.x + punch.y * punch.y) > (is_sniper ? 0.3f : 1.2f)) { aim.aimFrames = 0; continue; }
        }

        bool do_shift = !is_sniper && settings::Enabled(cfg->trigger_shift_fire);
        if (do_shift) { g_input.HoldShift(true); std::this_thread::sleep_for(std::chrono::milliseconds(30)); }

        int delay = weapon ? weapon->trigger_delay_ms : cfg->trigger_delay_ms;
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));

        bool still_on_target = true;
        if (off::m_iIDEntIndex) still_on_target = g_proc.Read<int>(pawn + off::m_iIDEntIndex) == entIdx;
        if (still_on_target) still_on_target = g_proc.Read<int>(target + off::m_iHealth) > 0;
        if (still_on_target) still_on_target = CrosshairOnHitbox(pawn, target, is_sniper ? 0.85f : 1.0f);
        if (!still_on_target) {
            if (do_shift) g_input.HoldShift(false);
            aim.aimFrames = 0;
            continue;
        }

        g_input.ClickLeft();
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
