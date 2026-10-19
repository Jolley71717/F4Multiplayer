# Ways forward a script opens: a survey (2026-10-19)

`ScriptDoors` (src/game/ScriptDoors.h) shares a quest stage that opens a door only a script opens, for stages read
off the game's files and found to do nothing but open the way. It started with Vault 114's gear door (DN142's 50).
This survey reads the whole game for every other place where the way forward is opened by a script rather than by a
player activating a plain door, and says for each whether it can go on that list.

Read: Fallout4.esm, DLCCoast.esm, DLCUltraHighResolution.esm and the nine Creation Club plugins installed with the
game (ccBGSFO4044 to ccSBJFO4003), and every compiled script in their archives (8,776 scripts). Of the quests, the
stage fragments; of everything else, the scripts on activators, doors, terminals, triggers, scenes, dialogue and
packages. Calls were followed into the scripts they call (five deep), quest aliases resolved to their references
(forced, unique, and by location ref type within the quest's locations), and linked references (XLKR) resolved to
what they point at.

Looked for: `SetOpen`/`SetOpenNoWait`, `Lock(false)`/`Unlock`/`SetLockLevel`, `PlayAnimation`/
`PlayGamebryoAnimation` and `Activate` on doors, gates, lifts and their activators, `Disable` of collision markers,
blockers and enable markers with a door, wall or debris under them, `Enable` of a way through, `BlockActivation(false)`,
and moving platforms.

Verdicts:

- **SAFE**: a stage of a quest the session doesn't share otherwise, that only opens the way (lights and sounds at
  most), set by something placed in the world. Goes on the list.
- **CARE**: opens the way but also does something else (a scene, an NPC sent somewhere, objectives, another quest's
  stage, enemies let out). Sharing it needs more than the list; the effect in a friend's game is said.
- **NEVER**: main or faction story, the opening, a companion, a settlement or faction takeover, a random encounter,
  or anything each player should see happen in their own game.
- **ALREADY SHARED**: reaches a friend's game some other way: the quest is one QuestSync shares, the stage is set by
  a quest QuestSync shares, the door is a plain door whose lock and open state DoorShare shares, or the activator's
  script answers anyone, so the presser's stand-in pressing it in the friend's game (WorldSync::ApplyRemoteActivation)
  does the same.
- **NOT A PATH**: matched the search but opens no way forward (a safe, a container, a light switch, lights, a
  stage that closes a door), or a stage nothing in the world sets.

## Counts

Quest stages that open something by script (one that opens a way and is set by a quest QuestSync shares counts as
ALREADY SHARED: the friend's game sets it too, when it replays that quest's stage):

| Verdict | Stages | Of which |
|---|---|---|
| SAFE | 1 | DN142 50 (Vault 114, already listed). No new one. |
| CARE | 21 | 11 quests: DN011, DN019JoinCult, DN048, DN050, DN083_Barney, DN102, DLC03KyeDialogue, DLC03MQ02Fight, ccBGSFO4115_DM_Quest, ccFSVFO4007_Quest, ccSBJFO4003_Quest |
| NEVER | 84 | MQ101 (20), DN084 (14), BoS301 (8), Inst307Fight (6), DN017, DNPrime_BoS304, DN136_Attack, FFGoodneighbor01MemorySequence, REObjectKMK02 (3 each), DLC03MQ06_NukeHandling and _WindFarmHandling (4), and 17 more in 13 quests |
| ALREADY SHARED | 322 | 289 stages of 85 quests QuestSync shares (MQ203 22, MS09 18, RR303 17, MS17 12...), and 33 stages of unshared quests that a shared quest sets (DN130 by BoS100, BoS101, BoS201, BoSR01, RR303; BoSEnable by the Brotherhood quests; DN017 by BoS101; DLC03AcadiaDialogue by DLC03DialogueDiMA...) |
| NOT A PATH | 34 | AO_ and CIS_ quests (11), Far Harbor settlement and fog condenser stages (5), safes and containers (5), DN033's breakers (2), DN066 10 and 20 (nothing placed sets them), DN058 17 (closes a door), and 8 others |

The search found 458 stages; DN102's 55, 56 and 58 (CARE) and DN058's 17 were added by hand. Plus, outside quests:
activators and terminals (below). Every unshared stage is in the tables further down, by quest.

## What is not shared today, and how it could be

These are the gaps worth closing, most noticed first. None of them is a quest stage that can simply be listed.

1. **Security gates opened from a terminal.** 96 terminals link a gate with `LinkTerminalSwitchDoor` (SecurityGateSm01
   and Lg01, HitSwitchDoorExSm01 and Lg01, all `Default2StateActivator`), and the terminal's menu item opens it
   (`TERM_nativeSwitchDoorTermina_000DEA36`: `SetOpenNoWait(true)` along the chain of linked gates). The menu runs
   only in the game of whoever uses the terminal, and a gate is an activator, so DoorShare doesn't share its open
   state (`OpensShared` is false for it). In a friend's game the gate stays shut, behind a terminal they may not be
   able to hack. Med-Tek Research's lab, exit and cell doors (TERM 00101A86, 00100D50, 00100EEB) are the same.
   Suggestion: for an activator whose script is `Default2StateActivator` (or extends it), share its open state like a
   door's, and apply it in a friend's game by calling the script's own `SetOpenNoWait` (through the console's
   `cf`/papyrus call, as the stage is set), not the engine's open state, so the script's state and animation agree.
   The linked-ref watch that already exists (`WatchRefStates`) puts the gate on watch when the terminal is used.
