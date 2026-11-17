// The online menus: pages on the title menu's own buttons, the game's Load screen for choosing a file, and the overlay's
// panels and dialogs where a page needs more than buttons (the lobby's slots, the server list, the keyboard, settings).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common.h"

namespace coop {

enum class Page { Title, Name, Online, Join, HostLobby, GuestLobby, Settings, PickFile, Loading, AutoLoad, HostStart };

// Buttons as the game reads them (B_* bits of host/nn_hid.c)
enum : uint64_t {
    kA = 1 << 0, kB = 1 << 1, kX = 1 << 2, kY = 1 << 3, kLS = 1 << 4, kRS = 1 << 5, kL = 1 << 6, kR = 1 << 7,
    kZL = 1 << 8, kZR = 1 << 9, kPlus = 1 << 10, kMinus = 1 << 11, kLeft = 1 << 12, kUp = 1 << 13, kRight = 1 << 14,
    kDown = 1 << 15
};

// (the npad hook, the game's main thread) the player's input this frame; what the game gets is changed in place
void menus_input(uint64_t* buttons, int32_t stick[4]);
// (the title screen's update) the menus' work on the game's thread
void menus_tick(Ctx* c);
// (the game, in play) a loaded game: the session's start finished
void menus_in_play();

struct MenuView {
    Page page = Page::Title;
    int file = -1;                  // the chosen file's row (the Load screen's, top first)
    std::string file_label;
    std::string loading_text;
    int64_t loading_since = 0;
    bool name_editing = false;
    bool hide_loading = false;      // (the game's own question is up: the player answers it)
};
MenuView menus_view();
Page menus_page();   // (cheap: the page alone)
void menus_probe(const std::string& what, const std::string& a, const std::string& b);
// the overlay's own dialogs: the keyboard (name, address, password), settings, lists. While one is open every input is
// the overlay's
bool menus_modal();

// ---- the keyboard and the dialogs (overlay.cpp draws them, input reaches them through menus_input and the window)
struct TextEdit {
    bool open = false;
    std::string title, text, hint;
    int max = 20;
    int cursor_key = 0;             // the on-screen keyboard's key
    int purpose = 0;                // kEditName, kEditAddress, kEditPassword, kEditServer
};
enum { kEditName = 1, kEditAddress, kEditPassword, kEditServer, kEditJoinPassword };
TextEdit menus_text_edit();
void menus_text_char(uint32_t ch);   // (the window: a typed character)
void menus_text_key(int vk);         // (the window: a key: back, enter, escape, arrows)

// The saves on disk, newest first (as the game's Load screen lists them)
struct SaveInfo {
    int slot = -1;
    std::string location, map, when;
    int64_t mtime = 0;
};
std::vector<SaveInfo> list_saves();
// When the newest of them was written (a FILETIME; 0 none): the files' times only, for the end of a save
int64_t newest_save_time();
// the player's save folder (the account's, under the --user folder or the game's user folder); empty: none yet
std::wstring save_account_dir();
// (the game closing or the session ending in the pause menu: the menu closed as its Back does, so the game's save can run)
void menus_close_pause();

}  // namespace coop
