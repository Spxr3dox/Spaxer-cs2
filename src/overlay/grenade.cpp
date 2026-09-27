#include "overlay/grenade.h"
#include "config/settings.h"
#include "sdk/visibility.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace grenade {

namespace {

constexpr float kDegToRad = 3.14159265f / 180.f;
constexpr float kTickSeconds = 1.f / 64.f;
constexpr float kGravity = 320.f;
constexpr float kElasticity = 0.45f;
constexpr float kMaxThrowSpeed = 750.f * 0.9f;
constexpr float kStopSpeed = 20.f;
constexpr float kTimedFuse = 1.5f;
constexpr float kFireFuse = 2.f;
constexpr float kMaxSimulation = 12.f;
constexpr float kFloorNormalZ = 0.7f;
constexpr float kSurfaceOffset = 0.5f;

enum class Kind { None, Timed, Fire, Stop };

struct PathPoint { Vec3 pos; bool bounce; };

Kind KindFor(int definition) {
    switch (definition) {
        case 43: case 44: return Kind::Timed;
        case 46: case 48: return Kind::Fire;
        case 45: case 47: return Kind::Stop;
        default: return Kind::None;
    }
}

Vec3 ViewForward(const render::Camera& camera) {
    if (camera.use_matrix) {
        const float* m = camera.matrix;
        float length = std::sqrt(m[12] * m[12] + m[13] * m[13] + m[14] * m[14]);
        if (length > 1e-3f) return {m[12] / length, m[13] / length, m[14] / length};
    }
    return camera.forward;
}

Vec3 ThrowDirection(const Vec3& forward) {
    float pitch = -std::asin(std::clamp(forward.z, -1.f, 1.f)) / kDegToRad;
    float yaw = std::atan2(forward.y, forward.x);
    pitch -= (90.f - std::fabs(pitch)) * 10.f / 90.f;
    float cp = std::cos(pitch * kDegToRad), sp = std::sin(pitch * kDegToRad);
    return {cp * std::cos(yaw), cp * std::sin(yaw), -sp};
}

std::vector<PathPoint> Simulate(uintptr_t pawn, const Vec3& view_forward, Kind kind) {
    std::vector<PathPoint> path;
    float strength = 1.f;
    if (off::m_flThrowStrength) {
        uintptr_t weapon = game::ActiveWeapon(pawn);
        float value = weapon ? g_proc.Read<float>(weapon + off::m_flThrowStrength) : 1.f;
        if (std::isfinite(value)) strength = std::clamp(value, 0.f, 1.f);
    }
    Vec3 dir = ThrowDirection(view_forward);
    Vec3 eye = game::EyePosition(pawn);
    Vec3 src{eye.x, eye.y, eye.z + strength * 12.f - 12.f};
    Vec3 reach{src.x + dir.x * 22.f, src.y + dir.y * 22.f, src.z + dir.z * 22.f};
    Vec3 hit;
    if (vis::Raycast(src, reach, hit)) reach = hit;
    Vec3 pos{reach.x - dir.x * 6.f, reach.y - dir.y * 6.f, reach.z - dir.z * 6.f};

    float speed = kMaxThrowSpeed * (strength * 0.7f + 0.3f);
    Vec3 player_velocity = off::m_vecVelocity ? g_proc.Read<Vec3>(pawn + off::m_vecVelocity) : Vec3{};
    Vec3 vel{dir.x * speed + player_velocity.x * 1.25f, dir.y * speed + player_velocity.y * 1.25f,
             dir.z * speed + player_velocity.z * 1.25f};

    path.push_back({pos, false});
    float fuse = kind == Kind::Timed ? kTimedFuse : kind == Kind::Fire ? kFireFuse : kMaxSimulation;
    for (float t = 0.f; t < fuse; t += kTickSeconds) {
        Vec3 move{vel.x * kTickSeconds, vel.y * kTickSeconds, vel.z * kTickSeconds - 0.5f * kGravity * kTickSeconds * kTickSeconds};
        vel.z -= kGravity * kTickSeconds;
        Vec3 next{pos.x + move.x, pos.y + move.y, pos.z + move.z};
        Vec3 normal;
        if (!vis::Raycast(pos, next, hit, &normal)) {
            pos = next;
            path.push_back({pos, false});
            continue;
        }
        pos = {hit.x + normal.x * kSurfaceOffset, hit.y + normal.y * kSurfaceOffset, hit.z + normal.z * kSurfaceOffset};
        path.push_back({pos, true});
        bool floor = normal.z > kFloorNormalZ;
        if (kind == Kind::Fire && floor) break;
        float into = vel.x * normal.x + vel.y * normal.y + vel.z * normal.z;
        vel = {(vel.x - 2.f * into * normal.x) * kElasticity, (vel.y - 2.f * into * normal.y) * kElasticity,
               (vel.z - 2.f * into * normal.z) * kElasticity};
        if (floor && vel.x * vel.x + vel.y * vel.y + vel.z * vel.z < kStopSpeed * kStopSpeed) break;
    }
    return path;
}

}

void Draw(cairo_t* cr, const render::Camera& camera, const Settings& settings) {
    if (!settings::Enabled(settings.grenade_trajectory) || !camera.valid || !vis::Ready()) return;
    uintptr_t pawn = game::LocalPawn();
    if (!pawn) return;
    Kind kind = KindFor(game::ActiveWeaponDefinitionIndex(pawn));
    if (kind == Kind::None) return;

    std::vector<PathPoint> path = Simulate(pawn, ViewForward(camera), kind);
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

    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) { cairo_set_source_rgba(cr, 0, 0, 0, 0.55); cairo_set_line_width(cr, 4.0); }
        else { cairo_set_source_rgba(cr, 1.0, 0.62, 0.2, 0.95); cairo_set_line_width(cr, 2.0); }
        cairo_move_to(cr, screen[0].first, screen[0].second);
        for (size_t i = 1; i < screen.size(); i++) cairo_line_to(cr, screen[i].first, screen[i].second);
        cairo_stroke(cr);
    }
    for (const auto& bounce : bounces) {
        cairo_arc(cr, bounce.first, bounce.second, 3.5, 0, 6.2831853);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
        cairo_fill(cr);
    }
    const auto& last = screen.back();
    cairo_arc(cr, last.first, last.second, 6.0, 0, 6.2831853);
    cairo_set_source_rgba(cr, 1.0, 0.3, 0.2, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    cairo_restore(cr);
}

}
