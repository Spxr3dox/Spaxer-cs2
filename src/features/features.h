#pragma once
struct Settings;

namespace features {
    void UpdateBomb();
    void UpdatePlayer();
    void UpdateEsp();
    bool ReloadCs2Crosshair(Settings* cfg);

    void StartTriggerBot();
    void StopTriggerBot();
    void StartMovement();
    void StopMovement();
    void StartAimbot();
    void StopAimbot();
    void StartRcs();
    void StopRcs();
    void ApplyUnsafe();
    void ApplyGlow();
    void ApplyChams();
    void ApplyRadarHack();
    void UpdateHitmarker();
    void UpdateSoundEsp();
}
