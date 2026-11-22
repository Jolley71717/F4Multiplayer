#!/bin/bash
# elevator-ride.sh: a Nexus report (0.20.4): "Elevators are a bit funky. If I press the button, the host will fall either through
# the floor or ceiling depending on which the direction the elevator is traveling." A lift's car (SimpleElevatorMasterScript) is
# moved by its own script in each game; a friend's press is replayed by their stand-in, so the two cars run a network trip apart;
# and the friend's stand-in was placed where its player reports being, so beside our car it sank into the floor or rose off it.
#
# As in the report, both players ride. Both are coc'd to LIFT_CELL and put on the car by dev `setpos` (console placement, said
# so; the spot is CAR, or read from the car's 3D: the middle of its largest shape at its collision body's height), then each
# takes a real step on it. Each ride lasts until the car has stopped again (RIDE s at most). The car's and the helper's 3D
# (nodes, bodies, collision layers) go to the run log as the lift's survey. The watcher's game traces every frame (dev `trace`: its own player's height and
# the stand-in's as it was drawn); the PRESSER starts the lift (a real E on BUTTON when the crosshair names it, else the dev
# channel's `activate`, said so). Two rides: the first pressed by the VM's player, the way back by the host's, so each game is
# once the one whose car leaves first. The measure, from the host's trace: how far the VM player's stand-in rose above or
# sank below the host's own player (who stands on the host's car) while either moved, against the gap the two had at rest.
# NETSIM (ms, optional) adds that much lag to the friend's movement states in the watcher's game (dev `netsim`), as a slower
# connection than the rig's LAN has.
# REPRO: YES = the stand-in was more than GAP u off the watcher's floor during a ride; NO = it stayed within GAP in both rides;
# VOID = the car did not move in the watcher's game (no ride to judge).
# Run from tools/rig/repro with both games joined (rig.sh join).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
LIFT_CELL=${LIFT_CELL:-ThicketExcavationsExt04}; LIFT=${LIFT:-00110358}   # the lift's master activator (SimpleElevatorMasterScript)
# Thicket's lift, read from Fallout4.esm: the master (a marker, LinkCustom01) moves the platform helper 00110377; the car,
# QryElevatorLift01 00110372 (a STAT), and the button inside it, 00110375, are attached to that helper (XATR) and ride with it;
# the call button at the foot, 00110376, is not. The car rests at the foot (z 894) and goes up some 3500 u.
CARREF=${CARREF:-00110372}                    # the car (where to stand is read from its 3D at run time)
HELPER=${HELPER:-00110377}                    # what the lift's animation moves (its 3D listed for the survey)
BUTTON=${BUTTON:-00110375}                    # the button that rides in the car
CAR=${CAR:-}                                  # "x y z": a spot on the car's floor; empty = from the car's 3D (`nodes`)
RIDE=${RIDE:-60}; GAP=${GAP:-12}; NETSIM=${NETSIM:-0}
local_id() { dc "$1" 'net status' | grep -oE 'status=[a-z]+ id=[0-9]+' | grep -oE '[0-9]+$'; }
# gap_of <trace.csv> <player id>: the stand-in's height over the watcher's own, sample by sample, against the first ten samples'
gap_of() {
	awk -F, -v p="p$2" '
		$2 == "me" { mez = $5; have = 1; next }
		$2 == p && have {
			have = 0; n++; g = $5 - mez
			if (n <= 10) { g0 += g / 10; z0 = mez; zlo = mez; zhi = mez; next }
			d = g - g0; if (mez > zhi) zhi = mez; if (mez < zlo) zlo = mez
			if (mez - z0 > 3 || z0 - mez > 3 || $5 - z0 - g0 > 3 || z0 + g0 - $5 > 3) { m++; if (d > hi) hi = d; if (d < lo) lo = d }
		}
		END { printf "samples=%d moving=%d restGap=%.1f above=%.1f below=%.1f travel=%.0f\n", n, m, g0, hi, -lo, zhi - zlo }' "$1"
}
need_session
VMID=$(local_id vm)
rmark host; rmark vm
for who in host vm; do dc $who "god on" >/dev/null; dc $who "console coc $LIFT_CELL" >/dev/null; done
say "both players coc'd to $LIFT_CELL (console placement)"; sleep 15; wait_ingame host 60; wait_ingame vm 60; sleep 8
# The survey, in the run log: the car's and the helper's 3D in each game (node names, collision bodies and their layers:
# L2 is an animated static, what the fix names always).
for who in host vm; do for r in $CARREF $HELPER; do say "survey: $who nodes $r: $(dc $who "nodes $r" | cut -c1-1400)"; done; done
if [ -z "$CAR" ]; then
	# The car's floor: under the middle of its largest drawn shape, at its first collision body's height.
	CAR=$(dc host "nodes $CARREF" | tr ';' '\n' | awk '
		/ geom / && match($0, /c=\([^)]*\) r=[0-9]+/) { s = substr($0, RSTART + 3, RLENGTH - 3); gsub(/[(),]/, " ", s); gsub(/r=/, "", s); split(s, a, " "); if (a[4] + 0 > r) { r = a[4] + 0; x = a[1]; y = a[2] } }
		/ body / && bz == "" && match($0, /at \([^)]*\)/) { s = substr($0, RSTART + 4, RLENGTH - 5); gsub(/,/, " ", s); split(s, b, " "); bz = b[3] }
		END { if (x != "" && bz != "") printf "%d %d %d", x, y, bz }')
	[ -n "$CAR" ] || verdict VOID "no spot on the car $CARREF: its 3D has no drawn shape or no collision body in the host's game"
	say "the car's floor from its 3D in the host's game: $CAR"
