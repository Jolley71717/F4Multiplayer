## 0.8.10

Install it over 0.8.9. (This is the same file as the 0.8.10-rc1 test build, so seeing "rc1" in your log is normal.) A small fix:
enemies a friend's game spawned could not be hurt by you.

It still uses protocol 35, so it plays with 0.8.9, but the fix works only in the games that have it: everyone should
update.

What changed:

- Your hits on an enemy that a friend's game spawned (random encounters, ambushes, and the like) now stick, and a
  friend's shot no longer heals what you took off it. Before, the enemy's health went straight back up for the player
  whose game didn't spawn it, so only a hit that killed it outright counted, and a hit from the other player could
  heal it in its own game (the Deathclaw that kept regenerating).

Known issues:

- If both of you hit the same enemy within the same instant, one of the two hits can still be lost.
- The rest are as in 0.8.9: friends' faces untried with LooksMenu, the stand-in's grenade not shown in its hand during
  the wind-up, Dogmeat squatting on the joiner, a friend's parked frame taking a moment to move when first synced.

For testers:

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you
  restarted the game since) and the crash log; the first lines of the plugin log name the build. The plugin log names
  players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
