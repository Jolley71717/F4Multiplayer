#!/bin/bash
# side-npc-voices.sh <talker> [name...]: a player's talk with side characters, and whether every voiced line is heard: in the
# talker's own game (the engine's own voice, the NPC run here or mirrored from the friend's game and taken over for the talk) and in
# the friend's game standing beside them (the line shared, played at the friend's copy of the NPC). Reported on Nexus (0.17.0):
# "some audio doesn't play from certain side character npcs when talking in quests".
#
# Diamond City's market (console coc DiamondCityOrigin, said): the other game's player comes first (FIRST_WAIT s), so its game runs
# the market's NPCs, then the talker's. For each NPC named (a part of its name; default a few of the market's people with voiced
# dialogue), in the talker's game: the friend walks to it (real walk, the pad's stick) and stands by; the talker walks to it and
# presses E (a real key), and answers by real keys (W, A, D, S in turn, REPLIES at most; a trade or other menu the answer opens is
# shut by a real Tab), then leaves by Tab (Enter after; the dev channel's dialogue reset only if both fail, said). While the
# conversation lasts, the talker's game is read every half second: the NPC's line (`npcvoice`: talking, subtitle, its voice
# sound), the sounds that follow the NPC (`sounds`), and the recording the mod would share (`voiceline`: info, response, voice
# type, the engine's own voice file and the file the mod resolves). Both games' logs are at debug level for the run.
#
# Per NPC line (a subtitle said while talking): HEARD HERE when the engine had a voice sound for it in the talker's game, SILENT
# HERE when it had none though a recording exists; then in the friend's game VOICED (the line played at the friend's copy), or
# the reason it was not ("no voice file", "is not said: ...", held then let go, text only). Lines with no recording in the game at
# all (no engine file, no file the mod resolves) are counted apart (UNVOICED: the game's own silent lines).
# REPRO: YES when a voiced NPC line is SILENT HERE, or reached the friend's game unvoiced while a recording exists (the mod's
# file empty while the engine's is not counts too: the friend could not play it); NO when every voiced line was heard in both
# games; VOID when no conversation could be opened. The talks' log lines are kept per NPC in $REPRO_OUT.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
TALKER=${1:-host}; case "$TALKER" in host|vm) ;; *) echo "usage: $0 <host|vm> [name...]"; exit 2;; esac
LISTENER=$(other_game "$TALKER"); shift
if [ $# -gt 0 ]; then NAMES=("$@"); else NAMES=("Arturo" "Myrna" "Percy" "Takahashi" "Moe Cronin" "John"); fi
REPLIES=${REPLIES:-6}; CELL=${CELL:-DiamondCityOrigin}; ARRIVE=${ARRIVE:-1}; FIRST_WAIT=${FIRST_WAIT:-20}; TALK_MAX=${TALK_MAX:-120}
VK_W=0x57; VK_A=0x41; VK_S=0x53; VK_D=0x44; ANSWERS=($VK_W $VK_A $VK_D $VK_S $VK_W $VK_W $VK_D $VK_A)
need_session

talking() { [ "$(field "$(dc "$1" dialogue)" inDialogue)" = true ]; }
quiet() { [ "$(field "$(dc "$1" dialogue)" inDialogue)" = false ]; }
counters() { dc "$1" 'net status' | grep -oE 'conversations: [^|]*' | grep -oE '(received|voiced|unplaced|missing|unplayed|reported|placed|held)=[0-9/]+' | paste -sd' ' -; }
cnt() { grep -oE "(^| )$2=[0-9]+" <<<"$1" | head -1 | cut -d= -f2; }
player_id() { dc "$1" 'net status' | grep -oE ' id=[0-9]+' | head -1 | cut -d= -f2; }
# ref_of <who> <name>: a loaded, living actor reference of that name in that game ("" when none)
ref_of() { dc "$1" "findref $2" | tr ';' '\n' | grep -E "dead=false 3d=true" | grep -oE '[0-9A-F]{8}' | head -1; }

say "side-npc-voices: $TALKER talks, $LISTENER stands by; NPCs: ${NAMES[*]}; up to $REPLIES answers each"
for g in host vm; do dc "$g" 'pad unplug' >/dev/null; dc "$g" 'log level debug' >/dev/null; done
say "set-up: both logs at debug level (dev log level; back to the ini's at the games' restart)"
HID=$(player_id host); VID=$(player_id vm)
if [ "$TALKER" = host ]; then TID=$HID; LID=$VID; else TID=$VID; LID=$HID; fi
if [ "$ARRIVE" = 1 ]; then
	say "set-up: $LISTENER comes to Diamond City first by console (coc $CELL), so its game runs the market's NPCs; $TALKER after $FIRST_WAIT s (coc)"
	dc "$LISTENER" focus >/dev/null; dc "$LISTENER" "console coc $CELL" >/dev/null; sleep 8
	wait_ingame "$LISTENER" 60 || verdict VOID "$LISTENER did not come back into the game after its coc"
	sleep "$FIRST_WAIT"
	dc "$TALKER" focus >/dev/null; dc "$TALKER" "console coc $CELL" >/dev/null; sleep 8
	wait_ingame "$TALKER" 60 || verdict VOID "$TALKER did not come back into the game after its coc"
	sleep 10
fi
for g in host vm; do dc "$g" 'pad unplug' >/dev/null; dc "$g" 'god on' >/dev/null; done

TALKS=0; LINES=0; SILENT_HERE=0; UNHEARD=0; UNVOICED=0; NOFILE_OURS=0; VOICED_THERE=0
for name in "${NAMES[@]}"; do
	ref=$(ref_of "$TALKER" "$name")
	[ -n "$ref" ] || { say "$name: no loaded living reference in $TALKER's game ($(dc "$TALKER" "findref $name" | cut -c1-160)): skipped"; continue; }
	owner=$(field "$(dc "$TALKER" "ghosts $ref")" owner)
	case "$owner" in "$LID") runs="$LISTENER's game runs it (mirrored in $TALKER's)";; "$TID") runs="$TALKER's game runs it";; *) runs="owner '$owner'";; esac
	say "$name ($ref): $runs; base voice $(dc "$TALKER" "voiceline $ref" | grep -oE 'voiceType=[0-9A-F]+')"
	N=$(tr ' |' '__' <<<"$name")
	rwalk "$LISTENER" "$ref" 300 0.8; say "$name: $LISTENER walked to it: $WALKED"
	rwalk "$TALKER" "$ref" 110 0.6; say "$name: $TALKER walked to it: $WALKED"
	dc "$TALKER" focus >/dev/null; dc "$TALKER" 'mouse 5 0' >/dev/null   # the keyboard and mouse, not the pad, the input in use
	opened=0
	for try in 1 2 3; do
		rpress_e "$TALKER" "$ref"
		if wait_for 6 talking "$TALKER"; then opened=1; break; fi
		say "$name: try $try: E (rollover '$ROLLOVER') opened no conversation; menus $(menus "$TALKER")"
		menu_open "$TALKER" BarterMenu && { rkey "$TALKER" $VK_TAB; sleep 1; }
		rwalk "$TALKER" "$ref" 90 0.5
	done
	[ "$opened" = 1 ] || { say "$name: no conversation opened"; continue; }
	TALKS=$((TALKS + 1))
	rmark "$TALKER" "$N"; rmark "$LISTENER" "$N"
	cT0=$(counters "$TALKER"); cL0=$(counters "$LISTENER")
	say "$name: conversation open: $(dc "$TALKER" dialogue | cut -c1-200)"
	S="$REPRO_OUT/samples-$N.txt"; : > "$S"
	n=0; t0=$SECONDS
	while [ $((SECONDS - t0)) -lt "$TALK_MAX" ]; do
		nv=$(dc "$TALKER" "npcvoice $ref"); vl=$(dc "$TALKER" "voiceline $ref"); so=$(dc "$TALKER" sounds | tr '|' '\n' | grep -c "of $ref ")
		echo "$(date +%T.%N | cut -c1-12) $nv | $vl | following=$so" >> "$S"
		quiet "$TALKER" && break
		if menu_open "$TALKER" BarterMenu || menu_open "$TALKER" ContainerMenu || menu_open "$TALKER" PipboyMenu; then
			say "$name: an answer opened $(menus "$TALKER"): shut by a real Tab"; rkey "$TALKER" $VK_TAB; sleep 1.5; continue
		fi
		dm=$(dc "$TALKER" 'dialogue menu')
		if [ "$(field "$dm" buttonsShown)" = true ] && [ "$(field "$(dc "$TALKER" dialogue)" allowInput)" = true ]; then
			[ "$n" -ge "$REPLIES" ] && break
			k=${ANSWERS[$((n % ${#ANSWERS[@]}))]}; n=$((n + 1))
			rkey "$TALKER" "$k" 80; sleep 0.6
		fi
		sleep 0.3
	done
	if ! quiet "$TALKER"; then
		rkey "$TALKER" $VK_TAB 60; wait_for 4 quiet "$TALKER" || { rkey "$TALKER" $VK_ENTER 60; wait_for 4 quiet "$TALKER"; } || {
			say "$name: Tab and Enter did not end it: dialogue reset by the dev channel: $(dc "$TALKER" 'dialogue reset' | cut -c1-120)"; sleep 2; }
	fi
	sleep 4   # the last line's sound in the friend's game
	cT1=$(counters "$TALKER"); cL1=$(counters "$LISTENER")
	rsince "$TALKER" "$N" > "$REPRO_OUT/talk-$N-$TALKER.log"; rsince "$LISTENER" "$N" > "$REPRO_OUT/talk-$N-$LISTENER.log"
	say "$name: $n answers; $TALKER counters $cT0 -> $cT1; $LISTENER counters $cL0 -> $cL1"
	# The talker's lines of this NPC, each subtitle once: did the engine have a voice sound for it here, and what file is it.
	while IFS= read -r sub; do
		[ -n "$sub" ] || continue
		rows=$(grep -F "subtitle='$sub'" "$S" | grep 'talking=true')
		snd=$(grep -oE 'sound=[0-9A-F]+' <<<"$rows" | sort -u | grep -vE 'sound=(0|FFFFFFFF)$' | paste -sd, -)
		fol=$(grep -oE 'following=[1-9]' <<<"$rows" | head -1)
		eng=$(grep -oE "engineFile='[^']*'" <<<"$rows" | grep -v "''" | head -1)
		ours=$(grep -oE " file='[^']*'" <<<"$rows" | grep -v "''" | head -1 | sed 's/^ //')
		info=$(grep -oE 'info=[0-9A-F]+ response=[0-9]+ voiceType=[0-9A-F]+' <<<"$rows" | grep -v 'info=00000000' | head -1)
		LINES=$((LINES + 1))
		if [ -z "$eng" ] && [ -z "$ours" ]; then UNVOICED=$((UNVOICED + 1)); say "  UNVOICED '$sub' (no recording: the engine named no file, the mod resolved none; $info)"; continue; fi
		[ -n "$eng" ] && [ -z "$ours" ] && { NOFILE_OURS=$((NOFILE_OURS + 1)); say "  NO FILE (MOD) '$sub': the engine's $eng, the mod resolved none ($info): the friend can't play it"; }
		if [ -z "$snd" ] && [ -z "$fol" ]; then SILENT_HERE=$((SILENT_HERE + 1)); say "  SILENT HERE '$sub': no voice sound in $TALKER's game ($eng $ours)"
		else say "  heard here '$sub' ($snd $fol; $eng)"; fi
	done < <(grep -oE "subtitle='[^']*'" "$S" | sed -E "s/^subtitle='//; s/'$//" | awk '!seen[$0]++')
	# The friend's game: each line it got, played or why not.
	L="$REPRO_OUT/talk-$N-$LISTENER.log"
	v=$(grep -cE "Conversations: [0-9A-F]{8} says '[Ss]ound" "$L"); VOICED_THERE=$((VOICED_THERE + v))
	grep -E "Conversations: (no voice file|no sound for|a line from .*(not said|held|let go))" "$L" | sed 's/^/  '"$LISTENER"': /' | head -20 | while IFS= read -r l; do say "$l"; done
	dv=$(( $(cnt "$cL1" voiced) - $(cnt "$cL0" voiced) )); dr=$(( $(cnt "$cL1" received) - $(cnt "$cL0" received) ))
	du=$(( $(cnt "$cL1" unplaced) - $(cnt "$cL0" unplaced) )); dm=$(( $(cnt "$cL1" missing) - $(cnt "$cL0" missing) )); dp=$(( $(cnt "$cL1" unplayed) - $(cnt "$cL0" unplayed) ))
	say "$name: $LISTENER got $dr lines, voiced $dv; text only $du, no file $dm, no sound $dp"
	UNHEARD=$((UNHEARD + dm + dp))
	# Text only (no recording named) for a line whose recording exists is a line the friend could not hear.
	if [ "$du" -gt 0 ]; then
		grep -E "Conversations: [0-9A-F]{8} says '.*' \(reported\)" "$REPRO_OUT/talk-$N-$TALKER.log" | head -20 | sed 's/^/  '"$TALKER"' reported: /' | while IFS= read -r l; do say "$l"; done
		UNHEARD=$((UNHEARD + du))
	fi
done

say "talks=$TALKS lines=$LINES silentHere=$SILENT_HERE unheardThere=$UNHEARD (no file there, no sound, or text only) noFileMod=$NOFILE_OURS unvoiced=$UNVOICED voicedThere=$VOICED_THERE"
rkeep host all; rkeep vm all
[ "$TALKS" -gt 0 ] || verdict VOID "no conversation opened"
if [ "$SILENT_HERE" -gt 0 ] || [ "$UNHEARD" -gt 0 ] || [ "$NOFILE_OURS" -gt 0 ]; then
	verdict YES "$SILENT_HERE lines silent in $TALKER's own game, $UNHEARD unheard in $LISTENER's, $NOFILE_OURS with a file the mod can't name"
fi
verdict NO "every voiced line was heard: $LINES lines in $TALKS talks, $VOICED_THERE voiced in $LISTENER's game"