fi
read -r CX CY CZ <<<"$CAR"
dc host "setpos 00000014 $((CX - 35)) $CY $((CZ + 60)) 0" >/dev/null; dc vm "setpos 00000014 $((CX + 35)) $CY $((CZ + 60)) 0" >/dev/null; sleep 3
rwalk host $((CX - 20)) $CY 12 0.5; hs=$WALKED; rwalk vm $((CX + 20)) $CY 12 0.5
say "both put on the car by setpos 60 u over its floor (console placement), then a real step each: host $hs; vm $WALKED"
for who in host vm; do
	z=$(field "$(dc $who pos)" z)
	awk -v z="$z" -v c="$CZ" 'BEGIN { exit !(z != "" && z > c - 30 && z < c + 150) }' || verdict VOID "$who is not on the car after the setpos and the step (z=$z, the car's floor $CZ)"
done
say "on the car: host z=$(field "$(dc host pos)" z) vm z=$(field "$(dc vm pos)" z); the VM's stand-in in the host's game z=$(field "$(dc host "refinfo $(dc host 'net status' | grep -oE "\[$VMID '[^']*' puppet=[0-9A-F]{8}" | grep -oE '[0-9A-F]{8}$')")" z)"
[ "$NETSIM" != 0 ] && say "netsim in the host's game: $(dc host "netsim $NETSIM")"
WORST=0; RIDES=0; NOTES=""
# ride <presser> <n>: one ride, traced in the host's game
ride() {
	local presser=$1 n=$2 z0 z1 out a b
	z0=$(field "$(dc host pos)" z)
	dc host "trace start 60" >/dev/null; sleep 1.5
	dc $presser "turnto $BUTTON 300 aim" >/dev/null; sleep 0.8; ROLLOVER=$(rrollover $presser)
	if grep -qi "button\|elevator\|lift" <<<"$ROLLOVER"; then rkey $presser "$VK_E" 100; say "ride $n: $presser pressed E on $BUTTON (crosshair: '$ROLLOVER')"
	else dc $presser "activate $BUTTON" >/dev/null; say "ride $n: $presser started the lift by the dev channel's activate on $BUTTON (the crosshair named '${ROLLOVER:-nothing}', not the button)"; fi
	# Until the host's car has gone and stopped again (its height still for 4 s after moving), RIDE s at most.
	local end=$((SECONDS + RIDE)) last=$z0 still=0 went=0 zn
	while [ $SECONDS -lt $end ]; do
		sleep 2; zn=$(field "$(dc host pos)" z)
		awk -v a="$zn" -v b="$z0" 'BEGIN { exit !(a - b > 100 || b - a > 100) }' && went=1
		if awk -v a="$zn" -v b="$last" 'BEGIN { exit !(a - b < 3 && b - a < 3) }'; then still=$((still + 1)); else still=0; fi
		last=$zn; [ $went = 1 ] && [ $still -ge 2 ] && break
	done
	out=$(dc host "trace stop"); z1=$(field "$(dc host pos)" z)
	cp -f "$HOST_F4SE/F4Multiplayer_trace.csv" "$REPRO_OUT/trace-ride$n.csv" 2>/dev/null || { say "ride $n: no trace file ($out)"; return; }
	out=$(gap_of "$REPRO_OUT/trace-ride$n.csv" "$VMID")
	say "ride $n ($presser pressed; the host's height $z0 -> $z1): $out"
	a=$(field "$out" above); b=$(field "$out" below)
	[ "$(field "$out" moving)" -gt 20 ] 2>/dev/null || { say "ride $n: the host's car did not move: not judged"; return; }
	RIDES=$((RIDES + 1))
	for v in "$a" "$b"; do awk -v v="$v" -v w="$WORST" 'BEGIN { exit !(v > w) }' && WORST=$v; done
	NOTES="$NOTES; ride $n ($presser pressed): up to $a above and $b below the floor"
}
ride vm 1
sleep 4
ride host 2
[ "$NETSIM" != 0 ] && dc host "netsim off" >/dev/null
say "rides counted: host $(dc host 'net status' | grep -oE 'rideFrames=[0-9]+ rideMisses=[0-9]+ rideApartMost=[0-9]+') vm $(dc vm 'net status' | grep -oE 'ridesNamed=[0-9]+') (builds that have them)"
rsince host | grep -E "RemotePlayers: player [0-9]+('s stand-in| stands on)" | tail -6 | sed 's/^/  host log: /' | tee -a "$REPRO_OUT/run.log"
rsince vm | grep -E "Session: our player stands on" | tail -3 | sed 's/^/  vm log: /' | tee -a "$REPRO_OUT/run.log"
for who in host vm; do dc $who "god off" >/dev/null; done
rkeep host end; rkeep vm end
[ "$RIDES" = 0 ] && verdict VOID "the car moved in the host's game in neither ride"
awk -v w="$WORST" -v g="$GAP" 'BEGIN { exit !(w > g) }' && verdict YES "the friend's stand-in was up to $WORST u off the floor of the car both rode (limit $GAP):${NOTES#;}"
verdict NO "the friend's stand-in stayed within $WORST u of the floor of the car both rode, in $RIDES ride(s) (limit $GAP):${NOTES#;}"
