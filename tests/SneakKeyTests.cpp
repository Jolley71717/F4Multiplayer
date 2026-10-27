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
