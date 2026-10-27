#include "Test.h"

#include "FormTranslate.h"
#include "ProtocolSamples.h"

#include <format>
#include <span>
#include <string>
#include <vector>

using FormTranslate::Result;
using FormTranslate::Translate;
using FormTranslate::Way;
using namespace ProtocolSamples;
using Names = std::vector<std::string>;

namespace
{
	// Full plugins P0 to P253 and light ones L0 to L4095: every place a form ID can name.
	Names Every(bool a_rotated)
	{
		Names full{ "Fallout4.esm" };  // first in every game: the engine loads it there
		for (std::size_t i = 1; i < FormIds::MAX_FULL; ++i) {
			full.push_back(std::format("P{}.esp", a_rotated ? 1 + (i % (FormIds::MAX_FULL - 1)) : i));
		}
		Names light;
		for (std::size_t i = 0; i < FormIds::MAX_LIGHT; ++i) {
			light.push_back(std::format("L{}.esl", a_rotated ? (i + 7) % FormIds::MAX_LIGHT : i));
		}
		return Protocol::ComparedLoadOrder(full, light);
	}

	// What the packet becomes: the same, the new one, or nothing (dropped).
	std::optional<Bytes> After(const Bytes& a_packet, const FormIds::Mapper& a_mapper, Way a_way, Result* a_result = nullptr)
	{
		auto result = Translate(a_packet, a_mapper, a_way);
		const auto kind = result.kind;
		auto       packet = kind == Result::Kind::kChanged ? result.packet : a_packet;
		if (a_result) {
			*a_result = std::move(result);
		}
		if (kind == Result::Kind::kDropped) {
			return std::nullopt;
		}
		return packet;
	}

	// Ours: Fallout4.esm, Mine.esp, Shared.esp, light MyLight.esl. The session's: Fallout4.esm, Shared.esp, Theirs.esp.
	const FormIds::Mapper MISSING(Protocol::ComparedLoadOrder({ "Fallout4.esm", "Mine.esp", "Shared.esp" }, { "MyLight.esl" }),
		Protocol::ComparedLoadOrder({ "Fallout4.esm", "Shared.esp", "Theirs.esp" }, {}));
	constexpr std::uint32_t MINE = 0x01000801;
	constexpr std::uint32_t SHARED = 0x02000802;      // ours; the session's 0x01000802
	constexpr std::uint32_t THEIRS = 0x02000803;      // the session's; we lack it
	constexpr std::uint32_t SESSION_SHARED = 0x01000802;
}

TEST("form translate: with the same load order every sample is left as it is, undecoded")
{
	const FormIds::Mapper same(Every(false), Every(false));
	REQUIRE(same.Identity());
	for (const auto& sample : Samples()) {
		for (const auto way : { Way::kToSession, Way::kToLocal }) {
			const auto result = Translate(sample.packet, same, way);
			CHECK(result.kind == Result::Kind::kSame);
			CHECK(result.packet.empty());
		}
	}
}

TEST("form translate: every sample goes to the session's numbering and back to the same bytes")
{
	const FormIds::Mapper moved(Every(false), Every(true));
	REQUIRE(!moved.Identity());
	std::size_t changed = 0;
	for (const auto& sample : Samples()) {
		Result     out;
		const auto there = After(sample.packet, moved, Way::kToSession, &out);
		if (!there) {
			Test::Fail(__FILE__, __LINE__, sample.name + " was dropped");
			continue;
		}
		CHECK(out.entries == 0);
		CHECK(out.fields == 0);
		changed += out.kind == Result::Kind::kChanged ? 1 : 0;
		const auto back = After(*there, moved, Way::kToLocal);
		if (!back || *back != sample.packet) {
			Test::Fail(__FILE__, __LINE__, sample.name + " did not come back as it was");
		}
	}
	// Most samples carry an ID of some plugin after Fallout4.esm (REF_B is the tenth's).
	CHECK(changed >= 10);
}

TEST("form translate: a moved plugin's form gets its place in the other numbering")
{
	const FormIds::Mapper m(Protocol::ComparedLoadOrder({ "Fallout4.esm", "A.esp", "B.esp" }, { "L.esl" }),
		Protocol::ComparedLoadOrder({ "Fallout4.esm", "B.esp", "A.esp" }, { "X.esl", "L.esl" }));
	const auto out = After(Encode(ActorDeath{ 0x01000801, 2, false }, MT::kReportDeath), m, Way::kToSession);
	REQUIRE(out);
	const auto death = DecodeActorDeath(*out);
	REQUIRE(death);
	CHECK(death->refId == 0x02000801u);
	CHECK(PeekType(*out) == MT::kReportDeath);

	// A light plugin's, and back.
	// The session's light 0 (X.esl) is not ours.
	const auto hit = After(Encode(ActorHealth{ 0xFE000ABC, 0.5f }, MT::kActorHealth), m, Way::kToLocal);
	CHECK(!hit);
	const auto health = After(Encode(ActorHealth{ 0xFE001ABC, 0.5f }, MT::kActorHealth), m, Way::kToLocal);
	REQUIRE(health);
	CHECK(DecodeActorHealth(*health)->refId == 0xFE000ABCu);
}

