#pragma once
#include <atomic>
#include <chrono>
#include <string>
#include <mutex>
#include <vector>
#include <cstdint>

struct EspEntry {
    float world_head_x, world_head_y, world_head_z;
    float world_feet_x, world_feet_y, world_feet_z;
    int   hp;
    int   team;
    bool  visible;
    bool  spotted_valid;
    uintptr_t pawn;
    char  model[40];
    char  name[32];
    char  weapon[24];
    uint32_t flags;
    int   ammo;
    int   max_ammo;
};

enum EspFlag : uint32_t {
    kFlagFlashed = 1u << 0,
    kFlagBomb = 1u << 1,
    kFlagDefusing = 1u << 2,
    kFlagKit = 1u << 3,
    kFlagScoped = 1u << 4,
    kFlagReloading = 1u << 5,
    kFlagArmor = 1u << 6,
    kFlagHelmet = 1u << 7,
};

enum class GrenadeKind : int { He, Flash, Smoke, Fire, Decoy };

struct ThrownGrenade {
    uintptr_t entity;
    GrenadeKind kind;
    int team;
    double seen_at;
};

struct SpectatorEntry {
    int team;
    char name[32];
};

struct DroppedItemEntry {
    float world_x, world_y, world_z;
    char name[32];
};

enum class NoticeKind : int { Info, Hit, Kill, On, Off, Bomb };

struct Notice {
    std::string text;
    NoticeKind kind;
    std::chrono::steady_clock::time_point time;
};

struct GameEventRecord {
    std::string name;
    std::vector<std::pair<std::string, double>> numbers;
    std::vector<std::pair<std::string, std::string>> strings;
};

struct HudState {
    std::atomic<bool>  bomb_visible{false};
    std::atomic<float> bomb_blow_secs{0.f};
    std::atomic<int>   bomb_site{0};
    std::atomic<bool>  bomb_being_defused{false};
    std::atomic<float> bomb_defuse_secs{0.f};

    std::atomic<bool> attached{false};
    std::atomic<bool> in_game{false};
    std::atomic<bool> cs2_focused{false};
    std::atomic<int>  local_ping{-1};
    std::atomic<int>  overlay_fps{0};
    std::atomic<int>  local_team{0};
    std::atomic<int>  screen_w{1920};
    std::atomic<int>  screen_h{1080};

    std::mutex           esp_mtx;
    std::vector<EspEntry> esp_players;

    std::mutex                 spectators_mtx;
    std::vector<SpectatorEntry> spectators;

    std::mutex                   items_mtx;
    std::vector<DroppedItemEntry> dropped_items;

    std::mutex                 grenades_mtx;
    std::vector<ThrownGrenade> thrown_grenades;

    std::mutex media_mtx;
    char media_title[128]{};
    char media_artist[128]{};
    char media_status[32]{};
    char media_player_name[64]{};
    bool media_active{false};

    std::mutex          notices_mtx;
    std::vector<Notice> notices;

    std::mutex                   game_events_mtx;
    std::vector<GameEventRecord> game_events;
};

extern HudState g_hud;

inline void PushGameEvent(GameEventRecord event) {
    std::lock_guard<std::mutex> lock(g_hud.game_events_mtx);
    g_hud.game_events.push_back(std::move(event));
    if (g_hud.game_events.size() > 64) g_hud.game_events.erase(g_hud.game_events.begin());
}

inline void PushNotice(std::string text, NoticeKind kind) {
    std::lock_guard<std::mutex> lock(g_hud.notices_mtx);
    g_hud.notices.push_back({std::move(text), kind, std::chrono::steady_clock::now()});
    if (g_hud.notices.size() > 8) g_hud.notices.erase(g_hud.notices.begin());
}
