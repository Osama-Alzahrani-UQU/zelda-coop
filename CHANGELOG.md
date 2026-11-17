# Changelog

All versions of Longshot Co-op by [Osama-Alzahrani-UQU](https://github.com/Osama-Alzahrani-UQU).

## 1.1.2 Beta (2026-11-18)

### Enemies
- **Enemy attacks are seen by everyone**: the animation an enemy plays in the game running it (its attack, its jump, its
  reaction to a hit, its fall, getting up) is played on the same enemy in the other players' games, with the effects and
  sounds the game ties to those animations. Before, the other games showed the enemy's moves but left it in its own state
  there (an Octorok attacking your friend stayed asleep under the water in your game).
- Kills made in another player's game now reach yours within a tenth of a second.
- Both players need this version: a host refuses a friend with another one.

### Known limits
- Shots fired by enemies (Octorok rocks, Deku Scrub nuts) are still seen only in the game of the player they are fired
  at; the enemy's shooting animation is seen by everyone.

## 1.1.1 Beta (2026-11-17)

### Fixes
- **Guests really load the host's world**: the host's save is written into the save slot the guest's game lists first
  (the game keeps its list of saves from when it starts), so the guest no longer ends up in their own save.
- **No more invisible or doubled enemies**: the stand-in enemies (made when another game had an enemy this game had not)
  are gone; they were unseen, kept fighting and came back. The same enemy is now paired in every game by where the map
  placed it.
- **Every enemy is shared now**: enemies whose life the game keeps another way were never shared before (Bari, Biri,
  Shabom, Tektites, Wallmaster, the Lost Woods stage Scrubs); their moves, hits and deaths now reach every game, the Biri
  a Bari splits into included.
- **No enemy left out in a full room**: every enemy near another player is sent (the nearest each moment, the others in
  turn, each one several times a second); before, only the 8 nearest were.
- A death the game makes without its usual damage step is now caught and shared too.
- Room for every player's enemies (up to 256 at once): with 3 or 4 players some enemies were dropped.
- Two games can no longer run the same enemy at once and copy each other (it stood still, then jumped).
- **Bosses no longer get stuck in walls**: each game runs its own boss (hits and the defeat are still shared).
- The menus (item wheel, pause menu) keep the world going while another player is in the session.
- Both players need this version: a host refuses a friend with another one.

### Added
- Doors another player opens swing open in your world with their own animation.
- Webs, rocks, cracked walls and locks another player breaks disappear in your world at once.
- Bombs go off in every world with their real blast; the other players' seeds, Deku Nuts, arrows and hookshot are drawn
  as they fly, with a nut's flash and a spark where a shot hits.
- A friend opening a chest in your room tells you that yours still holds its item.

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
