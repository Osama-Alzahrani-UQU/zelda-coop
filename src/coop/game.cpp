// The game as the mod sees it, on the game's threads: Link's state each frame (sent to the others), the session's actions
// done there (gifts into and out of the pouch, the trip to the host), the camera for the nameplates, the pause menu.
//
// Addresses are of the supported build (e0f3ccfa); most come from Longshot's own research in its runtime (host/hos_main.c,
// host/oot_menu.c, host/oot_fov.c): Link's actor, his life, his controller, the pouch, GameData and the debug teleport.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <deque>
#include <mutex>

#include "coop.h"
#include "coopsave.h"
#include "dialogs.h"
#include "effects.h"
#include "enemies.h"
#include "world.h"
#include "fellow.h"
#include "game.h"
#include "menus.h"
#include "pose.h"
#include "runtime.h"
#include "session.h"
#include "title.h"

namespace coop {

namespace {
std::mutex g_snap_mutex;
LocalSnapshot g_snap;
std::mutex g_cam_mutex;
CameraView g_cam;
std::mutex g_actions_mutex;
std::deque<GameAction> g_actions;
std::atomic<int64_t> g_pause_ns{0}, g_play_ns{0};
std::atomic<uint64_t> g_pause_screen{0};   // (the pause menu's screen object, while it is up)

constexpr uint64_t kGameDataSlot = 0x84c9e180ULL;   // GameDataMgr
constexpr uint64_t kEventMgrSlot = 0x84c9e090ULL;   // EventMgr
constexpr uint64_t kCameraMgrSlot = 0x84ca6bc0ULL;  // CameraMgr
constexpr uint64_t kLookAtCameraVt = 0x84c18a88ULL; // sead::LookAtCamera
constexpr uint64_t kFnGameDataBool = 0x8022d9c0ULL, kFnGameDataString = 0x80cd0ea0ULL;
constexpr uint32_t kHashIsChildhood = 0xcea220d3u, kHashCurrentBanc = 0x1d6189dau, kHashLoadingBanc = 0xef2b60c2u;
constexpr uint64_t kFnPouchEdit = 0x828d4a70ULL;    // OneShotAddOrRemovePouchContent
constexpr uint64_t kFnPouchCount = 0x805b63e0ULL;   // QueryGetPouchContentNumByActorName's count
constexpr uint64_t kFnChangePosition = 0x806d2530ULL;   // OneShotPhysicsCctChangePosition (the controller's vtable +0xe8)
constexpr uint64_t kPauseFocusHandler = 0x802ed660ULL;  // (the pause menu's vtable slot 46)

uint64_t singleton(uint64_t slot) {
    const uint64_t h = r64(slot);
    return h ? r64(h) : 0;
}

// (host/hos_main.c hos_oot_is_player: the actor's vtable and its resource record's name "Player")
bool is_player(uint64_t actor) {
    if (r64(actor) != 0x84b6c460ULL) return false;
    const uint64_t res = r64(actor + 16);
    return res && r32(res + 88) == 0x79616c50 && r16(res + 92) == 0x7265 && r8(res + 94) == 0;
}

// GameDataMgr getters by MurMur3 name hash (host/hos_main.c): f_8022d9c0 (bool) and f_80cd0ea0 (String64), (mgr, &out, hash)
bool gamedata_bool(Ctx* c, uint32_t hash, bool* value) {
    const uint64_t mgr = singleton(kGameDataSlot);
    A64Fn fn = rt().a64_lookup(kFnGameDataBool);
    if (!mgr || !fn) return false;
    Ctx saved;
    memcpy(&saved, c, sizeof saved);
    const uint64_t base = (c->x[31] - 0x80) & ~0xFULL;
    w64(base, 0);
    c->x[31] = base;
    c->x[0] = mgr, c->x[1] = base, c->x[2] = hash, c->x[30] = 0;
    fn(c);
    const bool ok = c->x[0] & 1;
    *value = r8(base) & 1;
    memcpy(c, &saved, sizeof saved);
    return ok;
}
bool gamedata_string(Ctx* c, uint32_t hash, char* out, size_t cap) {
    out[0] = 0;
    const uint64_t mgr = singleton(kGameDataSlot);
    A64Fn fn = rt().a64_lookup(kFnGameDataString);
    if (!mgr || !fn) return false;
    Ctx saved;
    memcpy(&saved, c, sizeof saved);
    const uint64_t base = (c->x[31] - 0x80) & ~0xFULL;
    w64(base, 0);
    c->x[31] = base;
    c->x[0] = mgr, c->x[1] = base, c->x[2] = hash, c->x[30] = 0;
    fn(c);
    const bool found = c->x[0] & 1;
    const uint64_t text = r64(base);
    memcpy(c, &saved, sizeof saved);
    if (!found || !text) return false;
    const std::string s = cstr_at(text, cap - 1);
    snprintf(out, cap, "%s", s.c_str());
    return true;
}

}  // namespace

// (host/hos_main.c oot_pouch_count) f_805b63e0(pouch manager, &name, 0, 0): the count of one pouch content, -1 not ready
int pouch_count(Ctx* c, const char* name) {
    const uint64_t manager = r64(0x84e98e28ULL);
    A64Fn fn = rt().a64_lookup(kFnPouchCount);
    const size_t len = strlen(name);
    if (!manager || !fn || len >= 0x60) return -1;
    Ctx saved;
    memcpy(&saved, c, sizeof saved);
    const uint64_t frame = (c->x[31] - 0x80) & ~0xFULL;
    poke(frame + 0x10, name, len + 1);
    w64(frame, frame + 0x10);
    c->x[31] = frame;
    c->x[0] = manager, c->x[1] = frame, c->x[2] = 0, c->x[3] = 0, c->x[30] = 0;
    fn(c);
    const int n = (int)(int32_t)c->x[0];
    memcpy(c, &saved, sizeof saved);
    return n;
}

// (host/hos_main.c oot_pouch_change) OneShotAddOrRemovePouchContent for one content by actor name: delta > 0 adds, < 0
// removes. The event's status (the result word's top bits), -1 when the pouch is not ready
int pouch_change(Ctx* c, const char* name, int delta) {
    const uint64_t pouch = r64(0x84c9e0d0ULL), state = r64(0x84c9e2a8ULL);
    A64Fn edit = rt().a64_lookup(kFnPouchEdit);
    const size_t len = strlen(name);
    if (!edit || !pouch || !r64(pouch) || !state || !r64(state) || !r64(0x84eb6888ULL) || !r64(0x84e8c060ULL) || !delta || len >= 0x60)
        return -1;
    Ctx saved;
    memcpy(&saved, c, sizeof saved);
    const uint64_t ev = (c->x[31] - 0xe0) & ~0xFULL;
    memset(gp(ev), 0, 0xe0);
    poke(ev + 0x80, name, len + 1);
    w64(ev + 0x20, ev + 0x60);
    w64(ev + 0x30, ev + 0x70);
    w64(ev + 0x38, ev + 0x68);
    w64(ev + 0x48, ev + 0x74);
    w64(ev + 0x50, ev + 0x74);
    w64(ev + 0x60, ev + 0x80);
    w64(ev + 0x68, 0x83f0a6d8ULL);   // ("Script")
    w32(ev + 0x70, (uint32_t)delta);
    c->x[31] = ev;
    c->x[0] = ev, c->x[30] = 0;
    edit(c);
    const int result = (int)(r32(ev + 0x18) >> 30);
    memcpy(c, &saved, sizeof saved);
    return result;
}

namespace {
// Link's controller (vtable 0x84c54f10, the one OneShotPhysicsCctChangePosition moves) from his components
uint64_t player_controller(uint64_t components) {
    static const unsigned offsets[] = {80, 32, 16, 8};
    uint64_t ctl = components;
    for (unsigned off : offsets) {
        ctl = r64(ctl + off);
        if (!ctl) return 0;
    }
    return r64(ctl) == 0x84c54f10ULL ? ctl : 0;
}

void move_player(Ctx* c, uint64_t components, const float to[3]) {
    const uint64_t ctl = player_controller(components);
    A64Fn fn = rt().a64_lookup(kFnChangePosition);
    if (!ctl || !fn || r64(0x84c54f10ULL + 0xe8) != kFnChangePosition) return;
    Ctx saved;
    memcpy(&saved, c, sizeof saved);
    const uint64_t arg = (c->x[31] - 0x40) & ~0xFULL;
    for (int i = 0; i < 3; i++) wf(arg + 4 * (uint64_t)i, to[i]);
    w32(arg + 12, 0);
    c->x[31] = arg;
    c->x[0] = ctl, c->x[1] = arg, c->x[30] = 0;
    fn(c);
    memcpy(c, &saved, sizeof saved);
}

uint64_t gameplay_view() {
    const uint64_t manager = singleton(kCameraMgrSlot);
    const uint64_t cameras = manager && r32(manager + 32) ? r64(manager + 40) : 0;
    const uint64_t camera = cameras ? r64(cameras) : 0;
    return camera ? r64(camera + 120) : 0;
}

// the trip to the host: the debug teleport's destination with the host's map, then beside him
struct Trip {
    int stage = 0;          // 0 none, 1 age asked, 2 teleport asked, 3 waiting to arrive, 4 placing
    int ticket = 0;
    int64_t at = 0;
    char map[48] = {0};
    float pos[3] = {0, 0, 0};
    bool child = false;
} g_trip;
bool g_trip_done = false;

// ---- the save (game_save_now): asked from any thread, done at Link's next update when nothing stops it
std::atomic<int> g_save{0};          // kSaveIdle, kSaveAsked, kSaveRunning, kSaveDone, kSaveFailed
std::atomic<int64_t> g_save_at{0};
std::atomic<bool> g_save_stopped{false};   // (the window is closing: no save starts any more)
char g_save_why[48] = "";
std::atomic<bool> g_save_quiet{false};
std::atomic<int> g_save_tries{0};   // (a request the game took but wrote nothing for: asked again, twice at most)
void save_update(Ctx* c, int64_t now, bool loading);

// this player's items for Give To, twice a second while a dialog is open: from Link's update in play, from the pause menu's
// update while the game is paused (the pouch read, not changed)
void update_counts(Ctx* c, int64_t now) {
    static std::atomic<int64_t> next{0};
    int64_t due = next.load();
    if (!dialogs_open() || now < due || !next.compare_exchange_strong(due, now + 500000000LL)) return;
    std::vector<int> counts;
    for (const Item& it : giftable_items()) counts.push_back(item_count(c, it.pouch));
    dialogs_set_counts(counts);
}

// ---- this Link's gear, as the game's own equip reads it (f_82e16c80): GameData's player equipment struct (key 0x060890b0,
// in the manager's table at +0xc20 / +0xc2c, entries of 32 bytes at +0xc18) and its string members by their murmur3 hash,
// read with f_80a9a860(manager, &string, &{entry, 0, 0}, hash)
constexpr uint64_t kGameDataMgrSlot = 0x84c9e180ULL, kFnGameDataMember = 0x80a9a860ULL, kEmptyString = 0x83f04f8cULL;
constexpr uint32_t kPlayerEquipStruct = 0x060890b0u;
const char* const kGearMembers[] = {"EquipWeapon", "EquipShield", "EquipHeadArmor", "EquipArmor", "EquipBoot", "EquipHandArmor"};

uint32_t murmur3(const char* s) {
    const size_t n = strlen(s);
    uint32_t h = 0;
    auto rotl = [](uint32_t x, int r) { return (x << r) | (x >> (32 - r)); };
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t k;
        memcpy(&k, s + i, 4);
        k *= 0xcc9e2d51u, k = rotl(k, 15), k *= 0x1b873593u;
        h ^= k, h = rotl(h, 13), h = h * 5 + 0xe6546b64u;
    }
    uint32_t k = 0;
    for (size_t j = 0; i + j < n; j++) k |= (uint32_t)(uint8_t)s[i + j] << (8 * j);
    if (n & 3) k *= 0xcc9e2d51u, k = rotl(k, 15), k *= 0x1b873593u, h ^= k;
    h ^= (uint32_t)n, h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    return h;
}

uint64_t gamedata_entry(uint64_t mgr, uint32_t key) {
    const uint64_t table = r64(mgr + 0xc20), entries = r64(mgr + 0xc18);
    const uint32_t cap = r32(mgr + 0xc2c), count = r32(mgr + 0xc10);
    if (!table || !entries || !cap) return 0;
    for (uint32_t i = key % cap, tries = 0; tries < cap; tries++, i = (i + 1) % cap) {
        const uint32_t k = r32(table + 8 * (uint64_t)i);
        if (k == key) {
            const uint32_t idx = r32(table + 8 * (uint64_t)i + 4);
            return entries + 32 * (uint64_t)(idx < count ? idx : 0);
        }
        if (!k) return 0;
    }
    return 0;
}

std::string read_gear(Ctx* c) {
    static uint64_t buf = 0;
    if (!buf) buf = guest_alloc(0x20, 16);
    const uint64_t mgr = singleton(kGameDataMgrSlot);
    const uint64_t entry = mgr ? gamedata_entry(mgr, kPlayerEquipStruct) : 0;
    if (!buf || !entry || !rt().a64_lookup(kFnGameDataMember)) return {};
    std::string out;
    for (const char* member : kGearMembers) {
        const uint64_t cell[4] = {kEmptyString, entry, 0, 0};
        poke(buf, cell, sizeof cell);
        std::string name;
        if (guest_call(c, kFnGameDataMember, mgr, buf, buf + 8, murmur3(member)) & 1) name = cstr_at(r64(buf), 63);
        for (char ch : name)
            if (!(isalnum((unsigned char)ch) || ch == '_')) name.clear();
        out += (out.empty() && member == kGearMembers[0] ? "" : "|") + name;
    }
    return out;
}

// ---- the bags: this game holds no Deku Sticks, Nuts, Seeds, Bombs or Arrows (0 of 0) until it has their bag (the first
// stick bundle, the seed bag with the slingshot...): Item_StickBag_Small and its kind. A gift or a pick-up of those before
// it is refused by the game. So a player in a session gets the bags the host has (as the starting gear goes), and a gift
// of something whose bag this game lacks brings the smallest bag first. The wallets are the rupees' bags.
struct BagKind {
    const char* item;
    const char* bags[3];   // (small, medium, large; the wallet's small one is everyone's from the start)
};
const BagKind kBags[] = {
    {"Item_DekuStick", {"Item_StickBag_Small", "Item_StickBag_Medium", "Item_StickBag_Large"}},
    {"Item_DekuNut", {"Item_NutBag_Small", "Item_NutBag_Medium", "Item_NutBag_Large"}},
    {"Item_DekuSeed", {"Item_BulletBag_Small", "Item_BulletBag_Medium", "Item_BulletBag_Large"}},
    {"Item_Bomb", {"Item_BombBag_Small", "Item_BombBag_Medium", "Item_BombBag_Large"}},
    {"Item_Arrow_Normal", {"Item_Quiver_Small", "Item_Quiver_Medium", "Item_Quiver_Large"}},
    {"Rupees", {nullptr, "Item_Wallet_Medium", "Item_Wallet_Large"}},
};

const BagKind* bag_kind_of(const std::string& item) {
    for (const BagKind& k : kBags)
        if (item == k.item) return &k;
    return nullptr;
}
// (a bag's kind and its size, 1..3)
const BagKind* bag_of(const std::string& name, int* level) {
    for (const BagKind& k : kBags)
        for (int i = 0; i < 3; i++)
            if (k.bags[i] && name == k.bags[i]) {
                *level = i + 1;
                return &k;
            }
    return nullptr;
}
// (the biggest bag of a kind this game has: 0 none, -1 the pouch not ready)
int bag_level(Ctx* c, const BagKind& k) {
    for (int i = 2; i >= 0; i--) {
        if (!k.bags[i]) return 1;   // (the first wallet)
        const int n = pouch_count(c, k.bags[i]);
        if (n < 0) return -1;
        if (n > 0) return i + 1;
    }
    return 0;
}

// ---- rupees: the pouch's own (OneshotIncreaseRupee, f_82982fc0): the count f_80a87c30(pouch, 1), the wallet's size
// f_80a876a0(pouch), a change f_833c6d70(pouch, delta, 7); the pouch *(*0x84e98e28)
constexpr uint64_t kPouchSlot = 0x84e98e28ULL, kFnRupees = 0x80a87c30ULL, kFnRupeeMax = 0x80a876a0ULL, kFnRupeeAdd = 0x833c6d70ULL;
int rupees(Ctx* c) {
    const uint64_t pouch = r64(kPouchSlot);
    if (pouch < kHeapBase || !rt().a64_lookup(kFnRupees)) return -1;
    return (int)(int32_t)guest_call(c, kFnRupees, pouch, 1);
}
int rupees_change(Ctx* c, int delta) {   // (what really moved: the wallet holds what it can)
    const uint64_t pouch = r64(kPouchSlot);
    const int had = rupees(c);
    if (had < 0 || !rt().a64_lookup(kFnRupeeAdd) || !rt().a64_lookup(kFnRupeeMax)) return 0;
    const int max = (int)(int32_t)guest_call(c, kFnRupeeMax, pouch);
    const int to = std::clamp(had + delta, 0, std::max(max, had));
    if (to == had) return 0;
    guest_call(c, kFnRupeeAdd, pouch, (uint64_t)(int64_t)(to - had), 7);
    return rupees(c) - had;
}
}  // namespace

