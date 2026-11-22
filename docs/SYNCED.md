# What is shared in co-op

Each of you plays in your own game, with your own save. The mod keeps your games matching where it counts: the
same enemies, much of the same loot, the same quests and the host's settlements. This page lists, system by system, what is
shared between players and what isn't. It is written for version 0.21.0.

What each word means:

- **Synced:** the same in every player's game.
- **Partly:** some of it is shared; the line says what isn't.
- **Your own:** each player keeps their own, on purpose (your character, your save, your companion's affinity).
- **Not yet:** not shared today. Something we know about and may fix later.

Settings named here, such as `bSharedStorage`, are the host's, in `F4Multiplayer.ini` or the F1 menu, unless the line
says otherwise. [INSTALL.md](../dist/INSTALL.md) explains each one.

## Players

| What | Status | Details |
|---|---|---|
| Walking, running, sneaking, jumping | Synced | Friends are drawn a fraction of a second behind, so their movement stays smooth. |
| Face, hair, body | Synced | Their own face, sculpting and body shape. With LooksMenu on both sides, its body sliders and overlays too (not tried yet). If another mod has changed how the game builds faces, friends get a settler's face instead. |
| Armor and weapons worn | Synced | With the mods on each piece. Gear a script mod made while playing shows without that item or mod. Nothing a friend wears is in your game to loot. |
| Shooting, reloading, aiming | Synced | With the gun's own sound and muzzle flash. Friends' bullets, tracers and laser beams are seen in flight and do no damage. Many rounds of an automatic weapon show no tracer. |
| Melee swings, punches, grenade throws | Synced | |
| Gun bash (hitting with a gun's butt) | Not yet | Friends don't see it. |
| Missiles, mini nukes, explosive rounds | Synced | Seen in flight; the blast happens once in every game. |
| Name and health over their head | Synced | The name when you look at them, the health when they're hurt. F6 lists everyone's health and level. |
| Getting hit by enemies | Partly | An enemy that a friend's game runs hurts you through their game. You lose the health (scaled by your own difficulty), but your perks and legendary armor effects don't count against that hit, and it does no limb damage, radiation or stagger. |
| Going down and being helped up | Synced | A lethal hit knocks you down; a friend has 45 seconds to help you up. The host sets the rules. |
| Crawling while down | Synced | The host's `bDownedCrawl`, off by default and experimental. |
| Dying | Your own | Dying loads your own last save. The host can have everyone autosave as they go down (`bSaveOnDown`). |
| Power armor | Synced | Friends see you get in and out and wear it. A frame you park stands where you left it in their game. |
| Borrowing a friend's power armor | Partly | You can ask to borrow a friend's parked frame (their own `bLendPowerArmor`, on by default); it goes back to them where you get out. The loan ends if either of you leaves or you load a save. A piece with a mod made while playing isn't lent. |
| VATS | Partly | VATS runs in real time in a session, so friends don't stand frozen. The host can keep the game's slow motion, or slow everyone together while anyone is in VATS. VATS never picks a friend (your own `bVatsSkipsFriends`). |
| Emotes (F2 to F4) | Synced | Point, cheer, clap. Not in power armor. |
| Ping (F8) | Synced | An enemy, an item, a friend or a spot, on everyone's compass for 15 seconds. |
| Voice chat | Synced | Steam voice, full volume up close and fading with distance. |
| Sitting and lying down | Partly | Chairs, couches, benches, stools, beds and sleeping bags show. At a workbench, a terminal, a cooking or chemistry station or a work spot, a friend just stands. A vertibird seat isn't shown. |
| Lockpicking, hacking, looting, talking, crafting | Not yet | No animation: a friend just stands there, and their tag says "[in a menu]". In the Pip-Boy their tag says "[Pip-Boy]" and they glance at their wrist now and then. |
| Pip-Boy light | Synced | |
| Level, SPECIAL, perks, caps, inventory | Your own | Nothing a friend does changes your character: no item, perk or point is taken from you or put on you for them. |
| XP | Partly | You get half the XP of a friend's kills. A player behind the group's average level earns a little more (catch-up XP, the host's `iCatchUpXp`). Quest XP comes with the quest in each game. |
| Survival needs, radiation, chems, difficulty | Your own | Jet slows only your own game. Your difficulty sets the damage you deal and take. The host picks the level enemies are built at and the chance of a legendary for everyone. |
| Teleport to a friend (F7) | Synced | Not in a conversation, a scene, furniture, a memory or Survival. |
| Fast travel together | Not yet | Each of you fast travels on your own. Places one of you discovers are on everyone's map. |

## Combat and enemies

| What | Status | Details |
|---|---|---|
| The same enemies in every game | Synced | Each NPC is run by one player's game and copied in the others: the same raider in the same place, with the same health. |
| An NPC's gear | Partly | A copy wears what the NPC wears in the game that runs it when your game's copy carries the same items; otherwise it wears its own. Its body holds your game's own loot. |
| Who enemies go after | Synced | Any player. |
| Kills | Synced | Dead for everyone. The kill message names who did it. |
| Leveled enemies and legendaries | Synced | Built the same in every game, at the level the host picks (`sNpcLevel`, the group's average by default). |
| Random encounters and ambushes | Partly | Shared, fights and calm meetings alike (traders, travellers). Only the player whose game runs it can talk in it; the others see and hear it. Up to 32 such NPCs per player, 128 per session, are shared at once. |
| Enemy idle and work animations | Not yet | An NPC run by a friend's game walks and fights, but doesn't play its idle, work or sitting animations (hands up, cowering, sitting) in your game, or greet you. Talking to it works. |
| An enemy whose player opens a menu | Partly | Its copies stand still for a few seconds, then a friend's game nearby takes it over. |
| Two hits at the same moment | Not yet | If you and a friend hit the same enemy in the same instant, one of the hits can be lost. |
| Explosions | Synced | Each goes off once in every game: grenades, mines, traps, cars, barrels, robots' and turrets' death blasts. |
| Limbs blown off | Not yet | Shown only in the game where it happened, for any creature. |
| Friendly fire | Synced | Off by default (the host's F1 menu). A friend's grenade or Fat Man shell that lands at your feet hurts you even with it off. |
| Group difficulty | Synced | The host's `iGroupDifficulty` makes enemies tougher for each extra player (off by default). |

## NPCs and companions

| What | Status | Details |
|---|---|---|
| NPCs and what they do | Synced | Where they are and who they fight. A friendly NPC passes to your game when you stand next to it and the friend whose game ran it is far away. |
| NPCs in power armor | Not yet | An NPC in a power armor frame in its own game isn't put in the frame in a friend's game. |
| Conversations | Partly | A conversation plays in one game. Friends nearby see its subtitles and hear its lines (their own `bSpokenLines`). Its choices reach the others when they move a quest on. |
| Companions | Partly | One owner each: the first to recruit a companion keeps them, and a second player's recruit of the same one is dismissed with a notice. A companion walks with their owner in every game. Dogmeat too. |
| Companion affinity, romance, inventory | Your own | Each game keeps its own. |
| Robots built at a robot workbench | Partly | Their parts and paint look the same for everyone. Each is its builder's. |
| Vendors and trading | Your own | Each game's vendors have their own stock and caps. |
| Pickpocketing, an NPC's inventory | Your own | |
| Dead NPCs coming back | Partly | They stay dead for everyone until the host's game resets that area. On a dedicated server they stay dead for the whole session. |

## Quests and dialogue

| What | Status | Details |
|---|---|---|
| Side quests | Synced | When one of you moves a quest on, everyone moves along (never backwards). A joiner is caught up on the host's. |
| Main story and factions | Synced | By default one story for everyone. The host can let each player play the story on their own (`sStory = own`). Make big choices together: if two of you side with rival factions, both games get both results. |
| Misc quests, and Far Harbor's and Nuka-World's own stories | Partly | Shared with the host's *More quests* switch (about 80% of the game's quests then). Without it, each player's own. *More quests* only works with the shared story (not with `sStory = own`). |
| Quests left out even with *More quests* | Your own | Quests whose targets each game picks, repeatable quests, quests that teleport you or deal with your own gear or rank, survival mode's and your dog's. |
| Radiant jobs | Partly | Shared with the shared story (and everyone's own `bSharedRadiants` on): the Minutemen's settlement requests and Preston's defense jobs (the host's), the Brotherhood's, the Institute's and the Railroad's repeatable jobs (those with someone to escort, rescue or kill included), Diamond City's wanted posters, Far Harbor's settlement requests. Everyone gets the same job for the same place. |
| Radiant jobs not shared yet | Not yet | The Castle's defense, the Minutemen's refugees, the Railroad's High Ground, and Nuka-World's cache and rival-boss jobs and settlement raids. |
| Quest rewards | Synced | When one of you turns a shared quest in, everyone gets the reward once. Not yet: rewards that a scene or a terminal hands out. |
| Companion quests | Partly | Curie's Emergent Behavior, MacCready's Long Road Ahead, Cait's, Nick's Long Time Coming and Danse's Blind Betrayal are shared. Each companion's recruiting and affinity are their owner's, and Dogmeat's and X6-88's quests are each player's own. |
| Memories (Kellogg's, DiMA's) | Your own | The player in the memory lounger plays it; friends are never pulled in. |
| A quest step that moves you | Partly | It happens only near the friend who did it. Farther away, you're offered to follow (hold F7). |
| A faction turning hostile | Synced | It waits until you're out of reach of that faction's people, then the HUD tells you whose choice it was. |
| Console quest shortcuts | Your own | A quest stage set from the console stays in the game of the player who typed it. |
| Quest terminals | Partly | Easy City Downs' race terminals are shared. Other terminals whose choices a quest keeps aren't yet. |
| Diamond City's synth accusation | Partly | Plays once for everyone. Only the player whose game runs it can talk to the two men. |

## World

| What | Status | Details |
|---|---|---|
| Doors | Synced | Opened, closed, locked and unlocked. A door a script or a quest opens nearby too. |
| Gates and vault doors | Partly | Security gates opened from a terminal, a card reader or a button, and Vault 114's gear door, open for everyone. Some other doors a quest script opens may still open only for the player who opened them. |
| Elevators | Not yet | Each game runs its own elevator. Doors that open by themselves, the Mechanist's lair doors and Pulowski shelters aren't shared either. |
| Locks | Synced | A lock one of you picks, or opens with a key or a terminal, is open for everyone. |
| Terminals | Partly | What you read on a terminal is your own. What it opens or unlocks is shared. |
| Loot in containers and on bodies | Synced | Ammo, aid, food, junk and caps: what one of you takes is gone for the others. Each game rolls a chest's loot on its own, so two copies can hold different extras. |
| Weapons and armor in containers and on bodies | Your own | Each game keeps its own. A friend's looted gun stays on your copy of the body. |
| Keys, notes, holotapes, bobbleheads, magazines | Your own | Each of you collects your own. |
| Items lying about | Synced | Picked up by one of you, gone for all. |
| Things you pick up, carry and throw | Synced | Junk, bottles, tires and the like, and what players knock about. What a blast throws lands differently in each game. |
| Plants you pick | Synced | They grow back on each game's own schedule. |
| Buttons, switches, levers | Synced | A friend's press is made in your game by their stand-in. |
| Time of day | Synced | Everyone follows the host's clock. |
| Sleeping and waiting | Partly | The host's sleep or wait moves everyone's clock on. A friend who sleeps or waits is brought back to the host's time of day. |
| Weather | Synced | The host's, while you're outside in the same land as the host. |
| Cars, barrels, fuel tanks, gas pumps | Synced | Blow up once and stay wrecked in every game. Wrecks from before the session stay as each save has them. Gas canisters aren't shared yet. |
| Traps | Synced | Mines, tripwires, gun, grenade, flame, arc and radiation traps, IEDs, Far Harbor's bear traps: set off, disarmed or taken for everyone. The XP for disarming is the disarmer's. |
| Mines | Synced | A mine already in the world that a friend sets off beeps the whole countdown in every game. A mine picked up is gone for everyone. |
| Pressure plates, thrown mines, traps built in a settlement | Not yet | Each game's own. A mine a friend throws is theirs alone. |
| Areas that reset | Partly | When the host's game resets a cleared place, its enemies are back for everyone. |

## Settlements and building

At a settlement the host owns, with the host's `bSharedWorkshop` (on by default):

| What | Status | Details |
|---|---|---|
| Building | Synced | What a friend builds becomes the host's piece: on the host's power grid, counted in the host's settlement and kept in the host's save. The host must be nearby. |
| Turrets and robots a friend builds | Not yet | They stay the friend's own pieces. |
| Scrapping, storing, repairing | Synced | Done by the host's game (`bSharedWorkshopEdit`, on by default). Scrap goes to the host's workbench, which also pays for repairs. Only fully destroyed pieces can be repaired. |
| Moving pieces | Not yet | Nobody moves another player's pieces, and a friend's pieces there are the host's. |
| Snapping to another player's pieces | Not yet | |
| Wiring and power | Partly | A friend can wire the host's pieces. A wire from a friend's own piece to a host's piece is refused, and nobody scraps a host's wire on its own. |
| Settlers' jobs | Synced | Friends can give the host's settlers a bed, a crop or a guard post. |
| Supply lines and sending settlers | Synced | With the host's Local Leader. |
| Settlers the host recruited | Partly | Friends see them while the host's game has them loaded (up to 32). |
| The settlement's figures | Partly | The workshop bar shows the host's food, water, power, defense, beds and happiness. The Pip-Boy's workshop list still shows your own. |
| Workbench materials | Partly | Each player's own, unless the host turns on `bSharedStorage` (experimental, off by default). |
| Attacks | Synced | The host's alone (`bSharedAttacks`): one attack, fought together. A friend there while the host is away sees no attackers, and it ends off-screen. |
| What a friend builds there, in their own save | Not kept | It's the host's: it isn't kept in the friend's save. |

Anywhere else (a friend's own settlement, or with the shared workshop off):

| What | Status | Details |
|---|---|---|
| Building | Partly | What a friend builds shows in your game as a plain copy: you can't scrap, store, move or wire it, and your settlers can't work it. |
| Power | Partly | Shown, not shared: a friend's lights and machines light up as their game powers them, but their generator never powers your lamp. |
| Food, water, defense | Partly | A friend's crops, pumps, turrets and generators count in yours once you've seen them. Beds don't count. Happiness, population and supply lines are each game's own. |
| Attacks | Your own | Each game runs its own. |
| Crafting stations | Partly | A chemistry, cooking, armor or weapons station uses the junk of the settlement it stands in, in your game. |
| Conveyor belts | Partly | They run or stop as their builder's game says. What rides on them isn't shared. |
| Building done before the session | Your own | A guest's earlier building stays out of the host's world unless the host turns on `bShareGuestBuilds`. |
| Dedicated server | Not yet | No shared workshop: everyone builds their own pieces, and attacks are each player's own. |

## Items and trading

| What | Status | Details |
|---|---|---|
| Giving an item | Synced | Drop it from your Pip-Boy near a friend: it moves from your game to theirs with its mods, legendary effect and name, and only one of you can take it. |
| What can't be given | Not yet | Keys, notes, holotapes, books, bobbleheads, magazines, quest items, items a mod made while playing, and an item you found lying in the world and picked up before the session (or before your last load). Things you looted, bought or crafted can be given whenever you got them. |
| Giving through a container | Partly | Weapons and armor put in a container stay in your game, unless the host turns on `bContainerGifts` (experimental, off by default). Ammo, aid and junk in a container are shared loot anyway. |
| Caps | Partly | Put them in a container for a friend (caps can't be dropped). |
| Trading with a friend | Not yet | You can't talk to, trade with or hand things to a friend's stand-in. |
| Weapon racks (Contraptions) | Partly | Friends see the weapons on your racks where their game has the same rack, but can't take them. |
| Loading an old save after giving | Not yet | Loading a save from before you gave an item a friend has since taken leaves you both with it. |

## Pip-Boy and map

| What | Status | Details |
|---|---|---|
| Discovered locations | Synced | A place one of you discovers is on everyone's map, ready for fast travel (your own `bShareMap`). |
| Friends on the map | Synced | The world map and the local map show friends with their name. You can't fast travel to a friend there (use F7). |
| Compass | Synced | A marker in each friend's color, in the same interior or the same land. |
| Map mods | Not yet | A mod's own map page that doesn't line up with the game's map shows no friends. |
| Inventory, stats, perks, quests list | Your own | Your Pip-Boy is yours. The quests in it move with the shared quests. |
| Radio | Your own | |

## Saves

| What | Status | Details |
|---|---|---|
| Your save | Your own | What happens in a session goes into your save when you save. Friends' stand-ins are never saved. |
| The shared world between sessions | Synced | The host's PC keeps it: dead NPCs stay dead, buildings stand, the map stays filled in. |
| Catching up | Synced | Joining, reconnecting, loading a save or dying catches you up on what happened meanwhile. |
| The host loading an older save | Partly | Something the host killed alone after that save comes back for everyone. What a friend saw die stays dead. |
| The host's game crashing | Partly | The session carries on from the last time it was saved to disk; what came after can be lost. |
| The opening (Vault 111) | Your own | A new character plays it alone and joins the shared world on leaving the vault. |

## DLC areas

| What | Status | Details |
|---|---|---|
| Far Harbor, Nuka-World, Automatron | Synced | Their NPCs, loot, doors and traps are shared as in the Commonwealth. The map and compass show friends in the land you're in. Everyone needs the add-on. |
| Their quests | Partly | Their side quests are shared like the base game's. Far Harbor's and Nuka-World's own stories need the host's *More quests*. |
| DiMA's memories | Your own | |
| Nuka-World raider jobs | Not yet | The cache jobs, the rival-boss jobs and settlement raids stay each player's own. |
| The add-ons' settlement attacks | Synced | Far Harbor's, Automatron's and Nuka-World's attacks on the host's settlements and outposts are the host's, as other attacks. |
| Automatron's Mechanist lair and track system doors | Not yet | They open and close in each game on their own. |
| Workshop add-ons (Contraptions, Vault-Tec, Wasteland Workshop) | Partly | Their pieces are shared as any settlement piece. Conveyor belts run as their builder's game says, but what rides on them isn't shared, and Contraptions' baseball pitcher isn't either. |
| Mods only one player has | Not yet | What comes from a mod only one of you has (an NPC, an item, a quest, a piece) is left out in the other games. The F1 menu's *Compare mods* page names the mods that differ. |

Something here wrong, or missing? [Report a bug](https://github.com/Jolley71717/F4Multiplayer/issues/new?template=bug_report.yml)
or [propose an idea](https://github.com/Jolley71717/F4Multiplayer/issues/new?template=idea.yml).
