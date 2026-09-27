#pragma once
struct Settings;
typedef struct _cairo cairo_t;
namespace render { struct Camera; }

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
    void InitLua(Settings* cfg);
    void ShutdownLua();
    void ReloadLua();
    void PaintLua(cairo_t* cr, const render::Camera& camera);
    void TickLua();
    void FrameLua();
}
