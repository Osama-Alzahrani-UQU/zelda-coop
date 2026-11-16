// The mod's drawing over the game, inside the renderer's own Dear ImGui frame (its context, its fonts: the game's own
// menu font), in the style of the game's own screens: the Load screen's parchment cards and banner, white text with a
// soft shadow, a white glow round what is chosen. The title menu's buttons are the game's; these are the panels beside
// them (the lobby's slots, the servers, the keyboard, settings), the notices, and in play the players' list under the
// hearts and the nameplates over the other players.
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "coop.h"
#include "dialogs.h"
#include "fellow.h"
#include "game.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "menus.h"
#include "runtime.h"
#include "session.h"
#include "title.h"

namespace coop {

namespace {

constexpr ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
ImU32 fade(ImU32 c, float k) {
    const int a = (int)((c >> IM_COL32_A_SHIFT & 0xFF) * std::clamp(k, 0.0f, 1.0f));
    return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
}

// (the game's Load screen and pause menu, sampled: parchment, its dark edge and gold line, the banner's tan, the ink)
const ImU32 kParchTop = rgba(170, 134, 86, 240), kParchBot = rgba(140, 108, 66, 240), kParchEdge = rgba(74, 52, 28, 255),
            kGoldLine = rgba(226, 186, 104, 200), kBannerTop = rgba(214, 186, 140, 250), kBannerBot = rgba(176, 140, 92, 250),
            kBannerInk = rgba(52, 34, 16), kWhite = rgba(250, 246, 236), kShadow = rgba(20, 12, 4, 200), kDim = rgba(214, 200, 176),
            kDark = rgba(22, 16, 10, 215), kHeart = rgba(226, 46, 40), kHeartDark = rgba(70, 20, 18, 230),
            kGlow = rgba(255, 255, 255, 235), kGood = rgba(140, 220, 120), kWarn = rgba(255, 196, 96), kBlue = rgba(150, 190, 255),
            kDown = rgba(236, 64, 52);

struct Ui {
    ImDrawList* d;
    ImFont *body, *strong, *heading;
    float s, w, h;
};

float text_w(ImFont* f, float px, const std::string& t) { return f->CalcTextSizeA(px, FLT_MAX, 0, t.c_str()).x; }

void text(Ui& u, ImFont* f, float px, float x, float y, ImU32 col, const std::string& t, bool shadow = true) {
    if (shadow) {
        const float o = std::max(1.0f, px * 0.06f);
        u.d->AddText(f, px, ImVec2(x + o, y + o), fade(kShadow, ((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f), t.c_str());
    }
    u.d->AddText(f, px, ImVec2(x, y), col, t.c_str());
}
void text_c(Ui& u, ImFont* f, float px, float cx, float y, ImU32 col, const std::string& t, bool shadow = true) {
    text(u, f, px, cx - text_w(f, px, t) * 0.5f, y, col, t, shadow);
}
void text_r(Ui& u, ImFont* f, float px, float rx, float y, ImU32 col, const std::string& t, bool shadow = true) {
    text(u, f, px, rx - text_w(f, px, t), y, col, t, shadow);
}
// a text cut with "..." to fit
std::string fit(ImFont* f, float px, const std::string& t, float room) {
    if (text_w(f, px, t) <= room) return t;
    std::string s = t;
    while (!s.empty() && text_w(f, px, s + "...") > room) s.pop_back();
    return s + "...";
}

// a parchment card (the Load screen's slot): its gradient, dark edge, gold inner line; focused: the white glow round it
void card(Ui& u, ImVec2 a, ImVec2 b, bool focused = false, float alpha = 1.0f) {
    const float r = 7 * u.s;
    if (focused) {
        for (int i = 3; i >= 1; i--)
            u.d->AddRect(ImVec2(a.x - i * 2.0f * u.s, a.y - i * 2.0f * u.s), ImVec2(b.x + i * 2.0f * u.s, b.y + i * 2.0f * u.s),
                         fade(kGlow, alpha * 0.18f * (4 - i)), r + i * 2 * u.s, 0, 2.0f * u.s);
    }
    u.d->AddRectFilledMultiColor(ImVec2(a.x + r * 0.3f, a.y), ImVec2(b.x - r * 0.3f, b.y), fade(kParchTop, alpha), fade(kParchTop, alpha),
                                 fade(kParchBot, alpha), fade(kParchBot, alpha));
    u.d->AddRect(a, b, fade(kParchEdge, alpha), r, 0, 2.2f * u.s);
    u.d->AddRect(ImVec2(a.x + 3 * u.s, a.y + 3 * u.s), ImVec2(b.x - 3 * u.s, b.y - 3 * u.s), fade(kGoldLine, alpha * 0.8f), r * 0.7f, 0, 1.0f * u.s);
    if (focused) u.d->AddRect(a, b, fade(kGlow, alpha), r, 0, 2.6f * u.s);
}

// a dark box (the game's dialogs and the HUD's backing)
void box(Ui& u, ImVec2 a, ImVec2 b, float alpha = 1.0f) {
    const float r = 8 * u.s;
    u.d->AddRectFilled(a, b, fade(kDark, alpha), r);
    u.d->AddRect(a, b, fade(rgba(168, 136, 86, 200), alpha), r, 0, 1.5f * u.s);
}

// the banner over a panel (the Load screen's "Load" bar)
void banner(Ui& u, ImVec2 a, ImVec2 b, const std::string& title) {
    u.d->AddRectFilledMultiColor(a, b, kBannerTop, kBannerTop, kBannerBot, kBannerBot);
    u.d->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), rgba(110, 82, 44), 2 * u.s);
    const float px = 21 * u.s;
    text_c(u, u.heading, px, (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f - px * 0.55f, kBannerInk, title, false);
}

// a heart, filled by a quarter at a time (fill 0..1)
void heart(Ui& u, ImVec2 c, float size, float fill, float alpha) {
    auto shape = [&](ImU32 col, float scale) {
        const float r = size * 0.28f * scale;
        const ImVec2 l(c.x - r * 0.95f, c.y - size * 0.12f * scale), rr(c.x + r * 0.95f, c.y - size * 0.12f * scale);
        u.d->PathClear();
        u.d->PathArcTo(l, r, 3.1416f * 0.80f, 3.1416f * 2.0f, 12);
        u.d->PathArcTo(rr, r, 3.1416f * 1.0f, 3.1416f * 2.20f, 12);
        u.d->PathLineTo(ImVec2(c.x, c.y + size * 0.42f * scale));
        u.d->PathFillConvex(col);
    };
    shape(fade(rgba(40, 10, 8), alpha), 1.12f);
    shape(fade(kHeartDark, alpha), 1.0f);
    if (fill > 0.01f) {
        u.d->PushClipRect(ImVec2(c.x - size, c.y - size), ImVec2(c.x - size * 0.5f + size * fill, c.y + size), true);
        shape(fade(kHeart, alpha), 1.0f);
        u.d->PopClipRect();
    }
}

// the hearts of a life (4 a heart), in rows of ten
void hearts(Ui& u, float x, float y, float size, int life, int life_max, float alpha) {
    if (life_max <= 0) return;
    const int n = std::min(20, (life_max + 3) / 4);
    for (int i = 0; i < n; i++) {
        const float fill = std::clamp((life - i * 4) / 4.0f, 0.0f, 1.0f);
        const ImVec2 c(x + (i % 10) * size * 1.05f + size * 0.5f, y + (i / 10) * size * 1.0f + size * 0.5f);
        heart(u, c, size, fill, alpha);
    }
}

// a button prompt as the game draws them: a round button with its letter, then the word
float prompt(Ui& u, float x, float y, const char* key, const char* word, float alpha = 1.0f) {
    const float px = 16 * u.s, r = 10 * u.s;
    const float tw = text_w(u.body, px, word);
    text(u, u.body, px, x, y - px * 0.55f, fade(kWhite, alpha), word);
    const ImVec2 c(x + tw + 6 * u.s + r, y);
    u.d->AddCircleFilled(c, r, fade(kWhite, alpha), 20);
    const float kp = 13 * u.s;
    text_c(u, u.strong, kp, c.x, c.y - kp * 0.55f, fade(rgba(30, 30, 30), alpha), key, false);
    return tw + 6 * u.s + 2 * r + 22 * u.s;
}
// the prompts at the bottom right, as the game's "Back (B)"
void prompts_br(Ui& u, std::initializer_list<std::pair<const char*, const char*>> items) {
    float total = 0;
    for (const auto& it : items) total += text_w(u.body, 16 * u.s, it.second) + 6 * u.s + 20 * u.s + 22 * u.s;
    float x = u.w - total - 20 * u.s;
    const float y = u.h - 22 * u.s;
    // (over the game's own prompts, as in its pause menu: a dark band under ours so the two do not mix)
    if (pause_menu_open()) {
        const ImU32 band = rgba(16, 11, 6, 255), clear = rgba(16, 11, 6, 0);
        u.d->AddRectFilledMultiColor(ImVec2(u.w * 0.56f, u.h - 44 * u.s), ImVec2(u.w * 0.66f, u.h), clear, band, band, clear);
        u.d->AddRectFilled(ImVec2(u.w * 0.66f, u.h - 44 * u.s), ImVec2(u.w, u.h), band);
    }
    for (const auto& it : items) x += prompt(u, x, y, it.first, it.second);
}

std::string role_text(const Player& p) { return p.host ? "Host" : (p.ready ? "Ready" : "Not ready"); }

// ---- the title menu's pages: the panels beside the game's buttons
void draw_lobby(Ui& u, const SessionView& v, const MenuView& mv) {
    // (the right column beside the title's logo: the logo spans the middle 46% of the picture)
    const float x0 = u.w * 0.745f, pw = u.w - x0 - 18 * u.s, ph = 382 * u.s;
    const float y0 = u.h * 0.5f - ph * 0.5f - 60 * u.s;
    box(u, ImVec2(x0, y0), ImVec2(x0 + pw, y0 + ph), 0.94f);
    banner(u, ImVec2(x0, y0), ImVec2(x0 + pw, y0 + 40 * u.s), v.role == Role::Host ? "Lobby  (Host)" : "Lobby");
    float y = y0 + 52 * u.s;
    text(u, u.strong, 19 * u.s, x0 + 18 * u.s, y, kWhite, fit(u.strong, 19 * u.s, v.server, pw - 36 * u.s));
    y += 26 * u.s;
    // the server's state
    std::string status;
    ImU32 scol = kGood;
    if (v.role == Role::Host) {
        std::string ip = v.host_ip;
        const std::string def = ":" + std::to_string(net::kPortPlay);
        if (ip.size() > def.size() && ip.compare(ip.size() - def.size(), def.size(), def) == 0) ip.resize(ip.size() - def.size());
        status = v.server_running ? "Server running  " + ip : "Server stopped";
        scol = v.server_running ? kGood : kWarn;
    } else {
        status = v.phase == Phase::Starting || v.phase == Phase::Playing ? "The host is starting the game..." : "Connected to " + v.host_ip;
    }
    u.d->AddCircleFilled(ImVec2(x0 + 24 * u.s, y + 9 * u.s), 5 * u.s, scol, 16);
    text(u, u.body, 15.5f * u.s, x0 + 36 * u.s, y, kDim, fit(u.body, 15.5f * u.s, status, pw - 54 * u.s));
    y += 30 * u.s;
    int count = 0;
    for (const auto& p : v.players) count += p.used;
    // the four slots
    for (int i = 0; i < kMaxPlayers; i++) {
        const Player& p = v.players[i];
        const ImVec2 a(x0 + 14 * u.s, y), b(x0 + pw - 14 * u.s, y + 52 * u.s);
        if (p.used) {
            card(u, a, b, p.me);
            text(u, u.strong, 19 * u.s, a.x + 16 * u.s, a.y + 7 * u.s, kWhite, fit(u.strong, 19 * u.s, p.name, pw * 0.55f));
            text(u, u.body, 14 * u.s, a.x + 16 * u.s, a.y + 29 * u.s, p.host ? kWarn : (p.ready ? kGood : kDim), role_text(p) + (p.me ? "  (You)" : ""));
            if (!p.me && p.ping >= 0) text_r(u, u.body, 14 * u.s, b.x - 14 * u.s, a.y + 18 * u.s, kDim, std::to_string(p.ping) + " ms");
        } else {
            u.d->AddRect(a, b, rgba(168, 136, 86, 110), 7 * u.s, 0, 1.5f * u.s);
            const bool open = i < v.host.max_players;
            const float t = (float)fmod(now_ns() / 1e9, 1.6) / 1.6f;
            const std::string dots = open ? std::string("Waiting for a player") + std::string(1 + (int)(t * 3), '.') : "Closed";
            text(u, u.body, 16 * u.s, a.x + 16 * u.s, a.y + 16 * u.s, fade(kDim, open ? 0.85f : 0.45f), dots);
        }
        y += 60 * u.s;
    }
    text(u, u.body, 14.5f * u.s, x0 + 18 * u.s, y0 + ph - 26 * u.s, kDim,
         fit(u.body, 14.5f * u.s, "File: " + (mv.file_label.empty() ? std::string("-") : mv.file_label), pw * 0.62f));
    text_r(u, u.body, 14.5f * u.s, x0 + pw - 18 * u.s, y0 + ph - 26 * u.s, kDim, std::to_string(count) + " / " + std::to_string(v.host.max_players));
}

void draw_online_info(Ui& u) {
    const float x0 = u.w * 0.745f, pw = u.w - x0 - 18 * u.s, ph = 150 * u.s;
    const float y0 = u.h * 0.5f - 130 * u.s;
    box(u, ImVec2(x0, y0), ImVec2(x0 + pw, y0 + ph), 0.92f);
    banner(u, ImVec2(x0, y0), ImVec2(x0 + pw, y0 + 38 * u.s), "Online");
    float y = y0 + 52 * u.s;
    text(u, u.body, 16 * u.s, x0 + 18 * u.s, y, kDim, "Player");
    text_r(u, u.strong, 17 * u.s, x0 + pw - 18 * u.s, y, kWhite, Session::get().name());
    y += 28 * u.s;
    const uint32_t ip = net::preferred_ip();
    bool radmin = false;
    for (const auto& f : net::interfaces()) radmin |= f.radmin && f.ip == ip;
    text(u, u.body, 16 * u.s, x0 + 18 * u.s, y, kDim, radmin ? "Radmin VPN" : "Network");
    text_r(u, u.strong, 17 * u.s, x0 + pw - 18 * u.s, y, radmin ? kGood : kWarn, ip ? net::Addr{ip, 0}.str().substr(0, net::Addr{ip, 0}.str().size() - 2) : "No network");
    y += 30 * u.s;
    text(u, u.body, 13 * u.s, x0 + 16 * u.s, y, fade(kDim, 0.8f),
         fit(u.body, 13 * u.s, radmin ? "Friends on your Radmin VPN network find your games." : "Join a Radmin VPN network with your friends.", pw - 30 * u.s));
}

void draw_join(Ui& u) {
    const auto servers = Session::get().servers();
    const float x0 = u.w * 0.745f, pw = u.w - x0 - 18 * u.s, ph = 120 * u.s + std::max<size_t>(1, servers.size()) * 58 * u.s;
    const float y0 = u.h * 0.5f - 170 * u.s;
    box(u, ImVec2(x0, y0), ImVec2(x0 + pw, y0 + ph), 0.92f);
    banner(u, ImVec2(x0, y0), ImVec2(x0 + pw, y0 + 38 * u.s), "Games on your network");
    float y = y0 + 50 * u.s;
    if (servers.empty()) {
        const float t = (float)fmod(now_ns() / 1e9, 1.6) / 1.6f;
        text(u, u.body, 16 * u.s, x0 + 18 * u.s, y + 14 * u.s, kDim, "Looking for games" + std::string(1 + (int)(t * 3), '.'));
        y += 58 * u.s;
    }
    for (const auto& s : servers) {
        const ImVec2 a(x0 + 14 * u.s, y), b(x0 + pw - 14 * u.s, y + 50 * u.s);
        card(u, a, b, false);
        text(u, u.strong, 18 * u.s, a.x + 14 * u.s, a.y + 6 * u.s, kWhite, fit(u.strong, 18 * u.s, s.server, pw * 0.6f));
        text(u, u.body, 14 * u.s, a.x + 14 * u.s, a.y + 28 * u.s, kDim,
             "Host: " + s.host + (s.in_game ? "   In game" : "") + (s.locked ? "   Password" : "") + (s.invite ? "   Invited you" : ""));
        text_r(u, u.strong, 16 * u.s, b.x - 14 * u.s, a.y + 15 * u.s, s.players < s.max ? kGood : kWarn,
               std::to_string(s.players) + "/" + std::to_string(s.max));
        y += 58 * u.s;
    }
    text(u, u.body, 13 * u.s, x0 + 16 * u.s, y0 + ph - 50 * u.s, fade(kDim, 0.85f), fit(u.body, 13 * u.s, "Choose a game below the logo to join it.", pw - 30 * u.s));
    text(u, u.body, 13 * u.s, x0 + 16 * u.s, y0 + ph - 30 * u.s, fade(kDim, 0.85f), fit(u.body, 13 * u.s, "Not listed? Join by Address (the host's Radmin IP).", pw - 30 * u.s));
}

void draw_loading(Ui& u, const MenuView& mv) {
    const float t = (float)((now_ns() - mv.loading_since) / 1e9);
    const float a = std::min(1.0f, t * 4);
    u.d->AddRectFilled(ImVec2(0, 0), ImVec2(u.w, u.h), fade(rgba(8, 6, 4, 200), a));
    const float cx = u.w * 0.5f, cy = u.h * 0.5f;
    // (the game's loading mark: a ring of gold dots turning)
    for (int i = 0; i < 12; i++) {
        const float ang = i / 12.0f * 6.2832f + t * 4.0f;
        const float k = 0.25f + 0.75f * (float)((i + (int)(t * 12)) % 12) / 11.0f;
        u.d->AddCircleFilled(ImVec2(cx + cosf(ang) * 26 * u.s, cy - 30 * u.s + sinf(ang) * 26 * u.s), 4 * u.s, fade(kGoldLine, a * k), 12);
    }
    text_c(u, u.heading, 24 * u.s, cx, cy + 14 * u.s, fade(kWhite, a), mv.loading_text);
}

// ---- the dialogs
void draw_keyboard(Ui& u, const TextEdit& e) {
    u.d->AddRectFilled(ImVec2(0, 0), ImVec2(u.w, u.h), rgba(0, 0, 0, 120));
    const float pw = 640 * u.s, ph = 400 * u.s;
    const float x0 = (u.w - pw) * 0.5f, y0 = (u.h - ph) * 0.5f;
    box(u, ImVec2(x0, y0), ImVec2(x0 + pw, y0 + ph), 0.97f);
    banner(u, ImVec2(x0, y0), ImVec2(x0 + pw, y0 + 40 * u.s), e.title);
    // the field
    const ImVec2 fa(x0 + 30 * u.s, y0 + 56 * u.s), fb(x0 + pw - 30 * u.s, y0 + 100 * u.s);
    card(u, fa, fb, true);
    const bool caret = fmod(now_ns() / 1e9, 1.0) < 0.55;
    text(u, u.strong, 24 * u.s, fa.x + 14 * u.s, fa.y + 8 * u.s, kWhite, e.text + (caret ? "|" : " "));
    if (!e.hint.empty()) text(u, u.body, 14 * u.s, x0 + 32 * u.s, y0 + 106 * u.s, kDim, e.hint);
    // the keys
    static const char* const keys = "1234567890QWERTYUIOPASDFGHJKL-ZXCVBNM_.:";
    const float kx0 = x0 + 30 * u.s, ky0 = y0 + 134 * u.s, kw = (pw - 60 * u.s) / 10, kh = 44 * u.s;
    for (int y = 0; y < 5; y++) {
        for (int x = 0; x < 10; x++) {
            const int k = y * 10 + x;
            const bool sel = e.cursor_key == k || (y == 4 && e.cursor_key / 10 == 4 &&
                                                   ((x <= 2 && e.cursor_key % 10 <= 2) || (x >= 3 && x <= 6 && e.cursor_key % 10 >= 3 && e.cursor_key % 10 <= 6) ||
                                                    (x >= 7 && x <= 8 && e.cursor_key % 10 >= 7 && e.cursor_key % 10 <= 8) || (x == 9 && e.cursor_key % 10 == 9)));
            if (y == 4 && x != 0 && x != 3 && x != 7 && x != 9) continue;
            float span = 1;
            std::string label;
            if (y < 4) label = std::string(1, keys[k]);
            else if (x == 0) span = 3, label = "Aa";
            else if (x == 3) span = 4, label = "Space";
            else if (x == 7) span = 2, label = "Delete";
            else span = 1, label = "OK";
            const ImVec2 a(kx0 + x * kw + 3 * u.s, ky0 + y * (kh + 4 * u.s)), b(kx0 + (x + span) * kw - 3 * u.s, ky0 + y * (kh + 4 * u.s) + kh);
            card(u, a, b, sel);
            text_c(u, u.strong, 19 * u.s, (a.x + b.x) * 0.5f, a.y + kh * 0.5f - 11 * u.s, kWhite, label);
        }
    }
    prompts_br(u, {{"B", "Delete"}, {"+", "Done"}, {"-", "Cancel"}});
}

void draw_list_dialog(Ui& u, const DialogsView& dv, float width, bool values) {
    u.d->AddRectFilled(ImVec2(0, 0), ImVec2(u.w, u.h), rgba(0, 0, 0, 110));
    const int n = (int)dv.rows.size();
    const float rowh = 46 * u.s;
    const float extra = dv.kind == kDlgSettings ? 92 * u.s : (dv.text.empty() ? 30 * u.s : 70 * u.s);
    const float pw = width * u.s, ph = 50 * u.s + n * rowh + extra;
    const float x0 = (u.w - pw) * 0.5f, y0 = std::max(20 * u.s, (u.h - ph) * 0.5f);
    box(u, ImVec2(x0, y0), ImVec2(x0 + pw, y0 + ph), 0.97f);
    banner(u, ImVec2(x0, y0), ImVec2(x0 + pw, y0 + 40 * u.s), dv.title);
    float y = y0 + 48 * u.s;
    if (dv.kind == kDlgSettings) {
        // (the tabs: Host and Player, as the game's pages, L and R)
        const char* tabs[2] = {"Host", "Player"};
        for (int i = 0; i < 2; i++) {
            const float tx = x0 + pw * (0.30f + 0.40f * i);
            text_c(u, u.strong, 19 * u.s, tx, y + 2 * u.s, dv.tab == i ? kWhite : fade(kDim, 0.7f), tabs[i]);
            if (dv.tab == i) u.d->AddLine(ImVec2(tx - 40 * u.s, y + 28 * u.s), ImVec2(tx + 40 * u.s, y + 28 * u.s), kGoldLine, 2.5f * u.s);
        }
        prompt(u, x0 + 18 * u.s, y + 14 * u.s, "L", "");
        prompt(u, x0 + pw - 46 * u.s, y + 14 * u.s, "R", "");
        y += 40 * u.s;
    } else if (!dv.text.empty()) {
        text(u, u.body, 16 * u.s, x0 + 24 * u.s, y + 4 * u.s, kDim, fit(u.body, 16 * u.s, dv.text, pw - 48 * u.s));
        y += 36 * u.s;
    }
    for (int i = 0; i < n; i++) {
        const auto& r = dv.rows[(size_t)i];
        const ImVec2 a(x0 + 16 * u.s, y), b(x0 + pw - 16 * u.s, y + rowh - 6 * u.s);
        card(u, a, b, i == dv.sel);
        text(u, u.strong, 18 * u.s, a.x + 14 * u.s, a.y + 9 * u.s, kWhite, fit(u.strong, 18 * u.s, r.name, pw * 0.55f));
        if (values && !r.value.empty()) {
            const std::string val = dv.kind == kDlgSettings && i == dv.sel && !r.edit ? "<  " + r.value + "  >" : r.value;
            text_r(u, u.strong, 18 * u.s, b.x - 14 * u.s, a.y + 9 * u.s, i == dv.sel ? rgba(255, 226, 150) : kDim, val);
        }
        y += rowh;
    }
    if (dv.kind == kDlgSettings && dv.sel >= 0 && dv.sel < n) {
        text(u, u.body, 14.5f * u.s, x0 + 22 * u.s, y + 6 * u.s, kDim, fit(u.body, 14.5f * u.s, dv.rows[(size_t)dv.sel].help, pw - 44 * u.s));
    }
    switch (dv.kind) {
    case kDlgSettings: prompts_br(u, {{"A", "Change"}, {"B", "Back"}}); break;
    case kDlgInvite: prompts_br(u, {{"A", "Invite"}, {"B", "Back"}}); break;
    case kDlgGiftOffer: prompts_br(u, {{"A", "OK"}, {"B", "Decline"}}); break;
    default: prompts_br(u, {{"A", "OK"}, {"B", "Back"}}); break;
    }
}

void draw_amount(Ui& u, const DialogsView& dv) {
    u.d->AddRectFilled(ImVec2(0, 0), ImVec2(u.w, u.h), rgba(0, 0, 0, 110));
    const float pw = 460 * u.s, ph = 190 * u.s;
    const float x0 = (u.w - pw) * 0.5f, y0 = (u.h - ph) * 0.5f;
    box(u, ImVec2(x0, y0), ImVec2(x0 + pw, y0 + ph), 0.97f);
    banner(u, ImVec2(x0, y0), ImVec2(x0 + pw, y0 + 40 * u.s), dv.title);
    text_c(u, u.body, 16 * u.s, x0 + pw * 0.5f, y0 + 54 * u.s, kDim, dv.text);
    const ImVec2 a(x0 + pw * 0.5f - 90 * u.s, y0 + 86 * u.s), b(x0 + pw * 0.5f + 90 * u.s, y0 + 140 * u.s);
    card(u, a, b, true);
    text_c(u, u.heading, 30 * u.s, x0 + pw * 0.5f, a.y + 10 * u.s, kWhite, std::to_string(dv.amount));
    text(u, u.strong, 26 * u.s, a.x + 12 * u.s, a.y + 12 * u.s, dv.amount > 1 ? kWhite : fade(kDim, 0.3f), "<");
    text_r(u, u.strong, 26 * u.s, b.x - 12 * u.s, a.y + 12 * u.s, dv.amount < dv.amount_max ? kWhite : fade(kDim, 0.3f), ">");
    text_c(u, u.body, 14 * u.s, x0 + pw * 0.5f, y0 + 152 * u.s, fade(kDim, 0.8f), "You have " + std::to_string(dv.amount_max));
    prompts_br(u, {{"A", "Give"}, {"B", "Cancel"}});
}

void draw_waiting(Ui& u, const DialogsView& dv) {
    const float pw = 380 * u.s, ph = 96 * u.s;
    const float x0 = (u.w - pw) * 0.5f, y0 = u.h * 0.22f;
    box(u, ImVec2(x0, y0), ImVec2(x0 + pw, y0 + ph), 0.95f);
    banner(u, ImVec2(x0, y0), ImVec2(x0 + pw, y0 + 38 * u.s), dv.title);
    const float t = (float)fmod(now_ns() / 1e9, 1.6) / 1.6f;
    text_c(u, u.body, 16 * u.s, x0 + pw * 0.5f, y0 + 54 * u.s, kDim, "Waiting for an answer" + std::string(1 + (int)(t * 3), '.'));
    prompts_br(u, {{"B", "Cancel"}});
}

// ---- the notices: top centre, as the game's small banners
void draw_toasts(Ui& u, bool sounds) {
    (void)sounds;
    const auto toasts = Session::get().toasts();
    const int64_t now = net::now_ms();
    float y = 18 * u.s;
    for (const auto& t : toasts) {
        const float age = (float)(now - t.at_ms) / 1000.0f;
        if (age > 5.0f) continue;
        const float a = std::min(1.0f, std::min(age * 5, (5.0f - age) * 2));
        const float px = 17 * u.s;
        const float tw = text_w(u.strong, px, t.text);
        const float pw = tw + 60 * u.s, ph = 38 * u.s;
        const float x0 = (u.w - pw) * 0.5f;
        box(u, ImVec2(x0, y), ImVec2(x0 + pw, y + ph), a * 0.95f);
        const ImU32 dot = t.kind == 1 ? kGood : t.kind == 2 ? kWarn : kBlue;
        u.d->AddCircleFilled(ImVec2(x0 + 20 * u.s, y + ph * 0.5f), 5 * u.s, fade(dot, a), 16);
        text(u, u.strong, px, x0 + 36 * u.s, y + ph * 0.5f - px * 0.58f, fade(kWhite, a), t.text);
        y += ph + 6 * u.s;
    }
}

// ---- in play: the players' list under the hearts, the nameplates
bool project(const CameraView& cam, const float p[3], float w, float h, float* sx, float* sy, float* depth) {
    const float* m = cam.m;
    const float vx = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    const float vy = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    const float vz = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
    if (vz > -0.2f) return false;
    const float t = tanf(cam.fovy * 0.5f);
    const float aspect = w / h;
    const float nx = (vx / -vz) / (t * aspect), ny = (vy / -vz) / t;
    *sx = (nx * 0.5f + 0.5f) * w;
    *sy = (0.5f - ny * 0.5f) * h;
    *depth = -vz;
    return nx > -1.3f && nx < 1.3f && ny > -1.3f && ny < 1.3f;
}

// the shown position: between the last two states, by when they came (the network's jitter smoothed)
void smoothed(const Player& p, int smoothing, float out[3]) {
    const PlayerState &a = p.prev, &b = p.st;
    const int64_t now = net::now_ms();
    const int64_t span = std::max<int64_t>(1, b.got_ms - a.got_ms);
    static const int delay[4] = {0, 50, 80, 120};
    const float t = smoothing == 0 || !a.got_ms || span > 500 ? 1.0f : std::clamp((float)(now - delay[smoothing] - a.got_ms) / (float)span, 0.0f, 1.25f);
    for (int i = 0; i < 3; i++) out[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * t;
}


// ---- the other players in the world. The game's own Link cannot be added to its world yet (an actor asked of its manager
// is made but never placed), so each other player is drawn where they are: a figure of Link in his colours, sized as he
// is (a child 1.3 m, an adult 1.8 m), turned as he faces, his steps as he moves, lying on his side when down.
// Moved without delay: the newest state carried forward by the speed it shows (up to 150 ms), then eased toward that.
struct Shown {
    bool init = false;
    uint32_t seq = 0;
    float pos[3] = {0, 0, 0}, vel[3] = {0, 0, 0};
    float yaw = 0, phase = 0, down = 0;
    int64_t at = 0;
};
Shown g_shown[kMaxPlayers];
std::atomic<bool> g_hit_test[kMaxPlayers];   // (developer probe: a hit shown on that player's nameplate)

float angle_to(float from, float to) {
    float d = fmodf(to - from + 3.14159265f, 6.2831853f);
    if (d < 0) d += 6.2831853f;
    return d - 3.14159265f;
}

void update_shown(int i, const Player& p, int smoothing) {
    Shown& s = g_shown[i];
    const int64_t now = now_ns();
    const float dt = s.at ? std::clamp((float)(now - s.at) / 1e9f, 0.0f, 0.1f) : 0.0f;
    s.at = now;
    if (s.seq != p.st.seq) {
        s.seq = p.st.seq;
        const float span = (float)(p.st.got_ms - p.prev.got_ms) / 1000.0f;
        if (p.prev.got_ms && span > 0.004f && span < 0.5f)
            for (int k = 0; k < 3; k++) s.vel[k] = (p.st.pos[k] - p.prev.pos[k]) / span;
        else
            for (float& v : s.vel) v = 0;
    }
    const float age = std::clamp((float)(net::now_ms() - p.st.got_ms) / 1000.0f, 0.0f, 0.15f);
    float target[3];
    for (int k = 0; k < 3; k++) target[k] = p.st.pos[k] + s.vel[k] * age;
    const float jump = sqrtf((target[0] - s.pos[0]) * (target[0] - s.pos[0]) + (target[1] - s.pos[1]) * (target[1] - s.pos[1]) +
                             (target[2] - s.pos[2]) * (target[2] - s.pos[2]));
    static const float rate[4] = {1000.0f, 30.0f, 18.0f, 10.0f};   // (Motion smoothing: off, light, normal, strong)
    const float k = !s.init || jump > 6.0f ? 1.0f : 1.0f - expf(-rate[std::clamp(smoothing, 0, 3)] * dt);
    for (int j = 0; j < 3; j++) s.pos[j] += (target[j] - s.pos[j]) * k;
    s.yaw = !s.init ? p.st.yaw : s.yaw + angle_to(s.yaw, p.st.yaw) * (1.0f - expf(-16.0f * dt));
    const float speed = std::min(sqrtf(s.vel[0] * s.vel[0] + s.vel[2] * s.vel[2]), 9.0f);
    s.phase = fmodf(s.phase + speed * dt * 3.2f, 6.2831853f);
    const float want_down = (p.st.flags & kDowned) ? 1.0f : 0.0f;
    s.down += (want_down - s.down) * (1.0f - expf(-7.0f * dt));
    s.init = true;
}

// a point of the figure (x right, y up, z forward, in heights) placed in the world: lying turns it onto its side first
void figure_point(const Shown& s, float height, float x, float y, float z, float out[3]) {
    const float a = s.down * 1.5708f, ca = cosf(a), sa = sinf(a);
    const float lx = x * ca - y * sa, ly = x * sa + y * ca;   // (roll about the forward axis)
    const float wx = lx * height, wy = ly * height + s.down * 0.12f, wz = z * height;
    const float c = cosf(s.yaw), sn = sinf(s.yaw);
    out[0] = s.pos[0] + wx * c + wz * sn;
    out[1] = s.pos[1] + wy;
    out[2] = s.pos[2] - wx * sn + wz * c;
}

void draw_figure(Ui& u, const CameraView& cam, const Shown& s, bool child, float alpha) {
    const float H = child ? 1.3f : 1.8f;
    const float focal = 0.5f * u.h / tanf(cam.fovy * 0.5f);
    const float speed = std::min(sqrtf(s.vel[0] * s.vel[0] + s.vel[2] * s.vel[2]) / 3.0f, 1.0f) * (1.0f - s.down);
    const float swing = sinf(s.phase) * 0.55f * speed;
    struct Seg {
        float a[3], b[3];   // (figure units)
        float width;        // (heights)
        ImU32 col;
        float depth = 0;
        ImVec2 pa, pb;
        float wa = 0, wb = 0;
        bool ok = false;
    };
    const ImU32 tunic = rgba(52, 128, 46), tunic_dark = rgba(40, 100, 36), skin = rgba(238, 196, 156), hair = rgba(236, 196, 88),
                tights = rgba(232, 222, 196), boot = rgba(96, 62, 32), belt = rgba(110, 72, 36), hat = rgba(58, 138, 48);
    const float leg_f = sinf(swing) * 0.22f, leg_up = (1 - cosf(swing)) * 0.22f;
    std::vector<Seg> segs = {
        // legs (hip -> foot), swinging
        {{-0.07f, 0.46f, 0}, {-0.07f, 0.05f + leg_up, leg_f}, 0.075f, tights},
        {{0.07f, 0.46f, 0}, {0.07f, 0.05f + leg_up * 0.0f, -leg_f}, 0.075f, tights},
        {{-0.07f, 0.16f + leg_up, leg_f * 0.75f}, {-0.07f, 0.03f + leg_up, leg_f + 0.03f}, 0.095f, boot},
        {{0.07f, 0.16f, -leg_f * 0.75f}, {0.07f, 0.03f, -leg_f + 0.03f}, 0.095f, boot},
        // the tunic: skirt and chest
        {{0, 0.36f, 0}, {0, 0.50f, 0}, 0.30f, tunic_dark},
        {{0, 0.48f, 0}, {0, 0.73f, 0}, 0.23f, tunic},
        {{0, 0.49f, 0.005f}, {0, 0.52f, 0.005f}, 0.235f, belt},
        // arms (shoulder -> hand), swinging against the legs
        {{-0.15f, 0.70f, 0}, {-0.18f, 0.46f, -leg_f * 0.8f}, 0.065f, tunic},
        {{0.15f, 0.70f, 0}, {0.18f, 0.46f, leg_f * 0.8f}, 0.065f, tunic},
        {{-0.18f, 0.48f, -leg_f * 0.8f}, {-0.185f, 0.43f, -leg_f * 0.85f}, 0.06f, skin},
        {{0.18f, 0.48f, leg_f * 0.8f}, {0.185f, 0.43f, leg_f * 0.85f}, 0.06f, skin},
        // neck, head, hair, the hat falling back
        {{0, 0.72f, 0}, {0, 0.77f, 0}, 0.07f, skin},
        {{0, 0.84f, 0.01f}, {0, 0.845f, 0.01f}, 0.215f, skin},
        {{0, 0.87f, -0.03f}, {0, 0.88f, -0.035f}, 0.22f, hair},
        {{0, 0.90f, -0.01f}, {0, 0.93f, -0.06f}, 0.19f, hat},
        {{0, 0.93f, -0.06f}, {0, 0.88f, -0.18f}, 0.13f, hat},
        {{0, 0.88f, -0.18f}, {0, 0.78f, -0.27f}, 0.07f, hat},
    };
    for (auto& g : segs) {
        float wa[3], wb[3], da, db;
        figure_point(s, H, g.a[0], g.a[1], g.a[2], wa);
        figure_point(s, H, g.b[0], g.b[1], g.b[2], wb);
        if (!project(cam, wa, u.w, u.h, &g.pa.x, &g.pa.y, &da) || !project(cam, wb, u.w, u.h, &g.pb.x, &g.pb.y, &db)) continue;
        g.depth = (da + db) * 0.5f;
        g.wa = g.width * H * focal / std::max(da, 0.3f);
        g.wb = g.width * H * focal / std::max(db, 0.3f);
        g.ok = true;
    }
    // (the face toward the camera: the hat and the hair behind it are drawn first; farther parts first)
    std::stable_sort(segs.begin(), segs.end(), [](const Seg& x, const Seg& y) { return x.depth > y.depth; });
    // (a soft shadow on the ground)
    {
        float g0[3], sx, sy, sd;
        g0[0] = s.pos[0], g0[1] = s.pos[1] + 0.02f, g0[2] = s.pos[2];
        if (project(cam, g0, u.w, u.h, &sx, &sy, &sd)) {
            const float r = 0.28f * H * focal / std::max(sd, 0.3f);
            u.d->AddEllipseFilled(ImVec2(sx, sy), ImVec2(r * (1.0f + s.down * 1.4f), r * 0.32f), fade(rgba(0, 0, 0, 120), alpha), 0, 20);
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        for (const auto& g : segs) {
            if (!g.ok) continue;
            const float w = (g.wa + g.wb) * 0.5f;
            const ImU32 col = pass == 0 ? fade(rgba(24, 16, 8, 230), alpha) : fade(g.col, alpha);
            const float grow = pass == 0 ? std::max(1.5f, w * 0.12f) * 2 : 0;
            u.d->AddLine(g.pa, g.pb, col, w + grow);
            u.d->AddCircleFilled(g.pa, (w + grow) * 0.5f, col, 16);
            u.d->AddCircleFilled(g.pb, (w + grow) * 0.5f, col, 16);
        }
    }
    // (the eyes when he faces the camera)
    float head[3], fwd[3], hx, hy, hd, fx, fy, fd;
    figure_point(s, H, 0, 0.845f, 0.10f, fwd);
    figure_point(s, H, 0, 0.845f, 0.0f, head);
    if (s.down < 0.5f && project(cam, head, u.w, u.h, &hx, &hy, &hd) && project(cam, fwd, u.w, u.h, &fx, &fy, &fd) && fd < hd - 0.06f) {
        const float r = 0.016f * H * focal / std::max(hd, 0.3f);
        const float side = (fx - hx);
        for (int e = -1; e <= 1; e += 2) {
            float ep[3], ex, ey, ed;
            figure_point(s, H, 0.045f * e, 0.85f, 0.105f, ep);
            if (project(cam, ep, u.w, u.h, &ex, &ey, &ed)) u.d->AddCircleFilled(ImVec2(ex, ey), r, fade(rgba(40, 70, 140), alpha), 10);
        }
        (void)side;
    }
}

void draw_hud(Ui& u, const SessionView& v, const GuestSettings& gs) {
    const LocalSnapshot me = local_snapshot();
    const float k = gs.hud_scale / 100.0f;
    // (the players' list: at the left under the game's hearts and magic)
    float y = 128 * u.s;
    const float x = 26 * u.s;
    if (gs.hud) {
        for (int i = 0; i < kMaxPlayers; i++) {
            const Player& p = v.players[i];
            if (!p.used || p.me) continue;
            const bool stale = net::now_ms() - p.st.got_ms > 3000;
            const float a = stale ? 0.55f : 1.0f;
            const float px = 16 * u.s * k;
            const float rows = p.st.life_max > 40 ? 2 : 1;
            const float bh = px * 1.5f + 15 * u.s * k * rows + 10 * u.s * k;
            box(u, ImVec2(x - 8 * u.s, y - 4 * u.s), ImVec2(x + 196 * u.s * k, y + bh), 0.62f * a);
            text(u, u.strong, px, x, y, fade(kWhite, a), fit(u.strong, px, p.name, 150 * u.s * k));
            if (p.st.flags & kDowned) {
                const float pulse = 0.55f + 0.45f * sinf((float)(now_ns() / 1e9) * 5.0f);
                u.d->AddRect(ImVec2(x - 8 * u.s, y - 4 * u.s), ImVec2(x + 196 * u.s * k, y + bh), fade(kDown, pulse * a), 8 * u.s, 0, 2.0f * u.s);
                text_r(u, u.strong, 12.5f * u.s * k, x + 186 * u.s * k, y + 2 * u.s, fade(kDown, a), "DOWN");
            } else if (p.host) {
                text_r(u, u.body, 12 * u.s * k, x + 186 * u.s * k, y + 2 * u.s, fade(kWarn, a), "Host");
            }
            hearts(u, x, y + px * 1.35f, 14 * u.s * k, p.st.life, p.st.life_max, a);
            if (p.st.map[0] && strcmp(p.st.map, me.map) != 0) {
                text(u, u.body, 11.5f * u.s * k, x, y + bh - 16 * u.s * k, fade(kDim, a * 0.9f), fit(u.body, 11.5f * u.s * k, place_name(p.st.map), 180 * u.s * k));
            }
            y += bh + 8 * u.s;
        }
    }
    // the other players in the world, then their nameplates
    const CameraView cam = camera_view();
    if (!cam.valid || now_ns() - cam.at_ns > 500000000LL) return;
    for (int i = 0; i < kMaxPlayers; i++) {
        const Player& p = v.players[i];
        if (!p.used || p.me || !(p.st.flags & kInPlay) || strcmp(p.st.map, me.map) != 0) {
            g_shown[i].init = false;
            continue;
        }
        update_shown(i, p, gs.smoothing);
        float d[3] = {g_shown[i].pos[0] - me.pos[0], g_shown[i].pos[1] - me.pos[1], g_shown[i].pos[2] - me.pos[2]};
        const float away = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (away < 120.0f && !puppet_placed(i)) draw_figure(u, cam, g_shown[i], p.st.child, std::clamp(1.3f - away / 120.0f, 0.3f, 1.0f));
    }
    if (!gs.nameplates) return;
    for (int i = 0; i < kMaxPlayers; i++) {
        const Player& p = v.players[i];
        if (!p.used || p.me || !(p.st.flags & kInPlay) || strcmp(p.st.map, me.map) != 0 || !g_shown[i].init) continue;
        // (behind a wall: no nameplate, as the world hides the player; the list at the left keeps his hearts)
        static float shown_k[kMaxPlayers] = {1, 1, 1, 1};
        shown_k[i] += ((player_behind_wall(i) ? 0.0f : 1.0f) - shown_k[i]) * 0.25f;
        if (shown_k[i] < 0.03f) continue;
        float pos[3] = {g_shown[i].pos[0], g_shown[i].pos[1], g_shown[i].pos[2]};
        bool pchild = p.st.child;
        const bool on_puppet = puppet_position(i, pos, &pchild);   // (the game's Link of that player: his own place)
        const bool down = p.st.flags & kDowned;
        if (down) {
            // (a red ring on the ground round them, beating)
            const float t = (float)(now_ns() / 1e9);
            const float r = 0.75f + 0.12f * sinf(t * 4.0f);
            ImVec2 ring[24];
            int n = 0;
            for (int j = 0; j < 24; j++) {
                const float ang = j / 24.0f * 6.2832f;
                const float q[3] = {pos[0] + r * cosf(ang), pos[1] + 0.05f, pos[2] + r * sinf(ang)};
                float rx, ry, rd;
                if (project(cam, q, u.w, u.h, &rx, &ry, &rd)) ring[n++] = ImVec2(rx, ry);
            }
            if (n == 24) {
                u.d->AddPolyline(ring, n, fade(kDown, 0.85f), ImDrawFlags_Closed, 3.0f * u.s);
                u.d->AddConvexPolyFilled(ring, n, fade(kDown, 0.16f + 0.08f * sinf(t * 4.0f)));
            }
        }
        pos[1] += (pchild ? (on_puppet ? 1.42f : 1.55f) : (on_puppet ? 1.98f : 2.15f)) * (1.0f - g_shown[i].down * 0.8f);   // (just over the head; low when lying)
        float sx, sy, depth;
        if (!project(cam, pos, u.w, u.h, &sx, &sy, &depth)) continue;
        if (!gs.far_names && depth > 30.0f) continue;
        const float scale = std::clamp(8.0f / std::max(depth, 1.0f), 0.55f, 1.15f) * k;
        const float a = std::clamp(1.4f - depth / 80.0f, 0.35f, 1.0f) * shown_k[i];
        const float px = 19 * u.s * scale;
        const float tw = text_w(u.strong, px, p.name);
        const float bw = std::max(tw + 22 * u.s * scale, 96 * u.s * scale);
        const float bh = px + 18 * u.s * scale;
        const ImVec2 a0(sx - bw * 0.5f, sy - bh), a1(sx + bw * 0.5f, sy);
        u.d->AddRectFilled(a0, a1, fade(kDark, 0.62f * a), 6 * u.s * scale);
        if (down) {
            const float pulse = 0.6f + 0.4f * sinf((float)(now_ns() / 1e9) * 5.0f);
            u.d->AddRect(a0, a1, fade(kDown, pulse * a), 6 * u.s * scale, 0, 2.2f * u.s * scale);
            const float dp = 15 * u.s * scale;
            text_c(u, u.strong, dp, sx, a0.y - dp * 1.25f, fade(kDown, a), "DOWN  -  needs a revive");
        }
        // (hit: the plate's edge flashes red and the hearts lost rise over it for a second; their Link plays the game's own
        // damage reaction, as their pose shows it)
        static int life_was[kMaxPlayers] = {-1, -1, -1, -1};
        static int64_t hit_at[kMaxPlayers];
        static int hit_lost[kMaxPlayers];
        const int64_t tnow = now_ns();
        if (g_hit_test[i].exchange(false)) hit_at[i] = tnow, hit_lost[i] = 2;
        if (life_was[i] >= 0 && p.st.life >= 0 && p.st.life < life_was[i]) hit_at[i] = tnow, hit_lost[i] = life_was[i] - p.st.life;
        life_was[i] = p.st.life;
        const float hit = hit_at[i] ? 1.0f - std::min(1.0f, (float)(tnow - hit_at[i]) / 1.1e9f) : 0.0f;
        if (hit > 0) {
            u.d->AddRect(a0, a1, fade(kDown, hit * a), 6 * u.s * scale, 0, 3.0f * u.s * scale);
            const float hp2 = 17 * u.s * scale, rise = (1.0f - hit) * 26 * u.s * scale;
            char lost[24];
            const int q = hit_lost[i];
            if (q % 4 == 0) snprintf(lost, sizeof lost, "-%d", q / 4);
            else snprintf(lost, sizeof lost, "-%.2g", q / 4.0f);
            const float lw = text_w(u.strong, hp2, lost), hs = hp2 * 0.9f;
            const float lx = sx - (lw + hs + 4 * u.s) * 0.5f, ly = a0.y - hp2 * 1.3f - rise - (down ? hp2 * 1.2f : 0.0f);
            text_c(u, u.strong, hp2, lx + lw * 0.5f, ly, fade(kDown, std::min(1.0f, hit * 1.6f) * a), lost);
            heart(u, ImVec2(lx + lw + 4 * u.s + hs * 0.5f, ly + hp2 * 0.55f), hs, 1.0f, std::min(1.0f, hit * 1.6f) * a);
        }
        text_c(u, u.strong, px, sx, a0.y + 3 * u.s * scale, fade(kWhite, a), p.name);
        // (life as a bar: red over dark, quarters as the hearts)
        if (p.st.life_max > 0) {
            const float frac = std::clamp((float)p.st.life / (float)p.st.life_max, 0.0f, 1.0f);
            const ImVec2 b0(a0.x + 9 * u.s * scale, a1.y - 9 * u.s * scale), b1(a1.x - 9 * u.s * scale, a1.y - 5 * u.s * scale);
            u.d->AddRectFilled(b0, b1, fade(kHeartDark, a), 2 * u.s);
            u.d->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * frac, b1.y), fade(frac > 0.25f ? kHeart : kWarn, a), 2 * u.s);
        }
        // (a small arrow under the plate, pointing at the player)
        u.d->AddTriangleFilled(ImVec2(sx - 6 * u.s * scale, sy), ImVec2(sx + 6 * u.s * scale, sy), ImVec2(sx, sy + 7 * u.s * scale), fade(kDark, 0.62f * a));
    }
}

// ---- the other players on the area map (the Map screen) and on the minimap of play: the game's own marker drawn again for
// each of them, in their colour: a triangle pointing where they face, with a pale rim, and the light of their camera
// fanning out from it. Their place is this player's arrow (as the game put it) moved by the distance between them, x east,
// y north (-z), in the map's units a metre (read from this player's own arrow) at the map's scale on the screen
ImU32 player_colour(int slot) {
    static const ImU32 kColours[kMaxPlayers] = {rgba(232, 176, 34), rgba(47, 111, 224), rgba(46, 168, 79), rgba(150, 82, 216)};
    return kColours[slot & 3];
}

// a fan from c: radius r, from angle a0 to a1 (screen radians), its colour from inner at the centre to outer at the rim
void light_fan(ImDrawList* d, ImVec2 c, float r, float a0, float a1, ImU32 inner, ImU32 outer) {
    const int n = 20;
    d->PrimReserve(n * 3, n + 2);
    const ImVec2 uv = d->_Data->TexUvWhitePixel;
    const ImDrawIdx base = (ImDrawIdx)d->_VtxCurrentIdx;
    d->PrimWriteVtx(c, uv, inner);
    for (int i = 0; i <= n; i++) {
        const float a = a0 + (a1 - a0) * (float)i / (float)n;
        d->PrimWriteVtx(ImVec2(c.x + r * cosf(a), c.y + r * sinf(a)), uv, outer);
    }
    for (int i = 0; i < n; i++) d->PrimWriteIdx(base), d->PrimWriteIdx((ImDrawIdx)(base + 1 + i)), d->PrimWriteIdx((ImDrawIdx)(base + 2 + i));
}

void draw_map_markers(Ui& u, const SessionView& v) {
    const MapArrow arrow = map_arrow();
    if (!arrow.shown || now_ns() - arrow.at_ns > 300000000LL) return;
    const LocalSnapshot me = local_snapshot();
    float cx = 0, cz = 0, width = 0;
    if (!me.valid || !map_capture(me.map, &cx, &cz, &width) || width <= 0) return;
    // (map units a metre: from this player's own arrow, its place on the map against his from the capture's centre; near
    // the centre, the last one found for this map and screen, or the Map screen's 2600 across the width)
    static char known_map[48];
    static float known_k[2] = {0, 0};
    const int which = arrow.paused ? 0 : 1;
    if (strcmp(known_map, me.map) != 0) snprintf(known_map, sizeof known_map, "%s", me.map), known_k[0] = known_k[1] = 0;
    const float dx = me.pos[0] - cx, dy = -(me.pos[2] - cz), dd = dx * dx + dy * dy;
    if (dd > 9.0f) {
        const float kk = (arrow.lx * dx + arrow.ly * dy) / dd;
        if (kk > 0.05f) known_k[which] = kk;
        else if (!arrow.paused) return;   // (a minimap that scrolls under a fixed arrow: no place known for the others)
    }
    const float k = known_k[which] > 0 ? known_k[which] : (arrow.paused ? 2600.0f / width : 0.0f);
    if (k <= 0) return;
    const float m = arrow.scale > 0.05f ? arrow.scale : 1.0f;      // (the map's scale on the layout)
    const float s = std::min(u.w / 3840.0f, u.h / 2160.0f);          // (pixels a layout unit)
    for (int i = 0; i < kMaxPlayers; i++) {
        const Player& p = v.players[i];
        if (!p.used || p.me || !(p.st.flags & kInPlay) || strcmp(p.st.map, me.map) != 0) continue;
        const float lx = arrow.x + (p.st.pos[0] - me.pos[0]) * k * m, ly = arrow.y - (p.st.pos[2] - me.pos[2]) * k * m;
        const ImVec2 c(u.w * 0.5f + lx * s, u.h * 0.5f - ly * s);
        if (c.x < -50 || c.y < -50 || c.x > u.w + 50 || c.y > u.h + 50) continue;
        const float alpha = std::clamp(arrow.alpha, 0.0f, 1.0f);
        const ImU32 col = player_colour(i);
        // (screen angles: world (sin yaw, cos yaw) on x, z: x right, z down the screen)
        const float face = atan2f(cosf(p.st.yaw), sinf(p.st.yaw));
        const float look = atan2f(cosf(p.st.cam_yaw), sinf(p.st.cam_yaw));
        light_fan(u.d, c, 300.0f * s * m, look - 0.74f, look + 0.74f, fade(rgba(255, 246, 205), 0.62f * alpha), rgba(255, 246, 205, 0));
        const float len = 78.0f * s * m;
        auto pt = [&](float along, float side) {
            return ImVec2(c.x + cosf(face) * along * len - sinf(face) * side * len, c.y + sinf(face) * along * len + cosf(face) * side * len);
        };
        const ImVec2 rim[3] = {pt(0.72f, 0), pt(-0.5f, 0.56f), pt(-0.5f, -0.56f)};
        const ImVec2 body[3] = {pt(0.52f, 0), pt(-0.36f, 0.41f), pt(-0.36f, -0.41f)};
        u.d->AddTriangleFilled(ImVec2(rim[0].x + s * 6, rim[0].y + s * 8), ImVec2(rim[1].x + s * 6, rim[1].y + s * 8),
                               ImVec2(rim[2].x + s * 6, rim[2].y + s * 8), fade(rgba(60, 30, 10), 0.35f * alpha));
        u.d->AddTriangleFilled(rim[0], rim[1], rim[2], fade(rgba(252, 240, 214), alpha));
        u.d->AddTriangleFilled(body[0], body[1], body[2], fade(col, alpha));
        if (arrow.paused) {   // (on the Map screen, their name under it; the minimap is too small for names)
            const float tp = std::max(12.0f, 46.0f * s);
            text_c(u, u.strong, tp, c.x, c.y + len * 0.62f, fade(rgba(70, 40, 20), alpha), p.name);
        }
    }
}

// ---- the other players on the world map (the Map screen's Y view): the game marks this player's area with its fairy at the
// left of the area's label; each other player gets a fairy of their own colour over the label of the area they are in (the
// last one known while they are inside a place the world map has no area for), their name over it, side by side when
// several are in the same area. Areas this player has not seen (in the fog) still show them.
void draw_fairy(ImDrawList* d, ImVec2 c, float r, ImU32 col, float alpha, float t) {
    for (int k = 4; k >= 1; k--) d->AddCircleFilled(c, r * (0.55f + 0.42f * (float)k), fade(col, 0.09f * alpha), 32);
    const float flap = 0.78f + 0.22f * sinf(t * 16.0f);
    const ImU32 wing = fade(rgba(255, 255, 255), 0.78f * alpha), rim = fade(col, 0.9f * alpha);
    for (int side = -1; side <= 1; side += 2) {
        const float sx = (float)side;
        const ImVec2 up(c.x + sx * r * 0.95f * flap, c.y - r * 0.42f), low(c.x + sx * r * 0.7f * flap, c.y + r * 0.45f);
        d->AddEllipseFilled(up, ImVec2(r * 0.78f * flap, r * 0.46f), rim, sx * -0.55f, 24);
        d->AddEllipseFilled(up, ImVec2(r * 0.64f * flap, r * 0.34f), wing, sx * -0.55f, 24);
        d->AddEllipseFilled(low, ImVec2(r * 0.5f * flap, r * 0.3f), rim, sx * 0.55f, 20);
        d->AddEllipseFilled(low, ImVec2(r * 0.38f * flap, r * 0.2f), wing, sx * 0.55f, 20);
    }
    d->AddCircleFilled(c, r * 0.58f, fade(col, alpha), 32);
    d->AddCircleFilled(ImVec2(c.x - r * 0.12f, c.y - r * 0.12f), r * 0.28f, fade(rgba(255, 255, 255), 0.95f * alpha), 24);
}

void draw_world_markers(Ui& u, const SessionView& v) {
    static std::string last_area[kMaxPlayers];
    for (int i = 0; i < kMaxPlayers; i++) {   // (each player's area kept up to date while the world map is not up too)
        const Player& p = v.players[i];
        if (!p.used) {
            last_area[i].clear();
            continue;
        }
        if (const char* a = world_area_of(p.st.map)) last_area[i] = a;
    }
    const WorldMap w = world_map();
    if (!w.shown || now_ns() - w.at_ns > 300000000LL) return;
    const float s = std::min(u.w / 3840.0f, u.h / 2160.0f);   // (pixels a layout unit)
    const float m = std::clamp(w.scale, 0.4f, 3.0f);
    const float alpha = std::clamp(w.alpha, 0.0f, 1.0f);
    const float t = (float)(now_ns() / 1e9);
    int in_area[64] = {0};
    for (int i = 0; i < kMaxPlayers; i++) {
        const Player& p = v.players[i];
        if (!p.used || p.me || last_area[i].empty()) continue;
        int at = -1;
        for (int k = 0; k < w.count && at < 0; k++)
            if (last_area[i] == w.areas[k].name) at = k;
        if (at < 0) continue;
        const int nth = in_area[at]++;
        const float spread = (nth == 0 ? 0.0f : (nth % 2 ? 1.0f : -1.0f) * (float)((nth + 1) / 2)) * 150.0f;
        const float bob = sinf(t * 2.6f + (float)i * 1.7f) * 10.0f;
        const float lx = w.areas[at].x + spread * m, ly = w.areas[at].y + (150.0f + bob) * m;
        const ImVec2 c(u.w * 0.5f + lx * s, u.h * 0.5f - ly * s);
        if (c.x < -80 || c.y < -80 || c.x > u.w + 80 || c.y > u.h + 80) continue;
        const ImU32 col = player_colour(i);
        draw_fairy(u.d, c, 50.0f * s * m, col, alpha, t + (float)i);
        // (the name on a parchment pill over the fairy)
        const float tp = std::max(12.0f, 40.0f * s * std::min(m, 1.4f));
        const float tw = text_w(u.strong, tp, p.name);
        const float ny = c.y - 50.0f * s * m * 1.9f - tp;
        u.d->AddRectFilled(ImVec2(c.x - tw * 0.5f - tp * 0.45f, ny - tp * 0.18f), ImVec2(c.x + tw * 0.5f + tp * 0.45f, ny + tp * 1.12f),
                           fade(rgba(252, 240, 214), 0.88f * alpha), tp * 0.5f);
        u.d->AddRect(ImVec2(c.x - tw * 0.5f - tp * 0.45f, ny - tp * 0.18f), ImVec2(c.x + tw * 0.5f + tp * 0.45f, ny + tp * 1.12f),
                     fade(col, 0.9f * alpha), tp * 0.5f, 0, std::max(1.5f, 3.0f * s * m));
        text_c(u, u.strong, tp, c.x, ny, fade(rgba(70, 40, 20), alpha), p.name);
    }
}

// (this player down: the picture darkened red at its edges, the banner, the time left, the give-up button)
void draw_downed(Ui& u) {
    if (!fellow_downed()) return;
    const float t = (float)(now_ns() / 1e9);
    const float beat = 0.5f + 0.5f * sinf(t * 3.2f);
    const float e = 150 * u.s;
    const ImU32 edge = fade(rgba(120, 0, 0), 0.55f + 0.2f * beat), none = rgba(120, 0, 0, 0);
    u.d->AddRectFilled(ImVec2(0, 0), ImVec2(u.w, u.h), rgba(30, 0, 0, 70));
    u.d->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(u.w, e), edge, edge, none, none);
    u.d->AddRectFilledMultiColor(ImVec2(0, u.h - e), ImVec2(u.w, u.h), none, none, edge, edge);
    u.d->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(e, u.h), edge, none, none, edge);
    u.d->AddRectFilledMultiColor(ImVec2(u.w - e, 0), ImVec2(u.w, u.h), none, edge, edge, none);
    const float cx = u.w * 0.5f, y = u.h * 0.2f;
    const float hp = 40 * u.s;
    text_c(u, u.heading, hp, cx, y, fade(kDown, 0.85f + 0.15f * beat), "YOU ARE DOWN");
    text_c(u, u.body, 18 * u.s, cx, y + hp * 1.25f, kWhite, "Wait here: another player can revive you");
    // (the time left)
    const float left = fellow_downed_left(), frac = std::clamp(left / 120.0f, 0.0f, 1.0f);
    const float bw = 360 * u.s, bh = 10 * u.s, by = y + hp * 1.25f + 34 * u.s;
    u.d->AddRectFilled(ImVec2(cx - bw * 0.5f - 2 * u.s, by - 2 * u.s), ImVec2(cx + bw * 0.5f + 2 * u.s, by + bh + 2 * u.s), kDark, 5 * u.s);
    u.d->AddRectFilled(ImVec2(cx - bw * 0.5f, by), ImVec2(cx - bw * 0.5f + bw * frac, by + bh), kDown, 4 * u.s);
    char secs[32];
    snprintf(secs, sizeof secs, "%d s", (int)ceilf(left));
    text_c(u, u.body, 15 * u.s, cx, by + bh + 6 * u.s, kDim, secs);
    // (give up: held)
    const float gp = fellow_giveup_progress();
    const float pw = prompt(u, cx - 110 * u.s, u.h * 0.78f, "-", "Hold to give up (back to the entrance)");
    if (gp > 0) {
        const float x0 = cx - 110 * u.s, y0 = u.h * 0.78f + 16 * u.s;
        u.d->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + (pw - 22 * u.s) * gp, y0 + 4 * u.s), kWarn, 2 * u.s);
    }
}

