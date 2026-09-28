#pragma once
#include <cairo.h>
#include <cstdint>

namespace icons {

double DrawFlagColumn(cairo_t* cr, uint32_t flags, double left, double top, double size, double alpha);

}
