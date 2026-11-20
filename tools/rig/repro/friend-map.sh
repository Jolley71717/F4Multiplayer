#!/bin/bash
# friend-map.sh: each friend on the Pip-Boy's world and local maps (FriendMap), both directions, and nothing of it in a save. A proof
# run, not a bug's repro: REPRO: NO = every check held (the feature works), YES = one failed (what failed is said), VOID = the scene
# could not be reached.
#
# For each way (the host looking at the VM's player, then the VM looking at the host's): the viewer opens the Pip-Boy (a real Tab) and
# goes to the map page (real keys, PIPKEYS); `friendmap` (dev) must say the world map is up, its fit from the page's own markers came
# out within 2 px, and the friend is drawn, at their state's spot. Then the friend walks (real walk) while the viewer's map stays
# open: the mark must move the way they walked (east is right, north is up on the map). Then the local map (real key, LOCALKEY): the
# friend beside us is drawn on it too. A screenshot of each open map is kept (judging the marker by eye; nothing is steered by it).
# Interiors (console placement, said so: both players coc'd into INTERIOR): on the local map the friend in our interior is drawn,
# and on the world map the viewer sees the friend at that interior's way out, faded (inside=1), as the game shows the player.
# Leaving: with the host's map open the VM leaves the session (`net leave`): the host's mark must go. Last, a save is made on the host
# with its map open and the friend drawn (console save, the Pip-Boy up), the session ended, and the save loaded solo: `saveguard
# strays` must be empty, `friendmap` must draw nothing, and the save file must not hold the friend's name.
# Keys: PIPKEYS (the keys that bring the Pip-Boy from the page it opens on to the map, tried in turn until `friendmap` says map=),
# LOCALKEY (the map page's world/local toggle). Run from tools/rig/repro with both games joined (rig.sh join).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
PIPKEYS=${PIPKEYS:-0x45}; LOCALKEY=${LOCALKEY:-0x51}; INTERIOR=${INTERIOR:-MuseumOfFreedom01}; SAVE=${SAVE:-f4mpfriendmap}
FAILS=0; NOTES=""
fail() { FAILS=$((FAILS + 1)); NOTES="$NOTES; $*"; say "FAIL: $*"; }
pass() { say "ok: $*"; }
fm() { dc "$1" friendmap; }
# shot <who> <name>: a screenshot of that game's screen into $REPRO_OUT (the VM's by its own capture task)
shot() {
	if [ "$1" = host ]; then powershell -NoProfile -ExecutionPolicy Bypass -File "$REPO/tools/screenshot.ps1" -OutFile "$(cygpath -w "$REPO_OUT_ABS/$2.png")" -Width 1600 >/dev/null 2>&1
	else
		vmssh 'Remove-Item C:\F4MP\shot.png -ErrorAction SilentlyContinue' >/dev/null
		vmssh "& C:\\F4MP\\in-vm-run.ps1 -Name F4MP-Shot -Exe 'C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe' -Arguments '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File C:\\F4MP\\vmshot.ps1'" >/dev/null
		wait_for 20 _vmshot_ready && scp -q -i "$VM_KEY" -o LogLevel=ERROR "$VM_SSH:C:/F4MP/shot.png" "$REPRO_OUT/$2.png" 2>/dev/null
	fi
	[ -f "$REPRO_OUT/$2.png" ] && say "screenshot: $REPRO_OUT/$2.png"
}
_vmshot_ready() { vmssh 'Test-Path C:\F4MP\shot.png' | grep -q True; }
REPO_OUT_ABS=$REPRO_OUT
pip_open() { menu_open "$1" PipboyMenu; }
pip_shut() { ! menu_open "$1" PipboyMenu; }
# open_map <who>: the Pip-Boy opened by a real Tab and brought to the map page by real keys; 0 = `friendmap` says a map is up
open_map() {
	local who=$1 k i
	pip_open "$who" || { rkey "$who" "$VK_TAB" 80; wait_for 8 pip_open "$who" || return 1; sleep 2; }
	for i in 1 2 3 4 5 6; do
		case "$(field "$(fm "$who")" map)" in world|local) return 0;; esac
		for k in $PIPKEYS; do dc "$who" "key $k 80" >/dev/null; sleep 1.5; done
	done
	case "$(field "$(fm "$who")" map)" in world|local) return 0;; esac
	return 1
}
# want_map <who> world|local: the map page's toggle pressed until that map is up
want_map() {
	local i
	for i in 1 2 3; do [ "$(field "$(fm "$1")" map)" = "$2" ] && return 0; dc "$1" "key $LOCALKEY 80" >/dev/null; sleep 2; done
	[ "$(field "$(fm "$1")" map)" = "$2" ]
}
close_map() { pip_open "$1" && { rkey "$1" "$VK_TAB" 80; wait_for 8 pip_shut "$1"; sleep 1; }; return 0; }
# px <probe> <w|l>: "x,y" where the one friend was drawn on that map, or empty
px() { grep -oE "(^| )$2px[0-9]+=[-0-9.]+,[-0-9.]+" <<<"$1" | head -1 | cut -d= -f2; }
wpos() { grep -oE "(^| )wpos[0-9]+=[-0-9]+,[-0-9]+" <<<"$1" | head -1 | cut -d= -f2; }
inside() { grep -oE "(^| )inside[0-9]+=[a-z]+" <<<"$1" | head -1 | cut -d= -f2; }
le2() { awk -v w="$1" 'BEGIN { exit !(w != "" && w + 0 <= 2.0) }'; }

