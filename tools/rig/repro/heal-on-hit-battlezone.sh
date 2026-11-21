#!/bin/bash
# heal-on-hit-battlezone.sh [shooter: vm|host|both] [waves: "52" | "52 54" ...]: "Some enemies spawned in the RobCo battlezone in
# Nuka World (scripted spawns) suffered from the heal on hit bug. We've noticed that it fixes itself after a few seconds" (two
# players, 0.20.1). The arena's robots stand in their pods from the cell's load (placed by the plugin, unconscious) and a quest
# stage wakes each wave; the run shoots a wave's robots from the moment its stage is set and reads each robot's health in BOTH
# games round every shot.
#
# Set-up, said where it happens: both players go to the arena's cell by console (coc DLC04GZBattlezone01, the host first, unless
# ENTER=0: then they are there already), each is given a 10mm pistol and its ammo by console, and the wave is started by
# console in STARTER's game (setstage DLC04GZBattlezone <52|54|56|58>: the arena's own "Start Wave" stages). Players are in
# god mode (rig.sh join). Shots are real clicks, aimed with the dev channel's turnto; a shooter with no line to any robot of the
# wave after NOLINE s walks towards one (a real walk). With shooter=both the other player fires at the same robot right after.
#
# Each shot: the robot's health, runner (owner=, 0 nobody), ghost and dead in the shooter's game (S) and the friend's (O),
# before the click and 0.4 s, 1.3 s and 2.6 s after. A shot LANDED when S's reading fell; it HEALED when S's reading then
# rose again by half the fall or more with the robot alive (robots do not heal themselves), or when the robot's health in S is
# higher at the next shot than it was left. The whole table is in samples.tsv, the games' health lines since the stage in
# health-<game>.log, the robots before the wave in before.txt.
#
# REPRO: YES when a shot healed; NO when MIN_LANDED shots or more landed in the first SOON s after a wave's stage (the window the
# report names) and none healed; VOID otherwise (no line of fire, the robots did not wake, too few shots landed).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
SHOOT=${1:-vm}; case "$SHOOT" in vm|host) S=$SHOOT; BOTH=0;; both) S=vm; BOTH=1;; *) echo "usage: $0 [vm|host|both] [\"52 54\"]"; exit 2;; esac
O=$(other_game "$S"); WAVES=${2:-52}; STARTER=${STARTER:-host}; ENTER=${ENTER:-1}
DUR=${DUR:-70}; SOON=${SOON:-20}; MIN_LANDED=${MIN_LANDED:-4}; NOLINE=${NOLINE:-20}
PISTOL=00004822; AMMO=0001F276; CELL=DLC04GZBattlezone01
need_session

Q=$(dc host "quests edid:DLC04GZBattlezone" | grep -oE '[0-9A-F]{8} DLC04GZBattlezone ' | head -1 | cut -d' ' -f1)
[ -n "$Q" ] || verdict VOID "the arena's quest (DLC04GZBattlezone) is not in the host's game: is Nuka-World installed on the rig?"
IX=${Q:0:2}
# the wave's robots, as the plugin places them (DLCNukaWorld.esm), Protectrons and the big ones first: they are the easy shots
robots_of() {
	case "$1" in
	52) echo "${IX}032FC2 ${IX}029467 ${IX}032FC5 ${IX}032FC4 ${IX}029424" ;;
	54) echo "${IX}029423 ${IX}029422" ;;
	56) echo "${IX}032FC6 ${IX}029468" ;;
	58) echo "${IX}029421 ${IX}032FCA ${IX}032FC8 ${IX}029466 ${IX}029465" ;;
	esac
}
now_ms() { date +%s%3N; }
info() { dc "$1" "refinfo $2"; }
brief() { grep -oE "(3d|disabled|formFlags|ghost|hp|permHp|owner|dead)=[^ ]*" <<<"$1" | paste -sd' ' -; }
hp_of() { local h; h=$(field "$1" hp); echo "${h:-?}"; }
counters() { dc "$1" 'net status' | grep -oE '(shots|copy[A-Za-z]+|damage[A-Za-z]+)=[0-9/]+' | paste -sd' ' -; }
id_of() { field "$(dc "$1" 'net status')" id; }
alive_in() { local i; i=$(info "$1" "$2"); [ "$(field "$i" dead)" = false ] && [ "$(field "$i" 3d)" = true ] && [ "$(field "$i" disabled)" = false ]; }

