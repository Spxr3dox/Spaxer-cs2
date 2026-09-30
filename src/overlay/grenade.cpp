#include "overlay/grenade.h"
#include "config/settings.h"
#include "input/input.h"
#include "sdk/visibility.h"
#include "state.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <linux/input.h>
#include <gdk/gdkkeysyms.h>

namespace grenade {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDegToRad = kPi / 180.f;
constexpr float kTickSeconds = 1.f / 64.f;
constexpr float kGravity = 320.f;
constexpr float kElasticity = 0.45f;
constexpr float kMaxThrowSpeed = 750.f * 0.9f;
constexpr float kStopSpeed = 20.f;
constexpr float kTimedFuse = 1.5f;
constexpr float kFireFuse = 2.f;
constexpr float kMaxSimulation = 12.f;
constexpr float kFloorNormalZ = 0.7f;
constexpr float kFireDetonateNormalZ = 0.866f;
constexpr float kSurfaceOffset = 0.5f;
constexpr float kMovingSpeed = 5.f;

struct PathPoint { Vec3 pos; bool bounce; };

struct Style { double r, g, b, a; };

constexpr Style kOwnStyle{1.0, 0.62, 0.2, 1.0};
constexpr Style kEnemyStyle{1.0, 0.3, 0.28, 1.0};
constexpr Style kFriendlyStyle{0.35, 0.8, 1.0, 1.0};

static inline Style ColorFromRgba(uint32_t rgba, double default_a = 1.0) {
    double r = ((rgba >> 24) & 0xFF) / 255.0;
    double g = ((rgba >> 16) & 0xFF) / 255.0;
    double b = ((rgba >>  8) & 0xFF) / 255.0;
    double a = ( rgba        & 0xFF) / 255.0;
    if (a <= 0.0) a = default_a;
    return {r, g, b, a};
}

static inline float NormAngle(float a) {
    while (a > 180.f) a -= 360.f;
    while (a < -180.f) a += 360.f;
    return a;
}

static inline Vec3 AngleToForward(float pitch, float yaw) {
    float rad_p = pitch * kDegToRad;
    float rad_y = yaw * kDegToRad;
    float cp = std::cos(rad_p);
    float sp = std::sin(rad_p);
    float cy = std::cos(rad_y);
    float sy = std::sin(rad_y);
    return {cp * cy, cp * sy, -sp};
}

static int LinuxKeyFromGdk(uint32_t keyval) {
    switch (keyval) {
        case GDK_KEY_F1: return KEY_F1;
        case GDK_KEY_F2: return KEY_F2;
        case GDK_KEY_F3: return KEY_F3;
        case GDK_KEY_F4: return KEY_F4;
        case GDK_KEY_F5: return KEY_F5;
        case GDK_KEY_F6: return KEY_F6;
        case GDK_KEY_F7: return KEY_F7;
        case GDK_KEY_F8: return KEY_F8;
        case GDK_KEY_F9: return KEY_F9;
        case GDK_KEY_F10: return KEY_F10;
        case GDK_KEY_F11: return KEY_F11;
        case GDK_KEY_F12: return KEY_F12;
        case GDK_KEY_Insert: return KEY_INSERT;
        case GDK_KEY_Delete: return KEY_DELETE;
        case GDK_KEY_Home: return KEY_HOME;
        case GDK_KEY_End: return KEY_END;
        case GDK_KEY_Page_Up: return KEY_PAGEUP;
        case GDK_KEY_Page_Down: return KEY_PAGEDOWN;
        case GDK_KEY_space: return KEY_SPACE;
        case GDK_KEY_Shift_L: case GDK_KEY_Shift_R: return KEY_LEFTSHIFT;
        case GDK_KEY_Control_L: case GDK_KEY_Control_R: return KEY_LEFTCTRL;
        case GDK_KEY_Alt_L: case GDK_KEY_Alt_R: return KEY_LEFTALT;
        default: return -1;
    }
}

static bool IsBindActive(uint32_t kv) {
    if (kv == 0) return false;
    int lk = LinuxKeyFromGdk(kv);
    if (lk >= 0 && g_input.IsKeyDown(lk)) return true;
    if (kv == 1 || kv == 2 || kv == 3 || kv == 4 || kv == 5) return g_input.IsMouseDown(static_cast<int>(kv));
    return false;
}

const char* ThrowTypeStr(ThrowType t) {
    switch (t) {
        case ThrowType::Standing: return "Standing";
        case ThrowType::Jumpthrow: return "Jumpthrow";
        case ThrowType::Runthrow: return "Runthrow";
        case ThrowType::Crouch: return "Crouch";
        default: return "Standing";
    }
}

static const std::vector<LineupSpot> kBuiltinLineups = {
    {"Window Smoke", "de_mirage", GrenadeKind::Smoke, ThrowType::Jumpthrow, {-1236.f, -708.f, -159.f}, -22.5f, -86.5f, false},
    {"Top Mid Smoke", "de_mirage", GrenadeKind::Smoke, ThrowType::Standing, {-1152.f, -384.f, -159.f}, -28.0f, -65.0f, false},
    {"Connector Smoke", "de_mirage", GrenadeKind::Smoke, ThrowType::Jumpthrow, {-1216.f, -320.f, -159.f}, -34.0f, -80.0f, false},
    {"A Jungle Smoke", "de_mirage", GrenadeKind::Smoke, ThrowType::Standing, {-512.f, -640.f, -63.f}, -31.0f, -44.0f, false},
    {"A Stairs Smoke", "de_mirage", GrenadeKind::Smoke, ThrowType::Standing, {-580.f, -690.f, -63.f}, -38.5f, -38.0f, false},
    {"A CT Spawn Smoke", "de_mirage", GrenadeKind::Smoke, ThrowType::Jumpthrow, {-640.f, -730.f, -63.f}, -29.0f, -27.0f, false},
    {"B Market Door Smoke", "de_mirage", GrenadeKind::Smoke, ThrowType::Standing, {-1050.f, 750.f, -95.f}, -25.0f, 15.0f, false},
    {"B Short Smoke", "de_mirage", GrenadeKind::Smoke, ThrowType::Jumpthrow, {-1120.f, 850.f, -95.f}, -30.0f, 35.0f, false},
    {"A Ramp Flash", "de_mirage", GrenadeKind::Flash, ThrowType::Standing, {-200.f, -600.f, -63.f}, -45.0f, -135.0f, false},
    {"Mid Flash", "de_mirage", GrenadeKind::Flash, ThrowType::Standing, {-900.f, -500.f, -159.f}, -50.0f, -70.0f, false},

    {"Coffins Smoke", "de_inferno", GrenadeKind::Smoke, ThrowType::Standing, {550.f, 1550.f, 96.f}, -35.0f, 78.0f, false},
    {"CT Spawn Smoke", "de_inferno", GrenadeKind::Smoke, ThrowType::Jumpthrow, {680.f, 1420.f, 96.f}, -42.0f, 65.0f, false},
    {"Banana Car Molotov", "de_inferno", GrenadeKind::Fire, ThrowType::Jumpthrow, {900.f, 800.f, 32.f}, -30.0f, 85.0f, false},
    {"A Long Corner Smoke", "de_inferno", GrenadeKind::Smoke, ThrowType::Standing, {-250.f, 400.f, 64.f}, -28.0f, -15.0f, false},
    {"A Pit Flash", "de_inferno", GrenadeKind::Flash, ThrowType::Standing, {1300.f, 180.f, 160.f}, -40.0f, -90.0f, false},

    {"Xbox Smoke", "de_dust2", GrenadeKind::Smoke, ThrowType::Standing, {-1100.f, -700.f, 160.f}, -32.0f, 40.0f, false},
    {"A Long Cross Smoke", "de_dust2", GrenadeKind::Smoke, ThrowType::Jumpthrow, {400.f, -800.f, 64.f}, -25.0f, 110.0f, false},
    {"CT Spawn Smoke", "de_dust2", GrenadeKind::Smoke, ThrowType::Standing, {1100.f, -500.f, 64.f}, -38.0f, 160.0f, false},
    {"B Doors Smoke", "de_dust2", GrenadeKind::Smoke, ThrowType::Standing, {-750.f, 1100.f, 64.f}, -22.0f, 10.0f, false},
    {"Mid to B Smoke", "de_dust2", GrenadeKind::Smoke, ThrowType::Jumpthrow, {-300.f, 400.f, -64.f}, -40.0f, 30.0f, false},

    {"Outside Garage Smoke", "de_nuke", GrenadeKind::Smoke, ThrowType::Standing, {-2500.f, -1300.f, -200.f}, -30.0f, 45.0f, false},
    {"Outside Cross 1 Smoke", "de_nuke", GrenadeKind::Smoke, ThrowType::Jumpthrow, {-2100.f, -1050.f, 50.f}, -26.0f, 38.0f, false},
    {"Outside Cross 2 Smoke", "de_nuke", GrenadeKind::Smoke, ThrowType::Jumpthrow, {-2100.f, -1000.f, 50.f}, -28.0f, 42.0f, false},
    {"A Site Hut Molotov", "de_nuke", GrenadeKind::Fire, ThrowType::Standing, {-1400.f, -800.f, -200.f}, -45.0f, 15.0f, false},

    {"Donut Smoke", "de_ancient", GrenadeKind::Smoke, ThrowType::Jumpthrow, {-1600.f, -800.f, 64.f}, -35.0f, 60.0f, false},
    {"B Cave Smoke", "de_ancient", GrenadeKind::Smoke, ThrowType::Standing, {-400.f, -1600.f, 64.f}, -28.0f, 90.0f, false},
    {"A Site CT Smoke", "de_ancient", GrenadeKind::Smoke, ThrowType::Jumpthrow, {400.f, -400.f, 64.f}, -33.0f, 120.0f, false},

    {"Mid Bridge Smoke", "de_anubis", GrenadeKind::Smoke, ThrowType::Jumpthrow, {-1500.f, 400.f, 32.f}, -32.0f, -20.0f, false},
    {"A Camera Smoke", "de_anubis", GrenadeKind::Smoke, ThrowType::Standing, {100.f, 1200.f, 64.f}, -30.0f, -75.0f, false},
    {"B Connector Smoke", "de_anubis", GrenadeKind::Smoke, ThrowType::Jumpthrow, {-500.f, -1000.f, 64.f}, -28.0f, 45.0f, false},

    {"A Ramp Smoke", "de_vertigo", GrenadeKind::Smoke, ThrowType::Standing, {-800.f, -200.f, -100.f}, -35.0f, 20.0f, false},
    {"Mid Boost Smoke", "de_vertigo", GrenadeKind::Smoke, ThrowType::Jumpthrow, {-300.f, 600.f, -100.f}, -40.0f, 75.0f, false},
    {"B Site Stairs Smoke", "de_vertigo", GrenadeKind::Smoke, ThrowType::Standing, {400.f, -400.f, -100.f}, -30.0f, -110.0f, false},

    {"Monster Smoke", "de_overpass", GrenadeKind::Smoke, ThrowType::Standing, {-2200.f, 1200.f, 100.f}, -32.0f, -45.0f, false},
    {"B Short Smoke", "de_overpass", GrenadeKind::Smoke, ThrowType::Jumpthrow, {-1800.f, 800.f, 100.f}, -38.0f, -15.0f, false},
    {"A Long Smoke", "de_overpass", GrenadeKind::Smoke, ThrowType::Standing, {-400.f, -1200.f, 200.f}, -26.0f, 80.0f, false}
};

static std::vector<LineupSpot> s_custom_lineups;
static bool s_custom_loaded = false;

static std::string LineupsFilePath() {
    const char* home = getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.config/spaxer/lineups.txt";
}

}

