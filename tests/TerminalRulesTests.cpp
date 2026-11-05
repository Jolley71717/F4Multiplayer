// Choices made at a quest's terminals (src/game/TerminalRules.h: Easy City Downs' race): which readings are sent, when a
// friend's is made here (the race on here, near its maker), the book across a load, the wire, and the server's relay and
// catch-up.

#include "ServerFixture.h"

#include "game/TerminalRules.h"

using namespace TestServer;

namespace
{
	using namespace TerminalRules;
	using Protocol::TerminalChoice;

	constexpr std::uint8_t STATE = 0;      // RaceState
	constexpr std::uint8_t TARGETING = 1;  // RaceTargeting
	constexpr std::uint8_t RESERVES = 3;   // RaceReserves (once)
	constexpr std::uint8_t DESTRUCT = 4;   // RaceSelfDestruct (once)

	const Clock::time_point T0 = Clock::time_point{} + std::chrono::hours(1);

	// The race on here, near the friend who made the choice, our player at no terminal.
	constexpr RaceHere On(std::optional<std::int32_t> a_ours = 0)
	{
		return { .ours = a_ours, .settled = true, .setUp = true, .over = false, .nearMaker = true, .raceRunning = true, .atTerminal = false };
	}

	std::vector<TerminalChoice> Choices(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		return Decoded(a_packets, MT::kTerminalChoice, [](auto a_data) { return Protocol::DecodeTerminalChoice(a_data); });
	}
}

TEST("terminal rules: the rows are DN035QuestScript's, each with the values its terminals set")
{
	static_assert(ROWS.size() == Protocol::TERMINAL_ROWS);
	for (const auto& row : ROWS) {
		CHECK(row.plugin == "Fallout4.esm" && row.quest == 0x0EF40C && row.script == "DN035QuestScript");
	}
	for (const auto& values : Protocol::TERMINAL_VALUES) {
		CHECK(values.low <= values.high);
	}
	CHECK(ROWS[TARGETING].property == "RaceTargeting" && ROWS[TARGETING].setter == "SetRaceTargeting");
	CHECK(ROWS[DESTRUCT].setter == "RaceSelfDestruct" && Once(DESTRUCT) && Once(RESERVES) && !Once(STATE) && !Once(TARGETING));
	CHECK(Valid(TARGETING, 0) && Valid(TARGETING, 2) && !Valid(TARGETING, 3));
	CHECK(Valid(DESTRUCT, 1) && !Valid(DESTRUCT, 0) && !Valid(DESTRUCT, 2));
	CHECK(!Valid(Protocol::TERMINAL_ROWS, 0));
}

TEST("terminal rules: a change our player made at a terminal is sent; a first reading, a stage's or a friend's is not")
{
	CHECK(!ToSend(TARGETING, std::nullopt, 2, true, std::nullopt));  // the first reading: a baseline
	CHECK(!ToSend(TARGETING, 0, 0, true, std::nullopt));             // unchanged
	CHECK(ToSend(TARGETING, 0, 2, true, std::nullopt) == std::optional<std::uint8_t>{ 2 });
	CHECK(!ToSend(TARGETING, 0, 2, false, std::nullopt));  // no terminal open: the quest's own stage or timer
	CHECK(!ToSend(TARGETING, 0, 2, true, 2));              // what a friend's choice made it here
	CHECK(ToSend(TARGETING, 0, 1, true, 2) == std::optional<std::uint8_t>{ 1 });  // ours after theirs
	CHECK(!ToSend(TARGETING, 0, 7, true, std::nullopt));   // no value of its terminals
	CHECK(!ToSend(TARGETING, 0, -1, true, std::nullopt));
	// The self-destruct: sent as "it happened" (1), once; its countdown (2 to 10) is never sent.
	CHECK(ToSend(DESTRUCT, 0, 1, true, std::nullopt) == std::optional<std::uint8_t>{ 1 });
	CHECK(ToSend(DESTRUCT, 0, 3, true, std::nullopt) == std::optional<std::uint8_t>{ 1 });  // read once the countdown ran on
	CHECK(!ToSend(DESTRUCT, 2, 3, true, std::nullopt));
	CHECK(!ToSend(DESTRUCT, 10, 0, true, std::nullopt));
}