// (beside a fallen player: hold A to revive them)
void draw_revive_prompt(Ui& u, const SessionView& v) {
    float progress = 0;
    const int target = fellow_revive_target(&progress);
    if (target < 0 || target >= kMaxPlayers || !v.players[target].used) return;
    const std::string word = "Hold to revive " + v.players[target].name;
    const float cx = u.w * 0.5f, y = u.h * 0.70f;
    const float tw = text_w(u.body, 16 * u.s, word) + 6 * u.s + 20 * u.s;
    box(u, ImVec2(cx - tw * 0.5f - 18 * u.s, y - 22 * u.s), ImVec2(cx + tw * 0.5f + 18 * u.s, y + 24 * u.s), 0.8f);
    prompt(u, cx - tw * 0.5f, y, "A", word.c_str());
    const float bw = tw;
    u.d->AddRectFilled(ImVec2(cx - bw * 0.5f, y + 13 * u.s), ImVec2(cx + bw * 0.5f, y + 17 * u.s), kHeartDark, 2 * u.s);
    if (progress > 0) u.d->AddRectFilled(ImVec2(cx - bw * 0.5f, y + 13 * u.s), ImVec2(cx - bw * 0.5f + bw * progress, y + 17 * u.s), kGood, 2 * u.s);
}

}  // namespace

