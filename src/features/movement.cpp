#include "features.h"
#include "config/settings.h"
#include "input/input.h"
#include "sdk/game.h"
#include "sdk/dumper.h"
#include "sdk/offsets.h"
#include "state.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <linux/input.h>
#include <thread>
#include <vector>

namespace features {

using Clock = std::chrono::steady_clock;

static std::atomic<bool> s_running{false};
static std::thread s_thread;

constexpr int32_t kJumpPressed = 65537;
constexpr int32_t kJumpReleased = 256;
constexpr uintptr_t kButtonStateFromName = 0x28;
constexpr int kConfirmPresses = 2;
constexpr int kMaxFailedJumps = 5;
constexpr auto kJumpHoldLimit = std::chrono::milliseconds(60);
constexpr auto kStrafeHold = std::chrono::milliseconds(40);
constexpr auto kStrafeTick = std::chrono::microseconds(15625);
constexpr int kAutoStrafeFlipTicks = 10;
constexpr float kAirWishSpeed = 30.f;

static bool OnGround(uintptr_t pawn) {
    return pawn && off::m_fFlags && (g_proc.Read<uint32_t>(pawn + off::m_fFlags) & 1u);
}

static float HorizontalSpeed(uintptr_t pawn) {
    if (!off::m_vecVelocity) return 0.f;
    Vec3 velocity = g_proc.Read<Vec3>(pawn + off::m_vecVelocity);
    return std::hypot(velocity.x, velocity.y);
}

static const char* JumpCachePath() {
    static char path[512];
    if (!path[0]) {
        const char* home = getenv("HOME");
        snprintf(path, sizeof(path), "%s/.config/spaxer/jump_button", home ? home : "/tmp");
    }
    return path;
}

static bool LooksLikeButtonState(uintptr_t address) {
    uint32_t value = 0;
    if (!g_proc.ReadBytes(address, &value, sizeof(value))) return false;
    return (value & ~0x10101u) == 0;
}

static uintptr_t LoadCachedJump() {
    FILE* f = fopen(JumpCachePath(), "r");
    if (!f) return 0;
    unsigned long offset = 0;
    bool ok = fscanf(f, "%lx", &offset) == 1;
    fclose(f);
    return ok && offset ? off::g_ClientBase + offset : 0;
}

static void SaveCachedJump(uintptr_t address) {
    FILE* f = fopen(JumpCachePath(), "w");
    if (!f) return;
    fprintf(f, "%lx\n", static_cast<unsigned long>(address - off::g_ClientBase));
    fclose(f);
}

class JumpButton {
public:
    uintptr_t Address() const { return m_state; }

    void Update(bool space_held) {
        if (m_pid != g_proc.pid()) Reset();
        if (m_state || !off::g_ClientBase) return;
        if (m_candidates.empty()) {
            Scan();
            return;
        }
        Observe(space_held);
    }

    void ReportJump(bool left_ground) {
        if (left_ground) {
            m_failed_jumps = 0;
            return;
        }
        if (++m_failed_jumps < kMaxFailedJumps) return;
        fprintf(stderr, "[bhop] jump button 0x%lx does not jump, recalibrating\n", m_state);
        m_rejected.push_back(m_state);
        remove(JumpCachePath());
        m_state = 0;
        m_failed_jumps = 0;
        m_candidates.clear();
        m_next_scan = {};
    }

private:
    void Reset() {
        *this = JumpButton{};
        m_pid = g_proc.pid();
    }

    bool Rejected(uintptr_t address) const {
        return std::find(m_rejected.begin(), m_rejected.end(), address) != m_rejected.end();
    }

    void Scan() {
        auto now = Clock::now();
        if (now < m_next_scan) return;
        m_next_scan = now + std::chrono::seconds(5);

        std::vector<uintptr_t> slots = dumper::FindButtonNameSlots("jump");
        std::vector<uintptr_t> ordered;
        auto add = [&](uintptr_t address) {
            if (Rejected(address) || !LooksLikeButtonState(address)) return;
            if (std::find(ordered.begin(), ordered.end(), address) == ordered.end()) ordered.push_back(address);
        };
        for (uintptr_t slot : slots) add(slot + kButtonStateFromName);
        size_t layout_matches = ordered.size();
        uintptr_t cached = LoadCachedJump();
        bool cached_matches_layout = std::find(ordered.begin(), ordered.end(), cached) != ordered.end();
        if (cached_matches_layout || layout_matches == 1) {
            m_state = cached_matches_layout ? cached : ordered.front();
            SaveCachedJump(m_state);
            fprintf(stderr, "[bhop] jump button at client+0x%lx\n", m_state - off::g_ClientBase);
            return;
        }
        for (uintptr_t slot : slots)
            for (uintptr_t field = 0; field < 0x80; field += 4) add(slot + field);

        m_candidates = std::move(ordered);
        m_seen_pressed.assign(m_candidates.size(), 0);
        m_confirmations.assign(m_candidates.size(), 0);
        fprintf(stderr, "[bhop] %zu jump slots, %zu candidates; tap space twice to calibrate\n",
                slots.size(), m_candidates.size());
    }

