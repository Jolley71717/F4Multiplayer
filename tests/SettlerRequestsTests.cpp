// A friend's job for a settler at the host's settlement is a request the host's game carries out
// (game/SettlerRequests.h): what the friend's game does with the command, what the host's game checks, and what the
// friend's game makes of the answer.

#include "Test.h"

#include "game/SettlerRequests.h"

#include <array>
#include <chrono>
#include <optional>
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

// The give-command's hook runs on the UI's thread in workshop mode: it reads only the main thread's view, which knows the
// workbench it was made at and the runtime NPCs it names.
TEST("settler requests: the hooks' view knows the workbench it was made at and the NPCs it names, and nothing else")
{
	using Npc = SettlerRequests::HookView::Npc;
	constexpr std::uint32_t PLUGIN = 0x0001CA7D;   // a plugin's NPC (Codsworth)
	constexpr std::uint32_t OURS = 0xFF000A01;     // a runtime NPC of our game's the session knows
	constexpr std::uint32_t THEIRS = 0xFF000A02;   // our copy of another player's runtime NPC
	constexpr std::uint32_t UNNAMED = 0xFF000A03;  // a runtime NPC the view doesn't name

	SettlerRequests::HookView view;
	CHECK(!view.HandsOver());
	CHECK(view.HostRuns(0) == std::optional<bool>{ false });
	CHECK(!view.HostRuns(BENCH).has_value());  // made at no workbench: unknown there
	view.Set(true, BENCH, true, { Npc{ THEIRS, true, true }, Npc{ OURS, true, false } });
	CHECK(view.HandsOver());
	CHECK(view.HostRuns(BENCH) == std::optional<bool>{ true });
	CHECK(view.HostRuns(0) == std::optional<bool>{ false });
	CHECK(!view.HostRuns(BENCH + 1).has_value());  // another workbench than the one it was made at

	// a plugin's reference by its id alone: an actor is the session's, never a copy; anything else is nobody's
	auto npc = view.Settler(PLUGIN, true);
	REQUIRE(npc.has_value());
	CHECK(npc->shared);
	CHECK(!npc->copy);
	npc = view.Settler(PLUGIN, false);
	REQUIRE(npc.has_value());
	CHECK(!npc->shared);
	CHECK(!npc->copy);
	// a runtime one as the view names it (sorted whatever the order it was given in), else unknown
	npc = view.Settler(OURS, true);
	REQUIRE(npc.has_value());
	CHECK(npc->shared);
	CHECK(!npc->copy);
	npc = view.Settler(THEIRS, true);
	REQUIRE(npc.has_value());
	CHECK(npc->copy);
	CHECK(!view.Settler(UNNAMED, true).has_value());
	CHECK(!view.Settler(OURS, false).has_value());  // (a runtime reference that isn't an actor: the main thread decides)

	// made at no workbench (workshop mode closed), and cleared (a load, leaving): nothing runs, nothing named
	view.Set(true, 0, true, { Npc{ OURS, true, false } });
	CHECK(view.HostRuns(0) == std::optional<bool>{ false });
	CHECK(!view.HostRuns(BENCH).has_value());
	view.Clear();
	CHECK(!view.HandsOver());
	CHECK(!view.Settler(OURS, true).has_value());
	CHECK(!view.HostRuns(BENCH).has_value());
}