int item_count(Ctx* c, const char* name) {
    if (!strcmp(name, "Rupees")) return rupees(c);
    return pouch_count(c, name);
}

int item_change(Ctx* c, const char* name, int delta) {
    if (!strcmp(name, "Rupees")) return rupees_change(c, delta);
    return pouch_change(c, name, delta);
}

namespace {
// ---- the items the session moves into or out of this game: a gift's add or removal, the host's starting gear and bags.
// The pouch takes a change at its next update, in play and in the pause menu alike; what really moved is read back (the
// game may hold fewer: a bag full). A Deku Stick (and its kind) going to a game without its bag brings the bag first.
std::string item_label(const std::string& pouch);

struct Moving {
    uint64_t key;
    std::string item;
    bool add;
    int had, n;
    int64_t until;
};
std::vector<Moving> g_moving;

bool item_action(const GameAction& a) {
    return a.kind == GameAction::AddItem || a.kind == GameAction::RemoveItem || a.kind == GameAction::Starter;
}

void item_actions(Ctx* c, int64_t now) {
    Session& ses = Session::get();
    {
        GameAction a;
        while (ses.pop_action(&a))
            if (a.kind != GameAction::StartGame) game_queue_action(a);
    }
    std::deque<GameAction> mine;
    {
        std::lock_guard<std::mutex> lock(g_actions_mutex);
        std::deque<GameAction> rest;
        for (const GameAction& a : g_actions) (item_action(a) ? mine : rest).push_back(a);
        g_actions.swap(rest);
    }
    std::string starter_got;
    for (GameAction a : mine) {
        const bool add = a.kind != GameAction::RemoveItem;
        // (its bag first, when this game has none: the action again at the next update, the bag taken by then)
        if (add) {
            int want_level = 0;
            const BagKind* kind = bag_of(a.s, &want_level);
            if (kind) {   // (a bag from the host: when this game's is smaller)
                const int level = bag_level(c, *kind);
                if (level < 0) {
                    game_queue_action(a);
                    continue;
                }
                if (level < want_level) {
                    const int status = pouch_change(c, a.s.c_str(), 1);
                    log("bags: %s from the host (this game's was size %d; event status %d)", a.s.c_str(), level, status);
                    if (status >= 0) starter_got += (starter_got.empty() ? "" : ", ") + std::string(a.s.find("Wallet") != std::string::npos ? "a bigger wallet" : "a bag");
                }
                continue;
            }
            if (const BagKind* need = bag_kind_of(a.s); need && need->bags[0]) {
                const int level = bag_level(c, *need);
                if (level == 0 && a.pos[2] < 40) {
                    if (a.pos[2] == 0) log("bags: %s needs %s first: given", a.s.c_str(), need->bags[0]), pouch_change(c, need->bags[0], 1);
                    a.pos[2] += 1;
                    game_queue_action(a);
                    continue;
                }
            }
        }
        const int have = item_count(c, a.s.c_str());
        if (have < 0) {   // (the pouch not ready yet)
            if (a.pos[1] < 600) a.pos[1] += 1, game_queue_action(a);
            continue;
        }
        if (a.kind == GameAction::Starter) {
            const int status = have == 0 ? pouch_change(c, a.s.c_str(), a.n) : -1;
            log("starter: %s x%d (had %d, event status %d)", a.s.c_str(), a.n, have, status);
            if (status >= 0) {
                g_moving.push_back({0, a.s, true, 0, a.n, now + 1200000000LL});
                starter_got += (starter_got.empty() ? "" : ", ") + item_label(a.s);
            }
            continue;
        }
        int status = -1;
        if (a.n > 0 && (add || have >= a.n)) {
            if (a.s == "Rupees") status = 0, rupees_change(c, add ? a.n : -a.n);
            else status = pouch_change(c, a.s.c_str(), add ? a.n : -a.n);
        }
        log("gift: %s %s %d (had %d, event status %d)", add ? "add" : "remove", a.s.c_str(), a.n, have, status);
        if (status < 0) {
            if (a.key) ses.gift_result((uint32_t)a.key, false, 0);
            continue;
        }
        g_moving.push_back({a.key, a.s, add, have, a.n, now + 1200000000LL});
    }
    for (auto it = g_moving.begin(); it != g_moving.end();) {
        const int count = item_count(c, it->item.c_str());
        const int moved = count < 0 ? 0 : std::max(0, std::min(it->n, it->add ? count - it->had : it->had - count));
        if (moved < it->n && now < it->until) {
            ++it;
            continue;
        }
        log("gift: %s %s: %d of %d (now %d)", it->add ? "added" : "removed", it->item.c_str(), moved, it->n, count);
        if (it->key) ses.gift_result((uint32_t)it->key, moved > 0, moved);
        it = g_moving.erase(it);
    }
    if (!starter_got.empty()) ses.toast("From the host: " + starter_got, 1);
}

// the starting gear the host shares with players who join (the game's own first weapons and items), with the bags the
// host has (always: without them nothing of their kind can be held): the host's game says what it has every 2 s; a
// player's game takes each one it has none of (a bag: when its own is smaller), again whenever the host's list changes
const char* const kStarterGear[] = {"Weapon_Sword_Kokiri", "Weapon_Shield_Deku", "Weapon_Range_SlingShot",
                                    "Item_DekuStick",      "Item_DekuNut",       "Item_DekuSeed"};

void starter_tick(Ctx* c, int64_t now, const SessionView& v) {
    static int64_t next = 0;
    if (now < next || v.role != Role::Host) return;
    next = now + 2000000000LL;
    std::vector<std::pair<std::string, int>> have;
    for (const BagKind& k : kBags) {
        const int level = bag_level(c, k);
        if (level > 0 && k.bags[level - 1]) have.push_back({k.bags[level - 1], 1});
    }
    if (v.host.starter)
        for (const char* item : kStarterGear) {
            const int n = pouch_count(c, item);
            if (n > 0) have.push_back({item, n});
        }
    Session::get().set_starter(have);
}

std::string item_label(const std::string& pouch) {
    for (const Item& it : giftable_items())
        if (pouch == it.pouch) return it.label;
    return pouch;
}

// the gifts' prompts (offered: Accept / Decline; accepted: How many?), six times a second: from Link's update, and from the
// pause menu's while the game is paused (the items themselves move when the game goes on)
void gifts_tick(int64_t now, int me) {
    static std::atomic<int64_t> next{0};
    int64_t due = next.load();
    if (now < due || !next.compare_exchange_strong(due, now + 150000000LL)) return;
    dialogs_gifts_update(Session::get().gifts(), me);
}

int destination_of(const char* map, bool child) {
    if (!rt().hos_oot_destination) return -1;
    for (int i = 0; i < 64; i++) {
        const char *cm = nullptr, *am = nullptr;
        if (!rt().hos_oot_destination(i, &cm, &am)) break;
        const char* m = child ? cm : am;
        if (m && !strcmp(m, map)) return i;
    }
    return -1;
}
}  // namespace

