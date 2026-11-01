#!/bin/bash
# hits.sh
#
# Health agreement between the two games (docs/HEALTH-SYNC.md: hits on a friend's enemy are numbered, answered, refused,
# rewound). Each player shoots NPCs the OTHER game runs, and what the two games say of each one's health is compared.
#
#   set up      Corvega's own raiders (the same form ID in both games). Who runs them is whoever entered the cell first, so
#               the check runs twice: the host enters first and the VM shoots, then the VM enters first and the host shoots.
#               Four raiders that both games say the runner runs are moved near the shooter (NPCs are moved, never the
#               players: they turn with turnto and fire real clicks) and held still, each at a spot neither player stands in
#               the other's line of fire to (spot_for). Each player has a gun AND its ammo (counted before shooting).
#   what a hit does (HEALTH-SYNC.md, NpcClaims.h, Server.cpp handleHealth): a hit on a raider another game runs asks to take it
#               over (a claim, ahead of the damage on the same ordered connection). The server grants it unless the runner hit
#               it within its takeover cooldown (5 s); granted, the damage that follows is answered by the SERVER "you run it"
#               (the shooter's copyAnswers rises, nothing is given back, the runner's damageAnswered does not move). Refused,
#               the damage goes to the runner, which takes it off and answers it "applied" (the runner's damageAnswered rises).
#   copy hits   (round, round2) one shot at a raider nobody has hit: the takeover path. Health fell in both games and agrees,
#               the shooter now runs it in both games, its copyAnswers rose and copyAnswers' refunds did not.
#   pause       the runner shoots its raider (keeping it: its claim starts the cooldown), opens the pause menu, and the other
#               player shoots it at once (one pair_shot): the damage goes to the runner's game while it is in a menu (0.10.1: held and applied,
#               not lost). Health agrees after the menu closes, the runner still runs it in both games, and its damageAnswered
#               rose. It runs last: a menu open past 5 s hands the runner's NPCs to friends (PauseHandOff), the other targets too.
#   kill        (before pause) runner and shooter fire in pairs (pair_shot: both aim, then the runner clicks and the shooter
#               within T_PAIR_GAP_MS, well inside the 5 s cooldown, so the runner keeps the raider and the shooter's hits go to it as
#               damage) until it dies: dead in both games, and the runner's damageAnswered rose.
#   before both the raider is checked alive, the runner's and at full health (restored if a fight hurt it: fresh_target), and
#               both players' shots must reach it (prove_lines: one of the runner's own, then the shooter's in a pair); a spot where
#               one does not is left for another (re_place), up to 3. No spot, or no fresh target, FAILS as set-up and skips the
#               step. A kill whose raider the shooter takes over mid-way FAILS as that, not as missing damage.
#   lines       The lobby's display cars, counter, rope stands and machines stand in some lines of fire (spot_for knows only of
#               bodies): a raider put into one is moved on (placed_ok), and every step proves its shots reach the raider before it
#               counts anything. A copy hit's shot is its own proof: one that meets something else changes nothing in either game
#               (no hit: no claim, no damage), so the raider is moved and shot again, or replaced by a spare, up to 3 tries; then
#               it FAILS as set-up, naming the blocked line, and the step is not run. Each try is in targets.txt.
#   end         no living NPC's health rose in a copy alone (a heal-back: the 0.10.1 bug); copyOwed = 0 on both games;
#               copyUnseen, the session's ignored hits and copyLost unchanged; give-backs and real heals are reported.
#   Targets must be alive in both games when the shooting starts (a raider the session has dead can read alive in `actors`).
#   A click that fires nothing (the game's own shots counter did not rise) FAILS once per game: its misses are not the mod's.
#   A step whose shot did not land (health unchanged in both games) is inconclusive and FAILS; the spots and positions are in
#   targets.txt. Health is the current value (hp=), never the base. damageHeld is not asserted: a menu does not hold damage
#   (WorldSync::InWorld is false only on a loading screen), so it moves only for a runner on a loading screen or an NPC not
#   loaded there (tests/DamageHoldTests.cpp).
#
#   Preflight: both machines run the same DLL, and with CHECK_DLL=<the F4Multiplayer.dll built for the run, or its md5> the one
#   given (dll_preflight in steps.sh; a mismatch stops the run).
#
# Exit code = number of failed checks. About 11 minutes (two cell entries of about a minute each). Needs both games joined and the rig held (see guard.sh).
. "$(dirname "${BASH_SOURCE[0]}")/assert.sh"
. "$RIG/steps.sh"
. "$RIG/crashlib.sh"
. "$RIG/guard.sh"