TEST("settler requests: the hook passes a command on at once only when every fact it can't know gives the game's own path")
{
	// Every command the rules tell apart, with each fact known and unknown: passed on at once exactly when every value the
	// unknown facts (and pending, never known there) could take gives kEngine.
	const std::array<Target, 4>        targets{ Target::kHostsPiece, Target::kCopy, Target::kPlaced, Target::kOwn };
	const std::array<std::uint32_t, 2> commands{ SettlerRequests::COMMAND_ASSIGN, SettlerRequests::COMMAND_ASSIGN + 1 };
	const std::array<std::uint32_t, 2> benches{ 0, BENCH };
	int                                checked = 0;
	for (const auto command : commands) {
		for (const auto bench : benches) {
			for (const auto target : targets) {
				for (unsigned bits = 0; bits < 64; ++bits) {
					Given given;
					given.command = command;
					given.bench = bench;
					given.target = target;
					given.hasTarget = (bits & 1) != 0;
					given.handsOver = (bits & 2) != 0;
					given.hostRuns = (bits & 4) != 0;
					given.settlerShared = (bits & 8) != 0;
					given.settlerCopy = (bits & 16) != 0;
					given.editAllowed = (bits & 32) != 0;
					const auto engineWith = [&](bool a_runs, bool a_shared, bool a_copy, bool a_pending) {
						auto g = given;
						g.hostRuns = a_runs;
						g.settlerShared = a_shared;
						g.settlerCopy = a_copy;
						g.pending = a_pending;
						return SettlerRequests::OnGive(g).give == Give::kEngine;
					};
					for (unsigned known = 0; known < 4; ++known) {
						const bool runsKnown = (known & 1) != 0;
						const bool settlerKnown = (known & 2) != 0;
						bool       all = true;
						for (unsigned v = 0; v < 16; ++v) {
							const bool runs = runsKnown ? given.hostRuns : (v & 1) != 0;
							const bool shared = settlerKnown ? given.settlerShared : (v & 2) != 0;
							const bool copy = settlerKnown ? given.settlerCopy : (v & 4) != 0;
							all = all && engineWith(runs, shared, copy, (v & 8) != 0);
						}
						CHECK(SettlerRequests::PassesNow(given, runsKnown, settlerKnown) == all);
						++checked;
					}
					// all known: the game's own path exactly when OnGive says so, whatever waits and whatever the host allows
					auto pending = given;
					pending.pending = true;
					CHECK(SettlerRequests::PassesNow(given, true, true) == (SettlerRequests::OnGive(given).give == Give::kEngine));
					CHECK(SettlerRequests::PassesNow(given, true, true) == (SettlerRequests::OnGive(pending).give == Give::kEngine));
				}
			}
		}
	}
	CHECK(checked == 2 * 2 * 4 * 64 * 4);
}

TEST("settler requests: off the main thread the host's settler is always kept from the engine, and our own plain cases go on")
{
	// the host's settler at the host's piece: kept, whatever the hook can't tell
	CHECK(!SettlerRequests::PassesNow(Asked(), true, true));
	CHECK(!SettlerRequests::PassesNow(Asked(), false, false));
	// a settlement the view doesn't know as the host's (a frame old, another workbench): a copy target is never passed on
	auto given = Asked();
	given.target = Target::kCopy;
	CHECK(!SettlerRequests::PassesNow(given, false, true));
	// our own plugin settler at our own settlement, our own piece: the game's own at once
	given = Asked();
	given.hostRuns = false;
	given.settlerShared = true;
	given.target = Target::kOwn;
	CHECK(SettlerRequests::PassesNow(given, true, true));
	// ...but a runtime settler the view doesn't name there is kept (it may be our copy of another player's NPC): the main
	// thread decides, and sends the engine's event itself if it is ours
	CHECK(!SettlerRequests::PassesNow(given, true, false));
	// at a settlement the view doesn't know, our own settler and piece are kept too (the host's game may run it)
	CHECK(!SettlerRequests::PassesNow(given, false, true));
	// the rule not in force, outside workshop mode, or another command: always on at once, nothing known needed
	given = Asked();
	given.handsOver = false;
	CHECK(SettlerRequests::PassesNow(given, false, false));
	given = Asked();
	given.bench = 0;
	CHECK(SettlerRequests::PassesNow(given, false, false));
	given = Asked();
	given.command = SettlerRequests::COMMAND_ASSIGN + 1;
	CHECK(SettlerRequests::PassesNow(given, false, false));
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
