// A leveled NPC's roll (its templates, level and legendary status) and a spawn's factions, reported by the game that
// runs it and made the same in the others' (NpcRoll.h): the wire format, the translation, the server's rule (the
// first runner's roll is the session's) and the pure logic each game decides by.

#include "ServerFixture.h"

#include "FormTranslate.h"
#include "game/NpcRoll.h"

#include <map>

using namespace TestServer;
using namespace std::chrono_literals;
using Protocol::FactionRank;
using Protocol::NpcIdentity;
using NpcRoll::Roll;

namespace
{
	constexpr std::uint32_t BLOATFLY = 0x0001F0B1;     // a plugin's reference, placed as a leveled bloatfly
	constexpr std::uint32_t SPAWN = 0xFF001234;        // a spawn id
	constexpr std::uint32_t ENC_BLOATFLY = 0x00010A00;  // the NPC it was placed as: templated on the list below
	constexpr std::uint32_t LIST = 0x00010B00;          // its leveled list
	constexpr std::uint32_t PLAIN = 0x00010C00;         // what the list gives
	constexpr std::uint32_t GLOWING = 0x00010C01;
	constexpr std::uint32_t LEGENDARY = 0x00010D00;     // the legendary template of both
	constexpr std::uint32_t DEEP_LIST = 0x00010B01;     // a nested list, leading back to the first
	constexpr std::uint32_t DEEP = 0x00010C02;

	constexpr std::uint32_t SETTLERS = 0x000337F3;
	constexpr std::uint32_t SCAVENGERS = 0x0001BCC0;
	constexpr std::uint32_t WORKSHOP = 0x00058302;

	// ENC_BLOATFLY -> LIST -> PLAIN, GLOWING, DEEP_LIST -> DEEP, LIST (a cycle). PLAIN and GLOWING lead to LEGENDARY by
	// their legendary template; LEGENDARY is templated on PLAIN. A form not here isn't in the game.
	std::optional<NpcRoll::Node> Look(std::uint32_t a_id)
	{
		using NpcRoll::Node;
		static const std::map<std::uint32_t, Node> forms{
			{ ENC_BLOATFLY, { true, { { LIST, false } } } },
			{ LIST, { false, { { PLAIN, false }, { GLOWING, false }, { DEEP_LIST, false }, { 0x00099999, false } } } },
			{ PLAIN, { true, { { LEGENDARY, true } } } },
			{ GLOWING, { true, { { LEGENDARY, true } } } },
			{ LEGENDARY, { true, { { PLAIN, false } } } },
			{ DEEP_LIST, { false, { { DEEP, false }, { LIST, false } } } },
			{ DEEP, { true, {} } },
		};
		const auto it = forms.find(a_id);
		return it != forms.end() ? std::optional{ it->second } : std::nullopt;
	}

	// The owner's report of a bloatfly rolled as a_npc in the traits and stats slots (and the spells slot: a_spells).
	NpcIdentity Rolled(std::uint32_t a_npc, std::uint32_t a_spells, std::uint16_t a_level = 12)
	{
		NpcIdentity roll{ .playerId = 1, .refId = BLOATFLY, .originalBase = ENC_BLOATFLY, .level = a_level };
		roll.templates[0] = a_npc;
		roll.templates[1] = a_npc;
		roll.templates[3] = a_spells;
		roll.mask = NpcRoll::MaskOf(roll.templates);
		return roll;
	}

	// Ours as our game rolled it: plain.
	NpcRoll::Ours OursPlain()
	{
		NpcRoll::Ours ours{ .leveled = true, .originalBase = ENC_BLOATFLY, .level = 12 };
		ours.templates[0] = PLAIN;
		ours.templates[1] = PLAIN;
		ours.templates[3] = PLAIN;
		return ours;
	}

	const auto REACH = NpcRoll::Walk(ENC_BLOATFLY, Look);
	const auto Reachable = [](std::uint32_t a_id) { return REACH.Has(a_id); };