while [ $# -gt 0 ]; do case "$1" in *) echo "unknown option $1"; exit 2;; esac; done
rig_guard || exit 2
a_init hits; tunnel
trap a_interrupted INT TERM
trap put_back EXIT   # an exit part-way (interrupt, circuit breaker) still lets the raiders go and puts their health back
SERIES=$A_OUT/hp-series.txt; : > "$SERIES"

# --- targets: "<owner>:<ref in the owner's game>:<ref in the other game>:<role>" -----------------------------------------------------
declare -a TG; declare -A MYID FLIPPED
other_of() { [ "$1" = host ] && echo vm || echo host; }
# one refinfo call: current health (hp=, not baseHp), who runs the NPC (owner=, a player number) and whether it is dead
rinfo() { dc "$1" "refinfo $2"; }
hpv() { field "$(rinfo "$1" "$2")" hp; }
runner() { field "$(rinfo "$1" "$2")" owner; }
deadv() { field "$(rinfo "$1" "$2")" dead; }
ammo_of() { dc "$1" "count 00000014 0001F276" | grep -oE -- '-?[0-9]+' | tail -1; }   # rounds of the 10mm the pistol uses
# Corvega's own raiders, the same form ID in both games. Who runs them depends on who entered the cell first, so the check goes in
# twice: the host enters first (it runs them, the VM shoots), then the VM enters first (it runs them, the host shoots). One raider
# per step: a takeover or a kill uses it up.
ROLES=(kill pause round round2)   # placed in this order: the two both players shoot get the clearest spots
# enter_with_runner <runner>: both games leave to Sanctuary, then the runner enters Corvega, and 25 s later the other
enter_with_runner() {
	local r=$1 o; o=$(other_of "$r")
	bash "$RIG/dcfast.sh" both "console coc SanctuaryExt" >/dev/null 2>&1; sleep 15; wait_ingame host 30; wait_ingame vm 30
	dc "$r" "console coc CorvegaAssemblyPlant01" >/dev/null; sleep 25; wait_ingame "$r" 30
	dc "$o" "console coc CorvegaAssemblyPlant01" >/dev/null; sleep 25; wait_ingame "$o" 30
}
# spot_for "<shooter's pos>" "<other's pos>" "<x,y x,y ...>" <both 1|0> ["<x,y ...>"]: "x y z degrees" for a raider 250 or 450 u
# from the shooter, the first of a few headings off its own with a clear line from the shooter (and, with both=1, from the other
# player too: nothing within 35 u of the line, past the first 10 u), 150 u or more from the other player, clear of the spots
# already taken (110 u apart, out of every line to them) and of the spots to avoid (the last list: spots tried and found bad,
# 110 u apart). Empty if none is. The positions (dev pos lines) are read once per phase: a player's heading can drift while
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
	BEGIN {
		if (px == "" || ox == "" || yaw == "") exit
		n = split(taken, T, " "); na = split(avoid, V, " "); nc = split("0 -15 15 -30 30 -45 45 -60 60 -90 90", C, " ")
		for (i = 1; i <= nc; i++) for (rr = 250; rr <= 450; rr += 200) {
			a = (yaw + C[i]) * 3.14159265 / 180; nx = px + rr * sin(a); ny = py + rr * cos(a); ok = 1
			if (blk(px, py, nx, ny, ox, oy) || (both && blk(ox, oy, nx, ny, px, py)) || sqrt((nx - ox) ^ 2 + (ny - oy) ^ 2) < 150) ok = 0
			for (j = 1; j <= n && ok; j++) { split(T[j], q, ",")
				if (sqrt((nx - q[1]) ^ 2 + (ny - q[2]) ^ 2) < 110 || blk(px, py, nx, ny, q[1], q[2]) || (both && blk(ox, oy, nx, ny, q[1], q[2])) || blk(px, py, q[1], q[2], nx, ny) || blk(ox, oy, q[1], q[2], nx, ny)) ok = 0 }
			for (j = 1; j <= na && ok; j++) { split(V[j], q, ","); if (sqrt((nx - q[1]) ^ 2 + (ny - q[2]) ^ 2) < 110) ok = 0 }
			if (ok) { printf "%.1f %.1f %.1f %d\n", nx, ny, pz, C[i]; exit }
		}
	}'
}
# put_at <runner> <ref> <x> <y> <z>: the runner's console moves the NPC there (never a player) and holds it still
put_at() {
	case " ${HELD[*]} " in *" $2 "*) ;; *) HELD+=("$2");; esac
	dc "$1" "console $2.setrestrained 1" >/dev/null
	dc "$1" "console $2.setpos x $3" >/dev/null; dc "$1" "console $2.setpos y $4" >/dev/null; dc "$1" "console $2.setpos z $5" >/dev/null
}
# placed_ok <ref> <x> <y> <z>: the raider stands where it was put, on the players' floor, in both games (within 30 u across and
# 12 u up or down). One put into a prop is pushed out of it or stands on it (2026-11-01: 18 u up at one spot, 63 u off and 48 u
# up at another), behind or above what the shot meets: the spot is not used. Prints what was read, for targets.txt.
placed_ok() {
	local g i bad=0 out=""
	for g in host vm; do
		i=$(rinfo $g "$1")
		out="$out $g=($(field "$i" x),$(field "$i" y),$(field "$i" z))"
		awk -v x="$(field "$i" x)" -v y="$(field "$i" y)" -v z="$(field "$i" z)" -v px="$2" -v py="$3" -v pz="$4" \
			'BEGIN{ if (x == "" || z == "") exit 1; dz = z - pz; if (dz < 0) dz = -dz; exit !(sqrt((x - px) ^ 2 + (y - py) ^ 2) <= 30 && dz <= 12) }' || bad=1
	done
	echo "$out"; return $bad
}
# Where the phase's targets stand: SPOT[<ref>]="x,y" for each raider placed, AVOID the spots found bad (a raider pushed off or up,
# hurt or killed there, or a line that the shots proved blocked). PS_PH and PO_PH are the shooter's and the runner's pos lines,
# read once per phase (spot_for).
declare -A SPOT; AVOID=""; PS_PH=""; PO_PH=""
taken_but() { local r out=""; for r in "${!SPOT[@]}"; do [ "$r" = "$1" ] || out="$out ${SPOT[$r]}"; done; echo "$out"; }
# place_raider <runner> <ref> <both 1|0>: the raider put at the first good spot for it (spot_for, then placed_ok and unhurt), up
# to 4 spots. 0: placed (SPOT set, PLACED="x y z degrees"); 1: no good spot left; 2: hurt or killed at a spot (not usable).
place_raider() {
	local r=$1 a=$2 sp x y z off k got
	for k in 1 2 3 4; do
		sp=$(spot_for "$PS_PH" "$PO_PH" "$(taken_but "$a")" "$3" "$AVOID"); [ -z "$sp" ] && return 1
		read -r x y z off <<<"$sp"; put_at "$r" "$a" "$x" "$y" "$z"; sleep 1.5
		# a spot can be inside something that kills it (2026-10-31: one placed raider died there at once)
		if ! unhurt host "$a" || ! unhurt vm "$a"; then echo "raider $a died or was hurt at $x $y: spot avoided" >> "$A_OUT/targets.txt"; AVOID="$AVOID $x,$y"; return 2; fi
		if got=$(placed_ok "$a" "$x" "$y" "$z"); then SPOT[$a]="$x,$y"; PLACED="$x $y $z $off"; return 0; fi
		echo "raider $a put at $x $y $z ($off degrees) stood elsewhere:$got (on or in a prop): spot avoided" >> "$A_OUT/targets.txt"; AVOID="$AVOID $x,$y"
	done
	return 1
}
# swap_spare <target>: a copy-hit target's raider replaced by a spare (one pick_targets left unplaced: still alive, unhurt and the
# runner's), put at a good spot; the old one stays held where it stands. SWAPPED is the new target. 1: no spare could be used.
SPARE=()
swap_spare() {
	local t=$1 r a i role; r=$(tg_o "$t"); role=$(tg_role "$t")
	while [ ${#SPARE[@]} -gt 0 ]; do
		a=${SPARE[0]}; SPARE=("${SPARE[@]:1}")
		unhurt host "$a" && unhurt vm "$a" && [ "$(runner host "$a")" = "${MYID[$r]}" ] && [ "$(runner vm "$a")" = "${MYID[$r]}" ] || continue
		place_raider "$r" "$a" 0 || continue
		SWAPPED="$r:$a:$a:$role"
		for i in "${!TG[@]}"; do [ "${TG[$i]}" = "$t" ] && TG[$i]=$SWAPPED; done
		echo "$role $(tg_ro "$t") swapped for spare $a at $PLACED (x y z degrees)" >> "$A_OUT/targets.txt"
		sleep 1; sample "$SWAPPED" start; return 0
	done
	return 1
}
# re_place <target> <why>: its raider moved to another good spot (after a shot that did not reach it there). 1: none left.
re_place() {
	local t=$1 a r both; a=$(tg_ro "$t"); r=$(tg_o "$t"); both=0; case "$(tg_role "$t")" in kill|pause) both=1;; esac
	echo "$(tg_role "$t") $a at ${SPOT[$a]}: $2: spot avoided, moving it" >> "$A_OUT/targets.txt"
	AVOID="$AVOID ${SPOT[$a]}"; unset "SPOT[$a]"
	place_raider "$r" "$a" "$both" || { echo "$(tg_role "$t") $a: no other spot" >> "$A_OUT/targets.txt"; return 1; }
	echo "$(tg_role "$t") $a now at $PLACED (x y z degrees)" >> "$A_OUT/targets.txt"; sleep 1
}
# pick_targets <runner>: four living raiders that both games say the runner runs, each put at its own clear spot near the shooter
# unhurt <who> <ref>: alive in that game, with at least T_TARGET_HP health (a raider an earlier run hurt can die of the copy hit:
# a kill leaves no runner and no "you run it" answer to check)
T_TARGET_HP=${T_TARGET_HP:-25}
T_TOUGH_HP=${T_TOUGH_HP:-120}   # kill and pause targets get this much health in both games (pick_targets)
declare -A TOUGH_BASE   # "<game>:<ref>" -> that game's base health before it was raised, put back by put_back
HELD=()                 # raiders put_at holds still (setrestrained 1), let go by put_back
PAUSE_WHO=""            # the game whose pause menu the pause step has open
unhurt() { local i; i=$(rinfo "$1" "$2"); [ "$(field "$i" dead)" = false ] && is_num "$(field "$i" hp)" && awk -v h="$(field "$i" hp)" -v m="$T_TARGET_HP" 'BEGIN{exit !(h >= m)}'; }
pick_targets() {
	local r=$1 s a g k=0 ro rv x y z off rc; s=$(other_of "$r"); TG=(); SPOT=(); AVOID=""; SPARE=(); PS_PH=$(dc "$s" pos); PO_PH=$(dc "$r" pos)
	echo "phase $r runs: shooter $s at $(cut -d' ' -f2-5 <<<"$PS_PH"), $r at $(cut -d' ' -f2-5 <<<"$PO_PH")" >> "$A_OUT/targets.txt"
	for a in $(host 'actors 3000' | tr ';' '\n' | grep "Raider' dead=false" | grep -o "[0-9A-F]\{8\}"); do
		case " ${ALL_TG[*]} " in *":$a:"*) continue;; esac   # not one an earlier phase already shot
		unhurt host "$a" && unhurt vm "$a" || continue   # the actors list can call one alive that a game has dead; a hurt one can die of one hit
		ro=$(runner host "$a"); rv=$(runner vm "$a")
		[ "$ro" = "${MYID[$r]}" ] && [ "$rv" = "${MYID[$r]}" ] || continue
		if [ $k -ge 4 ]; then SPARE+=("$a"); [ ${#SPARE[@]} -ge 3 ] && break; continue; fi   # spares for a copy hit whose raider no spot suits
		place_raider "$r" "$a" "$([ $k -lt 2 ] && echo 1 || echo 0)"; rc=$?
		[ $rc = 1 ] && break; [ $rc = 2 ] && continue
		read -r x y z off <<<"$PLACED"
		# kill and pause are shot by both players: a raider of 30 hp died of the pair's first two hits (2026-10-31), and a hit
		# that kills a copy goes the death path, never routed damage, so neither step saw damageAnswered. Both games give
		# them T_TOUGH_HP (health syncs as a fraction of the most, so both must agree on the most).
		if [ $k -lt 2 ]; then
			for g in host vm; do TOUGH_BASE[$g:$a]=$(field "$(rinfo $g "$a")" baseHp); done
			if ! is_num "${TOUGH_BASE[host:$a]}" || ! is_num "${TOUGH_BASE[vm:$a]}"; then echo "raider $a: base health unread (host '${TOUGH_BASE[host:$a]}' vm '${TOUGH_BASE[vm:$a]}'), not raised, skipped" >> "$A_OUT/targets.txt"; unset "TOUGH_BASE[host:$a]" "TOUGH_BASE[vm:$a]"; continue; fi
			dc host "console $a.setav health $T_TOUGH_HP" >/dev/null; dc vm "console $a.setav health $T_TOUGH_HP" >/dev/null; sleep 1
			echo "raider $a health: host $(field "$(rinfo host "$a")" hp), vm $(field "$(rinfo vm "$a")" hp)" >> "$A_OUT/targets.txt"
			if ! T_TARGET_HP=$((T_TOUGH_HP - 1)) unhurt host "$a" || ! T_TARGET_HP=$((T_TOUGH_HP - 1)) unhurt vm "$a"; then echo "raider $a: health not raised, skipped" >> "$A_OUT/targets.txt"; continue; fi
		fi
		TG+=("$r:$a:$a:${ROLES[$k]}"); echo "target $r:$a role ${ROLES[$k]} at $x $y ($off degrees off the shooter's heading)" >> "$A_OUT/targets.txt"; k=$((k + 1))
	done
	sleep 3
	expect_ge "set up ($r runs, $s shoots): raiders both games say $r runs, each at a clear spot" "${#TG[@]}" 4
	for t in "${TG[@]}"; do sample "$t" start; done
	local dead=""; for t in "${TG[@]}"; do [ "$(deadv host "$(tg_ro "$t")")" = false ] && [ "$(deadv vm "$(tg_ro "$t")")" = false ] || dead="$dead $(tg_ro "$t")($(tg_role "$t"))"; done
	if [ -n "$dead" ]; then fail "set up ($r runs, $s shoots): the targets are alive in both games when the shooting starts" "dead:$dead (their steps prove nothing)"; else pass "set up ($r runs, $s shoots): the targets are alive in both games when the shooting starts" "${#TG[@]}"; fi
}
tg_o() { cut -d: -f1 <<<"$1"; }; tg_ro() { cut -d: -f2 <<<"$1"; }; tg_rp() { cut -d: -f3 <<<"$1"; }; tg_role() { cut -d: -f4 <<<"$1"; }
tg_by() { local t; for t in "${TG[@]}"; do [ "$(tg_o "$t")" = "$1" ] && [ "$(tg_role "$t")" = "$2" ] && { echo "$t"; return; }; done; }   # tg_by <runner> <role>
tg_ref() { [ "$2" = "$(tg_o "$1")" ] && tg_ro "$1" || tg_rp "$1"; }   # tg_ref <target> <game>: that game's ref

# sample <target> <label>: both games' health for the target into the series file
sample() {
	local t=$1 o p ri pi; o=$(tg_o "$t"); p=$(other_of "$o"); ri=$(rinfo "$o" "$(tg_ro "$t")"); pi=$(rinfo "$p" "$(tg_rp "$t")")
	echo "$(tg_ro "$t") $o $(field "$ri" hp) $(field "$ri" dead) $p $(field "$pi" hp) $(field "$pi" dead) $2 $(date +%T.%3N)" >> "$SERIES"
}
# armed <who>: tops up the 10mm rounds when low; FAILS (returns 1) when the player still has none (clicks would fire nothing)
armed() {
	local am; am=$(ammo_of "$1"); if ! is_num "$am" || [ "$am" -lt 5 ]; then dc "$1" "console player.additem 0001F276 400" >/dev/null; sleep 1; am=$(ammo_of "$1"); fi
	is_num "$am" && [ "$am" -ge 5 ] && return 0
	fail "$1 has ammo for the shot" "10mm count '$am': clicks would fire nothing"; return 1
}
# fire <who> <target>: one real shot (turn to the NPC, that game's own ref for it, and click), then both games' health sampled.
# The game's own count of its player's attacks (net status shots=, from the player's animation graph) must rise: a click that
# never reaches the game (on 2026-10-31 the host's mouse input stopped reaching its game while keys still did) fires nothing,
# and every check after it would read as a miss. That is said once per game, as what it is.
declare -A NOFIRE
shots_of() { dc "$1" "net status" | grep -oE ' shots=[0-9]+' | head -1 | cut -d= -f2; }
fired() {  # fired <who> <shots before> <shots after>
	if is_num "$2" && is_num "$3" && [ "$3" -le "$2" ] && [ -z "${NOFIRE[$1]}" ]; then
		NOFIRE[$1]=1; rig_broken input "$1's click fired its gun (its player's shots counter rose)" "shots=$2 before and $3 after: the click did not reach the game (its later misses are this, not the mod)$(mouse_why "$1")"
	fi
}
fire() {
	local r a b; r=$(tg_ref "$2" "$1"); a=$(shots_of "$1")
	dc "$1" "turnto $r 300" >/dev/null; sleep 0.5; dc "$1" "click 80" >/dev/null; sleep 0.6; sample "$2" "shot-$1"
	b=$(shots_of "$1"); fired "$1" "$a" "$b"
}
# pair_shot <runner> <shooter> <target> [esc]: the runner's shot and the shooter's, close together: both aim first, then the
# runner clicks and the shooter right after (with esc, the runner opens the pause menu in between). A whole fire() takes 7-9 s
# on this rig (turnto, click and the health reads go through the dev channel of a slow host game), longer than the server's 5 s
# takeover cooldown: a shooter's hit a whole fire() after the runner's takes the raider over, as designed (2026-10-31 runs). The
# gap between the two clicks is left in PAIR_GAP (ms) for the step to check against T_PAIR_GAP_MS.
# A dev command runs on its game's main thread, and its answer comes back as soon as it has run (DevChannel RunOnMainThread):
# the runner's click is pressed just before its answer, so the gap is counted from that answer to the shooter's. It used to be
# counted from before the runner's call, so the time that call waited for a hitching game's main thread counted too (2026-11-01:
# 4448 ms with the host game's frames at 45 ms and hitches of several seconds). Before the pair both games must answer a call
# within T_READY_MS (ready): one in a hitch would press its click late. Each pair's timings go to targets.txt.
T_PAIR_GAP_MS=${T_PAIR_GAP_MS:-4000}   # gaps up to 3.5 s were seen on the rig (2026-10-31); still inside the 5 s cooldown
T_READY_MS=${T_READY_MS:-1000}
call_ms() { local a; a=$(date +%s%3N); dc "$1" "$2" >/dev/null; echo $(( $(date +%s%3N) - a )); }
ready() {  # ready <who> <who>: both games answer `status` within T_READY_MS, up to 4 tries 1.5 s apart; READY_MS says what they took
	local k a b; for k in 1 2 3 4; do
		a=$(call_ms "$1" status); b=$(call_ms "$2" status); READY_MS="$1 $a ms, $2 $b ms"
		[ "$a" -le "$T_READY_MS" ] && [ "$b" -le "$T_READY_MS" ] && return 0; sleep 1.5
	done; return 1
}
pair_shot() {
	local o=$1 p=$2 t=$3 a b c0 so sp rd
	so=$(shots_of "$o"); sp=$(shots_of "$p")
	dc "$o" "turnto $(tg_ref "$t" "$o") 300" >/dev/null; dc "$p" "turnto $(tg_ref "$t" "$p") 300" >/dev/null
	dc "$o" focus >/dev/null; dc "$p" focus >/dev/null; ready "$o" "$p" && rd=ready || rd="NOT ready"; sleep 0.3
	c0=$(date +%s%3N); dc "$o" "click 80" >/dev/null; a=$(date +%s%3N); sleep 0.4
	[ "$4" = esc ] && { dc "$o" "key 0x1B 100" >/dev/null; PAIR_ESC=$(date +%s%3N); }
	dc "$p" "click 80" >/dev/null; b=$(date +%s%3N)
	PAIR_GAP=$((b - a))
	echo "pair $o then $p at $(tg_ro "$t") ($(tg_role "$t"))${4:+ with $4}: before it $rd ($READY_MS); the runner's click answered in $((a - c0)) ms, the shooter's $PAIR_GAP ms after that" >> "$A_OUT/targets.txt"
	# with esc the menu is open from here: every read waits for after it closes (PAIR_SHOTS), or the runner sits in it past
	# PauseHandOff's 5 s and hands the raider over (2026-11-01: closed 5-6 s after it opened, the shooter ran the raider after)
	if [ "$4" = esc ]; then PAIR_SHOTS="$so $sp"; return; fi
	sleep 0.6; sample "$t" "pair-$o-$p"
	fired "$o" "$so" "$(shots_of "$o")"; fired "$p" "$sp" "$(shots_of "$p")"
}
# fresh_target <label> <target>: before a kill or pause step, its raider is alive, still the runner's, and at its tough health in
# the runner's game (restored there if it was hurt; the copy follows): the raiders fight back while the other steps shoot, and
# a grenade or a fire before the step (2026-11-01: a pause target dead of a raider's grenade, a kill target burnt from 120 to
# 42 hp) made the step test nothing while it read as the mod's failure. FAILS, and the step is not run, if it can't be.
fresh_target() {
	local l=$1 t=$2 o ro i h k
	o=$(tg_o "$t"); ro=$(tg_ro "$t")
	i=$(rinfo "$o" "$ro"); h=$(field "$i" hp)
	if [ "$(field "$i" dead)" = false ] && is_num "$h" && _cmp "$h" '<' "$T_TOUGH_HP"; then
		dc "$o" "console $ro.restoreav health 1000" >/dev/null
		for k in 1 2 3 4; do sleep 1; i=$(rinfo "$o" "$ro"); h=$(field "$i" hp); is_num "$h" && _cmp "$h" '>=' "$T_TOUGH_HP" && break; done
	fi
	if [ "$(field "$i" dead)" = false ] && [ "$(field "$i" owner)" = "${MYID[$o]}" ] && is_num "$h" && _cmp "$h" '>=' "$((T_TOUGH_HP * 9 / 10))"; then
		pass "$l: set-up: the target is alive, $o's and at full health as the step starts" "hp $h, player $(field "$i" owner)"; return 0
	fi
	fail "$l: set-up: the target is alive, $o's and at full health as the step starts" "dead=$(field "$i" dead) runner=player '$(field "$i" owner)' hp=$h (hurt, killed or taken before its step; the raiders fight back): the step is not run"; return 1
}
# top_up <target>: its raider's health put back to T_TOUGH_HP in the runner's game (the copy follows), quietly, between the line
# proofs' tries. 1 if it is dead or not the runner's any more.
top_up() {
	local o ro i h k; o=$(tg_o "$1"); ro=$(tg_ro "$1")
	dc "$o" "console $ro.restoreav health 1000" >/dev/null
	for k in 1 2 3 4; do sleep 1; i=$(rinfo "$o" "$ro"); h=$(field "$i" hp); is_num "$h" && _cmp "$h" '>=' "$T_TOUGH_HP" && break; done
	[ "$(field "$i" dead)" = false ] && [ "$(field "$i" owner)" = "${MYID[$o]}" ]
}
# runner_reaches <target>: one of two shots of the runner's own reaches its raider (health falls in the runner's game). RL says what was seen.
# The kill and pause steps lean on the runner's hit landing just before the shooter's: one that hit furniture instead
# (2026-11-01: a desk fan in front of a seated raider) let the shooter's hit take the raider over, as designed, and the step
# read that as routed damage that never came.
runner_reaches() {
	local o ro h0 h1 k
	o=$(tg_o "$1"); ro=$(tg_ro "$1")
	for k in 1 2; do
		h0=$(hpv "$o" "$ro"); fire "$o" "$1"; sleep 0.5; h1=$(hpv "$o" "$ro")
		if is_num "$h0" && is_num "$h1" && _cmp "$h1" '<' "$h0"; then RL="the runner's hp $h0 -> $h1 (shot $k)"; return 0; fi
	done
	RL="the runner's own 2 shots did not reach it (hp $h0 -> $h1)"; return 1
}
# reached <shooter> <runner> <ref> <shooter's copyHits before> <runner's hp before>: the shooter's shot reached the raider: the
# shooter's game took a hit on its copy (copyHits rose), or the raider's health fell in the runner's game. Neither: the bullet met
# something else, and changed nothing in either game (no hit, so no claim and no damage): the spot is the cause, not the mod.
# REACH says what was read.
reached() {
	local c1 h1; sleep 1.5; c1=$(field "$(dc "$1" 'net status')" copyHits); h1=$(hpv "$2" "$3")
	REACH="the shooter's copyHits $4 -> $c1, the runner's hp $5 -> $h1"
	{ is_num "$4" && is_num "$c1" && [ "$c1" -gt "$4" ]; } || { is_num "$5" && is_num "$h1" && _cmp "$h1" '<' "$5"; }
}
# prove_lines <label> <target>: before a kill or pause step, both players' shots must reach its raider: the runner's own
# (runner_reaches), then the shooter's in one pair_shot (its copyHits rises; the runner keeps the raider, the pair being inside
# the takeover cooldown). Where a shot meets something else (2026-11-01: the VM's and the host's shots at raiders 450 u off
# across the lobby never reached them, while the same raiders took the other player's), the raider is moved to another spot
# (re_place) and the proof starts again, up to 3 spots. Nothing is counted until it passes. FAILS as set-up, and the step is not
# run, if no spot gives both lines; a pair that hands the raider over FAILS as that (as the kill step's would).
prove_lines() {
	local l=$1 t=$2 o p ro k c0 c1 why=""
	o=$(tg_o "$t"); p=$(other_of "$o"); ro=$(tg_ro "$t")
	fresh_target "$l" "$t" || return 1
	for k in 1 2 3; do
		if [ $k -gt 1 ] && ! top_up "$t"; then fail "$l: set-up: both players' shots reach the target" "its raider died or was taken while the lines were proved ($why): the step is not run"; return 1; fi
		if ! runner_reaches "$t"; then why=$RL
		else
			c0=$(field "$(dc "$p" 'net status')" copyHits)
			pair_shot "$o" "$p" "$t"; sleep 1.5; c1=$(field "$(dc "$p" 'net status')" copyHits)
			if [ "$(runner "$o" "$ro")" = "${MYID[$p]}" ]; then
				if [ "$PAIR_GAP" -gt 5000 ]; then fail "$l: set-up: both players' shots reach the target" "the line proof's pair came $PAIR_GAP ms apart, past the 5 s cooldown, and the shooter took it over (as designed): the step is not run"
				else fail "$l: the runner kept it through the line proof's pair (inside the takeover cooldown)" "the shooter took it over (pair gap $PAIR_GAP ms; $RL): see both plugin logs for that pair's hits; the step is not run"; fi
				return 1
			fi
			# the pair's runner shot alone drops the health: the shooter's game must have taken the hit on its copy
			if is_num "$c0" && is_num "$c1" && [ "$c1" -gt "$c0" ]; then
				pass "$l: set-up: both players' shots reach the target" "spot $k: $RL; the shooter's copyHits $c0 -> $c1 in a pair $PAIR_GAP ms apart"; return 0
			fi
			why="the shooter's shot in the pair did not reach it (its copyHits $c0 -> $c1)"
		fi
		echo "$l: spot $k: $why" >> "$A_OUT/targets.txt"
		[ $k -lt 3 ] && { re_place "$t" "$why" || break; }
	done
	fail "$l: set-up: both players' shots reach the target" "at none of $k spot(s); the last: $why (targets.txt): the step is not run"; return 1
}
# copy_shot <shooter> <target>: one shot at an NPC the OTHER game runs (read in the shooter's game first: one it runs already is
# no copy hit, and is skipped with a FAIL)
copy_shot() {
	local s=$1 t=$2 run; armed "$s" || return 1; dc "$s" focus >/dev/null; sleep 0.3
	run=$(runner "$s" "$(tg_ref "$t" "$s")")
	if [ "$run" = "${MYID[$s]}" ]; then fail "copy hit on $(tg_o "$t")'s $(tg_ro "$t")" "the shooter ($s) already runs it before the shot: not a copy hit"; FLIPPED[$(tg_ro "$t")]=1; return 1; fi
	fire "$s" "$t"
}
settle() { local t=$1 i; for i in 1 2 3 4 5 6 7 8; do sleep 0.8; sample "$t" settle; done; }
series_rises() {  # series_rises <ref>: "<rises in the owner's game> <rises in the other's>", while neither game has it dead
	awk -v r="$1" '$1==r && $4!="true" && $7!="true" && $3!="" && $6!="" { if (n && $3>po+0.5) ro++; if (n && $6>pp+0.5) rp++; po=$3; pp=$6; n=1 } END{print ro+0, rp+0}' "$SERIES"
}
last_hp() { awk -v r="$1" '$1==r {o=$3; p=$6; od=$4; pd=$7} END{print o, p, od, pd}' "$SERIES"; }
start_hp() { awk -v r="$1" '$1==r && $8=="start" {print $3, $6; exit}' "$SERIES"; }
# agree <label> <target>: health in both games after a settle. Both dead is agreement (a dead actor's number means nothing).
agree() {
	local ro ho hp_ dO dP; ro=$(tg_ro "$2"); read -r ho hp_ dO dP <<<"$(last_hp "$ro")"
	if [ "$dO" = true ] && [ "$dP" = true ]; then pass "$1: dead in both games" "hp now $ho/$hp_ (not compared)"; return; fi
	if [ "$dO" = true ] || [ "$dP" = true ]; then fail "$1: dead in both games or neither" "owner dead=$dO ($ho) other dead=$dP ($hp_)"; return; fi
	if ! is_num "$ho" || ! is_num "$hp_"; then fail "$1: health read in both games" "owner '$ho' other '$hp_'"; return; fi
	if awk -v a="$ho" -v b="$hp_" -v t="$T_HP_DIFF" 'BEGIN{d=a-b; if(d<0)d=-d; exit !(d<=t)}'; then pass "$1: health agrees (within $T_HP_DIFF hp)" "owner $ho other $hp_"; else fail "$1: health agrees (within $T_HP_DIFF hp)" "owner $ho other $hp_"; fi
}
# landed <label> <target>: health fell from the start in both games (or it died). A step whose shots did not land proves nothing: FAIL.
landed() {
	local ro so sp eo ep dO dP; ro=$(tg_ro "$2"); read -r so sp <<<"$(start_hp "$ro")"; read -r eo ep dO dP <<<"$(last_hp "$ro")"
	if is_num "$so" && is_num "$eo" && is_num "$ep" && { [ "$dO" = true ] || awk -v a="$so" -v b="$eo" -v c="$sp" -v d="$ep" 'BEGIN{exit !(b<a && d<c)}'; }; then pass "$1: the shots landed (health fell in both games)" "runner $so -> $eo, copy $sp -> $ep"
	else fail "$1: the shots landed (health fell in both games)" "runner $so -> $eo, copy $sp -> $ep: inconclusive, no shot hit it (aim or line of fire: targets.txt) or one game did not follow"; fi
}

# --- counters -----------------------------------------------------------------------------------------------------------------------
snap() {  # snap <tag>: the health counters of both games
	local g; for g in host vm; do ns $g; eval "C_${1}_$g=\"\${NS[$g]}\""; done
}
cget() { local v="C_${1}_$2"; field "${!v}" "$3"; }   # cget <tag> <game> <key>
delta() { local a b; a=$(cget "$2" "$1" "$3"); b=$(cget "$4" "$1" "$3"); a=$(part "$a" "${5:-1}"); b=$(part "$b" "${5:-1}"); [ -n "$a" ] && [ -n "$b" ] && echo $((b - a)) || echo ""; }
# answered <label> <runner> <shooter> <tag before> <tag after>: the shooter heard an answer to its hit (copyAnswers=<answers>/<refunds>/<hp>)
# and got nothing back (refunds unchanged: neither a takeover's "you run it" nor an "applied" gives health back)
answered() {
	expect_ge "$1: the shooter's hit was answered ($3's copyAnswers)" "$(delta "$3" "$4" copyAnswers "$5" 1)" 1
	expect_zero "$1: nothing given back for it ($3's copyAnswers refunds)" "$(delta "$3" "$4" copyAnswers "$5" 2)"
}

# run_phase <runner>: the runner runs the NPCs, the other player shoots them
run_phase() {
	local o=$1 p t ro rl ow op n before
	p=$(other_of "$o")
	section "$o runs the NPCs, $p shoots: set up"
	enter_with_runner "$o"; pick_targets "$o"
	[ "${#TG[@]}" -lt 1 ] && return

	section "$o runs the NPCs, $p shoots: copy hits take the raider over"
	for rl in round round2; do
		t=$(tg_by $o $rl); [ -z "$t" ] && continue
		# The copy hit's own shot proves the shooter's line: one that meets something else changes nothing in either game, so
		# the raider is moved (re_place) or, the third time, another raider is put in (swap_spare), and it is shot again. Nothing
		# is counted until a shot reaches it; at none of 3 tries it FAILS as set-up and the step is not run.
		local k c0 h0 ok=0
		for k in 1 2 3; do
			ro=$(tg_ro "$t"); snap "${rl}0"; c0=$(cget "${rl}0" "$p" copyHits); h0=$(hpv "$o" "$ro")
			copy_shot "$p" "$t" || { ok=skip; break; }
			reached "$p" "$o" "$ro" "$c0" "$h0" && { ok=1; break; }
			echo "$rl $ro: try $k: the shooter's shot did not reach it ($REACH)" >> "$A_OUT/targets.txt"
			[ $k = 3 ] && break
			if [ $k = 2 ] && swap_spare "$t"; then t=$SWAPPED; else re_place "$t" "the shooter's shot did not reach it" || break; fi
		done
		[ "$ok" = skip ] && continue
		ro=$(tg_ro "$t")
		if [ "$ok" != 1 ]; then fail "$rl, $o's $ro: set-up: the shooter's shot reaches the target" "not in $k tries ($REACH after the last): its line was blocked at each spot (targets.txt): the step is not run"; continue; fi
		pass "$rl, $o's $ro: set-up: the shooter's shot reaches the target" "try $k: $REACH"
		settle "$t"; snap "${rl}1"
		agree "$rl, $o's $ro" "$t"; landed "$rl, $o's $ro" "$t"
		# the claim went ahead of the damage: granted (nobody hit it lately), the shooter runs it now, in both games
		ow=$(runner "$o" "$ro"); op=$(runner "$p" "$ro")
		if [ "$ow" = "${MYID[$p]}" ] && [ "$op" = "${MYID[$p]}" ]; then pass "$rl, $o's $ro: the hit took it over (the shooter runs it in both games)" "player ${MYID[$p]}"
		else fail "$rl, $o's $ro: the hit took it over (the shooter runs it in both games)" "runner says player '$ow', shooter says '$op', shooter is ${MYID[$p]}"; fi
		answered "$rl, $o's $ro" "$o" "$p" "${rl}0" "${rl}1"
		expect_zero "$rl, $o's $ro: the old runner was not sent the damage ($o's damageAnswered)" "$(delta "$o" "${rl}0" damageAnswered "${rl}1" 1)"
	done

	section "$o runs the NPCs, $p shoots: kill (the runner keeps it, the shooter's hits go to it as damage)"
	t=$(tg_by $o kill)
	if [ -n "$t" ] && armed "$o" && armed "$p" && ro=$(tg_ro "$t") && prove_lines "kill ($o's NPC $ro)" "$t"; then
		# A pair whose shooter click came over T_PAIR_GAP_MS after the runner's (a game's main thread hitched in between) is
		# late: its hits still count while the runner keeps the raider, the gap check covers the pairs in time, and the late
		# ones are reported; more than 2 late pairs, or a late pair past the 5 s cooldown that let the shooter take it over (as
		# designed), FAIL as set-up.
		local gap=0 inTime=0 late="" handed="" hb ha; n=0; snap K0
		while [ $n -lt 30 ]; do   # a T_TOUGH_HP raider takes several hits
			[ "$(deadv "$o" "$ro")" = true ] && break
			hb=$(hpv "$o" "$ro"); pair_shot "$o" "$p" "$t"; n=$((n + 2))
			if [ "$PAIR_GAP" -gt "$T_PAIR_GAP_MS" ]; then late="$late $PAIR_GAP"; else inTime=$((inTime + 1)); [ "$PAIR_GAP" -gt "$gap" ] && gap=$PAIR_GAP; fi
			# the shooter running it now: that pair's hit took it over, so the runner's own claim had not come first
			[ "$(runner "$o" "$ro")" = "${MYID[$p]}" ] && { ha=$(hpv "$o" "$ro"); handed="after $n shots (pair gap $PAIR_GAP ms; health in the runner's game $hb -> $ha over that pair)"; break; }
		done
		sleep 4; sample "$t" killed; snap K1
		if [ $inTime -gt 0 ]; then expect_le "kill ($o's NPC $ro): each shooter's click came soon after the runner's (ms, the most of $inTime pair(s) in time; the takeover cooldown is 5000)" "$gap" "$T_PAIR_GAP_MS"
		else fail "kill ($o's NPC $ro): set-up: pairs in time (the shooter's click at most $T_PAIR_GAP_MS ms after the runner's)" "none:$late ms (the games' main threads hitched; targets.txt has each pair's timings)"; fi
		if [ -n "$late" ]; then
			if [ "$(wc -w <<<"$late")" -gt 2 ]; then fail "kill ($o's NPC $ro): set-up: at most 2 late pairs" "$(wc -w <<<"$late") pairs late:$late ms (the games' main threads hitched; targets.txt)"
			else warn "kill ($o's NPC $ro): late pairs (over $T_PAIR_GAP_MS ms; their hits counted, the runner kept it)" "$late ms"; fi
		fi
		if [ -n "$handed" ] && [ "$PAIR_GAP" -gt 5000 ]; then
			fail "kill ($o's NPC $ro): set-up: the pairs inside the takeover cooldown" "the shooter took it over $handed: its click came past the 5 s cooldown, so the takeover was right; the damage checks are not run"
		elif [ -n "$handed" ]; then
			# no routed damage to look for once the shooter runs it. The runner's line was shown clear (prove_lines), so this is
			# either that pair's runner shot missing after all or the claim not holding: the log has which
			fail "kill ($o's NPC $ro): the runner kept it while both shot (each pair inside the takeover cooldown)" "the shooter took it over $handed: see both plugin logs for that pair's hits; the damage checks are not run"
		else
			landed "kill ($o's NPC $ro)" "$t"
			expect_eq "kill ($o's NPC $ro): dead in the runner's game after $n shots" "$(deadv "$o" "$ro")" true
			expect_eq "kill ($o's NPC $ro): dead in the other game" "$(deadv "$p" "$ro")" true
			expect_ge "kill ($o's NPC $ro): the runner applied the shooter's hits ($o's damageAnswered)" "$(delta "$o" K0 damageAnswered K1 1)" 1
			expect_zero "kill ($o's NPC $ro): nothing given back to the shooter ($p's copyAnswers refunds)" "$(delta "$p" K0 copyAnswers K1 2)"
		fi
	fi

	section "$o runs the NPCs, $p shoots: a hit while the runner is in the pause menu"
	t=$(tg_by $o pause)
	if [ -n "$t" ] && armed "$o" && armed "$p" && ro=$(tg_ro "$t") && prove_lines "pause ($o runs the NPC)" "$t"; then
		local menu_ms pm try
		# The runner's own shot keeps the raider (its claim starts the server's 5 s takeover cooldown), the menu opens, and the
		# shooter fires at once: well inside the cooldown. The menu closes about 1 s later, well before the paused game's hand-off
		# (PauseHandOff::AFTER, 5 s): nothing is read while it is open (the reads took it past 5 s, 2026-11-01). A pair that came
		# late (a game's main thread hitched) is fired again once, the raider's health put back first, if the runner still has it.
		for try in 1 2; do
			snap P0
			pair_shot "$o" "$p" "$t" esc
			PAUSE_WHO=$o   # put_back closes it if the run stops before this step does
			pm=$(field "$(dc "$o" paused)" pauseMenu)   # one read while it is open
			sleep 0.5; dc "$o" "key 0x1B 100" >/dev/null; menu_ms=$(( $(date +%s%3N) - PAIR_ESC )); sleep 1.5
			[ "$(field "$(dc "$o" paused)" pauseMenu)" = true ] && { dc "$o" "key 0x1B 100" >/dev/null; menu_ms=$(( $(date +%s%3N) - PAIR_ESC )); sleep 1; }
			PAUSE_WHO=""
			[ $try = 1 ] && [ "$PAIR_GAP" -gt "$T_PAIR_GAP_MS" ] && [ "$(runner "$o" "$ro")" = "${MYID[$o]}" ] && top_up "$t" || break
			echo "pause $ro: the pair came $PAIR_GAP ms apart (over $T_PAIR_GAP_MS): fired again" >> "$A_OUT/targets.txt"; sleep 2
		done
		expect_le "pause ($o runs the NPC): the shooter's click came soon after the runner's (ms; the takeover cooldown is 5000)" "$PAIR_GAP" "$T_PAIR_GAP_MS"
		expect_le "pause ($o runs the NPC): the menu was closed before the paused game's hand-off (ms open; PauseHandOff is 5000)" "$menu_ms" 4500
		fired "$o" "$(cut -d' ' -f1 <<<"$PAIR_SHOTS")" "$(shots_of "$o")"; fired "$p" "$(cut -d' ' -f2 <<<"$PAIR_SHOTS")" "$(shots_of "$p")"
		settle "$t"; sleep 3; sample "$t" afterpause; snap P1
		# the runner's game counts a friend's hit it applied with the pause menu open (damageAnswered=<applied>/<refused>/<in pause>):
		# the proof the hit landed while the menu was up, not just that the menu was up by the time of the read (pm). Read from P1:
		# it used to be read before P1 was taken (2026-11-01: against no P1 at all, "no number to compare (got '')")
		expect_ge "pause ($o runs the NPC): the shooter's hit was applied while the runner's pause menu was open ($o's damageAnswered in pause; paused read pauseMenu=$pm)" "$(delta "$o" P0 damageAnswered P1 3)" 1
		agree "pause ($o runs the NPC): after the menu closes" "$t"; landed "pause ($o runs the NPC)" "$t"
		local k1 k2; k1=$(runner "$o" "$ro"); k2=$(runner "$p" "$(tg_rp "$t")")
		if [ "$k1" = "${MYID[$o]}" ] && [ "$k2" = "${MYID[$o]}" ]; then pass "pause ($o runs the NPC): the runner kept it (its own hit just before was within the takeover cooldown)" "player $k1"; else fail "pause ($o runs the NPC): the runner kept it (its own hit just before was within the takeover cooldown)" "runner says player '$k1', shooter says '$k2', runner is ${MYID[$o]}"; fi
		expect_ge "pause ($o runs the NPC): the runner applied the shooter's hit sent while it was in the menu ($o's damageAnswered)" "$(delta "$o" P0 damageAnswered P1 1)" 1
		answered "pause ($o runs the NPC)" "$o" "$p" P0 P1
	fi

	put_back
	ALL_TG+=("${TG[@]}")
}
# put_back: what the run changed in the running games is undone: the pause menu closed if the pause step left it open, the held
# raiders let go, and the raised health put back. At the end of each phase, and on every exit (the EXIT trap: an interrupt, the
# circuit breaker, a normal end), so a run that stops part-way leaves no raider held or at T_TOUGH_HP. The raised health stays in
# both running games (a console setav, nobody's session state): left, a later run on the same games (smoke's health compare,
# 2026-11-01) finds 120 in one and 30 in a game that restarted since. The damage is taken off first: a raider left at 1 of 120
# put back to a base of 30 sits at -89, and the copy died of it in one game while the runner's stayed up (2026-11-01: smoke then
# found it dead in the VM only). Each game gets its own base back (baseHp, read before the raise; hp would set a hurt raider's
# base to what it had left). Each change is undone once; a game that does not answer is skipped (a broken rig: each call would
# wait out its timeouts).
put_back() {
	local r k g up=""
	[ -n "$PAUSE_WHO${HELD[*]}${!TOUGH_BASE[*]}" ] || return 0
	for g in host vm; do [ -n "$(dc $g status)" ] && up="$up $g"; done
	sender() { case " $up " in *" $1 "*) dc "$@";; esac; }
	for g in host vm; do case " $up " in *" $g "*) ;; *) echo "put_back: $g did not answer: what this run held or raised there is left (restart that game)";; esac; done
	if [ -n "$PAUSE_WHO" ]; then [ "$(field "$(sender "$PAUSE_WHO" paused)" pauseMenu)" = true ] && sender "$PAUSE_WHO" "key 0x1B 100" >/dev/null; PAUSE_WHO=""; fi
	for r in "${HELD[@]}"; do sender host "console $r.setrestrained 0" >/dev/null; sender vm "console $r.setrestrained 0" >/dev/null; done; HELD=()
	[ "${#TOUGH_BASE[@]}" -gt 0 ] || return 0
	for k in "${!TOUGH_BASE[@]}"; do sender "${k%%:*}" "console ${k#*:}.restoreav health 1000" >/dev/null; done
	sleep 1
	for k in "${!TOUGH_BASE[@]}"; do
		g=${k%%:*}; r=${k#*:}
		is_num "${TOUGH_BASE[$k]}" && sender "$g" "console $r.setav health ${TOUGH_BASE[$k]}" >/dev/null
	done
	TOUGH_BASE=()
}

