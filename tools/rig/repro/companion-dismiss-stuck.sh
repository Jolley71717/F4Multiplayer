#!/bin/bash
# companion-dismiss-stuck.sh <who> [swaps]: after a companion is dismissed in a session, a player is left in a conversation or a
# scene that they can't leave and can't answer their way out of, and can't save (reported on the rig: the dismissed Codsworth's
# greeting held the host, Tab and Enter did nothing, no save was possible). <who> (host or vm) dismisses; the other game stays in the
# session beside them, and both games are watched, since either could be the one held.
#
# Each round, in <who>'s game: Codsworth taken on (console cqf FollowersScript.SetCompanion), he and Nick Valentine moved to that
# player (console moveto), then the swap (SetCompanion of Nick), whose settlement list asking where Codsworth goes is closed by a
# real Tab; both players then stand beside the dismissed Codsworth for STAY s; Nick dismissed (console, quiet). Any conversation
# that opens in either game is left the way a player on a keyboard would: a real Tab, then Enter, then the top reply (a real W) up
# to REPLIES times, each when the replies show; one still open after that, or LEAVE s with no reply to give, is HELD. Then each
# game saves by a real quick-save key (F5) and the run checks that a new quick save file appeared: one that doesn't is NO SAVE.
# The virtual pad is never used (the reply buttons would show the pad's icons and the keyboard's keys would do nothing: a
# rig-only state seen on 2026-11-19), and no player walks.
#
# Console set-up, said so: Sanctuary's workbench made <who>'s (the swap asks where to send the dismissed companion only when the
# player owns a settlement), Nick enabled in both games, and Codsworth's first meeting made done in the games MET names (both,
# who, other, none; default both), as a real save past it has it: DialogueCodsworthPostWar's 200 and 250 set by console where
# they aren't done. The rig's worlds are a coc into Sanctuary from the main menu, where his first meeting may never have happened:
# there the swap frees him beside the player and the game's own first-meeting scene (001_CodsworthPostWarIntro, or
# 010_CodsworthPostWar_SkippedCodsworth) can start, a forced conversation that sat with no replies to give in either game on the
# rig (2026-11-20; not compared with the unmodded game). MET=none keeps those worlds as they are, and a first meeting in <who>'s own
# game there (impossible in a real save: his companion was met) is counted apart, not HELD; MET=who leaves the onlooker's unmet,
# as a friend whose character never met him beside a player who has him, the case a real session can have.
#
# REPRO: YES on a HELD or NO SAVE in either game; NO when every round's conversations were left or answered and every save made.
# Each round also counts the onlooker's copies let go to their own AI while the dismissing game was stopped by the list.
# STOP=1 (the default) stops at the first YES. The quick saves made are moved to Saves-aside/<run> in each game's documents.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
WHO=${1:-host}; case "$WHO" in host|vm) ;; *) echo "usage: $0 <host|vm> [swaps]"; exit 2;; esac
OTHER=$(other_game "$WHO")
SWAPS=${2:-5}; STOP=${STOP:-1}; STAY=${STAY:-30}; LEAVE=${LEAVE:-20}; REPLIES=${REPLIES:-30}; MET=${MET:-both}
CODSWORTH=0001CA7D; NICK=00002F25; BENCH=000250FE; POSTWAR=0001E64E
VK_F5=0x74
need_session
held_ref() { dc "$WHO" companions | grep -oE "(^| )Companion=[0-9A-F]{8}" | grep -oE "[0-9A-F]{8}$"; }
dog_ref() { dc "$WHO" companions | grep -oE "DogmeatCompanion=[0-9A-F]{8}" | cut -d= -f2; }
talking() { [ "$(field "$(dc "$1" dialogue)" inDialogue)" = true ]; }
quiet() { [ "$(field "$(dc "$1" dialogue)" inDialogue)" = false ]; }
holds() { [ "$(held_ref)" = "$1" ]; }
free() { local c; c=$(dc "$WHO" companions); [ -n "$c" ] && ! grep -qE "(^| )Companion=[0-9A-F]{8}" <<<"$c"; }
list_up() { menu_open "$WHO" Workshop_CaravanMenu; }
list_shut() { local m; m=$(menus "$WHO"); [ -n "$m" ] && ! grep -q "Workshop_CaravanMenu:" <<<"$m"; }
dismiss() { dc "$WHO" "console cqf Followers \"FollowersScript.DismissCompanion\" $1 0 1" >/dev/null; }
scene_of() { field "$(dc "$1" dialogue)" playerScene; }
has_stage() { dc "$1" "stages $POSTWAR" | grep -oE "done\([0-9]+\)=[0-9,]*" | cut -d= -f2 | tr ',' '\n' | grep -qx "$2"; }

