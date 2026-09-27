#include "dumper.h"
#include "offsets.h"
#include "memory/process.h"
#include "sdk/game.h"
#include <cstdio>
#include <string_view>
#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <string>

namespace dumper {

static FILE* g_log = nullptr;
static void LogInit() {
    if (g_log) return;
    g_log = fopen("/tmp/spaxer.log", "w");
    if (g_log) setvbuf(g_log, nullptr, _IOLBF, 0);
}
static void Log(const char* fmt, ...) {
    LogInit();
    if (!g_log) return;
    va_list a; va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fflush(g_log);
}

static uintptr_t GetEntityFromList(uintptr_t entityList, int index) {
    if (index <= 0 || !entityList) return 0;
    int chunk = index >> 9;
    int entry_idx = index & 0x1FF;
    uintptr_t list_entry = g_proc.Read<uintptr_t>(entityList + 8 * chunk + 0x10);
    if (!list_entry) return 0;
    return g_proc.Read<uintptr_t>(list_entry + 0x70 * entry_idx);
}

struct MapRegion { uintptr_t start; uintptr_t end; };

static std::vector<MapRegion> GetRWRegionsWithBSS(const char* libname) {
    std::vector<MapRegion> regions;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", g_proc.pid());
    FILE* f = fopen(path, "r");
    if (!f) return regions;
    char line[1024];
    uintptr_t lastLibEnd = 0;
    bool foundLib = false;
    while (fgets(line, sizeof(line), f)) {
        uintptr_t start, end;
        if (sscanf(line, "%lx-%lx", &start, &end) != 2) continue;
        bool isRW = strstr(line, "rw-") != nullptr;
        bool hasLib = strstr(line, libname) != nullptr;
        if (hasLib && isRW) {
            regions.push_back({start, end});
            lastLibEnd = end;
            foundLib = true;
        } else if (foundLib && isRW && start == lastLibEnd && !strchr(line, '/')) {
            regions.push_back({start, end});
            lastLibEnd = end;
        } else if (foundLib && start != lastLibEnd) {
            foundLib = false;
        }
    }
    fclose(f);
    return regions;
}

static std::vector<MapRegion> GetReadOnlyRegions(const char* libname) {
    std::vector<MapRegion> regions;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", g_proc.pid());
    FILE* f = fopen(path, "r");
    if (!f) return regions;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        uintptr_t start, end;
        char perms[8]{};
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) continue;
        if (perms[0] == 'r' && perms[1] != 'w' && strstr(line, libname)) regions.push_back({start, end});
    }
    fclose(f);
    return regions;
}

static bool TryVerifyEntityList(uintptr_t val) {
    uintptr_t chunk0 = g_proc.Read<uintptr_t>(val + 0x10);
    if (chunk0 < 0x10000 || chunk0 > 0x7FFFFFFFFFFF) return false;

    int validPtrs = 0;
    for (int ei = 1; ei <= 64; ei++) {
        uintptr_t ent = g_proc.Read<uintptr_t>(chunk0 + 0x70 * ei);
        if (ent > 0x10000 && ent < 0x7FFFFFFFFFFF) validPtrs++;
    }
    if (validPtrs < 2) return false;

    for (int ei = 1; ei <= 64; ei++) {
        uintptr_t ctrl = g_proc.Read<uintptr_t>(chunk0 + 0x70 * ei);
        if (ctrl < 0x10000 || ctrl > 0x7FFFFFFFFFFF) continue;
        int team = game::Team(ctrl);
        if (team != 2 && team != 3) continue;
        uint32_t pawnH = g_proc.Read<uint32_t>(ctrl + off::m_hPlayerPawn);
        if (pawnH == 0 || pawnH == 0xFFFFFFFF) continue;
        int pawnIdx = pawnH & 0x7FFF;
        if (pawnIdx <= 0 || pawnIdx > 0x4000) continue;
        uintptr_t pawn = GetEntityFromList(val, pawnIdx);
        if (pawn < 0x10000 || pawn > 0x7FFFFFFFFFFF) continue;
        int hp = g_proc.Read<int>(pawn + off::m_iHealth);
        int pawnTeam = game::Team(pawn);
        if (hp >= 1 && hp <= 100 && (pawnTeam == 2 || pawnTeam == 3)) return true;
    }

    int validPawns = 0;
    for (int ei = 1; ei <= 256; ei++) {
        uintptr_t ent = GetEntityFromList(val, ei);
        if (ent < 0x10000 || ent > 0x7FFFFFFFFFFF) continue;
        int hp = g_proc.Read<int>(ent + off::m_iHealth);
        int team = game::Team(ent);
        if (hp >= 1 && hp <= 100 && (team == 2 || team == 3)) {
            if (++validPawns >= 3) return true;
        }
    }
    return false;
}

