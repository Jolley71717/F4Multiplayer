// Which of our game's runtime NPCs our player placed with the console (ConsoleSpawns), so they are shared with friends
// calm too. Each test names what the players would see go wrong if it failed: an NPC one player spawned with
// `player.placeatme` stood in that player's game only, or every calm NPC a script made in one game was doubled in the
// other.

#include "Test.h"

#include "game/ConsoleSpawns.h"

#include <chrono>
#include <cstdint>

using namespace std::chrono_literals;
using ConsoleSpawns::Book;

namespace
{
	using Clock = ConsoleSpawns::Clock;

	constexpr std::uint32_t SETTLER = 0x00020593;
	constexpr std::uint32_t BRAHMIN = 0x000A1CEA;
	constexpr std::uint32_t CELL = 0x0000DA26;
	constexpr std::uint32_t WORLD = 0x0000003C;

	ConsoleSpawns::Spot At(float a_x, float a_y, std::uint32_t a_cell = 0, std::uint32_t a_world = WORLD)
	{
		return { .cell = a_cell, .worldspace = a_world, .x = a_x, .y = a_y, .z = 0.0f };
	}

	const Clock::time_point T0 = Clock::time_point{} + 1h;
}

TEST("console spawns: the runtime NPC that first stands by the command's reference soon after is the console's, and stays so")
{
	Book book;
	book.Ran(At(1000.0f, 1000.0f), T0);
	// The NPC's model loads in the background: the scan sees it standing a second later, a step off the player.
	CHECK(book.Seen(0xFF001234, SETTLER, At(1060.0f, 990.0f), T0 + 1s));
	CHECK(book.Made(0xFF001234));
	CHECK(book.Found() == 1);
	// ...and long after the command, wherever it has walked: it was shared and stays shared.
	CHECK(book.Seen(0xFF001234, SETTLER, At(9000.0f, -4000.0f), T0 + 20min));
	CHECK(book.Found() == 1);
}

TEST("console spawns: `placeatme <base> 5` gives five, each the console's")
{
	Book book;
	book.Ran(At(0.0f, 0.0f), T0);
	for (std::uint32_t i = 0; i < 5; ++i) {
		CHECK(book.Seen(0xFF002000 + i, SETTLER, At(30.0f * static_cast<float>(i), 10.0f), T0 + 2s));
	}
	CHECK(book.Size() == 5);
}

TEST("console spawns: an NPC that stood there before the command ran is not the console's")
{
	Book book;
	// A script's calm NPC beside the player (each game's scripts make their own: sharing it would double it).
	CHECK(!book.Seen(0xFF000AAA, BRAHMIN, At(50.0f, 50.0f), T0));
	book.Ran(At(0.0f, 0.0f), T0 + 1min);
	CHECK(!book.Seen(0xFF000AAA, BRAHMIN, At(50.0f, 50.0f), T0 + 1min + 1s));
	CHECK(!book.Made(0xFF000AAA));
	// The console's own, placed by that command, still is.
	CHECK(book.Seen(0xFF000AAB, SETTLER, At(20.0f, 0.0f), T0 + 1min + 1s));
}

TEST("console spawns: one that appears too late, too far, or in another cell is not the console's")
{
	Book book;
	book.Ran(At(0.0f, 0.0f), T0);
	// Past the reach of a place command (its distance argument, a few steps' walk).
	CHECK(!book.Seen(0xFF003001, SETTLER, At(ConsoleSpawns::PLACED_WITHIN + 200.0f, 0.0f), T0 + 1s));
	// Indoors while the command ran outdoors: another place altogether.
	CHECK(!book.Seen(0xFF003002, SETTLER, At(0.0f, 0.0f, CELL, 0), T0 + 1s));
	// Within reach, but the command is long done: an encounter's or a script's that wandered up.
	CHECK(!book.Seen(0xFF003003, SETTLER, At(10.0f, 0.0f), T0 + ConsoleSpawns::APPEARS_WITHIN));
	CHECK(book.Size() == 0);
	// Once seen as not the console's, a later command beside it does not make it so.
	book.Ran(At(10.0f, 0.0f), T0 + 30s);
	CHECK(!book.Seen(0xFF003003, SETTLER, At(10.0f, 0.0f), T0 + 31s));
}

TEST("console spawns: with no command there is nothing of the console's")
{
	Book book;
	CHECK(!book.Seen(0xFF004001, SETTLER, At(0.0f, 0.0f), T0));
	CHECK(!book.Made(0xFF004001));
	CHECK(book.Ran() == 0);
	CHECK(book.Found() == 0);
}

