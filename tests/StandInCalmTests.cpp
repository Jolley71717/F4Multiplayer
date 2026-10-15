#include "Test.h"

#include "game/SaveStrays.h"
#include "game/StandInCalm.h"

#include <cstdio>
#include <string>
#include <vector>

// A friend's stand-in never fights us or another stand-in: our punch turned one on us (red name and health bar)
// and, as it runs no AI, kept us in combat for good. Its anger is cleared, its fight with us is ended (and ended
// again if the engine keeps it up), and then the combat it left us in, unless something real fights us.
namespace
{
	using namespace std::chrono_literals;
	using StandInCalm::Clock;
	using StandInCalm::Foe;

	const auto t0 = Clock::time_point{} + 1h;
	constexpr auto FRAME = 16ms;

	// One game's frames: its stand-ins, and whether we are in combat and with whom.
	struct Game
	{
		StandInCalm::Ours               ours;
		std::vector<StandInCalm::Track> standIns;
		std::vector<StandInCalm::Facts> facts;  // what the engine says of each stand-in
		bool                            weInCombat = false;
		bool                            realFoe = false;
		std::uint32_t                   stops = 0;
		std::uint32_t                   forgiven = 0;
		std::uint32_t                   oursEnded = 0;

		explicit Game(std::size_t a_standIns) :
			standIns(a_standIns), facts(a_standIns) {}

		// The frame at a_now, as RemotePlayers::Update runs it: each stand-in (KeepCalm), then us (KeepUsCalm). An
		// ended fight is gone from the engine's facts; our combat lasts while a stand-in fights us or it is not ended.
		void Frame(Clock::time_point a_now)
		{
			for (std::size_t i = 0; i < standIns.size(); ++i) {
				const auto calm = StandInCalm::Frame(standIns[i], ours, facts[i], a_now);
				if (calm.forgive) {
					++forgiven;
					facts[i].angry = false;
				}
				if (calm.stop) {
					++stops;
					facts[i].inCombat = false;
					facts[i].target = Foe::kNobody;
				}
			}
			if (StandInCalm::OursDue(ours, weInCombat, weInCombat && realFoe, a_now)) {
				++oursEnded;
				weInCombat = false;
			}
		}

		void Run(Clock::time_point a_from, Clock::time_point a_to)
		{
			for (auto t = a_from; t < a_to; t += FRAME) {
				Frame(t);
			}
		}
	};

	// Our punch, as the engine took it before the fix: the stand-in angry with us and fighting us, and us in combat.
	void Punched(Game& a_game, std::size_t a_standIn)
	{
		a_game.facts[a_standIn] = { .inCombat = true, .target = Foe::kUs, .angry = true };
		a_game.weInCombat = true;
	}
}

TEST("stand-in calm: a stand-in our punch turned on us is forgiven and its fight ended in the same frame")
{
	Game game(1);
	Punched(game, 0);
	game.Frame(t0);
	CHECK(game.forgiven == 1);
	CHECK(game.stops == 1);
	CHECK(!game.facts[0].inCombat);
	CHECK(!game.facts[0].angry);
}

TEST("stand-in calm: the combat it left us in is ended a moment later, once, when nobody else fights us")
{
	Game game(1);
	Punched(game, 0);
	game.Frame(t0);
	game.Run(t0 + FRAME, t0 + StandInCalm::OURS_SETTLE - FRAME);
	CHECK(game.weInCombat);  // the game is given a moment to end it by itself
	game.Run(t0 + StandInCalm::OURS_SETTLE, t0 + StandInCalm::OURS_SETTLE + 1s);
	CHECK(!game.weInCombat);
	CHECK(game.oursEnded == 1);
	CHECK(game.ours.clears == 1);
	// Back in combat later (a raider), nothing of ours is ended: no stand-in's fight with us was ended lately.
	game.weInCombat = true;
	game.Run(t0 + 2s, t0 + 30s);
	CHECK(game.weInCombat);
	CHECK(game.oursEnded == 1);
}

