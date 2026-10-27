// What holds the player's sneak key back, for the `controls` dev command (src/game/SneakKey.h).

#include "Test.h"

#include "game/SneakKey.h"

TEST("sneak key: on their feet, alive and nothing in the way, nothing holds it back")
{
	CHECK(SneakKey::Blocking({}) == "none");
	// One crippled leg is still a leg.
	CHECK(SneakKey::Blocking({ .leftLeg = 0.0f }) == "none");
	// Swimming in third person: the third-person sneak has no swim condition.
	CHECK(SneakKey::Blocking({ .swimming = true }) == "none");
}

TEST("sneak key: each gate is named, and several are named together")
{
	CHECK(SneakKey::Blocking({ .layersLetSneak = false }) == "layer");
	CHECK(SneakKey::Blocking({ .handlerOn = false }) == "handler-off");
	CHECK(SneakKey::Blocking({ .dialogueCamera = true }) == "dialogue");
	CHECK(SneakKey::Blocking({ .lifeState = 7 }) == "not-alive");
	CHECK(SneakKey::Blocking({ .knockState = 6 }) == "knocked");
	CHECK(SneakKey::Blocking({ .meleeAttackState = 2 }) == "melee-attack");
	CHECK(SneakKey::Blocking({ .leftLeg = 0.0f, .rightLeg = 0.0f }) == "legs-crippled");
	CHECK(SneakKey::Blocking({ .firstPerson = true, .swimming = true }) == "swimming");
	CHECK(SneakKey::Blocking({ .layersLetSneak = false, .lifeState = 7, .meleeAttackState = 1 }) == "layer not-alive melee-attack");
}

TEST("sneak key: a layer with sneaking off blocks it, a forced sneaking event wins over the layers")
{
	constexpr std::uint32_t sneaking = 1u << 7;  // UserEvents::USER_EVENT_FLAG::kSneaking
	constexpr std::uint32_t all = 0xFFFFFFFFu;
	// No layer in use: the cache is all ones.
	CHECK(SneakKey::EventEnabled(all, 0, sneaking));
	// A layer with sneaking off (a DisablePlayerControls with sneaking) clears the bit in the cache.
	CHECK(!SneakKey::EventEnabled(all & ~sneaking, 0, sneaking));
	// Other events off in a layer leave sneaking alone.
	CHECK(SneakKey::EventEnabled(all & ~(1u << 6), 0, sneaking));
	CHECK(SneakKey::EventEnabled(all & ~(1u << 8), 0, sneaking));
	// Forced on (Game.ForceEnablePlayerControls): the key works whatever the layers say. This is the blocked=layer the
	// readout gave while the key worked.
	CHECK(SneakKey::EventEnabled(all & ~sneaking, sneaking, sneaking));
	CHECK(SneakKey::EventEnabled(0, sneaking, sneaking));
	// Forcing another event does not force sneaking.
	CHECK(!SneakKey::EventEnabled(all & ~sneaking, (1u << 6) | (1u << 8), sneaking));
}

TEST("sneak key: an input layer counts only while something besides the manager holds it")
{
	CHECK(!SneakKey::LayerCounts(0));
	CHECK(!SneakKey::LayerCounts(1));  // the manager's own reference: let go
	CHECK(SneakKey::LayerCounts(2));
}
