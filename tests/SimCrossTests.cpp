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
	constexpr std::uint32_t STIMPAK = 0x00023736;
	constexpr std::uint32_t SANCTUARY = 0x000250FE;
	constexpr std::uint32_t RED_ROCKET = 0x000250FF;
	constexpr std::uint32_t WALL = 0x0001ABCD;
	constexpr std::uint32_t JUNK = 0x0003B001;
	constexpr std::uint32_t PISTOL = 0x0000DF42;
	constexpr std::uint32_t VAULT_SUIT = 0x0001EED7;

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

	// What player a_id's stand-in wears in a_viewer's game: nothing of theirs (still the settler's own outfit)
	// is told apart from nothing at all.
	std::optional<std::vector<std::uint32_t>> StandInWears(const Client& a_viewer, std::uint32_t a_id)
	{
		const auto seen = a_viewer.FriendById(a_id);
		return seen ? seen->wearing : std::nullopt;
	}

	using Gear = std::vector<std::uint32_t>;
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

// ---- a friend's gear: the server's repeat filter, rejoins and stand-ins ------------------------------

// The server passes on no gear report that is the same as the player's last (a game resending its list had
// everyone strip and dress its stand-in again), and a stand-in is dressed only from what comes. Coming back
// in any way is a new connection's first report, and a newcomer is sent the latest as they join.
TEST("cross: a friend's gear is on their stand-in in every game after a crash and rejoin, a hello again and a server restart, though their game sends the same list each time")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	auto&   carol = s.Player("Carol");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	REQUIRE(carol.Join());
	const Gear kit{ PISTOL, VAULT_SUIT };
	const auto dressed = [&](const char* a_when) {
		for (const auto viewer : { &host, &carol }) {
			if (StandInWears(*viewer, bob.Id()) != kit) {
				Test::Fail(__FILE__, __LINE__, std::format("{}: Bob's stand-in isn't in his gear in {}'s game", a_when, viewer->Name()));
			}
		}
	};
	bob.Move(100.0f, 100.0f);
	bob.Equip(kit);
	s.Settle();
	dressed("at first");
	CHECK(host.Heard(MT::kPlayerEquipment) == 1);

	// His game sends the same list again (a modified one, or after loading a save): nobody hears of it.
	for (int i = 0; i < 3; ++i) {
		bob.Equip(kit);
	}
	s.Settle();
	CHECK(host.Heard(MT::kPlayerEquipment) == 1);
	CHECK(carol.Heard(MT::kPlayerEquipment) == 1);

	// His game crashes and is back before the server noticed: a new stand-in everywhere, dressed again.
	bob.Vanish();
	REQUIRE(bob.Join());
	bob.StandStill(1);
	s.Settle();
	dressed("after a crash and rejoin");
	CHECK(host.Heard(MT::kPlayerEquipment) == 2);

	// His game restarts while Steam keeps the connection: hello again on it.
	bob.HelloAgain();
	s.Settle();
	bob.StandStill(1);
	s.Settle();
	REQUIRE(bob.Welcomed());
	dressed("after saying hello again");

	// The server restarts; Carol comes back last, and is sent his gear as she joins.
	s.RestartServer();
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	REQUIRE(carol.Join());
	bob.StandStill(1);
	s.Settle();
	dressed("after the server restarted");

	// He puts the pistol away for good: the change goes on everywhere (after StandInGear's gap).
	bob.Equip({ VAULT_SUIT });
	s.Settle();
	host.Frames(31);
	carol.Frames(31);
	CHECK(StandInWears(host, bob.Id()) == Gear{ VAULT_SUIT });
	CHECK(StandInWears(carol, bob.Id()) == Gear{ VAULT_SUIT });
	for (const auto player : { &host, &bob, &carol }) {
		CHECK(player->Undecodable() == 0);
	}
}

// ---- the session file, a restart, the loot counts and who runs what ----------------------------------

// The host's game writes the session every 15 s, and a file cut short is carried on from the backup before
// it: the server's table of loot then ends before the games' own counts. Their count must go back to the
// server's end, or the next loot, numbered from there, is taken for some they already have.
TEST("cross: after the server carries on from its backup and lost its last loot, everyone there still gets every loot made since, once, and a newcomer gets the server's")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	host.Loot(TRUNK, CAPS, -1);  // #0 to #2
	bob.Loot(TRUNK, CAPS, -2);
	host.Loot(TRUNK, CAPS, -3);
	s.Settle();
	s.RestartServer();  // written: #0 to #2
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	host.Loot(TRUNK, CAPS, -10);  // #3 and #4
	bob.Loot(TRUNK, STIMPAK, -1);
	s.Settle();
	s.StopServer();  // written again: the first file is the backup now

	// The newest file is cut short (a full disk, the power going), and the server carries on from the backup.
	const std::filesystem::path file = s.ServerOptions().sessionFile;
	std::filesystem::resize_file(file, std::filesystem::file_size(file) / 2);
	s.StartServer();
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	CHECK(s.Logged("carrying on from the backup"));
	host.Loot(TRUNK, CAPS, -100);
	bob.Loot(TRUNK, CAPS, -200);
	s.Settle();
	// What the server lost stays in the worlds that had it; what came since is in each once.
	for (const auto player : { &host, &bob }) {
		CHECK(player->Count(TRUNK, CAPS) == -316);
		CHECK(player->Count(TRUNK, STIMPAK) == -1);
	}
	CHECK_AGREE({ &host, &bob });

	// A friend who joins now gets the session as the server has it, and loot from here on reaches all alike.
	auto& carol = s.Player("Carol");
	REQUIRE(carol.Join());
	CHECK(carol.Count(TRUNK, CAPS) == -306);
	CHECK(carol.Count(TRUNK, STIMPAK) == 0);
	carol.Loot(TRUNK, CAPS, -1000);
	host.Loot(TRUNK, STIMPAK, -4);
	s.Settle();
	CHECK(host.Count(TRUNK, CAPS) == -1316);
	CHECK(bob.Count(TRUNK, CAPS) == -1316);
	CHECK(carol.Count(TRUNK, CAPS) == -1306);
	CHECK(bob.Count(TRUNK, STIMPAK) == -5);
	CHECK(carol.Count(TRUNK, STIMPAK) == -4);
	for (const auto player : { &host, &bob, &carol }) {
		CHECK(player->Undecodable() == 0);
	}
}

