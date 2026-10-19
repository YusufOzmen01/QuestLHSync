#include "inline_hook.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

extern "C" {
#include "hde/hde64.h"
}

namespace {

constexpr size_t kJmp = 14;  // jmp qword [rip+0]; dq address

void PutJmp(uint8_t *at, const void *to) {
  at[0] = 0xFF;
  at[1] = 0x25;
  memset(at + 2, 0, 4);
  uint64_t a = (uint64_t)to;
  memcpy(at + 6, &a, 8);
}

bool Writable(void *p, size_t n, bool on) {
  long pg = sysconf(_SC_PAGESIZE);
  uintptr_t s = (uintptr_t)p & ~(uintptr_t)(pg - 1), e = ((uintptr_t)p + n + pg - 1) & ~(uintptr_t)(pg - 1);
  return mprotect((void *)s, e - s, PROT_READ | PROT_EXEC | (on ? PROT_WRITE : 0)) == 0;
}

}  // namespace

bool HookCreate(void *target, void *detour, InlineHook &h, std::string &why) {
  auto *p = (uint8_t *)target;
  size_t n = 0;
  while (n < kJmp) {
    hde64s hs;
    unsigned len = hde64_disasm(p + n, &hs);
    if (!len || (hs.flags & F_ERROR)) { why = "an instruction it can't read"; return false; }
    if (hs.flags & F_RELATIVE) { why = "a branch in its first instructions"; return false; }
    if ((hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5) {
      why = "a RIP-relative operand in its first instructions";
      return false;
    }
    bool jumps = hs.opcode == 0xFF && hs.modrm_reg >= 2 && hs.modrm_reg <= 5;  // call / jmp through a register
    if (hs.opcode == 0xC3 || hs.opcode == 0xC2 || hs.opcode == 0xCC || jumps) {
      why = "it ends or jumps before there's room for the hook";
      return false;
    }
    n += len;
  }
  if (n > sizeof h.saved) { why = "its first instructions are too long"; return false; }
  void *t = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (t == MAP_FAILED) { why = "no executable memory for the trampoline"; return false; }
  memcpy(t, p, n);
  PutJmp((uint8_t *)t + n, p + n);
  if (!Writable(p, n, true)) {
    munmap(t, 4096);
    why = "its code can't be made writable";
    return false;
  }
  memcpy(h.saved, p, n);
  uint8_t patch[32];
  PutJmp(patch, detour);
  memset(patch + kJmp, 0xCC, n - kJmp);  // the rest of a split instruction: never reached
  memcpy(p, patch, n);
  Writable(p, n, false);
  __builtin___clear_cache((char *)p, (char *)p + n);
  __builtin___clear_cache((char *)t, (char *)t + n + kJmp);
  h.target = target;
  h.tramp = t;
  h.n = n;
  return true;
}

void HookRemove(InlineHook &h) {
  if (!h.target) return;
  if (Writable(h.target, h.n, true)) {
    memcpy(h.target, h.saved, h.n);
    Writable(h.target, h.n, false);
    __builtin___clear_cache((char *)h.target, (char *)h.target + h.n);
  }
  h.target = nullptr;  // the trampoline stays: a thread may still be in it
}
