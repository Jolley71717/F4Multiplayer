// Whole sessions played through the real server by simulated players (tests/sim/Sim.h): what each
// player's game ends up showing after joining, playing, leaving, crashing, reloading, a server restart
// and lost or reordered packets. Each test names what the players would see go wrong if it failed.

#include "sim/Sim.h"

#include <algorithm>

using namespace Sim;
using Reason = Protocol::ClaimReason;

namespace
{
	// References every game has (from the load order), as the plugin shares them.
	constexpr std::uint32_t RAIDER_1 = 0x0002A001;
	constexpr std::uint32_t RAIDER_2 = 0x0002A002;
	constexpr std::uint32_t RAIDER_3 = 0x0002A003;
	constexpr std::uint32_t RAIDER_4 = 0x0002A004;
	constexpr std::uint32_t BLOATFLY = 0x0002A010;
	constexpr std::uint32_t DOGMEAT = 0x0001D162;
	constexpr std::uint32_t DANSE = 0x0005DE4D;
	constexpr std::uint32_t TRUNK = 0x000AE12F;    // a container everyone loots
	constexpr std::uint32_t CAPS = 0x0000000F;
	constexpr std::uint32_t STIMPAK = 0x00023736;
	constexpr std::uint32_t SANCTUARY = 0x000250FE;  // a workbench
	constexpr std::uint32_t RED_ROCKET = 0x000250FF;
	constexpr std::uint32_t WALL = 0x0001ABCD;       // a base object players build
	constexpr std::uint32_t TURRET = 0x0001ABCE;
	constexpr std::uint32_t JUNK = 0x0003B001;       // a piece of the world a settlement starts with
	constexpr std::uint32_t QUEST = 0x000229A5;
	constexpr std::uint32_t DOOR = 0x0001E0AA;
	constexpr std::uint32_t MUSEUM_SUIT = 0x0004D0A1;  // a power armor frame placed by the game's own plugin
	constexpr std::uint32_t FRAME_BASE = 0x000F5BFF;

	std::set<std::pair<std::uint32_t, std::uint16_t>> Stages(std::initializer_list<std::pair<std::uint32_t, std::uint16_t>> a_stages) { return a_stages; }

	// Nothing the server sent any of them failed to decode.
	void CheckAllDecoded(std::initializer_list<const Client*> a_clients)
	{
		for (const auto client : a_clients) {
			CHECK(client->Undecodable() == 0);
		}
	}
}

// ---- a whole evening -------------------------------------------------------------------------------

