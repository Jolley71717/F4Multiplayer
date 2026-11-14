# Nexus Mods page

What to paste into the Nexus mod page. New versions are uploaded by the `nexus-upload.yml` workflow, which sends
`F4Multiplayer-<version>-nexus.zip` (the zip without `F4MPServer.exe`) from the GitHub release: see
docs/RELEASING.md.

## Create draft

- **Mod name:** F4Multiplayer - Co-op over Steam
- **Category:** Gameplay
- **Short description** (350 characters max):

Play Fallout 4 with your friends over Steam: build settlements together, play about 80% of the game's quests together, and fight side by side against the same enemies. Invite friends from the Steam overlay, no port forwarding. Your friends appear as themselves, with voice chat. Everyone keeps their own save. F4SE plugin, alpha.

## Description (BBCode)

```
[size=5][b]Play Fallout 4 with your friends[/b][/size]

Explore the Commonwealth side by side, fight the same raiders, finish quests together and build a settlement as a team. One player hosts and up to three friends join through Steam (more if the host allows it). No port forwarding, no VPN, no IP addresses: invite a friend from the Steam overlay and they drop into your world. Everyone keeps their own save, character and gear.

[size=4][b]Build settlements together[/b][/size]
Stand in the same settlement and build it as a team. What your friend builds or wires appears in the host's settlement as it goes up: the same walls, the same power grid, the same turrets. Hand out jobs to settlers, set up supply lines, scrap and repair, and watch the settlement's food, water, defense and happiness grow for everyone. When raiders come for it, it's one attack, and you hold the walls together.

[size=4][b]Accomplish quests together[/b][/size]
About 80% of the game's quests are shared, the add-ons' included. When one of you moves a quest on, everyone moves with it: side quests, the companions' own quests, radiant jobs, random fights on the road, even the main story and the factions, so you face the big choices as a group. Turn in a quest and everyone gets the reward. Get thrown out of a faction and you're both out. Prefer your own pace? The host can let each player play the main story on their own.

[size=4][b]Fight side by side[/b][/size]
Every NPC is the same in every game: the raider you're shooting is the one your friend is shooting, in the same place, with the same health. Enemies go after any of you, kills count for everyone, and you can see who is in VATS. Take a lethal hit and you go down instead of dying: a friend has 45 seconds to get you back up.

[size=4][b]See your friends in your world[/b][/size]
Your friends appear as themselves: their own face, body, armor and weapons, their mods' gear included, with their name and health overhead. You see them run, aim, reload, throw grenades, climb into power armor and cheer. Talk with Steam voice chat that fades with distance, ping a spot with "over here!", check where everyone is, or teleport to a friend who's wandered off.

[size=4][b]A world that remembers[/b][/size]
The host's PC keeps the shared world between sessions: the NPCs you killed stay dead, the walls you built still stand, and the map fills in with every place anyone discovered. Time and weather follow the host. Disconnect, reload or die, and your game catches up on what happened while you were away.

[size=4][b]Requirements[/b][/size]
[list]
[*]Fallout 4 [b]1.11.240[/b] (Steam, current version)
[*]Windows. Linux through Proton should work but is untested: reports welcome.
[*][url=https://www.nexusmods.com/fallout4/mods/42147]F4SE[/url] 0.7.9
[*][url=https://www.nexusmods.com/fallout4/mods/47327]Address Library for F4SE Plugins[/url]
[*]Everyone needs the same version of this mod, and ideally [b]the same mods[/b] (in any order).
[/list]

[size=4][b]Install[/b][/size]
Install with Vortex or Mod Organizer 2, or extract the zip into Fallout 4\Data. The host sets [b]bHost = true[/b] in Data\F4SE\Plugins\F4Multiplayer.ini; friends change nothing. Always start the game with F4SE. To share the most quests, the host turns on [b]More quests[/b] in the F1 menu. The full guide is in the zip: F4Multiplayer\INSTALL.md.

[size=4][b]Good to know[/b][/size]
[list]
[*][b]Your own game stays yours:[/b] perks, SPECIAL, caps, inventory, weapons and armor, companions' affinity and romance, survival needs and radiation are each player's own. Loot in the world (ammo, chems, food, junk) is shared, and a friend paying in a conversation never charges you.
[*][b]Starting out:[/b] a new character plays the Vault 111 opening alone and joins the shared world on leaving the vault. Alternate-start mods are untried; details in INSTALL.md.
[*][b]Controls:[/b] F1 menu, F6 player list, F7 teleport to a friend, F8 "over here!" ping, F2 to F4 emotes, hold the mouse back button to talk.
[*][b]Mods:[/b] friends may have different mods: what comes from a mod only one player has is left out in the other games, and a joiner is told which mods differ.
[*][b]Languages:[/b] English, French, German, Italian, Spanish, Polish, Brazilian Portuguese, Russian, Japanese, and Simplified and Traditional Chinese (Japanese and Chinese need the game in that language).
[/list]

[size=4][b]Alpha[/b][/size]
Back up your saves first, or keep a separate save for co-op.

[b]Known gaps[/b]
[list]
[*]Building is shared only at settlements the host owns. At a friend's own settlement, each player's pieces stay their own.
[*]Radiant jobs whose enemies are made on the spot (the Castle's defense, a courser, a patrol) stay each player's own.
[*]To give a friend an item, drop it near them: keys, notes, holotapes, bobbleheads, magazines and quest items can't be given.
[*]Some doors a quest script opens may still open only for the player who opened them.
[/list]

[b]Something broke?[/b]
Attach your log to a bug report on GitHub: Documents\My Games\Fallout4\F4SE\F4Multiplayer.log (or F4Multiplayer.1.log if you've restarted the game since). It calls other players "player N" and leaves out Steam IDs and IP addresses. Crash logs and screenshots can still show names, so check those first.

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
