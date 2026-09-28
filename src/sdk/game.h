#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "memory/process.h"
#include "sdk/offsets.h"

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };

namespace game {

inline constexpr uintptr_t ENTITY_IDENTITY_SIZE = 0x70;

inline uintptr_t EntityFromList(uintptr_t entityList, int index) {
    if (index <= 0 || !entityList) return 0;
    int chunk = index >> 9;
    int entry_idx = index & 0x1FF;
    uintptr_t list_entry = g_proc.Read<uintptr_t>(entityList + 8 * chunk + 0x10);
    if (!list_entry) return 0;
    return g_proc.Read<uintptr_t>(list_entry + ENTITY_IDENTITY_SIZE * entry_idx);
}

inline uintptr_t EntityList() {
    return off::g_EntityListPtr;
}

inline uintptr_t LocalController() {
    int idx = off::g_LocalControllerIdx;
    if (idx <= 0) return 0;
    return EntityFromList(off::g_EntityListPtr, idx);
}

inline uintptr_t LocalPawn() {
    uintptr_t ctrl = LocalController();
    if (!ctrl) return 0;
    uint32_t h = g_proc.Read<uint32_t>(ctrl + off::m_hPlayerPawn);
    if (h == 0 || h == 0xFFFFFFFF) return 0;
    return EntityFromList(EntityList(), h & 0x7FFF);
}

inline Vec3 Origin(uintptr_t pawn) {
    if (!pawn) return {0,0,0};
    uintptr_t sn = g_proc.Read<uintptr_t>(pawn + off::m_pGameSceneNode);
    if (!sn) return {0,0,0};
    return g_proc.Read<Vec3>(sn + off::m_vecAbsOrigin);
}

namespace bones {
    inline constexpr int pelvis = 1;
    inline constexpr int chest  = 4;
    inline constexpr int neck   = 6;
    inline constexpr int head   = 7;
}

inline Vec3 EyePosition(uintptr_t pawn) {
    Vec3 o = Origin(pawn);
    bool crouch = off::m_fFlags && (g_proc.Read<uint32_t>(pawn + off::m_fFlags) & 2u);
    return {o.x, o.y, o.z + (crouch ? 46.f : 64.f)};
}

inline bool IsDormant(uintptr_t pawn) {
    if (!pawn || !off::m_bDormant || !off::m_pGameSceneNode) return false;
    uintptr_t node = g_proc.Read<uintptr_t>(pawn + off::m_pGameSceneNode);
    return node && g_proc.Read<bool>(node + off::m_bDormant);
}

inline bool SpottedBy(uintptr_t pawn, int controller_index, bool& known) {
    known = false;
    if (!pawn || !off::m_entitySpottedState || !off::m_bSpottedByMask) return false;
    int bit = controller_index - 1;
    if (bit < 0 || bit >= 64) return false;
    uint32_t mask[2] = {};
    if (!g_proc.ReadBytes(pawn + off::m_entitySpottedState + off::m_bSpottedByMask, mask, sizeof(mask))) return false;
    known = true;
    return (mask[bit / 32] >> (bit % 32)) & 1u;
}

inline int Team(uintptr_t entity) {
    return entity ? g_proc.Read<uint8_t>(entity + off::m_iTeamNum) : 0;
}

inline bool BoneNearOrigin(const Vec3& bone, const Vec3& origin) {
    float dx = bone.x - origin.x, dy = bone.y - origin.y, dz = bone.z - origin.z;
    return std::isfinite(dx) && std::isfinite(dy) && std::isfinite(dz) && dx * dx + dy * dy + dz * dz < 100.f * 100.f;
}

inline Vec3 BonePosition(uintptr_t pawn, int bone, float fallback_height) {
    Vec3 origin = Origin(pawn);
    Vec3 fallback{origin.x, origin.y, origin.z + fallback_height};
    if (!pawn || !off::m_pGameSceneNode || !off::m_modelState) return fallback;
    uintptr_t node = g_proc.Read<uintptr_t>(pawn + off::m_pGameSceneNode);
    uintptr_t array = node ? g_proc.Read<uintptr_t>(node + off::m_modelState + 0x80) : 0;
    if (!array) return fallback;
    Vec3 position = g_proc.Read<Vec3>(array + static_cast<uintptr_t>(bone) * 32);
    return BoneNearOrigin(position, origin) ? position : fallback;
}

inline uintptr_t ActiveWeapon(uintptr_t pawn) {
    if (!pawn || !off::m_pWeaponServices || !off::m_hActiveWeapon) return 0;
    uintptr_t services = g_proc.Read<uintptr_t>(pawn + off::m_pWeaponServices);
    uint32_t handle = services ? g_proc.Read<uint32_t>(services + off::m_hActiveWeapon) : 0;
    if (!handle || handle == 0xFFFFFFFF) return 0;
    return EntityFromList(EntityList(), handle & 0x7FFF);
}

inline int ActiveWeaponDefinitionIndex(uintptr_t pawn) {
    if (!off::m_AttributeManager || !off::m_Item || !off::m_iItemDefinitionIndex) return 0;
    uintptr_t weapon = ActiveWeapon(pawn);
    return weapon ? g_proc.Read<int>(weapon + off::m_AttributeManager + off::m_Item + off::m_iItemDefinitionIndex) : 0;
}

inline constexpr uintptr_t kEntityIdentity = 0x10;
inline constexpr uintptr_t kIdentityDesignerName = 0x20;

inline bool DesignerName(uintptr_t entity, char* out, size_t size) {
    out[0] = 0;
    uintptr_t identity = g_proc.Read<uintptr_t>(entity + kEntityIdentity);
    if (!identity) return false;
    uintptr_t name = g_proc.Read<uintptr_t>(identity + kIdentityDesignerName);
    if (!name) return false;
    for (size_t length = size - 1; length >= 8; length /= 2) {
        if (!g_proc.ReadBytes(name, out, length)) continue;
        out[length] = 0;
        return true;
    }
    return false;
}

inline bool DesignerNameIs(uintptr_t entity, const char* expected) {
    char buffer[40];
    return DesignerName(entity, buffer, sizeof(buffer)) && std::strcmp(buffer, expected) == 0;
}

struct EntitySlot {
    int index;
    uintptr_t entity;
    const char* designer;
};

inline const char* DesignerNameAt(uintptr_t name_ptr) {
    thread_local std::unordered_map<uintptr_t, std::string> cache;
    if (!name_ptr) return "";
    auto it = cache.find(name_ptr);
    if (it != cache.end()) return it->second.c_str();
    if (cache.size() > 4096) cache.clear();
    char buffer[48] = {};
    bool ok = false;
    for (size_t length = sizeof(buffer) - 1; length >= 8 && !ok; length /= 2) {
        if (g_proc.ReadBytes(name_ptr, buffer, length)) {
            buffer[length] = 0;
            ok = true;
        }
    }
    if (!ok) return "";
    return cache.emplace(name_ptr, buffer).first->second.c_str();
}

inline std::vector<EntitySlot> EntitySnapshot(uintptr_t list, int from, int to) {
    std::vector<EntitySlot> slots;
    if (!list) return slots;
    thread_local std::vector<uint8_t> chunk_data(ENTITY_IDENTITY_SIZE * 512);
    for (int chunk = from >> 9; chunk <= (to - 1) >> 9; chunk++) {
        uintptr_t chunk_base = g_proc.Read<uintptr_t>(list + 8 * chunk + 0x10);
        if (!chunk_base || !g_proc.ReadBytes(chunk_base, chunk_data.data(), chunk_data.size())) continue;
        int first = std::max(from, chunk << 9), last = std::min(to, (chunk + 1) << 9);
        for (int index = first; index < last; index++) {
            const uint8_t* identity = chunk_data.data() + ENTITY_IDENTITY_SIZE * (index & 0x1FF);
            uintptr_t entity, name_ptr;
            std::memcpy(&entity, identity, sizeof(entity));
            std::memcpy(&name_ptr, identity + kIdentityDesignerName, sizeof(name_ptr));
            if (!entity) continue;
            slots.push_back({index, entity, DesignerNameAt(name_ptr)});
        }
    }
    return slots;
}

inline uintptr_t PlantedC4() {
    if (!off::g_EntityListPtr || !off::m_bBombTicking) return 0;
    for (const EntitySlot& slot : EntitySnapshot(off::g_EntityListPtr, 65, 2048))
        if (!std::strcmp(slot.designer, "planted_c4")) return slot.entity;
    return 0;
}

}
