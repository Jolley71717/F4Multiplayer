#include "Test.h"
#include "game/AttackKind.h"

#include <vector>

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

TEST("AttackKind: a grenade listed before the gun does not make a friend's shots swings")
{
	constexpr std::int32_t grenade = 10;
	constexpr std::int32_t mine = 11;
	constexpr std::int32_t sword = 1;
	// The engine lists what an actor has equipped in its own order; a grenade can come first.
	CHECK(AttackKind::HoldsGun(std::vector<std::int32_t>{ grenade, AttackKind::GUN_TYPE }));
	CHECK(AttackKind::HoldsGun(std::vector<std::int32_t>{ AttackKind::GUN_TYPE, grenade }));
	CHECK(AttackKind::HoldsGun(std::vector<std::int32_t>{ mine, AttackKind::GUN_TYPE }));
	CHECK(AttackKind::ReplayShot(AttackKind::HoldsGun(std::vector<std::int32_t>{ grenade, AttackKind::GUN_TYPE }), true) == AttackKind::Replay::kFire);
	CHECK(AttackKind::ReplayShot(AttackKind::HoldsGun(std::vector<std::int32_t>{ grenade, AttackKind::GUN_TYPE }), false) == AttackKind::Replay::kSoundOnly);
	// No gun among them: melee, or fists.
	CHECK(!AttackKind::HoldsGun(std::vector<std::int32_t>{ grenade }));
	CHECK(!AttackKind::HoldsGun(std::vector<std::int32_t>{ grenade, sword }));
	CHECK(!AttackKind::HoldsGun(std::vector<std::int32_t>{}));
	CHECK(AttackKind::ReplayShot(AttackKind::HoldsGun(std::vector<std::int32_t>{ grenade, sword }), true) == AttackKind::Replay::kSwing);
}

TEST("AttackKind: a gun's bash is not reported as a shot, so friends hear no gunshot for it")
{
	using AttackKind::Event;
	// A shot is always reported.
	CHECK(AttackKind::ReportsAsShot(Event::kFire, true));
	CHECK(AttackKind::ReportsAsShot(Event::kFire, false));
	// The hit frame of a swing or a punch is, with no gun in hand: the stand-in swings (ReplayShot's kSwing).
	CHECK(AttackKind::ReportsAsShot(Event::kSwing, false));
	// The same event with a gun in hand is a bash: reported, a friend holding a gun replays it as a shot.
	CHECK(AttackKind::ReplayShot(true, true) == AttackKind::Replay::kFire);
	CHECK(!AttackKind::ReportsAsShot(Event::kSwing, true));
	CHECK(!AttackKind::ReportsAsShot(Event::kSwing, AttackKind::HoldsGun(std::vector<std::int32_t>{ 10, AttackKind::GUN_TYPE })));
}