2. **Gates behind ID card readers.** `IDCardReaderScript` (28 readers: SecurityGateSm01 and the like) checks that
   the player has the card and opens the linked gate by activating it. The script answers only the player, so the
   friend's stand-in pressing the reader does nothing. Same fix as 1 (the gate's open state), since the gate is
   linked from the reader.
3. **Activators pressed while the friend was elsewhere.** A lever, button or switch whose script answers anyone
   (`Default2StateActivator` on 326 references, `Default1StateActivator` 219, `ToggleButtonScript`, `DoorChain`,
   `securityBarScript`, `DN008GateButtonScript`, `SimpleElevatorMasterScript` 20 lifts, `PowerLiftMiniCartScript`
   16, `CircuitBreakerMasterScript` 30...) is pressed in the friend's game by the presser's stand-in, but only if
   the friend has it loaded (its 3D) and the stand-in is there. Pressed while they were elsewhere, it stays as it
   was in their game, for good: nothing is replayed later. Same fix as 1 covers the gates and doors among them;
   toggles (a lift's car, a breaker) would need their state, not a replayed press.
4. **Lifts and moving platforms.** A lift's car is moved by its script in each game separately; the press is
   replayed (3), so both cars move when the friend has the lift loaded. Each player rides their own game's car; the
   stand-in doesn't ride, it is moved to where its player reports being. Load elevators (`LoadElevatorPanelScript`,
   `loadelevatordoorscript`) teleport whoever uses them: each player rides their own and there is nothing to share.

## Quest stages, by quest

Form IDs are within their plugin (no load-order byte). "Set by" says what sets the stage in a normal game.
"Repeatable": whether the quest allows a stage to be set again (DNAM flag 0x0008); none of these does, so every
stage below is one-way, done once.

### SAFE

| Quest | Stage | Location | Opens | Also does | Set by | Verdict |
|---|---|---|---|---|---|---|
| DN142 (Fallout4.esm 13B747) | 50 | Vault 114, Park Street station | gear door 0B25D1 (VltGearDoor114, "Stage2"); disables the doorway's CollisionMarker 178F07 | klaxon lights on | the console 145DA1 (VaultDoorConsoleScript, player only) | SAFE, listed |

### CARE

