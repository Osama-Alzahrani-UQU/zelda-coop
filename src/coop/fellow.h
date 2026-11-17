// Playing together in the world: the enemies a player defeats fall in the others' games too (each game drops its own
// loot), and a player who falls waits for the others to revive them, or gives up and goes back to the area's entrance.
#pragma once

#include <string>
#include <vector>

#include "common.h"

namespace coop {

// ---- the game's actors. Every actor is a GameActor (vtable 0x84b45e18): its pack name at *(+0x260) ("Enemy_Dekubaba"),
// its place at +0x31c, its components at *(+0x270) (count +0x278); component 87 is its actor body (vtable 0x84b6c460, back
// to the GameActor at +24), whose life is at *(+0x1c10) + 8 (max at + 0x18). They are linked in one ring at +0x190.
uint64_t game_actor_of(uint64_t body);               // the body (the life hook's actor) -> its GameActor, 0 none
std::string game_actor_name(uint64_t ga);
bool game_actor_pos(uint64_t ga, float out[3]);
uint64_t game_actor_life(uint64_t ga);               // the address of its life value (u32), 0 none

// ---- kills (game threads)
void fellow_enemy_died(uint64_t body);               // (the life hook: an actor's life reached 0 in this game)
// an enemy brought down as by a blow of the game's own (its death, its loot), not sent to the others (a kill of theirs
// carried out here): false when its body takes no life message
bool fellow_kill_actor(Ctx* c, uint64_t ga);
// a change of an actor's life as the game's own (change < 0 a blow: the actor reacts to it as to a hit, at its body's next
// update): false when its body takes no life message
bool fellow_life_message(Ctx* c, uint64_t ga, int change);
// Link's update: the others' kills carried out here, the fall and the revive (player: Link's body, mover: his GameActor)
void fellow_update(Ctx* c, uint64_t player, uint64_t mover, const char* map, bool child, bool loading, bool event, int64_t now);
void fellow_revived(int from);                       // (the session: another player revived this one)

// ---- the fall (the life hook, Link's own life going down: true keeps it as it was)
bool fellow_guard_life(uint32_t before, uint32_t after);
bool fellow_downed();
void fellow_forget_kills();                          // (enemies come back for everyone: the kills not done yet dropped)
float fellow_downed_left();                          // seconds before giving up by itself
float fellow_giveup_progress();                      // (holding the give-up button) 0..1
int fellow_revive_target(float* progress);           // a fallen player close enough to revive: their slot (-1 none)

// ---- the other players as the game's own Link (puppet.cpp): Link's update, every frame
void puppets_update(Ctx* c, const char* map, bool loading, int64_t now);
bool puppet_placed(int slot);
uint64_t puppet_actor(int slot);     // (that player's Link's GameActor while it is in the world here, 0 none)
bool player_behind_wall(int slot);   // (a wall between the camera and that player: no nameplate)
bool puppet_position(int slot, float out[3], bool* child);
// (the items of the hands that player's Link has out and the puppet cannot wear (the slingshot, a Deku Stick...): shown by
// stand-alone ones of the same name, made by puppet.cpp; by the FNV-1a of the name, 0 while not made)
void puppet_want_held(int slot, const std::vector<std::string>& names);
uint64_t puppet_held(int slot, uint32_t name_hash);   // (where that player's Link is drawn now)   // (that player's Link is in the world: the overlay's figure not drawn)

// ---- the controller in play (menus_input, no dialog open): frozen while down; A held beside a fallen player revives
void fellow_input(uint64_t* buttons, int32_t stick[4], uint64_t pressed);

}  // namespace coop
