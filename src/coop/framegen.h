// Co-op Frame Generation: the renderer's own frame generation, set up by the mod for smooth, low-delay play (tuned on
// NVIDIA cards, where the game's DLSS Frame Generation needs an RTX 40 or newer and its FSR path runs instead).
//
// The renderer (bin\longshot_vk.dll) reads a few settings of its own from the environment when it starts: the frame
// generation's multiple (RX_VK_FRAMEGEN_MULT, 2 to 8), frame pacing (RX_VK_PACE), each present made on a thread and queue of
// its own once its copy is done (RX_VK_PRESENT_THREAD) and how many presents may wait ahead of the screen (RX_VK_PRESENTS_AHEAD,
// 2 unless set). Co-op Frame Generation sets them before the renderer starts: two frames for each one the game draws, the
// frames paced evenly by a present thread of their own, and one present ahead instead of two (one frame less of input
// delay). It is only used while the game's own Frame generation option is Off, so the two never run together.
#pragma once

namespace coop {

// the game's own Frame generation option (longshot_options.ini frame_generation): 0 off, more on, -1 not known
int game_framegen_option();
// (the mod's start, before the renderer starts) Co-op Frame Generation set up when it is on and the game's own is Off
void coop_framegen_apply();
// (this run) Co-op Frame Generation is running
bool coop_framegen_running();

}  // namespace coop
