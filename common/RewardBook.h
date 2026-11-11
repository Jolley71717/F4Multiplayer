#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// Shared quest rewards: on a shared quest, both players get the full reward (caps, items, XP) when either turns it in.
//
// Most of a quest's reward is already both players': a stage's own script (its AddItem, its RewardPlayerXP) and the quest
// XP the engine gives when a stage completes the quest run in every game the stage is set in, and QuestSync sets a
// friend's stages in ours, live or as a joiner catches up. What never reaches a friend's game is:
// - a line's: what a dialogue line's script hands the player (a reward paid in conversation: Inst302's 500 caps, MS14's
//   caps and chems, the surgery key), run only in the game of the player who talked (src/game/RewardLines.h lists them);
// - a turn-in stage's: a stage the friends' games skip because it takes from the player (StageRules kPays: the fern flower
//   delivered, the space suits handed in), and so never give what it gives back, nor the quest XP when it completes the
//   quest.
// The game that turned it in claims what its player got from those (Capture), reports it, and the server keeps it for its
// run (Ledger) and sends it once to every other player, there now or joining later; their games give it (Receiver).
//
// Kept free of the game so it can be tested (tests/RewardBookTests.cpp); src/game/QuestRewards.cpp and Server.cpp use it.
namespace RewardBook
{
	using Clock = std::chrono::steady_clock;
	using Kind = Protocol::RewardKind;
	using Item = Protocol::RewardItem;

	// ---------------- the game that turned it in ----------------

	// A line's script runs as the line begins (Fragment_Begin) and as it ends (Fragment_End); its items come at most this
	// long before the line is first seen (seen at the next poll) ...
	inline constexpr auto LINE_LOOKBACK = std::chrono::milliseconds{ 1000 };
	// ... and this long after it was last seen (the end fragment runs as the line goes, on the script's own time).
	inline constexpr auto LINE_SETTLE = std::chrono::milliseconds{ 3000 };
	// A turn-in stage is seen done up to a stage poll (500 ms) after it was set, and its script has given by then or within
	// STAGE_SETTLE: the window it claims around the moment it was seen.
	inline constexpr auto STAGE_LOOKBACK = std::chrono::milliseconds{ 1500 };
	inline constexpr auto STAGE_SETTLE = std::chrono::milliseconds{ 3000 };
	// Gains no moment claimed are forgotten after this.
	inline constexpr auto GAIN_KEPT = std::chrono::seconds{ 15 };

	// One item of a line (RewardLines::Row): what its script gives, and how many (0: any; claims one gain of it whole).
	struct Want
	{
		std::uint32_t item = 0;
		std::uint16_t count = 0;
	};

	// What our player got from one turn-in, to report (Protocol::QuestReward without the server's id).
	struct Claimed
	{
		Kind              kind = Kind::kLine;
		std::uint32_t     quest = 0;
		std::uint32_t     source = 0;
		std::uint32_t     xp = 0;
		std::vector<Item> items;

		bool operator==(const Claimed&) const = default;
	};

	// Items a script added to our player (from nowhere: not a pickup, a container's, a trade's or the session's own) and the
	// player's own XP gains, with the moments that may have given them, paired up as the moments close. A line claims only
	// its own items (the table's), up to their counts; a stage what is left in its window when every line open with it has
	// closed (a stage a line sets gives in the same window: the line's items are the line's, the rest the stage's), and its
	// XP. A gain is claimed once.
	class Capture
	{
	public:
		void Gain(std::uint32_t a_form, std::int32_t a_count, Clock::time_point a_at)
		{
			// (An item made in this game, a runtime form, is no other game's: never claimed.)
			if (Protocol::IsShareableRef(a_form) && a_count > 0) {
				gains.push_back({ a_form, a_count, a_at });
			}
		}

		void Xp(float a_xp, Clock::time_point a_at)
		{
			if (a_xp > 0.0f) {
				xps.push_back({ a_xp, a_at });
			}
		}

		// Our player or who they talk to is saying a_info, a line that gives a_wants (seen at each poll it is said).
		void Line(std::uint32_t a_quest, std::uint32_t a_info, std::span<const Want> a_wants, Clock::time_point a_now)
		{
			for (auto& moment : moments) {
				if (moment.kind == Kind::kLine && moment.source == a_info && !moment.closed) {
					moment.last = a_now;
					return;
				}
			}
			Moment moment;
			moment.kind = Kind::kLine;
			moment.quest = a_quest;
			moment.source = a_info;
			moment.first = a_now;
			moment.last = a_now;
			moment.wants.assign(a_wants.begin(), a_wants.end());
			moments.push_back(std::move(moment));
		}

