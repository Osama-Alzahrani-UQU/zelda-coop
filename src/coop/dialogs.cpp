#include "dialogs.h"

#include <algorithm>
#include <mutex>

#include "config.h"
#include "coop.h"

namespace coop {

namespace {
std::recursive_mutex g_mutex;
DialogsView g_v;
std::vector<int> g_counts;
std::vector<FriendEntry> g_friends;
std::string g_give_item, g_give_label;
int g_give_have = 0;
std::vector<int> g_player_slots;   // (the Give To players' list: their slots)
std::vector<uint32_t> g_handled;   // (gifts already prompted)
bool g_game_press = false;          // (the item menu's first row: the game's own A, given back once)
std::string g_item_ask;             // (A on an item: its menu asked, for the pause menu's update)
bool g_item_ask_equipment = false;
// (a text the settings want typed)
int g_edit_purpose = 0;
std::string g_edit_title, g_edit_text;
int g_edit_max = 0;
// (the dpad and the stick: a press, then repeats while held)
uint64_t g_held = 0;
int64_t g_next_repeat = 0;

constexpr int kStickDeadzone = 16000;

uint64_t dirs(uint64_t raw, const int32_t s[4]) {
    uint64_t d = raw & (kUp | kDown | kLeft | kRight);
    if (s[1] > kStickDeadzone) d |= kUp;
    if (s[1] < -kStickDeadzone) d |= kDown;
    if (s[0] < -kStickDeadzone) d |= kLeft;
    if (s[0] > kStickDeadzone) d |= kRight;
    return d;
}
// the directions pressed this frame, with repeats while one is held
uint64_t dir_edges(uint64_t raw, const int32_t s[4]) {
    const uint64_t d = dirs(raw, s);
    const int64_t now = now_ns();
    uint64_t out = d & ~g_held;
    if (out) g_next_repeat = now + 350000000LL;
    else if (d && now >= g_next_repeat) out = d, g_next_repeat = now + 90000000LL;
    g_held = d;
    return out;
}

std::string on_off(bool v) { return v ? "On" : "Off"; }

void build_settings_rows() {
    g_v.rows.clear();
    const HostSettings h = config().host;
    const GuestSettings g = config().guest;
    if (g_v.tab == 0) {
        g_v.rows.push_back({"Server name", h.server, "The name your friends see in their list of games.", true});
        g_v.rows.push_back({"Players", std::to_string(h.max_players), "How many players the server takes, you included (2 to 4).", false});
        g_v.rows.push_back({"Password", h.password.empty() ? "None" : std::string(h.password.size(), '*'),
                            "A password your friends must type to join. None: anyone on your network can join.", true});
        g_v.rows.push_back({"Join while playing", on_off(h.join_in_progress), "Friends may join after the game has started.", false});
        g_v.rows.push_back({"Shared kills", on_off(h.share_kills), "An enemy or a boss defeated in one game is defeated in every game.", false});
        g_v.rows.push_back({"Gifts", on_off(h.gifts), "Players may give each other items with Give To.", false});
        g_v.rows.push_back({"Same age as the host", on_off(h.same_age), "Guests play Link at the host's age, child or adult.", false});
        g_v.rows.push_back({"Bring guests to the host", on_off(h.start_with_host), "Guests appear beside the host when the game starts.", false});
        g_v.rows.push_back({"Revive fallen players", on_off(h.revive),
                            "A player who falls waits for the others to revive them, or gives up and goes back to the area's entrance. Off: the game's own Game Over.",
                            false});
        g_v.rows.push_back({"Enemies return when a player joins", on_off(h.respawn_for_new),
                            "On: when a player joins, the defeated enemies come back for everyone (help them or let them fight). Off: they stay defeated.",
                            false});
        g_v.rows.push_back({"Starting gear for new players", on_off(h.starter),
                            "A player who joins gets the starting gear you have (Kokiri Sword, Deku Shield, Fairy Slingshot, Deku Sticks, Nuts and Seeds) when they do not have it yet.",
                            false});
        g_v.rows.push_back({"Port", std::to_string(h.port), "The network port the server listens on (47812 unless it is taken).", true});
    } else {
        static const char* const smooth[] = {"Off", "Light", "Normal", "Strong"};
        g_v.rows.push_back({"Your name", config().name, "The name the other players see over your head and in their lists.", true});
        g_v.rows.push_back({"Nameplates", on_off(g.nameplates), "Names and life over the other players' heads.", false});
        g_v.rows.push_back({"Nameplates through walls", on_off(g.far_names), "The other players' nameplates show from afar and behind walls.", false});
        g_v.rows.push_back({"Players list", on_off(g.hud), "The players' names and hearts at the left, under your hearts.", false});
        g_v.rows.push_back({"Players list size", std::to_string(g.hud_scale) + "%", "The size of the players' list and nameplates.", false});
        g_v.rows.push_back({"Ask before gifts", on_off(g.gift_prompt), "Ask before taking a gift. Off: gifts are taken at once.", false});
        g_v.rows.push_back({"Motion smoothing", smooth[std::clamp(g.smoothing, 0, 3)], "How the other players' movement is smoothed over the network's jitter.", false});
        g_v.rows.push_back({"Notice sounds", on_off(g.sounds), "A sound for joins, invitations and gifts.", false});
        g_v.rows.push_back({"Auto-save", on_off(g.autosave), "Your game saves itself every 3 minutes while you play online, so nothing is lost.", false});
    }
    g_v.sel = std::min<int>(g_v.sel, (int)g_v.rows.size() - 1);
}

void settings_change(int dir, bool press_a) {
    HostSettings& h = config().host;
    GuestSettings& g = config().guest;
    auto request_edit = [](int purpose, const char* title, const std::string& text, int max) {
        g_edit_purpose = purpose, g_edit_title = title, g_edit_text = text, g_edit_max = max;
    };
    if (g_v.tab == 0) {
        switch (g_v.sel) {
        case 0: if (press_a) request_edit(kEditServer, "Server name", h.server, 24); break;
        case 1: h.max_players = std::clamp(h.max_players + (dir ? dir : 1), 2, kMaxPlayers); break;
        case 2: if (press_a) request_edit(kEditPassword, "Server password (empty: none)", h.password, 16); break;
        case 3: h.join_in_progress = !h.join_in_progress; break;
        case 4: h.share_kills = !h.share_kills; break;
        case 5: h.gifts = !h.gifts; break;
        case 6: h.same_age = !h.same_age; break;
        case 7: h.start_with_host = !h.start_with_host; break;
        case 8: h.revive = !h.revive; break;
        case 9: h.respawn_for_new = !h.respawn_for_new; break;
        case 10: h.starter = !h.starter; break;
        case 11: if (press_a) request_edit(kEditServer + 100, "Port (1024 to 65535)", std::to_string(h.port), 5); break;
        }
    } else {
        switch (g_v.sel) {
        case 0: if (press_a) request_edit(kEditName, "Your name", config().name, 16); break;
        case 1: g.nameplates = !g.nameplates; break;
        case 2: g.far_names = !g.far_names; break;
        case 3: g.hud = !g.hud; break;
        case 4: g.hud_scale = std::clamp(g.hud_scale + (dir ? dir : 1) * 10, 60, 160); break;
        case 5: g.gift_prompt = !g.gift_prompt; break;
        case 6: g.smoothing = (g.smoothing + (dir < 0 ? 3 : 1)) % 4; break;
        case 7: g.sounds = !g.sounds; break;
        case 8: g.autosave = !g.autosave; break;
        }
    }
    config_save();
    Session::get().set_host_settings(h);
    Session::get().set_guest_settings(g);
    build_settings_rows();
}

void build_invite_rows() {
    g_friends = Session::get().friends();
    g_v.rows.clear();
    for (const auto& f : g_friends) g_v.rows.push_back({f.name, f.busy ? "In a game" : "Online", "", false});
    if (g_v.rows.empty()) g_v.rows.push_back({"No one found yet", "", "Friends show here while they are in the Online menu on the same Radmin VPN network.", false});
    g_v.sel = std::min<int>(g_v.sel, (int)g_v.rows.size() - 1);
}

void build_give_items() {
    g_v.rows.clear();
    const auto& items = giftable_items();
    for (size_t i = 0; i < items.size(); i++) {
        const int n = i < g_counts.size() ? g_counts[i] : -1;
        if (n <= 0) continue;
        g_v.rows.push_back({items[i].label, items[i].quantity ? "x" + std::to_string(n) : "", items[i].pouch, false});
    }
    if (g_v.rows.empty()) g_v.rows.push_back({"Nothing to give", "", "", false});
    g_v.sel = std::min<int>(g_v.sel, (int)g_v.rows.size() - 1);
}

void build_give_players() {
    const SessionView v = Session::get().view();
    g_v.rows.clear();
    g_player_slots.clear();
    for (int i = 0; i < kMaxPlayers; i++) {
        if (!v.players[i].used || i == v.me) continue;
        g_v.rows.push_back({v.players[i].name, v.players[i].host ? "Host" : "", "", false});
        g_player_slots.push_back(i);
    }
    if (g_v.rows.empty()) g_v.rows.push_back({"No other player", "", "", false});
    g_v.sel = 0;
}

std::vector<ServerEntry> g_join_list;

void build_join_rows() {
    g_join_list = Session::get().servers();
    g_v.rows.clear();
    for (const ServerEntry& s : g_join_list) {
        std::string what = s.host + "  -  " + std::to_string(s.players) + "/" + std::to_string(s.max);
        if (s.locked) what += "  -  password";
        g_v.rows.push_back({s.server, what, s.in_game ? "In game" : "In the lobby", false});
    }
    if (g_join_list.empty()) g_v.rows.push_back({"Looking for games on your network...", "", "", false});
    g_v.sel = std::min<int>(g_v.sel, (int)g_v.rows.size() - 1);
}

void build_ingame_rows() {
    const SessionView v = Session::get().view();
    g_v.rows.clear();
    for (int i = 0; i < kMaxPlayers; i++) {
        if (!v.players[i].used) continue;
        const auto& p = v.players[i];
        std::string where = p.me ? "You" : (p.st.map[0] ? place_name(p.st.map) : "...");
        g_v.rows.push_back({p.name + (p.host ? "  (Host)" : ""), where, p.ping >= 0 && !p.me ? std::to_string(p.ping) + " ms" : "", false});
    }
    g_v.rows.push_back({"Give To...", "", "Give one of your items to another player.", false});
    g_v.rows.push_back({"Online Settings", "", "", false});
    g_v.rows.push_back({v.role == Role::Host ? "Close the session" : "Leave the session", "", "", false});
}
}  // namespace

const std::vector<Item>& giftable_items() {
    static const std::vector<Item> items = {
        {"Deku Sticks", "Item_DekuStick", true},   {"Deku Nuts", "Item_DekuNut", true},
        {"Bombs", "Item_Bomb", true},              {"Bombchus", "Item_Bombchu", true},
        {"Deku Seeds", "Item_DekuSeed", true},     {"Arrows", "Item_Arrow_Normal", true},
        {"Magic Beans", "Item_MagicBean", true},   {"Empty Bottle", "Item_Bottle", true},
        {"Gold Skulltula Tokens", "Item_Token_Gold", true},
        {"Kokiri Sword", "Weapon_Sword_Kokiri", false}, {"Deku Shield", "Weapon_Shield_Deku", false},
        {"Hylian Shield", "Weapon_Shield_Hylian", false}, {"Fairy Slingshot", "Weapon_Range_SlingShot", false},
        {"Fairy Bow", "Weapon_Range_Bow", false},  {"Boomerang", "Item_Boomerang", false},
        {"Hookshot", "Item_Hookshot", false},      {"Megaton Hammer", "Item_Hammer", false},
        {"Lens of Truth", "Item_GlassTruth", false}, {"Goron Tunic", "Armor_Adult_Goron", false},
        {"Zora Tunic", "Armor_Adult_Zora", false}, {"Iron Boots", "Armor_Adult_HeavyBoot", false},
        {"Hover Boots", "Armor_Adult_HoverBoot", false},
    };
    return items;
}

void dialogs_set_counts(const std::vector<int>& counts) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_counts = counts;
    if (g_v.kind == kDlgGiveItem) build_give_items();
}