TEST("form translate: a message about a form the other side lacks is dropped, and says which ID")
{
	Result out;
	CHECK(!After(Encode(ActorDeath{ MINE, 1, true }, MT::kReportDeath), MISSING, Way::kToSession, &out));
	CHECK(out.unmapped == std::vector<std::uint32_t>{ MINE });
	CHECK(!After(Encode(RefState{ THEIRS, RefState::kYes, RefState::kNo }, MT::kRefStateChanged), MISSING, Way::kToLocal));
	CHECK(!After(Encode(Activation{ 2, MINE }, MT::kReportActivation), MISSING, Way::kToSession));
	CHECK(!After(Encode(ContainerChange{ SHARED, MINE, 1 }, MT::kReportContainer), MISSING, Way::kToSession));
	// A runtime one is nobody's plugin's: as it is.
	Result runtime;
	CHECK(After(Encode(ActorDeath{ RUNTIME_REF, 1, true }, MT::kReportDeath), MISSING, Way::kToSession, &runtime));
	CHECK(runtime.kind == Result::Kind::kSame);
}

TEST("form translate: an explosion set off by an NPC of a plugin we lack is dropped; a player's or a spawn's is kept")
{
	Result out;
	CHECK(!After(Encode(Explosion{ 2, 0x0001D2EE, 1.0f, 2.0f, 3.0f, 0, 0x3C, THEIRS }, MT::kExplosion), MISSING, Way::kToLocal, &out));
	CHECK(out.unmapped == std::vector<std::uint32_t>{ THEIRS });
	CHECK(After(Encode(Explosion{ 2, 0x0001D2EE, 1.0f, 2.0f, 3.0f, 0, 0x3C, 0 }, MT::kExplosion), MISSING, Way::kToLocal));
	CHECK(After(Encode(Explosion{ 2, 0x0001D2EE, 1.0f, 2.0f, 3.0f, 0, 0x3C, RUNTIME_REF }, MT::kExplosion), MISSING, Way::kToLocal));
	const auto shared = After(Encode(Explosion{ 2, 0x0001D2EE, 1.0f, 2.0f, 3.0f, 0, 0x3C, SESSION_SHARED }, MT::kExplosion), MISSING, Way::kToLocal);
	REQUIRE(shared);
	CHECK(DecodeExplosion(*shared)->source == SHARED);
}

TEST("form translate: an entry of a list the other side can't name is left out, the rest kept")
{
	ActorState a, b, c;
	a.refId = SHARED, b.refId = MINE, c.refId = 0x00012345;
	c.cell = MINE;  // in a cell of our mod: its state means nothing there
	Result     out;
	const auto packet = After(Encode(std::vector<ActorState>{ a, b, c }, MT::kActorStates), MISSING, Way::kToSession, &out);
	REQUIRE(packet);
	const auto states = DecodeActorStates(*packet);
	REQUIRE(states);
	REQUIRE(states->size() == 1);
	CHECK((*states)[0].refId == SESSION_SHARED);
	CHECK(out.entries == 2);

	Equipment gear;
	gear.playerId = 1;
	gear.items = { SHARED, MINE, 0x0001F00D };
	gear.mods = { { MINE, 0, { { 0x0001F00E, 0 } } }, { SHARED, 0, { { MINE, 0 }, { 0x0001F00F, 1 } } } };
	const auto worn = After(Encode(gear, MT::kEquipment), MISSING, Way::kToSession);
	REQUIRE(worn);
	const auto back = DecodeEquipment(*worn);
	REQUIRE(back);
	CHECK(back->items == std::vector<std::uint32_t>({ SESSION_SHARED, 0x0001F00D }));
	REQUIRE(back->mods.size() == 1);
	CHECK(back->mods[0].item == SESSION_SHARED);
	REQUIRE(back->mods[0].mods.size() == 1);
	CHECK(back->mods[0].mods[0].mod == 0x0001F00Fu);
}

