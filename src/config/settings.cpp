#include "settings.h"
#include "settings_fields.h"
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <pwd.h>

namespace settings {

static char s_path[512] = {0};

const char* Path() {
    if (s_path[0]) return s_path;
    const char* home = getenv("HOME");
    if (!home) { passwd* pw = getpwuid(getuid()); if (pw) home = pw->pw_dir; }
    if (!home) home = "/tmp";
    snprintf(s_path, sizeof(s_path), "%s/.config/spaxer", home);
    mkdir(s_path, 0755);
    snprintf(s_path, sizeof(s_path), "%s/.config/spaxer/settings.bin", home);
    return s_path;
}

static constexpr uint32_t kMigrationLevel = 9;

static void DefaultEspExtras(Settings& s) {
    s.sound_esp_rgba = 0x00CCFFC0;
    s.weapon_esp_rgba = 0x59D9FFF2;
    s.weapon_esp_distance_m = 30;
    s.arrows_rgba = 0xFF5252E6;
    s.arrows_radius = 140;
    s.arrows_size = 12;
}

static void DefaultHumanizer(Settings& s) {
    s.aimbot_humanize = 0;
    s.aimbot_speed_min_x100 = 20;
    s.aimbot_speed_max_x100 = 45;
    s.aimbot_shake_x100 = 15;
    s.aimbot_release_x100 = 35;
}

static void DefaultWorld(Settings& s) {
    s.night_sky = 1;
    s.ambient_tint = 1;
    s.ambient_tint_rgba = 0x0A184030;
    s.world_brightness = 100;
}

static void Migrate(Settings& s) {
    if (s.migration_level < 1) s.render_lead_ms = 30;
    if (s.migration_level < 2) DefaultEspExtras(s);
    if (s.migration_level < 3) DefaultHumanizer(s);
    if (s.migration_level < 4) s.autowall_min_damage = 20;
    if (s.migration_level < 5) s.night_mode_strength = 60;
    if (s.migration_level < 6) s.weather_density = 60;
    if (s.migration_level < 7) DefaultWorld(s);
    if (s.migration_level < 8) s.smoke_color_strength = 100;
    if (s.migration_level < 9) {
        s.esp_flags = 1;
        s.grenade_world = 1;
    }
    s.migration_level = kMigrationLevel;
}

static void DefaultChams(Settings& s) {
    s.chams_visible_rgba = 0xFF2D55B4;
    s.chams_hidden_rgba = 0xFFD60A8C;
    s.chams_team_rgba = 0x30D158A0;
}

void Defaults(Settings& s) {
    memset(&s, 0, sizeof(s));
    s.magic = SETTINGS_MAGIC;
    s.version = SETTINGS_VERSION;
    s.watermark = 1;
    s.bomb_timer = 1;
    s.crosshair = 1;
    s.esp = 1;
    s.esp_box = 1;
    s.esp_health = 1;
    s.esp_name = 1;
    s.esp_weapon = 0;
    s.esp_team_check = 1;
    s.esp_skeleton = 0;
    s.esp_head_circle = 1;
    s.esp_bone_debug = 0;
    s.sniper_crosshair = 1;
    s.no_flash = 0;
    s.no_smoke = 0;
    s.smoke_color_enabled = 0;
    s.smoke_color_rgba = 0xFF00FFFF;
    s.trigger_flash_check = 1;
    s.aimbot_flash_check = 1;
    s.rcs_enabled = 0;
    s.rcs_strength_x100 = 80;
    s.spectators = 1;
    s.keybinds = 1;
    s.radar_hack = 1;
    s.hitmarker = 1;
    s.sound_esp = 1;
    s.crosshair_size = 6;
    s.crosshair_gap  = 4;
    s.crosshair_thickness = 2;
    s.crosshair_color_rgba = 0x00FF78FF;
    s.crosshair_dot = 0;
    s.crosshair_outline = 1;
    s.trigger_enabled = 0;
    s.trigger_hitchance = 60;
    s.trigger_delay_ms  = 40;
    s.trigger_fov_x100  = 300;
    s.trigger_aim_correction = 0;
    s.trigger_shift_fire = 0;
    s.aimbot_enabled = 0;
    s.aimbot_fov_x100 = 500;
    s.aimbot_smooth_x100 = 35;
    s.aimbot_sens_x1000 = 100;
    s.aimbot_thru_walls = 1;
    s.aimbot_point_head = 1;
    s.aimbot_point_neck = 0;
    s.aimbot_point_chest = 1;
    s.aimbot_point_pelvis = 0;
    s.bind_aimbot = 0;
    for (WeaponSettings& weapon : s.weapon_settings) {
        weapon.aimbot_fov_x100 = s.aimbot_fov_x100;
        weapon.aimbot_smooth_x100 = s.aimbot_smooth_x100;
        weapon.trigger_hitchance = s.trigger_hitchance;
        weapon.trigger_delay_ms = s.trigger_delay_ms;
        weapon.rcs_strength_x100 = s.rcs_strength_x100;
    }
    s.snap_tap = 1;
    s.bunnyhop = 1;
    s.auto_strafe = 1;
    s.hud_wm_x = 20; s.hud_wm_y = 20;
    s.hud_bomb_x = -1; s.hud_bomb_y = 20;
    s.hud_keybinds_x = 20; s.hud_keybinds_y = 60;
    s.bind_toggle_gui = 0xff63;
    s.bind_edit_hud   = 0xffc5;
    s.bind_trigger    = 0;
    s.bind_watermark  = 0;
    s.bind_bomb       = 0;
    s.bind_crosshair  = 0;
    s.bind_esp        = 0;
    s.bind_snap_tap   = 0;
    s.bind_bunnyhop   = 0;
    s.bind_auto_strafe = 0;
    s.bind_keybinds   = 0;
    s.edit_mode = 0;
    s.arrows = 1;
    s.thirdperson = 0;
    s.esp_dropped_weapons = 1;
    s.grenade_trajectory = 1;
    s.bind_thirdperson = 0;
    s.bind_arrows = 0;
    s.glow = 0;
    s.glow_team = 0;
    s.glow_enemy_rgba = 0xFF3B30FF;
    s.glow_team_rgba = 0x0A84FFFF;
    s.bind_glow = 0;
    s.radar = 1;
    s.hud_radar_x = -1; s.hud_radar_y = -1;
    s.bhop_auto_jump = 1;
    s.aimbot_key_mode = 0;
    s.aimbot_target_mode = 0;
    s.aimbot_lock = 0;
    s.aimbot_switch_delay_ms = 150;
    DefaultChams(s);
    s.render_lead_ms = 30;
    DefaultEspExtras(s);
    DefaultHumanizer(s);
    s.autowall = 0;
    s.autowall_min_damage = 20;
    s.night_mode = 0;
    s.night_mode_strength = 60;
    s.bind_night_mode = 0;
    s.weather_mode = 0;
    s.weather_density = 60;
    DefaultWorld(s);
    s.smoke_color_strength = 100;
    s.esp_flags = 1;
    s.trigger_autostop = 0;
    s.grenade_world = 1;
    s.migration_level = kMigrationLevel;
}

const std::vector<FieldInfo>& Fields() {
#define SPAXER_FIELD_INFO(name, kind) FieldInfo{#name, FieldKind::kind, offsetof(Settings, name)},
    static const std::vector<FieldInfo> fields = {SPAXER_SETTINGS_FIELDS(SPAXER_FIELD_INFO)};
#undef SPAXER_FIELD_INFO
    return fields;
}

const FieldInfo* FindField(const std::string& name) {
    for (const FieldInfo& field : Fields())
        if (name == field.name) return &field;
    return nullptr;
}

Settings* Attach() {
    const char* path = Path();
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return nullptr;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return nullptr; }
    if (st.st_size < (off_t)sizeof(Settings)) {
        if (ftruncate(fd, sizeof(Settings)) < 0) { close(fd); return nullptr; }
    }
    void* p = mmap(nullptr, sizeof(Settings), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return nullptr;
    Settings* s = static_cast<Settings*>(p);
    if (s->magic != SETTINGS_MAGIC || s->version != SETTINGS_VERSION) {
        Settings def;
        Defaults(def);
        *s = def;
    }
    if (!s->chams_visible_rgba && !s->chams_hidden_rgba && !s->chams_team_rgba) DefaultChams(*s);
    if (s->migration_level < kMigrationLevel) Migrate(*s);
    return s;
}

static void HomeDir(char* out, size_t n) {
    const char* home = getenv("HOME");
    if (!home) { passwd* pw = getpwuid(getuid()); if (pw) home = pw->pw_dir; }
    if (!home) home = "/tmp";
    snprintf(out, n, "%s", home);
}

static void ConfigsDir(char* out, size_t n) {
    char home[448] = {0};
    HomeDir(home, sizeof(home));
    snprintf(out, n, "%s/.config/spaxer/configs", home);
}

static bool CleanName(const std::string& in, std::string& out) {
    out.clear();
    if (in.empty() || in.size() > 32) return false;
    for (char c : in) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
        out.push_back(c);
    }
    return !out.empty();
}