TEST("sim: a whole evening: two friends explore, fight, loot, build; one leaves and comes back; the server restarts; a late third friend finds the same world")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	const auto hostId = host.Id();
	const auto bobId = bob.Id();
	const auto session = host.SessionId();
	CHECK(hostId != bobId);
	CHECK(bob.SessionId() == session);
	REQUIRE(bob.FriendById(hostId));
	CHECK(bob.FriendById(hostId)->name == "Host");
	REQUIRE(host.FriendById(bobId));

	// Exploring: each sees the other where they are, running, then standing.
	host.Move(1000.0f, 2000.0f, 0.0f, 300.0f);
	bob.Move(1100.0f, 2000.0f, 0.0f, 150.0f);
	s.Settle();
	{
		const auto seen = bob.FriendById(hostId);
		REQUIRE(seen && seen->state);
		CHECK(seen->state->x == 1000.0f);
		CHECK(seen->standIn.walking);  // the stand-in runs
	}
	host.StandStill();
	s.Settle();
	CHECK(!bob.FriendById(hostId)->standIn.walking);  // ... and stops when they do

	// A fight: the host's game runs the raider it is near; bob sees it move and fall.
	host.Claim(RAIDER_1);
	s.Settle();
	CHECK(bob.OwnerOf(RAIDER_1) == hostId);
	host.RunNpc(RAIDER_1, 1200.0f, 2100.0f);
	s.Settle();
	REQUIRE(bob.NpcStates().contains(RAIDER_1));
	CHECK(bob.NpcStates().at(RAIDER_1).x == 1200.0f);
	host.Kill(RAIDER_1, 1200.0f, 2100.0f, 0.0f);
	bob.Kill(RAIDER_3);  // nobody ran this one
	s.Settle();
	CHECK(bob.WorldNow().dead.contains(RAIDER_1));
	CHECK(bob.OwnerOf(RAIDER_1) == 0);  // the dead run nowhere
	CHECK(host.WorldNow().dead.contains(RAIDER_3));

	// Looting the same trunk, and building at Sanctuary.
	host.Loot(TRUNK, CAPS, -50);
	bob.Loot(TRUNK, STIMPAK, -2);
	bob.Loot(TRUNK, CAPS, 10);
	const auto hostWall = host.Build(SANCTUARY, WALL, 100.0f, 200.0f);
	const auto hostTurret = host.Build(SANCTUARY, TURRET, 150.0f, 200.0f);
	const auto bobWall = bob.Build(SANCTUARY, WALL, 300.0f, 200.0f);
	host.MoveBuilt(hostTurret, 160.0f, 210.0f);
	host.Scrap(SANCTUARY, JUNK);
	host.Stage(QUEST, 10);
	bob.Stage(QUEST, 20);
	s.Settle();
	for (const auto player : { &host, &bob }) {
		CHECK(player->Count(TRUNK, CAPS) == -40);
		CHECK(player->Count(TRUNK, STIMPAK) == -2);
		CHECK(player->WorldNow().scrapped.contains(JUNK));
		CHECK(player->WorldNow().questStages == Stages({ { QUEST, 10 }, { QUEST, 20 } }));
	}
	CHECK((bob.At(SANCTUARY) == std::set<WorkshopKey>{ { hostId, hostWall }, { hostId, hostTurret }, { bobId, bobWall } }));
	CHECK(bob.Settlements().at({ hostId, hostTurret }).position[0] == 160.0f);  // where it was moved to
	CHECK_AGREE({ &host, &bob });

	// Bob goes home for a while; the host plays on.
	bob.Leave();
	CHECK(!host.FriendById(bobId));
	host.Kill(RAIDER_4);
	host.Loot(TRUNK, CAPS, -5);
	const auto hostCrops = host.Build(RED_ROCKET, TURRET, 10.0f, 10.0f);
	s.Settle();

	// He comes back as himself, and finds what happened meanwhile, his own loot not counted again.
	REQUIRE(bob.Join());
	CHECK(bob.Id() == bobId);
	CHECK(bob.WorldNow().dead.contains(RAIDER_4));
	CHECK(bob.Count(TRUNK, CAPS) == -45);
	CHECK(bob.Settlements().contains({ hostId, hostCrops }));
	CHECK(host.FriendById(bobId));
	CHECK_AGREE({ &host, &bob });

	// The host's server restarts from its session file: same session, same IDs, nothing counted twice.
	s.RestartServer();
	CHECK(!host.Welcomed());
	CHECK(!bob.Welcomed());
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	CHECK(s.Logged("loaded the saved session"));
	CHECK(host.Id() == hostId);
	CHECK(bob.Id() == bobId);
	CHECK(bob.SessionId() == session);
	CHECK(bob.Count(TRUNK, CAPS) == -45);
	CHECK(bob.At(SANCTUARY).contains({ hostId, hostWall }));  // the copies came back with the session
	CHECK_AGREE({ &host, &bob });

	// Carol joins late and sees the same world, and both friends where they stand.
	host.Move(500.0f, 500.0f);
	bob.Move(600.0f, 500.0f);
	s.Settle();
	auto& carol = s.Player("Carol");
	REQUIRE(carol.Join());
	s.Settle();
	CHECK_AGREE({ &host, &bob, &carol });
	CHECK((carol.WorldNow().dead == std::set<std::uint32_t>{ RAIDER_1, RAIDER_3, RAIDER_4 }));
	CHECK(carol.Count(TRUNK, CAPS) == -45);
	CHECK(carol.Count(TRUNK, STIMPAK) == -2);
	CHECK(carol.Settlements().size() == 4);
	REQUIRE(carol.FriendById(hostId));
	REQUIRE(carol.FriendById(bobId));
	REQUIRE(carol.FriendById(bobId)->state);
	CHECK(carol.FriendById(bobId)->state->x == 600.0f);
	CheckAllDecoded({ &host, &bob, &carol });
}

// ---- convergence -----------------------------------------------------------------------------------

namespace
{
	// What every world should hold, from what the players did while the server heard them.
	struct Truth
	{
		std::set<std::uint32_t>                                        dead;
		std::map<std::uint32_t, std::map<std::uint32_t, std::int64_t>> contents;
		std::set<std::uint32_t>                                        pickedUp;
		std::set<std::pair<std::uint32_t, std::uint16_t>>              stages;
		std::set<std::uint32_t>                                        scrapped;
	};

