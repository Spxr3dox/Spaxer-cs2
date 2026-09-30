#include "state.h"
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include "sdk/game.h"
#include "memory/process.h"
#include "sdk/offsets.h"
#include "config/settings.h"
#include <cstring>
#include <cmath>
#include <cctype>

namespace features {

static void ClearEsp() {
    std::lock_guard<std::mutex> lock(g_hud.esp_mtx);
    g_hud.esp_players.clear();
}

static void ClearSpectators() {
    std::lock_guard<std::mutex> lock(g_hud.spectators_mtx);
    g_hud.spectators.clear();
}

static bool IsText(const char* text, size_t size) {
    for (size_t i = 0; i < size && text[i]; i++) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 32 || c == 127) return false;
    }
    return text[0] != 0;
}

static bool ModelStemFromPath(const std::string& path, char out[40]) {
    size_t end = path.find(".vmdl");
    if (end == std::string::npos) return false;
    size_t start = path.rfind('/', end);
    start = start == std::string::npos ? 0 : start + 1;
    if (end <= start || end - start >= 40) return false;
    memcpy(out, path.data() + start, end - start);
    out[end - start] = 0;
    return true;
}

static bool TryModelName(uintptr_t model_state, uintptr_t offset, int indirection, char out[40]) {
    uintptr_t pointer = g_proc.Read<uintptr_t>(model_state + offset);
    if (pointer < 0x10000 || pointer > 0x7FFFFFFFFFFF) return false;
    if (indirection) {
        pointer = g_proc.Read<uintptr_t>(pointer + 8);
        if (pointer < 0x10000 || pointer > 0x7FFFFFFFFFFF) return false;
    }
    std::string path = g_proc.ReadString(pointer, 127);
    return path.find("models/") != std::string::npos && ModelStemFromPath(path, out);
}

static void ReadModelStem(uintptr_t scene_node, char out[40]) {
    static uintptr_t s_offset = 0;
    static int s_indirection = -1;
    out[0] = 0;
    if (!scene_node || !off::m_modelState) return;
    uintptr_t model_state = scene_node + off::m_modelState;
    if (s_indirection >= 0 && TryModelName(model_state, s_offset, s_indirection, out)) return;
    static int s_scan_attempts = 0;
    if (s_scan_attempts++ % 32 != 0) return;
    for (int indirection = 0; indirection < 2; indirection++) {
        for (uintptr_t offset = 0; offset < 0x300; offset += 8) {
            if (TryModelName(model_state, offset, indirection, out)) {
                s_offset = offset;
                s_indirection = indirection;
                return;
            }
        }
    }
}

static void ReadPlayerName(uintptr_t controller, char name[32]) {
    if (!controller || !off::m_iszPlayerName) return;
    char direct[32]{};
    if (g_proc.ReadBytes(controller + off::m_iszPlayerName, direct, sizeof(direct) - 1) && IsText(direct, sizeof(direct))) {
        memcpy(name, direct, sizeof(direct) - 1);
        return;
    }
    uintptr_t pointer = g_proc.Read<uintptr_t>(controller + off::m_iszPlayerName);
    if (!pointer) return;
    std::string value = g_proc.ReadString(pointer, 31);
    if (IsText(value.c_str(), value.size() + 1)) snprintf(name, 32, "%s", value.c_str());
}

