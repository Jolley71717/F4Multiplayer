#!/bin/bash
# standin-settler-lines.sh [owner] [raiderBase]: a player's stand-in speaking the settler's own lines (a player's report, 0.20.1:
# generic settler chatter in the settler's voice, not the player's, at random times). The owner's stand-in (host by default, or vm)
# stands in the watcher's game; the watcher's log is put at debug, where Conversations names every actor the engine has talking
# ("Conversations: <ref> says '<line>'"), and the watcher's dev `npcvoice <puppet>` is read every second. A friend's own shared
# lines are plain 3D sounds at the stand-in (Conversations::Play, no Say), so a line the engine has the stand-in saying is the
# settler's own. Three things that make an NPC speak are tried on it, each watched TRY s: the watcher walks up to it and stands
# before it (a hello); the watcher shoots it (its pain lines: dev turnto + real clicks, a 10mm and rounds given by console, said so);
# a raider placed beside it (dev spawn, said so) attacks it (pain and combat lines) and is removed after (console disable).
# REPRO: YES when the engine had the stand-in saying any line, NO when it said nothing through all three, VOID when the scene could
# not be set up (no stand-in, no raider base here) or, with nothing said yet, the raider was not in combat with the stand-in within
# 20 s (it went for the watcher's player, or stood idle: a NO then would test nothing). Its end steps (lib.sh on_end) put the
# watcher's log back at info and disable the raider however the run ends. On a build that keeps stand-ins quiet (it has dev `puppetvoice`) a line the
# engine starts is ended the frame it is seen, so there the verdict goes by what could be heard: the stand-in found talking by a
# 1 s read (YES), and the plugin's own count is reported (net status ownLines=seen/ended; each seen one must be ended).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
OWNER=${1:-host}; RAIDER=${2:-001AE38D}; TRY=${TRY:-20}   # a raider base (an NPC by dev edid in both games; vats-enter.sh uses it too)
[ "$OWNER" = host ] || [ "$OWNER" = vm ] || { echo "usage: standin-settler-lines.sh [host|vm] [raiderBaseHex]"; exit 2; }
WATCHER=$(other_game "$OWNER")
need_session
PUPPET=$(dc "$WATCHER" 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | head -1 | cut -d= -f2)
[ -n "$PUPPET" ] || verdict VOID "the $WATCHER's game has no stand-in for the $OWNER's player (no puppet= in its net status)"
run_cap 1200   # three triggers of TRY s and a walk: the longest good run took 8 minutes; one still going after 20 is stuck
log_back() { dc "$WATCHER" "log level info" >/dev/null; }
dc "$WATCHER" "log level debug" >/dev/null; on_end log_back   # the log back at info however the run ends
rmark "$WATCHER" all
QUIET=0; case "$(dc "$WATCHER" puppetvoice)" in voiceMode=*) QUIET=1;; esac
# ownlines: the watcher's net status ownLines=seen/ended (a count for the game's life: this run's are the difference from here)
ownlines() { dc "$WATCHER" 'net status' | grep -oE 'ownLines=[0-9]+/[0-9]+' | head -1 | cut -d= -f2; }
OWN0=$(ownlines); OWN0=${OWN0:-0/0}
MODE=$(dc "$WATCHER" puppetvoice | grep -oE '^voiceMode=[0-9]+' | cut -d= -f2)
[ "$QUIET" = 1 ] && say "this build keeps stand-ins quiet ($(dc "$WATCHER" puppetvoice | cut -c1-80)): the verdict goes by talking heard"
SAID=0; LINES=""
# watch <label> [marked]: TRY s of npcvoice reads and the log's "says" lines for the puppet; counts what it said. With "marked" the
# log's mark is the caller's, set before the trigger: the shots' pain lines come at the shots, before the watch starts (a mark set
# here missed them in a run on 0.21.0, where the log had 'Oof!', 'Agh!' and 'Yeagh!' and the watch counted 0).
watch() {
	local i v t n
	[ "${2:-}" = marked ] || rmark "$WATCHER" t
	for i in $(seq 1 "$TRY"); do
		v=$(dc "$WATCHER" "npcvoice $PUPPET")
		if [ "$(field "$v" talking)" = true ]; then t=$(grep -oE "subtitle='[^']*'" <<<"$v"); say "  $1: the stand-in is talking ($t, $(grep -oE 'state=[0-9]+ elapsed=[0-9.]+' <<<"$v"))"; fi
		echo "$1 $i: $v" >> "$REPRO_OUT/npcvoice.log"
		sleep 1
	done
	n=$(rsince "$WATCHER" t | grep -c "Conversations: $PUPPET says")
	LINES="$LINES$(rsince "$WATCHER" t | grep "Conversations: $PUPPET says" | sed -E "s/.*says '([^']*)'.*/\1/" | head -3 | tr '\n' '|')"
	n2=$(grep -c "$1 .* talking=true" "$REPRO_OUT/npcvoice.log")
	say "  $1: $n line(s) in the log, talking seen $n2 time(s)"
	# a quiet build ends each line it sees (its "says" line is logged all the same): there it goes by what was heard; with the end
	# measure off (voiceMode 0 or 1) the log's lines played, as on 0.21.0
	if [ "$QUIET" = 1 ] && [ $(( ${MODE:-3} & 2 )) -ne 0 ]; then SAID=$((SAID + n2)); else SAID=$((SAID + n + (n == 0 ? n2 : 0))); fi
}
say "standin-settler-lines: the $OWNER's stand-in $PUPPET in the $WATCHER's game, watched for its own lines; each trigger $TRY s"
# 1. a hello: the watcher walks up to it and stands before it
say "trigger 1: the $WATCHER's player walks up to the stand-in (real walk) and stands before it"
rwalk "$WATCHER" "$PUPPET" 90 0.6; dc "$WATCHER" "turnto $PUPPET 300" >/dev/null
watch hello
# 2. pain: the watcher shoots it (its damage goes to the owner, who is in god mode on the rig)
say "trigger 2: the $WATCHER's player shoots the stand-in (console additem 10mm + rounds, said so; dev draw; real clicks)"
dc "$WATCHER" "console player.additem 00004822 1" >/dev/null; dc "$WATCHER" "console player.additem 0001F276 60" >/dev/null; sleep 1
dc "$WATCHER" "console player.equipitem 00004822" >/dev/null; sleep 1; dc "$WATCHER" "draw on" >/dev/null; sleep 2
rmark "$WATCHER" t
for i in 1 2 3 4 5 6; do dc "$WATCHER" "turnto $PUPPET 300 aim" >/dev/null; sleep 0.4; dc "$WATCHER" focus >/dev/null; dc "$WATCHER" "click 80" >/dev/null; sleep 1.5; done
watch shot marked
dc "$WATCHER" "draw off" >/dev/null
# 3. a raider beside it
e=$(dc "$WATCHER" "edid $RAIDER")
case "$e" in "'"*) say "trigger 3: a raider ($RAIDER $e) placed 300 u from the $WATCHER's player (dev spawn, said so), left to attack $TRY s";;
*) say "trigger 3 skipped: $RAIDER is no form in the $WATCHER's game ($e)"; e="";; esac
if [ -n "$e" ]; then
	rmark "$WATCHER" t   # before the spawn: lines said while the gate below waits are counted too (watch ... marked)
	before=$(dc "$WATCHER" "spawn $RAIDER 300"); say "  spawn: $(cut -c1-120 <<<"$before")"
	R=$(grep -oE '[0-9A-F]{8}' <<<"$before" | head -1)
	raider_gone() { [ -n "$R" ] && dc "$WATCHER" "console $R.disable" >/dev/null; return 0; }
	on_end raider_gone
	# a NO needs the raider to have fought the stand-in: one that fights the watcher's player, or stands idle, tests nothing.
	# With a line already said the verdict is YES either way, so the gate is only for a run still on its way to NO
	raider_on_standin() { [ "$(field "$(dc "$WATCHER" "combat $R")" target)" = "$PUPPET" ]; }
	[ "$SAID" -gt 0 ] || scene_live "the raider ${R:-?} is in combat with the $OWNER's stand-in $PUPPET" 20 raider_on_standin
	watch raider marked
	raider_gone
