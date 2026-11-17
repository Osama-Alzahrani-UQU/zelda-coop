#include "coopsave.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "coop.h"
#include "dialogs.h"
#include "game.h"
#include "menus.h"
#include "session.h"

namespace coop {

namespace {

std::mutex g_mutex;

// ---- files
bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool read_file(const std::wstring& p, std::vector<uint8_t>* out) {
    out->clear();
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out->insert(out->end(), buf, buf + n);
    fclose(f);
    return !out->empty();
}

bool write_file(const std::wstring& p, const void* data, size_t n) {
    const std::wstring tmp = p + L".coop_tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    const bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    if (!ok || !MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// (a folder and everything in it)
void remove_tree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            const std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p);
            else SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL), DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// (the files of one folder into another, made if needed; their times kept)
bool copy_files(const std::wstring& from, const std::wstring& to) {
    CreateDirectoryW(to.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ok &= CopyFileW((from + L"\\" + fd.cFileName).c_str(), (to + L"\\" + fd.cFileName).c_str(), FALSE) != 0;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

std::wstring slot_name(int slot) {
    wchar_t b[16];
    swprintf(b, 16, L"slot_%02d", slot);
    return b;
}

constexpr int kSlots = 32;

// ---- the guest's own saves, aside while it plays in the host's world: bin\coop\save_backup (with the account folder they
// came from and a mark that the copy is whole)
std::wstring backup_dir() { return mod_dir() + L"\\save_backup" + profile_suffix(); }

bool backup_saves(const std::wstring& account) {
    const std::wstring bak = backup_dir();
    if (exists(bak + L"\\whole.txt")) return true;   // (a copy from before, not put back yet: the player's own; kept)
    remove_tree(bak);
    CreateDirectoryW(mod_dir().c_str(), nullptr);
    if (!CreateDirectoryW(bak.c_str(), nullptr)) return false;
    bool ok = true;
    for (int s = 0; s < kSlots; s++) {
        const std::wstring from = account + L"\\" + slot_name(s);
        if (exists(from)) ok &= copy_files(from, bak + L"\\" + slot_name(s));
    }
    const std::string acc = narrow(account);
    ok = ok && write_file(bak + L"\\account.txt", acc.data(), acc.size());
    if (!ok) {
        remove_tree(bak);
        return false;
    }
    const char mark[] = "Longshot Co-op: this player's own saves, put back when it leaves the host's world\n";
    if (!write_file(bak + L"\\whole.txt", mark, sizeof mark - 1)) {
        remove_tree(bak);
        return false;
    }
    log("coopsave: this player's own saves copied aside (%s)", narrow(bak).c_str());
    return true;
}

// ---- the host: the guests' requests for its save
struct Ask {
    int slot;
    int64_t since;
    int stage;            // (0 new, 1 a fresh save asked)
    int64_t saved_before; // (the newest save's time when the save was asked)
};
std::vector<Ask> g_asks;

std::wstring players_dir() { return mod_dir() + L"\\players" + profile_suffix(); }

std::wstring player_file(const std::string& name) {
    std::wstring w = widen(name);
    for (wchar_t& ch : w)
        if (ch < 32 || wcschr(L"\\/:*?\"<>|.", ch)) ch = L'_';
    if (w.empty()) w = L"player";
    return players_dir() + L"\\" + w + L".txt";
}

bool read_slot(int slot, std::vector<uint8_t>* caption, std::vector<uint8_t>* progress) {
    const std::wstring account = save_account_dir();
    if (account.empty() || slot < 0) return false;
    const std::wstring dir = account + L"\\" + slot_name(slot);
    return read_file(dir + L"\\caption.sav", caption) && read_file(dir + L"\\progress.sav", progress);
}

// (to that guest: the save of that slot (-1: the newest), or none)
void send_save(int to, int slot) {
    std::vector<uint8_t> caption, progress;
    if (slot < 0) {
        const auto saves = list_saves();
        if (!saves.empty()) slot = saves[0].slot;
    }
    if (!read_slot(slot, &caption, &progress)) caption.clear(), progress.clear();
    Session::get().send_world(to, caption, progress);
    // (and the guest's own things, kept from its last visit)
    const SessionView v = Session::get().view();
    if (to > 0 && to < kMaxPlayers && v.players[to].used) {
        std::vector<uint8_t> list;
        if (read_file(player_file(v.players[to].name), &list))
            Session::get().send_items_back(to, std::string(list.begin(), list.end()));
    }
}

// ---- a guest's items: the giftable things and the bags, "name=count" lines
const char* const kBagNames[] = {
    "Item_StickBag_Small",  "Item_StickBag_Medium",  "Item_StickBag_Large",  "Item_NutBag_Small",  "Item_NutBag_Medium",
    "Item_NutBag_Large",    "Item_BulletBag_Small",  "Item_BulletBag_Medium", "Item_BulletBag_Large", "Item_BombBag_Small",
    "Item_BombBag_Medium",  "Item_BombBag_Large",    "Item_Quiver_Small",    "Item_Quiver_Medium",  "Item_Quiver_Large",
    "Item_Wallet_Medium",   "Item_Wallet_Large",
};
bool is_bag(const std::string& name) {
    for (const char* b : kBagNames)
        if (name == b) return true;
    return false;
}

// (this game's things now; empty: the pouch not ready)
std::string items_now(Ctx* c) {
    std::string out;
    for (const char* b : kBagNames) {
        const int n = pouch_count(c, b);
        if (n < 0) return {};
        out += std::string(b) + "=" + std::to_string(n) + "\n";
    }
    for (const Item& it : giftable_items()) {
        const int n = item_count(c, it.pouch);
        if (n < 0) return {};
        out += std::string(it.pouch) + "=" + std::to_string(n) + "\n";
    }
    return out;
}

std::vector<std::pair<std::string, int>> parse_items(const std::string& list) {
    std::vector<std::pair<std::string, int>> out;
    size_t at = 0;
    while (at < list.size()) {
        size_t end = list.find('\n', at);
        if (end == std::string::npos) end = list.size();
        const std::string line = list.substr(at, end - at);
        at = end + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0 || eq > 64) continue;
        out.push_back({line.substr(0, eq), std::max(0, atoi(line.c_str() + eq + 1))});
    }
    return out;
}

struct Back {
    std::vector<std::pair<std::string, int>> items;
    int stage = 0;          // (1 the bags, 2 the rest)
    int64_t next = 0;
} g_back;
bool g_items_settled = false;        // (this guest's things are its own now: sent to the host as they change)
int64_t g_play_since = 0, g_next_send = 0;
std::string g_last_sent;
std::atomic<bool> g_arrival{false};

}  // namespace

int coopsave_install(const std::vector<uint8_t>& caption, const std::vector<uint8_t>& progress) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::wstring account = save_account_dir();
    if (account.empty() || caption.empty() || progress.empty()) return -1;
    if (!backup_saves(account)) {
        log("coopsave: this player's saves could not be copied aside: the host's world is not installed");
        return -1;
    }
    // (into the slot the game lists first now: its newest. The game reads its list of saves when it starts and keeps it,
    // so a slot written now is not moved up that list; the first row's slot is loaded from disk as it is now. It was
    // copied aside above.)
    int slot = -1;
    int64_t newest = INT64_MIN;
    for (int s = 0; s < kSlots; s++) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExW((account + L"\\" + slot_name(s) + L"\\caption.sav").c_str(), GetFileExInfoStandard, &a)) continue;
        const int64_t t = ((int64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
        if (t > newest) newest = t, slot = s;
    }
    if (slot < 0) slot = 0;
    const std::wstring dir = account + L"\\" + slot_name(slot);
    CreateDirectoryW(dir.c_str(), nullptr);
    if (!write_file(dir + L"\\progress.sav", progress.data(), progress.size()) ||
        !write_file(dir + L"\\caption.sav", caption.data(), caption.size())) {
        log("coopsave: the host's world could not be written into %s", narrow(dir).c_str());
        return -1;
    }
    log("coopsave: the host's world (%zu + %zu bytes) in %s", caption.size(), progress.size(), narrow(dir).c_str());
    g_items_settled = false, g_play_since = 0, g_back = Back{};
    g_arrival = true;
    return slot;
}