static bool ScanForEntityList() {
    const char* libs[] = {"libclient.so", "libengine2.so", nullptr};
    char mempath[64];
    snprintf(mempath, sizeof(mempath), "/proc/%d/mem", g_proc.pid());

    for (int li = 0; libs[li]; li++) {
        auto regions = GetRWRegionsWithBSS(libs[li]);
        Log("[dumper] %s rw- regions: %zu\n", libs[li], regions.size());
        for (auto& r : regions) Log("[dumper]   %lx-%lx (%lx bytes)\n", r.start, r.end, r.end - r.start);

        int memFd = open(mempath, O_RDONLY);
        if (memFd < 0) continue;

        for (auto& r : regions) {
            constexpr size_t CHUNK = 4096;
            uint8_t buf[CHUNK];
            for (uintptr_t addr = r.start; addr < r.end; addr += CHUNK) {
                size_t toRead = r.end - addr;
                if (toRead > CHUNK) toRead = CHUNK;
                ssize_t n = pread(memFd, buf, toRead, (off_t)addr);
                if (n < (ssize_t)sizeof(uintptr_t)) continue;

                for (size_t i = 0; i + 8 <= (size_t)n; i += 8) {
                    uintptr_t val;
                    memcpy(&val, buf + i, 8);
                    if (val < 0x10000 || val > 0x7FFFFFFFFFFF) continue;
                    uintptr_t chunk0 = g_proc.Read<uintptr_t>(val + 0x10);
                    if (chunk0 < 0x10000 || chunk0 > 0x7FFFFFFFFFFF) continue;
                    uintptr_t ent1 = g_proc.Read<uintptr_t>(chunk0 + 0x70);
                    if (ent1 < 0x10000 || ent1 > 0x7FFFFFFFFFFF) continue;
                    if (TryVerifyEntityList(val)) {
                        off::g_EntityListPtr = val;
                        Log("[dumper] entity list @ 0x%lX (ref *0x%lX)\n", val, addr + i);
                        close(memFd);
                        return true;
                    }
                }
            }
        }
        close(memFd);
    }
    return false;
}

static bool LooksLikeWorldPos(float x, float y, float z) {
    auto valid = [](float v){ return v >= -20000.f && v <= 20000.f && !(v != v); };
    if (!valid(x) || !valid(y) || !valid(z)) return false;
    float mag = (x < 0 ? -x : x) + (y < 0 ? -y : y);
    if (mag < 10.f) return false;
    return true;
}

static bool ProbePawnLayoutMulti() {
    if (off::m_pGameSceneNode && off::m_vecAbsOrigin) return true;
    if (!off::g_EntityListPtr) return false;

    off::m_vecAbsOrigin = 0xC8;
    std::vector<uintptr_t> pawns;
    for (int i = 1; i <= 64 && pawns.size() < 8; i++) {
        uintptr_t ctrl = GetEntityFromList(off::g_EntityListPtr, i);
        if (!ctrl) continue;
        uint32_t ph = g_proc.Read<uint32_t>(ctrl + off::m_hPlayerPawn);
        if (!ph || ph == 0xFFFFFFFF) continue;
        uintptr_t pawn = GetEntityFromList(off::g_EntityListPtr, ph & 0x7FFF);
        if (!pawn) continue;
        pawns.push_back(pawn);
    }
    if (pawns.size() < 2) { Log("[dumper] probe: only %zu pawns\n", pawns.size()); return false; }

    static const uintptr_t kOriginCands[] = {0xC8, 0xD0, 0xCC, 0xB8, 0xC0, 0xE0, 0xE8, 0xF0};
    int best_off = 0, best_hits = 0;
    uintptr_t best_origin = 0xC8;
    for (uintptr_t off_p = 0x100; off_p <= 0xA00; off_p += 8) {
        for (uintptr_t oc : kOriginCands) {
            int hits = 0;
            for (uintptr_t pawn : pawns) {
                uintptr_t sn = g_proc.Read<uintptr_t>(pawn + off_p);
                if (sn < 0x10000 || sn > 0x7FFFFFFFFFFF) continue;
                float x = g_proc.Read<float>(sn + oc);
                float y = g_proc.Read<float>(sn + oc + 4);
                float z = g_proc.Read<float>(sn + oc + 8);
                if (!LooksLikeWorldPos(x, y, z)) continue;
                hits++;
            }
            if (hits > best_hits) { best_hits = hits; best_off = off_p; best_origin = oc; }
        }
    }
    off::m_vecAbsOrigin = best_origin;
    if (best_hits >= (int)pawns.size() / 2 && best_hits >= 2) {
        off::m_pGameSceneNode = best_off;
        Log("[dumper] probed m_pGameSceneNode=0x%X origin=0x%lX (matched %d/%zu pawns)\n",
            best_off, off::m_vecAbsOrigin, best_hits, pawns.size());
        return true;
    }
    Log("[dumper] probe failed: best offset 0x%X hit %d/%zu\n", best_off, best_hits, pawns.size());
    return false;
}

struct WorldPt { float x, y, z; };
struct CameraSample { WorldPt eye; WorldPt forward; };

static bool GetCameraSample(int local_idx, CameraSample& sample) {
    if (!off::g_EntityListPtr || !off::m_pGameSceneNode || !off::m_vecAbsOrigin || !off::m_angEyeAngles)
        return false;
    uintptr_t controller = GetEntityFromList(off::g_EntityListPtr, local_idx);
    if (!controller) return false;
    uint32_t pawnHandle = g_proc.Read<uint32_t>(controller + off::m_hPlayerPawn);
    if (!pawnHandle || pawnHandle == 0xFFFFFFFF) return false;
    uintptr_t pawn = GetEntityFromList(off::g_EntityListPtr, pawnHandle & 0x7FFF);
    if (!pawn) return false;
    int hp = g_proc.Read<int>(pawn + off::m_iHealth);
    if (hp <= 0 || hp > 100) return false;
    uintptr_t sceneNode = g_proc.Read<uintptr_t>(pawn + off::m_pGameSceneNode);
    if (!sceneNode) return false;
    WorldPt origin = g_proc.Read<WorldPt>(sceneNode + off::m_vecAbsOrigin);
    WorldPt angles = g_proc.Read<WorldPt>(pawn + off::m_angEyeAngles);
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
        !std::isfinite(angles.x) || !std::isfinite(angles.y)) return false;
    constexpr float radians = 3.14159265358979323846f / 180.f;
    float pitch = angles.x * radians;
    float yaw = angles.y * radians;
    float cp = cosf(pitch);
    sample.eye = {origin.x, origin.y, origin.z + 64.f};
    sample.forward = {cp * cosf(yaw), cp * sinf(yaw), -sinf(pitch)};
    return true;
}

