## 0.10.0-rc1

A test build of 0.10.0. Install it over 0.9.2. It uses a new protocol (55), so it doesn't play with 0.9.2 or
anything older: everyone in a session needs it, and a friend still on an older version can't join. The host's
session file from 0.9.x still loads.

New:

Shared storage (experimental, host setting `bSharedStorage`, off by default):

- A container in a home or settlement has one copy for everyone, the host's. A friend who opens theirs sees what the
  host's holds, and what one player takes is gone for the others.
- Ammo, aid and junk are shared this way. Weapons, armor, keys and notes stay in each player's own copy.
- A container is shared once the host has been near it in the session. A chest that rolls its loot as you open it gets a
  friend's waiting takes as the menu opens.
- A take that loses a race to another player's is undone: you get nothing and the item stays in the container.
- Loading a save takes nothing from a friend and gives them nothing; the host's copy is put right instead.

Giving items (experimental, host setting `bContainerGifts`, off by default):

- Put a weapon or armor piece in a chest, dresser or safe (not a workbench, a vendor's or a companion's) and it is a
  gift: it shows in that container for every friend, and the first who takes it has it. Until then you can take it
  back. Mods, legendary effects, names and a power armor piece's condition go with it.
- A new character no longer gets back the gifts an older one took.
- A gift shows in an open chest at once.
- It works with shared storage on or off.

Power armor borrowing:

- A friend can borrow a power armor frame your game made (one you found or looted) once it stands parked. They press E on
  it to ask and E again to get in. They wear your pieces and fusion core, named as yours ("Sam's T-60 Left Arm"), and
  everyone sees one suit with the friend inside.
- When they get out, the frame comes back to you where they left it, worn as they left it. The pieces stay yours.
- It stays with you if the loan ends any other way (either of you leaves or loads a save). A borrower still inside
  keeps it until they get out. A borrower who loads a save made in a lent frame gets out of it, and the lent pieces come
  off them.
- Your own setting `bLendPowerArmor` turns it off for your frames.
- A refused frame says why: the session's gate, a block, the input layers, or the player's own state.

Combat and downs:

- Crawl while you wait for help (experimental, host setting `bDownedCrawl`, off by default): forward and back with your
  movement keys, left and right turn you. You stop while a friend is next to you. Walls, ledges and steep ground stop you.
- Autosave when you go down (host setting `bSaveOnDown`, off by default). A death loads the game's newest save, which can
  be many minutes old; with this on, everyone's game saves as they go down, so a bleed-out takes them back only to that
  moment. A death that loads that save puts you back where you went down, up with the revive health.
