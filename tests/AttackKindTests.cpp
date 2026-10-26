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

TEST("attack kind: a stand-in's shot that would go off flies inert when the inert launch is in place, else it is held; a plain one is held, a stream or a harpoon leaves the gun")
{
	using AttackKind::Fire;
	using AttackKind::ShotReplay;
	constexpr bool standIn = true, copy = false, known = true, inert = true;
	constexpr std::uint32_t missile = AttackKind::PROJECTILE_MOTION_MISSILE | AttackKind::PROJECTILE_EXPLOSION;  // a missile, a mini nuke
	constexpr std::uint32_t shell = 0x20000 | AttackKind::PROJECTILE_EXPLOSION;                                   // a grenade launcher's shell
	// A bullet (with no motion type set too) and a laser's beam: a stand-in's held, they would hit our player; a copy's
	// is the engine's, as before.
	CHECK(ShotReplay(standIn, known, 0, false, false, inert) == Fire::kHeld);
	CHECK(ShotReplay(standIn, known, AttackKind::PROJECTILE_MOTION_BEAM, false, false, inert) == Fire::kHeld);
	CHECK(ShotReplay(standIn, known, AttackKind::PROJECTILE_MOTION_MISSILE, false, false, inert) == Fire::kHeld);  // most bullets: the missile motion, no blast
	CHECK(ShotReplay(copy, known, AttackKind::PROJECTILE_MOTION_MISSILE, false, false, inert) == Fire::kFired);
	CHECK(ShotReplay(copy, known, AttackKind::PROJECTILE_MOTION_BEAM, false, false, inert) == Fire::kFired);
	// Whatever goes off by its projectile: launched from a record with no blast, for a stand-in and a copy alike, so
	// friends see it fly; its blast comes as the shooter's report, once.
	CHECK(ShotReplay(standIn, known, missile, true, false, inert) == Fire::kInert);
	CHECK(ShotReplay(copy, known, missile, true, false, inert) == Fire::kInert);
	CHECK(ShotReplay(standIn, known, shell, true, false, inert) == Fire::kInert);
	CHECK(ShotReplay(standIn, known, AttackKind::PROJECTILE_MOTION_BEAM, true, false, inert) == Fire::kInert);  // a beam with a blast: it lands within the frame, inert from the start
	CHECK(ShotReplay(copy, known, AttackKind::PROJECTILE_EXPLOSION, true, false, inert) == Fire::kInert);        // an Explosive round's own record
	CHECK(ShotReplay(standIn, known, missile, true, true, inert) == Fire::kInert);                               // enchanted as well: the record's blast is the one taken
	// With no inert launch (the engine's entry not as 1.11.240 has it): held, as every such shot was (fired as it is,
	// it went off twice).
	CHECK(ShotReplay(standIn, known, missile, true, false, !inert) == Fire::kHeld);
	CHECK(ShotReplay(copy, known, missile, true, false, !inert) == Fire::kHeld);
	CHECK(ShotReplay(copy, known, AttackKind::PROJECTILE_MOTION_BEAM, true, false, !inert) == Fire::kHeld);
	// One that goes off by an enchantment alone (no game legendary does): held, there is no record to take the blast from.
	CHECK(ShotReplay(standIn, known, 0x80000, false, true, inert) == Fire::kHeld);
	CHECK(ShotReplay(copy, known, AttackKind::PROJECTILE_MOTION_MISSILE, false, true, inert) == Fire::kHeld);
	// What neither goes off nor flies straight: a lobbed one with no blast, a flamer's or cryolator's stream, a cone, a
	// barrier, a harpoon: fired.
	for (const std::uint32_t motion : { 0x20000u, 0x80000u, 0x100000u, 0x200000u, 0x400000u }) {
		CHECK(ShotReplay(standIn, known, motion, false, false, inert) == Fire::kFired);
		CHECK(ShotReplay(copy, known, motion, false, false, inert) == Fire::kFired);
	}
	// What the gun fires unknown: a stand-in's held, as every shot was; a copy's the engine's, as before.
	CHECK(ShotReplay(standIn, !known, 0x80000, false, false, inert) == Fire::kHeld);
	CHECK(ShotReplay(standIn, !known, AttackKind::PROJECTILE_EXPLOSION, true, false, inert) == Fire::kHeld);
	CHECK(ShotReplay(copy, !known, 0, false, false, inert) == Fire::kFired);
	CHECK(ShotReplay(copy, !known, 0, false, true, inert) == Fire::kHeld);
}