	// One random thing a_player does, as the session's dice say. NPCs someone else runs are left alone
	// (in the game a hit takes them over first), as are the ones others touched this round.
	void Act(Session& a_s, Client& a_player, std::size_t a_index, Truth& a_truth, std::set<std::uint32_t>& a_touched)
	{
		auto&       dice = a_s.Random();
		const auto  roll = dice.Between(0, 99);
		const auto  npc = 0x0002B000u + dice.Between(1u, 24u);
		const auto  free = !a_touched.contains(npc) && !a_player.WorldNow().dead.contains(npc) && (a_player.OwnerOf(npc) == 0 || a_player.OwnerOf(npc) == a_player.Id());
		const auto& built = a_player.WorldNow().built;
		if (roll < 12) {
			a_player.Move(dice.Between(-5000.0f, 5000.0f), dice.Between(-5000.0f, 5000.0f), 0.0f, dice.Between(0.0f, 400.0f));
		} else if (roll < 22 && free) {
			a_touched.insert(npc);
			a_player.Claim(npc);
		} else if (roll < 34 && free) {
			a_touched.insert(npc);
			a_player.Kill(npc);
			a_truth.dead.insert(npc);
		} else if (roll < 52) {
			const auto container = 0x000AE100u + dice.Between(0u, 2u);
			const auto item = 0x00023700u + dice.Between(0u, 2u);
			auto       count = dice.Between(-5, 5);
			count = count == 0 ? 1 : count;
			a_player.Loot(container, item, count);
			a_truth.contents[container][item] += count;
		} else if (roll < 58) {
			const auto ref = 0x0003C000u + dice.Between(1u, 30u);
			a_player.PickUp(ref);
			a_truth.pickedUp.insert(ref);
		} else if (roll < 65) {
			const auto stage = std::pair{ QUEST + dice.Between(0u, 1u), static_cast<std::uint16_t>(dice.Between(1, 6) * 10) };
			a_player.Stage(stage.first, stage.second);
			a_truth.stages.insert(stage);
		} else if (roll < 75) {
			a_player.Build(dice.Between(0, 1) ? SANCTUARY : RED_ROCKET, WALL, dice.Between(0.0f, 1000.0f), dice.Between(0.0f, 1000.0f));
		} else if (roll < 81 && !built.empty()) {
			const auto& [ref, item] = dice.Pick(built);
			a_player.MoveBuilt(ref, dice.Between(0.0f, 1000.0f), dice.Between(0.0f, 1000.0f));
		} else if (roll < 86 && !built.empty()) {
			const auto ref = dice.Pick(built).first;
			a_player.Scrap(SANCTUARY, ref);
		} else if (roll < 90) {
			const auto junk = JUNK + dice.Between(0u, 9u);
			a_player.Scrap(SANCTUARY, junk);
			a_truth.scrapped.insert(junk);
		} else if (roll < 95) {
			// Each player has doors of their own here: two players at one door at once is a known bug (below).
			a_player.Door(DOOR + static_cast<std::uint32_t>(a_index) * 16 + dice.Between(0u, 3u), static_cast<std::uint8_t>(dice.Between(0, 1)), static_cast<std::uint8_t>(dice.Between(0, 1)));
		} else {
			a_player.Hurt(npc, dice.Between(0.1f, 1.0f));
		}
	}
}

TEST("sim: whatever three players do, however they come and go and however often the server restarts, their worlds end up the same and hold everything they did")
{
	for (std::uint32_t seed = 1; seed <= 3; ++seed) {
		Session                    s{ {}, seed };
		std::vector<Client*>       players{ &s.Host(), &s.Player("Bob"), &s.Player("Carol") };
		Truth                      truth;
		auto&                      dice = s.Random();
		int                        restarts = 0;
		for (int round = 0; round < 120; ++round) {
			std::set<std::uint32_t> touched;
			for (std::size_t i = 0; i < players.size(); ++i) {
				auto& player = *players[i];
				if (!player.Welcomed()) {
					if (dice.Chance(0.3)) {
						// A server in the host's game turns a friend away while nobody is in (the host's game
						// decides the session's load order); they are let in once someone is.
						const bool anyoneIn = std::ranges::any_of(players, [](const Client* a_other) { return a_other->Welcomed(); });
						if (!player.Join()) {
							REQUIRE(i != 0);
							REQUIRE(!anyoneIn);
							REQUIRE(!player.Rejections().empty());
							CHECK(player.Rejections().back() == Protocol::REJECT_HOST_NOT_READY);
						}
					}
					continue;
				}
				const auto roll = dice.Between(0, 99);
				if (roll < 3) {
					player.Leave();
				} else if (roll < 5) {
					player.Vanish();  // a crash: the server finds out when they come back
				} else {
					Act(s, player, i, truth, touched);
				}
			}
			// At least one restart a seed, halfway if the dice gave none before.
			if (restarts < 2 && (dice.Chance(0.02) || (round == 60 && restarts == 0))) {
				++restarts;
				s.RestartServer();
			}
			s.Settle();
		}
		for (const auto player : players) {
			REQUIRE(player->Join());
		}
		s.Settle();
		const int failuresBefore = Test::Failures();
		CHECK(restarts > 0);
		CHECK(s.Logged("loaded the saved session"));
		CHECK_AGREE(players);
		const auto& world = players[0]->WorldNow();
		CHECK(world.dead == truth.dead);
		// (Loot taken and put back to nothing leaves nothing: a world lists only what a container holds.)
		auto expected = truth.contents;
		for (auto& [container, items] : expected) {
			std::erase_if(items, [](const auto& a_item) { return a_item.second == 0; });
		}
		std::erase_if(expected, [](const auto& a_container) { return a_container.second.empty(); });
		CHECK(world.Contents() == expected);
		CHECK(world.pickedUp == truth.pickedUp);
		CHECK(world.questStages == truth.stages);
		CHECK(world.scrapped == truth.scrapped);
		// What each built and kept is in everyone's game, and nothing else.
		std::set<WorkshopKey> built;
		for (const auto player : players) {
			for (const auto& [ref, item] : player->WorldNow().built) {
				built.insert({ player->Id(), ref });
			}
		}
		for (const auto player : players) {
			const auto settlements = player->Settlements();
			std::set<WorkshopKey> keys;
			for (const auto& [key, item] : settlements) {
				keys.insert(key);
			}
			CHECK(keys == built);
			// ... and none of its own came back as a copy (a second wall on top of its own).
			CHECK(std::ranges::none_of(player->Copies(), [&](const WorkshopKey& a_key) { return a_key.first == player->Id(); }));
			CHECK(player->Undecodable() == 0);
		}
		if (Test::Failures() != failuresBefore) {
			std::printf("  (seed %u, %d server restarts)\n", seed, restarts);
		}
	}
}