	std::vector<std::uint8_t> Claim(std::uint32_t a_ref, Protocol::ClaimReason a_reason = Protocol::ClaimReason::kUnowned)
	{
		return Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_ref, a_reason } });
	}

	std::vector<std::uint8_t> Report(const NpcIdentity& a_roll)
	{
		auto roll = a_roll;
		roll.playerId = 0;
		return Protocol::Encode(roll, MT::kReportNpcIdentity);
	}

	std::optional<NpcIdentity> Told(FakeTransport& a_fake, std::uint32_t a_peer, std::chrono::milliseconds a_wait = 2s)
	{
		const auto packet = a_fake.WaitFor(a_peer, Type(MT::kNpcIdentity), a_wait);
		return packet ? Protocol::DecodeNpcIdentity(*packet) : std::nullopt;
	}
}

// ---- wire --------------------------------------------------------------------------------------------------------

TEST("npc roll: a roll reads back, slots in mask order and factions with their ranks")
{
	auto roll = Rolled(GLOWING, LEGENDARY);
	roll.flags = NpcIdentity::kLegendary;
	const auto decoded = Protocol::DecodeNpcIdentity(Protocol::Encode(roll, MT::kNpcIdentity));
	REQUIRE(decoded);
	CHECK(*decoded == roll);

	NpcIdentity spawn{ .playerId = 3, .refId = SPAWN, .level = 7 };
	NpcRoll::SetFactions(spawn, { { SCAVENGERS, 0 }, { WORKSHOP, -1 } });
	const auto back = Protocol::DecodeNpcIdentity(Protocol::Encode(spawn, MT::kNpcIdentity));
	REQUIRE(back);
	CHECK(*back == spawn);
	CHECK(back->factions[1].rank == -1);
}

TEST("npc roll: a report naming no NPC, too high a level, slots without an original or bad factions is refused")
{
	const auto refused = [](const NpcIdentity& a_roll) { return !Protocol::DecodeNpcIdentity(Protocol::Encode(a_roll, MT::kNpcIdentity)); };
	auto roll = Rolled(PLAIN, PLAIN);
	CHECK(!refused(roll));

	auto player = roll;
	player.refId = Protocol::PLAYER_REF_ID;
	CHECK(refused(player));
	auto none = roll;
	none.refId = 0;
	CHECK(refused(none));
	auto high = roll;
	high.level = Protocol::MAX_IDENTITY_LEVEL + 1;
	CHECK(refused(high));
	auto orphan = roll;
	orphan.originalBase = 0;
	CHECK(refused(orphan));
	auto flags = roll;
	flags.flags = 0x80;
	CHECK(refused(flags));
	auto unflagged = roll;
	unflagged.factions = { { SCAVENGERS, 0 } };
	CHECK(refused(unflagged));
	auto cutOnly = roll;
	cutOnly.flags = NpcIdentity::kFactionsCut;
	CHECK(refused(cutOnly));
	auto twice = roll;
	twice.flags = NpcIdentity::kFactions;
	twice.factions = { { SCAVENGERS, 0 }, { SCAVENGERS, 1 } };
	CHECK(refused(twice));
	auto zero = roll;
	zero.flags = NpcIdentity::kFactions;
	zero.factions = { { 0, 0 } };
	CHECK(refused(zero));

	// A slot past the thirteenth, a faction count past the most, and anything after the end.
	auto bytes = Protocol::Encode(roll, MT::kNpcIdentity);
	auto wideMask = bytes;
	wideMask[17] = 0x20;  // mask's high byte (little-endian): bit 13
	CHECK(!Protocol::DecodeNpcIdentity(wideMask));
	auto many = Protocol::Encode(NpcIdentity{ .playerId = 1, .refId = SPAWN, .flags = NpcIdentity::kFactions }, MT::kNpcIdentity);
	many.back() = static_cast<std::uint8_t>(Protocol::MAX_IDENTITY_FACTIONS + 1);
	for (std::size_t i = 0; i <= Protocol::MAX_IDENTITY_FACTIONS; ++i) {
		many.insert(many.end(), { 0x01, 0x00, 0x00, 0x00, 0x00 });
	}
	CHECK(!Protocol::DecodeNpcIdentity(many));
	bytes.push_back(0);
	CHECK(!Protocol::DecodeNpcIdentity(bytes));
	// The slots are what the mask says: a short packet is refused.
	auto shortened = Protocol::Encode(roll, MT::kNpcIdentity);
	shortened.resize(shortened.size() - 3);
	CHECK(!Protocol::DecodeNpcIdentity(shortened));
}

