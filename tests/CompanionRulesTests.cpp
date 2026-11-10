// What our game says and does about companions (src/game/CompanionRules.h): reports from the aliases' contents, the
// server's word, and the dismissal of a refused recruit.

#include "Test.h"

#include "game/CompanionRules.h"

using namespace std::chrono_literals;
using CompanionRules::Book;
using CompanionRules::Held;
using CompanionRules::Slot;
using CompanionRules::Verdict;
using State = Protocol::CompanionState;

namespace
{
	constexpr std::uint32_t DOGMEAT = 0x0001D162;
	constexpr std::uint32_t PIPER = 0x0002F1F8;
	constexpr std::uint32_t US = 2;
	constexpr std::uint32_t THEM = 1;

	const auto T0 = CompanionRules::Clock::time_point{} + 1h;

	Protocol::CompanionOwner Owner(std::uint32_t a_id, std::uint32_t a_player) { return { a_id, a_player, State::kOwner }; }
}

TEST("companion rules: an NPC the aliases hold is reported recruited once, again if unanswered, and let go when they drop it")
{
	Book book;
	auto reports = book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0);
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].refId == DOGMEAT && reports[0].recruited);
	CHECK(book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0 + 1s).empty());  // not again right away
	reports = book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0 + CompanionRules::REPORT_RETRY);
	REQUIRE(reports.size() == 1);  // unanswered: again
	CHECK(reports[0].recruited);
	CHECK(book.OnOwner(Owner(DOGMEAT, US), US, T0 + 6s).verdict == Verdict::kOurs);
	CHECK(book.IsOurs(DOGMEAT));
	CHECK(book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0 + 1min).empty());  // answered: never again while held

	// A second companion joins; then the dog is let go.
	reports = book.Look({ { DOGMEAT, Slot::kDogmeat }, { PIPER, Slot::kCompanion } }, T0 + 2min);
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].refId == PIPER && reports[0].recruited);
	CHECK(book.OnOwner(Owner(PIPER, US), US, T0 + 2min + 1s).verdict == Verdict::kOurs);
	reports = book.Look({ { PIPER, Slot::kCompanion } }, T0 + 3min);
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].refId == DOGMEAT && !reports[0].recruited);
	CHECK(!book.IsOurs(DOGMEAT));
	CHECK(book.Recruited() == 1);
	CHECK(book.DueDismissal(T0 + 3min) == std::nullopt);
}

TEST("companion rules: a refused recruit is dismissed a moment after the word, again while the aliases still hold it, then given up on")
{
	Book book;
	(void)book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0);
	const auto word = book.OnOwner(Owner(DOGMEAT, THEM), US, T0 + 100ms);
	CHECK(word.verdict == Verdict::kRefused);
	CHECK(word.playerId == THEM);
	CHECK(!word.held);
	CHECK(book.OwnerOf(DOGMEAT) == THEM);
	// Not at once: the recruit's own scripts finish first.
	CHECK(!book.DueDismissal(T0 + 100ms));
	auto due = book.DueDismissal(T0 + 100ms + CompanionRules::DISMISS_DELAY);
	REQUIRE(due);
	CHECK(due->id == DOGMEAT && due->slot == Slot::kDogmeat && due->attempt == 1);
	book.Dismissing(DOGMEAT, T0 + 2s);
	CHECK(!book.DueDismissal(T0 + 2s + CompanionRules::DISMISS_RETRY - 1ms));
	// The same word again (a broadcast, a repeat) changes nothing: one refusal, one notice.
	CHECK(book.OnOwner(Owner(DOGMEAT, THEM), US, T0 + 3s).verdict == Verdict::kTheirs);
	due = book.DueDismissal(T0 + 2s + CompanionRules::DISMISS_RETRY);
	REQUIRE(due);
	CHECK(due->attempt == 2);
	book.Dismissing(DOGMEAT, T0 + 12s);
	due = book.DueDismissal(T0 + 22s);
	REQUIRE(due);
	CHECK(due->attempt == 3);
	book.Dismissing(DOGMEAT, T0 + 22s);
	CHECK(!book.DueDismissal(T0 + 1h));  // no fourth
	CHECK(book.GivenUp(T0 + 31s).empty());
	const auto gaveUp = book.GivenUp(T0 + 32s);
	REQUIRE(gaveUp.size() == 1);
	CHECK(gaveUp[0] == DOGMEAT);
	CHECK(book.GivenUp(T0 + 1h).empty());  // said once
	CHECK(book.Describe().find("dismissals=3") != std::string::npos);
	CHECK(book.Describe().find("gaveUp=1") != std::string::npos);
	CHECK(book.Describe().find("dismissed=0") != std::string::npos);
	// Given up on, the aliases letting it go later counts as nothing.
	(void)book.Look({}, T0 + 2h);
	CHECK(book.Describe().find("dismissed=0") != std::string::npos);
}