bool SaveConfig(const std::string& name) {
    std::string clean;
    if (!CleanName(name, clean)) return false;
    char dir[512] = {0};
    ConfigsDir(dir, sizeof(dir));
    char home[448] = {0};
    HomeDir(home, sizeof(home));
    char base[512] = {0};
    snprintf(base, sizeof(base), "%s/.config/spaxer", home);
    mkdir(base, 0755);
    mkdir(dir, 0755);
    char path[576] = {0};
    snprintf(path, sizeof(path), "%s/%s.bin", dir, clean.c_str());
    Settings* s = Attach();
    if (!s) return false;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    ssize_t w = write(fd, s, sizeof(Settings));
    close(fd);
    if (w != (ssize_t)sizeof(Settings)) return false;
    RememberConfig(clean);
    return true;
}

bool LoadConfig(const std::string& name) {
    std::string clean;
    if (!CleanName(name, clean)) return false;
    char dir[512] = {0};
    ConfigsDir(dir, sizeof(dir));
    char path[576] = {0};
    snprintf(path, sizeof(path), "%s/%s.bin", dir, clean.c_str());
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    Settings tmp{};
    Defaults(tmp);
    ssize_t r = read(fd, &tmp, sizeof(tmp));
    close(fd);
    if (r < (ssize_t)sizeof(uint32_t) * 2) return false;
    if (tmp.magic != SETTINGS_MAGIC) return false;
    tmp.magic = SETTINGS_MAGIC;
    tmp.version = SETTINGS_VERSION;
    Settings* s = Attach();
    if (!s) return false;
    uint32_t saved_edit_mode = s->edit_mode;
    *s = tmp;
    s->edit_mode = saved_edit_mode;
    RememberConfig(clean);
    return true;
}

