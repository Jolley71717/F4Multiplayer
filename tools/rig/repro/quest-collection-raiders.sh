#!/bin/bash
# quest-collection-raiders.sh: the raiders a quest creates in a reference COLLECTION alias, seen and hit by both players. Special
# Delivery (MS09Mission01, 000503B8, a side quest that runs once) creates three raider reinforcements in its collection
# RaiderReinforcements (alias 16) and their leader in the reference alias RaiderReinforcementLeader (alias 18), all at its
# ReinforcementMarker (0019A7F6), switched off until its script switches them on. Every game's run makes its own, under its own
# runtime ids. 0.21.1-rc keyed the collection's hostile members with the collection's alias, as it keys a single alias's: the game
# that shared its members first owned the alias, the friend's game held all of its own members back as their twins, and nothing
# pairs a collection's members, so the friend's raiders were shared with nobody: the other player never saw them and could not
# hurt them, and a hit on them stayed in the friend's game (an audit of PR 552). In 0.21.0 and 0.21.1 a collection's members are
# plain spawns: each game shares its own beside its copies of the friend's, so each player sees the raiders twice. With the fix
# a collection's members are keyed and paired: the VM's members stand for the host's (paired, no copies beside them).
#   1. Both games run the quest (console startquest MS09Mission01 in a game where it is not running, said), both players go to the
#      marker (console player.moveto 0019A7F6, said), each with a 10mm and its rounds (console additem), and walk (the pad) about
#      1000 u from it. Each game's collection members are found there (runtime raiders near the marker, in a quest alias, still
#      switched off, not the leader): at least two in each game, else VOID.
#   2. The host's members are switched on first (console <ref>.enable in the host's game, standing for the quest's script), so the
#      host's game shares them first; once they have left the marker (they come at the players), the VM's are switched on.
#   3. The VM shoots one of its paired members (real clicks, up to 12, until one lands in its game): one of the host's members must
#      take the hit (lower health, or dead). Then the host shoots one of its own members: one of the VM's paired members must take it.
# REPRO: YES when a game holds copies of the friend's members beside its own (doubled), when none of the VM's members is paired,
# or when a landed hit stays in the shooter's game. NO when the VM's members are paired with no copies and both directions' hits
# reach the other game. VOID when the scene could not be made (members not found or not switched on, no shot landing in 12, a
# click that fired nothing).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
QUEST=${QUEST:-000503B8}          # Special Delivery (MS09Mission01)
QUEST_EDID=${QUEST_EDID:-MS09Mission01}
LEADER_ALIAS=${LEADER_ALIAS:-18}  # RaiderReinforcementLeader: a single alias, left switched off
MARKER=${MARKER:-0019A7F6}        # ReinforcementMarker, where the quest creates them
AWAY=${AWAY:-1000}                # how far from the marker the players wait
run_cap 1800
need_session

