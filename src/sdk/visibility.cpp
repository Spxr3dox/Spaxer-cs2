#include "sdk/visibility.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <locale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <limits.h>
#include <memory>
#include <mutex>
#include <set>
#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace vis {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kLeafSize = 4;
constexpr uint8_t kSight = static_cast<uint8_t>(Blocks::Sight);
constexpr int kMaxPenetrations = 4;
constexpr float kMaxWallThickness = 90.f;
constexpr float kDamageLostPerWall = 0.16f;

float PenetrationModifier(const char* surface) {
    static const std::pair<const char*, float> kFamilies[] = {
        {"metal_sand_barrel", 0.01f}, {"solidmetal", 0.27f}, {"wood_dense", 0.6f}, {"wood_tree", 0.6f},
        {"glass", 0.99f}, {"grate", 1.f}, {"chain", 1.f}, {"wood", 1.f}, {"plaster", 1.f}, {"sheetrock", 1.f},
        {"cardboard", 1.f}, {"paper", 1.f}, {"carpet", 1.f}, {"cloth", 1.f}, {"upholstery", 1.f},
        {"plastic", 0.75f}, {"tile", 0.6f}, {"clay", 0.6f}, {"pottery", 0.6f}, {"sand", 0.3f}, {"dirt", 0.3f},
        {"mud", 0.3f}, {"grass", 0.3f}, {"gravel", 0.4f}, {"concrete", 0.5f}, {"rock", 0.5f}, {"brick", 0.5f},
        {"asphalt", 0.5f}, {"stucco", 0.5f}, {"boulder", 0.5f}, {"no_decal", 0.5f}, {"rubber", 0.5f},
        {"computer", 0.4f}, {"metal", 0.5f},
    };
    for (const auto& [family, modifier] : kFamilies)
        if (strstr(surface, family)) return modifier;
    return 1.f;
}
constexpr auto kMapPollInterval = std::chrono::seconds(2);

struct Triangle { float v0[3], e1[3], e2[3]; };

struct Node {
    float min[3], max[3];
    uint32_t first;
    uint32_t count;
};

class Collision {
public:
    bool Load(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        char magic[4];
        uint32_t version = 0, material_count = 0, count = 0;
        bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "SMAP", 4) == 0 &&
                  fread(&version, 4, 1, f) == 1 && version == 3 && fread(&material_count, 4, 1, f) == 1;
        for (uint32_t i = 0; ok && i < material_count; i++) {
            uint8_t length = 0;
            char name[256]{};
            ok = fread(&length, 1, 1, f) == 1 && fread(name, 1, length, f) == length;
            modifiers_.push_back(PenetrationModifier(name));
        }
        ok = ok && fread(&count, 4, 1, f) == 1;
        std::vector<std::array<float, 9>> raw(ok ? count : 0);
        std::vector<uint8_t> raw_materials(ok ? count : 0);
        std::vector<uint8_t> raw_flags(ok ? count : 0);
        ok = ok && fread(raw.data(), sizeof(float) * 9, count, f) == count &&
             fread(raw_materials.data(), 1, count, f) == count && fread(raw_flags.data(), 1, count, f) == count;
        fclose(f);
        if (!ok || raw.empty()) return false;

        triangles_.resize(raw.size());
        std::vector<std::array<float, 6>> bounds(raw.size());
        std::vector<std::array<float, 3>> centers(raw.size());
        for (size_t i = 0; i < raw.size(); i++) {
            const auto& t = raw[i];
            for (int a = 0; a < 3; a++) {
                triangles_[i].v0[a] = t[a];
                triangles_[i].e1[a] = t[3 + a] - t[a];
                triangles_[i].e2[a] = t[6 + a] - t[a];
                bounds[i][a] = std::min({t[a], t[3 + a], t[6 + a]});
                bounds[i][3 + a] = std::max({t[a], t[3 + a], t[6 + a]});
                centers[i][a] = (bounds[i][a] + bounds[i][3 + a]) * 0.5f;
            }
        }
        order_.resize(raw.size());
        for (uint32_t i = 0; i < order_.size(); i++) order_[i] = i;
        nodes_.reserve(raw.size() / 2 + 1);
        Build(0, static_cast<uint32_t>(order_.size()), bounds, centers);

