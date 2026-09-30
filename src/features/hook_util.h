#pragma once
#include <cstdint>
#include <cstddef>

namespace hookutil {

// Pattern format: "48 8B ? ? ? ? 90" — spaces separate bytes, "?" is wildcard.
uintptr_t PatternScan(uintptr_t start, size_t size, const char* ida_pattern);
uintptr_t PatternScanModule(const char* soname, const char* ida_pattern);

// Inline hook: overwrite first bytes of target with 14-byte 64-bit indirect JMP,
// save originals into trampoline (which then jumps back to target+n).
// Returns original function pointer (trampoline) or nullptr on failure.
void* InstallInlineHook(void* target, void* replacement);
bool RemoveInlineHook(void* target, void* saved_original);

// VMT hook: replace function pointer at vtable[index] with replacement.
// Returns original function pointer.
void* VmtHook(void** vtable, int index, void* replacement);

// Utility: get instance vtable
inline void** GetVTable(void* instance) {
    return *reinterpret_cast<void***>(instance);
}

} // namespace hookutil
