#!/bin/bash
# friendbot.sh: the friend bot (dev `friendbot`, src/game/FriendBotPlan.h) drives the VM's player after the host's, on the
# engine's own AI (Game.SetPlayerAIDriven and a package made at run time): it follows, goes through a load door after the host,
# fights what fights the two of them, waits when told, and lets go of the player under a menu, a conversation, a save and a load.
# Nothing of the VM's player is moved by the rig while the bot drives it: no setpos, no walkto, no movement keys. The host is
# walked by the pad (tools/rig/walkto.sh) and goes through a door with E, as in the other repros.
# Set-up, said so where it happens: both games to Valentine's office ($CELL, an interior whose door leads out to Diamond City) by
# console coc; both games to the Institute's concourse
# ($INNER, its doors lead to other interiors) by console coc and both players placed near one of its doors by console setpos;
# there a raider placed by dev spawnenemies in the host's game for the fight (in Diamond City it was placed out of the friend's
# sight, among the guards); a conversation started by the console's activate when the friend's real E does nothing.
# (Vault 81 was the first choice: arrived by coc, its intercom's conversation held the friend's game and nothing ended it.)
# Steps (each gets its own verdict line, "step <name>: YES|NO - why", in the REPRO convention: YES = the bot failed it):
#   follow-int   the host walks off in the office; the friend comes within 250 units in 30 s
#   door-out     the host goes out through the office's door; the friend is in the host's cell (Diamond City) in 90 s
#   follow-ext   the host walks 600 units off outside; the friend comes within 400 units in 40 s
#   wait         friendbot wait; the host walks 600 units off; the friend stays within 150 units; friendbot follow brings it back
#   fight        (in the Institute, after door-int) a raider 500 units from the host; the friend's log says "FriendBot: fight" and its pad fires in 40 s
#   door-in      the host goes in through the nearest load door; the friend is in the host's cell in 90 s
#   keys         the friend's own keys work while the bot drives: a real Tab opens the VM's Pip-Boy
#   menu         the VM's Pip-Boy (the real Tab, else the dev channel's `menu PipboyMenu`): the bot lets go (aiDriven=false);
#                shut, it drives again
#   dialogue     the VM's player talks to the NPC nearest (a real E): the bot lets go; the conversation ended, it drives again
#   save-load    the VM's quick save and quick load (real F5, F9; else the console's save and load, said): the bot lets go
#                before the save and drives again after the save and after the load
#   door-int     in the Institute's concourse, the host goes through a load door to another interior; the friend follows in 90 s
#   off          friendbot off: aiDriven=false
#   no-crash     both games still in the game, no new crash log on either machine
# A door step whose friend was in the host's new cell before the host went is no test of the door: YES, said so.
# REPRO: YES when a step failed (its line and the friend's FriendBot lines say how), NO when every step worked, VOID when the scene
# could not be made (no session, no load door the host could take).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
CELL=${CELL:-DmndValentines01}
OFFICE_DOOR=${OFFICE_DOOR:-000022CB}
TALK_TO=${TALK_TO:-}                        # the NPC the friend talks to (empty: the nearest NPC to the friend in the office)
INNER=${INNER:-InstituteConcourse}
INNER_DOOR=${INNER_DOOR:-00020BCE}          # one of the concourse's doors to another Institute interior
RAIDER=${RAIDER:-001AE38D}
VK_PIPBOY=0x09; VK_F5=0x74; VK_F9=0x78
run_cap 2700
need_session
on_end dc vm "friendbot off"

