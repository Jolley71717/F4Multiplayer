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

TEST("attack kind: a stand-in's shot that would go off, or a plain bullet or beam with the trails on, flies inert when the inert launch is in place, else it is held; a stream or a harpoon leaves the gun")
{
	using AttackKind::Fire;
	using AttackKind::ShotReplay;
	constexpr bool standIn = true, copy = false, known = true, inert = true, trails = true;
	constexpr std::uint32_t missile = AttackKind::PROJECTILE_MOTION_MISSILE | AttackKind::PROJECTILE_EXPLOSION;  // a missile, a mini nuke
	constexpr std::uint32_t shell = 0x20000 | AttackKind::PROJECTILE_EXPLOSION;                                   // a grenade launcher's shell
	// A stand-in's plain bullet (no motion type set too, or the missile motion) and a laser's beam: launched inert with the
	// inert launch in place and the trails on, so friends see them fly; held with either missing. A copy's is the
	// engine's, as before.
	for (const std::uint32_t plain : { 0u, AttackKind::PROJECTILE_MOTION_MISSILE, AttackKind::PROJECTILE_MOTION_BEAM }) {
		CHECK(ShotReplay(standIn, known, plain, false, false, false, inert, trails) == Fire::kInert);
		CHECK(ShotReplay(standIn, known, plain, false, false, false, !inert, trails) == Fire::kHeld);
		CHECK(ShotReplay(standIn, known, plain, false, false, false, inert, !trails) == Fire::kHeld);
		CHECK(ShotReplay(standIn, known, plain, false, false, false, !inert, !trails) == Fire::kHeld);
		CHECK(ShotReplay(copy, known, plain, false, false, false, inert, trails) == Fire::kFired);
		CHECK(ShotReplay(copy, known, plain, false, false, false, inert, !trails) == Fire::kFired);
		CHECK(ShotReplay(standIn, !known, plain, false, false, false, inert, trails) == Fire::kHeld);  // the record unknown: held whatever
		CHECK(ShotReplay(standIn, known, plain, false, true, false, inert, trails) == Fire::kHeld);   // an enchantment that explodes: no record to take it from
	}
	// The trails toggle touches nothing that goes off, and nothing that was already fired.
	CHECK(ShotReplay(standIn, known, missile, true, false, false, inert, !trails) == Fire::kInert);
	CHECK(ShotReplay(copy, known, missile, true, false, false, inert, !trails) == Fire::kInert);
	CHECK(ShotReplay(standIn, known, 0x80000, false, false, false, inert, !trails) == Fire::kFired);
	CHECK(ShotReplay(copy, known, AttackKind::PROJECTILE_MOTION_BEAM, false, false, false, inert, trails) == Fire::kFired);
	// Whatever goes off by its projectile: launched from a record with no blast, for a stand-in and a copy alike, so
	// friends see it fly; its blast comes as the shooter's report, once.
	CHECK(ShotReplay(standIn, known, missile, true, false, false, inert, trails) == Fire::kInert);
	CHECK(ShotReplay(copy, known, missile, true, false, false, inert, trails) == Fire::kInert);
	CHECK(ShotReplay(standIn, known, shell, true, false, false, inert, trails) == Fire::kInert);
	CHECK(ShotReplay(standIn, known, AttackKind::PROJECTILE_MOTION_BEAM, true, false, false, inert, trails) == Fire::kInert);  // a beam with a blast: it lands within the frame, inert from the start
	CHECK(ShotReplay(copy, known, AttackKind::PROJECTILE_EXPLOSION, true, false, false, inert, trails) == Fire::kInert);        // an Explosive round's own record
	CHECK(ShotReplay(standIn, known, missile, true, true, false, inert, trails) == Fire::kInert);                               // enchanted as well: the record's blast is the one taken
	// With no inert launch (the engine's entry not as 1.11.240 has it): held, as every such shot was (fired as it is,
	// it went off twice).
	CHECK(ShotReplay(standIn, known, missile, true, false, false, !inert, trails) == Fire::kHeld);
	CHECK(ShotReplay(copy, known, missile, true, false, false, !inert, trails) == Fire::kHeld);
	CHECK(ShotReplay(copy, known, AttackKind::PROJECTILE_MOTION_BEAM, true, false, false, !inert, trails) == Fire::kHeld);
	// One that goes off by an enchantment alone (no game legendary does): held, there is no record to take the blast from.
	CHECK(ShotReplay(standIn, known, 0x80000, false, true, false, inert, trails) == Fire::kHeld);
	CHECK(ShotReplay(copy, known, AttackKind::PROJECTILE_MOTION_MISSILE, false, true, false, inert, trails) == Fire::kHeld);
	// What neither goes off nor flies straight: a lobbed one with no blast, a flamer's or cryolator's stream, a cone, a
	// barrier, a harpoon: fired.
	for (const std::uint32_t motion : { 0x20000u, 0x80000u, 0x100000u, 0x200000u, 0x400000u }) {
		CHECK(ShotReplay(standIn, known, motion, false, false, false, inert, trails) == Fire::kFired);
		CHECK(ShotReplay(copy, known, motion, false, false, false, inert, trails) == Fire::kFired);
	}
	// What the gun fires unknown: a stand-in's held, as every shot was; a copy's the engine's, as before.
	CHECK(ShotReplay(standIn, !known, 0x80000, false, false, false, inert, trails) == Fire::kHeld);
	CHECK(ShotReplay(standIn, !known, AttackKind::PROJECTILE_EXPLOSION, true, false, false, inert, trails) == Fire::kHeld);
	CHECK(ShotReplay(copy, !known, 0, false, false, false, inert, trails) == Fire::kFired);
	CHECK(ShotReplay(copy, !known, 0, false, true, false, inert, trails) == Fire::kHeld);
}

