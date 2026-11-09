#!/bin/bash
# env.sh: where things are on the rig. Sourced by every script in tools/rig; nothing in this repository names a
# person or a machine. What differs per rig lives in the rig's own folder ($STAGE):
#   rig.conf         shell assignments, read if present:  VM_SSH=<user>@<vm host name>  (required)
#                    and optionally HOST_F4SE, HOST_GAME, VM_F4SE (see below)
#   vm_ssh_key       the key for that ssh login
#   vm-dev.token     the VM game's dev-channel token (rig.sh deploy/join writes it; vm_reach below reads it again)
#   vm-reach.log     each time vm_reach made the VM's channel reachable again (a run records them as HEAL lines)
#   rig.sh           deploy / join / status for the two games (tools/rig does not replace it)
# Everything else is a default that follows $HOME.
RIG=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$RIG/../.." && pwd)
STAGE=${F4MP_STAGE:-$HOME/F4MP-VM/stage}
[ -f "$STAGE/rig.conf" ] && . "$STAGE/rig.conf"
: "${VM_SSH:?set VM_SSH=<user>@<host> in $STAGE/rig.conf}"
HOST_F4SE=${HOST_F4SE:-$HOME/Documents/My Games/Fallout4/F4SE}       # the host game's F4SE folder: logs, ini, sessions
HOST_GAME=${HOST_GAME:-F:/SteamLibrary/steamapps/common/Fallout 4}   # the host game's install folder
VM_F4SE=${VM_F4SE:-Documents/My Games/Fallout4/F4SE}                 # the same folder on the VM, relative to its user profile (scp form)
VM_KEY=${VM_KEY:-$STAGE/vm_ssh_key}
CHECKS=${F4MP_CHECKS:-$STAGE/checks}                                 # where runs write their logs and results
INPUT_PS1=$REPO/tools/input.ps1                                      # real mouse and keyboard on the host game's window

