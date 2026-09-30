#include "esp_internal.h"
#include "../core/core.h"
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <atomic>

std::mutex g_esp_mtx;
EspSnapshot g_esp_snap;
std::atomic<bool> g_esp_ready{false};

namespace off {
    static uintptr_t m_iHealth = 0;
    static uintptr_t m_iTeamNum = 0;
    static uintptr_t m_hPlayerPawn = 0;
    static uintptr_t m_pGameSceneNode = 0;
    static uintptr_t m_vecAbsOrigin = 0;
    static uintptr_t m_iszPlayerName = 0;
    static uintptr_t m_bIsLocalPlayerController = 0;
    static uintptr_t m_lifeState = 0;
    static uintptr_t m_bDormant = 0;
    static uintptr_t dwViewMatrix = 0;
    static uintptr_t g_ClientBase = 0;
}

static thread_local sigjmp_buf s_jb;
static thread_local volatile bool s_in = false;

static void SegHandler(int) {
    if (s_in) siglongjmp(s_jb, 1);
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
}

template<typename Fn>
static bool Safe(Fn fn) {
    struct sigaction sa{}, o1{}, o2{};
    sa.sa_handler = SegHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &o1);
    sigaction(SIGBUS, &sa, &o2);
    s_in = true;
    bool ok = false;
    if (sigsetjmp(s_jb, 1) == 0) { fn(); ok = true; }
    s_in = false;
    sigaction(SIGSEGV, &o1, nullptr);
    sigaction(SIGBUS, &o2, nullptr);
    return ok;
}

template<typename T>
static T SafeRead(uintptr_t addr, T def = T{}) {
    if (addr < 0x10000) return def;
    T v = def;
    Safe([&]() { v = *(T*)addr; });
    return v;
}

static bool ReadOffsets() {
    FILE* f = fopen("/tmp/offsets_dump.h", "r");
    if (!f) return false;
    char line[256];
    int got = 0;
    while (fgets(line, sizeof(line), f)) {
        char name[64]; unsigned int v;
        if (sscanf(line, "%63s = 0x%x", name, &v) != 2) continue;
        auto match = [&](const char* n, uintptr_t& out){ if (!strcmp(name, n)) { out = v; got++; } };
        match("m_iHealth", off::m_iHealth);
        match("m_iTeamNum", off::m_iTeamNum);
        match("m_hPlayerPawn", off::m_hPlayerPawn);
        match("m_pGameSceneNode", off::m_pGameSceneNode);
        match("m_vecAbsOrigin", off::m_vecAbsOrigin);
        match("m_iszPlayerName", off::m_iszPlayerName);
        match("m_bIsLocalPlayerController", off::m_bIsLocalPlayerController);
        match("m_lifeState", off::m_lifeState);
        match("m_bDormant", off::m_bDormant);
    }
    fclose(f);
    Log("[esp] offsets loaded %d fields", got);
    return off::m_iHealth && off::m_iTeamNum && off::m_hPlayerPawn && off::m_pGameSceneNode;
}

static bool FindLibclientBase() {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, "libclient.so")) continue;
        if (!strstr(line, " r-xp ")) continue;
        uintptr_t start;
        if (sscanf(line, "%lx-", &start) == 1) {
            off::g_ClientBase = start;
            Log("[esp] libclient.so base=0x%lx", start);
            fclose(f);
            return true;
        }
    }
    fclose(f);
    return false;
}

struct MapRegion { uintptr_t start, end; };
static std::vector<MapRegion> GetRWRegionsWithBSS() {
    std::vector<MapRegion> out;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return out;
    char line[1024];
    uintptr_t lastEnd = 0;
    bool inLib = false;
    while (fgets(line, sizeof(line), f)) {
        uintptr_t start, end;
        if (sscanf(line, "%lx-%lx", &start, &end) != 2) continue;
        bool rw = strstr(line, "rw-") != nullptr;
        bool lib = strstr(line, "libclient.so") != nullptr;
        if (lib && rw) { out.push_back({start, end}); lastEnd = end; inLib = true; }
        else if (inLib && rw && start == lastEnd && !strchr(line, '/')) { out.push_back({start, end}); lastEnd = end; }
        else if (inLib && start != lastEnd) inLib = false;
    }
    fclose(f);
    return out;
}

static uintptr_t GetEntity(uintptr_t entityList, int idx) {
    if (idx <= 0 || idx > 0x4000) return 0;
    uintptr_t chunk = SafeRead<uintptr_t>(entityList + 8 * ((idx & 0x7FFF) >> 9) + 16);
    if (chunk < 0x10000) return 0;
    return SafeRead<uintptr_t>(chunk + 120 * (idx & 0x1FF));
}

