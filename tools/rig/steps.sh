#!/bin/bash
# steps.sh: the standing two-game set as functions that assert (source assert.sh first; smoke.sh and hits.sh do).
# Each step_* drives the games the way the old soak scripts did (scripted real keys and clicks, console placement
# only to reach a scene, never injected or teleported movement in place of walking) and records named checks.
# Thresholds are variables so a run can say what it used; the reasons are in docs/RELEASING.md and the PR that added this.

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
step_preflight() {
	section "preflight"
	local a h v
	rig_up "both games up"; rig_ssh_up "ssh to the VM"
	pads_off   # a virtual pad left plugged by an interrupted run takes the keyboard and mouse from this one
	for a in host vm; do rig_keys_move $a "$a's keys move its player"; done
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
	local conts c name hb vb va ha i n vn taken=0 chests=0 notaken="" left="" kept=0 other=0 nomenu="" opened unread="" hunread="" skip cl
	: > "$A_OUT/looted.txt"
	cl=$(clear_menus vm) || { fail "loot: set-up: no menu open in the VM's game before looting" "$cl"; return; }
	[ -n "$cl" ] && echo "closed in the VM's game before looting: $cl" >> "$A_OUT/looted.txt"
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
		if [ -n "$skip" ]; then echo "$c $name: left out: $skip" >> "$A_OUT/looted.txt"; continue; fi
		# The take is pressed only once the container menu is open, and the activate is tried again once. A chest that opens by a
		# script of its own never opens the menu for a console activate: it is left out (a WARN), not counted as a take that failed.
		clear_menus vm >/dev/null; vm focus >/dev/null; sleep 0.3; opened=0
		for i in 1 2; do vm "console $c.activate player" >/dev/null; menu_open vm ContainerMenu 4 && { opened=1; break; }; echo "$c: activate $i opened no container menu" >> "$A_OUT/looted.txt"; done
		if [ $opened = 0 ]; then nomenu="$nomenu $c"; continue; fi
		chests=$((chests + 1)); printf '%s host %s | vm %s\n' "$c" "$hb" "$vb" >> "$A_OUT/looted.txt"
		# R (take all) also closes the menu: a Tab after it opens the Pip-Boy, and every activate after that opens nothing
		# (2026-11-01: one chest looted, then each one after it "opened no container menu"). Tab only while the menu is still up.
		sleep 0.5; vm "key 0x52 60" >/dev/null; sleep 1.5
		menu_open vm ContainerMenu 0 && { vm "key 0x09 60" >/dev/null; sleep 1; }
		cl=$(clear_menus vm); [ -n "$cl" ] && echo "$c: closed after the take: $cl" >> "$A_OUT/looted.txt"
		va=$(vm "contents $c"); if ! contents_ok "$va"; then unread="$unread $c"; elif has_items "$va"; then notaken="$notaken $c"; fi
		echo "$c vm after the take: $va" >> "$A_OUT/looted.txt"
	done <<<"$conts"
	sleep 5
	while read -r c _; do
		[ -n "$c" ] || continue
		vb=$(sed -n "s/^$c host .* | vm //p" "$A_OUT/looted.txt" | head -1); hb=$(sed -n "s/^$c host \(.*\) | vm .*/\1/p" "$A_OUT/looted.txt" | head -1)
		ha=$(host "contents $c"); echo "$c host after: $ha" >> "$A_OUT/looted.txt"
		contents_ok "$ha" || { hunread="$hunread $c"; continue; }   # no reading of the host's copy: nothing about it is known
		while read -r i vn; do
			[ -n "$i" ] || continue
			if ! shared_item "$i"; then kept=$((kept + 1)); continue; fi
			# each game rolls a chest on its own: an item the host's copy never held proves nothing (2026-10-31: no chest's two
			# rolls had an item in common, and the old count passed on items that were never there)
			[ "$(count_in "$hb" "$i")" -gt 0 ] || { other=$((other + 1)); continue; }
			taken=$((taken + 1)); n=$(count_in "$ha" "$i"); [ "$n" -gt 0 ] && left="$left $c:${i}x$n"
		done <<<"$(stacks_of "$vb")"
	done < <(grep -E "^[0-9A-F]{8} host " "$A_OUT/looted.txt")
	[ -n "$nomenu" ] && warn "loot: chests a console activate opens no container menu for (left out)" "$nomenu (each tried twice, or known to open by a script of its own)"
	if [ $chests -eq 0 ]; then fail "loot: VM takes all from the host's chests" "no chest within 2500 u could be stocked in both games and opened (reasons in looted.txt): nothing was looted (inconclusive)"; return; fi
	if [ -n "$unread" ]; then fail "loot: VM takes all from $chests chest(s) in the container menu" "the VM's contents after the take could not be read for:$unread${notaken:+; still holding items:$notaken}"
	elif [ -z "$notaken" ]; then pass "loot: VM takes all from $chests chest(s) in the container menu" "each empty in the VM's game after R"; else fail "loot: VM takes all from $chests chest(s) in the container menu" "still holding items in the VM's game:$notaken (the menu take did not happen)"; fi
	if [ -n "$hunread" ]; then fail "loot: the VM's takes reach the host's chests" "the host's contents after the takes could not be read for:$hunread (looted.txt): nothing is known of those"
	elif [ $taken -eq 0 ] && [ $other -gt 0 ]; then warn "loot: the VM's takes reach the host's chests" "the host's rolls held none of the $other shared stack(s) the VM took (each game rolls a chest on its own; what only one roll put there stays by design): nothing to see, inconclusive"
	elif [ $taken -eq 0 ]; then fail "loot: the VM's takes reach the host's chests" "the VM took no item of the kind the session shares (only weapons, armor, keys, notes, books: $kept stack(s)): inconclusive"
	elif [ -z "$left" ]; then pass "loot: the VM's takes reach the host's chests" "$taken shared stack(s) both games held gone from the host's copies; $other only the VM's roll had; $kept weapon/armor/personal stack(s) stay by design"
	else fail "loot: the VM's takes reach the host's chests" "still in the host's copy:$left (of $taken shared stacks taken; details in looted.txt)"; fi
}
# ---- terminals, loot, the safe -----------------------------------------------------------------------------------------
_useterm() { local who=$1 ref=$2; dc "$who" focus >/dev/null; sleep 0.5; dc "$who" "console $ref.activate player" >/dev/null; sleep 3; dc "$who" "key 0x0D 60" >/dev/null; sleep 2; dc "$who" "key 0x09 60" >/dev/null; sleep 1.5; clear_menus "$who" >/dev/null; sleep 1; }   # a blind second Tab opened the Pip-Boy when the first closed the terminal
_safe_unlock() {  # _safe_unlock <actor> : that player unlocks the Super-Duper Mart safe through the storage terminal
	local actor=$1 other safe term=001CA8C8 b_a b_o a_a a_o
	other=$([ "$actor" = host ] && echo vm || echo host)
	safe=$(host "refs 31 2500 Safe" | grep -o "^[0-9A-F]\{8\}"); [ -z "$safe" ] && safe=$(host "refs 31 2500" | grep -o "[0-9A-F]\{8\} base=[0-9A-F]* 'Safe'" | head -1 | cut -c1-8)
	if [ -z "$safe" ]; then fail "safe ($actor unlocks)" "the safe was not found near the player"; return; fi
	host "console $safe.lock 25" >/dev/null; vm "console $safe.lock 25" >/dev/null; host "console $term.unlock" >/dev/null; vm "console $term.unlock" >/dev/null; sleep 1
	b_a=$(dcf "$actor" "refinfo $safe" locked); b_o=$(dcf "$other" "refinfo $safe" locked)
	[ -n "$b_a" ] && [ -n "$b_o" ] && [ "$b_a" != "no" ] && [ "$b_o" != "no" ] && pass "safe ($actor unlocks): starts locked in both games" "$actor $b_a $other $b_o" || fail "safe ($actor unlocks): starts locked in both games" "$actor locked=$b_a $other locked=$b_o"
	dc "$actor" focus >/dev/null; sleep 1; dc "$actor" "console $term.activate player" >/dev/null; sleep 3
	dc "$actor" "key 0x0D 60" >/dev/null; sleep 2; dc "$actor" "key 0x0D 60" >/dev/null; sleep 2; dc "$actor" "key 0x09 60" >/dev/null; sleep 1.5; clear_menus "$actor" >/dev/null; sleep 3
	a_a=$(dcf "$actor" "refinfo $safe" locked); a_o=$(dcf "$other" "refinfo $safe" locked)
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
	if [ "$(dcf $d pa extraPA)" != "true" ]; then fail "power armor ($d): got into the frame" "extraPA is not true after 3 tries (frame $f)"; _pa_frames_off; return; fi
	pass "power armor ($d): got into the frame" "after $tries tries (frame $f)"
	ns $v
	hp=$(grep -o "puppet=FF[0-9A-F]*" <<<"${NS[$v]}" | head -1 | cut -d= -f2)
	s=$(grep -oE 'in=[a-z]* skel=[a-z]* tries=[0-9]* pieces=[0-9]*/[0-9]*' <<<"${NS[$v]}" | head -1)
	expect_eq "power armor ($d): the other game's stand-in is in the suit" "$(field "$s" in)/$(field "$s" skel)" "true/true"
	pieces=$(field "$s" pieces)
	if [ -n "$pieces" ] && [ "$(part "$pieces" 1)" = "$(part "$pieces" 2)" ] && [ "$(part "$pieces" 2)" != 0 ]; then pass "power armor ($d): stand-in wears all pieces" "$pieces"; else fail "power armor ($d): stand-in wears all pieces" "pieces='$pieces'"; fi
	if [ -z "$hp" ]; then fail "power armor ($d): stand-in found" "no puppet=FF.. in the other game's net status"; _pa_frames_off; return; fi
	_xy() { grep -o " [xy]=[-0-9]*" <<<"$1" | cut -d= -f2 | tr '\n' ' '; }
	_dist() { local hq; hq=$(dc $v "refinfo $hp"); read -r vx vy <<<"$(_xy "$(dc $d 'refinfo 00000014')")"; read -r hx hy <<<"$(_xy "$hq")"; awk -v a=$vx -v b=$vy -v c=$hx -v e=$hy 'BEGIN{printf "%d", sqrt((a-c)^2+(b-e)^2)}'; }
	# While moving, the driver and its stand-in are compared at the same moment: both games trace (dev `trace`, 20 Hz, each row
	# stamped with its machine's clock) and the driver's own path is read at each stand-in sample's time, the VM's clock offset
	# taken out (trace.sh clock). Reading the two games one after the other used to count up to ~0.3 s of running as distance.
	local clk clk0 clk1 did tr
	# trace.sh clock resyncs the VM's time service; a reading right after it once said 1078 ms and then -14 (2026-11-01: the step
	# still settling), so the offset is read again before and after the trace, and must hold still across it. A reading of -103
	# between two of -12 (2026-11-01) was this side stalling through a whole read, not the VM's clock: trace.sh offset now takes
	# a reading only once enough of its lines came through with no stall, and says unknown otherwise
	# A reading can come back empty (an ssh hiccup, 2026-11-05: before '', after -13): up to three tries, 2 s apart.
	_offset() { local o i; for i in 1 2 3; do o=$(bash "$RIG/trace.sh" offset 2>/dev/null); is_num "$o" && break; sleep 2; done; echo "$o"; }
	bash "$RIG/trace.sh" clock >/dev/null 2>&1; sleep 3; clk0=$(_offset)
	did=$(dcf $d 'net status' id)
	bash "$RIG/dcfast.sh" both "trace start 20" >/dev/null 2>&1; dc $d focus >/dev/null; sleep 0.3
	dc $d "key 0x57 3000" >/dev/null; dc $d "mouse 600 0" >/dev/null
	for i in $(seq 1 12); do
		case $i in 3) dc $d "key 0x57 2500" >/dev/null;; 5) dc $d "key 0x20 80" >/dev/null;; 6) dc $d "mouse 1587 0" >/dev/null; dc $d "key 0x57 3000" >/dev/null;; esac; sleep 0.6
	done
	sleep 1.5; host 'trace stop' >/dev/null; vm 'trace stop' >/dev/null; settled=$(_dist); clk1=$(_offset)
	clk=""; is_num "$clk0" && is_num "$clk1" && [ $(( clk1 > clk0 ? clk1 - clk0 : clk0 - clk1 )) -le 50 ] && clk=$(( (clk0 + clk1) / 2 ))
	cp -f "$HOST_F4SE/F4Multiplayer_trace.csv" "$A_OUT/pa-$d-host.csv" 2>/dev/null; scp -q -i "$VM_KEY" -o ConnectTimeout=20 "$VM_SSH:$VM_F4SE/F4Multiplayer_trace.csv" "$A_OUT/pa-$d-vm.csv" 2>/dev/null
	if ! is_num "$clk" || [ "${clk#-}" -gt 500 ]; then fail "power armor ($d): stand-in distance while moving, at the same moment" "the VM's clock offset is unknown, moved over 50 ms during the trace or is over 500 ms (before '$clk0', after '$clk1'): the two traces can't be lined up"
	elif [ -z "$did" ]; then fail "power armor ($d): stand-in distance while moving, at the same moment" "no player id in the driver's net status"
	else
		tr=$(pa_same_time "$A_OUT/pa-$d-$d.csv" "$A_OUT/pa-$d-$v.csv" "p$did" "$([ $d = vm ] && echo "$clk" || echo 0)" "$([ $v = vm ] && echo "$clk" || echo 0)")
		echo "$tr" > "$A_OUT/pa-$d-distance.txt"
		if [ "$(field "$tr" n)" -ge 40 ] 2>/dev/null; then
			expect_le "power armor ($d): stand-in distance while moving, at the same moment (max of $(field "$tr" n) samples; mean $(field "$tr" mean) u, p95 $(field "$tr" p95) u; VM clock $clk ms)" "$(field "$tr" max)" "$T_PA_MOVING" u
		else fail "power armor ($d): stand-in distance while moving, at the same moment" "too few stand-in samples over the driver's path ('$tr'): the trace did not cover the drive"; fi
	fi
	expect_le "power armor ($d): stand-in distance after stopping" "$settled" "$T_PA_SETTLED" u
	for tries in 1 2 3; do dc $d "mouse 0 -900" >/dev/null; sleep 0.3; dc $d "key 0x45 1300" >/dev/null; sleep 6; [ "$(dcf $d pa extraPA)" = "false" ] && break; done
	expect_eq "power armor ($d): climbed out" "$(dcf $d pa extraPA)" false
	ns $v; expect_eq "power armor ($d): the other game's stand-in is out of the suit" "$(grep -o 'pa=[a-z/]*' <<<"${NS[$v]}" | head -1)" "pa=false/false"
	_pa_frames_off
}

