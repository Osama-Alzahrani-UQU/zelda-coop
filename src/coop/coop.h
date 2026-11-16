// Longshot Co-op: the parts of the mod and how they meet.
#pragma once

#include <string>

#include "common.h"

struct ImGuiContext;

namespace coop {

// ---- game.cpp: the game as the mod sees it (game threads)
void game_player_update(Ctx* c, uint64_t actor);   // Link's update, each frame in play
void game_menu_update(Ctx* c, uint64_t screen);    // a menu screen's update
void game_text_draw(Ctx* c, uint64_t box);          // a text box about to be drawn
void game_camera_update(Ctx* c, uint64_t camera);   // a camera's lens built (its vertical angle in s0)
// an actor's life lowered (before -> after, its address; the player's or another actor's): game threads, any time
void game_actor_damaged(Ctx* c, uint64_t actor, uint32_t before, uint32_t after, uint64_t life_addr, bool player);
struct GameAction;
void game_queue_action(const GameAction& a);        // (the session's: done at Link's next update)
bool pause_menu_open();                             // the game's pause menu is up
uint64_t pause_menu_screen();                       // its screen object (0: not up)
std::string pause_focused_item(bool* equipment);    // the item its Items or Equipment page points at ("" none)
std::string menu_screens_seen();                    // (research: the menu screens updated lately)
bool game_in_play();                                // Link is being updated (in play, not in a menu or a load)
// A save of the game in play into its own slot, at Link's next update (any thread; the window closing, the server closing,
// a player leaving). game_save_state: kSaveIdle, kSaveAsked, kSaveRunning, kSaveDone, kSaveFailed
enum { kSaveIdle = 0, kSaveAsked, kSaveRunning, kSaveDone, kSaveFailed };
void game_save_now(const char* why, bool quiet = false);   // (quiet: no notice when written)
int game_destination_of(const char* map, bool child);
int game_raycast(Ctx* c, const float from[3], const float to[3], int layer, int sublayer, int mask, int submask, float* dist);   // (1 hit)      // the debug teleport's destination of a map, -1 none
void game_save_stop();   // (the window is closing after its save: none starts any more)
int game_save_state();
void game_warp_player(Ctx* c, const float to[3]);   // (tests: Link moved there, at his next update)
uint64_t spawn_actor(Ctx* c, const char* name, const float pos[3], float yaw, int variant = 0);   // (research: ActorMgr's request)

// ---- input.cpp: the player's controller and keyboard for the mod's menus
void input_from_game(uint64_t states, int count);   // the game's GetNpadStates (No.1): the states written there
void input_window_hook();                            // the game's window: keyboard and mouse for the menus

// ---- overlay.cpp: drawing over the game (the presenting thread, inside the renderer's ImGui frame)
bool overlay_wanted();
void overlay_hit_test(int slot);   // (developer probe: a hit shown on that player's nameplate)
void overlay_draw(ImGuiContext* ctx, uint32_t w, uint32_t h);

// ---- probe.cpp: developer probe (bin\coop\probe_on): guest calls from bin\coop\probe.txt on the game's thread
bool debug_on();   // (bin\coop\probe_on: research and tests)
void probe_poll_file();
void probe_run(Ctx* c, int where);   // (1 a menu screen's update, 2 Link's, 3 a camera's lens, 4 a mover's matrix)
void probe_pins(int where);          // (the probe's pinned bytes written from that hook)

}  // namespace coop
