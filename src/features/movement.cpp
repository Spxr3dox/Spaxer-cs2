#include "features.h"
#include "config/settings.h"
#include "input/input.h"
#include "sdk/game.h"
#include "sdk/offsets.h"
#include "sdk/visibility.h"
#include "state.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <linux/input.h>
#include <thread>

namespace features {

using Clock = std::chrono::steady_clock;

static std::atomic<bool> s_running{false};
static std::thread s_thread;

constexpr auto kJumpHold = std::chrono::milliseconds(20);
constexpr auto kGroundRetry = std::chrono::milliseconds(80);
constexpr float kLandLead = 0.004f;
constexpr float kHalfGravity = 400.f;
constexpr auto kStrafeHold = std::chrono::milliseconds(40);
constexpr auto kStrafeTick = std::chrono::microseconds(15625);
constexpr int kMouseDeadzone = 2;
constexpr float kSteerDeadzone = 4.f;
constexpr float kAirWishSpeed = 30.f;
constexpr float kRadToDeg = 57.29578f;
constexpr float kFastStopEngage = 30.f;
constexpr float kFastStopRelease = 12.f;

static bool OnGround(uintptr_t pawn) {
    return pawn && off::m_fFlags && (g_proc.Read<uint32_t>(pawn + off::m_fFlags) & 1u);
}

static float NormalizeDegrees(float degrees) {
    while (degrees > 180.f) degrees -= 360.f;
    while (degrees < -180.f) degrees += 360.f;
    return degrees;
}

class Jumper {
public:
    void Update(bool enabled, bool scroll, bool space_held, bool grounded, uintptr_t pawn, Clock::time_point now) {
        if (m_space_down && now - m_changed_at >= kJumpHold) Set(false, now);
        if (!enabled || !space_held) {
            if (!scroll && m_active) g_input.SetVirtualKey(KEY_SPACE, false);
            m_active = false;
            m_space_down = false;
            m_pressed = false;
            m_airborne = false;
            return;
        }
        if (!m_active) {
            m_active = true;
            m_space_down = true;
            m_changed_at = now;
        }
        if (!grounded) {
            Vec3 origin = game::Origin(pawn);
            float vz = off::m_vecVelocity ? g_proc.Read<float>(pawn + off::m_vecVelocity + 8) : 0.f;
            if (!m_airborne) {
                m_airborne = true;
                m_pressed = false;
                m_takeoff_z = origin.z;
            }
            if (m_pressed || vz >= 0.f) return;
            float ground = m_takeoff_z;
            Vec3 hit;
            if (vis::Ready() && vis::Raycast({origin.x, origin.y, origin.z + 2.f}, {origin.x, origin.y, origin.z - 512.f}, hit, nullptr, vis::Blocks::Grenades))
                ground = hit.z;
            float height = std::max(0.f, origin.z - ground);
            float land_in = (vz + std::sqrt(vz * vz + 4.f * kHalfGravity * height)) / (2.f * kHalfGravity);
            if (land_in <= kLandLead) Press(scroll, now);
            return;
        }
        m_airborne = false;
        if (m_pressed && now - m_pressed_at < kGroundRetry) return;
        Press(scroll, now);
    }

private:
    void Press(bool scroll, Clock::time_point now) {
        m_pressed = true;
        m_pressed_at = now;
        if (scroll) {
            g_input.ScrollDown();
            return;
        }
        if (m_space_down) g_input.ForceVirtualKey(KEY_SPACE, false);
        Set(true, now);
    }

    void Set(bool down, Clock::time_point now) {
        m_space_down = down;
        m_changed_at = now;
        g_input.ForceVirtualKey(KEY_SPACE, down);
    }

    bool m_active = false;
    bool m_space_down = false;
    bool m_pressed = false;
    bool m_airborne = false;
    float m_takeoff_z = 0.f;
    Clock::time_point m_changed_at{};
    Clock::time_point m_pressed_at{};
};

class Strafer {
public:
    void Stop() {
        if (!m_engaged) return;
        m_engaged = false;
        m_direction = 0;
        g_input.SetAutoStrafe(0);
    }

