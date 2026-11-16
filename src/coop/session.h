// Longshot Co-op session: hosting, joining, the lobby, the players' states, invitations, gifts and shared events.
// All networking runs on the session's own thread; the game's threads and the overlay read snapshots under a lock.
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "net.h"

namespace coop {

constexpr int kMaxPlayers = 4;

// What a game says about its Link, many times a second (unreliable, newest wins)
struct PlayerState {
    uint32_t seq = 0;
    float pos[3] = {0, 0, 0};
    float yaw = 0;
    float speed = 0;
    float cam_yaw = 0;                  // (where the player's camera looks: the light of their map marker)
    int16_t life = -1, life_max = -1;   // (the game's life units: 4 a heart)
    uint8_t child = 0;
    uint8_t flags = 0;                  // kInPlay, kLoading, kEvent, kPaused
    char map[48] = {0};
    int64_t got_ms = 0;                 // (when it arrived here)
};
enum : uint8_t { kInPlay = 1, kLoading = 2, kEvent = 4, kPaused = 8, kTitle = 16, kDowned = 32 };

struct Player {
    bool used = false;
    std::string name;
    bool host = false, ready = false, me = false;
    int ping = -1;
    PlayerState st, prev;   // the last two, for smooth motion
    bool starter_sent = false;   // (the host: its starting gear sent to this player)
    std::string gear;            // what their Link wears: "weapon|shield|head|tunic|boots|hands" (the game's actor names)
};

struct ServerEntry {
    net::Addr addr;          // its play socket
    std::string server, host;
    int players = 0, max = kMaxPlayers;
    bool in_game = false, locked = false, invite = false;
    uint32_t session = 0;
    int64_t seen_ms = 0;
};

struct FriendEntry {
    net::Addr find;          // its discovery socket (invitations go there)
    std::string name;
    bool busy = false;       // in a session already
    int64_t seen_ms = 0;
};

enum class Role { None, Host, Guest };
enum class Phase { Idle, Connecting, Lobby, Starting, Playing };

struct HostSettings {
    std::string server = "Hyrule Co-op";
    int max_players = 4;
    std::string password;
    bool join_in_progress = true;   // players may join after the game started
    bool share_kills = true;        // an enemy or boss killed in one game dies in the others
    bool gifts = true;              // Give To between players
    bool same_age = true;           // guests take the host's age when they join
    bool start_with_host = true;    // guests are brought to the host's map when the game starts
    bool revive = true;             // a player who falls waits for the others to revive them (or gives up)
    bool respawn_for_new = true;    // enemies already defeated come back for players who join later
    bool starter = true;            // a player who joins gets the starting gear the host has (what they do not have yet)
    uint16_t port = net::kPortPlay;
};

struct GuestSettings {
    bool nameplates = true;         // names and life over the other players
    bool hud = true;                // the players' list under the hearts
    bool far_names = true;          // nameplates through walls and from afar
    int hud_scale = 100;            // percent
    bool gift_prompt = true;        // ask before taking a gift (off: take them at once)
    bool sounds = true;             // the menus' sounds for notices
    int smoothing = 2;              // 0 off, 1 light (50 ms), 2 normal (80 ms), 3 strong (120 ms)
    bool autosave = true;           // the game saved every few minutes while in a session
};

struct Toast {
    std::string text;
    int kind = 0;                   // 0 info, 1 good, 2 warning
    int64_t at_ms = 0;
};

// A gift on its way: offered by `from` to `to`, accepted or declined, then sent with an amount
struct Gift {
    uint32_t id = 0;
    int from = -1, to = -1;
    std::string item, label;
    int have = 0;                   // (the giver's count when offered)
    int amount = 0;
    int state = 0;                  // kGiftOffered, kGiftAccepted, kGiftDeclined, kGiftSent, kGiftDone, kGiftFailed
    int64_t at_ms = 0;
};
enum { kGiftOffered = 1, kGiftAccepted, kGiftDeclined, kGiftSent, kGiftDone, kGiftFailed };

// Something the game's thread has to do for the session
struct GameAction {
    enum Kind { AddItem, RemoveItem, Kill, GoToHost, StartGame, Leave, Revive, SaveNow, Starter, EnemyHit } kind;
    std::string s;
    int n = 0;
    uint64_t key = 0;
    float pos[3] = {0, 0, 0};
};

// A shared enemy as a game sees it (the player nearest to it runs it, its lead; the others' games copy it): known in every
// game by its actor's name and where it was placed (its home), and in its own game by an id (the same while it lives
// there), with its place, its turn (3 x 3, rows) and its pose (pose.cpp's form, every bone). Enemies a game runs that the
// others are near are sent without the lead too: a game that has no such enemy (one made while it ran: a Peahat's larvae)
// shows a stand-in of it
struct EnemySnap {
    int owner = -1;
    uint32_t id = 0;
    bool lead = true;
    std::string map, name;
    float home[3] = {0, 0, 0}, pos[3] = {0, 0, 0}, rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::vector<uint8_t> pose;
    int64_t got_ms = 0;
};

// A placed object's state shared between games (world.cpp): GameData's object flag (its 64-bit key: the object's
// placement hash, or that ^ 2) and its value as the game keeps it
struct WorldFlag {
    uint64_t key = 0;
    uint32_t value = 0;
};

// A kill shared between games: an enemy by its actor's name and where it fell, in a map
struct KillEvent {
    uint32_t id = 0;
    std::string actor;
    float home[3] = {0, 0, 0};
    std::string map;
};

struct SessionView {
    Role role = Role::None;
    Phase phase = Phase::Idle;
    int me = -1;
    Player players[kMaxPlayers];
    std::string server, host_ip, status;
    bool server_running = false;
    int host_file = -1;
    HostSettings host;
};

class Session {
public:
    static Session& get();

