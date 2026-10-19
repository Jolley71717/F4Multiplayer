# The empty hand in a stand-in's throw (bead 2r3)

A friend throws a grenade, mine or Molotov. Their stand-in plays the throw in your game, but its hand
is empty during the wind-up. This is cosmetic (S3). The explosion comes as its own report and goes
off where it did in the thrower's game. This note sets out what main already does, what the rig
showed across three rounds, why the third round was reverted, and round 4: the options weighed,
the one drafted (off by default) and the checks that decide it, then round 5, which turns it on. Each step below needs the game. A
Linux build can't tell which one works.

## What main does today

- **The throw.** The thrower's graph sends `WeaponFire(2)` at the release, and their game reports
  `ShotAction::kThrow`. On our side, `RemotePlayers::PlayAction` calls
  `Equipment::HoldThrowable(actor, gear.items)`, then `WeaponFire::PlayAction`. That has the stand-in
  perform `ActionThrow` (00004E32) and arms `ThrowReplays`, so the stand-in's own release is kept from
  the engine and no second grenade leaves its hand. Our copies of NPCs go the same way through
  `NpcSync`.
- **The form shown.** It is the first grenade or mine (Molotovs are grenades) in the thrower's last
  gear report. The player's inventory does flag an equipped throwable, so `Equipment::Read` sends it
  with their gear. The form is already on the wire, so this needs no protocol change.
- **Equipping it.** `HoldThrowable` gives the stand-in the item if it has none left. It then equips
  it through `ActorEquipManager::EquipObject` in the weapon's own equip slot (forced, applied now,
  silent). The console's `equipitem` leaves a throwable in an NPC's inventory but never puts it in
  the engine's equipped list (`middleHigh->equippedItems`), which is what the throw reads. This
  happens when gear arrives and again before every replayed throw (round 1, 83fd2e9).
- **The gear sync.** It leaves the throwable alone. `RemotePlayers::Throwable` keeps it out of the
  gear check, which an NPC's inventory never passes for a throwable (1e0e40b). `Apply`, `Wear`,
  `WearOnStandIn` and `WearInArmor` each call `HoldThrowable` after dressing. Their unequip loops
  read `Equipment::Read`, which never lists an NPC's throwable, so they never take it off.
  `AttackKind::HoldsGun` stops a grenade beside the gun from turning shots into swings.

So the brief's suspected cause is not what goes wrong. The thrown weapon is equipped on time, and
nothing unequips it. **The engine lists the grenade but never shows its model.**

## What the rig showed

| Round | Change | Result |
|---|---|---|
| 1 (83fd2e9) | Equip through the equip manager instead of the console | The engine lists the grenade at equip index 2. The throw's `weaponAttach(2)` event arrives (`attaches=` in `net status`). The hand is still empty. |
| 2 (25341ad) | Ask `TESObjectREFR::AttachWeapon(instance, index 2)` for the 3D | The biped's grenade slot (`3d[42]` in `equipped`) stays empty, and so does the hand. |
| 3 (1f59f34, reverted in 6ade87a) | The plugin loads the weapon's model (`BSModelDB::Demand`), clones it and hangs it under `LArm_Hand` until the release | Reverted three minutes after the merge: the game crashed in `BSGeometry::ProcessClone` (bead notes). |
| 4 (PR #81, 1d61b6f) | Read-only readouts of the preload and the hands. The database's own node, no clone, hung under `LArm_Hand` behind `handthrowable on` (off at every start) | The readout showed the engine *does* hang the grenade on the stand-in, in a `WeaponLeft` it keeps culled, so the hang never ran (`the engine shows its own`). Reworked to draw that node instead: the grenade shows. See "Round 4 on the rig" below. |
| 5 (this branch) | Round 4's drawing of the engine's own `WeaponLeft`, on from the start (ini `bGrenadeInHand`, true by default). The database's node hangs only with the dev's `handthrowable model` | See "Round 5: on by default" below. |

Round 3 also noted that the local player's biped grenade slot is empty, yet the player is shown the
grenade. So the engine hangs a throwable's model at attach time somewhere other than the biped slot.
Round 3's hand readout then named the place (maintainer's notes, 2026-10): **the engine hangs the
grenade as a child named `WeaponLeft` under `LArm_Hand`, and only during its own throws.** A friend's
stand-in, whose throw we start with `ActionThrow`, gets none, although the engine runs the same
`weaponAttach(2)` for it.

## Why round 3 was reverted

