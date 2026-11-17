// Shared enemies: an enemy that another player is near is run by one game only, the one whose player is nearest to it;
// the other games show their own copy of it doing what it does there (its place, its turn and every bone of its pose), so
// every player sees the same fight: the enemy going for the player it attacks, its attacks and its reactions. An enemy one
// game made while it ran (a Peahat's larvae...) that another game does not have is shown there by a stand-in of it. Hits on
// it are shared too: the life one game takes from it is taken from its copies in the others.
#pragma once

#include "common.h"

namespace coop {

// (Link's update, every frame: this game's enemies near other players sent, the others' copied)
void enemies_update(Ctx* c, uint64_t mover, const char* map, bool loading, int64_t now);
// (the life hook: a hit on an enemy here, its life from before to after)
void enemies_hit(uint64_t actor, uint32_t before, uint32_t after);
// (developer test: a hit of that much life on that enemy (its GameActor), taken and sent as the life hook would)
void enemies_hit_test(uint64_t ga, int amount);
// (a hit from another game: that much life taken from this game's copy; owner and id: the enemy as its game knows it)
void enemies_take_hit(Ctx* c, const std::string& map, const std::string& name, const float home[3], int amount, int owner, uint32_t id);

}  // namespace coop