		// A turn-in stage done here, seen now.
		void Stage(std::uint32_t a_quest, std::uint16_t a_stage, Clock::time_point a_now)
		{
			Moment moment;
			moment.kind = Kind::kStage;
			moment.quest = a_quest;
			moment.source = a_stage;
			moment.first = a_now;
			moment.last = a_now;
			moments.push_back(std::move(moment));
		}

		// The turn-ins whose windows are over, with what they claimed (one that claimed nothing gave nothing here: its script
		// took another branch, or the player had it already as a quest item). Lines first, in the order they were seen.
		std::vector<Claimed> Poll(Clock::time_point a_now)
		{
			std::vector<Claimed> out;
			for (auto& moment : moments) {
				if (moment.kind == Kind::kLine && !moment.closed && a_now >= moment.last + LINE_SETTLE) {
					moment.closed = true;
					if (auto claimed = ClaimLine(moment); !claimed.items.empty()) {
						out.push_back(std::move(claimed));
					}
				}
			}
			for (auto& moment : moments) {
				if (moment.kind != Kind::kStage || moment.closed || a_now < moment.last + STAGE_SETTLE) {
					continue;
				}
				// A line open beside it claims its own first.
				const bool lineOpen = std::ranges::any_of(moments, [&](const Moment& a_line) {
					return a_line.kind == Kind::kLine && !a_line.closed && a_line.first - LINE_LOOKBACK <= moment.last + STAGE_SETTLE;
				});
				if (lineOpen) {
					continue;
				}
				moment.closed = true;
				if (auto claimed = ClaimStage(moment); !claimed.items.empty() || claimed.xp > 0) {
					out.push_back(std::move(claimed));
				}
			}
			std::erase_if(moments, [](const Moment& a_moment) { return a_moment.closed; });
			// What no open moment can still claim goes after GAIN_KEPT.
			const auto oldest = OldestWindow(a_now);
			std::erase_if(gains, [&](const Got& a_gain) { return a_gain.left <= 0 || (a_gain.at < oldest && a_now - a_gain.at > GAIN_KEPT); });
			std::erase_if(xps, [&](const GotXp& a_xp) { return a_xp.left <= 0.0f || (a_xp.at < oldest && a_now - a_xp.at > GAIN_KEPT); });
			return out;
		}

		void Clear()
		{
			gains.clear();
			xps.clear();
			moments.clear();
		}

		[[nodiscard]] std::size_t Open() const { return moments.size(); }
		[[nodiscard]] std::size_t Kept() const { return gains.size() + xps.size(); }

	private:
		struct Got
		{
			std::uint32_t     form = 0;
			std::int32_t      left = 0;
			Clock::time_point at{};
		};

		struct GotXp
		{
			float             left = 0.0f;
			Clock::time_point at{};
		};

		struct Moment
		{
			Kind              kind = Kind::kLine;
			std::uint32_t     quest = 0;
			std::uint32_t     source = 0;
			Clock::time_point first{};
			Clock::time_point last{};
			std::vector<Want> wants;
			bool              closed = false;
		};

		// The earliest a still-open moment's window reaches back to (now, with none open).
		Clock::time_point OldestWindow(Clock::time_point a_now) const
		{
			auto oldest = a_now;
			for (const auto& moment : moments) {
				oldest = (std::min)(oldest, moment.first - (moment.kind == Kind::kLine ? LINE_LOOKBACK : STAGE_LOOKBACK));
			}
			return oldest;
		}

		static void Add(std::vector<Item>& a_items, std::uint32_t a_form, std::int32_t a_count)
		{
			for (auto& item : a_items) {
				if (item.form == a_form) {
					item.count = (std::min)(item.count + a_count, Protocol::MAX_ITEM_COUNT);
					return;
				}
			}
			if (a_items.size() < Protocol::MAX_REWARD_ITEMS) {
				a_items.push_back({ a_form, a_count });
			}
		}

