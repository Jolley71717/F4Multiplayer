// Choices made at a quest's terminals (src/game/TerminalRules.h: Easy City Downs' race): which readings are sent, when a
// friend's is made here, the book across a load, the wire, and the server's relay and catch-up.

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
		CHECK(row.low <= row.high);
	}
	CHECK(ROWS[TARGETING].property == "RaceTargeting" && ROWS[TARGETING].setter == "SetRaceTargeting");
	CHECK(ROWS[DESTRUCT].setter == "RaceSelfDestruct" && ROWS[DESTRUCT].once);
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
	// The self-destruct: sent as "it happened" (1), once; its countdown (2 to 10) is never sent.
	CHECK(ToSend(DESTRUCT, 0, 1, true, std::nullopt) == std::optional<std::uint8_t>{ 1 });
	CHECK(ToSend(DESTRUCT, 0, 3, true, std::nullopt) == std::optional<std::uint8_t>{ 1 });  // read once the countdown ran on
	CHECK(!ToSend(DESTRUCT, 2, 3, true, std::nullopt));
	CHECK(!ToSend(DESTRUCT, 10, 0, true, std::nullopt));
}

TEST("terminal rules: a friend's choice is made once the quest has run here a while, with our player at no terminal, if ours differs")
{
	CHECK(ToApply(TARGETING, 2, 0, true, false) == Step::kCall);
	CHECK(ToApply(TARGETING, 2, 2, true, false) == Step::kDone);
	CHECK(ToApply(TARGETING, 2, std::nullopt, true, false) == Step::kWait);  // the quest isn't running here
	CHECK(ToApply(TARGETING, 2, 0, false, false) == Step::kWait);           // it only just started
	CHECK(ToApply(TARGETING, 2, 0, true, true) == Step::kWait);             // our player is at a terminal
	CHECK(ToApply(TARGETING, 9, 0, true, false) == Step::kDone);            // no value of its terminals: never
	CHECK(ToApply(DESTRUCT, 1, 0, true, false) == Step::kCall);
	CHECK(ToApply(DESTRUCT, 1, 4, true, false) == Step::kDone);  // it happened here already
	CHECK(ToApply(RESERVES, 1, 1, true, false) == Step::kDone);
}

TEST("terminal rules: the book sends ours, makes theirs without sending it back, and makes the session's again after a load")
{
	Book book;
	CHECK(!book.Read(TARGETING, 0, false));  // baseline
	CHECK(book.Read(TARGETING, 2, true) == std::optional<std::uint8_t>{ 2 });
	CHECK(!book.Read(TARGETING, 2, true));

	// A friend's: due once the quest has settled here, then made; our reading of it is not sent back, even at a terminal.
	book.Heard(STATE, 1);
	CHECK(book.Due(STATE, true, false) == Step::kWait);  // no reading yet
	CHECK(!book.Read(STATE, 0, false));
	CHECK(book.Due(STATE, false, false) == Step::kWait);
	CHECK(book.Due(STATE, true, false) == Step::kCall);
	CHECK(book.Pending(STATE) == std::optional<std::uint8_t>{ 1 });
	book.Made(STATE, true);
	CHECK(book.Due(STATE, true, false) == Step::kDone);
	CHECK(!book.Read(STATE, 1, true));
	CHECK(book.Read(STATE, 0, true) == std::optional<std::uint8_t>{ 0 });  // our player's own choice afterwards

	// One that happens once, made here: its countdown is not ours to send.
	book.Heard(DESTRUCT, 1);
	CHECK(!book.Read(DESTRUCT, 0, false));
	CHECK(book.Due(DESTRUCT, true, false) == Step::kCall);
	book.Made(DESTRUCT, true);
	CHECK(!book.Read(DESTRUCT, 1, true));
	CHECK(!book.Read(DESTRUCT, 2, true));

	// A load: readings start afresh, and the session's latest of each row is made again where the loaded game differs.
	book.OnGameLoaded();
	CHECK(!book.Read(TARGETING, 0, true));  // baseline again, though at a terminal
	CHECK(book.Due(TARGETING, true, false) == Step::kCall);  // the session's (ours, sent before) is 2
	CHECK(book.Pending(TARGETING) == std::optional<std::uint8_t>{ 2 });
	CHECK(!book.Read(STATE, 0, false));
	CHECK(book.Due(STATE, true, false) == Step::kDone);  // the session's latest is ours: 0
	book.Made(STATE, false);
	CHECK(!book.Read(DESTRUCT, 5, false));
	CHECK(book.Due(DESTRUCT, true, false) == Step::kDone);  // it happened in the loaded game too

	// A friend's choice heard while ours waits: theirs is the latest.
	book.Heard(TARGETING, 1);
	CHECK(book.Pending(TARGETING) == std::optional<std::uint8_t>{ 1 });
	book.Clear();
	CHECK(book.Due(TARGETING, true, false) == Step::kDone);
	book.Heard(TARGETING, 7);  // no value of its terminals
	CHECK(!book.Pending(TARGETING));
}

TEST("terminal rules: a choice reads back, and one past the rows does not")
{
	const TerminalChoice choice{ 3, TARGETING, 2 };
	CHECK(Protocol::DecodeTerminalChoice(Protocol::Encode(choice, MT::kReportTerminalChoice)) == choice);
	auto bytes = Protocol::Encode(TerminalChoice{ 3, Protocol::TERMINAL_ROWS, 0 }, MT::kTerminalChoice);
	CHECK(!Protocol::DecodeTerminalChoice(bytes));
	bytes = Protocol::Encode(choice, MT::kTerminalChoice);
	bytes.pop_back();
	CHECK(!Protocol::DecodeTerminalChoice(bytes));
}

TEST("terminal rules: the server passes a choice on as its player's, not back, and tells a joiner each row's latest")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Protocol::Encode(TerminalChoice{ 99, TARGETING, 0 }, MT::kReportTerminalChoice));  // claims to be someone else's
	f.fake->Deliver(2, Protocol::Encode(TerminalChoice{ 0, TARGETING, 2 }, MT::kReportTerminalChoice));
	f.fake->Deliver(1, Protocol::Encode(TerminalChoice{ 0, DESTRUCT, 1 }, MT::kReportTerminalChoice));
	const auto toBob = Choices(Received(f, 2));
	REQUIRE(toBob.size() == 2);
	CHECK(toBob[0] == (TerminalChoice{ alice, TARGETING, 0 }));
	CHECK(toBob[1] == (TerminalChoice{ alice, DESTRUCT, 1 }));
	const auto toAlice = Choices(Received(f, 1));
	REQUIRE(toAlice.size() == 1);
	CHECK(toAlice[0].row == TARGETING && toAlice[0].value == 2);

	JoinAs(f, 3, "Carol");
	auto toCarol = Choices(Received(f, 3));
	std::ranges::sort(toCarol, {}, &TerminalChoice::row);
	REQUIRE(toCarol.size() == 2);
	CHECK(toCarol[0] == (TerminalChoice{ 0, TARGETING, 2 }));  // Bob's, the latest
	CHECK(toCarol[1] == (TerminalChoice{ 0, DESTRUCT, 1 }));
}