// Only when the mod has something to draw: otherwise the renderer keeps its own path (no ImGui frame, no extra pass)
void overlay_hit_test(int slot) {
    if (slot >= 0 && slot < kMaxPlayers) g_hit_test[slot] = true;
}

bool overlay_wanted() {
    if (dialogs_open() || menus_modal()) return true;
    if (fellow_downed() || fellow_revive_target(nullptr) >= 0) return true;
    const int save = game_save_state();
    if (save == kSaveAsked || save == kSaveRunning) return true;
    Session& ses = Session::get();
    if (net::now_ms() - ses.last_toast_ms() < 5200) return true;
    const Page p = menus_page();
    if (p == Page::Loading || p == Page::AutoLoad) return true;
    if (title::active() && p != Page::Title) return true;
    if (game_in_play() && ses.others_present()) return true;   // (the players' list and the nameplates)
    if (map_arrow().shown && ses.others_present()) return true;   // (the other players on the area map)
    if (ses.others_present()) {   // (on the world map)
        const WorldMap w = world_map();
        if (w.shown && now_ns() - w.at_ns < 300000000LL) return true;
    }
    return false;
}

// (the game saving: its own kind of mark at the bottom right, a ring of gold dots turning, and the word)
void draw_saving(Ui& u) {
    const int st = game_save_state();
    if (st != kSaveAsked && st != kSaveRunning) return;
    const float t = (float)(now_ns() / 1e9);
    const float cx = u.w - 46 * u.s, cy = u.h - 46 * u.s;
    for (int i = 0; i < 10; i++) {
        const float ang = i / 10.0f * 6.2832f + t * 3.5f;
        const float k = 0.25f + 0.75f * (float)((i + (int)(t * 10)) % 10) / 9.0f;
        u.d->AddCircleFilled(ImVec2(cx + cosf(ang) * 14 * u.s, cy + sinf(ang) * 14 * u.s), 2.6f * u.s, fade(kGoldLine, k), 10);
    }
    text_r(u, u.strong, 17 * u.s, cx - 26 * u.s, cy - 10 * u.s, kWhite, "Saving...");
}