[[maybe_unused]] static bool MatrixMatchesCamera(const float* m, const CameraSample& sample) {
    for (int i = 0; i < 16; i++)
        if (!std::isfinite(m[i]) || fabsf(m[i]) > 1e5f) return false;
    if (fabsf(m[12] - sample.forward.x) > 0.60f ||
        fabsf(m[13] - sample.forward.y) > 0.60f ||
        fabsf(m[14] - sample.forward.z) > 0.60f) return false;
    float expected_w = -(m[12] * sample.eye.x + m[13] * sample.eye.y + m[14] * sample.eye.z);
    if (fabsf(m[15] - expected_w) > 128.f) return false;
    if (fabsf(m[0]) + fabsf(m[1]) + fabsf(m[2]) < 1e-4f) return false;
    if (fabsf(m[4]) + fabsf(m[5]) + fabsf(m[6]) < 1e-4f) return false;
    return true;
}

static std::vector<WorldPt> CollectVisibleOrigins(int local_idx) {
    std::vector<WorldPt> pts;
    if (!off::g_EntityListPtr || !off::m_pGameSceneNode) return pts;
    int ctrls = 0, valid_pawns = 0, alive = 0, with_origin = 0;
    for (int i = 1; i <= 64 && pts.size() < 8; i++) {
        uintptr_t ctrl = GetEntityFromList(off::g_EntityListPtr, i);
        if (!ctrl) continue;
        ctrls++;
        if (i == local_idx) continue;
        uint32_t ph = g_proc.Read<uint32_t>(ctrl + off::m_hPlayerPawn);
        if (!ph || ph == 0xFFFFFFFF) continue;
        uintptr_t pawn = GetEntityFromList(off::g_EntityListPtr, ph & 0x7FFF);
        if (!pawn) continue;
        valid_pawns++;
        int hp = g_proc.Read<int>(pawn + off::m_iHealth);
        if (hp <= 0 || hp > 100) continue;
        alive++;
        uintptr_t sn = g_proc.Read<uintptr_t>(pawn + off::m_pGameSceneNode);
        if (!sn) continue;
        WorldPt origin = g_proc.Read<WorldPt>(sn + off::m_vecAbsOrigin);
        if (!LooksLikeWorldPos(origin.x, origin.y, origin.z)) continue;
        with_origin++;
        pts.push_back(origin);
    }

    if (pts.size() < 3 && local_idx > 0) {
        uintptr_t ctrl = GetEntityFromList(off::g_EntityListPtr, local_idx);
        if (ctrl) {
            uint32_t ph = g_proc.Read<uint32_t>(ctrl + off::m_hPlayerPawn);
            if (ph && ph != 0xFFFFFFFF) {
                uintptr_t pawn = GetEntityFromList(off::g_EntityListPtr, ph & 0x7FFF);
                if (pawn) {
                    uintptr_t sn = g_proc.Read<uintptr_t>(pawn + off::m_pGameSceneNode);
                    if (sn) {
                        WorldPt o = g_proc.Read<WorldPt>(sn + off::m_vecAbsOrigin);
                        if (LooksLikeWorldPos(o.x, o.y, o.z)) {
                            const float dxs[4] = {200.f, -200.f, 0.f, 0.f};
                            const float dys[4] = {0.f, 0.f, 200.f, -200.f};
                            for (int k = 0; k < 4 && pts.size() < 8; k++)
                                pts.push_back({o.x + dxs[k], o.y + dys[k], o.z});
                        }
                    }
                }
            }
        }
    }

    static int loglimit = 0;
    if (loglimit++ < 3)
        Log("[dumper] collect: ctrls=%d pawns=%d alive=%d origin=%d total_pts=%zu\n",
            ctrls, valid_pawns, alive, with_origin, pts.size());
    return pts;
}

