#pragma once
#include "sdk/game.h"

namespace render {

struct Camera {
    Vec3 eye{}, angles{};
    Vec3 forward{}, right{}, up{};
    float tan_half_h = 1.f, tan_half_v = 0.75f;
    float matrix[16]{};
    int width = 0, height = 0;
    bool use_matrix = false;
    bool valid = false;

    float Depth(const Vec3& p) const;
    bool Project(const Vec3& p, float& sx, float& sy) const;
    bool Project(float x, float y, float z, float& sx, float& sy) const { return Project(Vec3{x, y, z}, sx, sy); }
    float PixelsPerUnit(const Vec3& p) const;
    bool ProjectAngles(const Vec3& p, float& sx, float& sy) const;
    bool ProjectMatrix(const Vec3& p, float& sx, float& sy) const;
};

Camera ReadCamera(int width, int height, float lead_seconds);

}
