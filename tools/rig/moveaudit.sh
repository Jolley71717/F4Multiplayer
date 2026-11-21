#!/bin/bash
# moveaudit.sh: how a friend's character looks when it moves, measured on the two-game rig.
#   moveaudit.sh run [groups...]     one player does each kind of movement with real keys while the other game's stand-in for it
#                                    is read; then the other way round. Needs the rig (rig.lock held, both games joined, both
#                                    dev channels on). Prints PASS / FAIL / VOID per kind and direction; exit 1 on any FAIL.
#   moveaudit.sh analyse <folder>    the numbers again from a run's files (after a threshold change, or a closer look)
#   moveaudit.sh selftest            the measuring (moveaudit.awk) on made-up traces; no game
# Groups (all of them when none is named): gaits state pistol rifle melee first drop chair pa; and `one`, the segments MA_SEG names.
# MA_MOVERS="host vm" (default) says who moves; MA_OUT names the run's folder (default $CHECKS/moveaudit-<stamp>); MA_NOANALYSE=1
# leaves the measuring for `analyse` later; MA_SPOT="ax ay az heading mx my" moves the whole scene (a slope, a flight of stairs).
# chair: a chair placed by the console, sat on with the real E key and left with a real step. pa: a power armor frame placed by the
# console, entered with E; the gaits, a jump, a turn and a higher fall in the suit.
#
# What a run does. The watcher stands at the top of a flat street in Sanctuary looking along it; the mover is put 250 u in front
# of it before each segment (console setpos: a placement to reach the scene, never the movement measured) and then moves down
# the street with real key presses (dev `key`: the keys a player holds), real mouse moves (`mouse`) or the virtual pad's stick.
# Sideways and backward movement is done facing across or up the street, so every path runs away from the watcher and stays
# in its view (a character out of view is not a fair test of its animation). Each group of segments is one dev `trace` on both
# games (every frame's position of the mover in its own game, and of its stand-in in the watcher's), lined up by the two
# machines' clocks (clocklib.sh). While the mover moves, the watcher's game is asked about 30 times a second what the stand-in's
# animation graph is doing (dev `anim`: gait, sneak, leg swing, whether its legs were told to move) and which animation
# events it raised (`animlog`: footfalls, jump and sneak events); the mover's game is asked the same of its own player.
# A segment the mover did not do as meant (a walk that was a run, a sprint that never started, a key pressed while the game
# was not in front) is VOID: the rig's or the game's own input, never a verdict on the stand-in.
#
# The limits, and why (each can be set: MA_AWK="-v T_LAG=350 ..."):
#   lag <= 300 ms             a friend is drawn in the past on purpose: 70 to 250 ms behind what arrived (RemotePlayers), plus
#                             the trip over Steam. The rig measured 100 to 125 ms on main; 300 ms is where it shows in play.
#   path error mean <= 12 u, p95 <= 35 u (the delay taken out)    main measured about 2 u; a character is about 45 u wide,
#                             so a quarter of a body on average, or most of one for a twentieth of the time, is visible.
#   beside the line <= 10 u   sideways error is not hidden by the delay the way error along the path is.
#   distance 0.93 to 1.08 of the mover's    less: corners cut or a late start never made up; more: jitter or overshoot.
#   snaps 0                   a step in one frame over three times what the mover's top speed covers in it (25 u at least).
#   backward steps <= 6 u     a stand-in never walks back along the mover's path; under 6 u is the smoothing's breath.
#   start and stop <= 400 ms after the mover   the lag plus a 30 Hz state plus the stand-in's own start and stop speeds.
#   overshoot <= 25 u, coming back <= 10 u     past where the mover stopped, then pulled back: the slide a player notices.
#   rest error <= 10 u, heading <= 15 degrees while turning, <= 10 at rest
#   legs told to move >= 85% of the time the mover moves; leg swing >= 0.05 (thigh rotation) in every second of it: under
#                             that the character glides; legs still told to move >= 15% of the time the mover stands: it
#                             runs on the spot.
#   footfalls 0.7 to 1.4 times the mover's own   the same gait at the same speed lands as many feet: fewer is a walk
#                             animation at run speed (long sliding strides), more is a run animation at walk speed.
#   gait and sneak as the mover's own graph says (iSyncLocomotionSpeed, iIsInSneak), read mid-movement in both games.
#   jumps and falls: 0.6 to 1.4 of the mover's height, <= 25 u off in height on average.
#   sitting: seated, and up again, <= 2500 ms after the mover's own game says so: sitting down is a 2 s animation the
#                             stand-in may start once the mover is seated; later than that it stands in the chair, or sits on air.
# Rig rules (AGENTS.md): real movement only; the console placements are named in the run's log where they happen.
RIG=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
[ "$1" = selftest ] && : "${VM_SSH:=selftest-no-vm}"
. "$RIG/env.sh"; . "$RIG/clocklib.sh"; . "$RIG/guard.sh"
AWK=$RIG/moveaudit.awk
# the street: the watcher's spot and heading, and the mover's start 250 u down it
AX=-80783; AY=89350; AZ=7800; AH=60
MX=-80566; MY=89475
# MA_SPOT: another place for the same scene (the mover's start should be 250 u from the watcher along the heading; z a little
# above the ground, the players drop onto it)
[ -n "$MA_SPOT" ] && read -r AX AY AZ AH MX MY <<<"$MA_SPOT"
HZ=${MA_HZ:-100}                      # trace samples a second (every frame, at the rig's frame rates)
VK_W=0x57; VK_A=0x41; VK_S=0x53; VK_D=0x44; VK_SHIFT=0x10; VK_CTRL=0x11; VK_SPACE=0x20; VK_CAPS=0x14; VK_R=0x52; VK_V=0x56; VK_E=0x45
PVARS="iIsInSneak iSyncSneakWalkRun iSyncLocomotionRangeID IsSprinting bIsSneaking iState"   # graph variables asked for besides `anim`'s own