bool dialogs_open() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_v.kind != kDlgNone;
}

void dialogs_open_settings() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_v = DialogsView{};
    g_v.kind = kDlgSettings;
    g_v.title = "Online Settings";
    build_settings_rows();
}

void dialogs_open_invite() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_v = DialogsView{};
    g_v.kind = kDlgInvite;
    g_v.title = "Invite Friends";
    build_invite_rows();
}

void dialogs_open_ingame() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_v = DialogsView{};
    g_v.kind = kDlgIngame;
    g_v.title = "Online";
    build_ingame_rows();
}

void dialogs_open_join() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_v = DialogsView{};
    g_v.kind = kDlgJoin;
    g_v.title = "Join a game";
    Session::get().searching(true);
    build_join_rows();
}

void dialogs_open_give() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_v = DialogsView{};
    if (!Session::get().view().host.gifts) {
        Session::get().toast("The host turned gifts off", 2);
        return;
    }
    g_v.kind = kDlgGiveItem;
    g_v.title = "Give To: choose an item";
    build_give_items();
}

bool dialogs_is_giftable(const std::string& pouch) {
    for (const auto& it : giftable_items())
        if (pouch == it.pouch) return true;
    return false;
}

void dialogs_ask_item_menu(const std::string& pouch, bool equipment) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_item_ask = pouch;
    g_item_ask_equipment = equipment;
}

