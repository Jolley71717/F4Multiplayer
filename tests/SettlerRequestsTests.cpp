// A friend's job for a settler at the host's settlement is a request the host's game carries out
// (game/SettlerRequests.h): what the friend's game does with the command, what the host's game checks, and what the
// friend's game makes of the answer.

#include "Test.h"

#include "game/SettlerRequests.h"

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

using SettlerRequests::Facts;
using SettlerRequests::Give;
using SettlerRequests::Given;
using SettlerRequests::Target;
using Why = Protocol::WorkshopAnswer::Why;

namespace
{
	constexpr std::uint32_t BENCH = 0x000250FE;

	// A friend in workshop mode at the host's settlement, a settler the session knows selected, told to use the host's piece.
	constexpr Given Asked()
	{
		Given given;
		given.command = SettlerRequests::COMMAND_ASSIGN;
		given.hasTarget = true;
		given.handsOver = true;
		given.bench = BENCH;
		given.hostRuns = true;
		given.settlerShared = true;
		given.target = Target::kHostsPiece;
		given.editAllowed = true;
		given.pending = false;
		return given;
	}

	// Everything the host's game needs to assign.
	constexpr Facts Fine()
	{
		Facts facts;
		facts.benchKnown = true;
		facts.hostOwned = true;
		facts.benchLoaded = true;
		facts.editAllowed = true;
		facts.settlerKnown = true;
		facts.settlerOfBench = true;
		facts.commandable = true;
		facts.companion = false;
		facts.pieceKnown = true;
		facts.pieceOfBench = true;
		facts.assignable = true;
		return facts;
	}

	std::string_view Notice(const Given& a_given)
	{
		const auto decision = SettlerRequests::OnGive(a_given);
		return decision.notice ? std::string_view{ decision.notice } : std::string_view{};
	}
}

TEST("settler requests: a settlement is the host's to run only on evidence, and never once an answer said it isn't")
{
	CHECK(SettlerRequests::HostRuns(true, false, true, false));   // an applied answer
	CHECK(SettlerRequests::HostRuns(true, false, false, true));   // one of the host's pieces stands there
	CHECK(SettlerRequests::HostRuns(true, false, true, true));
	CHECK(!SettlerRequests::HostRuns(true, false, false, false));  // nothing known: as before the rule
	CHECK(!SettlerRequests::HostRuns(true, true, false, true));    // an answer said the workbench isn't the host's
	CHECK(!SettlerRequests::HostRuns(true, true, true, true));
	CHECK(!SettlerRequests::HostRuns(false, false, true, true));   // the rule off, or we are the host
}

TEST("settler requests: a job for the host's settler at the host's piece is asked of the host's game")
{
	const auto decision = SettlerRequests::OnGive(Asked());
	CHECK(decision.give == Give::kAsk);
	CHECK(decision.notice == nullptr);
	// A settlement's own bed or crop (placed by a plugin, the same reference in every game) is asked the same way.
	auto placed = Asked();
	placed.target = Target::kPlaced;
	CHECK(SettlerRequests::OnGive(placed).give == Give::kAsk);
}

TEST("settler requests: any other command, and any command where the rule isn't in force, is the game's own")
{
	auto given = Asked();
	given.command = SettlerRequests::COMMAND_ASSIGN + 1;
	CHECK(SettlerRequests::OnGive(given).give == Give::kEngine);
	given = Asked();
	given.command = 0;
	CHECK(SettlerRequests::OnGive(given).give == Give::kEngine);
	given = Asked();
	given.hasTarget = false;
	CHECK(SettlerRequests::OnGive(given).give == Give::kEngine);
	given = Asked();
	given.handsOver = false;  // the host's own game, the rule off, a session with no host's game
	CHECK(SettlerRequests::OnGive(given).give == Give::kEngine);
	given.target = Target::kCopy;  // (then even a copy is left to the game, as before the rule)
	CHECK(SettlerRequests::OnGive(given).give == Give::kEngine);
	given = Asked();
	given.bench = 0;  // command mode outside workshop mode: a companion's
	CHECK(SettlerRequests::OnGive(given).give == Give::kEngine);
}

