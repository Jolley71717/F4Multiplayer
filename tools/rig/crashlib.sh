#!/bin/bash
# crashlib.sh: crash logs on the two rig machines, told apart by NAME and MODIFICATION TIME, never by how many there are.
# The crash logger keeps only the newest 20, so a new crash log pushes the oldest out and the count stays at 20.
# Source it (". $S/crashlib.sh"); `bash crashlib.sh selftest` checks the logic without touching either machine.
#
#   crash_list_host / crash_list_vm   a listing: a first line "ok", then one "crash-<stamp>.log <mtime epoch>" line per log,
#                                     sorted. Empty output = the machine could not be read (a dropped ssh), not "no logs".
#   crash_new <before> <now>          the lines in <now> that were not in <before> (a new name, or the same name rewritten).
#                                     Nothing when either listing is empty (unreadable): the caller can't tell, and a note
#                                     goes to stderr.
#   crash_count <listing>             how many logs the listing holds
#   crash_names <lines>               just the names, one per line
#   crash_newest <listing>            the newest log: "name mtime"
#   crash_stamp <listing>             "name (mtime as a date)" of the newest log, for a status line
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
CL_HOST_DIR="$HOST_F4SE"
CL_KEY="$VM_KEY"
CL_VM="$VM_SSH"

crash_list_host() {
	local f
	echo ok
	for f in "$CL_HOST_DIR"/crash-*.log; do [ -e "$f" ] && echo "$(basename "$f") $(stat -c %Y "$f")"; done | LC_ALL=C sort
}

crash_list_vm() {  # three tries: one ssh hiccup must not read as "the VM has no crash logs"
	local t out
	for t in 1 2 3; do
		out=$(ssh -i "$CL_KEY" -o LogLevel=ERROR -o ConnectTimeout=20 "$CL_VM" '"ok"; Get-ChildItem "$env:USERPROFILE\Documents\My Games\Fallout4\F4SE" -Filter "crash-*.log" -ErrorAction SilentlyContinue | ForEach-Object { $_.Name + " " + [DateTimeOffset]::new($_.LastWriteTimeUtc).ToUnixTimeSeconds() }' 2>/dev/null | tr -d '\r')
		case "$out" in ok*) echo ok; printf '%s\n' "$out" | grep '^crash-' | LC_ALL=C sort; return;; esac
		sleep 3
	done
}

crash_new() {
	case "$1" in ok*) ;; *) echo "crash list before: unreadable, can't tell if a crash log is new" >&2; return 0;; esac
	case "$2" in ok*) ;; *) echo "crash list now: unreadable, can't tell if a crash log is new" >&2; return 0;; esac
	comm -13 <(printf '%s\n' "$1" | LC_ALL=C sort) <(printf '%s\n' "$2" | LC_ALL=C sort) | grep '^crash-'
}

crash_count() { printf '%s\n' "$1" | grep -c '^crash-'; }
crash_names() { printf '%s\n' "$1" | grep '^crash-' | cut -d' ' -f1; }
crash_newest() { printf '%s\n' "$1" | grep '^crash-' | LC_ALL=C sort | tail -1; }
crash_stamp() {
	local n; n=$(crash_newest "$1"); [ -n "$n" ] || { echo "none"; return; }
	echo "${n% *} ($(date -d "@${n#* }" '+%F %T' 2>/dev/null || echo "${n#* }"))"
}

# A fast fail (abort(): std::terminate, a pure virtual call; a /GS check) ends the game with no crash-*.log: the crash logger never
# sees it (2026-11-12, rig run 9: the VM's game gone mid-step, read as a rig fault). Windows' Application log has every one.
#   crash_events_host / crash_events_vm <since epoch>   Fallout4.exe's crashes there since then (Application Error, id 1000): a
#                                     first line "ok", then "<epoch> <HH:MM:SS> <module> <code> +<offset>" per crash. Empty output =
#                                     the log could not be read (a dropped ssh), not "none".
#   crash_event_lines <listing>       just the crash lines
_cl_events_ps() {  # the PowerShell that lists them since $1
	printf '%s' "\"ok\"; Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000; StartTime=[DateTimeOffset]::FromUnixTimeSeconds($1).LocalDateTime} -ErrorAction SilentlyContinue | Where-Object { \$_.Properties[0].Value -eq 'Fallout4.exe' } | ForEach-Object { '{0} {1} {2} 0x{3} +0x{4}' -f [DateTimeOffset]::new(\$_.TimeCreated).ToUnixTimeSeconds(), \$_.TimeCreated.ToString('HH:mm:ss'), \$_.Properties[3].Value, \$_.Properties[6].Value, (\$_.Properties[7].Value -replace '^0x','') }"
}
crash_events_host() {
	[[ $1 =~ ^[0-9]+$ ]] || return 0
	powershell -NoProfile -Command "$(_cl_events_ps "$1")" 2>/dev/null | tr -d '\r' | grep -E '^(ok$|[0-9]+ )'
}
crash_events_vm() {  # three tries, as crash_list_vm
	local t out
	[[ $1 =~ ^[0-9]+$ ]] || return 0
	for t in 1 2 3; do
		out=$(ssh -i "$CL_KEY" -o LogLevel=ERROR -o ConnectTimeout=20 "$CL_VM" "$(_cl_events_ps "$1")" 2>/dev/null | tr -d '\r')
		case "$out" in ok*) printf '%s\n' "$out" | grep -E '^(ok$|[0-9]+ )'; return;; esac
		sleep 3
	done
}
crash_event_lines() { printf '%s\n' "$1" | grep -E '^[0-9]+ '; }

