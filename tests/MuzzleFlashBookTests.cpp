#include "Test.h"

#include "game/MuzzleFlashBook.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace std::chrono_literals;
using MuzzleFlashBook::Book;
using MuzzleFlashBook::Clock;
using MuzzleFlashBook::FlashSeconds;
using MuzzleFlashBook::Kind;

namespace
{
	const auto              START = Clock::time_point{} + 1h;
	constexpr std::uint32_t STAND_IN = 0xFF000801;
	constexpr std::uint32_t OTHER = 0xFF000802;
}

TEST("MuzzleFlashBook: a projectile's flash time is kept within bounds, nonsense gets the default")
{
	CHECK(FlashSeconds(0.1f) == 0.1f);
	CHECK(FlashSeconds(0.05f) == 0.05f);
	CHECK(FlashSeconds(0.001f) == MuzzleFlashBook::MIN_SECONDS);
	CHECK(FlashSeconds(1.0f) == MuzzleFlashBook::MAX_SECONDS);
	CHECK(FlashSeconds(0.0f) == MuzzleFlashBook::DEFAULT_SECONDS);
	CHECK(FlashSeconds(-1.0f) == MuzzleFlashBook::DEFAULT_SECONDS);
	CHECK(FlashSeconds(1e9f) == MuzzleFlashBook::DEFAULT_SECONDS);
	CHECK(FlashSeconds(std::numeric_limits<float>::quiet_NaN()) == MuzzleFlashBook::DEFAULT_SECONDS);
	CHECK(FlashSeconds(std::numeric_limits<float>::infinity()) == MuzzleFlashBook::DEFAULT_SECONDS);
}

TEST("MuzzleFlashBook: a shot shows a flash, which comes off after its time")
{
	Book book;
	REQUIRE(book.Ask(STAND_IN, START) == Kind::kShow);
	book.Shown(STAND_IN, START, 0.1f);
	CHECK(book.Shows(STAND_IN));
	CHECK(book.TakeDue(START + 50ms).empty());
	const auto due = book.TakeDue(START + 101ms);
	REQUIRE(due.size() == 1);
	CHECK(due[0] == STAND_IN);
	CHECK(!book.Shows(STAND_IN));
	CHECK(book.TakeDue(START + 200ms).empty());  // once
}

TEST("MuzzleFlashBook: automatic fire keeps one flash up instead of showing a new one per shot")
{
	Book book;
	book.Shown(STAND_IN, START, 0.1f);
	auto at = START;
	for (int shot = 1; shot <= 20; ++shot) {
		at += 60ms;  // 16 shots a second: each comes while the last flash still shows
		REQUIRE(book.Ask(STAND_IN, at) == Kind::kExtend);
		book.Extended(STAND_IN, at, 0.1f);
		CHECK(book.TakeDue(at).empty());
	}
	CHECK(book.Showing() == 1);
	CHECK(book.TakeDue(at + 99ms).empty());
	CHECK(book.TakeDue(at + 101ms).size() == 1);  // off a flash's time after the last shot
}

TEST("MuzzleFlashBook: keeping a flash up never shortens it")
{
	Book book;
	book.Shown(STAND_IN, START, 0.2f);
	book.Extended(STAND_IN, START + 10ms, 0.05f);
	CHECK(book.TakeDue(START + 150ms).empty());
	CHECK(book.TakeDue(START + 201ms).size() == 1);
}

TEST("MuzzleFlashBook: a shooter whose flash is gone flashes again only after the gap")
{
	Book book;
	book.Shown(STAND_IN, START, 0.03f);
	CHECK(book.TakeDue(START + 31ms).size() == 1);
	CHECK(book.Ask(STAND_IN, START + 40ms) == Kind::kTooSoon);
	CHECK(book.Ask(STAND_IN, START + MuzzleFlashBook::MIN_GAP) == Kind::kShow);
	CHECK(book.Ask(OTHER, START + 40ms) == Kind::kShow);  // the gap is per shooter
}

TEST("MuzzleFlashBook: no more than MAX_SHOWING flashes show at once")
{
	Book book;
	for (std::uint32_t i = 0; i < MuzzleFlashBook::MAX_SHOWING; ++i) {
		REQUIRE(book.Ask(STAND_IN + i, START) == Kind::kShow);
		book.Shown(STAND_IN + i, START, 0.1f);
	}
	CHECK(book.Ask(0xFF001000, START) == Kind::kTooMany);
	CHECK(book.Ask(STAND_IN, START + 10ms) == Kind::kExtend);  // one showing is still kept up
	(void)book.TakeDue(START + 101ms);
	CHECK(book.Ask(0xFF001000, START + 101ms) == Kind::kShow);
}

TEST("MuzzleFlashBook: a stand-in deleted mid-flash, and a save, take flashes off at once")
{
	Book book;
	book.Shown(STAND_IN, START, 0.1f);
	book.Shown(OTHER, START, 0.1f);
	CHECK(book.Forget(STAND_IN));
	CHECK(!book.Forget(STAND_IN));
	CHECK(book.Ask(STAND_IN, START + 1ms) == Kind::kShow);  // forgotten entirely: a new stand-in with its id may flash
	const auto all = book.TakeAll();
	REQUIRE(all.size() == 1);
	CHECK(all[0] == OTHER);
	CHECK(book.Showing() == 0);
	CHECK(book.TakeDue(START + 1s).empty());
}

TEST("MuzzleFlashBook: keeping up a flash that is not showing does nothing")
{
	Book book;
	book.Extended(STAND_IN, START, 0.1f);
	CHECK(!book.Shows(STAND_IN));
	CHECK(book.Ask(STAND_IN, START) == Kind::kShow);
}

TEST("MuzzleFlashBook: many shooters over a long fight leave nothing behind")
{
	Book book;
	auto at = START;
	for (std::uint32_t i = 0; i < 1000; ++i) {
		const auto shooter = 0xFF000000 + (i % 300);
		if (book.Ask(shooter, at) == Kind::kShow) {
			book.Shown(shooter, at, 0.1f);
		}
		at += 20ms;
		(void)book.TakeDue(at);
	}
	(void)book.TakeDue(at + 1s);
	CHECK(book.Showing() == 0);
}
