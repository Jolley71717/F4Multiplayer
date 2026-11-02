#!/bin/bash
# Builds the plugin's own menu movies: ui/<Name>/<Name>.as, with the shared ui/shared, into ui/<Name>/<Name>.swf, which
# is committed (so only a change to ui/ needs Java and this compiler; building, packaging and playing don't). Uses the
# pinned compiler (tools/flash/fetch.sh, run first if it is missing). The game's movies are SWF 15 (Flash Player 11.2),
# and so are ours: the flash.* API comes from playerglobal 32.0 as an external library, so nothing of Adobe's is in the
# movie, and only what 11.2 had may be used. Then <Name>.sources records what it was built from (each .as of ui/<Name>
# and ui/shared, with the SHA-256 of its text with LF line endings), which F4MPTests checks against the sources. Commit
# all three. mxmlc writes its compile time into the movie (8 bytes of its ProductInfo tag): a rebuild that differs from
# the committed movie only there keeps the committed one, so an unchanged menu leaves git clean.
#
#   tools/flash/build.sh            # every movie
#   tools/flash/build.sh MPMenu     # one
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
flex=$("$root/tools/flash/fetch.sh" | sed -n 's/^FLEX_HOME=//p')
playerglobal=$flex/frameworks/libs/player/32.0/playerglobal.swc

names=("$@")
if [ ${#names[@]} -eq 0 ]; then
	for dir in "$root"/ui/*/; do
		name=$(basename "$dir")
		[ "$name" = shared ] || names+=("$name")
	done
fi

if command -v sha256sum > /dev/null; then sha() { sha256sum | cut -d' ' -f1; }; else sha() { shasum -a 256 | cut -d' ' -f1; }; fi

for name in "${names[@]}"; do
	src=$root/ui/$name
	before=$(mktemp)
	[ -f "$src/$name.swf" ] && cp "$src/$name.swf" "$before"
	java -Dsun.io.useCanonCaches=false -Djava.util.Arrays.useLegacyMergeSort=true -jar "$flex/lib/mxmlc.jar" \
		+flexlib="$flex/frameworks" -target-player=32.0 -swf-version=15 -compress=false -static-link-runtime-shared-libraries=true \
		-compiler.library-path= -compiler.external-library-path="$playerglobal" -compiler.source-path="$src" -compiler.source-path+="$root/ui/shared" \
		-default-size 1280 720 -default-frame-rate 60 -default-background-color 0x000000 \
		-warnings=true -output "$src/$name.swf" "$src/$name.as"
	# only the compile time differs (at most 8 bytes, all within 8 of each other): keep the committed movie
	if [ -s "$before" ] && [ "$(wc -c < "$before")" = "$(wc -c < "$src/$name.swf")" ]; then
		diffs=$(cmp -l "$before" "$src/$name.swf" | awk '{print $1}' || true)
		if [ -z "$diffs" ] || [ $(( $(echo "$diffs" | tail -1) - $(echo "$diffs" | head -1) )) -lt 8 ]; then
			cp "$before" "$src/$name.swf"
		fi
	fi
	rm -f "$before"
	{
		echo "# The sources $name.swf was built from (tools/flash/build.sh), each with the SHA-256 of its text with LF line endings."
		(cd "$root/ui" && find "$name" shared -name '*.as' -type f | LC_ALL=C sort | while read -r path; do
			echo "$(tr -d '\r' < "$path" | sha)  $path"
		done)
	} > "$src/$name.sources"
done
