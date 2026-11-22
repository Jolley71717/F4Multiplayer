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
#
# A run that tests nothing ends early, as VOID (never a pass), so it gives the rig back (see "runs that test nothing" at the end):
#   scene_live <label> <seconds> <check...>   what must happen early for the run to mean anything (an NPC in combat with the
#                                             stand-in, the friend hit, the cell changed after a door): VOID at once when the
#                                             check has not succeeded in that time, with the last log lines of both games
#   idle_watch <regex> <seconds>              opt-in: VOID (stalled) when neither game's plugin log has a line matching the regex
#                                             for that long; `idle_watch off` before a quiet stretch that is meant to be quiet
#   run_cap <seconds>                         the run's time cap (REPRO_CAP, 3600 s by default; every run has one): VOID past it
#   on_end <command...>                       an end step (god mode off, a log level back): run once however the run ends, a
#                                             verdict, a watchdog, a time cap, Ctrl-C or an error. Use it, not a trap of your own
# A run ended this way runs its end steps (and stops a walk it left going), and exits 2 like any VOID; rig.lock is never touched
# here, so its holder frees it as after any run.
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
	_REPRO_WALKING="$_REPRO_WALKING $who"   # a run ended mid-walk stops this walk (_repro_end_steps)
	if [ $# -ge 2 ] && is_int "${1%.*}" && is_int "${2%.*}"; then WALKED=$(bash "$RIG/walkto.sh" "$who" "$1" "$2" "${4:-0.8}" "${3:-120}" 40 2>&1 | tail -1); else WALKED=$(bash "$RIG/walkto.sh" "$who" "$1" "${3:-0.8}" "${2:-120}" 40 2>&1 | tail -1); fi
	dc "$who" "pad unplug" >/dev/null; _REPRO_WALKING=${_REPRO_WALKING/ $who/}
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
# verdict YES|NO|VOID <why>: the last line of every repro script. The run's end steps (on_end) run first and its watchers stop
verdict() { _repro_finish; say "REPRO: $1 - $2"; say "logs and notes: $REPRO_OUT"; [ "$1" = VOID ] && exit 2; exit 0; }
# need_session: both games answer and are joined; VOID otherwise
need_session() {
	local h v; h=$(dc host 'net status'); v=$(dc vm 'net status')
	grep -q 'status=joined' <<<"$v" && grep -q 'status=' <<<"$h" || verdict VOID "both games in one session first (rig.sh join): host '$(grep -oE 'status=[a-z]+' <<<"$h")' vm '$(grep -oE 'status=[a-z]+' <<<"$v")'"
}

# ---- runs that test nothing end early: the scene gate, the idle watch, the time cap --------------------------------------------
# On 2026-11-21 a hits repro ran three times, minutes each, and tested nothing: the raiders fought the host's player instead of
# the friend's stand-in; a `tdetect` had switched every NPC's AI off; the players and a raider stood idle. A door case "passed"
# through a door that was no load door. So a run says what must happen for it to mean anything, and ends as VOID the moment it
# can't: VOID is the rig's, never a pass, and the rig goes back to the queue minutes earlier.
#   scene_live <label> <seconds> <check...>   the check (any command; 0 = yes) tried every 0.5 s (wait_for) for that long. When it
#                                             succeeds: "scene live: <label>" and the run goes on. When it never does: VOID
#                                             "scene not live: <label>", with the last log lines of both games (repro_tails).
#                                             Put it right after what should start the scene, before the run measures anything:
#                                               _fights() { [ "$(field "$(dc vm "combat $RAIDER")" target)" = "$PUPPET" ]; }
#                                               scene_live "the raider is in combat with the friend's stand-in" 20 _fights
#                                               scene_live "the player's cell changed after the door" 30 _cell_changed
#   idle_watch <regex> <seconds>              opt-in, in the background: VOID "stalled" when no line matching the extended
#                                             regex came into either game's plugin log for that long, e.g.
#                                               idle_watch 'CopyWatch: hit on|took .* damage|PlayerHit' 120
#                                             Calling it again replaces it; `idle_watch off` stops it (a load or a long walk that
#                                             is meant to log nothing). The logs are read as they are, never through the dev
#                                             channel (a game's channel serves one client: the watch must not take it from the
#                                             script): the host's file, the VM's over ssh, the last 400 lines every REPRO_POLL s.
#   run_cap <seconds>                         the run's time cap, counted from its start: VOID "time cap" past it. Every run has
#                                             one (3600 s by default; a script that runs longer sets its own, about twice its longest good run).
#                                             An exported REPRO_CAP wins over both. No script holds the rig for ever.
#   on_end <command...>                       an end step, run once however the run ends; the last one given runs first.
# A watcher that fires leaves its reason in $REPRO_OUT/.watch, signals the script (USR1) and stops what the script is running (a
# sleep, a dev command, a walk), so the script's trap gives the VOID at once: the end steps run (a walk left going is stopped and
# its pad unplugged first), the tails are said, and the script exits 2. The watchers die with the script however it ends. rig.lock
# is not touched: its holder frees it as after any run. Test: bash tools/rig/repro/watch-selftest.sh (no game, no VM).
REPRO_PID=$$
_REPRO_CAP_SET=${REPRO_CAP:+1}              # a cap the caller exported wins over the script's own run_cap
REPRO_CAP=${REPRO_CAP:-3600}                # the run's time cap, s (run_cap sets the script's own)
REPRO_POLL=${REPRO_POLL:-10}                # how often the watchers look, s
REPRO_TAIL=${REPRO_TAIL:-20}                # log lines of each game said with a VOID of these
REPRO_TAIL_RE=${REPRO_TAIL_RE:-}            # scene_live's tail: only the lines matching this (empty: any) ...
REPRO_TAIL_SKIP=${REPRO_TAIL_SKIP:-'gfx: '} # ... and never these (rgfx_text's display-object dumps)
_RW=$REPRO_OUT/.watch; mkdir -p "$_RW"; rm -rf "${_RW:?}/fired" "${_RW:?}/ending" "${_RW:?}"/*.pid
printf '%(%s)T\n' -1 > "$_RW/start"; echo "$REPRO_CAP" > "$_RW/cap"

# _rtail <who> <lines>: the last lines of that game's plugin log, read as they are (no dev command). REPRO_LOG_HOST / REPRO_LOG_VM:
# other files to read instead (the self-test's)
_rtail() {
	if [ "$1" = host ]; then tail -n "$2" "${REPRO_LOG_HOST:-$HOST_F4SE/F4Multiplayer.log}" 2>/dev/null
	elif [ -n "${REPRO_LOG_VM:-}" ]; then tail -n "$2" "$REPRO_LOG_VM" 2>/dev/null
	else vmssh "Get-Content \"\$env:USERPROFILE\\Documents\\My Games\\Fallout4\\F4SE\\F4Multiplayer.log\" -Tail $2"; fi | tr -d '\r'
}
# repro_tails [regex]: the last REPRO_TAIL lines of both games' plugin logs (those matching the regex, else REPRO_TAIL_RE; never
# REPRO_TAIL_SKIP's), said and kept as $REPRO_OUT/void-<who>.log
repro_tails() {
	local w f l re=${1:-$REPRO_TAIL_RE}
	for w in host vm; do
		f=$REPRO_OUT/void-$w.log
		_rtail "$w" 1000 | SKIP=$REPRO_TAIL_SKIP RE=$re N=$REPRO_TAIL awk '
			(ENVIRON["SKIP"] == "" || $0 !~ ENVIRON["SKIP"]) && (ENVIRON["RE"] == "" || $0 ~ ENVIRON["RE"]) { l[++n] = $0 }
			END { for (i = (n > ENVIRON["N"] + 0 ? n - ENVIRON["N"] + 1 : 1); i <= n; i++) print l[i] }' > "$f"
		mapfile -t l < "$f"
		say "the last ${#l[@]} line(s) of the $w's plugin log${re:+ matching '$re'}:"
		[ ${#l[@]} -gt 0 ] && printf "    $w| %s\n" "${l[@]}" | tee -a "$REPRO_OUT/run.log"
	done
	return 0
}

scene_live() {
	local label=$1 secs=$2 t0=$SECONDS; shift 2
	{ is_int "$secs" && [ $# -gt 0 ]; } || verdict VOID "usage: scene_live <label> <seconds> <check command...> (got '$label' '$secs')"
	if wait_for "$secs" "$@"; then say "scene live: $label (after $((SECONDS - t0)) s)"; return 0; fi
	say "scene not live: $label: the check ($*) did not succeed in $secs s"
	repro_tails
	verdict VOID "scene not live: $label (not within $secs s), so the run tested nothing"
}

run_cap() {
	{ is_int "$1" && [ "$1" -gt 0 ]; } || { say "run_cap: '$1' is not a number of seconds; the cap stays $(cat "$_RW/cap") s"; return 1; }
	[ -n "$_REPRO_CAP_SET" ] && { say "time cap: REPRO_CAP=$REPRO_CAP from the caller, not the script's $1 s"; return 0; }
	echo "$1" > "$_RW/cap"; say "time cap: $1 s from the run's start"
}

idle_watch() {
	if [ "$1" = off ]; then _watch_stop idle; say "idle watch off"; return 0; fi
	{ [ -n "$1" ] && is_int "$2" && [ "$2" -gt 0 ]; } || { say "usage: idle_watch <regex> <seconds> | idle_watch off"; return 1; }
	say "idle watch: VOID (stalled) when neither game's plugin log has a line matching '$1' for $2 s"
	_watch_start idle _idle_watch "$1" "$2"
}

# on_end <command...>: an end step; _repro_end_steps runs them once (a walk left going first, then the last given first)
_REPRO_END=(); _REPRO_WALKING=""
on_end() { local q; printf -v q '%q ' "$@"; _REPRO_END+=("$q"); }
_repro_end_steps() {
	local w i
	for w in $(tr ' ' '\n' <<<"$_REPRO_WALKING" | sort -u); do
		dc "$w" "walkto stop" >/dev/null; dc "$w" "pad unplug" >/dev/null; say "end: $w's walk stopped and its pad unplugged (the run ended mid-walk)"
	done
	for ((i = ${#_REPRO_END[@]} - 1; i >= 0; i--)); do
		say "end step: ${_REPRO_END[i]}"; eval "${_REPRO_END[i]}" || say "end step failed: ${_REPRO_END[i]}"
	done
}
# _repro_finish: once, in the script's own shell (a verdict in a subshell only ends that subshell, as it always did)
_repro_finish() {
	{ [ "$BASHPID" = "$REPRO_PID" ] && [ -z "$_REPRO_ENDING" ]; } || return 0
	_REPRO_ENDING=1; : > "$_RW/ending"
	_watch_stop_all
	_repro_end_steps
}

# ---- the watchers (background subshells; their pids in $_RW/<name>.pid) ----
_watch_start() { local n=$1; shift; _watch_stop "$n"; "$@" & echo $! > "$_RW/$n.pid"; }
_watch_stop() { local p; p=$(cat "$_RW/$1.pid" 2>/dev/null) || return 0; rm -f "${_RW:?}/$1.pid"; _kill_tree "$p"; }
_watch_stop_all() { local f; for f in "$_RW"/*.pid; do [ -e "$f" ] && _watch_stop "$(basename "$f" .pid)"; done; return 0; }
# _ps_pairs: "pid ppid" of every process (procps' ps, or Git Bash's, whose first column can be a state letter)
_ps_pairs() { ps -e -o pid= -o ppid= 2>/dev/null || ps -e 2>/dev/null | awk 'NR > 1 { i = ($1 ~ /^[0-9]+$/) ? 1 : 2; print $i, $(i + 1) }'; }
# _descendants <pid> [pid...]: the processes under that one, one a line; the other pids given (and what is under them) left out
_descendants() {
	local root=$1; shift
	_ps_pairs | awk -v root="$root" -v skip=" $* " '
		{ kids[$2] = kids[$2] " " $1 }
		END { q[1] = root; n = 1
			for (i = 1; i <= n; i++) { c = split(kids[q[i]], k, " ")
				for (j = 1; j <= c; j++) if (index(skip, " " k[j] " ") == 0 && !(k[j] in seen) && k[j] != root) { seen[k[j]] = 1; q[++n] = k[j]; print k[j] } } }'
}
_kill_tree() { local p; for p in $(_descendants "$1") "$1"; do kill "$p" 2>/dev/null; done; return 0; }
# _watch_fire <why> [tail regex]: (in a watcher) the run ends: the reason left for the script, the script signalled, what it runs
# now stopped (the watchers left out) so its trap runs at once. The first watcher to fire wins; none fires once the script ends
_watch_fire() {
	[ -e "$_RW/ending" ] && exit 0
	mkdir "$_RW/fired" 2>/dev/null || exit 0
	printf '%s\n' "$1" > "$_RW/fired/why"; printf '%s\n' "${2:-}" > "$_RW/fired/re"
	local f skip="" kids
	for f in "$_RW"/*.pid; do [ -e "$f" ] && skip="$skip $(cat "$f")"; done
	kids=$(_descendants "$REPRO_PID" $skip)
	# the walk above can take tens of seconds on Git Bash: a script that reached its own verdict meanwhile is running its end steps
	# now, and those must not be killed
	[ -e "$_RW/ending" ] && exit 0
	kill -USR1 "$REPRO_PID" 2>/dev/null
	for f in $kids; do kill "$f" 2>/dev/null; done
	exit 0
}
_cap_watch() {
	trap - EXIT INT TERM USR1
	local t0 cap now; read -r t0 < "$_RW/start"
	while kill -0 "$REPRO_PID" 2>/dev/null; do
		cap=""; read -r cap < "$_RW/cap"; printf -v now '%(%s)T' -1
		is_int "$cap" && [ $((now - t0)) -ge "$cap" ] && _watch_fire "time cap: the run took $((now - t0)) s, past its cap of $cap s (run_cap, REPRO_CAP)"
		sleep "$REPRO_POLL"
	done
}
# _idle_watch <regex> <seconds>: each look, the lines each log got since the last look (those after the last look's last line; all
# 400 when it is gone: a new log, or more than 400 lines) are searched for the regex (awk's extended regex, as grep -E's). Few forks a
# look: the rig's host is the maintainer's PC, often busy with builds
_idle_watch() {
	trap - EXIT INT TERM USR1
	local re=$1 secs=$2 w t m now last seen="" miss=0 looks=0 quiet
	local -A prev
	printf -v last '%(%s)T' -1; for w in host vm; do prev[$w]=$(_rtail "$w" 1); done
	while kill -0 "$REPRO_PID" 2>/dev/null; do
		sleep "$REPRO_POLL"; looks=$((looks + 1))
		for w in host vm; do
			t=$(_rtail "$w" 400)
			[ -n "$t" ] || { miss=$((miss + 1)); continue; }
			# the last new line that matches, or nothing
			m=$(P=${prev[$w]} RE=$re awk '{ l[NR] = $0; if ($0 == ENVIRON["P"]) at = NR } END { for (i = NR; i > at; i--) if (l[i] ~ ENVIRON["RE"]) { print l[i]; exit } }' <<<"$t")
			prev[$w]=${t##*$'\n'}
			[ -n "$m" ] && { printf -v last '%(%s)T' -1; seen="$w: $m"; }
		done
		printf -v now '%(%s)T' -1; quiet=$((now - last))
		if [ "$quiet" -ge "$secs" ]; then
			[ "$miss" -gt 0 ] && miss="; a log could not be read $miss time(s) in $looks looks" || miss=""
			_watch_fire "stalled: no line matching '$re' in either game's plugin log for $quiet s (the limit: $secs s)$miss; the last: ${seen:-none since the watch began}"
		fi
	done
}

_repro_fired() {
	[ -n "$_REPRO_ENDING" ] && return 0
	local why re; why=$(cat "$_RW/fired/why" 2>/dev/null); re=$(cat "$_RW/fired/re" 2>/dev/null); why=${why:-a watcher ended the run}
	say "watchdog: $why"
	repro_tails "$re"
	verdict VOID "$why"
}
_repro_sig() { [ -n "$_REPRO_ENDING" ] && exit 2; verdict VOID "the run was stopped (SIG$1) before its verdict"; }
trap _repro_fired USR1
trap '_repro_sig INT' INT
trap '_repro_sig TERM' TERM
trap _repro_finish EXIT
_watch_start cap _cap_watch
