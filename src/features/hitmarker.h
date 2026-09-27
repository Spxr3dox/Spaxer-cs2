#pragma once
#include "sdk/game.h"
#include <chrono>
#include <mutex>
#include <vector>

namespace features {

struct HitMark {
    Vec3 position;
    int damage;
    std::chrono::steady_clock::time_point time;
};

extern std::mutex g_hitmarks_mtx;
extern std::vector<HitMark> g_hitmarks;
void UpdateHitmarker();

}