TEST("console spawns: an id the engine gives again names another NPC")
{
	Book book;
	book.Ran(At(0.0f, 0.0f), T0);
	CHECK(book.Seen(0xFF005001, SETTLER, At(0.0f, 0.0f), T0 + 1s));
	// The settler is gone (disabled and deleted with the console) and its id goes to a script's brahmin later: not the
	// console's, though the id was.
	CHECK(!book.Seen(0xFF005001, BRAHMIN, At(500.0f, 0.0f), T0 + 5min));
	CHECK(!book.Made(0xFF005001));

	// The other way: an id a script's NPC held, seen by the scan, then given to the console's NPC of another base.
	CHECK(!book.Seen(0xFF005002, BRAHMIN, At(0.0f, 0.0f), T0 + 5min));
	book.Ran(At(0.0f, 0.0f), T0 + 6min);
	CHECK(book.Seen(0xFF005002, SETTLER, At(0.0f, 0.0f), T0 + 6min + 1s));

	// ...and of the same base, when the scan has not seen the old one for a long while.
	CHECK(!book.Seen(0xFF005003, SETTLER, At(0.0f, 0.0f), T0 + 7min));
	book.Ran(At(0.0f, 0.0f), T0 + 7min + ConsoleSpawns::SEEN_KEPT);
	CHECK(book.Seen(0xFF005003, SETTLER, At(0.0f, 0.0f), T0 + 7min + ConsoleSpawns::SEEN_KEPT + 1s));
}

TEST("console spawns: the commands and the NPCs remembered are capped")
{
	Book book;
	for (std::size_t i = 0; i < ConsoleSpawns::MAX_COMMANDS + 8; ++i) {
		book.Ran(At(100000.0f * static_cast<float>(i), 0.0f), T0);
	}
	CHECK(book.Waiting() == ConsoleSpawns::MAX_COMMANDS);
	// The oldest commands went: nothing is placed by the first any more, the last still places.
	CHECK(!book.Seen(0xFF006001, SETTLER, At(0.0f, 0.0f), T0 + 1s));
	CHECK(book.Seen(0xFF006002, SETTLER, At(100000.0f * static_cast<float>(ConsoleSpawns::MAX_COMMANDS + 7), 0.0f), T0 + 1s));

	// Old commands go with time.
	book.Ran(At(0.0f, 0.0f), T0 + 1min);
	CHECK(book.Waiting() == 1);

	// The console's NPCs remembered: past MAX_MADE the one seen longest ago goes.
	Book many;
	many.Ran(At(0.0f, 0.0f), T0);
	for (std::uint32_t i = 0; i < ConsoleSpawns::MAX_MADE + 4; ++i) {
		CHECK(many.Seen(0xFF010000 + i, SETTLER, At(0.0f, 0.0f), T0 + std::chrono::milliseconds(i)));
	}
	CHECK(many.Size() == ConsoleSpawns::MAX_MADE);
	CHECK(!many.Made(0xFF010000));
	CHECK(many.Made(0xFF010000 + static_cast<std::uint32_t>(ConsoleSpawns::MAX_MADE) + 3));

	// The others seen: never more than MAX_SEEN.
	Book crowd;
	for (std::uint32_t i = 0; i < ConsoleSpawns::MAX_SEEN + 100; ++i) {
		CHECK(!crowd.Seen(0xFF100000 + i, BRAHMIN, At(0.0f, 0.0f), T0 + std::chrono::milliseconds(i)));
	}
	crowd.Ran(At(0.0f, 0.0f), T0 + 1min);
	// One of the latest is still known as seen before the command; nothing was taken for the console's.
	CHECK(!crowd.Seen(0xFF100000 + static_cast<std::uint32_t>(ConsoleSpawns::MAX_SEEN) + 99, BRAHMIN, At(0.0f, 0.0f), T0 + 1min + 1s));
	CHECK(crowd.Size() == 0);
}

TEST("console spawns: a load or the session's end forgets them all")
{
	Book book;
	book.Ran(At(0.0f, 0.0f), T0);
	CHECK(book.Seen(0xFF007001, SETTLER, At(0.0f, 0.0f), T0 + 1s));
	book.Ran(At(0.0f, 0.0f), T0 + 2s);
	book.Clear();
	CHECK(book.Size() == 0);
	CHECK(book.Waiting() == 0);
	CHECK(!book.Made(0xFF007001));
	// The id names whatever the loaded save holds there: not the console's, and the command before the load places nothing.
	CHECK(!book.Seen(0xFF007001, SETTLER, At(0.0f, 0.0f), T0 + 3s));
}
