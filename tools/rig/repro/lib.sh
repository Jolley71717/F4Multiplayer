#!/bin/bash
# lib.sh: what the repro scripts in this folder share (sourced, never run). Each script reproduces one reported bug on the two-game
# rig (the host PC's game and the VM's, joined over Steam, both dev channels on) with real input where a player would use it, and
# ends in one line from the plugin logs and the dev channel: "REPRO: YES" (the bug happened), "REPRO: NO" (it did not: after a fix),
# or "REPRO: VOID" (the scene could not be reached: the rig's, not a verdict). Run it before and after a fix.
#
# Rig rules (AGENTS.md): take rig.lock first and back up the ini and session files (tools/rig/backup.sh); no -ExecutionPolicy flags.
# Console steps used to reach a scene (coc, setpos, a settlement's owner, a quest stage) are named in the output where they happen.
# Movement is real (tools/rig/walkto.sh: the virtual pad's stick); keys are real key presses into the game's window (dev `key`);
# turning is the dev channel's `turnto` (a look, as the rig's other steps aim) or a real mouse move.
RIG=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$RIG/env.sh"
REPRO_NAME=${REPRO_NAME:-$(basename "$0" .sh)}
REPRO_OUT=${REPRO_OUT:-$CHECKS/repro-$REPRO_NAME-$(date +%m%d-%H%M%S)}
mkdir -p "$REPRO_OUT"
VK_E=0x45; VK_ENTER=0x0D; VK_TAB=0x09; VK_LEFT=0x25; VK_UP=0x26; VK_RIGHT=0x27; VK_DOWN=0x28; VK_SPACE=0x20

# say <text>: a line of the run's output, also kept in $REPRO_OUT/run.log
say() { echo "$(date +%T) $*" | tee -a "$REPRO_OUT/run.log"; }
# field <text> <name>: the value of name=value in a dev-channel reply (as assert.sh's)
field() { grep -oE "(^|[] [(;|,:])$2=[^] ;|,)]*" <<<"$1" | head -1 | sed 's/^[^=]*=//'; }
is_int() { [[ $1 =~ ^-?[0-9]+$ ]]; }
# wait_for <seconds> <command...>: until the command succeeds, every 0.5 s
wait_for() { local end=$((SECONDS + $1)); shift; while :; do "$@" && return 0; [ $SECONDS -ge "$end" ] && return 1; sleep 0.5; done; }
wait_ingame() { local i; for i in $(seq 1 "${2:-60}"); do dc "$1" status | grep -q 'ingame=true' && return 0; sleep 2; done; return 1; }
other_game() { [ "$1" = host ] && echo vm || echo host; }

# ---- the plugin logs ------------------------------------------------------------------------------------------------------------
# rlog <who> [lines]: that game's plugin log (all of it, or its last lines), flushed first
rlog() {
	dc "$1" "log flush" >/dev/null
	if [ "$1" = host ]; then
		if [ -n "$2" ]; then tail -n "$2" "$HOST_F4SE/F4Multiplayer.log"; else cat "$HOST_F4SE/F4Multiplayer.log"; fi
	else
		vmssh "Get-Content \"\$env:USERPROFILE\\Documents\\My Games\\Fallout4\\F4SE\\F4Multiplayer.log\"${2:+ -Tail $2}"
	fi | tr -d '\r'
}
# rmark <who> [tag]: remember how long that game's log is now; rsince <who> [tag]: the lines written since (a log that got shorter
# is a new game's: all of it)
rmark() { rlog "$1" | wc -l > "$REPRO_OUT/mark-$1${2:+-$2}"; }
rsince() {
	local m; m=$(cat "$REPRO_OUT/mark-$1${2:+-$2}" 2>/dev/null || echo 0)
	rlog "$1" | awk -v m="$m" 'NR == 1 { all = $0 } { l[NR] = $0 } END { s = (NR < m) ? 1 : m + 1; for (i = s; i <= NR; i++) print l[i] }'
}
# rkeep <who> <name>: that game's whole log kept as $REPRO_OUT/<name>-<who>.log
rkeep() { rlog "$1" > "$REPRO_OUT/$2-$1.log"; }

