#include "overlay/world.h"
#include "config/settings.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace world {

static constexpr int kSkeletonBones = 23;
static constexpr float kPi = 3.14159265358979f;

struct Rgb { double r, g, b; };

static Rgb UnpackRgb(uint32_t rgba) {
    return {((rgba >> 24) & 0xFF) / 255.0, ((rgba >> 16) & 0xFF) / 255.0, ((rgba >> 8) & 0xFF) / 255.0};
}

static float Distance(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool LivePlayer::Bone(int index, Vec3& out) const {
    if (index < 0 || index >= static_cast<int>(bones.size())) return false;
    const chams::BoneTransform& bone = bones[index];
    out = {bone.x, bone.y, bone.z};
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z) && Distance(out, origin) < 110.f;
}

static void Advance(Vec3& point, const Vec3& shift) {
    point.x += shift.x; point.y += shift.y; point.z += shift.z;
}

std::vector<LivePlayer> CapturePlayers(const Settings& settings) {
    std::vector<EspEntry> entries;
    {
        std::lock_guard<std::mutex> lock(g_hud.esp_mtx);
        entries = g_hud.esp_players;
    }
    bool want_models = settings::Enabled(settings.chams);
    float lead_seconds = std::clamp(settings.render_lead_ms, 0, 200) / 1000.f;
    std::vector<LivePlayer> players;
    players.reserve(entries.size());
    for (const EspEntry& entry : entries) {
        LivePlayer player;
        player.info = entry;
        Vec3 cached{entry.world_feet_x, entry.world_feet_y, entry.world_feet_z};
        player.origin = cached;
        uintptr_t node = entry.pawn && off::m_pGameSceneNode ? g_proc.Read<uintptr_t>(entry.pawn + off::m_pGameSceneNode) : 0;
        if (node) {
            Vec3 live = g_proc.Read<Vec3>(node + off::m_vecAbsOrigin);
            if (std::isfinite(live.x) && std::isfinite(live.y) && std::isfinite(live.z) && Distance(live, cached) < 256.f)
                player.origin = live;
        }
        if (want_models && entry.model[0]) player.model = chams::FindModel(entry.model);
        int bone_count = player.model ? std::max(player.model->bone_count, kSkeletonBones) : kSkeletonBones;
        uintptr_t array = node && off::m_modelState ? g_proc.Read<uintptr_t>(node + off::m_modelState + 0x80) : 0;
        if (array) {
            player.bones.resize(bone_count);
            if (!g_proc.ReadBytes(array, player.bones.data(), player.bones.size() * sizeof(chams::BoneTransform)))
                player.bones.clear();
        }
        Vec3 pelvis;
        if (!player.Bone(1, pelvis)) {
            player.bones.clear();
            player.model = nullptr;
        }
        if (lead_seconds > 0.f && off::m_vecVelocity) {
            Vec3 velocity = g_proc.Read<Vec3>(entry.pawn + off::m_vecVelocity);
            if (std::isfinite(velocity.x) && std::isfinite(velocity.y) && std::isfinite(velocity.z) &&
                velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z < 1500.f * 1500.f) {
                Vec3 shift{velocity.x * lead_seconds, velocity.y * lead_seconds, velocity.z * lead_seconds};
                Advance(player.origin, shift);
                for (chams::BoneTransform& bone : player.bones) {
                    bone.x += shift.x; bone.y += shift.y; bone.z += shift.z;
                }
            }
        }
        players.push_back(std::move(player));
    }
    return players;
}

struct ChamsCapsule { int from, to; float radius; };

static constexpr ChamsCapsule kCapsules[] = {
    {6, 7, 3.6f}, {5, 6, 3.4f}, {1, 2, 6.8f}, {2, 3, 7.0f}, {3, 4, 7.2f}, {4, 5, 6.6f},
    {5, 9, 3.6f}, {9, 10, 3.2f}, {10, 11, 2.7f},
    {5, 13, 3.6f}, {13, 14, 3.2f}, {14, 15, 2.7f},
    {1, 17, 5.0f}, {17, 18, 4.4f}, {18, 19, 3.4f},
    {1, 20, 5.0f}, {20, 21, 4.4f}, {21, 22, 3.4f},
};

