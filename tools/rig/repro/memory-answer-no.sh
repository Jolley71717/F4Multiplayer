#!/bin/bash
# memory-answer-no.sh [who]: No on the "exit the simulation?" box that one of DiMA's memories (Far Harbor) opens over a down
# inside it, with downed mode on. Rig report (2026-11-20): with the mod in a session, No left the player down until downed mode's
# own stand-up 20 s into the down, with 26 health, and a Yes in that same second left them alive inside the memory. The unmodded
# game, timed on the rig (2026-11-20, DLL out, real keys, screenshots every 0.5 s): the box comes the moment the player is down
# and stops the game; the game's own clock for the down (fEssentialCompanionDeathTime, 10 s out of a fight; fEssentialDeathTime,
# 20 s in one) runs only once No is answered, and the game stands the player up ~10.5 s after the No with full health (85),
# whether No came 2 s or 10 s into the down. <who> (vm, the default, or host) goes into the first memory and takes the hit there;
# the other player stays out (no friend can come into a memory). Then <who> reads the box for HOLD s (default 12: a player who
# reads it a while, longer than the game's 10 s), answers No with a real key and is watched.
# Set-up, said so where it happens (as downed-inside-memory.sh):
#   - the way in is DLC03MQ04_01's own quick way in (console setstage <DLC03MQ04_01> 1, the stage EnterVR(True) runs from; in play
#     DiMA's terminal does the same), with Far Harbor's DLCCoast.esm at load index FH (02 on the rig); the memory's tutorial box
#     is closed with a real Enter;
#   - the lethal hit: <who>'s god mode off and its health brought down to 10 by console damageav (not lethal itself), then a
#     frag grenade's explosion (000E574F) placed at the player's feet by console placeatme: an injected grenade, as the rig's
#     combat steps inject the host's grenades. The other player keeps god mode on;
#   - the memory's own box over the down (it comes when the down ends the memory's workshop mode: MQ04Script's OnWorkshopMode,
#     EnterVR(False, True)) is answered No with a real Enter (No is its first answer) HOLD s into the down. No starts the
#     memory's workshop mode again and nothing more: the stand-up is the game's own;
#   - the way out at the end is the game's own, as in downed-inside-memory.sh (dev `workshop exit`, then the memory's own box
#     answered Yes with real keys), and then console coc SanctuaryExt (a placement) out of the Nucleus for the next run; a player
#     still down is left as found (restart it).
# Read every half second: the life state (Actor's lifeState: 0 alive, 7 an essential's down, 1 dying, 2 dead), the health, the
# character's flags (niFlags, boolFlags: bit 5 the stay-down flag, bit 18 essential), whether the game has the player in combat,
# and downed mode's own idea (`rules`: keptUps counts its own stand-ups of a down a script keeps essential).
# REPRO: YES when, after the No, the player is not up within the game's own time (10 s, or 20 s in combat) and UP_WITHIN s more
# (default 3), or is up with health below HEALTH_SHARE (default 80 %) of what they had in the memory before the hit (the game's own
# stand-up gives full health; downed mode's 30 %), or downed mode stood them up (keptUps); NO when the game stood them up in that
# time with that health; VOID when the scene was not reached (no down, no box).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
WHO=${1:-vm}; FRIEND=$(other_game "$WHO"); FH=${FH:-02}; UP_WITHIN=${UP_WITHIN:-3}; HOLD=${HOLD:-12}; HEALTH_SHARE=${HEALTH_SHARE:-80}; WATCH=${WATCH:-25}
DIMA_FIRST=${FH}01CBFA      # DLC03MQ04_01, the first memory's quest
VR_WORLD=${FH}004EA4        # DLC03VRWorldspace
FRAG_BLAST=000E574F         # the frag grenade's explosion
[ "$WHO" = host ] || [ "$WHO" = vm ] || verdict VOID "usage: memory-answer-no.sh [host|vm]"
need_session