The bead notes record a crash in `BSGeometry::ProcessClone` with a demanded model cloned by a default
`NiCloningProcess`. The way round 3 cloned the model explains it:

```cpp
RE::NiCloningProcess cloning{};               // zero-filled, not the engine's constructor
const auto cloned = loaded->CreateClone(cloning);
if (cloned) {
	cloned->ProcessClone(cloning);            // called on the clone
}
```

In Gamebryo, `NiObject::Clone` calls `CreateClone` on the **source** and then `ProcessClone` on the
**source**. `ProcessClone` looks the object up in the clone map to find its copy and fixes up that
copy's controllers and extra data. Called on the clone, the lookup finds nothing, and the fix-up
dereferences what it didn't find. A zero-filled `NiCloningProcess` is also not what the engine's
constructor builds: that sets up the two hash maps, sets the append character to `'$'` and the scale
to 1. CommonLibF4 has no address for `NiObject::Clone` or for that constructor, and nothing in `src/`
cloned a model or attached a child node then. So there was no working call to copy, and no way to prove
one on Linux. If a clone is tried again, fix it first (option 3 below).

## Round 4: the options

The goal this round is the safest way to a grenade in the hand, so each option below is weighed
by what CommonLibF4 (1.11.240) gives for it, what could crash, and how it fails safe. Nothing here
guesses an offset: every call is a virtual on an object the engine made, or a function with a
`REL::ID` in CommonLibF4's `IDs.h`.

### 1. Let the engine do it: its own throw path

The engine already runs its throw path for the stand-in. `ActionThrow` plays the graph, the graph
sends `weaponAttach(2)`, and our NPC animation hook passes that event on to the engine (only the
release, `WeaponFire(2)`, is kept back). The engine's handler for it is, by its name,
`WeaponAttachHandler` (CommonLibF4 has its RTTI and vtable IDs only, no class). It hangs nothing on the
stand-in. So the path is not what is missing. What is missing is something it reads, which the engine sets up only
for an actor it dressed or whose throw it chose. The best candidate is the grenade entry's
`EquippedWeaponData` (`EquippedWeaponData.h`): its `weaponPreload` (`NiPointer<QueuedFile>` at
0x58) is the weapon model the engine loads ahead of time, which a throw could hang without waiting.
An entry with no data, or data with no model preload, would hang nothing, as seen.

What CommonLibF4 offers to make the engine fill that in:

- `AIProcess::SetEquippedItem(Actor*, const BGSObjectInstance&, const BGSEquipSlot*)` (2231627),
  `Actor::HandleItemEquip(bool a_cullBone)` (2229781), `AIProcess::RequestLoadAnimationsForWeaponChange`
  (2231758). Each has an address, but what each does to a stand-in's process is not known. Calling
  them in turn to see which fills the preload would be guessing at engine state.
- `Actor::Reset3D(bool, std::uint32_t, bool, std::uint32_t)` (2229913, used in `Face.cpp`) and
  `TESObjectREFR::UpdateReference3D()` (2201071, the dev command `refresh3d`): the engine rebuilds the
  actor's 3D and dresses it from its equipped list, the way it dresses its own NPCs. Known effects, but
  a rebuild per throw flickers and can take a stand-in out of its power armor frame.
- Equip the grenade before the stand-in's 3D is built (in `Apply`, which runs as it is placed), so
  the engine's first dressing sets it up. This calls no new function.

**Crash risk:** low for the 3D rebuild (already used), unknown for the three engine calls.
**Fails safe by:** doing nothing new until the measurement in check A below says the preload is what
is missing. **Verdict:** the best fix if check A shows no `m` in the stand-in's `pre=` and an `m`
in the player's. It can't be written before that without guessing, so round 4 only adds the readout that
decides it.

### 2. Call the engine's own attach function

- `TESObjectREFR::AttachWeapon(const BGSObjectInstanceT<TESObjectWEAP>&, BGSEquipIndex)` (virtual
  A5) was round 2. It loads into the biped's grenade slot, which even the player's throw leaves empty.
  It changed nothing. `Equipment::LoadThrowable3D` still calls it (rate-limited). It is harmless, so
  it stays.
- `Actor::DoReparentWeapon(const TESObjectWEAP*, BGSEquipIndex, bool a_weaponDrawn)` (virtual 118)
  moves a weapon's 3D between its holster and its hand node. It moves 3D that already exists, and the
  stand-in has none for the grenade.
