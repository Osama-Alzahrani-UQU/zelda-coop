#include "menus.h"

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "config.h"
#include "coop.h"
#include "coopsave.h"
#include "dialogs.h"
#include "fellow.h"
#include "session.h"
#include "title.h"

namespace coop {

namespace {

std::mutex g_mutex;
Page g_page = Page::Title;
Page g_after_pick = Page::Online;   // where a file choice leads (the host's lobby, a guest's lobby)
bool g_pick_host = false;
int g_file = -1;
std::string g_file_label;
std::string g_loading_text;
int64_t g_loading_since = 0, g_loading_until = 0;
Page g_loading_next = Page::Online;
TextEdit g_edit;
int g_focus_memory[16] = {0};
std::vector<std::string> g_page_items;
uint64_t g_prev_raw = 0, g_swallow = 0;
uint64_t g_inject = 0;
int g_inject_frames = 0;
std::vector<ServerEntry> g_join_servers;   // the servers shown on the Join page's buttons
int64_t g_join_refresh = 0;
ServerEntry g_pending_server;      // (a locked server, its password being typed)
int64_t g_in_play_ns = 0;

// the automatic load of a file (the host's start, a guest's)
struct AutoLoad {
    int step = 0;
    int row = -1;
    int64_t at = 0;
    int tries = 0;
} g_auto;

// pending decisions from the input (handled at the title's update, on the game's thread)
int g_decide = -1;
bool g_back = false, g_open_online = false;
int g_picked_row = -1;
// (a guest: the host's world on its way, then loaded from this game's own Load screen)
bool g_world_wait = false;
int64_t g_world_since = 0;

void inject(uint64_t b, int frames) {
    g_inject = b;
    g_inject_frames = frames;
}

std::vector<std::string> items_for(Page p, const SessionView& v) {
    switch (p) {
    case Page::Online:
        return {"Host Game", "Join Game", "Online Settings", "Back"};
    case Page::HostStart:
        if (list_saves().empty()) return {"New Game", "Back"};
        return {"Continue", "New Game", "Choose a File", "Back"};
    case Page::Join: {
        std::vector<std::string> items;
        for (const auto& s : g_join_servers) {
            char b[96];
            snprintf(b, sizeof b, "%s%s (%d/%d)", s.invite ? "Invite: " : "", s.server.c_str(), s.players, s.max);
            items.push_back(b);
        }
        if (items.empty()) items.push_back("Searching for games...");
        items.push_back("Join by Address");
        items.push_back("Back");
        return items;
    }
    case Page::HostLobby:
        return {"Start Game", v.server_running ? "Stop Server" : "Start Server", "Invite Friends", "Leave Lobby"};
    case Page::GuestLobby: {
        const bool ready = v.me >= 0 && v.players[v.me].ready;
        return {ready ? "Not Ready" : "Ready", "Leave Lobby"};
    }
    default:
        return {};
    }
}

void go(Page p, int focus = -1) {
    if (g_page == p && focus < 0) return;
    g_focus_memory[(int)g_page] = std::max(0, title::focused());
    g_page = p;
    const SessionView v = Session::get().view();
    g_page_items = items_for(p, v);
    if (p == Page::Title) {
        title::show_page({}, 3);
    } else if (!g_page_items.empty()) {
        title::show_page(g_page_items, focus >= 0 ? focus : 0);
    }
    Session::get().searching(p == Page::Join || p == Page::Online);
    log("menus: page %d", (int)p);
}

void loading(const std::string& text, Page next, int ms) {
    g_loading_text = text;
    g_loading_since = now_ns();
    g_loading_until = now_ns() + (int64_t)ms * 1000000;
    g_loading_next = next;
    g_page = Page::Loading;
    title::show_page({" "}, 0);   // (the menu's buttons out of the way under the loading panel)
}

void open_edit(int purpose, const std::string& title_text, const std::string& text, int max, const std::string& hint) {
    g_edit = TextEdit{};
    g_edit.open = true;
    g_edit.purpose = purpose;
    g_edit.title = title_text;
    g_edit.text = text;
    g_edit.max = max;
    g_edit.hint = hint;
}

void start_pick(bool host, Page after) {
    g_pick_host = host;
    g_after_pick = after;
    g_picked_row = -1;
    g_page = Page::PickFile;
    g_auto = AutoLoad{};
    g_auto.step = 100;   // (opening the Load screen for the player's own choice)
    g_auto.at = now_ns();
    title::show_page({}, 0);
}

void start_autoload(int row) {
    g_page = Page::AutoLoad;
    g_auto = AutoLoad{};
    g_auto.step = 1;
    g_auto.row = row;
    g_auto.at = now_ns();
    title::show_page({}, 0);
    log("menus: loading file row %d", row);
}

// (a new game: the title's own New Game. The game asks first (the autosave is written over): a guest's game answers Start
// itself, its own saves copied aside before; the host answers the game's question)
void start_newgame(bool answer) {
    if (answer && !coopsave_protect()) {
        Session::get().toast("Your saves could not be copied aside: no new game", 2);
        Session::get().leave();
        go(Page::Online, 1);
        return;
    }
    g_page = Page::AutoLoad;
    g_auto = AutoLoad{};
    g_auto.step = 20;
    g_auto.row = answer ? 1 : 0;
    g_auto.at = now_ns();
    title::show_page({}, 1);
    log("menus: starting a new game");
}

// (a guest told to start: the host's world asked for, loaded once it is here)
void start_world_wait() {
    Session::get().request_world();
    g_world_wait = true;
    g_world_since = now_ns();
    g_loading_text = "Getting the host's world...";
    g_loading_since = now_ns();
    g_loading_until = now_ns() + 3600000000000LL;
    g_loading_next = Page::AutoLoad;
    g_page = Page::Loading;
    title::show_page({" "}, 0);
}

std::string file_label(int row);

// (the host: the server up and the game started at once, from the file chosen (-2 a new game); friends join it as it goes)
void host_go(int file) {
    HostSettings hs = config().host;
    hs.join_in_progress = true;
    g_file = file;
    g_file_label = file == -2 ? std::string("New Game") : file_label(file);
    if (!Session::get().host_start(hs, file)) {
        Session::get().toast("The server could not start (the network port is in use?)", 2);
        go(Page::Online, 0);
        return;
    }
    Session::get().start_game();
}

void edit_done(bool ok) {
    TextEdit e = g_edit;
    g_edit.open = false;
    if (!ok) {
        if (e.purpose == kEditName && config().name.empty()) go(Page::Title);
        return;
    }
    std::string t = e.text;
    while (!t.empty() && t.back() == ' ') t.pop_back();
    switch (e.purpose) {
    case kEditName:
        if (t.empty()) t = "Link";
        config().name = t;
        config_save();
        Session::get().set_name(t);
        if (g_page == Page::Name || g_page == Page::Title) go(Page::Online);
        break;
    case kEditAddress: {
        unsigned a = 0, b = 0, c = 0, d = 0, port = net::kPortPlay;
        if (sscanf(t.c_str(), "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) >= 4 && a < 256 && b < 256 && c < 256 && d < 256) {
            config().last_host = t;
            config_save();
            Session::get().join(net::Addr{(a << 24) | (b << 16) | (c << 8) | d, (uint16_t)port}, "");
            loading("Joining " + t + "...", Page::GuestLobby, 600000);
        } else {
            Session::get().toast("That is not an address (example: 26.12.34.56)", 2);
        }
        break;
    }
    case kEditJoinPassword:
        Session::get().join(g_pending_server.addr, t);
        loading("Joining " + g_pending_server.server + "...", Page::GuestLobby, 600000);
        break;
    default:
        dialogs_edit_done(e.purpose, t);
        break;
    }
}

void decide(int index) {
    const SessionView v = Session::get().view();
    switch (g_page) {
    case Page::Online:
        if (index == 0) go(Page::HostStart);
        else if (index == 1) go(Page::Join);
        else if (index == 2) dialogs_open_settings();
        else go(Page::Title);
        break;
    case Page::Join: {
        const int n = (int)g_join_servers.size();
        if (index < n) {
            const ServerEntry s = g_join_servers[(size_t)index];
            if (s.locked) {
                g_pending_server = s;
                open_edit(kEditJoinPassword, "Password for " + s.server, "", 16, "The password the host set");
            } else {
                Session::get().join(s.addr, "");
                loading("Joining " + s.server + "...", Page::GuestLobby, 600000);
            }
        } else if (index == std::max(1, n)) {
            open_edit(kEditAddress, "Host's address", config().last_host, 21, "The host's Radmin VPN address, as 26.12.34.56");
        } else if (index == std::max(1, n) + 1) {
            go(Page::Online, 1);
        }
        break;
    }
    case Page::HostStart: {
        const std::string item = index >= 0 && index < (int)g_page_items.size() ? g_page_items[(size_t)index] : "Back";
        if (item == "Continue") host_go(0);
        else if (item == "New Game") host_go(-2);
        else if (item == "Choose a File") start_pick(true, Page::HostLobby);
        else go(Page::Online, 0);
        break;
    }
    case Page::HostLobby:
        if (index == 0) {
            int others = 0;
            for (int i = 1; i < kMaxPlayers; i++) others += v.players[i].used;
            if (!others) Session::get().toast("Starting alone: friends can still join while you play", 0);
            Session::get().start_game();
        } else if (index == 1) {
            if (v.server_running) Session::get().host_stop();
            else Session::get().host_resume();
            go(Page::HostLobby, 1);
        } else if (index == 2) {
            dialogs_open_invite();
        } else {
            Session::get().leave();
            go(Page::Online, 0);
        }
        break;
    case Page::GuestLobby:
        if (index == 0) {
            const bool ready = v.me >= 0 && v.players[v.me].ready;
            Session::get().set_ready(!ready);
            go(Page::GuestLobby, 0);
        } else {
            Session::get().leave();
            go(Page::Online, 1);
        }
        break;
    default:
        break;
    }
}

void back() {
    switch (g_page) {
    case Page::Online: go(Page::Title); break;
    case Page::HostStart: go(Page::Online, 0); break;
    case Page::Join: go(Page::Online, 1); break;
    case Page::HostLobby:
    case Page::GuestLobby:
        Session::get().leave();
        go(Page::Online, g_page == Page::HostLobby ? 0 : 1);
        break;
    default: break;
    }
}

std::string file_label(int row) {
    const auto saves = list_saves();
    if (row >= 0 && row < (int)saves.size()) return saves[(size_t)row].location + "  (" + saves[(size_t)row].when + ")";
    return "File " + std::to_string(row + 1);
}

}  // namespace

// ---- input (the game's main thread, each controller read)
void menus_input(uint64_t* buttons, int32_t stick[4]) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t raw = *buttons;
    uint64_t pressed = raw & ~g_prev_raw;
    g_prev_raw = raw;
    uint64_t take = 0;   // (bits the game does not get from this press on, until let go)
    {   // (a settings row to type: the keyboard opens for it)
        int purpose = 0, max = 0;
        std::string t, text;
        if (!g_edit.open && dialogs_take_edit(&purpose, &t, &text, &max)) open_edit(purpose, t, text, max, "");
    }
    const bool in_play = now_ns() - g_in_play_ns < 500000000LL || pause_menu_open();   // (paused: Link is not updated)
    if (in_play && !pause_menu_open() && !g_edit.open && !dialogs_open() && !Session::get().active() &&
        (raw & (kLS | kRS)) == (kLS | kRS) && (pressed & (kLS | kRS))) {
        dialogs_open_join(), take |= kLS | kRS;   // (not in a session: the games on the network, to join from here)
    }
    if (in_play && !g_edit.open && !dialogs_open() && Session::get().active()) {
        // (in play: L3 + R3 opens the online panel; L3 in the pause menu, Give To)
        if ((raw & (kLS | kRS)) == (kLS | kRS) && (pressed & (kLS | kRS))) {
            dialogs_open_ingame(), take |= kLS | kRS;
        } else if (pause_menu_open() && (pressed & kA)) {
            // (A on an item of the Items or Equipment page: its menu, with Give To)
            bool equipment = false;
            const std::string item = pause_focused_item(&equipment);
            if (!item.empty() && dialogs_is_giftable(item)) {
                dialogs_ask_item_menu(item, equipment);   // (the pause menu's update reads the count: the menu, or the A back)
                take |= kA, pressed &= ~(uint64_t)kA;
            }
        }
    }
    if (dialogs_take_game_press()) inject(kA, 3);   // (the item menu's Select / Equip: the game's own A)
    if (g_edit.open || dialogs_open()) {
        // (every input is the dialog's)
        if (g_edit.open) {
            dialogs_keyboard_input(&g_edit, raw, pressed, stick, edit_done);
        } else {
            dialogs_input(raw, pressed, stick);
        }
        *buttons = 0;
        for (int i = 0; i < 4; i++) stick[i] = 0;
        g_swallow |= raw;
    } else {
        if (in_play && Session::get().active()) fellow_input(buttons, stick, pressed);   // (down: frozen; A beside a fallen player)
        switch (g_page) {
        case Page::Title:
            if (title::online_focused() && (pressed & kA)) g_open_online = true, take |= kA;
            break;
        case Page::Online:
        case Page::HostStart:
        case Page::Join:
        case Page::HostLobby:
        case Page::GuestLobby:
            if (pressed & kA) g_decide = title::focused();
            if (pressed & kB) g_back = true;
            take |= kA | kB | kX | kY | kPlus | kMinus | kL | kR | kZL | kZR;
            break;
        case Page::PickFile:
            if (loadscreen::open() && (pressed & kA)) g_picked_row = loadscreen::focused_row(), take |= kA;
            break;
        case Page::AutoLoad:
            if (g_auto.step == 25) break;   // (the game's own question: the host answers it)
            [[fallthrough]];
        case Page::Loading:
        case Page::Name:
        case Page::Settings:
            take |= raw;
            *buttons = 0;
            for (int i = 0; i < 4; i++) stick[i] = 0;
            break;
        }
        g_swallow |= take & raw;
    }
    // (a press the menus took stays away from the game until it is let go)
    g_swallow &= raw;
    *buttons &= ~g_swallow;
    if (g_inject_frames > 0) {
        *buttons |= g_inject;
        if (--g_inject_frames == 0) g_inject = 0;
    }
}

// ---- the menus' work on the game's thread (the title screen's update)
void menus_tick(Ctx* c) {
    (void)c;
    std::lock_guard<std::mutex> lock(g_mutex);
    const int64_t now = now_ns();
    const SessionView v = Session::get().view();
    // decisions from the input
    if (g_open_online) {
        g_open_online = false;
        title::sound("mc_Abutton_decide");
        if (config().name.empty()) {
            g_page = Page::Name;
            open_edit(kEditName, "Your name", "", 16, "The name the other players see");
        } else {
            go(Page::Online);
        }
    }
    if (g_decide >= 0) {
        const int d = g_decide;
        g_decide = -1;
        title::sound("mc_Abutton_decide");
        decide(d);
    }
    if (g_back) {
        g_back = false;
        title::sound("mc_ReturnFromDetail");
        back();
    }
    // the Join page's buttons follow the servers found
    if (g_page == Page::Join && now >= g_join_refresh) {
        g_join_refresh = now + 700000000LL;
        auto s = Session::get().servers();
        std::sort(s.begin(), s.end(), [](const ServerEntry& a, const ServerEntry& b) { return a.invite > b.invite; });
        if (s.size() > 2) s.resize(2);
        bool same = s.size() == g_join_servers.size();
        for (size_t i = 0; same && i < s.size(); i++)
            same = s[i].session == g_join_servers[i].session && s[i].players == g_join_servers[i].players && s[i].invite == g_join_servers[i].invite;
        if (!same) {
            g_join_servers = s;
            go(Page::Join, std::max(0, std::min(title::focused(), (int)items_for(Page::Join, v).size() - 1)));
        }
    }
    // the lobby's buttons follow the session (the server's state, ready)
    if (g_page == Page::HostLobby || g_page == Page::GuestLobby) {
        const auto items = items_for(g_page, v);
        if (items != g_page_items) go(g_page, std::max(0, title::focused()));
        if (v.role == Role::None) go(Page::Online, g_page == Page::HostLobby ? 0 : 1);
    }
    // a guest joining: the lobby once the host welcomed it, the file choice first
    if (g_page == Page::Loading && g_loading_next == Page::GuestLobby) {
        if (v.role == Role::Guest && v.phase != Phase::Connecting) {
            go(Page::GuestLobby);
        } else if (v.role == Role::None && now - g_loading_since > 500000000LL) {
            go(Page::Join);
        }
    } else if (g_page == Page::Loading && g_world_wait) {
        // (a guest: the host's world coming; into one of this game's slots once it is all here, then loaded)
        std::vector<uint8_t> caption, progress;
        int percent = 0;
        const int got = Session::get().take_world(&caption, &progress, &percent);
        if (got == 1) {
            g_world_wait = false;
            if (coopsave_install(caption, progress) >= 0) {
                start_autoload(0);   // (written now: the newest, the Load screen's first row)
            } else {
                Session::get().toast("No save folder of your own yet: starting a new game", 0);
                start_newgame(true);
            }
        } else if (got < 0) {
            g_world_wait = false;
            start_newgame(true);   // (the host has no save yet: a new game, as the host's)
        } else if (v.role == Role::None) {
            g_world_wait = false;
            go(Page::Online, 1);
        } else if (now - g_world_since > 120000000000LL) {
            g_world_wait = false;
            Session::get().toast("The host's world did not arrive", 2);
            Session::get().leave();
            go(Page::Online, 1);
        } else {
            char t[96];
            snprintf(t, sizeof t, percent > 0 ? "Getting the host's world... %d%%" : "Getting the host's world...", percent);
            g_loading_text = t;
        }
    } else if (g_page == Page::Loading && now >= g_loading_until) {
        go(g_loading_next);
    }
    // the host started (or the guest was told to): the host's file loads (a new game starts); a guest asks for the host's
    // world, or starts a new game with the host
    GameAction a;
    while (Session::get().pop_action(&a)) {
        if (a.kind == GameAction::StartGame) {
            if (v.role == Role::Guest) {
                if (a.n == -2) start_newgame(true);
                else start_world_wait();
            } else if (g_file == -2) {
                start_newgame(false);
            } else {
                if (g_file < 0) g_file = 0;   // (no file chosen: the newest)
                start_autoload(g_file);
            }
        } else {
            game_queue_action(a);   // (items, kills, the trip to the host: the game's side, in play)
        }
    }
    // the host's world for guests that ask while the host's game is starting
    if (v.role == Role::Host) coopsave_host_tick(false);
    // a guest's own saves, aside while it played in the host's world: back once it is out of the session, at the title
    static int64_t next_restore = 0;
    if (v.role == Role::None && now >= next_restore && g_page != Page::AutoLoad && !g_world_wait) {
        next_restore = now + 2000000000LL;
        if (coopsave_backup_waiting() && coopsave_restore("back at the title"))
            Session::get().toast("Your own saves are back", 1);
    }
    // the file choice: the game's Load screen opened for it, the row taken when the player picks one
    if (g_page == Page::PickFile) {
        if (g_auto.step == 100) {
            if (title::native() && title::focused() == 0 && now - g_auto.at > 250000000LL) {
                inject(kA, 3);
                g_auto.step = 101, g_auto.at = now;
            } else if (title::native() && title::focused() != 0) {
                title::focus(0);
            }
        } else if (g_auto.step == 101) {
            if (loadscreen::open()) g_auto.step = 102;
            else if (now - g_auto.at > 4000000000LL) go(Page::Online);
        } else if (g_auto.step == 102) {
            if (g_picked_row >= 0) {
                g_file = g_picked_row;
                g_file_label = file_label(g_file);
                g_picked_row = -1;
                inject(kB, 3);   // (the Load screen closed as its Back does)
                g_auto.step = 103, g_auto.at = now;
            } else if (!loadscreen::open() && title::active()) {
                // (the player backed out of the Load screen)
                if (g_after_pick == Page::GuestLobby) Session::get().leave();
                if (g_pick_host) go(Page::HostStart, 2);
                else go(Page::Online, 1);
            }
        } else if (g_auto.step == 103) {
            if (!loadscreen::open() && title::active()) {
                if (g_pick_host) {
                    host_go(g_file);
                } else {
                    go(Page::GuestLobby);
                }
            } else if (now - g_auto.at > 3000000000LL) {
                inject(kB, 3);
                g_auto.at = now;
            }
        }
    }
    // the automatic load: Continue, the row, the game's own load
    if (g_page == Page::AutoLoad) {
        switch (g_auto.step) {
        case 1:
            if (title::native()) {
                title::focus(0);
                g_auto.step = 2, g_auto.at = now;
            }
            break;
        case 2:
            if (title::focused() == 0 && now - g_auto.at > 60000000LL) {
                inject(kA, 3);
                g_auto.step = 3, g_auto.at = now;
            } else if (now - g_auto.at > 1500000000LL) {
                title::focus(0);
                g_auto.at = now;
            }
            break;
        case 3:
            if (loadscreen::open()) {
                loadscreen::select_row(g_auto.row);
                g_auto.step = 4, g_auto.at = now;
            } else if (now - g_auto.at > 4000000000LL && g_auto.tries++ < 2) {
                g_auto.step = 1;
            }
            break;
        case 4:
            if (loadscreen::focused_row() == g_auto.row && now - g_auto.at > 90000000LL) {
                inject(kA, 3);
                g_auto.step = 5, g_auto.at = now;
            } else if (now - g_auto.at > 1000000000LL) {
                loadscreen::select_row(g_auto.row);
                g_auto.at = now;
            }
            break;
        case 5:
            // (the game asks to confirm: Load / Cancel, Load focused. Its own dialog, over the list: A again until the load
            // starts, a few times at most)
            if (now - g_auto.at > (g_auto.tries ? 1000000000LL : 600000000LL) && g_auto.tries < 5) {
                g_auto.tries++;
                inject(kA, 3);
                g_auto.at = now;
            }
            break;
        case 20:   // (a new game: the title's New Game focused, then pressed)
            if (title::native()) {
                title::focus(1);
                g_auto.step = 21, g_auto.at = now;
            }
            break;
        case 21:
            if (title::focused() == 1 && now - g_auto.at > 60000000LL) {
                inject(kA, 3);
                g_auto.step = 22, g_auto.at = now, g_auto.tries = 0;
            } else if (now - g_auto.at > 1500000000LL) {
                title::focus(1);
                g_auto.at = now;
            }
            break;
        case 22:
            // (the game's question over the title ("Previous autosave data will be overwritten", Cancel focused): a guest's
            // game moves to Start and takes it; the host answers it)
            if (!title::active()) {
                g_auto.step = 24, g_auto.at = now;
            } else if (!title::menu_ready() && now - g_auto.at > 300000000LL) {
                if (g_auto.row == 1) inject(kUp, 3), g_auto.step = 23;
                else g_auto.step = 25;
                g_auto.at = now;
            } else if (title::menu_ready() && now - g_auto.at > 1500000000LL) {
                g_auto.step = g_auto.tries++ < 3 ? 20 : 24;   // (the press missed: again)
                g_auto.at = now;
            }
            break;
        case 23:
            if (now - g_auto.at > 200000000LL) inject(kA, 3), g_auto.step = 24, g_auto.at = now;
            break;
        case 24:
            // (the game starting: the title gone, the panel off for its opening; else the answer missed: again)
            if (!title::active()) {
                if (now - g_auto.at > 400000000LL) g_page = Page::Title;
            } else if (now - g_auto.at > 1200000000LL && g_auto.tries++ < 3) {
                g_auto.step = title::menu_ready() ? 20 : 22, g_auto.at = now;
            }
            break;
        case 25:
            // (the host's answer: Start, the game starting; Cancel, back at its choice with the server down)
            if (!title::active()) {
                if (now - g_auto.at > 400000000LL) g_page = Page::Title;
            } else if (!title::menu_ready()) {
                g_auto.at = now;
            } else if (now - g_auto.at > 1200000000LL) {
                Session::get().leave();
                go(Page::HostStart, 1);
            }
            break;
        }
    }
}

void menus_in_play() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_in_play_ns = now_ns();
    g_world_wait = false;
    if (g_page == Page::AutoLoad || g_page == Page::Loading || g_page == Page::HostLobby || g_page == Page::GuestLobby ||
        g_page == Page::HostStart) {
        g_page = Page::Title;
        title::show_page({}, 3);
    }
}