        std::vector<Triangle> sorted(triangles_.size());
        materials_.resize(triangles_.size());
        flags_.resize(triangles_.size());
        for (size_t i = 0; i < order_.size(); i++) {
            sorted[i] = triangles_[order_[i]];
            flags_[i] = raw_flags[order_[i]];
            materials_[i] = raw_materials[order_[i]] < modifiers_.size() ? raw_materials[order_[i]] : 0;
        }
        triangles_ = std::move(sorted);
        order_.clear();
        order_.shrink_to_fit();
        return true;
    }

    bool Blocked(const Vec3& from, const Vec3& to) const {
        bool blocked = false;
        Traverse(from, to, kSight, [&](uint32_t, float, bool) { blocked = true; return false; });
        return blocked;
    }

    bool Nearest(const Vec3& from, const Vec3& to, uint8_t mask, float& fraction, Vec3* normal) const {
        fraction = 2.f;
        uint32_t nearest = 0;
        Traverse(from, to, mask, [&](uint32_t index, float t, bool) {
            if (t < fraction) {
                fraction = t;
                nearest = index;
            }
            return true;
        });
        if (fraction > 1.f) return false;
        if (normal) {
            const Triangle& tri = triangles_[nearest];
            Vec3 n{tri.e1[1] * tri.e2[2] - tri.e1[2] * tri.e2[1], tri.e1[2] * tri.e2[0] - tri.e1[0] * tri.e2[2],
                   tri.e1[0] * tri.e2[1] - tri.e1[1] * tri.e2[0]};
            float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (length > 0.f) n = {n.x / length, n.y / length, n.z / length};
            Vec3 dir{to.x - from.x, to.y - from.y, to.z - from.z};
            if (n.x * dir.x + n.y * dir.y + n.z * dir.z > 0.f) n = {-n.x, -n.y, -n.z};
            *normal = n;
        }
        return true;
    }

    float Damage(const Vec3& from, const Vec3& to, const Ballistics& weapon) const {
        struct Hit { float t; bool entering; uint8_t material; };
        std::vector<Hit> hits;
        Traverse(from, to, kSight, [&](uint32_t index, float t, bool entering) {
            hits.push_back({t, entering, materials_[index]});
            return true;
        });
        std::sort(hits.begin(), hits.end(), [](const Hit& l, const Hit& r) { return l.t < r.t; });

        float distance = std::sqrt((to.x - from.x) * (to.x - from.x) + (to.y - from.y) * (to.y - from.y) +
                                   (to.z - from.z) * (to.z - from.z));
        float damage = weapon.damage * std::pow(weapon.range_modifier, distance / 500.f);
        int depth = 0, penetrations = 0;
        float entered_at = 0.f;
        uint8_t entered_material = 0;
        for (const Hit& hit : hits) {
            if (hit.entering) {
                if (depth++ == 0) {
                    entered_at = hit.t;
                    entered_material = hit.material;
                }
                continue;
            }
            if (depth == 0 || --depth > 0) continue;
            float thickness = (hit.t - entered_at) * distance;
            if (++penetrations > kMaxPenetrations || thickness > kMaxWallThickness) return 0.f;
            float combined = (modifiers_[entered_material] + modifiers_[hit.material]) * 0.5f;
            float modifier = combined > 0.f ? 1.f / combined : 100.f;
            float lost = damage * kDamageLostPerWall + std::max(0.f, 3.f / weapon.penetration * 1.25f) * modifier * 3.f +
                         modifier * thickness * thickness / 24.f;
            damage -= std::max(0.f, lost);
            if (damage < 1.f) return 0.f;
        }
        return depth > 0 ? 0.f : damage;
    }

