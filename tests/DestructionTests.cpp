// Destructible objects shared (src/game/DestructionRules.h, the server's handleDestruction): a car one player shoots is
// as wrecked in every game, and goes off once in each.

#include "ServerFixture.h"

#include "game/DestructionRules.h"

#include <limits>
#include <set>

using namespace TestServer;
using DestructionRules::Stage;

namespace
{
	// A station wagon as Fallout4.esm has it (CarStationWagon01, DEST: 500 health): it burns at 80%, a first blast at
	// 75% (CarExplosion), its big one at 50% (CarNukeLargeVehicles), where it becomes the hulk and counts destroyed.
	constexpr std::uint32_t CAR_EXPLOSION = 0x00022496;
	constexpr std::uint32_t CAR_NUKE = 0x00023D3B;
	const std::vector<Stage> CAR{
		{ 80, 0, 0, false },
		{ 75, 0, CAR_EXPLOSION, false },
		{ 70, 0, 0, false },
		{ 50, DestructionRules::STAGE_DESTROY, CAR_NUKE, true },
		{ 0, 0, 0, false },
	};
	constexpr std::uint32_t CAR_HEALTH = 500;
	// An explosive barrel (BarrelTankFlammable02): one hit point, and its one stage blows it up and takes it away.
	constexpr std::uint32_t BARREL_EXPLOSION = 0x000A4C97;
	const std::vector<Stage> BARREL{ { 0, DestructionRules::STAGE_DISABLE | DestructionRules::STAGE_DESTROY, BARREL_EXPLOSION, false } };

	constexpr std::uint32_t WAGON = 0x0001B8AA;
	constexpr std::uint32_t BARREL_REF = 0x0001B8AB;

	constexpr std::uint32_t COMMONWEALTH = 0x0000003C;

	// A report of an object standing at a_x, a_y in the Commonwealth.
	Protocol::Destruction Report(std::uint32_t a_ref, float a_health, bool a_destroyed = false, bool a_explode = false, float a_x = 1000.0f, float a_y = 2000.0f)
	{
		return { a_ref, a_health, a_destroyed, a_explode, 0, COMMONWEALTH, a_x, a_y, 0.0f };
	}

	// The same entry as a relay or the session file has it: no position.
	Protocol::Destruction Unplaced(Protocol::Destruction a_entry)
	{
		a_entry.cell = a_entry.worldspace = 0, a_entry.x = a_entry.y = a_entry.z = 0.0f;
		return a_entry;
	}
}

TEST("destruction rules: the stages passed are the engine's, from the health rounded up to a percent")
{
	CHECK(DestructionRules::Passed(CAR, -1.0f, CAR_HEALTH) == 0);  // never damaged
	CHECK(DestructionRules::Passed(CAR, 500.0f, CAR_HEALTH) == 0);
	CHECK(DestructionRules::Passed(CAR, 401.0f, CAR_HEALTH) == 0);  // 80.2% rounds up to 81
	CHECK(DestructionRules::Passed(CAR, 400.0f, CAR_HEALTH) == 1);  // 80%: burning
	CHECK(DestructionRules::Passed(CAR, 375.0f, CAR_HEALTH) == 2);  // 75%: its first blast
	CHECK(DestructionRules::Passed(CAR, 251.0f, CAR_HEALTH) == 3);
	CHECK(DestructionRules::Passed(CAR, 250.0f, CAR_HEALTH) == 4);  // 50%: the hulk
	CHECK(DestructionRules::Passed(CAR, 1.0f, CAR_HEALTH) == 4);
	CHECK(DestructionRules::Passed(CAR, 0.0f, CAR_HEALTH) == 5);
	CHECK(DestructionRules::Passed(BARREL, 1.0f, 1) == 0);
	CHECK(DestructionRules::Passed(BARREL, 0.0f, 1) == 1);
	CHECK(DestructionRules::Percent(-1.0f, CAR_HEALTH) == 100);
	CHECK(DestructionRules::Percent(1.0f, 0) == 100);  // no maximum: no damage to speak of
}

