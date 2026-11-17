// Developer probe: guest calls and memory from a text file, run on the game's thread (research and tests only).
// Off unless bin\coop\probe_on exists. Commands are written to bin\coop\probe.txt; each line runs once at the next game
// tick (a menu screen's update or Link's) and the file is emptied. Results go to coop_log.txt:
//   call FN [A0 [A1 ...]]          x0 = FN(A0..A5); also s0
//   calls FN A0 "text" [A2 ...]    the same with A1 a guest copy of the text (UTF-8, NUL ended)
//   read ADDR N                    N bytes at ADDR, as hex
//   write ADDR HEX                 bytes written at ADDR
//   q ADDR N                       N qwords at ADDR
//   pin ADDR HEX FRAMES WHERE      bytes written at ADDR each frame, from a hook (m menu, p Link, c camera lens, f mover)
//   mirror 1|0                     this Link's own pose shown on the other players' Links (pose.cpp without the network)
//   asforce NAME FRAMES            an AS asked for on Link at each of his updates
//   fall                           Link down, as a blow that would end him makes him (a session with others)
//   goto MAP                       the runtime's debug teleport to that map
//   save                           the game's own save asked (as the mod's autosave)
//   ehit GAMEACTOR N               a hit of N on that enemy, shared as the life hook does
//   "@m ", "@p ", "@c ", "@f "     a line run only at that hook
#include <atomic>
#include <cctype>
#include <cmath>
#include <functional>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "coop.h"
#include "menus.h"
#include "effects.h"
#include "enemies.h"
#include "fellow.h"
#include "game.h"
#include "pose.h"
#include "runtime.h"

namespace coop {

namespace {
std::mutex g_mutex;
std::vector<std::string> g_lines;
bool g_on = false;
uint64_t g_scratch = 0;
// (pin: bytes written at an address each frame for a while, from one of the hooks)
struct Pin {
    uint64_t addr = 0;
    std::vector<uint8_t> bytes;
    int where = 0, frames = 0;
};
std::vector<Pin> g_pins;
std::atomic<int> g_pin_count{0};

int where_of(char w) { return w == 'm' ? 1 : w == 'p' ? 2 : w == 'c' ? 3 : w == 'f' ? 4 : 0; }

uint64_t num(const std::string& s) { return strtoull(s.c_str(), nullptr, 0); }

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') i++;
        if (i >= line.size()) break;
        if (line[i] == '"') {
            size_t e = line.find('"', i + 1);
            if (e == std::string::npos) e = line.size();
            out.push_back("\"" + line.substr(i + 1, e - i - 1));
            i = e + 1;
        } else {
            size_t e = line.find(' ', i);
            if (e == std::string::npos) e = line.size();
            out.push_back(line.substr(i, e - i));
            i = e;
        }
    }
    return out;
}

