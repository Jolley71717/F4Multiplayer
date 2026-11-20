#!/bin/bash
# downed-after-memory.sh [who]: downed mode after one of DiMA's memories (Far Harbor). Player report: two players with downed on
# went through DiMA's memories; some time later a lethal hit killed the joiner outright instead of putting them down, and later
# the host the same way. <who> (vm, the default, or host) goes into the first memory and leaves it the way the game does; then
# each player takes a lethal hit from a real source, the friend who stayed out first (helped up by the memory's player standing
# beside them, so the down ends without a bleed-out), then the memory's player.
# Set-up, said so where it happens:
#   - the way in is DLC03MQ04_01's own quick way in (console setstage <DLC03MQ04_01> 1, the stage EnterVR(True) runs from; in play
#     DiMA's terminal does the same), with Far Harbor's DLCCoast.esm at load index FH (02 on the rig);
#   - the memory's tutorial box is closed with a real Enter; the way out is the game's own: the request to leave workshop mode (dev
#     `workshop exit`, what a Tab at the menu's top does; a sent Tab does not end workshop mode on the rig), then the memory's own
#     box "exit the simulation?" answered Yes with real keys (Down, Enter): MQ04Script.EnterVR(False), as any way out runs it;
#   - the friend comes to the memory's player by the mod's own teleport key (a real F7 hold);
#   - the lethal hit: the target's god mode off and its health brought down to 10 by console damageav (not lethal itself), then a
#     frag grenade's explosion (000E574F) placed at the player's feet by console placeatme: an injected grenade, as the rig's
#     combat steps inject the host's grenades. The other player keeps god mode on, so the blast never touches them.
# RELOAD=1: after the memory its player saves and loads (real F5, then F9) before the hits.
# Read for each: the player's essential bit (Actor::boolFlags bit 18, which the engine works out every frame from the reference's
# own essential override, else the base form's flag that downed mode sets) before, in and after the memory, and whether the hit
# put them down (`rules`: down=true) or killed them (life state dying or dead).
# REPRO: YES when a player died outright; NO when both went down (and were helped up); VOID when the scene was not reached.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
WHO=${1:-vm}; FRIEND=$(other_game "$WHO"); FH=${FH:-02}; RELOAD=${RELOAD:-0}; QUIET=${QUIET:-SanctuaryExt}
DIMA_FIRST=${FH}01CBFA      # DLC03MQ04_01, the first memory's quest
VR_WORLD=${FH}004EA4        # DLC03VRWorldspace
FRAG_BLAST=000E574F         # the frag grenade's explosion
VK_F5=0x74; VK_F9=0x78; VK_F7=0x76
[ "$WHO" = host ] || [ "$WHO" = vm ] || verdict VOID "usage: downed-after-memory.sh [host|vm]"
need_session

