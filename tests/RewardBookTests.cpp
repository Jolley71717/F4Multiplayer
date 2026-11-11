// Shared quest rewards, kept free of the game (common/RewardBook.h): the turn-in's game claims what its player got from a
// reward line or a turn-in stage, the server keeps each reward for its run and sends it once to every other player, and a
// friend's game gives it once, unless its player got the same in their own game.

#include "Test.h"

#include "RewardBook.h"
#include "game/RewardLines.h"
#include "game/StageRules.h"

#include <algorithm>
#include <string_view>

using namespace std::chrono_literals;
using RewardBook::Capture;
using RewardBook::Claimed;
using RewardBook::Kind;
using RewardBook::Ledger;
using RewardBook::Receiver;
using RewardBook::Verdict;
using RewardBook::Want;

namespace
{
	constexpr std::uint32_t CAPS = 0x0000000F;
	constexpr std::uint32_t STIMPAK = 0x00023736;
	constexpr std::uint32_t JET = 0x00033778;
	constexpr std::uint32_t QUEST = 0x000A8258;
	constexpr std::uint32_t OTHER_QUEST = 0x0001F6A5;
	constexpr std::uint32_t INFO = 0x000D60BB;
	constexpr std::uint32_t OTHER_INFO = 0x000D60BC;

	const auto T0 = RewardBook::Clock::time_point{} + 1h;

	std::int32_t CountOf(const Claimed& a_claimed, std::uint32_t a_form)
	{
		for (const auto& item : a_claimed.items) {
			if (item.form == a_form) {
				return item.count;
			}
		}
		return 0;
	}

	// Polls a_capture every 100 ms from a_from to a_until, with what closed in between.
	std::vector<Claimed> PollUntil(Capture& a_capture, RewardBook::Clock::time_point a_from, RewardBook::Clock::time_point a_until)
	{
		std::vector<Claimed> out;
		for (auto now = a_from; now <= a_until; now += 100ms) {
			for (auto& claimed : a_capture.Poll(now)) {
				out.push_back(std::move(claimed));
			}
		}
		return out;
	}

	Protocol::QuestReward Reward(std::uint32_t a_id, Kind a_kind, std::uint32_t a_quest, std::uint32_t a_source, std::uint32_t a_run = 9)
	{
		return Protocol::QuestReward{ a_id, 2, a_kind, a_quest, a_source, 0, { { CAPS, 100 } }, a_run };
	}
}

TEST("reward capture: a line claims its own items, up to their counts, and not what came with it")
{
	Capture    capture;
	const Want wants[] = { { CAPS, 500 } };
	// The line's script gives as it begins, a moment before the poll sees it said; a pickup's stimpak comes with it.
	capture.Gain(CAPS, 500, T0);
	capture.Gain(STIMPAK, 1, T0 + 100ms);
	capture.Line(QUEST, INFO, wants, T0 + 200ms);
	capture.Line(QUEST, INFO, wants, T0 + 2s);  // still said
	CHECK(capture.Open() == 1);
	// Not closed while it settles.
	CHECK(capture.Poll(T0 + 4s).empty());
	const auto claimed = PollUntil(capture, T0 + 4s, T0 + 6s);
	REQUIRE(claimed.size() == 1);
	CHECK(claimed[0].kind == Kind::kLine);
	CHECK(claimed[0].quest == QUEST);
	CHECK(claimed[0].source == INFO);
	CHECK(claimed[0].xp == 0);
	CHECK(claimed[0].items.size() == 1);
	CHECK(CountOf(claimed[0], CAPS) == 500);
	CHECK(capture.Open() == 0);
}