// ---- coming back ------------------------------------------------------------------------------------

TEST("sim: a friend whose game crashes and comes back before the server notices keeps their ID, and nobody gets their builds twice")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	const auto bobId = bob.Id();
	const auto wall = bob.Build(SANCTUARY, WALL, 1.0f, 2.0f);
	bob.Loot(TRUNK, CAPS, -7);
	s.Settle();

	bob.Vanish();  // the server still holds the old connection
	REQUIRE(bob.Join());
	CHECK(bob.Id() == bobId);
	CHECK(s.Logged(std::format("dropping the old connection of player {} ", bobId)));
	REQUIRE(host.FriendById(bobId));
	// His own wall isn't sent back to him as someone else's (his game would build a second one on top of
	// it), and his loot isn't counted again.
	CHECK(bob.Copies().empty());
	CHECK(bob.Settlements().size() == 1);
	CHECK((bob.Settlements().contains({ bobId, wall })));
	CHECK(bob.Count(TRUNK, CAPS) == -7);
	CHECK((host.Settlements().contains({ bobId, wall })));
	CHECK(host.Settlements().size() == 1);
	CHECK_AGREE({ &host, &bob });
	CheckAllDecoded({ &host, &bob });
}

TEST("sim: a game that says hello again on the same connection is welcomed again without its loot counted twice")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	host.Loot(TRUNK, CAPS, -3);
	bob.Loot(TRUNK, CAPS, -4);
	s.Settle();
	bob.HelloAgain();  // Steam kept the connection while his game restarted
	s.Settle();
	CHECK(bob.Welcomed());
	CHECK(host.FriendById(bob.Id()));
	CHECK(bob.Count(TRUNK, CAPS) == -7);
	CHECK(host.Count(TRUNK, CAPS) == -7);
	CHECK_AGREE({ &host, &bob });
}

TEST("sim: after the server restarts from its session file, a friend who was away the whole time catches up on everything")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	bob.Loot(TRUNK, STIMPAK, -1);
	s.Settle();
	bob.Leave();
	host.Kill(RAIDER_2);
	host.Loot(TRUNK, CAPS, -20);
	host.Build(RED_ROCKET, TURRET, 5.0f, 5.0f);
	host.Stage(QUEST, 30);
	host.Door(DOOR, Protocol::RefState::kYes, Protocol::RefState::kNo);
	host.PickUp(0x0003C001);
	s.Settle();
	s.RestartServer();  // with Bob still away
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	CHECK(bob.WorldNow().dead.contains(RAIDER_2));
	CHECK(bob.Count(TRUNK, CAPS) == -20);
	CHECK(bob.Count(TRUNK, STIMPAK) == -1);
	CHECK((bob.WorldNow().questStages.contains({ QUEST, 30 })));
	REQUIRE(bob.WorldNow().doors.contains(DOOR));
	CHECK((bob.WorldNow().doors.at(DOOR) == std::pair<std::uint8_t, std::uint8_t>(Protocol::RefState::kYes, Protocol::RefState::kNo)));
	CHECK(bob.WorldNow().pickedUp.contains(0x0003C001));
	CHECK(bob.At(RED_ROCKET).size() == 1);
	CHECK_AGREE({ &host, &bob });
}

// ---- reloading a save -------------------------------------------------------------------------------