private:
    uint32_t Build(uint32_t begin, uint32_t end, const std::vector<std::array<float, 6>>& bounds,
                   const std::vector<std::array<float, 3>>& centers) {
        uint32_t index = static_cast<uint32_t>(nodes_.size());
        nodes_.push_back({});
        Node node{};
        for (int a = 0; a < 3; a++) { node.min[a] = 1e30f; node.max[a] = -1e30f; }
        float center_min[3] = {1e30f, 1e30f, 1e30f}, center_max[3] = {-1e30f, -1e30f, -1e30f};
        for (uint32_t i = begin; i < end; i++) {
            const auto& b = bounds[order_[i]];
            const auto& c = centers[order_[i]];
            for (int a = 0; a < 3; a++) {
                node.min[a] = std::min(node.min[a], b[a]);
                node.max[a] = std::max(node.max[a], b[3 + a]);
                center_min[a] = std::min(center_min[a], c[a]);
                center_max[a] = std::max(center_max[a], c[a]);
            }
        }
        if (end - begin <= kLeafSize) {
            node.first = begin;
            node.count = end - begin;
            nodes_[index] = node;
            return index;
        }
        int axis = 0;
        for (int a = 1; a < 3; a++)
            if (center_max[a] - center_min[a] > center_max[axis] - center_min[axis]) axis = a;
        uint32_t middle = begin + (end - begin) / 2;
        std::nth_element(order_.begin() + begin, order_.begin() + middle, order_.begin() + end,
                         [&](uint32_t l, uint32_t r) { return centers[l][axis] < centers[r][axis]; });
        Build(begin, middle, bounds, centers);
        node.first = Build(middle, end, bounds, centers);
        node.count = 0;
        nodes_[index] = node;
        return index;
    }

    static bool HitsBox(const Node& node, const float* origin, const float* inv) {
        float t_min = 0.f, t_max = 1.f;
        for (int a = 0; a < 3; a++) {
            float t0 = (node.min[a] - origin[a]) * inv[a];
            float t1 = (node.max[a] - origin[a]) * inv[a];
            if (t0 > t1) std::swap(t0, t1);
            t_min = std::max(t_min, t0);
            t_max = std::min(t_max, t1);
            if (t_min > t_max) return false;
        }
        return true;
    }

    template <typename Visitor>
    void Traverse(const Vec3& from, const Vec3& to, uint8_t mask, Visitor&& visit) const {
        float origin[3] = {from.x, from.y, from.z};
        float dir[3] = {to.x - from.x, to.y - from.y, to.z - from.z};
        float inv[3];
        for (int a = 0; a < 3; a++) inv[a] = dir[a] != 0.f ? 1.f / dir[a] : 1e30f;
        uint32_t stack[64];
        int top = 0;
        stack[top++] = 0;
        while (top) {
            const Node& node = nodes_[stack[--top]];
            if (!HitsBox(node, origin, inv)) continue;
            if (node.count) {
                for (uint32_t i = node.first; i < node.first + node.count; i++) {
                    if (!(flags_[i] & mask)) continue;
                    float t;
                    bool entering;
                    if (HitsTriangle(triangles_[i], origin, dir, t, entering) && !visit(i, t, entering)) return;
                }
                continue;
            }
            uint32_t self = static_cast<uint32_t>(&node - nodes_.data());
            if (top + 2 > 64) return;
            stack[top++] = node.first;
            stack[top++] = self + 1;
        }
    }

    static bool HitsTriangle(const Triangle& tri, const float* origin, const float* dir, float& t, bool& entering) {
        float p[3] = {dir[1] * tri.e2[2] - dir[2] * tri.e2[1], dir[2] * tri.e2[0] - dir[0] * tri.e2[2],
                      dir[0] * tri.e2[1] - dir[1] * tri.e2[0]};
        float det = tri.e1[0] * p[0] + tri.e1[1] * p[1] + tri.e1[2] * p[2];
        if (std::fabs(det) < 1e-9f) return false;
        float inv_det = 1.f / det;
        float s[3] = {origin[0] - tri.v0[0], origin[1] - tri.v0[1], origin[2] - tri.v0[2]};
        float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv_det;
        if (u < 0.f || u > 1.f) return false;
        float q[3] = {s[1] * tri.e1[2] - s[2] * tri.e1[1], s[2] * tri.e1[0] - s[0] * tri.e1[2],
                      s[0] * tri.e1[1] - s[1] * tri.e1[0]};
        float v = (dir[0] * q[0] + dir[1] * q[1] + dir[2] * q[2]) * inv_det;
        if (v < 0.f || u + v > 1.f) return false;
        t = (tri.e2[0] * q[0] + tri.e2[1] * q[1] + tri.e2[2] * q[2]) * inv_det;
        entering = det > 0.f;
        return t > 1e-4f && t < 0.999f;
    }

    std::vector<Triangle> triangles_;
    std::vector<uint8_t> materials_;
    std::vector<uint8_t> flags_;
    std::vector<float> modifiers_;
    std::vector<Node> nodes_;
    std::vector<uint32_t> order_;
};

