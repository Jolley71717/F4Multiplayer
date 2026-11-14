// Settlement attacks (common/AttackBook.h): the attacks known, the host's game's reports of its runs, a friend's game's
// view of the host's word and its verdicts on its own runs and its mirrors, the notices, and the server's book.

#include "Test.h"

#include "AttackBook.h"

#include <cctype>
#include <map>
#include <set>
#include <string>

using namespace std::chrono_literals;
using namespace AttackBook;

namespace
{
	constexpr std::uint32_t SANCTUARY = 0x000250FE;
	constexpr std::uint32_t RED_ROCKET = 0x000250FF;
	constexpr std::uint32_t RAIDERS = 0x001125E2;  // WorkshopRaiderAttack01
	constexpr std::uint32_t GUNNERS = 0x000C97A8;  // WorkshopGunnerAttack01
	constexpr std::uint32_t LOCATION = 0x0001F228;

	Seen Running(std::uint32_t a_quest, std::uint32_t a_workshop, bool a_begun = false, bool a_done = false)
	{
		return { a_quest, true, a_workshop, LOCATION, a_begun, a_done };
	}

	Seen Stopped(std::uint32_t a_quest, bool a_begun = false, bool a_done = false)
	{
		return { a_quest, false, 0, 0, a_begun, a_done };
	}

	Protocol::SettlementAttack HostAttack(std::uint32_t a_quest, std::uint32_t a_workshop, Phase a_phase, Outcome a_outcome = Outcome::kNone)
	{
		return { a_workshop, a_quest, LOCATION, a_phase, a_outcome };
	}
}

TEST("attack book: the attacks are the game's and the add-ons' random settlement attacks, each once, found by their file and ID there")
{
	std::set<std::pair<std::string_view, std::uint32_t>> ids;
	std::map<std::string_view, int>                      perFile;
	for (const auto& attack : ATTACKS) {
		CHECK(attack.quest != 0 && (attack.quest >> 24) == 0);
		CHECK(attack.file == FALLOUT4 || attack.file == FAR_HARBOR || attack.file == AUTOMATRON || attack.file == NUKA_WORLD);
		// Fallout4.esm's are Workshop*Attack*; an add-on's carry its prefix (DLC01, DLC03, DLC04) before the same.
		CHECK(attack.editorId.find("Workshop") != std::string_view::npos && attack.editorId.find("Attack") != std::string_view::npos);
		CHECK((attack.file == FALLOUT4) == attack.editorId.starts_with("Workshop"));
		ids.insert({ attack.file, attack.quest });
		++perFile[attack.file];
		const auto found = Find(attack.file, attack.quest);
		REQUIRE(found);
		CHECK(found->editorId == attack.editorId);
		// Whatever load index the game gives the file, the ID there is the same; the file's name is matched as the game does.
		CHECK(Find(attack.file, attack.quest | 0x01000000) == found);
		CHECK(Find(attack.file, attack.quest | 0x05000000) == found);
		std::string upper{ attack.file };
		for (auto& c : upper) {
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		CHECK(Find(upper, attack.quest) == found);
	}
	CHECK(ids.size() == ATTACKS.size());
	CHECK(perFile[FALLOUT4] == 13);
	CHECK(perFile[FAR_HARBOR] == 1);
	CHECK(perFile[AUTOMATRON] == 4);
	CHECK(perFile[NUKA_WORLD] == 6);
	CHECK(Find(FALLOUT4, RAIDERS)->editorId == "WorkshopRaiderAttack01");
	CHECK(Find(FAR_HARBOR, 0x0004B651)->editorId == "DLC03WorkshopTrapperAttack01");
	CHECK(Find(AUTOMATRON, 0x0000FD1B)->editorId == "DLC01WorkshopBotRaiderAttack02");
	// Nuka-World's Brotherhood attacks with a vertibird and its crew are in: a mirror never reaches the stages that enable them.
	CHECK(Find(NUKA_WORLD, 0x00035F1D)->editorId == "DLC04WorkshopBOSAttack02");
	CHECK(Find(NUKA_WORLD, 0x0003638B)->editorId == "DLC04WorkshopBOSAttack03");
	// An ID is its file's: the same number in another file is no attack.
	CHECK(!Find(AUTOMATRON, RAIDERS));
	CHECK(!Find(FALLOUT4, 0x0000FD1B));
	CHECK(!Find("DLCworkshop03.esm", 0x0004B651));
	// Preston's "Defend X" job is no random attack here (each player's own for now), nor is any other quest, nor the Creation
	// Club VR workshop's wave (no settlement, no WorkshopAttackScript).
	CHECK(!Find(FALLOUT4, 0x000A1412));  // MinRadiantOwned01
	CHECK(!Find(FALLOUT4, 0x000289E4));  // Followers
	CHECK(!Find(FALLOUT4, 0x0024A38D));  // VRWorkshopShared_AttackQuest
}

TEST("attack book: the host's game reports a run as it is rolled, begins, is defended and ends, each once")
{
	HostRuns runs;
	CHECK(runs.Look({ Stopped(RAIDERS) }).empty());

	auto reports = runs.Look({ Running(RAIDERS, SANCTUARY) });
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].workshop == SANCTUARY && reports[0].quest == RAIDERS && reports[0].location == LOCATION && reports[0].phase == Phase::kRolled);
	CHECK(runs.Look({ Running(RAIDERS, SANCTUARY) }).empty());

	reports = runs.Look({ Running(RAIDERS, SANCTUARY, true) });
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].phase == Phase::kUnderway);

	reports = runs.Look({ Running(RAIDERS, SANCTUARY, true, true) });
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].phase == Phase::kDefended);
	CHECK(runs.Live().size() == 1);

	reports = runs.Look({ Stopped(RAIDERS, true, true) });
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].phase == Phase::kOver && reports[0].outcome == Outcome::kDefended && reports[0].workshop == SANCTUARY);
	CHECK(runs.Look({ Stopped(RAIDERS, true, true) }).empty());
	CHECK(runs.Size() == 0);
}

