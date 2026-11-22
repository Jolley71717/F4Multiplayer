#!/bin/bash
# standin-invisible-travel.sh [rounds] [refA] [refB]: a player's stand-in invisible to the other after both fast-travelled to the
# same place at the same moment (a player's report, 0.20.1: three times in 25-30 h of play, the stand-in on the compass and acting on
# the world but not drawn; a disconnect and a reload did not bring it back, a restart did). Both players fast travel to one spot at
# the same moment, there and back, ROUNDS times (default 12), by the game's own fast travel (console cgf "Game.FastTravel" <ref>: the
# travel's loading screen, time passing and its autosave, as the map's; said so). After each arrival, each game is asked about its
# stand-in of the other: dev `net status` (puppet=), `refinfo <puppet>` (3d=, its 3D's place against its own, disabled, cell) and the
# weapon's shapes (dev `gunparts`: every shape "(culled)" is a stand-in whose whole 3D is culled), for up to WAIT s, and the log's
# RemotePlayers lines since the travel (spawned, placed again, no head). On a build with dev `standin` (after this fix) its answer
# is read too (culled=, fade=, children=).
# Console set-up, said so: the fast travels themselves (cgf), and both players put at the start (console moveto). Nothing else moves them.
# With MAP=1 the travels are made on the Pip-Boy's own map with real input instead (see map_pick below): both travel to the marker of
# the place they stand at, at the same moment; when the map cannot be driven by keys here, the run says so and goes on by cgf.
# REPRO: YES when, after an arrival, a game has a stand-in for the other for WAIT s with no 3D, or with its 3D away from where it
# stands, or culled, or no stand-in at all while both stand within 2000 u in the same space; NO when every arrival has both
# stand-ins drawn; VOID when the travels could not be made.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
ROUNDS=${1:-12}; A=${2:-000250FE}; B=${3:-00054BAE}   # Sanctuary's workshop, Red Rocket's workshop (both refs in both games by dev edid and refinfo)
WAIT=${WAIT:-25}; TOL=${TOL:-60}
need_session
for r in "$A" "$B"; do e=$(dc host "edid $r"); case "$e" in "'"*) ;; *) verdict VOID "ref $r is no form in the host's game: $e";; esac; say "destination $r: $e"; done

# standin_of <who>: the puppet that game has for the other player, or empty
standin_of() { dc "$1" 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | head -1 | cut -d= -f2; }
# look <who> <puppet>: one look at that stand-in: "3d=<bool> off=<u between its 3D and itself, or -> disabled=<bool> culled=<n/m shapes> standin='<dev standin, if any>'"
look() {
	local ri off culled st n m
	ri=$(dc "$1" "refinfo $2")
	case "$ri" in error*) echo "gone"; return;; esac
	if [ "$(field "$ri" 3d)" = true ]; then
		off=$(awk -v x="$(field "$ri" x)" -v y="$(field "$ri" y)" -v z="$(field "$ri" z)" -v a="$(field "$ri" 3dx)" -v b="$(field "$ri" 3dy)" -v c="$(field "$ri" 3dz)" 'BEGIN{printf "%d", sqrt((x-a)^2+(y-b)^2+(z-c)^2)}')
	else off=-; fi
	culled=$(dc "$1" "gunparts $2"); case "$culled" in *parts=*) n=$(grep -o '(culled)' <<<"$culled" | wc -l); m=$(grep -oE 'shapes=[0-9]+' <<<"$culled" | head -1 | cut -d= -f2); culled="$n/${m:-?}";; *) culled=-;; esac
	st=$(dc "$1" "standin $2"); case "$st" in error*|"") st="";; esac
	echo "3d=$(field "$ri" 3d) off=$off disabled=$(field "$ri" disabled) cell=$(field "$ri" cell) culled=$culled standin='$st'"
}
# dist_players: how far apart the two players are (both games' own positions), or empty when they are not in one space
dist_players() {
	local h v; h=$(dc host status); v=$(dc vm status)
	[ "$(field "$h" cell)" = "$(field "$v" cell)" ] || [ "$(field "$h" worldspace)" = "$(field "$v" worldspace)" ] || { echo ""; return; }
	dist_xy "$(pos_of host)" "$(pos_of vm)"
}
# invisible <look>: whether a look says the stand-in is there and not drawn
invisible() { case "$1" in gone) return 1;; esac; [ "$(field "$1" 3d)" != true ] && return 0; [ "$(field "$1" off)" != - ] && [ "$(field "$1" off)" -gt "$TOL" ] && return 0; case "$(field "$1" culled)" in -|0/*) ;; */*) return 0;; esac; case "$1" in *"culled=true"*|*"fade=0.00"*) return 0;; esac; return 1; }

