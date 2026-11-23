#!/bin/bash
# infinite-health.sh: an NPC the host's game runs is given an infinite health (a script or console that makes an NPC immortal
# this way), and the VM's player sees and shoots it. 0.21.1 took the NPC's health share of an infinite maximum as 0 (health /
# infinity): the VM's copy was set to no health, and a hit from the VM on its copy was applied in the host's game as
# share * maximum = an infinite blow, felling an NPC nothing fells there. With the fix no share is sent of a maximum that is no
# finite number above 0 (the copy keeps its health), the host's game refuses the friend's hit on it (the copy gets the
# health back), and a death of the copy in the VM's game (a blow bigger than its health, a grenade there) leaves the host's
# raider standing (2026-11-23: the fix's first run refused the hit, then a grenade felled the copy and the death felled it).
#   1. Both players go to an open, flat spot (console player.moveto 0019A7F6, to reach the scene). The host places a raider
#      400 u in front of it (dev spawnenemies 001AE38D 1 400: made at runtime, shared by the session as
#      an ambush's) and holds it where it stands (console setrestrained 1); the VM's player walks (the pad) beside the host and
#      its game's copy of the raider is found (a raider near, run by the host's game).
#   2. The host's game gives its raider an infinite health (console <ref>.setav health 1e45 written out, past a float's range):
#      its permanent health then reads inf. The VM's copy is read 8 s on.
#   3. The VM shoots its copy (real clicks, up to 12, until one lands in its game).
#   4. At once, the VM's game kills its copy (console kill, a blow beyond its health). The host's raider is read 8 s on.
# REPRO: YES when the VM's copy is set to no health (or dies) after the host's raider's maximum went infinite, or when the VM's
# landed hit, or the copy's death, fells the host's raider. NO when the copy keeps its health and the host's raider stands
# after the hit and the copy's death, and its game heard of that death. VOID when the scene could not be made (no raider
# placed, no copy found, the maximum not infinite, no shot landing in 12) or the death never reached the host's game.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
RAIDER=${RAIDER:-001AE38D}
MARKER=${MARKER:-0019A7F6}   # an open, flat spot in the Commonwealth (a quest's reinforcements' marker)
HUGE=999999999999999999999999999999999999999999999  # 1e45: past a float's range, read as infinity
run_cap 900
need_session

info() { dc "$1" "refinfo $2"; }
hp() { field "$(info "$1" "$2")" hp; }
perm() { field "$(info "$1" "$2")" permHp; }
dead() { [ "$(field "$(info "$1" "$2")" dead)" = true ]; }
owner_of() { field "$(dc "$1" "ghosts $2")" owner; }
shots_of() { dc "$1" "net status" | grep -oE ' shots=[0-9]+' | head -1 | cut -d= -f2; }
lower() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a != "" && b != "" && a + 0 < b - 0.5) }'; }
zeroish() { awk -v a="$1" 'BEGIN { exit !(a != "" && a + 0 < 1) }'; }

# 1. the raider, held, and the VM's copy of it
HID=$(field "$(dc host 'net status')" id); VID=$(field "$(dc vm 'net status')" id)
# both to a flat open spot first (console player.moveto, to reach the scene): where the save stands, a raider 400 u ahead
# stood 280 u up a slope and none of the VM's shots reached it (2026-11-23)
for w in host vm; do
	dc $w focus >/dev/null; dc $w "console player.moveto $MARKER" >/dev/null; sleep 12; wait_ingame $w 90 || verdict VOID "$w is not in the game after the move"
done
sleep 5
rwalk vm $(pos_of host) 200 || true
dc host "console player.additem 00004822 1" >/dev/null; dc vm "console player.additem 00004822 1" >/dev/null
dc vm "console player.additem 0001F276 200" >/dev/null; dc vm "console player.equipitem 00004822" >/dev/null; dc vm "draw on" >/dev/null
rmark host set; rmark vm set
R=$(dc host "spawnenemies $RAIDER 1 400" | grep -oE 'ids=[^ ]*' | cut -d= -f2 | cut -d, -f1)
[ -n "$R" ] || verdict VOID "the host's game placed no raider (dev spawnenemies $RAIDER 1 400)"
dc host "console $R.setrestrained 1" >/dev/null
say "set-up: the host's raider $R placed 400 u ahead of the host (dev spawnenemies) and held (console setrestrained 1); player ids host $HID, vm $VID"
# its spawn id (the host's log: "sharing <ref> (base ...) as <id>") and the VM's copy of it ("placed <ref> ... as our copy of
# player <host>'s <id>"), up to 40 s: a copy stands in the VM's game only once the host shared it
SID=""; C=""
for i in $(seq 1 20); do
	sleep 2
	[ -n "$SID" ] || SID=$(rsince host set | grep -oE "Spawns: sharing $R \(base [0-9A-F]+\) as FF[0-9A-F]{6}" | tail -1 | grep -oE 'FF[0-9A-F]{6}$')
	[ -n "$SID" ] && C=$(rsince vm set | grep -oE "Spawns: placed FF[0-9A-F]{6} .* as our copy of player $HID's $SID" | tail -1 | grep -oE 'placed FF[0-9A-F]{6}' | cut -c8-)
	[ -n "$C" ] && ! dead vm "$C" && break
