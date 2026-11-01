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
#   end         no living NPC's health rose in a copy alone (a heal-back: the 0.10.1 bug); copyOwed = 0 on both games;
#               copyUnseen, the session's ignored hits and copyLost unchanged; give-backs and real heals are reported.
#   Targets must be alive in both games when the shooting starts (a raider the session has dead can read alive in `actors`).
#   A click that fires nothing (the game's own shots counter did not rise) FAILS once per game: its misses are not the mod's.
#   A step whose shot did not land (health unchanged in both games) is inconclusive and FAILS; the spots and positions are in
#   targets.txt. Health is the current value (hp=), never the base. damageHeld is not asserted: a menu does not hold damage
#   (WorldSync::InWorld is false only on a loading screen), so it moves only for a runner on a loading screen or an NPC not
#   loaded there (tests/DamageHoldTests.cpp).
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
# spot_for <shooter> <other> "<x,y x,y ...>" <both 1|0>: "x y z degrees" for a raider 250 u from the shooter, the first of a few headings off
# its own with a clear line from the shooter (and, with both=1, from the other player too: neither within 50 u of the other's line to it, ahead of them), 150 u or more from the
# other player, and is clear of the spots already taken (110 u apart, out of every line to them). Empty if none is.
# Both coc'd to the same marker, so the other player often stands beside the shooter: a raider behind them takes no shot (the
# 0.10.1 runs' kill step fired 16 shots that hit a barrel and a stand-in, never the raider).
spot_for() {
	local ps po; ps=$(dc "$1" pos); po=$(dc "$2" pos)
	awk -v px="$(field "$ps" x)" -v py="$(field "$ps" y)" -v pz="$(field "$ps" z)" -v yaw="$(field "$ps" rz)" -v ox="$(field "$po" x)" -v oy="$(field "$po" y)" -v taken="$3" -v both="$4" '
	# blk: q stands in the line of fire from a to b (more than 30 u ahead of a, short of b, within 50 u of the line)
	function blk(ax, ay, bx, by, qx, qy,   dx, dy, l, s, d) { dx = bx - ax; dy = by - ay; l = sqrt(dx * dx + dy * dy); if (l == 0) return 0; s = ((qx - ax) * dx + (qy - ay) * dy) / l; d = ((qx - ax) * dy - (qy - ay) * dx) / l; if (d < 0) d = -d; return s > 30 && s < l - 20 && d < 50 }
	BEGIN {
		if (px == "" || ox == "") exit
		n = split(taken, T, " "); nc = split("0 -15 15 -30 30 -45 45 -60 60 -90 90 -120 120", C, " ")
		for (i = 1; i <= nc; i++) {
			a = (yaw + C[i]) * 3.14159265 / 180; nx = px + 250 * sin(a); ny = py + 250 * cos(a); ok = 1
			if (blk(px, py, nx, ny, ox, oy) || (both && blk(ox, oy, nx, ny, px, py)) || sqrt((nx - ox) ^ 2 + (ny - oy) ^ 2) < 150) ok = 0
			for (j = 1; j <= n && ok; j++) { split(T[j], q, ",")
				if (sqrt((nx - q[1]) ^ 2 + (ny - q[2]) ^ 2) < 110 || blk(px, py, nx, ny, q[1], q[2]) || (both && blk(ox, oy, nx, ny, q[1], q[2])) || blk(px, py, q[1], q[2], nx, ny) || blk(ox, oy, q[1], q[2], nx, ny)) ok = 0 }
			if (ok) { printf "%.1f %.1f %.1f %d\n", nx, ny, pz, C[i]; exit }
		}
	}'
}
# put_at <runner> <ref> <x> <y> <z>: the runner's console moves the NPC there (never a player) and holds it still
put_at() {
	dc "$1" "console $2.setrestrained 1" >/dev/null
	dc "$1" "console $2.setpos x $3" >/dev/null; dc "$1" "console $2.setpos y $4" >/dev/null; dc "$1" "console $2.setpos z $5" >/dev/null
}
# pick_targets <runner>: four living raiders that both games say the runner runs, each put at its own clear spot near the shooter
# unhurt <who> <ref>: alive in that game, with at least T_TARGET_HP health (a raider an earlier run hurt can die of the copy hit:
# a kill leaves no runner and no "you run it" answer to check)
T_TARGET_HP=${T_TARGET_HP:-25}
unhurt() { local i; i=$(rinfo "$1" "$2"); [ "$(field "$i" dead)" = false ] && is_num "$(field "$i" hp)" && awk -v h="$(field "$i" hp)" -v m="$T_TARGET_HP" 'BEGIN{exit !(h >= m)}'; }
pick_targets() {
	local r=$1 s a k=0 ro rv sp taken="" x y z off; s=$(other_of "$r"); TG=()
	echo "phase $r runs: shooter $s at $(dc "$s" pos | cut -d' ' -f2-5), $r at $(dc "$r" pos | cut -d' ' -f2-5)" >> "$A_OUT/targets.txt"
	for a in $(host 'actors 3000' | tr ';' '\n' | grep "Raider' dead=false" | grep -o "[0-9A-F]\{8\}"); do
		[ $k -ge 4 ] && break
		case " ${ALL_TG[*]} " in *":$a:"*) continue;; esac   # not one an earlier phase already shot
		unhurt host "$a" && unhurt vm "$a" || continue   # the actors list can call one alive that a game has dead; a hurt one can die of one hit
		ro=$(runner host "$a"); rv=$(runner vm "$a")
		if [ "$ro" = "${MYID[$r]}" ] && [ "$rv" = "${MYID[$r]}" ]; then
			sp=$(spot_for "$s" "$r" "$taken" "$([ $k -lt 2 ] && echo 1 || echo 0)"); [ -z "$sp" ] && break
			read -r x y z off <<<"$sp"; put_at "$r" "$a" "$x" "$y" "$z"; taken="$taken $x,$y"
			TG+=("$r:$a:$a:${ROLES[$k]}"); echo "target $r:$a role ${ROLES[$k]} at $x $y ($off degrees off the shooter's heading)" >> "$A_OUT/targets.txt"; k=$((k + 1))
		fi
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
		NOFIRE[$1]=1; fail "$1's click fired its gun (its player's shots counter rose)" "shots=$2 before and $3 after: the click did not reach the game (its later misses are this, not the mod)"
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
T_PAIR_GAP_MS=${T_PAIR_GAP_MS:-3000}
pair_shot() {
	local o=$1 p=$2 t=$3 a b so sp
	so=$(shots_of "$o"); sp=$(shots_of "$p")
	dc "$o" "turnto $(tg_ref "$t" "$o") 300" >/dev/null; dc "$p" "turnto $(tg_ref "$t" "$p") 300" >/dev/null
	dc "$o" focus >/dev/null; dc "$p" focus >/dev/null; sleep 0.5
	a=$(date +%s%3N); dc "$o" "click 80" >/dev/null; sleep 0.4
	[ "$4" = esc ] && dc "$o" "key 0x1B 100" >/dev/null
	dc "$p" "click 80" >/dev/null; b=$(date +%s%3N)
	PAIR_GAP=$((b - a)); sleep 0.6; sample "$t" "pair-$o-$p"
	fired "$o" "$so" "$(shots_of "$o")"; fired "$p" "$sp" "$(shots_of "$p")"
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
		t=$(tg_by $o $rl); [ -z "$t" ] && continue; ro=$(tg_ro "$t")
		snap "${rl}0"; copy_shot "$p" "$t" || continue; settle "$t"; snap "${rl}1"
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
	if [ -n "$t" ] && armed "$o" && armed "$p"; then
		local gap=0; ro=$(tg_ro "$t"); n=0; snap K0
		while [ $n -lt 8 ]; do
			[ "$(deadv "$o" "$ro")" = true ] && break
			pair_shot "$o" "$p" "$t"; n=$((n + 2)); [ "$PAIR_GAP" -gt "$gap" ] && gap=$PAIR_GAP
		done
		sleep 4; sample "$t" killed; snap K1
		expect_le "kill ($o's NPC $ro): each shooter's click came soon after the runner's (ms, the most; the takeover cooldown is 5000)" "$gap" "$T_PAIR_GAP_MS"
		landed "kill ($o's NPC $ro)" "$t"
		expect_eq "kill ($o's NPC $ro): dead in the runner's game after $n shots" "$(deadv "$o" "$ro")" true
		expect_eq "kill ($o's NPC $ro): dead in the other game" "$(deadv "$p" "$ro")" true
		expect_ge "kill ($o's NPC $ro): the runner applied the shooter's hits ($o's damageAnswered)" "$(delta "$o" K0 damageAnswered K1 1)" 1
		expect_zero "kill ($o's NPC $ro): nothing given back to the shooter ($p's copyAnswers refunds)" "$(delta "$p" K0 copyAnswers K1 2)"
	fi

	section "$o runs the NPCs, $p shoots: a hit while the runner is in the pause menu"
	t=$(tg_by $o pause)
	if [ -n "$t" ] && armed "$o" && armed "$p"; then
		ro=$(tg_ro "$t"); snap P0
		# The runner's own shot keeps the raider (its claim starts the server's 5 s takeover cooldown), the menu opens, and the
		# shooter fires at once: well inside the cooldown, and before the paused game's hand-off (PauseHandOff::AFTER, 5 s).
		pair_shot "$o" "$p" "$t" esc
		expect_le "pause ($o runs the NPC): the shooter's click came soon after the runner's (ms; the takeover cooldown is 5000)" "$PAIR_GAP" "$T_PAIR_GAP_MS"
		if [ "$(field "$(dc "$o" paused)" pauseMenu)" = true ]; then pass "pause ($o runs the NPC): the pause menu was open for the shot" "pauseMenu=true"; else fail "pause ($o runs the NPC): the pause menu was open for the shot" "paused says pauseMenu=$(field "$(dc "$o" paused)" pauseMenu)"; fi
		sleep 2.5; sample "$t" paused
		dc "$o" "key 0x1B 100" >/dev/null; sleep 1.5
		[ "$(field "$(dc "$o" paused)" pauseMenu)" = true ] && { dc "$o" "key 0x1B 100" >/dev/null; sleep 1; }
		settle "$t"; sleep 3; sample "$t" afterpause; snap P1
		agree "pause ($o runs the NPC): after the menu closes" "$t"; landed "pause ($o runs the NPC)" "$t"
		local k1 k2; k1=$(runner "$o" "$ro"); k2=$(runner "$p" "$(tg_rp "$t")")
		if [ "$k1" = "${MYID[$o]}" ] && [ "$k2" = "${MYID[$o]}" ]; then pass "pause ($o runs the NPC): the runner kept it (its own hit just before was within the takeover cooldown)" "player $k1"; else fail "pause ($o runs the NPC): the runner kept it (its own hit just before was within the takeover cooldown)" "runner says player '$k1', shooter says '$k2', runner is ${MYID[$o]}"; fi
		expect_ge "pause ($o runs the NPC): the runner applied the shooter's hit sent while it was in the menu ($o's damageAnswered)" "$(delta "$o" P0 damageAnswered P1 1)" 1
		answered "pause ($o runs the NPC)" "$o" "$p" P0 P1
	fi

	for t in "${TG[@]}"; do dc "$o" "console $(tg_ro "$t").setrestrained 0" >/dev/null; dc "$p" "console $(tg_ro "$t").setrestrained 0" >/dev/null; done
	ALL_TG+=("${TG[@]}")
}

# ============================================================ run ============================================================
section "preflight"
ALL_TG=()
expect_match "both games up" "$(alive)" '^host=ingame=true vm=ingame=true$'
crash_snapshot; log_mark host; log_mark vm
for g in host vm; do dc $g "console player.additem 00004822 1" >/dev/null; dc $g "console player.additem 0001F276 400" >/dev/null; dc $g "console player.equipitem 00004822" >/dev/null; dc $g "draw on" >/dev/null; done
sleep 2; snap A
for g in host vm; do
	ns $g; MYID[$g]=$(nsf $g id)
	expect_ge "set up: $g has 10mm ammo for the pistol (count read before shooting)" "$(ammo_of $g)" 20
	expect_match "set up: $g has the pistol equipped" "$(dc $g 'equipped 00000014' | cut -c1-80)" '00004822'
done
pass "baseline counters taken" "copyHits host $(cget A host copyHits) vm $(cget A vm copyHits); copyUnseen $(cget A host copyUnseen)/$(cget A vm copyUnseen)"

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
expect_match "both games up" "$(alive)" '^host=ingame=true vm=ingame=true$'
crash_verdict "hits"
for g in host vm; do log_check $g "$g"; log_privacy $g "$g"; done
a_summary