void LoadCustomLineups() {
    s_custom_lineups.clear();
    s_custom_loaded = true;
    std::ifstream file(LineupsFilePath());
    if (!file.is_open()) return;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string map, name, kind_str, throw_str, x_str, y_str, z_str, pitch_str, yaw_str;
        if (!std::getline(ss, map, '|') || !std::getline(ss, name, '|') || !std::getline(ss, kind_str, '|') ||
            !std::getline(ss, throw_str, '|') || !std::getline(ss, x_str, '|') || !std::getline(ss, y_str, '|') ||
            !std::getline(ss, z_str, '|') || !std::getline(ss, pitch_str, '|') || !std::getline(ss, yaw_str, '|'))
            continue;
        LineupSpot spot;
        spot.map = map;
        spot.name = name;
        spot.kind = static_cast<GrenadeKind>(std::atoi(kind_str.c_str()));
        spot.throw_type = static_cast<ThrowType>(std::atoi(throw_str.c_str()));
        spot.pos.x = std::atof(x_str.c_str());
        spot.pos.y = std::atof(y_str.c_str());
        spot.pos.z = std::atof(z_str.c_str());
        spot.pitch = std::atof(pitch_str.c_str());
        spot.yaw = std::atof(yaw_str.c_str());
        spot.custom = true;
        s_custom_lineups.push_back(spot);
    }
}

