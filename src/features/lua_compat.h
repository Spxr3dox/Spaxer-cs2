#pragma once
#include <cairo.h>
#include "render/camera.h"

struct Settings;
struct lua_State;

namespace lua_compat {
    void Register(lua_State* L, Settings* cfg);
    void BeginPaint(cairo_t* cr, const render::Camera& camera);
    void EndPaint();
    void DispatchGameEvents(lua_State* L);
}