static bool ScanForViewMatrix(int local_idx) {
    if (!off::g_ClientBase) return false;
    CameraSample camera{};
    if (!GetCameraSample(local_idx, camera)) {
        Log("[dumper] camera sample unavailable\n");
        return false;
    }
    Log("[dumper] camera eye %.1f %.1f %.1f forward %.3f %.3f %.3f\n",
        camera.eye.x, camera.eye.y, camera.eye.z,
        camera.forward.x, camera.forward.y, camera.forward.z);
    auto pts = CollectVisibleOrigins(local_idx);
    if (pts.size() < 3) { Log("[dumper] view matrix scan: need 3+ bot origins, have %zu\n", pts.size()); return false; }
    Log("[dumper] view matrix scan with %zu bot origins\n", pts.size());
    for (auto& p : pts) Log("  pt %.1f %.1f %.1f\n", p.x, p.y, p.z);

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", g_proc.pid());
    FILE* f = fopen(path, "r");
    if (!f) return false;
    std::vector<std::pair<uintptr_t, uintptr_t>> regions;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        uintptr_t s, e;
        if (sscanf(line, "%lx-%lx", &s, &e) != 2) continue;
        bool hasLib = strstr(line, "libclient.so") != nullptr;
        bool isRw   = strstr(line, "rw-") != nullptr;
        if (hasLib && isRw) regions.push_back({s, e});
    }
    fclose(f);

    char mempath[64];
    snprintf(mempath, sizeof(mempath), "/proc/%d/mem", g_proc.pid());
    int fd = open(mempath, O_RDONLY);
    if (fd < 0) return false;

    for (auto& r : regions) {
        constexpr size_t CHUNK = 65536;
        uint8_t buf[CHUNK];
        for (uintptr_t addr = r.first; addr < r.second; addr += CHUNK) {
            size_t toRead = r.second - addr;
            if (toRead > CHUNK) toRead = CHUNK;
            ssize_t n = pread(fd, buf, toRead, (off_t)addr);
            if (n < 64) continue;
            for (size_t i = 0; i + 64 <= (size_t)n; i += 4) {
                float m[16];
                memcpy(m, buf + i, 64);
                bool bad = false;
                for (int k = 0; k < 16; k++)
                    if (m[k] != m[k] || m[k] > 1e10f || m[k] < -1e10f) { bad = true; break; }
                if (bad) continue;

                int passing = 0;
                float first_sx = 999.f, first_sy = 999.f;
                bool varied = false;
                bool head_above_feet = true;
                for (auto& p : pts) {
                    float w = m[12]*p.x + m[13]*p.y + m[14]*p.z + m[15];
                    if (w < 30.f || w > 1e5f) { head_above_feet = false; continue; }
                    float x = m[0]*p.x + m[1]*p.y + m[2]*p.z + m[3];
                    float y = m[4]*p.x + m[5]*p.y + m[6]*p.z + m[7];
                    float inv = 1.f / w;
                    float sx = 0.5f * (1.f + x * inv);
                    float sy = 0.5f * (1.f - y * inv);
                    if (sx < -0.05f || sx > 1.05f || sy < -0.05f || sy > 1.05f) { head_above_feet = false; continue; }

                    float hz = p.z + 72.f;
                    float wh = m[12]*p.x + m[13]*p.y + m[14]*hz + m[15];
                    float yh = m[4]*p.x + m[5]*p.y + m[6]*hz + m[7];
                    if (wh <= 0.f) { head_above_feet = false; continue; }
                    float sy_head = 0.5f * (1.f - yh / wh);
                    if (sy_head >= sy) head_above_feet = false;

                    if (first_sx == 999.f) { first_sx = sx; first_sy = sy; }
                    else if (!varied) {
                        if (fabsf(sx - first_sx) > 0.05f || fabsf(sy - first_sy) > 0.05f)
                            varied = true;
                    }
                    passing++;
                }
                if (head_above_feet && varied && passing >= (int)pts.size() - 1 && passing >= 3) {
                    off::dwViewMatrix = (addr + i) - off::g_ClientBase;
                    Log("[dumper] dwViewMatrix=0x%lX (abs=0x%lX) matched %d/%zu\n",
                        off::dwViewMatrix, addr + i, passing, pts.size());
                    close(fd);
                    return true;
                }
            }
        }
    }
    close(fd);
    Log("[dumper] view matrix scan failed\n");
    return false;
}

static const char* CachePath() {
    static char path[512];
    if (path[0]) return path;
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(path, sizeof(path), "%s/.config/spaxer/offsets.cache", home);
    return path;
}

static void SaveCache() {
    FILE* f = fopen(CachePath(), "w");
    if (!f) return;
    fprintf(f, "m_hPlayerPawn=0x%lX\n", off::m_hPlayerPawn);
    fprintf(f, "m_pGameSceneNode=0x%lX\n", off::m_pGameSceneNode);
    fprintf(f, "m_vecAbsOrigin=0x%lX\n", off::m_vecAbsOrigin);
    fprintf(f, "m_angEyeAngles=0x%lX\n", off::m_angEyeAngles);
    fprintf(f, "m_iHealth=0x%lX\n", off::m_iHealth);
    fprintf(f, "m_iTeamNum=0x%lX\n", off::m_iTeamNum);
    fprintf(f, "m_fFlags=0x%lX\n", off::m_fFlags);
    fprintf(f, "m_bDidSmokeEffect=0x%lX\n", off::m_bDidSmokeEffect);
    fprintf(f, "m_vSmokeColor=0x%lX\n", off::m_vSmokeColor);
    fprintf(f, "m_iPing=0x%lX\n", off::m_iPing);
    fclose(f);
}

static void LoadCache() {
    FILE* f = fopen(CachePath(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64] = {0};
        unsigned long v = 0;
        if (sscanf(line, "%63[^=]=0x%lx", key, &v) == 2) {
            std::string k(key);
            if      (k == "m_hPlayerPawn")    off::m_hPlayerPawn = v;
            else if (k == "m_pGameSceneNode") off::m_pGameSceneNode = v;
            else if (k == "m_vecAbsOrigin")   off::m_vecAbsOrigin = v;
            else if (k == "m_angEyeAngles")   off::m_angEyeAngles = v;
            else if (k == "m_iHealth")        off::m_iHealth = v;
            else if (k == "m_iTeamNum")       off::m_iTeamNum = v;
            else if (k == "m_fFlags")         off::m_fFlags = v;
            else if (k == "m_bDidSmokeEffect") off::m_bDidSmokeEffect = v;
            else if (k == "m_vSmokeColor")     off::m_vSmokeColor = v;
            else if (k == "m_iPing")           off::m_iPing = v;
        }
    }
    fclose(f);
    Log("[dumper] loaded offsets.cache\n");
}

