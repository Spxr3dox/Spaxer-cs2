#pragma once
#include <cstdint>
#include <atomic>
#include <string>

namespace off {
    inline uintptr_t g_ClientBase = 0;
    inline uintptr_t g_EngineBase = 0;
    inline uintptr_t g_SchemaBase = 0;

    inline uintptr_t g_EntityListPtr = 0;
    inline int       g_LocalControllerIdx = -1;
    inline std::atomic<bool> g_OffsetsReady{false};

    inline uintptr_t dwViewMatrix         = 0x1A2A8F0;
    inline uintptr_t dwWindowWidth        = 0x19EE47C;
    inline uintptr_t dwWindowHeight       = 0x19EE480;

    inline uintptr_t m_iHealth      = 0x4BC;
    inline uintptr_t m_iTeamNum     = 0x557;
    inline uintptr_t m_iIDEntIndex  = 0x42B4;
    inline uintptr_t m_hPlayerPawn  = 0xA94;

    inline uintptr_t m_lifeState      = 0;
    inline uintptr_t m_fFlags         = 0;
    inline uintptr_t m_pGameSceneNode = 0;
    inline uintptr_t m_modelState     = 0;
    inline uintptr_t m_pWeaponServices = 0;
    inline uintptr_t m_hActiveWeapon   = 0;
    inline uintptr_t m_AttributeManager     = 0;
    inline uintptr_t m_Item                 = 0;
    inline uintptr_t m_iItemDefinitionIndex = 0;
    inline uintptr_t m_bIsLocalPlayerController = 0;
    inline uintptr_t m_iszPlayerName = 0;
    inline uintptr_t m_iPing         = 0;

    inline uintptr_t m_vecAbsOrigin = 0;
    inline uintptr_t m_angEyeAngles = 0;
    inline uintptr_t m_aimPunchAngle = 0;
    inline uintptr_t m_aimPunchCache  = 0;
    inline uintptr_t m_iShotsFired    = 0;

    inline uintptr_t m_bIsScoped         = 0;
    inline uintptr_t m_iClip1            = 0;
    inline uintptr_t m_pCameraServices   = 0;
    inline uintptr_t m_iFOV              = 0;
    inline uintptr_t m_bIsThirdPersonView = 0;

    inline uintptr_t m_flFlashMaxAlpha = 0;
    inline uintptr_t m_flFlashDuration = 0;
    inline uintptr_t m_vecVelocity     = 0;
    inline uintptr_t m_bDidSmokeEffect = 0;
    inline uintptr_t m_vSmokeColor     = 0;
    inline uintptr_t m_Glow              = 0;
    inline uintptr_t m_clrRender         = 0;
    inline uintptr_t m_hOwnerEntity      = 0;
    inline uintptr_t m_bDormant          = 0;
    inline uintptr_t m_iGlowType         = 0;
    inline uintptr_t m_glowColorOverride = 0;
    inline uintptr_t m_bGlowing          = 0;
    inline uintptr_t m_entitySpottedState = 0x2270;
    inline uintptr_t m_bSpotted        = 0x8;
    inline uintptr_t m_bSpottedByMask  = 0xC;

    inline uintptr_t m_pReserveAmmo = 0;

    inline uintptr_t m_pObserverServices = 0;
    inline uintptr_t m_hObserverTarget   = 0;
    inline uintptr_t m_iObserverMode     = 0;

    inline uintptr_t m_bBombTicking      = 0;
    inline uintptr_t m_bBombDefused      = 0;
    inline uintptr_t m_bBeingDefused     = 0;
    inline uintptr_t m_nBombSite         = 0;
    inline uintptr_t m_flTimerLength     = 0;
    inline uintptr_t m_flDefuseLength    = 0;
    inline uintptr_t m_flC4Blow          = 0;
    inline uintptr_t m_flDefuseCountDown = 0;

    void ResetProcessState();
    void UpdateBases();
    void SetJsonPath(const std::string& path);
    bool LoadConfiguredJson();
    bool LoadFromJson(const std::string& path);
    bool LoadFromPseFile();
}
