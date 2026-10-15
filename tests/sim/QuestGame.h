#pragma once

// One player's quests, for the sim (tests/sim/Sim.h): what QuestSync does in that player's game, run on a
// model of the game's quests instead of TESQuest. The rules are the plugin's own, free of the game:
// StageLog::Queue sets friends' stages in the order they came, one per quest at a time, the next once the
// last reads done, with the holds, timeouts and the lock's timer (paused time not counted);
// StageRules::Judge decides each stage (one that moves the player, a payment, a side quest moving the
// story on, a lock whose release isn't coming); StageRules::Hold, Find and FindLock read the table
// generated from the game's quest fragments; StageLog::Settable says which quests still take stages after
// leaving; StageLog::WaitsFor says whether a quest takes them now; StageLog::NewlyDone finds what this
// game reports, StageLog::Unstarted keeps what was reached just before the quest was started; and
// StageLog::Outbox keeps or drops those reports while not connected (the story's sent only if the
// welcome shares it), at a pace the server takes.
// The glue between them is a copy of QuestSync.cpp's and only as good as that copy: when a stage counts
// as live, the friend nearby, the combat hold, the warning and its cover, the lock and its release (looked
// at before the queue's poll), what is dropped when the session ends (with Session's
// Story::SetShared(false)) or the server changes, and the 5 s settle before reporting. Left out: scenes
// (only a conversation holds stages back), the MQ104/MQ105 repair, completion notices
// and the type checks (a test says which quests are the story).
//
// The model of a quest: the stages it has, which are done, the current one, whether it is running and
// started here (an objective shown, by any stage but a quiet one, or a moment later), and what its stages'
// scripts do (start another quest, complete this one, award an achievement). What a stage does to the player comes from StageRules' table by its form ID
// (Fallout4.esm's, load order index 00): moved, made to pay, controls taken and given back.

#include "Protocol.h"

