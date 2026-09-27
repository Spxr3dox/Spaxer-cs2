#include "features.h"
#include "config/settings.h"
#include "input/input.h"
#include "sdk/game.h"
#include "sdk/offsets.h"
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

constexpr auto kJumpPressTimeout = std::chrono::milliseconds(40);
constexpr auto kJumpRetryDelay = std::chrono::milliseconds(300);
constexpr auto kTakeoffFlicker = std::chrono::milliseconds(60);
constexpr auto kStrafeHold = std::chrono::milliseconds(40);
constexpr auto kStrafeTick = std::chrono::microseconds(15625);
constexpr int kMouseDeadzone = 2;
constexpr float kSteerDeadzone = 4.f;
constexpr float kAirWishSpeed = 30.f;
constexpr float kRadToDeg = 57.29578f;

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
    void Update(bool enabled, bool space_held, bool grounded, Clock::time_point now) {
        if (!enabled || !space_held) {
            g_input.SetVirtualKey(KEY_SPACE, false);
            m_space_down = true;
            m_changed_at = now;
            return;
        }
        bool taking_off = now - m_pressed_at < kTakeoffFlicker;
        if (!grounded || (taking_off && !m_space_down)) {
            if (m_space_down) Set(false, now);
            return;
        }
        if (m_space_down) {
            if (now - m_changed_at >= kJumpPressTimeout) {
                Set(false, now);
                m_retry_at = now + kJumpRetryDelay;
            }
            return;
        }
        if (now < m_retry_at) return;
        Set(true, now);
        m_pressed_at = now;
    }

private:
    void Set(bool down, Clock::time_point now) {
        m_space_down = down;
        m_changed_at = now;
        g_input.ForceVirtualKey(KEY_SPACE, down);
    }

    bool m_space_down = true;
    Clock::time_point m_changed_at{};
    Clock::time_point m_pressed_at{};
    Clock::time_point m_retry_at{};
};

class Strafer {
public:
    void Stop() {
        m_direction = 0;
        g_input.SetAutoStrafe(0);
    }

    void FollowMouse(int mouse_dx, Clock::time_point now) {
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
    int m_direction = 0;
    Clock::time_point m_hold_until{};
    Clock::time_point m_next_tick{};
};

static void Loop() {
    Settings* cfg = settings::Attach();
    if (!cfg) return;
    Jumper jumper;
    Strafer strafer;
    while (s_running.load()) {
        int mouse_dx = g_input.TakeMouseDX();
        uintptr_t pawn = game::LocalPawn();
        bool active = pawn && g_hud.in_game.load() && g_hud.cs2_focused.load();
        bool space_held = active && g_input.IsPhysicalKeyDown(KEY_SPACE);
        bool grounded = active && OnGround(pawn);
        auto now = Clock::now();

        bool bhop = active && settings::Enabled(cfg->bunnyhop) && settings::Enabled(cfg->bhop_auto_jump);
        jumper.Update(bhop, space_held, grounded, now);

        bool user_strafing = g_input.IsPhysicalKeyDown(KEY_A) || g_input.IsPhysicalKeyDown(KEY_D);
        bool strafe = active && settings::Enabled(cfg->auto_strafe) && space_held && !grounded && !user_strafing;
        if (!strafe) strafer.Stop();
        else if (cfg->auto_strafe_mode == 1) strafer.FullAuto(pawn, mouse_dx, now);
        else strafer.FollowMouse(mouse_dx, now);

        std::this_thread::sleep_for(std::chrono::milliseconds(space_held ? 1 : 5));
    }
    strafer.Stop();
    g_input.SetVirtualKey(KEY_SPACE, false);
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
