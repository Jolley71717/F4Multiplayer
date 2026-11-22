#!/bin/bash
# npc-hit-on-player.sh [victim]: what an NPC's hit on a player does when the NPC runs in the friend's game. Report: a hit an NPC
# lands on a friend's stand-in is resisted by the stand-in's copy of the victim's armor in the game that runs the NPC, then
# forwarded and taken off the victim's health only, so the victim's own perks and resistances, limb damage and radiation never
# apply. <victim> (vm, the default, or host) is hit; the other game (the "runner") runs the attackers.
# No gamma gun or other explosive shot: those go off in both games and are never forwarded (the victim's own game's copy hits it
# there, the engine's own way), so they are not this bug. A first run's gamma blasts downed the victim that way.
# Set-up, said so where it happens:
#   - both games to Vault111Ext by console coc (a quiet place with no settlers to fight);
#   - the victim walks (a real walk) about 700 units away from the runner's player, so the attackers pick the stand-in;
#   - in the runner's game, two raiders made with the dev channel's spawn (in front of the runner's player, who faces the
#     stand-in: a runtime NPC, shared by the session as a spawn): a melee raider beside the stand-in, and a raider given a 10mm
#     pistol and its rounds (console additem, equipitem: plain shots) about 600 units short of the stand-in, in clear sight of it
#     (not restrained: a restrained actor plays its bound idle and never attacks), both told to fight the stand-in (console
#     startcombat). The runner's player has god mode and is put in the raiders' faction (console
#     addtofaction RaiderFaction, taken out at the end: raiders made next to it otherwise fight it and leave the stand-in, which
#     is in PlayerFaction only);
#   - phase "plain": the victim as it is; phase "armored": the victim's own damage resistance raised by 5000 (console modav
#     damageresist: what Toughness and other perks raise; the stand-in's copy does not have it), the shooter removed;
#   - the victim's health raised by 1000 (console modav health, put back at the end) and restored by console between samples
#     (restoreav), so no hit downs it.
# Read in the victim's game, each sample: its hits taken (`net status` hits: taken=), health, Rads and the six limb conditions
# (dev `av`), and the mod's log lines about the hits.
# REPRO: YES when hits were taken and the victim's game applied none of its own side: no limb condition lost, and the armored
# phase's hits as hard as the plain phase's (its own resistance ignored). NO when the hits cost limb condition and the armored
# phase's hits were far softer. VOID when no hit was taken, or the victim went down (not the scene meant). A gate: no hit
# forwarded to the victim within GATE (30) seconds of the startcombat ends the run VOID at once, with the raiders' AI facts
# (dev mirror: package, combat target) and the stand-in's health in the runner's game, which say why.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
WHO=${1:-vm}; RUNNER=$(other_game "$WHO"); CELL=${CELL:-Vault111Ext}
MELEE=${MELEE:-00047162}       # EncRaider01Melee
SHOOTER=${SHOOTER:-001AE38D}   # the raider vats-enter.sh uses
PISTOL=00004822; AMMO=0001F276 # a 10mm and its rounds
AV_HEALTH=000002D4; AV_RADS=000002E1
LIMBS="0000036C 0000036D 0000036E 0000036F 00000370 00000371"   # Perception (head), Endurance (torso), left/right attack, left/right mobility
SECS=${SECS:-40}; GATE=${GATE:-30}
[ "$WHO" = host ] || [ "$WHO" = vm ] || verdict VOID "usage: npc-hit-on-player.sh [host|vm]"
need_session

av() { local v; v=$(dc "$WHO" "av 00000014 $1"); awk -v v="$v" 'BEGIN { if (v ~ /^-?[0-9.]+$/) printf "%.1f", v }'; }
taken() { field "$(dc "$WHO" 'net status')" taken; }
limbs() { local l s=""; for l in $LIMBS; do s="$s $(av "$l")"; done; echo "${s# }"; }
limb_min() { tr ' ' '\n' <<<"$1" | sort -g | head -1; }
standin_of() { dc "$1" 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | head -1 | cut -d= -f2; }
spawn_at() { field "$(dc "$RUNNER" "spawn $1 $2")" ref; }