#include "game/StageLog.h"
#include "game/StageRules.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Sim
{
	// A quest as one player's game has it.
	struct Quest
	{
		bool                    story = false;    // the main story or a faction's: shared only with sStory = shared
		std::set<std::uint16_t> stages;           // the stages it has
		bool                    running = false;
		bool                    started = false;  // an objective has been shown (QuestSync::PlayerStarted)
		// Stages whose scripts show none of its objectives (a stage set starts the quest, but shows the
		// first objective only if its script does), and those whose scripts show it a moment after the
		// stage reads done (the game runs a stage's script a moment after setting it).
		std::set<std::uint16_t> quiet;
		std::set<std::uint16_t> showsLater;
		bool                    completed = false;
		std::uint16_t           current = 0;
		std::set<std::uint16_t> done;
		// What its stages' scripts do: start another quest at one of its stages, complete and stop this
		// one, award an achievement.
		std::map<std::uint16_t, std::pair<std::uint32_t, std::uint16_t>> starts;
		std::set<std::uint16_t>                                          finishes;
		std::map<std::uint16_t, std::string>                             awards;
	};

	// What a save keeps of the quests: the quests, the achievements and whether the player can move.
	struct QuestSave
	{
		std::map<std::uint32_t, Quest> quests;
		std::set<std::string>          achievements;
		bool                           controls = true;
	};

	struct Position
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
	};

	class QuestGame
	{
	public:
		using Clock = StageLog::Clock;
		using Packet = std::vector<std::uint8_t>;
		using FriendAt = std::function<std::optional<Position>(std::uint32_t)>;

		static constexpr auto POLL_INTERVAL = std::chrono::milliseconds(500);  // QuestSync::Frame polls twice a second

		// ---- the game ----

		// This game has the quest (every game has the same ones; their state is each game's own).
		void Give(std::uint32_t a_id, Quest a_quest) { quests[a_id] = std::move(a_quest); }
		[[nodiscard]] bool Modelled(std::uint32_t a_id) const { return quests.contains(a_id); }
		[[nodiscard]] const Quest& Of(std::uint32_t a_id) const { return quests.at(a_id); }
		[[nodiscard]] bool Has(std::uint32_t a_id, std::uint16_t a_stage) const { return Modelled(a_id) && quests.at(a_id).done.contains(a_stage); }

		// The game sets a stage (the player got there, a script set it, or the console did): TESQuest::SetStage.
		void Do(std::uint32_t a_id, std::uint16_t a_stage)
		{
			const auto it = quests.find(a_id);
			if (it == quests.end() || !it->second.stages.contains(a_stage) || it->second.done.contains(a_stage)) {
				return;
			}
			auto& quest = it->second;
			quest.running = true;  // SetStage starts a quest that isn't running
			if (quest.showsLater.contains(a_stage)) {
				showing.emplace_back(a_id, now + POLL_INTERVAL);
			} else if (!quest.quiet.contains(a_stage)) {
				quest.started = true;  // ... and the stage shows its objective
			}
			quest.done.insert(a_stage);
			quest.current = (std::max)(quest.current, a_stage);
			const auto rule = StageRules::Find(StageRules::File::kFallout4, a_id & 0x00FFFFFF, a_stage);
			if (rule && (rule->flags & StageRules::kMoves)) {
				++moved;
			}
			if (rule && (rule->flags & StageRules::kPays)) {
				++paid;
			}
			if (StageRules::FindLock(StageRules::File::kFallout4, a_id & 0x00FFFFFF, a_stage)) {
				controls = false;
			}
			for (const auto& lock : StageRules::LOCKS) {
				if (lock.releaseQuest == (a_id & 0x00FFFFFF) && lock.releaseStage == a_stage) {
					controls = true;
				}
			}
			history.emplace_back(a_id, a_stage);
			if (const auto award = quest.awards.find(a_stage); award != quest.awards.end()) {
				achievements.insert(award->second);
			}
			if (quest.finishes.contains(a_stage)) {
				quest.completed = true;
				quest.running = false;
			}
			if (const auto start = quest.starts.find(a_stage); start != quest.starts.end()) {
				Do(start->second.first, start->second.second);
			}
		}

		// What the player is doing, as QuestSync reads it from the game.
		bool loading = false;   // a loading screen (WorldSync::InWorld false)
		bool paused = false;    // a menu is open (UI::menuMode)
		bool talking = false;   // in a conversation (DialogueMenu)
		bool fighting = false;  // in combat
		bool opening = false;   // a new character still in Vault 111 (Story::InOpening)
		bool story = false;     // the host shares the story (Story::Shared: the welcome's, false out of a session)

		// ---- QuestSync ----

		// A stage from another player: live (they just did it), or from the session (a WorldState).
		void Apply(const Protocol::QuestStage& a_stage, bool a_live)
		{
			const auto it = quests.find(a_stage.quest);
			if (it == quests.end() || !Shared(it->second) || StageLog::Finished(Flags(it->second)) || Settled(a_stage.quest, a_stage.stage)) {
				return;
			}
			if (!it->second.stages.contains(a_stage.stage)) {
				++unknown;
				return;
			}
			if (!waiting.Add(a_stage.quest, a_stage.stage)) {
				return;
			}
			if (a_live) {
				liveAt[Key(a_stage.quest, a_stage.stage)] = Live{ now, a_stage.playerId };
			}
		}

		// One poll (QuestSync::Frame, then Session's TakeOutgoing): POLL_INTERVAL of play. a_inSession:
		// welcomed; a_wanted: in a session or joining or rejoining one. Returns the reports to send.
		std::vector<Packet> Poll(bool a_inSession, bool a_wanted, Position a_me, const FriendAt& a_friendAt)
		{
			now += POLL_INTERVAL;
			me = a_me;
			std::erase_if(showing, [&](const auto& a_show) {
				if (a_show.second >= now) {
					return false;
				}
				quests[a_show.first].started = true;  // the stage's script has run
				return true;
			});
			friendAt = a_friendAt;
			if (loading) {
				Game game{ *this };
				waiting.Poll(now, game, true);
				return outbox.Take(now, { a_inSession, a_wanted, story });
			}
			if (a_inSession || locked) {
				busy = talking;
				combatHold = CombatHold();
				std::erase_if(liveAt, [&](const auto& a_entry) { return now - a_entry.second.at >= StageRules::LIVE_FOR; });
				CheckReleased(a_inSession);
				Game game{ *this };
				gaveUp += waiting.Poll(now, game, paused).size();
				std::erase_if(waited, [&](std::uint32_t a_quest) { return !waiting.Contains(a_quest); });
				if (!paused) {
					CheckLock(a_inSession);
				}
			}
			// The console runs what was set a moment later.
			for (const auto& [quest, stage] : std::exchange(console, {})) {
				Do(quest, stage);
			}

			if (!reportFrom) {
				reportFrom = now + SETTLE_TIME;
			}
			const bool report = now >= *reportFrom;
			unstarted.Expire(now);
			for (const auto& [id, quest] : quests) {
				StageLog::Stages done(quest.done.begin(), quest.done.end());
				const auto [known, firstLook] = doneStages.try_emplace(id);
				if (firstLook) {
					known->second = std::move(done);
					continue;
				}
				auto       fresh = StageLog::NewlyDone(known->second, std::move(done));
				const auto remote = remoteStageAt.find(id);
				const bool reaction = remote != remoteStageAt.end() && now - remote->second < SCRIPT_REACTION_WINDOW;
				// The story's too while not welcomed, whatever the story is then (the outbox sends them if the welcome shares it).
				if ((fresh.empty() && !unstarted.Has(id)) || !report || reaction || (quest.story && a_inSession && !story) || opening) {
					continue;
				}
				if (!quest.started) {
					unstarted.Add(id, fresh, now);
					continue;
				}
				fresh = unstarted.Take(id, std::move(fresh), now);
				for (const auto stage : fresh) {
					outbox.Add(Protocol::Encode(Protocol::QuestStage{ id, stage }, Protocol::MessageType::kReportQuestStage), quest.story);
					++reported;
				}
			}
			return outbox.Take(now, { a_inSession, a_wanted, story });
		}

		// Session::OnDisconnected: QuestSync::OnSessionEnded and Story::SetShared(false).
		void OnSessionEnded()
		{
			if (locked) {
				waiting.Keep({ locked->quest, locked->releaseQuest });
			} else {
				waiting.Clear();
			}
			waited.clear();
			liveAt.clear();
			warnedAt.reset();
			remoteStageAt.clear();
			story = false;
		}

		// Session::SwitchServer: QuestSync::OnServerChanged.
		void OnServerChanged() { outbox.NewSession(); }

		// A save is loaded: QuestSync::Rebaseline.
		void Load(const QuestSave& a_save)
		{
			quests = a_save.quests;
			achievements = a_save.achievements;
			controls = a_save.controls;
			doneStages.clear();
			remoteStageAt.clear();
			reportFrom.reset();
			waiting.Clear();
			waited.clear();
			liveAt.clear();
			warnedAt.reset();
			locked.reset();
			console.clear();
			showing.clear();
			unstarted.Clear();
			outbox.Clear();
		}

		[[nodiscard]] QuestSave Save() const { return { quests, achievements, controls }; }

		// ---- what the player sees ----

		[[nodiscard]] const std::set<std::string>&      Achievements() const { return achievements; }
		[[nodiscard]] bool                              Controls() const { return controls; }
		[[nodiscard]] bool                              Locked() const { return locked.has_value(); }
		[[nodiscard]] const std::vector<std::string>&   Notices() const { return notices; }
		[[nodiscard]] StageLog::Stages                  Waiting(std::uint32_t a_quest) const { return waiting.Waiting(a_quest); }
		[[nodiscard]] std::size_t                       WaitingCount() const { return waiting.Size(); }
		[[nodiscard]] const std::vector<std::pair<std::uint32_t, std::uint16_t>>& Skipped() const { return skipped; }
		[[nodiscard]] Clock::time_point                 Now() const { return now; }
		// Every stage set in this game, in order: its own progress, friends' and what scripts set.
		[[nodiscard]] const std::vector<std::pair<std::uint32_t, std::uint16_t>>& History() const { return history; }

		int         moved = 0;  // stages that moved the player (a teleport, a fade, MoveTo)
		int         paid = 0;   // stages that took caps or items from the player
		std::size_t reported = 0;
		std::size_t applied = 0;
		std::size_t caughtUp = 0;
		std::size_t gaveUp = 0;
		std::size_t unknown = 0;
		std::size_t released = 0;  // controls given back by the lock's timeout

	private:
		// QuestSync's own timings.
		static constexpr auto SETTLE_TIME = std::chrono::seconds(5);
		static constexpr auto SCRIPT_REACTION_WINDOW = std::chrono::seconds(3);
		static constexpr auto AFTER_COMBAT = std::chrono::seconds(5);
		static constexpr auto MAX_COMBAT_HOLD = std::chrono::seconds(60);

		struct Live
		{
			Clock::time_point at;
			std::uint32_t     playerId = 0;
		};

		struct LockHeld
		{
			std::uint32_t quest = 0;
			std::uint16_t stage = 0;
			std::uint32_t releaseQuest = 0;
			std::uint16_t releaseStage = 0;
		};

		// The game as the queue sees it (QuestSync's Game).
		struct Game
		{
			QuestGame& self;

			StageLog::Quest Quest(std::uint32_t a_quest) const { return self.StateOf(a_quest); }
			bool            Settled(std::uint32_t a_quest, std::uint16_t a_stage) const { return self.Settled(a_quest, a_stage); }
			StageLog::Set   Set(std::uint32_t a_quest, std::uint16_t a_stage) const { return self.SetHere(a_quest, a_stage); }
			Clock::duration Hold(std::uint32_t a_quest, std::uint16_t a_stage) const
			{
				return StageRules::Hold(StageRules::Find(StageRules::File::kFallout4, a_quest & 0x00FFFFFF, a_stage));
			}
		};

		static std::uint64_t Key(std::uint32_t a_quest, std::uint16_t a_stage) { return (static_cast<std::uint64_t>(a_quest) << 16) | a_stage; }

		static std::uint16_t Flags(const Quest& a_quest)
		{
			return static_cast<std::uint16_t>((a_quest.running ? StageLog::QUEST_RUNNING : 0) | (a_quest.completed ? StageLog::QUEST_COMPLETED : 0));
		}

		bool Shared(const Quest& a_quest) const { return !a_quest.story || story; }

		bool Unlocking(std::uint32_t a_quest) const { return locked && (a_quest == locked->quest || a_quest == locked->releaseQuest); }

		StageLog::Quest StateOf(std::uint32_t a_id)
		{
			const auto it = quests.find(a_id);
			if (it == quests.end() || !StageLog::Settable(Shared(it->second), Unlocking(a_id)) || StageLog::Finished(Flags(it->second))) {
				return StageLog::Quest::kGone;
			}
			StageLog::QuestHere here;
			here.inWorld = !loading;
			here.opening = opening;
			here.running = it->second.running;
			here.started = it->second.started;
			here.busy = busy;
			const auto wait = StageLog::WaitsFor(here);
			if (wait == StageLog::Wait::kNone) {
				return StageLog::Quest::kReady;
			}
			if (wait != StageLog::Wait::kBusy) {
				waited.insert(a_id);
			}
			return StageLog::Quest::kWait;
		}

		bool Settled(std::uint32_t a_id, std::uint16_t a_stage) const
		{
			const auto it = quests.find(a_id);
			return it == quests.end() || a_stage <= it->second.current || it->second.done.contains(a_stage);
		}

		bool CombatHold()
		{
			if (fighting) {
				if (!combatSince) {
					combatSince = now;
				}
				lastCombat = now;
				return now - *combatSince < MAX_COMBAT_HOLD;
			}
			combatSince.reset();
			return now - lastCombat < AFTER_COMBAT;
		}

		bool NearFriend(std::uint32_t a_id) const
		{
			const auto at = a_id && friendAt ? friendAt(a_id) : std::nullopt;
			if (!at) {
				return false;
			}
			const float dx = at->x - me.x;
			const float dy = at->y - me.y;
			const float dz = at->z - me.z;
			return dx * dx + dy * dy + dz * dz <= StageRules::NEAR_FRIEND * StageRules::NEAR_FRIEND;
		}

		void SetStageHere(std::uint32_t a_id, std::uint16_t a_stage)
		{
			console.emplace_back(a_id, a_stage);
			if (const auto known = doneStages.find(a_id); known != doneStages.end()) {
				StageLog::Insert(known->second, a_stage);
			}
			remoteStageAt[a_id] = now;
			++applied;
		}

		StageLog::Set SetHere(std::uint32_t a_id, std::uint16_t a_stage)
		{
			const auto local = a_id & 0x00FFFFFF;
			const auto rule = StageRules::Find(StageRules::File::kFallout4, local, a_stage);
			const auto lock = StageRules::FindLock(StageRules::File::kFallout4, local, a_stage);
			const auto releaseId = lock ? (a_id & 0xFF000000) | lock->releaseQuest : 0u;

			StageRules::Situation situation;
			situation.sharedStory = story;
			const auto live = liveAt.find(Key(a_id, a_stage));
			situation.live = live != liveAt.end() && now - live->second.at < StageRules::LIVE_FOR;
			const bool unlocking = Unlocking(a_id);
			situation.held = combatHold && !unlocking;
			if (lock) {
				situation.releaseComing = waiting.Contains(releaseId, lock->releaseStage) || Has(releaseId, lock->releaseStage);
			}
			if (unlocking) {
				situation.warned = StageRules::WARNING;
			} else if (warnedAt && now - *warnedAt < StageRules::WARNING_COVERS) {
				situation.warned = now - *warnedAt;
			}
			if (rule && (rule->flags & StageRules::kMoves) && situation.live) {
				situation.nearFriend = NearFriend(live->second.playerId);
			}
			switch (StageRules::Judge(rule, situation)) {
			case StageRules::Verdict::kSkip:
				skipped.emplace_back(a_id, a_stage);
				return StageLog::Set::kSkipped;
			case StageRules::Verdict::kWarn:
				warnedAt = now;
				notices.push_back(std::format("Catching up with your friends' story in {} s (it may move you)",
					std::chrono::duration_cast<std::chrono::seconds>(StageRules::WARNING).count()));
				return StageLog::Set::kLater;
			case StageRules::Verdict::kLater:
				return StageLog::Set::kLater;
			case StageRules::Verdict::kQuestWaits:
				return StageLog::Set::kQuestWaits;
			case StageRules::Verdict::kSet:
				break;
			}
			SetStageHere(a_id, a_stage);
			if (lock) {
				locked = LockHeld{ a_id, a_stage, releaseId, lock->releaseStage };
				waiting.Lock(a_id, releaseId);
			}
			if (waited.erase(a_id) > 0) {
				++caughtUp;
			}
			return StageLog::Set::kSet;
		}

		void Unlock(bool a_inSession)
		{
			locked.reset();
			waiting.Unlock();
			if (!a_inSession) {
				waiting.Clear();
			}
		}

		void CheckReleased(bool a_inSession)
		{
			if (locked && (!Modelled(locked->releaseQuest) || Has(locked->releaseQuest, locked->releaseStage))) {
				Unlock(a_inSession);
			}
		}

		void CheckLock(bool a_inSession)
		{
			CheckReleased(a_inSession);
			if (!locked || !waiting.LockOverdue()) {
				return;
			}
			SetStageHere(locked->releaseQuest, locked->releaseStage);
			++released;
			notices.push_back("Your friends' story step didn't finish; you have your controls back");
			Unlock(a_inSession);
		}

		std::map<std::uint32_t, Quest>                      quests;
		std::set<std::string>                               achievements;
		bool                                                controls = true;
		std::vector<std::string>                            notices;
		std::vector<std::pair<std::uint32_t, std::uint16_t>> skipped;
		std::vector<std::pair<std::uint32_t, std::uint16_t>> history;

		Clock::time_point                                   now = Clock::time_point{} + std::chrono::hours(1);
		Position                                            me;
		FriendAt                                            friendAt;
		StageLog::Queue                                     waiting;
		std::set<std::uint32_t>                             waited;
		std::map<std::uint64_t, Live>                       liveAt;
		std::optional<LockHeld>                               locked;
		std::optional<Clock::time_point>                    warnedAt;
		std::map<std::uint32_t, StageLog::Stages>           doneStages;
		std::map<std::uint32_t, Clock::time_point>          remoteStageAt;
		std::optional<Clock::time_point>                    reportFrom;
		std::vector<std::pair<std::uint32_t, std::uint16_t>> console;  // set, to run at the end of the poll
		std::vector<std::pair<std::uint32_t, Clock::time_point>> showing;  // objectives a stage's script shows a moment later
		StageLog::Unstarted                                 unstarted;
		StageLog::Outbox<Packet>                            outbox;
		bool                                                busy = false;
		bool                                                combatHold = false;
		std::optional<Clock::time_point>                    combatSince;
		Clock::time_point                                   lastCombat{};
	};
}
