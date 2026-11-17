// The host's world for its guests. A guest plays in the host's save: when the game starts (or a guest joins one under
// way) the guest asks the host for it, the host sends its save's two files (the file it is starting from, or a fresh
// save of the game in play), and the guest's game loads them from one of its own save slots. The guest's own saves are
// copied aside first and put back when it is out of the session again (back at the title, or at the next start after a
// crash), so playing online never costs a guest its own game. What a guest carries (its items, rupees and bags) is kept
// by the host under the guest's name and given back the next time it joins: each player keeps their own things in the
// host's world.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common.h"

namespace coop {

// (a guest) the host's save into one of this game's save slots (its own saves copied aside first): the slot, -1 when
// there is no save folder yet (a game never saved: it starts a new game instead)
int coopsave_install(const std::vector<uint8_t>& caption, const std::vector<uint8_t>& progress);
// the guest's own saves put back when a copy of them is waiting (true: put back). Any thread with no game loaded from them
bool coopsave_restore(const char* why);
// (a guest about to start a new game with the host: the game's New Game writes over the autosave) its own saves aside
bool coopsave_protect();
bool coopsave_backup_waiting();

// (the host, its game's threads: the title's update with in_play false, Link's or the pause menu's with it true) the
// guests' requests for its save answered, the guests' items kept
void coopsave_host_tick(bool in_play);
// (a guest, Link's update) its items given back once its game is in the host's world, and sent to the host as they go
void coopsave_items_tick(Ctx* c, int64_t now);
// (a guest, Link's update) true once after its game loaded the host's world: the guest arrives with full hearts (the
// host's save holds the host's hearts)
bool coopsave_take_arrival();

}  // namespace coop