# ============================================================ run ============================================================
section "preflight"
ALL_TG=()
rig_up "both games up"
crash_snapshot; log_mark host; log_mark vm
dll_preflight
for g in host vm; do dc $g "console player.additem 00004822 1" >/dev/null; dc $g "console player.additem 0001F276 400" >/dev/null; dc $g "console player.equipitem 00004822" >/dev/null; dc $g "draw on" >/dev/null; done
sleep 2; snap A
for g in host vm; do
	ns $g; MYID[$g]=$(nsf $g id)
	expect_ge "set up: $g has 10mm ammo for the pistol (count read before shooting)" "$(ammo_of $g)" 20
	expect_match "set up: $g has the pistol equipped" "$(dc $g 'equipped 00000014' | cut -c1-80)" '00004822'
done
pass "baseline counters taken" "copyHits host $(cget A host copyHits) vm $(cget A vm copyHits); copyUnseen $(cget A host copyUnseen)/$(cget A vm copyUnseen)"
# Every step needs both games' clicks: a game whose mouse never came up (2026-11-01: the VM, started by in-vm-3-game.ps1, fired
# nothing for the whole run, 18 failures that proved nothing) is stopped here, before ten minutes of misses.
# (assert.sh's circuit breaker: two rig-broken failures end the run; one game that can't fire ruins every step, so it ends here)
dead=""; for g in host vm; do rig_click_fires $g "preflight: $g's click fires its gun" || dead="$dead $g"; done
for g in host vm; do rig_keys_move $g "preflight: $g's keys move its player"; done
[ -n "$dead" ] && [ -z "$A_RIG_STOPPED" ] && _rig_stop input

