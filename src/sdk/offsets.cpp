#include "offsets.h"
#include "memory/process.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace off {

static std::string g_json_path = "offsets.json";

static bool ParseHex(const char* s, uintptr_t& out) {
    while (*s && (isspace(static_cast<unsigned char>(*s)) || *s == '"')) ++s;
    bool negative = *s == '-';
    if (negative) ++s;
    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    if (!*s) return false;
    char* end = nullptr;
    errno = 0;
    unsigned long long value = strtoull(s, &end, base);
    if (end == s || errno == ERANGE) return false;
    while (*end && (isspace(static_cast<unsigned char>(*end)) || *end == ',' ||
                    *end == '}' || *end == '"' || *end == ';')) {
        ++end;
    }
    if (*end) return false;
    uintptr_t parsed = static_cast<uintptr_t>(value);
    out = negative ? ~parsed + 1 : parsed;
    return true;
}

struct OffsetEntry {
    const char* name;
    uintptr_t* value;
};

static const OffsetEntry kOffsetTable[] = {
    {"dwViewMatrix", &dwViewMatrix},
    {"dwWindowWidth", &dwWindowWidth},
    {"dwWindowHeight", &dwWindowHeight},
    {"g_EntityListPtr", &g_EntityListPtr},
    {"m_iHealth", &m_iHealth},
    {"m_iTeamNum", &m_iTeamNum},
    {"m_hPlayerPawn", &m_hPlayerPawn},
    {"m_iIDEntIndex", &m_iIDEntIndex},
    {"m_iPing", &m_iPing},
    {"m_iszPlayerName", &m_iszPlayerName},
    {"m_lifeState", &m_lifeState},
    {"m_fFlags", &m_fFlags},
    {"m_pGameSceneNode", &m_pGameSceneNode},
    {"m_modelState", &m_modelState},
    {"m_pWeaponServices", &m_pWeaponServices},
    {"m_hActiveWeapon", &m_hActiveWeapon},
    {"m_AttributeManager", &m_AttributeManager},
    {"m_Item", &m_Item},
    {"m_iItemDefinitionIndex", &m_iItemDefinitionIndex},
    {"m_bIsLocalPlayerController", &m_bIsLocalPlayerController},
    {"m_vecAbsOrigin", &m_vecAbsOrigin},
    {"m_angEyeAngles", &m_angEyeAngles},
    {"m_aimPunchAngle", &m_aimPunchAngle},
    {"m_aimPunchCache", &m_aimPunchCache},
    {"m_iShotsFired", &m_iShotsFired},
    {"m_bIsScoped", &m_bIsScoped},
    {"m_iClip1", &m_iClip1},
    {"m_pCameraServices", &m_pCameraServices},
    {"m_iFOV", &m_iFOV},
    {"m_bIsThirdPersonView", &m_bIsThirdPersonView},
    {"m_flFlashMaxAlpha", &m_flFlashMaxAlpha},
    {"m_flFlashDuration", &m_flFlashDuration},
    {"m_vecVelocity", &m_vecVelocity},
    {"m_bDidSmokeEffect", &m_bDidSmokeEffect},
    {"m_vSmokeColor", &m_vSmokeColor},
    {"m_flThrowStrength", &m_flThrowStrength},
    {"m_bPinPulled", &m_bPinPulled},
    {"m_nSmokeEffectTickBegin", &m_nSmokeEffectTickBegin},
    {"m_flMinExposure", &m_flMinExposure},
    {"m_flMaxExposure", &m_flMaxExposure},
    {"m_bExposureControl", &m_bExposureControl},
    {"m_Glow", &m_Glow},
    {"m_clrRender", &m_clrRender},
    {"m_nRenderMode", &m_nRenderMode},
    {"m_fEffects", &m_fEffects},
    {"m_hOwnerEntity", &m_hOwnerEntity},
    {"m_bDormant", &m_bDormant},
    {"m_iGlowType", &m_iGlowType},
    {"m_glowColorOverride", &m_glowColorOverride},
    {"m_bGlowing", &m_bGlowing},
    {"m_entitySpottedState", &m_entitySpottedState},
    {"m_bSpotted", &m_bSpotted},
    {"m_bSpottedByMask", &m_bSpottedByMask},
    {"m_pReserveAmmo", &m_pReserveAmmo},
    {"m_pObserverServices", &m_pObserverServices},
    {"m_hObserverTarget", &m_hObserverTarget},
    {"m_iObserverMode", &m_iObserverMode},
    {"m_bBombTicking", &m_bBombTicking},
    {"m_bBombDefused", &m_bBombDefused},
    {"m_bBeingDefused", &m_bBeingDefused},
    {"m_nBombSite", &m_nBombSite},
    {"m_flTimerLength", &m_flTimerLength},
    {"m_flDefuseLength", &m_flDefuseLength},
    {"m_flC4Blow", &m_flC4Blow},
    {"m_flDefuseCountDown", &m_flDefuseCountDown},
};

static void AssignKey(const std::string& key, uintptr_t value) {
    if (key == "LocalControllerIdx") {
        g_LocalControllerIdx = static_cast<int>(value);
        return;
    }
    for (const OffsetEntry& entry : kOffsetTable) {
        if (key == entry.name) {
            *entry.value = value;
            return;
        }
    }
}

bool Get(const std::string& key, uintptr_t& out) {
    for (const OffsetEntry& entry : kOffsetTable) {
        if (key == entry.name) {
            out = *entry.value;
            return true;
        }
    }
    return false;
}

std::vector<std::string> Names() {
    std::vector<std::string> names;
    for (const OffsetEntry& entry : kOffsetTable) names.emplace_back(entry.name);
    return names;
}