static bool ProbeSmokeOffsets() {
    if (off::m_bDidSmokeEffect && off::m_vSmokeColor && off::m_iPing) return true;
    if (!off::g_SchemaBase) return false;

    char mempath[64];
    snprintf(mempath, sizeof(mempath), "/proc/%d/mem", g_proc.pid());
    int fd = open(mempath, O_RDONLY);
    if (fd < 0) return false;

    auto regions = GetRWRegionsWithBSS("libschemasystem.so");
    uintptr_t schema_sys = 0;
    for (auto& r : regions) {
        constexpr size_t CHUNK = 65536;
        uint8_t buf[CHUNK];
        for (uintptr_t addr = r.start; addr < r.end; addr += CHUNK) {
            size_t toRead = std::min((size_t)(r.end - addr), CHUNK);
            ssize_t n = pread(fd, buf, toRead, (off_t)addr);
            if (n < 32) continue;
            for (size_t i = 0; i + 0x200 <= (size_t)n; i += 8) {
                int32_t scopes_len = *reinterpret_cast<int32_t*>(buf + i + 0x1F0);
                uintptr_t scopes_vec = *reinterpret_cast<uintptr_t*>(buf + i + 0x1F8);
                if (scopes_len >= 1 && scopes_len <= 32 && scopes_vec > 0x10000 && scopes_vec < 0x7FFFFFFFFFFF) {
                    schema_sys = addr + i;
                    break;
                }
            }
            if (schema_sys) break;
        }
        if (schema_sys) break;
    }

    if (!schema_sys) { close(fd); return false; }

    int32_t scopes_len = g_proc.Read<int32_t>(schema_sys + 0x1F0);
    uintptr_t scopes_vec = g_proc.Read<uintptr_t>(schema_sys + 0x1F8);

    for (int i = 0; i < scopes_len; i++) {
        uintptr_t scope = g_proc.Read<uintptr_t>(scopes_vec + i * 8);
        if (!scope) continue;
        char scope_name[128] = {0};
        uintptr_t name_ptr = g_proc.Read<uintptr_t>(scope + 0x08);
        if (!name_ptr) continue;
        pread(fd, scope_name, sizeof(scope_name) - 1, (off_t)name_ptr);
        if (strstr(scope_name, "client.dll") == nullptr && strstr(scope_name, "libclient.so") == nullptr) continue;

        uintptr_t hash_vec = scope + 0x560 + 0x90;
        for (int b = 0; b < 1024; b++) {
            uintptr_t elem = g_proc.Read<uintptr_t>(hash_vec + b * 24 + 0x28);
            while (elem && elem > 0x10000 && elem < 0x7FFFFFFFFFFF) {
                uintptr_t class_ptr = g_proc.Read<uintptr_t>(elem + 0x10);
                if (class_ptr) {
                    uintptr_t cname_ptr = g_proc.Read<uintptr_t>(class_ptr + 0x08);
                    char cname[128] = {0};
                    if (cname_ptr) pread(fd, cname, sizeof(cname) - 1, (off_t)cname_ptr);
                    if (strstr(cname, "SmokeGrenadeProjectile") != nullptr) {
                        int16_t fcount = g_proc.Read<int16_t>(class_ptr + 0x24);
                        uintptr_t fvec = g_proc.Read<uintptr_t>(class_ptr + 0x30);
                        for (int f = 0; f < fcount && f < 200; f++) {
                            uintptr_t field = fvec + f * 0x20;
                            uintptr_t fname_ptr = g_proc.Read<uintptr_t>(field);
                            char fname[128] = {0};
                            if (fname_ptr) pread(fd, fname, sizeof(fname) - 1, (off_t)fname_ptr);
                            int32_t foff = g_proc.Read<int32_t>(field + 0x10);
                            if (strcmp(fname, "m_bDidSmokeEffect") == 0) off::m_bDidSmokeEffect = foff;
                            if (strcmp(fname, "m_vSmokeColor") == 0) off::m_vSmokeColor = foff;
                        }
                    }
                    if (strstr(cname, "PlayerController") != nullptr || strstr(cname, "CBasePlayerController") != nullptr || strstr(cname, "CCSPlayerController") != nullptr) {
                        int16_t fcount = g_proc.Read<int16_t>(class_ptr + 0x24);
                        uintptr_t fvec = g_proc.Read<uintptr_t>(class_ptr + 0x30);
                        for (int f = 0; f < fcount && f < 200; f++) {
                            uintptr_t field = fvec + f * 0x20;
                            uintptr_t fname_ptr = g_proc.Read<uintptr_t>(field);
                            char fname[128] = {0};
                            if (fname_ptr) pread(fd, fname, sizeof(fname) - 1, (off_t)fname_ptr);
                            int32_t foff = g_proc.Read<int32_t>(field + 0x10);
                            if (strcmp(fname, "m_iPing") == 0 && foff > 0) off::m_iPing = foff;
                        }
                    }
                }
                elem = g_proc.Read<uintptr_t>(elem + 0x08);
            }
        }
    }

    close(fd);
    if ((off::m_bDidSmokeEffect && off::m_vSmokeColor) || off::m_iPing) {
        Log("[dumper] probed schema: m_bDidSmokeEffect=0x%lX, m_vSmokeColor=0x%lX, m_iPing=0x%lX\n",
            off::m_bDidSmokeEffect, off::m_vSmokeColor, off::m_iPing);
        SaveCache();
        return true;
    }
    return false;
}

static bool s_pse_loaded = false;