ms() { local e=${EPOCHREALTIME/[.,]/}; MS=${e:0:13}; }
say() { printf '%(%T)T %s\n' -1 "$*" | tee -a "$OUT/run.log"; }
# ask <host|vm> <command>: one dev-channel command from this shell itself (no process started: about a frame a call), the reply
# in $A. The game serves one client at a time, so nothing else may call that game meanwhile: a refused call is tried again.
ask() {
	local p t i r; A=""
	if [ "$1" = host ]; then p=7790; t=$TOK_H; else p=7792; t=$TOK_V; fi
	for i in 1 2 3 4; do
		if exec 9<>"/dev/tcp/127.0.0.1/$p"; then
			echo "auth $t" >&9; r=""; read -t 5 -r r <&9
			if [ "${r%$'\r'}" = ok ]; then echo "$2" >&9; read -t 20 -r A <&9; exec 9>&-; A=${A%$'\r'}; return 0; fi
			exec 9>&-
		fi 2>/dev/null
		sleep 0.25
	done
	return 1
}
fieldof() { local s=" $1" n=$2; [[ $s == *[\ \(\;\|,:]"$n="* ]] || { F=""; return 1; }; F=${s#*[ (;|,:]"$n="}; F=${F%%[] ;|,)]*}; }
mark() { ms; printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$MS" "$M" "$GROUP" "$SEG" "$1" "$2" >> "$OUT/marks.tsv"; }
# put <who> <x> <y> <z> <heading>: console placement
put() { ask "$1" "console player.setpos x $2"; ask "$1" "console player.setpos y $3"; ask "$1" "console player.setpos z $4"; ask "$1" "console player.setangle z $5"; ask "$1" "console player.setangle x 0"; }
key() { ask "$M" "key $1 $2"; mark key "$1 $2 $A"; }
mouse() { ask "$M" "mouse $1 $2"; mark key "mouse $1 $2 $A"; }
probe() { ask "$M" "anim 00000014 $PVARS" && mark probe "$A"; }
# hold <ms>: wait, reading the mover's own animation events as they come (the log holds 200 at most between two reads)
hold() {
	local end; ms; end=$((MS + $1))
	while :; do
		ms; [ "$MS" -ge "$end" ] && break
		if [ $((end - MS)) -gt 320 ]; then ask "$M" "animlog on 0" && [ -n "$A" ] && [ "$A" != none ] && mark mev "$A"; sleep 0.22; else sleep 0.04; fi
	done
}
# seg <name> <kind> [heading] [awk options]: the mover put at the street's start and settled, its event log emptied, the window opened
seg() {
	SEG=$1; KIND=$2
	put "$M" $MX $MY $AZ "${3:-$AH}"; ask "$M" focus; sleep 2.2
	ask "$M" "animlog on 0"
	mark begin "$KIND${4:+ $4}"
}
# end [tail ms]: the segment's tail (the stop, and the stand-in coming to rest), the mover's last events, the window closed
end() { hold "${1:-1800}"; ask "$M" "animlog on 0" && [ -n "$A" ] && [ "$A" != none ] && mark mev "$A"; mark end ""; echo -n "."; }

# ---- the segments --------------------------------------------------------------------------------------------------------------
# <prefix> names the state the mover is in (a drawn weapon, first person); <opts> are awk options for every segment of it
s_run()     { seg "${1}run" run "" "$2"; key $VK_W 4000; hold 2000; probe; hold 2000; end; }
s_walk()    { seg "${1}walk" walk "" "$2"; key $VK_CAPS 80; hold 450; key $VK_W 5000; hold 2500; probe; hold 2500; key $VK_CAPS 80; end; }
s_sprint()  { seg "${1}sprint" sprint "" "$2"; key $VK_W 4600; hold 700; key $VK_SHIFT 3200; hold 1600; probe; hold 2300; end; }
s_stopstart() { seg "${1}stopstart" stopstart "" "$2"; local i; for i in 1 2 3; do key $VK_W 1500; hold 1500; hold 1300; done; end 1200; }
s_strafel() { seg "${1}strafeleft" strafe $(((AH + 90) % 360)) "$2"; key $VK_A 3000; hold 1500; probe; hold 1500; end; }
s_strafer() { seg "${1}straferight" strafe $(((AH + 270) % 360)) "$2"; key $VK_D 3000; hold 1500; probe; hold 1500; end; }
s_back()    { seg "${1}backpedal" back $(((AH + 180) % 360)) "$2"; key $VK_S 3000; hold 1500; probe; hold 1500; end; }
s_turn()    { seg "${1}turn" turn "" "$2"; local i; hold 400; for i in 1 2 3 4; do mouse 400 0; hold 650; done; mouse -800 0; hold 900; mouse -800 0; end; }
s_runturn() { seg "${1}runturn" runturn "" "$2"; key $VK_W 4000; hold 1300; mouse 200 0; hold 900; mouse -200 0; hold 1800; end; }
s_sneakstill() { seg "${1}sneakstill" sneakstill "" "$2"; hold 500; key $VK_CTRL 100; hold 1500; probe; hold 1500; key $VK_CTRL 100; end 2200; }
s_sneakrun() { seg "${1}sneakrun" sneakrun "" "$2"; key $VK_CTRL 100; hold 900; key $VK_W 4000; hold 2000; probe; hold 2600; key $VK_CTRL 100; end 2200; }
s_sneakwalk() { seg "${1}sneakwalk" sneakwalk "" "$2"; key $VK_CAPS 80; hold 300; key $VK_CTRL 100; hold 900; key $VK_W 4000; hold 2000; probe; hold 2600; key $VK_CTRL 100; hold 300; key $VK_CAPS 80; end 2200; }
s_jumpstand() { seg "${1}jumpstand" jumpstand "" "$2"; hold 500; key $VK_SPACE 80; hold 2200; key $VK_SPACE 80; end 2200; }
s_jumprun() { seg "${1}jumprun" jumprun "" "$2"; key $VK_W 3600; hold 1500; key $VK_SPACE 80; hold 2100; end; }
# the pad's stick part of the way: the in-between speeds a controller player moves at. A plugged pad takes the keyboard and
# mouse away and its first input is lost, so it is plugged before the window opens and unplugged after it.
s_pad()     { # s_pad <name> <stick 0-1>
	SEG=$1; KIND=padwalk; put "$M" $MX $MY $AZ $AH; ask "$M" focus; ask "$M" "pad stick l 0 0 300"; sleep 2.2; ask "$M" "animlog on 0"; mark begin "padwalk"
	ask "$M" "pad stick l 0 $2 4000"; mark key "pad stick l 0 $2 4000 $A"; hold 2000; probe; hold 2000; end; ask "$M" "pad unplug"; sleep 0.5
}
# aiming down the sights while walking: the aim button held (a synthesized button event for the game's own SecondaryAttack control)
s_ads()     { seg "${1}adswalk" adswalk "" "$2"; ask "$M" "input SecondaryAttack hold=3800 device=mouse"; mark key "aim $A"; hold 700; key $VK_W 2800; hold 1400; probe; hold 1400; end 2000; }
# a fall: the mover is put 450 u up in the air (console placement, to reach a height: the fall and the landing are the game's
# own) and the window opens as it starts to drop; the stand-in's jump up to that height is the placement's, so snaps in the
# window's first 700 ms are not counted
s_drop()    {
	SEG=${1}drop; KIND=fall; put "$M" $MX $MY $AZ $AH; ask "$M" focus; sleep 2.2; ask "$M" "animlog on 0"
	ask "$M" "console player.setpos z $((AZ + ${DROP_H:-450}))"; mark begin "fall snapSkip=700${2:+ $2}"; hold 1500; probe; hold 1500; end 1000
}
# the same while running: W held, then the placement into the air mid-run
s_droprun() {
	SEG=${1}droprun; KIND=fall; put "$M" $MX $MY $AZ $AH; ask "$M" focus; sleep 2.2; ask "$M" "animlog on 0"
	ask "$M" "key $VK_W 3500"; sleep 1.2; ask "$M" "console player.setpos z $((AZ + ${DROP_H:-450}))"; mark begin "fall snapSkip=700${2:+ $2}"; hold 2300; end 1800
}

# ---- the mover's state (camera, weapon), set with real keys and read back ---------------------------------------------------------
cam() { ask "$M" controls; fieldof "$A" camera; CAM=$F; }
# pov <first|third>: the mover's camera (V taps); camera state 0 is first person, 8 third
pov() { local want=8 i; [ "$1" = first ] && want=0; for i in 1 2 3; do cam; [ "$CAM" = "$want" ] && return 0; ask "$M" focus; ask "$M" "key $VK_V 80"; sleep 1.5; done; cam; [ "$CAM" = "$want" ]; }
drawn() { ask "$M" equipped; fieldof "$A" drawn; [ "$F" = true ]; }
# arm <weapon form> [ammo form]: the weapon given and equipped (console: the set-up), then drawn with the real R key
arm() {
	ask "$M" "console player.additem $1 1"; [ -n "$2" ] && ask "$M" "console player.additem $2 200"; ask "$M" "console player.equipitem $1"; sleep 2.5
	ask "$M" equipped; say "  $M equipped: ${A:0:110}"
	[[ $A == *"${1#000}"* || $A == *"$1"* ]] || { say "  the weapon $1 is not in the mover's hand"; return 1; }
	local i; for i in 1 2; do drawn && break; ask "$M" focus; ask "$M" "key $VK_R 80"; sleep 2; done
	drawn && return 0
	ask "$M" "draw on"; sleep 2; say "  R did not draw it: drawn through the dev channel instead (not a key press): $A"; drawn
}
unarm() { local i; for i in 1 2; do drawn || return 0; ask "$M" focus; ask "$M" "key $VK_R 900"; sleep 2.5; done; drawn || return 0; ask "$M" "draw off"; sleep 2; say "  holding R did not holster: holstered through the dev channel"; ! drawn; }

# ---- the groups: each is one trace -------------------------------------------------------------------------------------------------
g_gaits()  { pov third || return 1; s_run; s_walk; s_sprint; s_stopstart; s_strafel; s_strafer; s_back; s_turn; }
g_state()  { pov third || return 1; s_sneakstill; s_sneakrun; s_sneakwalk; s_jumpstand; s_jumprun; s_runturn; s_pad padslow 0.4; s_pad padmid 0.7; }
g_weapon() { # g_weapon <name> <form> <ammo> [melee]
	pov third || return 1; arm "$2" "$3" || { unarm; return 1; }
	s_run "$1-"; s_walk "$1-"; s_sprint "$1-"; s_stopstart "$1-"; s_strafel "$1-"; s_back "$1-"; s_sneakrun "$1-"; s_jumprun "$1-"; [ -z "$4" ] && s_ads "$1-"
	unarm
}
g_pistol() { g_weapon pistol 00004822 0001F276; }
g_rifle()  { g_weapon rifle 0004F46A 0001F66B; }
g_melee()  { g_weapon melee 0008E736 "" melee; }
g_first()  { pov first || return 1; local o="noGraph=1"; s_run fp- $o; s_walk fp- $o; s_sprint fp- $o; s_stopstart fp- $o; s_strafel fp- $o; s_back fp- $o; s_jumprun fp- $o; s_sneakrun fp- $o; s_turn fp- $o; pov third; }
g_drop()   { pov third || return 1; s_drop; s_droprun; }
GROUPS_ALL="gaits state pistol rifle melee first drop chair pa"
# one: the segments MA_SEG names, for a closer look at one kind or a repro (MA_SEG="s_sprint; s_sprint" moveaudit.sh run one)
g_one()    { [ -n "$MA_SEG" ] || { say "  MA_SEG names the segments of group one (s_run, s_sprint ...)"; return 1; }; pov "${MA_POV:-third}" || return 1; eval "$MA_SEG"; }


# ---- furniture and power armor --------------------------------------------------------------------------------------------------
# newref <base>: the reference of that base listed near the mover now and not in $1's list before (a thing just placed)
newref() { local r; NEW=""; ask "$M" "refs 42 400"; for r in $(grep -oE "[0-9A-F]{8} base=$1" <<<"$A" | cut -c1-8); do [[ $BEFORE == *"$r base=$1"* ]] && continue; NEW=$r; return 0; done; return 1; }
sitstate() { ask "$M" "paframes 100"; SIT=$(grep -oE 'our player: sitState=-?[0-9]+' <<<"$A"); SIT=${SIT##*=}; }
# holdsit <ms>: wait, reading the mover's own sit state about four times a second
holdsit() { local end; ms; end=$((MS + $1)); while :; do ms; [ "$MS" -ge "$end" ] && break; sitstate; [ -n "$SIT" ] && mark msit "$SIT"; sleep 0.2; done; }
# a chair: one is placed in front of the mover (console: the set-up), the mover looks at it and sits with the real E key, sits
# 5 s, then gets up with a real step forward. Read: how long after the mover the stand-in sits and gets up, and where it is.
s_chair() {
	SEG=${1}chair; KIND=sit; put "$M" $MX $MY $AZ $AH; ask "$M" focus; sleep 1.5
	ask "$M" "refs 42 400"; BEFORE=$A; ask "$M" "console player.placeatme 000DF45C"; sleep 2
	newref 000DF45C || { say "  no chair listed after the placing: the segment is not run"; return 1; }
	CHAIR=$NEW; key $VK_S 350; sleep 1.5; ask "$M" "turnto $CHAIR 300 aim"; sleep 1.5; ask "$M" "animlog on 0"
	mark begin "sit"; key $VK_E 120; holdsit 6000; probe
	sitstate; [ "$SIT" = 3 ] || { ask "$M" "activate $CHAIR"; mark key "activate $CHAIR $A (the dev channel: the E did not seat the mover)"; holdsit 5000; }
	key $VK_W 700; holdsit 4000; end 500
	ask "$M" "console $CHAIR.disable"; ask "$M" "console $CHAIR.markfordelete"
}
g_chair()  { pov third || return 1; s_chair; s_chair second-; }
# power armor: a frame is placed at the mover's feet and a fusion core given (console: the set-up), the mover gets in with the
# real E key (the dev channel's activate when two E's miss, said in the log), and the same segments are run in the suit; the
# fall is from higher, for the suit's heavy landing. Out with E held, or by the console when that fails (said too).
pa_in() { ask "$M" pa; fieldof "$A" extraPA; [ "$F" = true ]; }
pa_enter() {
	local i
	put "$M" $MX $MY $AZ $AH; ask "$M" focus; sleep 1.5; ask "$M" "console player.additem 00075FE4 2"
	ask "$M" "refs 42 400"; BEFORE=$A; ask "$M" "console player.placeatme 0004A9C1"; sleep 2
	newref 0004A9C1 || { say "  no frame listed after the placing"; return 1; }
	FRAME=$NEW; ask "$M" "key $VK_S 300"; sleep 1.2; ask "$M" "key $VK_W 120"; sleep 1
	for i in 1 2; do
		if [ $i = 1 ]; then ask "$M" "turnto $FRAME 300"; sleep 1; ask "$M" "mouse 0 -150"; sleep 0.8; else ask "$M" "turnto $FRAME 300 aim"; sleep 1.5; fi
		ask "$M" "key $VK_E 120"; sleep 10; pa_in && { say "  $M: in frame $FRAME at E $i"; return 0; }
	done
	ask "$M" "activate $FRAME"; sleep 10; say "  the E did not get the mover into the frame: the dev channel's activate instead ($A)"; pa_in
}
pa_exit() {
	local i; for i in 1 2; do pa_in || break; ask "$M" focus; ask "$M" "mouse 0 -900"; sleep 0.4; ask "$M" "key $VK_E 1500"; sleep 7; done
	pa_in && { ask "$M" 'console player.cf "Actor.SwitchToPowerArmor" none'; sleep 6; say "  E held did not get the mover out of the suit: taken out by the console"; }
	ask "$M" "refs 42 1500"; for i in $(tr ';' '\n' <<<"$A" | grep 0004A9C1 | grep -oE '^ *FF[0-9A-F]{6}'); do ask "$M" "console $i.disable"; ask "$M" "console $i.markfordelete"; done
	! pa_in
}
g_pa()     {
	pov third || return 1; pa_enter || { pa_exit; return 1; }
	s_run pa-; s_walk pa-; s_sprint pa-; s_stopstart pa-; s_strafel pa-; s_back pa-; s_jumprun pa-; s_turn pa-; DROP_H=1100 s_drop pa-
	pa_exit
}
# poller <watcher> <puppet> <file> <stop file>: the watcher's game asked about the stand-in, over and over, until the stop file is there
poller() {
	local i=0
	while [ ! -e "$4" ]; do
		ask "$1" "anim $2 $PVARS" && { ms; printf '%s\tA\t%s\n' "$MS" "$A"; }
		i=$((i + 1)); [ $((i % 5)) = 0 ] && ask "$1" "animlog on $2" && { ms; printf '%s\tE\t%s\n' "$MS" "$A"; }
	done >> "$3"
}
# group <name>: both placed, one trace on both games around the group's segments, the files fetched, the clocks lined up
group() {
	GROUP=$1; SEG=-; local base=$OUT/$M-$GROUP stop=$OUT/.stop a b hs vs after l ok=ok pid
	say "== $M moves, $W watches: $GROUP"
	put "$W" $AX $AY $AZ $AH; put "$M" $MX $MY $AZ $AH; ask "$W" focus; ask "$M" focus; sleep 1.5
	ask "$W" "net status"; fieldof "$A" puppet; PUP=$F
	[ -n "$PUP" ] || { say "  no stand-in in $W's game (net status: ${A:0:80}): the group is not run"; printf '%s\t%s\trig\t0\tno stand-in\n' "$M" "$GROUP" >> "$OUT/groups.tsv"; return 1; }
	rm -f "$stop"; : > "$base-anim.tsv"
	a=$(bash "$RIG/dcfast.sh" both "trace start $HZ" 2>&1 | tr '\n' ' '); say "  trace start: $a"
	poller "$W" "$PUP" "$base-anim.tsv" "$stop" & pid=$!
	"g_$GROUP" || { ok=state; say "  the mover could not be put in the state this group needs (camera or weapon): its segments so far stand, the rest are not run"; }
	echo; touch "$stop"; wait "$pid" 2>/dev/null; rm -f "$stop"
	ask host "trace stop"; a=$A; hs=$(wall_step "$a"); ask vm "trace stop"; b=$A; vs=$(wall_step "$b")
	ask "$W" "animlog off"; ask "$M" "animlog off"
	say "  trace stop: host ${a%% ->*} (wallStep ${hs:-none}), vm ${b%% ->*} (wallStep ${vs:-none}); stand-in readings $(wc -l < "$base-anim.tsv")"
	cp -f "$HOST_F4SE/F4Multiplayer_trace.csv" "$OUT/.host.csv" 2>/dev/null
	scp -q -o BatchMode=yes -i "$VM_KEY" "$VM_SSH:$VM_F4SE/F4Multiplayer_trace.csv" "$OUT/.vm.csv" 2>/dev/null
	# the nearby NPCs' rows are dropped: the mover's and the stand-in's are what is measured
	grep -vE '^[0-9.]+,n' "$OUT/.$M.csv" > "$base-own.csv" 2>/dev/null; grep -vE '^[0-9.]+,n' "$OUT/.$W.csv" > "$base-view.csv" 2>/dev/null
	rm -f "$OUT/.host.csv" "$OUT/.vm.csv"
	after=$(bash "$RIG/trace.sh" offset 2>/dev/null | tail -1)
	if l=$(clock_lineup "$OFFSET" "$after" "$hs" "$vs"); then
		set -- $l; printf '%s\t%s\tok\t%s\t%s\n' "$M" "$GROUP" "$2" "${l#ok $2 }" >> "$OUT/groups.tsv"; say "  clocks: the VM's is $2 ms from the host's"
	else
		printf '%s\t%s\trig\t0\t%s\n' "$M" "$GROUP" "${l#rig }" >> "$OUT/groups.tsv"; say "  RIG: $l"
	fi
	[[ $after =~ ^-?[0-9]+$ ]] && OFFSET=$after
	[ "$ok" = ok ]
}
# calib: the mover's keys reach its game and W runs (always-run on): W held 1.2 s, the ground covered read from `pos`
calib() {
	local x0 y0 d try
	for try in 1 2; do
		put "$M" $MX $MY $AZ $AH; ask "$M" focus; sleep 1.5
		ask "$M" pos; fieldof "$A" x; x0=$F; fieldof "$A" y; y0=$F
		ask "$M" "key $VK_W 1200"; local k=$A; sleep 1.9; ask "$M" pos; fieldof "$A" x; local x1=$F; fieldof "$A" y
		d=$(awk -v a="$x0" -v b="$y0" -v c="$x1" -v e="$F" 'BEGIN { printf "%d", sqrt((c - a) ^ 2 + (e - b) ^ 2) }')
		say "  $M: W for 1.2 s covered $d u ($k)"
		[ "$d" -ge 250 ] && return 0
		[ "$d" -lt 30 ] && { say "  RIG: W does not move $M's player (not in front, a menu open, or the game paused)"; ask "$M" menus; say "  menus: ${A:0:120}"; ask "$M" paused; say "  paused: ${A:0:80}"; return 1; }
		say "  $M walks on W (always-run is off): Caps Lock pressed to turn it on"; ask "$M" "key $VK_CAPS 80"; sleep 0.6
	done
	return 1
}
discover() { # what the scene holds, for the run's record (read only)
	local w c
	for w in host vm; do for c in status "net status" controls equipped pos paused menus "refinfo 000250FE" "refs 42 2500" "paframes 3000" "markers Vault" "markers Sanctuary" "markers Red Rocket" "forms 36 Stairs" "forms 37 Stairs" "forms 36 Ramp" "forms 36 Stair" "forms 42 Chair" "refs 36 9000 Stair" "refs 37 9000 Stair" "refs 36 9000 Step" "refs 36 9000 Ramp" "refs 36 9000 Bridge" "refs 36 9000 Porch"; do ask "$w" "$c"; printf '%s\t%s\t%s\n' "$w" "$c" "$A"; done; done >> "$OUT/discover.tsv"
}

run() {
	rig_guard || exit 2
	OUT=${MA_OUT:-$CHECKS/moveaudit-$(date +%m%d-%H%M%S)}; mkdir -p "$OUT"; local groups="${*:-$GROUPS_ALL}" g rigfail=0
	TOK_H=$(tr -d '\r\n ' < "$HOST_F4SE/F4Multiplayer_dev.token"); TOK_V=$(tr -d '\r\n ' < "$STAGE/vm-dev.token")
	say "moveaudit: $groups; movers: ${MA_MOVERS:-host vm}; files in $OUT"
	ask host status || { say "RIG: the host's game does not answer"; exit 2; }; [[ $A == *ingame=true* ]] || { say "RIG: the host is not in a game: ${A:0:80}"; exit 2; }
	ask vm status || { say "RIG: the VM's game does not answer"; exit 2; }; [[ $A == *ingame=true* ]] || { say "RIG: the VM is not in a game: ${A:0:80}"; exit 2; }
	say "console placement: both players are put on the street in Sanctuary (setpos) before each segment; all movement after it is real input"
	M=host; W=vm; GROUP=setup; SEG=-
	put host $AX $AY $AZ $AH; put vm $MX $MY $AZ $AH; sleep 3; discover
	local c; c=$(bash "$RIG/trace.sh" clock 2>&1 | tail -1); say "clocks: $c"
	OFFSET=$(bash "$RIG/trace.sh" offset 2>/dev/null | tail -1); [[ $OFFSET =~ ^-?[0-9]+$ ]] || { say "RIG: the VM's clock offset was not read ('$OFFSET')"; exit 2; }
	for M in ${MA_MOVERS:-host vm}; do
		[ "$M" = host ] && W=vm || W=host
		GROUP=calib; put "$W" $AX $AY $AZ $AH
		calib || { rigfail=$((rigfail + 1)); say "RIG: $M's movement keys could not be made to work: its groups are not run"; continue; }
		for g in $groups; do
			declare -F "g_$g" >/dev/null || { say "no such group: $g"; continue; }
			group "$g"
			if [ "$g" != drop ]; then ask "$M" "pad unplug"; fi
		done
	done
	# MA_NOANALYSE=1: the measuring is left for `analyse <folder>` later (it takes minutes and needs no rig)
	if [ -n "$MA_NOANALYSE" ]; then say "not analysed yet: bash tools/rig/moveaudit.sh analyse $OUT"; else analyse "$OUT"; fi
}

# analyse <folder>: every segment of a run measured (moveaudit.awk), one line each, and the table kept as results.tsv
analyse() {
	local d=$1 m g s t0 ko t1 st off kind opts o line v why met bad=0 n=0 voids=0; local -a va
	[ -f "$d/marks.tsv" ] || { echo "no marks.tsv in $d"; return 2; }
	: > "$d/results.tsv"
	while IFS=$'\t' read -r t0 m g s ko; do
		kind=${ko%% *}; opts=${ko#"$kind"}; va=(); for o in $opts; do va+=(-v "$o"); done
		t1=$(awk -F'\t' -v m="$m" -v g="$g" -v s="$s" '$2 == m && $3 == g && $4 == s && $5 == "end" { t = $1 } END { print t }' "$d/marks.tsv")
		IFS=$'\t' read -r st off < <(awk -F'\t' -v m="$m" -v g="$g" '$1 == m && $2 == g { r = $3 "\t" $4 "\t" $5 } END { print r }' "$d/groups.tsv" 2>/dev/null)
		if [ -z "$t1" ]; then line=$'VOID\tthe segment was not finished\t'
		elif [ "$st" != ok ]; then line=$'VOID\tthe two traces could not be lined up (the rig: '"$(awk -F'\t' -v m="$m" -v g="$g" '$1 == m && $2 == g { r = $5 } END { print r }' "$d/groups.tsv" 2>/dev/null)"$')\t'
		else
			awk -F'\t' -v m="$m" -v g="$g" -v s="$s" '$2 == m && $3 == g && $4 == s' "$d/marks.tsv" > "$d/.seg.tsv"
			local oo=0 vo=0; if [ "$m" = host ]; then vo=$off; else oo=$off; fi
			[ -f "$d/$m-$g-anim.tsv" ] || : > "$d/$m-$g-anim.tsv"
			# shellcheck disable=SC2086
			line=$(awk -f "$AWK" -v kind="$kind" -v t0="$t0" -v t1="$t1" -v ownOff="$oo" -v viewOff="$vo" -v ownF="$d/$m-$g-own.csv" -v viewF="$d/$m-$g-view.csv" \
				-v animF="$d/$m-$g-anim.tsv" -v markF="$d/.seg.tsv" "${va[@]}" $MA_AWK "$d/$m-$g-own.csv" "$d/$m-$g-view.csv" FS='\t' "$d/$m-$g-anim.tsv" "$d/.seg.tsv" 2>&1 | tail -1)
			rm -f "$d/.seg.tsv"
		fi
		IFS=$'\t' read -r v why met <<<"$line"
		case "$v" in PASS) ;; FAIL) bad=$((bad + 1)) ;; VOID) voids=$((voids + 1)) ;; *) v=VOID; why="the measuring gave no verdict: ${line:0:120}"; met=""; voids=$((voids + 1)) ;; esac
		n=$((n + 1)); printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$v" "$m" "$g" "$s" "$kind" "$why" "$met" >> "$d/results.tsv"
		printf '%-4s %-4s moves  %-22s %s\n' "$v" "$m" "$s" "$why"
	done < <(awk -F'\t' '$5 == "begin" { print $1 "\t" $2 "\t" $3 "\t" $4 "\t" $6 }' "$d/marks.tsv")
	echo "moveaudit: $n segments, $bad FAIL, $voids VOID; numbers in $d/results.tsv"
	[ "$bad" -eq 0 ]
}

# selftest: made-up traces through the measuring. The mover stands 1 s, runs 4 s down the street at 373 u/s, stands 3 s; the
# stand-in is that path <delay> ms later, with one fault at a time.
selftest() {
	local d bad=0; d=$(mktemp -d)
	gen() { # gen <name> <delay ms> <fault>
		awk -v delay="$2" -v fault="$3" -v own="$d/$1-own.csv" -v view="$d/$1-view.csv" -v anim="$d/$1-anim.tsv" -v marks="$d/$1-marks.tsv" '
		function along(t,   s) { if (fault == "still" || fault ~ /^sit/) return 0; s = (t < 1000) ? 0 : (t > 5000 ? 4000 : t - 1000); return 0.373 * s }
		function vpos(t,   a) { a = along(t - delay)
			if (fault == "snap" && t >= 3000 && t < 3200) a += 60
			if (fault == "overshoot" && t - delay > 5000) { e = t - delay - 5000; a += (e < 150) ? 50 * e / 150 : (e < 450 ? 50 * (450 - e) / 300 : 0) }
			if (fault == "slow") a *= 0.8
			return a }
		BEGIN { T = 1795000000000; sx = 0.8660254; sy = 0.5
			for (t = -700; t <= 8700; t += 16.667) {
				a = along(t); printf "%.3f,me,%.2f,%.2f,7800.00,60.00,%.1f\n", T + t, -80566 + a * sx, 89475 + a * sy, 0 > own
				a = vpos(t + 7); printf "%.3f,p2,%.2f,%.2f,7800.00,60.00,0.0\n", T + t + 7, -80566 + a * sx, 89475 + a * sy > view
				printf "%.3f,fp2,0,0,0,0,0\n", T + t + 7 > view
			}
			for (t = 0; t <= 8500; t += 33) {
				mv = (t - delay > 1000 && t - delay < 5000) ? "true" : "false"; if (fault == "onthespot") mv = "true"; if (fault == "nolegs" || fault ~ /^sit/) mv = "false"
				leg = (mv == "true" && fault != "glide") ? 0.4 * sin(t / 80) : 0.01; gait = (fault == "gait") ? 0 : 1
				# sitting: the mover is seated from 1500 to 5500 ms; the stand-in 600 ms later, or late, or never, or for good
				sit = 0; if (fault ~ /^sit/) { s0 = 2100 + (fault == "sitlate" ? 4000 : 0); if (fault != "sitnever" && t >= s0 && (fault == "sitstuck" || t < s0 + 4000)) sit = 3 }
				printf "%d\tA\tFF000001 base=x 3d=true vars: Speed=373.0 Direction=0.0 iSyncLocomotionSpeed=%di iIsInSneak=0i leg=(%.3f,0.100,0.200) moveMode=00A6 puppet: moving=%s speed=373 idleStops=0 sit=%d furniture=00000000\n", T + t, gait, leg, mv, sit > anim
				if (t % 330 < 33) printf "%d\tE\t%s\n", T + t, (mv == "true" && fault != "nofeet") ? "FootLeft() FootRight() SoundPlay(x)" : "none" > anim
			}
			printf "%d\thost\tg\ts\tbegin\trun\n", T > marks
			printf "%d\thost\tg\ts\tkey\t0x57 4000 pressing 57 for 4000 ms front=yes\n", T + 1000 > marks
			printf "%d\thost\tg\ts\tprobe\t00000014 base=7 vars: Speed=373.0 iSyncLocomotionSpeed=1i iIsInSneak=0i leg=(0.1,0.1,0.1) moveMode=00A6 \n", T + 3000 > marks
			printf "%d\thost\tg\ts\tmev\tFootLeft() FootRight() FootLeft() FootRight() FootLeft() FootRight() FootLeft() FootRight() FootLeft() FootRight() FootLeft() FootRight() FootLeft() FootRight() FootLeft() FootRight() FootLeft() FootRight() FootLeft() FootRight() FootLeft() FootRight() FootLeft() FootRight()\n", T + 5000 > marks
			if (fault ~ /^sit/) for (t = 0; t <= 8000; t += 250) printf "%d\thost\tg\ts\tmsit\t%d\n", T + t, (t >= 1500 && t < 5500) ? 3 : 0 > marks
			printf "%d\thost\tg\ts\tend\t\n", T + 8000 > marks
		}'
	}
	t() { # t <name> <delay> <fault> <verdict> <text the reason holds>
		gen "$1" "$2" "$3"
		local r; r=$(awk -f "$AWK" -v kind="${6:-run}" -v t0=1795000000000 -v t1=1795000008000 -v ownOff=0 -v viewOff=0 -v ownF="$d/$1-own.csv" -v viewF="$d/$1-view.csv" \
			-v animF="$d/$1-anim.tsv" -v markF="$d/$1-marks.tsv" "$d/$1-own.csv" "$d/$1-view.csv" FS='\t' "$d/$1-anim.tsv" "$d/$1-marks.tsv" 2>&1 | tail -1)
		local v=${r%%$'\t'*} why; why=$(cut -f2 <<<"$r")
		if [ "$v" = "$4" ] && [[ $why == *"$5"* ]]; then echo "ok   $1: $v ${why:0:100}"; else echo "FAIL $1: got [$v: $why], want [$4: ..$5..]"; echo "     $(cut -f3 <<<"$r")"; bad=1; fi
	}
	t "a stand-in 120 ms behind, nothing wrong" 120 none PASS ok
	t "a stand-in 500 ms behind" 500 none FAIL "ms behind the mover"
	t "a 60 u jump forward and back" 120 snap FAIL "snap(s)"
	t "50 u past the stop, then back" 120 overshoot FAIL "past where the mover stopped"
	t "four fifths of the distance" 120 slow FAIL "times the mover's distance"
	t "legs never told to move" 120 nolegs FAIL "legs were told to move"
	t "legs told to move, none swinging" 120 glide FAIL "glides"
	t "legs still going after the stop" 120 onthespot FAIL "still told to move"
	t "a walk animation at run speed" 120 gait FAIL "plays gait 0"
	t "no footfalls" 120 nofeet FAIL "footfalls"
	t "the mover never moved" 120 still VOID "did not move"
	t "sits and gets up 600 ms after the mover" 120 sit PASS ok sit
	t "sits 4 s after the mover" 120 sitlate FAIL "sat down" sit
	t "never sits" 120 sitnever FAIL "never sat down" sit
	t "stays seated" 120 sitstuck FAIL "stayed seated" sit
	t "the mover never sat" 120 still VOID "never sat" sit
	# the plumbing: a run's folder read by analyse, and a field read from a reply
	mkdir "$d/run"; gen x 120 none; cp -f "$d/x-own.csv" "$d/run/host-g-own.csv"; cp -f "$d/x-view.csv" "$d/run/host-g-view.csv"; cp -f "$d/x-anim.tsv" "$d/run/host-g-anim.tsv"
	cp -f "$d/x-marks.tsv" "$d/run/marks.tsv"; printf 'host\tg\tok\t0\ttest\n' > "$d/run/groups.tsv"
	local r; r=$(analyse "$d/run" | head -1); if [[ $r == "PASS host moves  s "*ok* ]]; then echo "ok   a run's folder is read: $r"; else echo "FAIL a run's folder: got [$r]"; bad=1; fi
	printf 'host\tg\trig\t0\ta clock was set\n' > "$d/run/groups.tsv"; r=$(analyse "$d/run" | head -1)
	if [[ $r == VOID*"a clock was set"* ]]; then echo "ok   clocks that can't be lined up are VOID"; else echo "FAIL clocks: got [$r]"; bad=1; fi
	fieldof "sneak: blocked=false sneaking=true legs=1/2 camera=8" camera; if [ "$F" = 8 ]; then echo "ok   a field is read from a reply"; else echo "FAIL field: got [$F]"; bad=1; fi
	fieldof "drawn=true weaponState=2 00004822 '10mm'" drawn; if [ "$F" = true ]; then echo "ok   a reply's first field is read"; else echo "FAIL first field: got [$F]"; bad=1; fi
	rm -rf "$d"; [ $bad -eq 0 ] && echo "selftest: all ok" || echo "selftest: FAILED"; return $bad
}

case "$1" in
run) shift; run "$@" ;;
analyse) analyse "${2:?the folder of a run}" ;;
selftest) selftest ;;
*) echo "usage: moveaudit.sh run [groups: $GROUPS_ALL] | analyse <folder> | selftest" ;;
esac
