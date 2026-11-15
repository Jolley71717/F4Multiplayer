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

TEST("form translate: a hit on a stand-in by an NPC of a plugin the other side lacks is kept, naming no attacker")
{
	// The victim's game can't show the NPC, but it hurt their stand-in in the sender's game all the same: dropped, that
	// NPC could never hurt them (PlayerHits takes an unnamed attacker's hit when the sender is near).
	for (const auto type : { MT::kPlayerHit, MT::kPlayerDamaged }) {
		Result     out;
		const auto ours = After(Encode(PlayerHit{ 2, 12.5f, false, MINE }, type), MISSING, Way::kToSession, &out);
		REQUIRE(ours);
		CHECK(out.fields == 1);
		CHECK(out.unmapped == std::vector<std::uint32_t>{ MINE });
		const auto hit = DecodePlayerHit(*ours);
		REQUIRE(hit);
		CHECK(hit->attacker == 0u);
		CHECK(hit->damage == 12.5f);
		CHECK(!hit->byPlayer);
		const auto theirs = After(Encode(PlayerHit{ 2, 3.0f, false, THEIRS }, type), MISSING, Way::kToLocal);
		REQUIRE(theirs);
		CHECK(DecodePlayerHit(*theirs)->attacker == 0u);
		// One we both have is named as each side knows it.
		const auto shared = After(Encode(PlayerHit{ 2, 3.0f, false, SESSION_SHARED }, type), MISSING, Way::kToLocal);
		REQUIRE(shared);
		CHECK(DecodePlayerHit(*shared)->attacker == SHARED);
	}
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

	// What the shooter's gun fires, a mod's record the other side lacks: none (the receiver goes by its own copy of the
	// gun), and the shot still goes, with its aim as sent.
	Result     lacking;
	const auto fired = After(Encode(Shot{ 2, 0, ShotAction::kShot, 0, 1.5f, -12, MINE }, MT::kReportShot), MISSING, Way::kToSession, &lacking);
	REQUIRE(fired);
	const auto plain = DecodeShot(*fired);
	REQUIRE(plain);
	CHECK(plain->projectile == 0u);
	CHECK(plain->heading == 1.5f);
	CHECK(plain->pitch == -12);
	CHECK(lacking.fields == 1);
	// One both sides have is renumbered like any other detail.
	const auto shared = After(Encode(Shot{ 2, 0, ShotAction::kShot, 0, 1.5f, -12, SHARED }, MT::kReportShot), MISSING, Way::kToSession);
	REQUIRE(shared);
	const auto renumbered = DecodeShot(*shared);
	REQUIRE(renumbered);
	CHECK(renumbered->projectile == SESSION_SHARED);
	CHECK(renumbered->pitch == -12);

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
	// A stage's globals: each named in our numbering, or left out (the quest decides whether the stage comes at all).
	w.questStages = { { THEIRS, 10, 0, { { SESSION_SHARED, 1.0f } } }, { 0x0001ED86, 20, 0, { { THEIRS, 1.0f }, { SESSION_SHARED, 2.0f } } } };
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
	CHECK((got->questStages[0].globals == std::vector<StageGlobal>{ { SHARED, 2.0f } }));
	CHECK(out.entries == 6);
	CHECK(out.unmapped == std::vector<std::uint32_t>{ THEIRS });
}