static int FindLocalPlayerController() {
    if (!off::g_EntityListPtr) return -1;
    uintptr_t chunk0 = g_proc.Read<uintptr_t>(off::g_EntityListPtr + 0x10);
    if (!chunk0 || chunk0 < 0x10000) return -1;

    int requested = off::g_LocalControllerIdx;
    if (requested > 0) {
        uintptr_t controller = GetEntityFromList(off::g_EntityListPtr, requested);
        uint32_t pawnHandle = controller ? g_proc.Read<uint32_t>(controller + off::m_hPlayerPawn) : 0;
        uintptr_t pawn = pawnHandle && pawnHandle != 0xFFFFFFFF
            ? GetEntityFromList(off::g_EntityListPtr, pawnHandle & 0x7FFF) : 0;
        if (pawn) return requested;
    }

    if (off::m_bIsLocalPlayerController) {
        for (int i = 1; i <= 64; i++) {
            uintptr_t controller = GetEntityFromList(off::g_EntityListPtr, i);
            if (!controller || controller < 0x10000) continue;
            if (!g_proc.Read<bool>(controller + off::m_bIsLocalPlayerController)) continue;
            uint32_t pawnHandle = g_proc.Read<uint32_t>(controller + off::m_hPlayerPawn);
            if (pawnHandle == 0 || pawnHandle == 0xFFFFFFFF) continue;
            uintptr_t pawn = GetEntityFromList(off::g_EntityListPtr, pawnHandle & 0x7FFF);
            if (!pawn) continue;
            int hp = g_proc.Read<int>(pawn + off::m_iHealth);
            int team = game::Team(pawn);
            if (hp > 0 && hp <= 100 && (team == 2 || team == 3))
                return i;
        }
        for (int i = 1; i <= 64; i++) {
            uintptr_t controller = GetEntityFromList(off::g_EntityListPtr, i);
            if (!controller || controller < 0x10000) continue;
            if (g_proc.Read<bool>(controller + off::m_bIsLocalPlayerController))
                return i;
        }
    }

    for (int i = 1; i <= 64; i++) {
        uintptr_t controller = GetEntityFromList(off::g_EntityListPtr, i);
        if (!controller || controller < 0x10000) continue;
        uint32_t pawnHandle = g_proc.Read<uint32_t>(controller + off::m_hPlayerPawn);
        if (pawnHandle == 0 || pawnHandle == 0xFFFFFFFF) continue;
        uintptr_t pawn = GetEntityFromList(off::g_EntityListPtr, pawnHandle & 0x7FFF);
        if (!pawn) continue;
        int hp = g_proc.Read<int>(pawn + off::m_iHealth);
        int team = game::Team(pawn);
        if (hp > 0 && hp <= 100 && (team == 2 || team == 3))
            return i;
    }

    static const uintptr_t kOriginCands[] = {0xC8, 0xD0, 0xCC, 0xB8, 0xC0, 0xE0, 0xE8, 0xF0, 0x100, 0x108};

    if (off::m_hPlayerPawn && off::m_hPlayerPawn != 0xA94 && off::m_pGameSceneNode && off::m_vecAbsOrigin) {
        for (int i = 1; i <= 64; i++) {
            uintptr_t controller = GetEntityFromList(off::g_EntityListPtr, i);
            if (!controller || controller < 0x10000) continue;
            uint32_t pawnHandle = g_proc.Read<uint32_t>(controller + off::m_hPlayerPawn);
            if (!pawnHandle || pawnHandle == 0xFFFFFFFF) continue;
            uintptr_t pawn = GetEntityFromList(off::g_EntityListPtr, pawnHandle & 0x7FFF);
            if (!pawn || pawn < 0x10000) continue;
            uintptr_t sn = g_proc.Read<uintptr_t>(pawn + off::m_pGameSceneNode);
            if (sn < 0x10000) continue;
            float x = g_proc.Read<float>(sn + off::m_vecAbsOrigin);
            float y = g_proc.Read<float>(sn + off::m_vecAbsOrigin + 4);
            float z = g_proc.Read<float>(sn + off::m_vecAbsOrigin + 8);
            if (LooksLikeWorldPos(x, y, z)) {
                return i;
            }
        }
    }

    if (s_pse_loaded) return requested > 0 ? requested : 1;
    static bool s_dump_file_present = (access("/tmp/offsets_dump.h", F_OK) == 0);
    if (s_dump_file_present) return requested > 0 ? requested : 1;

    static int s_probe_entry = 0;
    if ((s_probe_entry++ % 60) == 0) Log("[dumper] combined probe starting\n");
    for (uintptr_t hp_try = 0x800; hp_try < 0xC80; hp_try += 4) {
        int first_i = -1;
        int total_hits = 0;
        uintptr_t best_scene = 0, best_origin = 0;
        int best_scene_hits = 0;
        std::vector<uintptr_t> pawn_cands;
        for (int i = 1; i <= 64; i++) {
            uintptr_t controller = GetEntityFromList(off::g_EntityListPtr, i);
            if (!controller || controller < 0x10000) continue;
            uint32_t pawnHandle = g_proc.Read<uint32_t>(controller + hp_try);
            if (!pawnHandle || pawnHandle == 0xFFFFFFFF) continue;
            uint32_t idx = pawnHandle & 0x7FFF;
            if (idx < 100 || idx > 4095) continue;
            uintptr_t pawn = GetEntityFromList(off::g_EntityListPtr, idx);
            if (!pawn || pawn < 0x10000) continue;
            uintptr_t vtable = g_proc.Read<uintptr_t>(pawn);
            if (!vtable || vtable < 0x10000) continue;
            if (!off::g_ClientBase || vtable < off::g_ClientBase || vtable > off::g_ClientBase + 0x8000000) continue;
            bool dup = false;
            for (uintptr_t p : pawn_cands) if (p == pawn) { dup = true; break; }
            if (dup) continue;
            if (first_i < 0) first_i = i;
            total_hits++;
            if (pawn_cands.size() < 8) pawn_cands.push_back(pawn);
        }
        if (total_hits < 2) continue;

        for (uintptr_t sn_off = 0x100; sn_off <= 0x1000; sn_off += 4) {
            for (uintptr_t oc : kOriginCands) {
                int hits = 0;
                for (uintptr_t pawn : pawn_cands) {
                    uintptr_t sn = g_proc.Read<uintptr_t>(pawn + sn_off);
                    if (sn < 0x10000 || sn > 0x7FFFFFFFFFFF) continue;
                    float x = g_proc.Read<float>(sn + oc);
                    float y = g_proc.Read<float>(sn + oc + 4);
                    float z = g_proc.Read<float>(sn + oc + 8);
                    if (!LooksLikeWorldPos(x, y, z)) continue;
                    hits++;
                }
                if (hits > best_scene_hits) {
                    best_scene_hits = hits;
                    best_scene = sn_off;
                    best_origin = oc;
                }
            }
        }
        if (total_hits >= 2 && s_probe_entry % 60 == 1)
            Log("[dumper] hp_try=0x%lX total_hits=%d best_scene_hits=%d best_scene=0x%lX\n",
                hp_try, total_hits, best_scene_hits, best_scene);

        if (best_scene_hits >= 2) {
            off::m_hPlayerPawn = hp_try;
            off::m_pGameSceneNode = best_scene;
            off::m_vecAbsOrigin = best_origin;
            Log("[dumper] combined probe: m_hPlayerPawn=0x%lX m_pGameSceneNode=0x%lX m_vecAbsOrigin=0x%lX (scene hits=%d)\n",
                hp_try, best_scene, best_origin, best_scene_hits);
            SaveCache();
            return first_i;
        }
    }
    return -1;
}