bool LoadFromJson(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    std::string buf;
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) buf.append(tmp, n);
    fclose(f);

    size_t i = 0;
    while (i < buf.size()) {
        if (buf[i] != '"') { i++; continue; }
        size_t ks = ++i;
        while (i < buf.size() && buf[i] != '"') i++;
        if (i >= buf.size()) break;
        std::string key = buf.substr(ks, i - ks);
        i++;
        while (i < buf.size() && buf[i] != ':' && buf[i] != ',' && buf[i] != '}') i++;
        if (i >= buf.size() || buf[i] != ':') continue;
        i++;
        while (i < buf.size() && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r')) i++;
        if (i >= buf.size()) break;
        if (buf[i] == '{' || buf[i] == '[') continue;
        size_t vs = i;
        while (i < buf.size() && buf[i] != ',' && buf[i] != '}' && buf[i] != '\n') i++;
        std::string val = buf.substr(vs, i - vs);
        uintptr_t v = 0;
        if (ParseHex(val.c_str(), v)) AssignKey(key, v);
    }
    return true;
}

bool LoadFromPseFile() {
    FILE* f = fopen("/tmp/offsets_dump.h", "r");
    if (!f) return false;
    char line[512];
    int dump_pid = 0;
    std::vector<std::pair<std::string, uintptr_t>> values;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '/' && line[1] == '/') {
            unsigned parsed_pid = 0;
            if (sscanf(line, "// CS2 runtime dump (PID %u)", &parsed_pid) == 1)
                dump_pid = static_cast<int>(parsed_pid);
            continue;
        }
        char* eq = strchr(line, '=');
        if (!eq) continue;
        std::string key(line, eq - line);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        size_t ks = 0;
        while (ks < key.size() && (key[ks] == ' ' || key[ks] == '\t')) ks++;
        key = key.substr(ks);
        std::string val(eq + 1);
        uintptr_t value = 0;
        if (ParseHex(val.c_str(), value))
            values.emplace_back(std::move(key), value);
    }
    fclose(f);
    bool stale = dump_pid != 0 && dump_pid != g_proc.pid();
    int applied = 0;
    for (const auto& [key, value] : values) {
        if (stale && (key == "g_EntityListPtr" || key == "LocalControllerIdx")) continue;
        AssignKey(key, value);
        applied++;
    }
    fprintf(stderr, "[offsets] applied %d/%zu fields from /tmp/offsets_dump.h (stale=%d, dump_pid=%d, cs2_pid=%d)\n",
            applied, values.size(), (int)stale, dump_pid, g_proc.pid());
    return applied > 0;
}

void ResetProcessState() {
    g_ClientBase = 0;
    g_EngineBase = 0;
    g_SchemaBase = 0;
    g_EntityListPtr = 0;
    g_LocalControllerIdx = -1;
    g_OffsetsReady.store(false);

    dwViewMatrix = 0;
    dwWindowWidth = 0x19EE47C;
    dwWindowHeight = 0x19EE480;
    m_iHealth = 0x4BC;
    m_iTeamNum = 0x557;
    m_iIDEntIndex = 0x42B4;
    m_hPlayerPawn = 0xA94;
    m_lifeState = 0;
    m_fFlags = 0;
    m_pGameSceneNode = 0;
    m_modelState = 0;
    m_pWeaponServices = 0;
    m_hActiveWeapon = 0;
    m_AttributeManager = 0;
    m_Item = 0;
    m_iItemDefinitionIndex = 0;
    m_bIsLocalPlayerController = 0;
    m_iszPlayerName = 0;
    m_iPing = 0;
    m_vecAbsOrigin = 0;
    m_angEyeAngles = 0;
    m_aimPunchAngle = 0;
    m_aimPunchCache = 0;
    m_iShotsFired = 0;
    m_bIsScoped = 0;
    m_iClip1 = 0;
    m_pCameraServices = 0;
    m_iFOV = 0;
    m_bIsThirdPersonView = 0;
    m_flFlashMaxAlpha = 0;
    m_flFlashDuration = 0;
    m_vecVelocity = 0;
    m_bDidSmokeEffect = 0;
    m_vSmokeColor = 0;
    m_flThrowStrength = 0;
    m_bPinPulled = 0;
    m_nSmokeEffectTickBegin = 0;
    m_flMinExposure = 0;
    m_flMaxExposure = 0;
    m_bExposureControl = 0;
    m_Glow = 0;
    m_clrRender = 0;
    m_nRenderMode = 0;
    m_fEffects = 0;
    m_hOwnerEntity = 0;
    m_bDormant = 0;
    m_iGlowType = 0;
    m_glowColorOverride = 0;
    m_bGlowing = 0;
    m_pReserveAmmo = 0;
    m_pObserverServices = 0;
    m_hObserverTarget = 0;
    m_iObserverMode = 0;
    m_bBombTicking = 0;
    m_bBombDefused = 0;
    m_bBeingDefused = 0;
    m_nBombSite = 0;
    m_flTimerLength = 0;
    m_flDefuseLength = 0;
    m_flC4Blow = 0;
    m_flDefuseCountDown = 0;
}

void SetJsonPath(const std::string& path) {
    g_json_path = path;
}

bool LoadConfiguredJson() {
    return LoadFromJson(g_json_path);
}

void UpdateBases() {
    g_ClientBase = g_proc.ModuleBase("libclient.so");
    g_EngineBase = g_proc.ModuleBase("libengine2.so");
    g_SchemaBase = g_proc.ModuleBase("libschemasystem.so");
}

}