need_session
say "host: $(dc host 'net status' | grep -oE 'status=[a-z]+ id=[0-9]+')  vm: $(dc vm 'net status' | grep -oE 'status=[a-z]+ id=[0-9]+')"

# ---- each way, outside -----------------------------------------------------------------------------------------------------
for viewer in vm host; do
	friend=$(other_game $viewer)
	close_map $viewer; close_map $friend
	# the friend a little way off, so their mark is not under our own arrow
	read -r fx fy <<<"$(pos_of $friend)"; read -r vx vy <<<"$(pos_of $viewer)"
	say "[$viewer looks] viewer at $vx,$vy; friend at $fx,$fy ($(dist_xy "$vx $vy" "$fx $fy") u apart)"
	if ! open_map $viewer; then fail "$viewer: no map page after a Tab and the keys $PIPKEYS ($(fm $viewer | cut -c1-120); menus $(menus $viewer))"; close_map $viewer; continue; fi
	want_map $viewer world || fail "$viewer: the world map would not come up with $LOCALKEY ($(fm $viewer | cut -c1-80))"
	sleep 1.5; p=$(fm $viewer); say "$viewer world: $p"
	[ "$(field "$p" wfitted)" = true ] && le2 "$(field "$p" wworst)" && pass "$viewer: the world map's fit from $(field "$p" wused) of the page's markers, worst $(field "$p" wworst) px" || fail "$viewer: world map fit fitted=$(field "$p" wfitted) worst=$(field "$p" wworst)"
	[ "$(field "$p" wshown)" = 1 ] && [ -n "$(px "$p" w)" ] && pass "$viewer: the friend is drawn on the world map at $(px "$p" w)" || fail "$viewer: the friend is not drawn on the world map (wshown=$(field "$p" wshown))"
	shot $viewer "world-$viewer"
	# the friend's spot as the mark has it, against where the friend's game says they are
	read -r gx gy <<<"$(pos_of $friend)"; w=$(wpos "$p")
	d=$(dist_xy "${w%,*} ${w#*,}" "$gx $gy"); [ -n "$d" ] && [ "$d" -le 300 ] && pass "$viewer: the mark is at the friend's spot (${d} u from where their game has them)" || fail "$viewer: the mark's spot $w is ${d:-?} u from the friend's $gx,$gy"
	# the friend walks while the map stays open: the mark follows
	before=$(px "$p" w); tx=$((gx + 900)); ty=$((gy + 900))
	rwalk $friend $tx $ty 150; say "$friend walked toward $tx,$ty (north-east): $WALKED"
	sleep 2; p2=$(fm $viewer); after=$(px "$p2" w); read -r hx hy <<<"$(pos_of $friend)"
	say "$viewer world after the walk: mark $before -> $after; friend $gx,$gy -> $hx,$hy"
	awk -v b="$before" -v a="$after" -v dx=$((hx - gx)) -v dy=$((hy - gy)) 'BEGIN { split(b, p, ","); split(a, q, ","); mx = q[1] - p[1]; my = q[2] - p[2];
		if (b == "" || a == "" || (dx * dx + dy * dy) < 200 * 200) exit 1;
		# east (+x) is right (+px), north (+y) is up (-px)
		exit !((dx > 100 ? mx > 0.2 : dx < -100 ? mx < -0.2 : 1) && (dy > 100 ? my < -0.2 : dy < -100 ? my > 0.2 : 1)) }' \
		&& pass "$viewer: the mark moved with the friend while the map was open" || fail "$viewer: the mark did not follow the walk ($before -> $after for a move of $((hx - gx)),$((hy - gy)))"
	# the local map: the friend beside us is on it
	rwalk $friend "$vx" "$vy" 200 >/dev/null; sleep 1
	if want_map $viewer local; then
		sleep 1.5; p=$(fm $viewer); say "$viewer local: $p"
		[ "$(field "$p" lfitted)" = true ] && le2 "$(field "$p" lworst)" && [ "$(field "$p" lshown)" = 1 ] && pass "$viewer: the friend is drawn on the local map at $(px "$p" l) (fit worst $(field "$p" lworst) px, fallback $(field "$p" lfallback))" \
			|| fail "$viewer: local map lfitted=$(field "$p" lfitted) lworst=$(field "$p" lworst) lshown=$(field "$p" lshown)"
		shot $viewer "local-$viewer"
		want_map $viewer world >/dev/null
	else fail "$viewer: the local map would not come up with $LOCALKEY"; fi
	close_map $viewer