TEST("sim: a host who reloads an older save gets back the others' kills and loot since, but not its own loot")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	host.Loot(TRUNK, CAPS, -10);
	bob.Loot(TRUNK, STIMPAK, -3);
	s.Settle();
	const auto save = host.SaveGame();

	bob.Kill(RAIDER_1);
	bob.Loot(TRUNK, CAPS, -5);
	host.Loot(TRUNK, STIMPAK, -1);  // after the save: the reload undoes it in the host's game
	host.Stage(QUEST, 10);
	bob.Stage(QUEST, 20);
	s.Settle();

	host.LoadGame(save);
	s.Settle();
	CHECK(host.WorldNow().dead.contains(RAIDER_1));
	CHECK(host.Count(TRUNK, CAPS) == -15);    // its own -10 from the save, and Bob's -5 sent again
	CHECK(host.Count(TRUNK, STIMPAK) == -3);  // its own -1 isn't replayed: the save has that stimpak back in the trunk
	CHECK(bob.Count(TRUNK, STIMPAK) == -4);
	CHECK((host.WorldNow().questStages == Stages({ { QUEST, 10 }, { QUEST, 20 } })));

	// A friend who joins now sees the session's trunk, as Bob does.
	auto& carol = s.Player("Carol");
	REQUIRE(carol.Join());
	CHECK_AGREE({ &bob, &carol });
	CHECK(carol.Count(TRUNK, STIMPAK) == -4);
}

TEST("sim: a friend who reloads the same save twice doesn't get the session's loot twice")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	host.Loot(TRUNK, CAPS, -10);
	s.Settle();
	const auto save = bob.SaveGame();  // has the host's -10 already
	host.Loot(TRUNK, CAPS, -2);
	s.Settle();
	bob.LoadGame(save);
	s.Settle();
	bob.LoadGame(save);
	s.Settle();
	CHECK(bob.Count(TRUNK, CAPS) == -12);
	CHECK_AGREE({ &host, &bob });
}

// ---- the host's world is the session's --------------------------------------------------------------

TEST("sim: an NPC dead in the host's save is dead for a friend whose game was running it, but a story character isn't, unless the story is shared")
{
	for (const bool shared : { false, true }) {
		Server::Options options;
		options.sharedStory = shared;
		Session s{ options };
		auto&   host = s.Host();
		auto&   bob = s.Player("Bob");
		host.DeadInSave(BLOATFLY);
		host.DeadInSave(DANSE);
		host.Character(DANSE, { .unique = true, .essential = true });
		REQUIRE(host.Join());
		REQUIRE(bob.Join());

		// Both are alive in Bob's game, which takes them on as it loads them.
		bob.Claim(BLOATFLY);
		bob.Claim(DANSE);
		s.Settle();
		CHECK(host.OwnerOf(BLOATFLY) == bob.Id());
		host.CheckFoundDead();
		s.Settle();
		CHECK(bob.WorldNow().dead.contains(BLOATFLY));
		CHECK(bob.OwnerOf(BLOATFLY) == 0);
		CHECK(bob.WorldNow().dead.contains(DANSE) == shared);

		// A late joiner's world agrees with the host's on the bloatfly whatever the story.
		auto& carol = s.Player("Carol");
		REQUIRE(carol.Join());
		CHECK(carol.WorldNow().dead.contains(BLOATFLY));
	}
}

TEST("sim: a friend's game can't make an NPC dead for everyone by saying it found it dead; only the host's can")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	auto&   carol = s.Player("Carol");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	REQUIRE(carol.Join());
	host.Claim(RAIDER_1);
	s.Settle();
	// Carol's save has the raider dead and her game says so, the way the host's game would.
	carol.DeadInSave(RAIDER_1);
	carol.Raw(Protocol::Encode(Protocol::ActorDeath{ .refId = RAIDER_1, .foundDead = true }, MT::kReportDeath));
	s.Settle();
	CHECK(!host.WorldNow().dead.contains(RAIDER_1));
	CHECK(!bob.WorldNow().dead.contains(RAIDER_1));
	CHECK(bob.OwnerOf(RAIDER_1) == host.Id());
}

// ---- power armor ------------------------------------------------------------------------------------

TEST("sim: a friend in the museum's power armor hides everyone's own copy of that suit, a late joiner's too, until they get out")
{
	Session s;
	s.PlaceFrame(MUSEUM_SUIT, { FRAME_BASE, COMMONWEALTH, 1000.0f, 1000.0f, 0.0f });
	auto& host = s.Host();
	auto& bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	host.Move(1010.0f, 1000.0f);  // at the suit
	host.WearFrame(MUSEUM_SUIT);
	s.Settle();
	CHECK(bob.Hides(MUSEUM_SUIT));
	CHECK(!host.Hides(MUSEUM_SUIT));  // the host is in it

	auto& carol = s.Player("Carol");
	REQUIRE(carol.Join());
	CHECK(carol.Hides(MUSEUM_SUIT));

	host.WearFrame(0);
	s.Settle();
	CHECK(!bob.Hides(MUSEUM_SUIT));
	CHECK(!carol.Hides(MUSEUM_SUIT));
}

