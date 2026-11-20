#!/bin/bash
# downed-inside-memory.sh [who]: a lethal hit inside one of DiMA's memories (Far Harbor) with downed mode on. Rig report
# (2026-11-19): inside the memory a lethal hit put the player down where no friend can reach them; after the 45 s bleed-out the
# kill was refused, because the memory itself keeps the player essential (MQ04Script.EnterVR's Actor.SetEssential(True)), and
# the player stayed on the ground until they loaded a save. <who> (vm, the default, or host) goes into the first memory and
# takes the hit there; the other player stays out (no friend can come into a memory). Then <who> is watched well past the
# bleed-out: still down, or up and able to walk.
# Set-up, said so where it happens:
#   - the way in is DLC03MQ04_01's own quick way in (console setstage <DLC03MQ04_01> 1, the stage EnterVR(True) runs from; in play
#     DiMA's terminal does the same), with Far Harbor's DLCCoast.esm at load index FH (02 on the rig); the memory's tutorial box
#     is closed with a real Enter;
#   - the lethal hit: <who>'s god mode off and its health brought down to 10 by console damageav (not lethal itself), then a frag
#     grenade's explosion (000E574F) placed at the player's feet by console placeatme: an injected grenade, as the rig's combat
#     steps inject the host's grenades. The other player keeps god mode on;
#   - the memory answers the down with its own box ("Do you want to exit the simulation?"), which keeps the game in menu mode while
#     it is up: answered No with a real Enter, as a player who goes on in the memory would;
#   - up again, the player walks (the virtual pad's stick, real movement) 250 u east, and back west if east is blocked;
#   - the way out at the end is the game's own, as in downed-after-memory.sh (dev `workshop exit`, then the memory's own box
#     answered Yes with real keys), so the game is left out of the memory; a player still down is left as found (restart it).
# Read every 2 s: the life state (Actor's lifeState: 0 alive, 7 an essential's down, 1 dying, 2 dead), downed mode's own idea
# (`rules`: down=, and keptUps= on a build that has it) and the essential bit (boolFlags bit 18).
# REPRO: YES when the player is still down (or dead, or killed and the save loaded) WAIT s after the hit (default 75: the 45 s
# bleed-out and 30 s more); NO when they are up, alive, still in the memory, and the walk moved them; VOID when the scene was
# not reached.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
WHO=${1:-vm}; FRIEND=$(other_game "$WHO"); FH=${FH:-02}; WAIT=${WAIT:-75}
DIMA_FIRST=${FH}01CBFA      # DLC03MQ04_01, the first memory's quest
VR_WORLD=${FH}004EA4        # DLC03VRWorldspace
FRAG_BLAST=000E574F         # the frag grenade's explosion
[ "$WHO" = host ] || [ "$WHO" = vm ] || verdict VOID "usage: downed-inside-memory.sh [host|vm]"
need_session

