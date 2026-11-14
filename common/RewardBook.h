#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <optional>
#include <random>
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
	// XP if the stage completes its quest. A gain is claimed once.
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

		// A turn-in stage done here, seen now. a_completes: the stage itself completes its quest (RewardLines::TURN_INS), so
		// the quest XP in its window is its own; a stage it sets in turn that completes the quest is set in the friends'
		// games too and gives them their XP there, so a stage that doesn't complete it claims items only.
		void Stage(std::uint32_t a_quest, std::uint16_t a_stage, bool a_completes, Clock::time_point a_now)
		{
			Moment moment;
			moment.kind = Kind::kStage;
			moment.quest = a_quest;
			moment.source = a_stage;
			moment.first = a_now;
			moment.last = a_now;
			moment.completes = a_completes;
			moments.push_back(std::move(moment));
		}

		// Whether a moment of this line or stage is still open here (our player is turning the same in now): a friend's
		// reward of it waits until it closes, so our own report, if any, is known first (Receiver::Reported).
		[[nodiscard]] bool IsOpen(Kind a_kind, std::uint32_t a_quest, std::uint32_t a_source) const
		{
			return std::ranges::any_of(moments, [&](const Moment& a_moment) {
				return !a_moment.closed && a_moment.kind == a_kind && a_moment.quest == a_quest && a_moment.source == a_source;
			});
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
			bool              completes = false;  // a stage that completes its quest: claims the XP in its window
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
				if (a_moment.completes && gained.left > 0.0f && gained.at >= from && gained.at <= until) {
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
	// A reward sent to a player's game this soon before the server saw them leave may have gone down a link that was already
	// dead: the transport calls a peer gone only once it has heard nothing for its timeout (SteamTransport's 20 s), and a
	// reliable message sent meanwhile is lost with the link. Such a send is not counted as had (Ledger::Left), so their
	// rejoin's welcome sends it again; a game that did get it judges it had by its id (Receiver), kept in its saves too
	// (Receiver::SaveRecord: a game restarted from a save made after it has it, one from an earlier save gives it).
	// The rig, 2026-11-12: the VM's game was killed, the host's turn-in 15 s later was sent to it and counted had, its "left"
	// came 3 s after that, and its rejoin got nothing.
	inline constexpr auto UNSURE_SEND = std::chrono::seconds{ 30 };

	// Who a player is to the ledger: the identity their game says hello with (kept on their PC, the same when they come
	// back), or their player id when they gave none.
	[[nodiscard]] inline std::uint64_t Who(std::uint64_t a_helloId, std::uint32_t a_playerId)
	{
		return a_helloId != 0 ? a_helloId : (std::uint64_t{ 1 } << 63) | a_playerId;
	}

	// A server run's id: random and non-zero, so a game that stays up through a restart of the server (the same address) knows
	// the new run's rewards, numbered from 1 again, from the old run's it was given (Receiver).
	[[nodiscard]] inline std::uint32_t NewRun()
	{
		std::random_device random;
		std::uint32_t      run = 0;
		while (run == 0) {
			run = static_cast<std::uint32_t>(random());
		}
		return run;
	}

	// The rewards of the server's run, each with the players whose games have it (who turned it in, and everyone it was sent
	// to). Not in the session file: a reward a player missed for the whole run of a server is not given later (the session
	// file's layout stays as it is).
	class Ledger
	{
	public:
		explicit Ledger(std::uint32_t a_run = NewRun()) :
			run(a_run != 0 ? a_run : 1)
		{}

		[[nodiscard]] std::uint32_t Run() const { return run; }

		struct Entry
		{
			std::uint32_t                     id = 0;
			std::uint32_t                     playerId = 0;  // who turned it in, as the server numbers them now
			std::uint64_t                     giver = 0;     // Who
			Claimed                           reward;
			Clock::time_point                 at{};
			std::unordered_set<std::uint64_t> had;  // Who: the giver, and everyone it was sent to
			std::vector<std::pair<std::uint64_t, Clock::time_point>> sent;  // its sends of the last UNSURE_SEND (Who, when)
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

		// The rewards a player's game hasn't had, oldest first; each is marked had as it is returned (the caller sends it), its
		// send noted at a_now for Left.
		std::vector<const Entry*> Owed(std::uint64_t a_who, Clock::time_point a_now = {})
		{
			std::vector<const Entry*> out;
			for (auto& entry : entries) {
				if (entry.had.insert(a_who).second) {
					Note(entry, a_who, a_now);
					out.push_back(&entry);
				}
			}
			return out;
		}

		// A reward sent to a_who (live) at a_now.
		void Sent(std::uint32_t a_id, std::uint64_t a_who, Clock::time_point a_now = {})
		{
			for (auto& entry : entries) {
				if (entry.id == a_id) {
					entry.had.insert(a_who);
					Note(entry, a_who, a_now);
					return;
				}
			}
		}

		// a_who's game left (its link closed, or found dead) at a_now: the rewards sent to it in the UNSURE_SEND before are no
		// longer counted as had, so its next welcome sends them again (Owed). Returns how many.
		std::size_t Left(std::uint64_t a_who, Clock::time_point a_now)
		{
			std::size_t unsure = 0;
			for (auto& entry : entries) {
				bool recent = false;
				std::erase_if(entry.sent, [&](const auto& a_send) {
					if (a_send.first != a_who && a_now - a_send.second < UNSURE_SEND) {
						return false;  // another's, still recent
					}
					recent = recent || (a_send.first == a_who && a_now - a_send.second < UNSURE_SEND);
					return true;
				});
				if (recent && a_who != entry.giver && entry.had.erase(a_who) != 0) {
					++unsure;
				}
			}
			return unsure;
		}

		void Clear() { entries.clear(); }

		[[nodiscard]] std::size_t Size() const { return entries.size(); }

	private:
		// A send at a known time (a caller with none, a zero time, has it counted had for good).
		static void Note(Entry& a_entry, std::uint64_t a_who, Clock::time_point a_now)
		{
			if (a_now != Clock::time_point{}) {
				a_entry.sent.emplace_back(a_who, a_now);
			}
		}

		std::deque<Entry> entries;
		std::uint32_t     run = 0;
		std::uint32_t     lastId = 0;
	};

	// ---------------- a friend's game ----------------

	enum class Verdict : std::uint8_t
	{
		kGive,
		kHad,        // this id of this server run was given here already (sent again after a reconnect)
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
			// Another run of the server (restarted, the same address): its ids are new.
			if (a_reward.run != run) {
				given.clear();
				run = a_reward.run;
			}
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
			run = 0;
		}

		// Another server joined: our own turn-ins are not its. The ids judged stay under their run: another server's run is
		// another random id, which clears them at its first reward (Judge), and those a save's record gave (LoadRecord), read
		// before a join, are its world's.
		void NewServer() { own.clear(); }

		// The save's record of the rewards judged here (written with each save, read back with it): a game that gave a reward,
		// saved and then left before the server knew the reward reached it (UNSURE_SEND) is sent it again when it comes back,
		// and judges it had by this record after a restart too. Its run, a count and the ids, the newest MAX_KEPT (the server
		// keeps no older); nothing when none was judged.
		[[nodiscard]] std::vector<std::uint8_t> SaveRecord() const
		{
			std::vector<std::uint8_t> out;
			if (run == 0 || given.empty()) {
				return out;
			}
			std::vector<std::uint32_t> ids(given.begin(), given.end());
			std::ranges::sort(ids, std::greater<>{});
			if (ids.size() > MAX_KEPT) {
				ids.resize(MAX_KEPT);
			}
			const auto put = [&](const auto& a_value) {
				const auto* bytes = reinterpret_cast<const std::uint8_t*>(&a_value);
				out.insert(out.end(), bytes, bytes + sizeof(a_value));
			};
			put(run);
			put(static_cast<std::uint16_t>(ids.size()));
			for (const auto id : ids) {
				put(id);
			}
			return out;
		}

		// A save loaded: the rewards judged here are the ones its record names (none without a record, or with one that can't be
		// read), whatever this game judged before the load; the save's world has what it was given. False when it can't be read.
		bool LoadRecord(std::span<const std::uint8_t> a_bytes)
		{
			given.clear();
			run = 0;
			std::uint32_t savedRun = 0;
			std::uint16_t count = 0;
			if (a_bytes.size() < sizeof(savedRun) + sizeof(count)) {
				return a_bytes.empty();
			}
			std::memcpy(&savedRun, a_bytes.data(), sizeof(savedRun));
			std::memcpy(&count, a_bytes.data() + sizeof(savedRun), sizeof(count));
			if (savedRun == 0 || count > MAX_KEPT || a_bytes.size() != sizeof(savedRun) + sizeof(count) + std::size_t{ count } * sizeof(std::uint32_t)) {
				return false;
			}
			for (std::size_t i = 0; i < count; ++i) {
				std::uint32_t id = 0;
				std::memcpy(&id, a_bytes.data() + sizeof(savedRun) + sizeof(count) + i * sizeof(id), sizeof(id));
				given.insert(id);
			}
			run = savedRun;
			return true;
		}

		[[nodiscard]] std::size_t Judged() const { return given.size(); }

		static constexpr std::size_t MAX_RECORD = sizeof(std::uint32_t) + sizeof(std::uint16_t) + MAX_KEPT * sizeof(std::uint32_t);
		static_assert(MAX_KEPT <= 0xFFFF, "SaveRecord's count is two bytes");

	private:
		struct Own
		{
			Kind              kind = Kind::kLine;
			std::uint32_t     quest = 0;
			std::uint32_t     source = 0;
			Clock::time_point at{};
		};

		std::vector<Own>                  own;
		std::unordered_set<std::uint32_t> given;  // ids of the run below judged here
		std::uint32_t                     run = 0;
	};

	// A friend's reward held for the stages before it (Hold::kEarlierStage) is given anyway after this long: a stage that waits
	// for good here (its quest stopped here before) would otherwise keep it for the rest of the session.
	inline constexpr auto EARLIER_STAGE_HOLD = std::chrono::minutes{ 10 };

	// Why a friend's reward waits before it is judged here (src/game/QuestRewards.cpp, each frame our player is in the world).
	enum class Hold : std::uint8_t
	{
		kNone,
		kOwnOpen,       // our player is turning the same line or stage in now (Capture::IsOpen): our own report, if any, comes first
		kEarlierStage,  // a turn-in stage's, while a friend's stage of its quest still waits to be set here (QuestSync's queue: our
		                // player in a conversation or a scene, the quest not yet started for the session). Closing the turn-in
		                // (CloseTurnIn) moves an ordered quest up to it, and the lower stages waiting would then be dropped as passed;
		                // one that completes or stops its quest drops every stage of it still waiting. Their scripts would never run
		                // here. So it waits for them, as the turn-in came after them in the friend's game.
	};

	// a_ownOpen: Capture::IsOpen for the reward. a_waiting: the friends' stages of its quest still waiting here. a_endsQuest: its
	// stage completes or stops the quest (RewardLines::TURN_INS). a_heldFor: how long it has been held for those stages.
	[[nodiscard]] inline Hold HoldOf(Kind a_kind, std::uint32_t a_source, bool a_ownOpen, std::span<const std::uint16_t> a_waiting, bool a_endsQuest,
		Clock::duration a_heldFor)
	{
		if (a_ownOpen) {
			return Hold::kOwnOpen;
		}
		if (a_kind != Kind::kStage || a_heldFor >= EARLIER_STAGE_HOLD) {
			return Hold::kNone;
		}
		const bool passed = std::ranges::any_of(a_waiting, [&](std::uint16_t a_stage) { return a_endsQuest || a_stage < a_source; });
		return passed ? Hold::kEarlierStage : Hold::kNone;
	}

	// What a friend's game does to its own quest once it gave a friend's turn-in stage's reward, so that its player can't
	// turn the same in again and get it all twice: the stage marked done without running its script (the script would give
	// the reward again, and take the delivery; the engine doesn't run a stage done already again, so the player's own
	// turn-in, which sets it, then gives nothing: the rig's rewards step checks it), and the quest completed and stopped
	// where the stage would (RewardLines::TURN_INS), so its dialogue goes. Nothing already so is done again.
	struct Closing
	{
		bool mark = false;      // mark the stage done
		bool complete = false;  // set the quest's completed flag (not through the engine's completion: no quest XP, the reward had it)
		bool stop = false;      // stop the quest

		bool operator==(const Closing&) const = default;
	};

	[[nodiscard]] inline Closing CloseTurnIn(bool a_completes, bool a_stops, bool a_stageDone, bool a_completed, bool a_running)
	{
		return { !a_stageDone, a_completes && !a_completed, a_stops && a_running };
	}

	// Whether the friends' rewards still to be given here stay through a load: they are our player's, so a load of the same
	// character keeps them (a quickload, a death reload), but a new game or another character's save is not that player, who
	// would otherwise get them on a rejoin. a_before: the character of the world before the load (none: no world loaded yet
	// since the plugin started); a_loaded: the loaded save's (CharacterBinding::Character).
	[[nodiscard]] constexpr bool KeepsPending(std::optional<std::uint64_t> a_before, std::uint64_t a_loaded, bool a_newGame)
	{
		return !a_newGame && (!a_before || *a_before == a_loaded);
	}

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

	[[nodiscard]] inline const char* Why(Hold a_hold)
	{
		switch (a_hold) {
		case Hold::kOwnOpen:
			return "our player is turning the same in now";
		case Hold::kEarlierStage:
			return "a friend's stage of its quest still waits to be set here, and closing the turn-in first would drop it";
		default:
			return "not held";
		}
	}
}
