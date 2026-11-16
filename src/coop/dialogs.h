// The overlay's dialogs: the on-screen keyboard, Online Settings (a Host tab and a Player tab), the invitations' friends
// list, and in play the online panel, Give To and the gifts' prompts. Their state lives here; overlay.cpp draws them from
// dialogs_view(); input reaches them from menus_input (the controller) and the window (keys).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "menus.h"
#include "session.h"

namespace coop {

bool dialogs_open();
void dialogs_input(uint64_t raw, uint64_t pressed, int32_t stick[4]);
void dialogs_keyboard_input(TextEdit* e, uint64_t raw, uint64_t pressed, int32_t stick[4], void (*done)(bool ok));
void dialogs_edit_done(int purpose, const std::string& text);
// a text the dialogs want typed (a settings row): taken by the menus, which open the keyboard for it
bool dialogs_take_edit(int* purpose, std::string* title, std::string* text, int* max);
void dialogs_char(uint32_t ch);
void dialogs_key(int vk);

void dialogs_open_settings();
void dialogs_open_invite();
void dialogs_open_ingame();     // the online panel in play (players, Give To, leave)
// (in play, not in a session: the games on the network, to join from the game being played: a new player's first game
// included, who has no file for the title's Join Game yet)
void dialogs_open_join();
void dialogs_open_give();       // Give To: an item, then a player
// (the pause menu, A on an item online) the item's menu: the game's own action, Give To..., Cancel. false when the
// item is not one that can be given (the game keeps its A)
bool dialogs_is_giftable(const std::string& pouch);
// (the pause menu, A on an item online) asked from the controller; carried out at the pause menu's update, which reads the
// count: the item's menu when the player has some, else the game gets its A back
void dialogs_ask_item_menu(const std::string& pouch, bool equipment);
bool dialogs_take_item_menu_ask(std::string* pouch, bool* equipment);
void dialogs_open_item_menu(const std::string& pouch, bool equipment, int count);
void dialogs_give_game_press();   // (the game gets its A back)
bool dialogs_take_game_press();  // (the item menu's first row chosen: the game gets its A back once)
void dialogs_close();

// ---- in play: the gifts' prompts (offered to this player: Accept / Decline; accepted by the other: How many?)
void dialogs_gifts_update(const std::vector<Gift>& gifts, int me);

// The items that can be given, as the game's pouch names them, with this player's counts (read in play)
struct Item {
    const char* label;
    const char* pouch;
    bool quantity;   // a count (bombs, arrows, ...) rather than one held or not
};
const std::vector<Item>& giftable_items();
void dialogs_set_counts(const std::vector<int>& counts);   // (the game: this player's count of each giftable item)

struct SettingRow {
    std::string name, value, help;
    bool edit = false;   // A types it on the keyboard
};

struct DialogsView {
    int kind = 0;                       // kDlgNone, kDlgSettings, kDlgInvite, kDlgIngame, kDlgGiveItem, kDlgGivePlayer,
                                        // kDlgGiftOffer, kDlgGiftAmount, kDlgWaiting
    int tab = 0;                        // (settings) 0 Host, 1 Player
    int sel = 0;
    std::vector<SettingRow> rows;       // (settings, invite, ingame, give lists: one per row)
    std::string title, text;
    int amount = 1, amount_max = 1;
    uint32_t gift = 0;
};
enum { kDlgNone = 0, kDlgSettings, kDlgInvite, kDlgIngame, kDlgGiveItem, kDlgGivePlayer, kDlgGiftOffer, kDlgGiftAmount, kDlgWaiting,
       kDlgItemMenu, kDlgJoin };
DialogsView dialogs_view();

}  // namespace coop
