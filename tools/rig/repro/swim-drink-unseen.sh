#!/bin/bash
# swim-drink-unseen.sh [who]: a player's report on 0.20.1: "Drinking water from sinks and open water sources isn't synced. Swimming
# is not in an optimal state, as well."
#
# One player (who: vm by default; the other game watches) wades into the sea at Nordhagen Beach, drinks from it, swims out at the
# surface, dives, drinks while swimming, and walks out again. The watcher's game is asked what the swimmer's stand-in did:
#   - the drink: the swimmer's own animation graph raises a drink event as it bends to the water; does the stand-in's?
#   - the swim: the swimmer's graph goes into its swim state as the water takes them (and out as they leave it); does the stand-in's,
#     or does it walk along the sea bed and across the surface upright? Its body's own 3D says which too: how high its head and
#     the middle of its body are over its feet (a standing person's head is about 120 up, a swimmer lies flat), read while it
#     stood on the beach and again as its owner swims. Its position is compared with its owner's all the way (both games' `trace`).
# YES = the watcher's game missed any of it; NO = the stand-in drank and swam as its owner did; VOID = the swimmer never drank or
# swam (the scene, not the bug).
#
# Console placement (setpos) puts both players on the beach, and god mode is on (the sea's rads, and what lives in it); every move
# after that is a real walk (the pad's stick), every drink a real E held down, every look down a real mouse move.
# PART=sea|pump|both (sea): `pump` has the player use a settlement's water pump (work furniture) as well, which is only reported.
# TRIALS=1: after each reading, the stand-in's graph is sent the game's own swim and drink events by hand (dev `event`), to see what
# it takes. That is a look for a fix, said in the output where it happens, and a run with it is no proof of anything after it.
. "$(dirname "$0")/lib.sh"
. "$RIG/crashlib.sh"
S=${1:-vm}; W=$(other_game "$S"); OUT=$REPRO_OUT
PART=${PART:-sea}; TRIALS=${TRIALS:-0}
SURFACE=450  # Nordhagen Beach's water height
PUMP=${PUMP:-002472D3}
need_session
BAD=""; GOOD=0
differs() { BAD="${BAD:+$BAD; }$1"; say "UNSEEN: $1"; }
same() { GOOD=$((GOOD + 1)); say "seen: $1"; }

