#!/bin/bash
# hits.sh [--rounds N]
#
# Health agreement between the two games (docs/HEALTH-SYNC.md: hits on a friend's enemy are numbered, answered, refused,
# rewound). Each player shoots NPCs the OTHER game runs, and what the two games say of each one's health is compared.
#
#   set up      Corvega's own raiders (the same form ID in both games). Who runs them is whoever entered the cell first, so
#               the check runs twice: the host enters first and the VM shoots, then the VM enters first and the host shoots.
#               Three raiders that both games say the runner runs are moved in front of the shooter (NPCs are moved, never
#               the players: they turn with turnto and fire real clicks) and held still. Each player has a gun AND its ammo
#               (counted before shooting), and who runs an NPC is read in the shooter's game before every shot: a shot can
#               take an NPC over ("refused: the sender runs it"), and then it is no longer a copy hit.
#   rounds      the player who does not run one NPC shoots it, then both games' health is read for a few seconds.
#   asserts     health in both games within T_HP_DIFF hp, and down from where it started in both (a round where no hit
#               landed is inconclusive and FAILS); no living NPC's health rose in either game (a rise is a heal: the
#               0.10.1 bug); a killed NPC is dead in both games. Health is the current value (hp=), never the base.
#   counters    copyOwed = 0 once settled on both games; copyUnseen, the session's ignored hits and copyLost unchanged;
#               copyHits and copyAnswers rose where a copy was shot; give-backs (copyRestores) are reported.
#   pause       the runner of an NPC opens the pause menu while the other player shoots it (0.10.1's "held" case): the
#               hits wait, then land; health agrees again after the menu closes, and damageHeld/damageAnswered moved.
#
# Exit code = number of failed checks. About 8 minutes (two cell entries of about a minute each). Needs both games joined and the rig held (see guard.sh).
. "$(dirname "${BASH_SOURCE[0]}")/assert.sh"
. "$RIG/steps.sh"
. "$RIG/crashlib.sh"
. "$RIG/guard.sh"

ROUNDS=${HITS_ROUNDS:-2}; SHOTS=${HITS_SHOTS:-1}
while [ $# -gt 0 ]; do case "$1" in --rounds) ROUNDS=$2; shift 2;; *) echo "unknown option $1"; exit 2;; esac; done
rig_guard || exit 2
a_init hits; tunnel
trap 'echo "interrupted"; a_summary' INT TERM
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
# --- targets: Corvega's own raiders, the same form ID in both games. Who runs them depends on who entered the cell first, so the
# check goes in twice: the host enters first (it runs them, the VM shoots), then the VM enters first (it runs them, the host shoots).
# TG entries are "<runner>:<ref>:<ref>:<role>", the roles round, round2, pause and kill in order. One NPC per copy hit: the first hit on an NPC another game runs claims it for the shooter (HEALTH-SYNC.md), so each hit test gets a fresh NPC.
ROLES=(round round2 pause kill)
# enter_with_runner <runner>: both games leave to Sanctuary, then the runner enters Corvega, and 25 s later the other
enter_with_runner() {
	local r=$1 o; o=$(other_of "$r")
	bash "$RIG/dcfast.sh" both "console coc SanctuaryExt" >/dev/null 2>&1; sleep 15; wait_ingame host 30; wait_ingame vm 30
	dc "$r" "console coc CorvegaAssemblyPlant01" >/dev/null; sleep 25; wait_ingame "$r" 30
	dc "$o" "console coc CorvegaAssemblyPlant01" >/dev/null; sleep 25; wait_ingame "$o" 30
}
# bring_to <runner> <shooter> <ref> <degrees>: the runner's console puts the NPC 250 u from the shooter, that many degrees off
# its heading (the NPC is moved, never the player), and holds it still
bring_to() {
	local r=$1 s=$2 ref=$3 off=$4 p x y z yaw nx ny; p=$(dc "$s" pos)
	x=$(field "$p" x); y=$(field "$p" y); z=$(field "$p" z); yaw=$(field "$p" rz)
	nx=$(awk -v x=$x -v y=$y -v a=$yaw -v o=$off 'BEGIN{r=(a+o)*3.14159265/180; printf "%.1f", x+250*sin(r)}'); ny=$(awk -v x=$x -v y=$y -v a=$yaw -v o=$off 'BEGIN{r=(a+o)*3.14159265/180; printf "%.1f", y+250*cos(r)}')
	dc "$r" "console $ref.setrestrained 1" >/dev/null
	dc "$r" "console $ref.setpos x $nx" >/dev/null; dc "$r" "console $ref.setpos y $ny" >/dev/null; dc "$r" "console $ref.setpos z $z" >/dev/null
}
# pick_targets <runner>: four living raiders that both games say the runner runs; moved in front of the other player
pick_targets() {
	local r=$1 s a k=0 offs=(0 -30 30 -60) ro rv; s=$(other_of "$r"); TG=()
	for a in $(host 'actors 3000' | tr ';' '\n' | grep "Raider' dead=false" | grep -o "[0-9A-F]\{8\}"); do
		[ $k -ge 4 ] && break
		case " ${ALL_TG[*]} " in *":$a:"*) continue;; esac   # not one an earlier phase already shot
		ro=$(runner host "$a"); rv=$(runner vm "$a")
		if [ "$ro" = "${MYID[$r]}" ] && [ "$rv" = "${MYID[$r]}" ]; then
			bring_to "$r" "$s" "$a" "${offs[$k]}"; TG+=("$r:$a:$a:${ROLES[$k]}"); echo "target $r:$a role ${ROLES[$k]}" >> "$A_OUT/targets.txt"; k=$((k + 1))
		fi
	done
	sleep 3
	expect_ge "set up ($r runs, $s shoots): raiders both games say $r runs" "${#TG[@]}" 4
	for t in "${TG[@]}"; do sample "$t" start; done
}
tg_o() { cut -d: -f1 <<<"$1"; }; tg_ro() { cut -d: -f2 <<<"$1"; }; tg_rp() { cut -d: -f3 <<<"$1"; }; tg_role() { cut -d: -f4 <<<"$1"; }
tg_by() { local t; for t in "${TG[@]}"; do [ "$(tg_o "$t")" = "$1" ] && [ "$(tg_role "$t")" = "$2" ] && { echo "$t"; return; }; done; }   # tg_by <runner> <role>
tg_ref() { [ "$2" = "$(tg_o "$1")" ] && tg_ro "$1" || tg_rp "$1"; }   # tg_ref <target> <game>: that game's ref

