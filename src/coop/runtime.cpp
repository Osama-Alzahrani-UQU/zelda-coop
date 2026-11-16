#include "runtime.h"
#include "fellow.h"

#include <mutex>
#include <unordered_map>
#include <vector>

#include "MinHook.h"
#include "coop.h"
#include "symbols.h"

namespace coop {

namespace {
Runtime g_rt;

template <class T>
bool bind(const SymbolTable& t, const char* name, T& out) {
    out = (T)t.find(name);
    if (!out) log("runtime: %s not found", name);
    return out != nullptr;
}

bool hook(void* target, void* detour, void** original, const char* what) {
    if (!target) return false;
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK) {
        log("hooks: %s could not be hooked (MinHook %s)", what, MH_StatusToString(s));
        return false;
    }
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        log("hooks: %s could not be switched on (MinHook %s)", what, MH_StatusToString(s));
        return false;
    }
    log("hooks: %s at %p", what, target);
    return true;
}

// ---- the exe's hooks: all on the game's threads

// hos_oot_lift_player(c, actor): Link's movement update (f_82c415e0, x19 the actor whose vtable is 0x84b45e18), each frame
// he is updated in play. The runtime's cheats run here; the mod's game tick runs after them.
void hook_lift_player(Ctx* c, uint64_t actor) {
    g_rt.orig_lift_player(c, actor);
    game_player_update(c, actor);
}

// hos_oot_menu_tick(c, screen): every menu screen's update (f_802f1380), each frame a screen is up (the title's included)
void hook_menu_tick(Ctx* c, uint64_t screen) {
    g_rt.orig_menu_tick(c, screen);
    game_menu_update(c, screen);
}

// npad_states(c): GetNpadStates for one controller (x0 the states, x1 how many, x2 the npad id): the player's input as the
// game will read it. The mod's menus read it there and keep it from the game while they are open.
void hook_npad_states(Ctx* c) {
    const uint64_t out = c->x[0];
    const int count = (int)c->x[1];
    const uint64_t id_at = c->x[2];
    g_rt.orig_npad_states(c);
    if (count > 0 && r32(id_at) == 0) input_from_game(out, count);
}

// f_8152b670 (lifted): a text box's draw, x0 the box (its UTF-8 text at +0xf0, its length at +0x124: host/prompts.c), each
// time the game shows it. The mod's texts in the game's own boxes are put there before the game draws them.
void hook_text_draw(uint64_t box) {
    game_text_draw(nullptr, box);
    g_rt.orig_text_draw(box);
}

// The life guard Longshot spliced into f_804bd8c0 (nx2/oot_patches.py patch_life): before each CAS that lowers an actor's
// life (x11 the old value, x12 the new, x13 its address, x19 the actor) it asks hos_oot_hearts_enabled() and then
// hos_oot_is_player(c, actor). The first answered 1 makes the second run for every hit on every actor: the mod sees each
// one there; the player's own protection keeps the runtime's real answer.
thread_local int t_hearts_real = -1;
int hook_hearts_enabled() {
    t_hearts_real = g_rt.orig_hearts_enabled();
    return 1;
}
int hook_is_player(Ctx* c, uint64_t actor) {
    const int real = t_hearts_real;
    t_hearts_real = -1;
    const int is = g_rt.orig_is_player(c, actor);
    if (real < 0) return is;   // (another caller: hos_oot_lift_player's own check)
    const uint32_t before = (uint32_t)c->x[11], after = (uint32_t)c->x[12];
    if (after < before && c->x[13] >= kHeapBase) game_actor_damaged(c, actor, before, after, c->x[13], is != 0);
    // (Link's own life going down: held when the hit would end it in a session, and while he is down)
    if (is && after < before && c->x[13] == r64(actor + 7184) + 8 && fellow_guard_life(before, after)) return 1;
    return real && is;
}

// hos_oot_flight_pose(c, actor, matrix): a mover's matrix just made by its movement (3 x 4 rows; nx2/oot_patches.py
// patch_movement): the runtime's flight pose, then the developer probe's (a fallen Link lies down through his pose:
// pose.cpp)
void hook_flight_pose(Ctx* c, uint64_t actor, uint64_t matrix) {
    g_rt.orig_flight_pose(c, actor, matrix);
    probe_pins(4);
    if (debug_on()) probe_run(c, 4);
}

// hos_oot_fov(c, camera): each camera's lens about to be built (its vertical angle in s0; host/oot_fov.c): the gameplay
// camera's angle kept for the nameplates, after the runtime's own Field of view changed it
void hook_oot_fov(Ctx* c, uint64_t camera) {
    g_rt.orig_oot_fov(c, camera);
    game_camera_update(c, camera);
}