# essbit <who>: 1 when the player's character is essential now (boolFlags bit 18), 0 when not, empty when unread
essbit() { local b; b=$(field "$(dc "$1" 'flags 00000014')" boolFlags); [[ $b =~ ^[0-9A-Fa-f]{8}$ ]] && echo $(( (16#$b >> 18) & 1 )); }
ours() { field "$(dc "$1" rules)" essential; }   # downed mode's own idea: it made the player essential
facts() { echo "essential bit $(essbit "$1"), downed mode's essential=$(ours "$1"), $(dc "$1" 'flags 00000014' | grep -oE 'lifeState=[0-9]+ health=[-0-9.]+')"; }
in_vr() { [ "$(field "$(dc "$WHO" status)" worldspace)" = "$VR_WORLD" ]; }
out_vr() { local s; s=$(dc "$WHO" status); grep -q 'ingame=true' <<<"$s" && [ "$(field "$s" worldspace)" != "$VR_WORLD" ]; }
box_up() { menu_open "$WHO" MessageBoxMenu; }
is_down() { [ "$(field "$(dc "$1" rules)" down)" = true ]; }
is_up() { [ "$(field "$(dc "$1" rules)" down)" = false ]; }
dying() { case "$(field "$(dc "$1" 'flags 00000014')" lifeState)" in 1|2) return 0;; esac; return 1; }   # dying or dead (7 is an essential's down)
god() { dc "$1" "god $2" >/dev/null; sleep 0.5; dc "$1" "god $2"; }
heal() { god "$1" on >/dev/null; dc "$1" "console player.restoreav health 10000" >/dev/null; }

# hit <target> <helper>: the lethal hit; HIT says what happened (down | died | none), HIT_WHY the detail
hit() {
	local t=$1 h=$2 hp i p
	heal "$h"
	[ "$(god "$t" off)" = "god=false" ] || { HIT=none; HIT_WHY="$t's god mode would not go off"; return; }
	say "$t: before the hit: $(facts "$t")"
	rmark "$t" hit; dc "$t" focus >/dev/null
	hp=$(field "$(dc "$t" 'flags 00000014')" health); hp=${hp%.*}
	is_int "$hp" && [ "$hp" -gt 10 ] && dc "$t" "console player.damageav health $((hp - 10))" >/dev/null
	dc "$t" "console player.placeatme $FRAG_BLAST" >/dev/null
	HIT=none; HIT_WHY="neither down nor dead 10 s after the blast ($(facts "$t"))"
	for i in $(seq 1 30); do
		if is_down "$t" || rsince "$t" hit | grep -q 'Downed: down;'; then HIT=down; HIT_WHY="down and waiting for help ($(dc "$t" rules | grep -oE 'downs=[0-9]+'))"; break; fi
		if dying "$t"; then HIT=died; HIT_WHY="killed outright: $(dc "$t" 'flags 00000014' | grep -oE 'lifeState=[0-9]+ health=[-0-9.]+'), never down"; break; fi
		if rsince "$t" hit | grep -q 'Save: loading'; then HIT=died; HIT_WHY="killed outright: never down, and the death loaded a save"; break; fi
		sleep 0.3
	done
	say "$t: the frag's blast at its feet: $HIT_WHY"
	if [ "$HIT" = down ]; then
		# the helper stands beside them (the teleport): the help comes after the rules' 2 s; else a real walk to the stand-in
		if ! wait_for 12 is_up "$t"; then
			p=$(dc "$h" 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | head -1 | cut -d= -f2)
			[ -n "$p" ] && { rwalk "$h" "$p" 90 0.6; say "$h walks to $t's stand-in to help ($WALKED)"; }
			wait_for 20 is_up "$t"
		fi
		is_up "$t" && say "$t: helped up by $h ($(rsince "$t" hit | grep -oE 'Downed: helped up by player [0-9]+' | tail -1))" || say "$t: still down: not helped up"
	elif [ "$HIT" = died ]; then
		sleep 12; wait_ingame "$t" 60
		say "$t: after the death: $(rsince "$t" hit | grep -E 'Save: loading|Downed: ' | cut -c16-170 | tail -3 | paste -sd'|' -)"
		say "$t: now: $(dc "$t" status | grep -oE 'ingame=[a-z]+ .*location=[0-9A-F]+ .*' | cut -c1-150); $(facts "$t")"
	fi
	heal "$t"
}

# ---- before ----------------------------------------------------------------------------------------------------------------------
for w in host vm; do
	r=$(dc $w rules)
	[ "$(field "$r" revive)" = true ] || verdict VOID "$w: downed mode is off in the session's rules ($(cut -c1-80 <<<"$r"))"
	say "$w before: $(facts $w)"
done
[ "$(essbit "$WHO")" = 1 ] && [ "$(essbit "$FRIEND")" = 1 ] || verdict VOID "downed mode had not made both players essential before the memory"
in_vr && verdict VOID "$WHO is in DiMA's memory already (an earlier run): restart that game"
rmark host; rmark vm

# ---- the memory ------------------------------------------------------------------------------------------------------------------
say "set-up: $WHO goes into DiMA's first memory by its quick way in (console setstage $DIMA_FIRST 1: MQ04Script.EnterVR(True))"
dc "$WHO" focus >/dev/null; dc "$WHO" "console setstage $DIMA_FIRST 1" >/dev/null
wait_for 60 in_vr || verdict VOID "$WHO did not get into DiMA's memory ($(dc "$WHO" status | cut -c1-140))"
wait_for 20 box_up && { say "$WHO in the memory: the tutorial box '$(rmsgbox "$WHO")' closed with a real Enter"; rkey "$WHO" $VK_ENTER; sleep 2; }
say "$WHO in the memory: $(facts "$WHO")"
for try in 1 2 3; do
	dc "$WHO" "workshop exit" >/dev/null   # what a Tab at the build menu's top does (the game's request to leave workshop mode)
	wait_for 10 box_up || { sleep 2; continue; }
	BOX=$(rmsgbox "$WHO"); say "$WHO: leaving: the memory's own box '$BOX': real Down, Enter (Yes)"
	rkey "$WHO" $VK_DOWN; sleep 0.5; rkey "$WHO" $VK_ENTER
	wait_for 60 out_vr && break
done
out_vr || verdict VOID "$WHO did not come out of DiMA's memory by its own box ($(dc "$WHO" status | cut -c1-140))"
sleep 8; wait_ingame "$WHO" 30
say "$WHO out of the memory at $(dc "$WHO" status | grep -oE "location=[0-9A-F]+ '[^']*'"): $(facts "$WHO")"
# The way out lands in the Nucleus, whose Children of Atom fight a character that hasn't met them (a coc character): their shots
# and grenades would make the hits below no test. Both players go to a quiet place first.
say "set-up: $WHO to $QUIET by console coc (the Nucleus's own people fight a coc character)"
dc "$WHO" focus >/dev/null; dc "$WHO" "console coc $QUIET" >/dev/null; sleep 6; wait_ingame "$WHO" 60; sleep 4
say "$WHO at $(dc "$WHO" status | grep -oE "location=[0-9A-F]+ '[^']*'"): $(facts "$WHO")"
say "$FRIEND (never in the memory): $(facts "$FRIEND")"
if [ "$RELOAD" = 1 ]; then
	rmark "$WHO" reload
	rkey "$WHO" $VK_F5; sleep 6; rkey "$WHO" $VK_F9; sleep 15; wait_ingame "$WHO" 60; sleep 5
	say "$WHO saved and loaded (real F5, F9): $(rsince "$WHO" reload | grep -oE 'Save: (loading|.*begins).*' | cut -c1-90 | paste -sd'|' -); $(facts "$WHO")"
fi

# ---- the friend comes over (the teleport key), then the hits ---------------------------------------------------------------------
dc "$FRIEND" "console player.restoreav health 10000" >/dev/null
rkey "$FRIEND" $VK_F7 1500; sleep 12; wait_ingame "$FRIEND" 60; sleep 3
P=$(dc "$FRIEND" 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | head -1 | cut -d= -f2)
D=$(dist_xy "$(pos_of "$FRIEND")" "$(field "$(dc "$FRIEND" "refinfo $P")" x) $(field "$(dc "$FRIEND" "refinfo $P")" y)")
say "$FRIEND teleported to $WHO (a real F7 hold): $(dc "$FRIEND" status | grep -oE "location=[0-9A-F]+ '[^']*'"), ${D:-?} u from $WHO's stand-in"
[ -n "$D" ] && [ "$D" -le 600 ] || verdict VOID "$FRIEND is not beside $WHO after the teleport (${D:-no stand-in})"

hit "$FRIEND" "$WHO"; F_HIT=$HIT; F_WHY=$HIT_WHY
[ "$F_HIT" = down ] && ! is_up "$FRIEND" && verdict VOID "$FRIEND went down but was not helped up: the next hit would find them down"
hit "$WHO" "$FRIEND"; W_HIT=$HIT; W_WHY=$HIT_WHY
rkeep host downed-after-memory; rkeep vm downed-after-memory
heal host; heal vm

say "the friend who stayed out ($FRIEND): $F_HIT; the memory's player ($WHO): $W_HIT"
if [ "$F_HIT" = none ] || [ "$W_HIT" = none ]; then verdict VOID "a hit did neither ($FRIEND: $F_WHY; $WHO: $W_WHY)"; fi
if [ "$W_HIT" = died ] || [ "$F_HIT" = died ]; then
	why=""; [ "$W_HIT" = died ] && why="$WHO, after DiMA's memory, was $W_WHY"
	[ "$F_HIT" = died ] && why="${why:+$why; }$FRIEND, never in it, was $F_WHY"
	verdict YES "$why"
fi
verdict NO "both went down instead of dying: $WHO after DiMA's memory, and $FRIEND who stayed out"
