#!/bin/bash
# Builds the plugin's own menu movies (interface/<Name>/<Name>.as) into build/interface/<Name>.swf with the pinned
# compiler (tools/flash/fetch.sh, run first if it is missing). The game's movies are SWF 15 (Flash Player 11.2), and
# so are ours: the flash.* API comes from playerglobal 32.0 as an external library, so nothing of Adobe's is in the
# movie, and only what 11.2 had may be used.
#
#   tools/flash/build.sh            # every movie
#   tools/flash/build.sh F4MPMenu   # one
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
flex=$("$root/tools/flash/fetch.sh" | sed -n 's/^FLEX_HOME=//p')
playerglobal=$flex/frameworks/libs/player/32.0/playerglobal.swc
out=$root/build/interface
mkdir -p "$out"

names=("$@")
if [ ${#names[@]} -eq 0 ]; then
	for dir in "$root"/interface/*/; do
		names+=("$(basename "$dir")")
	done
fi

for name in "${names[@]}"; do
	src=$root/interface/$name
	java -Dsun.io.useCanonCaches=false -Djava.util.Arrays.useLegacyMergeSort=true -jar "$flex/lib/mxmlc.jar" \
		+flexlib="$flex/frameworks" -target-player=32.0 -swf-version=15 -static-link-runtime-shared-libraries=true \
		-compiler.library-path= -compiler.external-library-path="$playerglobal" -compiler.source-path="$src" \
		-default-size 1280 720 -default-frame-rate 60 -default-background-color 0x000000 \
		-warnings=true -output "$out/$name.swf" "$src/$name.as"
done