static void FillCapsules(cairo_t* cr, const render::Camera& camera, const LivePlayer& player, uint32_t rgba) {
    Rgb color = UnpackRgb(rgba);
    cairo_push_group(cr);
    cairo_set_source_rgb(cr, color.r, color.g, color.b);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (const ChamsCapsule& capsule : kCapsules) {
        Vec3 a, b;
        float ax, ay, bx, by;
        if (!player.Bone(capsule.from, a) || !player.Bone(capsule.to, b)) continue;
        if (!camera.Project(a, ax, ay) || !camera.Project(b, bx, by)) continue;
        float scale = camera.PixelsPerUnit({(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f});
        cairo_set_line_width(cr, std::max(2.f, capsule.radius * 2.f * scale));
        cairo_move_to(cr, ax, ay);
        cairo_line_to(cr, bx, by);
        cairo_stroke(cr);
    }
    Vec3 head;
    float hx, hy;
    if (player.Bone(7, head) && camera.Project({head.x, head.y, head.z + 3.f}, hx, hy)) {
        cairo_arc(cr, hx, hy, std::max(2.f, 5.2f * camera.PixelsPerUnit(head)), 0, 2 * kPi);
        cairo_fill(cr);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, (rgba & 0xFF) / 255.0);
}

static bool RoughlyOnScreen(const render::Camera& camera, const LivePlayer& player) {
    float sx, sy;
    Vec3 center{player.origin.x, player.origin.y, player.origin.z + 36.f};
    if (!camera.Project(center, sx, sy)) return camera.Depth(center) > -60.f;
    float margin = camera.width * 0.6f;
    return sx > -margin && sx < camera.width + margin && sy > -margin && sy < camera.height + margin;
}

void DrawChams(cairo_t* cr, const render::Camera& camera, const std::vector<LivePlayer>& players, const Settings& settings) {
    if (!settings::Enabled(settings.chams) || !camera.valid) return;
    static chams::ModelRenderer renderer;
    renderer.Begin(camera.width, camera.height);
    int local_team = g_hud.local_team.load();
    bool show_team = settings::Enabled(settings.chams_team);
    auto material = settings.chams_material == static_cast<uint32_t>(chams::Material::Flat) ? chams::Material::Flat : chams::Material::Metallic;
    for (const LivePlayer& player : players) {
        bool enemy = player.Enemy(local_team);
        if (!enemy && !show_team) continue;
        if (!RoughlyOnScreen(camera, player)) continue;
        uint32_t rgba = !enemy ? settings.chams_team_rgba
                       : (player.info.spotted_valid && player.info.visible ? settings.chams_visible_rgba : settings.chams_hidden_rgba);
        if (player.model && static_cast<int>(player.bones.size()) >= player.model->bone_count)
            renderer.Draw(*player.model, player.bones.data(), camera, rgba, material);
        else if (!player.bones.empty())
            FillCapsules(cr, camera, player, rgba);
    }
    renderer.Paint(cr);
}

struct PlayerAnimation {
    float alpha = 0.f;
    float health = 100.f;
    std::chrono::steady_clock::time_point seen;
};

static float FrameDelta() {
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    float delta = std::chrono::duration<float>(now - last).count();
    last = now;
    return std::clamp(delta, 0.f, 0.1f);
}

static void OutlinedText(cairo_t* cr, const char* text, double center_x, double baseline, double size,
                         Rgb color, double alpha, bool bold) {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_text_extents_t extents;
    cairo_text_extents(cr, text, &extents);
    double x = std::round(center_x - extents.width * 0.5 - extents.x_bearing);
    double y = std::round(baseline);
    cairo_new_path(cr);
    cairo_move_to(cr, x, y);
    cairo_text_path(cr, text);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, 3.0);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.75 * alpha);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgba(cr, color.r, color.g, color.b, alpha);
    cairo_fill(cr);
}

static Rgb HealthColor(float fraction) {
    fraction = std::clamp(fraction, 0.f, 1.f);
    if (fraction > 0.5f) {
        float t = (fraction - 0.5f) * 2.f;
        return {1.0 - 0.7 * t, 0.85 + 0.1 * t, 0.25 + 0.1 * t};
    }
    float t = fraction * 2.f;
    return {1.0, 0.3 + 0.55 * t, 0.25};
}

struct ScreenBox { double left, top, right, bottom; };