# _pa_frames_off: the frames step_pa placed (base 0004A9C1, and any copy the other game made of one) are taken out of both games
# near Sanctuary's bridge: left standing, a soak's next round walked into one on the doors and drops paths (2026-11-06).
_pa_frames_off() {
	local g r
	for g in host vm; do
		for r in $(dc $g "refs 42 1500" | tr ';' '\n' | grep "0004A9C1" | awk '{print $1}'); do
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

# wait_for <seconds> <command...>: runs the command every 0.5 s until it succeeds or the time is up
wait_for() { local end=$((SECONDS + $1)); shift; while :; do "$@" && return 0; [ $SECONDS -ge "$end" ] && return 1; sleep 0.5; done; }
pads_off() { dc host "pad unplug" >/dev/null; dc vm "pad unplug" >/dev/null; }
# is_int <value>: a whole number. A dev-channel reply that isn't one (an "error: timed out waiting for game thread") inside $(( )) is a
# syntax error, which ends the whole top-level if/while holding the step without a word: so every value read from a game is checked
# before arithmetic. Like is_num but without decimals, which $(( )) can't take either.
is_int() { [[ $1 =~ ^-?[0-9]+$ ]]; }
# walk <who> <check name> <speed> <radius> <target: ref | x y>: that game's player walks there (tools/rig/walkto.sh), up to 2 tries.
# One that never arrives FAILS under the name (WALK_LAST: how the walk ended). 0 = arrived.
walk() {
	local who=$1 label=$2 sp=$3 r=$4 try; shift 4
	for try in 1 2; do
		WALK_LAST=$(bash "$RIG/walkto.sh" "$who" "$@" "$sp" "$r" 60 2>&1 | tail -1)
		case "$WALK_LAST" in *": arrived "*) return 0;; esac
		sleep 1
	done
	fail "$label" "$who's walk to $* did not arrive, twice: $WALK_LAST"; dc "$who" "pad unplug" >/dev/null; return 1
}
# pitch_to <who> <ref>: degrees down from that player's eyes (120 u over its feet) to the reference's origin
pitch_to() {
	local p r; p=$(dc "$1" pos); r=$(dc "$1" "refinfo $2")
	awk -v px="$(field "$p" x)" -v py="$(field "$p" y)" -v pz="$(field "$p" z)" -v rx="$(field "$r" x)" -v ry="$(field "$r" y)" -v rz="$(field "$r" z)" \
		'BEGIN{ if (px == "" || rx == "") exit; printf "%d", atan2(120 + pz - rz, sqrt((rx - px) ^ 2 + (ry - py) ^ 2)) * 57.29578 }'
}
# press_a <who> <ref> <check> [pitch...]: that player faces the reference (turnto; "auto" is turnto's own pitch, at a standing body's
# chest) at each pitch in turn and presses the pad's A, until the check (a command line, run after each press) succeeds. PRESSES
# says how many it took. 0 = the check passed.
press_a() {
	local who=$1 ref=$2 check=$3 p n=0; shift 3
	for p in "${@:-auto}"; do
		n=$((n + 1)); PRESSES=$n
		if [ "$p" = auto ]; then dc "$who" "turnto $ref 300" >/dev/null; else dc "$who" "turnto $ref 300 $p" >/dev/null; fi
		sleep 0.8; dc "$who" "pad button a" >/dev/null
		eval "$check" && return 0
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
		if walk $taker "drop ($giver gives): set-up: the other player walks to the copy" 0.6 70 "$copy"; then
			p=$(pitch_to $taker "$copy"); is_int "$p" || p=50
			if press_a $taker "$copy" "wait_for 4 _count_is $taker $((tb + DROP_N))" "$p" $((p + 10)) $((p - 10)) 50 60; then
				pass "drop ($giver gives): the other player takes it with a press" "$tb -> $((tb + DROP_N)) after $PRESSES press(es)"
			else fail "drop ($giver gives): the other player takes it with a press" "count $tb -> $(dc $taker "count 00000014 $DROP_ITEM") after $PRESSES presses (copy: $(dc $taker "refinfo $copy" | cut -c1-120))"; fi
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
# down left its camera or input layer on, or looped the camera. God mode goes back on and health back to full after.
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
}

# ---- churn: cell hops with save and reload ------------------------------------------------------------------------------------
step_churn() {
	local mins=${1:-3} end half hopper=vm watcher=host i=0 lastsave c a
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
		a=$(alive); hops=$((hops + 1)); echo "$(date +%T) hop $i $c: $a"; case "$a" in *DOWN*) downs=$((downs + 1));; esac
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
	vmssh 'Stop-Process -Name Fallout4 -Force -ErrorAction SilentlyContinue; Start-Sleep 4; powershell -NoProfile -ExecutionPolicy Bypass -File C:\F4MP\in-vm-3-game.ps1 -WaitSeconds 150' > "$A_OUT/reconnect-vm.log" 2>&1
	vmssh 'Get-Content "$env:USERPROFILE\Documents\My Games\Fallout4\F4SE\F4Multiplayer_dev.token" -Raw' | tr -d '[:space:]' > "$STAGE/vm-dev.token"
	bash "$STAGE/rig.sh" join > "$A_OUT/reconnect-join.log" 2>&1
	sleep 3; ns vm; ns host
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