TEST("terminal rules: a friend's choice is made only while the race is on here, near its maker, with our player at no terminal")
{
	CHECK(ToApply(TARGETING, 2, On(0)) == Step::kCall);
	CHECK(ToApply(TARGETING, 2, On(2)) == Step::kDone);
	CHECK(ToApply(TARGETING, 9, On(0)) == Step::kDone);  // no value of its terminals: never
	auto here = On(0);
	here.ours.reset();  // the quest isn't running here
	CHECK(ToApply(TARGETING, 2, here) == Step::kWait);
	here = On(0);
	here.settled = false;  // it only just started
	CHECK(ToApply(TARGETING, 2, here) == Step::kWait);
	here = On(0);
	here.setUp = false;  // its first stage hasn't run
	CHECK(ToApply(TARGETING, 2, here) == Step::kWait);
	here = On(0);
	here.nearMaker = false;  // our player is not at the Downs with the friend who made it
	CHECK(ToApply(TARGETING, 2, here) == Step::kWait);
	CHECK(ToApply(DESTRUCT, 1, here) == Step::kWait);
	here = On(0);
	here.atTerminal = true;
	CHECK(ToApply(TARGETING, 2, here) == Step::kWait);
	here = On(0);
	here.over = true;  // the robots blew up here, or the quest ended: dropped
	CHECK(ToApply(TARGETING, 2, here) == Step::kDone);
	CHECK(ToApply(DESTRUCT, 1, here) == Step::kDone);
	CHECK(ToApply(DESTRUCT, 1, On(0)) == Step::kCall);
	CHECK(ToApply(DESTRUCT, 1, On(4)) == Step::kDone);  // it happened here already
	CHECK(ToApply(RESERVES, 1, On(1)) == Step::kDone);
	// Stage 10 runs as the quest starts, race or none: the once rows wait for a race running here (RaceState 0).
	here = On(0);
	here.raceRunning = false;
	CHECK(ToApply(DESTRUCT, 1, here) == Step::kWait);
	CHECK(ToApply(RESERVES, 1, here) == Step::kWait);
	CHECK(ToApply(DESTRUCT, 1, On(3)) == Step::kDone);
	here.ours = 3;
	CHECK(ToApply(DESTRUCT, 1, here) == Step::kDone);  // it happened here already, race or none
	CHECK(ToApply(TARGETING, 2, here) == Step::kCall);  // the other rows don't wait for it
	CHECK(ToApply(STATE, 1, here) == Step::kCall);
}

TEST("terminal rules: the book makes a friend's choice without sending it back, and drops one gone stale")
{
	Book book;
	CHECK(!book.Read(TARGETING, 0, false, T0));  // baseline
	CHECK(book.Read(TARGETING, 2, true, T0) == std::optional<std::uint8_t>{ 2 });
	CHECK(!book.Read(TARGETING, 2, true, T0));

	// A friend's: waits while the race isn't on here, then made; our reading of it is not sent back, even at a terminal.
	book.Heard(STATE, 1, 7, T0);
	CHECK(book.Due(STATE, On(), T0) == Step::kWait);  // no reading yet
	CHECK(!book.Read(STATE, 0, false, T0));
	auto away = On();
	away.nearMaker = false;
	CHECK(book.Due(STATE, away, T0) == Step::kWait);
	CHECK(book.Due(STATE, On(), T0) == Step::kCall);
	REQUIRE(book.Pending(STATE));
	CHECK(book.Pending(STATE)->value == 1 && book.Pending(STATE)->playerId == 7);
	book.Made(STATE, true, T0);
	CHECK(book.Due(STATE, On(), T0) == Step::kDone);
	CHECK(!book.Read(STATE, 1, true, T0));
	CHECK(book.Read(STATE, 0, true, T0) == std::optional<std::uint8_t>{ 0 });  // our player's own choice afterwards

	// A joiner's catch-up is as old as the server says: it goes stale CHOICE_LIFE after it was made, not after it came.
	Book joined;
	CHECK(!joined.Read(STATE, 0, false, T0));
	joined.Heard(STATE, 1, 7, T0 - std::chrono::minutes(8));
	CHECK(joined.Due(STATE, On(), T0 + std::chrono::minutes(1)) == Step::kCall);
	CHECK(joined.Due(STATE, On(), T0 + std::chrono::minutes(3)) == Step::kDone);

	// A friend's choice never made (our player never near them) goes stale.
	book.Heard(TARGETING, 1, 7, T0);
	CHECK(book.Due(TARGETING, away, T0 + CHOICE_LIFE) == Step::kWait);
	CHECK(book.Due(TARGETING, On(), T0 + CHOICE_LIFE + std::chrono::seconds(1)) == Step::kDone);

	// One that happens once, made here: its countdown is not ours to send.
	book.Heard(DESTRUCT, 1, 7, T0);
	CHECK(!book.Read(DESTRUCT, 0, false, T0));
	CHECK(book.Due(DESTRUCT, On(), T0) == Step::kCall);
	book.Made(DESTRUCT, true, T0);
	CHECK(!book.Read(DESTRUCT, 1, true, T0));
	CHECK(!book.Read(DESTRUCT, 2, true, T0));

	// A heard choice past its row's values is never pending.
	book.Clear();
	book.Heard(TARGETING, 7, 7, T0);
	CHECK(!book.Pending(TARGETING));
}

