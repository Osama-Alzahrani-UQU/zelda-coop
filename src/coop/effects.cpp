// The players' blasts in every world (effects.h).
//
//   this game:  each frame the actors around Link are looked at: a bomb (Item_Bomb, Item_Bombchu) the game made here near
//               Link (not one the map placed, not one this mod made) is followed while it lives; when it is gone in the
//               map Link is still in, it went off: where it was goes to the others (reliable, relayed by the host).
//   the others: the same actor is made at that place (spawn_actor; in the world once its request is done, f_8038fbd0) and
//               given a blow of the game's own (fellow_life_message): it goes off there, its own blast hurting and breaking
//               what is near it in that world (enemies, walls, rocks, Link too). The hits that blast makes there are not
//               sent back (the bomb's own game sends its own): no hit is counted twice.
//   projectiles: the game makes its seeds, nuts and arrows ahead (at load) and only launches them, so they cannot be made
//               here as they fly: this player's (one of them back in the actors' ring near Link) is followed and where it is
//               sent; the others' are drawn by the overlay where they are, their end as their own burst (a Deku Nut's flash,
//               a seed's or an arrow's spark). What they hit is shared as hits.
#include "effects.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "coop.h"
#include "fellow.h"
#include "game.h"
#include "pose.h"
#include "runtime.h"
#include "session.h"

