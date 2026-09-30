#include "../core/core.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>

namespace off_fi {
    static uintptr_t m_iHealth = 0;
    static uintptr_t m_iTeamNum = 0;
    static uintptr_t m_hPlayerPawn = 0;
    static uintptr_t m_pGameSceneNode = 0;
    static uintptr_t m_vecAbsOrigin = 0;
    static uintptr_t m_angEyeAngles = 0;
    static uintptr_t m_bIsThirdPersonView = 0;
    static uintptr_t m_flMinExposure = 0;
    static uintptr_t m_flMaxExposure = 0;
    static uintptr_t m_bExposureControl = 0;
    static uintptr_t m_bIsLocalPlayerController = 0;
    static uintptr_t m_pObserverServices = 0;
    static uintptr_t m_iObserverMode = 0;
    static uintptr_t m_bForcedObserverMode = 0;
    static uintptr_t m_iShotsFired = 0;
    static uintptr_t g_client_base = 0;
}

static std::atomic<bool> g_sa{false};
static std::atomic<bool> g_tp{false};
static std::atomic<bool> g_nm{false};
static std::atomic<bool> g_aa{false};

extern "C" void SpxFISilentAim(int on) { g_sa.store(on != 0); }
extern "C" void SpxFIThirdperson(int on) { g_tp.store(on != 0); }
extern "C" void SpxFINightMode(int on) { g_nm.store(on != 0); }
extern "C" void SpxFIAntiAim(int on) { g_aa.store(on != 0); }
extern "C" int SpxFIGetSilentAim() { return g_sa.load(); }
extern "C" int SpxFIGetThirdperson() { return g_tp.load(); }
extern "C" int SpxFIGetNightMode() { return g_nm.load(); }
extern "C" int SpxFIGetAntiAim() { return g_aa.load(); }

#include <sys/uio.h>
static pid_t g_pid = 0;

static bool PVR(uintptr_t addr, void* out, size_t n) {
    if (addr < 0x10000) return false;
    if (!g_pid) g_pid = getpid();
    iovec l{ out, n };
    iovec r{ reinterpret_cast<void*>(addr), n };
    ssize_t got = process_vm_readv(g_pid, &l, 1, &r, 1, 0);
    return got == (ssize_t)n;
}

template<typename T> static T Rd(uintptr_t a, T d = T{}) {
    T v = d;
    if (PVR(a, &v, sizeof(T))) return v;
    return d;
}
template<typename T> static void Wr(uintptr_t a, T v) {
    if (a < 0x10000) return;
    // writes: in-process direct assignment is fine — game memory in same address space
    // guard by verifying address is readable first
    T tmp;
    if (!PVR(a, &tmp, sizeof(T))) return;
    *(T*)a = v;
}

static bool LoadOffsets() {
    FILE* f = fopen("/tmp/offsets_dump.h", "r"); if (!f) return false;
    char ln[256]; int got = 0;
    while (fgets(ln, sizeof(ln), f)) {
        char nm[64]; unsigned int v;
        if (sscanf(ln, "%63s = 0x%x", nm, &v) != 2) continue;
        auto m = [&](const char* n, uintptr_t& o){ if (!strcmp(nm,n)){o=v;got++;} };
        m("m_iHealth", off_fi::m_iHealth);
        m("m_iTeamNum", off_fi::m_iTeamNum);
        m("m_hPlayerPawn", off_fi::m_hPlayerPawn);
        m("m_pGameSceneNode", off_fi::m_pGameSceneNode);
        m("m_vecAbsOrigin", off_fi::m_vecAbsOrigin);
        m("m_angEyeAngles", off_fi::m_angEyeAngles);
        m("m_bIsThirdPersonView", off_fi::m_bIsThirdPersonView);
        m("m_flMinExposure", off_fi::m_flMinExposure);
        m("m_flMaxExposure", off_fi::m_flMaxExposure);
        m("m_bExposureControl", off_fi::m_bExposureControl);
        m("m_bIsLocalPlayerController", off_fi::m_bIsLocalPlayerController);
        m("m_pObserverServices", off_fi::m_pObserverServices);
        m("m_iObserverMode", off_fi::m_iObserverMode);
        m("m_bForcedObserverMode", off_fi::m_bForcedObserverMode);
        m("m_iShotsFired", off_fi::m_iShotsFired);
    }
    fclose(f);
    Log("[fi] offsets %d fields (hp=0x%x eye=0x%x tp=0x%x)", got,
        (unsigned)off_fi::m_iHealth, (unsigned)off_fi::m_angEyeAngles, (unsigned)off_fi::m_bIsThirdPersonView);
    return true;
}

