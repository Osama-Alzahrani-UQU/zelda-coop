#pragma once

#include "common.h"

namespace coop {

struct LocalSnapshot {
    bool valid = false;
    uint64_t player = 0, mover = 0;
    float pos[3] = {0, 0, 0};
    float yaw = 0;
    int life = -1, life_max = -1;
    bool child = false;
    char map[48] = {0};
    int64_t at_ns = 0;
};
LocalSnapshot local_snapshot();

// The gameplay camera as last built: world to view (3 x 4, rows) and the vertical angle, for the nameplates
struct CameraView {
    bool valid = false;
    float m[12] = {0};
    float fovy = 0.8f;
    int64_t at_ns = 0;
};
CameraView camera_view();

// The area map's own player arrow as the Map screen shows it: its place on the screen's layout (3840 x 2160, the centre
// at 0, y up), its alpha (shown: the area map is up)
struct MapArrow {
    bool shown = false;
    bool paused = false;              // (the Map screen, the game paused; else the minimap of play)
    float x = 0, y = 0, alpha = 0;    // (on the layout)
    float scale = 1;                  // (the map's scale on the layout)
    float lx = 0, ly = 0;             // (on the map: the capture's centre at 0)
    int64_t at_ns = 0;
};
MapArrow map_arrow();
// An area map's capture: its centre (x, z) and the width its picture covers in metres; false when the map has no player
// marker (or is not known)
bool map_capture(const char* map, float* cx, float* cz, float* width);

// The world map (the Map screen's other view, its Y): each area's place on the screen's layout (3840 x 2160, the centre at
// 0, y up) as the game puts it there (the centre of its name's label; fogged areas too), the map's scale (its zoom);
// shown: the world map is up
struct WorldMap {
    bool shown = false;
    float scale = 1, alpha = 0;
    struct Area {
        char name[24];   // (its part's name without Pa_ and _00: KokiriForest, Field, Village...)
        float x, y;
    } areas[40];
    int count = 0;
    int64_t at_ns = 0;
};
WorldMap world_map();
// (the world map's area a map is in: its prefix's (an inside place under its area's name); null when not known)
const char* world_area_of(const char* map);

// The pouch, on a game thread with its context (Link's update, a menu's): one content's count by its actor name (-1: not
// ready), and an add (delta > 0) or a removal (the event's status, -1 not ready)
int pouch_count(Ctx* c, const char* name);
int item_count(Ctx* c, const char* name);   // (a pouch content's count; "Rupees": the wallet's)
int item_change(Ctx* c, const char* name, int delta);   // (a pouch content added or removed; "Rupees": the wallet's)
// An AS (animation sequence) played on an actor by its name (Link's: AS/Player_Child.root.asb); keep: nothing done when it
// is already the one playing. On a game thread with its context.
bool game_play_as(Ctx* c, uint64_t actor, const char* name, float rate, bool keep);
// the AS an actor plays now (its AS controller's first slot: components +0x40 -> +0x18 -> +0x18, slots at +0x20, a node's name
// at +0x10); empty when it has none
std::string game_actor_anim(uint64_t actor);
void game_force_as(const std::string& name, int frames);   // (developer test: played on Link each frame for a while)
int pouch_change(Ctx* c, const char* name, int delta);

}  // namespace coop