static int Team(uintptr_t ent) {
    if (!off::m_iTeamNum || ent < 0x10000) return 0;
    return SafeRead<uint8_t>(ent + off::m_iTeamNum);
}

static bool VerifyList(uintptr_t val) {
    uintptr_t chunk0 = SafeRead<uintptr_t>(val + 0x10);
    if (chunk0 < 0x10000) return false;
    int good = 0;
    for (int i = 1; i <= 64; i++) {
        uintptr_t ent = SafeRead<uintptr_t>(chunk0 + 0x70 * i);
        if (ent < 0x10000) continue;
        int t = Team(ent);
        if (t != 2 && t != 3) continue;
        uint32_t pawnH = SafeRead<uint32_t>(ent + off::m_hPlayerPawn);
        if (pawnH == 0 || pawnH == 0xFFFFFFFF) continue;
        uintptr_t pawn = GetEntity(val, pawnH & 0x7FFF);
        if (pawn < 0x10000) continue;
        int hp = SafeRead<int>(pawn + off::m_iHealth);
        if (hp >= 1 && hp <= 100 && ++good >= 2) return true;
    }
    return false;
}

static uintptr_t g_entity_list = 0;

static bool ScanEntityList() {
    for (auto& r : GetRWRegionsWithBSS()) {
        for (uintptr_t p = r.start; p + 8 <= r.end; p += 8) {
            uintptr_t val = SafeRead<uintptr_t>(p);
            if (val < 0x10000 || val > 0x7FFFFFFFFFFF) continue;
            uintptr_t chunk0 = SafeRead<uintptr_t>(val + 0x10);
            if (chunk0 < 0x10000 || chunk0 > 0x7FFFFFFFFFFF) continue;
            if (VerifyList(val)) {
                g_entity_list = val;
                Log("[esp] entity list @ 0x%lx", val);
                return true;
            }
        }
    }
    return false;
}

static void EspLoop() {
    for (int i = 0; i < 60 && !ReadOffsets(); i++) sleep(2);
    if (!off::m_iHealth) { Log("[esp] no offsets, giving up"); return; }
    for (int i = 0; i < 60 && !FindLibclientBase(); i++) sleep(2);
    for (int i = 0; i < 120 && !ScanEntityList(); i++) sleep(3);
    if (!g_entity_list) { Log("[esp] gave up finding list"); return; }
    off::dwViewMatrix = 0x1A2A8F0;
    Log("[esp] running w/ dwViewMatrix=0x%lx", off::dwViewMatrix);

    while (true) {
        EspSnapshot snap;
        Safe([&]() { memcpy(snap.view_matrix, (void*)(off::g_ClientBase + off::dwViewMatrix), sizeof(snap.view_matrix)); });
        for (int i = 1; i <= 64; i++) {
            uintptr_t ctrl = GetEntity(g_entity_list, i);
            if (!ctrl) continue;
            int team = Team(ctrl);
            if (team != 2 && team != 3) continue;
            uint32_t pawnH = SafeRead<uint32_t>(ctrl + off::m_hPlayerPawn);
            if (pawnH == 0 || pawnH == 0xFFFFFFFF) continue;
            uintptr_t pawn = GetEntity(g_entity_list, pawnH & 0x7FFF);
            if (!pawn) continue;
            int hp = SafeRead<int>(pawn + off::m_iHealth);
            if (hp <= 0 || hp > 100) continue;
            uintptr_t node = SafeRead<uintptr_t>(pawn + off::m_pGameSceneNode);
            if (!node) continue;
            float ox = SafeRead<float>(node + off::m_vecAbsOrigin);
            float oy = SafeRead<float>(node + off::m_vecAbsOrigin + 4);
            float oz = SafeRead<float>(node + off::m_vecAbsOrigin + 8);
            bool local = off::m_bIsLocalPlayerController ? SafeRead<bool>(ctrl + off::m_bIsLocalPlayerController) : false;
            EspPlayer p{};
            p.ox = ox; p.oy = oy; p.oz = oz;
            p.hx = ox; p.hy = oy; p.hz = oz + 72.f;
            p.health = hp; p.team = team; p.local = local;
            p.dormant = off::m_bDormant ? SafeRead<bool>(node + off::m_bDormant) : false;
            char nb[64] = {};
            if (off::m_iszPlayerName) {
                uintptr_t pn = SafeRead<uintptr_t>(ctrl + off::m_iszPlayerName);
                if (pn > 0x10000) Safe([&]() { strncpy(nb, (const char*)pn, 63); nb[63] = 0; });
            }
            p.name = nb;
            snap.players.push_back(p);
        }
        snap.valid = true;
        {
            std::lock_guard<std::mutex> lk(g_esp_mtx);
            g_esp_snap = std::move(snap);
        }
        g_esp_ready.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

void EspStart() {
    std::thread(EspLoop).detach();
}
