# Shared rolls: one leveled NPC for the whole session

Protocol 57, session file layout 41. Code: `src/game/SharedRoll.h` (pure, tested in `tests/SharedRollTests.cpp`),
`src/game/RollHook.cpp` (the engine side), `common/SessionLevel.h`, `common/Server.cpp` (rolls and level kept),
`src/game/Spawns.cpp` (`PlaceCopy`). Probes for the rig: `tools/rollprobes.ps1`.

**Not tested in a game yet.** Every address below was found by reading the 1.11.240 executable, not by running it.

## The problem

Fallout 4 builds a leveled NPC (a raider placed from a leveled list, a bloatfly, a ghoul) the first time a game needs
it. It rolls the list and the legendary chance at that game's player's level, with that game's dice. Five players
walking into one place made five rolls, so each game had its own raiders: one game had a legendary where another
had a plain one, or a Veteran where another had a Psycho. Protocol 50 had the game that runs an NPC report its roll,
and the others wrote that roll on their copy. But the report came seconds after every other game had already built
its own, and a built NPC can't safely be rebuilt mid-fight, so the roll only took at the next cell load. A spawn's
copy (a random encounter, an ambush) was placed from the leveled base and rolled again. A scripted scene that places
its NPCs at runtime in each game, with 5 players, became 10 raiders where 2 belonged.

## What every game does now

Every game builds the NPC the same way the first time. It doesn't wait for anyone's report. In order of preference:

1. **A spawn's copy.** `Spawns::PlaceCopy` arms the owner's roll on its own thread (`RollHook::Placement`) around
   `CreateReferenceAtLocation`. The resolver then builds our copy as that roll. `net status` shows
   `copiesRolled=consumed/armed`; one that is armed and not consumed is logged.
2. **The session's roll of a plugin's NPC.** The server keeps it for the session's life, not only while someone runs
   the NPC. It goes when the NPC dies. It is saved in the session file and sent to a joiner with the welcome. Rolls
   from our own game's reports are kept too, while the session has none. A roll is used only when the NPC it was
   placed as matches ours and each template is an NPC in our game. Otherwise the engine's roll stands
   (`rollsNotWhole=`).
3. **The session's pick.** This is `SharedRoll::Pick`: the engine's resolver over the game's own data, with our dice.
   The seed is the session id, the reference's key and an ordinal:
   - A plugin's reference is keyed by its plugin's file name (any case) and its id within the plugin, so the key is
     the same whatever the load order.
   - A runtime reference (FF) is keyed by the NPC it was placed as, its cell or worldspace, its spot on a 256-unit grid,
     and the how-many-th of the same at that spot within 10 s.
   - The lists are read at the session's NPC level.

   It is made only when the session has a level and the rule isn't `own`.

Each build is logged for a session's first 32, for example
`RollHook: 0001F0B1 built as the session's pick of 0001A000 ...`. Counters are in `net status` (`rolls` on the dev
console): `sessionRolls rollsForced rollsPlaced rollsPicked rollsMissed rollsNotWhole rollsUnconsumed rollLevels
rollsPrewritten rollsDiffer rollFaults npcLevel npcLevelRule`. `rolls show <ref>` gives the roll the session has for
a ref, and `rolls pick <ref>` gives what the pick would be now.

**Pre-write.** A session roll that arrives for a reference in memory that is not loaded is written onto it straight
away (`Rolls::Apply`, 8 a frame on the main thread). It takes at the next cell load. The reference must have no 3D,
its cell must not be attached, and it must already be leveled.

**The report becomes a check.** The runner's report still goes out. A copy's roll that differs from the runner's is
logged once per NPC (`NpcSync: X differs from player N's roll of it`) and counted (`rollsDiffer=`). With one roll per
session this count should stay at 0. Protocol 50's write-after on the next load is kept as the fallback.

## The session's NPC level (`sNpcLevel`)

The host's rule, in `[Host]` `sNpcLevel` (the dedicated server takes `--npc-level`):

- `highest` (the default): the highest level any player has had in the session. It never goes down, so an NPC rolled
  at it stays that NPC, and a late joiner new to the game meets the same raiders as the others.
- `host`: the host's level, whoever else is there. It is kept while the host is away. A dedicated server has no host
  game, so it uses `highest`.
- `own`: there is no session level, and each game rolls at its own player's level, as before. Session rolls and spawn
  rolls still apply. Only the pick is off.

The server works the level out from the players' statuses, sends it in `kSessionWorld` (177) whenever it changes and
right after each welcome, and keeps it in the session file. The rule travels in the welcome as a byte after the
limits.

Players see the difference: under `highest` a level-5 player walking with a level-30 friend meets level-30 raiders.
That is the cost of seeing the same NPC. `own` is there for a host who would rather not.

`bSharedRolls = false` in F4Multiplayer.ini turns the hooks off: every game rolls its own NPCs again, and protocol 50's
report and write-after still apply. It is a kill switch and is not in the shipped ini.

## How the engine does it (1.11.240, by Address Library ID)

