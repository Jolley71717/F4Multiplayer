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
# not be set up (no stand-in, no raider base here).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
OWNER=${1:-host}; RAIDER=${2:-0002A4F0}; TRY=${TRY:-20}
[ "$OWNER" = host ] || [ "$OWNER" = vm ] || { echo "usage: standin-settler-lines.sh [host|vm] [raiderBaseHex]"; exit 2; }
WATCHER=$(other_game "$OWNER")
need_session
PUPPET=$(dc "$WATCHER" 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | head -1 | cut -d= -f2)
[ -n "$PUPPET" ] || verdict VOID "the $WATCHER's game has no stand-in for the $OWNER's player (no puppet= in its net status)"
dc "$WATCHER" "log level debug" >/dev/null
rmark "$WATCHER" all
SAID=0; LINES=""
# watch <label>: TRY s of npcvoice reads and the log's "says" lines for the puppet; counts what it said
watch() {
	local i v t n
	rmark "$WATCHER" t
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
	SAID=$((SAID + n + (n == 0 ? n2 : 0)))
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
watch shot
dc "$WATCHER" "draw off" >/dev/null
# 3. a raider beside it
e=$(dc "$WATCHER" "edid $RAIDER")
case "$e" in "'"*) say "trigger 3: a raider ($RAIDER $e) placed 300 u from the $WATCHER's player (dev spawn, said so), left to attack $TRY s";;
*) say "trigger 3 skipped: $RAIDER is no form in the $WATCHER's game ($e)"; e="";; esac
if [ -n "$e" ]; then
	before=$(dc "$WATCHER" "spawn $RAIDER 300"); say "  spawn: $(cut -c1-120 <<<"$before")"
	R=$(grep -oE '[0-9A-F]{8}' <<<"$before" | head -1)
	watch raider
	[ -n "$R" ] && dc "$WATCHER" "console $R.disable" >/dev/null
fi
dc "$WATCHER" "log level info" >/dev/null
rkeep "$WATCHER" standin-settler-lines; rkeep "$OWNER" standin-settler-lines
say "lines said: $SAID${LINES:+ ($LINES)}"
[ "$SAID" -gt 0 ] && verdict YES "the engine had the stand-in $PUPPET saying its own line(s): $SAID"
verdict NO "the stand-in $PUPPET said nothing of its own through a hello, shots and a raider's attack"
