#!/bin/bash
# menustress.sh: the menus players race in, over and over, on one game or both at once. Each cycle goes
# through workshop mode (enter, highlight the nearest junk, scrap it, leave), the Pip-Boy (open, a page,
# close) and a conversation (an NPC in front, talk, pick options, leave). Our soak only walks, follows and
# fights; a workshop deadlock, a dialogue that takes no input or a HUD crash was found only by a player.
#
#   tools/menustress.sh [-m minutes] [-g host|vm|both] [-s steps] [-n npcRefHex] [-o outDir]
#
#   -m  how long (default 60)
#   -g  which games (default both): host is this PC's game, vm the other one through an SSH tunnel
#   -s  the steps of a cycle, in order (default wpd): w workshop, p Pip-Boy, d dialogue
#   -n  the NPC to talk to, a reference in both games (default 0001CA7D, Codsworth); none: no dialogue
#   -o  where the logs go (default a new folder in the temp folder)
#
# Each game needs bDevChannel = true and a save in a settlement's build area (Sanctuary), with junk around
# to scrap: the loop scraps the settlement's real junk, and in a session your friend's game scraps it too.
# Use a test save and don't save over it.
#
# The dev channels: host on 127.0.0.1:${F4MP_HOST_PORT:-7790} with the token the game writes
# (Documents\My Games\Fallout4\F4SE\F4Multiplayer_dev.token, or the file F4MP_HOST_TOKEN names); vm on
# 127.0.0.1:${F4MP_VM_PORT:-7792}, a tunnel to the other PC's 7790, with its token in the file F4MP_VM_TOKEN.
# Every command opens its own connection, as devctl does: a channel serves one client at a time, and held
# open it would lock everyone else out. A script that holds a channel (follow.sh holds the VM's) must be
# stopped first, or this loop's commands go unanswered.
#
# How it presses keys and picks options can be changed without a rebuild (the defaults are a player's keys
# through Windows, the `key` dev command; `input <userEvent>` hands the game a button past Windows instead):
#   WS_ENTER   ("workshop enter")  WS_SCRAP ("key 52 80": R)  WS_LEAVE ("key 09 80": Tab)
#   PIP_KEY    ("key 09 80": Tab)  PIP_PAGE ("key 27 60": Right)
#   TALK       ("key 45 80": E)    PICK     ("key 57 80": W, the top option)  PICK_GAME ("input Forward")
#   LEAVE_TALK ("key 09 80": Tab)  EXCHANGES (3)
#   WS_SCRAP=none leaves the junk alone; WS_SCRAP="engine" scraps through the game's scrap function instead.
#
# What it counts as stuck (logged as STUCK with the game's menus, dialogue and workshop state, then put
# right as far as the channel can, and listed in stuck.txt):
#   - a menu still open, or not open, well after the step asked for it to close or open
#   - any menu open longer than STUCK_SECONDS (120) whatever the step
#   - a dialogue option picked, and nothing happening for PICK_SECONDS (4): no line, no new options. Then the
#     same pick goes to the game past Windows (PICK_GAME), and the log says whether that one took.
#   - the game thread hung: the channel answers, but says it waited 12 s for the game thread and gave up
#     (a frozen game, or one that draws on but runs no frame of ours). Logged as HANG, once per hang, with
#     the plugin's own reports from the host's log (its hang watch and frame watch lines: where the main
#     thread is), and on the host the game is caught in the act: tools/hangcatch.ps1 -Now writes every
#     thread's stack and a minidump into <outDir>/hangs. The catch stops the game for the look, so it
#     waits for a second timed-out answer in a row, unless the plugin's frame watch already reported the
#     main thread stuck. The loop then waits until the game runs commands again; whatever a step missed
#     meanwhile is put down to the hang, not counted as stuck.
#       HANGCATCH ("auto": when powershell.exe is there; "none": never)  GAME_LOG (the host's plugin log,
#       F4Multiplayer.log next to its token)
#   - the channel not answering at all for SILENT_SECONDS (30): another client holds the channel, or the
#     game is gone
# A step the game refuses (no workbench near, no junk, the NPC elsewhere) is a miss, not stuck.
# A key, mouse or click goes only with the game in front (`focus` brings it there and says so); one that can't
# is not sent, logged and counted (unsentKeys; unsentPicks for options), never taken for a menu that ignored it.
# At the end, a summary per game; the exit code is 1 when anything got stuck, else 0. Stop early with
# touch <outDir>/stop.