void run_line(Ctx* c, const std::string& line) {
    auto t = split(line);
    if (t.empty()) return;
    if (!g_scratch) g_scratch = guest_alloc(0x4000, 16);
    if (t[0] == "call" || t[0] == "calls") {
        uint64_t a[6] = {0, 0, 0, 0, 0, 0};
        size_t str_at = 0;
        for (size_t i = 2; i < t.size() && i < 8; i++) {
            const bool quoted = !t[i].empty() && t[i][0] == '"';
            const bool prefixed = t[i].size() > 2 && t[i][0] == 's' && t[i][1] == ':';
            if (quoted || prefixed) {
                const std::string text = t[i].substr(quoted ? 1 : 2);
                const uint64_t at = g_scratch + 0x100 * (str_at++ % 8);
                poke(at, text.c_str(), text.size() + 1);
                a[i - 2] = at;
            } else {
                a[i - 2] = num(t[i]);
            }
        }
        float s0 = 0;
        const uint64_t r = guest_call(c, num(t[1]), a[0], a[1], a[2], a[3], a[4], a[5], &s0);
        log("probe: %s -> x0 %#llx (s0 %g)", line.c_str(), (unsigned long long)r, s0);
    } else if (t[0] == "read" && t.size() >= 3) {
        const uint64_t a = num(t[1]), n = num(t[2]);
        std::string hex;
        for (uint64_t i = 0; i < n && i < 512; i++) {
            char b[4];
            snprintf(b, sizeof b, "%02x", r8(a + i));
            hex += b;
            if (i % 16 == 15) hex += ' ';
        }
        log("probe: read %#llx: %s", (unsigned long long)a, hex.c_str());
    } else if (t[0] == "q" && t.size() >= 3) {
        const uint64_t a = num(t[1]), n = num(t[2]);
        std::string s;
        for (uint64_t i = 0; i < n && i < 64; i++) {
            char b[24];
            snprintf(b, sizeof b, "%llx ", (unsigned long long)r64(a + 8 * i));
            s += b;
        }
        log("probe: q %#llx: %s", (unsigned long long)a, s.c_str());
    } else if (t[0] == "spawn" && t.size() >= 2) {
        const LocalSnapshot s = local_snapshot();
        const float d = t.size() > 2 ? (float)atof(t[2].c_str()) : 2.0f;
        const float pos[3] = {s.pos[0] + d * sinf(s.yaw), s.pos[1], s.pos[2] + d * cosf(s.yaw)};
        const uint64_t link = spawn_actor(c, t[1].c_str(), pos, s.yaw + 3.14159f, t.size() > 3 ? atoi(t[3].c_str()) : 0);
        log("probe: spawn %s link %#llx", t[1].c_str(), (unsigned long long)link);
    } else if (t[0] == "warp" && t.size() >= 4) {
        const float to[3] = {(float)atof(t[1].c_str()), (float)atof(t[2].c_str()), (float)atof(t[3].c_str())};
        game_warp_player(c, to);
        log("probe: warp %.2f %.2f %.2f", to[0], to[1], to[2]);
    } else if (t[0] == "strscan" && t.size() >= 2) {
        // strscan ADDR [PREFIX] [DEPTH]: identifier strings reachable from ADDR through pointers (0x800 bytes an object)
        const uint64_t root = num(t[1]);
        const std::string prefix = t.size() > 2 ? t[2] : "";
        const int depth = t.size() > 3 ? atoi(t[3].c_str()) : 3;
        std::vector<uint64_t> seen;
        int found = 0;
        auto ident = [&](uint64_t p) -> std::string {
            if (p < 0x80000000ULL || p >= 0x2000000000ULL) return {};
            std::string s = cstr_at(p, 64);
            const bool any = !prefix.empty() && prefix[0] == '*';   // (*text: anywhere in it)
            if (s.size() < 4 || (any ? s.find(prefix.substr(1)) == std::string::npos : s.compare(0, prefix.size(), prefix) != 0)) return {};
            for (char ch : s)
                if (!(isalnum((unsigned char)ch) || ch == '_')) return {};
            return s;
        };
        std::function<void(uint64_t, int, const std::string&)> walk = [&](uint64_t obj, int d, const std::string& path) {
            if (found > 300 || seen.size() > 40000) return;
            for (uint64_t s : seen)
                if (s == obj) return;
            seen.push_back(obj);
            for (uint64_t off = 0; off < 0x800; off += 8) {
                const uint64_t v = r64(obj + off);
                char step[24];
                snprintf(step, sizeof step, "+%llx", (unsigned long long)off);
                const std::string here = path + step;
                const std::string s = ident(v);
                if (!s.empty()) {
                    log("probe: strscan %s -> \"%s\"", here.c_str(), s.c_str());
                    found++;
                } else if (d > 1 && v >= kHeapBase && v < kHeapBase + 0x100000000ULL) {
                    walk(v, d - 1, here + ">");
                }
            }
        };
        walk(root, depth, "");
        log("probe: strscan done (%d found, %zu objects)", found, seen.size());
    } else if (t[0] == "ray" && t.size() >= 11) {
        // ray X0 Y0 Z0 X1 Y1 Z1 LAYER SUB MASK SUBMASK
        float a[3], b[3];
        for (int i = 0; i < 3; i++) a[i] = (float)atof(t[1 + i].c_str()), b[i] = (float)atof(t[4 + i].c_str());
        float dist = -1;
        const int hit = game_raycast(c, a, b, atoi(t[7].c_str()), atoi(t[8].c_str()), atoi(t[9].c_str()), atoi(t[10].c_str()), &dist);
        log("probe: ray hit %d dist %.3f", hit, dist);
    } else if (t[0] == "screens") {
        log("probe: screens %s", menu_screens_seen().c_str());
    } else if (t[0] == "pause") {
        log("probe: pause screen %#llx", (unsigned long long)pause_menu_screen());
    } else if (t[0] == "pouch" && t.size() >= 2) {
        // pouch NAME [DELTA]: a pouch content's count, and an add or a removal
        const int before = pouch_count(c, t[1].c_str());
        const int status = t.size() > 2 ? pouch_change(c, t[1].c_str(), atoi(t[2].c_str())) : -9;
        log("probe: pouch %s %d -> %d (status %d)", t[1].c_str(), before, pouch_count(c, t[1].c_str()), status);
    } else if (t[0] == "me") {
        const LocalSnapshot s = local_snapshot();
        const uint64_t life = r64(s.player + 7184);
        std::string q;
        for (int i = 0; i < 8; i++) {
            char b[24];
            snprintf(b, sizeof b, "%x ", r32(life + 4 * (uint64_t)i));
            q += b;
        }
        log("probe: me player %#llx mover %#llx pos %.2f %.2f %.2f yaw %.2f life %d/%d map %s child %d; life words %s",
            (unsigned long long)s.player, (unsigned long long)s.mover, s.pos[0], s.pos[1], s.pos[2], s.yaw, s.life, s.life_max, s.map,
            s.child, q.c_str());
    } else if (t[0] == "menu" && t.size() >= 2) {
        menus_probe(t[1], t.size() > 2 ? t[2] : "", t.size() > 3 ? t[3] : "");
        log("probe: menu %s", t[1].c_str());
    } else if (t[0] == "str" && t.size() >= 2) {
        // (a lasting guest copy of the text: its address)
        const std::string text = t[1].size() > 2 && t[1][1] == ':' ? t[1].substr(2) : t[1];
        const uint64_t at = guest_alloc(text.size() + 16, 16);
        poke(at, text.c_str(), text.size() + 1);
        log("probe: str %#llx \"%s\"", (unsigned long long)at, text.c_str());
    } else if (t[0] == "write" && t.size() >= 3) {
        const uint64_t a = num(t[1]);
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i + 1 < t[2].size(); i += 2) bytes.push_back((uint8_t)strtoul(t[2].substr(i, 2).c_str(), nullptr, 16));
        poke(a, bytes.data(), bytes.size());
        log("probe: wrote %zu bytes at %#llx", bytes.size(), (unsigned long long)a);
    } else if (t[0] == "dests") {
        // dests: the runtime's debug teleport destinations (child map, adult map)
        std::string all;
        for (int i = 0; i < 64 && rt().hos_oot_destination; i++) {
            const char *cm = nullptr, *am = nullptr;
            if (!rt().hos_oot_destination(i, &cm, &am)) break;
            all += std::to_string(i) + ":" + (cm ? cm : "-") + "/" + (am ? am : "-") + " ";
        }
        log("probe: dests %s", all.c_str());
    } else if (t[0] == "goto" && t.size() >= 2) {
        // goto MAP: the runtime's debug teleport to that map's destination (as the trip to the host takes)
        const LocalSnapshot s = local_snapshot();
        const int dst = game_destination_of(t[1].c_str(), s.child);
        const int ticket = dst >= 0 && rt().hos_oot_teleport_ask ? rt().hos_oot_teleport_ask(dst) : -1;
        log("probe: goto %s: destination %d, ticket %d", t[1].c_str(), dst, ticket);
    } else if (t[0] == "save") {
        // save: the game's own save asked, as the mod's autosave asks it
        game_save_now("the probe", false);
        log("probe: save asked");
    } else if (t[0] == "ehit" && t.size() >= 3) {
        // ehit GAMEACTOR N: a hit of N on that enemy, taken and shared as the life hook does (enemies.cpp)
        enemies_hit_test(strtoull(t[1].c_str(), nullptr, 16), atoi(t[2].c_str()));
        log("probe: ehit %s %s", t[1].c_str(), t[2].c_str());
    } else if (t[0] == "ekill" && t.size() >= 2) {
        // ekill GAMEACTOR: that enemy brought down by a blow of the game's own (a life message), as another game's kill is
        log("probe: ekill %s %s", t[1].c_str(), fellow_kill_actor(c, strtoull(t[1].c_str(), nullptr, 16)) ? "sent" : "not taken");
    } else if (t[0] == "spawnfx" && t.size() >= 2) {
        // spawnfx NAME [UP]: that actor made (and put in the world) 2 m in front of Link, UP metres up; its fate logged
        effects_test_spawn(t[1], t.size() > 2 ? (float)atof(t[2].c_str()) : 1.0f);
        log("probe: spawnfx %s", t[1].c_str());
    } else if (t[0] == "edel" && t.size() >= 2) {
        // edel GAMEACTOR: that actor deleted as the scripts' OneShotGenericDeleteThisActor does (f_800c6490(actor, 0))
        const uint64_t ga = strtoull(t[1].c_str(), nullptr, 16);
        log("probe: edel %s -> %llu", t[1].c_str(), (unsigned long long)(guest_call(c, 0x800c6490ULL, ga, 0) & 1));
    } else if (t[0] == "asact" && t.size() >= 3) {
        // asact GAMEACTOR NAME: that animation (AS) played on that actor, as a door's for another player
        log("probe: asact %s %s -> %d", t[1].c_str(), t[2].c_str(),
            (int)game_play_as(c, strtoull(t[1].c_str(), nullptr, 16), t[2].c_str(), 1.0f, false));
    } else if (t[0] == "xlink" && t.size() >= 3) {
        // xlink GAMEACTOR KEY [MODE]: an effect key played on that actor as the scripts do (its effect part f_804d8220(actor),
        // then f_83bf69e0(&part, &key, 0, mode))
        static uint64_t buf = 0;
        if (!buf) buf = guest_alloc(0x80, 16);
        const uint64_t ga = strtoull(t[1].c_str(), nullptr, 16);
        const uint64_t part = buf ? guest_call(c, 0x804d8220ULL, ga) : 0;
        if (part && t[2].size() < 60) {
            w64(buf, part);
            poke(buf + 0x20, t[2].c_str(), t[2].size() + 1);
            w64(buf + 8, buf + 0x20);
            const uint64_t r = guest_call(c, 0x83bf69e0ULL, buf, buf + 8, 0, t.size() > 3 ? strtoull(t[3].c_str(), nullptr, 10) : 0);
            log("probe: xlink %s %s -> part %#llx, %#llx", t[1].c_str(), t[2].c_str(), (unsigned long long)part, (unsigned long long)r);
        } else {
            log("probe: xlink %s: no effect part", t[1].c_str());
        }
    } else if (t[0] == "stickfire" && t.size() >= 2) {
        // stickfire SECONDS: this player's Deku Stick as lit (its fire shown on it in the others' worlds)
        effects_test_stick(atoi(t[1].c_str()));
        log("probe: stickfire %s", t[1].c_str());
    } else if (t[0] == "eblow" && t.size() >= 3) {
        // eblow GAMEACTOR N: a blow of N on that enemy as the game's own (seen by the life hook: shared as a player's hit is)
        log("probe: eblow %s %s %s", t[1].c_str(), t[2].c_str(),
            fellow_life_message(c, strtoull(t[1].c_str(), nullptr, 16), -atoi(t[2].c_str())) ? "sent" : "not taken");
    } else if (t[0] == "hurt" && t.size() >= 2) {
        // hurt SLOT: that player's nameplate shows a hit, as when their life goes down
        overlay_hit_test(atoi(t[1].c_str()));
        log("probe: hurt %s", t[1].c_str());
    } else if (t[0] == "fall") {
        // fall: Link down as a blow that would end him makes him (the life guard's own way in)
        log("probe: fall %s", fellow_guard_life(4, 0) ? "down" : "not taken (alone, or revive off)");
    } else if (t[0] == "asforce" && t.size() >= 3) {
        // asforce NAME FRAMES: that AS asked for on Link at each of his updates for a while
        game_force_as(t[1], atoi(t[2].c_str()));
        log("probe: asforce %s %s", t[1].c_str(), t[2].c_str());
    } else if (t[0] == "mirror" && t.size() >= 2) {
        // mirror 1|0: this Link's own pose shown on the other players' Links (the pose's whole path but the network)
        pose_mirror(atoi(t[1].c_str()) != 0);
        log("probe: mirror %s", t[1].c_str());
    } else if (t[0] == "pin" && t.size() >= 5) {
        // pin ADDR HEX FRAMES WHERE: the bytes written at ADDR each frame for FRAMES frames, from the hook WHERE (m a menu
        // screen's update, p Link's update, c a camera's lens, f a mover's matrix)
        Pin p;
        p.addr = num(t[1]);
        for (size_t i = 0; i + 1 < t[2].size(); i += 2) p.bytes.push_back((uint8_t)strtoul(t[2].substr(i, 2).c_str(), nullptr, 16));
        p.frames = atoi(t[3].c_str());
        p.where = where_of(t[4][0]);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pins.push_back(p);
            g_pin_count = (int)g_pins.size();
        }
        log("probe: pinned %zu bytes at %#llx for %d frames (%c)", p.bytes.size(), (unsigned long long)p.addr, p.frames, t[4][0]);
    } else {
        log("probe: unknown: %s", line.c_str());
    }
}
}  // namespace