bool dialogs_take_item_menu_ask(std::string* pouch, bool* equipment) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_item_ask.empty()) return false;
    *pouch = g_item_ask, *equipment = g_item_ask_equipment;
    g_item_ask.clear();
    return true;
}

void dialogs_give_game_press() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_game_press = true;
}

void dialogs_open_item_menu(const std::string& pouch, bool equipment, int count) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const auto& items = giftable_items();
    size_t i = 0;
    while (i < items.size() && pouch != items[i].pouch) i++;
    if (i == items.size() || g_v.kind != kDlgNone) return;
    g_v = DialogsView{};
    g_v.kind = kDlgItemMenu;
    g_v.title = items[i].label;
    g_give_item = items[i].pouch, g_give_label = items[i].label;
    g_v.rows = {{equipment ? "Equip" : "Select", "", "", false},
                {"Give To...", "", "Give it to another player in the session.", false},
                {"Cancel", "", "", false}};
    g_v.sel = 0;   // (the game's own action first, as its own item menus have it: A twice does what A always did)
    g_give_have = count;
}

bool dialogs_take_game_press() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const bool p = g_game_press;
    g_game_press = false;
    return p;
}

void dialogs_close() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_v = DialogsView{};
}

bool dialogs_take_edit(int* purpose, std::string* title, std::string* text, int* max) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!g_edit_purpose) return false;
    *purpose = g_edit_purpose, *title = g_edit_title, *text = g_edit_text, *max = g_edit_max;
    g_edit_purpose = 0;
    return true;
}

