#!/bin/bash
# host-restart-stale-copies.sh: after the host's game restarts mid-session (quit to the desktop and back), the joiner keeps stale
# copies of NPCs and can kill copies whose NPCs are alive in the host's game (seen on the rig and in players' logs: the joiner's
# "lives after all ..." lines, the server's "death of ... dropped: player 1 runs it and the reporter wasn't fighting it").
#   1. Both games in a populated cell (console coc $STALE_CELL, said), joined. The joiner (the VM) gets a pistol and rounds by
#      console additem and kills (real clicks, `turnto ... aim` to aim; a real walk to 400 u when a wall is between) the nearest
#      STALE_KILLS NPCs it can see (never a game-made FF reference: the players' stand-ins), so the session holds deaths and the joiner copies
#   2. The host's game is stopped (as a quit to the desktop) and started again (f4se_loader, no -ExecutionPolicy), loads the same
#      cell by console coc from its main menu (a fresh world: the dead are alive there again, as after a reload of an older
#      save) and hosts; the joiner's game is NOT reloaded: it joins the new lobby where it stands (steam://joinlobby).
#   3. 20 s on, each NPC the host's game lists is compared: dead in the joiner's game and alive in the host's is a stale copy.
#   4. The joiner fights again: it shoots (three clicks) each NPC, nearest first, that lives in its own game and it can see, up to 10; 30 s on
#   5. Compared again. REPRO: YES when an NPC lies dead in the joiner's game while the host's game has it alive (after the 30 s
#      the copies get to be raised), NO when every NPC both games list agrees.
# FRESH_SESSION=1: the restarted host starts a new session (its session file moved aside, see below); without it the host reloads
# the same session, whose deaths are replayed into its fresh world (WorldSync "killed ... (died in another player's world)").
# Both logs' lines from the restart on are kept, and the lines that name the case are printed.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
STALE_CELL=${STALE_CELL:-CorvegaAssemblyPlant01}; STALE_KILLS=${STALE_KILLS:-3}
PISTOL=00004822; AMMO=0001F276
need_session
HOSTID=$(grep -oE 'joinlobby/377160/\$lobby/[0-9]+' "$STAGE/rig.sh" | grep -oE '[0-9]+$' | head -1)
[ -n "$HOSTID" ] || verdict VOID "the host's Steam id for the join link is not in \$STAGE/rig.sh"
npcs() { dc host 'actors 3000' | grep -oE "[0-9A-F]{8} '" | cut -c1-8; }
dead_in() { field "$(dc "$1" "combat $2")" dead; }
# compare <label>: every NPC the host's game lists, dead or alive in each game; STALE: dead for the joiner, alive for the host
compare() {
	local a h v n=0 both=0; STALE=""; MISSING=0; DEADBOTH=""
	for a in $(npcs); do
		n=$((n + 1)); h=$(dead_in host "$a"); v=$(dead_in vm "$a")
		[ -z "$h" ] || [ -z "$v" ] && { MISSING=$((MISSING + 1)); continue; }
		both=$((both + 1))
		[ "$v" = true ] && [ "$h" = false ] && STALE="$STALE $a"
		[ "$v" = true ] && [ "$h" = true ] && DEADBOTH="$DEADBOTH $a"
	done
	say "$1: $n NPCs listed by the host's game, $both read in both, $MISSING missing in one; dead in both:${DEADBOTH:- none}; dead for the joiner and alive for the host:${STALE:- none}"
	COMPARED=$both
}
arm_vm() { dc vm "console player.additem $PISTOL 1" >/dev/null; dc vm "console player.additem $AMMO 300" >/dev/null; dc vm "console player.equipitem $PISTOL" >/dev/null; sleep 2; dc vm "draw on" >/dev/null; sleep 1; }
# in_sight <ref>: the joiner turns to it (aiming at its middle) and the crosshair's pick reaches it (no wall between)
in_sight() { dc vm "turnto $1 300 aim" | grep -q 'inPick=true'; }
# shoot <ref> [clicks]: the joiner turns to it and clicks; when a wall is between, it first walks (real walk) to 400 u of it
shoot() {
	local i
	in_sight "$1" || { rwalk vm "$1" 400 0.8; in_sight "$1"; } || return 1
	for i in $(seq 1 "${2:-2}"); do dc vm "turnto $1 200 aim" >/dev/null; sleep 0.3; dc vm "click 120" >/dev/null; sleep 1.0; done
}
# the NPCs the joiner's game lists within 2500 u of the joiner, nearest first
near_vm() { local p; p=$(pos_of vm); dc vm 'actors 2500' | grep -oE "[0-9A-F]{8} '" | cut -c1-8 | while read -r a; do r=$(dc vm "refinfo $a"); echo "$(dist_xy "$p" "$(field "$r" x) $(field "$r" y)") $a"; done | sort -n | awk '{print $2}'; }