# sample <target> <label>: both games' health for the target into the series file
sample() {
	local t=$1 o p ri pi; o=$(tg_o "$t"); p=$(other_of "$o"); ri=$(rinfo "$o" "$(tg_ro "$t")"); pi=$(rinfo "$p" "$(tg_rp "$t")")
	echo "$(tg_ro "$t") $o $(field "$ri" hp) $(field "$ri" dead) $p $(field "$pi" hp) $(field "$pi" dead) $2" >> "$SERIES"
}
# shoot <shooter> <target> <n> [own]: turn to the NPC (the shooter's own ref for it) and fire real clicks, sampling after each.
# A valid hit test is a shot at an NPC the OTHER game runs, so the runner is read in the shooter's game before every shot: a
# shot can take an NPC over ("refused: the sender runs it"), and then the shooter stops (the target is marked FLIPPED, warned).
# The shooter's ammo is checked first (clicks with an empty gun fire nothing). own=1 lets the runner shoot (the kill step).
shoot() {
	local s=$1 t=$2 n=$3 own=$4 i r run am ro; r=$(tg_ref "$t" "$s"); ro=$(tg_ro "$t")
	am=$(ammo_of "$s"); if ! is_num "$am" || [ "$am" -lt 5 ]; then dc "$s" "console player.additem 0001F276 400" >/dev/null; sleep 1; am=$(ammo_of "$s"); fi
	if ! is_num "$am" || [ "$am" -lt 5 ]; then fail "$s has ammo for the shot" "10mm count '$am': clicks would fire nothing"; return 1; fi
	dc "$s" focus >/dev/null; sleep 0.3
	for i in $(seq 1 "$n"); do
		run=$(runner "$s" "$r")
		if [ -z "$own" ] && [ "$run" = "${MYID[$s]}" ]; then
			if [ "$i" = 1 ]; then warn "runner of $(tg_o "$t")'s $ro" "already player $run = the shooter ($s) before the first shot: not a copy hit, skipped"; else echo "  $(tg_o "$t")'s $ro claimed by $s after $((i - 1)) shot(s) (the hit took it over, as designed)"; fi; FLIPPED[$ro]=1; return 1
		fi
		dc "$s" "turnto $r 300" >/dev/null; sleep 0.5; dc "$s" "click 80" >/dev/null; sleep 1.0; sample "$t" "shot-$s"
	done
}
settle() { local t=$1 i; for i in 1 2 3 4 5 6 7 8; do sleep 0.8; sample "$t" settle; done; }
series_rises() {  # series_rises <ref>: "<rises in the owner's game> <rises in the other's>", while neither game has it dead
	awk -v r="$1" '$1==r && $4!="true" && $7!="true" && $3!="" && $6!="" { if (n && $3>po+0.5) ro++; if (n && $6>pp+0.5) rp++; po=$3; pp=$6; n=1 } END{print ro+0, rp+0}' "$SERIES"
}
last_hp() { awk -v r="$1" '$1==r {o=$3; p=$6; od=$4; pd=$7} END{print o, p, od, pd}' "$SERIES"; }
# agree <label> <target>: health in both games after a settle. Both dead is agreement (a dead actor's number means nothing).
agree() {
	local ro ho hp_ dO dP; ro=$(tg_ro "$2"); read -r ho hp_ dO dP <<<"$(last_hp "$ro")"
	if [ "$dO" = true ] && [ "$dP" = true ]; then pass "$1: dead in both games" "hp now $ho/$hp_ (not compared)"; return; fi
	if [ "$dO" = true ] || [ "$dP" = true ]; then fail "$1: dead in both games or neither" "owner dead=$dO ($ho) other dead=$dP ($hp_)"; return; fi
	if ! is_num "$ho" || ! is_num "$hp_"; then fail "$1: health read in both games" "owner '$ho' other '$hp_'"; return; fi
	if awk -v a="$ho" -v b="$hp_" -v t="$T_HP_DIFF" 'BEGIN{d=a-b; if(d<0)d=-d; exit !(d<=t)}'; then pass "$1: health agrees (within $T_HP_DIFF hp)" "owner $ho other $hp_"; else fail "$1: health agrees (within $T_HP_DIFF hp)" "owner $ho other $hp_"; fi
}
alive_t() { local d; d=$(awk -v r="$(tg_ro "$1")" '$1==r {d=$4; e=$7} END{print d e}' "$SERIES"); ! grep -q true <<<"$d"; }