TEST("stand-in calm: our combat is left alone while something real fights us, and after the window too")
{
	Game game(1);
	Punched(game, 0);
	game.realFoe = true;
	game.Run(t0, t0 + StandInCalm::OURS_WINDOW + 1s);
	CHECK(game.stops == 1);
	CHECK(game.weInCombat);
	CHECK(game.oursEnded == 0);
	// The raider dies after the window: our combat is the game's to end now.
	game.realFoe = false;
	game.Run(t0 + StandInCalm::OURS_WINDOW + 1s, t0 + StandInCalm::OURS_WINDOW + 5s);
	CHECK(game.weInCombat);
	CHECK(game.oursEnded == 0);
}

TEST("stand-in calm: a combat the game ended by itself is not ended again")
{
	Game game(1);
	Punched(game, 0);
	game.Frame(t0);
	game.weInCombat = false;
	game.Run(t0 + FRAME, t0 + StandInCalm::OURS_WINDOW + 1s);
	CHECK(game.oursEnded == 0);
	CHECK(!game.ours.stoppedAt);
}

TEST("stand-in calm: a fight the engine keeps starting is ended at most once a STOP_GAP")
{
	Game game(1);
	std::uint32_t frames = 0;
	for (auto t = t0; t < t0 + 5s; t += FRAME) {
		Punched(game, 0);  // every frame, a hit lands
		game.Frame(t);
		++frames;
	}
	CHECK(game.forgiven == frames);  // the anger is cleared every time
	const auto most = static_cast<std::uint32_t>(5s / StandInCalm::STOP_GAP) + 1;
	CHECK(game.stops >= most - 1);
	CHECK(game.stops <= most);
	CHECK(game.standIns[0].stops == game.stops);
}

TEST("stand-in calm: two stand-ins fighting each other are parted, and our combat is not touched")
{
	Game game(2);
	game.facts[0] = { .inCombat = true, .target = Foe::kStandIn };
	game.facts[1] = { .inCombat = true, .target = Foe::kStandIn };
	game.weInCombat = true;  // our own fight, with someone else
	game.Run(t0, t0 + 3s);
	CHECK(game.stops == 2);
	CHECK(!game.ours.stoppedAt);
	CHECK(game.weInCombat);
	CHECK(game.oursEnded == 0);
}

TEST("stand-in calm: a stand-in an enemy fights, or one in combat with no target, is left alone")
{
	Game game(2);
	game.facts[0] = { .inCombat = true, .target = Foe::kOther };
	game.facts[1] = { .inCombat = true, .target = Foe::kNobody };
	game.Run(t0, t0 + 3s);
	CHECK(game.stops == 0);
	CHECK(game.facts[0].inCombat);
	CHECK(!StandInCalm::Hostile(game.facts[0]));
	CHECK(!StandInCalm::Hostile(game.facts[1]));
}

TEST("stand-in calm: anger with no fight is only forgiven")
{
	Game game(1);
	game.facts[0].angry = true;
	game.Frame(t0);
	CHECK(game.forgiven == 1);
	CHECK(game.stops == 0);
	CHECK(!game.facts[0].angry);
}

TEST("stand-in calm: of two stand-ins, the one fighting us is ended and the one an enemy fights is not")
{
	Game game(2);
	Punched(game, 0);
	game.facts[1] = { .inCombat = true, .target = Foe::kOther };
	game.Run(t0, t0 + 2s);
	CHECK(game.standIns[0].stops == 1);
	CHECK(game.standIns[1].stops == 0);
	CHECK(game.facts[1].inCombat);
	CHECK(game.oursEnded == 1);
}

TEST("stand-in calm: a stand-in is placed in the faction SaveGuard knows it by, and ignores friends' hits")
{
	const auto commands = StandInCalm::SetupCommands(0xFF002482);
	REQUIRE(commands.size() == 2);
	unsigned ref = 0, faction = 0;
	int      rank = -1;
	REQUIRE(std::sscanf(commands[0].c_str(), "%08X.addtofaction %08X %d", &ref, &faction, &rank) == 3);
	CHECK(ref == 0xFF002482);
	CHECK(SaveStrays::StandInFaction(faction, rank));
	CHECK(commands[1] == "FF002482.setignorefriendlyhits 1");
	// It starts no fight and joins none.
	CHECK(StandInCalm::AGGRESSION == 0.0f);
	CHECK(StandInCalm::ASSISTANCE == 0.0f);
}