MINUTES=60; GAMES=both; STEPS=wpd; NPC=0001CA7D; OUTDIR=""
while getopts "m:g:s:n:o:" opt; do
	case $opt in
	m) MINUTES=$OPTARG ;;
	g) GAMES=$OPTARG ;;
	s) STEPS=$OPTARG ;;
	n) NPC=$OPTARG ;;
	o) OUTDIR=$OPTARG ;;
	*) sed -n '2,12p' "$0"; exit 2 ;;
	esac
done
[[ $MINUTES =~ ^[0-9]+$ ]] && [ "$MINUTES" -gt 0 ] || { echo "-m is whole minutes"; exit 2; }
[[ $STEPS =~ ^[wpd]+$ ]] || { echo "-s is made of w, p and d"; exit 2; }
case $GAMES in host | vm | both) ;; *) echo "-g is host, vm or both"; exit 2 ;; esac
[ "$NPC" = none ] && NPC=""
[ -n "$NPC" ] && ! [[ $NPC =~ ^[0-9A-Fa-f]{1,8}$ ]] && { echo "-n is a reference in hex, or none"; exit 2; }
[ -z "$OUTDIR" ] && OUTDIR="${TMPDIR:-/tmp}/f4mp-menustress-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUTDIR" || exit 2
rm -f "$OUTDIR/stop"

HOST_PORT=${F4MP_HOST_PORT:-7790}
VM_PORT=${F4MP_VM_PORT:-7792}
if [ -n "$F4MP_HOST_TOKEN" ]; then HOST_TOKEN=$F4MP_HOST_TOKEN
elif [ -n "$USERPROFILE" ]; then HOST_TOKEN="$(cygpath -u "$USERPROFILE" 2>/dev/null || echo "$USERPROFILE")/Documents/My Games/Fallout4/F4SE/F4Multiplayer_dev.token"
else HOST_TOKEN="$HOME/Documents/My Games/Fallout4/F4SE/F4Multiplayer_dev.token"; fi
VM_TOKEN=${F4MP_VM_TOKEN:-}
TOOLS=$(cd "$(dirname "$0")" && pwd)
GAME_LOG=${GAME_LOG:-$(dirname "$HOST_TOKEN")/F4Multiplayer.log}
HANGCATCH=${HANGCATCH:-auto}

WS_ENTER=${WS_ENTER:-workshop enter}
WS_SCRAP=${WS_SCRAP:-key 52 80}
WS_LEAVE=${WS_LEAVE:-key 09 80}
PIP_KEY=${PIP_KEY:-key 09 80}
PIP_PAGE=${PIP_PAGE:-key 27 60}
TALK=${TALK:-key 45 80}
PICK=${PICK:-key 57 80}
PICK_GAME=${PICK_GAME:-input Forward}
LEAVE_TALK=${LEAVE_TALK:-key 09 80}
EXCHANGES=${EXCHANGES:-3}
STUCK_SECONDS=${STUCK_SECONDS:-120}
PICK_SECONDS=${PICK_SECONDS:-4}
SILENT_SECONDS=${SILENT_SECONDS:-30}

# f <var> <text> <field>: sets var to field=value from the text (empty when it isn't there).
f() { if [[ " $2" =~ \ $3=([^ ]*) ]]; then printf -v "$1" '%s' "${BASH_REMATCH[1]}"; else printf -v "$1" ''; fi; }

# --- one game's loop (runs in its own subshell: everything below is per game) ---