void SaveCustomLineups() {
    std::ofstream file(LineupsFilePath());
    if (!file.is_open()) return;
    for (const LineupSpot& spot : s_custom_lineups) {
        file << spot.map << "|" << spot.name << "|" << static_cast<int>(spot.kind) << "|"
             << static_cast<int>(spot.throw_type) << "|" << spot.pos.x << "|" << spot.pos.y << "|"
             << spot.pos.z << "|" << spot.pitch << "|" << spot.yaw << "\n";
    }
}

std::vector<LineupSpot> GetLineupsForMap(const std::string& map) {
    if (!s_custom_loaded) LoadCustomLineups();
    std::vector<LineupSpot> result;
    for (const LineupSpot& spot : kBuiltinLineups) {
        if (spot.map == map || map.empty()) result.push_back(spot);
    }
    for (const LineupSpot& spot : s_custom_lineups) {
        if (spot.map == map || map.empty()) result.push_back(spot);
    }
    return result;
}

const std::vector<LineupSpot>& GetAllLineups() {
    if (!s_custom_loaded) LoadCustomLineups();
    return s_custom_lineups;
}

float FuseFor(GrenadeKind kind) {
    switch (kind) {
        case GrenadeKind::He: case GrenadeKind::Flash: return kTimedFuse;
        case GrenadeKind::Fire: return kFireFuse;
        default: return kMaxSimulation;
    }
}

namespace {

const char* LabelFor(GrenadeKind kind) {
    switch (kind) {
        case GrenadeKind::He: return "HE";
        case GrenadeKind::Flash: return "FLASH";
        case GrenadeKind::Smoke: return "SMOKE";
        case GrenadeKind::Fire: return "FIRE";
        default: return "DECOY";
    }
}

float EffectRadius(GrenadeKind kind) {
    switch (kind) {
        case GrenadeKind::He: return 350.f;
        case GrenadeKind::Smoke: return 144.f;
        case GrenadeKind::Fire: return 150.f;
        default: return 0.f;
    }
}

bool KindForWeapon(int definition, GrenadeKind& kind) {
    switch (definition) {
        case 43: kind = GrenadeKind::Flash; return true;
        case 44: kind = GrenadeKind::He; return true;
        case 45: kind = GrenadeKind::Smoke; return true;
        case 46: case 48: kind = GrenadeKind::Fire; return true;
        case 47: kind = GrenadeKind::Decoy; return true;
        default: return false;
    }
}

}