    void FollowMouse(int mouse_dx, Clock::time_point now) {
        m_engaged = true;
        if (std::abs(mouse_dx) >= kMouseDeadzone) {
            m_direction = mouse_dx > 0 ? 1 : -1;
            m_hold_until = now + kStrafeHold;
        } else if (now >= m_hold_until) {
            m_direction = 0;
        }
        g_input.SetAutoStrafe(m_direction);
    }

    void FullAuto(uintptr_t pawn, int mouse_dx, Clock::time_point now) {
        bool steering_known = off::m_vecVelocity && off::m_angEyeAngles;
        if (!steering_known || std::abs(mouse_dx) >= kMouseDeadzone || now < m_hold_until) {
            FollowMouse(mouse_dx, now);
            return;
        }
        if (now < m_next_tick) return;
        m_next_tick = now + kStrafeTick;
        m_engaged = true;

        Vec3 velocity = g_proc.Read<Vec3>(pawn + off::m_vecVelocity);
        float view_yaw = g_proc.Read<Vec3>(pawn + off::m_angEyeAngles).y;
        float velocity_yaw = std::atan2(velocity.y, velocity.x) * kRadToDeg;
        float view_to_velocity = NormalizeDegrees(view_yaw - velocity_yaw);

        if (std::hypot(velocity.x, velocity.y) < kAirWishSpeed) m_direction = 0;
        else if (view_to_velocity > kSteerDeadzone) m_direction = -1;
        else if (view_to_velocity < -kSteerDeadzone) m_direction = 1;
        else m_direction = m_direction > 0 ? -1 : 1;
        g_input.SetAutoStrafe(m_direction);
    }

private:
    bool m_engaged = false;
    int m_direction = 0;
    Clock::time_point m_hold_until{};
    Clock::time_point m_next_tick{};
};

class FastStop {
public:
    void Update(bool enabled, uintptr_t pawn) {
        int side_key = 0, forward_key = 0;
        if (enabled && off::m_vecVelocity && off::m_angEyeAngles) {
            Vec3 velocity = g_proc.Read<Vec3>(pawn + off::m_vecVelocity);
            float speed = std::hypot(velocity.x, velocity.y);
            float release = m_engaged ? kFastStopRelease : kFastStopEngage;
            if (speed > release) {
                float yaw = g_proc.Read<Vec3>(pawn + off::m_angEyeAngles).y / kRadToDeg;
                float fx = std::cos(yaw), fy = std::sin(yaw);
                float forward_speed = velocity.x * fx + velocity.y * fy;
                float side_speed = velocity.x * fy - velocity.y * fx;
                side_key = side_speed > release ? KEY_A : side_speed < -release ? KEY_D : 0;
                forward_key = forward_speed > release ? KEY_S : forward_speed < -release ? KEY_W : 0;
            }
        }
        m_engaged = side_key || forward_key;
        Hold(KEY_A, side_key == KEY_A);
        Hold(KEY_D, side_key == KEY_D);
        Hold(KEY_W, forward_key == KEY_W);
        Hold(KEY_S, forward_key == KEY_S);
    }

private:
    void Hold(int key, bool down) {
        bool& held = key == KEY_A ? m_a : key == KEY_D ? m_d : key == KEY_W ? m_w : m_s;
        if (held == down) return;
        held = down;
        g_input.SetVirtualKey(key, down);
    }

