#!/bin/bash
# turret-hits.sh: a Nexus report (0.20.4): "I can see the host getting pummeled by two turrets, yet they claim they are not taking
# damage." The reporter was the friend: the turrets fired in the friend's game at the host's stand-in. An enemy turret is a plugin
# NPC one game runs (NpcSync); in that game it fires at the other player's stand-in, and each hit is to reach that player as damage
# (kPlayerHit -> the server -> kPlayerDamaged -> Session::ApplyDamageToPlayer). Their own copy of the turret has its AI off and
# fires nothing, so without the forwarded hits the turret can't hurt them at all.
#
# Scene (console placement throughout, said so): the VM's player goes into INTERIOR first, so the VM's game runs the turrets, and
# the first of TURRETS alive in both games is the scene. The raiders near it are killed from the console first (in the first
# run they downed the VM before the watch began). Each round: the owner's player stands where coc put them (thousands of units
# off, out of the turret's sight; ownership holds within 6000 u), the victim is put 250 u before the turret by dev `setpos` and
# takes a real step, god mode off for the watch. If the turret does not take on the stand-in by itself in WATCH_FREE reads, the
# owner's console starts its combat with the stand-in (`startcombat`, said so). The victim's health and `hits: taken=` say
# whether the hits arrived; the server's drop lines (in the host's log) and, with builds that have them, the owner's
# `standInHits=` say where they went.
#   round 1: the VM's game runs the turret, the host is the victim (the report's case);
#   round 2: the VM leaves, the host's game takes the turret, the VM comes back as the victim;
#   step 3: the session has the turret dead while the VM's game has it alive: the VM away, the host kills it from the console,
#           the VM comes back: is it dead in the VM's game, and if not, does it fire at the host's stand-in, and is that felt?
# REPRO: YES = a turret fought a stand-in and its player took no damage (which round is said); NO = every round's victim lost
# health; VOID = no turret alive in both games, or no round where it fought a stand-in.
# Run from tools/rig/repro with both games joined (rig.sh join).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
INTERIOR=${INTERIOR:-CorvegaAssemblyPlant01}; AWAY=${AWAY:-SanctuaryExt}
TURRETS=${TURRETS:-"000D21E1 000D224B 000D224C 000E721B 000E7531"}   # LvlTurretTripodRaider in Corvega's plant (Fallout4.esm)
WATCH_FREE=${WATCH_FREE:-6}; WATCH=${WATCH:-12}; CLEAR=${CLEAR:-2000}
FAILS=0; NOTES=""; VALID=0
fail() { FAILS=$((FAILS + 1)); NOTES="$NOTES; $*"; say "FAIL: $*"; }
pass() { say "ok: $*"; }
local_id() { dc "$1" 'net status' | grep -oE 'status=[a-z]+ id=[0-9]+' | grep -oE '[0-9]+$'; }
puppet_of() { dc "$1" 'net status' | grep -oE "\[$2 '[^']*' puppet=[0-9A-F]{8}" | grep -oE '[0-9A-F]{8}$'; }
hits_taken() { dc "$1" 'net status' | grep -oE 'hits: taken=[0-9]+' | grep -oE '[0-9]+$'; }
hp_of() { field "$(dc "$1" 'combat 00000014')" hp; }
alive_here() { local r; r=$(dc "$1" "refinfo $2"); grep -q "3d=true" <<<"$r" && grep -q "dead=false" <<<"$r"; }
dead_here() { grep -q "dead=true" <<<"$(dc "$1" "refinfo $2")"; }
owner_of() { field "$(dc "$1" "mirror $2")" owner; }
other() { [ "$1" = host ] && echo vm || echo host; }
coc() { dc $1 "god on" >/dev/null; dc $1 "console coc $2" >/dev/null; sleep 12; wait_ingame $1 60; sleep 6; say "$1 coc'd to $2 (console placement)"; }
# in_front <who> <turret> <distance>: that player put on the turret's facing line, that far before it, looking at it
in_front() {
	local who=$1 t=$2 d=$3 ri x y z rz px py
	ri=$(dc $who "refinfo $t"); x=$(field "$ri" x); y=$(field "$ri" y); z=$(field "$ri" z); rz=$(field "$ri" rz)
	read -r px py <<<"$(awk -v x="$x" -v y="$y" -v rz="$rz" -v d="$d" 'BEGIN { r = rz * 3.14159265 / 180; printf "%d %d", x + d * sin(r), y + d * cos(r) }')"
	dc $who "setpos 00000014 $px $py ${z%.*} $(awk -v rz="$rz" 'BEGIN { printf "%d", (rz + 180) % 360 }')" >/dev/null; sleep 2
	rwalk $who $((px + 20)) $py 15 0.5 >/dev/null; dc $who "turnto $t 300" >/dev/null
	say "$who put $d u in front of $t by setpos (console placement), then a real step ($WALKED); now at $(pos_of $who)"
}
# clear_around <who> <turret>: the living non-turret actors within CLEAR of the turret killed from that game's console
clear_around() {
	local who=$1 t=$2 n=0 line id pups
	pups="$(puppet_of host "$VID") $(puppet_of vm "$HID")"
	for line in $(dc $who "actors $CLEAR" | tr ';' '\n' | tr ' ' '_'); do
		line=${line#_}; id=${line%%_*}; [[ $id =~ ^[0-9A-F]{8}$ ]] || continue
		grep -q "dead=false" <<<"$line" || continue; grep -qi "turret" <<<"$line" && continue
		grep -q "$id" <<<"$pups" && continue
		dc $who "console $id.kill" >/dev/null; n=$((n + 1))
	done
	say "set-up: $n living non-turret actor(s) within $CLEAR u of $who's player killed from $who's console"
}
# round <owner> <victim> <turret> <name>: the owner's turret against the victim's stand-in
round() {
	local owner=$1 victim=$2 t=$3 name=$4 vid pup hp0 hp1 h0 h1 i c targ fought=0 forced=no s0 s1
	vid=$(local_id $victim); pup=$(puppet_of $owner "$vid")
	say "[$name] $owner's game runs $t (owner=$(owner_of host $t) by the host's mirror; host id $(local_id host), vm id $(local_id vm)); $victim's stand-in there: ${pup:-none}"
	[ -n "$pup" ] || { fail "$name: $owner has no stand-in of $victim"; return; }
	in_front $victim $t 250
	say "  $owner's player is $(dist_xy "$(pos_of $owner)" "$(field "$(dc $owner "refinfo $t")" x | cut -d. -f1) $(field "$(dc $owner "refinfo $t")" y | cut -d. -f1)") u from the turret"
	s0=$(dc $owner 'net status' | grep -oE 'standInHits=[0-9]+/[0-9]+')
	dc $victim "god off" >/dev/null
	hp0=$(hp_of $victim); h0=$(hits_taken $victim)
	for i in $(seq 1 $((WATCH_FREE + WATCH))); do
		if [ $i = $((WATCH_FREE + 1)) ] && [ $fought = 0 ]; then
			dc $owner "console $t.startcombat $pup" >/dev/null; forced=yes
			say "  the turret took on nothing of $victim's in $WATCH_FREE reads (target $targ): its combat with the stand-in started from $owner's console (startcombat)"
		fi
		c=$(dc $owner "combat $t"); targ=$(field "$c" target)
		[ "$targ" = "$pup" ] && fought=$((fought + 1))
		[ $((i % 3)) = 0 ] && say "  read $i: $owner's turret $c; $victim hp=$(hp_of $victim) hits taken=$(hits_taken $victim)"
	done
	dc $victim "god on" >/dev/null
	hp1=$(hp_of $victim); h1=$(hits_taken $victim); s1=$(dc $owner 'net status' | grep -oE 'standInHits=[0-9]+/[0-9]+ noData=[0-9]+ noDamage=[0-9]+ unshared=[0-9]+')
	say "  result: the turret targeted the stand-in in $fought reads (combat started from the console: $forced); $victim hp $hp0 -> $hp1, hits taken $h0 -> $h1; $owner's NPC hits on stand-ins (builds that count them): ${s0:-none} -> ${s1:-none}"
	rsince $victim | grep -E "Session: a hit by" | tail -3 | sed "s/^/  $victim log: /" | tee -a "$REPRO_OUT/run.log"
	rsince host | grep -E "server: a hit" | tail -3 | sed "s/^/  host log (the server): /" | tee -a "$REPRO_OUT/run.log"
	[ "$fought" -ge 2 ] || { say "  [$name] the turret never fought the stand-in: proves nothing"; return; }
	VALID=$((VALID + 1))
	if awk -v a="$hp0" -v b="$hp1" 'BEGIN { exit !(a - b >= 5) }' || [ "$((h1 - h0))" -ge 3 ] 2>/dev/null; then pass "$name: $victim lost $(awk -v a="$hp0" -v b="$hp1" 'BEGIN { printf "%.0f", a - b }') health to $owner's turret (hits taken +$((h1 - h0)))"
	else fail "$name: $victim took no damage from $owner's turret firing at its stand-in (hp $hp0 -> $hp1, hits taken +$((h1 - h0)), combat from the console: $forced)"; fi
}
wait_owner() {  # wait_owner <id> <turret> <seconds>
	local i; for i in $(seq 1 $(($3 / 3))); do [ "$(owner_of host $2)" = "$1" ] && return 0; sleep 3; done; return 1
}

need_session
HID=$(local_id host); VID=$(local_id vm)
rmark host; rmark vm
# ---- set-up: the VM's game first in the plant, so it runs the turrets
coc vm "$INTERIOR"; read -r VAX VAY <<<"$(pos_of vm)"; VAZ=$(field "$(dc vm pos)" z)
T=""
for t in $TURRETS; do
	dc vm "console player.moveto $t" >/dev/null; sleep 5   # console placement: the turret loads and the VM's game claims it
	alive_here vm $t && { T=$t; break; }
	say "turret $t in the VM's game: $(dc vm "refinfo $t" | grep -oE '3d=[a-z]+|dead=[a-z]+' | tr '\n' ' ')"
done
[ -n "$T" ] || verdict VOID "no turret of $TURRETS is alive with its 3D in the VM's game"
clear_around vm $T
dc vm "setpos 00000014 ${VAX%.*} ${VAY%.*} ${VAZ%.*} 0" >/dev/null; sleep 3; say "vm put back where coc put it (console placement): $(pos_of vm)"
coc host "$INTERIOR"
alive_here host $T || verdict VOID "turret $T is not alive with its 3D in the host's game ($(dc host "refinfo $T" | cut -c1-120))"
say "scene: turret $T ($(dc host "refinfo $T" | cut -c1-110))"
# ---- round 1: the VM's game runs it, the host is the victim
if wait_owner "$VID" $T 15; then round vm host $T "round 1, the friend's turret"; else say "round 1 not tried: the turret's owner is $(owner_of host $T), not the VM ($VID)"; fi
# ---- round 2: the host's game takes it, the VM is the victim
coc vm "$AWAY"
dc host "setpos 00000014 ${VAX%.*} ${VAY%.*} ${VAZ%.*} 0" >/dev/null; sleep 2; say "host put where coc put the players (console placement), out of the turret's sight"
if wait_owner "$HID" $T 45; then
	coc vm "$INTERIOR"; round host vm $T "round 2, the host's turret"
else say "round 2 not tried: the host's game did not take $T within 45 s (owner $(owner_of host $T))"; coc vm "$INTERIOR"; fi
# ---- step 3: dead for the session, alive in the VM's game?
coc vm "$AWAY"
dc host "console player.moveto $T" >/dev/null; sleep 3; dc host "console $T.kill" >/dev/null; sleep 4
say "step 3: the VM away, the host killed $T from its console (dead in the host's game: $(dead_here host $T && echo yes || echo no))"
dc host "setpos 00000014 ${VAX%.*} ${VAY%.*} ${VAZ%.*} 0" >/dev/null
coc vm "$INTERIOR"; dc vm "console player.moveto $T" >/dev/null; sleep 2
a0=$(dc vm "combat $T"); sleep 12; a1=$(dc vm "combat $T")
say "step 3: $T in the VM's game on its return: $a0; 12 s later: $a1"
rsince vm | grep -E "WorldSync: (killed $T|$T )" | tail -3 | sed 's/^/  vm log: /' | tee -a "$REPRO_OUT/run.log"
if grep -q "dead=false" <<<"$a1"; then fail "step 3: $T is dead for the session and alive in the VM's game 12 s after its return ($a1)"; else pass "step 3: $T is dead in the VM's game too"; fi
for who in host vm; do dc $who "god on" >/dev/null; done
rkeep host end; rkeep vm end
[ "$VALID" = 0 ] && [ "$FAILS" = 0 ] && verdict VOID "the turret never fought a stand-in in either round"
[ "$FAILS" = 0 ] && verdict NO "the forwarded turret hits reached the target in $VALID round(s), and a turret dead for the session died in the returning game"
verdict YES "$FAILS check(s) failed ($VALID round(s) with the turret fighting a stand-in):${NOTES#;}"