// ActorMgr::requestCreateActor (f_80392a70), as the pouch player's creation calls it (f_80c38510): the manager, the name (a
// cell whose first word points at it), the create arg (+0 the position, +0x0c the rotation 3 x 3, +0x30 the scale, +0x40 an
// empty name, +0x48 0x10000), the link the actor's handle goes to, three zeros, a delegate (vtable 0x84bfecb0), and on the
// stack a zero byte and the result's int. The actor is made later (a request): its handle shows in the link.
struct SpawnMem {
    uint64_t base = 0;   // name cell +0, name text +0x10 (64), arg +0x60 (0xa0), link +0x100 (0x40), delegate +0x140 (0x40), result +0x180
};
uint64_t spawn_actor(Ctx* c, const char* name, const float pos[3], float yaw, int variant) {
    const uint64_t mgr = r64(0x84eb1888ULL);
    A64Fn fn = rt().a64_lookup(0x80392a70ULL);
    if (!mgr || !fn || strlen(name) >= 63) return 0;
    const uint64_t m = guest_alloc(0x200, 16);
    if (!m) return 0;
    poke(m + 0x10, name, strlen(name) + 1);
    w64(m, m + 0x10);
    w32(m + 8, 64);
    const uint64_t arg = m + 0x60;
    for (int i = 0; i < 3; i++) wf(arg + 4 * (uint64_t)i, pos[i]);
    const float cy = cosf(yaw), sy = sinf(yaw);
    const float rot[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
    for (int i = 0; i < 9; i++) wf(arg + 0x0c + 4 * (uint64_t)i, rot[i]);
    for (int i = 0; i < 3; i++) wf(arg + 0x30 + 4 * (uint64_t)i, 1.0f);
    // (as the scripts' create node f_832dca10 asks it: +0x40 an empty name, +0x48 0, +0x98 1; x4 1, no delegate)
    w64(arg + 0x40, 0x83f04f8cULL);
    w32(arg + 0x48, (variant & 16) ? 0x10001 : (variant & 1) ? 0x10000 : 0);
    w8(arg + 0x98, (variant & 2) ? 0 : 1);
    if (variant & 4) w64(m + 0x140, 0x84bfecb0ULL);   // (the pouch player's delegate)
    Ctx saved;
    memcpy(&saved, c, sizeof saved);
    const uint64_t sp = (c->x[31] - 0x100) & ~0xFULL;
    w64(sp, 0);              // (the 9th argument: a zero byte)
    w64(sp + 8, m + 0x180);  // (the 10th: the result's int)
    w64(sp + 0x10, 0);       // (the 11th)
    c->x[31] = sp;
    c->x[0] = mgr, c->x[1] = m, c->x[2] = arg, c->x[3] = m + 0x100;
    c->x[4] = (variant & 8) ? 0 : 1, c->x[5] = 0, c->x[6] = 0, c->x[7] = (variant & 4) ? m + 0x140 : 0;
    c->x[30] = 0;
    fn(c);
    const uint64_t r = c->x[0];
    memcpy(c, &saved, sizeof saved);
    log("spawn: %s at %.2f %.2f %.2f -> %#llx (link at %#llx)", name, pos[0], pos[1], pos[2], (unsigned long long)r,
        (unsigned long long)(m + 0x100));
    return m + 0x100;
}

// ---- the save: the game's own autosave, asked as its scripts ask it (OneShotAutoSaveMgrRequestAutoSave, f_80bc76b0), and
// over when a save file of this player's is written after the request. The game keeps a rolling list of saves (slot_NN
// folders): its autosave is written as the newest of them, the one Continue and the Load screen's first row load.
namespace {
int64_t g_save_filetime = 0;   // (the wall clock at the request, as a FILETIME)

int64_t filetime_now() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((int64_t)ft.dwHighDateTime << 32) | (int64_t)ft.dwLowDateTime;
}

// The request: the main actor's autosave component (f_80bc7880(*0x84eb7648, 0xc): null off that actor's thread), then
// f_80bc7730(component, reason, 0), which sets the flag the component's update carries out a third of a second later.
// f_80bc72d0(component, reason, 0, 0) says whether the game saves now (not in an event, not while a save is being written);
// reason 0 does not wait for the autosave's own cooldown. While the game is paused the request waits: the pause menu is
// closed for it (game_menu_update). 1 asked, 0 not now, -1 no save manager
int save_request(Ctx* c) {
    constexpr uint64_t kFnComponent = 0x80bc7880ULL, kFnCanSave = 0x80bc72d0ULL, kFnRequest = 0x80bc7730ULL;
    const uint64_t main_actor = r64(0x84eb7648ULL);
    if (!rt().a64_lookup(kFnComponent) || !rt().a64_lookup(kFnCanSave) || !rt().a64_lookup(kFnRequest)) return -1;
    if (main_actor < kHeapBase || !guest_ok(main_actor, 0x498) || !r64(main_actor + 0x430) || !r64(main_actor + 0x1f0)) return 0;
    const uint64_t component = guest_call(c, kFnComponent, main_actor, 0xc);
    if (component < kHeapBase || !guest_ok(component, 0x160) || !r64(component + 0x130)) return 0;
    if (!(guest_call(c, kFnCanSave, component, 0, 0, 0) & 1)) return 0;
    guest_call(c, kFnRequest, component, 0, 0);
    return 1;
}
}  // namespace