- A teleport to a friend makes a travel autosave (if the game's travel autosave setting is on). The HUD now says that a
  death loads the last save.
- A joiner is told the host's rules for being down when they join, and the player list shows them. A down finishes under
  the rules it began with.
- A friend's flamer stream, cryolator spray, harpoon or railway spike goes where they aimed, not at you.
- A helped-up player gets up the game's own way at once, and stands out of sneak.

NPCs and the world:

- An NPC alive in the game that runs it is alive everywhere. Damage a friend deals that the other game can't apply yet is
  kept and applied when it can. An enemy out of sight in your game still hurts you.
- Two players shooting one NPC no longer pass it back and forth every few seconds: it stays with whoever hit it first.
- Friends' copies of NPCs stand still while the NPC's owner is in the Pip-Boy or a menu, instead of walking off and
  jumping back. A game stopped in a menu for 5 seconds hands its NPCs to a friend nearby, so they don't freeze.
- A friend's explosion no longer leaves enemies unhittable for good, and a legendary weapon's bleed or burn on a copy
  isn't healed back.
- Doors and gates a script opens or unlocks reach friends without anyone touching them, including Vault 114's.
- The Prydwen moors at Boston Airport in every game once one game's flyover ends.
- Knocked clutter lands much closer to the same place in every game, including balls and cans that roll on. The server
  keeps where moved objects came to rest, so a game that loads the cell later puts them there too.
- Explosive props whose last stage only takes them away (a gas canister) are shared like a barrel.
- An enemy an older save kept as a ghost is repaired after a load, so shots hit it again.
- A companion that followed its player into an interior is brought to them there, so the host's Dogmeat shows indoors.
- A scene a friend starts in your game no longer holds their later stages back: Codsworth's search finishes for the
  host too.

Settlements:

- The host can bring in what a friend built before the session (host setting `bShareGuestBuilds`, off by default). A
  second friend's report of the same piece makes no second copy.
- A friend's build is placed only once the cell it stands in is loaded for you, and doubled copies a save holds are
  removed.
- A power armor frame moved in workshop mode is no longer sent as building.

How friends look and move:

- A friend who starts running or walking from standing moves at the clip's pace at once, not the slowest rate.
- A friend's holstered gun no longer stays out on their stand-in, and a grenade stuck in its hand is put away.
- A friend's stand-in on your very spot steps aside so you can walk off it.
- A friend in power armor whose frame your game doesn't have is shown in a frame every game has, never out of their suit.
- A face over the sculpt-slider limit now drops the same highest ids every time.
- A friend's LooksMenu looks no longer end up on whatever later takes the form ID in your save.
- A plain armor piece is sent as plain, so a friend's game no longer rolls its own mods onto it (the Raider chest).

Quality of life:

- Friends' voices: `fVoiceRange` now only sets how far away you can hear a friend; how far your own voice carries isn't
  changed.
- A new character with no name yet can't host or join under the last character's session.
- The F1 menu's Compare mods no longer names the HD texture pack.
- Settings in the install guide are split into the host's and your own, with when the ini is read.

The dedicated server:

- It tries a failed session save again on its own, with a growing wait, and once more when it stops.
- It takes the new host rules as options: `--container-gifts`, `--downed-crawl` and `--save-on-down`. It never shares
  storage: there is no host game to keep the copy.

Stability:

- A down survives a dropped connection, and a friend's first shot on a copy is kept.
- A friend's take from a chest your game holds without its inventory yet waits for the chest's roll instead of taking
  nothing.
- A friend's emptied chest or corpse takes only the leftovers of what they took; your own roll, a mod's items and your
  stash stay.
- A frame copy 0.9.1 left in a save goes, and the ghost copies older versions made are cleared.
- Disabled power armor frame copies a save kept go after a load.
- A legendary only in one game can't heal itself on a hit.
- A hit in the same second as your own Molotov or grenade no longer counts the blast and burn twice.
- A blast an NPC set off names that NPC, so a game without it (a DLC it doesn't own) isn't hurt by it.
- A friend's move of an object whose kept place still waits in your game is judged from that place.
- A drop record is no longer also sent as a workshop power report.
- The sneak key works again after going down sneaking and being helped up.
- The log says why a friend never saw a hostile NPC your game made, why copies walk off or change hands, which sounds
  the plugin makes the game play and when one repeats, and what each save and load was.

New settings in `F4Multiplayer.ini`:

- `bSharedStorage = false` (host's rule)
- `bContainerGifts = false` (host's rule)
- `bDownedCrawl = false` (host's rule)
- `bSaveOnDown = false` (host's rule)
- `bShareGuestBuilds = false` (host's rule)
- `bLendPowerArmor = true` (your own)

The game reads the ini when it starts.
A session that has used shared storage or gifts can't be opened by an older version of the mod afterwards.

Known issues, and not in this release yet:

- Not yet: the Pip-Boy friend map, seeing a friend at a workbench, a raised Pip-Boy and a menu marker over a friend, and
  the distance on the compass.
- Not yet: other factions' radiant quests and settlement attacks, whose enemies or helpers are made on the spot.
- Shared storage and gifts are experimental. Right after the host loads a save, a friend's menu on a shared container
  closes until the host has been near it again.
- A gift has one case it can't put right: the giver loads a save from before they put it in, after a friend took it,
  and then both have it.
- A borrowed frame doesn't carry on after the borrower loads a save: the loan ends and they are sent out of it.

For testers:

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you
  restarted the game since) and the crash log; the first lines of the plugin log name the build. The plugin log names
  players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