static bool ComputeBox(const render::Camera& camera, const LivePlayer& player, ScreenBox& box) {
    double min_x = 1e9, min_y = 1e9, max_x = -1e9, max_y = -1e9;
    int projected = 0;
    auto include = [&](const Vec3& point) {
        float sx, sy;
        if (!camera.Project(point, sx, sy)) return;
        min_x = std::min<double>(min_x, sx); max_x = std::max<double>(max_x, sx);
        min_y = std::min<double>(min_y, sy); max_y = std::max<double>(max_y, sy);
        projected++;
    };
    Vec3 head;
    if (player.Bone(7, head)) {
        for (int bone = 1; bone < kSkeletonBones; bone++) {
            Vec3 point;
            if (player.Bone(bone, point)) include(point);
        }
        include({head.x, head.y, head.z + 8.f});
        include(player.origin);
    } else {
        include(player.origin);
        include({player.info.world_head_x, player.info.world_head_y, player.info.world_head_z + 4.f});
    }
    if (projected < 2) return false;
    double height = max_y - min_y;
    if (height < 4.0) return false;
    double width = std::max(max_x - min_x + height * 0.12, height * 0.42);
    double center = (min_x + max_x) * 0.5;
    box = {center - width * 0.5, min_y - height * 0.03, center + width * 0.5, max_y + height * 0.02};
    return box.right > 0 && box.left < camera.width && box.bottom > 0 && box.top < camera.height;
}

static void DrawSkeleton(cairo_t* cr, const render::Camera& camera, const LivePlayer& player, Rgb color, double alpha, bool head_circle) {
    static constexpr int kLinks[][2] = {
        {7, 6}, {6, 5}, {5, 4}, {4, 2}, {2, 1},
        {5, 9}, {9, 10}, {10, 11}, {5, 13}, {13, 14}, {14, 15},
        {1, 17}, {17, 18}, {18, 19}, {1, 20}, {20, 21}, {21, 22},
    };
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_new_path(cr);
    for (const auto& link : kLinks) {
        if (head_circle && link[0] == 7) continue;
        Vec3 a, b;
        float ax, ay, bx, by;
        if (!player.Bone(link[0], a) || !player.Bone(link[1], b)) continue;
        if (!camera.Project(a, ax, ay) || !camera.Project(b, bx, by)) continue;
        cairo_move_to(cr, ax, ay);
        cairo_line_to(cr, bx, by);
    }
    Vec3 head;
    float hx, hy;
    bool have_head = head_circle && player.Bone(7, head) && camera.Project({head.x, head.y, head.z + 2.f}, hx, hy);
    double radius = have_head ? std::max(2.5, 4.6 * camera.PixelsPerUnit(head)) : 0.0;
    if (have_head) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, hx, hy, radius, 0, 2 * kPi);
    }
    cairo_set_line_width(cr, 3.0);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.55 * alpha);
    cairo_stroke_preserve(cr);
    cairo_set_line_width(cr, 1.4);
    cairo_set_source_rgba(cr, color.r, color.g, color.b, alpha);
    cairo_stroke(cr);
}

static void RoundedRect(cairo_t* cr, double x, double y, double w, double h, double r) {
    r = std::min({r, w * 0.5, h * 0.5});
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -kPi / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, kPi / 2);
    cairo_arc(cr, x + r, y + h - r, r, kPi / 2, kPi);
    cairo_arc(cr, x + r, y + r, r, kPi, 3 * kPi / 2);
    cairo_close_path(cr);
}