void game_save_stop() { g_save_stopped = true; }

void game_save_now(const char* why, bool quiet) {
    if (g_save_stopped.load() || (!game_in_play() && !pause_menu_open())) return;   // (closing, or no game in play)
    int expected = kSaveIdle;
    if (!g_save.compare_exchange_strong(expected, kSaveAsked)) {
        expected = kSaveDone;
        if (!g_save.compare_exchange_strong(expected, kSaveAsked)) {
            expected = kSaveFailed;
            if (!g_save.compare_exchange_strong(expected, kSaveAsked)) return;   // (one already on its way)
        }
    }
    snprintf(g_save_why, sizeof g_save_why, "%s", why);
    g_save_quiet = quiet;
    g_save_tries = 0;
    g_save_at = now_ns();
    log("save: asked (%s)", why);
}

int game_save_state() {
    // (a save nothing carries on any more, Link not updated: the game went back to the title screen: over)
    int st = g_save.load();
    if ((st == kSaveAsked || st == kSaveRunning) && now_ns() - g_save_at.load() > 30000000000LL) {
        if (g_save.compare_exchange_strong(st, kSaveFailed)) log("save: abandoned (the game is not in play)");
        return kSaveFailed;
    }
    return st;
}

namespace {
void save_update(Ctx* c, int64_t now, bool loading) {
    const int st = g_save.load();
    static int64_t hold_until = 0;   // (4 s after this player was down: the give-up's trip to the entrance under way)
    if (fellow_downed()) hold_until = now + 4000000000LL;
    if (st == kSaveAsked) {
        // (this player down: Link's life is held at none until a revive or the give-up; a save now would keep that life and
        // the game would load into its Game Over: the save waits, and a while after, for the give-up's map change to start
        // (a save asked as it starts is not written))
        if (fellow_downed() || now < hold_until || (g_save_quiet.load() && pause_menu_open())) {
            g_save_at = now;
            return;
        }
        // (asked again at every update until the game takes it: a map loading, an event playing, a save being written)
        const int64_t stamp = filetime_now();
        const int asked = loading ? 0 : save_request(c);
        if (asked > 0) {
            g_save_filetime = stamp;
            g_save_at = now;
            g_save = kSaveRunning;
            log("save: requested (%s)", g_save_why);
        } else if (asked < 0 || now - g_save_at.load() > 20000000000LL) {
            g_save = kSaveFailed;
            log("save: not done: %s", asked < 0 ? "the game's save manager was not found" : "the game kept refusing for 20 s");
            Session::get().toast("The game could not be saved", 2);
        }
    } else if (st == kSaveRunning) {
        static int64_t next_check = 0;
        if (now < next_check) return;
        next_check = now + 150000000LL;
        if (newest_save_time() > g_save_filetime) {
            g_save = kSaveDone;
            log("save: written (%s)", g_save_why);
            if (!g_save_quiet.load()) Session::get().toast("Game saved", 1);
        } else if (now - g_save_at.load() > 6000000000LL && g_save_tries.load() < 2) {
            // (the game took the request but wrote nothing: one taken as a map change starts (the give-up's trip to the
            // entrance) is dropped; asked again)
            g_save_tries++;
            g_save_at = now;
            g_save = kSaveAsked;
            log("save: nothing written in 6 s: asked again (%s)", g_save_why);
        } else if (now - g_save_at.load() > 15000000000LL) {
            g_save = kSaveFailed;
            log("save: no save file was written in 15 s");
            Session::get().toast("The game could not be saved", 2);
        }
    }
}

// (paused: the game's save waits for the game to go on, so the pause menu is closed as its Back does, until it is)
void save_unpause(int64_t now) {
    const int st = g_save.load();
    if (st != kSaveAsked && st != kSaveRunning) return;
    static std::atomic<int64_t> next{0};
    int64_t due = next.load();
    if (now < due || !next.compare_exchange_strong(due, now + 400000000LL)) return;
    if (g_save_quiet.load()) return;   // (an autosave waits for the menu to be closed by the player: it is not shut on them)
    dialogs_close();
    menus_close_pause();
}
}  // namespace

void game_warp_player(Ctx* c, const float to[3]) {
    const LocalSnapshot s = local_snapshot();
    if (!s.valid || r64(s.mover) != 0x84b45e18ULL) return;
    move_player(c, r64(s.mover + 624), to);
}


// A ray through the game's physics world, as its scripts cast one (QueryPhysicsRayCastEntity, exec f_8094ab70): its
// parameters through pointers (+0x28 start, +0x30 end, +0x38 check self, +0x40 the layer, +0x48 the sub layer, +0x50 and
// +0x58 the hit masks (indices into the physics' tables), +0x60 a flag), its results the same way (+0x68 hit, +0x70 the
// place hit, +0x78 the normal, +0x80 the distance, +0x88 the material), the node's own result at *(+0). 1 hit, 0 clear,
// -1 not cast
int game_raycast(Ctx* c, const float from[3], const float to[3], int layer, int sublayer, int mask, int submask, float* dist) {
    constexpr uint64_t kFnRayCast = 0x8094ab70ULL;
    if (!rt().a64_lookup(kFnRayCast) || !r64(0x84ea7ff0ULL)) return -1;
    Ctx saved;
    memcpy(&saved, c, sizeof saved);
    const uint64_t ctx = (c->x[31] - 0x400) & ~0xFULL;   // (ctx 0x100, then the values)
    memset(gp(ctx), 0, 0x200);
    const uint64_t v = ctx + 0x100;
    for (int i = 0; i < 3; i++) wf(v + 4 * (uint64_t)i, from[i]), wf(v + 0x10 + 4 * (uint64_t)i, to[i]);
    w32(v + 0x20, 0);   // check self
    w32(v + 0x24, (uint32_t)layer), w32(v + 0x28, (uint32_t)sublayer), w32(v + 0x2c, (uint32_t)mask), w32(v + 0x30, (uint32_t)submask);
    w32(v + 0x34, 0);   // (the flag)
    w64(ctx + 0x00, v + 0x80);   // (the node's result)
    w64(ctx + 0x28, v), w64(ctx + 0x30, v + 0x10), w64(ctx + 0x38, v + 0x20), w64(ctx + 0x40, v + 0x24), w64(ctx + 0x48, v + 0x28);
    w64(ctx + 0x50, v + 0x2c), w64(ctx + 0x58, v + 0x30), w64(ctx + 0x60, v + 0x34);
    w64(ctx + 0x68, v + 0x40), w64(ctx + 0x70, v + 0x44), w64(ctx + 0x78, v + 0x50), w64(ctx + 0x80, v + 0x60), w64(ctx + 0x88, v + 0x64);
    c->x[31] = ctx - 0x10;
    c->x[0] = ctx, c->x[30] = 0;
    rt().a64_lookup(kFnRayCast)(c);
    const int hit = r8(v + 0x40) & 1;
    if (dist) *dist = rf(v + 0x60);
    memcpy(c, &saved, sizeof saved);
    return hit;
}