void dialogs_edit_done(int purpose, const std::string& text) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (purpose == kEditServer) config().host.server = text.empty() ? "Hyrule Co-op" : text;
    else if (purpose == kEditPassword) config().host.password = text;
    else if (purpose == kEditServer + 100) {
        const int p = atoi(text.c_str());
        if (p >= 1024 && p <= 65535) config().host.port = (uint16_t)p;
    }
    config_save();
    Session::get().set_host_settings(config().host);
    if (g_v.kind == kDlgSettings) build_settings_rows();
}

void dialogs_gifts_update(const std::vector<Gift>& gifts, int me) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (const Gift& g : gifts) {
        if (std::find(g_handled.begin(), g_handled.end(), g.id) != g_handled.end()) continue;
        if (g.to == me && g.state == kGiftOffered && g_v.kind == kDlgNone) {
            g_handled.push_back(g.id);
            const SessionView v = Session::get().view();
            g_v = DialogsView{};
            g_v.kind = kDlgGiftOffer;
            g_v.gift = g.id;
            g_v.title = "A gift";
            g_v.text = (g.from >= 0 && g.from < kMaxPlayers ? v.players[g.from].name : std::string("A player")) +
                       " wants to give you " + g.label + ".";
            g_v.rows = {{"Accept", "", "", false}, {"Decline", "", "", false}};
        } else if (g.from == me && g.state == kGiftAccepted && (g_v.kind == kDlgNone || g_v.kind == kDlgWaiting)) {
            g_handled.push_back(g.id);
            g_v = DialogsView{};
            g_v.kind = kDlgGiftAmount;
            g_v.gift = g.id;
            g_v.title = "How many?";
            g_v.text = "How many " + g.label + " to give?";
            g_v.amount = 1;
            g_v.amount_max = std::max(1, g.have);
        } else if (g.from == me && (g.state == kGiftDeclined || g.state == kGiftFailed) && g_v.kind == kDlgWaiting && g_v.gift == g.id) {
            g_handled.push_back(g.id);
            g_v = DialogsView{};
        }
    }
}

