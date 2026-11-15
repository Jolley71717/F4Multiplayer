#!/bin/bash
# dcfast.sh <host|vm> <command>: one dev-channel command over a raw socket (no PowerShell start-up).
# dcfast.sh both <command>: the same command to both games, written back to back (a few ms apart).
# dcfast.sh selftest: the answer / no answer cases against a listener of its own (needs perl; touches no game).
# The game's listener serves one client at a time (backlog 1), so a connect or auth can be refused
# while another call is in flight: every call is retried up to three times. A VM call that still gets no answer asks
# vm_reach_once (env.sh) to make the VM's tunnel and token again when the VM's game runs, and is tried once more.
#
# A game that takes the connection and then says nothing is not answering: its main thread is hung or stalled (the VM's
# game stood still 15 to 72 s sixteen times in the runs of 2026-11-08 to 11-14), and the empty reply made the check that
# read it fail as the mod's ("got '', want '1'"). The command is never sent twice (it may have run). Instead, when a run
# gave a place for it (F4MP_DC_STATE, set by assert.sh's a_init), the game's state is written there: <state>.<host|vm>
# holds what happened while that game's LAST command got no answer in time, and is removed by its next answer. assert.sh
# records a check that fails meanwhile as a rig fault (channel), not a FAIL. A game that can't be reached at all (nothing
# listens, the connection is closed) is not this: it may be down or have crashed, and that stays the caller's to say.
[ "$1" = selftest ] && : "${VM_SSH:=selftest-no-vm}"
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
who=$1; shift; cmd="$*"
DC_T_AUTH=${DC_T_AUTH:-5}; DC_T_ANS=${DC_T_ANS:-20}   # seconds waited for the auth's reply and for the command's
na_set() { [ -n "${F4MP_DC_STATE:-}" ] && printf "%(%s)T\t%s (%(%T)T, '%s')\n" -1 "$2" -1 "${cmd:0:48}" > "$F4MP_DC_STATE.$1"; return 0; }   # na_set <host|vm> <what>: "<epoch><tab><what> (<time>, '<command>')"
na_clear() { [ -n "${F4MP_DC_STATE:-}" ] && [ -e "$F4MP_DC_STATE.$1" ] && rm -f "$F4MP_DC_STATE.$1"; return 0; }
one() {  # one <port> <token> <command> <host|vm>: prints the reply; fails if the connection or auth failed (NA: the auth timed out)
	local r; NA=""
	exec 3<>/dev/tcp/127.0.0.1/$1 2>/dev/null || return 1
	echo "auth $2" >&3; read -t "$DC_T_AUTH" -r reply <&3 2>/dev/null; r=$?
	[ "$reply" = "ok" ] || { [ $r -gt 128 ] && NA="took the connection and gave no answer to auth in $DC_T_AUTH s"; exec 3>&-; return 1; }
	echo "$3" >&3; read -t "$DC_T_ANS" -r out <&3; r=$?; echo "$out"; exec 3>&-
	if [ $r -gt 128 ]; then na_set "$4" "took the command and gave no answer in $DC_T_ANS s"; else na_clear "$4"; fi
	return 0
}
try() {
	local na=0
	for i in 1 2 3; do one "$@" && return 0; [ -n "$NA" ] && na=$((na + 1)); sleep 0.4; done
	if [ -n "$NA" ]; then na_set "$4" "$NA, in $na of three tries"; else na_clear "$4"; fi   # the last try is what the game does now
	echo "error: dev channel unreachable on port $1" >&2; return 1
}
both() {  # both: 0 = both answered; 1 = the host's side failed; 2 = the VM's side did
	local a b ra rb f=1
	for i in 1 2 3; do
		f=1; exec 3<>/dev/tcp/127.0.0.1/7790 2>/dev/null || { sleep 0.4; continue; }
		f=2; exec 4<>/dev/tcp/127.0.0.1/7792 2>/dev/null || { exec 3>&-; sleep 0.4; continue; }
		echo "auth $tok_host" >&3; echo "auth $(tok_vm)" >&4; read -t "$DC_T_AUTH" -r a <&3 2>/dev/null; read -t "$DC_T_AUTH" -r b <&4 2>/dev/null
		if [ "$a" = "ok" ] && [ "$b" = "ok" ]; then
			echo "$cmd" >&3; echo "$cmd" >&4; read -t "$DC_T_ANS" -r a <&3; ra=$?; read -t "$DC_T_ANS" -r b <&4; rb=$?; echo "host: $a"; echo "vm: $b"; exec 3>&- 4>&-
			if [ $ra -gt 128 ]; then na_set host "took the command and gave no answer in $DC_T_ANS s"; else na_clear host; fi
			if [ $rb -gt 128 ]; then na_set vm "took the command and gave no answer in $DC_T_ANS s"; else na_clear vm; fi
			return 0
		fi
		f=1; [ "$a" = "ok" ] && f=2; exec 3>&- 4>&-; sleep 0.4
	done; return $f
}
if [ "$who" = selftest ]; then
	command -v perl >/dev/null || { echo "selftest: skipped (no perl for the listener)"; exit 0; }
	bad=0; d=$(mktemp -d); export F4MP_DC_STATE=$d/na; DC_T_AUTH=1; DC_T_ANS=1; cmd="net status"
	# a listener of the test's own on a port of its own: <port> <mode>; it ends by itself (the token "quit", or 60 s)
	lis() {
		perl -e 'use IO::Socket::INET; my ($p, $m) = @ARGV; $SIG{ALRM} = sub { exit 0 }; alarm 60;
			my $s = IO::Socket::INET->new(Listen => 5, LocalAddr => "127.0.0.1", LocalPort => $p, ReuseAddr => 1) or exit 1;
			while (my $c = $s->accept) { $c->autoflush(1); my $l = <$c> // ""; exit 0 if $l =~ /^auth quit/;
				if ($m eq "mute") { my $q = <$c>; close $c; next }                 # no reply to auth
				print $c "ok\n"; my $q = <$c> // "";
				if ($m eq "silent") { sleep 2 } elsif ($m ne "drop") { print $c "answer to $q" }
				close $c }' "$1" "$2" &
		sleep 1
	}
	okt() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2], want [$3]"; bad=1; fi; }
	st() { if [ -s "$F4MP_DC_STATE.$1" ]; then sed "s/^[0-9]*\t//; s/ ([0-9:]*, / (T, /" "$F4MP_DC_STATE.$1"; else echo none; fi; }
	p=$((40000 + RANDOM % 20000))
	lis $p answer;         okt "an answer is printed" "$(one $p t "net status" host)" "answer to net status"; okt "... and leaves no state" "$(st host)" none
	lis $((p + 1)) silent; okt "no answer in time prints an empty reply, once (the command is not sent again)" "$(one $((p + 1)) t "net status" vm; echo "rc=$?")" $'\nrc=0'
	okt "... and the game's state says so, with the command" "$(st vm)" "took the command and gave no answer in 1 s (T, 'net status')"; okt "... the other game's is untouched" "$(st host)" none
	one $p t "net status" vm >/dev/null; okt "its next answer clears the state" "$(st vm)" none
	lis $((p + 2)) mute;   try $((p + 2)) t "net status" host >/dev/null 2>&1; okt "auth never answered: the call fails" $? 1
	okt "... and the state says the game took the connection and said nothing" "$(st host)" "took the connection and gave no answer to auth in 1 s, in 3 of three tries (T, 'net status')"
	lis $((p + 3)) drop;   na_set host "took the command and gave no answer in 1 s"; one $((p + 3)) t "net status" host >/dev/null; okt "a connection closed with no reply is not a hang: no state" "$(st host)" none
	na_set host "took the command and gave no answer in 1 s"; try $((p + 9)) t "net status" host >/dev/null 2>&1; okt "nothing listening: the call fails" $? 1
	okt "... and a game that can't be reached is not called hung (it may be down or have crashed)" "$(st host)" none
	( F4MP_DC_STATE=""; one $((p + 1)) t "net status" vm >/dev/null ); okt "with no run to tell, nothing is written" "$(ls "$d" | wc -l | tr -d ' ')" 0
	for q in $p $((p + 1)) $((p + 2)) $((p + 3)); do one $q quit x host >/dev/null 2>&1; done; wait 2>/dev/null
	rm -rf "$d"; [ $bad -eq 0 ] && echo "selftest: all ok" || echo "selftest: FAILED"; exit $bad
fi
tok_host=$(tr -d '\r\n ' < "$HOST_F4SE/F4Multiplayer_dev.token")
tok_vm() { tr -d '\r\n ' < "$STAGE/vm-dev.token"; }   # read at each try: a heal can write a new one
case "$who" in
host) try 7790 "$tok_host" "$cmd" host ;;
vm)   try 7792 "$(tok_vm)" "$cmd" vm 2>/dev/null || { vm_reach_once && try 7792 "$(tok_vm)" "$cmd" vm; } || { echo "error: dev channel unreachable on port 7792" >&2; exit 1; } ;;
both) both; r=$?
	[ $r = 2 ] && vm_reach_once && { both; r=$?; }
	[ $r = 0 ] || { echo "error: both failed" >&2; exit 1; } ;;
esac