- `Actor::AttachArrow(const BSTSmartPointer<BipedAnim>&, BGSEquipIndex)` (virtual 0ED) and the
  `ArrowAttachHandler`. These are Skyrim's arrow-in-hand, still in Fallout 4's tables. Whether a throw
  uses them is not known, and the bow slot it would read is empty on a stand-in.
- `AIProcess::SetWeaponBonesCulled(const Actor&, bool, WEAPON_CULL_TYPE)` (2232535) shows or hides
  weapon bones that already hold something.

**Verdict:** none of them makes a model the stand-in doesn't have. Nothing in CommonLibF4 is the
function the throw itself uses to make `WeaponLeft`.

### 3. A clone made properly

Round 3 crashed in `BSGeometry::ProcessClone` because it called `ProcessClone` on the clone, not on
the source (see above). The fix would be `loaded->CreateClone(cloning)` followed by
`loaded->ProcessClone(cloning)`. Both are virtuals on the loaded node (1A and 20 in `NiObject.h`).
The `NiCloningProcess` would be CommonLibF4's struct with its own empty `BSTHashMap`s,
`copyType = kCopyExact`, `appendChar = '$'` and `scale = {1, 1, 1}`. But Bethesda's `NiObject::Clone`
and the constructor of `NiCloningProcess` have no address in CommonLibF4. So whether the engine does
more around a clone than those two calls can't be checked. The cost of a mistake is the same crash
again, on the first throw.

The engine's own clone of a form's model is `TESBoundObject::Clone3D(TESObjectREFR* a_requester,
NiPointer<NiAVObject>&)` (virtual 5E; `TESObject::Clone3D` at 4E, `UnClone3D` at 4F). It is a virtual,
so it needs no address. But what a weapon's override does with an actor as the requester (its instance
data, its loaded-3D bookkeeping) is not known, and passing no requester may crash too.

**Verdict:** possible, but not the safest. It is the next step if option 4 shows the model in the
hand but the shared node turns out to be a problem (check D).

### 4. The database's own node, no clone (drafted, off by default)

`BSModelDB::Demand(const char*, NiPointer<NiNode>*, const DBTraits::ArgsType&)` (2275154) returns the
one node the engine's model database keeps for a file. The engine never puts that node in the world.
It clones it every time it shows the model. Round 4 hangs the node itself under the stand-in's
`LArm_Hand` with `NiNode::AttachChild(NiAVObject*, bool)` (virtual 3A) and takes it off with
`NiNode::DetachChild(NiAVObject*)` (virtual 3D). Both are called on engine-made nodes, so they go
through the engine's own vtable. Nothing is cloned, so the crash of round 3 can't happen. Nothing of
the node is changed but its parent: not its transform, not its flags, not its name. So a copy the
engine makes of it meanwhile (the player's own frag, a raider's) is the same as ever. This is what
the bead itself suggested.

What could still go wrong, and what guards it:

| Risk | Guard |
|---|---|
| A node has one parent. Two stand-ins throwing the same grenade at once would steal it from each other. | `HandThrowable::Book` lets one model hang on one stand-in at a time (a second waits). A model with any parent at all is never touched. |
| The stand-in's 3D goes (cell change, rebuild, delete) while the model hangs. Detaching from a freed hand would crash. | We hold `NiPointer`s to the model, the hand and the actor's 3D root while it hangs. The skeleton stays alive until we have taken the model off. The root and hand are compared by pointer each frame, and a held pointer is never reused. |
| The engine's own grenade shows too. | A `WeaponLeft` under the hand means the engine shows its own. Then ours is never hung, or comes off at once. |
| The release never comes. | Off after `MAX_HOLD` (3 s). |
| The gear changes mid-throw. | Off when the engine no longer lists that throwable. |
| A save, a load, the session's end, a stand-in or copy deleted. | `DropHandThrowables` runs next to `EndAllShields` (before a save or load, and on disconnect). `DropHandThrowable` runs next to `ReleaseSoundsOf` (stand-in and copy release). |
| The toggle goes off. | Every model comes off at once, and the cached database nodes are let go. |
| Rendering the database's own node (never done by the engine) misbehaves, or the engine adds its collision to the world when it reprocesses the actor's 3D. | Not guardable from here. It is why this is a dev toggle, off at every start, tried first on a stand-in standing still (check C). |
| The skeleton's update doesn't carry the new child along (it would stay where it was, unseen). | Logged: 400 ms after the hang, the model's and the hand's world positions. |
| Loading the model on the main thread hitches. | Once per grenade type (the node is kept), small files, and round 3 did the same. |