bool AddCurrentSpot(const std::string& custom_name, ThrowType throw_type) {
    uintptr_t pawn = game::LocalPawn();
    if (!pawn) {
        PushNotice("Local player not found", NoticeKind::Off);
        return false;
    }
    std::string map = vis::CurrentMap();
    if (map.empty()) {
        PushNotice("Not in map", NoticeKind::Off);
        return false;
    }
    if (!s_custom_loaded) LoadCustomLineups();
    Vec3 pos = game::Origin(pawn);
    Vec3 ang = off::m_angEyeAngles ? g_proc.Read<Vec3>(pawn + off::m_angEyeAngles) : Vec3{};
    GrenadeKind kind = GrenadeKind::Smoke;
    KindForWeapon(game::ActiveWeaponDefinitionIndex(pawn), kind);
    std::string name = custom_name.empty() ? (std::string(LabelFor(kind)) + " Lineup") : custom_name;
    LineupSpot spot{name, map, kind, throw_type, pos, ang.x, ang.y, true};
    s_custom_lineups.push_back(spot);
    SaveCustomLineups();
    PushNotice("Saved lineup: " + name, NoticeKind::Info);
    return true;
}

bool RemoveNearestSpot() {
    uintptr_t pawn = game::LocalPawn();
    if (!pawn) return false;
    std::string map = vis::CurrentMap();
    if (map.empty()) return false;
    if (!s_custom_loaded) LoadCustomLineups();
    Vec3 pos = game::Origin(pawn);
    float nearest_dist = 250.f;
    int nearest_idx = -1;
    for (size_t i = 0; i < s_custom_lineups.size(); i++) {
        if (s_custom_lineups[i].map != map) continue;
        float dx = s_custom_lineups[i].pos.x - pos.x;
        float dy = s_custom_lineups[i].pos.y - pos.y;
        float dz = s_custom_lineups[i].pos.z - pos.z;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist < nearest_dist) {
            nearest_dist = dist;
            nearest_idx = static_cast<int>(i);
        }
    }
    if (nearest_idx >= 0) {
        std::string removed_name = s_custom_lineups[nearest_idx].name;
        s_custom_lineups.erase(s_custom_lineups.begin() + nearest_idx);
        SaveCustomLineups();
        PushNotice("Removed lineup: " + removed_name, NoticeKind::Info);
        return true;
    }
    PushNotice("No nearby custom lineup", NoticeKind::Off);
    return false;
}

std::vector<PathPoint> Fly(Vec3 pos, Vec3 vel, GrenadeKind kind, float time_left) {
    std::vector<PathPoint> path;
    path.push_back({pos, false});
    for (float t = 0.f; t < time_left; t += kTickSeconds) {
        Vec3 next{pos.x + vel.x * kTickSeconds, pos.y + vel.y * kTickSeconds,
                  pos.z + vel.z * kTickSeconds - 0.5f * kGravity * kTickSeconds * kTickSeconds};
        vel.z -= kGravity * kTickSeconds;
        Vec3 hit, normal;
        if (!vis::Raycast(pos, next, hit, &normal, vis::Blocks::Grenades)) {
            pos = next;
            path.push_back({pos, false});
            continue;
        }
        pos = {hit.x + normal.x * kSurfaceOffset, hit.y + normal.y * kSurfaceOffset, hit.z + normal.z * kSurfaceOffset};
        path.push_back({pos, true});
        if (kind == GrenadeKind::Fire && normal.z >= kFireDetonateNormalZ) break;
        float into = vel.x * normal.x + vel.y * normal.y + vel.z * normal.z;
        vel = {(vel.x - 2.f * into * normal.x) * kElasticity, (vel.y - 2.f * into * normal.y) * kElasticity,
               (vel.z - 2.f * into * normal.z) * kElasticity};
        if (normal.z > kFloorNormalZ && vel.x * vel.x + vel.y * vel.y + vel.z * vel.z < kStopSpeed * kStopSpeed) break;
    }
    return path;
}

Vec3 ViewForward(const render::Camera& camera) {
    if (camera.use_matrix) {
        const float* m = camera.matrix;
        float length = std::sqrt(m[12] * m[12] + m[13] * m[13] + m[14] * m[14]);
        if (length > 1e-3f) return {m[12] / length, m[13] / length, m[14] / length};
    }
    return camera.forward;
}

std::vector<PathPoint> PredictOwnThrow(uintptr_t pawn, const Vec3& view_forward, GrenadeKind kind) {
    float strength = 1.f;
    if (off::m_flThrowStrength) {
        uintptr_t weapon = game::ActiveWeapon(pawn);
        float value = weapon ? g_proc.Read<float>(weapon + off::m_flThrowStrength) : 1.f;
        if (std::isfinite(value)) strength = std::clamp(value, 0.f, 1.f);
    }
    float pitch = -std::asin(std::clamp(view_forward.z, -1.f, 1.f)) / kDegToRad;
    float yaw = std::atan2(view_forward.y, view_forward.x);
    pitch -= (90.f - std::fabs(pitch)) * 10.f / 90.f;
    float cp = std::cos(pitch * kDegToRad), sp = std::sin(pitch * kDegToRad);
    Vec3 dir{cp * std::cos(yaw), cp * std::sin(yaw), -sp};

    Vec3 eye = game::EyePosition(pawn);
    Vec3 src{eye.x, eye.y, eye.z + strength * 12.f - 12.f};
    Vec3 reach{src.x + dir.x * 22.f, src.y + dir.y * 22.f, src.z + dir.z * 22.f};
    Vec3 hit;
    if (vis::Raycast(src, reach, hit, nullptr, vis::Blocks::Grenades)) reach = hit;
    Vec3 pos{reach.x - dir.x * 6.f, reach.y - dir.y * 6.f, reach.z - dir.z * 6.f};

    float speed = kMaxThrowSpeed * (strength * 0.7f + 0.3f);
    Vec3 player_velocity = off::m_vecVelocity ? g_proc.Read<Vec3>(pawn + off::m_vecVelocity) : Vec3{};
    Vec3 vel{dir.x * speed + player_velocity.x * 1.25f, dir.y * speed + player_velocity.y * 1.25f,
             dir.z * speed + player_velocity.z * 1.25f};
    return Fly(pos, vel, kind, FuseFor(kind));
}

