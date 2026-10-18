## 0.8.9-rc1

A release candidate: everything since 0.8.8, built together and run through the rig's full set (movement, Corvega
combat, terminals and loot, grenades, power armor, a five-minute cell churn with save reloads, a reconnect and the
fuzz pass) with no crash on either machine. Install it over 0.8.8 and say what holds and what doesn't.

Everyone needs the same build to play together: it uses protocol 35, so a friend on 0.8.8 gets "Version mismatch"
and has to update too.

What changed:

- Friends wear their own face. Their stand-in now has their face, hair, skin, face paint and sculpting, built by the
  game the same way it builds your own character's head. `bFriendFaces` is on by default. If another mod has changed
  the same spot in the game, the mod leaves it alone and friends get the settler's face, as in 0.8.8 (the log says
  so).
- Which mods differ, by name. When your mods don't match the host's, the HUD says how many differ and the F1 menu has
  a "Compare mods" page: the mods only the host has, the ones only you have, and the ones loaded in a different place,
  ten to a page, with "Ask the host again" for after you've changed them. The game can't download mods for you, so
  you still install or disable them yourself and restart. Mods the host lets differ (`sIgnoredPlugins`) aren't listed
  unless they move other mods' place.
- The host's hang on a teleport is fixed: the loading screen that never ended, seen under the test rig's rapid
  teleports, came from the game's texture loading waiting on work that hadn't been handed over yet. It is handed over
  before every move now.
- A second, separate host hang is fixed: a broken object's physics (a skeleton near Sanctuary, in the one case seen)
  could make the game's physics loop forever. The loop now ends, and the object's physics is rebuilt from where it
  was saved.
- Character names in any script: a friend named in Cyrillic, Chinese or Japanese, or with emoji, is seen under that
  name, and a joiner who names a new character after connecting is renamed for everyone. An English-language game
  draws Cyrillic on the name tag; Chinese and Japanese show as boxes unless the game is in that language.
- The mod never changes your own gear for a friend. Every place the session gives, takes or equips things on a
  friend's behalf now refuses your own character, and says so in the log. Five conversations that took caps or items
  (the Diamond City bat sale, Bunker Hill's 10 caps, Vault 81's caps and fertilizer, Far Harbor's charge card) no
  longer charge you when a friend pays. A unique item a friend picks up disappears from your world only while it's
  still lying there, never from an inventory.
- A friend's power armor frame made at runtime (not one the game placed) now stands in your game too, parked before
  you joined or since.
- Fast loading keeps vsync off for the first 20 s of a loading screen at most (`iFastLoadingSeconds`), so a stuck one
  no longer runs the GPU flat out.
- Voice chat costs nothing in a frame while nobody could hear you and no friend's voice is playing.
- If the graphics driver resets, the log now says so, so a crash that follows can be told from one of the mod's.

Known issues:

- Friends' faces haven't been tried with LooksMenu or with a heavily sculpted character yet; reports welcome.
- A stand-in's grenade isn't visible in its hand during the throw wind-up (a fix is in, behind a developer toggle
  that is off until it has been tried in real play).
- Dogmeat's mirrored animation can squat instead of running on the joiner; the capture that will tell why is in
  `docs/DOG-SQUAT.md`.
- A friend's parked frame synced for the first time may take a moment to move from where the game placed it.

For testers:

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you
  restarted the game since) and the crash log; the first lines of the plugin log name the build. The plugin log names
  players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