static bool s_ready = false;
static int  s_fail_streak = 0;
static int  s_offsets_pid = -1;
static int  s_pse_retry = 0;
static int  s_pse_loaded_pid = 0;

static int DumpFilePid() {
    FILE* f = fopen("/tmp/offsets_dump.h", "r");
    if (!f) return 0;
    char line[128] = {0};
    int dump_pid = 0;
    if (fgets(line, sizeof(line), f)) {
        const char* marker = strstr(line, "PID ");
        if (marker) dump_pid = atoi(marker + 4);
    }
    fclose(f);
    return dump_pid;
}
static int  s_layout_retry = 0;
static int  s_vm_retry = 0;
static bool s_entity_list_checked = false;
static int  s_last_logged = -2;

void Reset() {
    s_ready = false;
    s_fail_streak = 0;
    s_offsets_pid = -1;
    s_pse_loaded = false;
    s_pse_loaded_pid = 0;
    s_pse_retry = 0;
    s_layout_retry = 0;
    s_vm_retry = 0;
    s_entity_list_checked = false;
    s_last_logged = -2;
    off::ResetProcessState();
    LoadCache();
}

bool ReadyForGameplay() { return off::g_OffsetsReady.load() && off::g_EntityListPtr != 0; }

std::vector<uintptr_t> FindButtonNameSlots(const char* name) {
    std::vector<uintptr_t> out;
    std::string needle(1, '\0');
    needle += name;
    needle += '\0';
    constexpr size_t kChunk = 1 << 20;
    std::vector<char> buf(kChunk + needle.size());
    std::vector<uintptr_t> strings;
    for (const auto& r : GetReadOnlyRegions("libclient.so")) {
        for (uintptr_t at = r.start; at < r.end; at += kChunk) {
            size_t len = std::min<size_t>(kChunk + needle.size(), r.end - at);
            if (!g_proc.ReadBytes(at, buf.data(), len)) continue;
            std::string_view view(buf.data(), len);
            for (size_t pos = view.find(needle); pos != std::string_view::npos; pos = view.find(needle, pos + 1))
                strings.push_back(at + pos + 1);
        }
    }
    if (strings.empty()) return out;
    std::vector<uint8_t> data(kChunk);
    for (const auto& r : GetRWRegionsWithBSS("libclient.so")) {
        for (uintptr_t at = r.start; at < r.end; at += kChunk) {
            size_t len = std::min<size_t>(kChunk, r.end - at);
            if (!g_proc.ReadBytes(at, data.data(), len)) continue;
            for (size_t off = 0; off + 8 <= len; off += 8) {
                uintptr_t value;
                memcpy(&value, data.data() + off, 8);
                if (std::find(strings.begin(), strings.end(), value) != strings.end()) out.push_back(at + off);
            }
        }
    }
    return out;
}

