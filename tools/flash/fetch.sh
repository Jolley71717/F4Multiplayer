#!/bin/bash
# The ActionScript 3 compiler for the plugin's own menu panels (Interface/F4MP/*.swf): Apache Flex SDK's mxmlc and compc,
# prebuilt, and Adobe's playerglobal.swc (the flash.* API they compile against). Both are pinned by SHA-256 and unpacked
# outside the repo; neither is committed (playerglobal.swc is Adobe's, not ours to redistribute).
#
#   tools/flash/fetch.sh            # into ~/.flex/4.16.1 (FLEX_HOME), once; again only if a file is missing
#   FLEX_ROOT=/elsewhere tools/flash/fetch.sh
#
# Needs curl, tar and shasum; the compiler needs Java. The Flex tarball's checksum was pinned after its Apache release
# signature checked out (gpg --verify against https://archive.apache.org/dist/flex/KEYS, key 44998F3E...61FC8).
#
# playerglobal: Adobe's CDN still serves only version 32.0. The game's own movies are SWF 15 (Flash Player 11.2), so
# panels are built with -swf-version=15 against it and use only what 11.2 had (Scaleform GFx 4 knows no more).
set -euo pipefail

FLEX_VERSION=4.16.1
FLEX_URL=https://archive.apache.org/dist/flex/$FLEX_VERSION/binaries/apache-flex-sdk-$FLEX_VERSION-bin.tar.gz
FLEX_SHA256=17fda7ac8d3e476cad3127f345455ef316acfb87c6f4322e5897bd8d9b09388e
PLAYERGLOBAL_VERSION=32.0
PLAYERGLOBAL_URL=https://fpdownload.macromedia.com/get/flashplayer/updaters/32/playerglobal32_0.swc
PLAYERGLOBAL_SHA256=7d4d6168d27603cfb3b750302448e354e0bbc1bdd58f5d101c3dcf6891e9bb65

root=${FLEX_ROOT:-$HOME/.flex}
home=$root/$FLEX_VERSION
playerglobal=$home/frameworks/libs/player/$PLAYERGLOBAL_VERSION/playerglobal.swc

if [ -f "$home/lib/mxmlc.jar" ] && [ -f "$home/lib/compc.jar" ] && [ -f "$playerglobal" ]; then
	echo "FLEX_HOME=$home"
	exit 0
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fetch() {  # url sha256 file
	curl -sSLf --retry 3 -o "$3" "$1"
	local got
	got=$(shasum -a 256 "$3" | cut -d' ' -f1)
	if [ "$got" != "$2" ]; then
		echo "checksum mismatch for $1: expected $2, got $got" >&2
		exit 1
	fi
}

fetch "$FLEX_URL" "$FLEX_SHA256" "$work/flex.tar.gz"
fetch "$PLAYERGLOBAL_URL" "$PLAYERGLOBAL_SHA256" "$work/playerglobal.swc"

mkdir -p "$work/sdk"
tar xzf "$work/flex.tar.gz" -C "$work/sdk" --strip-components 1
mkdir -p "$(dirname "$work/sdk/frameworks/libs/player/$PLAYERGLOBAL_VERSION/playerglobal.swc")"
cp -f "$work/playerglobal.swc" "$work/sdk/frameworks/libs/player/$PLAYERGLOBAL_VERSION/playerglobal.swc"

mkdir -p "$root"
rm -rf "$home"
mv "$work/sdk" "$home"
echo "FLEX_HOME=$home"