static const char* WeaponName(int definition) {
    switch (definition) {
        case 1: return "Desert Eagle";
        case 2: return "Dual Berettas";
        case 3: return "Five-SeveN";
        case 4: return "Glock-18";
        case 7: return "AK-47";
        case 8: return "AUG";
        case 9: return "AWP";
        case 10: return "FAMAS";
        case 11: return "G3SG1";
        case 13: return "Galil AR";
        case 14: return "M249";
        case 16: return "M4A4";
        case 17: return "MAC-10";
        case 19: return "P90";
        case 23: return "MP5-SD";
        case 24: return "UMP-45";
        case 25: return "XM1014";
        case 26: return "PP-Bizon";
        case 27: return "MAG-7";
        case 28: return "Negev";
        case 29: return "Sawed-Off";
        case 30: return "Tec-9";
        case 31: return "Zeus";
        case 32: return "P2000";
        case 33: return "MP7";
        case 34: return "MP9";
        case 35: return "Nova";
        case 36: return "P250";
        case 38: return "SCAR-20";
        case 39: return "SG 553";
        case 40: return "SSG 08";
        case 42: return "Knife";
        case 43: return "Flashbang";
        case 44: return "HE Grenade";
        case 45: return "Smoke";
        case 46: return "Molotov";
        case 47: return "Decoy";
        case 48: return "Incendiary";
        case 49: return "C4";
        case 57: return "Healthshot";
        case 60: return "M4A1-S";
        case 61: return "USP-S";
        case 63: return "CZ75-Auto";
        case 64: return "R8 Revolver";
        default: return "";
    }
}

static int MagazineSize(int definition) {
    switch (definition) {
        case 1: return 7;
        case 2: return 30;
        case 3: return 20;
        case 4: return 20;
        case 7: return 30;
        case 8: return 30;
        case 9: return 5;
        case 10: return 25;
        case 11: return 20;
        case 13: return 35;
        case 14: return 100;
        case 16: return 30;
        case 17: return 30;
        case 19: return 50;
        case 23: return 30;
        case 24: return 25;
        case 25: return 7;
        case 26: return 64;
        case 27: return 5;
        case 28: return 150;
        case 29: return 7;
        case 30: return 18;
        case 32: return 13;
        case 33: return 30;
        case 34: return 30;
        case 35: return 8;
        case 36: return 13;
        case 38: return 20;
        case 39: return 30;
        case 40: return 10;
        case 60: return 20;
        case 61: return 12;
        case 63: return 12;
        case 64: return 8;
        default: return 0;
    }
}

static void ReadWeapon(uintptr_t list, uintptr_t pawn, EspEntry& entry) {
    if (!list || !pawn || !off::m_AttributeManager || !off::m_Item || !off::m_iItemDefinitionIndex) return;
    uintptr_t weapon = game::ActiveWeapon(pawn);
    int definition = weapon ? g_proc.Read<int>(weapon + off::m_AttributeManager + off::m_Item + off::m_iItemDefinitionIndex) : 0;
    const char* name = WeaponName(definition);
    if (name[0]) strncpy(entry.weapon, name, sizeof(entry.weapon) - 1);
    entry.max_ammo = MagazineSize(definition);
    entry.ammo = entry.max_ammo && off::m_iClip1 ? std::clamp(g_proc.Read<int>(weapon + off::m_iClip1), 0, entry.max_ammo) : 0;
}