| What | ID | Offset | Confidence |
|---|---|---|---|
| TESActorBaseData's leveled-actor resolver | 2192884 | +309B10 | high |
| its call of `IsLeveledCreatureResolved(ref)` | (resolver +0xCB) | | high |
| `TESObjectREFR::IsLeveledCreatureResolved` | 2202656 | +5639E0 | high |
| TESNPC's `TESActorBaseData::CopyFromTemplateForms` | 2207448 | +6580A0 | high |
| ... its slot in `VTABLE::TESNPC[1]` (the TESActorBaseData at +0x68) | 1220548 | slot 7 | high |
| `TESObjectREFR::SetLeveledCreature` (resolver +0x607 calls it) | | +563A10 | high |
| `TESObjectREFR::GetEncounterZone` | 2202627 | +562950 | high |
| the default encounter zone | 4798032 | | medium |
| `BGSEncounterZone` level for the player | 2199472 | +494EC0 | high |
| ... its scale above the minimum: `mulss xmm1, [rip+disp]` at +0x4A | | | medium (bytes checked at install) |
| `ExtraDataList::GetLevCreaModifier(float*, int*)` | 2190331 | +284C90 | high |
| the game's difficulty | 2233056 | +D6D600 | high |
| `GetLegendaryMults(difficulty, float* chanceMult, float* countMult)` | 2209078 | +6CEEA0 | medium |
| `CalculateCurrentFormListForRef` | 2193260 | | high |
| the actor's level for the list (`GetCalcLevel(ref, useModifier)`) | 2202630 | | high |

**The resolver.** `void Resolve(TESActorBaseData* original /* npc+0x68 */, TESObjectREFR* ref, bool skipInit)`, Microsoft
x64 (rcx, rdx, r8b). At +0xCB it calls `IsLeveledCreatureResolved(ref)` and returns at once on true. After that call
it has no early return before the build. So a decision armed in hook 1 is always consumed by hook 2 on the same
thread. A decision still armed at the next resolve on that thread is dropped and counted
(`rollsUnconsumed=`, which should stay at 0).

**The legendary decision.** It is made only for a ref that is not dead (`IsDead(false)`, vfunc 0x600) and not deleted
(formFlags bit 5). A cooldown counter at +30EB498 is compared with `countMult x` a game setting, and then
`random(0,100) < legendChance->value x chanceMult` is drawn. The multipliers come from the difficulty.

**The build.** In order:
1. `CreateFormOfType(NPC_)`.
2. vfunc 0x1B0 copies the original into the new NPC.
3. `call [r8+0x38]`, that is `CopyFromTemplateForms(newNpc+0x68, templates)` at +0x5C6. The templates are the 13
   rolled slots on the stack at rbp-0x20.
4. `SetLeveledCreature(ref, ref->data.objectReference, templates)` at +0x607, which keeps the same array on the ref
   as ExtraLeveledCreature. The game saves that array and protocol 50 reports it.
5. `SetObjectReference` (vfunc 0xAA).

Because one array feeds both the build and the extra, overwriting it in `CopyFromTemplateForms` makes the NPC, its
save and its report all ours.

**The list level.** `CalculateCurrentFormListForRef` reads the list at
`GetCalcLevel(ref, !legendary) x the ref's GetLevCreaModifier` (floored). The modifier is applied only to a plain
roll. `GetCalcLevel` uses the ref's encounter zone:
- A zone whose level is locked uses its locked level (+0x44, u16).
- Otherwise it uses the zone's level for the player. Below the minimum the result is the minimum, or the player's
  level when the zone's flag bit 1 (match PC below minimum) is set. Above it the result is
  `min + (pl - min) x scale`, capped at the maximum (+0x33, 0 for none).
- With no zone, the worldspace grid's level or the player's level is used.

The engine adds a small variance that it cycles through.

**The hooks.**
- **Hook 1** is a `write_call<5>` on the resolver's +0xCB call, using one 14-byte trampoline stub. It calls the
  original. If the original returns false (the ref is not resolved yet) and the hooks are enabled, it decides
  (above) and arms a thread_local POD.
- **Hook 2** replaces TESNPC[1]'s slot 7, after checking that the slot holds ID 2207448's address. When a decision is
  armed for this thread, it overwrites `templates[0..12]` (nullptr outside the roll's mask) and calls the original.
  Then, for an NPC whose level is its player's times a multiplier (ACTOR_BASE_DATA `kPCLevelMult`), it sets the level
  from the roll's report or from the session's level within the NPC's calculated bounds. It always clears the armed
  decision.

**Crash safety.** Both hooks check their bytes or slot at install. If either doesn't match, they are not installed,
with a warning. The game's data is read under SEH (`ReadFacts`, `Pick`). A fault turns the hooks off for the rest of
the run and is logged once. A template that isn't an NPC in our game, a mask without the NPC's use flags, or anything
unexpected leaves the engine's own roll.

## Where we differ from the engine, on purpose or not yet

- **A zone's locked level** comes from each game's own first visit. The pick reads the zone's bounds, not its lock, so
  two games that locked a zone at different levels still pick the same.
- **The variance** the engine cycles through is each game's own count, so the pick leaves it out.
- **The legendary cooldown** between legendaries (a per-game counter) is not kept, so a session can see two
  legendaries close together.
- **The difficulty multiplier** is each game's own, since each player picks a difficulty. Two games on different
  difficulties can pick legendary differently. A game on Survival and one on Normal give the same raider only when
  the draw is below both chances. A host rule for this could come later.
- **Chance-none and use-all lists** are read as plain lists.
- **The level variance on the NPC's own level** for `kPCLevelMult` NPCs is not reproduced.

## Changes to note

- The ours-there stopgap (a friend's spawn that our own game made too) is folded in here and not shipped on its own:
  `Spawns::OwnMadeNpcAt`, `npcOursThere=`.
- The brief proposed carrying the roll in `NEW_REFR_DATA`. Arming a thread_local around the placement was used
  instead: it needs no knowledge of that struct's layout, and the resolver runs inside `CreateReferenceAtLocation`
  on the same thread.
- A session file with rolls or a level is layout 41. A file with neither is still written in the older layout, so
  0.10.1 can read it.