void DrawGroundRing(cairo_t* cr, const render::Camera& camera, const Vec3& center, float radius, const Style& style) {
    if (radius <= 0.f) return;
    constexpr int kSegments = 40;
    bool started = false;
    cairo_new_path(cr);
    for (int i = 0; i <= kSegments; i++) {
        float angle = i * 2.f * kPi / kSegments;
        float sx, sy;
        if (!camera.Project({center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius, center.z}, sx, sy)) {
            started = false;
            continue;
        }
        if (started) cairo_line_to(cr, sx, sy);
        else cairo_move_to(cr, sx, sy);
        started = true;
    }
    cairo_set_source_rgba(cr, style.r, style.g, style.b, style.a * 0.35);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
}

void DrawPath(cairo_t* cr, const render::Camera& camera, const std::vector<PathPoint>& path, GrenadeKind kind,
              const Style& style) {
    std::vector<std::pair<float, float>> screen;
    std::vector<std::pair<float, float>> bounces;
    screen.reserve(path.size());
    for (const PathPoint& point : path) {
        float sx, sy;
        if (!camera.Project(point.pos, sx, sy)) continue;
        screen.push_back({sx, sy});
        if (point.bounce) bounces.push_back({sx, sy});
    }
    if (screen.size() < 2) return;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) { cairo_set_source_rgba(cr, 0, 0, 0, 0.55); cairo_set_line_width(cr, 4.0); }
        else { cairo_set_source_rgba(cr, style.r, style.g, style.b, 0.95); cairo_set_line_width(cr, 2.0); }
        cairo_move_to(cr, screen[0].first, screen[0].second);
        for (size_t i = 1; i < screen.size(); i++) cairo_line_to(cr, screen[i].first, screen[i].second);
        cairo_stroke(cr);
    }
    for (const auto& bounce : bounces) {
        cairo_arc(cr, bounce.first, bounce.second, 3.0, 0, 2 * kPi);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
        cairo_fill(cr);
    }
    DrawGroundRing(cr, camera, path.back().pos, EffectRadius(kind), style);
    const auto& last = screen.back();
    cairo_arc(cr, last.first, last.second, 5.0, 0, 2 * kPi);
    cairo_set_source_rgba(cr, style.r, style.g, style.b, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    const char* label = LabelFor(kind);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 10.0);
    cairo_text_extents_t extents;
    cairo_text_extents(cr, label, &extents);
    cairo_move_to(cr, last.first - extents.width * 0.5, last.second - 9.0);
    cairo_text_path(cr, label);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
    cairo_set_line_width(cr, 3.0);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgba(cr, style.r, style.g, style.b, 1.0);
    cairo_fill(cr);
}

double NowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void DrawThrown(cairo_t* cr, const render::Camera& camera, int local_team) {
    std::vector<ThrownGrenade> grenades;
    {
        std::lock_guard<std::mutex> lock(g_hud.grenades_mtx);
        grenades = g_hud.thrown_grenades;
    }
    struct Sample { Vec3 pos; double time; Vec3 velocity; };
    static std::unordered_map<uintptr_t, Sample> samples;
    double now = NowSeconds();
    std::erase_if(samples, [&](const auto& entry) {
        return std::none_of(grenades.begin(), grenades.end(), [&](const ThrownGrenade& g) { return g.entity == entry.first; });
    });
    for (const ThrownGrenade& grenade : grenades) {
        if (grenade.kind == GrenadeKind::Smoke && off::m_bDidSmokeEffect &&
            g_proc.Read<uint8_t>(grenade.entity + off::m_bDidSmokeEffect))
            continue;
        Vec3 pos = game::Origin(grenade.entity);
        Vec3 vel = off::m_vecVelocity ? g_proc.Read<Vec3>(grenade.entity + off::m_vecVelocity) : Vec3{};
        if (!std::isfinite(pos.x) || !std::isfinite(vel.x)) continue;
        auto [sample, inserted] = samples.try_emplace(grenade.entity, Sample{pos, now, {}});
        double dt = now - sample->second.time;
        if (!inserted && dt > 0.005) {
            Vec3 moved{static_cast<float>((pos.x - sample->second.pos.x) / dt), static_cast<float>((pos.y - sample->second.pos.y) / dt),
                       static_cast<float>((pos.z - sample->second.pos.z) / dt)};
            sample->second = {pos, now, moved};
        }
        if (std::hypot(vel.x, vel.y, vel.z) < kMovingSpeed) vel = sample->second.velocity;
        if (std::hypot(vel.x, vel.y, vel.z) < kMovingSpeed) continue;
        float time_left = FuseFor(grenade.kind) - static_cast<float>(now - grenade.seen_at);
        if (time_left <= 0.f) continue;
        const Style& style = local_team && grenade.team == local_team ? kFriendlyStyle : kEnemyStyle;
        DrawPath(cr, camera, Fly(pos, vel, grenade.kind, time_left), grenade.kind, style);
    }
}

