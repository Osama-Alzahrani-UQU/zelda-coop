// The shared world: what one player does to the world's placed objects (a door unlocked, a switch pressed, a spider web
// broken, a torch lit for good, a boulder blown up...) is done in the other players' worlds too. Each game keeps its own
// world (its own save); the objects' state goes from game to game as the game keeps it: GameData's object flags.
#pragma once

#include "common.h"

namespace coop {

// (Link's update, every frame: this game's object changes sent, the others' put in this world)
void world_update(Ctx* c, uint64_t mover, const char* map, bool loading, int64_t now);
// Blows on placed objects, live: grass cut, a pot broken, a web burnt, an eye switch struck. The life hook sees a placed
// object's life go down here (any thread): the blow goes to the others by the object's placement (the same in every
// game); there it is given to the same object as a blow of the game's own (Link's update), so it is cut, broken or
// struck there as it is here, its own effect and its own drop
void world_object_hit(uint64_t body, uint32_t before, uint32_t after);
void world_take_object_hit(Ctx* c, const std::string& map, const std::string& name, uint64_t placement, int amount);

}  // namespace coop
