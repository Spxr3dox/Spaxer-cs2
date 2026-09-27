#include "overlay/weather.h"
#include "config/settings.h"
#include "sdk/visibility.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <vector>

namespace weather {

namespace {

using Clock = std::chrono::steady_clock;

constexpr float kTwoPi = 6.2831853f;
constexpr float kMaxFrameTime = 0.05f;
constexpr float kTeleportDistance = 600.f;
constexpr float kGroundProbeDepth = 1500.f;
constexpr float kRoofProbeHeight = 1200.f;
constexpr float kSplashSeconds = 0.18f;
constexpr int kVisibilityStride = 6;
constexpr int kSpawnAttemptsPerFrame = 80;

struct Style {
    int max_count;
    float radius;
    float spawn_low, spawn_high;
    Vec3 velocity;
    float sway;
    float size_min, size_max;
    float r, g, b, alpha;
    float lifetime;
};

constexpr Style kRain{1400, 650.f, 150.f, 600.f, {60.f, 25.f, -1100.f}, 0.f, 0.5f, 0.7f, 0.72f, 0.80f, 0.92f, 0.45f, 0.f};
constexpr Style kSnow{1100, 550.f, 50.f, 450.f, {8.f, 4.f, -70.f}, 18.f, 1.2f, 2.2f, 1.f, 1.f, 1.f, 0.85f, 0.f};
constexpr Style kAsh{650, 550.f, 30.f, 400.f, {6.f, 3.f, -25.f}, 12.f, 0.8f, 1.6f, 0.55f, 0.55f, 0.55f, 0.75f, 0.f};
constexpr Style kEmbers{300, 500.f, 0.f, 120.f, {4.f, 2.f, 35.f}, 20.f, 1.4f, 2.4f, 1.f, 0.55f, 0.15f, 1.f, 6.f};

struct Particle {
    Vec3 pos{};
    float floor_z = 0.f;
    float phase = 0.f;
    float size = 1.f;
    float age = 0.f;
    float splash = -1.f;
    bool alive = false;
    bool visible = false;
};

class Simulation {
public:
    void Draw(cairo_t* cr, const render::Camera& camera, Mode mode, int density) {
        auto now = Clock::now();
        float dt = last_ == Clock::time_point{} ? 0.f : std::chrono::duration<float>(now - last_).count();
        last_ = now;
        dt = std::min(dt, kMaxFrameTime);
        time_ += dt;

        const Style* style = StyleFor(mode);
        if (!style || !camera.valid) {
            particles_.clear();
            return;
        }
        if (mode != mode_ || Distance2D(camera.eye, last_eye_) > kTeleportDistance) {
            particles_.clear();
            mode_ = mode;
        }
        last_eye_ = camera.eye;
        size_t wanted = static_cast<size_t>(style->max_count * std::clamp(density, 5, 100) / 100);
        bool warmup = particles_.empty();
        particles_.resize(wanted);

        int spawn_budget = warmup ? static_cast<int>(wanted) * 2 : kSpawnAttemptsPerFrame;
        frame_++;
        for (size_t i = 0; i < particles_.size(); i++) {
            Particle& p = particles_[i];
            if (!p.alive) {
                if (spawn_budget-- > 0) Spawn(p, camera.eye, *style, warmup);
                continue;
            }
            Step(p, *style, dt, mode);
            if (!p.alive) continue;
            if (Distance2D(p.pos, camera.eye) > style->radius * 1.15f) {
                p.alive = false;
                continue;
            }
            if ((i + frame_) % kVisibilityStride == 0) p.visible = vis::LineOfSight(camera.eye, p.pos);
        }
        Render(cr, camera, *style, mode);
    }

private:
    static const Style* StyleFor(Mode mode) {
        switch (mode) {
            case Mode::Rain: return &kRain;
            case Mode::Snow: return &kSnow;
            case Mode::Ash: return &kAsh;
            case Mode::Embers: return &kEmbers;
            default: return nullptr;
        }
    }

    static float Distance2D(const Vec3& a, const Vec3& b) {
        return std::hypot(a.x - b.x, a.y - b.y);
    }

    float Random(float low, float high) {
        return std::uniform_real_distribution<float>(low, high)(rng_);
    }

    void Spawn(Particle& p, const Vec3& eye, const Style& style, bool warmup) {
        float angle = Random(0.f, kTwoPi);
        float distance = style.radius * std::sqrt(Random(0.f, 1.f));
        float x = eye.x + std::cos(angle) * distance, y = eye.y + std::sin(angle) * distance;
        float floor_z = eye.z - 64.f;
        if (vis::Ready()) {
            Vec3 ground;
            if (!vis::Raycast({x, y, eye.z + 64.f}, {x, y, eye.z - kGroundProbeDepth}, ground)) return;
            Vec3 roof;
            if (vis::Raycast({x, y, ground.z + 8.f}, {x, y, ground.z + kRoofProbeHeight}, roof)) return;
            floor_z = ground.z;
        }
        float base = std::max(floor_z, eye.z - 64.f);
        float z = style.velocity.z > 0.f ? floor_z + Random(style.spawn_low, style.spawn_high)
                                         : base + Random(style.spawn_low, style.spawn_high);
        if (warmup && style.velocity.z < 0.f) z = Random(floor_z, z);
        p = Particle{};
        p.pos = {x, y, z};
        p.floor_z = floor_z;
        p.phase = Random(0.f, kTwoPi);
        p.size = Random(style.size_min, style.size_max);
        p.age = warmup && style.lifetime > 0.f ? Random(0.f, style.lifetime) : 0.f;
        p.alive = true;
    }