G=""; LOG=""; PORT=""; TOKEN=""; OUT=""; MENUS=""
silent_since=0; silent_reported=0; hung_since=0; hang_answers=0; hang_caught=0; game_log_at=0
cycles=0; steps=0; misses=0; stuck_count=0; scraps=0; picks=0; picks_ignored=0; picks_unsent=0; game_picks=0; game_picks_taken=0; silences=0; hangs=0; unsent=0
FOCUS=""; DROPPED=""

log() { echo "$(date +%T) $*" >> "$LOG"; }

# The host's plugin log: its size now, and the lines of the plugin's own hang and frame watches written
# since the last look (the game flushes warnings at once, so they are there while it is still hung).
# Succeeds when one of them is a frame watch line: the plugin's own word that the main thread is stuck.
game_log_size() { if [ -f "$GAME_LOG" ]; then wc -c < "$GAME_LOG" | tr -d ' '; else echo 0; fi; }
game_log_watch_lines() {
	[ "$G" = host ] && [ -f "$GAME_LOG" ] || return 1
	local size lines; size=$(game_log_size)
	[ "$size" -lt "$game_log_at" ] && game_log_at=0  # a restarted game began a new log
	lines=$(tail -c +$((game_log_at + 1)) "$GAME_LOG" | head -c $((size - game_log_at)) | tr -d '\r' | grep -E 'Hang( watch)?:|Frame watch:')
	game_log_at=$size
	[ -n "$lines" ] && printf '%s\n' "$lines" | sed 's/^/  game log: /' >> "$LOG"
	[[ $lines == *"Frame watch:"* ]]
}

# The host's game caught in the act: every thread's stack and a minidump (tools/hangcatch.ps1 -Now, which
# stops the game for the look and lets it go). Only this PC's game: the VM's is out of reach.
catch_hang() {
	[ "$G" = host ] && [ "$HANGCATCH" != none ] || return
	if ! command -v powershell.exe > /dev/null || [ ! -f "$TOOLS/hangcatch.ps1" ]; then
		log "  no catch: needs powershell.exe and tools/hangcatch.ps1 (HANGCATCH=none turns this off)"; return
	fi
	local dir="$OUTDIR/hangs" script="$TOOLS/hangcatch.ps1"
	mkdir -p "$dir"
	if command -v cygpath > /dev/null; then dir=$(cygpath -w "$dir"); script=$(cygpath -w "$script"); fi
	log "  catching the game's threads (tools/hangcatch.ps1 -Now)"
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$script" -Now -OutDir "$dir" 2>&1 | tr -d '\r' | sed 's/^/  hangcatch: /' >> "$LOG"
}

# The channel answered that the game thread ran none of our command for 12 s. The first such answer of a
# hang is logged as HANG, with the plugin's own reports; the hang lasts until a command runs. The game is
# caught in the act once (catch_hang stops it for the look, which a game that was only loading would not
# thank it for): at once when the plugin's frame watch says the main thread is stuck, else on the second
# timed-out answer in a row.
game_thread_hung() {
	local watched=1
	if [ $hung_since = 0 ]; then
		hung_since=$(($1 - 12)); hangs=$((hangs + 1)); hang_answers=0; hang_caught=0
		log "HANG: the game thread has run no command for 12 s (the channel answers; the game runs no frame of ours)"
		echo "$(date +%T) $G game thread hung" >> "$OUTDIR/stuck.txt"
	fi
	hang_answers=$((hang_answers + 1))
	game_log_watch_lines && watched=0
	[ $hang_caught = 1 ] && return
	if [ $watched = 0 ] || [ $hang_answers -ge 2 ]; then
		hang_caught=1
		catch_hang
	fi
}

