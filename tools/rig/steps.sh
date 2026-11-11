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
# crash_snapshot <var-prefix>: remember both machines' crash logs
crash_snapshot() { C_HOST0=$(crash_list_host); C_VM0=$(crash_list_vm); }
crash_verdict() {  # crash_verdict <label>: no new crash log on either machine
	local hn vn h v; h=$(crash_list_host); v=$(crash_list_vm); hn=$(crash_new "$C_HOST0" "$h"); vn=$(crash_new "$C_VM0" "$v")
	case "$h" in ok*) ;; *) warn "$1 crash logs (host)" "listing unreadable";; esac
	case "$v" in ok*) ;; *) warn "$1 crash logs (vm)" "listing unreadable (ssh)";; esac
	if [ -n "$hn" ]; then fail "$1 crash logs (host)" "new: $(crash_names "$hn" | tr '\n' ' ')"; else pass "$1 crash logs (host)" "none new ($(crash_count "$h") on disk)"; fi
	if [ -n "$vn" ]; then fail "$1 crash logs (vm)" "new: $(crash_names "$vn" | tr '\n' ' ')"; else pass "$1 crash logs (vm)" "none new ($(crash_count "$v") on disk)"; fi
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
# (the reconnect step's sequence; logs in $A_OUT/<name>-vm.log and <name>-join.log)
_vm_restart_rejoin() {
	vmssh 'Stop-Process -Name Fallout4 -Force -ErrorAction SilentlyContinue; Start-Sleep 4; powershell -NoProfile -ExecutionPolicy Bypass -File C:\F4MP\in-vm-3-game.ps1 -WaitSeconds 150' > "$A_OUT/$1-vm.log" 2>&1
	vmssh 'Get-Content "$env:USERPROFILE\Documents\My Games\Fallout4\F4SE\F4Multiplayer_dev.token" -Raw' | tr -d '[:space:]' > "$STAGE/vm-dev.token"
	bash "$STAGE/rig.sh" join > "$A_OUT/$1-join.log" 2>&1
	sleep 3
}
step_preflight() {
	section "preflight"
	local a h v
	rig_up "both games up"; rig_ssh_up "ssh to the VM"
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
	local a n gun rows flagged tracks i ids
	enter_both CorvegaAssemblyPlant01 18
	echo "owners: host $(host 'net status' | grep -o 'npcs: owned=[0-9]* fast=[0-9]* mirrored=[0-9]*') vm $(vm 'net status' | grep -o 'npcs: owned=[0-9]* fast=[0-9]* mirrored=[0-9]*')"
	host "console player.equipitem 0015B043" >/dev/null; host "draw on" >/dev/null; sleep 1
	bash "$RIG/dcfast.sh" both "trace start 20" >/dev/null; host focus >/dev/null; sleep 0.5
	for n in 1 2 3 4 5 6; do
		a=$(host 'actors 3000' | grep -o "^[0-9A-F]\{8\}\|; [0-9A-F]\{8\}" | tr -d '; ' | head -3 | tail -1)
		[ -n "$a" ] && host "turnto $a 300 12" >/dev/null
		sleep 0.5; powershell -NoProfile -ExecutionPolicy Bypass -File "$INPUT_PS1" -Click -HoldMs 700 >/dev/null 2>&1; sleep 3.5
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
	sleep 4; hp_compare "after the host shot"
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
_throw() {  # _throw <who>: that player throws the grenade in hand (a real key)
	if [ "$1" = host ]; then host focus >/dev/null; sleep 1; powershell -NoProfile -ExecutionPolicy Bypass -File "$INPUT_PS1" -Key 0x12 -HoldMs 400 >/dev/null 2>&1
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
			_throw $who
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
# _trail_burst <shooter> <gun> <label> <clicks> <on|off> [beam]: the shooter fires that gun; its friend's game (the watcher) has its trails
# on or off. "beam": the gun's projectile is a tracer on every shot (the laser), so every trail must come out a tracer (trailTracers): a
# beam without the projectile's tracer flag gets no 3D and draws nothing.
_trail_burst() {
	local shooter=$1 gun=$2 label=$3 n=$4 mode=$5 beam=${6:-} watcher b a s0 s1 v i shots need ok=1
	watcher=$(other_game "$shooter")
	dc "$watcher" "trails $mode" >/dev/null
	dc "$shooter" "console player.equipitem $gun" >/dev/null; sleep 3; dc "$shooter" "draw on" >/dev/null; sleep 1.5; dc "$shooter" focus >/dev/null; sleep 0.5
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
	expect_zero "$label: no hit on the watcher's player by the stand-in (puppetHitsOnUs)" "$((a[4] - b[4]))"
	expect_zero "$label: no hit on any actor by the stand-in (puppetHits)" "$((a[9] - b[9]))"
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
step_trails() {
	section "friend shot trails (a stand-in's plain bullet and laser beam fly inert, both ways)"
	local who gun h0="$C_HOST0" v0="$C_VM0"   # the run's crash baseline, put back at the end: the end check counts from the start
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
	crash_verdict "friend shot trails"
	C_HOST0="$h0"; C_VM0="$v0"   # (it used to stay this step's, so a crash before the last trails step of a soak went unseen at the end)
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
_no_msgbox() { dc "$1" menus | grep -q 'MessageBoxMenu:' || return 0; dc "$1" "menu MessageBoxMenu hide" >/dev/null; return 1; }
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
# DN070's 5 (MoveTo the Jamaica Plain treasure's entry: kMoves) and DN036's 500 (Phyllis's faction turned on the player: kMoves|kHostile).
# Set-up by console, said so in the report: both games coc to SanctuaryExt, then the host's to FOLLOW_HOST_CELL (far: more than 6000 u).
# Accept: the host's console setstage DN070 5; the VM's log has the offer and the stage waits; the VM takes it with the teleport key's own
# action (dev `party go`, what the key calls; the offer aimed it at the host), and within FOLLOW_SET s its log has the stage set "after
# arriving beside player N" and the stage is done there. Decline: the VM's console coc back to SanctuaryExt (set-up); the host's console
# shows DN036's objective (a quest the player never started is not shared) and sets its 500; the VM's log has the fight offer,
# the stage is not set while it is open, and after OFFER_FOR the offer "was not taken up" and the stage is skipped. Quest stages are for good in a save: run
# it on throwaway saves (backup.sh before, restore after). Once a game has a stage done there is nothing to test: a FAIL naming it.
FOLLOW_MOVE_QUEST=${FOLLOW_MOVE_QUEST:-000E9914}; FOLLOW_MOVE_STAGE=${FOLLOW_MOVE_STAGE:-5}     # DN070
FOLLOW_FIGHT_QUEST=${FOLLOW_FIGHT_QUEST:-0008FCED}; FOLLOW_FIGHT_STAGE=${FOLLOW_FIGHT_STAGE:-500}  # DN036
FOLLOW_FIGHT_OBJECTIVE=${FOLLOW_FIGHT_OBJECTIVE:-10}  # DN036 500 alone leaves the quest unstarted (no objective shown), and a quest the player never started isn't shared: shown first
FOLLOW_HOST_CELL=${FOLLOW_HOST_CELL:-ConcordExt}; FOLLOW_SET=${FOLLOW_SET:-45}; FOLLOW_OFFER_FOR=${FOLLOW_OFFER_FOR:-90}
_f_done() { dc "$1" "stages $2" | sed -nE 's/.* done\([0-9]+\)=([0-9,]+|none).*/\1/p'; }
_f_has() { case ",$(_f_done "$1" "$2")," in *",$3,"*) return 0;; esac; return 1; }
_f_log_has() { grep -qE "$2" "$(win_read follow "$1")"; }   # _f_log_has <who> <regex>: the game's log since win_mark has it
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

	# Decline: the VM back at Sanctuary, far again; a fight offer it doesn't take.
	dc vm "console coc SanctuaryExt" >/dev/null; sleep 14; wait_ingame vm 30
	if ! wait_for 20 _f_far; then fail "follow: set-up: the VM's coc back to SanctuaryExt put it far from the host" "host $(dc host pos); VM $(dc vm pos)"; return; fi
	dc host "console setobjectivedisplayed $FOLLOW_FIGHT_QUEST $FOLLOW_FIGHT_OBJECTIVE 1" >/dev/null; sleep 2   # set-up: the host has the quest started
	win_mark follow vm
	dc host "console setstage $FOLLOW_FIGHT_QUEST $FOLLOW_FIGHT_STAGE" >/dev/null
	if ! wait_for 15 _f_has host "$FOLLOW_FIGHT_QUEST" "$FOLLOW_FIGHT_STAGE"; then fail "follow: set-up: the host's console set $FOLLOW_FIGHT_QUEST $FOLLOW_FIGHT_STAGE" "host done $(_f_done host "$FOLLOW_FIGHT_QUEST")"; return; fi
	if wait_for 20 _f_log_has vm "QuestSync: offer to follow player [0-9]+ into $FOLLOW_FIGHT_QUEST .* stage $FOLLOW_FIGHT_STAGE \(a faction turns on the player\)"; then
		pass "follow: the far VM is offered to join the host's fight ($FOLLOW_FIGHT_QUEST $FOLLOW_FIGHT_STAGE)" "worded as a fight"
	else
		fail "follow: the far VM is offered to join the host's fight ($FOLLOW_FIGHT_QUEST $FOLLOW_FIGHT_STAGE)" "no fight offer in the VM's log"; return
	fi
	sleep $((FOLLOW_OFFER_FOR - 20))
	if _f_has vm "$FOLLOW_FIGHT_QUEST" "$FOLLOW_FIGHT_STAGE"; then fail "follow: declined, the stage is not set while the offer is open" "done: $(_f_done vm "$FOLLOW_FIGHT_QUEST")"
	else pass "follow: declined, the stage is not set while the offer is open" "VM done $(_f_done vm "$FOLLOW_FIGHT_QUEST") after $((FOLLOW_OFFER_FOR - 20)) s"; fi
	if wait_for 45 _f_log_has vm "QuestSync: not setting $FOLLOW_FIGHT_QUEST .* stage $FOLLOW_FIGHT_STAGE: " \
		&& grep -qE "QuestSync: the offer to follow player [0-9]+ into $FOLLOW_FIGHT_QUEST .* was not taken up" "$A_OUT/windelta-follow-vm.txt" && ! _f_has vm "$FOLLOW_FIGHT_QUEST" "$FOLLOW_FIGHT_STAGE"; then
		pass "follow: not taken up in $FOLLOW_OFFER_FOR s, the stage is skipped in the VM's game" "$(grep -oE "QuestSync: not setting $FOLLOW_FIGHT_QUEST .*" "$A_OUT/windelta-follow-vm.txt" | head -1)"
	else
		fail "follow: not taken up in $FOLLOW_OFFER_FOR s, the stage is skipped in the VM's game" "VM done $(_f_done vm "$FOLLOW_FIGHT_QUEST"); $(grep -E "QuestSync: (set|not setting|the offer) " "$A_OUT/windelta-follow-vm.txt" | tail -3 | tr '\n' '|')"
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
		[ -n "$t" ] && { host "turnto $t 300 $p" >/dev/null; sleep 0.4; powershell -NoProfile -ExecutionPolicy Bypass -File "$INPUT_PS1" -Click -HoldMs 400 >/dev/null 2>&1; }
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
# _qa_compare <label> <host lines> <vm lines>: each alias's creature in both games, alive or dead alike, the same health while alive
_qa_compare() {
	local a r rv h v n=0 bad=0 odd="" gone=0
	while read -r a r; do
		[ -z "$r" ] && continue; rv=$(awk -v a="$a" '$1 == a { print $2 }' <<<"$3")
		if [ -z "$rv" ]; then odd="$odd alias $a: none in the VM's game;"; bad=$((bad + 1)); continue; fi
		h=$(actor_hp host "$r"); v=$(actor_hp vm "$rv")
		if [ -z "$h" ] && [ -z "$v" ]; then gone=$((gone + 1)); continue; fi   # gone from both games (its quest stopped and let it go)
		n=$((n + 1))
		if [ "$(field "$h" dead)" != "$(field "$v" dead)" ]; then bad=$((bad + 1)); odd="$odd alias $a $r/$rv dead=$(field "$h" dead)/$(field "$v" dead);"
		elif [ "$(field "$h" dead)" = false ] && ! awk -v a="$(field "$h" hp)" -v b="$(field "$v" hp)" -v t="$T_HP_DIFF" 'BEGIN{d=a-b; if(d<0)d=-d; exit !(a!="" && b!="" && d<=t)}'; then
			bad=$((bad + 1)); odd="$odd alias $a $r/$rv hp=$(field "$h" hp)/$(field "$v" hp);"
		fi
	done <<<"$2"
	if [ $n -eq 0 ]; then fail "$1" "no creature compared ($gone gone from both games):$odd"
	elif [ $bad -eq 0 ]; then pass "$1" "$n by alias, alike in both games$([ $gone -gt 0 ] && echo ", $gone gone from both")"
	else fail "$1" "$bad of $n differ:$odd"; fi
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
	_qa_compare "quest actors ($QA_QUEST): each creature alike in both games 10 s after the shots ($who in the host's game)" "$hl" "$vl"
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
}
# ---- downed again and again ------------------------------------------------------------------------------------------------------------------
# A player downed, helped up and downed again at once, over and over (as in a fire that keeps burning, a rocket engine's): each next
# down starts while the camera is still on its way back from the one before, or the body still getting up. The helper stands at the
# down player's stand-in the whole time (reached by a real walk), so each down is helped up after the rules' help time. The next lethal
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
step_downed_repeat() {
	local down=${1:-vm} rounds=${2:-5} helper puppet g v a0 a1 f n out hits=0
	helper=$(other_game "$down")
	section "downed again and again at once on each help-up ($down, $rounds rounds)"
	g=$(_god "$down" on); dc "$down" "console player.restoreav health 10000" >/dev/null
	puppet=$(dc "$helper" 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | head -1 | cut -d= -f2)
	[ -n "$puppet" ] || { fail "repeat ($down): set-up: the helper's game has the stand-in" "no puppet= in its net status"; return; }
	walk "$helper" "repeat ($down): set-up: the helper walks to the stand-in" 0.8 100 "$puppet" || return
	dc "$helper" "pad unplug" >/dev/null
	win_mark rep "$down"
	win_mark rep "$helper"
	g=$(_god "$down" off); [ "$g" = "god=false" ] || { fail "repeat ($down): set-up: god mode off" "$g"; return; }
	# the rounds run on the down player's own machine (tools/rig/downrepeat.ps1): over the tunnel a command is too slow for a get-up
	if [ "$down" = vm ]; then
		scp -q -i "$VM_KEY" -o ConnectTimeout=20 "$RIG/downrepeat.ps1" "$VM_SSH:downrepeat.ps1" 2>/dev/null
		out=$(vmssh "powershell -NoProfile -ExecutionPolicy Bypass -File downrepeat.ps1 -Rounds $rounds -Explosion $GRENADE_EXPLOSION")
	else out=$(powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$RIG/downrepeat.ps1")" -Rounds "$rounds" -Explosion "$GRENADE_EXPLOSION" | tr -d '\r'); fi
	sed 's/^/  /' <<<"$out"
	hits=$(grep -c ': helped up$' <<<"$out")
	expect_eq "repeat ($down): downed and helped up again and again" "$hits" "$rounds"
	g=$(_god "$down" on); dc "$down" "console player.restoreav health 10000" >/dev/null
	sleep 6   # the last get-up plays out, and a stuck transition would outlast the game's own (0.5-1 s) by far
	v=$(_view "$down"); expect_match "repeat ($down): the camera a first- or third-person view, alive" "$v" '^camera=(0|8) life=0$'
	sleep 3; v=$(_view "$down"); expect_match "repeat ($down): still a view 3 s on" "$v" '^camera=(0|8) life=0$'
	a0=$(_away "$down")
	walk "$down" "repeat ($down): set-up: the down player walks off" 1 100 $PARK_SPOT
	a1=$(_away "$down")
	if is_num "$a0" && is_num "$a1"; then expect_le "repeat ($down): the camera near the player, before and after a walk" "$(( a0 > a1 ? a0 : a1 ))" 400 u
	else pass "repeat ($down): the camera near the player" "not measured: this build's rules have no camera distance"; fi
	dc "$down" "pad button b" >/dev/null
	if menu_open "$down" PipboyMenu 4; then
		sleep 1; dc "$down" "pad button b" >/dev/null
		if wait_for 6 _pipboy_gone "$down"; then pass "repeat ($down): the Pip-Boy opens on a press and closes" "pad B twice"; else fail "repeat ($down): the Pip-Boy opens on a press and closes" "it opened and stayed open"; clear_menus "$down" >/dev/null; fi
	else fail "repeat ($down): the Pip-Boy opens on a press and closes" "no PipboyMenu after the pad's B ($(dc "$down" menus | grep -oE 'open=[^ ]*'))"; fi
	f=$(win_read rep "$down")
	if [ -s "$f" ]; then
		n=$(grep -c "Downed: helped up by player" "$f"); expect_ge "repeat ($down): its log has the help-ups" "$n" "$rounds"
		n=$(grep -c "way back looped" "$f"); expect_zero "repeat ($down): the camera's way back never looped (the push keeps the view)" "$n"
		pass "repeat ($down): other camera fixes in its log (counted only)" "pushes kept $(grep -c "its way back is the view" "$f"), strays $(grep -c "up, settled and out of any down" "$f"), stuck transitions $(grep -c "transition state has been on" "$f"), left-on $(grep -c "but the down left on" "$f")"
		local who lf first later
		for who in "$down" "$helper"; do
			if [ "$who" = "$down" ]; then lf=$f; else lf=$(win_read rep "$who"); fi
			[ -s "$lf" ] || continue
			read -r first later <<<"$(_far_reads "$lf")"
			if [ "$later" -gt 0 ]; then warn "repeat ($who's log): the camera far from the player (away over 1000) after the first down frame" "$later read(s)"; else pass "repeat ($who's log): no far camera read after the first down frame" "ok"; fi
			if [ "$first" -gt 0 ]; then warn "repeat ($who's log): the first down frame's camera read was far (the known one-frame engine flash, started off the main thread; counted, not a fail)" "$first read(s)"; fi
		done
	else fail "repeat ($down): its log window read" "no new lines"; fi
	walk "$down" "repeat: set-up: back to the marker" 1 100 $MARKER_SPOT; walk "$helper" "repeat: set-up: the helper back to the marker" 1 100 $MARKER_SPOT
	pads_off
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
# hands each game the same quest with the same settlement; each shows its objective, which starts the sharing. The game's forced start of
# a friend's run was refused for these two by the game in a VM that stood elsewhere (its log: "the game didn't start it with their fills"; the
# cause was not found), so the runs are joined by their same targets (RadiantSync: "it runs here with the same targets"). Asserted: the run is shared in both games with the
# same settlement, a stage the host sets is set once in each game, the reward stage gives the player an item in each game, and neither
# game makes or places a spawn for it.
# case: <edid> <quest id> <map marker to arrive at> <objective to show> <a middle stage> <reward stage> <end stage>
RAD_CASES=${RAD_CASES:-"DN121 00026340 0002C07B 1 100 900 1000;DN151 000482CD 0002B889 5 100 400 500"}
_rad_run() { dc "$1" "quests edid:$2" | tr ';' '\n' | grep -E " $2 " | grep -o 'running=[a-z0-9]*' | head -1; }
_rad_done() { dc "$1" "stages $2" | grep -o 'done([0-9]*)=[0-9a-z,]*' | sed 's/^done([0-9]*)=//'; }
_rad_inv() { dc "$1" inventory | grep -o ' items=[0-9]*' | sed 's/ items=//'; }
_rad_spawns() { dc "$1" 'net status' | grep -o 'spawns: ours=[0-9]* theirs=[0-9]* copies=[0-9]* reported=[0-9]* granted=[0-9]* refused=[0-9]* received=[0-9]* placed=[0-9]*'; }
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
_sws_copy_of() {  # _sws_copy_of <who> <builder's ref>: that game's copy of the other game's piece, by its key, or ""
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
		else fail "$label: the host built the VM's generator for it" "$(grep -E 'WorkshopSync: (player|request|our)' "$A_OUT/windelta-sws-host.txt" "$A_OUT/windelta-sws-vm.txt" | tail -3 | tr '\n' '|')"; return 1; fi
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
	else fail "$label: the host's grid holds the generator, the light and the wire" "generator $gc light $lc wire '$w': $(host "wsgrid $SANCT_BENCH" | cut -c1-200)"; fi
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
	# pieces, and the engine looks for wires only on them when one is scrapped), built beside the generator on the side toward the
	# host (the one-way step's light stands on the other side; the real hooks' one was scrapped); its own count (dl) read from the
	# host's budget before the wire, so that the scrap is seen to take the wire's count as well as its own.
	vg=$(_sws_copy_of vm "$(grep -oE "WorkshopSync: [0-9A-F]{8} \(base $SWS_GENERATOR\) built" "$A_OUT/windelta-sws-host.txt" "$A_OUT"/winmark-sws-host/0.log 2>/dev/null | head -1 | cut -d' ' -f2)")
	[ -z "$vg" ] && vg=$SWS_VG
	[ -n "$SWS_RH_WH" ] && host "wsscrap $SWS_RH_WH" >/dev/null   # the real hooks' construction light, had that step left it standing
	local hl hA hB dl dw
	hA=$(_sws_budget host); win_mark sws host; win_mark sws vm
	l=$(_sws_build_vm_beside "$SWS_WIRED_LIGHT" "$vg" -150 "$label (wire's end)"); [ -z "$l" ] && return 1
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
	vw0=$(_sws_copy_of vm "$w")
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
			else pass "$label (wire's end): set-up: the host confirms its scrap" "with $how: $kr"; fi
		else
			fail "$label (wire's end): the host's Scrap on its light, the wire's end, asks to confirm (ExamineConfirmMenu)" "never opened: pressed with $SWS_LAST_PRESS; now $(host menus | grep -oE 'open=[^ ]*'); $(grep -oE "WorkshopSync: workshop mode's Scrap key on $hl[^;]*" "$(win_read sws host)" | tail -1 | cut -c1-260)"
		fi
	fi
	sleep 2; wait_for 8 _no_msgbox host; _ws_exit host; sleep 2
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
	local who=$1 ref=$2 vk=$3 how=${4:-select} label=${5:-sharedws (real hooks)} t dz ds f r w w2 pk ok="" ago where c0 c1
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
	for f in 0:0 -4:0 4:0 -7:0 7:0 -2:0 2:0 0:-6 0:6 4:-6 4:6; do
		dz=$(( r * ${f%%:*} / 10 )); ds=$(( r * ${f#*:} / 10 ))
		[ "$f" = 0:0 ] || t=$(dc "$who" "turnto $ref 500 aim $dz $ds")
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
		fail "$label: set-up: $who's workshop mode reaches $ref (aimed at its 3D; $how)" "never, heights and sides tried within radius $r: ${where:-no measure of $ref} from the wire pick's last ray, ${ago:-?} ms old$( { is_int "$ago" && [ "$ago" -gt 3000 ]; } && echo ' (stale: the wire pick is not running; no wire held?)'); the engine's verdict: $(grep -oE '(canPlace|placeStatus|wireStatus|wireFrom|wireTarget)=[^ ]*' <<<"$w" | tr '\n' ' ')(wireStatus no-end: no second end its filter took; blocked: its cast along the wire met something; too-long; already-wired); picksAdmitted +$(( ${c1:-0} - ${c0:-0} )) while aiming; last turnto '$(grep -oE 'aim .*' <<<"$t")' | $(grep -oE 'wire pick runs=[^|]*' <<<"$pk" | cut -c1-1200)"
		return 1
	fi
	if [ -z "$ok" ]; then
		ago=$(grep -oE 'last run [0-9]+ ms ago' <<<"$pk" | grep -oE '[0-9]+')
		where="$(grep -oE "$ref: center [^|]*" <<<"$pk" | grep -oE '(radius|along|miss)=[0-9-]+' | tr '\n' ' ')$(grep -oE "$ref: bodies [^|]*" <<<"$pk" | grep -oE 'inCast=[a-z]+')"
		c1=$(_sws_counter "$who" picksAdmitted)
		fail "$label: set-up: $who's workshop mode reaches $ref (aimed at its 3D; $how)" "never, heights and sides tried within radius $r: ${where:-no measure of $ref} from the general pick's last ray, ${ago:-?} ms old$( { is_int "$ago" && [ "$ago" -gt 3000 ]; } && echo ' (stale: the general pick is not running; something held?)'); now $(_sws_fields "$w")picksAdmitted +$(( ${c1:-0} - ${c0:-0} )) while aiming; last turnto '$(grep -oE 'aim .*' <<<"$t")' | $(cut -c1-1400 <<<"$pk")"
		return 1
	fi
	SWS_LAST_PRESS="$(_sws_fields "$w2")key $vk: $(dc "$who" "key $vk 80")"
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
_sws_real_hooks() {  # _sws_real_hooks <generator copy on the VM> <light copy on the VM>
	local vg=$1 vl=$2 i b0 b1 asked0 asked1 g0 s0 c0 a0 w misses=0 label="sharedws (real hooks)" we wh vw hw=""
	crash_snapshot
	# The wire's second end must be a piece that takes a wire in any game: the engine's wire filter passes only a reference with the
	# workshopPowerConnection value or the WorkshopPowerConnection keyword, copy or not; a light bulb has neither (no player strings
	# a wire to one). So the VM builds a construction light (WorkshopConstructionLight01, which has the keyword and radiates no power,
	# so nothing connects it to the generator before the wire), a request the host builds; its copy is the second end. The keys are
	# tried on $vl, the copy of the one-way step's construction light (150 units beside the generator, on the side away from the host).
	# This one stands beside the generator on that same side but 300 units out (_sws_build_vm_beside): at 150 it stood exactly where
	# the one-way step's light stands and the pick took either; a step in front of the VM it stood in front of the generator and took
	# workshop mode's pick from it (and from the host's aims in the budget step). It is scrapped at the end of this step.
	win_mark sws host; win_mark sws vm
	we=$(_sws_build_vm_beside "$SWS_WIRED_LIGHT" "$vg" 300 "$label"); [ -z "$we" ] && return 1
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
	win_mark sws vm; win_mark sws host
	# Attach Wire as a player does: the generator's copy highlighted, the key (the wire starts there and is held: heldBase= the
	# wire, newItem=true); then the construction light's copy aimed at until the engine says the wire may go there (canPlace), the
	# key again (the wire placed, taken off and asked of the host: wiresAsked +1). The first wire asked ends the loop (a second
	# between the same ends would be one the host strings twice); a wire left in hand is put away before the next try, and two
	# failed tries in a row end it too (the reason is in their FAILs).
	for i in 1 2 3 4 5; do
		_sws_put_away vm "$label (wire $i)" || break
		if ! _sws_aim_press vm "$vg" "$WS_KEY_WIRE" select "$label (wire $i, first end)"; then misses=$((misses + 1)); [ "$misses" -ge 2 ] && break; continue; fi
		w=$(vm workshop)
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
	if grep -qE "wire filter admitted copy $vw " "$(win_read sws vm)"; then pass "$label: the engine's wire filter admitted the construction light's copy as the second end" "$(grep -oE "WorkshopSync: workshop mode.s wire filter admitted copy $vw [^;]*" "$(win_read sws vm)" | head -1 | cut -c1-260)"
	else fail "$label: the engine's wire filter admitted the construction light's copy as the second end" "no 'admitted copy $vw' line; wsmode: $(vm wsmode | grep -oE 'wire(PickRuns|Targets|FilterCopyYes|FilterCopyNo)=[0-9]+' | tr '\n' ' ')$(grep -oE "WorkshopSync: workshop mode.s wire filter refused copy $vw [^;]*" "$(win_read sws vm)" | head -1 | cut -c1-260)"; fi
	if [ "$((asked1 - asked0))" -ge 1 ]; then
		if wait_for 15 _sws_wire_answered vm; then pass "$label: the host strung the VM's real wire and answered it applied" "$(grep -oE 'WorkshopSync: request [0-9]+ \(wire\)[^;]*' "$(win_read sws vm)" | tail -1 | cut -c1-200)"
		else fail "$label: the host strung the VM's real wire and answered it applied" "$(grep -E 'WorkshopSync: (request|player)' "$(win_read sws host)" "$(win_read sws vm)" | grep -i wire | tail -3 | cut -c1-200 | tr '\n' '|')"; fi
		# wired: the host's grid holds the wire it answered with as an edge's connector (not just the light as a lone node), and the
		# light is powered by it (a construction light radiates and receives no power without one)
		hw=$(grep -oE 'WorkshopSync: request [0-9]+ \(wire\) at workbench [0-9A-F]{8}: applied, the host.s [0-9A-F]{8}' "$(win_read sws vm)" | tail -1 | grep -oE '[0-9A-F]{8}$')
		if [ -n "$hw" ] && wait_for 10 _sws_grid_has host "$wh" "$hw"; then pass "$label: the host's grid holds its construction light, wired (the wire $hw an edge's connector)" "$(host "wsgrid $SANCT_BENCH" | cut -c1-160)"
		else fail "$label: the host's grid holds its construction light, wired (the wire an edge's connector)" "wire '$hw': $(host "wsgrid $SANCT_BENCH" | cut -c1-200)"; fi
		if wait_for 20 _sws_host_powered "$wh"; then pass "$label: the host's construction light is powered by the wire" "$(host "wsinfo $wh" | grep -oE 'powered=[a-z]+|power=[0-9A-F]+' | tr '\n' ' ')"
		else fail "$label: the host's construction light is powered by the wire" "$(host "wsinfo $wh" | cut -c1-160)"; fi
	fi
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
	# press (the menu's own call for Accept and Store, the scrap request for Scrap: the button hints' every-frame tests count nothing)
	g0=$(_sws_counter vm grabsRefused); s0=$(_sws_counter vm storesRefused); c0=$(_sws_counter vm scrapsRefused)
	if _sws_aim_press vm "$vl" "$WS_KEY_SELECT" select "$label (grab)"; then
		expect_eq "$label: Accept (grab) on a copy is refused, once for one press" "$(( $(_sws_counter vm grabsRefused) - g0 ))" 1
		expect_eq "$label: after the refused grab workshop mode holds nothing (held=, not the highlight)" "$(field "$(vm workshop)" held)" 00000000
	fi
	# after each key, anything workshop mode was left holding is put away before the next (or the keys stop, loudly)
	local keys=1
	_sws_put_away vm "$label (after the grab)" || keys=""
	if [ -n "$keys" ] && _sws_aim_press vm "$vl" "$WS_KEY_SCRAP" select "$label (scrap)"; then
		expect_eq "$label: Scrap (XButton) on a copy is refused, once for one press" "$(( $(_sws_counter vm scrapsRefused) - c0 ))" 1
		# refused before the engine asks: its scrap confirmation (ExamineConfirmMenu) never opens for a copy
		if _menu_open vm ExamineConfirmMenu; then fail "$label: Scrap on a copy never asks to confirm (refused before ExamineConfirmMenu)" "the confirmation opened: pressed with $SWS_LAST_PRESS; cancelled"; dc vm "menu ExamineConfirmMenu cancel" >/dev/null; sleep 1
		else pass "$label: Scrap on a copy never asks to confirm (refused before ExamineConfirmMenu)" "$(vm menus | grep -oE 'open=[^ ]*' | cut -c1-120)"; fi
	fi
	# Repair and Assign are Accept's own path (an undamaged copy with no settler commanded comes to the grab, refused): no crash
	[ -n "$keys" ] && { _sws_put_away vm "$label (after the scrap)" || keys=""; }
	[ -n "$keys" ] && { _sws_aim_press vm "$vl" "$WS_KEY_REPAIR" select "$label (repair)"; _sws_put_away vm "$label (after repair)" || keys=""; }
	[ -n "$keys" ] && { _sws_aim_press vm "$vl" "$WS_KEY_ASSIGN" select "$label (assign)"; sleep 3; _sws_put_away vm "$label (after assign)" || keys=""; }
	# Store last: Cancel with nothing the store test lets go of is workshop mode's own Cancel (it may leave the menu). Judged only
	# when the key was pressed on the highlighted copy.
	if [ -n "$keys" ] && _sws_aim_press vm "$vl" "$WS_KEY_STORE" select "$label (store)"; then
		if [ "$(( $(_sws_counter vm storesRefused) - s0 ))" -ge 1 ]; then pass "$label: Store (Cancel) on a copy is refused" "storesRefused +$(( $(_sws_counter vm storesRefused) - s0 ))"; else fail "$label: Store (Cancel) on a copy is refused" "storesRefused did not move: pressed with $SWS_LAST_PRESS (map: $(cut -c1-160 <<<"$SWS_KEYMAP"))"; fi
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
	_vm_restart_rejoin
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
	_vm_restart_rejoin
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
_sws_cleanup() {
	local r who
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
_sws_stacks() { dc "$1" "contents $SANCT_BENCH" | grep -oE 'x[0-9]+' | tr -d x | awk '{ s += $1 } END { print s + 0 }'; }   # the items the bench holds in that game, all stacks summed
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
	a0=$(_sws_counter vm scrapsAsked); s0=$(_sws_counter host scrapsServed); g0=$(_sws_counter host served); o0=$(field "$(vm status)" offThread)
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
	expect_eq "$label: the confirmation's Yes ran on the VM's main thread (no off-thread call counted across it)" "$(field "$(vm status)" offThread)" "${o0:-0}"
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
	expect_eq "$label: the VM's own workbench is unchanged (nothing was scrapped there)" "$vc1" "$vc0"
	expect_eq "$label: the VM's inventory is unchanged" "$(field "$(vm inventory)" hash)" "$inv0"
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
	_vm_restart_rejoin
	enter_both SanctuaryExt 14
	_sws_cleanup
}

_rad_stage_once() { [[ ",$(_rad_done "$1" "$2")," == *",$3,"* ]]; }
_rad_taken() { dc "$1" "net status" | grep -o "radiants: runs=[^ ]* begun=[^ ]* joined=[^ ]* declined=[^ ]* sent=[^ ]* taken=[0-9]*" | sed "s/.*taken=//"; }
_rad_shared() { [[ $(dc "$1" "stages $2") == *"this run shared"* ]]; }
step_radiants() {
	section "radiants (story-manager runs shared by their run, both ways)"
	local entry edid id marker obj mid reward end who other h0 v0 d0 s0 st
	enter_both SanctuaryExt 14
	IFS=';' read -ra cases <<<"$RAD_CASES"
	for entry in "${cases[@]}"; do
		read -r edid id marker obj mid reward end <<<"$entry"
		if [ "$(_rad_run host "$edid")" != "running=false" ] || [ "$(_rad_run vm "$edid")" != "running=false" ] || [ "$(_rad_done host "$id")" != none ]; then
			warn "radiants ($edid): not run: a game has run it already (it runs once: restart both games)" "host $(_rad_run host "$edid") vm $(_rad_run vm "$edid")"; continue
		fi
		h0=$(_rad_inv host); v0=$(_rad_inv vm); s0="$(_rad_spawns host) / $(_rad_spawns vm)"
		for who in host vm; do dc "$who" "console player.moveto $marker" >/dev/null; wait_ingame "$who" 40; done
		for who in host vm; do
			if ! wait_for 30 _rad_is_running "$who" "$edid"; then fail "radiants ($edid): the story manager hands the quest to $who" "$(_rad_run "$who" "$edid")"; continue 2; fi
		done
		pass "radiants ($edid): both games run it" "arrived at $marker"
		for who in host vm; do dc "$who" "console setobjectivedisplayed $edid $obj 1" >/dev/null; done
		if wait_for 20 _rad_shared host "$id" && wait_for 20 _rad_shared vm "$id"; then pass "radiants ($edid): the run is shared in both games" "$(_rad_here host "$id")"
		else fail "radiants ($edid): the run is shared in both games" "host '$(dc host "stages $id" | cut -c1-90)' vm '$(dc vm "stages $id" | cut -c1-90)'"; continue; fi
		expect_eq "radiants ($edid): the same settlement in both games" "$(_rad_here vm "$id")" "$(_rad_here host "$id")"
		ns host; ns vm; h0=$(_rad_inv host); v0=$(_rad_inv vm); s0="$(_rad_spawns host) / $(_rad_spawns vm)"; st=$(_rad_taken vm)
		dc host "console setstage $edid $mid" >/dev/null
		if wait_for 12 _rad_stage_once vm "$id" "$mid"; then pass "radiants ($edid): the host's stage $mid is set in the other game" "vm done $(_rad_done vm "$id")"
		else fail "radiants ($edid): the host's stage $mid is set in the other game" "host $(_rad_done host "$id") vm $(_rad_done vm "$id")"; fi
		dc host "console setstage $edid $reward" >/dev/null
		wait_for 12 _rad_stage_once vm "$id" "$reward"
		[ "$end" != "$reward" ] && { dc host "console setstage $edid $end" >/dev/null; wait_for 12 _rad_stage_once vm "$id" "$end"; }
		sleep 3
		expect_eq "radiants ($edid): the same stages are done in both games" "$(_rad_done vm "$id")" "$(_rad_done host "$id")"
		d0=$(_rad_done vm "$id" | tr ',' '\n' | sort | uniq -d | tr '\n' ' ')
		expect_eq "radiants ($edid): no stage is set twice (the done list has each once)" "$d0" ""
		expect_between "radiants ($edid): the other game took each stage of the host's once" "$(( $(_rad_taken vm) - st ))" 1 "$(( $(_rad_done host "$id" | tr ',' '\n' | wc -l) ))" stage
		expect_ge "radiants ($edid): the reward is in the host's pack" "$(( $(_rad_inv host) - h0 ))" 1 item
		expect_ge "radiants ($edid): the reward is in the other game's pack" "$(( $(_rad_inv vm) - v0 ))" 1 item
		expect_eq "radiants ($edid): no spawns made or placed for it" "$(_rad_spawns host) / $(_rad_spawns vm)" "$s0"
	done
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
