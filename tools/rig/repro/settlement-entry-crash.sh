#!/bin/bash
# settlement-entry-crash.sh [phases...]: a crash entering a built-up settlement (player reports on 0.10.1: "crashes to desktop at
# Sanctuary every time, other settlements sometimes", and "on the host at one fixed place, also during fast travel"; 0.10.0 was
# fine). Both players in one session; Sanctuary built up by both of them and Red Rocket by both, with powered, wired pieces; then each
# player in turn enters Sanctuary and leaves it again, by fast travel and on foot, with saves in between, while the other stands in
# it, and both at once.
#
# Set-up (console, said so where it runs): both games coc SanctuaryExt; the host's Sanctuary made the host's and the VM's Red Rocket
# the VM's (WorkshopScript.SetOwnedByPlayer 1 in that game: a settlement each player has claimed). The pieces are built the engine's
# way short of the menu, as the rig's workshop steps do: each made a step in front of the builder (wsreport place ... quiet, the
# builder put at each spot of a grid by setpos) and placed with the engine's own steps (wsfinish: linked to the bench, budgeted, its
# scripts started, the placed event our sink reports), each light wired to its generator (wswire, the wire reported as placed). A
# cluster is a small generator and three lights (construction light, light box, bulb) with their wires: 7 pieces.
#   Sanctuary: SANCT_HOST clusters of the host's (its own settlement: copies in the VM's game) and SANCT_VM of the VM's, built with
#              the VM's shared-workshop rule off for the build (`wsmode off`, put back after): what a friend built there in their
#              own game before (copies in the host's game). Both games then hold a friend's powered copies to hush at a save.
#   Red Rocket: RR_VM clusters of the VM's (its own settlement) and RR_HOST of the host's (copies the other way).
# The workbenches are opened once in workshop mode first (dev `workshop enter`/`exit`): a wire needs the bench's workshop data, which
# the engine makes as workshop mode first opens there. Builds take hundreds of dev calls, so the build calls go over the game's
# socket from this shell (fq), not a new bash per call (dc: about 2.7 s a call on the rig, 2026-11-20).
# Travel is the game's own fast travel (Game.FastTravel on the map marker, through the console's cgf: the engine's fast travel with
# its time and its loading screen). The engine's travel autosave does not come after it on the rig (no "Save:" line, bSaveOnTravel=1
# on both machines), so a console save is sent with each travel: the dev channel holds it to the first frame after the loading
# screen, where the travel autosave would run (the arrival save); a second save follows ~15 s later. Walking in (the pad's stick,
# walkto.sh) needs WALK_ROUTE: no route from Red Rocket over the river into Sanctuary was found for the pad yet (stuck at the south
# bank near (-77600, 85000) and at Sanctuary's south edge near (-78600, 89000)), so the phase is skipped without one. Saves are
# console saves (SAVE_PREFIX, moved aside after the run by hand: never deleted).
# Phases (default all, in this order):
#   ft-host   the host travels Sanctuary -> Red Rocket -> Sanctuary ROUNDS times, a save at each end; the VM stands at Sanctuary
#   ft-vm     the same with the VM travelling; the host stands at Sanctuary
#   ft-both   both travel at once, ROUNDS times
#   walk      each player in turn (host, then VM): fast travel to Red Rocket, then walks into Sanctuary (WALKS times each), a save
#             on arrival
#   load      each player in turn: a save at Red Rocket, a travel to Sanctuary, then that save loaded and a travel to Sanctuary again
#             (the first arrival after a load), LOADS times each. A game with Creations loaded asks before the load ("continue
#             loading this save?"): answered Yes with a real Enter
# SANCT_HOST_FILE reuses the host's clusters already standing at Sanctuary in this game (one "<generator> <light>..." line each),
# from a start of this script stopped during its build. To stop a run, kill the script's own pid, not only the shell that started
# it (a stopped wrapper once left the script building on beside the next run).
# Run on 0.20.3 (2026-11-20): NO, 52 fast travels, 104 saves, 4 loads, both directions (checks/repro-settlement-entry-crash-run2).
# After every step both games must answer; a game down, a new crash log of the crash logger (crash-*.log), a new plugin crash
# report (F4Multiplayer-crash-*.txt) or a new Application Error for Fallout4.exe on either machine is a crash: its logs are kept.
# REPRO: YES on any crash (the step and the logs named); NO when every step of every phase ran with both games up and no crash;
# VOID when the set-up or a step could not be reached.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
. ../crashlib.sh
SANCT_BENCH=000250FE; RR_BENCH=00054BAE
SANCT_MARKER=${SANCT_MARKER:-}; RR_MARKER=${RR_MARKER:-}     # map marker references (found by name with dev `markers` when empty)
GEN=${GEN:-00030386}; LIGHTS=${LIGHTS:-"000D38D4 001F59AC 000560DF"}
SANCT_HOST=${SANCT_HOST:-18}; SANCT_VM=${SANCT_VM:-8}; RR_VM=${RR_VM:-6}; RR_HOST=${RR_HOST:-3}
ROUNDS=${ROUNDS:-5}; WALKS=${WALKS:-2}; LOADS=${LOADS:-2}
SAVE_PREFIX=${SAVE_PREFIX:-f4mp_entry}
# grids the builders stand on (x y z, columns, spacing): Sanctuary's open ground west of the bench, Red Rocket's lot
SANCT_GRID=${SANCT_GRID:-"-80700 89100 7900 6 320"}; RR_GRID=${RR_GRID:-"-71200 81500 7700 4 320"}
# the walk from Red Rocket into Sanctuary, "x y" points, ";"-separated (the road west, the bridge, the street to the bench)
WALK_ROUTE=${WALK_ROUTE:-}
PHASES=${*:-ft-host ft-vm ft-both walk load}
A_OUT=$REPRO_OUT; RUN_C_T0=$(date +%s)
need_session
say "settlement-entry-crash: phases $PHASES; Sanctuary host $SANCT_HOST + vm $SANCT_VM clusters, Red Rocket vm $RR_VM + host $RR_HOST; rounds $ROUNDS; out $REPRO_OUT"
say "games: host $(dc host 'net status' | grep -oE 'status=[a-z]+|hash=[0-9A-F]+' | tr '\n' ' ') | vm $(dc vm 'net status' | grep -oE 'status=[a-z]+|hash=[0-9A-F]+' | tr '\n' ' ')"