| Quest | Stage | Location | Opens | Also does | Set by | What sharing it would do in a friend's game | Suggested way |
|---|---|---|---|---|---|---|---|
| DN050 (Fallout4.esm 045770) | 15, 30, 40 | General Atomics Factory | the Mister Handy test course doors 09B4A4, 09B4A6, 09B4A5 (activators, `SetOpen`) | 30 and 40 start the Proctor's test scenes 02 and 03 and swap the course lights; 15 only lights | 30 and 40: the Proctor's dialogue (TIF 04578F, 045789); 15: nothing found | the Proctor would start the next test in the friend's game, wherever the friend is in it | share the doors' open state (gap 1) instead of the stages |
| DN066 (Fallout4.esm 084CE1) | none: its terminals (not counted) | Med-Tek Research | lab doors 08274D, 08274E, 083313, the exit door, the elevator (`MakeElevatorFunctional`), 12 cell doors | the lab terminal 101A87 also enables the last ghouls (DN066FinalGhoulsEnable); the cell terminal 100EE1 lets out what the cells hold | terminals, menu items | today: the friend's doors stay shut and their elevator dead | gap 1. Stages 10 and 20 open the same doors but nothing placed sets them (their message activators 084CDF and 0860B0 have no references): NOT A PATH |
| DN102 (Fallout4.esm 03479A) | 23 | HalluciGen, Inc. | unlocks and opens DN102_AmnesiacDoor 0E433F (a door with DefaultRefOnOpen, which sets 26) | sends the amnesiac enemy to its package | not found in a script (a scene or dialogue) | the amnesiac walks out in the friend's game | leave: the door is scripted, and the enemy is the host's NPC |
| DN102 | 55, 56, 58 | HalluciGen, Inc. | lab doors 089273, 089272, 089270 (`SetOpenNoWait`, activators) | turns the buttons off, releases the test subjects' hold position, may set 59 | buttons 0AE169, 0AE16A, 0AE16B (`DefaultRefOnActivate`, anyone) and terminal 0E5242 | the buttons are already pressed in the friend's game by the stand-in when they have them loaded (ALREADY SHARED then); otherwise, nothing | gap 3 |
| DN048 (Fallout4.esm 0CC19F) | 10 | Fraternal Post 115 | unlocks the front and back doors 045123, 0CC190 (plain doors) | throws the breaker 119D09, sounds the klaxon 1B9C31, enables the ambushers' marker 03B2DB | trigger in the building | an ambush spawns in the friend's game | none needed for the way: plain doors, their locks are shared once the host uses them |
| DN011 (Fallout4.esm 0FF3C2) | 400, 500, 550 | Boston Public Library | unlocks the load doors 0FF3FB, 0EBEA6; swaps the barred-door chains 100197, 10019A; 550 disables a door and collision set | 400 puts the player in DN011SecurityFaction (the robots stop shooting) and disables the enemy trigger | 500 and 550: triggers 10017D, 10017E (DefaultRefOnTriggerEnter); 400: not found in a script | the friend would be made a friend of the library's robots without doing it | leave: per-player faction; the friend walking through the triggers sets their own 500 and 550, and a load door's unlock is shared by DoorShare once the host uses the door |
| DN019JoinCult (Fallout4.esm 0C5093) | 730 | Charles View Amphitheater | unlocks the amphitheater door (alias AmphitheaterDoor, filled from DN019) | nothing else | the cult's dialogue (not traced) | the friend could walk in without joining | leave: a reward for a choice; the door's lock reaches the friend once the host uses it, if it is a plain door |
| DN083_Barney (Fallout4.esm 1243D6, "Gun Run") | 15 | Barney Rook's bunker | opens the gate (alias GateDoor), unlocks the bunker door | completes objective 6, shows 15, starts DN083_BarneyRunsToBunker, sends Barney to his package | Barney's dialogue (not traced) | the friend's quest would advance and Barney run off in their game | leave: a misc quest with objectives (QuestSync's call, not this list's) |
| DLC03KyeDialogue (DLCCoast.esm 0140FC) | 20 | Vim! Basement | activates the door's dummy 01386D | completes the quest's objectives, blocks the intercom, stops the quest | Kye's intercom talk | ends the friend's talk with Kye | leave |
| DLC03MQ02Fight (DLCCoast.esm 043822) | 100 | Far Harbor | opens the hull gate 00184F, disables its collision 0486F5 | sets DLC03MQ02 300 (the story quest, which QuestSync shares) | trigger 0481F7 (DefaultRefOnTriggerEnter); its 1000, which does the same, is set by DLC03MQ02 and so is ALREADY SHARED | the friend's siege ends with the host's | could be listed after a look in a real game: the story stage it sets is shared already, and the siege quest is started by the shared DLC03MQ02 |
| ccFSVFO4007_Quest (ccFSVFO4007-Halloween.esl 000271) | 30, 45, 50, 100 | the Halloween house | main and side doors, the bathroom boards | objectives, a gas explosion, ghouls enabled | the house's keypad and terminal | the friend's Creation Club quest would advance | leave: a quest with objectives |
| ccSBJFO4003_Quest (ccSBJFO4003-Grenade.esl 00012A) | 30 | Monsignor Plaza | disables the CollisionMarker 000146 (NavCut) | objectives 20 done, 30 shown | the quest | same | leave |
| ccBGSFO4115_DM_Quest (ccBGSFO4115-X02.esl 000845) | 100 | the X-02 bunker | unlocks and activates the gate (alias GateDoor) | lights, objectives, plays a radio, starts ccBGSFO4115_EnclaveRadioQuest | the quest | same | leave |