void dialogs_keyboard_input(TextEdit* e, uint64_t raw, uint64_t pressed, int32_t stick[4], void (*done)(bool ok)) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    // (the keys: 10 a row, 4 rows of letters, then Shift, Space, Delete, OK)
    const uint64_t d = dir_edges(raw, stick);
    const int cols = 10, rows = 5;
    int x = e->cursor_key % cols, y = e->cursor_key / cols;
    if (d & kUp) y = (y + rows - 1) % rows;
    if (d & kDown) y = (y + 1) % rows;
    if (d & kLeft) x = (x + cols - 1) % cols;
    if (d & kRight) x = (x + 1) % cols;
    e->cursor_key = y * cols + x;
    auto type = [&](const char* ch) {
        if ((int)e->text.size() < e->max) e->text += ch;
    };
    if (pressed & kA) {
        static const char* const keys = "1234567890QWERTYUIOPASDFGHJKL-ZXCVBNM_.:";
        if (y < 4) {
            char ch[2] = {keys[y * 10 + x], 0};
            if (e->hint.find("address") == std::string::npos && e->cursor_key >= 10 && ch[0] >= 'A' && ch[0] <= 'Z' &&
                !(e->text.empty()))
                ch[0] = (char)(ch[0] - 'A' + 'a');   // (a capital first, then small letters, as names are written)
            type(ch);
        } else {
            // (the bottom row: 0-2 Shift (spaced), 3-6 Space, 7-8 Delete, 9 OK)
            if (x <= 2) {
                if (!e->text.empty()) {
                    char& last = e->text.back();
                    if (last >= 'a' && last <= 'z') last = (char)(last - 'a' + 'A');
                    else if (last >= 'A' && last <= 'Z') last = (char)(last - 'A' + 'a');
                }
            } else if (x <= 6) {
                type(" ");
            } else if (x <= 8) {
                if (!e->text.empty()) e->text.pop_back();
            } else {
                done(true);
                return;
            }
        }
    }
    if (pressed & kB) {
        if (!e->text.empty()) e->text.pop_back();
        else done(false);
        return;
    }
    if (pressed & kY) type(" ");
    if (pressed & kPlus) done(true);
    else if (pressed & kMinus) done(false);
}