TEST("attack book: a run's outcome is by its stages: defended, resolved undefended, or called off before it began")
{
	CHECK(OutcomeOf(true, true) == Outcome::kDefended);
	CHECK(OutcomeOf(false, true) == Outcome::kDefended);
	CHECK(OutcomeOf(true, false) == Outcome::kResolved);
	CHECK(OutcomeOf(false, false) == Outcome::kCalledOff);

	HostRuns runs;
	(void)runs.Look({ Running(RAIDERS, SANCTUARY, true) });
	auto reports = runs.Look({ Stopped(RAIDERS, true, false) });
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].outcome == Outcome::kResolved);

	(void)runs.Look({ Running(GUNNERS, SANCTUARY) });
	reports = runs.Look({ Stopped(GUNNERS) });
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].outcome == Outcome::kCalledOff);
}

TEST("attack book: a run seen first mid-attack is reported at its phase; one that moved settlement between looks ends and starts again")
{
	HostRuns runs;
	auto     reports = runs.Look({ Running(RAIDERS, SANCTUARY, true) });
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].phase == Phase::kUnderway);

	reports = runs.Look({ Running(RAIDERS, RED_ROCKET) });
	REQUIRE(reports.size() == 2);
	CHECK(reports[0].workshop == SANCTUARY && reports[0].phase == Phase::kOver && reports[0].outcome == Outcome::kResolved);
	CHECK(reports[1].workshop == RED_ROCKET && reports[1].phase == Phase::kRolled);

	// A run whose workshop alias isn't filled yet isn't reported until it is.
	HostRuns early;
	CHECK(early.Look({ Running(RAIDERS, 0) }).empty());
	CHECK(early.Look({ Running(RAIDERS, SANCTUARY) }).size() == 1);
}