    void Step(Particle& p, const Style& style, float dt, Mode mode) {
        if (p.splash >= 0.f) {
            p.splash += dt;
            if (p.splash > kSplashSeconds) p.alive = false;
            return;
        }
        float sway_x = style.sway * std::sin(time_ * 1.3f + p.phase);
        float sway_y = style.sway * std::cos(time_ * 1.1f + p.phase * 1.7f);
        p.pos.x += (style.velocity.x + sway_x) * dt;
        p.pos.y += (style.velocity.y + sway_y) * dt;
        p.pos.z += style.velocity.z * dt;
        p.age += dt;
        if (style.lifetime > 0.f && p.age > style.lifetime) p.alive = false;
        if (p.pos.z <= p.floor_z) {
            p.pos.z = p.floor_z;
            if (mode == Mode::Rain) p.splash = 0.f;
            else p.alive = false;
        }
    }

    void Render(cairo_t* cr, const render::Camera& camera, const Style& style, Mode mode) {
        cairo_save(cr);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        for (const Particle& p : particles_) {
            if (!p.alive || !p.visible) continue;
            float sx, sy;
            if (!camera.Project(p.pos, sx, sy)) continue;
            if (sx < -20.f || sy < -20.f || sx > camera.width + 20.f || sy > camera.height + 20.f) continue;
            float ppu = camera.PixelsPerUnit(p.pos);
            float fade = std::clamp(1.f - std::hypot(p.pos.x - camera.eye.x, p.pos.y - camera.eye.y) / style.radius, 0.f, 1.f);
            float alpha = style.alpha * (0.35f + 0.65f * fade);
            switch (mode) {
                case Mode::Rain: DrawRain(cr, camera, p, style, alpha, ppu, sx, sy); break;
                case Mode::Embers: DrawEmber(cr, p, style, alpha, ppu, sx, sy); break;
                default: DrawFlake(cr, p, style, alpha, ppu, sx, sy); break;
            }
        }
        cairo_restore(cr);
    }

    void DrawRain(cairo_t* cr, const render::Camera& camera, const Particle& p, const Style& style, float alpha,
                  float ppu, float sx, float sy) {
        if (p.splash >= 0.f) {
            float progress = p.splash / kSplashSeconds;
            cairo_set_source_rgba(cr, style.r, style.g, style.b, alpha * (1.f - progress));
            cairo_set_line_width(cr, 1.f);
            cairo_save(cr);
            cairo_translate(cr, sx, sy);
            cairo_scale(cr, 1.0, 0.35);
            cairo_arc(cr, 0, 0, std::max(1.5f, (1.f + progress * 5.f) * ppu), 0, kTwoPi);
            cairo_restore(cr);
            cairo_stroke(cr);
            return;
        }
        Vec3 tail{p.pos.x - style.velocity.x * 0.025f, p.pos.y - style.velocity.y * 0.025f, p.pos.z - style.velocity.z * 0.025f};
        float tx, ty;
        if (!camera.Project(tail, tx, ty)) return;
        cairo_set_source_rgba(cr, style.r, style.g, style.b, alpha);
        cairo_set_line_width(cr, std::clamp(p.size * ppu, 0.6f, 2.5f));
        cairo_move_to(cr, sx, sy);
        cairo_line_to(cr, tx, ty);
        cairo_stroke(cr);
    }

    void DrawFlake(cairo_t* cr, const Particle& p, const Style& style, float alpha, float ppu, float sx, float sy) {
        cairo_set_source_rgba(cr, style.r, style.g, style.b, alpha);
        cairo_arc(cr, sx, sy, std::clamp(p.size * ppu, 0.6f, 5.f), 0, kTwoPi);
        cairo_fill(cr);
    }

    void DrawEmber(cairo_t* cr, const Particle& p, const Style& style, float alpha, float ppu, float sx, float sy) {
        float flicker = 0.55f + 0.45f * std::sin(time_ * 9.f + p.phase * 3.f);
        float life = style.lifetime > 0.f ? std::clamp(1.f - p.age / style.lifetime, 0.f, 1.f) : 1.f;
        float radius = std::clamp(p.size * ppu, 0.8f, 4.f);
        cairo_pattern_t* glow = cairo_pattern_create_radial(sx, sy, 0, sx, sy, radius * 3.f);
        cairo_pattern_add_color_stop_rgba(glow, 0, 1.0, 0.85, 0.5, alpha * flicker * life);
        cairo_pattern_add_color_stop_rgba(glow, 0.35, style.r, style.g, style.b, alpha * 0.6f * flicker * life);
        cairo_pattern_add_color_stop_rgba(glow, 1, style.r, style.g * 0.5, style.b, 0);
        cairo_set_source(cr, glow);
        cairo_arc(cr, sx, sy, radius * 3.f, 0, kTwoPi);
        cairo_fill(cr);
        cairo_pattern_destroy(glow);
    }

    std::vector<Particle> particles_;
    std::mt19937 rng_{std::random_device{}()};
    Clock::time_point last_{};
    Vec3 last_eye_{};
    Mode mode_ = Mode::Off;
    float time_ = 0.f;
    uint64_t frame_ = 0;
};

Simulation s_simulation;

}

void Draw(cairo_t* cr, const render::Camera& camera, const Settings& settings) {
    Mode mode = settings.weather_mode <= static_cast<uint32_t>(Mode::Embers) ? static_cast<Mode>(settings.weather_mode) : Mode::Off;
    s_simulation.Draw(cr, camera, mode, settings.weather_density);
}

}
