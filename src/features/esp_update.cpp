#include "state.h"
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

static void ReadWeapon(uintptr_t list, uintptr_t pawn, char weapon[24]) {
    if (!list || !pawn || !off::m_pWeaponServices || !off::m_hActiveWeapon ||
        !off::m_AttributeManager || !off::m_Item || !off::m_iItemDefinitionIndex) return;
    uintptr_t services = g_proc.Read<uintptr_t>(pawn + off::m_pWeaponServices);
    uint32_t handle = services ? g_proc.Read<uint32_t>(services + off::m_hActiveWeapon) : 0;
    uintptr_t entity = game::EntityFromList(list, handle & 0x7FFF);
    int definition = entity ? g_proc.Read<int>(entity + off::m_AttributeManager + off::m_Item + off::m_iItemDefinitionIndex) : 0;
    const char* name = WeaponName(definition);
    if (name[0]) strncpy(weapon, name, 23);
}

static void UpdateSpectators(uintptr_t list, uintptr_t local_pawn) {
    if (!list || !local_pawn || !off::m_pObserverServices || !off::m_hObserverTarget) {
        ClearSpectators();
        return;
    }
    std::vector<SpectatorEntry> out;
    out.reserve(16);
    for (int i = 1; i <= 64; i++) {
        uintptr_t controller = game::EntityFromList(list, i);
        if (!controller) continue;
        uint32_t pawn_handle = g_proc.Read<uint32_t>(controller + off::m_hPlayerPawn);
        uintptr_t pawn = game::EntityFromList(list, pawn_handle & 0x7FFF);
        if (!pawn || pawn == local_pawn) continue;
        uintptr_t observer = g_proc.Read<uintptr_t>(pawn + off::m_pObserverServices);
        uint32_t target_handle = observer ? g_proc.Read<uint32_t>(observer + off::m_hObserverTarget) : 0;
        uintptr_t target = game::EntityFromList(list, target_handle & 0x7FFF);
        if (target != local_pawn) continue;
        SpectatorEntry entry{};
        entry.team = game::Team(pawn);
        ReadPlayerName(controller, entry.name);
        if (entry.name[0]) out.push_back(entry);
    }
    std::lock_guard<std::mutex> lock(g_hud.spectators_mtx);
    g_hud.spectators.swap(out);
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
        ReadWeapon(list, pawn, e.weapon);
        e.pawn = pawn;
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
        for (int i = 64; i < 1024; i++) {
            uintptr_t entity = game::EntityFromList(list, i);
            if (!entity) continue;
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

    if (cfg && settings::Enabled(cfg->grenade_trajectory) && local_pawn && off::m_angEyeAngles) {
        int act_idx = game::ActiveWeaponDefinitionIndex(local_pawn);
        bool is_grenade = (act_idx >= 43 && act_idx <= 48);
        if (is_grenade) {
            constexpr float rad = 3.14159265358979323846f / 180.f;
            Vec3 eye_pos = game::EyePosition(local_pawn);
            Vec3 eye_ang = g_proc.Read<Vec3>(local_pawn + off::m_angEyeAngles);
            float pitch = eye_ang.x;
            if (pitch < -90.f) pitch += 360.f;
            else if (pitch > 90.f) pitch -= 360.f;
            pitch -= (90.f - std::fabs(pitch)) * 10.f / 90.f;
            float cp = cosf(pitch * rad), sp = sinf(pitch * rad);
            float cy = cosf(eye_ang.y * rad), sy = sinf(eye_ang.y * rad);
            Vec3 forward{cp * cy, cp * sy, -sp};
            Vec3 player_vel = off::m_vecVelocity ? g_proc.Read<Vec3>(local_pawn + off::m_vecVelocity) : Vec3{};
            constexpr float throw_speed = 675.f, gravity = 320.f, dt = 0.01f, elasticity = 0.45f;
            bool timed = act_idx == 43 || act_idx == 44;
            bool fire = act_idx == 46 || act_idx == 48;
            float fuse_time = timed ? 1.5f : 3.0f;
            float floor_z = game::Origin(local_pawn).z;
            Vec3 vel{forward.x * throw_speed + player_vel.x * 1.25f,
                     forward.y * throw_speed + player_vel.y * 1.25f,
                     forward.z * throw_speed + player_vel.z * 1.25f};
            Vec3 curr{eye_pos.x + forward.x * 16.f, eye_pos.y + forward.y * 16.f, eye_pos.z + forward.z * 16.f};
            std::vector<GrenadePoint> path;
            path.reserve(320);
            path.push_back({curr.x, curr.y, curr.z, false});
            for (float t = 0.f; t < fuse_time; t += dt) {
                curr.x += vel.x * dt;
                curr.y += vel.y * dt;
                curr.z += vel.z * dt - 0.5f * gravity * dt * dt;
                vel.z -= gravity * dt;
                if (curr.z <= floor_z && vel.z < 0.f) {
                    curr.z = floor_z;
                    path.push_back({curr.x, curr.y, curr.z, true});
                    if (fire) break;
                    vel.x *= elasticity;
                    vel.y *= elasticity;
                    vel.z = -vel.z * elasticity;
                    if (vel.x * vel.x + vel.y * vel.y + vel.z * vel.z < 20.f * 20.f) break;
                    continue;
                }
                path.push_back({curr.x, curr.y, curr.z, false});
            }
            std::lock_guard<std::mutex> lk_gr(g_hud.grenade_mtx);
            g_hud.grenade_path.swap(path);
        } else {
            std::lock_guard<std::mutex> lk_gr(g_hud.grenade_mtx);
            g_hud.grenade_path.clear();
        }
    } else {
        std::lock_guard<std::mutex> lk_gr(g_hud.grenade_mtx);
        g_hud.grenade_path.clear();
    }
}

}
