#!/bin/bash
# dcfast.sh <host|vm> <command>: one dev-channel command over a raw socket (no PowerShell start-up).
# dcfast.sh both <command>: the same command to both games, written back to back (a few ms apart).
# The game's listener serves one client at a time (backlog 1), so a connect or auth can be refused
# while another call is in flight: every call is retried up to three times. A VM call that still gets no answer asks
# vm_reach_once (env.sh) to make the VM's tunnel and token again when the VM's game runs, and is tried once more.
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
who=$1; shift; cmd="$*"
tok_host=$(tr -d '\r\n ' < "$HOST_F4SE/F4Multiplayer_dev.token")
tok_vm() { tr -d '\r\n ' < "$STAGE/vm-dev.token"; }   # read at each try: a heal can write a new one
one() {  # one <port> <token> <command>: prints the reply; fails if the connection or auth failed
	exec 3<>/dev/tcp/127.0.0.1/$1 2>/dev/null || return 1
	echo "auth $2" >&3; read -t 5 -r reply <&3 2>/dev/null; [ "$reply" = "ok" ] || { exec 3>&-; return 1; }
	echo "$3" >&3; read -t 20 -r out <&3; echo "$out"; exec 3>&-
}
try() { for i in 1 2 3; do one "$@" && return 0; sleep 0.4; done; echo "error: dev channel unreachable on port $1" >&2; return 1; }
both() {  # both: 0 = both answered; 1 = the host's side failed; 2 = the VM's side did
	local a b f=1
	for i in 1 2 3; do
		f=1; exec 3<>/dev/tcp/127.0.0.1/7790 2>/dev/null || { sleep 0.4; continue; }
		f=2; exec 4<>/dev/tcp/127.0.0.1/7792 2>/dev/null || { exec 3>&-; sleep 0.4; continue; }
		echo "auth $tok_host" >&3; echo "auth $(tok_vm)" >&4; read -t 5 -r a <&3 2>/dev/null; read -t 5 -r b <&4 2>/dev/null
		if [ "$a" = "ok" ] && [ "$b" = "ok" ]; then echo "$cmd" >&3; echo "$cmd" >&4; read -t 20 -r a <&3; read -t 20 -r b <&4; echo "host: $a"; echo "vm: $b"; exec 3>&- 4>&-; return 0; fi
		f=1; [ "$a" = "ok" ] && f=2; exec 3>&- 4>&-; sleep 0.4
	done; return $f
}
case "$who" in
host) try 7790 "$tok_host" "$cmd" ;;
vm)   try 7792 "$(tok_vm)" "$cmd" 2>/dev/null || { vm_reach_once && try 7792 "$(tok_vm)" "$cmd"; } || { echo "error: dev channel unreachable on port 7792" >&2; exit 1; } ;;
both) both; r=$?
	[ $r = 2 ] && vm_reach_once && { both; r=$?; }
	[ $r = 0 ] || { echo "error: both failed" >&2; exit 1; } ;;
esac
