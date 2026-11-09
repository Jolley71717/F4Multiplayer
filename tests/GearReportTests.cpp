// Our player's gear report (game/GearReport.h): the hand weapons it names are the ones the engine holds, not every stack
// the inventory flags equipped. With two copies of a 10mm, a switch to the laser rifle left a stale flag on a 10mm stack,
// and the report named both guns for the rest of the session: every friend's game dressed the stand-in again and again,
// putting its gun away each time.

#include "Test.h"

#include "game/GearReport.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace
{
	using GearReport::HandsAsHeld;
	using Items = std::vector<std::uint32_t>;

	constexpr std::uint32_t PISTOL_10MM = 0x00004822;
	constexpr std::uint32_t LASER_RIFLE = 0x0009983B;
	constexpr std::uint32_t KNIFE = 0x0000E6E9;        // a melee weapon: held in the hand like a gun
	constexpr std::uint32_t FRAG = 0x0000E56B;         // a grenade: flagged equipped while one is carried
	constexpr std::uint32_t MINE = 0x000E56C2;         // a mine: the same
	constexpr std::uint32_t VAULT_SUIT = 0x0001EED7;
	constexpr std::uint32_t PIP_BOY = 0x00021B3B;
	constexpr std::uint32_t T60_HELMET = 0x00154ABD;   // power armor pieces and the frame item, worn in a frame
	constexpr std::uint32_t T60_TORSO = 0x00154AC1;
	constexpr std::uint32_t FRAME = 0x00031CE5;

	bool InHand(std::uint32_t a_item)
	{
		return a_item == PISTOL_10MM || a_item == LASER_RIFLE || a_item == KNIFE;
	}
}

TEST("gear report: a gun flagged equipped beside the one the engine holds is left out (the stale 10mm after a switch to the laser)")
{
	const Items flagged{ PISTOL_10MM, VAULT_SUIT, PIP_BOY, LASER_RIFLE };
	CHECK((HandsAsHeld(flagged, Items{ LASER_RIFLE }, InHand) == Items{ VAULT_SUIT, PIP_BOY, LASER_RIFLE }));
	// And the other way round: back on the 10mm, a stale laser flag.
	CHECK((HandsAsHeld(flagged, Items{ PISTOL_10MM }, InHand) == Items{ PISTOL_10MM, VAULT_SUIT, PIP_BOY }));
}

TEST("gear report: one gun held and flagged is said as it always was, and so is the rest of the list")
{
	const Items flagged{ PISTOL_10MM, VAULT_SUIT, PIP_BOY };
	CHECK(HandsAsHeld(flagged, Items{ PISTOL_10MM }, InHand) == flagged);
	const Items noGun{ VAULT_SUIT, PIP_BOY };
	CHECK(HandsAsHeld(noGun, Items{}, InHand) == noGun);
	CHECK(HandsAsHeld(Items{}, Items{ PISTOL_10MM }, InHand) == Items{});
}

TEST("gear report: a grenade or mine stays in the report whether or not the engine lists it, and never displaces a gun")
{
	const Items flagged{ PISTOL_10MM, FRAG, MINE, LASER_RIFLE };
	CHECK((HandsAsHeld(flagged, Items{ LASER_RIFLE }, InHand) == Items{ FRAG, MINE, LASER_RIFLE }));
	CHECK((HandsAsHeld(flagged, Items{ LASER_RIFLE, FRAG }, InHand) == Items{ FRAG, MINE, LASER_RIFLE }));
	// Only a grenade held by the engine: no hand weapon is, so the guns are said as the inventory flags them.
	CHECK(HandsAsHeld(flagged, Items{ FRAG }, InHand) == flagged);
	const Items onlyGrenade{ FRAG, VAULT_SUIT };
	CHECK(HandsAsHeld(onlyGrenade, Items{}, InHand) == onlyGrenade);
}

TEST("gear report: a melee weapon is a hand weapon like a gun")
{
	CHECK((HandsAsHeld(Items{ KNIFE, LASER_RIFLE, VAULT_SUIT }, Items{ KNIFE }, InHand) == Items{ KNIFE, VAULT_SUIT }));
	CHECK((HandsAsHeld(Items{ KNIFE, LASER_RIFLE, VAULT_SUIT }, Items{ LASER_RIFLE }, InHand) == Items{ LASER_RIFLE, VAULT_SUIT }));
}

TEST("gear report: when the engine holds none of the flagged weapons, or can't be read, the inventory's flags are said as before")
{
	// Unarmed with a stale flag, or a switch in flight (the inventory flags the new gun, the engine still has the old):
	// nothing is dropped, so a gun never vanishes from a report for a moment.
	const Items flagged{ PISTOL_10MM, VAULT_SUIT };
	CHECK(HandsAsHeld(flagged, Items{}, InHand) == flagged);
	CHECK(HandsAsHeld(flagged, Items{ LASER_RIFLE }, InHand) == flagged);
	// No middle-high process to read: nothing to go by.
	const Items both{ PISTOL_10MM, VAULT_SUIT, LASER_RIFLE };
	CHECK(HandsAsHeld(both, std::nullopt, InHand) == both);
}

TEST("gear report: power armor pieces and the frame are never touched; only the stale gun goes")
{
	const Items flagged{ PISTOL_10MM, FRAME, LASER_RIFLE, T60_HELMET, T60_TORSO };
	CHECK((HandsAsHeld(flagged, Items{ LASER_RIFLE }, InHand) == Items{ FRAME, LASER_RIFLE, T60_HELMET, T60_TORSO }));
	const Items unarmed{ FRAME, T60_HELMET, T60_TORSO };
	CHECK(HandsAsHeld(unarmed, Items{}, InHand) == unarmed);
}

TEST("gear report: the sorted list stays sorted")
{
	const auto out = HandsAsHeld(Items{ PISTOL_10MM, FRAG, KNIFE, VAULT_SUIT, PIP_BOY, LASER_RIFLE, T60_HELMET }, Items{ LASER_RIFLE }, InHand);
	CHECK(std::ranges::is_sorted(out));
	CHECK((out == Items{ FRAG, VAULT_SUIT, PIP_BOY, LASER_RIFLE, T60_HELMET }));
}

TEST("gear report: of two copies of a gun flagged equipped, the mods said are the held copy's, else the first's as before")
{
	using GearReport::StackToRead;
	int plain = 0, modded = 0, other = 0;  // stand for three stacks' instance data
	CHECK(StackToRead({ &plain, &modded }, &modded) == 1);
	CHECK(StackToRead({ &plain, &modded }, &plain) == 0);
	CHECK(StackToRead({ &plain, &modded }, &other) == 0);   // the engine's instance matches neither: the first, as before
	CHECK(StackToRead({ &plain, &modded }, nullptr) == 0);  // nothing held, or not our player
	CHECK(StackToRead({ nullptr, &modded }, nullptr) == 0); // a stack without instance data never matches "nothing"
	CHECK(StackToRead({ &modded }, &modded) == 0);
	CHECK(StackToRead({}, &modded) == 0);
}