namespace coop {

namespace {
constexpr uint64_t kGameActorVt = 0x84b45e18ULL, kFnActivate = 0x8038fbd0ULL;
constexpr float kOwnReach = 4.0f;          // (a bomb first seen this near Link is his: taken out, set down or thrown)
constexpr float kBlastReach = 80.0f;       // (another player's blast made here when this near Link)
constexpr float kBlastZone = 7.0f;         // (the hits within this of a blast made here are that blast's: not sent)
constexpr int64_t kBlastZoneNs = 1500000000LL;

bool heap(uint64_t a) { return a >= kHeapBase && a < kHeapBase + 0x100000000ULL; }

float dist3(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

bool is_blast(const std::string& name) { return name == "Item_Bomb" || name == "Item_Bombchu"; }

// ---- this game's bombs, followed while they live
struct Own {
    std::string name;
    float pos[3];
    int64_t seen;
};
std::unordered_map<uint64_t, Own> g_own;
std::unordered_map<uint64_t, int64_t> g_seen;   // (every actor near Link: when last seen; a new one is looked at once)
std::string g_map;

// ---- the others' blasts, made here
struct Made {
    std::string name;
    uint64_t link = 0, ga = 0;
    int state = 1;   // 1 asked, 2 in the world (struck)
    float pos[3] = {0, 0, 0};
    int64_t asked = 0, next_log = 0;
    int logs = 0;
    bool test = false;   // (probe: made only, not struck)
};
std::vector<Made> g_made;

std::mutex g_zone_mutex;
struct Zone {
    float pos[3];
    int64_t until;
};
std::vector<Zone> g_zones;   // (where the others' blasts went off here, a while: the hits there are theirs)

bool made_here(uint64_t ga) {
    for (const Made& m : g_made)
        if (m.ga == ga && !m.test) return true;   // (a probe's test actor counts as this player's own)
    return false;
}

void made_tick(Ctx* c, int64_t now) {
    for (auto it = g_made.begin(); it != g_made.end();) {
        Made& m = *it;
        bool drop = false;
        if (m.state == 1) {
            const uint64_t rec = r64(m.link);
            const int rs = heap(rec) ? (int)(r32(rec) & 0xffff) : 0;
            if (rs == 3) {
                m.ga = guest_call(c, kFnActivate, rec, 1, 0);
                m.state = 2;
                m.next_log = now;
                if (!m.test) {
                    {
                        std::lock_guard<std::mutex> lock(g_zone_mutex);
                        g_zones.push_back({{m.pos[0], m.pos[1], m.pos[2]}, now + kBlastZoneNs});
                    }
                    const bool blow = fellow_life_message(c, m.ga, -1);
                    log("fx: another player's %s goes off here at %.1f %.1f %.1f%s", m.name.c_str(), m.pos[0], m.pos[1], m.pos[2],
                        blow ? "" : " (it took no blow)");
                } else {
                    log("fx: %s made (%#llx)", m.name.c_str(), (unsigned long long)m.ga);
                }
            } else if ((rs >= 4 && rs <= 6) || now - m.asked > 3000000000LL) {
                log("fx: %s not made (record state %d)", m.name.c_str(), rs);
                drop = true;
            }
        } else if (m.test && debug_on() && now >= m.next_log && m.logs < 12) {
            m.next_log = now + 400000000LL;
            m.logs++;
            float p[3] = {0, 0, 0};
            const bool alive = heap(m.ga) && r64(m.ga) == kGameActorVt && game_actor_name(m.ga) == m.name && game_actor_pos(m.ga, p);
            log("fx: %s %s at %.2f %.2f %.2f", m.name.c_str(), alive ? "in the world" : "gone", p[0], p[1], p[2]);
            if (!alive) drop = true;
        } else if (now - m.asked > 10000000000LL) {
            drop = true;   // (kept a while: its actor is not taken for this game's own)
        }
        it = drop ? g_made.erase(it) : it + 1;
    }
}

std::vector<std::pair<std::string, float>> g_test;   // (probe: actors to make in front of Link: name, height)

// ---- this player's projectiles (actors the game launches from a store of its own: in the actors' ring while they fly)
struct Proj {
    uint32_t id;
    int kind;
    float pos[3];
    int64_t seen, sent;
};
std::unordered_map<uint64_t, Proj> g_proj;
uint32_t g_next_proj = 1;

int proj_kind(const std::string& name) {
    if (name == "Shootable_DekuSeed") return kProjSeed;
    if (name == "Item_DekuNut") return kProjNut;
    if (name.rfind("Item_Arrow_", 0) == 0) return kProjArrow;
    if (name == "Shootable_Hook" || name == "Shootable_HookLong") return kProjHook;
    return 0;
}

// ---- the others' projectiles, for the overlay
struct Remote {
    int owner;
    uint32_t id;
    int kind;
    float pos[3], prev[3];
    bool end;
    int64_t got, end_at;
};
std::mutex g_remote_mutex;
std::vector<Remote> g_remote;

// ---- doors: the ones near Link, their animation as last read; the others' door animations played here (for a while
// their change is not this player's own)
bool is_door(const std::string& name) {
    if (name.rfind("Obj_", 0) != 0 || name.find("Door") == std::string::npos) return false;
    static const char* const kNot[] = {"Frame", "Surround", "ChainLock", "AirWall", "Locator", "Area", "NpcOnly", "Restricted"};
    for (const char* w : kNot)
        if (name.find(w) != std::string::npos) return false;
    return true;
}
bool door_anim(const std::string& a) {
    return a.rfind("Push", 0) == 0 || a.rfind("Close", 0) == 0 || a.rfind("Open", 0) == 0;
}
// (an actor's animation playing now: its AS component (components +0x40) -> +0x18 -> +0x18, the first slot's node, its
// name at +0x10)
std::string anim_of(uint64_t ga) { return game_actor_anim(ga); }
struct Door {
    std::string anim;
    int64_t quiet_until = 0;   // (an animation played here for another player: its change not sent back)
};
std::unordered_map<uint64_t, Door> g_doors;

// ---- the Deku Stick's fire: this player's (lit when a fire's ignite message came from near Link while a stick is out);
// the others' (until when, by slot), their burning end found each frame for the overlay
std::atomic<uint64_t> g_lit_sender{0};
std::atomic<int64_t> g_lit_at{0};
int64_t g_my_fire_until = 0;
int g_test_fire = 0;
int64_t g_fire_until[kMaxPlayers] = {0, 0, 0, 0};
std::mutex g_fire_mutex;
std::vector<StickFire> g_fires;

uint32_t fnv(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

// (this player's Link holds a Deku Stick out: Weapon_DekuStick back in the actors' ring within reach of Link)
bool stick_out(uint64_t mover, const float me[3], float tip[3]) {
    uint64_t a = mover;
    for (int steps = 0; steps < 8192; steps++) {
        const uint64_t next = r64(a + 0x190);
        if (!heap(next)) break;
        a = next - 0x190;
        if (a == mover || r64(a) != kGameActorVt) break;
        const uint64_t sn = r64(a + 0x260);
        if (sn < 0x80000000ULL || r8(sn) != 'W' || r8(sn + 7) != 'D') continue;   // ("Weapon_D...")
        float p[3];
        if (game_actor_name(a) != "Weapon_DekuStick" || !game_actor_pos(a, p) || dist3(p, me) > 2.0f) continue;
        if (tip) memcpy(tip, p, sizeof p);
        return true;
    }
    return false;
}

// (a held stick's burning end: its root along its own length, the model's +Y, 0.75 m)
void stick_tip(uint64_t stick, float out[3]) {
    float p[3];
    game_actor_pos(stick, p);
    const float ax = rf(stick + 0x328 + 4 * 1), ay = rf(stick + 0x328 + 4 * 4), az = rf(stick + 0x328 + 4 * 7);   // (the rotation's second column)
    for (int k = 0; k < 3; k++) out[k] = p[k];
    out[0] += 0.75f * ax, out[1] += 0.75f * ay, out[2] += 0.75f * az;
}

std::mutex g_struck_mutex;
std::vector<uint64_t> g_struck;   // (bombs whose life went to 0 (going off), from the life hook)
}  // namespace

void effects_actor_struck(uint64_t body, uint32_t after) {
    if (after != 0) return;
    const uint64_t ga = game_actor_of(body);
    if (!ga || !is_blast(game_actor_name(ga))) return;
    std::lock_guard<std::mutex> lock(g_struck_mutex);
    if (g_struck.size() < 64) g_struck.push_back(ga);
}

void effects_test_spawn(const std::string& name, float up) { g_test.push_back({name, up}); }

std::vector<ProjectileView> effects_projectiles() {
    std::lock_guard<std::mutex> lock(g_remote_mutex);
    const int64_t now = now_ns();
    std::vector<ProjectileView> out;
    for (const Remote& r : g_remote) {
        ProjectileView v;
        v.owner = r.owner, v.kind = r.kind, v.end = r.end;
        memcpy(v.pos, r.pos, sizeof v.pos);
        memcpy(v.prev, r.prev, sizeof v.prev);
        v.age = (float)(now - (r.end ? r.end_at : r.got)) / 1e9f;
        out.push_back(v);
    }
    return out;
}

void effects_stick_lit(uint64_t sender) {
    g_lit_sender = sender;
    g_lit_at = now_ns();
}

void effects_test_stick(int seconds) { g_test_fire = seconds; }

std::vector<StickFire> effects_stick_fires() {
    std::lock_guard<std::mutex> lock(g_fire_mutex);
    return g_fires;
}

bool effects_remote_blast_near(const float pos[3]) {
    std::lock_guard<std::mutex> lock(g_zone_mutex);
    const int64_t now = now_ns();
    g_zones.erase(std::remove_if(g_zones.begin(), g_zones.end(), [&](const Zone& z) { return now > z.until; }), g_zones.end());
    for (const Zone& z : g_zones)
        if (dist3(z.pos, pos) < kBlastZone) return true;
    return false;
}

void effects_update(Ctx* c, uint64_t mover, const char* map, bool loading, int64_t now) {
    Session& ses = Session::get();
    float me[3];
    if (loading || !map[0] || !game_actor_pos(mover, me)) return;
    if (g_map != map) {
        g_own.clear(), g_seen.clear(), g_made.clear(), g_proj.clear(), g_doors.clear(), g_map = map;
        std::lock_guard<std::mutex> lock(g_remote_mutex);
        g_remote.clear();
    }
    // ---- the others' blasts: made here, near this player
    for (const GameAction& a : ses.take_effects()) {
        const size_t cut = a.s.find('\n');
        if (cut == std::string::npos || a.s.substr(0, cut) != map) continue;
        const std::string name = a.s.substr(cut + 1);
        if (!is_blast(name) || dist3(a.pos, me) > kBlastReach) continue;
        Made m;
        m.name = name;
        memcpy(m.pos, a.pos, sizeof m.pos);
        float at[3] = {a.pos[0], a.pos[1] + 0.2f, a.pos[2]};
        m.link = spawn_actor(c, name.c_str(), at, 0, 0);
        m.asked = now;
        if (m.link) g_made.push_back(m);
    }
    for (const auto& [name, up] : g_test) {
        const LocalSnapshot s = local_snapshot();
        Made m;
        m.name = name, m.test = true;
        m.pos[0] = me[0] + 2.0f * sinf(s.yaw), m.pos[1] = me[1] + up, m.pos[2] = me[2] + 2.0f * cosf(s.yaw);
        m.link = spawn_actor(c, name.c_str(), m.pos, s.yaw, 0);
        m.asked = now;
        if (m.link) g_made.push_back(m);
    }
    g_test.clear();
    made_tick(c, now);
    // ---- this player's Deku Stick lit (a fire's message from near Link, the stick out): the others told; out when put away
    {
        float tip[3];
        const bool out = stick_out(mover, me, tip);
        const int64_t lit = g_lit_at.exchange(0);
        if (lit && out) {
            const uint64_t from = g_lit_sender.load();
            float fp[3];
            const bool close_by = heap(from) && r64(from) == kGameActorVt && game_actor_pos(from, fp) && dist3(fp, me) < 4.0f;
            if (close_by || from == mover) {
                if (now >= g_my_fire_until) {
                    ses.send_stick_fire(20);
                    log("fx: this player's Deku Stick lit (by %s): the others told", game_actor_name(from).c_str());
                }
                g_my_fire_until = now + 20000000000LL;
            }
        }
        if (g_test_fire > 0) {
            g_my_fire_until = now + (int64_t)g_test_fire * 1000000000LL;
            ses.send_stick_fire(g_test_fire);
            log("fx: this player's Deku Stick as lit for %d s (probe)", g_test_fire);
            g_test_fire = 0;
        }
        if (g_my_fire_until && (!out || now >= g_my_fire_until)) {
            if (!out) ses.send_stick_fire(0), log("fx: this player's Deku Stick put away: its fire out");
            g_my_fire_until = 0;
        }
    }
    // ---- the others' sticks lit: their burning end, each frame, for the overlay
    {
        int slot = -1, seconds = 0;
        while (ses.take_stick_fire(&slot, &seconds))
            if (slot >= 0 && slot < kMaxPlayers) g_fire_until[slot] = seconds ? now + (int64_t)seconds * 1000000000LL : 0;
        static const uint32_t stick = fnv("Weapon_DekuStick");
        std::vector<StickFire> fires;
        for (int s = 0; s < kMaxPlayers; s++) {
            if (now >= g_fire_until[s]) continue;
            const uint64_t a = puppet_held(s, stick);
            if (!heap(a) || r64(a) != kGameActorVt) continue;
            StickFire f;
            f.slot = s;
            // (the stick's model is drawn in the renderer's frame (its actor parked under the map): its end there, moved
            // by the offset between that frame and the world taken from that player's Link (its root bone, its place))
            bool bones = pose_actor_tip(a, 0.75f, f.pos);
            const uint64_t link = puppet_actor(s);
            float root[3], at[3];
            if (bones && link && pose_actor_root(link, root) && game_actor_pos(link, at))
                for (int k = 0; k < 3; k++) f.pos[k] += at[k] - root[k];
            else bones = false;
            if (!bones) continue;
            fires.push_back(f);
            static int64_t next_log = 0;
            if (debug_on() && now >= next_log) {
                next_log = now + 1000000000LL;
                float gp3[3] = {0, 0, 0}, hp[3] = {0, 0, 0};
                bool child = false;
                game_actor_pos(a, gp3);
                puppet_position(s, hp, &child);
                log("fx: player %d's stick burning: end %.2f %.2f %.2f (%s), stick actor at %.2f %.2f %.2f, Link at %.2f %.2f %.2f", s,
                    f.pos[0], f.pos[1], f.pos[2], bones ? "bones" : "actor", gp3[0], gp3[1], gp3[2], hp[0], hp[1], hp[2]);
            }
        }
        std::lock_guard<std::mutex> lock(g_fire_mutex);
        g_fires.swap(fires);
    }
    // ---- the others' doors: the same door (its placement) plays the same animation here
    for (const GameAction& a : ses.take_doors()) {
        const size_t cut = a.s.find('\n');
        if (cut == std::string::npos || a.s.substr(0, cut) != map) continue;
        const std::string anim = a.s.substr(cut + 1);
        uint64_t d = mover;
        bool found = false;
        for (int steps = 0; steps < 8192 && !found; steps++) {
            const uint64_t next = r64(d + 0x190);
            if (!heap(next)) break;
            d = next - 0x190;
            if (d == mover || r64(d) != kGameActorVt) break;
            const uint64_t rec = r64(d + 0x3d8);
            found = heap(rec) && r64(rec + 0x88) == a.key;
        }
        if (!found) continue;
        Door& door = g_doors[d];
        door.quiet_until = now + 2500000000LL;
        const bool played = game_play_as(c, d, anim.c_str(), 1.0f, false);
        door.anim = anim_of(d);
        log("fx: a door %s here as in another player's game (%s)%s", game_actor_name(d).c_str(), anim.c_str(), played ? "" : ": not played");
    }
    // ---- the others' projectiles: kept for the overlay (an end shown a moment, one not heard of for a while dropped)
    {
        std::lock_guard<std::mutex> lock(g_remote_mutex);
        for (const ProjectileSnap& p : ses.take_projectiles()) {
            if (p.map != map) continue;
            auto it = std::find_if(g_remote.begin(), g_remote.end(), [&](const Remote& r) { return r.owner == p.owner && r.id == p.id; });
            if (it == g_remote.end()) {
                if (g_remote.size() >= 64) continue;
                Remote r{p.owner, p.id, p.kind, {p.pos[0], p.pos[1], p.pos[2]}, {p.pos[0], p.pos[1], p.pos[2]}, false, now, 0};
                g_remote.push_back(r);
                if (debug_on()) log("fx: player %d's projectile %u (kind %d) in flight at %.1f %.1f %.1f", p.owner, p.id, p.kind, p.pos[0], p.pos[1], p.pos[2]);
                it = g_remote.end() - 1;
            } else if (!it->end) {
                memcpy(it->prev, it->pos, sizeof it->prev);
            }
            if (it->end) continue;
            memcpy(it->pos, p.pos, sizeof it->pos);
            it->got = now;
            if (p.end) {
                it->end = true, it->end_at = now;
                if (debug_on()) log("fx: player %d's projectile %u ended at %.1f %.1f %.1f", p.owner, p.id, p.pos[0], p.pos[1], p.pos[2]);
            }
        }
        g_remote.erase(std::remove_if(g_remote.begin(), g_remote.end(),
                                      [&](const Remote& r) { return r.end ? now - r.end_at > 700000000LL : now - r.got > 1500000000LL; }),
                       g_remote.end());
    }
    // ---- this game's actors near Link: its bombs and projectiles followed; (developer) every new kind logged once
    const bool share = ses.active() && ses.others_present();
    static std::vector<std::string> logged;
    uint64_t ga = mover;
    for (int steps = 0; steps < 8192; steps++) {
        const uint64_t next = r64(ga + 0x190);
        if (!heap(next)) break;
        ga = next - 0x190;
        if (ga == mover || r64(ga) != kGameActorVt) break;
        // (a projectile: launched (in the ring again) near Link, followed while it flies)
        if (share) {
            auto pr = g_proj.find(ga);
            if (pr != g_proj.end()) {
                float p[3];
                if (game_actor_pos(ga, p)) memcpy(pr->second.pos, p, sizeof p);
                pr->second.seen = now;
                if (now - pr->second.sent >= 33000000LL) {
                    pr->second.sent = now;
                    ses.send_projectile(map, pr->second.id, pr->second.kind, false, pr->second.pos);
                }
                continue;
            }
            const uint64_t sn = r64(ga + 0x260);
            if (sn >= 0x80000000ULL && (r8(sn) == 'S' || r8(sn) == 'I')) {   // (Shootable_..., Item_...)
                const std::string name = game_actor_name(ga);
                const int kind = proj_kind(name);
                float p[3];
                if (kind && game_actor_pos(ga, p) && dist3(p, me) < kOwnReach && g_proj.size() < 32) {
                    Proj pj{g_next_proj++, kind, {p[0], p[1], p[2]}, now, now};
                    g_proj[ga] = pj;
                    ses.send_projectile(map, pj.id, kind, false, pj.pos);
                    continue;
                }
            }
        }
        // (a door near Link: its opening or closing sent, unless it plays what another player's game asked)
        {
            const uint64_t sn = r64(ga + 0x260);
            if (sn >= 0x80000000ULL && r8(sn) == 'O' && r8(sn + 4) != 0) {
                auto dr = g_doors.find(ga);
                if (dr != g_doors.end() || is_door(game_actor_name(ga))) {
                    float p[3];
                    if (game_actor_pos(ga, p) && dist3(p, me) < 15.0f) {
                        Door& door = g_doors[ga];
                        const std::string anim = anim_of(ga);
                        if (!anim.empty() && anim != door.anim) {
                            const bool mine = now >= door.quiet_until && dist3(p, me) < 6.0f;
                            if (share && mine && door_anim(anim)) {
                                const uint64_t rec = r64(ga + 0x3d8);
                                if (heap(rec)) {
                                    ses.send_door(map, r64(rec + 0x88), anim);
                                    log("fx: door %s %s here: sent", game_actor_name(ga).c_str(), anim.c_str());
                                }
                            }
                            door.anim = anim;
                        }
                    }
                }
            }
        }
        auto own = g_own.find(ga);
        if (own != g_own.end()) {
            float p[3];
            if (game_actor_pos(ga, p)) memcpy(own->second.pos, p, sizeof p);
            own->second.seen = now;
            continue;
        }
        auto it = g_seen.find(ga);
        if (it != g_seen.end()) {
            it->second = now;
            continue;
        }
        g_seen[ga] = now;
        float p[3];
        if (!game_actor_pos(ga, p) || dist3(p, me) > 12.0f) continue;
        const std::string name = game_actor_name(ga);
        if (share && is_blast(name) && dist3(p, me) < kOwnReach && !made_here(ga)) {
            g_own[ga] = Own{name, {p[0], p[1], p[2]}, now};
            log("fx: this player's %s followed", name.c_str());
        }
        if (debug_on() && !name.empty() && std::find(logged.begin(), logged.end(), name) == logged.end() && logged.size() < 400) {
            logged.push_back(name);
            log("fx: new actor %s %.1f m from Link (ga %#llx)", name.c_str(), dist3(p, me), (unsigned long long)ga);
        }
    }
    // (a bomb of this player's going off (its life to 0), or gone in the map Link is still in: it went off; its place to the
    // others)
    std::vector<uint64_t> struck;
    {
        std::lock_guard<std::mutex> lock(g_struck_mutex);
        struck.swap(g_struck);
    }
    for (auto it = g_own.begin(); it != g_own.end();) {
        const bool off = std::find(struck.begin(), struck.end(), it->first) != struck.end();
        if (it->second.seen == now && !off) {
            ++it;
            continue;
        }
        if (share && dist3(it->second.pos, me) < kBlastReach) {
            ses.send_effect(map, it->second.name, it->second.pos);
            log("fx: this player's %s went off at %.1f %.1f %.1f: sent", it->second.name.c_str(), it->second.pos[0], it->second.pos[1],
                it->second.pos[2]);
        }
        g_seen[it->first] = now;   // (its actor, while it lingers after going off: not taken for a new bomb)
        it = g_own.erase(it);
    }
    // (a projectile out of the ring: it hit, flashed or came back: its end where it was)
    for (auto it = g_proj.begin(); it != g_proj.end();) {
        if (it->second.seen == now) {
            ++it;
            continue;
        }
        if (share) ses.send_projectile(map, it->second.id, it->second.kind, true, it->second.pos);
        it = g_proj.erase(it);
    }
    for (auto it = g_seen.begin(); it != g_seen.end();) it = now - it->second > 2000000000LL ? g_seen.erase(it) : std::next(it);
}

}  // namespace coop
