#!/bin/bash
# env.sh: where things are on the rig. Sourced by every script in tools/rig; nothing in this repository names a
# person or a machine. What differs per rig lives in the rig's own folder ($STAGE):
#   rig.conf         shell assignments, read if present:  VM_SSH=<user>@<vm host name>  (required)
#                    and optionally HOST_F4SE, HOST_GAME, VM_F4SE (see below)
#   vm_ssh_key       the key for that ssh login
#   vm-dev.token     the VM game's dev-channel token (rig.sh deploy/join writes it)
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
# the VM's dev channel is reached through an ssh tunnel on 7792
tunnel() { netstat -an | grep -q ":7792.*LISTEN" || { (ssh -i "$VM_KEY" -N -L 7792:127.0.0.1:7790 -o ServerAliveInterval=30 "$VM_SSH" >/dev/null 2>&1 &); sleep 2; }; }
# place <who> <x> <y> <z> <angle>: console placement (allowed by the rig rules when said; never used to stand in for movement)
place() { local w=$1; dc "$w" "console player.setpos x $2" >/dev/null; dc "$w" "console player.setpos y $3" >/dev/null; dc "$w" "console player.setpos z $4" >/dev/null; dc "$w" "console player.setangle z $5" >/dev/null; dc "$w" "console player.setangle x 0" >/dev/null; }
# alive: both games answer `status`; a loading screen can outlast one call. Prints "host=ingame=true vm=ingame=true".
alive() { local t h v; for t in 1 2 3 4; do h=$(host status | grep -o 'ingame=[a-z]*'); v=$(vm status | grep -o 'ingame=[a-z]*'); [ -n "$h" ] && [ -n "$v" ] && break; sleep 5; done; echo "host=${h:-DOWN} vm=${v:-DOWN}"; }
# wait_ingame <who> [tries]: until that game says ingame=true (2 s apart)
wait_ingame() { local i; for i in $(seq 1 "${2:-60}"); do dc "$1" status | grep -q 'ingame=true' && return 0; sleep 2; done; return 1; }
# enter_both <cell>: both games load the cell (console coc) and wait for it
enter_both() { bash "$RIG/dcfast.sh" both "console coc $1" >/dev/null 2>&1; sleep "${2:-14}"; wait_ingame host 30; wait_ingame vm 30; }
VM_GAME=${VM_GAME:-C:/Program Files (x86)/Steam/steamapps/common/Fallout 4}   # the VM game's install folder (scp form)
