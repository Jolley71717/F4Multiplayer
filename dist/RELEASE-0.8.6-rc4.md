## 0.8.6-rc4 (release candidate)

A candidate for 0.8.6. Install a crash logger (Buffout 4 or Addictol) before trying it, and report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you restarted the game since) and the crash log. The plugin log names players as "player N" and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.

Protocol 29: everyone needs this build (0.8.5 and the earlier 0.8.6 rcs can't join).

What changed since rc3:

- A friend who runs is shown running, not walking fast: their stand-in follows their own walk, run or sprint, at the pace their own game plays it, and strafes or backs up when they do.
- A friend's stand-in moves more evenly when their game runs slowly, and no longer slows down and lurches after a stall. A friend whose game hitched while joining is drawn close behind within a second or two, not trailing for half a minute.
- People and creatures another player's game runs strafe, back up and turn the way they do in that game, instead of flipping between directions.
- A friend's gun shows on their stand-in with their own barrel, sights and receiver, not a set rolled for your level. Armor and power armor pieces keep their mods too; a mod from a plugin you don't have is left off instead of the gun being handed over again and again.
- Downed: a player knocked down with friends around waits to be helped up again (rc3 could kill them outright). A friend within a few steps helps them up after 2 seconds; after 45 seconds alone they die once and the game loads their last save. Friends see "is back up", never a false "died", and pausing while you get up doesn't kill you. A friend's stand-in no longer sits on the ground while they fight.
- A turret or robot a friend builds in a settlement shows up in your game too, on your side.
- The host's Preston, Sturges and Jun in Sanctuary show up beside them for a friend whose copies still wait in the museum.
- A friend's Pip-Boy light follows them across cells without flickering.
- A friend's kill of an NPC that is switched off in your game leaves a corpse.
- Saving and loading are a little faster.

Known issues:

- A friend in power armor is drawn on the AtomCat frame instead of the plain one (hidden under the armor pieces).
- Two players opening or closing the same door at the same instant can end up seeing it differently; using the door again puts them back in step.
