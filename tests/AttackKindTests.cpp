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

TEST("AttackKind: a stand-in's own bullet after a replayed shot is kept from the engine, so it hits nobody")
{
	using AttackKind::Event;
	using namespace std::chrono_literals;
	// The firing animation's "WeaponFire" soon after the replay: held, for an automatic gun's run of shots too.
	CHECK(AttackKind::HoldsOwnFire(Event::kFire, false, 0ms));
	CHECK(AttackKind::HoldsOwnFire(Event::kFire, false, 120ms));
	CHECK(AttackKind::HoldsOwnFire(Event::kFire, false, AttackKind::HOLD_FIRE_FOR));
	// A throw's release is ThrowReplays' to hold; a swing fires nothing.
	CHECK(!AttackKind::HoldsOwnFire(Event::kFire, true, 120ms));
	CHECK(!AttackKind::HoldsOwnFire(Event::kSwing, false, 120ms));
	// Long after the last replay (or a clock gone backwards): not a replay's.
	CHECK(!AttackKind::HoldsOwnFire(Event::kFire, false, AttackKind::HOLD_FIRE_FOR + 1ms));
	CHECK(!AttackKind::HoldsOwnFire(Event::kFire, false, -1ms));
}

TEST("attack kind: a stand-in's shot that would go off or fly straight is held from the engine, a stream or a harpoon leaves the gun")
{
	using AttackKind::HoldsShotOf;
	// A bullet (with no motion type set too) and a laser's beam: held, they would hit our player.
	CHECK(HoldsShotOf(true, 0, false));
	CHECK(HoldsShotOf(true, AttackKind::PROJECTILE_MOTION_BEAM, false));
	CHECK(HoldsShotOf(true, AttackKind::PROJECTILE_MOTION_MISSILE, false));  // most bullets: the missile motion, no blast
	// Whatever goes off: held, its blast comes as the shooter's report, once (fired here too, it went off twice).
	CHECK(HoldsShotOf(true, AttackKind::PROJECTILE_EXPLOSION, false));
	CHECK(HoldsShotOf(true, 0, true));
	CHECK(HoldsShotOf(true, AttackKind::PROJECTILE_MOTION_MISSILE | AttackKind::PROJECTILE_EXPLOSION, true));  // a missile, a mini nuke
	CHECK(HoldsShotOf(true, 0x20000 | AttackKind::PROJECTILE_EXPLOSION, true));                            // a grenade launcher's shell
	CHECK(HoldsShotOf(true, AttackKind::PROJECTILE_MOTION_BEAM, true));                                     // a beam with a blast: it lands within the frame
	CHECK(HoldsShotOf(true, 0x80000, true));                                                              // an enchanted gun's stream (a legendary Explosive)
	// What neither goes off nor flies straight: a lobbed one with no blast, a flamer's or cryolator's stream, a cone, a
	// barrier, a harpoon: fired.
	CHECK(!HoldsShotOf(true, 0x20000, false));
	CHECK(!HoldsShotOf(true, 0x80000, false));
	CHECK(!HoldsShotOf(true, 0x100000, false));
	CHECK(!HoldsShotOf(true, 0x200000, false));
	CHECK(!HoldsShotOf(true, 0x400000, false));
	// What the gun fires unknown: held, as every shot was.
	CHECK(HoldsShotOf(false, 0x80000, false));
	CHECK(HoldsShotOf(false, AttackKind::PROJECTILE_EXPLOSION, true));
}