TEST("attack book: a friend's game keeps its own run on a settlement the host doesn't own, and asks again each time while it runs")
{
	CHECK(Judge({ .word = Word::kUnknown }) == Verdict::kAsk);
	CHECK(Judge({ .word = Word::kUnknown, .askedLately = true }) == Verdict::kKeep);  // never stopped on a guess
	// "Not the host's" is no answer for good: the host may take the settlement later, so it is asked again each ASK_AGAIN.
	CHECK(Judge({ .word = Word::kNotHost }) == Verdict::kAsk);
	CHECK(Judge({ .word = Word::kNotHost, .askedLately = true }) == Verdict::kKeep);
	CHECK(Judge({ .word = Word::kNotHost, .begun = true, .loadedHere = true }) == Verdict::kAsk);
	CHECK(Judge({ .word = Word::kNotHost, .begun = true, .loadedHere = true, .askedLately = true }) == Verdict::kKeep);
	// The answer changed to the host's: defused at once.
	CHECK(Judge({ .word = Word::kHost, .askedLately = true }) == Verdict::kDefuse);
}

TEST("attack book: a friend's own run at the host's settlement is defused first, wherever their player is, then stopped")
{
	// Not defused yet: stage 100 first, so the quest's 1000 rolls no damage on the friend's save, there or away.
	for (const bool loaded : { false, true }) {
		for (const bool begun : { false, true }) {
			CHECK(Judge({ .word = Word::kHost, .begun = begun, .loadedHere = loaded }) == Verdict::kDefuse);
			CHECK(Judge({ .word = Word::kUnknown, .hostAttackHere = true, .begun = begun, .loadedHere = loaded }) == Verdict::kDefuse);
			CHECK(Judge({ .word = Word::kNotHost, .hostAttackHere = true, .begun = begun, .loadedHere = loaded }) == Verdict::kDefuse);
		}
	}
	// Defused: stopped, unless its own fight is on around the friend's player (stopped once they leave).
	CHECK(Judge({ .word = Word::kHost, .done = true }) == Verdict::kStop);
	CHECK(Judge({ .word = Word::kHost, .done = true, .loadedHere = true }) == Verdict::kStop);
	CHECK(Judge({ .word = Word::kHost, .begun = true, .done = true }) == Verdict::kStop);
	CHECK(Judge({ .word = Word::kHost, .begun = true, .done = true, .loadedHere = true }) == Verdict::kHold);
}

TEST("attack book: a mirror starts with the host's run, is defused as soon as it runs, and stops when the host's run ends")
{
	// Not running here: started, unless the host's run is over, ours runs elsewhere, or a start is under way.
	CHECK(Next({ .hostPhase = Phase::kRolled }) == MirrorStep::kStart);
	CHECK(Next({ .hostPhase = Phase::kUnderway }) == MirrorStep::kStart);  // a joiner's
	CHECK(Next({ .hostPhase = Phase::kOver }) == MirrorStep::kNone);
	CHECK(Next({ .hostPhase = Phase::kRolled, .elsewhere = true }) == MirrorStep::kNone);
	CHECK(Next({ .hostPhase = Phase::kRolled, .startTried = true }) == MirrorStep::kNone);

	// Running and not defused: stage 100 at once, whatever the host's run is at, so no save of ours holds a mirror that
	// rolls damage.
	for (const auto phase : { Phase::kRolled, Phase::kUnderway, Phase::kDefended, Phase::kOver }) {
		CHECK(Next({ .hostPhase = phase, .running = true }) == MirrorStep::kDefuse);
		CHECK(Next({ .hostPhase = phase, .running = true, .begun = true }) == MirrorStep::kDefuse);
		CHECK(Next({ .hostPhase = phase, .running = true, .hasObjective = true, .loadedHere = true, .hostAttackersHere = true }) == MirrorStep::kDefuse);
	}
	// Defused, it waits for the host while their run is rolled or defended.
	CHECK(Next({ .hostPhase = Phase::kRolled, .running = true, .done = true }) == MirrorStep::kNone);
	CHECK(Next({ .hostPhase = Phase::kDefended, .running = true, .done = true }) == MirrorStep::kNone);
	// Over: stopped (defused: its 1000 rolls nothing).
	CHECK(Next({ .hostPhase = Phase::kOver, .running = true, .done = true }) == MirrorStep::kStop);
	CHECK(Next({ .hostPhase = Phase::kOver, .running = true, .begun = true, .done = true, .loadedHere = true }) == MirrorStep::kStop);
}

