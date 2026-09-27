#pragma once
#include <cstdint>
#include <vector>

namespace dumper {
    void Reset();
    void Run();
    bool ReadyForGameplay();
    std::vector<uintptr_t> FindButtonNameSlots(const char* name);
    std::vector<uintptr_t> FindButtonStateCandidates(const char* name);
}
