#!/bin/bash
# backup.sh save <name> | restore <name> | check <name>
#
# The rig rules (AGENTS.md): back up the ini and the session files on both machines before a test, restore them byte
# for byte afterwards. save copies, for the host and the VM, F4Multiplayer.ini and every F4Multiplayer*.session* and
# F4Multiplayer_player.id into $STAGE/backup/<name>/{host,vm}, with a hash list. restore puts them back (session files that
# did not exist at save time are moved into <name>/new-<time>, never deleted) and checks every hash. Stop both games
# before restore: a running game writes its session file again.
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
cmd=$1; name=${2:?name}; B=$STAGE/backup/$name
HOST_INI="$HOST_GAME/Data/F4SE/Plugins/F4Multiplayer.ini"; VM_INI="$VM_GAME/Data/F4SE/Plugins/F4Multiplayer.ini"
scp_vm() { scp -q -p -i "$VM_KEY" -o LogLevel=ERROR "$@" 2>&1 | grep -v "quantum\|openssh\|store now"; }
hashes() { (cd "$1" && sha256sum * 2>/dev/null | sort -k2); }
case "$cmd" in
save)
	[ -e "$B" ] && { echo "backup $name already exists: $B"; exit 1; }
	mkdir -p "$B/host" "$B/vm"
	cp -p "$HOST_F4SE"/F4Multiplayer*.session* "$HOST_F4SE/F4Multiplayer_player.id" "$HOST_INI" "$B/host/" 2>/dev/null
	scp_vm "$VM_SSH:$VM_F4SE/F4Multiplayer*.session*" "$VM_SSH:$VM_F4SE/F4Multiplayer_player.id" "$VM_SSH:$VM_INI" "$B/vm/"
	hashes "$B/host" > "$B/host.sha256"; hashes "$B/vm" > "$B/vm.sha256"
	echo "saved $(ls "$B/host" | wc -l) host files and $(ls "$B/vm" | wc -l) vm files to $B" ;;
restore|check)
	[ -d "$B" ] || { echo "no backup $name"; exit 1; }
	if [ "$cmd" = restore ]; then
		tag=$(date +%m%d-%H%M); mkdir -p "$B/host/new-$tag" "$B/vm/new-$tag"
		for f in "$HOST_F4SE"/F4Multiplayer*.session* "$HOST_F4SE/F4Multiplayer_player.id"; do [ -e "$f" ] && [ ! -e "$B/host/$(basename "$f")" ] && mv "$f" "$B/host/new-$tag/"; done
		for f in "$B"/host/*; do [ -f "$f" ] && case "$f" in *F4Multiplayer.ini) cp -p "$f" "$HOST_INI";; *) cp -p "$f" "$HOST_F4SE/$(basename "$f")";; esac; done
		keep=$(cd "$B/vm" && ls | grep -v '^new-' | tr '\n' ' ')
		ps='$d="$env:USERPROFILE\Documents\My Games\Fallout4\F4SE"; $keep=@(KEEPLIST); $new="C:\F4MP\rig-new-TAG"; New-Item -ItemType Directory -Force $new | Out-Null; Get-ChildItem $d -Filter "F4Multiplayer*.session*" | ForEach-Object { if ($keep -notcontains $_.Name) { Move-Item $_.FullName (Join-Path $new $_.Name) -Force } }'
		kl=$(cd "$B/vm" && ls | grep -v '^new-' | sed "s/.*/'&'/" | paste -sd, -)
		ps=${ps//KEEPLIST/$kl}; vmssh "${ps//TAG/$tag}" >/dev/null
		for f in "$B"/vm/*; do [ -f "$f" ] || continue; n=$(basename "$f"); case "$n" in F4Multiplayer.ini) scp_vm "$f" "$VM_SSH:$VM_INI";; *) scp_vm "$f" "$VM_SSH:$VM_F4SE/$n";; esac; done
	fi
	bad=0
	while read -r h n; do n=${n#"*"}
		[ "$n" = "F4Multiplayer.ini" ] && now=$(sha256sum "$HOST_INI" | cut -c1-64) || now=$(sha256sum "$HOST_F4SE/$n" 2>/dev/null | cut -c1-64)
		[ "$h" = "$now" ] || { echo "DIFF host $n"; bad=1; }
	done < "$B/host.sha256"
	while read -r h n; do n=${n#"*"}
		[ "$n" = "F4Multiplayer.ini" ] && p="$VM_INI" || p="$VM_F4SE/$n"
		now=$(vmssh "(Get-FileHash -Algorithm SHA256 \"\$(if ('$p' -like 'C:*') { '$p' } else { \$env:USERPROFILE + '\' + ('$p' -replace '/','\') })\").Hash" | tr -d '[:space:]' | tr A-F a-f)
		[ "$h" = "$now" ] || { echo "DIFF vm $n"; bad=1; }
	done < "$B/vm.sha256"
	[ $bad = 0 ] && echo "all files identical to backup $name (host $(wc -l < "$B/host.sha256"), vm $(wc -l < "$B/vm.sha256"))"; exit $bad ;;
*) echo "usage: backup.sh save|restore|check <name>" ;;
esac