void menus_close_pause() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_inject_frames == 0) inject(kB, 3);   // (B, as the player's: one level of the pause menu back at a time)
}

// (probe.cpp, tests) the menus moved without the controller: "host ROW", "join A.B.C.D PORT", "start", "page N"
void menus_probe(const std::string& what, const std::string& a, const std::string& b) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (what == "host") {
        g_file = atoi(a.c_str());
        g_file_label = file_label(g_file);
        Session::get().host_start(config().host, g_file);
        go(Page::HostLobby);
    } else if (what == "join") {
        unsigned x1 = 0, x2 = 0, x3 = 0, x4 = 0;
        sscanf(a.c_str(), "%u.%u.%u.%u", &x1, &x2, &x3, &x4);
        g_file = 0;
        g_file_label = file_label(0);
        Session::get().join(net::Addr{(x1 << 24) | (x2 << 16) | (x3 << 8) | x4, (uint16_t)atoi(b.c_str())}, "");
        go(Page::GuestLobby);
    } else if (what == "start") {
        Session::get().start_game();
    } else if (what == "page") {
        go((Page)atoi(a.c_str()));
    }
}

Page menus_page() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_page;
}

MenuView menus_view() {
    std::lock_guard<std::mutex> lock(g_mutex);
    MenuView v;
    v.page = g_page;
    v.file = g_file;
    v.file_label = g_file_label;
    v.loading_text = g_loading_text;
    v.loading_since = g_loading_since;
    v.name_editing = g_edit.open;
    // (the game's own question, the host's to answer; a new game's opening, the title gone: no panel over them)
    v.hide_loading = g_page == Page::AutoLoad && (g_auto.step == 25 || (g_auto.step >= 22 && !title::active()));
    return v;
}