TEST("companion rules: a dismissal that takes ends the retries and counts")
{
	Book book;
	(void)book.Look({ { PIPER, Slot::kCompanion } }, T0);
	REQUIRE(book.OnOwner(Owner(PIPER, THEM), US, T0).verdict == Verdict::kRefused);
	const auto due = book.DueDismissal(T0 + 2s);
	REQUIRE(due);
	CHECK(due->slot == Slot::kCompanion);
	book.Dismissing(PIPER, T0 + 2s);
	// The script let her go: the aliases are empty next look, and that is reported.
	const auto reports = book.Look({}, T0 + 3s);
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].refId == PIPER && !reports[0].recruited);
	CHECK(!book.DueDismissal(T0 + 1h));
	CHECK(book.GivenUp(T0 + 1h).empty());
	CHECK(book.Describe().find("dismissed=1") != std::string::npos);
	CHECK(book.Describe().find("refused=1") != std::string::npos);
}

TEST("companion rules: a companion held for a player who left refuses our recruit too, worded as held")
{
	Book book;
	(void)book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0);
	const auto word = book.OnOwner({ DOGMEAT, THEM, State::kHeld }, US, T0);
	CHECK(word.verdict == Verdict::kRefused);
	CHECK(word.held);
	CHECK(word.playerId == THEM);
	CHECK(book.DueDismissal(T0 + 2s));
}

TEST("companion rules: words about NPCs we don't hold are news, and free forgets the owner")
{
	Book book;
	CHECK(book.OnOwner(Owner(DOGMEAT, THEM), US, T0).verdict == Verdict::kTheirs);
	CHECK(book.OwnerOf(DOGMEAT) == THEM);
	CHECK(book.OnOwner({ DOGMEAT, THEM, State::kHeld }, US, T0).verdict == Verdict::kHeld);
	CHECK(book.OnOwner({ DOGMEAT, 0, State::kFree }, US, T0).verdict == Verdict::kFree);
	CHECK(!book.OwnerOf(DOGMEAT));
	CHECK(!book.DueDismissal(T0 + 1h));
	CHECK(book.Known() == 0);
	// A word that we own one we never reported (a rejoin's welcome lists it before our look): ours, nothing to dismiss.
	CHECK(book.OnOwner(Owner(PIPER, US), US, T0).verdict == Verdict::kOurs);
	CHECK(!book.IsOurs(PIPER));  // not in our aliases yet
	CHECK(book.Look({ { PIPER, Slot::kCompanion } }, T0 + 1s).size() == 1);  // reported when they are
}

TEST("companion rules: full is nothing to do, and a load reports the aliases again")
{
	Book book;
	(void)book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0);
	CHECK(book.OnOwner({ DOGMEAT, 0, State::kFull }, US, T0).verdict == Verdict::kFull);
	CHECK(!book.DueDismissal(T0 + 1h));
	CHECK(book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0 + 1h).empty());  // answered, if with a refusal
	book.Reload();
	const auto reports = book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0 + 1h + 1s);
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].recruited);
	// Refused again after the load: dismissed again.
	CHECK(book.OnOwner(Owner(DOGMEAT, THEM), US, T0 + 1h + 2s).verdict == Verdict::kRefused);
	CHECK(book.DueDismissal(T0 + 1h + 4s));
	book.Reset();
	CHECK(book.Recruited() == 0 && book.Known() == 0);
}

TEST("companion rules: an owner that was ours and is refused later (the server's book changed under a load) is dismissed")
{
	Book book;
	(void)book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0);
	CHECK(book.OnOwner(Owner(DOGMEAT, US), US, T0).verdict == Verdict::kOurs);
	CHECK(book.OnOwner(Owner(DOGMEAT, THEM), US, T0 + 1s).verdict == Verdict::kRefused);
	CHECK(!book.IsOurs(DOGMEAT));
	CHECK(book.DueDismissal(T0 + 3s));
	// Ours again (the server's book settled on us): the dismissal is off.
	CHECK(book.OnOwner(Owner(DOGMEAT, US), US, T0 + 2s).verdict == Verdict::kOurs);
	CHECK(!book.DueDismissal(T0 + 1h));
}
