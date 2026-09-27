#pragma once
#include <cairo.h>
#include "render/camera.h"

struct Settings;

namespace lua_engine {
    void Init(Settings* cfg);
    void Shutdown();
    void Reload();
    void DispatchPaint(cairo_t* cr, const render::Camera& camera);
    void DispatchTick();
    void DispatchFrame();
}