    // ---- from the menus (any thread)
    void set_name(const std::string& name);
    std::string name();
    bool host_start(const HostSettings& s, int file);    // the server up, the lobby open
    void host_stop();                                    // the server down (the lobby stays, empty)
    bool host_resume();                                  // the server up again
    bool join(const net::Addr& addr, const std::string& password);
    void leave();                                        // out of the session, back to the menus
    void set_ready(bool ready);
    void start_game();                                   // (host) everyone into the game
    void invite(const FriendEntry& f);
    void kick(int slot);
    void searching(bool on);                             // the join menu is open: hosts' beacons listened for, presence sent

    // ---- from the game (its threads)
    void push_state(const PlayerState& st);              // Link's state now (sent at most every frame)
    void push_pose(const std::vector<uint8_t>& blob);    // Link's skeleton pose now (pose.cpp's form; sent unreliable)
    void push_gear(const std::string& gear);             // what Link wears now (sent when it changes, and every 5 s)
    std::string gear(int slot);                          // what that player's Link wears ("" not known yet)
    // a player's newest pose when it is not the one the caller has (seq): false when there is none newer
    bool take_pose(int slot, uint32_t have, std::vector<uint8_t>* blob, uint32_t* seq);
    bool pop_action(GameAction* out);                    // what the game has to do
    void kill_happened(const KillEvent& k);              // an enemy died here: the others' copies die too
    std::vector<KillEvent> take_kills();                 // the others' kills come since the last call
    // (the state of placed objects: what changed in this game, to the others (reliable, the host relays); and what the
    // others' games changed, since the last call)
    void send_world_flags(const std::string& map, const std::vector<WorldFlag>& flags);
    std::vector<WorldFlag> take_world_flags();
    // the players' newest two states (for the game's every frame: no strings copied); false when not in a session
    bool states(PlayerState st[kMaxPlayers], PlayerState prev[kMaxPlayers], bool used[kMaxPlayers], int* me);
    bool take_respawn();                                 // a player joined and the enemies come back for everyone
    void revive(int slot);                               // this player revived the one in that slot
    // ---- shared enemies (enemies.cpp)
    void send_enemy(const EnemySnap& e);                 // (this game runs it: sent unreliable to the others)
    std::vector<EnemySnap> enemies();                    // (the others' newest, each enemy once, fresh ones)
    // (a hit on an enemy here: the same life taken from its copies in the others' games)
    // (owner, id: the enemy as the game it comes from knows it; owner -1: this game's own)
    void send_enemy_hit(const std::string& map, const std::string& name, const float home[3], int amount, int owner, uint32_t id);
    // (the host's game: its starting gear, each item's pouch name and count, for players who join)
    void set_starter(const std::vector<std::pair<std::string, int>>& have);

    // ---- gifts
    uint32_t gift_offer(int to, const std::string& item, const std::string& label, int have);
    void gift_reply(uint32_t id, bool accept);           // (the one offered to)
    void gift_send(uint32_t id, int amount);             // (the giver, once accepted: the amount taken from its game)
    void gift_cancel(uint32_t id);
    std::vector<Gift> gifts();
    void gift_result(uint32_t id, bool ok, int moved);   // (the game's side of an add or a remove: how many really moved)

    // ---- for the overlay and the menus
    SessionView view();
    std::vector<ServerEntry> servers();
    std::vector<FriendEntry> friends();
    std::vector<Toast> toasts();
    void toast(const std::string& text, int kind = 0);
    std::vector<KillEvent> kills_for(const std::string& map);
    HostSettings host_settings();
    void set_host_settings(const HostSettings& s);
    GuestSettings guest_settings();
    void set_guest_settings(const GuestSettings& s);
    bool active();   // in a session (hosting or joined)
    bool others_present();      // another player is in the session (cheap: no copies)
    int64_t last_toast_ms();    // when the last notice came (net::now_ms)

private:
    Session();
    void thread_main();
    struct Impl;
    Impl* d;
};

}  // namespace coop