_vmshot_ready() { vmssh 'Test-Path C:\F4MP\shot.png' | grep -q True; }
# shot <who> <name>: a still of that game's window, kept as evidence (never steered by)
shot() {
	if [ "$1" = host ]; then powershell -NoProfile -ExecutionPolicy Bypass -File "$REPO/tools/screenshot.ps1" -OutFile "$(cygpath -w "$OUT/$2.png")" -Width 1600 >/dev/null 2>&1
	else
		vmssh 'Remove-Item C:\F4MP\shot.png -ErrorAction SilentlyContinue' >/dev/null
		vmssh "& C:\\F4MP\\in-vm-run.ps1 -Name F4MP-Shot -Exe 'C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe' -Arguments '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File C:\\F4MP\\vmshot.ps1'" >/dev/null
		wait_for 20 _vmshot_ready && scp -q -i "$VM_KEY" -o LogLevel=ERROR "$VM_SSH:C:/F4MP/shot.png" "$OUT/$2.png" 2>/dev/null
	fi
	[ -f "$OUT/$2.png" ] && say "still: $OUT/$2.png"
}
# bone_z <nodes file> <name>: the world height of the middle of the 3D node of that name ('' when it has none, or no size)
bone_z() { grep -iE "#[0-9]+ [a-z]+ '$2' c=" "$1" | grep -vE ' r=0( |$)' | head -1 | sed -E 's/.* c=\([^,]*, [^,]*, (-?[0-9]+)\).*/\1/'; }
# root_z <nodes file>: ... of the whole body (the first node listed: its bound holds every part)
root_z() { grep -E "#0 [a-z]+ '" "$1" | head -1 | sed -E 's/.* c=\([^,]*, [^,]*, (-?[0-9]+)\).*/\1/'; }
# snap <label>: the swimmer in its own game and its stand-in in the watcher's, now. HEAD_UP and MID_UP: the stand-in's head and
# the middle of its body over its feet (kept in $OUT/pose-<label>.txt too, for a snap taken beside a walk); S_MODE, P_MODE: the
# two's movement bits (0400 = swimming); S_Z, P_Z: their heights
snap() {
	local a b hz mz
	a=$(dc "$S" 'refinfo 00000014'); b=$(dc "$W" "refinfo $P")
	S_Z=$(field "$a" z); S_Z=${S_Z%.*}; P_Z=$(field "$b" z); P_Z=${P_Z%.*}
	S_MODE=$(grep -oE 'moveMode=[0-9A-Fa-f]+' <<<"$a" | cut -d= -f2); P_MODE=$(grep -oE 'moveMode=[0-9A-Fa-f]+' <<<"$b" | cut -d= -f2)
	dc "$W" "nodes $P" | tr ';' '\n' > "$OUT/nodes-standin-$1.txt"; dc "$S" "nodes 00000014" | tr ';' '\n' > "$OUT/nodes-swimmer-$1.txt"
	hz=$(bone_z "$OUT/nodes-standin-$1.txt" HEAD); HEAD_UP=""; is_int "$hz" && is_int "$P_Z" && HEAD_UP=$((hz - P_Z))
	mz=$(root_z "$OUT/nodes-standin-$1.txt"); MID_UP=""; is_int "$mz" && is_int "$P_Z" && MID_UP=$((mz - P_Z))
	echo "${HEAD_UP:-?} ${MID_UP:-?}" > "$OUT/pose-$1.txt"
	say "[$1] swimmer: x=$(field "$a" x) z=$S_Z (depth of the water over its feet $((SURFACE - ${S_Z:-0}))) look=$(field "$a" rx) moveMode=$S_MODE | its stand-in: x=$(field "$b" x) z=$P_Z look=$(field "$b" rx) moveMode=$P_MODE head ${HEAD_UP:-?} and middle ${MID_UP:-?} over its feet"
	{
		echo "[$1] swimmer refinfo: $a"; echo "[$1] stand-in refinfo: $b"
		echo "[$1] stand-in anim: $(dc "$W" "anim $P iState iSyncIdleLocomotion bIsInMT iLocomotionSpeedState")"
		echo "[$1] swimmer anim: $(dc "$S" "anim 00000014 iState iSyncIdleLocomotion bIsInMT iLocomotionSpeedState")"
		echo "[$1] stand-in motion: $(dc "$W" "motion $P")"
	} >> "$OUT/raw.log"
}
swims() { [ -n "$1" ] && [ $((0x$1 & 0x400)) -ne 0 ]; }
# lower <label> <than label> <by>: the stand-in's body was lower at the first snap than at the second by that much or more (its
# head, or the middle of its body when its head could not be read): bent to the water, or lying in it
lower() {
	local a b i
	for i in 1 2; do
		a=$(cut -d' ' -f$i "$OUT/pose-$1.txt" 2>/dev/null); b=$(cut -d' ' -f$i "$OUT/pose-$2.txt" 2>/dev/null)
		if is_int "$a" && is_int "$b"; then [ $((b - a)) -ge "$3" ]; return; fi
	done
	return 1
}
pose() { local p; p=$(cat "$OUT/pose-$1.txt" 2>/dev/null); echo "head ${p%% *} middle ${p##* }"; }
# The two graphs' events since the last areset: OWN (the swimmer's, in its own game) and SEEN (its stand-in's, in the watcher's)
aread() { OWN=$(dc "$S" 'animlog on'); SEEN=$(dc "$W" "animlog on $P"); echo "[$1] swimmer's graph: $OWN" >> "$OUT/raw.log"; echo "[$1] stand-in's graph: $SEEN" >> "$OUT/raw.log"; }
areset() { dc "$S" 'animlog off' >/dev/null; dc "$S" 'animlog on' >/dev/null; dc "$W" 'animlog off' >/dev/null; dc "$W" "animlog on $P" >/dev/null; }
events() { grep -oiE "[A-Za-z_]*($2)[A-Za-z_]*\(" <<<"$1" | tr -d '(' | sort | uniq -c | awk '{printf "%s%s x%s", (NR > 1 ? ", " : ""), $2, $1}'; }
look() { dc "$W" "turnto $P 300" >/dev/null; sleep 1; }
# drink <label> <what>: the swimmer holds E for 3 s on the water it looks at; was a drink seen on the stand-in?
drink() {
	local own seen
	snap "$1-before"; areset; say "rollover: '$(rrollover "$S")'"
	dc "$S" focus >/dev/null; sleep 0.3; dc "$S" "key $VK_E 3000" >/dev/null; sleep 1.8; snap "$1"; shot "$W" "$1"; sleep 3.5; aread "$1"
	own=$(events "$OWN" 'drink|LoopingActivate'); seen=$(events "$SEEN" 'drink|LoopingActivate')
	say "[$1] the swimmer's own graph: ${own:-no drink event} (all of it: $(events "$OWN" '.' | cut -c1-400)) | its stand-in's: ${seen:-no drink event} (all of it: $(events "$SEEN" '.' | cut -c1-400))"
	say "[$1] the stand-in's body before: $(pose "$1-before"); 1.8 s into the drink: $(pose "$1")"
	if [ -z "$own" ]; then say "[$1] VOID for this drink: the swimmer's own game showed none (nothing to drink under the crosshair?)"
	elif [ -n "$seen" ]; then same "$2: the stand-in drank too ($seen)"
	elif lower "$1" "$1-before" 20; then same "$2: the stand-in bent to the water as its owner drank (its body $(pose "$1"), $(pose "$1-before") before)"
	else differs "$2: the swimmer drank (${own}), its stand-in in the other game did nothing (no drink event; its body $(pose "$1"), $(pose "$1-before") before)"; fi
}