# the quick saves: the newest Quicksave file's name in that game's Saves folder
SAVES_HOST="$HOME/Documents/My Games/Fallout4/Saves"
newest_quick() {
	if [ "$1" = host ]; then ls -t "$SAVES_HOST" 2>/dev/null | grep -m1 -E '^Quicksave.*\.fos$'
	else vmssh '(Get-ChildItem "$env:USERPROFILE\Documents\My Games\Fallout4\Saves\Quicksave*.fos" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1).Name'; fi
}
MADE_HOST=(); MADE_VM=()
# quicksave <who> <round>: a real F5; 0 when a new quick save file appeared within 15 s
quicksave() {
	local g=$1 r=$2 before now i
	before=$(newest_quick "$g"); rkey "$g" $VK_F5 80
	for i in $(seq 1 15); do
		sleep 1; now=$(newest_quick "$g")
		if [ -n "$now" ] && [ "$now" != "$before" ]; then
			if [ "$g" = host ]; then MADE_HOST+=("${now%.fos}"); else MADE_VM+=("${now%.fos}"); fi
			say "round $r: $g saved by a real F5 ($now)"; return 0
		fi
	done
	say "round $r: NO SAVE: $g's real F5 made no quick save in 15 s (newest still '$before'); menus $(menus "$g"); dialogue $(dc "$g" dialogue | cut -c1-260)"
	return 1
}

HELD=0; NOSAVE=0; TALKS=0; N=0; PIP=0; OWNFIRST=0; OPEN_OTHER=0; FALLBACKS=0
# leave <game> <where> <round>: a conversation open now in that game is left by a real Tab, Enter, then answered by the top reply
# (a real W) whenever the replies show, up to REPLIES; HELD when it is still open after that or after LEAVE s with nothing to answer
# (1). One in the dismissing game itself with Codsworth, in a world that never had his first meeting (no 50 or 200 done), is the
# game's own first meeting with the companion the console just took on and let go: a world no real save has (one can't take him on
# before meeting him), counted apart (OWN FIRST MEETING, 2), not HELD. A conversation in the onlooker's game at all is counted
# (opened beside the onlooker): the dismissal's own conversations are the dismissing player's.
leave() {
	local g=$1 where=$2 r=$3 d t0 n=0 idle=0 dm
	d=$(dc "$g" dialogue); TALKS=$((TALKS + 1)); t0=$(date +%T.%N | cut -c1-12)
	[ "$g" = "$OTHER" ] && OPEN_OTHER=$((OPEN_OTHER + 1))
	say "round $r: $g: a conversation is open $where ($t0): $(cut -c1-300 <<<"$d")"
	say "round $r: $g: dialogue menu: $(dc "$g" 'dialogue menu' | cut -c1-300)"
	say "round $r: $g: menus before the Tab: $(menus "$g")"
	rkey "$g" $VK_TAB 60; sleep 1; say "round $r: $g: menus after the Tab: $(menus "$g")"
	if menu_open "$g" PipboyMenu; then
		PIP=$((PIP + 1)); say "round $r: $g: the Tab opened the Pip-Boy over the conversation; a second real Tab shuts it"
		rkey "$g" $VK_TAB 60; sleep 1.5
	fi
	wait_for 4 quiet "$g" && { say "round $r: $g: left by a real Tab"; return 0; }
	rkey "$g" $VK_ENTER 60; wait_for 4 quiet "$g" && { say "round $r: $g: left by Enter (after a Tab that did not)"; return 0; }
	# a forced conversation: answered as a player would, by the top reply each time the replies show
	while ! quiet "$g"; do
		dm=$(dc "$g" 'dialogue menu')
		if [ "$(field "$dm" buttonsShown)" = true ] && [ "$(field "$(dc "$g" dialogue)" allowInput)" = true ]; then
			[ "$n" -ge "$REPLIES" ] && break
			n=$((n + 1)); idle=0; rkey "$g" 0x57 80; sleep 2
		else
			idle=$((idle + 1)); [ "$idle" -ge $((LEAVE * 2)) ] && break; sleep 0.5
		fi
	done
	quiet "$g" && { say "round $r: $g: answered to its end: $n replies (the top one, a real W), scene was $(field "$d" playerScene)"; return 0; }
	if [ "$g" = "$WHO" ] && [ "$(field "$d" speaker)" = "$CODSWORTH" ] && ! has_stage "$g" 50 && ! has_stage "$g" 200; then
		OWNFIRST=$((OWNFIRST + 1))
		say "round $r: OWN FIRST MEETING in $g (the dismissing game; its world never had Codsworth's first meeting: $(dc "$g" "stages $POSTWAR" | grep -oE 'done\([0-9]+\)=[0-9,]*')): not counted as held; scene $(field "$d" playerScene)"
		say "round $r: $g: ended by the dev channel's reset so the run can go on: $(dc "$g" 'dialogue reset' | cut -c1-160)"; sleep 2
		return 2
	fi
	HELD=$((HELD + 1))
	say "round $r: HELD $g $where: a real Tab, Enter and $n replies did not end it (no replies to give for $((idle / 2)) s at the end)"
	say "round $r: $g: dialogue: $(dc "$g" dialogue | cut -c1-400)"
	say "round $r: $g: dialogue menu: $(dc "$g" 'dialogue menu' | cut -c1-400)"
	say "round $r: $g: menus: $(menus "$g"); controls: $(dc "$g" controls | cut -c1-200)"
	say "round $r: $g: scenes: $(dc "$g" scenes | tr '\n' '|' | cut -c1-500)"
	rsince host round > "$REPRO_OUT/held-$HELD-host.log"; rsince vm round > "$REPRO_OUT/held-$HELD-vm.log"
	say "round $r: the round's log lines kept: held-$HELD-host.log, held-$HELD-vm.log"
	quicksave "$g" "$r" || NOSAVE=$((NOSAVE + 1))
	say "round $r: $g: ended by the dev channel's reset so the run can go on: $(dc "$g" 'dialogue reset' | cut -c1-160)"; sleep 2
	return 1
}
# look <round> <where>: both games, a conversation open now in either is left (leave); 1 when one held
look() {
	local g ok=0
	for g in "$WHO" "$OTHER"; do talking "$g" && { leave "$g" "$2" "$1"; [ $? = 1 ] && ok=1; }; done
	return $ok
}

