// Whole sessions (tests/sim/Sim.h) where the rules of separate parts meet: the server's limits on a joiner
// against an honest busy evening and against its own log, the session file and a server restart against
// the loot counts and who runs which NPC, and the server's gear rules against stand-ins after a rejoin.
// Each test names what the players would see go wrong if it failed.

#include "sim/Sim.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <regex>

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

// ---- a whole settlement at once: the game's send line against the server's event budget -------------

namespace
{
	constexpr std::uint32_t JUNK_FIELD = 0x0004C000;  // a big settlement's junk, pieces of the world itself
	constexpr std::uint32_t BOTTLE = 0x0004B001;

	struct Burst
	{
		std::vector<std::uint32_t> built;
		std::vector<std::uint32_t> scrapped;
	};

	// A blueprint placed, then "scrap all" on the settlement's junk: a_count reports from one game in a single
	// moment, two of every three a piece built and the third a piece of the world scrapped.
	Burst BuildAndScrapAtOnce(Client& a_player, std::uint32_t a_count)
	{
		Burst out;
		for (std::uint32_t i = 0; i < a_count; ++i) {
			if (i % 3 == 2) {
				a_player.Scrap(SANCTUARY, JUNK_FIELD + i);
				out.scrapped.push_back(JUNK_FIELD + i);
			} else {
				out.built.push_back(a_player.Build(SANCTUARY, WALL, static_cast<float>(i % 40) * 10.0f, static_cast<float>(i / 40) * 10.0f));
			}
		}
		return out;
	}

	// Whether a_viewer's game shows all of a_player's burst: every piece built standing at Sanctuary, every
	// piece of junk gone.
	bool ShowsAll(const Client& a_viewer, const Client& a_player, const Burst& a_burst)
	{
		const auto at = a_viewer.At(SANCTUARY);
		const bool built = std::ranges::all_of(a_burst.built, [&](std::uint32_t a_ref) { return at.contains({ a_player.Id(), a_ref }); });
		const bool scrapped = std::ranges::all_of(a_burst.scrapped, [&](std::uint32_t a_ref) { return a_viewer.WorldNow().scrapped.contains(a_ref); });
		return built && scrapped;
	}
}

// The server takes at most 500 events a second from a player and drops the rest, with whatever else of theirs
// came that second. A blueprint placed, or "scrap all" in a big settlement, reports every piece in one moment:
// the others never saw most of them, nor a kill made right after. The game now lets its settlement reports out
// 200 a second (SendPacer), the rest waiting in line, here through the real line at the passing of real time
// into the real server.
TEST("cross: fifteen hundred pieces built and scrapped at once all reach a friend and a late joiner, and a kill, a pickup and a reload made meanwhile get through")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	const auto worldStates = bob.Heard(MT::kWorldState);

	const auto burst = BuildAndScrapAtOnce(bob, 1500);
	const auto save = bob.SaveGame();
	s.Pass(std::chrono::milliseconds(100));
	CHECK(bob.WorkshopUnsent() > 1000);  // most of it still waits its turn
	// Meanwhile Bob reloads (his game asks for the session's world again), kills a raider and picks a bottle up.
	bob.LoadGame(save);
	bob.Kill(RAIDER_1);
	bob.PickUp(BOTTLE);
	for (int step = 0; step < 200 && bob.WorkshopUnsent() > 0; ++step) {
		s.Pass(std::chrono::milliseconds(100));
	}
	CHECK(bob.WorkshopUnsent() == 0);
	s.Settle();

	CHECK(!s.Logged("too many events"));
	CHECK(LimitLinesAbout(s, bob.Id()).empty());
	CHECK(bob.Heard(MT::kWorldState) == worldStates + 1);
	CHECK(ShowsAll(host, bob, burst));
	CHECK(host.WorldNow().dead.contains(RAIDER_1));
	CHECK(host.WorldNow().pickedUp.contains(BOTTLE));

	auto& dave = s.Player("Dave");
	REQUIRE(dave.Join());
	CHECK(ShowsAll(dave, bob, burst));
	CHECK(dave.At(SANCTUARY).size() == burst.built.size());
	CHECK(dave.WorldNow().scrapped.size() == burst.scrapped.size());
	CHECK(dave.WorldNow().dead.contains(RAIDER_1));
	CHECK(dave.WorldNow().pickedUp.contains(BOTTLE));
	CHECK_AGREE({ &host, &bob, &dave });
}