# 1. the scene, and deaths before the restart
for w in host vm; do say "set-up: $w console coc $STALE_CELL"; dc $w focus >/dev/null; dc $w "console coc $STALE_CELL" >/dev/null; done
sleep 15; wait_ingame host 60; wait_ingame vm 60; dc host "god on" >/dev/null; dc vm "god on" >/dev/null; sleep 5
wait_for 30 eval 'dc vm "net status" | grep -q status=joined' || verdict VOID "the joiner left the session at the cell's load"
list=$(npcs); [ "$(wc -w <<<"$list")" -ge 3 ] || verdict VOID "fewer than 3 NPCs near the host at $STALE_CELL ($(wc -w <<<"$list"))"
say "set-up: the joiner armed by console (pistol, rounds)"; arm_vm
k=0
for a in $(near_vm); do
	[ $k -ge "$STALE_KILLS" ] && break; [ $SECONDS -gt 420 ] && break
	case "$a" in FF*) continue;; esac   # a game-made reference (a player's stand-in among them): never shot
	[ "$(dead_in vm "$a")" = false ] || continue
	for i in 1 2 3 4; do shoot "$a" 2 || break; [ "$(dead_in vm "$a")" = true ] && break; done
	[ "$(dead_in vm "$a")" = true ] && { k=$((k + 1)); say "before the restart: the joiner killed $a (host's game: dead=$(dead_in host "$a"))"; }
done
sleep 5; compare "before the restart"
[ -n "$DEADBOTH" ] || verdict VOID "no NPC lies dead in both games before the restart (the joiner killed $k): no shared deaths for the copies to go stale on"

# 2. the host's game restarts; the joiner rejoins where it stands
rmark vm restart
say "the host's game is stopped (a quit to the desktop) and started again"
SESSION_FILE=$(grep -oE "its session is kept in [^ ]+\.session" "$HOST_F4SE/F4Multiplayer.log" | tail -1 | awk '{print $NF}')
powershell -NoProfile -Command "Get-Process Fallout4 -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:\$false" >/dev/null 2>&1; sleep 6
# FRESH_SESSION=1: the host's game comes back to a new session (its session file and that file's .bak moved aside into the run's
# folder first, never deleted; the rig's backup restores them), as a host who starts another character or loses the file: the
# session's history of deaths is not replayed into the host's fresh world. Without it the restarted host loads the same session.
if [ "${FRESH_SESSION:-0}" = 1 ]; then
	[ -n "$SESSION_FILE" ] && [ -f "$HOST_F4SE/$SESSION_FILE" ] || verdict VOID "FRESH_SESSION: the host's session file is not known ('$SESSION_FILE')"
	mkdir -p "$REPRO_OUT/session-aside"; mv -f "$HOST_F4SE/$SESSION_FILE" "$REPRO_OUT/session-aside/"
	[ -f "$HOST_F4SE/$SESSION_FILE.bak" ] && mv -f "$HOST_F4SE/$SESSION_FILE.bak" "$REPRO_OUT/session-aside/"
	say "FRESH_SESSION: the host's session file $SESSION_FILE (and its .bak) moved aside to $REPRO_OUT/session-aside"