TEST("attack kind: a stand-in's plain bullet or beam from an enchanted gun or of a hitscan record is held, trails or not; nothing else changes for them")
{
	using AttackKind::Fire;
	using AttackKind::ShotReplay;
	constexpr bool standIn = true, copy = false, known = true, inert = true, trails = true, enchanted = true;
	constexpr std::uint32_t hitscan = AttackKind::PROJECTILE_HITSCAN;
	constexpr std::uint32_t missile = AttackKind::PROJECTILE_MOTION_MISSILE | AttackKind::PROJECTILE_EXPLOSION;
	for (const std::uint32_t plain : { 0u, AttackKind::PROJECTILE_MOTION_MISSILE, AttackKind::PROJECTILE_MOTION_BEAM, 0x40A0Cu, 0x40A08u }) {
		// A legendary (Wounding, Freezing, Poisoner's) on the stand-in's gun: held as before the trails, whatever the toggle.
		CHECK(ShotReplay(standIn, known, plain, false, false, enchanted, inert, trails) == Fire::kHeld);
		CHECK(ShotReplay(standIn, known, plain, false, false, enchanted, inert, !trails) == Fire::kHeld);
		CHECK(ShotReplay(standIn, known, plain, false, false, enchanted, !inert, trails) == Fire::kHeld);
		// The hitscan flag: held (never handed to the engine as it is, which a real round of the stand-in's was).
		CHECK(ShotReplay(standIn, known, plain | hitscan, false, false, false, inert, trails) == Fire::kHeld);
		CHECK(ShotReplay(standIn, known, plain | hitscan, false, false, false, inert, !trails) == Fire::kHeld);
		// A copy's plain shot stays the engine's, enchanted or hitscan (as before the trails).
		CHECK(ShotReplay(copy, known, plain, false, false, enchanted, inert, trails) == Fire::kFired);
		CHECK(ShotReplay(copy, known, plain | hitscan, false, false, false, inert, trails) == Fire::kFired);
	}
	// The game's own bullet and laser records (no hitscan flag), unenchanted: still trails.
	CHECK(ShotReplay(standIn, known, 0x40A0C, false, false, false, inert, trails) == Fire::kInert);
	CHECK(ShotReplay(standIn, known, 0x40A08, false, false, false, inert, trails) == Fire::kInert);
	// What goes off is launched inert as before, enchanted (a Wounding missile launcher) or not.
	CHECK(ShotReplay(standIn, known, missile, true, false, enchanted, inert, trails) == Fire::kInert);
	CHECK(ShotReplay(copy, known, missile | hitscan, true, false, enchanted, inert, trails) == Fire::kInert);
	// A stream or a harpoon from an enchanted gun: fired as before.
	CHECK(ShotReplay(standIn, known, 0x80000, false, false, enchanted, inert, trails) == Fire::kFired);
}

TEST("attack kind: an enchantment that acts on the wielder (an armor-piercing receiver, a recon scope) holds no trail; one that lands on a hit does")
{
	using AttackKind::EnchantmentOnHit;
	using AttackKind::Fire;
	using AttackKind::ShotReplay;
	// The engine's casting types (constant, fire and forget, concentration, scroll) and deliveries (self, touch, aimed,
	// target actor, target location), as Fallout4.esm's enchantment records give them.
	constexpr std::int32_t constant = 0, fireAndForget = 1, concentration = 2, scroll = 3;
	constexpr std::int32_t self = 0, touch = 1, aimed = 2, targetActor = 3, targetLocation = 4;
	// enchModArmorPenetration (the 10mm's, the assault rifle's and the combat shotgun's armor-piercing receivers),
	// EnchReconScopeEquipped, a Bloodied or Nocturnal legendary: constant, self. They act on the one holding the gun.
	CHECK(!EnchantmentOnHit(constant, self));
	// A legendary's bleed, poison or cryo, a melee mod's shock, a laser receiver's fire: fire and forget, by touch.
	CHECK(EnchantmentOnHit(fireAndForget, touch));
	// Anything else is taken to reach what the shot hits, as before.
	for (const std::int32_t delivery : { touch, aimed, targetActor, targetLocation }) {
		CHECK(EnchantmentOnHit(constant, delivery));
	}
	for (const std::int32_t cast : { fireAndForget, concentration, scroll }) {
		CHECK(EnchantmentOnHit(cast, self));
	}
	// The trails step's 10mm with an armor-piercing automatic receiver (its bullet 0x40A0C): its gun counts as having no
	// enchantment that lands on a hit, so its shot flies inert as a trail instead of being held.
	constexpr bool standIn = true, known = true, inert = true, trails = true;
	CHECK(ShotReplay(standIn, known, 0x40A0C, false, false, EnchantmentOnHit(constant, self), inert, trails) == Fire::kInert);
	CHECK(ShotReplay(standIn, known, 0x40A0C, false, false, EnchantmentOnHit(fireAndForget, touch), inert, trails) == Fire::kHeld);
}