# ---- crashes ---------------------------------------------------------------------------------------------------------------------
# _ours <who>: the plugin's own crash reports there, "ok" then "name mtime" a line (as the hush probe's)
_ours() {
	local out t
	if [ "$1" = host ]; then echo ok; for f in "$HOST_F4SE"/F4Multiplayer-crash-*.txt; do [ -e "$f" ] && echo "$(basename "$f") $(stat -c %Y "$f")"; done | LC_ALL=C sort; return; fi
	for t in 1 2 3; do
		out=$(vmssh '"ok"; Get-ChildItem "$env:USERPROFILE\Documents\My Games\Fallout4\F4SE" -Filter "F4Multiplayer-crash-*.txt" -ErrorAction SilentlyContinue | ForEach-Object { $_.Name + " " + [DateTimeOffset]::new($_.LastWriteTimeUtc).ToUnixTimeSeconds() }')
		case "$out" in ok*) echo ok; printf '%s\n' "$out" | grep '^F4Multiplayer-crash-' | LC_ALL=C sort; return;; esac
		sleep 3
	done
}
RUN_C_HOST0=$(crash_list_host); RUN_C_VM0=$(crash_list_vm); O_HOST0=$(_ours host); O_VM0=$(_ours vm)
case "$RUN_C_VM0$O_VM0" in ok*ok*) ;; *) verdict VOID "the VM's crash logs or reports could not be listed before the run (ssh)";; esac
say "crash logs before: host $(crash_count "$RUN_C_HOST0") (newest $(crash_stamp "$RUN_C_HOST0")), vm $(crash_count "$RUN_C_VM0") (newest $(crash_stamp "$RUN_C_VM0")); plugin reports: host $(grep -c '^F4M' <<<"$O_HOST0"), vm $(grep -c '^F4M' <<<"$O_VM0")"
CRASHED=""
# _crashes: anything new since the run began, as one line ("" = none)
_crashes() {
	local hc vc ho vo he ve
	hc=$(crash_names "$(crash_new "$RUN_C_HOST0" "$(crash_list_host)" 2>/dev/null)" | tr '\n' ' ')
	vc=$(crash_names "$(crash_new "$RUN_C_VM0" "$(crash_list_vm)" 2>/dev/null)" | tr '\n' ' ')
	ho=$(comm -13 <(printf '%s\n' "$O_HOST0") <(_ours host) | grep '^F4M' | cut -d' ' -f1 | tr '\n' ' ')
	vo=$(comm -13 <(printf '%s\n' "$O_VM0") <(_ours vm) | grep '^F4M' | cut -d' ' -f1 | tr '\n' ' ')
	he=$(crash_event_lines "$(crash_events_host "$RUN_C_T0")" | tr '\n' ';'); ve=$(crash_event_lines "$(crash_events_vm "$RUN_C_T0")" | tr '\n' ';')
	echo "${hc:+host crash log: $hc}${vc:+vm crash log: $vc}${ho:+host report: $ho}${vo:+vm report: $vo}${he:+host app error: $he}${ve:+vm app error: $ve}"
}
# check <label>: both games up after a step; a game down (or anything new in the crash folders, looked at every CHECK_EVERY steps
# and at a game down) ends the run with YES, its logs kept
CHECK_EVERY=${CHECK_EVERY:-4}; STEPS=0
check() {
	local a c
	STEPS=$((STEPS + 1)); a=$(alive)
	case "$a" in *DOWN*) sleep 8; a=$(alive);; esac
	if [[ $a == *DOWN* ]] || [ $((STEPS % CHECK_EVERY)) = 0 ]; then c=$(_crashes); fi
	if [[ $a == *DOWN* ]] || [ -n "$c" ]; then
		CRASHED="$1: $a ${c}"
		say "CRASH at '$1': $a; $c"
		crash_keep "$1" | while read -r l; do say "$l"; done
		rkeep host end 2>/dev/null; rkeep vm end 2>/dev/null
		for w in host vm; do [ -n "$(dc $w status)" ] && rlog $w 60 > "$REPRO_OUT/tail-$w.log"; done
		verdict YES "a crash at '$1' ($a; $c)"
	fi
}