std::mutex s_mutex;
std::shared_ptr<const Collision> s_collision;
std::string s_map;
std::string s_loaded_map;
std::set<std::string> s_requested;
bool s_has_sun = false;
Vec3 s_sun{};
Clock::time_point s_next_poll{};

std::string MapsDir() {
    const char* home = getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.config/spaxer/maps/";
}

bool LoadSun(const std::string& path, Vec3& sun) {
    std::ifstream file(path);
    file.imbue(std::locale::classic());
    float pitch = 0.f, yaw = 0.f;
    if (!(file >> pitch >> yaw)) return false;
    constexpr float kDegToRad = 3.14159265f / 180.f;
    float cp = std::cos(pitch * kDegToRad);
    sun = {-cp * std::cos(yaw * kDegToRad), -cp * std::sin(yaw * kDegToRad), std::sin(pitch * kDegToRad)};
    return true;
}

std::string DetectMap(int pid) {
    char fd_dir[64];
    snprintf(fd_dir, sizeof(fd_dir), "/proc/%d/fd", pid);
    DIR* dir = opendir(fd_dir);
    if (!dir) return {};
    std::string found;
    while (dirent* entry = readdir(dir)) {
        char link[320], target[PATH_MAX];
        snprintf(link, sizeof(link), "%s/%s", fd_dir, entry->d_name);
        ssize_t len = readlink(link, target, sizeof(target) - 1);
        if (len <= 0) continue;
        target[len] = 0;
        const char* maps = strstr(target, "/csgo/maps/");
        if (!maps) continue;
        std::string name = maps + strlen("/csgo/maps/");
        if (name.find('/') != std::string::npos || name.size() < 5 || name.compare(name.size() - 4, 4, ".vpk") != 0) continue;
        name.resize(name.size() - 4);
        if (name == "graphics_settings" || name.ends_with("_vanity")) continue;
        found = name;
        break;
    }
    closedir(dir);
    return found;
}

void RequestConversion(const std::string& map) {
    if (!s_requested.insert(map).second) return;
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0) return;
    exe[len] = 0;
    std::string script = std::string(exe, strrchr(exe, '/')) + "/tools/map_collision.py";
    struct stat st;
    if (stat(script.c_str(), &st) != 0) return;
    char* argv[] = {const_cast<char*>("python3"), script.data(), const_cast<char*>(map.c_str()), nullptr};
    pid_t child;
    if (posix_spawnp(&child, "python3", nullptr, nullptr, argv, environ) == 0)
        fprintf(stderr, "[vis] converting collision for %s\n", map.c_str());
}

}

void Update() {
    auto now = Clock::now();
    if (now < s_next_poll) return;
    s_next_poll = now + kMapPollInterval;

    std::string map = g_proc.pid() > 0 ? DetectMap(g_proc.pid()) : std::string{};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_map = map;
        if (map != s_loaded_map) {
            s_collision.reset();
            s_loaded_map.clear();
        }
    }
    if (map.empty() || map == s_loaded_map) return;

    std::string path = MapsDir() + map + ".smap";
    auto collision = std::make_shared<Collision>();
    if (!collision->Load(path)) {
        RequestConversion(map);
        return;
    }
    Vec3 sun;
    bool has_sun = LoadSun(MapsDir() + map + ".sun", sun);
    std::lock_guard<std::mutex> lock(s_mutex);
    s_collision = std::move(collision);
    s_loaded_map = map;
    s_has_sun = has_sun;
    s_sun = sun;
    fprintf(stderr, "[vis] loaded collision for %s\n", map.c_str());
}