TEST("settler requests: at a settlement not known to be the host's our own settlers take our own pieces, never a copy")
{
	auto given = Asked();
	given.hostRuns = false;
	given.target = Target::kOwn;
	CHECK(SettlerRequests::OnGive(given).give == Give::kEngine);
	given.target = Target::kPlaced;
	CHECK(SettlerRequests::OnGive(given).give == Give::kEngine);
	for (const auto copy : { Target::kHostsPiece, Target::kCopy }) {
		given.target = copy;
		CHECK(SettlerRequests::OnGive(given).give == Give::kRefuse);
		CHECK(Notice(given) == "workshop.shared.assign_copy");
	}
	// Whatever the host allows and whatever waits: nothing is asked there.
	given.target = Target::kOwn;
	given.editAllowed = false;
	given.pending = true;
	CHECK(SettlerRequests::OnGive(given).give == Give::kEngine);
	// Our copy of another player's NPC (the host's recruit, before the settlement is known as the host's): our scripts never
	// give it a job, whatever the piece.
	given.settlerCopy = true;
	for (const auto target : { Target::kOwn, Target::kPlaced, Target::kHostsPiece, Target::kCopy }) {
		given.target = target;
		CHECK(SettlerRequests::OnGive(given).give == Give::kRefuse);
		CHECK(Notice(given) == "workshop.shared.assign_their_settler");
	}
	// Once the settlement is known as the host's, the copy's job is asked of the host.
	given = Asked();
	given.settlerCopy = true;
	CHECK(SettlerRequests::OnGive(given).give == Give::kAsk);
}

TEST("settler requests: the host's game serves a few of a friend's requests a second, each friend apart")
{
	using Clock = SettlerRequests::Pace::Clock;
	SettlerRequests::Pace pace;
	const Clock::time_point start{};
	int served = 0;
	for (int i = 0; i < 20; ++i) {
		served += pace.Take(2, start);
	}
	CHECK(served == static_cast<int>(SettlerRequests::SERVE_BURST));
	CHECK(pace.Take(3, start));  // another friend has its own
	CHECK(!pace.Take(2, start + std::chrono::milliseconds(100)));
	CHECK(pace.Take(2, start + std::chrono::seconds(1)));  // SERVE_PER_SECOND come back
	// A flood over ten seconds is served at about SERVE_PER_SECOND.
	served = 0;
	for (int i = 0; i < 1000; ++i) {
		served += pace.Take(4, start + std::chrono::milliseconds(10 * i));
	}
	CHECK(served <= static_cast<int>(SettlerRequests::SERVE_BURST + 10 * SettlerRequests::SERVE_PER_SECOND) + 1);
}

TEST("settler requests: a settler only our game has is ours to assign, but not to another player's piece")
{
	auto given = Asked();
	given.settlerShared = false;
	given.target = Target::kOwn;
	CHECK(SettlerRequests::OnGive(given).give == Give::kEngine);
	given.target = Target::kPlaced;
	CHECK(SettlerRequests::OnGive(given).give == Give::kEngine);
	for (const auto copy : { Target::kHostsPiece, Target::kCopy }) {
		given.target = copy;
		CHECK(SettlerRequests::OnGive(given).give == Give::kRefuse);
		CHECK(Notice(given) == "workshop.shared.assign_not_hosts");
	}
}

TEST("settler requests: the host's settler is never assigned in the friend's game: each refusal says why")
{
	auto given = Asked();
	given.editAllowed = false;
	CHECK(SettlerRequests::OnGive(given).give == Give::kRefuse);
	CHECK(Notice(given) == "workshop.shared.assign_off");
	// The host's setting comes before a waiting request, and both before the piece.
	given.pending = true;
	given.target = Target::kOwn;
	CHECK(Notice(given) == "workshop.shared.assign_off");
	given = Asked();
	given.pending = true;
	CHECK(SettlerRequests::OnGive(given).give == Give::kRefuse);
	CHECK(Notice(given) == "workshop.shared.assign_pending");
	// A piece of our own game's, or a copy of a piece at another settlement or of another friend's, is not the host's to give.
	for (const auto wrong : { Target::kOwn, Target::kCopy }) {
		given = Asked();
		given.target = wrong;
		CHECK(SettlerRequests::OnGive(given).give == Give::kRefuse);
		CHECK(Notice(given) == "workshop.shared.assign_wrong_piece");
	}
}

TEST("settler requests: the host's game assigns only when every fact holds")
{
	CHECK(!SettlerRequests::Refusal(Fine()));
	CHECK(SettlerRequests::Refusal(Facts{}) == Why::kNotHostOwned);
}