# ---- set-up -------------------------------------------------------------------------------------------------------------------
_loc() { dc "$1" status | grep -oE "location=[0-9A-F]+ '[^']*'" | head -1; }
_cellname() { field "$(dc "$1" 'refinfo 00000014')" cell; }
_marker() {   # _marker <name>: the reference of the map marker of exactly that name (dev `markers <name>`)
	dc host "markers $1" | grep -oE "[0-9A-F]{8} '$1'" | head -1 | cut -c1-8
}
[ -n "$SANCT_MARKER" ] || SANCT_MARKER=$(_marker "Sanctuary")
[ -n "$RR_MARKER" ] || RR_MARKER=$(_marker "Red Rocket Truck Stop")
[ -n "$SANCT_MARKER" ] && [ -n "$RR_MARKER" ] || verdict VOID "the map markers were not found (Sanctuary '$SANCT_MARKER', Red Rocket '$RR_MARKER': $(dc host 'markers Sanctuary' | cut -c1-200))"
say "map markers: Sanctuary $SANCT_MARKER, Red Rocket $RR_MARKER"
for w in host vm; do
	case "$(_loc $w)" in *Sanctuary*) ;; *) say "set-up: $w to Sanctuary (console coc SanctuaryExt)"; dc $w focus >/dev/null; dc $w "console coc SanctuaryExt" >/dev/null; sleep 6; wait_ingame $w 60;; esac