# call <command...>: one command on the game's channel; OUT is the reply. Fails when the channel doesn't
# answer, or answers that the game thread didn't run the command (a hang, above). Keeps count of how long
# the channel hasn't answered at all: SILENT_SECONDS of that is logged once, and so is its end.
call() {
	OUT=""
	local tok a fd now
	tok=$(tr -d '\r\n ' < "$TOKEN" 2>/dev/null)
	if [ -n "$tok" ] && { exec {fd}<>"/dev/tcp/127.0.0.1/$PORT"; } 2>/dev/null; then
		if echo "auth $tok" >&$fd 2>/dev/null && read -t 5 -r a <&$fd && [ "${a%$'\r'}" = ok ]; then
			echo "$*" >&$fd 2>/dev/null && read -t 15 -r OUT <&$fd
		fi
		exec {fd}>&-
	fi
	OUT=${OUT%$'\r'}
	now=$(date +%s)
	if [ -n "$OUT" ]; then
		if [ $silent_reported = 1 ]; then log "the channel answers again after $((now - silent_since)) s"; fi
		silent_since=0; silent_reported=0
		if [[ $OUT == "error: timed out waiting for game thread"* ]]; then
			game_thread_hung "$now"
			return 1
		fi
		if [ $hung_since != 0 ]; then
			log "the game thread runs commands again after about $((now - hung_since)) s"
			game_log_watch_lines
			hung_since=0
		fi
		return 0
	fi
	[ $silent_since = 0 ] && silent_since=$now
	if [ $silent_reported = 0 ] && [ $((now - silent_since)) -ge "$SILENT_SECONDS" ]; then
		silent_reported=1; silences=$((silences + 1))
		log "SILENT: no answer from the channel for $((now - silent_since)) s (the game hung or busy, or another client holds the channel)"
		echo "$(date +%T) $G channel silent" >> "$OUTDIR/stuck.txt"
	fi
	return 1
}

# A step done, missed (the game wouldn't), or stuck (the game should have and didn't).
done_step() { steps=$((steps + 1)); }
miss() {
	if [ $hung_since != 0 ]; then log "during the hang: $*"; return; fi
	misses=$((misses + 1)); log "miss: $*"
}
stuck() {
	if [ $hung_since != 0 ]; then log "during the hang: $*"; return; fi  # the hang says why, and the channel can't look
	stuck_count=$((stuck_count + 1))
	log "STUCK: $*"
	echo "$(date +%T) $G $*" >> "$OUTDIR/stuck.txt"
	call menus && log "  menus: $OUT"
	call dialogue && log "  dialogue: $OUT"
	call workshop && log "  workshop: $OUT"
	call "net status" && log "  net: ${OUT:0:600}"
}

# in_front: asks for the game to be in front and waits up to 3 s for it. FOCUS is the last answer of focus, and
# DROPPED the plugin's count of the input it dropped so far (it sends key, mouse and click only with the game
# in front). A plugin that predates that check (focus answers only "requested") counts as in front.
in_front() {
	local i
	FOCUS=""
	for ((i = 0; i < 7; i++)); do
		call focus || return 1
		FOCUS=$OUT; f DROPPED "$OUT" dropped
		[[ $OUT == front=yes* || $OUT != *front=* ]] && return 0
		sleep 0.5
	done
	return 1
}

# press <command>: a step's key. key, mouse and click go through Windows, which hands them to whatever window
# is in front, so the game is brought to the front first; when it can't be, the press is not sent, logged and
# counted (unsent), and press fails. On 2026-10-11 presses sent while a screenshot task had the front went
# nowhere and were read as a dialogue that no longer took input. Other commands (input ...) are sent as they are.
press() {
	case $1 in
	key\ * | mouse\ * | click*)
		if ! in_front; then
			unsent=$((unsent + 1))
			log "not sent, the game isn't in front: '$*' (focus: ${FOCUS:-no answer})"
			return 1
		fi ;;
	esac
	call "$@"
}

# menus_now: MENUS is the menus line. is_open <menu>: whether it lists the menu open. open_for <menu>:
# SECS is how long it has been (whole seconds), 0 when it isn't.
menus_now() { call menus && [[ $OUT != error* ]] && MENUS=$OUT; }
is_open() { local o; f o "$MENUS" open; [[ ",$o," == *",$1:"* ]]; }
open_for() { local o; f o "$MENUS" open; SECS=0; if [[ ",$o," =~ ,$1:([0-9]+) ]]; then SECS=${BASH_REMATCH[1]}; fi; }

