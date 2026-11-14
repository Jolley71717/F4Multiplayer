#!/bin/bash
# steps.sh: the standing two-game set as functions that assert (source assert.sh first; smoke.sh and hits.sh do).
# Each step_* drives the games the way the old soak scripts did (scripted real keys and clicks, console placement
# only to reach a scene, never injected or teleported movement in place of walking) and records named checks.
# Thresholds are variables so a run can say what it used; the reasons are in docs/RELEASING.md and the PR that added this.

. "$RIG/clocklib.sh"   # clock_lineup, wall_step: the two machines' traces lined up by their clocks

# movement (trace.sh): passing runs of 0.8.8 to 0.10.1 were mean error 0 to 5 u, best lag 100 to 150 ms
T_MOVE_MEAN=${T_MOVE_MEAN:-10}          # u, mean position error at the best lag, each way
T_MOVE_LAG=${T_MOVE_LAG:-400}           # ms, best lag
T_MOVE_SAMPLES=${T_MOVE_SAMPLES:-100}   # samples each side of the fit
# NPC trace: a stand-in floating or sunk shows as dz over 120 u; a trace with no tracks is a failed trace, not a pass
T_NPC_TRACKS=${T_NPC_TRACKS:-10}
T_HP_DIFF=${T_HP_DIFF:-2}               # hp, the same actor in both games
# health agreement needs at least this many actors read in BOTH games (an actor one game cannot answer for is only reported as
# missing): with fewer, "nothing differs" would mean "almost nothing was compared", which is a FAIL, not a pass
T_HP_MIN_COMPARED=${T_HP_MIN_COMPARED:-3}
# power armor: the stand-in's distance from the driver at the same moment (both games' traces lined up on the clock), and after stopping
T_PA_MOVING=${T_PA_MOVING:-200}; T_PA_SETTLED=${T_PA_SETTLED:-40}
# perf (docs/PERF.md): ours_in_hitches must stay 0. The 0.8.7 baseline had p99 at most 2.1 ms (1.7 host, 2.1 VM) and p50 0.13 to 0.35 ms;
# the limits below (p50 1.0 ms, p99 4.0 ms) are about twice the baseline's worst p99 and several times its p50: loose on purpose,
# they catch a real regression and not rig noise. Compare against the baseline in docs/PERF.md for anything finer.
T_PERF_P50=${T_PERF_P50:-1.0}; T_PERF_P99=${T_PERF_P99:-4.0}
T_WS_GROWTH_MB=${T_WS_GROWTH_MB:-1000} # working set growth between two warm points of a run (a warning, not a failure: no baseline yet)

# ---- small readers ---------------------------------------------------------------------------------------------
# actor_hp <who> <ref> -> "dead=false hp=57" (empty when the game has no such actor)
actor_hp() { dc "$1" "combat $2" | grep -o 'dead=[a-z]* hp=-\?[0-9.]*'; }
# actors_on_host: refs of the actors the host sees within 3000 u (living and dead), one per line
actors_on_host() { host 'actors 3000' | grep -o "[0-9A-F]\{8\} '" | cut -c1-8; }
# crash_snapshot: remember both machines' crash logs, and the time (C_T0): a game crash in Windows' Application log since then
# counts too (a fast fail, abort(), writes no crash log)
crash_snapshot() { C_HOST0=$(crash_list_host); C_VM0=$(crash_list_vm); C_T0=$(date +%s); }
crash_verdict() {  # crash_verdict <label>: no new crash log on either machine, and no game crash in either Application log
	local hn vn h v he ve
	h=$(crash_list_host); v=$(crash_list_vm); hn=$(crash_new "$C_HOST0" "$h"); vn=$(crash_new "$C_VM0" "$v")
	case "$h" in ok*) ;; *) warn "$1 crash logs (host)" "listing unreadable";; esac
	case "$v" in ok*) ;; *) warn "$1 crash logs (vm)" "listing unreadable (ssh)";; esac
	if [ -n "$C_T0" ]; then
		he=$(crash_events_host "$C_T0"); ve=$(crash_events_vm "$C_T0")
		case "$he" in ok*) ;; *) warn "$1 crash events (host)" "Windows' Application log unreadable";; esac
		case "$ve" in ok*) ;; *) warn "$1 crash events (vm)" "Windows' Application log unreadable (ssh)";; esac
	fi
	he=$(crash_event_lines "$he"); ve=$(crash_event_lines "$ve")
	if [ -n "$hn$he" ]; then fail "$1 crash logs (host)" "new: $(crash_names "$hn" | tr '\n' ' ')${he:+the game crashed (Application log): $(cut -d' ' -f2- <<<"$he" | tr '\n' ';')}"; else pass "$1 crash logs (host)" "none new ($(crash_count "$h") on disk)"; fi
	if [ -n "$vn$ve" ]; then fail "$1 crash logs (vm)" "new: $(crash_names "$vn" | tr '\n' ' ')${ve:+the game crashed (Application log): $(cut -d' ' -f2- <<<"$ve" | tr '\n' ';')}"; else pass "$1 crash logs (vm)" "none new ($(crash_count "$v") on disk)"; fi
}
# working_set <who>: the game's working set in MB (empty if it can't be read)
working_set() {
	if [ "$1" = host ]; then powershell -NoProfile -Command "[int]((Get-Process Fallout4 -ErrorAction SilentlyContinue | Select-Object -First 1).WorkingSet64/1MB)" 2>/dev/null | tr -d '\r'
	else vmssh '[int]((Get-Process Fallout4 -ErrorAction SilentlyContinue | Select-Object -First 1).WorkingSet64/1MB)'; fi
}
dll_md5() {  # dll_md5 <who>
	if [ "$1" = host ]; then md5sum "$HOST_GAME/Data/F4SE/Plugins/F4Multiplayer.dll" 2>/dev/null | cut -c1-32
	else vmssh "(Get-FileHash 'C:/Program Files (x86)/Steam/steamapps/common/Fallout 4/Data/F4SE/Plugins/F4Multiplayer.dll' -Algorithm MD5).Hash" | tr -d '[:space:]' | tr A-F a-f; fi
}

# ---- preflight --------------------------------------------------------------------------------------------------
# dll_preflight: both machines have the same F4Multiplayer.dll in the game's plugins folder, and it is the one the run was given:
# CHECK_DLL=<a built F4Multiplayer.dll, or its md5> (a branch's build), or SMOKE_ZIP=<an rc zip> (the zip's). rig.sh deploy copies
# the main checkout's build folder to both games, so a run meant for a branch's build has to have copied that build there first;
# one that didn't tests main's DLL and nothing says so (2026-11-01: a hits run's result was read as "the VM may not have run the
# branch's DLL", and nothing in it could say which DLL either game had). A running game holds its DLL open, so the file is the one
# it loaded. A mismatch, or no reading, stops the run: every check after it would be about another build. With neither given the
# hash is only reported (a WARN), so the run's results name the build they tested.
dll_preflight() {
	local hd vd want="" what=""
	hd=$(dll_md5 host); vd=$(dll_md5 vm); A_DLL=$hd
	if [ -n "$CHECK_DLL" ]; then
		if [[ $CHECK_DLL =~ ^[0-9A-Fa-f]{32}$ ]]; then want=$(tr A-F a-f <<<"$CHECK_DLL"); what="CHECK_DLL"
		else want=$(md5sum "$CHECK_DLL" 2>/dev/null | cut -c1-32); what="CHECK_DLL $CHECK_DLL"; fi
	elif [ -n "$SMOKE_ZIP" ]; then want=$(unzip -p "$SMOKE_ZIP" F4SE/Plugins/F4Multiplayer.dll 2>/dev/null | md5sum | cut -c1-32); what="SMOKE_ZIP $SMOKE_ZIP"
		[ "$want" = d41d8cd98f00b204e9800998ecf8427e ] && want=""   # the md5 of nothing: no DLL in the zip
	fi
	if [ -z "$hd" ] || [ -z "$vd" ]; then rig_broken dll "the DLL both games run is read" "host '$hd' vm '$vd': a machine's DLL could not be hashed"; [ -z "$A_RIG_STOPPED" ] && _rig_stop dll; return 1; fi
	if [ "$hd" != "$vd" ]; then rig_broken dll "same DLL on both machines" "host $hd vm $vd: the games run different builds"; [ -z "$A_RIG_STOPPED" ] && _rig_stop dll; return 1; fi
	pass "same DLL on both machines" "$hd"
	if [ -n "$CHECK_DLL$SMOKE_ZIP" ]; then
		if [ -z "$want" ]; then rig_broken dll "the DLL is the one given" "$what: no DLL hash could be read from it"; [ -z "$A_RIG_STOPPED" ] && _rig_stop dll; return 1; fi
		if [ "$hd" != "$want" ]; then rig_broken dll "the DLL is the one given" "$what is $want, both games run $hd: the deploy put another build there"; [ -z "$A_RIG_STOPPED" ] && _rig_stop dll; return 1; fi
		pass "the DLL is the one given" "$what ($want)"
	else
		warn "the DLL is the one given" "no CHECK_DLL (or SMOKE_ZIP) given: both games run $hd, unchecked against the build meant for the run"
	fi
}
# _keys_move_ok <who>: a forward key press (0.8 s) moves that game's player at least 20 u (no record; rig_keys_move is the check)
_keys_move_ok() {
	local p0 p1 d
	rig_input_ready "$1"; sleep 1; p0=$(dc "$1" pos); dc "$1" "key 0x57 800" >/dev/null; sleep 1.6; p1=$(dc "$1" pos)
	d=$(awk -v a="$(field "$p0" x)" -v b="$(field "$p0" y)" -v c="$(field "$p1" x)" -v e="$(field "$p1" y)" 'BEGIN{if (a=="" || c=="") {print ""; exit}; printf "%d", sqrt((c-a)^2 + (e-b)^2)}')
	is_num "$d" && [ "$d" -ge 20 ]
}
# _wants_step <step>: smoke.sh will run that step (its --only and --skip; outside smoke.sh, every step is wanted)
_wants_step() { case " ${skip:-} " in *" $1 "*) return 1;; esac; [ -z "${steps:-}" ] && return 0; case " $steps " in *" $1 "*) return 0;; esac; return 1; }
# _vm_restart_rejoin <log name>: the VM's game is stopped, started again by its own start script and rejoins the host's session
# (the reconnect step's sequence; logs in $A_OUT/<name>-vm.log and <name>-join.log). 0 once the VM's game says joined. With no
# name, each call's logs get their own (rejoin-1, rejoin-2...): unnamed calls once wrote one "-join.log" over the other.
VRR_N=0
_vm_restart_rejoin() {
	local n=$1; [ -n "$n" ] || { VRR_N=$((VRR_N + 1)); n=rejoin-$VRR_N; }
	VRR_LOG=$n
	vmssh 'Stop-Process -Name Fallout4 -Force -ErrorAction SilentlyContinue; Start-Sleep 4; powershell -NoProfile -ExecutionPolicy Bypass -File C:\F4MP\in-vm-3-game.ps1 -WaitSeconds 150' > "$A_OUT/$n-vm.log" 2>&1
	vmssh 'Get-Content "$env:USERPROFILE\Documents\My Games\Fallout4\F4SE\F4Multiplayer_dev.token" -Raw' | tr -d '[:space:]' > "$STAGE/vm-dev.token"
	bash "$STAGE/rig.sh" join > "$A_OUT/$n-join.log" 2>&1
	local i; for i in 1 2 3 4 5; do sleep 3; dc vm 'net status' | grep -q 'status=joined' && return 0; done   # rig.sh join waits 60 s for it; 15 s more
	return 1
}
# _sws_rejoin <label>: _vm_restart_rejoin for a step that goes on in the session, checked: a VM that didn't rejoin fails here, with
# the VM's last Steam event (2026-11-12, run 5: the host's Steam lost its link, Steam dropped the lobby, and both rejoins got
# "could not enter lobby (response 2)"; the step went on with the VM alone, its "in-session" checks out of the session, and failed
# much later on a build the host never heard of, its detail a VM log window 13 minutes stale). 1 when it didn't.
_sws_rejoin() {
	if _vm_restart_rejoin; then pass "$1: set-up: the VM's game restarts and rejoins the host's session" "$(dc vm 'net status' | grep -oE 'status=[a-z]+ id=[0-9]+')"; return 0; fi
	fail "$1: set-up: the VM's game restarts and rejoins the host's session" "VM $(dc vm 'net status' | grep -oE 'status=[a-z]+ id=[0-9]+'); the VM's last Steam event: $(dc vm steam | grep -oE "last='[^']*'" | cut -c1-160); the host's $(dc host 'net status' | grep -oE 'lobby=[0-9]+'); logs $VRR_LOG-vm.log, $VRR_LOG-join.log"
	return 1
}
# _host_restart_rejoin <log name>: the host's game is closed and started again with the DLL it has (restart-game.ps1 -NoDeploy,
# to its main menu), then rig.sh join loads both games into the lobby cell and the VM joins the new session (logs in
# $A_OUT/<name>-host.log and <name>-join.log). For a host game whose mouse died (hits.sh preflight). A restart that failed (a
# non-zero exit, or no "Main menu reached" in its log) or a join that failed is said in the run's output with the log's last
# line (a user's profile path in it shortened to ~), and returns 1.
_log_tail() { tr -d '\r' < "$1" 2>/dev/null | grep -v '^[[:space:]]*$' | tail -1 | sed -E 's#[A-Za-z]:[\/]Users[\/][^\/ ]+#~#g; s#/c/Users/[^/ ]+#~#g' | cut -c1-200; }
_host_restart_rejoin() {
	local rc jrc menu=yes
	powershell -NoProfile -ExecutionPolicy Bypass -Command "& '$(cygpath -w "$REPO/tools/restart-game.ps1")' -NoDeploy -Cell '' -GamePath '$(cygpath -w "$HOST_GAME")'" > "$A_OUT/$1-host.log" 2>&1; rc=$?
	grep -q 'Main menu reached' "$A_OUT/$1-host.log" || menu=no
	[ $rc = 0 ] && [ $menu = yes ] || echo "$1: the host's game restart failed (restart-game.ps1 exit $rc, main menu reached: $menu; $1-host.log): $(_log_tail "$A_OUT/$1-host.log")"
	bash "$STAGE/rig.sh" join > "$A_OUT/$1-join.log" 2>&1; jrc=$?
	[ $jrc = 0 ] || echo "$1: rig.sh join after the host's restart failed (exit $jrc; $1-join.log): $(_log_tail "$A_OUT/$1-join.log")"
	sleep 3
	[ $rc = 0 ] && [ $menu = yes ] && [ $jrc = 0 ]
}
step_preflight() {
	section "preflight"
	local a h v
	rig_up "both games up"; rig_ssh_up "ssh to the VM"
	rig_host_load "preflight: the host is not building or testing"
	pads_off   # a virtual pad left plugged by an interrupted run takes the keyboard and mouse from this one
	# A VM game whose window is in front and takes no key at all (2026-11-07: "its input still never reached the player", the
	# whole soak void) comes back when the game is started again: the VM's game is restarted and rejoins, as the reconnect step
	# does, and the check made after it (a HEAL). Its keys staying dead after that is a broken rig, and the run stops.
	if [ -z "$A_RIG_STOPPED" ] && ! _keys_move_ok vm && ! _keys_move_ok vm; then
		echo "preflight: the VM's keys move nothing: its game is restarted and rejoins"
		_vm_restart_rejoin preflight-restart; _vm_minsteam; sleep 2; vm focus >/dev/null; sleep 1
		_keys_move_ok vm && healed "preflight: vm's keys move its player" "a forward key moved nothing, twice; the VM's game was restarted and rejoined, and its keys move the player again"
	fi
	for a in host vm; do rig_keys_move $a "$a's keys move its player"; done
	# The host's ini settings the chosen steps need (a rig set-up, not the mod's): read up front, so a run set up without them
	# stops here as the rig's (VERDICT: RIG) instead of failing the bench and quest-actor steps every round (2026-11-07: a soak
	# started on the rig's resting ini).
	local need="" rr; rr=$(host rules)
	_wants_step bench && [ "$(field "$rr" sharedStorage)" != true ] && need="$need bSharedStorage = true (rules say sharedStorage='$(field "$rr" sharedStorage)');"
	_wants_step questactors && [ "$(field "$rr" quests)" != safe ] && need="$need sQuests = safe (rules say quests='$(field "$rr" quests)');"
	if [ -n "$need" ]; then
		rig_unhealed setup "preflight: the host's ini has what the chosen steps need" "the host's F4Multiplayer.ini wants${need%;}: set it, restart the host's game and run again"
		[ -z "$A_RIG_STOPPED" ] && { A_RIG_STOPPED=1; echo "STOP: the host's ini is not set up for these steps"; a_summary; }
	fi
	pass "preflight: the host's ini has what the chosen steps need" "sharedStorage=$(field "$rr" sharedStorage) quests=$(field "$rr" quests)"
	ns host; ns vm
	expect_match "host session is up" "$(nsf host status)" "^(hosting|host|joined)$"
	expect_eq "vm session joined" "$(nsf vm status)" joined
	crash_snapshot; log_mark host; log_mark vm
	dll_preflight
	WS0_HOST=$(working_set host); WS0_VM=$(working_set vm)
	host 'perf reset' >/dev/null; vm 'perf reset' >/dev/null
	pass "baselines taken" "crash logs host $(crash_count "$C_HOST0") vm $(crash_count "$C_VM0"); working set host ${WS0_HOST:-?} MB vm ${WS0_VM:-?} MB"
}

# ---- movement -----------------------------------------------------------------------------------------------------
# trace_line <file> <label>: the compare line for one direction
_mv_check() {  # _mv_check <logfile> <who sees whom label> <line prefix>
	local l lag mean max n
	l=$(grep "^$3:" "$1" | head -1)
	if [ -z "$l" ]; then fail "movement $2" "no result line (see $1)"; return; fi
	case "$l" in *"not enough samples"*) fail "movement $2" "$l"; return;; esac
	lag=$(sed -n 's/.*best lag \([0-9]*\) ms.*/\1/p' <<<"$l"); mean=$(sed -n 's/.*best lag [0-9]* ms: mean err \([0-9]*\) u.*/\1/p' <<<"$l")
	max=$(sed -n 's/.*best lag [0-9]* ms: mean err [0-9]* u, max \([0-9]*\) u.*/\1/p' <<<"$l")
	n=$(sed -n 's/.*samples owner=\([0-9]*\) viewer=\([0-9]*\).*/\1 \2/p' <<<"$l")
	case "$l" in *CAPPED*) fail "movement $2: fit" "best lag is the search limit: a clock or a stall, not a fit";; *"best at 0 ms"*) fail "movement $2: fit" "best lag at the 0 ms floor: check the clocks";; *) pass "movement $2: fit" "found at $lag ms";; esac
	expect_le "movement $2: lag" "$lag" "$T_MOVE_LAG" ms
	expect_le "movement $2: mean error" "$mean" "$T_MOVE_MEAN" u
	local s1 s2; read -r s1 s2 <<<"$n"; expect_ge "movement $2: owner samples" "$s1" "$T_MOVE_SAMPLES"; expect_ge "movement $2: viewer samples" "$s2" "$T_MOVE_SAMPLES"
	A_NOTE="$A_NOTE max err $max u;"
}
step_movement() {
	section "movement both ways"
	local rc
	# Into Sanctuary first: round 1 starts there from the join, but a soak round starts wherever the churn before it ended (an
	# interior, 2026-11-06), and placing by coordinates alone left each game without the other's stand-in.
	enter_both SanctuaryExt 14
	place host -80500 89300 7784 45; place vm -80380 89400 7784 45; sleep 3
	bash "$RIG/trace.sh" run 18 > "$A_OUT/movement.log" 2>&1; rc=$?
	# Exit 3: the clocks could not line the traces up (clocklib.sh), so nothing was fitted: the rig's. Placed again and traced
	# once more (the same walk); a second one is recorded as a rig fault, not a failure.
	if [ $rc -eq 3 ]; then
		echo "movement: $(grep -m1 'VM clock: RIG\|not tracing\|could not' "$A_OUT/movement.log"); placed again and traced once more"
		cp -f "$A_OUT/movement.log" "$A_OUT/movement-clock1.log"
		place host -80500 89300 7784 45; place vm -80380 89400 7784 45; sleep 3
		bash "$RIG/trace.sh" run 18 > "$A_OUT/movement.log" 2>&1; rc=$?
		if [ $rc -eq 3 ]; then rig_fault clock "movement trace ran" "twice: $(grep -m1 'VM clock: RIG\|not tracing\|could not' "$A_OUT/movement.log"); clocks set in the last 6 min: $(clock_steps 6)"; return; fi
	fi
	if [ $rc -ne 0 ]; then fail "movement trace ran" "trace.sh refused or failed: $(grep -m1 'not tracing\|could not\|not fitting\|error' "$A_OUT/movement.log")"; return; fi
	pass "movement trace ran" "clock $(sed -n 's/^VM clock: \(-\?[0-9]*\) ms from.*/\1/p' "$A_OUT/movement.log" | head -1) ms off, drift $(sed -n 's/.*drift \(-\?[0-9]*\) ms.*/\1/p' "$A_OUT/movement.log" | head -1) ms"
	_mv_check "$A_OUT/movement.log" "VM player seen by host" "VM player as the host sees them"
	_mv_check "$A_OUT/movement.log" "host player seen by VM" "host player as the VM sees them"
}

# ---- combat: NPC trace and health agreement (Corvega) --------------------------------------------------------------
# hp_compare <label>: every actor the host sees, its health in both games; records one check, prints the odd ones.
hp_compare() {
	local a h v hh vv d n=0 bad=0 miss=0 odd="" hs vs
	for a in $(actors_on_host); do
		h=$(actor_hp host "$a"); v=$(actor_hp vm "$a"); hh=$(field "$h" hp); vv=$(field "$v" hp); n=$((n + 1))
		hs=$(field "$h" dead); vs=$(field "$v" dead)
		if [ -z "$hh" ] || [ -z "$vv" ]; then miss=$((miss + 1)); odd="$odd $a:missing"; continue; fi
		d=$(awk -v a="$hh" -v b="$vv" 'BEGIN{d=a-b; if(d<0)d=-d; printf "%d", d}')
		if [ "$hs" = true ] && [ "$vs" = true ]; then continue; fi   # dead in both: the number of a dead actor means nothing
		if [ "$hs" != "$vs" ]; then bad=$((bad + 1)); odd="$odd $a:dead=$hs/$vs"; elif [ "$d" -gt "$T_HP_DIFF" ]; then bad=$((bad + 1)); odd="$odd $a:hp=$hh/$vv"; fi
	done
	HP_N=$n; HP_BAD=$bad; HP_MISS=$miss
	if [ $n -lt 5 ]; then fail "$1 health: actors compared" "only $n actors listed"; return; fi
	if [ $((n - miss)) -lt "$T_HP_MIN_COMPARED" ]; then fail "$1 health: actors compared" "only $((n - miss)) of $n actors read in both games (want at least $T_HP_MIN_COMPARED), $miss missing in one game:$odd"; return; fi
	if [ $bad -eq 0 ]; then pass "$1 health agrees (within $T_HP_DIFF hp)" "$((n - miss)) of $n actors compared, $miss missing in one game"; else fail "$1 health agrees (within $T_HP_DIFF hp)" "$bad of $n differ:$odd"; fi
}
step_combat() {
	section "combat (Corvega: NPC trace, health agreement)"
	local a n gun rows flagged tracks i ids shots=0
	enter_both CorvegaAssemblyPlant01 18
	echo "owners: host $(host 'net status' | grep -o 'npcs: owned=[0-9]* fast=[0-9]* mirrored=[0-9]*') vm $(vm 'net status' | grep -o 'npcs: owned=[0-9]* fast=[0-9]* mirrored=[0-9]*')"
	host "console player.equipitem 0015B043" >/dev/null; host "draw on" >/dev/null; sleep 1
	bash "$RIG/dcfast.sh" both "trace start 20" >/dev/null; host focus >/dev/null; sleep 0.5
	for n in 1 2 3 4 5 6; do
		a=$(host 'actors 3000' | grep -o "^[0-9A-F]\{8\}\|; [0-9A-F]\{8\}" | tr -d '; ' | head -3 | tail -1)
		[ -n "$a" ] && host "turnto $a 300 12" >/dev/null
		# a real click (assert.sh host_input): refused when another window covers the host's game, which is the rig's failure, recorded
		# as such, and the rest of the host's shots are not tried (with none sent, the health check after the host shot would agree trivially: it is not made)
		sleep 0.5; host_input "combat: the host's click reaches its game (shot $n)" -Click -HoldMs 700 || break; shots=$((shots + 1)); sleep 3.5
	done
	bash "$RIG/dcfast.sh" both "key 0x57 2000" >/dev/null; sleep 4
	host "trace stop" >/dev/null; vm "trace stop" >/dev/null
	cp "$HOST_F4SE/F4Multiplayer_trace.csv" "$A_OUT/host-npc.csv"; scp -q -i "$VM_KEY" "$VM_SSH:$VM_F4SE/F4Multiplayer_trace.csv" "$A_OUT/vm-npc.csv"
	rows="host $(wc -l < "$A_OUT/host-npc.csv") vm $(wc -l < "$A_OUT/vm-npc.csv")"
	bash "$RIG/trace.sh" npcs "$A_OUT/host-npc.csv" "$A_OUT/vm-npc.csv" > "$A_OUT/npc-tracks.txt" 2>&1
	tracks=$(grep -c 'samples=' "$A_OUT/npc-tracks.txt"); flagged=$(grep -c 'FLOATING' "$A_OUT/npc-tracks.txt")
	expect_ge "NPC trace: rows in both files" "$(( $(wc -l < "$A_OUT/host-npc.csv") < $(wc -l < "$A_OUT/vm-npc.csv") ? $(wc -l < "$A_OUT/host-npc.csv") : $(wc -l < "$A_OUT/vm-npc.csv") ))" 1000 "($rows)"
	expect_ge "NPC trace: NPCs tracked in both games" "$tracks" "$T_NPC_TRACKS"
	expect_zero "NPC trace: NPCs floating or sunk (dz over 120 u)" "$flagged"
	if [ "$shots" -gt 0 ]; then sleep 4; hp_compare "after the host shot"; else echo "after the host shot: health not compared (the host's click was refused, no shot went)"; fi
	# the joiner's turn: gun, ammo, aim at each living actor in turn, real clicks
	vm "console player.additem 00004822 1" >/dev/null; vm "console player.additem 0001F276 200" >/dev/null; vm "console player.equipitem 00004822" >/dev/null; vm "draw on" >/dev/null; sleep 1
	for a in $(actors_on_host | head -10); do
		[ "$(field "$(vm "combat $a")" dead)" = "true" ] && continue
		for i in 1 2; do vm "turnto $a 300" >/dev/null; sleep 0.4; vm "click 120" >/dev/null; sleep 1.2; done
	done
	sleep 5; hp_compare "after the VM shot"
}

# menu_open <who> <menu> <seconds>: the menu is open in that game (dev `menus`: "open=HUDMenu:2051.3,ContainerMenu:1.2 ..."),
# waiting up to that many seconds for it
menu_open() { local w=0; while :; do grep -qE "[=,]$2:" <<<"$(dc "$1" menus | grep -oE 'open=[^ ]*')" && return 0; [ "$w" -ge "$(( $3 * 4 ))" ] && return 1; sleep 0.25; w=$((w + 1)); done; }
# menu_up_for <who> <menu> <seconds> <wait>: waits up to <wait> seconds for that menu to have been open at least <seconds> (`menus`
# gives each open menu its seconds open)
menu_up_for() { local i s; for i in $(seq 1 $(( $4 * 4 ))); do s=$(dc "$1" menus | grep -oE "[=,]$2:[0-9.]+" | head -1 | cut -d: -f2); [ -n "$s" ] && _cmp "$s" ">=" "$3" && return 0; sleep 0.25; done; return 1; }
# keys_seen <who>: the keyboard messages that game's window has taken off its queue so far (dev `mousedev` keys=: its raw input, which
# Windows hands only to the window in front; empty from a build without it)
keys_seen() { field "$(dc "$1" mousedev)" keys; }
# clear_menus <who>: closes what would swallow the next activate or key in that game: a message box (Enter: its one button,
# OK), the Pip-Boy, a terminal or a container menu (Tab). 2026-11-01: a Tab after a take opened the Pip-Boy, and an ammo stack
# added to a magazine rack opened "You can only place Magazines on the Rack"; each made every activate after it open nothing.
# Echoes what it closed; false when something is still open after four tries.
clear_menus() {
	local i m closed=""
	for i in 1 2 3 4; do
		m=$(dc "$1" menus | grep -oE 'open=[^ ]*')
		if grep -q 'MessageBoxMenu:' <<<"$m"; then dc "$1" focus >/dev/null; dc "$1" "key 0x0D 60" >/dev/null; closed="$closed MessageBoxMenu"
		elif grep -qE '(PipboyMenu|TerminalMenu|TerminalHolotapeMenu|ContainerMenu):' <<<"$m"; then dc "$1" focus >/dev/null; dc "$1" "key 0x09 60" >/dev/null; closed="$closed $(grep -oE '(PipboyMenu|TerminalMenu|TerminalHolotapeMenu|ContainerMenu)' <<<"$m" | head -1)"
		else echo "${closed# }"; return 0; fi
		sleep 1.2
	done
	echo "${closed# } (still open: $m)"; return 1
}
has_items() { grep -qE "[0-9A-F]{8}x[0-9]+" <<<"$1"; }   # contents: "REF: 000FD9A8x1 0003377Bx1 (2 stacks)" or "REF: empty"
stacks_of() { grep -oE "[0-9A-F]{8}x[0-9]+" <<<"$1" | tr x ' '; }   # contents -> "<item> <count>" lines
count_in() { stacks_of "$1" | awk -v i="$2" '$1==i {n+=$2} END{print n+0}'; }   # count_in <contents> <item>
# contents_ok <contents>: a whole answer of `contents`, every stack listed ("REF: <item>x<n> ... (N stacks)", N at most 60, the
# most it lists; or "REF: empty"). An error, a timed-out read (empty), "no inventory" or a list cut short is no reading: it must
# never pass as "nothing left in it".
_CONTENTS_RE='^[0-9A-F]{8}:( [0-9A-F]{8}x-?[0-9]+)* (\(([0-9]+) stacks\)|empty)$'
contents_ok() { [[ $1 =~ $_CONTENTS_RE ]] && { [ -z "${BASH_REMATCH[3]}" ] || [ "${BASH_REMATCH[3]}" -le 60 ]; }; }
# The loot the session shares (src/game/LootScope.h, Of): a take from a chest is everyone's unless the item is a weapon or armor
# (only the base form would travel), a key, a note or a book (each player collects their own), or featured or a quest item
# (rare in a chest, not told apart here). By form type (dev `edid`: ARMO 29, BOOK 30, WEAP 43, KEYM 47, NOTE 50).
declare -A _ITYPE
shared_item() { local t; t=${_ITYPE[$1]}; [ -z "$t" ] && { t=$(dcf vm "edid $1" type); _ITYPE[$1]=${t:-?}; }; case "$t" in 29|30|43|47|50|"?") return 1;; esac; return 0; }
# _loot: the VM loots the host's chests near Red Rocket the way a player does: opens each (activate), takes all (R in the
# container menu) and closes it (Tab). Console removeallitems used to stand in for the take; it goes through the same container
# event, but the old check then wanted the host's chest EMPTY, which the design never does: weapons, armor, keys, notes and books
# stay in a friend's copy, and so does what only the host's own roll of the chest put there (LootSync TryEmpty). Asserted: every
# chest's take emptied it in the VM's game (the menu take happened), and no item of the session's kind the VM took is left in the
# host's copy (each take went across, and the emptying gave up the rest of what the host's roll had more of).
# Which chests (2026-11-01): every container within 2500 u, not the first eight the list gives. Left out, each with its reason in
# looted.txt: the workshop and its container, racks and displays (a magazine rack answers an ammo stack with a message box that
# blocks every activate after it), a locked chest (it opens the lockpick), one that can't be read, and one a console activate opens
# no menu for (a WARN). Menus left open (a Pip-Boy a stray Tab opened, after the terminals or a take) are closed first: the
# 00152CD5 and 000587FD "opened no container menu" of earlier runs came right after the terminal step. Every chosen chest is
# stocked in BOTH games with the same stack first (console additem, 10mm rounds: a from-nothing add the mod never reports), so
# each game's copy holds a shared stack whatever earlier runs took or each game rolled: the cross-game check always has something
# to see. Up to LOOT_CHESTS (3) are looted.
LOOT_ITEM=0001F276; LOOT_N=7; LOOT_CHESTS=${LOOT_CHESTS:-3}
_loot() {
	local conts c name hb vb va ha i n vn taken=0 chests=0 notaken="" lost="" nl left="" kept=0 other=0 nomenu="" opened unread="" hunread="" skip cl how k0 k1 k2="" kr takehow=""
	LOOTED="$A_OUT/looted$(rsuf).txt"; : > "$LOOTED"
	cl=$(clear_menus vm) || { fail "loot: set-up: no menu open in the VM's game before looting" "$cl"; return; }
	[ -n "$cl" ] && echo "closed in the VM's game before looting: $cl" >> "$LOOTED"
	conts=$(host 'refs 31 2500' | tr ';' '\n' | grep -oE "[0-9A-F]{8} base=[0-9A-F]+ '[^']*'" | sed -E "s/ base=[0-9A-F]+ /|/")
	while IFS='|' read -r c name; do
		[ -n "$c" ] || continue
		[ $chests -ge "$LOOT_CHESTS" ] && break
		skip=""
		case "$name" in *Workshop*) skip="a workshop (its own menu)";; *Rack*|*Display*|*Mannequin*) skip="takes only its own kind of item (a rack or display)";; esac
		[ -z "$skip" ] && [ "$(dcf host "refinfo $c" locked)" = yes ] && skip="locked (opens the lockpick)"
		[ -z "$skip" ] && [ "$(dcf vm "refinfo $c" locked)" = yes ] && skip="locked in the VM's game"
		if [ -z "$skip" ]; then
			host "console $c.additem $LOOT_ITEM $LOOT_N" >/dev/null; vm "console $c.additem $LOOT_ITEM $LOOT_N" >/dev/null; sleep 1
			cl="$(clear_menus host)$(clear_menus vm)"   # a container that takes only its own kind says so in a message box
			hb=$(host "contents $c"); vb=$(vm "contents $c")
			if [ -n "$cl" ]; then skip="the stocking opened a menu (closed: $cl)"
			elif ! contents_ok "$hb" || ! contents_ok "$vb"; then skip="contents not read (host: $hb | vm: $vb)"
			elif [ "$(count_in "$hb" $LOOT_ITEM)" -lt $LOOT_N ] || [ "$(count_in "$vb" $LOOT_ITEM)" -lt $LOOT_N ]; then skip="the stocked stack is not in both copies (host: $hb | vm: $vb)"; fi
		fi
		if [ -n "$skip" ]; then echo "$c $name: left out: $skip" >> "$LOOTED"; continue; fi
		# The take is pressed only once the container menu is open, and the activate is tried again once. A chest that opens by a
		# script of its own never opens the menu for a console activate: it is left out (a WARN), not counted as a take that failed.
		clear_menus vm >/dev/null; vm focus >/dev/null; sleep 0.3; opened=0
		for i in 1 2; do vm "console $c.activate player" >/dev/null; menu_open vm ContainerMenu 4 && { opened=1; break; }; echo "$c: activate $i opened no container menu" >> "$LOOTED"; done
		if [ $opened = 0 ]; then nomenu="$nomenu $c"; continue; fi
		chests=$((chests + 1)); printf '%s host %s | vm %s\n' "$c" "$hb" "$vb" >> "$LOOTED"
		# R (take all) also closes the menu: a Tab after it opens the Pip-Boy, and every activate after that opens nothing
		# (2026-11-01: one chest looted, then each one after it "opened no container menu"). Tab only while the menu is still up.
		# R is pressed once the menu has been up a second (its movie takes no input while it loads), and each press is logged with
		# what tells whether it reached the game: the window in front as the key went (`key`'s front=), the keyboard messages the
		# game's window took off its queue (`mousedev` keys=: its raw input, which Windows hands only to the window in front) before,
		# 1.5 s after and 3.5 s after (a late rise is a late press, not a lost one), and the input tally (`focus`). The counts are
		# trustworthy: in the 0.14.0-rc1 soak (2026-11-07) every R that took raised keys= by 2 (down and up), as PR 332's review
		# asked to see before a resend gated on them. So R is pressed again once, and only, when the menu stayed up AND the press
		# is seen never to have reached the game (keys= flat 3.5 s on, or the window not in front as it went): a press the rig lost,
		# recorded HEAL when the second takes. A press that reached the game and left the menu up is the mod's failure (or the
		# game's), as always. Two presses that both never reached it are the rig's: a rig fault (input), not a failure.
		menu_up_for vm ContainerMenu 1 4
		for i in 1 2; do
			vm focus >/dev/null; k0=$(keys_seen vm); kr=$(vm "key 0x52 100" | grep -oE 'front=[a-z]+'); sleep 1.5
			k1=$(keys_seen vm); k2=""
			if ! menu_open vm ContainerMenu 0; then how="R: the menu closed"
			else sleep 2; k2=$(keys_seen vm); how="R: the menu stayed up"; fi
			how="$how (keys $k0 -> $k1${k2:+ -> $k2}; key $kr; $(dc vm focus | grep -oE 'input: .*' | cut -c1-160))"
			echo "$c: press $i: $how" >> "$LOOTED"; takehow="$takehow $c: $how;"
			# lost: the menu still up, and the press seen not to have reached the game's window
			menu_open vm ContainerMenu 0 || break
			if [ "$kr" = front=no ] || { is_int "$k0" && is_int "$k2" && [ "$k2" -le "$k0" ]; }; then
				[ $i = 1 ] && { echo "$c: the press never reached the game's window: pressed again" >> "$LOOTED"; continue; }
				lost="$lost $c"
			fi
			break
		done
		[ $i = 2 ] && ! menu_open vm ContainerMenu 0 && healed "loot: the take's R reached the game" "$c: the first R never reached the game's window (keys flat, $kr), the second took"
		menu_open vm ContainerMenu 0 && { vm "key 0x09 60" >/dev/null; sleep 1; }
		cl=$(clear_menus vm); [ -n "$cl" ] && echo "$c: closed after the take: $cl" >> "$LOOTED"
		va=$(vm "contents $c"); if ! contents_ok "$va"; then unread="$unread $c"; elif has_items "$va"; then notaken="$notaken $c"; fi
		echo "$c vm after the take: $va" >> "$LOOTED"
	done <<<"$conts"
	sleep 5
	while read -r c _; do
		[ -n "$c" ] || continue
		case " $lost " in *" $c "*) echo "$c: no take reached the game (the rig's): not compared" >> "$LOOTED"; continue;; esac
		vb=$(sed -n "s/^$c host .* | vm //p" "$LOOTED" | head -1); hb=$(sed -n "s/^$c host \(.*\) | vm .*/\1/p" "$LOOTED" | head -1)
		ha=$(host "contents $c"); echo "$c host after: $ha" >> "$LOOTED"
		contents_ok "$ha" || { hunread="$hunread $c"; continue; }   # no reading of the host's copy: nothing about it is known
		while read -r i vn; do
			[ -n "$i" ] || continue
			if ! shared_item "$i"; then kept=$((kept + 1)); continue; fi
			# each game rolls a chest on its own: an item the host's copy never held proves nothing (2026-10-31: no chest's two
			# rolls had an item in common, and the old count passed on items that were never there)
			[ "$(count_in "$hb" "$i")" -gt 0 ] || { other=$((other + 1)); continue; }
			taken=$((taken + 1)); n=$(count_in "$ha" "$i"); [ "$n" -gt 0 ] && left="$left $c:${i}x$n"
		done <<<"$(stacks_of "$vb")"
	done < <(grep -E "^[0-9A-F]{8} host " "$LOOTED")
	[ -n "$nomenu" ] && warn "loot: chests a console activate opens no container menu for (left out)" "$nomenu (each tried twice, or known to open by a script of its own)"
	if [ $chests -eq 0 ]; then fail "loot: VM takes all from the host's chests" "no chest within 2500 u could be stocked in both games and opened (reasons in ${LOOTED##*/}): nothing was looted (inconclusive)"; return; fi
	if [ -n "$unread" ]; then fail "loot: VM takes all from $chests chest(s) in the container menu" "the VM's contents after the take could not be read for:$unread${notaken:+; still holding items:$notaken}"
	elif [ -z "$notaken" ]; then pass "loot: VM takes all from $chests chest(s) in the container menu" "each empty in the VM's game after R"
	else
		nl=""; for c in $notaken; do case " $lost " in *" $c "*) ;; *) nl="$nl $c";; esac; done   # chests whose R reached the game
		if [ -z "$nl" ]; then rig_fault input "loot: VM takes all from $chests chest(s) in the container menu" "R never reached the VM's game window, pressed twice, for:$lost (the presses:$takehow)"
		else fail "loot: VM takes all from $chests chest(s) in the container menu" "still holding items in the VM's game:$notaken (the menu take did not happen${lost:+; R never reached the game for:$lost}; the presses:$takehow)"; fi
	fi
	if [ -n "$hunread" ]; then fail "loot: the VM's takes reach the host's chests" "the host's contents after the takes could not be read for:$hunread (${LOOTED##*/}): nothing is known of those"
	elif [ $taken -eq 0 ] && [ $other -gt 0 ]; then warn "loot: the VM's takes reach the host's chests" "the host's rolls held none of the $other shared stack(s) the VM took (each game rolls a chest on its own; what only one roll put there stays by design): nothing to see, inconclusive"
	elif [ $taken -eq 0 ]; then fail "loot: the VM's takes reach the host's chests" "the VM took no item of the kind the session shares (only weapons, armor, keys, notes, books: $kept stack(s)): inconclusive"
	elif [ -z "$left" ]; then pass "loot: the VM's takes reach the host's chests" "$taken shared stack(s) both games held gone from the host's copies; $other only the VM's roll had; $kept weapon/armor/personal stack(s) stay by design"
	else fail "loot: the VM's takes reach the host's chests" "still in the host's copy:$left (of $taken shared stacks taken; details in ${LOOTED##*/})"; fi
}
# ---- terminals, loot, the safe -----------------------------------------------------------------------------------------
_useterm() { local who=$1 ref=$2; dc "$who" focus >/dev/null; sleep 0.5; dc "$who" "console $ref.activate player" >/dev/null; sleep 3; dc "$who" "key 0x0D 60" >/dev/null; sleep 2; dc "$who" "key 0x09 60" >/dev/null; sleep 1.5; clear_menus "$who" >/dev/null; sleep 1; }   # a blind second Tab opened the Pip-Boy when the first closed the terminal
# _safe_term <who> <terminal>: that player opens the storage terminal (waiting for its menu, not a fixed 3 s; the activate tried
# twice), picks the unlock entry and closes it. 1 = the terminal's menu never opened (SAFE_DIAG says what the game showed; SAFE_RIG
# names a rig cause seen at an activate: another menu open, or the player in power armor). With the guard before the step a
# terminal that never opens on the game's own activate points at the game or the mod (a terminal sync holding a stale lock),
# so it is a rig fault only with SAFE_RIG, and the mod's failure otherwise.
_safe_term() {
	local i
	SAFE_DIAG=""; SAFE_RIG=""
	for i in 1 2; do
		clear_menus "$1" >/dev/null; dc "$1" focus >/dev/null; sleep 1; dc "$1" "console $2.activate player" >/dev/null
		menu_open "$1" TerminalMenu 10 && break
		SAFE_DIAG="$SAFE_DIAG activate $i: no terminal menu in 10 s (menus $(dc "$1" menus | grep -oE 'open=[^ ]*' | cut -c1-80); $(dc "$1" pa | cut -c1-60));"
		local o; o=$(st_open "$1"); [ -n "$o" ] && [ "$o" != "?" ] && SAFE_RIG="${SAFE_RIG:+$SAFE_RIG, }$o open at activate $i"
		pa_in "$1" && SAFE_RIG="${SAFE_RIG:+$SAFE_RIG, }in power armor at activate $i"
		[ $i = 2 ] && return 1
	done
	sleep 2
	dc "$1" "key 0x0D 60" >/dev/null; sleep 2; dc "$1" "key 0x0D 60" >/dev/null; sleep 2; dc "$1" "key 0x09 60" >/dev/null; sleep 1.5; clear_menus "$1" >/dev/null; sleep 3
}
_safe_unlock() {  # _safe_unlock <actor> : that player unlocks the Super-Duper Mart safe through the storage terminal
	local actor=$1 other safe term=001CA8C8 b_a b_o a_a a_o
	other=$([ "$actor" = host ] && echo vm || echo host)
	safe=$(host "refs 31 2500 Safe" | grep -o "^[0-9A-F]\{8\}"); [ -z "$safe" ] && safe=$(host "refs 31 2500" | grep -o "[0-9A-F]\{8\} base=[0-9A-F]* 'Safe'" | head -1 | cut -c1-8)
	if [ -z "$safe" ]; then fail "safe ($actor unlocks)" "the safe was not found near the player"; return; fi
	host "console $safe.lock 25" >/dev/null; vm "console $safe.lock 25" >/dev/null; host "console $term.unlock" >/dev/null; vm "console $term.unlock" >/dev/null; sleep 1
	b_a=$(dcf "$actor" "refinfo $safe" locked); b_o=$(dcf "$other" "refinfo $safe" locked)
	[ -n "$b_a" ] && [ -n "$b_o" ] && [ "$b_a" != "no" ] && [ "$b_o" != "no" ] && pass "safe ($actor unlocks): starts locked in both games" "$actor $b_a $other $b_o" || fail "safe ($actor unlocks): starts locked in both games" "$actor locked=$b_a $other locked=$b_o"
	local opened=0
	_safe_term "$actor" $term && opened=1
	a_a=$(dcf "$actor" "refinfo $safe" locked)
	# Once more if the actor's own game still has it locked: 2026-11-06 the VM stalled 5 s through the key presses and the unlock
	# entry was never picked (both games agreed it was locked). A second miss still fails, unless the terminal never opened in
	# either: then the unlock was never asked for, a rig fault (healed and run again) only when SAFE_RIG names the rig's
	# state in the way, otherwise the mod's failure.
	if [ "$a_a" != no ]; then echo "safe ($actor unlocks): still locked after the terminal (locked=$a_a), once more"; _safe_term "$actor" $term && opened=1; a_a=$(dcf "$actor" "refinfo $safe" locked); fi
	if [ "$a_a" != no ] && [ $opened = 0 ]; then
		if [ -n "$SAFE_RIG" ]; then rig_fault input "safe ($actor unlocks): unlocked in the actor's game" "the storage terminal's menu never opened for $actor (two tries of two activates), the rig's state in the way ($SAFE_RIG):$SAFE_DIAG"
		else fail "safe ($actor unlocks): the storage terminal opens" "the game's own activate (two tries of two) never opened the terminal's menu for $actor, nothing else open and out of power armor:$SAFE_DIAG"; fi
		return
	fi
	a_o=$(dcf "$other" "refinfo $safe" locked)
	expect_eq "safe ($actor unlocks): unlocked in the actor's game" "$a_a" no
	expect_eq "safe ($actor unlocks): the other game sees it unlocked" "$a_o" no
}
step_terminals() {
	section "terminals, loot, the safe"
	local who l0 c before after
	enter_both RedRocketExt 16
	expect_match "terminals: both up at the start" "$(alive)" '^host=ingame=true vm=ingame=true$'
	for who in host vm; do
		_useterm $who 00048506
		expect_match "terminal used by $who: both games up" "$(alive)" '^host=ingame=true vm=ingame=true$'
		l=$(dcf $who 'net status' linked)
		expect_ge "terminal used by $who: linked refs watched" "$l" 1
	done
	_loot
	expect_match "loot: both games up" "$(alive)" '^host=ingame=true vm=ingame=true$'
	ns host; ns vm
	expect_le "loot: host pending loot settled" "$(nsf host pendingLoot)" 5; expect_le "loot: vm pending loot settled" "$(nsf vm pendingLoot)" 5
	enter_both SuperDuperMart01 16
	_safe_unlock host; _safe_unlock vm
	expect_match "terminals: both up at the end" "$(alive)" '^host=ingame=true vm=ingame=true$'
}

# ---- grenades ------------------------------------------------------------------------------------------------------------
# The step throws GRENADE_ITEM (the frag grenade); its projectile 000E574E goes off as GRENADE_EXPLOSION. The throw can set off other
# explosions at the same instant (a frag near a car or a mine adds its own, e.g. 00054A73 radius 32 beside the grenade's 000E574F radius
# 500), which the plugin correctly reports and replays too; so the counts are of the grenade's own explosion base only, read from the
# plugin log, not the all-explosions counters of `net status`. Override GRENADE_ITEM and GRENADE_EXPLOSION together for another throwable.
GRENADE_ITEM=${GRENADE_ITEM:-000EEBED}; GRENADE_EXPLOSION=${GRENADE_EXPLOSION:-000E574F}
_gh() { ns "$1"; echo "$(field "${NS[$1]}" ghosts)"; }   # ghost projectiles of that game
_ex_setoff() { ns "$1"; field "${NS[$1]}" explosions | cut -d/ -f2; }   # explosions from other players that game set off (all bases)
SETOFF_LOG_CAP=5   # WeaponFire::ApplyExplosion logs its first 5 set-offs per game run, then no more
# The count window is its own log mark, kept apart from the run's (MARK_*[host|vm]) so the final plugin-log check still covers the whole run.
_blast_mark() {  # _blast_mark <who>: remember where that game's log is now
	_log_fetch "$1" "$A_OUT/blastmark-$1"
	MARK_SIZE[b$1]=$(stat -c %s "$A_OUT/blastmark-$1/0.log" 2>/dev/null || echo 0); MARK_FIRST[b$1]=$(head -1 "$A_OUT/blastmark-$1/0.log" 2>/dev/null)
}
_blast_read() {  # _blast_read <who>: writes the lines since _blast_mark to $A_OUT/blastdelta-<who>.txt and prints its path
	_log_fetch "$1" "$A_OUT/blastnow-$1"; _log_delta "b$1" "$A_OUT/blastnow-$1" > "$A_OUT/blastdelta-$1.txt"; echo "$A_OUT/blastdelta-$1.txt"
}
# _blast_count <delta file> sent|setoff <explosion base>: how many explosions of that base the lines hold
#   sent    "WeaponFire: our explosion <ref> (<base>) at ... : reported"   (this game's own throw, reported to the friend)
#   setoff  "WeaponFire: explosion <base> radius N from player N: set off"  (a friend's throw, replayed here)
# Prints nothing when the file is empty (the log was not read), so the caller can tell "none" from "unknown".
_blast_count() {
	[ -s "$1" ] || return 0
	case "$2" in
		sent) grep -cE "WeaponFire: our explosion [0-9A-F]+ \($3\) .*: reported" "$1";;
		setoff) grep -cE "WeaponFire: explosion $3 radius [0-9]+ from player [0-9]+: set off" "$1";;
	esac
	return 0
}
_throw() {  # _throw <who>: that player throws the grenade in hand (a real key). 1 when the host's key was refused (host_input: the game
	# not in front, so nothing was sent and recorded as the rig's failure); the caller then skips that thrower's checks
	if [ "$1" = host ]; then host focus >/dev/null; sleep 1; host_input "grenade thrown by host: the throw key reaches its game" -Key 0x12 -HoldMs 400 || return 1
	else vm focus >/dev/null; sleep 1; vm "key 0x12 700" >/dev/null; fi
	sleep 6
}
step_grenades() {
	section "grenades both ways"
	local who other g_t0 g_o0 g_t1 g_o1 sent setoff try x_o0 x_o1 capped
	enter_both SanctuaryExt 14
	place host -80500 89300 7784 45; place vm -80400 89250 7784 300
	for who in host vm; do dc $who "console player.additem $GRENADE_ITEM 5" >/dev/null; dc $who "console player.unequipitem 0015B043" >/dev/null; dc $who "console player.equipitem $GRENADE_ITEM" >/dev/null; done; sleep 4
	for who in host vm; do
		other=$([ $who = host ] && echo vm || echo host); try=1
		while :; do
			g_t0=$(_gh $who); g_o0=$(_gh $other); _blast_mark $who; _blast_mark $other; x_o0=$(_ex_setoff $other)
			_throw $who || continue 2   # the host's key refused: no throw to check, on to the other thrower
			g_t1=$(_gh $who); g_o1=$(_gh $other); x_o1=$(_ex_setoff $other)
			sent=$(_blast_count "$(_blast_read $who)" sent "$GRENADE_EXPLOSION"); setoff=$(_blast_count "$(_blast_read $other)" setoff "$GRENADE_EXPLOSION")
			is_num "$sent" && [ "$sent" -ge 1 ] && break
			[ $try -ge 2 ] && break
			warn "grenade thrown by $who" "first throw was not reported by its game (own-base explosions reported: '${sent:-log not read}'): throwing again"; try=$((try + 1))
		done
		if is_num "$sent"; then expect_eq "grenade thrown by $who: reported by the thrower's game" "$sent" 1; else fail "grenade thrown by $who: reported by the thrower's game" "the thrower's plugin log was not read (no new lines since the throw)"; fi
		capped=$(grep -cE "WeaponFire: explosion [0-9A-F]+ radius [0-9]+ from player [0-9]+:" "$A_OUT/blastmark-$other/0.log" 2>/dev/null)
		if is_num "$capped" && [ "$capped" -ge "$SETOFF_LOG_CAP" ]; then
			# that game already logged its first SETOFF_LOG_CAP explosions (WeaponFire::ApplyExplosion logs no more): its counter
			# instead, every base (a frag sets off a small second explosion beside its own, so at least 1; 2026-11-01: hits' raiders
			# threw their grenades in the same host game and smoke read 0 from a log that had stopped)
			if is_num "$x_o0" && is_num "$x_o1"; then expect_ge "grenade thrown by $who: set off in the other game (its counter: its log's explosion lines are used up)" "$((x_o1 - x_o0))" 1
			else fail "grenade thrown by $who: set off in the other game (its counter: its log's explosion lines are used up)" "explosions counter unreadable (before '$x_o0', after '$x_o1')"; fi
		elif is_num "$setoff"; then expect_eq "grenade thrown by $who: replayed once in the other game" "$setoff" 1; else fail "grenade thrown by $who: replayed once in the other game" "the other game's plugin log was not read (no new lines since the throw)"; fi
		if is_num "$g_t0" && is_num "$g_o0" && is_num "$g_t1" && is_num "$g_o1"; then
			expect_eq "grenade thrown by $who: no new ghost projectiles (either game)" "$g_t1/$g_o1" "$g_t0/$g_o0"
		else fail "grenade thrown by $who: no new ghost projectiles (either game)" "ghosts counter unreadable (before $who '$g_t0' $other '$g_o0', after $who '$g_t1' $other '$g_o1')"; fi
	done
}

# ---- friends' shot trails: a stand-in's plain bullet and laser beam are launched inert in the watcher's game, both ways ---------------
# Each player fires a 10mm pistol (a bullet) and a laser gun (a beam), facing away from the other, 300 u apart. The WATCHER's game
# (the one with the shooter's stand-in) must launch one inert trail per shot (`trails=`, `firesInert=`), hold none (`firesHeld=`), see no
# hit by the stand-in on its player (`puppetHitsOnUs=`) or on any actor (`puppetHits=`), and have none left in flight afterwards
# (`inertLive=`). Then each fires the laser once more facing the other (the watcher's player in the line of fire, 300 u): still no hit.
# Then `trails off` on the watcher: the same shots are held as before (`firesHeld=`, `trailsHeld=` rise; `firesInert=`, `trails=` don't). What it measures is the
# counters, not pixels: every laser trail must come out a tracer (`trailTracers=`), which a beam needs for its 3D; the beam itself was seen
# drawn on the rig by eye (2026-11-07, docs/DESIGN.md, Weapon fire). The graph refuses an
# attack while the last one plays, so a few of an automatic gun's shots launch nothing (T_TRAIL_FRACTION: the share of the shooter's shots
# that must have launched a trail; the pistol and the laser lost 0 to 1 in 20 on the rig, 2026-11-06).
# Then each player swaps the 10mm in their hand for another copy with other mods (_gear_swap: the stand-in must hold the new copy),
# and the VM fires the laser from inside power armor, then is shot at in it (_trails_pa).
TRAIL_PISTOL=${TRAIL_PISTOL:-00004822}; TRAIL_PISTOL_AMMO=${TRAIL_PISTOL_AMMO:-0001F276}
TRAIL_LASER=${TRAIL_LASER:-0009983B};   TRAIL_LASER_AMMO=${TRAIL_LASER_AMMO:-000C1897}
T_TRAIL_FRACTION=${T_TRAIL_FRACTION:-80}
# _tr_snap <who>: that game's "firesInert firesHeld trails trailsHeld puppetHitsOnUs inertLive inertRefusedFull shots trailTracers puppetHits trailsHeldGun", one `net status`
_tr_snap() { local s; s=$(dc "$1" 'net status'); echo "$(field "$s" firesInert) $(field "$s" firesHeld) $(field "$s" trails) $(field "$s" trailsHeld) $(field "$s" puppetHitsOnUs) $(field "$s" inertLive) $(field "$s" inertRefusedFull) $(field "$s" shots) $(field "$s" trailTracers) $(field "$s" puppetHits) $(field "$s" trailsHeldGun)"; }
# _tr_strip <who> <gun>: every copy of that gun out of the player's inventory. One `removeitem <gun> 99` did not always
# take them all (2026-11-08: the host kept an unequipped modded, enchanted 10mm, which `equipitem` then took), so it is
# repeated until the player's count of it reads 0; a rig fault if it never does. The counts are printed to the log.
_tr_strip() {
	local i n first
	first=$(dc "$1" "count 00000014 $2")
	for i in 1 2 3 4 5; do
		n=$(dc "$1" "count 00000014 $2")
		[ "$n" = 0 ] && break
		dc "$1" "console player.removeitem $2 99" >/dev/null; sleep 1
	done
	n=$(dc "$1" "count 00000014 $2")
	echo "  $1: $2 copies before ${first:-?}, after ${n:-?}"
	[ "$n" = 0 ] || rig_fault state "trails: $1's copies of $2 removed" "still '$n' after 5 removeitem tries (before '$first')"
}
_tr_idle() { [ "$(field "$(dc "$1" 'net status')" inertLive)" = 0 ]; }
# _tr_hit_warn <label> <hits>: a hit check of a burst at a watcher in power armor (TR_HITS=warn): a stand-in's inert beam was seen to
# register 0-damage hits on a player in the suit (2026-11-07, the 0.14.0-rc1 soak), a known open bug: counted as a WARN until its fix
# makes it a check like the others.
_tr_hit_warn() { if [ "$2" = 0 ]; then pass "$1" 0; else warn "$1" "$2 hit(s): the known open bug of an inert beam's hits on a player in power armor"; fi; }
# _trail_burst <shooter> <gun> <label> <clicks> <on|off> [beam]: the shooter fires that gun; its friend's game (the watcher) has its trails
# on or off. "beam": the gun's projectile is a tracer on every shot (the laser), so every trail must come out a tracer (trailTracers): a
# beam without the projectile's tracer flag gets no 3D and draws nothing.
_trail_burst() {
	local shooter=$1 gun=$2 label=$3 n=$4 mode=$5 beam=${6:-} watcher b a s0 s1 v i shots need ok=1
	watcher=$(other_game "$shooter")
	dc "$watcher" "trails $mode" >/dev/null
	dc "$shooter" "console player.equipitem $gun" >/dev/null; sleep 3; dc "$shooter" "draw on" >/dev/null; sleep 1.5; dc "$shooter" focus >/dev/null; sleep 0.5
	# the host's game covered by another window or off every screen: none of the burst's clicks is sent (a click there lands on that
	# window), a rig-broken input failure naming it (assert.sh spot_or_broken); the watcher's trails put back on
	spot_or_broken "$shooter" "$label: set-up: the host's clicks reach its game" || { dc "$watcher" "trails on" >/dev/null; return; }
	b=($(_tr_snap "$watcher")); s0=$(field "$(dc "$shooter" 'net status')" shots)
	for i in $(seq 1 "$n"); do dc "$shooter" "click 120" >/dev/null; sleep 1.4; done
	sleep 4
	a=($(_tr_snap "$watcher")); s1=$(field "$(dc "$shooter" 'net status')" shots)
	dc "$watcher" "trails on" >/dev/null
	for v in "${b[@]}" "${a[@]}" "$s0" "$s1"; do is_int "$v" || ok=0; done
	[ ${#b[@]} = 11 ] && [ ${#a[@]} = 11 ] || ok=0
	[ $ok = 1 ] || { fail "$label: counters read" "before '${b[*]}' after '${a[*]}' shooter's shots '$s0' '$s1'"; return; }
	shots=$((s1 - s0))
	if [ "$shots" -lt 1 ]; then rig_broken input "$label: the shooter's gun fired" "no shot counted in $shooter's game after $n clicks (before $s0, after $s1)"; return; fi
	need=$(( (shots * T_TRAIL_FRACTION + 99) / 100 ))
	local hitcheck=expect_zero; [ "${TR_HITS:-}" = warn ] && hitcheck=_tr_hit_warn
	$hitcheck "$label: no hit on the watcher's player by the stand-in (puppetHitsOnUs)" "$((a[4] - b[4]))"
	$hitcheck "$label: no hit on any actor by the stand-in (puppetHits)" "$((a[9] - b[9]))"
	expect_zero "$label: no inert shot refused for want of room" "$((a[6] - b[6]))"
	# A shot held for its gun (trailsHeldGun: an enchantment of the gun lands on a hit) is by design (AttackKind::ShotReplay,
	# EnchantmentOnHit): `additem` rolls the gun's object template, and the laser rolls a fire-setting receiver at times.
	# That measures the roll, not the build: a rig fault, and the trail checks are not made. (The 10mm's rolls of this kind,
	# 0.15.0 to 0.16.0-rc1, "12 mods": an armor-piercing automatic receiver, a constant effect on the wielder, which
	# builds up to 0.16.0 held too; not a legendary, and not a copy removeitem left behind.)
	if [ "$mode" = on ] && [ "$((a[10] - b[10]))" -gt 0 ]; then
		rig_fault state "$label: the shooter's gun has no on-hit enchantment" "$((a[10] - b[10])) of $shots shot(s) held for the gun in the watcher's game (trailsHeldGun): the shooter's rolled copy carries one"
		return
	fi
	if [ "$mode" = on ]; then
		expect_ge "$label: trails launched inert in the watcher's game (of $shots shots)" "$((a[2] - b[2]))" "$need"
		expect_ge "$label: firesInert rose with them" "$((a[0] - b[0]))" "$need"
		expect_zero "$label: none held (firesHeld)" "$((a[1] - b[1]))"
		[ -z "$beam" ] || expect_ge "$label: every trail a tracer, so the beam is drawn (trailTracers)" "$((a[8] - b[8]))" "$((a[2] - b[2]))"
		if wait_for 15 _tr_idle "$watcher"; then pass "$label: no inert shot left in flight" "inertLive back to 0"; else fail "$label: no inert shot left in flight" "inertLive still $(field "$(dc "$watcher" 'net status')" inertLive) after 15 s"; fi
	else
		expect_zero "$label (trails off): nothing launched inert" "$((a[2] - b[2]))"
		expect_zero "$label (trails off): firesInert did not rise" "$((a[0] - b[0]))"
		expect_ge "$label (trails off): held as before (firesHeld)" "$((a[1] - b[1]))" "$need"
		expect_ge "$label (trails off): counted in trailsHeld" "$((a[3] - b[3]))" "$need"
	fi
}
# _gear_swap <shooter>: the shooter's stand-in in the watcher's game swaps the gun in its hand for another copy of the same gun with
# other mods (Equipment::PutOn's take-away: the copy in its hand taken out in one dressing, the new one given in the next). The
# shooter carries two differently modded copies of GEARSWAP_GUN (`additem` rolls the gun's template: stripped, then added one at a
# time until `itemmods` shows two stacks whose mods differ; a rig fault if 8 never do), holds the first drawn and then equips the
# second (dev `equipstack`: the console's equipitem takes any copy). The watcher's game must log the take-away; its stand-in must
# hold the gun built with the shooter's new mods within 8 s, still 4 s on, with no "isn't wearing their gear" refit in its log
# meanwhile. The take-away used the console's removeitem, which lands a moment later: landing after the next dressing had given
# the new copy, it took that too, and the stand-in was left without the gun until a refit.
GEARSWAP_GUN=${GEARSWAP_GUN:-$TRAIL_PISTOL}
# _gs_stack <itemmods answer> <n>: the n-th stack's mods (sorted, "none" without any); _gs_mods <equipped answer> <base>: the mods
# on the equipped copy of that item, sorted
_gs_stack() { local s; s=$(tr ';' '\n' <<<"$1" | grep '^ stack' | sed -n "$2p"); [ -n "$s" ] || return; s=$(grep -oE 'mods=[0-9A-F,]*' <<<"$s" | cut -d= -f2); [ -n "$s" ] && tr ',' '\n' <<<"$s" | grep . | sort | paste -sd, - || echo none; }
_gs_mods() { grep -oE "mods\[$2\]=[0-9A-F,]*" <<<"$1" | head -1 | cut -d= -f2 | tr ',' '\n' | grep . | sort | paste -sd, -; }
# _gs_holds <watcher> <stand-in> <base> <mods>: the stand-in has that item on, built with those mods
_gs_holds() { local e; e=$(dc "$1" "equipped $2"); grep -q " $3 '" <<<"$e" && [ "$(_gs_mods "$e" "$3")" = "$4" ]; }
_gear_swap() {
	local shooter=$1 watcher puppet i im a b m0 m1 r e f n label="gun swap by $1"
	watcher=$(other_game "$shooter")
	puppet=$(dc "$watcher" 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | head -1 | cut -d= -f2)
	[ -n "$puppet" ] || { fail "$label: set-up: the watcher's game has the stand-in" "no puppet= in its net status"; return; }
	_tr_strip "$shooter" "$GEARSWAP_GUN"
	for i in 1 2 3 4 5 6 7 8; do
		dc "$shooter" "console player.additem $GEARSWAP_GUN 1" >/dev/null; sleep 0.5
		im=$(dc "$shooter" "itemmods $GEARSWAP_GUN"); a=$(_gs_stack "$im" 1); b=$(_gs_stack "$im" 2)
		[ -n "$a" ] && [ -n "$b" ] && [ "$a" != "$b" ] && break
	done
	echo "  $shooter: $im"
	if [ -z "$a" ] || [ -z "$b" ] || [ "$a" = "$b" ]; then rig_fault state "$label: set-up: two copies of $GEARSWAP_GUN with other mods" "after $i additem(s): $(cut -c1-200 <<<"$im")"; return; fi
	dc "$shooter" "equipstack $GEARSWAP_GUN 1" >/dev/null; sleep 2; dc "$shooter" "draw on" >/dev/null; sleep 1
	m0=$(_gs_mods "$(dc "$shooter" equipped)" "$GEARSWAP_GUN")
	if ! wait_for 10 _gs_holds "$watcher" "$puppet" "$GEARSWAP_GUN" "$m0"; then
		fail "$label: set-up: the stand-in holds the first copy, with its mods" "shooter's mods '$m0'; the stand-in: $(dc "$watcher" "equipped $puppet" | cut -c1-240)"; return
	fi
	win_mark gs "$watcher"
	# the other copy: the first stack whose mods aren't those in hand (the engine may have put the equipped one first)
	im=$(dc "$shooter" "itemmods $GEARSWAP_GUN"); n=2
	for i in 1 2 3 4; do a=$(_gs_stack "$im" "$i"); [ -n "$a" ] && [ "$a" != "${m0:-none}" ] && { n=$i; break; }; done
	r=$(dc "$shooter" "equipstack $GEARSWAP_GUN $n"); m1=$(_gs_mods "$r" "$GEARSWAP_GUN")
	if [ "$(field "$r" accepted)" != true ] || [ "$m1" = "$m0" ]; then
		rig_fault state "$label: set-up: the shooter holds the other copy" "equipstack $n: $(cut -c1-200 <<<"$r") (mods before '$m0'; $(cut -c1-200 <<<"$im"))"; return
	fi
	dc "$shooter" "draw on" >/dev/null
	if wait_for 8 _gs_holds "$watcher" "$puppet" "$GEARSWAP_GUN" "$m1"; then pass "$label: the stand-in holds the new copy, with its mods" "$m1"
	else fail "$label: the stand-in holds the new copy, with its mods" "want '$m1'; the stand-in: $(dc "$watcher" "equipped $puppet" | cut -c1-240)"; fi
	sleep 4
	e=$(dc "$watcher" "equipped $puppet")
	if grep -q " $GEARSWAP_GUN '" <<<"$e" && [ "$(_gs_mods "$e" "$GEARSWAP_GUN")" = "$m1" ]; then pass "$label: the stand-in still holds it 4 s on" "$m1"
	else fail "$label: the stand-in still holds it 4 s on" "want '$m1'; the stand-in: $(cut -c1-240 <<<"$e")"; fi
	f=$(win_read gs "$watcher")
	if [ ! -s "$f" ]; then fail "$label: the watcher's log window read" "no new lines"
	else
		n=$(grep -c "carries other mods than theirs; taken away" "$f"); expect_ge "$label: the watcher's game took the gun in its hand away first (its log)" "$n" 1
		n=$(grep -c "isn't wearing their gear" "$f"); expect_zero "$label: no refit of the stand-in's gear (the watcher's log)" "$n"
	fi
	_tr_strip "$shooter" "$GEARSWAP_GUN"; dc "$shooter" "console player.additem $GEARSWAP_GUN 1" >/dev/null
}
# pa_get_in <who>: that player gets into a frame placed at their feet, as step_pa's driver does (console placeatme, a step back, a
# step in, the keyboard's E; up to 3 tries). PA_FRAME: the frame. 0 = in.
pa_get_in() {
	local w=$1 p x y tries
	p=$(dc "$w" pos); x=$(field "$p" x); y=$(field "$p" y)
	dc "$w" "console player.placeatme 0004A9C1" >/dev/null; sleep 2
	PA_FRAME=$(dc "$w" "refs 42 300" | tr ';' '\n' | grep "0004A9C1" | awk -v x="$x" -v y="$y" '{printf "%d %s\n", sqrt(($(NF-2)-x)^2+($(NF-1)-y)^2), $1}' | sort -n | head -1 | cut -d' ' -f2)
	dc "$w" focus >/dev/null; sleep 0.5; dc "$w" "key 0x53 300" >/dev/null; sleep 1.2; dc "$w" "key 0x57 120" >/dev/null; sleep 1
	for tries in 1 2 3; do
		[ -n "$PA_FRAME" ] && dc "$w" "turnto $PA_FRAME 300" >/dev/null; sleep 1; dc "$w" "mouse 0 -150" >/dev/null; sleep 0.8; dc "$w" "key 0x45 120" >/dev/null; sleep 10
		[ "$(dcf "$w" pa extraPA)" = "true" ] && return 0; dc "$w" "key 0x57 100" >/dev/null; sleep 0.8
	done
	return 1
}
# pa_leave <who> <label>: that player climbs out of the suit a step put them in (state.sh pa_climb_out: the E, then the pad's A); the
# climb-out itself is step_pa's check. Out by neither, the console takes them out, recorded HEAL (not a player's action). The frames
# placed go (_pa_frames_off).
pa_leave() {
	clear_menus "$1" >/dev/null
	if ! pa_climb_out "$1" 3; then
		if pa_force_out "$1"; then healed "$2: out of the suit" "taken out by the console (Actor.SwitchToPowerArmor none), not a player's action:$PA_DIAG"
		else rig_unhealed state "$2: out of the suit" "neither the E, the pad's A nor the console took the player out:$PA_DIAG"; fi
	fi
	_pa_frames_off "$1"
}
# _trails_pa: the VM gets into a frame (pa_get_in) and fires the laser, the host watching: weapons fired from inside power armor once
# drew no tracer (the stand-in swapped guns every 3 s, a stale equipped flag, so it fired the 10mm), so every trail must come out a
# tracer, with no refit of the stand-in's gear in the host's log. Then the host fires the laser at the VM's player in the suit: the
# trail checks stand, the hit checks are WARN (TR_HITS=warn, _tr_hit_warn).
_trails_pa() {
	local p hx y z f n
	if ! pa_get_in vm; then rig_fault input "trails in power armor: set-up: the VM gets into a frame" "extraPA is not true after 3 tries of the E (frame $PA_FRAME)"; _pa_frames_off vm; return; fi
	p=$(dc vm pos); y=$(field "$p" y); z=$(field "$p" z); hx=$(awk -v x="$(field "$p" x)" 'BEGIN{ if (x == "") exit; printf "%d", x - 300 }')
	if [ -z "$hx" ] || ! is_num "$y" || ! is_num "$z"; then fail "trails in power armor: set-up: the VM's position read" "'$p'"; pa_leave vm "trails in power armor"; return; fi
	place host "$hx" "$y" "$z" 270; dc vm "console player.setangle z 90" >/dev/null; sleep 3   # 300 u apart, facing away
	win_mark trpa host
	_trail_burst vm "$TRAIL_LASER" "laser shot by vm in power armor" 6 on beam
	f=$(win_read trpa host)
	if [ -s "$f" ]; then n=$(grep -c "isn't wearing their gear" "$f"); expect_zero "laser shot by vm in power armor: no refit of the stand-in's gear (the host's log)" "$n"
	else fail "laser shot by vm in power armor: the host's log window read" "no new lines"; fi
	place host "$hx" "$y" "$z" 90; dc vm "console player.setangle z 270" >/dev/null; sleep 1   # facing each other
	TR_HITS=warn _trail_burst host "$TRAIL_LASER" "laser shot by host at the watcher in power armor" 5 on beam
	pa_leave vm "trails in power armor"
}
step_trails() {
	section "friend shot trails (a stand-in's plain bullet and laser beam fly inert, both ways)"
	local who gun h0="$C_HOST0" v0="$C_VM0" t0="$C_T0"   # the run's crash baseline, put back at the end: the end check counts from the start
	enter_both SanctuaryExt 14
	crash_snapshot
	place host -80650 89300 7784 270; place vm -80350 89300 7784 90   # 300 u apart, each facing away from the other
	# Each player's own copies of the two guns go first, so `equipitem` takes the one given here: a copy picked up in play
	# may carry a legendary whose effect lands on a hit, held by design (AttackKind::ShotReplay), and the step would
	# measure the save, not the build. A fresh copy of each is then given (its object template rolled, as any `additem`).
	for who in host vm; do
		for gun in "$TRAIL_PISTOL" "$TRAIL_LASER"; do _tr_strip "$who" "$gun"; done
		for gun in "$TRAIL_PISTOL 1" "$TRAIL_PISTOL_AMMO 300" "$TRAIL_LASER 1" "$TRAIL_LASER_AMMO 300"; do dc "$who" "console player.additem $gun" >/dev/null; done
	done
	sleep 4
	for who in host vm; do
		_trail_burst "$who" "$TRAIL_PISTOL" "10mm shot by $who" 6 on
		_trail_burst "$who" "$TRAIL_LASER" "laser shot by $who" 6 on beam
	done
	# Facing each other: the watcher's player in the line of fire of the friend's inert laser.
	place host -80650 89300 7784 90; place vm -80350 89300 7784 270; sleep 1
	for who in host vm; do
		_trail_burst "$who" "$TRAIL_LASER" "laser shot by $who at the watcher" 5 on beam
	done
	place host -80650 89300 7784 270; place vm -80350 89300 7784 90; sleep 1
	_trail_burst host "$TRAIL_PISTOL" "10mm shot by host" 5 off
	for who in host vm; do dc "$who" "trails on" >/dev/null; done
	for who in host vm; do _gear_swap "$who"; done
	_trails_pa
	for who in host vm; do dc "$who" "trails on" >/dev/null; done
	crash_verdict "friend shot trails"
	C_HOST0="$h0"; C_VM0="$v0"; C_T0="$t0"   # (it used to stay this step's, so a crash before the last trails step of a soak went unseen at the end)
}

# ---- workshop build sync, then clean up what the test placed --------------------------------------------------------------------
step_workshop() {
	section "workshop build sync"
	local out ref before after gone
	enter_both SanctuaryExt 14
	place host -80500 89300 7784 45; place vm -80380 89400 7784 45; sleep 2
	before=$(vm 'net status' | grep -o 'workshop: [^|]*' | cut -c1-120)
	out=$(host "wsreport place ${WS_TEST_BASE:-000DF45C} ${WS_BENCH:-250FE}"); ref=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
	if [ -z "$ref" ]; then fail "workshop: host builds a piece" "$out"; return; fi
	echo "$ref" >> "$A_OUT/ws-placed.txt"; sleep 5
	after=$(vm 'net status' | grep -o 'workshop: [^|]*' | cut -c1-120)
	if [ "$before" != "$after" ]; then pass "workshop: the VM's game takes the piece" "$after"; else fail "workshop: the VM's game takes the piece" "workshop counters did not move: $after"; fi
	host "wsreport $ref ${WS_BENCH:-250FE} scrap" >/dev/null; sleep 5
	gone=$(vm 'net status' | grep -o 'workshop: [^|]*' | cut -c1-120)
	if [ "$gone" != "$after" ]; then pass "workshop: scrapping it reaches the VM" "$gone"; else fail "workshop: scrapping it reaches the VM" "workshop counters did not move after the scrap: $gone"; fi
	bash "$RIG/cleanup.sh" run "$A_OUT/ws-placed.txt" >> "$A_OUT/cleanup.log" 2>&1
}

# ---- power armor, both ways ---------------------------------------------------------------------------------------------------
# step_pa <driver>: the driver places a frame, walks up and presses E (a real key); the other game's stand-in must wear it,
# keep up while the driver runs, turns and jumps, and the driver must climb out again.
# pa_same_time <driver.csv> <viewer.csv> <stand-in's who> <driver clock ms> <viewer clock ms>: for each sample of the driver's
# stand-in in the viewer's trace, the distance to where the driver's own trace had them at that same moment (each file's clock,
# that machine's minus the host's, taken out; the driver's path interpolated between its samples). "n= mean= p95= max=" in u.
pa_same_time() {
	awk -F, -v who="$3" -v doff="$4" -v voff="$5" '
	FNR == 1 { f++; next }
	f == 1 && $2 == "me" { n++; t[n] = $1 - doff; x[n] = $3; y[n] = $4 }
	f == 2 && $2 == who { m++; vt[m] = $1 - voff; vx[m] = $3; vy[m] = $4 }
	END {
		j = 1; k = 0
		for (i = 1; i <= m; i++) {
			if (n < 2 || vt[i] < t[1] || vt[i] > t[n]) continue
			while (j < n - 1 && t[j + 1] < vt[i]) j++
			u = (t[j + 1] == t[j]) ? 0 : (vt[i] - t[j]) / (t[j + 1] - t[j])
			d[++k] = sqrt((x[j] + (x[j + 1] - x[j]) * u - vx[i]) ^ 2 + (y[j] + (y[j + 1] - y[j]) * u - vy[i]) ^ 2); s += d[k]
		}
		if (k == 0) { print "n=0"; exit }
		for (a = 2; a <= k; a++) { v = d[a]; b = a - 1; while (b > 0 && d[b] > v) { d[b + 1] = d[b]; b-- } d[b + 1] = v }
		p = int(k * 0.95); if (p < 1) p = 1
		printf "n=%d mean=%.0f p95=%.0f max=%.0f\n", k, s / k, d[p], d[k]
	}' "$1" "$2"
}
# clock_steps <minutes>: who set a clock lately, read from each machine's own record of it (the System log's Kernel-General event 1,
# "the system time has changed", read only): "host 16:24:29 +1525 ms (reason 1); vm ..." for every step of 50 ms or more in the
# last N minutes, or "none". Neither machine's time settings are touched. Found 2026-11-07: the host's own time service steps
# the host's clock by 1 to 6 s about once a day, and the VM's time sync follows it within seconds; a trace running then can't
# be lined up. That is the rig's (a rig fault), and this names it.
CLOCK_PS='Get-WinEvent -FilterHashtable @{LogName="System";ProviderName="Microsoft-Windows-Kernel-General";Id=1;StartTime=(Get-Date).AddMinutes(-MIN)} -ErrorAction SilentlyContinue | ForEach-Object { $x=[xml]$_.ToXml(); $d=@{}; $x.Event.EventData.Data | ForEach-Object { $d[$_.Name]=$_."#text" }; $ms=[int](([datetime]$d["NewTime"] - [datetime]$d["OldTime"]).TotalMilliseconds); if ([math]::Abs($ms) -ge 50) { "{0:HH:mm:ss} {1:+0;-0} ms (reason {2})" -f $_.TimeCreated, $ms, $d["Reason"] } }'
clock_steps() {
	local enc h v; enc=$(printf '%s' "${CLOCK_PS//MIN/$1}" | iconv -f utf-8 -t utf-16le | base64 -w0)
	h=$(powershell.exe -NoProfile -EncodedCommand "$enc" 2>/dev/null | tr -d '\r' | paste -sd, - | sed 's/,/, /g')
	v=$(vmssh "powershell -NoProfile -EncodedCommand $enc" | paste -sd, - | sed 's/,/, /g')
	[ -z "$h$v" ] && { echo none; return; }
	echo "${h:+host $h}${h:+${v:+; }}${v:+vm $v}"
}
# rsuf: a suffix naming this round (and the second attempt after a rig fault) for a step's evidence files, so a soak keeps every
# round's rather than only the last's: "-r2", "-r2-again", "" outside a soak
rsuf() { echo "${A_ROUND:+-r$A_ROUND}${A_AGAIN:+-again}"; }
step_pa() {
	local d=$1 v tries f worst settled i hp vx vy hx hy dd s skel pieces dx dy ang
	v=$([ "$d" = host ] && echo vm || echo host)
	section "power armor: $d drives"
	enter_both SanctuaryExt 16
	if [ "$d" = vm ]; then place host -80700 89200 7790 45; place vm -80240 89560 7790 225; dx=-80240; dy=89560
	else place vm -80050 89150 7790 45; place host -79950 89250 7790 225; dx=-79950; dy=89250; fi
	sleep 3
	dc $d "console player.placeatme 0004A9C1" >/dev/null; sleep 2
	f=$(dc $d "refs 42 300" | tr ';' '\n' | grep "0004A9C1" | awk -v x=$dx -v y=$dy '{printf "%d %s\n", sqrt(($(NF-2)-x)^2+($(NF-1)-y)^2), $1}' | sort -n | head -1 | cut -d' ' -f2)
	dc $d focus >/dev/null; sleep 0.5; dc $d "key 0x53 300" >/dev/null; sleep 1.2; dc $d "key 0x57 120" >/dev/null; sleep 1
	for tries in 1 2 3; do
		[ -n "$f" ] && dc $d "turnto $f 300" >/dev/null; sleep 1; dc $d "mouse 0 -150" >/dev/null; sleep 0.8; dc $d "key 0x45 120" >/dev/null; sleep 10
		[ "$(dcf $d pa extraPA)" = "true" ] && break; dc $d "key 0x57 100" >/dev/null; sleep 0.8
	done
	if [ "$(dcf $d pa extraPA)" != "true" ]; then fail "power armor ($d): got into the frame" "extraPA is not true after 3 tries (frame $f)"; _pa_frames_off $d; return; fi
	pass "power armor ($d): got into the frame" "after $tries tries (frame $f)"
	ns $v
	hp=$(grep -o "puppet=FF[0-9A-F]*" <<<"${NS[$v]}" | head -1 | cut -d= -f2)
	s=$(grep -oE 'in=[a-z]* skel=[a-z]* tries=[0-9]* pieces=[0-9]*/[0-9]*' <<<"${NS[$v]}" | head -1)
	expect_eq "power armor ($d): the other game's stand-in is in the suit" "$(field "$s" in)/$(field "$s" skel)" "true/true"
	pieces=$(field "$s" pieces)
	if [ -n "$pieces" ] && [ "$(part "$pieces" 1)" = "$(part "$pieces" 2)" ] && [ "$(part "$pieces" 2)" != 0 ]; then pass "power armor ($d): stand-in wears all pieces" "$pieces"; else fail "power armor ($d): stand-in wears all pieces" "pieces='$pieces'"; fi
	if [ -z "$hp" ]; then fail "power armor ($d): stand-in found" "no puppet=FF.. in the other game's net status"; return; fi
	_xy() { grep -o " [xy]=[-0-9]*" <<<"$1" | cut -d= -f2 | tr '\n' ' '; }
	_dist() { local hq; hq=$(dc $v "refinfo $hp"); read -r vx vy <<<"$(_xy "$(dc $d 'refinfo 00000014')")"; read -r hx hy <<<"$(_xy "$hq")"; awk -v a=$vx -v b=$vy -v c=$hx -v e=$hy 'BEGIN{printf "%d", sqrt((a-c)^2+(b-e)^2)}'; }
	# While moving, the driver and its stand-in are compared at the same moment: both games trace (dev `trace`, 20 Hz, each row
	# stamped with its machine's clock) and the driver's own path is read at each stand-in sample's time, the VM's clock offset
	# taken out. Reading the two games one after the other used to count up to ~0.3 s of running as distance.
	local clk clk0 clk1 did tr hs vs lu pa_moving="power armor ($d): stand-in distance while moving, at the same moment"
	# The offset is read before and after the trace and lined up with what each game says its clock stepped during it
	# (clocklib.sh clock_lineup): a time service setting a clock mid-trace failed this check in the 0.12.0, 0.13.1 and 0.13.2
	# soaks. trace.sh clock settles the VM's clock first (a resync only when it is over 100 ms off, then waiting until two
	# readings agree: a forced resync before every trace was what let the VM's second time source set it 1.9 s ahead). A reading
	# of -103 between two of -12 (2026-11-01) was this side stalling through a whole read, not the VM's clock: trace.sh offset
	# takes a reading only once enough of its lines came through with no stall, and says unknown otherwise. A reading can come
	# back empty (an ssh hiccup, 2026-11-05: before '', after -13; under a loaded host, 2026-11-07): trace.sh offset tries up to
	# CLOCK_TRIES (4) times, 3 s apart, a measurement only. Offsets that can't line the traces up are the rig's:
	# a rig fault, not a failure.
	_offset() { local o; o=$(bash "$RIG/trace.sh" offset 2>/dev/null); is_num "$o" && echo "$o"; }
	bash "$RIG/trace.sh" clock >/dev/null 2>&1; clk0=$(_offset)
	did=$(dcf $d 'net status' id)
	bash "$RIG/dcfast.sh" both "trace start 20" >/dev/null 2>&1; dc $d focus >/dev/null; sleep 0.3
	dc $d "key 0x57 3000" >/dev/null; dc $d "mouse 600 0" >/dev/null
	for i in $(seq 1 12); do
		case $i in 3) dc $d "key 0x57 2500" >/dev/null;; 5) dc $d "key 0x20 80" >/dev/null;; 6) dc $d "mouse 1587 0" >/dev/null; dc $d "key 0x57 3000" >/dev/null;; esac; sleep 0.6
	done
	sleep 1.5; hs=$(host 'trace stop'); vs=$(vm 'trace stop'); settled=$(_dist); clk1=$(_offset)
	lu=$(clock_lineup "$clk0" "$clk1" "$(wall_step "$hs")" "$(wall_step "$vs")"); clk=""; case "$lu" in ok*) clk=$(cut -d' ' -f2 <<<"$lu");; esac
	echo "power armor ($d): VM clock before '$clk0' after '$clk1', stepped during the trace host '$(wall_step "$hs")' vm '$(wall_step "$vs")': $lu"
	cp -f "$HOST_F4SE/F4Multiplayer_trace.csv" "$A_OUT/pa-$d-host$(rsuf).csv" 2>/dev/null; scp -q -i "$VM_KEY" -o ConnectTimeout=20 "$VM_SSH:$VM_F4SE/F4Multiplayer_trace.csv" "$A_OUT/pa-$d-vm$(rsuf).csv" 2>/dev/null
	if [ -z "$clk" ]; then rig_fault clock "$pa_moving" "the two traces can't be lined up: ${lu#rig }; clocks set in the last 3 min: $(clock_steps 3)"
	elif [ -z "$did" ]; then fail "$pa_moving" "no player id in the driver's net status"
	else
		tr=$(pa_same_time "$A_OUT/pa-$d-$d$(rsuf).csv" "$A_OUT/pa-$d-$v$(rsuf).csv" "p$did" "$([ $d = vm ] && echo "$clk" || echo 0)" "$([ $v = vm ] && echo "$clk" || echo 0)")
		echo "$tr" > "$A_OUT/pa-$d-distance$(rsuf).txt"
		if [ "$(field "$tr" n)" -ge 40 ] 2>/dev/null; then
			# the name stays the same from round to round (a soak's summary tells a rig fault made good in another round by it)
			if is_num "$(field "$tr" max)" && _cmp "$(field "$tr" max)" '<=' "$T_PA_MOVING"; then pass "$pa_moving" "$(field "$tr" max) u (at most $T_PA_MOVING; max of $(field "$tr" n) samples, mean $(field "$tr" mean) u, p95 $(field "$tr" p95) u; VM clock $clk ms)"
			else fail "$pa_moving" "$(field "$tr" max) u, want at most $T_PA_MOVING (max of $(field "$tr" n) samples, mean $(field "$tr" mean) u, p95 $(field "$tr" p95) u; VM clock $clk ms)"; fi
		else fail "$pa_moving" "too few stand-in samples over the driver's path ('$tr'): the trace did not cover the drive"; fi
	fi
	expect_le "power armor ($d): stand-in distance after stopping" "$settled" "$T_PA_SETTLED" u
	# The climb-out: the keyboard's E, and the pad's A only when no E was seen to reach the game (state.sh pa_climb_out).
	# 2026-11-06 (host) and 2026-11-07 (the VM, 0.14.0-rc1) the E alone, up to five times, left the player in the suit, and the VM
	# spent a whole round in it, failing four later steps. An E that reached the game's window (nothing open, in front, its key
	# count up) and left the player in is the climb-out's failure: a regression that eats a keyboard player's way out is the
	# mod's (or the game's), and no other press may pass it. Out by the pad after Es that never reached the game passes with a
	# WARN (the rig's input). Out by none fails, with what the game said; the player is then taken out by the console, said so,
	# so the stand-in's check and the steps after are still made.
	clear_menus $d >/dev/null
	if pa_climb_out $d 3; then
		pass "power armor ($d): climbed out" "$PA_HOW"
		case "$PA_HOW" in "E held"*) ;; *) warn "power armor ($d): the keyboard's E climbs out" "the E never reached the game's window (the rig's input); out by $PA_HOW;$PA_DIAG";; esac
	else
		if [ -n "$PA_REACHED" ]; then fail "power armor ($d): climbed out" "the keyboard's E reached the game's window and left the player in (3 tries):$PA_DIAG"
		else fail "power armor ($d): climbed out" "still in after the keyboard's E (3 tries, none seen to reach the game) and the pad's A:$PA_DIAG"; fi
		if pa_force_out $d; then healed "power armor ($d): out of the suit for the next checks" "taken out by the console (Actor.SwitchToPowerArmor none): not a player's action"
		else rig_unhealed state "power armor ($d): out of the suit for the next checks" "the console did not take the player out either"; fi
	fi
	ns $v; expect_eq "power armor ($d): the other game's stand-in is out of the suit" "$(grep -o 'pa=[a-z/]*' <<<"${NS[$v]}" | head -1)" "pa=false/false"
	_pa_frames_off $d
}

# _pa_frames_off <driver>: the frames step_pa placed (base 0004A9C1, and any copy the other game made of one) are taken out of both games
# near Sanctuary's bridge: left standing, a soak's next round walked into one on the doors and drops paths (2026-11-06).
# The driver's game first (its copies then go from the other game by themselves). The engine disables a worn frame, and a
# disabled one is skipped, so a frame someone is still in (a failed climb-out) is never taken from under them. Game-made refs only.
_pa_frames_off() {
	local g r
	for g in $1 $([ "$1" = host ] && echo vm || echo host); do
		for r in $(dc $g "refs 42 1500" | tr ';' '\n' | grep "0004A9C1" | awk '{print $1}' | grep "^FF"); do
			[ "$(field "$(dc $g "refinfo $r")" disabled)" = false ] || continue
			dc $g "console $r.disable" >/dev/null; dc $g "console $r.markfordelete" >/dev/null
		done
	done
}

# ---- what players do together: the workbench, containers, doors, giving items, down and helped up ------------------------------------
# These steps move the players with the dev channel's virtual pad (`walkto`: the game's own movement from a stick, re-aimed each
# frame; never a position set) and press its buttons (A activates, B opens and closes the Pip-Boy, as a gamepad's do), so the
# game's window need not be in front. Each step starts both players at Sanctuary's marker (the loot step at Concord's; console coc:
# placement to reach the scene, as the other steps do) and unplugs the pads at its end, so the keyboard and mouse the other steps
# use are the game's again.
# Console set-up (items added, a settlement made the host's, damage) is named in each step's comment.
SANCT_BENCH=000250FE; LINK_BENCH=0006F5C5; LINK_LOCATION=0006B4D0; CODSWORTH=0001CA7D
MARKER_SPOT="-80300 89550"   # just past Sanctuary's coc marker, on the road: the walks below start or pass here
PARK_SPOT="-79600 88900"     # on the road about 1000 u from the marker: out of a helper's reach, out of the way of a crosshair
DROP_ASIDE=${DROP_ASIDE:-"-79400 88700"}   # 280 u on from PARK_SPOT along the road: where a giver stands while the taker comes for the drop
DROP_CLEAR=${DROP_CLEAR:-100}   # a giver closer than this to the copy may stand in the way of the taker's press (a rig fault, not the mod's)

# wait_for <seconds> <command...>: runs the command every 0.5 s until it succeeds or the time is up
wait_for() { local end=$((SECONDS + $1)); shift; while :; do "$@" && return 0; [ $SECONDS -ge "$end" ] && return 1; sleep 0.5; done; }
pads_off() { dc host "pad unplug" >/dev/null; dc vm "pad unplug" >/dev/null; }
# is_int <value>: a whole number. A dev-channel reply that isn't one (an "error: timed out waiting for game thread") inside $(( )) is a
# syntax error, which ends the whole top-level if/while holding the step without a word: so every value read from a game is checked
# before arithmetic. Like is_num but without decimals, which $(( )) can't take either.
is_int() { [[ $1 =~ ^-?[0-9]+$ ]]; }
# walk <who> <check name> <speed> <radius> <target: ref | x y>: that game's player walks there (tools/rig/walkto.sh), up to 3 tries.
# A walk that sticks (no headway for 8 s) is freed the way a player frees one before the next try: the other player, standing
# within WALK_CROWD u, steps away from them (2026-11-07, the 0.14.0-rc1 soak: the taker of a drop walked up to the copy at the
# giver's feet, into the giver's stand-in, and pushed the stick for 20 s without moving an inch until the giver walked off);
# otherwise the walker backs off and steps to the side. The other player is never asked to step aside when the walk is to them
# (the target is their stand-in: the helper's walk to a downed friend sticks on the friend) or when they are down (a downed
# player standing up to walk would change what the step tests): then the walker backs off. One who stepped aside walks back to
# where they stood once the walk is over, as the step left them. A walk freed that way is recorded HEAL. One that never arrives is a rig
# fault under the name (RIG: the set-up of the step's checks failed, not the mod; smoke.sh heals and runs the step again), with
# what held the player: the other player's distance, and the game's input layers (a layer of the mod's holding movement off
# would be the mod's, and is there to be read). WALK_LAST: how the last walk ended. 0 = arrived.
WALK_CROWD=${WALK_CROWD:-220}
_xy_of() { local p; p=$(dc "$1" pos); echo "$(field "$p" x) $(field "$p" y)"; }
_dist_xy() { awk -v a="$1" -v b="$2" 'BEGIN{split(a,p," "); split(b,q," "); if (p[1]=="" || q[1]=="") {print ""; exit} printf "%d", sqrt((q[1]-p[1])^2+(q[2]-p[2])^2)}'; }
walk() {
	local who=$1 label=$2 sp=$3 r=$4 try other me them d to heal="" why orig="" nomove=""; shift 4
	other=$(other_game "$who")
	# the target is the other player (their stand-in in the walker's game), or they are down: they stay where they are
	if [ $# = 1 ] && dc "$who" 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | grep -qx "puppet=$1"; then nomove="the walk is to them"
	elif declare -F st_down >/dev/null && st_down "$other"; then nomove="they are down"; fi   # st_down: state.sh (smoke.sh sources it)
	_walk_back() {  # the other player who stepped aside walks back to where they stood
		[ -n "$orig" ] || return 0
		if bash "$RIG/walkto.sh" "$other" $orig 1 100 30 2>&1 | tail -1 | grep -q ': arrived '; then heal="$heal $other walked back;"; else heal="$heal $other's walk back did not arrive;"; fi
		dc "$other" "pad unplug" >/dev/null
	}
	for try in 1 2 3; do
		WALK_LAST=$(bash "$RIG/walkto.sh" "$who" "$@" "$sp" "$r" 60 2>&1 | tail -1)
		case "$WALK_LAST" in *": arrived "*) _walk_back; [ -n "$heal" ] && healed "$label" "$who's walk stuck, freed:$heal; arrived on try $try"; return 0;; esac
		case "$WALK_LAST" in *" stuck "*) ;; *) sleep 1; continue;; esac
		me=$(_xy_of "$who"); them=$(_xy_of "$other"); d=$(_dist_xy "$me" "$them")
		if is_int "$d" && [ "$d" -lt "$WALK_CROWD" ] && [ -z "$nomove" ]; then
			# the other player steps 300 u straight away from the walker (a real walk), and walks back once the walk is over
			[ -n "$orig" ] || orig=$them
			to=$(awk -v a="$me" -v b="$them" 'BEGIN{split(a,p," "); split(b,q," "); dx=q[1]-p[1]; dy=q[2]-p[2]; l=sqrt(dx*dx+dy*dy); if (l<1) {dx=1; dy=0; l=1} printf "%d %d", q[1]+dx/l*300, q[2]+dy/l*300}')
			bash "$RIG/walkto.sh" "$other" $to 1 100 30 >/dev/null 2>&1; dc "$other" "pad unplug" >/dev/null
			heal="$heal the other player ($other) stood $d u away and stepped aside;"
		else
			dc "$who" "pad stick l 0 -1 900" >/dev/null; sleep 1.2; dc "$who" "pad stick l 1 0 700" >/dev/null; sleep 1
			heal="$heal backed off and stepped to the side (the other player ${d:-?} u away${nomove:+, not asked to move: $nomove});"
		fi
	done
	_walk_back
	why="at ($(_xy_of "$who")), the other player $(_dist_xy "$(_xy_of "$who")" "$(_xy_of "$other")") u away; input layers: $(dc "$who" controls | grep -oE 'layers=[^|]*' | cut -c1-160)"
	rig_fault walk "$label" "$who's walk to $* did not arrive in 3 tries: $WALK_LAST${heal:+ (tried:$heal)}; $why"; dc "$who" "pad unplug" >/dev/null; return 1
}
# walk_away <who> <check name> <x0> <y0> <dist> [seconds]: that game's player walks (real movement: tools/rig/walkto.sh at a run, in
# legs of WALK_LEG u) until it stands more than <dist> u from the point, where the rig knows no path. A leg heads straight away from
# the point (WALK_AWAY_FIRST, a direction "dx dy", while the player still stands on it); one that sticks or times out turns the
# next 45 degrees to one side, then to the other, then 90 and 135 (a wall is walked along, not through); a leg that arrives keeps
# its turn while that still leads away (45), and the one after a wider turn heads straight away again. WALK_OUT: how far from the
# point the player stands at the end; WALK_LEGS: each leg's turn and how it ended. 0 = it is that far. A walk that never gets
# there is a rig fault under the name (as walk's: the set-up of the step's checks failed, not the mod).
WALK_LEG=${WALK_LEG:-2500}; WALK_AWAY_FIRST=${WALK_AWAY_FIRST:-0 -1}
walk_away() {
	local who=$1 label=$2 x0=$3 y0=$4 dist=$5 end=$((SECONDS + ${6:-300})) turns=(0 45 -45 90 -90 135 -135) t=0 me d to last blind=0
	WALK_OUT=""; WALK_LEGS=""
	while [ $SECONDS -lt "$end" ]; do
		me=$(_xy_of "$who"); d=$(_dist_xy "$x0 $y0" "$me")
		if ! is_int "$d"; then blind=$((blind + 1)); [ "$blind" -ge 5 ] && break; sleep 2; continue; fi   # the game gave no position
		WALK_OUT=$d
		if [ "$d" -gt "$dist" ]; then dc "$who" "pad unplug" >/dev/null; return 0; fi
		to=$(awk -v o="$x0 $y0" -v m="$me" -v f="$WALK_AWAY_FIRST" -v a="${turns[$t]}" -v leg="$WALK_LEG" 'BEGIN{
			split(o,p," "); split(m,q," "); split(f,g," "); dx=q[1]-p[1]; dy=q[2]-p[2]; l=sqrt(dx*dx+dy*dy)
			if (l < 200) { dx=g[1]; dy=g[2]; l=sqrt(dx*dx+dy*dy); if (l == 0) { dx=0; dy=-1; l=1 } }
			r=a*3.14159265/180; ux=dx/l; uy=dy/l
			printf "%d %d", q[1]+(ux*cos(r)-uy*sin(r))*leg, q[2]+(ux*sin(r)+uy*cos(r))*leg }')
		last=$(bash "$RIG/walkto.sh" "$who" $to 1 200 40 2>&1 | tail -1)
		WALK_LEGS="$WALK_LEGS ${turns[$t]}:$(cut -d' ' -f2 <<<"$last" | tr -d ':')"
		case "$last" in
		*": arrived "*) case "${turns[$t]}" in 0|45|-45) ;; *) t=0;; esac;;
		*) t=$(( (t + 1) % ${#turns[@]} ));;
		esac
	done
	dc "$who" "pad unplug" >/dev/null
	rig_fault walk "$label" "$who's walk away from ($x0 $y0) ended ${WALK_OUT:-?} u from it, want more than $dist (legs, turn:end:$WALK_LEGS); at ($(_xy_of "$who")); input layers: $(dc "$who" controls | grep -oE 'layers=[^|]*' | cut -c1-160)"
	return 1
}
# pitch_to <who> <ref>: degrees down from that player's eyes (120 u over its feet) to the reference's origin
pitch_to() {
	local p r; p=$(dc "$1" pos); r=$(dc "$1" "refinfo $2")
	awk -v px="$(field "$p" x)" -v py="$(field "$p" y)" -v pz="$(field "$p" z)" -v rx="$(field "$r" x)" -v ry="$(field "$r" y)" -v rz="$(field "$r" z)" \
		'BEGIN{ if (px == "" || rx == "") exit; printf "%d", atan2(120 + pz - rz, sqrt((rx - px) ^ 2 + (ry - py) ^ 2)) * 57.29578 }'
}
# press_a <who> <ref> <check> [pitch...]: that player faces the reference (turnto; "auto" is turnto's own pitch, at a standing body's
# chest) at each pitch in turn and presses the pad's A, until the check (a command line, run after each press) succeeds. PRESSES
# says how many it took. 0 = the check passed. The pad is plugged in with a neutral stick first: a press that is the pad's first
# input is lost while the game takes it on as the controller (2026-11-07, state.sh pa_climb_out). PRESS_REACHED is set when a
# press was seen read by the game (the pad's status after it: gameSeesPad=true and its answered count up), PRESS_PAD has the
# last status read.
press_a() {
	local who=$1 ref=$2 check=$3 p n=0 s a0 a1; shift 3
	PRESS_REACHED=""; PRESS_PAD=""
	dc "$who" "pad stick l 0 0 300" >/dev/null; sleep 1.5
	for p in "${@:-auto}"; do
		n=$((n + 1)); PRESSES=$n
		if [ "$p" = auto ]; then dc "$who" "turnto $ref 300" >/dev/null; else dc "$who" "turnto $ref 300 $p" >/dev/null; fi
		sleep 0.8; a0=$(field "$(dc "$who" pad)" answered); dc "$who" "pad button a" >/dev/null
		eval "$check" && return 0
		s=$(dc "$who" pad); a1=$(field "$s" answered); PRESS_PAD=$(cut -c1-200 <<<"$s")
		[ "$(field "$s" gameSeesPad)" = true ] && is_int "$a0" && is_int "$a1" && [ "$a1" -gt "$a0" ] && PRESS_REACHED=1
	done
	return 1
}
# A window of a game's plugin log of its own (the run's marks stay as they are, so the end checks still cover the whole run)
win_mark() { dc "$2" "log flush" >/dev/null; _log_fetch "$2" "$A_OUT/winmark-$1-$2"; MARK_SIZE[w$1$2]=$(stat -c %s "$A_OUT/winmark-$1-$2/0.log" 2>/dev/null || echo 0); MARK_FIRST[w$1$2]=$(head -1 "$A_OUT/winmark-$1-$2/0.log" 2>/dev/null); }
win_read() { dc "$2" "log flush" >/dev/null; _log_fetch "$2" "$A_OUT/winnow-$1-$2"; _log_delta "w$1$2" "$A_OUT/winnow-$1-$2" > "$A_OUT/windelta-$1-$2.txt"; echo "$A_OUT/windelta-$1-$2.txt"; }
other_game() { [ "$1" = host ] && echo vm || echo host; }

# ---- the host's workbench shared, and a take across its supply line -------------------------------------------------------------------
# With the host's bSharedStorage on, a friend in workshop mode at a settlement the host owns builds from the host's workbench and from
# the workbenches the host's supply lines link to it (src/game/Storage.cpp, SupplyLines.h); the server books each take, so nothing is
# used twice. Set-up by the host's console (a coc character owns no settlement): Sanctuary and Abernathy Farm made the host's
# (WorkshopScript.SetOwnedByPlayer, as the game does it) and Codsworth made Sanctuary's provisioner to Abernathy Farm
# (WorkshopParentScript's AddActorToWorkshopPUBLIC and AssignCaravanActorPUBLIC, what the settlement menu's supply line calls), then
# BENCH_N1 steel added to Sanctuary's workbench and BENCH_N2 to Abernathy Farm's. The VM walks to Sanctuary's workbench, enters workshop
# mode (dev `workshop enter`: a real E opened none on the rig, PR 302) and builds with 3 more steel than Sanctuary's holds (dev
# `storage build`: the engine's own take for a build, no piece placed), so 3 come from Abernathy Farm's, which neither game has loaded.
# Then the host walks there, enters workshop mode and builds 2, all from Abernathy Farm's. Asserted: the VM's view has the host's counts
# at both workbenches; each build comes off the host's true piles (the total falls by the build, nothing taken twice, no take
# refused, the builder's pockets untouched) and the other game's list follows; leaving, the VM's own stash at both is back.
# Abernathy Farm, not Red Rocket: a container the host's world once called storage is the host's for good (common/StorageBook.h:
# one true copy, kept in the session file), and the terminals step loots Red Rocket's chests as a roll of loot.
BENCH_ITEM=${BENCH_ITEM:-000731A4}; BENCH_N1=${BENCH_N1:-10}; BENCH_N2=${BENCH_N2:-5}   # steel
_bench_view() { dc "$1" "storage bench $BENCH_ITEM"; }
_in_workshop() { [ "$(field "$(dc "$1" workshop)" menu)" = "$2" ]; }
_no_msgbox() { _menu_open "$1" MessageBoxMenu || return 0; dc "$1" "menu MessageBoxMenu hide" >/dev/null; return 1; }
_ws_enter() { dc "$1" "workshop enter" >/dev/null; wait_for 25 _in_workshop "$1" true || return 1; sleep 2; wait_for 8 _no_msgbox "$1"; return 0; }
_ws_exit() { wait_for 8 _no_msgbox "$1"; dc "$1" "workshop exit" >/dev/null; wait_for 15 _in_workshop "$1" false; }
_host_total() {  # the host's two piles added; "unreadable(a|b)" (not a number) when a count didn't come back as one
	local a b; a=$(dc host "count $SANCT_BENCH $BENCH_ITEM"); b=$(dc host "count $LINK_BENCH $BENCH_ITEM")
	if is_int "$a" && is_int "$b"; then echo $((a + b)); else echo "unreadable($a|$b)"; fi
}
_vm_sees() {  # the VM's view shows the linked workbench, with the host's counts at both
	local b; b=$(_bench_view vm)
	[ "$(field "$b" supply)" = "$LINK_BENCH:shown" ] && [ "$(field "$b" copy)" = "$(dc host "count $SANCT_BENCH $BENCH_ITEM")" ] && [ "$(field "$b" at$LINK_BENCH)" = "$(dc host "count $LINK_BENCH $BENCH_ITEM")" ]
}
_list_is() { [ "$(field "$(_bench_view "$1")" workshopList)" = "$2" ]; }
step_bench() {
	section "shared workbench and a supply-line take"
	local r s0 n built hs hr vs0 vr0 vp0 vp1 t0 t1 rf0 or0 rf1 or1 b ok
	r=$(dc host rules)
	if [ "$(field "$r" sharedStorage)" != true ]; then fail "bench: set-up: the host's bSharedStorage is on" "rules say sharedStorage='$(field "$r" sharedStorage)': the gate needs bSharedStorage = true in the host's F4Multiplayer.ini"; return; fi
	enter_both SanctuaryExt 14
	host "console $SANCT_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; host "console $LINK_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 2
	host "console cqf WorkshopParent \"WorkshopParentScript.AddActorToWorkshopPUBLIC\" $CODSWORTH $SANCT_BENCH" >/dev/null; sleep 2
	host "console cqf WorkshopParent \"WorkshopParentScript.AssignCaravanActorPUBLIC\" $CODSWORTH $LINK_LOCATION" >/dev/null; sleep 2
	ok=$(host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer workshopID")
	if [ "$(field "$ok" OwnedByPlayer)" != true ] || [ "$(field "$(host "scriptvars $LINK_BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ] \
		|| [ "$(field "$(host "scriptvars $CODSWORTH WorkshopNPCScript workshopID")" workshopID)" != "$(field "$ok" workshopID)" ]; then
		fail "bench: set-up: Sanctuary and Abernathy Farm the host's, Codsworth Sanctuary's provisioner" "Sanctuary: $ok; Abernathy Farm: $(host "scriptvars $LINK_BENCH WorkshopScript OwnedByPlayer"); Codsworth: $(host "scriptvars $CODSWORTH WorkshopNPCScript workshopID")"; return
	fi
	host "console $SANCT_BENCH.additem $BENCH_ITEM $BENCH_N1" >/dev/null; host "console $LINK_BENCH.additem $BENCH_ITEM $BENCH_N2" >/dev/null
	vs0=$(vm "count $SANCT_BENCH $BENCH_ITEM"); vr0=$(vm "count $LINK_BENCH $BENCH_ITEM"); vp0=$(vm "count 00000014 $BENCH_ITEM")
	rf0=$(field "$(vm storage)" refusedTakes); or0=$(field "$(vm storage)" ownRefused)
	walk vm "bench: set-up: the VM walks to Sanctuary's workbench" 0.8 150 $SANCT_BENCH || { pads_off; return; }
	vm "pad unplug" >/dev/null
	if ! _ws_enter vm; then fail "bench: the VM enters workshop mode at the host's workbench" "$(vm workshop) | $(vm menus | grep -oE 'open=[^ ]*')"; pads_off; return; fi
	# the host's 10 s supply-line report and the VM's paced views
	if wait_for 30 _vm_sees; then pass "bench: the VM views the host's workbench and the one its supply line links, with the host's counts" "$(_bench_view vm | grep -oE 'supply=.*')"
	else fail "bench: the VM views the host's workbench and the one its supply line links, with the host's counts" "VM: $(_bench_view vm | cut -c1-260) | host: Sanctuary $(host "count $SANCT_BENCH $BENCH_ITEM") Abernathy Farm $(host "count $LINK_BENCH $BENCH_ITEM")"; _ws_exit vm; pads_off; return; fi
	hs=$(host "count $SANCT_BENCH $BENCH_ITEM"); hr=$(host "count $LINK_BENCH $BENCH_ITEM")
	if ! is_int "$hs" || ! is_int "$hr"; then fail "bench: the host's piles are read before the build" "Sanctuary '$hs', Abernathy Farm '$hr': not numbers"; _ws_exit vm; pads_off; return; fi
	t0=$((hs + hr)); n=$((hs + 3))
	built=$(vm "storage build $BENCH_ITEM $n"); sleep 6
	t1=$(_host_total)
	if ! is_int "$t1"; then fail "bench: the host's piles are read after the VM's build" "$t1 ($built)"; _ws_exit vm; pads_off; return; fi
	expect_eq "bench: the VM's build of $n comes off the host's piles once (host total)" "$t1" "$((t0 - n))"
	expect_eq "bench: ... Sanctuary's emptied, 3 from Abernathy Farm's across the supply line" "$(host "count $SANCT_BENCH $BENCH_ITEM")/$(host "count $LINK_BENCH $BENCH_ITEM")" "0/$((hr - 3))"
	if wait_for 6 _list_is vm "$t1"; then pass "bench: the VM's list agrees with the host's piles" "$t1 ($built)"; else fail "bench: the VM's list agrees with the host's piles" "VM list $(field "$(_bench_view vm)" workshopList), host total $t1 ($built)"; fi
	vp1=$(vm "count 00000014 $BENCH_ITEM"); expect_eq "bench: the VM's pockets untouched by its build" "$vp1" "$vp0"
	rf1=$(field "$(vm storage)" refusedTakes); or1=$(field "$(vm storage)" ownRefused)
	if is_num "$rf0" && is_num "$rf1" && is_num "$or0" && is_num "$or1"; then expect_eq "bench: no take refused (refusedTakes and ownRefused unchanged)" "$((rf1 - rf0))/$((or1 - or0))" 0/0
	else fail "bench: no take refused (refusedTakes and ownRefused unchanged)" "counters unreadable: before '$rf0/$or0' after '$rf1/$or1'"; fi
	# the host's turn: its own build, the VM's view follows
	if walk host "bench: set-up: the host walks to its workbench" 0.8 150 $SANCT_BENCH; then
		host "pad unplug" >/dev/null
		if _ws_enter host; then
			t0=$(_host_total); built=$(host "storage build $BENCH_ITEM 2"); sleep 4; t1=$(_host_total)
			if ! is_int "$t0" || ! is_int "$t1"; then fail "bench: the host's piles are read around its build" "before $t0, after $t1 ($built)"
			else
				expect_eq "bench: the host's build of 2 (from Abernathy Farm's) comes off its piles" "$t1" "$((t0 - 2))"
				if wait_for 6 _list_is vm "$t1"; then pass "bench: the VM's list follows the host's build" "$t1"; else fail "bench: the VM's list follows the host's build" "VM list $(field "$(_bench_view vm)" workshopList), host total $t1 ($built)"; fi
			fi
			_ws_exit host
		else fail "bench: the host enters workshop mode at its workbench" "$(host workshop)"; fi
	fi
	_ws_exit vm || fail "bench: the VM leaves workshop mode" "$(vm workshop)"
	sleep 3
	expect_eq "bench: the VM's own stash at both workbenches back after workshop mode" "$(vm "count $SANCT_BENCH $BENCH_ITEM")/$(vm "count $LINK_BENCH $BENCH_ITEM")" "$vs0/$vr0"
	pads_off
}

# ---- container loot, both ways ----------------------------------------------------------------------------------------------------------
# A take from a container is everyone's (src/game/LootScope.h). The mailbox by the road into Concord, which is no settlement (a
# container in a settlement the host owns is storage, which has one true copy: the bench step makes Sanctuary the host's, so the
# loot is looked at elsewhere), gets the same stack in both games (console additem: a from-nothing add the mod never reports, as
# _loot stocks its chests). One player walks to it (around a car the straight line from the marker runs into) and takes with
# presses (the game's quick take from the crosshair's list: A on a pad, E on a keyboard) until its copy holds none of the stack,
# then walks back. Asserted: the taker has what left its copy, and the other game's copy lost that many too (down to none).
LOOT_BOX=${LOOT_BOX:-000222D0}
CONCORD_SPOT="-59400 72600"; CONCORD_WAY1="-59700 72900"; CONCORD_WAY2="-60200 73600"
_box_none() { [ "$(dc "$1" "count $LOOT_BOX $LOOT_ITEM")" = 0 ]; }
_box_is() { [ "$(dc "$1" "count $LOOT_BOX $LOOT_ITEM")" = "$2" ]; }
step_loot() {
	section "container loot (a take shows in the other game), both ways"
	local who other tb ob ib ia took oa want
	enter_both ConcordExt 14
	for who in vm host; do
		other=$(other_game $who)
		host "console $LOOT_BOX.additem $LOOT_ITEM $LOOT_N" >/dev/null; vm "console $LOOT_BOX.additem $LOOT_ITEM $LOOT_N" >/dev/null; sleep 1.5
		tb=$(dc $who "count $LOOT_BOX $LOOT_ITEM"); ob=$(dc $other "count $LOOT_BOX $LOOT_ITEM"); ib=$(dc $who "count 00000014 $LOOT_ITEM")
		if ! is_int "$tb" || ! is_int "$ob" || ! is_int "$ib" || [ "$tb" -lt $LOOT_N ] || [ "$ob" -lt $LOOT_N ]; then fail "loot ($who takes): set-up: the stack is in both copies" "$who '$tb' $other '$ob' (taker holds '$ib')"; continue; fi
		walk $who "loot ($who takes): set-up: walks to the mailbox" 1 100 $CONCORD_WAY1 && walk $who "loot ($who takes): set-up: walks to the mailbox" 1 100 $CONCORD_WAY2 \
			&& walk $who "loot ($who takes): set-up: walks to the mailbox" 0.8 90 $LOOT_BOX || { pads_off; continue; }
		if press_a $who $LOOT_BOX "wait_for 2 _box_none $who" auto 30 45 auto 15 60; then
			ia=$(dc $who "count 00000014 $LOOT_ITEM")
			if is_int "$ia"; then expect_eq "loot ($who takes, $PRESSES press(es)): the taker has the $tb that left its copy" "$((ia - ib))" "$tb"
			else fail "loot ($who takes, $PRESSES press(es)): the taker has the $tb that left its copy" "the taker's count after the take is '$ia', not a number"; fi
			want=$((ob - tb)); [ $want -lt 0 ] && want=0
			if wait_for 8 _box_is $other $want; then pass "loot ($who takes): the other game's copy lost the same (to none)" "$ob -> $want"
			else oa=$(dc $other "count $LOOT_BOX $LOOT_ITEM"); fail "loot ($who takes): the other game's copy lost the same (to none)" "$ob -> $oa (want $want)"; fi
		else fail "loot ($who takes): presses take the stack" "its copy still holds $(dc $who "count $LOOT_BOX $LOOT_ITEM") after $PRESSES presses ($(dc $who menus | grep -oE 'open=[^ ]*'))"; fi
		menu_open $who ContainerMenu 0 && { dc $who "pad button b" >/dev/null; sleep 1; }
		walk $who "loot ($who takes): set-up: walks back" 1 100 $CONCORD_WAY2; walk $who "loot ($who takes): set-up: walks back" 1 100 $CONCORD_WAY1; walk $who "loot ($who takes): set-up: walks back" 1 100 $CONCORD_SPOT
		dc $who "pad unplug" >/dev/null
	done
	pads_off
}

# ---- doors ---------------------------------------------------------------------------------------------------------------------------------
# A plain door's open state is shared, whoever moves it (src/game/DoorShare.h). One player walks to a gate in Sanctuary, opens (or
# closes) it with a press and steps away; then the other does the same, so the gate ends as it began. Asserted after each: the
# actor's game has the gate in its new state and the other game agrees (dev `doors`: open=1 open, 3 closed, 2 and 4 on the way).
DOOR_REF=${DOOR_REF:-00075315}
door_state() { dc "$1" "doors 3000" | tr ';' '\n' | grep -o "$2 '[^']*' open=[0-9]" | grep -o '[0-9]$'; }
door_is() { [ "$(door_state "$1" "$2")" = "$3" ]; }
step_doors() {
	section "doors (a gate opened and closed), both ways"
	local who other h v s0 want what
	enter_both SanctuaryExt 14
	h=$(door_state host $DOOR_REF); v=$(door_state vm $DOOR_REF)
	if [ -z "$h" ] || [ -z "$v" ]; then fail "doors: set-up: the gate is in both games" "host '$h' vm '$v' (doors within 3000 u of the marker)"; return; fi
	expect_eq "doors: the gate starts the same in both games" "$v" "$h"
	for who in vm host; do
		other=$(other_game $who); s0=$(door_state $who $DOOR_REF); want=$([ "$s0" = 1 ] && echo 3 || echo 1); what=$([ $want = 1 ] && echo opens || echo closes)
		walk $who "doors ($who $what the gate): set-up: walks to it" 0.8 90 $DOOR_REF || continue
		if press_a $who $DOOR_REF "wait_for 4 door_is $who $DOOR_REF $want" auto 15 0 30; then
			pass "doors ($who $what the gate): its own game" "open=$want after $PRESSES press(es)"
			if wait_for 6 door_is $other $DOOR_REF $want; then pass "doors ($who $what the gate): the other game agrees" "open=$want"
			else fail "doors ($who $what the gate): the other game agrees" "$other has open=$(door_state $other $DOOR_REF), want $want"; fi
		else fail "doors ($who $what the gate): its own game" "open=$(door_state $who $DOOR_REF) after $PRESSES presses (want $want)"; fi
		walk $who "doors ($who): set-up: steps away" 1 100 $PARK_SPOT; walk $who "doors ($who): set-up: back to the marker" 1 100 $MARKER_SPOT
		dc $who "pad unplug" >/dev/null
	done
	expect_eq "doors: the gate ends as it began, in both games" "$(door_state host $DOOR_REF)/$(door_state vm $DOOR_REF)" "$h/$h"
	pads_off
}

# ---- quests: a silent quest's stages, set in the host's game ------------------------------------------------------------------------------
# A quest with no objectives the host's sQuests = safe shares (src/game/QuestFilter.h SILENT; DN046, Fort Strong's own) gets two of its
# stages set by the host's console (setstage: Fort Strong is out of the rig's walking reach), the higher first. The host's setting is
# the one the `stages` line names ("silent (shared with the story: sQuests = safe)" or "(not shared: sQuests = story)"). With safe:
# the VM's game starts it for the session and sets both, out of order. With story: nothing of it moves there. Either way the VM's
# player stays where it was, with the same inventory (no teleport, nothing given or taken). Once a game has done them all (a soak's
# later rounds) there is nothing left to set: a PASS when both games agree, a WARN otherwise.
QUEST_SILENT=${QUEST_SILENT:-000423D5}; QUEST_STAGES=${QUEST_STAGES:-10 50 100 200}
_q_done() { dc "$1" "stages $QUEST_SILENT" | sed -nE 's/.* done\([0-9]+\)=([0-9,]+|none).*/\1/p'; }
_q_has() { case ",$(_q_done "$1")," in *",$2,"*) return 0;; esac; return 1; }
_q_moved() {  # _q_moved <pos before> <pos after>: how far the player went, in units (x and y)
	awk -v a="$(field "$1" x) $(field "$1" y)" -v b="$(field "$2" x) $(field "$2" y)" 'BEGIN{split(a,p," "); split(b,q," "); if (p[1]=="" || q[1]=="") {print "?"; exit} printf "%d", sqrt((q[1]-p[1])^2+(q[2]-p[2])^2)}'
}
step_quests() {
	section "quests (a silent quest's stages set in the host's game by console setstage)"
	local line kind todo s lo hi vd0 pos0 inv0 moved
	enter_both SanctuaryExt 14
	line=$(dc host "stages $QUEST_SILENT")
	case "$line" in
	*"sQuests = safe"*) kind=safe ;;
	*"sQuests = story"*) kind=story ;;
	*) fail "quests: set-up: the host reads $QUEST_SILENT as a silent quest" "got '$line'"; return ;;
	esac
	todo=""; for s in $QUEST_STAGES; do _q_has host "$s" || todo="$todo $s"; done
	read -r lo hi <<<"$(awk '{print $1, $NF}' <<<"$todo")"
	if [ -z "$lo" ] || [ "$lo" = "$hi" ]; then
		if [ $kind = story ] || [ "$(_q_done vm)" = "$(_q_done host)" ]; then pass "quests ($kind): nothing left to set" "host and VM done $(_q_done host)"
		else warn "quests ($kind): nothing left to set" "host done $(_q_done host), VM $(_q_done vm)"; fi
		return
	fi
	vd0=$(_q_done vm); pos0=$(dc vm pos); inv0=$(field "$(dc vm inventory)" hash)
	dc host "console setstage $QUEST_SILENT $hi" >/dev/null; sleep 3; dc host "console setstage $QUEST_SILENT $lo" >/dev/null
	if ! wait_for 10 _q_has host "$lo" || ! _q_has host "$hi"; then fail "quests ($kind): set-up: the host's console set $hi, then $lo" "host done $(_q_done host)"; return; fi
	if [ $kind = safe ]; then
		if wait_for 20 _q_has vm "$lo" && _q_has vm "$hi"; then pass "quests (safe): the VM's game set the host's $hi and $lo" "done $(_q_done vm) (was $vd0)"
		else fail "quests (safe): the VM's game set the host's $hi and $lo" "VM done $(_q_done vm) (was $vd0)"; fi
	else
		sleep 15; expect_eq "quests (story): nothing of it moves in the VM's game" "$(_q_done vm)" "$vd0"
	fi
	moved=$(_q_moved "$pos0" "$(dc vm pos)")
	if is_num "$moved"; then expect_le "quests ($kind): the VM's player stays where it was" "$moved" 64 u
	else fail "quests ($kind): the VM's player stays where it was" "no position read"; fi
	expect_eq "quests ($kind): the VM's inventory is unchanged" "$(field "$(dc vm inventory)" hash)" "$inv0"
}

# ---- the follow offer: a friend's stage that moves the player, the player far away (opt-in: smoke.sh --only follow) -------------------------
# A live stage that moves the player is set only with the friend who did it near; one far away is offered to go with them (src/game/
# StageRules.h Verdict::kOffer, QuestSync.cpp): a HUD line, the teleport key aimed at that friend, the quest waiting OFFER_FOR (90 s), and
# the stage set once the player is beside them. Two SAFE quests of Fallout4.esm (shared with the host's sQuests = safe and a shared story):
# DN070's 5 (MoveTo the Jamaica Plain treasure's entry: kMoves) and DN036's 500 (Phyllis's faction turned on the player: a HOSTILE row).
# Set-up by console, said so in the report: both games coc to SanctuaryExt, then the host's to FOLLOW_HOST_CELL (far: more than 6000 u).
# Accept: the host's console setstage DN070 5; the VM's log has the offer and the stage waits; the VM takes it with the teleport key's own
# action (dev `party go`, what the key calls; the offer aimed it at the host), and within FOLLOW_SET s its log has the stage set "after
# arriving beside player N" and the stage is done there. Far, a faction turned on the player: the VM's console coc back to SanctuaryExt
# (set-up); the host's console shows DN036's objective (a quest the player never started is not shared) and sets its 500; none of
# Phyllis's faction is near the VM (she is at Egret Tours Marina), so the VM's game sets it at once with the log's "none of them near",
# no offer made and nothing skipped (the hold while the faction is near is step_hostile's). Quest stages are for good in a save: run
# it on throwaway saves (backup.sh before, restore after). Once a game has a stage done there is nothing to test: a FAIL naming it.
FOLLOW_MOVE_QUEST=${FOLLOW_MOVE_QUEST:-000E9914}; FOLLOW_MOVE_STAGE=${FOLLOW_MOVE_STAGE:-5}     # DN070
FOLLOW_FIGHT_QUEST=${FOLLOW_FIGHT_QUEST:-0008FCED}; FOLLOW_FIGHT_STAGE=${FOLLOW_FIGHT_STAGE:-500}  # DN036
FOLLOW_FIGHT_OBJECTIVE=${FOLLOW_FIGHT_OBJECTIVE:-10}  # DN036 500 alone leaves the quest unstarted (no objective shown), and a quest the player never started isn't shared: shown first
FOLLOW_HOST_CELL=${FOLLOW_HOST_CELL:-ConcordExt}; FOLLOW_SET=${FOLLOW_SET:-45}; FOLLOW_OFFER_FOR=${FOLLOW_OFFER_FOR:-90}
_f_done() { dc "$1" "stages $2" | sed -nE 's/.* done\([0-9]+\)=([0-9,]+|none).*/\1/p'; }
_f_has() { case ",$(_f_done "$1" "$2")," in *",$3,"*) return 0;; esac; return 1; }
# _f_done_sure <who> <id>: the same, asked up to 3 times. "" = the game gave no answer, which is never "the stage is not done": a
# check that a stage is NOT set reads this and says rig broken on "" (_f_has is false both for "not done" and for no answer).
_f_done_sure() { local i d=""; for i in 1 2 3; do d=$(_f_done "$1" "$2"); [ -n "$d" ] && break; sleep 1; done; echo "$d"; }
_f_log_has() { grep -qE "$2" "$(win_read follow "$1")"; }   # _f_log_has <who> <regex>: the game's log since win_mark has it
# _cell_edid <who> [cellHex]: the editor ID of a cell (by default the one that game's player stands in; '' when not read).
# _coc_landed <who> <cell> [pos before]: the coc put the player in that cell, or (an exterior coc marker may stand over the cell's edge)
# at least 4096 u from where it stood before. A coc to a name no cell has does nothing, silently (2026-11-12: kickout's
# CambridgePoliceStationExt left the VM at Sanctuary for three runs).
_cell_edid() { local c=${2:-}; [ -z "$c" ] && c=$(field "$(dc "$1" status)" cell); [ -n "$c" ] && dc "$1" "edid $c" | sed -nE "s/^'([^']*)'.*/\1/p"; }
_coc_landed() {
	[ "$(_cell_edid "$1")" = "$2" ] && return 0
	[ -n "${3:-}" ] || return 1
	local d; d=$(_q_moved "$3" "$(dc "$1" pos)"); [ "$d" != "?" ] && [ "$d" -ge 4096 ]
}
_f_far() {  # _f_far: the host and the VM more than 6000 u apart, or in different spaces (an interior each, another worldspace)
	local h v hs vs; hs=$(dc host status); vs=$(dc vm status)
	[ "$(field "$hs" interior)" != "$(field "$vs" interior)" ] || [ "$(field "$hs" worldspace)" != "$(field "$vs" worldspace)" ] && return 0
	[ "$(field "$hs" interior)" = true ] && [ "$(field "$hs" cell)" != "$(field "$vs" cell)" ] && return 0
	h=$(dc host pos); v=$(dc vm pos)
	awk -v a="$(field "$h" x) $(field "$h" y)" -v b="$(field "$v" x) $(field "$v" y)" 'BEGIN{split(a,p," "); split(b,q," "); if (p[1]=="" || q[1]=="") exit 1; exit !(sqrt((q[1]-p[1])^2+(q[2]-p[2])^2) > 6000)}'
}
step_follow() {
	section "follow (a friend's stage that moves the player offered to a far player; console placement and setstage on the host)"
	local q s line kind
	for q in "$FOLLOW_MOVE_QUEST $FOLLOW_MOVE_STAGE" "$FOLLOW_FIGHT_QUEST $FOLLOW_FIGHT_STAGE"; do
		read -r line s <<<"$q"
		kind=$(dc host "stages $line")
		case "$kind" in *"sQuests = safe"*) ;; *) fail "follow: set-up: the host shares $line (sQuests = safe, a shared story)" "got '$kind'"; return;; esac
		if _f_has host "$line" "$s" || _f_has vm "$line" "$s"; then fail "follow: set-up: neither game has $line stage $s done (a throwaway save from before it)" "host $(_f_done host "$line"), VM $(_f_done vm "$line")"; return; fi
	done
	enter_both SanctuaryExt 14
	dc host "console coc $FOLLOW_HOST_CELL" >/dev/null; sleep 14; wait_ingame host 30
	if ! wait_for 20 _f_far; then fail "follow: set-up: the host's coc to $FOLLOW_HOST_CELL put it far from the VM" "host $(dc host pos); VM $(dc vm pos)"; return; fi

	# Accept: the VM goes with the host by the teleport key's action.
	win_mark follow vm
	dc host "console setstage $FOLLOW_MOVE_QUEST $FOLLOW_MOVE_STAGE" >/dev/null
	if ! wait_for 15 _f_has host "$FOLLOW_MOVE_QUEST" "$FOLLOW_MOVE_STAGE"; then fail "follow: set-up: the host's console set $FOLLOW_MOVE_QUEST $FOLLOW_MOVE_STAGE" "host done $(_f_done host "$FOLLOW_MOVE_QUEST")"; return; fi
	if wait_for 20 _f_log_has vm "QuestSync: offer to follow player [0-9]+ into $FOLLOW_MOVE_QUEST .* stage $FOLLOW_MOVE_STAGE "; then
		pass "follow: the far VM is offered to go with the host ($FOLLOW_MOVE_QUEST $FOLLOW_MOVE_STAGE)" "$(grep -oE "QuestSync: offer to follow player [0-9]+ into $FOLLOW_MOVE_QUEST[^;]*" "$A_OUT/windelta-follow-vm.txt" | head -1)"
	else
		fail "follow: the far VM is offered to go with the host ($FOLLOW_MOVE_QUEST $FOLLOW_MOVE_STAGE)" "no offer line in the VM's log; VM done $(_f_done vm "$FOLLOW_MOVE_QUEST")"; return
	fi
	sleep 3
	if _f_has vm "$FOLLOW_MOVE_QUEST" "$FOLLOW_MOVE_STAGE"; then fail "follow: the stage waits in the VM's game while the offer is open" "already done: $(_f_done vm "$FOLLOW_MOVE_QUEST")"
	else pass "follow: the stage waits in the VM's game while the offer is open" "VM done $(_f_done vm "$FOLLOW_MOVE_QUEST")"; fi
	dc vm "party go" >/dev/null
	if wait_for "$FOLLOW_SET" _f_log_has vm "QuestSync: set $FOLLOW_MOVE_QUEST .* stage $FOLLOW_MOVE_STAGE .*after arriving beside player [0-9]+ by the teleport key" \
		&& wait_for 10 _f_has vm "$FOLLOW_MOVE_QUEST" "$FOLLOW_MOVE_STAGE"; then
		pass "follow: after the teleport key the VM's game sets it beside the host" "$(grep -oE "QuestSync: set $FOLLOW_MOVE_QUEST .*" "$A_OUT/windelta-follow-vm.txt" | head -1)"
	else
		fail "follow: after the teleport key the VM's game sets it beside the host" "VM done $(_f_done vm "$FOLLOW_MOVE_QUEST"); $(grep -E "Party: (teleport|can't|arrived)|QuestSync: (set|not setting|the offer) " "$A_OUT/windelta-follow-vm.txt" | tail -3 | tr '\n' '|')"
	fi

	# A faction turned on the player, the VM far again and none of that faction near: set at once, no offer, nothing skipped.
	dc vm "console coc SanctuaryExt" >/dev/null; sleep 14; wait_ingame vm 30
	if ! wait_for 20 _f_far; then fail "follow: set-up: the VM's coc back to SanctuaryExt put it far from the host" "host $(dc host pos); VM $(dc vm pos)"; return; fi
	dc host "console setobjectivedisplayed $FOLLOW_FIGHT_QUEST $FOLLOW_FIGHT_OBJECTIVE 1" >/dev/null; sleep 2   # set-up: the host has the quest started
	win_mark follow vm
	dc host "console setstage $FOLLOW_FIGHT_QUEST $FOLLOW_FIGHT_STAGE" >/dev/null
	if ! wait_for 15 _f_has host "$FOLLOW_FIGHT_QUEST" "$FOLLOW_FIGHT_STAGE"; then fail "follow: set-up: the host's console set $FOLLOW_FIGHT_QUEST $FOLLOW_FIGHT_STAGE" "host done $(_f_done host "$FOLLOW_FIGHT_QUEST")"; return; fi
	if wait_for 30 _f_log_has vm "QuestSync: set $FOLLOW_FIGHT_QUEST .* stage $FOLLOW_FIGHT_STAGE .*it turns [0-9A-F]{8} '[^']*' on the player: none of them near" \
		&& wait_for 10 _f_has vm "$FOLLOW_FIGHT_QUEST" "$FOLLOW_FIGHT_STAGE"; then
		pass "follow: a faction turned on the player, the VM far and none of that faction near: set at once ($FOLLOW_FIGHT_QUEST $FOLLOW_FIGHT_STAGE)" "$(grep -oE "QuestSync: set $FOLLOW_FIGHT_QUEST .*" "$A_OUT/windelta-follow-vm.txt" | head -1)"
	else
		fail "follow: a faction turned on the player, the VM far and none of that faction near: set at once ($FOLLOW_FIGHT_QUEST $FOLLOW_FIGHT_STAGE)" "VM done $(_f_done vm "$FOLLOW_FIGHT_QUEST"); $(grep -E "QuestSync: (set|not setting|the offer|offer) " "$A_OUT/windelta-follow-vm.txt" | tail -3 | tr '\n' '|')"
	fi
	if grep -qE "QuestSync: (offer to follow|not setting) .*$FOLLOW_FIGHT_QUEST .* stage $FOLLOW_FIGHT_STAGE" "$A_OUT/windelta-follow-vm.txt"; then
		fail "follow: no offer for it, nothing skipped" "$(grep -oE "QuestSync: (offer to follow|not setting) .*$FOLLOW_FIGHT_QUEST .*" "$A_OUT/windelta-follow-vm.txt" | head -1)"
	else pass "follow: no offer for it, nothing skipped" "the VM's log has neither"; fi
}

# ---- hostile: a friend's stage that turns a faction on a far player waits while any of them is near, then lands (opt-in: smoke.sh --only hostile)
# A shared quest's stage that turns a faction on the player (src/game/StageRules.h HOSTILE, Verdict::kHostileWaits, QuestSync.cpp,
# src/game/HostileHold.h) is set live beside the friend who did it, and otherwise waits, its quest with it, while any of that faction is
# alive and loaded within HostileHold::RANGE (8192 u) of this player or they are fighting; it is set once none is, and never skipped. The
# Children of Atom's kick-out: DLC03CoA_DialogueNucleus 70 (Far Harbor, SAFE: shared with the host's sQuests = safe and a shared story; it
# sets DLC03CoA_Kickout 700, SetEnemy DLC03ChildrenofAtomFaction PlayerFaction). Set-up by console, said so in the report: the host coc
# to HOSTILE_HOST_CELL, the VM coc into the Nucleus (HOSTILE_NEAR_CELL: among the Children of Atom, far from the host); the host's console
# shows the quest's objective (a quest the player never started isn't shared) and sets the stage, once both games' catch-up after the
# join is over and a Child of Atom is asserted near the VM's player (set-up checks, the near-actor list printed). Asserted: the VM's log has the hold
# line naming the faction and how many of them are near, and the stage is not done there HOSTILE_HOLD s later (a game that gives no
# answer then is the rig's, not a pass), with no offer and nothing skipped in the log read after that wait; then the VM leaves the
# Nucleus by console placement to HOSTILE_AWAY_CELL (far from the host too, so the stage lands by the faction being out of sight, not
# beside the friend; a walk out of the Nucleus is no path the rig has), and within HOSTILE_LAND s the stage is done there, the log's
# "after N s of play waiting for ... to be out of sight". Quest stages are for good in a save, and this one ends the Children of Atom for
# the VM: run it on throwaway saves (backup.sh before, restore after); a game with the stage done is a FAIL naming it.
HOSTILE_QUEST=${HOSTILE_QUEST:-DLC03CoA_DialogueNucleus}; HOSTILE_STAGE=${HOSTILE_STAGE:-70}; HOSTILE_OBJECTIVE=${HOSTILE_OBJECTIVE:-40}   # its one objective is 40
HOSTILE_HOST_CELL=${HOSTILE_HOST_CELL:-ConcordExt}; HOSTILE_NEAR_CELL=${HOSTILE_NEAR_CELL:-DLC03Nucleus}; HOSTILE_AWAY_CELL=${HOSTILE_AWAY_CELL:-SanctuaryExt}
HOSTILE_HOLD=${HOSTILE_HOLD:-20}; HOSTILE_LAND=${HOSTILE_LAND:-60}
_h_id() { dc "$1" "quests edid:$2" | tr ';' '\n' | awk -v e="$2" '$2 == e { print $1; exit }'; }
_h_log_has() { grep -qE "$2" "$(win_read hostile "$1")"; }   # _h_log_has <who> <regex>: the game's log since win_mark has it
# _h_quests <who>: that game's quest counters (`net status`'s "quests: reported=..." part: WorldSync's applied= comes before it)
_h_quests() { dc "$1" 'net status' | grep -oE 'quests: reported=[^|]*' | head -1; }
# _h_catchup_over <who> [quiet s] [max s]: that game's catch-up of the session's stages after its join is over: QuestSync's applied,
# caughtUp and setting hold still across <quiet> s and nothing is being set (setting=0); up to <max> s. A live stage set before it
# waits behind the whole queue, about half a second a stage (2026-11-11: 37 s and 47 s behind about 150 persisted stages), and the
# hold the step looks for came after its wait. Prints what it saw.
_h_catchup_over() {
	local w=$1 quiet=${2:-10} max=${3:-240} t0=$SECONDS q a="" b=""
	while [ $((SECONDS - t0)) -lt "$max" ]; do
		q=$(_h_quests "$w"); b="applied=$(field "$q" applied) caughtUp=$(field "$q" caughtUp) setting=$(field "$q" setting)"
		if [ -n "$(field "$q" applied)" ] && [ "$b" = "$a" ] && [ "$(field "$q" setting)" = 0 ]; then echo "$w: the join's catch-up is over after $((SECONDS - t0)) s ($b)"; return 0; fi
		a=$b; sleep "$quiet"
	done
	echo "$w: the join's catch-up still going after $max s ($b)"; return 1
}
# _h_near <who> <faction>: dev `near <faction>`: the faction's members the hold counts as near that game's player, read as the hold reads
# them (every actor in the game's high process, so every loaded cell, alive, enabled, no stand-in, not following a player, within
# HostileHold::RANGE). "near <faction> within 8192: <n> <ref> '<name>'; ...".
_h_near() { dc "$1" "near $2" | tr -d '\r'; }
_h_near_any() { local n; n=$(sed -nE 's/^near [0-9A-F]{8} within [0-9]+: ([0-9]+).*/\1/p' <<<"$(_h_near "$1" "$2")"); is_int "$n" && [ "$n" -gt 0 ]; }
# _h_assert_near <label> <who> <faction>: a set-up check that a member of the faction is near that game's player before the stage is
# set (2026-11-11: kickout's VM set the stage with "none of them near", and the log could not say whether one really was). Prints
# what `near` read either way. 1 when none is (or the read failed).
_h_assert_near() {
	local r n
	r=$(_h_near "$2" "$3")
	echo "$1: the members of $3 near the $2's player, as the hold counts them: $r"
	n=$(sed -nE 's/^near [0-9A-F]{8} within [0-9]+: ([0-9]+).*/\1/p' <<<"$r")
	if is_int "$n" && [ "$n" -gt 0 ]; then pass "$1: set-up: a member of $3 is near the $2's player" "$(cut -c1-300 <<<"$r")"; return 0; fi
	fail "$1: set-up: a member of $3 is near the $2's player" "dev near read '$(cut -c1-300 <<<"$r")'"; return 1
}
step_hostile() {
	section "hostile (a friend's stage that turns a faction on the VM's player waits while they are near; console placement and setstage)"
	local id line done1 log
	id=$(_h_id host "$HOSTILE_QUEST")
	if [ -z "$id" ] || [ "$id" != "$(_h_id vm "$HOSTILE_QUEST")" ]; then fail "hostile: set-up: both games have $HOSTILE_QUEST (the add-on loaded, the same form ID)" "host '$id', VM '$(_h_id vm "$HOSTILE_QUEST")'"; return; fi
	line=$(dc host "stages $id")
	case "$line" in *"sQuests = safe"*) ;; *) fail "hostile: set-up: the host shares $HOSTILE_QUEST (sQuests = safe, a shared story)" "got '$line'"; return;; esac
	if _f_has host "$id" "$HOSTILE_STAGE" || _f_has vm "$id" "$HOSTILE_STAGE"; then fail "hostile: set-up: neither game has $HOSTILE_QUEST stage $HOSTILE_STAGE done (a throwaway save from before it)" "host $(_f_done host "$id"), VM $(_f_done vm "$id")"; return; fi
	enter_both SanctuaryExt 14
	dc host "console coc $HOSTILE_HOST_CELL" >/dev/null; sleep 14; wait_ingame host 30
	dc vm "console coc $HOSTILE_NEAR_CELL" >/dev/null; sleep 20; wait_ingame vm 30
	if ! wait_for 20 _f_far; then fail "hostile: set-up: the host in $HOSTILE_HOST_CELL and the VM in $HOSTILE_NEAR_CELL are far apart" "host $(dc host status); VM $(dc vm status)"; return; fi
	sleep 5   # the Nucleus's people loaded around the VM
	# The join's (and the loads') catch-up over in both games first, so the live stage isn't queued behind it; and a Child of Atom
	# near the VM's player, asserted, so a stage set with "none of them near" says something.
	if ! _h_catchup_over vm || ! _h_catchup_over host; then fail "hostile: set-up: the join's catch-up is over in both games before the stage" "host $(_h_quests host | cut -c1-120); VM $(_h_quests vm | cut -c1-120)"; return; fi
	_h_assert_near hostile vm "${HOSTILE_FACTION:-${id:0:2}004462}" || return   # DLC03ChildrenofAtomFaction (Far Harbor's 004462: StageRules' HOSTILE row of 70)
	dc host "console setobjectivedisplayed $HOSTILE_QUEST $HOSTILE_OBJECTIVE 1" >/dev/null; sleep 2   # set-up: the host has the quest started
	win_mark hostile vm
	dc host "console setstage $HOSTILE_QUEST $HOSTILE_STAGE" >/dev/null
	if ! wait_for 15 _f_has host "$id" "$HOSTILE_STAGE"; then fail "hostile: set-up: the host's console set $HOSTILE_QUEST $HOSTILE_STAGE" "host done $(_f_done host "$id")"; return; fi
	if wait_for 20 _h_log_has vm "QuestSync: $id .* stage $HOSTILE_STAGE turns [0-9A-F]{8} '[^']*' on the player, and [0-9]+ of them (is|are) near; waiting until none of them is within"; then
		pass "hostile: the far VM's game holds the stage while the faction is near" "$(grep -oE "QuestSync: $id .* stage $HOSTILE_STAGE turns [^;]*" "$A_OUT/windelta-hostile-vm.txt" | head -1)"
	else
		fail "hostile: the far VM's game holds the stage while the faction is near" "no hold line in the VM's log; VM done $(_f_done vm "$id"); $(grep -E "QuestSync: (set|not setting|offer) " "$A_OUT/windelta-hostile-vm.txt" | tail -3 | tr '\n' '|')"; return
	fi
	sleep "$HOSTILE_HOLD"
	# A game that gives no answer has not shown the stage unset, and a log that could not be read has not shown nothing skipped.
	done1=$(_f_done_sure vm "$id")
	if [ -z "$done1" ]; then rig_broken game "hostile: the stage is not set while they are near ($HOSTILE_HOLD s)" "the VM's game gave no answer to 'stages $id', asked 3 times: '$(dc vm "stages $id" | cut -c1-120)'"; return; fi
	case ",$done1," in
	*",$HOSTILE_STAGE,"*) fail "hostile: the stage is not set while they are near ($HOSTILE_HOLD s)" "done: $done1";;
	*) pass "hostile: the stage is not set while they are near ($HOSTILE_HOLD s)" "VM done $done1";;
	esac
	log=$(win_read hostile vm)   # read again after the wait: what the game logged during it is what this check is about
	if ! grep -qE "QuestSync: $id .* stage $HOSTILE_STAGE turns " "$log"; then rig_broken game "hostile: no offer for it, nothing skipped" "the VM's log could not be read after the wait: the hold line it had before is not in it"; return; fi
	if grep -qE "QuestSync: (offer to follow|not setting) .*$id .* stage $HOSTILE_STAGE" "$log"; then
		fail "hostile: no offer for it, nothing skipped" "$(grep -oE "QuestSync: (offer to follow|not setting) .*$id .*" "$log" | head -1)"
	else pass "hostile: no offer for it, nothing skipped" "the VM's log, read after the $HOSTILE_HOLD s, has neither"; fi
	# Out of the Nucleus (console placement), still far from the host: the faction out of sight, the stage lands.
	dc vm "console coc $HOSTILE_AWAY_CELL" >/dev/null; sleep 14; wait_ingame vm 30
	if ! wait_for 20 _f_far; then fail "hostile: set-up: the VM's coc to $HOSTILE_AWAY_CELL put it far from the host" "host $(dc host pos); VM $(dc vm pos)"; return; fi
	if wait_for "$HOSTILE_LAND" _h_log_has vm "QuestSync: set $id .* stage $HOSTILE_STAGE .*after [0-9]+ s of play waiting for [0-9A-F]{8} '[^']*' to be out of sight" \
		&& wait_for 10 _f_has vm "$id" "$HOSTILE_STAGE"; then
		pass "hostile: with none of the faction near, the VM's game sets it" "$(grep -oE "QuestSync: set $id .*" "$A_OUT/windelta-hostile-vm.txt" | head -1)"
	else
		fail "hostile: with none of the faction near, the VM's game sets it" "VM done $(_f_done vm "$id"); $(grep -E "QuestSync: (set|not setting|$id) " "$A_OUT/windelta-hostile-vm.txt" | tail -3 | tr '\n' '|')"
	fi
}

# ---- more quests sQuests = safe shares, set in the host's game (opt-in: smoke.sh --only quests_safe) ----------------------------------------
# Quests QuestFilter::SAFE gained by three rules (src/game/QuestFilter.h, tools/questscan.cs): Atom Cats Garage (DN054, whose 1 starts the
# garage's radio station, each game's own: ALLOW_STARTS), Vault 75 (DN143) and the Wasteland Refrigerator's two misc objectives
# (MS01MiscObj, MS01MiscObjSold: repeatable by their flags, run once as the game starts them: RUNS_ONCE), and the Detective Case Files
# (MS07, whose 0 is a developer's quickstart: StageRules' kDebug). Set-up by console, said so in the report: both games coc to
# SanctuaryExt; the host's console sets each quest's QSAFE_QUESTS stages (in the order listed: a lower one first where two are, as a
# friend's game drops a stage below one it has done as settled), and the
# VM's game must reach the same stages within QSAFE_WAIT s, its player staying where it was. Vault 75: what its created aliases make are
# the four ID cards in its key holders' inventories, not creatures, so nothing of it pairs (QuestActors pairs creatures only): checked is
# that both games' created aliases hold their items alike, and that no pairing of it is counted. First (QSAFE_DEBUG0=1, the default), before
# any other stage of MS07, while neither game has started it: the host's console sets MS07's 0, the quickstart, which moves the host to a
# test marker and makes Nick its companion; the host's game never reports it (its log says so) and the VM's game must not have it,
# and the host's console coc's back to SanctuaryExt. (Set after MS07's other stages, the VM's game would drop it as already settled, and
# the check would pass without testing anything.) The quickstart's SetCompanion would ask the host which companion to let go, a menu a
# scripted run can't answer: the host's companion and Dogmeat are dismissed by console first (quietly), Nick dismissed after, and the
# host's own put back in their aliases by console. Needs the host's sQuests = safe and a shared story (`rules`). Quest stages are for good in
# a save: run it on throwaway saves from before these quests (backup.sh before, restore after); a stage already done in either game is a
# FAIL naming it. Atom Cats Garage (DN054, 00048516) is not in the default list: it is start-enabled, so its stage 1 is done at load in
# any save and the set-up can never find it undone (2026-11-11). Add it back by QSAFE_QUESTS only on a save where it isn't.
QSAFE_QUESTS=${QSAFE_QUESTS:-"000FCB15:100 00028657:10 000704F4:10 000229FC:50,99"}
QSAFE_MS07=${QSAFE_MS07:-000229FC}; QSAFE_VAULT75=${QSAFE_VAULT75:-000FCB15}
QSAFE_WAIT=${QSAFE_WAIT:-25}; QSAFE_DEBUG0=${QSAFE_DEBUG0:-1}
# quest:stage pairs MS07 0's script sets in the host's game, none of which the VM's game may get (DialogueDiamondCity 170, MQ103 100
# and 1000, MQ104 10 and 1000); a pair the VM's game had done before is left out of the check.
QSAFE_CHAIN=${QSAFE_CHAIN:-"00003648:170 000229E5:100 000229E5:1000 0001F25E:10 0001F25E:1000"}
_qs_log_has() { grep -qE "$2" "$(win_read quests_safe "$1")"; }   # _qs_log_has <who> <regex>: the game's log since win_mark has it
_qs_companions_free() { [ -z "$(_ch_held host)" ] && [ -z "$(_c_held host)" ]; }   # the host's Companion and Dogmeat aliases both empty
# _qs_quickstart <VM pos before>: the host's console sets MS07's 0 (before any other MS07 stage), the VM's game must skip it and say why.
# The host's companion and Dogmeat are let go first and put back after (QS_HUMAN, QS_DOG: what the aliases held).
_qs_quickstart() {
	local pos0=$1 moved re nick vd dk0 dk1
	QS_HUMAN=$(_ch_held host); QS_DOG=$(_c_held host)
	[ -n "$QS_HUMAN" ] && dc host "console cqf Followers \"FollowersScript.DismissCompanion\" $QS_HUMAN 0 1" >/dev/null
	[ -n "$QS_DOG" ] && _c_dismiss host
	if ! wait_for 15 _qs_companions_free; then
		fail "quests_safe: set-up: the host's companion let go before $QSAFE_MS07 0 (its SetCompanion would ask which to dismiss)" "Companion '$(_ch_held host)', Dogmeat '$(_c_held host)'"
		return 1
	fi
	win_mark quests_safe host; win_mark quests_safe vm
	dk0=$(field "$(dc host 'net status')" debugKept)
	# What MS07 0's script sets in the host's game (StageRules::QUICKSTART_SETS: DialogueDiamondCity 170, through it 600, MQ103's
	# end and MQ104's start; MQ103's and MQ104's 1000 seen reaching the VM on 2026-11-12) that the VM's game hasn't done yet.
	local item chain=""
	for item in $QSAFE_CHAIN; do _qa_has vm "${item%%:*}" "${item#*:}" || chain="$chain $item"; done
	dc host "console setstage $QSAFE_MS07 0" >/dev/null
	if ! wait_for 15 _qa_has host "$QSAFE_MS07" 0; then
		fail "quests_safe: set-up: the host's console set $QSAFE_MS07 0" "host done $(_qa_done host "$QSAFE_MS07")"
	else
		# Since the quickstart rule (StageRules::IsDebug) the host's game never reports a quickstart (it counts each in `net status`'s
		# debugKept= and logs only the first of the game's run, "not reporting ... a developer's quickstart"), so the VM's game never
		# hears of it: the proof is the VM's game, by its own answer, without stage 0 QSAFE_WAIT s later and with no "set" line for it.
		# Its "not setting ... a developer's quickstart" line comes only from an older host or a session file that has the stage.
		re="QuestSync: not reporting $QSAFE_MS07 stage 0: it is a developer's quickstart"
		sleep "$QSAFE_WAIT"; vd=$(_qa_done vm "$QSAFE_MS07")
		if [ -z "$vd" ]; then rig_broken game "quests_safe: the host's $QSAFE_MS07 0 (a developer's quickstart) is not set in the VM's game" "the VM's game gave no answer to 'stages $QSAFE_MS07': nothing read, so nothing shown"
		elif [[ ",$vd," != *",0,"* ]] && ! _qs_log_has vm "QuestSync: set $QSAFE_MS07 '[^']*' stage 0 "; then
			pass "quests_safe: the host's $QSAFE_MS07 0 (a developer's quickstart) is not set in the VM's game" "VM done $vd; host: $(grep -oE "$re" "$(win_read quests_safe host)" | head -1 || true)"
		else
			fail "quests_safe: the host's $QSAFE_MS07 0 (a developer's quickstart) is not set in the VM's game" "VM done $vd; $(grep -E "QuestSync: (set|not setting|starting) .*$QSAFE_MS07" "$(win_read quests_safe vm)" | tail -3 | tr '\n' '|')"
		fi
		# Nor is what its script set in the host's game (not reported while MS07 0 is done there), and the VM's game doesn't
		# start MQ105 for an MQ104 it thinks was left stuck (RepairStory). Read after the same QSAFE_WAIT.
		local got=""
		for item in $chain; do _qa_has vm "${item%%:*}" "${item#*:}" && got="$got $item"; done
		if [ -z "$chain" ]; then warn "quests_safe: what the host's $QSAFE_MS07 0 sets does not reach the VM's game" "the VM's game had all of '$QSAFE_CHAIN' done before: nothing to test"
		elif [ -z "$got" ] && ! _qs_log_has vm "QuestSync: MQ104 is at .* MQ105 never started; starting it"; then
			pass "quests_safe: what the host's $QSAFE_MS07 0 sets does not reach the VM's game" "none of${chain} done in the VM's game; no MQ105 repair"
		else
			fail "quests_safe: what the host's $QSAFE_MS07 0 sets does not reach the VM's game" "VM done now:${got:- none}; $(grep -E "QuestSync: (set|MQ104 is at) " "$(win_read quests_safe vm)" | tail -3 | tr '\n' '|')"
		fi
		# The host's own side: its count of quickstarts kept went up (the log line is only the first of the game's run, so it can't
		# be required). A WARN, not a FAIL: a stage of a quest the session hasn't started waits unjudged (StageLog::Unstarted), and
		# the VM's answer above is the test.
		dk1=$(field "$(dc host 'net status')" debugKept)
		if ! is_int "$dk0" || ! is_int "$dk1" || [ "$dk1" -le "$dk0" ]; then
			warn "quests_safe: the host's game counts its $QSAFE_MS07 0 as a quickstart it doesn't report" "debugKept '$dk0' then '$dk1'; log: '$(grep -oE "$re" "$(win_read quests_safe host)" | head -1)'"
		fi
		moved=$(_q_moved "$pos0" "$(dc vm pos)")
		if is_num "$moved"; then expect_le "quests_safe: the VM's player stays where it was after the host's quickstart" "$moved" 64 u
		else fail "quests_safe: the VM's player stays where it was after the host's quickstart" "no position read"; fi
	fi
	dc host "console coc SanctuaryExt" >/dev/null; sleep 14; wait_ingame host 30
	# Put the host's companions back as they were: Nick (the quickstart's) let go, the host's own recruited again by console.
	nick=$(_ch_held host)
	if [ -n "$nick" ] && [ "$nick" != "$QS_HUMAN" ]; then dc host "console cqf Followers \"FollowersScript.DismissCompanion\" $nick 0 1" >/dev/null; sleep 4; fi
	if [ -n "$QS_HUMAN" ] && [ "$(_ch_held host)" != "$QS_HUMAN" ]; then
		dc host "console cqf Followers \"FollowersScript.SetCompanion\" $QS_HUMAN 1 1 1" >/dev/null; sleep 1; dc host "console $QS_HUMAN.evp" >/dev/null
	fi
	if [ -n "$QS_DOG" ] && [ "$(_c_held host)" != "$QS_DOG" ]; then
		dc host "console cqf Followers \"FollowersScript.SetDogmeatCompanion\" $QS_DOG" >/dev/null; sleep 1; dc host "console $QS_DOG.evp" >/dev/null
	fi
	sleep 3
	expect_eq "quests_safe: set-up: the host's companions as they were before the quickstart (Companion/Dogmeat)" "$(_ch_held host)/$(_c_held host)" "$QS_HUMAN/$QS_DOG"
	return 0   # only a failed set-up before the quickstart (return 1) stops the step
}
step_quests_safe() {
	section "quests_safe (SAFE's new quests: the host's console setstage, the VM's game the same; console placement and setstage on the host)"
	local item q stages s line pos0 moved hl vl hp vp all
	for item in $QSAFE_QUESTS; do
		q=${item%%:*}; stages=${item#*:}
		line=$(dc host "stages $q")
		case "$line" in *"sQuests = safe"*) ;; *) fail "quests_safe: set-up: the host shares $q (sQuests = safe, a shared story)" "got '$line'"; return;; esac
		for s in ${stages//,/ }; do
			if _qa_has host "$q" "$s" || _qa_has vm "$q" "$s"; then fail "quests_safe: set-up: neither game has $q stage $s done (a throwaway save from before it)" "host $(_qa_done host "$q"), VM $(_qa_done vm "$q")"; return; fi
		done
	done
	if [ "$QSAFE_DEBUG0" = 1 ] && { _qa_has host "$QSAFE_MS07" 0 || _qa_has vm "$QSAFE_MS07" 0; }; then
		fail "quests_safe: set-up: neither game has $QSAFE_MS07 stage 0 done" "host $(_qa_done host "$QSAFE_MS07"), VM $(_qa_done vm "$QSAFE_MS07")"; return
	fi
	enter_both SanctuaryExt 14
	pos0=$(dc vm pos)
	# MS07's 0, the developer's quickstart, first: neither game has started MS07, so the VM's game judges it as the quest's start
	# (set after MS07's other stages, it would be dropped there as already settled, and the check would test nothing).
	if [ "$QSAFE_DEBUG0" = 1 ]; then _qs_quickstart "$pos0" || return; fi
	hp=$(_qa_paired host pairs); vp=$(_qa_paired vm pairs)
	for item in $QSAFE_QUESTS; do
		q=${item%%:*}; stages=${item#*:}
		for s in ${stages//,/ }; do dc host "console setstage $q $s" >/dev/null; sleep 3; done
		all=1; for s in ${stages//,/ }; do wait_for 10 _qa_has host "$q" "$s" || all=0; done
		if [ $all = 0 ]; then fail "quests_safe: set-up: the host's console set $q ${stages//,/ then }" "host done $(_qa_done host "$q")"; continue; fi
		all=1; for s in ${stages//,/ }; do wait_for "$QSAFE_WAIT" _qa_has vm "$q" "$s" || all=0; done
		if [ $all = 1 ]; then pass "quests_safe: the VM's game set the host's $q ${stages//,/ and }" "host done $(_qa_done host "$q"), VM $(_qa_done vm "$q")"
		else fail "quests_safe: the VM's game set the host's $q ${stages//,/ and }" "host done $(_qa_done host "$q"), VM $(_qa_done vm "$q")"; fi
	done
	moved=$(_q_moved "$pos0" "$(dc vm pos)")
	if is_num "$moved"; then expect_le "quests_safe: the VM's player stays where it was" "$moved" 64 u
	else fail "quests_safe: the VM's player stays where it was" "no position read"; fi

	# Vault 75: its created aliases (the ID cards) filled alike in both games; nothing of it paired (no creature).
	if _qa_started vm "$QSAFE_VAULT75"; then
		sleep 5
		hl=$(_qa_aliases host "$QSAFE_VAULT75" . 4 | awk '{print $1}' | sort | tr '\n' ' '); vl=$(_qa_aliases vm "$QSAFE_VAULT75" . 4 | awk '{print $1}' | sort | tr '\n' ' ')
		if [ -n "$hl" ] && [ "$hl" = "$vl" ]; then pass "quests_safe: Vault 75's created aliases hold their items in both games" "aliases ${hl% }"
		elif [ -z "$hl" ] && [ -z "$vl" ]; then warn "quests_safe: Vault 75's created aliases hold their items in both games" "neither game's \`aliases\` shows a reference for them (an item in an inventory may have none)"
		else fail "quests_safe: Vault 75's created aliases hold their items in both games" "host '${hl% }', VM '${vl% }'"; fi
		if [ "$(_qa_paired host pairs)" = "$hp" ] && [ "$(_qa_paired vm pairs)" = "$vp" ]; then pass "quests_safe: nothing of Vault 75 is paired (its created aliases are items, not creatures)" "pairs host $hp, VM $vp"
		else warn "quests_safe: nothing of Vault 75 is paired (its created aliases are items, not creatures)" "pairs host $hp -> $(_qa_paired host pairs), VM $vp -> $(_qa_paired vm pairs) (another quest's pairing in the meantime?)"; fi
	fi
}

# ---- achievements: a caught-up stage's achievement is dropped, a live one is the game's (opt-in: smoke.sh --only achievements) -----------
# A friend's stage set here runs its script, Game.AddAchievement included. The maintainer's rule (src/game/Achievements.cpp,
# AchievementGate.h): a stage that happens live while both are in the session awards both players; one caught up for a joiner (done
# while they were out of the session, sent in the welcome) awards nothing: the call its fragment makes is dropped. Two stages of the shared
# story whose own fragment awards (src/game/AchievementStages.h), each set by the host's console after an objective of its quest is shown
# (a quest the player never started is not shared), on throwaway saves (backup.sh before, restore after); once a game has one done there
# is nothing to test: a FAIL naming it. Live: ACH_LIVE_QUEST's stage with the VM in the session; the VM's log has QuestSync set it and
# "Game.AddAchievement from ...Fragment_Stage_<stage>...: a live stage's, the game's call runs" (the call's achievement id isn't in the line: the
# fragment names the stage, and src/game/AchievementStages.h's comments say which achievement). Caught up: the VM's game drops its
# connection (dev `net drop`: it reconnects by itself in ~5 s and is welcomed with the session's stages), the host sets ACH_CATCHUP_QUEST's
# stage meanwhile (its report reaches the server in 0.5 s); the VM's log has QuestSync set it "(catching up)" and the award "dropped".
# With mods loaded the game awards nothing to Steam either way; the gate's lines are what is checked, so the step holds with any save.
# The hook itself must be in place first (`net status` on the VM: hook=yes); the hook's own install is the game test of this engine hook.
ACH_LIVE_QUEST=${ACH_LIVE_QUEST:-0003A457}; ACH_LIVE_STAGE=${ACH_LIVE_STAGE:-650}; ACH_LIVE_OBJECTIVE=${ACH_LIVE_OBJECTIVE:-10}            # Min02 "Sanctuary" (achievement 14)
ACH_CATCHUP_QUEST=${ACH_CATCHUP_QUEST:-000AA778}; ACH_CATCHUP_STAGE=${ACH_CATCHUP_STAGE:-980}; ACH_CATCHUP_OBJECTIVE=${ACH_CATCHUP_OBJECTIVE:-10}  # Min03 "Old Guns" (achievement 15)
ACH_SET=${ACH_SET:-60}; ACH_AWARD=${ACH_AWARD:-30}; ACH_REJOIN=${ACH_REJOIN:-90}
_a_log_has() { grep -qE "$2" "$(win_read ach "$1")"; }   # _a_log_has <who> <regex>: the game's log since win_mark has it
_a_hook() { dc vm "net status" | grep -oE 'achievements: [^|]*' | grep -oE 'hook=[a-z]+' | head -1; }
_a_joined() { dc vm "net status" | grep -q status=joined; }
_a_setup() {  # _a_setup <quest> <stage> <objective>: the host's game shares the quest and neither game has the stage done
	local kind; kind=$(dc host "stages $1")
	case "$kind" in *"shared=story"*|*"shared=finale"*|*"sQuests = safe"*) ;; *) fail "achievements: set-up: the host shares $1 (the story)" "got '$kind'"; return 1;; esac
	if _f_has host "$1" "$2" || _f_has vm "$1" "$2"; then fail "achievements: set-up: neither game has $1 stage $2 done (a throwaway save from before it)" "host $(_f_done host "$1"), VM $(_f_done vm "$1")"; return 1; fi
	dc host "console setobjectivedisplayed $1 $3 1" >/dev/null; sleep 2   # the host has the quest started
	return 0
}
step_achievements() {
	section "achievements (a caught-up stage's award dropped, a live one the game's; console setstage on the host, throwaway saves)"
	local hook; hook=$(_a_hook)
	if [ "$hook" != "hook=yes" ]; then fail "achievements: set-up: the VM's game has Game.AddAchievement hooked" "net status says '${hook:-nothing}'"; return; fi
	pass "achievements: the VM's game has Game.AddAchievement hooked" "$hook"
	ns vm; [ "$(nsf vm status)" = joined ] || { fail "achievements: set-up: the VM is in the session" "status=$(nsf vm status)"; return; }

	# Live: the VM is in the session when the host does the stage.
	_a_setup "$ACH_LIVE_QUEST" "$ACH_LIVE_STAGE" "$ACH_LIVE_OBJECTIVE" || return
	win_mark ach vm
	dc host "console setstage $ACH_LIVE_QUEST $ACH_LIVE_STAGE" >/dev/null
	if ! wait_for 15 _f_has host "$ACH_LIVE_QUEST" "$ACH_LIVE_STAGE"; then fail "achievements: set-up: the host's console set $ACH_LIVE_QUEST $ACH_LIVE_STAGE" "host done $(_f_done host "$ACH_LIVE_QUEST")"; return; fi
	if ! wait_for "$ACH_SET" _a_log_has vm "QuestSync: set $ACH_LIVE_QUEST .* stage $ACH_LIVE_STAGE "; then
		fail "achievements: set-up: the VM's game set the host's live $ACH_LIVE_QUEST $ACH_LIVE_STAGE" "no set line in $ACH_SET s; VM done $(_f_done vm "$ACH_LIVE_QUEST"); $(grep -E "QuestSync: (set|not setting) $ACH_LIVE_QUEST" "$A_OUT/windelta-ach-vm.txt" | tail -2 | tr '\n' '|')"; return
	fi
	local lf="Achievements: Game.AddAchievement from .*Fragment_Stage_0*${ACH_LIVE_STAGE}_Item_[0-9]+"   # the call from the stage's fragment
	if wait_for "$ACH_AWARD" _a_log_has vm "$lf: a live stage's, the game's call runs"; then
		pass "achievements: a live stage's award ($ACH_LIVE_QUEST $ACH_LIVE_STAGE) is left to the game in the VM's game" "$(grep -oE "$lf: .*" "$A_OUT/windelta-ach-vm.txt" | head -1 | cut -c1-160)"
	else
		fail "achievements: a live stage's award ($ACH_LIVE_QUEST $ACH_LIVE_STAGE) is left to the game in the VM's game" "$(grep -E "Achievements: " "$A_OUT/windelta-ach-vm.txt" | tail -3 | cut -c1-160 | tr '\n' '|')"
	fi
	if grep -qE "$lf: dropped" "$A_OUT/windelta-ach-vm.txt"; then
		fail "achievements: the live stage's award is not dropped" "$(grep -oE "$lf: .*" "$A_OUT/windelta-ach-vm.txt" | head -1 | cut -c1-160)"
	else pass "achievements: the live stage's award is not dropped" "no dropped line for its fragment"; fi

	# Caught up: the VM is out of the session when the host does the stage, and welcomed with it on its reconnect.
	_a_setup "$ACH_CATCHUP_QUEST" "$ACH_CATCHUP_STAGE" "$ACH_CATCHUP_OBJECTIVE" || return
	win_mark ach vm
	local r; r=$(dc vm "net drop")
	case "$r" in dropped) ;; *) fail "achievements: set-up: the VM's game dropped its connection" "net drop said '$r'"; return;; esac
	dc host "console setstage $ACH_CATCHUP_QUEST $ACH_CATCHUP_STAGE" >/dev/null
	if ! wait_for 15 _f_has host "$ACH_CATCHUP_QUEST" "$ACH_CATCHUP_STAGE"; then fail "achievements: set-up: the host's console set $ACH_CATCHUP_QUEST $ACH_CATCHUP_STAGE" "host done $(_f_done host "$ACH_CATCHUP_QUEST")"; return; fi
	if ! wait_for "$ACH_REJOIN" _a_joined; then fail "achievements: set-up: the VM's game rejoined by itself after net drop" "status $(dc vm 'net status' | grep -oE 'status=[a-z]+')"; return; fi
	pass "achievements: the VM's game rejoined by itself after net drop" "within $ACH_REJOIN s"
	if ! wait_for "$ACH_SET" _a_log_has vm "QuestSync: set $ACH_CATCHUP_QUEST .* stage $ACH_CATCHUP_STAGE "; then
		fail "achievements: set-up: the VM's game set the host's $ACH_CATCHUP_QUEST $ACH_CATCHUP_STAGE on its rejoin" "no set line in $ACH_SET s; VM done $(_f_done vm "$ACH_CATCHUP_QUEST"); $(grep -E "QuestSync: (set|not setting) $ACH_CATCHUP_QUEST" "$A_OUT/windelta-ach-vm.txt" | tail -2 | tr '\n' '|')"; return
	fi
	if ! grep -qE "QuestSync: set $ACH_CATCHUP_QUEST .* stage $ACH_CATCHUP_STAGE .*\(catching up\)" "$A_OUT/windelta-ach-vm.txt"; then
		fail "achievements: set-up: the host's $ACH_CATCHUP_QUEST $ACH_CATCHUP_STAGE reached the VM in its welcome, not live" "$(grep -oE "QuestSync: set $ACH_CATCHUP_QUEST .*" "$A_OUT/windelta-ach-vm.txt" | head -1 | cut -c1-160)"; return
	fi
	local cf="Achievements: Game.AddAchievement from .*Fragment_Stage_0*${ACH_CATCHUP_STAGE}_Item_[0-9]+"
	if wait_for "$ACH_AWARD" _a_log_has vm "$cf: dropped"; then
		pass "achievements: a caught-up stage's award ($ACH_CATCHUP_QUEST $ACH_CATCHUP_STAGE) is dropped in the VM's game" "$(grep -oE "$cf: dropped.*" "$A_OUT/windelta-ach-vm.txt" | head -1 | cut -c1-160)"
	else
		fail "achievements: a caught-up stage's award ($ACH_CATCHUP_QUEST $ACH_CATCHUP_STAGE) is dropped in the VM's game" "$(grep -E "Achievements: " "$A_OUT/windelta-ach-vm.txt" | tail -3 | cut -c1-160 | tr '\n' '|')"
	fi
	if grep -qE "$cf: a live stage's, the game's call runs" "$A_OUT/windelta-ach-vm.txt"; then
		fail "achievements: the caught-up stage's award never ran the game's call" "$(grep -oE "$cf: .*" "$A_OUT/windelta-ach-vm.txt" | head -1 | cut -c1-160)"
	else pass "achievements: the caught-up stage's award never ran the game's call" "no 'the game's call runs' line for its fragment"; fi
}

# ---- the add-ons' other faction outcomes, Combat Zone's and the end of The Big Dig (opt-in: smoke.sh --only outcomes) ---------------------
# The quests the host's sQuests = safe shares for their outcomes (tools/questscan.cs ALLOW_OUTCOMES; src/game/QuestFilter.h SAFE and
# SILENT): one stage of each set by one game's console, the other game checked, in both directions, the other game's player beside the
# setter (both coc to SanctuaryExt) or far from them (the setter's coc to OUTCOMES_FAR_CELL). Set-up by console, said so in the report:
# the coc placements, the setter's setobjectivedisplayed for a quest with objectives (a quest the player never started isn't shared), and
# the setstage. A case "set": the other game sets it (its log's "QuestSync: set", the stage done there): the hostile ones, whose faction
# is nowhere near Sanctuary, land at once, never skipped and never offered (the hold while one is near is step_hostile's). A case "own":
# the other game never sets it (its log's "not setting ... (kOwn)", the stage not done there): the Hubologists' sessions are each
# player's own. Quest stages are for good in a save, and these end Bobbi, Harvey, the Hubologists, Far Harbor's town, the Children of
# Atom's door and the Mechanist for the player: run it on throwaway saves (backup.sh before, restore after) with all three add-ons in
# both games; a game with a stage done already is a FAIL naming it.
# A quest the story manager starts from an event the game refuses to start any other way (console setstage on a stopped quest
# included: src/game/QuestStartRules.h; 2026-11-12, run 4: "host done none", the host's log "didn't start here"). The other game
# starts it through the engine's own start first when none of its aliases that must fill comes from the event
# (QuestStartRules::StartsFromFriend), and sets the friend's stage at the next poll, after the start's own start-up stages: a case
# "start" (FFDiamondCity01, CLOC, no start-up stage: its log's "started here through the game's own start for a friend's stage 10;
# the stage is set at the next poll", then "set ... stage 10", the stage done there). Not a run-once change-of-location handler
# (QuestStartRules::NOT_FROM_FRIEND: DLC01Lair_ChangeLocation starts DLC01Lair and stops itself, which in the other game would burn
# its one run away from the Lair): a case "event", the other game tries the stage and its quest won't start there (its log's
# "didn't start here ... the story manager starts it from its event"), the stage not done there, as for a quest whose needed alias
# comes from the event.
# Each case: <setter> <placement: near|far> <quest editor ID> <stage> <objective to show first, - for none> <set|own|event|start>
OUTCOMES_CASES=${OUTCOMES_CASES:-"host near CZ 250 290 set
vm far FFGoodneighbor07 30 10 set
vm near DLC04DialogueHubologists 21 30 own
host far DLC04DialogueHubologists 200 30 set
vm near DLC04DialogueHarvey 200 100 set
host far DLC03MQ00 100 - set
vm far DLC03CoA_DialogueNucleusEntrance 50 - set
vm near DLC01MasterQuest 300 - set
host near DLC01Lair 1400 - set
vm far DLC01Lair_ChangeLocation 0 - event
vm far FFDiamondCity01 10 10 start
host far DLC04MS01SierraAttacks 10 - set"}
OUTCOMES_FAR_CELL=${OUTCOMES_FAR_CELL:-ConcordExt}; OUTCOMES_SET=${OUTCOMES_SET:-30}; OUTCOMES_OWN=${OUTCOMES_OWN:-30}
# Quests not running in a fresh game, whose stage the console's setstage would not set: the setter's game starts them first by dev
# `queststart`, the engine's own start (set-up, said so). 2026-11-11: "the vm's console set DLC01Lair_ChangeLocation 0: vm done none",
# and FFGoodneighbor07 30 never left the VM.
# A start may itself do the stage to set: DLC01Lair_ChangeLocation's 0 is its start-up stage, whose fragment starts DLC01Lair if it
# isn't running and then stops its own quest (DLCRobot.esm; 2026-11-12: queststart answered "is running", 10 s later running=false
# with 0 done). Then the start was the stage's set-up and no setstage follows: the other game is checked for the stage as for any.
OUTCOMES_START=${OUTCOMES_START:-"FFGoodneighbor07 DLC01Lair_ChangeLocation FFDiamondCity01"}
# The stage lines name the quest in quotes, and a name may hold an apostrophe ("[DLC01 - Mechanist's Lair Dungeon Quest]"): '.*', not
# '[^']*' (2026-11-11: DLC01Lair 1400 was set and read as failed).
step_outcomes() {
	section "outcomes (the add-ons' other faction outcomes, Combat Zone's and The Big Dig's end, set by one game's console; console placement)"
	local setter where edid stage obj expect other id line
	while read -r -u 3 setter where edid stage obj expect; do   # fd 3: an ssh inside a dc call (vm_reach) must not eat the list
		[ -z "$setter" ] && continue
		other=$(other_game "$setter")
		id=$(_h_id "$setter" "$edid")
		if [ -z "$id" ] || [ "$id" != "$(_h_id "$other" "$edid")" ]; then fail "outcomes: set-up: both games have $edid (its add-on loaded, the same form ID)" "$setter '$id', $other '$(_h_id "$other" "$edid")'"; continue; fi
		line=$(dc host "stages $id")
		case "$line" in *"sQuests = safe"*|*"shared=side "*|*"shared=story "*) ;; *) fail "outcomes: set-up: the host shares $edid (sQuests = safe, or a side or story quest; a shared story)" "got '$line'"; continue;; esac
		if _f_has host "$id" "$stage" || _f_has vm "$id" "$stage"; then fail "outcomes: set-up: neither game has $edid stage $stage done (a throwaway save from before it)" "host $(_f_done host "$id"), VM $(_f_done vm "$id")"; continue; fi
		enter_both SanctuaryExt 14
		if [ "$where" = far ]; then
			dc "$setter" "console coc $OUTCOMES_FAR_CELL" >/dev/null; sleep 14; wait_ingame "$setter" 30
			if ! wait_for 20 _f_far; then fail "outcomes: set-up: the $setter's coc to $OUTCOMES_FAR_CELL put it far from the $other" "host $(dc host pos); VM $(dc vm pos)"; continue; fi
		elif wait_for 5 _f_far; then fail "outcomes: set-up: both games' coc to SanctuaryExt put them side by side" "host $(dc host pos); VM $(dc vm pos)"; continue; fi
		# The other game's window opens before anything is started: a start may share a stage at once (DLC01Lair_ChangeLocation's 0, if
		# it is its start-up stage), and its line would fall before a window opened after the catch-up wait.
		win_mark outcomes "$other"
		local bystart=0
		case " $OUTCOMES_START " in *" $edid "*)
			if ! _rad_is_running "$setter" "$edid"; then
				# The engine's own start (dev `queststart`): the console's startquest does nothing for a quest the story manager starts on
				# a change of location, as DLC01Lair_ChangeLocation is (2026-11-11: "running=false").
				echo "outcomes: set-up: the $setter's game starts $edid ($id) by dev queststart before setting its $stage: $(dc "$setter" "queststart $id" | cut -c1-160)"
				if ! wait_for 10 _o_started "$setter" "$edid" "$id" "$stage"; then fail "outcomes: set-up: the $setter's game started $edid (dev queststart)" "$(_rad_run "$setter" "$edid"); $setter done $(_f_done "$setter" "$id")"; continue; fi
				bystart=1   # whether the start did the stage itself is read just before the setstage: the game may answer "running" first
			fi;;
		esac
		[ "$obj" != - ] && { dc "$setter" "console setobjectivedisplayed $edid $obj 1" >/dev/null; sleep 2; }   # set-up: the setter has the quest started
		# The catch-up after the join and the coc over in both games first: a live stage waits behind it (2026-11-11: CZ 250 47 s).
		if ! _h_catchup_over "$other" 10 180 || ! _h_catchup_over "$setter" 10 180; then fail "outcomes: set-up: the catch-up is over in both games before $edid $stage" "host $(_h_quests host | cut -c1-120); VM $(_h_quests vm | cut -c1-120)"; continue; fi
		if [ "$bystart" = 1 ] && _f_has "$setter" "$id" "$stage"; then
			echo "outcomes: set-up: the $setter's start of $edid did its $stage itself (a start-up stage; $(_rad_run "$setter" "$edid") after it, as its own script may stop it): no setstage"
		else dc "$setter" "console setstage $edid $stage" >/dev/null; fi
		if ! wait_for 15 _f_has "$setter" "$id" "$stage"; then fail "outcomes: set-up: the $setter's console set $edid $stage" "$setter done $(_f_done "$setter" "$id")"; continue; fi
		if [ "$expect" = own ]; then
			# The other game's own skip ("not setting ... stage 21: ... (kOwn)") is the proof; the stage it reads is printed, since
			# the other game may have another stage done already (2026-11-11: "host done 200", from the case before, with no line
			# for 21 in the 15 s it waited; the line came in later sessions' catch-ups).
			if wait_for "$OUTCOMES_OWN" _o_log_has "$other" "QuestSync: not setting $id '.*' stage $stage: .*\(kOwn\)" && ! _f_has "$other" "$id" "$stage"; then
				pass "outcomes: $edid $stage from the $setter ($where) is each player's own: the $other's game doesn't set it" "$(grep -oE "QuestSync: not setting $id .*" "$A_OUT/windelta-outcomes-$other.txt" | head -1)"
			else
				fail "outcomes: $edid $stage from the $setter ($where) is each player's own: the $other's game doesn't set it" "stage $stage done in the $other's game: $(_f_has "$other" "$id" "$stage" && echo yes || echo no) (done $(_f_done "$other" "$id")); no 'not setting $id ... stage $stage: ... (kOwn)' line in $OUTCOMES_OWN s; $(grep -E "QuestSync: (set|not setting|[0-9]+ stage\(s\) of) $id" "$A_OUT/windelta-outcomes-$other.txt" | tail -2 | tr '\n' '|')"
			fi
			continue
		fi
		if [ "$expect" = event ]; then
			if wait_for "$OUTCOMES_SET" _o_log_has "$other" "QuestSync: $id '.*' didn't start here when a friend's stage $stage was set .*the story manager starts it from its event" && ! _f_has "$other" "$id" "$stage"; then
				pass "outcomes: $edid $stage from the $setter ($where): the $other's game can't start it (a story manager event's quest) and says so" "$(grep -oE "QuestSync: $id .*didn't start here.*" "$A_OUT/windelta-outcomes-$other.txt" | head -1 | cut -c1-220)"
			else
				fail "outcomes: $edid $stage from the $setter ($where): the $other's game can't start it (a story manager event's quest) and says so" "$other done $(_f_done "$other" "$id"); $(grep -E "QuestSync: (set|not setting|offer to follow|$id) " "$A_OUT/windelta-outcomes-$other.txt" | tail -3 | tr '\n' '|')"
			fi
			continue
		fi
		# A start through the game's own start that the other game refused, or that failed, is said once ("it runs once and has run": the
		# save ran it already): a failure names it.
		local refused
		if [ "$expect" = start ]; then
			if wait_for "$OUTCOMES_SET" _o_log_has "$other" "QuestSync: $id '.*' \(the story manager starts it from an event\) started here through the game's own start for a friend's stage $stage; the stage is set at the next poll"; then
				pass "outcomes: $edid $stage from the $setter ($where): the $other's game starts the quest through the game's own start first" "$(grep -oE "QuestSync: $id .*started here through.*" "$A_OUT/windelta-outcomes-$other.txt" | head -1 | cut -c1-220)"
			else
				refused=$(grep -oE "QuestSync: $id '.*' \(the story manager starts it from an event\) couldn't be started here.*" "$(win_read outcomes "$other")" | head -1 | cut -c1-220)
				fail "outcomes: $edid $stage from the $setter ($where): the $other's game starts the quest through the game's own start first" "${refused:-no start line}; $other done $(_f_done "$other" "$id"); $(_rad_run "$other" "$edid"); $(grep -E "QuestSync: (set|not setting|offer to follow|$id) " "$A_OUT/windelta-outcomes-$other.txt" | tail -3 | tr '\n' '|')"
			fi
		fi
		if wait_for "$OUTCOMES_SET" _o_log_has "$other" "QuestSync: set $id '.*' stage $stage \(was " && wait_for 10 _f_has "$other" "$id" "$stage"; then
			pass "outcomes: $edid $stage from the $setter ($where): the $other's game sets it" "$(grep -oE "QuestSync: set $id .*" "$A_OUT/windelta-outcomes-$other.txt" | head -1)"
		else
			refused=$(grep -oE "QuestSync: $id '.*' \(the story manager starts it from an event\) couldn't be started here.*" "$(win_read outcomes "$other")" | head -1 | cut -c1-220)
			fail "outcomes: $edid $stage from the $setter ($where): the $other's game sets it" "${refused:+$refused; }$other done $(_f_done "$other" "$id"); $(grep -E "QuestSync: (set|not setting|offer to follow|$id) " "$A_OUT/windelta-outcomes-$other.txt" | tail -3 | tr '\n' '|')"
		fi
		if grep -qE "QuestSync: (offer to follow|not setting) .*$id .* stage $stage" "$A_OUT/windelta-outcomes-$other.txt"; then
			fail "outcomes: $edid $stage: no offer for it, nothing skipped" "$(grep -oE "QuestSync: (offer to follow|not setting) .*$id .*" "$A_OUT/windelta-outcomes-$other.txt" | head -1)"
		fi
	done 3<<<"$OUTCOMES_CASES"
}
_o_log_has() { grep -qE "$2" "$(win_read outcomes "$1")"; }   # _o_log_has <who> <regex>: the game's log since win_mark has it
_o_started() { _rad_is_running "$1" "$2" || _f_has "$1" "$3" "$4"; }   # _o_started <who> <edid> <id> <stage>: it runs, or its start did the stage

# ---- kickout: a friend's kick-out from a faction waits while any of them is near, then lands as the game's (opt-in: smoke.sh --only kickout)
# The Brotherhood's kick-out, BoSKickOut 20 (Fallout4.esm, SILENT: shared with the host's sQuests = safe and a shared story; no
# objectives, so a friend's stage starts it here): BoSKickOutScript.KickOut puts the player out of the Brotherhood and makes it their
# enemy, fails its quests and, for a Danse who has ever followed the player, dismisses him (src/game/StageRules.h HOSTILE and DISMISSES,
# QuestSync.cpp). Set-up by console, said so in the report: the host coc to KICKOUT_HOST_CELL, the VM coc to KICKOUT_NEAR_CELL (the
# Cambridge police station's exterior, CambridgePDExt, beside Danse, Rhys and Haylen: Fallout4.esm places them in the next cell west,
# enabled from a new game's start, 0005DE4D Danse at (-28768,10297); far from the host; the VM's cell is checked after the coc); the
# host's console sets the stage, once both games' catch-up after the join is over and a member of the Brotherhood is asserted near
# the VM's player (set-up checks).
# Asserted: the VM's log has the hold line naming the Brotherhood and how many of them are near, the stage is not done there
# KICKOUT_HOLD s later (a game that gives no answer then is the rig's, not a pass), and no offer is made and nothing skipped in the
# log read after that wait. Then the VM's player walks away from the station, real movement (walk_away: the virtual pad's stick, at a
# run, until it stands KICKOUT_WALK_OUT u from where it stood: HostileHold::RANGE, 8192 u, and the station's width; KICKOUT_WALK_FOR
# s at most, a walk that doesn't get there is a rig fault), still far from the host, and within KICKOUT_LAND s the stage is done
# there, the log's "waiting for ... to be out of sight". The VM's own companion (whoever the Followers quest's Companion alias
# holds, as `companions` lists it; none is fine) is the same after as before when it is of another faction, and no dismissal of ours
# is logged (the aliases not read, before or after, is the rig's); a Brotherhood one (Danse) would be taken by the kick-out as in the
# game, with the HUD line first (the DISMISSES warning, not exercised here: recruiting Danse is the Brotherhood's story). Quest
# stages are for good in a save, and this one ends the Brotherhood for both: run it on throwaway saves (backup.sh before, restore
# after); a game with the stage done is a FAIL naming it.
KICKOUT_QUEST=${KICKOUT_QUEST:-BoSKickOut}; KICKOUT_STAGE=${KICKOUT_STAGE:-20}
KICKOUT_HOST_CELL=${KICKOUT_HOST_CELL:-ConcordExt}; KICKOUT_NEAR_CELL=${KICKOUT_NEAR_CELL:-CambridgePDExt}
# (Not CambridgePoliceStationExt: no cell of that name exists, and a coc to it does nothing. Until 2026-11-12 the VM stayed at Sanctuary,
# so "none of them near" was always true.)
KICKOUT_HOLD=${KICKOUT_HOLD:-20}; KICKOUT_LAND=${KICKOUT_LAND:-60}; KICKOUT_WALK_OUT=${KICKOUT_WALK_OUT:-11000}; KICKOUT_WALK_FOR=${KICKOUT_WALK_FOR:-300}
KICKOUT_NEAR_WAIT=${KICKOUT_NEAR_WAIT:-30}
KICKOUT_FACTION=${KICKOUT_FACTION:-0005DE41}   # Brotherhood of Steel, the faction the kick-out turns (the VM's log, 2026-11-11)
_k_log_has() { grep -qE "$2" "$(win_read kickout "$1")"; }   # _k_log_has <who> <regex>: the game's log since win_mark has it
# _k_companions <who>: that game's Followers aliases as `companions` lists them, asked up to 3 times. "" = they were not read (no
# answer, or no Followers quest found): two readings that are both "" are no proof that a companion stayed.
_k_companions() { local i c=""; for i in 1 2 3; do c=$(dc "$1" companions | head -1); case "$c" in *dismissDue=*) echo "$c"; return 0;; esac; sleep 1; done; return 1; }
step_kickout() {
	section "kickout (a friend's kick-out from the Brotherhood waits while they are near the VM, and leaves a companion of another faction alone; console placement to the scene and setstage, a real walk away)"
	local id line pos0 x0 y0 done1 log companions0 companions1
	id=$(_h_id host "$KICKOUT_QUEST")
	if [ -z "$id" ] || [ "$id" != "$(_h_id vm "$KICKOUT_QUEST")" ]; then fail "kickout: set-up: both games have $KICKOUT_QUEST (the same form ID)" "host '$id', VM '$(_h_id vm "$KICKOUT_QUEST")'"; return; fi
	line=$(dc host "stages $id")
	case "$line" in *"sQuests = safe"*) ;; *) fail "kickout: set-up: the host shares $KICKOUT_QUEST (sQuests = safe, a shared story)" "got '$line'"; return;; esac
	if _f_has host "$id" "$KICKOUT_STAGE" || _f_has vm "$id" "$KICKOUT_STAGE"; then fail "kickout: set-up: neither game has $KICKOUT_QUEST stage $KICKOUT_STAGE done (a throwaway save from before it)" "host $(_f_done host "$id"), VM $(_f_done vm "$id")"; return; fi
	enter_both SanctuaryExt 14
	dc host "console coc $KICKOUT_HOST_CELL" >/dev/null; sleep 14; wait_ingame host 30
	pos0=$(dc vm pos)
	dc vm "console coc $KICKOUT_NEAR_CELL" >/dev/null; sleep 20; wait_ingame vm 30
	if ! _coc_landed vm "$KICKOUT_NEAR_CELL" "$pos0"; then fail "kickout: set-up: the VM's coc to $KICKOUT_NEAR_CELL put it in that cell" "VM $(dc vm status | cut -c1-160); its cell's editor ID '$(_cell_edid vm)'"; return; fi
	echo "kickout: set-up: placed by console coc: the host at $KICKOUT_HOST_CELL ($(dc host pos | cut -c1-60)), the VM at $KICKOUT_NEAR_CELL (its cell '$(_cell_edid vm)', $(dc vm pos | cut -c1-60))"
	if ! wait_for 20 _f_far; then fail "kickout: set-up: the host in $KICKOUT_HOST_CELL and the VM in $KICKOUT_NEAR_CELL are far apart" "host $(dc host status); VM $(dc vm status)"; return; fi
	sleep 5   # the station's Brotherhood loaded around the VM
	# As hostile: the catch-up over in both games, and a member of the Brotherhood near the VM's player, asserted first.
	if ! _h_catchup_over vm || ! _h_catchup_over host; then fail "kickout: set-up: the join's catch-up is over in both games before the stage" "host $(_h_quests host | cut -c1-120); VM $(_h_quests vm | cut -c1-120)"; return; fi
	# The station's Brotherhood may come into the VM's high process some seconds after the coc (2026-11-12: none read 25 s after it):
	# up to KICKOUT_NEAR_WAIT s more for one. None then: the world has none there (a coc-from-menu game may not have placed Danse's
	# squad yet), a set-up the step can't make, and the FAIL says so.
	wait_for "$KICKOUT_NEAR_WAIT" _h_near_any vm "$KICKOUT_FACTION"
	_h_assert_near kickout vm "$KICKOUT_FACTION" || return
	companions0=$(_k_companions vm)
	if [ -z "$companions0" ]; then rig_broken game "kickout: set-up: the VM's companions are read before the kick-out" "the VM's game gave no reading of its Followers aliases, asked 3 times: '$(dc vm companions | cut -c1-120)'"; return; fi
	pos0=$(dc vm pos); x0=$(field "$pos0" x); y0=$(field "$pos0" y)
	if ! is_num "$x0" || ! is_num "$y0"; then rig_broken game "kickout: set-up: where the VM stands at the station is read" "the VM's game gave no position: '$(cut -c1-120 <<<"$pos0")'"; return; fi
	win_mark kickout vm
	dc host "console setstage $KICKOUT_QUEST $KICKOUT_STAGE" >/dev/null
	if ! wait_for 15 _f_has host "$id" "$KICKOUT_STAGE"; then fail "kickout: set-up: the host's console set $KICKOUT_QUEST $KICKOUT_STAGE" "host done $(_f_done host "$id")"; return; fi
	if wait_for 20 _k_log_has vm "QuestSync: $id .* stage $KICKOUT_STAGE turns [0-9A-F]{8} '[^']*' on the player, and [0-9]+ of them (is|are) near"; then
		pass "kickout: the far VM's game holds the stage while the Brotherhood is near" "$(grep -oE "QuestSync: $id .* stage $KICKOUT_STAGE turns [^;]*" "$A_OUT/windelta-kickout-vm.txt" | head -1)"
	else
		fail "kickout: the far VM's game holds the stage while the Brotherhood is near" "no hold line in the VM's log; VM done $(_f_done vm "$id"); $(grep -E "QuestSync: (set|not setting|offer) " "$A_OUT/windelta-kickout-vm.txt" | tail -3 | tr '\n' '|')"; return
	fi
	sleep "$KICKOUT_HOLD"
	# A game that gives no answer has not shown the stage unset, and a log that could not be read has not shown nothing skipped.
	done1=$(_f_done_sure vm "$id")
	if [ -z "$done1" ]; then rig_broken game "kickout: the stage is not set while they are near ($KICKOUT_HOLD s)" "the VM's game gave no answer to 'stages $id', asked 3 times: '$(dc vm "stages $id" | cut -c1-120)'"; return; fi
	case ",$done1," in
	*",$KICKOUT_STAGE,"*) fail "kickout: the stage is not set while they are near ($KICKOUT_HOLD s)" "done: $done1";;
	*) pass "kickout: the stage is not set while they are near ($KICKOUT_HOLD s)" "VM done $done1";;
	esac
	log=$(win_read kickout vm)   # read again after the wait: what the game logged during it is what this check is about
	if ! grep -qE "QuestSync: $id .* stage $KICKOUT_STAGE turns " "$log"; then rig_broken game "kickout: no offer for it, nothing skipped" "the VM's log could not be read after the wait: the hold line it had before is not in it"; return; fi
	if grep -qE "QuestSync: (offer to follow|not setting) .*$id .* stage $KICKOUT_STAGE" "$log"; then
		fail "kickout: no offer for it, nothing skipped" "$(grep -oE "QuestSync: (offer to follow|not setting) .*$id .*" "$log" | head -1)"
	else pass "kickout: no offer for it, nothing skipped" "the VM's log, read after the $KICKOUT_HOLD s, has neither"; fi
	# Away from the station on foot (real movement, no console placement), still far from the host: the Brotherhood out of sight, the
	# kick-out lands.
	walk_away vm "kickout: set-up: the VM walks out of the Brotherhood's sight ($KICKOUT_WALK_OUT u from where it stood at the station)" "$x0" "$y0" "$KICKOUT_WALK_OUT" "$KICKOUT_WALK_FOR" || { pads_off; return; }
	if ! wait_for 20 _f_far; then fail "kickout: set-up: the VM's walk away from the station left it far from the host" "host $(dc host pos); VM $(dc vm pos)"; return; fi
	if wait_for "$KICKOUT_LAND" _k_log_has vm "QuestSync: set $id .* stage $KICKOUT_STAGE .*waiting for [0-9A-F]{8} '[^']*' to be out of sight" \
		&& wait_for 10 _f_has vm "$id" "$KICKOUT_STAGE"; then
		pass "kickout: with none of the Brotherhood near (the VM walked $WALK_OUT u away), the VM's game sets it" "$(grep -oE "QuestSync: set $id .*" "$A_OUT/windelta-kickout-vm.txt" | head -1); legs (turn:end):$WALK_LEGS"
	else
		fail "kickout: with none of the Brotherhood near (the VM walked $WALK_OUT u away), the VM's game sets it" "VM done $(_f_done vm "$id"); $(grep -E "QuestSync: (set|not setting|$id) " "$A_OUT/windelta-kickout-vm.txt" | tail -3 | tr '\n' '|')"; return
	fi
	sleep 5
	# Aliases that could not be read, before or after, compare equal and prove nothing: that is the rig's. The log is read again
	# after the wait, for a dismissal made during it.
	companions1=$(_k_companions vm)
	if [ -z "$companions1" ]; then rig_broken game "kickout: the VM's own companion (of another faction) is as before" "the VM's game gave no reading of its Followers aliases after the kick-out, asked 3 times: '$(dc vm companions | cut -c1-120)' (before: '$companions0')"; return; fi
	log=$(win_read kickout vm)
	if ! grep -qE "QuestSync: set $id .* stage $KICKOUT_STAGE " "$log"; then rig_broken game "kickout: the VM's own companion (of another faction) is as before" "the VM's log could not be read after the kick-out: the line that set the stage is not in it"; return; fi
	if [ "$companions0" = "$companions1" ] && ! grep -qE "Companions: .*dismiss" "$log"; then
		pass "kickout: the VM's own companion (of another faction) is as before" "$companions1"
	else
		fail "kickout: the VM's own companion (of another faction) is as before" "before '$companions0', after '$companions1'; $(grep -E "Companions: " "$log" | tail -2 | tr '\n' '|')"
	fi
}

# ---- a shared quest's created creatures, paired across games (src/game/QuestActors.h) ------------------------------------------------------
# A quest the session shares (with the host's sQuests = safe: SAFE's Gun Run, SILENT's objective-less ones) creates its creatures in each
# game's run; the game that shares one first names its alias, and the other game pairs its own creature from that alias with it (mirrored,
# dying when it dies there). Two scenes, each reached by console placement (said in the report; the walk into the scene is real movement):
# 1. Gun Run (DN083_Barney, SAFE: running from a new game's start, its mirelurks made at Salem). Both players are placed QA_GR_APPROACH
#    from the first mirelurk, the host walks up to it (the ambush: stage 5), both shoot (the host's real clicks, the VM's game's `click`, aimed
#    at each creature's origin) for a while, and the host's game injects the damage on those still alive (the rig rules allow the host's
#    injected fire; what is checked is that the deaths reach the other game, not the aim).
#    Asserted: stage 5 in both games; a pairing (`questActors: paired=`, expected in the VM's game: the host's ambush goes first); each
#    mirelurk, by its alias, alive or dead alike in both games with the same health; once the three are dead, stage 10 in both; and no
#    storm of the server's same-alias refusals (host log window).
# 2. A SILENT quest whose creatures are made as it starts (QA_QUEST, default DN119Fight: Rotten Landfill's molerats, started as the
#    location loads). The host walks to QA_TRIGGER_SPOT ("x y z"; by default where QA_SCENE_REF stands: Rotten Landfill's map marker), the VM
#    placed 100 u behind it. (A forced alias's `aliases` data is a handle, not a form ID: the scene is named by a reference instead.)
#    Asserted: the quest started in both games (a stage done) and how the VM's started (the session's "starting the quest here" line, or
#    its own story manager: a WARN, as both players stood there); a pairing; each creature, by its alias, dead or alive alike after the kill.
# Needs the host's sQuests = safe (`rules` quests=safe): a FAIL saying so otherwise. Once a game's run is over (a soak's later rounds),
# there is nothing left to pair: a PASS when both games agree (a WARN otherwise), but only once this run has proved that scene's pairing
# (QA_GR_PAIRED, QA_PAIRED: smoke.sh runs every step in one shell, so they last across rounds). A run that starts with a scene over
# fails it: use a save from before Gun Run or the landfill fight.
QA_GR_QUEST=001243D6; QA_GR_RE=${QA_GR_RE:-Mirelurk}; QA_GR_APPROACH=${QA_GR_APPROACH:--900 0}
QA_QUEST=${QA_QUEST:-000BACBC}; QA_RE=${QA_RE:-^Molerat0[0-9]$}; QA_SCENE_REF=${QA_SCENE_REF:-0002C4D4}; QA_CELL=${QA_CELL:-}
QA_TRIGGER_SPOT=${QA_TRIGGER_SPOT:-}; QA_APPROACH=${QA_APPROACH:-1500 0}
# QA_FIRST=vm: the landfill's VM placed there alone first, and the host only once the VM's game has started the quest and made its
# creatures (the joiner's game first, on purpose: the 0.14.0-rc1 soak's failing round, where both stood there and the VM's started it)
QA_FIRST=${QA_FIRST:-}
QA_GUN=00004822; QA_AMMO=0001F276   # a 10mm pistol and its rounds, given to both players (console) for the fight
QA_GR_PAIRED=${QA_GR_PAIRED:-}; QA_PAIRED=${QA_PAIRED:-}   # set once a scene's pairing passes in this run
# _qa_aliases <who> <quest> <name regex> [fill type]: "<alias id> <ref>" of the quest's ref aliases of that fill type (4, created; any
# with none) whose name matches and that hold a reference now (`aliases`: " | Ref 26 StartMirelurk01 flags=.. fillType=4 .. here=FF001234")
_qa_aliases() {
	dc "$1" "aliases $2" | tr '|' '\n' | awk -v re="$3" -v ft="$4" '
		$1 == "Ref" && $3 ~ re && (ft == "" || index($0, "fillType=" ft " ") > 0) && match($0, /here=[0-9A-F]+/) {
			h = substr($0, RSTART + 5, RLENGTH - 5); if (h != "00000000") print $2, h }'
}
_qa_done() { dc "$1" "stages $2" | sed -nE 's/.* done\([0-9]+\)=([0-9,]+|none).*/\1/p'; }
_qa_has() { case ",$(_qa_done "$1" "$2")," in *",$3,"*) return 0;; esac; return 1; }
_qa_started() { local d; d=$(_qa_done "$1" "$2"); [ -n "$d" ] && [ "$d" != none ]; }
_qa_paired() { field "$(dc "$1" 'net status' | grep -oE 'questActors: [^|;]*')" "$2"; }   # _qa_paired <who> <field: paired|pairs|keyed|twinsHeld>
_qa_any_paired() { local h v; h=$(_qa_paired host paired); v=$(_qa_paired vm paired); { is_int "$v" && [ "$v" -ge 1 ]; } || { is_int "$h" && [ "$h" -ge 1 ]; }; }
_qa_all_dead() {  # _qa_all_dead <who> <"alias ref" lines>
	local a r; while read -r a r; do [ -z "$r" ] && continue; [ "$(field "$(dc "$1" "combat $r")" dead)" = true ] || return 1; done <<<"$2"; return 0
}
_qa_arm() {  # _qa_arm <who>: a pistol and rounds, drawn
	dc "$1" "console player.additem $QA_GUN 1" >/dev/null; dc "$1" "console player.additem $QA_AMMO 300" >/dev/null
	dc "$1" "console player.equipitem $QA_GUN" >/dev/null; dc "$1" "draw on" >/dev/null; sleep 1
}
# _qa_first_alive <who> <"alias ref" lines>: the first of them alive in that game
_qa_first_alive() { local a r; while read -r a r; do [ -n "$r" ] && [ "$(field "$(dc "$1" "combat $r")" dead)" = false ] && { echo "$r"; return; }; done <<<"$2"; }
# _qa_shoot <host lines> <vm lines> <seconds>: both players shoot at the first living creature of their own game's list (by alias), the host
# with real clicks on its window, the VM's game with its `click`, until every one of the host's is dead or the time is up
_qa_shoot() {
	local end=$((SECONDS + $3)) t p a r
	pads_off; _qa_arm host; _qa_arm vm; host focus >/dev/null; QA_INJECTED=""
	while [ $SECONDS -lt "$end" ] && ! _qa_all_dead host "$1"; do
		# aimed at the creature's origin (a mirelurk or a molerat is low: turnto's own pitch, a standing body's chest, shoots over it)
		t=$(_qa_first_alive host "$1"); p=$([ -n "$t" ] && pitch_to host "$t"); is_int "$p" || p=10
		# a refused click (host_input: another window covers the host's game) ends the shooting: it is recorded as the rig's failure, and
		# the creatures left alive are killed by the injected damage below, so the deaths' sync is still checked
		[ -n "$t" ] && { host "turnto $t 300 $p" >/dev/null; sleep 0.4; host_input "quest actors: the host's click reaches its game" -Click -HoldMs 400 || break; }
		t=$(_qa_first_alive vm "$2"); p=$([ -n "$t" ] && pitch_to vm "$t"); is_int "$p" || p=10
		[ -n "$t" ] && { vm "turnto $t 300 $p" >/dev/null; sleep 0.4; vm "click 150" >/dev/null; }
		sleep 1
	done
	# Those still alive after the shots: the host's game injects the damage (as the downed step does; the rig rules allow the host's
	# injected fire): what is checked is that each death reaches the other game, not the aim. Only on a creature the host's game runs:
	# a console damageav is no hit, so on one the VM's game runs (the host's own paired with the VM's, mirrored: the VM's game shared
	# its creatures first) it killed the host's mirror alone, and the creature lived on in the VM's game (0.14.0-rc1 soak, alias 24).
	# Those are left as the shots left them (QA_LEFT), and compared all the same.
	QA_LEFT=""
	if ! _qa_all_dead host "$1"; then
		local hid o; hid=$(dcf host 'net status' id)
		while read -r a r; do
			[ -n "$r" ] && [ "$(field "$(host "combat $r")" dead)" = false ] || continue
			o=$(field "$(host "ghosts $r")" owner)
			if is_int "$o" && [ "$o" -ne 0 ] && [ "$o" != "$hid" ]; then QA_LEFT="$QA_LEFT $a"; continue; fi
			host "console $r.damageav health 2000" >/dev/null; QA_INJECTED="$QA_INJECTED $a"
		done <<<"$1"
		[ -n "$QA_INJECTED" ] && wait_for 10 _qa_all_dead host "$1"
	fi
	_qa_all_dead host "$1"
}
# _qa_diff <host lines> <vm lines>: each alias's creature in both games, alive or dead alike, the same health while alive. Sets QA_N
# (compared), QA_BAD (differ), QA_ODD (how) and QA_GONE (gone from both); true when some were compared and none differ.
_qa_diff() {
	local a r rv h v
	QA_N=0; QA_BAD=0; QA_ODD=""; QA_GONE=0
	while read -r a r; do
		[ -z "$r" ] && continue; rv=$(awk -v a="$a" '$1 == a { print $2 }' <<<"$2")
		if [ -z "$rv" ]; then QA_ODD="$QA_ODD alias $a: none in the VM's game;"; QA_BAD=$((QA_BAD + 1)); continue; fi
		h=$(actor_hp host "$r"); v=$(actor_hp vm "$rv")
		if [ -z "$h" ] && [ -z "$v" ]; then QA_GONE=$((QA_GONE + 1)); continue; fi   # gone from both games (its quest stopped and let it go)
		QA_N=$((QA_N + 1))
		if [ "$(field "$h" dead)" != "$(field "$v" dead)" ]; then QA_BAD=$((QA_BAD + 1)); QA_ODD="$QA_ODD alias $a $r/$rv dead=$(field "$h" dead)/$(field "$v" dead);"
		elif [ "$(field "$h" dead)" = false ] && ! awk -v a="$(field "$h" hp)" -v b="$(field "$v" hp)" -v t="$T_HP_DIFF" 'BEGIN{d=a-b; if(d<0)d=-d; exit !(a!="" && b!="" && d<=t)}'; then
			QA_BAD=$((QA_BAD + 1)); QA_ODD="$QA_ODD alias $a $r/$rv hp=$(field "$h" hp)/$(field "$v" hp);"
		fi
	done <<<"$1"
	[ $QA_N -gt 0 ] && [ $QA_BAD -eq 0 ]
}
# _qa_compare <label> <host lines> <vm lines> [the last _qa_diff's]: the verdict on _qa_diff, run now unless "last" is given
_qa_compare() {
	[ "$4" = last ] || _qa_diff "$2" "$3"
	if [ $QA_N -eq 0 ]; then fail "$1" "no creature compared ($QA_GONE gone from both games):$QA_ODD"
	elif [ $QA_BAD -eq 0 ]; then pass "$1" "$QA_N by alias, alike in both games$([ $QA_GONE -gt 0 ] && echo ", $QA_GONE gone from both")"
	else fail "$1" "$QA_BAD of $QA_N differ:$QA_ODD"; fi
}
# _qa_place_near <x> <y> <z> <dx> <dy> [vm]: both players placed at that point plus the offset, 400 u up (a slope at the spot must not put them under
# the ground; god mode takes the fall) (console: to reach the scene), the VM 100 u
# further back on the same line, both facing the point; with "vm", the VM alone
_qa_place_near() {
	local hx hy vx vy ang hz
	hx=$(awk -v a="$1" -v d="$4" 'BEGIN{printf "%d", a + d}'); hy=$(awk -v a="$2" -v d="$5" 'BEGIN{printf "%d", a + d}')
	vx=$(awk -v a="$hx" -v d="$4" -v e="$5" 'BEGIN{l=sqrt(d*d+e*e); if(l==0)l=1; printf "%d", a + d / l * 100}')
	vy=$(awk -v a="$hy" -v d="$4" -v e="$5" 'BEGIN{l=sqrt(d*d+e*e); if(l==0)l=1; printf "%d", a + e / l * 100}')
	ang=$(awk -v d="$4" -v e="$5" 'BEGIN{a=atan2(-d, -e) * 57.29578; if (a < 0) a += 360; printf "%d", a}')
	hz=$(awk -v a="$3" 'BEGIN{printf "%d", a + 400}')
	# A far jump loads cells, and the console lines after the first setpos are lost while it does (2026-11-07: x set, y not): placed
	# again once both games are in, up to three times, until each stands within 300 u (x and y) of its spot
	local try d
	for try in 1 2 3; do
		[ "$6" = vm ] || place host "$hx" "$hy" "$hz" "$ang"
		place vm "$vx" "$vy" "$hz" "$ang"
		sleep 8; [ "$6" = vm ] || wait_ingame host 30; wait_ingame vm 30; sleep 2
		if [ "$6" = vm ]; then d=$(awk -v b="$(_q_moved "x=$vx y=$vy" "$(dc vm pos)")" 'BEGIN{print b == "?" ? 99999 : b}')
		else d=$(awk -v a="$(_q_moved "x=$hx y=$hy" "$(dc host pos)")" -v b="$(_q_moved "x=$vx y=$vy" "$(dc vm pos)")" 'BEGIN{print (a == "?" || b == "?") ? 99999 : (a > b ? a : b)}'); fi
		[ "$d" -le 300 ] 2>/dev/null && break
	done
	echo "placed (try $try): host $(dc host pos | cut -c1-60) vm $(dc vm pos | cut -c1-60)"
	sleep 3
}
# _qa_have_both: both games' creatures read from the quest's aliases into the caller's hl and vl
_qa_have_both() { hl=$(_qa_aliases host $QA_QUEST "$QA_RE" 4); vl=$(_qa_aliases vm $QA_QUEST "$QA_RE" 4); [ -n "$hl" ] && [ -n "$vl" ]; }
# _qa_have_vm: the VM's game has the creatures in its aliases (vl)
_qa_have_vm() { vl=$(_qa_aliases vm $QA_QUEST "$QA_RE" 4); [ -n "$vl" ]; }
# _qa_steady: both games' creatures read again from the quest's aliases into QA_H and QA_V, the same in two reads QA_STEADY s apart. DN119Fight
# refills an alias as its molerat dies (its waves), each game on its own, one up to ~3 s after the other (16:53:35 and 16:53:38 in the
# 0.16.1-rc1 soak's run 2): reads that hold still for longer hold one generation in both games.
QA_STEADY=${QA_STEADY:-4}
# QA_LAND: how long the compare after the shots waits for a death or a hit still landing in the other game (see _qa_silent)
QA_LAND=${QA_LAND:-8}
_qa_steady() {
	local h v; h=$(_qa_aliases host $QA_QUEST "$QA_RE" 4); v=$(_qa_aliases vm $QA_QUEST "$QA_RE" 4); sleep "$QA_STEADY"
	QA_H=$(_qa_aliases host $QA_QUEST "$QA_RE" 4); QA_V=$(_qa_aliases vm $QA_QUEST "$QA_RE" 4)
	[ -n "$QA_H" ] && [ -n "$QA_V" ] && [ "$h" = "$QA_H" ] && [ "$v" = "$QA_V" ]
}
# _qa_refilled <first host lines> <first VM lines>: the aliases whose creature in either game is another than at the first read (QA_H, QA_V)
_qa_refilled() {
	local a r out=""
	while read -r a r; do
		[ -z "$a" ] && continue
		[ "$(awk -v a="$a" '$1 == a { print $2 }' <<<"$1")" = "$r" ] && [ "$(awk -v a="$a" '$1 == a { print $2 }' <<<"$2")" = "$(awk -v a="$a" '$1 == a { print $2 }' <<<"$QA_V")" ] || out="$out $a"
	done <<<"$QA_H"
	echo "${out# }"
}
# _qa_paired_since: a pairing made since the caller read p0 (VM) and h0 (host): the pairs counts went up (a creature dead and let go is no
# longer paired=)
_qa_paired_since() { local a b; a=$(_qa_paired vm pairs); b=$(_qa_paired host pairs); { is_int "$a" && is_int "$p0" && [ "$a" -gt "$p0" ]; } || { is_int "$b" && is_int "$h0" && [ "$b" -gt "$h0" ]; }; }
# _qa_unproven <label> <scene>: a scene over before this run proved its pairing: nothing about pairing was checked, so a FAIL
_qa_unproven() { fail "$1: over before this run proved a pairing" "nothing left to pair, so the pairing was never checked: use a save from before $2"; }
# _qa_over <label> <quest> <last stage> <proved>: the run is over in the host's game already (a soak's later rounds): both games agree or
# not; only once this run proved the pairing (<proved> set)
_qa_over() {
	_qa_has host "$2" "$3" || return 1
	[ -n "$4" ] || { _qa_unproven "$1" "Gun Run"; return 0; }
	if [ "$(_qa_done vm "$2")" = "$(_qa_done host "$2")" ]; then pass "$1: its run is over, nothing left to pair" "host and VM done $(_qa_done host "$2")"
	else warn "$1: its run is over, nothing left to pair" "host done $(_qa_done host "$2"), VM $(_qa_done vm "$2")"; fi
	return 0
}
_qa_gunrun() {
	local hl vl r x y z f n p0 h0 a
	_qa_over "quest actors (Gun Run)" $QA_GR_QUEST 10 "$QA_GR_PAIRED" && return
	hl=$(_qa_aliases host $QA_GR_QUEST "$QA_GR_RE" 4)
	r=$(head -1 <<<"$hl" | awk '{print $2}')
	if [ -z "$r" ]; then fail "quest actors (Gun Run): set-up: the host's game holds its mirelurks" "$(dc host "stages $QA_GR_QUEST" | cut -c1-160); aliases: $(dc host "aliases $QA_GR_QUEST" | cut -c1-200)"; return; fi
	f=$(dc host "refinfo $r"); x=$(field "$f" x); y=$(field "$f" y); z=$(field "$f" z)
	if ! is_num "$x" || ! is_num "$y" || ! is_num "$z"; then fail "quest actors (Gun Run): set-up: where the first mirelurk is" "refinfo $r: '$(cut -c1-160 <<<"$f")'"; return; fi
	_qa_place_near "$x" "$y" "$z" $QA_GR_APPROACH
	win_mark qa host; p0=$(_qa_paired vm pairs); h0=$(_qa_paired host pairs)
	walk host "quest actors (Gun Run): set-up: the host walks up to the first mirelurk" 0.8 250 "$r"
	if wait_for 20 _qa_has host $QA_GR_QUEST 5 && wait_for 20 _qa_has vm $QA_GR_QUEST 5; then pass "quest actors (Gun Run): the ambush (stage 5) in both games" "host done $(_qa_done host $QA_GR_QUEST), VM $(_qa_done vm $QA_GR_QUEST)"
	else fail "quest actors (Gun Run): the ambush (stage 5) in both games" "host done $(_qa_done host $QA_GR_QUEST), VM $(_qa_done vm $QA_GR_QUEST)"; fi
	if wait_for 15 _qa_any_paired; then QA_GR_PAIRED=1; pass "quest actors (Gun Run): a mirelurk paired" "VM paired=$(_qa_paired vm paired) pairs $p0 -> $(_qa_paired vm pairs) keyed=$(_qa_paired vm keyed); host paired=$(_qa_paired host paired) pairs $h0 -> $(_qa_paired host pairs) keyed=$(_qa_paired host keyed)"
	else fail "quest actors (Gun Run): a mirelurk paired" "VM: $(dc vm 'net status' | grep -oE 'questActors: [^|;]*'); host: $(dc host 'net status' | grep -oE 'questActors: [^|;]*')"; fi
	vl=$(_qa_aliases vm $QA_GR_QUEST "$QA_GR_RE" 4)
	sleep 3; _qa_compare "quest actors (Gun Run): each mirelurk alike in both games, before the shots" "$hl" "$vl"
	if _qa_shoot "$hl" "$vl" 60; then pass "quest actors (Gun Run): the host's mirelurks die (both players' shots for 60 s, then the host's injected damage)" "$(wc -l <<<"$hl") dead; injected on alias(es):${QA_INJECTED:- none}${QA_LEFT:+; left alone (run by the VM game):$QA_LEFT}"
	else fail "quest actors (Gun Run): the host's mirelurks die (both players' shots for 60 s, then the host's injected damage)" "$(while read -r a r; do echo -n "$a $(actor_hp host "$r"); "; done <<<"$hl")"; fi
	sleep 4; _qa_compare "quest actors (Gun Run): each mirelurk alike in both games, after the shots" "$hl" "$vl"
	if wait_for 30 _qa_has host $QA_GR_QUEST 10 && wait_for 10 _qa_has vm $QA_GR_QUEST 10; then pass "quest actors (Gun Run): all three counted dead in both games (stage 10)" "host done $(_qa_done host $QA_GR_QUEST), VM $(_qa_done vm $QA_GR_QUEST)"
	else fail "quest actors (Gun Run): all three counted dead in both games (stage 10)" "host done $(_qa_done host $QA_GR_QUEST), VM $(_qa_done vm $QA_GR_QUEST)"; fi
	f=$(win_read qa host); n=$(grep -c "is the same quest alias's" "$f" 2>/dev/null)
	expect_le "quest actors (Gun Run): the server's same-alias refusals (no storm)" "${n:-0}" 3
}
_qa_silent() {
	local k hl vl r x y z f p0 h0 who
	[ -n "$QA_CELL" ] && enter_both "$QA_CELL" 16
	k=$(dc host "stages $QA_QUEST")
	case "$k" in *"silent (shared"*) ;; *) fail "quest actors ($QA_QUEST): set-up: the host's game shares it as a silent quest" "got '$(cut -c1-200 <<<"$k")'"; return;; esac
	# Started before and not running now: its run is over
	f=$(field "$k" flags)
	if _qa_started host $QA_QUEST && [[ $f =~ ^[0-9A-F]{4}$ ]] && [ $((0x$f & 1)) -eq 0 ]; then
		[ -n "$QA_PAIRED" ] || { _qa_unproven "quest actors ($QA_QUEST)" "the landfill fight"; return; }
		if [ "$(_qa_done vm $QA_QUEST)" = "$(_qa_done host $QA_QUEST)" ]; then pass "quest actors ($QA_QUEST): its run is over, nothing left to pair" "host and VM done $(_qa_done host $QA_QUEST)"
		else warn "quest actors ($QA_QUEST): its run is over, nothing left to pair" "host done $(_qa_done host $QA_QUEST), VM $(_qa_done vm $QA_QUEST)"; fi
		return
	fi
	# Still running, its creatures all dead (a soak's later round: a quest may run on after its fight): nothing left to pair either, and
	# each creature must be dead in both games
	hl=$(_qa_aliases host $QA_QUEST "$QA_RE" 4)
	if _qa_started host $QA_QUEST && [ -n "$hl" ] && _qa_all_dead host "$hl"; then
		[ -n "$QA_PAIRED" ] || { _qa_unproven "quest actors ($QA_QUEST)" "the landfill fight"; return; }
		_qa_compare "quest actors ($QA_QUEST): its fight is over, each creature dead in both games" "$hl" "$(_qa_aliases vm $QA_QUEST "$QA_RE" 4)"
		return
	fi
	if [ -n "$QA_TRIGGER_SPOT" ]; then read -r x y z <<<"$QA_TRIGGER_SPOT"
	else
		f=$(dc host "refinfo $QA_SCENE_REF"); x=$(field "$f" x); y=$(field "$f" y); z=$(field "$f" z)
	fi
	if ! is_num "$x" || ! is_num "$y" || ! is_num "$z"; then fail "quest actors ($QA_QUEST): set-up: where its scene is" "no QA_TRIGGER_SPOT, and refinfo $QA_SCENE_REF: '$(cut -c1-120 <<<"$f")'"; return; fi
	# The fight can start as the place loads, before any walk, and be over in seconds (DN119Fight's scavenger and molerats): the counts
	# are read before the placement, and the creatures as soon as each game has them
	win_mark qa2 vm; p0=$(_qa_paired vm pairs); h0=$(_qa_paired host pairs)
	if [ "$QA_FIRST" = vm ]; then
		_qa_place_near "$x" "$y" "$z" $QA_APPROACH vm
		if wait_for 20 _qa_started vm $QA_QUEST; then pass "quest actors ($QA_QUEST): set-up: the VM's game started it first (QA_FIRST=vm)" "VM done $(_qa_done vm $QA_QUEST)"
		else warn "quest actors ($QA_QUEST): set-up: the VM's game started it first (QA_FIRST=vm)" "not started in 20 s: the host is placed all the same"; fi
		wait_for 10 _qa_have_vm; sleep 3   # its creatures made, and shared before the host arrives
	fi
	_qa_place_near "$x" "$y" "$z" $QA_APPROACH
	_qa_have_both
	walk host "quest actors ($QA_QUEST): set-up: the host walks to its scene" 0.8 800 "$x" "$y"   # the scene is a landfill: its heaps stop a walk short of the marker
	if wait_for 10 _qa_started host $QA_QUEST && wait_for 10 _qa_started vm $QA_QUEST; then pass "quest actors ($QA_QUEST): started in both games" "host done $(_qa_done host $QA_QUEST), VM $(_qa_done vm $QA_QUEST)"
	else fail "quest actors ($QA_QUEST): started in both games" "host done $(_qa_done host $QA_QUEST), VM $(_qa_done vm $QA_QUEST)"; return; fi
	[ -n "$hl" ] && [ -n "$vl" ] || wait_for 10 _qa_have_both
	f=$(win_read qa2 vm)
	if grep -q "QuestSync: set $QA_QUEST .*(starting the quest here for the session)" "$f" 2>/dev/null; then pass "quest actors ($QA_QUEST): the VM's game started it for the session" "$(grep -m1 "QuestSync: set $QA_QUEST .*starting the quest here" "$f" | cut -c1-200)"
	else warn "quest actors ($QA_QUEST): the VM's game started it for the session" "no such line: its own game started it (both players stood at the scene)"; fi
	if [ -z "$hl" ] || [ -z "$vl" ]; then fail "quest actors ($QA_QUEST): set-up: both games' creatures read from its aliases" "host '$hl' VM '$vl' (done: host $(_qa_done host $QA_QUEST), VM $(_qa_done vm $QA_QUEST))"; return; fi
	# A pairing made at any time since the placement
	if wait_for 15 _qa_paired_since; then QA_PAIRED=1; pass "quest actors ($QA_QUEST): a creature paired" "VM pairs $p0 -> $(_qa_paired vm pairs) paired=$(_qa_paired vm paired) keyed=$(_qa_paired vm keyed); host pairs $h0 -> $(_qa_paired host pairs) paired=$(_qa_paired host paired) keyed=$(_qa_paired host keyed)"
	else fail "quest actors ($QA_QUEST): a creature paired" "VM: $(dc vm 'net status' | grep -oE 'questActors: [^|;]*' | cut -c1-120); host: $(dc host 'net status' | grep -oE 'questActors: [^|;]*' | cut -c1-120)"; fi
	_qa_shoot "$hl" "$vl" 45 && who="all dead" || who="not all dead"; who="$who; injected on alias(es):${QA_INJECTED:- none}${QA_LEFT:+; left alone (run by the VM game):$QA_LEFT}"
	# Compared are the aliases' creatures as they are after the shots, read again: the first read can be stale in one game and not the
	# other. 0.15.0-rc1 soak run 2, alias 22: the host's first creature (dead, the VM's paired one killed) against the VM's next one
	# (alive); 0.16.1-rc1 soak run 2, alias 3: the host's first creature dead, the VM's paired one died and refilled before the shots,
	# then gone from its game. The mod's pairs and deaths were right both times; the snapshot was not. At least 10 s after the shots.
	sleep $((10 - QA_STEADY > 0 ? 10 - QA_STEADY : 0)); QA_H=""; QA_V=""
	wait_for 20 _qa_steady || echo "quest actors ($QA_QUEST): its aliases still changing after 20 s; compared as last read"
	if [ -n "$QA_H" ] && [ -n "$QA_V" ]; then
		f=$(_qa_refilled "$hl" "$vl"); [ -n "$f" ] && echo "quest actors ($QA_QUEST): refilled since the first read (compared as they are now): alias(es) $f"
		hl=$QA_H; vl=$QA_V
	fi
	# A death still landing is waited for (QA_LAND s), not failed. The shots end with the first read's creatures dead, but an alias
	# refilled since is still fought by the scene's own NPCs (its scavenger), and any read can fall inside a death's landing: a spawn's
	# owner sends its death at the engine's death event, ~3 s after the fall begins (every "fell to a blow of ours" -> "died here" in
	# the rig's logs: 3.0-3.9 s), and it lands 3.0-4.2 s after the killing blow. 0.17.0-rc1 soak, alias 3: the VM's refilled creature
	# fell ~21:18:27, killed by the scene, ~18 s after the shots ended; read dead in the VM, alive in the host's game, ~3 s before its
	# kill landed there (21:18:30.3). A death lost, or later than QA_LAND, still fails.
	local land=$SECONDS
	if ! _qa_diff "$hl" "$vl" && [ $QA_N -gt 0 ]; then
		echo "quest actors ($QA_QUEST): differ at first:$QA_ODD waiting up to $QA_LAND s for a death or a hit to land"
		wait_for "$QA_LAND" _qa_diff "$hl" "$vl" && echo "quest actors ($QA_QUEST): alike after $((SECONDS - land)) s"
	fi
	_qa_compare "quest actors ($QA_QUEST): each creature alike in both games 10 s after the shots ($who in the host's game; a death landing waited for up to $QA_LAND s)" "$hl" "$vl" last
}
step_questactors() {
	section "quest actors (a shared quest's created creatures paired across games: Gun Run, then $QA_QUEST)"
	local q
	q=$(field "$(host rules)" quests)
	if [ "$q" != safe ]; then fail "quest actors: set-up: the host's sQuests = safe" "rules say quests='$q'"; return; fi
	enter_both SanctuaryExt 14
	_qa_gunrun; pads_off
	expect_match "quest actors (Gun Run): both games up" "$(alive)" "host=ingame=true vm=ingame=true"
	_qa_silent; pads_off
	expect_match "quest actors ($QA_QUEST): both games up" "$(alive)" "host=ingame=true vm=ingame=true"
}

# ---- giving an item: drop and pickup, both ways ---------------------------------------------------------------------------------------------
# A dropped item is the session's (src/game/Drops.cpp): the server numbers it, each other game places a copy where it lies, the first
# to take it has it, and the other copies go. The giver walks off to the road, gets DROP_N of DROP_ITEM (console additem) and drops
# them (dev `dropitem`: the console's drop, which the mod takes as the Pip-Boy's); the other player walks to its copy and takes it
# with a press. Asserted: the giver's count fell; the other game lists the drop and placed a copy, shown; after the take the taker
# has them, the giver's count stays down, and neither game lists the drop any more.
DROP_ITEM=${DROP_ITEM:-00023736}; DROP_N=${DROP_N:-3}   # stimpaks: a shared kind (not a key, note, book, magazine or quest item)
_drop_new() {  # _drop_new <who> <giver id> <drop ids before>: "<drop id> <copy ref|none>" of a new drop of DROP_ITEM by that giver
	local i c
	dc "$1" drops | tr ';' '\n' | sed -nE "s/^ ?drop ([0-9]+) base=$DROP_ITEM x$DROP_N .* owner=$2 .* copy=([0-9A-Za-z]+)$/\1 \2/p" | while read -r i c; do case " $3 " in *" $i "*) ;; *) echo "$i $c";; esac; done | tail -1
}
_drop_ids() { dc "$1" drops | grep -oE 'drop [0-9]+ base=' | grep -oE '[0-9]+' | tr '\n' ' '; }
_drop_copied() { DROP_SEEN=$(_drop_new "$1" "$2" "$3"); [ -n "$DROP_SEEN" ] && [ "${DROP_SEEN#* }" != none ]; }
_drop_gone() { ! dc "$1" drops | grep -qE "drop $2 base="; }
_shown() { local r; r=$(dc "$1" "refinfo $2"); [ "$(field "$r" disabled)" = false ] && [ "$(field "$r" 3d)" = true ]; }
_count_is() { [ "$(dc "$1" "count 00000014 $DROP_ITEM")" = "$2" ]; }
step_drops() {
	section "giving an item (drop and pickup), both ways"
	local giver taker gid ids gb ga tb id copy p
	enter_both SanctuaryExt 14
	for giver in host vm; do
		taker=$(other_game $giver); gid=$(dcf $giver 'net status' id)
		walk $giver "drop ($giver gives): set-up: the giver walks off to the road" 1 100 $PARK_SPOT || continue
		dc $giver "pad unplug" >/dev/null
		dc $giver "console player.additem $DROP_ITEM $DROP_N" >/dev/null; sleep 1
		gb=$(dc $giver "count 00000014 $DROP_ITEM"); tb=$(dc $taker "count 00000014 $DROP_ITEM"); ids=$(_drop_ids $taker)
		if ! is_int "$gb" || ! is_int "$tb"; then fail "drop ($giver gives): set-up: both counts are read before the drop" "giver '$gb' taker '$tb': not numbers"; walk $giver "drop: set-up: back to the marker" 1 100 $MARKER_SPOT; continue; fi
		dc $giver "dropitem $DROP_ITEM $DROP_N" >/dev/null
		if wait_for 6 _count_is $giver $((gb - DROP_N)); then pass "drop ($giver gives): its count fell by $DROP_N" "$gb -> $((gb - DROP_N))"
		else fail "drop ($giver gives): its count fell by $DROP_N" "$gb -> $(dc $giver "count 00000014 $DROP_ITEM")"; walk $giver "drop: set-up: back to the marker" 1 100 $MARKER_SPOT; continue; fi
		if ! wait_for 20 _drop_copied $taker "$gid" "$ids"; then fail "drop ($giver gives): the other game lists it and placed a copy" "${DROP_SEEN:-not listed} ($(dc $taker drops | cut -c1-200))"; walk $giver "drop: set-up: back to the marker" 1 100 $MARKER_SPOT; continue; fi
		read -r id copy <<<"$DROP_SEEN"
		pass "drop ($giver gives): the other game lists it and placed a copy" "drop $id, copy $copy"
		if wait_for 8 _shown $taker "$copy"; then pass "drop ($giver gives): the copy is shown in the other game" "$(dc $taker "refinfo $copy" | grep -oE ' x=[^ ]* y=[^ ]* z=[^ ]*')"
		else fail "drop ($giver gives): the copy is shown in the other game" "$(dc $taker "refinfo $copy" | cut -c1-200)"; fi
		# The giver steps aside first (a real walk, checked): the copy lies at the giver's feet, and a taker walking up to it
		# walked into the giver's stand-in and stuck there for 20 s (2026-11-07, the 0.14.0-rc1 soak), a walk that never arrived;
		# and with the stand-in on or next to the copy no press took it (the stand-in, not the copy, under the crosshair).
		walk $giver "drop ($giver gives): set-up: the giver steps aside" 1 100 $DROP_ASIDE; dc $giver "pad unplug" >/dev/null
		if walk $taker "drop ($giver gives): set-up: the other player walks to the copy" 0.6 70 "$copy"; then
			p=$(pitch_to $taker "$copy"); is_int "$p" || p=50
			# Whether a press reached the copy is read from the taker's own claims (`net status` drops: claimed=, a press on a
			# copy asks the server for it; takeFailed=, a grant the engine's pickup then lost). 2026-11-07: the host's five presses
			# on the VM's drop took nothing (0.14.0-rc1, the soak's round 2 and a rerun) and nothing in either log said whether
			# one had landed. A press that claimed it and got nothing is the mod's failure. No claim at all means no press
			# activated the copy. That is the rig's (a rig fault: the step is healed and run again, so the real press is still
			# checked) only on a shown rig cause: the giver still within DROP_CLEAR u of the copy (the step-aside didn't take
			# them away), or no press seen read by the game (press_a's PRESS_REACHED). Otherwise it is the mod's failure, and the
			# game's own activate of the copy (dev `activate`, the engine's ActivateRef, not a player's press) says which: a copy
			# it takes is a player's press on a copy that does nothing (a broken player-activate path), one it can't take is a
			# copy nothing can take.
			local c0 c1 tf0 tf1 st gd
			st=$(dc $taker 'net status' | grep -oE 'drops: lying=[^|]*'); c0=$(field "$st" claimed); tf0=$(field "$st" takeFailed)
			if press_a $taker "$copy" "wait_for 4 _count_is $taker $((tb + DROP_N))" "$p" $((p + 10)) $((p - 10)) 50 60; then
				pass "drop ($giver gives): the other player takes it with a press" "$tb -> $((tb + DROP_N)) after $PRESSES press(es)"
			else
				st=$(dc $taker 'net status' | grep -oE 'drops: lying=[^|]*'); c1=$(field "$st" claimed); tf1=$(field "$st" takeFailed)
				local why="count $tb -> $(dc $taker "count 00000014 $DROP_ITEM") after $PRESSES presses; claims $c0 -> $c1, failed takes $tf0 -> $tf1; the taker at ($(_xy_of $taker)), the giver at ($(_xy_of $giver)) (copy: $(dc $taker "refinfo $copy" | cut -c1-120))"
				if is_int "$c0" && is_int "$c1" && [ "$c1" -eq "$c0" ]; then
					gd=$(_dist_xy "$(_xy_of $giver)" "$(dc $taker "refinfo $copy" | grep -oE ' x=[^ ]*' | cut -d= -f2) $(dc $taker "refinfo $copy" | grep -oE ' y=[^ ]*' | cut -d= -f2)")
					local act; act=$(dc $taker "activate $copy"); sleep 0.5
					local took=""; wait_for 6 _count_is $taker $((tb + DROP_N)) && took=1
					if [ -z "$took" ]; then fail "drop ($giver gives): the other player takes it with a press" "no press activated the copy, and the game's own activate of it took nothing either ($act): $why"
					elif is_int "$gd" && [ "$gd" -lt "$DROP_CLEAR" ]; then rig_fault input "drop ($giver gives): the other player takes it with a press" "no press activated the copy (no claim made) with the giver still $gd u from it (the step-aside didn't take them $DROP_CLEAR u away), and the game's own activate took it ($act); $why"
					elif [ -z "$PRESS_REACHED" ]; then rig_fault input "drop ($giver gives): the other player takes it with a press" "no press activated the copy (no claim made), and no press was seen read by the game (pad: $PRESS_PAD); the game's own activate took it ($act); $why"
					else fail "drop ($giver gives): the other player takes it with a press" "the pad's presses reached the game and none activated the copy (no claim made), with the giver $gd u away, while the game's own activate of it took it ($act): $why"; fi
				else fail "drop ($giver gives): the other player takes it with a press" "a press claimed the copy and the take never came: $why"; fi
			fi
			if wait_for 8 _drop_gone $taker "$id" && wait_for 4 _drop_gone $giver "$id"; then pass "drop ($giver gives): neither game lists it after the take" "drop $id"
			else fail "drop ($giver gives): neither game lists it after the take" "host: $(host drops | cut -c1-160) | vm: $(vm drops | cut -c1-160)"; fi
			ga=$(dc $giver "count 00000014 $DROP_ITEM"); expect_eq "drop ($giver gives): the giver doesn't get it back" "$ga" "$((gb - DROP_N))"
		fi
		walk $taker "drop: set-up: the taker back to the marker" 1 100 $MARKER_SPOT; walk $giver "drop: set-up: the giver back to the marker" 1 100 $MARKER_SPOT
		pads_off
	done
	pads_off
}

# ---- down and helped up, both ways -----------------------------------------------------------------------------------------------------------
# With a friend in the session a lethal hit puts the player down to wait for help (src/game/Downed.cpp); a friend within 300 u of their
# stand-in for 2 s helps them up with the rules' share of health, and the down's camera and input layer end. The helper walks off out
# of reach; the other player's god mode goes off and damage is injected on it (console damageav, as the host's gunfire is injected for
# combat); the helper walks back to the stand-in. Asserted: down and waiting in its own game and shown down in the helper's, and not
# helped from afar; helped up after the walk, the helper's game counting the help; health back to the rules' share; the camera a
# first- or third-person view again (dev `controls`: camera=0 or 8, not the bleed-out's 11 or a transition's 5) with the life state
# alive; the Pip-Boy opens on a press of the pad's B and closes on another; and the log window holds the help and no warning that the
# down left its camera or input layer on, or looped the camera. God mode goes back on and health back to full after. Then the joiner
# is downed again and again (step_downed_repeat).
_down_waits() { local r; r=$(dc "$1" rules); [ "$(field "$r" down)" = true ] && [ "$(field "$r" waits)" = true ]; }
_friends_down() { local n; n=$(field "$(dc "$1" rules)" friendsDown); is_num "$n" && [ "$n" -ge 1 ]; }
_helped_up() { local r; r=$(dc "$1" rules); [ "$(field "$r" down)" = false ] && is_num "$(field "$r" revives)" && [ "$(field "$r" revives)" -gt "$2" ]; }
_pipboy_gone() { ! menu_open "$1" PipboyMenu 0; }
_god() { dc "$1" "god $2" >/dev/null; sleep 0.5; dc "$1" "god $2"; }   # `god` answers before the console's tgm has run: the second call says
step_downed() {
	section "down and helped up (injected damage, a real walk to help), both ways"
	local down helper r r0 h0 hp0 hp1 rh puppet c f g n
	enter_both SanctuaryExt 14
	for down in host vm; do
		helper=$(other_game $down)
		g=$(_god $down on); dc $down "console player.restoreav health 10000" >/dev/null; sleep 1; hp0=$(dcf $down "refinfo 00000014" hp)
		walk $helper "down ($down): set-up: the helper walks out of reach" 1 100 $PARK_SPOT || continue
		dc $helper "pad unplug" >/dev/null
		r=$(dc $down rules); r0=$(field "$r" revives); rh=$(field "$r" health | tr -d '%'); h0=$(field "$(dc $helper rules)" helped)
		if ! is_num "$r0" || ! is_num "$rh" || ! is_num "$h0" || ! is_num "$hp0" || [ "$(field "$r" revive)" != true ]; then fail "down ($down): set-up: the rules allow a revive" "$(cut -c1-200 <<<"$r") | helper helped='$h0' hp0='$hp0'"; walk $helper "down: set-up: back" 1 100 $MARKER_SPOT; continue; fi
		win_mark down $down
		g=$(_god $down off); [ "$g" = "god=false" ] || { fail "down ($down): set-up: god mode off" "$g"; walk $helper "down: set-up: back" 1 100 $MARKER_SPOT; continue; }
		dc $down "console player.damageav health 10000" >/dev/null
		if wait_for 6 _down_waits $down; then pass "down ($down): goes down and waits for help" "$(grep -oE 'downs=[0-9]*' <<<"$(dc $down rules)")"
		else fail "down ($down): goes down and waits for help" "$(dc $down rules | grep -oE 'downed: [^|]*' | cut -c1-200); hp $(dcf $down "refinfo 00000014" hp)"; _god $down on >/dev/null; dc $down "console player.restoreav health 10000" >/dev/null; walk $helper "down: set-up: back" 1 100 $MARKER_SPOT; continue; fi
		if wait_for 6 _friends_down $helper; then pass "down ($down): the helper's game shows them down" "friendsDown=$(field "$(dc $helper rules)" friendsDown)"; else fail "down ($down): the helper's game shows them down" "$(dc $helper rules | grep -oE 'friendsDown=[0-9]*')"; fi
		sleep 3
		if _down_waits $down; then pass "down ($down): not helped from out of reach" "still down 3 s on, the helper about 1000 u off"; else fail "down ($down): not helped from out of reach" "$(dc $down rules | grep -oE 'downed: [^|]*' | cut -c1-160)"; fi
		puppet=$(dc $helper 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | head -1 | cut -d= -f2)
		if [ -z "$puppet" ]; then fail "down ($down): set-up: the helper's game has the stand-in" "no puppet= in its net status"
		else
			walk $helper "down ($down): set-up: the helper walks to the stand-in" 0.8 150 "$puppet"
			if wait_for 15 _helped_up $down "$r0"; then pass "down ($down): helped up after the walk" "revives $r0 -> $(field "$(dc $down rules)" revives)"
			else
				walk $helper "down ($down): set-up: the helper steps closer" 0.5 70 "$puppet"
				if wait_for 10 _helped_up $down "$r0"; then pass "down ($down): helped up after the walk" "after a step closer"; else fail "down ($down): helped up after the walk" "$(dc $down rules | grep -oE 'downed: [^|]*' | cut -c1-200)"; fi
			fi
		fi
		n=$(field "$(dc $helper rules)" helped); if is_num "$n" && [ "$n" -gt "$h0" ]; then pass "down ($down): the helper's game counts the help" "helped $h0 -> $n"; else fail "down ($down): the helper's game counts the help" "helped $h0 -> '$n'"; fi
		sleep 4   # the get-up plays out
		hp1=$(dcf $down "refinfo 00000014" hp)
		expect_ge "down ($down): health back to the rules' $rh% (of $hp0)" "$hp1" "$(awk -v h="$hp0" -v p="$rh" 'BEGIN{printf "%d", h * p / 100 - 2}')"
		c=$(dc $down controls); expect_match "down ($down): the camera a first- or third-person view again, alive" "camera=$(grep -oE 'camera=[0-9-]+' <<<"$c" | cut -d= -f2) life=$(grep -oE 'life=[0-9]+' <<<"$c" | cut -d= -f2)" '^camera=(0|8) life=0$'
		dc $down "pad button b" >/dev/null
		if menu_open $down PipboyMenu 4; then
			sleep 1; dc $down "pad button b" >/dev/null   # the VM's Pip-Boy takes 2-3 s to put away
			if wait_for 6 _pipboy_gone $down; then pass "down ($down): the Pip-Boy opens on a press and closes" "pad B twice"; else fail "down ($down): the Pip-Boy opens on a press and closes" "it opened and stayed open 6 s after the second press"; clear_menus $down >/dev/null; fi
		else fail "down ($down): the Pip-Boy opens on a press and closes" "no PipboyMenu after the pad's B ($(dc $down menus | grep -oE 'open=[^ ]*'))"; fi
		f=$(win_read down $down)
		if [ ! -s "$f" ]; then fail "down ($down): its log window read" "no new lines"
		else
			n=$(grep -c "Downed: helped up by player" "$f"); expect_ge "down ($down): its log has the help" "$n" 1
			n=$(grep -cE "\[W\] Downed: (up again, but the down left on|the camera's way back looped)" "$f"); expect_zero "down ($down): no camera or input layer left on by the down (log warnings)" "$n"
		fi
		g=$(_god $down on); dc $down "console player.restoreav health 10000" >/dev/null
		[ "$g" = "god=true" ] || fail "down ($down): god mode back on" "$g"
		walk $helper "down: set-up: the helper back to the marker" 1 100 $MARKER_SPOT
		pads_off
	done
	pads_off
	step_downed_repeat vm 5   # the joiner, as in the reports: downed again at once on each help-up
	step_downed_repeat vm 5 pa   # and in power armor, where each blast at the feet shoves the down player off
}
# ---- downed again and again ------------------------------------------------------------------------------------------------------------------
# A player downed, helped up and downed again at once, over and over (as in a fire that keeps burning, a rocket engine's): each next
# down starts while the camera is still on its way back from the one before, or the body still getting up. The helper stands at the
# down player's stand-in the whole time (reached by a real walk, and walked back to it whenever a blast shoved it off: _rep_follow; its
# distance after each walk back must stay in the help range, or it is the rig's fault), so each down is helped up after the rules'
# help time. Run once as is and once with the down player in power armor ("pa"), where each blast shoves them. The next lethal
# hit (console damageav, as the host's gunfire is injected for combat; sent on the down player's own machine by downrepeat.ps1, as a
# command over the tunnel takes too long) comes as soon as the down player's game says they are up (in the
# get-up), half a second on, or 0.3 s after a grenade's explosion at their feet (which on the rig does not knock the player over), and
# one round takes a second hit while still down. Asserted after the last help-up: the camera a first- or third-person view (not a
# transition or the bleed-out camera), still so a few seconds on, alive; the camera near the player (dev `rules`: away, where the build
# has it) before and after the down player walks off; the Pip-Boy opens and closes; no loop of the camera's way back in the log (the
# camera's push keeps the view, Downed::Install). The log's other camera fixes are counted, never failed.
_view() { local c; c=$(dc "$1" controls); echo "camera=$(grep -oE 'camera=[0-9-]+' <<<"$c" | cut -d= -f2) life=$(grep -oE 'life=[0-9]+' <<<"$c" | cut -d= -f2)"; }
_away() { dc "$1" rules | grep -oE 'away [0-9?]+' | tail -1 | cut -d' ' -f2; }
# The Downed lines of a log window whose camera root was far from the player (away over 1000): the down line's own read (the first frame; the known one-frame engine flash, PR 364 notes) and every later read. Prints "first later".
_far_reads() { awk '/Downed: .*camera .* away [0-9]+/ { match($0, /away [0-9]+/); v = substr($0, RSTART + 5, RLENGTH - 5) + 0; if (v > 1000) { if ($0 ~ /Downed: down;/) a++; else b++ } } END { print a + 0, b + 0 }' "$1"; }
# _rep_follow <helper> <stand-in> <stop file> <out file>: while the rounds run, the helper keeps to the down player's stand-in as a
# player would. Every second, over REP_FOLLOW u from it, it walks back within 100 u (walkto.sh, real movement). A blast at the feet of
# a downed player in power armor shoves them 150-250 u: by the 4th round of a soak the helper that stood still was out of the 300 u
# help range, and that down was never helped (2026-11-07). Writes "<largest gap read> <largest distance after a walk back, or with
# none needed> <walks back> <reads that gave a distance>" to the out file each second; stops when the stop file is there.
REP_FOLLOW=${REP_FOLLOW:-150}
_ref_xy() { local r; r=$(dc "$1" "refinfo $2"); echo "$(field "$r" x) $(field "$r" y)"; }
_rep_follow() {
	local helper=$1 puppet=$2 stop=$3 out=$4 d gap=0 stood=0 walks=0 reads=0
	while [ ! -e "$stop" ]; do
		d=$(_dist_xy "$(_xy_of "$helper")" "$(_ref_xy "$helper" "$puppet")")
		if is_int "$d"; then
			reads=$((reads + 1))
			[ "$d" -gt "$gap" ] && gap=$d
			if [ "$d" -gt "$REP_FOLLOW" ]; then
				bash "$RIG/walkto.sh" "$helper" "$puppet" 0.8 100 15 >/dev/null 2>&1; walks=$((walks + 1))
				d=$(_dist_xy "$(_xy_of "$helper")" "$(_ref_xy "$helper" "$puppet")")
			fi
			is_int "$d" && [ "$d" -gt "$stood" ] && stood=$d
		fi
		echo "$gap $stood $walks $reads" > "$out"
		sleep 1
	done
	dc "$helper" "pad unplug" >/dev/null
}
step_downed_repeat() {
	local down=${1:-vm} rounds=${2:-5} pa=${3:-} helper puppet g v a0 a1 f n out hits=0 tag="" lb stop fo fpid gap stood walks reads lf
	helper=$(other_game "$down")
	[ -n "$pa" ] && tag=" in power armor"; lb="$down$tag"
	section "downed again and again at once on each help-up ($down$tag, $rounds rounds)"
	g=$(_god "$down" on); dc "$down" "console player.restoreav health 10000" >/dev/null
	# "pa": the down player in a frame first (pa_get_in: a real E), and out of it at the end (pa_leave). A blast at the feet shoves a
	# downed player in power armor 150-250 u, where one out of it lies still: the helper's follow below is what keeps them in reach.
	if [ -n "$pa" ] && ! pa_get_in "$down"; then rig_fault input "repeat ($lb): set-up: in a frame" "extraPA is not true after 3 tries of the E (frame $PA_FRAME)"; _pa_frames_off "$down"; return; fi
	[ -n "$pa" ] && sleep 3   # the helper's game puts the stand-in in the suit
	puppet=$(dc "$helper" 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | head -1 | cut -d= -f2)
	[ -n "$puppet" ] || { fail "repeat ($lb): set-up: the helper's game has the stand-in" "no puppet= in its net status"; [ -n "$pa" ] && pa_leave "$down" "repeat ($lb)"; return; }
	walk "$helper" "repeat ($lb): set-up: the helper walks to the stand-in" 0.8 100 "$puppet" || { [ -n "$pa" ] && pa_leave "$down" "repeat ($lb)"; return; }
	dc "$helper" "pad unplug" >/dev/null
	win_mark rep "$down"
	win_mark rep "$helper"
	g=$(_god "$down" off); [ "$g" = "god=false" ] || { fail "repeat ($lb): set-up: god mode off" "$g"; [ -n "$pa" ] && pa_leave "$down" "repeat ($lb)"; return; }
	# the helper keeps to the stand-in while the rounds run, as a player would (_rep_follow, in the background)
	stop="$A_OUT/rep-follow-stop$(rsuf)"; fo="$A_OUT/rep-follow$(rsuf).txt"; rm -f "$stop" "$fo"
	_rep_follow "$helper" "$puppet" "$stop" "$fo" & fpid=$!
	# the rounds run on the down player's own machine (tools/rig/downrepeat.ps1): over the tunnel a command is too slow for a get-up
	if [ "$down" = vm ]; then
		scp -q -i "$VM_KEY" -o ConnectTimeout=20 "$RIG/downrepeat.ps1" "$VM_SSH:downrepeat.ps1" 2>/dev/null
		out=$(vmssh "powershell -NoProfile -ExecutionPolicy Bypass -File downrepeat.ps1 -Rounds $rounds -Explosion $GRENADE_EXPLOSION")
	else out=$(powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$RIG/downrepeat.ps1")" -Rounds "$rounds" -Explosion "$GRENADE_EXPLOSION" | tr -d '\r'); fi
	touch "$stop"; wait "$fpid" 2>/dev/null
	sed 's/^/  /' <<<"$out"
	gap=""; stood=""; walks=""; reads=""
	[ -s "$fo" ] && read -r gap stood walks reads 2>/dev/null < "$fo"
	# The helper's distance to the stand-in after each walk back (or with none needed) is the rig's: out of the 300 u help range
	# (DownRules::HELP_RANGE), a down not helped up is the walk's fault, not the mod's. No read that gave a distance: not measured,
	# also the rig's (never a pass at "at most 0 u").
	if ! is_int "$reads" || [ "$reads" = 0 ] || ! is_int "$stood"; then rig_fault walk "repeat ($lb): the helper kept within help range of the stand-in" "no distance between the helper and the stand-in was read ('$(cat "$fo" 2>/dev/null)')"
	elif [ "$stood" -gt 300 ]; then rig_fault walk "repeat ($lb): the helper kept within help range of the stand-in" "$stood u after a walk back, over the 300 u help range (largest gap $gap u, $walks walk(s) back)"
	else pass "repeat ($lb): the helper kept within help range of the stand-in" "at most $stood u after each walk back (largest gap $gap u, $walks walk(s) back; help range 300 u)"; fi
	hits=$(grep -c ': helped up$' <<<"$out")
	expect_eq "repeat ($lb): downed and helped up again and again" "$hits" "$rounds"
	g=$(_god "$down" on); dc "$down" "console player.restoreav health 10000" >/dev/null
	sleep 6   # the last get-up plays out, and a stuck transition would outlast the game's own (0.5-1 s) by far
	v=$(_view "$down"); expect_match "repeat ($lb): the camera a first- or third-person view, alive" "$v" '^camera=(0|8) life=0$'
	sleep 3; v=$(_view "$down"); expect_match "repeat ($lb): still a view 3 s on" "$v" '^camera=(0|8) life=0$'
	a0=$(_away "$down")
	walk "$down" "repeat ($lb): set-up: the down player walks off" 1 100 $PARK_SPOT
	a1=$(_away "$down")
	if is_num "$a0" && is_num "$a1"; then expect_le "repeat ($lb): the camera near the player, before and after a walk" "$(( a0 > a1 ? a0 : a1 ))" 400 u
	else pass "repeat ($lb): the camera near the player" "not measured: this build's rules have no camera distance"; fi
	dc "$down" "pad button b" >/dev/null
	if menu_open "$down" PipboyMenu 4; then
		sleep 1; dc "$down" "pad button b" >/dev/null
		if wait_for 6 _pipboy_gone "$down"; then pass "repeat ($lb): the Pip-Boy opens on a press and closes" "pad B twice"; else fail "repeat ($lb): the Pip-Boy opens on a press and closes" "it opened and stayed open"; clear_menus "$down" >/dev/null; fi
	else fail "repeat ($lb): the Pip-Boy opens on a press and closes" "no PipboyMenu after the pad's B ($(dc "$down" menus | grep -oE 'open=[^ ]*'))"; fi
	f=$(win_read rep "$down")
	if [ -s "$f" ]; then
		n=$(grep -c "Downed: helped up by player" "$f"); expect_ge "repeat ($lb): its log has the help-ups" "$n" "$rounds"
		n=$(grep -c "way back looped" "$f"); expect_zero "repeat ($lb): the camera's way back never looped (the push keeps the view)" "$n"
		pass "repeat ($lb): other camera fixes in its log (counted only)" "pushes kept $(grep -c "its way back is the view" "$f"), strays $(grep -c "up, settled and out of any down" "$f"), stuck transitions $(grep -c "transition state has been on" "$f"), left-on $(grep -c "but the down left on" "$f")"
		local who lf first later
		for who in "$down" "$helper"; do
			if [ "$who" = "$down" ]; then lf=$f; else lf=$(win_read rep "$who"); fi
			[ -s "$lf" ] || continue
			read -r first later <<<"$(_far_reads "$lf")"
			if [ "$later" -gt 0 ]; then warn "repeat ($who's log$tag): the camera far from the player (away over 1000) after the first down frame" "$later read(s)"; else pass "repeat ($who's log$tag): no far camera read after the first down frame" "ok"; fi
			if [ "$first" -gt 0 ]; then warn "repeat ($who's log$tag): the first down frame's camera read was far (the known one-frame engine flash, started off the main thread; counted, not a fail)" "$first read(s)"; fi
		done
	else fail "repeat ($lb): its log window read" "no new lines"; fi
	lf=$(win_read rep "$helper")
	[ -s "$lf" ] && pass "repeat ($lb): the helper's log says when the down player was out of reach (counted only)" "$(grep -c "is down [0-9]* u away; help starts within" "$lf") line(s), $(grep -c "is down within reach, but we are in a fight" "$lf") held by a fight"
	walk "$down" "repeat: set-up: back to the marker" 1 100 $MARKER_SPOT; walk "$helper" "repeat: set-up: the helper back to the marker" 1 100 $MARKER_SPOT
	pads_off
	[ -n "$pa" ] && pa_leave "$down" "repeat ($lb)"
}


# ---- churn: cell hops with save and reload ------------------------------------------------------------------------------------
step_churn() {
	local mins=${1:-3} end half hopper=vm watcher=host i=0 lastsave c a down_run=0
	section "cell churn with save and reload (${mins} min)"
	local cells=(Vault81Entry Vault81 SuperDuperMart01 RedRocketExt SanctuaryExt) hops=0 downs=0
	end=$(( $(date +%s) + mins * 60 )); half=$(( $(date +%s) + mins * 30 )); lastsave=$(date +%s)
	dc $watcher "console coc ${cells[0]}" >/dev/null; sleep 12
	while [ "$(date +%s)" -lt "$end" ]; do
		if [ "$(date +%s)" -gt "$half" ] && [ $hopper = vm ]; then hopper=host; watcher=vm; echo "-- roles swap: host hops"; fi
		c=${cells[$((i % ${#cells[@]}))]}; dc $hopper "console coc $c" >/dev/null; sleep 20
		if [ $(( $(date +%s) - lastsave )) -gt 90 ]; then
			dc $hopper "console save f4mpchurn" >/dev/null; sleep 6; dc $hopper "console load f4mpchurn" >/dev/null
			[ $hopper = host ] && { sleep 8; host "key 0x0D 100" >/dev/null; }
			sleep 20; lastsave=$(date +%s)
		fi
		a=$(alive); hops=$((hops + 1)); echo "$(date +%T) hop $i $c: $a"
		# A game down after two hops in a row is gone (a load outlasts one): the churn stops there and the crash logs are read
		# now. 2026-11-04 the host crashed 70 minutes into the 0.12.0 soak's churn, which hopped the VM alone for 8 more minutes until
		# the run was stopped by hand, and the verdict said only "interrupted".
		case "$a" in *DOWN*) downs=$((downs + 1)); down_run=$((down_run + 1));; *) down_run=0;; esac
		if [ "$down_run" -ge 2 ]; then
			# the run ends here: every step after it would fail on the missing game (_rig_stop reads the crash logs and names a crash).
			# A game gone mid-churn is most likely the mod's (a crash or a hang), so it is a plain FAIL, not "rig broken".
			expect_zero "churn: hops after which a game was down" "$downs"
			fail "churn: both games up" "$a after two hops in a row: a game went down (a crash or a hang); the churn stopped"
			[ -z "$A_RIG_STOPPED" ] && _rig_stop "a game went down in the churn" "$a after two hops in a row (a crash or a hang: the crash check below says which)"; return
		fi
		i=$((i + 1))
	done
	expect_zero "churn: hops after which a game was down" "$downs"
	expect_ge "churn: hops made" "$hops" 4
	ns host; ns vm
	expect_eq "churn: host still hosting a joined session" "$(nsf host status)" joined 2>/dev/null
	expect_eq "churn: vm still joined" "$(nsf vm status)" joined
}

# ---- perf: our frame cost, read after each phase (the recorder keeps only the last 3600 frames, about a minute) ----------------------
# perf_phase <phase>: our work in the frames since the last reset, in each game, then a reset. ours_in_hitches is the number that
# says whether a stutter is ours (docs/PERF.md): it must stay 0.
perf_phase() {
	local who p h p50 p99 fr bad
	for who in host vm; do
		p=$(dc $who perf); h=$(field "$p" ours_in_hitches); p50=$(field "$p" p50); p99=$(field "$p" p99); fr=$(field "$p" frames); bad=""
		if ! is_num "$h" || ! is_num "$p50" || ! is_num "$p99"; then fail "perf after $1 ($who)" "no perf numbers (ours_in_hitches '$h', p50 '$p50', p99 '$p99'): ${p:0:120}"; continue; fi
		[ "$h" != 0 ] && bad="$bad ours_in_hitches=$h (want 0);"
		_cmp "$p50" "<=" "$T_PERF_P50" || bad="$bad p50 $p50 ms (limit $T_PERF_P50);"
		_cmp "$p99" "<=" "$T_PERF_P99" || bad="$bad p99 $p99 ms (limit $T_PERF_P99);"
		if [ -z "$bad" ]; then pass "perf after $1 ($who)" "frames=$fr p50=$p50 p99=$p99 ms, ours_in_hitches=0"; else fail "perf after $1 ($who)" "$bad frames=$fr"; fi
		dc $who "perf reset" >/dev/null
	done
}

# ---- reconnect: the VM's game restarts and rejoins ----------------------------------------------------------------------------------
_vm_minsteam() { vmssh "& C:\\F4MP\\in-vm-run.ps1 -Name \"F4MP-MinSteam-\$(Get-Date -Format HHmmssfff)\" -Exe C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe -Arguments '-WindowStyle Hidden -NoProfile -ExecutionPolicy Bypass -File C:\\F4MP\\minsteam.ps1'" >/dev/null; }
_vm_ready_to_fire() {
	local s0 s1 try
	vm "console player.additem 00004822 1" >/dev/null; vm "console player.additem 0001F276 20" >/dev/null; vm "console player.equipitem 00004822" >/dev/null
	for try in 1 2; do
		_vm_minsteam; sleep 2; vm focus >/dev/null; sleep 1; vm "draw on" >/dev/null; sleep 1.5
		s0=$(nsf_now vm shots); vm "click 120" >/dev/null; sleep 2; s1=$(nsf_now vm shots)
		is_num "$s0" && is_num "$s1" && [ "$s1" -gt "$s0" ] && break
	done
	if is_num "$s0" && is_num "$s1" && [ "$s1" -gt "$s0" ]; then pass "reconnect: the restarted VM game fires a real click" "shots $s0 -> $s1 (try $try)"
	else rig_broken input "reconnect: the restarted VM game fires a real click" "shots '$s0' -> '$s1' after 2 tries: its mouse and attacks are dead (restart it with focus sent during start-up); later VM steps on this game will fail$(mouse_why vm)"; fi
}
nsf_now() { field "$(dc "$1" 'net status')" "$2"; }
step_reconnect() {
	section "reconnect (the VM's game restarts and rejoins)"
	local j
	_vm_restart_rejoin reconnect; ns vm; ns host
	expect_eq "reconnect: VM rejoined" "$(nsf vm status)" joined
	# the players list names people: only its length is used, never printed
	np=$(grep -oE 'players: \[.*' <<<"${NS[host]}" | grep -oE "puppet=FF[0-9A-F]+" | sort -u | wc -l)
	expect_ge "reconnect: host lists the VM's player (puppets known)" "$np" 1
	rig_up "reconnect: both games up"
	# A game started by in-vm-3-game.ps1 can come up taking keys but not the mouse or the attack actions (2026-10-31: after
	# run 1's reconnect, run 2's VM grenade and power armor steps failed; hits.sh saw the same as shots=0). Steam's windows pop
	# over the game after a lobby join, so they are minimised and the game brought to the front, and one real click must fire
	# (`net status` shots counts our player's attacks): a game that can't is said here, not as later steps' failures.
	_vm_ready_to_fire
	sleep 20; WSB_VM=$(working_set vm)   # the VM's new baseline: the world is loaded and the rejoin has settled
}

# ---- fuzz: made-up packets against each game's decoders -------------------------------------------------------------------------------
step_fuzz() {
	section "fuzz (both games)"
	local who r seed=${1:-100}
	for who in host vm; do
		r=$(dc $who "fuzz 10 $seed"); expect_match "fuzz on $who: still here" "$r" 'still here$'
		sleep 2
	done
	expect_match "fuzz: both games up after" "$(alive)" '^host=ingame=true vm=ingame=true$'
	# The fuzz's made-up container changes are numbered 0-999 (DevCommands Fuzz); those past the server's last one are
	# waited for, asked of the server about 15 s later, and let go of when it answers that its log ends before them
	# (ContainerLedger OnChunk). That one WorldSync [W] line is this step's own doing, so it is asserted here (at most one
	# per game) and left out of the end check's warning count (FUZZ_LEDGER_LINE).
	sleep 20
	for who in host vm; do
		local n; n=$(grep -cE "$FUZZ_LEDGER_LINE" "$(log_delta_file $who)")
		if [ "$n" -le 1 ]; then pass "fuzz on $who: its made-up container changes let go of, once at most" "$n line(s)"
		else fail "fuzz on $who: its made-up container changes let go of, once at most" "$n lines: more than the fuzz explains"; fi
	done
	FUZZ_RAN=1
}

# ---- the end of a run --------------------------------------------------------------------------------------------------------------------
step_final() {
	section "end checks (crash logs, plugin logs, privacy, memory)"
	local g v
	crash_verdict "run"
	for g in host vm; do log_check $g "$g"; log_privacy $g "$g"; done
	if [ -n "$WSB_VM" ]; then v=$(working_set vm); [ -n "$v" ] && warn_gt "working set growth (vm, since the rejoin settled)" "$((v - WSB_VM))" "$T_WS_GROWTH_MB" MB; echo "working set: vm $WSB_VM -> ${v:-?} MB (rejoin settled -> end)"; fi
}

# ---- the cleanup at the start of a run: a known-clean save ----------------------------------------------------------------------------
smoke_cleanup_start() {
	section "known-clean start (test pieces from earlier runs)"
	bash "$RIG/cleanup.sh" scan --enter > "$A_OUT/cleanup-start.log" 2>&1
	if [ $? -eq 0 ]; then pass "no test pieces in the save at the start" "$(grep -c orphaned "$A_OUT/cleanup-start.log") game(s) show orphans of the session file (cleanup.sh: no game can scrap them)"
	else warn "test pieces from an earlier run were in the save" "removing them: $(grep 'still has' "$A_OUT/cleanup-start.log" | tr '\n' ' ')"
		bash "$RIG/cleanup.sh" run >> "$A_OUT/cleanup-start.log" 2>&1 && pass "test pieces removed before the run" "" || fail "test pieces removed before the run" "see $A_OUT/cleanup-start.log"
	fi
}

# ---- memory: working set after the combat step (loaded and warm) against just before the reconnect -----------------------------
ws_mark() { WSA_HOST=$(working_set host); WSA_VM=$(working_set vm); }
ws_check() {
	local h v; h=$(working_set host); v=$(working_set vm)
	echo "working set: host ${WSA_HOST:-?} -> ${h:-?} MB, vm ${WSA_VM:-?} -> ${v:-?} MB (after combat -> before the reconnect)"
	if [ -n "$WSA_HOST" ] && [ -n "$h" ]; then warn_gt "working set growth (host, combat to churn end)" "$((h - WSA_HOST))" "$T_WS_GROWTH_MB" MB; fi
	if [ -n "$WSA_VM" ] && [ -n "$v" ]; then warn_gt "working set growth (vm, combat to churn end)" "$((v - WSA_VM))" "$T_WS_GROWTH_MB" MB; fi
}

# ---- a story-manager radiant shared by its run: Out of the Fire (DN121) and Troubled Waters (DN151) ----------------------------------------
# Run on demand (`smoke.sh --only radiants --skip cleanup`), never in the loop: both quests run once, so a game takes this step once per
# fresh start (a game that has run one skips it with a WARN). RadiantRules shares them by their run (PR 320): both players arrive at the
# place (console player.moveto its map marker: placement to reach the scene, no movement stands in for anything), where the story manager
# hands each game the same quest with the same settlement; each shows its objective, which starts the sharing. With both games there
# the runs are joined by their same targets (RadiantSync: "it runs here with the same targets"); a VM that stands elsewhere is RAD_AWAY's
# case, below (it used to be refused: the console's startquest starts no quest of the story manager's). Asserted: the run is shared in both games with the
# same settlement, a stage the host sets is set once in each game, the reward stage gives the player an item in each game, and neither
# game makes or places a spawn for it.
# RAD_AWAY="<edid> ..." runs those cases with the VM left where it stands (Sanctuary), so its story manager never hands it the quest:
# only the host arrives and shows the objective, and the VM's game starts the host's run itself with the host's fills (RadiantSync's
# StartHere, through the game's own start: game/QuestStart). Asserted then: the VM's quest isn't running before, its log says the run
# is shared "started here with their fills", and the rest as above.
# case: <edid> <quest id> <map marker to arrive at> <objective to show> <a middle stage> <reward stage> <end stage>
RAD_CASES=${RAD_CASES:-"DN121 00026340 0002C07B 1 100 900 1000;DN151 000482CD 0002B889 5 100 400 500"}
RAD_AWAY=${RAD_AWAY:-}
_rad_log_has() { grep -qE "$2" "$(win_read radiants "$1")"; }
_rad_tail() { grep -E 'RadiantSync:|QuestStart:' "$A_OUT/windelta-radiants-$1.txt" 2>/dev/null | tail -4 | tr '\n' '|' | cut -c1-600; }
_rad_run() { dc "$1" "quests edid:$2" | tr ';' '\n' | grep -E " $2 " | grep -o 'running=[a-z0-9]*' | head -1; }
_rad_done() { dc "$1" "stages $2" | grep -o 'done([0-9]*)=[0-9a-z,]*' | sed 's/^done([0-9]*)=//'; }
_rad_inv() { dc "$1" inventory | grep -o ' items=[0-9]*' | sed 's/ items=//'; }
_rad_spawns() { dc "$1" 'net status' | grep -o 'spawns: ours=[0-9]* theirs=[0-9]* copies=[0-9]* reported=[0-9]* granted=[0-9]* refused=[0-9]* received=[0-9]* placed=[0-9]*'; }
# What was ever made or placed (the counts that only grow): the live ones fall as a passing encounter's actors die.
_rad_spawned() { _rad_spawns "$1" | grep -o 'reported=.*'; }
_rad_same_done() { [ "$(_rad_done vm "$1")" = "$(_rad_done host "$1")" ]; }
_rad_here() { dc "$1" "aliases $2" | tr '|' '\n' | grep -E "^ Ref [0-9]+ (Workshop|EventWorkshop|FoundWorkshop) " | grep -o 'here=[0-9A-F]*' | sort | tr '\n' ' '; }
_rad_is_running() { [ "$(_rad_run "$1" "$2")" = running=true ]; }
# ---- the shared workshop: a friend's build and wire at the host's settlement, as the host's pieces (opt-in: smoke.sh --only sharedws-power) ------
# With the host's bSharedWorkshop on (the default; src/game/SharedWorkshop.h, docs/DESIGN.md "Shared workshop"), a VM piece placed at
# Sanctuary becomes a request the host's game builds as its own and finishes with the engine's placement steps, and the VM's piece
# is adopted as its copy; a VM wire between two of the host's copies is taken off the VM's grid in the same frame and strung by the
# host's game instead. Set-up by console, said so in the report: both games coc to SanctuaryExt, Sanctuary made the host's
# (WorkshopScript.SetOwnedByPlayer, as the bench step does), then both walk to the workbench (real movement). The pieces go
# through the dev commands' request path (wsreport place and wswire hand over under the rule, as workshop mode does): the host places
# a generator (wsreport place + wsfinish: the host's own finish, probe P1), the VM places a light (a request: "built for player N"
# on the host) and wires the generator's copy to the light's copy (a request). Asserted: the host's grid holds generator, wire and
# light; the host's light is powered; the VM's copy of the light shows blocked, the host's, powered and lit; the VM's grids name no copy
# and no runtime reference, and its audit finds nothing, before and after an in-session save; the VM leaves and loads that save
# alone: grids clean, no copy with power links, the generator's copy hushed. Then the reverse: the VM places the generator (a
# request), the host the light and the wire (its own). Saves are throwaway (backup.sh before, restore after). The bases: the
# small generator (00030386, "Generator - Small") and the light bulb (env SWS_GENERATOR, SWS_LIGHT; verify with dev `edid` on a
# new game version).
SWS_GENERATOR=${SWS_GENERATOR:-00030386}; SWS_LIGHT=${SWS_LIGHT:-000560DF}; SWS_SAVE=${SWS_SAVE:-f4mp_sharedws}
# A light a wire can end on (WorkshopConstructionLight01: the WorkshopPowerConnection keyword, no power radius). The light bulb
# above takes no wire in any game: the engine's wire filter refuses it as an end (the dev wswire path asks nothing of it).
SWS_WIRED_LIGHT=${SWS_WIRED_LIGHT:-000D38D4}
SWS_MG_TURRET=${SWS_MG_TURRET:-000B3036}   # Machinegun Turret: a templated NPC, whose placed actor gets a runtime base (sharedws-prereq)
_sws_grid_clean() {  # _sws_grid_clean <who> <label>: that game's grids at Sanctuary name no copy and no runtime reference
	local g; g=$(dc "$1" "wsgrid $SANCT_BENCH")
	if [ "$(field "$g" copies)" = "named=0" ] || grep -q "copies named=0" <<<"$g"; then
		if grep -qE ' [ne]FF[0-9A-F]{6}' <<<"$g"; then fail "$2" "a grid names a runtime reference: $(cut -c1-200 <<<"$g")"; return 1; fi
		pass "$2" "$(cut -c1-120 <<<"$g")"; return 0
	fi
	[ -n "$g" ] || g="no answer from the $1 game's dev channel (the game or its tunnel not answering: nothing is known of its grids)"
	fail "$2" "$(cut -c1-200 <<<"$g")"; return 1
}
_sws_audit_clean() { local p; p=$(dc "$1" wspower); if [ "$(field "$p" found)" = 0 ]; then pass "$2" "found=0 strays=$(field "$(dc "$1" wsmode)" strays)"; else fail "$2" "$(cut -c1-160 <<<"$p")"; fi; }
_sws_copy_of() {  # _sws_copy_of <who> <builder's ref>: that game's copy of the other game's piece, by its key, or "" (1)
	# An empty or zero id is no piece: it used to match the first copy listed (2026-11-11: `_sws_copy_of vm ""` handed the host a
	# copy of something else, and the budget check read the host's right refusal as a failure)
	[[ $2 =~ ^[0-9A-F]{8}$ ]] && [ "$2" != 00000000 ] || return 1
	dc "$1" "wspower copies" | grep -oE "[0-9A-F]{8}(\s\(not loaded\))?=[0-9]+:$2" | grep -oE '^[0-9A-F]{8}' | head -1
}
_sws_has_copy() { [ -n "$(_sws_copy_of "$1" "$2")" ]; }
_sws_no_copy() {  # that game answered, and holds no copy of it (no answer from a game is no evidence: the VM's game once froze here, and an empty answer read as "gone")
	local c; c=$(dc "$1" "wspower copies")
	case "$c" in ""|error*) return 1;; esac
	! grep -qE "[0-9A-F]{8}(\s\(not loaded\))?=[0-9]+:$2" <<<"$c"
}
_sws_wire_answered() { grep -qE "WorkshopSync: request [0-9]+ \(wire\) at workbench [0-9A-F]{8}: applied" "$(win_read sws "$1")"; }
_sws_built() { grep -qE "WorkshopSync: [0-9A-F]{8} \(base $2\) built for player [0-9]+ at workbench" "$(win_read sws "$1")"; }
_sws_answered() { grep -qE "WorkshopSync: request [0-9]+ \((build|wire)\) at workbench [0-9A-F]{8}: applied" "$(win_read sws "$1")"; }
_sws_lit() {  # that game's copy shows its builder's power: a light (a LIGH: wsinfo says lit=) has its light made; a piece that shows power by its graph (the construction light) was told Powered
	local i l; i=$(dc "$1" "wsinfo $2"); l=$(field "$i" lit)
	[ "$(field "$i" theirPower)" = on ] || return 1
	if [ -n "$l" ]; then [ "$l" = yes ]; else [ "$(field "$i" told)" = Powered ]; fi
}
_sws_host_powered() { [ "$(field "$(host "wsinfo $1")" powered)" = yes ]; }
_sws_grid_has() { local g i; g=$(dc "$1" "wsgrid $SANCT_BENCH"); for i in "${@:2}"; do grep -qE "[ne>/]$i" <<<"$g" || return 1; done; return 0; }   # a node (nID), an edge's ends (eA>B) or its connector (/wire)
_sws_one_way() {  # _sws_one_way <generator's game> <light's game>: the generator placed by the first, the light and the wire by the second (the VM's go through requests)
	local gen=$1 lit=$2 out g l w gc lc r label
	label="sharedws ($gen generator, $lit light)"
	win_mark sws host; win_mark sws vm
	if [ "$gen" = host ]; then
		out=$(host "wsreport place $SWS_GENERATOR $SANCT_BENCH"); g=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
		if [ -z "$g" ]; then fail "$label: the generator is placed" "$out"; return 1; fi
		_sws_placed host "$g"
		sleep 2; out=$(host "wsfinish $g $SANCT_BENCH")
		case "$out" in finished*) pass "$label: the host's finish ran its steps on the generator" "$(cut -c1-160 <<<"$out")";; *) fail "$label: the host's finish ran its steps on the generator" "$out"; return 1;; esac
	else
		g=$(_sws_build_vm "$SWS_GENERATOR" "$label"); [ -z "$g" ] && return 1
		if wait_for 20 _sws_built host "$SWS_GENERATOR"; then pass "$label: the host built the VM's generator for it" "$(grep -oE "WorkshopSync: [0-9A-F]{8} \(base $SWS_GENERATOR\) built for player [0-9]+[^;]*" "$A_OUT/windelta-sws-host.txt" | head -1 | cut -c1-160)"
		else fail "$label: the host built the VM's generator for it" "$(grep -E 'WorkshopSync: (player|request|our)' "$A_OUT/windelta-sws-host.txt" "$(win_read sws vm)" | tail -3 | tr '\n' '|')"; return 1; fi
		g=$(grep -oE "WorkshopSync: [0-9A-F]{8} \(base $SWS_GENERATOR\) built" "$A_OUT/windelta-sws-host.txt" | head -1 | cut -d' ' -f2); _sws_placed host "$g"  # the host's piece
	fi
	sleep 3
	if [ "$lit" = host ]; then
		out=$(host "wsreport place $SWS_LIGHT $SANCT_BENCH"); l=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
		if [ -z "$l" ]; then fail "$label: the light is placed" "$out"; return 1; fi
		_sws_placed host "$l"
		sleep 2; host "wsfinish $l $SANCT_BENCH" >/dev/null
		gc=$g; lc=$l
	else
		# the VM's light is the construction light: the host strings a friend's wire only between pieces a wire can end on (the
		# bulb takes none), and it stands beside the generator, out of the VM's line to the pieces it aims at later (_sws_build_vm_beside)
		local vgc; wait_for 15 _sws_has_copy vm "$g"; vgc=$(_sws_copy_of vm "$g")
		[ -z "$vgc" ] && { fail "$label: set-up: the VM has its copy of the host's generator" "$(vm 'wspower copies' | cut -c1-160)"; return 1; }
		l=$(_sws_build_vm_beside "$SWS_WIRED_LIGHT" "$vgc" 150 "$label"); [ -z "$l" ] && return 1
		if wait_for 20 _sws_built host "$SWS_WIRED_LIGHT"; then pass "$label: the host built the VM's light for it" "host log: built for player"; else fail "$label: the host built the VM's light for it" "no 'built for player' line for base $SWS_WIRED_LIGHT on the host"; return 1; fi
		if wait_for 10 _sws_answered vm; then pass "$label: the VM's request was answered applied and its light adopted as the host's copy" "$(grep -oE 'WorkshopSync: our [0-9A-F]{8} .* is the host.s piece now[^;]*' "$A_OUT/windelta-sws-vm.txt" | head -1 | cut -c1-160)"
		else fail "$label: the VM's request was answered applied and its light adopted as the host's copy" "$(grep -E 'WorkshopSync: request' "$A_OUT/windelta-sws-vm.txt" | tail -2 | tr '\n' '|')"; return 1; fi
		lc=$(grep -oE "WorkshopSync: [0-9A-F]{8} \(base $SWS_WIRED_LIGHT\) built" "$A_OUT/windelta-sws-host.txt" | head -1 | cut -d' ' -f2); _sws_placed host "$lc"  # the host's light
		gc=$g
	fi
	sleep 3
	# the wire: from the generator to the light, by whoever placed the light (the VM wires copy to copy: a request)
	if [ "$lit" = host ]; then out=$(host "wswire $gc $lc"); else
		local vg vl; vg=$(_sws_copy_of vm "$gc"); vl=$(_sws_copy_of vm "$lc")
		if [ -z "$vg" ] || [ -z "$vl" ]; then fail "$label: the VM has copies of both the host's pieces" "generator copy '$vg' light copy '$vl'; $(vm 'wspower copies' | cut -c1-160)"; return 1; fi
		win_mark sws host; win_mark sws vm
		out=$(vm "wswire $vg $vl")
	fi
	case "$out" in wire*|requested*) pass "$label: the wire is strung" "$out";; *) fail "$label: the wire is strung" "$out"; return 1;; esac
	if [ "$lit" = vm ]; then
		if wait_for 15 _sws_answered vm; then pass "$label: the host strung the VM's wire between its pieces" "$(grep -oE 'WorkshopSync: request [0-9]+ \(wire\)[^;]*' "$A_OUT/windelta-sws-vm.txt" | head -1)"
		else fail "$label: the host strung the VM's wire between its pieces" "$(grep -E 'WorkshopSync: (request|player)' "$A_OUT/windelta-sws-host.txt" "$A_OUT/windelta-sws-vm.txt" | tail -3 | tr '\n' '|')"; return 1; fi
	fi
	sleep 5
	w=$(host "wsinfo $lc" | grep -oE 'power=[0-9A-F]{8}' | head -1 | cut -d= -f2)
	if _sws_grid_has host "$gc" "$lc" ${w:+"$w"}; then pass "$label: the host's grid holds the generator, the light and the wire" "$(host "wsgrid $SANCT_BENCH" | cut -c1-160)"
	else fail "$label: the host's grid holds the generator, the light and the wire" "generator $gc light $lc wire '$w': $(host "wsgrid $SANCT_BENCH") | generator: $(host "wsinfo $gc") | light: $(host "wsinfo $lc") | wire: $(host "wsinfo ${w:-0}")"; fi
	if wait_for 20 _sws_host_powered "$lc"; then pass "$label: the host's light is powered (within 20 s of the wire)" "powered=yes"; else fail "$label: the host's light is powered (within 20 s of the wire)" "$(host "wsinfo $lc" | cut -c1-160)"; fi
	local vl2; vl2=$(_sws_copy_of vm "$lc")
	SWS_VG=$(_sws_copy_of vm "$gc"); SWS_VL=$vl2; SWS_HG=$gc   # the VM's copies, for the real-hook checks after, and the host's generator (the radiator check)
	if [ -n "$vl2" ] && wait_for 10 _sws_lit vm "$vl2"; then pass "$label: the VM's copy of the light shows the host's power and is lit" "$(vm "wsinfo $vl2" | grep -oE 'blocked=[a-z]+|builder=[0-9]+|theirPower=[a-z]+|lit=[a-z]+' | tr '\n' ' ')"
	else fail "$label: the VM's copy of the light shows the host's power and is lit" "copy '$vl2': $(vm "wsinfo ${vl2:-0}" | cut -c1-200)"; fi
	expect_eq "$label: the VM's copy of the light is out of workshop mode's reach (blocked)" "$(field "$(vm "wsinfo ${vl2:-0}")" blocked)" true
	_sws_grid_clean vm "$label: the VM's grids name no copy and no runtime reference"
	_sws_audit_clean vm "$label: the VM's audit finds nothing tied in"
	return 0
}
# A friend's wire to a radiator with an unwired light already in its range: the light powered on both games. The host's game
# strings a friend's wire without PowerUtils::UpdateMovingWirelessItem on its ends (SharedWorkshop.h, WireStep), whose radiator
# half connects a radiator's receivers in range; this holds that nothing is lost without it (MakeReferenceStatic connects them
# as each piece is placed, and AddConnection carries them into the merged grid). If it fails, the fix is that call on radiator
# ends alone, never on receivers. Set-up by console, said so: the host's pylon and light are made where wsreport place puts
# them, moved by setpos (the pylon 300 units to the side of the step's generator, so it can't snap to it; the light 100 units
# from the pylon, well inside its 500-unit PowerRadiation, read from Fallout4.esm), then reported and placed with the engine's
# steps (wsfinish), the pylon first. The control: before the wire the host's light is unpowered (no other power reaches that
# spot). Then the VM wires the generator's copy to the pylon's copy (a request the host strings). Asserted: the host's light is
# powered within 20 s, and the VM's copy of it shows the host's power and is lit.
SWS_RADIATOR=${SWS_RADIATOR:-0015D76F}   # WorkshopPowerPylon01 (PowerRadiation 500); 000C01C6 is the wall conduit (WorkshopPowerConnectorWall01)
_sws_host_piece_at() {  # _sws_host_piece_at <base> <x> <y> <z> <label>: a host piece made, moved there, reported and finished; prints its reference (a FAIL goes to stderr, out of the caller's $(...))
	local out r; out=$(host "wsreport place $1 $SANCT_BENCH quiet"); r=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
	[ -z "$r" ] && { fail "$5: set-up: the host makes a piece of base $1" "$out" >&2; echo ""; return 1; }
	_sws_placed host "$r"
	out=$(host "setpos $r $2 $3 $4"); case "$out" in error*) fail "$5: set-up: the host's $r is moved beside the generator" "$out" >&2; echo ""; return 1;; esac
	host "wsreport $r $SANCT_BENCH" >/dev/null; sleep 2
	out=$(host "wsfinish $r $SANCT_BENCH")
	case "$out" in finished*) ;; *) fail "$5: set-up: the host's finish ran on its $r" "$out" >&2; echo ""; return 1;; esac
	echo "$r"
}
_sws_radiator() {  # _sws_radiator <the host's generator> <its copy on the VM>
	local gen=$1 vg=$2 label="sharedws (VM wire to a pylon, light in its range)" i x y z p l vp vl out
	if [ -z "$gen" ] || [ -z "$vg" ]; then fail "$label: set-up: the host's generator and its VM copy are known" "generator '$gen' copy '$vg'"; return 1; fi
	i=$(host "refinfo $gen"); x=$(field "$i" x); y=$(field "$i" y); z=$(field "$i" z)
	if [ -z "$x" ] || [ -z "$y" ] || [ -z "$z" ]; then fail "$label: set-up: the generator's position" "$(cut -c1-160 <<<"$i")"; return 1; fi
	win_mark sws host; win_mark sws vm
	p=$(_sws_host_piece_at "$SWS_RADIATOR" "$(awk -v a="$x" 'BEGIN{printf "%.1f", a + 300}')" "$y" "$z" "$label"); [ -z "$p" ] && return 1
	sleep 2
	l=$(_sws_host_piece_at "$SWS_LIGHT" "$(awk -v a="$x" 'BEGIN{printf "%.1f", a + 300}')" "$(awk -v a="$y" 'BEGIN{printf "%.1f", a + 100}')" "$z" "$label"); [ -z "$l" ] && return 1
	sleep 3
	if _sws_host_powered "$l"; then fail "$label: the control: before the wire the host's light by the pylon is unpowered" "$(host "wsinfo $l" | cut -c1-160) (another radiator reaches that spot?)"; return 1; fi
	pass "$label: the control: before the wire the host's light by the pylon is unpowered" "pylon $p, light $l: powered=$(field "$(host "wsinfo $l")" powered)"
	wait_for 15 _sws_has_copy vm "$p"; wait_for 15 _sws_has_copy vm "$l"
	vp=$(_sws_copy_of vm "$p"); vl=$(_sws_copy_of vm "$l")
	if [ -z "$vp" ] || [ -z "$vl" ]; then fail "$label: the VM has copies of the host's pylon and light" "pylon copy '$vp' light copy '$vl'; $(vm 'wspower copies' | cut -c1-160)"; return 1; fi
	win_mark sws host; win_mark sws vm
	out=$(vm "wswire $vg $vp")
	case "$out" in requested*) pass "$label: the VM's wire from the generator's copy to the pylon's copy is asked of the host" "$out";; *) fail "$label: the VM's wire from the generator's copy to the pylon's copy is asked of the host" "$out"; return 1;; esac
	if wait_for 15 _sws_answered vm; then pass "$label: the host strung the VM's wire to its pylon" "$(grep -oE 'WorkshopSync: request [0-9]+ \(wire\)[^;]*' "$A_OUT/windelta-sws-vm.txt" | head -1)"
	else fail "$label: the host strung the VM's wire to its pylon" "$(grep -E 'WorkshopSync: (request|player)' "$A_OUT/windelta-sws-host.txt" "$A_OUT/windelta-sws-vm.txt" | tail -3 | tr '\n' '|')"; return 1; fi
	if wait_for 20 _sws_host_powered "$l"; then pass "$label: the host's unwired light in the pylon's range is powered (within 20 s of the wire)" "$(host "wsinfo $l" | grep -oE 'powered=[a-z]+|power=[0-9A-F]+' | tr '\n' ' ')"
	else fail "$label: the host's unwired light in the pylon's range is powered (within 20 s of the wire)" "$(host "wsinfo $l" | cut -c1-160) | $(host "wsgrid $SANCT_BENCH" | cut -c1-200)"; fi
	if wait_for 10 _sws_lit vm "$vl"; then pass "$label: the VM's copy of that light shows the host's power and is lit" "$(vm "wsinfo $vl" | grep -oE 'theirPower=[a-z]+|lit=[a-z]+' | tr '\n' ' ')"
	else fail "$label: the VM's copy of that light shows the host's power and is lit" "copy $vl: $(vm "wsinfo $vl" | cut -c1-200)"; fi
	_sws_grid_clean vm "$label: the VM's grids name no copy and no runtime reference"
	return 0
}
# The real hooks, not the dev commands: workshop mode on the VM, its camera aimed at the middle of a copy's 3D (turnto ... aim),
# a key pressed only once workshop mode highlights that copy with nothing held (dev workshop selected=; the wire's second end: with
# the wire held, once the engine says it may be put down there, canPlace=true), its keys pressed. Workshop mode holds nothing before
# the keys (Cancel put away whatever it held). Attach
# Wire from the generator's copy to the copy of a construction light the VM built (a light bulb takes no wire in any game), up to
# five tries until one is asked: the engine's wire filter admits the copy (the VM's "admitted copy" line), the wire is taken off the
# VM's grid the frame it is strung and asked of the host (the VM's log), the host strings it (answered applied, the light on its
# grid), and the VM's bench budget (wsres av: the WorkshopCurrentTriangles/Draws values) is the same
# before and after (a copy end is never taken out of the budget). Then grab, Scrap, Repair, Assign and Store on a copy: refused
# (the wsmode counters move) and no crash on either machine.
# The keys are the game's own bindings, read from each game's control map at the start of the step (dev `controls workshop`: the
# Workshop and WorkshopAddendum contexts as event=vk). Workshop mode's events are the pad's names, and its menu handler
# (WorkshopMenu's user event handler, 1.11.240) acts on them so: Cancel stores the highlighted piece (when it can be stored: the
# store test is hooked), XButton scraps it (on release), YButton starts a wire at a power connection and, pressed again on the
# other end, strings it, and Accept (or Activate) grabs the highlighted piece, repairs it when it is damaged, and assigns it while
# a settler is commanded: one key for select, repair and assign, the engine choosing by what is highlighted. Each action is
# matched to its event by the exact name; an event with no keyboard binding fails the step. WS_KEY_* (the VM's) and HWS_KEY_* (the
# host's) set in the environment override the maps (said in the report).
SWS_BED=${SWS_BED:-00118F57}       # the workshop sleeping bag
SWS_SETTLER=${SWS_SETTLER:-00020593}   # a settler (LvlWorkshopNPC), made in the VM's game for the bed control
SWS_ACTIONS="SELECT:Accept SCRAP:XButton STORE:Cancel WIRE:YButton REPAIR:Accept ASSIGN:Accept"   # action:event, as the menu handler acts
_sws_key_of() {  # _sws_key_of <event> <map>: that event's virtual key (the number after "event="; an older build's "event=vk/x" too), or ""
	grep -oE "(^| )$1=[0-9A-F]+" <<<"$2" | head -1 | sed 's/.*=//'
}
_sws_keys() {  # _sws_keys <who> [prefix]: <prefix>* (WS_KEY_* by default) from that game's workshop control map; false (and a FAIL naming the event) when one is missing
	local m k v ev var pre=${2:-WS_KEY_} over="" got=""; m=$(dc "$1" "controls workshop"); [ "$pre" = WS_KEY_ ] && SWS_KEYMAP=$m
	for k in $SWS_ACTIONS; do
		ev=${k#*:}; var=$pre${k%%:*}
		if [ -n "${!var}" ]; then over+=" ${k%%:*}"; else
			v=$(_sws_key_of "$ev" "$m")
			if [ -z "$v" ]; then fail "sharedws: set-up: workshop mode's ${k%%:*} key (event $ev) is in the $1's control map" "no keyboard binding for $ev: $(grep -oE '[A-Za-z]+=[0-9A-F]+' <<<"$m" | tr '\n' ' ' | cut -c1-400)"; return 1; fi
			printf -v "$var" '0x%s' "$v"
		fi
		got+="$(tr '[:upper:]' '[:lower:]' <<<"${k%%:*}")=${!var} "
	done
	pass "sharedws: set-up: workshop mode's keys read from the $1's control map" "$got${over:+(set in the environment:$over)}"
}
_sws_budget() { dc "$1" "wsres av $SANCT_BENCH" | grep -oE '(^|; )(triangles|draws)\[[^]]*\]=[^;]*' | sed 's/^; //; s/\[[^]]*\]//' | tr '\n' ' '; }   # "triangles=<values> draws=<values> " of that game's Sanctuary bench
_sws_budget_gt() {  # _sws_budget_gt <after> <before>: the current triangles value grew (the first number after "triangles=")
	local a b; a=$(grep -oE 'triangles=[^ ]*' <<<"$1" | grep -oE '[0-9]+' | head -1); b=$(grep -oE 'triangles=[^ ]*' <<<"$2" | grep -oE '[0-9]+' | head -1)
	is_int "$a" && is_int "$b" && [ "$a" -gt "$b" ]
}
# The host's Sanctuary budget (the bug the budget scope fixes): a VM build must add exactly the piece to the host's bench totals
# whatever the host's workshop mode holds: (a) right after the host loaded a save (workshop mode's totals 0), (b) after the host
# built at another settlement (its totals), (c) in workshop mode at Sanctuary (the live totals). The three deltas must agree (one
# base, one count) and be above 0. Then a VM wire, counted once drawn (wsmode wiresCounted); the engine's scrap test refuses it as
# it refuses every wire (failed=recipe), to wsmode scraptest and to the real Scrap key, so the host scraps its end (a construction
# light the VM built for it, its own count measured first) with the real key: the wire goes with it in both games, and the host's
# budget falls by more than the end's own count (the wire's count went too). Set-up by console, said so: the host's save/load and
# its coc to Red Rocket and back.
_sws_wait_counted() { [ "$(_sws_counter host wiresCounted)" -gt "$1" ]; }
_sws_host_budget_cases() {
	local label="sharedws (host budget)" h0 h1 d1 d2 d3 l w wc0 vg vl i
	# (a) right after a load
	host "console save ${SWS_SAVE}_host" >/dev/null; sleep 8; wait_ingame host 30
	host "console load ${SWS_SAVE}_host" >/dev/null; sleep 20; wait_ingame host 60; sleep 5
	h0=$(_sws_budget host); win_mark sws host
	l=$(_sws_build_vm "$SWS_LIGHT" "$label (after a load)"); [ -z "$l" ] && return 1
	wait_for 20 _sws_built host "$SWS_LIGHT" || { fail "$label (after a load): the host built the VM's light" "no 'built for player' line"; return 1; }
	sleep 3; h1=$(_sws_budget host)
	if _sws_budget_gt "$h1" "$h0"; then pass "$label: after the host loaded a save, a VM build adds to the host's Sanctuary budget (not 0 plus the piece)" "$h0 -> $h1"; else fail "$label: after the host loaded a save, a VM build adds to the host's Sanctuary budget" "$h0 -> $h1"; fi
	d1=$(( $(grep -oE 'triangles=[0-9]+' <<<"$h1" | grep -oE '[0-9]+') - $(grep -oE 'triangles=[0-9]+' <<<"$h0" | grep -oE '[0-9]+') )) 2>/dev/null
	# (b) after building at another settlement: the host's workshop mode at Red Rocket (console placement to get there and back, said here)
	host "console coc RedRocketExt" >/dev/null; sleep 14; wait_ingame host 30
	if _ws_enter host; then sleep 3; _ws_exit host; else fail "$label (another settlement): set-up: the host's workshop mode at Red Rocket (the case needs its totals loaded there)" "$(host workshop) | $(host menus | grep -oE 'open=[^ ]*' | cut -c1-100)"; host "console coc SanctuaryExt" >/dev/null; sleep 14; wait_ingame host 30; return 1; fi
	host "console coc SanctuaryExt" >/dev/null; sleep 14; wait_ingame host 30
	walk host "$label: set-up: the host walks back to Sanctuary's workbench" 0.8 200 $SANCT_BENCH || { pads_off; return 1; }; pads_off
	h0=$(_sws_budget host); win_mark sws host
	l=$(_sws_build_vm "$SWS_LIGHT" "$label (another settlement)"); [ -z "$l" ] && return 1
	wait_for 20 _sws_built host "$SWS_LIGHT" || { fail "$label (another settlement): the host built the VM's light" "no 'built for player' line"; return 1; }
	sleep 3; h1=$(_sws_budget host)
	if _sws_budget_gt "$h1" "$h0"; then pass "$label: after building at Red Rocket, a VM build adds to the host's Sanctuary budget (not Red Rocket's totals)" "$h0 -> $h1"; else fail "$label: after building at Red Rocket, a VM build adds to the host's Sanctuary budget" "$h0 -> $h1"; fi
	d2=$(( $(grep -oE 'triangles=[0-9]+' <<<"$h1" | grep -oE '[0-9]+') - $(grep -oE 'triangles=[0-9]+' <<<"$h0" | grep -oE '[0-9]+') )) 2>/dev/null
	# (c) in workshop mode at Sanctuary
	if ! _ws_enter host; then fail "$label (in workshop mode): set-up: the host enters workshop mode at Sanctuary" "$(host workshop)"; return 1; fi
	h0=$(_sws_budget host); win_mark sws host
	l=$(_sws_build_vm "$SWS_LIGHT" "$label (in workshop mode)"); [ -z "$l" ] && { _ws_exit host; return 1; }
	wait_for 20 _sws_built host "$SWS_LIGHT" || { fail "$label (in workshop mode): the host built the VM's light" "no 'built for player' line"; _ws_exit host; return 1; }
	sleep 3; h1=$(_sws_budget host); _ws_exit host
	if _sws_budget_gt "$h1" "$h0"; then pass "$label: in workshop mode at Sanctuary, a VM build adds to the host's live budget" "$h0 -> $h1"; else fail "$label: in workshop mode at Sanctuary, a VM build adds to the host's live budget" "$h0 -> $h1"; fi
	d3=$(( $(grep -oE 'triangles=[0-9]+' <<<"$h1" | grep -oE '[0-9]+') - $(grep -oE 'triangles=[0-9]+' <<<"$h0" | grep -oE '[0-9]+') )) 2>/dev/null
	if is_int "$d1" && [ "$d1" -gt 0 ] && [ "$d1" = "$d2" ] && [ "$d2" = "$d3" ]; then pass "$label: the three builds added the same count to the host's budget" "triangles +$d1 each"; else fail "$label: the three builds added the same count to the host's budget" "deltas $d1 / $d2 / $d3"; fi
	# a VM wire between the host's copies, counted once drawn; the engine refuses to scrap it (no wire is scrapped by itself), so it
	# goes with its end. The end is a construction light (a wire end in any game: the host strings a friend's wire only between such
	# pieces, and the engine looks for wires only on them when one is scrapped), built north-west of the generator where the real
	# hooks' one stood (scrapped; _sws_build_vm_off: south of the generator a static of the world's stood between the host and its
	# light, 2026-11-11 and 12, and took every aim); its own count (dl) read from the host's budget before the wire, so that the
	# scrap is seen to take the wire's count as well as its own.
	vg=$(_sws_copy_of vm "$(grep -oE "WorkshopSync: [0-9A-F]{8} \(base $SWS_GENERATOR\) built" "$A_OUT/windelta-sws-host.txt" "$A_OUT"/winmark-sws-host/0.log 2>/dev/null | head -1 | cut -d' ' -f2)")
	[ -z "$vg" ] && vg=$SWS_VG
	[ -n "$SWS_RH_WH" ] && host "wsscrap $SWS_RH_WH" >/dev/null   # the real hooks' construction light, had that step left it standing
	local hl hA hB dl dw
	hA=$(_sws_budget host); win_mark sws host; win_mark sws vm
	l=$(_sws_build_vm_off "$SWS_WIRED_LIGHT" "$vg" $SWS_WIRE_END_OFF "$label (wire's end)"); [ -z "$l" ] && return 1
	wait_for 20 _sws_built host "$SWS_WIRED_LIGHT" || { fail "$label (wire's end): the host built the VM's construction light" "no 'built for player' line for base $SWS_WIRED_LIGHT"; return 1; }
	# the VM's light was adopted as its copy of the host's piece: found by the host's id (the "built for player" line), not the VM's own
	hl=$(grep -oE "WorkshopSync: [0-9A-F]{8} \(base $SWS_WIRED_LIGHT\) built" "$(win_read sws host)" | tail -1 | cut -d' ' -f2)
	wait_for 15 _sws_has_copy vm "$hl"; vl=$(_sws_copy_of vm "$hl")
	if [ -z "$vg" ] || [ -z "$vl" ]; then fail "$label (wire): the VM has copies of the generator and the construction light" "generator '$vg' light '$vl' (the host's light '$hl')"; return 1; fi
	sleep 3; hB=$(_sws_budget host); dl=$(_sws_dtri "$hB" "$hA")
	h0=$hB; wc0=$(_sws_counter host wiresCounted); win_mark sws host
	vm "wswire $vg $vl" >/dev/null
	if wait_for 20 _sws_wait_counted "$wc0"; then pass "$label (wire): the host's wire for the VM was counted once drawn (the budget's AddObject added to the totals)" "wiresCounted $wc0 -> $(_sws_counter host wiresCounted); $(grep -oE 'WorkshopSync: our wire [0-9A-F]{8} for a friend counted[^;]*' "$(win_read sws host)" | tail -1 | cut -c1-220)"
	else fail "$label (wire): the host's wire for the VM was counted once drawn (the budget's AddObject added to the totals)" "wiresCounted stayed $wc0, wiresUncounted=$(_sws_counter host wiresUncounted); $(grep -oE 'WorkshopSync: our wire [0-9A-F]{8} for a friend (added nothing|has no 3D)[^;]*' "$(win_read sws host)" | tail -1 | cut -c1-260)"; return 1; fi
	w=$(grep -oE 'request [0-9]+ \(wire\)[^;]*made [0-9A-F]{8}' "$(win_read sws host)" | grep -oE '[0-9A-F]{8}$' | head -1)
	[ -z "$w" ] && { fail "$label (wire): the host's log names the wire it made" "$(grep -E 'request .*wire' "$A_OUT/windelta-sws-host.txt" | tail -1)"; return 1; }
	h1=$(_sws_budget host)
	_sws_budget_gt "$h1" "$h0" && pass "$label (wire): the wire added to the host's budget" "$h0 -> $h1" || fail "$label (wire): the wire added to the host's budget" "$h0 -> $h1"
	if ! _ws_enter host; then fail "$label (wire): set-up: the host enters workshop mode to scrap the wire" "$(host workshop)"; return 1; fi
	# workshop mode may come back holding a new piece's ghost (the build menu's last item, as the host-busy step left it): the pick
	# never runs while one is held, so Cancel puts it away first
	if ! _sws_put_away host "$label (wire)"; then _ws_exit host; return 1; fi
	# No wire is scrapped by itself in any game: WorkshopCanScrapRef (1.11.240, read whole) passes a piece whose base a recipe makes
	# (inGameFormFlags bit 0, set only by BGSConstructibleObject::InitItemImpl), and no recipe makes the wire (BlackWireSpline01), a
	# player's own wire included. So the host's wire for the VM is refused by that test at that field (wsmode scraptest:
	# failed=recipe), and the real Scrap key reaches the test (past Workshop::RequestScrapPlacementItem's gate, scrapKeys +1) and is
	# refused there: no ExamineConfirmMenu, the host's "Scrap key on" line says failed=recipe. The wire then goes the way a player's
	# does, with an end: the host scraps its light (the wire's end) with the real key and confirms; the wire must go with it, in the
	# host's game and the VM's, and the host's budget fall below what it was with the wire.
	local st sk0 sk1 vw0 h2
	st=$(dc host "wsmode scraptest $w"); sk0=$(_sws_counter host scrapKeys)
	if grep -q 'engine=false' <<<"$st" && grep -q 'failed=recipe' <<<"$st"; then pass "$label (wire): the engine's scrap test refuses the host's wire for want of a recipe, as it refuses every wire" "$st"
	else fail "$label (wire): the engine's scrap test refuses the host's wire for want of a recipe, as it refuses every wire" "$st"; fi
	if _sws_aim_press host "$w" "$HWS_KEY_SCRAP" select "$label (wire)"; then
		sleep 1; sk1=$(_sws_counter host scrapKeys)
		if _menu_open host ExamineConfirmMenu; then
			fail "$label (wire): the host's real Scrap key on its wire is refused by the engine's scrap test (failed=recipe, no confirmation)" "the confirmation opened: pressed with $SWS_LAST_PRESS; cancelled"
			dc host "menu ExamineConfirmMenu cancel" >/dev/null; sleep 1
		elif [ "${sk1:-0}" -gt "${sk0:-0}" ] 2>/dev/null && grep -qE "Scrap key on $w: the engine's scrap test said no \(failed=recipe" "$(win_read sws host)"; then
			pass "$label (wire): the host's real Scrap key on its wire is refused by the engine's scrap test (failed=recipe, no confirmation)" "scrapKeys $sk0 -> $sk1; $(grep -oE "WorkshopSync: workshop mode's Scrap key on $w[^;]*" "$(win_read sws host)" | tail -1 | cut -c1-260)"
		else
			fail "$label (wire): the host's real Scrap key on its wire is refused by the engine's scrap test (failed=recipe, no confirmation)" "scrapKeys $sk0 -> $sk1 ($( [ "${sk1:-0}" -gt "${sk0:-0}" ] 2>/dev/null && echo "reached the test: $(grep -oE "WorkshopSync: workshop mode's Scrap key on $w[^;]*" "$(win_read sws host)" | tail -1 | cut -c1-260)" || echo "never reached the test: the gate, the menu, or the key"); pressed with $SWS_LAST_PRESS"
		fi
	fi
	# the vanilla way: the wire's end scrapped, with the real key and the confirmation's Accept (or E)
	local confirmed=false; vw0=$(_sws_copy_of vm "$w")
	if ! _sws_put_away host "$label (wire's end)"; then _ws_exit host; return 1; fi
	if _sws_aim_press host "$hl" "$HWS_KEY_SCRAP" select "$label (wire's end)"; then
		if wait_for 6 _menu_open host ExamineConfirmMenu; then
			pass "$label (wire's end): the host's Scrap on its light, the wire's end, asks to confirm (ExamineConfirmMenu)" "pressed with $SWS_LAST_PRESS"
			local how=Accept kr
			kr=$(dc host "key $HWS_KEY_SELECT 80"); sleep 1
			if ! wait_for 5 _menu_closed host ExamineConfirmMenu; then
				how=Activate; kr="$kr; then $(dc host "key 45 80")"   # E, the other confirm key, pressed only while the confirmation is still up
				wait_for 5 _menu_closed host ExamineConfirmMenu
			fi
			if _menu_open host ExamineConfirmMenu; then fail "$label (wire's end): set-up: the host confirms its scrap" "the confirmation stayed open after Accept ($HWS_KEY_SELECT) and E: $kr; $(host menus | grep -oE 'open=[^ ]*')"; dc host "menu ExamineConfirmMenu cancel" >/dev/null; sleep 1
			else confirmed=true; pass "$label (wire's end): set-up: the host confirms its scrap" "with $how: $kr"; fi
		else
			fail "$label (wire's end): the host's Scrap on its light, the wire's end, asks to confirm (ExamineConfirmMenu)" "never opened: pressed with $SWS_LAST_PRESS; now $(host menus | grep -oE 'open=[^ ]*'); $(grep -oE "WorkshopSync: workshop mode's Scrap key on $hl[^;]*" "$(win_read sws host)" | tail -1 | cut -c1-260)"
		fi
	fi
	sleep 2; wait_for 8 _no_msgbox host; _ws_exit host; sleep 2
	# no confirmed scrap (the aim at the light never reached it, or the confirmation stayed up): the checks of what the scrap takes
	# with it would only fail after the set-up FAIL above (2026-11-11: three of them did, on an aim that never reached the light)
	$confirmed || return 1
	h2=$(_sws_budget host)
	local ri gone drop
	ri=$(host "refinfo $w")
	case "$ri" in error*) gone=true;; *) gone=$(field "$ri" deleted);; esac
	if [ "$gone" = true ]; then pass "$label (wire): its end scrapped by the host, the wire is gone with it" "$(cut -c1-120 <<<"$ri")"
	else fail "$label (wire): its end scrapped by the host, the wire is gone with it" "the host's wire $w: $(cut -c1-160 <<<"$ri"); the host's grids: $(host "wsgrid $SANCT_BENCH" | cut -c1-200)"; fi
	if [ -n "$vw0" ]; then
		if wait_for 10 _sws_no_copy vm "$w"; then pass "$label (wire): ... and the VM's copy of the wire with it" "the VM's $vw0 no longer a copy of $w"; else fail "$label (wire): ... and the VM's copy of the wire with it" "the VM still holds $vw0 as the copy of $w, or did not answer: $(vm "refinfo $vw0" | cut -c1-160)"; fi
	fi
	# the scrap must take the wire's count with its end's: the budget falls by more than the end's own count (dl, measured when it
	# was built), not merely below its value with the wire (the end alone does that)
	drop=$(_sws_dtri "$h1" "$h2"); dw=$(_sws_dtri "$h1" "$h0")
	if is_int "$dl" && [ "$dl" -gt 0 ] && is_int "$drop" && [ "$drop" -gt "$dl" ]; then
		pass "$label (wire): with the wire and its end scrapped, the host's budget fell by more than the end's own count (the wire's went too)" "triangles -$drop: the end's own $dl, the wire's $dw; $h1 -> $h2 (before the end and the wire: $(_sws_tri "$hA"))"
	elif is_int "$dl" && [ "$drop" = "$dl" ]; then
		fail "$label (wire): with the wire and its end scrapped, the host's budget fell by more than the end's own count (the wire's went too)" "only the end's own count left (triangles -$drop): the wire's $dw stayed, the wire $( [ "$gone" = true ] && echo "deleted all the same (its count never taken off)" || echo "still there"); $h1 -> $h2"
	else
		fail "$label (wire): with the wire and its end scrapped, the host's budget fell by more than the end's own count (the wire's went too)" "triangles -$drop, the end's own '$dl', the wire's '$dw'; $h1 -> $h2"
	fi
	_sws_grid_clean vm "$label (wire): after the host scrapped the wire's end, the VM's grids name no copy and no runtime reference"
}
_sws_tri() { grep -oE 'triangles=[0-9]+' <<<"$1" | grep -oE '[0-9]+' | head -1; }   # the current triangles value of a _sws_budget line
_sws_dtri() { local a b; a=$(_sws_tri "$1"); b=$(_sws_tri "$2"); is_int "$a" && is_int "$b" && echo $(( a - b )); }   # the triangles of one _sws_budget line less another's, or ""
_sws_counter() { field "$(dc "$1" wsmode)" "$2"; }
_sws_offthread() { field "$(dc "$1" 'net status')" offThread; }   # that game's off-main-thread count (MainThread::OffThread counts every call; its log names only a run's first)
_sws_same_thread() {  # _sws_same_thread <who> <count before> <check>: passes only when the count was read before and is the same now
	local now; now=$(_sws_offthread "$1")
	if is_int "$2" && [ "$now" = "$2" ]; then pass "$3" "offThread=$now"; else fail "$3" "offThread '$2' -> '$now'"; fi
}
_sws_wire_held() { local w; w=$(dc "$1" workshop); [ "$(field "$w" heldBase)" = "$SWS_WIRE" ] && [ "$(field "$w" newItem)" = true ]; }   # that game holds a new wire (Attach Wire's first end)
SWS_WIRE=${SWS_WIRE:-0001D971}   # the workshop's power wire (what Attach Wire strings)
_menu_open() { dc "$1" menus | grep -qE "open=([^ ]*,)?$2:"; }
_menu_closed() { ! _menu_open "$@"; }
_sws_fields() { grep -oE '(^| )(held|heldBase|newItem|selected|editing|canPlace|placeStatus|wireStatus|wireFrom|wireTarget|scrapGate)=[^ ]*' <<<"$1" | tr -d ' ' | tr '\n' ' '; }   # dev workshop's fields, on one line
# What workshop mode holds decides what can be picked. While it holds a piece (a new piece's ghost, a wire being strung, or a
# grabbed piece: dev workshop's held=) the general pick never runs (Workshop::UpdateForegroundReferenceJob, 1.11.240, skips it while
# the new-item byte is 0 or an edit is active), so nothing is highlighted and selected= is the held piece itself; only a held wire's
# own pick (its second end) runs. _sws_holding <who>: that game holds a piece.
_sws_holding() { local h; h=$(field "$(dc "$1" workshop)" held); [ -n "$h" ] && [ "$h" != 00000000 ]; }
# _sws_put_away <who> <label>: Cancel (that game's Store key) until workshop mode holds nothing, one press at a time and only while
# something is held (Cancel with nothing held stores the highlighted piece, or leaves workshop mode), at most four presses. A loud
# set-up FAIL, and false, if it still holds something or has left workshop mode.
_sws_put_away() {
	local who=$1 label=$2 vk i w0
	if [ "$who" = host ]; then vk=$HWS_KEY_STORE; else vk=$WS_KEY_STORE; fi
	w0=$(dc "$who" workshop)
	for i in 1 2 3 4; do _sws_holding "$who" || break; dc "$who" "key $vk 80" >/dev/null; sleep 1.5; done
	if _sws_holding "$who" || ! _in_workshop "$who" true; then
		fail "$label: set-up: $who's workshop mode holds nothing (Cancel put away what it held)" "before: $(_sws_fields "$w0"); after $i Cancel(s) ($vk): $(dc "$who" workshop | cut -c1-200)"
		return 1
	fi
	case "$(field "$w0" held)" in ""|00000000) ;; *) pass "$label: set-up: $who's workshop mode holds nothing (Cancel put away what it held)" "put away: $(_sws_fields "$w0")";; esac
	return 0
}
# _sws_aim_press <who> <ref> <vk> [select|wire] [label]: points that game's camera at the middle of the reference's 3D (turnto <ref>
# <ms> aim <dz> <side>: from the eye workshop mode's pick casts its ray from), a few heights tried within the piece's own bound (tenths
# of the radius turnto reports, so a small light is tried on itself, not above and below it), then its sides (a piece in front of
# its middle takes the pick, as it would a player's: the pick highlights the nearest piece its ray meets), until workshop mode has
# it, then presses the key. "select" (the default): with nothing held, workshop mode's highlighted piece (dev workshop's selected=) is the reference,
# on two reads 0.4 s apart (a highlight met only while the camera swept past is no aim). "wire": with the wire held (heldBase= the
# wire, newItem=true), the engine says the wire may be put down now (canPlace=true, Workshop::IsSplineValid's verdict: its wire pick
# found a second end it accepts), twice 0.4 s apart. Never pressed on whatever happens to be under the crosshair: holding the wrong
# thing, a turn the game refused, or a reference never reached is a loud set-up failure, and false. The failure leads with where the
# reference stood from the last ray of the pick that runs in that state (radius, miss: how far its centre is off the ray, inCast:
# whether the cast met its body, and how old that ray is): "select", the general pick's (it runs only while nothing is held);
# "wire", the wire pick's (it runs instead while a wire is held: reached=, whether its search met the reference, target=, whether
# its filter took it as the second end) with the engine's spline status (wireStatus=: no-end, blocked, too-long, already-wired)
# and the wire's ends; then that pick's whole last run. SWS_LAST_PRESS says what workshop mode had just before the key, and the
# key's answer (front=: whether the game was in front to take it).
SWS_LAST_PRESS=""
_sws_aim_press() {
	local who=$1 ref=$2 vk=$3 how=${4:-select} label=${5:-sharedws (real hooks)} t dz ds f r w w2 pk ok="" ago where c0 c1 nn
	w=$(dc "$who" workshop)
	if [ "$how" = wire ]; then
		if [ "$(field "$w" heldBase)" != "$SWS_WIRE" ] || [ "$(field "$w" newItem)" != true ]; then
			fail "$label: set-up: $who holds the wire it started before aiming at $ref" "$(_sws_fields "$w")"; return 1
		fi
	else
		case "$(field "$w" held)" in ""|00000000) ;; *)
			fail "$label: set-up: $who's workshop mode holds nothing before aiming at $ref (the general pick runs only then)" "$(_sws_fields "$w")"; return 1;;
		esac
	fi
	c0=$(_sws_counter "$who" picksAdmitted)
	dc "$who" "wsmode pick $ref" >/dev/null
	t=$(dc "$who" "turnto $ref 500 aim 0")
	r=$(grep -oE 'radius=[0-9]+' <<<"$t" | head -1 | cut -d= -f2); { is_int "$r" && [ "$r" -gt 0 ]; } || r=30
	# The aims, as "dz:side" units from the bound's centre: its middle, heights and sides within the bound (tenths of its radius); then
	# heights over the piece's own origin, where its stand meets the ground and a body is sure to be. A bound's centre can be air (the
	# construction light's, between its legs and its lamp): 2026-11-11 every aim at FF001203 in the bound missed its bodies
	# (inCast=false, miss=123 of radius 190). The origin's heights are read from refinfo's z and the pick's centre.
	# 2026-11-12, the construction light (dev nodes): its bound (radius 190) is mostly its beam's cone, a shape with no body that
	# reaches 100 and more units out in front of the stand; the stand, the one body a ray can meet, has a bound of radius 75 whose
	# centre is 106 units from the whole bound's, so no aim around the whole bound's centre crossed it (every run from 2026-11-10:
	# miss=38, inCast=false). So right after the bound's centre the aims go to each node that holds a body a ray meets (not the
	# navmesh cut, layer 49, nor a trigger, 12), at its own middle and heights and sides within its own bound ("aim node <i>":
	# "n<i>/dz:side" below); then the rest of the bound's aims. At most the 4 largest such nodes: an actor's skeleton has a body on
	# each of a score of bones, and 7 aims at each (2-3 s an aim) would make a miss take minutes.
	local aims="0:0 " cz oz h nd ni nr
	nd=$(dc "$who" "nodes $ref" | tr ';' '\n' | grep -oE "#[0-9]+ [a-z]+ '[^']*' c=\([^)]*\) r=[0-9]+ body b[0-9]+:L[0-9]+" | grep -vE ':L(49|12)$' | sed -E 's/^#([0-9]+) .* r=([0-9]+) body.*/\1 \2/' | sort -k2,2nr | head -4)
	while read -r ni nr; do
		is_int "$ni" && is_int "$nr" || continue; [ "$nr" -gt 0 ] || nr=20
		for f in 0:0 -4:0 4:0 -7:0 7:0 0:-5 0:5; do aims+="n$ni/$(( nr * ${f%%:*} / 10 )):$(( nr * ${f#*:} / 10 )) "; done
	done <<<"$nd"
	for f in -4:0 4:0 -7:0 7:0 -2:0 2:0 0:-6 0:6 4:-6 4:6; do aims+="$(( r * ${f%%:*} / 10 )):$(( r * ${f#*:} / 10 )) "; done
	cz=$(dc "$who" "wsmode pick $ref" | grep -oE "$ref: center \([-0-9]+, [-0-9]+, [-0-9]+\)" | head -1 | grep -oE '[-0-9]+\)$' | tr -d ')')
	oz=$(field "$(dc "$who" "refinfo $ref")" z); oz=${oz%%.*}
	if is_int "$cz" && is_int "$oz"; then for h in 15 40 70 110; do aims+="$(( oz + h - cz )):0 "; done; fi
	for f in $aims; do
		nn=""; case "$f" in n*/*) nn=${f%%/*}; nn="node ${nn#n}"; f=${f#*/};; esac   # "n<i>/dz:side": that node's middle
		dz=${f%%:*}; ds=${f#*:}
		[ "$f" = 0:0 ] && [ -z "$nn" ] || t=$(dc "$who" "turnto $ref 500 aim ${nn:+$nn }$dz $ds")
		case "$t" in error*|"") dc "$who" "wsmode pick off" >/dev/null; fail "$label: set-up: $who aims at $ref" "turnto said '$t'"; return 1;; esac
		sleep 1.2
		w=$(dc "$who" workshop)
		if [ "$how" = wire ]; then
			[ "$(field "$w" canPlace)" = true ] && [ "$(field "$w" heldBase)" = "$SWS_WIRE" ] || continue
			sleep 0.4; w2=$(dc "$who" workshop); [ "$(field "$w2" canPlace)" = true ] && { ok=1; break; }
		else
			[ "$(field "$w" selected)" = "$ref" ] || continue
			sleep 0.4; w2=$(dc "$who" workshop); [ "$(field "$w2" selected)" = "$ref" ] && { ok=1; break; }
		fi
	done
	pk=$(dc "$who" "wsmode pick off")
	if [ -z "$ok" ] && [ "$how" = wire ]; then
		# the wire pick's evidence, not the general pick's (which doesn't run while the wire is held: its ray is stale)
		ago=$(grep -oE 'wire run [0-9]+ ms ago' <<<"$pk" | grep -oE '[0-9]+')
		where=$(grep -oE "wire $ref: [^|]*" <<<"$pk" | grep -oE '(radius|along|miss)=[0-9-]+|(inCast|reached|target)=[a-z]+' | tr '\n' ' ')
		c1=$(_sws_counter "$who" picksAdmitted)
		fail "$label: set-up: $who's workshop mode reaches $ref (aimed at its 3D; $how)" "never, heights and sides tried within radius $r and on the nodes with a body (index radius: $(tr '\n' ',' <<<"$nd")): ${where:-no measure of $ref} from the wire pick's last ray, ${ago:-?} ms old$( { is_int "$ago" && [ "$ago" -gt 3000 ]; } && echo ' (stale: the wire pick is not running; no wire held?)'); the engine's verdict: $(grep -oE '(canPlace|placeStatus|wireStatus|wireFrom|wireTarget)=[^ ]*' <<<"$w" | tr '\n' ' ')(wireStatus no-end: no second end its filter took; blocked: its cast along the wire met something; too-long; already-wired); picksAdmitted +$(( ${c1:-0} - ${c0:-0} )) while aiming; last turnto '$(grep -oE 'aim .*' <<<"$t")' | $(grep -oE 'wire pick runs=[^|]*' <<<"$pk" | cut -c1-1200)"
		return 1
	fi
	if [ -z "$ok" ]; then
		ago=$(grep -oE 'last run [0-9]+ ms ago' <<<"$pk" | grep -oE '[0-9]+')
		where="$(grep -oE "$ref: center [^|]*" <<<"$pk" | grep -oE '(radius|along|miss)=[0-9-]+' | tr '\n' ' ')$(grep -oE "$ref: bodies [^|]*" <<<"$pk" | grep -oE 'inCast=[a-z]+')"
		c1=$(_sws_counter "$who" picksAdmitted)
		fail "$label: set-up: $who's workshop mode reaches $ref (aimed at its 3D; $how)" "never, heights and sides tried within radius $r and on the nodes with a body (index radius: $(tr '\n' ',' <<<"$nd")): ${where:-no measure of $ref} from the general pick's last ray, ${ago:-?} ms old$( { is_int "$ago" && [ "$ago" -gt 3000 ]; } && echo ' (stale: the general pick is not running; something held?)'); now $(_sws_fields "$w")picksAdmitted +$(( ${c1:-0} - ${c0:-0} )) while aiming; last turnto '$(grep -oE 'aim .*' <<<"$t")' | $(cut -c1-1400 <<<"$pk")"
		return 1
	fi
	SWS_LAST_PRESS="$(_sws_fields "$w2")aimed ${nn:+at $nn }$dz:$ds; key $vk: $(dc "$who" "key $vk 80")"
	sleep 1.5
}
# _sws_build_vm <base> [label]: a VM build the engine's way, short of the menu: the reference made (wsreport place ... quiet) and
# placed with the engine's own steps (wsfinish: linked, counted in the budget, its scripts started, the placed event), whose event
# hands it over to the host as workshop mode's own placement would. Prints the VM's reference (a FAIL goes to stderr, out of the caller's $(...)).
_sws_build_vm() {
	local out r; out=$(vm "wsreport place $1 $SANCT_BENCH quiet"); r=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
	[ -z "$r" ] && { echo ""; return 1; }
	_sws_placed vm "$r"
	sleep 1.5; out=$(vm "wsfinish $r $SANCT_BENCH")
	case "$out" in finished*) ;; *) fail "${2:-sharedws}: the VM's finish ran on its piece" "$out" >&2; echo ""; return 1;; esac
	echo "$r"
}
# _sws_build_vm_beside <base> <the VM's ref> <units> [label]: _sws_build_vm, but the piece moved first (setpos, console placement)
# to that many units beside that reference, across the VM's line to it: on the side away from the host's player, or toward it when
# <units> is negative (two pieces beside one reference stand apart). _sws_build_vm puts a piece a step in front of the VM, which
# is where the VM looks at every other piece from: a tall piece there (the construction light) stood in front of each of them and
# took workshop mode's pick in both games (the pick highlights the nearest piece its ray meets, as for any player). Prints the
# VM's reference (a FAIL goes to stderr).
_sws_build_vm_beside() {
	local out r i p h x y z px py hx hy at
	i=$(vm "refinfo $2"); p=$(vm pos); h=$(host pos)
	x=$(field "$i" x); y=$(field "$i" y); z=$(field "$i" z); px=$(field "$p" x); py=$(field "$p" y); hx=$(field "$h" x); hy=$(field "$h" y)
	if [ -z "$x" ] || [ -z "$y" ] || [ -z "$z" ] || [ -z "$px" ] || [ -z "$py" ]; then fail "${4:-sharedws}: set-up: where $2 and the VM stand" "$(cut -c1-120 <<<"$i") | $(cut -c1-120 <<<"$p")" >&2; echo ""; return 1; fi
	# the two spots across the line from the VM to the reference: the one farther from the host's player (nearer for a negative
	# distance; either when the host's place is unknown)
	at=$(awk -v x="$x" -v y="$y" -v px="$px" -v py="$py" -v hx="${hx:-$px}" -v hy="${hy:-$py}" -v d="$3" 'BEGIN{
		far = d >= 0; if (d < 0) d = -d
		dx = x - px; dy = y - py; l = sqrt(dx * dx + dy * dy); if (l < 1) l = 1
		rx = x + d * dy / l; ry = y - d * dx / l; lx = x - d * dy / l; ly = y + d * dx / l
		right = (rx - hx) ^ 2 + (ry - hy) ^ 2 >= (lx - hx) ^ 2 + (ly - hy) ^ 2
		if (right == far) printf "%.1f %.1f", rx, ry; else printf "%.1f %.1f", lx, ly }')
	out=$(vm "wsreport place $1 $SANCT_BENCH quiet"); r=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
	[ -z "$r" ] && { fail "${4:-sharedws}: set-up: the VM makes a piece of base $1" "$out" >&2; echo ""; return 1; }
	_sws_placed vm "$r"
	out=$(vm "setpos $r $at $z"); case "$out" in error*|"") fail "${4:-sharedws}: set-up: the VM's $r is moved beside $2" "$out" >&2; echo ""; return 1;; esac
	sleep 1.5; out=$(vm "wsfinish $r $SANCT_BENCH")
	case "$out" in finished*) ;; *) fail "${4:-sharedws}: the VM's finish ran on its piece" "$out" >&2; echo ""; return 1;; esac
	echo "$r"
}
# _sws_build_vm_off <base> <the VM's ref> <dx> <dy> [label]: _sws_build_vm, but the piece moved first (setpos, console placement)
# to that reference's spot plus dx, dy in the world's own axes (not the players': the scene is Sanctuary's, the same each run).
# Prints the VM's reference (a FAIL goes to stderr). The wire's end pieces stand north-west of the generator
# (SWS_WIRE_END_OFF, default "-170 200"): 2026-11-12, dev nodes and a light set on each side of the generator, the VM's real wire
# from the generator's copy was placeable (wireStatus ok) only to the north (-170 200, 130 200); to the south (0 -150, 0 -300,
# -85 -235, -175 -185) the engine said blocked for every aim, and the casts from both players met a static the world holds
# there with no reference (b542 at 100 units from the VM, b2872 at 67-93 from the host): the host's aims at its wire's end there
# (the budget step) met it first too. North-west keeps it clear of the one-way light (150 north of the generator) in both
# players' lines.
SWS_WIRE_END_OFF=${SWS_WIRE_END_OFF:-"-170 200"}
_sws_build_vm_off() {
	local out r i x y z at
	i=$(vm "refinfo $2"); x=$(field "$i" x); y=$(field "$i" y); z=$(field "$i" z)
	if [ -z "$x" ] || [ -z "$y" ] || [ -z "$z" ]; then fail "${5:-sharedws}: set-up: where $2 stands" "$(cut -c1-120 <<<"$i")" >&2; echo ""; return 1; fi
	at=$(awk -v x="$x" -v y="$y" -v dx="$3" -v dy="$4" 'BEGIN{ printf "%.1f %.1f", x + dx, y + dy }')
	out=$(vm "wsreport place $1 $SANCT_BENCH quiet"); r=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
	[ -z "$r" ] && { fail "${5:-sharedws}: set-up: the VM makes a piece of base $1" "$out" >&2; echo ""; return 1; }
	_sws_placed vm "$r"
	out=$(vm "setpos $r $at $z"); case "$out" in error*|"") fail "${5:-sharedws}: set-up: the VM's $r is moved to $2 plus ($3, $4)" "$out" >&2; echo ""; return 1;; esac
	sleep 1.5; out=$(vm "wsfinish $r $SANCT_BENCH")
	case "$out" in finished*) ;; *) fail "${5:-sharedws}: the VM's finish ran on its piece" "$out" >&2; echo ""; return 1;; esac
	echo "$r"
}
_sws_real_hooks() {  # _sws_real_hooks <generator copy on the VM> <light copy on the VM>
	local vg=$1 vl=$2 i b0 b1 asked0 asked1 g0 s0 c0 a0 w misses=0 label="sharedws (real hooks)" we wh vw hw=""
	crash_snapshot
	# The wire's second end must be a piece that takes a wire in any game: the engine's wire filter passes only a reference with the
	# workshopPowerConnection value or the WorkshopPowerConnection keyword, copy or not; a light bulb has neither (no player strings
	# a wire to one). So the VM builds a construction light (WorkshopConstructionLight01, which has the keyword and radiates no power,
	# so nothing connects it to the generator before the wire), a request the host builds; its copy is the second end. The keys are
	# tried on $vl, the copy of the one-way step's construction light (150 units beside the generator, on the side away from the host).
	# This one stands beside the generator 300 units out on the other side (_sws_build_vm_beside, below): at 150 it stood exactly where
	# the one-way step's light stands and the pick took either; a step in front of the VM it stood in front of the generator and took
	# workshop mode's pick from it (and from the host's aims in the budget step). It is scrapped at the end of this step.
	# On the other side of the generator (toward the host, -300): on the same side as the one-way light it stood behind that light's
	# copy from the VM's eye, so the wire's ray to it was blocked by the copy (2026-11-11: FF001203 in the ray, another piece the
	# second end), and it stood in front of that copy for the grab, scrap, repair, assign and store aims after.
	# On that side (south) the engine refused the wire to it at every aim (wireStatus blocked: a static of the world's between), so
	# it stands north-west of the generator now (_sws_build_vm_off, SWS_WIRE_END_OFF), out of the VM's lines to the generator and to
	# the one-way light.
	win_mark sws host; win_mark sws vm
	we=$(_sws_build_vm_off "$SWS_WIRED_LIGHT" "$vg" $SWS_WIRE_END_OFF "$label"); [ -z "$we" ] && return 1
	if ! wait_for 20 _sws_built host "$SWS_WIRED_LIGHT"; then fail "$label: set-up: the host built the VM's construction light" "no 'built for player' line for base $SWS_WIRED_LIGHT on the host"; return 1; fi
	wh=$(grep -oE "WorkshopSync: [0-9A-F]{8} \(base $SWS_WIRED_LIGHT\) built" "$(win_read sws host)" | tail -1 | cut -d' ' -f2); SWS_RH_WH=$wh
	wait_for 15 _sws_has_copy vm "$wh"; vw=$(_sws_copy_of vm "$wh")
	if [ -z "$vw" ]; then fail "$label: set-up: the VM has its copy of the host's construction light" "the host's '$wh'; $(vm 'wspower copies' | cut -c1-200)"; return 1; fi
	_sws_placed host "$wh"
	if ! _ws_enter vm; then fail "$label: the VM enters workshop mode at the host's workbench" "$(vm workshop) | $(vm menus | grep -oE 'open=[^ ]*')"; return 1; fi
	# Workshop mode may open holding a new piece (the build menu's item, or a wire left from before): every check below needs the
	# general pick, which runs only with nothing held, so it is put away first, loudly.
	if ! _sws_put_away vm "$label"; then wait_for 8 _no_msgbox vm; _ws_exit vm; return 1; fi
	b0=$(_sws_budget vm); asked0=$(_sws_counter vm wiresAsked)
	# the main thread's off-thread count (net status offThread=, MainThread::OffThread) in both games before the real wire
	local ot0h ot0v; ot0h=$(field "$(dc host 'net status')" offThread); ot0v=$(field "$(dc vm 'net status')" offThread)
	win_mark sws vm; win_mark sws host
	# Attach Wire as a player does: the generator's copy highlighted, the key (the wire starts there and is held: heldBase= the
	# wire, newItem=true); then the construction light's copy aimed at until the engine says the wire may go there (canPlace), the
	# key again (the wire placed, taken off and asked of the host: wiresAsked +1). The first wire asked ends the loop (a second
	# between the same ends would be one the host strings twice); a wire left in hand is put away before the next try, and two
	# failed tries in a row end it too (the reason is in their FAILs).
	for i in 1 2 3 4 5; do
		_sws_put_away vm "$label (wire $i)" || break
		if ! _sws_aim_press vm "$vg" "$WS_KEY_WIRE" select "$label (wire $i, first end)"; then misses=$((misses + 1)); [ "$misses" -ge 2 ] && break; continue; fi
		wait_for 3 _sws_wire_held vm; w=$(vm workshop)   # the key's wire shows a frame or more after the press (2026-11-11: read at once, wire 2 held nothing yet)
		if [ "$(field "$w" heldBase)" != "$SWS_WIRE" ] || [ "$(field "$w" newItem)" != true ]; then
			fail "$label (wire $i): Attach Wire on the generator's copy starts a wire (held, base $SWS_WIRE)" "now $(_sws_fields "$w")| pressed with $SWS_LAST_PRESS"
			misses=$((misses + 1)); [ "$misses" -ge 2 ] && break; continue
		fi
		a0=$(_sws_counter vm wiresAsked)
		if ! _sws_aim_press vm "$vw" "$WS_KEY_WIRE" wire "$label (wire $i, second end)"; then misses=$((misses + 1)); [ "$misses" -ge 2 ] && break; continue; fi
		sleep 1
		if [ "$(_sws_counter vm wiresAsked)" -gt "$a0" ] 2>/dev/null; then break
		else
			fail "$label (wire $i): the wire, placeable on the construction light's copy, is strung and asked of the host" "wiresAsked $a0 -> $(_sws_counter vm wiresAsked); now $(_sws_fields "$(vm workshop)")| pressed with $SWS_LAST_PRESS; $(grep -E 'WorkshopSync: wire ' "$(win_read sws vm)" | tail -1 | cut -c1-200)"
			misses=$((misses + 1)); [ "$misses" -ge 2 ] && break
		fi
		sleep 1
	done
	if ! _sws_put_away vm "$label (after the wires)"; then wait_for 8 _no_msgbox vm; _ws_exit vm; return 1; fi
	asked1=$(_sws_counter vm wiresAsked)
	if [ "$((asked1 - asked0))" -ge 1 ]; then pass "$label: Attach Wire between two of the host's copies is taken off and asked of the host ($((asked1 - asked0)) of $i tries)" "$(grep -oE 'WorkshopSync: wire [0-9A-F]{8} between copies[^;]*' "$(win_read sws vm)" | head -1 | cut -c1-160)"
	else fail "$label: Attach Wire between two of the host's copies is taken off and asked of the host" "wiresAsked $asked0 -> $asked1 over $i tries (each try's FAIL above says where it stopped); picksAdmitted=$(_sws_counter vm picksAdmitted); $(vm menus | grep -oE 'open=[^ ]*' | cut -c1-120)"; fi
	# the wire filter took the copy as the second end by the engine's own rule (its "admitted copy" line), and the host strung the wire
	# (the filter logs only its first 5 admitted copies a session, and the earlier sections' wires use them up: 2026-11-12, 279
	# admitted and no line for the light; then the wire asked between the light's and the generator's copies is the evidence)
	if grep -qE "wire filter admitted copy $vw " "$(win_read sws vm)"; then pass "$label: the engine's wire filter admitted the construction light's copy as the second end" "$(grep -oE "WorkshopSync: workshop mode.s wire filter admitted copy $vw [^;]*" "$(win_read sws vm)" | head -1 | cut -c1-260)"
	elif grep -qE "WorkshopSync: wire [0-9A-F]{8} between copies of player [0-9]+'s ($wh and [0-9A-F]{8}|[0-9A-F]{8} and $wh) taken off" "$A_OUT/windelta-sws-vm.txt"; then pass "$label: the engine's wire filter admitted the construction light's copy as the second end" "its log lines used up earlier (wireFilterCopyYes=$(_sws_counter vm wireFilterCopyYes)); the wire asked names it: $(grep -oE "WorkshopSync: wire [0-9A-F]{8} between copies[^;]*" "$A_OUT/windelta-sws-vm.txt" | head -1 | cut -c1-160)"
	else fail "$label: the engine's wire filter admitted the construction light's copy as the second end" "no 'admitted copy $vw' line; wsmode: $(vm wsmode | grep -oE 'wire(PickRuns|Targets|FilterCopyYes|FilterCopyNo)=[0-9]+' | tr '\n' ' ')$(grep -oE "WorkshopSync: workshop mode.s wire filter refused copy $vw [^;]*" "$(win_read sws vm)" | head -1 | cut -c1-260)"; fi
	if [ "$((asked1 - asked0))" -ge 1 ]; then
		if wait_for 15 _sws_wire_answered vm; then pass "$label: the host strung the VM's real wire and answered it applied" "$(grep -oE 'WorkshopSync: request [0-9]+ \(wire\)[^;]*' "$(win_read sws vm)" | tail -1 | cut -c1-200)"
		else fail "$label: the host strung the VM's real wire and answered it applied" "$(grep -E 'WorkshopSync: (request|player)' "$(win_read sws host)" "$(win_read sws vm)" | grep -i wire | tail -3 | cut -c1-200 | tr '\n' '|')"; fi
		# wired: the host's grid holds the wire it answered with as an edge's connector (not just the light as a lone node), and the
		# light is powered by it (a construction light radiates and receives no power without one)
		hw=$(grep -oE 'WorkshopSync: request [0-9]+ \(wire\) at workbench [0-9A-F]{8}: applied, the host.s [0-9A-F]{8}' "$(win_read sws vm)" | tail -1 | grep -oE '[0-9A-F]{8}$')
		if [ -n "$hw" ] && wait_for 10 _sws_grid_has host "$wh" "$hw"; then pass "$label: the host's grid holds its construction light, wired (the wire $hw an edge's connector)" "$(host "wsgrid $SANCT_BENCH" | cut -c1-160)"
		else fail "$label: the host's grid holds its construction light, wired (the wire an edge's connector)" "wire '$hw': $(host "wsgrid $SANCT_BENCH") | light: $(host "wsinfo $wh") | wire: $(host "wsinfo ${hw:-0}")"; fi
		if wait_for 20 _sws_host_powered "$wh"; then pass "$label: the host's construction light is powered by the wire" "$(host "wsinfo $wh" | grep -oE 'powered=[a-z]+|power=[0-9A-F]+' | tr '\n' ' ')"
		else fail "$label: the host's construction light is powered by the wire" "light: $(host "wsinfo $wh") | grid: $(host "wsgrid $SANCT_BENCH")"; fi
	fi
	# The real wire's hand-over runs on the main thread in both games: no off-thread call counted across it, and no "[E] MainThread"
	# line in either log since the mark (2026-11-11: the VM's "[E] MainThread: WorkshopSync::HandOver ran off the main thread" as the
	# engine's placed event for a wire strung in real workshop mode came on a workshop job thread). The log line is written once per
	# game run, the count every time: both are read.
	local ot1h ot1v mt
	sleep 2; ot1h=$(field "$(dc host 'net status')" offThread); ot1v=$(field "$(dc vm 'net status')" offThread)
	if is_int "$ot0v" && [ "$ot1v" = "$ot0v" ]; then pass "$label: the real wire ran on the VM's main thread (offThread unchanged across it)" "offThread=$ot1v"
	else fail "$label: the real wire ran on the VM's main thread (offThread unchanged across it)" "offThread '$ot0v' -> '$ot1v'"; fi
	if is_int "$ot0h" && [ "$ot1h" = "$ot0h" ]; then pass "$label: the real wire ran on the host's main thread (offThread unchanged across it)" "offThread=$ot1h"
	else fail "$label: the real wire ran on the host's main thread (offThread unchanged across it)" "offThread '$ot0h' -> '$ot1h'"; fi
	mt=$(grep -h '\[E\] MainThread' "$(win_read sws host)" "$(win_read sws vm)" 2>/dev/null | head -2 | cut -c1-200 | tr '\n' '|')
	if [ -z "$mt" ]; then pass "$label: neither game's log has an [E] MainThread line across the real wire" "none"
	else fail "$label: neither game's log has an [E] MainThread line across the real wire" "$mt"; fi
	# the friend's own grid: the wire it strung is gone from it (taken off in the same frame), its copies on none of its grids
	expect_eq "$label: the VM's bench budget is the same after the wires (no copy end taken out of it)" "$(_sws_budget vm)" "$b0"
	_sws_grid_clean vm "$label: after real wiring, the VM's grids name no copy and no runtime reference"
	# Nothing below means anything unless workshop mode's pick reached the copies (the pick hook admits a copy it is asked
	# about): said loudly, apart from the keys.
	local m; m=$(vm wsmode)
	if [ "$(field "$m" pickRuns)" -ge 1 ] 2>/dev/null; then pass "$label: workshop mode's general pick ran on the VM (hooked at its call)" "pickRuns=$(field "$m" pickRuns)"
	else fail "$label: workshop mode's general pick ran on the VM (hooked at its call)" "pickRuns=$(field "$m" pickRuns) hooks=$(field "$m" hooks): the pick never ran (or its call isn't hooked: the log's 'picks hooked' line)"; fi
	if [ "$(field "$m" pickCopyHits)" -ge 1 ] 2>/dev/null; then pass "$label: the pick's cast hit the host's copies" "pickCopyHits=$(field "$m" pickCopyHits) pickCopyRefused=$(field "$m" pickCopyRefused) filterAdmitted=$(field "$m" filterAdmitted) filterOtherBench=$(field "$m" filterOtherBench) standInsPassed=$(field "$m" standInsPassed)"
	else fail "$label: the pick's cast hit the host's copies" "pickCopyHits=0: the ray never met a copy's body (the log's 'pick watch' lines: each hit and the miss); standInsPassed=$(field "$m" standInsPassed) (the host's stand-in passed by)"; fi
	if [ "$(field "$m" picksAdmitted)" -ge 1 ] 2>/dev/null; then pass "$label: workshop mode's pick reached the host's copies (aimed by turnto; picksAdmitted>0)" "picksAdmitted=$(field "$m" picksAdmitted)"
	else fail "$label: workshop mode's pick reached the host's copies (aimed by turnto; picksAdmitted>0)" "picksAdmitted=$(field "$m" picksAdmitted) pickCopyRefused=$(field "$m" pickCopyRefused): no copy was ever highlighted, so no key below can act on one; $(vm "wsinfo $vl" | grep -oE 'blocked=[a-z]+|builder=[0-9]+' | tr '\n' ' ')$(field "$m" handsOver | sed 's/^/handsOver=/'); $(grep -oE 'WorkshopSync: workshop mode.s pick reached copy [0-9A-F]{8} and its test refused it[^;]*' "$(win_read sws vm)" | head -1 | cut -c1-200)"; fi
	# Both players stand at the bench (the eye inside or behind the host's stand-in): the picks must pass it by as they pass the
	# player, or it hides every piece behind it.
	if [ "$(field "$m" standInsPassed)" -ge 1 ] 2>/dev/null; then pass "$label: workshop mode's picks passed by the host's stand-in (standInsPassed>0)" "standInsPassed=$(field "$m" standInsPassed)"
	else fail "$label: workshop mode's picks passed by the host's stand-in (standInsPassed>0)" "standInsPassed=$(field "$m" standInsPassed): the stand-in never stood in the ray (or the hits' hook never named it the player); the host at$(dc host pos | grep -oE '(^| )[xyz]=[0-9.-]+' | tr -d '\n'), the VM at$(vm pos | grep -oE '(^| )[xyz]=[0-9.-]+' | tr -d '\n')"; fi
	# Each key below is pressed only with the copy highlighted (dev workshop's selected=, nothing held), and each refusal counts one
	# press (the menu's own call for Accept and Store, the scrap request for Scrap: the button hints' every-frame tests count nothing).
	# Scrap and Store follow the host's bSharedWorkshopEdit (SharedWorkshop::OnScrapKey, OnStoreKey): on (the default), the engine's
	# own confirmation (ExamineConfirmMenu) or yes/no box opens for the copy as for the host's own piece, and only its Yes asks the
	# host (sharedws-scrap and sharedws-store answer it Yes); here each is turned down, and nothing may be asked. Off: the key is
	# refused before the engine is asked. (These checks were written before a friend's scrap and store existed, and never ran until
	# the aims reached the copy, 2026-11-12: they still said "refused" for the edit-on default.)
	local edit sa0 sta0; edit=$(field "$(dc host rules)" sharedWorkshopEdit)
	g0=$(_sws_counter vm grabsRefused); s0=$(_sws_counter vm storesRefused); c0=$(_sws_counter vm scrapsRefused)
	sa0=$(_sws_counter vm scrapsAsked); sta0=$(_sws_counter vm storesAsked)
	if _sws_aim_press vm "$vl" "$WS_KEY_SELECT" select "$label (grab)"; then
		expect_eq "$label: Accept (grab) on a copy is refused, once for one press" "$(( $(_sws_counter vm grabsRefused) - g0 ))" 1
		expect_eq "$label: after the refused grab workshop mode holds nothing (held=, not the highlight)" "$(field "$(vm workshop)" held)" 00000000
	fi
	# after each key, anything workshop mode was left holding is put away before the next (or the keys stop, loudly)
	local keys=1
	_sws_put_away vm "$label (after the grab)" || keys=""
	if [ -n "$keys" ] && _sws_aim_press vm "$vl" "$WS_KEY_SCRAP" select "$label (scrap)"; then
		if [ "$edit" = true ]; then
			if wait_for 6 _menu_open vm ExamineConfirmMenu; then
				pass "$label: Scrap on a copy asks to confirm (the engine's ExamineConfirmMenu, the host's bSharedWorkshopEdit on)" "pressed with $SWS_LAST_PRESS; turned down"
				dc vm "menu ExamineConfirmMenu cancel" >/dev/null; sleep 1
			else fail "$label: Scrap on a copy asks to confirm (the engine's ExamineConfirmMenu, the host's bSharedWorkshopEdit on)" "never opened: pressed with $SWS_LAST_PRESS; scrapsRefused +$(( $(_sws_counter vm scrapsRefused) - c0 )); $(vm menus | grep -oE 'open=[^ ]*' | cut -c1-120)"; fi
			expect_eq "$label: Scrap on a copy turned down at the confirmation asks nothing of the host" "$(( $(_sws_counter vm scrapsAsked) - sa0 ))" 0
		else
			expect_eq "$label: Scrap (XButton) on a copy is refused, once for one press (bSharedWorkshopEdit off)" "$(( $(_sws_counter vm scrapsRefused) - c0 ))" 1
			# refused before the engine asks: its scrap confirmation (ExamineConfirmMenu) never opens for a copy
			if _menu_open vm ExamineConfirmMenu; then fail "$label: Scrap on a copy never asks to confirm (refused before ExamineConfirmMenu; bSharedWorkshopEdit off)" "the confirmation opened: pressed with $SWS_LAST_PRESS; cancelled"; dc vm "menu ExamineConfirmMenu cancel" >/dev/null; sleep 1
			else pass "$label: Scrap on a copy never asks to confirm (refused before ExamineConfirmMenu; bSharedWorkshopEdit off)" "$(vm menus | grep -oE 'open=[^ ]*' | cut -c1-120)"; fi
		fi
	fi
	# Repair and Assign are Accept's own path (an undamaged copy with no settler commanded comes to the grab, refused): no crash
	[ -n "$keys" ] && { _sws_put_away vm "$label (after the scrap)" || keys=""; }
	[ -n "$keys" ] && { _sws_aim_press vm "$vl" "$WS_KEY_REPAIR" select "$label (repair)"; _sws_put_away vm "$label (after repair)" || keys=""; }
	[ -n "$keys" ] && { _sws_aim_press vm "$vl" "$WS_KEY_ASSIGN" select "$label (assign)"; sleep 3; _sws_put_away vm "$label (after assign)" || keys=""; }
	# Store last: Cancel with nothing the store test lets go of is workshop mode's own Cancel (it may leave the menu). Judged only
	# when the key was pressed on the highlighted copy.
	if [ -n "$keys" ] && _sws_aim_press vm "$vl" "$WS_KEY_STORE" select "$label (store)"; then
		if [ "$edit" = true ]; then
			if wait_for 6 _sws_msgbox_open vm; then
				pass "$label: Store on a copy asks to confirm (the engine's yes/no box, the host's bSharedWorkshopEdit on)" "pressed with $SWS_LAST_PRESS; turned down"
				dc vm "menu MessageBoxMenu hide" >/dev/null; sleep 1
			else fail "$label: Store on a copy asks to confirm (the engine's yes/no box, the host's bSharedWorkshopEdit on)" "never opened: pressed with $SWS_LAST_PRESS; storesRefused +$(( $(_sws_counter vm storesRefused) - s0 )) (the engine's own store test said no?); $(vm menus | grep -oE 'open=[^ ]*' | cut -c1-120)"; fi
			expect_eq "$label: Store on a copy turned down at the box asks nothing of the host" "$(( $(_sws_counter vm storesAsked) - sta0 ))" 0
		elif [ "$(( $(_sws_counter vm storesRefused) - s0 ))" -ge 1 ]; then pass "$label: Store (Cancel) on a copy is refused (bSharedWorkshopEdit off)" "storesRefused +$(( $(_sws_counter vm storesRefused) - s0 ))"
		else fail "$label: Store (Cancel) on a copy is refused (bSharedWorkshopEdit off)" "storesRefused did not move: pressed with $SWS_LAST_PRESS (map: $(cut -c1-160 <<<"$SWS_KEYMAP"))"; fi
	fi
	expect_eq "$label: the copy still stands after Scrap and Store" "$(field "$(vm "wsinfo $vl")" deleted)" false
	wait_for 8 _no_msgbox vm; _ws_exit vm
	crash_verdict "$label: Repair and Assign on a copy crash neither game"
	# the construction light (and its wire) scrapped, so it stands in front of nothing in the steps after (the host's own pieces:
	# scrapped in the host's game, the VM's copies go with them)
	[ -n "$hw" ] && host "wsscrap $hw" >/dev/null
	host "wsscrap $wh" >/dev/null; SWS_RH_WH=""; sleep 2
	return 0
}
# A VM build while the host is in workshop mode placing a new piece of its own (building together is the point): the host
# builds the VM's all the same, and its own ghost survives it and can still be placed. The host's ghost is workshop mode's own:
# Accept on the build menu (from its first category down to its first item) until workshop mode holds a new piece (dev
# `workshop`: held=, newItem=true); then the VM's build; then the same ghost still held, and Accept puts it down (the reference
# then a placed piece, not deleted, and workshop mode's next ghost another reference). The host needs the first item's components.
# Accept with a piece highlighted grabs that piece instead (newItem=false), and the next Accept would put it down elsewhere: so the
# host looks up at the sky first (turnto the bench at 70 degrees up, nothing to highlight), and an Accept that leaves a piece of
# the scene held (not a new item, or one of the step's own bases) stops the loop and fails the set-up rather than pressing on.
# Once the ghost is held the host looks level again (toward the bench), so the ghost stands on the ground to be put down, and turns
# (mouse) until workshop mode says it may be put down there (dev workshop canPlace=, Workshop::CanPlaceItem: the ghost green): a
# ghost that is red before the VM's build proves nothing about the build. After the build the same ghost must still be placeable
# (canPlace=true) and Accept must put it down; the host's log says what the finish saw of its workshop mode ("finished while our
# workshop mode is open: its placement item unchanged/CHANGED ...").
HWS_GHOST_BASE=${HWS_GHOST_BASE:-00110000}   # the piece the busy host holds as its ghost (_sws_host_busy picks it by base)
_sws_host_ghost() { local w; w=$(dc host workshop); [ "$(field "$w" newItem)" = true ] && [ -n "$(field "$w" held)" ] && [ "$(field "$w" held)" != 00000000 ]; }
_sws_host_grabbed() {  # _sws_host_grabbed: the host's workshop mode holds a piece that isn't a new one from the build menu (a grab of the scene)
	local w h b; w=$(dc host workshop); h=$(field "$w" held); b=$(field "$w" heldBase)
	[ -z "$h" ] || [ "$h" = 00000000 ] && return 1
	[ "$(field "$w" newItem)" != true ] && return 0
	case " $SWS_GENERATOR $SWS_LIGHT $SWS_BED $SWS_RADIATOR " in *" $b "*) return 0;; esac
	return 1
}
_sws_host_busy() {
	local out l i w ghost t label="sharedws (host in workshop mode)"
	if ! _ws_enter host; then fail "$label: the host enters workshop mode" "$(host workshop)"; return 1; fi
	t=$(dc host "turnto $SANCT_BENCH 600 -70"); sleep 1
	case "$t" in error*|"") fail "$label: set-up: the host looks up, away from the scene's pieces" "turnto said '$t'"; _ws_exit host; return 1;; esac
	for i in 1 2 3 4 5; do
		_sws_host_ghost && break
		if _sws_host_grabbed; then
			fail "$label: set-up: the host's Accepts reach the build menu, not a piece of the scene" "Accept grabbed a placed piece: $(host workshop | cut -c1-200)"
			_ws_exit host; return 1
		fi
		dc host "key $HWS_KEY_SELECT 80" >/dev/null; sleep 1.5
	done
	if ! _sws_host_ghost && _sws_host_grabbed; then fail "$label: set-up: the host's Accepts reach the build menu, not a piece of the scene" "Accept grabbed a placed piece: $(host workshop | cut -c1-200)"; _ws_exit host; return 1; fi
	# The ghost picked by its base, not as the menu's first item comes: 2026-11-11 the menu handed an Automatron robot workbench
	# piece (01003ECA) that could stand nowhere there ("never green after six turns"). The menu's next item (the Right arrow, the
	# menu's own key) until the held base is HWS_GHOST_BASE (default 00110000, the Fallout4.esm piece the menu held first in the
	# 2026-11-10 runs, put down at Sanctuary's bench in 5 of 6 of them); at most HWS_GHOST_STEPS presses.
	if _sws_host_ghost; then
		local gb gp=0
		while :; do
			gb=$(field "$(dc host workshop)" heldBase)
			[ "$gb" = "$HWS_GHOST_BASE" ] || [ "$gp" -ge "${HWS_GHOST_STEPS:-20}" ] && break
			dc host "key 0x27 80" >/dev/null; sleep 1; gp=$((gp + 1))
		done
		echo "$label: the host's ghost by base: $gb after $gp Right arrow press(es) through the build menu (wanted $HWS_GHOST_BASE)"
		if ! _sws_host_ghost || [ "$gb" != "$HWS_GHOST_BASE" ]; then
			fail "$label: set-up: the host's ghost is the piece $HWS_GHOST_BASE, picked by its base" "after $gp Right arrow press(es): $(host workshop | cut -c1-200)"; _ws_exit host; return 1
		fi
	fi
	w=$(host workshop); ghost=$(field "$w" held)
	if _sws_host_ghost; then pass "$label: set-up: the host holds a new piece's ghost from its build menu" "held=$ghost base=$(field "$w" heldBase)"
		dc host "turnto $SANCT_BENCH 600 0" >/dev/null; sleep 1   # level again: the ghost on the ground, where Accept can put it down
		for i in 1 2 3 4 5 6; do
			[ "$(field "$(dc host workshop)" canPlace)" = true ] && break
			dc host "mouse 300 0" >/dev/null; sleep 1   # a red ghost (into the bench, off the build area): turned away a little
		done
		w=$(host workshop)
		if [ "$(field "$w" canPlace)" = true ]; then pass "$label: set-up: the host's ghost may be put down where it stands before the VM's build" "canPlace=true held=$(field "$w" held)"
		else fail "$label: set-up: the host's ghost may be put down where it stands before the VM's build" "never green after six turns: $(cut -c1-200 <<<"$w")"; ghost=""; fi
	else fail "$label: set-up: the host holds a new piece's ghost from its build menu" "after 5 Accepts: $(cut -c1-200 <<<"$w") (the first item's components?)"; ghost=""; fi
	win_mark sws host; win_mark sws vm
	l=$(_sws_build_vm "$SWS_LIGHT" "$label")
	if wait_for 20 _sws_built host "$SWS_LIGHT"; then pass "$label: the host built the VM's light while in workshop mode" "$(grep -oE "WorkshopSync: [0-9A-F]{8} \(base $SWS_LIGHT\) built for player [0-9]+[^;]*" "$A_OUT/windelta-sws-host.txt" | head -1 | cut -c1-140)"
	else fail "$label: the host built the VM's light while in workshop mode" "$(grep -E 'WorkshopSync: (player|request)' "$A_OUT/windelta-sws-host.txt" "$A_OUT/windelta-sws-vm.txt" | tail -2 | tr '\n' '|')"; fi
	_sws_placed host "$(grep -oE "WorkshopSync: [0-9A-F]{8} \(base $SWS_LIGHT\) built" "$A_OUT/windelta-sws-host.txt" | head -1 | cut -d' ' -f2)"
	if [ -n "$ghost" ]; then
		w=$(host workshop)
		if [ "$(field "$w" held)" = "$ghost" ] && [ "$(field "$w" newItem)" = true ]; then pass "$label: the host's ghost survived the VM's build (the same reference, still a new piece)" "held=$ghost"
		else fail "$label: the host's ghost survived the VM's build (the same reference, still a new piece)" "before held=$ghost; after: $(cut -c1-200 <<<"$w")"; fi
		local fin; fin=$(grep -oE 'WorkshopSync: [0-9A-F]{8} finished while our workshop mode is open[^;]*' "$(win_read sws host)" | tail -1 | cut -c1-240)
		if [ "$(field "$w" canPlace)" = true ]; then pass "$label: the host's ghost may still be put down after the VM's build (canPlace)" "canPlace=true; ${fin:-no finish line in the host log}"
		else fail "$label: the host's ghost may still be put down after the VM's build (canPlace)" "it was green before the build: $(cut -c1-200 <<<"$w"); ${fin:-no finish line in the host log}"; fi
		dc host "key $HWS_KEY_SELECT 80" >/dev/null; sleep 2
		w=$(host workshop); out=$(host "refinfo $ghost")
		if [ "$(field "$w" held)" != "$ghost" ] && [ "$(field "$out" deleted)" = false ]; then pass "$label: the host's ghost can still be placed (Accept put it down; workshop mode holds another)" "placed $ghost; now $(grep -oE 'held=[0-9A-F]+' <<<"$w")"; _sws_placed host "$ghost"
		else fail "$label: the host's ghost can still be placed (Accept put it down; workshop mode holds another)" "ghost $ghost: $(cut -c1-120 <<<"$out") | $(cut -c1-160 <<<"$w")"; fi
	fi
	_ws_exit host
}
# A bed from the VM with a settler assigned to it in the VM's game before the host takes it over (the adoption fires the
# destroyed event so the assignment goes), then a save during the pending window and a load alone: nothing of the session on
# the VM's grids, no pending piece, the settler no longer the owner of the bed's copy (unassigned, WorkshopObjectScript gives a
# piece with bDefaultPlayerOwnership, as the bed has, back to the player: owner=00000007, the Player's NPC; or no owner at all).
# Both windows are made by console, said so: the control's
# settler is made in the VM's game (wsreport place ... quiet), added to Sanctuary as the bench step adds Codsworth and assigned
# to the bed by WorkshopParentScript.AssignActorToObjectPUBLIC; the pending window is the host's answers held (dev wsmode hold)
# from before the VM's piece until after its save, then served.
_sws_bed_and_pending_save() {
	local out b bc b0 before settler label="sharedws (bed, save in the pending window)"
	win_mark sws host; win_mark sws vm
	b0=$(_sws_budget vm)
	# the bed the engine's way, with the rule held off (wsmode off) so the finish's scripts and placed event run for the piece as the
	# VM's own (WorkshopParent makes it Sanctuary's), the control's settler assigned to it by console; then the rule back
	# (wsmode host) and the piece handed over (wsreport: the request path), so the adoption's destroyed event has an assignment to undo
	vm "wsmode off" >/dev/null
	settler=$(vm "wsreport place $SWS_SETTLER $SANCT_BENCH quiet" | grep -o "[0-9A-F]\{8\}" | head -1)
	if [ -z "$settler" ]; then fail "$label: set-up: the control's settler is made in the VM's game" "no settler made of base $SWS_SETTLER"; vm "wsmode host" >/dev/null; return 1; fi
	SWS_SETTLER_REF=$settler
	vm "console cqf WorkshopParent \"WorkshopParentScript.AddActorToWorkshopPUBLIC\" $settler $SANCT_BENCH" >/dev/null; sleep 3
	b=$(_sws_build_vm "$SWS_BED" "$label"); [ -z "$b" ] && { vm "wsmode host" >/dev/null; return 1; }
	sleep 3   # the bed's placed event: WorkshopParent makes it one of Sanctuary's objects first
	vm "console cqf WorkshopParent \"WorkshopParentScript.AssignActorToObjectPUBLIC\" $settler $b" >/dev/null
	_sws_owned() { grep -q ' owner=' <<<"$(vm "wsinfo $1")"; }
	wait_for 15 _sws_owned "$b"; before=$(vm "wsinfo $b" | grep -oE 'owner=[0-9A-F]+')
	if [ -n "$before" ]; then pass "$label: the control: the settler is assigned to the VM's bed before the hand-over" "$before (settler $settler)"
	else fail "$label: the control: the settler is assigned to the VM's bed before the hand-over" "no owner on bed $b after AssignActorToObjectPUBLIC($settler, $b): $(vm "wsinfo $b" | cut -c1-120) | $(vm "scriptvars $settler WorkshopNPCScript workshopID" | cut -c1-80)"; vm "wsmode host" >/dev/null; return 1; fi
	# with the rule off the finish's placed event reported the bed as the VM's own (the host made a copy, the server keeps it):
	# reported scrapped before the hand-over, so no stray copy stays on the host or in the server's rejoin list
	_sws_host_copyless() { [ -z "$(_sws_copy_of host "$1")" ]; }
	vm "wsreport $b $SANCT_BENCH scrap" >/dev/null
	if wait_for 15 _sws_host_copyless "$b"; then pass "$label: the bed reported as the VM's own while the rule was off is scrapped for the session before the hand-over (no copy on the host)" "host copies of $b: none"
	else fail "$label: the bed reported as the VM's own while the rule was off is scrapped for the session before the hand-over (no copy on the host)" "host copy '$(_sws_copy_of host "$b")' of the VM's $b"; fi
	vm "wsmode host" >/dev/null; sleep 1
	vm "wsreport $b $SANCT_BENCH" >/dev/null
	if wait_for 20 _sws_built host "$SWS_BED"; then pass "$label: the host built the VM's bed" "ok"; else fail "$label: the host built the VM's bed" "$(grep -E 'WorkshopSync: (player|request)' "$A_OUT/windelta-sws-host.txt" | tail -2 | tr '\n' '|')"; return 1; fi
	_sws_placed host "$(grep -oE "WorkshopSync: [0-9A-F]{8} \(base $SWS_BED\) built" "$A_OUT/windelta-sws-host.txt" | head -1 | cut -d' ' -f2)"
	wait_for 15 _sws_answered vm; sleep 5
	bc=$(_sws_copy_of vm "$(grep -oE "WorkshopSync: [0-9A-F]{8} \(base $SWS_BED\) built" "$A_OUT/windelta-sws-host.txt" | head -1 | cut -d' ' -f2)")
	# the bed's owner is the whole assignment: the settler's side keeps nothing naming its bed (WorkshopNPCScript has no such
	# property; WorkshopObjectScript.GetAssignedActor is the bed's GetActorRefOwner), so no owner here means no settler assigned
	local after; after=$(vm "wsinfo ${bc:-0}" | grep -oE 'owner=[0-9A-F]+')
	if [ -n "$bc" ] && { [ -z "$after" ] || [ "$after" = owner=00000007 ]; } && [ "$after" != "$before" ]; then pass "$label: after the adoption the VM's copy of the bed has no settler assigned (the destroyed event undid it: no owner, or the player's)" "before $before, after ${after:-no owner}"
	else fail "$label: after the adoption the VM's copy of the bed has no settler assigned (the destroyed event undid it: no owner, or the player's)" "copy '$bc': before $before (settler $settler), after ${after:-no owner}"; fi
	expect_eq "$label: the VM's bench budget is back to what it was before the bed (counted by the finish, taken out by the adoption)" "$(_sws_budget vm)" "$b0"
	# the save in the pending window: the host's answers held, so the VM's piece is pending while the save is made. The hold must
	# end inside SharedWorkshop::ANSWER_WAIT (15 s from the request) or the VM takes the request as unanswered: the save and its
	# checks take about 8 s. The pending piece goes before the save (BeforeSave, with the destroyed event); its request stays
	# open, dropped (wsmode pending=0 dropped=1), so the host's late answer is matched ("dropped before a save") and the host's copy
	# comes on its own once the host serves again
	host "wsmode hold" >/dev/null
	if [ "$(field "$(host wsmode)" held)" != true ]; then fail "$label: set-up: the host's answers are held (dev wsmode hold)" "$(host wsmode | grep -oE 'host: .*' | cut -c1-160)"; host "wsmode serve" >/dev/null; return 1; fi
	win_mark sws host
	out=$(vm "wsreport place $SWS_LIGHT $SANCT_BENCH quiet"); b=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1); [ -n "$b" ] && _sws_placed vm "$b"
	vm "wsfinish $b $SANCT_BENCH" >/dev/null
	_sws_pending() { local p; p=$(_sws_counter vm pending); is_int "$p" && [ "$p" -ge 1 ]; }
	if wait_for 5 _sws_pending; then pass "$label: one piece of the VM's is pending when the save is asked for (the host's answer held)" "pending=$(_sws_counter vm pending)"
	else fail "$label: one piece of the VM's is pending when the save is asked for (the host's answer held)" "wsmode said pending='$(_sws_counter vm pending)': the save would prove nothing; $(vm wsmode | cut -c1-160)"; host "wsmode serve" >/dev/null; return 1; fi
	vm "console save ${SWS_SAVE}_pending" >/dev/null
	sleep 4; wait_ingame vm 30
	local wsm; wsm=$(vm wsmode)
	expect_eq "$label: after a save in the pending window no piece of the VM's still waits" "$(field "$wsm" pending)" 0
	expect_eq "$label: ...its request stays open, dropped, for the host's late answer" "$(field "$wsm" dropped)" 1
	host "wsmode serve" >/dev/null
	local hp=""
	if wait_for 20 _sws_built host "$SWS_LIGHT"; then hp=$(grep -oE "WorkshopSync: [0-9A-F]{8} \(base $SWS_LIGHT\) built" "$A_OUT/windelta-sws-host.txt" | tail -1 | cut -d' ' -f2); _sws_placed host "$hp"; fi
	_sws_late_matched() { grep -qE "WorkshopSync: the host built our [0-9A-F]{8} as its [0-9A-F]{8}, but ours is gone \(dropped before a save" "$(win_read sws vm)"; }
	if wait_for 15 _sws_late_matched; then pass "$label: the host's late answer is matched to the dropped request (not adopted, not unknown)" "$(grep -oE 'WorkshopSync: the host built our [^;]*dropped before a save[^;]*' "$A_OUT/windelta-sws-vm.txt" | tail -1 | cut -c1-200)"
	else fail "$label: the host's late answer is matched to the dropped request (not adopted, not unknown)" "$(grep -E 'WorkshopSync: (request|the host built|our)' "$A_OUT/windelta-sws-vm.txt" | tail -2 | tr '\n' '|' | cut -c1-300); $(vm wsmode | grep -oE '(pending|dropped|answersUnknown|answersLate)=[0-9]+' | tr '\n' ' ')"; fi
	expect_eq "$label: once answered, no request of the VM's is open" "$(field "$(vm wsmode)" dropped)/$(field "$(vm wsmode)" pending)" 0/0
	_sws_has_copy_vm() { [ -n "$(_sws_copy_of vm "$1")" ]; }
	if [ -n "$hp" ] && wait_for 15 _sws_has_copy_vm "$hp"; then pass "$label: the host's piece for the dropped request comes to the VM on its own, as a copy" "the VM's copy $(_sws_copy_of vm "$hp") of the host's $hp"
	else fail "$label: the host's piece for the dropped request comes to the VM on its own, as a copy" "host piece '$hp': no copy on the VM"; fi
	_sws_grid_clean vm "$label: after that save, the VM's grids name no copy and no runtime reference"
	vm "net leave" >/dev/null; sleep 4
	vm "console load ${SWS_SAVE}_pending" >/dev/null; sleep 20; wait_ingame vm 60
	_sws_grid_clean vm "$label: that save loaded alone: grids clean"
	expect_eq "$label: that save loaded alone: no copy of ours linked to a workbench (audit)" "$(field "$(vm wspower)" found)" 0
	_sws_rejoin "$label" || return 1
	enter_both SanctuaryExt 14
	walk vm "$label: set-up: the VM walks back to the workbench" 0.8 300 $SANCT_BENCH || { pads_off; return 1; }
	pads_off
}
step_sharedws_power() {
	section "shared workshop (a friend builds and wires at the host's settlement; console placement, SetOwnedByPlayer and throwaway saves on both)"
	local r
	r=$(dc host rules)
	if [ "$(field "$r" sharedWorkshop)" != true ]; then fail "sharedws: set-up: the host's bSharedWorkshop is on" "rules say sharedWorkshop='$(field "$r" sharedWorkshop)'"; return; fi
	if [ "$(field "$(vm wsmode)" hooks)" != true ]; then fail "sharedws: set-up: the VM's pick hooks are installed" "$(vm wsmode | cut -c1-160)"; return; fi
	enter_both SanctuaryExt 14
	host "console $SANCT_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 2
	if [ "$(field "$(host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then fail "sharedws: set-up: Sanctuary is the host's" "$(host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer")"; return; fi
	vm "wsmode host" >/dev/null
	_sws_keys vm || return
	_sws_keys host HWS_KEY_ || return   # the host's own keys for its own workshop mode (its ghost, its scrap of the VM's wire)
	walk host "sharedws: set-up: the host walks to Sanctuary's workbench" 0.8 200 $SANCT_BENCH || { pads_off; return; }
	walk vm "sharedws: set-up: the VM walks to Sanctuary's workbench" 0.8 300 $SANCT_BENCH || { pads_off; return; }
	pads_off
	# A settlement taken by console has no workshop data until its workshop is opened once (the engine makes it then; a host that
	# took the settlement in play has it already): a VM build before that must be refused as such, with the host's notice, and the
	# host's workshop mode once at Sanctuary is the set-up for everything after.
	local unopened; win_mark sws host; win_mark sws vm
	unopened=$(_sws_build_vm "$SWS_LIGHT" "sharedws: set-up (unopened bench)")
	if wait_for 15 grep -qE "request [0-9]+ \(build\) at workbench [0-9A-F]{8}: (the host hasn.t opened that workshop yet|applied)" "$(win_read sws vm)"; then
		if grep -qE "hasn.t opened that workshop yet" "$A_OUT/windelta-sws-vm.txt"; then pass "sharedws: a build at a workshop the host never opened is refused as such (the piece stays the VM's own)" "$(grep -oE 'request [0-9]+ \(build\)[^;]*' "$A_OUT/windelta-sws-vm.txt" | head -1)"
		else warn "sharedws: a build at a workshop the host never opened is refused as such" "Sanctuary's bench had its workshop data already (an earlier run opened it): applied instead"; fi
	else fail "sharedws: a build at a workshop the host never opened is answered" "$(grep -E 'WorkshopSync: request' "$A_OUT/windelta-sws-vm.txt" | tail -2 | tr '\n' '|')"; fi
	[ -n "$unopened" ] && vm "wsscrap $unopened" >/dev/null
	if _ws_enter host; then sleep 3; _ws_exit host; pass "sharedws: set-up: the host opened Sanctuary's workshop once (its workshop data made)" "ok"; else fail "sharedws: set-up: the host opened Sanctuary's workshop once" "$(host workshop)"; return; fi
	_sws_one_way host vm || return
	_sws_radiator "$SWS_HG" "$SWS_VG"
	# the real hooks (workshop mode's own keys), the host in workshop mode, a bed, and a save in the pending window
	if [ -n "$SWS_VG" ] && [ -n "$SWS_VL" ]; then _sws_real_hooks "$SWS_VG" "$SWS_VL"; else fail "sharedws (real hooks): the VM has copies of both the host's pieces" "generator '$SWS_VG' light '$SWS_VL'"; fi
	_sws_host_busy
	_sws_host_budget_cases
	_sws_bed_and_pending_save || return
	# an in-session save on the VM, and the same after it
	vm "console save $SWS_SAVE" >/dev/null; sleep 8; wait_ingame vm 30
	_sws_grid_clean vm "sharedws: after an in-session save, the VM's grids still name no copy and no runtime reference"
	_sws_audit_clean vm "sharedws: after an in-session save, the VM's audit finds nothing tied in"
	# the VM leaves and loads that save alone: nothing of the session on its grids, and nothing humming
	vm "net leave" >/dev/null; sleep 4
	vm "console load $SWS_SAVE" >/dev/null; sleep 20; wait_ingame vm 60
	_sws_grid_clean vm "sharedws: loaded alone, the VM's grids name no copy and no runtime reference"
	_sws_audit_clean vm "sharedws: loaded alone, the VM's audit finds no copy with power links"
	local lit; lit=$(vm 'wsgrid' | grep -c 'COPY'); expect_eq "sharedws: loaded alone, no grid names a copy" "$lit" 0
	# back into the session for the reverse
	_sws_rejoin "sharedws" || return
	enter_both SanctuaryExt 14
	walk host "sharedws: set-up: the host walks back to the workbench" 0.8 200 $SANCT_BENCH || { pads_off; return; }
	walk vm "sharedws: set-up: the VM walks back to the workbench" 0.8 300 $SANCT_BENCH || { pads_off; return; }
	pads_off
	_sws_one_way vm host || return
	_sws_cleanup
}
# The pieces this step placed scrapped as the engine scraps (dev wsscrap: off the grids, links, budget and workbench, told to the
# scripts, reported, deleted) by whichever game owns each, so no grid names a deleted reference; cleanup.sh then removes what is left.
_sws_placed() { echo "$2" >> "$A_OUT/ws-placed.txt"; echo "$2" >> "$A_OUT/ws-placed-$1.txt"; }   # _sws_placed <who> <ref>: a piece that game made (runtime ids are each game's own: scrapped there alone)
# Codsworth made Sanctuary's settler (AddActorToWorkshopPUBLIC) walks off to a settler's spot out east, about (-76300, 91000), past
# what the players' walk can reach (2026-11-12: the host's walk to him never arrived in 3 tries, in sharedws-assign and -supply).
# Set-up therefore puts him by console a few hundred units west of the workbench, on the open road, and holds him there with
# SetRestrained (SetAV SpeedMult 0 did not hold him); the players then walk to him for real. Said in a WARN line; let go after.
SWS_CODS_SPOT=${SWS_CODS_SPOT:-"-79500 89560 7840"}; SWS_CODS_HELD=""
_sws_codsworth_hold() {  # _sws_codsworth_hold <label>
	local ri
	host "console $CODSWORTH.setrestrained 1" >/dev/null; SWS_CODS_HELD=1
	host "setpos $CODSWORTH $SWS_CODS_SPOT" >/dev/null; sleep 3
	ri=$(host "refinfo $CODSWORTH")
	warn "$1: set-up: Codsworth put by console west of Sanctuary's workbench and held there (setpos $SWS_CODS_SPOT, SetRestrained 1: as a settler he walks off past the players' reach); the players walk to him" "at ($(field "$ri" x), $(field "$ri" y), $(field "$ri" z))"
}
_sws_codsworth_free() { [ -n "$SWS_CODS_HELD" ] || return 0; host "console $CODSWORTH.setrestrained 0" >/dev/null; SWS_CODS_HELD=""; }
_sws_cleanup() {
	local r who
	_sws_codsworth_free
	for who in host vm; do for r in $(sort -u "$A_OUT/ws-placed-$who.txt" 2>/dev/null); do dc "$who" "wsscrap $r" >/dev/null; done; done
	sleep 3
	local g; g=$(host "wsgrid $SANCT_BENCH")
	if grep -q "gone named=0" <<<"$g"; then pass "sharedws: cleanup: the host's grids name no deleted reference" "$(grep -oE 'copies named=[0-9]+ gone named=[0-9]+' <<<"$g")"; else fail "sharedws: cleanup: the host's grids name no deleted reference" "$(cut -c1-200 <<<"$g")"; fi
	host "wsmode serve" >/dev/null   # the pending-save check's hold, whatever stopped it
	# the bed control's settler (the VM's, linked to the bench: not a piece, wsscrap would take it off the budget): a plain remove
	[ -n "$SWS_SETTLER_REF" ] && vm "remove $SWS_SETTLER_REF" >/dev/null
	# cleanup.sh applies the list on the host alone: the host-made ids only (a VM-made id can name something else there); the VM's own
	# pieces went through its wsscrap above, and cleanup.sh's scan of the VM needs no list
	bash "$RIG/cleanup.sh" run "$A_OUT/ws-placed-host.txt" >> "$A_OUT/cleanup.log" 2>&1
}

# ---- the shared workshop's phase-2 prerequisites (opt-in: smoke.sh --only sharedws-prereq) -------------------------------------
# A deleted copy stays deleted across its cell's unload and load, an in-session save and a solo load (WorkshopSync's Bury: the
# record of changes dropped with the delete; a friend's generator copy once came back alive after a trip away), and a
# machinegun turret (a templated NPC, whose actor gets a runtime base) is shared under its original base, and its buried copy
# (the actor's record and its runtime base's dropped together) survives two loads. Console placement on the host (wsreport
# place + wsfinish, as sharedws-power's host side), the host's scrap by dev wsscrap (the engine's scrap short of the yield:
# reported scrapped, as a real one is); the VM's trips by console coc (said here: no movement is under test). Throwaway saves
# on the VM; the host's pieces scrapped at the end and cleanup.sh run.
# The order matters: the return needs the copy's record written out to the cell's buffer while the copy stands (the unload of
# its cell does that), and only then the delete. Scrapped first, the copy's record was never buffered, the engine's own delete
# drops it, and every assertion passes on the old code too.
_sws_ref_gone() {  # _sws_ref_gone <who> <ref> [base]: that game has no live reference under the id: none, one deleted, or (a base given) one of another base, the id handed out again once the buried form was gone (not the piece back); a dead channel is no evidence
	local i; i=$(dc "$1" "wsinfo $2")
	case "$i" in "") return 1;; error*|*"no such"*) return 0;; esac
	[ "$(field "$i" deleted)" = true ] && return 0
	[ -n "${3:-}" ] && [ "$(field "$i" base)" != "$3" ]
}
_sws_trip() {  # _sws_trip <who>: that game's player away to Red Rocket and back to Sanctuary by console (Sanctuary's cell unloaded and loaded again)
	dc "$1" "console coc RedRocketExt" >/dev/null; sleep 12; wait_ingame "$1" 40
	dc "$1" "console coc SanctuaryExt" >/dev/null; sleep 14; wait_ingame "$1" 40
}
_sws_refound() { dc "$1" "wspower copies" | grep -o "found again=[0-9]*" | sed "s/.*=//"; }   # copies found again under their id, their handle naming nothing (Live)
_sws_buried() { local p; p=$(dc "$1" wspower); echo "buried=$(field "$p" buried) buriedNoRecord=$(field "$p" buriedNoRecord) recordsKept=$(field "$p" recordsKept) buriedPersistent=$(field "$p" buriedPersistent) basesDropped=$(field "$p" basesDropped)"; }
step_sharedws_prereq() {
	section "shared workshop prerequisites (a deleted copy stays deleted; machinegun turrets are shared and their buried copies survive loads; console placement, coc trips and throwaway saves on the VM)"
	local r g vgc t vtc out before after rf0 rf1
	r=$(dc host rules)
	if [ "$(field "$r" sharedWorkshop)" != true ]; then fail "sharedws-prereq: set-up: the host's bSharedWorkshop is on" "rules say sharedWorkshop='$(field "$r" sharedWorkshop)'"; return; fi
	enter_both SanctuaryExt 14
	host "console $SANCT_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 2
	if _ws_enter host; then sleep 3; _ws_exit host; else fail "sharedws-prereq: set-up: the host opened Sanctuary's workshop once" "$(host workshop)"; return; fi
	win_mark swp host; win_mark swp vm
	# a generator of the host's, its copy on the VM
	out=$(host "wsreport place $SWS_GENERATOR $SANCT_BENCH"); g=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
	if [ -z "$g" ]; then fail "sharedws-prereq: the host's generator is placed" "$out"; return; fi
	_sws_placed host "$g"; sleep 2; host "wsfinish $g $SANCT_BENCH" >/dev/null
	if wait_for 20 _sws_has_copy vm "$g"; then vgc=$(_sws_copy_of vm "$g"); pass "sharedws-prereq: the VM has a copy of the host's generator" "copy $vgc of $g"
	else fail "sharedws-prereq: the VM has a copy of the host's generator" "$(vm 'wspower copies' | cut -c1-160)"; return; fi
	# the copy stands through a trip away: its cell's unload writes its record out to the cell's buffer, the load makes it again
	# from that buffer, and Live finds it again under its id (the shape of the record the engine's own delete then leaves alone)
	rf0=$(_sws_refound vm)
	_sws_trip vm
	if wait_for 20 _sws_has_copy vm "$g" && [ "$(_sws_copy_of vm "$g")" = "$vgc" ]; then pass "sharedws-prereq: after a trip away with the copy standing, the VM finds its copy again under the same id" "$(vm "wsinfo $vgc" | cut -c1-140)"
	else fail "sharedws-prereq: after a trip away with the copy standing, the VM finds its copy again under the same id" "copy now '$(_sws_copy_of vm "$g")', was $vgc; $(vm "wsinfo $vgc" | cut -c1-160)"; return; fi
	rf1=$(_sws_refound vm)
	if [ "${rf1:-0}" -gt "${rf0:-0}" ] 2>/dev/null; then pass "sharedws-prereq: the trip unloaded the copy's cell (found again under its id: $rf0 -> $rf1)" "its record is the buffered kind now"
	else warn "sharedws-prereq: the trip may not have unloaded the copy's cell (found again: $rf0 -> $rf1): the delete below proves less" "$(vm 'wspower copies' | cut -c1-160)"; fi
	# the host scraps it: the VM's copy goes, buried (its buffered record dropped with it)
	# Counted: buried (its record dropped by Bury) and buriedNoRecord (the engine's delete had dropped it already, nothing left to
	# drop): either way the copy went without a record (2026-11-11: buried=0 buriedNoRecord=1 read as a failure).
	_swp_went() { local p b n; p=$(vm wspower); b=$(field "$p" buried); n=$(field "$p" buriedNoRecord); is_int "$b" || b=0; is_int "$n" || n=0; echo $((b + n)); }
	before=$(_swp_went)
	host "wsscrap $g" >/dev/null
	if wait_for 20 _sws_no_copy vm "$g" && wait_for 10 _sws_ref_gone vm "$vgc"; then pass "sharedws-prereq: the host's scrap takes the VM's copy away (deleted)" "$(vm "wsinfo $vgc" | cut -c1-140)"
	else fail "sharedws-prereq: the host's scrap takes the VM's copy away (deleted)" "$(vm "wsinfo $vgc" | cut -c1-200)"; return; fi
	after=$(_swp_went)
	expect_ge "sharedws-prereq: the copy went without its record of changes (wspower buried + buriedNoRecord; $(_sws_buried vm))" "$(( ${after:-0} - ${before:-0} ))" 1 reference
	expect_eq "sharedws-prereq: the buried copy has no record of changes left (wsinfo record)" "$(field "$(vm "wsinfo $vgc")" record)" false
	if grep -q "still holds its record of changes" "$(win_read swp vm)"; then warn "sharedws-prereq: the engine kept a deleted reference's record" "$(grep -m1 'still holds its record' "$A_OUT/windelta-swp-vm.txt" | cut -c1-160)"; fi
	# the VM's cell unloads and loads again: nothing of the generator stands under the copy's id (the old code brought it back here)
	_sws_trip vm
	if _sws_ref_gone vm "$vgc" "$SWS_GENERATOR"; then pass "sharedws-prereq: after its cell unloaded and loaded again, the deleted copy is not back" "$(vm "wsinfo $vgc" | cut -c1-140)"
	else fail "sharedws-prereq: after its cell unloaded and loaded again, the deleted copy is not back" "$(vm "wsinfo $vgc" | cut -c1-200)"; fi
	expect_eq "sharedws-prereq: no stray generator stands at the spot (the VM's copies list names none of it)" "$(_sws_copy_of vm "$g")" ""
	# an in-session save on the VM, then the save loaded alone: still nothing under the id, grids clean
	vm "console save $SWS_SAVE" >/dev/null; sleep 8; wait_ingame vm 30
	vm "net leave" >/dev/null; sleep 4
	vm "console load $SWS_SAVE" >/dev/null; sleep 20; wait_ingame vm 60
	if _sws_ref_gone vm "$vgc" "$SWS_GENERATOR"; then pass "sharedws-prereq: loaded alone, the deleted copy is not in the save" "$(vm "wsinfo $vgc" | cut -c1-140)"
	else fail "sharedws-prereq: loaded alone, the deleted copy is not in the save" "$(vm "wsinfo $vgc" | cut -c1-200)"; fi
	_sws_grid_clean vm "sharedws-prereq: loaded alone, the VM's grids name no copy and no runtime reference"
	_vm_restart_rejoin swp
	enter_both SanctuaryExt 14
	# a machinegun turret of the host's: its actor's base is made at runtime here, and it is shared under its original all the same
	win_mark swp host; win_mark swp vm
	out=$(host "wsreport place $SWS_MG_TURRET $SANCT_BENCH"); t=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
	if [ -z "$t" ]; then fail "sharedws-prereq: the host's machinegun turret is placed" "$out"; _sws_cleanup; return; fi
	_sws_placed host "$t"; sleep 2; host "wsfinish $t $SANCT_BENCH" >/dev/null
	local tb; tb=$(field "$(host "wsinfo $t")" base)
	if wait_for 25 _sws_has_copy vm "$t"; then vtc=$(_sws_copy_of vm "$t"); pass "sharedws-prereq: the host's machinegun turret has a copy on the VM (its base here $tb, reported under the original)" "copy $vtc of $t: $(vm "wsinfo $vtc" | cut -c1-120)"
	else fail "sharedws-prereq: the host's machinegun turret has a copy on the VM (its base here $tb)" "$(vm 'wspower copies' | cut -c1-160); host: $(host "wsinfo $t" | cut -c1-160)"; _sws_cleanup; return; fi
	# the host scraps the turret: the VM's copy, an actor over a runtime base of its own, is buried with both records dropped, and
	# the VM's game then loads a save twice with the copy's cell still loaded (an actor kept in memory through a load over a base
	# the load freed crashed the second load, on stand-ins)
	local pbefore pafter; before=$(field "$(vm wspower)" basesDropped); pbefore=$(field "$(vm wspower)" buriedPersistent)
	host "wsscrap $t" >/dev/null
	if wait_for 20 _sws_no_copy vm "$t" && wait_for 10 _sws_ref_gone vm "$vtc"; then pass "sharedws-prereq: the host's scrap takes the VM's turret copy away (deleted)" "$(vm "wsinfo $vtc" | cut -c1-140)"
	else fail "sharedws-prereq: the host's scrap takes the VM's turret copy away (deleted)" "$(vm "wsinfo $vtc" | cut -c1-200)"; _sws_cleanup; return; fi
	# the base's record goes with the actor's (basesDropped) when the actor was buried with its record dropped; an actor buried while
	# persistent keeps the engine's own record (buriedPersistent; Bury), the load frees actor and base together, and the two loads
	# below are the proof either way (2026-11-11: the copy was persistent, buriedPersistent 2, basesDropped 0, both loads fine)
	after=$(field "$(vm wspower)" basesDropped); pafter=$(field "$(vm wspower)" buriedPersistent)
	if [ "$(( ${after:-0} - ${before:-0} ))" -ge 1 ] || [ "$(( ${pafter:-0} - ${pbefore:-0} ))" -ge 1 ]; then pass "sharedws-prereq: the turret copy's runtime base lost its record with the actor's, or the actor was buried persistent (the load frees both)" "$(_sws_buried vm)"
	else fail "sharedws-prereq: the turret copy's runtime base lost its record with the actor's, or the actor was buried persistent (the load frees both)" "basesDropped $before -> $after, buriedPersistent $pbefore -> $pafter: $(_sws_buried vm)"; fi
	vm "net leave" >/dev/null; sleep 4
	vm "console load $SWS_SAVE" >/dev/null; sleep 20
	if wait_ingame vm 90; then pass "sharedws-prereq: the VM's game is in the game after the first load over the buried turret copy" "$(vm status | cut -c1-120)"
	else fail "sharedws-prereq: the VM's game is in the game after the first load over the buried turret copy" "$(vm status | cut -c1-200)"; _vm_restart_rejoin swp; _sws_cleanup; return; fi
	vm "console load $SWS_SAVE" >/dev/null; sleep 20
	if wait_ingame vm 90; then pass "sharedws-prereq: the VM's game is in the game after the second load (the stand-in crash's shape)" "$(vm status | cut -c1-120)"
	else fail "sharedws-prereq: the VM's game is in the game after the second load (the stand-in crash's shape)" "$(vm status | cut -c1-200)"; fi
	_vm_restart_rejoin swp
	_sws_cleanup
}

# ---- the shared workshop's scrap request: a friend scraps the host's piece (opt-in: smoke.sh --only sharedws-scrap) ----------------
# The host builds a generator, the VM a construction light wired to it (the one-way set-up: the host's pieces, the VM's copies).
# Then the VM, in workshop mode, aims at its copy of the host's generator and presses the real Scrap key: the engine's own test
# passes the copy (the host's bSharedWorkshopEdit on), its confirmation (ExamineConfirmMenu) opens and is confirmed with the
# real key; the confirmation's Yes asks the host instead of scrapping (WorkshopSync's ScrapAcceptHook), and the host's game
# scraps its generator with the engine's own scrap (Workshop::ScrapReference inside the bench and budget scopes). Asserted: the
# VM's game scrapped nothing itself (its copy goes only with the host's scrapped report), the host's generator and the wire to it
# are gone, the host's bench holds more than before (the yield), the VM's copies of both are gone, the VM's own bench and
# inventory are unchanged, both games' grids name no deleted reference, and the VM's grids are clean after an in-session save and
# loaded alone (no stray generator). Console placement and SetOwnedByPlayer, said so; throwaway saves on both.
SWS_SCRAP_SAVE=${SWS_SCRAP_SAVE:-f4mp_sharedws_scrap}
_sws_stacks() {  # the items the bench holds in that game, all stacks summed; nothing when the game gave no real answer (no answer, or an error)
	local out; out=$(dc "$1" "contents $SANCT_BENCH")
	case "$out" in "$SANCT_BENCH:"*) ;; *) return 1;; esac
	grep -oE 'x[0-9]+' <<<"$out" | tr -d x | awk '{ s += $1 } END { print s + 0 }'
}
_sws_unchanged() {  # _sws_unchanged <check> <before> <after> [int]: passes only when both were really read (a number, with int) and are equal
	local name=$1 before=$2 after=$3 kind=${4:-}
	if [ -z "$before" ] || [ -z "$after" ] || { [ "$kind" = int ] && { ! is_int "$before" || ! is_int "$after"; }; }; then fail "$name" "not read: before '$before', after '$after'"; return; fi
	expect_eq "$name" "$after" "$before"
}
_sws_scrap_asked() { grep -qE "WorkshopSync: the scrap of our copy $2 of player [0-9]+'s [0-9A-F]{8} at workbench [0-9A-F]{8} asked of the host as request [0-9]+" "$(win_read sws "$1")"; }
_sws_scrap_answered() { grep -qE "WorkshopSync: request [0-9]+ \(scrap\) at workbench [0-9A-F]{8}: $2" "$(win_read sws "$1")"; }
_sws_gone() { local ri; ri=$(dc "$1" "refinfo $2"); case "$ri" in error*) return 0;; esac; [ "$(field "$ri" deleted)" = true ] || [ "$(field "$ri" disabled)" = true ]; }
step_sharedws_scrap() {
	section "shared workshop: a friend scraps the host's piece (the real Scrap key and the engine's confirmation; console placement, SetOwnedByPlayer and throwaway saves on both)"
	local r label="sharedws-scrap"
	r=$(dc host rules)
	if [ "$(field "$r" sharedWorkshop)" != true ]; then fail "$label: set-up: the host's bSharedWorkshop is on" "rules say sharedWorkshop='$(field "$r" sharedWorkshop)'"; return; fi
	if [ "$(field "$r" sharedWorkshopEdit)" != true ]; then fail "$label: set-up: the host's bSharedWorkshopEdit is on" "rules say sharedWorkshopEdit='$(field "$r" sharedWorkshopEdit)'"; return; fi
	if [ "$(field "$(vm wsmode)" hooks)" != true ]; then fail "$label: set-up: the VM's pick hooks are installed (the scrap confirmation's call among them)" "$(vm wsmode | cut -c1-160)"; return; fi
	if [ "$(field "$(vm wsmode)" edit)" != true ]; then fail "$label: set-up: the VM has the host's bSharedWorkshopEdit from the welcome" "$(vm wsmode | cut -c1-160)"; return; fi
	enter_both SanctuaryExt 14
	host "console $SANCT_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 2
	if [ "$(field "$(host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then fail "$label: set-up: Sanctuary is the host's" "$(host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer")"; return; fi
	vm "wsmode host" >/dev/null
	_sws_keys vm || return
	walk host "$label: set-up: the host walks to Sanctuary's workbench" 0.8 200 $SANCT_BENCH || { pads_off; return; }
	walk vm "$label: set-up: the VM walks to Sanctuary's workbench" 0.8 300 $SANCT_BENCH || { pads_off; return; }
	pads_off
	if _ws_enter host; then sleep 3; _ws_exit host; pass "$label: set-up: the host opened Sanctuary's workshop once (its workshop data made)" "ok"; else fail "$label: set-up: the host opened Sanctuary's workshop once" "$(host workshop)"; return; fi
	# the host's generator, the VM's light built as the host's and wired to it (the VM's copies of all three)
	_sws_one_way host vm || return
	local hg=$SWS_HG vg=$SWS_VG hl w vw c0 c1 vc0 vc1 inv0 a0 s0 g0 o0
	hl=$(grep -oE "WorkshopSync: [0-9A-F]{8} \(base $SWS_WIRED_LIGHT\) built" "$A_OUT/windelta-sws-host.txt" | head -1 | cut -d' ' -f2)
	w=$(host "wsinfo ${hl:-0}" | grep -oE 'power=[0-9A-F]{8}' | head -1 | cut -d= -f2)
	vw=$(_sws_copy_of vm "${w:-0}")
	if [ -z "$hg" ] || [ -z "$vg" ]; then fail "$label: set-up: the host's generator and the VM's copy of it" "generator '$hg' copy '$vg'"; return; fi
	crash_snapshot
	win_mark sws host; win_mark sws vm
	c0=$(_sws_stacks host); vc0=$(_sws_stacks vm); inv0=$(field "$(vm inventory)" hash)
	a0=$(_sws_counter vm scrapsAsked); s0=$(_sws_counter host scrapsServed); g0=$(_sws_counter host served); o0=$(field "$(dc vm 'net status')" offThread)
	# the real Scrap key on the copy, then the engine's own confirmation, confirmed with the real key (Accept, else E)
	if ! _ws_enter vm; then fail "$label: the VM enters workshop mode at the host's workbench" "$(vm workshop) | $(vm menus | grep -oE 'open=[^ ]*')"; return; fi
	if ! _sws_put_away vm "$label"; then wait_for 8 _no_msgbox vm; _ws_exit vm; return; fi
	if _sws_aim_press vm "$vg" "$WS_KEY_SCRAP" select "$label"; then
		if wait_for 6 _menu_open vm ExamineConfirmMenu; then
			pass "$label: the VM's Scrap on its copy of the host's generator asks to confirm (the engine's own ExamineConfirmMenu, the host allowing it)" "pressed with $SWS_LAST_PRESS"
			local how=Accept kr
			kr=$(dc vm "key $WS_KEY_SELECT 80"); sleep 1
			if ! wait_for 5 _menu_closed vm ExamineConfirmMenu; then
				how=Activate; kr="$kr; then $(dc vm "key 45 80")"
				wait_for 5 _menu_closed vm ExamineConfirmMenu
			fi
			if _menu_open vm ExamineConfirmMenu; then fail "$label: set-up: the VM confirms the scrap" "the confirmation stayed open after Accept ($WS_KEY_SELECT) and E: $kr; $(vm menus | grep -oE 'open=[^ ]*')"; dc vm "menu ExamineConfirmMenu cancel" >/dev/null; sleep 1
			else pass "$label: set-up: the VM confirms the scrap" "with $how: $kr"; fi
		else
			fail "$label: the VM's Scrap on its copy of the host's generator asks to confirm (the engine's own ExamineConfirmMenu, the host allowing it)" "never opened: pressed with $SWS_LAST_PRESS; now $(vm menus | grep -oE 'open=[^ ]*'); $(grep -oE "WorkshopSync: workshop mode's (Scrap key on|scrap of copy) $vg[^;]*" "$(win_read sws vm)" | tail -1 | cut -c1-260)"
		fi
	fi
	sleep 2; wait_for 8 _no_msgbox vm; _ws_exit vm; sleep 2
	# asked, not scrapped here; served by the host's engine; answered applied
	if wait_for 10 _sws_scrap_asked vm "$vg"; then pass "$label: the confirmation's Yes asked the host instead of scrapping the copy" "$(grep -oE "WorkshopSync: the scrap of our copy $vg[^;]*" "$A_OUT/windelta-sws-vm.txt" | head -1 | cut -c1-200)"
	else fail "$label: the confirmation's Yes asked the host instead of scrapping the copy" "no 'asked of the host' line on the VM: $(grep -E 'WorkshopSync: (the scrap|workshop mode|request)' "$A_OUT/windelta-sws-vm.txt" | tail -3 | tr '\n' '|' | cut -c1-400)"; fi
	expect_eq "$label: the VM counts one scrap asked" "$(( $(_sws_counter vm scrapsAsked) - ${a0:-0} ))" 1
	expect_eq "$label: the confirmation's Yes ran on the VM's main thread (no off-thread call counted across it)" "$(field "$(dc vm 'net status')" offThread)" "${o0:-0}"
	if wait_for 20 _sws_scrap_answered vm applied; then pass "$label: the host's game scrapped its generator and answered applied" "$(grep -oE 'WorkshopSync: request [0-9]+ \(scrap\)[^;]*' "$A_OUT/windelta-sws-vm.txt" | head -1)"
	else fail "$label: the host's game scrapped its generator and answered applied" "VM: $(grep -E 'WorkshopSync: request' "$A_OUT/windelta-sws-vm.txt" | tail -2 | tr '\n' '|'); host: $(grep -E "WorkshopSync: (player|our $hg)" "$(win_read sws host)" | tail -2 | tr '\n' '|' | cut -c1-400)"; fi
	if grep -qE "WorkshopSync: our $hg \(base $SWS_GENERATOR\) at workbench [0-9A-F]{8} scrapped for player [0-9]+'s request [0-9]+: bench budget scrap" "$(win_read sws host)"; then pass "$label: the host ran the engine's scrap inside the bench and budget scopes" "$(grep -oE "WorkshopSync: our $hg [^;]*" "$A_OUT/windelta-sws-host.txt" | head -1 | cut -c1-200)"
	else fail "$label: the host ran the engine's scrap inside the bench and budget scopes" "$(grep -E "WorkshopSync: (our $hg|player)" "$A_OUT/windelta-sws-host.txt" | tail -2 | tr '\n' '|' | cut -c1-300)"; fi
	expect_eq "$label: the host counts one scrap served" "$(( $(_sws_counter host scrapsServed) - ${s0:-0} ))" 1
	# the host's generator gone, the wire with it (the engine destroys the wires strung to a scrapped piece), its grid clean
	if wait_for 5 _sws_gone host "$hg"; then pass "$label: the host's generator is gone" "$(host "refinfo $hg" | cut -c1-120)"; else fail "$label: the host's generator is gone" "$(host "refinfo $hg" | cut -c1-200)"; fi
	if [ -n "$w" ]; then if wait_for 5 _sws_gone host "$w"; then pass "$label: the wire to it went with it (the engine's scrap destroys a piece's wires)" "$(host "refinfo $w" | cut -c1-120)"; else fail "$label: the wire to it went with it (the engine's scrap destroys a piece's wires)" "$(host "refinfo $w" | cut -c1-200)"; fi; fi
	local g; g=$(host "wsgrid $SANCT_BENCH")
	if grep -q "gone named=0" <<<"$g"; then pass "$label: the host's grids name no deleted reference after the scrap" "$(grep -oE 'copies named=[0-9]+ gone named=[0-9]+' <<<"$g")"; else fail "$label: the host's grids name no deleted reference after the scrap" "$(cut -c1-200 <<<"$g")"; fi
	# the yield: the host's bench holds more than before; the VM's own bench and inventory as before
	c1=$(_sws_stacks host); vc1=$(_sws_stacks vm)
	if is_int "$c1" && is_int "$c0" && [ "$c1" -gt "$c0" ]; then pass "$label: the host's workbench holds the yield (more items than before)" "$c0 -> $c1"; else fail "$label: the host's workbench holds the yield (more items than before)" "$c0 -> $c1: $(host "contents $SANCT_BENCH" | cut -c1-200)"; fi
	_sws_unchanged "$label: the VM's own workbench is unchanged (nothing was scrapped there)" "$vc0" "$vc1" int
	_sws_unchanged "$label: the VM's inventory is unchanged" "$inv0" "$(field "$(vm inventory)" hash)"
	# the VM's copies of the generator and the wire gone with the host's report
	if wait_for 15 _sws_no_copy vm "$hg"; then pass "$label: the VM's copy of the generator went with the host's scrapped report" "no copy of $hg (was $vg)"; else fail "$label: the VM's copy of the generator went with the host's scrapped report" "$(vm "refinfo $vg" | cut -c1-160); $(vm 'wspower copies' | cut -c1-160)"; fi
	if [ -n "$w" ] && [ -n "$vw" ]; then if wait_for 10 _sws_no_copy vm "$w"; then pass "$label: ... and its copy of the wire" "no copy of $w (was $vw)"; else fail "$label: ... and its copy of the wire" "$(vm "refinfo $vw" | cut -c1-160)"; fi; fi
	_sws_grid_clean vm "$label: the VM's grids name no copy and no runtime reference after the scrap"
	_sws_audit_clean vm "$label: the VM's audit finds nothing tied in after the scrap"
	# an in-session save on the VM, then loaded alone: no stray generator, grids clean
	vm "console save $SWS_SCRAP_SAVE" >/dev/null; sleep 8; wait_ingame vm 30
	_sws_grid_clean vm "$label: after an in-session save, the VM's grids still name no copy and no runtime reference"
	vm "net leave" >/dev/null; sleep 4
	vm "console load $SWS_SCRAP_SAVE" >/dev/null; sleep 20; wait_ingame vm 60
	_sws_grid_clean vm "$label: loaded alone, the VM's grids name no copy and no runtime reference"
	_sws_audit_clean vm "$label: loaded alone, the VM's audit finds no copy with power links"
	if _sws_gone vm "$vg"; then pass "$label: loaded alone, the VM's old copy of the generator is not back (no stray in the save)" "$(vm "refinfo $vg" | cut -c1-120)"; else fail "$label: loaded alone, the VM's old copy of the generator is not back (no stray in the save)" "$(vm "refinfo $vg" | cut -c1-200)"; fi
	_sws_rejoin "$label"
	enter_both SanctuaryExt 14
	_sws_cleanup
}

# ---- the shared workshop's store request: a friend stores the host's piece (opt-in: smoke.sh --only sharedws-store) ----------------
# The host builds a generator, the VM a construction light wired to it (the one-way set-up: the host's pieces, the VM's copies).
# Then the VM, in workshop mode, aims at its copy of the host's generator and presses the real Store key (Cancel): the engine's own
# test passes the copy (the host's bSharedWorkshopEdit on), its yes/no box (MessageBoxMenu) opens and is answered Yes with the real
# key; the Yes asks the host instead of storing (WorkshopSync's StoreAcceptHook), and the host's game stores its generator with the
# engine's own store (Workshop::DestroyItem with its store flag inside the bench and budget scopes). Asserted: the VM's game stored
# nothing itself (its own stored list, bench and inventory unchanged), the host's generator and the wire to it are gone, the host's
# stored list holds one more of the generator's base, the host's bench holds no more items than before (a store yields nothing), the
# VM's copies of both are gone, both games' grids name no deleted reference, and the VM's grids are clean after an in-session save and
# loaded alone (no stray generator). Then the host places the generator back from its stored list by the dev stand-in (console
# placement, the finish, and the stored entry taken off the list as the engine's own placement from the store takes it: wsstored use)
# and the VM gets a copy of it again. Console placement and SetOwnedByPlayer, said so; throwaway saves on both.
SWS_STORE_SAVE=${SWS_STORE_SAVE:-f4mp_sharedws_store}
_sws_stored_count() {  # that game's Sanctuary stored list: how many of that base; nothing when the game gave no list (no answer, or an error: no workshop data yet)
	local out; out=$(dc "$1" "wsstored $SANCT_BENCH")
	case "$out" in "stored list of workbench "*) ;; *) return 1;; esac
	grep -oE "$2x[0-9]+" <<<"$out" | cut -dx -f2 | awk '{ s += $1 } END { print s + 0 }'
}
_sws_store_asked() { grep -qE "WorkshopSync: the store of our copy $2 of player [0-9]+'s [0-9A-F]{8} at workbench [0-9A-F]{8} asked of the host as request [0-9]+" "$(win_read sws "$1")"; }
_sws_store_answered() { grep -qE "WorkshopSync: request [0-9]+ \(store\) at workbench [0-9A-F]{8}: $2" "$(win_read sws "$1")"; }
_sws_msgbox_open() { _menu_open "$1" MessageBoxMenu; }
_sws_box_done() { ! _sws_msgbox_open "$1" || _sws_store_asked "$1" "$2"; }   # _sws_box_done <who> <copy>: the box closed, or the store already asked (no E then)
_sws_box_closed() { ! _sws_msgbox_open "$1"; }
step_sharedws_store() {
	section "shared workshop: a friend stores the host's piece (the real Store key and the engine's yes/no box; console placement, SetOwnedByPlayer and throwaway saves on both)"
	local r label="sharedws-store"
	r=$(dc host rules)
	if [ "$(field "$r" sharedWorkshop)" != true ]; then fail "$label: set-up: the host's bSharedWorkshop is on" "rules say sharedWorkshop='$(field "$r" sharedWorkshop)'"; return; fi
	if [ "$(field "$r" sharedWorkshopEdit)" != true ]; then fail "$label: set-up: the host's bSharedWorkshopEdit is on" "rules say sharedWorkshopEdit='$(field "$r" sharedWorkshopEdit)'"; return; fi
	if [ "$(field "$(vm wsmode)" hooks)" != true ]; then fail "$label: set-up: the VM's pick hooks are installed (the store box's call among them)" "$(vm wsmode | cut -c1-160)"; return; fi
	if [ "$(field "$(vm wsmode)" edit)" != true ]; then fail "$label: set-up: the VM has the host's bSharedWorkshopEdit from the welcome" "$(vm wsmode | cut -c1-160)"; return; fi
	enter_both SanctuaryExt 14
	host "console $SANCT_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 2
	if [ "$(field "$(host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then fail "$label: set-up: Sanctuary is the host's" "$(host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer")"; return; fi
	vm "wsmode host" >/dev/null
	_sws_keys vm || return
	walk host "$label: set-up: the host walks to Sanctuary's workbench" 0.8 200 $SANCT_BENCH || { pads_off; return; }
	walk vm "$label: set-up: the VM walks to Sanctuary's workbench" 0.8 300 $SANCT_BENCH || { pads_off; return; }
	pads_off
	if _ws_enter host; then sleep 3; _ws_exit host; pass "$label: set-up: the host opened Sanctuary's workshop once (its workshop data made)" "ok"; else fail "$label: set-up: the host opened Sanctuary's workshop once" "$(host workshop)"; return; fi
	# the host's generator, the VM's light built as the host's and wired to it (the VM's copies of all three)
	_sws_one_way host vm || return
	local hg=$SWS_HG vg=$SWS_VG hl w vw c0 c1 vc0 vc1 inv0 a0 s0 st0 vst0 o0 gbase=$SWS_GENERATOR
	hl=$(grep -oE "WorkshopSync: [0-9A-F]{8} \(base $SWS_WIRED_LIGHT\) built" "$A_OUT/windelta-sws-host.txt" | head -1 | cut -d' ' -f2)
	w=$(host "wsinfo ${hl:-0}" | grep -oE 'power=[0-9A-F]{8}' | head -1 | cut -d= -f2)
	vw=$(_sws_copy_of vm "${w:-0}")
	if [ -z "$hg" ] || [ -z "$vg" ]; then fail "$label: set-up: the host's generator and the VM's copy of it" "generator '$hg' copy '$vg'"; return; fi
	crash_snapshot
	win_mark sws host; win_mark sws vm
	c0=$(_sws_stacks host); vc0=$(_sws_stacks vm); inv0=$(field "$(vm inventory)" hash)
	st0=$(_sws_stored_count host "$gbase"); vst0=$(_sws_stored_count vm "$gbase")
	a0=$(_sws_counter vm storesAsked); s0=$(_sws_counter host storesServed); o0=$(field "$(dc vm 'net status')" offThread)
	# the real Store key (Cancel) on the copy, then the engine's own yes/no box, answered Yes with the real key (Accept, else E)
	if ! _ws_enter vm; then fail "$label: the VM enters workshop mode at the host's workbench" "$(vm workshop) | $(vm menus | grep -oE 'open=[^ ]*')"; return; fi
	is_int "$vst0" || vst0=$(_sws_stored_count vm "$gbase")  # no list before the VM's game made this bench's workshop data: its workshop mode has now
	if ! _sws_put_away vm "$label"; then wait_for 8 _no_msgbox vm; _ws_exit vm; return; fi
	if _sws_aim_press vm "$vg" "$WS_KEY_STORE" select "$label"; then
		if wait_for 6 _sws_msgbox_open vm; then
			pass "$label: the VM's Store on its copy of the host's generator asks to confirm (the engine's own yes/no box, the host allowing it)" "pressed with $SWS_LAST_PRESS"
			local how=Accept kr
			# Accept, then wait for the box to close (as the scrap step does); E only while it stays open and no store was asked.
			# 2026-11-11: the box closed just after a 1 s look, E went to a closed box, and the check read it as stuck open.
			kr=$(dc vm "key $WS_KEY_SELECT 80"); sleep 1
			if ! wait_for 5 _sws_box_done vm "$vg"; then
				how=Activate; kr="$kr; then $(dc vm "key 45 80")"
				wait_for 5 _sws_box_done vm "$vg"
			fi
			wait_for 5 _sws_box_closed vm
			if _sws_msgbox_open vm; then fail "$label: set-up: the VM answers the box Yes" "the box stayed open after Accept ($WS_KEY_SELECT) and E: $kr; $(vm menus | grep -oE 'open=[^ ]*')"; dc vm "menu MessageBoxMenu hide" >/dev/null; sleep 1
			else pass "$label: set-up: the VM answers the box Yes" "with $how: $kr"; fi
		else
			fail "$label: the VM's Store on its copy of the host's generator asks to confirm (the engine's own yes/no box, the host allowing it)" "never opened: pressed with $SWS_LAST_PRESS; now $(vm menus | grep -oE 'open=[^ ]*'); $(grep -oE "WorkshopSync: (workshop mode's store of copy|the store) $vg[^;]*" "$(win_read sws vm)" | tail -1 | cut -c1-260)"
		fi
	fi
	sleep 2; wait_for 8 _no_msgbox vm; _ws_exit vm; sleep 2
	# asked, not stored here; served by the host's engine; answered applied
	if wait_for 10 _sws_store_asked vm "$vg"; then pass "$label: the box's Yes asked the host instead of storing the copy" "$(grep -oE "WorkshopSync: the store of our copy $vg[^;]*" "$A_OUT/windelta-sws-vm.txt" | head -1 | cut -c1-200)"
	else fail "$label: the box's Yes asked the host instead of storing the copy" "no 'asked of the host' line on the VM: $(grep -E 'WorkshopSync: (the store|workshop mode|request)' "$A_OUT/windelta-sws-vm.txt" | tail -3 | tr '\n' '|' | cut -c1-400)"; fi
	expect_eq "$label: the VM counts one store asked" "$(( $(_sws_counter vm storesAsked) - ${a0:-0} ))" 1
	expect_eq "$label: the box's Yes ran on the VM's main thread (no off-thread call counted across it)" "$(field "$(dc vm 'net status')" offThread)" "${o0:-0}"
	if wait_for 20 _sws_store_answered vm applied; then pass "$label: the host's game stored its generator and answered applied" "$(grep -oE 'WorkshopSync: request [0-9]+ \(store\)[^;]*' "$A_OUT/windelta-sws-vm.txt" | head -1)"
	else fail "$label: the host's game stored its generator and answered applied" "VM: $(grep -E 'WorkshopSync: request' "$A_OUT/windelta-sws-vm.txt" | tail -2 | tr '\n' '|'); host: $(grep -E "WorkshopSync: (player|our $hg)" "$(win_read sws host)" | tail -2 | tr '\n' '|' | cut -c1-400)"; fi
	if grep -qE "WorkshopSync: our $hg \(base $gbase\) at workbench [0-9A-F]{8} stored for player [0-9]+'s request [0-9]+: bench budget holotape store" "$(win_read sws host)"; then pass "$label: the host ran the engine's store (its holotape call first) inside the bench and budget scopes" "$(grep -oE "WorkshopSync: our $hg [^;]*" "$A_OUT/windelta-sws-host.txt" | head -1 | cut -c1-200)"
	else fail "$label: the host ran the engine's store (its holotape call first) inside the bench and budget scopes" "$(grep -E "WorkshopSync: (our $hg|player)" "$A_OUT/windelta-sws-host.txt" | tail -2 | tr '\n' '|' | cut -c1-300)"; fi
	expect_eq "$label: the host counts one store served" "$(( $(_sws_counter host storesServed) - ${s0:-0} ))" 1
	# the host's generator gone, the wire with it (the engine's store destroys the wires strung to a piece, as its scrap does), its grid clean
	if wait_for 5 _sws_gone host "$hg"; then pass "$label: the host's generator is gone" "$(host "refinfo $hg" | cut -c1-120)"; else fail "$label: the host's generator is gone" "$(host "refinfo $hg" | cut -c1-200)"; fi
	if [ -n "$w" ]; then if wait_for 5 _sws_gone host "$w"; then pass "$label: the wire to it went with it" "$(host "refinfo $w" | cut -c1-120)"; else fail "$label: the wire to it went with it" "$(host "refinfo $w" | cut -c1-200)"; fi; fi
	local g; g=$(host "wsgrid $SANCT_BENCH")
	if grep -q "gone named=0" <<<"$g"; then pass "$label: the host's grids name no deleted reference after the store" "$(grep -oE 'copies named=[0-9]+ gone named=[0-9]+' <<<"$g")"; else fail "$label: the host's grids name no deleted reference after the store" "$(cut -c1-200 <<<"$g")"; fi
	# the stored list: the host's holds one more of the generator; the VM's own is unchanged; no yield anywhere (a store gives nothing back)
	local st1; st1=$(_sws_stored_count host "$gbase")
	if is_int "$st0" && is_int "$st1" && [ $(( st1 - st0 )) -eq 1 ]; then pass "$label: the host's stored list holds one more of the generator (the host's to place back free)" "$st0 -> $st1"
	else fail "$label: the host's stored list holds one more of the generator (the host's to place back free)" "'$st0' -> '$st1': $(host "wsstored $SANCT_BENCH" | cut -c1-200)"; fi
	_sws_unchanged "$label: the VM's own stored list is unchanged (nothing was stored there)" "$vst0" "$(_sws_stored_count vm "$gbase")" int
	c1=$(_sws_stacks host); vc1=$(_sws_stacks vm)
	if is_int "$c1" && is_int "$c0" && [ "$c1" -le "$c0" ]; then pass "$label: the host's workbench holds no more items than before (a store yields nothing)" "$c0 -> $c1"; else fail "$label: the host's workbench holds no more items than before (a store yields nothing)" "$c0 -> $c1: $(host "contents $SANCT_BENCH" | cut -c1-200)"; fi
	_sws_unchanged "$label: the VM's own workbench is unchanged" "$vc0" "$vc1" int
	_sws_unchanged "$label: the VM's inventory is unchanged" "$inv0" "$(field "$(vm inventory)" hash)"
	# the VM's copies of the generator and the wire gone with the host's report
	if wait_for 15 _sws_no_copy vm "$hg"; then pass "$label: the VM's copy of the generator went with the host's report" "no copy of $hg (was $vg)"; else fail "$label: the VM's copy of the generator went with the host's report" "$(vm "refinfo $vg" | cut -c1-160); $(vm 'wspower copies' | cut -c1-160)"; fi
	if [ -n "$w" ] && [ -n "$vw" ]; then if wait_for 10 _sws_no_copy vm "$w"; then pass "$label: ... and its copy of the wire" "no copy of $w (was $vw)"; else fail "$label: ... and its copy of the wire" "$(vm "refinfo $vw" | cut -c1-160)"; fi; fi
	_sws_grid_clean vm "$label: the VM's grids name no copy and no runtime reference after the store"
	_sws_audit_clean vm "$label: the VM's audit finds nothing tied in after the store"
	# an in-session save on the VM, then loaded alone: no stray generator, grids clean
	vm "console save $SWS_STORE_SAVE" >/dev/null; sleep 8; wait_ingame vm 30
	_sws_grid_clean vm "$label: after an in-session save, the VM's grids still name no copy and no runtime reference"
	vm "net leave" >/dev/null; sleep 4
	vm "console load $SWS_STORE_SAVE" >/dev/null; sleep 20; wait_ingame vm 60
	_sws_grid_clean vm "$label: loaded alone, the VM's grids name no copy and no runtime reference"
	_sws_audit_clean vm "$label: loaded alone, the VM's audit finds no copy with power links"
	if _sws_gone vm "$vg"; then pass "$label: loaded alone, the VM's old copy of the generator is not back (no stray in the save)" "$(vm "refinfo $vg" | cut -c1-120)"; else fail "$label: loaded alone, the VM's old copy of the generator is not back (no stray in the save)" "$(vm "refinfo $vg" | cut -c1-200)"; fi
	_sws_rejoin "$label" || return
	enter_both SanctuaryExt 14
	# the host places the generator back from its stored list (the dev stand-in: console placement, the finish, the stored entry
	# taken off the list as the engine's own placement from the store takes it), and the VM gets a copy of it again
	win_mark sws host; win_mark sws vm
	local out g2; out=$(host "wsreport place $gbase $SANCT_BENCH"); g2=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
	if [ -n "$g2" ]; then
		_sws_placed host "$g2"; sleep 2; host "wsfinish $g2 $SANCT_BENCH" >/dev/null; sleep 1
		out=$(host "wsstored $SANCT_BENCH use $g2")
		if grep -q "taken=true" <<<"$out"; then pass "$label: the host placed the generator back from its stored list (console placement, the finish, and the stored entry taken)" "$out"; else fail "$label: the host placed the generator back from its stored list (console placement, the finish, and the stored entry taken)" "$out"; fi
		_sws_unchanged "$label: the host's stored list is back to what it was" "$st0" "$(_sws_stored_count host "$gbase")" int
		if wait_for 15 _sws_has_copy vm "$g2"; then pass "$label: the VM has a copy of the generator placed back" "copy $(_sws_copy_of vm "$g2") of $g2"; else fail "$label: the VM has a copy of the generator placed back" "$(vm 'wspower copies' | cut -c1-200)"; fi
	else
		fail "$label: set-up: the generator is placed back" "$out"
	fi
	_sws_cleanup
}

# ---- the shared workshop's move request: a friend moves the host's piece (opt-in: smoke.sh --only sharedws-move) -------------------
# The host builds a generator and a sleeping bag (console placement, said so; the host's pieces, the VM's copies). Three parts.
# The host's side alone (the dev wsmove on the host's own piece, the steps a friend's request runs): the bag moved a step, the
# VM's copy following through the piece stream; the generator and a spot in another cell refused with their reasons, nothing
# moved. The request (the dev wsmove on the VM's copy: the copy put a step on and its move asked, as the drop of a grabbed copy
# asks it): the host's game moves its bag with the engine's steps and answers applied, both stand at the spot, nothing is
# yielded or counted anywhere; the copy of the generator asked the same way is refused (a power piece) and put back where it
# stood. The engine's grab (SWS_MOVE_GRAB=1, the default; wsmode grab on: the dev switch it sits behind): with the switch off
# the real Accept key on the copy is refused; with it on, the key picks the copy up in workshop mode, Cancel lets it go where
# it stood and nothing is asked; picked up again, the VM turns its view (the held piece goes where it looks: the player's own
# way of moving it, no setpos) until the engine says it can be put down, Accept puts it down, the move is asked, and the
# host's bag ends where the VM's copy stands. Then the VM's grids are clean after an in-session save and loaded alone (no stray
# bag). Never run before this branch's first rig run: the grab part's aim (where the VM looks to carry the bag) may need tuning.
# Console placement and SetOwnedByPlayer, said so; throwaway saves on the VM.
SWS_MOVE_SAVE=${SWS_MOVE_SAVE:-f4mp_sharedws_move}
SWS_MOVE_PIECE=${SWS_MOVE_PIECE:-$SWS_BED}   # the piece that is moved: no power, nothing standing on it (the sleeping bag: furniture, so the furniture steps mean something)
SWS_MOVE_GRAB=${SWS_MOVE_GRAB:-1}            # 0: the host's side and the request alone, without the engine's grab on the VM
SWS_MOVE_STEPS='furnitureTaken stackLink(\(unlinked\))? transform static furnitureFree movedEvent navmesh resources'
_sws_place() { local i; i=$(dc "$1" "refinfo $2"); echo "$(field "$i" x) $(field "$i" y) $(field "$i" z) $(field "$i" rz)"; }   # "x y z heading(deg)" of that game's reference
_sws_near() {  # _sws_near "<x y ..>" "<x y ..>" [units]: both were read, and they are within that many units of each other on the ground (2)
	awk -v a="$1" -v b="$2" -v t="${3:-2}" 'BEGIN{ if (split(a, p, " ") < 2 || split(b, q, " ") < 2) exit 1; dx = p[1] - q[1]; dy = p[2] - q[2]; exit !(sqrt(dx * dx + dy * dy) <= t) }'
}
_sws_at() { _sws_near "$(_sws_place "$1" "$2")" "$3" "${4:-2}"; }   # _sws_at <who> <ref> "<x y ..>" [units]
_sws_step() {  # _sws_step "<x y z rz>" <units>: the spot that many units along x, toward the middle of its cell (never across a cell's edge: a move into another cell is refused)
	awk -v a="$1" -v d="$2" 'BEGIN{ if (split(a, p, " ") < 4) exit 1; o = p[1] - 4096 * int(p[1] / 4096); if (o < 0) o += 4096; printf "%.1f %s %s %s", p[1] + (o > 2048 ? -d : d), p[2], p[3], p[4] }'
}
_sws_move_asked() { grep -qE "WorkshopSync: the move of our copy $2 of player [0-9]+'s [0-9A-F]{8} at workbench [0-9A-F]{8} from .* asked of the host as request [0-9]+" "$(win_read sws "$1")"; }
_sws_move_answered() { grep -qE "WorkshopSync: request [0-9]+ \(move\) of our copy $2 at workbench [0-9A-F]{8}: $3" "$(win_read sws "$1")"; }
_sws_move_logged() { grep -qE "$2" "$(win_read sws "$1")"; }
_sws_held_is() { [ "$(field "$(dc "$1" workshop)" held)" = "$2" ]; }
_sws_not_holding() { ! _sws_holding "$1"; }
_sws_can_place() { local w; w=$(dc "$1" workshop); [ "$(field "$w" canPlace)" = true ] && [ "$(field "$w" held)" = "$2" ]; }
step_sharedws_move() {
	section "shared workshop: a friend moves the host's piece (the host's steps, the request, then the engine's grab behind its dev switch; console placement, SetOwnedByPlayer and throwaway saves)"
	local r label="sharedws-move"
	r=$(dc host rules)
	if [ "$(field "$r" sharedWorkshop)" != true ]; then fail "$label: set-up: the host's bSharedWorkshop is on" "rules say sharedWorkshop='$(field "$r" sharedWorkshop)'"; return; fi
	if [ "$(field "$r" sharedWorkshopEdit)" != true ]; then fail "$label: set-up: the host's bSharedWorkshopEdit is on" "rules say sharedWorkshopEdit='$(field "$r" sharedWorkshopEdit)'"; return; fi
	if [ "$(field "$(vm wsmode)" hooks)" != true ]; then fail "$label: set-up: the VM's pick hooks are installed (the grab key's among them)" "$(vm wsmode | cut -c1-160)"; return; fi
	if [ "$(field "$(vm wsmode)" edit)" != true ]; then fail "$label: set-up: the VM has the host's bSharedWorkshopEdit from the welcome" "$(vm wsmode | cut -c1-160)"; return; fi
	expect_eq "$label: copies are not picked up in workshop mode until the dev switch says so (wsmode grab off by default)" "$(_sws_counter vm grab)" off
	enter_both SanctuaryExt 14
	host "console $SANCT_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 2
	if [ "$(field "$(host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then fail "$label: set-up: Sanctuary is the host's" "$(host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer")"; return; fi
	vm "wsmode host" >/dev/null
	_sws_keys vm || return
	walk host "$label: set-up: the host walks to Sanctuary's workbench" 0.8 200 $SANCT_BENCH || { pads_off; return; }
	walk vm "$label: set-up: the VM walks to Sanctuary's workbench" 0.8 300 $SANCT_BENCH || { pads_off; return; }
	pads_off
	if _ws_enter host; then sleep 3; _ws_exit host; pass "$label: set-up: the host opened Sanctuary's workshop once (its workshop data made)" "ok"; else fail "$label: set-up: the host opened Sanctuary's workshop once" "$(host workshop)"; return; fi
	# the host's generator (where wsreport place puts it) and its sleeping bag 300 units beside it, toward the middle of the cell, where
	# the steps below move it on (2026-11-11: put 200 units along +x, the steps toward the middle brought it back to 40 units from the generator, and the
	# engine never let the grabbed copy be put down there: placeStatus 3, intersecting); the VM's copies of both
	local out hg hb vg vb g0 b0 b1 b2 gto far
	win_mark sws host; win_mark sws vm
	out=$(host "wsreport place $SWS_GENERATOR $SANCT_BENCH"); hg=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
	if [ -z "$hg" ]; then fail "$label: set-up: the host's generator is placed" "$out"; return; fi
	_sws_placed host "$hg"; sleep 2
	out=$(host "wsfinish $hg $SANCT_BENCH")
	case "$out" in finished*) ;; *) fail "$label: set-up: the host's finish ran on its generator" "$out"; _sws_cleanup; return;; esac
	g0=$(_sws_place host "$hg")
	hb=$(_sws_host_piece_at "$SWS_MOVE_PIECE" $(_sws_step "$g0" 300 | cut -d" " -f1-3) "$label")
	if [ -z "$hb" ]; then _sws_cleanup; return; fi
	wait_for 15 _sws_has_copy vm "$hg"; wait_for 15 _sws_has_copy vm "$hb"
	vg=$(_sws_copy_of vm "$hg"); vb=$(_sws_copy_of vm "$hb")
	if [ -z "$vg" ] || [ -z "$vb" ]; then fail "$label: set-up: the VM has copies of the host's generator and sleeping bag" "generator $hg copy '$vg', bag $hb copy '$vb'; $(vm 'wspower copies' | cut -c1-160)"; _sws_cleanup; return; fi
	b0=$(_sws_place host "$hb"); b1=$(_sws_step "$b0" 120); b2=$(_sws_step "$b0" 240); gto=$(_sws_step "$g0" 100)
	far=$(awk -v a="$b0" 'BEGIN{ split(a, p, " "); printf "%.1f %s %s %s", p[1] + 8192, p[2], p[3], p[4] }')
	if [ -z "$b1" ] || [ -z "$b2" ] || [ -z "$gto" ]; then fail "$label: set-up: where the host's pieces stand" "generator '$g0' bag '$b0'"; _sws_cleanup; return; fi
	if _sws_at vm "$vb" "$b0" 5; then pass "$label: set-up: the VM's copy of the bag stands where the host's bag does" "host $b0 | VM $(_sws_place vm "$vb")"; else fail "$label: set-up: the VM's copy of the bag stands where the host's bag does" "host $b0 | VM $(_sws_place vm "$vb")"; fi
	crash_snapshot
	local c0 vc0 inv0 bud0 vbud0 a0 s0 p0 l0 o0
	c0=$(_sws_stacks host); vc0=$(_sws_stacks vm); inv0=$(field "$(vm inventory)" hash); bud0=$(_sws_budget host); vbud0=$(_sws_budget vm)
	a0=$(_sws_counter vm movesAsked); s0=$(_sws_counter host movesServed); p0=$(_sws_counter vm movesPutBack); o0=$(field "$(dc vm 'net status')" offThread)

	# 1. the host's side alone: its own piece moved with the steps a friend's request runs (the dev wsmove), the copy following
	win_mark sws host; win_mark sws vm
	out=$(host "wsmove $hb $b1")
	if grep -qE "^moved $hb \(workbench [0-9A-F]{8}\) from .* to .*: $SWS_MOVE_STEPS \| arrived=true at " <<<"$out"; then pass "$label: the host's game moves its bag with the engine's steps of a moved piece's drop (the dev wsmove)" "$(cut -c1-260 <<<"$out")"
	else fail "$label: the host's game moves its bag with the engine's steps of a moved piece's drop (the dev wsmove)" "$(cut -c1-300 <<<"$out")"; _sws_cleanup; return; fi
	if _sws_at host "$hb" "$b1"; then pass "$label: the host's bag stands at the spot" "$(_sws_place host "$hb")"; else fail "$label: the host's bag stands at the spot" "asked '$b1', stands at '$(_sws_place host "$hb")'"; fi
	if wait_for 15 _sws_at vm "$vb" "$b1"; then pass "$label: the VM's copy followed it there (the host's report of its moved piece, the piece stream alone)" "$(_sws_place vm "$vb")"
	else fail "$label: the VM's copy followed it there (the host's report of its moved piece, the piece stream alone)" "host at '$b1', the copy at '$(_sws_place vm "$vb")'"; fi
	out=$(host "wsmove $hg $gto")
	if grep -q "^refused: the piece is wired or powered" <<<"$out" && _sws_at host "$hg" "$g0" 1; then pass "$label: the host's game refuses to move a power piece that way (its generator: a plain refusal, nothing moved)" "$(cut -c1-200 <<<"$out")"
	else fail "$label: the host's game refuses to move a power piece that way (its generator: a plain refusal, nothing moved)" "$(cut -c1-240 <<<"$out"); stands at '$(_sws_place host "$hg")', stood at '$g0'"; fi
	out=$(host "wsmove $hb $far")
	if grep -q "^refused: too far from the workbench" <<<"$out" && grep -q "sameCell=false" <<<"$out" && _sws_at host "$hb" "$b1" 1; then pass "$label: ... and a move into another cell (out of reach: nothing moved)" "$(cut -c1-200 <<<"$out")"
	else fail "$label: ... and a move into another cell (out of reach: nothing moved)" "$(cut -c1-240 <<<"$out"); stands at '$(_sws_place host "$hb")'"; fi
	out=$(vm "wsmove $vb $far")
	case "$out" in "error: "*"another cell"*) pass "$label: the VM's game asks no move into another cell (its copy not put there)" "$(cut -c1-160 <<<"$out")";; *) fail "$label: the VM's game asks no move into another cell (its copy not put there)" "$(cut -c1-200 <<<"$out")";; esac

	# 2. the request: the VM's copy put a step on and its move asked (the dev wsmove on a copy: what the drop of a grabbed copy asks)
	win_mark sws host; win_mark sws vm
	out=$(vm "wsmove $vb $b2")
	case "$out" in "copy $vb put at "*"asked of the host as request "*) pass "$label: the VM's move of its copy of the bag is asked of the host" "$(cut -c1-200 <<<"$out")";; *) fail "$label: the VM's move of its copy of the bag is asked of the host" "$(cut -c1-240 <<<"$out")"; _sws_cleanup; return;; esac
	if wait_for 10 _sws_move_asked vm "$vb"; then pass "$label: ... as a numbered request naming the host's piece and both spots" "$(grep -oE "WorkshopSync: the move of our copy $vb[^;]*" "$A_OUT/windelta-sws-vm.txt" | head -1 | cut -c1-240)"
	else fail "$label: ... as a numbered request naming the host's piece and both spots" "$(grep -E 'WorkshopSync: (the move|request)' "$A_OUT/windelta-sws-vm.txt" | tail -3 | tr '\n' '|' | cut -c1-400)"; fi
	if wait_for 20 _sws_move_answered vm "$vb" applied; then pass "$label: the host's game moved its bag and answered applied" "$(grep -oE 'WorkshopSync: request [0-9]+ \(move\)[^;]*' "$A_OUT/windelta-sws-vm.txt" | head -1)"
	else fail "$label: the host's game moved its bag and answered applied" "VM: $(grep -E 'WorkshopSync: request' "$A_OUT/windelta-sws-vm.txt" | tail -2 | tr '\n' '|'); host: $(grep -E "WorkshopSync: (player|our $hb|the move of our $hb)" "$(win_read sws host)" | tail -2 | tr '\n' '|' | cut -c1-400)"; fi
	if _sws_move_logged host "WorkshopSync: our $hb at workbench [0-9A-F]{8} moved for player [0-9]+'s request [0-9]+ from .* to .*: $SWS_MOVE_STEPS"; then pass "$label: the host ran the engine's steps of a moved piece's drop for the request, in their order" "$(grep -oE "WorkshopSync: our $hb at workbench [^;]*" "$A_OUT/windelta-sws-host.txt" | head -1 | cut -c1-300)"
	else fail "$label: the host ran the engine's steps of a moved piece's drop for the request, in their order" "$(grep -E "WorkshopSync: (our $hb|the move of our $hb|player)" "$A_OUT/windelta-sws-host.txt" | tail -2 | tr '\n' '|' | cut -c1-400)"; fi
	expect_eq "$label: the VM counts one move asked" "$(( $(_sws_counter vm movesAsked) - ${a0:-0} ))" 1
	expect_eq "$label: the host counts two moves made (its own probe's and the request's: the refused ones made none)" "$(( $(_sws_counter host movesServed) - ${s0:-0} ))" 2
	if wait_for 5 _sws_at host "$hb" "$b2"; then pass "$label: the host's bag stands where the VM put its copy" "$(_sws_place host "$hb")"; else fail "$label: the host's bag stands where the VM put its copy" "asked '$b2', stands at '$(_sws_place host "$hb")'"; fi
	sleep 3   # the host's report of its moved piece has reached the VM by now: the copy stays where it is
	if _sws_at vm "$vb" "$b2"; then pass "$label: the VM's copy stands there too, after the host's report" "$(_sws_place vm "$vb")"; else fail "$label: the VM's copy stands there too, after the host's report" "asked '$b2', stands at '$(_sws_place vm "$vb")'"; fi
	expect_eq "$label: the VM's copy is still the host's copy (one copy of the bag, the same reference)" "$(_sws_copy_of vm "$hb")" "$vb"
	expect_eq "$label: ... and still out of workshop mode's own reach (blocked)" "$(field "$(vm "wsinfo $vb")" blocked)" true
	expect_eq "$label: the host's bag is still a piece of Sanctuary's workbench, alive" "$(field "$(host "wsinfo $hb")" deleted) $(field "$(host "wsinfo $hb")" disabled)" "false false"
	local g; g=$(host "wsgrid $SANCT_BENCH")
	if grep -q "gone named=0" <<<"$g"; then pass "$label: the host's grids name no deleted reference after the move" "$(grep -oE 'copies named=[0-9]+ gone named=[0-9]+' <<<"$g")"; else fail "$label: the host's grids name no deleted reference after the move" "$(cut -c1-200 <<<"$g")"; fi
	_sws_unchanged "$label: the host's workbench holds what it held (a move yields nothing)" "$c0" "$(_sws_stacks host)" int
	_sws_unchanged "$label: the VM's own workbench is unchanged" "$vc0" "$(_sws_stacks vm)" int
	_sws_unchanged "$label: the VM's inventory is unchanged" "$inv0" "$(field "$(vm inventory)" hash)"
	_sws_unchanged "$label: the host's build budget is what it was (a move counts nothing)" "$bud0" "$(_sws_budget host)"
	_sws_unchanged "$label: the VM's build budget is what it was" "$vbud0" "$(_sws_budget vm)"
	_sws_grid_clean vm "$label: the VM's grids name no copy and no runtime reference after the move"
	_sws_audit_clean vm "$label: the VM's audit finds nothing tied in after the move"
	# a power piece's copy asked the same way: the host refuses, the copy goes back where it stood
	win_mark sws host; win_mark sws vm
	local vg0; vg0=$(_sws_place vm "$vg")
	out=$(vm "wsmove $vg $gto")
	case "$out" in "copy $vg put at "*"asked of the host as request "*) pass "$label: the VM's move of its copy of the generator is asked (the dev command skips the grab key's own refusal)" "$(cut -c1-200 <<<"$out")";; *) fail "$label: the VM's move of its copy of the generator is asked (the dev command skips the grab key's own refusal)" "$(cut -c1-240 <<<"$out")";; esac
	if wait_for 20 _sws_move_answered vm "$vg" "the piece is wired or powered"; then pass "$label: the host's game refuses it: a power piece is not moved for a friend" "$(grep -oE 'WorkshopSync: request [0-9]+ \(move\)[^;]*' "$A_OUT/windelta-sws-vm.txt" | head -1)"
	else fail "$label: the host's game refuses it: a power piece is not moved for a friend" "VM: $(grep -E 'WorkshopSync: request' "$A_OUT/windelta-sws-vm.txt" | tail -2 | tr '\n' '|'); host: $(grep -E "WorkshopSync: (player|the move of our $hg)" "$(win_read sws host)" | tail -2 | tr '\n' '|' | cut -c1-400)"; fi
	if _sws_move_logged host "WorkshopSync: the move of our $hg for player [0-9]+'s request [0-9]+ refused \(.*power=true"; then pass "$label: ... for its power, said in the host's log" "$(grep -oE "WorkshopSync: the move of our $hg [^;]*" "$A_OUT/windelta-sws-host.txt" | head -1 | cut -c1-240)"
	else fail "$label: ... for its power, said in the host's log" "$(grep -E "WorkshopSync: (the move of our|player)" "$A_OUT/windelta-sws-host.txt" | tail -2 | tr '\n' '|' | cut -c1-300)"; fi
	if wait_for 10 _sws_at vm "$vg" "$vg0" 1; then pass "$label: the VM's copy of the generator is back where it stood" "$(_sws_place vm "$vg")"; else fail "$label: the VM's copy of the generator is back where it stood" "stood at '$vg0', stands at '$(_sws_place vm "$vg")'"; fi
	if _sws_at host "$hg" "$g0" 1; then pass "$label: the host's generator never moved" "$(_sws_place host "$hg")"; else fail "$label: the host's generator never moved" "stood at '$g0', stands at '$(_sws_place host "$hg")'"; fi
	expect_eq "$label: the VM counts one copy put back" "$(( $(_sws_counter vm movesPutBack) - ${p0:-0} ))" 1
	expect_eq "$label: the dev moves ran on the VM's main thread (no off-thread call counted across them)" "$(field "$(dc vm 'net status')" offThread)" "${o0:-0}"
	_sws_grid_clean vm "$label: the VM's grids name no copy and no runtime reference after the refused move"

	# 3. the engine's grab, behind its dev switch: refused while off; on, a cancel asks nothing, a drop elsewhere asks the move
	if [ "$SWS_MOVE_GRAB" = 1 ]; then
		local r0 ad0 l1 a1 w side here where="" kr
		if ! _ws_enter vm; then fail "$label: the VM enters workshop mode at the host's workbench" "$(vm workshop) | $(vm menus | grep -oE 'open=[^ ]*')"; _sws_cleanup; return; fi
		if ! _sws_put_away vm "$label"; then _ws_exit vm; _sws_cleanup; return; fi
		win_mark sws host; win_mark sws vm
		r0=$(_sws_counter vm grabsRefused)
		if _sws_aim_press vm "$vb" "$WS_KEY_SELECT" select "$label"; then
			sleep 1
			if _sws_not_holding vm && [ "$(( $(_sws_counter vm grabsRefused) - ${r0:-0} ))" -ge 1 ]; then pass "$label: with the switch off, the real Accept key on the copy picks nothing up (refused, as before)" "pressed with $SWS_LAST_PRESS"
			else fail "$label: with the switch off, the real Accept key on the copy picks nothing up (refused, as before)" "$(_sws_fields "$(vm workshop)") grabsRefused $r0 -> $(_sws_counter vm grabsRefused)"; _sws_put_away vm "$label"; fi
		fi
		vm "wsmode grab on" >/dev/null
		expect_eq "$label: set-up: the dev switch is on (wsmode grab on)" "$(_sws_counter vm grab)" on
		# picked up, then Cancel: let go where it stood, nothing asked
		ad0=$(_sws_counter vm grabsAdmitted); l1=$(_sws_counter vm movesLeft); a1=$(_sws_counter vm movesAsked); here=$(_sws_place vm "$vb")
		if _sws_aim_press vm "$vb" "$WS_KEY_SELECT" select "$label"; then
			if wait_for 6 _sws_held_is vm "$vb"; then pass "$label: with the switch on, the real Accept key picks the copy up (the engine's own grab)" "pressed with $SWS_LAST_PRESS; now $(_sws_fields "$(vm workshop)")"
			else fail "$label: with the switch on, the real Accept key picks the copy up (the engine's own grab)" "pressed with $SWS_LAST_PRESS; now $(_sws_fields "$(vm workshop)"); $(grep -oE "WorkshopSync: the grab of our copy $vb[^;]*" "$(win_read sws vm)" | tail -1 | cut -c1-240)"; fi
			if wait_for 5 _sws_move_logged vm "WorkshopSync: our player holds our copy $vb "; then pass "$label: ... and the VM's game follows the grab" "$(grep -oE "WorkshopSync: our player holds our copy $vb[^;]*" "$A_OUT/windelta-sws-vm.txt" | head -1 | cut -c1-160)"
			else fail "$label: ... and the VM's game follows the grab" "$(grep -E "WorkshopSync: (the grab|our player holds|our copy $vb)" "$A_OUT/windelta-sws-vm.txt" | tail -2 | tr '\n' '|' | cut -c1-300)"; fi
			expect_eq "$label: one grab admitted" "$(( $(_sws_counter vm grabsAdmitted) - ${ad0:-0} ))" 1
			dc vm "key $WS_KEY_STORE 80" >/dev/null   # Cancel while a piece is held: the engine puts it back where it was grabbed
			if wait_for 8 _sws_not_holding vm; then pass "$label: Cancel lets the copy go" "$(_sws_fields "$(vm workshop)")"; else fail "$label: Cancel lets the copy go" "$(_sws_fields "$(vm workshop)")"; _sws_put_away vm "$label"; fi
			if wait_for 8 _sws_move_logged vm "WorkshopSync: our copy $vb let go where it stood: nothing asked"; then pass "$label: a cancelled grab asks nothing of the host" "$(grep -oE "WorkshopSync: our copy $vb let go[^;]*" "$A_OUT/windelta-sws-vm.txt" | head -1 | cut -c1-160)"
			else fail "$label: a cancelled grab asks nothing of the host" "$(grep -E "WorkshopSync: (our copy $vb|the move of our copy|request)" "$A_OUT/windelta-sws-vm.txt" | tail -3 | tr '\n' '|' | cut -c1-400)"; fi
			expect_eq "$label: ... counted as let go, no move asked" "$(( $(_sws_counter vm movesLeft) - ${l1:-0} )) $(( $(_sws_counter vm movesAsked) - ${a1:-0} ))" "1 0"
			if wait_for 5 _sws_at vm "$vb" "$here" 1; then pass "$label: the copy stands where it stood" "$(_sws_place vm "$vb")"; else fail "$label: the copy stands where it stood" "stood at '$here', stands at '$(_sws_place vm "$vb")'"; fi
			expect_eq "$label: ... still the host's copy, out of workshop mode's own reach again (blocked)" "$(_sws_copy_of vm "$hb") $(field "$(vm "wsinfo $vb")" blocked)" "$vb true"
			_sws_unchanged "$label: the VM's build budget is what it was after the cancelled grab (the engine's put-back counted nothing here)" "$vbud0" "$(_sws_budget vm)"
		fi
		# picked up again and carried: the VM turns its view beside the generator's copy until the engine says the bag can be put down
		win_mark sws host; win_mark sws vm
		a1=$(_sws_counter vm movesAsked); s0=$(_sws_counter host movesServed)
		if _sws_aim_press vm "$vb" "$WS_KEY_SELECT" select "$label" && wait_for 6 _sws_held_is vm "$vb"; then
			for side in 220 -220 320 -320 420 -420; do
				kr=$(dc vm "turnto $vg 900 aim 0 $side"); sleep 2.5
				if _sws_can_place vm "$vb" && { sleep 0.5; _sws_can_place vm "$vb"; } && ! _sws_at vm "$vb" "$here" 40; then where=$side; break; fi
			done
			if [ -z "$where" ]; then
				fail "$label: set-up: the VM carries the held copy to a spot the engine lets it be put down at (its view turned beside the generator's copy)" "never canPlace=true away from where it stood, sides 220 to 420 tried: $(_sws_fields "$(vm workshop)"); last turn said '$(cut -c1-120 <<<"$kr")'"
				_sws_put_away vm "$label"
			else
				pass "$label: set-up: the VM carries the held copy to a spot the engine lets it be put down at (its view turned beside the generator's copy)" "side $where: $(_sws_fields "$(vm workshop)")"
				kr=$(dc vm "key $WS_KEY_SELECT 80")
				if wait_for 8 _sws_not_holding vm; then pass "$label: the real Accept key puts the copy down" "$kr; $(_sws_fields "$(vm workshop)")"; else fail "$label: the real Accept key puts the copy down" "$kr; $(_sws_fields "$(vm workshop)")"; _sws_put_away vm "$label"; fi
				if wait_for 10 _sws_move_asked vm "$vb"; then pass "$label: where it was put down is asked of the host as a move" "$(grep -oE "WorkshopSync: the move of our copy $vb[^;]*" "$A_OUT/windelta-sws-vm.txt" | head -1 | cut -c1-240)"
				else fail "$label: where it was put down is asked of the host as a move" "$(grep -E "WorkshopSync: (our copy $vb|the move of our copy|request)" "$A_OUT/windelta-sws-vm.txt" | tail -3 | tr '\n' '|' | cut -c1-400)"; fi
				if wait_for 20 _sws_move_answered vm "$vb" applied; then pass "$label: the host's game moved its bag there and answered applied" "$(grep -oE 'WorkshopSync: request [0-9]+ \(move\)[^;]*' "$A_OUT/windelta-sws-vm.txt" | head -1)"
				else fail "$label: the host's game moved its bag there and answered applied" "VM: $(grep -E 'WorkshopSync: request' "$A_OUT/windelta-sws-vm.txt" | tail -2 | tr '\n' '|'); host: $(grep -E "WorkshopSync: (player|our $hb|the move of our $hb)" "$(win_read sws host)" | tail -2 | tr '\n' '|' | cut -c1-400)"; fi
				expect_eq "$label: one move asked, one made by the host" "$(( $(_sws_counter vm movesAsked) - ${a1:-0} )) $(( $(_sws_counter host movesServed) - ${s0:-0} ))" "1 1"
				sleep 3
				here=$(_sws_place vm "$vb")
				if _sws_at host "$hb" "$here" 3 && ! _sws_near "$here" "$b2" 40; then pass "$label: the host's bag stands where the VM put its copy down, away from where it stood" "host $(_sws_place host "$hb") | VM $here | before $b2"
				else fail "$label: the host's bag stands where the VM put its copy down, away from where it stood" "host '$(_sws_place host "$hb")' | VM '$here' | before '$b2'"; fi
				expect_eq "$label: the VM's copy is still the host's copy, blocked again" "$(_sws_copy_of vm "$hb") $(field "$(vm "wsinfo $vb")" blocked)" "$vb true"
			fi
		fi
		_sws_put_away vm "$label"
		_ws_exit vm; sleep 2
		vm "wsmode grab off" >/dev/null
		expect_eq "$label: the VM's grab held nothing back and holds nothing at the end" "$(_sws_counter vm grabHeld) $(_sws_counter vm movesHeldBack) $(_sws_counter vm movesWaiting)" "00000000 0 0"
		_sws_unchanged "$label: the VM's build budget is what it was after the grabs (the engine's drop of the copy counted nothing here)" "$vbud0" "$(_sws_budget vm)"
		_sws_unchanged "$label: the host's build budget is what it was" "$bud0" "$(_sws_budget host)"
		_sws_unchanged "$label: the VM's own workbench is unchanged after the grabs" "$vc0" "$(_sws_stacks vm)" int
		_sws_unchanged "$label: the VM's inventory is unchanged after the grabs" "$inv0" "$(field "$(vm inventory)" hash)"
		_sws_grid_clean vm "$label: the VM's grids name no copy and no runtime reference after the grabs"
		_sws_audit_clean vm "$label: the VM's audit finds nothing tied in after the grabs"
		g=$(host "wsgrid $SANCT_BENCH")
		if grep -q "gone named=0" <<<"$g"; then pass "$label: the host's grids name no deleted reference after the grabs" "$(grep -oE 'copies named=[0-9]+ gone named=[0-9]+' <<<"$g")"; else fail "$label: the host's grids name no deleted reference after the grabs" "$(cut -c1-200 <<<"$g")"; fi
	fi

	# an in-session save on the VM, then loaded alone: no stray bag, grids clean
	vm "console save $SWS_MOVE_SAVE" >/dev/null; sleep 8; wait_ingame vm 30
	_sws_grid_clean vm "$label: after an in-session save, the VM's grids still name no copy and no runtime reference"
	vm "net leave" >/dev/null; sleep 4
	vm "console load $SWS_MOVE_SAVE" >/dev/null; sleep 20; wait_ingame vm 60
	_sws_grid_clean vm "$label: loaded alone, the VM's grids name no copy and no runtime reference"
	_sws_audit_clean vm "$label: loaded alone, the VM's audit finds no copy with power links"
	# the save keeps the VM's copy of the host's bag, as it keeps every copy (found again when the session sends its building after
	# a load: FindExisting; one no session keeps is a stray, shown unpowered): loaded alone, the copy is still out of workshop mode's
	# reach (the block keyword is saved with it)
	if _sws_gone vm "$vb"; then pass "$label: loaded alone, the VM's old copy of the bag is gone or kept out of workshop mode's reach" "gone: $(vm "refinfo $vb" | cut -c1-120)"
	elif [ "$(field "$(vm "wsinfo $vb")" blocked)" = true ]; then pass "$label: loaded alone, the VM's old copy of the bag is gone or kept out of workshop mode's reach" "kept, blocked: $(vm "refinfo $vb" | cut -c1-120)"
	else fail "$label: loaded alone, the VM's old copy of the bag is gone or kept out of workshop mode's reach" "kept, not blocked: $(vm "wsinfo $vb" | cut -c1-200)"; fi
	_vm_restart_rejoin sharedws-move
	enter_both SanctuaryExt 14
	if wait_for 20 _sws_has_copy vm "$hb" && _sws_at vm "$(_sws_copy_of vm "$hb")" "$(_sws_place host "$hb")" 5; then pass "$label: joined again, the VM's new copy of the bag stands where the host's bag was moved to" "host $(_sws_place host "$hb") | VM $(_sws_place vm "$(_sws_copy_of vm "$hb")")"
	else fail "$label: joined again, the VM's new copy of the bag stands where the host's bag was moved to" "host '$(_sws_place host "$hb")' | VM copy '$(_sws_copy_of vm "$hb")' at '$(_sws_place vm "$(_sws_copy_of vm "$hb")")'"; fi
	_sws_cleanup
}

# ---- settlement damage and a friend's repair (opt-in: smoke.sh --only sharedws-repair) -------------------------------------------
# The host builds a generator, the VM a construction light wired to it (the one-way set-up: the host's pieces, the VM's copies).
# The host's generator is then wrecked in the host's game: by the host's own gunfire (injected clicks on its window, as the rig
# rules allow for the host), or, when the generator has no destruction stages in this load order or outlasts the shots, by the dev
# stand-in `wsdamage wreck` (the destroyed flag and a health of 0 set as the engine's damage leaves them, without the damage); the
# report says which. Asserted: the VM's copy shows the host's damage within seconds (the engine's own destroyed flag on the copy,
# not only what was received). Then the VM, in workshop mode, aims at its wrecked copy and presses the real Repair key (Accept):
# first with the host's side refusing (`wsdamage repairs off`, the dev stand-in for bSharedWorkshopEdit off in the host's game
# alone: the VM still believes it may ask), then, when the host's workbench can't pay, with the bench as it is (the no-materials
# refusal), then with the cost put in the host's bench by console (additem). Asserted for the real repair: the press asked the
# host and repaired nothing in the VM's game, the host's game paid from its workbench (exactly the cost fewer items) and ran the
# engine's repair, the host's generator and the VM's copy are whole, and the VM's own bench and inventory are unchanged. Then the
# VM shoots its own copy (its game's `click`): the copy is whole again within seconds and the host's piece never changed. Then a
# late joiner: the VM saves in the session and loads alone (grids clean), the host's generator is wrecked again while the VM is
# away (dev), the VM's game is restarted and rejoins, and its new copy shows wrecked. Console placement and SetOwnedByPlayer, said
# so; no movement is injected; throwaway saves on the VM.
SWS_REPAIR_SAVE=${SWS_REPAIR_SAVE:-f4mp_sharedws_repair}
SWS_REPAIR_BURSTS=${SWS_REPAIR_BURSTS:-10}   # bursts of 3 shots the host fires at its generator before the dev stand-in wrecks it
_swr_of() { field "$(dc "$1" "wsdamage $2")" "$3"; }   # _swr_of <who> <ref> <field>: a field of that game's `wsdamage <ref>`
_swr_counter() { field "$(dc "$1" wsdamage)" "$2"; }
_swr_destroyed() { [ "$(_swr_of "$1" "$2" destroyed)" = "$3" ]; }   # _swr_destroyed <who> <ref> <0|1>: the engine's flag on it in that game
_swr_whole() { local d; d=$(dc "$1" "wsdamage $2"); [ "$(field "$d" destroyed)" = 0 ] && [ "$(field "$d" health)" = none ]; }
_swr_asked() { grep -qE "WorkshopSync: the repair of our copy $2 of player [0-9]+'s [0-9A-F]{8} at workbench [0-9A-F]{8} asked of the host as request [0-9]+" "$(win_read sws "$1")"; }
_swr_answered() { grep -qE "WorkshopSync: request [0-9]+ \(repair\) at workbench [0-9A-F]{8}: $2" "$(win_read sws "$1")"; }
# _swr_press <label> <what>: the VM enters workshop mode, aims at its copy $vg and presses the real Repair key; leaves workshop mode. 1 when the press never went.
_swr_press() {
	local label=$1 ok=1
	win_mark sws host; win_mark sws vm
	if ! _ws_enter vm; then fail "$label: set-up: the VM enters workshop mode at the host's workbench ($2)" "$(vm workshop) | $(vm menus | grep -oE 'open=[^ ]*')"; return 1; fi
	if _sws_put_away vm "$label ($2)" && _sws_aim_press vm "$SWR_VG" "$WS_KEY_REPAIR" select "$label ($2)"; then ok=0; fi
	sleep 2; wait_for 8 _no_msgbox vm; _ws_exit vm; sleep 1
	return $ok
}
step_sharedws_repair() {
	section "shared workshop: the host's damage on a friend's copies, and a friend's repair (the real Repair key; the host's injected gunfire or the dev wreck; console placement, SetOwnedByPlayer and throwaway saves)"
	local r label="sharedws-repair"
	r=$(dc host rules)
	if [ "$(field "$r" sharedWorkshop)" != true ]; then fail "$label: set-up: the host's bSharedWorkshop is on" "rules say sharedWorkshop='$(field "$r" sharedWorkshop)'"; return; fi
	if [ "$(field "$r" sharedWorkshopEdit)" != true ]; then fail "$label: set-up: the host's bSharedWorkshopEdit is on" "rules say sharedWorkshopEdit='$(field "$r" sharedWorkshopEdit)'"; return; fi
	if [ "$(field "$(vm wsmode)" hooks)" != true ]; then fail "$label: set-up: the VM's pick hooks are installed (the Accept key's among them)" "$(vm wsmode | cut -c1-160)"; return; fi
	if [ "$(field "$(vm wsmode)" edit)" != true ]; then fail "$label: set-up: the VM has the host's bSharedWorkshopEdit from the welcome" "$(vm wsmode | cut -c1-160)"; return; fi
	case "$(dc host wsdamage)" in "workshop damage:"*) ;; *) fail "$label: set-up: the host's build has the damage mirror (dev wsdamage)" "$(dc host wsdamage | cut -c1-160)"; return;; esac
	case "$(dc vm wsdamage)" in "workshop damage:"*) ;; *) fail "$label: set-up: the VM's build has the damage mirror (dev wsdamage)" "$(dc vm wsdamage | cut -c1-160)"; return;; esac
	host "wsdamage repairs on" >/dev/null; host "wsdamage mirror on" >/dev/null; vm "wsdamage mirror on" >/dev/null
	enter_both SanctuaryExt 14
	host "console $SANCT_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 2
	if [ "$(field "$(host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then fail "$label: set-up: Sanctuary is the host's" "$(host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer")"; return; fi
	vm "wsmode host" >/dev/null
	_sws_keys vm || return
	walk host "$label: set-up: the host walks to Sanctuary's workbench" 0.8 200 $SANCT_BENCH || { pads_off; return; }
	walk vm "$label: set-up: the VM walks to Sanctuary's workbench" 0.8 300 $SANCT_BENCH || { pads_off; return; }
	pads_off
	if _ws_enter host; then sleep 3; _ws_exit host; pass "$label: set-up: the host opened Sanctuary's workshop once (its workshop data and recipes made)" "ok"; else fail "$label: set-up: the host opened Sanctuary's workshop once" "$(host workshop)"; return; fi
	_sws_one_way host vm || return
	local hg=$SWS_HG vg=$SWS_VG d how="" i hh vh
	SWR_VG=$vg
	if [ -z "$hg" ] || [ -z "$vg" ]; then fail "$label: set-up: the host's generator and the VM's copy of it" "generator '$hg' copy '$vg'"; return; fi
	crash_snapshot
	# before any damage: both whole, the host's piece its own to tell, the VM's a copy that tells nothing
	d=$(host "wsdamage $hg")
	expect_eq "$label: set-up: the host's generator is its own piece, whole" "kind=$(field "$d" kind) destroyed=$(field "$d" destroyed) health=$(field "$d" health)" "kind=own destroyed=0 health=none"
	d=$(vm "wsdamage $vg")
	expect_eq "$label: set-up: the VM's copy of it is a copy, whole, and tracked by nothing of the VM's" "kind=$(field "$d" kind) destroyed=$(field "$d" destroyed)" "kind=copy destroyed=0"

	# ---- the damage: the host's gunfire, else the dev wreck -----------------------------------------------------------------
	local destructible; destructible=$(_swr_of host "$hg" destructible)
	if [ "$destructible" = 1 ]; then
		pads_off; _qa_arm host; host focus >/dev/null
		for i in $(seq 1 "$SWS_REPAIR_BURSTS"); do
			host "turnto $hg 400 aim 0" >/dev/null; sleep 0.6
			host_input "$label: the host's click reaches its game" -Click -HoldMs 400 || break
			sleep 0.5; host_input "$label: the host's click reaches its game" -Click -HoldMs 400 || break
			sleep 0.5; host_input "$label: the host's click reaches its game" -Click -HoldMs 400 || break
			sleep 1
			d=$(host "wsdamage $hg"); hh=$(field "$d" health)
			if [ "$(field "$d" destroyed)" = 1 ]; then how="the host's gunfire ($(( i * 3 )) shots)"; break; fi
			# damaged, not destroyed: the copy shows the same health (once: the first burst that left a health)
			if [ -z "$vh" ] && [ -n "$hh" ] && [ "$hh" != none ]; then
				sleep 3; hh=$(_swr_of host "$hg" health); vh=$(_swr_of vm "$vg" health)
				if awk -v a="$hh" -v b="$vh" 'BEGIN{ if (a == "" || b == "" || a == "none" || b == "none") exit 1; d = a - b; if (d < 0) d = -d; exit !(d <= 1) }'; then pass "$label: a generator damaged but standing shows the same health on the VM's copy" "host $hh, copy $vh"
				else fail "$label: a generator damaged but standing shows the same health on the VM's copy" "host '$hh', copy '$vh': $(vm "wsdamage $vg" | cut -c1-220)"; fi
			fi
		done
		host "draw off" >/dev/null
	fi
	if [ -z "$how" ]; then
		d=$(host "wsdamage wreck $hg")
		how="the dev stand-in wsdamage wreck (destructible=$destructible in the host's game$( [ "$destructible" = 1 ] && echo ", the shots left health $(_swr_of host "$hg" health)" ))"
		case "$d" in error*) fail "$label: set-up: the host's generator is wrecked" "$d"; _sws_cleanup; return;; esac
	fi
	if wait_for 5 _swr_destroyed host "$hg" 1; then pass "$label: the host's generator is destroyed in the host's game (by $how)" "$(host "wsdamage $hg" | cut -c1-200)"
	else fail "$label: the host's generator is destroyed in the host's game (by $how)" "$(host "wsdamage $hg" | cut -c1-200)"; _sws_cleanup; return; fi
	if wait_for 10 _swr_destroyed vm "$vg" 1; then pass "$label: the VM's copy shows the host's damage (the engine's destroyed flag on the copy, within 10 s)" "$(vm "wsdamage $vg" | cut -c1-220)"
	else fail "$label: the VM's copy shows the host's damage (the engine's destroyed flag on the copy, within 10 s)" "$(vm "wsdamage $vg" | cut -c1-260) | $(vm wsdamage | cut -c1-200)"; fi
	expect_eq "$label: ... and it is what the host said of it (not the VM's own damage)" "wanted=$(_swr_of vm "$vg" wanted) wantedDestroyed=$(_swr_of vm "$vg" wantedDestroyed)" "wanted=1 wantedDestroyed=1"
	expect_eq "$label: the VM reports nothing of a copy (it tracks no piece of the host's)" "$(_swr_of vm "$vg" tracked)" ""

	# ---- the cost, as the host's game works it out -----------------------------------------------------------------------------
	local bill cost total=0 payable line n
	bill=$(host "wsdamage cost $hg"); cost=$(grep -oE 'cost=[^ ]*' <<<"$bill" | cut -d= -f2); payable=$(field "$bill" payable)
	case "$bill" in piece=*recipe=[0-9A-F]*cost=*) ;; *) fail "$label: set-up: the host's game has the generator's recipe and its repair cost" "$bill"; _sws_cleanup; return;; esac
	for line in ${cost//,/ }; do n=${line%%x*}; is_int "$n" && total=$(( total + n )); done
	if [ "$total" -ge 1 ] && [ "$(field "$bill" needsRepair)" = 1 ]; then pass "$label: set-up: the host's game prices the repair (each recipe line times fWorkshopRepairComponentMult, one at least)" "$(cut -c1-240 <<<"$bill")"
	else fail "$label: set-up: the host's game prices the repair" "$bill"; _sws_cleanup; return; fi

	local a0 n0 s0 u0 c0 c1 vc0 vc1 inv0 o0
	# ---- refused: the host's side off ---------------------------------------------------------------------------------------
	host "wsdamage repairs off" >/dev/null
	a0=$(_swr_counter vm repairsAsked); s0=$(_swr_counter host repairsServed); c0=$(_sws_stacks host)
	if _swr_press "$label" "the host refusing"; then
		if wait_for 10 _swr_asked vm "$vg"; then pass "$label: with the host refusing, the VM's Repair on its wrecked copy asks the host" "pressed with $SWS_LAST_PRESS"
		else fail "$label: with the host refusing, the VM's Repair on its wrecked copy asks the host" "no 'asked of the host' line: pressed with $SWS_LAST_PRESS; $(grep -E 'WorkshopSync: (the repair|workshop mode|request)' "$A_OUT/windelta-sws-vm.txt" | tail -3 | tr '\n' '|' | cut -c1-400)"; fi
		if wait_for 15 _swr_answered vm "the host doesn't allow it"; then pass "$label: ... and the host's game refuses it (not allowed), said to the VM" "$(grep -oE 'WorkshopSync: request [0-9]+ \(repair\)[^;]*' "$A_OUT/windelta-sws-vm.txt" | head -1)"
		else fail "$label: ... and the host's game refuses it (not allowed), said to the VM" "$(grep -E 'WorkshopSync: request' "$A_OUT/windelta-sws-vm.txt" | tail -2 | tr '\n' '|'); host: $(grep -E "WorkshopSync: player" "$(win_read sws host)" | tail -2 | tr '\n' '|' | cut -c1-300)"; fi
		expect_eq "$label: ... the host repaired nothing" "$(( $(_swr_counter host repairsServed) - ${s0:-0} ))" 0
		_sws_unchanged "$label: ... the host's workbench is untouched" "$c0" "$(_sws_stacks host)" int
		expect_eq "$label: ... the host's generator and the VM's copy are still wrecked" "host=$(_swr_of host "$hg" destroyed) copy=$(_swr_of vm "$vg" destroyed)" "host=1 copy=1"
	fi
	host "wsdamage repairs on" >/dev/null
	expect_eq "$label: set-up: the host's side serves repairs again" "$(_swr_counter host repairs)" on

	# ---- refused: the workbench can't pay (only when it can't as it stands) -------------------------------------------------
	if [ "$payable" = 0 ]; then
		u0=$(_swr_counter host repairsUnpaid); c0=$(_sws_stacks host)
		if _swr_press "$label" "the bench lacking the cost"; then
			if wait_for 15 _swr_answered vm "the workbench lacks the materials"; then pass "$label: with the host's workbench lacking the cost, the repair is refused for materials" "$(grep -oE 'WorkshopSync: request [0-9]+ \(repair\)[^;]*' "$A_OUT/windelta-sws-vm.txt" | head -1)"
			else fail "$label: with the host's workbench lacking the cost, the repair is refused for materials" "$(grep -E 'WorkshopSync: request' "$A_OUT/windelta-sws-vm.txt" | tail -2 | tr '\n' '|'); host: $(grep -E "WorkshopSync: (player|our $hg)" "$(win_read sws host)" | tail -2 | tr '\n' '|' | cut -c1-300)"; fi
			expect_eq "$label: ... counted by the host as unpaid" "$(( $(_swr_counter host repairsUnpaid) - ${u0:-0} ))" 1
			_sws_unchanged "$label: ... nothing taken from the host's workbench (all of the cost or none)" "$c0" "$(_sws_stacks host)" int
			expect_eq "$label: ... the host's generator is still wrecked" "$(_swr_of host "$hg" destroyed)" 1
		fi
	else
		warn "$label: the no-materials refusal is not exercised this run" "the host's workbench could already pay ($(cut -c1-160 <<<"$bill")): run on a save whose Sanctuary bench is empty to exercise it"
	fi
	# the cost put in the host's bench (console additem, said so): each line's loose item, so the payment is exactly the cost
	for line in ${cost//,/ }; do
		n=${line%%x*}; r=${line##*/}
		is_int "$n" && [ -n "$r" ] && host "console $SANCT_BENCH.additem $r $n" >/dev/null
	done
	sleep 2; bill=$(host "wsdamage cost $hg")
	if [ "$(field "$bill" payable)" = 1 ]; then pass "$label: set-up: the host's workbench holds the cost (put there by console)" "$(cut -c1-200 <<<"$bill")"
	else fail "$label: set-up: the host's workbench holds the cost (put there by console)" "$bill"; _sws_cleanup; return; fi

	# ---- the repair --------------------------------------------------------------------------------------------------------
	a0=$(_swr_counter vm repairsAsked); n0=$(_swr_counter vm mended); s0=$(_swr_counter host repairsServed)
	c0=$(_sws_stacks host); vc0=$(_sws_stacks vm); inv0=$(field "$(vm inventory)" hash); o0=$(field "$(dc vm 'net status')" offThread)
	if _swr_press "$label" "the repair"; then
		if wait_for 10 _swr_asked vm "$vg"; then pass "$label: the VM's Repair on its wrecked copy asks the host instead of repairing the copy" "$(grep -oE "WorkshopSync: the repair of our copy $vg[^;]*" "$A_OUT/windelta-sws-vm.txt" | head -1 | cut -c1-220)"
		else fail "$label: the VM's Repair on its wrecked copy asks the host instead of repairing the copy" "no 'asked of the host' line: pressed with $SWS_LAST_PRESS; $(grep -E 'WorkshopSync: (the repair|workshop mode|request)' "$A_OUT/windelta-sws-vm.txt" | tail -3 | tr '\n' '|' | cut -c1-400)"; fi
		expect_eq "$label: the VM counts one repair asked" "$(( $(_swr_counter vm repairsAsked) - ${a0:-0} ))" 1
		expect_eq "$label: the press ran on the VM's main thread (no off-thread call counted across it)" "$(field "$(dc vm 'net status')" offThread)" "${o0:-0}"
		if wait_for 20 _swr_answered vm applied; then pass "$label: the host's game repaired its generator and answered applied" "$(grep -oE 'WorkshopSync: request [0-9]+ \(repair\)[^;]*' "$A_OUT/windelta-sws-vm.txt" | head -1)"
		else fail "$label: the host's game repaired its generator and answered applied" "VM: $(grep -E 'WorkshopSync: request' "$A_OUT/windelta-sws-vm.txt" | tail -2 | tr '\n' '|'); host: $(grep -E "WorkshopSync: (player|our $hg)" "$(win_read sws host)" | tail -2 | tr '\n' '|' | cut -c1-400)"; fi
		if grep -qE "WorkshopSync: our $hg \(base $SWS_GENERATOR\) at workbench [0-9A-F]{8} repaired for player [0-9]+'s request [0-9]+: paid .* \| repaired$" "$(win_read sws host)"; then pass "$label: the host paid from its workbench and ran the engine's repair, which left the piece whole" "$(grep -oE "WorkshopSync: our $hg [^;]*" "$A_OUT/windelta-sws-host.txt" | head -1 | cut -c1-240)"
		else fail "$label: the host paid from its workbench and ran the engine's repair, which left the piece whole" "$(grep -E "WorkshopSync: (our $hg|player)" "$A_OUT/windelta-sws-host.txt" | tail -2 | tr '\n' '|' | cut -c1-400)"; fi
		expect_eq "$label: the host counts one repair served" "$(( $(_swr_counter host repairsServed) - ${s0:-0} ))" 1
		if wait_for 5 _swr_whole host "$hg"; then pass "$label: the host's generator is whole (no destroyed flag, no health kept)" "$(host "wsdamage $hg" | cut -c1-200)"; else fail "$label: the host's generator is whole (no destroyed flag, no health kept)" "$(host "wsdamage $hg" | cut -c1-240)"; fi
		if wait_for 10 _swr_whole vm "$vg"; then pass "$label: the VM's copy is whole again, mended by the host's word (within 10 s)" "$(vm "wsdamage $vg" | cut -c1-220)"; else fail "$label: the VM's copy is whole again, mended by the host's word (within 10 s)" "$(vm "wsdamage $vg" | cut -c1-260) | $(vm wsdamage | cut -c1-200)"; fi
		expect_ge "$label: ... by the mirror's mend (the VM's game ran no repair of its own)" "$(( $(_swr_counter vm mended) - ${n0:-0} ))" 1
		c1=$(_sws_stacks host); vc1=$(_sws_stacks vm)
		if is_int "$c0" && is_int "$c1"; then expect_eq "$label: the host's workbench paid exactly the cost (items fewer)" "$(( c0 - c1 ))" "$total"; else fail "$label: the host's workbench paid exactly the cost (items fewer)" "not read: before '$c0', after '$c1'"; fi
		_sws_unchanged "$label: the VM's own workbench is unchanged (it paid nothing)" "$vc0" "$vc1" int
		_sws_unchanged "$label: the VM's inventory is unchanged" "$inv0" "$(field "$(vm inventory)" hash)"
		expect_eq "$label: both games still stand (no crash across the repair)" "$(wait_ingame host 10 >/dev/null 2>&1 && echo up || echo down)/$(wait_ingame vm 10 >/dev/null 2>&1 && echo up || echo down)" "up/up"
	fi

	# ---- the VM's own shots at its copy: put back, and never the host's piece's -------------------------------------------
	local m0 w0 hs0
	if _swr_whole vm "$vg"; then
		m0=$(_swr_counter vm mended); w0=$(_swr_counter vm wrecked); hs0=$(host "wsdamage $hg" | grep -oE 'said=[0-9] saidHealth=[^ ]* saidDestroyed=[0-9]')
		_qa_arm vm
		for i in 1 2 3 4 5 6; do vm "turnto $vg 400 aim 0" >/dev/null; sleep 0.6; vm "click 150" >/dev/null; sleep 0.8; done
		vm "draw off" >/dev/null
		if wait_for 8 _swr_whole vm "$vg"; then pass "$label: after the VM shoots its own copy, the copy is whole again (as the host's piece is)" "$(vm "wsdamage $vg" | cut -c1-200); mended +$(( $(_swr_counter vm mended) - ${m0:-0} ))"
		else fail "$label: after the VM shoots its own copy, the copy is whole again (as the host's piece is)" "$(vm "wsdamage $vg" | cut -c1-260)"; fi
		if [ "$(( $(_swr_counter vm mended) - ${m0:-0} ))" -ge 1 ]; then pass "$label: ... the copy did take the VM's shots and was put back (the mirror's mend ran)" "mended +$(( $(_swr_counter vm mended) - ${m0:-0} ))"
		else warn "$label: the VM's shots left no damage on its copy to put back" "mended +0 (destructible=$(_swr_of vm "$vg" destructible) in the VM's game; six shots): the put-back is not exercised this run"; fi
		expect_eq "$label: ... the VM's shots never wrecked the copy to a word of the host's (nothing new was said)" "$(( $(_swr_counter vm wrecked) - ${w0:-0} ))" 0
		expect_eq "$label: ... and the host's generator never changed (whole, and what the host said of it as before)" "$(_swr_of host "$hg" destroyed)/$(_swr_of host "$hg" health)/$(host "wsdamage $hg" | grep -oE 'said=[0-9] saidHealth=[^ ]* saidDestroyed=[0-9]')" "0/none/$hs0"
	fi

	# ---- a save in the session, loaded alone; then a late joiner ------------------------------------------------------------
	vm "console save $SWS_REPAIR_SAVE" >/dev/null; sleep 8; wait_ingame vm 30
	_sws_grid_clean vm "$label: after an in-session save, the VM's grids name no copy and no runtime reference"
	vm "net leave" >/dev/null; sleep 4
	vm "console load $SWS_REPAIR_SAVE" >/dev/null; sleep 20; wait_ingame vm 60
	_sws_grid_clean vm "$label: loaded alone, the VM's grids name no copy and no runtime reference"
	_sws_audit_clean vm "$label: loaded alone, the VM's audit finds no copy with power links"
	expect_eq "$label: loaded alone, the VM mirrors nobody's damage" "$(_swr_counter vm mirrored)" 0
	# the host's generator wrecked again while the VM is away (the dev stand-in), then the VM's game restarted and joined again
	d=$(host "wsdamage wreck $hg")
	if wait_for 5 _swr_destroyed host "$hg" 1; then pass "$label: set-up: the host's generator is wrecked again while the VM is away (dev wsdamage wreck)" "$(cut -c1-160 <<<"$d")"; else fail "$label: set-up: the host's generator is wrecked again while the VM is away" "$d"; fi
	_vm_restart_rejoin sharedws-repair
	enter_both SanctuaryExt 14
	vm "wsmode host" >/dev/null
	local vg2; wait_for 30 _sws_has_copy vm "$hg"; vg2=$(_sws_copy_of vm "$hg")
	if [ -n "$vg2" ] && wait_for 45 _swr_destroyed vm "$vg2" 1; then pass "$label: a VM that joins after the damage sees its new copy wrecked (the host's snapshot, within 45 s)" "$(vm "wsdamage $vg2" | cut -c1-220)"
	else fail "$label: a VM that joins after the damage sees its new copy wrecked (the host's snapshot, within 45 s)" "copy '$vg2': $(vm "wsdamage ${vg2:-0}" | cut -c1-260) | $(vm wsdamage | cut -c1-200)"; fi
	expect_eq "$label: no piece's damage was dropped or capped in either game" "$(_swr_counter host capped)/$(_swr_counter host dropped)/$(_swr_counter vm capped)/$(_swr_counter vm dropped)" "0/0/0/0"
	_sws_cleanup
}

# ---- the shared workshop's assign request: a friend gives the host's settler a job (opt-in: smoke.sh --only sharedws-assign) -------
# Sanctuary is the host's, and Codsworth (a plugin's NPC both games have, standing in Sanctuary) is made its settler in the host's
# game only by console (WorkshopParentScript.AddActorToWorkshopPUBLIC). The VM's game does not hold him as a settler: the host's
# game names him (common/SettlerRoster.h) and the VM's workshop mode offers Command on him from that. Both players walk to him (real movement);
# the host places a sleeping bag there (console placement and the engine's finish) and the VM gets its copy. Then the VM, in
# workshop mode, commands Codsworth with the real key (Accept with him highlighted) and gives him its copy of the host's bed with
# the real key (Accept with the copy highlighted): the engine's give-command asks the host instead of sending the VM's scripts
# their event (WorkshopSync's GiveCommandHook), and the host's game gives its Codsworth its bed with the workshop's own script.
# Asserted: the VM's game heard Codsworth named as Sanctuary's settler and offers him Command (wsmode offerHook, rosterHere,
# offeredIds), the VM's game takes the settlement as one the host's game runs (the host's bed stands there as its copy), the request
# was asked and nothing assigned in the VM's game (its copy's owner unchanged, one request counted, none refused here), the VM's
# command mode ended, the host served it and answered applied, neither game counted an off-thread call across the keys and the
# answer (net status offThread=: workshop mode's Accept reaches the give-command on the UI's thread, and the hook hands the
# command to the main thread), the host's bed is owned by Codsworth, the VM's game did not take
# Codsworth over (NpcSync's settlerTakeovers unchanged), no crash, and the VM's grids and audit are clean after an in-session save
# and loaded alone (no stray bed). If the real key never puts Codsworth in the VM's command mode that is a FAIL, and the step
# goes on with the scripts' own call (Actor.SetDoingFavor by console, said so) so the request itself is still tried.
# Console placement, SetOwnedByPlayer and AddActorToWorkshopPUBLIC (the host's game), said so; a throwaway save on the VM. The host's
# game refuses its player's current companion: Codsworth must not be the host's (the set-up says so if he is). A VM save that holds
# Codsworth as its own settler already (an older run's) is a WARN: the step then proves the request, not the host's word.
SWS_ASSIGN_SAVE=${SWS_ASSIGN_SAVE:-f4mp_sharedws_assign}
_sws_assign_asked() { grep -qE "WorkshopSync: our player's job for the settler $2 \(the session's [0-9A-F]{8}\) at the host's $3 \(ours $4\), workbench [0-9A-F]{8}, asked of the host as request [0-9]+ \(nothing assigned here\)" "$(win_read sws "$1")"; }
_sws_assign_answered() { grep -qE "WorkshopSync: request [0-9]+ \(assign\) for the settler $2 at workbench [0-9A-F]{8}: $3" "$(win_read sws "$1")"; }
_sws_commanded() { [ "$(_sws_counter "$1" assignCommanded)" = "$2" ]; }
_sws_owner_of() { dc "$1" "wsinfo $2" | grep -oE ' owner=[0-9A-F]+' | head -1 | cut -d= -f2; }   # a workshop object's owner (a bed's settler: the actor, or its base), or ""
_sws_owned_by() { local o; o=$(_sws_owner_of "$1" "$2"); [ -n "$o" ] && { [ "$o" = "$3" ] || [ "$o" = "$4" ]; }; }
_sws_takeovers() { dc "$1" 'net status' | grep -oE 'settlerTakeovers=[0-9]+' | head -1 | cut -d= -f2; }
_sws_offered() { [[ ",$(dc "$1" wsmode | grep -oE 'offeredIds=[^ ]*' | cut -d= -f2)," == *",$2,"* ]]; }   # that game's workshop mode offers the actor Command (the host's word)
_sws_none_offered() { [ "$(_sws_counter "$1" offered)" = 0 ]; }
_sws_settler_of() { [ "$(field "$(dc "$1" "scriptvars $2 WorkshopNPCScript workshopID")" workshopID)" = "$(field "$(dc "$1" "scriptvars $SANCT_BENCH WorkshopScript workshopID")" workshopID)" ]; }
# Codsworth is let go whichever way the body returns (he is held in its set-up: _sws_codsworth_hold)
step_sharedws_assign() { _sws_assign_body; _sws_codsworth_free; }
_sws_assign_body() {
	section "shared workshop: a friend gives the host's settler a bed (the real Command and Assign keys; console placement, SetOwnedByPlayer, AddActorToWorkshopPUBLIC on the host and a throwaway save)"
	local r m ri who out hb bc cbase a0 n0 p0 f0 s0 d0 t0 o0 vo0 o1 ot0v ot0h how vmown=false label="sharedws-assign"
	r=$(dc host rules)
	if [ "$(field "$r" sharedWorkshop)" != true ]; then fail "$label: set-up: the host's bSharedWorkshop is on" "rules say sharedWorkshop='$(field "$r" sharedWorkshop)'"; return; fi
	if [ "$(field "$r" sharedWorkshopEdit)" != true ]; then fail "$label: set-up: the host's bSharedWorkshopEdit is on (a friend's job for a settler rides on it)" "rules say sharedWorkshopEdit='$(field "$r" sharedWorkshopEdit)'"; return; fi
	m=$(vm wsmode)
	if [ "$(field "$m" hooks)" != true ]; then fail "$label: set-up: the VM's pick hooks are installed" "$(cut -c1-160 <<<"$m")"; return; fi
	if [ "$(field "$m" edit)" != true ]; then fail "$label: set-up: the VM has the host's bSharedWorkshopEdit from the welcome" "$(cut -c1-160 <<<"$m")"; return; fi
	if [ "$(field "$m" assignHook)" != true ] || [ "$(field "$(host wsmode)" assignHook)" != true ]; then fail "$label: set-up: both games have the give-command hooked (wsmode assignHook)" "VM: $(grep -oE 'settlers: .*' <<<"$m" | cut -c1-160) | host: $(host wsmode | grep -oE 'settlers: .*' | cut -c1-160)"; return; fi
	if [ "$(field "$m" offerHook)" = true ]; then pass "$label: the VM's game has Actor's HasKeyword hooked (Command offered on the host's settlers)" "offerHook=true"
	else fail "$label: the VM's game has Actor's HasKeyword hooked (Command offered on the host's settlers)" "$(grep -oE 'offerHook=[^ ]*' <<<"$m"): see its log for the WorkshopSync HasKeyword line"; return; fi
	enter_both SanctuaryExt 14
	host "console $SANCT_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 2
	if [ "$(field "$(host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then fail "$label: set-up: Sanctuary is the host's" "$(host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer")"; return; fi
	vm "wsmode host" >/dev/null
	_sws_keys vm || return
	# Codsworth: there, alive, not the host's companion; then Sanctuary's settler in both games (console set-up)
	ri=$(host "refinfo $CODSWORTH"); cbase=$(field "$ri" base)
	if [ -z "$cbase" ] || [ "$(field "$ri" dead)" != false ] || [ "$(field "$ri" disabled)" = true ]; then fail "$label: set-up: Codsworth stands in the host's Sanctuary" "$(cut -c1-200 <<<"$ri")"; return; fi
	if [ "$(field "$ri" companion)" != false ]; then fail "$label: set-up: Codsworth is not the host's current companion (the host's game gives a companion no job: dismiss him first)" "$(grep -oE 'companion=[a-z]+' <<<"$ri")"; return; fi
	if _sws_settler_of vm $CODSWORTH; then vmown=true; warn "$label: set-up: the VM's save holds Codsworth as Sanctuary's settler already (an older run's): the step proves the request, not that the host's word alone offers Command" "VM: $(vm "scriptvars $CODSWORTH WorkshopNPCScript workshopID" | cut -c1-80)"; fi
	host "console cqf WorkshopParent \"WorkshopParentScript.AddActorToWorkshopPUBLIC\" $CODSWORTH $SANCT_BENCH" >/dev/null
	if wait_for 10 _sws_settler_of host $CODSWORTH; then pass "$label: set-up: Codsworth is Sanctuary's settler in the host's game (console: AddActorToWorkshopPUBLIC)" "workshopID=$(field "$(host "scriptvars $CODSWORTH WorkshopNPCScript workshopID")" workshopID)"
	else fail "$label: set-up: Codsworth is Sanctuary's settler in the host's game (console: AddActorToWorkshopPUBLIC)" "host: $(host "scriptvars $CODSWORTH WorkshopNPCScript workshopID" | cut -c1-80)"; return; fi
	_sws_codsworth_hold "$label"
	# the host walks to him, opens its workshop once (its workshop data made) and places the bed there; the VM gets its copy
	walk host "$label: set-up: the host walks to Codsworth" 0.8 250 $CODSWORTH || { pads_off; return; }
	pads_off
	if [ "$(field "$(host workshop)" inArea)" != true ]; then fail "$label: set-up: Codsworth stands inside Sanctuary's build area" "$(host workshop | cut -c1-120)"; return; fi
	if _ws_enter host; then sleep 3; _ws_exit host; pass "$label: set-up: the host opened Sanctuary's workshop once (its workshop data made)" "ok"; else fail "$label: set-up: the host opened Sanctuary's workshop once" "$(host workshop)"; return; fi
	out=$(host "wsreport place $SWS_BED $SANCT_BENCH"); hb=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
	if [ -z "$hb" ]; then fail "$label: set-up: the host's bed is placed (console placement)" "$out"; return; fi
	_sws_placed host "$hb"; sleep 2; host "wsfinish $hb $SANCT_BENCH" >/dev/null; sleep 3   # the bed's placed event: WorkshopParent makes it one of Sanctuary's objects
	if wait_for 15 _sws_has_copy vm "$hb"; then bc=$(_sws_copy_of vm "$hb"); pass "$label: set-up: the host's bed, and the VM's copy of it" "bed $hb, copy $bc"
	else fail "$label: set-up: the host's bed, and the VM's copy of it" "bed $hb: no copy on the VM: $(vm 'wspower copies' | cut -c1-200)"; _sws_cleanup; return; fi
	crash_snapshot
	win_mark sws host; win_mark sws vm
	a0=$(_sws_counter vm assignsAsked); n0=$(_sws_counter vm assignsNotAsked); p0=$(_sws_counter vm assignsApplied); f0=$(_sws_counter vm assignsRefused)
	s0=$(_sws_counter host assignsServed); d0=$(_sws_counter host assignsTurnedDown); t0=$(_sws_takeovers vm)
	o0=$(_sws_owner_of host "$hb"); vo0=$(_sws_owner_of vm "$bc")
	if _sws_owned_by host "$hb" $CODSWORTH "$cbase"; then fail "$label: set-up: the host's bed is nobody's before the request" "owner=$o0"; _sws_cleanup; return; fi
	# the VM walks to Codsworth and enters workshop mode there
	walk vm "$label: set-up: the VM walks to Codsworth" 0.8 250 $CODSWORTH || { pads_off; _sws_cleanup; return; }
	pads_off
	if ! _ws_enter vm; then fail "$label: the VM enters workshop mode at the host's settlement" "$(vm workshop) | $(vm menus | grep -oE 'open=[^ ]*')"; _sws_cleanup; return; fi
	if ! _sws_put_away vm "$label"; then wait_for 8 _no_msgbox vm; _ws_exit vm; _sws_cleanup; return; fi
	m=$(vm wsmode)
	expect_eq "$label: the VM's workshop mode is at Sanctuary's workbench" "$(field "$m" assignOpen)" "$SANCT_BENCH"
	# the off-thread count in both games before the keys (MainThread::OffThread: every call counted, where the log names only the
	# first of a run, so a line grep misses this one after any other): the command and the job run where the requests live
	ot0v=$(_sws_offthread vm); ot0h=$(_sws_offthread host)
	expect_eq "$label: the VM's game takes Sanctuary as a settlement the host's game runs (the host's bed stands there as its copy)" "$(field "$m" assignHostRuns)" true
	# the host's word: its game named Codsworth as Sanctuary's settler, and the VM's workshop mode offers him Command
	if wait_for 8 _sws_offered vm $CODSWORTH; then m=$(vm wsmode); pass "$label: the host's game named Codsworth as Sanctuary's settler and the VM's workshop mode offers him Command" "$(grep -oE 'rosterBenches=.*' <<<"$m" | cut -c1-200) | host: $(host wsmode | grep -oE 'rosterSent=[^ ]* rosterReports=[^ ]*')"
	else m=$(vm wsmode); fail "$label: the host's game named Codsworth as Sanctuary's settler and the VM's workshop mode offers him Command" "VM: $(grep -oE 'rosterBenches=.*' <<<"$m" | cut -c1-200) | host: $(host wsmode | grep -oE 'rosterSent=[^ ]* rosterReports=[^ ]*')"; fi
	# Command: the real key (Accept) with Codsworth highlighted; the VM's player then has him in command mode
	how="the real key"
	if _sws_aim_press vm $CODSWORTH "$WS_KEY_SELECT" select "$label (command)" && wait_for 5 _sws_commanded vm $CODSWORTH; then
		pass "$label: the VM commands Codsworth with the real key (Accept with him highlighted in workshop mode)" "pressed with $SWS_LAST_PRESS; assignCommanded=$(_sws_counter vm assignCommanded)"
	else
		fail "$label: the VM commands Codsworth with the real key (Accept with him highlighted in workshop mode)" "assignCommanded=$(_sws_counter vm assignCommanded) (want $CODSWORTH); last press: ${SWS_LAST_PRESS:-none}; $(vm menus | grep -oE 'open=[^ ]*' | cut -c1-120); $(_sws_fields "$(vm workshop)")"
		wait_for 8 _no_msgbox vm
		if ! _in_workshop vm true || ! _sws_put_away vm "$label (after the command key)"; then wait_for 8 _no_msgbox vm; _ws_exit vm; _sws_cleanup; return; fi
		# the stand-in, said so: the scripts' own way into command mode (workshop), by console
		how="the console stand-in (Actor.SetDoingFavor)"
		vm "console $CODSWORTH.cf \"Actor.SetDoingFavor\" 1 1" >/dev/null
		if wait_for 8 _sws_commanded vm $CODSWORTH; then warn "$label: Codsworth is in the VM's command mode by console (Actor.SetDoingFavor: the real Command key did not do it, see the FAIL above)" "assignCommanded=$(_sws_counter vm assignCommanded)"
		else fail "$label: set-up: Codsworth is in the VM's command mode (the console stand-in after the real key)" "assignCommanded=$(_sws_counter vm assignCommanded): nothing to give a job"; wait_for 8 _no_msgbox vm; _ws_exit vm; _sws_cleanup; return; fi
	fi
	expect_eq "$label: commanding Codsworth did not make the VM's game take him over (the host's game runs him)" "$(_sws_takeovers vm)" "${t0:-0}"
	# Assign: the real key (Accept) with the VM's copy of the host's bed highlighted
	if _sws_aim_press vm "$bc" "$WS_KEY_ASSIGN" select "$label (assign)"; then
		if wait_for 10 _sws_assign_asked vm $CODSWORTH "$hb" "$bc"; then pass "$label: the VM's job for Codsworth at its copy of the host's bed is asked of the host (nothing assigned in the VM's game)" "commanded with $how; $(grep -oE "WorkshopSync: our player's job for the settler [^;]*" "$A_OUT/windelta-sws-vm.txt" | head -1 | cut -c1-240)"
		else fail "$label: the VM's job for Codsworth at its copy of the host's bed is asked of the host (nothing assigned in the VM's game)" "no 'asked of the host' line on the VM (commanded with $how; pressed with $SWS_LAST_PRESS): $(grep -E "WorkshopSync: (our player's job|request)" "$A_OUT/windelta-sws-vm.txt" | tail -3 | tr '\n' '|' | cut -c1-400); $(vm wsmode | grep -oE 'settlers: .*' | cut -c1-200)"; fi
	fi
	sleep 2; wait_for 8 _no_msgbox vm
	expect_eq "$label: the VM counts one job asked" "$(( $(_sws_counter vm assignsAsked) - ${a0:-0} ))" 1
	expect_eq "$label: ... and none kept from its scripts without asking" "$(( $(_sws_counter vm assignsNotAsked) - ${n0:-0} ))" 0
	if wait_for 5 _sws_commanded vm 00000000; then pass "$label: the VM's command mode ended with the request (as the scripts' own first step ends it)" "assignCommanded=00000000"; else fail "$label: the VM's command mode ended with the request (as the scripts' own first step ends it)" "assignCommanded=$(_sws_counter vm assignCommanded)"; fi
	# served by the host's game with the workshop's own script; answered applied
	if wait_for 20 _sws_assign_answered vm $CODSWORTH applied; then pass "$label: the host's game gave Codsworth the bed and answered applied" "$(grep -oE 'WorkshopSync: request [0-9]+ \(assign\)[^;]*' "$A_OUT/windelta-sws-vm.txt" | head -1)"
	else fail "$label: the host's game gave Codsworth the bed and answered applied" "VM: $(grep -E 'WorkshopSync: request' "$A_OUT/windelta-sws-vm.txt" | tail -2 | tr '\n' '|' | cut -c1-300); host: $(grep -E "WorkshopSync: player [0-9]+'s request" "$(win_read sws host)" | tail -2 | tr '\n' '|' | cut -c1-400)"; fi
	if grep -qE "WorkshopSync: player [0-9]+'s request [0-9]+ \(assign\) at workbench $SANCT_BENCH, the settler $CODSWORTH to $hb: applied" "$(win_read sws host)"; then pass "$label: the host served the request for its own Codsworth and its own bed" "$(grep -oE "WorkshopSync: player [0-9]+'s request [0-9]+ \(assign\)[^;]*" "$A_OUT/windelta-sws-host.txt" | head -1 | cut -c1-200)"
	else fail "$label: the host served the request for its own Codsworth and its own bed" "$(grep -E "WorkshopSync: player [0-9]+'s request" "$A_OUT/windelta-sws-host.txt" | tail -2 | tr '\n' '|' | cut -c1-400)"; fi
	expect_eq "$label: the VM counts one job applied, none refused" "$(( $(_sws_counter vm assignsApplied) - ${p0:-0} ))/$(( $(_sws_counter vm assignsRefused) - ${f0:-0} ))" 1/0
	expect_eq "$label: the host counts one job served, none turned down" "$(( $(_sws_counter host assignsServed) - ${s0:-0} ))/$(( $(_sws_counter host assignsTurnedDown) - ${d0:-0} ))" 1/0
	_sws_same_thread vm "$ot0v" "$label: the VM's command and job were decided on its main thread (no off-thread call counted across them: the give-command's hook hands workshop mode's Accept over)"
	_sws_same_thread host "$ot0h" "$label: the host served the job on its main thread (no off-thread call counted across it)"
	# the assignment is the host's: its bed owned by Codsworth (the scripts run a moment after the answer); the VM's copy as it was
	if wait_for 20 _sws_owned_by host "$hb" $CODSWORTH "$cbase"; then pass "$label: the host's bed is Codsworth's (the workshop's own script assigned him)" "owner ${o0:-none} -> $(_sws_owner_of host "$hb") (Codsworth $CODSWORTH, base $cbase)"
	else fail "$label: the host's bed is Codsworth's (the workshop's own script assigned him)" "owner ${o0:-none} -> '$(_sws_owner_of host "$hb")' (want Codsworth $CODSWORTH or his base $cbase): $(host "wsinfo $hb" | cut -c1-200)"; fi
	o1=$(_sws_owner_of vm "$bc")
	if [ "$(field "$(vm "wsinfo $bc")" deleted)" = false ] && [ "$o1" = "$vo0" ]; then pass "$label: the VM's copy of the bed has the owner it had (nothing was assigned in the VM's game)" "owner ${vo0:-none} -> ${o1:-none}"
	else fail "$label: the VM's copy of the bed has the owner it had (nothing was assigned in the VM's game)" "owner ${vo0:-none} -> ${o1:-none}: $(vm "wsinfo $bc" | cut -c1-200)"; fi
	expect_eq "$label: the VM's game never took Codsworth over (NpcSync's settlerTakeovers)" "$(_sws_takeovers vm)" "${t0:-0}"
	if [ "$vmown" = false ]; then
		if _sws_settler_of vm $CODSWORTH; then fail "$label: Codsworth is still no settler of the VM's own game (nothing of the host's word reached it)" "VM: $(vm "scriptvars $CODSWORTH WorkshopNPCScript workshopID" | cut -c1-80)"
		else pass "$label: Codsworth is still no settler of the VM's own game (nothing of the host's word reached it)" "workshopID=$(field "$(vm "scriptvars $CODSWORTH WorkshopNPCScript workshopID")" workshopID)"; fi
		# offeredYes counts only the questions the engine itself answered no. Codsworth carries the command keyword in the VM's
		# own game too (2026-11-12: commanded with offeredYes=0), so the engine said yes and the hook had nothing to change:
		# then this run says so instead of failing (the hook's yes needs a settler the VM's game would not offer Command on).
		if [ "$(_sws_counter vm offeredYes)" -ge 1 ]; then pass "$label: the VM's game answered the engine's Command questions yes for him (its own game would have said no)" "$(_sws_counter vm offeredYes) answer(s)"
		else warn "$label: the VM's own game offered Command on Codsworth without the hook (its engine answered yes itself: offeredYes=0, commanded all the same): this settler does not prove the hook's yes" "offeredYes=0 assignCommanded seen above"; fi
	fi
	wait_for 8 _no_msgbox vm; _ws_exit vm; sleep 2
	if wait_for 5 _sws_none_offered vm; then pass "$label: workshop mode closed, the VM's game offers nobody Command" "offered=0"; else fail "$label: workshop mode closed, the VM's game offers nobody Command" "$(vm wsmode | grep -oE 'offered=[^ ]* offeredIds=[^ ]*')"; fi
	crash_verdict "$label: a job asked of the host crashes neither game"
	_sws_grid_clean vm "$label: the VM's grids name no copy and no runtime reference after the request"
	_sws_audit_clean vm "$label: the VM's audit finds nothing tied in after the request"
	# an in-session save on the VM, then loaded alone: no stray bed, grids clean
	vm "console save $SWS_ASSIGN_SAVE" >/dev/null; sleep 8; wait_ingame vm 30
	_sws_grid_clean vm "$label: after an in-session save, the VM's grids still name no copy and no runtime reference"
	vm "net leave" >/dev/null; sleep 4
	vm "console load $SWS_ASSIGN_SAVE" >/dev/null; sleep 20; wait_ingame vm 60
	_sws_grid_clean vm "$label: loaded alone, the VM's grids name no copy and no runtime reference"
	_sws_audit_clean vm "$label: loaded alone, the VM's audit finds no copy tied in"
	# the save keeps the VM's copy of the host's bed, as it keeps every copy (as sharedws-move's bag: found again by FindExisting
	# when a session sends the building; 2026-11-12 the old "not there" check failed on that): loaded alone, the copy is gone or
	# still out of workshop mode's reach (the block keyword is saved with it) and nobody's (no job of Codsworth's in the VM's save)
	if _sws_gone vm "$bc"; then pass "$label: loaded alone, the VM's copy of the host's bed is gone, or kept out of workshop mode's reach and nobody's" "gone: $(vm "refinfo $bc" | cut -c1-120)"
	elif o1=$(_sws_owner_of vm "$bc"); [ "$o1" = "$vo0" ] && [ "$(field "$(vm "wsinfo $bc")" blocked)" = true ]; then pass "$label: loaded alone, the VM's copy of the host's bed is gone, or kept out of workshop mode's reach and nobody's" "kept, blocked, owner ${o1:-none}"
	else fail "$label: loaded alone, the VM's copy of the host's bed is gone, or kept out of workshop mode's reach and nobody's" "owner ${vo0:-none} -> ${o1:-none}: $(vm "wsinfo $bc" | cut -c1-200)"; fi
	_sws_rejoin "$label"
	enter_both SanctuaryExt 14
	_sws_cleanup
}

# ---- the shared workshop's supply line: a friend sends the host's settler on a supply line (opt-in: smoke.sh --only sharedws-supply) ---
# Sanctuary and Abernathy Farm are the host's (SetOwnedByPlayer by console) and the host's player has Local Leader (added by console
# when the host's game says rank 0: player.addperk $SWS_LEADER_PERK). Both players walk to Sanctuary's workbench (real movement).
# A settler of the host's game there: a generic settler (SWS_SETTLER) made by console placement in front of the host's player,
# made Sanctuary's by the scripts' own call (AddActorToWorkshopPUBLIC by console) and held where it stands (SetRestrained 1), all
# said so. Not Codsworth: the engine offers no Supply Line on a companion (2026-11-12: the host's own game, with Local Leader,
# opened no caravan menu on its settler Codsworth, and did on a generic settler). The host's game shares the settler under its
# spawn id and tells the session its settlements and its Local Leader rank (common/Protocol.h HostSettlements); the VM's game
# places its copy, names it the host's settler there (the roster) and, in workshop mode at Sanctuary, offers Supply Line on it
# from the host's word. The VM highlights the copy and presses the real Supply Line key (workshop mode's RShoulder event,
# SWS_SUPPLY_EVENT: Q on a keyboard; YButton, Space, is the wire's), the caravan menu opens and lists the host's settlements (its
# filter answered from the host's word, and leaving out Sanctuary, the settler's own), and the VM picks an entry with the real keys
# (Accept, Down, Accept: Abernathy Farm is wanted). The pick is asked of the host
# (WorkshopSync's TransferEventHook: the VM's game sends its scripts nothing), the host's game judges it and sends its own
# engine's event, as its own caravan menu would.
# Asserted: the VM heard the host's settlements and its rank (wsmode hostSettlements, hostLeader), its workshop mode answered the
# perk and the list from them (perkAnswers, listedAnswers), the caravan menu opened, the pick went to another settlement the host
# owns, never Sanctuary (the settler's own: its location SWS_SANCT_LOCATION), the pick was decided on the VM's main thread (its
# off-thread count, net status offThread=, unchanged across the keys) and the caravan menu closed by itself after it (nothing pressed or sent to
# close it), one supply line was asked and none kept from the VM's scripts without asking, and the host's game either served
# it (answered applied, its Sanctuary has a new supply line: storage bench ourLine=, read in the host's workshop mode) or, when
# Sanctuary had lines before (an older run's, or the bench step's to Abernathy Farm), refused a second line on a linked route
# (answered "a supply line links them already", no new line), or refused for want of Local Leader (no new line); never refused
# the destination ("not another settlement the host owns": the list offered one). The copy is no settler of the VM's own game, no crash, and the
# VM's grids and audit are clean after an in-session save and loaded alone. The host's settler is removed at the end (console).
# Throwaway save on the VM.
SWS_SUPPLY_SAVE=${SWS_SUPPLY_SAVE:-f4mp_sharedws_supply}
SWS_SUPPLY_EVENT=${SWS_SUPPLY_EVENT:-RShoulder}   # the workshop menu's event for Supply Line on a highlighted settler (Q; the host's own game's caravan menu opened on it, 2026-11-12)
SWS_LEADER_PERK=${SWS_LEADER_PERK:-0004D88D}    # LocalLeader01
SWS_SANCT_LOCATION=${SWS_SANCT_LOCATION:-0001F228}  # SanctuaryHillsLocation: the settler's own settlement, never a destination
SWS_SUPPLY_SETTLER=""                            # the host's settler the step made (removed at its end, however it ends)
_sws_lines() { dc host "storage bench x" | grep -oE 'ourLine=[0-9A-F]+' | cut -d= -f2 | sort -u | tr '\n' ' '; }   # the host's supply lines from the workbench its workshop mode has open
_sws_supply_asked() { grep -qE "WorkshopSync: our player's supply line for the settler $2 \(the session's [0-9A-F]{8}\) at workbench $3 to location [0-9A-F]{8} asked of the host as request [0-9]+ \(nothing sent here\)" "$(win_read sws "$1")"; }
_sws_supply_answered() { grep -qE "WorkshopSync: request [0-9]+ \(supply line\) for the settler $2 at workbench [0-9A-F]{8}: ($3)" "$(win_read sws "$1")"; }
_sws_caravan_closed() { ! _menu_open "$1" Workshop_CaravanMenu; }
_sws_heard_host() { [ "$(_sws_counter vm hostSettlements)" -ge 2 ] 2>/dev/null && [ "$(_sws_counter vm hostLeader)" -ge 1 ] 2>/dev/null; }
step_sharedws_supply() {
	_sws_supply_body
	if [ -n "$SWS_SUPPLY_SETTLER" ]; then host "console $SWS_SUPPLY_SETTLER.setrestrained 0" >/dev/null; host "remove $SWS_SUPPLY_SETTLER" >/dev/null; SWS_SUPPLY_SETTLER=""; fi
}
_sws_supply_body() {
	section "shared workshop: a friend sends the host's settler on a supply line (the real Supply Line and Accept keys; console SetOwnedByPlayer, Local Leader, a settler made, AddActorToWorkshopPUBLIC and SetRestrained on the host, a throwaway save)"
	local r m out hs spawn c ids lines0 lines1 loc a0 n0 p0 f0 s0 d0 key kr how ot0v new="" label="sharedws-supply"
	r=$(dc host rules)
	if [ "$(field "$r" sharedWorkshop)" != true ] || [ "$(field "$r" sharedWorkshopEdit)" != true ]; then fail "$label: set-up: the host's bSharedWorkshop and bSharedWorkshopEdit are on" "rules say sharedWorkshop='$(field "$r" sharedWorkshop)' sharedWorkshopEdit='$(field "$r" sharedWorkshopEdit)'"; return; fi
	m=$(vm wsmode)
	if [ "$(field "$m" transferHooks)" = true ] && [ "$(field "$m" offerHook)" = true ]; then pass "$label: the VM's game has the caravan menu and HasKeyword hooked" "transferHooks=true offerHook=true"
	else fail "$label: the VM's game has the caravan menu and HasKeyword hooked" "$(grep -oE '(transferHooks|offerHook)=[^ ]*' <<<"$m" | tr '\n' ' '): see its log for the WorkshopSync caravan menu line"; return; fi
	enter_both SanctuaryExt 14
	host "console $SANCT_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; host "console $LINK_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 2
	if [ "$(field "$(host "scriptvars $LINK_BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then fail "$label: set-up: Sanctuary and Abernathy Farm are the host's" "$(host "scriptvars $LINK_BENCH WorkshopScript OwnedByPlayer")"; return; fi
	if ! wait_for 12 _sws_heard_host; then
		host "console player.addperk $SWS_LEADER_PERK" >/dev/null
		if wait_for 12 _sws_heard_host; then warn "$label: set-up: the host's player had no Local Leader: added by console (player.addperk $SWS_LEADER_PERK)" "hostLeader=$(_sws_counter vm hostLeader)"
		else fail "$label: the VM's game heard the host's settlements and its Local Leader rank" "VM: hostSettlements=$(_sws_counter vm hostSettlements) hostLeader=$(_sws_counter vm hostLeader) | host: $(host wsmode | grep -oE '(settlementsTold|settlementsReports)=[^ ]*' | tr '\n' ' ')"; return; fi
	fi
	pass "$label: the VM's game heard the host's settlements and its Local Leader rank" "hostSettlements=$(_sws_counter vm hostSettlements) hostLeader=$(_sws_counter vm hostLeader)"
	vm "wsmode host" >/dev/null
	_sws_keys vm || return
	key=$(_sws_key_of "$SWS_SUPPLY_EVENT" "$SWS_KEYMAP"); [ -n "$key" ] && key=0x$key
	if [ -z "$key" ]; then fail "$label: set-up: workshop mode's Supply Line key (event $SWS_SUPPLY_EVENT) is in the VM's control map" "$(grep -oE '[A-Za-z]+=[0-9A-F]+' <<<"$SWS_KEYMAP" | tr '\n' ' ' | cut -c1-300)"; return; fi
	# the host walks to the workbench and opens its workshop once (its workshop data made; Sanctuary's lines before, read there)
	walk host "$label: set-up: the host walks to Sanctuary's workbench" 0.8 300 $SANCT_BENCH || { pads_off; return; }
	pads_off
	if _ws_enter host; then sleep 2; lines0=$(_sws_lines); _ws_exit host; else fail "$label: set-up: the host opened Sanctuary's workshop" "$(host workshop | cut -c1-120)"; return; fi
	# the settler: made in front of the host's player (console placement), Sanctuary's by the scripts' own call, held there
	win_mark sws host; win_mark sws vm
	out=$(host "wsreport place $SWS_SETTLER $SANCT_BENCH quiet"); hs=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
	case "$hs" in FF*) SWS_SUPPLY_SETTLER=$hs;; *) fail "$label: set-up: the host's game made a settler (console placement)" "$out"; return;; esac
	host "console cqf WorkshopParent \"WorkshopParentScript.AddActorToWorkshopPUBLIC\" $hs $SANCT_BENCH" >/dev/null
	if ! wait_for 15 _sws_settler_of host "$hs"; then fail "$label: set-up: the new NPC is Sanctuary's settler in the host's game (console: AddActorToWorkshopPUBLIC)" "$(host "scriptvars $hs WorkshopNPCScript workshopID" | cut -c1-120)"; return; fi
	host "console $hs.setrestrained 1" >/dev/null
	warn "$label: set-up: a settler made in the host's game by console in front of its player, Sanctuary's by AddActorToWorkshopPUBLIC and held there (SetRestrained 1)" "$hs (base $SWS_SETTLER) workshopID=$(field "$(host "scriptvars $hs WorkshopNPCScript workshopID")" workshopID)"
	if wait_for 30 _sws_recruit_shared host "$hs"; then spawn=$(sed -nE "s/.*Spawns: sharing $hs \(base [0-9A-F]{8}\) as ([0-9A-F]{8}).*/\1/p" "$A_OUT/windelta-sws-host.txt" | tail -1)
	else fail "$label: set-up: the host's game shares its settler as a settler of Sanctuary's workbench" "$(grep -E "Spawns: .*$hs" "$A_OUT/windelta-sws-host.txt" | tail -2 | tr '\n' '|' | cut -c1-300)"; return; fi
	walk vm "$label: set-up: the VM walks to Sanctuary's workbench" 0.8 300 $SANCT_BENCH || { pads_off; return; }
	pads_off
	if wait_for 30 _sws_recruit_placed vm "$spawn"; then c=$(_sws_recruit_copy vm "$spawn"); pass "$label: set-up: the VM's game placed its copy of the host's settler" "host $hs (spawn $spawn), copy $c"
	else fail "$label: set-up: the VM's game placed its copy of the host's settler" "spawn $spawn: $(grep -E "Spawns: .*$spawn" "$(win_read sws vm)" | tail -2 | tr '\n' '|' | cut -c1-300)"; return; fi
	ids="($c|$spawn|$hs)"   # the settler in the VM's log lines: its copy, or the session's id
	crash_snapshot
	win_mark sws host; win_mark sws vm
	a0=$(_sws_counter vm transfersAsked); n0=$(_sws_counter vm transfersNotAsked); p0=$(_sws_counter vm transfersApplied); f0=$(_sws_counter vm transfersRefused)
	s0=$(_sws_counter host transfersServed); d0=$(_sws_counter host transfersTurnedDown)
	if ! _ws_enter vm; then fail "$label: the VM enters workshop mode at the host's settlement" "$(vm workshop) | $(vm menus | grep -oE 'open=[^ ]*')"; return; fi
	if ! _sws_put_away vm "$label"; then wait_for 8 _no_msgbox vm; _ws_exit vm; return; fi
	m=$(vm wsmode)
	expect_eq "$label: the VM's game takes Sanctuary as a settlement the host's game runs" "$(field "$m" hostRunsOpen)" "$SANCT_BENCH"
	if ! wait_for 8 _sws_offered vm $c; then fail "$label: the host's game named its settler as Sanctuary's (the VM's workshop mode offers its copy the settler actions)" "$(grep -oE 'rosterBenches=[^ ]*|offeredIds=[^ ]*' <<<"$m" | tr '\n' ' ')"; _ws_exit vm; return; fi
	ot0v=$(_sws_offthread vm)   # the off-thread count before the keys (checked after the pick)
	# Supply Line: the real key with the copy highlighted; the caravan menu opens with the host's settlements
	if _sws_aim_press vm $c "$key" select "$label (supply line)" && wait_for 6 _menu_open vm Workshop_CaravanMenu; then
		pass "$label: the VM's Supply Line key on the host's settler opens the caravan menu" "pressed with $SWS_LAST_PRESS; perkAnswers=$(_sws_counter vm perkAnswers) listedAnswers=$(_sws_counter vm listedAnswers)"
	else
		fail "$label: the VM's Supply Line key on the host's settler opens the caravan menu" "key $key ($SWS_SUPPLY_EVENT); last press: ${SWS_LAST_PRESS:-none}; perkAnswers=$(_sws_counter vm perkAnswers) hostLeader=$(_sws_counter vm hostLeader); $(vm menus | grep -oE 'open=[^ ]*' | cut -c1-160)"
		wait_for 8 _no_msgbox vm; _ws_exit vm; return
	fi
	expect_ge "$label: the VM's caravan list answered from the host's settlements" "$(_sws_counter vm listedAnswers)" 1 answer
	expect_ge "$label: the VM's workshop mode asked the host's Local Leader rank" "$(_sws_counter vm perkAnswers)" 1 answer
	# the pick: the real keys on the list's entry. The list holds the host's settlements but Sanctuary, the settler's own (the VM's
	# copy is no settler of its game, so the engine's filter can't tell the settler's own: our answer of the open workbench's
	# owned value leaves it out; before that fix, 2026-11-12, the list held Sanctuary too and the keys picked it): with only
	# Sanctuary and Abernathy Farm the host's, Abernathy Farm alone. The menu opens with nothing picked by Accept alone: Accept
	# first (picks nothing), then Down and Accept, each only while the menu is still open and nothing was asked. Checked below
	local ks k
	kr=""
	for ks in "$WS_KEY_SELECT" "28 $WS_KEY_SELECT"; do
		_menu_open vm Workshop_CaravanMenu || break
		for k in $ks; do dc vm "key $k 80" >/dev/null; sleep 1; done
		kr+="[$ks] "
		wait_for 4 _sws_supply_asked vm "$ids" $SANCT_BENCH && break
	done
	if wait_for 6 _sws_supply_asked vm "$ids" $SANCT_BENCH; then
		loc=$(grep -oE "supply line for the settler $ids [^;]* to location [0-9A-F]{8}" "$A_OUT/windelta-sws-vm.txt" | head -1 | grep -oE '[0-9A-F]{8}$')
		pass "$label: the VM's pick is asked of the host (nothing sent to the VM's scripts)" "keys $kr; location $loc; $(grep -oE "WorkshopSync: our player's supply line [^;]*" "$A_OUT/windelta-sws-vm.txt" | head -1 | cut -c1-240)"
	else
		fail "$label: the VM's pick is asked of the host (nothing sent to the VM's scripts)" "keys tried: $kr; $(vm menus | grep -oE 'open=[^ ]*' | cut -c1-120); $(grep -E 'WorkshopSync: (our player|request|a friend)' "$A_OUT/windelta-sws-vm.txt" | tail -3 | tr '\n' '|' | cut -c1-400)"
	fi
	# the list leaves out the settler's own settlement: a pick of Sanctuary is a FAIL (the list held it); any other is a settlement
	# the host owns (Abernathy Farm, or a third the host's save owns: the host's answer below says whether it took it as one)
	local own=false
	if [ -n "$loc" ] && [ "$loc" = "$SWS_SANCT_LOCATION" ]; then
		own=true
		fail "$label: the VM's caravan list leaves out the settler's own settlement (the pick goes to another the host owns)" "picked Sanctuary's location $loc: the list held the settler's own (the host refuses it)"
	elif [ -n "$loc" ] && [ "$loc" = "$LINK_LOCATION" ]; then
		pass "$label: the VM's caravan list leaves out the settler's own settlement (the pick goes to another the host owns)" "picked Abernathy Farm's location $loc, not Sanctuary's $SWS_SANCT_LOCATION"
	elif [ -n "$loc" ]; then
		pass "$label: the VM's caravan list leaves out the settler's own settlement (the pick goes to another the host owns)" "picked location $loc: neither Sanctuary's $SWS_SANCT_LOCATION nor Abernathy Farm's $LINK_LOCATION (a third settlement of the host's save; its answer below)"
	fi
	# the pick was decided on the VM's main thread (the request's book, the session's queue and the HUD are the main thread's; the
	# hook hands an off-thread pick over): no off-thread call counted across the keys (the count, not the log: it names only a
	# run's first, often an earlier one's)
	_sws_same_thread vm "$ot0v" "$label: the caravan menu's pick was decided on the VM's main thread (no off-thread call counted across the keys)"
	# the caravan menu closed by itself after the pick (its own close path; our game withheld only the event): nothing pressed yet
	if wait_for 5 _sws_caravan_closed vm; then pass "$label: the caravan menu closed by itself after the pick" "$(vm menus | grep -oE 'open=[^ ]*' | cut -c1-120)"
	else
		fail "$label: the caravan menu closed by itself after the pick" "still open 5 s after Accept: $(vm menus | grep -oE 'open=[^ ]*' | cut -c1-160)"
		dc vm "menu Workshop_CaravanMenu cancel" >/dev/null; sleep 1   # only after the FAIL, so the step can go on
	fi
	wait_for 8 _no_msgbox vm
	expect_eq "$label: the VM counts one supply line asked, none kept from its scripts without asking" "$(( $(_sws_counter vm transfersAsked) - ${a0:-0} ))/$(( $(_sws_counter vm transfersNotAsked) - ${n0:-0} ))" 1/0
	# the host's answer: applied; or a refusal for the route alone (Sanctuary had a line to it already) or for want of Local
	# Leader; never of the destination. A pick of Sanctuary (the FAIL above) the host must refuse as a destination, never serve
	local answer=""
	how=""
	if wait_for 20 _sws_supply_answered vm "$ids" 'applied|a supply line links them already|the host lacks Local Leader|not another settlement the host owns'; then
		if _sws_supply_answered vm "$ids" applied; then how=applied
		elif _sws_supply_answered vm "$ids" 'a supply line links them already'; then how=linked
		elif _sws_supply_answered vm "$ids" 'the host lacks Local Leader'; then how=leader
		else how=destination; fi
		answer=$(grep -oE 'WorkshopSync: request [0-9]+ \(supply line\)[^;]*' "$A_OUT/windelta-sws-vm.txt" | head -1)
		if [ "$own" = true ]; then
			if [ "$how" = destination ]; then pass "$label: the host's game refused the settler's own settlement as a destination" "$answer"
			else fail "$label: the host's game refused the settler's own settlement as a destination" "answered $how for Sanctuary's location $loc: $answer"; fi
		elif [ "$how" = destination ]; then fail "$label: the host's game answered the supply line (applied, or refused for the route or Local Leader alone)" "refused the destination, location ${loc:-?} (the list offered one the host's game doesn't take): $answer"
		else pass "$label: the host's game answered the supply line ($how)" "$answer"; fi
	else fail "$label: the host's game answered the supply line (applied, or refused for the route or Local Leader alone)" "VM: $(grep -E 'WorkshopSync: request' "$A_OUT/windelta-sws-vm.txt" | tail -2 | tr '\n' '|' | cut -c1-300); host: $(grep -E "WorkshopSync: player [0-9]+'s request" "$(win_read sws host)" | tail -2 | tr '\n' '|' | cut -c1-400)"; fi
	if [ -n "$how" ] && [ "$how" != applied ]; then
		expect_eq "$label: the VM counts none applied, one refused" "$(( $(_sws_counter vm transfersApplied) - ${p0:-0} ))/$(( $(_sws_counter vm transfersRefused) - ${f0:-0} ))" 0/1
		expect_eq "$label: the host counts none served, one turned down" "$(( $(_sws_counter host transfersServed) - ${s0:-0} ))/$(( $(_sws_counter host transfersTurnedDown) - ${d0:-0} ))" 0/1
	else
		expect_eq "$label: the VM counts one applied, none refused" "$(( $(_sws_counter vm transfersApplied) - ${p0:-0} ))/$(( $(_sws_counter vm transfersRefused) - ${f0:-0} ))" 1/0
		expect_eq "$label: the host counts one served, none turned down" "$(( $(_sws_counter host transfersServed) - ${s0:-0} ))/$(( $(_sws_counter host transfersTurnedDown) - ${d0:-0} ))" 1/0
	fi
	wait_for 8 _no_msgbox vm; _ws_exit vm; sleep 2
	# the line is the host's: its Sanctuary links one more settlement (the scripts run a moment after the event)
	sleep 5
	if _ws_enter host; then sleep 2; lines1=$(_sws_lines); _ws_exit host; fi
	for r in $lines1; do [[ " $lines0 " == *" $r "* ]] || new+="$r "; done
	if [ "$how" = linked ]; then
		if [ -n "$lines0" ] && [ -z "$new" ]; then pass "$label: the host refused a second supply line on a route Sanctuary already had, and made none" "before: $lines0; after: ${lines1:-none}; location ${loc:-?}"
		else fail "$label: the host refused a second supply line on a route Sanctuary already had, and made none" "before: ${lines0:-none}; after: ${lines1:-none}; new: ${new:-none}; location ${loc:-?}"; fi
	elif [ -n "$how" ] && [ "$how" != applied ]; then
		if [ -z "$new" ]; then pass "$label: the host made no supply line for the refused pick ($how)" "before: ${lines0:-none}; after: ${lines1:-none}; location ${loc:-?}"
		else fail "$label: the host made no supply line for the refused pick ($how)" "new: $new; location ${loc:-?}"; fi
	elif [ -n "$new" ]; then pass "$label: the host's Sanctuary has a new supply line (the host's own scripts made it)" "before: ${lines0:-none}; after: ${lines1:-none}"
	else fail "$label: the host's Sanctuary has a new supply line (the host's own scripts made it)" "before: ${lines0:-none}; after: ${lines1:-none}; location ${loc:-?}"; fi
	if _sws_settler_of vm "$c"; then fail "$label: the VM's copy of the host's settler is no settler of the VM's own game (the host's word reached no script there)" "VM: $(vm "scriptvars $c WorkshopNPCScript workshopID" | cut -c1-80)"
	else pass "$label: the VM's copy of the host's settler is no settler of the VM's own game (the host's word reached no script there)" "workshopID=$(field "$(vm "scriptvars $c WorkshopNPCScript workshopID")" workshopID)"; fi
	crash_verdict "$label: a supply line asked of the host crashes neither game"
	_sws_grid_clean vm "$label: the VM's grids name no copy and no runtime reference after the request"
	_sws_audit_clean vm "$label: the VM's audit finds nothing tied in after the request"
	vm "console save $SWS_SUPPLY_SAVE" >/dev/null; sleep 8; wait_ingame vm 30
	vm "net leave" >/dev/null; sleep 4
	vm "console load $SWS_SUPPLY_SAVE" >/dev/null; sleep 20; wait_ingame vm 60
	_sws_grid_clean vm "$label: loaded alone, the VM's grids name no copy and no runtime reference"
	_sws_audit_clean vm "$label: loaded alone, the VM's audit finds no copy tied in"
	if _sws_settler_of vm "$c"; then fail "$label: loaded alone, the VM's save holds no settler under the copy's id" "VM: $(vm "scriptvars $c WorkshopNPCScript workshopID" | cut -c1-80)"
	else pass "$label: loaded alone, the VM's save holds no settler under the copy's id" "$(vm "refinfo $c" | cut -c1-100)"; fi
	_sws_rejoin "$label"
	enter_both SanctuaryExt 14
}

# ---- the shared workshop's recruits: a settler the host's game made is seen by a friend (opt-in: smoke.sh --only sharedws-recruit) ---
# Sanctuary is the host's (SetOwnedByPlayer by console). Both players walk to its workbench (real movement). The host's game makes
# a settler there by console, the stand-in for a recruitment beacon's arrival (a reference of the settler's base made a step in
# front of the host's player, then WorkshopParentScript.AddActorToWorkshopPUBLIC: the scripts' own way a recruit becomes the
# workshop's), said so. Before the script call the new NPC is a calm runtime NPC and nothing of it is shared (the control); after
# it the host's game shares it calm as a settler of Sanctuary's workbench, with a settler's place, and the VM's game places its
# copy where the host's settler stands.
# Asserted: nothing shared before the NPC is the workshop's; the host's log line and counters (net status: settlersOurs,
# settlersShared) after; the VM hears of it as a settler of the same workbench (settlersReceived, settlersTheirs, none vetoed),
# places a living copy within reach of the host's settler, and did not take anything over (NpcSync's settlerTakeovers); in
# workshop mode at Sanctuary the VM's game takes the settlement as one the host's game runs (wsmode assignHostRuns: the host's
# game naming its settlers there is the evidence when no piece of the host's stands there) and offers Command on its copy (wsmode
# offeredIds: the host's game names the settler under its spawn id); no crash; the VM's grids and audit are clean, and after an
# in-session save loaded alone the copy is not there (no stray settler in the friend's save). Rejoined, the VM has the settler
# again from the welcome; the host's settler removed by console, the VM's copy goes.
# Console placement of the settler, SetOwnedByPlayer and AddActorToWorkshopPUBLIC (the host's game only), said so; a throwaway
# save on the VM. Not asserted: a job for the copy (sharedws-assign proves the request with a plugin's settler).
SWS_RECRUIT_SAVE=${SWS_RECRUIT_SAVE:-f4mp_sharedws_recruit}
_sws_spawnf() { field "$(dc "$1" 'net status')" "$2"; }   # a field of the spawns' dev line (settlersOurs, settlersTheirs, ...)
_sws_recruit_shared() { grep -qE "Spawns: sharing $2 \(base [0-9A-F]{8}\) as [0-9A-F]{8}, our settler of workbench $SANCT_BENCH" "$(win_read sws "$1")"; }
_sws_recruit_any() { grep -qE "Spawns: sharing $2 " "$(win_read sws "$1")"; }
_sws_recruit_heard() { grep -qE "Spawns: player [0-9]+ shares $2 \(base [0-9A-F]{8}\) .*, their settler of workbench $SANCT_BENCH" "$(win_read sws "$1")"; }
_sws_recruit_copy() { sed -nE "s/.*Spawns: placed ([0-9A-F]{8}).* as our copy of player [0-9]+'s $2\$/\1/p" "$(win_read sws "$1")" | tail -1; }
_sws_recruit_placed() { [ -n "$(_sws_recruit_copy "$1" "$2")" ]; }
_sws_recruit_near() {  # _sws_recruit_near <the VM's copy> <the host's settler> <units>: the copy stands that near where the host's settler stands
	local c s d; c=$(vm "refinfo $1"); s=$(host "refinfo $2")
	d=$(_dist_xy "$(field "$c" x) $(field "$c" y)" "$(field "$s" x) $(field "$s" y)"); is_int "$d" && [ "$d" -le "$3" ]
}
_sws_spawnf_is() { [ "$(_sws_spawnf "$1" "$2")" = "$3" ]; }
_sws_spawnd() { local now; now=$(_sws_spawnf "$1" "$2"); if is_int "$now" && is_int "$3"; then echo $((now - $3)); else echo "?"; fi; }   # how far a counter moved from <was>, "?" when unread
step_sharedws_recruit() {
	section "shared workshop: a settler the host's game made is seen by a friend (console: the settler made and AddActorToWorkshopPUBLIC on the host, SetOwnedByPlayer; a throwaway save)"
	local r m out s spawn copy ri sh0 rc0 vt0 hd0 t0 ho vt label="sharedws-recruit"
	r=$(dc host rules)
	if [ "$(field "$r" sharedWorkshop)" != true ]; then fail "$label: set-up: the host's bSharedWorkshop is on" "rules say sharedWorkshop='$(field "$r" sharedWorkshop)'"; return; fi
	enter_both SanctuaryExt 14
	host "console $SANCT_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 2
	if [ "$(field "$(host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then fail "$label: set-up: Sanctuary is the host's" "$(host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer")"; return; fi
	vm "wsmode host" >/dev/null
	walk host "$label: set-up: the host walks to Sanctuary's workbench" 0.8 300 $SANCT_BENCH || { pads_off; return; }
	walk vm "$label: set-up: the VM walks to Sanctuary's workbench" 0.8 300 $SANCT_BENCH || { pads_off; return; }
	pads_off
	if [ "$(field "$(host workshop)" inArea)" != true ]; then fail "$label: set-up: the host stands inside Sanctuary's build area" "$(host workshop | cut -c1-120)"; return; fi
	if _ws_enter host; then sleep 3; _ws_exit host; pass "$label: set-up: the host opened Sanctuary's workshop once (its workshop data made)" "ok"; else fail "$label: set-up: the host opened Sanctuary's workshop once" "$(host workshop)"; return; fi
	crash_snapshot
	win_mark sws host; win_mark sws vm
	sh0=$(_sws_spawnf host settlersShared); hd0=$(_sws_spawnf host settlersHeld); rc0=$(_sws_spawnf vm settlersReceived); vt0=$(_sws_spawnf vm settlersVetoed); t0=$(_sws_takeovers vm)
	if ! is_int "$sh0" || ! is_int "$hd0" || ! is_int "$rc0" || ! is_int "$vt0"; then fail "$label: set-up: both games' spawns lines carry the settlers' counters" "host settlersShared='$sh0' VM settlersReceived='$rc0': $(dc host 'net status' | grep -oE ' settlers: [^|]*' | cut -c1-160)"; return; fi
	# the stand-in for a beacon's arrival: a settler's reference made in the host's game (console placement), not yet the workshop's
	out=$(host "wsreport place $SWS_SETTLER $SANCT_BENCH quiet"); s=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
	case "$s" in FF*) pass "$label: set-up: the host's game made a settler's reference (console placement, a runtime reference)" "$s";; *) fail "$label: set-up: the host's game made a settler's reference (console placement)" "$out"; return;; esac
	# the control: a calm runtime NPC that is nobody's settler is not shared
	sleep 6
	if _sws_recruit_any host "$s"; then fail "$label: a calm runtime NPC that is no settlement's is not shared (the control)" "$(grep -E "Spawns: sharing $s " "$A_OUT/windelta-sws-host.txt" | head -1 | cut -c1-200)"
	else pass "$label: a calm runtime NPC that is no settlement's is not shared (the control)" "settlersShared=$(_sws_spawnf host settlersShared)"; fi
	# it becomes Sanctuary's settler the scripts' own way (console: AddActorToWorkshopPUBLIC, the host's game only)
	host "console cqf WorkshopParent \"WorkshopParentScript.AddActorToWorkshopPUBLIC\" $s $SANCT_BENCH" >/dev/null
	if wait_for 15 _sws_settler_of host "$s"; then pass "$label: set-up: the new NPC is Sanctuary's settler in the host's game (console: AddActorToWorkshopPUBLIC)" "workshopID=$(field "$(host "scriptvars $s WorkshopNPCScript workshopID")" workshopID)"
	else fail "$label: set-up: the new NPC is Sanctuary's settler in the host's game (console: AddActorToWorkshopPUBLIC)" "$(host "scriptvars $s WorkshopNPCScript workshopID" | cut -c1-120)"; host "remove $s" >/dev/null; return; fi
	# the host's game shares it, calm, as a settler of Sanctuary's workbench
	if wait_for 30 _sws_recruit_shared host "$s"; then
		spawn=$(sed -nE "s/.*Spawns: sharing $s \(base [0-9A-F]{8}\) as ([0-9A-F]{8}), our settler of workbench $SANCT_BENCH.*/\1/p" "$A_OUT/windelta-sws-host.txt" | tail -1)
		pass "$label: the host's game shares its settler as a settler of Sanctuary's workbench" "$(grep -oE "Spawns: sharing $s [^;]*" "$A_OUT/windelta-sws-host.txt" | tail -1 | cut -c1-200)"
	else
		fail "$label: the host's game shares its settler as a settler of Sanctuary's workbench" "no 'our settler of workbench' line for $s: $(grep -E "Spawns: .*$s" "$A_OUT/windelta-sws-host.txt" | tail -2 | tr '\n' '|' | cut -c1-400); $(dc host 'net status' | grep -oE ' settlers: [^|]*' | cut -c1-160)"
		host "remove $s" >/dev/null; return
	fi
	expect_ge "$label: the host counts a settler shared" "$(_sws_spawnd host settlersShared "$sh0")" 1
	expect_eq "$label: ... and none held back for want of a place" "$(_sws_spawnd host settlersHeld "$hd0")" 0
	expect_ge "$label: the host's book holds it in a settler's place (settlersOurs)" "$(_sws_spawnf host settlersOurs)" 1
	# the VM hears of it as a settler of the same workbench and places its copy
	if wait_for 30 _sws_recruit_heard vm "$spawn"; then pass "$label: the VM hears of the host's settler as a settler of Sanctuary's workbench" "$(grep -oE "Spawns: player [0-9]+ shares $spawn [^;]*" "$A_OUT/windelta-sws-vm.txt" | tail -1 | cut -c1-220)"
	else fail "$label: the VM hears of the host's settler as a settler of Sanctuary's workbench" "$(grep -E "Spawns: .*$spawn" "$(win_read sws vm)" | tail -2 | tr '\n' '|' | cut -c1-400); $(dc vm 'net status' | grep -oE ' settlers: [^|]*' | cut -c1-160)"; fi
	expect_ge "$label: the VM counts a settler heard of" "$(_sws_spawnd vm settlersReceived "$rc0")" 1
	expect_eq "$label: ... and none taken as a plain spawn (its workbench is one here)" "$(_sws_spawnd vm settlersVetoed "$vt0")" 0
	expect_ge "$label: the VM's book holds the host's settler (settlersTheirs)" "$(_sws_spawnf vm settlersTheirs)" 1
	if wait_for 30 _sws_recruit_placed vm "$spawn"; then copy=$(_sws_recruit_copy vm "$spawn"); pass "$label: the VM's game places its copy of the host's settler" "copy $copy of $spawn (the host's $s)"
	else fail "$label: the VM's game places its copy of the host's settler" "no 'placed ... as our copy' line for $spawn: $(grep -E "Spawns: .*$spawn" "$(win_read sws vm)" | tail -3 | tr '\n' '|' | cut -c1-400)"; fi
	if [ -n "$copy" ]; then
		ri=$(vm "refinfo $copy")
		if [ "$(field "$ri" dead)" = false ] && [ "$(field "$ri" disabled)" != true ]; then pass "$label: the VM's copy stands there alive" "$(cut -c1-120 <<<"$ri")"; else fail "$label: the VM's copy stands there alive" "$(cut -c1-200 <<<"$ri")"; fi
		if wait_for 20 _sws_recruit_near "$copy" "$s" 400; then pass "$label: the VM's copy stands where the host's settler stands (within 400 units)" "host ($(field "$(host "refinfo $s")" x),$(field "$(host "refinfo $s")" y)) VM ($(field "$(vm "refinfo $copy")" x),$(field "$(vm "refinfo $copy")" y))"
		else fail "$label: the VM's copy stands where the host's settler stands (within 400 units)" "host: $(host "refinfo $s" | cut -c1-120) | VM: $(vm "refinfo $copy" | cut -c1-120)"; fi
	fi
	expect_eq "$label: the VM's game took nothing over (NpcSync's settlerTakeovers)" "$(_sws_takeovers vm)" "${t0:-0}"
	# the VM in workshop mode at Sanctuary: its game takes the settlement as one the host's game runs
	if _ws_enter vm; then
		m=$(vm wsmode)
		expect_eq "$label: the VM's workshop mode is at Sanctuary's workbench" "$(field "$m" assignOpen)" "$SANCT_BENCH"
		expect_eq "$label: the VM's game takes Sanctuary as a settlement the host's game runs (the host's game names its settlers there)" "$(field "$m" assignHostRuns)" true
		if [ -n "$copy" ] && wait_for 8 _sws_offered vm "$copy"; then pass "$label: the VM's workshop mode offers Command on its copy of the host's settler (the host's game names it under its spawn id)" "$(vm wsmode | grep -oE 'rosterHere=[^ ]* rosterTaken=[^ ]* offered=[^ ]* offeredIds=[^ ]*')"
		else fail "$label: the VM's workshop mode offers Command on its copy of the host's settler (the host's game names it under its spawn id)" "copy '${copy:-none}': $(vm wsmode | grep -oE 'offerHook=.*' | cut -c1-200) | host: $(host wsmode | grep -oE 'rosterSent=[^ ]* rosterReports=[^ ]*')"; fi
		wait_for 8 _no_msgbox vm; _ws_exit vm; sleep 2
	else fail "$label: the VM enters workshop mode at the host's settlement" "$(vm workshop) | $(vm menus | grep -oE 'open=[^ ]*')"; fi
	crash_verdict "$label: a shared settler crashes neither game"
	_sws_grid_clean vm "$label: the VM's grids name no copy and no runtime reference with the host's settler there"
	_sws_audit_clean vm "$label: the VM's audit finds nothing tied in with the host's settler there"
	# an in-session save on the VM, then loaded alone: the copy is not in the friend's save
	vm "console save $SWS_RECRUIT_SAVE" >/dev/null; sleep 8; wait_ingame vm 30
	vm "net leave" >/dev/null; sleep 4
	vm "console load $SWS_RECRUIT_SAVE" >/dev/null; sleep 20; wait_ingame vm 60
	_sws_grid_clean vm "$label: loaded alone, the VM's grids name no copy and no runtime reference"
	_sws_audit_clean vm "$label: loaded alone, the VM's audit finds no copy tied in"
	if [ -n "$copy" ]; then
		if _sws_gone vm "$copy"; then pass "$label: loaded alone, the VM's copy of the host's settler is not there (no stray settler in the save)" "$(vm "refinfo $copy" | cut -c1-120)"; else fail "$label: loaded alone, the VM's copy of the host's settler is not there (no stray settler in the save)" "$(vm "refinfo $copy" | cut -c1-200)"; fi
	fi
	_sws_rejoin "$label" || return
	enter_both SanctuaryExt 14
	# rejoined: the welcome carries the host's settler; then the host's settler goes (console removal) and so does the VM's copy
	_sws_recruit_back() { ho=$(_sws_spawnf host settlersOurs); vt=$(_sws_spawnf vm settlersTheirs); is_int "$ho" && is_int "$vt" && [ "$ho" -ge 1 ] && [ "$vt" -ge "$ho" ]; }
	if wait_for 40 _sws_recruit_back; then pass "$label: rejoined, the VM has the host's settler again (from the welcome)" "host settlersOurs=$ho, VM settlersTheirs=$vt"
	else fail "$label: rejoined, the VM has the host's settler again (from the welcome)" "host settlersOurs='$ho', VM settlersTheirs='$vt': $(dc vm 'net status' | grep -oE ' settlers: [^|]*' | cut -c1-160)"; host "remove $s" >/dev/null; _sws_cleanup; return; fi
	host "remove $s" >/dev/null
	if wait_for 30 _sws_spawnf_is host settlersOurs $((ho - 1)); then pass "$label: the host's settler removed (console), its place is free" "settlersOurs $ho -> $((ho - 1))"; else fail "$label: the host's settler removed (console), its place is free" "settlersOurs was $ho: $(dc host 'net status' | grep -oE ' settlers: [^|]*' | cut -c1-160)"; fi
	if wait_for 30 _sws_spawnf_is vm settlersTheirs $((vt - 1)); then pass "$label: ... and the VM's game lets go of it" "settlersTheirs $vt -> $((vt - 1))"; else fail "$label: ... and the VM's game lets go of it" "settlersTheirs was $vt: $(dc vm 'net status' | grep -oE ' settlers: [^|]*' | cut -c1-160)"; fi
	_sws_cleanup
}

_rad_stage_once() { [[ ",$(_rad_done "$1" "$2")," == *",$3,"* ]]; }
_rad_taken() { dc "$1" "net status" | grep -o "radiants: runs=[^ ]* begun=[^ ]* joined=[^ ]* declined=[^ ]* sent=[^ ]* taken=[0-9]*" | sed "s/.*taken=//"; }
_rad_shared() { [[ $(dc "$1" "stages $2") == *"this run shared"* ]]; }
step_radiants() {
	section "radiants (story-manager runs shared by their run, both ways)"
	local entry edid id marker obj mid reward end who other h0 v0 d0 s0 st away
	enter_both SanctuaryExt 14
	IFS=';' read -ra cases <<<"$RAD_CASES"
	for entry in "${cases[@]}"; do
		read -r edid id marker obj mid reward end <<<"$entry"
		if [ "$(_rad_run host "$edid")" != "running=false" ] || [ "$(_rad_run vm "$edid")" != "running=false" ] || [ "$(_rad_done host "$id")" != none ]; then
			warn "radiants ($edid): not run: a game has run it already (it runs once: restart both games)" "host $(_rad_run host "$edid") vm $(_rad_run vm "$edid")"; continue
		fi
		h0=$(_rad_inv host); v0=$(_rad_inv vm); s0="$(_rad_spawned host) / $(_rad_spawned vm)"
		away=0; [[ " $RAD_AWAY " == *" $edid "* ]] && away=1
		if [ "$away" = 1 ]; then
			# The VM stays where it is: the host's run is started in its game with the host's fills.
			win_mark radiants vm
			dc host "console player.moveto $marker" >/dev/null; wait_ingame host 40
			if ! wait_for 30 _rad_is_running host "$edid"; then fail "radiants ($edid, VM away): the story manager hands the quest to host" "$(_rad_run host "$edid")"; continue; fi
			expect_eq "radiants ($edid, VM away): the VM's game doesn't run it before the host's run is shared" "$(_rad_run vm "$edid")" "running=false"
			dc host "console setobjectivedisplayed $edid $obj 1" >/dev/null
			if wait_for 30 _rad_log_has vm "RadiantSync: sharing player [0-9]+'s run [0-9]+ of $id '.*' \(started here with their fills\)"; then
				pass "radiants ($edid, VM away): the VM's game starts the host's run here with the host's fills" "started here: $(grep -cE "RadiantSync: sharing .* of $id .*started here with their fills" "$A_OUT/windelta-radiants-vm.txt"); $(dc vm 'net status' | grep -oE 'quest starts: [^|]*' | head -1)"
			else fail "radiants ($edid, VM away): the VM's game starts the host's run here with the host's fills" "$(_rad_tail vm)"; continue; fi
			expect_eq "radiants ($edid, VM away): the VM's quest runs" "$(_rad_run vm "$edid")" "running=true"
		else
			for who in host vm; do dc "$who" "console player.moveto $marker" >/dev/null; wait_ingame "$who" 40; done
			for who in host vm; do
				if ! wait_for 30 _rad_is_running "$who" "$edid"; then fail "radiants ($edid): the story manager hands the quest to $who" "$(_rad_run "$who" "$edid")"; continue 2; fi
			done
			pass "radiants ($edid): both games run it" "arrived at $marker"
			for who in host vm; do dc "$who" "console setobjectivedisplayed $edid $obj 1" >/dev/null; done
		fi
		if wait_for 20 _rad_shared host "$id" && wait_for 20 _rad_shared vm "$id"; then pass "radiants ($edid): the run is shared in both games" "$(_rad_here host "$id")"
		else fail "radiants ($edid): the run is shared in both games" "host '$(dc host "stages $id" | cut -c1-90)' vm '$(dc vm "stages $id" | cut -c1-90)'"; continue; fi
		expect_eq "radiants ($edid): the same settlement in both games" "$(_rad_here vm "$id")" "$(_rad_here host "$id")"
		ns host; ns vm; h0=$(_rad_inv host); v0=$(_rad_inv vm); s0="$(_rad_spawned host) / $(_rad_spawned vm)"; st=$(_rad_taken vm)
		dc host "console setstage $edid $mid" >/dev/null
		if wait_for 12 _rad_stage_once vm "$id" "$mid"; then pass "radiants ($edid): the host's stage $mid is set in the other game" "vm done $(_rad_done vm "$id")"
		else fail "radiants ($edid): the host's stage $mid is set in the other game" "host $(_rad_done host "$id") vm $(_rad_done vm "$id")"; fi
		dc host "console setstage $edid $reward" >/dev/null
		wait_for 12 _rad_stage_once vm "$id" "$reward"
		[ "$end" != "$reward" ] && { dc host "console setstage $edid $end" >/dev/null; wait_for 12 _rad_stage_once vm "$id" "$end"; }
		# The quest's shutdown stage is done as it stops, which the other game's does a few seconds after the host's.
		sleep 3; wait_for 20 _rad_same_done "$id"
		expect_eq "radiants ($edid): the same stages are done in both games" "$(_rad_done vm "$id")" "$(_rad_done host "$id")"
		d0=$(_rad_done vm "$id" | tr ',' '\n' | sort | uniq -d | tr '\n' ' ')
		expect_eq "radiants ($edid): no stage is set twice (the done list has each once)" "$d0" ""
		expect_between "radiants ($edid): the other game took each stage of the host's once" "$(( $(_rad_taken vm) - st ))" 1 "$(( $(_rad_done host "$id" | tr ',' '\n' | wc -l) ))" stage
		expect_ge "radiants ($edid): the reward is in the host's pack" "$(( $(_rad_inv host) - h0 ))" 1 item
		expect_ge "radiants ($edid): the reward is in the other game's pack" "$(( $(_rad_inv vm) - v0 ))" 1 item
		expect_eq "radiants ($edid): no spawns made or placed for it" "$(_rad_spawned host) / $(_rad_spawned vm)" "$s0"
	done
}

# ---- radiant-rr: a Railroad job whose target is made at runtime, shared by its run with the host's actor (opt-in: smoke.sh --only radiant-rr) ----
# src/game/RadiantRules.h (Entry::spawned), RadiantSync (Refill, ResendChangedFills, ForceAliasRef), Spawns (FollowedRadiantOf,
# RemoveFollowedActor). Variable Removal (RRR07): the host's console starts the job and takes it (`startquest`, then `setstage 100`: as
# PAM would hand it out; set-up, said so), which shares its run; the VM's game starts the same quest with the host's place (or, holding
# its own on offer, stops that for the host's, or, running it with the same place already, joins it: its log says which) and shows the
# same CourserLocation. Then both players are placed at the courser by console (`player.moveto`: placement to reach the scene, said so;
# the host to its courser, the VM to its own, made at the same place, then beside the host's, where its copy is placed): the host's
# courser, hostile, is shared (a spawn), the host's run
# is sent again with its spawn id, the VM's own courser is removed (`followRemoved=`) and its CourserTarget alias is forced onto its copy
# of the host's. The host's console kills the courser with the player as killer (`kill 14`; the host's injected gunfire if it still
# stands; set-up: the fight step tests shooting), and it must read dead: its 200 is set in both games, and the
# copy is dead in the VM's (held by the protected alias there: a FAIL since the kill lifts the protection, 2026-11-12); the host's console sets 300 (the turn-in at PAM, a conversation): 300 and 400 done in both, the quest
# stopped in both. Quest stages and the Railroad's offer count are for good in a save: throwaway saves only (backup.sh before, restore
# after). Neither game may have Variable Removal given to its player already (a WARN, not run).
RR_QUEST=${RR_QUEST:-00186C79}; RR_EDID=${RR_EDID:-RRR07}; RR_LOC_ALIAS=${RR_LOC_ALIAS:-2}; RR_ACTOR_ALIAS=${RR_ACTOR_ALIAS:-5}; RR_TAKEN=${RR_TAKEN:-100}
_rr_log() { win_read rr "$1"; }   # the game's log since the step's mark
_rr_seen() { grep -qE "$2" "$(_rr_log "$1")"; }   # _rr_seen <who> <regex>
_rr_last() { grep -E "$2" "$(_rr_log "$1")" | tail -1 | cut -c1-700; }  # long: a force's WARN ends with what the alias holds
_rr_alias() { dc "$1" "aliases $2" | tr '|' '\n' | grep -E "^ (Ref|Loc) $3 " | grep -o 'here=[0-9A-F]*' | sed 's/here=//'; }   # _rr_alias <who> <quest> <alias id>
_rr_alias_is() { [ "$(_rr_alias "$1" "$2" "$3")" = "$4" ]; }
_rr_removed() { field "$(dc "$1" 'net status')" followRemoved; }
_rr_stopped() { [ "$(_rad_run "$1" "$2")" = running=false ]; }
step_radiant_rr() {
	section "radiant-rr (a Railroad job's courser made at runtime: the host's stands in both games; console start, placement and kill as set-up)"
	local loc courser own spawn copy r0 who i
	for who in host vm; do
		if _rad_stage_once "$who" "$RR_QUEST" "$RR_TAKEN"; then warn "radiant-rr: not run: the $who's player has $RR_EDID already (stages $(_rad_done "$who" "$RR_QUEST"))" "throwaway saves without it"; return; fi
	done
	win_mark rr host; win_mark rr vm
	dc host "console startquest $RR_QUEST" >/dev/null; sleep 3
	dc host "console setstage $RR_QUEST $RR_TAKEN" >/dev/null
	echo "radiant-rr: the host's console started $RR_EDID and set its $RR_TAKEN (taken from PAM: set-up)"
	if wait_for 30 _rr_seen host "RadiantSync: sharing our run [0-9]+ of $RR_QUEST"; then pass "radiant-rr: the host shares its run of $RR_EDID" "$(_rr_last host "RadiantSync: sharing our run [0-9]+ of $RR_QUEST")"
	else fail "radiant-rr: the host shares its run of $RR_EDID" "$(_rad_run host "$RR_EDID") done $(_rad_done host "$RR_QUEST"); $(_rr_last host "RadiantSync")"; return; fi
	if wait_for 60 _rr_seen vm "RadiantSync: sharing player [0-9]+'s run [0-9]+ of $RR_QUEST"; then pass "radiant-rr: the VM shares the host's run" "$(_rr_last vm "RadiantSync: sharing player [0-9]+'s run [0-9]+ of $RR_QUEST")"
	else fail "radiant-rr: the VM shares the host's run" "$(_rad_run vm "$RR_EDID"); $(_rr_last vm "RadiantSync: .*$RR_QUEST")"; return; fi
	loc=$(_rr_alias host "$RR_QUEST" "$RR_LOC_ALIAS")
	expect_eq "radiant-rr: the same place (alias $RR_LOC_ALIAS) in both games" "$(_rr_alias vm "$RR_QUEST" "$RR_LOC_ALIAS")" "$loc"
	courser=$(_rr_alias host "$RR_QUEST" "$RR_ACTOR_ALIAS"); own=$(_rr_alias vm "$RR_QUEST" "$RR_ACTOR_ALIAS")
	if [ -z "${courser//0}" ]; then fail "radiant-rr: the host's $RR_EDID made its courser (alias $RR_ACTOR_ALIAS)" "here=$courser"; return; fi
	r0=$(_rr_removed vm)
	dc host "console player.moveto $courser" >/dev/null; wait_ingame host 40
	if [ -n "${own//0}" ]; then dc vm "console player.moveto $own" >/dev/null; else dc vm "console player.moveto $courser" >/dev/null; fi; wait_ingame vm 40
	echo "radiant-rr: both placed at the courser by console (set-up): the host at its $courser, the VM at its own ${own:-none}"
	if wait_for 60 _rr_seen host "Spawns: sharing $courser .* as [0-9A-F]{8}"; then spawn=$(_rr_last host "Spawns: sharing $courser .* as [0-9A-F]{8}" | sed -nE "s/.*Spawns: sharing $courser .* as ([0-9A-F]{8}).*/\1/p"); pass "radiant-rr: the host shares its courser" "as $spawn"
	else fail "radiant-rr: the host shares its courser" "no 'Spawns: sharing $courser' in its log: $(_rr_last host "Spawns: ")"; return; fi
	# The VM's own courser stands at a marker of the same place that the VM's game picked, not the host's (2026-11-12: the VM's game
	# placed no copy of the host's courser, and logged nothing: a spawn past SpawnBook's PLACE_RANGE, 6000 u, is never logged). Now the
	# host's is shared, the VM is placed by console beside the host's courser (set-up, said so), where its copy is placed.
	# A setpos moves the player within the space it stands in: when the host's courser stands in an interior (2026-11-12: the host's in
	# UniversityPoint01, the VM's own at an exterior marker of the same place, the VM set to the right x and y outdoors, no copy placed),
	# the VM's console first coc's into that cell, by its editor ID, then the setpos.
	local cf cx cy cz ccell hcell cedid
	cf=$(dc host "refinfo $courser"); cx=$(field "$cf" x); cy=$(field "$cf" y); cz=$(field "$cf" z); ccell=$(field "$cf" cell)
	hcell=$(dc host status)
	if [ "$(field "$hcell" interior)" = true ] && [ -n "$ccell" ] && [ "$ccell" != "$(field "$(dc vm status)" cell)" ]; then
		cedid=$(_cell_edid host "$ccell")
		if [ -n "$cedid" ]; then
			dc vm "console coc $cedid" >/dev/null; sleep 14; wait_ingame vm 40
			echo "radiant-rr: the host's courser stands in the interior $ccell ($cedid): the VM's console coc's into it first (set-up): VM $(dc vm status | cut -c1-120)"
		else echo "radiant-rr: the host's courser stands in the interior $ccell, whose editor ID could not be read: the VM's setpos stays in its own space"; fi
	fi
	if is_num "$cx" && is_num "$cy" && is_num "$cz"; then
		echo "radiant-rr: the VM's player placed by console beside the host's courser at ($cx,$cy) in $ccell (set-up); it stood $(_q_moved "x=$cx y=$cy" "$(dc vm pos)") u from it"
		_qa_place_near "$cx" "$cy" "$cz" 300 0 vm
	else echo "radiant-rr: where the host's courser stands could not be read ('$(cut -c1-120 <<<"$cf")'): the VM stays at its own"; fi
	if wait_for 30 _rr_seen host "RadiantSync: our run [0-9]+ of $RR_QUEST .*: its fills changed"; then pass "radiant-rr: the host's run is sent again with the courser's spawn id" "ok"
	else fail "radiant-rr: the host's run is sent again with the courser's spawn id" "$(_rr_last host "RadiantSync: our run")"; fi
	if wait_for 60 _rr_seen vm "RadiantSync: player [0-9]+'s run [0-9]+ of $RR_QUEST .*: alias $RR_ACTOR_ALIAS forced onto our copy [0-9A-F]{8}"; then
		copy=$(_rr_last vm "alias $RR_ACTOR_ALIAS forced onto our copy" | sed -nE 's/.*forced onto our copy ([0-9A-F]{8}).*/\1/p')
		pass "radiant-rr: the VM's CourserTarget is forced onto its copy of the host's courser" "copy $copy of $spawn"
	else fail "radiant-rr: the VM's CourserTarget is forced onto its copy of the host's courser" "$(_rr_last vm "RadiantSync: player .*$RR_QUEST"); copy placed: '$(_rr_last vm "Spawns: placed .* as our copy of player [0-9]+'s $spawn")'; $(dc vm 'net status' | grep -oE ' notShared=[0-9]+ unplaced=[0-9/]+'); VM $(dc vm pos | cut -c1-60)"; return; fi
	if wait_for 20 _rr_alias_is vm "$RR_QUEST" "$RR_ACTOR_ALIAS" "$copy"; then pass "radiant-rr: the VM's alias $RR_ACTOR_ALIAS holds the copy" "here=$copy"
	else fail "radiant-rr: the VM's alias $RR_ACTOR_ALIAS holds the copy" "here=$(_rr_alias vm "$RR_QUEST" "$RR_ACTOR_ALIAS"), copy $copy; $(_rr_last vm "alias $RR_ACTOR_ALIAS (forced again|still holds|could not be forced|waits to be forced)"); $(dc vm 'net status' | grep -oE ' refillsFailed=[0-9]+ refillsRetried=[0-9]+ refillsHeld=[0-9]+ refillsGivenUp=[0-9]+ forceCalls=[0-9/]+ forceDirect=[0-9]+ forceWaits=[0-9]+')"; fi
	expect_ge "radiant-rr: the VM's own courser is removed" "$(( $(_rr_removed vm) - r0 ))" 1 removed
	# The kill names the player as its killer (`kill 14`): CourserTarget is a protected alias, which a bare console `kill` (no killer)
	# leaves alive, so 200 never came (2026-11-11: "done 0,50,100"). If the courser still stands, the host's real gunfire at it
	# (injected clicks, turnto aims: allowed for combat set-ups), a few bursts. Then it must read dead before 200 is waited for.
	dc host "console $courser.kill 14" >/dev/null
	echo "radiant-rr: the host's console killed its courser with the player as killer (kill 14: set-up)"
	if ! wait_for 10 _e_dead host "$courser"; then
		echo "radiant-rr: the courser still stands after kill 14: the host is given a pistol (00004822) and 300 rounds (0001F276), equips and draws it (console additem and equipitem: set-up), then fires at it (injected gunfire, set-up)"
		_e_arm host; sleep 2
		for i in 1 2 3 4 5 6 7 8; do _e_shoot host "$courser"; sleep 1.5; _e_dead host "$courser" && break; done
	fi
	if wait_for 10 _e_dead host "$courser"; then pass "radiant-rr: set-up: the host's courser is dead (killed by the player)" "$(dc host "ghosts $courser" | cut -c1-120)"
	else fail "radiant-rr: set-up: the host's courser is dead (killed by the player)" "$(dc host "ghosts $courser" | cut -c1-160)"; return; fi
	if wait_for 60 _rad_stage_once host "$RR_QUEST" 200; then pass "radiant-rr: the kill's stage 200 in the host's game" "done $(_rad_done host "$RR_QUEST")"
	else fail "radiant-rr: the kill's stage 200 in the host's game" "done $(_rad_done host "$RR_QUEST")"; return; fi
	if wait_for 30 _rad_stage_once vm "$RR_QUEST" 200; then pass "radiant-rr: the kill's stage 200 in the VM's game too" "done $(_rad_done vm "$RR_QUEST")"
	else fail "radiant-rr: the kill's stage 200 in the VM's game too" "done $(_rad_done vm "$RR_QUEST")"; fi
	# The copy is held by the VM's CourserTarget, a protected alias: the host's death report must land on it all the same (the console's
	# kill names no killer, so Spawns lifts the protection for it: src/game/CopyKill.h). Its kill line, read here, also brings the
	# step's log delta past the kill (windelta-rr-vm.txt).
	if wait_for 20 _e_dead vm "$copy"; then pass "radiant-rr: the copy is dead in the VM's game" "$(_rr_last vm "Spawns: killed $copy" | cut -c1-200)"
	else fail "radiant-rr: the copy is dead in the VM's game" "ghosts $copy: $(dc vm "ghosts $copy" | cut -c1-120); $(_rr_last vm "Spawns: (killed $copy|$copy still stands)"); $(dc vm 'net status' | grep -oE ' killsTried=[0-9]+ killsLifted=[0-9]+ killsWaiting=[0-9]+ killsGivenUp=[0-9]+')"; fi
	dc host "console setstage $RR_QUEST 300" >/dev/null
	echo "radiant-rr: the host's console set 300 (the turn-in at PAM: set-up)"
	wait_for 20 _rad_stage_once vm "$RR_QUEST" 300; wait_for 20 _rad_stage_once vm "$RR_QUEST" 400; sleep 3
	expect_eq "radiant-rr: the same stages are done in both games" "$(_rad_done vm "$RR_QUEST")" "$(_rad_done host "$RR_QUEST")"
	if wait_for 20 _rr_stopped host "$RR_EDID" && wait_for 20 _rr_stopped vm "$RR_EDID"; then pass "radiant-rr: the job is over in both games" "ok"
	else fail "radiant-rr: the job is over in both games" "host $(_rad_run host "$RR_EDID") vm $(_rad_run vm "$RR_EDID")"; fi
}

# ---- radiant-inst: an Institute job whose synth is made at runtime and never hostile, shared by its run with the host's (opt-in: smoke.sh --only radiant-inst) ----
# src/game/RadiantRules.h (Entry::spawned for InstR03NEW, InstR04, BoSR03, BoSR04), Spawns::OwnedRadiantOf (a shared radiant's own actors
# shared calm too: the squire, the scribe and the synth are never hostile, so the fight rule would never share them), RadiantSync::FollowsSpawn.
# Reclamation (InstR04): the host's console starts the job and sets its 100 (as the Institute's giver would hand it out: set-up, said so), which
# enables and restrains the synth at its dungeon's marker and shares the run; the VM's game starts the same quest with the host's dungeon (or
# replaces or joins its own: its log says which). Both players are placed at the synth by console (`player.moveto`: placement to reach the
# scene, said so): the host's calm synth is shared (`radiantActors=`), the host's run is sent again with its spawn id, the VM's own synth is
# removed (`followRemoved=`) and its Synth alias is forced onto its copy of the host's. The host's console sets 200 (the synth recalled: in play
# a conversation with it, the owner's) and 1000 (the turn-in): both stages in both games, the job over in both. Quest stages are for good in a
# save: throwaway saves only (backup.sh before, restore after). Neither game may have Reclamation given to its player already (a WARN, not run).
INST_QUEST=${INST_QUEST:-000EDE28}; INST_EDID=${INST_EDID:-InstR04}; INST_LOC_ALIAS=${INST_LOC_ALIAS:-1}; INST_ACTOR_ALIAS=${INST_ACTOR_ALIAS:-6}; INST_TAKEN=${INST_TAKEN:-100}
_inst_radiant_actors() { field "$(dc "$1" 'net status')" radiantActors; }
step_radiant_inst() {
	section "radiant-inst (an Institute job's synth made at runtime, calm: the host's stands in both games; console start, placement and stages as set-up)"
	local loc synth own spawn copy r0 who
	for who in host vm; do
		if _rad_stage_once "$who" "$INST_QUEST" "$INST_TAKEN"; then warn "radiant-inst: not run: the $who's player has $INST_EDID already (stages $(_rad_done "$who" "$INST_QUEST"))" "throwaway saves without it"; return; fi
	done
	win_mark rr host; win_mark rr vm
	dc host "console startquest $INST_QUEST" >/dev/null; sleep 3
	dc host "console setstage $INST_QUEST $INST_TAKEN" >/dev/null
	echo "radiant-inst: the host's console started $INST_EDID and set its $INST_TAKEN (handed out by the Institute: set-up)"
	if wait_for 30 _rr_seen host "RadiantSync: sharing our run [0-9]+ of $INST_QUEST"; then pass "radiant-inst: the host shares its run of $INST_EDID" "$(_rr_last host "RadiantSync: sharing our run [0-9]+ of $INST_QUEST")"
	else fail "radiant-inst: the host shares its run of $INST_EDID" "$(_rad_run host "$INST_EDID") done $(_rad_done host "$INST_QUEST"); $(_rr_last host "RadiantSync")"; return; fi
	if wait_for 60 _rr_seen vm "RadiantSync: sharing player [0-9]+'s run [0-9]+ of $INST_QUEST"; then pass "radiant-inst: the VM shares the host's run" "$(_rr_last vm "RadiantSync: sharing player [0-9]+'s run [0-9]+ of $INST_QUEST")"
	else fail "radiant-inst: the VM shares the host's run" "$(_rad_run vm "$INST_EDID"); $(_rr_last vm "RadiantSync: .*$INST_QUEST")"; return; fi
	loc=$(_rr_alias host "$INST_QUEST" "$INST_LOC_ALIAS")
	expect_eq "radiant-inst: the same dungeon (alias $INST_LOC_ALIAS) in both games" "$(_rr_alias vm "$INST_QUEST" "$INST_LOC_ALIAS")" "$loc"
	synth=$(_rr_alias host "$INST_QUEST" "$INST_ACTOR_ALIAS"); own=$(_rr_alias vm "$INST_QUEST" "$INST_ACTOR_ALIAS")
	if [ -z "${synth//0}" ]; then fail "radiant-inst: the host's $INST_EDID made its synth (alias $INST_ACTOR_ALIAS)" "here=$synth"; return; fi
	r0=$(_rr_removed vm)
	dc host "console player.moveto $synth" >/dev/null; wait_ingame host 40
	if [ -n "${own//0}" ]; then dc vm "console player.moveto $own" >/dev/null; else dc vm "console player.moveto $synth" >/dev/null; fi; wait_ingame vm 40
	echo "radiant-inst: both placed at the synth by console (set-up): the host at its $synth, the VM at its own ${own:-none}"
	if wait_for 60 _rr_seen host "Spawns: sharing $synth .* as [0-9A-F]{8}, made by our shared radiant"; then spawn=$(_rr_last host "Spawns: sharing $synth .* as [0-9A-F]{8}" | sed -nE "s/.*Spawns: sharing $synth .* as ([0-9A-F]{8}).*/\1/p"); pass "radiant-inst: the host shares its calm synth" "as $spawn ($(_inst_radiant_actors host) radiant actors shared)"
	else fail "radiant-inst: the host shares its calm synth" "no 'Spawns: sharing $synth' in its log: $(_rr_last host "Spawns: ")"; return; fi
	if wait_for 30 _rr_seen host "RadiantSync: our run [0-9]+ of $INST_QUEST .*: its fills changed"; then pass "radiant-inst: the host's run is sent again with the synth's spawn id" "ok"
	else fail "radiant-inst: the host's run is sent again with the synth's spawn id" "$(_rr_last host "RadiantSync: our run")"; fi
	if wait_for 60 _rr_seen vm "RadiantSync: player [0-9]+'s run [0-9]+ of $INST_QUEST .*: alias $INST_ACTOR_ALIAS forced onto our copy [0-9A-F]{8}"; then
		copy=$(_rr_last vm "alias $INST_ACTOR_ALIAS forced onto our copy" | sed -nE 's/.*forced onto our copy ([0-9A-F]{8}).*/\1/p')
		pass "radiant-inst: the VM's Synth alias is forced onto its copy of the host's synth" "copy $copy of $spawn"
	else fail "radiant-inst: the VM's Synth alias is forced onto its copy of the host's synth" "$(_rr_last vm "RadiantSync: player .*$INST_QUEST"); $(_rr_last vm "Spawns: .*$spawn")"; return; fi
	if wait_for 20 _rr_alias_is vm "$INST_QUEST" "$INST_ACTOR_ALIAS" "$copy"; then pass "radiant-inst: the VM's alias $INST_ACTOR_ALIAS holds the copy" "here=$copy"
	else fail "radiant-inst: the VM's alias $INST_ACTOR_ALIAS holds the copy" "here=$(_rr_alias vm "$INST_QUEST" "$INST_ACTOR_ALIAS"), copy $copy"; fi
	expect_ge "radiant-inst: the VM's own synth is removed" "$(( $(_rr_removed vm) - r0 ))" 1 removed
	dc host "console setstage $INST_QUEST 200" >/dev/null
	echo "radiant-inst: the host's console set 200 (the synth recalled, a conversation in play: set-up)"
	if wait_for 30 _rad_stage_once vm "$INST_QUEST" 200; then pass "radiant-inst: the recall's stage 200 in the VM's game too" "done $(_rad_done vm "$INST_QUEST")"
	else fail "radiant-inst: the recall's stage 200 in the VM's game too" "done $(_rad_done vm "$INST_QUEST")"; fi
	dc host "console setstage $INST_QUEST 1000" >/dev/null
	echo "radiant-inst: the host's console set 1000 (the turn-in: set-up)"
	wait_for 20 _rad_stage_once vm "$INST_QUEST" 1000; sleep 3
	expect_eq "radiant-inst: the same stages are done in both games" "$(_rad_done vm "$INST_QUEST")" "$(_rad_done host "$INST_QUEST")"
	if wait_for 20 _rr_stopped host "$INST_EDID" && wait_for 20 _rr_stopped vm "$INST_EDID"; then pass "radiant-inst: the job is over in both games" "ok"
	else fail "radiant-inst: the job is over in both games" "host $(_rad_run host "$INST_EDID") vm $(_rad_run vm "$INST_EDID")"; fi
}

# ---- radiant-min: Preston's "defend the settlement", the host's attack the only one (opt-in: smoke.sh --only radiant-min) ----
# src/game/RadiantRules.h (Entry::attack, Begins, HoldsAttack, UNTAKEN and OWNER_STAGES' attack rows), RadiantSync (HoldAttacks). The
# raiders attacking a settlement (MinRadiantOwned01) at MIN_WORKSHOP: the host's console starts the job as Preston's event would
# (`radiantstart` with the settlement, its FoundWorkshop named as it too, the settlement first made the host's by SetOwnedByPlayer: set-up,
# said so) and sets its 100 (taken from Preston: set-up), which shares its run (the host
# hosts); the VM's game starts the same quest with the host's settlement and holds its own attack back ("is the host's attack",
# `attacksHeld=1`). Then, with the host away at MIN_AWAY_CELL: the VM's player comes to the settlement (console coc: placement, said so):
# its own arrival (70) never reaches the host's game, where the attack's clock would start without the host. The host's console sets
# 120 (the defences built: set-up), which the host's game answers with its attack's start (130; a night attack waits for the night: a
# WARN); the VM takes the 120 and never starts its own attack (its 130 and 300 never done) for MIN_HOLD_WAIT s, near the settlement and
# then away from it. Both come to the settlement (set-up): the attack ends by the host's game (its 200, the attackers dead, or 310, the
# attack failed; set by the host's console after MIN_END_WAIT s if neither came: set-up, said so), and the VM takes that end; its own 300,
# which rolls the settlement's damage, is never done. On a 200 the host's console sets 400 (the turn-in to the settlement: set-up): the
# job is over in both games, and the VM lets its hold go. Quest stages are for good in a save: throwaway saves only (backup.sh before,
# restore after). Neither game may run the job already (a WARN, not run). With the hold, the VM's settlers' count is held too (its
# log's "its settlers' deaths are the host's to count", its Settlers group raised by 10000: RadiantRules::ATTACK_SETTLERS_HELD), and
# as the job ends given back to the size it had ("its own to count again"; `net status` settlersHeld=0).
# Every "never" is read from the game's answer (_mr_never): a game that gives none (dead, or not answering) is a broken rig and a FAIL,
# never a pass. MIN_FAIL=1 ends the attack the other way, with both players away: after the VM's arrival (70) the VM's player goes back
# to MIN_AWAY_CELL (the settlement not loaded in its game: where an attack's 300 would roll the damage), the host's console sets 120 and,
# as soon as the host's attack starts (130; before it can end by 200 with nobody there), 290 (the settlers dead: set-up, said so); the
# host's game fails the job (its 300 and 310). Asserted in the VM's game: the host's 290 is set; its own 290 fragment sets its 300,
# whose fragment goes straight to 310 with 290 done (both done); its 130 and 200 are never done (no attack of its own, no defended
# branch of a damage roll); and its settlement values (`wsres sum`, happiness aside: 310 lowers it in both games) are the same after
# as before (no damage rolled). Then the job is over in both games and the hold let go, its timer cancelled, as on a 200. Either way
# the step also logs what the end did in the VM's game (_mr_300: its stages, its quest, the settlement's values before and after).
MIN_QUEST=${MIN_QUEST:-000A1412}; MIN_EDID=${MIN_EDID:-MinRadiantOwned01}; MIN_WORKSHOP=${MIN_WORKSHOP:-000250FE}; MIN_WORKSHOP_ALIAS=${MIN_WORKSHOP_ALIAS:-9}
MIN_NEAR_CELL=${MIN_NEAR_CELL:-SanctuaryExt}; MIN_AWAY_CELL=${MIN_AWAY_CELL:-CorvegaAssemblyPlantExt03}; MIN_HOLD_WAIT=${MIN_HOLD_WAIT:-40}; MIN_END_WAIT=${MIN_END_WAIT:-180}
MIN_FAIL=${MIN_FAIL:-0}; MIN_FOUND_ALIAS=${MIN_FOUND_ALIAS:-23}   # FoundWorkshop
# The aliases MinRadiantOwned01 needs (Fallout4.esm, none optional): Workshop (9), which has no fill of its own and is filled only by
# FoundWorkshop's or EventWorkshop's "force into alias 9" (ALFI); ActualLocation (2, Workshop's location); SettlementCenterMarker (3, the
# location's LocationCenterMarker); PrestonGarvey (7, unique); Raiders (13, made at SettlementAttackMarker, 14, an optional alias: the
# out-of-sight markers linked to the center marker by WorkshopLinkAttackMarker); Player (22). With FoundWorkshop and EventWorkshop
# forced (2026-11-12) the game still couldn't fill one, and its answer doesn't name it. So the start is tried again, each time with
# one more of them forced as Sanctuary has it (MIN_FORCE_MORE, one group per try; set-up, each answer printed): the first try that
# starts names the alias by the group it added (a force that skips the force-into, the likeliest, is 9), and is a FAIL: such a start
# is not the one Preston's event makes.
MIN_FORCE_MORE=${MIN_FORCE_MORE:-"9=000250FE|2=0001F228|3=0004BE7A 14=000A3772|7=0001A4D7"}   # Sanctuary: bench, location, center and attack markers; Preston
MIN_SPOKESMAN_ALIAS=${MIN_SPOKESMAN_ALIAS:-6}; MIN_SETTLER_BASE=${MIN_SETTLER_BASE:-00020593}   # SettlementSpokesman; WorkshopNPC
_mr_settler() {  # _mr_settler <who>: a settler made in who's game (in front of its player) and added to MIN_WORKSHOP; prints its reference
	local s
	s=$(dc "$1" "wsreport place $MIN_SETTLER_BASE $MIN_WORKSHOP quiet" | grep -o "[0-9A-F]\{8\}" | head -1)
	[ -z "$s" ] && return
	dc "$1" "console cqf WorkshopParent \"WorkshopParentScript.AddActorToWorkshopPUBLIC\" $s $MIN_WORKSHOP" >/dev/null; sleep 3
	echo "$s"
}
_mr_boss() { [ -n "$2" ] && echo "workshopID $(field "$(dc "$1" "scriptvars $2 WorkshopNPCScript workshopID")" workshopID) of $(field "$(dc "$1" "scriptvars $MIN_WORKSHOP WorkshopScript workshopID")" workshopID)"; }   # _mr_boss <who> <settler>: the settler's workshop and Sanctuary's
# The settlers the step made (MR_SETTLERS, "who:ref ..."), taken out of both games after it however it ended (set-up undone, said so):
# left standing, a later step at Sanctuary would meet them.
MR_SETTLERS=""
_mr_settlers_off() {
	local s
	for s in $MR_SETTLERS; do
		dc "${s%%:*}" "console ${s#*:}.disable" >/dev/null; dc "${s%%:*}" "console ${s#*:}.markfordelete" >/dev/null
		echo "radiant-min: the ${s%%:*}'s settler ${s#*:} (set-up) disabled and marked for delete by its console"
	done
	MR_SETTLERS=""
}
step_radiant_min() { MR_SETTLERS=""; _step_radiant_min; _mr_settlers_off; }
_mr_never() {  # _mr_never <who> <check name> <stage>...: none of the stages is done in who's game, by its own answer
	local who=$1 name=$2 got st; shift 2
	got=$(_rad_done "$who" "$MIN_QUEST")
	if [ -z "$got" ]; then rig_broken game "$name" "the $who's game gave no answer to 'stages $MIN_QUEST': nothing read, so nothing shown"; return 1; fi
	for st; do
		if [[ ",$got," == *",$st,"* ]]; then fail "$name" "its $st is done; $who done $got"; return 1; fi
	done
	pass "$name" "$who done $got"
}
_mr_values() { dc vm "wsres sum $MIN_WORKSHOP" | sed 's/; .*//' | cut -c1-900; }   # the settlement's values in the VM's game (its pieces left out)
_mr_300() {  # _mr_300 <when> [values before]: what the VM's game holds of the attack's end; a log for the reader, no verdict
	local got values
	got=$(_rad_done vm "$MIN_QUEST"); values=$(_mr_values)
	echo "radiant-min: (300) $1: vm done '${got:-no answer}', its own 300 $([[ ",$got," == *",300,"* ]] && echo done || echo 'not done'), 310 $([[ ",$got," == *",310,"* ]] && echo done || echo 'not done'), ${MIN_EDID} $(_rad_run vm "$MIN_EDID")"
	echo "radiant-min: (300) $1: the VM's settlement: ${values:-no answer}"
	if [ $# -ge 2 ]; then
		if [ -z "$values" ] || [ -z "$2" ]; then echo "radiant-min: (300) the VM's settlement values could not be read both times: nothing to compare"
		elif [ "$values" = "$2" ]; then echo "radiant-min: (300) the VM's settlement values are the same after the end as before it"
		else echo "radiant-min: (300) the VM's settlement values CHANGED over the end; before: $2"; fi
		grep -E "$MIN_QUEST" "$(_rr_log vm)" | tail -4 | cut -c1-220 | sed "s/^/radiant-min: (300) the VM's log: /"
	fi
}
_mr_held() { field "$(dc "$1" 'net status')" attacksHeld; }
_mr_settlers_held() { field "$(dc "$1" 'net status')" settlersHeld; }   # the held attacks whose settlers' count is held too (RadiantRules::ATTACK_SETTLERS_HELD)
_mr_cancelled() { field "$(dc "$1" 'net status')" attackTimersCancelled; }
_mr_held_is() { [ "$(_mr_held "$1")" = "$2" ]; }
_mr_ended() { _rad_stage_once "$1" "$MIN_QUEST" 200 || _rad_stage_once "$1" "$MIN_QUEST" 310; }
_mr_own_attack() { _rad_stage_once vm "$MIN_QUEST" 130 || _rad_stage_once vm "$MIN_QUEST" 300; }   # the VM's own attack began, or rolled its damage
_mr_go() { dc "$1" "console coc $2" >/dev/null; sleep 14; wait_ingame "$1" 30; }
# Its stages done, but 130 and 300 (each game's own attack) and 70 and 80 (each game's own arrival). 70 is the VM's own in every run
# (RadiantRules OWNER_STAGES: never reported); 80 follows it in the VM's game when its arrival beats the host's 20/100 (a scene can
# hold the host's: SceneHold); the host's own 70 is missing when its attack resolves before its player arrives. None is shared.
_mr_but_attack() { _rad_done "$1" "$MIN_QUEST" | tr ',' '\n' | grep -vxE "70|80|130|300" | paste -sd, -; }
_mr_failed_here() { _rad_stage_once "$1" "$MIN_QUEST" 300 && _rad_stage_once "$1" "$MIN_QUEST" 310; }   # its 300 and 310 done
_mr_but_happiness() { sed -E 's/ happiness: [^)]*\)//' <<<"$1"; }   # _mr_values without the happiness (310 lowers it in every game)
_step_radiant_min() {
	section "radiant-min (Preston's settlement attack: the host's run shared, the VM's own attack held back; console start, stages and placement as set-up)"
	local who end before vmdone hostdone c0 c1
	for who in host vm; do
		if [ "$(_rad_run "$who" "$MIN_EDID")" != running=false ]; then warn "radiant-min: not run: the $who's game runs $MIN_EDID already ($(_rad_run "$who" "$MIN_EDID"))" "throwaway saves without it"; return; fi
	done
	_mr_go host "$MIN_AWAY_CELL"; _mr_go vm "$MIN_AWAY_CELL"
	echo "radiant-min: both at $MIN_AWAY_CELL by console coc (set-up)"
	win_mark rr host; win_mark rr vm
	c0=$(_mr_cancelled vm)
	# An empty count here was read as 0 by the cancel check at the end, which then passed on a cancel from before the step.
	if ! [[ "$c0" =~ ^[0-9]+$ ]]; then rig_broken game "radiant-min: the VM's count of cancelled attack timers before the step" "no number for attackTimersCancelled in its 'net status' ('$c0'): the cancel check at the end could not be made"; return; fi
	# A coc character owns no settlement, and the job is Preston's for one the player owns: the settlement made the host's first
	# (WorkshopScript.SetOwnedByPlayer, as the bench step does; set-up), and FoundWorkshop (MIN_FOUND_ALIAS, a matching alias) named as
	# the settlement too, as Preston's own pick would be (2026-11-11: "the game couldn't fill an alias it needs", twice).
	dc host "console $MIN_WORKSHOP.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 2
	# The job's SettlementSpokesman (alias 6) is needed and is filled only by SettlementBoss (21) or SettlementLeader (20), both
	# optional: the settlement location's Boss or SettlementLeader reference, which WorkshopParent makes of each settler it makes
	# (SetAsBoss in AddActorToWorkshop). A coc world's Sanctuary has nobody, so no game can fill it (2026-11-12, run 4: "couldn't fill
	# an alias it needs" with every other needed alias forced, the event alias EventWorkshop being optional). Preston's own event comes
	# only for a settlement with people (FoundWorkshop and EventWorkshop want WorkshopRatingPopulation above 0), so a player's start
	# always has one. Set-up: a settler (WorkshopNPC) made in each game and added to Sanctuary (AddActorToWorkshopPUBLIC, as the
	# bench step adds Codsworth); the VM's is its own pick for the spokesman (the host's settler is no NPC the VM's game has:
	# RadiantRules::Plan::unforced, left to the aliases it follows there).
	local settler vmsettler
	settler=$(_mr_settler host); vmsettler=$(_mr_settler vm)
	MR_SETTLERS="${settler:+host:$settler} ${vmsettler:+vm:$vmsettler}"
	echo "radiant-min: set-up: a settler made by console and added to $MIN_WORKSHOP in each game: host ${settler:-none} ($(_mr_boss host "$settler")), VM ${vmsettler:-none} ($(_mr_boss vm "$vmsettler"))"
	local forced="$MIN_FOUND_ALIAS=$MIN_WORKSHOP" more answer first groups
	# The record's fill words of Workshop and the two that force into it, before the start: where the game keeps the force-into
	# (a reader's note for the alias it can't fill; a force writes over data words 1 and 2).
	dc host "aliases $MIN_QUEST" | tr '|' '\n' | grep -E "^ *[A-Za-z]+ (9|23|24) " | cut -c1-200 | sed "s/^ */radiant-min: set-up: the host's alias before the start: /"
	answer=$(dc host "radiantstart $MIN_QUEST $MIN_WORKSHOP - $forced"); first=$answer
	echo "radiant-min: the host's console made $MIN_WORKSHOP the host's (SetOwnedByPlayer: set-up); radiantstart with $forced: $(cut -c1-220 <<<"$answer")"
	# Only the fill failure is tried again (a "wait" or another error names no alias). A start that needed more forced than Preston's
	# event gives (the settlement, FoundWorkshop) is not a start the mod made: a FAIL naming the group, and the rest of the step runs on
	# it for the reader (a force-into that doesn't reach Workshop would be RadiantSync's own, the forcing a friend's game follows a run by).
	case "$answer" in *"couldn't fill an alias"*)
		IFS='|' read -r -a groups <<<"$MIN_FORCE_MORE${settler:+|$MIN_SPOKESMAN_ALIAS=$settler}"
		for more in "${groups[@]}"; do
			[ -z "$more" ] && continue
			forced="$forced $more"
			answer=$(dc host "radiantstart $MIN_QUEST $MIN_WORKSHOP - $forced")
			echo "radiant-min: set-up: radiantstart again with $more forced too ($forced): $(cut -c1-220 <<<"$answer")"
			if [ "${answer#ok}" != "$answer" ]; then
				fail "radiant-min: set-up: the host's game starts $MIN_EDID with only the settlement and FoundWorkshop named, as Preston's event would" "it started only once $more was forced too (the alias it couldn't fill is among those; forced: $forced); the first answer: $(cut -c1-200 <<<"$first")"
				break
			fi
		done;;
	esac
	if ! wait_for 20 _rad_is_running host "$MIN_EDID"; then fail "radiant-min: the host's game runs $MIN_EDID at $MIN_WORKSHOP" "$(_rad_run host "$MIN_EDID"); $(_rr_last host "RadiantSync"); owned: $(dc host "scriptvars $MIN_WORKSHOP WorkshopScript OwnedByPlayer" | cut -c1-80)"; return; fi
	dc host "aliases $MIN_QUEST" | tr '|' '\n' | grep -E "^ *Ref ($MIN_SPOKESMAN_ALIAS|20|21) " | grep -o '^ *Ref [0-9]* [A-Za-z]*\|here=[0-9A-F]*' | paste -sd' ' | sed "s/^ */radiant-min: the host's spokesman and its sources after the start: /"
	dc host "console setstage $MIN_QUEST 100" >/dev/null
	echo "radiant-min: the host's console started $MIN_EDID with the settlement $MIN_WORKSHOP and set its 100 (taken from Preston: set-up)"
	if wait_for 30 _rr_seen host "RadiantSync: sharing our run [0-9]+ of $MIN_QUEST"; then pass "radiant-min: the host shares its run of $MIN_EDID" "$(_rr_last host "RadiantSync: sharing our run [0-9]+ of $MIN_QUEST")"
	else fail "radiant-min: the host shares its run of $MIN_EDID (the host hosts the session)" "done $(_rad_done host "$MIN_QUEST"); $(_rr_last host "RadiantSync")"; return; fi
	if wait_for 60 _rr_seen vm "RadiantSync: sharing player [0-9]+'s run [0-9]+ of $MIN_QUEST"; then pass "radiant-min: the VM shares the host's run" "$(_rr_last vm "RadiantSync: sharing player [0-9]+'s run [0-9]+ of $MIN_QUEST")"
	else fail "radiant-min: the VM shares the host's run" "$(_rad_run vm "$MIN_EDID"); $(_rr_last vm "RadiantSync: .*$MIN_QUEST")"; return; fi
	expect_eq "radiant-min: the same settlement (alias $MIN_WORKSHOP_ALIAS) in both games" "$(_rr_alias vm "$MIN_QUEST" "$MIN_WORKSHOP_ALIAS")" "$(_rr_alias host "$MIN_QUEST" "$MIN_WORKSHOP_ALIAS")"
	if wait_for 10 _rr_seen vm "RadiantSync: $MIN_QUEST '[^']*' is the host's attack: this game's own is held back"; then pass "radiant-min: the VM holds its own attack back" "attacksHeld=$(_mr_held vm)"
	else fail "radiant-min: the VM holds its own attack back" "attacksHeld=$(_mr_held vm); $(_rr_last vm "RadiantSync: .*$MIN_QUEST")"; return; fi
	expect_eq "radiant-min: the host holds nothing back (its attack is the one)" "$(_mr_held host)" 0
	# Its settlers' deaths are the host's to count while held: the VM's Settlers group (the one whose stage is 290) is raised by 10000.
	if wait_for 10 _rr_seen vm "RadiantSync: $MIN_QUEST '[^']*': its settlers' deaths are the host's to count"; then
		pass "radiant-min: the VM holds its settlers' count back with its attack (its Settlers group raised by 10000)" "$(_rr_last vm "RadiantSync: $MIN_QUEST '[^']*': its settlers' deaths are the host's to count" | grep -o 'group .*'); settlersHeld=$(_mr_settlers_held vm)"
	else fail "radiant-min: the VM holds its settlers' count back with its attack (its Settlers group raised by 10000)" "settlersHeld=$(_mr_settlers_held vm); $(_rr_last vm "RadiantSync: .*$MIN_QUEST")"; fi

	# The VM near, the host away: the VM's arrival is its own.
	_mr_go vm "$MIN_NEAR_CELL"
	echo "radiant-min: the VM's player at $MIN_NEAR_CELL by console coc (set-up); the host's stays at $MIN_AWAY_CELL"
	if wait_for 20 _rad_stage_once vm "$MIN_QUEST" 70; then pass "radiant-min: the VM's arrival sets its own 70" "done $(_rad_done vm "$MIN_QUEST")"
	else warn "radiant-min: the VM's arrival sets its own 70 (CheckLocation: a coc may not change the location as walking in does)" "done $(_rad_done vm "$MIN_QUEST")"; fi
	sleep 5
	_mr_never host "radiant-min: the VM's arrival never starts the host's attack clock (the host's 70 not done)" 70

	if [ "$MIN_FAIL" = 1 ]; then
		# MIN_FAIL: the host's 290 (the settlers dead) comes while the settlement isn't loaded in the VM's game, the case where an
		# attack's own 300 would roll the settlement's damage (CheckResolveAttack), and before the host's attack can end by 200. The
		# VM's 290 fragment sets its own 300, whose fragment goes straight to 310 with 290 done: no damage roll, no 200 (the roll's
		# defended branch), and its settlement values (bar happiness, which 310 lowers in both games) the same after as before.
		_mr_go vm "$MIN_AWAY_CELL"
		echo "radiant-min: MIN_FAIL=1: the VM's player back at $MIN_AWAY_CELL by console coc (set-up), before the host's defences are built"
		before=$(_mr_values); _mr_300 "before the host's 290"
		dc host "console setstage $MIN_QUEST 120" >/dev/null
		echo "radiant-min: the host's console set 120 (the settlement's defences built: set-up)"
		# The 290 is set at once after the 120, with nothing waited on between: with both players away, the host's attack is resolved
		# by its damage roll (defended) within a second of 120 (2026-11-13: done 120,130,150,200 by the first read), so waiting for
		# the host's 130 first never found the attack still on. The host's player can't stay near for it without teleporting it back
		# (the step's only placements are the set-up cocs above), so 290 goes first instead; the VM's 120 is judged after it.
		if _mr_ended host; then
			fail "radiant-min: MIN_FAIL=1: the host's attack is still on as the host's console sets 290" "it already ended (host done $(_rad_done host "$MIN_QUEST")): the 290 path is not shown"
		else
			dc host "console setstage $MIN_QUEST 290" >/dev/null
			echo "radiant-min: MIN_FAIL=1: the host's console set 290 (the settlers dead: set-up); its game fails the job (its 300 and 310)"
			if wait_for 30 _rad_stage_once vm "$MIN_QUEST" 120; then pass "radiant-min: the host's 120 is set in the VM's game" "done $(_rad_done vm "$MIN_QUEST")"
			else fail "radiant-min: the host's 120 is set in the VM's game" "vm done $(_rad_done vm "$MIN_QUEST")"; fi
			if ! wait_for 30 _rad_stage_once host "$MIN_QUEST" 310; then
				echo "radiant-min: the host's game didn't fail the job by itself in 30 s (host done $(_rad_done host "$MIN_QUEST")); its console set 310 (the job failed: set-up)"
				dc host "console setstage $MIN_QUEST 310" >/dev/null
			fi
			if wait_for 30 _rad_stage_once vm "$MIN_QUEST" 290; then pass "radiant-min: the host's 290 (the settlers dead) is set in the VM's game" "done $(_rad_done vm "$MIN_QUEST")"
			else fail "radiant-min: the host's 290 (the settlers dead) is set in the VM's game" "host $(_rad_done host "$MIN_QUEST") vm $(_rad_done vm "$MIN_QUEST")"; fi
			if wait_for 30 _mr_failed_here vm; then pass "radiant-min: the VM's game fails the job too, by its own fragments (its 300 and 310 done)" "done $(_rad_done vm "$MIN_QUEST")"
			else fail "radiant-min: the VM's game fails the job too, by its own fragments (its 300 and 310 done)" "vm done $(_rad_done vm "$MIN_QUEST")"; fi
			sleep 5
			_mr_never vm "radiant-min: the VM's 300 rolled no damage: its own attack never began, nor did a roll defend it (its 130 and 200 not done)" 130 200
			local after; after=$(_mr_values)
			if [ -z "$before" ] || [ -z "$after" ] || [ "${before#error}" != "$before" ] || [ "${after#error}" != "$after" ]; then
				rig_broken game "radiant-min: the VM's settlement values are the same after its 300 as before (no damage roll; happiness aside)" "'wsres sum $MIN_WORKSHOP' not read both times (before '$before', after '$after')"
			else expect_eq "radiant-min: the VM's settlement values are the same after its 300 as before (no damage roll; happiness aside)" "$(_mr_but_happiness "$after")" "$(_mr_but_happiness "$before")"; fi
		fi
	else
		# The host's defences built: its attack starts in its game, never in the VM's.
		dc host "console setstage $MIN_QUEST 120" >/dev/null
		echo "radiant-min: the host's console set 120 (the settlement's defences built: set-up)"
		if wait_for 30 _rad_stage_once vm "$MIN_QUEST" 120; then pass "radiant-min: the host's 120 is set in the VM's game" "done $(_rad_done vm "$MIN_QUEST")"
		else fail "radiant-min: the host's 120 is set in the VM's game" "vm done $(_rad_done vm "$MIN_QUEST")"; fi
		if wait_for 20 _rad_stage_once host "$MIN_QUEST" 130; then pass "radiant-min: the host's attack starts in its game (130)" "done $(_rad_done host "$MIN_QUEST")"
		else warn "radiant-min: the host's attack starts in its game (130; a night attack waits for the night)" "done $(_rad_done host "$MIN_QUEST")"; fi
		wait_for "$MIN_HOLD_WAIT" _mr_own_attack   # stages stay done: the read after the wait shows the whole of it, if the game answers
		_mr_never vm "radiant-min: the VM never starts its own attack, near the settlement (its 130 and 300 not done)" 130 300

		# The VM away: still nothing of its own (no clean-up, no damage roll); the quest runs there until the host's game ends it.
		_mr_go vm "$MIN_AWAY_CELL"
		echo "radiant-min: the VM's player back at $MIN_AWAY_CELL by console coc (set-up)"
		wait_for "$MIN_HOLD_WAIT" _mr_own_attack
		_mr_never vm "radiant-min: the VM never starts its own attack or rolls its damage, away from the settlement" 130 300
		if ! _mr_ended host; then
			expect_eq "radiant-min: the VM's quest still runs away from the settlement (no clean-up of its own)" "$(_rad_run vm "$MIN_EDID")" running=true
		fi

		# Both at the settlement: the attack ends by the host's game.
		_mr_go host "$MIN_NEAR_CELL"; _mr_go vm "$MIN_NEAR_CELL"
		echo "radiant-min: both players at $MIN_NEAR_CELL by console coc (set-up)"
		before=$(_mr_values); _mr_300 "before the attack's end"
		if ! wait_for "$MIN_END_WAIT" _mr_ended host; then
			dc host "console setstage $MIN_QUEST 200" >/dev/null
			echo "radiant-min: the host's attack didn't end in $MIN_END_WAIT s; its console set 200 (the attackers dead: set-up)"
		fi
	fi
	end=200; _rad_stage_once host "$MIN_QUEST" 200 || end=310
	if wait_for 30 _rad_stage_once vm "$MIN_QUEST" "$end"; then pass "radiant-min: the host's end of the attack ($end) is set in the VM's game" "done $(_rad_done vm "$MIN_QUEST")"
	else fail "radiant-min: the host's end of the attack ($end) is set in the VM's game" "host $(_rad_done host "$MIN_QUEST") vm $(_rad_done vm "$MIN_QUEST")"; fi
	sleep 5; _mr_300 "after the host's $end" "$before"; echo "radiant-min: (300) the host done '$(_rad_done host "$MIN_QUEST")'"
	# On a failure the host's 290 (the settlers dead) is set in the VM's game too, where the quest's own fragment sets its 300 (judged
	# above with MIN_FAIL): only 130 is judged here then.
	if [ "$end" = 200 ]; then _mr_never vm "radiant-min: the VM's own attack never began, nor rolled its damage (its 130 and 300 not done)" 130 300
	else _mr_never vm "radiant-min: the VM's own attack never began (its 130 not done)" 130; fi
	if [ "$end" = 200 ] && ! _rad_stage_once host "$MIN_QUEST" 400; then
		dc host "console setstage $MIN_QUEST 400" >/dev/null
		echo "radiant-min: the host's console set 400 (the turn-in to the settlement: set-up)"
	fi
	if wait_for 30 _rr_stopped host "$MIN_EDID" && wait_for 30 _rr_stopped vm "$MIN_EDID"; then pass "radiant-min: the job is over in both games" "host done $(_rad_done host "$MIN_QUEST")"
	else fail "radiant-min: the job is over in both games" "host $(_rad_run host "$MIN_EDID") vm $(_rad_run vm "$MIN_EDID") (vm done $(_rad_done vm "$MIN_QUEST"))"; fi
	vmdone=$(_mr_but_attack vm); hostdone=$(_mr_but_attack host)
	if [ -z "$vmdone" ] || [ -z "$hostdone" ]; then rig_broken game "radiant-min: the same stages are done in both games, but 130 and 300 (each game's own) and 70 and 80 (each game's own arrival)" "no answer to 'stages $MIN_QUEST' (vm '$vmdone', host '$hostdone'): nothing read, so nothing shown"
	else expect_eq "radiant-min: the same stages are done in both games, but 130 and 300 (each game's own) and 70 and 80 (each game's own arrival)" "$vmdone" "$hostdone"; fi
	if wait_for 10 _mr_held_is vm 0; then pass "radiant-min: the VM lets its hold go as the job ends" "$(_rr_last vm "RadiantSync: $MIN_QUEST '[^']*': its own attack is let go")"
	else fail "radiant-min: the VM lets its hold go as the job ends" "attacksHeld=$(_mr_held vm)"; fi
	c1=$(_mr_cancelled vm)
	if ! [[ "$c1" =~ ^[0-9]+$ ]]; then rig_broken game "radiant-min: the VM cancels its hold's timer as the job ends (none is left to start its attack later)" "no number for attackTimersCancelled in its 'net status' ('$c1'; $c0 before)"
	else expect_ge "radiant-min: the VM cancels its hold's timer as the job ends (none is left to start its attack later)" "$(( c1 - c0 ))" 1 cancelled; fi
	# Its settlers given back with the hold: the Settlers group's size the one it had before the raise.
	local raised given
	raised=$(_rr_last vm "RadiantSync: $MIN_QUEST '[^']*': its settlers' deaths are the host's to count" | sed -nE 's/.*\(group [0-9]+ size ([0-9-]+) -> [0-9-]+\).*/\1/p')
	given=$(_rr_last vm "RadiantSync: $MIN_QUEST '[^']*': its settlers are its own to count again" | sed -nE 's/.*\(group [0-9]+ size [0-9-]+ -> ([0-9-]+).*/\1/p')
	if [ -z "$raised" ] || [ -z "$given" ]; then fail "radiant-min: the VM gives its settlers' count back as the job ends (the size it had)" "raised '$raised', given back '$given'; settlersHeld=$(_mr_settlers_held vm); $(_rr_last vm "RadiantSync: $MIN_QUEST '[^']*': its settlers")"
	else expect_eq "radiant-min: the VM gives its settlers' count back as the job ends (the size it had)" "$given" "$raised"; fi
	local sh; sh=$(_mr_settlers_held vm)
	if ! [[ "$sh" =~ ^[0-9]+$ ]]; then rig_broken game "radiant-min: the VM holds no settlers' count after the job" "no number for settlersHeld in its 'net status' ('$sh')"
	else expect_eq "radiant-min: the VM holds no settlers' count after the job" "$sh" 0; fi
}

# ---- encounter-fight: a fight at one trigger is one run for both; a checkpoint's objective shared by its run (opt-in: smoke.sh --only encounter-fight) --
# src/game/Encounters.h (the fight window, a run that hurt its player never stopped), EncounterRunBook.h and EncounterRuns.h (an encounter
# with an objective followed by the friend's game). Two parts, each at an RE trigger of Fallout4.esm, both players walking to it together
# (real movement: walkto.sh's pad) from a cell their consoles coc to (set-up, said so); for the fight both are first placed by console
# ENC_FIGHT_NEAR u from its trigger (the coc spot is 4970 u away and the walk from it sticks; said so), then walk in. When the run's
# actors stand more than ENC_RUN_FAR u from the other player, both are placed by console near them before the checks (_e_near_run; said so).
# Fight: ENC_FIGHT_TRIGGER, an RETriggerAssault south-east of Lexington. Each game rolls an encounter as its player crosses it; when
# neither rolled one in ENC_ROLL s, the host's console rolls ENC_FIGHT_QUEST there (`radiantstart`, the story manager's event, said so).
# Asserted from the logs since the walk: one run stands between the two games (the other game's "Encounters: our ... is stopped", or it
# rolled none; both standing is a FAIL, unless a "hurt our player" line explains it: a WARN); the other game's copies of the owner's
# actors are hostile to its player (dev `ghosts`); the owner's player shoots them (the host's gunfire through input.ps1, the VM's through
# its dev `click`; injected fire is fine for combat checks), the ones a shot did not kill are killed by the owner's console (set-up, said
# so), and every copy dies in the other game; a kill by a shot gives the other game a share of its XP (`net status` xpShared up; else the
# owner's xpReported or xpWithheld and its log say why: _e_xp_check).
# Checkpoint: ENC_CP_TRIGGER, an RETriggerCheckpoint at a military checkpoint north-west of Lexington; ENC_CP_QUEST (RECheckpoint09,
# raiders) is rolled by the host's console there when neither game rolled a checkpoint. Every checkpoint attack (RECheckpoint01 to 16)
# needs a defender standing there (Defender01, a reference of type RECheckpointDefender at the checkpoint's location, not optional), and
# only a faction's RECheckpoint*Won*_Init quest makes them, once that faction has won the main quest (RECheckpointMQWon*): a rig save
# from early in the story has none, so no checkpoint can roll or be started (2026-11-12: "couldn't fill an alias it needs"). So first
# both games' consoles start ENC_CP_INIT (the Minutemen's, RECheckpointMinWon01_Init) at the trigger (radiantstart, set-up, said so):
# each game makes its own defenders, as each world with that victory would (the Init quests aren't shared).
# Any other encounter either game rolled on the way in is stopped by its console first (set-up, said so): one encounter per spot, and a
# friend's camp 2309 u off once took it (Encounters.h, COLLIDE_RANGE).
# Asserted: one game shares its run ("EncounterRuns: our ... stands"), the other follows it ("... runs here: followed"), the same one checkpoint quest runs in both games; the owner's attackers
# killed by its console (set-up: the fight part tested the shooting), the defenders' victory stage ENC_CP_WIN is done in both games.
# A checkpoint whose attackers fall to the defenders first (on the rig, 2026-11-12: 16 s after the start) ends by itself: what is
# left of the mid-run checks is a WARN, and the other's followed run must end with the owner's (_e_cp_gone).
# Mid-run, before the kills: every attacker and defender alias filled in the owner's game is filled in the other's (with its copies:
# none of the owner's actors removed there); the other game saves (`console save`) and its aliases are filled again (the copies a save
# deletes are forced back in); it loads that save and follows the run again (its save's run of the quest, with none of its actors,
# makes way). Then both coc away (set-up, said so) to ENC_AWAY_CELL: once the owner's checkpoint quest stops, the other's stops too
# (a WARN if the owner's never stops in ENC_AWAY_WAIT s). Last, a fresh checkpoint run at the same trigger, followed, and the owner's
# link dropped (`net drop`; when the host owns it, the VM's own drop ends its session instead): the other's followed quest stops, and
# the VM rejoins. Quest stages are for good in a save: run it on throwaway saves (backup.sh before, restore after); the step also
# writes the save ENC_SAVE in the other game.
ENC_FIGHT_CELL=${ENC_FIGHT_CELL:-CorvegaAssemblyPlantExt03}; ENC_FIGHT_TRIGGER=${ENC_FIGHT_TRIGGER:-00187334}; ENC_FIGHT_QUEST=${ENC_FIGHT_QUEST:-0002672B}  # REAssaultSC02
ENC_CP_CELL=${ENC_CP_CELL:-POIMilitaryBB09}; ENC_CP_TRIGGER=${ENC_CP_TRIGGER:-00169C68}; ENC_CP_QUEST=${ENC_CP_QUEST:-000AF884}; ENC_CP_WIN=${ENC_CP_WIN:-400}
ENC_CP_INIT=${ENC_CP_INIT:-000AC513}   # RECheckpointMinWon01_Init: the checkpoint's Minutemen defenders (ENC_CP_QUEST's Defender01); "-" for none
ENC_ROLL=${ENC_ROLL:-30}; ENC_SETTLE=${ENC_SETTLE:-30}; ENC_SHOTS=${ENC_SHOTS:-3}
ENC_FIGHT_NEAR=${ENC_FIGHT_NEAR:-1500}   # how far from ENC_FIGHT_TRIGGER both are placed before the walk in (_e_place_near)
ENC_AWAY_CELL=${ENC_AWAY_CELL:-SanctuaryExt}; ENC_AWAY_WAIT=${ENC_AWAY_WAIT:-120}; ENC_SAVE=${ENC_SAVE:-f4mpencf}
_e_log() { win_read encf "$1"; }   # the game's log since the step's mark
# _e_runs <who>: the encounter quests that game shared actors of since the mark ("Spawns: sharing ... our encounter <quest>")
_e_runs() { sed -nE "s/.*Spawns: sharing [0-9A-F]{8} .* as [0-9A-F]{8}, our encounter ([0-9A-F]{8}) .*/\1/p" "$(_e_log "$1")" | sort -u; }
_e_stopped() { sed -nE "s/.*Encounters: our ([0-9A-F]{8}) '[^']*' is stopped.*/\1/p" "$(_e_log "$1")" | sort -u; }
_e_hurt() { grep -cE "Encounters: our [0-9A-F]{8} '[^']*' hurt our player" "$(_e_log "$1")"; }
_e_standing() { comm -23 <(_e_runs "$1") <(_e_stopped "$1"); }   # rolled and not stopped
_e_any_run() { [ -n "$(_e_runs host)$(_e_runs vm)" ]; }
# _e_actors <owner> <quest>: "local spawn" of each actor of the owner's run it shared
_e_actors() { sed -nE "s/.*Spawns: sharing ([0-9A-F]{8}) .* as ([0-9A-F]{8}), our encounter $2 .*/\1 \2/p" "$(_e_log "$1")" | sort -u; }
# _e_copy <who> <spawn>: that game's copy of the spawn ("Spawns: placed <local> @... as our copy of player N's <spawn>")
_e_copy() { sed -nE "s/.*Spawns: placed ([0-9A-F]{8}) .* as our copy of player [0-9]+'s $2$/\1/p" "$(_e_log "$1")" | tail -1; }
_e_ghost() { field "$(dc "$1" "ghosts $2")" "$3"; }   # _e_ghost <who> <ref> <hostile|dead>
_e_dead() { [ "$(_e_ghost "$1" "$2" dead)" = true ]; }
# _e_xpf <who> <counter>: a party counter of that game's `net status`: xpShared (friends' shares taken), xpReported (its own kill XP
# reported for shares), xpWithheld (kill XP kept back: a quest stage or a discovery in its window). Totals since the game started.
# (2026-11-12: this read xpCredit, a balance of shares not yet seen in the player's XP that the next poll spends, so it read 0
# seconds after any kill, share or no share.)
_e_xpf() { field "$(dc "$1" 'net status')" "$2" | cut -d/ -f1; }
_e_xp_more() { local v; v=$(_e_xpf "$1" "$2"); is_num "$v" && [ "$v" -gt "$3" ]; }   # _e_xp_more <who> <counter> <before>
# _e_xp_check: after the fight, the other game took a share of the owner's kills' XP (the owner holds kill XP 2.5 s before it reports
# it; up to 20 s). None: the owner's counters and log say why: reported and not taken (FAIL), kept back for a quest stage or a
# discovery in its window (WARN, by design: that XP may be the quest's, never shared; the stage's quest named), or neither (FAIL).
_e_xp_check() {
	local label="encounter-fight: the $other gets a share of the $owner's kills' XP" why
	if [ "$shot" = 0 ]; then warn "$label" "no actor died to a shot (console kills carry no XP): not checked"; return; fi
	if ! is_num "$xs0" || ! is_num "$xr0" || ! is_num "$xw0"; then
		fail "$label" "counters unreadable before the shots: the $other's xpShared '$xs0', the $owner's xpReported '$xr0' xpWithheld '$xw0'"; return
	fi
	if wait_for 20 _e_xp_more "$other" xpShared "$xs0"; then
		xs1=$(_e_xpf "$other" xpShared); xr1=$(_e_xpf "$owner" xpReported)
		pass "$label" "+$((xs1 - xs0)) xp in the $other's game (the $owner reported $((xr1 - xr0)) xp of its kills): $(grep -E "Party: \+[0-9]+ XP, our share of player" "$(_e_log "$other")" | tail -1 | cut -c1-160)"
		return
	fi
	xs1=$(_e_xpf "$other" xpShared); xr1=$(_e_xpf "$owner" xpReported); xw1=$(_e_xpf "$owner" xpWithheld)
	if is_num "$xr1" && [ "$xr1" -gt "$xr0" ]; then
		why=$(grep -E "Party: player [0-9]+'s kill XP .* gives us no share" "$(_e_log "$other")" | tail -1 | cut -c1-200)
		fail "$label" "the $owner reported $((xr1 - xr0)) xp of its kills; the $other took none (xpShared ${xs1:-?}): ${why:-no line of it in its log}"
	elif is_num "$xw1" && [ "$xw1" -gt "$xw0" ]; then
		why=$(grep -E "Party: our kill XP .* is not shared" "$(_e_log "$owner")" | tail -1 | cut -c1-260)
		warn "$label" "the $owner kept its kills' XP back ($((xw1 - xw0)) time(s): a quest stage or a discovery came in its window; quest XP is never shared, by design): ${why:-no line of it in its log}"
	else
		fail "$label" "the $owner neither reported nor kept back any kill XP (xpReported ${xr1:-?}, xpWithheld ${xw1:-?}, as before the shots): its kills gave no XP it took for a kill's"
	fi
}
# _e_up <who>: that game answers its dev channel, in the world or not. A game that crashed answers nothing: the checks below that
# read "not running" from it must not take that for a stop (2026-11-12: "the vm's followed ... stops once the link drops" passed
# on a VM whose game had crashed before the drop).
_e_up() { local t; for t in 1 2 3; do dc "$1" status | grep -q 'ingame=' && return 0; [ "$t" = 3 ] || sleep 3; done; return 1; }   # three tries: a tunnel hiccup is no crash
# _e_down <check>: a game no longer answers: a crash in Windows' Application log since the step began (a fast fail writes no
# crash log) or a game gone is a FAIL; one that runs and doesn't answer is a rig fault, as before.
_e_down() {
	local who ev pid said=
	local ps='"ok"; (Get-Process Fallout4 -ErrorAction SilentlyContinue | Select-Object -First 1).Id'   # "ok" first: an empty answer is a dropped ssh, not "no game"
	for who in host vm; do
		_e_up "$who" && continue
		said=1
		if [ "$who" = host ]; then
			ev=$(crash_event_lines "$(crash_events_host "${C_T0:-0}")")
			pid=$(powershell -NoProfile -Command "$ps" 2>/dev/null | tr -d '\r')
		else
			ev=$(crash_event_lines "$(crash_events_vm "${C_T0:-0}")")
			pid=$(vmssh "$ps")
		fi
		if [ -n "$ev" ]; then fail "encounter-fight: $1" "the $who's game crashed: $(tail -1 <<<"$ev" | cut -d' ' -f2-) (Windows' Application log); its log ends: $(tail -1 "$(_e_log "$who")" | cut -c1-200)"
		elif [ "$(head -1 <<<"$pid")" != ok ]; then rig_fault state "encounter-fight: $1" "the $who's game gives no answer, and its process list could not be read (ssh)"
		elif [ -z "$(sed 1d <<<"$pid")" ]; then fail "encounter-fight: $1" "the $who's game is gone (no Fallout4 process), with no crash in Windows' Application log"
		else rig_fault state "encounter-fight: $1" "the $who's game runs (pid $(sed 1d <<<"$pid" | head -1)) but its dev channel gives no answer"; fi
	done
	# Both answered on this second look (a long hiccup): the check that called this gets its verdict here, not none.
	[ -n "$said" ] || rig_fault state "encounter-fight: $1" "a game gave no answer, then both answered again"
}
_e_shoot() {  # _e_shoot <who> <ref>: that player turns to the actor and fires a burst
	dc "$1" "turnto $2 300 12" >/dev/null; sleep 0.4
	if [ "$1" = host ]; then powershell -NoProfile -ExecutionPolicy Bypass -File "$INPUT_PS1" -Click -HoldMs 700 >/dev/null 2>&1; else dc vm "click 120" >/dev/null; sleep 0.3; dc vm "click 120" >/dev/null; fi
	sleep 1.5
}
_e_walk_both() {  # _e_walk_both <trigger>: both players walk to it together (real movement); RIG on a walk that never arrives
	local hp vp
	bash "$RIG/walkto.sh" host "$1" 1 150 90 > "$A_OUT/encf-walk-host.log" 2>&1 & hp=$!
	bash "$RIG/walkto.sh" vm "$1" 1 150 90 > "$A_OUT/encf-walk-vm.log" 2>&1 & vp=$!
	wait $hp; wait $vp; pads_off
	grep -q ': arrived ' "$A_OUT/encf-walk-host.log" && grep -q ': arrived ' "$A_OUT/encf-walk-vm.log" && return 0
	rig_fault walk "encounter-fight: both players walk to the trigger $1" "host: $(tail -1 "$A_OUT/encf-walk-host.log"); vm: $(tail -1 "$A_OUT/encf-walk-vm.log")"; return 1
}
# _e_place_near <trigger> <units>: both players placed by console that many units from the trigger, on the line from it back toward
# where the coc put the host, facing it (set-up: placement to reach the scene, said so; the walk in after it is real movement). The
# coc spot of ENC_FIGHT_CELL is 4970 u from ENC_FIGHT_TRIGGER and both walks stuck there at once, the same spot in two runs
# (2026-11-11: "stuck to 00187334 dist=4968 ... at=(-28873,29574,3496)"). The height: the higher of the trigger's and the coc spot's
# (_qa_place_near puts them 400 u above it; god mode takes the fall).
_e_place_near() {
	local f tx ty tz p px py pz dx dy l
	f=$(dc host "refinfo $1"); tx=$(field "$f" x); ty=$(field "$f" y); tz=$(field "$f" z)
	p=$(dc host pos); px=$(field "$p" x); py=$(field "$p" y); pz=$(field "$p" z)
	if ! is_num "$tx" || ! is_num "$ty" || ! is_num "$tz" || ! is_num "$px" || ! is_num "$py" || ! is_num "$pz"; then
		fail "encounter-fight: set-up: where the trigger $1 and the host are" "refinfo $1: '$(cut -c1-160 <<<"$f")'; pos: '$(cut -c1-120 <<<"$p")'"; return 1
	fi
	l=$(awk -v a="$tx" -v b="$ty" -v c="$px" -v d="$py" 'BEGIN{printf "%d", sqrt((c-a)^2+(d-b)^2)}')
	if [ "$l" -le "$2" ]; then echo "encounter-fight: the coc spot is $l u from $1, within $2: not placed"; return 0; fi
	dx=$(awk -v a="$tx" -v c="$px" -v l="$l" -v n="$2" 'BEGIN{printf "%d", (c-a) / l * n}')
	dy=$(awk -v b="$ty" -v d="$py" -v l="$l" -v n="$2" 'BEGIN{printf "%d", (d-b) / l * n}')
	tz=$(awk -v a="$tz" -v b="$pz" 'BEGIN{printf "%d", (a > b ? a : b)}')
	echo "encounter-fight: both players placed by console $2 u from the trigger $1 (the coc spot is $l u away and the walk from it sticks; set-up), then walk in (real movement)"
	_qa_place_near "$tx" "$ty" "$tz" "$dx" "$dy"
}
# _e_near_run <owner> <quest>: the run's actors may stand far from the trigger the players walked to (2026-11-11: RESceneCT07's two
# stood 7.5k u off, past SpawnBook's PLACE_RANGE of 6000 u, so the VM's game placed no copy and every check after read nothing). When the
# other game's player is more than ENC_RUN_FAR u from the owner's first actor, both players are placed by console ENC_RUN_NEAR u from it
# (set-up, said so). Then every actor's copy is waited for (ENC_SETTLE s), printed either way: the checks after read the copies.
ENC_RUN_FAR=${ENC_RUN_FAR:-4500}; ENC_RUN_NEAR=${ENC_RUN_NEAR:-1200}
# _e_all_copied <other> <owner> <quest>: the other game has a copy of each of the owner's actors of the run still alive there (a
# dead spawn's copy is never placed: a corpse its run made is left out)
_e_all_copied() { local local spawn; while read -r local spawn; do [ -n "$spawn" ] || continue; [ -n "$(_e_copy "$1" "$spawn")" ] || _e_dead "$2" "$local" || return 1; done < <(_e_actors "$2" "$3"); return 0; }
_e_near_run() {
	local owner=$1 q=$2 other a f ax ay az p px py pz l dx dy
	other=$(other_game "$owner")
	a=$(_e_actors "$owner" "$q" | head -1 | awk '{print $1}')
	[ -n "$a" ] || return 0   # no actor shared: the caller's check says so
	f=$(dc "$owner" "refinfo $a"); ax=$(field "$f" x); ay=$(field "$f" y); az=$(field "$f" z)
	p=$(dc "$other" pos); px=$(field "$p" x); py=$(field "$p" y); pz=$(field "$p" z)
	if ! is_num "$ax" || ! is_num "$ay" || ! is_num "$az" || ! is_num "$px" || ! is_num "$py"; then
		echo "encounter-fight: where the $owner's $a and the $other's player are could not be read (refinfo '$(cut -c1-120 <<<"$f")', pos '$(cut -c1-100 <<<"$p")'): not placed"
	else
		l=$(awk -v a="$ax" -v b="$ay" -v c="$px" -v d="$py" 'BEGIN{printf "%d", sqrt((c-a)^2+(d-b)^2)}')
		if [ "$l" -gt "$ENC_RUN_FAR" ]; then
			dx=$(awk -v a="$ax" -v c="$px" -v l="$l" -v n="$ENC_RUN_NEAR" 'BEGIN{printf "%d", (c-a) / l * n}')
			dy=$(awk -v b="$ay" -v d="$py" -v l="$l" -v n="$ENC_RUN_NEAR" 'BEGIN{printf "%d", (d-b) / l * n}')
			echo "encounter-fight: the $owner's $q stands $l u from the $other's player (past $ENC_RUN_FAR; a copy is placed only within 6000 u): both players placed by console $ENC_RUN_NEAR u from its $a (set-up)"
			_qa_place_near "$ax" "$ay" "$az" "$dx" "$dy"
		else echo "encounter-fight: the $owner's $q stands $l u from the $other's player: not placed"; fi
	fi
	if wait_for "$ENC_SETTLE" _e_all_copied "$other" "$owner" "$q"; then echo "encounter-fight: the $other's game has a copy of each of the $owner's actors of $q"
	else echo "encounter-fight: the $other's game has no copy of some of the $owner's actors of $q after $ENC_SETTLE s: $(dc "$other" 'net status' | grep -oE 'spawns: [^;|]*' | cut -c1-200)$(dc "$other" 'net status' | grep -oE ' notShared=[0-9]+ unplaced=[0-9/]+')"; fi
	return 0
}
_e_arm() { dc "$1" "console player.additem 00004822 1" >/dev/null; dc "$1" "console player.additem 0001F276 300" >/dev/null; dc "$1" "console player.equipitem 00004822" >/dev/null; dc "$1" "draw on" >/dev/null; }
# _e_cp_running <who>: the checkpoint attacks (RECheckpoint01_... to RECheckpoint16_...) running in that game. Dev `quests edid:`
# prints "<id> <editor id> '<name>' ... running=<bool>", 25 at most a call: RECheckpoint0 and RECheckpoint1 match 9 and 7.
# (2026-11-12: this read `quests running:6`, which prints a quest's name, 'RE Checkpoint: Enemy Raiders', not its editor
# id: no checkpoint was ever seen, and the one the host's console started ran 16 s unseen.)
_e_cp_running() { local p; for p in RECheckpoint0 RECheckpoint1; do dc "$1" "quests edid:$p" | tr ';' '\n' | grep -E ' running=true( |$)' | grep -oE "^ ?[0-9A-F]{8} RECheckpoint[0-9]+_[A-Za-z0-9_]*" | awk '{print $1}'; done | sort -u; }
_e_cp_any() { [ -n "$(_e_cp_running host)$(_e_cp_running vm)" ]; }
_e_done() { dc "$1" "stages $2" | sed -nE 's/.* done\([0-9]+\)=([0-9,]+|none).*/\1/p'; }
_e_has() { case ",$(_e_done "$1" "$2")," in *",$3,"*) return 0;; esac; return 1; }
_e_followed() { grep -qE "EncounterRuns: player [0-9]+'s $2 '[^']*' runs here: followed" "$(_e_log "$1")"; }
_e_followed_n() { grep -cE "EncounterRuns: player [0-9]+'s $2 '[^']*' runs here: followed" "$(_e_log "$1")"; }
_e_followed_more() { [ "$(_e_followed_n "$1" "$2")" -gt "$3" ]; }   # _e_followed_more <who> <quest> <count before>
# _e_filled <who> <quest>: the ids of the quest's attacker and defender aliases filled in that game (dev `aliases`), one a line
_e_filled() { dc "$1" "aliases $2" | tr '|' '\n' | grep -E "^ Ref [0-9]+ (Attacker|Defender)[A-Za-z0-9_]* " | grep -vE 'here=0*( |$)' | awk '{print $2}' | sort -u; }
# _e_filled_like <who> <quest> <ids>: every alias in ids is filled in that game
_e_filled_like() { local id have; have=" $(_e_filled "$1" "$2" | tr '\n' ' ') "; for id in $3; do case "$have" in *" $id "*) ;; *) return 1;; esac; done; return 0; }
_e_cp_runs() { case " $(_e_cp_running "$1" | tr '\n' ' ') " in *" $2 "*) return 0;; esac; return 1; }
_e_cp_stopped() { _e_up "$1" && ! _e_cp_runs "$1" "$2"; }   # a game that gives no answer has stopped nothing (_e_up)
_e_joined() { ns vm; [ "$(nsf vm status)" = joined ]; }
# _e_cp_share: a checkpoint is rolled at ENC_CP_TRIGGER (by the host's console if neither game rolls one) and one game shares it by its
# run; sets owner, other and cp. Only lines after the mark count.
_e_cp_share() {
	local a who e
	if ! wait_for "$ENC_ROLL" _e_cp_any; then
		# Another encounter either game rolled on the way in (2026-11-12: the VM's camp RECamprj03, its actors 2309 u from the
		# console's checkpoint raiders) holds the spot: a run started within Encounters' COLLIDE_RANGE of a friend's that was first
		# gives way to it, so the console's checkpoint was stopped the moment it stood. Each such run is stopped by its own
		# game's console first (set-up, said so); its actors still standing are then plain spawns, no rival.
		for who in host vm; do
			for e in $(_e_standing "$who"); do
				dc "$who" "console stopquest $e" >/dev/null
				echo "encounter-fight: the $who's encounter $e, rolled on the way in, stopped by its console so the checkpoint has the spot (set-up)"
			done
		done
		sleep 5
		if [ "$ENC_CP_INIT" != - ]; then
			for who in host vm; do
				echo "encounter-fight: the $who's console starts $ENC_CP_INIT at $ENC_CP_TRIGGER, the checkpoint's defenders a faction's victory would make (radiantstart, set-up): $(dc "$who" "radiantstart $ENC_CP_INIT $ENC_CP_TRIGGER" | cut -c1-220)"
			done
			sleep 5
		fi
		echo "encounter-fight: neither game rolled a checkpoint at $ENC_CP_TRIGGER; the host's console rolls $ENC_CP_QUEST there (radiantstart, set-up): $(dc host "radiantstart $ENC_CP_QUEST $ENC_CP_TRIGGER" | cut -c1-220)"
		if ! wait_for "$ENC_ROLL" _e_cp_any; then
			fail "encounter-fight: a checkpoint runs at the trigger$1" "none in $ENC_ROLL s; host: $(grep -E "Encounters: our [0-9A-F]{8} 'RECheckpoint[0-9][^']*' is stopped" "$(_e_log host)" | tail -1 | cut -c1-200)"
			return 1
		fi
	fi
	owner=""
	for a in $(seq 1 "$((ENC_SETTLE * 2))"); do
		for who in host vm; do grep -qE "EncounterRuns: our [0-9A-F]{8} 'RECheckpoint[0-9][^']*' stands: shared" "$(_e_log "$who")" && { owner=$who; break 2; }; done
		sleep 1
	done
	if [ -z "$owner" ]; then fail "encounter-fight: a game shares its checkpoint by its run$1" "no 'EncounterRuns: our ... stands' in either log"; return 1; fi
	cp=$(sed -nE "s/.*EncounterRuns: our ([0-9A-F]{8}) 'RECheckpoint[0-9][^']*' stands: shared.*/\1/p" "$(_e_log "$owner")" | tail -1); other=$(other_game "$owner")
	pass "encounter-fight: the $owner shares its checkpoint $cp by its run$1" "ok"
	if wait_for 60 _e_followed "$other" "$cp"; then pass "encounter-fight: the $other follows the $owner's $cp$1" "ok"; return 0; fi
	fail "encounter-fight: the $other follows the $owner's $cp$1" "$(grep -E "EncounterRuns: .*$cp" "$(_e_log "$other")" | tail -2 | cut -c1-200)"; return 1
}
# _e_cp_over <who> <quest>: that game's own run of the quest is over (its quest no longer runs there)
_e_cp_over() { grep -qE "Encounters: our $2 '[^']*' is over" "$(_e_log "$1")"; }
# _e_cp_gone <what>: the owner's checkpoint ended by itself before <what> could be checked. Its attackers fell to the defenders
# both games' consoles made (2026-11-12, rig: the raiders of 000AF884 were dead 16 s after the start, 5 s after the run stood; one
# copy died to the VM's own defenders, its death killing the host's actor): a WARN for what is left of the mid-run checks, and the
# other's followed run must end with the owner's.
_e_cp_gone() {
	warn "encounter-fight: $1" "the $owner's $cp ended by itself first ($(grep -E "Encounters: our $cp '[^']*' is over" "$(_e_log "$owner")" | tail -1 | cut -c2-13); its attackers fell to the defenders): not checked"
	if wait_for 30 _e_cp_stopped "$other" "$cp"; then pass "encounter-fight: the $other's followed $cp ends with the $owner's run" "$(grep -E "EncounterRuns: .*$cp .*(ended in their game|is over)" "$(_e_log "$other")" | tail -1 | cut -c1-160)"
	else fail "encounter-fight: the $other's followed $cp ends with the $owner's run" "still running 30 s after the $owner's ended: $(grep -E "EncounterRuns: .*$cp" "$(_e_log "$other")" | tail -1 | cut -c1-160)"; fi
}
# _e_own_shared_n <who> <quest>: how many times that game shared a run of the quest as its own ("EncounterRuns: our ... stands: shared")
_e_own_shared_n() { grep -cE "EncounterRuns: our $2 '[^']*' stands: shared" "$(_e_log "$1")"; }
# _e_cp_gone_load <own shared before> <owner's follows before>: the owner's checkpoint ended by itself around the other's load of its
# save of the run (rig run 11, 2026-11-12: the host's ended 1.3 s into the VM's load; the confirm run: 6 s before it). The run the other's save holds is a save's of
# a run it followed, with nobody's run left to follow: hollow (none of its actors stands: the copies were never saved; what is left
# is its own reinforcements, still disabled). It must never be shared as the other's own run (that run 11 shared it, and the
# owner's game followed it, starting its finished checkpoint over), the owner's game must follow nothing of the other's, and it is
# stopped as hollow once it has run EncounterRunBook.h's HOLLOW_WAIT (5 min) with nobody's run of it heard. Its victory stage can
# never come (the owner's last stages went before the load): cpStale skips that check.
ENC_HOLLOW_WAIT=${ENC_HOLLOW_WAIT:-330}   # s: HOLLOW_WAIT (300) from the first poll after the load, and a margin
_e_cp_gone_load() {
	cpStale=1
	warn "encounter-fight: after loading its save the $other follows the $owner's $cp again" "the $owner's $cp ended by itself around the $other's load ($(grep -E "Encounters: our $cp '[^']*' is over" "$(_e_log "$owner")" | tail -1 | cut -c2-13)): nothing to follow, not checked"
	local n=0
	for n in $(seq 1 20); do [ "$(_e_own_shared_n "$other" "$cp")" -gt "$1" ] && break; sleep 1; done   # LookAtOurs would share it within STOP_WINDOW of the load
	if [ "$(_e_own_shared_n "$other" "$cp")" -gt "$1" ]; then fail "encounter-fight: the $other's loaded save's $cp is never shared as its own run" "$(grep -E "EncounterRuns: our $cp '[^']*' stands: shared" "$(_e_log "$other")" | tail -1 | cut -c1-200)"
	else pass "encounter-fight: the $other's loaded save's $cp is never shared as its own run" "none shared in 20 s, $(grep -E "EncounterRuns: our $cp .* none of its actors" "$(_e_log "$other")" | wc -l) stopped as hollow so far"; fi
	if [ "$(_e_followed_n "$owner" "$cp")" -gt "$2" ]; then fail "encounter-fight: the $owner's game does not start its finished $cp again for the $other's" "$(grep -E "EncounterRuns: player [0-9]+'s $cp '[^']*' runs here: followed" "$(_e_log "$owner")" | tail -1 | cut -c1-200)"
	else pass "encounter-fight: the $owner's game does not start its finished $cp again for the $other's" "ok"; fi
	if wait_for "$ENC_HOLLOW_WAIT" _e_cp_stopped "$other" "$cp"; then pass "encounter-fight: the $other's loaded save's $cp is stopped as hollow" "$(grep -E "EncounterRuns: our $cp .* none of its actors" "$(_e_log "$other")" | tail -1 | cut -c1-200)"
	else fail "encounter-fight: the $other's loaded save's $cp is stopped as hollow" "still running $ENC_HOLLOW_WAIT s after the load: $(grep -E "EncounterRuns: .*$cp" "$(_e_log "$other")" | tail -1 | cut -c1-200)"; fi
}
# _e_cp_midrun: the checks while the checkpoint runs (one quest in both games, the aliases, a save and a load in the other game),
# then the owner's attackers killed by its console. Each part first looks whether the owner's run already ended by itself
# (_e_cp_gone says so once and the rest is skipped).
_e_cp_midrun() {
	local filled followed0 ownShared0 ownerFollowed0
	cpH=$(_e_cp_running host | tr '\n' ' '); cpV=$(_e_cp_running vm | tr '\n' ' ')
	if { [ "${cpH% }" != "$cp" ] || [ "${cpV% }" != "$cp" ]; } && wait_for 5 _e_cp_over "$owner" "$cp"; then
		# Ended by itself: still no checkpoint but the owner's may run in either game (a second one is the mod's, a FAIL).
		local extra; extra=$(printf '%s\n' $cpH $cpV | grep -vx "$cp" | sort -u | tr '\n' ' ')
		[ -z "${extra// }" ] || fail "encounter-fight: no checkpoint quest but the $owner's $cp runs in either game" "also running: ${extra% } (host [${cpH% }] vm [${cpV% }])"
		_e_cp_gone "the same one checkpoint quest runs in both games, and the mid-run checks"; return
	fi
	expect_eq "encounter-fight: one checkpoint quest runs in the host's game" "${cpH% }" "$cp"
	expect_eq "encounter-fight: the same one checkpoint quest runs in the VM's game" "${cpV% }" "$cp"
	# Mid-run: the other's aliases hold the owner's actors' copies; a save there keeps them; a load of it follows the run again.
	sleep 5
	if _e_cp_over "$owner" "$cp"; then _e_cp_gone "the mid-run checks (aliases, save, load)"; return; fi
	filled=$(_e_filled "$owner" "$cp" | tr '\n' ' ')
	if [ -z "${filled// }" ]; then warn "encounter-fight: the $other's $cp aliases hold the $owner's actors" "the $owner's own has no attacker or defender alias filled: not checked"
	elif wait_for 20 _e_filled_like "$other" "$cp" "$filled"; then pass "encounter-fight: the $other's $cp aliases hold the $owner's actors" "aliases $filled; $(field "$(dc "$other" 'net status')" followRemoved) of its own removed"
	elif _e_cp_over "$owner" "$cp"; then _e_cp_gone "the $other's $cp aliases hold the $owner's actors, and the save and load"; return
	else fail "encounter-fight: the $other's $cp aliases hold the $owner's actors" "the $owner fills [$filled], the $other [$(_e_filled "$other" "$cp" | tr '\n' ' ')]"; fi
	if _e_cp_over "$owner" "$cp"; then _e_cp_gone "the save and load mid-run"; return; fi
	dc "$other" "console save $ENC_SAVE" >/dev/null; sleep 10
	echo "encounter-fight: the $other saved ($ENC_SAVE) mid-run"
	if [ -n "${filled// }" ]; then
		if wait_for 40 _e_filled_like "$other" "$cp" "$filled"; then pass "encounter-fight: after a save the $other's $cp aliases hold the copies again" "$(grep -cE "EncounterRuns: .* forced (again )?onto our copy" "$(_e_log "$other")") forced again"
		elif _e_cp_over "$owner" "$cp"; then _e_cp_gone "after a save the $other's $cp aliases hold the copies again, and the load"; return
		else fail "encounter-fight: after a save the $other's $cp aliases hold the copies again" "[$(_e_filled "$other" "$cp" | tr '\n' ' ')] of [$filled]; $(grep -E "EncounterRuns: .*(forced|could not be forced)" "$(_e_log "$other")" | tail -1 | cut -c1-200)"; fi
	fi
	if _e_cp_over "$owner" "$cp"; then _e_cp_gone "the load mid-run"; return; fi
	followed0=$(_e_followed_n "$other" "$cp"); ownShared0=$(_e_own_shared_n "$other" "$cp"); ownerFollowed0=$(_e_followed_n "$owner" "$cp")
	dc "$other" "console load $ENC_SAVE" >/dev/null
	[ "$other" = host ] && { sleep 8; host "key 0x0D 100" >/dev/null; }
	sleep 20; wait_ingame "$other" 30
	echo "encounter-fight: the $other loaded $ENC_SAVE mid-run"
	if wait_for 90 _e_followed_more "$other" "$cp" "$followed0"; then pass "encounter-fight: after loading its save the $other follows the $owner's $cp again" "$(grep -cE "EncounterRuns: our $cp .* none of its actors" "$(_e_log "$other")") hollow run(s) of it stopped"
	elif _e_cp_over "$owner" "$cp"; then _e_cp_gone_load "$ownShared0" "$ownerFollowed0"; return
	else fail "encounter-fight: after loading its save the $other follows the $owner's $cp again" "$(grep -E "EncounterRuns: .*$cp" "$(_e_log "$other")" | tail -2 | cut -c1-200)"; fi
	if [ -n "${filled// }" ] && ! wait_for 30 _e_filled_like "$other" "$cp" "$filled"; then
		warn "encounter-fight: after the load the $other's $cp aliases hold the $owner's actors" "[$(_e_filled "$other" "$cp" | tr '\n' ' ')] of [$filled] (actors killed meanwhile leave theirs)"
	fi
	for a in $(dc "$owner" "aliases $cp" | tr '|' '\n' | grep -E "^ Ref [0-9]+ Attacker[0-9]+ " | grep -o 'here=[0-9A-F]*' | sed 's/here=//' | grep -v '^0*$'); do
		dc "$owner" "console $a.kill" >/dev/null
	done
	echo "encounter-fight: the $owner's checkpoint attackers killed by its console (set-up)"
}
step_encounter_fight() {
	section "encounter-fight (a fight at one trigger is one run; a checkpoint followed by its run; console coc, placement, roll and kills as set-up)"
	# Neither game may crash in it: a crash log, or a crash in Windows' Application log (a fast fail writes no crash log; rig run 9,
	# 2026-11-12: the VM's game gone mid-step was read as a rig fault). The run's crash baseline is put back after, as trails does.
	local h0="$C_HOST0" v0="$C_VM0" t0="$C_T0"
	crash_snapshot
	_e_fight
	crash_verdict "encounter-fight"
	C_HOST0="$h0"; C_VM0="$v0"; C_T0="$t0"
}
_e_fight() {
	local owner other q standH standV a who local spawn copy n shot hostile ownHostile calm dead xs0 xr0 xw0 xs1 xr1 xw1 cpH cpV cp cpStale=""
	win_mark encf host; win_mark encf vm
	enter_both "$ENC_FIGHT_CELL" 16
	_e_arm host; _e_arm vm
	_e_place_near "$ENC_FIGHT_TRIGGER" "$ENC_FIGHT_NEAR" || return
	_e_walk_both "$ENC_FIGHT_TRIGGER" || return
	if ! wait_for "$ENC_ROLL" _e_any_run; then
		echo "encounter-fight: neither game rolled an encounter at $ENC_FIGHT_TRIGGER; the host's console rolls $ENC_FIGHT_QUEST there (radiantstart, set-up): $(dc host "radiantstart $ENC_FIGHT_QUEST $ENC_FIGHT_TRIGGER" | cut -c1-220)"
		if ! wait_for "$ENC_ROLL" _e_any_run; then fail "encounter-fight: an encounter runs at the trigger" "none rolled, and the console's $ENC_FIGHT_QUEST shared no actor in $ENC_ROLL s"; return; fi
	fi
	sleep "$ENC_SETTLE"
	standH=$(_e_standing host | tr '\n' ' '); standV=$(_e_standing vm | tr '\n' ' ')
	echo "encounter-fight: host rolled [$(_e_runs host | tr '\n' ' ')] stopped [$(_e_stopped host | tr '\n' ' ')]; vm rolled [$(_e_runs vm | tr '\n' ' ')] stopped [$(_e_stopped vm | tr '\n' ' ')]"
	if [ -n "${standH// }" ] && [ -n "${standV// }" ]; then
		if [ "$(( $(_e_hurt host) + $(_e_hurt vm) ))" -gt 0 ]; then warn "encounter-fight: one run stands between the two games" "both stand (host $standH, vm $standV), a run's actors hurt its player first: never stopped by design"
		else fail "encounter-fight: one run stands between the two games" "both stand: host $standH, vm $standV"; fi
	elif [ -z "${standH// }${standV// }" ]; then fail "encounter-fight: one run stands between the two games" "none stands: both stopped?"; return
	else pass "encounter-fight: one run stands between the two games" "host [$standH] vm [$standV]"; fi
	if [ -n "${standH// }" ]; then owner=host; q=${standH%% *}; else owner=vm; q=${standV%% *}; fi
	other=$(other_game "$owner")
	_e_near_run "$owner" "$q" || return
	# An actor of the owner's run already dead before any shot is no fight's: a corpse its run made (2026-11-12: the VM's
	# RESceneCC_DN101, three dead raiders its own script kills as they load: no copy placed, nothing to shoot, no XP; the mod
	# now offers an encounter's actor only after Encounters::SHARE_SETTLE, so a corpse is seldom shared at all). Those are
	# said and left out of the fight checks below; a run of corpses alone is a WARN, not checked. A copy the other game placed of
	# one of them must be dead too (checked after the fight part): a copy standing alive for a dead actor is the mod's, a FAIL.
	local corpses=0 alive="$A_OUT/encf-alive.txt" predead="$A_OUT/encf-predead.txt" withCopy=0 deadCopies=0
	: > "$alive"; : > "$predead"
	while read -r local spawn; do
		[ -n "$spawn" ] || continue
		if _e_dead "$owner" "$local"; then corpses=$((corpses + 1)); echo "$local $spawn" >> "$predead"; else echo "$local $spawn" >> "$alive"; fi
	done < <(_e_actors "$owner" "$q")
	if [ "$corpses" -gt 0 ]; then echo "encounter-fight: $corpses of the $owner's shared actors of $q were dead before any shot (corpses its run made): left out of the checks"; fi
	if [ "$corpses" = 0 ] && [ ! -s "$alive" ]; then fail "encounter-fight: the owner ($owner) shares actors of $q" "none in its log"; return; fi
	if [ ! -s "$alive" ]; then
		warn "encounter-fight: the $other's copies of the $owner's $q fight and die as the $owner's actors do" "every one of the $owner's $corpses shared actor(s) of $q was dead before any shot (a run of corpses): not checked"
	else
	# The copies are as hostile as the owner's actors: a calm run (2026-11-12: RESceneJSDN077, a scene, rolled at the assault
	# trigger and shared "(calm)") has none hostile in its own game either, and its copies are calm by design (Spawns.h).
	xs0=$(_e_xpf "$other" xpShared); xr0=$(_e_xpf "$owner" xpReported); xw0=$(_e_xpf "$owner" xpWithheld); n=0; hostile=0; ownHostile=0; shot=0
	while read -r local spawn; do
		copy=$(_e_copy "$other" "$spawn"); n=$((n + 1))
		[ "$(_e_ghost "$owner" "$local" hostile)" = true ] && ownHostile=$((ownHostile + 1))
		[ -n "$copy" ] && [ "$(_e_ghost "$other" "$copy" hostile)" = true ] && hostile=$((hostile + 1))
	done < "$alive"
	# A calm run (none of the owner's actors hostile to its own player) is not checked when no copy is hostile either: the two
	# games agree. Copies hostile to a run calm in its own game is the mod's and a FAIL. "(calm)" is the log's word for an
	# actor neither fighting nor hostile when shared: one shared fighting (with another faction) may be calm by now, so a
	# calm run without it is no fault while the copies agree; the count is said.
	calm=$(grep -cE "Spawns: sharing .* our encounter $q .*\(calm\)" "$(_e_log "$owner")")
	if [ "$ownHostile" = 0 ] && [ "$hostile" = 0 ]; then
		warn "encounter-fight: the $other's copies of the $owner's $q are hostile to its player" "the $owner's run is calm (none of its $n actors hostile to its own player; $calm shared '(calm)'), and so are the $other's copies: not checked"
	elif [ "$ownHostile" = 0 ]; then
		fail "encounter-fight: the $other's copies of the $owner's $q are as hostile to its player as the $owner's actors are to its own" "the $owner's $n actor(s) hostile to none, $calm shared '(calm)'; the $other's copies hostile: $hostile"
	else expect_eq "encounter-fight: the $other's copies of the $owner's $q are hostile to its player, as the $owner's actors are to its own" "$hostile" "$ownHostile"; fi
	for a in $(seq 1 "$ENC_SHOTS"); do
		while read -r local spawn; do _e_dead "$owner" "$local" || _e_shoot "$owner" "$local"; done < "$alive"
	done
	while read -r local spawn; do _e_dead "$owner" "$local" && shot=$((shot + 1)); done < "$alive"
	while read -r local spawn; do _e_dead "$owner" "$local" || dc "$owner" "console $local.kill" >/dev/null; done < "$alive"
	echo "encounter-fight: $shot of $n killed by the $owner's shots, the rest by its console (set-up)"
	sleep 10; dead=0
	while read -r local spawn; do copy=$(_e_copy "$other" "$spawn"); [ -n "$copy" ] && _e_dead "$other" "$copy" && dead=$((dead + 1)); done < "$alive"
	expect_eq "encounter-fight: every copy dies in the $other's game as the $owner's actors die" "$dead" "$n"
	_e_xp_check
	fi
	while read -r local spawn; do
		copy=$(_e_copy "$other" "$spawn"); [ -n "$copy" ] || continue
		withCopy=$((withCopy + 1)); _e_dead "$other" "$copy" && deadCopies=$((deadCopies + 1))
	done < "$predead"
	if [ "$withCopy" -gt 0 ]; then expect_eq "encounter-fight: the $other's copies of the $owner's actors dead before any shot are dead too" "$deadCopies" "$withCopy"; fi

	# The checkpoint: shared by its run.
	win_mark encf host; win_mark encf vm
	enter_both "$ENC_CP_CELL" 16
	_e_walk_both "$ENC_CP_TRIGGER" || return
	_e_cp_share "" || return
	_e_cp_midrun
	if wait_for 60 _e_has "$owner" "$cp" "$ENC_CP_WIN"; then pass "encounter-fight: the defenders' victory ($cp stage $ENC_CP_WIN) in the $owner's game" "done $(_e_done "$owner" "$cp")"
	else fail "encounter-fight: the defenders' victory ($cp stage $ENC_CP_WIN) in the $owner's game" "done $(_e_done "$owner" "$cp")"; _e_save_rm "$other"; return; fi
	if [ -n "$cpStale" ]; then warn "encounter-fight: the defenders' victory ($cp stage $ENC_CP_WIN) in the $other's game too" "the $other loaded its save of the run around the end of the $owner's: its last stages came before the load, not checked (done $(_e_done "$other" "$cp"))"
	elif wait_for 30 _e_has "$other" "$cp" "$ENC_CP_WIN"; then pass "encounter-fight: the defenders' victory ($cp stage $ENC_CP_WIN) in the $other's game too" "done $(_e_done "$other" "$cp")"
	else fail "encounter-fight: the defenders' victory ($cp stage $ENC_CP_WIN) in the $other's game too" "done $(_e_done "$other" "$cp")"; fi

	# Both leave the area (console coc, set-up): the owner's checkpoint ends, and the other's with it (its last stages, then its end).
	enter_both "$ENC_AWAY_CELL" 16
	echo "encounter-fight: both left for $ENC_AWAY_CELL by console (set-up)"
	if ! wait_for "$ENC_AWAY_WAIT" _e_cp_stopped "$owner" "$cp"; then
		warn "encounter-fight: leaving ends the checkpoint in both games" "the $owner's $cp still runs $ENC_AWAY_WAIT s after both left (the $other's: $(_e_cp_runs "$other" "$cp" && echo running || echo stopped))"
	elif wait_for 30 _e_cp_stopped "$other" "$cp"; then pass "encounter-fight: leaving ends the checkpoint in both games" "the $owner's $cp stopped, then the $other's"
	else fail "encounter-fight: leaving ends the checkpoint in both games" "the $owner's $cp stopped; the $other's still runs 30 s later: $(grep -E "EncounterRuns: .*$cp" "$(_e_log "$other")" | tail -1 | cut -c1-200)"; fi

	# A fresh run, followed, and the owner's link dropped mid-run: the other's followed quest stops (nothing left of the owner's there).
	win_mark encf host; win_mark encf vm
	enter_both "$ENC_CP_CELL" 16
	_e_walk_both "$ENC_CP_TRIGGER" || { _e_save_rm "$other"; return; }
	_e_cp_share " (a second run)" || { pads_off; _e_save_rm "$other"; return; }
	# Both games must answer before the drop: one that crashed would read as "stopped" below (2026-11-12, rig run 9: the VM's
	# game crashed after the second run was shared, and the check after the drop passed on it).
	if ! _e_up host || ! _e_up vm; then _e_down "a game answers before the drop"; pads_off; _e_save_rm "$other"; return; fi
	if [ "$owner" = vm ]; then
		dc vm "net drop" >/dev/null
		echo "encounter-fight: the owner (vm) dropped its link mid-run"
	else
		dc vm "net drop" >/dev/null
		echo "encounter-fight: the host owns the run (its drop would end the session): the follower (vm) dropped its link instead, ending its session"
	fi
	if wait_for 30 _e_cp_stopped "$other" "$cp"; then pass "encounter-fight: the $other's followed $cp stops once the link drops" "$(grep -E "EncounterRuns: .*$cp .*(ended|session ends)" "$(_e_log "$other")" | tail -1 | cut -c1-160)"
	elif ! _e_up "$other"; then _e_down "the $other's followed $cp stops once the link drops"
	else fail "encounter-fight: the $other's followed $cp stops once the link drops" "still running 30 s later"; fi
	if wait_for 90 _e_joined; then pass "encounter-fight: the VM rejoins after its drop" "ok"
	elif ! _e_up vm; then _e_down "the VM rejoins after its drop"
	else rig_fault state "encounter-fight: the VM rejoins after its drop" "net status: $(nsf vm status)"; fi
	pads_off
	_e_save_rm "$other"
}
# _e_save_rm <who>: the save ENC_SAVE the step wrote in that game (and its co-save) removed; the backup's restore does not
# know it, so it would stay behind.
_e_save_rm() {
	if [ "$1" = host ]; then rm -f "$(dirname "$HOST_F4SE")/Saves/$ENC_SAVE".*
	else vmssh "Remove-Item -Force \"\$env:USERPROFILE\\Documents\\My Games\\Fallout4\\Saves\\$ENC_SAVE.*\" -ErrorAction SilentlyContinue" >/dev/null; fi
	echo "encounter-fight: the $1's save $ENC_SAVE removed"
}

# ---- companions: one owner per companion (opt-in: smoke.sh --only companions) ----------------------------------------------------
# The first game to recruit a companion keeps them (src/game/Companions.cpp, common/CompanionBook.h, protocol 65): the other's
# recruit is refused, its copy dismissed with a notice; the owner letting them go, or leaving, frees them (a dropped owner's are
# held 5 min for a rejoin). Dogmeat at Red Rocket, both games there by console (set-up, said so). The set-up recruits and the
# dismissal in the owner's game go through the game's own Followers script by console (`cqf Followers "FollowersScript.…"`, as the
# bench step assigns settlers): what is under test is the session's book, the refused game's dismissal and the hold, not the
# dialogue. The refused recruit is the VM's own doing through the script too; a real talk to the copy is the same path (the copy's
# dialogue runs the VM's own Followers script), and is left to a hand test.
# Real movement: the host walks COMPANION_WALK_DX/DY from where it stands (walkto.sh's pad; no teleport, no injected movement)
# while the VM's copy of Dogmeat is read against the host's own Dogmeat once a second: the copy stands where the dog stands
# (within COMPANION_SYNC u). When the host's dog moved at least COMPANION_SYNC_MOVE over those reads that is the "follows in
# sync" check; when he didn't, it is the "at rest" check, the copy matching a dog standing still. The dog following the host at
# heel is a hand check, not this step's: a console recruit never makes him follow (SetDogmeatCompanion then evp, or setav WaitingForPlayer 0, leave him standing; 0.16.1 the same), only the
# dialogue's recruit does. The drop case kills and restarts the VM's game alone and rejoins it through its dev channel
# (_vm_restart_rejoin_only): the host's game is never reloaded, so the server and its book stay as they are. Saves: the games'
# standing rig saves have Dogmeat free at Red Rocket; a game with Dogmeat already recruited is set up by dismissing him first
# (said so).
COMPANION_DOGMEAT=${COMPANION_DOGMEAT:-0001D162}; COMPANION_CELL=${COMPANION_CELL:-RedRocketExt}
COMPANION_WALK_DX=${COMPANION_WALK_DX:-1200}; COMPANION_WALK_DY=${COMPANION_WALK_DY:-0}  # the host's walk: from where it stands after the coc, this far along x and y (the forecourt is open eastward); COMPANION_WALK_TO="x y" overrides
COMPANION_SYNC=${COMPANION_SYNC:-150}                # u, the VM's copy of the dog from the host's own dog, at every read during the walk
COMPANION_SYNC_S=${COMPANION_SYNC_S:-10}             # reads (one a second) during the host's walk
COMPANION_SYNC_MOVE=${COMPANION_SYNC_MOVE:-100}      # u the host's own dog must move over those reads for the check to be the moving one ("follows in sync"); under it the check is the at-rest one
COMPANION_SETTLE=${COMPANION_SETTLE:-25}             # s, for a report, its answer and a dismissal to show in the logs
COMPANION_WAIT_HOLD=${COMPANION_WAIT_HOLD:-0}        # 1: also wait the 5-minute hold out after the VM's game is killed (adds ~7 min)
_c_log_has() { grep -qE "$2" "$(win_read companions "$1")"; }   # _c_log_has <who> <regex>: the game's log since win_mark has it
_c_held() { dc "$1" companions | grep -oE "DogmeatCompanion=[0-9A-F]{8}" | cut -d= -f2; }   # the ref the game's Dogmeat alias holds ("" when none)
_c_owner() { dc "$1" companions | grep -oE "DogmeatCompanion=[0-9A-F]{8} '[^']*' owner=[0-9]+" | grep -oE "owner=[0-9]+" | cut -d= -f2; }
_c_recruit() { dc "$1" "console cqf Followers \"FollowersScript.SetDogmeatCompanion\" $COMPANION_DOGMEAT" >/dev/null; sleep 1; dc "$1" "console $COMPANION_DOGMEAT.evp" >/dev/null; }   # SetDogmeatCompanion(Actor) fills the alias and sets the follow values; the alias package is evaluated by the dialogue's end in the real recruit, so evp here (a console-recruited Dogmeat stood still before)
_c_dismiss() { dc "$1" "console cqf Followers \"FollowersScript.DismissDogmeatCompanion\" 0 1" >/dev/null; }   # (ShowLocationAssignmentListIfAvailable, SuppressDismissMessage): the quiet path, no actor argument
_c_free_both() {  # both games with Dogmeat out of their aliases (set-up: dismissed by console where he is held)
	local w; for w in host vm; do
		if [ -n "$(_c_held "$w")" ]; then _c_dismiss "$w"; sleep 4; fi
		[ -z "$(_c_held "$w")" ] || return 1
	done
}
_c_player_id() { field "$(dc "$1" 'net status')" id; }   # that game's player id in the session
_c_sync_read() {  # "<u> <x> <y>": the VM's copy of the dog from the host's own dog (both read at once), and where the host's dog is
	local p1 p2 h v
	dc host "refinfo $COMPANION_DOGMEAT" > "$A_OUT/companions-sync-host.txt" & p1=$!
	dc vm "refinfo $COMPANION_DOGMEAT" > "$A_OUT/companions-sync-vm.txt" & p2=$!
	wait "$p1" "$p2"
	h=$(<"$A_OUT/companions-sync-host.txt"); v=$(<"$A_OUT/companions-sync-vm.txt")
	echo "$(_dist_xy "$(field "$h" x) $(field "$h" y)" "$(field "$v" x) $(field "$v" y)") $(field "$h" x) $(field "$h" y)"
}
# _vm_restart_rejoin_only <log name>: the VM's game alone is stopped, started again by its own start script, loaded into
# COMPANION_CELL and joined to the host's lobby through its dev channel (`steam join <lobby>`). The host's game is left as it
# is: rig.sh join loads the host too, whose input can die on that load, and the rig then restarts the host's game, the
# server and its book with it (2026-11-10). 0 once the VM's game says joined.
_vm_restart_rejoin_only() {
	local lobby i
	vmssh 'Stop-Process -Name Fallout4 -Force -ErrorAction SilentlyContinue; Start-Sleep 4; powershell -NoProfile -ExecutionPolicy Bypass -File C:\F4MP\in-vm-3-game.ps1 -WaitSeconds 150' > "$A_OUT/$1-vm.log" 2>&1
	vm_token_fetch >> "$A_OUT/$1-vm.log" 2>&1
	for i in 1 2 3 4 5; do vm focus >/dev/null; sleep 1; grep -q '^front=yes' <<<"$(vm focus)" && break; done   # in front for its first load (rig.sh's front)
	vm "console coc $COMPANION_CELL" >/dev/null
	wait_ingame vm 90 || { echo "the VM's game is not in the game after its coc" >> "$A_OUT/$1-vm.log"; return 1; }
	vm "god on" >/dev/null
	lobby=$(field "$(host 'net status')" lobby)
	if ! is_num "$lobby" || [ "$lobby" = 0 ]; then echo "the host hosts no lobby ('$lobby')" >> "$A_OUT/$1-vm.log"; return 1; fi
	echo "steam join: $(vm "steam join $lobby")" >> "$A_OUT/$1-vm.log"
	for i in $(seq 1 30); do vm 'net status' | grep -q 'status=joined' && { sleep 3; return 0; }; sleep 3; done
	echo "the VM's game never said joined: $(vm 'net status' | cut -c1-120)" >> "$A_OUT/$1-vm.log"; return 1
}
_c_tail() { grep -E 'Companions:' "$A_OUT/windelta-companions-$1.txt" | tail -3 | tr '\n' '|'; }   # the last Companions lines of that game's window
step_companions() {
	section "companions (one owner per companion: Dogmeat at Red Rocket; console placement, console recruits for set-up)"
	local hid vid n wp i r worst reads x0 y0 x1 y1 moved
	enter_both "$COMPANION_CELL" 14
	hid=$(_c_player_id host); vid=$(_c_player_id vm)
	if ! is_num "$hid" || ! is_num "$vid"; then fail "companions: set-up: both games are in the session" "host id '$hid', VM id '$vid'"; return; fi
	if ! wait_for 20 _c_free_both; then fail "companions: set-up: Dogmeat out of both games' aliases" "host '$(_c_held host)', VM '$(_c_held vm)'"; return; fi
	expect_eq "companions: set-up: the Followers aliases are read on both games" "$(dc host companions | grep -c 'aliases empty')$(dc vm companions | grep -c 'aliases empty')" 11
	sleep 3

	# 1. The host recruits: granted; the VM hears whose he is.
	win_mark companions host; win_mark companions vm
	_c_recruit host
	if wait_for "$COMPANION_SETTLE" _c_log_has host "Companions: $COMPANION_DOGMEAT \(.*\) is ours"; then pass "companions: the host's recruit of Dogmeat is granted" "$(grep -oE "Companions: $COMPANION_DOGMEAT.*is ours" "$A_OUT/windelta-companions-host.txt" | head -1)"
	else fail "companions: the host's recruit of Dogmeat is granted" "host alias '$(_c_held host)'; $(_c_tail host)"; return; fi
	if wait_for "$COMPANION_SETTLE" _c_log_has vm "Companions: $COMPANION_DOGMEAT \(.*\) is player $hid's"; then pass "companions: the VM hears Dogmeat is player $hid's" "owner=$(_c_owner vm)"
	else fail "companions: the VM hears Dogmeat is player $hid's" "$(_c_tail vm)"; fi

	# 2. Sync while the host walks (real movement): the VM's copy of Dogmeat stands where the host's own Dogmeat stands, read
	# once a second. (The dog following at heel is a hand check: a console recruit never makes him follow.)
	if [ -z "$COMPANION_WALK_TO" ]; then n=$(dc host pos); COMPANION_WALK_TO="$(awk -v x="$(field "$n" x)" -v y="$(field "$n" y)" -v dx="$COMPANION_WALK_DX" -v dy="$COMPANION_WALK_DY" 'BEGIN{printf "%d %d", x+dx, y+dy}')"; fi
	bash "$RIG/walkto.sh" host $COMPANION_WALK_TO 0.6 150 90 > "$A_OUT/companions-walk.log" 2>&1 & wp=$!
	sleep 1
	worst=0; reads=0; x0=""; y0=""; x1=""; y1=""
	for i in $(seq 1 "$COMPANION_SYNC_S"); do
		r=$(_c_sync_read); n=${r%% *}
		if is_num "$n"; then
			reads=$((reads + 1)); _cmp "$n" '>' "$worst" && worst=$n
			x1=$(cut -d' ' -f2 <<<"$r"); y1=$(cut -d' ' -f3 <<<"$r"); [ -z "$x0" ] && { x0=$x1; y0=$y1; }
		fi
		sleep 1
	done
	moved=$(_dist_xy "$x0 $y0" "$x1 $y1")
	if [ "$reads" -lt "$COMPANION_SYNC_S" ]; then fail "companions: while the host walks the VM's copy of Dogmeat is read against the host's Dogmeat" "$reads of $COMPANION_SYNC_S reads had both positions (the copy or the dog not loaded?)"
	elif ! is_num "$moved" || _cmp "$moved" '<' "$COMPANION_SYNC_MOVE"; then
		# The host's dog stood still (a console recruit leaves him standing): what is measured is the copy at rest.
		n="companions: the VM's copy of Dogmeat stands where the host's Dogmeat stands (at rest; following is a hand check)"
		if _cmp "$worst" '<=' "$COMPANION_SYNC"; then pass "$n" "worst $worst u over $reads reads (at most $COMPANION_SYNC); the host's dog moved ${moved:-?} u (under $COMPANION_SYNC_MOVE: at rest)"
		else fail "$n" "worst $worst u over $reads reads, want at most $COMPANION_SYNC; the host's dog moved ${moved:-?} u (under $COMPANION_SYNC_MOVE: at rest)"; fi
	elif _cmp "$worst" '<=' "$COMPANION_SYNC"; then pass "companions: while the host walks the VM's copy of Dogmeat follows in sync with the host's Dogmeat" "worst $worst u over $reads reads (at most $COMPANION_SYNC); the host's dog moved ${moved:-?} u meanwhile"
	else fail "companions: while the host walks the VM's copy of Dogmeat follows in sync with the host's Dogmeat" "worst $worst u over $reads reads, want at most $COMPANION_SYNC; the host's dog moved ${moved:-?} u meanwhile"; fi
	if wait "$wp"; then pass "companions: the host's walk arrived (real movement)" "$(tail -1 "$A_OUT/companions-walk.log")"
	else rig_fault walk "companions: the host's walk arrived (real movement)" "$(tail -1 "$A_OUT/companions-walk.log")"; fi

	# 3. The VM recruits the host's companion: refused, its copy dismissed, the notice shown.
	win_mark companions vm; win_mark companions host
	_c_recruit vm
	if wait_for "$COMPANION_SETTLE" _c_log_has vm "Companions: $COMPANION_DOGMEAT \(.*\) is player $hid's; our recruit is refused: dismissing our copy"; then pass "companions: the VM's recruit of the host's Dogmeat is refused" "$(grep -oE 'Companions: .*refused.*' "$A_OUT/windelta-companions-vm.txt" | head -1)"
	else fail "companions: the VM's recruit of the host's Dogmeat is refused" "VM alias '$(_c_held vm)'; $(_c_tail vm)"; fi
	if wait_for "$COMPANION_SETTLE" _c_log_has vm "Companions: dismissing $COMPANION_DOGMEAT .*FollowersScript.DismissDogmeatCompanion \(try 1 of 3\)"; then pass "companions: the VM's game dismisses its copy by the Followers script (try 1)" "$(grep -oE 'Companions: dismissing.*' "$A_OUT/windelta-companions-vm.txt" | head -1)"
	else fail "companions: the VM's game dismisses its copy by the Followers script (try 1)" "$(_c_tail vm)"; fi
	# The VM's own answer to the call (src/game/Companions.cpp logs it with what the alias holds then): ran and empty is the pass.
	if wait_for "$COMPANION_SETTLE" _c_log_has vm "Companions: the VM (ran|cancelled) FollowersScript.DismissDogmeatCompanion for $COMPANION_DOGMEAT"; then
		n=$(grep -oE "Companions: the VM (ran|cancelled) FollowersScript.DismissDogmeatCompanion for $COMPANION_DOGMEAT.*" "$(win_read companions vm)" | head -1)
		expect_match "companions: the VM ran DismissDogmeatCompanion and the alias was empty after it" "$n" "the VM ran .*DogmeatCompanion alias holds nothing"
	else fail "companions: the VM ran DismissDogmeatCompanion and the alias was empty after it" "no answer from the VM to the queued call; $(_c_tail vm)"; fi
	if wait_for "$COMPANION_SETTLE" _c_log_has vm "Companions: our aliases let go of $COMPANION_DOGMEAT"; then pass "companions: the VM's Dogmeat alias is empty again after the dismissal" "VM alias '$(_c_held vm)'"
	else fail "companions: the VM's Dogmeat alias is empty again after the dismissal" "VM alias '$(_c_held vm)' (the script's quiet dismissal did not take: see the PR's risks)"; fi
	expect_eq "companions: the host still has Dogmeat" "$(_c_held host)" "$COMPANION_DOGMEAT"
	sleep 2
	if grep -qE "Companions: $COMPANION_DOGMEAT \(.*\) is player $vid's" "$(win_read companions host)"; then fail "companions: the host's game never hears Dogmeat is the VM's" "it did"; else pass "companions: the host's game never hears Dogmeat is the VM's" "no such line"; fi
	n=$(dc vm 'net status' | grep -oE 'companions: [^|]*' | head -1); expect_match "companions: the VM's net status counts the refusal" "$n" "refused=[1-9]"

	# 4. The host lets him go: free; the VM recruits him: granted; the host's game hears whose he is.
	win_mark companions vm; win_mark companions host
	_c_dismiss host
	if wait_for "$COMPANION_SETTLE" _c_log_has vm "Companions: $COMPANION_DOGMEAT \(.*\) is free"; then pass "companions: the host's dismissal frees Dogmeat for everyone" "VM hears free"
	else fail "companions: the host's dismissal frees Dogmeat for everyone" "host alias '$(_c_held host)'; $(_c_tail vm)"; fi
	_c_recruit vm
	if wait_for "$COMPANION_SETTLE" _c_log_has vm "Companions: $COMPANION_DOGMEAT \(.*\) is ours"; then pass "companions: the VM's recruit after the host's dismissal is granted" "ok"
	else fail "companions: the VM's recruit after the host's dismissal is granted" "$(_c_tail vm)"; fi
	if wait_for "$COMPANION_SETTLE" _c_log_has host "Companions: $COMPANION_DOGMEAT \(.*\) is player $vid's"; then pass "companions: the host hears Dogmeat is player $vid's" "ok"
	else fail "companions: the host hears Dogmeat is player $vid's" "$(_c_tail host)"; fi

	# 5. The owner's game drops (killed, no leave): held for them; the host's recruit is refused as held; the VM rejoins and has him again.
	crash_snapshot
	win_mark companions host
	vmssh 'Stop-Process -Name Fallout4 -Force -ErrorAction SilentlyContinue' >/dev/null 2>&1
	if wait_for 60 _c_log_has host "Companions: $COMPANION_DOGMEAT \(.*\) is held for player $vid, who left"; then pass "companions: a dropped owner's Dogmeat is held for them" "ok"
	else fail "companions: a dropped owner's Dogmeat is held for them" "$(_c_tail host)"; fi
	win_mark companions host
	_c_recruit host
	if wait_for "$COMPANION_SETTLE" _c_log_has host "Companions: $COMPANION_DOGMEAT \(.*\) is player $vid's, held while they are away; our recruit is refused"; then pass "companions: the host's recruit while held is refused" "ok"
	else fail "companions: the host's recruit while held is refused" "$(_c_tail host)"; fi
	wait_for "$COMPANION_SETTLE" _c_log_has host "Companions: our aliases let go of $COMPANION_DOGMEAT" >/dev/null
	expect_eq "companions: the host's copy is dismissed while Dogmeat is held" "$(_c_held host)" ""
	# The VM's game alone comes back (the host's is not reloaded: the server's hold must outlive the VM's game, not the
	# host's), loaded into the cell from its main menu, which does not hold Dogmeat (recruited by console, never saved): the
	# server hands him back at the welcome (held for this identity), the VM's first look finds its aliases without him and
	# lets him go, and he is free for everyone. (A save that holds him keeps him the VM's: the hand test in the PR.)
	if ! _vm_restart_rejoin_only companions; then rig_broken game "companions: the VM's game alone is restarted and rejoins the host's session" "$(tail -1 "$A_OUT/companions-vm.log")"; return; fi
	pass "companions: the VM's game alone is restarted and rejoins the host's session" "$(field "$(vm 'net status')" status); the host's game not reloaded"
	if wait_for 90 _c_log_has host "Companions: $COMPANION_DOGMEAT \(.*\) is player [0-9]+'s\$"; then pass "companions: the rejoin hands Dogmeat back to the VM (under its id)" "$(grep -oE "Companions: $COMPANION_DOGMEAT.*is player [0-9]+'s$" "$A_OUT/windelta-companions-host.txt" | tail -1)"
	else fail "companions: the rejoin hands Dogmeat back to the VM (under its id)" "$(_c_tail host)"; fi
	if wait_for 60 _c_log_has host "Companions: $COMPANION_DOGMEAT \(.*\) is free"; then pass "companions: the VM's save without Dogmeat lets him go, and he is free again" "VM alias '$(_c_held vm)'"
	else fail "companions: the VM's save without Dogmeat lets him go, and he is free again" "VM alias '$(_c_held vm)'; $(_c_tail host)"; fi
	vid=$(_c_player_id vm)
	if [ "$COMPANION_WAIT_HOLD" = 1 ]; then
		_c_recruit vm; sleep 4   # the VM's again, for the hold to run on
		# The hold runs out: the VM's game is killed again, and 5 min later Dogmeat is free; the host recruits him.
		win_mark companions host
		vmssh 'Stop-Process -Name Fallout4 -Force -ErrorAction SilentlyContinue' >/dev/null 2>&1
		if wait_for 400 _c_log_has host "Companions: $COMPANION_DOGMEAT \(.*\) is free"; then pass "companions: after the 5-minute hold a dropped owner's Dogmeat is free" "ok"
		else fail "companions: after the 5-minute hold a dropped owner's Dogmeat is free" "$(_c_tail host)"; fi
		_c_recruit host
		if wait_for "$COMPANION_SETTLE" _c_log_has host "Companions: $COMPANION_DOGMEAT \(.*\) is ours"; then pass "companions: the host recruits him once free" "ok"; else fail "companions: the host recruits him once free" "host alias '$(_c_held host)'"; fi
		_c_dismiss host; sleep 4
		_vm_restart_rejoin_only companions-2 || rig_broken game "companions: the VM's game rejoins after the hold" "$(tail -1 "$A_OUT/companions-2-vm.log")"
	fi
	crash_verdict "companions"
	expect_eq "companions: Dogmeat is free in both games at the end" "$(_c_held host)$(_c_held vm)" ""
	# ... and the server's book is empty: no game knows an owner (known=0 in both games' companions line).
	expect_eq "companions: the server's book is empty at the end" "$(dc host companions | grep -oE 'known=[0-9]+')/$(dc vm companions | grep -oE 'known=[0-9]+')" "known=0/known=0"
}

# ---- companions_human: the Companion slot (a human companion) (opt-in: smoke.sh --only companions_human) ------------------------------
# The same refusal through the Followers quest's Companion alias (ClearCompanion's path, not Dogmeat's): Codsworth at
# Sanctuary, both games there by console (set-up, said so). The script's SetCompanion, read off FollowersScript.pex, is
# SetCompanion(Actor ActorToMakeCompanion, Bool SetCompanion, Bool FillCompanionAlias, Bool SuppressDismissMessage): the console
# call must give all four (a one-argument call is dropped by the VM and fills nothing, which is what an earlier run met), and the
# alias is checked filled before anything else (a loud FAIL otherwise). Codsworth is a robot of the plugin, a plugin NPC with a
# base (not one built at a workbench), so he is shared like any NPC. After a console recruit the alias package is evaluated (evp),
# as the dialogue's end would.
COMPANION_HUMAN=${COMPANION_HUMAN:-$CODSWORTH}; COMPANION_HUMAN_CELL=${COMPANION_HUMAN_CELL:-SanctuaryExt}
_ch_held() { dc "$1" companions | grep -oE "(^| )Companion=[0-9A-F]{8}" | grep -oE "[0-9A-F]{8}$"; }   # the ref the game's Companion alias holds ("" when none)
_ch_recruit() { dc "$1" "console cqf Followers \"FollowersScript.SetCompanion\" $COMPANION_HUMAN 1 1 1" >/dev/null; sleep 1; dc "$1" "console $COMPANION_HUMAN.evp" >/dev/null; }
_ch_dismiss() { dc "$1" "console cqf Followers \"FollowersScript.DismissCompanion\" $COMPANION_HUMAN 0 1" >/dev/null; }   # (actor, ShowLocationAssignmentListIfAvailable, SuppressDismissMessage)
_ch_free_both() { local w; for w in host vm; do if [ -n "$(_ch_held "$w")" ]; then _ch_dismiss "$w"; sleep 4; fi; [ -z "$(_ch_held "$w")" ] || return 1; done; }
# Read afresh on each retry of wait_for (a $(...) in wait_for's arguments expands once, before the wait).
_ch_holds() { [ "$(_ch_held "$1")" = "$COMPANION_HUMAN" ]; }   # _ch_holds <who>: that game's Companion alias holds him now
_ch_filled_or_let_go() { _ch_holds "$1" || _c_log_has "$1" "Companions: our aliases let go of $COMPANION_HUMAN"; }   # ... or it filled and let go again already (a refusal's dismissal)
step_companions_human() {
	section "companions_human (the Companion slot: Codsworth at Sanctuary; console placement, console recruits for set-up)"
	local hid vid n
	enter_both "$COMPANION_HUMAN_CELL" 14
	hid=$(_c_player_id host); vid=$(_c_player_id vm)
	if ! is_num "$hid" || ! is_num "$vid"; then fail "companions_human: set-up: both games are in the session" "host id '$hid', VM id '$vid'"; return; fi
	if ! wait_for 20 _ch_free_both; then fail "companions_human: set-up: the Companion alias empty in both games" "host '$(_ch_held host)', VM '$(_ch_held vm)'"; return; fi
	# Dogmeat out of both games' aliases first: SetCompanion dismisses a held Dogmeat with the settlement list on (its own
	# DismissDogmeatCompanion(True, False)), a menu a scripted run can't answer.
	if ! wait_for 20 _c_free_both; then fail "companions_human: set-up: Dogmeat out of both games' aliases (SetCompanion would dismiss him with the settlement list up)" "host '$(_c_held host)', VM '$(_c_held vm)'"; return; fi
	if [ "$(dc host "refinfo $COMPANION_HUMAN" | grep -c 'ref=')" != 1 ]; then fail "companions_human: set-up: the host has $COMPANION_HUMAN loaded" "$(dc host "refinfo $COMPANION_HUMAN" | cut -c1-120)"; return; fi

	# 1. The host recruits by the script's SetCompanion (all four arguments): the alias must fill, or nothing below means anything.
	win_mark companions host; win_mark companions vm
	_ch_recruit host
	if wait_for 10 _ch_holds host; then pass "companions_human: SetCompanion($COMPANION_HUMAN, 1, 1, 1) by console fills the host's Companion alias" "ok"
	else fail "companions_human: SetCompanion($COMPANION_HUMAN, 1, 1, 1) by console fills the host's Companion alias" "alias '$(_ch_held host)': the console call did not take (its arity, or the actor); nothing below is meaningful"; return; fi
	if wait_for "$COMPANION_SETTLE" _c_log_has host "Companions: $COMPANION_HUMAN \(.*\) is ours"; then pass "companions_human: the host's recruit is granted" "ok"
	else fail "companions_human: the host's recruit is granted" "$(_c_tail host)"; return; fi
	if wait_for "$COMPANION_SETTLE" _c_log_has vm "Companions: $COMPANION_HUMAN \(.*\) is player $hid's"; then pass "companions_human: the VM hears whose he is" "ok"
	else fail "companions_human: the VM hears whose he is" "$(_c_tail vm)"; fi

	# 2. The VM recruits him too: refused, dismissed by ClearCompanion on the first try (the VM says it ran it, the alias empty
	# then), the alias let go, no second try, no "going home" message.
	win_mark companions vm
	_ch_recruit vm
	if wait_for 10 _ch_filled_or_let_go vm; then pass "companions_human: SetCompanion by console fills the VM's Companion alias" "ok"
	else fail "companions_human: SetCompanion by console fills the VM's Companion alias" "alias '$(_ch_held vm)'"; return; fi
	if wait_for "$COMPANION_SETTLE" _c_log_has vm "Companions: $COMPANION_HUMAN \(.*\) is player $hid's; our recruit is refused: dismissing our copy"; then pass "companions_human: the VM's recruit is refused" "ok"
	else fail "companions_human: the VM's recruit is refused" "$(_c_tail vm)"; fi
	if wait_for "$COMPANION_SETTLE" _c_log_has vm "Companions: dismissing $COMPANION_HUMAN .*FollowersScript.ClearCompanion \(try 1 of 3\)"; then pass "companions_human: the VM dismisses its copy by ClearCompanion" "ok"
	else fail "companions_human: the VM dismisses its copy by ClearCompanion" "$(_c_tail vm)"; fi
	# The VM's own answer to the call (src/game/Companions.cpp logs it with what the alias holds then): ran and empty is the
	# pass; ran with him still held, cancelled, or no answer at all each say a different thing went wrong.
	if wait_for "$COMPANION_SETTLE" _c_log_has vm "Companions: the VM (ran|cancelled) FollowersScript.ClearCompanion for $COMPANION_HUMAN"; then
		n=$(grep -oE "Companions: the VM (ran|cancelled) FollowersScript.ClearCompanion for $COMPANION_HUMAN.*" "$(win_read companions vm)" | head -1)
		expect_match "companions_human: the VM ran ClearCompanion and the alias was empty after it" "$n" "the VM ran .*Companion alias holds nothing"
	else fail "companions_human: the VM ran ClearCompanion and the alias was empty after it" "no answer from the VM to the queued call; $(_c_tail vm)"; fi
	if _c_log_has vm "\[W\] Companions: "; then fail "companions_human: no companion WARN in the VM's log" "$(grep -oE '\[W\] Companions: .*' "$(win_read companions vm)" | head -1)"
	else pass "companions_human: no companion WARN in the VM's log" "none"; fi
	if wait_for "$COMPANION_SETTLE" _c_log_has vm "Companions: our aliases let go of $COMPANION_HUMAN"; then pass "companions_human: the VM's Companion alias is empty again on the first try" "alias '$(_ch_held vm)'"
	else fail "companions_human: the VM's Companion alias is empty again on the first try" "alias '$(_ch_held vm)'; $(_c_tail vm)"; fi
	if _c_log_has vm "Companions: dismissing $COMPANION_HUMAN .*\(try 2 of 3\)"; then fail "companions_human: one dismissal was enough" "a second try went"; else pass "companions_human: one dismissal was enough" "no second try"; fi
	expect_eq "companions_human: the host still has him" "$(_ch_held host)" "$COMPANION_HUMAN"
	expect_eq "companions_human: the VM still knows the owner after its let-go" "$(dc vm companions | grep -oE 'known=[0-9]+')" "known=1"

	# 3. The host lets him go; the VM recruits him; put back.
	win_mark companions vm; win_mark companions host
	_ch_dismiss host
	if wait_for "$COMPANION_SETTLE" _c_log_has vm "Companions: $COMPANION_HUMAN \(.*\) is free"; then pass "companions_human: the host's dismissal frees him" "ok"
	else fail "companions_human: the host's dismissal frees him" "host alias '$(_ch_held host)'; $(_c_tail vm)"; fi
	_ch_recruit vm
	if wait_for "$COMPANION_SETTLE" _c_log_has vm "Companions: $COMPANION_HUMAN \(.*\) is ours"; then pass "companions_human: the VM's recruit after the host's dismissal is granted" "ok"
	else fail "companions_human: the VM's recruit after the host's dismissal is granted" "$(_c_tail vm)"; fi
	_ch_dismiss vm; sleep 4
	expect_eq "companions_human: the Companion alias is empty in both games at the end" "$(_ch_held host)$(_ch_held vm)" ""
	expect_eq "companions_human: the server's book is empty at the end" "$(dc host companions | grep -oE 'known=[0-9]+')/$(dc vm companions | grep -oE 'known=[0-9]+')" "known=0/known=0"
}

# ---- attacks: the host's settlement attack, shared (opt-in: smoke.sh --only attacks) ----------------------------------------------
# A random attack on a settlement the host owns is the host's (src/game/SettlementAttacks.cpp, common/AttackBook.h): the VM's game
# runs the host's exact quest as a mirror (same quest, same workbench) whose own timer never begins it, takes stage 100 (the
# attack's done stage) as soon as it runs, so neither its shutdown stage nor any save of the VM's rolls the host's damage, shows
# "Defend <settlement>" only while the VM's player and the host's attackers are there, and stops with the host's run; a roll of the
# VM's own at the host's settlement is defused and stopped. The settlement is Red Rocket, not Sanctuary: with Nuka-World installed
# (the rig has it) no attack quest can start at Sanctuary. Nuka-World's start-game quest DLC04WorkshopParent holds Sanctuary's
# workbench in a reserving alias (SanctuaryWorkshop), and an attack quest's Workshop alias takes no reserved reference, so the story
# manager starts none there, forced or not (rig, 2026-11-11: the forced roll started one at Red Rocket, Abernathy Farm and Starlight
# Drive-In within seconds and never at Sanctuary; dev `aliases <DLC04WorkshopParent>` shows the hold). All set-up is by console and
# said so: both games coc to Red Rocket; it is made the host's (WorkshopScript.SetOwnedByPlayer, as the bench step does); the host's
# attack is rolled with WorkshopScript.CheckForAttack(true) on the workbench (WorkshopParentScript.TestForceAttack's own call; the
# random roll is its unforced form); its 24 game-hour wait is cut short with `setstage <quest> 20`; the fight's end is
# `setstage <quest> 100` (in the game the attackers' deaths set it); the run's end is `stopquest` (its shutdown stage, 1000, as
# when its aliases unload). Round 1: the VM at the settlement (it sees the attack: objective shown, the host's attackers shared).
# Round 2: the VM away in an interior (the settlement not loaded there: the case where a run of its own would roll damage), the
# host's run ending undefended; the VM's own settlement (its resources, and 500 caps put in its workbench by console) is the same
# after. Round 3: the VM's own forced roll at the settlement (a WARN, not a FAIL, when its story manager starts no quest).
# Round 4: the VM, away, saves (`save f4mpattacks`) while it mirrors the host's attack and leaves (`net leave`); its save, loaded
# solo, has the mirror stopped and plays no attack out; the VM joins the host's lobby again. It changes both saves (the settlement
# owned, caps, a save written on the VM): throwaway saves only. ATTACK_BENCH, ATTACK_CELL and ATTACK_NAME pick another settlement.
ATTACK_BENCH=${ATTACK_BENCH:-00054BAE}; ATTACK_CELL=${ATTACK_CELL:-RedRocketExt}; ATTACK_NAME=${ATTACK_NAME:-Red Rocket}
ATTACK_AWAY_CELL=${ATTACK_AWAY_CELL:-SuperDuperMart01}   # where the VM waits in round 2: an interior, the settlement unloaded there once the VM's cell buffer is purged
ATTACK_SETTLE=${ATTACK_SETTLE:-30}   # s, for a phase to reach the other game and its mirror to take a step (looks every 0.5 s, steps again every 5 s, a start waits 15 s)
# ATTACK_QUEST: an attack quest's editor ID (an add-on's, say DLC01WorkshopRobotAttack01): the host's game starts that one at the
# bench with its Workshop alias and location forced (radiantstart) instead of letting the story manager roll one (its roll
# picks only Fallout4.esm's). Empty: the roll.
ATTACK_QUEST=${ATTACK_QUEST:-}
_at_list() { dc "$1" attacks; }   # entries end in "; ", then the attacks: counters
_at_form_of() { dc "$1" "forms 80 $2" | grep -oE "[0-9A-F]{8}:$2:" | head -1 | cut -c1-8; }   # _at_form_of <who> <questEditorId>: its form ID in that game ("" when it has no such quest: the add-on isn't installed)
_at_entry() { _at_list "$1" | tr ';' '\n' | grep -E "^ ?$2 .* workshop=$ATTACK_BENCH " | head -1; }   # _at_entry <who> <quest>: that quest's entry at the bench ("" when not running there)
_at_quest_at() { _at_list "$1" | tr ';' '\n' | grep -E " workshop=$ATTACK_BENCH " | grep -E "$2" | grep -oE '^ ?[0-9A-F]{8}' | tr -d ' ' | head -1; }   # _at_quest_at <who> <own|mirror>: the quest running at the bench
_at_none_at() { ! _at_list "$1" | tr ';' '\n' | grep -qE " workshop=$ATTACK_BENCH "; }   # no attack quest runs at the bench in that game
_at_log_has() { grep -qE "$2" "$(win_read attacks "$1")"; }
_at_log_ever() { win_read attacks "$1" >/dev/null; grep -qE "$2" "$A_OUT/winnow-attacks-$1/0.log"; }   # anywhere in that game's log since it started, not only since the mark
# _at_vm_away: the VM goes to the interior, and its cell buffer is purged (`pcb`): the game keeps a cell it just left, and the
# workbench's 3D with it, so the settlement would still count as loaded there (the game's own Is3DLoaded says so too).
_at_vm_away() { dc vm "console coc $ATTACK_AWAY_CELL" >/dev/null; sleep 12; wait_ingame vm 40; dc vm "console pcb" >/dev/null; sleep 3; }
_at_entry_is() { [[ $(_at_entry "$1" "$2") =~ $3 ]]; }
_at_gone() { [ -z "$(_at_entry "$1" "$2")" ]; }
_at_count() { field "$(dc "$1" 'net status' | grep -oE 'attacks: [^|]*' | head -1)" "$2"; }
# _at_attackers_far: every attacker the host's game shared since the mark stands past ATTACK_PLACE_RANGE of the VM's player (none
# read, or one whose place could not be read: false). Sets atFar to each one's distance (" FF0010C0 10530 u; ...") for the step's lines.
ATTACK_PLACE_RANGE=${ATTACK_PLACE_RANGE:-6000}   # SpawnBook.h PLACE_RANGE
_at_attackers_far() {
	local me ref at d near=0 n=0
	atFar=""
	me=$(dc vm pos)
	for ref in $(sed -nE 's/.*Spawns: sharing ([0-9A-F]{8}) .*/\1/p' "$(win_read attacks host)" | sort -u); do
		at=$(dc host "refinfo $ref")
		d=$(awk -v a="$me" -v b="$at" 'function f(s, k) { return match(s, " " k "=-?[0-9.]+") ? substr(s, RSTART + length(k) + 2, RLENGTH - length(k) - 2) : "" }
			BEGIN { ax = f(a, "x"); ay = f(a, "y"); bx = f(b, "x"); by = f(b, "y"); if (ax == "" || ay == "" || bx == "" || by == "") exit; printf "%d", sqrt((ax - bx) ^ 2 + (ay - by) ^ 2) }')
		if [ -z "$d" ]; then atFar="$atFar $ref unread;"; near=1; continue; fi   # not known to be far: never a WARN
		n=$((n + 1)); atFar="$atFar $ref $d u;"
		[ "$d" -le "$ATTACK_PLACE_RANGE" ] && near=1
	done
	[ -n "$atFar" ] || atFar=" none read"
	[ "$n" -gt 0 ] && [ "$near" = 0 ]
}
_at_tail() { grep -E 'SettlementAttacks:' "$A_OUT/windelta-attacks-$1.txt" 2>/dev/null | tail -3 | tr '\n' '|'; }
# _at_order <who> <first regex> <then regex>: in that game's window, a line matching the first comes before one matching the second
_at_order() { awk -v a="$2" -v b="$3" '$0 ~ a && !x {x=NR} $0 ~ b && x && NR>x {ok=1} END{exit !ok}' "$A_OUT/windelta-attacks-$1.txt"; }
# _at_roll_host: the host's game rolls an attack on the bench (forced), or starts ATTACK_QUEST there; echoes the quest that runs
# there, "" when none did
_at_roll_host() {
	local q="" i id
	if [ -n "$ATTACK_QUEST" ]; then
		id=$(_at_form_of host "$ATTACK_QUEST")
		if [ -z "$id" ]; then echo ""; return; fi
		# Alias 1 (Workshop) is filled from the location alias, not the event, so it is named; the location is the bench's own.
		host "radiantstart $id $ATTACK_BENCH - 1=$ATTACK_BENCH" >/dev/null
		for i in $(seq 1 40); do q=$(_at_entry host "$id" | grep -oE '^ ?[0-9A-F]{8}' | tr -d ' '); [ -n "$q" ] && break; sleep 0.5; done
		echo "$q"; return
	fi
	host "console $ATTACK_BENCH.cf \"WorkshopScript.CheckForAttack\" 1" >/dev/null
	for i in $(seq 1 40); do q=$(_at_quest_at host own); [ -n "$q" ] && break; sleep 0.5; done
	if [ -z "$q" ]; then   # TestForceAttack: the same call on the workshop the player stands in
		host "console cqf WorkshopParent \"WorkshopParentScript.TestForceAttack\"" >/dev/null
		for i in $(seq 1 40); do q=$(_at_quest_at host own); [ -n "$q" ] && break; sleep 0.5; done
	fi
	echo "$q"
}
_at_mirror_round() {  # _at_mirror_round <round label> <at|away> [optional]: the host rolls, begins, (defends if at) and ends; the VM mirrors. optional: a start that doesn't run is a WARN (return 2), not a FAIL
	local tag=$1 where=$2 optional=${3:-} q vs0 ve0 cp0 cp1 outcome
	vs0=$(_at_count vm mirrorStarts); ve0=$(_at_count vm mirrorsEnded); cp0=$(_rad_spawns vm | grep -oE 'copies=[0-9]+' | cut -d= -f2)
	win_mark attacks host; win_mark attacks vm
	q=$(_at_roll_host)
	if [ -z "$q" ]; then
		if [ -n "$optional" ]; then warn "attacks ($tag): set-up: the host's game starts the attack on $ATTACK_NAME" "not run: no attack quest runs at $ATTACK_BENCH (a location alias its conditions found nothing for at $ATTACK_NAME?): $(_at_list host | cut -c1-160)"; return 2; fi
		fail "attacks ($tag): set-up: the host's game rolls an attack on $ATTACK_NAME" "no attack quest runs at $ATTACK_BENCH: $(_at_list host | cut -c1-160)"; return 1
	fi
	pass "attacks ($tag): set-up: the host's game rolls an attack on $ATTACK_NAME" "$(_at_entry host "$q" | cut -c1-120)"
	if wait_for "$ATTACK_SETTLE" _at_log_has host "SettlementAttacks: our attack $q '.*' on $ATTACK_BENCH .*: phase 0"; then pass "attacks ($tag): the host's game reports its attack (rolled)" "$q"
	else fail "attacks ($tag): the host's game reports its attack (rolled)" "$(_at_tail host)"; fi
	if wait_for "$ATTACK_SETTLE" _at_log_has vm "SettlementAttacks: the host's attack $q '.*' on $ATTACK_BENCH .*: phase 0"; then pass "attacks ($tag): the VM hears the host's attack" "$q"
	else fail "attacks ($tag): the VM hears the host's attack" "$(_at_tail vm)"; return 1; fi
	if wait_for 45 _at_entry_is vm "$q" "mirror\(host phase 0\)"; then pass "attacks ($tag): the VM runs the host's quest at the same workbench as a mirror" "$(_at_entry vm "$q" | cut -c1-140)"
	else fail "attacks ($tag): the VM runs the host's quest at the same workbench as a mirror" "VM: $(_at_list vm | cut -c1-200); $(_at_tail vm)"; return 1; fi
	if _at_log_ever vm "SettlementAttacks: settlement $ATTACK_BENCH is the host's"; then pass "attacks ($tag): the VM hears $ATTACK_NAME is the host's" "ok"
	else fail "attacks ($tag): the VM hears $ATTACK_NAME is the host's" "$(_at_tail vm)"; fi
	# Defused as soon as it runs (stage 100 before the host's attack even begins): no save of the VM's ever holds a mirror
	# whose 1000 would roll the host's damage.
	if wait_for "$ATTACK_SETTLE" _at_log_has vm "SettlementAttacks: our mirror $q '.*': stage 100 \(defused as it runs"; then pass "attacks ($tag): the VM's mirror takes stage 100 as soon as it runs" "$(_at_entry vm "$q" | cut -c1-140)"
	else fail "attacks ($tag): the VM's mirror takes stage 100 as soon as it runs" "$(_at_entry vm "$q" | cut -c1-140); $(_at_tail vm)"; fi

	# Begun: the 24 game-hour wait cut short in the host's game (set-up). The mirror never begins on its own.
	host "console setstage $q 20" >/dev/null
	# Phase 1 or 2: an attack begun and defended between two of the host's phase looks is reported as phase 2 only (Nuka-World's Minutemen attack is defended 0.5 s after its 20).
	if wait_for "$ATTACK_SETTLE" _at_log_has vm "SettlementAttacks: the host's attack $q '.*' on $ATTACK_BENCH .*: phase [12]"; then pass "attacks ($tag): the host's attack begins and the VM hears it" "ok"
	else fail "attacks ($tag): the host's attack begins and the VM hears it" "host: $(_at_entry host "$q" | cut -c1-120); $(_at_tail vm)"; fi
	sleep 3
	expect_match "attacks ($tag): the VM's mirror has not begun on its own (no attackers of its own) and is defused" "$(_at_entry vm "$q")" "begun=false done=true"
	if [ "$where" = at ]; then
		# An add-on's attack (optional) may make no attacker the host's game shares at ATTACK_NAME (2026-11-11, a coc world's Red
		# Rocket: Nuka-World's Minutemen attack was defended 0.5 s after its 20, the Brotherhood's rides in on a vertibird enabled
		# later): then the objective, the copies and the objective's completion have nothing to show, and are WARNs naming why.
		local noattackers="" cpNow=""
		if [ -n "$optional" ] && ! wait_for 15 _at_log_has host "Spawns: sharing [0-9A-F]{8} "; then
			noattackers="the host's game shared no attacker of $q in 15 s after its 20 (host: $(_at_entry host "$q" | cut -c1-100); $(_rad_spawns host)): nothing of the host's attack stands at $ATTACK_NAME for the VM to see"
		fi
		# "Defend <settlement>" is shown only while the VM's player and the host's attackers (their copies) are there.
		if wait_for "$ATTACK_SETTLE" _at_log_has vm "SettlementAttacks: our player is at $ATTACK_BENCH during the host's attack.*: our mirror's objective 10 is shown"; then pass "attacks ($tag): the VM at $ATTACK_NAME with the host's attackers there is shown the attack's objective" "ok"
		elif [ -n "$noattackers" ]; then warn "attacks ($tag): the VM at $ATTACK_NAME with the host's attackers there is shown the attack's objective" "not judged: $noattackers"
		elif _at_attackers_far && cpNow=$(_rad_spawns vm | grep -oE 'copies=[0-9]+' | cut -d= -f2) && is_int "$cp0" && is_int "$cpNow" && [ "$cpNow" -le "$cp0" ]; then
			# None of the host's attackers came within a copy's range of the VM's player (rig run 11, 2026-11-12: the feral attack's
			# three ghouls stood ~10500 u east of Red Rocket the whole 49 s; the same quest made 2 copies on another roll): the copies,
			# the objective and its completion have nothing to show, as with no attacker shared. Only with no copy placed in the VM's
			# game since the round began: one placed means an attacker was in range, and the objective is judged.
			noattackers="the host's attackers stood past $ATTACK_PLACE_RANGE u of the VM's player (a copy is placed only within it: SpawnBook PLACE_RANGE):$atFar"
			warn "attacks ($tag): the VM at $ATTACK_NAME with the host's attackers there is shown the attack's objective" "not judged: $noattackers"
		else fail "attacks ($tag): the VM at $ATTACK_NAME with the host's attackers there is shown the attack's objective" "$(_at_tail vm) attackers:$atFar"; fi
		# The host's attackers are the VM's to fight: Spawns shares them while the host's game has them loaded.
		sleep 10; cp1=$(_rad_spawns vm | grep -oE 'copies=[0-9]+' | cut -d= -f2)
		if [ -n "$noattackers" ]; then warn "attacks ($tag): the host's attackers are copied into the VM's game" "not judged: $noattackers (copies '$cp0' then '$cp1')"
		elif is_int "$cp0" && is_int "$cp1" && [ "$((cp1 - cp0))" -ge 1 ]; then pass "attacks ($tag): the host's attackers are copied into the VM's game" "$((cp1 - cp0)) copy (at least 1)"
		elif is_int "$cp0" && is_int "$cp1"; then _at_attackers_far; fail "attacks ($tag): the host's attackers are copied into the VM's game" "$((cp1 - cp0)) copy, want at least 1; the host's attackers from the VM's player:$atFar"
		else fail "attacks ($tag): the host's attackers are copied into the VM's game" "spawns copies '$cp0' then '$cp1'"; fi
		# Defended: the fight's end set in the host's game (set-up); the VM's objective is completed with it.
		host "console setstage $q 100" >/dev/null
		if wait_for "$ATTACK_SETTLE" _at_log_has vm "SettlementAttacks: the host's attack $q '.*' on $ATTACK_BENCH .*: phase 2"; then pass "attacks ($tag): the host's defence reaches the VM" "ok"
		else fail "attacks ($tag): the host's defence reaches the VM" "$(_at_tail host) / $(_at_tail vm)"; fi
		if wait_for "$ATTACK_SETTLE" _at_log_has vm "SettlementAttacks: our mirror's objective 10 at $ATTACK_BENCH is completed"; then pass "attacks ($tag): the VM's objective is completed with the host's defence" "ok"
		elif [ -n "$noattackers" ]; then warn "attacks ($tag): the VM's objective is completed with the host's defence" "not judged: no objective was shown ($noattackers)"
		else fail "attacks ($tag): the VM's objective is completed with the host's defence" "$(_at_tail vm)"; fi
		expect_match "attacks ($tag): the VM's mirror stays defused through the host's defence" "$(_at_entry vm "$q")" "done=true"
		outcome=1
	else
		if _at_log_has vm "SettlementAttacks: our player is at $ATTACK_BENCH"; then fail "attacks ($tag): the VM away is not shown the attack's objective" "it was"
		else pass "attacks ($tag): the VM away is not shown the attack's objective" "no such line"; fi
		expect_match "attacks ($tag): $ATTACK_NAME is not loaded in the VM's game" "$(_at_entry vm "$q")" "loaded=false"
		outcome=2   # begun and never defended: the host's run resolves (the host's own roll where the settlement isn't loaded there)
	fi

	# The run ends in the host's game (stopquest: its shutdown stage). The VM's mirror, defused since it ran, stops with it.
	host "console stopquest $q" >/dev/null
	if wait_for "$ATTACK_SETTLE" _at_log_has host "SettlementAttacks: our attack $q '.*' on $ATTACK_BENCH .*: phase 3, outcome $outcome"; then pass "attacks ($tag): the host's game reports the end (outcome $outcome)" "ok"
	else fail "attacks ($tag): the host's game reports the end (outcome $outcome)" "$(_at_tail host)"; fi
	if wait_for "$ATTACK_SETTLE" _at_log_has vm "SettlementAttacks: the host's attack $q '.*' on $ATTACK_BENCH .*: phase 3, outcome $outcome"; then pass "attacks ($tag): the VM hears the end" "ok"
	else fail "attacks ($tag): the VM hears the end" "$(_at_tail vm)"; fi
	if wait_for "$ATTACK_SETTLE" _at_gone vm "$q"; then pass "attacks ($tag): the VM's mirror stops with the host's run" "$(_at_list vm | cut -c1-100)"
	else fail "attacks ($tag): the VM's mirror stops with the host's run" "$(_at_entry vm "$q" | cut -c1-140); $(_at_tail vm)"; fi
	sleep 2; win_read attacks vm >/dev/null
	# What keeps the VM's save from rolling the host's damage: stage 100 is done before the mirror's shutdown stage runs.
	if _at_order vm "SettlementAttacks: our mirror $q '.*': stage 100" "SettlementAttacks: our mirror $q '.*' at $ATTACK_BENCH has stopped"; then
		pass "attacks ($tag): the VM's mirror took stage 100 before it stopped (no damage rolled in the VM's save)" "$(_rad_done vm "$q")"
	else fail "attacks ($tag): the VM's mirror took stage 100 before it stopped (no damage rolled in the VM's save)" "stages done '$(_rad_done vm "$q")'; $(_at_tail vm)"; fi
	expect_match "attacks ($tag): the VM's stages of the quest have 100" ",$(_rad_done vm "$q")," ",100,"
	expect_ge "attacks ($tag): the VM's net status counts the mirror's start" "$(( $(_at_count vm mirrorStarts) - vs0 ))" 1 start
	expect_ge "attacks ($tag): the VM's net status counts the mirror's end" "$(( $(_at_count vm mirrorsEnded) - ve0 ))" 1 end
	wait_for 20 _at_gone host "$q" || fail "attacks ($tag): the host's run has stopped" "$(_at_entry host "$q" | cut -c1-120)"
	return 0
}
# _at_res <who>: that game's own settlement: its food, water, safety and power, and the caps in its workbench
_at_res() {
	local v k out=""
	v=$(dc "$1" "wsres av $ATTACK_BENCH")
	for k in food water safety power; do out+="$k=$(grep -oE "; $k\[[^]]*\]=-?[0-9.]+" <<<"$v" | grep -oE -- '-?[0-9.]+$') "; done
	echo "${out}caps=$(dc "$1" "count $ATTACK_BENCH 0000000F")"
}
# _at_save_leave_round: the VM, away from the settlement, saves while it mirrors the host's attack and leaves the session (its mirror
# is closed down: timer cancelled, stopped); the host's run ends without it; the VM's save loaded solo holds the mirror defused
# (stage 100 done, 20 not), which is stopped on the load, and no attack plays out at the VM's own settlement. The VM then joins
# the host's lobby again (`steam join`, the host's game left as it is).
_at_save_leave_round() {
	local tag="VM saves and leaves" q r0 r1 d lobby i
	_at_vm_away
	win_mark attacks host; win_mark attacks vm
	q=$(_at_roll_host)
	if [ -z "$q" ]; then fail "attacks ($tag): set-up: the host's game rolls an attack on $ATTACK_NAME" "no attack quest runs at $ATTACK_BENCH: $(_at_list host | cut -c1-160)"; return 1; fi
	if wait_for 45 _at_entry_is vm "$q" "done=true .*mirror\(host phase 0\)"; then pass "attacks ($tag): the VM mirrors the host's attack, defused" "$(_at_entry vm "$q" | cut -c1-140)"
	else fail "attacks ($tag): the VM mirrors the host's attack, defused" "$(_at_entry vm "$q" | cut -c1-140); $(_at_tail vm)"; host "console stopquest $q" >/dev/null; return 1; fi
	r0=$(_at_res vm)
	dc vm "console save f4mpattacks" >/dev/null; sleep 8
	dc vm "net leave" >/dev/null
	if wait_for "$ATTACK_SETTLE" _at_log_has vm "SettlementAttacks: $q '.*' at $ATTACK_BENCH is closed down \(the session ended"; then pass "attacks ($tag): leaving, the VM closes its mirror down (timer cancelled, stopped)" "ok"
	else fail "attacks ($tag): leaving, the VM closes its mirror down (timer cancelled, stopped)" "$(_at_entry vm "$q" | cut -c1-140); $(_at_tail vm)"; fi
	expect_eq "attacks ($tag): no attack quest runs at $ATTACK_NAME in the VM's game after it left" "$(_at_none_at vm && echo none)" "none"
	host "console stopquest $q" >/dev/null   # the host's run ends while the VM is away
	wait_for 20 _at_gone host "$q" || fail "attacks ($tag): the host's run has stopped" "$(_at_entry host "$q" | cut -c1-120)"

	# The save made mid-mirror, loaded solo: the run it holds is stopped at once, its timer cancelled first.
	win_mark attacks vm
	dc vm "console load f4mpattacks" >/dev/null; sleep 20; wait_ingame vm 60
	if wait_for "$ATTACK_SETTLE" _at_log_has vm "SettlementAttacks: the loaded game holds $q '.*' at $ATTACK_BENCH defused by a session"; then pass "attacks ($tag): the VM's save loaded solo holds the mirror defused, and stops it" "ok"
	else fail "attacks ($tag): the VM's save loaded solo holds the mirror defused, and stops it" "$(_at_list vm | cut -c1-160); $(_at_tail vm)"; fi
	if wait_for "$ATTACK_SETTLE" _at_none_at vm; then pass "attacks ($tag): the VM's save plays no attack out: nothing runs at $ATTACK_NAME" "ok"
	else fail "attacks ($tag): the VM's save plays no attack out: nothing runs at $ATTACK_NAME" "$(_at_list vm | cut -c1-160)"; fi
	d=$(_rad_done vm "$q")
	if [[ ",$d," == *",20,"* ]]; then fail "attacks ($tag): the attack never began in the VM's save (stage 20 not done)" "stages done '$d'"
	else pass "attacks ($tag): the attack never began in the VM's save (stage 20 not done)" "stages done '$d'"; fi
	sleep 10; r1=$(_at_res vm)
	expect_eq "attacks ($tag): the VM's own $ATTACK_NAME is as it was" "$r1" "$r0"

	# Back in the session (the host's game left as it is).
	lobby=$(field "$(host 'net status')" lobby)
	if ! is_num "$lobby" || [ "$lobby" = 0 ]; then rig_broken game "attacks ($tag): the VM joins the host's lobby again" "the host hosts no lobby ('$lobby')"; return 1; fi
	vm "steam join $lobby" >/dev/null
	for i in $(seq 1 30); do vm 'net status' | grep -q 'status=joined' && break; sleep 3; done
	if vm 'net status' | grep -q 'status=joined'; then pass "attacks ($tag): the VM joins the host's session again" "ok"
	else rig_broken game "attacks ($tag): the VM joins the host's session again" "$(vm 'net status' | cut -c1-120)"; return 1; fi
	sleep 3
	return 0
}
step_attacks() {
	section "attacks (the host's settlement attack shared: $ATTACK_NAME; console placement, console set-up of the attack's stages)"
	local ok q d0 n r0 r1
	enter_both "$ATTACK_CELL" 14
	n=$(_at_list host); if [[ ! $n =~ hosting=true ]]; then fail "attacks: set-up: the host's game hosts and reads attacks" "'$(cut -c1-160 <<<"$n")' (an old build?)"; return; fi
	expect_match "attacks: set-up: the VM's game is a friend's" "$(_at_list vm)" "hosting=false"
	host "console $ATTACK_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 2
	ok=$(host "scriptvars $ATTACK_BENCH WorkshopScript OwnedByPlayer")
	if [ "$(field "$ok" OwnedByPlayer)" != true ]; then fail "attacks: set-up: $ATTACK_NAME is the host's" "$ok"; return; fi
	if ! _at_none_at host || ! _at_none_at vm; then fail "attacks: set-up: no attack runs at $ATTACK_NAME in either game" "host '$(_at_list host | cut -c1-120)' VM '$(_at_list vm | cut -c1-120)'"; return; fi
	crash_snapshot

	# Round 1: both at the settlement; the host's attack is defended.
	_at_mirror_round "VM there" at || { crash_verdict attacks; return; }

	# Round 2: the VM away in an interior; the host's run ends undefended. The VM's own settlement takes nothing from it: its
	# food, water, safety and power, and the caps put in its workbench first (an attack the attackers win takes from it), are
	# the same after. (A run of the VM's own that rolled would hurt them only when its attackers won the roll: a pass here is
	# no proof on its own; the stage-100 checks are.)
	dc vm "console $ATTACK_BENCH.additem F 500" >/dev/null; sleep 2
	_at_vm_away
	r0=$(_at_res vm)
	_at_mirror_round "VM away" away
	sleep 5; r1=$(_at_res vm)
	if [[ $r0 =~ food=[0-9] ]] && [[ $r0 =~ caps=[1-9] ]]; then expect_eq "attacks (VM away): the VM's own $ATTACK_NAME takes no damage and loses no caps" "$r1" "$r0"
	else fail "attacks (VM away): the VM's own $ATTACK_NAME takes no damage and loses no caps" "couldn't read them: '$r0'"; fi

	# Round 3: the VM's own forced roll at the settlement (the host's): defused, then stopped, its attackers removed; the host's game
	# never takes it as an attack.
	win_mark attacks host; win_mark attacks vm
	d0="$(_at_count vm ownDefused)/$(_at_count vm ownStopped)"
	dc vm "console $ATTACK_BENCH.cf \"WorkshopScript.CheckForAttack\" 1" >/dev/null
	if ! wait_for 20 _at_log_has vm "SettlementAttacks: our own attack [0-9A-F]{8} '.*' on $ATTACK_BENCH "; then
		warn "attacks (VM's own roll): not run: the VM's forced roll at $ATTACK_NAME started no attack quest" "$(_at_list vm | cut -c1-160)"
	else
		q=$(grep -oE "SettlementAttacks: our own attack [0-9A-F]{8}" "$A_OUT/windelta-attacks-vm.txt" | head -1 | grep -oE '[0-9A-F]{8}$')
		if wait_for "$ATTACK_SETTLE" _at_log_has vm "SettlementAttacks: our own attack $q '.*' on $ATTACK_BENCH .* is at the host's settlement.*: stage 100"; then pass "attacks (VM's own roll): defused at the host's settlement (stage 100)" "$q"
		else fail "attacks (VM's own roll): defused at the host's settlement (stage 100)" "$(_at_tail vm)"; fi
		if wait_for "$ATTACK_SETTLE" _at_log_has vm "SettlementAttacks: our own attack $q '.*' on $ATTACK_BENCH .* is stopped, its attackers removed"; then pass "attacks (VM's own roll): stopped, its attackers removed" "ok"
		else fail "attacks (VM's own roll): stopped, its attackers removed" "$(_at_tail vm)"; fi
		if wait_for "$ATTACK_SETTLE" _at_gone vm "$q"; then pass "attacks (VM's own roll): no longer runs in the VM's game" "ok"
		else fail "attacks (VM's own roll): no longer runs in the VM's game" "$(_at_entry vm "$q" | cut -c1-140)"; fi
		expect_match "attacks (VM's own roll): the VM's stages of the quest have 100" ",$(_rad_done vm "$q")," ",100,"
		if _at_log_has host "SettlementAttacks: (our attack|the host's attack) $q"; then fail "attacks (VM's own roll): the host's game takes no attack of the VM's" "it did: $(_at_tail host)"
		else pass "attacks (VM's own roll): the host's game takes no attack of the VM's" "no such line"; fi
		n="$(_at_count vm ownDefused)/$(_at_count vm ownStopped)"
		if [ "$n" != "$d0" ]; then pass "attacks (VM's own roll): the VM's net status counts it" "$d0 -> $n"; else fail "attacks (VM's own roll): the VM's net status counts it" "$d0 -> $n"; fi
	fi

	# Round 4: the VM saves while it mirrors the host's attack and leaves; its save, loaded solo, plays no attack out.
	_at_save_leave_round
	crash_verdict attacks
	dc vm "console coc $ATTACK_CELL" >/dev/null; sleep 12; wait_ingame vm 40
	expect_eq "attacks: no attack runs at $ATTACK_NAME in either game at the end" "$(_at_none_at host && echo none)/$(_at_none_at vm && echo none)" "none/none"
}

# ---- the save's hush in a big settlement (opt-in: smoke.sh --only hush-probe) ------------------------------------------------------------
# Two crash reports from 0.10.1 (a host crashing at one fixed place and on fast travel; a crash entering Sanctuary, 0.10.0 fine) point at
# the one save-time step 0.10.1 added: before every save, each copy of a friend's piece shown powered is shown unpowered and powered
# again after (WorkshopSync::BeforeSave, WorkshopPower::Mirror::Hush), and Sanctuary holds the most. This loads that step as hard as a
# big settlement does, both ways: the builder's game places HP_PIECES powered pieces at Sanctuary (wsreport place, as the workshop step
# does: console placement, said so; the player is moved by setpos between rings of 12), the saver's game takes them as copies and is told
# each is powered as their builder's game would say it (dev `wspower want`: the pieces are no workbench's items in the builder's game, so
# its own power reports never say otherwise), and then, with `wspower hushlog on` (a log line for each copy before the hush touches it),
# the saver makes HP_SAVES saves standing among them and HP_TRAVELS travels out to HP_AWAY and back (console coc: a cell change behind a
# loading screen), each with a save 4 s after arriving, where the game's travel autosave comes (rig, 2026-11-03). After every save both
# games must be up, and at the end no new crash log (Buffout 4 / Addictol's crash-*.log) and no new F4Multiplayer-crash-*.txt (the
# plugin's own report) may be on either machine. Asserted too: each standing save's hush shows at least HP_MIN_HUSHED percent of the
# pieces unpowered, with the hush's per-copy line for each. The pieces are scrapped by their builder at the end (the saver's copies go
# with the report) and the saver's copies of them must be gone. The saves (HP_SAVE) are throwaway; back up the ini and session files
# first (backup.sh save) and restore them after, as for any rig run: the session file holds the pieces until the scrap.
# With PR 379's shared workshop merged (on by default), a piece a friend places at a settlement the host owns becomes the host's piece
# (no copy of it in the host's game for the hush to touch), so both games are set to `wsmode off` for the probe and each is put back
# to its own setting from before after it (_hp_wsmode_off, _hp_wsmode_back); a game without the command (before that merge) is left
# alone.
HP_PIECES=${HP_PIECES:-156}; HP_TRAVELS=${HP_TRAVELS:-30}; HP_SAVES=${HP_SAVES:-10}; HP_MIN_HUSHED=${HP_MIN_HUSHED:-90}
# the construction light and the light box (activators lit by their graphs), the small generator, the light bulb (a LIGH): read from
# Fallout4.esm's records; the set-up checks each with dev `edid`
HP_BASES=${HP_BASES:-"000D38D4 001F59AC 00030386 000560DF"}
HP_AWAY=${HP_AWAY:-RedRocketExt}; HP_SAVE=${HP_SAVE:-f4mp_hushprobe}
HP_X=${HP_X:--80500}; HP_Y=${HP_Y:-89300}; HP_Z=${HP_Z:-7884}; HP_RING=${HP_RING:-300}   # the rings' grid: 4 a row, HP_RING apart
_hp_id() { field "$(dc "$1" 'net status')" id; }
# _hp_ours <who>: the plugin's own crash reports on that machine, "name mtime" a line after a first line "ok" (no "ok": unreadable)
_hp_ours() {
	local f out t
	if [ "$1" = host ]; then
		echo ok; for f in "$HOST_F4SE"/F4Multiplayer-crash-*.txt; do [ -e "$f" ] && echo "$(basename "$f") $(stat -c %Y "$f")"; done | LC_ALL=C sort; return
	fi
	for t in 1 2 3; do
		out=$(ssh -i "$VM_KEY" -o LogLevel=ERROR -o ConnectTimeout=20 "$VM_SSH" '"ok"; Get-ChildItem "$env:USERPROFILE\Documents\My Games\Fallout4\F4SE" -Filter "F4Multiplayer-crash-*.txt" -ErrorAction SilentlyContinue | ForEach-Object { $_.Name + " " + [DateTimeOffset]::new($_.LastWriteTimeUtc).ToUnixTimeSeconds() }' 2>/dev/null | tr -d '\r')
		case "$out" in ok*) echo ok; printf '%s\n' "$out" | grep '^F4Multiplayer-crash-' | LC_ALL=C sort; return;; esac
		sleep 3
	done
}
# _hp_ours_new <before> <after>: the names in <after> not in <before> (a new name, or the same name rewritten); nothing when either
# listing is unreadable (the step stops before the probe when the first one is, so that never counts every old report as new)
_hp_ours_new() {
	case "$1" in ok*) ;; *) return 0;; esac; case "$2" in ok*) ;; *) return 0;; esac
	comm -13 <(printf '%s\n' "$1" | LC_ALL=C sort) <(printf '%s\n' "$2" | LC_ALL=C sort) | grep '^F4Multiplayer-crash-' | cut -d' ' -f1
}
# _hp_fetch <who> <names...>: those files from that machine's F4SE folder copied to $A_OUT/hp-crash/<who>-<name>
_hp_fetch() {
	local w=$1 f; shift; mkdir -p "$A_OUT/hp-crash"
	for f in "$@"; do
		if [ "$w" = host ]; then cp -f "$HOST_F4SE/$f" "$A_OUT/hp-crash/host-$f" 2>/dev/null
		else scp -q -i "$VM_KEY" -o LogLevel=ERROR -o ConnectTimeout=20 "$VM_SSH:$VM_F4SE/$f" "$A_OUT/hp-crash/vm-$f" 2>/dev/null; fi
	done
}
# _hp_crash_reports: the plugin's new crash reports (F4Multiplayer-crash-*.txt, against HP_H0/HP_V0) and the crash logger's new
# crash-*.log (against crash_snapshot's) on both machines, named and copied to $A_OUT/hp-crash. A new report of the plugin's is a FAIL
# here; a new crash log is crash_verdict's FAIL. Once a run: from _hp_up when a game goes down (before _rig_stop ends the run, which
# it does with an exit), else at the end of the step.
_hp_crash_reports() {
	local h1 v1 hn vn hc vc
	[ -n "$HP_CRASH_READ" ] && return 0; HP_CRASH_READ=1
	sleep 5   # a game going down may still be writing its report
	h1=$(_hp_ours host); v1=$(_hp_ours vm)
	case "$v1" in ok*) ;; *) warn "hush-probe: the VM's crash reports (F4Multiplayer-crash-*.txt) are read" "the listing after was unreadable (ssh)";; esac
	hn=$(_hp_ours_new "$HP_H0" "$h1"); vn=$(_hp_ours_new "$HP_V0" "$v1")
	hc=$(crash_names "$(crash_new "$C_HOST0" "$(crash_list_host)" 2>/dev/null)"); vc=$(crash_names "$(crash_new "$C_VM0" "$(crash_list_vm)" 2>/dev/null)")
	# shellcheck disable=SC2086   # names, one a line, never a space in one
	{ [ -z "$hn$hc" ] || _hp_fetch host $hn $hc; [ -z "$vn$vc" ] || _hp_fetch vm $vn $vc; }
	# shellcheck disable=SC2086
	[ -z "$hc$vc" ] || echo "hush-probe: the crash logger's new crash logs (copied to $A_OUT/hp-crash):${hc:+ host $(echo $hc)}${vc:+ vm $(echo $vc)}"
	# shellcheck disable=SC2086
	if [ -z "$hn$vn" ]; then pass "hush-probe: no new crash report of the plugin's on either machine" "host $(grep -c '^F4Multiplayer-crash-' <<<"$h1"), vm $(grep -c '^F4Multiplayer-crash-' <<<"$v1") on disk"
	else fail "hush-probe: no new crash report of the plugin's on either machine" "new:${hn:+ host $(echo $hn)}${vn:+ vm $(echo $vn)} (copied to $A_OUT/hp-crash)"; fi
}
# _hp_scrap [alive]: the pieces placed and not scrapped yet (HP_LIVE: "<builder> <saver> <refs file>") scrapped by their builder, and
# the saver's hushlog put back off. Called on every way out of _hp_direction after
# the placement, by _hp_up before a game down ends the run, and by the step's EXIT trap; once (HP_LIVE is cleared first). Given
# alive's line, a game it says is down is left alone.
_hp_scrap() {
	local b s file r
	[ -n "$HP_LIVE" ] || return 0
	read -r b s file <<<"$HP_LIVE"; HP_LIVE=""
	case "${1:-}" in *"$s=DOWN"*) ;; *) dc "$s" "wspower hushlog off" >/dev/null;; esac
	case "${1:-}" in *"$b=DOWN"*) return 0;; esac
	for r in $(cat "$file" 2>/dev/null); do dc "$b" "wsreport $r $SANCT_BENCH scrap" >/dev/null; dc "$b" "remove $r" >/dev/null; done
}
# _hp_wsmode_off: PR 379's shared workshop turned off in both games for the probe (`wsmode off`), each game's own setting before it
# (dev=on|off|host, as plain `wsmode` shows it) kept in HP_WS for _hp_wsmode_back. A game whose `wsmode` shows no dev= (a build
# without the command, before 379) is left alone: nothing to turn off, nothing to put back.
_hp_wsmode_off() {
	local w m; HP_WS=""
	for w in host vm; do
		m=$(field "$(dc "$w" wsmode)" dev)
		case "$m" in on|off|host) dc "$w" "wsmode off" >/dev/null; HP_WS="$HP_WS $w=$m";; esac
	done
	echo "hush-probe: the shared workshop off for the probe:${HP_WS:- no wsmode command in either game, nothing to do}"
}
# _hp_wsmode_back [alive]: each game's shared-workshop setting from before the probe put back (`wsmode on|off|host`); once (HP_WS is
# cleared first). Given alive's line, a game it says is down is left alone. Called at the step's end, by _hp_up before a game down
# ends the run, and by the step's EXIT trap.
_hp_wsmode_back() {
	local saved=$HP_WS p w; HP_WS=""
	for p in $saved; do
		w=${p%%=*}
		case "${1:-}" in *"$w=DOWN"*) continue;; esac
		dc "$w" "wsmode ${p#*=}" >/dev/null
	done
}
# _hp_copies <saver> <builder id> <refs file>: how many of the builder's pieces in the file the saver keeps a copy of
_hp_copies() { dc "$1" "wspower copies" | grep -oE "=$2:[0-9A-F]{8}" | cut -d: -f2 | sort -u | comm -12 - <(sort -u "$3") | wc -l; }
_hp_copies_at_least() { [ "$(_hp_copies "$1" "$2" "$3")" -ge "$4" ]; }
_hp_copies_none() { [ "$(_hp_copies "$1" "$2" "$3")" = 0 ]; }
# _hp_up <label>: both games up after a save. A game down ends the probe (and the run), but first the crash reports are read and
# copied and the pieces scrapped in the builder's game if it is still up: _rig_stop ends with an exit
_hp_up() {
	local a; a=$(alive)
	case "$a" in *DOWN*)
		fail "$1: both games up" "$a: a game went down (a crash or a hang)"
		_hp_crash_reports; _hp_scrap "$a"; _hp_wsmode_back "$a"
		[ -z "$A_RIG_STOPPED" ] && _rig_stop "a game went down in the hush probe" "$a after $1"; return 1;; esac
	return 0
}
# _hp_save <saver> <label>: a save; its hush's line from the saver's log goes to $A_OUT/hp-hushes-<saver>.txt. Prints "<hushed> <lines>":
# how many copies the hush showed unpowered ("-" when the line is missing) and how many per-copy lines it logged
_hp_save() {
	local d line n
	win_mark hp "$1"
	dc "$1" "console save $HP_SAVE" >/dev/null; sleep 6; wait_ingame "$1" 30
	d=$(win_read hp "$1")
	line=$(grep -E "WorkshopSync: [0-9]+ friends' pieces shown unpowered for the save \(" "$d" | tail -1)
	echo "$2: ${line:-no hush line}" >> "$A_OUT/hp-hushes-$1.txt"
	n=$(sed -nE "s/.*WorkshopSync: ([0-9]+) friends' pieces shown unpowered for the save \(.*/\1/p" <<<"$line")
	echo "${n:--} $(grep -c "WorkshopSync: hush: friend's piece" "$d")"
}
# _hp_build <builder> <refs file>: HP_PIECES pieces placed in rings of 12 around the builder's player, the bases in turn; prints how many
_hp_build() {
	local who=$1 file=$2 i=0 k=0 a out ref bases=($HP_BASES)
	: > "$file"
	while [ "$i" -lt "$HP_PIECES" ]; do
		dc "$who" "console player.setpos x $((HP_X + (k % 4) * HP_RING))" >/dev/null; dc "$who" "console player.setpos y $((HP_Y + (k / 4) * HP_RING))" >/dev/null
		dc "$who" "console player.setpos z $HP_Z" >/dev/null; sleep 1
		for a in 0 30 60 90 120 150 180 210 240 270 300 330; do
			[ "$i" -ge "$HP_PIECES" ] && break
			dc "$who" "console player.setangle z $a" >/dev/null
			out=$(dc "$who" "wsreport place ${bases[$((i % ${#bases[@]}))]} $SANCT_BENCH"); ref=$(grep -oE "reported [0-9A-F]{8}" <<<"$out" | cut -c10-)
			[ -n "$ref" ] && echo "$ref" >> "$file"
			i=$((i + 1))
		done
		k=$((k + 1))
	done
	wc -l < "$file" | tr -d ' '
}
# _hp_direction <builder> <saver>: one way round
_hp_direction() {
	local b=$1 s=$2 label="hush-probe ($2 saves among $1's pieces)" bid bhex file placed r n lines want i t="" ok=0 short="" refused
	file="$A_OUT/hp-placed-$b.txt"; : > "$A_OUT/hp-hushes-$s.txt"
	bid=$(_hp_id "$b"); [ -n "$bid" ] || { fail "$label: set-up: the builder's player id is read" "$(dc "$b" 'net status' | cut -c1-120)"; return 1; }
	bhex=$(printf '%X' "$bid")
	enter_both SanctuaryExt 14
	HP_LIVE="$b $s $file"   # from here on every way out scraps what was placed (_hp_scrap)
	placed=$(_hp_build "$b" "$file")
	expect_ge "$label: set-up: the $b placed the pieces at Sanctuary (console placement)" "$placed" "$HP_PIECES"
	want=$((placed * 95 / 100))
	if wait_for 90 _hp_copies_at_least "$s" "$bid" "$file" "$want"; then pass "$label: set-up: the $s keeps copies of them" "$(_hp_copies "$s" "$bid" "$file") of $placed"
	else fail "$label: set-up: the $s keeps copies of them" "$(_hp_copies "$s" "$bid" "$file") of $placed (want $want)"; _hp_scrap; return 1; fi
	n=0; for r in $(cat "$file"); do case "$(dc "$s" "wspower want $bhex $r on")" in ok*) n=$((n + 1));; esac; done
	expect_eq "$label: set-up: the $s is told each piece is powered (wspower want)" "$n" "$placed"
	dc "$s" "wspower hushlog on" >/dev/null; sleep 5
	echo "$label: $(dc "$s" wspower | grep -oE '(mirrored|shown|graphWaits|graphRefused)=[0-9]+' | tr '\n' ' ')"
	# standing among them
	for i in $(seq 1 "$HP_SAVES"); do
		read -r n lines <<<"$(_hp_save "$s" "standing save $i")"
		_hp_up "$label: standing save $i" || { _hp_scrap; return 1; }
		if [ "$n" = - ] || [ $((n * 100)) -lt $((placed * HP_MIN_HUSHED)) ]; then short="$short $i:$n"
		elif [ "$lines" -lt "$n" ]; then short="$short $i:lines=$lines<$n"; fi
		sleep 4
	done
	if [ -z "$short" ]; then pass "$label: each of $HP_SAVES standing saves hushed at least $HP_MIN_HUSHED% of the $placed pieces, a log line for each" "$(tail -1 "$A_OUT/hp-hushes-$s.txt" | cut -c1-220)"
	else fail "$label: each of $HP_SAVES standing saves hushed at least $HP_MIN_HUSHED% of the $placed pieces, a log line for each" "short (save:hushed):$short; $(tail -1 "$A_OUT/hp-hushes-$s.txt" | cut -c1-200)"; fi
	# travels out and back, a save 4 s after each arrival (where the travel autosave comes)
	for i in $(seq 1 "$HP_TRAVELS"); do
		if [ $((i % 2)) = 1 ]; then t=$HP_AWAY; else t=SanctuaryExt; fi
		dc "$s" "console coc $t" >/dev/null; sleep 6; wait_ingame "$s" 60; sleep 4
		_hp_save "$s" "travel $i to $t" >/dev/null
		_hp_up "$label: travel $i to $t and its save" || { _hp_scrap; return 1; }
		ok=$((ok + 1))
	done
	expect_eq "$label: travels made, each with a save after it, both games up after each" "$ok" "$HP_TRAVELS"
	echo "$label: the hush at each save ($A_OUT/hp-hushes-$s.txt):"; cut -c1-240 "$A_OUT/hp-hushes-$s.txt" | sed 's/^/  /'
	refused=$(sed -nE 's/.* ([0-9]+) the game refused.*/\1/p' "$A_OUT/hp-hushes-$s.txt" | awk '{n+=$1} END{print n+0}')
	if [ "$refused" = 0 ]; then pass "$label: no hush the game refused" "0"; else warn "$label: no hush the game refused" "$refused over all the saves"; fi
	# back at Sanctuary, the builder scraps its pieces; the saver's copies go with the reports
	[ "$t" = SanctuaryExt ] || { dc "$s" "console coc SanctuaryExt" >/dev/null; sleep 6; wait_ingame "$s" 60; }
	_hp_scrap
	if wait_for 60 _hp_copies_none "$s" "$bid" "$file"; then pass "$label: the pieces scrapped, the $s keeps no copy of them" "0 of $placed"
	else fail "$label: the pieces scrapped, the $s keeps no copy of them" "$(_hp_copies "$s" "$bid" "$file") of $placed left"; fi
	return 0
}
step_hush_probe() {
	section "the save's hush among $HP_PIECES powered pieces of a friend's at Sanctuary, both ways (console placement and coc travel; throwaway saves)"
	local o prev
	for o in $HP_BASES; do
		case "$(host "edid $o")" in error*|"") fail "hush-probe: set-up: base $o is in the host's game" "$(host "edid $o")"; return;; esac
	done
	crash_snapshot; HP_H0=$(_hp_ours host); HP_V0=$(_hp_ours vm); HP_CRASH_READ=""; HP_LIVE=""; HP_WS=""
	# without the VM's listings before, a report or log the probe made can't be told from an old one: the rig is broken, no probe
	case "$HP_V0" in ok*) ;; *) rig_broken ssh "hush-probe: the VM's crash reports (F4Multiplayer-crash-*.txt) are read before the probe" "the listing over ssh failed three times: no probe"; return;; esac
	case "$C_VM0" in ok*) ;; *) rig_broken ssh "hush-probe: the VM's crash logs (crash-*.log) are read before the probe" "the listing over ssh failed three times: no probe"; return;; esac
	prev=$(trap -p EXIT); trap '_hp_scrap; _hp_wsmode_back' EXIT   # a run ended part-way (an interrupt, the circuit breaker) still scraps the pieces
	_hp_wsmode_off   # PR 379's shared workshop: the builder's pieces stay its own at Sanctuary, so the saver keeps copies to hush
	_hp_direction vm host && _hp_direction host vm
	_hp_scrap; _hp_wsmode_back; if [ -n "$prev" ]; then eval "$prev"; else trap - EXIT; fi
	_hp_crash_reports
	crash_verdict "hush-probe"
}

# attacks-addons (opt-in, --only attacks-addons): the add-ons' settlement attacks, one of each add-on installed on both machines
# (ATTACK_ADDON_QUESTS), each started on the host's settlement (the attacks step's: Red Rocket, ATTACK_BENCH, ATTACK_CELL and
# ATTACK_NAME) by `radiantstart` with its Workshop alias and location forced (the story manager's own roll there picks only
# Fallout4.esm's, and Nuka-World's run only on a raider outpost), then round 1 of the attacks step with the VM there: the host
# reports it, the VM mirrors it at the same bench, takes stage 100 as it runs, is shown the objective with the host's attackers
# copied in, and stops with the host's run. A quest whose start doesn't run there (a location alias of its own finding nothing
# there) is a WARN, not a FAIL; so are the objective, copies and
# completion checks of a round whose host shared no attacker in 15 s after its 20 (nothing there to show: _at_mirror_round). Throwaway saves (the settlement owned).
# Not in the default list: the Brotherhood's vertibird attack (DLC04WorkshopBOSAttack02), whose start never reaches its 20/22 (the bird
# and its crew) on the rig and so only ever WARNed; a mirror never reaches them anyway (docs/DESIGN.md). Still runnable by naming it in
# ATTACK_ADDON_QUESTS.
ATTACK_ADDON_QUESTS=${ATTACK_ADDON_QUESTS:-"DLC03WorkshopTrapperAttack01 DLC01WorkshopBotRaiderAttack02 DLC04WorkshopMinutemenAttack01"}
step_attacks_addons() {
	section "attacks-addons (the add-ons' settlement attacks shared: one each, started on the host's $ATTACK_NAME by radiantstart; console set-up)"
	local ok q n ran=0
	enter_both "$ATTACK_CELL" 14
	n=$(_at_list host); if [[ ! $n =~ hosting=true ]]; then fail "attacks-addons: set-up: the host's game hosts and reads attacks" "'$(cut -c1-160 <<<"$n")' (an old build?)"; return; fi
	expect_match "attacks-addons: set-up: the VM's game is a friend's" "$(_at_list vm)" "hosting=false"
	host "console $ATTACK_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 2
	ok=$(host "scriptvars $ATTACK_BENCH WorkshopScript OwnedByPlayer")
	if [ "$(field "$ok" OwnedByPlayer)" != true ]; then fail "attacks-addons: set-up: $ATTACK_NAME is the host's" "$ok"; return; fi
	if ! _at_none_at host || ! _at_none_at vm; then fail "attacks-addons: set-up: no attack runs at $ATTACK_NAME in either game" "host '$(_at_list host | cut -c1-120)' VM '$(_at_list vm | cut -c1-120)'"; return; fi
	crash_snapshot
	for q in $ATTACK_ADDON_QUESTS; do
		if [ -z "$(_at_form_of host "$q")" ] || [ -z "$(_at_form_of vm "$q")" ]; then
			warn "attacks-addons ($q): its add-on is installed on both machines" "not run: host '$(_at_form_of host "$q")' VM '$(_at_form_of vm "$q")' (the game's 'forms 80 $q')"; continue
		fi
		ATTACK_QUEST=$q _at_mirror_round "$q" at optional; case $? in 1) crash_verdict attacks; return;; 0) ran=$((ran + 1));; esac
		sleep 3
	done
	expect_ge "attacks-addons: at least one add-on's attack ran" "$ran" 1 quest
	crash_verdict attacks
	expect_eq "attacks-addons: no attack runs at $ATTACK_NAME in either game at the end" "$(_at_none_at host && echo none)/$(_at_none_at vm && echo none)" "none/none"
}

# ---- quests_outcomes: the add-ons' endings and kick-outs, the chooser's for both (opt-in: smoke.sh --only quests_outcomes) -------------------
# With the host's sQuests = safe, the quests whose stages decide a faction's or a settlement's fate are shared (tools/questscan.cs
# ALLOW_OUTCOMES): the player who chooses decides for both, and the friend gets the outcome once near them (a stage that turns a
# faction on the player is StageRules' kMoves|kHostile). One check per ending: the chooser's console sets the ending's stages, the other
# game must have them done within QOUT_WAIT s, its player where it stood (nothing of these moves the player). Both players stand in
# SanctuaryExt (console placement, said so in the report), near each other, so a kMoves stage is set at once; the far case is the follow
# step's. The chooser alternates, the host for the first ending, the VM for the second, and so on; QOUT_SWAP=1 flips it, so a run with it
# and one without, each on fresh throwaway saves, check every ending both ways. Quest stages are for good in a save, and these end the
# add-ons' stories: run it on throwaway saves (backup.sh before, restore after); an ending already done in either game is a FAIL naming it.
# ending: <editor ID>:<objective shown first, or ->:<stages set in order, the last the ending's>. A SAFE quest (objectives) is shared
# only once the chooser has started it, so its objective is shown first; a SILENT one (none) is started in the other game by the stage.
# The Children of Atom's kick-out comes before the nuke, whose 100 sets the kick-out's 700 as well.
# The nuke's 100 calls Game.RequestSave in both games (its fragment), so each writes an autosave mid-run with the ending done, and its
# timers set 140 and 150 (the blast) a little later in both. Never continue from that autosave: the rig's next run, or any step after
# this one, must load the throwaway save by name or the backed-up saves restored (backup.sh), never the newest save.
QOUT_ENDINGS=${QOUT_ENDINGS:-"DLC03AtomM01_CombatStop:-:1,100;DLC03CoA_Kickout:-:700;DLC03AcadiaDefense:-:100;DLC03MQ06_WindFarmHandling:-:50;DLC03MQ06_NukeHandling:-:100;DLC04SafariAdventureQuest:10:10,175;DLC04RaiderKickout:10:20"}
QOUT_WAIT=${QOUT_WAIT:-30}; QOUT_SWAP=${QOUT_SWAP:-0}
_qo_id() { dc "$1" "quests edid:$2" | tr ';' '\n' | awk -v e="$2" '$2 == e { print $1; exit }'; }
_qo_done() { dc "$1" "stages $2" | sed -nE 's/.* done\([0-9]+\)=([0-9,]+|none).*/\1/p'; }
_qo_has() { case ",$(_qo_done "$1" "$2")," in *",$3,"*) return 0;; esac; return 1; }
_qo_all() { local s; for s in ${3//,/ }; do _qo_has "$1" "$2" "$s" || return 1; done; return 0; }   # _qo_all <who> <quest id> <stages,...>
step_quests_outcomes() {
	section "quests_outcomes (the add-ons' endings and kick-outs: the chooser's console setstage, the other game the same; console placement)"
	local entries entry edid obj stages id ids="" line s i=0 from to pos0 moved
	IFS=';' read -ra entries <<<"$QOUT_ENDINGS"
	for entry in "${entries[@]}"; do
		IFS=':' read -r edid obj stages <<<"$entry"
		id=$(_qo_id host "$edid")
		if [ -z "$id" ] || [ "$id" != "$(_qo_id vm "$edid")" ]; then fail "quests_outcomes: set-up: both games have $edid (the add-on loaded, the same form ID)" "host '$id', VM '$(_qo_id vm "$edid")'"; return; fi
		line=$(dc host "stages $id")
		case "$line" in *"sQuests = safe"*) ;; *) fail "quests_outcomes: set-up: the host shares $edid (sQuests = safe, a shared story)" "got '$line'"; return;; esac
		s=${stages##*,}
		if _qo_has host "$id" "$s" || _qo_has vm "$id" "$s"; then fail "quests_outcomes: set-up: neither game has $edid's ending $s done (a throwaway save from before it)" "host $(_qo_done host "$id"), VM $(_qo_done vm "$id")"; return; fi
		ids="$ids $id"
	done
	enter_both SanctuaryExt 14
	read -ra ids <<<"$ids"
	for entry in "${entries[@]}"; do
		IFS=':' read -r edid obj stages <<<"$entry"; id=${ids[$i]}
		if [ $(( (i + QOUT_SWAP) % 2 )) = 0 ]; then from=host; to=vm; else from=vm; to=host; fi
		i=$((i + 1))
		pos0=$(dc $to pos)
		[ "$obj" != - ] && { dc $from "console setobjectivedisplayed $edid $obj 1" >/dev/null; sleep 2; }   # set-up: the chooser has the quest started
		for s in ${stages//,/ }; do dc $from "console setstage $edid $s" >/dev/null; sleep 3; done
		if ! wait_for 10 _qo_all $from "$id" "$stages"; then fail "quests_outcomes ($edid): set-up: the $from's console set ${stages//,/ then }" "$from done $(_qo_done $from "$id")"; continue; fi
		if wait_for "$QOUT_WAIT" _qo_all $to "$id" "$stages"; then pass "quests_outcomes ($edid): the $from's ${stages//,/ and } is set in the $to's game" "$from done $(_qo_done $from "$id"), $to $(_qo_done $to "$id")"
		else fail "quests_outcomes ($edid): the $from's ${stages//,/ and } is set in the $to's game" "$from done $(_qo_done $from "$id"), $to $(_qo_done $to "$id")"; fi
		moved=$(_q_moved "$pos0" "$(dc $to pos)")
		if is_num "$moved"; then expect_le "quests_outcomes ($edid): the $to's player stays where it was" "$moved" 64 u
		else fail "quests_outcomes ($edid): the $to's player stays where it was" "no position read"; fi
	done
}

# ---- companion_quests: Curie's and MacCready's quests shared (opt-in: smoke.sh --only companion_quests) ---------------------------------
# A companion's quest is shared as a side quest; the stages whose script is its owner's (StageRules kEach: recruits, dismissals,
# affinity, the companion's perks) are marked done in the friend's game with only their EACH acts done (objectives, the world's
# half), src/game/QuestSync.cpp SetEachHere. Set-up by console, said so in the report: both games coc to SanctuaryExt; the
# stages are set by console setstage in the owner's game (what is under test is the friend's game's handling of them, not the
# dialogue that sets them in play). Quest stages are for good in a save: run it on throwaway saves (backup.sh before, restore
# after); a game with either quest's stages already done is a FAIL naming them.
# Curie (the host the owner): 100 starts it in the VM's game; the VM holds Codsworth as its companion (console recruit), and the
# host's 200 (ClearCompanion, DisallowCompanion in its script) leaves him with the VM: the per-stage skip; the host's 200 put her
# in DisallowedCompanionFaction (the mark of the game that drives her quest), the VM's didn't (`stages`: drive=scripts in the host's game,
# drive=mirror in the VM's); after the host's 300 (her reset) NewCurieBody holds her there and not in the VM's, the mark a reload
# before 600 keeps. Both games coc to the Memory Den
# and Curie is put by the player in each (console moveto: loaded near both players for 300, as at the lab); in the VM, by
# console, she is made met (HasBeenCompanionFaction), given an affinity and a stack of rounds. 300, her new body, sets the VM's
# globals her base reads (synth 100, robot 0), enables the memory lounger, moves her items into the robot body (alias
# FakeCurieRobot), and gives her new body back her affinity and her factions. In both games her new body is the synth (`rolls
# show`: her templates; no session roll of her kept to force the robot back). The VM saves and loads: 300 is still done, not set
# again (one "without its script" line), and she is still the synth. 600 completes the quest in the VM's game while her affinity's global there
# stays 0 (the host's is 1).
# MacCready (the VM the owner): the VM's 30 (the hire, by console: the mark of the driving game) is never set in the host's game;
# 60 starts it in the host's game; 120 leaves the host's affinity global at 0; the VM's game is then killed (the owner leaving
# mid-quest) and the host carries on with its own 140 (its script runs: its own stage); the VM's game restarts and rejoins and
# catches 140 up as a plain shared stage; its game drives his quest (`stages`) when its save holds the hire, mirrors it when it
# doesn't, and a 120 caught up is set accordingly; the host's coc to FOLLOW_HOST_CELL
# puts it far, and the VM's 150 (a stage that moves nobody) is set in the host's game without a follow offer; the VM's 255
# completes it in the host's game but leaves it running (his hire's greeting stays in the host's own save), his second
# affinity global there 0. A friend recruiting the companion once the 5-minute hold lapses is a hand test.
CQ_CURIE=${CQ_CURIE:-0016454E}; CQ_MAC=${CQ_MAC:-00027411}
CQ_CURIE_SYNTH=0010BD4C; CQ_CURIE_ROBOT=001846CD; CQ_CURIE_AFFINITY=00204C21; CQ_LOUNGER=0017FC1C
CQ_MAC_AFFINITY_A=001C53D5; CQ_MAC_AFFINITY_B=000576D9
CQ_SYNTH_NPC=001647C6; CQ_ROBOT_NPC=001846CB   # the NPCs LCharCurie rolls by her globals: her synth body, her robot one
CQ_CURIE_REF=00102249; CQ_DISALLOWED=00075D56; CQ_HAS_BEEN=000A1B85; CQ_CA_AFFINITY=000A1B80; CQ_LAB_CELL=${CQ_LAB_CELL:-GoodneighborTheMemoryDen}
CQ_AFF=${CQ_AFF:-321}; CQ_MARK_ITEM=0001F276; CQ_MARK_N=37   # her affinity and a stack of 10mm rounds, given in the VM before 300
CQ_SET=${CQ_SET:-30}   # s, for a stage set in one game to be done in the other
_cq_log_has() { grep -qE "$2" "$(win_read companion_quests "$1")"; }   # _cq_log_has <who> <regex>: the game's log since win_mark has it
_cq_line() { grep -oE "$2" "$(win_read companion_quests "$1")" | head -1; }
_cq_global() { dc "$1" "global $2" | awk '{print $2}'; }
_cq_completed() {  # _cq_completed <who> <quest>: the quest's QUEST_COMPLETED flag (0x2) in that game
	local f; f=$(dc "$1" "stages $2" | sed -nE 's/.*flags=([0-9A-F]+).*/\1/p')
	[ -n "$f" ] && [ $(( 16#$f & 2 )) -ne 0 ]
}
_cq_factions() { dc "$1" "factions $CQ_CURIE_REF" | grep -oE '[0-9A-F]{8}:-?[0-9]+' | sort | tr '\n' ' '; }   # her faction changes, sorted
_cq_running() { local f; f=$(dc "$1" "stages $2" | sed -nE 's/.*flags=([0-9A-F]+).*/\1/p'); [ -n "$f" ] && [ $(( 16#$f & 1 )) -ne 0 ]; }   # QUEST_ENABLED (0x1)
_cq_count() { grep -cE "$2" "$(win_read companion_quests "$1")"; }
_cq_synth() {  # _cq_synth <who> <when>: Curie built as her synth body in that game (rolls show: her templates), no session roll kept of her
	local r; r=$(dc "$1" "rolls show $CQ_CURIE_REF")
	if echo "$r" | grep -qE "templates=[0-9A-F,]*$CQ_SYNTH_NPC" && ! echo "$r" | grep -qE "templates=[0-9A-F,]*$CQ_ROBOT_NPC" && echo "$r" | grep -q " session=none"; then
		pass "companion_quests: $2 Curie is her synth body in $1's game, not a stored roll of the robot" "$(echo "$r" | cut -c1-200)"
	else fail "companion_quests: $2 Curie is her synth body in $1's game, not a stored roll of the robot" "$(echo "$r" | cut -c1-240)"; fi
}
_cq_set() {  # _cq_set <owner> <friend> <quest> <stage>: the owner's console sets it; within CQ_SET s the friend's game has it done
	dc "$1" "console setstage $3 $4" >/dev/null
	if ! wait_for 15 _f_has "$1" "$3" "$4"; then fail "companion_quests: set-up: $1's console set $3 $4" "$1 done $(_f_done "$1" "$3")"; return 1; fi
	if wait_for "$CQ_SET" _f_has "$2" "$3" "$4"; then pass "companion_quests: $3 stage $4 from $1 is done in $2's game" "done $(_f_done "$2" "$3")"
	else fail "companion_quests: $3 stage $4 from $1 is done in $2's game" "$2 done $(_f_done "$2" "$3")"; return 1; fi
}
step_companion_quests() {
	section "companion_quests (Curie's and MacCready's quests; console placement, console setstage and a console recruit for set-up)"
	local w q s kind fac0 fac1 robot n
	for q in "$CQ_CURIE" "$CQ_MAC"; do
		for w in host vm; do
			kind=$(dc "$w" "stages $q")
			case "$kind" in *"shared=companion"*) ;; *) fail "companion_quests: set-up: $w reads $q as a companion's quest" "got '$kind'"; return;; esac
		done
	done
	for w in host vm; do
		if [ "$(_f_done "$w" "$CQ_CURIE")" != none ]; then fail "companion_quests: set-up: no stage of Curie's quest done in $w's game (a throwaway save from before it)" "done $(_f_done "$w" "$CQ_CURIE")"; return; fi
		for s in 30 40 50 60 120 140 150 255; do
			if _f_has "$w" "$CQ_MAC" "$s"; then fail "companion_quests: set-up: MacCready's quest not past its start in $w's game" "done $(_f_done "$w" "$CQ_MAC")"; return; fi
		done
	done
	enter_both SanctuaryExt 14
	crash_snapshot

	# Curie, the host the owner.
	win_mark companion_quests vm
	_cq_set host vm "$CQ_CURIE" 100 || return
	if ! wait_for 20 _ch_free_both; then fail "companion_quests: set-up: the Companion alias empty in both games" "host '$(_ch_held host)', VM '$(_ch_held vm)'"; return; fi
	_ch_recruit vm
	if ! wait_for 20 _ch_holds vm; then fail "companion_quests: set-up: the VM's console recruit of Codsworth" "alias '$(_ch_held vm)'"; return; fi
	_cq_set host vm "$CQ_CURIE" 200
	if wait_for 10 _cq_log_has vm "QuestSync: set $CQ_CURIE .* stage 200 .*without its script"; then pass "companion_quests: the VM's 200 is marked done without its script (kEach)" "$(_cq_line vm "QuestSync: set $CQ_CURIE .* stage 200 .*")"
	else fail "companion_quests: the VM's 200 is marked done without its script (kEach)" "no such line in the VM's log"; fi
	sleep 5
	expect_eq "companion_quests: Curie's 200 (ClearCompanion in its script) leaves the VM's companion with it" "$(_ch_held vm)" "$COMPANION_HUMAN"
	_ch_dismiss vm; sleep 4
	expect_match "companion_quests: the host's 200 put Curie in DisallowedCompanionFaction (its game drives her quest)" "$(dc host "factions $CQ_CURIE_REF")" " $CQ_DISALLOWED:"
	if dc vm "factions $CQ_CURIE_REF" | grep -q " $CQ_DISALLOWED:"; then fail "companion_quests: the VM's mirrored 200 left Curie out of DisallowedCompanionFaction" "$(dc vm "factions $CQ_CURIE_REF")"
	else pass "companion_quests: the VM's mirrored 200 left Curie out of DisallowedCompanionFaction" "$(dc vm "factions $CQ_CURIE_REF")"; fi
	expect_match "companion_quests: after its 200 the host's game drives Curie's quest (the faction's mark)" "$(dc host "stages $CQ_CURIE")" "drive=scripts marked=true"
	expect_match "companion_quests: after its mirrored 200 the VM's game mirrors Curie's quest" "$(dc vm "stages $CQ_CURIE")" "drive=mirror marked=false"
	# Her new body near both players (console placement): the Memory Den, Curie put by the player in each game.
	enter_both "$CQ_LAB_CELL" 14
	for w in host vm; do dc "$w" "console $CQ_CURIE_REF.moveto player" >/dev/null; done
	sleep 4
	dc vm "console $CQ_CURIE_REF.addtofaction $CQ_HAS_BEEN 0" >/dev/null
	dc vm "console $CQ_CURIE_REF.setav CA_Affinity $CQ_AFF" >/dev/null
	dc vm "console $CQ_CURIE_REF.additem $CQ_MARK_ITEM $CQ_MARK_N" >/dev/null
	sleep 2
	expect_eq "companion_quests: set-up: Curie's affinity in the VM's game (console)" "$(dc vm "av $CQ_CURIE_REF $CQ_CA_AFFINITY")" "$CQ_AFF"
	expect_match "companion_quests: set-up: Curie holds the rounds in the VM's game (console)" "$(dc vm "contents $CQ_CURIE_REF")" " ${CQ_MARK_ITEM}x$CQ_MARK_N"
	fac0=$(_cq_factions vm)
	_cq_set host vm "$CQ_CURIE" 300
	sleep 7   # the reset follows the removal on a later poll (StageRules::RESET_WAIT at the latest), the dressing a second after it (BODY_WAIT)
	if _cq_log_has vm "QuestSync: set $CQ_CURIE .* stage 300 .*her body changes"; then pass "companion_quests: the VM's game gives her a new body (met, not following it)" "$(_cq_line vm "QuestSync: set $CQ_CURIE .* stage 300 .*")"
	else fail "companion_quests: the VM's game gives her a new body (met, not following it)" "$(_cq_line vm "QuestSync: set $CQ_CURIE .* stage 300 .*")"; fi
	if _cq_log_has vm "QuestSync: $CQ_CURIE_REF's new body dressed"; then pass "companion_quests: her new body is dressed in the VM's game" "$(_cq_line vm "QuestSync: $CQ_CURIE_REF's new body dressed.*")"
	else fail "companion_quests: her new body is dressed in the VM's game" "no such line in the VM's log"; fi
	expect_eq "companion_quests: her new body: the VM's synth global" "$(_cq_global vm $CQ_CURIE_SYNTH)" "100"
	expect_eq "companion_quests: her new body: the VM's robot global" "$(_cq_global vm $CQ_CURIE_ROBOT)" "0"
	if _cq_log_has vm "QuestSync: set $CQ_CURIE .* stage 300 .*could not be"; then fail "companion_quests: every act of her new body is done in the VM's game" "$(_cq_line vm "QuestSync: set $CQ_CURIE .* stage 300 .*")"
	else pass "companion_quests: every act of her new body is done in the VM's game" "$(_cq_line vm "stage 300 .*acts.*")"; fi
	expect_match "companion_quests: the memory lounger is on in the VM's game (formFlags bit 0x800 clear)" "$(field "$(dc vm "refinfo $CQ_LOUNGER")" formFlags)" "^[0-9A-F]*[0-7][0-9A-F]{2}$"
	expect_eq "companion_quests: her new body keeps her affinity in the VM's game" "$(dc vm "av $CQ_CURIE_REF $CQ_CA_AFFINITY")" "$CQ_AFF"
	fac1=$(_cq_factions vm)
	expect_eq "companion_quests: her new body keeps her factions in the VM's game" "$fac1" "$fac0"
	# Her new body is rolled by the globals her 300 set, in both games (RollHook leaves her to the engine's roll): the synth.
	for w in host vm; do _cq_synth "$w" "after 300"; done
	# The host's own 300 resets her actor, which may take her out of DisallowedCompanionFaction; the mark a reload from between
	# its 300 and 600 keeps is NewCurieBody (which only its 300's script fills) holding her. Its drive stays the script's.
	echo "-- the host's Curie after its own 300 (her reset): $(dc host "factions $CQ_CURIE_REF")"
	expect_eq "companion_quests: the host's 300 put Curie in NewCurieBody" "$(_qa_aliases host "$CQ_CURIE" NewCurieBody | awk '{print $2; exit}')" "$CQ_CURIE_REF"
	if [ -n "$(_qa_aliases vm "$CQ_CURIE" NewCurieBody)" ]; then fail "companion_quests: the VM's mirrored 300 left NewCurieBody empty" "$(_qa_aliases vm "$CQ_CURIE" NewCurieBody)"
	else pass "companion_quests: the VM's mirrored 300 left NewCurieBody empty" "empty"; fi
	expect_match "companion_quests: after its 300 the host's game still drives Curie's quest (what a reload before 600 keeps)" "$(dc host "stages $CQ_CURIE")" "drive=scripts marked=true"
	expect_match "companion_quests: after its mirrored 300 the VM's game still mirrors Curie's quest" "$(dc vm "stages $CQ_CURIE")" "drive=mirror marked=false"
	robot=$(_qa_aliases vm "$CQ_CURIE" FakeCurieRobot | awk '{print $2; exit}')
	if [ -z "$robot" ]; then fail "companion_quests: her items are in the robot body in the VM's game" "no FakeCurieRobot in the VM's aliases: $(dc vm "aliases $CQ_CURIE" | cut -c1-200)"
	else expect_match "companion_quests: her items are in the robot body in the VM's game" "$(dc vm "contents $robot")" " ${CQ_MARK_ITEM}x$CQ_MARK_N"; fi
	if dc vm "contents $CQ_CURIE_REF" | grep -q " ${CQ_MARK_ITEM}x"; then fail "companion_quests: her new body holds none of them" "$(dc vm "contents $CQ_CURIE_REF" | cut -c1-200)"
	else pass "companion_quests: her new body holds none of them" "$(dc vm "contents $CQ_CURIE_REF" | cut -c1-120)"; fi
	# The done bit is the save's: the VM saves and loads, 300 is still done there, and the session's 300 isn't set again.
	dc vm "console save f4mpcq" >/dev/null; sleep 6; dc vm "console load f4mpcq" >/dev/null; sleep 20
	if ! wait_ingame vm 60; then rig_broken game "companion_quests: the VM's game loads its save after 300" "$(dc vm status | cut -c1-120)"; return; fi
	sleep 10
	if _f_has vm "$CQ_CURIE" 300; then pass "companion_quests: 300 is still done in the VM's game after a save and a load" "done $(_f_done vm "$CQ_CURIE")"
	else fail "companion_quests: 300 is still done in the VM's game after a save and a load" "done $(_f_done vm "$CQ_CURIE")"; fi
	n=$(_cq_count vm "QuestSync: set $CQ_CURIE .* stage 300 .*without its script")
	expect_eq "companion_quests: the session's 300 is not set again in the VM's loaded game" "$n" "1"
	expect_eq "companion_quests: her affinity after the load in the VM's game" "$(dc vm "av $CQ_CURIE_REF $CQ_CA_AFFINITY")" "$CQ_AFF"
	_cq_synth vm "after a save and a load"
	_cq_set host vm "$CQ_CURIE" 400; _cq_set host vm "$CQ_CURIE" 500; _cq_set host vm "$CQ_CURIE" 600
	if wait_for 15 _cq_completed vm "$CQ_CURIE"; then pass "companion_quests: Curie's quest is completed in the VM's game" "$(dc vm "stages $CQ_CURIE" | cut -c1-80)"
	else fail "companion_quests: Curie's quest is completed in the VM's game" "$(dc vm "stages $CQ_CURIE" | cut -c1-120)"; fi
	expect_eq "companion_quests: her affinity's global is the host's alone (host/VM)" "$(_cq_global host $CQ_CURIE_AFFINITY)/$(_cq_global vm $CQ_CURIE_AFFINITY)" "1/0"

	# MacCready, the VM the owner: its hire (30, by console) marks the game that drives his quest; a payment, never set in the host's.
	dc vm "console setstage $CQ_MAC 30" >/dev/null
	if ! wait_for 15 _f_has vm "$CQ_MAC" 30; then fail "companion_quests: set-up: the VM's console set $CQ_MAC 30" "VM done $(_f_done vm "$CQ_MAC")"; return; fi
	_cq_set vm host "$CQ_MAC" 60 || return
	if _f_has host "$CQ_MAC" 30; then fail "companion_quests: the VM's hire (30, a payment) is not set in the host's game" "host done $(_f_done host "$CQ_MAC")"
	else pass "companion_quests: the VM's hire (30, a payment) is not set in the host's game" "host done $(_f_done host "$CQ_MAC")"; fi
	_cq_set vm host "$CQ_MAC" 120
	expect_eq "companion_quests: his first affinity global is the VM's alone (VM/host)" "$(_cq_global vm $CQ_MAC_AFFINITY_A)/$(_cq_global host $CQ_MAC_AFFINITY_A)" "1/0"
	# The owner leaves mid-quest: the host carries on alone.
	vmssh 'Stop-Process -Name Fallout4 -Force -ErrorAction SilentlyContinue' >/dev/null 2>&1
	sleep 10
	dc host "console setstage $CQ_MAC 140" >/dev/null
	if wait_for 15 _f_has host "$CQ_MAC" 140; then pass "companion_quests: with the owner gone the host carries MacCready's quest on (its own 140)" "host done $(_f_done host "$CQ_MAC")"
	else fail "companion_quests: with the owner gone the host carries MacCready's quest on (its own 140)" "host done $(_f_done host "$CQ_MAC")"; fi
	if ! _vm_restart_rejoin_only companion_quests; then rig_broken game "companion_quests: the VM's game alone is restarted and rejoins the host's session" "$(tail -1 "$A_OUT/companion_quests-vm.log")"; return; fi
	pass "companion_quests: the VM's game alone is restarted and rejoins the host's session" "$(field "$(vm 'net status')" status)"
	if wait_for 90 _f_has vm "$CQ_MAC" 140; then pass "companion_quests: the rejoined VM catches the host's 140 up" "VM done $(_f_done vm "$CQ_MAC")"
	else fail "companion_quests: the rejoined VM catches the host's 140 up" "VM done $(_f_done vm "$CQ_MAC")"; fi
	# 140 is no owner's stage (not kEach): set as any shared stage, with its script, whichever game drives.
	if _cq_log_has vm "QuestSync: set $CQ_MAC '[^']*' stage 140 \(was [0-9]+\)" && ! _cq_log_has vm "QuestSync: set $CQ_MAC .* stage 140 .*its script"; then
		pass "companion_quests: the rejoined VM sets the host's 140 as a plain shared stage" "$(_cq_line vm "QuestSync: set $CQ_MAC .* stage 140 .*")"
	else fail "companion_quests: the rejoined VM sets the host's 140 as a plain shared stage" "$(_cq_line vm "QuestSync: set $CQ_MAC .* stage 140 .*")"; fi
	# Which game drives: the restarted game's save holds the hire or not (whichever save in-vm-3-game.ps1 loaded). With it, the
	# drive is the script's (and a 120 caught up runs its script: his first affinity global 1 there, from the save or the catch-up);
	# without it, the VM mirrors (a 120 caught up is marked done without its script).
	if _f_has vm "$CQ_MAC" 30; then
		expect_match "companion_quests: the hirer's restarted game drives MacCready's quest" "$(dc vm "stages $CQ_MAC")" "drive=scripts marked=true"
		if _cq_log_has vm "QuestSync: set $CQ_MAC .* stage 120 .*without its script"; then fail "companion_quests: the hirer's game never mirrors the session's 120" "$(_cq_line vm "QuestSync: set $CQ_MAC .* stage 120 .*")"
		else pass "companion_quests: the hirer's game never mirrors the session's 120" "$(_cq_line vm "QuestSync: set $CQ_MAC .* stage 120 .*")"; fi
		expect_eq "companion_quests: his first affinity global after the rejoin (VM/host)" "$(_cq_global vm $CQ_MAC_AFFINITY_A)/$(_cq_global host $CQ_MAC_AFFINITY_A)" "1/0"
	else
		echo "-- the VM's restarted game loaded a save from before its hire: it mirrors his quest"
		expect_match "companion_quests: a restarted game without the hire mirrors MacCready's quest" "$(dc vm "stages $CQ_MAC")" "drive=mirror marked=false"
		if _cq_log_has vm "QuestSync: set $CQ_MAC .* stage 120 .*with its script"; then fail "companion_quests: a game without the hire never runs the session's 120 with its script" "$(_cq_line vm "QuestSync: set $CQ_MAC .* stage 120 .*")"
		else pass "companion_quests: a game without the hire never runs the session's 120 with its script" "$(_cq_line vm "QuestSync: set $CQ_MAC .* stage 120 .*")"; fi
	fi
	# Far apart: a stage that moves nobody is set without an offer.
	dc vm "console coc SanctuaryExt" >/dev/null; sleep 14; wait_ingame vm 30
	dc host "console coc $FOLLOW_HOST_CELL" >/dev/null; sleep 14; wait_ingame host 30
	if ! wait_for 20 _f_far; then fail "companion_quests: set-up: the host's coc to $FOLLOW_HOST_CELL put it far from the VM" "host $(dc host pos); VM $(dc vm pos)"; return; fi
	win_mark companion_quests host
	_cq_set vm host "$CQ_MAC" 150
	if _cq_log_has host "QuestSync: offer to follow player [0-9]+ into $CQ_MAC"; then fail "companion_quests: no follow offer for MacCready's 150 in the far host's game" "$(_cq_line host "QuestSync: offer to follow.*")"
	else pass "companion_quests: no follow offer for MacCready's 150 in the far host's game" "none"; fi
	_cq_set vm host "$CQ_MAC" 255
	if wait_for 15 _cq_completed host "$CQ_MAC"; then pass "companion_quests: MacCready's quest is completed in the host's game" "$(dc host "stages $CQ_MAC" | cut -c1-80)"
	else fail "companion_quests: MacCready's quest is completed in the host's game" "$(dc host "stages $CQ_MAC" | cut -c1-120)"; fi
	if _cq_running host "$CQ_MAC"; then pass "companion_quests: the mirrored 255 leaves his quest running in the host's game (his hire's greeting)" "$(dc host "stages $CQ_MAC" | cut -c1-80)"
	else fail "companion_quests: the mirrored 255 leaves his quest running in the host's game (his hire's greeting)" "$(dc host "stages $CQ_MAC" | cut -c1-120)"; fi
	expect_eq "companion_quests: his second affinity global is the VM's alone (VM/host)" "$(_cq_global vm $CQ_MAC_AFFINITY_B)/$(_cq_global host $CQ_MAC_AFFINITY_B)" "1/0"
	crash_verdict "companion_quests"
}

# ---- companion_quests_2: Cait's quest and Nick's after the Memory Den (opt-in: smoke.sh --only companion_quests_2) -------------------
# The other companions' quests (StageRules.h: Nick's, Cait's, Danse's; the SAFE MQ202NickPostSurgery and MS07cPostQuest): only a
# stage that would recruit or dismiss a friend's companion is its owner's (kEach). Set-up by console, said so in the report: both
# games coc to SanctuaryExt, Cait and Nick moved to the players (console moveto), the owner recruits by console (cqf Followers
# SetCompanion), the stages are set by console setstage in the owner's game. Quest stages are for good in a save: run it on
# throwaway saves (backup.sh before, restore after); a game with either quest's stages done is a FAIL naming them. Nick's quest
# is SAFE's: the host must share the safe quests (sQuests = safe), else the step fails its set-up.
# Cait (the host the owner, the VM with no companion): the host's 60 (DisallowCompanion in its script) dismisses her from the
# host and puts her in DisallowedCompanionFaction (the mark: `stages` says drive=scripts marked=true in the host's game,
# drive=mirror in the VM's), the VM's 60 is marked done without its script and leaves its copy of her out of the faction; the
# host's 255 (AllowCompanion(Cait, True, False)) gives her back to the host, and the VM's 255, marked done without its script,
# leaves the VM's Companion alias empty (a plain 255 would have recruited her there: nobody follows the VM) and completes the
# quest in the VM's game.
# Nick (the VM the owner, nobody's companion in either game, disallowed in both by console first, as MQ202 12 leaves him): the
# VM's 90 (AllowCompanion(Nick, True, True): ForceCompanion) recruits him to the VM and is marked done without its script in the
# host's game, whose Companion alias stays empty; its script's 100 reaches the host as a plain stage, whose script allows the
# host's copy of him again (out of DisallowedCompanionFaction) and stops the quest there.
# Then the case a shared stage that sets an owner's stage would open (StageRules.h SHARED_SETS_EACH): the host's MS07c 305 (plain,
# shared by its type) runs in the VM's game and starts Nick's post-quest (MS07cPostQuest 5) there by its own script; the host's
# MS07cPostQuest 20 (COMNick 80: SetCompanion) recruits Nick to the host and never reaches the VM (the quest is unshared), whose
# Companion alias stays empty.
CQ2_CAIT=${CQ2_CAIT:-001666C5}; CQ2_NICK_DEN=${CQ2_NICK_DEN:-000FAC86}; CQ2_MS07C=${CQ2_MS07C:-000229FE}; CQ2_NICK_AFTER=${CQ2_NICK_AFTER:-0018817F}
CQ2_CAIT_REF=00079305; CQ2_NICK_REF=00002F25; CQ2_DISALLOWED=00075D56
_cq2_log_has() { grep -qE "$2" "$(win_read companion_quests_2 "$1")"; }
_cq2_line() { grep -oE "$2" "$(win_read companion_quests_2 "$1")" | head -1; }
_cq2_recruit() { dc "$1" "console cqf Followers \"FollowersScript.SetCompanion\" $2 1 1 1" >/dev/null; sleep 1; dc "$1" "console $2.evp" >/dev/null; }   # _cq2_recruit <who> <ref>
_cq2_dismiss() { dc "$1" "console cqf Followers \"FollowersScript.DismissCompanion\" $2 0 1" >/dev/null; }   # _cq2_dismiss <who> <ref>
_cq2_holds() { [ "$(_ch_held "$1")" = "$2" ]; }   # _cq2_holds <who> <ref>: that game's Companion alias holds the ref now
_cq2_empty() { [ -z "$(_ch_held "$1")" ]; }
# _cq2_disallowed <who> <ref>: "true" or "false", that game's actor in DisallowedCompanionFaction now (dev command `infaction`: the
# game's IsInFaction), or the reply itself when it is neither (an older build without the command). Not `factions`: that lists only
# the actor's faction changes, and DisallowCompanion's AddToFaction need not show there (2026-11-11: Cait and Nick read "none" in
# both games though the scripts had run).
_cq2_disallowed() { local r; r=$(dc "$1" "infaction $2 $CQ2_DISALLOWED" | tr -d '\r'); case "$r" in *": true") echo true;; *": false") echo false;; *) echo "?($r)";; esac; }
_cq2_free_both() {  # both Companion aliases empty, whoever they hold
	local w h; for w in host vm; do h=$(_ch_held "$w"); if [ -n "$h" ]; then _cq2_dismiss "$w" "$h"; sleep 4; fi; [ -z "$(_ch_held "$w")" ] || return 1; done
}
step_companion_quests_2() {
	section "companion_quests_2 (Cait's quest, Nick's after the Memory Den; console placement, console recruits and console setstage for set-up)"
	local w q kind r
	for w in host vm; do
		kind=$(dc "$w" "stages $CQ2_NICK_DEN")
		case "$kind" in *"shared=safe (shared"*) ;; *) fail "companion_quests_2: set-up: $w shares Nick's quest after the Memory Den (sQuests = safe)" "got '$kind'"; return;; esac
		for q in "$CQ2_CAIT" "$CQ2_NICK_DEN" "$CQ2_MS07C" "$CQ2_NICK_AFTER"; do
			if [ "$(_f_done "$w" "$q")" != none ]; then fail "companion_quests_2: set-up: no stage of $q done in $w's game (a throwaway save from before it)" "done $(_f_done "$w" "$q")"; return; fi
		done
	done
	enter_both SanctuaryExt 14
	crash_snapshot
	for w in host vm; do dc "$w" "console $CQ2_CAIT_REF.moveto player" >/dev/null; dc "$w" "console $CQ2_NICK_REF.moveto player" >/dev/null; done
	sleep 4
	if ! wait_for 20 _cq2_free_both; then fail "companion_quests_2: set-up: the Companion alias empty in both games" "host '$(_ch_held host)', VM '$(_ch_held vm)'"; return; fi

	# Cait, the host the owner: recruited there by console; the VM follows nobody.
	_cq2_recruit host "$CQ2_CAIT_REF"
	if ! wait_for 20 _cq2_holds host "$CQ2_CAIT_REF"; then fail "companion_quests_2: set-up: the host's console recruit of Cait" "alias '$(_ch_held host)'"; return; fi
	win_mark companion_quests_2 vm
	win_mark companion_quests_2 host
	_cq_set host vm "$CQ2_CAIT" 10 || return
	_cq_set host vm "$CQ2_CAIT" 60
	if wait_for 10 _cq2_log_has vm "QuestSync: set $CQ2_CAIT .* stage 60 .*without its script"; then pass "companion_quests_2: the VM's 60 (Cait's chair) is marked done without its script (kEach)" "$(_cq2_line vm "QuestSync: set $CQ2_CAIT .* stage 60 .*")"
	else fail "companion_quests_2: the VM's 60 (Cait's chair) is marked done without its script (kEach)" "no such line in the VM's log"; fi
	if wait_for 15 _cq2_empty host; then pass "companion_quests_2: the host's 60 dismissed Cait (DisallowCompanion in its script)" "alias empty"
	else fail "companion_quests_2: the host's 60 dismissed Cait (DisallowCompanion in its script)" "alias '$(_ch_held host)'"; fi
	expect_eq "companion_quests_2: the host's 60 put Cait in DisallowedCompanionFaction (its game drives her quest)" "$(_cq2_disallowed host "$CQ2_CAIT_REF")" true
	expect_eq "companion_quests_2: the VM's mirrored 60 left its Cait out of DisallowedCompanionFaction" "$(_cq2_disallowed vm "$CQ2_CAIT_REF")" false
	expect_match "companion_quests_2: after its 60 the host's game drives Cait's quest (the faction's mark)" "$(dc host "stages $CQ2_CAIT")" "drive=scripts marked=true"
	expect_match "companion_quests_2: after its mirrored 60 the VM's game mirrors Cait's quest" "$(dc vm "stages $CQ2_CAIT")" "drive=mirror marked=false"
	for q in 70 80 90 100 110; do _cq_set host vm "$CQ2_CAIT" "$q"; done
	if _cq2_log_has vm "QuestSync: set $CQ2_CAIT .* stage (70|80|90|100|110) .*its script"; then fail "companion_quests_2: Cait's 70-110 are set in the VM's game as plain shared stages" "$(_cq2_line vm "QuestSync: set $CQ2_CAIT .* stage (70|80|90|100|110) .*its script.*")"
	else pass "companion_quests_2: Cait's 70-110 are set in the VM's game as plain shared stages" "no owner's line for them"; fi
	_cq_set host vm "$CQ2_CAIT" 255
	if wait_for 10 _cq2_log_has vm "QuestSync: set $CQ2_CAIT .* stage 255 .*without its script"; then pass "companion_quests_2: the VM's 255 (Cait's cure) is marked done without its script (kEach)" "$(_cq2_line vm "QuestSync: set $CQ2_CAIT .* stage 255 .*")"
	else fail "companion_quests_2: the VM's 255 (Cait's cure) is marked done without its script (kEach)" "no such line in the VM's log"; fi
	if wait_for 15 _cq2_holds host "$CQ2_CAIT_REF"; then pass "companion_quests_2: the host's 255 gave Cait back to the host (AllowCompanion with MakeCompanionIfNoneCurrently)" "alias $(_ch_held host)"
	else fail "companion_quests_2: the host's 255 gave Cait back to the host (AllowCompanion with MakeCompanionIfNoneCurrently)" "alias '$(_ch_held host)'"; fi
	sleep 5
	expect_eq "companion_quests_2: the VM's mirrored 255 recruited nobody (a plain 255 would have, with nobody following the VM)" "$(_ch_held vm)" ""
	if wait_for 15 _cq_completed vm "$CQ2_CAIT"; then pass "companion_quests_2: Cait's quest is completed in the VM's game" "$(dc vm "stages $CQ2_CAIT" | cut -c1-80)"
	else fail "companion_quests_2: Cait's quest is completed in the VM's game" "$(dc vm "stages $CQ2_CAIT" | cut -c1-120)"; fi
	expect_match "companion_quests_2: after its 255 the host's game still drives Cait's quest (driven since its 60)" "$(dc host "stages $CQ2_CAIT")" "drive=scripts"
	_cq2_dismiss host "$CQ2_CAIT_REF"; sleep 4

	# Nick after the Memory Den, the VM the owner: nobody's companion in either game before its 90, and disallowed in both first
	# (as MQ202 12 leaves him), so the allow his 100 does in the host's game is seen.
	if ! wait_for 20 _cq2_free_both; then fail "companion_quests_2: set-up: the Companion alias empty in both games before Nick's quest" "host '$(_ch_held host)', VM '$(_ch_held vm)'"; return; fi
	for w in host vm; do dc "$w" "console cqf Followers \"FollowersScript.DisallowCompanion\" $CQ2_NICK_REF 1" >/dev/null; done
	sleep 3
	for w in host vm; do
		r=$(_cq2_disallowed "$w" "$CQ2_NICK_REF")
		if [ "$r" = true ]; then pass "companion_quests_2: set-up: Nick disallowed in $w's game (console DisallowCompanion)" "in $CQ2_DISALLOWED: $r"
		else fail "companion_quests_2: set-up: Nick disallowed in $w's game (console DisallowCompanion)" "in $CQ2_DISALLOWED: $r; changes: $(dc "$w" "factions $CQ2_NICK_REF")"; return; fi
	done
	win_mark companion_quests_2 host
	_cq_set vm host "$CQ2_NICK_DEN" 20 || return
	_cq_set vm host "$CQ2_NICK_DEN" 90
	if wait_for 10 _cq2_log_has host "QuestSync: set $CQ2_NICK_DEN .* stage 90 .*without its script"; then pass "companion_quests_2: the host's 90 (Nick's forced recruit) is marked done without its script (kEach)" "$(_cq2_line host "QuestSync: set $CQ2_NICK_DEN .* stage 90 .*")"
	else fail "companion_quests_2: the host's 90 (Nick's forced recruit) is marked done without its script (kEach)" "no such line in the host's log"; fi
	if wait_for 15 _cq2_holds vm "$CQ2_NICK_REF"; then pass "companion_quests_2: the VM's 90 recruited Nick to the VM (ForceCompanion in its script)" "alias $(_ch_held vm)"
	else fail "companion_quests_2: the VM's 90 recruited Nick to the VM (ForceCompanion in its script)" "alias '$(_ch_held vm)'"; fi
	sleep 5
	expect_eq "companion_quests_2: the host's mirrored 90 recruited nobody (a plain 90 would have forced Nick on the host)" "$(_ch_held host)" ""
	if wait_for "$CQ_SET" _f_has host "$CQ2_NICK_DEN" 100; then pass "companion_quests_2: the VM's 100 (set by its 90's script) is done in the host's game" "done $(_f_done host "$CQ2_NICK_DEN")"
	else fail "companion_quests_2: the VM's 100 (set by its 90's script) is done in the host's game" "host done $(_f_done host "$CQ2_NICK_DEN")"; fi
	if _cq2_log_has host "QuestSync: set $CQ2_NICK_DEN .* stage 100 .*its script"; then fail "companion_quests_2: the host sets 100 as a plain shared stage (AllowCompanion with neither flag)" "$(_cq2_line host "QuestSync: set $CQ2_NICK_DEN .* stage 100 .*")"
	else pass "companion_quests_2: the host sets 100 as a plain shared stage (AllowCompanion with neither flag)" "$(_cq2_line host "QuestSync: set $CQ2_NICK_DEN .* stage 100 .*")"; fi
	sleep 5
	expect_eq "companion_quests_2: the host's 100 took its Nick out of DisallowedCompanionFaction (its script's AllowCompanion)" "$(_cq2_disallowed host "$CQ2_NICK_REF")" false
	if _cq_running host "$CQ2_NICK_DEN"; then fail "companion_quests_2: the host's 100 stopped Nick's quest there (its script's Stop)" "$(dc host "stages $CQ2_NICK_DEN" | cut -c1-80)"
	else pass "companion_quests_2: the host's 100 stopped Nick's quest there (its script's Stop)" "$(dc host "stages $CQ2_NICK_DEN" | cut -c1-80)"; fi
	expect_eq "companion_quests_2: Nick stays the VM's after its 100" "$(_ch_held vm)" "$CQ2_NICK_REF"
	expect_eq "companion_quests_2: the host's Companion alias is still empty after Nick's quest" "$(_ch_held host)" ""
	_cq2_dismiss vm "$CQ2_NICK_REF"; sleep 4

	# A shared stage that sets an owner's stage: the host's MS07c 305 runs in the VM's game and starts Nick's post-quest there by
	# its own script; the host's MS07cPostQuest 20 (COMNick 80: SetCompanion) recruits Nick to the host and never reaches the VM.
	if ! wait_for 20 _cq2_free_both; then fail "companion_quests_2: set-up: the Companion alias empty in both games before MS07c 305" "host '$(_ch_held host)', VM '$(_ch_held vm)'"; return; fi
	win_mark companion_quests_2 vm
	_cq_set host vm "$CQ2_MS07C" 305 || return
	if wait_for 20 _f_has vm "$CQ2_NICK_AFTER" 5; then pass "companion_quests_2: the VM's own 305 started Nick's post-quest there (its 5, by its script)" "VM done $(_f_done vm "$CQ2_NICK_AFTER")"
	else fail "companion_quests_2: the VM's own 305 started Nick's post-quest there (its 5, by its script)" "VM done $(_f_done vm "$CQ2_NICK_AFTER")"; fi
	dc host "console setstage $CQ2_NICK_AFTER 20" >/dev/null
	if ! wait_for 15 _f_has host "$CQ2_NICK_AFTER" 20; then fail "companion_quests_2: set-up: the host's console set $CQ2_NICK_AFTER 20" "host done $(_f_done host "$CQ2_NICK_AFTER")"; return; fi
	if wait_for 15 _cq2_holds host "$CQ2_NICK_REF"; then pass "companion_quests_2: the host's MS07cPostQuest 20 recruited Nick to the host (COMNick 80 in its script)" "alias $(_ch_held host)"
	else fail "companion_quests_2: the host's MS07cPostQuest 20 recruited Nick to the host (COMNick 80 in its script)" "alias '$(_ch_held host)'"; fi
	sleep "$CQ_SET"
	if _f_has vm "$CQ2_NICK_AFTER" 20; then fail "companion_quests_2: the host's MS07cPostQuest 20 never reaches the VM's game (the quest is unshared)" "VM done $(_f_done vm "$CQ2_NICK_AFTER")"
	else pass "companion_quests_2: the host's MS07cPostQuest 20 never reaches the VM's game (the quest is unshared)" "VM done $(_f_done vm "$CQ2_NICK_AFTER")"; fi
	if _cq2_log_has vm "QuestSync: set $CQ2_NICK_AFTER "; then fail "companion_quests_2: the VM's game set no stage of Nick's post-quest from the host" "$(_cq2_line vm "QuestSync: set $CQ2_NICK_AFTER .*")"
	else pass "companion_quests_2: the VM's game set no stage of Nick's post-quest from the host" "none"; fi
	expect_eq "companion_quests_2: the VM's Companion alias is still empty after the host's MS07cPostQuest 20 (no second Nick)" "$(_ch_held vm)" ""
	_cq2_dismiss host "$CQ2_NICK_REF"; sleep 4
	crash_verdict "companion_quests_2"
}

# ---- shared quest rewards: a turn-in stage's reward reaches the friend (opt-in: smoke.sh --only rewards) ------------------------------------
# A turn-in stage (src/game/StageRules.h kPays: it takes from the player, so a friend's game never sets it) gives its reward only in the
# game that set it; QuestRewards (common/RewardBook.h) claims what that player got and the server sends it to the others once. Each case:
# the host's console sets the quest's start stage (set-up: the quest started, shared) and then its turn-in stage (set-up by console, said
# so: the walk to the quest giver and the talk are not under test). Asserted: the host's game claims a reward for the stage; the VM's
# game gives it once ("given here"), its caps go up by the caps the host's claim names, and it closes its own turn-in: the stage marked
# done without its script, and the quest left as the turn-in leaves it; then the VM's own turn-in (its console sets the same stage, as
# its dialogue would) gives no caps, changes no stage and is claimed as no turn-in of its own. Two turn-ins:
# - REWARD_QUEST's REWARD_STAGE, one that completes and stops its quest (FFDiamondCity03 30: the fern flower; the VM's quest completed
#   and stopped);
# - REWARD_SALE_QUEST's REWARD_SALE_STAGE, one that gives items and doesn't end its quest (V81_03 3: a Jet sold for caps; the host's
#   player is given REWARD_SALE_ITEM by console first, as the sale takes it; the VM's quest still running, not completed).
# A start may be several stages, comma-separated, set in that order: a quest is shared only once its player has started it, an
# objective shown (QuestSync's SessionStarted), and what was done in it just before goes with that start (StageLog::Unstarted, 60 s).
# V81_03's 1 ("PC talked to Bobby first") has no fragment and shows no objective: alone, the host's game keeps it back as the talk
# before the quest and drops it 60 s later, so it never reached the VM (2026-11-12: "unstarted=1", every objective dormant). Its 100
# ("PC gets quest") shows objective 100: 1 then 100 is the conversation's own order, and both go to the VM together.
# With REWARD_AWAY=1 the sale is instead turned in with the VM's game gone (killed): rejoining, it is sent the reward with its welcome
# and gives it, after its start stage, which comes with the same welcome (RewardBook::Hold::kEarlierStage). Dialogue lines' rewards
# (RewardLines.h) need a real conversation: a hand test. Quest stages are for good in a save: run it on throwaway saves (backup.sh
# before, restore after). A game that has a turn-in done already has nothing to test: a FAIL naming it.
REWARD_QUEST=${REWARD_QUEST:-0002125F}; REWARD_START=${REWARD_START:-10}; REWARD_STAGE=${REWARD_STAGE:-30}   # FFDiamondCity03: the fern flower
REWARD_SALE_QUEST=${REWARD_SALE_QUEST:-00033527}; REWARD_SALE_START=${REWARD_SALE_START:-1,100}; REWARD_SALE_STAGE=${REWARD_SALE_STAGE:-3}   # V81_03: a Jet sold
REWARD_SALE_ITEM=${REWARD_SALE_ITEM:-000366C5}   # Jet: what the sale takes from the host's player
REWARD_AWAY=${REWARD_AWAY:-0}; REWARD_START_WAIT=${REWARD_START_WAIT:-45}
REWARD_CELL=${REWARD_CELL:-SanctuaryExt}; REWARD_SETTLE=${REWARD_SETTLE:-25}
_r_log_has() { grep -qE "$2" "$(win_read rewards "$1")"; }   # _r_log_has <who> <regex>: the game's log since win_mark has it
_r_caps() { field "$(dc "$1" inventory)" caps; }
_r_claimed_caps() {  # the caps the host's claim for <quest> stage <stage> names (0 when none)
	grep -oE "QuestRewards: our player turned in $1 \(stage $2\): [^;]*" "$A_OUT/windelta-rewards-host.txt" | head -1 | grep -oE "0000000F x[0-9]+" | grep -oE "[0-9]+$" || echo 0
}
_r_tail() { grep -E 'QuestRewards:' "$A_OUT/windelta-rewards-$1.txt" | tail -3 | tr '\n' '|'; }
_r_flags() { dc "$1" "stages $2" | sed -nE 's/.* flags=([0-9A-Fa-f]+) .*/\1/p'; }   # the quest's flags (0x1 running, 0x2 completed)
_r_whole_log_has() { dc "$1" "log flush" >/dev/null; _log_fetch "$1" "$A_OUT/rewards-whole-$1"; grep -qE "$2" "$A_OUT/rewards-whole-$1/0.log"; }   # the game's whole current log (a restarted game's is new)
_r_has_all() { local st; for st in ${3//,/ }; do _f_has "$1" "$2" "$st" || return 1; done; }   # _r_has_all <who> <quest> <stage,stage...>
_r_setup() {  # _r_setup <quest> <start[,start...]> <stage>: shared on the host, the turn-in not done in either game, then started on both
	local kind st; kind=$(field "$(dc host "stages $1")" shared)
	case "$(dc host "stages $1")" in *"shared=no "*|*"(not shared"*|error*) fail "rewards: set-up: the host shares $1" "$(dc host "stages $1" | cut -c1-160)"; return 1;; esac
	if _f_has host "$1" "$3" || _f_has vm "$1" "$3"; then fail "rewards: set-up: neither game has $1 stage $3 done (a throwaway save from before it)" "host $(_f_done host "$1"), VM $(_f_done vm "$1")"; return 1; fi
	# The join's catch-up over in both games first (a live stage waits behind it), and both games' logs of the start kept: V81_03's 1
	# never reached the VM in three runs, and the logs of that part were lost each time (2026-11-11).
	_h_catchup_over vm 10 120; _h_catchup_over host 10 60
	win_mark rsetup host; win_mark rsetup vm
	for st in ${2//,/ }; do dc host "console setstage $1 $st" >/dev/null; sleep 1; done
	echo "rewards: set-up: the host's console set $1 $2; the host's objectives of it now: $(dc host "objectives $1" | cut -c1-160)"
	if ! wait_for "$REWARD_START_WAIT" _r_has_all vm "$1" "$2"; then
		fail "rewards: set-up: the host's $1 $2 reached the VM's game (the quest started and shared)" "host $(_f_done host "$1"), VM $(_f_done vm "$1"); kind $kind; the host's log: $(grep -E "$1|QuestSync: not reporting" "$(win_read rsetup host)" | tail -3 | cut -c1-200 | tr '\n' '|'); the VM's log: $(grep -E "$1" "$(win_read rsetup vm)" | tail -3 | cut -c1-200 | tr '\n' '|'); host $(dc host 'net status' | grep -oE 'quests: reported=[0-9]+[^|]* unstarted=[0-9]+' | grep -oE '(reported|unsent|unstarted)=[0-9]+' | tr '\n' ' '); host objectives: $(dc host "objectives $1" | cut -c1-160)"
		return 1
	fi
}
_r_given_count() {  # _r_given_count <quest> <stage>: the VM's "given here" lines for it since win_mark
	win_read rewards vm >/dev/null
	grep -cE "QuestRewards: player [0-9]+'s reward [0-9]+ \($1 stage $2\) given here" "$A_OUT/windelta-rewards-vm.txt"
}
# _r_live <label> <quest> <start> <stage> <ends: stops|runs> [item]: the host turns the stage in with the VM's game there. item: given
# to the host's player by console first, before the stage's window (a console gain that early is in no stage's window), as the stage
# takes it.
_r_live() {
	local label=$1 quest=$2 start=$3 stage=$4 ends=$5 item=${6:-} caps0 caps1 caps2 caps3 claimed flags done2 closing want
	_r_setup "$quest" "$start" "$stage" || return
	if [ -n "$item" ]; then dc host "console player.additem $item 1" >/dev/null; sleep 5; fi
	win_mark rewards host; win_mark rewards vm
	caps0=$(_r_caps vm)
	dc host "console setstage $quest $stage" >/dev/null
	if ! wait_for 15 _f_has host "$quest" "$stage"; then fail "$label: set-up: the host's console set $quest $stage" "host done $(_f_done host "$quest")"; return; fi
	if wait_for "$REWARD_SETTLE" _r_log_has host "QuestRewards: our player turned in $quest \(stage $stage\)"; then
		pass "$label: the host's game claims the turn-in's reward" "$(grep -oE "QuestRewards: our player turned in $quest[^;]*" "$A_OUT/windelta-rewards-host.txt" | head -1 | cut -c1-200)"
	else
		fail "$label: the host's game claims the turn-in's reward" "$(_r_tail host)"; return
	fi
	if wait_for "$REWARD_SETTLE" _r_log_has vm "QuestRewards: player [0-9]+'s reward [0-9]+ \($quest stage $stage\) given here"; then
		pass "$label: the VM's game gives the host's reward" "$(grep -oE "QuestRewards: player [0-9]+'s reward [0-9]+ \($quest stage $stage\) given here[^;]*" "$A_OUT/windelta-rewards-vm.txt" | head -1)"
	else
		fail "$label: the VM's game gives the host's reward" "$(_r_tail vm)"; return
	fi
	claimed=$(_r_claimed_caps "$quest" "$stage"); caps1=$(_r_caps vm)
	if is_num "$caps0" && is_num "$caps1"; then expect_eq "$label: the VM's caps go up by the caps the host's claim names" "$((caps1 - caps0))" "$claimed"
	else fail "$label: the VM's caps go up by the caps the host's claim names" "caps not read: before '$caps0', after '$caps1'"; fi
	# Closed in the VM's game once given (QuestSync::CloseTurnIn): the stage marked done without its script, and the quest completed
	# and stopped only where the turn-in does that (RewardLines::TURN_INS), so the VM's player can't turn the same in again.
	closing=$(grep -oE "QuestSync: $quest '[^']*' stage $stage is a friend's turn-in whose reward was given here: [^(]*" "$A_OUT/windelta-rewards-vm.txt" | head -1)
	if [ "$ends" = stops ]; then want="given here: marked done, the quest completed, the quest stopped "; else want="given here: marked done "; fi
	if [ -n "$closing" ] && [ "${closing##*whose reward was }" = "$want" ]; then
		pass "$label: the VM's game closes its own turn-in once the reward is given (the stage marked done, its script not run)" "$closing"
	else
		fail "$label: the VM's game closes its own turn-in once the reward is given (the stage marked done, its script not run)" "'$closing' (want '...$want'); $(grep -E 'QuestSync: .*turn-in' "$A_OUT/windelta-rewards-vm.txt" | tail -2 | tr '\n' '|')"
	fi
	if _f_has vm "$quest" "$stage"; then pass "$label: the turn-in stage reads done in the VM's game" "VM done $(_f_done vm "$quest")"
	else fail "$label: the turn-in stage reads done in the VM's game" "VM done $(_f_done vm "$quest")"; fi
	flags=$(_r_flags vm "$quest")
	if [ "$ends" = stops ]; then
		if [ -n "$flags" ] && (( (16#$flags & 2) != 0 && (16#$flags & 1) == 0 )); then pass "$label: the VM's quest is completed and stopped, as the turn-in leaves it" "flags=$flags"
		else fail "$label: the VM's quest is completed and stopped, as the turn-in leaves it" "flags='$flags' (0x2 completed, 0x1 running)"; fi
	else
		if [ -n "$flags" ] && (( (16#$flags & 2) == 0 && (16#$flags & 1) != 0 )); then pass "$label: the VM's quest still runs and isn't completed, as the turn-in leaves it" "flags=$flags (host $(_r_flags host "$quest"))"
		else fail "$label: the VM's quest still runs and isn't completed, as the turn-in leaves it" "flags='$flags' (0x2 completed, 0x1 running; host $(_r_flags host "$quest"))"; fi
	fi
	sleep 5
	expect_eq "$label: given once in the VM's game" "$(_r_given_count "$quest" "$stage")" 1
	# The VM's own turn-in after the mirror (its dialogue would set the same stage): nothing more is given.
	caps2=$(_r_caps vm); done2=$(_f_done vm "$quest"); win_mark rewards vm
	dc vm "console setstage $quest $stage" >/dev/null
	sleep 10; win_read rewards vm >/dev/null; caps3=$(_r_caps vm)
	if is_num "$caps2" && is_num "$caps3"; then expect_eq "$label: the VM's own turn-in after the mirror gives no caps again" "$((caps3 - caps2))" 0
	else fail "$label: the VM's own turn-in after the mirror gives no caps again" "caps not read: before '$caps2', after '$caps3'"; fi
	expect_eq "$label: the VM's own turn-in after the mirror changes no stage" "$(_f_done vm "$quest")" "$done2"
	if grep -qE "QuestRewards: our player turned in $quest \(stage $stage\)" "$A_OUT/windelta-rewards-vm.txt"; then
		fail "$label: the VM's game claims no turn-in of its own after the mirror" "$(_r_tail vm)"
	else pass "$label: the VM's game claims no turn-in of its own after the mirror" "ok"; fi
	sleep 5
	expect_eq "$label: given no more in the VM's game" "$(_r_given_count "$quest" "$stage")" 0
}
step_rewards() {
	section "rewards (a turn-in stage's reward mirrored to the friend; console placement, setstage and additem on the host)"
	enter_both "$REWARD_CELL" 14
	_r_live rewards "$REWARD_QUEST" "$REWARD_START" "$REWARD_STAGE" stops
	if [ "$REWARD_AWAY" != 1 ]; then
		_r_live "rewards (sale)" "$REWARD_SALE_QUEST" "$REWARD_SALE_START" "$REWARD_SALE_STAGE" runs "$REWARD_SALE_ITEM"
		return
	fi
	# Away: the VM's game is gone while the host turns the sale in; its rejoin's welcome brings the reward, after the start stage.
	_r_setup "$REWARD_SALE_QUEST" "$REWARD_SALE_START" "$REWARD_SALE_STAGE" || return
	vmssh 'Stop-Process -Name Fallout4 -Force -ErrorAction SilentlyContinue' >/dev/null 2>&1; sleep 8
	dc host "console player.additem $REWARD_SALE_ITEM 1" >/dev/null; sleep 5
	win_mark rewards host
	dc host "console setstage $REWARD_SALE_QUEST $REWARD_SALE_STAGE" >/dev/null
	if ! wait_for "$REWARD_SETTLE" _r_log_has host "QuestRewards: our player turned in $REWARD_SALE_QUEST \(stage $REWARD_SALE_STAGE\)"; then
		fail "rewards (away): the host's game claims the turn-in's reward with the VM gone" "$(_r_tail host)"; return
	fi
	pass "rewards (away): the host's game claims the turn-in's reward with the VM gone" "ok"
	if ! _vm_restart_rejoin_only rewards-rejoin; then fail "rewards (away): set-up: the VM's game restarts and rejoins" "$(tail -2 "$A_OUT/rewards-rejoin-vm.log" | tr '\n' '|')"; return; fi
	if wait_for 60 _r_whole_log_has vm "QuestRewards: player [0-9]+'s reward [0-9]+ \($REWARD_SALE_QUEST stage $REWARD_SALE_STAGE\) given here"; then
		pass "rewards (away): the VM's game, rejoining, gives the reward turned in while it was gone" "ok"
		if _r_has_all vm "$REWARD_SALE_QUEST" "$REWARD_SALE_START"; then pass "rewards (away): the start stage from the same welcome is set in the VM's game, not dropped by the closing" "VM done $(_f_done vm "$REWARD_SALE_QUEST")"
		else fail "rewards (away): the start stage from the same welcome is set in the VM's game, not dropped by the closing" "VM done $(_f_done vm "$REWARD_SALE_QUEST")"; fi
		if _f_has vm "$REWARD_SALE_QUEST" "$REWARD_SALE_STAGE"; then pass "rewards (away): the turn-in stage reads done in the VM's game (closed)" "VM done $(_f_done vm "$REWARD_SALE_QUEST")"
		else fail "rewards (away): the turn-in stage reads done in the VM's game (closed)" "VM done $(_f_done vm "$REWARD_SALE_QUEST")"; fi
	else
		fail "rewards (away): the VM's game, rejoining, gives the reward turned in while it was gone" "$(_r_tail vm)"
	fi
}

# ---- the teleport quests and the host's timer (opt-in: smoke.sh --only teleport_quests) -------------------------------------------
# Objective-less quests shared with sQuests = safe since 0.17 whose stages teleport the player or their companion have StageRules rows
# (kMoves): a far friend is offered to go with the chooser and nobody is moved without taking it up. InstituteTeleportEffects 10 (X6-88
# moved to the relay beside the player when he is the companion: nothing moves in either game here, and no stage of the story is set by
# it; RRAct3Pickup 1 would do, but it sets MQ206Min 1500, a bridge that moves the VM into the Institute anyway) is set by the host's
# console with the VM far: the VM's log has the offer, the VM's player stays where it is, and after OFFER_FOR the offer "was not taken
# up" and the stage is skipped there. Then Goodneighbor's gate guards (FFGoodneighbor08) on the host's timer (StageRules::HOST_TIMERS):
# the VM's console sets its 25 (its own timer's start; not 200: at Sanctuary the VM's 200 would set its own 300, whose stop runs the
# shutdown stage 1000 there): the VM's log says it doesn't report it and the host's game never has it; the host's console sets 25 (the
# host's own timer's start; set-up only: the VM has its own 25 done, so the host's is settled there before any timer line is logged) and
# then 200 ("the host's timer decides" for 200 and 300 in the VM's log), which at Concord sets the
# host's 300, whose stop runs 1000 (the quest's shutdown stage): the VM's log drops 200 and 300, the VM's game never has 300, and the
# host's 1000 reaches the VM (the shutdown stage every game takes). The VM's own two-hour game timer (six minutes at the default
# timescale) runs on in the VM's game after the step. Between the two, the VM's start (its 10) starts the far host's quest through the
# engine's own start, and the VM's 20 (Sammy's scene, which can't play at Concord) makes the host's game set its own 25 after 30 s of play
# (StageRules::TIMER_STARTS). Set-up by console, said so in the report.
# Quest stages are for good in a save: run it on throwaway saves (backup.sh before, restore after). A game with a stage already done has
# nothing to test: a FAIL naming it.
TQ_MOVE_QUEST=${TQ_MOVE_QUEST:-001ED4FA}; TQ_MOVE_STAGE=${TQ_MOVE_STAGE:-10}      # InstituteTeleportEffects 10
TQ_TIMER_QUEST=${TQ_TIMER_QUEST:-0015DE65}                                       # FFGoodneighbor08: 25 the VM's own timer's start; the host's 25, 200 and 300 its timer's, 1000 the shutdown stage the host's stop runs
TQ_TIMER_EDID=${TQ_TIMER_EDID:-FFGoodneighbor08}
TQ_HOST_CELL=${TQ_HOST_CELL:-ConcordExt}; TQ_OFFER_FOR=${TQ_OFFER_FOR:-90}; TQ_KICK_WAIT=${TQ_KICK_WAIT:-45}   # the host's own 25 after the VM's 20: TIMER_STARTS' 30 s and the poll
_tq_log_has() { grep -qE "$2" "$(win_read tq "$1")"; }   # _tq_log_has <who> <regex>: the game's log since win_mark has it
# _tq_start <who>: FFGoodneighbor08 running in that game before its console sets a stage of it. The quest is not running in a fresh
# game, and the console's setstage on a stopped quest does nothing (2026-11-11: "the VM's console set 0015DE65 25: VM done none").
# Set-up by console (startquest), said so.
_tq_start() {
	_rad_is_running "$1" "$TQ_TIMER_EDID" && return 0
	# The engine's own start (dev `queststart`): the console's startquest does nothing for a quest the story manager starts on a
	# change of location, as FFGoodneighbor08 is (2026-11-11: "running=false").
	echo "  set-up: the $1's game starts $TQ_TIMER_EDID ($TQ_TIMER_QUEST) by dev queststart before setting its stages: $(dc "$1" "queststart $TQ_TIMER_QUEST" | cut -c1-160)"
	wait_for 10 _rad_is_running "$1" "$TQ_TIMER_EDID"
}
step_teleport_quests() {
	section "teleport quests (a silent quest's teleport offered to a far player, and a timer's stages kept to the host; console placement and setstage)"
	local kind s vd0 pos0 moved
	for s in "$TQ_MOVE_QUEST" "$TQ_TIMER_QUEST"; do
		kind=$(dc host "stages $s")
		case "$kind" in *"sQuests = safe"*) ;; *) fail "teleport quests: set-up: the host shares $s (sQuests = safe, a shared story)" "got '$kind'"; return;; esac
	done
	if _f_has host "$TQ_MOVE_QUEST" "$TQ_MOVE_STAGE" || _f_has vm "$TQ_MOVE_QUEST" "$TQ_MOVE_STAGE"; then fail "teleport quests: set-up: neither game has $TQ_MOVE_QUEST stage $TQ_MOVE_STAGE done (a throwaway save from before it)" "host $(_f_done host "$TQ_MOVE_QUEST"), VM $(_f_done vm "$TQ_MOVE_QUEST")"; return; fi
	for s in 25 200 300 1000; do
		if _f_has host "$TQ_TIMER_QUEST" "$s" || _f_has vm "$TQ_TIMER_QUEST" "$s"; then fail "teleport quests: set-up: neither game has $TQ_TIMER_QUEST stage $s done (a throwaway save from before it)" "host $(_f_done host "$TQ_TIMER_QUEST"), VM $(_f_done vm "$TQ_TIMER_QUEST")"; return; fi
	done
	enter_both SanctuaryExt 14
	dc host "console coc $TQ_HOST_CELL" >/dev/null; sleep 14; wait_ingame host 30
	if ! wait_for 20 _f_far; then fail "teleport quests: set-up: the host's coc to $TQ_HOST_CELL put it far from the VM" "host $(dc host pos); VM $(dc vm pos)"; return; fi

	# The teleport: offered to the far VM, not taken up, nobody moved.
	win_mark tq vm; vd0=$(_f_done vm "$TQ_MOVE_QUEST"); pos0=$(dc vm pos)
	dc host "console setstage $TQ_MOVE_QUEST $TQ_MOVE_STAGE" >/dev/null
	if ! wait_for 15 _f_has host "$TQ_MOVE_QUEST" "$TQ_MOVE_STAGE"; then fail "teleport quests: set-up: the host's console set $TQ_MOVE_QUEST $TQ_MOVE_STAGE" "host done $(_f_done host "$TQ_MOVE_QUEST")"; return; fi
	if wait_for 20 _tq_log_has vm "QuestSync: offer to follow player [0-9]+ into $TQ_MOVE_QUEST .* stage $TQ_MOVE_STAGE \(it moves the player\)"; then
		pass "teleport quests: the far VM is offered to go with the host ($TQ_MOVE_QUEST $TQ_MOVE_STAGE)" "$(grep -oE "QuestSync: offer to follow player [0-9]+ into $TQ_MOVE_QUEST[^;]*" "$A_OUT/windelta-tq-vm.txt" | head -1)"
	else
		fail "teleport quests: the far VM is offered to go with the host ($TQ_MOVE_QUEST $TQ_MOVE_STAGE)" "no offer line in the VM's log; VM done $(_f_done vm "$TQ_MOVE_QUEST")"; return
	fi
	sleep $((TQ_OFFER_FOR - 20))
	if _f_has vm "$TQ_MOVE_QUEST" "$TQ_MOVE_STAGE"; then fail "teleport quests: not taken up, the stage is not set while the offer is open" "done: $(_f_done vm "$TQ_MOVE_QUEST")"
	else pass "teleport quests: not taken up, the stage is not set while the offer is open" "VM done $(_f_done vm "$TQ_MOVE_QUEST") (was $vd0) after $((TQ_OFFER_FOR - 20)) s"; fi
	if wait_for 45 _tq_log_has vm "QuestSync: not setting $TQ_MOVE_QUEST .* stage $TQ_MOVE_STAGE: " \
		&& grep -qE "QuestSync: the offer to follow player [0-9]+ into $TQ_MOVE_QUEST .* was not taken up" "$A_OUT/windelta-tq-vm.txt" && ! _f_has vm "$TQ_MOVE_QUEST" "$TQ_MOVE_STAGE"; then
		pass "teleport quests: not taken up in $TQ_OFFER_FOR s, the stage is skipped in the VM's game" "$(grep -oE "QuestSync: not setting $TQ_MOVE_QUEST .*" "$A_OUT/windelta-tq-vm.txt" | head -1)"
	else
		fail "teleport quests: not taken up in $TQ_OFFER_FOR s, the stage is skipped in the VM's game" "VM done $(_f_done vm "$TQ_MOVE_QUEST"); $(grep -E "QuestSync: (set|not setting|the offer) " "$A_OUT/windelta-tq-vm.txt" | tail -3 | tr '\n' '|')"
	fi
	moved=$(_q_moved "$pos0" "$(dc vm pos)")
	if is_num "$moved"; then expect_le "teleport quests: the VM's player stays where it was" "$moved" 64 u
	else fail "teleport quests: the VM's player stays where it was" "no position read"; fi

	# The host's timer: the VM's own 25 stays in the VM's game; the host's 25, 200 and 300 never reach the VM; its 1000 does.
	# First the VM's start of the event reaches the far host: its 10 (the start-up stage) starts the host's FFGoodneighbor08 through
	# the engine's own start (the story manager starts it on a change of location, and the console's setstage of a stopped
	# event-scoped quest is refused: QuestStartRules::StartsFromFriend), and the host's game has 10 done. Only when neither game ran
	# the quest and the host has no 10 yet.
	local tqfresh=0
	if ! _rad_is_running vm "$TQ_TIMER_EDID" && ! _rad_is_running host "$TQ_TIMER_EDID" && ! _f_has host "$TQ_TIMER_QUEST" 10; then tqfresh=1; fi
	win_mark tq host
	if ! _tq_start vm; then fail "teleport quests: set-up: the VM's console started $TQ_TIMER_EDID" "$(_rad_run vm "$TQ_TIMER_EDID"); VM done $(_f_done vm "$TQ_TIMER_QUEST")"; return; fi
	if [ "$tqfresh" = 1 ]; then
		if wait_for 30 _f_has host "$TQ_TIMER_QUEST" 10 && _tq_log_has host "QuestSync: $TQ_TIMER_QUEST '.*' \(the story manager starts it from an event\) started here through the game's own start"; then
			pass "teleport quests: the VM's start of $TQ_TIMER_EDID (its 10) starts the far host's quest through the game's own start" "$(grep -oE "QuestSync: (set )?$TQ_TIMER_QUEST '.*' .*stage 10.*" "$A_OUT/windelta-tq-host.txt" | head -2 | cut -c1-200 | tr '\n' '|')"
		else
			fail "teleport quests: the VM's start of $TQ_TIMER_EDID (its 10) starts the far host's quest through the game's own start" "host done $(_f_done host "$TQ_TIMER_QUEST"); $(_rad_run host "$TQ_TIMER_EDID"); $(grep -E "QuestSync: .*$TQ_TIMER_QUEST" "$(win_read tq host)" | tail -3 | cut -c1-200 | tr '\n' '|')"
		fi
	else echo "  teleport quests: the host's start from the VM's 10 not checked: $TQ_TIMER_EDID ran in a game already, or the host has its 10"; fi
	win_mark tq vm; win_mark tq host
	dc vm "console setstage $TQ_TIMER_QUEST 25" >/dev/null
	if ! wait_for 15 _f_has vm "$TQ_TIMER_QUEST" 25; then fail "teleport quests: set-up: the VM's console set $TQ_TIMER_QUEST 25" "VM done $(_f_done vm "$TQ_TIMER_QUEST")"; return; fi
	if wait_for 20 _tq_log_has vm "QuestSync: not reporting $TQ_TIMER_QUEST .* stage 25: a timer's stage"; then
		pass "teleport quests: the VM's game keeps its own timer's 25 to itself" "$(grep -oE "QuestSync: not reporting $TQ_TIMER_QUEST .*" "$A_OUT/windelta-tq-vm.txt" | head -1)"
	else
		fail "teleport quests: the VM's game keeps its own timer's 25 to itself" "$(grep -E "QuestSync: (not reporting|set) " "$A_OUT/windelta-tq-vm.txt" | tail -3 | tr '\n' '|')"
	fi
	sleep 10
	if _f_has host "$TQ_TIMER_QUEST" 25; then fail "teleport quests: the host's game never has the VM's timer's start" "host done $(_f_done host "$TQ_TIMER_QUEST")"
	else pass "teleport quests: the host's game never has the VM's timer's start" "host done $(_f_done host "$TQ_TIMER_QUEST")"; fi
	# The VM's 20 (Sammy's scene at the gate, which sets 25 where it plays) reaches the far host, where the scene can't play (Sammy
	# not loaded): the host's game sets its own 25, its timer's start, once the scene has had its 30 s (StageRules::TIMER_STARTS),
	# or the host's event would never end. Only when the host's quest runs (started from the VM's 10 above) and has no 20 yet.
	if _rad_is_running host "$TQ_TIMER_EDID" && ! _f_has host "$TQ_TIMER_QUEST" 20; then
		win_mark tq host
		dc vm "console setstage $TQ_TIMER_QUEST 20" >/dev/null
		if ! wait_for 15 _f_has vm "$TQ_TIMER_QUEST" 20; then fail "teleport quests: set-up: the VM's console set $TQ_TIMER_QUEST 20" "VM done $(_f_done vm "$TQ_TIMER_QUEST")"; return; fi
		if ! wait_for 20 _f_has host "$TQ_TIMER_QUEST" 20; then fail "teleport quests: the VM's 20 (Sammy's scene) reaches the far host" "host done $(_f_done host "$TQ_TIMER_QUEST"); $(grep -E "QuestSync: .*$TQ_TIMER_QUEST" "$(win_read tq host)" | tail -3 | cut -c1-200 | tr '\n' '|')"
		elif wait_for "$TQ_KICK_WAIT" _f_has host "$TQ_TIMER_QUEST" 25 && _tq_log_has host "QuestSync: $TQ_TIMER_QUEST '.*' has run [0-9]+ s of play with stage 20 done and 25 not .*setting 25 here, the start of the host's timer"; then
			pass "teleport quests: the VM's 20 reaches the far host, whose game starts its own timer (25) when the scene can't play there" "$(grep -oE "QuestSync: $TQ_TIMER_QUEST '.*' has run .*" "$A_OUT/windelta-tq-host.txt" | head -1 | cut -c1-200)"
		else
			fail "teleport quests: the VM's 20 reaches the far host, whose game starts its own timer (25) when the scene can't play there" "host done $(_f_done host "$TQ_TIMER_QUEST") after $TQ_KICK_WAIT s; $(grep -E "QuestSync: .*$TQ_TIMER_QUEST" "$(win_read tq host)" | tail -3 | cut -c1-200 | tr '\n' '|')"
		fi
	else echo "  teleport quests: the host's own 25 after the VM's 20 not checked: the host's $TQ_TIMER_EDID isn't running, or it has 20 already"; fi
	# The host's 25 is set-up only when the step before didn't set it (the VM's game already has its own 25 done, so the host's
	# is settled there before the timer check logs anything); the host's 200 at Concord sets its 300, whose stop runs the shutdown
	# stage 1000. The host's quest is started first the same way if it isn't running (a stopped quest takes no setstage).
	if ! _tq_start host; then fail "teleport quests: set-up: the host's console started $TQ_TIMER_EDID" "$(_rad_run host "$TQ_TIMER_EDID"); host done $(_f_done host "$TQ_TIMER_QUEST")"; return; fi
	if _f_has host "$TQ_TIMER_QUEST" 25; then echo "  set-up: the host has its own 25 already (its game set it above): no setstage of it"
	else
		dc host "console setstage $TQ_TIMER_QUEST 25" >/dev/null
		if ! wait_for 15 _f_has host "$TQ_TIMER_QUEST" 25; then fail "teleport quests: set-up: the host's console set $TQ_TIMER_QUEST 25" "host done $(_f_done host "$TQ_TIMER_QUEST")"; return; fi
	fi
	sleep 3
	dc host "console setstage $TQ_TIMER_QUEST 200" >/dev/null
	if ! wait_for 15 _f_has host "$TQ_TIMER_QUEST" 300 || ! wait_for 15 _f_has host "$TQ_TIMER_QUEST" 1000; then fail "teleport quests: set-up: the host's console set $TQ_TIMER_QUEST 200, which set 300 and stopped the quest (1000)" "host done $(_f_done host "$TQ_TIMER_QUEST")"; return; fi
	if wait_for 20 _tq_log_has vm "QuestSync: not setting $TQ_TIMER_QUEST .* stage 300: a timer's stage" && _tq_log_has vm "QuestSync: not setting $TQ_TIMER_QUEST .* stage 200: a timer's stage" \
		&& ! _f_has vm "$TQ_TIMER_QUEST" 200 && ! _f_has vm "$TQ_TIMER_QUEST" 300; then
		pass "teleport quests: the host's timer's end and stop (200, 300) are dropped in the VM's game" "$(grep -oE "QuestSync: not setting $TQ_TIMER_QUEST .* stage 300.*" "$A_OUT/windelta-tq-vm.txt" | head -1)"
	else
		fail "teleport quests: the host's timer's end and stop (200, 300) are dropped in the VM's game" "VM done $(_f_done vm "$TQ_TIMER_QUEST"); $(grep -E "QuestSync: (not setting|set) $TQ_TIMER_QUEST" "$A_OUT/windelta-tq-vm.txt" | tail -3 | tr '\n' '|')"
	fi
	if wait_for 20 _f_has vm "$TQ_TIMER_QUEST" 1000; then pass "teleport quests: the host's 1000 (the shutdown stage its stop ran: the guards put back) reaches the VM" "VM done $(_f_done vm "$TQ_TIMER_QUEST")"
	else fail "teleport quests: the host's 1000 (the shutdown stage its stop ran: the guards put back) reaches the VM" "VM done $(_f_done vm "$TQ_TIMER_QUEST"); host $(_f_done host "$TQ_TIMER_QUEST")"; fi
}

# ---- the shared workshop's figures: a friend sees the host's settlement figures (opt-in: smoke.sh --only sharedws-numbers) --------
# The host's game is the only one that counts its settlement (WorkshopNumbers.h): it tells the session its workbench's figures (power
# made and in use, food, water, defense, beds, people, happiness, and which of them the bar marks short) when they change, and a
# friend's game shows those in its workshop bar (the bar's thirteen engine calls answered with the host's) and in one line on entering
# workshop mode there; it puts none of the host's pieces' values on its own workbench (no mirror there). Asserted: with Sanctuary the
# host's, the VM holds the host's figures before anything is built; the host then builds a generator, a machinegun turret and a bed
# (console placement and the finish), its own figures move, and the VM's are the host's again, figure for figure; the VM's own
# workbench has no mirror of the host's values; the VM's workshop mode there says the line once with the host's figures, the bar's
# calls are answered (when its hooks are in) and the figures still hold after a few seconds of workshop mode (nothing local writes
# over them); and after the pieces are scrapped the VM's figures are the host's once more. dev wsnumbers reads both sides: what a
# friend's game shows, what the host's game tells. Console placement and SetOwnedByPlayer, said so; no saves.
SWN_FIELDS="power powerUsed food water defense beds population happiness target flags"
_swn_figures() {  # _swn_figures <who>: the figures that game shows (a friend's) or tells (the host's) of Sanctuary on one line; false when it has none
	local r f out=
	r=$(dc "$1" "wsnumbers $SANCT_BENCH")
	[ "$(field "$r" hosts)" = 1 ] || return 1
	for f in $SWN_FIELDS; do out="$out$f=$(field "$r" "$f") "; done
	echo "$out"
}
_swn_told() { [ "$(field "$(host "wsnumbers $SANCT_BENCH")" told)" = 1 ]; }   # the host's game has told the session the figures it has now
_swn_agree() {  # the host has told its figures as they are now and the VM shows the same
	local h v
	_swn_told || return 1
	h=$(_swn_figures host) || return 1
	v=$(_swn_figures vm) || return 1
	[ "$h" = "$v" ]
}
_swn_moved() { local h; h=$(_swn_figures host) || return 1; [ "$h" != "$1" ]; }   # _swn_moved <figures before>: the host's own figures are others now
_swn_build() {  # _swn_build <base> <what> <label>: a host piece placed at the host and finished; false (and a FAIL) when it wasn't
	local out p
	out=$(host "wsreport place $1 $SANCT_BENCH"); p=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
	if [ -z "$p" ]; then fail "$3: set-up: the host's $2 is placed" "$out"; return 1; fi
	_sws_placed host "$p"
	sleep 2; out=$(host "wsfinish $p $SANCT_BENCH")
	case "$out" in finished*) pass "$3: set-up: the host's finish ran its steps on the $2" "$p: $(cut -c1-140 <<<"$out")";; *) fail "$3: set-up: the host's finish ran its steps on the $2" "$out"; return 1;; esac
	sleep 1
}
_swn_said() { grep -E "WorkshopSync: workshop mode at the host's settlement $SANCT_BENCH: its figures said" "$(win_read swn vm)" | tail -1; }
step_sharedws_numbers() {
	section "shared workshop: a friend sees the host's settlement figures (console placement and SetOwnedByPlayer; no saves)"
	local r label="sharedws-numbers" hooks f h v h0 said0 ans0 line own
	r=$(dc host rules)
	if [ "$(field "$r" sharedWorkshop)" != true ]; then fail "$label: set-up: the host's bSharedWorkshop is on" "rules say sharedWorkshop='$(field "$r" sharedWorkshop)'"; return; fi
	r=$(vm wsnumbers)
	case "$r" in numbers:*) ;; *) fail "$label: set-up: the VM's build knows dev wsnumbers" "$(cut -c1-160 <<<"$r")"; return;; esac
	hooks=$(field "$r" hooks)
	expect_eq "$label: the VM's workshop bar is hooked (its thirteen calls for the open workbench's figures)" "$hooks" 13
	enter_both SanctuaryExt 14
	host "console $SANCT_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 2
	if [ "$(field "$(host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then fail "$label: set-up: Sanctuary is the host's" "$(host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer")"; return; fi
	vm "wsmode host" >/dev/null
	walk host "$label: set-up: the host walks to Sanctuary's workbench" 0.8 200 $SANCT_BENCH || { pads_off; return; }
	walk vm "$label: set-up: the VM walks to Sanctuary's workbench" 0.8 300 $SANCT_BENCH || { pads_off; return; }
	pads_off
	if _ws_enter host; then sleep 3; _ws_exit host; pass "$label: set-up: the host opened Sanctuary's workshop once (its workshop data made)" "ok"; else fail "$label: set-up: the host opened Sanctuary's workshop once" "$(host workshop)"; return; fi
	crash_snapshot
	# before anything is built: the host's game tells Sanctuary's figures (its player owns it), the VM holds them
	if wait_for 45 _swn_agree; then pass "$label: before any build the VM holds the host's figures of Sanctuary" "$(_swn_figures vm)"
	else fail "$label: before any build the VM holds the host's figures of Sanctuary" "host: $(host "wsnumbers $SANCT_BENCH" | cut -c1-330) | vm: $(vm "wsnumbers $SANCT_BENCH" | cut -c1-330)"; return; fi
	h0=$(_swn_figures host)
	win_mark swn host; win_mark swn vm
	# the host builds a generator, a machinegun turret and a bed
	_swn_build "$SWS_GENERATOR" generator "$label" || { _sws_cleanup; return; }
	_swn_build "$SWS_MG_TURRET" "machinegun turret" "$label" || { _sws_cleanup; return; }
	_swn_build "$SWS_BED" bed "$label" || { _sws_cleanup; return; }
	if wait_for 40 _swn_moved "$h0"; then pass "$label: set-up: the host's builds moved its own figures" "before: $h0| now: $(_swn_figures host)"
	else fail "$label: set-up: the host's builds moved its own figures (else the comparison below proves nothing new)" "still $h0| $(host "wsnumbers $SANCT_BENCH" | grep -oE 'ownPower=.*' | cut -c1-200)"; fi
	sleep 3   # the last of the three counted (the engine and the settlement's scripts count on their own time)
	wait_for 40 _swn_agree
	h=$(host "wsnumbers $SANCT_BENCH"); v=$(vm "wsnumbers $SANCT_BENCH")
	expect_eq "$label: after the builds the host's game has told the figures it has now" "$(field "$h" told)" 1
	expect_eq "$label: after the builds the VM holds figures of the host's for Sanctuary" "$(field "$v" hosts)" 1
	for f in $SWN_FIELDS; do
		expect_eq "$label: after the host's generator, turret and bed the VM's $f is the host's" "$(field "$v" "$f")" "$(field "$h" "$f")"
	done
	# the VM's own workbench: no mirror of the host's pieces' values on it (its own world's count is its own, and not what is shown)
	own=$(grep -oE 'ownPower=.*' <<<"$v" | cut -c1-200)
	expect_eq "$label: the VM's own workbench has no mirror of the host's values (the host's settlement)" "$(field "$v" mirrorOn)" 0
	pass "$label: (for the record) the VM's own world counts, never shown or told" "$own; mirror entries left out: $(field "$v" mirrorLeftOut)"
	# the VM's workshop mode at the host's settlement: the line once, the bar's calls answered with the host's figures
	said0=$(field "$(vm wsnumbers)" said); ans0=$(field "$(vm wsnumbers)" answers)
	if ! _ws_enter vm; then fail "$label: the VM enters workshop mode at the host's workbench" "$(vm workshop) | $(vm menus | grep -oE 'open=[^ ]*')"; _sws_cleanup; return; fi
	sleep 3
	r=$(vm wsnumbers)
	expect_eq "$label: the VM's workshop mode at the host's settlement says its figures once" "$(( $(field "$r" said) - said0 ))" 1
	line=$(_swn_said)
	h=$(host "wsnumbers $SANCT_BENCH")
	expect_eq "$label: the line said names the host's figures" "$(grep -oE 'people [0-9]+, food [0-9]+, water [0-9]+, power [0-9]+ of which [0-9]+ in use, defense [0-9]+, beds [0-9]+, happiness [0-9]+' <<<"$line")" \
		"people $(field "$h" population), food $(field "$h" food), water $(field "$h" water), power $(field "$h" power) of which $(field "$h" powerUsed) in use, defense $(field "$h" defense), beds $(field "$h" beds), happiness $(field "$h" happiness)"
	if [ "$hooks" = 13 ]; then
		expect_ge "$label: the workshop bar's calls were answered with the host's figures" "$(( $(field "$r" answers) - ans0 ))" 1 calls
		if grep -q "the bar shows them" <<<"$line"; then pass "$label: the VM's log says its bar shows the host's figures" "$(cut -c1-200 <<<"$line")"; else fail "$label: the VM's log says its bar shows the host's figures" "$(cut -c1-240 <<<"$line")"; fi
	fi
	sleep 5   # in workshop mode: nothing of the VM's own writes over what it holds
	if wait_for 10 _swn_agree; then pass "$label: after seconds of the VM's workshop mode its figures are still the host's" "$(_swn_figures vm)"
	else fail "$label: after seconds of the VM's workshop mode its figures are still the host's" "host: $(_swn_figures host)| vm: $(_swn_figures vm)"; fi
	_ws_exit vm
	crash_verdict "$label: the VM's workshop mode with the bar answered:"
	# the pieces scrapped: the figures go back, and the VM's with them
	h=$(_swn_figures host)
	_sws_cleanup
	if wait_for 40 _swn_moved "$h"; then pass "$label: set-up: the scraps moved the host's own figures" "before: $h| now: $(_swn_figures host)"
	else warn "$label: set-up: the scraps moved the host's own figures" "still $h"; fi
	if wait_for 40 _swn_agree; then pass "$label: after the scraps the VM's figures are the host's again" "$(_swn_figures vm)"
	else fail "$label: after the scraps the VM's figures are the host's again" "host: $(_swn_figures host)| vm: $(_swn_figures vm)"; fi
}