done
if [ "$(field "$(dc host "scriptvars $SANCT_BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then
	say "set-up: the host's Sanctuary made the host's (console $SANCT_BENCH.cf WorkshopScript.SetOwnedByPlayer 1)"
	dc host "console $SANCT_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 3
fi
say "rules: $(dc host rules | grep -oE 'sharedWorkshop=[a-z]+') ; host wsmode: $(dc host wsmode | cut -c1-100) ; vm wsmode: $(dc vm wsmode | cut -c1-100)"

# fq <who> <command>: one dev call over the game's socket from this shell (no new bash per call: the build makes hundreds); dc when
# the socket gives no answer
FQ_TOK_HOST=$(tr -d '\r\n ' < "$HOST_F4SE/F4Multiplayer_dev.token" 2>/dev/null)
fq() {
	local who=$1 port tok a out; shift
	if [ "$who" = host ]; then port=7790; tok=$FQ_TOK_HOST; else port=7792; tok=$(tr -d '\r\n ' < "$STAGE/vm-dev.token" 2>/dev/null); fi
	if exec 5<>/dev/tcp/127.0.0.1/$port 2>/dev/null; then
		echo "auth $tok" >&5; read -t 5 -r a <&5
		if [ "$a" = ok ]; then echo "$*" >&5; read -t 20 -r out <&5; exec 5>&-; [ -n "$out" ] && { echo "$out"; return 0; }; else exec 5>&-; fi
	fi
	dc "$who" "$@"
}
# _open_bench <who> <bench>: workshop mode opened once at that workbench and left (dev `workshop enter`, Enter on the first visit's
# tutorial box, `workshop exit`): the engine makes the bench's workshop data as workshop mode first opens there, which a wire needs
_open_bench() {
	local who=$1 i
	fq "$who" "workshop enter $2" >/dev/null; sleep 5
	for i in 1 2 3 4 5 6; do
		menu_open "$who" MessageBoxMenu && { dc "$who" focus >/dev/null; sleep 0.3; fq "$who" "key 0x0D 80" >/dev/null; sleep 1.5; }
		fq "$who" "workshop exit" >/dev/null; sleep 3
		[ "$(field "$(fq "$who" workshop)" menu)" = true ] || return 0
	done
	return 1
}
# _build <who> <bench> <grid "x y z cols spacing"> <clusters> <file>: that many clusters, each at the next spot of the grid; the pieces
# listed in the file a cluster to a line ("<generator> <light> <light> <light>"). Prints "<pieces> <finished>"
_build() {
	local who=$1 bench=$2 file=$5 x0 y0 z cols sp k out g l line fin=0 n=0 a i
	read -r x0 y0 z cols sp <<<"$3"
	for k in $(seq 0 $(($4 - 1))); do
		fq "$who" "console player.setpos x $((x0 + (k % cols) * sp))" >/dev/null; fq "$who" "console player.setpos y $((y0 + (k / cols) * sp))" >/dev/null
		fq "$who" "console player.setpos z $z" >/dev/null; sleep 1
		line=""; i=0
		for a in 0 90 180 270; do
			fq "$who" "console player.setangle z $a" >/dev/null
			if [ $a = 0 ]; then out=$(fq "$who" "wsreport place $GEN $bench quiet"); else i=$((i + 1)); out=$(fq "$who" "wsreport place $(cut -d' ' -f$i <<<"$LIGHTS") $bench quiet"); fi
			l=$(grep -oE "placed [0-9A-F]{8}" <<<"$out" | cut -c8-); [ -n "$l" ] || continue
			line="$line $l"; n=$((n + 1))
		done
		sleep 1.2
		for l in $line; do case "$(fq "$who" "wsfinish $l $bench")" in finished*) fin=$((fin + 1));; *) sleep 1; case "$(fq "$who" "wsfinish $l $bench")" in finished*) fin=$((fin + 1));; esac;; esac; done
		echo "${line# }" >> "$file"
	done
	echo "$n $fin"
}
# _wire <who> <bench> <file>: in each cluster of the file, each light wired to the generator (wswire, the wire reported as placed);
# the wires listed in <file>.wires. Prints how many
_wire() {
	local who=$1 bench=$2 g ls l out w n=0
	while read -r g ls; do
		for l in $ls; do
			out=$(fq "$who" "wswire $g $l"); w=$(grep -oE "^wire [0-9A-F]{8}" <<<"$out" | cut -c6-)
			[ -n "$w" ] || { echo "$who wswire $g $l: $out" >> "$REPRO_OUT/wire-errors.txt"; continue; }
			echo "$w" >> "$3.wires"; n=$((n + 1)); fq "$who" "wsreport $w $bench" >/dev/null
		done
	done < "$3"
	echo "$n"
}
# _wsmode_off / _wsmode_back <who>: the shared-workshop rule off in that game for a build, then back to what it was
_wsmode_off() { WSM_PREV=$(field "$(dc "$1" wsmode)" dev); dc "$1" "wsmode off" >/dev/null; }
_wsmode_back() { case "$WSM_PREV" in on|off|host) dc "$1" "wsmode $WSM_PREV" >/dev/null;; *) dc "$1" "wsmode host" >/dev/null;; esac; }

