// Whole sessions (tests/sim/Sim.h) where the rules of separate parts meet: the server's limits on a joiner
// against an honest busy evening and against its own log, the session file and a server restart against
// the loot counts and who runs which NPC, and the server's gear rules against stand-ins after a rejoin.
// Each test names what the players would see go wrong if it failed.

#include "sim/Sim.h"

#include <algorithm>
#include <array>
#include <filesystem>

using namespace Sim;
using Reason = Protocol::ClaimReason;

namespace
{
	constexpr std::uint32_t RAIDER_1 = 0x0002A001;
	constexpr std::uint32_t TRUNK = 0x000AE12F;
	constexpr std::uint32_t CAPS = 0x0000000F;
	constexpr std::uint32_t SANCTUARY = 0x000250FE;
	constexpr std::uint32_t RED_ROCKET = 0x000250FF;
	constexpr std::uint32_t WALL = 0x0001ABCD;
	constexpr std::uint32_t JUNK = 0x0003B001;

	// The server's log lines about holding back player a_id (Server.cpp's wording, by subject).
	std::vector<std::string> LimitLinesAbout(const Session& a_s, std::uint32_t a_id)
	{
		const std::array<std::string, 5> about{ std::format("player {} has ", a_id), std::format("player {} sends too many", a_id), std::format("player {} reports ", a_id),
			std::format("from player {} dropped", a_id), std::format("sent player {} the world", a_id) };
		std::vector<std::string> out;
		for (const auto& line : a_s.LogLines()) {
			if (std::ranges::any_of(about, [&](const std::string& a_text) { return line.find(a_text) != std::string::npos; })) {
				out.push_back(line);
			}
		}
		return out;
	}
}

// ---- the server's limits, its log, and an honest evening ------------------------------------------

// Players attach the server's log to bug reports, so it names nobody (log privacy); the limits a joiner can
// hit (shares of the session's tables, the world-state budget, a kill of someone else's NPC) were logged
// with the name their game gave, a Steam persona.
TEST("cross: a joiner whose game hits every limit is named in the server's log by number only, and everyone else's world keeps exactly their share")
{
	Server::Options options;
	options.maxWorldEntriesPerPlayer = 3;
	options.maxContainerChangesPerPlayer = 3;
	options.worldStateRefill = std::chrono::milliseconds(300);
	Session s{ options };
	auto&   host = s.Host();
	auto&   quill = s.Player("Quillon Ashgrove");
	auto&   carol = s.Player("Carol");
	REQUIRE(host.Join());
	REQUIRE(quill.Join());
	REQUIRE(carol.Join());
	host.Claim(RAIDER_1);  // the host's game runs it
	s.Settle();

	quill.Kill(RAIDER_1);  // never fought it: dropped
	for (std::uint32_t i = 0; i < 5; ++i) {
		quill.Kill(0x0002C000 + i);
		quill.Loot(TRUNK, CAPS, -1);
		quill.PickUp(0x0003C000 + i);
	}
	s.Settle();
	// Reloading again and again: the world state is owed, then sent when the budget refills.
	const auto save = quill.SaveGame();
	for (int i = 0; i < 4; ++i) {
		quill.LoadGame(save);
	}
	s.Settle();
	s.SettleAfter(std::chrono::milliseconds(700));

	const auto id = quill.Id();
	CHECK(s.Logged(std::format("server: death of {:08X} from player {} dropped: player {} runs it", RAIDER_1, id, host.Id())));
	CHECK(s.Logged(std::format("server: player {} has added 3 dead NPC entries this run, their share", id)));
	CHECK(s.Logged(std::format("server: player {} has made 3 container changes this session, their share", id)));
	CHECK(s.Logged(std::format("server: player {} has added 3 picked-up item entries this run, their share", id)));
	CHECK(s.Logged(std::format("server: player {} has asked for the world too often", id)));
	CHECK(s.Logged(std::format("server: sent player {} the world they asked for", id)));
	for (const auto& line : s.LogLines()) {
		for (const auto name : { "Quillon", "Ashgrove", "Carol", "Host" }) {
			if (line.find(name) != std::string::npos) {
				Test::Fail(__FILE__, __LINE__, std::format("the server's log names '{}': {}", name, line));
			}
		}
	}
	CHECK(LimitLinesAbout(s, carol.Id()).empty());
	CHECK(LimitLinesAbout(s, host.Id()).empty());

	// The others' worlds hold the three of each that were kept, and agree; his own game has all five.
	CHECK_AGREE({ &host, &carol });
	CHECK(!carol.WorldNow().dead.contains(RAIDER_1));
	CHECK(carol.WorldNow().dead.size() == 3);
	CHECK(carol.Count(TRUNK, CAPS) == -3);
	CHECK(carol.WorldNow().pickedUp.size() == 3);
	CHECK(quill.WorldNow().dead.size() == 6);
	auto& dave = s.Player("Dave");
	REQUIRE(dave.Join());
	CHECK_AGREE({ &host, &carol, &dave });
}

