#include "features.h"
#include "config/settings.h"
#include "input/input.h"
#include "sdk/game.h"
#include "sdk/dumper.h"
#include "sdk/offsets.h"
#include "state.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <linux/input.h>
#include <thread>
#include <vector>

namespace features {

static std::atomic<bool> s_running{false};
static std::thread s_thread;

static bool OnGround(uintptr_t pawn) {
    return pawn && off::m_fFlags && (g_proc.Read<uint32_t>(pawn + off::m_fFlags) & 1u);
}

constexpr int32_t kJumpPressed = 65537;
constexpr int32_t kJumpReleased = 256;

struct JumpButton {
    int pid = -1;
    uintptr_t state = 0;
    std::vector<uintptr_t> candidates;
    std::vector<uint8_t> seen_pressed;
    std::vector<uint8_t> confirmations;
    std::chrono::steady_clock::time_point next_scan{};
};

static const char* JumpCachePath() {
    static char path[512];
    if (!path[0]) {
        const char* home = getenv("HOME");
        snprintf(path, sizeof(path), "%s/.config/spaxer/jump_button", home ? home : "/tmp");
    }
    return path;
}

static bool LooksLikeButtonState(uintptr_t address) {
    uint32_t value = g_proc.Read<uint32_t>(address);
    return (value & ~0x10101u) == 0;
}

static uintptr_t LoadJumpButton() {
    FILE* f = fopen(JumpCachePath(), "r");
    if (!f) return 0;
    unsigned long offset = 0;
    bool ok = fscanf(f, "%lx", &offset) == 1;
    fclose(f);
    if (!ok || !offset) return 0;
    uintptr_t address = off::g_ClientBase + offset;
    return LooksLikeButtonState(address) ? address : 0;
}

static void SaveJumpButton(uintptr_t address) {
    FILE* f = fopen(JumpCachePath(), "w");
    if (!f) return;
    fprintf(f, "%lx\n", static_cast<unsigned long>(address - off::g_ClientBase));
    fclose(f);
}

static void CalibrateJump(JumpButton& jump, bool space_held) {
    if (jump.pid != g_proc.pid()) {
        jump = JumpButton{};
        jump.pid = g_proc.pid();
    }
    if (jump.state || !off::g_ClientBase) return;
    if (jump.candidates.empty()) {
        auto now = std::chrono::steady_clock::now();
        if (now < jump.next_scan) return;
        if ((jump.state = LoadJumpButton())) return;
        jump.next_scan = now + std::chrono::seconds(5);
        jump.candidates = dumper::FindButtonStateCandidates("jump");
        jump.seen_pressed.assign(jump.candidates.size(), 0);
        jump.confirmations.assign(jump.candidates.size(), 0);
        return;
    }
    for (size_t i = 0; i < jump.candidates.size(); i++) {
        uint32_t value = g_proc.Read<uint32_t>(jump.candidates[i]);
        if (space_held) {
            if (value & 1u) jump.seen_pressed[i] = 1;
        } else if (jump.seen_pressed[i] && !(value & 1u)) {
            jump.seen_pressed[i] = 0;
            if (++jump.confirmations[i] >= 2) {
                jump.state = jump.candidates[i];
                SaveJumpButton(jump.state);
                return;
            }
        }
    }
}

static void Loop() {
    Settings* cfg = settings::Attach();
    if (!cfg) return;
    float prev_yaw = 0.f;
    bool has_prev_yaw = false;
    int strafe_dir = 0;
    JumpButton jump;
    auto strafe_until = std::chrono::steady_clock::now();
    while (s_running.load()) {
        uintptr_t pawn = game::LocalPawn();
        bool active = pawn && g_hud.in_game.load() && g_hud.cs2_focused.load();
        bool grounded = active && OnGround(pawn);

        bool space_held = g_input.IsKeyDown(KEY_SPACE);
        bool bhop = settings::Enabled(cfg->bunnyhop) && settings::Enabled(cfg->bhop_auto_jump) && active;
        if (active) CalibrateJump(jump, space_held);
        if (bhop && space_held && jump.state) {
            if (grounded) {
                g_proc.Write<int32_t>(jump.state, kJumpPressed);
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
            g_proc.Write<int32_t>(jump.state, kJumpReleased);
        }
        if (settings::Enabled(cfg->auto_strafe) && active && !grounded && space_held) {
            Vec3 angles = off::m_angEyeAngles ? g_proc.Read<Vec3>(pawn + off::m_angEyeAngles) : Vec3{};
            int dir = 0;
            if (has_prev_yaw) {
                float yaw_delta = angles.y - prev_yaw;
                while (yaw_delta > 180.f) yaw_delta -= 360.f;
                while (yaw_delta < -180.f) yaw_delta += 360.f;
                if (yaw_delta > 0.5f) dir = -1;
                else if (yaw_delta < -0.5f) dir = 1;
            }
            prev_yaw = angles.y;
            has_prev_yaw = true;

            auto now = std::chrono::steady_clock::now();
            if (dir != 0) {
                strafe_dir = dir;
                strafe_until = now + std::chrono::milliseconds(60);
            } else if (now >= strafe_until) {
                strafe_dir = 0;
            }
            g_input.SetAutoStrafe(strafe_dir);
        } else {
            g_input.SetAutoStrafe(0);
            has_prev_yaw = false;
            strafe_dir = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(bhop && space_held ? 1 : 10));
    }
    g_input.SetAutoStrafe(0);
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
