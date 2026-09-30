#include "features.h"
#include "config/settings.h"
#include "input/input.h"
#include "sdk/game.h"
#include "sdk/visibility.h"
#include "state.h"
#include <linux/input.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <locale>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace features {

using Clock = std::chrono::steady_clock;

static std::atomic<bool> s_running{false};
static std::thread s_thread;

constexpr auto kEffectsInterval = std::chrono::milliseconds(4);
constexpr auto kEntityScanInterval = std::chrono::milliseconds(250);
constexpr int kFirstNonPlayerEntity = 65;
constexpr int kMaxEntities = 2048;
constexpr float kDarkestExposureScale = 0.004f;

struct ExposureBackup { float min_exposure, max_exposure; bool control; };

struct WorldEntities {
    std::vector<uintptr_t> smokes;
    std::vector<uintptr_t> post_processing;
};

static WorldEntities ScanWorldEntities() {
    WorldEntities found;
    uintptr_t list = game::EntityList();
    if (!list) return found;
    char name[40];
    for (int i = kFirstNonPlayerEntity; i < kMaxEntities; i++) {
        uintptr_t entity = game::EntityFromList(list, i);
        if (!entity || !game::DesignerName(entity, name, sizeof(name))) continue;
        if (!strcmp(name, "smokegrenade_projectile")) found.smokes.push_back(entity);
        else if (!strcmp(name, "post_processing_volume")) found.post_processing.push_back(entity);
    }
    return found;
}

template <typename T>
static void WriteIfDifferent(uintptr_t address, T value) {
    if (g_proc.Read<T>(address) != value) g_proc.Write<T>(address, value);
}

static void ApplyAntiFlash(uintptr_t pawn) {
    if (off::m_flFlashMaxAlpha && g_proc.Read<float>(pawn + off::m_flFlashMaxAlpha) > 0.01f)
        g_proc.Write<float>(pawn + off::m_flFlashMaxAlpha, 0.f);
    if (off::m_flFlashDuration && g_proc.Read<float>(pawn + off::m_flFlashDuration) > 0.01f)
        g_proc.Write<float>(pawn + off::m_flFlashDuration, 0.f);
}

static void ApplySmokes(const std::vector<uintptr_t>& smokes, const Settings& cfg) {
    bool no_smoke = settings::Enabled(cfg.no_smoke);
    bool recolor = settings::Enabled(cfg.smoke_color_enabled) && off::m_vSmokeColor;
    float strength = std::clamp(cfg.smoke_color_strength, 10, 300) / 100.f;
    auto channel = [&](int shift) { return std::min(255.f, ((cfg.smoke_color_rgba >> shift) & 0xFF) * strength); };
    Vec3 color{channel(24), channel(16), channel(8)};
    for (uintptr_t smoke : smokes) {
        if (no_smoke) {
            if (off::m_nSmokeEffectTickBegin) WriteIfDifferent<int32_t>(smoke + off::m_nSmokeEffectTickBegin, 0);
            if (off::m_bDidSmokeEffect) WriteIfDifferent<uint8_t>(smoke + off::m_bDidSmokeEffect, 1);
        }
        if (recolor) {
            Vec3 current = g_proc.Read<Vec3>(smoke + off::m_vSmokeColor);
            if (std::fabs(current.x - color.x) > 0.5f || std::fabs(current.y - color.y) > 0.5f ||
                std::fabs(current.z - color.z) > 0.5f)
                g_proc.Write<Vec3>(smoke + off::m_vSmokeColor, color);
        }
    }
}

class NightMode {
public:
    void Apply(const WorldEntities& world, const Settings& cfg) {
        Prune(world);
        bool night = settings::Enabled(cfg.night_mode);
        float strength = night ? std::clamp(cfg.night_mode_strength, 0, 100) / 100.f : 0.f;
        float brightness = std::clamp(cfg.world_brightness, 10, 400) / 100.f;
        if (!off::m_flMinExposure || !off::m_flMaxExposure || world.post_processing.empty()) return;
        LoadOriginals(world);
        bool neutral = !night && std::fabs(brightness - 1.f) < 0.01f;
        float exposure_scale = neutral ? 1.f : brightness * std::pow(kDarkestExposureScale, strength);
        for (uintptr_t volume : world.post_processing) {
            auto found = exposure_.find(volume);
            if (found == exposure_.end()) continue;
            const ExposureBackup& original = found->second;
            if (!original.control || !(original.max_exposure > original.min_exposure) || original.min_exposure <= 0.f) continue;
            WriteIfDifferent<float>(volume + off::m_flMinExposure, original.min_exposure * exposure_scale);
            WriteIfDifferent<float>(volume + off::m_flMaxExposure, original.max_exposure * exposure_scale);
        }
    }

    void Forget() {
        exposure_.clear();
    }

