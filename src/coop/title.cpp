// The title menu's buttons and the Load screen, driven on the game's threads.
//
// The title screen (UITitleScreen, vtable 0x84c106e8; its layout Title_00 in UI/Layout2Archive/Title) has four button parts,
// Pa_Button_00..03, lined up by the alignment pane A_BtnAlign_00. Its animation "BtnNum" shows as many of them as its frame
// (the game sets 3: Continue, New Game, Options). The fourth has no text in the layout: the mod shows it (BtnNum at frame 4)
// with the text "Online", and the game lays it out, focuses, lights and animates it as its own. For the mod's pages the
// four buttons get the page's texts (each button's text boxes: T_Btn_00..02, its states, and T_BtnSh_00, the shadow) and
// BtnNum the page's count; the game's texts are put back for its own page. Screen fields: +0x28 the layout, +0x2c8 the
// buttons' count, +0x2d0 their array, +0x2e0 the focused one; a button's +56 is non-zero while it has the focus.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

#include "coop.h"
#include "runtime.h"
#include "title.h"

namespace coop {

namespace {
constexpr uint64_t kTitleScreenVt = 0x84c106e8ULL;
constexpr uint64_t kLoadScreenVt = 0x84c0eba0ULL;
constexpr uint64_t kFnLayoutAnim = 0x8045c7a0ULL;     // (layout, name) -> its animator (a dummy when there is none)
constexpr uint64_t kFnAnimToEnd = 0x802f0f00ULL;      // (animator): at its last frame, bound to the layout
constexpr uint64_t kFnAnimFromStart = 0x802f0fe0ULL;  // (animator): from its first frame, playing
constexpr uint64_t kFnAnimSetFrame = 0x802f1350ULL;   // (controller, s0 frame)
constexpr uint64_t kFnLayoutPane = 0x8045e1e0ULL;     // (layout, "Part/Pane") -> the pane's handle
constexpr uint64_t kFnSetFocus = 0x8088ffc0ULL;       // (screen, button, 0): the focus manager's focus on the button
constexpr uint64_t kFnPlaySound = 0x80ac65d0ULL;      // (slink user, name; x8 the result): a UI sound
constexpr uint64_t kDummyAnim = 0x84df2270ULL;
constexpr int kBoxes = 4;                             // T_Btn_00, T_Btn_01, T_Btn_02, T_BtnSh_00

struct BoxText {
    uint64_t box = 0, text = 0;
    uint32_t len = 0;
};

std::mutex g_mutex;
uint64_t g_screen = 0, g_anim = 0;
BoxText g_orig[4][kBoxes];      // the boxes (found at attach); .text/.len: the game's texts as they were when a page of the
                                // mod's replaced them, put back for the game's own page
std::vector<uint64_t> g_ours;   // (the guest texts the mod wrote: a box holding one of these shows the mod's text)
uint64_t g_buttons[4] = {0, 0, 0, 0};
bool g_failed = false;
// (wanted by the menus, applied at the title's update)
std::vector<std::string> g_want_items;
bool g_want_native = true;
int g_want_focus = 0;
uint32_t g_want_serial = 1, g_applied_serial = 0;
std::string g_want_sound;
int g_want_focus_only = -1;
// (what the menus read)
std::atomic<int> g_focused{-1}, g_count{0};
std::atomic<bool> g_native{true};
std::atomic<int64_t> g_seen_ns{0};
std::atomic<int> g_state{0};

// the Load screen
std::mutex g_load_mutex;
std::atomic<int64_t> g_load_seen{0};
std::atomic<int> g_load_row{-1};
int g_load_want_row = -1;

uint64_t text_box(uint64_t handle) {
    const uint64_t pane = handle ? r64(handle + 8) : 0;
    return pane ? r64(pane + 8) : 0;
}

void set_box_text(uint64_t box, uint64_t text, uint32_t len) {
    if (!box || (r64(box + 0xf0) == text && r32(box + 0x124) == len)) return;
    w64(box + 0xf0, text);
    w32(box + 0x124, len);
    // (laid out and drawn anew, as the bits the word after the status allows: host/prompts.c mark_box)
    const uint64_t status = r64(box + 8);
    if (status) {
        const uint32_t word = r32(status), mask = r32(status + 4);
        w32(status, word | ((0x20000u | 0x100000u) & mask));
    }
}

float anim_frame(uint64_t anim) {
    const uint64_t ctl = r64(anim + 0x20);
    return ctl ? rf(ctl + 0x18) : -1;
}

// the BtnNum animator at frame n (n buttons shown)
void show_count(Ctx* c, int n) {
    if (std::fabs(anim_frame(g_anim) - (float)n) < 0.25f) return;
    if (n >= 4) {
        guest_call(c, kFnAnimToEnd, g_anim);
        return;
    }
    // (bound from its first frame, then held at n: the controller's frame set, as play-from-end does at its end)
    guest_call(c, kFnAnimFromStart, g_anim);
    const uint64_t ctl = r64(g_anim + 0x20);
    if (!ctl) return;
    Ctx saved;
    memcpy(&saved, c, sizeof saved);
    const float f = (float)n;
    memcpy(&c->vreg[0][0], &f, 4);
    A64Fn fn = rt().a64_lookup(kFnAnimSetFrame);
    if (fn) {
        c->x[0] = ctl, c->x[30] = 0;
        fn(c);
    }
    memcpy(c, &saved, sizeof saved);
}

void play_sound(Ctx* c, uint64_t screen, const std::string& name) {
    const uint64_t holder = r64(screen + 0x198);
    const uint64_t user = holder ? r64(holder + 8) : 0;
    if (!user) return;
    guest_call_frame(c, kFnPlaySound, 0x80, nullptr, nullptr, nullptr, 0x40, user, guest_string(name));
}

bool attach(Ctx* c, uint64_t screen) {
    const uint64_t layout = r64(screen + 0x28);
    if (!layout || r32(screen + 0x2c8) != 4) {
        if (!g_failed) log("title: the title screen's buttons are not the four this mod knows (%u): no Online entry", r32(screen + 0x2c8));
        g_failed = true;
        return false;
    }
    g_anim = guest_call(c, kFnLayoutAnim, layout, guest_string("BtnNum"));
    if (!g_anim || g_anim == kDummyAnim) {
        if (!g_failed) log("title: no BtnNum animation: no Online entry");
        g_failed = true;
        return false;
    }
    static const char* const parts[kBoxes] = {"T_Btn_00", "T_Btn_01", "T_Btn_02", "T_BtnSh_00"};
    const uint64_t buttons = r64(screen + 0x2d0);
    for (int b = 0; b < 4; b++) {
        g_buttons[b] = buttons ? r64(buttons + 8 * (uint64_t)b) : 0;
        for (int k = 0; k < kBoxes; k++) {
            char path[64];
            snprintf(path, sizeof path, "Pa_Button_%02d/%s", b, parts[k]);
            BoxText& o = g_orig[b][k];
            o.box = text_box(guest_call(c, kFnLayoutPane, layout, guest_string(path)));
            o.text = 0, o.len = 0;   // (the game sets its texts as the menu comes in: taken when a page replaces them)
        }
    }
    if (!g_orig[3][0].box) {
        if (!g_failed) log("title: the fourth button's text not found: no Online entry");
        g_failed = true;
        return false;
    }
    g_screen = screen;
    g_applied_serial = 0;   // (the wanted page applied anew to this screen)
    log("title: attached to the title screen %#llx", (unsigned long long)screen);
    return true;
}

bool is_ours(uint64_t text) { return std::find(g_ours.begin(), g_ours.end(), text) != g_ours.end(); }

uint64_t our_text(const std::string& t) {
    const uint64_t g = guest_string(t);
    if (g && !is_ours(g)) g_ours.push_back(g);
    return g;
}

void apply_page(Ctx* c) {
    if (g_want_native) {
        // (the game's three: left as the game has them; a box still showing the mod's text gets the game's back)
        for (int b = 0; b < 3; b++)
            for (int k = 0; k < kBoxes; k++) {
                BoxText& o = g_orig[b][k];
                if (o.box && is_ours(r64(o.box + 0xf0)) && o.text) set_box_text(o.box, o.text, o.len);
            }
        const std::string online = "Online";
        for (int k = 0; k < kBoxes; k++) set_box_text(g_orig[3][k].box, our_text(online), (uint32_t)online.size());
        show_count(c, 4);
        g_count = 4;
    } else {
        const int n = (int)std::min<size_t>(4, g_want_items.size());
        for (int b = 0; b < n; b++) {
            const std::string& t = g_want_items[(size_t)b];
            for (int k = 0; k < kBoxes; k++) {
                BoxText& o = g_orig[b][k];
                if (!o.box) continue;
                const uint64_t cur = r64(o.box + 0xf0);
                if (!is_ours(cur)) o.text = cur, o.len = r32(o.box + 0x124);   // (the game's, kept for its page)
                set_box_text(o.box, our_text(t), (uint32_t)t.size());
            }
        }
        show_count(c, std::max(1, n));
        g_count = n;
    }
    g_native = g_want_native;
}

int focused_index(uint64_t screen) {
    const uint64_t f = r64(screen + 0x2e0);
    for (int b = 0; b < 4; b++)
        if (g_buttons[b] && g_buttons[b] == f) return b;
    return -1;
}

// ---- the Load screen: its rows (buttons in the screen's entry list), ordered top first by their place on screen
struct Row {
    uint64_t button;
    float y;
};
int load_rows(uint64_t screen, Row* rows, int max) {
    const uint64_t head = screen + 136;
    const int32_t off = (int32_t)r32(screen + 156);
    uint64_t node = r64(screen + 144);
    int n = 0;
    for (int guard = 0; node && node != head && guard < 64 && n < max; guard++) {
        const uint64_t entry = node - (uint64_t)(int64_t)off;
        const uint64_t button = r64(entry + 24);
        // (the button's hit box: its global matrix at box+0x28, the translation's y at +0x50: host/oot_menu.c button_rect)
        const uint64_t box = r64(button + 0x58);
        if (button && box) rows[n++] = Row{button, rf(box + 0x50)};
        node = r64(node + 8);
    }
    std::sort(rows, rows + n, [](const Row& a, const Row& b) { return a.y > b.y; });
    return n;
}

void load_screen_update(Ctx* c, uint64_t screen) {
    Row rows[32];
    const int n = load_rows(screen, rows, 32);
    int focus = -1;
    for (int i = 0; i < n; i++)
        if (r8(rows[i].button + 56)) focus = i;
    g_load_row = focus;
    g_load_seen = now_ns();
    int want;
    {
        std::lock_guard<std::mutex> lock(g_load_mutex);
        want = g_load_want_row;
        g_load_want_row = -1;
    }
    if (want >= 0 && want < n && want != focus) guest_call(c, kFnSetFocus, screen, rows[want].button, 0);
}
}  // namespace

void title_screen_update(Ctx* c, uint64_t screen) {
    const uint64_t vt = r64(screen);
    if (vt == kLoadScreenVt) {
        load_screen_update(c, screen);
        return;
    }
    if (vt != kTitleScreenVt) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (screen != g_screen) {
        g_failed = false;
        if (!attach(c, screen)) return;
    }
    if (g_applied_serial != g_want_serial || std::fabs(anim_frame(g_anim) - (float)std::max(1, g_count.load())) > 0.25f) {
        const bool first = g_applied_serial != g_want_serial;
        apply_page(c);
        if (first) {
            g_applied_serial = g_want_serial;
            if (g_want_focus >= 0 && g_want_focus < 4 && g_buttons[g_want_focus])
                guest_call(c, kFnSetFocus, screen, g_buttons[g_want_focus], 0);
        }
    } else {
        // (the texts kept: the game may lay a box out from its own data again)
        apply_page(c);
    }
    if (g_want_focus_only >= 0 && g_want_focus_only < 4 && g_buttons[g_want_focus_only]) {
        guest_call(c, kFnSetFocus, screen, g_buttons[g_want_focus_only], 0);
        g_want_focus_only = -1;
    }
    if (!g_want_sound.empty()) {
        play_sound(c, screen, g_want_sound);
        g_want_sound.clear();
    }
    const int fi = focused_index(screen);
    if (fi != g_focused.load()) log("title: focus %d", fi);
    {   // (the screen's state byte: 1 while its menu takes input; logged as it changes, for tests and reports)
        static int last_state = -1;
        const int st = r8(screen + 0x1a8);
        if (st != last_state) log("title: state %d", st), last_state = st;
        g_state = st;
    }
    g_focused = fi;
    g_seen_ns = now_ns();
}

namespace title {
bool active() { return now_ns() - g_seen_ns.load() < 200000000LL; }
void show_page(const std::vector<std::string>& items, int focus) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_want_native = items.empty();
    g_want_items = items;
    g_want_focus = items.empty() ? focus : std::min<int>(focus, (int)items.size() - 1);
    g_want_serial++;
}
bool native() { return g_native.load(); }
int focused() { return active() ? g_focused.load() : -1; }
int count() { return g_count.load(); }
bool online_focused() { return active() && g_native.load() && g_focused.load() == 3; }
bool menu_ready() { return active() && g_state.load() == 1; }
void sound(const char* name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_want_sound = name;
}
void focus(int index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_want_focus_only = index;
}
}  // namespace title

namespace loadscreen {
bool open() { return now_ns() - g_load_seen.load() < 200000000LL; }
int focused_row() { return open() ? g_load_row.load() : -1; }
void select_row(int row) {
    std::lock_guard<std::mutex> lock(g_load_mutex);
    g_load_want_row = row;
}
}  // namespace loadscreen

}  // namespace coop