TEST("npc roll: translated, a slot the other side lacks is none with its bit kept, a faction it lacks is left out")
{
	// Ours: Fallout4.esm, Mine.esp, Shared.esp. The session's: Fallout4.esm, Shared.esp, Theirs.esp.
	const FormIds::Mapper mapper(Protocol::ComparedLoadOrder({ "Fallout4.esm", "Mine.esp", "Shared.esp" }, {}),
		Protocol::ComparedLoadOrder({ "Fallout4.esm", "Shared.esp", "Theirs.esp" }, {}));
	constexpr std::uint32_t MINE = 0x01000801;
	constexpr std::uint32_t SHARED = 0x02000802;
	constexpr std::uint32_t SESSION_SHARED = 0x01000802;

	NpcIdentity roll{ .refId = BLOATFLY, .originalBase = SHARED, .level = 9 };
	roll.templates[0] = SHARED;
	roll.templates[2] = MINE;
	roll.mask = NpcRoll::MaskOf(roll.templates);
	NpcRoll::SetFactions(roll, { { SCAVENGERS, 0 }, { MINE, 1 } });
	const auto result = FormTranslate::Translate(Protocol::Encode(roll, MT::kReportNpcIdentity), mapper, FormTranslate::Way::kToSession);
	REQUIRE(result.kind == FormTranslate::Result::Kind::kChanged);
	const auto out = Protocol::DecodeNpcIdentity(result.packet);
	REQUIRE(out);
	CHECK(out->originalBase == SESSION_SHARED);
	CHECK(out->templates[0] == SESSION_SHARED);
	CHECK(out->templates[2] == 0u);
	CHECK(out->mask == 0b101);
	CHECK((out->factions == std::vector<FactionRank>{ { SCAVENGERS, 0 } }));

	// Placed as an NPC the session lacks: no roll to make (the slots go), the factions stay.
	auto mine = roll;
	mine.originalBase = MINE;
	const auto bare = FormTranslate::Translate(Protocol::Encode(mine, MT::kReportNpcIdentity), mapper, FormTranslate::Way::kToSession);
	REQUIRE(bare.kind == FormTranslate::Result::Kind::kChanged);
	const auto plain = Protocol::DecodeNpcIdentity(bare.packet);
	REQUIRE(plain);
	CHECK(plain->originalBase == 0u);
	CHECK(plain->mask == 0);
	CHECK(plain->factions.size() == 1);

	// An NPC the session lacks: dropped.
	auto lacked = roll;
	lacked.refId = MINE;
	CHECK(FormTranslate::Translate(Protocol::Encode(lacked, MT::kReportNpcIdentity), mapper, FormTranslate::Way::kToSession).kind == FormTranslate::Result::Kind::kDropped);
}

// ---- the server ----------------------------------------------------------------------------------------------------

TEST("npc roll: the runner's roll reaches the others with its ID, never from anyone else")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Deliver(1, Claim(BLOATFLY));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kActorOwners)));

	f.fake->Deliver(2, Report(Rolled(PLAIN, PLAIN)));  // Bob doesn't run it
	CHECK(!Told(*f.fake, 1, 300ms));
	f.fake->Deliver(1, Report(Rolled(GLOWING, LEGENDARY)));
	const auto told = Told(*f.fake, 2);
	REQUIRE(told);
	CHECK(told->playerId == alice->playerId);
	CHECK(told->templates[3] == LEGENDARY);
	CHECK(!Told(*f.fake, 1, 300ms));  // not echoed
	// The same again is no news.
	f.fake->Deliver(1, Report(Rolled(GLOWING, LEGENDARY)));
	CHECK(!Told(*f.fake, 2, 300ms));
}