static void DrawSpotCircle3D(cairo_t* cr, const render::Camera& camera, const Vec3& center, float radius, const Style& style) {
    constexpr int kSegments = 32;
    std::vector<std::pair<float, float>> pts;
    pts.reserve(kSegments + 1);
    for (int i = 0; i <= kSegments; i++) {
        float angle = i * 2.f * kPi / kSegments;
        float sx, sy;
        if (camera.Project({center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius, center.z}, sx, sy)) {
            pts.push_back({sx, sy});
        }
    }
    if (pts.size() < 3) return;
    cairo_new_path(cr);
    cairo_move_to(cr, pts[0].first, pts[0].second);
    for (size_t i = 1; i < pts.size(); i++) cairo_line_to(cr, pts[i].first, pts[i].second);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, style.r, style.g, style.b, style.a * 0.22);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, style.r, style.g, style.b, style.a * 0.95);
    cairo_set_line_width(cr, 2.0);
    cairo_stroke(cr);
}

static void DrawSpotText(cairo_t* cr, float x, float y, const std::string& text, const Style& style) {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11.0);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text.c_str(), &ext);
    float pad = 4.0f;
    cairo_rectangle(cr, x - pad, y - ext.height - pad, ext.width + pad * 2.0f, ext.height + pad * 2.0f);
    cairo_set_source_rgba(cr, 0.06, 0.07, 0.10, 0.85);
    cairo_fill(cr);
    cairo_move_to(cr, x, y);
    cairo_text_path(cr, text.c_str());
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.9);
    cairo_set_line_width(cr, 2.5);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgba(cr, style.r, style.g, style.b, 1.0);
    cairo_fill(cr);
}

static void DrawOffscreenPointer(cairo_t* cr, float cx, float cy, float edge_r, float angle, const Style& style, const std::string& label) {
    float px = cx + std::cos(angle) * edge_r;
    float py = cy + std::sin(angle) * edge_r;
    float size = 11.0f;
    float tip_x = px + std::cos(angle) * size;
    float tip_y = py + std::sin(angle) * size;
    float left_x = px + std::cos(angle + 2.35f) * size;
    float left_y = py + std::sin(angle + 2.35f) * size;
    float right_x = px + std::cos(angle - 2.35f) * size;
    float right_y = py + std::sin(angle - 2.35f) * size;

    cairo_new_path(cr);
    cairo_move_to(cr, tip_x, tip_y);
    cairo_line_to(cr, left_x, left_y);
    cairo_line_to(cr, right_x, right_y);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, style.r, style.g, style.b, style.a * 0.9);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.85);
    cairo_set_line_width(cr, 2.0);
    cairo_stroke(cr);

    if (!label.empty()) {
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 10.0);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, label.c_str(), &ext);
        float lx = px - ext.width * 0.5f;
        float ly = py + (std::sin(angle) > 0 ? 18.0f : -12.0f);
        cairo_rectangle(cr, lx - 3.0f, ly - ext.height - 2.0f, ext.width + 6.0f, ext.height + 4.0f);
        cairo_set_source_rgba(cr, 0.06, 0.07, 0.10, 0.85);
        cairo_fill(cr);
        cairo_move_to(cr, lx, ly);
        cairo_text_path(cr, label.c_str());
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.9);
        cairo_set_line_width(cr, 2.0);
        cairo_stroke_preserve(cr);
        cairo_set_source_rgba(cr, style.r, style.g, style.b, 1.0);
        cairo_fill(cr);
    }
}