		Claimed ClaimLine(const Moment& a_moment)
		{
			Claimed out{ Kind::kLine, a_moment.quest, a_moment.source, 0, {} };
			const auto from = a_moment.first - LINE_LOOKBACK;
			const auto until = a_moment.last + LINE_SETTLE;
			const auto within = [&](const Got& a_gain) { return a_gain.left > 0 && a_gain.at >= from && a_gain.at <= until; };
			// Counted items first, then each "any" one takes one gain of its item whole (a leveled list's caps).
			auto wants = a_moment.wants;
			std::ranges::stable_sort(wants, [](const Want& a, const Want& b) { return (a.count != 0) > (b.count != 0); });
			for (const auto& want : wants) {
				if (want.count != 0) {
					std::int32_t need = want.count;
					for (auto& gain : gains) {
						if (need <= 0) {
							break;
						}
						if (gain.form == want.item && within(gain)) {
							const auto take = (std::min)(need, gain.left);
							gain.left -= take;
							need -= take;
							Add(out.items, want.item, take);
						}
					}
				} else {
					for (auto& gain : gains) {
						if (gain.form == want.item && within(gain)) {
							Add(out.items, want.item, gain.left);
							gain.left = 0;
							break;
						}
					}
				}
			}
			return out;
		}

		Claimed ClaimStage(const Moment& a_moment)
		{
			Claimed    out{ Kind::kStage, a_moment.quest, a_moment.source, 0, {} };
			const auto from = a_moment.first - STAGE_LOOKBACK;
			const auto until = a_moment.last + STAGE_SETTLE;
			for (auto& gain : gains) {
				if (gain.left > 0 && gain.at >= from && gain.at <= until) {
					Add(out.items, gain.form, gain.left);
					gain.left = 0;
				}
			}
			float xp = 0.0f;
			for (auto& gained : xps) {
				if (gained.left > 0.0f && gained.at >= from && gained.at <= until) {
					xp += gained.left;
					gained.left = 0.0f;
				}
			}
			out.xp = static_cast<std::uint32_t>((std::min)(xp + 0.5f, static_cast<float>(Protocol::MAX_REWARD_XP)));
			return out;
		}

		std::vector<Got>    gains;
		std::vector<GotXp>  xps;
		std::vector<Moment> moments;
	};

	// ---------------- the server ----------------

	// The same line or stage reported by another player this soon after the first: both turned it in at once, and each
	// game gave its own (the second is refused).
	inline constexpr auto SAME_TURN_IN = std::chrono::seconds{ 60 };
	// Rewards the server keeps for its run (the oldest go first): a session's evening of quests is a few dozen.
	inline constexpr std::size_t MAX_KEPT = 512;

	// Who a player is to the ledger: the identity their game says hello with (kept on their PC, the same when they come
	// back), or their player id when they gave none.
	[[nodiscard]] inline std::uint64_t Who(std::uint64_t a_helloId, std::uint32_t a_playerId)
	{
		return a_helloId != 0 ? a_helloId : (std::uint64_t{ 1 } << 63) | a_playerId;
	}

	// The rewards of the server's run, each with the players whose games have it (who turned it in, and everyone it was sent
	// to). Not in the session file: a reward a player missed for the whole run of a server is not given later (the session
	// file's layout stays as it is).
	class Ledger
	{
	public:
		struct Entry
		{
			std::uint32_t                     id = 0;
			std::uint32_t                     playerId = 0;  // who turned it in, as the server numbers them now
			std::uint64_t                     giver = 0;     // Who
			Claimed                           reward;
			Clock::time_point                 at{};
			std::unordered_set<std::uint64_t> had;  // Who: the giver, and everyone it was sent to
		};

		enum class Refusal : std::uint8_t
		{
			kNone,
			kSameTurnIn,  // another player reported the same line or stage within SAME_TURN_IN
			kRepeat,      // the giver reported the same within SAME_TURN_IN (sent twice)
		};

		// The reward a player's game reports: kept and numbered, or refused.
		std::pair<const Entry*, Refusal> Add(std::uint32_t a_playerId, std::uint64_t a_who, Claimed a_reward, Clock::time_point a_now)
		{
			for (const auto& entry : entries) {
				if (entry.reward.kind == a_reward.kind && entry.reward.quest == a_reward.quest && entry.reward.source == a_reward.source && a_now - entry.at < SAME_TURN_IN) {
					return { nullptr, entry.giver == a_who ? Refusal::kRepeat : Refusal::kSameTurnIn };
				}
			}
			if (entries.size() >= MAX_KEPT) {
				entries.pop_front();
			}
			Entry entry;
			entry.id = ++lastId;
			entry.playerId = a_playerId;
			entry.giver = a_who;
			entry.reward = std::move(a_reward);
			entry.at = a_now;
			entry.had.insert(a_who);
			entries.push_back(std::move(entry));
			return { &entries.back(), Refusal::kNone };
		}