done
if [ -z "$C" ]; then
	rsince host set | grep -E "Spawns: .*$R|$SID" | tail -6 | sed 's/^/  host| /' | tee -a "$REPRO_OUT/run.log"
	rsince vm set | grep -E "Spawns: .*${SID:-xxxx}" | tail -6 | sed 's/^/  vm| /' | tee -a "$REPRO_OUT/run.log"
	rkeep host infinite-health; rkeep vm infinite-health
	verdict VOID "the VM's game placed no copy of the host's raider $R (spawn ${SID:-not shared}) in 40 s"
fi
say "set-up: the VM's copy of it: $C at $(hp vm "$C") health (max $(perm vm "$C")); the host's: $(hp host "$R") (max $(perm host "$R"))"
rmark host inf; rmark vm inf

# 2. an infinite health in the host's game
dc host "console $R.setav health $HUGE" >/dev/null; sleep 1
P=$(perm host "$R"); H=$(hp host "$R")
say "the host's raider's health set past a float's range (console $R.setav health 1e45): health $H, max $P"
grep -qiE 'inf' <<<"$P" || verdict VOID "the host's raider's maximum did not go infinite ($P): the console took the number otherwise"
CV0=$(hp vm "$C"); sleep 8; CV1=$(hp vm "$C")
say "the VM's copy $C: health $CV0 at once, $CV1 8 s on$(dead vm "$C" && echo ' (dead)')"
rsince host inf | grep -E "health|$R|non-finite|not finite" | tail -6 | sed 's/^/  host| /' | tee -a "$REPRO_OUT/run.log"
rsince vm inf | grep -E "health|$C|refus" | tail -6 | sed 's/^/  vm| /' | tee -a "$REPRO_OUT/run.log"
BAD=""
if dead vm "$C" || zeroish "$CV1"; then BAD="$BAD the VM's copy was set to no health ($CV1) once the host's raider's maximum went infinite;"; fi

# 3. the VM shoots its copy
if ! dead vm "$C"; then
	rmark host shot; rmark vm shot
	h0=$(hp vm "$C"); n0=$(shots_of vm); LANDED=""
	# a hit on a copy shows in the copyHits count; the copy's own health follows its owner's reports and may not drop at once
	ch0=$(field "$(dc vm 'net status')" copyHits)
	for i in $(seq 1 12); do
		[ $((i % 4)) -eq 1 ] && { rwalk vm "$C" 150 || true; }
		dc vm "turnto $C 300 aim" >/dev/null; sleep 0.5; dc vm "click 80" >/dev/null; sleep 1.2
		h=$(hp vm "$C"); ch=$(field "$(dc vm 'net status')" copyHits)
		if dead vm "$C" || lower "$h" "$h0" || lower "$ch0" "$ch"; then LANDED=$i; break; fi
	done
	n1=$(shots_of vm)
	[ -n "$n0" ] && [ -n "$n1" ] && [ "$n1" -gt "$n0" ] || verdict VOID "the VM's clicks fired nothing (shots=$n0 before, $n1 after)"
	[ -n "$LANDED" ] || verdict VOID "none of the VM's 12 shots at its copy $C landed (shots=$n0 -> $n1)"
	# 4. at once, the copy dies in the VM's game (console kill: a blow beyond the copy's health, as a grenade there dealt one on
	# the rig). The server takes a friend's death of a spawn only within 10 s of their fight (this hit), so no read comes first.
	rmark host death
	if dead vm "$C"; then KILLED="the hit felled the copy"; else dc vm "console $C.kill" >/dev/null; KILLED="the VM's game killed its copy (console kill)"; fi
	sleep 8
	say "[vm shoots] shot $LANDED landed on the copy $C (at $h0 before it); $KILLED; the host's raider $R 8 s on: health $(hp host "$R")$(dead host "$R" && echo ' (dead)')"
	rsince host shot | grep -E "damage on|Spawns: .*($R|no end)" | tail -8 | sed 's/^/  host| /' | tee -a "$REPRO_OUT/run.log"
	rsince vm shot | grep -E "CopyWatch|refus|$C" | tail -6 | sed 's/^/  vm| /' | tee -a "$REPRO_OUT/run.log"
	REFUSED=$(rsince host shot | grep -cE "damage on $R .*refused")
	KEPT=$(rsince host death | grep -cE "Spawns: player [0-9]+'s copy of our $R .*has no end here")
	dead host "$R" && BAD="$BAD the VM's hit or its copy's death felled the host's raider, whose health is infinite there (hit refused there: $REFUSED);"
else
	say "[vm shoots] skipped: the copy is dead already"
fi
rkeep host infinite-health; rkeep vm infinite-health
dc host "console $R.disable" >/dev/null
[ -n "$BAD" ] && verdict YES "an NPC with an infinite health maximum is broken across the games:$BAD"
[ "${KEPT:-0}" -gt 0 ] || verdict VOID "the host's raider stands, but its game never heard of the copy's death (the server takes it only within 10 s of the VM's fight, or the VM's game sent none)"
verdict NO "the copy kept its health ($CV1) once the host's raider's maximum went infinite; the VM's hit was refused in the host's game ($REFUSED line(s)) and the copy's death left the host's raider standing"