TEST("sim: a friend who says they wear the museum's suit from across the map doesn't make anyone's copy of it vanish")
{
	Session s;
	s.PlaceFrame(MUSEUM_SUIT, { FRAME_BASE, COMMONWEALTH, 1000.0f, 1000.0f, 0.0f });
	auto& host = s.Host();
	auto& bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	bob.Move(40000.0f, -30000.0f);
	bob.WearFrame(MUSEUM_SUIT);
	s.Settle();
	CHECK(!host.Hides(MUSEUM_SUIT));
	// Out again, and in once more at the suit itself: now it is theirs, and goes from the host's world.
	bob.WearFrame(0);
	s.Settle();
	bob.Move(1000.0f, 1010.0f);
	s.Settle();  // walks over to it first
	bob.WearFrame(MUSEUM_SUIT);
	s.Settle();
	CHECK(host.Hides(MUSEUM_SUIT));
}

TEST("sim: a friend who says they wear the museum's suit at its spot, but names a frame of another kind, doesn't make anyone's copy of it vanish")
{
	// A fresh session: their wear is judged on its own, not left over from an earlier one.
	Session s;
	s.PlaceFrame(MUSEUM_SUIT, { FRAME_BASE, COMMONWEALTH, 1000.0f, 1000.0f, 0.0f });
	auto& host = s.Host();
	auto& bob = s.Player("Bob");
	auto& carol = s.Player("Carol");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	REQUIRE(carol.Join());
	bob.Move(1000.0f, 1010.0f);
	s.Settle();
	auto status = Protocol::PlayerStatus{};
	status.powerArmor = true;
	status.powerArmorFrameRef = MUSEUM_SUIT;
	status.powerArmorFrame = FRAME_BASE + 1;
	bob.Status(status);
	s.Settle();
	REQUIRE(host.FriendById(bob.Id()) && host.FriendById(bob.Id())->wear);  // judged
	CHECK(!host.Hides(MUSEUM_SUIT));
	CHECK(!carol.Hides(MUSEUM_SUIT));
	// A late joiner judges it the same way.
	auto& dave = s.Player("Dave");
	REQUIRE(dave.Join());
	s.Settle();
	REQUIRE(dave.FriendById(bob.Id()) && dave.FriendById(bob.Id())->wear);
	CHECK(!dave.Hides(MUSEUM_SUIT));
}

// ---- adversarial ------------------------------------------------------------------------------------