TEST("npc roll: a newcomer is told the run NPCs' rolls, and one taken over keeps its first runner's")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Deliver(1, Claim(BLOATFLY));
	f.fake->Deliver(1, Report(Rolled(GLOWING, LEGENDARY)));
	REQUIRE(Told(*f.fake, 2));

	// It follows Bob now: his game takes it over at once and reports its own roll. The session's stays hers.
	f.fake->Take(1);
	f.fake->Deliver(2, Claim(BLOATFLY, Protocol::ClaimReason::kCompanion));
	const auto owners = f.fake->WaitFor(1, Type(MT::kActorOwners));
	REQUIRE(owners);
	const auto taken = Protocol::DecodeOwners(*owners);
	REQUIRE(taken);
	REQUIRE(taken->size() == 1);
	CHECK((*taken)[0].playerId != alice->playerId);
	f.fake->Deliver(2, Report(Rolled(PLAIN, PLAIN)));
	CHECK(!Told(*f.fake, 1, 300ms));

	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	const auto told = Told(*f.fake, 3);
	REQUIRE(told);
	CHECK(told->playerId == alice->playerId);
	CHECK(told->templates[0] == GLOWING);
}

TEST("npc roll: forgotten when the NPC dies, when its runner lets it go and when its runner leaves")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(bob);
	constexpr std::uint32_t OTHER = 0x0001F0B2;
	constexpr std::uint32_t THIRD = 0x0001F0B3;
	f.fake->Deliver(1, Claim(BLOATFLY));
	f.fake->Deliver(1, Claim(OTHER));
	f.fake->Deliver(1, Claim(THIRD));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kActorOwners)));
	for (const auto ref : { BLOATFLY, OTHER, THIRD }) {
		auto roll = Rolled(GLOWING, LEGENDARY);
		roll.refId = ref;
		f.fake->Deliver(1, Report(roll));
		REQUIRE(Told(*f.fake, 2));
	}
	f.fake->Deliver(1, Protocol::Encode(Protocol::ActorDeath{ BLOATFLY, 0, true }, MT::kReportDeath));
	f.fake->Deliver(1, Protocol::EncodeRelease(std::vector<std::uint32_t>{ OTHER }));
	Sync(f, 1);

	// Bob runs the released one now: his roll is the session's.
	f.fake->Deliver(2, Claim(OTHER));
	auto bobs = Rolled(PLAIN, PLAIN);
	bobs.refId = OTHER;
	f.fake->Deliver(2, Report(bobs));
	const auto taken = Told(*f.fake, 1);
	REQUIRE(taken);
	CHECK(taken->playerId == bob->playerId);

	// Alice leaves: hers go with her. Carol hears Bob's only.
	f.fake->Drop(1);
	REQUIRE(f.fake->WaitFor(2, Type(MT::kPlayerLeft)));
	f.fake->Deliver(2, Claim(THIRD));  // runs it now, with no roll of its own sent
	Sync(f, 2);
	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	const auto told = Told(*f.fake, 3);
	REQUIRE(told);
	CHECK(told->refId == OTHER);
	CHECK(told->playerId == bob->playerId);
	CHECK(!Told(*f.fake, 3, 300ms));
}

// ---- factions ------------------------------------------------------------------------------------------------------

TEST("npc roll: an actor's factions are its base's with its changes over them, the players' own left out")
{
	const std::vector<FactionRank> base{ { SCAVENGERS, 0 }, { WORKSHOP, -1 }, { NpcRoll::PLAYER_FACTION, 0 }, { 0x00000123, 2 } };
	const std::vector<FactionRank> changes{ { SCAVENGERS, -1 }, { SETTLERS, 1 }, { NpcRoll::CURRENT_COMPANION_FACTION, 0 }, { 0x00000123, 3 } };
	CHECK((NpcRoll::Effective(base, changes) == std::vector<FactionRank>{ { 0x00000123, 3 }, { SETTLERS, 1 } }));
	CHECK((NpcRoll::Effective(base, {}) == std::vector<FactionRank>{ { 0x00000123, 2 }, { SCAVENGERS, 0 } }));

	NpcIdentity roll;
	std::vector<FactionRank> many;
	for (std::uint32_t i = 1; i <= Protocol::MAX_IDENTITY_FACTIONS + 5; ++i) {
		many.push_back({ 0x00100000 + i, 0 });
	}
	NpcRoll::SetFactions(roll, many);
	CHECK(roll.factions.size() == Protocol::MAX_IDENTITY_FACTIONS);
	CHECK(roll.flags == (NpcIdentity::kFactions | NpcIdentity::kFactionsCut));
	NpcIdentity few;
	NpcRoll::SetFactions(few, {});
	CHECK(few.flags == NpcIdentity::kFactions);
}

