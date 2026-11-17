// Shared enemies (enemies.h). Every game has its own copy of each enemy the map places (the same actor at the same home: its
// GameActor's +0x2ec); an enemy within reach of another player is run by the game whose player is nearest to it (its lead):
//
//   each game, 15 times a second: its enemies near another player, each with its id (its GameActor's place in the heap),
//           its name and home, its place (+0x31c), its turn (+0x328: 3 x 3, rows), its pose (pose.cpp: every bone) and
//           whether it leads it, to the others (unreliable; the host relays). The ones it leads go first.
//   the others: an enemy led by another game is copied by this game's own of it (the same name, its home within 1.5 m):
//           put at that place and turn every frame (carried forward by its speed, as the players are) and drawn in that
//           pose at the camera's lens; its own game goes on running it, but what is seen is what the game leading it does:
//           going for the player it chose, its attacks, its reactions, its death. A game copying an enemy does not lead it.
//   stand-ins: an enemy another game sends that this game has none of (made while that game ran: a Peahat's larvae, an
//           enemy of a room this game has not loaded) is shown by a stand-in: the same actor asked of the game (as the
//           puppets are), moved and posed as a copy is, whether that game leads it or not; when it stops coming the stand-in
//           is put away under the map (kept there for the next one of that name). A stand-in gives way to this game's own
//           copy when one turns up (its room loaded). Stand-ins are never sent.
//   hits:   a hit on an enemy in any game takes the same life from it in the others (the hook sees each hit; the life taken
//           there is written here, which the hook does not see: no echo); found there by its id when it is theirs, else as
//           their copy or stand-in of it, else by its name and home. Its death is shared as before (fellow.cpp).
//   hand-over: a player nearer to an enemy than the one leading it by 3 m takes it over (each side decides the same way)
#include "enemies.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "coop.h"
#include "effects.h"
#include "fellow.h"
#include "game.h"
#include "pose.h"
#include "runtime.h"
#include "session.h"