bool menus_modal() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_edit.open || dialogs_open();
}

TextEdit menus_text_edit() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_edit;
}

void menus_text_char(uint32_t ch) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_edit.open) {
        dialogs_char(ch);
        return;
    }
    if (ch < 32 || ch == 127) return;
    if ((int)g_edit.text.size() >= g_edit.max) return;
    if (ch < 128) g_edit.text.push_back((char)ch);
    else if (ch < 0x800) g_edit.text += (char)(0xC0 | (ch >> 6)), g_edit.text += (char)(0x80 | (ch & 0x3F));
    else g_edit.text += (char)(0xE0 | (ch >> 12)), g_edit.text += (char)(0x80 | ((ch >> 6) & 0x3F)), g_edit.text += (char)(0x80 | (ch & 0x3F));
}

void menus_text_key(int vk) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_edit.open) {
        dialogs_key(vk);
        return;
    }
    if (vk == VK_BACK) {
        while (!g_edit.text.empty() && ((unsigned char)g_edit.text.back() & 0xC0) == 0x80) g_edit.text.pop_back();
        if (!g_edit.text.empty()) g_edit.text.pop_back();
    } else if (vk == VK_RETURN) {
        edit_done(true);
    } else if (vk == VK_ESCAPE) {
        edit_done(false);
    }
}