### NEVER

| Quest | Stages | Why |
|---|---|---|
| MQ101 | 20 stages | the opening: Vault 111, each player's own |
| DN092 (Museum of Freedom) | 77 | enables the deathclaw's hole blocker and sets MQ102 120 (story) |
| DN084 (Mass Fusion) | 4, 10, 20, 200, 210, 225, 230, 242, 260, 350, 360, 369, 370, 399 | the glass elevator (its script sets 200), descent doors and reactor of BoS303 and Mass Fusion (Institute); 210 sets BoS303 80 or InstMassFusion 80 |
| DN017 (Arcjet Systems) | 86, 88, 89 | BoS101's rocket assault and elevator; 89 sets BoS101 290 |
| DN009_MQ205 | 250 | Hunter/Hunted |
| BoS301, BoSEnable, BoSKickOut, DNPrime_BoS304, Inst307Fight, DN136_Attack | 8, 1, 1, 3, 6, 3 | faction story and kick-outs |
| DN130 (Cambridge Police Station) | 80 | the station's state swaps as the Brotherhood moves in and out |
| DN046 (Fort Strong), DN165 (Boston Airport), DN049 (Galleria) | 2, 1, 1 | location takeovers (Brotherhood, settlement) |
| FFGoodneighbor01MemorySequence | 3 | the memory den's set pieces |
| COMCurieQuest, CZ, MS07Intro | 2, 1, 1 | companions (Curie, Cait) and Nick's intro |
| DLC03MQ06_NukeHandling, DLC03MQ06_WindFarmHandling | 2, 2 | Far Harbor's ending |
| DLC03_V118_Intro, DLC03_V118_PearlHiring | 1, 1 | Vault 118's intercom (makes the player talk) and Pearl's elevator, part of the shared DLC03_V118_Quest |
| REObjectKMK02, UFOCrashQuest | 3, 2 | random encounters, each player's own |

### ALREADY SHARED

| Quest | Stages | How |
|---|---|---|
| 85 quests QuestSync shares | 289 | the friend's game sets the same stage and its fragment opens the same door (MQ203's memories, MS09, RR303, MS17, MQ302, MQ106, MQ102, BoS302B, Creation Club's ccOTMFO4001_Quest, DN121, BoS304...) |
| DN017 (Arcjet Systems) | 20, 200 | set by BoS101 ("Call to Arms"): 200 unlocks BoS101LoadDoorRef 0807EC |
| DN130 (Cambridge Police Station) | 20, 30, 40, 50, 60, 90 | set by BoS100 ("Fire Support"; 20 unlocks the front and roof doors 06EAAA, 06EAFD), BoS101, BoSR01 and BoSR02, BoS201, MQ206, RR303 |
| BoSEnable, BoSKickOut, BoSDansePostGame, RRKickOut | 4, 1, 1, 1 | set by the Brotherhood's quests (BoS201 to BoS305, BoSM, BoSR), InstMassFusion, RR303 and RR304, BoS302, BoS302B |
| DN084 (Mass Fusion) | 15, 500 | set by MQ302 750 and BoS303 200 to 220 |
| DN046 (Fort Strong), DN133 (The Castle), DN165 (Boston Airport) | 50; 10; 30 | set by BoS202 20; Min03 0; BoS201 and BoSM02 |
| DN136_Attack, DNPrime_BoS304, Inst307Fight | 10 and 2000; 15 and 1000; 10 and 1000 | set by MQ302 and its Minutemen and Railroad versions; BoS304; Inst307 |
| FFGoodneighbor01MemorySequence, FFGoodneighbor07, MS07Intro | 5 and 10; 10; 400 | set by FFGoodneighbor01, MS16, MS07c |
| DLC03AcadiaDialogue (DLCCoast.esm 0052B7) | 100 | set by DLC03DialogueDiMA 250 (a dialogue quest QuestSync shares): unlocks Acadia's stairwell and Faraday doors 0305C2, 0305C1, starts scenes and DLC03AcadiaM01 |
| DLC03MQ02Fight (DLCCoast.esm 043822) | 1000 | set by DLC03MQ02: opens Far Harbor's hull gate |
| DLC03CoA_FFNucleus03_PumpHandler | 10 | set by DLC03CoA_FFNucleus03 30 |

### NOT A PATH