Everything happens on the main thread. The animation hook only queues the stand-in's
`weaponAttach(2)` and its release (`WeaponFire(2)` or `weaponDetach(2)`), and only while the toggle
is on. Only a throw `ThrowReplays` still waits on (a friend's, replayed here) gets a model: an NPC's
own throw never does.

The model sits at the hand bone's origin, the wrist, with its own transform. Placing it in the palm
needs a node of our own between hand and model (so the shared node's transform stays untouched).
That waits for the engine's own offset, which check B reads off the player.

### The pick

Option 4, behind `handthrowable on`, which is off at every start, is the safest thing that can
show a grenade now. It is drafted in `src/game/HandThrowable.h` (the decisions, unit-tested in
`tests/HandThrowableTests.cpp`) and in the "grenade in a stand-in's hand" section of
`src/game/WeaponFire.cpp`. With the toggle off, the only new things running are read-only logs:
the `pre=` preload readout, what hangs under the hands, and the player's own throw 400 ms in.
Those settle option 1. If check A finds the preload missing, option 1 is the real fix, and option 4
stays a dev tool or goes.

## Round 4 on the rig (2026-10-18)

Two games over Steam, a frag (000EEBED) equipped on both, ten throws each way (the host's through the
console's `player.PerformAction 4E32`, the VM's with its own key). No crash in either game.

- **Check A.** The stand-in's grenade entry reads `000EEBED@2(pre=m-)`, the same as the player's: the
  model preload is there. A missing preload is not the cause, so option 1 is off the table.
- **Check B, and the cause.** On the stand-in, before and 400 ms into a replayed throw, the hand reads
  `LArm_Hand['WeaponLeft'(culled)@(8.0,-2.6,-0.8)x1.00{'Weapon  (000EEBED)' }]`. The engine hangs the
  grenade on the stand-in as it does on its own throws, under `WeaponLeft`, but both `WeaponLeft` and
  the `Weapon (000EEBED)` node in it stay culled: on a throw the engine did not start itself it never
  un-culls them. After the throw the engine empties `WeaponLeft` itself (`{}`).
- **So option 4 as drafted did nothing.** A `WeaponLeft` under the hand counted as "the engine shows its
  own", and every throw logged `no throwable hung on ...: the engine shows its own`.
- **Option 5, the fix now behind the toggle.** At the replayed throw's `weaponAttach(2)`, a culled
  `WeaponLeft` holding something is un-culled, with any culled child in it. At the release (or any other
  way out in the table above) the same nodes are culled again. These are the stand-in's own nodes,
  made for it by the engine: nothing is hung, cloned or shared, and the database's node is not touched.
  A drawn `WeaponLeft` (one the engine shows itself) still keeps ours out. The database-node hang stays
  as the fallback for a hand with no `WeaponLeft`, which the rig never showed.
- **Seen.** `drew the engine's own WeaponLeft under LArm_Hand of <S> (throwable 000EEBED, holding
  'Weapon  (000EEBED)'(culled))`, then `<S>'s throwable at (-80526,89455,7911), its hand at
  (-80528,89453,7903)` and `took throwable off <S>: released` about 450 ms later. `inhand=` counted up
  `n/n` on both sides, `ghosts=0`, explosions one per throw each way. A VM screenshot at the top of the
  wind-up shows a dark frag in the stand-in's raised left fist. (A build that un-culled only
  `WeaponLeft` was caught on screen only after the release, so whether the child needs it too is not
  proven; it was culled, so it is un-culled.)
- **Still open.** The grenade shows only from `weaponAttach(2)` to the release, about half a second.
  Whether the engine's own throw shows it longer is not measured. Checks D (fast travel, save and load, power armor, a
  toggle-off mid-throw, two friends at once) were not run. Neither were a Molotov or a mine.

## Round 4: the checks

Use the dev channel (DEV-CHANNEL.md) on two games over Steam, with god mode on. `<S>` is the
friend's stand-in's form ID in your game. Everything here runs with the toggle off unless a step
turns it on.

**A. Is the preload what is missing?** (read only; decides option 1)

1. Hold a frag grenade yourself. `equipped` lists your grenade as `<id>@2(pre=mp)` or similar. `m` is
   the model preload and `p` the projectile's.
2. Let the friend equip a frag. `equipped <S>` shows the stand-in's entry. `(nodata)` means the
   engine never set it up as a weapon. `(pre=-p)` or `(pre=--)` means no model preload.
