// The other players in the world, as the game's own Link: for each other player in this map, the game is asked for a
// PouchPlayer_Child (or _Adult) actor (the Link of the pause menu: the player's own body, hat and tunic, standing as he
// stands there), placed in the world and moved every frame to where that player is.
//
//   made:   ActorMgr::requestCreateActor (spawn_actor, as the scripts' create node f_832dca10 asks it); the request's
//           record (*link) reaches state 3 when the actor is made, then f_8038fbd0(record, 1, 0) puts it in the world
//           and returns it (SeqExecuteCreateActor's update, f_832dcb30, does the same)
//   moved:  f_804e6630(its physics component *(*(actor + 0x270) + 0x50), &position, &rotation 3 x 3, 1), as the warp
//           nodes do (OneShotWarpToHomeMatrix, f_82978560)
//   kept:   its GameActor's vtable and name checked each frame: a map change takes the game's actors away, and it is
//           made again in the new map
//   gear:   what that player's Link wears (session: the game's actor names), put on as the game's own equip does
//           (f_82e16c80's last step): f_81033f20(its EquipmentUser *(components + 0x200), slot, {name, 0, 0}, 0, 1); the slots
//           are the equip's: 0 weapon, 6 shield, 7 head, 8 tunic, 9 boots, 10 hands

//
// Motion without delay: the newest state carried forward by the speed it shows (up to 150 ms), eased toward. Its body
// moves as its player's Link does (pose.cpp: the pose that player's game sends, the fall to the ground included).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include "coop.h"
#include "fellow.h"
#include "game.h"
#include "runtime.h"
#include "session.h"