static void UpdateSpectators(uintptr_t list, uintptr_t local_pawn) {
    if (!list || !local_pawn) {
        ClearSpectators();
        return;
    }
    uintptr_t local_ctrl = game::LocalController();
    uint32_t local_pawn_h = local_ctrl ? g_proc.Read<uint32_t>(local_ctrl + off::m_hPlayerPawn) : 0;
    uint32_t local_idx = (local_pawn_h && local_pawn_h != 0xFFFFFFFF) ? (local_pawn_h & 0x7FFF) : 0;
    uintptr_t obs_srv_off = off::m_pObserverServices ? off::m_pObserverServices : 0x1290;
    uintptr_t obs_tgt_off = off::m_hObserverTarget ? off::m_hObserverTarget : 0x4C;

    std::vector<SpectatorEntry> out;
    out.reserve(16);
    for (int i = 1; i <= 64; i++) {
        uintptr_t controller = game::EntityFromList(list, i);
        if (!controller || controller == local_ctrl) continue;
        uint32_t pawn_handle = g_proc.Read<uint32_t>(controller + off::m_hPlayerPawn);
        if (!pawn_handle || pawn_handle == 0xFFFFFFFF) continue;
        uint32_t p_idx = pawn_handle & 0x7FFF;
        if (p_idx == local_idx) continue;
        uintptr_t pawn = game::EntityFromList(list, p_idx);
        if (!pawn || pawn == local_pawn) continue;

        uintptr_t observer = g_proc.Read<uintptr_t>(pawn + obs_srv_off);
        if (!observer || observer < 0x10000 || observer > 0x7FFFFFFFFFFF) continue;

        uint32_t target_handle = g_proc.Read<uint32_t>(observer + obs_tgt_off);
        if (!target_handle || target_handle == 0xFFFFFFFF) continue;
        uint32_t target_idx = target_handle & 0x7FFF;

        bool is_spectating = (local_idx && target_idx == local_idx);
        if (!is_spectating) {
            uintptr_t target = game::EntityFromList(list, target_idx);
            if (target && target == local_pawn) is_spectating = true;
        }

        if (is_spectating) {
            SpectatorEntry entry{};
            entry.team = game::Team(pawn);
            if (!entry.team) entry.team = g_proc.Read<uint8_t>(controller + off::m_iTeamNum);
            ReadPlayerName(controller, entry.name);
            if (entry.name[0]) out.push_back(entry);
        }
    }
    std::lock_guard<std::mutex> lock(g_hud.spectators_mtx);
    g_hud.spectators.swap(out);
}

static double NowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static bool GrenadeKindFor(const char* name, GrenadeKind& kind) {
    if (!strcmp(name, "hegrenade_projectile")) kind = GrenadeKind::He;
    else if (!strcmp(name, "flashbang_projectile")) kind = GrenadeKind::Flash;
    else if (!strcmp(name, "smokegrenade_projectile")) kind = GrenadeKind::Smoke;
    else if (!strcmp(name, "molotov_projectile") || !strcmp(name, "incendiary_projectile") || !strcmp(name, "inferno")) kind = GrenadeKind::Fire;
    else if (!strcmp(name, "decoy_projectile")) kind = GrenadeKind::Decoy;
    else return false;
    return true;
}

static int ScanWorldObjects(uintptr_t list) {
    static std::unordered_map<uintptr_t, double> first_seen;
    int bomb_carrier = -1;
    std::vector<ThrownGrenade> grenades;
    double now = NowSeconds();
    for (const game::EntitySlot& slot : game::EntitySnapshot(list, 65, 2048)) {
        uintptr_t entity = slot.entity;
        const char* name = slot.designer;
        if (!name[0]) continue;
        if (!strcmp(name, "weapon_c4")) {
            uint32_t owner = off::m_hOwnerEntity ? g_proc.Read<uint32_t>(entity + off::m_hOwnerEntity) : 0xFFFFFFFF;
            if (owner && owner != 0xFFFFFFFF) bomb_carrier = static_cast<int>(owner & 0x7FFF);
            continue;
        }
        GrenadeKind kind;
        if (!GrenadeKindFor(name, kind)) continue;
        int team = 0;
        if (off::m_hThrower) {
            uint32_t thrower = g_proc.Read<uint32_t>(entity + off::m_hThrower);
            uintptr_t pawn = thrower && thrower != 0xFFFFFFFF ? game::EntityFromList(list, thrower & 0x7FFF) : 0;
            team = pawn ? game::Team(pawn) : 0;
        }
        auto [it, inserted] = first_seen.try_emplace(entity, now);
        grenades.push_back({entity, kind, team, it->second});
    }
    std::erase_if(first_seen, [&](const auto& entry) {
        return std::none_of(grenades.begin(), grenades.end(), [&](const ThrownGrenade& g) { return g.entity == entry.first; });
    });
    std::lock_guard<std::mutex> lock(g_hud.grenades_mtx);
    g_hud.thrown_grenades.swap(grenades);
    return bomb_carrier;
}