// What the line is for: the same moment sent all at once, as the game did before it, is cut by the server's
// event budget. Should this ever pass (the budget raised, the server counting otherwise), the test above no
// longer shows anything.
TEST("cross: the same fifteen hundred sent all at once, as the game used to, lose pieces and the kill made with them, though the reload's request is still answered")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	bob.PaceWorkshop(false);
	const auto worldStates = bob.Heard(MT::kWorldState);

	const auto burst = BuildAndScrapAtOnce(bob, 1500);
	const auto save = bob.SaveGame();
	s.Pass(std::chrono::milliseconds(100));
	bob.LoadGame(save);
	bob.Kill(RAIDER_1);
	s.Settle();

	CHECK(s.Logged(std::format("server: player {} sends too many events", bob.Id())));
	CHECK(!ShowsAll(host, bob, burst));
	CHECK(host.At(SANCTUARY).size() <= 500);
	CHECK(!host.WorldNow().dead.contains(RAIDER_1));
	// A request for the world is no event: its game asks once and never again.
	CHECK(bob.Heard(MT::kWorldState) == worldStates + 1);
}

// A reload's request for the session's world is sent once and never again: dropped, the player's world lacked the
// session's loot and the others' building until they rejoined.
TEST("cross: a reload's request for the world is answered in a second its player has already spent their event budget")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	const auto save = bob.SaveGame();
	const auto worldStates = bob.Heard(MT::kWorldState);
	// After Bob's save: his reload lacks them until the server sends them again.
	host.Loot(TRUNK, STIMPAK, -1);
	const auto wall = host.Build(RED_ROCKET, WALL, 1.0f, 1.0f);
	s.Settle();

	for (std::uint32_t i = 0; i < 600; ++i) {
		bob.Raw(Protocol::Encode(Protocol::RefPickedUp{ 0x00B10000 + i }, MT::kReportPickup));
	}
	bob.LoadGame(save);
	s.Settle();

	CHECK(s.Logged(std::format("server: player {} sends too many events", bob.Id())));
	CHECK(bob.Heard(MT::kWorldState) == worldStates + 1);
	CHECK(bob.Count(TRUNK, STIMPAK) == -1);
	CHECK(bob.Copies().contains({ host.Id(), wall }));
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