flags() { dc "$1" 'flags 00000014'; }
essbit() { local b; b=$(field "$(flags "$1")" boolFlags); [[ $b =~ ^[0-9A-Fa-f]{8}$ ]] && echo $(( (16#$b >> 18) & 1 )); }
life() { field "$(flags "$1")" lifeState; }
hp() { local h; h=$(field "$(flags "$1")" health); echo "${h%.*}"; }
facts() {
	local r f; r=$(dc "$1" rules); f=$(flags "$1")
	echo "life state $(field "$f" lifeState), health $(field "$f" health), niFlags $(field "$f" niFlags), boolFlags $(field "$f" boolFlags), in combat $(field "$(dc "$1" 'combat 14')" inCombat), worldspace $(field "$(dc "$1" status)" worldspace), downed mode: essential=$(field "$r" essential) keptEssential=$(field "$r" keptEssential) down=$(field "$r" down) keptUps=$(field "$r" keptUps) bleedOuts=$(field "$r" bleedOuts)"
}
now_ms() { date +%s%3N; }
since() { awk -v a="$1" -v b="$(now_ms)" 'BEGIN { printf "%.1f", (b - a) / 1000 }'; }
in_vr() { [ "$(field "$(dc "$WHO" status)" worldspace)" = "$VR_WORLD" ]; }
out_vr() { local s; s=$(dc "$WHO" status); grep -q 'ingame=true' <<<"$s" && [ "$(field "$s" worldspace)" != "$VR_WORLD" ]; }
box_up() { menu_open "$WHO" MessageBoxMenu; }
god() { dc "$1" "god $2" >/dev/null; sleep 0.5; dc "$1" "god $2"; }
heal() { god "$1" on >/dev/null; dc "$1" "console player.restoreav health 10000" >/dev/null; }

# leave: out of the memory the game's way (its box), as downed-inside-memory.sh does; 0 when out
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
# finish: healed, out of the memory by its own box when up, the logs kept
finish() {
	heal "$WHO"
	if [ "$(life "$WHO")" = 0 ]; then
		if leave; then
			sleep 8; wait_ingame "$WHO" 30; say "$WHO out of the memory by its own box: $(facts "$WHO")"
			# back to Sanctuary for the next run (console coc, a placement): out of the Nucleus, whose Children of Atom and turrets
			# attack a coc'd character
			dc "$WHO" "console coc SanctuaryExt" >/dev/null; sleep 10; wait_ingame "$WHO" 30; say "$WHO taken back to SanctuaryExt (console coc) for the next run"
		else say "$WHO: could not leave the memory by its box: restart that game"; fi
	else
		say "$WHO is left as found (life state $(life "$WHO"), in the memory): restart that game"
	fi
	rkeep host memory-answer-no; rkeep vm memory-answer-no
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
heal "$WHO"; sleep 1
FULL=$(hp "$WHO")
say "$WHO in the memory (healed: full health $FULL): $(facts "$WHO")"
say "$FRIEND (stays out): $(facts "$FRIEND")"
is_int "$FULL" && [ "$FULL" -gt 20 ] || verdict VOID "$WHO's health in the memory unread ('$FULL')"

# ---- the lethal hit inside it ----------------------------------------------------------------------------------------------------
[ "$(god "$WHO" off)" = "god=false" ] || verdict VOID "$WHO's god mode would not go off"
rmark "$WHO" hit; dc "$WHO" focus >/dev/null
dc "$WHO" "console player.damageav health $((FULL - 10))" >/dev/null
say "set-up: $WHO at $(hp "$WHO") health (console damageav); a frag grenade's blast placed at its feet (console placeatme $FRAG_BLAST)"
dc "$WHO" "console player.placeatme $FRAG_BLAST" >/dev/null
T0=$(now_ms); HIT=none; KEPT0=$(field "$(dc "$WHO" rules)" keptUps)
for i in $(seq 1 40); do
	box_up && { HIT=down; break; }
	l=$(life "$WHO")
	case "$l" in 7) HIT=down; break;; 1|2) HIT=died; break;; esac
	rsince "$WHO" hit | grep -q 'Save: loading' && { HIT=died; break; }
	sleep 0.25
done
DOWN_AT=$(since "$T0")
# the box first (the game's comes the moment the player is down), the slow facts after
BOX_AT=""; box_up && BOX_AT=$(since "$T0")
say "$WHO: the frag's blast: $HIT at +$DOWN_AT s${BOX_AT:+, the box up at +$BOX_AT s} ($(facts "$WHO"))"
[ "$HIT" = none ] && { heal "$WHO"; verdict VOID "the blast neither put $WHO down nor killed them ($(facts "$WHO"))"; }
[ "$HIT" = died ] && { finish; verdict VOID "$WHO died at the blast itself, before any box (that is downed-inside-memory.sh's bug, not this one)"; }

# ---- the memory's box over the down, read HOLD s and answered No -----------------------------------------------------------------
# Watched every half second until the box comes (the unmodded game's comes at the down): a stand-up before the box is said, and
# the run goes on to the box all the same.
UP_BEFORE=""; last=""
for i in $(seq 1 60); do
	[ -n "$BOX_AT" ] && break
	box_up && { BOX_AT=$(since "$T0"); break; }
	l=$(life "$WHO")
	[ "$l" != "$last" ] && { say "$WHO at +$(since "$T0") s, no box yet: $(facts "$WHO")"; last=$l; }
	[ "$l" = 0 ] && UP_BEFORE=${UP_BEFORE:-$(since "$T0")}
	sleep 0.5
done
[ -n "$BOX_AT" ] || { finish; verdict VOID "no box over the down within 30 s ($(menus "$WHO"); $(facts "$WHO"))"; }
# Answered the moment it is seen, as a player who reads the one line and stays: Enter (No is the box's first answer). The text is
# written to the log first (dev `gfx`, quick) and read from it after.
dc "$WHO" "gfx MessageBoxMenu root.Menu_mc 1" >/dev/null
until [ "$(awk -v a="$T0" -v b="$(now_ms)" -v h="$HOLD" 'BEGIN { print (b - a >= h * 1000) }')" = 1 ]; do sleep 0.2; done
box_up || say "$WHO: the box was gone before the No (menus: $(menus "$WHO"); $(facts "$WHO"))"
COMBAT=$(field "$(dc "$WHO" 'combat 14')" inCombat)
rkey "$WHO" $VK_ENTER; TN=$(now_ms)
say "$WHO: the memory's box up over the down at +$BOX_AT s; read $HOLD s and answered No with a real Enter at +$(since "$T0") s (in combat: $COMBAT)"

# ---- watched: up at once with the game's own health, or not ------------------------------------------------------------------------
# Read as fast as the dev channel answers (the box only every fourth read): the stand-up lies between the last read down
# (DOWN_LAST) and the first read up (UP_AT).
UP_AT=""; UP_HP=""; DOWN_LAST=0; last=""; AGAIN=0; n=0
while [ "$(awk -v a="$TN" -v b="$(now_ms)" -v w="$WATCH" 'BEGIN { print (b - a < w * 1000) }')" = 1 ]; do
	f=$(flags "$WHO"); l=$(field "$f" lifeState); h=$(field "$f" health); t=$(since "$TN")
	if [ "$l" != "$last" ]; then say "$WHO at +$t s after the No: life state $l, health $h, niFlags $(field "$f" niFlags), boolFlags $(field "$f" boolFlags), in combat $(field "$(dc "$WHO" 'combat 14')" inCombat)"; last=$l; fi
	case "$l" in
	0) [ -z "$UP_AT" ] && { UP_AT=$t; UP_HP=${h%.*}; } ;;
	7) UP_AT=""; UP_HP=""; DOWN_LAST=$t ;;
	1|2) break ;;
	esac
	# up, and stayed so for 2 s: the stand-up is the one that counts
	[ -n "$UP_AT" ] && [ "$(awk -v u="$UP_AT" -v s="$(since "$TN")" 'BEGIN { print (s - u >= 2) }')" = 1 ] && break
	n=$((n + 1)); [ $((n % 4)) = 0 ] && [ -z "$UP_AT" ] && if box_up; then AGAIN=$((AGAIN + 1)); say "$WHO: a box over the down again at +$(since "$TN") s after the No: '$(rmsgbox "$WHO")': answered No (Enter)"; rkey "$WHO" $VK_ENTER; fi
	sleep 0.1