TEST("destruction rules: a report goes out when a stage the session hasn't seen is passed, with its explosion")
{
	const DestructionRules::Known whole{};
	CHECK(!DestructionRules::Due(WAGON, CAR, CAR_HEALTH, { 450.0f, false }, whole));  // scratched: no stage yet
	const auto burning = DestructionRules::Due(WAGON, CAR, CAR_HEALTH, { 390.0f, false }, whole);
	REQUIRE(burning);
	CHECK(burning->health == 390.0f);
	CHECK(!burning->explode);  // burning sets nothing off
	const auto blast = DestructionRules::Due(WAGON, CAR, CAR_HEALTH, { 360.0f, false }, { 1, false });
	REQUIRE(blast);
	CHECK(blast->explode);
	CHECK(!DestructionRules::Due(WAGON, CAR, CAR_HEALTH, { 355.0f, false }, { 2, false }));  // the same stage: no news
	// Burnt down past two stages at once: the big one is among them.
	const auto hulk = DestructionRules::Due(WAGON, CAR, CAR_HEALTH, { 240.0f, true }, { 2, false });
	REQUIRE(hulk);
	CHECK(hulk->explode);
	CHECK(hulk->destroyed);
	// Destroyed by a script with its health left as it was: news too.
	const auto destroyed = DestructionRules::Due(WAGON, CAR, CAR_HEALTH, { 390.0f, true }, { 1, false });
	REQUIRE(destroyed);
	CHECK(destroyed->destroyed);
	CHECK(!destroyed->explode);
	CHECK(!DestructionRules::Due(WAGON, CAR, CAR_HEALTH, { 390.0f, true }, { 1, true }));
	// The session's further gone than our copy (we took a friend's): nothing to say.
	CHECK(!DestructionRules::Due(WAGON, CAR, CAR_HEALTH, { 390.0f, false }, { 4, true }));
}

TEST("destruction rules: a friend's car goes off here once, for the stages our copy passes now")
{
	// Ours is whole; theirs burnt down to the hulk after their shots: ours becomes the hulk and sets off both blasts.
	const auto plan = DestructionRules::PlanApply(CAR, CAR_HEALTH, {}, Report(WAGON, 240.0f, true, true));
	CHECK(plan.apply);
	CHECK(plan.health == 240.0f);
	CHECK(plan.destroyed);
	CHECK(plan.from == 0);
	CHECK(plan.to == 4);
	CHECK((plan.explosions == std::vector<std::uint32_t>{ CAR_EXPLOSION, CAR_NUKE }));
	CHECK(plan.replaceModel);
	CHECK(!plan.disable);

	// Ours had its first blast already (our own shot): only the big one goes off here.
	const auto later = DestructionRules::PlanApply(CAR, CAR_HEALTH, { 360.0f, false }, Report(WAGON, 240.0f, true, true));
	CHECK((later.explosions == std::vector<std::uint32_t>{ CAR_NUKE }));
	CHECK(later.from == 2);

	// Ours is the hulk already (its own nuke went off here): nothing more, however the report says it went off.
	const auto again = DestructionRules::PlanApply(CAR, CAR_HEALTH, { 240.0f, true }, Report(WAGON, 240.0f, true, true));
	CHECK(!again.apply);
	CHECK(again.explosions.empty());
	const auto lower = DestructionRules::PlanApply(CAR, CAR_HEALTH, { 240.0f, true }, Report(WAGON, 100.0f, true, true));
	CHECK(lower.apply);  // lower still: our copy takes it, and no stage comes of it
	CHECK(lower.explosions.empty());
	CHECK(!lower.replaceModel);

	// The wreck alone (a catch-up, or the server let no explosion through).
	const auto quiet = DestructionRules::PlanApply(CAR, CAR_HEALTH, {}, Report(WAGON, 240.0f, true, false));
	CHECK(quiet.apply);
	CHECK(quiet.explosions.empty());
	CHECK(quiet.replaceModel);

	// A wreck is never mended: a report of more health than ours changes nothing.
	CHECK(!DestructionRules::PlanApply(CAR, CAR_HEALTH, { 100.0f, true }, Report(WAGON, 400.0f, false, true)).apply);

	// A barrel: it goes off and goes away.
	const auto barrel = DestructionRules::PlanApply(BARREL, 1, {}, Report(BARREL_REF, 0.0f, true, true));
	CHECK((barrel.explosions == std::vector<std::uint32_t>{ BARREL_EXPLOSION }));
	CHECK(barrel.disable);
	CHECK(barrel.destroyed);
}