static void DrawAimTargetMarker(cairo_t* cr, float sx, float sy, const Style& style, bool draw_line, float cx, float cy,
                               const std::string& name, const std::string& throw_type, bool is_locked) {
    if (draw_line) {
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.6);
        cairo_set_line_width(cr, 3.0);
        cairo_move_to(cr, cx, cy);
        cairo_line_to(cr, sx, sy);
        cairo_stroke(cr);

        cairo_set_source_rgba(cr, style.r, style.g, style.b, 0.8);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, cx, cy);
        cairo_line_to(cr, sx, sy);
        cairo_stroke(cr);
    }

    float r_ring = is_locked ? 12.0f : 9.0f;

    cairo_arc(cr, sx, sy, r_ring, 0, 2 * kPi);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.85);
    cairo_set_line_width(cr, 4.0);
    cairo_stroke(cr);

    cairo_arc(cr, sx, sy, r_ring, 0, 2 * kPi);
    cairo_set_source_rgba(cr, style.r, style.g, style.b, style.a);
    cairo_set_line_width(cr, 2.0);
    cairo_stroke_preserve(cr);
    if (is_locked) {
        cairo_set_source_rgba(cr, style.r, style.g, style.b, 0.35);
        cairo_fill(cr);
    }

    cairo_arc(cr, sx, sy, 2.5, 0, 2 * kPi);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
    cairo_fill(cr);

    float tick_inner = r_ring + 2.0f;
    float tick_outer = r_ring + 7.0f;

    cairo_move_to(cr, sx - tick_outer, sy); cairo_line_to(cr, sx - tick_inner, sy);
    cairo_move_to(cr, sx + tick_inner, sy); cairo_line_to(cr, sx + tick_outer, sy);
    cairo_move_to(cr, sx, sy - tick_outer); cairo_line_to(cr, sx, sy - tick_inner);
    cairo_move_to(cr, sx, sy + tick_inner); cairo_line_to(cr, sx, sy + tick_outer);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.85);
    cairo_set_line_width(cr, 3.5);
    cairo_stroke(cr);

    cairo_move_to(cr, sx - tick_outer, sy); cairo_line_to(cr, sx - tick_inner, sy);
    cairo_move_to(cr, sx + tick_inner, sy); cairo_line_to(cr, sx + tick_outer, sy);
    cairo_move_to(cr, sx, sy - tick_outer); cairo_line_to(cr, sx, sy - tick_inner);
    cairo_move_to(cr, sx, sy + tick_inner); cairo_line_to(cr, sx, sy + tick_outer);
    cairo_set_source_rgba(cr, style.r, style.g, style.b, style.a);
    cairo_set_line_width(cr, 2.0);
    cairo_stroke(cr);

    std::string tag = name + " [" + throw_type + "]";
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11.0);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, tag.c_str(), &ext);
    float tx = sx - ext.width * 0.5f;
    float ty = sy + r_ring + 18.0f;

    cairo_rectangle(cr, tx - 4.0f, ty - ext.height - 2.0f, ext.width + 8.0f, ext.height + 5.0f);
    cairo_set_source_rgba(cr, 0.06, 0.07, 0.10, 0.88);
    cairo_fill(cr);

    cairo_move_to(cr, tx, ty);
    cairo_text_path(cr, tag.c_str());
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.9);
    cairo_set_line_width(cr, 2.5);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgba(cr, is_locked ? 0.35 : style.r, is_locked ? 1.0 : style.g, is_locked ? 0.45 : style.b, 1.0);
    cairo_fill(cr);
}

static void ApplyAimAssist(const LineupSpot& lineup, uintptr_t pawn, const Settings& cfg) {
    if (!off::m_angEyeAngles) return;
    Vec3 cur_ang = g_proc.Read<Vec3>(pawn + off::m_angEyeAngles);
    float d_pitch = lineup.pitch - cur_ang.x;
    float d_yaw = NormAngle(lineup.yaw - cur_ang.y);
    float dist = std::sqrt(d_pitch * d_pitch + d_yaw * d_yaw);
    if (dist < 0.01f) return;
    float sens = (cfg.aimbot_sens_x1000 ? cfg.aimbot_sens_x1000 : 100) / 1000.f;
    float pixels_per_deg = 1.f / (0.022f * (sens > 0.01f ? sens : 1.0f));
    float dx = -d_yaw * pixels_per_deg;
    float dy = d_pitch * pixels_per_deg;
    int move_x = std::clamp(static_cast<int>(dx * 0.45f), -80, 80);
    int move_y = std::clamp(static_cast<int>(dy * 0.45f), -60, 60);
    if (std::abs(dx) < 2.0f && std::abs(dy) < 2.0f) {
        move_x = static_cast<int>(dx);
        move_y = static_cast<int>(dy);
    }
    if (move_x || move_y) g_input.MouseMove(move_x, move_y);
}

