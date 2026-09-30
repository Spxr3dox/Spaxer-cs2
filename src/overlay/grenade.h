#pragma once
#include "render/camera.h"
#include "state.h"
#include <cairo.h>
#include <vector>
#include <cstdint>

struct Settings;

namespace grenade {

void Draw(cairo_t* cr, const render::Camera& camera, const Settings& settings);

// Public simulation API (used by Lua scripts).
struct SimPoint { Vec3 pos; bool bounce; };

// Fuse for each grenade kind (seconds).
float FuseFor(GrenadeKind kind);

// Simulate free-flight of a projectile with grenade physics.
// Starts at pos with velocity vel, respects gravity, bounces (elasticity 0.45),
// stops at fuse time or when velocity is under 20 u/s on a floor-like surface,
// stops early for fire grenades on floors. Returns full path (first point = origin).
std::vector<SimPoint> Simulate(Vec3 pos, Vec3 vel, GrenadeKind kind, float time_left);

// Build a throw from the local player's current view + active grenade weapon.
// Reads m_flThrowStrength from the weapon (0..1, 1 = full press).
// Returns origin, velocity and fuse ready to feed into Simulate().
struct ThrowSetup { Vec3 origin; Vec3 velocity; float fuse; GrenadeKind kind; };
bool BuildOwnThrow(uintptr_t pawn, const Vec3& view_forward, GrenadeKind kind, ThrowSetup& out);

}
