#!/bin/bash
# turret-hits.sh: a Nexus report (0.20.4): "I can see the host getting pummeled by two turrets, yet they claim they are not taking
# damage." An enemy turret is a plugin NPC one game runs (NpcSync); in that game it fires at the other player's stand-in, and each
# hit is to reach that player as damage (kPlayerHit -> the server -> kPlayerDamaged -> Session::ApplyDamageToPlayer). Their own
# copy of the turret has its AI off and fires nothing, so without the forwarded hits the turret can't hurt them at all.
#
# Both players coc'd into INTERIOR (console placement, said so); the first of TURRETS alive in both games is the scene. Each player
# is put in front of it by dev `setpos` (console placement, said so): the victim 160 u before it, the owner 420 u before it, then
# each takes a real step. The victim's god mode is off for the watch (the owner keeps the rig's); for WATCH seconds the owner's game
# is read for the turret's combat target and the victim's for its health and `net status` hits taken. Then the owner coc's away
# (the turret is released and the other game claims it) and back, and the roles swap, so a turret each game runs is tried.
# REPRO: YES = the owner's turret fought the victim's stand-in and the victim took no damage (or only part); NO = the victim lost
# health from the forwarded hits in both roles; VOID = no turret alive in both games, or it never fought the stand-in.
# Run from tools/rig/repro with both games joined (rig.sh join).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
INTERIOR=${INTERIOR:-CorvegaAssemblyPlant01}
TURRETS=${TURRETS:-"000D21E1 000D224B 000D224C 000E721B 000E7531"}   # LvlTurretTripodRaider in Corvega's plant (Fallout4.esm)
WATCH=${WATCH:-20}
FAILS=0; NOTES=""; VALID=0
fail() { FAILS=$((FAILS + 1)); NOTES="$NOTES; $*"; say "FAIL: $*"; }
pass() { say "ok: $*"; }
local_id() { dc "$1" 'net status' | grep -oE 'status=[a-z]+ id=[0-9]+' | grep -oE '[0-9]+$'; }
puppet_of() { dc "$1" 'net status' | grep -oE "\[$2 '[^']*' puppet=[0-9A-F]{8}" | grep -oE '[0-9A-F]{8}$'; }
hits_taken() { dc "$1" 'net status' | grep -oE 'hits: taken=[0-9]+' | grep -oE '[0-9]+$'; }
hp_of() { field "$(dc "$1" 'combat 00000014')" hp; }
alive_here() { local r; r=$(dc "$1" "refinfo $2"); grep -q "3d=true" <<<"$r" && grep -q "dead=false" <<<"$r"; }
owner_of() { field "$(dc "$1" "mirror $2")" owner; }
# in_front <who> <turret> <distance>: that player put on the turret's facing line, that far before it, looking back at it
in_front() {
	local who=$1 t=$2 d=$3 ri x y z rz px py
	ri=$(dc $who "refinfo $t"); x=$(field "$ri" x); y=$(field "$ri" y); z=$(field "$ri" z); rz=$(field "$ri" rz)
	read -r px py <<<"$(awk -v x="$x" -v y="$y" -v rz="$rz" -v d="$d" 'BEGIN { r = rz * 3.14159265 / 180; printf "%d %d", x + d * sin(r), y + d * cos(r) }')"
	dc $who "setpos 00000014 $px $py ${z%.*} $(awk -v rz="$rz" 'BEGIN { printf "%d", (rz + 180) % 360 }')" >/dev/null; sleep 2
	rwalk $who $px $py 40 0.5 >/dev/null; dc $who "turnto $t 300" >/dev/null
	say "$who put $d u in front of $t by setpos (console placement) and took a step ($WALKED); now at $(pos_of $who)"
}
coc_both() { local who; for who in host vm; do dc $who "god on" >/dev/null; dc $who "console coc $1" >/dev/null; done; say "both players coc'd into $1 (console placement)"; sleep 15; wait_ingame host 60; wait_ingame vm 60; sleep 8; }
# watch_round <owner> <victim> <turret>
watch_round() {
	local owner=$1 victim=$2 t=$3 vid oid pup hp0 hp1 h0 h1 i c fought=0 targ ign0 ign1
	vid=$(local_id $victim); oid=$(local_id $owner); pup=$(puppet_of $owner "$vid")
	say "[$owner runs $t, $victim is the target] owner=$(owner_of $owner $t) (ids: $owner=$oid $victim=$vid); the victim's stand-in in the owner's game: ${pup:-none}"
	[ -n "$pup" ] || { fail "$owner has no stand-in of $victim"; return; }
	in_front $owner $t 420; in_front $victim $t 160
	dc $victim "god off" >/dev/null
	hp0=$(hp_of $victim); h0=$(hits_taken $victim); ign0=$(rlog $victim | grep -c "on our stand-in in player .* ignored")
	for i in $(seq 1 $WATCH); do
		c=$(dc $owner "combat $t"); targ=$(field "$c" target)
		[ "$targ" = "$pup" ] && fought=$((fought + 1))
		[ $((i % 4)) = 0 ] && say "  t=${i}s owner: turret $c; victim hp=$(hp_of $victim) hits taken=$(hits_taken $victim); victim's copy: $(dc $victim "combat $t")"
		sleep 1
	done
	dc $victim "god on" >/dev/null
	hp1=$(hp_of $victim); h1=$(hits_taken $victim); ign1=$(rlog $victim | grep -c "on our stand-in in player .* ignored")
	say "  result: the owner's turret targeted the stand-in in $fought of $WATCH reads; victim hp $hp0 -> $hp1, hits taken $h0 -> $h1, ignored-hit log lines +$((ign1 - ign0))"
	say "  owner's count of NPC hits on stand-ins (sent/seen, builds that have it): $(dc $owner 'net status' | grep -oE 'standInHits=[^ ]+ noData=[0-9]+ noDamage=[0-9]+ unshared=[0-9]+'); victim's: $(dc $victim 'net status' | grep -oE 'hits: taken=[0-9]+ unseen=[0-9]+ ignored=[0-9]+ mult=[^ ]+')"
	rsince $victim | grep -E "Session: a hit by" | tail -3 | sed "s/^/  $victim log: /" | tee -a "$REPRO_OUT/run.log"
	rsince host | grep -E "server: a hit" | tail -2 | sed "s/^/  host log (the server): /" | tee -a "$REPRO_OUT/run.log"
	[ "$fought" -ge 3 ] || { say "  the turret did not fight the stand-in (target was $targ): this round proves nothing"; return; }
	VALID=$((VALID + 1))
	if awk -v a="$hp0" -v b="$hp1" 'BEGIN { exit !(a - b >= 5) }'; then pass "$victim lost $(awk -v a="$hp0" -v b="$hp1" 'BEGIN { printf "%.0f", a - b }') health to $owner's turret (hits taken +$((h1 - h0)))"
	else fail "$victim took no damage from $owner's turret firing at its stand-in (hp $hp0 -> $hp1, hits taken +$((h1 - h0)), ignored +$((ign1 - ign0)))"; fi
}
need_session
rmark host; rmark vm
coc_both "$INTERIOR"
T=""
for t in $TURRETS; do
	dc host "console player.moveto $t" >/dev/null; dc vm "console player.moveto $t" >/dev/null; sleep 6   # console placement: the turret loads in both
	if alive_here host $t && alive_here vm $t; then T=$t; break; fi
	say "turret $t: host $(dc host "refinfo $t" | grep -oE '3d=[a-z]+|dead=[a-z]+' | tr '\n' ' ') vm $(dc vm "refinfo $t" | grep -oE '3d=[a-z]+|dead=[a-z]+' | tr '\n' ' ')"