TEST("reward capture: a line takes no more than its count; a gain it can't have is left for others")
{
	Capture    capture;
	const Want wants[] = { { CAPS, 100 }, { JET, 2 } };
	capture.Gain(CAPS, 250, T0);  // a caps stack bigger than the line's: the line has 100 of it
	capture.Gain(JET, 1, T0);
	capture.Line(QUEST, INFO, wants, T0 + 100ms);
	const auto claimed = PollUntil(capture, T0, T0 + 5s);
	REQUIRE(claimed.size() == 1);
	CHECK(CountOf(claimed[0], CAPS) == 100);
	CHECK(CountOf(claimed[0], JET) == 1);  // gave one only (the script's other branch): that one
}

TEST("reward capture: an 'any' want (a leveled list's caps) takes one gain whole")
{
	Capture    capture;
	const Want wants[] = { { CAPS, 0 } };
	capture.Gain(CAPS, 37, T0);
	capture.Gain(CAPS, 12, T0 + 50ms);
	capture.Line(QUEST, INFO, wants, T0 + 100ms);
	const auto claimed = PollUntil(capture, T0, T0 + 5s);
	REQUIRE(claimed.size() == 1);
	CHECK(CountOf(claimed[0], CAPS) == 37);
}

TEST("reward capture: a line that gave nothing (another branch) reports nothing; gains outside its window are not its")
{
	Capture    capture;
	const Want wants[] = { { CAPS, 100 } };
	capture.Gain(CAPS, 100, T0);
	capture.Line(QUEST, INFO, wants, T0 + 5s);  // seen long after the caps: not this line's
	CHECK(PollUntil(capture, T0, T0 + 10s).empty());
	CHECK(capture.Open() == 0);

	capture.Line(QUEST, OTHER_INFO, wants, T0 + 20s);
	capture.Gain(CAPS, 100, T0 + 20s + 4s);  // after the line's settle
	CHECK(PollUntil(capture, T0 + 20s, T0 + 30s).empty());
}

TEST("reward capture: a stage that completes its quest claims what came in its window and the player's own XP gains")
{
	Capture capture;
	capture.Gain(CAPS, 50, T0);
	capture.Xp(150.0f, T0 + 200ms);
	capture.Stage(QUEST, 30, true, T0 + 500ms);
	capture.Gain(STIMPAK, 2, T0 + 1s);
	capture.Xp(0.4f, T0 + 1s);
	const auto claimed = PollUntil(capture, T0, T0 + 5s);
	REQUIRE(claimed.size() == 1);
	CHECK(claimed[0].kind == Kind::kStage);
	CHECK(claimed[0].quest == QUEST);
	CHECK(claimed[0].source == 30);
	CHECK(claimed[0].xp == 150);  // 150.4, rounded
	CHECK(CountOf(claimed[0], CAPS) == 50);
	CHECK(CountOf(claimed[0], STIMPAK) == 2);
}

TEST("reward capture: a stage that gave nothing reports nothing; one that gave XP only reports it")
{
	Capture capture;
	capture.Stage(QUEST, 30, true, T0);
	CHECK(PollUntil(capture, T0, T0 + 5s).empty());

	capture.Stage(QUEST, 40, true, T0 + 10s);
	capture.Xp(300.0f, T0 + 10s + 200ms);
	const auto claimed = PollUntil(capture, T0 + 10s, T0 + 15s);
	REQUIRE(claimed.size() == 1);
	CHECK(claimed[0].items.empty());
	CHECK(claimed[0].xp == 300);
}

TEST("reward capture: a line and the stage it sets share a window: the line's items are the line's, the rest the stage's")
{
	Capture    capture;
	const Want wants[] = { { CAPS, 500 } };
	capture.Gain(CAPS, 500, T0);
	capture.Gain(STIMPAK, 3, T0 + 100ms);
	capture.Xp(200.0f, T0 + 100ms);
	capture.Line(QUEST, INFO, wants, T0 + 200ms);
	capture.Stage(QUEST, 400, true, T0 + 300ms);
	capture.Line(QUEST, INFO, wants, T0 + 4s);  // the line is still being said after the stage's settle: the stage waits
	const auto claimed = PollUntil(capture, T0, T0 + 10s);
	REQUIRE(claimed.size() == 2);
	CHECK(claimed[0].kind == Kind::kLine);
	CHECK(CountOf(claimed[0], CAPS) == 500);
	CHECK(CountOf(claimed[0], STIMPAK) == 0);
	CHECK(claimed[0].xp == 0);
	CHECK(claimed[1].kind == Kind::kStage);
	CHECK(CountOf(claimed[1], CAPS) == 0);
	CHECK(CountOf(claimed[1], STIMPAK) == 3);
	CHECK(claimed[1].xp == 200);
}

