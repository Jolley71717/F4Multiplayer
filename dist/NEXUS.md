# Nexus Mods page

What to paste into the Nexus mod page. New versions are uploaded by the `nexus-upload.yml` workflow, which sends
`F4Multiplayer-<version>-nexus.zip` (the zip without `F4MPServer.exe`) from the GitHub release: see
docs/RELEASING.md.

## Create draft

- **Mod name:** F4Multiplayer - Co-op over Steam
- **Category:** Gameplay
- **Short description** (350 characters max):

Co-op through Steam for 4+ players: invite friends from the overlay, no port forwarding. Build, run and defend settlements together, and play about 80% of the game's quests together. Shared NPCs, combat, loot, doors and map. Voice chat, revive a downed friend, teleport to friends. Everyone keeps their own save. F4SE plugin, alpha.

## Description (BBCode)

```
[size=5][b]Play Fallout 4 with your friends: 4 players by default, more if the host allows it[/b][/size]

One player hosts, the others join through Steam: Shift+Tab, right-click a friend, [b]Invite to Game[/b] (or [b]Join Game[/b] on the host). No port forwarding, no VPN, no IP addresses. Everyone keeps their own save, character and inventory.

[size=4][b]New in 0.17: build, run and defend settlements together[/b][/size]
[list]
[*][b]Build together:[/b] at the host's settlements, everything a friend builds or wires in workshop mode is built in the host's game. You stand in the same settlement: the same walls, the same power grid, the same turrets. Friends can wire their pieces to the host's, and scrap, store, move and repair the host's pieces too.
[*][b]Run it together:[/b] a friend sees the host's settlers, can give them jobs and set up supply lines, and their workshop bar shows the settlement's real food, water, defense, happiness and power.
[*][b]Defend it together:[/b] a settlement attack is one attack for everyone, the add-ons' too: you fight the same attackers at the same walls.
[*][b]About 80% of the game's quests are shared:[/b] 345 of the 431 quests with a Pip-Boy entry in the base game and the three story add-ons, with the host's "More quests" switch on. That includes the companions' quests, faction kick-outs, radiant jobs and random fights. Nearly all of the rest are hidden bookkeeping quests.
[/list]

[size=4][b]What you share[/b][/size]
[list]
[*][b]NPCs and combat:[/b] one game runs each NPC and the others copy it, so everyone fights the same raider in the same place. Enemies go after any player, and kills and damage count for everyone, including on enemies a friend's game spawned (random encounters, ambushes).
[*][b]The world:[/b] loot taken or stored, items picked up, doors and locks, deaths.
[*][b]Quests:[/b] side quests, companions' quests, radiant jobs and random fights: progress in one game moves the others forward, whichever story setting the host picks, and a turn-in's reward reaches every player once. A friend who hasn't started the quest has it started for them, and a joiner is caught up on the host's side quests. A quest you failed is never started over. A step that would teleport you only happens while you are with the friend who did it, and nobody pays for a friend's bribe.
[*][b]The story moves together:[/b] by default, when any player moves the main story or a faction quest forward, everyone else is fast-forwarded to the same point (a joiner is first brought to the host's point; after that whoever moves the story on leads, and their choices become everyone's). Each step runs in every game in order, achievements included (the game only awards them without mods), and is written into each player's save. A step that has to move you (the Institute's teleporter) comes after a warning, never mid-fight. The host can instead set [b]sStory = own[/b]: then each player plays the main quest and the factions in their own game, at their own pace, with their own choices. If you blew up the Institute and your friend has not, it stands in their game and is a crater in yours; you can still walk around their Boston with them, and their quest NPCs are shared like any others. Nothing resets or reverts.
[*][b]Map:[/b] a location one player finds shows up on everyone's map, ready for fast travel.
[*][b]Settlements:[/b] at the host's settlements you build, wire, scrap, repair and manage settlers together, and fight the same attacks. At a friend's own settlement, what they build, move or scrap appears in your game.
[*][b]The session is saved:[/b] the host's PC keeps the shared world, so dead NPCs stay dead and walls stand next time you play.
[*][b]Time and weather[/b] follow the host.
[*][b]XP:[/b] half the XP for your friends' kills.
[/list]

[size=4][b]Playing together[/b][/size]
[list]
[*][b]How friends appear:[/b] your game places a stand-in character where each friend is, whenever you're in the same interior or near each other outside. It isn't a settler and joins no settlement. It has their own face, hair, sculpting, sex and body shape, wears the armor and weapons from their own save (with their mods), and carries their name and health over its head. You see them aim, fire, reload, swing, throw grenades, climb into power armor and emote (F2 point, F3 cheer, F4 clap), and their Pip-Boy light at night. Enemies fight them, and a hit on the stand-in reaches your friend. You can't loot, trade with or talk to a stand-in: their inventory, caps and perks stay in their own game.
[*][b]Voice chat[/b] through Steam, with volume that fades with distance. Hold the mouse back button to talk.
[*][b]Going down:[/b] a lethal hit knocks you down instead; a friend next to you for 2 seconds gets you up. 45 seconds alone and you die as usual.
[*][b]F6[/b] player list (where everyone is, health, level), [b]F7[/b] teleport to a friend, [b]F8[/b] "over here!" ping.
[*]Reconnecting, reloading a save or dying keeps the shared world consistent: your game catches up on what happened.
[*][b]Starting out:[/b] a new character plays the Vault 111 opening on their own, untouched by the session, and joins the shared world when they leave the vault. Alternate-start mods are untried. The mod spots the opening only by pre-war Sanctuary and Vault 111, so a start anywhere else is shared from the first moment. Play the alternate start through, name your character and save before you host or join. Everyone needs the alternate-start mod installed to share its start. Details in INSTALL.md.
[*][b]Per player, not shared:[/b] companions, romance and affinity (each game keeps its own; the companions' quests are shared), what companions, settlers and any living NPC carry, perks, SPECIAL, caps, crafting and vendors, radio, survival needs, addiction and radiation. Keys, paper notes, holotapes, bobbleheads, magazines, all weapons and armor, settlement workbench storage and power armor frames are each player's too: a gun your friend loots stays on your copy of the corpse. The mod never changes your own gear for a friend, and a friend paying in a conversation doesn't charge you. [b]Shared:[/b] NPCs, loot (ammo, chems, food, junk and other loot in containers and on corpses), doors and locks, buttons and terminals, deaths, side quests, map, settlements, time and weather.
[*][b]Languages:[/b] the mod's messages and F1 menu follow your game's language: English, French, German, Italian, Spanish (Spain and Latin America), Polish, Brazilian Portuguese, Russian, Japanese, and Simplified and Traditional Chinese. Russian shows on any game. Japanese and Chinese need the game itself in that language, or they show as boxes.
[*][b]Mods:[/b] friends may have other mods, or the same ones in another order: what comes from a mod only one player has (its NPCs, items, quests) is left out in the other games, and a joiner is told which mods differ. To share a mod's content, everyone needs that mod.
[/list]

[size=4][b]Requirements[/b][/size]
[list]
[*]Fallout 4 [b]1.11.240[/b] (Steam, current version)
[*]Windows. Linux through Proton should work (F4SE runs there and Steam networking goes through the game's own Steam library), but it is untested: reports welcome.
[*][url=https://www.nexusmods.com/fallout4/mods/42147]F4SE[/url] 0.7.9
[*][url=https://www.nexusmods.com/fallout4/mods/47327]Address Library for F4SE Plugins[/url]
[*]Everyone needs the same version of this mod, and ideally [b]the same mods[/b] (in any order).
[/list]

[size=4][b]Install[/b][/size]
Install with Vortex or Mod Organizer 2, or extract the zip into Fallout 4\Data. The host sets [b]bHost = true[/b] in Data\F4SE\Plugins\F4Multiplayer.ini; friends change nothing. Always start the game with F4SE. The full guide is in the zip: F4Multiplayer\INSTALL.md.

[size=4][b]Alpha[/b][/size]
Back up your saves first, or keep a separate save for co-op.

[b]Known gaps[/b]
[list]
[*]Building is shared only at settlements the host owns. At a friend's own settlement, each player's pieces stay their own.
[*]Radiant jobs whose enemies are made on the spot (the Castle's defense, a courser, a patrol) stay each player's own.
[*]To give a friend an item, drop it near them: keys, notes, holotapes, bobbleheads, magazines and quest items can't be given.
[*]Vault 114's gear door and security gates opened from a terminal or card reader open for everyone; other doors a quest script opens may still open only for the player who opened them.
[/list]

[b]Something broke?[/b]
Attach your log to a bug report: Documents\My Games\Fallout4\F4SE\F4Multiplayer.log (or F4Multiplayer.1.log if you've restarted the game since). It calls other players "player N" and leaves out Steam IDs and IP addresses. Crash logs and screenshots can still show names, so check those first.

Updates come to Nexus in batches. The newest build is always on GitHub.

[size=4][b]Open source[/b][/size]
GPL-3.0. Source and releases: [url=https://github.com/Jolley71717/F4Multiplayer]github.com/Jolley71717/F4Multiplayer[/url].
```

## Permissions

- Other user's assets: none used.
- Upload permission: do not upload to other sites (point to this page).
- Modification permission: yes, under GPL-3.0 (source must be shared).
- Conversion permission: no.
- Asset use permission: yes, with credit, under GPL-3.0.
- Credits: F4SE and CommonLibF4 (libxse), ENet, Address Library.

The changelog for each upload since 0.8.7 is `dist/NEXUS-<version>.txt` (one line per entry, what the upload
workflow sends).
