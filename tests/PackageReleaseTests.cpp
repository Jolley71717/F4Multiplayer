#include "Test.h"

#include "game/PackageRelease.h"

// Which package an NPC we stop mirroring is let go of (Puppets::ReleaseNpc): the do-nothing package a mirror is given,
// which evp keeps for good, and nothing from the game's data.
namespace
{
	constexpr std::int8_t SANDBOX = 18;  // what a Diamond City resident runs on its own
}

TEST("package release: the do-nothing package a mirror is given is let go of")
{
	// As read on the rig from an NPC handed over to the client's game: package=FF00xxxx packType=27.
	CHECK(PackageRelease::IsMirrorsDoNothing(PackageRelease::DO_NOTHING, 0xFF000A1Cu));
	CHECK(PackageRelease::IsMirrorsDoNothing(27, 0xFF000001u));
	CHECK(PackageRelease::IsMirrorsDoNothing(27, 0xFFFFFFFFu));
}

TEST("package release: the NPC's own package is kept")
{
	// Its sandbox package, from Fallout4.esm, as resetai gave one back.
	CHECK(!PackageRelease::IsMirrorsDoNothing(SANDBOX, 0x0005BC91u));
	// A do-nothing package from a plugin (a quest's or a scene's) is the NPC's own, not ours.
	CHECK(!PackageRelease::IsMirrorsDoNothing(PackageRelease::DO_NOTHING, 0x0005BC91u));
	CHECK(!PackageRelease::IsMirrorsDoNothing(PackageRelease::DO_NOTHING, 0xFE012345u));
	// Another package the engine made (a flee, a dialogue) is the engine's.
	CHECK(!PackageRelease::IsMirrorsDoNothing(SANDBOX, 0xFF000A1Cu));
	CHECK(!PackageRelease::IsMirrorsDoNothing(0, 0xFF000A1Cu));
}

TEST("package release: only forms made while the game runs count as made")
{
	CHECK(PackageRelease::IsMadeForm(0xFF000000u));
	CHECK(!PackageRelease::IsMadeForm(0xFE000800u));  // a light plugin's
	CHECK(!PackageRelease::IsMadeForm(0x00000014u));
}
