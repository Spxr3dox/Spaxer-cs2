#include "core.h"
#include "../sdk/schema.h"
#include "../features/esp_internal.h"
extern void FeaturesInternalStart();
#include <pthread.h>
#include <unistd.h>
#include <cstdio>
#include <cstdarg>
#include <atomic>

static FILE* g_log = nullptr;
static pthread_t g_init_thread{};
static std::atomic<bool> g_ready{false};
static std::atomic<bool> g_shutdown{false};

void Log(const char* fmt, ...) {
    if (!g_log) g_log = fopen("/tmp/spaxer_internal.log", "a");
    if (!g_log) return;
    va_list a; va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

bool CoreReady() { return g_ready.load(); }
bool CoreShutdown() { return g_shutdown.load(); }

static void* InitThread(void*) {
    Log("[core] init thread started, pid=%d", getpid());
    for (int attempt = 1; attempt <= 60 && !g_shutdown.load(); attempt++) {
        sleep(2);
        if (false && SchemaDoDump()) {
            Log("[core] schema ready in %d attempts", attempt);
            g_ready.store(true);
            return nullptr;
        }
        Log("[core] schema attempt %d failed", attempt);
    }
    Log("[core] schema init gave up");
    return nullptr;
}

__attribute__((constructor))
static void OnLoad() {
    Log("[core] libspaxer.so loaded into pid=%d", getpid());
    // EspStart();
    FeaturesInternalStart();
    pthread_create(&g_init_thread, nullptr, InitThread, nullptr);
    pthread_detach(g_init_thread);
}

__attribute__((destructor))
static void OnUnload() {
    g_shutdown.store(true);
    Log("[core] libspaxer.so unloading");
    if (g_log) { fclose(g_log); g_log = nullptr; }
}