# ---- what the player sees (read from the menus' display objects: dev `gfx`, which writes them to the plugin log) ---------------
# rgfx_text <who> <menu> <path> <depth> <regex on the object's path>: the texts of the matching display objects, one a line
rgfx_text() {
	dc "$1" "gfx $2 $3 $4" >/dev/null
	rlog "$1" 400 | grep "gfx: $3" | grep -E "gfx: [^ ]*$5[^ ]* " | grep -oE "visible=[a-z]+ .*text='.*' color=" | sed -E "s/visible=([a-z]+) .*text='/\1 /; s/' color=$//" | tail -40
}
# rhud <who>: the HUD's messages now (the notifications at the top left), visible ones only, "|"-joined
rhud() { rgfx_text "$1" HUDMenu root.HUDNotificationsGroup_mc.Messages_mc 4 MessageText_tf | awk '$1 == "true" { $1 = ""; print substr($0, 2) }' | sort -u | paste -sd'|' -; }
# rmsgbox <who>: the open message box's body text, or empty
rmsgbox() { menu_open "$1" MessageBoxMenu || return 0; rgfx_text "$1" MessageBoxMenu root.Menu_mc 1 Body_tf | tail -1 | cut -d' ' -f2-; }
# rrollover <who>: what the crosshair's rollover names, when it shows ("" when nothing is under the crosshair)
rrollover() { rgfx_text "$1" HUDMenu root.CenterGroup_mc.RolloverWidget_mc 1 RolloverName_tf | tail -1 | awk '$1 == "true" { $1 = ""; print substr($0, 2) }'; }
menu_open() { dc "$1" menus | grep -qE "open=([^ ]*,)?$2:"; }
menus() { dc "$1" menus | grep -oE 'open=[^ ]*' | sed 's/^open=//'; }