static void DrawLineups(cairo_t* cr, const render::Camera& camera, const Settings& settings, uintptr_t pawn) {
    if (!settings::Enabled(settings.grenade_helper) || !pawn) return;
    std::string map = vis::CurrentMap();
    if (map.empty()) return;

    GrenadeKind held_kind = GrenadeKind::Smoke;
    bool has_grenade = KindForWeapon(game::ActiveWeaponDefinitionIndex(pawn), held_kind);
    if (settings::Enabled(settings.grenade_helper_only_held) && !has_grenade) return;

    std::vector<LineupSpot> lineups = GetLineupsForMap(map);
    if (lineups.empty()) return;

    Vec3 my_pos = game::Origin(pawn);
    Vec3 eye_pos = game::EyePosition(pawn);
    Vec3 cur_ang = off::m_angEyeAngles ? g_proc.Read<Vec3>(pawn + off::m_angEyeAngles) : camera.angles;
    float center_x = camera.width * 0.5f;
    float center_y = camera.height * 0.5f;

    Style style_spot = ColorFromRgba(settings.grenade_helper_spot_color_rgba ? settings.grenade_helper_spot_color_rgba : 0x4C8DFFC8);
    Style style_active = ColorFromRgba(settings.grenade_helper_active_color_rgba ? settings.grenade_helper_active_color_rgba : 0x32DC64FF);
    Style style_aim = ColorFromRgba(settings.grenade_helper_aim_color_rgba ? settings.grenade_helper_aim_color_rgba : 0xFFB400DC);

    bool aim_assist_key = IsBindActive(settings.bind_grenade_helper_aim);
    if (settings.bind_grenade_helper_aim == 0) aim_assist_key = g_input.IsMouseDown(2) || g_input.IsMouseDown(5);

    for (const LineupSpot& lineup : lineups) {
        if (settings::Enabled(settings.grenade_helper_only_held) && has_grenade && lineup.kind != held_kind)
            continue;

        float dx = lineup.pos.x - my_pos.x;
        float dy = lineup.pos.y - my_pos.y;
        float dz = lineup.pos.z - my_pos.z;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > 900.f) continue;

        bool is_aligned = (dist < 35.0f);
        const Style& cur_spot_style = is_aligned ? style_active : style_spot;

        DrawSpotCircle3D(cr, camera, lineup.pos, 18.0f, cur_spot_style);

        float screen_spot_x = 0.f, screen_spot_y = 0.f;
        if (camera.Project(lineup.pos, screen_spot_x, screen_spot_y)) {
            std::string label = lineup.name + " [" + ThrowTypeStr(lineup.throw_type) + "]";
            DrawSpotText(cr, screen_spot_x + 14.f, screen_spot_y - 14.f, label, cur_spot_style);
        }

        float d_pitch = lineup.pitch - cur_ang.x;
        float d_yaw = NormAngle(lineup.yaw - cur_ang.y);

        Vec3 fwd = AngleToForward(lineup.pitch, lineup.yaw);
        Vec3 target_world{eye_pos.x + fwd.x * 2000.f, eye_pos.y + fwd.y * 2000.f, eye_pos.z + fwd.z * 2000.f};

        float aim_sx = 0.f, aim_sy = 0.f;
        bool on_screen = camera.Project(target_world, aim_sx, aim_sy) &&
                         aim_sx >= 0.f && aim_sx <= camera.width && aim_sy >= 0.f && aim_sy <= camera.height;

        if (on_screen) {
            float aim_delta = std::hypot(aim_sx - center_x, aim_sy - center_y);
            bool is_locked = is_aligned && (aim_delta < 12.0f);
            const Style& cur_aim_style = is_locked ? style_active : (is_aligned ? style_active : style_aim);

            DrawAimTargetMarker(cr, aim_sx, aim_sy, cur_aim_style, settings::Enabled(settings.grenade_helper_draw_line),
                               center_x, center_y, lineup.name, ThrowTypeStr(lineup.throw_type), is_locked);

            if (is_locked) {
                std::string ready_txt = "READY: " + std::string(ThrowTypeStr(lineup.throw_type));
                cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
                cairo_set_font_size(cr, 12.0);
                cairo_text_extents_t ext;
                cairo_text_extents(cr, ready_txt.c_str(), &ext);
                float rx = center_x - ext.width * 0.5f;
                float ry = center_y + 32.0f;
                cairo_rectangle(cr, rx - 6.0f, ry - ext.height - 3.0f, ext.width + 12.0f, ext.height + 6.0f);
                cairo_set_source_rgba(cr, 0.05, 0.06, 0.09, 0.92);
                cairo_fill(cr);
                cairo_move_to(cr, rx, ry);
                cairo_text_path(cr, ready_txt.c_str());
                cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.9);
                cairo_set_line_width(cr, 2.5);
                cairo_stroke_preserve(cr);
                cairo_set_source_rgba(cr, 0.25, 0.95, 0.45, 1.0);
                cairo_fill(cr);
            }

            if (is_aligned && settings::Enabled(settings.grenade_helper_aim) && aim_assist_key) {
                ApplyAimAssist(lineup, pawn, settings);
            }
        } else if (is_aligned) {
            float angle = std::atan2(-d_pitch, d_yaw);
            float edge_r = std::min(camera.width, camera.height) * 0.40f;
            std::string deg_label = std::to_string(static_cast<int>(std::hypot(d_pitch, d_yaw))) + "°";
            DrawOffscreenPointer(cr, center_x, center_y, edge_r, angle, style_active, deg_label);
        }
    }
}

static void HandleTokenAndHotkeys(const Settings& settings) {
    static int32_t last_token = 0;
    if (settings.grenade_helper_save_token != last_token) {
        last_token = settings.grenade_helper_save_token;
        AddCurrentSpot();
    }
    static bool prev_save = false;
    bool now_save = IsBindActive(settings.bind_grenade_helper_save);
    if (now_save && !prev_save) AddCurrentSpot();
    prev_save = now_save;

    static bool prev_remove = false;
    bool now_remove = IsBindActive(settings.bind_grenade_helper_remove);
    if (now_remove && !prev_remove) RemoveNearestSpot();
    prev_remove = now_remove;
}

void Draw(cairo_t* cr, const render::Camera& camera, const Settings& settings) {
    HandleTokenAndHotkeys(settings);
    if (!camera.valid || !vis::Ready()) return;
    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    uintptr_t pawn = game::LocalPawn();
    if (settings::Enabled(settings.grenade_world)) DrawThrown(cr, camera, pawn ? game::Team(pawn) : 0);
    GrenadeKind kind;
    if (settings::Enabled(settings.grenade_trajectory) && pawn && KindForWeapon(game::ActiveWeaponDefinitionIndex(pawn), kind))
        DrawPath(cr, camera, PredictOwnThrow(pawn, ViewForward(camera), kind), kind, kOwnStyle);
    DrawLineups(cr, camera, settings, pawn);
    cairo_restore(cr);
}

}