int game_destination_of(const char* map, bool child) { return destination_of(map, child); }

void game_queue_action(const GameAction& a) {
    std::lock_guard<std::mutex> lock(g_actions_mutex);
    g_actions.push_back(a);
}

// ---- kills: every hit on an actor reaches here (runtime.cpp hook_is_player); a hit that ends an enemy's life is sent to the
// other players' games, which find the same enemy and end its life too
namespace {
std::string actor_name(uint64_t actor) {
    // (the actor's resource record at +16, its name inline at +88: host/hos_main.c hos_oot_is_player reads "Player" there)
    const uint64_t res = r64(actor + 16);
    if (!res || !guest_ok(res, 160)) return {};
    std::string s = cstr_at(res + 88, 63);
    for (char ch : s)
        if (ch < 32 || ch > 126) return {};
    return s;
}

}  // namespace

void game_actor_damaged(Ctx* c, uint64_t actor, uint32_t before, uint32_t after, uint64_t life_addr, bool player) {
    (void)c;
    if (debug_on()) {
        static std::atomic<int> n{0};
        if (n++ < 400)
            log("hit: actor %#llx %s life %u -> %u at %#llx%s; words %llx %llx %llx %llx %llx", (unsigned long long)actor,
                actor_name(actor).c_str(), before, after, (unsigned long long)life_addr, player ? " (Link)" : "",
                (unsigned long long)r64(actor), (unsigned long long)r64(actor + 8), (unsigned long long)r64(actor + 16),
                (unsigned long long)r64(actor + 24), (unsigned long long)r64(actor + 32));
    }
    if (!player && after < before && after != 0) enemies_hit(actor, before, after);   // (the same life taken in the others)
    if (!player && after < before) world_object_hit(actor, before, after);   // (grass cut, a switch struck: in the others')
    if (!player && after < before) effects_actor_struck(actor, after);       // (a bomb going off: in the others' worlds)
    if (player || after != 0 || before == 0) return;
    fellow_enemy_died(actor);   // (an enemy down here: down in the others' games too)
}

bool pause_menu_open() { return now_ns() - g_pause_ns.load() < 250000000LL; }
bool coop_online_together() {
    Session& ses = Session::get();
    return ses.active() && ses.others_present();
}
uint64_t pause_menu_screen() { return pause_menu_open() ? g_pause_screen.load() : 0; }

// The item the pause menu points at, on its Items or Equipment page (the pages hang off the screen's +8 object: Items at
// +0x3c8 (vtable 0x84c01538, the item's actor name at *(+0x88)), Equipment at +0x3d0 (0x84c00f88, *(+0x58)); the page in
// front has +0x28 = 2 and +0x2a = 1)
std::string pause_focused_item(bool* equipment) {
    if (equipment) *equipment = false;
    const uint64_t screen = pause_menu_screen();
    if (!screen || r64(screen) != 0x84c0baa8ULL) return {};
    const uint64_t base = r64(screen + 8);
    if (base < kHeapBase) return {};
    auto front = [](uint64_t page, uint64_t vt) { return page >= kHeapBase && r64(page) == vt && r8(page + 0x28) == 2 && r8(page + 0x2a) == 1; };
    const uint64_t items = r64(base + 0x3c8), equip = r64(base + 0x3d0);
    uint64_t name = 0;
    if (front(items, 0x84c01538ULL)) {
        name = r64(items + 0x88);
    } else if (front(equip, 0x84c00f88ULL)) {
        name = r64(equip + 0x58);
        if (equipment) *equipment = true;
    }
    if (name < 0x80000000ULL || !guest_ok(name, 64)) return {};
    std::string n = cstr_at(name, 63);
    for (char ch : n)
        if (!(isalnum((unsigned char)ch) || ch == '_')) return {};
    return n;
}
bool game_in_play() { return now_ns() - g_play_ns.load() < 300000000LL; }

// An AS played on an actor, as ExecuteASPlay's enter does (f_802201d0): the request {actor, the AS's name, slot, rate,
// -1, -1, keep (nothing done when that AS is the one playing), 0, 0, node} to f_802222a0 (its node is only carried)
bool game_play_as(Ctx* c, uint64_t actor, const char* name, float rate, bool keep) {
    static uint64_t req = 0;
    if (!req) req = guest_alloc(0x40, 16);
    const uint64_t s = guest_string(name);
    if (!req || !s || actor < kHeapBase || r64(actor) != 0x84b45e18ULL) return false;
    struct {
        uint64_t actor, name;
        int32_t slot;
        float rate, start, end;
        uint8_t keep, all, flag, pad[5];
        uint64_t node;
    } r = {actor, s, 0, rate, -1.0f, -1.0f, (uint8_t)(keep ? 1 : 0), 0, 0, {0, 0, 0, 0, 0}, 0};
    static_assert(sizeof r == 0x30, "the AS request");
    poke(req, &r, sizeof r);
    return guest_call(c, 0x802222a0ULL, req) & 1;
}

namespace {
std::mutex g_force_mutex;
std::string g_force_name;
int g_force_frames = 0;
}  // namespace
void game_force_as(const std::string& name, int frames) {
    std::lock_guard<std::mutex> lock(g_force_mutex);
    g_force_name = name, g_force_frames = frames;
}

