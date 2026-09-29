#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <signal.h>
#include <setjmp.h>
#include <pthread.h>
#include <unistd.h>

static FILE* g_log = nullptr;
static void L(const char* fmt, ...) {
    if (!g_log) g_log = fopen("/tmp/spaxer_dump_inject.log", "a");
    if (!g_log) return;
    va_list a; va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fflush(g_log);
}

typedef void* (*CreateInterfaceFn)(const char*, int*);

static std::string FindLibPath(const char* name) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return "";
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, " 00000000 ")) continue;
        char* slash = strrchr(line, '/');
        if (!slash) continue;
        char* fname = slash + 1;
        char* nl = strchr(fname, '\n');
        if (nl) *nl = 0;
        if (strcmp(fname, name) != 0) continue;
        char* p = strchr(line, '/');
        if (!p) continue;
        std::string path(p);
        fclose(f);
        return path;
    }
    fclose(f);
    return "";
}

struct LibFactory { std::string name; CreateInterfaceFn factory; };
static std::vector<LibFactory> g_factories;
static bool g_factories_loaded = false;

static void LoadFactories() {
    if (g_factories_loaded) return;
    const char* libs[] = {"libschemasystem.so", "libengine2.so", "libclient.so", "libtier0.so", nullptr};
    for (int i = 0; libs[i]; i++) {
        std::string p = FindLibPath(libs[i]);
        if (p.empty()) continue;
        void* h = dlopen(p.c_str(), RTLD_NOLOAD | RTLD_LAZY);
        if (!h) continue;
        auto fn = (CreateInterfaceFn)dlsym(h, "CreateInterface");
        if (fn) {
            g_factories.push_back({libs[i], fn});
            L("[inject] factory %s @ %p\n", libs[i], (void*)fn);
        }
        dlclose(h);
    }
    g_factories_loaded = !g_factories.empty();
}

static void* GetIface(const char* name) {
    for (auto& lib : g_factories) {
        int st = 0;
        void* p = lib.factory(name, &st);
        if (p) return p;
    }
    return nullptr;
}

static thread_local sigjmp_buf s_jb;
static thread_local volatile bool s_in = false;
static void Crash(int) {
    if (s_in) siglongjmp(s_jb, 1);
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
}

template<typename Fn>
static bool Safe(Fn fn) {
    struct sigaction sa{}, o1{}, o2{};
    sa.sa_handler = Crash;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &o1);
    sigaction(SIGBUS, &sa, &o2);
    s_in = true;
    bool ok = false;
    if (sigsetjmp(s_jb, 1) == 0) ok = fn();
    s_in = false;
    sigaction(SIGSEGV, &o1, nullptr);
    sigaction(SIGBUS, &o2, nullptr);
    return ok;
}

static void* CallV(void* obj, int idx, const void* arg = nullptr) {
    void* r = nullptr;
    Safe([&]() -> bool {
        void** vt = *(void***)obj;
        if ((uintptr_t)vt < 0x10000) return false;
        void* fp = vt[idx];
        if ((uintptr_t)fp < 0x10000) return false;
        auto fn = reinterpret_cast<void*(*)(void*, const void*)>(fp);
        r = fn(obj, arg);
        return r != nullptr && (uintptr_t)r > 0x10000;
    });
    return r;
}

template<typename T>
static T ReadSafe(uintptr_t addr, T def = T{}) {
    T v = def;
    Safe([&]() -> bool {
        v = *(T*)addr;
        return true;
    });
    return v;
}

static std::string SafeStr(uintptr_t addr) {
    if (addr < 0x10000) return "";
    char buf[256] = {};
    bool ok = Safe([&]() -> bool {
        for (int i = 0; i < 255; i++) {
            buf[i] = *(char*)(addr + i);
            if (buf[i] == 0) return true;
            if (buf[i] < 0x20 || buf[i] > 0x7E) return false;
        }
        return true;
    });
    if (!ok) return "";
    return buf;
}

struct Field { std::string name; uint32_t offset; std::string type; };

static std::vector<Field> DumpClass(void* scope, const char* className) {
    std::vector<Field> out;
    void* classInfo = nullptr;
    for (int ci : {2, 3, 4, 5}) {
        classInfo = CallV(scope, ci, className);
        if (classInfo) break;
    }
    if (!classInfo) return out;

    for (int fcOff : {0x1C, 0x20, 0x24, 0x28}) {
        int16_t fieldCount = ReadSafe<int16_t>((uintptr_t)classInfo + fcOff);
        if (fieldCount <= 0 || fieldCount > 2000) continue;
        for (int fpOff : {fcOff + 0x8, fcOff + 0xC, fcOff + 0x10}) {
            uintptr_t fieldsPtr = ReadSafe<uintptr_t>((uintptr_t)classInfo + fpOff);
            if (fieldsPtr < 0x10000) continue;
            std::string test = SafeStr(ReadSafe<uintptr_t>(fieldsPtr));
            if (test.empty() || test[0] != 'm') continue;
            for (int i = 0; i < fieldCount && i < 500; i++) {
                uintptr_t fAddr = fieldsPtr + i * 0x20;
                uintptr_t namePtr = ReadSafe<uintptr_t>(fAddr);
                int32_t fOff = ReadSafe<int32_t>(fAddr + 0x10);
                std::string name = SafeStr(namePtr);
                if (name.empty()) continue;
                uintptr_t typePtr = ReadSafe<uintptr_t>(fAddr + 0x8);
                std::string type = typePtr > 0x10000 ? SafeStr(ReadSafe<uintptr_t>(typePtr + 0x8)) : "";
                out.push_back({name, (uint32_t)fOff, type});
            }
            if (!out.empty()) return out;
        }
    }
    return out;
}

