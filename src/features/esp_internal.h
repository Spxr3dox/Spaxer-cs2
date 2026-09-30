#pragma once
#include <atomic>
#include <mutex>
#include <vector>
#include <string>

struct EspPlayer {
    float ox, oy, oz;    // origin world
    float hx, hy, hz;    // head world
    int   health;
    int   team;
    bool  dormant;
    bool  local;
    std::string name;
};

struct EspSnapshot {
    std::vector<EspPlayer> players;
    float view_matrix[16];
    bool  valid;
};

extern std::mutex g_esp_mtx;
extern EspSnapshot g_esp_snap;
extern std::atomic<bool> g_esp_ready;

void EspStart();
