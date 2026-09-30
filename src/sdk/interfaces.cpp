#include "interfaces.h"
#include "../core/core.h"
#include <dlfcn.h>
#include <signal.h>
#include <setjmp.h>
#include <cstring>
#include <cstdio>
#include <vector>

struct LibFactory { const char* name; CreateInterfaceFn factory; };
static std::vector<LibFactory> g_factories;
static bool g_loaded = false;

static thread_local sigjmp_buf s_jb;
static thread_local volatile bool s_in = false;
static void SdkCrash(int) {
    if (s_in) siglongjmp(s_jb, 1);
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
}

template<typename Fn>
static bool Safe(Fn fn) {
    struct sigaction sa{}, o1{}, o2{};
    sa.sa_handler = SdkCrash;
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
        if (!p) { fclose(f); return ""; }
        std::string path(p);
        fclose(f);
        return path;
    }
    fclose(f);
    return "";
}

bool SdkLoadFactories() {
    if (g_loaded) return true;
    static const char* libs[] = {"libschemasystem.so", "libengine2.so", "libclient.so", "libtier0.so", nullptr};
    for (int i = 0; libs[i]; i++) {
        std::string p = FindLibPath(libs[i]);
        if (p.empty()) continue;
        void* h = dlopen(p.c_str(), RTLD_NOLOAD | RTLD_LAZY);
        if (!h) continue;
        auto fn = (CreateInterfaceFn)dlsym(h, "CreateInterface");
        if (fn) {
            g_factories.push_back({libs[i], fn});
            Log("[sdk] factory %s @ %p", libs[i], (void*)fn);
        }
        dlclose(h);
    }
    g_loaded = !g_factories.empty();
    return g_loaded;
}

void* SdkGetIface(const char* name) {
    for (auto& lib : g_factories) {
        int st = 0;
        void* p = lib.factory(name, &st);
        if (p) return p;
    }
    return nullptr;
}

uintptr_t SdkFindLibBase(const char* soname) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[1024];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, soname)) continue;
        if (!strstr(line, " r-xp ") && !strstr(line, " r--p ")) continue;
        sscanf(line, "%lx-", &base);
        break;
    }
    fclose(f);
    return base;
}

void* SdkCallV(void* obj, int idx, const void* arg) {
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

std::string SdkSafeStr(uintptr_t addr) {
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

template<typename T>
T SdkReadSafe(uintptr_t addr, T def) {
    T v = def;
    Safe([&]() -> bool { v = *(T*)addr; return true; });
    return v;
}
template int8_t   SdkReadSafe(uintptr_t, int8_t);
template int16_t  SdkReadSafe(uintptr_t, int16_t);
template int32_t  SdkReadSafe(uintptr_t, int32_t);
template int64_t  SdkReadSafe(uintptr_t, int64_t);
template uint8_t  SdkReadSafe(uintptr_t, uint8_t);
template uint16_t SdkReadSafe(uintptr_t, uint16_t);
template uint32_t SdkReadSafe(uintptr_t, uint32_t);
template uint64_t SdkReadSafe(uintptr_t, uint64_t);
template float    SdkReadSafe(uintptr_t, float);
template double   SdkReadSafe(uintptr_t, double);
