#include "render/esp_icons.h"
#include "state.h"
#include <chrono>
#include <cmath>

namespace icons {

namespace {

constexpr double kPi = 3.14159265358979;

struct Tint { double r, g, b; };

enum class Glyph { Helmet, Shield, Kit, Defusing, Bomb, Flash, Scope, Reload };

void Badge(cairo_t* cr, double x, double y, double size, Tint tint, double alpha) {
    double radius = size * 0.28;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + size - radius, y + radius, radius, -kPi / 2, 0);
    cairo_arc(cr, x + size - radius, y + size - radius, radius, 0, kPi / 2);
    cairo_arc(cr, x + radius, y + size - radius, radius, kPi / 2, kPi);
    cairo_arc(cr, x + radius, y + radius, radius, kPi, 3 * kPi / 2);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.07, 0.07, 0.09, 0.82 * alpha);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, tint.r, tint.g, tint.b, 0.85 * alpha);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
}

double Seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void DrawGlyph(cairo_t* cr, Glyph glyph, Tint tint, double alpha) {
    cairo_set_source_rgba(cr, tint.r, tint.g, tint.b, alpha);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, 0.11);
    switch (glyph) {
        case Glyph::Helmet:
            cairo_new_path(cr);
            cairo_arc(cr, 0.5, 0.62, 0.3, kPi, 2 * kPi);
            cairo_line_to(cr, 0.86, 0.72);
            cairo_line_to(cr, 0.14, 0.72);
            cairo_close_path(cr);
            cairo_fill(cr);
            break;
        case Glyph::Shield:
            cairo_new_path(cr);
            cairo_move_to(cr, 0.5, 0.14);
            cairo_line_to(cr, 0.82, 0.26);
            cairo_curve_to(cr, 0.82, 0.58, 0.7, 0.76, 0.5, 0.88);
            cairo_curve_to(cr, 0.3, 0.76, 0.18, 0.58, 0.18, 0.26);
            cairo_close_path(cr);
            cairo_fill(cr);
            break;
        case Glyph::Kit:
        case Glyph::Defusing:
            cairo_new_path(cr);
            cairo_move_to(cr, 0.28, 0.2);
            cairo_line_to(cr, 0.62, 0.62);
            cairo_move_to(cr, 0.72, 0.2);
            cairo_line_to(cr, 0.38, 0.62);
            cairo_stroke(cr);
            cairo_arc(cr, 0.3, 0.74, 0.12, 0, 2 * kPi);
            cairo_stroke(cr);
            cairo_arc(cr, 0.7, 0.74, 0.12, 0, 2 * kPi);
            cairo_stroke(cr);
            if (glyph == Glyph::Defusing) {
                double start = std::fmod(Seconds() * 4.0, 2 * kPi);
                cairo_new_path(cr);
                cairo_arc(cr, 0.5, 0.5, 0.46, start, start + kPi * 1.2);
                cairo_set_line_width(cr, 0.08);
                cairo_stroke(cr);
            }
            break;
        case Glyph::Bomb:
            cairo_rectangle(cr, 0.16, 0.34, 0.68, 0.4);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 0.07, 0.07, 0.09, alpha);
            cairo_rectangle(cr, 0.28, 0.44, 0.3, 0.2);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 0.35, 0.6, 1.0, (0.5 + 0.5 * std::sin(Seconds() * 8.0)) * alpha);
            cairo_arc(cr, 0.7, 0.54, 0.06, 0, 2 * kPi);
            cairo_fill(cr);
            break;
        case Glyph::Flash:
            cairo_arc(cr, 0.5, 0.5, 0.17, 0, 2 * kPi);
            cairo_fill(cr);
            for (int i = 0; i < 8; i++) {
                double angle = i * kPi / 4;
                cairo_move_to(cr, 0.5 + std::cos(angle) * 0.27, 0.5 + std::sin(angle) * 0.27);
                cairo_line_to(cr, 0.5 + std::cos(angle) * 0.4, 0.5 + std::sin(angle) * 0.4);
            }
            cairo_stroke(cr);
            break;
        case Glyph::Scope:
            cairo_arc(cr, 0.5, 0.5, 0.3, 0, 2 * kPi);
            cairo_stroke(cr);
            cairo_move_to(cr, 0.5, 0.1); cairo_line_to(cr, 0.5, 0.36);
            cairo_move_to(cr, 0.5, 0.64); cairo_line_to(cr, 0.5, 0.9);
            cairo_move_to(cr, 0.1, 0.5); cairo_line_to(cr, 0.36, 0.5);
            cairo_move_to(cr, 0.64, 0.5); cairo_line_to(cr, 0.9, 0.5);
            cairo_stroke(cr);
            break;
        case Glyph::Reload: {
            double start = std::fmod(Seconds() * 5.0, 2 * kPi);
            double end = start + kPi * 1.5;
            cairo_new_path(cr);
            cairo_arc(cr, 0.5, 0.5, 0.28, start, end);
            cairo_stroke(cr);
            double tip_x = 0.5 + std::cos(end) * 0.28, tip_y = 0.5 + std::sin(end) * 0.28;
            double along_x = -std::sin(end), along_y = std::cos(end);
            double out_x = std::cos(end), out_y = std::sin(end);
            cairo_move_to(cr, tip_x + along_x * 0.14, tip_y + along_y * 0.14);
            cairo_line_to(cr, tip_x + out_x * 0.12, tip_y + out_y * 0.12);
            cairo_line_to(cr, tip_x - out_x * 0.12, tip_y - out_y * 0.12);
            cairo_close_path(cr);
            cairo_fill(cr);
            break;
        }
    }
}

void Icon(cairo_t* cr, Glyph glyph, Tint tint, double x, double y, double size, double alpha) {
    Badge(cr, x, y, size, tint, alpha);
    cairo_save(cr);
    double inset = size * 0.16;
    cairo_translate(cr, x + inset, y + inset);
    cairo_scale(cr, size - inset * 2, size - inset * 2);
    DrawGlyph(cr, glyph, tint, alpha);
    cairo_restore(cr);
}

}

double DrawFlagColumn(cairo_t* cr, uint32_t flags, double left, double top, double size, double alpha) {
    struct Entry { uint32_t bit; Glyph glyph; Tint tint; };
    static constexpr Entry kEntries[] = {
        {kFlagHelmet, Glyph::Helmet, {0.7, 0.8, 1.0}},  {kFlagBomb, Glyph::Bomb, {0.3, 0.55, 1.0}},
        {kFlagDefusing, Glyph::Defusing, {0.45, 0.75, 1.0}}, {kFlagKit, Glyph::Kit, {0.45, 0.75, 1.0}},
        {kFlagFlashed, Glyph::Flash, {0.78, 0.88, 1.0}},   {kFlagScoped, Glyph::Scope, {0.86, 0.9, 1.0}},
        {kFlagReloading, Glyph::Reload, {0.5, 0.65, 1.0}},
    };
    double y = top;
    double gap = size * 0.18;
    cairo_save(cr);
    if ((flags & kFlagArmor) && !(flags & kFlagHelmet)) {
        Icon(cr, Glyph::Shield, {0.7, 0.8, 1.0}, left, y, size, alpha);
        y += size + gap;
    }
    for (const Entry& entry : kEntries) {
        if (!(flags & entry.bit)) continue;
        Icon(cr, entry.glyph, entry.tint, left, y, size, alpha);
        y += size + gap;
    }
    cairo_restore(cr);
    return y - top;
}

}