void game_player_update(Ctx* c, uint64_t mover) {
    // (host/hos_main.c hos_oot_lift_player: the mover's vtable, its components, and the player actor 0x1d50 before the
    // movement component)
    if (r64(mover) != 0x84b45e18ULL) return;
    const uint64_t components = r64(mover + 624);
    const uint64_t movement = components ? r64(components + 800) : 0;
    if (movement < kHeapBase + 0x1d50 || r64(movement + 24) != mover || !is_player(movement - 0x1d50)) return;
    // (Link can be updated by different workers on successive frames: one at a time here)
    static std::mutex busy;
    if (!busy.try_lock()) return;
    std::lock_guard<std::mutex> unlock(busy, std::adopt_lock);
    const uint64_t player = movement - 0x1d50;
    const int64_t now = now_ns();
    g_play_ns = now;
    menus_in_play();
    LocalSnapshot s;
    s.valid = true;
    s.player = player;
    s.mover = mover;
    for (int i = 0; i < 3; i++) s.pos[i] = rf(mover + 796 + i * 4);
    s.yaw = atan2f(rf(mover + 816), rf(mover + 840));
    const uint64_t life = r64(player + 7184);
    s.life = life ? (int)r32(life + 8) : -1;
    s.life_max = life ? (int)r32(life + 24) : -1;   // (the life struct: two value objects, current at +8, max at +0x18; 4 a heart)
    s.at_ns = now;
    // (the map and the age: GameData, four times a second)
    static int64_t next_data = 0;
    static char map[48] = {0};
    static bool child = true, loading = false;
    if (now >= next_data) {
        next_data = now + 250000000LL;
        gamedata_string(c, kHashCurrentBanc, map, sizeof map);
        bool v = true;
        if (gamedata_bool(c, kHashIsChildhood, &v)) child = v;
        bool l = false;
        loading = gamedata_bool(c, kHashLoadingBanc, &l) && l;
    }
    s.child = child;
    snprintf(s.map, sizeof s.map, "%s", map);
    {
        std::lock_guard<std::mutex> lock(g_snap_mutex);
        g_snap = s;
    }
    // the camera for the nameplates: the gameplay view's matrix (world to view, 3 x 4 rows)
    if (const uint64_t view = gameplay_view(); view && r64(view) == kLookAtCameraVt) {
        std::lock_guard<std::mutex> lock(g_cam_mutex);
        for (int i = 0; i < 12; i++) g_cam.m[i] = rf(view + 8 + 4 * (uint64_t)i);
        g_cam.valid = true;
        g_cam.at_ns = now;
    }
    // what the others see of this Link
    Session& ses = Session::get();
    if (ses.active()) {
        static float last[3];
        static int64_t last_ns;
        PlayerState st;
        memcpy(st.pos, s.pos, sizeof st.pos);
        st.yaw = s.yaw;
        const float dt = last_ns ? (float)(now - last_ns) / 1e9f : 0;
        const float dx = s.pos[0] - last[0], dz = s.pos[2] - last[2];
        st.speed = dt > 0 ? sqrtf(dx * dx + dz * dz) / dt : 0;
        memcpy(last, s.pos, sizeof last);
        last_ns = now;
        st.life = (int16_t)s.life, st.life_max = (int16_t)s.life_max;
        st.child = child;
        const uint64_t events = singleton(kEventMgrSlot);
        st.flags = kInPlay | (loading ? kLoading : 0) | (events && r64(events + 104) ? kEvent : 0) | (pause_menu_open() ? kPaused : 0) |
                   (fellow_downed() ? kDowned : 0);
        snprintf(st.map, sizeof st.map, "%s", map);
        {
            std::lock_guard<std::mutex> lock(g_cam_mutex);
            st.cam_yaw = g_cam.valid ? atan2f(-g_cam.m[8], -g_cam.m[10]) : s.yaw;   // (the view's forward: -row 2)
        }
        ses.push_state(st);
        // (what he wears, once a second: sent when it changes)
        static int64_t next_gear = 0;
        if (now >= next_gear && ses.others_present()) {
            next_gear = now + 1000000000LL;
            const std::string gear = read_gear(c);
            if (!gear.empty()) ses.push_gear(gear);
        }
    }
    // the session's actions: the items (gifts, the starting gear) are moved at once, here and from the pause menu's update
    // (the pouch takes them while the game is paused too); the rest here
    item_actions(c, now);
    std::deque<GameAction> todo;
    {
        std::lock_guard<std::mutex> lock(g_actions_mutex);
        std::deque<GameAction> items;   // (the items asked again for the next update stay queued)
        for (const GameAction& a : g_actions) (item_action(a) ? items : todo).push_back(a);
        g_actions.swap(items);
    }
    for (const GameAction& a : todo) {
        if (a.kind == GameAction::Revive) fellow_revived(a.n);
        if (a.kind == GameAction::SaveNow) game_save_now("the host saved", true);
        if (a.kind == GameAction::ObjectHit) {   // (a blow on a placed object in another game)
            const size_t cut = a.s.find('\n');
            if (cut != std::string::npos) world_take_object_hit(c, a.s.substr(0, cut), a.s.substr(cut + 1), a.key, a.n);
            continue;
        }
        if (a.kind == GameAction::EnemyHit) {   // (a hit on a shared enemy in another game)
            const size_t cut = a.s.find('\n');
            if (cut != std::string::npos)
                enemies_take_hit(c, a.s.substr(0, cut), a.s.substr(cut + 1), a.pos, a.n, (int)(a.key >> 32), (uint32_t)a.key);
            continue;
        }
    }
    // the session as this update needs it (copied ten times a second, not every frame)
    static SessionView v;
    static int64_t next_view = 0;
    if (now >= next_view) {
        next_view = now + 100000000LL;
        v = ses.view();
        if (v.role == Role::None) g_trip = Trip{}, g_trip_done = false;   // (a new session: a new trip)
    }
    // the save asked for (the window closing, the server closing, a player leaving): done here, on the game's thread
    save_update(c, now, loading);
    // (online: the game saved every 3 minutes too, so a crash or a cut power loses little)
    static int64_t next_autosave = 0;
    if (v.role != Role::None && v.phase == Phase::Playing) {
        if (!next_autosave) next_autosave = now + 180000000000LL;
        if (now >= next_autosave) {
            next_autosave = now + 180000000000LL;
            if (ses.guest_settings().autosave) game_save_now("every 3 minutes", true);
        }
    } else {
        next_autosave = 0;
    }
    // the others' kills, the fall and the revive
    {
        const uint64_t events = singleton(kEventMgrSlot);
        fellow_update(c, player, mover, map, child, loading, events && r64(events + 104), now);
        puppets_update(c, map, loading, now);   // (the other players as the game's own Link)
        enemies_update(c, mover, map, loading, now);   // (enemies near other players: run by one game, copied by the others)
        world_update(c, mover, map, loading, now);     // (the placed objects' state: shared with the others' worlds)
        effects_update(c, mover, map, loading, now);   // (the players' projectiles and effects: in every world)
    }
    // the trip to the host, when the game started (guests) or a guest asked
    if (v.role == Role::Guest && v.phase == Phase::Playing) {
        const Player& host = v.players[0];
        if (g_trip.stage == 0 && host.used && (host.st.flags & kInPlay) && host.st.map[0] && !loading && v.host.start_with_host) {
            if (!g_trip_done) {
                g_trip_done = true;
                snprintf(g_trip.map, sizeof g_trip.map, "%s", host.st.map);
                memcpy(g_trip.pos, host.st.pos, sizeof g_trip.pos);
                g_trip.child = host.st.child;
                g_trip.at = now;
                if (v.host.same_age && (bool)host.st.child != child && rt().hos_oot_age_ask) {
                    g_trip.ticket = rt().hos_oot_age_ask(host.st.child ? 0 : 1);
                    g_trip.stage = 1;
                    ses.toast("Taking the host's age...", 0);
                } else if (strcmp(host.st.map, map) != 0) {
                    const int dst = destination_of(host.st.map, child);
                    if (dst >= 0 && rt().hos_oot_teleport_ask) {
                        g_trip.ticket = rt().hos_oot_teleport_ask(dst);
                        g_trip.stage = 2;
                        ses.toast("Travelling to the host...", 0);
                    } else {
                        ses.toast("Meet the host in " + std::string(host.st.map), 2);
                    }
                } else {
                    g_trip.stage = 4;
                }
            }
        } else if (g_trip.stage == 1 || g_trip.stage == 2) {
            // (asked: the map change runs; once Link is updated again in the host's map, placed beside him)
            if (now - g_trip.at > 1500000000LL && !loading && !strcmp(map, host.st.map)) g_trip.stage = 4, g_trip.at = now;
            else if (g_trip.stage == 1 && now - g_trip.at > 1500000000LL && !loading && (bool)host.st.child == child &&
                     strcmp(map, host.st.map) != 0) {
                const int dst = destination_of(host.st.map, child);
                if (dst >= 0 && rt().hos_oot_teleport_ask) g_trip.ticket = rt().hos_oot_teleport_ask(dst), g_trip.stage = 2, g_trip.at = now;
                else g_trip.stage = 0;
            } else if (now - g_trip.at > 30000000000LL) {
                g_trip.stage = 0;
            }
        }
        if (g_trip.stage == 4 && !loading && now - g_trip.at > 600000000LL) {
            float to[3] = {host.st.pos[0] + 1.2f * cosf(host.st.yaw), host.st.pos[1] + 0.3f, host.st.pos[2] - 1.2f * sinf(host.st.yaw)};
            move_player(c, components, to);
            g_trip.stage = 5;
            ses.toast("You joined " + host.name, 1);
            log("trip: placed beside the host in %s", map);
        }
    }
    // this player's items, for Give To (twice a second while a dialog may want them), and the host's starting gear
    update_counts(c, now);
    starter_tick(c, now, v);
    gifts_tick(now, v.me);
    // (the host's world for guests that ask; a guest's own items, given back and kept)
    static int64_t next_world = 0;
    if (now >= next_world) {
        next_world = now + 250000000LL;
        if (v.role == Role::Host) coopsave_host_tick(true);
    }
    if (v.role == Role::Guest || v.role == Role::None) coopsave_items_tick(c, now);
    // (a guest just arrived in the host's world: full hearts, not the host's)
    if (v.role == Role::Guest && !loading && s.life > 0 && s.life_max > 0 && coopsave_take_arrival()) {
        InterlockedExchange((volatile LONG*)gp(life + 8), s.life_max);
        log("coopsave: arrived in the host's world: life %d -> %d", s.life, s.life_max);
    }
    probe_poll_file();
    probe_run(c, 2);
    probe_pins(2);
    if (debug_on()) {
        std::lock_guard<std::mutex> lock(g_force_mutex);
        if (g_force_frames > 0) {
            g_force_frames--;
            game_play_as(c, mover, g_force_name.c_str(), 1.0f, true);
        }
    }
}

namespace {
// (research: the menu screens updated lately, by their vtable)
struct SeenScreen {
    std::atomic<uint64_t> vt{0}, screen{0};
    std::atomic<int64_t> at{0};
} g_seen[32];
}  // namespace