| Quest | Stages | What it is |
|---|---|---|
| AO_* (7 quests), ActorHello_CIS_Manager, CIS_* (3), Followers | 12 | companions' and NPCs' idle reactions, light switches |
| DN033 | 55, 65 | raiders throwing breakers |
| DN050 | 50 | unlocks the reward safe 19C79F |
| DN066 | 10, 20 | nothing placed sets them (see CARE) |
| DN083_Barney | 30 | enables the mirelurk nest |
| DLC03FarHarborFF02, FF03a, FF03c | 5 | fog condensers and settlement barricades |
| DLC03MQ02Fight | 20 | closes the hull gate for the siege |
| DLC03POIBuriedStash, DLC03PostQuestTreasureDig, DLC03CoA_FFNucleusQuestObjHandler | 3 | treasure and containers |
| ccBGSFO4115_DM_Quest 50, 80; ccBGSFO4115_EnclaveRadioQuest 30; ccBGSFO4116_Quest 15 | 4 | a radio, a container, a terminal |
| ccFSVFO4007_Quest | 40 | enables a collision marker (closes) |
| Patch_1_3 | 10 | the 1.3 patch's fix-ups |
| DN058 (Hardware Town) | 17 | closes the door for an ambush and sets 55 (found by hand) |

## Activators, terminals and triggers

Not quest stages, so not the list's; how each kind reaches a friend today:

| Kind | Examples | Shared? |
|---|---|---|
| Activator scripts that answer anyone | Default2StateActivator (326 refs), Default1StateActivator (219), ToggleButtonScript, DoorChain, securityBarScript, defaultactivatelinkedrefonactivate, DefaultRefEnableLinkedRefOnActivate, DN008GateButtonScript, DN017 and DN066 lab door scripts, CZToggleButtonScript, DLC03_LighthouseElevatorScript | ALREADY SHARED while the friend has it loaded (the stand-in presses it); not otherwise (gap 3) |
| Activator scripts that answer only the player | VaultDoorConsoleScript (Vault 114 listed; Vault 81's console sets V81_00_Intro 15, QuestSync's), IDCardReaderScript (28), DLC03POI04LockedBunkerDoorScript and DLC03POI04_RepairActivateScript, ccFSVFO4007 KeypadScript, DN015CleanRoomButtonScript | not shared: CARE, gap 2 for the gates; the others each need their own look |
| The same, story | DN084 card readers, DLC03_V118_VaultDoorReaderScript, BoS302BWallDetonatorScript, RR303ExplodingDoorTriggerScript, MQ203MemoryExitTrigger, Vault 111's elevator scripts | NEVER |
| Terminals linking plain doors | LinkTerminalDoor (33), LinkTerminalDoorLockUnlock (13), others (11) | ALREADY SHARED: the linked door is put on watch when the terminal is used, and its lock and open state are shared |
| Terminals linking gates | LinkTerminalSwitchDoor (96 gates), LinkTerminalDoor to an activator (3), Med-Tek's terminals | not shared: gap 1 |
| Triggers that unlock or open as the player walks in | DefaultUnlockDoorOnTriggerEnter, BunkerHil_UnlockGateScript, DNFinancial21_TriggerScript, ccBGSFO4115 DoorUnlockTriggerScript | each player's own: the friend walking in sets off their own |
| Load elevators | LoadElevatorPanelScript (160 refs), loadelevatordoorscript (87), ElevatorDoorForwardActivationScript | nothing to share: each player rides their own |
| Lifts and platforms | SimpleElevatorMasterScript (20), PowerLiftMiniCartScript (16), DN084_GlassElevatorScript (story), DLC03 boats | the press is replayed while loaded (gap 4) |

## Notes

- Plain doors a stage only unlocks (DN048, DN011's load doors, DLC03AcadiaDialogue, DN019JoinCult) reach the friend
  through DoorShare's lock state, but only once the host activates the door: a door is watched after a player uses
  it. A load door is read on the tick after the activation, before the loading screen; not checked in a game.
- Dialogue that sets a stage: some topic infos (DN050's Proctor lines) have compiled fragments but no script
  attached to the INFO record in the plugin; the setter was found by the fragment's name.
- A stage nothing sets is dead content: DN066 10 and 20, and DN050 15 as far as the files show. Listing one would
  do nothing and was not done.
- The scanner (an ESM, BA2 and Papyrus bytecode reader with a call tracer) was a one-off and is not in the
  repository; `tools/questscan.cs` covers the quest side.
