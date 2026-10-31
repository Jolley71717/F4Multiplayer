# NPC health and damage: why "heal on hit" keeps coming back, and the design that ends it

Written 2026-10-30 against 0.10.0, after the fifth report of the same bug (tracker s5ax; before it cqx, n2gk, wood,
t8mf; PRs #150, #187, #192; the 0.9.2 study in `tools/apiswarm/reports/investigate-damage-092.md`). Short on purpose.
The code paths it names are in `docs/DESIGN.md` ("Shared NPC AI", the "Health" and "Deaths" rows, "Deaths that end").

## The bug, as players see it

Two players shoot the same enemy. For one of them the hits don't stick: the health bar drops and jumps back. For the
other they do. Sometimes neither can finish it. It comes and goes with the enemy, not with the weapon. Deleting the
server's session file helps for a while.

## How health works today, in one paragraph

Every NPC is **run** by one game (the runner). The runner's game is the truth for its health. Everyone else has a
**copy**. When you hit a copy, your game books the drop as yours, sends it to the server as "damage: this much of its
health", and keeps the drop on your copy meanwhile. The runner takes the damage off its NPC and, whenever its NPC's
health changes, sends everyone "health: it stands at this much". Your copy is set to that. Anything that lowers or
raises your copy with no hit of yours behind it is undone locally within a tenth of a second (CopyWatch), because the
runner deals those things itself.

## The root pattern

The runner's report is **absolute** ("it stands at 70%"), and your hit is a **delta** that travels through a long pipe
to get into that report. The pipe has, today, about fifteen places that can drop the delta, and none of them tells your
game. Your game can't tell "my hit is still on its way" from "my hit was thrown away", so it guesses with a timer: a
hit older than one second is assumed to have arrived, and the next absolute report is applied in full. When the hit
was in fact dropped, that report is the "heal". Every release so far found one more place that dropped the delta and
patched that place (a stale report at the server, a copy's own heal, a dead ledger out of step, damage the runner
couldn't apply yet). The next release finds another, because the pipe is open-loop: nothing is ever acknowledged.

The second half of the pattern: **the same NPC has three different "is it alive, and whose is it" states** (the
server's dead list in the session file, each game's own dead ledger, each game's owner table), with three lifetimes
(forever; per run; per connection). They drift apart on cell resets, reloads of older saves, crashes and handovers,
and damage is dropped wherever two of them disagree.

## The paths, each with the rule it assumes and how play breaks it

Verdicts: **closed** = the new design makes the drop impossible or visible; **narrowed** = still a heuristic, but a
wrong guess is now a local, immediate bounce rather than a later heal; **open** = not addressed here, follow-up.

| # | Path | Rule it assumes | How real play breaks it | Verdict |
|---|---|---|---|---|
| 1 | Runner change (takeover by a hit, handoff to a nearer player, release on unload, the 0.10.0 menu-pause handover) | Everyone's owner table matches the server's | Tables lag by a round trip; the old runner drops damage addressed to it ("not ours"), the hitter's table may still name it; with two games disagreeing, both drop and both give the drop back: "neither can kill it" | closed: the server routes damage by its own table, to the runner only, and re-routes a runner's "not mine" once |
| 2 | HealthShare (runner's periodic report) and the ack-less report after the runner's own hit | A report older than 1 s after your hit includes your hit | Any dropped or slow delta: the report heals it. The 1 s is a guess | closed: no timer; a copy shows "runner's word minus my hits not yet acknowledged", for as long as that takes |
| 3 | The server's stale-report rule (#150) | Only the runner's word counts | Right, but it also drops the old runner's *acknowledgement* when it rides on a report | closed: acks are their own message, always forwarded |
| 4 | DamageHold (#192) | Held damage gets applied within 15 s | After 15 s it is dropped with a log line only; a runner whose NPC never loads (far away, kept by a hold) drops it | closed: expiry is a refusal; the hitter's copy gets the health back at once, logged |
| 5 | Runner drops: dead here, dead in its ledger, no such actor, no health | The hitter's copy and the runner's NPC are the same creature | They aren't after a cell reset in one game, a reload of an older save, or a crash mid-fight | closed for the heal (refused, given back, logged); the identity question itself is path 9 |
| 6 | The server's event budget (500/s per player) | A player can't send more than that | A minigun on several copies plus loot and doors can | closed: an over-budget damage is refused, not dropped |
| 7 | Dead ledger / SessionDeaths (each game) | A death learned from the session is final until the host's game offers the NPC alive | The host's game forgets a death on load only if it *saw the corpse in this run*; in a new run it kills the respawned NPC instead (F2a in the study). A joiner is reminded and kills its copy; the host is never told its damage was dropped | narrowed: the host's game is now told (refusal "dead for the session"); revival rule is path 9, open |
| 8 | The server's dead list in the session file | What the file has dead is dead in the host's world | The host's cells reset (days pass), the host reloads an older save, the host starts a new character on the same file (hash check only) | open, but ruled out for s5ax on the rig (fix/s5ax: a reload with the same file, an older save and a console resurrect never healed anything): see "What should persist" |
| 9 | Identity across respawns | A form id names one creature | A respawned NPC has the same id as the corpse the session remembers; the session can't tell them apart | open: game-time stamp on deaths (below) |
| 10 | CopyWatch attribution: a drop with no hit event of ours is given back (and OwnersToDeal: explosion, throwable, spell-alone, "threw lately") | Every hit of ours comes with a hit event, and the flags tell a gun from a throw | **Reproduced on the rig for s5ax (fix/s5ax, PR #232):** the host's shot on a client-run copy fired no hit event at all (copyHits=0), so the drop was "no hit of ours" and given back; seen once, after a long stall behind the "Creations are loaded" box and a load while hosting; a 136-mod load order with another plugin eating hit events is the other suspect. Also: unverified flags on this runtime (the 0.9.0 regression), a mod's weapon dealing damage through a spell | narrowed: PR #232 widens attribution (a drop within 1 s of our shot at the aimed enemy, no hit event, nothing else hit it in 10 s, is ours); with this PR the attributed hit is then sent and answered; a drop still unattributed is given back within 100 ms, visibly and logged, never healed later |
| 11 | Ghosts / BlastShield / GhostLedger | A copy is a ghost only while a friend's explosion lands | A stuck ghost (fixed in #194); while it lasts, the ghosted player's shots simply pass through | narrowed: the shot is never booked, so nothing is in flight; the symptom is "no damage", never "heal" |
| 12 | Legendary mutation / DoT / restores (#187, #194) | A copy's health only moves by reports and our hits | The copy's own legendary heal; a bleed's ticks with no hit event | unchanged: a gain of its own is taken back (#187); an effect of ours is ours for 10 s (#194). Both local |
| 13 | Load and cell reset | After a load the ledger is rebuilt from the world state before anything is offered | The world request can be refused (too many loads): the game carries on after 15 s with an empty ledger and the server keeps the host's offers unanswered ("worldStale") | open: part of path 8/9 |
| 14 | A friend's spawn (runtime NPC) | The copy's spawn id mapping outlives the fight | The mapping is reset on load; a copy whose id is gone is nobody's: neither booked nor given back | open, separate: a copy with no wire id should be logged and released |
| 15 | Hits on the player by a copy (PlayerHits, #192) | The attacker is here to see | Fixed in #192 by taking hits judged by the reporter's distance | unchanged |

## The design

Four rules. The first three are implemented in this PR; the fourth is written down with a plan.

### 1. One truth, and a copy is "the runner's word minus my hits in flight"

The runner's game is the only source of an NPC's health. Every report sets the copy to the runner's word minus every
hit of ours not yet answered, whichever way that moves it, however long the hits take. A copy never shows a heal for
a hit that is still on its way; a rise above where it stands is a real heal in the runner's game (a stimpak, a
mutation there). What our hits take off a copy goes out at most ten times a second per copy, so a runner's 15 s hold
puts at most 150 hits in flight.

### 2. Every hit is a numbered request that ends in exactly one of two answers

Each damage report carries a number (per sender, counting up). It ends either:

- **applied**: the runner took it off and says so (a `DamageAnswer` of "applied", sent the moment it is applied,
  before the report that includes it), or
- **refused**: by the server or the runner, with a reason (a `DamageAnswer` naming it: the session has it dead,
  nobody runs it, you run it yourself, it is dead or missing in the runner's game, the runner no longer runs it, held
  too long, over budget).

Nothing in between. On a refusal the hitter's copy gets the health back **at once** and the game logs why: the player
sees the bounce immediately instead of a heal seconds later, and the log names the reason (a copy without its 3D that
frame is owed it, and gets it as soon as it is seen). "You run it yourself" is the one refusal that keeps the drop:
the hit's claim took the NPC over, and the copy's health is now the truth.

A hit whose answer never comes (a server restart, an answer lost) is given back after two minutes with a "lost" line:
longer than any hold the runner's game can keep it in, and the server answers for a game that leaves. That is the
only timer left, and it errs on the side of showing the player the truth.

### 3. The server routes damage, by its own table, to the runner alone

The hitter's table no longer decides who applies the hit. The server sends damage to the runner it has on record;
anyone else learns through the runner's report. A runner that answers "not mine" (its table is behind) has the damage
re-routed by the server to whoever runs it now, once; if nobody does, the hitter is told.

### 4. What should persist, and which world wins (follow-up)

Today the session file keeps the dead list forever and the host's game trusts it over its own world in a new run: a
camp that respawned in the host's world is killed again as it loads. Deleting the file "helps" because it clears that
list. The joiner is told and kills its copy; the host is told nothing (fixed above: it is now refused and logged).

Proposed rule: **deaths persist, stamped with the host's game time**. The host's game already reports game days. A
death whose stamp is older than the game's own respawn interval (`iHoursToRespawnCell`, 7 days; 20 for a cleared cell)
is stale the moment the host's game loads that NPC alive: the host's game revives it for everyone (the existing
"alive again in the host's world" path), whatever its ledger's `seenDead` says. A death younger than that, with the
NPC alive in the host's loaded world, means the host loaded an older save: the kill is replayed as today. That is one
precise rule in place of three guesses (`seenDead`, the 10 s `RESPAWN_AFTER`, `worldStale` plus a 10 s grace), and it
needs a game-time field on `ActorDeath` and in the session file (a layout bump). The joiner side stays as it is
(reminded, kills its copy; forgets the death when anyone runs the NPC).

Nothing else about NPCs should persist: not ownership (it doesn't), not health (the runner's save holds it), not
spawn deaths (they don't).

### Should a copy give health back at all? (the s5ax finding, PR #232)

The one heal reproduced on the rig for s5ax was not a dropped delta but a hit that never had a hit event: CopyWatch
saw the copy drop with "no hit of ours" and gave it back. That is the attribution rule, not the pipe. Three options:

- **Never give back** (send every unexplained drop as our damage): wrong. The runner's game deals a copy's hazard
  damage itself (the fire a Molotov leaves, radiation), and a friend's explosion is replayed here; sent as ours too,
  the NPC takes those twice. The give-back exists for exactly those drops.
- **Make a copy immune to everything but our hits** so nothing needs giving back: not possible with the engine's
  ghost flag (a ghost passes our hits by too), which is why BlastShield ghosts only for the moment of a blast.
- **Keep the give-back, attribute better, and make what is given back loud.** This is the design: PR #232's rule
  (a drop within a second of our shot at the aimed enemy, with no hit event and nothing else hitting it, is ours)
  widens attribution to cover the missing-event case; this PR then guarantees an attributed hit lands or bounces
  with a reason; and a drop still given back is counted (`copyRestores=`) and logged with the last hit seen, so a
  recurrence names itself.

So **#232 is folded in, not superseded**: merge it first (small, reviewed, tested on the rig), then this PR on top
(they touch different lines of `CopyWatch`; the only overlap is the counters string). #232's aim check is coarse
(third person, VATS); with the loop closed, a wrong attribution there costs one refused hit and a log line, not a
heal.

### Trade-offs

- **A bounce instead of a heal.** A refused hit now visibly bounces back within a frame. That is the honest signal;
  it is also more noticeable than a heal a second later. The log line beside it says why, which is what every report so
  far lacked.
- **Protocol 56.** Damage and health messages gain fields and two message types are new; 0.10.0 games can't join a
  56 host. Nothing else changes on the wire.
- **A little more traffic.** One small ack per hit on a copy; damage no longer goes to every player, only to the
  runner, which saves more than the ack costs with three or more players.
- **Attribution heuristics stay.** Whether a drop on a copy is "our hit" is still judged from the hit event (path 10).
  A wrong judgement now costs a local bounce, never a delayed heal, and never reaches the runner.
- **The dead list is still the weak point** (paths 8, 9, 13). This PR makes every drop it causes visible and
  immediate; the stamp rule above is the fix, and it is a wire and file change of its own.

## Implemented here, and what is not verified in a game

Implemented (protocol 56): numbered damage and its answers (`DamageAnswer`), server routing to the runner
only with refusals for every drop and one re-route, runner acks and refusals (including DamageHold expiry), and the
copy's health as "the runner's word minus unacknowledged hits" with the 1 s write-off removed. Tests: protocol
round trips; the server's routing, refusals, re-route and ack forwarding; the copy book under in-flight, acked, refused
and lost hits; and two-game scenarios through the real server that reproduce the broken paths (a hit in flight for
longer than a second, damage the runner holds and then drops, a runner whose table is behind, a dead NPC).

Not tested in a game: the whole of it. The rig was in use by the s5ax fix. What a game test should look for is in the
PR.
