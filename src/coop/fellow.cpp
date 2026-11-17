// Playing together in the world (fellow.h): kills shared between the games, and falling, reviving and giving up.
//
// Kills: when an enemy's life reaches 0 here (the life hook), its name, where it fell and the map go to the others. Each
// other game, when its player is in that map, finds its own copy of that enemy (the same name, the nearest to where it fell)
// and sets its life to 0: the game plays the enemy's death and drops that game's own loot, so every player finds the drop
// on the ground in their own world. An enemy far from the player falls when the player comes near (40 m), so the loot is
// there when they arrive; a boss falls at once. A kill is carried out once: afterwards the game's own rules bring the
// enemies back as it always does (a map entered again).
//
// Falling: with another player in the session (and the host's Revive on), the hit that would end Link's life is held: he
// stays down, frozen, until another player holds A beside him for two seconds (he gets up with half his hearts), or he
// holds - to give up (or two minutes pass): full hearts at the entrance of the area, his items all kept.
#include "fellow.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

#include "coop.h"
#include "game.h"
#include "menus.h"
#include "runtime.h"
#include "session.h"

namespace coop {

namespace {
constexpr uint64_t kGameActorVt = 0x84b45e18ULL, kActorBodyVt = 0x84b6c460ULL;
constexpr int kBodyComponent = 87;
constexpr float kMatchRadius = 14.0f;    // (the same enemy in the other game: as near as this to where it fell)
constexpr float kApplyDistance = 40.0f;  // (an enemy falls when the player is this near)
constexpr float kBossDistance = 30.0f;   // (a boss: when the player is in its room and no event plays: its fight begun)
constexpr int64_t kSecond = 1000000000LL;
constexpr int64_t kBleedOut = 120 * kSecond;   // (down this long: given up by itself)
constexpr int64_t kReviveHold = 2 * kSecond, kGiveUpHold = 1500000000LL;
constexpr float kReviveReach = 3.0f;

bool heap(uint64_t a) { return a >= kHeapBase && a < kHeapBase + 0x100000000ULL; }

float dist(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

bool is_boss(const std::string& name) { return name.find("Boss") != std::string::npos; }

// ---- the fall
std::atomic<int64_t> g_down_since{0};
std::atomic<bool> g_forget_kills{false};
std::atomic<int64_t> g_life_held{0};         // (the life Link was held at)
std::atomic<bool> g_give_up{false};          // (asked: by the player's hold, or the bleed-out)
std::atomic<int> g_revived_by{-1};           // (the session: revived, by that slot)
// (the controller, main thread)
int64_t g_giveup_hold_since = 0, g_revive_hold_since = 0;
std::atomic<float> g_giveup_progress{0}, g_revive_progress{0};
std::atomic<int> g_revive_target{-1};        // (from Link's update: a fallen player in reach)
std::atomic<bool> g_a_taken{false};
}  // namespace

// ---- the game's actors
uint64_t game_actor_of(uint64_t body) {
    if (!heap(body) || r64(body) != kActorBodyVt) return 0;
    const uint64_t ga = r64(body + 24);
    return heap(ga) && r64(ga) == kGameActorVt ? ga : 0;
}

std::string game_actor_name(uint64_t ga) {
    const uint64_t s = r64(ga + 0x260);
    if (s < 0x80000000ULL || !guest_ok(s, 64)) return {};
    std::string n = cstr_at(s, 63);
    for (char ch : n)
        if (ch < 32 || ch > 126) return {};
    return n;
}

bool game_actor_pos(uint64_t ga, float out[3]) {
    for (int i = 0; i < 3; i++) out[i] = rf(ga + 0x31c + 4 * (uint64_t)i);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

uint64_t game_actor_life(uint64_t ga) {
    const uint64_t comps = r64(ga + 0x270);
    if (!heap(comps) || (int)r32(ga + 0x278) <= kBodyComponent) return 0;
    const uint64_t body = r64(comps + 8 * kBodyComponent);
    if (!heap(body) || r64(body) != kActorBodyVt || r64(body + 24) != ga) return 0;
    const uint64_t life = r64(body + 0x1c10);
    if (!heap(life) || r64(life + 0x28) != ga) return 0;
    return life + 8;
}

// ---- kills: this game's
namespace {
// (the enemies whose death this game sent, or carried out for another game: one message an enemy (the hook may see its
// last hit twice), and none back for a kill that came from another game)
std::mutex g_recent_mutex;
uint64_t g_recent[64];
int g_recent_at = 0;
bool kill_seen(uint64_t ga, bool add) {
    std::lock_guard<std::mutex> lock(g_recent_mutex);
    for (uint64_t r : g_recent)
        if (r == ga) return true;
    if (add) g_recent[g_recent_at++ % 64] = ga;
    return false;
}
}  // namespace

// The game's own way to change an actor's life: a life message to its body (the body's vtable +0x90, f_82e67f60, as the
// scripts' OneShotLifeSetOwnLife sends it: x1 the message, +4 its kind 0x46030000, +0x10 the change: +0 the amount, +8
// flags, 1 the amount). A negative amount is a blow: at the body's next update (f_804bd8c0) it goes through the game's
// damage as a hit does, so the enemy's own death plays and its loot drops. (Its life written to 0 instead leaves it
// standing at 0.)
bool fellow_kill_actor(Ctx* c, uint64_t ga) {
    if (!heap(ga) || r64(ga) != kGameActorVt) return false;
    kill_seen(ga, true);   // (its death here is another game's kill: not sent back)
    return fellow_life_message(c, ga, -100000);
}

bool fellow_life_message(Ctx* c, uint64_t ga, int change) {
    constexpr uint64_t kFnLifeMessage = 0x82e67f60ULL;
    constexpr uint32_t kLifeChange = 0x46030000u;
    const uint64_t comps = heap(ga) ? r64(ga + 0x270) : 0;
    if (!heap(comps) || (int)r32(ga + 0x278) <= kBodyComponent) return false;
    const uint64_t body = r64(comps + 8 * kBodyComponent);
    if (!heap(body) || r64(body) != kActorBodyVt || r64(body + 24) != ga) return false;
    if (r64(kActorBodyVt + 0x90) != kFnLifeMessage || !rt().a64_lookup(kFnLifeMessage)) return false;
    static uint64_t buf = 0;
    if (!buf) buf = guest_alloc(0x40, 16);
    if (!buf) return false;
    w32(buf + 0, 0xffffffffu);
    w32(buf + 4, kLifeChange);
    w32(buf + 8, 0xffffffffu);
    w32(buf + 0xc, 0);
    w64(buf + 0x10, buf + 0x20);
    w32(buf + 0x20, (uint32_t)change);
    w32(buf + 0x24, 0);
    w32(buf + 0x28, 1);
    guest_call(c, kFnLifeMessage, body, buf);
    return true;
}

void fellow_enemy_died(uint64_t body) {
    Session& ses = Session::get();
    if (!ses.active()) return;
    const uint64_t ga = game_actor_of(body);
    if (!ga) return;
    if (kill_seen(ga, true)) return;
    const std::string name = game_actor_name(ga);
    if (name.rfind("Enemy_", 0) != 0) return;   // (enemies and bosses: not pots, grass or the players)
    KillEvent k;
    k.actor = name;
    game_actor_pos(ga, k.home);
    for (int i = 0; i < 3; i++) k.spawn[i] = rf(ga + 0x2ec + 4 * (uint64_t)i);
    k.has_spawn = std::isfinite(k.spawn[0]) && std::isfinite(k.spawn[1]) && std::isfinite(k.spawn[2]);
    k.map = local_snapshot().map;
    if (k.map.empty()) return;
    log("kill: %s at %.1f %.1f %.1f in %s", name.c_str(), k.home[0], k.home[1], k.home[2], k.map.c_str());
    ses.kill_happened(k);
}

namespace {
struct Pending {
    KillEvent k;
    int64_t in_map_since = 0;
    bool logged = false;
};

struct Enemy {
    uint64_t ga, life;
    std::string name;
    float pos[3];
    float spawn[3];
};

// the enemies alive in this map: the ring of actors from Link's
std::vector<Enemy> living_enemies(uint64_t mover) {
    std::vector<Enemy> out;
    uint64_t ga = mover;
    for (int steps = 0; steps < 8192; steps++) {
        const uint64_t next = r64(ga + 0x190);
        if (!heap(next)) break;
        ga = next - 0x190;
        if (ga == mover || r64(ga) != kGameActorVt) break;
        const uint64_t s = r64(ga + 0x260);
        if (s < 0x80000000ULL || r32(s) != 0x6d656e45u || r16(s + 4) != 0x5f79) continue;   // ("Enemy_")
        Enemy e{ga, game_actor_life(ga), game_actor_name(ga), {0, 0, 0}, {0, 0, 0}};
        if (!e.life || (int32_t)r32(e.life) <= 0 || !game_actor_pos(ga, e.pos)) continue;
        for (int i = 0; i < 3; i++) e.spawn[i] = rf(ga + 0x2ec + 4 * (uint64_t)i);
        out.push_back(std::move(e));
    }
    return out;
}

void apply_kills(Ctx* c, uint64_t mover, const char* map, std::vector<Pending>& pending, int64_t now) {
    bool here = false;
    for (const auto& p : pending) here |= p.k.map == map;
    if (!here) return;
    std::vector<Enemy> enemies = living_enemies(mover);
    float me[3];
    game_actor_pos(mover, me);
    for (auto it = pending.begin(); it != pending.end();) {
        if (it->k.map != map) {
            ++it;
            continue;
        }
        if (!it->in_map_since) it->in_map_since = now;
        const bool boss = is_boss(it->k.actor);
        // (the same enemy: placed at the same spot by the map (its spawn, the same in every game); else the nearest of
        // that name to where it fell there)
        Enemy* best = nullptr;
        float best_d = boss ? 1e9f : kMatchRadius;
        if (it->k.has_spawn)
            for (auto& e : enemies)
                if (e.ga && e.name == it->k.actor && dist(e.spawn, it->k.spawn) < 1.5f) {
                    best = &e, best_d = dist(e.pos, it->k.home);
                    break;
                }
        if (!best)
            for (auto& e : enemies) {
                if (!e.ga || e.name != it->k.actor) continue;
                const float d = dist(e.pos, it->k.home);
                if (d < best_d) best_d = d, best = &e;
            }
        if (!best) {
            if (!it->logged) {
                it->logged = true;
                int same = 0;
                for (auto& e : enemies) same += e.name == it->k.actor;
                log("kill: %s not found near %.1f %.1f %.1f (%d alive of that name, %zu enemies here)", it->k.actor.c_str(), it->k.home[0],
                    it->k.home[1], it->k.home[2], same, enemies.size());
            }
            // (not found here: already down in this game, or not made yet; given up after a while in the map)
            if (now - it->in_map_since > 90 * kSecond) it = pending.erase(it);
            else ++it;
            continue;
        }
        if (dist(best->pos, me) > (boss ? kBossDistance : kApplyDistance)) {   // (a boss: in its room, the fight on)
            ++it;
            continue;
        }
        // (a blow of the game's own: the enemy's death and its loot; its life written to 0 when its body takes none)
        const bool blow = fellow_kill_actor(c, best->ga);
        if (!blow) InterlockedExchange((volatile LONG*)gp(best->life), 0);
        log("kill: %s fell here too (%.1f m from where it fell there, %.1f m from Link)%s", best->name.c_str(), best_d,
            dist(best->pos, me), blow ? "" : " (its life set)");
        best->ga = 0;   // (taken: another kill of the same name finds another one)
        it = pending.erase(it);
    }
}

// Link's life: its struct at *(player + 7184): the value at +8, the most at +0x18
uint64_t link_life(uint64_t player) {
    const uint64_t s = r64(player + 7184);
    return heap(s) ? s : 0;
}

void set_life(uint64_t player, int value) {
    if (const uint64_t s = link_life(player)) InterlockedExchange((volatile LONG*)gp(s + 8), value);
}

void get_up(uint64_t player, int life, const char* why) {
    set_life(player, life);
    g_down_since = 0;
    g_give_up = false;
    g_revived_by = -1;
    log("fall: up again (%s), life %d", why, life);
}
}  // namespace

void fellow_revived(int from) { g_revived_by = from; }

void fellow_update(Ctx* c, uint64_t player, uint64_t mover, const char* map, bool child, bool loading, bool event, int64_t now) {
    Session& ses = Session::get();
    // ---- the others' kills
    static std::vector<Pending> pending;
    if (g_forget_kills.exchange(false)) pending.clear();   // (a player joined: the defeated enemies come back for everyone)
    static int64_t next_kills = 0;
    if (now >= next_kills) {
        next_kills = now + 250000000LL;
        if (ses.take_respawn()) pending.clear();
        for (auto& k : ses.take_kills()) pending.push_back({std::move(k), 0});
        if (pending.size() > 2048) pending.erase(pending.begin(), pending.begin() + 512);
        if (!ses.active()) pending.clear();
        else if (!pending.empty() && !loading && !event && map[0]) apply_kills(c, mover, map, pending, now);
    }
    // ---- the fall
    const int64_t down = g_down_since.load();
    if (down) {
        const uint64_t ls = link_life(player);
        const int most = ls ? (int)r32(ls + 0x18) : 12;
        const int by = g_revived_by.load();
        if (by >= 0) {
            get_up(player, std::max(4, (most / 2 + 3) / 4 * 4), "revived");
        } else if (g_give_up.load() || now - down > kBleedOut || !ses.active() || !ses.others_present()) {
            // (given up: full hearts at the entrance of the area, nothing lost)
            get_up(player, most, "gave up");
            const int dst = game_destination_of(map, child);
            if (dst >= 0 && rt().hos_oot_teleport_ask) {
                rt().hos_oot_teleport_ask(dst);
                ses.toast("Back at the entrance, all your items kept", 0);
            } else {
                ses.toast("Up again, all your items kept", 0);
            }
        }
    }
    // ---- a fallen player in reach (for the controller's A)
    static int64_t next_reach = 0;
    if (now >= next_reach) {
        next_reach = now + 100000000LL;
        int target = -1;
        if (!down && ses.others_present()) {
            const SessionView v = ses.view();
            float me[3];
            game_actor_pos(mover, me);
            float best = kReviveReach;
            for (int i = 0; i < kMaxPlayers; i++) {
                const Player& p = v.players[i];
                if (!p.used || p.me || !(p.st.flags & kDowned) || strcmp(p.st.map, map) != 0) continue;
                const float d = dist(p.st.pos, me);
                if (d < best) best = d, target = i;
            }
        }
        g_revive_target = target;
    }
}

bool fellow_guard_life(uint32_t before, uint32_t after) {
    if (g_down_since.load()) return true;   // (down: nothing hurts him more)
    if (after != 0 || before == 0) return false;
    Session& ses = Session::get();
    if (!ses.active() || !ses.others_present() || !ses.host_settings().revive) return false;   // (alone: the game's Game Over)
    g_life_held = before;
    g_give_up = false;
    g_revived_by = -1;
    g_down_since = now_ns();
    log("fall: Link is down (life held at %u)", before);
    ses.toast("You are down! Another player can revive you", 2);
    return true;
}

bool fellow_downed() { return g_down_since.load() != 0; }

void fellow_forget_kills() { g_forget_kills = true; }

float fellow_downed_left() {
    const int64_t down = g_down_since.load();
    return down ? std::max(0.0f, (float)(kBleedOut - (now_ns() - down)) / 1e9f) : 0.0f;
}

float fellow_giveup_progress() { return g_giveup_progress.load(); }

int fellow_revive_target(float* progress) {
    if (progress) *progress = g_revive_progress.load();
    return g_revive_target.load();
}

void fellow_input(uint64_t* buttons, int32_t stick[4], uint64_t pressed) {
    (void)pressed;
    const int64_t now = now_ns();
    const uint64_t raw = *buttons;
    if (fellow_downed()) {
        // (down: frozen; - held gives up)
        if (raw & kMinus) {
            if (!g_giveup_hold_since) g_giveup_hold_since = now;
            const float p = std::min(1.0f, (float)(now - g_giveup_hold_since) / (float)kGiveUpHold);
            g_giveup_progress = p;
            if (p >= 1.0f) g_give_up = true;
        } else {
            g_giveup_hold_since = 0;
            g_giveup_progress = 0;
        }
        *buttons = 0;
        for (int i = 0; i < 4; i++) stick[i] = 0;
        return;
    }
    g_giveup_hold_since = 0;
    g_giveup_progress = 0;
    // (beside a fallen player: A held revives them; the game does not get that A)
    const int target = g_revive_target.load();
    if (target >= 0 && (raw & kA)) {
        if (!g_revive_hold_since) g_revive_hold_since = now;
        const float p = std::min(1.0f, (float)(now - g_revive_hold_since) / (float)kReviveHold);
        g_revive_progress = p;
        if (p >= 1.0f && !g_a_taken.exchange(true)) Session::get().revive(target);
        *buttons &= ~(uint64_t)kA;
        return;
    }
    if (!(raw & kA)) g_a_taken = false;
    else if (g_a_taken.load()) *buttons &= ~(uint64_t)kA;   // (the A that revived: kept from the game until let go)
    g_revive_hold_since = 0;
    g_revive_progress = 0;
}

}  // namespace coop