bot() { dc vm "friendbot $*"; }
dist_host_vm() { dist_xy "$(pos_of host)" "$(pos_of vm)"; }
FAILED=""
# step <name> YES|NO <why>: a step's verdict; a YES also says the friend's last FriendBot lines and its status
step() {
	if [ "$2" = YES ]; then
		FAILED="$FAILED $1"
		rlog vm 400 | grep 'FriendBot:' | grep -v 'FriendBot: status' | tail -10 | sed 's/^/    vm log: /' | tee -a "$REPRO_OUT/run.log"
		say "    vm: $(bot status)"
	fi
	say "step $1: $2 - $3"
}
crashes() { if [ "$1" = host ]; then ls "$HOST_F4SE"/crash-*.log 2>/dev/null | wc -l; else vmssh '(Get-ChildItem "$env:USERPROFILE\Documents\My Games\Fallout4\F4SE\crash-*.log" -ErrorAction SilentlyContinue).Count'; fi; }
CRASH_HOST=$(crashes host); CRASH_VM=$(crashes vm)
st() { field "$(bot status)" "$1"; }
driven() { [ "$(st aiDriven)" = true ]; }
let_go() { [ "$(st aiDriven)" = false ] && [ "$(st state)" = paused ]; }
follow_close() { d=$(dist_host_vm); is_int "$d" && [ "$d" -lt "${NEAR:-400}" ]; }
same_cell() { [ "$(cell_of vm)" = "$HOST_CELL" ]; }
# host_walk <dx> <dy>: the host walks (the pad) that far from where it stands
host_walk() { local hx hy; read -r hx hy <<<"$(pos_of host)"; rwalk host "$(( ${hx%.*} + $1 ))" "$(( ${hy%.*} + $2 ))" 80 || say "the host's walk ended short ($WALKED)"; }
# host_through <label> [door...]: the host walks (the pad) to each door given, else the 4 nearest, and presses E (twice at most)
# until its cell changes, waiting 40 s after each E (an elevator rides before it loads). WENT: that door
_moved_on() { local c; c=$(cell_of host); [ -n "$c" ] && [ "$c" != "$C0" ]; }
host_through() {
	local label=$1 d i; shift
	C0=$(cell_of host); WENT=""
	for d in ${*:-$(doors_near host | head -4)}; do
		say "$label: the host walks to door $d (the pad) and opens it (E)"
		rwalk host "$d" 150 || say "$label: the walk ended short ($WALKED)"
		for i in 1 2; do
			rpress_e host "$d"; say "$label: the host pressed E on '$ROLLOVER'"
			if wait_for 40 _moved_on; then sleep 4; wait_ingame host 60; WENT=$d; return 0; fi
		done
		say "$label: the host is still in cell $C0"
	done
	return 1
}
# door_step <name> [door...]: the host goes through (host_through); the friend must be in the host's new cell in 90 s
door_step() {
	local name=$1 vc; shift
	vc=$(cell_of vm)
	host_through "$name" "$@" || verdict VOID "$name: no door near the host's player took them elsewhere"
	HOST_CELL=$(cell_of host)
	if [ "$vc" = "$HOST_CELL" ]; then
		step "$name" YES "no test: the friend was in the host's new cell $HOST_CELL before the host went (a door step before failed)"
	elif wait_for 90 same_cell; then
		NEAR=400 wait_for 30 follow_close
		step "$name" NO "the friend came through from $vc to the host's cell $HOST_CELL ($(dist_host_vm) units from the host 30 s on; doors used $(st doorsUsed))"
	else
		step "$name" YES "the friend was not in the host's cell $HOST_CELL 90 s after the host went through $WENT (the friend is in '$(cell_of vm)')"
	fi
}

say "set-up: both games to $CELL by console coc"
enter_both "$CELL" 16
bot status | grep -q 'friendbot=' || verdict VOID "the VM's plugin has no friendbot command (an older build)"
[ "$(cell_of host)" = "$(cell_of vm)" ] || verdict VOID "the two games are not in one cell after the coc"

say "follow-int: the friend bot on in the VM; the host walks off in the office (the pad)"
say "vm: $(bot on | cut -c1-160)"
sleep 3
host_walk -300 -200
if NEAR=250 wait_for 30 follow_close; then step follow-int NO "the friend came within 250 units ($(dist_host_vm)) on a $(st mode) package"
else step follow-int YES "the friend did not come within 250 units of the host in 30 s (now $(dist_host_vm))"; fi

say "door-out: the host goes out through the office's door $OFFICE_DOOR (walk and E)"
door_step door-out "$OFFICE_DOOR"

say "follow-ext: the host walks 600 units off outside (the pad)"
host_walk 600 0
if NEAR=400 wait_for 40 follow_close; then step follow-ext NO "the friend came within 400 units ($(dist_host_vm))"
else step follow-ext YES "the friend did not come within 400 units of the host in 40 s (now $(dist_host_vm))"; fi

say "wait: friendbot wait; the host walks off (the pad)"
say "vm: $(bot wait | cut -c1-120)"
sleep 2
V0=$(pos_of vm)
host_walk -600 0
sleep 5
moved=$(dist_xy "$V0" "$(pos_of vm)")
if is_int "$moved" && [ "$moved" -le 150 ]; then
	say "vm: $(bot follow | cut -c1-120)"
	if NEAR=400 wait_for 40 follow_close; then step wait NO "the friend stayed ($moved units) and came back after follow ($(dist_host_vm))"
	else step wait YES "the friend stayed ($moved units) but after follow did not come back within 400 units in 40 s ($(dist_host_vm))"; fi
