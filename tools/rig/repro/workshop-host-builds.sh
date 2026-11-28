#!/bin/bash
# workshop-host-builds.sh [phases...]: the host's buildings at a host-owned settlement reach the friend's game, and stay right for a
# friend who leaves and comes back (player reports: GitHub #601 "when building together my builds as the host don't show for the
# other player" on 0.17.0; GitHub #591 "if a player leaves a save and joins back after the host places or alters a building item it
# won't appear on their save", "when scrapping items some such as lamps/lanterns will appear floating", "if trying to share loot
# with another player sometimes it doesn't always show up in the same container").
# Set-up (console, said where it happens): both games at Sanctuary (coc SanctuaryExt, when not there), the host's game made the
# owner of Sanctuary (WorkshopScript.SetOwnedByPlayer, as the rig's checks do), the shared workshop on (the default). Everything
# else is real input: the host walks (the pad) to the workbench and presses E, picks every piece from the build menu with its keys,
# turns it green with real mouse moves and puts it down with Enter; moves a piece with E on it and scraps it with R and Enter.
# The friend never builds here. Only the loot is moved by the dev channel (storage put: the menu's own transfer), and the pieces
# this run built are scrapped at its end by the dev channel (wsscrap, reported), so the next run starts on clear ground.
# Phases (default all, in this order; ADOPT="ref:kind ..." names pieces an earlier run left, for a phase run alone):
#   build       both in workshop mode. The host builds a wood floor, a lamp, a container, a generator and a crafting station. Each:
#               the friend's game holds a copy within 10 s (a wall and the wire from the generator to the lamp: PIECES, but the
#               ghost of a lamp is hard to wire on the rig).
#   live        the friend present: the host moves the container, scraps the lamp and the floor, builds a floor and a lamp. The
#               friend's copies follow within 15 s, and nothing of a scrapped piece stands at its spot.
#   rejoin      the friend saves (console save), leaves workshop mode and the session (the menu's Leave: dev net leave); the host
#               does the same again (move, scrap, build); the friend joins again (the Steam join link). Its copies are the host's
#               layout, and nothing stands at a scrapped piece's or a moved piece's old spot.
#   stale       the friend loads that older save while in the session (a death's reload, an older save): the copies the save holds
#               are the host's layout from before its changes. Same checks.
#   containers  the host puts 7 stimpaks into the container it built and into the Toolbox the settlement came with, then the friend
#               3 into its copy of each (dev count): both games hold the same in each.
# REPRO: YES when a piece never reached the friend, a standing piece has no copy or one at another spot, a scrapped or moved
# piece's copy stands alive in the friend's game, or a container holds other loot in the two games; NO when none does; VOID when
# a step could not be done (a ghost never green, a piece not highlighted, the friend not joining): never a pass.
cd "$(dirname "${BASH_SOURCE[0]}")" || exit 2
. ./lib.sh
BENCH=000250FE
run_cap 5400
PHASES=${*:-build live rejoin stale containers}
# kind:menu path[:base]
PIECES=${PIECES:-"floor:STRUCTURES,WOOD,FLOORS lamp:POWER,LIGHTS:0022D2CD container:FURNITURE,CONTAINERS generator:POWER,GENERATORS station:CRAFTING"}
need_session
say "workshop-host-builds: phases $PHASES; out $REPRO_OUT"
[ "$(field "$(dc host rules)" sharedWorkshop)" = true ] || verdict VOID "the host's bSharedWorkshop is off"
HOSTID=$(grep -oE 'joinlobby/377160/\$lobby/[0-9]+' "$STAGE/rig.sh" | grep -oE '[0-9]+$' | head -1)
[ -n "$HOSTID" ] || verdict VOID "the host's Steam id for the join link is not in \$STAGE/rig.sh"
rmark host; rmark vm
FAILS=""; RESULTS=""
fail() { FAILS="$FAILS; $*"; say "FAIL: $*"; }
declare -A KIND BASE      # host ref -> kind, base
STANDING=""               # the host's refs of pieces standing now, built through the menu
GEN=""; LIGHT=""
# ADOPT="ref:kind ...": pieces the host's game already has from an earlier run of this script (its kinds), for phases run alone
for a in $ADOPT; do KIND[${a%%:*}]=${a#*:}; STANDING="$STANDING ${a%%:*}"; done

# ---- set-up ----------------------------------------------------------------------------------------------------------------------
for w in host vm; do
	case "$(dc $w status | grep -oE "location=[0-9A-F]+ '[^']*'")" in *Sanctuary*) ;; *) say "set-up: $w to Sanctuary (console coc SanctuaryExt)"; dc $w focus >/dev/null; dc $w "console coc SanctuaryExt" >/dev/null; sleep 5; wait_ingame $w 60;; esac
