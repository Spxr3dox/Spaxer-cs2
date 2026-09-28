#include "render/model_chams.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace chams {

constexpr float kRenderScale = 0.5f;
constexpr int kBands = 12;
constexpr int kThreads = 6;

static constexpr uint32_t kFormatVersion = 2;

static std::string ModelsDir() {
    const char* home = getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.config/spaxer/models/";
}

template <typename T>
static bool ReadArray(FILE* file, std::vector<T>& out, size_t count) {
    out.resize(count);
    return fread(out.data(), sizeof(T), count, file) == count;
}

static std::unique_ptr<SkinnedModel> LoadModel(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return nullptr;
    struct Header { char magic[4]; uint32_t version, vertex_count, index_count; } header{};
    auto model = std::make_unique<SkinnedModel>();
    bool ok = fread(&header, sizeof(header), 1, file) == 1 && memcmp(header.magic, "SMDL", 4) == 0 &&
              header.version == kFormatVersion &&
              ReadArray(file, model->joints, header.vertex_count * 4) &&
              ReadArray(file, model->weights, header.vertex_count * 4) &&
              ReadArray(file, model->local, header.vertex_count * 12) &&
              ReadArray(file, model->local_normals, header.vertex_count * 12) &&
              ReadArray(file, model->indices, header.index_count);
    fclose(file);
    if (!ok) return nullptr;
    for (uint32_t index : model->indices)
        if (index >= header.vertex_count) return nullptr;
    model->bone_count = model->joints.empty() ? 0 : *std::max_element(model->joints.begin(), model->joints.end()) + 1;
    return model;
}

const SkinnedModel* FindModel(const std::string& stem) {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::unique_ptr<SkinnedModel>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(stem);
    if (it == cache.end()) it = cache.emplace(stem, LoadModel(ModelsDir() + stem + ".smdl")).first;
    return it->second.get();
}

static inline void Rotate(const BoneTransform& b, const float* v, float& ox, float& oy, float& oz) {
    float tx = 2.f * (b.qy * v[2] - b.qz * v[1]);
    float ty = 2.f * (b.qz * v[0] - b.qx * v[2]);
    float tz = 2.f * (b.qx * v[1] - b.qy * v[0]);
    ox = v[0] + b.qw * tx + (b.qy * tz - b.qz * ty);
    oy = v[1] + b.qw * ty + (b.qz * tx - b.qx * tz);
    oz = v[2] + b.qw * tz + (b.qx * ty - b.qy * tx);
}

void ModelRenderer::Begin(int screen_width, int screen_height) {
    int width = std::max(1, static_cast<int>(screen_width * kRenderScale));
    int height = std::max(1, static_cast<int>(screen_height * kRenderScale));
    if (width != width_ || height != height_) {
        width_ = width;
        height_ = height;
        color_.assign(static_cast<size_t>(width) * height, 0u);
        depth_.assign(static_cast<size_t>(width) * height, 0.f);
        min_x_ = min_y_ = 1;
        max_x_ = max_y_ = 0;
    }
    ClearDirty();
}

void ModelRenderer::ClearDirty() {
    if (max_x_ < min_x_) return;
    for (int y = min_y_; y <= max_y_; y++) {
        size_t row = static_cast<size_t>(y) * width_;
        std::fill(color_.begin() + row + min_x_, color_.begin() + row + max_x_ + 1, 0u);
        std::fill(depth_.begin() + row + min_x_, depth_.begin() + row + max_x_ + 1, 0.f);
    }
    min_x_ = min_y_ = 1;
    max_x_ = max_y_ = 0;
}