# The Pip-Boy's own map (MAP=1): map_pick <who> opens the Pip-Boy on its map (the game's Quick Map key, F4: a real key), then presses
# the pad's A: on the map the pad's crosshair is the middle of the map, which opens on the player, so the marker of the place the
# player stands at is picked and the game asks "Fast travel to ...?" (MessageBoxMenu). map_go <who> answers with the pad's A. Both
# travel to the marker of where they stand: the same place at the same moment. No picture of the screen is read: `menus` says
# whether the Pip-Boy, the question and the loading screen came.
map_pick() {
	rkey "$1" 0x73 80
	wait_for 6 menu_open "$1" PipboyMenu || { say "  map ($1): no PipboyMenu after F4 ($(menus "$1"))"; return 1; }
	sleep 2.5
	dc "$1" "pad button a" >/dev/null
	wait_for 4 menu_open "$1" MessageBoxMenu || { say "  map ($1): the map did not ask to travel after the pad's A ($(menus "$1"))"; return 1; }
}
map_go() { dc "$1" "pad button a" >/dev/null; }
map_close() { local w i; for w in host vm; do for i in 1 2 3; do menu_open "$w" MessageBoxMenu && { dc "$w" "pad button b" >/dev/null; sleep 1; }; menu_open "$w" PipboyMenu && { rkey "$w" 0x09 60; sleep 2.5; }; done; dc "$w" "pad unplug" >/dev/null; done; }
MAP=${MAP:-0}; MAPPED=0; CGF=0; NEARWARN=0
BAD=0; ROWS=0
say "standin-invisible-travel: $ROUNDS round(s) of both players fast-travelling (cgf) between $A and $B at the same moment; each arrival watched $WAIT s"
say "set-up: both players put at the first spot (console coc is the join's; here console moveto $A, said so)"
for w in host vm; do dc "$w" "console player.moveto $A" >/dev/null; done; sleep 6
from=$A; to=$B; rmark host all; rmark vm all
for round in $(seq 1 "$ROUNDS"); do
	rmark host r$round; rmark vm r$round
	t0=$(date +%s%3N)
	how=cgf
	if [ "$MAP" = 1 ]; then
		p0h=$(pos_of host); p0v=$(pos_of vm)
		if map_pick host && map_pick vm; then
			# the two answers in one moment
			( map_go host ) & ( map_go vm ) & wait
			if wait_for 8 menu_open host LoadingMenu || [ "$(dist_xy "$p0h" "$(pos_of host)")" -gt 50 ]; then how=map; MAPPED=$((MAPPED + 1)); else say "round $round: no loading screen on the host after the map's question was answered ($(menus host))"; fi
		fi
		[ $how = map ] || { map_close; MAP=0; say "round $round: the Pip-Boy's map could not be driven by keys here; this and the later rounds travel by console cgf \"Game.FastTravel\" (the same travel, called without the map; said so)"; }
		[ $how = map ] && for w in host vm; do dc "$w" "pad unplug" >/dev/null; done
	fi
	if [ $how = cgf ]; then
		CGF=$((CGF + 1))
		# the two travels in one moment: each game's console call in its own shell, started together
		( dc host "console cgf \"Game.FastTravel\" $to" >/dev/null ) &
		( dc vm "console cgf \"Game.FastTravel\" $to" >/dev/null ) &
		wait
	fi
	sleep 4
	wait_ingame host 60 && wait_ingame vm 60 || verdict VOID "a game did not come back in game after the travel in round $round"
	# both there?
	ok=0; for i in $(seq 1 30); do d=$(dist_players); [ -n "$d" ] && [ "$d" -lt 2000 ] && { ok=1; break; }; sleep 1; done
	[ $ok = 1 ] || { say "round $round: the players are not together after the travel to $to (dist '$(dist_players)'): the travel did not take; round skipped"; from=$to; to=$([ "$to" = "$A" ] && echo "$B" || echo "$A"); continue; }
	say "round $round: both $([ $how = map ] && echo "at the marker of where they stood (the Pip-Boy's map, real keys)" || echo "at $to (console cgf)") $(( ($(date +%s%3N) - t0) / 1000 )) s after the call, $(dist_players) u apart"
	# each game's stand-in of the other, watched WAIT s: the last look counts, the first drawn look ends the watch early
	for w in host vm; do
		last=""; p=""; drawn=0
		for i in $(seq 1 "$WAIT"); do
			p=$(standin_of "$w"); [ -z "$p" ] && { last="none"; sleep 1; continue; }
			last=$(look "$w" "$p")
			if ! invisible "$last" && [ "$last" != gone ]; then drawn=$i; break; fi
			sleep 1
		done
		ROWS=$((ROWS + 1))
		lines=$(rsince "$w" r$round | grep -E "RemotePlayers: (spawned|player [0-9]+ stand-in|player [0-9]+ is (near us|shown again))|Face: |FaceWatch" | cut -c1-160 | tail -4 | tr '\n' '|')
		if [ "$last" = none ]; then
			BAD=$((BAD + 1)); say "  $w: NO STAND-IN for the other after $WAIT s, both within $(dist_players) u ($lines)"
		elif [ $drawn = 0 ]; then
			BAD=$((BAD + 1)); say "  $w: stand-in $p NOT DRAWN after $WAIT s: $last ($lines)"
		else
			say "  $w: stand-in $p drawn after $drawn s: $last"
		fi
		echo "round $round $w $p drawn=$drawn $last | $lines" >> "$REPRO_OUT/looks.log"
	done
	from=$to; to=$([ "$to" = "$A" ] && echo "$B" || echo "$A")
done
# a build that looks for a friend near us and not shown says so itself: its lines over the whole run
for w in host vm; do n=$(rsince "$w" all | grep -cE "RemotePlayers: player [0-9]+ is near us and (still )?not shown"); m=$(rsince "$w" all | grep -c "is shown again, after"); [ "$n" -gt 0 ] && say "the $w said a friend near and not shown $n time(s), shown again $m time(s): $(rsince "$w" all | grep -E "is near us and|is shown again" | cut -c1-220 | head -4 | tr '\n' '|')"; NEARWARN=$((NEARWARN + n)); done
rkeep host standin-invisible-travel; rkeep vm standin-invisible-travel
say "looks: $ROWS, not drawn: $BAD (looks.log has each); travels by the Pip-Boy's map: $MAPPED, by console cgf: $CGF"
[ "$ROWS" -gt 0 ] || verdict VOID "no arrival could be watched"
[ "$BAD" -gt 0 ] && verdict YES "a stand-in was there and not drawn (or none stood) after $BAD of $ROWS arrivals"
[ "$NEARWARN" -gt 0 ] && verdict YES "the build itself found a friend near and not shown $NEARWARN time(s) (and may have mended it: see the lines above)"
verdict NO "both stand-ins were drawn after every one of $ROWS arrivals"