# One dev-channel command; the reply's last line. host() and vm() are the two games.
dc() { local w=$1; shift; bash "$RIG/dcfast.sh" "$w" "$@" 2>/dev/null | tail -1; }
host() { dc host "$@"; }
vm() { dc vm "$@"; }
vmssh() { ssh -i "$VM_KEY" -o LogLevel=ERROR -o ConnectTimeout=20 "$VM_SSH" "$@" 2>/dev/null | tr -d '\r'; }
# the VM's dev channel is reached through an ssh tunnel on 7792 (this PC) to the game's 7790 (the VM). The tunnel lives as
# long as its ssh, and nothing made it again: on 2026-11-09 (the 0.16.1-rc1 soak) the VM stalled for about 36 s in its graphics
# driver, the tunnel's ssh ended with it, and every VM call after was "Connection refused" on 7792 though the VM's game
# answered again on its own; the soak read the VM as DOWN and stopped. dcfast.sh now calls vm_reach (below) when the VM's
# channel gives no answer.
# tunnel_pid: the Windows pid that listens on 127.0.0.1:7792 (empty when nothing does)
tunnel_pid() { netstat -ano 2>/dev/null | tr -d '\r' | awk '$2 == "127.0.0.1:7792" && $4 == "LISTENING" {print $5; exit}'; }
# tunnel: made when nothing listens on 7792; 0 once something does. ExitOnForwardFailure: an ssh that can't take the port
# ends instead of staying as a session that forwards nothing; BatchMode: never a prompt.
tunnel() {
	local i; [ -n "$(tunnel_pid)" ] && return 0
	(ssh -i "$VM_KEY" -N -L 7792:127.0.0.1:7790 -o ServerAliveInterval=30 -o ExitOnForwardFailure=yes -o BatchMode=yes -o ConnectTimeout=20 "$VM_SSH" >/dev/null 2>&1 &)
	for i in $(seq 1 20); do sleep 0.5; [ -n "$(tunnel_pid)" ] && return 0; done; return 1
}
# tunnel_restart: the ssh that holds 7792 is ended (only an ssh.exe: anything else on the port is left alone, and fails) and
# a new tunnel made
tunnel_restart() {
	local p i; p=$(tunnel_pid)
	if [ -n "$p" ]; then
		tasklist //FI "PID eq $p" //NH 2>/dev/null | grep -qi '^ssh\.exe' || { echo "7792 is held by pid $p, which is not an ssh: left alone"; return 1; }
		taskkill //PID "$p" //F >/dev/null 2>&1
		for i in $(seq 1 20); do [ -z "$(tunnel_pid)" ] && break; sleep 0.25; done
	fi
	tunnel
}
# vm_token_fetch: the VM game's dev-channel token read again into $STAGE/vm-dev.token (the game writes a new one each start).
# Prints unchanged, changed or unread (1: no token read, the file is left as it was).
vm_token_fetch() {
	local t; t=$(vmssh 'Get-Content "$env:USERPROFILE\Documents\My Games\Fallout4\F4SE\F4Multiplayer_dev.token" -Raw' | tr -d '[:space:]')
	[[ $t =~ ^[0-9a-f]{64}$ ]] || { echo unread; return 1; }
	[ "$t" = "$(tr -d '[:space:]' < "$STAGE/vm-dev.token" 2>/dev/null)" ] && { echo unchanged; return 0; }
	printf '%s' "$t" > "$STAGE/vm-dev.token"; echo changed
}
# vm_reach: the VM's dev channel gave no answer (dcfast.sh tried three times): when the VM's Fallout4 runs, its token is read
# again and the tunnel made again, and the channel asked once more. Prints one line saying what it found and did; 0 = the
# VM's game answers now. A heal that worked is appended to $STAGE/vm-reach.log ("<epoch> TAB <time> TAB <what>"), which a run
# in progress records as a HEAL (assert.sh). Never a heal while the game is not running (a restart, a crash: not the tunnel's)
# or ssh to the VM does not answer (the VM itself is down or stalled: the caller's own wait goes on).
vm_reach() {
	local why s tok r a i
	if [ -n "$(tunnel_pid)" ]; then why="the tunnel on 7792 was up but the VM's game gave no answer through it (no reply, or its token refused)"
	else why="nothing listened on 7792 (the tunnel's ssh had ended)"; fi
	s=$(vmssh '"up " + ((Get-Process Fallout4 -ErrorAction SilentlyContinue | Select-Object -First 1).Id)' | tr -d '\r' | tail -1)
	case "$s" in
	"up "[0-9]*) ;;
	up*) echo "$why; the VM's Fallout4 is not running: not healed"; return 1 ;;
	*) echo "$why; ssh to the VM gives no answer either: not healed"; return 1 ;;
	esac
	tok=$(vm_token_fetch)
	r=$(tunnel_restart) || { echo "$why; the VM's Fallout4 runs (pid ${s#up }); the tunnel could not be made again: $r"; return 1; }
	for i in 1 2 3 4 5; do a=$(F4MP_NO_REACH=1 bash "$RIG/dcfast.sh" vm status 2>/dev/null | tail -1); [ -n "$a" ] && break; sleep 2; done
	if [ -z "$a" ]; then echo "$why; the VM's Fallout4 runs (pid ${s#up }), token $tok, tunnel made again: still no answer"; return 1; fi
	r="$why; the VM's Fallout4 runs (pid ${s#up }): token $tok, tunnel made again; it answers: $(cut -c1-60 <<<"$a")"
	printf '%s\t%s\t%s\n' "$(date +%s)" "$(date +%T)" "$r" >> "$STAGE/vm-reach.log"
	echo "$r"; return 0
}
# vm_reach_once: vm_reach for dcfast.sh, one at a time and not more often than every 30 s (a game that is down for good isn't
# asked over ssh on every call). 0 = the caller should try again: a heal worked, or another call's heal finished meanwhile.
vm_reach_once() {
	local lk=$STAGE/vm-reach.lock st=$STAGE/vm-reach.stamp i
	[ "$F4MP_NO_REACH" = 1 ] && return 1
	# a lock left by a call that was killed mid-heal goes after 2 minutes
	[ -d "$lk" ] && [ $(( $(date +%s) - $(stat -c %Y "$lk" 2>/dev/null || echo 0) )) -gt 120 ] && rmdir "$lk" 2>/dev/null
	if ! mkdir "$lk" 2>/dev/null; then
		for i in $(seq 1 120); do [ -d "$lk" ] || return 0; sleep 0.5; done; return 1
	fi
	if [ -f "$st" ] && [ $(( $(date +%s) - $(stat -c %Y "$st" 2>/dev/null || echo 0) )) -lt 30 ]; then rmdir "$lk"; return 1; fi
	touch "$st"; vm_reach >&2; i=$?; rmdir "$lk"; return $i
}
# place <who> <x> <y> <z> <angle>: console placement (allowed by the rig rules when said; never used to stand in for movement)
place() { local w=$1; dc "$w" "console player.setpos x $2" >/dev/null; dc "$w" "console player.setpos y $3" >/dev/null; dc "$w" "console player.setpos z $4" >/dev/null; dc "$w" "console player.setangle z $5" >/dev/null; dc "$w" "console player.setangle x 0" >/dev/null; }
# alive: both games answer `status`; a loading screen can outlast one call. Prints "host=ingame=true vm=ingame=true".
alive() { local t h v; for t in 1 2 3 4; do h=$(host status | grep -o 'ingame=[a-z]*'); v=$(vm status | grep -o 'ingame=[a-z]*'); [ -n "$h" ] && [ -n "$v" ] && break; sleep 5; done; echo "host=${h:-DOWN} vm=${v:-DOWN}"; }
# wait_ingame <who> [tries]: until that game says ingame=true (2 s apart)
wait_ingame() { local i; for i in $(seq 1 "${2:-60}"); do dc "$1" status | grep -q 'ingame=true' && return 0; sleep 2; done; return 1; }
# enter_both <cell>: both games load the cell (console coc) and wait for it
enter_both() { bash "$RIG/dcfast.sh" both "console coc $1" >/dev/null 2>&1; sleep "${2:-14}"; wait_ingame host 30; wait_ingame vm 30; }
VM_GAME=${VM_GAME:-C:/Program Files (x86)/Steam/steamapps/common/Fallout 4}   # the VM game's install folder (scp form)
