#include "state.h"
#include "sdk/game.h"
#include "memory/process.h"
#include "config/settings.h"
#include <chrono>

HudState g_hud;

namespace features {

void UpdatePlayer() {
    uintptr_t pawn = game::LocalPawn();
    if (!pawn) { g_hud.in_game.store(false); return; }
    int hp = g_proc.Read<int>(pawn + off::m_iHealth);
    g_hud.in_game.store(hp > 0);

    if (off::m_iPing) {
        uintptr_t ctrl = game::LocalController();
        if (ctrl) {
            int p = g_proc.Read<int>(ctrl + off::m_iPing);
            if (p >= 0 && p < 999) g_hud.local_ping.store(p);
        }
    }
}

static float NowSecs() {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<float>(clock::now() - t0).count();
}

static float s_plant_anchor  = 0.f;
static bool  s_was_defusing  = false;
static float s_defuse_anchor = 0.f;
static float s_defuse_len    = 10.f;

void UpdateBomb() {
    uintptr_t bomb = game::PlantedC4();
    if (!bomb) {
        g_hud.bomb_visible.store(false);
        s_plant_anchor = 0.f; s_was_defusing = false;
        return;
    }

    bool ticking = g_proc.Read<bool>(bomb + off::m_bBombTicking);
    bool defused = g_proc.Read<bool>(bomb + off::m_bBombDefused);
    float planted_blow = off::m_flC4Blow ? g_proc.Read<float>(bomb + off::m_flC4Blow) : 0.f;
    if (defused || (!ticking && planted_blow <= 0.f)) {
        g_hud.bomb_visible.store(false);
        s_plant_anchor = 0.f; s_was_defusing = false;
        return;
    }

    float blow_time = off::m_flC4Blow ? g_proc.Read<float>(bomb + off::m_flC4Blow) : 0.f;
    float timer_len = off::m_flTimerLength ? g_proc.Read<float>(bomb + off::m_flTimerLength) : 40.f;
    if (timer_len < 5.f || timer_len > 90.f) timer_len = 40.f;

    float now = NowSecs();
    if (blow_time > 0.f) {
        if (s_plant_anchor <= 0.f) s_plant_anchor = now;
        float elapsed = now - s_plant_anchor;
        float remain = timer_len - elapsed;
        if (remain < 0.f) {
            g_hud.bomb_visible.store(false);
            return;
        }
        int site = off::m_nBombSite ? g_proc.Read<int>(bomb + off::m_nBombSite) : 0;
        bool being_def = off::m_bBeingDefused ? g_proc.Read<bool>(bomb + off::m_bBeingDefused) : false;
        float d_remain = -1.f;
        if (being_def) {
            if (!s_was_defusing) {
                s_defuse_anchor = now;
                float dl = off::m_flDefuseLength ? g_proc.Read<float>(bomb + off::m_flDefuseLength) : 10.f;
                s_defuse_len = (dl >= 1.f && dl <= 15.f) ? dl : 10.f;
                s_was_defusing = true;
            }
            d_remain = s_defuse_len - (now - s_defuse_anchor);
            if (d_remain < 0.f) d_remain = 0.f;
        } else {
            s_was_defusing = false;
        }
        if (!g_hud.bomb_visible.load()) {
            Settings* notify_cfg = settings::Attach();
            if (notify_cfg && settings::Enabled(notify_cfg->notifications))
                PushNotice(std::string("Bomb planted at ") + (site == 0 ? "A" : "B"), NoticeKind::Bomb);
        }
        g_hud.bomb_visible.store(true);
        g_hud.bomb_blow_secs.store(remain);
        g_hud.bomb_site.store(site);
        g_hud.bomb_being_defused.store(being_def);
        g_hud.bomb_defuse_secs.store(d_remain);
    } else {
        g_hud.bomb_visible.store(false);
    }
}

}