namespace coop {

namespace {
constexpr uint64_t kGameActorVt = 0x84b45e18ULL;
constexpr uint64_t kFnActivate = 0x8038fbd0ULL, kFnSetTransform = 0x804e6630ULL, kFnEquip = 0x81033f20ULL;
constexpr uint64_t kEmptyString = 0x83f04f8cULL;
constexpr float kMaxSpeed = 30.0f;   // (metres a second: faster is a jump in place, not motion)

constexpr int kGearSlots[6] = {0, 6, 7, 8, 9, 10};   // (weapon, shield, head, tunic, boots, hands: session's gear order)

struct Puppet {
    int state = 0;              // 0 none, 1 asked, 2 in the world
    bool child = true;
    bool hidden = false;
    uint64_t link = 0, actor = 0, physics = 0;
    int64_t asked_at = 0, retry_at = 0;
    // (the motion shown)
    bool init = false;
    uint32_t seq = 0;
    float pos[3] = {0, 0, 0}, vel[3] = {0, 0, 0};
    float yaw = 0, down = 0;
    int64_t at = 0;
    // (what it wears)
    std::string gear_shown;
    int64_t gear_check = 0;
};
Puppet g_pup[kMaxPlayers];
std::atomic<bool> g_placed[kMaxPlayers];
std::atomic<uint64_t> g_actor[kMaxPlayers];
std::atomic<bool> g_child[kMaxPlayers];
std::atomic<bool> g_hidden_by_wall[kMaxPlayers];
uint64_t g_buf = 0;   // (guest memory for the transform: 0x40 a slot)

bool heap(uint64_t a) { return a >= kHeapBase && a < kHeapBase + 0x100000000ULL; }

float angle_to(float from, float to) {
    float d = fmodf(to - from + 3.14159265f, 6.2831853f);
    if (d < 0) d += 6.2831853f;
    return d - 3.14159265f;
}

std::vector<std::string> gear_parts(const std::string& g) {
    std::vector<std::string> out(6);
    size_t at = 0;
    for (int k = 0; k < 6; k++) {
        const size_t bar = g.find('|', at);
        out[k] = g.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
        if (bar == std::string::npos) break;
        at = bar + 1;
    }
    return out;
}

// the parts of its gear that changed, put on (an empty one: taken off)
void wear(Ctx* c, uint64_t actor, const std::string& was, const std::string& now) {
    static uint64_t cell = 0;
    if (!cell) cell = guest_alloc(0x20, 16);
    const uint64_t comps = r64(actor + 0x270);
    const uint64_t equipment = heap(comps) ? r64(comps + 0x200) : 0;
    if (!cell || !heap(equipment) || !rt().a64_lookup(kFnEquip)) return;
    const std::vector<std::string> a = gear_parts(was), b = gear_parts(now);
    for (int k = 0; k < 6; k++) {
        if (a[k] == b[k]) continue;
        const uint64_t cellv[2] = {b[k].empty() ? kEmptyString : guest_string(b[k]), 0};
        poke(cell, cellv, sizeof cellv);
        guest_call(c, kFnEquip, equipment, (uint64_t)kGearSlots[k], cell, 0, 1);
    }
}

bool alive(const Puppet& p) {
    if (!heap(p.actor) || r64(p.actor) != kGameActorVt) return false;
    const uint64_t s = r64(p.actor + 0x260);
    return s >= 0x80000000ULL && cstr_at(s, 24).rfind("PouchPlayer_", 0) == 0;
}

void motion(Puppet& p, const PlayerState& st, const PlayerState& prev, int smoothing, int64_t now) {
    const float dt = p.at ? std::clamp((float)(now - p.at) / 1e9f, 0.0f, 0.1f) : 0.0f;
    p.at = now;
    if (p.seq != st.seq) {
        p.seq = st.seq;
        const float span = (float)(st.got_ms - prev.got_ms) / 1000.0f;
        if (prev.got_ms && span > 0.004f && span < 0.5f)
            for (int k = 0; k < 3; k++) p.vel[k] = (st.pos[k] - prev.pos[k]) / span;
        else
            for (float& v : p.vel) v = 0;
        // (faster than Link can move: a warp, a respawn, a map's entrance: nothing carried forward, or the puppet would be
        // thrown past the place by many metres)
        if (p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1] + p.vel[2] * p.vel[2] > kMaxSpeed * kMaxSpeed)
            for (float& v : p.vel) v = 0;
    }
    const float age = std::clamp((float)(net::now_ms() - st.got_ms) / 1000.0f, 0.0f, 0.15f);
    float target[3];
    for (int k = 0; k < 3; k++) target[k] = st.pos[k] + p.vel[k] * age;
    const float dx = target[0] - p.pos[0], dy = target[1] - p.pos[1], dz = target[2] - p.pos[2];
    static const float rate[4] = {1000.0f, 30.0f, 18.0f, 10.0f};
    const float k = !p.init || dx * dx + dy * dy + dz * dz > 36.0f ? 1.0f : 1.0f - expf(-rate[std::clamp(smoothing, 0, 3)] * dt);
    for (int j = 0; j < 3; j++) p.pos[j] += (target[j] - p.pos[j]) * k;
    p.yaw = !p.init ? st.yaw : p.yaw + angle_to(p.yaw, st.yaw) * (1.0f - expf(-16.0f * dt));
    p.down += (((st.flags & kDowned) ? 1.0f : 0.0f) - p.down) * (1.0f - expf(-7.0f * dt));
    p.init = true;
}

void place(Ctx* c, int slot, Puppet& p, const float pos[3], float yaw) {
    if (!g_buf) g_buf = guest_alloc(0x40 * kMaxPlayers, 16);
    if (!g_buf || !heap(p.actor)) return;
    const uint64_t b = g_buf + 0x40 * (uint64_t)slot;
    const float cy = cosf(yaw), sy = sinf(yaw);
    const float r[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};   // (Yaw)
    const float px = pos[0], y = pos[1], pz = pos[2];
    // (an actor with a rigid body: moved through it; the pause menu's Link has none: its transform asked for, as the warp
    // node does then: +0x358 the place, +0x364 the turn, and bit 0x8000 of +0x4a8 cleared)
    if (heap(p.physics) && heap(r64(p.physics + 0x20))) {
        wf(b, px), wf(b + 4, y), wf(b + 8, pz);
        for (int i = 0; i < 9; i++) wf(b + 16 + 4 * (uint64_t)i, r[i]);
        if (guest_call(c, kFnSetTransform, p.physics, b, b + 16, 1) & 1) return;
    }
    wf(p.actor + 0x358, px), wf(p.actor + 0x35c, y), wf(p.actor + 0x360, pz);
    for (int i = 0; i < 9; i++) wf(p.actor + 0x364 + 4 * (uint64_t)i, r[i]);
    InterlockedAnd((volatile LONG*)gp(p.actor + 0x4a8), ~(LONG)0x8000);
}

// ---- the items of the hands the other Links have out (the slingshot, a Deku Stick...): the pause menu's Link cannot be
// given them (the game fails on them: they look for a real player), so a stand-alone actor of each is asked of the game
// (as the puppet is) and pose.cpp puts it in that Link's hand at each lens. Kept near that Link while wanted (the game
// draws what is near), put under the map when not, and kept for the next time (four a player at most)
constexpr int kMaxHeld = 4;
struct Held {
    std::string name;
    uint32_t hash = 0;
    uint64_t link = 0, actor = 0;
    int state = 1;   // 1 asked, 2 in the world
    bool wanted = false;
    int64_t asked_at = 0;
};
std::vector<Held> g_held[kMaxPlayers];   // (Link's update only)
std::string g_held_map;
std::mutex g_want_mutex;                 // (the wanted names: from the lens)
std::vector<std::string> g_want[kMaxPlayers];
int64_t g_want_at[kMaxPlayers];
std::atomic<uint64_t> g_held_actor[kMaxPlayers][kMaxHeld];   // (for the lens: the ones in the world and wanted)
std::atomic<uint32_t> g_held_hash[kMaxPlayers][kMaxHeld];
uint64_t g_held_buf = 0;

uint32_t name_fnv(const std::string& s) {
    uint32_t h = 2166136261u;
    for (char ch : s) h = (h ^ (uint8_t)ch) * 16777619u;
    return h;
}

void place_free(Ctx* c, uint64_t actor, const float pos[3]) {
    if (!g_held_buf) g_held_buf = guest_alloc(0x40, 16);
    if (!g_held_buf || !heap(actor)) return;
    const float r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const uint64_t comps = r64(actor + 0x270);
    const uint64_t phys = heap(comps) ? r64(comps + 0x50) : 0;
    if (heap(phys) && heap(r64(phys + 0x20))) {
        wf(g_held_buf, pos[0]), wf(g_held_buf + 4, pos[1]), wf(g_held_buf + 8, pos[2]);
        for (int i = 0; i < 9; i++) wf(g_held_buf + 16 + 4 * (uint64_t)i, r[i]);
        if (guest_call(c, kFnSetTransform, phys, g_held_buf, g_held_buf + 16, 1) & 1) return;
    }
    wf(actor + 0x358, pos[0]), wf(actor + 0x35c, pos[1]), wf(actor + 0x360, pos[2]);
    for (int i = 0; i < 9; i++) wf(actor + 0x364 + 4 * (uint64_t)i, r[i]);
    InterlockedAnd((volatile LONG*)gp(actor + 0x4a8), ~(LONG)0x8000);
}

void held_tick(Ctx* c, const char* map, bool loading, int64_t now) {
    if (g_held_map != map || loading) {   // (the map's actors went with it)
        for (auto& v : g_held) v.clear();
        g_held_map = map;
    }
    for (int slot = 0; slot < kMaxPlayers; slot++) {
        std::vector<std::string> want;
        {
            std::lock_guard<std::mutex> lock(g_want_mutex);
            if (now - g_want_at[slot] < 500000000LL) want = g_want[slot];
        }
        const Puppet& pup = g_pup[slot];
        const bool here = pup.state == 2 && !pup.hidden && !loading;
        auto& held = g_held[slot];
        for (Held& h : held) h.wanted = false;
        // (what the puppet wears comes with its gear (the sword, the shield, the sheath with the sword): no stand-alone ones)
        const std::string gear = here ? Session::get().gear(slot) : std::string();
        auto worn = [&](const std::string& n) { return n.rfind("Weapon_Sheath_", 0) == 0 || gear.find(n) != std::string::npos; };
        if (here)
            for (const std::string& name : want) {
                if (worn(name)) continue;
                const uint32_t hash = name_fnv(name);
                auto it = std::find_if(held.begin(), held.end(), [&](const Held& h) { return h.hash == hash; });
                if (it != held.end()) {
                    it->wanted = true;
                    continue;
                }
                if ((int)held.size() >= kMaxHeld) continue;
                Held h;
                h.name = name, h.hash = hash, h.wanted = true, h.asked_at = now;
                const float at[3] = {pup.pos[0], pup.pos[1] + 0.9f, pup.pos[2]};
                h.link = spawn_actor(c, name.c_str(), at, pup.yaw);
                if (!h.link) continue;
                held.push_back(h);
                log("puppet: player %d has %s out: a stand-alone one asked for", slot, name.c_str());
            }
        for (size_t i = 0; i < held.size();) {
            Held& h = held[i];
            bool drop = false;
            if (h.state == 1) {
                const uint64_t rec = r64(h.link);
                const int rs = heap(rec) ? (int)(r32(rec) & 0xffff) : 0;
                if (rs == 3) {
                    h.actor = guest_call(c, kFnActivate, rec, 1, 0);
                    h.state = 2;
                    drop = !heap(h.actor) || r64(h.actor) != kGameActorVt;
                    log("puppet: player %d's %s in the world (%#llx)", slot, h.name.c_str(), (unsigned long long)h.actor);
                } else if ((rs >= 4 && rs <= 6) || now - h.asked_at > 15000000000LL) {
                    drop = true;
                }
            } else if (!heap(h.actor) || r64(h.actor) != kGameActorVt || name_fnv(cstr_at(r64(h.actor + 0x260), 48)) != h.hash) {
                drop = true;
            } else if (h.wanted && here) {
                const float at[3] = {pup.pos[0], pup.pos[1] + 0.9f, pup.pos[2]};
                place_free(c, h.actor, at);
            } else {
                const float away[3] = {pup.pos[0], pup.pos[1] - 500.0f, pup.pos[2]};
                place_free(c, h.actor, away);
            }
            if (drop) held.erase(held.begin() + (ptrdiff_t)i);
            else i++;
        }
        for (int k = 0; k < kMaxHeld; k++) {
            const bool on = k < (int)held.size() && held[k].state == 2 && held[k].wanted && here;
            g_held_hash[slot][k] = on ? held[k].hash : 0;
            g_held_actor[slot][k] = on ? held[k].actor : 0;
        }
    }
}
}  // namespace