done

# ---- interiors (console placement: both players coc'd into $INTERIOR) --------------------------------------------------------
for who in vm host; do dc $who "console coc $INTERIOR" >/dev/null; done
say "both players coc'd into $INTERIOR (console placement, to reach an interior)"
sleep 15; wait_ingame host 60; wait_ingame vm 60; sleep 8
for viewer in host vm; do
	open_map $viewer || { fail "$viewer: no map page inside"; continue; }
	if want_map $viewer local; then
		sleep 1.5; p=$(fm $viewer); say "$viewer local inside: $p"
		[ "$(field "$p" lshown)" = 1 ] && pass "$viewer: the friend in our interior is drawn on the local map at $(px "$p" l)" || fail "$viewer: the friend in our interior is not on the local map (lshown=$(field "$p" lshown))"
		shot $viewer "local-inside-$viewer"
	else fail "$viewer: no local map inside"; fi
	if want_map $viewer world; then
		sleep 1.5; p=$(fm $viewer); say "$viewer world inside: $p"
		[ "$(field "$p" wshown)" = 1 ] && [ "$(inside "$p")" = true ] && pass "$viewer: the friend inside is drawn on the world map at the way out, $(wpos "$p")" || fail "$viewer: the friend inside is not on the world map (wshown=$(field "$p" wshown) inside=$(inside "$p"))"
		shot $viewer "world-inside-$viewer"
	else fail "$viewer: no world map inside"; fi
	close_map $viewer
