// What our game says and does about companions (src/game/CompanionRules.h): reports from the aliases' contents, the
// server's word, and the dismissal of a refused recruit.

#include "Test.h"

#include "game/CompanionRules.h"

#include <algorithm>

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

TEST("companion rules: one the server says is ours that the aliases don't hold is reported let go: a save loaded without it, or a rejoin's hand-back")
{
	// A save loaded without the companion: the server still has it as ours; the next look says we let it go, once.
	Book book;
	(void)book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0);
	CHECK(book.OnOwner(Owner(DOGMEAT, US), US, T0).verdict == Verdict::kOurs);
	book.Reload();
	auto reports = book.Look({}, T0 + 1s);
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].refId == DOGMEAT && !reports[0].recruited);
	CHECK(book.Look({}, T0 + 2s).empty());  // said once
	CHECK(book.Describe().find("letGo=1") != std::string::npos);
	// The server's word follows: free. And a repeat of its word that it is ours says it again.
	CHECK(book.OnOwner({ DOGMEAT, 0, State::kFree }, US, T0 + 3s).verdict == Verdict::kFree);
	CHECK(book.OnOwner(Owner(DOGMEAT, US), US, T0 + 4s).verdict == Verdict::kOurs);
	reports = book.Look({}, T0 + 5s);
	REQUIRE(reports.size() == 1);
	CHECK(!reports[0].recruited);

	// A rejoin's welcome lists the companion as ours (held through the drop); the save loaded holds it: reported recruited,
	// nothing let go. One it doesn't hold: let go.
	Book rejoined;
	CHECK(rejoined.OnOwner(Owner(DOGMEAT, US), US, T0).verdict == Verdict::kOurs);
	CHECK(rejoined.OnOwner(Owner(PIPER, US), US, T0).verdict == Verdict::kOurs);
	reports = rejoined.Look({ { DOGMEAT, Slot::kDogmeat } }, T0 + 1s);
	REQUIRE(reports.size() == 2);
	const auto dog = std::ranges::find_if(reports, [](const auto& a_r) { return a_r.refId == DOGMEAT; });
	const auto piper = std::ranges::find_if(reports, [](const auto& a_r) { return a_r.refId == PIPER; });
	REQUIRE(dog != reports.end() && piper != reports.end());
	CHECK(dog->recruited);
	CHECK(!piper->recruited);
	// Another's, or one held for another, is never ours to let go.
	Book other;
	CHECK(other.OnOwner(Owner(DOGMEAT, THEM), US, T0).verdict == Verdict::kTheirs);
	CHECK(other.OnOwner({ PIPER, THEM, State::kHeld }, US, T0).verdict == Verdict::kHeld);
	CHECK(other.Look({}, T0 + 1s).empty());
}

TEST("companion rules: a refused copy whose owner lets the NPC go before our dismissal is reported again, not dismissed")
{
	Book book;
	(void)book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0);
	REQUIRE(book.OnOwner(Owner(DOGMEAT, THEM), US, T0).verdict == Verdict::kRefused);
	CHECK(book.OnOwner({ DOGMEAT, 0, State::kFree }, US, T0 + 500ms).verdict == Verdict::kFree);
	CHECK(!book.DueDismissal(T0 + 1h));
	const auto reports = book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0 + 1s);
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].refId == DOGMEAT && reports[0].recruited);
	CHECK(book.OnOwner(Owner(DOGMEAT, US), US, T0 + 2s).verdict == Verdict::kOurs);
	CHECK(book.IsOurs(DOGMEAT));
	CHECK(book.Describe().find("reclaimed=1") != std::string::npos);
	// Free after a dismissal that took (the aliases are empty) is nothing to reclaim.
	Book done;
	(void)done.Look({ { PIPER, Slot::kCompanion } }, T0);
	REQUIRE(done.OnOwner(Owner(PIPER, THEM), US, T0).verdict == Verdict::kRefused);
	done.Dismissing(PIPER, T0 + 2s);
	(void)done.Look({}, T0 + 3s);
	CHECK(done.OnOwner({ PIPER, 0, State::kFree }, US, T0 + 4s).verdict == Verdict::kFree);
	CHECK(done.Look({}, T0 + 5s).empty());
}

TEST("companion rules: a look while the game isn't settled (a save or load, a loading screen, no cell) reports nothing, whatever the aliases read")
{
	// Ours, held; then a load starts: the aliases read empty for a while. No let-go, no change; the first settled look
	// with the save's aliases says it all.
	Book book;
	(void)book.Look({ { DOGMEAT, Slot::kDogmeat }, { PIPER, Slot::kCompanion } }, T0);
	CHECK(book.OnOwner(Owner(DOGMEAT, US), US, T0).verdict == Verdict::kOurs);
	CHECK(book.OnOwner(Owner(PIPER, US), US, T0).verdict == Verdict::kOurs);
	for (int i = 1; i <= 20; ++i) {
		CHECK(book.Look({}, T0 + i * 500ms, false).empty());
	}
	CHECK(book.Recruited() == 2);
	CHECK(book.IsOurs(DOGMEAT) && book.IsOurs(PIPER));
	// Settled, the aliases hold both again: nothing new.
	CHECK(book.Look({ { DOGMEAT, Slot::kDogmeat }, { PIPER, Slot::kCompanion } }, T0 + 11s, true).empty());
	CHECK(book.Describe().find("letGo=0") != std::string::npos);
	// A load whose save holds Dogmeat alone: Piper is let go once settled, not before.
	CHECK(book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0 + 12s, false).empty());
	book.Reload();
	CHECK(book.Look({}, T0 + 13s, false).empty());
	const auto reports = book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0 + 14s, true);
	REQUIRE(reports.size() == 2);
	const auto dog = std::ranges::find_if(reports, [](const auto& a_r) { return a_r.refId == DOGMEAT; });
	const auto piper = std::ranges::find_if(reports, [](const auto& a_r) { return a_r.refId == PIPER; });
	REQUIRE(dog != reports.end() && piper != reports.end());
	CHECK(dog->recruited && !piper->recruited);
	// A pending dismissal is not advanced by an unsettled look either (the game skips it; the book just answers nothing).
	Book refused;
	(void)refused.Look({ { DOGMEAT, Slot::kDogmeat } }, T0);
	REQUIRE(refused.OnOwner(Owner(DOGMEAT, THEM), US, T0).verdict == Verdict::kRefused);
	CHECK(refused.Look({}, T0 + 5s, false).empty());
	CHECK(refused.Recruited() == 1);
	CHECK(refused.DueDismissal(T0 + 5s));
}