TEST("sim: a joiner's game that claims what isn't its own changes nothing in anyone else's world")
{
	Session s;
	auto&   host = s.Host();
	auto&   mallory = s.Player("Mallory");
	auto&   carol = s.Player("Carol");
	REQUIRE(host.Join());
	REQUIRE(mallory.Join());
	REQUIRE(carol.Join());
	host.Claim(DOGMEAT, Reason::kCompanion);
	host.Claim(RAIDER_1, Reason::kInteract);  // the host is fighting it right now
	host.RunNpc(DOGMEAT, 10.0f, 10.0f);
	const auto wall = host.Build(SANCTUARY, WALL, 1.0f, 1.0f);
	auto       status = Protocol::PlayerStatus{};
	status.level = 12;
	status.health = 100;
	host.Status(status);
	carol.Kill(RAIDER_2);
	s.Settle();

	// Mallory's game claims the host's companion and the raider, kills and moves them, dresses the dog,
	// scraps the host's wall, speaks as the host, loots impossible amounts, helps herself up and has a
	// dead raider hit Carol.
	mallory.Claim(DOGMEAT, Reason::kCompanion);
	mallory.Claim(RAIDER_1, Reason::kInteract);
	mallory.Kill(DOGMEAT);
	mallory.Raw(Protocol::Encode(Protocol::ActorDeath{ .refId = RAIDER_1, .foundDead = true }, MT::kReportDeath));
	mallory.RunNpc(DOGMEAT, 90000.0f, 90000.0f);
	mallory.NpcGear(DOGMEAT, { 0x000DEAD1 });
	mallory.Scrap(SANCTUARY, wall);
	{
		Protocol::WorkshopItem scrap;  // naming the host as its builder
		scrap.playerId = host.Id();
		scrap.refId = wall;
		scrap.op = Protocol::WorkshopOp::kScrapped;
		scrap.workshop = SANCTUARY;
		mallory.Raw(Protocol::Encode(scrap, MT::kReportWorkshopItem));
	}
	auto spoof = status;
	spoof.playerId = host.Id();
	spoof.level = 99;
	mallory.Raw(Protocol::Encode(spoof, MT::kReportStatus));
	mallory.Raw(Protocol::Encode(Protocol::ContainerChange{ TRUNK, CAPS, 0 }, MT::kReportContainer));
	mallory.Raw(Protocol::Encode(Protocol::ContainerChange{ TRUNK, CAPS, Protocol::MAX_ITEM_COUNT + 1 }, MT::kReportContainer));
	mallory.Revive(mallory.Id());
	mallory.HitPlayer(carol.Id(), 40.0f, RAIDER_2);
	s.Settle();

	for (const auto witness : { &host, &carol }) {
		CHECK(witness->OwnerOf(DOGMEAT) == host.Id());
		CHECK(witness->OwnerOf(RAIDER_1) == host.Id());
		CHECK(!witness->WorldNow().dead.contains(DOGMEAT));
		CHECK(!witness->WorldNow().dead.contains(RAIDER_1));
		CHECK(witness->Count(TRUNK, CAPS) == 0);
		CHECK((witness->Settlements().contains({ host.Id(), wall })));
	}
	REQUIRE(carol.NpcStates().contains(DOGMEAT));
	CHECK(carol.NpcStates().at(DOGMEAT).x == 10.0f);  // where the host's game has it
	CHECK(!carol.NpcGearSeen().contains(DOGMEAT));
	REQUIRE(carol.FriendById(host.Id()) && carol.FriendById(host.Id())->status);
	CHECK(carol.FriendById(host.Id())->status->level == 12);
	REQUIRE(carol.FriendById(mallory.Id()) && carol.FriendById(mallory.Id())->status);
	CHECK(carol.FriendById(mallory.Id())->status->level == 99);  // her lie is told as hers
	CHECK(carol.Damage().empty());
	CHECK(mallory.RevivedBy().empty());
	CHECK(mallory.OwnerOf(DOGMEAT) == host.Id());  // she is told who runs it

	// Someone joining afterwards gets the host's world, not Mallory's.
	auto& dave = s.Player("Dave");
	REQUIRE(dave.Join());
	CHECK((dave.At(SANCTUARY).contains({ host.Id(), wall })));
	CHECK(!dave.WorldNow().dead.contains(DOGMEAT));
	CHECK_AGREE({ &host, &carol, &dave });
	CheckAllDecoded({ &host, &mallory, &carol, &dave });
}

TEST("sim: a game with another load order is turned away while friends play, and nobody sees it arrive")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	auto&   modded = s.Player("Modded");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	modded.LoadOrder(0x9999);
	CHECK(!modded.Join());
	CHECK(!modded.Rejections().empty());
	CHECK(host.Friends().size() == 1);
	CHECK(bob.Friends().size() == 1);
}

// ---- lost, repeated and reordered packets ------------------------------------------------------------

TEST("sim: positions lost, repeated and out of order still leave each player seeing where the other stopped")
{
	for (std::uint32_t seed = 1; seed <= 3; ++seed) {
		const int before = Test::Failures();
		Session   s{ {}, seed };
		auto&     host = s.Host();
		auto&     bob = s.Player("Bob");
		REQUIRE(host.Join());
		REQUIRE(bob.Join());
		const Faults bad{ .drop = 0.3, .duplicate = 0.3, .reorder = 0.3 };
		host.faults = bad;
		bob.faults = bad;
		bob.Claim(DOGMEAT, Reason::kCompanion);
		s.Settle();
		for (int i = 1; i <= 30; ++i) {
			host.Move(i * 100.0f, 0.0f, 0.0f, 300.0f);
			bob.Move(0.0f, i * 50.0f, 0.0f, 150.0f);
			bob.RunNpc(DOGMEAT, 10.0f, i * 50.0f);
			if (i % 5 == 0) {
				s.Settle();
			}
		}
		// They stop; their games keep saying where they stand, as games do.
		for (int i = 0; i < 8; ++i) {
			host.StandStill(1);
			bob.StandStill(1);
			bob.RunNpc(DOGMEAT, 10.0f, 1500.0f);
			s.Settle();
		}
		// What goes reliably is untouched by the losses.
		bob.Kill(RAIDER_1);
		host.Loot(TRUNK, CAPS, -1);
		s.Settle();
		const auto hostSeen = bob.FriendById(host.Id());
		const auto bobSeen = host.FriendById(bob.Id());
		REQUIRE(hostSeen && hostSeen->state && bobSeen && bobSeen->state);
		CHECK(hostSeen->state->x == 3000.0f);
		CHECK(!hostSeen->standIn.walking);
		CHECK(bobSeen->state->y == 1500.0f);
		CHECK(!bobSeen->standIn.walking);
		REQUIRE(host.NpcStates().contains(DOGMEAT));
		CHECK(host.NpcStates().at(DOGMEAT).y == 1500.0f);
		CHECK_AGREE({ &host, &bob });
		if (Test::Failures() != before) {
			std::printf("  (seed %u)\n", seed);
		}
	}
}

