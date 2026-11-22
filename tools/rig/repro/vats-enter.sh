#!/bin/bash
# vats-enter.sh [who] [rounds]: a game that crashes as its player enters VATS in a session. Player report (on 0.13): the host's
# game crashed whenever he entered VATS while they played together. <who> (host, the default, or vm) is the one in VATS; the
# other is the friend. Each round <who> enters VATS with a real Q, queues shots with real Q presses, plays them with a real E
# (the playback, whose last shot kills: the kill cam), and Tabs out when nothing could be queued. The rounds cycle through:
#   plain      raiders round <who>, the friend beside them, not fighting
#   line       <who> faces the friend's stand-in first, so the first raider stands straight behind it: the stand-in in view and
#              in the line of fire (VATS leaves it out: bVatsSkipsFriends, the default)
#   fight      as plain, and the friend fires real clicks at the raiders while <who> is in VATS (the friend in combat)
#   standin    bVatsSkipsFriends off for the run's player in VATS (dev `vatstargets skip off`, as 0.13.0 had no such option):
#              <who> faces the friend's stand-in, so VATS picks it, and the shots are played at it (the friend has god mode)
# Then RAPID (20) quick entries: in on a real Q and straight out on a real Tab.
# After each round both games must answer and no new crash log may be on either machine (Fallout4.exe's Application Errors,
# which a fast fail leaves without a crash log, are read at the end and when a game stops answering).
# Set-up, said so where it happens: both games to Vault111Ext by console coc (a quiet place with no settlers to fight); each
# player given a 10mm, its ammo and 2000 action points (console additem, equipitem, setav/restoreav); raiders made round <who>
# with the dev channel's spawnenemies (as a random encounter's, shared by the session), held where they stand (console
# setrestrained 1: they still fight) and brought down to 5 health (console damageav) so the first VATS shot kills them.
# REPRO: YES when a game crashed or stopped answering; NO when every round entered VATS and no game crashed; VOID when VATS
# never opened or the scene could not be made.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
. "$RIG/crashlib.sh"
WHO=${1:-host}; ROUNDS=${2:-16}; RAPID=${RAPID:-20}; FRIEND=$(other_game "$WHO"); CELL=${CELL:-Vault111Ext}
RAIDER=${RAIDER:-001AE38D}; PISTOL=00004822; AMMO=0001F276; VK_Q=0x51; VK_D=0x44
VARIANTS=(plain line fight standin)
run_cap $((9000 * (ROUNDS > 16 ? ROUNDS : 16) / 16))   # 16 rounds took up to 71 minutes: 2.5 h, more for more rounds
[ "$WHO" = host ] || [ "$WHO" = vm ] || verdict VOID "usage: vats-enter.sh [host|vm] [rounds]"
need_session

alive_both() { local w; for w in host vm; do wait_for 30 _answers "$w" || return 1; done; }
_answers() { dc "$1" status | grep -q 'ingame=true'; }
vats_up() { [ "$(field "$(dc "$WHO" timescale)" vatsMenu)" = true ]; }
vats_down() { local t; t=$(dc "$WHO" timescale); [ -n "$t" ] && [ "$(field "$t" vatsMenu)" = false ] && [ "$(grep -oE 'vats: mode=[0-9]+' <<<"$t" | cut -d= -f2)" = 0 ]; }
vmode() { dc "$WHO" timescale | grep -oE 'vats: mode=[0-9]+' | cut -d= -f2; }
standin_of() { dc "$1" 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | head -1 | cut -d= -f2; }
refhp() { dc "$1" "refinfo $2" | grep -oE 'hp=[-0-9.]+ dead=[a-z]+'; }
# nearest <who>: the nearest raider alive in that game's enemies list, seen or not
nearest() { dc "$1" 'enemies 3000' | tr ';' '\n' | grep "'Raider'" | grep -oE '^ ?FF[0-9A-F]{8}' | tr -d ' ' | head -1; }
crashes() { CH=$(crash_list_host); CV=$(crash_list_vm); }
# new_crashes [events]: a crash log on either machine that was not there before; with "events" also Fallout4.exe's Application
# Errors since the start (a fast fail writes no crash log), read at the end and when a game stops answering (they are slow)
new_crashes() {
	local nh nv eh="" ev=""; crashes; nh=$(crash_new "$CH0" "$CH"); nv=$(crash_new "$CV0" "$CV")
	[ -n "$1" ] && { eh=$(crash_event_lines "$(crash_events_host "$T0")"); ev=$(crash_event_lines "$(crash_events_vm "$T0")"); }
	NEW="${nh:+host log: $nh; }${nv:+vm log: $nv; }${eh:+host event: $eh; }${ev:+vm event: $ev; }"
	[ -n "$NEW" ]
}
# kit <who>: a 10mm drawn, ammo and action points (console, set-up)
kit() {
	dc "$1" "console player.additem $PISTOL 1" >/dev/null; dc "$1" "console player.additem $AMMO 300" >/dev/null
	dc "$1" "console player.equipitem $PISTOL" >/dev/null; dc "$1" "draw on" >/dev/null
	dc "$1" 'console player.setav actionpoints 2000' >/dev/null; dc "$1" 'console player.restoreav actionpoints 2000' >/dev/null
}
# raiders <n> <radius>: made round <who>, held still and brought down to 5 health (set-up); RAIDERS lists them
raiders() {
	local r hp
	RAIDERS=$(dc "$WHO" "spawnenemies $RAIDER $1 $2" | grep -oE 'ids=[^ ]*' | cut -d= -f2 | tr ',' ' ')
	for r in $RAIDERS; do
		dc "$WHO" "console $r.setrestrained 1" >/dev/null
		hp=$(field "$(dc "$WHO" "refinfo $r")" hp); hp=${hp%.*}
		is_int "$hp" && [ "$hp" -gt 5 ] && dc "$WHO" "console $r.damageav health $((hp - 5))" >/dev/null
	done
}
clear_raiders() { local r; for r in $RAIDERS; do dc "$WHO" "remove $r" >/dev/null; done; RAIDERS=""; }
# first_alive: the first of this round's raiders still alive (the one VATS is turned to)
first_alive() { local r; for r in $RAIDERS; do [ "$(field "$(dc "$WHO" "refinfo $r")" dead)" = false ] && { echo "$r"; return; }; done; }
playback_timer() { dc "$WHO" timescale | grep -oE 'playback=[0-9.]+' | cut -d= -f2; }

