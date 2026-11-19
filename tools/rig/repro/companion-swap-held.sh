#!/bin/bash
# companion-swap-held.sh [swaps]: the host's player held in a Codsworth conversation that Tab and Enter don't leave, opened as
# Codsworth is taken back on by SetCompanion after a companion swap (seen on the rig: smoke.sh --only swap_greeting, the first round
# after a real Tab on the settlement list). The host's game alone is driven; the friend's game stays in the session. Each round, as
# the swap_greeting step: Codsworth taken on (console cqf FollowersScript.SetCompanion), he and Nick Valentine moved to the player
# (console moveto), then the swap (SetCompanion of Nick), whose settlement list asking where Codsworth goes is closed by a real Tab;
# then the player stands beside the dismissed Codsworth 30 s; Nick dismissed (console, quiet). A conversation open at either look
# (as Codsworth is taken back on, or after the swap) is left the way a player would: a real Tab, then Enter, then 20 s. One that
# is still open after that is HELD: the game's `dialogue` and `dialogue menu` answers and both logs' lines of that round are kept,
# then the dev channel's `dialogue reset` ends it so the run goes on. Console set-up, said so: Sanctuary made the host's (the swap
# asks where to send the dismissed companion only when the player owns a settlement), Nick enabled.
# Seen on 0.20.1 in a coc world (Out of Time short of Codsworth's first meeting): the swap frees Codsworth beside the player and
# his first-meeting conversation (DialogueCodsworthPostWar) starts; Tab doesn't leave a forced scene, and on the rig the reply
# buttons show the pad's icons, so the keyboard's reply keys do nothing while the pad's buttons answer.
# Up to <swaps> swaps (default 30); STOP=1 (the default) stops at the first HELD. REPRO: YES on a HELD, NO when none in all swaps.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
SWAPS=${1:-30}; STOP=${STOP:-1}; STAY=${STAY:-30}; LEAVE=${LEAVE:-20}
CODSWORTH=0001CA7D; NICK=00002F25; BENCH=000250FE
need_session
held_ref() { dc host companions | grep -oE "(^| )Companion=[0-9A-F]{8}" | grep -oE "[0-9A-F]{8}$"; }
dog_ref() { dc host companions | grep -oE "DogmeatCompanion=[0-9A-F]{8}" | cut -d= -f2; }
talking() { [ "$(field "$(dc host dialogue)" inDialogue)" = true ]; }
quiet() { [ "$(field "$(dc host dialogue)" inDialogue)" = false ]; }
holds() { [ "$(held_ref)" = "$1" ]; }
free() { local c; c=$(dc host companions); [ -n "$c" ] && ! grep -qE "(^| )Companion=[0-9A-F]{8}" <<<"$c"; }
list_up() { menu_open host Workshop_CaravanMenu; }
list_shut() { local m; m=$(menus host); [ -n "$m" ] && ! grep -q "Workshop_CaravanMenu:" <<<"$m"; }
dismiss() { dc host "console cqf Followers \"FollowersScript.DismissCompanion\" $1 0 1" >/dev/null; }
HELD=0; TALKS=0; N=0; PIP=0
# leave <where> <round>: a conversation open now is left by a real Tab, Enter, then LEAVE s; HELD when not
leave() {
	local where=$1 r=$2 d t0
	d=$(dc host dialogue); TALKS=$((TALKS + 1)); t0=$(date +%T.%N | cut -c1-12)
	say "round $r: a conversation is open $where ($t0): $(cut -c1-260 <<<"$d")"
	say "round $r: menus before the Tab: $(menus host)"
	rkey host $VK_TAB 60; sleep 1; say "round $r: menus after the Tab: $(menus host)"
	menu_open host PipboyMenu && { PIP=$((PIP + 1)); say "round $r: the Tab opened the Pip-Boy over the conversation (it pauses the game)"; }
	wait_for 4 quiet && { say "round $r: left by a real Tab"; return 0; }
	rkey host $VK_ENTER 60; wait_for 5 quiet && { say "round $r: left by Enter (after a Tab that did not)"; return 0; }
	wait_for "$LEAVE" quiet && { say "round $r: ended by itself within $LEAVE s of a Tab and Enter that did not"; return 0; }
	HELD=$((HELD + 1))
	say "round $r: HELD $where: a real Tab, Enter and $LEAVE s did not leave it"
	say "round $r: dialogue: $(dc host dialogue | cut -c1-400)"
	say "round $r: dialogue menu: $(dc host 'dialogue menu' | cut -c1-400)"
	say "round $r: menus: $(menus host); controls: $(dc host controls | cut -c1-200)"
	say "round $r: scenes: $(dc host scenes | tr '\n' '|' | cut -c1-500)"
	rsince host round > "$REPRO_OUT/held-$HELD-host.log"; rsince vm round > "$REPRO_OUT/held-$HELD-vm.log"
	say "round $r: the round's log lines kept: held-$HELD-host.log, held-$HELD-vm.log"
	# does a reply end it? (a conversation that waits for the player's choice is not held, only unfinished): the choice keys in turn
	# a Pip-Boy over the conversation (the leaving Tab opened it): shut by a second real Tab, then does the conversation end by itself?
	if menu_open host PipboyMenu; then
		rkey host $VK_TAB 60; sleep 1.5
		say "round $r: a second real Tab with the Pip-Boy up: menus now $(menus host)"
		wait_for 30 quiet && { say "round $r: the conversation ended by itself within 30 s of the Pip-Boy being shut ($(dc host dialogue | cut -c1-160))"; return 1; }
		say "round $r: still in the conversation 30 s after the Pip-Boy was shut: $(dc host dialogue | grep -oE '(speaker|talking|voiceTimer|playerScene|allowInput)=[^ ]*' | paste -sd' ' -)"
	fi
	local k
	for k in 0x26 0x28 0x25 0x27 0x31; do
		rkey host "$k" 80; sleep 4
		quiet && { say "round $r: ended by a reply: key $k ($(dc host dialogue | cut -c1-200))"; return 1; }
		say "round $r: after key $k: $(dc host dialogue | grep -oE '(speaker|playerScene|buttonsShown|phase|topic)=[^ ]*' | paste -sd' ' -)"
	done
	# the reply buttons' labels are the gamepad's icons after the rig's virtual pad was used (the UI in pad mode): the keys above
	# then do nothing, and the pad's own buttons answer. Each that answers moves the conversation on (one reply); it is said, not
	# taken as the hold's end unless the conversation ends
	local bt answered=""
	for bt in y a; do
		[ "$(field "$(dc host 'dialogue menu')" buttonsShown)" = true ] || sleep 3
		dc host "pad button $bt 150" >/dev/null; sleep 4
		quiet && { say "round $r: ended by a reply: the pad's $bt"; dc host "pad unplug" >/dev/null; return 1; }
		say "round $r: after the pad's $bt: $(dc host dialogue | grep -oE '(talking|playerScene|allowInput)=[^ ]*' | paste -sd' ' -); $(dc host 'dialogue menu' | grep -oE 'buttonsShown=[a-z]+')"
		answered="$answered $bt"
	done
	dc host "pad unplug" >/dev/null
	say "round $r: the keyboard's reply keys did nothing; the pad's buttons ($answered) were sent (a conversation that goes on after them waits on the player's replies, it is not stuck)"
	say "round $r: ended by the dev channel's reset: $(dc host 'dialogue reset' | cut -c1-160)"; sleep 2
	return 1
}

