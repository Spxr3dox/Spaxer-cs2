#pragma once
#include "sdk/game.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace vis {

enum class Blocks : uint8_t { Sight = 1, Grenades = 2 };

struct Ballistics {
    float damage;
    float penetration;
    float range_modifier;
};

void Update();
bool Ready();
bool SunDirection(Vec3& out);
std::string CurrentMap();
bool LineOfSight(const Vec3& from, const Vec3& to);
bool Raycast(const Vec3& from, const Vec3& to, Vec3& hit, Vec3* normal = nullptr, Blocks blocks = Blocks::Sight);
bool CastBatch(const Vec3& origin, const Vec3* directions, size_t count, float length, uint8_t* blocked);
bool WeaponBallistics(int definition_index, Ballistics& out);
float DamageAt(const Vec3& from, const Vec3& to, const Ballistics& weapon);

}