say "status: $S $(dc "$S" status) | $W $(dc "$W" status)"
dc "$S" "god on" >/dev/null; dc "$W" "god on" >/dev/null; sleep 0.5
say "god mode on both (the sea's rads and what lives in it): $S $(dc "$S" "god on") | $W $(dc "$W" "god on")"
say "console placement (setpos): the swimmer ($S) to the beach at 63250 640, the watcher ($W) to 63250 900; every move after it is real"
place "$S" 63250 640 760 270; place "$W" 63250 900 760 270; sleep 5
P=$(dc "$W" 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | head -1 | cut -d= -f2)
[ -n "$P" ] || verdict VOID "the watcher's game has no stand-in for the swimmer (no puppet= in its net status)"
say "the swimmer's stand-in in the watcher's game: $P; swimmer at $(pos_of "$S"), watcher at $(pos_of "$W")"
for w in "$S" "$W"; do rmark "$w"; done
areset; snap dry
shot "$W" 0-dry

if [ "$PART" != pump ]; then
	say "== wading: the swimmer walks into the shallows (62650 640, under 100 deep) and the watcher to 62500 900"
	rwalk "$S" 62650 640 60; say "swimmer: $WALKED"; rwalk "$W" 62500 900 60; say "watcher: $WALKED"
	look; snap wade
	say "== a drink from open water: the swimmer looks down 50 degrees (a real mouse move) and holds E for 3 s"
	pitch_down "$S" 50; sleep 0.5
	drink 1-drink-open "a drink from open water"
	if [ "$TRIALS" = 1 ]; then
		say "TRIAL (by hand, not the game's doing): the stand-in sent g_drinkStart: $(dc "$W" "event $P g_drinkStart")"; sleep 2.5; snap 1t-trial-drink; shot "$W" 1t-trial-drink
		say "TRIAL: its body then: $(pose 1t-trial-drink); it is sent drinkEnd: $(dc "$W" "event $P drinkEnd")"; sleep 1.5; snap 1t-trial-drink-end
	fi
	pitch_down "$S" 0

	say "== into deep water: the swimmer walks west to 61800 640 (200 deep), both games tracing at 20 Hz"
	areset; dc "$S" 'trace start 20' >/dev/null; dc "$W" 'trace start 20' >/dev/null
	rwalk "$S" 61800 640 60; say "swimmer: $WALKED"; sleep 1; aread swim-start
	look; snap 2-tread; shot "$W" 2-tread
	own=$(events "$OWN" 'swim'); seen=$(events "$SEEN" 'swim')
	say "[swim start] the swimmer's own graph: ${own:-no swim event} (all of it: $(events "$OWN" '.' | cut -c1-400)) | its stand-in's: ${seen:-no swim event}"
	SWAM=0; if swims "$S_MODE" || [ -n "$own" ]; then SWAM=1; fi
	OWN_SWIM=${own:-moveMode $S_MODE}; SEEN_SWIM=$seen
	say "== swimming on at the surface (to 61450 600), read on the way"
	( sleep 1.5; snap 3-swim-forward; shot "$W" 3-swim-forward ) & rwalk "$S" 61450 600 60; wait; say "swimmer: $WALKED"
	if [ "$SWAM" = 0 ]; then say "VOID for the swim: the swimmer was not swimming at 61800 640 (moveMode=$S_MODE, z=$S_Z)"
	elif [ -n "$SEEN_SWIM" ]; then same "the swim's start: the stand-in's graph went into its swim too ($SEEN_SWIM; its body swimming forward: $(pose 3-swim-forward), on the beach: $(pose dry))"
	elif lower 3-swim-forward dry 40; then same "the swim's start: the stand-in lies in the water as its owner swims (its body $(pose 3-swim-forward), $(pose dry) on the beach)"
	else differs "the swim's start: the swimmer's graph went into its swim state ($OWN_SWIM), its stand-in's never did (no swim event; its body as its owner swam forward: $(pose 3-swim-forward), on the beach: $(pose dry))"; fi
	if [ "$TRIALS" = 1 ]; then
		look; say "TRIAL (by hand): the stand-in sent g_SwimStart_MT: $(dc "$W" "event $P g_SwimStart_MT")"; sleep 2; snap 3t-trial-tread; shot "$W" 3t-trial-tread
		( sleep 1.5; snap 3t-trial-forward; shot "$W" 3t-trial-forward ) & rwalk "$S" 61300 560 60; wait
		say "TRIAL: its body treading: $(pose 3t-trial-tread), as its owner swam forward: $(pose 3t-trial-forward)"
	fi

	say "== a dive: the swimmer looks down 55 degrees (a real mouse move) and swims on to 60900 500 (500 deep and more)"
	pitch_down "$S" 55; ( sleep 2.5; snap 4-diving ) & rwalk "$S" 60900 500 60; wait; say "swimmer: $WALKED"
	look; snap 5-dived; shot "$W" 5-dived
	say "== back up: the swimmer looks up 40 degrees and swims to 61050 520, then looks down 30 and holds E for 3 s (a drink while swimming)"
	pitch_down "$S" -40; rwalk "$S" 61050 520 60; pitch_down "$S" 30; sleep 2
	drink 6-drink-swimming "a drink while swimming"
	if [ "$TRIALS" = 1 ]; then
		say "TRIAL (by hand): the stand-in sent swimDrinkStart: $(dc "$W" "event $P swimDrinkStart")"; sleep 2; snap 6t-trial-swimdrink; shot "$W" 6t-trial-swimdrink
		say "TRIAL: it is sent drinkEnd: $(dc "$W" "event $P drinkEnd")"; sleep 1
	fi

	say "== out of the water: the swimmer looks level and walks back to 62700 640"
	pitch_down "$S" 0; areset; rwalk "$S" 61800 640 80; rwalk "$S" 62700 640 60; say "swimmer: $WALKED"; sleep 1; aread swim-stop
	look; snap 7-out; shot "$W" 7-out
	own=$(events "$OWN" 'swim'); seen=$(events "$SEEN" 'swim')
	say "[swim stop] the swimmer's own graph: ${own:-no swim event} | its stand-in's: ${seen:-no swim event}"
	if [ "$SWAM" = 1 ] && ! swims "$S_MODE"; then
		if swims "$P_MODE"; then differs "the swim's end: the swimmer stands in the shallows, its stand-in still has the swimming bit (moveMode=$P_MODE)"
		elif lower 7-out dry 40; then differs "the swim's end: the swimmer stands in the shallows, its stand-in still lies as a swimmer does (its body $(pose 7-out), $(pose dry) on the beach)"
		else same "the swim's end: the stand-in stands as its owner does (its body $(pose 7-out), $(pose dry) on the beach)"; fi
	fi
	if [ "$TRIALS" = 1 ]; then say "TRIAL (by hand): the stand-in sent SwimStop: $(dc "$W" "event $P SwimStop")"; sleep 2; snap 7t-trial-swimstop; fi
	say "trace: $S $(dc "$S" 'trace stop') | $W $(dc "$W" 'trace stop')"
	cp -f "$HOST_F4SE/F4Multiplayer_trace.csv" "$OUT/trace-host.csv" 2>/dev/null
	scp -q -i "$VM_KEY" -o LogLevel=ERROR "$VM_SSH:$VM_F4SE/F4Multiplayer_trace.csv" "$OUT/trace-vm.csv" 2>/dev/null
	# The stand-in's path in the watcher's trace: how far it moved between samples (a snap is a step far over a swimmer's pace), and
	# how its height went (a bob is the height going up and down while its owner's holds).
	if [ -s "$OUT/trace-$W.csv" ]; then
		say "the stand-in's path in the watcher's trace (who != player): $(awk -F, 'NR > 1 && $2 != "player" && $2 != "self" { n++; if (n > 1) { d = sqrt(($3-x)^2 + ($4-y)^2); dz = $5 - z; if (dz < 0) dz = -dz; if (d > maxd) maxd = d; if (dz > maxz) maxz = dz; if (d > 60) snaps++; if (dz > 25) zsnaps++; } x = $3; y = $4; z = $5; if (zmin == "" || $5 < zmin) zmin = $5; if (zmax == "" || $5 > zmax) zmax = $5 } END { printf "%d samples, longest step %.0f (steps over 60: %d), biggest change of height in a step %.0f (over 25: %d), height from %.0f to %.0f", n, maxd, snaps, maxz, zsnaps, zmin, zmax }' "$OUT/trace-$W.csv")"
		say "trace rows by who: $W $(cut -d, -f2 "$OUT/trace-$W.csv" | sort | uniq -c | paste -sd' ' -) | $S $(cut -d, -f2 "$OUT/trace-$S.csv" 2>/dev/null | sort | uniq -c | paste -sd' ' -)"
	fi
