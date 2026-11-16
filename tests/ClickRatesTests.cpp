#include "Test.h"

#include "game/ClickRates.h"
#include "game/DrawMatch.h"

using namespace std::chrono_literals;
using ClickRates::Source;

namespace
{
	const ClickRates::Clock::time_point T0 = ClickRates::Clock::time_point{} + 1000s;
	const std::uint32_t                 TRIES = DrawMatch::MAX_TRIES;
}

TEST("ClickRates: events within a source's limit in a window are not a repeat")
{
	ClickRates::Rates rates;
	const auto        limit = ClickRates::InfoOf(Source::kEquip).limit;
	for (std::uint32_t i = 0; i < limit; ++i) {
		CHECK(!rates.Note(Source::kEquip, T0 + std::chrono::milliseconds(i * 100)));
	}
	CHECK(rates.InWindow(Source::kEquip) == limit);
	CHECK(rates.Repeats(Source::kEquip) == 0);
	CHECK(rates.Peak(Source::kEquip) == limit);
}

TEST("ClickRates: the event past the limit is reported once, then not again within the report gap")
{
	ClickRates::Rates rates;
	const auto        limit = ClickRates::InfoOf(Source::kEmote).limit;
	std::uint32_t     reports = 0;
	for (std::uint32_t i = 0; i < limit + 20; ++i) {
		reports += rates.Note(Source::kEmote, T0 + std::chrono::milliseconds(i * 10));
	}
	CHECK(reports == 1);
	CHECK(rates.Repeats(Source::kEmote) == 1);
	// A repeat that goes on is said again a REPORT_EVERY later.
	std::uint32_t later = 0;
	for (std::uint32_t i = 0; i < limit + 1; ++i) {
		later += rates.Note(Source::kEmote, T0 + ClickRates::REPORT_EVERY + 1s + std::chrono::milliseconds(i * 10));
	}
	CHECK(later == 1);
}

TEST("ClickRates: sources repeating together are reported once a minute between them, not once each")
{
	ClickRates::Rates rates;
	std::uint32_t     reports = 0;
	// Every source over its limit in every window, for ten minutes: the log gets a line a minute at most.
	for (int i = 0; i < 6000; ++i) {
		for (std::size_t s = 0; s < static_cast<std::size_t>(Source::kCount); ++s) {
			reports += rates.Note(static_cast<Source>(s), T0 + i * 100ms, 5);
		}
	}
	CHECK(reports >= 9);
	CHECK(reports <= 10);
	// A source held back by another's report is not lost: it still counts, and says so once the gap is over.
	ClickRates::Rates two;
	CHECK(two.Note(Source::kEquip, T0, 100));
	CHECK(!two.Note(Source::kEmote, T0 + 1s, 100));
	CHECK(two.Repeats(Source::kEmote) == 1);
	CHECK(two.Note(Source::kEmote, T0 + ClickRates::REPORT_EVERY + 1s, 100));
}

TEST("ClickRates: a slow steady source never is a repeat, however long it goes on")
{
	ClickRates::Rates rates;
	std::uint32_t     reports = 0;
	// A check of the wrist every 6 s, for an hour.
	for (int i = 0; i < 600; ++i) {
		reports += rates.Note(Source::kEmote, T0 + i * 6s);
	}
	CHECK(reports == 0);
	CHECK(rates.Total(Source::kEmote) == 600);
	CHECK(rates.Repeats(Source::kEmote) == 0);
	CHECK(rates.Peak(Source::kEmote) <= 2);
}

TEST("ClickRates: a source clicking five times a second is a repeat in every window it goes on in")
{
	ClickRates::Rates rates;
	// An emote started every 0.2 s: 50 in a 10 s window, over its limit of 8.
	for (int i = 0; i < 600; ++i) {
		rates.Note(Source::kEmote, T0 + i * 200ms);
	}
	CHECK(rates.Repeats(Source::kEmote) >= 10);
	CHECK(rates.Peak(Source::kEmote) > ClickRates::InfoOf(Source::kEmote).limit);
}

TEST("ClickRates: sources are counted apart")
{
	ClickRates::Rates rates;
	for (int i = 0; i < 100; ++i) {
		rates.Note(Source::kEquip, T0 + i * 10ms);
	}
	CHECK(rates.Total(Source::kEquip) == 100);
	CHECK(rates.Total(Source::kDrawAsk) == 0);
	CHECK(rates.Repeats(Source::kDrawAsk) == 0);
}

TEST("ClickRates: a_count notes several at once")
{
	ClickRates::Rates rates;
	const auto        limit = ClickRates::InfoOf(Source::kVoiceUnderrun).limit;
	CHECK(rates.Note(Source::kVoiceUnderrun, T0, limit + 1));
	CHECK(rates.Total(Source::kVoiceUnderrun) == limit + 1);
}

TEST("ClickRates: the status line names the sources that did something, and the repeats")
{
	ClickRates::Rates rates;
	CHECK(rates.Describe() == "clicks: none repeats=0");
	rates.Note(Source::kEmote, T0);
	rates.Note(Source::kEmote, T0 + 1s);
	const auto line = rates.Describe();
	CHECK(line.find("emotes=2(peak 2/") != std::string::npos);
	CHECK(line.find("drawAsks") == std::string::npos);
	CHECK(line.find("hud") == std::string::npos);
	CHECK(line.find("repeats=0") != std::string::npos);
}