# sample <phase> <seconds>: hits taken, the health they cost and the rads, read each second; HITS, LOSS, RADS for the phase
sample() {
	local phase=$1 end=$((SECONDS + $2)) t0 h0 t1 h1 r0
	HITS=0; LOSS=0; r0=$(av $AV_RADS)
	t0=$(taken); h0=$(av $AV_HEALTH)
	while [ $SECONDS -lt $end ]; do
		sleep 1
		t1=$(taken); h1=$(av $AV_HEALTH)
		if is_int "$t0" && is_int "$t1" && [ "$t1" -gt "$t0" ]; then
			HITS=$((HITS + t1 - t0)); LOSS=$(awk -v a="$h0" -v b="$h1" -v l="$LOSS" 'BEGIN { d = a - b; printf "%.1f", l + (d > 0 ? d : 0) }')
		fi
		dc "$WHO" 'console player.restoreav health 10000' >/dev/null
		t0=$(taken); h0=$(av $AV_HEALTH)
	done
	RADS=$(awk -v a="$r0" -v b="$(av $AV_RADS)" 'BEGIN { printf "%.1f", b - a }')
	PER=$(awk -v l="$LOSS" -v h="$HITS" 'BEGIN { if (h > 0) printf "%.1f", l / h; else print "" }')
	say "$phase: $HITS hits taken in $2 s, health lost $LOSS ($PER a hit), rads +$RADS, limbs now $(limbs)"
}

# ---- set-up ------------------------------------------------------------------------------------------------------------------------
say "set-up: both games to $CELL by console coc"
enter_both "$CELL" 16
dc "$RUNNER" 'god on' >/dev/null; dc "$WHO" 'god off' >/dev/null; dc "$WHO" 'god off' | grep -q 'god=false' || verdict VOID "$WHO's god mode would not go off"
dc "$WHO" 'console player.modav health 1000' >/dev/null; dc "$WHO" 'console player.restoreav health 10000' >/dev/null
P=$(standin_of "$RUNNER"); [ -n "$P" ] || verdict VOID "$RUNNER's game has no stand-in for $WHO"
read -r rx ry <<<"$(pos_of "$RUNNER")"
rwalk "$WHO" "$(awk -v x="$rx" 'BEGIN { printf "%d", x + 700 }')" "$ry" 120
say "$WHO walks away from $RUNNER's player (a real walk): $WALKED; apart $(dist_xy "$(pos_of "$WHO")" "$rx $ry") units"
sleep 2
D=$(dist_xy "$(pos_of "$RUNNER")" "$(pos_of "$WHO")"); is_int "$D" || verdict VOID "could not read the players' positions"
dc "$RUNNER" "turnto $P 300" >/dev/null; sleep 0.8
L0=$(limbs); R00=$(av $AV_RADS)
say "$WHO before: health $(av $AV_HEALTH), rads $R00, limbs $L0, hits taken $(taken), damage resist $(av 000002E3)"
rmark "$WHO"; rmark "$RUNNER"

RAIDERS=0001CBED   # RaiderFaction
say "set-up: $RUNNER's player put in the raiders' faction (console addtofaction), so they leave it for the stand-in"
dc "$RUNNER" "console player.addtofaction $RAIDERS 1" >/dev/null
# cleanup: what the set-up changed, put back (also on a gate's VOID)
cleanup() {
	dc "$WHO" 'console player.modav damageresist -'"${DR_ADDED:-0}" >/dev/null
	for r in ${M:-} ${G:-}; do dc "$RUNNER" "remove $r" >/dev/null; done
	dc "$WHO" 'console player.modav health -1000' >/dev/null; dc "$WHO" 'console player.restoreav health 10000' >/dev/null
	for l in PerceptionCondition EnduranceCondition LeftAttackCondition RightAttackCondition LeftMobilityCondition RightMobilityCondition; do dc "$WHO" "console player.restoreav $l 100" >/dev/null; done
	dc "$WHO" 'god on' >/dev/null
	dc "$RUNNER" "console player.removefromfaction $RAIDERS" >/dev/null
	say "cleanup: raiders removed, $WHO's health and resistance put back, god on, $RUNNER's player out of the raiders' faction (console removefromfaction)"
}
# why: what the raiders and the stand-in were doing, in the runner's game
why() { local r; for r in ${M:-} ${G:-}; do echo -n "$r: $(dc "$RUNNER" "combat $r") $(dc "$RUNNER" "mirror $r" | grep -oE 'ai: .*' | cut -c1-140) apart $(dist_xy "$(pos_of_ref "$r")" "$(pos_of_ref "$P")"); "; done; echo "stand-in $P: $(dc "$RUNNER" "combat $P")"; }
pos_of_ref() { local p; p=$(dc "$RUNNER" "refinfo $1"); echo "$(field "$p" x) $(field "$p" y)"; }

