#include "framegen.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common.h"
#include "config.h"

namespace coop {

namespace {
bool g_running = false;
}  // namespace

int game_framegen_option() {
    if (const char* e = getenv("LONGSHOT_COOP_GAME_FG")) return atoi(e);   // (tests: the game's option as if set so)
    FILE* f = _wfopen((game_dir() + L"\\longshot_options.ini").c_str(), L"rb");
    if (!f) return -1;
    char line[256];
    int value = -1;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "frame_generation=", 17)) {
            value = atoi(line + 17);
            break;
        }
    }
    fclose(f);
    return value;
}

void coop_framegen_apply() {
    if (!config().guest.coop_fg) return;
    const int game = game_framegen_option();
    if (game != 0) {
        log("framegen: Co-op Frame Generation is on but the game's own Frame generation is %s: the game's is used",
            game < 0 ? "not known" : "on");
        return;
    }
    // (the renderer's own knobs, read once as it starts: the C runtime's environment it reads them from (UCRT getenv))
    struct Knob {
        const char* name;
        const char* value;
    };
    static const Knob kKnobs[] = {
        {"RX_VK_FRAMEGEN", "1"},          // (allowed)
        {"RX_VK_FRAMEGEN_MULT", "2"},     // (two frames for each drawn: the smoothest with the least delay)
        {"RX_VK_PACE", "1"},              // (frame pacing on: the frames shown evenly)
        {"RX_VK_PRESENT_THREAD", "1"},    // (each present on a thread and queue of its own: no present waits on the next frame's work)
        {"RX_VK_PRESENTS_AHEAD", "1"},    // (one present ahead of the screen, not two: a frame less of input delay)
    };
    for (const Knob& k : kKnobs) {
        if (getenv(k.name)) {   // (one the player set: theirs)
            log("framegen: %s=%s kept (set before the game started)", k.name, getenv(k.name));
            continue;
        }
        _putenv_s(k.name, k.value);
    }
    g_running = true;
    log("framegen: Co-op Frame Generation on (2x, paced, its own present thread, one present ahead)");
}

bool coop_framegen_running() { return g_running; }

}  // namespace coop