TEST("destruction rules: the watch keeps an object while it changes, and the oldest goes past its size")
{
	DestructionRules::Watch watch;
	const auto              t0 = DestructionRules::Clock::time_point{} + std::chrono::hours(1);
	watch.Add(WAGON, t0);
	watch.Prune(t0 + std::chrono::seconds(20));
	CHECK(watch.Contains(WAGON));
	watch.Changed(WAGON, t0 + std::chrono::seconds(20));  // its stage moved: watched for another half minute
	watch.Prune(t0 + std::chrono::seconds(45));
	CHECK(watch.Contains(WAGON));
	watch.Prune(t0 + std::chrono::seconds(51));
	CHECK(!watch.Contains(WAGON));
	watch.Changed(WAGON, t0);  // not watched: stays so
	CHECK(watch.Size() == 0);

	for (std::uint32_t i = 0; i < DestructionRules::Watch::MAX; ++i) {
		watch.Add(0x00100000 + i, t0 + std::chrono::milliseconds(i));
	}
	watch.Add(WAGON, t0 + std::chrono::seconds(1));
	CHECK(watch.Size() == DestructionRules::Watch::MAX);
	CHECK(watch.Contains(WAGON));
	CHECK(!watch.Contains(0x00100000));  // the one added first
}

TEST("destruction rules: what waits for an object not loaded is merged, and never goes off")
{
	DestructionRules::Waiting waiting;
	waiting.Add(Report(WAGON, 360.0f, false, true));
	waiting.Add(Report(WAGON, 400.0f, true, false));  // a later report says less health was lost, but destroyed
	const auto entry = waiting.Find(WAGON);
	REQUIRE(entry);
	CHECK(entry->health == 360.0f);
	CHECK(entry->destroyed);
	CHECK(!entry->explode);
	CHECK(waiting.Refs() == std::vector<std::uint32_t>{ WAGON });
	waiting.Erase(WAGON);
	CHECK(waiting.Empty());
}

TEST("destruction protocol: a report round trips and a bad one is refused")
{
	using namespace Protocol;
	const Destructions report{ 3, { Report(WAGON, 249.5f, true, true), Report(BARREL_REF, 0.0f) } };
	const auto back = DecodeDestructions(Encode(report, MessageType::kReportDestruction));
	REQUIRE(back);
	CHECK(*back == report);
	// A relay (and the session file, made of them) has no positions: the layout the files of 34 have.
	const auto relayed = Encode(report, MessageType::kDestructions);
	CHECK(relayed.size() + 2 * (4 + 4 + 3 * 4) == Encode(report, MessageType::kReportDestruction).size());
	const auto plain = DecodeDestructions(relayed);
	REQUIRE(plain);
	CHECK((*plain == Destructions{ 3, { Unplaced(report.entries[0]), Unplaced(report.entries[1]) } }));
	auto lost = Report(WAGON, 10.0f);
	lost.y = std::numeric_limits<float>::quiet_NaN();
	CHECK(!DecodeDestructions(Encode(Destructions{ 3, { lost } }, MessageType::kReportDestruction)));  // no place
	CHECK(DecodeDestructions(Encode(Destructions{ 3, { lost } }, MessageType::kDestructions)));         // a relay names none
	const auto bad = [](Destruction a_entry) { return !DecodeDestructions(Encode(Destructions{ 3, { a_entry } }, MessageType::kReportDestruction)); };
	CHECK(bad(Report(0xFF000123, 10.0f)));  // made at runtime: each game's own
	CHECK(bad(Report(PLAYER_REF_ID, 10.0f)));
	CHECK(bad(Report(0, 10.0f)));
	CHECK(bad(Report(WAGON, -1.0f)));
	CHECK(bad(Report(WAGON, std::numeric_limits<float>::quiet_NaN())));
	CHECK(bad(Report(WAGON, std::numeric_limits<float>::infinity())));
	CHECK(bad(Report(WAGON, MAX_DESTRUCTIBLE_HEALTH * 2.0f)));
	CHECK(!DecodeDestructions(Encode(Destructions{ 3, {} }, MessageType::kReportDestruction)));  // names nothing
	auto flags = Encode(report, MessageType::kReportDestruction);
	flags[1 + 4 + 1 + 4 + 4] = 0x04;  // an unknown flag on the first entry
	CHECK(!DecodeDestructions(flags));
	auto cut = Encode(report, MessageType::kReportDestruction);
	cut.pop_back();
	CHECK(!DecodeDestructions(cut));
	// More than a packet holds: the rest is left off, and what goes still reads.
	Destructions many{ 1, {} };
	for (std::uint32_t i = 0; i < 300; ++i) {
		many.entries.push_back(Report(0x00100000 + i, 10.0f));
	}
	const auto first = DecodeDestructions(Encode(many, MessageType::kDestructions));
	REQUIRE(first);
	CHECK(first->entries.size() == MAX_DESTRUCTION_ENTRIES);
}