# wait_menu <menu> open|closed <seconds>: until the menu is so, or the time is up (fails).
wait_menu() {
	local end=$(($(date +%s) + $3))
	while [ "$(date +%s)" -lt $end ]; do
		if menus_now; then
			if [ "$2" = open ] && is_open "$1"; then return 0; fi
			if [ "$2" = closed ] && ! is_open "$1"; then return 0; fi
		fi
		sleep 0.5
	done
	return 1
}

# Any menu up longer than STUCK_SECONDS, whatever asked for it.
check_menus() {
	menus_now || return
	local longest name secs
	f longest "$MENUS" longest
	[ -z "$longest" ] || [ "$longest" = none ] && return
	name=${longest%%:*}; secs=${longest#*:}; secs=${secs%.*}
	if [ "$secs" -ge "$STUCK_SECONDS" ]; then
		stuck "$name has been open $secs s"
		recover
	fi
}

# Puts the game back in play as far as the channel can: out of workshop mode, the Pip-Boy and the dialogue.
recover() {
	menus_now || return
	if is_open WorkshopMenu; then call "workshop exit"; wait_menu WorkshopMenu closed 10 || log "  recover: still in workshop mode"; fi
	if is_open PipboyMenu; then call "menu PipboyMenu force"; wait_menu PipboyMenu closed 10 || log "  recover: the Pip-Boy is still open"; fi
	if is_open DialogueMenu; then
		call "input Cancel"; wait_menu DialogueMenu closed 10 || { press "$LEAVE_TALK"; wait_menu DialogueMenu closed 10 || log "  recover: still in the dialogue"; }
	fi
}

workshop_step() {
	call workshop || return
	if [[ $OUT == error* ]]; then miss "workshop: $OUT"; return; fi
	local inArea bench junk id
	f inArea "$OUT" inArea; f bench "$OUT" bench
	if [ "$inArea" != true ]; then miss "workshop: not in a workbench's build area (bench=$bench)"; return; fi
	call "$WS_ENTER"
	if ! wait_menu WorkshopMenu open 10; then
		stuck "workshop mode didn't come up 10 s after '$WS_ENTER'"
		recover; return
	fi
	sleep 1.5
	if [ "$WS_SCRAP" != none ]; then
		call "workshop junk 1200"
		junk=$OUT; id=""
		[[ $junk =~ ^([0-9A-F]{8})\  ]] && id=${BASH_REMATCH[1]}
		if [ -z "$id" ]; then
			miss "workshop: no junk to scrap ($junk)"
		else
			call "turnto $id 300"; sleep 0.45
			call "workshop select $id"; sleep 0.4
			if [ "$WS_SCRAP" = engine ]; then call "workshop scrap $id"; else press "$WS_SCRAP"; fi
			log "scrap $id: $OUT"
			sleep 1.2
			call "refinfo $id"
			local dis del
			f dis "$OUT" disabled; f del "$OUT" deleted
			if [[ $OUT == error* ]] || [ "$dis" = true ] || [ "$del" = true ]; then scraps=$((scraps + 1)); else miss "workshop: $id is still there after '$WS_SCRAP'"; fi
		fi
	fi
	press "$WS_LEAVE"
	if ! wait_menu WorkshopMenu closed 10; then
		stuck "still in workshop mode 10 s after '$WS_LEAVE'"
		recover; return
	fi
	done_step
}

pipboy_step() {
	press "$PIP_KEY"
	if ! wait_menu PipboyMenu open 8; then miss "the Pip-Boy didn't open (in combat, or another menu up)"; recover; return; fi
	sleep 1.2; press "$PIP_PAGE"; sleep 1.2
	press "$PIP_KEY"
	if ! wait_menu PipboyMenu closed 8; then
		stuck "the Pip-Boy is still open 8 s after '$PIP_KEY'"
		recover; return
	fi
	done_step
}

# dialogue_now: TALKING (the speaker says something), ALLOW (the options take input) and DMENU from the
# dialogue command. Fails without an answer.
dialogue_now() {
	call dialogue || return 1
	f TALKING "$OUT" talking; f ALLOW "$OUT" allowInput; f DMENU "$OUT" dialogueMenu
	return 0
}

# pick_took <seconds>: whether a pick did something within the time: a line is spoken, the options are
# closed to input while it plays, or the conversation ends.
pick_took() {
	local end=$(($(date +%s) + $1))
	while [ "$(date +%s)" -lt $end ]; do
		sleep 0.4
		dialogue_now || continue
		[ "$TALKING" = true ] || [ "$ALLOW" = false ] || [ "$DMENU" = false ] && return 0
	done
	return 1
}

# wait_options <seconds>: until the options are up again (nobody talking, input allowed), or the dialogue ends.
wait_options() {
	local end=$(($(date +%s) + $1))
	while [ "$(date +%s)" -lt $end ]; do
		dialogue_now && { [ "$DMENU" = false ] || { [ "$TALKING" = false ] && [ "$ALLOW" = true ]; }; } && return 0
		sleep 0.5
	done
	return 1
}

dialogue_step() {
	[ -z "$NPC" ] && return
	call "refinfo $NPC" || return
	if [[ $OUT == error* ]]; then miss "dialogue: no $NPC here"; return; fi
	local dead; f dead "$OUT" dead
	[ "$dead" = true ] && { miss "dialogue: $NPC is dead"; return; }
	# In front of me, facing me: where E opens the dialogue (follow.sh learned it must be right in front).
	call pos || return
	local mx my mz mrz nx ny nrz
	f mx "$OUT" x; f my "$OUT" y; f mz "$OUT" z; f mrz "$OUT" rz
	read -r nx ny nrz < <(awk -v x="$mx" -v y="$my" -v r="$mrz" 'BEGIN{a=r*3.14159265/180; printf "%.0f %.0f %.0f\n", x+110*sin(a), y+110*cos(a), (r+180)%360}')
	call "setpos $NPC $nx $ny $mz $nrz"; sleep 0.6
	call "turnto $NPC 120"; sleep 0.3
	if ! press "$TALK"; then miss "dialogue: '$TALK' not sent (the game isn't in front)"; return; fi
	if ! wait_menu DialogueMenu open 6; then miss "dialogue: '$TALK' didn't open a dialogue with $NPC"; recover; return; fi
	local n
	for ((n = 1; n <= EXCHANGES; n++)); do
		if ! wait_options 20; then stuck "dialogue: no options to pick 20 s into exchange $n"; recover; return; fi
		[ "$DMENU" = false ] && break
		# The pick only counts when it reached the game: sent with the game in front, and not dropped by the
		# plugin for losing the front on the way (its dropped count unchanged across the pick).
		if ! press "$PICK"; then picks_unsent=$((picks_unsent + 1)); miss "dialogue: pick $n not sent (the game isn't in front)"; recover; return; fi
		local dropped_before=$DROPPED
		picks=$((picks + 1))
		if pick_took "$PICK_SECONDS"; then continue; fi
		in_front; if [ -n "$dropped_before" ] && [ -n "$DROPPED" ] && [ "$DROPPED" != "$dropped_before" ]; then
			picks=$((picks - 1)); picks_unsent=$((picks_unsent + 1))
			miss "dialogue: pick $n was dropped on its way (the game lost the front: $FOCUS)"; recover; return
		fi
		picks_ignored=$((picks_ignored + 1))
		# The pick reached the game and went nowhere. The state that says why, then the same pick past Windows:
		# if that one takes, the menu wasn't getting the key; if not, the menu takes no input at all.
		log "pick $n ('$PICK') did nothing in $PICK_SECONDS s"
		log "  focus: $FOCUS"
		call dialogue && log "  dialogue: $OUT"
		call "dialogue menu" && log "  dialogue menu: $OUT"
		call controls && log "  controls: $OUT"
		press "$PICK_GAME"; game_picks=$((game_picks + 1)); log "  '$PICK_GAME': $OUT"
		if pick_took "$PICK_SECONDS"; then
			game_picks_taken=$((game_picks_taken + 1)); log "  the pick past Windows took"
		else
			stuck "dialogue: exchange $n took neither '$PICK' nor '$PICK_GAME'"
			recover; return
		fi
	done
	menus_now
	if is_open DialogueMenu; then
		press "$LEAVE_TALK"
		if ! wait_menu DialogueMenu closed 10; then
			stuck "still in the dialogue 10 s after '$LEAVE_TALK'"
			recover; return
		fi
	fi
	done_step
}

summary() {
	echo "$G: cycles=$cycles steps=$steps misses=$misses stuck=$stuck_count hangs=$hangs silences=$silences scraps=$scraps picks=$picks ignored=$picks_ignored unsentPicks=$picks_unsent pickedPastWindows=$game_picks_taken/$game_picks unsentKeys=$unsent"
}

run_game() {
	G=$1; LOG="$OUTDIR/menustress-$G.log"
	if [ "$G" = host ]; then PORT=$HOST_PORT; TOKEN=$HOST_TOKEN; else PORT=$VM_PORT; TOKEN=$VM_TOKEN; fi
	if [ -z "$TOKEN" ] || [ ! -r "$TOKEN" ]; then echo "$G: no token file (${TOKEN:-set F4MP_VM_TOKEN})" | tee -a "$LOG"; echo "$G: skipped" > "$OUTDIR/summary-$G.txt"; return; fi
	local end=$(($(date +%s) + MINUTES * 60))
	game_log_at=$(game_log_size)
	log "start: $MINUTES min, steps $STEPS, npc ${NPC:-none}"
	if ! call status || [[ $OUT != *ingame=true* ]]; then log "not in game: ${OUT:-no answer}"; echo "$G: not in game" > "$OUTDIR/summary-$G.txt"; return; fi
	call menus
	[[ $OUT == error* ]] && log "warning: $OUT"
	local step i
	while [ "$(date +%s)" -lt $end ] && [ ! -e "$OUTDIR/stop" ]; do
		cycles=$((cycles + 1))
		for ((i = 0; i < ${#STEPS}; i++)); do
			# A hung game gets no new step until it runs commands again, and then is put back in play: the
			# step the hang cut short may have left a menu open.
			if [ $hung_since != 0 ]; then
				while [ $hung_since != 0 ] && [ "$(date +%s)" -lt $end ] && [ ! -e "$OUTDIR/stop" ]; do
					call status || sleep 2
				done
				[ $hung_since = 0 ] || break 2  # still hung when the time ran out, or a stop was asked for
				recover
			fi
			step=${STEPS:i:1}
			case $step in
			w) workshop_step ;;
			p) pipboy_step ;;
			d) dialogue_step ;;
			esac
			check_menus
			sleep 0.5
		done
		((cycles % 10 == 0)) && log "$(summary)"
	done
	game_log_watch_lines  # and any the plugin wrote that the loop didn't see (a freeze shorter than the channel's 12 s)
	log "end: $(summary)"
	summary > "$OUTDIR/summary-$G.txt"
}

case $GAMES in
both) (run_game host) & (run_game vm) & wait ;;
*) run_game "$GAMES" ;;
esac
cat "$OUTDIR"/summary-*.txt 2>/dev/null
echo "logs: $OUTDIR"
if [ -s "$OUTDIR/stuck.txt" ]; then echo "STUCK:"; cat "$OUTDIR/stuck.txt"; exit 1; fi
grep -q "not in game\|skipped" "$OUTDIR"/summary-*.txt 2>/dev/null && exit 1
exit 0
