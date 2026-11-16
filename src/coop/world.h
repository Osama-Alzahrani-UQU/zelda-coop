// The shared world: what one player does to the world's placed objects (a door unlocked, a switch pressed, a spider web
// broken, a torch lit for good, a boulder blown up...) is done in the other players' worlds too. Each game keeps its own
// world (its own save); the objects' state goes from game to game as the game keeps it: GameData's object flags.
#pragma once

#include "common.h"

namespace coop {

// (Link's update, every frame: this game's object changes sent, the others' put in this world)
void world_update(Ctx* c, uint64_t mover, const char* map, bool loading, int64_t now);

}  // namespace coop
