#pragma once
#include <mutex>
#include <vector>

namespace features {

struct SoundStep {
    float x = 0.f, y = 0.f, z = 0.f;
    float radius = 0.f;
    float alpha = 0.f;
};

extern std::mutex g_sound_steps_mtx;
extern std::vector<SoundStep> g_sound_steps;
void UpdateSoundEsp();

}