bool DeleteConfig(const std::string& name) {
    std::string clean;
    if (!CleanName(name, clean)) return false;
    char dir[512] = {0};
    ConfigsDir(dir, sizeof(dir));
    char path[576] = {0};
    snprintf(path, sizeof(path), "%s/%s.bin", dir, clean.c_str());
    return unlink(path) == 0;
}

std::vector<std::string> ListConfigs() {
    std::vector<std::string> out;
    char dir[512] = {0};
    ConfigsDir(dir, sizeof(dir));
    DIR* d = opendir(dir);
    if (!d) return out;
    dirent* e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n <= 4 || strcmp(e->d_name + n - 4, ".bin") != 0) continue;
        std::string clean;
        if (CleanName(std::string(e->d_name, n - 4), clean)) out.push_back(clean);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::string LastConfig() {
    char home[448] = {0};
    HomeDir(home, sizeof(home));
    char path[512] = {0};
    snprintf(path, sizeof(path), "%s/.config/spaxer/last.cfg", home);
    FILE* f = fopen(path, "r");
    if (!f) return {};
    char buf[40] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) buf[--n] = 0;
    std::string clean;
    if (!CleanName(buf, clean)) return {};
    return clean;
}

void RememberConfig(const std::string& name) {
    std::string clean;
    if (!CleanName(name, clean)) return;
    char home[448] = {0};
    HomeDir(home, sizeof(home));
    char base[512] = {0};
    snprintf(base, sizeof(base), "%s/.config/spaxer", home);
    mkdir(base, 0755);
    char path[512] = {0};
    snprintf(path, sizeof(path), "%s/.config/spaxer/last.cfg", home);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fwrite(clean.c_str(), 1, clean.size(), f);
    fclose(f);
}

}