bool coopsave_take_arrival() { return g_arrival.exchange(false); }

bool coopsave_backup_waiting() { return exists(backup_dir() + L"\\whole.txt"); }

bool coopsave_protect() {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::wstring account = save_account_dir();
    g_items_settled = false, g_play_since = 0, g_back = Back{};
    return account.empty() || backup_saves(account);
}

bool coopsave_restore(const char* why) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::wstring bak = backup_dir();
    if (!exists(bak + L"\\whole.txt")) return false;
    std::vector<uint8_t> acc;
    if (!read_file(bak + L"\\account.txt", &acc)) return false;
    const std::wstring account = widen(std::string(acc.begin(), acc.end()));
    if (account.empty() || !exists(account)) return false;
    for (int s = 0; s < kSlots; s++) {
        const std::wstring mine = bak + L"\\" + slot_name(s), there = account + L"\\" + slot_name(s);
        if (exists(mine)) {
            remove_tree(there);
            if (!copy_files(mine, there)) {
                log("coopsave: slot %d could not be put back; the copy stays in %s", s, narrow(bak).c_str());
                return false;
            }
        } else if (exists(there)) {
            remove_tree(there);   // (a slot the host's world made: not the player's)
        }
    }
    remove_tree(bak);
    log("coopsave: this player's own saves put back (%s)", why);
    return true;
}