TEST("reward capture: an item made in this game (a runtime form) is never claimed; a gain is claimed once")
{
	Capture capture;
	capture.Gain(0xFF001234, 1, T0);
	capture.Gain(0, 1, T0);
	capture.Gain(CAPS, 0, T0);
	CHECK(capture.Kept() == 0);

	capture.Gain(CAPS, 100, T0);
	capture.Stage(QUEST, 30, true, T0 + 100ms);
	capture.Stage(OTHER_QUEST, 10, true, T0 + 200ms);
	const auto claimed = PollUntil(capture, T0, T0 + 5s);
	REQUIRE(claimed.size() == 1);  // the second stage found the caps taken
	CHECK(claimed[0].quest == QUEST);
}

TEST("reward capture: gains no moment claims are forgotten; Clear forgets everything")
{
	Capture capture;
	capture.Gain(CAPS, 10, T0);
	capture.Xp(5.0f, T0);
	CHECK(capture.Kept() == 2);
	capture.Poll(T0 + RewardBook::GAIN_KEPT + 1s);
	CHECK(capture.Kept() == 0);

	const Want wants[] = { { CAPS, 10 } };
	capture.Gain(CAPS, 10, T0);
	capture.Line(QUEST, INFO, wants, T0);
	capture.Clear();
	CHECK(capture.Open() == 0);
	CHECK(capture.Kept() == 0);
}

TEST("reward ledger: a reward is numbered and kept; the same from another player soon after is refused, a repeat too")
{
	Ledger        ledger;
	const Claimed reward{ Kind::kLine, QUEST, INFO, 0, { { CAPS, 500 } } };
	const auto [first, none] = ledger.Add(1, 0x11, reward, T0);
	REQUIRE(first);
	CHECK(none == Ledger::Refusal::kNone);
	CHECK(first->id == 1);
	CHECK(first->playerId == 1);
	CHECK(first->reward == reward);

	const auto [twin, sameTurnIn] = ledger.Add(2, 0x22, reward, T0 + 10s);
	CHECK(!twin);
	CHECK(sameTurnIn == Ledger::Refusal::kSameTurnIn);
	const auto [again, repeat] = ledger.Add(1, 0x11, reward, T0 + 20s);
	CHECK(!again);
	CHECK(repeat == Ledger::Refusal::kRepeat);

	// Later, the same line is another turn-in (a repeatable quest's).
	const auto [later, laterRefusal] = ledger.Add(2, 0x22, reward, T0 + RewardBook::SAME_TURN_IN + 1s);
	REQUIRE(later);
	CHECK(laterRefusal == Ledger::Refusal::kNone);
	CHECK(later->id == 2);
	// Another line at once is another reward.
	CHECK(ledger.Add(2, 0x22, Claimed{ Kind::kLine, QUEST, OTHER_INFO, 0, { { CAPS, 5 } } }, T0 + 1s).first);
	CHECK(ledger.Size() == 3);
}

