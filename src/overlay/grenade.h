#pragma once
#include "render/camera.h"
#include <cairo.h>

struct Settings;

namespace grenade {

void Draw(cairo_t* cr, const render::Camera& camera, const Settings& settings);

}
