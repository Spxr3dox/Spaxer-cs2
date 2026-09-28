#include "features.h"
#include "config/settings.h"
#include "sdk/game.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace features {

static constexpr uint32_t kWhiteOpaque = 0xFFFFFFFFu;
static constexpr uint8_t kRenderNormal = 0;
static constexpr uint8_t kRenderTransColor = 1;
static constexpr uint32_t kEffectNoDraw = 0x20;

enum class HideMode : uint32_t { Off = 0, Transparent = 1, NoDraw = 2 };

static std::unordered_set<uintptr_t> s_modified;

static uint32_t ToRenderColor(uint32_t rgba, uint8_t alpha) {
    uint32_t r = (rgba >> 24) & 0xFF, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF;
    return r | (g << 8) | (b << 16) | (static_cast<uint32_t>(alpha) << 24);
}

template <typename T>
static void WriteIfChanged(uintptr_t address, T value) {
    if (g_proc.Read<T>(address) != value) g_proc.Write<T>(address, value);
}

static void ApplyPawn(uintptr_t pawn, bool tint, uint32_t tint_rgba, HideMode hide) {
    bool transparent = hide == HideMode::Transparent;
    if (off::m_clrRender) {
        uint32_t color = tint ? ToRenderColor(tint_rgba, transparent ? 0 : 0xFF)
                              : (transparent ? 0x00FFFFFFu : kWhiteOpaque);
        WriteIfChanged<uint32_t>(pawn + off::m_clrRender, color);
    }
    if (off::m_nRenderMode)
        WriteIfChanged<uint8_t>(pawn + off::m_nRenderMode, transparent ? kRenderTransColor : kRenderNormal);
    if (off::m_fEffects) {
        uint32_t effects = g_proc.Read<uint32_t>(pawn + off::m_fEffects);
        uint32_t wanted = hide == HideMode::NoDraw ? (effects | kEffectNoDraw) : (effects & ~kEffectNoDraw);
        if (wanted != effects) g_proc.Write<uint32_t>(pawn + off::m_fEffects, wanted);
    }
}

static void RestorePawn(uintptr_t pawn) {
    ApplyPawn(pawn, false, 0, HideMode::Off);
}

struct HideTarget {
    uintptr_t pawn;
    uint32_t rgba;
    uintptr_t weapon;
};

static std::mutex s_targets_mtx;
static std::vector<HideTarget> s_targets;
static std::atomic<bool> s_tint{false};
static std::atomic<uint32_t> s_hide{0};
static std::atomic<bool> s_running{false};
static std::thread s_thread;

void ApplyChams() {
    Settings* cfg = settings::Attach();
    if (!cfg || !g_proc.IsAlive()) {
        std::lock_guard<std::mutex> lock(s_targets_mtx);
        s_targets.clear();
        s_modified.clear();
        return;
    }
    bool chams = settings::Enabled(cfg->chams);
    bool tint = chams && settings::Enabled(cfg->chams_tint) && off::m_clrRender;
    HideMode hide = chams && cfg->chams_hide_model <= 2 ? static_cast<HideMode>(cfg->chams_hide_model) : HideMode::Off;
    s_tint.store(tint);
    s_hide.store(static_cast<uint32_t>(hide));
    std::vector<HideTarget> targets;
    if (tint || hide != HideMode::Off || !s_modified.empty()) {
        uintptr_t list = game::EntityList();
        if (!list) return;
        uintptr_t local = game::LocalPawn();
        int my_team = game::Team(local);
        bool show_team = settings::Enabled(cfg->chams_team);
        std::unordered_set<uintptr_t> touched;
        for (int i = 1; i <= 64; i++) {
            uintptr_t controller = game::EntityFromList(list, i);
            if (!controller) continue;
            uint32_t handle = g_proc.Read<uint32_t>(controller + off::m_hPlayerPawn);
            if (!handle || handle == 0xFFFFFFFF) continue;
            uintptr_t pawn = game::EntityFromList(list, handle & 0x7FFF);
            if (!pawn || pawn == local) continue;
            bool teammate = my_team && game::Team(pawn) == my_team;
            bool active = (tint || hide != HideMode::Off) && g_proc.Read<int>(pawn + off::m_iHealth) > 0 &&
                          (!teammate || show_team);
            if (!active) continue;
            uintptr_t weapon = hide != HideMode::Off ? game::ActiveWeapon(pawn) : 0;
            targets.push_back({pawn, teammate ? cfg->chams_team_rgba : cfg->chams_visible_rgba, weapon});
            touched.insert(pawn);
            if (weapon) touched.insert(weapon);
        }
        {
            std::lock_guard<std::mutex> lock(s_targets_mtx);
            s_targets.swap(targets);
        }
        for (uintptr_t entity : s_modified)
            if (!touched.count(entity)) RestorePawn(entity);
        s_modified.swap(touched);
        return;
    }
    std::lock_guard<std::mutex> lock(s_targets_mtx);
    s_targets.clear();
}

static void HideLoop() {
    std::vector<HideTarget> targets;
    while (s_running.load()) {
        {
            std::lock_guard<std::mutex> lock(s_targets_mtx);
            targets = s_targets;
        }
        bool tint = s_tint.load();
        auto hide = static_cast<HideMode>(s_hide.load());
        for (const HideTarget& target : targets) {
            ApplyPawn(target.pawn, tint, target.rgba, hide);
            if (target.weapon) ApplyPawn(target.weapon, false, 0, hide);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(targets.empty() ? 20 : 1));
    }
}

void StartChams() {
    if (s_running.exchange(true)) return;
    s_thread = std::thread(HideLoop);
}

void StopChams() {
    if (!s_running.exchange(false)) return;
    if (s_thread.joinable()) s_thread.join();
}

}