# SANCT_HOST_FILE: a file of clusters the host already placed at Sanctuary in this game (from an earlier start of this script that was
# stopped in its build): used instead of building the host's there again (wired here when they have no wires yet)
if [ -z "$SKIP_BUILD" ]; then
	say "set-up: building (console placement: builders put at each spot by setpos; the engine's finish; wires by wswire)"
	_open_bench host $SANCT_BENCH || say "set-up: the host's workshop mode at Sanctuary did not close ($(fq host workshop | cut -c1-80))"
	if [ -n "$SANCT_HOST_FILE" ]; then cp -f "$SANCT_HOST_FILE" "$REPRO_OUT/sanct-host.txt"; say "Sanctuary: the host's $(wc -l < "$SANCT_HOST_FILE") clusters placed earlier in this game ($SANCT_HOST_FILE)"
	else r=$(_build host $SANCT_BENCH "$SANCT_GRID" "$SANCT_HOST" "$REPRO_OUT/sanct-host.txt"); say "Sanctuary: the host built $r (pieces finished)"; fi
	say "Sanctuary: the host wired $(_wire host $SANCT_BENCH "$REPRO_OUT/sanct-host.txt") lights"
	_wsmode_off vm
	_open_bench vm $SANCT_BENCH || say "set-up: the VM's workshop mode at Sanctuary did not close ($(fq vm workshop | cut -c1-80))"
	SG=($SANCT_GRID); r=$(_build vm $SANCT_BENCH "${SG[0]} $((SG[1] + 4 * SG[4])) ${SG[2]} ${SG[3]} ${SG[4]}" "$SANCT_VM" "$REPRO_OUT/sanct-vm.txt")
	say "Sanctuary: the VM built $r, wired $(_wire vm $SANCT_BENCH "$REPRO_OUT/sanct-vm.txt") lights, with its shared-workshop rule off for the build"
	_wsmode_back vm; say "  the VM's rule back to '$WSM_PREV'"
	check "the build at Sanctuary"
	sleep 10
	say "Sanctuary copies: host $(dc host wspower | grep -oE '(mirrored|shown|graphWaits|graphRefused)=[0-9]+' | tr '\n' ' ') | vm $(dc vm wspower | grep -oE '(mirrored|shown|graphWaits|graphRefused)=[0-9]+' | tr '\n' ' ')"
	[ -s "$REPRO_OUT/wire-errors.txt" ] && say "wire errors: $(wc -l < "$REPRO_OUT/wire-errors.txt"), first: $(head -1 "$REPRO_OUT/wire-errors.txt")"
	# Red Rocket: both there by fast travel (the first travels of the run)
	for w in vm host; do dc $w focus >/dev/null; dc $w "console cgf \"Game.FastTravel\" $RR_MARKER" >/dev/null; sleep 3; done
	sleep 8; wait_ingame host 60; wait_ingame vm 60; sleep 6
	check "the first travel to Red Rocket"
	say "at Red Rocket: host $(_loc host) vm $(_loc vm)"
	if [ "$(field "$(dc vm "scriptvars $RR_BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then
		say "set-up: the VM's Red Rocket made the VM's (console $RR_BENCH.cf WorkshopScript.SetOwnedByPlayer 1 in the VM's game)"
		dc vm "console $RR_BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 3
	fi
	_open_bench vm $RR_BENCH || say "set-up: the VM's workshop mode at Red Rocket did not close"
	_open_bench host $RR_BENCH || say "set-up: the host's workshop mode at Red Rocket did not close"
	RG=($RR_GRID)
	r=$(_build vm $RR_BENCH "$RR_GRID" "$RR_VM" "$REPRO_OUT/rr-vm.txt"); say "Red Rocket: the VM built $r, wired $(_wire vm $RR_BENCH "$REPRO_OUT/rr-vm.txt")"
	r=$(_build host $RR_BENCH "${RG[0]} $((RG[1] + 3 * RG[4])) ${RG[2]} ${RG[3]} ${RG[4]}" "$RR_HOST" "$REPRO_OUT/rr-host.txt"); say "Red Rocket: the host built $r, wired $(_wire host $RR_BENCH "$REPRO_OUT/rr-host.txt")"
	check "the build at Red Rocket"
	sleep 10
	say "Red Rocket copies: host $(dc host wspower | grep -oE '(mirrored|shown)=[0-9]+' | tr '\n' ' ') | vm $(dc vm wspower | grep -oE '(mirrored|shown)=[0-9]+' | tr '\n' ' ')"
	for w in host vm; do dc $w focus >/dev/null; dc $w "console cgf \"Game.FastTravel\" $SANCT_MARKER" >/dev/null; sleep 3; done
	sleep 8; wait_ingame host 60; wait_ingame vm 60; sleep 8
	check "the travel back to Sanctuary after the build"
fi
for w in host vm; do dc $w "wspower hushlog on" >/dev/null; done
say "at Sanctuary: host $(_loc host) vm $(_loc vm); host $(dc host wspower | grep -oE '(mirrored|shown)=[0-9]+' | tr '\n' ' ') vm $(dc vm wspower | grep -oE '(mirrored|shown)=[0-9]+' | tr '\n' ' ')"

# ---- the moves ----------------------------------------------------------------------------------------------------------------
SAVES=0
# _save <who> <label>: a console save; its hush line from the log
_save() {
	local n
	SAVES=$((SAVES + 1)); n="${SAVE_PREFIX}_$1_$SAVES"
	rmark "$1" sv; dc "$1" "console save $n" >/dev/null; sleep 5; wait_ingame "$1" 30
	say "  $1 saved $n ($2): $(rsince "$1" sv | grep -E "friends' pieces shown unpowered for the save|SharedBeforeSave|Save: .* (begins|written)" | tail -2 | cut -c1-200 | tr '\n' '|')"
	LAST_SAVE[$1]=$n
}
declare -A LAST_SAVE
# _travel <who...> <marker> <label>: those players fast travel (each its own Game.FastTravel), wait for the arrival and the travel
# autosave, check both games, a save at the end
_travel() {
	local args=("$@") label=${args[-1]} m=${args[-2]} w ws=("${args[@]:0:${#args[@]}-2}")
	# The game's own travel autosave doesn't come on the rig after Game.FastTravel (no "Save:" line, 2026-11-20); a console save
	# sent right after the travel is held by the dev channel until the first frame after the loading screen and runs there, where
	# the travel autosave would: the arrival save
	for w in "${ws[@]}"; do rmark "$w" tr; dc "$w" focus >/dev/null; dc "$w" "console cgf \"Game.FastTravel\" $m" >/dev/null; SAVES=$((SAVES + 1)); dc "$w" "console save ${SAVE_PREFIX}_${w}_arrive_$SAVES" >/dev/null; done
	sleep 6; for w in "${ws[@]}"; do wait_ingame "$w" 60; done; sleep 8
	check "$label"
	for w in "${ws[@]}"; do
		say "  $w arrived ($label): $(_loc "$w"); arrival save: $(rsince "$w" tr | grep -E "Save: .* (begins|written)|friends' pieces shown unpowered" | head -3 | cut -c1-200 | tr '\n' '|')"
		_save "$w" "$label"
	done
	check "$label, the saves after"
}
for ph in $PHASES; do
	say "== phase $ph"
	case "$ph" in
	ft-host|ft-vm)
		w=${ph#ft-}
		for i in $(seq 1 "$ROUNDS"); do _travel "$w" "$RR_MARKER" "$ph $i: $w to Red Rocket"; _travel "$w" "$SANCT_MARKER" "$ph $i: $w into Sanctuary"; done ;;
	ft-both)
		for i in $(seq 1 "$ROUNDS"); do _travel host vm "$RR_MARKER" "$ph $i: both to Red Rocket"; _travel host vm "$SANCT_MARKER" "$ph $i: both into Sanctuary"; done ;;
	walk)
		[ -n "$WALK_ROUTE" ] || { say "walk: no WALK_ROUTE: phase skipped"; continue; }
		for w in host vm; do
			for i in $(seq 1 "$WALKS"); do
				_travel "$w" "$RR_MARKER" "walk $w $i: to Red Rocket"
				rmark "$w" wk; IFS=';' read -ra pts <<<"$WALK_ROUTE"; got=0
				for p in "${pts[@]}"; do rwalk "$w" $p 250 1.0 && got=$((got + 1)); say "  $w walk to $p: $WALKED"; check "walk $w $i: on the way ($p)"; done
				say "  $w walked into Sanctuary ($got of ${#pts[@]} points): $(_loc "$w") at $(pos_of "$w")"
				case "$(_loc "$w")" in *Sanctuary*) ;; *) say "  walk $w $i: not in Sanctuary at the end of the walk";; esac
				_save "$w" "walk $w $i: walked in"; check "walk $w $i: the save after walking in"
			done
		done ;;
	load)
		for w in host vm; do
			for i in $(seq 1 "$LOADS"); do
				_travel "$w" "$RR_MARKER" "load $w $i: to Red Rocket"; s=${LAST_SAVE[$w]}
				_travel "$w" "$SANCT_MARKER" "load $w $i: into Sanctuary"
				rmark "$w" ld; dc "$w" focus >/dev/null; dc "$w" "console load $s" >/dev/null
				# a game with Creations loaded asks first ("Creations are currently loaded ... continue loading this save?"): Yes (Enter)
				for k in $(seq 1 10); do
					menu_open "$w" MessageBoxMenu && { b=$(rmsgbox "$w"); case "$b" in *"continue loading"*) say "  $w: the game asked '$b': Yes (Enter)"; rkey "$w" $VK_ENTER; break;; esac; }
					rsince "$w" ld | grep -q 'Save: loading' && break; sleep 1
				done
				wait_for 60 eval 'rsince "$w" ld | grep -q "Save: loading"' || verdict VOID "load $w $i: the save $s was not loaded (no 'Save: loading' line in 60 s)"
				sleep 8; wait_ingame "$w" 60; sleep 8
				check "load $w $i: the save at Red Rocket ($s) loaded"
				say "  $w loaded $s: $(_loc "$w"); $(rsince "$w" ld | grep -E 'net|session|Catch' | tail -1 | cut -c1-160)"
				_travel "$w" "$SANCT_MARKER" "load $w $i: into Sanctuary, the first arrival after the load"
			done
		done ;;
	*) say "unknown phase $ph" ;;
	esac
done
c=$(_crashes); [ -z "$c" ] || { CRASHED="end: $c"; crash_keep end | while read -r l; do say "$l"; done; verdict YES "a crash: $c"; }
rkeep host end; rkeep vm end
say "hush lines: host $(grep -c "friends' pieces shown unpowered for the save" "$REPRO_OUT/end-host.log"), vm $(grep -c "friends' pieces shown unpowered for the save" "$REPRO_OUT/end-vm.log"); saves made $SAVES (${SAVE_PREFIX}_*)"
grep -hE "friends' pieces shown unpowered for the save" "$REPRO_OUT/end-host.log" | tail -2 | while read -r l; do say "host: ${l:0:220}"; done
grep -hE "friends' pieces shown unpowered for the save" "$REPRO_OUT/end-vm.log" | tail -2 | while read -r l; do say "vm: ${l:0:220}"; done
verdict NO "every step of phases '$PHASES' ran with both games up and no crash log, plugin crash report or Application Error on either machine"