run_phase host
run_phase vm

section "no heals, counters settled"
sleep 4; snap C
# A rise in the runner's game is a real heal there (a raider drinks a stimpak at 1 hp: the design allows it, HEALTH-SYNC.md) and is
# reported; a rise only in a copy is the heal-back bug (the 0.10.1 fix) and fails.
copyonly=0; healed=""; copyshown=""
for t in "${ALL_TG[@]}"; do
	ro=$(tg_ro "$t"); read -r a b <<<"$(series_rises "$ro")"
	[ "$a" -gt 0 ] && healed="$healed $ro(x$a)"
	if [ "$b" -gt "$a" ]; then copyonly=$((copyonly + b - a)); copyshown="$copyshown $ro"; fi
done
expect_zero "no copy's health rose without the runner's rising (a heal-back)" "$copyonly"; [ -n "$copyshown" ] && echo "copy-only rises:$copyshown"
[ -n "$healed" ] && warn "NPCs healed in the runner's game (real heals, both games followed)" "$healed"
for g in host vm; do
	expect_zero "copyOwed settled ($g)" "$(cget C $g copyOwed)"
	expect_zero "copyUnseen unchanged ($g)" "$(delta $g A copyUnseen C)"
	expect_zero "session's ignored hits unchanged ($g)" "$(delta $g A ignored C)"
	expect_zero "copyLost unchanged ($g)" "$(delta $g A copyLost C)"
	expect_ge "copy hits taken ($g shot the other game's NPCs)" "$(delta $g A copyHits C)" 1
	rr=$(delta $g A copyRestores C 1); [ -n "$rr" ] && [ "$rr" -gt 0 ] && warn "give-backs of a copy's health ($g)" "copyRestores rose by $rr (a drop with no hit of ours behind it given back: see the log for the last hit)"
done
[ "${#FLIPPED[@]}" -gt 0 ] && echo "NPCs the shooter ran before its copy shot: ${!FLIPPED[*]}"

section "end checks"
rig_up "both games up"
crash_verdict "hits"
for g in host vm; do log_check $g "$g"; log_privacy $g "$g"; done
a_summary