say "set-up: in $RUNNER's game a melee raider ($MELEE) and a raider with a 10mm pistol ($SHOOTER, console additem/equipitem, not restrained) made in front of its player toward $WHO's stand-in $P (dev spawn), told to fight it (console startcombat)"
M=$(spawn_at "$MELEE" $((D - 100))); G=$(spawn_at "$SHOOTER" $((D > 700 ? D - 600 : 100)))
[ -n "$M" ] && [ -n "$G" ] || { cleanup; verdict VOID "the raiders could not be made (melee '$M', shooter '$G')"; }
dc "$RUNNER" "console $G.additem $PISTOL 1" >/dev/null; dc "$RUNNER" "console $G.additem $AMMO 500" >/dev/null
dc "$RUNNER" "console $G.equipitem $PISTOL" >/dev/null
T0=$(taken)
for r in $M $G; do dc "$RUNNER" "console $r.startcombat $P" >/dev/null; done
say "raiders: melee $M ($(dc "$RUNNER" "refinfo $M" | grep -oE 'hp=[-0-9.]+' | head -1)), shooter $G ($(dc "$RUNNER" "equipped $G" | cut -c1-120)); $(why)"
# the gate: a hit forwarded to the victim within GATE seconds, or the run ends here
gate_hit() { local t; t=$(taken); is_int "$t" && is_int "$T0" && [ "$t" -gt "$T0" ]; }
if ! wait_for "$GATE" gate_hit; then
	W=$(why); cleanup
	verdict VOID "no hit was forwarded to $WHO within $GATE s of the startcombat: $W"
fi
say "gate: a hit reached $WHO within $GATE s"

sample plain "$SECS"
HITS_A=$HITS; PER_A=$PER; RADS_A=$RADS; L1=$(limbs)

say "set-up: the shooter removed; $WHO's own damage resistance raised by 5000 (console modav damageresist)"
dc "$RUNNER" "remove $G" >/dev/null
dc "$WHO" 'console player.modav damageresist 5000' >/dev/null; DR_ADDED=5000
dc "$RUNNER" "console $M.startcombat $P" >/dev/null
say "$WHO damage resist now $(av 000002E3)"
sample armored "$SECS"
HITS_B=$HITS; PER_B=$PER
G=""   # removed already
cleanup

rsince "$WHO" > "$REPRO_OUT/run-$WHO.log"; rsince "$RUNNER" > "$REPRO_OUT/run-$RUNNER.log"
say "$WHO's log on the hits: $(grep -E 'Session: .*hit|RealHits' "$REPRO_OUT/run-$WHO.log" | cut -c1-200 | tail -4 | paste -sd'|' -)"
DOWNS=$(grep -c 'Downed: down' "$REPRO_OUT/run-$WHO.log")
[ "$DOWNS" = 0 ] || verdict VOID "$WHO went down $DOWNS time(s) in the run: not the scene meant"
[ $((HITS_A + HITS_B)) -gt 0 ] || verdict VOID "no hit on $WHO's stand-in was taken in either phase"
LIMB_LOST=$(awk -v a="$(limb_min "$L0")" -v b="$(limb_min "$L1")" 'BEGIN { print (b < a - 0.5) ? 1 : 0 }')
SOFTER=$(awk -v a="$PER_A" -v b="$PER_B" 'BEGIN { print (a != "" && b != "" && b < a * 0.5) ? 1 : 0 }')
say "verdicts: rads +$RADS_A (a plain shot has none), limb condition lost $LIMB_LOST (min $(limb_min "$L0") -> $(limb_min "$L1")), armored hits far softer $SOFTER ($PER_A -> $PER_B a hit)"
if [ "$LIMB_LOST" = 0 ] && [ "$SOFTER" = 0 ]; then
	verdict YES "$WHO took $HITS_A + $HITS_B hits but its own side of them never ran: no limb damage, its own damage resistance ignored ($PER_A -> $PER_B a hit)"
fi
if [ "$LIMB_LOST" = 1 ] && [ "$SOFTER" = 1 ]; then
	verdict NO "$WHO's game applied the hits itself: limb damage, its own resistance counted ($PER_A -> $PER_B a hit)"
fi
verdict YES "only part of $WHO's side ran: limbs $LIMB_LOST, resistance $SOFTER ($PER_A -> $PER_B a hit)"