done
# the friend steps outside again (console placement back to Sanctuary), the viewer stays in: the world map shows the friend's own spot
dc vm "console coc SanctuaryExt" >/dev/null; say "vm coc'd back to SanctuaryExt (console placement)"; sleep 15; wait_ingame vm 60; sleep 5
if open_map host && want_map host world; then
	sleep 1.5; p=$(fm host); say "host world, host inside and vm outside: $p"
	read -r gx gy <<<"$(pos_of vm)"; w=$(wpos "$p"); d=$(dist_xy "${w%,*} ${w#*,}" "$gx $gy")
	[ "$(field "$p" wshown)" = 1 ] && [ "$(inside "$p")" = false ] && [ -n "$d" ] && [ "$d" -le 300 ] && pass "host inside: the friend outside is drawn at their own spot ($d u off)" || fail "host inside: friend outside wshown=$(field "$p" wshown) inside=$(inside "$p") off=${d:-?}"
	[ "$(field "$p" lshown)" = 1 ] && fail "host inside: the friend outside is still on our interior's local map" || true
	shot host "world-host-inside-friend-out"
else fail "host: no world map inside (friend out)"; fi
close_map host
dc host "console coc SanctuaryExt" >/dev/null; say "host coc'd back to SanctuaryExt (console placement)"; sleep 15; wait_ingame host 60; sleep 5

# ---- a save made with the map open, then the friend leaves, then the save loaded solo ------------------------------------------
FRIEND_NAME=$(dc host 'net status' | grep -oE 'players: \[[^]]*\]' | head -1)
open_map host && want_map host world; sleep 1.5; p=$(fm host)
[ "$(field "$p" wshown)" = 1 ] || fail "host: the friend is not drawn before the save ($(cut -c1-120 <<<"$p"))"
dc host "console save $SAVE" >/dev/null; sleep 8
say "host saved '$SAVE' with its map open and the friend drawn: $(cut -c1-90 <<<"$p")"
# leaving: the host's map still open, the VM leaves the session
dc vm "net leave" >/dev/null; sleep 6; p=$(fm host)
[ "$(field "$p" wshown)" = 0 ] && pass "the friend left: their mark is gone from the host's open map" || fail "the friend left but the host's map still draws $(field "$p" wshown)"
shot host "world-host-friend-left"
close_map host
# solo: the session ended on the host too, the save loaded
dc host "net leave" >/dev/null; sleep 4
dc host "console load $SAVE" >/dev/null; sleep 20; wait_ingame host 90; sleep 8
strays=$(dc host "saveguard strays"); say "host after the solo load: saveguard strays: $(cut -c1-200 <<<"$strays")"
grep -qiE '(^|[^0-9])0 strays|strays=0|none' <<<"$strays" && pass "the save loads solo with no strays" || fail "strays after the solo load: $(cut -c1-160 <<<"$strays")"
open_map host; sleep 1.5; p=$(fm host); say "host map solo: $(cut -c1-160 <<<"$p")"
[ "$(field "$p" shown)" = 0 ] && pass "nothing drawn on the solo map" || fail "the solo map draws $(field "$p" shown)"
shot host "world-host-solo"
close_map host
save_file=$(ls -t "$HOME/Documents/My Games/Fallout4/Saves/"*"$SAVE"*.fos 2>/dev/null | head -1)
name=$(sed -E 's/.*\[ *//; s/\].*//' <<<"$FRIEND_NAME" | tr ',' '\n' | grep -v "^ *$" | tail -1 | sed -E 's/^[0-9]+[:=]//; s/ *\(.*//; s/^ +//; s/ +$//')
say "the save: ${save_file:-not found}"
if [ -n "$save_file" ] && [ -n "$name" ] && [ ${#name} -ge 3 ]; then
	grep -qaF "$name" "$save_file" && fail "the save holds the friend's name" || pass "the save does not hold the friend's name"
	mkdir -p "$REPRO_OUT/saves"; say "the save stays where the game put it: move it aside after the run"
else say "the save file or the friend's name was not found: the name check was not made"; fi

if [ "$FAILS" = 0 ]; then verdict NO "every friend-map check held both ways (world, local, moving, interiors, leaving, a save with the map open)"; fi
verdict YES "$FAILS check(s) failed:${NOTES#;}"