fi
t0=$(date +%s)
powershell -NoProfile -Command "Start-Process -FilePath '$(cygpath -w "$HOST_GAME")\\f4se_loader.exe' -WorkingDirectory '$(cygpath -w "$HOST_GAME")'"
ok=""; for i in $(seq 1 90); do sleep 2; [ "$(stat -c %Y "$HOST_F4SE/F4Multiplayer.log" 2>/dev/null || echo 0)" -ge "$t0" ] && grep -q 'Game data ready' "$HOST_F4SE/F4Multiplayer.log" && { ok=1; break; }; done
[ -n "$ok" ] || verdict VOID "the host's game reached no main menu in 180 s"
for i in $(seq 1 30); do dc host status | grep -q '^ingame=' && break; sleep 2; done
dc host focus >/dev/null; sleep 1; dc host "console coc $STALE_CELL" >/dev/null
for i in $(seq 1 60); do dc host focus >/dev/null; dc host status | grep -q 'ingame=true' && break; sleep 2; done
dc host "god on" >/dev/null; sleep 5
say "the host is back in $STALE_CELL; the joiner (not reloaded) was: $(dc vm 'net status' | grep -oE 'status=[a-z]+' | head -1)"
lobby=$(dc host "net status" | grep -o 'lobby=[0-9]*' | cut -d= -f2)
vmssh "& C:\F4MP\in-vm-run.ps1 -Name \"F4MP-Join-\$(Get-Date -Format HHmmss)\" -Exe 'C:\Program Files (x86)\Steam\steam.exe' -Arguments 'steam://joinlobby/377160/$lobby/$HOSTID'" >/dev/null
wait_for 90 eval 'dc vm "net status" | grep -q status=joined' || verdict VOID "the joiner did not join the restarted host's session ($(dc vm 'net status' | grep -oE 'status=[a-z]+' | head -1))"
say "the joiner joined the restarted host's session; its cell: $(field "$(dc vm status)" cell) (the host's: $(field "$(dc host status)" cell))"
sleep 20; compare "20 s after the rejoin"

# 4. the joiner fights the NPCs whose copies it had
arm_vm; n=0
for a in $(near_vm); do
	case "$a" in FF*) continue;; esac
	[ $n -ge 10 ] && break
	[ "$(dead_in vm "$a")" = false ] || continue
	shoot "$a" 3 && n=$((n + 1))
done
say "the joiner shot at $n NPCs alive in its game"
sleep 30; compare "30 s after the joiner's fight"
FINAL=$STALE
rkeep host host-restart-stale-copies; rsince vm restart > "$REPRO_OUT/host-restart-stale-copies-vm.log"
V="$REPRO_OUT/host-restart-stale-copies-vm.log"; H="$REPRO_OUT/host-restart-stale-copies-host.log"
say "joiner's 'lives after all' lines: $(grep -c 'lives after all' "$V"); host's dropped deaths: $(grep -c 'dropped: player [0-9]* runs it' "$H")"
grep -E 'lives after all' "$V" | tail -3 | while read -r l; do say "joiner: ${l:0:260}"; done
grep -E 'dropped: player [0-9]* runs it' "$H" | tail -3 | while read -r l; do say "host: ${l:0:260}"; done
for a in $FINAL; do say "stale $a: host $(dc host "combat $a" | grep -oE 'dead=[a-z]+ hp=[-0-9.]+') | joiner $(dc vm "combat $a" | grep -oE 'dead=[a-z]+ hp=[-0-9.]+'); joiner's lines: $(grep -c "$a" "$V")"; done
[ "${COMPARED:-0}" -ge 3 ] || verdict VOID "only ${COMPARED:-0} NPCs read in both games"
[ -n "$FINAL" ] && verdict YES "$(wc -w <<<"$FINAL") NPC(s) dead in the joiner's game and alive in the host's 30 s after the fight:$FINAL"
verdict NO "every NPC both games list agrees, dead or alive, after the host's restart and the joiner's fight"