std::string menu_screens_seen() {
    std::string out;
    const int64_t now = now_ns();
    for (auto& s : g_seen) {
        if (!s.vt.load() || now - s.at.load() > 300000000LL) continue;
        char b[64];
        snprintf(b, sizeof b, "%llx@%llx ", (unsigned long long)s.vt.load(), (unsigned long long)s.screen.load());
        out += b;
    }
    return out;
}

// ---- the area map's player arrow (the Map screen, the pause's Minus): the screen (vtable 0x84c08300) reaches the arrow's
// object (vtable 0x84c01ab0) through one of its members (*(screen + 0x140) + 0xa0 in this build: found once by a search
// two levels deep), whose layout *(+0x20) has its root pane at f_80229650(layout + 0x40): its place on the
// layout (3840 x 2160, the centre at 0, y up) at +0x4c with its scale at +0x28 (the map's: 1 on the Map screen, 0.65 on
// the minimap of play), its place on the map at +0x6c (the capture's centre at 0, x east, y north: units a metre differ
// from map to minimap), its alpha as drawn (0 when no map shows it) at +0x78
namespace {
constexpr uint64_t kMapScreenVt = 0x84c08300ULL, kMapArrowVt = 0x84c01ab0ULL, kFnRootPane = 0x80229650ULL;
std::mutex g_arrow_mutex;
MapArrow g_arrow;

uint64_t arrow_at(uint64_t screen, int at, int at2) {
    const uint64_t m = r64(screen + (uint64_t)at);
    return m >= kHeapBase ? r64(m + (uint64_t)at2) : 0;
}

void map_arrow_update(Ctx* c, uint64_t screen) {
    static int at = -1, at2 = -1;
    static int64_t searched = 0;
    if (at < 0 || r64(arrow_at(screen, at, at2)) != kMapArrowVt) {
        at = -1;
        const int64_t now = now_ns();
        if (now - searched < 2000000000LL) return;   // (searched not more than every 2 s)
        searched = now;
        for (int o = 8; o < 0x2000 && at < 0; o += 8) {
            const uint64_t m = r64(screen + (uint64_t)o);
            if (m < kHeapBase || m >= kHeapBase + 0x100000000ULL) continue;
            for (int o2 = 0; o2 < 0x400; o2 += 8) {
                const uint64_t q = r64(m + (uint64_t)o2);
                if (q >= kHeapBase && r64(q) == kMapArrowVt) {
                    at = o, at2 = o2;
                    log("map: the arrow at *(screen + %#x) + %#x", o, o2);
                    break;
                }
            }
        }
        if (at < 0) return;
    }
    const uint64_t icon = arrow_at(screen, at, at2);
    const uint64_t layout = r64(icon + 0x20);
    if (layout < kHeapBase || !rt().a64_lookup(kFnRootPane)) return;
    const uint64_t pane = guest_call(c, kFnRootPane, layout + 0x40);
    if (pane < kHeapBase) return;
    MapArrow a;
    a.alpha = rf(pane + 0x78);
    a.x = rf(pane + 0x4c), a.y = rf(pane + 0x50);
    a.scale = rf(pane + 0x28);
    a.lx = rf(pane + 0x6c), a.ly = rf(pane + 0x70);
    a.paused = !game_in_play();
    a.shown = a.alpha > 0.01f && std::isfinite(a.x) && std::isfinite(a.y);
    a.at_ns = now_ns();
    std::lock_guard<std::mutex> lock(g_arrow_mutex);
    g_arrow = a;
}

// ---- the world map: the layout Pa_WorldMap_00 (a parts layout, vtable 0x84c41970, of the Map screen: its +0x28) reached
// from the screen through one of its members (*(screen + 0x78) + 0x18 in this build: found once by a search two levels
// deep); its root pane f_80229650(+0x40) is drawn (+0x78, the alpha) only while the world map is up, its scale at +0x28 the
// zoom. Its areas are its child layouts, listed from +0x1c0 (the list's head: the first at +0x8, each child's node at its
// +0x98, the next at node + 8), each with its name at +0x70 (length +0x78) and its pane in the world map's layout at
// +0x1a0: there the area's place on the screen's layout at +0x4c (the centre of its label, kept for fogged areas too)
constexpr uint64_t kLayoutVt = 0x84c41970ULL;
WorldMap g_world;

bool layout_named(uint64_t lay, const char* want, char* out = nullptr, size_t cap = 0) {
    if (lay < kHeapBase || lay >= kHeapBase + 0x100000000ULL || r64(lay) != kLayoutVt) return false;
    const uint64_t p = r64(lay + 0x70), n = r64(lay + 0x78);
    if (n == 0 || n > 47 || !guest_ok(p, n)) return false;
    char s[48];
    peek(p, s, n);
    s[n] = 0;
    if (out) snprintf(out, cap, "%s", s);
    return !want || !strcmp(s, want);
}

void world_map_update(Ctx* c, uint64_t screen) {
    static int at = -1, at2 = -1;
    static int64_t searched = 0;
    auto is_world = [&](uint64_t g) { return layout_named(g, "Pa_WorldMap_00") && r64(g + 0x28) == screen; };
    if (at < 0 || !is_world(arrow_at(screen, at, at2))) {
        at = -1;
        const int64_t now = now_ns();
        if (now - searched < 2000000000LL) return;
        searched = now;
        for (int o = 8; o < 0x800 && at < 0; o += 8) {
            const uint64_t m = r64(screen + (uint64_t)o);
            if (m < kHeapBase || m >= kHeapBase + 0x100000000ULL) continue;
            for (int o2 = 0; o2 < 0x400; o2 += 8)
                if (is_world(r64(m + (uint64_t)o2))) {
                    at = o, at2 = o2;
                    log("map: the world map at *(screen + %#x) + %#x", o, o2);
                    break;
                }
        }
        if (at < 0) return;
    }
    const uint64_t group = arrow_at(screen, at, at2);
    const uint64_t root = guest_call(c, kFnRootPane, group + 0x40);
    if (root < kHeapBase) return;
    WorldMap w;
    w.alpha = rf(root + 0x78);
    w.scale = rf(root + 0x28);
    w.shown = w.alpha > 0.01f && w.scale > 0.01f && std::isfinite(w.scale);
    if (w.shown) {
        const uint64_t head = group + 0x1c0;
        uint64_t node = r64(head + 8);
        for (int n = 0; n < 64 && node != head && w.count < (int)(sizeof w.areas / sizeof w.areas[0]); n++) {
            const uint64_t lay = node - 0x98;
            char name[48];
            if (!layout_named(lay, nullptr, name, sizeof name)) break;
            node = r64(node + 8);
            const uint64_t pane = r64(lay + 0x1a0);
            if (pane < kHeapBase || strncmp(name, "Pa_", 3) != 0) continue;
            const float x = rf(pane + 0x4c), y = rf(pane + 0x50);
            if (!std::isfinite(x) || !std::isfinite(y)) continue;
            WorldMap::Area& a = w.areas[w.count++];
            snprintf(a.name, sizeof a.name, "%s", name + 3);
            if (char* end = strstr(a.name, "_00")) *end = 0;
            a.x = x, a.y = y;
        }
    }
    w.at_ns = now_ns();
    std::lock_guard<std::mutex> lock(g_arrow_mutex);
    g_world = w;
}
}  // namespace

WorldMap world_map() {
    std::lock_guard<std::mutex> lock(g_arrow_mutex);
    return g_world;
}

const char* world_area_of(const char* map) {
    static const struct {
        const char *prefix, *area;
    } kAreas[] = {
        {"KokiriForest", "KokiriForest"}, {"KakarikoVillage", "Village"},  {"LonLonRanch", "LonLon"},
        {"Market", "Market"},             {"GoronCity", "GoronCity"},      {"ZoraDomain", "ZoraDomain"},
        {"ZoraRiver", "River"},           {"ZoraFountain", "Fountain"},    {"GerudoFortress", "Fortress"},
        {"GerudoValley", "Valley"},       {"LostWood", "LostWood"},        {"SacredForestMeadow", "Meadow"},
        {"HyruleField", "Field"},         {"HyruleCastle", "Castle"},      {"GanonCastle", "Castle"},
        {"Graveyard", "Grave"},           {"DeathMountainTrail", "Trail"}, {"DeathMountainCrater", "Crater"},
        {"LakeHylia", "Lake"},            {"HountedWasteland", "Desert"},  {"HauntedWasteland", "Desert"},
        {"DesertColossus", "Colossus"},   {"InsideTheDekuTree", "DekuTree"}, {"DodongoCavern", "Dodongo"},
        {"InsideJabuJabuBelly", "Jabujabu"}, {"ForestTemple", "Forest"},   {"FireTemple", "Fire"},
        {"WaterTemple", "Water"},         {"ShadowTemple", "Shadow"},      {"SpiritTemple", "Spirit"},
        {"BottomOfTheWell", "Well"},      {"IceCavern", "IceCavern"},
    };
    if (!map || !map[0]) return nullptr;
    for (const auto& a : kAreas)
        if (!strncmp(map, a.prefix, strlen(a.prefix))) return a.area;
    return nullptr;
}