// A first report with nothing on is passed on (the stand-in is undressed), but a newcomer was sent a friend's
// last report only when it named something: theirs stayed in the settler's own outfit.
TEST("cross: a friend with nothing on is shown with nothing on to a friend who joins later, as to those already there")
{
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	bob.Move(100.0f, 100.0f);
	bob.Equip({});
	s.Settle();
	CHECK(StandInWears(host, bob.Id()) == Gear{});

	auto& carol = s.Player("Carol");
	REQUIRE(carol.Join());
	bob.StandStill(1);
	s.Settle();
	CHECK(StandInWears(carol, bob.Id()) == Gear{});

	// Dressed, then everything off again: the same for whoever joins after.
	bob.Equip({ PISTOL, VAULT_SUIT });
	s.Settle();
	bob.Equip({});
	s.Settle();
	host.Frames(31);
	carol.Frames(31);
	auto& dave = s.Player("Dave");
	REQUIRE(dave.Join());
	bob.StandStill(1);
	s.Settle();
	for (const auto viewer : { &host, &carol, &dave }) {
		CHECK(StandInWears(*viewer, bob.Id()) == Gear{});
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

// Two friends shoot one raider the host's game runs. Each of the host's hits claims it again (NpcSync's
// OnLocalInteraction, 2 s apart at most), so Bob's hits, which used to take it over every cooldown and restart its AI in
// his game each time, are refused the takeover while the host keeps hitting it; his damage reaches the host's game all
// the same. Once the host stops, Bob's next hit takes it over as before, and his kill counts.
TEST("cross: two friends shooting the host's raider leave it with the host while the host keeps hitting it, every hit lands, and it goes to the friend once the host stops")
{
	Server::Options options;
	options.takeoverCooldown = std::chrono::seconds(400);  // long, crossed on the server's clock (Session::Advance), not by sleeps
	Session s{ options };
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	host.Claim(RAIDER_1);
	s.Settle();
	REQUIRE(bob.OwnerOf(RAIDER_1) == host.Id());
	host.TakeDamageHeard();

	// Six rounds, each within the cooldown of the last: both hit it, the host's game claims it again with its hit and
	// reports its health, Bob's says what it took off its copy and asks to take it over.
	float health = 1.0f;
	for (int round = 0; round < 6; ++round) {
		s.Advance(std::chrono::seconds(150));
		host.Claim(RAIDER_1, Reason::kInteract);
		health -= 0.1f;
		host.Hurt(RAIDER_1, health);
		bob.Damage(RAIDER_1, 0.05f);
		bob.Claim(RAIDER_1, Reason::kInteract);
		s.Settle();
		CHECK(bob.OwnerOf(RAIDER_1) == host.Id());
		CHECK(host.OwnerOf(RAIDER_1) == host.Id());
		const auto heard = host.TakeDamageHeard();
		REQUIRE(heard.size() == 1);
		CHECK(heard[0].refId == RAIDER_1);
		CHECK(bob.HealthHeard(RAIDER_1) == health);
	}

	// The host stops shooting. Past the cooldown, Bob's next hit takes it over, and his kill is believed.
	s.Advance(std::chrono::seconds(500));
	bob.Damage(RAIDER_1, 0.05f);
	bob.Claim(RAIDER_1, Reason::kInteract);
	s.Settle();
	CHECK(bob.OwnerOf(RAIDER_1) == bob.Id());
	CHECK(host.OwnerOf(RAIDER_1) == bob.Id());
	bob.Kill(RAIDER_1, 10.0f, 20.0f, 0.0f);
	s.Settle();
	CHECK(host.WorldNow().dead.contains(RAIDER_1));
	CHECK(host.OwnerOf(RAIDER_1) == 0);
	CHECK_AGREE({ &host, &bob });
}

// A raider the host's game made at runtime (a random encounter's) is shared as a spawn: the host's game runs it for
// good, and Bob's places a copy. Bob's hits ask to take it over as any hit does (NpcClaims::Book::OnHit, a kFight),
// which the server never grants, with no cooldown of the host's to wait out: his damage reaches the host's game, and his
// kill is believed because his claims and damage noted him fighting it.
TEST("cross: a friend shooting a raider the host's game spawned never takes it over, every hit lands in the host's game, and the friend's kill of it counts")
{
	constexpr std::uint32_t SPAWN = 0xFF001234;        // the spawn id the host's game shares it under
	constexpr std::uint32_t RAIDER_BASE = 0x0001D4D2;  // a plugin's NPC base, which every game has
	Session s;
	auto&   host = s.Host();
	auto&   bob = s.Player("Bob");
	REQUIRE(host.Join());
	REQUIRE(bob.Join());
	host.Spawn(SPAWN, RAIDER_BASE, 1000.0f, 2000.0f);
	s.Settle();
	REQUIRE(bob.CopyOf(SPAWN));
	CHECK(bob.CopyOf(SPAWN)->playerId == host.Id());
	CHECK(bob.CopyOf(SPAWN)->base == RAIDER_BASE);
	CHECK(!host.CopyOf(SPAWN));  // its own: it has it
	CHECK(bob.OwnerOf(SPAWN) == host.Id());
	CHECK(host.OwnerOf(SPAWN) == host.Id());
	host.TakeDamageHeard();

	// Bob shoots it for half a minute while the host never does: each round is past the server's takeover cooldown,
	// which would have handed a plugin's raider over on the first. It stays the host's, and the host hears every hit.
	for (int round = 0; round < 5; ++round) {
		s.Advance(std::chrono::seconds(6));
		bob.Damage(SPAWN, 0.1f);
		bob.Claim(SPAWN, Reason::kInteract);
		s.Settle();
		CHECK(bob.OwnerOf(SPAWN) == host.Id());
		CHECK(host.OwnerOf(SPAWN) == host.Id());
		CHECK(bob.CopyOf(SPAWN));
		const auto heard = host.TakeDamageHeard();
		REQUIRE(heard.size() == 1);
		CHECK(heard[0].refId == SPAWN);
		CHECK(heard[0].health == 0.1f);
	}
	CHECK(bob.TakeDamageHeard().empty());  // his own damage comes back to nobody but the runner

	// His kill: believed, since he was fighting it; the host's game kills its raider, and the spawn is over everywhere.
	bob.Died(SPAWN, true);
	s.Settle();
	CHECK(host.WorldNow().dead.contains(SPAWN));
	CHECK(host.KilledBy(SPAWN) == bob.Id());
	CHECK(host.OwnerOf(SPAWN) == 0);
	CHECK(bob.OwnerOf(SPAWN) == 0);
	CHECK(!bob.CopyOf(SPAWN));
	CHECK_AGREE({ &host, &bob });
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

// ---- the server's log against the soak scripts that read it ------------------------------------------

namespace
{
	// How many of a_lines a soak script's pattern finds (std::regex reads the scripts' patterns as .NET does;
	// their "(?i)" is a_icase here).
	std::size_t CountMatches(const std::vector<std::string>& a_lines, const std::string& a_pattern, bool a_icase = false)
	{
		const std::regex re{ a_pattern, a_icase ? std::regex::ECMAScript | std::regex::icase : std::regex::ECMAScript };
		return static_cast<std::size_t>(std::ranges::count_if(a_lines, [&](const std::string& a_line) { return std::regex_search(a_line, re); }));
	}

	// The lines a_lines has that a soak script stops on (tools/botsoak.ps1's error scan, tools/soak.ps1's
	// "nothing about" list).
	std::vector<std::string> ProblemLines(const std::vector<std::string>& a_lines)
	{
		const std::regex error{ R"(\b(error|assert\w*|failed|could not|exception)\b)", std::regex::ECMAScript | std::regex::icase };
		const std::regex nothingAbout{ "too many|no hello|load order changed|Server is full|wrong password|error|failed", std::regex::ECMAScript | std::regex::icase };
		std::vector<std::string> out;
		std::ranges::copy_if(a_lines, std::back_inserter(out), [&](const std::string& a_line) { return std::regex_search(a_line, error) || std::regex_search(a_line, nothingAbout); });
		return out;
	}
}

// tools/soak.ps1 and tools/botsoak.ps1 judge a release by the server's log: they wait for these lines, count
// them, and fail on anything that reads like an error. A rewording (as the log's privacy pass made) makes a
// soak abort, or pass without checking, long after the change that did it.
TEST("cross: an evening of joins, a leave, a crash and a server restart logs every line the soak scripts wait for and count, and nothing they fail on")
{
	Server::Options options;
	options.maxPlayers = 8;
	Session s{ options };
	auto&   raider = s.Host("Raider");
	auto&   owner = s.Player("Owner");
	auto&   looter = s.Player("Looter");
	auto&   rejoiner = s.Player("Rejoiner");
	auto&   late = s.Player("Late");
	REQUIRE(raider.Join());
	REQUIRE(owner.Join());
	REQUIRE(looter.Join());
	REQUIRE(rejoiner.Join());
	s.Settle();

	looter.Loot(TRUNK, CAPS, -10);
	looter.PickUp(0x0003C001);
	raider.Kill(RAIDER_1);
	s.Settle();

	rejoiner.Leave();  // leaves
	s.Settle();
	REQUIRE(rejoiner.Join());  // comes back
	s.Settle();
	rejoiner.Vanish();  // their game crashes; the server still holds the old connection
	REQUIRE(rejoiner.Join());
	s.Settle();
	REQUIRE(late.Join());
	s.Settle();
	for (auto* player : { &raider, &owner, &looter, &rejoiner, &late }) {
		player->Leave();
	}
	s.Settle();
	s.StopServer();
	const auto firstRun = s.LogLines();

	s.StartServer();
	REQUIRE(raider.Join());
	s.Settle();
	raider.Leave();
	s.Settle();
	s.StopServer();
	const auto all = s.LogLines();
	const std::vector<std::string> secondRun(all.begin() + static_cast<std::ptrdiff_t>(firstRun.size()), all.end());

	for (const auto* run : { &firstRun, &secondRun }) {
		CHECK(CountMatches(*run, R"(listening on UDP port \d+)") == 1);
		CHECK(CountMatches(*run, "server: stopped") == 1);
	}
	CHECK(CountMatches(secondRun, R"(server: loaded the saved session \()") == 1);

	const auto back = rejoiner.Id();
	for (const auto* player : { &raider, &owner, &looter, &late }) {
		CHECK(CountMatches(firstRun, std::format("server: player {} joined from ", player->Id())) == 1);
		CHECK(CountMatches(firstRun, std::format("server: player {} left", player->Id())) == 1);
	}
	CHECK(CountMatches(firstRun, std::format("server: player {} joined from ", back)) == 1);
	CHECK(CountMatches(firstRun, std::format("server: player {} came back from ", back)) == 2);
	CHECK(CountMatches(firstRun, std::format("dropping the old connection of player {} ", back)) == 1);
	CHECK(CountMatches(firstRun, std::format("server: player {} left", back)) == 3);  // left, crashed, went home

	CHECK(CountMatches(all, "Raider|Owner|Looter|Rejoiner|Late") == 0);
	for (const auto& line : ProblemLines(all)) {
		Test::Fail(__FILE__, __LINE__, "a soak script fails on this line: " + line);
	}
}

// The abuse soak (tools/soak.ps1 -Abuse) floods the server from one bot after another and counts each limit
// line once, by player number; the honest player must never be named in one.
TEST("cross: a session of flooders, one after another, logs each limit once in the words the abuse soak counts, never one about the honest player, and names nobody")
{
	Server::Options options;
	options.maxPlayers = 8;
	options.maxWorldEntries = 10;  // the abuse soak's --max-world-entries 1200 --max-share 1000, scaled down
	options.maxWorldEntriesPerPlayer = 8;
	options.maxContainerChangesPerPlayer = 8;
	options.maxWorkshopItemsPerPlayer = 8;
	options.worldStateRefill = std::chrono::milliseconds(200);
	Session s{ options };
	auto&   host = s.Host();  // opens the session and does nothing else (the soak's server is a dedicated one)
	auto&   honest = s.Player("Honest");
	auto&   looper = s.Player("Looper");
	REQUIRE(host.Join());
	REQUIRE(honest.Join());
	REQUIRE(looper.Join());
	honest.Loot(TRUNK, CAPS, -10);
	honest.Kill(RAIDER_1);
	honest.PickUp(0x0003C001);
	s.Settle();

	// The looper's game says hello again and again (a rejoin loop), past the world-state budget.
	for (int i = 0; i < 6; ++i) {
		looper.HelloAgain();
	}
	s.Settle();

	std::map<std::string, std::uint32_t> ids;
	const auto flood = [&](const std::string& a_name, auto a_act) {
		auto& bot = s.Player(a_name);
		REQUIRE(bot.Join());
		ids[a_name] = bot.Id();
		a_act(bot);
		s.Settle();
		bot.Leave();
		s.Settle();
	};
	flood("FloodKills", [](Client& a_bot) { for (std::uint32_t i = 0; i < 11; ++i) { a_bot.Kill(0x00A00000 + i); } });
	flood("FloodKills2", [](Client& a_bot) { for (std::uint32_t i = 0; i < 3; ++i) { a_bot.Kill(0x00B00000 + i); } });
	flood("FloodLoot", [](Client& a_bot) { for (int i = 0; i < 11; ++i) { a_bot.Loot(TRUNK, CAPS, -1); } });
	flood("FloodPickups", [](Client& a_bot) { for (std::uint32_t i = 0; i < 11; ++i) { a_bot.PickUp(0x00A10000 + i); } });
	flood("FloodClaims", [](Client& a_bot) { for (std::uint32_t i = 0; i < 50; ++i) { a_bot.Claim(0x00A20000 + i); } });
	s.SettleAfter(std::chrono::milliseconds(500));  // the looper's last hello is answered as its budget refills
	CHECK(looper.Welcomed());

	const auto log = s.LogLines();
	CHECK(CountMatches(log, std::format("server: player {} has added 8 dead NPC entries this run, their share", ids["FloodKills"])) == 1);
	CHECK(CountMatches(log, R"(dead NPC table is \d+% full \(\d+/10\); kill sharing stops at 100%)") == 1);
	CHECK(CountMatches(log, R"(dead NPC table is 100% full \(10/10\))") == 1);
	CHECK(CountMatches(log, std::format("server: player {} has made 8 container changes this session, their share", ids["FloodLoot"])) == 1);
	CHECK(CountMatches(log, std::format("server: player {} has added 8 picked-up item entries this run, their share", ids["FloodPickups"])) == 1);
	CHECK(CountMatches(log, "says hello too often") >= 1);
	CHECK(CountMatches(log, std::format("server: player {} .*(their share|too often|dropping some)", honest.Id())) == 0);
	CHECK(CountMatches(log, "Honest|Looper|Flood") == 0);
	CHECK(CountMatches(log, "too many events", true) == 0);

	// What the honest player did is in every game, and they agree on everything kept.
	// The honest take of 10 and the 8 of the loot flooder's that were kept, nothing past that share.
	CHECK(honest.Count(TRUNK, CAPS) == -18);
	CHECK(looper.Count(TRUNK, CAPS) == -18);
	CHECK_AGREE({ &host, &honest, &looper });
}