void coopsave_host_tick(bool in_play) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Session& ses = Session::get();
    const SessionView v = ses.view();
    if (v.role != Role::Host) {
        g_asks.clear();
        return;
    }
    const int64_t now = now_ns();
    // (the guests' things, kept by name)
    std::string name, list;
    while (ses.take_items_kept(&name, &list)) {
        CreateDirectoryW(mod_dir().c_str(), nullptr);
        CreateDirectoryW(players_dir().c_str(), nullptr);
        write_file(player_file(name), list.data(), list.size());
    }
    int slot = -1;
    while (ses.take_world_request(&slot)) {
        g_asks.erase(std::remove_if(g_asks.begin(), g_asks.end(), [&](const Ask& a) { return a.slot == slot; }), g_asks.end());
        g_asks.push_back({slot, now, 0, 0});
    }
    for (auto it = g_asks.begin(); it != g_asks.end();) {
        Ask& a = *it;
        bool done = false;
        if (a.stage == 0) {
            if (!in_play && v.host_file >= 0) {
                // (the host's game not in play yet, starting from a file: that file, as it is)
                const auto saves = list_saves();
                send_save(a.slot, v.host_file < (int)saves.size() ? saves[(size_t)v.host_file].slot : -1);
                done = true;
            } else if (in_play) {
                // (in play: a fresh save first, for the world as it is now)
                a.saved_before = newest_save_time();
                game_save_now("a friend joining", true);
                a.stage = 1, a.since = now;
            } else if (now - a.since > 90000000000LL) {
                send_save(a.slot, -1);   // (a new game, never in play: whatever there is)
                done = true;
            }
        } else {
            const bool written = newest_save_time() > a.saved_before;
            const int64_t wait = pause_menu_open() ? 6000000000LL : 20000000000LL;
            if (written || game_save_state() == kSaveFailed || now - a.since > wait) {
                send_save(a.slot, -1);
                done = true;
            }
        }
        it = done ? g_asks.erase(it) : it + 1;
    }
}

void coopsave_items_tick(Ctx* c, int64_t now) {
    Session& ses = Session::get();
    const SessionView v = ses.view();
    if (v.role != Role::Guest || v.phase != Phase::Playing) {
        if (v.role == Role::None) g_items_settled = false, g_play_since = 0, g_back = Back{}, g_last_sent.clear();
        return;
    }
    if (!g_play_since) g_play_since = now;
    std::string list;
    if (ses.take_items_back(&list)) {
        g_back.items = parse_items(list);
        g_back.stage = g_back.items.empty() ? 0 : 1;
        g_back.next = now + 1500000000LL;   // (the game settled in first)
    }
    if (g_back.stage && now >= g_back.next) {
        if (pouch_count(c, "Item_DekuStick") < 0 || item_count(c, "Rupees") < 0) {
            g_back.next = now + 300000000LL;
            return;
        }
        if (g_back.stage == 1) {
            // (the bags first: without its bag a game holds none of their kind)
            for (const auto& [item, n] : g_back.items)
                if (is_bag(item) && n > 0 && pouch_count(c, item.c_str()) == 0) pouch_change(c, item.c_str(), 1);
            g_back.stage = 2, g_back.next = now + 700000000LL;
        } else {
            int moved = 0;
            for (const auto& [item, n] : g_back.items) {
                if (is_bag(item)) continue;
                const int have = item_count(c, item.c_str());
                if (have >= 0 && have != n) item_change(c, item.c_str(), n - have), moved++;
            }
            g_back = Back{};
            g_items_settled = true;
            g_next_send = now + 5000000000LL;
            log("coopsave: this player's own items given back (%d changed)", moved);
            if (moved) ses.toast("Your own items are back", 1);
        }
        return;
    }
    // (a first visit: nothing kept for this player: the host's world's things are its own from now on)
    if (!g_items_settled && !g_back.stage && now - g_play_since > 10000000000LL) g_items_settled = true;
    if (g_items_settled && now >= g_next_send) {
        g_next_send = now + 10000000000LL;
        const std::string mine = items_now(c);
        if (!mine.empty() && mine != g_last_sent) {
            ses.send_items(mine);
            g_last_sent = mine;
        }
    }
}

}  // namespace coop