say "companion-dismiss-stuck: $WHO dismisses, $OTHER beside; up to $SWAPS swaps (a real Tab on the list); first meeting made done in: $MET; stop at the first YES: $STOP"
for g in host vm; do dc "$g" "pad unplug" >/dev/null; done
for g in host vm; do
	loc=$(dc "$g" status | grep -oE "location=[0-9A-F]+ '[^']*'")
	case "$loc" in *"Sanctuary"*) ;; *) verdict VOID "set-up: $g is not at Sanctuary ($loc)";; esac
done
[ -n "$(dog_ref)" ] && { say "set-up: Dogmeat dismissed in $WHO's game (console)"; dc "$WHO" "console cqf Followers \"FollowersScript.DismissDogmeatCompanion\" 0 1" >/dev/null; sleep 4; }
h=$(held_ref); [ -n "$h" ] && [ "$h" != "$CODSWORTH" ] && { say "set-up: companion $h dismissed in $WHO's game (console)"; dismiss "$h"; sleep 4; }
for g in host vm; do
	dc "$g" "console $NICK.enable" >/dev/null; sleep 1
	[ "$(field "$(dc "$g" "refinfo $NICK")" disabled)" = false ] || verdict VOID "Nick ($NICK) could not be enabled in $g's game: $(dc "$g" "refinfo $NICK" | cut -c1-120)"
done
case "$MET" in both) meet="host vm";; who) meet=$WHO;; other) meet=$OTHER;; none) meet="";; *) verdict VOID "MET=$MET: both, who, other or none";; esac
for g in $meet; do
	for s in 200 250; do has_stage "$g" $s || { dc "$g" "console setstage $POSTWAR $s" >/dev/null; sleep 2; }; done
	has_stage "$g" 200 && has_stage "$g" 250 || verdict VOID "set-up: Codsworth's first meeting not done in $g's game: $(dc "$g" "stages $POSTWAR" | cut -c1-160)"