done
if [ "$(field "$(dc host "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then
	say "set-up: the host's Sanctuary made the host's (console $BENCH.cf WorkshopScript.SetOwnedByPlayer 1)"
	dc host "console $BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 3
fi
say "Sanctuary: host OwnedByPlayer=$(field "$(dc host "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer), friend's game OwnedByPlayer=$(field "$(dc vm "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)"

# ---- real-menu helpers (as workshop-build-together.sh's) ---------------------------------------------------------------------------
_in_ws() {
	_ws_in "$1" && return 0
	ws_enter_real "$1" "$BENCH" && { say "$1 in workshop mode by its real E ($ENTER_SAID)"; return 0; }
	# a walk that ends wedged among Sanctuary's clutter (a player's own way out is a reload): the game comes to the cell's start by console, said
	say "set-up: $1's walk to the workbench ended $BENCH_DIST u off (console coc SanctuaryExt, then the walk again)"
	dc "$1" focus >/dev/null; dc "$1" "console coc SanctuaryExt" >/dev/null; sleep 6; wait_ingame "$1" 60
	ws_enter_real "$1" "$BENCH" && { say "$1 in workshop mode by its real E ($ENTER_SAID)"; return 0; }
	say "$1's real E opened no workshop mode ($ENTER_SAID; rollover '$ROLLOVER')"; return 1
}
# _top <who>: to the menu's top (a held ghost in a leaf level, as CRAFTING's, put away first with a Tab)
_top() { local who=$1; [ "$(field "$(ws_state "$who")" held)" != 00000000 ] && { rkey "$who" $VK_TAB; sleep 1; }; ws_top "$who"; }
_pick() {
	local who=$1 path=$2 base=$3 i g
	GHOST=""
	_top "$who" || { PICK_WHY="menu-top ($(ws_path "$who"))"; return 1; }
	# shellcheck disable=SC2046
	ws_nav "$who" $(tr ',' ' ' <<<"$path") || { PICK_WHY="menu ($WS_NAV)"; return 1; }
	if [ -n "$base" ]; then for i in $(seq 1 12); do [ "$(field "$(ws_state "$who")" heldBase)" = "$base" ] && break; rkey "$who" $VK_RIGHT; sleep 0.7; done; fi
	g=$(field "$(ws_state "$who")" held)
	[ -n "$g" ] && [ "$g" != 00000000 ] || { PICK_WHY="no ghost ($WS_NAV)"; return 1; }
	GHOST=$g; GHOSTBASE=$(field "$(ws_state "$who")" heldBase)
}
_put() {
	local who=$1 g=$GHOST
	PUT=""
	ws_green "$who" || { PUT_WHY="never green ($(ws_state "$who" | grep -oE 'canPlace=[a-z]+ placeStatus=[0-9]+'))"; return 1; }
	rkey "$who" $VK_ENTER; sleep 1.5
	if [ "$(field "$(ws_state "$who")" held)" = "$g" ] || [ "$(field "$(dc "$who" "refinfo $g")" deleted)" != false ]; then PUT_WHY="Enter put nothing down ($(ws_state "$who" | cut -c1-140))"; return 1; fi
	PUT=$g
}
_has_copy() { [ -n "$(ws_copy_of vm "$1")" ]; }
# _build <phase> <kind> <path> [base]: the host builds one through the menu; the friend's copy within 10 s (when it is in the session)
_build() {
	local ph=$1 kind=$2
	local spot try
	for try in 0 1 2 3; do
		# a ghost never green where the host stands (earlier pieces about): it walks (a real walk) to another stretch of open ground
		if [ "$try" -gt 0 ]; then spot=$(cut -d' ' -f$try <<<"$SPOTS" | tr ',' ' '); say "$ph $kind: $PUT_WHY: the host walks to $spot"; _top host; rwalk host $spot 80; fi
		_pick host "$3" "$4" || { say "$ph $kind: $PICK_WHY"; RESULTS="$RESULTS $ph/$kind:void"; return 1; }
		_put host && break
		[ "$try" = 3 ] && { say "$ph $kind: $PUT_WHY"; RESULTS="$RESULTS $ph/$kind:void"; return 1; }
	done
	local p=$PUT; GONE=${GONE//$p/}   # a reference id is made again after a scrap: the id now names this piece
	KIND[$p]=$kind; BASE[$p]=$GHOSTBASE; STANDING="$STANDING $p"; LAST=$p
	say "$ph $kind: the host put down $p (base $GHOSTBASE) at $(field "$(dc host "refinfo $p")" x) $(field "$(dc host "refinfo $p")" y) $(field "$(dc host "refinfo $p")" z)"
	[ "$(field "$(dc vm 'net status')" status)" = joined ] || return 0
	if wait_for 10 _has_copy "$p"; then say "$ph $kind: friend's copy $(ws_copy_of vm "$p")"; RESULTS="$RESULTS $ph/$kind:ok"
	else say "$ph $kind: host $p NOT in the friend's game after 10 s; friend's net: $(dc vm 'net status' | grep -oE 'workshop: [^|]*' | cut -c1-240)"; RESULTS="$RESULTS $ph/$kind:MISSING"; fail "$ph $kind $p never reached the friend"; fi
}
_wire() {
	local ph=$1 a=$2 b=$3 wire
	_top host
	dc host "turnto $a 300 aim" >/dev/null; sleep 1
	[ "$(field "$(ws_state host)" selected)" = "$a" ] || { rwalk host "$a" 150 0.5; dc host "turnto $a 300 aim" >/dev/null; sleep 1; }
	[ "$(field "$(ws_state host)" selected)" = "$a" ] || { say "$ph wire: the generator $a is not highlighted ($(ws_state host | grep -oE 'selected=[0-9A-F]+'))"; RESULTS="$RESULTS $ph/wire:void(aim)"; return 1; }
	rkey host $VK_SPACE; sleep 1.5
	wire=$(field "$(ws_state host)" held)
	dc host "turnto $b 600" >/dev/null; sleep 1
	for p in 15 25 35 45; do pitch_down host $p; sleep 0.5; [ "$(field "$(ws_state host)" wireStatus)" = ok ] && break; done
	rkey host $VK_SPACE; sleep 2
	if [ "$(field "$(ws_state host)" held)" != 00000000 ] || [ "$(field "$(dc host "refinfo $wire")" deleted)" != false ]; then say "$ph wire: not strung ($(ws_state host | cut -c1-200))"; RESULTS="$RESULTS $ph/wire:void(string)"; [ "$(field "$(ws_state host)" held)" != 00000000 ] && rkey host $VK_TAB; return 1; fi
	KIND[$wire]=wire; STANDING="$STANDING $wire"; LAST=$wire
	if wait_for 10 _has_copy "$wire"; then say "$ph wire: friend's copy $(ws_copy_of vm "$wire")"; RESULTS="$RESULTS $ph/wire:ok"
	else say "$ph wire: host $wire NOT in the friend's game after 10 s"; RESULTS="$RESULTS $ph/wire:MISSING"; fail "$ph wire $wire never reached the friend"; fi
}
# _select <who> <ref>: that player walks (a real walk) to within reach of the piece and looks at it until workshop mode highlights it
# (the look turned on to the piece's middle, then swept up and down in small real mouse moves: the middle of a small piece, a lamp,
# is not always where the game's highlight finds it). 0 = highlighted
_select() {
	local who=$1 p=$2 m d side px py r
	dc "$who" "mouse 0 -1500" >/dev/null; sleep 0.5
	r=$(dc "$who" "refinfo $p"); px=$(field "$r" x); py=$(field "$r" y); px=${px%.*}; py=${py%.*}
	for side in "" "250 0" "-250 0" "0 250" "0 -250"; do
		if [ -n "$side" ]; then set -- $side; rwalk "$who" $((px + $1)) $((py + $2)) 80
		else d=$(dist_xy "$(pos_of "$who")" "$px $py"); [ "${d:-9999}" -le 450 ] || rwalk "$who" "$p" 300 0.6; fi
		dc "$who" "turnto $p 300 aim" >/dev/null; sleep 1
		[ "$(field "$(ws_state "$who")" selected)" = "$p" ] && return 0
		for m in 40 40 40 40 -240 -40 -40 -40 -40 -40; do
			dc "$who" "mouse 0 $m" >/dev/null; sleep 0.6
			[ "$(field "$(ws_state "$who")" selected)" = "$p" ] && return 0
		done
	done
	return 1
}
# _scrap <who> <ref>: that player highlights the piece and presses the scrap key (R); the confirmation (ExamineConfirmMenu) is
# answered with Enter (Yes). 0 = the piece is gone (deleted) in that game
_scrap() {
	local who=$1 p=$2 i
	_top "$who"
	_select "$who" "$p" || { SCRAP_WHY="not highlighted ($(ws_state "$who" | grep -oE 'selected=[0-9A-F]+'))"; return 1; }
	rkey "$who" 0x52; sleep 1.5
	menu_open "$who" ExamineConfirmMenu && { rkey "$who" $VK_ENTER; sleep 2; }
	for i in 1 2 3 4 5 6; do [ "$(field "$(dc "$who" "refinfo $p")" deleted)" != false ] && return 0; sleep 1; done
	SCRAP_WHY="R scrapped nothing ($(ws_state "$who" | cut -c1-160); menus $(menus "$who"))"; return 1
}
# _move <who> <ref>: E on the highlighted piece (grabbed), the look turned a few degrees, Enter. 0 = it is at another spot now
_move() {
	local who=$1 p=$2 x0 y0 x1 y1
	_top "$who"
	x0=$(field "$(dc "$who" "refinfo $p")" x); y0=$(field "$(dc "$who" "refinfo $p")" y)
	_select "$who" "$p" || { MOVE_WHY="not highlighted"; return 1; }
	rkey "$who" $VK_E; sleep 1.5
	[ "$(field "$(ws_state "$who")" held)" = "$p" ] || { MOVE_WHY="E grabbed nothing ($(ws_state "$who" | cut -c1-160))"; return 1; }
	GHOST=$p
	dc "$who" "mouse 300 0" >/dev/null; sleep 1
	ws_green "$who" || { MOVE_WHY="never green"; rkey "$who" $VK_TAB; return 1; }
	rkey "$who" $VK_ENTER; sleep 2
	x1=$(field "$(dc "$who" "refinfo $p")" x); y1=$(field "$(dc "$who" "refinfo $p")" y)
	[ "$(dist_xy "$x0 $y0" "$x1 $y1")" -gt 30 ] && return 0
	MOVE_WHY="the piece is still at $x1 $y1 ($(ws_state "$who" | cut -c1-120))"; return 1
}
GONE=""
_layout() {
	local label=$1 p c hr cr bad=0 ok=0 hx hy hz cx cy cz
	for p in $STANDING; do
		c=$(ws_copy_of vm "$p")
		if [ -z "$c" ]; then say "$label: $p (${KIND[$p]:-?}) has NO copy in the friend's game"; fail "$label: the host's ${KIND[$p]:-piece} $p has no copy in the friend's game"; bad=$((bad + 1)); continue; fi
		hr=$(dc host "refinfo $p"); cr=$(dc vm "refinfo $c")
		hx=$(field "$hr" x); hy=$(field "$hr" y); hz=$(field "$hr" z); cx=$(field "$cr" x); cy=$(field "$cr" y); cz=$(field "$cr" z)
		d=$(dist_xy "${hx%.*} ${hy%.*}" "${cx%.*} ${cy%.*}")
		if [ "$(field "$cr" deleted)" != false ] || [ "${d:-9999}" -gt 20 ]; then say "$label: $p (${KIND[$p]}) at $hx $hy $hz, friend's copy $c at $cx $cy $cz deleted=$(field "$cr" deleted)"; fail "$label: ${KIND[$p]} $p is not where the host has it in the friend's game"; bad=$((bad + 1)); else ok=$((ok + 1)); fi
	done
	for p in $GONE; do
		c=$(ws_copy_of vm "$p")
		if [ -n "$c" ] && [ "$(field "$(dc vm "refinfo $c")" deleted)" = false ]; then say "$label: scrapped ${KIND[$p]} $p still has the copy $c standing in the friend's game"; fail "$label: scrapped ${KIND[$p]} $p still stands in the friend's game"; bad=$((bad + 1)); fi
	done
	say "$label: $ok standing pieces match, $bad wrong (host's $(echo $STANDING | wc -w) standing, $(echo $GONE | wc -w) scrapped)"
}
# the friend's lamp-like leftovers: references in the friend's game at a spot (all types 20..60 scanned) that are alive and no copy of a standing piece
# (_leftovers <who> <x> <y> <radius>)
_leftovers() {
	local who=$1 x=$2 y=$3 r=$4 t out
	for t in $(seq 20 60); do
		out=$(dc "$who" "refs $t 6000")
		[ "$out" = none ] && continue
		tr ';' '\n' <<<"$out" | awk -v x="$x" -v y="$y" -v r="$r" -v t="$t" '{ n = split($0, w, " "); for (i = 1; i <= n; i++) if (w[i] == "at") { dx = w[i+1] - x; dy = w[i+2] - y; if (dx*dx + dy*dy <= r*r) print "type " t ": " $0 } }'
	done
}

[ -n "$WHB_LIB" ] && return 0   # sourced for its helpers only (a rig session by hand)
# the pieces this run built go at its end, as the engine's scrap does them short of the yield (dev wsscrap: reported, so the friend's
# copies go too): the next run starts on clear ground, where the host's walks are not blocked by earlier pieces
_cleanup() { local p; for p in $STANDING; do [ "$(field "$(dc host "refinfo $p")" deleted)" = false ] && dc host "wsscrap $p" >/dev/null; done; say "end: the pieces this run built scrapped (dev wsscrap)"; }
on_end _cleanup
# ---- the phases ---------------------------------------------------------------------------------------------------------------------
SPOTS="-79100,90250 -78950,89850 -79600,90300"   # open ground north of the bench, for a ghost that is never green
HOST_SPOT=${HOST_SPOT:--79750 89150}; FRIEND_SPOT=${FRIEND_SPOT:--78900 89950}
SAVE=${SAVE:-f4mp_whb}
declare -A POS
OLD_SPOTS=""
_enter_both() {
	_in_ws host || return 1
	dc host "pad unplug" >/dev/null; rwalk host $HOST_SPOT 60
	_in_ws vm || return 1
	rwalk vm $FRIEND_SPOT 60
	say "both in workshop mode, $(dist_xy "$(pos_of host)" "$(pos_of vm)") u apart"
}
_build_all() {
	local ph=$1 p kind rest path base
	for p in $PIECES; do
		kind=${p%%:*}; rest=${p#*:}; path=${rest%%:*}; base=""; [ "$rest" != "$path" ] && base=${rest#*:}
		if [ "$kind" = wire ]; then [ -n "$GEN" ] && [ -n "$LIGHT" ] && _wire "$ph" "$GEN" "$LIGHT"; continue; fi
		_build "$ph" "$kind" "$path" "$base" && { [ "$kind" = generator ] && GEN=$LAST; [ "$kind" = lamp ] && LIGHT=$LAST; }
	done
}
# _remember <ref>: the piece's x y in the host's game, remembered as where it stood
_remember() { local r; r=$(dc host "refinfo $1"); POS[$1]="$(field "$r" x) $(field "$r" y)"; }
# _strays <label> <"x y">...: what stands alive in the friend's game at spots where a piece of the host's no longer is (a scrapped
# piece's, a moved piece's old one): copies the friend's save held that no report claimed, the leftover lamps and lanterns. Alive: not
# deleted, not disabled.
_strays() {
	local label=$1 sp x y t out id ri bad=0 line w copies; shift
	copies=$(dc vm "wspower copies")   # the references the friend's game keeps as copies of standing pieces: not leftovers
	for sp in "$@"; do
		x=${sp% *}; y=${sp#* }
		for t in $(seq 27 40); do
			out=$(dc vm "refs $t 8000"); [ "$out" = none ] && continue
			while read -r line; do
				[ -n "$line" ] || continue
				id=${line%% *}
				grep -q "$id=" <<<"$copies" && continue
				ri=$(dc vm "refinfo $id")
				[ "$(field "$ri" deleted)" = false ] && [ "$(field "$ri" disabled)" = false ] || continue
				say "$label: alive in the friend's game at the spot ($x $y) of a scrapped or moved piece: $line"; bad=$((bad + 1))
			done < <(tr ';' '\n' <<<"$out" | sed 's/^ *//' | awk -v x="${x%.*}" -v y="${y%.*}" '{ n = split($0, w, " "); for (i = 1; i <= n; i++) if (w[i] == "at") { dx = w[i+1] - x; dy = w[i+2] - y; if (dx*dx + dy*dy <= 40*40) print $0 } }')
		done
	done
	w=$(dc vm wspower)
	say "$label: friend's strays=$(field "$w" strays) savedCopies=$(field "$w" savedCopies) savedRemoved=$(field "$w" savedRemoved); $bad alive at those spots"
	[ "$bad" -eq 0 ] || fail "$label: $bad leftover(s) alive in the friend's game where the host scrapped or moved a piece"
	# the copies a save of the friend's held that no report claimed (WorkshopPower::Strays: shown unpowered, standing all the same)
	[ "$(field "$w" strays)" = 0 ] || fail "$label: the friend's game holds $(field "$w" strays) copies of pieces the host no longer has (copies its save held that no session keeps)"
}
_rejoin_friend() {  # the friend joins the host's lobby again (a player's click on the Steam join link)
	local lobby; lobby=$(field "$(dc host "net status")" lobby)
	vmssh "& C:\F4MP\in-vm-run.ps1 -Name \"F4MP-Join-\$(Get-Date -Format HHmmss)\" -Exe 'C:\Program Files (x86)\Steam\steam.exe' -Arguments 'steam://joinlobby/377160/$lobby/$HOSTID'" >/dev/null
	wait_for 90 eval 'dc vm "net status" | grep -q status=joined'
}
# _changes <phase>: what a host does to its settlement (real input): another floor and lamp, the container moved, the lamp and the
# first floor scrapped. OLD_SPOTS: where pieces stood that no longer stand there
_changes() {
	local ph=$1 kind rest path base p
	if [ -n "$CONTAINER" ]; then
		if _move host "$CONTAINER"; then say "$ph: the host moved the container $CONTAINER to $(field "$(dc host "refinfo $CONTAINER")" x) $(field "$(dc host "refinfo $CONTAINER")" y) (was ${POS[$CONTAINER]})"; OLD_SPOTS="$OLD_SPOTS|${POS[$CONTAINER]}"; _remember "$CONTAINER"
		else say "$ph: the host's move of the container failed: $MOVE_WHY"; RESULTS="$RESULTS $ph/move:void"; fi
	fi
	for p in $LAMP $FLOOR; do
		[ -n "$p" ] || continue
		if _scrap host "$p"; then say "$ph: the host scrapped ${KIND[$p]} $p (R, Enter): gone from ${POS[$p]}"; GONE="$GONE $p"; STANDING=${STANDING/ $p/}; OLD_SPOTS="$OLD_SPOTS|${POS[$p]}"
		else say "$ph: the host's scrap of ${KIND[$p]} $p failed: $SCRAP_WHY"; RESULTS="$RESULTS $ph/scrap-${KIND[$p]}:void"; fi
	done
	for p in "floor:STRUCTURES,WOOD,FLOORS" "lamp:POWER,LIGHTS:0022D2CD"; do
		kind=${p%%:*}; rest=${p#*:}; path=${rest%%:*}; base=""; [ "$rest" != "$path" ] && base=${rest#*:}
		_build "$ph" "$kind" "$path" "$base"
	done
	for p in $STANDING; do [ -n "${POS[$p]}" ] || _remember "$p"; done
}
_pick_pieces() { local p; CONTAINER=""; LAMP=""; FLOOR=""; for p in $STANDING; do case "${KIND[$p]}" in container) CONTAINER=$p;; lamp) [ -z "$LAMP" ] && LAMP=$p;; floor) [ -z "$FLOOR" ] && FLOOR=$p;; esac; done; }

for p in $STANDING; do [ -n "${POS[$p]}" ] || _remember "$p"; done   # pieces adopted from an earlier run
for ph in $PHASES; do
	case "$ph" in
	build)
		_enter_both || { RESULTS="$RESULTS build:void"; continue; }
		_build_all build
		for p in $STANDING; do _remember "$p"; done
		_layout "build: after the host's builds" ;;
	live)
		# the friend present: the host moves and scraps; the friend's copies follow within 15 s
		_pick_pieces; _in_ws host || { RESULTS="$RESULTS live:void"; continue; }
		OLD_SPOTS=""; _changes live
		sleep 15; _layout "live: 15 s after the host's moves and scraps"
		mapfile -t sp < <(tr '|' '\n' <<<"$OLD_SPOTS" | grep .); _strays "live" "${sp[@]}" ;;
	rejoin)
		[ -n "$STANDING" ] || { say "rejoin: nothing stands (run build first)"; RESULTS="$RESULTS rejoin:void"; continue; }
		_pick_pieces
		say "rejoin: the friend saves (console save $SAVE), leaves workshop mode and the session (the menu's Leave: net leave)"
		dc vm "console save $SAVE" >/dev/null; sleep 8; wait_ingame vm 30
		ws_leave vm; dc vm "net leave" >/dev/null; sleep 6
		say "rejoin: the friend is $(field "$(dc vm 'net status')" status)"
		_in_ws host || { RESULTS="$RESULTS rejoin:void"; continue; }
		OLD_SPOTS=""; _changes away
		say "away: standing: $STANDING; scrapped: $GONE"
		ws_leave host
		say "rejoin: the friend joins again (steam://joinlobby)"
		_rejoin_friend || { say "rejoin: the friend did not join ($(dc vm 'net status' | grep -oE 'status=[a-z]+' | head -1))"; RESULTS="$RESULTS rejoin:void(join)"; continue; }
		sleep 20; _layout "rejoin: 20 s after the friend came back (same world)"
		mapfile -t sp < <(tr '|' '\n' <<<"$OLD_SPOTS" | grep .); _strays "rejoin" "${sp[@]}" ;;
	stale)
		# the friend loads the save it made before the host's changes while in the session (a death's reload, an older save)
		[ -n "$GONE$OLD_SPOTS" ] || { say "stale: run rejoin first (the host's changes since the friend's save)"; RESULTS="$RESULTS stale:void"; continue; }
		say "stale: the friend loads $SAVE (the world before the host's changes) while in the session"
		dc vm "console load $SAVE" >/dev/null; sleep 20; wait_ingame vm 90
		wait_for 60 eval 'dc vm "net status" | grep -q status=joined' || { say "stale: the friend is not in the session after its load"; RESULTS="$RESULTS stale:void"; continue; }
		sleep 40; _layout "stale: 40 s after the friend loaded the older save"
		mapfile -t sp < <(tr '|' '\n' <<<"$OLD_SPOTS" | grep .); _strays "stale" "${sp[@]}" ;;
	containers)
		IT=00023736   # a stimpak: aid is shared loot
		_pick_pieces; PRE=000AE12F   # the Toolbox the settlement came with, in a Sanctuary house (not a piece anyone built)
		CC=""; [ -n "$CONTAINER" ] && CC=$(ws_copy_of vm "$CONTAINER")
		if [ -z "$CC" ]; then say "containers: the host's built container has no copy in the friend's game"; RESULTS="$RESULTS containers:void"; continue; fi
		say "containers: console additem and the dev transfer (storage put) stand for the loot a player puts in (the menu's own transfer)"
		for c in "$CONTAINER $CC built" "$PRE $PRE pre-placed"; do
			set -- $c; h=$1; f=$2; what=$3
			dc host "console player.additem $IT 7" >/dev/null; sleep 1
			dc host "storage put $h $IT 7" >/dev/null; sleep 8
			hc=$(dc host "count $h $IT" | grep -oE '[0-9]+$'); fc=$(dc vm "count $f $IT" | grep -oE '[0-9]+$')
			say "containers: $what: the host put 7 stimpaks in $h: it holds ${hc:-?}; the friend's $f holds ${fc:-?}"
			[ "${hc:-0}" = "${fc:-0}" ] || fail "containers: the host's $what container holds ${hc:-?} stimpaks, the friend's holds ${fc:-?}"
			dc vm "console player.additem $IT 3" >/dev/null; sleep 1
			dc vm "storage put $f $IT 3" >/dev/null; sleep 8
			hc=$(dc host "count $h $IT" | grep -oE '[0-9]+$'); fc=$(dc vm "count $f $IT" | grep -oE '[0-9]+$')
			say "containers: $what: the friend then put 3 in $f: the host's $h holds ${hc:-?}, the friend's holds ${fc:-?}"
			[ "${hc:-0}" = "${fc:-0}" ] || fail "containers: after the friend's put the $what container holds ${hc:-?} for the host and ${fc:-?} for the friend"
		done ;;
	esac
done
ws_leave host; ws_leave vm
rkeep host workshop-host-builds; rkeep vm workshop-host-builds
say "pieces: $RESULTS"
grep -E 'WorkshopSync: not placing|refused' "$REPRO_OUT/workshop-host-builds-vm.log" | tail -6 | while read -r l; do say "friend's log: ${l:0:240}"; done
[ -n "$STANDING$GONE" ] || verdict VOID "no host piece was put down"
if [ -n "$FAILS" ]; then verdict YES "${FAILS#; }"; fi
case "${RESULTS//wire:void/}" in *void*) verdict VOID "steps could not be done (${RESULTS# }), so the run did not test everything it should: no pass";; esac
verdict NO "every host piece reached the friend's game, the friend's layout matched the host's after a rejoin and an older save's load, and both containers held the same"