void overlay_draw(ImGuiContext* ctx, uint32_t w, uint32_t h) {
    if (!ctx) return;
    ImGui::SetCurrentContext(ctx);
    const VkFonts& f = rt().ui_fonts();
    Ui u{ImGui::GetForegroundDrawList(), f.body, f.strong ? f.strong : f.body, f.heading ? f.heading : f.body, h / 720.0f, (float)w, (float)h};
    if (!u.body) return;
    const MenuView mv = menus_view();
    const SessionView v = Session::get().view();
    const GuestSettings gs = Session::get().guest_settings();
    const bool on_title = title::active();
    if (on_title) {
        switch (mv.page) {
        case Page::Online: draw_online_info(u); break;
        case Page::Join: draw_join(u); break;
        case Page::HostLobby:
        case Page::GuestLobby: draw_lobby(u, v, mv); break;
        default: break;
        }
        if (mv.page != Page::Title && mv.page != Page::Loading && mv.page != Page::AutoLoad && mv.page != Page::PickFile)
            prompts_br(u, {{"A", "OK"}, {"B", "Back"}});
    }
    if (mv.page == Page::Loading || mv.page == Page::AutoLoad) {
        MenuView lv = mv;
        if (mv.page == Page::AutoLoad) lv.loading_text = "Starting the game...";
        draw_loading(u, lv);
    }
    if (v.role != Role::None) draw_map_markers(u, v), draw_world_markers(u, v);
    if (game_in_play() && v.role != Role::None) {
        draw_hud(u, v, gs);
        draw_downed(u);
        draw_revive_prompt(u, v);
    }
    // dialogs over everything
    const TextEdit e = menus_text_edit();
    const DialogsView dv = dialogs_view();
    if (e.open) {
        draw_keyboard(u, e);
    } else if (dv.kind == kDlgSettings) {
        draw_list_dialog(u, dv, 600, true);
    } else if (dv.kind == kDlgItemMenu) {
        draw_list_dialog(u, dv, 380, false);
    } else if (dv.kind == kDlgInvite || dv.kind == kDlgIngame || dv.kind == kDlgGiveItem || dv.kind == kDlgGivePlayer ||
               dv.kind == kDlgJoin) {
        draw_list_dialog(u, dv, 520, true);
    } else if (dv.kind == kDlgGiftOffer) {
        draw_list_dialog(u, dv, 520, false);
    } else if (dv.kind == kDlgGiftAmount) {
        draw_amount(u, dv);
    } else if (dv.kind == kDlgWaiting) {
        draw_waiting(u, dv);
    }
    draw_saving(u);
    draw_toasts(u, gs.sounds);
}

}  // namespace coop
