#include "overlay/night_sky.h"
#include "config/settings.h"
#include "sdk/visibility.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace nightsky {

namespace {

constexpr int kMaskWidth = 128;
constexpr int kMaskHeight = 72;
constexpr float kRayLength = 16000.f;
constexpr float kFarDistance = 50000.f;
constexpr int kStarCount = 700;
constexpr float kPi = 3.14159265f;
constexpr float kMoonYaw = 35.f * kPi / 180.f;
constexpr float kMoonElevation = 32.f * kPi / 180.f;
constexpr float kMoonAngularRadius = 1.4f * kPi / 180.f;
constexpr float kSunCoverAngularRadius = 9.f * kPi / 180.f;

struct Star { Vec3 dir; float size; float brightness; float phase; float rate; float warmth; };

struct Mask {
    std::vector<uint8_t> pixels;
    int stride = 0;
    bool valid = false;
};

class MaskWorker {
public:
    void Submit(const render::Camera& camera) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = camera;
            has_pending_ = true;
        }
        if (!running_.exchange(true)) thread_ = std::thread([this] { Run(); });
        wake_.notify_one();
    }

    bool Latest(Mask& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_.valid) return false;
        out = ready_;
        return true;
    }

    void Stop() {
        if (!running_.exchange(false)) return;
        wake_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

    ~MaskWorker() { Stop(); }

private:
    void Run() {
        std::vector<Vec3> directions(kMaskWidth * kMaskHeight);
        std::vector<uint8_t> blocked(directions.size());
        while (running_.load()) {
            render::Camera camera;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return has_pending_ || !running_.load(); });
                if (!running_.load()) return;
                camera = pending_;
                has_pending_ = false;
            }
            for (int y = 0; y < kMaskHeight; y++) {
                float v = (1.f - (y + 0.5f) / kMaskHeight * 2.f) * camera.tan_half_v;
                for (int x = 0; x < kMaskWidth; x++) {
                    float u = ((x + 0.5f) / kMaskWidth * 2.f - 1.f) * camera.tan_half_h;
                    Vec3 d{camera.forward.x + camera.right.x * u + camera.up.x * v,
                           camera.forward.y + camera.right.y * u + camera.up.y * v,
                           camera.forward.z + camera.right.z * u + camera.up.z * v};
                    float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                    directions[y * kMaskWidth + x] = {d.x / length, d.y / length, d.z / length};
                }
            }
            if (!vis::CastBatch(camera.eye, directions.data(), directions.size(), kRayLength, blocked.data())) continue;
            Mask mask;
            mask.stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, kMaskWidth);
            mask.pixels.assign(static_cast<size_t>(mask.stride) * kMaskHeight, 0);
            for (int y = 0; y < kMaskHeight; y++)
                for (int x = 0; x < kMaskWidth; x++)
                    if (!blocked[y * kMaskWidth + x] && directions[y * kMaskWidth + x].z > -0.05f)
                        mask.pixels[y * mask.stride + x] = 255;
            mask.valid = true;
            std::lock_guard<std::mutex> lock(mutex_);
            ready_ = std::move(mask);
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    render::Camera pending_;
    bool has_pending_ = false;
    Mask ready_;
};

std::vector<Star> MakeStars() {
    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> unit(0.f, 1.f);
    std::vector<Star> stars;
    stars.reserve(kStarCount);
    while (static_cast<int>(stars.size()) < kStarCount) {
        float z = unit(rng) * 0.98f + 0.02f;
        float angle = unit(rng) * 2.f * kPi;
        float r = std::sqrt(1.f - z * z);
        float magnitude = std::pow(unit(rng), 3.f);
        stars.push_back({{r * std::cos(angle), r * std::sin(angle), z}, 0.6f + magnitude * 1.6f,
                         0.35f + magnitude * 0.65f, unit(rng) * 2.f * kPi, 1.5f + unit(rng) * 3.f, unit(rng)});
    }
    return stars;
}

MaskWorker s_worker;
const std::vector<Star> s_stars = MakeStars();
const auto s_start = std::chrono::steady_clock::now();

Vec3 Far(const Vec3& eye, const Vec3& dir) {
    return {eye.x + dir.x * kFarDistance, eye.y + dir.y * kFarDistance, eye.z + dir.z * kFarDistance};
}

bool IsOpenSky(const Mask& mask, const render::Camera& camera, float sx, float sy) {
    int cx = static_cast<int>(sx / camera.width * kMaskWidth);
    int cy = static_cast<int>(sy / camera.height * kMaskHeight);
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= kMaskWidth || y >= kMaskHeight) continue;
            if (!mask.pixels[y * mask.stride + x]) return false;
        }
    return cx >= 0 && cy >= 0 && cx < kMaskWidth && cy < kMaskHeight;
}