void dialogs_input(uint64_t raw, uint64_t pressed, int32_t stick[4]) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const uint64_t d = dir_edges(raw, stick);
    const int n = (int)g_v.rows.size();
    if (g_v.kind == kDlgGiftAmount) {
        if (d & (kRight | kUp)) g_v.amount = std::min(g_v.amount_max, g_v.amount + 1);
        if (d & (kLeft | kDown)) g_v.amount = std::max(1, g_v.amount - 1);
        if (pressed & kR) g_v.amount = std::min(g_v.amount_max, g_v.amount + 10);
        if (pressed & kL) g_v.amount = std::max(1, g_v.amount - 10);
        if (pressed & kA) {
            Session::get().gift_send(g_v.gift, g_v.amount);
            if (pause_menu_open()) Session::get().toast("The gift goes as soon as you close the menu", 0);
            g_v = DialogsView{};
        } else if (pressed & kB) {
            Session::get().gift_cancel(g_v.gift);
            g_v = DialogsView{};
        }
        return;
    }
    if (n > 0) {
        if (d & kUp) g_v.sel = (g_v.sel + n - 1) % n;
        if (d & kDown) g_v.sel = (g_v.sel + 1) % n;
    }
    switch (g_v.kind) {
    case kDlgSettings:
        if (pressed & (kL | kZL)) g_v.tab = 0, g_v.sel = 0, build_settings_rows();
        if (pressed & (kR | kZR)) g_v.tab = 1, g_v.sel = 0, build_settings_rows();
        if (d & kLeft) settings_change(-1, false);
        if (d & kRight) settings_change(1, false);
        if (pressed & kA) settings_change(0, true);
        if (pressed & (kB | kPlus)) g_v = DialogsView{};
        break;
    case kDlgJoin: {
        // (this game goes on: the session joins it, and the trip to the host and the host's starting gear follow in play)
        static int64_t next;
        if (now_ns() > next) next = now_ns() + 500000000LL, build_join_rows();
        if ((pressed & kA) && g_v.sel < (int)g_join_list.size()) {
            const ServerEntry s = g_join_list[(size_t)g_v.sel];
            if (s.locked) {
                Session::get().toast("This game has a password: join it from the title menu (Online)", 2);
            } else {
                Session::get().searching(false);
                Session::get().join(s.addr, "");
                Session::get().toast("Joining " + s.server + "...", 0);
                g_v = DialogsView{};
            }
        }
        if (pressed & (kB | kMinus)) {
            Session::get().searching(false);
            g_v = DialogsView{};
        }
        break;
    }
    case kDlgInvite: {
        static int64_t next;
        if (now_ns() > next) next = now_ns() + 1000000000LL, build_invite_rows();
        if ((pressed & kA) && g_v.sel < (int)g_friends.size()) Session::get().invite(g_friends[(size_t)g_v.sel]);
        if (pressed & kB) g_v = DialogsView{};
        break;
    }
    case kDlgIngame: {
        static int64_t next;
        if (now_ns() > next) next = now_ns() + 500000000LL, build_ingame_rows();
        if (pressed & kA) {
            const int k = g_v.sel - (n - 3);
            if (k == 0) dialogs_open_give();
            else if (k == 1) dialogs_open_settings();
            else if (k == 2) {
                Session::get().leave();
                g_v = DialogsView{};
            }
        }
        if (pressed & (kB | kMinus)) g_v = DialogsView{};
        break;
    }
    case kDlgGiveItem: {
        const auto& items = giftable_items();
        if ((pressed & kA) && n > 0 && !g_v.rows[(size_t)g_v.sel].help.empty()) {
            const std::string pouch = g_v.rows[(size_t)g_v.sel].help;
            for (size_t i = 0; i < items.size(); i++)
                if (pouch == items[i].pouch) {
                    g_give_item = pouch, g_give_label = items[i].label;
                    g_give_have = i < g_counts.size() ? g_counts[i] : 0;
                }
            g_v.kind = kDlgGivePlayer;
            g_v.title = "Give " + g_give_label + " to";
            build_give_players();
        }
        if (pressed & kB) g_v = DialogsView{};
        break;
    }
    case kDlgItemMenu:
        if (pressed & kA) {
            if (g_v.sel == 0) {   // (the game's own Select / Equip)
                g_v = DialogsView{};
                g_game_press = true;
            } else if (g_v.sel == 1) {
                if (!Session::get().view().host.gifts) {
                    g_v = DialogsView{};
                    Session::get().toast("The host turned gifts off", 2);
                    break;
                }
                if (g_give_have <= 0) {
                    g_v = DialogsView{};
                    Session::get().toast(std::string("You have no ") + g_give_label + " to give", 2);
                    break;
                }
                g_v.kind = kDlgGivePlayer;
                g_v.title = "Give " + g_give_label + " to";
                build_give_players();
            } else {
                g_v = DialogsView{};
            }
        }
        if (pressed & kB) g_v = DialogsView{};
        break;
    case kDlgGivePlayer:
        if ((pressed & kA) && g_v.sel < (int)g_player_slots.size()) {
            const uint32_t id = Session::get().gift_offer(g_player_slots[(size_t)g_v.sel], g_give_item, g_give_label, g_give_have);
            g_v = DialogsView{};
            if (id) {
                g_v.kind = kDlgWaiting;
                g_v.gift = id;
                g_v.title = "Give To";
                g_v.text = "Waiting for an answer...";
            }
        }
        if (pressed & kB) g_v.kind = kDlgGiveItem, g_v.title = "Give To: choose an item", build_give_items();
        break;
    case kDlgGiftOffer:
        if (pressed & kA) {
            Session::get().gift_reply(g_v.gift, g_v.sel == 0);
            g_v = DialogsView{};
        } else if (pressed & kB) {
            Session::get().gift_reply(g_v.gift, false);
            g_v = DialogsView{};
        }
        break;
    case kDlgWaiting:
        if (pressed & kB) {
            Session::get().gift_cancel(g_v.gift);
            g_v = DialogsView{};
        }
        break;
    default:
        break;
    }
}

void dialogs_char(uint32_t) {}

void dialogs_key(int vk) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (vk == VK_ESCAPE && g_v.kind != kDlgNone && g_v.kind != kDlgGiftOffer) g_v = DialogsView{};
}

DialogsView dialogs_view() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_v;
}

}  // namespace coop