static bool FindBase() {
    FILE* f = fopen("/proc/self/maps", "r"); if (!f) return false;
    char ln[1024];
    while (fgets(ln, sizeof(ln), f)) {
        if (!strstr(ln, "libclient.so")) continue;
        if (!strstr(ln, " r-xp ")) continue;
        uintptr_t s;
        if (sscanf(ln, "%lx-", &s) == 1) {
            off_fi::g_client_base = s;
            fclose(f);
            Log("[fi] libclient.so base=0x%lx", s);
            return true;
        }
    }
    fclose(f);
    return false;
}

struct Region { uintptr_t s, e; };
static std::vector<Region> GetRW() {
    std::vector<Region> r;
    FILE* f = fopen("/proc/self/maps", "r"); if (!f) return r;
    char ln[1024];
    uintptr_t last = 0; bool in_lib = false;
    while (fgets(ln, sizeof(ln), f)) {
        uintptr_t s, e;
        if (sscanf(ln, "%lx-%lx", &s, &e) != 2) continue;
        bool rw = strstr(ln, "rw-") != nullptr;
        bool lib = strstr(ln, "libclient.so") != nullptr;
        if (lib && rw) { r.push_back({s, e}); last = e; in_lib = true; }
        else if (in_lib && rw && s == last && !strchr(ln, '/')) { r.push_back({s, e}); last = e; }
        else if (in_lib && s != last) in_lib = false;
    }
    fclose(f);
    return r;
}

static uintptr_t GetEntity(uintptr_t list, int idx) {
    if (idx <= 0 || idx > 0x4000) return 0;
    uintptr_t chunk = Rd<uintptr_t>(list + 8 * ((idx & 0x7FFF) >> 9) + 16);
    if (chunk < 0x10000) return 0;
    return Rd<uintptr_t>(chunk + 120 * (idx & 0x1FF));
}

static uintptr_t g_ent_list = 0;

static bool VerifyList(uintptr_t v) {
    uintptr_t c0 = Rd<uintptr_t>(v + 0x10);
    if (c0 < 0x10000) return false;
    int good = 0;
    for (int i = 1; i <= 64; i++) {
        uintptr_t e = Rd<uintptr_t>(c0 + 0x70 * i);
        if (e < 0x10000) continue;
        int t = Rd<uint8_t>(e + off_fi::m_iTeamNum);
        if (t != 2 && t != 3) continue;
        uint32_t ph = Rd<uint32_t>(e + off_fi::m_hPlayerPawn);
        if (ph == 0 || ph == 0xFFFFFFFF) continue;
        uintptr_t p = GetEntity(v, ph & 0x7FFF);
        if (p < 0x10000) continue;
        int hp = Rd<int>(p + off_fi::m_iHealth);
        if (hp >= 1 && hp <= 100 && ++good >= 2) return true;
    }
    return false;
}

static bool ScanList() {
    for (auto& r : GetRW()) {
        for (uintptr_t p = r.s; p + 8 <= r.e; p += 8) {
            uintptr_t v = Rd<uintptr_t>(p);
            if (v < 0x10000 || v > 0x7FFFFFFFFFFF) continue;
            uintptr_t c0 = Rd<uintptr_t>(v + 0x10);
            if (c0 < 0x10000 || c0 > 0x7FFFFFFFFFFF) continue;
            if (VerifyList(v)) { g_ent_list = v; Log("[fi] entity list @ 0x%lx", v); return true; }
        }
    }
    return false;
}

struct Vec3 { float x, y, z; };

static uintptr_t FindLocalPawn() {
    if (!g_ent_list) return 0;
    for (int i = 1; i <= 64; i++) {
        uintptr_t ctrl = GetEntity(g_ent_list, i);
        if (!ctrl) continue;
        if (!off_fi::m_bIsLocalPlayerController) continue;
        bool loc = Rd<bool>(ctrl + off_fi::m_bIsLocalPlayerController);
        if (!loc) continue;
        uint32_t ph = Rd<uint32_t>(ctrl + off_fi::m_hPlayerPawn);
        if (ph == 0 || ph == 0xFFFFFFFF) return 0;
        return GetEntity(g_ent_list, ph & 0x7FFF);
    }
    return 0;
}