TEST("reward ledger: each player's game is owed a reward once, the giver's never")
{
	Ledger ledger;
	const auto* entry = ledger.Add(1, 0x11, Claimed{ Kind::kStage, QUEST, 30, 100, {} }, T0).first;
	REQUIRE(entry);
	const auto id = entry->id;
	CHECK(ledger.Owed(0x11).empty());  // the giver has it
	ledger.Sent(id, 0x22);
	CHECK(ledger.Owed(0x22).empty());  // sent live
	const auto owed = ledger.Owed(0x33);  // joined later
	REQUIRE(owed.size() == 1);
	CHECK(owed[0]->id == id);
	CHECK(ledger.Owed(0x33).empty());  // once
	// A player with no identity is known by their id.
	CHECK(RewardBook::Who(0, 4) != RewardBook::Who(4, 0));
	CHECK(RewardBook::Who(0x44, 4) == 0x44);
	CHECK(ledger.Owed(RewardBook::Who(0, 4)).size() == 1);
	ledger.Clear();
	CHECK(ledger.Size() == 0);
	CHECK(ledger.Owed(0x55).empty());
}

TEST("reward ledger: the oldest go past MAX_KEPT")
{
	Ledger ledger;
	for (std::uint32_t i = 0; i < RewardBook::MAX_KEPT + 10; ++i) {
		REQUIRE(ledger.Add(1, 0x11, Claimed{ Kind::kStage, QUEST, i, 10, {} }, T0).first);
	}
	CHECK(ledger.Size() == RewardBook::MAX_KEPT);
	const auto owed = ledger.Owed(0x22);
	REQUIRE(owed.size() == RewardBook::MAX_KEPT);
	CHECK(owed.front()->reward.source == 10);
}

TEST("reward receiver: a friend's reward is given once")
{
	Receiver receiver;
	const auto reward = Reward(1, Kind::kLine, QUEST, INFO);
	CHECK(receiver.Judge(reward, false, false, T0) == Verdict::kGive);
	CHECK(receiver.Judge(reward, false, false, T0 + 1s) == Verdict::kHad);
	CHECK(receiver.Judge(Reward(2, Kind::kLine, QUEST, INFO), false, false, T0 + 2s) == Verdict::kGive);
}

TEST("reward receiver: not given when our player turned the same in at the same time")
{
	Receiver receiver;
	receiver.Reported(Claimed{ Kind::kLine, QUEST, INFO, 0, { { CAPS, 500 } } }, T0);
	CHECK(receiver.Judge(Reward(1, Kind::kLine, QUEST, INFO), false, false, T0 + 5s) == Verdict::kOwnTurnIn);
	CHECK(receiver.Judge(Reward(2, Kind::kLine, QUEST, OTHER_INFO), false, false, T0 + 5s) == Verdict::kGive);
	CHECK(receiver.Judge(Reward(3, Kind::kStage, QUEST, INFO), false, false, T0 + 5s) == Verdict::kGive);
	CHECK(receiver.Judge(Reward(4, Kind::kLine, QUEST, INFO), false, false, T0 + RewardBook::SAME_TURN_IN + 1s) == Verdict::kGive);
}

TEST("reward receiver: a stage done here gave its own; a line of a quest finished on our own did too")
{
	Receiver receiver;
	CHECK(receiver.Judge(Reward(1, Kind::kStage, QUEST, 30), true, false, T0) == Verdict::kStageDone);
	CHECK(receiver.Judge(Reward(2, Kind::kStage, QUEST, 30), false, true, T0) == Verdict::kGive);  // a stage's goes by its stage
	CHECK(receiver.Judge(Reward(3, Kind::kLine, QUEST, INFO), false, true, T0) == Verdict::kFinished);
	CHECK(receiver.Judge(Reward(4, Kind::kLine, QUEST, INFO), true, false, T0) == Verdict::kGive);  // a line's by its quest
	// Judged once whatever the verdict.
	CHECK(receiver.Judge(Reward(1, Kind::kStage, QUEST, 30), false, false, T0) == Verdict::kHad);
	receiver.Clear();
	CHECK(receiver.Judge(Reward(1, Kind::kStage, QUEST, 30), false, false, T0) == Verdict::kGive);
	for (const auto verdict : { Verdict::kGive, Verdict::kHad, Verdict::kOwnTurnIn, Verdict::kStageDone, Verdict::kFinished }) {
		CHECK(std::string_view{ RewardBook::Why(verdict) }.size() > 0);
	}
}