    bool m_engaged = false;
    bool m_a = false, m_d = false, m_w = false, m_s = false;
};

class LadderAssist {
public:
    void Update(bool fast_ladder_enabled, bool ladder_jump_enabled, uintptr_t pawn, int32_t cfg_sens, Clock::time_point now) {
        if (m_jumping && now - m_jump_time >= kJumpHold) {
            g_input.SetVirtualKey(KEY_SPACE, false);
            m_jumping = false;
        }
        if (!pawn) return;
        uint8_t mt = off::m_MoveType ? g_proc.Read<uint8_t>(pawn + off::m_MoveType) : 0;
        uint8_t amt = off::m_nActualMoveType ? g_proc.Read<uint8_t>(pawn + off::m_nActualMoveType) : 0;
        bool on_ladder = (mt == 9 || amt == 9);
        if (!on_ladder) {
            m_was_on_ladder = false;
            return;
        }
        bool w_down = g_input.IsPhysicalKeyDown(KEY_W);
        bool s_down = g_input.IsPhysicalKeyDown(KEY_S);
        Vec3 angles = off::m_angEyeAngles ? g_proc.Read<Vec3>(pawn + off::m_angEyeAngles) : Vec3{};

        if (fast_ladder_enabled && std::isfinite(angles.x) && (w_down || s_down)) {
            float target_pitch = w_down ? (angles.x < 30.f ? -89.f : 89.f) : (angles.x < 30.f ? 89.f : -89.f);
            float diff = target_pitch - angles.x;
            if (std::abs(diff) >= 1.0f) {
                float sens = cfg_sens > 10 ? cfg_sens / 1000.f : 2.0f;
                float pitch_scale = 0.022f * sens;
                int dy = std::clamp(static_cast<int>(std::round(diff / pitch_scale)), -30, 30);
                if (dy != 0) g_input.MouseMove(0, dy);
            }
        }

        if (ladder_jump_enabled && w_down && vis::Ready()) {
            Vec3 origin = game::Origin(pawn);
            float rad = angles.y * (3.14159265f / 180.f);
            float fwd_x = std::cos(rad) * 28.f;
            float fwd_y = std::sin(rad) * 28.f;

            Vec3 hit_waist, hit_head;
            bool wall_at_waist = vis::Raycast({origin.x, origin.y, origin.z + 24.f},
                                              {origin.x + fwd_x, origin.y + fwd_y, origin.z + 24.f}, hit_waist, nullptr, vis::Blocks::Grenades);
            bool wall_at_head  = vis::Raycast({origin.x, origin.y, origin.z + 62.f},
                                              {origin.x + fwd_x, origin.y + fwd_y, origin.z + 62.f}, hit_head, nullptr, vis::Blocks::Grenades);

            if (wall_at_waist && !wall_at_head) {
                if (!m_jumping && now - m_jump_time >= kGroundRetry) {
                    m_jumping = true;
                    m_jump_time = now;
                    g_input.SetVirtualKey(KEY_SPACE, true);
                }
            }
        }
        m_was_on_ladder = true;
    }

private:
    bool m_was_on_ladder = false;
    bool m_jumping = false;
    Clock::time_point m_jump_time{};
};

class EdgeJump {
public:
    void Update(bool enabled, bool grounded, uintptr_t pawn, Clock::time_point now) {
        if (m_jumping && now - m_jump_time >= kJumpHold) {
            g_input.SetVirtualKey(KEY_SPACE, false);
            m_jumping = false;
        }
        if (!enabled || !pawn || !off::m_vecVelocity) {
            m_was_grounded = grounded;
            return;
        }
        Vec3 vel = g_proc.Read<Vec3>(pawn + off::m_vecVelocity);
        float speed = std::hypot(vel.x, vel.y);
        Vec3 origin = game::Origin(pawn);

        if (grounded && speed > 40.f && vis::Ready()) {
            float dir_x = vel.x / speed;
            float dir_y = vel.y / speed;
            Vec3 test_start = {origin.x + dir_x * 16.f, origin.y + dir_y * 16.f, origin.z + 4.f};
            Vec3 test_end   = {origin.x + dir_x * 16.f, origin.y + dir_y * 16.f, origin.z - 36.f};
            Vec3 hit;
            bool hit_ground = vis::Raycast(test_start, test_end, hit, nullptr, vis::Blocks::Grenades);
            if (!hit_ground || (origin.z - hit.z > 22.f)) {
                Trigger(now);
            }
        } else if (m_was_grounded && !grounded && vel.z <= 0.f && speed > 30.f) {
            Trigger(now);
        }
        m_was_grounded = grounded;
    }

private:
    void Trigger(Clock::time_point now) {
        if (m_jumping || now - m_jump_time < kGroundRetry) return;
        m_jumping = true;
        m_jump_time = now;
        g_input.SetVirtualKey(KEY_SPACE, true);
    }