# ---- set-up ------------------------------------------------------------------------------------------------------------------------
say "set-up: both games to $CELL by console coc; a 10mm, ammo and 2000 action points each by console"
enter_both "$CELL" 16
for w in host vm; do dc "$w" 'god on' >/dev/null; kit "$w"; done
crashes; CH0=$CH; CV0=$CV; T0=$(date +%s)
[ -n "$CH0" ] && [ -n "$CV0" ] || verdict VOID "a crash folder could not be read (host '$(head -1 <<<"$CH0")', vm '$(head -1 <<<"$CV0")')"
say "crash logs before: host $(crash_count "$CH0") (newest $(crash_stamp "$CH0")), vm $(crash_count "$CV0") (newest $(crash_stamp "$CV0"))"
say "$WHO: $(dc "$WHO" vatstargets | cut -d'|' -f1); rules: $(dc host rules | grep -oE 'realTimeVats=[a-z]+'); $(dc "$WHO" vatstime | cut -c1-160)"
rmark host; rmark vm
OPENED=0; PLAYED=0; KILLS=0; STANDIN_SHOTS=0; NOTARGET=0; R=0

for R in $(seq 1 "$ROUNDS"); do
	v=${VARIANTS[$(((R - 1) % ${#VARIANTS[@]}))]}
	dc "$WHO" 'console player.restoreav actionpoints 2000' >/dev/null; clear_raiders
	P=$(standin_of "$WHO")
	[ -n "$P" ] || verdict VOID "round $R: $WHO's game has no stand-in for the friend ($(dc "$WHO" 'net status' | grep -oE 'status=[a-z]+'))"
	if [ "$v" = line ] || [ "$v" = standin ]; then dc "$WHO" "turnto $P 300 aim" >/dev/null; sleep 0.6; fi
	[ "$v" = standin ] && dc "$WHO" 'vatstargets skip off' >/dev/null || dc "$WHO" 'vatstargets skip on' >/dev/null
	raiders 3 350; sleep 2
	T=$P
	if [ "$v" != standin ]; then T=$(first_alive); [ -n "$T" ] || T=$(nearest "$WHO"); dc "$WHO" "turnto $T 300 aim" >/dev/null; sleep 0.6; fi
	before="target $T $(refhp "$WHO" "$T")"
	FPID=""
	if [ "$v" = fight ]; then   # the friend fires at the raiders (real clicks) through the round
		( for i in $(seq 1 8); do t=$(nearest "$FRIEND"); [ -n "$t" ] && dc "$FRIEND" "turnto $t 300 aim" >/dev/null; dc "$FRIEND" focus >/dev/null; dc "$FRIEND" 'click 80' >/dev/null; sleep 1.2; done ) & FPID=$!
	fi
	rkey "$WHO" $VK_Q 100
	if ! wait_for 4 vats_up; then
		say "round $R ($v): VATS did not open on a real Q ($before; $(dc "$WHO" 'enemies 1500' | tr ';' '\n' | grep -oE "los=[a-z]+ dist=[0-9]+" | paste -sd' ' -))"
		NOTARGET=$((NOTARGET + 1))
	else
		OPENED=$((OPENED + 1)); sleep 1.5
		tgt=$(dc "$WHO" vatstargets)
		rkey "$WHO" $VK_Q 100; sleep 0.6; rkey "$WHO" $VK_Q 100; sleep 0.6
		pb0=$(playback_timer); rkey "$WHO" $VK_E 100
		# played back: VATS's playback mode seen, or its playback timer moved (a playback can begin and end between two reads)
		mode=0; for i in $(seq 1 10); do t=$(dc "$WHO" timescale); [ "$(grep -oE 'vats: mode=[0-9]+' <<<"$t" | cut -d= -f2)" = 1 ] && { mode=1; break; }
			[ -n "$pb0" ] && [ "$(grep -oE 'playback=[0-9.]+' <<<"$t" | cut -d= -f2)" != "$pb0" ] && { mode=1; break; }; sleep 0.3; done
		[ "$mode" = 1 ] && PLAYED=$((PLAYED + 1))
		wait_for 20 vats_down || { rkey "$WHO" $VK_TAB 100; wait_for 5 vats_down || dc "$WHO" 'menu VATSMenu cancel' >/dev/null; }
		after=$(refhp "$WHO" "$T")
		[ "$v" != standin ] && [ "$mode" = 1 ] && grep -q 'dead=true' <<<"$after" && KILLS=$((KILLS + 1))
		[ "$v" = standin ] && [ "$mode" = 1 ] && STANDIN_SHOTS=$((STANDIN_SHOTS + 1))
		say "round $R ($v): VATS opened; $(cut -d'|' -f1 <<<"$tgt")| playback=$([ "$mode" = 1 ] && echo yes || echo no) | $before -> $after"
	fi
	[ -n "$FPID" ] && wait "$FPID" 2>/dev/null
	vats_down || { dc "$WHO" 'menu VATSMenu cancel' >/dev/null; sleep 1; }
	if ! alive_both; then
		sleep 10; new_crashes events
		say "round $R ($v): a game stopped answering: host '$(dc host status | cut -c1-40)', vm '$(dc vm status | cut -c1-40)'; $NEW"
		rkeep host vats-enter; rkeep vm vats-enter
		verdict YES "a game stopped answering in round $R ($v) after $WHO entered VATS ${NEW:+($NEW)}"
	fi
	if new_crashes; then rkeep host vats-enter; rkeep vm vats-enter; verdict YES "a crash in round $R ($v): $NEW"; fi
done
# Quick entries: RAPID times in and straight out again (real Q, then Tab), the raiders at full health and fighting, the look
# turned in turn at a raider and at the friend's stand-in (left out of VATS, bVatsSkipsFriends on); both games asked every 5.
dc "$WHO" 'vatstargets skip on' >/dev/null; clear_raiders
RAIDERS=$(dc "$WHO" "spawnenemies $RAIDER 3 350" | grep -oE 'ids=[^ ]*' | cut -d= -f2 | tr ',' ' '); for r in $RAIDERS; do dc "$WHO" "console $r.setrestrained 1" >/dev/null; done; sleep 2
QUICK=0
for i in $(seq 1 "$RAPID"); do
	if [ $((i % 2)) = 0 ]; then dc "$WHO" "turnto $(standin_of "$WHO") 300 aim" >/dev/null; else t=$(nearest "$WHO"); [ -n "$t" ] && dc "$WHO" "turnto $t 300 aim" >/dev/null; fi
	sleep 0.5; rkey "$WHO" $VK_Q 100
	if wait_for 3 vats_up; then QUICK=$((QUICK + 1)); sleep 1.2; rkey "$WHO" $VK_TAB 100; wait_for 5 vats_down || dc "$WHO" 'menu VATSMenu cancel' >/dev/null; fi
	sleep 0.8
	if [ $((i % 5)) = 0 ] && ! alive_both; then sleep 10; new_crashes events; rkeep host vats-enter; rkeep vm vats-enter; verdict YES "a game stopped answering in quick entry $i ${NEW:+($NEW)}"; fi
done
say "quick entries: VATS opened $QUICK of $RAPID times (real Q, then Tab)"
clear_raiders
if new_crashes events; then rkeep host vats-enter; rkeep vm vats-enter; verdict YES "a crash during the run: $NEW"; fi
rsince host > "$REPRO_OUT/run-host.log"; rsince vm > "$REPRO_OUT/run-vm.log"
say "$WHO: $(dc "$WHO" vatstargets | cut -d'|' -f1); $(dc "$WHO" vatstime | cut -c1-200)"
say "$WHO in VATS: opened $OPENED of $ROUNDS rounds (no target: $NOTARGET), played back $PLAYED, raiders killed in the playback $KILLS, stand-in shot at in VATS $STANDIN_SHOTS"
[ "$OPENED" -gt 0 ] || verdict VOID "VATS never opened in $ROUNDS rounds"
verdict NO "$WHO entered VATS $OPENED times in the rounds and $QUICK quick times ($PLAYED played back, $KILLS kills, $STANDIN_SHOTS at the friend's stand-in) and neither game crashed"
