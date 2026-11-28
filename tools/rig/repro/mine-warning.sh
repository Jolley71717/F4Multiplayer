#!/bin/bash
# mine-warning.sh [mode] [actor]: the HUD's warning for a mine (the red mine icon at the middle of the screen, which the game
# shows for a live explosive that could hurt the player) comes in the friend's game only once the mine has gone off. Player
# report (on 0.20.4): "a mine the host sets off is marked for the friend only after it has gone off".
#   mode    placed (the default): one of the world's own mines (the same reference in both games); the actor walks onto it
#           while the other player (the watcher) stands inside its blast, outside its trigger.
#           laid: the actor lays a frag mine of their own (a real hold of the throw key); the watcher's game is asked whether
#           it has a mine there at all, then the watcher walks onto where it lies. A player's own mine is not set off by a
#           friend, so a raider is then put on it by the layer's console (placeatme on the mine), well after the 8 s in
#           which the layer's game looks for the explosions of a throw: is that blast set off in the watcher's game?
#   actor   host (the default) or vm: who walks onto the placed mine, or who lays one; the other one is the watcher
# The warning is read from the HUD's own display objects, never from the screen: dev `gfx HUDMenu
# root.CenterGroup_mc.ExplosiveIndicatorBase_mc 2` writes them to the plugin log, about ten times a second in both games while the
# mine is walked onto. The mine's countdown is the dev channel's `traps probe` (each frame while it counts, in each game's log).
# Each game's own clock is used for its own lines only (when its warning showed, against when its mine went off).
# Set-up, said so where it happens: both games to the scene by console coc; god mode; in the laid mode the mine given by
# console additem and equipitem.
# REPRO: YES when the watcher's HUD showed no warning before the mine went off in its game while the actor's did (placed), or
# when the watcher's game has no mine where the actor laid one (laid; the line says too whether its blast was shared); NO
# when the watcher's warning showed before its mine went off (placed), or its game has the laid mine (laid); VOID when the
# mine never went off, the actor's own HUD showed no warning either (then this warning is not what the game shows for a
# mine), or the scene could not be made.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
. "$RIG/crashlib.sh"
MODE=${1:-placed}; ACTOR=${2:-host}; WATCHER=$(other_game "$ACTOR")
CELL=${CELL:-PoseidonEnergy01}; STAND=${STAND:-230}; HUDPATH=${HUDPATH:-root.CenterGroup_mc.ExplosiveIndicatorBase_mc}
MINE_ITEM=000E56C2; VK_THROW=0x12
[ "$MODE" = placed ] || [ "$MODE" = laid ] || verdict VOID "usage: mine-warning.sh [placed|laid] [host|vm]"
[ "$ACTOR" = host ] || [ "$ACTOR" = vm ] || verdict VOID "usage: mine-warning.sh [placed|laid] [host|vm]"
need_session