bool debug_on() {
    static int on = -1;
    if (on < 0) on = GetFileAttributesW((mod_dir() + L"\\probe_on").c_str()) != INVALID_FILE_ATTRIBUTES;
    return on == 1;
}

void probe_poll_file() {
    static int64_t next = 0;
    const int64_t now = now_ns();
    if (now < next) return;
    next = now + 250000000LL;
    if (!g_on) {
        static bool checked = false;
        if (!checked) {
            checked = true;
            g_on = GetFileAttributesW((mod_dir() + L"\\probe_on").c_str()) != INVALID_FILE_ATTRIBUTES;
            if (g_on) log("probe: on (bin\\coop\\probe.txt)");
        }
        if (!g_on) return;
    }
    const std::wstring path = mod_dir() + L"\\probe" + profile_suffix() + L".txt";
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
    fclose(f);
    if (all.empty()) return;
    FILE* w = _wfopen(path.c_str(), L"wb");
    if (w) fclose(w);
    std::lock_guard<std::mutex> lock(g_mutex);
    size_t i = 0;
    while (i < all.size()) {
        size_t e = all.find('\n', i);
        if (e == std::string::npos) e = all.size();
        std::string line = all.substr(i, e - i);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) g_lines.push_back(line);
        i = e + 1;
    }
}

void probe_run(Ctx* c, int where) {
    // (a line "@m ..." runs only at a menu screen's update, "@p ..." only at Link's: the thread a call is made from)
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<std::string> keep;
        for (auto& l : g_lines) {
            const int want = l.size() > 3 && l[0] == '@' && l[2] == ' ' ? where_of(l[1]) : 0;
            if (want ? want != where : where > 2) keep.push_back(l);
            else lines.push_back(want ? l.substr(3) : l);
        }
        g_lines.swap(keep);
    }
    for (const auto& l : lines) run_line(c, l);
}

void probe_pins(int where) {
    if (g_pin_count.load() == 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_pins.begin(); it != g_pins.end();) {
        if (it->where == where) {
            poke(it->addr, it->bytes.data(), it->bytes.size());
            if (--it->frames <= 0) {
                it = g_pins.erase(it);
                continue;
            }
        }
        ++it;
    }
    g_pin_count = (int)g_pins.size();
}

}  // namespace coop
