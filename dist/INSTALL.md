# F4Multiplayer: install and play

Four players by default; the host can allow more (`iMaxPlayers`, see [Settings](#settings)). Everyone needs:

- Fallout 4 **1.11.240** on Steam
- Windows, or Linux / Steam Deck through Proton (see [Linux and Steam Deck](#linux-and-steam-deck): start the game through Steam there)
- [F4SE 0.7.9](https://www.nexusmods.com/fallout4/mods/42147?tab=files): copy `f4se_loader.exe` and `f4se_1_11_240.dll` next to `Fallout4.exe`
- [Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327?tab=files) (1.11.240 file)
- This mod: install the zip with Vortex or Mod Organizer 2 (or extract it into `Fallout 4\Data`)
- Ideally **the same mods** as everyone else, in any order: what comes from a mod only one of you has (its NPCs, items, quests) is left out in the other game

**Everyone must use the same version of this mod.** Back up your saves (or start a fresh one)
before playing; this is an early alpha.

Always start the game with F4SE (`f4se_loader.exe`, or your mod manager's F4SE launcher).

## Linux and Steam Deck

F4SE and this mod run under Proton, and a Linux player can play with Windows players (same mods, same load order).
What matters is that **Steam starts the game**: Steam puts its overlay into a game only when Steam itself launches it.
Started any other way (a mod manager running `f4se_loader.exe` straight in the Proton prefix, a "non-Steam game"
shortcut), Shift+Tab, *Invite to Game* and the F1 menu's invite do nothing, and the game may not reach Steam at all
(then nobody can join you, and you can't join anyone).

1. Install F4SE, the Address Library and this mod into the game folder as on Windows
   (`steamapps/common/Fallout 4`, with `Data/F4SE/Plugins/F4Multiplayer.dll`, `F4Multiplayer.ini`, the
   `F4Multiplayer` folder and the Address Library's `version-1-11-240-0.bin` side by side).
2. Make Steam's Play button start F4SE. Either:
   - Steam > Fallout 4 > Properties > General > **Launch Options**:
     `bash -c 'exec "${@/Fallout4Launcher.exe/f4se_loader.exe}"' -- %command%`
   - or the loader as the game: rename `Fallout4Launcher.exe` to `Fallout4Launcher.exe.bak`, then copy
     `f4se_loader.exe` to `Fallout4Launcher.exe`. A game update can put the launcher back: do it again then.
3. Start Fallout 4 from Steam (Library, Play). In Amethyst or another Linux mod manager, pick its *Steam* launch (or
   deploy, then press Play in Steam), not a direct start of `f4se_loader.exe`.
4. In the game, Shift+Tab should open Steam's overlay, and F1 the multiplayer menu.

Use **Proton 9 or later** (or Proton Experimental). With older ones, Wine's DirectInput can keep the keyboard from
the mod, so F1 and the other hotkeys do nothing.

`WINEDLLOVERRIDES` is not needed for F4SE or this mod. Only a mod that puts its own Windows DLL next to
`Fallout4.exe` (an ENB's or ReShade's `d3d11.dll` / `dxgi.dll`, a preloader's `winhttp.dll` or `version.dll`) needs one,
for that DLL: `WINEDLLOVERRIDES="dxgi=n,b" bash -c 'exec "${@/Fallout4Launcher.exe/f4se_loader.exe}"' -- %command%`.

Linux file names are case sensitive. Wine looks past case on its own, but if a mod manager made a second folder that
differs only in case (`Data/f4se/plugins` beside `Data/F4SE/Plugins`, as real folders rather than links to one
folder), Wine may only look in one of them: keep all of F4SE's plugins in one.

The logs are inside the game's Proton prefix:
`steamapps/compatdata/377160/pfx/drive_c/`, then the `steamuser` folder in its `users` folder, then `Documents/My Games/Fallout4/F4SE/F4Multiplayer.log`
(under `~/.local/share/Steam/`, or `~/.var/app/com.valvesoftware.Steam/.local/share/Steam/` for Steam's Flatpak, or the
Steam library folder the game is installed in). F4SE's own `f4se.log` is beside it: it says whether
`F4Multiplayer.dll` loaded. The mod's log starts with lines saying what it runs on and how the game was started:
"Launch: this game was not started by Steam" means the overlay is missing (see step 2), and "Steam is not available
to the mod" says why invites and Join Game are off. The saved session and the other per-player files are in that
same folder.

## Settings

Edit `Data\F4SE\Plugins\F4Multiplayer.ini` (in Vortex: the mod's staging folder, or use
"Open in File Manager"). The file is read when the game starts: change it, then restart the game. The one
exception is the F1 menu's *Settings* page: a switch there applies to your game at once and is saved to the file
(for the host's rules, see below).

One player hosts:

```ini
bHost = true
```

The host's game starts hosting once it is in the world (not at the main menu, and for a new
character not until it is out of Vault 111). Friends who try to join earlier are told the host isn't
in the game yet, and their game tries again by itself.

Everyone else keeps the defaults, apart from the settings of their own (see [Your own settings](#your-own-settings)).
Players are named after the character they play (`sPlayerName = character`;
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

Friends may have other mods than you, or the same ones in another order: the mod matches what you
share by plugin file name, so a reorder changes nothing. What comes from a mod only one of you has (an
NPC, an item, a quest, a settlement piece) is left out in the other game, and the joiner is told which
mods differ (F1, *Compare mods*). To share a mod's content, everyone needs that mod. Two versions of one
mod count as the same mod, so keep those the same. The host can list cosmetic plugins (textures, sounds,
UI) in `sIgnoredPlugins` (comma-separated) so the joiner is not told about them.

### Host settings

These count only in the host's `F4Multiplayer.ini`. A guest's own value of them is not used: the game takes the
host's when it joins, so there is nothing for a guest to set. The two exceptions are `sPassword` (a guest sends their
own as the password to join) and `iPort` (a guest joining without Steam connects on their own `iPort`, unless
`sServerAddress` names a port). The ini is read once, when the game starts: after editing it by hand, restart the
game before you host. A switch in the F1 menu (friendly fire, story) is saved at once but counts from the
next time you host: choose *Stop hosting*, then *Host a session* again.

- `iMaxPlayers` (4): how many players can be in the session at once, the host included. More is allowed, but each
  extra player adds traffic and NPC work for the host, and sessions of more than two players have been tested
  less. A guest who comes when it is full is turned away.
- `sPassword` (empty): if set, a guest must have the same password in their own `sPassword` to join. The host's
  F1 menu can also make up or remove it while hosting.
- `iPort` (7779): the UDP port the host's game listens on. Guests only need it when they join without Steam.
- `bFriendlyFire` (false): on, players can hurt each other with weapons. Off, only enemies hurt a player.
- `sStory` (shared): `shared`, the main story and the faction quests move along for everyone. `own`, each guest plays
  them in their own game. Side quests are shared either way. See [What is shared](#what-is-shared).
- `sIgnoredPlugins` (empty): plugins, comma-separated, that guests may have different. A guest is not told about
  them when their mods differ from the host's.
- `bSaveSession` (true): the host's game keeps the shared world on disk and carries on with it next time that
  character hosts. Guests see no difference, but a guest who joins later catches up on what is kept.
- `bShareGuestBuilds` (false): off, what a guest built in their own save before joining does not come into the host's
  world: only building done during the session is shared. On, a guest's earlier settlement building comes into the
  host's world too. Two saves with different building at the same settlement then show both.
- `bSharedStorage` (false, experimental): on, a container in a home or a settlement (one a player owns, or one in a
  settlement they own) has one copy for everyone, the host's. A guest who opens theirs sees what is in the host's, and
  what one player takes is gone for everyone. Only ammo, aid and junk are shared this way; weapons, armor, keys and
  notes stay in each player's own copy. Off, each player keeps their own copy, and loot taken from one is taken from
  the others' copies too.
  - A container becomes shared once the host has been near it in the session. Until then it works as with this off.
  - A guest's own stash: the ammo, aid and junk a guest had in their own copy of a container before it was shared
    stay in their copy, but out of reach. While the host keeps that container the guest sees the host's things in it,
    not their own. Their own things come back when the host turns this setting off, or when the guest plays that save
    alone.
  - Turning it on in a session you played with 0.9.x: what a guest put into one of the host's containers back then is
    still in the guest's own copy (their stash, out of reach as above), so it leaves the host's copy the first time the
    host comes near that container. Chests at a shared base can look emptier than before. Nothing is lost: what
    guests put in waits in their own copy, and what the host put in stays.
  - Loading a save: a guest who loads an earlier save has nothing taken from them or given to them. The host's copy
    is put right instead, to match what the guest's loaded game did. A move a guest made as their connection dropped
    is sent again when they rejoin.
  - After the host loads a save, a guest's menu on a shared container closes with a notice until the host comes near
    it again. A take that loses a race to another player's take is undone: the guest gets nothing, and the item stays
    in the container.
  - Turning it off again: each player has their own copy again. The session remembers which containers were shared,
    so turning it back on carries on where it left off. A session that has used this setting can't be opened by an
    older version of the mod.
  A session on the dedicated server (`F4MPServer.exe`) has no host's game to keep the copy, so it never shares storage.
- `bContainerGifts` (false, experimental): on, a weapon or a piece of armor a player puts in a container gives it to
  the others. Any container works except a workbench, a vendor's and a companion's, and a container built in a
  settlement. The item then shows in that container for every player who opens it, and the first who takes it has it.
  Off, what a player puts in a container stays in their own copy of it, as in 0.9.x: drop an item to give it instead.
  - Taking a gift: it goes into your inventory a moment after you press Take, once the session says you were first.
    If a friend was first, you get nothing and a notice says so. Until someone takes it, its giver can take it back
    the same way.
  - Each item is given on its own: putting in three guns gives three gifts. Mods, a legendary effect, a name and a
    power armor piece's condition go with it. An item with a mod from a plugin a friend lacks doesn't show for them.
    Quest items and items made by a mod at runtime are not given (a notice says so once).
  - With `bSharedStorage` on or off it works the same: shared storage never shares weapons and armor, so they are
    given this way in any container.
  - Loading a save: a friend who loads a save from before taking a gift has it back in the container for everyone.
    A giver who loads a save from before putting the item in has it back in their own copy, and it is gone for the
    others. The one case not put right: a giver who loads a save from before putting it in, after a friend took it,
    has it too (both have it; the host's log counts it). A mod that moves items out of containers by itself (an
    auto-sorter, say) can make one more of a gift the same way; the gift then stops showing for that player until
    they load a save or rejoin, and each time it shows again such a mod can do it again.
  - A connection that drops while a take is on its way loses nothing: the item is in the container again after you
    rejoin, or in your inventory if the session says you took it. A giver whose connection drops just as
    the item is given finds it given once they rejoin (until then nobody can take it), or still in the container if
    it never reached the session. The dedicated server takes it as `--container-gifts`. Once a gift has been
    given in a session, an older version of the mod can't open that session any more, even after the gift is taken.
- The rules for going down: `bRevive` (true), `iBleedOutSeconds` (45), `fReviveSeconds` (2.0), `iReviveHealth` (30),
  `iMaxRevives` (0, no limit), `iReviveWindowMinutes` (0, per fight), `iReviveCooldownSeconds` (0),
  `bReviveOutOfCombat` (false), `bDownedCrawl` (false) and `bSaveOnDown` (false). Guests play by the host's. See
  [Going down](#going-down).
- `sNpcLevel` (average): the level every game builds leveled enemies at (raiders, ghouls, bloatflies, legendaries),
  so you all meet the same ones. `average` is the average level of the players in the session now (it changes as
  players join, leave and level up), `highest` is the highest level anyone in the session has reached, `host` is the
  host's, and `own` lets each game use its own player's level, as before (you may then see different enemies). An
  enemy keeps the level it was built at, in your save too. The dedicated server takes it as `--npc-level`.
- `sLegendaryDifficulty` (average): whose difficulty sets the chance of a legendary enemy. `average` uses the average
  of the players in the session now, `host` the host's, and `own` each game's own (one of you may then see a
  legendary where another sees a plain one). Your own difficulty still sets the damage you deal and take. The
  dedicated server takes it as `--legendary-difficulty`.
- `bRealTimeVats` (true): in a session VATS runs at full speed for everyone. False keeps the game's slow motion for
  everyone. See [VATS in real time](#vats-in-real-time).

Also host controls, not settings: the host can lock the session, kick and ban from the F1 menu (see
[Host controls](#host-controls)).

### Your own settings

These change only your own game. Set them as you like; friends don't need the same.

- `sPlayerName` (character): the name others see. See above.
- `sLanguage` (empty, the game's language): the language of the mod's messages and menu.
- `iKeyMenu`, `iKeyPlayerList`, `iKeyTeleport`, `iKeyPing`, `iKeyWhere`, `iKeyEmotePoint`, `iKeyEmoteCheer`,
  `iKeyEmoteClap` and `iKeyVoice`: your hotkeys. See [Hotkeys](#hotkeys).
- `bVoiceChat` (true), `bVoiceOpenMic` (false) and `fVoiceRange` (60): your voice chat. See [Voice chat](#voice-chat).
  `fVoiceRange` is how far away you can still hear a friend (0: you hear everyone, wherever they are). It changes
  only what you hear, not how far your voice carries to friends.
- `bSpokenLines` (true): hear what's said near you in a friend's game.
- `bCompassMarkers` (true): friends' markers on your compass.
- `bFriendFaces` (true): friends' stand-ins wear their own face. Off, they all have the settler's face.
- `bGrenadeInHand` (true): a friend's grenade is seen in their stand-in's hand as they throw.
- `iMyAppearance` (0) and `iPuppetBaseForm` (the female settler): which character friends see for you, and which one
  you see for friends who haven't picked one. Friends see only the two settlers.
- `bShareMap` (true): locations friends discover show on your map, and yours show on theirs.
- `bLendPowerArmor` (true): friends may borrow a power armor frame your game made (one you found or looted) once it
  stands parked. They press E on it to ask and again to get in, wear your pieces and fusion core, and it comes back
  to you where they get out, worn as they left it. The pieces stay yours. Off, nobody borrows your frames.
- `bSyncTime` (true): you follow the host's time of day and weather. If the host turns it off, nobody follows.
- `fXpShare` (0.5): the share of a friend's kill XP you get.
- `bSyncQuests` (true): turn it off and your game leaves the shared quests: nothing comes to you, nothing goes out.
- `bSharedRadiants` (true): needs `bSyncQuests` and the shared story. Everyone in the session needs it on for the
  repeatable jobs to match.
- `bHideTutorials` (true): the game's tutorial hints stay hidden in a session.
- `bRunInBackground` (true): your game keeps running when you alt-tab, while you are in a session.
- `bFastLoading` (true) and `iFastLoadingSeconds` (20): faster loading screens.
- `sTransport` (auto), `sServerAddress` (empty) and `bHost`: how you join or host. See
  [Without Steam](#without-steam-lan-or-vpn).

The rest (`bMenuIcons`, `iHangSeconds`, `bHangDump`, `sLogLevel`, `bDevChannel`, `iDevChannelPort`) is for the
mod's developers and bug reports: leave them alone unless you are asked to change one.

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

### Host controls

The host's F1 menu has **Host controls**: kick a player (they are dropped and told the host removed
them; they can join again), ban one (kicked, and refused every later join with "The host has banned
you from this session"), lift bans, lock the session (nobody new can join; those in it stay, and a player
whose game crashes or whose connection drops can rejoin, but not one who was kicked or banned) and unlock it, and make up or remove the
password. A new password is shown on the page and saved to your `sPassword`; friends put the same
in theirs. Bans are kept by Steam account in `Documents\My Games\Fallout4\F4SE\F4Multiplayer.bans`,
so they survive restarts; the file holds salted hashes, never names or Steam IDs, and the log calls a
banned player by a short alias (`~3fa2c1`). Deleting the file lifts every ban. Only the host can do
any of this: the server refuses the commands from anyone else.

### Without Steam (LAN or VPN)

Everyone sets `sTransport = enet`. Friends set `sServerAddress` to the host's IP address. The
host's game listens on UDP port 7779; allow it when Windows Firewall asks. Over the internet the
host also forwards UDP 7779 on their router.

Each player keeps their own save, character and inventory.

### Hotkeys

| Key | What it does |
|---|---|
| F1 | Multiplayer menu: host a session, join a friend who's hosting, see who's in the session, and switch friendly fire, shared story, voice chat, spoken lines, compass markers and fast loading (saved to the ini). See `docs/MENU.md` |
| F6 | Player list: where everyone is, how far and which way, their health and level |
| F7 | Teleport to a friend: tap to choose who, then hold for a second to go there. While you're down: hold to give up |
| F8 | Ping what you are looking at: an enemy, an item lying there, a friend, or the spot under your crosshair ("over here!" when you aim at the sky). You and your friends see it on the compass as a ring in your marker's color for 15 seconds, with a line naming it and how far away it is ("Raider (23 m NE)"). An enemy you ping is the same one in every game. A new ping replaces your last; one a second at most |
| F2, F3, F4 | Emotes: point, cheer, clap. Your friends see your stand-in do it. Not in power armor: they slid the view sideways there, so the key says so and does nothing |
| F10 | Where are my friends: a line each with their distance and direction ("Sam: 240 m NE"), or the place they are in when it is elsewhere |

Change them in `F4Multiplayer.ini` (`iKeyPlayerList`, `iKeyTeleport`, `iKeyPing`, `iKeyWhere`, `iKeyEmotePoint`, `iKeyEmoteCheer`, `iKeyEmoteClap`). They don't
work while a menu or the console is open.

### Voice chat

Hold the mouse's back side button to talk (`iKeyVoice`; set `bVoiceOpenMic = true` to talk without
a key). You hear a friend at full volume within 8 meters, fading out by 60 meters (`fVoiceRange`;
0 = you hear everyone, wherever they are). It uses the microphone chosen in Steam > Settings > Voice.

### Going down

With friends in the session, a hit that would kill you knocks you down instead. A friend who stands
next to you for 2 seconds helps you up (with 30% health). When a friend goes down, holding F7 takes
you to them. If nobody comes within 45 seconds, or you hold F7, you die as usual. The host can turn this off
with `bRevive = false`.

The host can make it harder (their ini sets it for everyone; a joiner's own settings aren't used):
the time to bleed out (`iBleedOutSeconds`), to help up (`fReviveSeconds`), the health you get up with
(`iReviveHealth`), how many times each player can be helped up per fight or per so many minutes
(`iMaxRevives`, `iReviveWindowMinutes`), a time after getting up when a lethal hit kills
(`iReviveCooldownSeconds`), and helping only out of a fight (`bReviveOutOfCombat`). When the host has
changed any of these, joiners are told the rules as they join, and the F1 menu shows them. The HUD
tells you how many revives you have left. The dedicated server takes the same rules as options (`--bleed-out`,
`--revive-seconds`, `--revive-health`, `--max-revives`, `--revive-window`, `--revive-cooldown`,
`--revive-out-of-combat`, `--no-revive`, `--downed-crawl`, `--save-on-down`).

The host can also let downed players crawl (`bDownedCrawl = true`; experimental, off by default):
while down, forward and back crawl slowly and left and right turn you, so you can reach a friend or
cover before you bleed out. Walls, ledges and steep ground stop you, and so does a friend right next
to you, so their help isn't broken. Your friends see you drag yourself along the ground.

Dying, in a session or alone, makes the game load your last save, so you lose what you did since it; the HUD
says so as you go down. Teleporting to a friend with F7 makes an autosave when you land, as a door or fast
travel would, if the game's own "Save on travel" setting is on (Settings > Gameplay). The host can also have
everyone's game make an autosave as they go down (`bSaveOnDown = true`, off by default; server:
`--save-on-down`), so bleeding out takes you back only to that moment. Neither saves in Survival, and neither
saves within a minute of another save.

### VATS in real time

In a session VATS runs at full speed instead of slowing the world almost to a stop, as in Fallout 76,
so your friends don't see you stand still while they fight. On Jet (or the like) VATS keeps its slow
motion. The host can keep the game's own slow motion for everyone with `bRealTimeVats = false` (server:
`--slow-vats`). The game's tutorial hints stay hidden in a session (`bHideTutorials`, your own setting).
The Pip-Boy and the other menus still pause your own game, as before (the session goes on for your
friends).

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
- **What you see them do:** walk, run, sneak and jump, aim, shoot (with the gun's sound and muzzle flash),
  reload, swing, punch, throw grenades, climb into power armor and park it, emote (F2, F3, F4), and turn on their
  Pip-Boy light. Their name is over their head when you look at them, with their health when they're hurt.
- **What they can do in your world:** enemies see them and fight them, and a hit on their stand-in reaches the real
  friend. Their kills, loot, doors and quest steps reach your game as described below. Pressing E on them does
  nothing: you can't talk to, trade with or give things to a stand-in (drop an item near them instead: see Giving items below).
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
  With the host's `bSharedStorage` on, a home's and a settlement's containers (not the workbench) have one
  copy for everyone, the host's (see [Host settings](#host-settings)).
- **Cars, barrels and other things that blow up:** a car, an explosive barrel, a fuel tank or a gas pump one of you
  shoots goes off in every game, once, and stays wrecked in every game: a friend who arrives later, or a reload, finds
  the burnt-out hulk, and it can't blow up a second time. A mine one of you shoots goes off for everyone too.
- **Giving items:** drop it from your Pip-Boy near your friend and they pick it up; it moves from
  your game to theirs, with its mods, legendary effect and name, and only one of you can take it.
  Weapons and armor put in a container stay in your game only, so drop them instead, unless the host turns on
  `bContainerGifts` (see [Host settings](#host-settings)): then one put in a container is given to whoever takes it
  first. Keys, notes,
  holotapes, bobbleheads, magazines and quest items can't be given (each player collects their own).
  Items left lying about are kept with the host's session.
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
- **Radiant quests:** with the shared story, when one of you gets one of these repeatable jobs,
  everyone's game starts the same job for the same place:
  - the Minutemen's settlement requests (raiders, kidnappers, ghouls, a place to clear, a hostile
    settlement to take, Abernathy Farm's locket) and Preston's requests from your settlements
    (building them up, a generator, a synth among the settlers);
  - the Brotherhood's Cleansing the Commonwealth, Quartermastery and Feeding the Troops;
  - the Institute's Pest Control, Hypothesis and Political Leanings;
  - the Railroad's Butcher's Bill, Concierge, Jackpot, Mercer Safehouse, Weathervane and Randolph
    Safehouse;
  - Diamond City's wanted posters.

  Finishing it finishes it for everyone, and each of you gets the reward once. If your game was
  holding the same job for another place and you hadn't taken it yet, it is swapped for your
  friend's. A job your game can't match (it needs someone your game doesn't have, or you already took
  your own) stays the other player's own, and the log says why. The jobs whose enemies or helpers the
  game makes on the spot stay each player's own: settlement attacks and the Castle's defense (your
  friend's attackers come to your game anyway, as everyone's enemies do), and the jobs that give you
  someone to escort, rescue or kill (a squire, a scribe, a courser, a patrol, a synth boss, refugees).
  Turn this off with `bSharedRadiants = false`.
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
they host that character, the session carries on where it stopped, whatever mods they have then (a
session saved by 0.8.11 or older carries on only with the same load order). Hosting
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
- Misc quests, and the radiant quests not listed above (settlement attacks, for example).

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
- Everyone needs the alternate-start mod installed to share its start (what comes from a mod only one player has
  is left out in the other games).

## Dedicated server (optional)

`F4Multiplayer\F4MPServer.exe [--port 7779] [--max-players 4] [--password X] [--friendly-fire] [--own-story]` runs a server
without playing (over UDP only). Everyone, including you, then joins with `sTransport = enet` and
`sServerAddress`.

Its console takes commands while it runs: `players` lists who is in (by number), `kick <n>` and
`ban <n>` remove one, `bans` lists the bans and `unban <n>`, `unban ~alias` or `unban all` lifts
them, `lock` and `unlock` close and open the session to new players, `password <text>` sets the
password (`password -` removes it, `password` says whether one is set) and `help` lists them;
Ctrl+C stops the server. `--ban-file PATH` keeps the bans in a file so they outlive the server;
without it they last until it stops.

## Known limitations (early version)

- Friends' faces haven't been tried with LooksMenu or with a heavily sculpted character yet; reports welcome.
- Keys, notes, holotapes, bobbleheads, magazines and quest items can't be given to a friend, and a weapon or armor put
  in a container is given only with the host's experimental `bContainerGifts` on: otherwise drop it near them.
- If you and a friend hit the same enemy in the same instant, one of the two hits can be lost.
- Misc quests (holotapes, notes, "talk to" errands, but Diamond City's wanted posters) and radiant quests other than those listed under
  "Radiant quests" are not shared, nor are settlement attacks: each game runs its own.
- Vault 114's gear door and security gates opened from a terminal or card reader open for everyone. Other doors
  a quest script opens may still open only for the player who opened them.
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
- "Your mods differ from the host's (N): see F1" (joined and told so): the multiplayer menu's *Compare mods* page lists every plugin you are missing and every one only you have (the log has them too); you can play, but what comes from those is left out in the other game. Plugins in another place in your load order are listed as nothing to change. Plugins the host lets differ (`sIgnoredPlugins`) are listed only where they move another plugin. To share a mod's content, install or disable yours to match, then restart the game: it loads mods only when it starts. "You just asked the host; try again in a few seconds": *Ask the host again* works once in five seconds.
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
