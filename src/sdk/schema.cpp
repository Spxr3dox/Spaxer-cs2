#include "schema.h"
#include "interfaces.h"
#include "../core/core.h"
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unistd.h>

std::vector<SchemaField> SchemaDumpClass(void* scope, const char* className) {
    std::vector<SchemaField> out;
    void* classInfo = nullptr;
    for (int ci : {2, 3, 4, 5}) {
        classInfo = SdkCallV(scope, ci, className);
        if (classInfo) break;
    }
    if (!classInfo) return out;

    for (int fcOff : {0x1C, 0x20, 0x24, 0x28}) {
        int16_t fieldCount = SdkReadSafe<int16_t>((uintptr_t)classInfo + fcOff);
        if (fieldCount <= 0 || fieldCount > 2000) continue;
        for (int fpOff : {fcOff + 0x8, fcOff + 0xC, fcOff + 0x10}) {
            uintptr_t fieldsPtr = SdkReadSafe<uintptr_t>((uintptr_t)classInfo + fpOff);
            if (fieldsPtr < 0x10000) continue;
            std::string test = SdkSafeStr(SdkReadSafe<uintptr_t>(fieldsPtr));
            if (test.empty() || test[0] != 'm') continue;
            for (int i = 0; i < fieldCount && i < 500; i++) {
                uintptr_t fAddr = fieldsPtr + i * 0x20;
                uintptr_t namePtr = SdkReadSafe<uintptr_t>(fAddr);
                int32_t fOff = SdkReadSafe<int32_t>(fAddr + 0x10);
                std::string name = SdkSafeStr(namePtr);
                if (name.empty()) continue;
                uintptr_t typePtr = SdkReadSafe<uintptr_t>(fAddr + 0x8);
                std::string type = typePtr > 0x10000 ? SdkSafeStr(SdkReadSafe<uintptr_t>(typePtr + 0x8)) : "";
                out.push_back({name, (uint32_t)fOff, type});
            }
            if (!out.empty()) return out;
        }
    }
    return out;
}

bool SchemaDoDump() {
    if (!SdkLoadFactories()) return false;
    void* schema = SdkGetIface("SchemaSystem_001");
    if (!schema) return false;

    static const char* mods[] = {"libclient.so", "client.dll", nullptr};
    void* scope = nullptr;
    for (int mi = 0; mods[mi] && !scope; mi++) {
        for (int idx : {13, 14, 12, 11, 15, 16, 10}) {
            scope = SdkCallV(schema, idx, mods[mi]);
            if (scope) { Log("[schema] scope @ %p vt[%d] '%s'", scope, idx, mods[mi]); break; }
        }
    }
    if (!scope) return false;

    static const char* kClasses[] = {
        "CEntityInstance", "C_BaseEntity", "C_BaseModelEntity", "C_BaseFlex", "C_BaseAnimGraph",
        "C_BasePlayerPawn", "C_CSPlayerPawnBase", "C_CSPlayerPawn", "CBasePlayerController",
        "CCSPlayerController", "C_EconEntity", "C_BasePlayerWeapon", "C_CSWeaponBase",
        "C_CSWeaponBaseGun", "C_C4", "C_PlantedC4", "C_BaseGrenade", "C_BaseCSGrenade",
        "C_BaseCSGrenadeProjectile", "C_SmokeGrenadeProjectile", "C_MolotovProjectile", "C_Inferno",
        "C_CSGameRules", "C_CSGameRulesProxy", "C_Team", "C_CSTeam", "CGameSceneNode",
        "CSkeletonInstance", "CModelState", "CBodyComponent", "CPlayer_WeaponServices",
        "CCSPlayer_WeaponServices", "CPlayer_ItemServices", "CCSPlayer_ItemServices",
        "CPlayer_MovementServices", "CCSPlayer_MovementServices", "CPlayer_ObserverServices",
        "CPlayer_CameraServices", "CCSPlayer_CameraServices", "CCSPlayerBase_CameraServices",
        "CCSPlayer_PingServices", "CCSPlayerController_InGameMoneyServices",
        "CCSPlayerController_ActionTrackingServices", "CCSPlayerController_InventoryServices",
        "C_EconItemView", "C_AttributeContainer", "CGlowProperty", "EntitySpottedState_t",
        "C_PostProcessingVolume", "C_FogController", "C_EnvSky", "C_SkyCamera",
    };

    char tmp[64];
    snprintf(tmp, sizeof(tmp), "/tmp/schema_full.txt.tmp.%d", getpid());
    FILE* out = fopen(tmp, "w");
    if (!out) return false;
    size_t total = 0;
    for (const char* cls : kClasses) {
        for (const auto& f : SchemaDumpClass(scope, cls)) {
            fprintf(out, "%s %s %u %s\n", cls, f.name.c_str(), f.offset, f.type.empty() ? "?" : f.type.c_str());
            total++;
        }
    }
    fclose(out);
    rename(tmp, "/tmp/schema_full.txt");
    Log("[schema] wrote /tmp/schema_full.txt with %zu fields", total);
    return total > 0;
}