std::vector<uintptr_t> FindButtonStateCandidates(const char* name) {
    std::vector<uintptr_t> out;
    for (uintptr_t slot : FindButtonNameSlots(name))
        for (uintptr_t field = 0; field < 0x80; field += 4) out.push_back(slot + field);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void Run() {
    LogInit();
    if (!g_proc.IsAlive()) {
        Reset();
        return;
    }

    int pid = g_proc.pid();
    if (s_offsets_pid != pid) {
        Reset();
        s_offsets_pid = pid;
        bool json = off::LoadConfiguredJson();
        Log("[dumper] cs2 pid=%d client=0x%lX engine=0x%lX schema=0x%lX json=%d\n",
            pid, off::g_ClientBase, off::g_EngineBase, off::g_SchemaBase, (int)json);
    }

    if (!off::g_ClientBase) {
        off::UpdateBases();
        if (!off::g_ClientBase) {
            static int wait_count = 0;
            if (++wait_count % 20 == 1)
                Log("[dumper] waiting for libclient.so... (attempt %d, cs2 pid=%d)\n", wait_count, g_proc.pid());
            return;
        }
        Log("[dumper] libclient.so loaded @ 0x%lX\n", off::g_ClientBase);
    }

    if (s_pse_retry-- <= 0) {
        s_pse_retry = 4;
        int dump_pid = DumpFilePid();
        bool fresh = dump_pid == pid;
        if (!s_pse_loaded || (fresh && s_pse_loaded_pid != pid)) {
            if (s_pse_loaded) {
                off::g_EntityListPtr = 0;
                off::g_LocalControllerIdx = -1;
                s_entity_list_checked = false;
            }
            if (off::LoadFromPseFile()) {
                s_pse_loaded = true;
                s_pse_loaded_pid = dump_pid;
                Log("[dumper] loaded /tmp/offsets_dump.h (dump pid=%d)\n", dump_pid);
            }
        }
    }

    if (off::g_EntityListPtr && !s_entity_list_checked) {
        if (!TryVerifyEntityList(off::g_EntityListPtr)) {
            Log("[dumper] loaded entity list is invalid, rescanning\n");
            off::g_EntityListPtr = 0;
            off::g_LocalControllerIdx = -1;
            s_pse_loaded = false;
        }
        s_entity_list_checked = true;
    }

    if (!off::g_EntityListPtr) {
        off::g_OffsetsReady.store(false);
        static int scan_attempt = 0;
        scan_attempt++;
        Log("[dumper] no entity list — scanning (attempt %d)...\n", scan_attempt);
        bool ok = ScanForEntityList();
        Log("[dumper] scan result=%d entityList=0x%lX (attempt %d)\n", (int)ok, off::g_EntityListPtr, scan_attempt);
        if (!ok) return;
        s_entity_list_checked = true;
    }

    int idx = FindLocalPlayerController();
    if (idx >= 0) {
        off::g_LocalControllerIdx = idx;
        off::g_OffsetsReady.store(true);
        s_ready = true;
        s_fail_streak = 0;
        if (idx != s_last_logged) {
            Log("[dumper] LOCAL PLAYER controller idx=%d\n", idx);
            s_last_logged = idx;
        }

        if (!off::m_pGameSceneNode && s_layout_retry-- <= 0) {
            s_layout_retry = 30;
            ProbePawnLayoutMulti();
        }

        static int s_angles_retry = 0;
        if (!off::m_angEyeAngles && s_angles_retry-- <= 0) {
            s_angles_retry = 15;
            uintptr_t ctrl = GetEntityFromList(off::g_EntityListPtr, idx);
            uint32_t ph = ctrl ? g_proc.Read<uint32_t>(ctrl + off::m_hPlayerPawn) : 0;
            uintptr_t pawn = (ph && ph != 0xFFFFFFFF) ? GetEntityFromList(off::g_EntityListPtr, ph & 0x7FFF) : 0;
            if (pawn) {
                for (uintptr_t o = 0x1000; o <= 0x2000; o += 4) {
                    float px = g_proc.Read<float>(pawn + o);
                    float py = g_proc.Read<float>(pawn + o + 4);
                    if (px >= -89.f && px <= 89.f && py >= -180.f && py <= 180.f &&
                        (fabsf(px) + fabsf(py) > 5.f) && std::isfinite(px) && std::isfinite(py)) {
                        off::m_angEyeAngles = o;
                        Log("[dumper] probed m_angEyeAngles=0x%lX (pitch=%.1f yaw=%.1f)\n", o, px, py);
                        SaveCache();
                        break;
                    }
                }
            }
        }

        static int s_hp_probe_retry = 0;
        static uintptr_t s_probed_hp = 0, s_probed_team = 0;
        static bool s_pse_file_exists = (access("/tmp/offsets_dump.h", F_OK) == 0);
        if (!s_pse_loaded && !s_pse_file_exists && (off::m_iHealth == 0x4BC || off::m_iTeamNum == 0x557) && s_hp_probe_retry-- <= 0) {
            s_hp_probe_retry = 30;
            std::vector<uintptr_t> pawns;
            for (int i = 1; i <= 64 && pawns.size() < 6; i++) {
                uintptr_t ctrl = GetEntityFromList(off::g_EntityListPtr, i);
                if (!ctrl) continue;
                uint32_t ph = g_proc.Read<uint32_t>(ctrl + off::m_hPlayerPawn);
                if (!ph || ph == 0xFFFFFFFF) continue;
                uintptr_t pawn = GetEntityFromList(off::g_EntityListPtr, ph & 0x7FFF);
                if (pawn) pawns.push_back(pawn);
            }
            if (pawns.size() >= 3) {
                if (!s_probed_team) {
                    for (uintptr_t o = 0x300; o <= 0x800; o++) {
                        int ok = 0;
                        for (uintptr_t p : pawns) {
                            uint8_t v = g_proc.Read<uint8_t>(p + o);
                            if (v == 2 || v == 3) ok++;
                        }
                        if (ok == (int)pawns.size()) {
                            s_probed_team = o;
                            off::m_iTeamNum = o;
                            Log("[dumper] probed m_iTeamNum=0x%lX\n", o);
                            SaveCache();
                            break;
                        }
                    }
                }
                if (!s_probed_hp) {
                    for (uintptr_t o = 0x300; o <= 0x800; o += 4) {
                        int ok = 0;
                        for (uintptr_t p : pawns) {
                            int v = g_proc.Read<int>(p + o);
                            if (v > 0 && v <= 100) ok++;
                        }
                        if (ok == (int)pawns.size()) {
                            if (s_probed_team && o == (s_probed_team & ~3ULL)) continue;
                            s_probed_hp = o;
                            off::m_iHealth = o;
                            Log("[dumper] probed m_iHealth=0x%lX\n", o);
                            SaveCache();
                            break;
                        }
                    }
                }
            }
        }
        ProbeSmokeOffsets();
        if (!off::dwViewMatrix && off::m_pGameSceneNode && off::m_angEyeAngles && s_vm_retry-- <= 0) {
            s_vm_retry = 30;
            ScanForViewMatrix(idx);
        }
        return;
    }

    off::g_OffsetsReady.store(false);
    s_ready = false;
    if (s_fail_streak % 60 == 0)
        Log("[dumper] no local player (streak=%d) — menu/dead/spectating\n", s_fail_streak);
    s_fail_streak++;

    if (s_fail_streak >= 60) {
        uintptr_t chunk0 = g_proc.Read<uintptr_t>(off::g_EntityListPtr + 0x10);
        if (!chunk0 || chunk0 < 0x10000) {
            Log("[dumper] entity list gone bad, rescanning\n");
            off::g_EntityListPtr = 0;
            s_pse_loaded = false;
            s_entity_list_checked = false;
            s_fail_streak = 0;
        }
    }
}

}
