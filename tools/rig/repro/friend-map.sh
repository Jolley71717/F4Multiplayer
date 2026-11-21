#!/bin/bash
# friend-map.sh: each friend on the Pip-Boy's world and local maps (FriendMap), both directions, and nothing of it in a save. A proof
# run, not a bug's repro: REPRO: NO = every check held (the feature works), YES = one failed (what failed is said), VOID = the scene
# could not be reached.
#
# For each way (the host looking at the VM's player, then the VM looking at the host's): the viewer opens the Pip-Boy (a real Tab) and
# goes to the map page (real keys: MAPKEY); `friendmap` (dev) must say the world map is up, its fit from the page's own markers came
# out within 2 px, and the friend is drawn, at their state's spot. Then the friend walks (real walk) while the viewer's map stays
# open: the mark must move the way they walked (east is right, north is up on the map). Then the local map (real key, LOCALKEY): the
# friend beside us is drawn on it too. A screenshot of each open map is kept (judging the marker by eye; nothing is steered by it).
# Interiors (console placement, said so: both players coc'd into INTERIOR): on the local map the friend in our interior is drawn,
# and on the world map the viewer sees the friend at that interior's way out (inside=true), our own way out by `friendmap doors`,
# where the game shows the player. Leaving: with the host's map open the VM leaves the session (`net leave`): the host's mark must go.
# Saves: each game saves by a real F5 the moment its Pip-Boy shuts on the map with the friend drawn (the game makes no save with the
# Pip-Boy up); the VM's loaded solo, the host's in its own empty session: `saveguard strays` empty, `friendmap` drawing nothing, and
# the host's file not holding the friend's name.
# Keys: MAPKEY (the game's QuickMap key, M: the Pip-Boy opens on its map; a pad's shoulder buttons did not turn its pages on the rig),
# LOCALKEY (the map page's world/local toggle, R). Run from tools/rig/repro with both games joined (rig.sh join).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
MAPKEY=${MAPKEY:-0x4D}; LOCALKEY=${LOCALKEY:-0x52}; INTERIOR=${INTERIOR:-CorvegaAssemblyPlant01}; SAVE=${SAVE:-f4mpfriendmap}
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
# open_map <who>: the Pip-Boy opened on its map by the game's own map key (QuickMap, M), a Pip-Boy on another page shut first by a
# real Tab; 0 = `friendmap` says a map is up
_on_map() { case "$(field "$(fm "$1")" map)" in world|local) pip_open "$1";; *) return 1;; esac; }
open_map() {
	local who=$1 i
	for i in 1 2 3; do
		pip_open "$who" && _on_map "$who" && return 0
		pip_open "$who" && { rkey "$who" "$VK_TAB" 80; wait_for 8 pip_shut "$who"; sleep 1; }
		rkey "$who" "$MAPKEY" 80; wait_for 8 pip_open "$who" && wait_for 6 _on_map "$who" && { sleep 1; return 0; }
	done
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
	if ! open_map $viewer; then fail "$viewer: no map page after the map key $MAPKEY ($(fm $viewer | cut -c1-120); menus $(menus $viewer))"; close_map $viewer; continue; fi
	want_map $viewer world || fail "$viewer: the world map would not come up with $LOCALKEY ($(fm $viewer | cut -c1-80))"
	sleep 1.5; p=$(fm $viewer); say "$viewer world: $p"
	[ "$(field "$p" wfitted)" = true ] && le2 "$(field "$p" wworst)" && pass "$viewer: the world map's fit from $(field "$p" wused) of the page's markers, worst $(field "$p" wworst) px" || fail "$viewer: world map fit fitted=$(field "$p" wfitted) worst=$(field "$p" wworst)"
	[ "$(field "$p" wshown)" = 1 ] && [ -n "$(px "$p" w)" ] && pass "$viewer: the friend is drawn on the world map at $(px "$p" w)" || fail "$viewer: the friend is not drawn on the world map (wshown=$(field "$p" wshown))"
	shot $viewer "world-$viewer"
	# the friend's spot as the mark has it, against where the friend's game says they are
	read -r gx gy <<<"$(pos_of $friend)"; gx=${gx%.*}; gy=${gy%.*}; w=$(wpos "$p")
	d=$(dist_xy "${w%,*} ${w#*,}" "$gx $gy"); [ -n "$d" ] && [ "$d" -le 300 ] && pass "$viewer: the mark is at the friend's spot (${d} u from where their game has them)" || fail "$viewer: the mark's spot $w is ${d:-?} u from the friend's $gx,$gy"
	# the friend walks while the map stays open: the mark follows
	before=$(px "$p" w); tx=$((gx + 900)); ty=$((gy + 900))
	rwalk $friend $tx $ty 150; say "$friend walked toward $tx,$ty (north-east): $WALKED"
	sleep 2; p2=$(fm $viewer); after=$(px "$p2" w); read -r hx hy <<<"$(pos_of $friend)"; hx=${hx%.*}; hy=${hy%.*}
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
hcell=$(field "$(dc host pos)" cell); vcell=$(field "$(dc vm pos)" cell)
for who in vm host; do dc $who "god on" >/dev/null; dc $who "console coc $INTERIOR" >/dev/null; done
say "both players coc'd into $INTERIOR (console placement, to reach an interior; god mode on while away from home)"
sleep 15; wait_ingame host 60; wait_ingame vm 60; sleep 8
[ "$(field "$(dc host pos)" cell)" = "$hcell" ] || [ "$(field "$(dc vm pos)" cell)" = "$vcell" ] && verdict VOID "a player did not reach $INTERIOR (still in the cell they were in)"
for viewer in host vm; do
	doors=$(dc $viewer "friendmap doors"); say "$viewer $doors"
	way=$(grep -oE "byDoors=[^ ]+" <<<"$doors" | cut -d= -f2); [ "${way:-none}" = none ] && way=$(grep -oE "byLocation=[^ ]+" <<<"$doors" | cut -d= -f2)
	open_map $viewer || { fail "$viewer: no map page inside"; continue; }
	if want_map $viewer local; then
		sleep 1.5; p=$(fm $viewer); say "$viewer local inside: $p"
		[ "$(field "$p" lshown)" = 1 ] && pass "$viewer: the friend in our interior is drawn on the local map at $(px "$p" l)" || fail "$viewer: the friend in our interior is not on the local map (lshown=$(field "$p" lshown))"
		shot $viewer "local-inside-$viewer"
	else fail "$viewer: no local map inside"; fi
	if want_map $viewer world; then
		sleep 1.5; p=$(fm $viewer); say "$viewer world inside: $p"
		[ "$(field "$p" wshown)" = 1 ] && [ "$(inside "$p")" = true ] && pass "$viewer: the friend inside is drawn on the world map at the way out, $(wpos "$p")" || fail "$viewer: the friend inside is not on the world map (wshown=$(field "$p" wshown) inside=$(inside "$p"))"
		w=$(wpos "$p"); o=${way#*@}; d=$(dist_xy "${w%,*} ${w#*,}" "${o%,*} ${o#*,}")
		[ "$way" != none ] && [ -n "$d" ] && [ "$d" -le 50 ] && pass "$viewer: the friend's mark is at our own way out ($way, ${d} u)" || fail "$viewer: the friend's mark $w is not at our own way out ($way)"
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
	grep -qE "(^| )l[0-9]+=true" <<<"$p" && fail "host inside: the friend outside is still counted for our interior's local map" || true
	shot host "world-host-inside-friend-out"
else fail "host: no world map inside (friend out)"; fi
close_map host
dc host "console coc SanctuaryExt" >/dev/null; say "host coc'd back to SanctuaryExt (console placement)"; sleep 15; wait_ingame host 60; sleep 5

for who in host vm; do dc $who "god off" >/dev/null; done

# ---- saves made right after the map was open, the friend leaving, and the saves loaded -----------------------------------------
# The game makes no save with the Pip-Boy up (neither its quicksave key nor the console's save do anything there): each save is a
# real quicksave (F5) the moment the Pip-Boy shuts on the map with the friend drawn on it, the friend still in the session. The
# VM's is loaded solo (it has left the session); the host's is loaded in its own session with nobody in it (the rig's host hosts
# again on every load, bHost), and its file is searched for the VM player's name.
FRIEND_NAME=$(dc host 'net status' | grep -oE "players: \[[0-9]+ '[^']+'" | head -1 | cut -d"'" -f2)
newest_save() {
	if [ "$1" = host ]; then ls -t "$HOME/Documents/My Games/Fallout4/Saves"/*.fos 2>/dev/null | head -1 | xargs -r -d '\n' basename
	else vmssh '(Get-ChildItem "$env:USERPROFILE\Documents\My Games\Fallout4\Saves" -Filter *.fos | Sort-Object LastWriteTime -Descending | Select-Object -First 1).Name' | tr -d '\r'; fi
}
declare -A SAVED
save_on_map() {  # save_on_map <who>: the friend drawn on its open world map, then F5 the moment the Pip-Boy shuts
	local who=$1 before p
	open_map $who && want_map $who world; sleep 1.5; p=$(fm $who)
	[ "$(field "$p" wshown)" = 1 ] || fail "$who: the friend is not drawn before the save ($(cut -c1-120 <<<"$p"))"
	before=$(newest_save $who)
	rkey $who 0x74 80; sleep 4
	[ "$(newest_save $who)" != "$before" ] && say "$who: F5 saved with the Pip-Boy up" || { close_map $who; rkey $who 0x74 80; sleep 8; }
	SAVED[$who]=$(newest_save $who)
	[ -n "${SAVED[$who]}" ] && [ "${SAVED[$who]}" != "$before" ] && say "$who saved by a real F5 the moment its map shut: ${SAVED[$who]}" || { fail "$who: no save was made (F5)"; SAVED[$who]=""; }
}
save_on_map host
save_on_map vm
# leaving: the host's map open, the VM leaves the session
open_map host >/dev/null; want_map host world >/dev/null
dc vm "net leave" >/dev/null; sleep 6; p=$(fm host)
[ "$(field "$p" wshown)" = 0 ] && [ -z "$(px "$p" w)" ] && pass "the friend left: their mark is gone from the host's open map" || fail "the friend left but the host's map still draws: $(cut -c1-200 <<<"$p")"
shot host "world-host-friend-left"
close_map host
# each save loaded: no strays, nothing of the friend's drawn on either map
for who in vm host; do
	[ -n "${SAVED[$who]}" ] || continue
	dc $who "console load \"${SAVED[$who]%.fos}\"" >/dev/null; sleep 20; wait_ingame $who 90; sleep 8
	st=$(dc $who 'net status'); sg=$(dc $who 'saveguard strays')
	say "$who after loading its save: $(grep -oE 'status=[a-z]+ id=[0-9]+ hosting=[a-z]+' <<<"$st"), players: $(grep -oE 'players: \[[^]]*\]' <<<"$st" | grep -c "'"); saveguard: $sg"
	[ $who = vm ] && { grep -q 'status=disconnected' <<<"$st" && pass "vm: its save loaded solo (no session)" || fail "vm: not solo after the load: $(cut -c1-80 <<<"$st")"; }
	[ "$(field "$sg" strays)" = 0 ] && pass "$who: its save loads with no strays" || fail "$who: strays after the load: $sg"
	open_map $who && want_map $who world; sleep 1.5; p=$(fm $who)
	[ "$(field "$p" map)" = world ] && [ "$(field "$p" wshown)" = 0 ] && [ -z "$(px "$p" w)" ] && ! grep -q '| friend' <<<"$p" && pass "$who: nothing of the friend's on the world map after the load" || fail "$who: the world map after the load: $(cut -c1-200 <<<"$p")"
	shot $who "world-$who-loaded"
	if want_map $who local; then sleep 1.5; p=$(fm $who)
		[ "$(field "$p" lshown)" = 0 ] && [ -z "$(px "$p" l)" ] && ! grep -q '| friend' <<<"$p" && pass "$who: nothing of the friend's on the local map after the load" || fail "$who: the local map after the load: $(cut -c1-200 <<<"$p")"
		shot $who "local-$who-loaded"
	else say "$who: the local map did not come up after the load (not checked)"; fi
	close_map $who
done
if [ -n "${SAVED[host]}" ] && [ ${#FRIEND_NAME} -ge 3 ]; then
	grep -qaF "$FRIEND_NAME" "$HOME/Documents/My Games/Fallout4/Saves/${SAVED[host]}" && fail "the host's save holds the friend's name" || pass "the host's save does not hold the friend's name"
else say "the host's save or the friend's name was not found: the name check was not made"; fi
say "the saves stay where the game put them (host ${SAVED[host]:-none}, vm ${SAVED[vm]:-none}): move them aside after the run"

if [ "$FAILS" = 0 ]; then verdict NO "every friend-map check held both ways (world, local, moving, interiors, leaving, saves made on the map)"; fi
verdict YES "$FAILS check(s) failed:${NOTES#;}"
