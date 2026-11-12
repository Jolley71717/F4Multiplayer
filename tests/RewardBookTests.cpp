// Shared quest rewards, kept free of the game (common/RewardBook.h): the turn-in's game claims what its player got from a
// reward line or a turn-in stage, the server keeps each reward for its run and sends it once to every other player, and a
// friend's game gives it once, unless its player got the same in their own game.

#include "Test.h"

#include "RewardBook.h"
#include "game/QuestRewards.h"
#include "game/RewardLines.h"
#include "game/StageRules.h"

#include <algorithm>
#include <functional>
#include <string_view>
#include <utility>

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

	// A turn-in's ends in RewardLines::TURN_INS (-1: not listed).
	int TurnInEnds(std::uint8_t a_file, std::uint32_t a_quest, std::uint16_t a_stage)
	{
		for (const auto& turnIn : RewardLines::TURN_INS) {
			if (turnIn.file == a_file && turnIn.quest == a_quest && turnIn.stage == a_stage) {
				return turnIn.ends;
			}
		}
		return -1;
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

// The rig, 2026-11-12: the VM's game was killed, the host's turn-in 15 s later went to its dead link and was counted had,
// the server saw it leave 3 s after that, and its rejoin was sent nothing.
TEST("reward ledger: a reward sent just before its player left is owed again, one sent long before isn't")
{
	Ledger      ledger;
	const auto* first = ledger.Add(1, 0x11, Claimed{ Kind::kStage, QUEST, 30, 100, {} }, T0).first;
	REQUIRE(first);
	const auto firstId = first->id;
	ledger.Sent(firstId, 0x22, T0);
	ledger.Sent(firstId, 0x33, T0);
	const auto* second = ledger.Add(1, 0x11, Claimed{ Kind::kLine, QUEST, INFO, 0, { { CAPS, 5 } } }, T0 + 40s).first;
	REQUIRE(second);
	const auto secondId = second->id;
	ledger.Sent(secondId, 0x22, T0 + 40s);
	ledger.Sent(secondId, 0x33, T0 + 40s);

	// 0x22 seen gone 15 s after the second: that one is owed again, the first (40 s before) isn't.
	CHECK(ledger.Left(0x22, T0 + 55s) == 1);
	auto owed = ledger.Owed(0x22, T0 + 70s);
	REQUIRE(owed.size() == 1);
	CHECK(owed[0]->id == secondId);
	CHECK(ledger.Owed(0x22, T0 + 70s).empty());  // once
	// Its welcome's send counts as a send too: gone again at once, owed again.
	CHECK(ledger.Left(0x22, T0 + 75s) == 1);
	CHECK(ledger.Owed(0x22, T0 + 80s).size() == 1);
	// Gone UNSURE_SEND after it: had.
	CHECK(ledger.Left(0x22, T0 + 80s + RewardBook::UNSURE_SEND) == 0);
	CHECK(ledger.Owed(0x22, T0 + 200s).empty());

	// Another player's leave touches only theirs; the giver's leave never makes it owe its own.
	CHECK(ledger.Owed(0x33, T0 + 60s).empty());
	CHECK(ledger.Left(0x11, T0 + 45s) == 0);
	CHECK(ledger.Owed(0x11, T0 + 50s).empty());
	// A send with no time (a zero time point) is had for good.
	ledger.Sent(secondId, 0x44);
	CHECK(ledger.Left(0x44, T0 + 41s) == 0);
	CHECK(ledger.Owed(0x44, T0 + 42s).size() == 1);  // the first, never sent to it
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

// A line's reward given, the game saved, and its link gone before the server knew the reward reached it (UNSURE_SEND): the
// server sends it again at the rejoin, to a game restarted from that save. Its record knows it had it; one from a save made
// before the reward doesn't, and gives it (that world never got it).
TEST("reward receiver: the save's record knows the rewards judged before it, after a restart and a join")
{
	Receiver before;
	CHECK(before.SaveRecord().empty());  // none judged: no record
	CHECK(before.Judge(Reward(1, Kind::kLine, QUEST, INFO), false, false, T0) == Verdict::kGive);
	const auto older = before.SaveRecord();
	CHECK(before.Judge(Reward(2, Kind::kLine, QUEST, OTHER_INFO), false, false, T0 + 1s) == Verdict::kGive);
	const auto record = before.SaveRecord();
	CHECK(record.size() == 6 + 2 * 4);
	CHECK(record.size() <= Receiver::MAX_RECORD);

	// Restarted: the save loaded, then the join (another server to this game: NewServer keeps them).
	Receiver after;
	CHECK(after.LoadRecord(record));
	after.NewServer();
	CHECK(after.Judged() == 2);
	CHECK(after.Judge(Reward(2, Kind::kLine, QUEST, OTHER_INFO), false, false, T0 + 60s) == Verdict::kHad);
	CHECK(after.Judge(Reward(3, Kind::kLine, QUEST, INFO), false, false, T0 + 61s) == Verdict::kGive);
	// Another server's run: its own ids.
	CHECK(after.Judge(Reward(2, Kind::kLine, QUEST, OTHER_INFO, 0xB), false, false, T0 + 62s) == Verdict::kGive);

	// The older save: it never had reward 2.
	Receiver fromOlder;
	CHECK(fromOlder.LoadRecord(older));
	CHECK(fromOlder.Judge(Reward(2, Kind::kLine, QUEST, OTHER_INFO), false, false, T0 + 60s) == Verdict::kGive);
	CHECK(fromOlder.Judge(Reward(1, Kind::kLine, QUEST, INFO), false, false, T0 + 60s) == Verdict::kHad);

	// A load replaces what this game judged before it; a save with no record (a revert) has none.
	CHECK(fromOlder.LoadRecord({}));
	CHECK(fromOlder.Judged() == 0);
	CHECK(fromOlder.Judge(Reward(1, Kind::kLine, QUEST, INFO), false, false, T0 + 70s) == Verdict::kGive);

	// A record that can't be read: none taken.
	auto bad = record;
	bad.pop_back();
	Receiver broken;
	CHECK(!broken.LoadRecord(bad));
	CHECK(broken.Judged() == 0);
	std::vector<std::uint8_t> zeroRun(6, 0);
	CHECK(!broken.LoadRecord(zeroRun));
	auto longer = record;
	longer.push_back(0);
	CHECK(!broken.LoadRecord(longer));  // a byte past its ids
	CHECK(!broken.LoadRecord(std::span{ record }.first(5)));  // shorter than its run and count
	auto tooMany = record;  // a count past MAX_KEPT
	tooMany[4] = static_cast<std::uint8_t>((RewardBook::MAX_KEPT + 1) & 0xFF);
	tooMany[5] = static_cast<std::uint8_t>((RewardBook::MAX_KEPT + 1) >> 8);
	CHECK(!broken.LoadRecord(tooMany));
	CHECK(broken.Judged() == 0);

	// Only the newest MAX_KEPT ids are kept (the server keeps no older).
	Receiver many;
	for (std::uint32_t id = 1; id <= RewardBook::MAX_KEPT + 10; ++id) {
		many.Judge(Reward(id, Kind::kLine, QUEST, INFO), false, false, T0 + std::chrono::hours{ 2 } * id);
	}
	const auto full = many.SaveRecord();
	CHECK(full.size() == Receiver::MAX_RECORD);
	Receiver loaded;
	CHECK(loaded.LoadRecord(full));
	CHECK(loaded.Judge(Reward(RewardBook::MAX_KEPT + 10, Kind::kLine, QUEST, INFO), false, false, T0) == Verdict::kHad);
	CHECK(loaded.Judge(Reward(1, Kind::kLine, QUEST, INFO), false, false, T0) == Verdict::kGive);
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
	// The quests of each player's own sessions (QuestRewards::OWN_SESSIONS) are never a shared turn-in, and their kOwn rows
	// (kPays's bit under its other name) are not the scan's.
	for (const auto& rule : StageRules::RULES) {
		if ((rule.flags & StageRules::kPays) == 0 || QuestRewards::IsOwnSessions(rule.file, rule.quest)) {
			continue;
		}
		const bool listed = std::ranges::any_of(RewardLines::TURN_INS, [&](const RewardLines::TurnIn& a_turnIn) {
			return a_turnIn.file == rule.file && a_turnIn.quest == rule.quest && a_turnIn.stage == rule.stage;
		});
		CHECK(listed);
	}
	CHECK(TurnInEnds(0, 0x02125F, 30) == (RewardLines::kCompletes | RewardLines::kStops));  // FFDiamondCity03: the fern flower; completes, Stop()
	CHECK(TurnInEnds(0, 0x099848, 90) == 0);  // MinRecruit02: the ransom; its 450, set in turn, completes the quest
	CHECK(TurnInEnds(0, 0x033527, 3) == 0);   // V81_03: a Jet sold
}

TEST("reward companions: no companion's quest is shared as a reward, its hire stages and lines included")
{
	// A companion is each player's own. MacCready's hire is paid at COMMacCreadyQuest 30/40/50 (a StageRules kPays row each
	// once the companion quests are shared): sharing it would hand the hirer's pickups around it to the friends and mark the
	// stage done in their saves, where a hire stage done is how a game knows it hired him (and drives his quest). So the
	// companions' quests are left out of the rewards (QuestRewards::COMPANION_QUESTS), keyed on the quest, whatever its rows.
	using QuestRewards::IsCompanionQuest;
	CHECK(IsCompanionQuest(0, 0x027411));  // COMMacCreadyQuest: the hire and Long Road Ahead
	CHECK(IsCompanionQuest(0, 0x16454E));  // COMCurieQuest
	CHECK(IsCompanionQuest(0, 0x0BBD9C));  // COMMacCready: his toy soldier line (RewardLines) is his gift, not shared
	CHECK(IsCompanionQuest(3, 0x027715));  // DLC04COMGage
	// The file counts: the same form ID in another master is another quest.
	CHECK(!IsCompanionQuest(1, 0x027411));
	// The rest stay shared: DialogueDiamondCity, FFDiamondCity03 (the rig's turn-in), DLC04MS01.
	CHECK(!IsCompanionQuest(0, 0x003648));
	CHECK(!IsCompanionQuest(0, 0x02125F));
	CHECK(!IsCompanionQuest(3, 0x000806));
	// Sorted by file and form ID, no repeats.
	const auto& list = QuestRewards::COMPANION_QUESTS;
	CHECK(std::ranges::is_sorted(list, std::less<>{}, [](const QuestRewards::Local& a_local) { return std::pair{ a_local.file, a_local.quest }; }));
	CHECK(std::ranges::adjacent_find(list, [](const QuestRewards::Local& a, const QuestRewards::Local& b) { return a.file == b.file && a.quest == b.quest; }) ==
		  std::ranges::end(list));
	// The game skips each row of a companion's quest by its quest: the toy soldier's line is one today.
	std::size_t lines = 0;
	for (const auto& row : RewardLines::ROWS) {
		lines += IsCompanionQuest(row.questFile, row.quest) ? 1 : 0;
	}
	CHECK(lines >= 1);
	// The only payment rows of a companion's quest are MacCready's hire stages (none before the companion quests are shared):
	// another one is looked at, and named here, before it ships.
	for (const auto& rule : StageRules::RULES) {
		if ((rule.flags & StageRules::kPays) != 0 && IsCompanionQuest(rule.file, rule.quest)) {
			CHECK(rule.file == 0 && rule.quest == 0x027411 && (rule.stage == 30 || rule.stage == 40 || rule.stage == 50));
		}
	}
}

TEST("reward own sessions: the Hubologists' stages are never a shared turn-in, and no other quest has a kOwn row")
{
	// StageRules' kOwn is kPays's bit: without this list the rewards would claim the Hubologists' rads, rank and objective
	// stages (and their donations) as turn-ins, hand what our player got there to the friends and mark the stage done in
	// their saves, where each player has their own sessions to do.
	using QuestRewards::IsOwnSessions;
	CHECK(StageRules::kOwn == StageRules::kPays);
	CHECK(IsOwnSessions(3, 0x00CBD0));   // DLC04DialogueHubologists
	CHECK(!IsOwnSessions(0, 0x00CBD0));  // the file counts
	CHECK(!IsOwnSessions(3, 0x000806));  // DLC04MS01 stays shared
	CHECK(!QuestRewards::IsCompanionQuest(3, 0x00CBD0));
	// Every kPays-bit row of theirs is covered, the donations (30-37) and the kOwn rows alike: 27 rows.
	std::size_t own = 0;
	for (const auto& rule : StageRules::RULES) {
		own += (rule.flags & StageRules::kPays) != 0 && IsOwnSessions(rule.file, rule.quest) ? 1 : 0;
	}
	CHECK(own == 27);
	// A paying row of any other quest is a turn-in the scan wrote (the test above), so a kOwn row added to another quest
	// without naming it in OWN_SESSIONS fails there.
	const auto& list = QuestRewards::OWN_SESSIONS;
	CHECK(std::ranges::is_sorted(list, std::less<>{}, [](const QuestRewards::Local& a_local) { return std::pair{ a_local.file, a_local.quest }; }));
}

TEST("reward turn-ins: a turn-in whose quest allows repeated stages is flagged, and each one is named here")
{
	// A friend's game closes a turn-in it gave the reward of by marking its stage done (CloseTurnIn), which holds only because
	// the engine doesn't run a stage done already again. A quest with Allow Repeated Stages (DNAM 0x0008) does: such a turn-in
	// is kRepeats, and no game claims it. The scan read the flag: DLC04MS01's 75 has it (the Quartz goes to Sierra; the player
	// gets nothing back, so nothing was shared there anyway). A new row with it fails here until it is looked at and named.
	CHECK(TurnInEnds(3, 0x000806, 75) == RewardLines::kRepeats);
	for (const auto& turnIn : RewardLines::TURN_INS) {
		if ((turnIn.ends & RewardLines::kRepeats) == 0) {
			continue;
		}
		const bool named = turnIn.file == 3 && turnIn.quest == 0x000806 && turnIn.stage == 75;
		CHECK(named);
	}
	// FFDiamondCity03, the rig's turn-in, has no such flag: its closing holds.
	CHECK((TurnInEnds(0, 0x02125F, 30) & RewardLines::kRepeats) == 0);
}

TEST("reward hold: a turn-in stage's reward waits for a friend's earlier stage of its quest still waiting here")
{
	using RewardBook::Hold;
	using RewardBook::HoldOf;
	const std::span<const std::uint16_t> empty{};
	// V81_03: the friend is talking to someone, so the giver's 1 (the quest's start) waits in their queue; the 3 (the Jet sold)
	// comes. Closing 3 would move the quest up to it and drop the 1 as passed, its script never run: it waits.
	const std::uint16_t lower[] = { 1 };
	CHECK(HoldOf(Kind::kStage, 3, false, lower, false, 0s) == Hold::kEarlierStage);
	CHECK(HoldOf(Kind::kStage, 3, false, lower, false, 9min) == Hold::kEarlierStage);
	// The 1 set (the conversation over): given.
	CHECK(HoldOf(Kind::kStage, 3, false, empty, false, 1min) == Hold::kNone);
	// A later stage waiting is no reason for one that only marks itself: it isn't passed by the closing.
	const std::uint16_t higher[] = { 40 };
	CHECK(HoldOf(Kind::kStage, 30, false, higher, false, 0s) == Hold::kNone);
	// One that completes or stops its quest (FFDiamondCity03's 30) drops every stage of it still waiting: any one holds it.
	CHECK(HoldOf(Kind::kStage, 30, false, higher, true, 0s) == Hold::kEarlierStage);
	CHECK(HoldOf(Kind::kStage, 30, false, empty, true, 0s) == Hold::kNone);
	// A line's reward closes nothing: stages waiting don't hold it.
	CHECK(HoldOf(Kind::kLine, INFO, false, lower, true, 0s) == Hold::kNone);
	// Our own turn-in of the same still open comes first, whatever waits.
	CHECK(HoldOf(Kind::kStage, 3, true, lower, false, 0s) == Hold::kOwnOpen);
	CHECK(HoldOf(Kind::kLine, INFO, true, empty, false, 0s) == Hold::kOwnOpen);
	// A stage that waits for good here (its quest stopped here before) holds it no longer than EARLIER_STAGE_HOLD.
	CHECK(HoldOf(Kind::kStage, 3, false, lower, false, RewardBook::EARLIER_STAGE_HOLD) == Hold::kNone);
	CHECK(HoldOf(Kind::kStage, 3, true, lower, false, RewardBook::EARLIER_STAGE_HOLD) == Hold::kOwnOpen);
}

TEST("reward hold: while held for an earlier stage, our player's own turn-in of it makes the friend's reward theirs")
{
	// The friend's reward waits for the 1; meanwhile our player sells their own Jet (the 3 done here, claimed and reported).
	// Released, the friend's is a twin within SAME_TURN_IN, or the stage reads done here after it: not given twice.
	Receiver receiver;
	const std::uint16_t lower[] = { 1 };
	CHECK(RewardBook::HoldOf(Kind::kStage, 3, false, lower, false, 0s) == RewardBook::Hold::kEarlierStage);
	receiver.Reported(Claimed{ Kind::kStage, QUEST, 3, 0, { { CAPS, 75 } } }, T0 + 10s);
	CHECK(receiver.Judge(Reward(1, Kind::kStage, QUEST, 3), true, false, T0 + 20s) == Verdict::kOwnTurnIn);
	CHECK(receiver.Judge(Reward(2, Kind::kStage, QUEST, 3), true, false, T0 + RewardBook::SAME_TURN_IN + 30s) == Verdict::kStageDone);
}