void ModelRenderer::Skin(const SkinnedModel& model, const BoneTransform* bones) {
    size_t count = model.VertexCount();
    positions_.resize(count * 3);
    normals_.resize(count * 3);
    #pragma omp parallel for schedule(static) num_threads(kThreads)
    for (size_t v = 0; v < count; v++) {
        float px = 0.f, py = 0.f, pz = 0.f, nx = 0.f, ny = 0.f, nz = 0.f;
        for (int k = 0; k < 4; k++) {
            float w = model.weights[v * 4 + k];
            if (w <= 0.f) continue;
            const BoneTransform& bone = bones[model.joints[v * 4 + k]];
            float rx, ry, rz;
            Rotate(bone, &model.local[v * 12 + k * 3], rx, ry, rz);
            px += w * (bone.x + rx);
            py += w * (bone.y + ry);
            pz += w * (bone.z + rz);
            Rotate(bone, &model.local_normals[v * 12 + k * 3], rx, ry, rz);
            nx += w * rx;
            ny += w * ry;
            nz += w * rz;
        }
        float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (length > 1e-6f) { nx /= length; ny /= length; nz /= length; }
        positions_[v * 3] = px; positions_[v * 3 + 1] = py; positions_[v * 3 + 2] = pz;
        normals_[v * 3] = nx; normals_[v * 3 + 1] = ny; normals_[v * 3 + 2] = nz;
    }
}

void ModelRenderer::Shade(const render::Camera& camera, uint32_t rgba, Material material) {
    float base_r = ((rgba >> 24) & 0xFF) / 255.f;
    float base_g = ((rgba >> 16) & 0xFF) / 255.f;
    float base_b = ((rgba >> 8) & 0xFF) / 255.f;
    float glint_r = base_r + (1.f - base_r) * 0.75f;
    float glint_g = base_g + (1.f - base_g) * 0.75f;
    float glint_b = base_b + (1.f - base_b) * 0.75f;
    size_t count = positions_.size() / 3;
    screen_.resize(count);
    #pragma omp parallel for schedule(static) num_threads(kThreads)
    for (size_t v = 0; v < count; v++) {
        Vec3 p{positions_[v * 3], positions_[v * 3 + 1], positions_[v * 3 + 2]};
        ScreenVertex& out = screen_[v];
        float depth = camera.Depth(p);
        out.valid = depth > 1.f && camera.Project(p, out.x, out.y);
        if (!out.valid) continue;
        out.x *= kRenderScale;
        out.y *= kRenderScale;
        out.inv_depth = 1.f / depth;
        if (material == Material::Flat) {
            out.r = base_r * alpha_byte_; out.g = base_g * alpha_byte_; out.b = base_b * alpha_byte_;
            continue;
        }
        float vx = camera.eye.x - p.x, vy = camera.eye.y - p.y, vz = camera.eye.z - p.z;
        float view_length = std::sqrt(vx * vx + vy * vy + vz * vz);
        vx /= view_length; vy /= view_length; vz /= view_length;
        float nx = normals_[v * 3], ny = normals_[v * 3 + 1], nz = normals_[v * 3 + 2];
        float n_dot_v = std::max(0.f, nx * vx + ny * vy + nz * vz);
        float lx = vx + camera.up.x * 0.8f, ly = vy + camera.up.y * 0.8f, lz = vz + camera.up.z * 0.8f;
        float light_length = std::sqrt(lx * lx + ly * ly + lz * lz);
        lx /= light_length; ly /= light_length; lz /= light_length;
        float diffuse = std::max(0.f, nx * lx + ny * ly + nz * lz);
        float hx = lx + vx, hy = ly + vy, hz = lz + vz;
        float half_length = std::sqrt(hx * hx + hy * hy + hz * hz);
        float n_dot_h = std::max(0.f, (nx * hx + ny * hy + nz * hz) / half_length);
        float h2 = n_dot_h * n_dot_h, h4 = h2 * h2, h8 = h4 * h4, h16 = h8 * h8;
        float specular = h16 * h8 * h4;
        float edge = 1.f - n_dot_v;
        float rim = edge * edge * std::sqrt(edge);
        float reflect_z = 2.f * n_dot_v * nz - vz;
        float environment = 0.5f + 0.5f * reflect_z;
        float body = 0.08f + 0.5f * diffuse + 0.38f * environment * environment;
        float shine = specular * 1.4f + rim * 0.75f;
        out.r = std::min(1.f, base_r * body + glint_r * shine) * alpha_byte_;
        out.g = std::min(1.f, base_g * body + glint_g * shine) * alpha_byte_;
        out.b = std::min(1.f, base_b * body + glint_b * shine) * alpha_byte_;
    }
}