// ---- what a leveled NPC can come out as --------------------------------------------------------------------------

TEST("npc roll: the walk finds what a leveled NPC's lists give, through legendary templates apart, cycles once")
{
	CHECK(!REACH.cut);
	CHECK((REACH.normal == std::unordered_set<std::uint32_t>{ ENC_BLOATFLY, PLAIN, GLOWING, DEEP }));
	CHECK((REACH.legendary == std::unordered_set<std::uint32_t>{ LEGENDARY, PLAIN }));
	CHECK(REACH.Has(LEGENDARY));
	CHECK(!REACH.Has(LIST));  // a list is no NPC
	CHECK(!REACH.Has(0x00099999));

	// Legendary: one of the roll's NPCs only a legendary template gives.
	CHECK(NpcRoll::Legendary(Rolled(GLOWING, LEGENDARY).templates, Rolled(GLOWING, LEGENDARY).mask, REACH));
	CHECK(!NpcRoll::Legendary(Rolled(PLAIN, PLAIN).templates, Rolled(PLAIN, PLAIN).mask, REACH));  // PLAIN: both ways
	CHECK(!NpcRoll::Legendary(Rolled(GLOWING, LEGENDARY).templates, 0b011, REACH));              // not a filled slot

	// A list too long to walk: cut.
	const auto endless = NpcRoll::Walk(1, [](std::uint32_t a_id) {
		return std::optional{ NpcRoll::Node{ true, { { a_id + 1, false } } } };
	});
	CHECK(endless.cut);
	CHECK(endless.normal.size() == NpcRoll::MAX_WALK);
	CHECK(NpcRoll::Walk(ENC_BLOATFLY, [](std::uint32_t) { return std::optional<NpcRoll::Node>{}; }).normal.empty());
}

// ---- making ours the owner's -------------------------------------------------------------------------------------

TEST("npc roll: ours is made their legendary roll at their level")
{
	const auto plan = NpcRoll::Decide(OursPlain(), Rolled(GLOWING, LEGENDARY, 20), Reachable);
	CHECK(plan.roll == Roll::kMake);
	CHECK(plan.templates);
	CHECK(plan.level == std::optional<std::uint16_t>{ 20 });
	CHECK(plan.Any());

	// Already their roll: nothing to do. Only their level differs: that alone.
	CHECK(NpcRoll::Decide(OursPlain(), Rolled(PLAIN, PLAIN), Reachable).roll == Roll::kSame);
	CHECK(!NpcRoll::Decide(OursPlain(), Rolled(PLAIN, PLAIN), Reachable).Any());
	const auto level = NpcRoll::Decide(OursPlain(), Rolled(PLAIN, PLAIN, 30), Reachable);
	CHECK(level.roll == Roll::kMake);
	CHECK(!level.templates);
	CHECK(level.level == std::optional<std::uint16_t>{ 30 });
	// A level of none (or past the most) isn't given.
	CHECK(!NpcRoll::Decide(OursPlain(), Rolled(GLOWING, GLOWING, 0), Reachable).level);
	CHECK(!NpcRoll::Decide(OursPlain(), Rolled(GLOWING, GLOWING, Protocol::MAX_IDENTITY_LEVEL + 1), Reachable).level);
}