3. If yours has `m` and the stand-in's doesn't: run `refresh3d <S>`, then `equipped <S>` again. If `m`
   appears, have the friend throw and watch the hand. A grenade there means option 1 works (equip
   before the 3D is built, or rebuild once on gear change). Write that next and leave option 4 off.

**B. Where the engine hangs its own.** (read only; places ours)

Throw a frag yourself. The log line `player mid-throw holds... hands LArm_Hand[...]` lists
`'WeaponLeft'@(x,y,z)x<scale>{...}`: where it sits under the hand and what is under it. Note it in the
bead. A `WeaponLeft` under `LArm_Hand` outside a throw (`equipped` with no throw going) would mean it
is a skeleton bone, not the engine's grenade. Then the engine-shows check needs the name of what
hangs under it instead.

**C. Option 4 on a stand-in standing still.** (the toggle on)

1. `handthrowable on`. The friend holds a frag and stands still. They throw.
2. Watch the stand-in's left hand from the wind-up to the release. The log has
   `hung throwable ... under LArm_Hand of <S>`, then `<S>'s throwable at (...), its hand at (...)`.
   The two positions should be a few units apart: the model follows the hand. Then comes
   `took throwable off <S>: released`. `net status` shows `inhand=1/1`.
3. A grenade seen at the wrist or in the palm is a pass. If none is seen but the log says hung and
   the positions match, the node is not drawn. If the positions are far apart, the skeleton's update
   doesn't carry it. Note which.
4. Ten throws in a row. `inhand` counts up the same on both sides (`n/n`), with no crash.

**D. Option 4 in the ways it could go wrong.** (the toggle on, only after C passes)

- You hold frag grenades too and throw at the same moment as the friend. Your own grenade shows as
  usual, and nothing else changes (the engine copied the shared node while it hung).
- Two friends throw frags at once. The second's log says `the model hangs elsewhere`, and nothing
  goes wrong.
- The friend throws and you fast-travel or walk through a door in the wind-up. Log:
  `took throwable off ...: 3D gone or rebuilt` (or `stand-in deleted, save, load or session end`).
  No crash on the loading screen or after it.
- Save and reload with a throw in the air, then throw again. `took throwable off` comes before the
  save, and the next throw hangs again.
- The friend changes gear in the wind-up: `throwable no longer equipped`.
- The friend in power armor throws (the frame rebuilds the 3D: `3D gone or rebuilt` when it does).
- `handthrowable off` mid-throw: `turned off`, and the hand is empty at once.
- A grenade lying on the ground nearby, and a raider throwing its own, look the same as without the
  toggle (no offset, no missing model).

## What proves it

- The friend throws a frag, a Molotov and a mine, in an exterior and an interior. You see each one
  in the stand-in's hand from the wind-up to the release, from the front and from the side.
- `net status` on your side: `attaches=` goes up by one per throw, `ghosts=` doesn't move (no second
  grenade), and `explosions=` set off counts one per throw. On the thrower's side, `explosions=`
  reported goes up by one per throw. With option 4: `inhand=` counts up the same on both sides.
- After the throw the stand-in has its gun out again, and `equipped <S>` lists the same gear as
  before the throw. `hands:` in that readout shows nothing of ours under `LArm_Hand` once the throw
  is over.
- No crash over ten throws in a row, with a cell change and a save reload in between.
- Only then does option 4 leave the dev toggle: as an ini key, for one release candidate's soak.
  Round 5 ships it on by default, with the key as the way back to an empty fist (below).

## Round 5: on by default

Round 4's drawing of the engine's own culled `WeaponLeft` is the only path that ever ran on the rig.
The hand with no `WeaponLeft`, which the database-node hang was for, never came up. So round 5 ships
the first and keeps the second as a dev experiment.

- **The ini key.** `bGrenadeInHand`, next to `bFriendFaces` in `dist/F4Multiplayer.ini`, true by
  default. `main.cpp` sets the toggle from it at load. False gives the empty fist of 0.8.11 and
  earlier.
- **Only the stand-in's own nodes, by default.** A hand with no culled `WeaponLeft` holding something
  is left alone (`no WeaponLeft of the engine's to draw`, `Skip::kNoEngineNode`). Nothing is hung,
  cloned or shared, and the model database is never asked for anything.
- **The dev's commands.** `handthrowable on|off` turns the drawing on or off for the session.
  `handthrowable model` also allows round 4's database-node hang on a hand with no `WeaponLeft`.
  Going back from `model` to `on` takes every hung database node off at once, as `off` does.
- **No protocol change.** The form is already in the gear report, and the drawing is local.