void ModelRenderer::Rasterize(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c, int band_top,
                              int band_bottom, DirtyRect& dirty) {
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-4f) return;
    const ScreenVertex& v0 = a;
    const ScreenVertex& v1 = area > 0.f ? b : c;
    const ScreenVertex& v2 = area > 0.f ? c : b;
    float inv_area = 1.f / std::fabs(area);

    int x0 = std::max(0, static_cast<int>(std::floor(std::min({v0.x, v1.x, v2.x}))));
    int y0 = std::max(band_top, static_cast<int>(std::floor(std::min({v0.y, v1.y, v2.y}))));
    int x1 = std::min(width_ - 1, static_cast<int>(std::ceil(std::max({v0.x, v1.x, v2.x}))));
    int y1 = std::min(band_bottom, static_cast<int>(std::ceil(std::max({v0.y, v1.y, v2.y}))));
    if (x0 > x1 || y0 > y1) return;

    float ea[3] = {v1.y - v2.y, v2.y - v0.y, v0.y - v1.y};
    float eb[3] = {v2.x - v1.x, v0.x - v2.x, v1.x - v0.x};
    float ec[3] = {v1.x * v2.y - v2.x * v1.y, v2.x * v0.y - v0.x * v2.y, v0.x * v1.y - v1.x * v0.y};

    bool touched = false;
    for (int y = y0; y <= y1; y++) {
        float py = y + 0.5f;
        float span_min = static_cast<float>(x0), span_max = static_cast<float>(x1);
        bool empty = false;
        for (int e = 0; e < 3 && !empty; e++) {
            float offset = eb[e] * py + ec[e];
            if (ea[e] > 0.f) span_min = std::max(span_min, std::ceil(-offset / ea[e] - 0.5f));
            else if (ea[e] < 0.f) span_max = std::min(span_max, std::floor(-offset / ea[e] - 0.5f));
            else if (offset < 0.f) empty = true;
        }
        if (empty || span_min > span_max) continue;
        int sx0 = static_cast<int>(span_min), sx1 = static_cast<int>(span_max);
        float px = sx0 + 0.5f;
        float w0 = (ea[0] * px + eb[0] * py + ec[0]) * inv_area;
        float w1 = (ea[1] * px + eb[1] * py + ec[1]) * inv_area;
        float dw0 = ea[0] * inv_area, dw1 = ea[1] * inv_area;
        size_t row = static_cast<size_t>(y) * width_;
        for (int x = sx0; x <= sx1; x++, w0 += dw0, w1 += dw1) {
            float w2 = 1.f - w0 - w1;
            float inv_depth = w0 * v0.inv_depth + w1 * v1.inv_depth + w2 * v2.inv_depth;
            float& stored = depth_[row + x];
            if (inv_depth <= stored) continue;
            stored = inv_depth;
            uint32_t r = static_cast<uint32_t>(std::clamp(w0 * v0.r + w1 * v1.r + w2 * v2.r, 0.f, alpha_byte_));
            uint32_t g = static_cast<uint32_t>(std::clamp(w0 * v0.g + w1 * v1.g + w2 * v2.g, 0.f, alpha_byte_));
            uint32_t bl = static_cast<uint32_t>(std::clamp(w0 * v0.b + w1 * v1.b + w2 * v2.b, 0.f, alpha_byte_));
            color_[row + x] = alpha_bits_ | (r << 16) | (g << 8) | bl;
            touched = true;
        }
    }
    if (touched) dirty.Add(x0, y0, x1, y1);
}