// --- The server ----------------------------------------------------------------------------------

namespace
{
	std::vector<std::uint8_t> Destroyed(std::vector<Protocol::Destruction> a_entries)
	{
		return Protocol::Encode(Protocol::Destructions{ 0, std::move(a_entries) }, MT::kReportDestruction);
	}

	std::vector<std::uint8_t> Shot()
	{
		return Protocol::Encode(Protocol::Shot{ 0, 0, Protocol::ShotAction::kShot }, MT::kReportShot);
	}

	std::vector<Protocol::Destruction> Relayed(Fixture& a_f, PeerId a_peer, std::uint32_t* a_from = nullptr)
	{
		std::vector<Protocol::Destruction> out;
		for (const auto& batch : Decoded(Received(a_f, a_peer), MT::kDestructions, Protocol::DecodeDestructions)) {
			if (a_from) {
				*a_from = batch.playerId;
			}
			out.insert(out.end(), batch.entries.begin(), batch.entries.end());
		}
		return out;
	}

	std::vector<std::uint8_t> StateAt(float a_x, float a_y, std::uint32_t a_sequence = 1)
	{
		Protocol::PlayerState state;
		state.sequence = a_sequence;
		state.worldspace = COMMONWEALTH;
		state.x = a_x, state.y = a_y;
		return Protocol::Encode(state);
	}

	// A player joined, standing near the objects of these tests (Report puts them at 1000, 2000).
	std::uint32_t JoinNear(Fixture& a_f, PeerId a_peer, std::string a_name, std::uint64_t a_identity = 0)
	{
		const auto id = JoinAs(a_f, a_peer, std::move(a_name), a_identity);
		a_f.fake->Deliver(a_peer, StateAt(1100.0f, 2000.0f));
		return id;
	}
}

TEST("server destruction: a car a player's game blew is relayed as theirs, going off with it whoever shot it")
{
	Fixture    f;
	const auto alice = JoinNear(f, 1, "Alice");
	JoinNear(f, 2, "Bob");
	Clear(f, { 1, 2 });

	// No shot of hers (her companion's, or a fire that spread): it goes off for the others all the same. Each game sets
	// its own off with the stage and deals its blows on the NPCs it runs; the wreck alone was a blow dealt nowhere.
	f.fake->Deliver(1, Destroyed({ Report(WAGON, 360.0f, false, true) }));
	std::uint32_t from = 0;
	auto          got = Relayed(f, 2, &from);
	REQUIRE(got.size() == 1);
	CHECK(from == alice);
	CHECK(got[0].health == 360.0f);
	CHECK(got[0].explode);
	CHECK(Relayed(f, 1).empty());  // not back to her

	f.fake->Deliver(1, Shot());
	f.fake->Deliver(1, Destroyed({ Report(WAGON, 240.0f, true, true) }));
	got = Relayed(f, 2);
	REQUIRE(got.size() == 1);
	CHECK(got[0].explode);
	CHECK(got[0].destroyed);

	// A stage her game didn't set anything off for asks for nothing.
	f.fake->Deliver(1, Destroyed({ Report(WAGON, 100.0f, true, false) }));
	got = Relayed(f, 2);
	REQUIRE(got.size() == 1);
	CHECK(!got[0].explode);
}

TEST("server destruction: a car the shooter set burning goes off from the friend whose copy got there first")
{
	// Found on the rig: each game's copy of a burning car burns on its own, and the friend's (who never shot) passed the
	// big stage first. Its explosion must reach the shooter, or the shooter's game gets the hulk with no blast.
	Fixture f;
	JoinNear(f, 1, "Alice");
	JoinNear(f, 2, "Bob");
	f.fake->Deliver(1, Shot());
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Destroyed({ Report(WAGON, 390.0f, false, false) }));  // burning: no explosion yet
	CHECK(Relayed(f, 2).size() == 1);
	f.fake->Deliver(2, Destroyed({ Report(WAGON, 250.0f, true, true) }));
	auto got = Relayed(f, 1);
	REQUIRE(got.size() == 1);
	CHECK(got[0].explode);

	// An object nobody attacked lately goes off too: a barrel the car's blast set off.
	f.fake->Deliver(2, Destroyed({ Report(BARREL_REF, 0.0f, true, true) }));
	got = Relayed(f, 1);
	REQUIRE(got.size() == 1);
	CHECK(got[0].explode);
}