TEST("attack book: a mirror shows \"Defend X\" only while our player and the host's attackers are at the settlement")
{
	const MirrorFacts on{ .hostPhase = Phase::kUnderway, .running = true, .done = true, .hasObjective = true, .loadedHere = true, .hostAttackersHere = true };
	CHECK(Next(on) == MirrorStep::kShowObjective);
	// Not with our player away, nor without the host's attackers there (the host's game hasn't them loaded), nor before
	// the attack begins, nor for a quest with no objective (the creature attacks).
	auto away = on;
	away.loadedHere = false;
	CHECK(Next(away) == MirrorStep::kNone);
	auto alone = on;
	alone.hostAttackersHere = false;
	CHECK(Next(alone) == MirrorStep::kNone);
	auto rolled = on;
	rolled.hostPhase = Phase::kRolled;
	CHECK(Next(rolled) == MirrorStep::kNone);
	auto creatures = on;
	creatures.hasObjective = false;
	CHECK(Next(creatures) == MirrorStep::kNone);

	// Shown: kept while they are there, hidden once either leaves.
	auto shown = on;
	shown.objectiveShown = true;
	CHECK(Next(shown) == MirrorStep::kNone);
	auto left = shown;
	left.loadedHere = false;
	CHECK(Next(left) == MirrorStep::kHideObjective);
	auto gone = shown;
	gone.hostAttackersHere = false;
	CHECK(Next(gone) == MirrorStep::kHideObjective);

	// The host's attackers died: completed. The host's run ended defended: completed, then stopped; undefended: hidden
	// (stage 1000 says "won" for an objective shown and not completed), then stopped.
	auto defended = shown;
	defended.hostPhase = Phase::kDefended;
	CHECK(Next(defended) == MirrorStep::kCompleteObjective);
	auto overWon = shown;
	overWon.hostPhase = Phase::kOver;
	overWon.outcome = Outcome::kDefended;
	CHECK(Next(overWon) == MirrorStep::kCompleteObjective);
	auto overLost = shown;
	overLost.hostPhase = Phase::kOver;
	overLost.outcome = Outcome::kResolved;
	CHECK(Next(overLost) == MirrorStep::kHideObjective);
	overLost.objectiveShown = false;
	CHECK(Next(overLost) == MirrorStep::kStop);
}

TEST("attack book: a run a session left defused is a leftover: stage 100 done and 20 not, running")
{
	CHECK(Leftover(Running(RAIDERS, SANCTUARY, false, true)));
	CHECK(!Leftover(Running(RAIDERS, SANCTUARY)));              // the game's own roll, waiting
	CHECK(!Leftover(Running(RAIDERS, SANCTUARY, true)));        // the game's own attack, on
	CHECK(!Leftover(Running(RAIDERS, SANCTUARY, true, true)));  // the game's own attack, defended
	CHECK(!Leftover(Stopped(RAIDERS, false, true)));
}

TEST("attack book: a run is closed down defused first, its timer cancelled, and stopped a look later")
{
	CHECK(NextClose({ .running = false }) == CloseStep::kNone);
	CHECK(NextClose({ .running = false, .done = true, .timerCancelled = true }) == CloseStep::kNone);
	CHECK(NextClose({ .running = true }) == CloseStep::kDefuse);
	CHECK(NextClose({ .running = true, .timerCancelled = true }) == CloseStep::kDefuse);  // never stopped undefended
	CHECK(NextClose({ .running = true, .done = true }) == CloseStep::kCancelTimer);
	CHECK(NextClose({ .running = true, .done = true, .timerCancelled = true }) == CloseStep::kStop);
}