void ModelRenderer::Draw(const SkinnedModel& model, const BoneTransform* bones, const render::Camera& camera,
                         uint32_t rgba, Material material) {
    if (width_ <= 0 || height_ <= 0 || !camera.valid) return;
    alpha_byte_ = static_cast<float>(rgba & 0xFF);
    alpha_bits_ = (rgba & 0xFFu) << 24;
    Skin(model, bones);
    Shade(camera, rgba, material);
    const std::vector<uint32_t>& indices = model.indices;
    visible_.clear();
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        uint32_t i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
        if (!screen_[i0].valid || !screen_[i1].valid || !screen_[i2].valid) continue;
        const float* p0 = &positions_[i0 * 3];
        const float* p1 = &positions_[i1 * 3];
        const float* p2 = &positions_[i2 * 3];
        float e1x = p1[0] - p0[0], e1y = p1[1] - p0[1], e1z = p1[2] - p0[2];
        float e2x = p2[0] - p0[0], e2y = p2[1] - p0[1], e2z = p2[2] - p0[2];
        float fx = e1y * e2z - e1z * e2y, fy = e1z * e2x - e1x * e2z, fz = e1x * e2y - e1y * e2x;
        float facing = fx * (camera.eye.x - p0[0]) + fy * (camera.eye.y - p0[1]) + fz * (camera.eye.z - p0[2]);
        if (facing > 0.f) visible_.push_back(static_cast<uint32_t>(t));
    }
    DirtyRect band_dirty[kBands];
    #pragma omp parallel for schedule(dynamic, 1) num_threads(kThreads)
    for (int band = 0; band < kBands; band++) {
        int top = band * height_ / kBands;
        int bottom = (band + 1) * height_ / kBands - 1;
        for (uint32_t t : visible_)
            Rasterize(screen_[indices[t]], screen_[indices[t + 1]], screen_[indices[t + 2]], top, bottom, band_dirty[band]);
    }
    for (const DirtyRect& rect : band_dirty)
        if (rect.Valid()) {
            if (max_x_ < min_x_) { min_x_ = rect.x0; min_y_ = rect.y0; max_x_ = rect.x1; max_y_ = rect.y1; continue; }
            min_x_ = std::min(min_x_, rect.x0); min_y_ = std::min(min_y_, rect.y0);
            max_x_ = std::max(max_x_, rect.x1); max_y_ = std::max(max_y_, rect.y1);
        }
}

float ModelRenderer::CoverageAt(int screen_x, int screen_y) const {
    int x = static_cast<int>(screen_x * kRenderScale), y = static_cast<int>(screen_y * kRenderScale);
    if (x < min_x_ || x > max_x_ || y < min_y_ || y > max_y_) return 0.f;
    return ((color_[static_cast<size_t>(y) * width_ + x] >> 24) & 0xFF) / 255.f;
}

void ModelRenderer::Paint(cairo_t* cr) {
    if (max_x_ < min_x_) return;
    int stride = width_ * 4;
    unsigned char* origin = reinterpret_cast<unsigned char*>(color_.data()) + static_cast<size_t>(min_y_) * stride + min_x_ * 4;
    cairo_surface_t* surface = cairo_image_surface_create_for_data(origin, CAIRO_FORMAT_ARGB32,
                                                                   max_x_ - min_x_ + 1, max_y_ - min_y_ + 1, stride);
    cairo_save(cr);
    cairo_scale(cr, 1.0 / kRenderScale, 1.0 / kRenderScale);
    cairo_set_source_surface(cr, surface, min_x_, min_y_);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_rectangle(cr, min_x_, min_y_, max_x_ - min_x_ + 1, max_y_ - min_y_ + 1);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_surface_destroy(surface);
}

}