    void Restore() {
        for (const auto& [volume, backup] : exposure_) {
            g_proc.Write<float>(volume + off::m_flMinExposure, backup.min_exposure);
            g_proc.Write<float>(volume + off::m_flMaxExposure, backup.max_exposure);
        }
        Forget();
    }

private:
    static std::string OriginalsPath() {
        std::string map = vis::CurrentMap();
        if (map.empty()) return {};
        const char* home = getenv("HOME");
        return std::string(home ? home : "/tmp") + "/.config/spaxer/maps/" + map + ".exposure";
    }

    void LoadOriginals(const WorldEntities& world) {
        if (exposure_.size() == world.post_processing.size()) return;
        exposure_.clear();
        std::string path = OriginalsPath();
        if (path.empty()) return;
        std::vector<ExposureBackup> saved;
        {
            std::ifstream file(path);
            file.imbue(std::locale::classic());
            ExposureBackup backup{};
            int control = 0;
            while (file >> backup.min_exposure >> backup.max_exposure >> control) {
                backup.control = control != 0;
                saved.push_back(backup);
            }
        }
        if (saved.size() != world.post_processing.size()) {
            saved.clear();
            for (uintptr_t volume : world.post_processing)
                saved.push_back({g_proc.Read<float>(volume + off::m_flMinExposure), g_proc.Read<float>(volume + off::m_flMaxExposure),
                                 off::m_bExposureControl && g_proc.Read<bool>(volume + off::m_bExposureControl)});
            {
                std::ofstream file(path);
                file.imbue(std::locale::classic());
                for (const ExposureBackup& backup : saved)
                    file << backup.min_exposure << ' ' << backup.max_exposure << ' ' << (backup.control ? 1 : 0) << '\n';
            }
        }
        for (size_t i = 0; i < saved.size(); i++) exposure_[world.post_processing[i]] = saved[i];
    }

    void Prune(const WorldEntities& world) {
        auto alive = [](const std::vector<uintptr_t>& list, uintptr_t entity) {
            return std::find(list.begin(), list.end(), entity) != list.end();
        };
        std::erase_if(exposure_, [&](const auto& entry) { return !alive(world.post_processing, entry.first); });
    }

    std::unordered_map<uintptr_t, ExposureBackup> exposure_;
};

static void EffectsLoop() {
    Settings* cfg = settings::Attach();
    if (!cfg) return;
    WorldEntities world;
    NightMode night;
    int scanned_pid = -1;
    Clock::time_point next_scan{};
    while (s_running.load()) {
        std::this_thread::sleep_for(kEffectsInterval);
        if (!g_proc.IsAlive() || !game::EntityList()) {
            world = {};
            night.Forget();
            continue;
        }
        auto now = Clock::now();
        if (now >= next_scan || scanned_pid != g_proc.pid()) {
            next_scan = now + kEntityScanInterval;
            if (scanned_pid != g_proc.pid()) night.Forget();
            scanned_pid = g_proc.pid();
            world = ScanWorldEntities();
        }
        uintptr_t pawn = game::LocalPawn();
        if (pawn && settings::Enabled(cfg->no_flash)) ApplyAntiFlash(pawn);
        ApplySmokes(world.smokes, *cfg);
        night.Apply(world, *cfg);

        if (settings::Enabled(cfg->auto_pickup) && pawn && g_hud.cs2_focused.load()) {
            std::vector<DroppedItemEntry> items;
            {
                std::lock_guard<std::mutex> lk(g_hud.items_mtx);
                items = g_hud.dropped_items;
            }
            Vec3 pos = game::Origin(pawn);
            for (const auto& item : items) {
                float dx = item.world_x - pos.x, dy = item.world_y - pos.y, dz = item.world_z - pos.z;
                if (dx * dx + dy * dy + dz * dz < 70.f * 70.f) {
                    static auto last_pickup = Clock::now();
                    if (now - last_pickup > std::chrono::milliseconds(500)) {
                        last_pickup = now;
                        g_input.SetVirtualKey(KEY_E, true);
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        g_input.SetVirtualKey(KEY_E, false);
                    }
                    break;
                }
            }
        }
    }
    night.Restore();
}

void ApplyUnsafe() {
    Settings* cfg = settings::Attach();
    if (!cfg || !g_proc.IsAlive()) return;
    uintptr_t pawn = game::LocalPawn();
    if (pawn && off::m_bIsThirdPersonView)
        WriteIfDifferent<bool>(pawn + off::m_bIsThirdPersonView, settings::Enabled(cfg->thirdperson));
}

void StartEffects() {
    if (s_running.exchange(true)) return;
    s_thread = std::thread(EffectsLoop);
}

void StopEffects() {
    if (!s_running.exchange(false)) return;
    if (s_thread.joinable()) s_thread.join();
}

}
