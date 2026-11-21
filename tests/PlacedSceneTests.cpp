// A town's scene over placed NPCs played once for the session (src/game/PlacedSceneRules.h): Diamond City's synth
// accusation. The game whose claim the server keeps first runs it; a friend's game whose quest doesn't run follows it, one
// whose own run went yields to it, each with the quest's stages marked done; the runner's end ends theirs; a friend's
// stand-in in a trigger's area has the runner set that stage; and a late joiner follows from the session's state, all
// through the real server.

#include "ServerFixture.h"

#include "game/PlacedSceneRules.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

using namespace TestServer;

namespace
{
	using PlacedSceneRules::Act;
	using PlacedSceneRules::Facts;
	using PlacedSceneRules::Role;

	constexpr std::uint32_t ACCUSATION = 0x0014BF48;
	constexpr std::uint32_t KYLE = 0x14BF68;
	constexpr std::uint32_t RILEY = 0x14BF65;
	// The quest's stages (Fallout4.esm's INDX): 10 its start-up stage, 1000 its shutdown stage.
	constexpr std::uint16_t STAGES[] = { 10, 15, 20, 25, 30, 35, 40, 50, 100, 110, 115, 120, 190, 200, 300, 500, 1000 };

	const PlacedSceneRules::Event& Accusation()
	{
		const auto event = PlacedSceneRules::Find(0, ACCUSATION & 0x00FFFFFF);
		REQUIRE(event);
		return *event;
	}

	// One game's quest, as the game side reads it and changes it (PlacedScenes.cpp).
	struct Game
	{
		bool                       running = false;
		std::vector<std::uint16_t> done;
		bool                       actorsOff = false;
		bool                       stopped = false;

		bool Done(std::uint16_t a_stage) const { return std::ranges::contains(done, a_stage); }

		void Set(std::uint16_t a_stage)
		{
			if (!Done(a_stage)) {
				done.push_back(a_stage);
				std::ranges::sort(done);
			}
		}

		Facts Read() const
		{
			const auto& event = Accusation();
			return { .running = running,
				.claimDone = std::ranges::any_of(event.claims, [&](const PlacedSceneRules::Claim& a_claim) { return Done(a_claim.stage); }),
				.mirror = PlacedSceneRules::IsMirror(STAGES, done, event.shutdown) };
		}

		Role RoleNow() const { return PlacedSceneRules::RoleOf(Read()); }

		// The game's own run: the story manager's start (10), then its trigger's claim.
		void Start()
		{
			running = true;
			Set(10);
		}

		Act Take(std::uint16_t a_stage, bool a_live)
		{
			const auto& event = Accusation();
			const auto  act = PlacedSceneRules::OnFriendStage(event, Read(), a_stage, a_live);
			switch (act) {
			case Act::kMark:
				if (a_stage != event.shutdown) {
					Set(a_stage);
				}
				break;
			case Act::kFollow:
			case Act::kYield:
				for (const auto stage : STAGES) {
					if (stage != event.shutdown) {
						Set(stage);
					}
				}
				break;
			case Act::kEndFollowed:
				actorsOff = true;
				break;
			case Act::kEndYielded:
				running = false;  // stopquest: its shutdown runs
				stopped = true;
				Set(event.shutdown);
				break;
			default:
				break;
			}
			return act;
		}
	};

	std::vector<Protocol::QuestStage> StagesOf(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		return Decoded(a_packets, MT::kQuestStage, Protocol::DecodeQuestStage);
	}

	void Report(Fixture& a_f, PeerId a_peer, std::uint16_t a_stage)
	{
		a_f.fake->Deliver(a_peer, Protocol::Encode(Protocol::QuestStage{ ACCUSATION, a_stage }, MT::kReportQuestStage));
	}
}

