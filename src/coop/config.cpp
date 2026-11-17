#include "config.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>

#include "common.h"

namespace coop {

namespace {
std::mutex g_mutex;

std::wstring path() { return mod_dir() + L"\\coop_settings" + profile_suffix() + L".ini"; }

bool to_bool(const std::string& v) { return v == "1" || v == "true" || v == "on"; }

void load(Config& c) {
    FILE* f = _wfopen(path().c_str(), L"rb");
    if (!f) return;
    std::map<std::string, std::string> kv;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        const size_t eq = s.find('=');
        if (eq == std::string::npos || s[0] == '#') continue;
        kv[s.substr(0, eq)] = s.substr(eq + 1);
    }
    fclose(f);
    auto get = [&](const char* k, std::string& out) { if (kv.count(k)) out = kv[k]; };
    auto getb = [&](const char* k, bool& out) { if (kv.count(k)) out = to_bool(kv[k]); };
    auto geti = [&](const char* k, int& out, int lo, int hi) {
        if (kv.count(k)) out = std::max(lo, std::min(hi, atoi(kv[k].c_str())));
    };
    get("name", c.name);
    get("last_host", c.last_host);
    get("server_name", c.host.server);
    get("password", c.host.password);
    geti("max_players", c.host.max_players, 2, kMaxPlayers);
    getb("join_in_progress", c.host.join_in_progress);
    getb("share_kills", c.host.share_kills);
    getb("gifts", c.host.gifts);
    getb("same_age", c.host.same_age);
    getb("start_with_host", c.host.start_with_host);
    getb("revive", c.host.revive);
    getb("respawn_for_new", c.host.respawn_for_new);
    getb("starter", c.host.starter);
    int port = c.host.port;
    geti("port", port, 1024, 65535);
    c.host.port = (uint16_t)port;
    getb("nameplates", c.guest.nameplates);
    getb("hud", c.guest.hud);
    getb("far_names", c.guest.far_names);
    geti("hud_scale", c.guest.hud_scale, 60, 160);
    getb("gift_prompt2", c.guest.gift_prompt);   // (a new name: gifts are taken at once unless asked again)
    getb("sounds", c.guest.sounds);
    geti("smoothing", c.guest.smoothing, 0, 3);
    getb("autosave", c.guest.autosave);
    getb("coop_frame_generation", c.guest.coop_fg);
}
}  // namespace

Config& config() {
    static Config c;
    static std::once_flag once;
    std::call_once(once, [] { load(c); });
    return c;
}

void config_save() {
    std::lock_guard<std::mutex> lock(g_mutex);
    const Config& c = config();
    CreateDirectoryW(mod_dir().c_str(), nullptr);
    FILE* f = _wfopen(path().c_str(), L"wb");
    if (!f) return;
    fprintf(f, "# Longshot Co-op settings (the in-game Online Settings write this file)\n");
    fprintf(f, "name=%s\nlast_host=%s\n", c.name.c_str(), c.last_host.c_str());
    fprintf(f, "server_name=%s\npassword=%s\nmax_players=%d\njoin_in_progress=%d\nshare_kills=%d\ngifts=%d\nsame_age=%d\n"
               "start_with_host=%d\nport=%u\n",
            c.host.server.c_str(), c.host.password.c_str(), c.host.max_players, c.host.join_in_progress, c.host.share_kills,
            c.host.gifts, c.host.same_age, c.host.start_with_host, c.host.port);
    fprintf(f, "nameplates=%d\nhud=%d\nfar_names=%d\nhud_scale=%d\ngift_prompt2=%d\nsounds=%d\nsmoothing=%d\n", c.guest.nameplates,
            c.guest.hud, c.guest.far_names, c.guest.hud_scale, c.guest.gift_prompt, c.guest.sounds, c.guest.smoothing);
    fprintf(f, "revive=%d\nrespawn_for_new=%d\nautosave=%d\nstarter=%d\n", c.host.revive, c.host.respawn_for_new, c.guest.autosave,
            c.host.starter);
    fprintf(f, "coop_frame_generation=%d\n", c.guest.coop_fg);
    fclose(f);
}

}  // namespace coop