bool SunDirection(Vec3& out) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_collision || !s_has_sun) return false;
    out = s_sun;
    return true;
}

bool Ready() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_collision != nullptr;
}

std::string CurrentMap() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_map;
}

bool LineOfSight(const Vec3& from, const Vec3& to) {
    std::shared_ptr<const Collision> collision;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        collision = s_collision;
    }
    return !collision || !collision->Blocked(from, to);
}

}

namespace vis {

bool WeaponBallistics(int definition_index, Ballistics& out) {
    switch (definition_index) {
        case 1: out = {53.f, 2.f, 0.81f}; return true;
        case 2: out = {38.f, 1.f, 0.75f}; return true;
        case 3: out = {32.f, 1.f, 0.81f}; return true;
        case 4: out = {30.f, 1.f, 0.85f}; return true;
        case 7: out = {36.f, 2.f, 0.98f}; return true;
        case 8: out = {28.f, 2.f, 0.98f}; return true;
        case 9: out = {115.f, 2.5f, 0.99f}; return true;
        case 10: out = {30.f, 2.f, 0.96f}; return true;
        case 11: out = {80.f, 2.5f, 0.98f}; return true;
        case 13: out = {30.f, 2.f, 0.98f}; return true;
        case 14: out = {32.f, 2.f, 0.97f}; return true;
        case 16: out = {33.f, 2.f, 0.97f}; return true;
        case 17: out = {29.f, 1.f, 0.80f}; return true;
        case 19: out = {26.f, 1.f, 0.86f}; return true;
        case 23: out = {27.f, 1.f, 0.85f}; return true;
        case 24: out = {35.f, 1.f, 0.85f}; return true;
        case 26: out = {27.f, 1.f, 0.80f}; return true;
        case 28: out = {35.f, 2.f, 0.97f}; return true;
        case 30: out = {33.f, 1.f, 0.83f}; return true;
        case 32: out = {35.f, 1.f, 0.91f}; return true;
        case 33: out = {29.f, 1.f, 0.85f}; return true;
        case 34: out = {26.f, 1.f, 0.87f}; return true;
        case 36: out = {38.f, 1.f, 0.85f}; return true;
        case 38: out = {80.f, 2.5f, 0.98f}; return true;
        case 39: out = {30.f, 2.f, 1.00f}; return true;
        case 40: out = {88.f, 2.5f, 0.98f}; return true;
        case 60: out = {38.f, 2.f, 0.94f}; return true;
        case 61: out = {35.f, 1.f, 0.91f}; return true;
        case 63: out = {31.f, 1.f, 0.85f}; return true;
        case 64: out = {86.f, 2.f, 0.94f}; return true;
        default: return false;
    }
}

float DamageAt(const Vec3& from, const Vec3& to, const Ballistics& weapon) {
    std::shared_ptr<const Collision> collision;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        collision = s_collision;
    }
    return collision ? collision->Damage(from, to, weapon) : 0.f;
}

}

namespace vis {

bool Raycast(const Vec3& from, const Vec3& to, Vec3& hit, Vec3* normal, Blocks blocks) {
    std::shared_ptr<const Collision> collision;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        collision = s_collision;
    }
    float fraction;
    if (!collision || !collision->Nearest(from, to, static_cast<uint8_t>(blocks), fraction, normal)) return false;
    hit = {from.x + (to.x - from.x) * fraction, from.y + (to.y - from.y) * fraction, from.z + (to.z - from.z) * fraction};
    return true;
}

}

namespace vis {

bool CastBatch(const Vec3& origin, const Vec3* directions, size_t count, float length, uint8_t* blocked) {
    std::shared_ptr<const Collision> collision;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        collision = s_collision;
    }
    if (!collision) return false;
    for (size_t i = 0; i < count; i++) {
        Vec3 end{origin.x + directions[i].x * length, origin.y + directions[i].y * length, origin.z + directions[i].z * length};
        blocked[i] = collision->Blocked(origin, end) ? 1 : 0;
    }
    return true;
}

}