if [ "$ENTER" = 1 ]; then
	for g in host vm; do
		say "$g goes to the arena by console: coc $CELL"
		dc "$g" "console coc $CELL" >/dev/null; sleep 4; wait_ingame "$g" 60 || verdict VOID "$g did not come out of the load into $CELL"
		dc "$g" "god on" >/dev/null; sleep 3
	done
fi
hc=$(field "$(dc host status)" cell); vc=$(field "$(dc vm status)" cell)
[ -n "$hc" ] && [ "$hc" = "$vc" ] || verdict VOID "the players are not in one cell (host $hc, vm $vc)"
for g in host vm; do
	dc "$g" "console player.additem $PISTOL 1" >/dev/null; dc "$g" "console player.additem $AMMO 400" >/dev/null
	dc "$g" "console player.equipitem $PISTOL" >/dev/null; dc "$g" "draw on" >/dev/null
done
sleep 6
SID=$(id_of "$S"); OID=$(id_of "$O")
say "quest $Q; shooter $S (player $SID), friend $O (player $OID); host at ($(pos_of host)), vm at ($(pos_of vm)); cell $hc"
say "stages host: $(dc host "stages $Q" | cut -c1-200)"
say "stages vm:   $(dc vm "stages $Q" | cut -c1-200)"
: > "$REPRO_OUT/before.txt"; printf 'ms\twave\tshot\tphase\tref\thpS\townerS\tghostS\tdeadS\thpO\townerO\tghostO\tdeadO\n' > "$REPRO_OUT/samples.tsv"
for w in 52 54 56 58; do for r in $(robots_of $w); do
	echo "wave $w $r host: $(brief "$(info host "$r")") | vm: $(brief "$(info vm "$r")")" >> "$REPRO_OUT/before.txt"
done; done
say "the robots before any wave (all in before.txt); wave ${WAVES%% *}'s:"; grep "^wave ${WAVES%% *} " "$REPRO_OUT/before.txt" | while read -r l; do say "  $l"; done

LANDED=0; HEALED=0; SOON_LANDED=0; SHOTS=0; FIRED_NOTE=""
declare -A LEFT   # the health S's game left each robot at, after its last shot
# sample <wave> <shot> <phase> <ref>: both games' reading of the robot into samples.tsv; HS HO (health), DS (dead in S), OWN ("S/O")
sample() {
	local a b; a=$(info "$S" "$4"); b=$(info "$O" "$4")
	HS=$(hp_of "$a"); HO=$(hp_of "$b"); DS=$(field "$a" dead); OWN="$(field "$a" owner)/$(field "$b" owner)"; GH="$(field "$a" ghost)/$(field "$b" ghost)"
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(( $(now_ms) - T0 ))" "$1" "$2" "$3" "$4" "$HS" "$(field "$a" owner)" "$(field "$a" ghost)" "$DS" "$HO" "$(field "$b" owner)" "$(field "$b" ghost)" "$(field "$b" dead)" >> "$REPRO_OUT/samples.tsv"
}
# target <wave>: the first of the wave's robots alive in S's game that S's game says its player can see (T, LINE=yes), else the
# first alive (LINE=no), else none
target() {
	local e r first=""; e=$(dc "$S" "enemies 8000")
	T=""; LINE=no
	for r in $(robots_of "$1"); do
		alive_in "$S" "$r" || continue
		[ -z "$first" ] && first=$r
		if tr ';' '\n' <<<"$e" | grep -E "(^| )$r " | grep -q 'los=true'; then T=$r; LINE=yes; return 0; fi
	done
	T=$first; [ -n "$T" ]
}
gt() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a+0 > b+0)}'; }
sub() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.1f", a - b}'; }
min() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.1f", (a+0 < b+0) ? a : b}'; }