essbit() { local b; b=$(field "$(dc "$1" 'flags 00000014')" boolFlags); [[ $b =~ ^[0-9A-Fa-f]{8}$ ]] && echo $(( (16#$b >> 18) & 1 )); }
life() { field "$(dc "$1" 'flags 00000014')" lifeState; }
hp() { local h; h=$(field "$(dc "$1" 'flags 00000014')" health); echo "${h%.*}"; }
facts() {
	local r; r=$(dc "$1" rules)
	echo "life state $(life "$1"), health $(hp "$1"), essential bit $(essbit "$1"), downed mode: essential=$(field "$r" essential) down=$(field "$r" down) keptUps=$(field "$r" keptUps) bleedOuts=$(field "$r" bleedOuts) killRetries=$(field "$r" killRetries)"
}
in_vr() { [ "$(field "$(dc "$WHO" status)" worldspace)" = "$VR_WORLD" ]; }
out_vr() { local s; s=$(dc "$WHO" status); grep -q 'ingame=true' <<<"$s" && [ "$(field "$s" worldspace)" != "$VR_WORLD" ]; }
box_up() { menu_open "$WHO" MessageBoxMenu; }
god() { dc "$1" "god $2" >/dev/null; sleep 0.5; dc "$1" "god $2"; }
heal() { god "$1" on >/dev/null; dc "$1" "console player.restoreav health 10000" >/dev/null; }

# leave: out of the memory the game's way (its box), as downed-after-memory.sh does; 0 when out
leave() {
	local try
	for try in 1 2 3; do
		dc "$WHO" "workshop exit" >/dev/null
		wait_for 10 box_up || { sleep 2; continue; }
		say "$WHO: leaving: the memory's own box '$(rmsgbox "$WHO")': real Down, Enter (Yes)"
		rkey "$WHO" $VK_DOWN; sleep 0.5; rkey "$WHO" $VK_ENTER
		wait_for 60 out_vr && return 0
	done
	out_vr
}

# ---- before ----------------------------------------------------------------------------------------------------------------------
for w in host vm; do
	r=$(dc $w rules)
	[ "$(field "$r" revive)" = true ] || verdict VOID "$w: downed mode is off in the session's rules ($(cut -c1-80 <<<"$r"))"
	say "$w before: $(facts $w)"
done
[ "$(essbit "$WHO")" = 1 ] || verdict VOID "downed mode had not made $WHO essential before the memory"
in_vr && verdict VOID "$WHO is in DiMA's memory already (an earlier run): restart that game"
[ "$(life "$WHO")" = 0 ] || verdict VOID "$WHO is not alive and up before the run (life state $(life "$WHO"))"
heal "$FRIEND" >/dev/null
rmark host; rmark vm

# ---- into the memory -------------------------------------------------------------------------------------------------------------
say "set-up: $WHO goes into DiMA's first memory by its quick way in (console setstage $DIMA_FIRST 1: MQ04Script.EnterVR(True))"
dc "$WHO" focus >/dev/null; dc "$WHO" "console setstage $DIMA_FIRST 1" >/dev/null
wait_for 60 in_vr || verdict VOID "$WHO did not get into DiMA's memory ($(dc "$WHO" status | cut -c1-140))"
wait_for 20 box_up && { say "$WHO in the memory: the tutorial box '$(rmsgbox "$WHO")' closed with a real Enter"; rkey "$WHO" $VK_ENTER; sleep 2; }
sleep 3
say "$WHO in the memory: $(facts "$WHO")"
say "$FRIEND (stays out): $(facts "$FRIEND")"

# ---- the lethal hit inside it ----------------------------------------------------------------------------------------------------
[ "$(god "$WHO" off)" = "god=false" ] || verdict VOID "$WHO's god mode would not go off"
rmark "$WHO" hit; dc "$WHO" focus >/dev/null
h=$(hp "$WHO"); is_int "$h" && [ "$h" -gt 10 ] && dc "$WHO" "console player.damageav health $((h - 10))" >/dev/null
say "set-up: $WHO at $(hp "$WHO") health (console damageav); a frag grenade's blast placed at its feet (console placeatme $FRAG_BLAST)"
dc "$WHO" "console player.placeatme $FRAG_BLAST" >/dev/null
T0=$SECONDS; HIT=none
for i in $(seq 1 40); do
	l=$(life "$WHO")
	case "$l" in 7) HIT=down; break;; 1|2) HIT=died; break;; esac
	rsince "$WHO" hit | grep -q 'Save: loading' && { HIT=died; break; }
	sleep 0.25
done
say "$WHO: the frag's blast: $HIT ($(facts "$WHO"))"
[ "$HIT" = none ] && { heal "$WHO"; verdict VOID "the blast neither put $WHO down nor killed them ($(facts "$WHO"))"; }
# The memory answers the down with its own box, "Do you want to exit the simulation?" (it leaves workshop mode for it): the player
# reads it and stays in the memory (No: a real Enter, No being the box's first answer), as one who wants to go on would. A box
# that comes again later is answered the same way.
no_box() {
	box_up || return 1
	local b; sleep 2; b=$(rmsgbox "$WHO")
	say "$WHO at +$((SECONDS - T0)) s: the memory's box '$b' over the down: answered No with a real Enter"
	rkey "$WHO" $VK_ENTER; sleep 1
	box_up && say "$WHO: the box is still up after the Enter ($(menus "$WHO"))"
	return 0
}
wait_for 8 box_up; no_box || say "$WHO: no box over the down ($(menus "$WHO"))"

# ---- watched past the bleed-out --------------------------------------------------------------------------------------------------
UP_AT=""; DOWN_AT=""; last=""
while [ $((SECONDS - T0)) -lt "$WAIT" ]; do
	l=$(life "$WHO"); now="life state $l"
	if [ "$now" != "$last" ]; then say "$WHO at +$((SECONDS - T0)) s: $(facts "$WHO")"; last=$now; fi
	case "$l" in 0) UP_AT=${UP_AT:-$((SECONDS - T0))};; 7) UP_AT=""; DOWN_AT=$((SECONDS - T0));; esac
	rsince "$WHO" hit | grep -q 'Save: loading' && { HIT=died; break; }
	no_box
	sleep 2