TEST("attack book: a friend's view keeps the host's word and live attacks, takes their settlements as the host's, and forgets all when the host leaves")
{
	View view;
	const auto now = Clock::now();
	CHECK(view.WordOn(SANCTUARY, now) == Word::kUnknown);
	view.OnOwner({ RED_ROCKET, false }, now);
	CHECK(view.WordOn(RED_ROCKET, now) == Word::kNotHost);

	CHECK(!view.OnAttack(HostAttack(RAIDERS, SANCTUARY, Phase::kRolled), now));
	CHECK(view.WordOn(SANCTUARY, now) == Word::kHost);
	CHECK(view.LiveAt(SANCTUARY));
	CHECK(view.LiveOf(RAIDERS)->phase == Phase::kRolled);
	CHECK(view.OnAttack(HostAttack(RAIDERS, SANCTUARY, Phase::kUnderway), now) == Phase::kRolled);
	CHECK(view.LiveOf(RAIDERS)->phase == Phase::kUnderway);
	CHECK(view.OnAttack(HostAttack(RAIDERS, SANCTUARY, Phase::kOver, Outcome::kDefended), now) == Phase::kUnderway);
	CHECK(!view.LiveAt(SANCTUARY));
	CHECK(!view.LiveOf(RAIDERS));
	CHECK(view.WordOn(SANCTUARY, now) == Word::kHost);

	// The host's game said otherwise of a settlement it attacked: its word stands.
	view.OnOwner({ SANCTUARY, false }, now);
	CHECK(view.WordOn(SANCTUARY, now) == Word::kNotHost);

	(void)view.OnAttack(HostAttack(GUNNERS, RED_ROCKET, Phase::kUnderway), now);
	view.OnOwner({ 0, false }, now);
	CHECK(view.WordOn(SANCTUARY, now) == Word::kUnknown);
	CHECK(view.WordOn(RED_ROCKET, now) == Word::kUnknown);
	CHECK(view.LiveCount() == 0);
}

TEST("attack book: a friend's view forgets a live attack never told over after LIVE_FOR, and keeps no more than its caps")
{
	View       view;
	const auto now = Clock::now();
	(void)view.OnAttack(HostAttack(RAIDERS, SANCTUARY, Phase::kRolled), now);
	CHECK(view.Expire(now + View::LIVE_FOR - 1s).empty());
	const auto gone = view.Expire(now + View::LIVE_FOR);
	REQUIRE(gone.size() == 1);
	CHECK(gone[0] == RAIDERS);
	CHECK(view.LiveCount() == 0);

	for (std::uint32_t i = 0; i < View::MAX_OWNERS + 10; ++i) {
		view.OnOwner({ 0x00100000 + i, true }, now);
	}
	CHECK(view.Owners() == View::MAX_OWNERS);
	for (std::uint32_t i = 0; i < View::MAX_LIVE + 5; ++i) {
		(void)view.OnAttack(HostAttack(0x00200000 + i, SANCTUARY, Phase::kRolled), now);
	}
	CHECK(view.LiveCount() == View::MAX_LIVE);
}

TEST("attack book: a friend hears of the host's attack as it begins, and its end only if they heard it begin")
{
	CHECK(NoticeFor(Phase::kRolled, Outcome::kNone, false).empty());
	CHECK(NoticeFor(Phase::kUnderway, Outcome::kNone, false) == "attack.under");
	CHECK(NoticeFor(Phase::kDefended, Outcome::kNone, true) == "attack.defended");
	CHECK(NoticeFor(Phase::kOver, Outcome::kResolved, true) == "attack.lost");
	CHECK(NoticeFor(Phase::kOver, Outcome::kDefended, true).empty());  // told when it was defended
	CHECK(NoticeFor(Phase::kOver, Outcome::kCalledOff, true).empty());
	CHECK(NoticeFor(Phase::kOver, Outcome::kResolved, false).empty());
	CHECK(NoticeFor(Phase::kDefended, Outcome::kNone, false).empty());
}

