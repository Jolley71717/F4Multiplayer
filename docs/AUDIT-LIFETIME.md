# Lifetime and validity audit (2026-10-10)

Scope: every place the plugin holds an engine pointer across a frame, reads an engine field whose
layout or resolution the engine controls, uses a form ID that arrived from the network, touches
engine objects off the game thread, deletes a reference, or dereferences an optional engine field.
Prompted by the Red Rocket terminal crash (a linked-ref entry read as a pointer before the engine
resolved it; fixed in 6da1cb6, 0.7.3).

Four readers covered the code: three independent read-only reviews (actor lifetime files; world
sync, weapons, workshop and the rest of `src/game`; the protocol, session, server and transports)
plus a manual pass over the puppet registry and the fixes. Line numbers are as of commit 0087695.

## Rules applied in the fixes

1. A raw engine pointer is never kept across a frame. Keep the handle or the form ID and resolve it at
   use; where a pointer is still stored for comparison, compare it, never follow it.
2. A registry keyed by address checks the form ID on every lookup; unregistering takes the ID and
   never reads the actor.
3. Every form ID from the network is looked up typed (`GetFormByID<T>`, which checks the form type)
   or followed by an `Is()`/`As<>()` check before a console verb that assumes a type.
4. Nothing is disabled or deleted while the engine walks its lists, and nothing is deleted before its
   3D is in (DeferredDelete).
5. Event sinks and hooks copy handles and IDs under the inbox lock; the reads happen in `Frame()`.
6. Memory the engine frees comes from the engine's allocator.
7. Coordinates from the network are finite and inside the world.

## Findings

Status: F = fixed (commit), O = open issue (beads id), N = checked, no change needed.

### Dangling pointers and address reuse

| Where | What | Status |
|---|---|---|
| Puppets.cpp:42, lookups at :100, :154, :349, :363, :372, :380, Unregister :332 | registry keyed by actor address; several lookups and the erase had no form-ID check | F 46dd515 (`Find` checks the ID; `Unregister(actor, id)`) |
| NpcSync.cpp:723 | equipment applied through the stored mirror pointer after its cell may have unloaded | F 46dd515 (re-resolved by ID) |
| NpcSync.cpp:167, RemotePlayers.cpp:387 | unregister by address of an actor already gone | F 46dd515 |
| RemotePlayers.cpp:43, :438-441 | face base (runtime NPC form) kept as a pointer across loads | F 46dd515 (by ID, remade when gone, reset on load) |
| RemotePlayers.cpp:657-668 | graveyard handles drained after a load | F 46dd515 (cleared on load) |
| NpcSync.cpp:127 | mirrors could target the player, a stand-in or a runtime actor | F 46dd515 |
| WeaponFire.cpp:251 | sound handle follows a stand-in's 3D root that may be deleted mid-tail | O 76z |
| WorldSync.cpp:537 | `LinkedTarget` returns the engine's pointer branch unverified | N (engine-owned entry; the ID branch handles the transient case) |
| RemotePlayers.cpp:265, :348, :453; WorkshopSync.cpp:163 | raw pointer taken from a temporary smart pointer, used in the same frame | N (same frame, cell holds the reference) |

### Deletes

| Where | What | Status |
|---|---|---|
| WeaponFire.cpp:511-519 | ghost projectile disabled and deleted inside the reference walk, before its 3D loads | F 4902dce |
| DeferredDelete.h:14 | 3 s fallback can delete during a long load stall | O clf |
| RemotePlayers.cpp:601 | young spawns go to the graveyard before a save instead of being deleted | O 6ut |
| RemotePlayers.cpp:336, :385 | frame copy deleted while the stand-in is inside it | O afx |
| WorldSync.cpp:595 | picked-up item disabled without a 3D check (disable only) | N |
| WorkshopSync.cpp:249-282 | copies deleted through DeferredDelete or the graveyard | N |

### Type confusion and engine layout

| Where | What | Status |
|---|---|---|
| WorldSync.cpp:530-537 | linked-ref entries hold form IDs until resolved | F 6da1cb6 |
| WorldSync.cpp:477, :519 | open/close state read and set on actors | F 4902dce |
| WorldSync.cpp:143 | ragdoll push source and target reversed | F 4902dce |
| WorldSync.cpp:331 | loose mods accepted as inventory objects | F 4902dce |
| Face.cpp:189, :202 | engine-owned structures allocated with CRT new | F 4902dce |
| Face.cpp:47-56 | tint entries built by writing a vtable into calloc'd memory | O (noted under 6hi; needs engine constructors) |
| MapShare.cpp:16-52, DevCommands.cpp | map-marker layout assumed by cast in two places | O 6hi |
| WeaponFire.cpp:111, :149, :398 | vtable offsets assumed for the animation hooks | N (version-pinned build) |
| Equipment.cpp:45, :70; RemotePlayers.cpp:104; WorldSync.cpp:121, :396, :804; Party.cpp:109 | untyped lookups | N (each followed by a type check) |
| RemotePlayers.cpp:331 | any furniture accepted as a power armor frame | O (trust epic, 4co family) |

### Races

| Where | What | Status |
|---|---|---|
| WorkshopSync.cpp:35-42 | sinks walked `copies` without a lock | F 4902dce |
| WeaponFire.cpp:450, :456 | `watchUntil` written from hooks, read in Frame | F 4902dce (atomic) |
| WorldSync.cpp:153-312 | death, hit, container, activate sinks | N (handles and IDs under `inboxLock`; `IsPuppet` under its own lock) |
| Puppets.cpp:161-221 | update hook | N (runs in the actor update, guarded by the registry lock) |
| Session.cpp, Server.cpp, DevChannel.cpp, transports | receive thread, server thread, listener | N (no engine objects touched off the game thread) |
| Steam transport at DLL unload | Steam API called after it may be gone | O 7v3 |

### Null dereferences

| Where | What | Status |
|---|---|---|
| RemotePlayers.cpp:264, :347, :452; WorkshopSync.cpp:232; WeaponFire.cpp:548 | `TESDataHandler` unchecked | F 4902dce (`EngineRefs`) |
| QuestSync.cpp:51 | `TESDataHandler` unchecked | F 4902dce |
| WorldSync.cpp:513 | `GetLock()` re-fetched | N (state only reported when a lock exists) |
| NpcSync.cpp:110, :202; Party.cpp:614; Compass.cpp:211; Session.cpp:90 | guarded by callers | N |

### Network trust (not crashes; own epic lkt)

Explosions at any position regardless of friendly fire; activations without proximity; deaths of
any NPC; container floods; 1000-damage hits attributed to any NPC; face spam leaking forms;
workshop placement of any base anywhere; unverified actor claims; a remote hello resetting the
host's session; world-state resend rate; linear quest search; forced quest stages; lock changes
without proximity; NPC equipment growth; unsanitised HUD text. Fixed already from this list:
unbounded coordinates (261d792), health backlog (4902dce), second welcome (4902dce), swing damage
(4902dce).

## Confirmation sweep

`sweep.sh` (stage scripts): unit tests, a clean build, deploy to both games, then over Steam in both
directions: movement trace, Corvega combat with NPC tracing, terminals, loot and the safe, grenades,
power armor in motion, a three-minute cell churn with save reloads, a reconnect, and three fuzz seeds
on the host with the crash logger attached. Pass means no "DOWN", no new crash log on either machine,
and lag, error and counter results no worse than the baseline (100 to 150 ms, under 5 units; NPC height
error under 40 units; ghosts and states counters moving as before).