else
	bot follow >/dev/null
	step wait YES "the friend moved $moved units while told to wait"
fi


say "door-in: the host goes in through the office's door from outside (the nearest load doors; walk and E)"
door_step door-in

say "keys and menu: the VM's Pip-Boy, a real Tab while the bot drives"
NEAR=250 wait_for 20 driven
pip() { menu_open vm PipboyMenu; }
rkey vm $VK_PIPBOY 100
if wait_for 4 pip; then KEYS=real; step keys NO "a real Tab opened the Pip-Boy while the bot drove"
else
	KEYS=dev; step keys YES "a real Tab opened no Pip-Boy while the bot drove (menus $(menus vm)); the menu step opens it by the dev channel"
	dc vm "menu PipboyMenu" >/dev/null
fi
if wait_for 6 pip && wait_for 6 let_go; then
	why="let go ($(bot status | grep -oE 'state=paused \("[^"]*"\)'), aiDriven=false) under the Pip-Boy (opened the $KEYS way)"
	rkey vm $VK_PIPBOY 100; wait_for 4 pip && dc vm "menu PipboyMenu force" >/dev/null  # let go, the player's own Tab shuts it
	if wait_for 10 driven; then step menu NO "$why; shut, it drives again"
	else step menu YES "$why, but after it shut the bot did not drive again in 10 s (menus $(menus vm))"; fi
else
	step menu YES "the bot did not let go under the Pip-Boy (menus $(menus vm))"
	pip && { rkey vm $VK_PIPBOY 100; sleep 2; pip && dc vm "menu PipboyMenu force" >/dev/null; }
fi
sleep 2

say "dialogue: the VM's player talks to the NPC nearest it (a real E)"
if [ -z "$TALK_TO" ]; then
	TALK_TO=$(dc vm "refs 45 800" | tr ';' '\n' | grep -vE '^ *(FF|00000014)' | grep -oE '^ *[0-9A-F]{8}' | tr -d ' ' | head -1)
fi
if [ -z "$TALK_TO" ]; then step dialogue YES "no test: no NPC near the friend to talk to"
else
	say "dialogue: $(dc vm "refinfo $TALK_TO" | cut -c1-90)"
	rwalk host "$TALK_TO" 150 >/dev/null; NEAR=250 wait_for 20 follow_close
	talking() { menu_open vm DialogueMenu || [ "$(field "$(dc vm 'net status')" talkingTo)" = "$TALK_TO" ]; }
	# A line the NPC says to us alone opens no DialogueMenu and sets no talkingTo; the bot's own pause for it shows it began.
	started() { talking || rsince vm talk | grep -q "FriendBot: .*a conversation"; }
	rmark vm talk
	for i in 1 2; do rpress_e vm "$TALK_TO"; wait_for 5 started && break; done
	TALKED="a real E"
	if ! started; then TALKED="the console's $TALK_TO.activate player (a real E did nothing)"; say "dialogue: $TALKED (set-up)"; dc vm "console $TALK_TO.activate player" >/dev/null; wait_for 6 started; fi
	if ! started; then step dialogue YES "the VM's E on $TALK_TO ('$ROLLOVER') opened no conversation (menus $(menus vm))"
	elif wait_for 6 let_go || rsince vm talk | grep -q "FriendBot: paused: a conversation"; then
		why="let go ($(bot status | grep -oE 'state=paused \("[^"]*"\)')) in the conversation started by $TALKED"
		for i in 1 2 3 4; do rkey vm $VK_TAB 100; sleep 2; talking || break; done
		talking && dc vm "menu DialogueMenu cancel" >/dev/null
		if wait_for 15 driven; then step dialogue NO "$why; ended, it drives again"
		else step dialogue YES "$why, but after it ended the bot did not drive again in 15 s (menus $(menus vm))"; fi
	else
		step dialogue YES "the bot did not let go in the conversation (menus $(menus vm))"
		for i in 1 2 3 4; do rkey vm $VK_TAB 100; sleep 2; talking || break; done
	fi
fi
sleep 3