TEST("form translate: a detail the other side lacks becomes none")
{
	Result     out;
	const auto packet = After(Encode(Shot{ 2, SHARED, ShotAction::kShot, MINE }, MT::kReportShot), MISSING, Way::kToSession, &out);
	REQUIRE(packet);
	const auto shot = DecodeShot(*packet);
	REQUIRE(shot);
	CHECK(shot->refId == SESSION_SHARED);
	CHECK(shot->sound == 0u);
	CHECK(out.fields == 1);
	// The shooter themselves (refId 0) with a mod's gun sound: still a shot.
	CHECK(After(Encode(Shot{ 2, 0, ShotAction::kShot, MINE }, MT::kReportShot), MISSING, Way::kToSession));

	// A line of a mod's recording is text only.
	const auto line = After(Encode(Line{ 1, SHARED, "Hi", MINE, 2, 0x0001F00D }, MT::kReportLine), MISSING, Way::kToSession);
	REQUIRE(line);
	const auto spoken = DecodeLine(*line);
	REQUIRE(spoken);
	CHECK(spoken->text == "Hi");
	CHECK(spoken->info == 0u);
	CHECK(spoken->response == 0);
	CHECK(spoken->voiceType == 0u);

	// A friend whose appearance is a mod's NPC: our default.
	const auto joined = After(Encode(PlayerJoined{ 3, "P", THEIRS }), MISSING, Way::kToLocal);
	REQUIRE(joined);
	CHECK(DecodePlayerJoined(*joined)->appearance == 0u);
}

TEST("form translate: a numbered container change keeps its number, emptied, so the ledger's count stays")
{
	Result     out;
	const auto packet = After(Encode(IndexedContainerChange{ 17, 2, { THEIRS, 0x0000000F, 3 } }), MISSING, Way::kToLocal, &out);
	REQUIRE(packet);
	const auto change = DecodeIndexedContainerChange(*packet);
	REQUIRE(change);
	CHECK(change->index == 17u);
	CHECK(change->playerId == 2u);
	CHECK(IsEmpty(change->change));
	CHECK(!IsValid(change->change));
	CHECK(out.entries == 1);

	const auto kept = After(Encode(IndexedContainerChange{ 18, 2, { SESSION_SHARED, 0x0000000F, 3 } }), MISSING, Way::kToLocal);
	REQUIRE(kept);
	CHECK(DecodeIndexedContainerChange(*kept)->change.container == SHARED);
}

TEST("form translate: the world state leaves out what we lack and keeps every container change's place")
{
	WorldState w;
	w.deadActors = { THEIRS, SESSION_SHARED, 0x00012345 };
	w.containerFirstIndex = 500;
	w.containerChanges = { { 500, 1, { SESSION_SHARED, 0x0F, 1 } }, { 501, 1, { SESSION_SHARED, THEIRS, 1 } }, { 502, 1, { 0x00012345, 0x0F, -1 } } };
	w.pickedUp = { THEIRS };
	w.refStates = { { THEIRS, RefState::kYes, RefState::kNo }, { SESSION_SHARED, RefState::kNo, RefState::kNo } };
	w.questStages = { { THEIRS, 10 }, { 0x0001ED86, 20 } };
	Result     out;
	const auto packet = After(Encode(w), MISSING, Way::kToLocal, &out);
	REQUIRE(packet);
	const auto got = DecodeWorldState(*packet);
	REQUIRE(got);
	CHECK(got->deadActors == std::vector<std::uint32_t>({ SHARED, 0x00012345 }));
	CHECK(got->containerFirstIndex == 500u);
	REQUIRE(got->containerChanges.size() == 3);
	CHECK(got->containerChanges[0].change.container == SHARED);
	CHECK(IsEmpty(got->containerChanges[1].change));
	CHECK(got->containerChanges[2].index == 502u);
	CHECK(got->containerChanges[2].change.count == -1);
	CHECK(got->pickedUp.empty());
	REQUIRE(got->refStates.size() == 1);
	CHECK(got->refStates[0].refId == SHARED);
	REQUIRE(got->questStages.size() == 1);
	CHECK(got->questStages[0].quest == 0x0001ED86u);
	CHECK(out.entries == 5);
	CHECK(out.unmapped == std::vector<std::uint32_t>{ THEIRS });
}