# ---- real input ------------------------------------------------------------------------------------------------------------------
# rkey <who> <vk> [ms]: a real key press into that game's window (brought to the front first)
rkey() { dc "$1" focus >/dev/null; sleep 0.3; dc "$1" "key $2 ${3:-80}" >/dev/null; }
# rwalk <who> <ref | x y> [radius] [speed]: that player walks there (the pad's stick); 0 = arrived (or as near as the walk got: WALKED)
rwalk() {
	local who=$1 r s; shift
	if [ $# -ge 2 ] && is_int "${1%.*}" && is_int "${2%.*}"; then WALKED=$(bash "$RIG/walkto.sh" "$who" "$1" "$2" "${4:-0.8}" "${3:-120}" 40 2>&1 | tail -1); else WALKED=$(bash "$RIG/walkto.sh" "$who" "$1" "${3:-0.8}" "${2:-120}" 40 2>&1 | tail -1); fi
	dc "$who" "pad unplug" >/dev/null
	grep -qE ': (arrived|stuck) ' <<<"$WALKED"
}
# rpress_e <who> <ref>: that player looks at the reference (from the eye to the middle of its 3D) and presses E. ROLLOVER: what the
# crosshair named before the press
rpress_e() { dc "$1" "turnto $2 300 aim" >/dev/null; sleep 0.8; ROLLOVER=$(rrollover "$1"); rkey "$1" "$VK_E" 100; }
pos_of() { local p; p=$(dc "$1" pos); echo "$(field "$p" x) $(field "$p" y)"; }
dist_xy() { awk -v a="$1" -v b="$2" 'BEGIN{split(a,p," "); split(b,q," "); if (p[1]=="" || q[1]=="") {print ""; exit} printf "%d", sqrt((q[1]-p[1])^2+(q[2]-p[2])^2)}'; }

# ---- workshop mode's own menu, by its keys ---------------------------------------------------------------------------------------
# The build menu's path ("STRUCTURES >  WOOD >  FLOORS") and the item named, read from WorkshopMenu's display objects; the held
# ghost from dev `workshop`. Right/Left move along a level, Enter goes down a level (on a leaf level it hands the first item's
# ghost), Tab goes up, but Tab with a placed piece highlighted asks to store it: so the player looks up at the sky first.
ws_state() { dc "$1" workshop; }
ws_path() { rgfx_text "$1" WorkshopMenu root.WorkshopBase_mc.DisplayPathBase_mc 1 DisplayPath_tf | tail -1 | cut -d' ' -f2- | sed 's/  */ /g'; }
ws_last() { ws_path "$1" | awk -F'>' '{print $NF}' | sed 's/^ *//; s/ *$//'; }
# ws_nav <who> <name>...: from the level the menu is at, each name in turn: Left to the level's start, Right until it is the
# highlighted one, Enter. 1 (WS_NAV says where) when a name is not found
ws_nav() {
	local who=$1 name i last; shift
	for name in "$@"; do
		last=$(ws_last "$who")
		if [ "$last" != "$name" ]; then for i in $(seq 1 30); do dc "$who" "key $VK_LEFT 60" >/dev/null; sleep 0.25; done; sleep 0.4; fi
		for i in $(seq 1 25); do last=$(ws_last "$who"); [ "$last" = "$name" ] && break; dc "$who" "key $VK_RIGHT 80" >/dev/null; sleep 0.6; done
		if [ "$last" != "$name" ]; then WS_NAV="'$name' not found (at '$(ws_path "$who")')"; return 1; fi
		dc "$who" "key $VK_ENTER 80" >/dev/null; sleep 1
	done
	WS_NAV="at '$(ws_path "$who")'"
}
# ws_top <who>: the player looks up (a real mouse move) and Tabs up to the menu's top level. A placed piece still highlighted (the
# highlight stays on the last piece the crosshair passed, even with the look in the sky) would take the Tab as "Store it in the
# workshop?": the player turns (mouse 400 px) until nothing is highlighted first, and a Store box that opens anyway is answered No
ws_top() {
	local who=$1 i s; dc "$who" focus >/dev/null
	menu_open "$who" MessageBoxMenu && { dc "$who" "key $VK_DOWN 80" >/dev/null; sleep 0.4; dc "$who" "key $VK_ENTER 80" >/dev/null; sleep 1; }
	dc "$who" "mouse 0 -1500" >/dev/null; sleep 0.8
	for i in 1 2 3 4 5 6 7 8 9 10; do
		case "$(ws_path "$who")" in *">"*) ;; *) return 0;; esac
		s=$(ws_state "$who")
		if [ "$(field "$s" held)" = 00000000 ] && [ "$(field "$s" selected)" != 00000000 ]; then dc "$who" "mouse 400 0" >/dev/null; sleep 0.8; continue; fi
		dc "$who" "key $VK_TAB 80" >/dev/null; sleep 1
		menu_open "$who" MessageBoxMenu && { dc "$who" "key $VK_DOWN 80" >/dev/null; sleep 0.4; dc "$who" "key $VK_ENTER 80" >/dev/null; sleep 1; }
	done
	case "$(ws_path "$who")" in *">"*) return 1;; esac
}
# pitch_down <who> <degrees>: real mouse moves (up or down, 150 px a time) until the look is about that many degrees down
pitch_down() {
	local who=$1 want=$2 i rx; dc "$who" focus >/dev/null
	for i in $(seq 1 20); do
		rx=$(field "$(dc "$who" 'refinfo 00000014')" rx); rx=${rx%.*}; is_int "$rx" || return 1
		[ "$rx" -ge $((want - 6)) ] && [ "$rx" -le $((want + 6)) ] && return 0
		if [ "$rx" -lt "$want" ]; then dc "$who" "mouse 0 150" >/dev/null; else dc "$who" "mouse 0 -150" >/dev/null; fi; sleep 0.4
	done
}
# ws_green <who>: the held ghost brought to a spot where it may be put down: at a look of 25, then 35, then 15 degrees down, the
# player turns (real mouse moves, 400 px = about 45 degrees a time, up to 9: a full turn) until the game says it may (canPlace).
# 0 = green
ws_green() {
	local who=$1 p i; dc "$who" focus >/dev/null
	for p in 25 35 15; do
		pitch_down "$who" "$p"
		for i in $(seq 1 9); do [ "$(field "$(ws_state "$who")" canPlace)" = true ] && return 0; dc "$who" "mouse 400 0" >/dev/null; sleep 0.9; done
	done
	[ "$(field "$(ws_state "$who")" canPlace)" = true ]
}
# rwalk_bench <who> <bench>: that player walks (real walk) to within reach of the workbench; when the straight line is blocked
# (Sanctuary's houses, and the pieces earlier runs put down west of the bench), by way of the open ground north of it. 0 = in reach
# (130 u). BENCH_DIST: how far it ended
rwalk_bench() {
	local who=$1 b=$2 bx by route wp wps
	read -r bx by <<<"$(field "$(dc "$who" "refinfo $b")" x) $(field "$(dc "$who" "refinfo $b")" y)"
	for route in "" "-78950 89850" "-79100 90250,-78950 89850" "-79600 90300,-79100 90250,-78950 89850"; do
		IFS=, read -ra wps <<<"$route"
		for wp in "${wps[@]}"; do rwalk "$who" $wp 80; done
		rwalk "$who" "$b" 95 0.6
		BENCH_DIST=$(dist_xy "$(pos_of "$who")" "${bx%.*} ${by%.*}")
		[ "${BENCH_DIST:-9999}" -le 130 ] && return 0
	done
	return 1
}
# ws_enter_real <who> <bench>: that player walks to the workbench and presses E; any message box over workshop mode (the first
# visit's tutorial) closed with Enter. 0 = in workshop mode. ENTER_SAID: the HUD's messages and the box's text after the E
ws_enter_real() {
	local who=$1 b=$2 i box
	rwalk_bench "$who" "$b"
	for i in 1 2 3; do
		rpress_e "$who" "$b"; sleep 1
		ENTER_SAID="hud='$(rhud "$who")'"
		if wait_for 6 _ws_in "$who"; then
			sleep 1.5; box=$(rmsgbox "$who"); [ -n "$box" ] && { ENTER_SAID="$ENTER_SAID box='$box'"; rkey "$who" $VK_ENTER; sleep 1; }
			return 0
		fi
		dc "$who" "mouse 0 200" >/dev/null; sleep 0.5
	done
	return 1
}
_ws_in() { [ "$(field "$(ws_state "$1")" menu)" = true ]; }
# ws_leave <who>: out of workshop mode: looks up, Tab to the menu's top, Tab once more (a player's way); when that leaves it up (on
# the rig a sent Tab at the top level does not end workshop mode), the dev channel's `workshop exit` (the game's own request to
# leave it, as the rig's checks use). WS_LEFT says which
ws_leave() {
	WS_LEFT="not in it"; _ws_in "$1" || return 0
	ws_top "$1"; rkey "$1" $VK_TAB; WS_LEFT="by a real Tab"
	wait_for 5 _ws_in_not "$1" && return 0
	dc "$1" "workshop exit" >/dev/null; WS_LEFT="by the dev channel's workshop exit (a real Tab at the top did not end it)"
	wait_for 10 _ws_in_not "$1"
}
_ws_in_not() { ! _ws_in "$1"; }
# ws_copy_of <who> <other game's ref>: that game's copy of the other game's piece (dev `wspower copies`), or empty
ws_copy_of() { dc "$1" "wspower copies" | grep -oE "[0-9A-F]{8}(\s\(not loaded\))?=[0-9]+:$2" | grep -oE '^[0-9A-F]{8}' | head -1; }

# ---- the verdict -----------------------------------------------------------------------------------------------------------------
# verdict YES|NO|VOID <why>: the last line of every repro script
verdict() { say "REPRO: $1 - $2"; say "logs and notes: $REPRO_OUT"; [ "$1" = VOID ] && exit 2; exit 0; }
# need_session: both games answer and are joined; VOID otherwise
need_session() {
	local h v; h=$(dc host 'net status'); v=$(dc vm 'net status')
	grep -q 'status=joined' <<<"$v" && grep -q 'status=' <<<"$h" || verdict VOID "both games in one session first (rig.sh join): host '$(grep -oE 'status=[a-z]+' <<<"$h")' vm '$(grep -oE 'status=[a-z]+' <<<"$v")'"
}
