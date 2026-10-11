# Bug plan and validation plan (October 2026)

How reported bugs are ranked and worked, and how every change is proven in two real games over
Steam before it ships. Work items live in beads (`bd list` in this repository); confirmed bugs also get a
GitHub issue once reproduced or fixed. Issues never name a machine, a user account or a path under a
user profile.

## Severity

| Level | Means | Response |
|---|---|---|
| S0 crash or data loss | A game exits, hangs, or a save is damaged | Reproduce first, fix before anything else, release to GitHub the same day |
| S1 breaks co-op | A shared thing does not reach the other game (door, kill, loot, button) or the stand-in is wrong in a way that misleads play | Fix in the next release; verify both directions |
| S2 wrong but playable | Visible glitch with no effect on what each player can do (sound, animation detail) | Batch into a release |
| S3 cosmetic or request | Looks off only when you stare; feature asks | Backlog |

## Open bugs by severity (beads id)

| Sev | Issue | State | Plan |
|---|---|---|---|
| S0 | Crash when using terminals, including the Red Rocket terminal (vua, GitHub #8) | confirmed 2026-10-10: both games crashed on the Red Rocket terminal; fixed in 6da1cb6 (linked references read as pointers before the engine resolved them) | Repro at Red Rocket with both players: every container opened and emptied by each player, the terminal used by each, with the other player near and far. Then the same in an interior with scripted containers (Vault 81, Corvega). Watch both game processes and the last log lines. Suspects: the container-change relay on a scripted inventory, the linked-ref watch on terminals (0.7.0), the activation report (0.7.2). |
| S1 | Friend's power armor displayed incorrectly (hms, GitHub #9) | fixed 484b107 (0.7.4): the worn frame follows its stand-in and is hidden while worn; verified both directions in motion | Reporter's screenshot requested. Meanwhile test the cases the rig has not: a player already in power armor when the friend joins; entering a frame that stands in a cell the friend has not loaded; a frame type with paint or material mods; running, jumping and fighting in the frame for a minute while the friend watches from several angles. |
| S1 | Flying NPCs (54x, GitHub #18) | confirmed: the 0.7.0 ragdoll push on a shared kill launched the killer's stand-in instead of the corpse (276 units on the pre-fix build, 10 after); fixed in 4902dce, 0.7.5 | Fight in Corvega and on the Sanctuary bridge stairs with both players. Extend the trace harness to sample mirrored NPCs against the owner's copy (z error over time, not one frame). Suspects: ragdoll push, the bring-over SetPosition, a mirror left in the air after its owner's copy died. |
| S3 | Gunfire sound bug (8nh) | not reproduced on 0.7.3: 46 shots in three bursts all replayed and sounded, no loop left active | Both players fire automatic and semi-automatic weapons near and far; the loop counter must return to zero after every burst; listen on both sides in a recording. |
| S3 | Corpse of an NPC killed in another game lies in a different spot (h7z) | seen in the Corvega trace, 271 units apart | Backlog; carry the death position with the dead list. |
| S3 | Empty hand during the stand-in's throw wind-up (2r3) | confirmed on video | Backlog; the engine attaches the grenade mesh only through its own throw behaviour. |

Fixed and verified this month, both directions, two games over Steam: buttons and switches
(GitHub #5), ghost grenades (GitHub #6), terminal-linked locks lost after a minute (GitHub #7). Lifetime audit
(docs/AUDIT-LIFETIME.md): ten code-confirmed lifetime and validity bugs fixed (GitHub #10 to #19); the
trust-hardening backlog is beads epic lkt. Trust items fixed so far: deaths only from the
NPC's runner, hit caps, face throttle, explosions only after a real throw and near the thrower, owner and
companion caps, activation and lock changes only from a player near the thing, NPC gear given once,
settlement pieces wait for their workbench, HUD text sanitised (0.7.6 to 0.8.0).

Feature asks from the thread, in order: in-game menu (620), alternate-start compatibility check
(0df), companions following both players (90g, needs a real recruit in a session).

## Validation: what counts as verified

A single screenshot of a standing stand-in proves nothing: the copy can look right while standing
still and be wrong the moment anyone moves, fights or leaves the cell. A change is verified only when
all of these hold, in two real games connected over Steam (host PC and the second machine), with the
counters and files below as evidence:

1. **Both directions.** Host acts, joined player observes; joined player acts, host observes. Every
   feature, every time.
2. **Location.** The same check in an exterior cell, an interior cell, and across a cell change
   (one player walks through a load door while the other watches, then both meet again). Shared-world
   features are never tested inside Vault 111: it counts as the opening and nothing is sent from there.
3. **Behaviour over time.** The feature is exercised while moving: running, turning, sneaking, jumping,
   in and out of power armor, mid-fight. The movement harness (`stage/trace.sh run`) records both
   players and every stand-in at 20 Hz and reports lag and position error per direction; its baseline
   is 100 to 150 ms lag and a mean error under 5 units. A visual bug that "goes away when standing
   still" shows up in the trace as error that grows with speed or after an event.
4. **Function, not looks.** The thing that should happen is read back from the other game: the door's
   open state and lock flag (`refinfo` shows `locked=`), the safe's lock, the NPC's health and death,
   the projectile list (`refs 51`), the counters in `net status` (`acts=`, `states=`, `ghosts=`,
   `explosions=`). Screenshots and recordings illustrate; counters and state reads decide.
5. **Nothing echoes.** After a replayed action the actor's own counters must not rise (a replayed
   press, kill or throw must not be reported back).
6. **Recovery.** One player reloads a save or reconnects mid-test; the world must match again
   (dead stay dead, doors keep their state, no duplicate loot).

### Standing test set (run before every release, both directions)

| Area | Where | What is read back |
|---|---|---|
| Movement and stance | Sanctuary street, Vault 81 corridors | trace.sh lag and error, sneaking and jumping visible on video |
| Combat and kills | Lexington feral ghouls (native actors; console-spawned refs are never shared) | health fractions, `dead=`, ragdoll push distance |
| Buttons and terminals | Vault 81 elevator call buttons; Super-Duper Mart storage terminal and safe | door open state, safe `locked=`, `acts=`, `states=` |
| Loot and containers | Red Rocket containers, a Vault 81 room | `containerNext=`, `pendingLoot=`, item counts on both sides |
| Grenades | Sanctuary | `explosions=` reported/set off, `refs 51` shows no stray projectile, `ghosts=` |
| Power armor | Sanctuary with T-45 pieces | the frame base and pieces listed by `equipped <stand-in>`, in and out, moving |
| Cell change | Red Rocket interior door | the stand-in disappears and reappears at the right place |
| Reconnect | any | counters match after `net status` on both sides |

### Evidence kept

Trace files and plots in `stage/trace/`, recordings of each side in `stage/`, and the counters pasted
into the beads issue when it is closed. GitHub issues get the commit and a one-line summary of the
verification, never paths or machine names.

## Releases

GitHub gets a release for every fix set. Nexus gets a batch only when the user says so, with the
`-Nexus` package (no server executable) and a support email asking for review; the page says that
newer builds are on GitHub.