done
BOX=$(rsince "$WHO" hit | grep 'gfx: root.Menu_mc.Body_tf' | head -1 | grep -oE "text='[^']*'" | sed "s/^text='//; s/'$//")
say "$WHO: the box read '$BOX'"
grep -qi 'simulation' <<<"$BOX" || say "$WHO: the box does not name the simulation: read it above"
END=$(facts "$WHO")
say "$WHO at the end of the watch: $END; menus open: $(menus "$WHO")"
say "$WHO's log since the hit: $(rsince "$WHO" hit | grep -E 'Downed: |Save: loading' | cut -c16-220 | tail -8 | paste -sd'|' -)"
L=$(life "$WHO"); KEPT1=$(field "$(dc "$WHO" rules)" keptUps)
finish

WANT=$((FULL * HEALTH_SHARE / 100)); GAME_TIME=10; [ "$COMBAT" = true ] && GAME_TIME=20; WITHIN=$((GAME_TIME + UP_WITHIN))
OURS=$(( ${KEPT1:-0} - ${KEPT0:-0} ))
case "$L" in 1|2) verdict YES "$WHO died after answering No ($END)" ;; esac
[ -n "$UP_AT" ] || verdict YES "$WHO, down inside DiMA's memory, answered its box No and was not up $WATCH s after it (box at +$BOX_AT s; boxes again: $AGAIN; $END)"
[ "$OURS" -gt 0 ] && verdict YES "$WHO answered the memory's box No and was stood up by downed mode, not the game, between +$DOWN_LAST and +$UP_AT s after it, not the game (keptUps +$OURS; health $UP_HP of $FULL; unmodded: the game's own stand-up ~10.5 s after the No, full health)"
OK_TIME=$(awk -v u="$UP_AT" -v w="$WITHIN" 'BEGIN { print (u <= w) }')
[ "$OK_TIME" = 1 ] || verdict YES "$WHO answered the memory's box No and was up only between +$DOWN_LAST and +$UP_AT s after it, past the game's own $GAME_TIME s and $UP_WITHIN s (health $UP_HP)"
is_int "$UP_HP" && [ "$UP_HP" -ge "$WANT" ] || verdict YES "$WHO answered the memory's box No and was up +$UP_AT s after it, but with health $UP_HP of $FULL (unmodded: full health)"
verdict NO "$WHO, down inside DiMA's memory, read its box $HOLD s and answered No; the game stood them up between +$DOWN_LAST and +$UP_AT s after it (its own $GAME_TIME s) with health $UP_HP of $FULL (box at +$BOX_AT s; keptUps +$OURS${UP_BEFORE:+; up before the box at +$UP_BEFORE s})"
