// The shared world (world.h).
//
//   the state:  GameData's object flags (its Bool64bitKey table: the manager *(*0x84c9e180), the count at +0xc58, the
//               entries at +0xc60, 16 bytes each, sorted by key: the key (u64), the kind (its ResetType: u32 at +8) and
//               the value (u32 at +12)). A placed object's flags are keyed by its placement hash (its GameActor's
//               *(+0x3d8) + 0x88) and by that hash ^ 2. Shared: the kinds 24 and 280 (the objects' lasting state);
//               not the counters (88) nor what comes back by itself (25: grass, pots, collectibles); and only for the
//               world's mechanisms (doors, switches, webs, blocks, platforms...): the objects that give something (chests,
//               hearts, rupees, items) stay each player's own, so each player opens their own chest and gets its item
//               (as each gets the loot of a shared kill), and enemies go by the shared kills (fellow.cpp). What an object
//               is: its actor's name, known while it is in this world (kept for the map: a web is gone once broken).
//   sent:       five times a second the table is read and what changed in this game (not written by the mod) goes to
//               the others with the map it changed in
//   put here:   at once, unless the object is in this world now (its placement hash among the actors here): the game
//               reads an object's flags when it makes it, so the change waits until the object is gone (its room or its
//               map left) and is there when the player comes back: the door open, the web broken. The object a player
//               is looking at never changes under their eyes, and they can still do the same themselves.
#include "world.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "coop.h"
#include "effects.h"
#include "fellow.h"
#include "game.h"
#include "runtime.h"
#include "session.h"