TEST("npc roll: a roll ours can't be is left as our game has it, and says why")
{
	// Not leveled there: nothing to give.
	NpcIdentity flat{ .playerId = 1, .refId = BLOATFLY, .level = 5 };
	CHECK(NpcRoll::Decide(OursPlain(), flat, Reachable).roll == Roll::kSame);
	CHECK(!NpcRoll::Decide(OursPlain(), flat, Reachable).Any());
	// Not leveled here (a plugin's own NPC as its base).
	auto plugin = OursPlain();
	plugin.leveled = false;
	CHECK(NpcRoll::Decide(plugin, Rolled(GLOWING, LEGENDARY), Reachable).roll == Roll::kNotLeveled);
	// Placed as another NPC here (a mod changed the reference).
	auto other = OursPlain();
	other.originalBase = 0x00010A99;
	CHECK(NpcRoll::Decide(other, Rolled(GLOWING, LEGENDARY), Reachable).roll == Roll::kOtherOriginal);
	// One of their roll's NPCs has no ID here, or the NPC it was placed as.
	auto hole = Rolled(GLOWING, LEGENDARY);
	hole.templates[3] = 0;
	CHECK(NpcRoll::Decide(OursPlain(), hole, Reachable).roll == Roll::kNotWhole);
	auto noOriginal = Rolled(GLOWING, LEGENDARY);
	noOriginal.originalBase = 0;
	CHECK(NpcRoll::Decide(OursPlain(), noOriginal, Reachable).roll == Roll::kNotWhole);
	// Nothing our lists give (a mod's list here differs).
	const auto stranger = NpcRoll::Decide(OursPlain(), Rolled(0x00077777, LEGENDARY), Reachable);
	CHECK(stranger.roll == Roll::kNotOurs);
	CHECK(!stranger.templates);
	CHECK(!stranger.level);
}

TEST("npc roll: our copy of a friend's spawn is put in their factions and taken out of the rest")
{
	NpcRoll::Ours copy{ .copy = true, .factions = { { SCAVENGERS, 0 }, { WORKSHOP, 0 } } };
	NpcIdentity theirs{ .playerId = 1, .refId = SPAWN, .level = 4 };
	NpcRoll::SetFactions(theirs, { { SETTLERS, 0 }, { WORKSHOP, 1 }, { NpcRoll::PLAYER_FACTION, 0 } });
	const auto plan = NpcRoll::Decide(copy, theirs, Reachable);
	CHECK(plan.roll == Roll::kSame);  // no roll: a plain NPC
	CHECK((plan.add == std::vector<FactionRank>{ { SETTLERS, 0 }, { WORKSHOP, 1 } }));
	CHECK((plan.remove == std::vector<std::uint32_t>{ SCAVENGERS }));
	CHECK(plan.Any());

	// A cut list: nothing taken away.
	auto cut = theirs;
	cut.flags |= NpcIdentity::kFactionsCut;
	CHECK(NpcRoll::Decide(copy, cut, Reachable).remove.empty());
	// No list sent, or not our copy (a plugin's NPC keeps our game's own): factions left alone.
	NpcIdentity unlisted{ .playerId = 1, .refId = SPAWN, .level = 4 };
	CHECK(!NpcRoll::Decide(copy, unlisted, Reachable).Any());
	auto own = copy;
	own.copy = false;
	CHECK(!NpcRoll::Decide(own, theirs, Reachable).Any());
	// The same already.
	NpcRoll::Ours same{ .copy = true, .factions = { { SETTLERS, 0 }, { WORKSHOP, 1 } } };
	CHECK(!NpcRoll::Decide(same, theirs, Reachable).Any());
}

TEST("npc roll: a friend's spawn is placed as the NPC their game placed it as, when that is leveled here too")
{
	constexpr std::uint32_t BASE = 0x00010C00;  // what SpawnBook::PickBase gives
	auto roll = Rolled(GLOWING, LEGENDARY);
	roll.refId = SPAWN;
	CHECK(NpcRoll::PlaceFrom(&roll, BASE, true) == ENC_BLOATFLY);
	CHECK(NpcRoll::PlaceFrom(&roll, BASE, false) == BASE);
	CHECK(NpcRoll::PlaceFrom(nullptr, BASE, true) == BASE);
	auto flat = roll;
	flat.mask = 0;
	CHECK(NpcRoll::PlaceFrom(&flat, BASE, true) == BASE);
	auto runtime = roll;
	runtime.originalBase = 0xFF000999;
	CHECK(NpcRoll::PlaceFrom(&runtime, BASE, true) == BASE);
}