aref() { dc "$1" "aliases $QUEST" | tr '|' '\n' | grep -E "^ Ref $2 " | grep -oE 'here=[0-9A-F]{8}' | cut -d= -f2 | grep -v '^00000000$'; }
info() { dc "$1" "refinfo $2"; }
hp() { field "$(info "$1" "$2")" hp; }
dead() { [ "$(field "$(info "$1" "$2")" dead)" = true ]; }
xy_of() { local i; i=$(info "$1" "$2"); echo "$(field "$i" x) $(field "$i" y)"; }
owner_of() { field "$(dc "$1" "ghosts $2")" owner; }
running() { local f; f=$(field "$(dc "$1" "stages $QUEST")" flags); [ -n "$f" ] && (( 16#$f & 1 )); }
shots_of() { dc "$1" "net status" | grep -oE ' shots=[0-9]+' | head -1 | cut -d= -f2; }
qa_of() { dc "$1" "net status" | grep -oE 'questActors: [^;]*' | head -1 | cut -c1-120; }
lower() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a != "" && b != "" && a + 0 < b - 0.5) }'; }
# raiders <who> [radius]: the runtime (0xFF) NPCs named like a raider in that game's loaded area near its player
raiders() { dc "$1" "refs 45 ${2:-6000} Raider" | tr ';' '\n' | grep -oE '^ ?FF[0-9A-F]{6}' | tr -d ' '; }
# members <who>: that game's members of the collection: runtime raiders within 400 u of the marker, in a quest alias, switched
# off, not the leader
members() {
	local r i leader; leader=$(aref "$1" "$LEADER_ALIAS")
	for r in $(raiders "$1" 8000); do
		[ "$r" = "$leader" ] && continue
		i=$(info "$1" "$r"); [ "$(field "$i" disabled)" = true ] || continue
		[ "$(field "$(dc "$1" "ghosts $r")" questAlias)" = true ] || continue
		d=$(dist_xy "$(field "$i" x) $(field "$i" y)" "$MXY"); [ -n "$d" ] && [ "$d" -le 400 ] && echo "$r"
	done
}
# copies <who> <owner id>: that game's copies of the other player's raiders (runtime raiders near, run by that player)
copies() { local r; for r in $(raiders "$1"); do [ "$(owner_of "$1" "$r")" = "$2" ] && ! dead "$1" "$r" && echo "$r"; done; }
# nearest <who> <x y> <refs...>: the one of the refs nearest that spot in that game
nearest() {
	local w=$1 at=$2 best="" bd=999999 r d; shift 2
	for r in "$@"; do d=$(dist_xy "$(xy_of "$w" "$r")" "$at"); [ -n "$d" ] && [ "$d" -lt "$bd" ] && { bd=$d; best=$r; }; done
	echo "$best"
}
hps() { local w=$1 r; shift; for r in "$@"; do printf '%s=%s%s ' "$r" "$(hp "$w" "$r")" "$(dead "$w" "$r" && echo '(dead)')"; done; }
# snap <who> <refs...>: "ref health" lines; dropped <who> <snap>: the refs of the snap whose health fell since, or that died
snap() { local w=$1 r; shift; for r in "$@"; do echo "$r $(hp "$w" "$r")"; done; }
dropped() { local w=$1 r h0; while read -r r h0; do [ -n "$r" ] || continue; if dead "$w" "$r" || lower "$(hp "$w" "$r")" "$h0"; then echo "$r"; fi; done <<<"$2"; }

# 1. the quest running in both games, both players at the marker, armed
for w in host vm; do
	if ! running $w; then
		say "set-up: $w's game is not running $QUEST_EDID: console startquest $QUEST_EDID"
		dc $w "console startquest $QUEST_EDID" >/dev/null; sleep 3
	fi
	running $w || verdict VOID "$w's game would not run $QUEST_EDID ($(dc $w "stages $QUEST"))"
	say "set-up: $w's quest: $(dc $w "stages $QUEST" | cut -c1-120)"
done
for w in host vm; do
	say "set-up: $w goes to the reinforcements' marker (console player.moveto $MARKER)"
	dc $w focus >/dev/null; dc $w "console player.moveto $MARKER" >/dev/null; sleep 12; wait_ingame $w 90 || verdict VOID "$w is not in the game after the move"
	dc $w "console player.additem 00004822 1" >/dev/null; dc $w "console player.additem 0001F276 300" >/dev/null
	dc $w "console player.equipitem 00004822" >/dev/null; dc $w "draw on" >/dev/null
done
MXY=$(xy_of host "$MARKER"); [ -n "$(echo $MXY | tr -d ' ')" ] || verdict VOID "the host's game has no marker $MARKER"
say "set-up: marker $MARKER at $MXY; both players have a 10mm and 300 rounds (console additem), drawn"
sleep 5   # the marker's cell and its neighbours loaded
MH=$(members host | tr '\n' ' '); MV=$(members vm | tr '\n' ' ')
say "set-up: the collection's members (switched off at the marker): host's [$MH], VM's [$MV]; leaders host $(aref host $LEADER_ALIAS) vm $(aref vm $LEADER_ALIAS) (left off)"
[ "$(wc -w <<<"$MH")" -ge 2 ] && [ "$(wc -w <<<"$MV")" -ge 2 ] || verdict VOID "fewer than two members found in a game (host [$MH], VM [$MV]): is the quest fresh in both (start from fresh games)?"
# both walk about AWAY from the marker, the VM 250 u beside the host
mx=${MXY% *}; my=${MXY#* }; placed=""
for dir in "1 0" "0 1" "-1 0" "0 -1"; do
	dx=${dir% *}; dy=${dir#* }
	tx=$(awk -v m="$mx" -v d="$dx" -v a="$AWAY" 'BEGIN{printf "%d", m + d * a}'); ty=$(awk -v m="$my" -v d="$dy" -v a="$AWAY" 'BEGIN{printf "%d", m + d * a}')
	rwalk host "$tx" "$ty" 150 || true
	d=$(dist_xy "$(pos_of host)" "$MXY"); [ -n "$d" ] && [ "$d" -ge $((AWAY * 6 / 10)) ] && { placed="$tx $ty"; break; }
	say "set-up: the host's walk toward ($tx, $ty) ended $d u from the marker; another way"
done
[ -n "$placed" ] || verdict VOID "the host could not walk $AWAY u from the marker any way"
rwalk vm $(awk -v p="$(pos_of host)" -v dx="$dx" -v dy="$dy" 'BEGIN{split(p,q," "); printf "%d %d", q[1] + dy * 250, q[2] + dx * 250}') 150 || true
say "set-up: host $(dist_xy "$(pos_of host)" "$MXY") u from the marker, vm $(dist_xy "$(pos_of vm)" "$MXY") u (walked, the pad)"
HID=$(field "$(dc host 'net status')" id); VID=$(field "$(dc vm 'net status')" id)
say "set-up: player ids: host $HID, vm $VID; questActors before: host [$(qa_of host)] vm [$(qa_of vm)]"
rmark host run; rmark vm run

# 2. the host's members on first, then the VM's once the host's have left the marker
say "the host's members switched on (console enable, standing for the quest's script): $MH"
for r in $MH; do dc host "console $r.enable" >/dev/null; done
_up() { local w=$1 r i; shift; for r in "$@"; do i=$(info "$w" "$r"); [ "$(field "$i" 3d)" = true ] && [ "$(field "$i" disabled)" = false ] || return 1; done; }
scene_live "the host's members stand loaded and switched on" 30 _up host $MH
_left() { local r d; for r in $MH; do dead host "$r" && continue; d=$(dist_xy "$(xy_of host "$r")" "$MXY"); [ -n "$d" ] && [ "$d" -lt 300 ] && return 1; done; return 0; }
wait_for 40 _left && say "the host's members have left the marker" || say "the host's members are still near the marker after 40 s (going on)"
say "the VM's members switched on: $MV"
for r in $MV; do dc vm "console $r.enable" >/dev/null; done
scene_live "the VM's members stand loaded and switched on" 30 _up vm $MV
sleep 15   # shared or held, copies placed or discarded, pairs made (once a second)
say "questActors: host [$(qa_of host)] vm [$(qa_of vm)]"
SH=""; for r in $MV; do SH="$SH $r:owner=$(owner_of vm "$r")"; done
say "the VM's members as the session has them (owner $HID = paired with the host's, $VID = shared by the VM):$SH"
CH=$(copies host "$VID" | tr '\n' ' '); CV=$(copies vm "$HID" | grep -vxF -f <(printf '%s\n' $MV) | tr '\n' ' ')
say "the host's copies of the VM's raiders: [$CH]; the VM's copies of the host's raiders (not its own members): [$CV]"
rsince host run | grep -E "Spawns: .*(player $VID's|alias 16|twin|stands where)" | tail -12 | sed 's/^/  host| /' | tee -a "$REPRO_OUT/run.log"
rsince vm run | grep -E "Spawns: .*(alias 16|twin|is not shared|player $HID's)" | tail -12 | sed 's/^/  vm| /' | tee -a "$REPRO_OUT/run.log"
BAD=""
[ -n "$(echo $CH)" ] && BAD="$BAD the host's game holds copies of the VM's members [$CH] beside its own [$MH] (doubled);"
[ -n "$(echo $CV)" ] && BAD="$BAD the VM's game holds copies of the host's members [$CV] beside its own [$MV] (doubled);"
PAIRED=$(for r in $MV; do [ "$(owner_of vm "$r")" = "$HID" ] && echo "$r"; done | tr '\n' ' ')
[ -n "$BAD" ] && { rkeep host quest-collection-raiders; rkeep vm quest-collection-raiders; verdict YES "a quest collection's members are each game's own, shared beside the friend's:$BAD"; }
[ -n "$(echo $PAIRED)" ] || verdict YES "no copies, but none of the VM's members [$MV] is paired with a host's (owners:$SH)"

# fire_until <shooter> <target> <hp before>: real clicks until one lands, up to 12; LANDED = the shot that did, empty if none. The
# shooter walks (the pad) to within 250 u first and again after every 4 that missed: the raiders keep moving on the hillside, and
# all 10 of a first fix run's shots from 350 u missed (2026-11-22)
fire_until() {
	local s=$1 t=$2 h0=$3 i h; LANDED=""
	for i in $(seq 1 12); do
		[ $((i % 4)) -eq 1 ] && { rwalk $s "$t" 250 || true; }
		dc $s "turnto $t 300 aim" >/dev/null; sleep 0.5; dc $s "click 80" >/dev/null; sleep 1.2
		h=$(hp $s "$t"); if dead $s "$t" || lower "$h" "$h0"; then LANDED=$i; return 0; fi
	done
	return 1
}

# 3. the VM shoots one of its paired members: one of the host's members must take it; then the host shoots one of its own: one of
# the VM's members must take it
DONE=0
V1=""; for r in $PAIRED; do dead vm "$r" || { V1=$r; break; }; done
[ -n "$V1" ] || verdict VOID "none of the VM's paired members is alive"
ALIVE_H=$(for r in $MH; do dead host "$r" || echo "$r"; done | tr '\n' ' ')
rmark host vm; rmark vm vm
H0=$(hp vm "$V1"); S1=$(snap host $ALIVE_H)
say "[vm shoots] its member $V1 (paired) at $H0 health; the host's members: $(hps host $ALIVE_H)"
step_aside host "$V1" 400 || true
n0=$(shots_of vm); fire_until vm "$V1" "$H0"; n1=$(shots_of vm)
[ -n "$n0" ] && [ -n "$n1" ] && [ "$n1" -gt "$n0" ] || verdict VOID "the VM's clicks fired nothing (shots=$n0 before, $n1 after)"
[ -n "$LANDED" ] || verdict VOID "none of the VM's 12 shots at $V1 landed (shots=$n0 -> $n1)"
sleep 5
say "[vm shoots] shot $LANDED landed: the VM's $V1 $H0 -> $(hp vm "$V1"); the host's members now: $(hps host $ALIVE_H)"
rsince vm vm | grep -E "CopyWatch: hit|damage on|Spawns: .*$V1" | tail -6 | sed 's/^/  vm| /' | tee -a "$REPRO_OUT/run.log"
rsince host vm | grep -E "CopyWatch|damage on|health" | tail -6 | sed 's/^/  host| /' | tee -a "$REPRO_OUT/run.log"
if D=$(dropped host "$S1" | tr '\n' ' '); [ -n "$(echo $D)" ]; then
	say "[vm shoots] the hit reached the host's game (its members that took it: $D)"; DONE=$((DONE + 1))
else
	BAD="$BAD the VM's landed shot on its paired member $V1 left every one of the host's members as it was;"
fi

ALIVE_H=$(for r in $MH; do dead host "$r" || echo "$r"; done | tr '\n' ' ')
ALIVE_V=$(for r in $PAIRED; do dead vm "$r" || echo "$r"; done | tr '\n' ' ')
[ -n "$(echo $ALIVE_H)" ] && [ -n "$(echo $ALIVE_V)" ] || verdict VOID "no live member left for the host's turn (host [$ALIVE_H], VM paired [$ALIVE_V])"
H2=$(nearest host "$(pos_of host)" $ALIVE_H)
rmark host host; rmark vm host
H20=$(hp host "$H2"); S2=$(snap vm $ALIVE_V)
say "[host shoots] its own member $H2 at $H20 health; the VM's paired members: $(hps vm $ALIVE_V)"
step_aside vm "$H2" 400 || true
n0=$(shots_of host); fire_until host "$H2" "$H20"; n1=$(shots_of host)
[ -n "$n0" ] && [ -n "$n1" ] && [ "$n1" -gt "$n0" ] || verdict VOID "the host's clicks fired nothing (shots=$n0 before, $n1 after)"
[ -n "$LANDED" ] || verdict VOID "none of the host's 12 shots at $H2 landed (shots=$n0 -> $n1)"
sleep 5
say "[host shoots] shot $LANDED landed: the host's $H2 $H20 -> $(hp host "$H2"); the VM's paired members now: $(hps vm $ALIVE_V)"
rsince host host | grep -E "CopyWatch: hit|damage on|Spawns: .*$H2" | tail -6 | sed 's/^/  host| /' | tee -a "$REPRO_OUT/run.log"
rsince vm host | grep -E "CopyWatch|damage|health" | tail -6 | sed 's/^/  vm| /' | tee -a "$REPRO_OUT/run.log"
if D=$(dropped vm "$S2" | tr '\n' ' '); [ -n "$(echo $D)" ]; then
	say "[host shoots] the hit reached the VM's game (its members that took it: $D)"; DONE=$((DONE + 1))
else
	BAD="$BAD the host's landed shot on its member $H2 left every one of the VM's paired members as it was;"
fi
say "questActors at the end: host [$(qa_of host)] vm [$(qa_of vm)]"
rkeep host quest-collection-raiders; rkeep vm quest-collection-raiders
[ -n "$BAD" ] && verdict YES "a quest collection's paired members did not share a hit:$BAD"
verdict NO "each game's members pair with the friend's (no copies beside them), and each landed shot ($DONE direction(s): vm, host) reached the other game"
