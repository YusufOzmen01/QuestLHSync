// A minimal x86-64 inline hook for Linux (MinHook's part on Windows): the function's first instructions move to a
// trampoline that jumps back after them, and a 14-byte absolute jump to the detour takes their place. Only
// instructions that run the same anywhere are moved (no RIP-relative operand, no relative branch); anything else
// and the hook isn't made. hde64 (third_party/minhook) tells the instructions apart.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

struct InlineHook {
  void *target = nullptr;
  void *tramp = nullptr;  // the moved instructions, then a jump back: call it for the original
  uint8_t saved[32];
  size_t n = 0;           // bytes moved
};

// before any thread runs target (the shim hooks right after the lighthouse driver is loaded, before its Init)
bool HookCreate(void *target, void *detour, InlineHook &h, std::string &why);
// puts the original bytes back
void HookRemove(InlineHook &h);