TEST("placed scenes: Diamond City's accusation is in the table with its claims, ends, NPCs and the triggers' areas")
{
	const auto& event = Accusation();
	CHECK(PlacedSceneRules::IsClaim(event, 15));
	CHECK(PlacedSceneRules::IsClaim(event, 20));
	CHECK(!PlacedSceneRules::IsClaim(event, 10));
	CHECK(PlacedSceneRules::IsEnd(event, 300));
	CHECK(PlacedSceneRules::IsEnd(event, 1000));
	CHECK(!PlacedSceneRules::IsEnd(event, 30));
	CHECK(event.shutdown == 1000);
	CHECK(PlacedSceneRules::IsActor(event, KYLE));
	CHECK(PlacedSceneRules::IsActor(event, RILEY));
	CHECK(!PlacedSceneRules::IsActor(event, 0x14BF69));  // Kyle's base, not his reference
	CHECK(PlacedSceneRules::Find(0, 0x14BF49) == nullptr);
	CHECK(PlacedSceneRules::Find(1, ACCUSATION & 0x00FFFFFF) == nullptr);

	// Kyle stands in both triggers' areas, a player's feet on the market's floor; the market's far side, the stands'
	// roofs and the city's gate are out of the scene's (20).
	const auto& scene = event.claims[1].area;
	CHECK(event.claims[1].stage == 20);
	CHECK(event.claims[1].prereq == 10);
	CHECK(PlacedSceneRules::Inside(scene, 1853.0f, 1045.0f, -1562.0f));
	CHECK(PlacedSceneRules::Inside(event.claims[0].area, 1853.0f, 1045.0f, -1562.0f));
	CHECK(!PlacedSceneRules::Inside(scene, 1853.0f + 1000.0f, 1045.0f, -1562.0f));
	CHECK(!PlacedSceneRules::Inside(scene, 1853.0f, 1045.0f, -1000.0f));
	CHECK(!PlacedSceneRules::Inside(scene, 1853.0f, 1045.0f, -1800.0f));
}

TEST("placed scenes: a friend's run's marks are told from a game's own run")
{
	const auto& event = Accusation();
	std::vector<std::uint16_t> marks(std::begin(STAGES), std::end(STAGES) - 1);
	CHECK(PlacedSceneRules::IsMirror(STAGES, marks, event.shutdown));
	marks.push_back(1000);
	CHECK(PlacedSceneRules::IsMirror(STAGES, marks, event.shutdown));  // a yielded run, stopped: its shutdown ran
	// The runner's own run, played to its end: security's shot (30), never Kyle unloaded (25).
	const std::vector<std::uint16_t> own{ 10, 15, 20, 30, 40, 50, 100, 110, 115, 120, 190, 200, 300, 500, 1000 };
	CHECK(!PlacedSceneRules::IsMirror(STAGES, own, event.shutdown));
	CHECK(!PlacedSceneRules::IsMirror(STAGES, {}, event.shutdown));
	CHECK(!PlacedSceneRules::IsMirror({}, {}, event.shutdown));

	CHECK(PlacedSceneRules::RoleOf({}) == Role::kNone);
	CHECK(PlacedSceneRules::RoleOf({ .running = true }) == Role::kNone);
	CHECK(PlacedSceneRules::RoleOf({ .running = true, .claimDone = true }) == Role::kRunner);
	CHECK(PlacedSceneRules::RoleOf({ .running = false, .claimDone = true }) == Role::kNone);  // our run ended
	CHECK(PlacedSceneRules::RoleOf({ .running = false, .claimDone = true, .mirror = true }) == Role::kFollower);
	CHECK(PlacedSceneRules::RoleOf({ .running = true, .claimDone = true, .mirror = true }) == Role::kYielded);

	// Only a run of our own reports, going or ended; and only the runner holds the NPCs and shares its guards.
	CHECK(!PlacedSceneRules::Reports({ .running = true }));
	CHECK(PlacedSceneRules::Reports({ .running = true, .claimDone = true }));
	CHECK(PlacedSceneRules::Reports({ .running = false, .claimDone = true }));
	CHECK(!PlacedSceneRules::Reports({ .running = true, .claimDone = true, .mirror = true }));
	CHECK(PlacedSceneRules::Runs({ .running = true, .claimDone = true }));
	CHECK(!PlacedSceneRules::Runs({ .running = false, .claimDone = true }));
	CHECK(!PlacedSceneRules::Runs({ .running = true, .claimDone = true, .mirror = true }));
	CHECK(PlacedSceneRules::Holds({ .running = true, .claimDone = true }));
	CHECK(!PlacedSceneRules::Holds({ .running = true, .claimDone = true, .released = true }));  // the scene is over
	CHECK(PlacedSceneRules::Runs({ .running = true, .claimDone = true, .released = true }));    // its guards still shared
	CHECK(Accusation().released == 200);
}