TEST("companion rules: a refused copy let go still knows the owner; only our own spent word is forgotten")
{
	// The rig saw the refused game's known drop to 0 after its let-go while the owner still held the companion.
	Book book;
	(void)book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0);
	REQUIRE(book.OnOwner(Owner(DOGMEAT, THEM), US, T0).verdict == Verdict::kRefused);
	book.Dismissing(DOGMEAT, T0 + 2s);
	const auto reports = book.Look({}, T0 + 3s);
	REQUIRE(reports.size() == 1);
	CHECK(!reports[0].recruited);
	CHECK(book.Known() == 1);
	CHECK(book.OwnerOf(DOGMEAT) == THEM);
	// Ours let go: the word is spent, known drops, and the server's free follows.
	Book ours;
	(void)ours.Look({ { PIPER, Slot::kCompanion } }, T0);
	REQUIRE(ours.OnOwner(Owner(PIPER, US), US, T0).verdict == Verdict::kOurs);
	CHECK(ours.Look({}, T0 + 1s).size() == 1);
	CHECK(ours.Known() == 0);
}

TEST("companion rules: a copy given up on after three dismissals is reported again once the companion is freed")
{
	Book book;
	(void)book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0);
	REQUIRE(book.OnOwner(Owner(DOGMEAT, THEM), US, T0).verdict == Verdict::kRefused);
	for (int i = 0; i < CompanionRules::DISMISS_TRIES; ++i) {
		book.Dismissing(DOGMEAT, T0 + 2s + i * CompanionRules::DISMISS_RETRY);
	}
	CHECK(book.GivenUp(T0 + 1h).size() == 1);
	CHECK(book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0 + 1h).empty());
	CHECK(book.OnOwner({ DOGMEAT, 0, State::kFree }, US, T0 + 1h + 1s).verdict == Verdict::kFree);
	const auto reports = book.Look({ { DOGMEAT, Slot::kDogmeat } }, T0 + 1h + 2s);
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].refId == DOGMEAT && reports[0].recruited);
	CHECK(book.OnOwner(Owner(DOGMEAT, US), US, T0 + 1h + 3s).verdict == Verdict::kOurs);
	CHECK(book.IsOurs(DOGMEAT));
	CHECK(!book.DueDismissal(T0 + 2h));
	CHECK(book.Describe().find("reclaimed=1") != std::string::npos);
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

TEST("companion rules: an alias is found by name with its id, 0 among them; one not in the quest is none, never 0")
{
	using CompanionRules::AliasEntry;
	// The Followers quest as the game has it: Companion is alias 0, DogmeatCompanion 5.
	const std::vector<AliasEntry> followers{ { "Companion", 0 }, { "Unused", 1 }, { nullptr, 2 }, { "DogmeatCompanion", 5 } };
	auto found = CompanionRules::FindFollowerAliases(followers, "Companion", "DogmeatCompanion");
	REQUIRE(found.companion.has_value());
	CHECK(*found.companion == 0);
	REQUIRE(found.dogmeat.has_value());
	CHECK(*found.dogmeat == 5);
	CHECK(found.Any());
	CHECK(CompanionRules::AliasText(found.companion) == "0");

	// The name as the game compares it (case ignored); a name that only starts the same is another alias.
	CHECK(CompanionRules::AliasNamed(followers, "companion") == std::optional<std::uint32_t>{ 0 });
	CHECK(CompanionRules::AliasNamed(followers, "Compan") == std::nullopt);
	CHECK(CompanionRules::AliasNamed(followers, "CompanionX") == std::nullopt);

	// A quest with only the Dogmeat alias: the companion one is none, not 0; with neither, nothing at all.
	found = CompanionRules::FindFollowerAliases({ { "DogmeatCompanion", 0 } }, "Companion", "DogmeatCompanion");
	CHECK(!found.companion.has_value());
	CHECK(found.dogmeat == std::optional<std::uint32_t>{ 0 });
	CHECK(found.Any());
	found = CompanionRules::FindFollowerAliases({ { "Other", 0 }, { "Companion", CompanionRules::NO_ALIAS } }, "Companion", "DogmeatCompanion");
	CHECK(!found.companion.has_value());
	CHECK(!found.dogmeat.has_value());
	CHECK(!found.Any());
	CHECK(CompanionRules::AliasText(found.companion) == "none");
}