// ---- the renderer's hooks: the overlay pass of each present (mkwvk::menu_record), on the presenting thread
thread_local VkCommandBuffer_T* t_cmd;
thread_local uint32_t t_index, t_w, t_h;
thread_local bool t_in_record, t_drawn;

void hook_menu_record(VkCommandBuffer_T* cmd, uint32_t index, uint32_t w, uint32_t h) {
    t_cmd = cmd, t_index = index, t_w = w, t_h = h;
    t_in_record = true, t_drawn = false;
    g_rt.orig_menu_record(cmd, index, w, h);
    t_in_record = false;
}

// ui::record: the runtime drew this frame's overlays (an ImGui frame is open): the mod's go over them, then the pass
void hook_ui_record(VkCommandBuffer_T* cmd, uint32_t index, uint32_t w, uint32_t h) {
    if (t_in_record && !t_drawn) {
        t_drawn = true;
        overlay_draw(*g_rt.gimgui, w, h);
    }
    g_rt.orig_ui_record(cmd, index, w, h);
}

// ui::idle: the runtime has nothing to show this frame (no ImGui frame): the mod opens one when it has something to draw
void hook_ui_idle() {
    if (t_in_record && !t_drawn && overlay_wanted() && g_rt.ui_begin(t_w, t_h)) {
        t_drawn = true;
        overlay_draw(*g_rt.gimgui, t_w, t_h);
        g_rt.orig_ui_record(t_cmd, t_index, t_w, t_h);
        return;
    }
    g_rt.orig_ui_idle();
}

const char* const kVkMenuRecord = "_ZN5mkwvk11menu_recordEP17VkCommandBuffer_Tjjj";
const char* const kVkUiRecord = "_ZN5mkwvk2ui6recordEP17VkCommandBuffer_Tjjj";
const char* const kVkUiIdle = "_ZN5mkwvk2ui4idleEv";
const char* const kVkUiBegin = "_ZN5mkwvk2ui5beginEjj";
const char* const kVkUiFonts = "_ZN5mkwvk2ui5fontsEv";
const char* const kVkMenuOpen = "_ZN5mkwvk12menu_is_openEv";
}  // namespace

Runtime& rt() { return g_rt; }

bool runtime_init() {
    g_rt.exe = GetModuleHandleW(nullptr);
    SymbolTable t;
    const std::vector<std::string> want = {
        "a64_lookup", "hos_log", "rx_option", "hos_sys_alloc", "hos_ctx", "hos_oot_lift_player", "hos_oot_menu_tick",
        "npad_states", "hos_oot_state", "hos_oot_current_map", "hos_oot_teleport_ask", "hos_oot_teleport_result",
        "hos_oot_age_ask", "hos_oot_destination", "hos_prompts_text_draw", "hos_oot_fov", "hos_oot_flight_pose", "hos_oot_hearts_enabled",
        "hos_oot_is_player"};
    const int64_t t0 = now_ns();
    if (!t.load(g_rt.exe, want)) {
        log("runtime: bin\\longshot.exe has no symbol table: not a Longshot build this mod knows");
        return false;
    }
    log("runtime: %zu of %zu names found in the exe's symbols in %.1f ms", t.count(), want.size(), (now_ns() - t0) / 1e6);
    // (each bound, all reported: not a short-circuit)
    bool ok = (int)bind(t, "a64_lookup", g_rt.a64_lookup) & (int)bind(t, "rx_option", g_rt.rx_option) &
              (int)bind(t, "hos_sys_alloc", g_rt.hos_sys_alloc) & (int)bind(t, "hos_ctx", g_rt.hos_ctx);
    bind(t, "hos_log", g_rt.hos_log);
    bind(t, "hos_oot_state", g_rt.hos_oot_state);
    bind(t, "hos_oot_current_map", g_rt.hos_oot_current_map);
    bind(t, "hos_oot_teleport_ask", g_rt.hos_oot_teleport_ask);
    bind(t, "hos_oot_teleport_result", g_rt.hos_oot_teleport_result);
    bind(t, "hos_oot_age_ask", g_rt.hos_oot_age_ask);
    bind(t, "hos_oot_destination", g_rt.hos_oot_destination);
    if (!ok) return false;
    if (MH_Initialize() != MH_OK) {
        log("hooks: MinHook did not start");
        return false;
    }
    ok = hook((void*)t.find("hos_oot_lift_player"), (void*)hook_lift_player, (void**)&g_rt.orig_lift_player,
              "hos_oot_lift_player");
    ok &= hook((void*)t.find("hos_oot_menu_tick"), (void*)hook_menu_tick, (void**)&g_rt.orig_menu_tick, "hos_oot_menu_tick");
    ok &= hook((void*)t.find("npad_states"), (void*)hook_npad_states, (void**)&g_rt.orig_npad_states, "npad_states");
    hook((void*)t.find("hos_oot_fov"), (void*)hook_oot_fov, (void**)&g_rt.orig_oot_fov, "hos_oot_fov");
    hook((void*)t.find("hos_oot_flight_pose"), (void*)hook_flight_pose, (void**)&g_rt.orig_flight_pose, "hos_oot_flight_pose");
    if (hook((void*)t.find("hos_oot_hearts_enabled"), (void*)hook_hearts_enabled, (void**)&g_rt.orig_hearts_enabled,
             "hos_oot_hearts_enabled") &&
        !hook((void*)t.find("hos_oot_is_player"), (void*)hook_is_player, (void**)&g_rt.orig_is_player, "hos_oot_is_player")) {
        MH_DisableHook((void*)t.find("hos_oot_hearts_enabled"));   // (both or neither: the guard's meaning kept)
    }
    g_rt.exe_ready = ok;
    return ok;
}

