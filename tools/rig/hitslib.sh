#!/bin/bash
# hitslib.sh: the part of hits.sh that needs no game: where a raider may be put (spot_for) and the lobby spots known bad.
# Sourced by hits.sh; `bash hitslib.sh selftest` checks it without touching a game.
declare -F field >/dev/null || field() { grep -oE "(^|[] [(;|,:])$2=[^] ;|,)]*" <<<"$1" | head -1 | sed 's/^[^=]*=//'; }   # assert.sh's

# The lobby's far row straight ahead (about 435 u in front of the coc marker, 0 and 15 degrees either way):
# shots from where the players stand almost never reach a raider there, from either game, as runner or as shooter
# (2026-11-01, five runs: the pause raider's runner missed it 2 or 3 times at -549,1150 in three of them, and the round raider
# at -782,1150 was missed in three; one shot of about a dozen landed). Each such spot cost a step its set-up, or a re-place.
# Within 75 u of them only (an entry's third number; 110 otherwise): the row 100 u nearer is another line, and the second
# phase, with the first one's body at 0 degrees, needs it (a 110 u ring left its pause raider no spot, 2026-11-01).
LOBBY_BLOCKED="-545,1150,75 -660,1135,75 -780,1150,75"
# Spots a prop stands on (a display car, the counter, the machines): a raider put there is pushed off or stands on top of it
# (placed_ok says "stood elsewhere"). From the targets.txt of the 40 runs of 2026-11-01 to 11-14, each spot by how often a
# raider put within 10 u of it stayed: -980,1272 (3 of 24), -1055,1366 (0 of 8), -1108,1590 (0 of 3), -885,1202 (4 of 11, and
# 3 of those 4 had a blocked line), -305,1275 (1 of 6). Each cost one of place_raider's tries, and a raider whose tries ran
# out ended the picking: two phases of 2026-11-14 had 2 targets, want 4. Within 60 u only: the spots 100 u nearer stand.
LOBBY_PROPS="-980,1272,60 -1055,1366,60 -1108,1590,60 -885,1202,60 -305,1275,60"

# spot_for "<shooter's pos>" "<other's pos>" "<x,y x,y ...>" <both 1|0> ["<x,y ...>"]: "x y z degrees" for a raider 250 to 450 u
# from the shooter, the first of a few headings off its own with a clear line from the shooter (and, with both=1, from the other
# player too: nothing within 35 u of the line, past the first 10 u), 150 u or more from the other player, clear of the spots
# already taken (110 u apart, out of every line to them) and of the spots to avoid (the last list: spots tried and found bad,
# 110 u apart, or "x,y,r": r u). Empty if none is. The positions (dev pos lines) are read once per phase: a player's heading can drift while
# raiders are moved around it (2026-10-31: the VM's turned 90 degrees during a set-up, and the last two spots landed behind it).
# Both coc'd to the same marker, the players stand about 45 u apart side by side, so near raiders on either side are in one
# player's line: the far ring gives the second raider both can shoot (the 0.10.1 runs' kill step fired 16 shots that hit a
# barrel and a stand-in, never the raider). Bodies are all it knows of: the lobby's display cars, counter, rope stands and
# machines are not (2026-11-01: a raider put at a spot stood 18 u up, on something, and another was pushed 63 u off and 48 u
# up), so a spot is proved in the games: placed_ok, and each step's shot that must reach the raider first.
spot_for() {
	awk -v px="$(field "$1" x)" -v py="$(field "$1" y)" -v pz="$(field "$1" z)" -v yaw="$(field "$1" rz)" -v ox="$(field "$2" x)" -v oy="$(field "$2" y)" -v taken="$3" -v both="$4" -v avoid="$5" '
	# blk: q stands in the line of fire from a to b (more than 10 u ahead of a, short of b, within 35 u of the line: a body)
	function blk(ax, ay, bx, by, qx, qy,   dx, dy, l, s, d) { dx = bx - ax; dy = by - ay; l = sqrt(dx * dx + dy * dy); if (l == 0) return 0; s = ((qx - ax) * dx + (qy - ay) * dy) / l; d = ((qx - ax) * dy - (qy - ay) * dx) / l; if (d < 0) d = -d; return s > 10 && s < l - 20 && d < 35 }
	function try(ang, rr,   a, nx, ny, ok, j, q) {
		a = (yaw + ang) * 3.14159265 / 180; nx = px + rr * sin(a); ny = py + rr * cos(a); ok = 1
		if (blk(px, py, nx, ny, ox, oy) || (both && blk(ox, oy, nx, ny, px, py)) || sqrt((nx - ox) ^ 2 + (ny - oy) ^ 2) < 150) ok = 0
		for (j = 1; j <= n && ok; j++) { split(T[j], q, ",")
			if (sqrt((nx - q[1]) ^ 2 + (ny - q[2]) ^ 2) < 110 || blk(px, py, nx, ny, q[1], q[2]) || (both && blk(ox, oy, nx, ny, q[1], q[2])) || blk(px, py, q[1], q[2], nx, ny) || blk(ox, oy, q[1], q[2], nx, ny)) ok = 0 }
		for (j = 1; j <= na && ok; j++) { split(V[j], q, ","); if (sqrt((nx - q[1]) ^ 2 + (ny - q[2]) ^ 2) < (q[3] == "" ? 110 : q[3])) ok = 0 }
		if (ok) printf "%.1f %.1f %.1f %d\n", nx, ny, pz, ang
		return ok
	}
	BEGIN {
		if (px == "" || ox == "" || yaw == "") exit
		n = split(taken, T, " "); na = split(avoid, V, " ")
		# the first spots as before, then a finer set in front, 250 to 450 u: with the players side by side only about 40
		# degrees either side give both a line, and once the kill and copy-hit raiders, a body and the bad spots had used up
		# those six, a pause raider moved for a blocked line found no spot and its step was not run (2026-11-01)
		nc = split("0 -15 15 -30 30 -45 45 -60 60 -90 90", C, " ")
		for (i = 1; i <= nc; i++) for (rr = 250; rr <= 450; rr += 200) if (try(C[i], rr)) exit
		nc = split("0 -8 8 -15 15 -23 23 -30 30 -38 38", C, " ")
		for (i = 1; i <= nc; i++) for (rr = 250; rr <= 450; rr += 100) if (try(C[i], rr)) exit
		# then the headings and distances between those, and the sides, reached only when the two sets above gave nothing:
		# on 2026-11-14 a pause raider and a copy-hit raider moved off a blocked line found "no other spot" in three phases
		# with both sets used up (three raiders placed, the prop spots and one blocked line avoided), and their steps were
		# not run. A spot here is proved in the games like any other.
		nc = split("-4 4 -11 11 -19 19 -26 26 -34 34 -41 41 -52 52 -68 68 -75 75", C, " ")
		for (i = 1; i <= nc; i++) for (rr = 300; rr <= 400; rr += 50) if (try(C[i], rr)) exit
	}'
}

