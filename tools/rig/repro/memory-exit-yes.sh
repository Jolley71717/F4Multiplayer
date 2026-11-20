#!/bin/bash
# memory-exit-yes.sh [who]: Yes on the "exit the simulation?" box that one of DiMA's memories (Far Harbor) opens over a down
# inside it, with downed mode on. Rig report (2026-11-20): inside a memory a lethal hit downs the player and the memory asks
# "Do you want to exit the simulation?"; answering Yes killed the player (the game went to the main menu or loaded a save) where
# the memory means to take them out of it alive. <who> (vm, the default, or host) goes into the first memory and takes the hit
# there; the other player stays out (no friend can come into a memory). Then <who> answers the box Yes with real keys and is
# watched: out of the memory alive, or dead.
# Set-up, said so where it happens (as downed-inside-memory.sh):
#   - the way in is DLC03MQ04_01's own quick way in (console setstage <DLC03MQ04_01> 1, the stage EnterVR(True) runs from; in play
#     DiMA's terminal does the same), with Far Harbor's DLCCoast.esm at load index FH (02 on the rig); the memory's tutorial box
#     is closed with a real Enter;
#   - the lethal hit: <who>'s god mode off and its health brought down to 10 by console damageav (not lethal itself), then a
#     frag grenade's explosion (000E574F) placed at the player's feet by console placeatme: an injected grenade, as the rig's
#     combat steps inject the host's grenades. The other player keeps god mode on;
#   - the memory's own box over the down is answered Yes with real keys (Down, Enter: No is its first answer), as a player who
#     wants out would, at once (a late Yes can come after downed mode has stood the player up, 20 s into the down);
#   - out of the memory and up, the player's god mode goes on at once (the way out leaves them in the Nucleus, whose Children of
#     Atom attack a coc'd character), and at the end they are taken back to SanctuaryExt by console coc.
# Read every second: the life state (Actor's lifeState: 0 alive, 7 an essential's down, 1 dying, 2 dead), the worldspace, the
# health, downed mode's own idea (`rules`) and the essential bit (boolFlags bit 18).
# REPRO: YES when the player died (life state dying or dead, a save loaded, or the game went to its main menu) after the Yes, or
# is still down out of the memory WAIT s after it (default 45); NO when they are out of the memory, alive and up; VOID when the
# scene was not reached (no down, no box, or the Yes did not take them out).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
WHO=${1:-vm}; FRIEND=$(other_game "$WHO"); FH=${FH:-02}; WAIT=${WAIT:-45}
DIMA_FIRST=${FH}01CBFA      # DLC03MQ04_01, the first memory's quest
VR_WORLD=${FH}004EA4        # DLC03VRWorldspace
FRAG_BLAST=000E574F         # the frag grenade's explosion
[ "$WHO" = host ] || [ "$WHO" = vm ] || verdict VOID "usage: memory-exit-yes.sh [host|vm]"
need_session

