// The player's input for the mod's menus: the controller as the game reads it (GetNpadStates: the pads and the keys
// bound to Switch buttons, merged by the runtime), and the window's keyboard for typing names and addresses.
#include <atomic>

#include "coop.h"
#include "menus.h"
#include "session.h"
#include "dialogs.h"

namespace coop {

namespace {
constexpr uint64_t kStateSize = 0x28;   // (host/nn_hid.c STATE_SIZE: +8 the buttons, +16 the sticks)
HWND g_window = nullptr;
WNDPROC g_old_proc = nullptr;
// (closing the window while in play: the game saved into its slot first, then the window really closes)
constexpr UINT_PTR kCloseTimer = 0x4C53;
bool g_close_allowed = false;
ULONGLONG g_close_started = 0;

LRESULT CALLBACK window_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE && !g_close_allowed) {
        if (game_in_play() || pause_menu_open()) {
            // (in play: the game saved first, as its own autosave, then the window really closes)
            if (!g_close_started) {
                g_close_started = GetTickCount64();
                dialogs_close();
                game_save_now("the game closing");
                Session::get().toast("Saving before closing...", 0);
                SetTimer(h, kCloseTimer, 100, nullptr);
            }
            return 0;
        }
        Session::get().leave();   // (in the menus: the others told at once, not by a timeout)
    }
    if (msg == WM_TIMER && wp == kCloseTimer) {
        const int st = game_save_state();
        if (st == kSaveDone || st == kSaveFailed || st == kSaveIdle || GetTickCount64() - g_close_started > 25000) {
            KillTimer(h, kCloseTimer);
            game_save_stop();
            Session::get().leave();   // (the others told at once, not by a timeout)
            g_close_allowed = true;
            PostMessageW(h, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    if (menus_modal()) {
        const TextEdit e = menus_text_edit();
        if (msg == WM_CHAR && e.open) {
            menus_text_char((uint32_t)wp);
            return 0;
        }
        if (msg == WM_KEYDOWN && (wp == VK_BACK || wp == VK_RETURN || wp == VK_ESCAPE)) {
            menus_text_key((int)wp);
            return 0;
        }
    }
    if (msg == WM_KEYDOWN && wp == VK_F2 && !(lp & (1 << 30)) && game_in_play()) {
        if (dialogs_open()) dialogs_close(), Session::get().searching(false);
        else if (Session::get().active()) dialogs_open_ingame();
        else dialogs_open_join();
        return 0;
    }
    return CallWindowProcW(g_old_proc, h, msg, wp, lp);
}

BOOL CALLBACK find_window(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) {
        RECT r;
        GetClientRect(h, &r);
        if (r.right > 320) {
            *(HWND*)lp = h;
            return FALSE;
        }
    }
    return TRUE;
}
}  // namespace

void input_from_game(uint64_t states, int count) {
    uint64_t buttons = r64(states + 8);
    int32_t stick[4] = {0, 0, 0, 0};
    peek(states + 16, stick, sizeof stick);
    const uint64_t before = buttons;
    int32_t before_stick[4];
    memcpy(before_stick, stick, sizeof stick);
    menus_input(&buttons, stick);
    if (buttons == before && !memcmp(stick, before_stick, sizeof stick)) return;
    for (int i = 0; i < count && i < 16; i++) {
        const uint64_t e = states + (uint64_t)i * kStateSize;
        w64(e + 8, buttons);
        poke(e + 16, stick, sizeof stick);
    }
}

void input_window_hook() {
    // (the game's window: made by the runtime once the renderer is up; looked for until it is there)
    for (int i = 0; i < 600 && !g_window; i++) {
        EnumWindows(find_window, (LPARAM)&g_window);
        if (!g_window) Sleep(100);
    }
    if (!g_window) {
        log("input: the game's window was not found: typing works with the on-screen keyboard only");
        return;
    }
    g_old_proc = (WNDPROC)SetWindowLongPtrW(g_window, GWLP_WNDPROC, (LONG_PTR)window_proc);
    log("input: the game's window %p (keyboard for the menus, F2 the online panel)", (void*)g_window);
}

}  // namespace coop