fi
log_back
rkeep "$WATCHER" standin-settler-lines; rkeep "$OWNER" standin-settler-lines
say "lines said: $SAID${LINES:+ ($LINES)}; the stand-in's own lines in the log over the whole run: $(rsince "$WATCHER" all | grep -c "Conversations: $PUPPET says")"
if [ "$QUIET" = 1 ]; then
	OWN=$(ownlines); OWN=${OWN:-0/0}
	seen=$(( ${OWN%/*} - ${OWN0%/*} )); ended=$(( ${OWN#*/} - ${OWN0#*/} ))
	say "the plugin's count of lines found on stand-ins and ended, this run: $seen/$ended (ownLines=$OWN, $OWN0 before; voiceMode=${MODE:-?})"
	# with the end measure off (voiceMode 0 or 1, the dev's A/B) a line seen is left to play: counted above by what was heard
	if [ $(( ${MODE:-3} & 2 )) -ne 0 ]; then
		[ "$seen" = "$ended" ] || verdict YES "the stand-in $PUPPET had $seen line(s) of its own and only $ended ended"
	fi
fi
[ "$SAID" -gt 0 ] && verdict YES "the engine had the stand-in $PUPPET saying its own line(s): $SAID"
verdict NO "the stand-in $PUPPET said nothing of its own through a hello, shots and a raider's attack"