TEST("form translate: a live stage's globals are named in the other numbering, one the other side lacks left out, and the stage goes with its quest")
{
	for (const auto type : { MT::kReportQuestStage, MT::kQuestStage }) {
		Result     out;
		const auto packet = After(Encode(QuestStage{ 0x0001ED86, 8, 7, { { THEIRS, 1.0f }, { SESSION_SHARED, 2.0f } } }, type), MISSING, Way::kToLocal, &out);
		REQUIRE(packet);
		const auto got = DecodeQuestStage(*packet);
		REQUIRE(got);
		CHECK(got->quest == 0x0001ED86u);
		CHECK(got->stage == 8);
		CHECK(got->playerId == 7u);
		CHECK((got->globals == std::vector<StageGlobal>{ { SHARED, 2.0f } }));
		CHECK(out.entries == 1);
		// Its quest we lack: the stage is dropped, globals or not.
		CHECK(!After(Encode(QuestStage{ THEIRS, 8, 7, { { SESSION_SHARED, 2.0f } } }, type), MISSING, Way::kToLocal));
		// Ours to the session: the same the other way.
		const auto sent = After(Encode(QuestStage{ 0x0001ED86, 8, 0, { { SHARED, 3.0f } } }, type), MISSING, Way::kToSession);
		REQUIRE(sent);
		const auto theirs = DecodeQuestStage(*sent);
		REQUIRE(theirs);
		CHECK((theirs->globals == std::vector<StageGlobal>{ { SESSION_SHARED, 3.0f } }));
	}
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
	// The quest alias that made it: the quest in our numbering, or, of a mod we lack, none (a spawn like any other).
	const auto keyedPacket = After(Encode(Spawn{ 2, RUNTIME_REF, SESSION_SHARED, 0, 0x0000003C, 1.0f, 2.0f, 3.0f, 0.0f, SESSION_SHARED, 26, SpawnKind::kQuestActor }, MT::kSpawned), MISSING, Way::kToLocal);
	REQUIRE(keyedPacket);
	const auto keyed = DecodeSpawn(*keyedPacket);
	REQUIRE(keyed);
	CHECK(keyed->quest == SHARED && keyed->alias == 26 && keyed->kind == SpawnKind::kQuestActor);
	const auto unkeyedPacket = After(Encode(Spawn{ 2, RUNTIME_REF, SESSION_SHARED, 0, 0x0000003C, 1.0f, 2.0f, 3.0f, 0.0f, THEIRS, 26, SpawnKind::kEncounter }, MT::kSpawned), MISSING, Way::kToLocal);
	REQUIRE(unkeyedPacket);
	const auto unkeyed = DecodeSpawn(*unkeyedPacket);
	REQUIRE(unkeyed);
	CHECK(unkeyed->quest == 0 && unkeyed->alias == 0 && unkeyed->kind == SpawnKind::kPlain && unkeyed->base == SHARED);
	// A settler's workbench (in quest's place) likewise: in our numbering, or, of a mod we lack, a spawn like any other.
	const auto settlerPacket = After(Encode(Spawn{ 2, RUNTIME_REF, SESSION_SHARED, 0, 0x0000003C, 1.0f, 2.0f, 3.0f, 0.0f, SESSION_SHARED, 0, SpawnKind::kSettler }, MT::kSpawned), MISSING, Way::kToLocal);
	REQUIRE(settlerPacket);
	const auto settler = DecodeSpawn(*settlerPacket);
	REQUIRE(settler);
	CHECK(settler->quest == SHARED && settler->alias == 0 && settler->kind == SpawnKind::kSettler);
	const auto benchlessPacket = After(Encode(Spawn{ 2, RUNTIME_REF, SESSION_SHARED, 0, 0x0000003C, 1.0f, 2.0f, 3.0f, 0.0f, THEIRS, 0, SpawnKind::kSettler }, MT::kSpawned), MISSING, Way::kToLocal);
	REQUIRE(benchlessPacket);
	const auto benchless = DecodeSpawn(*benchlessPacket);
	REQUIRE(benchless);
	CHECK(benchless->quest == 0 && benchless->kind == SpawnKind::kPlain && benchless->base == SHARED);

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

// A gift's container is a key: a game whose load order lacks the chest's plugin never hears of the gift (it would have no
// chest to show it in), and one that has it gets the chest in its own numbering. A loose drop has no holder to map.
TEST("form translate: a gift in a container the other game lacks is not passed to it, and one it has names the container in its load order")
{
	DroppedItem gift{ .nonce = 7, .base = 0x0004F46A, .count = 1, .templateIndex = 0xFFFF, .cell = 0x0001A2B3, .holder = THEIRS };
	CHECK(!After(Encode(gift, MT::kDropPlaced), MISSING, Way::kToLocal));
	gift.holder = SESSION_SHARED;
	const auto out = After(Encode(gift, MT::kDropPlaced), MISSING, Way::kToLocal);
	REQUIRE(out);
	const auto read = DecodeDroppedItem(*out);
	REQUIRE(read);
	CHECK(read->holder == SHARED);
	CHECK(read->base == gift.base);

	// Ours to the session, and in a list: the one in a container they lack is left out, the rest stay.
	DroppedItem mine = gift;
	mine.holder = MINE;
	CHECK(!After(Encode(mine, MT::kReportDrop), MISSING, Way::kToSession));
	DroppedItem loose = gift;
	loose.holder = 0;
	const auto list = After(Encode(DropList{ true, { gift, Protocol::DroppedItem{ gift.dropId, 0, 8, gift.base, 1, 0xFFFF, {}, -1.0f, "", gift.cell, 0, 0, 0, 0, 0, THEIRS }, loose } }), MISSING,
		Way::kToLocal);
	REQUIRE(list);
	const auto listed = DecodeDropList(*list);
	REQUIRE(listed);
	REQUIRE(listed->drops.size() == 2);
	CHECK(listed->drops[0].holder == SHARED);
	CHECK(listed->drops[1].holder == 0);
	CHECK(listed->last);
}

TEST("form translate: a job for a settler names the settler in the other numbering; a settler the other side lacks drops the request")
{
	using Protocol::WorkshopRequest;
	namespace Op = Protocol::WorkshopRequestOp;
	const auto there = After(Encode(WorkshopRequest{ 0, 7, Op::kAssign, 0x000250FE, 0, {}, {}, 1.0f, 0, 0, RUNTIME_REF, SHARED }, MT::kReportWorkshopRequest), MISSING, Way::kToSession);
	REQUIRE(there);
	const auto sent = Protocol::DecodeWorkshopRequest(*there);
	REQUIRE(sent);
	CHECK(sent->settler == SESSION_SHARED);
	CHECK(sent->target == RUNTIME_REF);  // the host's own piece: no translation changes it
	const auto here = After(Encode(WorkshopRequest{ 3, 7, Op::kAssign, 0x000250FE, 0, {}, {}, 1.0f, 0, 0, SESSION_SHARED, RUNTIME_REF + 1 }, MT::kWorkshopRequest), MISSING, Way::kToLocal);
	REQUIRE(here);
	const auto got = Protocol::DecodeWorkshopRequest(*here);
	REQUIRE(got);
	CHECK(got->target == SHARED);              // a piece a plugin placed, in our numbering
	CHECK(got->settler == RUNTIME_REF + 1);    // a spawn keeps the session's own id
	// The host's game could make nothing of a settler, or a piece, of a plugin it lacks.
	CHECK(!After(Encode(WorkshopRequest{ 0, 7, Op::kAssign, 0x000250FE, 0, {}, {}, 1.0f, 0, 0, RUNTIME_REF, MINE }, MT::kReportWorkshopRequest), MISSING, Way::kToSession));
	CHECK(!After(Encode(WorkshopRequest{ 3, 7, Op::kAssign, 0x000250FE, 0, {}, {}, 1.0f, 0, 0, RUNTIME_REF, THEIRS }, MT::kWorkshopRequest), MISSING, Way::kToLocal));
	CHECK(!After(Encode(WorkshopRequest{ 3, 7, Op::kAssign, 0x000250FE, 0, {}, {}, 1.0f, 0, 0, THEIRS, SESSION_SHARED }, MT::kWorkshopRequest), MISSING, Way::kToLocal));
}

TEST("form translate: a workbench's supply lines leave out a workbench the other side lacks; a workbench it lacks drops them")
{
	using Protocol::BenchLinks;
	for (const auto type : { MT::kReportBenchLinks, MT::kBenchLinks }) {
		const auto packet = After(Encode(BenchLinks{ SHARED, { MINE, 0x0001'2345 } }, type), MISSING, Way::kToSession);
		REQUIRE(packet);
		const auto links = DecodeBenchLinks(*packet);
		REQUIRE(links);
		CHECK(links->bench == SESSION_SHARED);
		CHECK(links->links == std::vector<std::uint32_t>{ 0x0001'2345 });
		CHECK(!After(Encode(BenchLinks{ MINE, { SHARED } }, type), MISSING, Way::kToSession));
	}
	// From the session: a workbench of a plugin we lack is left out, one we have comes in our numbering.
	const auto in = After(Encode(BenchLinks{ 0x0001'2345, { THEIRS, SESSION_SHARED } }, MT::kBenchLinks), MISSING, Way::kToLocal);
	REQUIRE(in);
	const auto links = DecodeBenchLinks(*in);
	REQUIRE(links);
	CHECK(links->links == std::vector<std::uint32_t>{ SHARED });
}

TEST("form translate: the host's settlement figures leave out a settlement the other side lacks; none left drops them, and \"forget them all\" goes as it is")
{
	using Protocol::BenchFigures;
	using Protocol::BenchNumbers;
	const auto figures = [](std::uint32_t a_bench) { return BenchFigures{ a_bench, BenchFigures::kHosts, 5, 3, 2, 6, 8, 2, 3, 55, 50 }; };
	for (const auto type : { MT::kReportBenchNumbers, MT::kBenchNumbers }) {
		const auto packet = After(Encode(BenchNumbers{ { figures(SHARED), figures(MINE), figures(0x0001'2345) } }, type), MISSING, Way::kToSession);
		REQUIRE(packet);
		CHECK((*packet)[0] == static_cast<std::uint8_t>(type));
		const auto numbers = DecodeBenchNumbers(*packet);
		REQUIRE(numbers);
		CHECK((numbers->benches == std::vector<BenchFigures>{ figures(SESSION_SHARED), figures(0x0001'2345) }));
		CHECK(!After(Encode(BenchNumbers{ { figures(MINE) } }, type), MISSING, Way::kToSession));
		const auto forget = After(Encode(BenchNumbers{}, type), MISSING, Way::kToSession);
		REQUIRE(forget);
		CHECK(DecodeBenchNumbers(*forget) == BenchNumbers{});
	}
	// From the session: a settlement of a plugin we lack is left out, one we have comes in our numbering.
	const auto in = After(Encode(BenchNumbers{ { figures(THEIRS), BenchFigures{ .bench = SESSION_SHARED } } }, MT::kBenchNumbers), MISSING, Way::kToLocal);
	REQUIRE(in);
	const auto numbers = DecodeBenchNumbers(*in);
	REQUIRE(numbers);
	CHECK(numbers->benches == std::vector<BenchFigures>{ BenchFigures{ .bench = SHARED } });
}

TEST("form translate: the host's settlers of a settlement leave out an NPC the other side lacks and keep a spawn's id; a workbench it lacks drops them")
{
	using Protocol::BenchSettlers;
	for (const auto type : { MT::kReportSettlers, MT::kSettlers }) {
		const auto packet = After(Encode(BenchSettlers{ SHARED, { MINE, RUNTIME_REF, 0x0001'2345 } }, type), MISSING, Way::kToSession);
		REQUIRE(packet);
		const auto list = DecodeBenchSettlers(*packet);
		REQUIRE(list);
		CHECK(list->bench == SESSION_SHARED);
		CHECK((list->settlers == std::vector<std::uint32_t>{ RUNTIME_REF, 0x0001'2345 }));
		CHECK((!After(Encode(BenchSettlers{ MINE, { SHARED } }, type), MISSING, Way::kToSession)));
		// Forget them all goes as it is.
		const auto forget = After(Encode(BenchSettlers{}, type), MISSING, Way::kToSession);
		REQUIRE(forget);
		CHECK((DecodeBenchSettlers(*forget) == BenchSettlers{}));
	}
	// From the session: an NPC of a plugin we lack is left out, one we have comes in our numbering.
	const auto in = After(Encode(BenchSettlers{ 0x0001'2345, { THEIRS, SESSION_SHARED, RUNTIME_REF } }, MT::kSettlers), MISSING, Way::kToLocal);
	REQUIRE(in);
	const auto list = DecodeBenchSettlers(*in);
	REQUIRE(list);
	CHECK((list->settlers == std::vector<std::uint32_t>{ SHARED, RUNTIME_REF }));
}

TEST("form translate: the host's settlements leave out a workbench the other side lacks, in the other side's order")
{
	using Protocol::HostSettlements;
	for (const auto type : { MT::kReportHostSettlements, MT::kHostSettlements }) {
		const auto packet = After(Encode(HostSettlements{ 2, { 0x0001'2345, MINE, SHARED } }, type), MISSING, Way::kToSession);
		REQUIRE(packet);
		const auto told = DecodeHostSettlements(*packet);
		REQUIRE(told);
		CHECK(told->localLeader == 2);
		CHECK((told->benches == std::vector<std::uint32_t>{ 0x0001'2345, SESSION_SHARED }));
	}
	const auto in = After(Encode(HostSettlements{ 1, { 0x0001'2345, SESSION_SHARED, THEIRS } }, MT::kHostSettlements), MISSING, Way::kToLocal);
	REQUIRE(in);
	const auto told = DecodeHostSettlements(*in);
	REQUIRE(told);
	CHECK((told->benches == std::vector<std::uint32_t>{ 0x0001'2345, SHARED }));
}

TEST("form translate: a Far Harbor radiant's start goes between games that load DLCCoast.esm at other places, and is dropped for a game without it")
{
	// Ours: DLCCoast.esm is load index 02. The session's: 05.
	const FormIds::Mapper both(Protocol::ComparedLoadOrder({ "Fallout4.esm", "A.esp", "DLCCoast.esm" }, {}),
		Protocol::ComparedLoadOrder({ "Fallout4.esm", "B.esp", "C.esp", "D.esp", "E.esp", "DLCCoast.esm" }, {}));
	constexpr std::uint32_t QUEST = 0x0200EB51;      // DLC03WorkshopRadiantOwned02
	constexpr std::uint32_t SESSION_QUEST = 0x0500EB51;
	constexpr std::uint32_t FAR_HARBOR = 0x02001234;  // a location of the add-on
	constexpr std::uint32_t SESSION_FAR_HARBOR = 0x05001234;
	constexpr std::uint32_t SANCTUARY = 0x000250FE;  // Fallout4.esm's: the same in every game
	const RadiantStart start{ 3, QUEST, 7, { RadiantFill{ 4, 1, FAR_HARBOR }, RadiantFill{ 9, 0, SANCTUARY } }, { 10 } };

	const auto toSession = After(Encode(start, MT::kReportRadiantStart), both, Way::kToSession);
	REQUIRE(toSession);
	const auto sent = DecodeRadiantStart(*toSession);
	REQUIRE(sent);
	CHECK(sent->quest == SESSION_QUEST);
	REQUIRE(sent->fills.size() == 2);
	CHECK(sent->fills[0].form == SESSION_FAR_HARBOR);
	CHECK(sent->fills[1].form == SANCTUARY);
	CHECK(sent->serial == 7);
	CHECK((sent->stages == std::vector<std::uint16_t>{ 10 }));

	// And back to the game that has it at 02.
	const auto toLocal = After(Encode(*sent, MT::kRadiantStarted), both, Way::kToLocal);
	REQUIRE(toLocal);
	CHECK(*DecodeRadiantStart(*toLocal) == start);
	// A stage of that run is named the same way.
	const auto stage = After(Encode(RadiantStage{ 0, QUEST, 3, 7, 10 }, MT::kReportRadiantStage), both, Way::kToSession);
	REQUIRE(stage);
	CHECK(DecodeRadiantStage(*stage)->quest == SESSION_QUEST);

	// The session's game that lacks the add-on: the start is dropped on the way to it (the run stays the sender's own).
	const FormIds::Mapper without(Protocol::ComparedLoadOrder({ "Fallout4.esm" }, {}),
		Protocol::ComparedLoadOrder({ "Fallout4.esm", "DLCCoast.esm" }, {}));
	CHECK(!After(Encode(RadiantStart{ 3, 0x0100EB51, 7, {}, {} }, MT::kRadiantStarted), without, Way::kToLocal));
	CHECK(!After(Encode(RadiantStage{ 0, 0x0100EB51, 3, 7, 10 }, MT::kRadiantStage), without, Way::kToLocal));
}

TEST("form translate: a question about an attack quest the other side lacks goes with no quest; any later phase of it is dropped")
{
	using Protocol::AttackOutcome;
	using Protocol::AttackPhase;
	using Protocol::SettlementAttack;
	// A friend's roll of an add-on's attack, asked of the host's game that lacks the add-on: the settlement is enough.
	Result     out;
	const auto in = After(Encode(SettlementAttack{ SESSION_SHARED, THEIRS, 0, AttackPhase::kRolled, AttackOutcome::kNone }, MT::kSettlementAttack), MISSING, Way::kToLocal, &out);
	REQUIRE(in);
	const auto asked = DecodeSettlementAttack(*in);
	REQUIRE(asked);
	CHECK(asked->workshop == SHARED);
	CHECK(asked->quest == 0);
	CHECK(asked->phase == AttackPhase::kRolled);
	CHECK(out.fields == 1);
	// The same quest underway, defended or over is about the quest: nothing to say without it.
	CHECK(!After(Encode(SettlementAttack{ SESSION_SHARED, THEIRS, 0, AttackPhase::kUnderway, AttackOutcome::kNone }, MT::kSettlementAttack), MISSING, Way::kToLocal));
	CHECK(!After(Encode(SettlementAttack{ SESSION_SHARED, THEIRS, 0, AttackPhase::kOver, AttackOutcome::kDefended }, MT::kSettlementAttack), MISSING, Way::kToLocal));
	// A workbench the other side lacks drops even a question.
	CHECK(!After(Encode(SettlementAttack{ THEIRS, SESSION_SHARED, 0, AttackPhase::kRolled, AttackOutcome::kNone }, MT::kSettlementAttack), MISSING, Way::kToLocal));
	// A quest both have keeps its id, in the other numbering.
	const auto both = After(Encode(SettlementAttack{ SESSION_SHARED, SESSION_SHARED, 0, AttackPhase::kRolled, AttackOutcome::kNone }, MT::kSettlementAttack), MISSING, Way::kToLocal);
	REQUIRE(both);
	CHECK(DecodeSettlementAttack(*both)->quest == SHARED);
}