fi

if [ "$PART" != sea ]; then
	say "== the water pump ($PUMP, work furniture; reported, not judged): console placement of both players beside it (setpos), then a real E"
	place "$S" 63260 3480 760 0; place "$W" 63150 3380 760 0; sleep 5
	say "pump: $(dc "$S" "refinfo $PUMP" | cut -c1-160)"; areset
	rwalk "$S" "$PUMP" 90 0.6; rpress_e "$S" "$PUMP"; say "rollover: '$ROLLOVER'"; sleep 2.5
	look; snap 8-pump; shot "$W" 8-pump; say "watcher's seat read-out: $(dc "$W" "seat $P" | cut -c1-300)"; sleep 4; aread pump
	say "[pump] the swimmer's own graph: $(events "$OWN" '.' | cut -c1-600) | its stand-in's: $(events "$SEEN" '.' | cut -c1-600)"
	rkey "$S" "$VK_E" 100; sleep 2
fi

for w in "$S" "$W"; do rsince "$w" > "$OUT/log-$w.txt"; done
say "the watcher's log on swimming and drinking: $(grep -ciE 'swim|drink' "$OUT/log-$W.txt") lines$(grep -iE 'swim|drink' "$OUT/log-$W.txt" | tail -4 | sed 's/^/ | /' | cut -c1-240 | tr -d '\n')"
say "crash folders: host $(crash_stamp "$(crash_list_host)") | vm $(crash_stamp "$(crash_list_vm)")"
[ "$TRIALS" = 1 ] && say "note: this run sent the stand-in events by hand (TRIALS=1): what follows a TRIAL line is no proof"
if [ -n "$BAD" ]; then verdict YES "$BAD"
elif [ "$GOOD" -gt 0 ]; then verdict NO "the stand-in did what its owner did, $GOOD of $GOOD checks (drinks and the swim's start and end)"
else verdict VOID "the swimmer never drank nor swam (nothing to judge)"; fi