void PaintStars(cairo_t* cr, const render::Camera& camera, const Mask& mask, float time) {
    for (const Star& star : s_stars) {
        float sx, sy;
        if (!camera.Project(Far(camera.eye, star.dir), sx, sy)) continue;
        if (sx < 0 || sy < 0 || sx >= camera.width || sy >= camera.height || !IsOpenSky(mask, camera, sx, sy)) continue;
        float twinkle = 0.75f + 0.25f * std::sin(time * star.rate + star.phase);
        cairo_set_source_rgba(cr, 0.85f + 0.15f * star.warmth, 0.88f, 1.0f - 0.2f * star.warmth, star.brightness * twinkle);
        cairo_arc(cr, sx, sy, star.size, 0, 2 * kPi);
        cairo_fill(cr);
    }
}

void PaintMoon(cairo_t* cr, const render::Camera& camera, const Mask& mask) {
    Vec3 moon_dir{std::cos(kMoonElevation) * std::cos(kMoonYaw), std::cos(kMoonElevation) * std::sin(kMoonYaw),
                  std::sin(kMoonElevation)};
    bool covers_sun = vis::SunDirection(moon_dir);
    float mx, my;
    if (!camera.Project(Far(camera.eye, moon_dir), mx, my)) return;
    if (mx < -camera.width * 0.2f || my < -camera.height * 0.2f || mx > camera.width * 1.2f || my > camera.height * 1.2f) return;
    if (!covers_sun && !IsOpenSky(mask, camera, mx, my)) return;
    float radius = std::tan(kMoonAngularRadius) / camera.tan_half_h * camera.width * 0.5f;
    if (covers_sun) {
        float cover = std::tan(kSunCoverAngularRadius) / camera.tan_half_h * camera.width * 0.5f;
        cairo_pattern_t* shade = cairo_pattern_create_radial(mx, my, cover * 0.45f, mx, my, cover);
        cairo_pattern_add_color_stop_rgba(shade, 0, 0.012, 0.02, 0.05, 1.0);
        cairo_pattern_add_color_stop_rgba(shade, 1, 0.012, 0.02, 0.05, 0.0);
        cairo_set_source(cr, shade);
        cairo_arc(cr, mx, my, cover, 0, 2 * kPi);
        cairo_fill(cr);
        cairo_pattern_destroy(shade);
    }
    cairo_pattern_t* halo = cairo_pattern_create_radial(mx, my, radius * 0.9f, mx, my, radius * 5.f);
    cairo_pattern_add_color_stop_rgba(halo, 0, 0.75, 0.82, 1.0, 0.18);
    cairo_pattern_add_color_stop_rgba(halo, 1, 0.75, 0.82, 1.0, 0.0);
    cairo_set_source(cr, halo);
    cairo_arc(cr, mx, my, radius * 5.f, 0, 2 * kPi);
    cairo_fill(cr);
    cairo_pattern_destroy(halo);
    cairo_pattern_t* disc = cairo_pattern_create_radial(mx - radius * 0.3f, my - radius * 0.3f, 0, mx, my, radius);
    cairo_pattern_add_color_stop_rgb(disc, 0, 0.97, 0.97, 0.93);
    cairo_pattern_add_color_stop_rgb(disc, 1, 0.80, 0.82, 0.80);
    cairo_set_source(cr, disc);
    cairo_arc(cr, mx, my, radius, 0, 2 * kPi);
    cairo_fill(cr);
    cairo_pattern_destroy(disc);
    const float kCraters[][3] = {{-0.3f, -0.1f, 0.22f}, {0.25f, 0.2f, 0.16f}, {0.05f, -0.4f, 0.12f}, {0.35f, -0.25f, 0.09f}};
    for (const auto& crater : kCraters) {
        cairo_set_source_rgba(cr, 0.55, 0.57, 0.58, 0.35);
        cairo_arc(cr, mx + crater[0] * radius, my + crater[1] * radius, crater[2] * radius, 0, 2 * kPi);
        cairo_fill(cr);
    }
}

}

void Draw(cairo_t* cr, const render::Camera& camera, const Settings& settings) {
    if (!settings::Enabled(settings.night_mode) || !settings::Enabled(settings.night_sky) || !camera.valid || !vis::Ready())
        return;
    s_worker.Submit(camera);
    Mask mask;
    if (!s_worker.Latest(mask)) return;
    float time = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_start).count();

    cairo_save(cr);
    PaintStars(cr, camera, mask, time);
    PaintMoon(cr, camera, mask);
    cairo_restore(cr);
}

void Shutdown() {
    s_worker.Stop();
}

}
