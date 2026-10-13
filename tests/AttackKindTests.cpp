#include "Test.h"
#include "game/AttackKind.h"

TEST("AttackKind: a gun replays through the animation graph")
{
	CHECK(AttackKind::ReplaysAsGun(AttackKind::GUN_TYPE, true));
}

TEST("AttackKind: melee weapons perform the attack action")
{
	for (std::int32_t type = 0; type < 9; ++type) {  // hand to hand, swords, axes, ... (everything but a gun)
		CHECK(!AttackKind::ReplaysAsGun(type, true));
	}
	CHECK(!AttackKind::ReplaysAsGun(10, true));  // grenade
}

TEST("AttackKind: fists are melee")
{
	CHECK(!AttackKind::ReplaysAsGun(AttackKind::GUN_TYPE, false));
	CHECK(!AttackKind::ReplaysAsGun(0, false));
}

TEST("AttackKind: a friend's gunshot is heard while their stand-in's gun is put away; only the firing animation waits for it")
{
	using AttackKind::Replay;
	CHECK(AttackKind::ReplayShot(true, true) == Replay::kFire);
	CHECK(AttackKind::ReplayShot(true, false) == Replay::kSoundOnly);
	// A swing or a punch needs the weapon out: asking the engine for one with it put away would draw it first.
	CHECK(AttackKind::ReplayShot(false, true) == Replay::kSwing);
	CHECK(AttackKind::ReplayShot(false, false) == Replay::kNone);
}
