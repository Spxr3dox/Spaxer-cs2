#pragma once

typedef struct _cairo cairo_t;

namespace xhair {

void Start();
void Stop();
void Request();
void Draw(cairo_t* cr, int width, int height);

}
