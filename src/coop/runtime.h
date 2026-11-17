// The Longshot runtime as the mod uses it: functions found by name in bin\longshot.exe and bin\longshot_vk.dll, and the
// hooks on them.
#pragma once

#include "common.h"

struct ImGuiContext;
struct ImFont;
struct VkCommandBuffer_T;

namespace coop {

struct VkFonts {   // (mkwvk::ui::Fonts, gpu/vk/vk_imgui.h)
    ImFont* body;
    ImFont* strong;
    ImFont* heading;
};

struct Runtime {
    // ---- bin\longshot.exe
    HMODULE exe = nullptr;
    A64Fn (*a64_lookup)(uint64_t addr) = nullptr;
    void (*hos_log)(const char* fmt, ...) = nullptr;
    int (*rx_option)(int id, int otherwise) = nullptr;
    uint64_t (*hos_sys_alloc)(uint64_t size, uint64_t align) = nullptr;
    Ctx* (*hos_ctx)() = nullptr;
    int (*hos_oot_state)(int* event, int* loading, int* child) = nullptr;
    int (*hos_oot_current_map)(char* out, size_t cap) = nullptr;
    int (*hos_oot_teleport_ask)(int index) = nullptr;
    int (*hos_oot_teleport_result)(int ticket, char* why, size_t why_cap, char* map, size_t map_cap, int* accepted) = nullptr;
    int (*hos_oot_age_ask)(int adult) = nullptr;
    int (*hos_oot_destination)(int index, const char** child, const char** adult) = nullptr;
    // ---- bin\longshot_vk.dll (once the renderer loaded it)
    HMODULE vk = nullptr;
    ImGuiContext** gimgui = nullptr;
    bool (*ui_begin)(uint32_t w, uint32_t h) = nullptr;
    const VkFonts& (*ui_fonts)() = nullptr;
    bool (*menu_is_open)() = nullptr;
    // (the originals behind the hooks)
    void (*orig_menu_record)(VkCommandBuffer_T* cmd, uint32_t index, uint32_t w, uint32_t h) = nullptr;
    void (*orig_ui_record)(VkCommandBuffer_T* cmd, uint32_t index, uint32_t w, uint32_t h) = nullptr;
    void (*orig_ui_idle)() = nullptr;
    void (*orig_lift_player)(Ctx* c, uint64_t actor) = nullptr;
    void (*orig_menu_tick)(Ctx* c, uint64_t screen) = nullptr;
    void (*orig_npad_states)(Ctx* c) = nullptr;
    void (*orig_text_draw)(uint64_t box) = nullptr;   // (hos_prompts_text_draw: called from f_8152b670, a text box's draw)
    void (*orig_oot_fov)(Ctx* c, uint64_t camera) = nullptr;
    void (*orig_flight_pose)(Ctx* c, uint64_t actor, uint64_t matrix) = nullptr;
    int (*orig_hearts_enabled)() = nullptr;
    int (*orig_is_player)(Ctx* c, uint64_t actor) = nullptr;
    void (*orig_pause_request)(Ctx* c) = nullptr;   // (the game's PauseMgr request, f_80b1ba60, as lifted)
    void (*orig_send_message)(Ctx* c) = nullptr;    // (the actors' message send, f_80393a20, as lifted)
    bool exe_ready = false, vk_ready = false;
};

Runtime& rt();

// Finds the runtime's functions and hooks them: the exe's at once, the renderer's when longshot_vk.dll is loaded (a
// thread of the mod's polls for it). false when this is not a Longshot build the mod knows.
bool runtime_init();
void runtime_vk_init();   // (once longshot_vk.dll is there)

// A guest function called from the host, on a game thread inside one of the game's own calls (c: that thread's context).
// The context is restored after: only x0 (and the out pointers' s0/s1) come back.
uint64_t guest_call(Ctx* c, uint64_t fn, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0,
                    uint64_t a4 = 0, uint64_t a5 = 0, float* s0 = nullptr, float* s1 = nullptr);

// The same with room on the guest stack: the stack pointer lowered by `stack` bytes (16-aligned) for the call, *frame its
// lowest address (an EventRequestArg, an out struct: its caller fills or reads it); x8 (an indirect result) at x8_off
// within it when x8_off >= 0
uint64_t guest_call_frame(Ctx* c, uint64_t fn, uint64_t stack, uint64_t* frame, void (*fill)(Ctx* c, uint64_t frame, void* arg),
                          void* arg, int64_t x8_off, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0);

// A guest allocation that stays (the runtime's system region: never freed); 0 when none
uint64_t guest_alloc(uint64_t size, uint64_t align = 16);
// A lasting guest copy of a text (interned: the same text gives the same address)
uint64_t guest_string(const std::string& text);

}  // namespace coop