say "save-load: the VM's quick save and quick load (real F5, F9; the console's save and load if the keys make none, said)"
newest_vm() { vmssh '(Get-ChildItem "$env:USERPROFILE\Documents\My Games\Fallout4\Saves\*.fos" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1).Name'; }
NEAR=250 wait_for 20 driven
rmark vm save; before=$(newest_vm)
rkey vm $VK_F5 80; sleep 8
how=real
if [ "$(newest_vm)" = "$before" ]; then
	how=console; say "save-load: a real F5 made no save; the console's save friendbotrig (set-up)"
	dc vm "console save friendbotrig" >/dev/null; sleep 8
fi
saved=$(rsince vm save | grep -c 'FriendBot: let go of the player (before a save or load)')
wait_for 15 driven; after_save=$(st aiDriven)
rmark vm load
if [ "$how" = real ]; then rkey vm $VK_F9 80; else dc vm "console load friendbotrig" >/dev/null; fi
sleep 15; wait_ingame vm 60; sleep 5
loaded=$(rsince vm load | grep -c 'FriendBot: ')
wait_for 30 driven; after_load=$(st aiDriven)
if [ "$saved" -ge 1 ] && [ "$after_save" = true ] && [ "$after_load" = true ]; then
	step save-load NO "the $how way: let go before the save, driving again after it and after the load"
else step save-load YES "the $how way: let go before the save: $saved line(s); driving after the save: $after_save; FriendBot lines after the load: $loaded; driving after the load: $after_load"; fi
need_session

say "door-int: both games to $INNER by console coc; both players placed 300 units from its door $INNER_DOOR by console setpos (set-up)"
bot wait >/dev/null
enter_both "$INNER" 16
ri=$(dc host "refinfo $INNER_DOOR"); ex=$(field "$ri" x); ey=$(field "$ri" y); ez=$(field "$ri" z)
is_int "${ex%.*}" || verdict VOID "door-int: no door $INNER_DOOR in the host's $INNER"
# toward the concourse's middle (its doors stand at its corners)
read -r px py <<<"$(awk -v x="${ex%.*}" -v y="${ey%.*}" 'BEGIN{l=sqrt(x*x+y*y); printf "%d %d", x-x/l*300, y-y/l*300}')"
dc host "setpos 14 $px $py ${ez%.*}" >/dev/null; dc vm "setpos 14 $((px - 60)) $((py - 60)) ${ez%.*}" >/dev/null; sleep 4
say "vm: $(bot follow | cut -c1-120)"
sleep 4
door_step door-int "$INNER_DOOR"
dc vm status | grep -q 'interior=true' || step door-int YES "the friend's cell is not an interior"

say "fight: a raider 500 units from the host (dev spawnenemies, set-up)"
rmark vm fight
R=$(dc host "spawnenemies $RAIDER 1 500" | grep -oE 'ids=[^ ]*' | cut -d= -f2 | cut -d, -f1)
if [ -z "$R" ]; then step fight YES "no test: the host's game placed no raider (dev spawnenemies $RAIDER 1 500)"
else
	fought() { rsince vm fight | grep -q 'FriendBot: fight' && [ "$(st fire)" -gt 0 ] 2>/dev/null; }
	if wait_for 40 fought; then step fight NO "$(rsince vm fight | grep 'FriendBot: fight' | head -1 | grep -oE 'fight [0-9A-F]+'); aim=$(st aim) fire=$(st fire)"
	else step fight YES "the friend's bot did not fight the raider $R in 40 s (fight lines: $(rsince vm fight | grep -c 'FriendBot: fight'), fire=$(st fire))"; fi
	dc host "console $R.kill" >/dev/null
fi
sleep 5
NEAR=400 wait_for 30 follow_close

say "off"
bot off >/dev/null
sleep 2
if grep -q 'aiDriven=false' <<<"$(bot status)"; then step off NO "aiDriven=false"; else step off YES "the friend's player is still AI-driven"; fi

ch=$(crashes host); cv=$(crashes vm)
if [ "$ch" = "$CRASH_HOST" ] && [ "$cv" = "$CRASH_VM" ] && wait_ingame host 5 && wait_ingame vm 5; then step no-crash NO "both games in the game, no new crash log"
else step no-crash YES "crash logs host $CRASH_HOST -> $ch, vm $CRASH_VM -> $cv; host $(dc host status | grep -oE 'ingame=[a-z]+') vm $(dc vm status | grep -oE 'ingame=[a-z]+')"; fi
rkeep vm friend; rkeep host host
[ -z "$FAILED" ] && verdict NO "every step worked: the friend bot followed, went through the doors, fought, waited and let go"
verdict YES "steps failed:$FAILED"