TEST("reward capture: a turn-in stage that doesn't complete its quest claims its items, not the XP of the stages it sets")
{
	// MinRecruit02 90 (the ransom paid) sets 400, which sets 450, which completes the quest: the quest XP lands in 90's
	// window, but 400 and 450 are set in the friend's game too and give it there.
	Capture capture;
	capture.Gain(CAPS, 120, T0 + 200ms);
	capture.Stage(QUEST, 90, false, T0);
	capture.Xp(450.0f, T0 + 300ms);
	const auto claimed = PollUntil(capture, T0, T0 + 5s);
	REQUIRE(claimed.size() == 1);
	CHECK(claimed[0].source == 90);
	CHECK(CountOf(claimed[0], CAPS) == 120);
	CHECK(claimed[0].xp == 0);

	// One that gives XP only, not completing: nothing to report.
	capture.Stage(QUEST, 91, false, T0 + 10s);
	capture.Xp(300.0f, T0 + 10s + 200ms);
	CHECK(PollUntil(capture, T0 + 10s, T0 + 15s).empty());
}

TEST("reward capture: a line or stage is open while our own game is claiming it")
{
	Capture    capture;
	const Want wants[] = { { CAPS, 500 } };
	capture.Line(QUEST, INFO, wants, T0);
	capture.Stage(QUEST, 30, true, T0);
	CHECK(capture.IsOpen(Kind::kLine, QUEST, INFO));
	CHECK(capture.IsOpen(Kind::kStage, QUEST, 30));
	CHECK(!capture.IsOpen(Kind::kLine, QUEST, OTHER_INFO));
	CHECK(!capture.IsOpen(Kind::kStage, QUEST, INFO));
	CHECK(!capture.IsOpen(Kind::kLine, OTHER_QUEST, INFO));
	PollUntil(capture, T0, T0 + 5s);
	CHECK(!capture.IsOpen(Kind::kLine, QUEST, INFO));
	CHECK(!capture.IsOpen(Kind::kStage, QUEST, 30));
}

TEST("reward receiver: two players hearing the same line a moment apart: the friend's waits for our claim, and is then a twin")
{
	// A's line closed and its reward reached B while B's own line was still settling. B's game holds it (IsOpen) until its
	// own claim closes and is reported; then it is B's own turn-in, not given again.
	Capture    capture;
	Receiver   receiver;
	const Want wants[] = { { CAPS, 500 } };
	capture.Gain(CAPS, 500, T0);
	capture.Line(QUEST, INFO, wants, T0 + 100ms);
	const auto friends = Reward(1, Kind::kLine, QUEST, INFO);
	CHECK(capture.IsOpen(friends.kind, friends.quest, friends.source));  // held
	const auto claimed = PollUntil(capture, T0, T0 + 5s);
	REQUIRE(claimed.size() == 1);
	receiver.Reported(claimed[0], T0 + 5s);
	CHECK(!capture.IsOpen(friends.kind, friends.quest, friends.source));
	CHECK(receiver.Judge(friends, false, false, T0 + 5s) == Verdict::kOwnTurnIn);
}

TEST("reward ledger: each run of the server has its own id, never 0")
{
	CHECK(Ledger{ 0x1234 }.Run() == 0x1234);
	CHECK(Ledger{ 0 }.Run() != 0);
	const Ledger first;
	const Ledger second;
	CHECK(first.Run() != 0);
	CHECK(second.Run() != 0);
	CHECK(RewardBook::NewRun() != 0);
}