TEST("attack book: the server's book keeps the host's word and live attacks, passes each phase once, and asks the host's game once in a while")
{
	Book       book;
	const auto now = Clock::now();
	CHECK(!book.OwnerOf(SANCTUARY, now));
	CHECK(book.SetOwner(SANCTUARY, true, now));
	CHECK(!book.SetOwner(SANCTUARY, true, now));
	CHECK(book.SetOwner(SANCTUARY, false, now));
	CHECK(book.OwnerOf(SANCTUARY, now) == false);

	CHECK(book.OnHostAttack(HostAttack(RAIDERS, SANCTUARY, Phase::kRolled), now));
	CHECK(!book.OnHostAttack(HostAttack(RAIDERS, SANCTUARY, Phase::kRolled), now));
	CHECK(book.OnHostAttack(HostAttack(RAIDERS, SANCTUARY, Phase::kUnderway), now));
	CHECK(book.Live().size() == 1);
	CHECK(book.OnHostAttack(HostAttack(RAIDERS, SANCTUARY, Phase::kOver, Outcome::kResolved), now));
	CHECK(book.Live().empty());

	CHECK(book.Ask(RED_ROCKET, now));
	CHECK(!book.Ask(RED_ROCKET, now + Book::ASK_EVERY - 1s));
	CHECK(book.Ask(RED_ROCKET, now + Book::ASK_EVERY));
	CHECK(book.Ask(SANCTUARY, now));
	CHECK(!book.Ask(SANCTUARY, now));
	book.Answered(SANCTUARY);  // the host's game spoke: the next question goes at once
	CHECK(book.Ask(SANCTUARY, now));

	(void)book.OnHostAttack(HostAttack(GUNNERS, RED_ROCKET, Phase::kRolled), now);
	CHECK(book.Expire(now + Book::LIVE_FOR - 1s) == 0);
	CHECK(book.Expire(now + Book::LIVE_FOR) == 1);

	for (std::uint32_t i = 0; i < Book::MAX_OWNERS + 10; ++i) {
		CHECK(book.SetOwner(0x00100000 + i, true, now));  // past the cap: told, not kept
	}
	CHECK(book.OwnersKept() == Book::MAX_OWNERS);
	book.Clear();
	CHECK(book.OwnersKept() == 0);
	CHECK(book.Owners(now).empty());
}

TEST("attack book: the server's word on a settlement expires after OWNER_FOR; said again, it is news again")
{
	Book       book;
	const auto now = Clock::now();
	CHECK(book.SetOwner(SANCTUARY, false, now));
	CHECK(book.OwnerOf(SANCTUARY, now + OWNER_FOR - 1s) == false);
	CHECK(!book.OwnerOf(SANCTUARY, now + OWNER_FOR));
	CHECK(book.Owners(now + OWNER_FOR - 1s).size() == 1);
	CHECK(book.Owners(now + OWNER_FOR).empty());  // a joiner hears no stale word
	CHECK(book.SetOwner(SANCTUARY, false, now + OWNER_FOR));  // the same word after it expired: told again
	CHECK(book.OwnerOf(SANCTUARY, now + OWNER_FOR) == false);

	// Expire drops the words gone stale as well as the live attacks.
	CHECK(book.SetOwner(RED_ROCKET, true, now));
	(void)book.Expire(now + OWNER_FOR + 1s);
	CHECK(book.OwnersKept() == 1);  // Sanctuary's, said again later
	(void)book.Expire(now + 2 * OWNER_FOR + 1s);
	CHECK(book.OwnersKept() == 0);

	// Past the cap, stale words make room.
	for (std::uint32_t i = 0; i < Book::MAX_OWNERS; ++i) {
		(void)book.SetOwner(0x00100000 + i, true, now);
	}
	CHECK(book.OwnersKept() == Book::MAX_OWNERS);
	CHECK(book.SetOwner(SANCTUARY, true, now + OWNER_FOR));
	CHECK(book.OwnerOf(SANCTUARY, now + OWNER_FOR) == true);
	CHECK(book.OwnersKept() == 1);
}