done
[ -n "$T" ] || verdict VOID "no turret of $TURRETS is alive with its 3D in both games"
say "scene: turret $T ($(dc host "refinfo $T" | cut -c1-100)); owner now $(owner_of host $T) (host id $(local_id host), vm id $(local_id vm))"
first=host; second=vm; [ "$(owner_of host $T)" = "$(local_id vm)" ] && { first=vm; second=host; }
watch_round $first $second $T
# the owner goes away: the turret is released and the other game claims it
dc $first "console coc SanctuaryExt" >/dev/null; say "$first coc'd to SanctuaryExt (console placement): the turret is released"; sleep 15; wait_ingame $first 60; sleep 6
dc $second "console player.moveto $T" >/dev/null; sleep 4
say "owner after the release: $(owner_of $second $T) ($second is $(local_id $second))"
dc $first "console coc $INTERIOR" >/dev/null; sleep 15; wait_ingame $first 60; sleep 6; dc $first "console player.moveto $T" >/dev/null; sleep 5
if [ "$(owner_of $second $T)" = "$(local_id $second)" ]; then watch_round $second $first $T; else say "$second did not get the turret (owner $(owner_of $second $T)): the second role is not tried"; fi
for who in host vm; do dc $who "god on" >/dev/null; done
rkeep host end; rkeep vm end
[ "$VALID" = 0 ] && verdict VOID "the turret never fought a stand-in in either role"
[ "$FAILS" = 0 ] && verdict NO "the forwarded turret hits reached the target in $VALID role(s)"
verdict YES "$FAILS of $VALID role(s) failed:${NOTES#;}"