// ---- the books -----------------------------------------------------------------------------------------------------

TEST("npc roll: the owner reports a roll when it changes, and afresh once forgotten")
{
	NpcRoll::Sent sent;
	const auto roll = Rolled(GLOWING, LEGENDARY);
	CHECK(sent.Changed(roll));
	CHECK(!sent.Changed(roll));
	CHECK(sent.Has(BLOATFLY));
	CHECK(sent.Changed(Rolled(GLOWING, LEGENDARY, 13)));
	sent.Forget(BLOATFLY);
	CHECK(!sent.Has(BLOATFLY));
	CHECK(sent.Changed(roll));
	sent.Clear();
	CHECK(sent.Changed(roll));
}

TEST("npc roll: a friend's report is kept and checked, tried a few times apart, then left alone")
{
	NpcRoll::Book book;
	constexpr std::uint32_t US = 2;
	const auto T0 = NpcRoll::Book::Clock::time_point{} + 1h;
	auto ours = Rolled(PLAIN, PLAIN);
	ours.playerId = US;
	CHECK(!book.Learn(ours, US));  // our own
	auto nobody = ours;
	nobody.playerId = 0;
	CHECK(!book.Learn(nobody, US));

	const auto roll = Rolled(GLOWING, LEGENDARY);
	CHECK(book.Learn(roll, US));
	CHECK(!book.Learn(roll, US));  // no news
	REQUIRE(book.Find(BLOATFLY));
	CHECK(book.Find(BLOATFLY)->templates[3] == LEGENDARY);
	CHECK((book.Due() == std::vector<std::uint32_t>{ BLOATFLY }));

	CHECK(book.Ask(BLOATFLY, T0) == NpcRoll::Book::Go::kTry);
	CHECK(book.Ask(BLOATFLY, T0 + 1s) == NpcRoll::Book::Go::kWait);
	CHECK(book.Ask(BLOATFLY, T0 + NpcRoll::Book::RETRY) == NpcRoll::Book::Go::kTry);
	CHECK(book.Ask(BLOATFLY, T0 + 2 * NpcRoll::Book::RETRY) == NpcRoll::Book::Go::kTry);
	CHECK(book.Ask(BLOATFLY, T0 + 3 * NpcRoll::Book::RETRY) == NpcRoll::Book::Go::kGiveUp);
	book.Settled(BLOATFLY);
	CHECK(!book.IsDue(BLOATFLY));

	// Mirrored again: checked again, with no tries left. A new report: tried afresh.
	book.Check(BLOATFLY);
	CHECK(book.IsDue(BLOATFLY));
	CHECK(book.Ask(BLOATFLY, T0 + 10 * NpcRoll::Book::RETRY) == NpcRoll::Book::Go::kGiveUp);
	CHECK(book.Learn(Rolled(GLOWING, LEGENDARY, 14), US));
	CHECK(book.Ask(BLOATFLY, T0 + 10 * NpcRoll::Book::RETRY) == NpcRoll::Book::Go::kTry);
	book.Check(0x00012345);  // no report for it
	CHECK(!book.IsDue(0x00012345));

	book.Forget(BLOATFLY);
	CHECK(!book.Find(BLOATFLY));
	CHECK(!book.IsDue(BLOATFLY));
	CHECK(book.Size() == 0);

	// At most MAX kept.
	for (std::uint32_t i = 0; i < NpcRoll::Book::MAX; ++i) {
		auto each = roll;
		each.refId = 0x00100000 + i;
		REQUIRE(book.Learn(each, US));
	}
	auto past = roll;
	past.refId = 0x00200000;
	CHECK(!book.Learn(past, US));
	CHECK(book.Size() == NpcRoll::Book::MAX);
	book.Clear();
	CHECK(book.Size() == 0);
	CHECK(book.Due().empty());
}