TEST("placed scenes: a friend's stage is followed, yielded to, marked, ended or ignored as our game's quest stands")
{
	const auto& event = Accusation();
	using PlacedSceneRules::OnFriendStage;

	// The runner ignores the others' stages, a catch-up's claim too; a live claim of theirs came first (the server kept it).
	const Facts runner{ .running = true, .claimDone = true };
	CHECK(OnFriendStage(event, runner, 10, true) == Act::kIgnore);
	CHECK(OnFriendStage(event, runner, 30, true) == Act::kIgnore);
	CHECK(OnFriendStage(event, runner, 1000, true) == Act::kIgnore);
	CHECK(OnFriendStage(event, runner, 15, false) == Act::kIgnore);
	CHECK(OnFriendStage(event, runner, 15, true) == Act::kYield);

	// No run here: the start-up stage of theirs is nothing to us; their claim is followed; their end before any claim
	// reached us (an unclaimed run of theirs that stopped) too is nothing.
	const Facts none{};
	CHECK(OnFriendStage(event, none, 10, true) == Act::kIgnore);
	CHECK(OnFriendStage(event, none, 1000, true) == Act::kIgnore);
	CHECK(OnFriendStage(event, none, 15, false) == Act::kFollow);
	CHECK(OnFriendStage(event, none, 20, true) == Act::kFollow);

	// Our run started, nobody claimed it here: theirs is the session's, live or caught up.
	const Facts started{ .running = true };
	CHECK(OnFriendStage(event, started, 15, true) == Act::kYield);
	CHECK(OnFriendStage(event, started, 15, false) == Act::kYield);
	CHECK(OnFriendStage(event, started, 10, true) == Act::kIgnore);

	// Following: each stage marked, the end ends it.
	const Facts follower{ .running = false, .claimDone = true, .mirror = true };
	CHECK(OnFriendStage(event, follower, 30, true) == Act::kMark);
	CHECK(OnFriendStage(event, follower, 300, true) == Act::kEndFollowed);
	CHECK(OnFriendStage(event, follower, 1000, false) == Act::kEndFollowed);
	const Facts yieldedRun{ .running = true, .claimDone = true, .mirror = true };
	CHECK(OnFriendStage(event, yieldedRun, 30, true) == Act::kMark);
	CHECK(OnFriendStage(event, yieldedRun, 1000, true) == Act::kEndYielded);
}

TEST("placed scenes: a friend's stand-in in a trigger's area has the game that runs the quest set that stage")
{
	const auto& event = Accusation();
	Game game;
	const auto in = [](bool a_15, bool a_20) { return [=](std::size_t a_claim) { return a_claim == 0 ? a_15 : a_20; }; };
	const auto done = [&](std::uint16_t a_stage) { return game.Done(a_stage); };

	CHECK(!PlacedSceneRules::ClaimFor(event, game.Read(), true, done, in(true, true)));  // our quest doesn't run
	game.Start();
	CHECK(!PlacedSceneRules::ClaimFor(event, game.Read(), true, done, in(false, false)));
	CHECK(PlacedSceneRules::ClaimFor(event, game.Read(), true, done, in(true, false)) == std::optional<std::uint16_t>{ 15 });
	// our own player elsewhere (the run started by a friend's stand-in coming into town): our scene couldn't play, so no
	// claim; the friend's own trigger claims in their game (seen on the rig: a claim made so stalled at 20 in both)
	CHECK(!PlacedSceneRules::ClaimFor(event, game.Read(), false, done, in(true, true)));
	CHECK(PlacedSceneRules::ClaimFor(event, game.Read(), true, done, in(false, true)) == std::optional<std::uint16_t>{ 20 });
	game.Set(15);
	CHECK(PlacedSceneRules::ClaimFor(event, game.Read(), true, done, in(true, false)) == std::nullopt);
	CHECK(PlacedSceneRules::ClaimFor(event, game.Read(), true, done, in(true, true)) == std::optional<std::uint16_t>{ 20 });
	game.Set(20);
	CHECK(!PlacedSceneRules::ClaimFor(event, game.Read(), true, done, in(true, true)));

	Game yielded;  // a run that yielded sets none: its stages are marked
	yielded.Start();
	CHECK(yielded.Take(15, true) == Act::kYield);
	CHECK(!PlacedSceneRules::ClaimFor(event, yielded.Read(), true, [&](std::uint16_t a_stage) { return yielded.Done(a_stage); }, in(true, true)));
}

