#!/bin/bash
# elevator-ride.sh: a Nexus report (0.20.4): "Elevators are a bit funky. If I press the button, the host will fall either through
# the floor or ceiling depending on which the direction the elevator is traveling." A lift's car (SimpleElevatorMasterScript) is
# moved by its own script in each game; the friend's press is replayed by their stand-in; the stand-in itself is placed where its
# player reports being. So in the watcher's game the car and the rider's stand-in part company by the lag.
#
# Both players coc'd to LIFT_CELL and put at the lift by dev `console player.moveto LIFT` (console placement, said so). The lift's
# buttons (ACTI refs near it whose name or editor ID says Button or Lift) and its moving parts (MSTT/STAT refs near it) are listed
# from the game. The RIDER walks onto the car (to CAR_SPOT, or the lift's own spot) and presses the nearest button with E; the
# WATCHER stands beside the shaft. For RIDE seconds, four times a second, the watcher's game is read for the car's drawn height
# (dev `refinfo`: 3dz of each moving part) and the rider's stand-in's height, and the rider's game for the rider's own height.
# REPRO: YES = while a car moved in the watcher's game the stand-in stood more than GAP u above or below it (what the report
# describes), or the watcher's car never moved though the rider's did; NO = the stand-in stayed within GAP of the car throughout;
# VOID = no button found, or no car moved in the rider's game (the press did nothing).
# Run from tools/rig/repro with both games joined (rig.sh join).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
LIFT_CELL=${LIFT_CELL:-ThicketExcavationsExt04}; LIFT=${LIFT:-00110358}   # the lift's master activator (SimpleElevatorMasterScript)
RIDER=${RIDER:-vm}; WATCHER=$(other_game $RIDER); RIDE=${RIDE:-16}; GAP=${GAP:-40}; CAR_SPOT=${CAR_SPOT:-}
local_id() { dc "$1" 'net status' | grep -oE 'status=[a-z]+ id=[0-9]+' | grep -oE '[0-9]+$'; }
puppet_of() { dc "$1" 'net status' | grep -oE "\[$2 '[^']*' puppet=[0-9A-F]{8}" | grep -oE '[0-9A-F]{8}$'; }
z3d() { field "$(dc "$1" "refinfo $2")" 3dz; }
zref() { field "$(dc "$1" "refinfo $2")" z; }
need_session
rmark host; rmark vm
for who in host vm; do dc $who "god on" >/dev/null; dc $who "console coc $LIFT_CELL" >/dev/null; done
say "both players coc'd to $LIFT_CELL (console placement)"; sleep 15; wait_ingame host 60; wait_ingame vm 60; sleep 8
for who in host vm; do dc $who "console player.moveto $LIFT" >/dev/null; done; sleep 5
say "both put at the lift $LIFT by moveto (console placement): $(dc host "refinfo $LIFT" | cut -c1-140)"
say "scripts on the lift: $(dc host "scripts $LIFT" | cut -c1-200)"
BUTTONS=$(dc $RIDER "refs 27 900" | tr ';' '\n' | grep -iE "button|lift|elevator" | sed 's/^ *//')
PARTS=$(dc $WATCHER "refs 38 900" | tr ';' '\n' | sed 's/^ *//'; dc $WATCHER "refs 36 600" | tr ';' '\n' | grep -iE "lift|elevator|platform|cage|car" | sed 's/^ *//')
say "buttons near the lift (rider's game): $(tr '\n' '|' <<<"$BUTTONS")"
say "moving-part candidates near the lift (watcher's game): $(tr '\n' '|' <<<"$PARTS" | cut -c1-600)"
BTN=$(head -1 <<<"$BUTTONS" | cut -d' ' -f1)
[[ $BTN =~ ^[0-9A-F]{8}$ ]] || verdict VOID "no button found within 900 u of the lift (refs 27: $(dc $RIDER 'refs 27 900' | cut -c1-200))"
PART_IDS=$(cut -d' ' -f1 <<<"$PARTS" | grep -E '^[0-9A-F]{8}$' | head -12)
pup=$(puppet_of $WATCHER "$(local_id $RIDER)")
[ -n "$pup" ] || verdict VOID "the watcher has no stand-in of the rider"
# the rider onto the car: the lift's own spot unless CAR_SPOT says where; the watcher steps aside 250 u
read -r lx ly <<<"$(field "$(dc $RIDER "refinfo $LIFT")" x) $(field "$(dc $RIDER "refinfo $LIFT")" y)"
if [ -n "$CAR_SPOT" ]; then rwalk $RIDER ${CAR_SPOT} 40 0.6; else rwalk $RIDER "${lx%.*}" "${ly%.*}" 60 0.6; fi; say "$RIDER walked onto the car: $WALKED; at $(pos_of $RIDER) z=$(field "$(dc $RIDER pos)" z)"
rwalk $WATCHER $((${lx%.*} + 250)) "${ly%.*}" 80 0.7; say "$WATCHER stepped beside the lift: $WALKED; at $(pos_of $WATCHER)"
sleep 2
# heights before the press
declare -A Z0
for p in $PART_IDS; do Z0[$p]=$(z3d $WATCHER $p); done
rz0=$(field "$(dc $RIDER pos)" z); sz0=$(zref $WATCHER $pup)
say "before: rider z=$rz0, stand-in z (watcher)=$sz0, parts 3dz: $(for p in $PART_IDS; do printf '%s=%s ' $p "${Z0[$p]}"; done)"
rpress_e $RIDER $BTN; say "$RIDER pressed E on button $BTN (crosshair: '${ROLLOVER:-nothing}')"
# the ride, read four times a second
worst=0; movedW=0; movedR=0; moving_part=""; samples=0
printf "t rider_z standin_z(watcher) parts_3dz(watcher)\n" > "$REPRO_OUT/ride.txt"
end=$((SECONDS + RIDE))
while [ $SECONDS -lt $end ]; do
	rz=$(field "$(dc $RIDER pos)" z); sz=$(zref $WATCHER $pup); line=""
	for p in $PART_IDS; do
		z=$(z3d $WATCHER $p); line="$line $p=$z"
		d=$(awk -v a="$z" -v b="${Z0[$p]}" 'BEGIN { d = a - b; if (d < 0) d = -d; printf "%d", d }')
		if [ "${d:-0}" -gt 5 ]; then movedW=1; moving_part=$p
			g=$(awk -v s="$sz" -v c="$z" 'BEGIN { d = s - c; if (d < 0) d = -d; printf "%d", d }')
			[ "$g" -gt "$worst" ] && worst=$g
		fi
	done
	awk -v a="$rz" -v b="$rz0" 'BEGIN { exit !((a - b) > 20 || (b - a) > 20) }' && movedR=1
	samples=$((samples + 1)); echo "$SECONDS $rz $sz$line" >> "$REPRO_OUT/ride.txt"
	sleep 0.25