done
END=$(facts "$WHO"); L=$(life "$WHO")
say "$WHO at +$((SECONDS - T0)) s (the 45 s bleed-out and $((WAIT - 45)) s more): $END; menus open: $(menus "$WHO")"
say "$WHO's log since the hit: $(rsince "$WHO" hit | grep -E 'Downed: |Save: loading' | cut -c16-200 | tail -8 | paste -sd'|' -)"
rkeep host downed-inside-memory; rkeep vm downed-inside-memory

if [ "$HIT" = died ] || rsince "$WHO" hit | grep -q 'Save: loading'; then
	heal "$WHO"
	verdict YES "$WHO was killed by a hit inside DiMA's memory, which keeps the player essential (the death loaded a save)"
fi
case "$L" in
7) heal "$WHO"; say "$WHO is left as found (down in the memory): restart that game"; verdict YES "$WHO is still down inside DiMA's memory $((SECONDS - T0)) s after the hit (last seen down at +${DOWN_AT:-?} s; $END): stuck until a load" ;;
1|2) verdict YES "$WHO is dead or dying inside DiMA's memory ($END)" ;;
0) ;;
*) verdict VOID "$WHO's life state unread ($END)" ;;
esac
in_vr || verdict VOID "$WHO is up but no longer in DiMA's memory ($(dc "$WHO" status | cut -c1-140))"

# ---- up: real movement -----------------------------------------------------------------------------------------------------------
read -r x y <<<"$(pos_of "$WHO")"
is_int "${x%.*}" || verdict VOID "$WHO's position unread"
x=${x%.*}; y=${y%.*}; MOVED=0
for dx in 250 -250; do
	rwalk "$WHO" $((x + dx)) "$y" 60 0.6
	MOVED=$(dist_xy "$x $y" "$(pos_of "$WHO")"); say "$WHO walks (the pad's stick) toward $((x + dx)),$y: $WALKED; moved ${MOVED:-?} u"
	[ "${MOVED:-0}" -ge 80 ] && break
done
heal "$WHO"
AFTER=$(facts "$WHO")
if leave; then sleep 8; wait_ingame "$WHO" 30; say "$WHO out of the memory by its own box: $(facts "$WHO")"; else say "$WHO: could not leave the memory by its box: restart that game"; fi
[ "${MOVED:-0}" -ge 80 ] || verdict YES "$WHO is up after the hit (since +${UP_AT:-?} s) but could not walk: moved ${MOVED:-?} u ($AFTER)"
verdict NO "$WHO, downed by a lethal hit inside DiMA's memory, was up again by +${UP_AT:-?} s, alive, and walked ${MOVED} u (at the end: $END)"
