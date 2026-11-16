// Link's body as each player animates it: the skeleton pose of this game's Link, read after the game animated it, is sent
// to the others; each other player's Link (the puppet) shows the pose its player sent in place of its own.
//
//   the model:  an actor's components *(+0x270): the model component at [1] (its model *(+0x18) when its vtable's +0xf8 is
//               the plain getter f_809f7160), whose units *(model + 0x28): unit [0] (vtable 0x84c28cc8) has the skeleton's
//               state at *(unit + 0xf8): the FSKL resource at +0, the local bones at *(+0x18) (0x60 a bone: flags, scale at
//               +0x10, the rotation and translation as four columns at +0x20) and the world bones at *(+0x20) (four columns,
//               0x40 a bone)
//   the frame:  the game animates the local bones (its AS), makes the world ones from them, then builds the camera's lens
//               (hos_oot_fov) and draws. At the lens the local bones are this frame's (read for this Link) and the world
//               ones are what is drawn (made again for the puppets from the pose sent: world = parent's world without its
//               scale x [rotation x scale | translation x parent's scale], as the game makes them)
#pragma once

#include <cstdint>
#include <vector>

#include "common.h"

namespace coop {

// (a camera's lens being built, on the game's thread: this Link's pose read and sent, the puppets' poses put on them)
void pose_frame(Ctx* c, int64_t now);

// (developer test: this Link's own pose shown on the puppets too, as if they had sent it)
void pose_mirror(bool on);

// ---- any actor's pose, every bone (the shared enemies: enemies.cpp)
bool pose_capture_actor(uint64_t actor, std::vector<uint8_t>* blob);   // (its bones as its game animated them)
// (the pose another game sent for this game's copy of an actor: drawn at the lenses while it keeps coming)
void pose_show_actor(uint64_t actor, const std::vector<uint8_t>& blob, int64_t now);

}  // namespace coop