TEST("ClickRates: one thing's own window starts at its first event and again once it is over")
{
	ClickRates::Own own;
	CHECK(own.Note(T0) == 1);
	CHECK(own.Note(T0 + 3s) == 2);
	CHECK(own.Note(T0 + ClickRates::WINDOW - 1ms) == 3);
	CHECK(own.Note(T0 + ClickRates::WINDOW) == 1);
	CHECK(own.Note(T0 + ClickRates::WINDOW + 1s) == 2);
	// Long silent, then again: a new window.
	CHECK(own.Note(T0 + 10min) == 1);
}

TEST("ClickRates: many stand-ins asked a few times each together are not a repeating sound")
{
	// Six of a friend's NPCs whose draws stall after a cell loaded: each asked at the same moments, every 3 s, four
	// times (DrawMatch's tries). 24 asks in a window, 4 of any one: counted together, the 13th said "repeating sound".
	ClickRates::Rates              rates;
	std::array<ClickRates::Own, 6> copies;
	std::uint32_t                  reports = 0;
	for (std::uint32_t ask = 0; ask < TRIES; ++ask) {
		for (auto& copy : copies) {
			const auto now = T0 + ask * 3s;
			reports += rates.NoteOwn(Source::kDrawAsk, now, copy.Note(now));
		}
	}
	CHECK(reports == 0);
	CHECK(rates.Total(Source::kDrawAsk) == 6u * TRIES);
	CHECK(rates.Peak(Source::kDrawAsk) == TRIES);
	CHECK(rates.Repeats(Source::kDrawAsk) == 0);
	CHECK(rates.Describe().find(std::format("drawAsks=24(peak 4/{})", ClickRates::InfoOf(Source::kDrawAsk).limit)) != std::string::npos);
}

TEST("ClickRates: one stand-in asked once a second without end is a repeating sound, alone or among others")
{
	// What the count is for: one stand-in whose draw is asked every second for the rest of the session. Counted with
	// every other stand-in under one limit of 12, its 10 a window were never said.
	ClickRates::Rates rates;
	ClickRates::Own   stuck;
	ClickRates::Own   other;
	std::uint32_t     reports = 0;
	std::uint32_t     firstAt = 0;
	for (std::uint32_t s = 0; s < 120; ++s) {
		const auto now = T0 + s * 1s + s * 16ms;  // a second and a frame apart
		const bool report = rates.NoteOwn(Source::kDrawAsk, now, stuck.Note(now));
		if (report && reports == 0) {
			firstAt = s;
		}
		reports += report;
		if (s % 20 == 0) {
			reports += rates.NoteOwn(Source::kDrawAsk, now, other.Note(now));  // another, drawing now and then: never it
		}
	}
	CHECK(firstAt == ClickRates::InfoOf(Source::kDrawAsk).limit);  // its ninth ask in ten seconds
	CHECK(reports == 2);                                           // said again a minute later, not every window
	CHECK(rates.Repeats(Source::kDrawAsk) >= 10);
}

TEST("ClickRates: a stand-in whose draw never takes, or stalls every time, stays under the draw-ask limit")
{
	// DrawMatch as Puppets runs it, a frame at a time for a minute: a draw the graph refuses (the state stays sheathed),
	// and one that stalls on its way each time (set back and asked again). Both end at MAX_TRIES asks.
	const auto limit = ClickRates::InfoOf(Source::kDrawAsk).limit;
	for (const bool stalls : { false, true }) {
		ClickRates::Rates rates;
		ClickRates::Own   own;
		DrawMatch::Track  track;
		auto              state = DrawMatch::State::kSheathed;
		std::uint32_t     most = 0;
		std::uint32_t     reports = 0;
		for (int ms = 0; ms < 60000; ms += 16) {
			const auto now = T0 + std::chrono::milliseconds(ms);
			const auto what = DrawMatch::Next(track, true, state, false, ClickRates::Clock::time_point{}, now);
			if (what == DrawMatch::Do::kNothing) {
				continue;
			}
			if (stalls) {
				state = DrawMatch::State::kWantToDraw;  // the engine takes the ask, the graph never plays it
			}
			const auto mine = own.Note(now);
			most = (std::max)(most, mine);
			reports += rates.NoteOwn(Source::kDrawAsk, now, mine);
		}
		CHECK(track.gaveUp);
		CHECK(most == TRIES);
		CHECK(most <= limit);
		CHECK(reports == 0);
	}
}

TEST("ClickRates: the stand-in said to repeat is the one past its limit, not another asked in the same window")
{
	ClickRates::Rates rates;
	ClickRates::Own   stuck;
	ClickRates::Own   other;
	const auto        limit = ClickRates::InfoOf(Source::kDrawAsk).limit;
	// Another source's report holds this one's back for a while.
	CHECK(rates.Note(Source::kEquip, T0 - 55s, 100));
	for (std::uint32_t i = 0; i < limit + 1; ++i) {
		CHECK(!rates.NoteOwn(Source::kDrawAsk, T0 + i * 100ms, stuck.Note(T0 + i * 100ms)));
	}
	CHECK(rates.Repeats(Source::kDrawAsk) == 1);
	// The gap is over, the window still the one the first repeated in: one asked once is not what repeats.
	CHECK(!rates.NoteOwn(Source::kDrawAsk, T0 + 6s, other.Note(T0 + 6s)));
	CHECK(rates.NoteOwn(Source::kDrawAsk, T0 + 7s, stuck.Note(T0 + 7s)));
}