// The server holds back a joiner who reports more than any fight or build makes (a modified game), on its
// own and all joiners together; an honest evening's busiest moments must stay well inside both, with a
// flooder in the same session.
TEST("cross: an honest evening's big raid, settlement build and take-all are never held back, though a flooding joiner in the same session is, and everyone's world ends up the same")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	auto&   carol = s.Player("Carol");
	auto&   mallory = s.Player("Mallory");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	REQUIRE(carol.Join());
	REQUIRE(mallory.Join());

	// Mallory's game reports two hundred made-up deaths and as many pickups, all at once.
	for (std::uint32_t i = 0; i < 200; ++i) {
		mallory.Kill(0x00B00000 + i);
		mallory.PickUp(0x00B10000 + i);
	}
	s.Settle();

	// The raid on Sanctuary: the host and both friends kill what attacks them.
	std::set<std::uint32_t> raid;
	for (std::uint32_t i = 0; i < 30; ++i) {
		host.Kill(0x0002D000 + i);
		raid.insert(0x0002D000 + i);
	}
	for (std::uint32_t i = 0; i < 25; ++i) {
		bob.Kill(0x0002D100 + i);
		carol.Kill(0x0002D200 + i);
		raid.insert(0x0002D100 + i);
		raid.insert(0x0002D200 + i);
	}
	s.Settle();
	// Rebuilding: Bob puts up two hundred pieces and clears the junk; Carol builds at Red Rocket.
	std::vector<std::uint32_t> walls;
	for (int i = 0; i < 200; ++i) {
		walls.push_back(bob.Build(SANCTUARY, WALL, i * 10.0f, 0.0f));
	}
	for (std::uint32_t i = 0; i < 10; ++i) {
		bob.Scrap(SANCTUARY, JUNK + i);
	}
	for (int i = 0; i < 50; ++i) {
		carol.Build(RED_ROCKET, WALL, 0.0f, i * 10.0f);
	}
	s.Settle();
	for (int i = 0; i < 40; ++i) {
		bob.MoveBuilt(walls[i], i * 10.0f, 50.0f);
	}
	for (int i = 190; i < 200; ++i) {
		bob.Scrap(SANCTUARY, walls[i]);
	}
	// Taking everything from a big stash: one report per kind of item.
	for (std::uint32_t i = 0; i < 60; ++i) {
		carol.Loot(TRUNK, 0x00024000 + i, -(1 + static_cast<std::int32_t>(i % 5)));
	}
	s.Settle();

	CHECK(LimitLinesAbout(s, bob.Id()).empty());
	CHECK(LimitLinesAbout(s, carol.Id()).empty());
	CHECK(LimitLinesAbout(s, host.Id()).empty());
	CHECK(!LimitLinesAbout(s, mallory.Id()).empty());

	// Mallory goes (four is a full session), and a friend who comes later finds it all; everyone honest agrees.
	mallory.Leave();
	auto& dave = s.Player("Dave");
	REQUIRE(dave.Join());
	CHECK_AGREE({ &host, &bob, &carol, &dave });
	for (const auto ref : raid) {
		CHECK(dave.WorldNow().dead.contains(ref));
	}
	std::size_t madeUp = 0;
	for (const auto ref : dave.WorldNow().dead) {
		madeUp += ref >= 0x00B00000 && ref < 0x00B10000;
	}
	CHECK(madeUp < 100);  // her own budget's burst (and what refilled meanwhile), not all she made up
	CHECK(dave.At(SANCTUARY).size() == 190);
	CHECK(dave.At(RED_ROCKET).size() == 50);
	CHECK(dave.Settlements().at({ bob.Id(), walls[0] }).position[1] == 50.0f);
	CHECK(dave.WorldNow().scrapped.size() == 10);
	CHECK(dave.WorldNow().Contents().at(TRUNK).size() == 60);
	for (const auto player : { &host, &bob, &carol, &dave }) {
		CHECK(player->Undecodable() == 0);
	}
}
