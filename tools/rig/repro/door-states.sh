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
doorcount() { dc "$1" wsdamage | grep -oE 'doors: known=[0-9]+ sent=[0-9]+ heard=[0-9]+ set=[0-9]+'; }
is_shut() { case "$(dstate "$1" "$2")" in 3|4) return 0;; esac; return 1; }
coc_both() { local who; for who in host vm; do dc $who "god on" >/dev/null; dc $who "console coc $1" >/dev/null; done; say "both players coc'd into $1 (console placement)"; sleep 15; wait_ingame host 60; wait_ingame vm 60; sleep 8; }
# press_door <who> <ref> <open|shut>: E on the door until that game has it that way (up to 3 presses, each only with the door
# under the crosshair: a friend's stand-in in the way takes the E instead, as the first rig run found). 0 = it is that way
press_door() {
	local who=$1 ref=$2 want=$3 i name
	name=$(dc $who "doors 3000" | tr ';' '\n' | grep -o "$ref '[^']*'" | cut -d"'" -f2)
	for i in 1 2 3; do
		dc $who "turnto $ref 300 aim" >/dev/null; sleep 0.8; ROLLOVER=$(rrollover $who)
		if [ -n "$name" ] && [ "${ROLLOVER,,}" != "${name,,}" ]; then
			say "$who: the crosshair names '${ROLLOVER:-nothing}', not the door ('$name'): no press"; dc $who "mouse 0 60" >/dev/null; sleep 0.5; continue
		fi
		rkey $who "$VK_E" 100; say "$who pressed E on $ref (crosshair: '${ROLLOVER:-nothing}')"
		wait_for 4 is_$want $who $ref && return 0
	done
	is_$want $who $ref
}
# one_way <actor> <actor's ref> <other> <other's ref> <open|shut>: the actor walks to its door, the other player walks back behind
# the actor (out of the crosshair), and the actor's E puts the door that way in its own game; the other game's door must follow
# within 6 s. A direction counts only when both doors start the other way: an actor's door already that way (the last change never
# reached its game) is first put back by a press of its own (set-up, said so); when the other game's door is already that way (left
# over from a change that never crossed) the direction proves nothing and is not counted.
one_way() {
	local who=$1 ref=$2 other=$3 oref=$4 want=$5 opp x y ax ay ri rz side sx sy
	[ "$want" = open ] && opp=shut || opp=open
	ri=$(dc $who "refinfo $ref"); x=$(field "$ri" x); y=$(field "$ri" y); rz=$(field "$ri" rz)
	# To a spot 90 u before the door along its facing (the far side if that one can't be reached): from beside its frame
	# the crosshair names nothing (the second run).
	for side in 90 -90; do
		read -r sx sy <<<"$(awk -v x="$x" -v y="$y" -v rz="$rz" -v d="$side" 'BEGIN { r = rz * 3.14159265 / 180; printf "%d %d", x + d * sin(r), y + d * cos(r) }')"
		rwalk $who "$sx" "$sy" 30 0.6; say "$who walked to $side u before the door ($ref at ${x%.*},${y%.*}): $WALKED"
		[ "$(dist_xy "$(pos_of $who)" "$sx $sy")" -le 45 ] 2>/dev/null && break
	done
	read -r ax ay <<<"$(pos_of $who)"
	rwalk $other $(awk -v dx="$x" -v dy="$y" -v ax="$ax" -v ay="$ay" 'BEGIN { vx = ax - dx; vy = ay - dy; l = sqrt(vx * vx + vy * vy); if (l < 1) { vx = 1; vy = 0; l = 1 } printf "%d %d", dx + vx / l * 340, dy + vy / l * 340 }') 70 0.7
	say "$other walked back behind $who, away from the door: $WALKED"
	if is_$want $who $ref; then
		press_door $who $ref $opp; say "set-up: $who's own door was already $want (the last change never reached its game); its own press put it back: open=$(dstate $who $ref)"
	fi
	if is_$want $other $oref; then say "not counted: $other's door $oref is already $want (left over from a change that never crossed): $who $want proves nothing"; NOTCOUNTED=$((NOTCOUNTED + 1)); return 0; fi
	if ! press_door $who $ref $want; then say "not counted: $who's presses never put its own door $ref $want (open=$(dstate $who $ref)): nothing to share"; NOTCOUNTED=$((NOTCOUNTED + 1)); return 1; fi
	pass "$who $want $ref in its own game (open=$(dstate $who $ref))"
	COUNTED=$((COUNTED + 1))
	if wait_for 6 is_$want $other $oref; then pass "$other agrees: $oref open=$(dstate $other $oref)"; else fail "$other does not agree: $oref open=$(dstate $other $oref) 6 s after $who $want $ref"; fi
	say "  built-door counters (builds that have them): host $(doorcount host); vm $(doorcount vm)"
}
COUNTED=0; NOTCOUNTED=0
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
	say "scripts on the door: host $(dc host "scripts $href" | cut -c1-160); vm $(dc vm "scripts $vref" | cut -c1-160)"
	say "the VM's copy of the host's door: $vref ($(dc vm "refinfo $vref" | cut -c1-120)); host door open=$(dstate host $href) VM copy open=$(dstate vm $vref)"
	one_way vm $vref host $href open
	one_way host $href vm $vref shut
	one_way host $href vm $vref open
	one_way vm $vref host $href shut
fi
dc host "wsreport $href $BENCH scrap" >/dev/null; sleep 2; say "the host's door scrapped again ($(dc host "refinfo $href" | cut -c1-80))"
for who in host vm; do dc $who "god off" >/dev/null; done
for who in host vm; do say "$who built doors (builds that share them): $(doorcount $who)"; done
rsince host | grep -E "WorldSync: [0-9A-F]{8} (state|locked|unlocked)|WorkshopSync: built door|refused|doorPoll" | tail -12 | sed 's/^/host log: /' | tee -a "$REPRO_OUT/run.log"
rsince vm | grep -E "WorldSync: [0-9A-F]{8} (state|locked|unlocked)|WorkshopSync: built door|refused" | tail -12 | sed 's/^/vm log: /' | tee -a "$REPRO_OUT/run.log"
rkeep host end; rkeep vm end
[ "$COUNTED" = 0 ] && [ "$FAILS" = 0 ] && verdict VOID "no direction could be tried ($NOTCOUNTED not counted)"
[ "$FAILS" = 0 ] && verdict NO "every door state reached the other game in the $COUNTED direction(s) tried (a plain dungeon door and a settlement-built door, both ways; $NOTCOUNTED not counted)"
verdict YES "$FAILS check(s) failed of $COUNTED direction(s) tried ($NOTCOUNTED not counted):${NOTES#;}"