// Who runs which NPC isn't saved: after a restart the host's game, loading the raider first, runs it. A friend
// fighting it then is refused the takeover (it just changed hands), and their kill must still count.
TEST("cross: after a server restart, a friend's kill of a raider the host's game took over while they fought it counts, and the loot from its corpse reaches everyone")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	bob.Claim(RAIDER_1);  // his game runs it
	s.Settle();
	REQUIRE(host.OwnerOf(RAIDER_1) == bob.Id());

	s.RestartServer();
	REQUIRE(host.Join());
	host.Claim(RAIDER_1);
	s.Settle();
	REQUIRE(bob.Join());
	bob.Claim(RAIDER_1);
	s.Settle();
	CHECK(bob.OwnerOf(RAIDER_1) == host.Id());  // he is told who runs it now

	// He shoots it: his game asks to take it over, says it is hurt, and that it died.
	bob.Claim(RAIDER_1, Reason::kInteract);
	bob.Hurt(RAIDER_1, 0.2f);
	bob.Kill(RAIDER_1, 10.0f, 20.0f, 0.0f);
	s.Settle();
	CHECK(host.WorldNow().dead.contains(RAIDER_1));
	CHECK(host.OwnerOf(RAIDER_1) == 0);
	CHECK(bob.OwnerOf(RAIDER_1) == 0);

	auto& carol = s.Player("Carol");
	REQUIRE(carol.Join());
	CHECK(carol.WorldNow().dead.contains(RAIDER_1));
	carol.Loot(RAIDER_1, CAPS, -5);
	s.Settle();
	CHECK(host.Count(RAIDER_1, CAPS) == -5);
	CHECK(bob.Count(RAIDER_1, CAPS) == -5);

	// And so it stays for a friend who comes after another restart.
	s.RestartServer();
	for (const auto player : { &host, &bob, &carol }) {
		REQUIRE(player->Join());
	}
	auto& dave = s.Player("Dave");
	REQUIRE(dave.Join());
	CHECK(dave.WorldNow().dead.contains(RAIDER_1));
	CHECK(dave.Count(RAIDER_1, CAPS) == -5);
	CHECK_AGREE({ &host, &bob, &carol, &dave });
}

// ---- reloading faster than the server answers ----------------------------------------------------------

// A world state past a player's budget is owed, not refused, and while it is owed their container changes
// wait in it and the session's building is sent with it (the copies went with the world the load threw away).
TEST("cross: a friend who reloads faster than the server answers gets, once it does, the loot and the building made meanwhile, and nothing twice")
{
	Server::Options options;
	options.worldStateRefill = std::chrono::milliseconds(250);
	Session s{ options };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	const auto first = host.Build(SANCTUARY, WALL, 1.0f, 1.0f);
	host.Loot(TRUNK, CAPS, -5);
	s.Settle();
	const auto save = bob.SaveGame();
	const auto second = host.Build(SANCTUARY, WALL, 2.0f, 2.0f);
	host.Loot(TRUNK, CAPS, -7);
	s.Settle();

	for (int i = 0; i < 5; ++i) {
		bob.LoadGame(save);
	}
	s.Settle();
	// Meanwhile: the host builds and loots, and so does Bob.
	const auto third = host.Build(RED_ROCKET, WALL, 3.0f, 3.0f);
	host.Loot(TRUNK, STIMPAK, -1);
	bob.Loot(TRUNK, CAPS, -2);
	s.Settle();
	s.SettleAfter(std::chrono::milliseconds(600));

	CHECK(s.Logged(std::format("sent player {} the world they asked for", bob.Id())));
	CHECK(bob.Count(TRUNK, CAPS) == -14);
	CHECK(bob.Count(TRUNK, STIMPAK) == -1);
	CHECK((bob.At(SANCTUARY) == std::set<WorkshopKey>{ { host.Id(), first }, { host.Id(), second } }));
	CHECK((bob.At(RED_ROCKET) == std::set<WorkshopKey>{ { host.Id(), third } }));
	CHECK(bob.Copies().size() == 3);
	CHECK_AGREE({ &host, &bob });
	auto& carol = s.Player("Carol");
	REQUIRE(carol.Join());
	CHECK_AGREE({ &host, &bob, &carol });
}