void puppet_want_held(int slot, const std::vector<std::string>& names) {
    if (slot < 0 || slot >= kMaxPlayers) return;
    std::lock_guard<std::mutex> lock(g_want_mutex);
    g_want[slot] = names;
    g_want_at[slot] = now_ns();
}

uint64_t puppet_held(int slot, uint32_t hash) {
    if (slot < 0 || slot >= kMaxPlayers || !hash) return 0;
    for (int k = 0; k < kMaxHeld; k++)
        if (g_held_hash[slot][k].load() == hash) return g_held_actor[slot][k].load();
    return 0;
}

bool player_behind_wall(int slot) { return slot >= 0 && slot < kMaxPlayers && g_hidden_by_wall[slot].load(); }

bool puppet_placed(int slot) { return slot >= 0 && slot < kMaxPlayers && g_placed[slot].load(); }

uint64_t puppet_actor(int slot) { return puppet_placed(slot) ? g_actor[slot].load() : 0; }

// where that player's Link is drawn now (his GameActor's place: the nameplate stays over his head at any distance)
bool puppet_position(int slot, float out[3], bool* child) {
    if (!puppet_placed(slot)) return false;
    const uint64_t a = g_actor[slot].load();
    if (!heap(a) || r64(a) != kGameActorVt) return false;
    for (int i = 0; i < 3; i++) out[i] = rf(a + 0x31c + 4 * (uint64_t)i);
    if (child) *child = g_child[slot].load();
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

void puppets_update(Ctx* c, const char* map, bool loading, int64_t now) {
    Session& ses = Session::get();
    PlayerState st[kMaxPlayers], prev[kMaxPlayers];
    bool used[kMaxPlayers];
    int me = -1;
    if (!ses.states(st, prev, used, &me)) {
        for (int i = 0; i < kMaxPlayers; i++) {
            if (g_pup[i].state == 2 && alive(g_pup[i]) && !g_pup[i].hidden) {
                const float away[3] = {g_pup[i].pos[0], g_pup[i].pos[1] - 500.0f, g_pup[i].pos[2]};
                place(c, i, g_pup[i], away, 0);
                g_pup[i].hidden = true;
            }
            g_placed[i] = false;
        }
        return;
    }
    const int smoothing = ses.guest_settings().smoothing;
    static int64_t next_walls = 0;
    const bool walls = now >= next_walls;
    float eye[3] = {0, 0, 0};
    if (walls) {
        next_walls = now + 100000000LL;
        const CameraView cam = camera_view();
        const float* m = cam.m;
        for (int k = 0; k < 3; k++) eye[k] = -(m[k] * m[3] + m[4 + k] * m[7] + m[8 + k] * m[11]);   // (-R^T t)
    }
    for (int i = 0; i < kMaxPlayers; i++) {
        Puppet& p = g_pup[i];
        const bool want = used[i] && i != me && (st[i].flags & kInPlay) && map[0] && !strcmp(st[i].map, map) && !loading;
        if (p.state == 2 && !alive(p)) p = Puppet{};   // (taken away with the map: made again)
        if (p.state == 2 && want && (bool)st[i].child != p.child) {
            // (the other age: this one put away, the other made)
            const float away[3] = {p.pos[0], p.pos[1] - 500.0f, p.pos[2]};
            place(c, i, p, away, 0);
            p = Puppet{};
        }
        if (want) motion(p, st[i], prev[i], smoothing, now);
        else p.init = false;
        if (walls && want) {
            const float h = st[i].child ? 1.2f : 1.7f;
            bool seen = false;
            for (float part : {h * (1.0f - 0.7f * p.down), h * 0.55f * (1.0f - 0.7f * p.down)}) {
                const float to[3] = {p.pos[0], p.pos[1] + part, p.pos[2]};
                const float dx = to[0] - eye[0], dy = to[1] - eye[1], dz = to[2] - eye[2];
                const float len = sqrtf(dx * dx + dy * dy + dz * dz);
                float dist = 0;
                const int hit = len > 0.5f ? game_raycast(c, eye, to, 0, 0, 3, 0, &dist) : 0;
                if (hit <= 0 || dist >= len - 0.35f) seen = true;
            }
            g_hidden_by_wall[i] = !seen;
        } else if (!want) {
            g_hidden_by_wall[i] = false;
        }
        switch (p.state) {
        case 0:
            if (want && now >= p.retry_at) {
                p.child = st[i].child;
                p.link = spawn_actor(c, p.child ? "PouchPlayer_Child" : "PouchPlayer_Adult", p.pos, p.yaw);
                p.state = p.link ? 1 : 0;
                p.asked_at = now;
                p.retry_at = now + 5000000000LL;
            }
            break;
        case 1: {
            const uint64_t rec = r64(p.link);
            const int rs = heap(rec) ? (int)(r32(rec) & 0xffff) : 0;
            if (rs == 3) {
                const uint64_t actor = guest_call(c, kFnActivate, rec, 1, 0);
                const uint64_t comps = heap(actor) ? r64(actor + 0x270) : 0;
                p.actor = actor;
                p.physics = heap(comps) ? r64(comps + 0x50) : 0;
                p.state = heap(actor) && r64(actor) == kGameActorVt ? 2 : 0;
                p.hidden = false;
                log("puppet: player %d in the world as %s (%#llx)", i, p.child ? "PouchPlayer_Child" : "PouchPlayer_Adult",
                    (unsigned long long)actor);
            } else if ((rs >= 4 && rs <= 6) || now - p.asked_at > 15000000000LL) {
                log("puppet: player %d's actor was not made (record state %d)", i, rs);
                p.state = 0;
            }
            break;
        }
        case 2:
            if (want) {
                place(c, i, p, p.pos, p.yaw);
                p.hidden = false;
                if (now >= p.gear_check) {
                    p.gear_check = now + 500000000LL;
                    const std::string g = ses.gear(i);
                    if (!g.empty() && g != p.gear_shown) {
                        wear(c, p.actor, p.gear_shown, g);
                        log("puppet: player %d wears %s", i, g.c_str());
                        p.gear_shown = g;
                    }
                }
            } else if (!p.hidden) {
                const float away[3] = {p.pos[0], p.pos[1] - 500.0f, p.pos[2]};
                place(c, i, p, away, 0);
                p.hidden = true;
            }
            break;
        }
        g_actor[i] = p.state == 2 ? p.actor : 0;
        g_child[i] = p.child;
        g_placed[i] = p.state == 2 && !p.hidden;
    }
    held_tick(c, map, loading, now);
}

}  // namespace coop