done
say "after: rider z=$(field "$(dc $RIDER pos)" z), stand-in z (watcher)=$(zref $WATCHER $pup); $samples samples in $REPRO_OUT/ride.txt"
say "the rider's own game: $([ $movedR = 1 ] && echo "the rider rose or fell (the car moved)" || echo "the rider's height never changed (no ride)")"
say "the watcher's game: $([ $movedW = 1 ] && echo "part $moving_part moved; the stand-in was at worst $worst u off the moving part's drawn height" || echo "no part moved")"
say "activations: rider $(dc $RIDER 'net status' | grep -oE 'acts=[0-9/]+') watcher $(dc $WATCHER 'net status' | grep -oE 'acts=[0-9/]+')"
rsince $WATCHER | grep -E "ApplyRemoteActivation|activation|refused" | tail -3 | sed "s/^/  $WATCHER log: /" | tee -a "$REPRO_OUT/run.log"
for who in host vm; do dc $who "god off" >/dev/null; done
rkeep host end; rkeep vm end
[ $movedR = 1 ] || verdict VOID "the press moved no car in the rider's game (button $BTN; the lift's parts: $(cut -c1-300 <<<"$PARTS" | tr '\n' '|'))"
[ $movedW = 1 ] || verdict YES "the rider rode the car in their game, but no car moved in the watcher's game (the press was not replayed there)"
[ "$worst" -gt "$GAP" ] && verdict YES "while the car moved in the watcher's game the rider's stand-in was up to $worst u off it (more than $GAP): through the floor or the ceiling"
verdict NO "the rider's stand-in stayed within $worst u of the moving car in the watcher's game (limit $GAP)"