static uint32_t ReadFlags(uintptr_t list, uintptr_t pawn, bool has_bomb) {
    struct FlashState { float duration = 0.f; double until = 0.0; };
    static std::unordered_map<uintptr_t, FlashState> flashes;
    uint32_t flags = has_bomb ? kFlagBomb : 0u;
    double now = NowSeconds();
    if (off::m_flFlashDuration) {
        float duration = g_proc.Read<float>(pawn + off::m_flFlashDuration);
        FlashState& state = flashes[pawn];
        if (duration > 0.1f && std::fabs(duration - state.duration) > 0.01f) state.until = now + duration;
        state.duration = duration;
        if (now < state.until) flags |= kFlagFlashed;
    }
    if (off::m_bIsScoped && g_proc.Read<bool>(pawn + off::m_bIsScoped)) flags |= kFlagScoped;
    if (off::m_bIsDefusing && g_proc.Read<bool>(pawn + off::m_bIsDefusing)) flags |= kFlagDefusing;
    if (off::m_ArmorValue && g_proc.Read<int>(pawn + off::m_ArmorValue) > 0) flags |= kFlagArmor;
    if (off::m_pItemServices) {
        uintptr_t items = g_proc.Read<uintptr_t>(pawn + off::m_pItemServices);
        if (items && off::m_bHasHelmet && g_proc.Read<bool>(items + off::m_bHasHelmet)) flags |= kFlagHelmet;
        if (items && off::m_bHasDefuser && g_proc.Read<bool>(items + off::m_bHasDefuser)) flags |= kFlagKit;
    }
    if (off::m_bInReload) {
        uintptr_t weapon = game::ActiveWeapon(pawn);
        if (weapon && g_proc.Read<bool>(weapon + off::m_bInReload)) flags |= kFlagReloading;
    }
    return flags;
}

