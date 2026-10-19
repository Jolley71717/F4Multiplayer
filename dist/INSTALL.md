# F4Multiplayer: install and play

Up to 4 players. Everyone needs:

- Fallout 4 **1.11.240** on Steam
- Windows. Linux through Proton should work (F4SE runs there; Steam invites and relays go through the game's own Steam library) but is untested; the saved session and per-player files land inside the Proton prefix's Documents folder.
- [F4SE 0.7.9](https://www.nexusmods.com/fallout4/mods/42147?tab=files): copy `f4se_loader.exe` and `f4se_1_11_240.dll` next to `Fallout4.exe`
- [Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327?tab=files) (1.11.240 file)
- This mod: install the zip with Vortex or Mod Organizer 2 (or extract it into `Fallout 4\Data`)
- **The same mods in the same load order** as everyone else (the server refuses mismatches)

**Everyone must use the same version of this mod.** Back up your saves (or start a fresh one)
before playing; this is an early alpha.

Always start the game with F4SE (`f4se_loader.exe`, or your mod manager's F4SE launcher).

## Settings

Edit `Data\F4SE\Plugins\F4Multiplayer.ini` (in Vortex: the mod's staging folder, or use
"Open in File Manager").

One player hosts. Only the host changes a setting:

```ini
bHost = true
```

The host's game starts hosting once it is in the world (not at the main menu, and for a new
character not until it is out of Vault 111). Friends who try to join earlier are told the host isn't
in the game yet, and their game tries again by itself.

Everyone else keeps the defaults. Players are named after the character they play (`sPlayerName = character`;
your Steam name stands in until a new character has a name). Set `sPlayerName` to a name of your own, or leave it
empty to use your Steam name. The name is sent when your game connects.

The mod's messages and its F1 menu follow the language the game is set to (English, Russian, Spanish,
German, French, Italian, Polish, Brazilian Portuguese, Japanese, Simplified and Traditional Chinese). Set `sLanguage` in
`F4Multiplayer.ini` to pick another (`en-gb` is English with UK spelling). The texts are in
`Data\F4SE\Plugins\F4Multiplayer\lang`, one file per language; each file's first line says whether it is a first machine
translation or has been reviewed against the game's own terminology. To correct one or add a language, edit the text
after each `=` (keep the `{placeholders}`) and send the file to the mod's page. Russian shows on a game in any
language: the game's fonts have Cyrillic letters. Japanese and Chinese need the game itself in that language: an
English game has no font for them and shows boxes instead (a friend's name written in them shows as boxes too). Arabic is not shipped: the game has no Arabic font and does not shape right-to-left text, so it waits for a font mod to be proven (a reviewed file is ready in the repository).

Friends join through Steam: no ports, IP addresses or VPNs. Traffic goes through Steam's relays,
so nobody sees anyone's IP address. Only Steam friends of the host (or players the host invites)
can join. Optionally set the same `sPassword` for everyone.

If a friend has a purely cosmetic mod you don't (textures, sounds, UI), the host can list its plugin
in `sIgnoredPlugins` (comma-separated) and the load orders are compared without it. Only do that for
mods that add nothing the session shares: an NPC, item or settlement mod listed there desyncs quietly.
Such a plugin must load after all your other plugins (an `.esl` or light one after the other light
ones): one earlier changes the form IDs of every mod after it, so the friend is refused. The free HD
texture pack works the same way: without other mods it never matters, with mods it loads before them,
so everyone needs it or nobody does.

## Playing

If your Steam status hides what you play ("private" game details), friends can't see you in a game: invite them from the overlay instead of waiting for Join Game, or set your game details to visible for friends while hosting.

1. Everyone starts the game **with F4SE** and loads a save.
2. The host invites friends: open the Steam overlay (Shift+Tab), go to Friends, right-click a
   friend and pick **Invite to Game**. The friend accepts the invite in their Steam overlay.

   Or the friend joins on their own: in the Steam overlay or friends list, right-click the host
   and pick **Join Game**.
3. You'll see "Multiplayer: connected" and "<name> joined" messages.
4. Players appear to each other when they are in the same interior, or near each other outside
   (within about 100 meters). See [How friends appear](#how-friends-appear).

Accept invites and use Join Game **while the game is already running with F4SE**. If Steam starts
Fallout 4 for you from an invite, it starts without F4SE and the mod doesn't load: close it, start
the game with F4SE, then join again.

### Without Steam (LAN or VPN)

Everyone sets `sTransport = enet`. Friends set `sServerAddress` to the host's IP address. The
host's game listens on UDP port 7779; allow it when Windows Firewall asks. Over the internet the
host also forwards UDP 7779 on their router.

Each player keeps their own save, character and inventory.

### Hotkeys

| Key | What it does |
|---|---|
| F1 | Multiplayer menu: host a session, join a friend who's hosting, see who's in the session, and switch friendly fire, shared story, voice chat, compass markers and fast loading (saved to the ini). See `docs/MENU.md` |
| F6 | Player list: where everyone is, how far and which way, their health and level |
| F7 | Teleport to a friend: tap to choose who, then hold for a second to go there. While you're down: hold to give up |
| F8 | "Over here!": tells everyone where you are, with distance and direction |
| F2, F3, F4 | Emotes: point, cheer, clap. Your friends see your stand-in do it |

Change them in `F4Multiplayer.ini` (`iKeyPlayerList`, `iKeyTeleport`, `iKeyPing`, `iKeyEmotePoint`, `iKeyEmoteCheer`, `iKeyEmoteClap`). They don't
work while a menu or the console is open.

### Voice chat

Hold the mouse's back side button to talk (`iKeyVoice`; set `bVoiceOpenMic = true` to talk without
a key). Friends hear you at full volume within 8 meters, fading out by 60 meters (`fVoiceRange`;
0 = everyone hears everyone). It uses the microphone chosen in Steam > Settings > Voice.

### Going down

With friends in the session, a hit that would kill you knocks you down instead. A friend who stands
next to you for 2 seconds helps you up (with 30% health). When a friend goes down, holding F7 takes
you to them. If nobody comes within 45 seconds, or you hold F7, you die as usual. Turn this off with
`bRevive = false`.

## How friends appear

Each of you plays in your own game, with your own save. A friend is shown in your game by a stand-in: a
character your game places where your friend is, and moves as they move.

- **Where:** wherever they are, once you are in the same interior or near each other outside. They are not a
  settler and don't join a settlement. Walk away and the stand-in goes; come back and it is there again. The F6
  list says where every friend is, and your compass marks friends in the same interior as you, or outside in the
  same land (the Commonwealth, Far Harbor, Nuka-World), however far.
- **How they look:** their own face, hair, skin, face paint and sculpting, their sex and body shape (and their
  LooksMenu body sliders if you both have LooksMenu). If another mod has changed the part of the game that builds
  faces, friends keep a settler's face instead (the log says so).
- **What they wear:** the armor and weapons from their own save, with the mods on each one. Nothing of it is in
  your game: you can't loot it, and their inventory, caps and perks stay theirs.
- **What you see them do:** walk, run, sneak and jump, aim, shoot (with the gun's sound, no muzzle flash yet),
  reload, swing, punch, throw grenades, climb into power armor and park it, emote (F2, F3, F4), and turn on their
  Pip-Boy light. Their name is over their head when you look at them, with their health when they're hurt.
- **What they can do in your world:** enemies see them and fight them, and a hit on their stand-in reaches the real
  friend. Their kills, loot, doors and quest steps reach your game as described below. Pressing E on them does
  nothing: you can't talk to, trade with or give things to a stand-in (giving items is coming in a later version).
- **Their companion:** it walks with them in your game too, but it stays their companion: its affinity, its romance
  and what it carries are kept in each game apart. If you both travel with Dogmeat, each game keeps its own.

The session never changes your own character for a friend: nothing it does on a friend's behalf gives you, takes
from you or puts on you any item, and a friend paying for something in a conversation doesn't charge you.

## What is shared

- **Players:** position, walking/running/sneaking/jumping, your armor and weapon (drawn or
  holstered), shooting, reloading, aiming, swinging a melee weapon or punching, your Pip-Boy light, and your name when someone looks at you (with your health when you're
  hurt). Messages tell everyone when a player kills something, dies, completes a quest, joins or
  leaves.
- **Time and weather:** everyone follows the host's time of day and weather (`bSyncTime`).
- **Map:** a location one player discovers appears on everyone's map, ready for fast travel
  (`bShareMap`).
- **XP:** you get half the XP for your friends' kills (`fXpShare`).
- **NPCs and combat:** each NPC is run by one player's game and the others copy it, so everyone sees
  the same raider in the same place. Its weapon and armor are copied from that game too, so a
  raider carries the same gun on every screen. Enemies a friend's game spawned (random encounters,
  ambushes) are there in your game too, and your hits on them stick. Enemies can attack any player, and the damage reaches that
  player. Killing or hurting an NPC counts for everyone. Players can't hurt each other unless the
  host sets `bFriendlyFire = true`, with one exception: a friend's grenade or Fat Man shell that lands
  at your feet hurts you, because in your game it is what kills the enemies beside you.
- **World:** deaths, loot taken from or put into containers and bodies, items picked up off the
  ground, doors opened/closed and locks picked. Not a settlement's workbench or your own companion:
  each game keeps its own, so junk a friend puts in the workbench is not in yours to build with, and
  what a friend gives your companion stays with the companion in their game.
- **Quests:** when a side quest moves forward in one game, it moves forward in the others (never
  backwards). A friend who hasn't started that quest has it started for them, so everyone sees the
  same gates open and the same people move on. When you join, your game is caught up on the host's
  side quests the same way (what you did before joining stays yours and isn't sent). A quest you
  failed or that stopped in your game is never started over. A step that
  would move you (a teleport, a fade to black, your controls taken away) only happens while you're
  with the friend who did it, and you never pay a friend's bribe or payment (caps, a Stimpak) out of
  your own pockets. Catching up waits while you're in a fight. Turn this off with
  `bSyncQuests = false` if a quest misbehaves.
- **The story:** the main story and the faction quests move along together. When any player moves
  the story forward, everyone else is fast-forwarded to the same point, one stage after another, so
  each stage runs in every game as it did in the first, achievements included (the game only awards
  them without mods), and goes into each player's save when they save. Scenes can play out where you
  aren't, and a friend's faction choices become yours. A story step that has to move you to go on
  (the Institute's teleporter) comes after a 10-second warning, and not during a fight; once it has
  begun it finishes even if the connection drops, so you are never left without your controls.
  Nothing else of a session's story reaches your game once you have left it.
- **Your own story instead:** the host can set `sStory = own` so everyone plays the main story and
  the faction quests in their own game, getting a notice when a friend finishes a story quest. Play
  it side by side: kills count for everyone, and each of you talks to the characters yourself. A
  story character that a friend's game says died where they were nowhere near, or that a friend's
  game switches on, stays as your own story has it. So does one you both saw die, once you load a
  save from before it died: your save decides. A side quest then never moves your story on
  (Sanctuary doesn't make you a Minuteman because a friend joined them).
- **Locked story areas:** with `sStory = own`, doors that a main-story stage opens (Fort Hagen's
  elevator, the Institute relay, Mass Fusion's reactor) open for each player at their own stage. A
  friend who is further along walks through; you are refused until you get there yourself. With the
  default shared story you all reach those stages together. Side-quest doors (the Museum of Freedom, Vault 81, Cabot House)
  open for everyone once each has the quest, since side quests are shared. Doors you unlock with a
  key, a bobby pin or a terminal open for your friends too.
- **Factions with separate stories:** your NPCs are shared, so if one of you is at war with a
  faction the other has joined, that faction's people are hostile to both of you where you stand
  together.
- **A new character:** play the opening up to leaving Vault 111 on your own; you can be connected
  meanwhile. Nothing from the session touches it, and what your friends did is applied once you're out.
  Using an alternate-start mod: see [Alternate-start mods](#alternate-start-mods).
- Players who join later catch up on everything that already happened this session. So does anyone
  who loads a save or dies and reloads.

For the closest shared world, start from similar saves (for example, everyone at the same point in
the story). Things that were already different between your saves before the session stay different.

With the shared story (the default), make big story choices together. Quest progress only moves forward, so if
one player sides with one faction and another player with a rival one, both games get both results.
If an earlier version left your main story stuck after Unlikely Valentine (Getting a Clue never
started), join a shared-story session once: it starts Getting a Clue for you. On your own, open the
console and type `setstage 229E6 10`.

What happens in a session is saved into your game when you save. Use a separate save for co-op if you
want to keep your solo playthrough untouched.

The host's game also keeps the shared world on disk (`bSaveSession`, on by default), one file per
character: `Documents\My Games\Fallout4\F4SE\F4Multiplayer.<character>.session`. The next time
they host that character with the same load order, the session carries on where it stopped. Hosting
another character (a solo playthrough, or a new one) hosts that character's own session, and the
co-op character's is kept for later. A joined friend who loads a save of another character leaves
the session. Delete a character's file to start its session over. The save before the last one is
kept beside it (`.bak`): if the file is ever damaged, the host carries on from that, and the damaged
file is kept as `.corrupt`.

Coming from 0.8.5 or earlier: the old `F4Multiplayer.session` becomes the session of the first
character that hosts with a save played in that session. A character with no such save starts a
fresh session and leaves the old file where it is. To give the old session to a character by hand,
quit the game and rename `F4Multiplayer.session` to the file name the game's log names for that
character (`F4Multiplayer.log`: "this character starts its own in F4Multiplayer.<character>.session"),
replacing that character's file if it has one. Going back to an older version starts that
version's session afresh.

## How your stories can differ

You play in one shared world, but each of you keeps your own save. What the session shares is the same in every
game while you play together. The rest stays as each save has it.

The same for everyone:

- The NPCs and enemies around you, and who is dead.
- Loot in containers and on bodies (ammo, chems, food, junk), doors and locks, buttons and terminals.
- Side quests: whoever moves one on moves everyone along, and a joiner is caught up on the host's.
- The map, what is built in settlements, the time of day and the weather.

Each player's own, always:

- Your character: level, SPECIAL, perks, caps, inventory, weapons and armor.
- Companions and romance: each game keeps its own affinity.
- Keys, notes, holotapes, bobbleheads and magazines: each of you collects your own.
- A settlement's workbench storage, vendors, the radio, survival needs.
- Misc and radiant quests (Minutemen settlement requests, for example).

The main story and the factions follow the host's `sStory` setting:

- **Shared (the default):** one story for everyone, starting from the host's. When you join, your game is brought
  to the host's point, one step after another. From then on, whoever moves the story on moves everyone along, with
  that player's choices. Nothing goes backwards: where you were already further than the host, you stay there, and
  what you did before joining isn't sent to the others. Make the big choices together: if two of you side with
  rival factions, both games get both results.
- **Own (`sStory = own`):** each of you plays the main story and the factions in your own game, at your own pace,
  with your own choices. Say you blew up the Institute and your friend hasn't: it is a crater in your game and still
  stands in theirs. You can still walk round their Boston with them, and story characters you meet there are shared
  like any NPC. But a story character a friend killed while you weren't there stays alive in your game, and doors
  that a story step opens open for each of you at your own step. If you are at war with a faction your friend has
  joined, its people are hostile to you both where you stand together.

What carries over:

- What happens in a session goes into your save when you save. Nothing resets or goes back: progress only moves
  forward.
- The host's PC keeps the shared world between sessions, so dead NPCs stay dead next time.
- Whatever was already different between your saves before the session stays different. For the closest world,
  start from saves at a similar point in the story, or each make a new character.

## New characters and alternate starts

A new character plays the opening on their own, up to leaving Vault 111. While a player is in the pre-war Sanctuary
or in Vault 111 and has not yet done "Exit Vault 111", the session leaves their game alone: no shared NPCs, quests,
time, weather or damage, and no teleports to or from them. What friends did meanwhile is applied once they are out.
A host's game starts hosting only once it is out.

### Alternate-start mods

No alternate-start mod has been tried with this mod yet. This is what its code says should happen; reports are
welcome.

- The mod knows the opening only by those two places, the pre-war Sanctuary and Vault 111. A start that wakes you in
  Vault 111 counts as the opening until you leave the vault (or the start marks "Exit Vault 111" done). A start anywhere else (a room of the mod's own, or out
  in the Commonwealth) doesn't: the session shares your world from the first moment, with time, weather and NPCs.
  If the start has scenes of its own, a friend's game could run their NPCs or change the time under them. So play
  the mod's start through before you host or join.
- A host's session is filed under the character, and the game gives a character its ID when its name is confirmed.
  If your start has you make your character somewhere else than the pre-war Sanctuary, name your character and save
  before you host or join. Otherwise hosting may start over with a fresh session once you have your name, and a
  friend who joined before naming theirs may be told "you loaded another character" the next time they load a save.
- With the shared story, the host's and your friends' main-story progress may be brought into your game even if
  your start skipped the main quest (the session starts a quest you haven't, unless your start stopped it). If your character is meant to have a story of their own, ask the host for `sStory = own`.
- Everyone needs the alternate-start mod installed, even players who don't use its start, because the load orders
  must match.

## Dedicated server (optional)

`F4Multiplayer\F4MPServer.exe [--port 7779] [--max-players 4] [--password X] [--friendly-fire] [--own-story]` runs a server
without playing (over UDP only). Everyone, including you, then joins with `sTransport = enet` and
`sServerAddress`.

## Known limitations (early version)

- Friends' faces haven't been tried with LooksMenu or with a heavily sculpted character yet; reports welcome.
- You can't give a friend an item yet: what you drop stays in your game. Giving items is coming in a later version.
- If you and a friend hit the same enemy in the same instant, one of the two hits can be lost.
- Other players' and NPCs' shots show the firing animation and play the gun's sound, but without a
  muzzle flash yet.
- Misc and radiant quests (e.g. Minutemen settlement requests) are not shared.
- NPCs killed in a session come back when their area resets in the host's game. On a dedicated server
  there is no host's game, so they stay dead for that session.
- Workshop building is shared one way per object: what a friend builds appears in your game as a plain copy you can't scrap, wire or assign settlers to (they can, on their side). Power and snapping to a friend's pieces don't cross games yet. Only things the workshop can build are copied, and no container that comes with things in it. What you keep in a copy of a friend's container comes to your inventory when the copy goes (they scrap it, or the session ends).
- If you load a save while in a session, a piece a friend moved or scrapped between that save and now stays where your save had it until the next session (the rest of their building comes back as it is now).

## Problems

- "Connection to the host is unstable": nothing has come back from the host for 4 seconds. It
  usually recovers ("connection restored"); after 20 seconds you're disconnected and the mod keeps
  trying to reconnect.

- Logs: `Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, and the last three launches' as
  `F4Multiplayer.1.log` (the one before) to `.3.log`. They name other players as `player N`, Steam
  accounts by a short alias such as `~3fa2c1`, and mask IP addresses, so they are fine to attach to a
  public bug report. Crash logs, hang `.dmp` files, screenshots and logs from older versions can still
  hold names: check those first. `sLogLevel = debug` adds more detail when you are asked for it.
- "Your mods differ from the host's (N): see F1" (refused for your load order, or joined and told so): the multiplayer menu's *Compare mods* page lists every plugin you are missing, every one only you have, and the ones in another place in your load order (the log has them too). Plugins the host lets differ (`sIgnoredPlugins`) are listed only where they move another plugin. Install, disable or sort yours to match, then restart the game: it loads mods only when it starts. "You just asked the host; try again in a few seconds": *Ask the host again* works once in five seconds.
- "Version mismatch": everyone needs the same version of this mod.
- No "Invite to Game" or "Join Game" in Steam: the host must have started the game with F4SE and
  `bHost = true`; check the host's log for a "Steam: hosting lobby" line.
- "you're hosting": only the host sets `bHost = true`.
- "The host isn't in the game yet": the host's game is at the main menu, loading, or in Vault 111's
  opening. Your game keeps trying and joins once the host is in.
- "you loaded another character": the session belongs to the character you played it with. Load
  that character's save (and join again, if you're a friend).
- "the host is in another session now": your game lost its connection and, trying again, found the
  host playing another character (or a fresh session). It leaves rather than mix that session into
  your save; join again from the menu if you want to play in it.
- "this game hosts once you're in the world": `bHost` is on, so this game can't join friends. Pick
  Stop hosting in the menu (or set `bHost = false`) first.
