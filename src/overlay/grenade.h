#pragma once
#include "render/camera.h"
#include "state.h"
#include <cairo.h>
#include <vector>
#include <string>
#include <cstdint>

struct Settings;

namespace grenade {

enum class ThrowType : int {
    Standing = 0,
    Jumpthrow = 1,
    Runthrow = 2,
    Crouch = 3
};

struct LineupSpot {
    std::string name;
    std::string map;
    GrenadeKind kind;
    ThrowType throw_type;
    Vec3 pos;
    float pitch;
    float yaw;
    bool custom;
};

struct SimPoint { Vec3 pos; bool bounce; };

struct ThrowSetup { Vec3 origin; Vec3 velocity; float fuse; GrenadeKind kind; };

float FuseFor(GrenadeKind kind);

std::vector<SimPoint> Simulate(Vec3 pos, Vec3 vel, GrenadeKind kind, float time_left);

bool BuildOwnThrow(uintptr_t pawn, const Vec3& view_forward, GrenadeKind kind, ThrowSetup& out);

void LoadCustomLineups();

void SaveCustomLineups();

bool AddCurrentSpot(const std::string& custom_name = "", ThrowType throw_type = ThrowType::Standing);

bool RemoveNearestSpot();

bool ClearAllCustomSpots();

const std::vector<LineupSpot>& GetAllLineups();

std::vector<LineupSpot> GetLineupsForMap(const std::string& map, bool include_builtin = true);

void Draw(cairo_t* cr, const render::Camera& camera, const Settings& settings);

}