namespace coop {

namespace {
constexpr uint64_t kGameActorVt = 0x84b45e18ULL, kFnSetTransform = 0x804e6630ULL, kFnActivate = 0x8038fbd0ULL;
constexpr float kShareRadius = 35.0f;      // (an enemy this near another player is shared with them)
constexpr float kHandOver = 3.0f;
constexpr float kHomeMatch = 1.5f;
constexpr int kMaxSent = 12;               // (led enemies sent a round: the nearest to the others, then the rest in turn)
constexpr int kMaxStandIns = 12;
constexpr int64_t kRoundNs = 66000000LL;   // (15 times a second)
constexpr int64_t kStaleNs = 400000000LL;

bool heap(uint64_t a) { return a >= kHeapBase && a < kHeapBase + 0x100000000ULL; }

float dist3(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

uint32_t id_of(uint64_t ga) { return (uint32_t)((ga - kHeapBase) >> 3); }

// ---- the stand-ins: actors asked of the game by name (spawn_actor), in the world once made (f_8038fbd0 on their request's
// record at state 3), kept for this map; one not in use is put under the map, each frame
struct StandIn {
    int key = 0;
    std::string name;
    uint64_t link = 0, ga = 0;
    int state = 1;   // 1 asked, 2 in the world
    bool used = false;
    float park[3] = {0, 0, 0};
    int64_t asked_at = 0;
};
std::vector<StandIn> g_pool;
std::string g_pool_map;
int g_next_key = 1;

bool stand_in_alive(const StandIn& s) {
    return s.state == 2 && heap(s.ga) && r64(s.ga) == kGameActorVt && game_actor_name(s.ga) == s.name;
}

StandIn* stand_in(int key) {
    for (StandIn& s : g_pool)
        if (s.key == key) return &s;
    return nullptr;
}

bool is_stand_in(uint64_t ga) {
    for (const StandIn& s : g_pool)
        if (s.state == 2 && s.ga == ga) return true;
    return false;
}

// (a stand-in for an enemy of this name: one put away taken back, else one asked of the game: in the world some frames on)
int stand_in_for(Ctx* c, const std::string& name, const float pos[3], const float rot[9], int64_t now) {
    for (StandIn& s : g_pool)
        if (!s.used && s.name == name && (s.state == 1 || stand_in_alive(s))) {
            s.used = true;
            return s.key;
        }
    if ((int)g_pool.size() >= kMaxStandIns) return 0;
    StandIn s;
    s.key = g_next_key++;
    s.name = name;
    s.link = spawn_actor(c, name.c_str(), pos, atan2f(rot[2], rot[0]));
    if (!s.link) return 0;
    s.asked_at = now;
    s.used = true;
    g_pool.push_back(s);
    return s.key;
}

// an enemy of this game
struct Here {
    uint64_t ga = 0;
    std::string name;
    float pos[3] = {0, 0, 0}, home[3] = {0, 0, 0};
    uint64_t placement = 0;   // (the map's placement: the same enemy in every game; 0 one made in play)
};

// (an actor's placement by the map: its record *(+0x3d8), the hash at +0x88; 0 for one made in play)
uint64_t placement_of(uint64_t ga) {
    const uint64_t rec = r64(ga + 0x3d8);
    const uint64_t h = heap(rec) ? r64(rec + 0x88) : 0;
    return h == ~0ULL ? 0 : h;
}

std::vector<Here> enemies_here(uint64_t mover) {
    std::vector<Here> out;
    uint64_t ga = mover;
    for (int steps = 0; steps < 8192; steps++) {
        const uint64_t next = r64(ga + 0x190);
        if (!heap(next)) break;
        ga = next - 0x190;
        if (ga == mover || r64(ga) != kGameActorVt) break;
        const uint64_t s = r64(ga + 0x260);
        if (s < 0x80000000ULL || r32(s) != 0x6d656e45u || r16(s + 4) != 0x5f79) continue;   // ("Enemy_")
        const uint64_t life = game_actor_life(ga);
        if (!life || (int32_t)r32(life) <= 0) continue;
        Here h;
        h.ga = ga;
        h.name = game_actor_name(ga);
        if (h.name.empty() || !game_actor_pos(ga, h.pos)) continue;
        for (int k = 0; k < 3; k++) h.home[k] = rf(ga + 0x2ec + 4 * (uint64_t)k);
        if (!std::isfinite(h.home[0]) || !std::isfinite(h.home[1]) || !std::isfinite(h.home[2])) continue;
        h.placement = placement_of(ga);
        out.push_back(std::move(h));
    }
    return out;
}

// an enemy of another game shown here: by this game's own of it (a copy) or by a stand-in
struct Shown {
    int owner = -1;
    uint32_t id = 0;
    uint64_t ga = 0;        // (a copy: this game's own)
    int stand_key = 0;      // (a stand-in: its key in the pool)
    std::string name;
    float home[3] = {0, 0, 0};
    float pos[3] = {0, 0, 0}, vel[3] = {0, 0, 0}, shown[3] = {0, 0, 0};
    float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    int64_t got = 0, sent_ms = 0;
    bool init = false;
};
std::vector<Shown> g_shown;
std::vector<uint64_t> g_led;   // (the enemies this game led last round)
std::vector<Here> g_here;
uint64_t g_buf = 0;

uint64_t shown_actor(const Shown& s) {
    if (!s.stand_key) return s.ga;
    const StandIn* si = stand_in(s.stand_key);
    return si && stand_in_alive(*si) ? si->ga : 0;
}

// (this game's own of an enemy another game sends, not shown for another already: one the map placed, by its placement
// (exact); one made in play (a larva, a respawned one), the nearest of that name to where it is there, its home or itself
// within reach)
const Here* counterpart(const std::string& name, const float home[3], const float pos[3], uint64_t placement = 0) {
    auto taken = [](const Here& h) {
        for (const Shown& s : g_shown)
            if (!s.stand_key && s.ga == h.ga) return true;
        return false;
    };
    if (placement) {
        for (const Here& h : g_here)
            if (h.placement == placement && h.name == name && !taken(h)) return &h;
        return nullptr;   // (a placed enemy this game does not have now: its room not loaded here, or it is down here)
    }
    const Here* best = nullptr;
    float best_d = 1e9f;
    for (const Here& h : g_here) {
        if (h.name != name || h.placement || taken(h)) continue;
        if (dist3(h.home, home) > kHomeMatch && dist3(h.pos, pos) > 6.0f) continue;
        const float d = dist3(h.pos, pos);
        if (d < best_d) best_d = d, best = &h;
    }
    return best;
}

// put an actor at a place and a turn: through its rigid body when it has one (as the warp nodes do), else as its
// transform asked for (+0x358 the place, +0x364 the turn, bit 0x8000 of +0x4a8 cleared): puppet.cpp's way
void place_actor(Ctx* c, uint64_t ga, const float pos[3], const float rot[9]) {
    if (!g_buf) g_buf = guest_alloc(0x40, 16);
    if (!g_buf) return;
    const uint64_t comps = r64(ga + 0x270);
    const uint64_t phys = heap(comps) ? r64(comps + 0x50) : 0;
    if (heap(phys) && heap(r64(phys + 0x20))) {
        for (int k = 0; k < 3; k++) wf(g_buf + 4 * (uint64_t)k, pos[k]);
        for (int i = 0; i < 9; i++) wf(g_buf + 16 + 4 * (uint64_t)i, rot[i]);
        if (guest_call(c, kFnSetTransform, phys, g_buf, g_buf + 16, 1) & 1) return;
    }
    for (int k = 0; k < 3; k++) wf(ga + 0x358 + 4 * (uint64_t)k, pos[k]);
    for (int i = 0; i < 9; i++) wf(ga + 0x364 + 4 * (uint64_t)i, rot[i]);
    InterlockedAnd((volatile LONG*)gp(ga + 0x4a8), ~(LONG)0x8000);
}

// (each frame: the stand-ins asked for put in the world once made; the ones gone (their map, their death) forgotten; the
// ones not in use kept under the map)
void stand_ins_tick(Ctx* c, const char* map, int64_t now) {
    if (g_pool_map != map) g_pool.clear(), g_pool_map = map;   // (the map's actors went with it)
    for (StandIn& s : g_pool) s.used = false;
    for (const Shown& sh : g_shown)
        if (sh.stand_key)
            if (StandIn* s = stand_in(sh.stand_key)) s->used = true;
    for (size_t i = 0; i < g_pool.size();) {
        StandIn& s = g_pool[i];
        bool drop = false;
        if (s.state == 1) {
            const uint64_t rec = r64(s.link);
            const int rs = heap(rec) ? (int)(r32(rec) & 0xffff) : 0;
            if (rs == 3) {
                const uint64_t actor = guest_call(c, kFnActivate, rec, 1, 0);
                s.ga = actor;
                s.state = 2;
                drop = !stand_in_alive(s);
                log("enemies: a stand-in %s in the world (%#llx)%s", s.name.c_str(), (unsigned long long)actor, drop ? ": not an actor" : "");
            } else if ((rs >= 4 && rs <= 6) || now - s.asked_at > 15000000000LL) {
                log("enemies: the stand-in %s was not made (record state %d)", s.name.c_str(), rs);
                drop = true;
            }
        } else if (!stand_in_alive(s)) {
            drop = true;
        } else {
            const uint64_t life = game_actor_life(s.ga);
            if (life && (int32_t)r32(life) <= 0) drop = true;   // (dying: left to its death)
            else if (!s.used) {
                const float level[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
                place_actor(c, s.ga, s.park, level);
            } else {
                float p[3];
                if (game_actor_pos(s.ga, p)) s.park[0] = p[0], s.park[1] = p[1] - 500.0f, s.park[2] = p[2];
            }
        }
        if (drop) g_pool.erase(g_pool.begin() + (ptrdiff_t)i);
        else i++;
    }
}
}  // namespace

void enemies_update(Ctx* c, uint64_t mover, const char* map, bool loading, int64_t now) {
    Session& ses = Session::get();
    PlayerState st[kMaxPlayers], prev[kMaxPlayers];
    bool used[kMaxPlayers];
    int me = -1;
    bool others = false;
    if (ses.active() && !loading && map[0] && ses.states(st, prev, used, &me) && me >= 0)
        for (int i = 0; i < kMaxPlayers; i++)
            others |= used[i] && i != me && (st[i].flags & kInPlay) && !strcmp(st[i].map, map);
    if (!others) {
        g_shown.clear(), g_led.clear(), g_here.clear();
        if (!loading && map[0]) stand_ins_tick(c, map, now);   // (the stand-ins put away)
        return;
    }
    float mine[3];
    game_actor_pos(mover, mine);
    auto near_other = [&](const float p[3]) {
        float d = 1e9f;
        for (int i = 0; i < kMaxPlayers; i++)
            if (used[i] && i != me && (st[i].flags & kInPlay) && !strcmp(st[i].map, map)) d = std::min(d, dist3(p, st[i].pos));
        return d;
    };
    static int64_t next_round = 0;
    if (now >= next_round) {
        next_round = now + kRoundNs;
        g_here = enemies_here(mover);
        g_here.erase(std::remove_if(g_here.begin(), g_here.end(), [](const Here& h) { return is_stand_in(h.ga); }), g_here.end());
        // (a death the life hook did not see (not through the body's damage step): an enemy alive last round, still
        // here with no life left now: its kill sent as the hook's would be (once: fellow.cpp keeps who it sent))
        {
            static std::vector<std::pair<uint64_t, std::string>> alive;
            for (const auto& [ga, name] : alive) {
                bool still = false;
                for (const Here& h : g_here) still |= h.ga == ga;
                if (still || !heap(ga) || r64(ga) != kGameActorVt || game_actor_name(ga) != name) continue;
                const uint64_t life = game_actor_life(ga);
                if (life && (int32_t)r32(life) <= 0 && fellow_enemy_died_actor(ga))
                    log("enemies: %s died here without the damage step: its kill sent", name.c_str());
            }
            alive.clear();
            for (const Here& h : g_here) alive.push_back({h.ga, h.name});
        }
        // ---- the others' enemies: copied by this game's own (a boss never: each game runs its own)
        for (const EnemySnap& e : ses.enemies()) {
            if (e.map != map || e.owner < 0 || e.owner >= kMaxPlayers || e.owner == me || !used[e.owner]) continue;
            if (e.name.find("Boss") != std::string::npos) continue;
            const bool take_over = dist3(e.pos, mine) < dist3(e.pos, st[e.owner].pos) - kHandOver;
            auto it = std::find_if(g_shown.begin(), g_shown.end(), [&](const Shown& s) { return s.owner == e.owner && s.id == e.id; });
            if (it == g_shown.end()) {
                Shown s;
                s.owner = e.owner, s.id = e.id, s.name = e.name;
                memcpy(s.home, e.home, sizeof s.home);
                // (no stand-ins: every player plays the host's world, so an enemy another game has is in this game too
                // where this player is; one this game does not have (its room not loaded here, or down here) is not made
                // here: a made one would be an extra enemy, unseen, still fighting)
                const Here* h = counterpart(e.name, e.home, e.pos, e.placement);
                if (!h) continue;
                if (!e.lead || take_over) continue;   // (this game runs its own)
                // (both games leading it at once (each the nearer by its own reckoning, a moment's lag apart): the lower
                // slot keeps it, so the two never copy each other and leave it to nobody)
                if (me < e.owner && std::find(g_led.begin(), g_led.end(), h->ga) != g_led.end()) continue;
                s.ga = h->ga;
                log("enemies: %s copied from player %d's game", e.name.c_str(), e.owner);
                g_shown.push_back(s);
                it = g_shown.end() - 1;
            } else if (!it->stand_key && (!e.lead || take_over)) {
                g_shown.erase(it);   // (its game leads it no more: this game runs its own)
                continue;
            }
            if (it->sent_ms != e.got_ms) {
                const float span = it->sent_ms ? (float)(e.got_ms - it->sent_ms) / 1000.0f : 0.0f;
                for (int k = 0; k < 3; k++) it->vel[k] = span > 0.01f && span < 0.5f ? (e.pos[k] - it->pos[k]) / span : 0.0f;
                if (it->vel[0] * it->vel[0] + it->vel[1] * it->vel[1] + it->vel[2] * it->vel[2] > 30.0f * 30.0f)   // (a jump in place)
                    for (float& v : it->vel) v = 0;
                memcpy(it->pos, e.pos, sizeof it->pos);
                memcpy(it->rot, e.rot, sizeof it->rot);
                it->sent_ms = e.got_ms;
                it->got = now;
                const uint64_t ga = shown_actor(*it);
                if (ga && !e.pose.empty()) pose_show_actor(ga, e.pose, now);
            }
        }
        g_shown.erase(std::remove_if(g_shown.begin(), g_shown.end(), [&](const Shown& s) { return now - s.got > kStaleNs; }), g_shown.end());

        // ---- this game's own enemies near another player, the ones it does not copy: sent, the ones it leads first
        struct Pick {
            bool lead;
            float near_other;
            const Here* h;
        };
        std::vector<Pick> picks;
        for (const Here& h : g_here) {
            bool copied = false;
            for (const Shown& s : g_shown) copied |= !s.stand_key && s.ga == h.ga;
            if (copied) continue;
            // (a boss and its parts (Enemy_DungeonBoss_*, Enemy_MiddleBoss_*) run in each game on their own: one put
            // where another game has it every frame is pushed through its walls as it climbs; its hits and its defeat
            // are shared all the same)
            if (h.name.find("Boss") != std::string::npos) continue;
            const float d_other = near_other(h.pos);
            if (d_other > kShareRadius) continue;
            const float d_me = dist3(h.pos, mine);
            const bool led = std::find(g_led.begin(), g_led.end(), h.ga) != g_led.end();
            picks.push_back({d_me < d_other || (led && d_me < d_other + kHandOver), d_other, &h});
        }
        // (only the ones this game leads go: the others' games copy only led enemies. All of them reach the others: the
        // nearest to them every round, the rest in turn, so a room full of enemies keeps every one of them in step)
        std::vector<uint64_t> led;
        for (const Pick& p : picks)
            if (p.lead) led.push_back(p.h->ga);
        picks.erase(std::remove_if(picks.begin(), picks.end(), [](const Pick& p) { return !p.lead; }), picks.end());
        std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.near_other < b.near_other; });
        if ((int)picks.size() > kMaxSent) {
            static size_t turn = 0;
            constexpr int kFirst = kMaxSent / 2;   // (the nearest always; the others in turn)
            std::vector<Pick> sent(picks.begin(), picks.begin() + kFirst);
            const size_t rest = picks.size() - kFirst;
            // (each of the rest at least every 4th round (264 ms): within the 400 ms its copies stay fresh)
            const size_t quota = std::min(rest, std::max((size_t)(kMaxSent - kFirst), (rest + 3) / 4));
            for (size_t k = 0; k < quota; k++) sent.push_back(picks[kFirst + (turn + k) % rest]);
            turn += quota;
            picks.swap(sent);
        }
        for (const Pick& p : picks) {
            EnemySnap e;
            e.map = map;
            e.name = p.h->name;
            e.id = id_of(p.h->ga);
            e.lead = p.lead;
            e.placement = p.h->placement;
            memcpy(e.home, p.h->home, sizeof e.home);
            memcpy(e.pos, p.h->pos, sizeof e.pos);
            for (int i = 0; i < 9; i++) e.rot[i] = rf(p.h->ga + 0x328 + 4 * (uint64_t)i);
            if (!pose_capture_actor(p.h->ga, &e.pose)) e.pose.clear();
            ses.send_enemy(e);
        }
        if (led.size() != g_led.size()) log("enemies: this game leads %zu shared enemies (%zu sent)", led.size(), picks.size());
        g_led.swap(led);
    }
    stand_ins_tick(c, map, now);
    // ---- every frame: the copies and stand-ins at the place their game gives them, carried forward by their speed (up to
    // 120 ms)
    for (Shown& s : g_shown) {
        const uint64_t ga = shown_actor(s);
        if (now - s.got > kStaleNs || !heap(ga) || r64(ga) != kGameActorVt) continue;
        const float age = std::clamp((float)(now - s.got) / 1e9f, 0.0f, 0.12f);
        float target[3];
        for (int k = 0; k < 3; k++) target[k] = s.pos[k] + s.vel[k] * age;
        if (!s.init || dist3(s.shown, target) > 4.0f) memcpy(s.shown, target, sizeof s.shown), s.init = true;
        else
            for (int k = 0; k < 3; k++) s.shown[k] += (target[k] - s.shown[k]) * 0.35f;
        place_actor(c, ga, s.shown, s.rot);
    }
}

namespace {
// (hits that came from another game, given here as the game's own blows: the life hook sees them; not sent back)
struct Muted {
    uint64_t ga;
    int amount;
    int64_t until;
};
std::mutex g_muted_mutex;
std::vector<Muted> g_muted;

bool muted_hit(uint64_t ga, int amount) {
    std::lock_guard<std::mutex> lock(g_muted_mutex);
    const int64_t now = now_ns();
    g_muted.erase(std::remove_if(g_muted.begin(), g_muted.end(), [&](const Muted& m) { return now > m.until; }), g_muted.end());
    for (auto it = g_muted.begin(); it != g_muted.end(); ++it)
        if (it->ga == ga && it->amount == amount) {
            g_muted.erase(it);
            return true;
        }
    return false;
}

void hit_sent(uint64_t ga, int amount) {
    Session& ses = Session::get();
    if (!ses.active() || !ses.others_present() || !ga || amount <= 0) return;
    if (muted_hit(ga, amount)) return;
    {   // (struck by another player's blast made here: its own game sends that hit)
        float p[3];
        if (game_actor_pos(ga, p) && effects_remote_blast_near(p)) return;
    }
    const std::string name = game_actor_name(ga);
    if (name.rfind("Enemy_", 0) != 0) return;
    const LocalSnapshot snap = local_snapshot();
    if (!snap.valid || !snap.map[0]) return;
    float home[3];
    for (int k = 0; k < 3; k++) home[k] = rf(ga + 0x2ec + 4 * (uint64_t)k);
    int owner = -1;   // (this game's own)
    uint32_t id = id_of(ga);
    for (const Shown& s : g_shown)
        if (shown_actor(s) == ga) {
            owner = s.owner, id = s.id;
            memcpy(home, s.home, sizeof home);
            break;
        }
    ses.send_enemy_hit(snap.map, name, home, amount, owner, id);
    log("enemies: %s hit here (-%d): sent as %s enemy %#x", name.c_str(), amount, owner < 0 ? "this game's" : owner == 0 ? "player 0's" : owner == 1 ? "player 1's" : owner == 2 ? "player 2's" : "player 3's", id);
}
}  // namespace

void enemies_hit(uint64_t body, uint32_t before, uint32_t after) {
    if (after >= before || after == 0) return;   // (a kill goes as a kill: fellow.cpp)
    hit_sent(game_actor_of(body), (int)(before - after));
}

void enemies_hit_test(uint64_t ga, int amount) {
    const uint64_t life = game_actor_life(ga);
    if (!life || (int)r32(life) <= amount) return;
    InterlockedExchange((volatile LONG*)gp(life), (LONG)((int)r32(life) - amount));
    hit_sent(ga, amount);
}

void enemies_take_hit(Ctx* c, const std::string& map, const std::string& name, const float home[3], int amount, int owner, uint32_t id) {
    const LocalSnapshot snap = local_snapshot();
    if (!snap.valid || map != snap.map || amount <= 0) return;
    PlayerState st[kMaxPlayers], prev[kMaxPlayers];
    bool used[kMaxPlayers];
    int me = -1;
    Session::get().states(st, prev, used, &me);
    uint64_t ga = 0;
    const char* how = "by its name and home";
    const std::vector<Here> here = enemies_here(snap.mover);
    if (owner == me) {   // (one of this game's own)
        for (const Here& h : here)
            if (id_of(h.ga) == id && h.name == name) ga = h.ga, how = "this game's own";
    } else {
        for (const Shown& s : g_shown)
            if (s.owner == owner && s.id == id) ga = shown_actor(s), how = s.stand_key ? "its stand-in" : "its copy";
    }
    if (!ga) {
        const Here* best = nullptr;
        float best_d = kHomeMatch;
        for (const Here& h : here) {
            if (h.name != name || is_stand_in(h.ga)) continue;
            const float d = dist3(h.home, home);
            if (d < best_d) best_d = d, best = &h;
        }
        if (best) ga = best->ga, how = "by its name and home";
    }
    const uint64_t life = ga ? game_actor_life(ga) : 0;
    if (!life) return;
    const int was = (int)r32(life);
    if (was <= 0) return;
    const int take = std::min(amount, was - 1);   // (its death comes as a kill)
    if (take <= 0) return;
    // (as a blow of the game's own: the enemy reacts here as it does there (its flinch, its knock-back), seen by the
    // player who struck it when this game leads it; the life hook sees it: not sent back)
    {
        std::lock_guard<std::mutex> lock(g_muted_mutex);
        g_muted.push_back({ga, take, now_ns() + 3000000000LL});
    }
    const bool blow = fellow_life_message(c, ga, -take);
    if (!blow) InterlockedExchange((volatile LONG*)gp(life), was - take);
    log("enemies: %s (player %d's enemy) hit in another game: life %d -> %d (%s%s)", name.c_str(), owner, was, was - take, how,
        blow ? ", as a blow" : "");
}

}  // namespace coop