static uintptr_t FindLibclientBase() {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[1024];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, "libclient.so")) continue;
        if (!strstr(line, " r-xp ") && !strstr(line, " r--p ")) continue;
        sscanf(line, "%lx-", &base);
        break;
    }
    fclose(f);
    return base;
}

static bool DoDump() {
    LoadFactories();
    if (g_factories.empty()) return false;

    void* schema = GetIface("SchemaSystem_001");
    if (!schema) return false;

    const char* mods[] = {"libclient.so", "client.dll", nullptr};
    void* scope = nullptr;
    for (int mi = 0; mods[mi] && !scope; mi++) {
        for (int idx : {13, 14, 12, 11, 15, 16, 10}) {
            scope = CallV(schema, idx, mods[mi]);
            if (scope) { L("[inject] scope @ %p vt[%d] '%s'\n", scope, idx, mods[mi]); break; }
        }
    }
    if (!scope) return false;

    std::unordered_map<std::string, uint32_t> found;
    struct Target { const char* cls; std::vector<const char*> fields; };
    std::vector<Target> targets = {
        {"CCSPlayerController", {"m_hPlayerPawn", "m_bIsLocalPlayerController", "m_iszPlayerName"}},
        {"CBasePlayerController", {"m_bIsLocalPlayerController", "m_iszPlayerName"}},
        {"C_BaseEntity", {"m_iHealth", "m_iTeamNum", "m_fFlags", "m_pGameSceneNode", "m_lifeState", "m_vecVelocity", "m_hOwnerEntity", "m_fEffects"}},
        {"C_BasePlayerPawn", {"m_pWeaponServices", "m_pCameraServices", "m_pObserverServices", "m_bIsThirdPersonView"}},
        {"CPlayer_ObserverServices", {"m_hObserverTarget", "m_iObserverMode"}},
        {"C_PlantedC4", {"m_bBombTicking", "m_bBombDefused", "m_bBeingDefused", "m_nBombSite", "m_flTimerLength", "m_flDefuseLength", "m_flC4Blow", "m_flDefuseCountDown"}},
        {"C_CSPlayerPawn", {"m_iIDEntIndex", "m_bIsScoped", "m_iShotsFired", "m_angEyeAngles", "m_flFlashMaxAlpha", "m_flFlashDuration", "m_aimPunchAngle", "m_aimPunchCache"}},
        {"C_CSPlayerPawnBase", {"m_iIDEntIndex", "m_pWeaponServices", "m_angEyeAngles", "m_bIsScoped", "m_flFlashMaxAlpha", "m_flFlashDuration"}},
        {"CGameSceneNode", {"m_vecAbsOrigin", "m_bDormant"}},
        {"C_CSPlayerPawn", {"m_entitySpottedState"}},
        {"EntitySpottedState_t", {"m_bSpotted", "m_bSpottedByMask"}},
        {"CSkeletonInstance", {"m_modelState"}},
        {"C_EconEntity", {"m_AttributeManager"}},
        {"C_AttributeContainer", {"m_Item"}},
        {"C_EconItemView", {"m_iItemDefinitionIndex"}},
        {"CPlayer_WeaponServices", {"m_hActiveWeapon"}},
        {"CBasePlayerWeapon", {"m_iClip1", "m_pReserveAmmo"}},
        {"C_BasePlayerWeapon", {"m_iClip1", "m_pReserveAmmo"}},
        {"CCSPlayerBase_CameraServices", {"m_iFOV"}},
        {"C_BaseModelEntity", {"m_Glow", "m_clrRender", "m_nRenderMode"}},
        {"CGlowProperty", {"m_iGlowType", "m_glowColorOverride", "m_bGlowing"}},
        {"C_SmokeGrenadeProjectile", {"m_nSmokeEffectTickBegin", "m_bDidSmokeEffect", "m_vSmokeColor"}},
        {"C_PostProcessingVolume", {"m_flMinExposure", "m_flMaxExposure", "m_bExposureControl"}},
        {"C_BaseCSGrenade", {"m_flThrowStrength", "m_bPinPulled"}},
        {"C_BaseGrenade", {"m_hThrower"}},
        {"C_CSPlayerPawn", {"m_bIsDefusing", "m_ArmorValue"}},
        {"C_CSPlayerPawnBase", {"m_bIsDefusing", "m_ArmorValue"}},
        {"C_BasePlayerPawn", {"m_pItemServices"}},
        {"CCSPlayer_ItemServices", {"m_bHasDefuser", "m_bHasHelmet"}},
        {"C_CSWeaponBase", {"m_bInReload", "m_fAccuracyPenalty"}},
    };

    for (auto& t : targets) {
        auto fields = DumpClass(scope, t.cls);
        for (auto& want : t.fields) {
            for (auto& f : fields) {
                if (f.name == want) {
                    if (found.find(f.name) == found.end())
                        found[f.name] = f.offset;
                    break;
                }
            }
        }
    }

    if (found.empty()) return false;
    L("[inject] found %zu fields\n", found.size());

    static const char* kSchemaClasses[] = {
        "CEntityInstance", "C_BaseEntity", "C_BaseModelEntity", "C_BaseFlex", "C_BaseAnimGraph", "C_BasePlayerPawn",
        "C_CSPlayerPawnBase", "C_CSPlayerPawn", "CBasePlayerController", "CCSPlayerController", "C_EconEntity",
        "C_BasePlayerWeapon", "C_CSWeaponBase", "C_CSWeaponBaseGun", "C_WeaponAWP", "C_C4", "C_PlantedC4",
        "C_BaseGrenade", "C_BaseCSGrenade", "C_BaseCSGrenadeProjectile", "C_SmokeGrenadeProjectile",
        "C_MolotovProjectile", "C_Inferno", "C_CSGameRules", "C_CSGameRulesProxy", "C_Team", "C_CSTeam",
        "CGameSceneNode", "CSkeletonInstance", "CModelState", "CBodyComponent", "CPlayer_WeaponServices",
        "CCSPlayer_WeaponServices", "CPlayer_ItemServices", "CCSPlayer_ItemServices", "CPlayer_MovementServices",
        "CPlayer_MovementServices_Humanoid", "CCSPlayer_MovementServices", "CPlayer_ObserverServices",
        "CPlayer_CameraServices", "CCSPlayerBase_CameraServices", "CCSPlayer_CameraServices",
        "CCSPlayer_PingServices", "CCSPlayerController_InGameMoneyServices",
        "CCSPlayerController_ActionTrackingServices", "CCSPlayerController_InventoryServices",
        "C_EconItemView", "C_AttributeContainer", "CGlowProperty", "EntitySpottedState_t",
        "C_PostProcessingVolume", "C_FogController", "C_EnvSky", "C_SkyCamera", "C_CSObserverPawn",
        "C_Chicken", "C_Hostage", "C_BaseToggle", "C_BaseTrigger", "C_PointCamera", "C_EnvWind",
    };
    char schema_tmp[64];
    snprintf(schema_tmp, sizeof(schema_tmp), "/tmp/schema_full.txt.tmp.%d", getpid());
    if (FILE* schema_out = fopen(schema_tmp, "w")) {
        size_t total = 0;
        for (const char* cls : kSchemaClasses) {
            for (const Field& f : DumpClass(scope, cls)) {
                fprintf(schema_out, "%s %s %u %s\n", cls, f.name.c_str(), f.offset, f.type.empty() ? "?" : f.type.c_str());
                total++;
            }
        }
        fclose(schema_out);
        rename(schema_tmp, "/tmp/schema_full.txt");
        L("[inject] wrote /tmp/schema_full.txt with %zu fields\n", total);
    }

    char tmp[64];
    snprintf(tmp, sizeof(tmp), "/tmp/offsets_dump.h.tmp.%d", getpid());
    FILE* out = fopen(tmp, "w");
    if (!out) return false;
    fprintf(out, "// CS2 runtime dump (PID %u)\n", getpid());
    for (auto& [k, v] : found) fprintf(out, "%s = 0x%X\n", k.c_str(), v);
    uintptr_t clientBase = FindLibclientBase();
    if (clientBase) fprintf(out, "// libclient.so @ 0x%lX\n", clientBase);
    fclose(out);
    rename(tmp, "/tmp/offsets_dump.h");
    L("[inject] wrote /tmp/offsets_dump.h with %zu fields\n", found.size());
    return true;
}

static void* Thread(void*) {
    for (int attempt = 1; attempt <= 60; attempt++) {
        sleep(2);
        if (DoDump()) {
            L("[inject] done in %d attempts\n", attempt);
            return nullptr;
        }
        L("[inject] attempt %d failed, retrying\n", attempt);
    }
    L("[inject] giving up after 60 attempts\n");
    return nullptr;
}

__attribute__((constructor))
static void Init() {
    L("[inject] loaded in pid=%d\n", getpid());
    pthread_t t;
    pthread_create(&t, nullptr, Thread, nullptr);
    pthread_detach(t);
}