TEST("server destruction: only news is relayed; a wreck is never mended, and comes once")
{
	Fixture f;
	JoinNear(f, 1, "Alice");
	JoinNear(f, 2, "Bob");
	f.fake->Deliver(1, Shot());
	f.fake->Deliver(2, Shot());
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Destroyed({ Report(WAGON, 240.0f, true, true) }));
	CHECK(Relayed(f, 2).size() == 1);
	// Bob's game blew the same car up on its own before hers reached it: its report is no news.
	f.fake->Deliver(2, Destroyed({ Report(WAGON, 245.0f, true, true) }));
	CHECK(Relayed(f, 1).empty());
	f.fake->Deliver(2, Destroyed({ Report(WAGON, 240.0f, true, true) }));
	CHECK(Relayed(f, 1).empty());
	// Further down: news, as the lowest of the two.
	f.fake->Deliver(2, Destroyed({ Report(WAGON, 50.0f, false, false) }));
	const auto got = Relayed(f, 1);
	REQUIRE(got.size() == 1);
	CHECK(got[0].health == 50.0f);
	CHECK(got[0].destroyed);  // destroyed stays
}

TEST("server destruction: a late joiner gets the wrecks alone, nobody named and nothing going off")
{
	Fixture f;
	JoinNear(f, 1, "Alice");
	f.fake->Deliver(1, Shot());
	f.fake->Deliver(1, Destroyed({ Report(WAGON, 240.0f, true, true), Report(BARREL_REF, 0.0f, true, true) }));
	Clear(f, { 1 });

	JoinNear(f, 2, "Bob");
	std::uint32_t from = 99;
	const auto    got = Relayed(f, 2, &from);
	CHECK(from == 0);
	REQUIRE(got.size() == 2);
	std::set<std::uint32_t> refs;
	for (const auto& entry : got) {
		refs.insert(entry.refId);
		CHECK(!entry.explode);
		CHECK(entry.destroyed);
	}
	CHECK((refs == std::set<std::uint32_t>{ WAGON, BARREL_REF }));
}

TEST("server destruction: a street of barrels going off at once goes off for everyone, beyond any explosion budget")
{
	// Each object's stage comes once in the session, near its reporter: that is the bound. Dropped past a budget, the
	// barrels' blows on the others' NPCs were dealt nowhere (the reporter leaves them to each NPC's own game).
	Fixture f;
	JoinNear(f, 1, "Alice");
	JoinNear(f, 2, "Bob");
	Clear(f, { 1, 2 });
	std::vector<Protocol::Destruction> barrels;
	for (std::uint32_t i = 0; i < 20; ++i) {
		barrels.push_back(Report(0x00100000 + i, 0.0f, true, true));
	}
	f.fake->Deliver(1, Destroyed(barrels));
	const auto got = Relayed(f, 2);
	REQUIRE(got.size() == 20);
	CHECK(std::ranges::count_if(got, [](const auto& a_entry) { return a_entry.explode; }) == 20);
	// They don't take from her budget for explosions reported apart either.
	for (int i = 0; i < 16; ++i) {
		f.fake->Deliver(1, Protocol::Encode(Protocol::Explosion{ 0, 0x0001D2EE, 1150.0f, 2000.0f, 0.0f, 0, COMMONWEALTH }, MT::kReportExplosion));
	}
	CHECK(Count(Received(f, 2), MT::kExplosion) == 16);
}