void runtime_vk_init() {
    g_rt.vk = GetModuleHandleW(L"longshot_vk.dll");
    if (!g_rt.vk) return;
    SymbolTable t;
    const std::vector<std::string> want = {"GImGui", kVkMenuRecord, kVkUiRecord, kVkUiIdle, kVkUiBegin, kVkUiFonts, kVkMenuOpen};
    if (!t.load(g_rt.vk, want)) {
        log("runtime: longshot_vk.dll has no symbol table: no overlay");
        return;
    }
    g_rt.gimgui = (ImGuiContext**)t.find("GImGui");
    g_rt.ui_begin = (bool (*)(uint32_t, uint32_t))t.find(kVkUiBegin);
    g_rt.ui_fonts = (const VkFonts& (*)())t.find(kVkUiFonts);
    g_rt.menu_is_open = (bool (*)())t.find(kVkMenuOpen);
    g_rt.orig_ui_record = nullptr;
    if (!g_rt.gimgui || !g_rt.ui_begin || !g_rt.ui_fonts) {
        log("runtime: the renderer's overlay functions were not found: no overlay");
        return;
    }
    bool ok = hook((void*)t.find(kVkMenuRecord), (void*)hook_menu_record, (void**)&g_rt.orig_menu_record, "menu_record");
    ok &= hook((void*)t.find(kVkUiRecord), (void*)hook_ui_record, (void**)&g_rt.orig_ui_record, "ui::record");
    ok &= hook((void*)t.find(kVkUiIdle), (void*)hook_ui_idle, (void**)&g_rt.orig_ui_idle, "ui::idle");
    g_rt.vk_ready = ok;
}

uint64_t guest_call(Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                    float* s0, float* s1) {
    A64Fn f = g_rt.a64_lookup ? g_rt.a64_lookup(fn) : nullptr;
    if (!f || !c) return 0;
    Ctx saved;
    memcpy(&saved, c, sizeof saved);
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[3] = a3, c->x[4] = a4, c->x[5] = a5;
    c->x[30] = 0;
    f(c);
    const uint64_t r = c->x[0];
    if (s0) memcpy(s0, &c->vreg[0][0], 4);
    if (s1) memcpy(s1, &c->vreg[1][0], 4);
    memcpy(c, &saved, sizeof saved);
    return r;
}

uint64_t guest_call_frame(Ctx* c, uint64_t fn, uint64_t stack, uint64_t* frame, void (*fill)(Ctx* c, uint64_t frame, void* arg),
                          void* arg, int64_t x8_off, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    A64Fn f = g_rt.a64_lookup ? g_rt.a64_lookup(fn) : nullptr;
    if (!f || !c) return 0;
    Ctx saved;
    memcpy(&saved, c, sizeof saved);
    const uint64_t base = (c->x[31] - stack - 0x40) & ~0xFULL;
    if (frame) *frame = base;
    memset(gp(base), 0, (size_t)stack);
    if (fill) fill(c, base, arg);
    c->x[31] = base;
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[3] = a3;
    if (x8_off >= 0) c->x[8] = base + (uint64_t)x8_off;
    c->x[30] = 0;
    f(c);
    const uint64_t r = c->x[0];
    memcpy(c, &saved, sizeof saved);
    return r;
}

uint64_t guest_alloc(uint64_t size, uint64_t align) {
    return g_rt.hos_sys_alloc ? g_rt.hos_sys_alloc(size, align) : 0;
}

uint64_t guest_string(const std::string& text) {
    static std::mutex m;
    static std::unordered_map<std::string, uint64_t> interned;
    std::lock_guard<std::mutex> lock(m);
    auto it = interned.find(text);
    if (it != interned.end()) return it->second;
    const uint64_t at = guest_alloc(text.size() + 16, 16);
    if (!at) return 0;
    poke(at, text.c_str(), text.size() + 1);
    interned[text] = at;
    return at;
}

}  // namespace coop
