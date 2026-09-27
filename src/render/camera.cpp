#include "render/camera.h"
#include <cmath>

namespace render {

static constexpr float kDegToRad = 3.14159265358979f / 180.f;
static constexpr float kBaseAspect = 4.f / 3.f;

float Camera::Depth(const Vec3& p) const {
    return (p.x - eye.x) * forward.x + (p.y - eye.y) * forward.y + (p.z - eye.z) * forward.z;
}

bool Camera::ProjectAngles(const Vec3& p, float& sx, float& sy) const {
    Vec3 d{p.x - eye.x, p.y - eye.y, p.z - eye.z};
    float depth = d.x * forward.x + d.y * forward.y + d.z * forward.z;
    if (depth < 1.f) return false;
    float along_right = d.x * right.x + d.y * right.y + d.z * right.z;
    float along_up = d.x * up.x + d.y * up.y + d.z * up.z;
    sx = width * 0.5f * (1.f + along_right / (depth * tan_half_h));
    sy = height * 0.5f * (1.f - along_up / (depth * tan_half_v));
    return std::isfinite(sx) && std::isfinite(sy);
}

bool Camera::ProjectMatrix(const Vec3& p, float& sx, float& sy) const {
    const float* m = matrix;
    float w = m[12] * p.x + m[13] * p.y + m[14] * p.z + m[15];
    if (w < 1.f) return false;
    float x = m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3];
    float y = m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7];
    sx = width * 0.5f * (1.f + x / w);
    sy = height * 0.5f * (1.f - y / w);
    return std::isfinite(sx) && std::isfinite(sy);
}

bool Camera::Project(const Vec3& p, float& sx, float& sy) const {
    if (!valid) return false;
    return use_matrix ? ProjectMatrix(p, sx, sy) : ProjectAngles(p, sx, sy);
}

float Camera::PixelsPerUnit(const Vec3& p) const {
    float depth = Depth(p);
    if (depth < 1.f) return 0.f;
    return height * 0.5f / (depth * tan_half_v);
}

static bool MatrixAgrees(const Camera& camera) {
    const Vec3 probes[] = {
        {camera.eye.x + camera.forward.x * 800.f, camera.eye.y + camera.forward.y * 800.f, camera.eye.z + camera.forward.z * 800.f},
        {camera.eye.x + camera.forward.x * 800.f + camera.right.x * 300.f + camera.up.x * 200.f,
         camera.eye.y + camera.forward.y * 800.f + camera.right.y * 300.f + camera.up.y * 200.f,
         camera.eye.z + camera.forward.z * 800.f + camera.right.z * 300.f + camera.up.z * 200.f},
    };
    float tolerance = camera.width * 0.04f;
    for (const Vec3& probe : probes) {
        float ax, ay, mx, my;
        if (!camera.ProjectAngles(probe, ax, ay) || !camera.ProjectMatrix(probe, mx, my)) return false;
        if (std::fabs(ax - mx) > tolerance || std::fabs(ay - my) > tolerance) return false;
    }
    return true;
}

Camera ReadCamera(int width, int height, float lead_seconds) {
    Camera camera;
    camera.width = width;
    camera.height = height;
    uintptr_t pawn = game::LocalPawn();
    if (!pawn || !off::m_angEyeAngles || width <= 0 || height <= 0) return camera;

    camera.eye = game::EyePosition(pawn);
    if (lead_seconds > 0.f && off::m_vecVelocity) {
        Vec3 velocity = g_proc.Read<Vec3>(pawn + off::m_vecVelocity);
        if (std::isfinite(velocity.x) && std::isfinite(velocity.y) && std::isfinite(velocity.z)) {
            camera.eye.x += velocity.x * lead_seconds;
            camera.eye.y += velocity.y * lead_seconds;
            camera.eye.z += velocity.z * lead_seconds;
        }
    }
    camera.angles = g_proc.Read<Vec3>(pawn + off::m_angEyeAngles);
    if (!std::isfinite(camera.eye.x) || !std::isfinite(camera.angles.x) || !std::isfinite(camera.angles.y)) return camera;

    float pitch = camera.angles.x * kDegToRad, yaw = camera.angles.y * kDegToRad;
    float cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
    camera.forward = {cp * cy, cp * sy, -sp};
    camera.right = {sy, -cy, 0.f};
    camera.up = {sp * cy, sp * sy, cp};

    float fov = 90.f;
    if (off::m_pCameraServices && off::m_iFOV) {
        uintptr_t services = g_proc.Read<uintptr_t>(pawn + off::m_pCameraServices);
        int value = services ? g_proc.Read<int>(services + off::m_iFOV) : 0;
        if (value > 0 && value < 170) fov = static_cast<float>(value);
    }
    if (fov >= 80.f && off::m_bIsScoped && g_proc.Read<bool>(pawn + off::m_bIsScoped)) fov = 40.f;
    float aspect = static_cast<float>(width) / static_cast<float>(height);
    camera.tan_half_h = std::tan(fov * 0.5f * kDegToRad) * aspect / kBaseAspect;
    camera.tan_half_v = camera.tan_half_h / aspect;
    camera.valid = true;

    if (off::dwViewMatrix && off::g_ClientBase &&
        g_proc.ReadBytes(off::g_ClientBase + off::dwViewMatrix, camera.matrix, sizeof(camera.matrix))) {
        bool finite = true;
        for (float value : camera.matrix) finite = finite && std::isfinite(value);
        camera.use_matrix = finite && MatrixAgrees(camera);
    }
    return camera;
}

}