void DrawEsp(cairo_t* cr, const render::Camera& camera, const std::vector<LivePlayer>& players, const Settings& settings) {
    static std::unordered_map<uintptr_t, PlayerAnimation> animations;
    float delta = FrameDelta();
    auto now = std::chrono::steady_clock::now();
    if (!settings::Enabled(settings.esp) || !camera.valid) {
        animations.clear();
        return;
    }
    int local_team = g_hud.local_team.load();
    for (const LivePlayer& player : players) {
        bool enemy = player.Enemy(local_team);
        if (!enemy && settings::Enabled(settings.esp_team_check)) continue;
        PlayerAnimation& animation = animations[player.info.pawn];
        animation.seen = now;
        animation.alpha = std::min(1.f, animation.alpha + delta / 0.18f);
        animation.health += (player.info.hp - animation.health) * std::min(1.f, delta * 10.f);

        ScreenBox box;
        if (!ComputeBox(camera, player, box)) continue;
        bool visible = player.info.spotted_valid && player.info.visible;
        Rgb color = enemy ? Rgb{1.0, 0.32, 0.32} : Rgb{0.35, 0.72, 1.0};
        double alpha = animation.alpha * (enemy && player.info.spotted_valid && !visible ? 0.7 : 1.0);
        double width = box.right - box.left, height = box.bottom - box.top;

        if (settings::Enabled(settings.esp_box)) {
            RoundedRect(cr, box.left, box.top, width, height, 3.0);
            cairo_set_line_width(cr, 3.0);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.55 * alpha);
            cairo_stroke_preserve(cr);
            cairo_set_line_width(cr, 1.3);
            cairo_set_source_rgba(cr, color.r, color.g, color.b, alpha);
            cairo_stroke(cr);
        }
        if (settings::Enabled(settings.esp_skeleton) && !player.bones.empty())
            DrawSkeleton(cr, camera, player, color, alpha, settings::Enabled(settings.esp_head_circle));

        if (settings::Enabled(settings.esp_health)) {
            float fraction = std::clamp(animation.health / 100.f, 0.f, 1.f);
            double bar_x = box.left - 6.0, bar_w = 3.0;
            RoundedRect(cr, bar_x - 1, box.top - 1, bar_w + 2, height + 2, 1.5);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.6 * alpha);
            cairo_fill(cr);
            double fill = height * fraction;
            Rgb health = HealthColor(fraction);
            cairo_pattern_t* gradient = cairo_pattern_create_linear(0, box.bottom - fill, 0, box.bottom);
            cairo_pattern_add_color_stop_rgba(gradient, 0, health.r, health.g, health.b, alpha);
            cairo_pattern_add_color_stop_rgba(gradient, 1, health.r * 0.7, health.g * 0.7, health.b * 0.7, alpha);
            RoundedRect(cr, bar_x, box.bottom - fill, bar_w, fill, 1.0);
            cairo_set_source(cr, gradient);
            cairo_fill(cr);
            cairo_pattern_destroy(gradient);
            if (player.info.hp < 100) {
                char text[16];
                snprintf(text, sizeof(text), "%d", player.info.hp);
                OutlinedText(cr, text, bar_x + bar_w * 0.5, box.bottom - fill + 3.0, 9.0, {1, 1, 1}, alpha, true);
            }
        }
        if (settings::Enabled(settings.esp_name) && player.info.name[0])
            OutlinedText(cr, player.info.name, (box.left + box.right) * 0.5, box.top - 5.0, 12.0, {1, 1, 1}, alpha, true);
        if (settings::Enabled(settings.esp_weapon) && player.info.weapon[0])
            OutlinedText(cr, player.info.weapon, (box.left + box.right) * 0.5, box.bottom + 13.0, 10.0, {0.85, 0.87, 0.9}, alpha, false);
    }
    for (auto it = animations.begin(); it != animations.end();) {
        if (now - it->second.seen > std::chrono::milliseconds(500)) it = animations.erase(it);
        else ++it;
    }
}

void DrawArrows(cairo_t* cr, const render::Camera& camera, const std::vector<LivePlayer>& players, const Settings& settings) {
    if (!settings::Enabled(settings.arrows) || !camera.valid) return;
    double cx = camera.width * 0.5, cy = camera.height * 0.5;
    double radius = std::clamp(settings.arrows_radius, 40, 500);
    double size = std::clamp(settings.arrows_size, 6, 32);
    Rgb color = UnpackRgb(settings.arrows_rgba);
    double color_alpha = (settings.arrows_rgba & 0xFF) / 255.0;
    int local_team = g_hud.local_team.load();
    for (const LivePlayer& player : players) {
        if (!player.Enemy(local_team)) continue;
        float sx, sy;
        Vec3 chest{player.origin.x, player.origin.y, player.origin.z + 40.f};
        if (camera.Project(chest, sx, sy) && sx >= 0 && sx <= camera.width && sy >= 0 && sy <= camera.height) continue;
        double target_yaw = std::atan2(player.origin.y - camera.eye.y, player.origin.x - camera.eye.x) * 180.0 / kPi;
        double relative = std::remainder(target_yaw - camera.angles.y, 360.0);
        double angle = (-relative - 90.0) * kPi / 180.0;
        double px = cx + radius * std::cos(angle), py = cy + radius * std::sin(angle);
        cairo_new_path(cr);
        cairo_move_to(cr, px + size * std::cos(angle), py + size * std::sin(angle));
        cairo_line_to(cr, px + size * 0.7 * std::cos(angle + 2.4), py + size * 0.7 * std::sin(angle + 2.4));
        cairo_line_to(cr, px + size * 0.7 * std::cos(angle - 2.4), py + size * 0.7 * std::sin(angle - 2.4));
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, color.r, color.g, color.b, color_alpha);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.9);
        cairo_set_line_width(cr, 1.2);
        cairo_stroke(cr);
    }
}

}