TEST("server destruction: only an object near its reporter, and where the session first had it")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinNear(f, 2, "Bob");
	f.fake->Deliver(2, Shot());
	Clear(f, { 1, 2 });

	// Alice hasn't said where she is: near nothing.
	f.fake->Deliver(1, Destroyed({ Report(WAGON, 240.0f, true, true) }));
	CHECK(Relayed(f, 2).empty());
	f.fake->Deliver(1, StateAt(90000.0f, 2000.0f));  // across the map
	Clear(f, { 1, 2 });
	f.fake->Deliver(1, Destroyed({ Report(WAGON, 240.0f, true, true) }));
	CHECK(Relayed(f, 2).empty());
	// A car "at her feet" in another space, at the same numbers: dropped.
	auto inside = Report(WAGON, 240.0f, true, true, 90000.0f, 2000.0f);
	inside.cell = 0x0001F00F, inside.worldspace = 0;
	f.fake->Deliver(1, Destroyed({ inside }));
	CHECK(Relayed(f, 2).empty());

	// Bob's, near him: relayed, without its place.
	f.fake->Deliver(2, Destroyed({ Report(WAGON, 360.0f, false, true) }));
	auto got = Relayed(f, 1);
	REQUIRE(got.size() == 1);
	CHECK(got[0].worldspace == 0);
	CHECK(got[0].x == 0.0f);
	// Now the session has it at 1000, 2000: Alice's report of it "at her feet" across the map is dropped, and Bob's of a
	// barrel beside her (well within the loaded cells around him) is not.
	f.fake->Deliver(1, Destroyed({ Report(WAGON, 100.0f, true, false, 90000.0f, 2000.0f) }));
	CHECK(Relayed(f, 2).empty());
	f.fake->Deliver(2, Destroyed({ Report(BARREL_REF, 0.0f, true, false, 12000.0f, 2000.0f) }));
	CHECK(Relayed(f, 1).size() == 1);
	f.fake->Deliver(2, Destroyed({ Report(0x00100001, 0.0f, true, false, 30000.0f, 2000.0f) }));  // past them
	CHECK(Relayed(f, 1).empty());
}

TEST("server destruction: each player has a share of the table")
{
	Server::Options options;
	options.maxWorldEntriesPerPlayer = 3;
	Fixture f{ options };
	JoinNear(f, 1, "Alice");
	JoinNear(f, 2, "Bob");
	Clear(f, { 1, 2 });
	f.fake->Deliver(1, Destroyed({ Report(0x00100001, 0.0f), Report(0x00100002, 0.0f), Report(0x00100003, 0.0f), Report(0x00100004, 0.0f) }));
	CHECK(Relayed(f, 2).size() == 3);
	CHECK(f.Logged("destroyed object entries this run, their share"));
	// One the table has already: still news when it is worse.
	f.fake->Deliver(1, Destroyed({ Report(0x00100001, 0.0f, true) }));
	CHECK(Relayed(f, 2).size() == 1);
}

TEST("server destruction: the wrecks are saved with the session, and a joiner after a restart gets them")
{
	const TempSessionFile file;
	Server::Options       options;
	options.sessionFile = file.path.string();
	{
		Fixture f{ options };
		JoinNear(f, 1, "Alice", 0xA11CE);
		f.fake->Deliver(1, Destroyed({ Report(WAGON, 240.0f, true, false) }));
		Clear(f, { 1 });
	}  // stopping saves
	{
		Fixture f{ options };
		JoinNear(f, 2, "Bob");
		const auto got = Relayed(f, 2);
		REQUIRE(got.size() == 1);
		CHECK(got[0].refId == WAGON);
		CHECK(got[0].health == 240.0f);
		CHECK(got[0].destroyed);
		CHECK(!got[0].explode);
		// The file keeps no places: the first report near its reporter places it again, and holds the rest to that.
		JoinNear(f, 3, "Carol");
		Clear(f, { 2, 3 });
		f.fake->Deliver(2, Destroyed({ Report(WAGON, 200.0f, true, false) }));
		CHECK(Relayed(f, 3).size() == 1);
		f.fake->Deliver(3, StateAt(90000.0f, 2000.0f, 2));
		f.fake->Deliver(3, Destroyed({ Report(WAGON, 100.0f, true, false, 90000.0f, 2000.0f) }));
		CHECK(Relayed(f, 2).empty());
	}
}

TEST("server destruction: past a generous budget of stage explosions a stage is still relayed, with nothing going off")
{
	// A broken or modified game naming every car near the others destroyed with its explosion: bounded by a budget far
	// past any fight (64 at once, then 64 a second), the stages themselves still relayed.
	Fixture f;
	JoinNear(f, 1, "Alice");
	JoinNear(f, 2, "Bob");
	Clear(f, { 1, 2 });
	std::vector<Protocol::Destruction> cars;
	for (std::uint32_t i = 0; i < 100; ++i) {
		cars.push_back(Report(0x00200000 + i, 0.0f, true, true));
	}
	f.fake->Deliver(1, Destroyed(cars));
	const auto got = Relayed(f, 2);
	REQUIRE(got.size() == 100);
	const auto exploding = std::ranges::count_if(got, [](const auto& a_entry) { return a_entry.explode; });
	CHECK(exploding >= 64);
	CHECK(exploding <= 66);  // what refilled while the test ran
}
