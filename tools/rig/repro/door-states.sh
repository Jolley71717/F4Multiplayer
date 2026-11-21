#!/bin/bash
# door-states.sh: a Nexus report (0.20.4): "Door open/close states are not syncing between players." Two kinds of door, each
# opened and closed by a real E press, both directions, the other game's door read within 6 s (dev `doors`: open=1 open, 2 opening,
# 3 closed, 4 closing):
#   1. a plain door of a dungeon (both players coc'd into INTERIOR, console placement, said so): the nearest unlocked closed door;
#   2. a door built in a settlement (back in Sanctuary: the host places DOOR_BASE at its workbench by dev `wsreport place` and
#      `wsfinish`, the engine's own finish, console placement, said so; the friend's copy of it comes as a workshop piece).
# REPRO: YES = a door's state did not reach the other game (which one is said); NO = every direction held; VOID = no door to use.
# Run from tools/rig/repro with both games joined (rig.sh join).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
INTERIOR=${INTERIOR:-CorvegaAssemblyPlant01}; HOME_CELL=${HOME_CELL:-SanctuaryExt}
DOOR_BASE=${DOOR_BASE:-00249E99}   # workshop_BldWoodPDoor01, a wood door of the workshop's STRUCTURES > WOOD > DOORS
BENCH=${BENCH:-000250FE}           # Sanctuary's workbench
FAILS=0; NOTES=""
fail() { FAILS=$((FAILS + 1)); NOTES="$NOTES; $*"; say "FAIL: $*"; }
pass() { say "ok: $*"; }
# dstate <who> <ref>: that game's open state of the door (1..4), or empty when it is not within 3000 u with its 3D
dstate() { dc "$1" "doors 3000" | tr ';' '\n' | grep -o "$2 '[^']*' open=[0-9]" | grep -o '[0-9]$'; }
is_open() { case "$(dstate "$1" "$2")" in 1|2) return 0;; esac; return 1; }
is_shut() { case "$(dstate "$1" "$2")" in 3|4) return 0;; esac; return 1; }
coc_both() { local who; for who in host vm; do dc $who "god on" >/dev/null; dc $who "console coc $1" >/dev/null; done; say "both players coc'd into $1 (console placement)"; sleep 15; wait_ingame host 60; wait_ingame vm 60; sleep 8; }
# one_way <actor> <actor's ref> <other> <other's ref> <open|shut>: the actor walks to its door and presses E until its own game has
# the door that way, then the other game's door must agree within 6 s
one_way() {
	local who=$1 ref=$2 other=$3 oref=$4 want=$5 i x y z
	read -r x y z <<<"$(field "$(dc $who "refinfo $ref")" x) $(field "$(dc $who "refinfo $ref")" y) $(field "$(dc $who "refinfo $ref")" z)"
	rwalk $who "${x%.*}" "${y%.*}" 110 0.7; say "$who walked to the door ($ref at ${x%.*},${y%.*}): $WALKED"
	for i in 1 2 3; do
		rpress_e $who $ref; say "$who pressed E on $ref (crosshair: '${ROLLOVER:-nothing}')"
		wait_for 4 is_$want $who $ref && break
		dc $who "mouse 0 150" >/dev/null; sleep 0.5
	done
	if ! is_$want $who $ref; then fail "$who $want $ref: its own game has open=$(dstate $who $ref) after $i press(es)"; return 1; fi
	pass "$who $want $ref in its own game (open=$(dstate $who $ref))"
	if wait_for 6 is_$want $other $oref; then pass "$other agrees: $oref open=$(dstate $other $oref)"; else fail "$other does not agree: $oref open=$(dstate $other $oref) 6 s after $who $want $ref"; fi
}
need_session
say "host: $(dc host 'net status' | grep -oE 'status=[a-z]+ id=[0-9]+')  vm: $(dc vm 'net status' | grep -oE 'status=[a-z]+ id=[0-9]+')"
rmark host; rmark vm

# ---- 1. a plain dungeon door --------------------------------------------------------------------------------------------------
coc_both "$INTERIOR"
read -r px py <<<"$(pos_of host)"
best=""; bestd=99999
for line in $(dc host "doors 2500" | tr ';' '\n' | tr ' ' '_'); do
	line=${line#_}; ref=${line%%_*}; [[ $ref =~ ^[0-9A-F]{8}$ ]] || continue
	grep -q "open=3 lock=[U-]" <<<"${line//_/ }" || continue
	ri=$(dc host "refinfo $ref"); d=$(dist_xy "$px $py" "$(field "$ri" x | cut -d. -f1) $(field "$ri" y | cut -d. -f1)")
	is_int "$d" && [ "$d" -lt "$bestd" ] && [ -n "$(dstate vm $ref)" ] && { best=$ref; bestd=$d; }
done
[ -n "$best" ] || verdict VOID "no closed, unlocked door within 2500 u of the host in $INTERIOR that the VM has too ($(dc host 'doors 2500' | cut -c1-200))"
say "plain door: $best ($(dc host "doors 2500" | tr ';' '\n' | grep "$best" | head -1 | sed 's/^ *//'), ${bestd} u from the host; VM has open=$(dstate vm $best))"
one_way vm $best host $best open
one_way host $best vm $best shut
one_way host $best vm $best open
one_way vm $best host $best shut

# ---- 2. a door built in a settlement ------------------------------------------------------------------------------------------
coc_both "$HOME_CELL"
out=$(dc host "wsreport place $DOOR_BASE $BENCH"); href=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
if [ -z "$href" ]; then say "the host's door was not placed: $out"; [ $FAILS = 0 ] && verdict VOID "the plain door held, but no settlement door could be placed ($out)"; verdict YES "$FAILS check(s) failed (settlement door not placed: $out):${NOTES#;}"; fi
sleep 2; dc host "wsfinish $href $BENCH" >/dev/null; sleep 2
say "set-up: the host placed a workshop door $href ($DOOR_BASE) at its workbench (console placement: wsreport place + wsfinish): $(dc host "refinfo $href" | cut -c1-120)"
vref=""; for i in $(seq 1 25); do vref=$(ws_copy_of vm "$href"); [ -n "$vref" ] && break; sleep 1; done
if [ -z "$vref" ]; then say "the VM has no copy of the host's door after 25 s: $(dc vm 'wspower copies' | cut -c1-200)"; fail "settlement door: the friend's copy of the host's door"; else
	say "the VM's copy of the host's door: $vref ($(dc vm "refinfo $vref" | cut -c1-120)); host door open=$(dstate host $href) VM copy open=$(dstate vm $vref)"
	one_way vm $vref host $href open
	one_way host $href vm $vref shut
	one_way host $href vm $vref open
	one_way vm $vref host $href shut
fi
dc host "wsreport $href $BENCH scrap" >/dev/null; sleep 2; say "the host's door scrapped again ($(dc host "refinfo $href" | cut -c1-80))"
for who in host vm; do dc $who "god off" >/dev/null; done
rsince host | grep -E "WorldSync: [0-9A-F]{8} (state|locked|unlocked)|refused|doorPoll" | tail -5 | sed 's/^/host log: /' | tee -a "$REPRO_OUT/run.log"
rsince vm | grep -E "WorldSync: [0-9A-F]{8} (state|locked|unlocked)|refused" | tail -5 | sed 's/^/vm log: /' | tee -a "$REPRO_OUT/run.log"
rkeep host end; rkeep vm end
[ "$FAILS" = 0 ] && verdict NO "every door state reached the other game (a plain dungeon door and a settlement-built door, both ways)"
verdict YES "$FAILS check(s) failed:${NOTES#;}"