# armed_mines <who>: the world's mines that game watches and has armed, one a line: "ref x y z"
armed_mines() { dc "$1" traps | sed 's/ | traps=.*//' | tr ';' '\n' | sed -nE 's/^ *([0-9A-F]{8}) mine armed at \(([-0-9.]+),([-0-9.]+),([-0-9.]+)\).*/\1 \2 \3 \4/p'; }
# mine_refs <who>: every mine reference that game has within 1500 u of its player (the world's and laid ones), one a line: "ref x y"
mine_refs() { dc "$1" "refs 51 1500 Mine" | tr ';' '\n' | awk '$1 ~ /^[0-9A-F]+$/ && NF >= 6 { print $1, $(NF - 2), $(NF - 1) }'; }
blasts_of() { field "$(dc "$1" 'net status')" explosions; }
# sample <who>: reads that game's HUD warning into its log until the stop file is there
sample() { while [ ! -e "$REPRO_OUT/stop" ]; do dc "$1" "gfx HUDMenu $HUDPATH 2" >/dev/null; done; }
# read_log <who> <mine ref or ->: from that game's log lines since the mark (its own clock, in seconds of the day):
#   SAMPLES (HUD reads), SHOWN (reads with a visible warning), FIRST and LAST (when it showed), ICON (which icon), COUNT (when the
#   mine's countdown was first seen running), OFF (when the mine was first seen gone), BLAST (when a friend's explosion was set
#   off here or our own was seen)
read_log() {
	local who=$1 mine=$2
	eval "$(rsince "$who" | awk -v mine="$mine" -v path="$HUDPATH" '
		function secs(s) { return substr(s, 2, 2) * 3600 + substr(s, 5, 2) * 60 + substr(s, 8, 6) }
		index($0, "gfx: " path " type=") { samples++; next }
		index($0, "gfx: " path ".") {
			rest = substr($0, index($0, "gfx: " path ".") + length("gfx: " path "."))
			split(rest, f, " "); n = split(f[1], seg, ".")
			if (n == 1 && rest ~ /visible=true/) { if (first == "") first = secs($1); last = secs($1); t = secs($1); if (t != seenAt) { shown++; seenAt = t } }
			if (n >= 2 && rest ~ /visible=true/ && f[1] ~ /(Mine|Grenade)Icon_mc$/) icon = seg[n]
			next
		}
		mine != "-" && index($0, "Traps: probe " mine) {
			if ($0 ~ /gone=true/) { if (off == "") off = secs($1); next }
			if (match($0, /timer=[-0-9.e+]+/)) { v = substr($0, RSTART + 6, RLENGTH - 6) + 0; if (v < 1e29 && count == "") count = secs($1) }
			next
		}
		/WeaponFire: (our explosion|explosion [0-9A-F]+ radius)/ { if (blast == "") blast = secs($1) }
		END { printf "SAMPLES=%d SHOWN=%d FIRST=%s LAST=%s ICON=%s COUNT=%s OFF=%s BLAST=%s\n", samples, shown, first == "" ? "-" : first, last == "" ? "-" : last, icon == "" ? "-" : icon, count == "" ? "-" : count, off == "" ? "-" : off, blast == "" ? "-" : blast }')"
}
rel() { [ "$1" = - ] || [ "$2" = - ] && { echo "?"; return; }; awk -v a="$1" -v b="$2" 'BEGIN { printf "%+.2f", a - b }'; }
# told <who> <when its mine went off, or ->: one line of what that game's HUD showed
told() {
	local w=$1 off=$2
	if [ "$SHOWN" -gt 0 ]; then echo "$w's HUD: $SAMPLES reads, the warning ($ICON) showed in $SHOWN of them, first $(rel "$FIRST" "$off") s and last $(rel "$LAST" "$off") s from when its mine went off"
	else echo "$w's HUD: $SAMPLES reads, the warning never showed"; fi
}

say "set-up: both games to $CELL by console coc; god mode"
enter_both "$CELL" 20
for g in host vm; do dc "$g" 'god on' >/dev/null; done
probe=$(dc "$WATCHER" "gfx HUDMenu $HUDPATH 2")
case "$probe" in error*|"") verdict VOID "the HUD's warning holder is not at $HUDPATH in $WATCHER's game ('$probe')" ;; esac
CH0=$(crash_list_host); CV0=$(crash_list_vm)
rm -f "$REPRO_OUT/stop"

