#pragma once
#include "render/camera.h"
#include <cairo.h>
#include <cstdint>
#include <string>
#include <vector>

namespace chams {

struct BoneTransform {
    float x, y, z, pad;
    float qx, qy, qz, qw;
};

struct SkinnedModel {
    std::vector<uint16_t> joints;
    std::vector<float> weights;
    std::vector<float> local;
    std::vector<float> local_normals;
    std::vector<uint32_t> indices;
    int bone_count = 0;
    size_t VertexCount() const { return weights.size() / 4; }
};

enum class Material : uint32_t { Metallic = 0, Flat = 1 };

const SkinnedModel* FindModel(const std::string& stem);

class ModelRenderer {
public:
    void Begin(int width, int height);
    void Draw(const SkinnedModel& model, const BoneTransform* bones, const render::Camera& camera,
              uint32_t rgba, Material material);
    void Paint(cairo_t* cr);

private:
    struct ScreenVertex { float x, y, inv_depth, r, g, b; bool valid; };
    struct DirtyRect {
        int x0 = 1, y0 = 1, x1 = 0, y1 = 0;
        bool Valid() const { return x1 >= x0; }
        void Add(int ax0, int ay0, int ax1, int ay1) {
            if (!Valid()) { x0 = ax0; y0 = ay0; x1 = ax1; y1 = ay1; return; }
            x0 = ax0 < x0 ? ax0 : x0; y0 = ay0 < y0 ? ay0 : y0;
            x1 = ax1 > x1 ? ax1 : x1; y1 = ay1 > y1 ? ay1 : y1;
        }
    };

    void Skin(const SkinnedModel& model, const BoneTransform* bones);
    void Shade(const render::Camera& camera, uint32_t rgba, Material material);
    void Rasterize(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c, int band_top, int band_bottom,
                   DirtyRect& dirty);
    void ClearDirty();

    int width_ = 0, height_ = 0;
    int min_x_ = 1, min_y_ = 1, max_x_ = 0, max_y_ = 0;
    float alpha_byte_ = 255.f;
    uint32_t alpha_bits_ = 0xFF000000u;
    std::vector<uint32_t> color_;
    std::vector<float> depth_;
    std::vector<float> positions_;
    std::vector<float> normals_;
    std::vector<ScreenVertex> screen_;
    std::vector<uint32_t> visible_;
};

}
