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

	Protocol::Destruction Report(std::uint32_t a_ref, float a_health, bool a_destroyed = false, bool a_explode = false)
	{
		return { a_ref, a_health, a_destroyed, a_explode };
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
	for (const auto type : { MessageType::kReportDestruction, MessageType::kDestructions }) {
		const auto back = DecodeDestructions(Encode(report, type));
		REQUIRE(back);
		CHECK(*back == report);
	}
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

	std::vector<std::uint8_t> StateAt(float a_x, float a_y)
	{
		Protocol::PlayerState state;
		state.sequence = 1;
		state.worldspace = 0x0000003C;
		state.x = a_x, state.y = a_y;
		return Protocol::Encode(state);
	}
}

TEST("server destruction: a car a player shot is relayed as theirs, going off only after their shot")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	// No shot of hers: the others get the wreck, not its explosion.
	f.fake->Deliver(1, Destroyed({ Report(WAGON, 360.0f, false, true) }));
	std::uint32_t from = 0;
	auto          got = Relayed(f, 2, &from);
	REQUIRE(got.size() == 1);
	CHECK(from == alice);
	CHECK(got[0].health == 360.0f);
	CHECK(!got[0].explode);
	CHECK(Relayed(f, 1).empty());  // not back to her

	// After a shot of hers, it goes off for them too.
	f.fake->Deliver(1, Shot());
	f.fake->Deliver(1, Destroyed({ Report(WAGON, 240.0f, true, true) }));
	got = Relayed(f, 2);
	REQUIRE(got.size() == 1);
	CHECK(got[0].explode);
	CHECK(got[0].destroyed);

	// A stage her game didn't set anything off for asks for nothing, shot or not.
	f.fake->Deliver(1, Destroyed({ Report(WAGON, 100.0f, true, false) }));
	got = Relayed(f, 2);
	REQUIRE(got.size() == 1);
	CHECK(!got[0].explode);
}

TEST("server destruction: only news is relayed; a wreck is never mended, and comes once")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
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
	JoinAs(f, 1, "Alice");
	f.fake->Deliver(1, Shot());
	f.fake->Deliver(1, Destroyed({ Report(WAGON, 240.0f, true, true), Report(BARREL_REF, 0.0f, true, true) }));
	Clear(f, { 1 });

	JoinAs(f, 2, "Bob");
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

TEST("server destruction: a joiner's explosions stop at their budget, the wrecks don't")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Shot());
	Clear(f, { 1, 2 });
	std::vector<Protocol::Destruction> barrels;
	for (std::uint32_t i = 0; i < 20; ++i) {
		barrels.push_back(Report(0x00100000 + i, 0.0f, true, true));  // a street of barrels at once
	}
	f.fake->Deliver(1, Destroyed(barrels));
	const auto got = Relayed(f, 2);
	REQUIRE(got.size() == 20);
	CHECK(std::ranges::count_if(got, [](const auto& a_entry) { return a_entry.explode; }) == 16);
	CHECK(f.Logged("sets off destructible objects faster than anyone throws"));
}

TEST("server destruction: an explosion report is believed after a shot, as after a throw")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, StateAt(100.0f, 100.0f));
	Clear(f, { 1, 2 });
	const auto blast = Protocol::Encode(Protocol::Explosion{ 0, 0x0001D2EE, 150.0f, 100.0f, 0.0f, 0, 0x0000003C }, MT::kReportExplosion);
	f.fake->Deliver(1, blast);
	CHECK(Count(Received(f, 2), MT::kExplosion) == 0);
	f.fake->Deliver(1, Shot());  // she shot a mine
	f.fake->Deliver(1, blast);
	CHECK(Count(Received(f, 2), MT::kExplosion) == 1);
}

TEST("server destruction: each player has a share of the table")
{
	Server::Options options;
	options.maxWorldEntriesPerPlayer = 3;
	Fixture f{ options };
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
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
		JoinAs(f, 1, "Alice", 0xA11CE);
		f.fake->Deliver(1, Destroyed({ Report(WAGON, 240.0f, true, false) }));
		Clear(f, { 1 });
	}  // stopping saves
	{
		Fixture f{ options };
		JoinAs(f, 2, "Bob");
		const auto got = Relayed(f, 2);
		REQUIRE(got.size() == 1);
		CHECK(got[0].refId == WAGON);
		CHECK(got[0].health == 240.0f);
		CHECK(got[0].destroyed);
		CHECK(!got[0].explode);
	}
}
