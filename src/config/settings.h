#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <string>
#include <vector>

constexpr uint32_t SETTINGS_MAGIC = 0x53505841u;
constexpr uint32_t SETTINGS_VERSION = 18;

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
    uint32_t hud_theme;
    uint32_t hud_accent_rgba;
    uint32_t silent_aim;
    uint32_t auto_scope;
    uint32_t long_jump;
    uint32_t slide_hop;
    uint32_t auto_pistol;
    uint32_t clan_tag_spin;
    uint32_t auto_accept;
    uint32_t damage_log;
    uint32_t auto_pickup;
    uint32_t bind_silent_aim;
    uint32_t bind_auto_scope;
    uint32_t bind_long_jump;
    uint32_t anti_aim;
    uint32_t night_mode_internal;
    uint32_t bind_anti_aim;
    uint32_t bind_night_mode_internal;
    uint32_t bind_thirdperson_internal;
    uint32_t thirdperson_internal;
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
    uint32_t lua_reload_token;
    uint32_t auto_strafe_mode;
    uint32_t autowall;
    int32_t  autowall_min_damage;
    uint32_t night_mode;
    int32_t  night_mode_strength;
    uint32_t bind_night_mode;
    uint32_t weather_mode;
    int32_t  weather_density;
    uint32_t night_sky;
    uint32_t ambient_tint;
    uint32_t ambient_tint_rgba;
    int32_t  world_brightness;
    int32_t  smoke_color_strength;
    uint32_t esp_flags;
    uint32_t trigger_autostop;
    uint32_t grenade_world;
    uint32_t fast_stop;
    uint32_t esp_ammo;
    uint32_t hit_sound;
    int32_t  hit_sound_volume;
    uint8_t  bind_modes[64];
    uint32_t bhop_method;
    uint32_t velocity_graph;
    int32_t  hud_velocity_x, hud_velocity_y;
    uint32_t trigger_force_shot;
    uint32_t bind_force_shot;
    uint32_t trigger_md_override;
    uint32_t bind_md_override;
    int32_t  md_override_value;
    uint32_t fast_stop_enabled;
    uint32_t bind_fast_stop;
    uint32_t fast_stop_mode;
    uint32_t saturation;
    int32_t  saturation_value;
    uint32_t hit_sound_kills_only;
    uint32_t min_damage_enabled;
    uint32_t fast_ladder;
    uint32_t media_player;
    int32_t  hud_spectators_x;
    int32_t  hud_spectators_y;
    int32_t  hud_media_x;
    int32_t  hud_media_y;
    uint32_t edge_bug;
    uint32_t bind_edge_bug;
    uint32_t edge_jump;
    uint32_t bind_edge_jump;
    uint32_t ladder_jump;
    uint32_t keystrokes;
    uint32_t notifications;
    uint32_t bind_keystrokes;
    uint32_t bind_notifications;
    int32_t  hud_keys_x;
    int32_t  hud_keys_y;
    int32_t  hud_notif_x;
    int32_t  hud_notif_y;
    uint32_t trigger_spread;
    uint32_t bind_spread_trigger;
    int32_t  spread_coverage;
    uint32_t spread_head_only;
    uint32_t gui_open;
    int32_t  gui_x, gui_y, gui_w, gui_h;
    uint32_t rage_auto_fire;
    uint32_t bind_rage_auto_fire;
    uint32_t grenade_helper;
    uint32_t grenade_helper_only_held;
    uint32_t grenade_helper_draw_line;
    uint32_t grenade_helper_aim;
    uint32_t bind_grenade_helper_aim;
    uint32_t bind_grenade_helper_save;
    uint32_t bind_grenade_helper_remove;
    uint32_t grenade_helper_spot_color_rgba;
    uint32_t grenade_helper_active_color_rgba;
    uint32_t grenade_helper_aim_color_rgba;
    int32_t  grenade_helper_save_token;
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

    enum class FieldKind { Toggle, Number, Color, Bind };

    struct FieldInfo {
        const char* name;
        FieldKind kind;
        size_t offset;
    };

    const std::vector<FieldInfo>& Fields();

    enum class BindMode : uint8_t { Toggle = 0, Hold = 1, Release = 2 };
    BindMode GetBindMode(const Settings& settings, const uint32_t* bind);
    void SetBindMode(Settings& settings, const uint32_t* bind, BindMode mode);
    const FieldInfo* FindField(const std::string& name);

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