    void Observe(bool space_held) {
        for (size_t i = 0; i < m_candidates.size(); i++) {
            uint32_t value = g_proc.Read<uint32_t>(m_candidates[i]);
            bool pressed = value & 1u;
            if (space_held) {
                if (pressed) m_seen_pressed[i] = 1;
            } else if (m_seen_pressed[i] && !pressed) {
                m_seen_pressed[i] = 0;
                ++m_confirmations[i];
            }
        }
        for (size_t i = 0; i < m_candidates.size(); i++) {
            if (m_confirmations[i] < kConfirmPresses) continue;
            m_state = m_candidates[i];
            SaveCachedJump(m_state);
            fprintf(stderr, "[bhop] jump button at client+0x%lx\n", m_state - off::g_ClientBase);
            return;
        }
    }

    int m_pid = -1;
    uintptr_t m_state = 0;
    int m_failed_jumps = 0;
    std::vector<uintptr_t> m_candidates;
    std::vector<uint8_t> m_seen_pressed;
    std::vector<uint8_t> m_confirmations;
    std::vector<uintptr_t> m_rejected;
    Clock::time_point m_next_scan{};
};

static bool JumpNow(uintptr_t pawn, uintptr_t state) {
    g_proc.Write<int32_t>(state, kJumpPressed);
    auto deadline = Clock::now() + kJumpHoldLimit;
    bool left_ground = false;
    while (Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!OnGround(pawn)) {
            left_ground = true;
            break;
        }
    }
    g_proc.Write<int32_t>(state, kJumpReleased);
    return left_ground;
}

class Strafer {
public:
    void Stop() {
        g_input.SetAutoStrafe(0);
        m_direction = 0;
        m_auto_ticks = 0;
    }

    void FollowMouse(int mouse_dx, Clock::time_point now) {
        if (mouse_dx != 0) {
            m_direction = mouse_dx > 0 ? 1 : -1;
            m_hold_until = now + kStrafeHold;
        } else if (now >= m_hold_until) {
            m_direction = 0;
        }
        g_input.SetAutoStrafe(m_direction);
    }

    void FullAuto(uintptr_t pawn, int mouse_dx, float degrees_per_pixel, Clock::time_point now) {
        if (mouse_dx != 0 || now < m_hold_until) {
            FollowMouse(mouse_dx, now);
            m_next_tick = now;
            return;
        }
        if (now < m_next_tick) return;
        m_next_tick = now + kStrafeTick;
        if (m_auto_ticks-- <= 0) {
            m_auto_side = m_auto_side > 0 ? -1 : 1;
            m_auto_ticks = kAutoStrafeFlipTicks;
        }
        float speed = std::max(HorizontalSpeed(pawn), kAirWishSpeed);
        float step_degrees = std::clamp(std::atan2(kAirWishSpeed, speed) * 57.29578f, 0.8f, 6.f);
        float pixels = step_degrees / std::max(degrees_per_pixel, 0.005f) + m_pixel_remainder;
        int whole_pixels = static_cast<int>(pixels);
        m_pixel_remainder = pixels - whole_pixels;
        if (whole_pixels) g_input.MouseMove(whole_pixels * m_auto_side, 0);
        m_direction = m_auto_side;
        g_input.SetAutoStrafe(m_direction);
    }

private:
    int m_direction = 0;
    int m_auto_side = 1;
    int m_auto_ticks = 0;
    float m_pixel_remainder = 0.f;
    Clock::time_point m_hold_until{};
    Clock::time_point m_next_tick{};
};

static void Loop() {
    Settings* cfg = settings::Attach();
    if (!cfg) return;
    JumpButton jump;
    Strafer strafer;
    bool released_in_air = false;
    while (s_running.load()) {
        int mouse_dx = g_input.TakeMouseDX();
        uintptr_t pawn = game::LocalPawn();
        bool active = pawn && g_hud.in_game.load() && g_hud.cs2_focused.load();
        bool space_held = active && g_input.IsKeyDown(KEY_SPACE);
        bool bhop = active && settings::Enabled(cfg->bunnyhop) && settings::Enabled(cfg->bhop_auto_jump);
        bool grounded = active && OnGround(pawn);
        auto now = Clock::now();

        if (active) jump.Update(space_held);

        if (bhop && space_held && jump.Address()) {
            if (grounded) {
                bool moving = HorizontalSpeed(pawn) > 50.f;
                bool left_ground = JumpNow(pawn, jump.Address());
                if (moving || left_ground) jump.ReportJump(left_ground);
                released_in_air = true;
                grounded = false;
            } else if (!released_in_air) {
                g_proc.Write<int32_t>(jump.Address(), kJumpReleased);
                released_in_air = true;
            }
        } else {
            released_in_air = false;
        }

        bool user_strafing = g_input.IsPhysicalKeyDown(KEY_A) || g_input.IsPhysicalKeyDown(KEY_D);
        bool strafe = active && settings::Enabled(cfg->auto_strafe) && space_held && !grounded && !user_strafing;
        if (!strafe) strafer.Stop();
        else if (cfg->auto_strafe_mode == 1) strafer.FullAuto(pawn, mouse_dx, cfg->aimbot_sens_x1000 / 1000.f, now);
        else strafer.FollowMouse(mouse_dx, now);

        std::this_thread::sleep_for(std::chrono::milliseconds(space_held ? 1 : 5));
    }
    strafer.Stop();
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
