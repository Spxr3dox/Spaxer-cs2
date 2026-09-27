#include "overlay/grenade.h"
#include "config/settings.h"
#include "sdk/visibility.h"
#include "state.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <vector>

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

struct Style { double r, g, b; };

constexpr Style kOwnStyle{1.0, 0.62, 0.2};
constexpr Style kEnemyStyle{1.0, 0.3, 0.28};
constexpr Style kFriendlyStyle{0.35, 0.8, 1.0};

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

float FuseFor(GrenadeKind kind) {
    switch (kind) {
        case GrenadeKind::He: case GrenadeKind::Flash: return kTimedFuse;
        case GrenadeKind::Fire: return kFireFuse;
        default: return kMaxSimulation;
    }
}

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
    cairo_set_source_rgba(cr, style.r, style.g, style.b, 0.35);
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

}

void Draw(cairo_t* cr, const render::Camera& camera, const Settings& settings) {
    if (!camera.valid || !vis::Ready()) return;
    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    uintptr_t pawn = game::LocalPawn();
    if (settings::Enabled(settings.grenade_world)) DrawThrown(cr, camera, pawn ? game::Team(pawn) : 0);
    GrenadeKind kind;
    if (settings::Enabled(settings.grenade_trajectory) && pawn && KindForWeapon(game::ActiveWeaponDefinitionIndex(pawn), kind))
        DrawPath(cr, camera, PredictOwnThrow(pawn, ViewForward(camera), kind), kind, kOwnStyle);
    cairo_restore(cr);
}

}