TEST("terminal rules: a friend's choice that never took here does not keep our own of that value from being sent")
{
	Book book;
	CHECK(!book.Read(TARGETING, 0, false, T0));
	book.Heard(TARGETING, 2, 7, T0);
	CHECK(book.Due(TARGETING, On(), T0) == Step::kCall);
	book.Made(TARGETING, true, T0);  // called, but the script refused it: the property stays 0
	CHECK(!book.Read(TARGETING, 0, false, T0 + std::chrono::seconds(1)));
	// Once EXPECT_LIFE is over, our player's own 2 at the terminal is ours, and sent.
	CHECK(book.Read(TARGETING, 2, true, T0 + EXPECT_LIFE + std::chrono::seconds(1)) == std::optional<std::uint8_t>{ 2 });

	// Within EXPECT_LIFE a reading of that value is the friend's call landing: not sent back.
	Book late;
	CHECK(!late.Read(TARGETING, 0, false, T0));
	late.Heard(TARGETING, 2, 7, T0);
	CHECK(late.Due(TARGETING, On(), T0) == Step::kCall);
	late.Made(TARGETING, true, T0);
	CHECK(!late.Read(TARGETING, 2, true, T0 + std::chrono::seconds(1)));
}

TEST("terminal rules: a load makes a friend's latest again under the same rules, never our own or a once row")
{
	Book book;
	CHECK(!book.Read(STATE, 0, false, T0));
	CHECK(!book.Read(TARGETING, 0, false, T0));
	CHECK(!book.Read(DESTRUCT, 0, false, T0));
	book.Heard(STATE, 1, 7, T0);  // a friend's
	CHECK(book.Due(STATE, On(), T0) == Step::kCall);
	book.Made(STATE, true, T0);
	CHECK(!book.Read(STATE, 1, false, T0));
	CHECK(book.Read(TARGETING, 2, true, T0) == std::optional<std::uint8_t>{ 2 });  // ours
	book.Heard(DESTRUCT, 1, 7, T0);
	CHECK(book.Due(DESTRUCT, On(), T0) == Step::kCall);
	book.Made(DESTRUCT, true, T0);

	book.OnGameLoaded(T0 + std::chrono::minutes(1));
	CHECK(!book.Read(STATE, 0, false, T0 + std::chrono::minutes(1)));
	CHECK(!book.Read(TARGETING, 0, true, T0 + std::chrono::minutes(1)));  // baseline again, though at a terminal
	CHECK(!book.Read(DESTRUCT, 0, false, T0 + std::chrono::minutes(1)));
	CHECK(book.Due(STATE, On(), T0 + std::chrono::minutes(1)) == Step::kCall);  // the friend's, while the race is on here
	auto away = On();
	away.nearMaker = false;
	CHECK(book.Due(STATE, away, T0 + std::chrono::minutes(1)) == Step::kWait);  // not into a save far from them
	CHECK(book.Due(TARGETING, On(), T0 + std::chrono::minutes(1)) == Step::kDone);  // ours is not made again: the save's own stands
	CHECK(!book.Pending(TARGETING));
	CHECK(!book.Pending(DESTRUCT));  // a once row: live only

	// A load past CHOICE_LIFE makes nothing again.
	book.OnGameLoaded(T0 + CHOICE_LIFE + std::chrono::minutes(1));
	CHECK(!book.Pending(STATE));
}