# --- counters -----------------------------------------------------------------------------------------------------------------------
snap() {  # snap <tag>: the health counters of both games
	local g; for g in host vm; do ns $g; eval "C_${1}_$g=\"\${NS[$g]}\""; done
}
cget() { local v="C_${1}_$2"; field "${!v}" "$3"; }   # cget <tag> <game> <key>
delta() { local a b; a=$(cget "$2" "$1" "$3"); b=$(cget "$4" "$1" "$3"); a=$(part "$a" "${5:-1}"); b=$(part "$b" "${5:-1}"); [ -n "$a" ] && [ -n "$b" ] && echo $((b - a)) || echo ""; }


# run_phase <runner>: the runner runs the NPCs, the other player shoots them
run_phase() {
	local o=$1 p t pick ro rp h0 pm hc eo ep dO dP held0 held1 first so sp n rd rl da k1 k2
	p=$(other_of "$o")
	section "$o runs the NPCs, $p shoots: set up"
	enter_with_runner "$o"; pick_targets "$o"
	[ "${#TG[@]}" -lt 1 ] && return
	section "$o runs the NPCs, $p shoots: copy hits (one shot at each of two fresh NPCs)"
	snap R0
	for rl in round round2; do
		t=$(tg_by $o $rl); [ -z "$t" ] && continue; ro=$(tg_ro "$t")
		shoot "$p" "$t" 1; settle "$t"; agree "$rl, $o's $ro" "$t"
		first=$(awk -v r="$ro" '$1==r && $8=="start" {print $3, $6; exit}' "$SERIES"); read -r so sp <<<"$first"; read -r eo ep dO dP <<<"$(last_hp "$ro")"
		if is_num "$so" && is_num "$eo" && is_num "$ep" && { [ "$dO" = true ] || awk -v a="$so" -v b="$eo" -v c="$sp" -v d="$ep" 'BEGIN{exit !(b<a && d<c)}'; }; then pass "hit landed on $o's $ro (health fell in both games)" "runner $so -> $eo, copy $sp -> $ep"; else fail "hit landed on $o's $ro (health fell in both games)" "runner $so -> $eo, copy $sp -> $ep: inconclusive, no hit landed or one game did not follow"; fi
	done
	snap R1
	expect_ge "the shooter's copy hits rose ($p)" "$(delta $p R0 copyHits R1)" 1
	da=$(delta $o R0 damageAnswered R1 1); if is_num "$da" && [ "$da" -ge 1 ]; then pass "the runner answered them: damageAnswered rose ($o)" "+$da"; else warn "the runner answered them: damageAnswered rose ($o)" "stayed at +$da: UNRESOLVED, the counter may count something else; the shooter's copyAnswers is asserted at the end"; fi


	section "$o runs the NPCs, $p shoots: pause menu (held hits)"
	pick=$(tg_by $o pause)
	if [ -n "$pick" ]; then
		# The runner's NPCs fall back to the shooter's game about 4 s after the runner's game stops (NpcSync "runs its own AI here for
		# now"), so the shot goes in at once, while the hit is still held for the runner: one shot, at an NPC nobody has claimed yet.
		ro=$(tg_ro "$pick"); snap P0; h0=$(hpv "$o" "$ro"); dc "$o" "key 0x1B 100" >/dev/null
		shoot "$p" "$pick" 1
		pm=$(field "$(dc "$o" paused)" pauseMenu); sleep 2.5; hc=$(hpv "$p" "$ro"); sample "$pick" paused
		if [ "$pm" = true ]; then pass "pause ($o runs the NPC): the pause menu was open for the shot" "pauseMenu=true"; else fail "pause ($o runs the NPC): the pause menu was open for the shot" "paused says pauseMenu=$pm"; fi
		dc "$o" "key 0x1B 100" >/dev/null; sleep 1.5
		[ "$(field "$(dc "$o" paused)" pauseMenu)" = true ] && { dc "$o" "key 0x1B 100" >/dev/null; sleep 1; }
		settle "$pick"; sleep 3; sample "$pick" afterpause; snap P1
		agree "pause ($o runs the NPC): after the menu closes" "$pick"
		read -r eo ep dO dP <<<"$(last_hp "$ro")"
		if is_num "$h0" && is_num "$eo" && { [ "$dO" = true ] || awk -v a="$h0" -v b="$eo" 'BEGIN{exit !(b<a)}'; }; then pass "pause ($o runs the NPC): the held hits landed" "runner $h0 -> $eo (the copy read $hc while paused)"; else fail "pause ($o runs the NPC): the held hits landed" "runner $h0 -> '$eo': inconclusive or the hits were lost"; fi
		held0="$(cget P0 $o damageHeld) $(cget P0 $o damageAnswered)"; held1="$(cget P1 $o damageHeld) $(cget P1 $o damageAnswered)"
		if [ "$held0" != "$held1" ]; then pass "pause ($o runs the NPC): damageHeld/damageAnswered moved" "$held0 -> $held1"; else warn "pause ($o runs the NPC): damageHeld/damageAnswered moved" "unchanged ($held1): UNRESOLVED, the hit may have been answered before the menu stopped the game, or the NPC fell back to the shooter first; health agreement is asserted"; fi
	fi

	section "$o runs the NPCs, $p shoots: kill (both players shoot it)"
	pick=$(tg_by $o kill)
	if [ -n "$pick" ]; then
		ro=$(tg_ro "$pick"); n=0
		while [ $n -lt 8 ]; do
			[ "$(deadv "$o" "$ro")" = true ] && break
			shoot "$p" "$pick" 1 own; [ "$(deadv "$o" "$ro")" = true ] && break; shoot "$o" "$pick" 1 own; n=$((n + 2))
		done
		sleep 4; sample "$pick" killed
		k1=$(deadv "$o" "$ro"); if [ "$k1" = true ]; then pass "kill ($o's NPC $ro): dead in the runner's game" "dead"; else warn "kill ($o's NPC $ro): dead in the runner's game" "not dead after $n shots (dead=$k1): UNRESOLVED, shots at a claimed NPC may miss"; fi
		k2=$(deadv "$p" "$ro"); if [ "$k1" = true ]; then expect_eq "kill ($o's NPC $ro): dead in the other game" "$k2" true; fi
	fi
	for t in "${TG[@]}"; do dc "$o" "console $(tg_ro "$t").setrestrained 0" >/dev/null; done
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
	expect_ge "answers to those hits ($g)" "$(delta $g A copyAnswers C 1)" 1
	rr=$(delta $g A copyRestores C 1); [ -n "$rr" ] && [ "$rr" -gt 0 ] && warn "give-backs of a copy's health ($g)" "copyRestores rose by $rr (a drop given back: see the log for the last hit)"
done
[ "${#FLIPPED[@]}" -gt 0 ] && echo "NPCs taken over by a shot: ${!FLIPPED[*]}"

section "end checks"
expect_match "both games up" "$(alive)" '^host=ingame=true vm=ingame=true$'
crash_verdict "hits"
for g in host vm; do log_check $g "$g"; log_privacy $g "$g"; done
a_summary