static uint8_t g_prev_obs_mode = 0;
static void ApplyThirdperson(uintptr_t pawn) {
    if (!pawn || !off_fi::m_pObserverServices || !off_fi::m_iObserverMode) return;
    uintptr_t svc = Rd<uintptr_t>(pawn + off_fi::m_pObserverServices);
    static int dbg_count = 0;
    if (g_tp.load() && (dbg_count++ % 60) == 0) {
        Log("[fi] tp: pawn=0x%lx svc=0x%lx", (unsigned long)pawn, (unsigned long)svc);
    }
    if (!svc) return;
    bool want = g_tp.load();
    uint8_t cur = Rd<uint8_t>(svc + off_fi::m_iObserverMode);
    if (want) {
        // OBS_MODE_CHASE = 4 (third-person chase view)
        if (cur != 4) {
            if (cur != 4) g_prev_obs_mode = cur;
            Wr<uint8_t>(svc + off_fi::m_iObserverMode, 4);
            if (off_fi::m_bForcedObserverMode) Wr<bool>(svc + off_fi::m_bForcedObserverMode, true);
        }
    } else {
        if (cur == 4) {
            Wr<uint8_t>(svc + off_fi::m_iObserverMode, g_prev_obs_mode);
            if (off_fi::m_bForcedObserverMode) Wr<bool>(svc + off_fi::m_bForcedObserverMode, false);
        }
    }
}

static void ApplyAntiAim(uintptr_t pawn) {
    if (!pawn || !off_fi::m_angEyeAngles) return;
    if (!g_aa.load()) return;
    Vec3 a = Rd<Vec3>(pawn + off_fi::m_angEyeAngles);
    a.y = fmodf(a.y + 180.f, 360.f);
    Wr<Vec3>(pawn + off_fi::m_angEyeAngles, a);
}

static uintptr_t BestTargetHead(uintptr_t local_pawn, int my_team) {
    // find nearest visible enemy pawn, return world head as vec3 encoded — for simplicity return pawn ptr
    if (!g_ent_list) return 0;
    Vec3 le = Rd<Vec3>(local_pawn + off_fi::m_angEyeAngles);
    (void)le;
    float best_hp = 1e9f;
    uintptr_t best = 0;
    for (int i = 1; i <= 64; i++) {
        uintptr_t ctrl = GetEntity(g_ent_list, i);
        if (!ctrl) continue;
        int team = Rd<uint8_t>(ctrl + off_fi::m_iTeamNum);
        if (team != 2 && team != 3) continue;
        if (team == my_team) continue;
        uint32_t ph = Rd<uint32_t>(ctrl + off_fi::m_hPlayerPawn);
        if (ph == 0 || ph == 0xFFFFFFFF) continue;
        uintptr_t p = GetEntity(g_ent_list, ph & 0x7FFF);
        if (!p) continue;
        int hp = Rd<int>(p + off_fi::m_iHealth);
        if (hp <= 0) continue;
        if (hp < best_hp) { best_hp = hp; best = p; }
    }
    return best;
}

static int g_last_shots = 0;
static void ApplySilentAim(uintptr_t local_pawn) {
    if (!local_pawn || !off_fi::m_angEyeAngles || !off_fi::m_pGameSceneNode || !off_fi::m_vecAbsOrigin) return;
    if (!g_sa.load()) return;
    // Trigger only on shot fired (m_iShotsFired increased)
    if (off_fi::m_iShotsFired) {
        int cur_shots = Rd<int>(local_pawn + off_fi::m_iShotsFired);
        bool fired = cur_shots != g_last_shots;
        g_last_shots = cur_shots;
        if (!fired) return;
    }
    int my_team = Rd<uint8_t>(local_pawn + off_fi::m_iTeamNum);
    uintptr_t enemy = BestTargetHead(local_pawn, my_team);
    if (!enemy) return;
    uintptr_t node = Rd<uintptr_t>(enemy + off_fi::m_pGameSceneNode);
    if (!node) return;
    Vec3 origin = Rd<Vec3>(node + off_fi::m_vecAbsOrigin);
    Vec3 head = { origin.x, origin.y, origin.z + 64.f };
    uintptr_t local_node = Rd<uintptr_t>(local_pawn + off_fi::m_pGameSceneNode);
    if (!local_node) return;
    Vec3 le_pos = Rd<Vec3>(local_node + off_fi::m_vecAbsOrigin);
    le_pos.z += 64.f;
    float dx = head.x - le_pos.x, dy = head.y - le_pos.y, dz = head.z - le_pos.z;
    float horiz = sqrtf(dx*dx + dy*dy);
    Vec3 ang;
    ang.x = -atan2f(dz, horiz) * 57.2957795f;
    ang.y = atan2f(dy, dx) * 57.2957795f;
    ang.z = 0.f;
    Wr<Vec3>(local_pawn + off_fi::m_angEyeAngles, ang);
}

