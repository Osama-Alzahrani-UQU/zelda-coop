// Longshot Co-op: shared definitions.
//
// The game is Longshot's native build of The Legend of Zelda: Ocarina of Time (Nintendo Switch 2, build e0f3ccfa): the
// game's AArch64 code lifted to C and compiled, run by Longshot's HLE runtime (bin\longshot.exe), drawn by its Vulkan
// renderer (bin\longshot_vk.dll). Both carry their COFF symbol tables, so this mod finds everything it uses by name.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace coop {

// The lifted code's guest context (a64-kit rt/a64.h GuestContext with Longshot's nx2/rtgen.py poll flag). Offsets are
// checked against the runtime at start (game.cpp: the context's size from hos_ctx's thread).
struct Ctx {
    uint64_t x[32];   // x[31] is SP
    uint64_t pc;
    uint8_t n, z, c, v;
    int32_t halted;
    uint64_t vreg[32][2];
    uint64_t tpidr_el0;
    uint64_t tpidrro_el0;
    uint64_t fpcr, fpsr;
    uint64_t excl_addr, excl_lo, excl_hi;
    uint32_t excl_size;
    int32_t excl_valid;
    void* thread;
    uint32_t poll;
};
static_assert(sizeof(Ctx) == 864, "the guest context's size");
static_assert(offsetof(Ctx, vreg) == 272, "vreg");

using A64Fn = void (*)(Ctx*);

// Guest memory: the 512 GB window at 16 TB (nx2/rtgen.py: A64_PTR(a) = A64_MEM + a, A64_HOST_BASE 0x100000000000)
constexpr uint64_t kGuestBase = 0x100000000000ULL;
constexpr uint64_t kGuestSpace = 1ULL << 39;
constexpr uint64_t kHeapBase = 0x1000000000ULL;

inline bool guest_ok(uint64_t a, uint64_t n = 8) { return a >= 0x100000 && a < kGuestSpace && n <= kGuestSpace - a; }
inline void* gp(uint64_t a) { return (void*)(kGuestBase + a); }

// Reads that never fault on a stray pointer: outside the window they read 0. A page of the window not committed yet is
// committed by the runtime's own handler on touch (mkw_glue.c commit_on_touch), as the game's own reads are.
inline bool peek(uint64_t a, void* dst, size_t n) {
    if (!guest_ok(a, n)) return false;
    memcpy(dst, gp(a), n);
    return true;
}
inline uint64_t r64(uint64_t a) { uint64_t v = 0; peek(a, &v, 8); return v; }
inline uint32_t r32(uint64_t a) { uint32_t v = 0; peek(a, &v, 4); return v; }
inline uint16_t r16(uint64_t a) { uint16_t v = 0; peek(a, &v, 2); return v; }
inline uint8_t r8(uint64_t a) { uint8_t v = 0; peek(a, &v, 1); return v; }
inline float rf(uint64_t a) { float v = 0; peek(a, &v, 4); return v; }
inline bool poke(uint64_t a, const void* src, size_t n) {
    if (!guest_ok(a, n)) return false;
    memcpy(gp(a), src, n);
    return true;
}
inline void w32(uint64_t a, uint32_t v) { poke(a, &v, 4); }
inline void w64(uint64_t a, uint64_t v) { poke(a, &v, 8); }
inline void w8(uint64_t a, uint8_t v) { poke(a, &v, 1); }
inline void wf(uint64_t a, float v) { poke(a, &v, 4); }

std::string cstr_at(uint64_t a, size_t max = 128);
// A map's name as players read it: "KokiriForest_Child" -> "Kokiri Forest"
std::string place_name(const std::string& map);

// ---- logging (coop\coop_log.txt, and the game's own log through hos_log once it is found)
void log(const char* fmt, ...);
void log_flush();

int64_t now_ns();

// The folders: bin\ (the game's exe), the game folder (image.bin, romfs, user), the mod's (bin\coop)
const std::wstring& bin_dir();
const std::wstring& game_dir();
const std::wstring& mod_dir();
std::wstring profile_suffix();   // "" or "_<LONGSHOT_COOP_PROFILE>"
std::string narrow(const std::wstring& w);
std::wstring widen(const std::string& s);

}  // namespace coop
