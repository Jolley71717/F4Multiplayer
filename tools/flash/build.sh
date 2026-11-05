#!/bin/bash
# Builds the plugin's own menu movies: ui/<Name>/<Name>.as, with the shared ui/shared, into dist/movies/<Name>.swf, which
# git ignores: movies are never committed (docs/UI.md), and releases compile them on GitHub (docs/RELEASING.md). Uses the
# pinned compiler (tools/flash/fetch.sh, run first if it is missing). The game's movies are SWF 15 (Flash Player 11.2),
# and so are ours: the flash.* API comes from playerglobal 32.0 as an external library, so nothing of Adobe's is in the
# movie, and only what 11.2 had may be used. Then dist/movies/<Name>.sources records what it was built from (each .as of
# ui/<Name> and ui/shared, with the SHA-256 of its text with LF line endings). mxmlc writes its compile time into the
# movie (the last 8 bytes of its ProductInfo tag): cleared, as build.ps1 does, so the same sources give the same bytes
# on a Mac and on Windows.
#
#   tools/flash/build.sh            # every movie
#   tools/flash/build.sh MPMenu     # one
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
flex=$("$root/tools/flash/fetch.sh" | sed -n 's/^FLEX_HOME=//p')
playerglobal=$flex/frameworks/libs/player/32.0/playerglobal.swc
movies=$root/dist/movies
mkdir -p "$movies"

names=("$@")
if [ ${#names[@]} -eq 0 ]; then
	for dir in "$root"/ui/*/; do
		name=$(basename "$dir")
		[ "$name" = shared ] || names+=("$name")
	done
fi

# The compile time in the ProductInfo tag (code 41: ids, version and build, then the date, 8 bytes), as build.ps1 clears it.
clear_compile_time() {
	perl -e '
		use strict;
		my $path = shift;
		open(my $in, "<:raw", $path) or die "$path: $!\n";
		local $/;
		my $data = <$in>;
		close $in;
		substr($data, 0, 3) eq "FWS" or die "$path: not an uncompressed movie\n";
		my $bits = ord(substr($data, 8, 1)) >> 3;
		my $at = 8 + int((5 + $bits * 4 + 7) / 8) + 4;
		my $stamped = 0;
		while ($at < length $data) {
			my $header = unpack("v", substr($data, $at, 2));
			$at += 2;
			my ($code, $length) = ($header >> 6, $header & 0x3F);
			if ($length == 0x3F) { $length = unpack("V", substr($data, $at, 4)); $at += 4; }
			if ($code == 41 && $length >= 26) { substr($data, $at + $length - 8, 8) = "\0" x 8; $stamped = 1; }
			$at += $length;
			last if $code == 0;
		}
		$stamped or die "$path: no ProductInfo tag to clear\n";
		open(my $out, ">:raw", $path) or die "$path: $!\n";
		print $out $data;
		close $out;
	' "$1"
}

if command -v sha256sum > /dev/null; then sha() { sha256sum | cut -d' ' -f1; }; else sha() { shasum -a 256 | cut -d' ' -f1; }; fi

for name in "${names[@]}"; do
	src=$root/ui/$name
	java -Dsun.io.useCanonCaches=false -Djava.util.Arrays.useLegacyMergeSort=true -jar "$flex/lib/mxmlc.jar" \
		+flexlib="$flex/frameworks" -target-player=32.0 -swf-version=15 -compress=false -static-link-runtime-shared-libraries=true \
		-compiler.library-path= -compiler.external-library-path="$playerglobal" -compiler.source-path="$src" -compiler.source-path+="$root/ui/shared" \
		-default-size 1280 720 -default-frame-rate 60 -default-background-color 0x000000 \
		-warnings=true -output "$movies/$name.swf" "$src/$name.as"
	clear_compile_time "$movies/$name.swf"
	{
		echo "# The sources $name.swf was built from (tools/flash/build.sh), each with the SHA-256 of its text with LF line endings."
		(cd "$root/ui" && find "$name" shared -name '*.as' -type f | LC_ALL=C sort | while read -r path; do
			echo "$(tr -d '\r' < "$path" | sha)  $path"
		done)
	} > "$movies/$name.sources"
done
