#pragma once
#include "state.h"
#include "render/camera.h"
#include "render/model_chams.h"
#include <cairo.h>
#include <vector>

struct Settings;

namespace world {

struct LivePlayer {
    EspEntry info;
    Vec3 origin{};
    std::vector<chams::BoneTransform> bones;
    const chams::SkinnedModel* model = nullptr;

    bool Bone(int index, Vec3& out) const;
    bool Enemy(int local_team) const { return !local_team || info.team != local_team; }
};

std::vector<LivePlayer> CapturePlayers(const Settings& settings);

void DrawChams(cairo_t* cr, const render::Camera& camera, const std::vector<LivePlayer>& players, const Settings& settings);
void DrawEsp(cairo_t* cr, const render::Camera& camera, const std::vector<LivePlayer>& players, const Settings& settings);
void DrawArrows(cairo_t* cr, const render::Camera& camera, const std::vector<LivePlayer>& players, const Settings& settings);

}