TEST("attack book: the friends waiting for the host's word on a settlement are kept once each until it comes")
{
	Book book;
	CHECK(book.TakeAwaiting(SANCTUARY).empty());
	book.Awaits(SANCTUARY, 2);
	book.Awaits(SANCTUARY, 3);
	book.Awaits(SANCTUARY, 2);  // asked twice: told once
	book.Awaits(RED_ROCKET, 2);
	const auto waiting = book.TakeAwaiting(SANCTUARY);
	REQUIRE(waiting.size() == 2);
	CHECK(waiting[0] == 2);
	CHECK(waiting[1] == 3);
	CHECK(book.TakeAwaiting(SANCTUARY).empty());
	CHECK(book.TakeAwaiting(RED_ROCKET).size() == 1);

	for (std::uint32_t i = 0; i < Book::MAX_ASKERS + 4; ++i) {
		book.Awaits(SANCTUARY, 100 + i);
	}
	CHECK(book.TakeAwaiting(SANCTUARY).size() == Book::MAX_ASKERS);
	book.Awaits(SANCTUARY, 2);
	book.Clear();
	CHECK(book.TakeAwaiting(SANCTUARY).empty());
}

TEST("attack book: a friend's view of the host's word expires after OWNER_FOR, and the host's attack refreshes a stale one")
{
	View       view;
	const auto now = Clock::now();
	view.OnOwner({ SANCTUARY, false }, now);
	CHECK(view.WordOn(SANCTUARY, now + OWNER_FOR - 1s) == Word::kNotHost);
	CHECK(view.WordOn(SANCTUARY, now + OWNER_FOR) == Word::kUnknown);
	// Said again (an answer, or the host's game telling a change): trusted again from then.
	view.OnOwner({ SANCTUARY, true }, now + OWNER_FOR);
	CHECK(view.WordOn(SANCTUARY, now + OWNER_FOR) == Word::kHost);
	CHECK(view.WordOn(SANCTUARY, now + 2 * OWNER_FOR) == Word::kUnknown);
	// The host's attack there makes a stale word the host's; it doesn't override a fresh one.
	(void)view.OnAttack(HostAttack(RAIDERS, SANCTUARY, Phase::kRolled), now + 2 * OWNER_FOR);
	CHECK(view.WordOn(SANCTUARY, now + 2 * OWNER_FOR) == Word::kHost);
	view.OnOwner({ RED_ROCKET, false }, now);
	(void)view.OnAttack(HostAttack(GUNNERS, RED_ROCKET, Phase::kRolled), now);
	CHECK(view.WordOn(RED_ROCKET, now) == Word::kNotHost);
}

TEST("attack book: the host's game tells a change of owner of a settlement it spoke of, once, and leaves alone what it can't read")
{
	HostWords  words;
	const auto now = Clock::now();
	CHECK(!words.Due(now));  // nothing said: nothing to read
	words.Said(SANCTUARY, false);
	words.Said(RED_ROCKET, true);
	words.Said(0, true);  // no settlement: not kept
	CHECK(words.Size() == 2);
	CHECK(words.Due(now));

	bool       sanctuaryOwned = false;
	const auto read = [&](std::uint32_t a_workshop) -> std::optional<bool> {
		if (a_workshop == SANCTUARY) {
			return sanctuaryOwned;
		}
		return std::nullopt;  // Red Rocket's workbench can't be read now: left as it was
	};
	CHECK(words.Recheck(now, read).empty());
	CHECK(!words.Due(now + HostWords::CHECK_EVERY - 1s));
	CHECK(words.Due(now + HostWords::CHECK_EVERY));

	// The host takes Sanctuary (the Minutemen): told once.
	sanctuaryOwned = true;
	auto changes = words.Recheck(now + HostWords::CHECK_EVERY, read);
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].workshop == SANCTUARY && changes[0].hostOwned);
	CHECK(words.Recheck(now + 2 * HostWords::CHECK_EVERY, read).empty());

	// Said again as an answer: the next change is from that.
	words.Said(SANCTUARY, false);
	changes = words.Recheck(now + 3 * HostWords::CHECK_EVERY, read);
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].hostOwned);

	for (std::uint32_t i = 0; i < HostWords::MAX + 10; ++i) {
		words.Said(0x00100000 + i, true);
	}
	CHECK(words.Size() == HostWords::MAX);
	words.Reset();
	CHECK(words.Size() == 0);
	CHECK(!words.Due(now));
}
