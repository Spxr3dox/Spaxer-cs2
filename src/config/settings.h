#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <string>
#include <vector>

constexpr uint32_t SETTINGS_MAGIC = 0x53505841u;
constexpr uint32_t SETTINGS_VERSION = 17;

struct WeaponSettings {
    uint32_t enabled;
    uint32_t aimbot_enabled;
    uint32_t trigger_enabled;
    uint32_t rcs_enabled;
    int32_t aimbot_fov_x100;
    int32_t aimbot_smooth_x100;
    int32_t trigger_hitchance;
    int32_t trigger_delay_ms;
    int32_t rcs_strength_x100;
};

struct Settings {
    uint32_t magic;
    uint32_t version;

    uint32_t watermark;
    uint32_t bomb_timer;
    uint32_t crosshair;
    uint32_t esp;
    uint32_t esp_box;
    uint32_t esp_health;
    uint32_t esp_name;
    uint32_t esp_weapon;
    uint32_t esp_team_check;
    uint32_t esp_skeleton;
    uint32_t esp_head_circle;
    uint32_t esp_bone_debug;
    uint32_t sniper_crosshair;
    uint32_t no_flash;
    uint32_t no_smoke;
    uint32_t smoke_color_enabled;
    uint32_t smoke_color_rgba;
    uint32_t trigger_flash_check;
    uint32_t aimbot_flash_check;
    uint32_t rcs_enabled;
    int32_t  rcs_strength_x100;
    uint32_t spectators;
    uint32_t keybinds;
    uint32_t radar_hack;
    uint32_t hitmarker;
    uint32_t sound_esp;

    uint32_t crosshair_size;
    int32_t  crosshair_gap;
    uint32_t crosshair_thickness;
    uint32_t crosshair_color_rgba;
    uint32_t crosshair_dot;
    uint32_t crosshair_outline;

    uint32_t trigger_enabled;
    int32_t  trigger_hitchance;
    int32_t  trigger_delay_ms;
    int32_t  trigger_fov_x100;
    uint32_t trigger_aim_correction;
    uint32_t trigger_shift_fire;
    uint32_t aimbot_enabled;
    int32_t  aimbot_fov_x100;
    int32_t  aimbot_smooth_x100;
    int32_t  aimbot_sens_x1000;
    uint32_t aimbot_thru_walls;
    uint32_t aimbot_point_head;
    uint32_t aimbot_point_neck;
    uint32_t aimbot_point_chest;
    uint32_t aimbot_point_pelvis;
    uint32_t bind_aimbot;

    WeaponSettings weapon_settings[65];

    uint32_t snap_tap;
    uint32_t bunnyhop;
    uint32_t auto_strafe;

    int32_t hud_wm_x, hud_wm_y;
    int32_t hud_bomb_x, hud_bomb_y;
    int32_t hud_keybinds_x, hud_keybinds_y;

    uint32_t bind_toggle_gui;
    uint32_t bind_edit_hud;
    uint32_t bind_trigger;
    uint32_t bind_watermark;
    uint32_t bind_bomb;
    uint32_t bind_crosshair;
    uint32_t bind_esp;
    uint32_t bind_snap_tap;
    uint32_t bind_bunnyhop;
    uint32_t bind_auto_strafe;
    uint32_t bind_keybinds;

    uint32_t edit_mode;

    uint32_t arrows;
    uint32_t thirdperson;
    uint32_t esp_dropped_weapons;
    uint32_t grenade_trajectory;
    uint32_t bind_thirdperson;
    uint32_t bind_arrows;
    uint32_t glow;
    uint32_t glow_team;
    uint32_t glow_enemy_rgba;
    uint32_t glow_team_rgba;
    uint32_t bind_glow;
    uint32_t radar;
    int32_t  hud_radar_x, hud_radar_y;
    uint32_t bhop_auto_jump;
    uint32_t aimbot_key_mode;
    uint32_t aimbot_target_mode;
    uint32_t aimbot_lock;
    int32_t  aimbot_switch_delay_ms;
    uint32_t chams;
    uint32_t chams_team;
    uint32_t chams_tint;
    uint32_t chams_visible_rgba;
    uint32_t chams_hidden_rgba;
    uint32_t chams_team_rgba;
    uint32_t bind_chams;
    uint32_t chams_material;
    uint32_t chams_hide_model;
    int32_t  render_lead_ms;
    uint32_t migration_level;
    uint32_t bind_sound_esp;
    uint32_t sound_esp_rgba;
    uint32_t bind_weapon_esp;
    uint32_t weapon_esp_rgba;
    int32_t  weapon_esp_distance_m;
    uint32_t arrows_rgba;
    int32_t  arrows_radius;
    int32_t  arrows_size;
    uint32_t aimbot_humanize;
    int32_t  aimbot_speed_min_x100;
    int32_t  aimbot_speed_max_x100;
    int32_t  aimbot_shake_x100;
    int32_t  aimbot_release_x100;
};

namespace settings {
    inline bool Enabled(const uint32_t& value) {
        return __atomic_load_n(&value, __ATOMIC_RELAXED) != 0;
    }

    inline void SetEnabled(uint32_t& value, bool enabled) {
        uint32_t val = enabled ? 1u : 0u;
        __atomic_store_n(&value, val, __ATOMIC_RELAXED);
    }

    inline void ToggleEnabled(uint32_t& value) {
        __atomic_fetch_xor(&value, 1u, __ATOMIC_RELAXED);
    }

    inline WeaponSettings* WeaponFor(Settings& settings, int definition) {
        if (definition <= 0 || definition >= 65) return nullptr;
        WeaponSettings& weapon = settings.weapon_settings[definition];
        return Enabled(weapon.enabled) ? &weapon : nullptr;
    }

    Settings* Attach();
    void Defaults(Settings& s);
    const char* Path();

    bool SaveConfig(const std::string& name);
    bool LoadConfig(const std::string& name);
    bool DeleteConfig(const std::string& name);
    std::vector<std::string> ListConfigs();
    std::string LastConfig();
    void RememberConfig(const std::string& name);
}