essbit() { local b; b=$(field "$(dc "$1" 'flags 00000014')" boolFlags); [[ $b =~ ^[0-9A-Fa-f]{8}$ ]] && echo $(( (16#$b >> 18) & 1 )); }
life() { field "$(dc "$1" 'flags 00000014')" lifeState; }
hp() { local h; h=$(field "$(dc "$1" 'flags 00000014')" health); echo "${h%.*}"; }
facts() {
	local r; r=$(dc "$1" rules)
	echo "life state $(life "$1"), health $(hp "$1"), essential bit $(essbit "$1"), worldspace $(field "$(dc "$1" status)" worldspace), downed mode: essential=$(field "$r" essential) keptEssential=$(field "$r" keptEssential) down=$(field "$r" down) keptUps=$(field "$r" keptUps) notEssentialCleared=$(field "$r" notEssentialCleared)"
}
in_vr() { [ "$(field "$(dc "$WHO" status)" worldspace)" = "$VR_WORLD" ]; }
box_up() { menu_open "$WHO" MessageBoxMenu; }
god() { dc "$1" "god $2" >/dev/null; sleep 0.5; dc "$1" "god $2"; }
heal() { god "$1" on >/dev/null; dc "$1" "console player.restoreav health 10000" >/dev/null; }
died_since_hit() { rsince "$WHO" hit | grep -q 'Save: loading'; }

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
	died_since_hit && { HIT=died; break; }
	sleep 0.25
done
say "$WHO: the frag's blast: $HIT at +$((SECONDS - T0)) s (life state $l)"
[ "$HIT" = none ] && { heal "$WHO"; verdict VOID "the blast neither put $WHO down nor killed them ($(facts "$WHO"))"; }
[ "$HIT" = died ] && { rkeep host memory-exit-yes; rkeep vm memory-exit-yes; heal "$WHO"; verdict VOID "$WHO died at the blast itself, before any box (that is downed-inside-memory.sh's bug, not this one)"; }

# ---- the memory's box over the down, answered Yes --------------------------------------------------------------------------------
wait_for 15 box_up || { heal "$WHO"; verdict VOID "no box over the down within 15 s ($(menus "$WHO"); $(facts "$WHO")): restart that game"; }
TB=$((SECONDS - T0))
# Answered at once, as a player who wants out reads the one line and answers: Down, Enter (No is the box's first answer). The
# box's text is written to the log first (dev `gfx`, quick) and read from it after the keys (reading the log takes seconds).
dc "$WHO" "gfx MessageBoxMenu root.Menu_mc 1" >/dev/null
TY=$SECONDS
rkey "$WHO" $VK_DOWN; sleep 0.5; rkey "$WHO" $VK_ENTER
rmark "$WHO" yes   # after the keys (a log read takes seconds); a death's load comes many seconds later
BOX=$(rsince "$WHO" hit | grep 'gfx: root.Menu_mc.Body_tf' | tail -1 | grep -oE "text='[^']*'" | sed "s/^text='//; s/'$//")
say "$WHO: the memory's box '$BOX' up over the down by +$TB s: answered Yes with real keys (Down, Enter) at +$((SECONDS - T0)) s"
grep -qi 'simulation' <<<"$BOX" || say "$WHO: the box does not name the simulation: read it above"
sleep 1
box_up && say "$WHO: a box is up after the Enter: '$(rmsgbox "$WHO")' ($(menus "$WHO"))"

# ---- watched: out alive, or dead -------------------------------------------------------------------------------------------------
OUT_AT=""; UP_OUT_SINCE=""; DIED=""; last=""
while [ $((SECONDS - TY)) -lt "$WAIT" ]; do
	s=$(dc "$WHO" status); l=$(life "$WHO"); ws=$(field "$s" worldspace)
	now="life state $l, ingame $(field "$s" ingame), worldspace $ws"
	if [ "$now" != "$last" ]; then say "$WHO at +$((SECONDS - TY)) s after the Yes: $now, health $(hp "$WHO")"; last=$now; fi
	case "$l" in 1|2) DIED="life state $l at +$((SECONDS - TY)) s"; break;; esac
	rsince "$WHO" yes | grep -q 'Save: loading' && { DIED="a save loaded at +$((SECONDS - TY)) s"; break; }
	if grep -q 'ingame=false' <<<"$s" && menu_open "$WHO" MainMenu; then DIED="the game went to its main menu at +$((SECONDS - TY)) s"; break; fi
	if grep -q 'ingame=true' <<<"$s" && [ -n "$ws" ] && [ "$ws" != "$VR_WORLD" ]; then
		OUT_AT=${OUT_AT:-$((SECONDS - TY))}
		# Out and up: god mode on at once, as the way out leaves the player among the Nucleus's Children of Atom, who attack a
		# coc'd character (a down from them would read as this bug's). Out but down: watched as it is.
		if [ "$l" = 0 ]; then
			[ -z "$UP_OUT_SINCE" ] && { UP_OUT_SINCE=$SECONDS; god "$WHO" on >/dev/null; say "$WHO out of the memory and up at +$((SECONDS - TY)) s: god mode on (the Nucleus's Children of Atom attack)"; }
			[ $((SECONDS - UP_OUT_SINCE)) -ge 5 ] && break
		fi
	fi
	sleep 1
done
END=$(facts "$WHO")
say "$WHO at +$((SECONDS - TY)) s after the Yes: $END; menus open: $(menus "$WHO")"
say "$WHO's log since the Yes: $(rsince "$WHO" yes | grep -E 'Downed: |Save: loading|essential' | cut -c16-220 | tail -10 | paste -sd'|' -)"
rkeep host memory-exit-yes; rkeep vm memory-exit-yes

L=$(life "$WHO")
# back to Sanctuary for the next run (console coc, a placement): out of the Nucleus, whose Children of Atom attack
if [ -z "$DIED" ] && [ "$L" = 0 ] && ! in_vr; then dc "$WHO" "console coc SanctuaryExt" >/dev/null; sleep 10; wait_ingame "$WHO" 30; say "$WHO taken back to SanctuaryExt (console coc) for the next run"; fi
[ -n "$DIED" ] && verdict YES "$WHO, down inside DiMA's memory, answered its box Yes and died ($DIED; at the end: $END)"
in_vr && { heal "$WHO"; verdict VOID "$WHO is still in DiMA's memory $WAIT s after the Yes ($END): the box's Yes did not take them out"; }
case "$L" in
0) heal "$WHO"; verdict NO "$WHO, down inside DiMA's memory, answered its box Yes and was out of it by +${OUT_AT:-?} s, alive and up ($END)" ;;
7) heal "$WHO"; verdict YES "$WHO answered the memory's box Yes and is out of it but still down $WAIT s after ($END)" ;;
*) verdict VOID "$WHO's life state unread after the Yes ($END)" ;;
esac