namespace coop {

namespace {
constexpr uint64_t kGameDataSlot = 0x84c9e180ULL, kGameActorVt = 0x84b45e18ULL;
constexpr int64_t kPollNs = 200000000LL;
constexpr int kMaxSentAtOnce = 64;   // (more at once: a save was loaded, not something done: not sent)

bool heap(uint64_t a) { return a >= kHeapBase && a < kHeapBase + 0x100000000ULL; }
bool shared_kind(uint32_t kind) { return kind == 24 || kind == 280; }

struct Pending {
    uint64_t key = 0;
    uint32_t value = 0;
    bool told = false;
};

std::vector<uint8_t> g_table;    // (the entries as last read)
std::vector<uint32_t> g_seen;    // (each entry's value as last seen: what changed since is this game's doing)
uint64_t g_at = 0;
std::vector<Pending> g_pending;
std::unordered_map<uint64_t, std::string> g_names;   // (placement hash -> actor name, for the objects seen in this map)
std::string g_names_map;

// (an object whose state is the world's: not one that gives something, comes back, or is an enemy)
bool shared_object(const std::string& name) {
    if (name.rfind("Obj_", 0) != 0) return false;
    static const char* const kOwn[] = {"TBox", "Heart", "Rupee", "Item", "Plant", "Grass", "Pot", "Respawn", "Skull",
                                       "Collect", "Fairy", "Bug", "Fish", "Supply"};
    for (const char* w : kOwn)
        if (name.find(w) != std::string::npos) return false;
    return true;
}

uint64_t key_at(size_t i) {
    uint64_t k;
    memcpy(&k, g_table.data() + 16 * i, 8);
    return k;
}
uint32_t u32_at(size_t i, size_t off) {
    uint32_t v;
    memcpy(&v, g_table.data() + 16 * i + off, 4);
    return v;
}

long find_key(uint64_t key) {
    size_t lo = 0, hi = g_table.size() / 16;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const uint64_t k = key_at(mid);
        if (k == key) return (long)mid;
        if (k < key) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

std::unordered_map<uint64_t, uint64_t> g_actor_of;   // (placement hash -> its actor here now)

// (the placement hashes of the objects in this world now; their names kept for the map)
std::unordered_set<uint64_t> objects_here(uint64_t mover) {
    std::unordered_set<uint64_t> out;
    g_actor_of.clear();
    uint64_t ga = mover;
    for (int steps = 0; steps < 8192; steps++) {
        const uint64_t next = r64(ga + 0x190);
        if (!heap(next)) break;
        ga = next - 0x190;
        if (ga == mover || r64(ga) != kGameActorVt) break;
        const uint64_t rec = r64(ga + 0x3d8);
        if (!heap(rec)) continue;
        const uint64_t hash = r64(rec + 0x88);
        out.insert(hash);
        g_actor_of[hash] = ga;
        if (!g_names.count(hash) && g_names.size() < 20000) g_names[hash] = cstr_at(r64(ga + 0x260), 64);
    }
    return out;
}

// An object whose changed state means it is gone (a web broken, a rock or a cracked wall blown up, a lock opened, a floor
// fallen): taken away at once when another player's game says so, while this player looks at it. Others (a ladder that
// falls into place, a door, a platform, a switch) keep their state for when they are made again.
bool goes_away(const std::string& name) {
    static const char* const kGone[] = {"SpiderWeb_", "Breakable", "BrokenBomb", "Crack", "Rock", "Boulder", "Barricade",
                                        "Lock", "FallingFloor", "FallFloor", "Bombable"};
    if (name.find("Ladder") != std::string::npos || name.find("Prop") != std::string::npos) return false;
    for (const char* w : kGone)
        if (name.find(w) != std::string::npos) return true;
    return false;
}

// (an object taken away here as its own break does: a blow when it has a life (its break, its effect), else the scripts'
// OneShotGenericDeleteThisActor (f_800c6490(actor, 0)); its parts (the actors of its kind at its place) with it)
void take_away(Ctx* c, uint64_t mover, uint64_t ga, const std::string& name) {
    float at[3];
    if (!game_actor_pos(ga, at)) return;
    std::vector<uint64_t> all = {ga};
    uint64_t a = mover;
    for (int steps = 0; steps < 8192; steps++) {
        const uint64_t next = r64(a + 0x190);
        if (!heap(next)) break;
        a = next - 0x190;
        if (a == mover || r64(a) != kGameActorVt) break;
        float p[3];
        if (a == ga || !game_actor_pos(a, p)) continue;
        const float dx = p[0] - at[0], dy = p[1] - at[1], dz = p[2] - at[2];
        if (dx * dx + dy * dy + dz * dz > 1.5f * 1.5f) continue;
        const std::string other = game_actor_name(a);
        if (other.find("Parts") != std::string::npos && other.rfind("Obj_", 0) == 0) all.push_back(a);
    }
    for (uint64_t x : all) {
        const uint64_t comps = r64(x + 0x270);
        const uint64_t body = heap(comps) && (int)r32(x + 0x278) > 87 ? r64(comps + 8 * 87) : 0;
        const uint64_t life = heap(body) ? r64(body + 0x1c10) : 0;
        if (heap(life) && (int)r32(life + 8) > 0 && fellow_life_message(c, x, -100000)) continue;
        guest_call(c, 0x800c6490ULL, x, 0);
    }
    log("world: %s taken away here at once, as in another player's game (%zu actor%s)", name.c_str(), all.size(), all.size() == 1 ? "" : "s");
}

// (another player in this map now)
bool v_in_map(Session& ses, const char* map) {
    PlayerState st[kMaxPlayers], prev[kMaxPlayers];
    bool used[kMaxPlayers];
    int me = -1;
    if (!ses.states(st, prev, used, &me)) return false;
    for (int i = 0; i < kMaxPlayers; i++)
        if (used[i] && i != me && (st[i].flags & kInPlay) && !strcmp(st[i].map, map)) return true;
    return false;
}

// (what object a flag is of: its name, empty when not seen in this map)
std::string object_of(uint64_t key) {
    auto it = g_names.find(key);
    if (it == g_names.end()) it = g_names.find(key ^ 2);
    return it == g_names.end() ? std::string() : it->second;
}
}  // namespace

// ---- blows on placed objects (grass, pots, webs, eye switches): live, by placement
namespace {
struct Muted {
    uint64_t ga;
    int amount;
    int64_t until;
};
std::mutex g_muted_mutex;
std::vector<Muted> g_muted;   // (blows from another game given here: the life hook sees them; not sent back)

uint64_t placement_of(uint64_t ga) {
    const uint64_t rec = r64(ga + 0x3d8);
    return heap(rec) ? r64(rec + 0x88) : 0;
}
}  // namespace

void world_object_hit(uint64_t body, uint32_t before, uint32_t after) {
    Session& ses = Session::get();
    if (after >= before || !ses.active() || !ses.others_present()) return;
    const uint64_t ga = game_actor_of(body);
    if (!ga) return;
    const std::string name = game_actor_name(ga);
    if (name.rfind("Obj_", 0) != 0) return;   // (placed objects: not enemies (shared as kills and hits), not the players)
    const int amount = (int)(before - after);
    {
        std::lock_guard<std::mutex> lock(g_muted_mutex);
        const int64_t now = now_ns();
        g_muted.erase(std::remove_if(g_muted.begin(), g_muted.end(), [&](const Muted& m) { return now > m.until; }), g_muted.end());
        for (auto it = g_muted.begin(); it != g_muted.end(); ++it)
            if (it->ga == ga && it->amount == amount) {
                g_muted.erase(it);
                return;
            }
    }
    const uint64_t placement = placement_of(ga);
    const LocalSnapshot snap = local_snapshot();
    if (!placement || !snap.valid || !snap.map[0]) return;
    {   // (broken by another player's blast made here: its own game sends that)
        float p[3];
        if (game_actor_pos(ga, p) && effects_remote_blast_near(p)) return;
    }
    ses.send_object_hit(snap.map, name, placement, amount);
    log("world: %s struck here (-%d): sent (%016llx)", name.c_str(), amount, (unsigned long long)placement);
}

void world_take_object_hit(Ctx* c, const std::string& map, const std::string& name, uint64_t placement, int amount) {
    const LocalSnapshot snap = local_snapshot();
    if (!snap.valid || map != snap.map || amount <= 0 || !snap.mover) return;
    uint64_t ga = snap.mover;
    bool found = false;
    for (int steps = 0; steps < 8192 && !found; steps++) {
        const uint64_t next = r64(ga + 0x190);
        if (!heap(next)) return;
        ga = next - 0x190;
        if (ga == snap.mover || r64(ga) != kGameActorVt) return;
        found = placement_of(ga) == placement && game_actor_name(ga) == name;
    }
    if (!found) {
        log("world: %s struck in another game: not in this world now", name.c_str());
        return;
    }
    // (its life: the body's (component 87) *(+0x1c10) + 8)
    const uint64_t comps = r64(ga + 0x270);
    const uint64_t body = heap(comps) && (int)r32(ga + 0x278) > 87 ? r64(comps + 8 * 87) : 0;
    const uint64_t life = heap(body) ? r64(body + 0x1c10) : 0;
    const int left = heap(life) ? (int)r32(life + 8) : 0;
    if (left <= 0) return;   // (already cut or broken here)
    {
        std::lock_guard<std::mutex> lock(g_muted_mutex);
        g_muted.push_back({ga, std::min(amount, left), now_ns() + 3000000000LL});
    }
    const bool blow = fellow_life_message(c, ga, -amount);
    log("world: %s struck in another game: struck here too%s", name.c_str(), blow ? "" : " (not taken)");
}

void world_update(Ctx* c, uint64_t mover, const char* map, bool loading, int64_t now) {
    Session& ses = Session::get();
    if (!ses.active() || loading || !map[0]) return;
    static int64_t next = 0;
    if (now < next) return;
    next = now + kPollNs;
    if (g_names_map != map) g_names.clear(), g_names_map = map;
    const std::unordered_set<uint64_t> here = objects_here(mover);
    const uint64_t h = r64(kGameDataSlot);
    const uint64_t mgr = h ? r64(h) : 0;
    if (!heap(mgr)) return;
    const uint32_t n = r32(mgr + 0xc58);
    const uint64_t arr = r64(mgr + 0xc60);
    if (!heap(arr) || n == 0 || n > 65536) return;
    g_table.resize((size_t)n * 16);
    if (!peek(arr, g_table.data(), g_table.size())) return;
    if (arr != g_at || g_seen.size() != n) {   // (the table first seen, or made again: what it holds now is the start)
        g_at = arr;
        g_seen.resize(n);
        for (size_t i = 0; i < n; i++) g_seen[i] = u32_at(i, 12);
        log("world: %u object flags known", n);
        return;
    }
    // ---- what changed in this game
    std::vector<WorldFlag> out;
    size_t changed = 0;
    std::string what;
    for (size_t i = 0; i < n; i++) {
        const uint32_t v = u32_at(i, 12);
        if (v == g_seen[i]) continue;
        g_seen[i] = v;
        changed++;
        if (!shared_kind(u32_at(i, 8))) continue;
        const std::string name = object_of(key_at(i));
        if (!shared_object(name)) {
            if (!name.empty()) log("world: %s changed here: its own to each player (not sent)", name.c_str());
            // (a chest opened here: the others in this map told that theirs still holds its item)
            if (name.find("TBox") != std::string::npos && ses.others_present() && v_in_map(ses, map))
                ses.tell_others(ses.name() + " opened a chest here: yours still holds its item for you", 0);
            continue;
        }
        out.push_back({key_at(i), v});
        what = name;
    }
    if (changed > (size_t)kMaxSentAtOnce) {
        log("world: %zu object flags changed at once (a save loaded): not sent", changed);
        out.clear();
    }
    if (!out.empty() && ses.others_present()) {
        ses.send_world_flags(map, out);
        log("world: %zu object change%s sent (%s: %016llx = %08x%s)", out.size(), out.size() == 1 ? "" : "s", what.c_str(),
            (unsigned long long)out[0].key, out[0].value, out.size() > 1 ? ", ..." : "");
    }
    // ---- what changed in the others' games: put here, but not under an object that is here now
    for (const WorldFlag& f : ses.take_world_flags())
        if (g_pending.size() < 8192) g_pending.push_back({f.key, f.value, false});
    if (g_pending.empty()) return;
    size_t put = 0;
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        const long i = find_key(it->key);
        if (i < 0 || !shared_kind(u32_at((size_t)i, 8)) || u32_at((size_t)i, 12) == it->value) {
            it = g_pending.erase(it);   // (unknown here, not shared, or already so)
            continue;
        }
        if (here.count(it->key) || here.count(it->key ^ 2)) {
            // (in this world now: an object that goes away is taken away at once, its state written; the others wait)
            auto obj = g_actor_of.find(it->key);
            if (obj == g_actor_of.end()) obj = g_actor_of.find(it->key ^ 2);
            const std::string name = obj != g_actor_of.end() ? game_actor_name(obj->second) : std::string();
            if (obj != g_actor_of.end() && goes_away(name) && it->value != 0) {
                take_away(c, mover, obj->second, name);
                const uint32_t v = it->value;
                poke(arr + 16 * (uint64_t)i + 12, &v, 4);
                g_seen[(size_t)i] = v;
                it = g_pending.erase(it);
                continue;
            }
            if (!it->told) log("world: %016llx waits: its object is in this world now", (unsigned long long)it->key);
            it->told = true;
            ++it;
            continue;
        }
        const uint32_t v = it->value;
        poke(arr + 16 * (uint64_t)i + 12, &v, 4);
        g_seen[(size_t)i] = v;
        put++;
        log("world: %016llx = %08x put in this world", (unsigned long long)it->key, v);
        it = g_pending.erase(it);
    }
    (void)put;
}

}  // namespace coop