// (Game/MapMenuInfo's BancName with Game/MapCaptureInfo's CameraPos and Height, entry by entry; IsInvisibleMapIcon: none)
bool map_capture(const char* map, float* cx, float* cz, float* width) {
    struct Entry {
        const char* banc;
        float cx, cz, width;
        bool no_icon;
    };
    static const Entry kMaps[] = {
        {"KokiriForest_Child", -5.0f, -12.0f, 300.0f, false},
        {"KokiriForest_Adult", -5.0f, -12.0f, 300.0f, false},
        {"KakarikoVillage_Child", -50.0f, 42.0f, 246.0f, false},
        {"KakarikoVillage_Adult", -50.0f, 42.0f, 246.0f, false},
        {"KakarikoVillage_Adult_Alternative", -50.0f, 42.0f, 246.0f, false},
        {"LonLonRanch_Child", -86.0f, 239.0f, 276.0f, false},
        {"LonLonRanch_Adult", -86.0f, 239.0f, 276.0f, false},
        {"Market_Child", 0.0f, 3.0f, 214.0f, false},
        {"Market_Adult", 0.0f, 3.0f, 214.0f, false},
        {"GoronCity_Child", -5.0f, 0.0f, 156.0f, false},
        {"GoronCity_Adult", -5.0f, 0.0f, 156.0f, false},
        {"ZoraDomain_Child", 30.0f, -14.0f, 151.0f, false},
        {"ZoraDomain_Adult", 30.0f, -14.0f, 151.0f, false},
        {"ZoraDomain_Adult_Alternative", 30.0f, -14.0f, 151.0f, false},
        {"GerudoFortress_Child", 86.0f, -22.0f, 340.0f, false},
        {"GerudoFortress_Adult", 86.0f, -22.0f, 340.0f, false},
        {"LostWood_Child", 6.0f, -1.0f, 400.0f, true},
        {"LostWood_Adult", 6.0f, -1.0f, 400.0f, true},
        {"SacredForestMeadow_Child", -12.0f, -25.0f, 317.0f, false},
        {"SacredForestMeadow_Adult", -12.0f, -25.0f, 317.0f, false},
        {"HyruleField_Child", -23.0f, 197.0f, 736.0f, false},
        {"HyruleField_Adult", -23.0f, 197.0f, 736.0f, false},
        {"HyruleCastle", 0.0f, -80.0f, 242.0f, false},
        {"GanonCastle", 0.0f, -80.0f, 242.0f, false},
        {"Graveyard_Child", 18.0f, 8.0f, 148.0f, false},
        {"Graveyard_Adult", 18.0f, 8.0f, 148.0f, false},
        {"DeathMountainTrail_Child", -21.0f, -36.0f, 390.0f, false},
        {"DeathMountainTrail_Adult", -21.0f, -36.0f, 390.0f, false},
        {"DeathMountainCrater_Child", -12.0f, 12.0f, 188.0f, false},
        {"DeathMountainCrater_Adult", -12.0f, 12.0f, 188.0f, false},
        {"ZoraRiver_Child", 20.0f, -15.0f, 265.0f, false},
        {"ZoraRiver_Adult", 20.0f, -15.0f, 265.0f, false},
        {"ZoraFountain_Child", 30.0f, -13.0f, 195.0f, false},
        {"ZoraFountain_Adult", 30.0f, -13.0f, 195.0f, false},
        {"LakeHylia_Child", -200.0f, 490.0f, 422.0f, false},
        {"LakeHylia_Adult", -200.0f, 490.0f, 422.0f, false},
        {"GerudoValley_Child", 0.0f, -13.0f, 240.0f, false},
        {"GerudoValley_Adult", 0.0f, -13.0f, 240.0f, false},
        {"HountedWasteland_Child", 6.0f, -52.0f, 645.0f, true},
        {"HountedWasteland_Adult", 6.0f, -52.0f, 645.0f, true},
        {"DesertColossus_Child", -88.0f, 12.0f, 305.0f, false},
        {"DesertColossus_Adult", -88.0f, 12.0f, 305.0f, false},
        {"InsideTheDekuTree", -36.0f, 18.0f, 125.0f, false},
        {"DodongoCavern", 31.0f, -30.0f, 256.0f, false},
        {"InsideJabuJabuBelly", -1.0f, -92.0f, 328.0f, false},
        {"ForestTemple", -3.0f, 6.0f, 245.0f, false},
        {"FireTemple", 17.0f, -3.0f, 220.0f, false},
        {"WaterTemple", -24.0f, -45.0f, 263.0f, false},
        {"ShadowTemple", -22.0f, 7.0f, 433.0f, false},
        {"SpiritTemple", 0.0f, -24.0f, 202.0f, false},
        {"BottomOfTheWell", -12.0f, -32.0f, 238.0f, false},
        {"IceCavern", 11.0f, 12.0f, 287.0f, false},
        {"GanonCastle", 61.0f, 297.0f, 268.0f, false},
    };
    for (const Entry& e : kMaps)
        if (!strcmp(e.banc, map)) {
            if (e.no_icon) return false;
            *cx = e.cx, *cz = e.cz, *width = e.width;
            return true;
        }
    return false;
}

MapArrow map_arrow() {
    std::lock_guard<std::mutex> lock(g_arrow_mutex);
    return g_arrow;
}

void game_menu_update(Ctx* c, uint64_t screen) {
    const uint64_t vt = r64(screen);
    if (vt == kMapScreenVt) map_arrow_update(c, screen), world_map_update(c, screen);
    if (debug_on()) {
        int free_at = -1;
        bool found = false;
        for (int i = 0; i < 32 && !found; i++) {
            if (g_seen[i].vt.load() == vt) g_seen[i].screen = screen, g_seen[i].at = now_ns(), found = true;
            else if (!g_seen[i].vt.load() && free_at < 0) free_at = i;
        }
        if (!found && free_at >= 0) g_seen[free_at].vt = vt, g_seen[free_at].screen = screen, g_seen[free_at].at = now_ns();
    }
    if (r64(vt + 46 * 8) == kPauseFocusHandler) {
        const int64_t now = now_ns();
        g_pause_ns = now;
        g_pause_screen = screen;
        // (paused: Link is not updated; Give To's list still needs the pouch)
        // (A on an item: its menu when the player has some of it, else the game's own A)
        std::string ask;
        bool equipment = false;
        if (dialogs_take_item_menu_ask(&ask, &equipment)) {
            const int n = pouch_count(c, ask.c_str());
            if (n > 0) dialogs_open_item_menu(ask, equipment, n);
            else dialogs_give_game_press();
        }
        if (now - g_play_ns.load() > 100000000LL) {
            static int64_t next_world = 0;
            if (now >= next_world) next_world = now + 250000000LL, coopsave_host_tick(true);
            item_actions(c, now);   // (gifts move at once, the menu open or not)
            update_counts(c, now);
            static int64_t next_me = 0;
            static int me = -1;
            if (now >= next_me) next_me = now + 500000000LL, me = Session::get().view().me;
            if (me >= 0) gifts_tick(now, me);
        }
        save_unpause(now);
    }
    title_screen_update(c, screen);
    if (vt == 0x84c106e8ULL) menus_tick(c);
    probe_poll_file();
    probe_run(c, 1);
    probe_pins(1);
}

void game_text_draw(Ctx*, uint64_t) {}

void game_camera_update(Ctx* c, uint64_t camera) {
    // (the frame's bones are animated and their world matrices made: the players' poses go on their Links here)
    pose_frame(c, now_ns());
    probe_pins(3);
    // (oot_fov.c's test: the camera whose view is the gameplay view; its angle in s0 as the projection takes it)
    const uint64_t views = r64(camera + 0x18);
    const uint64_t view = views ? r64(views + 8) : 0;
    if (!view || view != gameplay_view()) return;
    if (debug_on()) probe_run(c, 3);
    float angle;
    memcpy(&angle, &c->vreg[0][0], 4);
    if (!(angle > 0.05f && angle < 3.0f)) return;
    std::lock_guard<std::mutex> lock(g_cam_mutex);
    g_cam.fovy = angle;
    // (the view as this frame draws it: read again here, the lens being built after the camera moved)
    if (r64(view) == kLookAtCameraVt) {
        for (int i = 0; i < 12; i++) g_cam.m[i] = rf(view + 8 + 4 * (uint64_t)i);
        g_cam.valid = true;
        g_cam.at_ns = now_ns();
    }
}

LocalSnapshot local_snapshot() {
    std::lock_guard<std::mutex> lock(g_snap_mutex);
    return g_snap;
}

CameraView camera_view() {
    std::lock_guard<std::mutex> lock(g_cam_mutex);
    return g_cam;
}

}  // namespace coop
