// The game's title menu as the mod drives it: its own four buttons show the mod's pages (texts, count, focus, sounds), so
// the online menus look, move and sound exactly as the game's own.
#pragma once

#include <string>
#include <vector>

#include "common.h"

namespace coop {

// (game threads) every menu screen's update: the title screen is driven here, the Load screen is watched
void title_screen_update(Ctx* c, uint64_t screen);

namespace title {
// the title screen is up (updated in the last 200 ms)
bool active();
// the buttons' texts for a page of the mod's (at most four; empty: the game's own with the Online entry). Applied at the
// title screen's next update, the first one focused
void show_page(const std::vector<std::string>& items, int focus = 0);
bool native();                    // the game's own page is shown (with Online)
int focused();                    // the focused button's index, -1 none
int count();                      // the buttons shown
bool online_focused();            // the game's page, its Online entry focused
bool menu_ready();                // the title's menu is in and takes input
void sound(const char* name);     // a menu sound at the next update (mc_Abutton_decide, mc_ReturnFromDetail, ...)
void focus(int index);            // the game's focus set on a button at the next update
}  // namespace title

// ---- the Load screen (the game's own save list, vtable 0x84c0eba0): the mod's file choice and the automatic load
namespace loadscreen {
bool open();                      // up now
int focused_row();                // the focused row, top first (-1 none)
void select_row(int row);         // the game's focus on that row at the next update
}  // namespace loadscreen

}  // namespace coop