# shot_fault <shooter> <check name> <reason>: the set-up fault of a shooter's shot that reached its target at no spot (its
# game counted no hit on a copy: copyHits). A blocked line reads so, and so would a mod that never counted that game's hits on
# copies; one run's step can't tell them apart, the whole run can. The names are kept per shooter, and at the end
# (shot_faults_stand) a shooter whose copyHits never rose in the whole run, while runners' own shots landed (RUNNER_HITS),
# has them turned back into FAILs.
declare -A SHOT_FAULTS; RUNNER_HITS=0   # RUNNER_HITS: the runners' own shots that landed (hits.sh counts them)
shot_fault() { SHOT_FAULTS[$1]="${SHOT_FAULTS[$1]}$2"$'\n'; setup_fault "$2" "$3"; }
shot_faults_stand() {  # shot_faults_stand <shooter> <its copyHits' rise over the run>
	local nm
	[ -n "${SHOT_FAULTS[$1]}" ] && is_num "$2" && [ "$2" -le 0 ] && [ "$RUNNER_HITS" -gt 0 ] || return 0
	while IFS= read -r nm; do
		[ -n "$nm" ] && rig_fault_stands setup "$nm" "no shot of $1's reached a copy in the whole run (its copyHits rose by $2) while $RUNNER_HITS of the runners' own shots landed: that is not a blocked line at each spot, so this failure stands"
	done <<<"${SHOT_FAULTS[$1]}"
	echo "$1's shots reached no copy in the whole run: its set-up faults \"the shooter's shot reaches\" are FAILs again"
}
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "$1" = selftest ]; then
	bad=0
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2], want [$3]"; bad=1; fi; }
	# far <spot "x y z deg"> <x,y> <u>: the spot is at least that far from the point
	far() { awk -v s="$1" -v q="$2" -v d="$3" 'BEGIN{split(s, a, " "); split(q, b, ","); exit !(s != "" && sqrt((a[1] - b[1]) ^ 2 + (a[2] - b[2]) ^ 2) >= d)}'; }
	S="pos x=-664.5 y=1593.1 z=-256.0 rz=180.0"; O="pos x=-625.2 y=1577.9 z=-256.0 rz=180.0"   # where the players stand after the coc
	AV="$LOBBY_BLOCKED $LOBBY_PROPS"
	# an empty lobby: straight ahead of the shooter, 250 u
	t "the first spot is straight ahead, 250 u" "$(spot_for "$S" "$O" "" 1 "")" "-664.5 1343.1 -256.0 0"
	t "a position that was not read gives no spot" "$(spot_for "pos" "$O" "" 1 "")" ""
	# the four targets of a phase, placed one after the other: four spots, 110 u apart, none on a prop
	taken=""; n=0; for k in 1 1 0 0; do sp=$(spot_for "$S" "$O" "$taken" $k "$AV"); [ -n "$sp" ] || break; n=$((n + 1)); read -r x y _ <<<"$sp"
		for q in $taken; do far "$sp" "$q" 110 || { echo "FAIL spot $n ($sp) is within 110 u of $q"; bad=1; }; done
		for q in $LOBBY_PROPS; do far "$sp" "${q%,*}" 60 || { echo "FAIL spot $n ($sp) is on the prop spot $q"; bad=1; }; done
		taken="$taken $x,$y"; done
	t "a phase's four targets each get a spot" "$n" 4
	# the prop spot at 45 degrees, 450 u (a display car: 21 of 24 raiders put there stood elsewhere) is not offered any more
	t "the prop spot at 45 degrees is skipped" "$(spot_for "$S" "$O" "-664.5,1343.1 -789.5,1376.6" 0 "$AV" | cut -d' ' -f1-2)" "-881.0 1468.1"
	# 2026-11-14, 02:34: kill, pause and round placed, three prop spots and the round2 raider's blocked spot avoided: the two
	# older sets give nothing ("round2: no other spot"); the set between them does
	tk="-664.5,1343.1 -789.5,1376.6 -914.5,1593.1"; av="$LOBBY_BLOCKED -982.7,1274.9 -881.0,1468.1 -1054.2,1368.1 -755.1,1255.0"
	sp=$(spot_for "$S" "$O" "$tk" 0 "$av"); [ -n "$sp" ] && echo "ok   a copy-hit raider moved off a blocked line gets another spot ($sp)" || { echo "FAIL a copy-hit raider moved off a blocked line gets another spot: none"; bad=1; }
	for q in $tk -755.1,1255.0; do far "$sp" "$q" 110 || { echo "FAIL that spot ($sp) is within 110 u of $q"; bad=1; }; done
	# the same phase's pause raider (both players shoot it), moved off its blocked spot, the prop spot it was then put on avoided
	tk="-664.5,1343.1 -914.5,1593.1"; av="$av -789.5,1376.6 -889.5,1203.4"
	sp=$(spot_for "$S" "$O" "$tk" 1 "$av"); [ -n "$sp" ] && echo "ok   a pause raider moved off a blocked line gets another spot ($sp)" || { echo "FAIL a pause raider moved off a blocked line gets another spot: none"; bad=1; }
	far "$sp" "-625.2,1577.9" 150 || { echo "FAIL the pause raider's spot ($sp) is within 150 u of the other player"; bad=1; }
	# an avoided spot's ring: 110 u without a third number, the number with one
	t "an avoided spot keeps 110 u clear" "$(spot_for "$S" "$O" "" 1 "-664.5,1343.1" | cut -d' ' -f4)" "0"
	t "... the far ring at the same heading" "$(spot_for "$S" "$O" "" 1 "-664.5,1343.1" | cut -d' ' -f2)" "1143.1"
	t "an avoided spot with a radius keeps only that clear" "$(spot_for "$S" "$O" "" 1 "-664.5,1300,40" | cut -d' ' -f1-2)" "-664.5 1343.1"
	# the whole run's rule for a shooter's shot (shot_fault, shot_faults_stand), with assert.sh's parts stood in for
	is_num() { [[ $1 =~ ^-?[0-9]+(\.[0-9]+)?$ ]]; }; setup_fault() { LOG="${LOG}setup[$1] "; }; rig_fault_stands() { LOG="${LOG}stands[$1|$2] "; }
	run() { LOG=""; SHOT_FAULTS=(); RUNNER_HITS=$1; shot_fault vm "round: set-up: the shooter's shot reaches the target" "not in 3 tries"; shot_fault vm "pause: set-up: the shooter's shot reached the raider" "none"; shot_fault host "round2: set-up: the shooter's shot reaches the target" "no"; shot_faults_stand vm "$2" >/dev/null; shot_faults_stand host "$3" >/dev/null; echo "$LOG"; }
	three="setup[round: set-up: the shooter's shot reaches the target] setup[pause: set-up: the shooter's shot reached the raider] setup[round2: set-up: the shooter's shot reaches the target] "
	t "a shooter's shot that reached at no spot is a set-up fault; with hits on copies in the run it stays one" "$(run 6 2 1)" "$three"
	t "a shooter none of whose shots reached a copy in the whole run, while runners' shots landed: its faults stand, the other's do not" "$(run 6 0 3)" "${three}stands[setup|round: set-up: the shooter's shot reaches the target] stands[setup|pause: set-up: the shooter's shot reached the raider] "
	t "both shooters with none: both stand" "$(run 6 0 0)" "${three}stands[setup|round: set-up: the shooter's shot reaches the target] stands[setup|pause: set-up: the shooter's shot reached the raider] stands[setup|round2: set-up: the shooter's shot reaches the target] "
	t "no runner's shot landed either (nothing reaches anything: the lobby, not the mod): they stay set-up faults" "$(run 0 0 0)" "$three"
	t "a count that could not be read turns nothing back" "$(run 6 "" "?")" "$three"
	t "a shooter with no such fault has nothing turned back" "$(LOG=""; SHOT_FAULTS=(); RUNNER_HITS=4; shot_faults_stand vm 0; echo "[$LOG]")" "[]"
	[ $bad = 0 ] && echo "selftest: all ok" || echo "selftest: FAILED"
	exit $bad
fi