    bool m_was_grounded = true;
    bool m_jumping = false;
    Clock::time_point m_jump_time{};
};

class EdgeBug {
public:
    void Update(bool enabled, bool grounded, uintptr_t pawn, Clock::time_point now) {
        if (!enabled || grounded || !pawn || !off::m_vecVelocity) {
            if (m_crouched) {
                g_input.HoldCrouch(false);
                m_crouched = false;
            }
            return;
        }
        Vec3 vel = g_proc.Read<Vec3>(pawn + off::m_vecVelocity);
        if (vel.z >= -100.f || !vis::Ready()) {
            if (m_crouched && now - m_crouch_time > std::chrono::milliseconds(100)) {
                g_input.HoldCrouch(false);
                m_crouched = false;
            }
            return;
        }
        Vec3 origin = game::Origin(pawn);
        float dt = 0.015625f;
        float trace_depth = std::min(-80.f, vel.z * dt * 4.5f);

        constexpr float kOffsets[5][2] = {
            {0.f, 0.f}, {15.f, 15.f}, {15.f, -15.f}, {-15.f, 15.f}, {-15.f, -15.f}
        };

        Vec3 hits[5];
        bool has_hit[5];
        int hit_count = 0;
        float max_hit_z = -99999.f;

        for (int i = 0; i < 5; i++) {
            has_hit[i] = vis::Raycast({origin.x + kOffsets[i][0], origin.y + kOffsets[i][1], origin.z + 4.f},
                                      {origin.x + kOffsets[i][0], origin.y + kOffsets[i][1], origin.z + trace_depth},
                                      hits[i], nullptr, vis::Blocks::Grenades);
            if (has_hit[i]) {
                hit_count++;
                if (hits[i].z > max_hit_z) max_hit_z = hits[i].z;
            }
        }

        bool is_edge = false;
        if (hit_count > 0 && hit_count < 5) {
            is_edge = true;
        } else if (hit_count == 5) {
            for (int i = 0; i < 5; i++) {
                if (std::abs(hits[i].z - max_hit_z) > 4.f) {
                    is_edge = true;
                    break;
                }
            }
        }

        if (is_edge) {
            float dist = origin.z - max_hit_z;
            float time_to_hit = dist / (-vel.z);
            if (time_to_hit > 0.f && time_to_hit <= 0.075f) {
                if (!m_crouched) {
                    m_crouched = true;
                    m_crouch_time = now;
                    g_input.HoldCrouch(true);
                }
                return;
            }
        }
        if (m_crouched && now - m_crouch_time > std::chrono::milliseconds(120)) {
            g_input.HoldCrouch(false);
            m_crouched = false;
        }
    }

private:
    bool m_crouched = false;
    Clock::time_point m_crouch_time{};
};

class LongJump {
public:
    void Update(bool enabled, bool grounded, uintptr_t pawn, Clock::time_point now) {
        if (!enabled || !pawn || !off::m_vecVelocity) {
            if (m_crouched) { g_input.HoldCrouch(false); m_crouched = false; }
            m_was_grounded = grounded;
            return;
        }
        Vec3 vel = g_proc.Read<Vec3>(pawn + off::m_vecVelocity);
        if (m_was_grounded && !grounded && vel.z > 50.f) {
            m_jumping = true;
            m_takeoff_time = now;
            m_crouched = true;
            g_input.HoldCrouch(true);
        }
        if (m_jumping && !grounded) {
            if (m_crouched && now - m_takeoff_time > std::chrono::milliseconds(70)) {
                g_input.HoldCrouch(false);
                m_crouched = false;
            } else if (!m_crouched && vel.z < -180.f && vis::Ready()) {
                Vec3 origin = game::Origin(pawn);
                Vec3 hit;
                if (vis::Raycast({origin.x, origin.y, origin.z + 4.f}, {origin.x, origin.y, origin.z - 45.f}, hit, nullptr, vis::Blocks::Grenades)) {
                    m_crouched = true;
                    g_input.HoldCrouch(true);
                }
            }
        }
        if (grounded) {
            m_jumping = false;
            if (m_crouched) {
                g_input.HoldCrouch(false);
                m_crouched = false;
            }
        }
        m_was_grounded = grounded;
    }

private:
    bool m_was_grounded = true;
    bool m_jumping = false;
    bool m_crouched = false;
    Clock::time_point m_takeoff_time{};
};

class SlideHop {
public:
    void Update(bool enabled, bool grounded, uintptr_t pawn, Clock::time_point now) {
        if (!enabled || !pawn || !off::m_vecVelocity) {
            if (m_sliding) { g_input.HoldCrouch(false); m_sliding = false; }
            return;
        }
        Vec3 vel = g_proc.Read<Vec3>(pawn + off::m_vecVelocity);
        float horiz_speed = std::hypot(vel.x, vel.y);
        if (!grounded && vel.z < -60.f && horiz_speed > 120.f && vis::Ready()) {
            Vec3 origin = game::Origin(pawn);
            Vec3 hit;
            if (vis::Raycast({origin.x, origin.y, origin.z + 4.f}, {origin.x, origin.y, origin.z - 60.f}, hit, nullptr, vis::Blocks::Grenades)) {
                if (!m_sliding) {
                    m_sliding = true;
                    m_slide_start = now;
                    g_input.HoldCrouch(true);
                }
            }
        }
        if (grounded && m_sliding) {
            if (now - m_slide_start > std::chrono::milliseconds(180) || horiz_speed < 80.f) {
                g_input.HoldCrouch(false);
                m_sliding = false;
            }
        }
    }

private:
    bool m_sliding = false;
    Clock::time_point m_slide_start{};
};

static void Loop() {
    Settings* cfg = settings::Attach();
    if (!cfg) return;
    Jumper jumper;
    Strafer strafer;
    FastStop fast_stop;
    LadderAssist ladder;
    EdgeJump edge_jump;
    EdgeBug edge_bug;
    LongJump long_jump;
    SlideHop slide_hop;
    while (s_running.load()) {
        int mouse_dx = g_input.TakeMouseDX();
        uintptr_t pawn = game::LocalPawn();
        bool active = pawn && g_hud.in_game.load() && g_hud.cs2_focused.load();
        bool space_held = active && g_input.IsPhysicalKeyDown(KEY_SPACE);
        bool grounded = active && OnGround(pawn);
        auto now = Clock::now();

        bool bhop = active && settings::Enabled(cfg->bunnyhop) && settings::Enabled(cfg->bhop_auto_jump);
        jumper.Update(bhop, cfg->bhop_method == 1, space_held, grounded, pawn, now);

        bool user_strafing = g_input.IsPhysicalKeyDown(KEY_A) || g_input.IsPhysicalKeyDown(KEY_D);
        bool strafe = active && settings::Enabled(cfg->auto_strafe) && space_held && !grounded && !user_strafing;
        if (!strafe) strafer.Stop();
        else if (cfg->auto_strafe_mode == 1) strafer.FullAuto(pawn, mouse_dx, now);
        else strafer.FollowMouse(mouse_dx, now);

        bool moving_keys = user_strafing || g_input.IsPhysicalKeyDown(KEY_W) || g_input.IsPhysicalKeyDown(KEY_S);
        bool stop_mode_allows = settings::Enabled(cfg->fast_stop_enabled) && (cfg->fast_stop_mode == 1 || !grounded);
        fast_stop.Update(active && stop_mode_allows && !space_held && !moving_keys && !strafe, pawn);

        ladder.Update(active && settings::Enabled(cfg->fast_ladder),
                      active && settings::Enabled(cfg->ladder_jump),
                      pawn, cfg->aimbot_sens_x1000, now);

        edge_jump.Update(active && settings::Enabled(cfg->edge_jump), grounded, pawn, now);
        edge_bug.Update(active && settings::Enabled(cfg->edge_bug), grounded, pawn, now);
        long_jump.Update(active && settings::Enabled(cfg->long_jump), grounded, pawn, now);
        slide_hop.Update(active && settings::Enabled(cfg->slide_hop), grounded, pawn, now);

        bool fast_loop = space_held || (settings::Enabled(cfg->edge_bug) && !grounded) || (settings::Enabled(cfg->long_jump) && !grounded);
        std::this_thread::sleep_for(std::chrono::milliseconds(fast_loop ? 1 : 5));
    }
    strafer.Stop();
    fast_stop.Update(false, 0);
    g_input.SetVirtualKey(KEY_SPACE, false);
    g_input.HoldCrouch(false);
}

void StartMovement() {
    if (s_running.exchange(true)) return;
    s_thread = std::thread(Loop);
}

void StopMovement() {
    if (!s_running.exchange(false)) return;
    if (s_thread.joinable()) s_thread.join();
}

}