if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "$1" = selftest ]; then
	fail=0; chk() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; fail=1; fi; }
	# twenty logs; a new crash replaces the oldest, so the count is still twenty
	before="ok"; for i in $(seq -w 1 20); do before+=$'\n'"crash-2026-10-19-09-00-$i.log 17000000$i"; done
	after="ok"; for i in $(seq -w 2 20); do after+=$'\n'"crash-2026-10-19-09-00-$i.log 17000000$i"; done; after+=$'\n'"crash-2026-10-19-09-55-53.log 1700001000"
	chk "count before" "$(crash_count "$before")" 20
	chk "count after (still 20)" "$(crash_count "$after")" 20
	chk "new crash found though the count held" "$(crash_names "$(crash_new "$before" "$after")")" "crash-2026-10-19-09-55-53.log"
	chk "newest by name" "$(crash_newest "$after")" "crash-2026-10-19-09-55-53.log 1700001000"
	chk "nothing new when nothing changed" "$(crash_new "$before" "$before")" ""
	chk "an older log pushed out is not new" "$(crash_new "$after" "$after")" ""
	rewritten="${before/crash-2026-10-19-09-00-05.log 1700000005/crash-2026-10-19-09-00-05.log 1700009999}"
	chk "a log rewritten under the same name (new mtime) is new" "$(crash_names "$(crash_new "$before" "$rewritten")")" "crash-2026-10-19-09-00-05.log"
	chk "no logs at all, then one" "$(crash_names "$(crash_new "ok" $'ok\ncrash-a.log 5')")" "crash-a.log"
	chk "unreadable before: nothing claimed" "$(crash_new "" "$after" 2>/dev/null)" ""
	chk "unreadable now: nothing claimed" "$(crash_new "$before" "" 2>/dev/null)" ""
	chk "stamp of an empty listing" "$(crash_stamp "ok")" "none"
	chk "event lines of a listing" "$(crash_event_lines $'ok\n1794523506 15:45:06 ucrtbase.dll 0xc0000409 +0x00000000000a527e')" "1794523506 15:45:06 ucrtbase.dll 0xc0000409 +0x00000000000a527e"
	chk "no event lines in an empty listing" "$(crash_event_lines "ok")" ""
	chk "a since that is no number reads nothing" "$(crash_events_host "")" ""
	he=$(crash_events_host "$(( $(date +%s) - 3600 ))"); case "$he" in ok*) echo "ok   host Application log reads ($(crash_event_lines "$he" | grep -c .) game crash(es) in the last hour)";; *) echo "FAIL host Application log"; fail=1;; esac
	hl=$(crash_list_host); case "$hl" in ok*) echo "ok   host listing reads ($(crash_count "$hl") logs, newest $(crash_stamp "$hl"))";; *) echo "FAIL host listing"; fail=1;; esac
	exit $fail
fi