void UpdateEsp() {
    if (!g_hud.attached.load() || !off::g_OffsetsReady.load()) {
        ClearEsp();
        ClearSpectators();
        return;
    }

    uintptr_t list = game::EntityList();
    if (!list || !off::m_pGameSceneNode || !off::m_vecAbsOrigin) {
        ClearEsp();
        ClearSpectators();
        return;
    }

    uintptr_t local_pawn = game::LocalPawn();

    std::vector<EspEntry> out;
    out.reserve(32);

    int my_team = 0;
    if (local_pawn) my_team = game::Team(local_pawn);
    g_hud.local_team.store(my_team);

    int bomb_carrier = ScanWorldObjects(list);

    for (int i = 1; i <= 64; i++) {
        uintptr_t ctrl = game::EntityFromList(list, i);
        if (!ctrl) continue;
        uint32_t ph = g_proc.Read<uint32_t>(ctrl + off::m_hPlayerPawn);
        if (ph == 0 || ph == 0xFFFFFFFF) continue;
        uintptr_t pawn = game::EntityFromList(list, ph & 0x7FFF);
        if (!pawn) continue;
        if (pawn == local_pawn || game::IsDormant(pawn)) continue;

        int hp = g_proc.Read<int>(pawn + off::m_iHealth);
        if (hp <= 0 || hp > 100) continue;
        int team = game::Team(pawn);
        if (team != 2 && team != 3) team = 0;

        uintptr_t sn = g_proc.Read<uintptr_t>(pawn + off::m_pGameSceneNode);
        if (!sn) continue;
        Vec3 origin = g_proc.Read<Vec3>(sn + off::m_vecAbsOrigin);
        if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
            std::fabs(origin.x) > 20000.f || std::fabs(origin.y) > 20000.f ||
            std::fabs(origin.z) > 20000.f) continue;
        float head_height = 68.f;
        if (off::m_fFlags && (g_proc.Read<uint32_t>(pawn + off::m_fFlags) & 2u))
            head_height = 50.f;
        Vec3 head{ origin.x, origin.y, origin.z + head_height };

        EspEntry e{};
        e.world_head_x = head.x; e.world_head_y = head.y; e.world_head_z = head.z;
        e.world_feet_x = origin.x; e.world_feet_y = origin.y; e.world_feet_z = origin.z;
        e.hp = hp; e.team = team;
        e.visible = game::SpottedBy(pawn, off::g_LocalControllerIdx, e.spotted_valid);

        ReadPlayerName(ctrl, e.name);
        ReadWeapon(list, pawn, e);
        e.pawn = pawn;
        e.flags = ReadFlags(list, pawn, static_cast<int>(ph & 0x7FFF) == bomb_carrier);
        ReadModelStem(sn, e.model);
        out.push_back(e);
        if (out.size() >= 32) break;
    }

    std::lock_guard<std::mutex> lk(g_hud.esp_mtx);
    g_hud.esp_players.swap(out);
    UpdateSpectators(list, local_pawn);

    Settings* cfg = settings::Attach();
    if (cfg && (settings::Enabled(cfg->esp_dropped_weapons) || settings::Enabled(cfg->radar))) {
        std::vector<Vec3> holders;
        holders.reserve(32);
        for (int i = 1; i <= 64; i++) {
            uintptr_t controller = game::EntityFromList(list, i);
            uint32_t handle = controller ? g_proc.Read<uint32_t>(controller + off::m_hPlayerPawn) : 0;
            uintptr_t pawn = handle && handle != 0xFFFFFFFF ? game::EntityFromList(list, handle & 0x7FFF) : 0;
            if (pawn && g_proc.Read<int>(pawn + off::m_iHealth) > 0) holders.push_back(game::Origin(pawn));
        }
        std::vector<DroppedItemEntry> items;
        items.reserve(32);
        for (const game::EntitySlot& slot : game::EntitySnapshot(list, 64, 1024)) {
            uintptr_t entity = slot.entity;
            if (strncmp(slot.designer, "weapon_", 7) != 0) continue;
            int def = (off::m_AttributeManager && off::m_Item && off::m_iItemDefinitionIndex) ?
                g_proc.Read<int>(entity + off::m_AttributeManager + off::m_Item + off::m_iItemDefinitionIndex) : 0;
            const char* wname = WeaponName(def);
            if (!wname[0]) continue;
            if (off::m_hOwnerEntity) {
                uint32_t owner = g_proc.Read<uint32_t>(entity + off::m_hOwnerEntity);
                if (owner != 0xFFFFFFFF && owner != 0) continue;
            }
            uintptr_t sn = g_proc.Read<uintptr_t>(entity + off::m_pGameSceneNode);
            if (!sn) continue;
            Vec3 pos = g_proc.Read<Vec3>(sn + off::m_vecAbsOrigin);
            if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) ||
                std::fabs(pos.x) > 20000.f || std::fabs(pos.y) > 20000.f || std::fabs(pos.z) > 20000.f) continue;
            if (pos.x == 0.f && pos.y == 0.f && pos.z == 0.f) continue;
            bool held = false;
            for (const Vec3& h : holders) {
                float dx = pos.x - h.x, dy = pos.y - h.y, dz = pos.z - h.z;
                if (dx * dx + dy * dy < 48.f * 48.f && dz > -10.f && dz < 90.f) { held = true; break; }
            }
            if (held) continue;
            DroppedItemEntry it{};
            it.world_x = pos.x; it.world_y = pos.y; it.world_z = pos.z;
            strncpy(it.name, wname, sizeof(it.name) - 1);
            items.push_back(it);
            if (items.size() >= 48) break;
        }
        std::lock_guard<std::mutex> lk_it(g_hud.items_mtx);
        g_hud.dropped_items.swap(items);
    } else {
        std::lock_guard<std::mutex> lk_it(g_hud.items_mtx);
        g_hud.dropped_items.clear();
    }

}

}
