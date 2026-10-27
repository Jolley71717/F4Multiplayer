#include "Test.h"

#include "game/ClickRates.h"

using namespace std::chrono_literals;
using ClickRates::Source;

namespace
{
	const ClickRates::Clock::time_point T0 = ClickRates::Clock::time_point{} + 1000s;
}

TEST("ClickRates: events within a source's limit in a window are not a repeat")
{
	ClickRates::Rates rates;
	const auto        limit = ClickRates::InfoOf(Source::kDrawAsk).limit;
	for (std::uint32_t i = 0; i < limit; ++i) {
		CHECK(!rates.Note(Source::kDrawAsk, T0 + std::chrono::milliseconds(i * 100)));
	}
	CHECK(rates.InWindow(Source::kDrawAsk) == limit);
	CHECK(rates.Repeats(Source::kDrawAsk) == 0);
	CHECK(rates.Peak(Source::kDrawAsk) == limit);
}

TEST("ClickRates: the event past the limit is reported once, then not again within the report gap")
{
	ClickRates::Rates rates;
	const auto        limit = ClickRates::InfoOf(Source::kHud).limit;
	std::uint32_t     reports = 0;
	for (std::uint32_t i = 0; i < limit + 20; ++i) {
		reports += rates.Note(Source::kHud, T0 + std::chrono::milliseconds(i * 10));
	}
	CHECK(reports == 1);
	CHECK(rates.Repeats(Source::kHud) == 1);
	// A repeat that goes on is said again a REPORT_EVERY later.
	std::uint32_t later = 0;
	for (std::uint32_t i = 0; i < limit + 1; ++i) {
		later += rates.Note(Source::kHud, T0 + ClickRates::REPORT_EVERY + 1s + std::chrono::milliseconds(i * 10));
	}
	CHECK(later == 1);
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
	// A holster asked every 0.2 s: 50 in a 10 s window, over the limit of 12.
	for (int i = 0; i < 600; ++i) {
		rates.Note(Source::kDrawAsk, T0 + i * 200ms);
	}
	CHECK(rates.Repeats(Source::kDrawAsk) >= 10);
	CHECK(rates.Peak(Source::kDrawAsk) > ClickRates::InfoOf(Source::kDrawAsk).limit);
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
	rates.Note(Source::kHud, T0);
	rates.Note(Source::kHud, T0 + 1s);
	const auto line = rates.Describe();
	CHECK(line.find("hud=2(peak 2/") != std::string::npos);
	CHECK(line.find("drawAsks") == std::string::npos);
	CHECK(line.find("repeats=0") != std::string::npos);
}
