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
