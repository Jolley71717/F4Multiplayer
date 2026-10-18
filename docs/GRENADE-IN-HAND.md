# The empty hand in a stand-in's throw (bead 2r3)

A friend throws a grenade, mine or Molotov. Their stand-in plays the throw in your game, but its hand
is empty during the wind-up. This is cosmetic (S3). The explosion comes as its own report and goes
off where it did in the thrower's game. This note sets out what main already does, what the rig
showed across three rounds, why the third round was reverted, and what to try next. Each
step below needs the game. A Linux build can't tell which one works.

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

Round 3 also noted that the local player's biped grenade slot is empty, yet the player is shown the
grenade. So the engine hangs a throwable's model at attach time somewhere other than the biped slot.

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
clones a model or attaches a child node. So there was no working call to copy, and no way to prove
one on Linux. If the hand-hung model is tried again, fix the clone first (see step 3).

## What to try next, in order

Each step uses the dev channel (see DEV-CHANNEL.md) on two games over Steam, with god mode on.
`<S>` is the friend's stand-in's form ID in your game.

1. **Get the next measurement.** Re-add round 3's read-only diagnostics on their own, without the
   model hanging: from 1f59f34, the `pre=` preload readout in `Equipment::DescribeHeld` (the
   `weaponPreload` and `projectilePreload` of each equipped weapon), `Equipment::DescribeHands`, and
   the player's own mid-throw readout 400 ms after their `weaponAttach(2)`. Then compare:
   - `equipped` (the player) while holding a frag grenade, against `equipped <S>`. If the stand-in's
     grenade entry reads `(nodata)` or `pre=--` and the player's has a model preload, the engine
     never set up its weapon data. Whatever it hangs at `weaponAttach(2)` comes from that preload,
     so it has nothing to show.
   - The `player mid-throw ... hands[...]` log line from one of your own throws. It names the node
     the engine hangs a grenade under, and its name, on an actor it does show one for.
2. **Check that the engine shows it only on an NPC it dressed itself.** Find a raider near you
   (`enemies`) and give it a grenade the way a stand-in gets one: `console <raider>.additem <id> 1`
   (<id> is a frag grenade's form ID, read off `equipped` while you hold one), then let gear arrive or call `HoldThrowable`. Then `throw <raider>`, and watch
   the raider's hand. Do the same with a raider that rolled its own grenade (`equipped <raider>`
   lists one). Only the second showing it would confirm the hypothesis.
3. **Then the fix, depending on what steps 1 and 2 show:**
   - *The stand-in's entry has no data or preload, and an engine-dressed NPC shows the grenade.* Have
     the engine dress the stand-in: equip the throwable before its 3D is built (in `Apply`, which runs
     as it is placed) so the engine's own dressing sets the grenade up, rather than adding it after
     the 3D is loaded. As a check before writing code: `refresh3d <S>` (the engine's
     `UpdateReference3D`) after the grenade is held, `equipped <S>` again for `pre=`, and then have
     the friend throw. Don't rebuild a stand-in's 3D on every throw: it flickers, and it can take
     the stand-in out of its power armor frame.
   - *No NPC ever gets a preload this way.* Bring back round 3's hand-hung model with the clone done
     as the engine does it: `CreateClone` and then `ProcessClone`, both on the loaded source, with a
     clone process set up as the engine's constructor sets it. Hang it under the node step 1 named
     for the player, not a guessed `LArm_Hand` 6 units from the wrist. Take it off at the stand-in's
     `WeaponFire(2)` or `weaponDetach(2)` (the NPC animation hook already sees both), when the ghost
     watch ends, and before the stand-in is deleted or its 3D rebuilt. Test it first with
     `throw <S>` on a stand-in standing still, then in a fight, then in power armor.
     The bead's own suggestion is simpler: no clone at all. Hang the demanded node itself under the hand
     at the throw and take it off at the release (one stand-in at a time, as a node has one parent).

## What proves it

- The friend throws a frag, a Molotov and a mine, in an exterior and an interior. You see each one
  in the stand-in's hand from the wind-up to the release, from the front and from the side.
- `net status` on your side: `attaches=` goes up by one per throw, `ghosts=` doesn't move (no second
  grenade), and `explosions=` set off counts one per throw. On the thrower's side, `explosions=`
  reported goes up by one per throw.
- After the throw the stand-in has its gun out again, and `equipped <S>` lists the same gear as
  before the throw. With the hand-hung model, also check that the hand holds nothing once the throw
  is over.
- No crash over ten throws in a row, with a cell change and a save reload in between.
