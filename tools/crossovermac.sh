#!/bin/bash
# Fallout 4 under CrossOver on a Mac: what deploy.ps1, restart-game.ps1 and devctl.ps1 do on Windows, for the plugin
# built with tools/xwin.lua (docs/misc/MAC.md).
#
#   tools/crossovermac.sh deploy [mode]                 # the build into the game; the ini only if the game has none
#   tools/crossovermac.sh restart [cell] [--no-deploy]  # close, deploy, start through Steam, SanctuaryExt in god mode
#   tools/crossovermac.sh restart ''                    # stop at the main menu
#   tools/crossovermac.sh dev status pos "console coc QASmoke"
#   tools/crossovermac.sh test [name]                   # the Windows build's F4MPTests.exe; [name]: only those tests
#
# XSE_FO4_GAME_PATH is the game's folder inside a CrossOver bottle (as for xmake install); the bottle, its CrossOver
# and its Documents follow from it. F4MP_DEV_PORT and F4MP_DEV_TOKEN as for devctl.ps1. restart starts the game the
# way Steam's Play button does, so Steam's overlay is in it: Steam must start F4SE (docs/misc/MAC.md says how).
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
game=${XSE_FO4_GAME_PATH:?set XSE_FO4_GAME_PATH to the game folder in the CrossOver bottle}
case $game in
*/Bottles/*/drive_c/*) ;;
*) echo "XSE_FO4_GAME_PATH is not in a CrossOver bottle: $game" >&2; exit 1 ;;
esac
bottle_rest=${game#*/Bottles/}
bottle=${bottle_rest%%/*}
bottle_dir="${game%%/Bottles/*}/Bottles/$bottle"
f4se_docs="$bottle_dir/drive_c/users/crossover/Documents/My Games/Fallout4/F4SE"

deploy() {
	local build="$root/build/windows/x64/${1:-releasedbg}"
	local plugins="$game/Data/F4SE/Plugins"
	mkdir -p "$plugins/F4Multiplayer/lang"
	cp -f "$build/F4Multiplayer.dll" "$build/F4Multiplayer.pdb" "$plugins/"
	[ -f "$plugins/F4Multiplayer.ini" ] || cp "$root/dist/F4Multiplayer.ini" "$plugins/"
	# the texts belong to the build, unlike the ini
	cp -f "$root/dist/lang/"*.txt "$plugins/F4Multiplayer/lang/"
	echo "Deployed to $plugins"
}

# One reply line per command, as devctl.ps1 prints them.
dev() {
	[ $# -gt 0 ] || { echo "usage: crossovermac.sh dev <command> [<command> ...]" >&2; exit 2; }
	local token=${F4MP_DEV_TOKEN:-}
	if [ -z "$token" ]; then
		[ -f "$f4se_docs/F4Multiplayer_dev.token" ] || { echo "no dev token in $f4se_docs: is the game running with bDevChannel = true?" >&2; exit 1; }
		token=$(tr -d '\r\n' < "$f4se_docs/F4Multiplayer_dev.token")
	fi
	{ exec 3<>"/dev/tcp/127.0.0.1/${F4MP_DEV_PORT:-7790}"; } 2> /dev/null ||
		{ echo "nothing on port ${F4MP_DEV_PORT:-7790}: is the game running with bDevChannel = true?" >&2; exit 1; }
	printf 'auth %s\n' "$token" >&3
	local reply
	IFS= read -r -t 10 reply <&3
	[ "${reply%$'\r'}" = ok ] || { echo "authentication failed: $reply" >&2; exit 1; }
	for command in "$@"; do
		printf '%s\n' "$command" >&3
		echo "> $command"
		IFS= read -r -t 10 reply <&3
		echo "${reply%$'\r'}"
	done
	exec 3>&-
}

# The CrossOver whose version is the bottle's: another one updates the bottle, or fails to.
crossover_wine() {
	local version
	version=$(sed -n 's/^"Version" *= *"\(.*\)"/\1/p' "$bottle_dir/cxbottle.conf")
	for app in /Applications/CrossOver*.app; do
		if [ "$(/usr/libexec/PlistBuddy -c 'Print CFBundleVersion' "$app/Contents/Info.plist" 2>/dev/null)" = "$version" ]; then
			echo "$app/Contents/SharedSupport/CrossOver/bin/wine"
			return
		fi
	done
	echo "no CrossOver in /Applications has the bottle's version, $version" >&2
	exit 1
}

restart() {
	local cell=SanctuaryExt deploy_build=1 timeout=120
	for arg in "$@"; do
		case $arg in
		--no-deploy) deploy_build=0 ;;
		*) cell=$arg ;;
		esac
	done
	if ! cmp -s "$game/f4se_loader.exe" "$game/Fallout4Launcher.exe"; then
		echo "Steam's Play starts Fallout4Launcher.exe, which is not f4se_loader.exe: see docs/misc/MAC.md" >&2
		exit 1
	fi
	local wine
	wine=$(crossover_wine)

	# the bottle's own taskkill: a pkill would close every bottle's game, a second bottle's joiner too
	"$wine" --bottle "$bottle" --no-update taskkill /f /im Fallout4.exe > /dev/null 2>&1 || true
	sleep 3
	[ $deploy_build = 0 ] || deploy
	local launched
	launched=$(date +%s)
	local steam="C:\\Program Files (x86)\\Steam\\steam.exe"
	"$wine" --bottle "$bottle" --no-update --cx-app "$steam" -applaunch 377160 > /dev/null 2>&1

	local log="$f4se_docs/F4Multiplayer.log" deadline=$((launched + timeout))
	until [ -f "$log" ] && [ "$(stat -f %m "$log")" -ge "$launched" ] && grep -q 'Game data ready' "$log"; do
		[ "$(date +%s)" -lt $deadline ] || { echo "timed out waiting for the game to reach the main menu" >&2; exit 1; }
		sleep 2
	done
	echo "Main menu reached"
	[ -n "$cell" ] || return 0

	sleep 3
	local started status
	started=$(date +%s)
	dev "console coc $cell" > /dev/null
	until status=$(dev status | tail -1) && [ "${status#ingame=true}" != "$status" ]; do
		[ "$(date +%s)" -lt $deadline ] || { echo "timed out loading $cell" >&2; exit 1; }
		sleep 1
	done
	echo "$status (cell load took $(($(date +%s) - started))s)"
	dev "console tgm" > /dev/null
}

# The Windows build's tests, against Microsoft's C++ library as on Windows: the native build's libc++ fails a test
# MSVC passes. The exit code is the number of failed tests.
run_tests() {
	local wine
	wine=$(crossover_wine)
	# tests read the sources (SourceTree.h), so they run from the repo; the 2> drops Wine's own msync lines
	"$wine" --bottle "$bottle" --no-update --workdir "$root" "$root/build/windows/x64/releasedbg/F4MPTests.exe" "$@" \
		2> >(grep -v '^msync:' >&2) | tr -d '\r'
}

case ${1:-} in
deploy) shift; deploy "$@" ;;
restart) shift; restart "$@" ;;
dev) shift; dev "$@" ;;
test) shift; run_tests "$@" ;;
*) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