if [ "$MODE" = placed ]; then
	# a mine armed in both games: the one nearest the actor, with no other mine within 400 u of it (MINE=<ref> names one)
	wait_for 20 eval '[ -n "$(armed_mines "$ACTOR")" ] && [ -n "$(armed_mines "$WATCHER")" ]'
	A=$(armed_mines "$ACTOR"); W=$(armed_mines "$WATCHER"); AP=$(pos_of "$ACTOR")
	say "armed mines: $ACTOR's game $(wc -l <<<"$A" | tr -d ' ') ($(cut -d' ' -f1 <<<"$A" | paste -sd' ' -)), $WATCHER's $(grep -c . <<<"$W")"
	PICK=$(awk -v w="$W" -v at="$AP" -v want="$MINE" 'BEGIN { n = split(w, l, "\n"); for (i = 1; i <= n; i++) { split(l[i], f, " "); both[f[1]] = 1 }; split(at, p, " ") }
		{ id[NR] = $1; x[NR] = $2; y[NR] = $3; z[NR] = $4 }
		END { best = 0
			for (i = 1; i <= NR; i++) { if (!(id[i] in both) || (want != "" && id[i] != want)) continue
				lone = 1; for (j = 1; j <= NR; j++) if (j != i && sqrt((x[i]-x[j])^2 + (y[i]-y[j])^2) < 400) lone = 0
				if (want == "" && !lone) continue
				d = sqrt((x[i]-p[1])^2 + (y[i]-p[2])^2); if (!best || d < bd) { best = i; bd = d } }
			if (best) printf "%s %s %s %s %d", id[best], x[best], y[best], z[best], bd }' <<<"$A")
	[ -n "$PICK" ] || verdict VOID "no mine of $CELL is armed in both games and stands alone ($ACTOR's: $(paste -sd';' - <<<"$A"))"
	read -r M MX MY MZ MD <<<"$PICK"
	say "the mine: $M at $MX $MY $MZ, $MD u from $ACTOR; $WATCHER walks to $STAND u of it (inside its blast of 500, outside its trigger of 100), then $ACTOR walks onto it"
	rwalk "$WATCHER" "$MX" "$MY" "$STAND" 0.6
	WD=$(dist_xy "$(pos_of "$WATCHER")" "$MX $MY")
	is_int "$WD" && [ "$WD" -ge 165 ] && [ "$WD" -le 450 ] || verdict VOID "$WATCHER's walk ended $WD u from the mine, not inside its blast and outside its trigger ($WALKED)"
	[ "$(armed_mines "$WATCHER" | grep -c "^$M ")" = 1 ] && [ "$(armed_mines "$ACTOR" | grep -c "^$M ")" = 1 ] || verdict VOID "the mine went off or was lost before the test ($WATCHER stands $WD u from it)"
	dc "$WATCHER" "turnto $M 300" >/dev/null
	for g in host vm; do dc "$g" 'traps probe on' >/dev/null; done
	rmark host; rmark vm
	sample host & sample vm &
	sleep 1.5
	rwalk "$ACTOR" "$MX" "$MY" 25 0.6
	wait_for 12 eval '[ "$(armed_mines "$ACTOR" | grep -c "^$M ")" = 0 ]'; GONE_A=$?
	wait_for 8 eval '[ "$(armed_mines "$WATCHER" | grep -c "^$M ")" = 0 ]'; GONE_W=$?
	sleep 3; touch "$REPRO_OUT/stop"; wait
	for g in host vm; do dc "$g" 'traps probe off' >/dev/null; done
	rsince host > "$REPRO_OUT/run-host.log"; rsince vm > "$REPRO_OUT/run-vm.log"
	say "$ACTOR walked onto it ($WALKED); the mine gone: $ACTOR's game $([ $GONE_A = 0 ] && echo yes || echo no), $WATCHER's $([ $GONE_W = 0 ] && echo yes || echo no); $WATCHER stood $WD u from it"
	read_log "$ACTOR" "$M"; A_SHOWN=$SHOWN; A_FIRST=$FIRST; A_OFF=$OFF; A_SAMPLES=$SAMPLES
	say "$ACTOR's game: countdown seen $(rel "$COUNT" "$OFF") s from when its mine went off; $(told "$ACTOR" "$OFF")"
	read_log "$WATCHER" "$M"; W_SHOWN=$SHOWN; W_FIRST=$FIRST; W_OFF=$OFF; W_SAMPLES=$SAMPLES
	say "$WATCHER's game: countdown seen $(rel "$COUNT" "$OFF") s from when its mine went off; $(told "$WATCHER" "$OFF")"
	grep -hE "Traps: [0-9A-F]{8} " "$REPRO_OUT/run-host.log" | tail -4 | cut -c1-220 | sed 's/^/host log: /' | tee -a "$REPRO_OUT/run.log"
	grep -hE "Traps: [0-9A-F]{8} " "$REPRO_OUT/run-vm.log" | tail -4 | cut -c1-220 | sed 's/^/vm log: /' | tee -a "$REPRO_OUT/run.log"
	NH=$(crash_new "$CH0" "$(crash_list_host)"); NV=$(crash_new "$CV0" "$(crash_list_vm)")
	[ -n "$NH$NV" ] && say "NEW CRASH LOGS: host '$NH' vm '$NV'"
	[ $GONE_A = 0 ] && [ "$A_OFF" != - ] || verdict VOID "the mine never went off in $ACTOR's game ($WALKED)"
	[ "$W_OFF" != - ] || verdict VOID "the mine went off in $ACTOR's game and never in $WATCHER's: another fault than the one looked for"
	[ "$A_SAMPLES" -ge 5 ] && [ "$W_SAMPLES" -ge 5 ] || verdict VOID "too few HUD reads to say ($ACTOR $A_SAMPLES, $WATCHER $W_SAMPLES)"
	before() { [ "$1" != - ] && awk -v a="$1" -v b="$2" 'BEGIN { exit !(a < b) }'; }
	if ! before "$A_FIRST" "$A_OFF"; then
		verdict VOID "$ACTOR's own HUD showed no warning before its own mine went off either (shown in $A_SHOWN reads): the game does not mark a stepped-on mine this way, or the reads missed it"
	fi
	if before "$W_FIRST" "$W_OFF"; then
		verdict NO "$WATCHER's HUD warned of the mine $(rel "$W_FIRST" "$W_OFF") s from its blast, as $ACTOR's did ($(rel "$A_FIRST" "$A_OFF") s)"
	fi
	verdict YES "$ACTOR's HUD warned of the mine $(rel "$A_FIRST" "$A_OFF") s from its blast; $WATCHER's, $WD u from it, $([ "$W_SHOWN" -gt 0 ] && echo "only $(rel "$W_FIRST" "$W_OFF") s from (after) its blast" || echo "never did")"
fi

# ---- laid: a mine a player lays ----------------------------------------------------------------------------------------------------
say "set-up: a frag mine for $ACTOR by console additem and equipitem"
dc "$ACTOR" "console player.additem $MINE_ITEM 2" >/dev/null; dc "$ACTOR" "console player.equipitem $MINE_ITEM" >/dev/null; sleep 3
rwalk "$WATCHER" "$(pos_of "$ACTOR" | cut -d' ' -f1)" "$(pos_of "$ACTOR" | cut -d' ' -f2)" 350 0.6
A0=$(mine_refs "$ACTOR"); W0=$(mine_refs "$WATCHER"); XA0=$(blasts_of "$ACTOR"); XW0=$(blasts_of "$WATCHER")
rmark host; rmark vm
rkey "$ACTOR" $VK_THROW 600
sleep 5
NEWA=$(comm -13 <(sort <<<"$A0") <(mine_refs "$ACTOR" | sort) | head -1); NEWW=$(comm -13 <(sort <<<"$W0") <(mine_refs "$WATCHER" | sort) | head -1)
say "$ACTOR laid a mine with the throw key: its game has a new mine '${NEWA:-none}'; $WATCHER's game, $(dist_xy "$(pos_of "$WATCHER")" "$(pos_of "$ACTOR")") u away: '${NEWW:-none}'"
[ -n "$NEWA" ] || verdict VOID "$ACTOR's game has no new mine after the throw key ($(dc "$ACTOR" 'equipped 00000014' | cut -c1-100))"
read -r LM LX LY <<<"$NEWA"
# the layer steps back (a real walk), and the watcher walks onto where the mine lies in the layer's game
AP=$(pos_of "$ACTOR"); WP=$(pos_of "$WATCHER")
BACK=$(awk -v m="$LX $LY" -v w="$WP" 'BEGIN { split(m, a, " "); split(w, b, " "); d = sqrt((a[1]-b[1])^2 + (a[2]-b[2])^2); if (d < 1) d = 1; printf "%d %d", a[1] + (a[1]-b[1]) / d * 450, a[2] + (a[2]-b[2]) / d * 450 }')
rwalk "$ACTOR" ${BACK% *} ${BACK#* } 80 0.6
say "$ACTOR stepped back to $(dist_xy "$(pos_of "$ACTOR")" "$LX $LY") u of its mine ($WALKED); $WATCHER walks onto where it lies"
sample "$WATCHER" &
sleep 1
rwalk "$WATCHER" "$LX" "$LY" 25 0.6
wait_for 8 eval '[ "$(mine_refs "$ACTOR" | grep -c "^$LM ")" = 0 ]'; GONE_A=$?
sleep 3; touch "$REPRO_OUT/stop"; wait
read_log "$WATCHER" -
say "$WATCHER walked to it ($WALKED): the mine went off in $ACTOR's game: $([ $GONE_A = 0 ] && echo yes || echo "no (still there)"); explosions sent/set off: $ACTOR $XA0 -> $(blasts_of "$ACTOR"), $WATCHER $XW0 -> $(blasts_of "$WATCHER"); $(told "$WATCHER" "$BLAST")"
SHARED="not tried"
if [ $GONE_A != 0 ]; then
	# a raider on the mine, by the layer's console: the mine has lain for well over the throw's 8 s by now
	rmark host late; rmark vm late
	XA1=$(blasts_of "$ACTOR"); XW1=$(blasts_of "$WATCHER"); LAIN=$SECONDS
	dc "$ACTOR" "console $LM.placeatme ${RAIDER:-001AE38D} 1" >/dev/null
	wait_for 12 eval '[ "$(mine_refs "$ACTOR" | grep -c "^$LM ")" = 0 ]'; GONE_L=$?
	sleep 4
	SENT=$(rsince "$ACTOR" late | grep -c "WeaponFire: our explosion"); SETOFF=$(rsince "$WATCHER" late | grep -cE "WeaponFire: explosion [0-9A-F]+ radius")
	say "a raider put on the mine by $ACTOR's console: the mine went off there: $([ $GONE_L = 0 ] && echo yes || echo no); $ACTOR's game looked at $SENT explosion(s) of its own (explosions $XA1 -> $(blasts_of "$ACTOR")), $WATCHER's game set off $SETOFF ($XW1 -> $(blasts_of "$WATCHER"))"
	rsince "$ACTOR" late | grep "WeaponFire: our explosion" | tail -2 | cut -c1-220 | tee -a "$REPRO_OUT/run.log"
	if [ $GONE_L != 0 ]; then SHARED="the mine never went off for the raider"
	elif [ "$SETOFF" -gt 0 ]; then SHARED="its blast was set off in $WATCHER's game"
	else SHARED="its blast was NOT set off in $WATCHER's game ($ACTOR's game looked at $SENT explosion(s) of its own)"; fi
fi
rsince host > "$REPRO_OUT/run-host.log"; rsince vm > "$REPRO_OUT/run-vm.log"
grep -hE "WeaponFire: (our explosion|explosion [0-9A-F]+ radius)" "$REPRO_OUT/run-host.log" "$REPRO_OUT/run-vm.log" | tail -4 | cut -c1-220 | tee -a "$REPRO_OUT/run.log"
NH=$(crash_new "$CH0" "$(crash_list_host)"); NV=$(crash_new "$CV0" "$(crash_list_vm)")
[ -n "$NH$NV" ] && say "NEW CRASH LOGS: host '$NH' vm '$NV'"
[ -n "$NEWW" ] && verdict NO "$WATCHER's game has the mine $ACTOR laid ($NEWW); $SHARED"
verdict YES "$ACTOR laid a mine ($LM) and $WATCHER's game has none there: nothing to see or be warned of before it goes off; $SHARED"
