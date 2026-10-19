# A friend's Dogmeat squats instead of running: findings and what to check in the game

On the joiner, the host's Dogmeat is carried along at a run while his copy holds a squat or sit pose. This note
records what the code was checked for, why the cause cannot be pinned down without the game, and a short capture
that tells the possible causes apart, each with the fix it points to.

## What the code does, and what it rules out

The copy's graph is driven by `Puppets::DriveAnimation`. For a graph with `iSyncLocomotionRangeID` and
`fTrotFastClipMult` (the dog's), each frame sets the gait state first, then sends "moveStart" or "moveStop"
(MoveSync), then sets the gait's clip rate and sends its event ("ToRunForward" and the rest) when the gait changes,
when the dog sets off, or when a resync was taken (`CreatureGait::Choose` and `CreatureGait::Send`). The host
sends its own dog's gait and clip rate with each state (`NpcSync::SendOwnedStates`).

That logic agrees with itself and with the dog graph as `tests/CreatureGaitTests.cpp` models it: a running owner
gives the copy the run state, the run's rate, one event as it sets off and none after. Nothing in it can give a
squat. So the squat comes from one of these:

- something the model takes for granted about the real graph that is not so (which events it takes in which
  state, which variable holds the gait);
- an input that never goes through CreatureGait: the sit idle, the speed the host reports, the weapon-drawn and
  sneaking flags, or the aim point every mirrored NPC is now given (AimPoint::ForMirror).

These cannot be told apart from the code alone. The capture below takes about a minute.

## What the dog's graph itself says (read from the game's files, 2026-10-18)

DogmeatRoot.hkx and DogmeatDefault.hkx, read for their state machines, bindings and starting values, show two
things the code took for granted and the model in `tests/CreatureGaitTests.cpp` had wrong:

- **Nothing in the graph writes `iSyncLocomotionRangeID`.** The state machines that keep a sync variable name one
  (CombatNonCombatBehavior keeps `iSyncCombatState`, the head and walk-pose machines theirs); MovingStateMachine
  names none. Its start state is only read from the variable, as "moveStart" enters it, and the gait changes while
  moving by the "To...Forward" events alone. So the host's reading is whatever its engine last put there: the gait
  the dog set off in, and the gait he plays now only if the engine writes it again on every change. A dog following
  his player sets off at a walk and runs to catch up, so the host may well report a walk (`ownerGait=0`) while he
  runs, and his copy walks while carried along at five times the walk's pace: reading 4 below. The walking test of
  2026-10-14 could not show this, since a walk is also the gait a dog sets off in.
- **The root graph starts `fTrotFastClipMult` at 0** (the other rates start at 1). A copy whose graph is in the fast
  trot without having been given that rate holds the clip's first pose while it is carried along.
- The graph's `ComputePlaybackMultipliers` expression computes only the body's roll into a turn, despite its name:
  nothing in the graph computes a clip's rate from Speed.

What the copy does about it now (`CreatureGait::Fits`, `CreatureGait::Rates`): a gait whose clip would have to play
at more than twice its pace to carry the dog at the speed reported is not believed, and the copy goes by the speed
(the nearest pace) at the speed's rate; and every clip's rate is written each frame, the planned one at its rate and
the others at the speed over their pace, so no state the graph is in plays frozen. `mirror 0001D162` shows the gait
the copy was told as `played=` next to `ownerGait=`, and all four rates.

## The capture

Dogmeat is 0001D162. The host has him following, and runs for a few seconds, first with his weapon away, then
with it drawn. While the joiner sees the squat, run on the **joiner**:

```
mirror 0001D162
graph 0001D162 get bForceIdleStop iSyncLocomotionRangeID iSyncIdleLocomotion Speed Direction fWalkClipMult fTrotClipMult fTrotFastClipMult fRunClipMult bAnimationDriven
aim 0001D162
animlog on 0001D162
(two seconds later)
animlog off
event 0001D162 ToRunForward
```

and on the **host**, while he runs:

```
graph 0001D162 get iSyncLocomotionRangeID fRunClipMult fTrotFastClipMult Speed Direction
motion 0001D162
```

Then check the joiner's log for "Puppets: 0001D162".

## Reading it

Each finding below gives what the capture shows, then the fix it points to. They are listed most likely first.

1. **He is held in his sit while being carried.** The joiner shows `moving=false`, a `speed=` above 25,
   `bForceIdleStop=true`, and `idleStops` climbing while `taken` stays low (or the log says "still refuses to
   move after 6 idle stops"). The stops in `Puppets::ReleaseIdle` do not end this sit. Fix: find which event
   does, using `event 0001D162 <name>` while he sits (try "IdleStop", "turnStop", "IdleForceDefaultState" and
   "moveStop"). Add it to ReleaseIdle, and test it in `tests/IdleReleaseTests.cpp`.
2. **His sit took "moveStart".** The joiner shows `moving=true` but `bForceIdleStop=true`, and animlog has no
   gait events. MoveSync believes he moves, so ReleaseIdle never runs, and "ToRunForward" is refused (the `event`
   line says "rejected"). Fix: for a dog, count `bForceIdleStop` while moving as a refused start, so ReleaseIdle
   frees him.
3. **His moving graph takes "moveStart" again.** `resyncs=` climbs about twice a second while he runs, and the log
   repeats "graph was out of step ... sent moveStart again". Each resend restarts his take-off, and
   `CreatureGait::Send` (`a_resent`) restarts the gait's clip after it, so he keeps crouching to set off. Fix: for
   a dog, test a moving graph with something it refuses while moving, rather than "moveStart"; or skip the gait
   event on a resent "moveStart".
4. **The gait the host sends is not the one his dog plays.** `ownerGait=` on the joiner is 0 or 10 (walk, or
   walk backwards) while the host's own `iSyncLocomotionRangeID` reads 3 and his dog runs. Or the host's value
   stays stale after a back-up. Fixed as the graph's data above suggests: `CreatureGait::Choose` goes by the speed
   when the owner's gait's pace is far below it (a walk-back at 458 units/s cannot be right), tested in
   `tests/CreatureGaitTests.cpp`. With the fix the joiner shows `played=3` while `ownerGait=0`.
5. **The speed is in other units for a dog.** `speed=` on the joiner is under 25 while the host's dog runs, and
   the host's `motion` shows an `animationSpeed` near 1 rather than hundreds. The copy is never started, so it
   stands, then sits. Fix: in `Gait::Reported`, use the ground covered for a creature. (Less likely than it
   looks: the dog walking in the VM test reported an `animationSpeed` of 91 to 107, the walk's own pace, so the
   units are the same as a person's. It stays here only for the run, which was never staged.)
6. **Only with weapons drawn.** The squat comes only once the host draws, and the dog's alert stance takes over
   the locomotion: animlog shows "ToRunForward" refused or never sent. The combat locomotion has its own events,
   which CreatureGait does not know. Fix: read those events from the host's dog with animlog while it runs drawn,
   and add them as a second set of ranges.
7. **The weapon aim bends his body.** `aim` shows an `aimPoint` on the copy, with `ownerAim` well up or down. Run
   `aim 0001D162 clear` on the joiner: if he stands up straight for a moment, his whole body is turned toward that
   point. The host's dog sends one because its AI points its head along its path. Fix: in `Puppets::Aim`, give
   a mirrored NPC its owner's aim only when its graph has the weapon aim (`bAimEnabled` reads), and leave a dog
   alone.

If none of these fits, the animlog output and the two `graph get` lines are what to add to the issue.