say "companion-swap-held: up to $SWAPS swaps (real Tab on the list), stop at the first held: $STOP"
loc=$(dc host status | grep -oE "location=[0-9A-F]+ '[^']*'")
case "$loc" in *"Sanctuary"*) ;; *) say "set-up: the host is not at Sanctuary ($loc): console coc SanctuaryExt"; dc host focus >/dev/null; dc host "console coc SanctuaryExt" >/dev/null; sleep 5; wait_ingame host 60;; esac
[ -n "$(dog_ref)" ] && { say "set-up: Dogmeat dismissed (console)"; dc host "console cqf Followers \"FollowersScript.DismissDogmeatCompanion\" 0 1" >/dev/null; sleep 4; }
h=$(held_ref); [ -n "$h" ] && [ "$h" != "$CODSWORTH" ] && { say "set-up: companion $h dismissed (console)"; dismiss "$h"; sleep 4; }
dc host "console $NICK.enable" >/dev/null; sleep 1
[ "$(field "$(dc host "refinfo $NICK")" disabled)" = false ] || verdict VOID "Nick ($NICK) could not be enabled: $(dc host "refinfo $NICK" | cut -c1-120)"
say "set-up: Nick enabled, Sanctuary made the host's (console)"
dc host "console $BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 2
[ "$(field "$(dc host "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" = true ] || verdict VOID "Sanctuary is not the host's"

while [ "$N" -lt "$SWAPS" ]; do
	r=$((N + 1)); rmark host round; rmark vm round
	# Codsworth taken (back) on
	if ! holds "$CODSWORTH"; then
		say "round $r: SetCompanion Codsworth (console)"
		dc host "console cqf Followers \"FollowersScript.SetCompanion\" $CODSWORTH 1 1 1" >/dev/null; sleep 1; dc host "console $CODSWORTH.evp" >/dev/null
		wait_for 10 holds "$CODSWORTH" || verdict VOID "round $r: the Companion alias does not hold Codsworth ('$(held_ref)')"
	fi
	dc host "console $CODSWORTH.moveto player" >/dev/null; dc host "console $NICK.moveto player" >/dev/null; sleep 3
	if ! wait_for 10 quiet; then leave "as Codsworth was taken back on" "$r" || { [ "$STOP" = 1 ] && break; }; fi
	# the swap, the list closed by a real Tab
	say "round $r: SetCompanion Nick (console): the swap"
	dc host "console cqf Followers \"FollowersScript.SetCompanion\" $NICK 1 1 1" >/dev/null
	wait_for 15 list_up || verdict VOID "round $r: the swap's settlement list never opened ($(menus host))"
	sleep 1; rkey host $VK_TAB 60; say "round $r: a real Tab on the settlement list"
	if ! wait_for 8 list_shut; then
		say "round $r: the list is still up 8 s after the real Tab ($(menus host)): closed by the plugin's cancel"
		dc host "menu Workshop_CaravanMenu cancel" >/dev/null; sleep 2
	fi
	wait_for 15 holds "$NICK" || say "round $r: the swap did not finish (alias '$(held_ref)')"
	N=$r
	# the look beside the dismissed Codsworth
	t0=$SECONDS; opened=""
	while [ $((SECONDS - t0)) -lt "$STAY" ]; do talking && { opened=1; break; }; sleep 2; done
	if [ -n "$opened" ]; then leave "beside the dismissed Codsworth after the swap" "$r" || { [ "$STOP" = 1 ] && break; }; fi
	dismiss "$NICK"; wait_for 15 free || say "round $r: the Companion alias did not empty after Nick's dismissal ('$(held_ref)')"
done
# a held round that stopped the run: the next round's take-back would have counted it; the run's conversations so far are said
dismiss "$NICK"; sleep 2; holds "$CODSWORTH" || true
rkeep host companion-swap-held; rkeep vm companion-swap-held
say "swaps: $N; conversations opened: $TALKS; held: $HELD; the leaving Tab opened the Pip-Boy: $PIP"
[ "$HELD" -gt 0 ] && verdict YES "the host's player was held in a conversation a real Tab and Enter did not leave: $HELD time(s) in $N swap(s)"
verdict NO "no conversation held the host's player in $N swaps ($TALKS opened, each left by a real key)"
