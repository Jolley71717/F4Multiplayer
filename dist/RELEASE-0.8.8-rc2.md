## 0.8.8-rc2

The second candidate: rc1 plus every translation reviewed. The code is rc1's; only the language files changed, so if rc1
held for you, rc2 will too. Everything since 0.8.7, built together and run through the rig's full set (movement, Corvega
combat, terminals and loot, grenades, power armor, a five-minute cell churn with save reloads, a reconnect and the
fuzz pass) with no crash on either machine. Install it over 0.8.7 and say what holds and what doesn't.

Everyone needs the same build to play together: it uses protocol 33, so a friend on 0.8.7 gets "Version mismatch"
and has to update too.

What changed:

- Friends see your character's name, not your Steam name (`sPlayerName = character`, the new default; an empty value
  still means Steam, and a name written in the ini is used as written). A character name with no Latin letters in it
  falls back to the Steam name for now.
- The mod speaks the game's language. Every message and the F1 menu follow the game's own language setting (or
  `sLanguage` in the ini): English, English (UK), French, German, Italian, Spanish, Latin American Spanish, Polish,
  Brazilian Portuguese, Russian, Japanese, Simplified and Traditional Chinese. Each translation was reviewed line by
  line against the game's own terms in that language (its first line says so); where a Pip-Boy abbreviation could not
  be confirmed, the word is spelt out. Corrections are welcome as pull requests or Nexus comments: the files are in
  `Data\F4SE\Plugins\F4Multiplayer\lang\`, one `key = text` line each. Cyrillic or CJK text needs the game itself to be
  in that language (an English-language game has no font for it). Arabic is not shipped: the game has no Arabic font
  and does not shape right-to-left text; a reviewed file waits for a font mod to be proven.
- A friend's parked power armor stands where they left it. Frames the game placed (the museum's, the ones at
  settlements) are shared like NPCs: whoever last used or moved one runs it, everyone else's copy follows. A suit parked
  before you joined is on the rack when you arrive, and the second frame that used to sit a few feet from where a friend
  got out is gone.
- A friend in power armor walks instead of gliding: the suit's switch-in waits for their gear to settle, and a stand-in
  whose animation graph was held by an idle is freed.
- A friend's gun sweeps when they track a target instead of stepping every tenth of a second; the same for NPCs' aim.
- Mirrored NPCs play the owner's gait: people jog as well as walk and run, at the owner's pace; other creatures set off
  in the gait their owner's game has; an NPC carried along with its legs still stands instead of walking in place.
- The host's character no longer hitches and jumps on a joiner's fast game: when the next state is late the stand-in
  waits at the newest one instead of running past it, and the delay remembers late states by time rather than by frame.
- The Diamond City gate opens for friends: the game's dialogue quests (the intercom scene at the gate, Goodneighbor's
  and Covenant's entries) are shared with the story, as a rule rather than one special case.
- A friend's replayed grenade no longer kills your copies of enemies their game spawned at runtime (random encounters,
  ambushes); those copies are shielded like the game's own NPCs.
- Conversations are ended by the mod only when the mod itself removed the speaker (a friend's stand-in that left),
  never a quest scene; a friend's kept stand-in is calmed instead of fighting you.
- Fewer ways to crash on a save reload: every HUD, menu and map write of the mod's waits while the game loads and
  rebuilds its menus, in one tested place.
- The host's workshop freeze: the developer commands that could hand the engine's deadlock its moment (a menu looked up
  beside the renderer, a bare show or hide of the workshop menu) can't any more.
- If the mod's frames ever stop while the game draws on (seen twice after a `coc` from the main menu), the log now says
  within 15 seconds where the main thread sits, and the frame source is re-armed when it can be.
- Settlement junk the host scrapped before the session: the walk that reports it to joiners is now tested.
- Performance: the `perf` dev command reports what the mod costs the game per frame, part by part; on the rig that is
  0.2 ms a frame in a session, with none of the game's hitches caused by the mod. A developer build with Tracy is
  available for a frame-by-frame look (`docs/PERF.md`).
- For testing: `spawnenemies <base> <n> <radius>` puts a ring of hostiles round the player, snapped to the ground; the
  menu stress loop catches a real hang with the host's thread stacks and a minidump.

Tried and turned off:

- Friends' own faces. The whole look (head parts, skin tone, face paint, complexion, sculpting) now travels, but the
  engine builds no head for a copy made at runtime, so the stand-in would stand headless for a few seconds before the
  mod falls back to the settler's face. `bFriendFaces` is off by default; `facemask 7F` turns it on for an experiment.

Known issues:

- A friend's face and hair aren't copied yet (see above): their stand-in is a settler of their sex with their body
  shape, their LooksMenu body sliders and their gear.
- Dogmeat's mirrored animation can squat instead of running on the joiner; the capture that will tell why is in
  `docs/DOG-SQUAT.md`.
- A stand-in's grenade isn't visible in its hand during the throw wind-up.
- Power armor frames the game didn't place (ones made at runtime) are still shown only through the copy a friend steps
  out of.
- A friend's parked frame synced for the first time may take a moment to move from where the game placed it.

For testers:

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you
  restarted the game since) and the crash log; the first lines of the plugin log name the build. The plugin log names
  players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