TEST("reward receiver: a server restarted at the same address (another run): its ids are new, and given")
{
	// A Steam host's address doesn't change when they host again: the old run's reward 1 must not hide the new run's.
	Receiver receiver;
	CHECK(receiver.Judge(Reward(1, Kind::kLine, QUEST, INFO, 0xA), false, false, T0) == Verdict::kGive);
	CHECK(receiver.Judge(Reward(2, Kind::kLine, QUEST, OTHER_INFO, 0xA), false, false, T0) == Verdict::kGive);
	CHECK(receiver.Judge(Reward(1, Kind::kLine, QUEST, INFO, 0xA), false, false, T0 + 1s) == Verdict::kHad);
	// The new run numbers from 1 again.
	CHECK(receiver.Judge(Reward(1, Kind::kStage, OTHER_QUEST, 30, 0xB), false, false, T0 + 2s) == Verdict::kGive);
	CHECK(receiver.Judge(Reward(2, Kind::kStage, OTHER_QUEST, 40, 0xB), false, false, T0 + 2s) == Verdict::kGive);
	CHECK(receiver.Judge(Reward(1, Kind::kStage, OTHER_QUEST, 30, 0xB), false, false, T0 + 3s) == Verdict::kHad);
	// The same run sent again after a reconnect: had.
	CHECK(receiver.Judge(Reward(2, Kind::kStage, OTHER_QUEST, 40, 0xB), false, false, T0 + 4s) == Verdict::kHad);
}

TEST("reward receiver: a friend's turn-in reward before our own: given once, then the turn-in is closed here")
{
	// FFDiamondCity03 30: the friend handed in the fern flower first. Our game gives their reward and closes our own turn-in
	// as the stage would: the stage marked done (its script, which would give it all again, never runs; the engine doesn't
	// run a stage done already again, so our player's own turn-in later gives nothing), the quest completed and stopped.
	Receiver receiver;
	CHECK(receiver.Judge(Reward(1, Kind::kStage, QUEST, 30), false, false, T0) == Verdict::kGive);
	CHECK(RewardBook::CloseTurnIn(true, true, false, false, true) == (RewardBook::Closing{ true, true, true }));
	// Closed, the stage reads done here: another friend's reward of it later (past the same-time window) is not given.
	CHECK(receiver.Judge(Reward(2, Kind::kStage, QUEST, 30), true, false, T0 + RewardBook::SAME_TURN_IN + 1s) == Verdict::kStageDone);

	// A turn-in that doesn't end its quest (V81_03 3, a sale): only its stage is marked.
	CHECK(RewardBook::CloseTurnIn(false, false, false, false, true) == (RewardBook::Closing{ true, false, false }));
	// Nothing already so is done again: the stage done, the quest completed or stopped already.
	CHECK(RewardBook::CloseTurnIn(true, true, true, true, false) == (RewardBook::Closing{ false, false, false }));
	CHECK(RewardBook::CloseTurnIn(true, true, false, true, true) == (RewardBook::Closing{ true, false, true }));
}

TEST("reward turn-ins: every StageRules kPays row is in RewardLines::TURN_INS, and the scan read their ends")
{
	// tools/rewardscan.ps1 writes TURN_INS from StageRules' kPays rows: a row added there without running it again is caught here.
	for (const auto& rule : StageRules::RULES) {
		if ((rule.flags & StageRules::kPays) == 0) {
			continue;
		}
		const bool listed = std::ranges::any_of(RewardLines::TURN_INS, [&](const RewardLines::TurnIn& a_turnIn) {
			return a_turnIn.file == rule.file && a_turnIn.quest == rule.quest && a_turnIn.stage == rule.stage;
		});
		CHECK(listed);
	}
	const auto ends = [](std::uint32_t a_quest, std::uint16_t a_stage) -> int {
		for (const auto& turnIn : RewardLines::TURN_INS) {
			if (turnIn.file == 0 && turnIn.quest == a_quest && turnIn.stage == a_stage) {
				return turnIn.ends;
			}
		}
		return -1;
	};
	CHECK(ends(0x02125F, 30) == (RewardLines::kCompletes | RewardLines::kStops));  // FFDiamondCity03: the fern flower; completes, Stop()
	CHECK(ends(0x099848, 90) == 0);  // MinRecruit02: the ransom; its 450, set in turn, completes the quest
	CHECK(ends(0x033527, 3) == 0);   // V81_03: a Jet sold
}