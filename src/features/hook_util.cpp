#include "hook_util.h"
#include <vector>
#include "../core/core.h"
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace hookutil {

static std::vector<uint8_t> ParsePattern(const char* p, std::string& mask_out) {
    std::vector<uint8_t> bytes;
    mask_out.clear();
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '?') {
            bytes.push_back(0);
            mask_out.push_back('?');
            p++;
            if (*p == '?') p++;
        } else {
            char buf[3] = {p[0], p[1], 0};
            bytes.push_back((uint8_t)strtol(buf, nullptr, 16));
            mask_out.push_back('x');
            p += 2;
        }
    }
    return bytes;
}

uintptr_t PatternScan(uintptr_t start, size_t size, const char* ida_pattern) {
    std::string mask;
    auto bytes = ParsePattern(ida_pattern, mask);
    if (bytes.empty()) return 0;
    size_t plen = bytes.size();
    for (size_t i = 0; i + plen <= size; i++) {
        bool ok = true;
        for (size_t j = 0; j < plen; j++) {
            if (mask[j] == 'x' && ((uint8_t*)start)[i + j] != bytes[j]) { ok = false; break; }
        }
        if (ok) return start + i;
    }
    return 0;
}

uintptr_t PatternScanModule(const char* soname, const char* ida_pattern) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[1024];
    uintptr_t s = 0, e = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, soname)) continue;
        if (!strstr(line, " r-xp ")) continue;
        if (sscanf(line, "%lx-%lx", &s, &e) == 2) break;
    }
    fclose(f);
    if (!s || !e) return 0;
    return PatternScan(s, e - s, ida_pattern);
}

void* InstallInlineHook(void* target, void* replacement) {
    if (!target || !replacement) return nullptr;
    uintptr_t page = (uintptr_t)target & ~(4095UL);
    if (mprotect((void*)page, 8192, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        Log("[hook] mprotect fail at %p", target);
        return nullptr;
    }
    // 14-byte JMP [rip+0] : FF 25 00 00 00 00 <8-byte addr>
    static thread_local uint8_t saved[14];
    memcpy(saved, target, 14);
    uint8_t stub[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    uint64_t addr = (uint64_t)replacement;
    memcpy(stub + 6, &addr, 8);
    memcpy(target, stub, 14);
    // Trampoline: alloc executable page holding original bytes + JMP to (target+14)
    void* tramp = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) { Log("[hook] mmap tramp fail"); return nullptr; }
    memcpy(tramp, saved, 14);
    uint8_t back[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    uint64_t back_addr = (uint64_t)target + 14;
    memcpy(back + 6, &back_addr, 8);
    memcpy((uint8_t*)tramp + 14, back, 14);
    Log("[hook] installed @ %p -> %p (tramp %p)", target, replacement, tramp);
    return tramp;
}

void* VmtHook(void** vtable, int index, void* replacement) {
    if (!vtable || !replacement) return nullptr;
    uintptr_t page = (uintptr_t)&vtable[index] & ~(4095UL);
    if (mprotect((void*)page, 8192, PROT_READ | PROT_WRITE) != 0) {
        Log("[vmt] mprotect fail");
        return nullptr;
    }
    void* orig = vtable[index];
    vtable[index] = replacement;
    Log("[vmt] hook vtable[%d]=%p -> %p", index, orig, replacement);
    return orig;
}

} // namespace hookutil