namespace {
constexpr int kSaveSlots = 32;   // (slot_00 and on: the game keeps a rolling list of saves, each in its own folder)
}  // namespace

// the player's save folder: the --user folder when the game was given one, else the game folder's, then save and the account
std::wstring save_account_dir() {
    std::wstring user = game_dir() + L"\\user";
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i + 1 < argc; i++)
            if (!wcscmp(argv[i], L"--user")) user = argv[i + 1];
        LocalFree(argv);
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((user + L"\\save\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring account;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') account = user + L"\\save\\" + fd.cFileName;
    } while (account.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return account;
}

int64_t newest_save_time() {
    const std::wstring dir = save_account_dir();
    if (dir.empty()) return 0;
    int64_t newest = 0;
    for (int slot = 0; slot < kSaveSlots; slot++) {
        wchar_t p[32];
        swprintf(p, 32, L"\\slot_%02d\\caption.sav", slot);
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExW((dir + p).c_str(), GetFileExInfoStandard, &a)) continue;
        newest = std::max(newest, ((int64_t)a.ftLastWriteTime.dwHighDateTime << 32) | (int64_t)a.ftLastWriteTime.dwLowDateTime);
    }
    return newest;
}

std::vector<SaveInfo> list_saves() {
    std::vector<SaveInfo> out;
    const std::wstring account = save_account_dir();
    if (account.empty()) return out;
    for (int slot = 0; slot < kSaveSlots; slot++) {
        wchar_t p[32];
        swprintf(p, 32, L"\\slot_%02d\\caption.sav", slot);
        const std::wstring path = account + p;
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) continue;
        SaveInfo s;
        s.slot = slot;
        s.mtime = ((int64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
        FILETIME local;
        SYSTEMTIME st;
        FileTimeToLocalFileTime(&a.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &st);
        char when[48];
        snprintf(when, sizeof when, "%d/%d %d:%02d", st.wMonth, st.wDay, st.wHour, st.wMinute);
        s.when = when;
        // (the caption's location and map: the first readable names in it, "KokiriForest" and "KokiriForest/KokiriForest_Child")
        if (FILE* f = _wfopen(path.c_str(), L"rb")) {
            char buf[4096];
            const size_t n = fread(buf, 1, sizeof buf, f);
            fclose(f);
            std::string cur, first, second;
            for (size_t i = 0; i < n; i++) {
                const char ch = buf[i];
                if (ch >= 32 && ch < 127) cur.push_back(ch);
                else {
                    if (cur.size() >= 6) {
                        if (first.empty()) first = cur;
                        else if (second.empty()) { second = cur; break; }
                    }
                    cur.clear();
                }
            }
            // (the location's name spaced: KokiriForest -> Kokiri Forest; a stray prefix byte trimmed)
            size_t start = 0;
            while (start < first.size() && !(first[start] >= 'A' && first[start] <= 'Z' && start + 1 < first.size() &&
                                             first[start + 1] >= 'a' && first[start + 1] <= 'z'))
                start++;
            if (start >= first.size()) start = 0;
            std::string loc;
            for (size_t i = start; i < first.size(); i++) {
                if (i > start && first[i] >= 'A' && first[i] <= 'Z' && first[i - 1] >= 'a' && first[i - 1] <= 'z') loc.push_back(' ');
                loc.push_back(first[i]);
            }
            s.map = second;
            // (the map's path names the place best: "KokiriForest/KokiriForest_Child")
            const size_t slash = second.find('/');
            if (slash != std::string::npos && slash > 2) {
                loc.clear();
                const std::string place = second.substr(0, slash);
                for (size_t i = 0; i < place.size(); i++) {
                    if (i > 0 && place[i] >= 'A' && place[i] <= 'Z' && place[i - 1] >= 'a' && place[i - 1] <= 'z') loc.push_back(' ');
                    loc.push_back(place[i]);
                }
            }
            s.location = loc.empty() ? "File " + std::to_string(slot + 1) : loc;
        }
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const SaveInfo& a, const SaveInfo& b) { return a.mtime > b.mtime; });
    return out;
}

}  // namespace coop