TEST("terminal rules: a choice reads back, and one past the rows or its row's values does not")
{
	const TerminalChoice choice{ 3, TARGETING, 2 };
	CHECK(Protocol::DecodeTerminalChoice(Protocol::Encode(choice, MT::kReportTerminalChoice)) == choice);
	CHECK(!Protocol::DecodeTerminalChoice(Protocol::Encode(TerminalChoice{ 3, Protocol::TERMINAL_ROWS, 0 }, MT::kTerminalChoice)));
	CHECK(!Protocol::DecodeTerminalChoice(Protocol::Encode(TerminalChoice{ 3, TARGETING, 3 }, MT::kTerminalChoice)));
	CHECK(!Protocol::DecodeTerminalChoice(Protocol::Encode(TerminalChoice{ 3, DESTRUCT, 0 }, MT::kTerminalChoice)));
	auto bytes = Protocol::Encode(choice, MT::kTerminalChoice);
	bytes.pop_back();
	CHECK(!Protocol::DecodeTerminalChoice(bytes));
}

TEST("terminal rules: the server passes a choice on as its player's, and tells a joiner each row's fresh latest but no once row")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Protocol::Encode(TerminalChoice{ 99, TARGETING, 0, 500 }, MT::kReportTerminalChoice));  // claims to be someone else's, and old
	f.fake->Deliver(2, Protocol::Encode(TerminalChoice{ 0, TARGETING, 2 }, MT::kReportTerminalChoice));
	f.fake->Deliver(1, Protocol::Encode(TerminalChoice{ 0, DESTRUCT, 1 }, MT::kReportTerminalChoice));
	f.fake->Deliver(1, Protocol::Encode(TerminalChoice{ 0, STATE, 1 }, MT::kReportTerminalChoice));
	f.fake->Deliver(2, Protocol::Encode(TerminalChoice{ 0, TARGETING, 9 }, MT::kReportTerminalChoice));  // no value of its: refused
	const auto toBob = Choices(Received(f, 2));
	REQUIRE(toBob.size() == 3);
	CHECK(toBob[0] == (TerminalChoice{ alice, TARGETING, 0 }));
	CHECK(toBob[1] == (TerminalChoice{ alice, DESTRUCT, 1 }));
	CHECK(toBob[2] == (TerminalChoice{ alice, STATE, 1 }));
	const auto toAlice = Choices(Received(f, 1));
	REQUIRE(toAlice.size() == 1);
	CHECK(toAlice[0] == (TerminalChoice{ bob, TARGETING, 2 }));

	JoinAs(f, 3, "Carol");
	auto toCarol = Choices(Received(f, 3));
	std::ranges::sort(toCarol, {}, &TerminalChoice::row);
	REQUIRE(toCarol.size() == 2);
	CHECK(toCarol[0] == (TerminalChoice{ alice, STATE, 1 }));
	CHECK(toCarol[1] == (TerminalChoice{ bob, TARGETING, 2 }));  // Bob's, the latest (the bad one didn't replace it); no self-destruct
}

TEST("terminal rules: a joiner is not told a stale choice, nor one whose maker left")
{
	Fixture f;
	f.HoldClock();
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	f.fake->Deliver(1, Protocol::Encode(TerminalChoice{ 0, STATE, 1 }, MT::kReportTerminalChoice));
	f.fake->Deliver(2, Protocol::Encode(TerminalChoice{ 0, TARGETING, 2 }, MT::kReportTerminalChoice));
	REQUIRE(Choices(Received(f, 1)).size() == 1);
	f.fake->Drop(2);

	JoinAs(f, 3, "Carol");
	auto toCarol = Choices(Received(f, 3));
	REQUIRE(toCarol.size() == 1);  // Bob left: his targeting is not told
	CHECK(toCarol[0].row == STATE && toCarol[0].ageSeconds == 0);

	f.Advance(std::chrono::minutes(5));
	JoinAs(f, 5, "Eve");
	const auto toEve = Choices(Received(f, 5));
	REQUIRE(toEve.size() == 1);
	CHECK(toEve[0].row == STATE && toEve[0].ageSeconds >= 300 && toEve[0].ageSeconds <= 302);  // its age: Eve's wait ends with the server's

	f.Advance(std::chrono::minutes(6));
	JoinAs(f, 4, "Dave");
	CHECK(Choices(Received(f, 4)).empty());  // past its life
}

TEST("terminal rules: RaceState 0 is a running race, 1 a stopped one, and no reading none")
{
	using TerminalRules::RaceRunning;
	CHECK(RaceRunning(0));
	CHECK(!RaceRunning(1));  // Stop Race sets it to 1, and the terminal then offers Start Race
	CHECK(!RaceRunning(std::nullopt));
}
