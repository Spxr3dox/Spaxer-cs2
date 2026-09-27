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

    void Skin(const SkinnedModel& model, const BoneTransform* bones);
    void Shade(const render::Camera& camera, uint32_t rgba, Material material);
    void Rasterize(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c);
    void ClearDirty();

    int width_ = 0, height_ = 0;
    int min_x_ = 1, min_y_ = 1, max_x_ = 0, max_y_ = 0;
    float alpha_ = 1.f;
    std::vector<uint32_t> color_;
    std::vector<float> depth_;
    std::vector<float> positions_;
    std::vector<float> normals_;
    std::vector<ScreenVertex> screen_;
};

}
