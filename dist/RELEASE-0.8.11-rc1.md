## 0.8.11-rc1

A release candidate for one crash fix: a friend who had just changed their face could crash the other players' games
(the host's, in the report) as they came into view.

It still uses protocol 35, so it plays with 0.8.9 and 0.8.10, but the fix works only in the games that have it:
everyone should update, the host most of all.

What changed:

- A friend who had just picked a new face in the game's face editor (a preset, for example) no longer crashes the
  other players' games when they load in nearby. Their face showed no body shape values, and the game read past the
  end of the empty list as it built that friend's body. Now a missing value counts as no change, and the friend
  still shows their new face.

Known issues:

- If both of you hit the same enemy within the same instant, one of the two hits can still be lost.
- The rest are as in 0.8.10: friends' faces untried with LooksMenu, the stand-in's grenade not shown in its hand
  during the wind-up, Dogmeat squatting on the joiner, a friend's parked frame taking a moment to move when first
  synced.

For testers:

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you
  restarted the game since) and the crash log; the first lines of the plugin log name the build. The plugin log names
  players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