static void ApplyNightMode() {
    if (!g_ent_list || !off_fi::m_flMinExposure || !off_fi::m_flMaxExposure || !off_fi::m_bExposureControl) return;
    if (!g_nm.load()) return;
    // iterate all entities looking for C_PostProcessingVolume — brute force
    for (int i = 1; i <= 2048; i++) {
        uintptr_t e = GetEntity(g_ent_list, i);
        if (!e) continue;
        // No safe way to check entity class without RTTI — just try writing to fields
        Wr<bool>(e + off_fi::m_bExposureControl, true);
        Wr<float>(e + off_fi::m_flMinExposure, 0.15f);
        Wr<float>(e + off_fi::m_flMaxExposure, 0.35f);
    }
}

static bool LoadBridge() {
    FILE* f = fopen("/tmp/spx_bridge.txt", "r"); if (!f) return false;
    char ln[128];
    while (fgets(ln, sizeof(ln), f)) {
        char name[32]; unsigned long v = 0;
        if (sscanf(ln, "%31s 0x%lx", name, &v) == 2) {
            if (!strcmp(name, "entity_list")) g_ent_list = (uintptr_t)v;
            else if (!strcmp(name, "client_base")) off_fi::g_client_base = (uintptr_t)v;
        } else if (sscanf(ln, "%31s %ld", name, &v) == 2) {
            (void)v; // local_controller_idx not needed here (we walk entity list)
        }
    }
    fclose(f);
    return g_ent_list != 0;
}

static void MainLoop() {
    for (int i = 0; i < 60; i++) { if (LoadOffsets() && off_fi::m_iHealth) break; sleep(2); }
    // Wait for external to write bridge (has entity list ptr)
    for (int i = 0; i < 120 && !LoadBridge(); i++) sleep(1);
    if (!g_ent_list) { Log("[fi] no bridge, giving up"); return; }
    Log("[fi] loaded entity_list=0x%lx from bridge", (unsigned long)g_ent_list);
    Log("[fi] main loop running");
    int refresh_ctr = 0;
    while (true) {
        if (++refresh_ctr >= 30) { refresh_ctr = 0; LoadBridge(); }
        uintptr_t pawn = FindLocalPawn();
        if (pawn) {
            int hp = Rd<int>(pawn + off_fi::m_iHealth);
            if (hp > 0) {
                ApplyThirdperson(pawn);
                ApplySilentAim(pawn);
                ApplyAntiAim(pawn);
            }
        }
        ApplyNightMode();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}


static void ReadFlags() {
    FILE* f = fopen("/tmp/spx_internal.flags", "r"); if (!f) return;
    char ln[128];
    while (fgets(ln, sizeof(ln), f)) {
        char name[32]; unsigned v = 0;
        if (sscanf(ln, "%31s %u", name, &v) != 2) continue;
        if (!strcmp(name, "silent_aim"))      g_sa.store(v != 0);
        else if (!strcmp(name, "thirdperson")) g_tp.store(v != 0);
        else if (!strcmp(name, "night_mode"))  g_nm.store(v != 0);
        else if (!strcmp(name, "anti_aim"))    g_aa.store(v != 0);
    }
    fclose(f);
}

static void FlagsPollThread() {
    while (true) {
        ReadFlags();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void FeaturesInternalStart() {
    std::thread(MainLoop).detach();
    std::thread(FlagsPollThread).detach();
}