TEST("placed scenes: one accusation for the session through the server; the second claim yields, a late joiner follows, the end ends both")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	// Both players come into Diamond City: the story manager starts the quest in each game.
	Game alice, bob;
	alice.Start();
	bob.Start();

	// Alice's player walks into the market first: her claim reaches the server first and is kept; Bob's, a moment later,
	// is not, and never reaches Alice.
	alice.Set(15);
	CHECK(alice.RoleNow() == Role::kRunner);
	Report(f, 1, 10);
	Report(f, 1, 15);
	bob.Set(15);
	Report(f, 2, 15);
	CHECK(StagesOf(Received(f, 1)).empty());  // Bob's claim was dropped: Alice hears nothing
	const auto toBob = StagesOf(Received(f, 2));
	REQUIRE(toBob.size() == 2);
	for (const auto& stage : toBob) {
		bob.Take(stage.stage, true);
	}
	CHECK(bob.RoleNow() == Role::kYielded);
	CHECK(bob.Done(10) && bob.Done(20) && bob.Done(30));  // his triggers' stages done: they never fire
	CHECK(!bob.Done(1000));

	// Alice's scene plays on.
	for (const std::uint16_t stage : { 20, 30, 40, 50 }) {
		alice.Set(stage);
		Report(f, 1, stage);
	}
	for (const auto& stage : StagesOf(Received(f, 2))) {
		CHECK(bob.Take(stage.stage, true) == Act::kMark);
	}
	CHECK(alice.RoleNow() == Role::kRunner);

	// Carol joins late: the session's state has Alice's run, and her game, whose quest never started, follows it. Her
	// story manager never starts the quest then (its 10 done).
	JoinAs(f, 3, "Carol");
	Game carol;
	std::size_t caughtUp = 0;
	for (const auto& world : WorldStates(Received(f, 3))) {
		for (const auto& stage : world.questStages) {
			if (stage.quest == ACCUSATION) {
				carol.Take(stage.stage, false);
				++caughtUp;
			}
		}
	}
	CHECK(caughtUp == 6);
	CHECK(carol.RoleNow() == Role::kFollower);
	CHECK(carol.Done(10));
	CHECK(!carol.running);
	Clear(f, { 1, 2, 3 });

	// Alice's player leaves Diamond City: her quest stops (300, and its shutdown 1000). Bob's run stops; Carol's NPCs go off.
	alice.Set(300);
	alice.running = false;
	alice.Set(1000);
	Report(f, 1, 300);
	Report(f, 1, 1000);
	for (const auto& stage : StagesOf(Received(f, 2))) {
		bob.Take(stage.stage, true);
	}
	for (const auto& stage : StagesOf(Received(f, 3))) {
		carol.Take(stage.stage, true);
	}
	CHECK(bob.stopped);
	CHECK(!bob.running);
	CHECK(carol.actorsOff);
	CHECK(!carol.running);
	CHECK(StagesOf(Received(f, 1)).empty());  // nothing comes back to the runner
}
