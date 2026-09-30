#pragma once
#include <cstdint>
#include <string>

typedef void* (*CreateInterfaceFn)(const char*, int*);

bool SdkLoadFactories();
void* SdkGetIface(const char* name);
uintptr_t SdkFindLibBase(const char* soname);

void* SdkCallV(void* obj, int idx, const void* arg = nullptr);
std::string SdkSafeStr(uintptr_t addr);

template<typename T>
T SdkReadSafe(uintptr_t addr, T def = T{});