TEST("settler requests: the host's game refuses in order: the settlement, its being loaded, the setting, the settler, the piece")
{
	auto facts = Fine();
	facts.benchKnown = false;
	CHECK(SettlerRequests::Refusal(facts) == Why::kNotHostOwned);
	facts = Fine();
	facts.hostOwned = false;
	facts.benchLoaded = false;  // (the earlier reason wins)
	facts.editAllowed = false;
	CHECK(SettlerRequests::Refusal(facts) == Why::kNotHostOwned);
	facts = Fine();
	facts.benchLoaded = false;
	facts.editAllowed = false;
	facts.settlerKnown = false;
	CHECK(SettlerRequests::Refusal(facts) == Why::kBenchNotLoaded);
	facts = Fine();
	facts.editAllowed = false;
	facts.settlerKnown = false;
	facts.pieceKnown = false;
	CHECK(SettlerRequests::Refusal(facts) == Why::kNotAllowed);
	facts = Fine();
	facts.settlerKnown = false;
	facts.pieceKnown = false;
	CHECK(SettlerRequests::Refusal(facts) == Why::kNoSuchSettler);
	facts = Fine();
	facts.settlerOfBench = false;  // another settlement's settler, or no settler at all
	CHECK(SettlerRequests::Refusal(facts) == Why::kNoSuchSettler);
	facts = Fine();
	facts.commandable = false;  // a turret, a brahmin: linked to the bench, but our own workshop mode offers no Command on it
	CHECK(SettlerRequests::Refusal(facts) == Why::kNoSuchSettler);
	facts = Fine();
	facts.companion = true;  // the game's own command refuses the player's companion too
	CHECK(SettlerRequests::Refusal(facts) == Why::kNoSuchSettler);
	facts = Fine();
	facts.pieceKnown = false;
	facts.assignable = false;
	CHECK(SettlerRequests::Refusal(facts) == Why::kNoSuchPiece);
	facts = Fine();
	facts.pieceOfBench = false;  // a piece of another settlement, a friend's copy, the workbench itself
	CHECK(SettlerRequests::Refusal(facts) == Why::kNoSuchPiece);
	facts = Fine();
	facts.assignable = false;  // a chair, a wall
	CHECK(SettlerRequests::Refusal(facts) == Why::kNotAssignable);
}

TEST("settler requests: the console line names the workshop's own script, the settler and the piece")
{
	CHECK(SettlerRequests::AssignCommand(0x0001A4D7, 0xFF00123A) == "cqf WorkshopParent \"WorkshopParentScript.AssignActorToObjectPUBLIC\" 0001A4D7 FF00123A");
	CHECK(SettlerRequests::AssignCommand(0x14, 0x1) == "cqf WorkshopParent \"WorkshopParentScript.AssignActorToObjectPUBLIC\" 00000014 00000001");
}

TEST("settler requests: an applied answer teaches the workbench is the host's, and 'not the host's' that it isn't")
{
	const auto applied = SettlerRequests::Judge(Why::kApplied);
	CHECK(applied.benchOwned);
	CHECK(!applied.benchNotOwned);
	CHECK(std::string_view{ applied.notice } == "workshop.shared.assign_done");
	const auto notOwned = SettlerRequests::Judge(Why::kNotHostOwned);
	CHECK(notOwned.benchNotOwned);
	CHECK(!notOwned.benchOwned);
	CHECK(std::string_view{ notOwned.notice } == "workshop.shared.assign_not_owned");
}

TEST("settler requests: every refusal has its own words and teaches nothing about the workbench")
{
	const std::pair<Why, std::string_view> table[] = {
		{ Why::kBenchNotLoaded, "workshop.shared.assign_host_away" },
		{ Why::kHostAway, "workshop.shared.assign_host_away" },
		{ Why::kOff, "workshop.shared.assign_host_away" },
		{ Why::kNoAnswer, "workshop.shared.assign_no_answer" },
		{ Why::kNotAllowed, "workshop.shared.assign_off" },
		{ Why::kNoSuchSettler, "workshop.shared.assign_no_settler" },
		{ Why::kNoSuchPiece, "workshop.shared.assign_no_piece" },
		{ Why::kNotAssignable, "workshop.shared.assign_not_assignable" },
		{ Why::kFailed, "workshop.shared.assign_refused" },
		{ Why::kPieceBusy, "workshop.shared.assign_refused" },  // a reason of another request's: never sent for a settler
	};
	for (const auto& [why, notice] : table) {
		const auto verdict = SettlerRequests::Judge(why);
		CHECK(!verdict.benchOwned);
		CHECK(!verdict.benchNotOwned);
		REQUIRE(verdict.notice);
		CHECK(std::string_view{ verdict.notice } == notice);
	}
	// Every reason the protocol knows has words.
	for (std::uint8_t why = 0; why <= std::to_underlying(Protocol::WorkshopAnswer::LAST_WHY); ++why) {
		CHECK(SettlerRequests::Judge(static_cast<Why>(why)).notice != nullptr);
	}
}