TEST("sim: a friend who stops while their stand-in is mid-jump isn't left walking on the spot, nor standing while they run on")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	auto&   carol = s.Player("Carol");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	REQUIRE(carol.Join());
	host.Move(100.0f, 0.0f, 0.0f, 300.0f);
	s.Settle();
	const auto standIn = [&](const Client& a_viewer) -> const StandIn& {
		const auto seen = a_viewer.FriendById(host.Id());
		REQUIRE(seen && seen->state);
		return seen->standIn;
	};
	REQUIRE(standIn(bob).walking);
	REQUIRE(standIn(carol).walking);

	// In Bob's game the host's stand-in is mid-jump when the host stops: the stop is refused, and it lands
	// back in its walk. Carol's takes the stop.
	bob.Stumble(host.Id(), 5, true);
	host.StandStill();
	s.Settle();
	CHECK(!standIn(carol).walking);
	bob.Frames(3);
	REQUIRE(standIn(bob).walking);  // landed walking on the spot
	bob.Frames(30);  // a second
	CHECK(!standIn(bob).walking);   // told again, and it stops
	CHECK(!standIn(carol).walking);

	// The host runs on; in Carol's game their stand-in staggers just then and comes out standing.
	host.Move(500.0f, 0.0f, 0.0f, 300.0f);
	s.Settle();
	REQUIRE(standIn(carol).walking);
	carol.Stumble(host.Id(), 5, false);
	host.Move(600.0f, 0.0f, 0.0f, 300.0f);
	s.Settle();
	carol.Frames(3);
	REQUIRE(!standIn(carol).walking);  // standing while the host runs
	carol.Frames(30);
	CHECK(standIn(carol).walking);
	CHECK(standIn(bob).walking);
}

TEST("sim: kills, quest stages, pickups and scraps that arrive twice or out of order still leave every world the same")
{
	// A connection that breaks the transport's promise: it repeats and reorders even what goes reliably.
	// Container changes are left out: a game counts on the server's numbering arriving in order, which
	// the real transports guarantee.
	for (std::uint32_t seed = 1; seed <= 3; ++seed) {
		const int before = Test::Failures();
		Session   s{ {}, seed };
		auto&     host = s.Host();
		auto&     bob = s.Player("Bob");
		REQUIRE(host.Join());
		REQUIRE(bob.Join());
		bob.faults = { .duplicate = 0.4, .reorder = 0.4, .reliableToo = true };
		auto& dice = s.Random();
		for (int i = 0; i < 40; ++i) {
			auto& who = dice.Chance(0.5) ? host : bob;
			switch (dice.Between(0, 3)) {
			case 0:
				who.Kill(0x0002C000 + dice.Between(1u, 15u));
				break;
			case 1:
				who.Stage(QUEST, static_cast<std::uint16_t>(dice.Between(1, 9) * 10));
				break;
			case 2:
				who.PickUp(0x0003C000 + dice.Between(1u, 15u));
				break;
			default:
				who.Scrap(SANCTUARY, JUNK + dice.Between(0u, 9u));
				break;
			}
			if (i % 4 == 0) {
				s.Settle();
			}
		}
		s.Settle();
		CHECK_AGREE({ &host, &bob });
		if (Test::Failures() != before) {
			std::printf("  (seed %u)\n", seed);
		}
	}
}

// ---- known bugs -------------------------------------------------------------------------------------

TEST_KNOWN_BUG("sim: two friends working the same door at the same moment end up seeing it the same way",
	"a door's state is relayed to the others but not echoed to its reporter in the server's order, so the "
	"reporter keeps its own state while everyone else (and the session) keeps the later one")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	using Door = std::pair<std::uint8_t, std::uint8_t>;
	const Door opened{ Protocol::RefState::kYes, Protocol::RefState::kNo };
	const Door shut{ Protocol::RefState::kNo, Protocol::RefState::kNo };
	host.Door(DOOR, opened.first, opened.second);  // both act before either hears of the other
	bob.Door(DOOR, shut.first, shut.second);
	s.Settle();
	auto& carol = s.Player("Carol");
	REQUIRE(carol.Join());
	const auto doorOf = [](const Client& a_player) { return a_player.WorldNow().doors.contains(DOOR) ? a_player.WorldNow().doors.at(DOOR) : Door{}; };
	// The server heard the host first and Bob last, so the session (and a late joiner) has Bob's shut
	// door, and the host was told of it.
	CHECK(doorOf(carol) == shut);
	CHECK(doorOf(host) == shut);
	// The bug: Bob was told of the host's open door after shutting it, and nothing ever tells him the
	// session kept his own.
	KNOWN_CHECK(doorOf(bob) == shut);
}
