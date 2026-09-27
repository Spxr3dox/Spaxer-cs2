#pragma once
#include "render/camera.h"
#include <cairo.h>

struct Settings;

namespace nightsky {

void Draw(cairo_t* cr, const render::Camera& camera, const Settings& settings);
void Shutdown();

}
