# Changelog

All versions of Longshot Co-op by [Osama-Alzahrani-UQU](https://github.com/Osama-Alzahrani-UQU).

## 1.1.0 Beta (2026-11-17)

### Hosting and joining
- **Host Game** now offers **Continue**, **New Game** and **Choose a File**, and starts the game at once (no lobby to wait
  in; friends join while you play).
- **Guests go straight into the host's world**: the host's save is sent over the network and loaded; a guest no longer
  opens their own save list.
- Each guest's **own items, rupees and bags** are kept by the host and given back on every join.
- A guest's **own saves are set aside** while they play in the host's world and put back when they leave (at the title,
  at the next start after a crash, or on Uninstall).
- Faster automatic loading.
- A host refuses a friend on a different mod version with a clear message.

### Enemies
- Shared **kills are real deaths** in every game, with each game's **own loot** (they used to stand at 0 life in some
  games).
- Shared **hits are real blows**: the enemy reacts to the other player's hits in every game.
- Kills find the same enemy by where the map placed it.

### World
- **Live grass, pots, eye switches and burnable webs**: cut or struck in one game, in every game at once, with their own
  drops.

### Items and gifts
- Gifts of Deku Sticks, Nuts, Seeds, Bombs and Arrows now work for new players (the bag comes first).
- **Give To is faster**: item, amount, player; the only other player is chosen for you; no second question.
- **Rupees can be given** to another player.

### Interface
- Removed the old drawn figure of the other player that could flash during transitions.
- Steadier online HUD and markers (no pulsing).
- The pause menu no longer stops the world for everyone while playing together.

### Performance
- New **Co-op Frame Generation** option (Online Settings), made for NVIDIA cards: the renderer's frame generation at 2x,
  paced on a present thread of its own, one frame queued for less input delay. Only with the game's own Frame
  generation Off.

### Setup
- New Setup window with full credits, a built-in English guide and links.
- Uninstall puts back saves set aside during an online session before removing the mod, and also removes the items kept
  for guests when settings are removed.

## 1.0.0 Beta
- First public release: hosting and joining over Radmin VPN, the other players as Link with their pose and gear, shared
  enemies and kills, shared world objects, gifts, revive, area and world map markers.