TEST("form translate: a spawn keeps the session's own id, and a broken packet is left for its reader")
{
	const auto spawn = After(Encode(Spawn{ 2, RUNTIME_REF, SESSION_SHARED, 0, 0x0000003C, 1.0f, 2.0f, 3.0f, 0.0f }, MT::kSpawned), MISSING, Way::kToLocal);
	REQUIRE(spawn);
	const auto placed = DecodeSpawn(*spawn);
	REQUIRE(placed);
	CHECK(placed->refId == RUNTIME_REF);
	CHECK(placed->base == SHARED);
	// What to place is of a mod we lack: nothing to place.
	CHECK(!After(Encode(Spawn{ 2, RUNTIME_REF, THEIRS, 0, 0x0000003C, 1.0f, 2.0f, 3.0f, 0.0f }, MT::kSpawned), MISSING, Way::kToLocal));

	auto broken = Encode(ActorDeath{ MINE, 1, true }, MT::kReportDeath);
	broken.pop_back();
	CHECK(Translate(broken, MISSING, Way::kToSession).kind == Result::Kind::kSame);
	CHECK(Translate(Bytes{}, MISSING, Way::kToSession).kind == Result::Kind::kSame);
}

TEST("form translate: a friend in a frame lent to them names it by spawn id, which no translation changes")
{
	PlayerStatus status{ 2, 0, 0, 100, 5, false, false, false, true, SHARED, RUNTIME_REF };
	Result     out;
	const auto there = After(Encode(status, MT::kReportStatus), MISSING, Way::kToSession, &out);
	REQUIRE(there);
	const auto told = DecodePlayerStatus(*there);
	REQUIRE(told);
	CHECK(told->powerArmorFrame == SESSION_SHARED);
	CHECK(told->powerArmorFrameRef == RUNTIME_REF);
	CHECK(out.fields == 0);
}

TEST("form translate: a lent frame's piece whose base or mod the other side lacks is left out, the rest keep their slots")
{
	FrameLending lent{ 3, 2, RUNTIME_REF, LoanRefusal::kNone, {} };
	lent.kit.push_back(LentItem{ .slot = 0, .base = SHARED, .health = 0.5f });
	lent.kit.push_back(LentItem{ .slot = 1, .base = MINE, .health = 0.5f });                                // a piece of a plugin they lack
	lent.kit.push_back(LentItem{ .slot = 2, .base = SHARED, .mods = { { MINE, 0, 0 } }, .health = 0.5f });  // with a mod they lack
	lent.kit.push_back(LentItem{ .slot = 5, .base = SHARED, .mods = { { SHARED, 1, 0 } }, .core = true });
	Result     out;
	const auto there = After(Encode(lent, MT::kLendFrame), MISSING, Way::kToSession, &out);
	REQUIRE(there);
	const auto sent = DecodeFrameLending(*there);
	REQUIRE(sent);
	REQUIRE(sent->kit.size() == 2);
	CHECK(sent->kit[0].slot == 0);
	CHECK(sent->kit[0].base == SESSION_SHARED);
	CHECK(sent->kit[1].slot == 5);
	CHECK(sent->kit[1].mods[0].mod == SESSION_SHARED);
	CHECK(sent->kit[1].core);
	CHECK(out.entries == 2);

	// Coming in, a piece of the session's plugin we lack is left out the same way.
	FrameLending theirs{ 3, 2, RUNTIME_REF, LoanRefusal::kNone, { LentItem{ .slot = 0, .base = THEIRS }, LentItem{ .slot = 1, .base = SESSION_SHARED } } };
	const auto here = After(Encode(theirs, MT::kFrameLent), MISSING, Way::kToLocal);
	REQUIRE(here);
	const auto heard = DecodeFrameLending(*here);
	REQUIRE(heard);
	REQUIRE(heard->kit.size() == 1);
	CHECK(heard->kit[0].slot == 1);
	CHECK(heard->kit[0].base == SHARED);
}

TEST("form translate: a return to a place the other side lacks comes with no place, and is kept")
{
	const FrameReturn back{ 2, 3, RUNTIME_REF, false, THEIRS, 0, 10.0f, 20.0f, 30.0f, 0.5f, { { 0, true, 0.5f } } };
	Result     out;
	const auto here = After(Encode(back, MT::kFrameReturned), MISSING, Way::kToLocal, &out);
	REQUIRE(here);
	const auto heard = DecodeFrameReturn(*here);
	REQUIRE(heard);
	CHECK(heard->cell == 0u);
	CHECK(heard->worldspace == 0u);
	CHECK(heard->items == back.items);
	CHECK(out.fields == 1);
	// One it has keeps its place, in its own numbering.
	const FrameReturn shared{ 2, 3, RUNTIME_REF, false, SESSION_SHARED, 0, 10.0f, 20.0f, 30.0f, 0.5f, {} };
	const auto        kept = After(Encode(shared, MT::kFrameReturned), MISSING, Way::kToLocal);
	REQUIRE(kept);
	CHECK(DecodeFrameReturn(*kept)->cell == SHARED);
}
