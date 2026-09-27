#pragma once
#include "render/camera.h"
#include <cairo.h>
#include <cstdint>

struct Settings;

namespace weather {

enum class Mode : uint32_t { Off = 0, Rain = 1, Snow = 2, Ash = 3, Embers = 4 };

void Draw(cairo_t* cr, const render::Camera& camera, const Settings& settings);

}
