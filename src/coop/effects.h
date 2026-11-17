// The other players' projectiles and effects in this world: a Deku Nut thrown, a seed or an arrow shot, a bomb set down,
// a boomerang thrown, a Deku Stick's fire. Each game watches the actors that appear around its own Link: one the game made
// for a thing the player used (its kinds listed in effects.cpp) goes to the others with where it is and how fast it moves;
// they make the same actor at that place, in that direction, so it flies, flashes, explodes or burns there as well (its
// own effect, its own hit on what it meets there).
#pragma once

#include <string>
#include <vector>

#include "common.h"

namespace coop {

// (Link's update, every frame) this game's projectiles seen and sent; the others' made here
void effects_update(Ctx* c, uint64_t mover, const char* map, bool loading, int64_t now);
void effects_test_spawn(const std::string& name, float up);   // (developer probe: that actor made in front of Link)
// (any thread) a place where another player's blast went off here a moment ago: the hits there are that blast's, sent by
// its own game (not sent from here)
bool effects_remote_blast_near(const float pos[3]);
// (the life hook, any thread) an actor's life went down: a bomb of this player's going off is sent at once
void effects_actor_struck(uint64_t body, uint32_t after);

// The other players' projectiles, drawn by the overlay where they are (they are not actors of this world: the game makes
// its seeds, nuts and arrows ahead and only launches them, so they cannot be made here as they fly). An end shows as the
// thing's own burst: a Deku Nut's white flash, a seed's or an arrow's spark.
enum { kProjSeed = 1, kProjNut, kProjArrow, kProjHook };
struct ProjectileView {
    int owner = -1, kind = 0;
    float pos[3] = {0, 0, 0}, prev[3] = {0, 0, 0};
    bool end = false;
    float age = 0;   // (seconds since its end, or since it was last heard of)
};
std::vector<ProjectileView> effects_projectiles();   // (any thread: the overlay)

// The Deku Stick's fire: a fire that lights a stick sends the one holding it PerformIgniteUseDekuStick; one sent near this
// player's Link (its stick out) is this player's stick lit: the others are told, and their games draw the flame on the
// stick this player's Link holds there (until it burns down, about 20 s, or is put away)
void effects_stick_lit(uint64_t sender);   // (the message hook, any thread: the actor the message came from)
void effects_test_stick(int seconds);       // (developer probe: this player's stick as lit)
struct StickFire {
    int slot = -1;
    float pos[3] = {0, 0, 0};   // (the stick's burning end, in this world)
};
std::vector<StickFire> effects_stick_fires();   // (any thread: the overlay)

}  // namespace coop