		// The rewards a player's game hasn't had, oldest first; each is marked had as it is returned (the caller sends it).
		std::vector<const Entry*> Owed(std::uint64_t a_who)
		{
			std::vector<const Entry*> out;
			for (auto& entry : entries) {
				if (entry.had.insert(a_who).second) {
					out.push_back(&entry);
				}
			}
			return out;
		}

		// A reward sent to a_who (live).
		void Sent(std::uint32_t a_id, std::uint64_t a_who)
		{
			for (auto& entry : entries) {
				if (entry.id == a_id) {
					entry.had.insert(a_who);
					return;
				}
			}
		}

		void Clear() { entries.clear(); }

		[[nodiscard]] std::size_t Size() const { return entries.size(); }

	private:
		std::deque<Entry> entries;
		std::uint32_t     lastId = 0;
	};

	// ---------------- a friend's game ----------------

	enum class Verdict : std::uint8_t
	{
		kGive,
		kHad,        // this id was given here already (sent again after a reconnect)
		kOwnTurnIn,  // our game turned the same line or stage in within SAME_TURN_IN: our player got their own
		kStageDone,  // a turn-in stage done here: our own game ran it (a friend's is never set here), so it gave its own
		kFinished,   // a line of a quest our player had finished before any friend's stage of it was set here: they got
		             // its rewards in their own game
	};

	// What our game makes of a friend's reward: given once, unless our player got the same in their own game.
	class Receiver
	{
	public:
		// Our game reported this reward (Capture's): a friend's of the same line or stage soon after is a twin.
		void Reported(const Claimed& a_reward, Clock::time_point a_now)
		{
			std::erase_if(own, [&](const Own& a_own) { return a_now - a_own.at >= SAME_TURN_IN; });
			own.push_back({ a_reward.kind, a_reward.quest, a_reward.source, a_now });
		}

		// a_stageDone: the reward is a stage's and that stage is done here; a_finishedOnOwn: its quest is completed here and no
		// friend's stage of it was set here.
		Verdict Judge(const Protocol::QuestReward& a_reward, bool a_stageDone, bool a_finishedOnOwn, Clock::time_point a_now)
		{
			if (given.contains(a_reward.id)) {
				return Verdict::kHad;
			}
			const bool twin = std::ranges::any_of(own, [&](const Own& a_own) {
				return a_own.kind == a_reward.kind && a_own.quest == a_reward.quest && a_own.source == a_reward.source && a_now - a_own.at < SAME_TURN_IN;
			});
			auto verdict = Verdict::kGive;
			if (twin) {
				verdict = Verdict::kOwnTurnIn;
			} else if (a_reward.kind == Kind::kStage && a_stageDone) {
				verdict = Verdict::kStageDone;
			} else if (a_reward.kind == Kind::kLine && a_finishedOnOwn) {
				verdict = Verdict::kFinished;
			}
			given.insert(a_reward.id);  // judged once, whatever the verdict
			return verdict;
		}

		// A new session (another server): its ids start again.
		void Clear()
		{
			own.clear();
			given.clear();
		}

	private:
		struct Own
		{
			Kind              kind = Kind::kLine;
			std::uint32_t     quest = 0;
			std::uint32_t     source = 0;
			Clock::time_point at{};
		};

		std::vector<Own>                  own;
		std::unordered_set<std::uint32_t> given;
	};

	[[nodiscard]] inline const char* Why(Verdict a_verdict)
	{
		switch (a_verdict) {
		case Verdict::kHad:
			return "given here already";
		case Verdict::kOwnTurnIn:
			return "our player turned the same in at the same time and got their own";
		case Verdict::kStageDone:
			return "the stage is done here: our own game ran it and gave its own";
		case Verdict::kFinished:
			return "our player finished the quest in their own game before this session moved it here";
		default:
			return "given";
		}
	}
}
