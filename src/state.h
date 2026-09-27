#pragma once
#include <atomic>
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

};

extern HudState g_hud;