for W in $WAVES; do
	rmark host "w$W"; rmark vm "w$W"
	say "--- wave $W: counters before: $S: $(counters "$S") | $O: $(counters "$O")"
	say "$STARTER starts it by console: setstage $Q $W"
	T0=$(now_ms); dc "$STARTER" "console setstage $Q $W" >/dev/null
	END=$((SECONDS + DUR)); W0=$SECONDS; n=0; walked=0; wave_landed=0
	while [ $SECONDS -lt $END ]; do
		if ! target "$W"; then
			[ $((SECONDS - W0)) -lt 12 ] && { sleep 0.5; continue; }   # not awake yet
			say "wave $W: no robot of it is alive in $S's game any more"; break
		fi
		if [ "$LINE" = no ] && [ "$wave_landed" = 0 ] && [ $((SECONDS - W0)) -ge "$NOLINE" ] && [ "$walked" -lt 2 ]; then
			walked=$((walked + 1)); say "$S has no line to a robot of wave $W after $((SECONDS - W0)) s: it walks towards $T (a real walk)"
			rwalk "$S" "$T" 500; say "  $WALKED"; continue
		fi
		n=$((n + 1)); SHOTS=$((SHOTS + 1))
		dc "$S" "turnto $T 200 aim" >/dev/null; [ "$BOTH" = 1 ] && dc "$O" "turnto $T 200 aim" >/dev/null
		sleep 0.35
		sample "$W" "$n" before "$T"; s0=$HS; o0=$HO; own0=$OWN; gh0=$GH; at=$(( $(now_ms) - T0 ))
		rose=""
		if [ -n "${LEFT[$T]}" ] && [ "$s0" != "?" ] && gt "$(sub "$s0" "${LEFT[$T]}")" 1; then rose=" ROSE between shots (${LEFT[$T]} -> $s0)"; fi
		dc "$S" focus >/dev/null; dc "$S" "click 80" >/dev/null
		[ "$BOTH" = 1 ] && { dc "$O" focus >/dev/null; dc "$O" "click 80" >/dev/null; }
		sleep 0.4; sample "$W" "$n" a0.4 "$T"; s1=$HS; o1=$HO
		sleep 0.9; sample "$W" "$n" a1.3 "$T"; s2=$HS; o2=$HO
		sleep 1.3; sample "$W" "$n" a2.6 "$T"; s3=$HS; o3=$HO; own3=$OWN
		low=$(min "$s1" "$s2"); fall=$(sub "$s0" "$low"); what=missed
		if [ "$s0" != "?" ] && gt "$fall" 0.9; then
			what=landed; LANDED=$((LANDED + 1)); wave_landed=$((wave_landed + 1)); [ "$at" -le $((SOON * 1000)) ] && SOON_LANDED=$((SOON_LANDED + 1))
			back=$(sub "$s3" "$low"); b2=$(sub "$s2" "$s1"); gt "$b2" "$back" && back=$b2
			if [ "$DS" != true ] && gt "$back" "$(awk -v f="$fall" 'BEGIN{printf "%.1f", (f / 2 > 1) ? f / 2 : 1}')"; then what="HEALED (fell $fall, back up $back)"; HEALED=$((HEALED + 1)); fi
		fi
		[ -n "$rose" ] && { HEALED=$((HEALED + 1)); what="$what;$rose"; }
		LEFT[$T]=$s3
		say "wave $W shot $n +$(awk -v m="$at" 'BEGIN{printf "%.1f", m / 1000}')s $T line=$LINE runner S/O $own0 -> $own3 ghost $gh0 | $S: $s0 > $s1 > $s2 > $s3 | $O: $o0 > $o1 > $o2 > $o3 | $what"
	done
	sleep 3
	say "wave $W: counters after: $S: $(counters "$S") | $O: $(counters "$O")"
	for g in host vm; do
		rsince "$g" "w$W" | grep -E 'CopyWatch|CopyHealth|HealthShare|DamageHold|given back|refused|owed|NpcSync: .*(claim|took|taken|lost|releas|hand)|SessionDeaths|dead list' | cut -c1-300 > "$REPRO_OUT/health-$g-w$W.log"
		say "$g's health and claim lines since the stage: $(wc -l < "$REPRO_OUT/health-$g-w$W.log") (health-$g-w$W.log); the first: $(head -6 "$REPRO_OUT/health-$g-w$W.log" | cut -c1-220 | paste -sd'|' -)"
	done
	for r in $(robots_of "$W"); do say "  after: $r host: $(brief "$(info host "$r")") | vm: $(brief "$(info vm "$r")")"; done
done
rkeep host end; rkeep vm end
say "shots $SHOTS, landed $LANDED (in the first $SOON s of a wave: $SOON_LANDED), healed $HEALED"
[ "$HEALED" -gt 0 ] && verdict YES "$HEALED of $LANDED shots that landed on the arena's robots were healed back in the shooter's game"
[ "$SOON_LANDED" -ge "$MIN_LANDED" ] && verdict NO "$LANDED shots landed on the arena's robots ($SOON_LANDED in the first $SOON s after a wave's stage) and none was healed back"
verdict VOID "too few shots landed early to tell ($SOON_LANDED in the first $SOON s, $LANDED in all, of $SHOTS fired): see the shots above for the line of fire and whether the robots woke"