done
for g in host vm; do say "set-up: $g's DialogueCodsworthPostWar: $(dc "$g" "stages $POSTWAR" | grep -oE 'stage=[0-9]+|done\([0-9]+\)=[0-9,]*' | paste -sd' ' -)"; done
say "set-up: Nick enabled in both games, Sanctuary made $WHO's (console)"
dc "$WHO" "console $BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 2
[ "$(field "$(dc "$WHO" "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" = true ] || verdict VOID "Sanctuary is not $WHO's"
for g in host vm; do wait_for 10 quiet "$g" || look 0 "at set-up" || true; done

while [ "$N" -lt "$SWAPS" ]; do
	r=$((N + 1)); rmark host round; rmark vm round
	if ! holds "$CODSWORTH"; then
		say "round $r: SetCompanion Codsworth in $WHO's game (console)"
		dc "$WHO" "console cqf Followers \"FollowersScript.SetCompanion\" $CODSWORTH 1 1 1" >/dev/null; sleep 1; dc "$WHO" "console $CODSWORTH.evp" >/dev/null
		wait_for 10 holds "$CODSWORTH" || verdict VOID "round $r: the Companion alias does not hold Codsworth ('$(held_ref)')"
	fi
	dc "$WHO" "console $CODSWORTH.moveto player" >/dev/null; dc "$WHO" "console $NICK.moveto player" >/dev/null; sleep 3
	if ! { wait_for 8 quiet host && wait_for 8 quiet vm; }; then look "$r" "as Codsworth was taken on" || { [ "$STOP" = 1 ] && { N=$r; break; }; }; fi
	say "round $r: SetCompanion Nick in $WHO's game (console): the swap"
	dc "$WHO" "console cqf Followers \"FollowersScript.SetCompanion\" $NICK 1 1 1" >/dev/null
	if wait_for 15 list_up; then
		sleep 1; rkey "$WHO" $VK_TAB 60; say "round $r: a real Tab on the settlement list"
		if ! wait_for 8 list_shut; then
			say "round $r: the list is still up 8 s after the real Tab ($(menus "$WHO")): a second real Tab"
			rkey "$WHO" $VK_TAB 60; wait_for 8 list_shut || verdict VOID "round $r: the settlement list did not close on two real Tabs ($(menus "$WHO"))"
		fi
	else
		say "round $r: the swap's settlement list never opened in $WHO's game ($(menus "$WHO")): Codsworth dismissed without it"
	fi
	wait_for 15 holds "$NICK" || say "round $r: the swap did not finish (alias '$(held_ref)')"
	N=$r
	t0=$SECONDS; bad=0
	while [ $((SECONDS - t0)) -lt "$STAY" ]; do
		if talking host || talking vm; then look "$r" "beside the dismissed Codsworth after the swap" || bad=1; fi
		sleep 2
	done
	for g in "$WHO" "$OTHER"; do talking "$g" || quicksave "$g" "$r" || { NOSAVE=$((NOSAVE + 1)); bad=1; }; done
	# the onlooker's copies let go to their own AI while the dismissing game was stopped (NpcSync's "copy is silent" fallbacks)
	fb=$(rsince "$OTHER" round | grep -cE "runs its own AI here for now: player [0-9]+'s copy is silent")
	FALLBACKS=$((FALLBACKS + fb)); say "round $r: $OTHER's copies let go to their own AI as $WHO's game went silent: $fb"
	[ "$bad" = 1 ] && [ "$STOP" = 1 ] && break
	dismiss "$NICK"; wait_for 15 free || say "round $r: the Companion alias did not empty after Nick's dismissal ('$(held_ref)')"
done
dismiss "$NICK"; sleep 2
rkeep host companion-dismiss-stuck; rkeep vm companion-dismiss-stuck
# the quick saves made, moved aside (never deleted)
aside=companion-dismiss-stuck-$(basename "$REPRO_OUT")
if [ ${#MADE_HOST[@]} -gt 0 ]; then
	mkdir -p "$SAVES_HOST/../Saves-aside/$aside"
	for s in "${MADE_HOST[@]}"; do mv -f "$SAVES_HOST/$s".* "$SAVES_HOST/../Saves-aside/$aside/" 2>/dev/null; done
fi
for s in "${MADE_VM[@]}"; do
	vmssh "\$d=\"\$env:USERPROFILE\Documents\My Games\Fallout4\Saves-aside\\$aside\"; New-Item -ItemType Directory -Force \$d | Out-Null; Move-Item -Force \"\$env:USERPROFILE\Documents\My Games\Fallout4\Saves\\$s.*\" \$d" >/dev/null
done
say "quick saves moved to Saves-aside/$aside: host ${#MADE_HOST[@]}, vm ${#MADE_VM[@]}"
say "swaps: $N; conversations opened: $TALKS ($OPEN_OTHER beside the onlooker, $OWNFIRST own first meetings in the dismissing game); held: $HELD; saves refused: $NOSAVE; the onlooker's copies let go: $FALLBACKS; the leaving Tab opened the Pip-Boy: $PIP"
[ "$HELD" -gt 0 ] || [ "$NOSAVE" -gt 0 ] && verdict YES "a player was held in a conversation they could not leave or answer ($HELD), or could not save ($NOSAVE), in $N swap(s) by $WHO"
verdict NO "no player was held and every save was made in $N swaps by $WHO ($TALKS conversations opened, each left or answered by real keys)"
