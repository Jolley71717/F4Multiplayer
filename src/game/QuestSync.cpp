#include "game/QuestSync.h"
#include "game/Official.h"

#include "Config.h"
#include "game/Achievements.h"
#include "game/Hud.h"
#include "game/QuestStageLayout.h"
#include "game/RemotePlayers.h"
#include "game/StageLog.h"
#include "game/StageRules.h"
#include "game/Story.h"
#include "game/WorldSync.h"

namespace QuestSync
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using StageLog::QUEST_COMPLETED;
		using StageLog::QUEST_RUNNING;

		// BGSScene::parentQuest, read by BGSScene::SetSceneActive (+6376D0: `mov rdx,[rcx+0A8h]`, then the
		// quest's running bit at +F4).
		static_assert(offsetof(RE::BGSScene, parentQuest) == 0xA8);

		constexpr auto POLL_INTERVAL = 500ms;
		// After loading, quest scripts settle their stages for a moment; that isn't progress.
		constexpr auto SETTLE_TIME = 5s;

		// QUEST_DATA::questType values that are shared: main quest, the four factions, side quests
		// and DLC quests. 0 is "none" (internal quests) and 6 is "miscellaneous" (radiant/ambient).
		bool IsSharedType(std::int8_t a_type)
		{
			return a_type > 0 && a_type != 6;
		}

		// The factions' last quests, which the game files under no type (0, like its internal quests). They
		// are shared with the rest of the story when the host shares it; their stages award achievements.
		// Fallout4.esm: Inst305 "Powering Up", BoS302 "Blind Betrayal", BoS304 "Ad Victoriam".
		constexpr std::array<std::uint32_t, 3> STORY_FINALES{ 0x000A274B, 0x000B9F9D, 0x00173ED9 };

		bool IsFinale(const RE::TESQuest* a_quest)
		{
			return a_quest->data.questType == 0 && std::ranges::find(STORY_FINALES, a_quest->GetFormID()) != STORY_FINALES.end();
		}

		// Quests whose progress is watched here: the shared types and the factions' finales.
		bool Watched(const RE::TESQuest* a_quest)
		{
			return IsSharedType(a_quest->data.questType) || IsFinale(a_quest);
		}

		// A quest that completes this soon after a stage from another player was completed by them.
		constexpr auto REMOTE_COMPLETION_WINDOW = 10s;
		// Stages a quest's script sets this soon after another player's stage are its reaction to
		// that stage, not this player's progress: every game that applies it gets there by itself.
		constexpr auto SCRIPT_REACTION_WINDOW = 3s;
		// A friend's stage is live (they did it just now, not caught up from the session) this long after
		// it came; a stage that moves the player is set then if a friend is within NEAR_FRIEND (game
		// units, about 85 m): it moves them along with the friend who did it.
		constexpr auto  LIVE_FOR = 30s;
		constexpr float NEAR_FRIEND = 6000.0f;
		// Caught-up stages wait while the player fights and a moment after: a stage that moves them
		// mustn't come mid-fight, and its quest XP would be taken for kill XP (see Party). Not forever:
		// something out of reach can keep the player in combat.
		constexpr auto AFTER_COMBAT = 5s;
		constexpr auto MAX_COMBAT_HOLD = 60s;
		// A scene of a quest with stages waiting here holds them back only so long: a stage set from far
		// away can start a scene with the player in it that only a later stage of its quest ends (MQ104's
		// 18 starts one, its 30 ends it), while the scene's other actors are nowhere near.
		constexpr auto MAX_SCENE_HOLD = 30s;
		// After a warning that a stage will move the player, more such stages go without another one.
		constexpr auto WARNING_COVERS = 60s;
		// A stage set here took the player's controls away, and the one that gives them back hasn't
		// come this long after: it is set outright (MQ206's 1200 after its 500; the teleport between
		// them takes 10 s, a loading screen included).
		constexpr auto LOCK_TIMEOUT = 90s;
		// Stage reports kept while not connected (the welcome still to come, a reconnect), at most.
		constexpr std::size_t MAX_UNSENT = 1024;
		// The quests caught up with are named in one notice, once none came for a moment (or every so often).
		constexpr auto NOTICE_QUIET = 3s;
		constexpr auto NOTICE_EVERY = 15s;
		constexpr std::size_t NOTICE_NAMES = 3;

		// MQ104 "Unlikely Valentine" and MQ105 "Getting a Clue" (Fallout4.esm). MQ104's 210 starts MQ105 at
		// 10 and moves MQ104 on to 1000; nothing else starts MQ105.
		constexpr std::uint32_t MQ104 = 0x0001F25E;
		constexpr std::uint16_t MQ104_STARTS_MQ105 = 210;
		constexpr std::uint32_t MQ105 = 0x000229E6;
		constexpr std::uint16_t MQ105_FIRST = 10;

		std::unordered_map<std::uint32_t, std::uint16_t> knownStages;  // last current stage seen per quest
		std::unordered_map<std::uint32_t, StageLog::Stages> doneStages;  // the stages seen done per quest
		std::unordered_map<std::uint32_t, bool>          knownDone;    // last completed state seen per quest
		std::unordered_map<std::uint32_t, Clock::time_point> remoteStageAt;  // when another player last moved it
		StageLog::Queue                                  waiting;  // stages from other players, until they are set here
		std::unordered_set<std::uint32_t>                waited;   // quests whose stages waited for the quest to start here
		struct Live
		{
			Clock::time_point at;
			std::uint32_t     playerId;  // who did it (0: not known)
		};
		std::unordered_map<std::uint64_t, Live>          liveAt;  // friends' stages that came live (quest << 16 | stage)
		// A stage set here took the player's controls away (StageRules::LOCKS), and the stage that gives
		// them back is still to come.
		struct Locked
		{
			std::uint32_t     quest;
			std::uint16_t     stage;
			std::uint32_t     releaseQuest;
			std::uint16_t     releaseStage;
			Clock::time_point since;
		};
		std::optional<Locked>                            locked;
		std::unordered_set<std::uint64_t>                lockWaitLogged;  // lock stages logged as waiting for their release
		std::optional<Clock::time_point>                 reportFrom;  // set on the first in-game poll
		std::vector<std::vector<std::uint8_t>>           outgoing;
		Clock::time_point                                nextPoll{};
		bool                                             busy = false;        // this poll: a conversation or scene
		bool                                             combatHold = false;  // this poll: caught-up stages wait
		std::optional<Clock::time_point>                 combatSince;
		Clock::time_point                                lastCombat{};
		std::optional<Clock::time_point>                 sceneSince;
		std::optional<Clock::time_point>                 warnedAt;
		std::vector<std::string>                         caughtUpNames;
		Clock::time_point                                caughtUpFirst{};
		Clock::time_point                                caughtUpLast{};
		std::uint32_t                                    reported = 0;
		std::uint32_t                                    applied = 0;
		std::uint32_t                                    caughtUp = 0;  // ... after waiting for the quest to start here
		std::uint32_t                                    gaveUp = 0;    // set, but not done in time
		std::uint32_t                                    refused = 0;   // more than the queue holds
		std::uint32_t                                    unknown = 0;   // stages the quest doesn't have
		std::uint32_t                                    skipped = 0;   // not for this player (StageRules)
		std::uint32_t                                    completed = 0;
		std::uint32_t                                    repaired = 0;
		std::uint32_t                                    released = 0;  // controls given back by the timeout
		std::unordered_map<std::uint32_t, std::uint16_t> miscStages;  // misc quests: stage changes only
		Clock::time_point                                lastStageChange{};

		std::uint64_t Key(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			return (static_cast<std::uint64_t>(a_quest) << 16) | a_stage;
		}

		std::span<RE::TESQuest*> AllQuests()
		{
			const auto handler = RE::TESDataHandler::GetSingleton();
			if (!handler) {
				return {};
			}
			auto& quests = handler->GetFormArray<RE::TESQuest>();
			return { quests.data(), quests.size() };
		}

		// A stage set in the middle of a conversation or scene can break it; wait until it's over. A scene
		// of a quest with stages waiting here only holds them for MAX_SCENE_HOLD.
		bool PlayerBusy(Clock::time_point a_now)
		{
			const auto ui = RE::UI::GetSingleton();
			if (ui && ui->GetMenuOpen("DialogueMenu"sv)) {
				return true;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto scene = player ? player->GetCurrentScene() : nullptr;
			if (!scene || !scene->parentQuest || !waiting.Contains(scene->parentQuest->GetFormID())) {
				sceneSince.reset();
				return scene != nullptr;
			}
			if (!sceneSince) {
				sceneSince = a_now;
			}
			return a_now - *sceneSince < MAX_SCENE_HOLD;
		}

		// Whether caught-up stages wait now (see AFTER_COMBAT).
		bool CombatHold(Clock::time_point a_now)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (player && player->IsInCombat()) {
				if (!combatSince) {
					combatSince = a_now;
				}
				lastCombat = a_now;
				return a_now - *combatSince < MAX_COMBAT_HOLD;
			}
			combatSince.reset();
			return a_now - lastCombat < AFTER_COMBAT;
		}

		// The game is paused (the Pip-Boy, the pause menu, a container, a terminal): scripts don't run.
		bool Paused()
		{
			const auto ui = RE::UI::GetSingleton();
			return !ui || ui->menuMode != 0;
		}

		// Whether this friend (the one who did a stage) is close by. Not any friend: with three players,
		// a stage one of them did across the map mustn't move the two standing together.
		bool NearFriend(std::uint32_t a_id)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto position = a_id ? RemotePlayers::PositionOf(a_id) : std::nullopt;
			if (!player || !position) {
				return false;
			}
			const auto& at = player->data.location;
			const float dx = position->x - at.x;
			const float dy = position->y - at.y;
			const float dz = position->z - at.z;
			return dx * dx + dy * dy + dz * dz <= NEAR_FRIEND * NEAR_FRIEND;
		}

		// The main story and faction quests (their finales too) are shared only if the host chose a
		// shared story. Quests from other mods are never shared (see Official.h).
		bool Shared(const RE::TESQuest* a_quest)
		{
			if (!Official::IsOfficialForm(a_quest)) {
				return false;
			}
			if (IsFinale(a_quest)) {
				return Story::Shared();
			}
			return IsSharedType(a_quest->data.questType) && (Story::Shared() || !Story::IsStoryType(a_quest->data.questType));
		}

		std::optional<StageRules::File> FileOf(const RE::TESQuest* a_quest)
		{
			const auto file = a_quest ? a_quest->GetFile(0) : nullptr;
			return file ? StageRules::FileOf(file->GetFilename()) : std::nullopt;
		}

		const StageRules::Rule* RuleFor(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			const auto file = FileOf(RE::TESForm::GetFormByID<RE::TESQuest>(a_quest));
			return file ? StageRules::Find(*file, a_quest & 0x00FFFFFF, a_stage) : nullptr;
		}

		// Whether stages from other players can be set on this quest now. They wait until this player
		// has started the quest too (an objective is shown; the game runs some quests in the background
		// long before that), possibly because another quest's stage starts it (MQ104's 210 starts MQ105).
		StageLog::Quest StateOf(std::uint32_t a_quest)
		{
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			if (!quest || !Shared(quest) || StageLog::Finished(quest->data.flags)) {
				return StageLog::Quest::kGone;
			}
			// Quest scripts must not run at the main menu or during a loading screen.
			if (!WorldSync::InWorld() || Story::InOpening() || !(quest->data.flags & QUEST_RUNNING) || !PlayerStarted(quest)) {
				waited.insert(a_quest);
				return StageLog::Quest::kWait;
			}
			return busy ? StageLog::Quest::kWait : StageLog::Quest::kReady;
		}

		// Stages only move forward: never undo progress a player already has. A stage this quest is
		// already past here is left alone, done or not (it may be a branch this player didn't take).
		bool IsSettled(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			return !quest || a_stage <= quest->currentStage || QuestStageLayout::IsStageDone(quest, a_stage);
		}

		void NoteCaughtUp(const char* a_name, Clock::time_point a_now)
		{
			if (!a_name || !*a_name) {
				return;
			}
			if (caughtUpNames.empty()) {
				caughtUpFirst = a_now;
			}
			if (std::ranges::find(caughtUpNames, std::string_view(a_name)) == caughtUpNames.end()) {
				caughtUpNames.emplace_back(a_name);
			}
			caughtUpLast = a_now;
		}

		// One notice for the quests caught up with lately (a late joiner catches up dozens).
		void FlushCaughtUp(Clock::time_point a_now)
		{
			if (caughtUpNames.empty() || (a_now - caughtUpLast < NOTICE_QUIET && a_now - caughtUpFirst < NOTICE_EVERY)) {
				return;
			}
			std::string names;
			const auto  shown = (std::min)(caughtUpNames.size(), NOTICE_NAMES);
			for (std::size_t i = 0; i < shown; ++i) {
				names += (i > 0 ? ", " : "") + caughtUpNames[i];
			}
			if (caughtUpNames.size() > shown) {
				names += std::format(" and {} more", caughtUpNames.size() - shown);
			}
			Hud::Notify(std::format("Caught up with your friends: {}", names));
			caughtUpNames.clear();
		}

		const char* WhySkipped(std::uint8_t a_flags)
		{
			if (a_flags & StageRules::kPays) {
				return "it takes caps or items from the player";
			}
			if (a_flags & StageRules::kStory) {
				return "it moves the main story or a faction's on, and the story isn't shared";
			}
			return "it moves the player, who isn't with the friend who did it";
		}

		// Sets a stage here as one from another player: through the console (it runs a moment later),
		// and not reported back.
		void SetStageHere(RE::TESQuest* a_quest, std::uint16_t a_stage)
		{
			const auto id = a_quest->GetFormID();
			RE::Console::ExecuteCommand(std::format("setstage {:08X} {}", id, a_stage).c_str());
			knownStages[id] = std::max(a_quest->currentStage, a_stage);
			if (const auto known = doneStages.find(id); known != doneStages.end()) {
				StageLog::Insert(known->second, a_stage);
			}
			remoteStageAt[id] = Clock::now();
			lastStageChange = Clock::now();  // its quest XP is ours, not kill XP
			++applied;
		}

		// Whether a stage of this quest goes on from a lock stage set here (MQ206's 1000, 1050 and 1200
		// after its 500): without a combat hold, since the player can't fight anyway, and without
		// another warning, since they were warned before the lock.
		bool Unlocking(std::uint32_t a_quest)
		{
			return locked && (a_quest == locked->quest || a_quest == locked->releaseQuest);
		}

		// Sets another player's stage here, if it is this player's to have (see StageRules::Judge). That
		// runs the stage's script, as it did in their game (an achievement in it too, where the game
		// awards them), and starts a quest from wherever the player is; it can pull a new character out
		// of the opening.
		StageLog::Set SetHere(std::uint32_t a_quest, std::uint16_t a_stage, Clock::time_point a_now)
		{
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			if (!quest) {
				return StageLog::Set::kSkipped;
			}
			const char* name = quest->GetFullName();
			const auto  file = FileOf(quest);
			const auto  local = a_quest & 0x00FFFFFF;
			const auto  rule = file ? StageRules::Find(*file, local, a_stage) : nullptr;
			const auto  lock = file ? StageRules::FindLock(*file, local, a_stage) : nullptr;
			const auto  releaseId = lock ? (a_quest & 0xFF000000) | lock->releaseQuest : 0u;

			StageRules::Situation now;
			now.sharedStory = Story::Shared();
			const auto live = liveAt.find(Key(a_quest, a_stage));
			now.live = live != liveAt.end() && a_now - live->second.at < LIVE_FOR;
			const bool unlocking = Unlocking(a_quest);
			now.held = combatHold && !unlocking;
			if (lock) {
				const auto release = RE::TESForm::GetFormByID<RE::TESQuest>(releaseId);
				now.releaseComing = waiting.Contains(releaseId, lock->releaseStage) || (release && QuestStageLayout::IsStageDone(release, lock->releaseStage));
			}
			if (unlocking) {
				now.warned = StageRules::WARNING;
			} else if (warnedAt && a_now - *warnedAt < WARNING_COVERS) {
				now.warned = a_now - *warnedAt;
			}
			if (rule && (rule->flags & StageRules::kMoves) && now.live) {
				now.nearFriend = NearFriend(live->second.playerId);
			}
			switch (StageRules::Judge(rule, now)) {
			case StageRules::Verdict::kSkip:
				++skipped;
				REX::INFO("QuestSync: not setting {:08X} '{}' stage {}: {}", a_quest, name ? name : "", a_stage, WhySkipped(rule ? rule->flags : 0));
				return StageLog::Set::kSkipped;
			case StageRules::Verdict::kWarn:
				warnedAt = a_now;
				Hud::Notify(std::format("Catching up with your friends' story in {} s: {} (it may move you)",
					std::chrono::duration_cast<std::chrono::seconds>(StageRules::WARNING).count(), name ? name : "a quest"));
				return StageLog::Set::kLater;
			case StageRules::Verdict::kLater:
				return StageLog::Set::kLater;
			case StageRules::Verdict::kQuestWaits:
				if (lockWaitLogged.insert(Key(a_quest, a_stage)).second) {
					REX::INFO("QuestSync: {:08X} '{}' stage {} takes the player's controls away; waiting for {:08X} stage {}, which gives them back",
						a_quest, name ? name : "", a_stage, releaseId, lock ? lock->releaseStage : 0);
				}
				return StageLog::Set::kQuestWaits;
			case StageRules::Verdict::kSet:
				break;
			}

			SetStageHere(quest, a_stage);
			REX::INFO("QuestSync: set {:08X} '{}' stage {} (was {}){}", a_quest, name ? name : "", a_stage, quest->currentStage, now.live ? "" : " (catching up)");
			if (lock) {
				locked = Locked{ a_quest, a_stage, releaseId, lock->releaseStage, a_now };
			}
			if (waited.erase(a_quest) > 0) {
				++caughtUp;
				NoteCaughtUp(name, a_now);
			}
			Achievements::AfterStageSet(a_quest, a_stage);
			return StageLog::Set::kSet;
		}

		// The player has their controls back once the lock's release stage is done. If it hasn't come
		// LOCK_TIMEOUT after the lock (its stage was given up on, or never ran), it is set outright
		// rather than leave the player unable to move.
		void CheckLock(Clock::time_point a_now)
		{
			if (!locked) {
				return;
			}
			const auto release = RE::TESForm::GetFormByID<RE::TESQuest>(locked->releaseQuest);
			if (!release || QuestStageLayout::IsStageDone(release, locked->releaseStage)) {
				locked.reset();
				return;
			}
			if (a_now - locked->since < LOCK_TIMEOUT) {
				return;
			}
			REX::WARN("QuestSync: {:08X} stage {} took the player's controls {} s ago and {:08X} stage {} hasn't given them back; setting it",
				locked->quest, locked->stage, std::chrono::duration_cast<std::chrono::seconds>(LOCK_TIMEOUT).count(), locked->releaseQuest, locked->releaseStage);
			SetStageHere(release, locked->releaseStage);
			++released;
			Hud::Notify("Your friends' story step didn't finish; you have your controls back");
			locked.reset();
		}

		// The game as the queue sees it.
		struct Game
		{
			Clock::time_point now;

			StageLog::Quest Quest(std::uint32_t a_quest) const { return StateOf(a_quest); }
			bool            Settled(std::uint32_t a_quest, std::uint16_t a_stage) const { return IsSettled(a_quest, a_stage); }
			StageLog::Set   Set(std::uint32_t a_quest, std::uint16_t a_stage) const { return SetHere(a_quest, a_stage, now); }
			Clock::duration Hold(std::uint32_t a_quest, std::uint16_t a_stage) const { return StageRules::Hold(RuleFor(a_quest, a_stage)); }
		};

		// An older version set only the highest stage of a friend's quest. MQ104 went straight to 1000,
		// its 210 never ran here, and so MQ105 never started: the main story stopped there, and the
		// session file doesn't have 210 to send again. Starts MQ105 for a player left like that.
		void RepairStory()
		{
			const auto mq104 = RE::TESForm::GetFormByID<RE::TESQuest>(MQ104);
			const auto mq105 = RE::TESForm::GetFormByID<RE::TESQuest>(MQ105);
			if (!mq104 || !mq105 || !Official::IsOfficialForm(mq104) || !Official::IsOfficialForm(mq105)) {
				return;
			}
			// In the game only 210 moves MQ104 past 210 (to 1000).
			if (mq104->currentStage <= MQ104_STARTS_MQ105 || QuestStageLayout::IsStageDone(mq104, MQ104_STARTS_MQ105)) {
				return;
			}
			if ((mq105->data.flags & (QUEST_RUNNING | QUEST_COMPLETED)) || !QuestStageLayout::Done(mq105).empty()) {
				return;
			}
			REX::WARN("QuestSync: MQ104 is at {} without its stage {}, and MQ105 never started; starting it", mq104->currentStage, MQ104_STARTS_MQ105);
			RE::Console::ExecuteCommand(std::format("setstage {:08X} {}", MQ105, MQ105_FIRST).c_str());
			++repaired;
			const char* name = mq105->GetFullName();
			Hud::Notify(std::format("Your main story was stuck; it goes on with {}", name && *name ? name : "the next quest"));
		}
	}

	bool PlayerStarted(const RE::TESQuest* a_quest)
	{
		return a_quest && std::ranges::any_of(a_quest->objectives, [](const RE::BGSQuestObjective* a_objective) {
			return a_objective && a_objective->state != static_cast<char>(RE::QUEST_OBJECTIVE_STATE::kDormant);
		});
	}

	void Apply(const Protocol::QuestStage& a_stage, bool a_live)
	{
		if (!Config::Get().syncQuests) {
			return;
		}
		const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_stage.quest);
		if (!quest || !Shared(quest) || StageLog::Finished(quest->data.flags) || IsSettled(a_stage.quest, a_stage.stage)) {
			return;
		}
		// A stage this quest doesn't have would only be set, never read done, and hold up the real ones.
		if (!QuestStageLayout::Has(quest, a_stage.stage)) {
			if (unknown++ == 0) {
				REX::WARN("QuestSync: {:08X} has no stage {}; ignoring it (and any more like it)", a_stage.quest, a_stage.stage);
			}
			return;
		}
		// Set by Frame, in the order they came: a session's stages arrive in the order they were reported
		// (all of them for a player who joins late or loads a save).
		if (!waiting.Add(a_stage.quest, a_stage.stage)) {
			if (waiting.Size() >= StageLog::Queue::MAX_WAITING && refused++ == 0) {
				REX::WARN("QuestSync: more than {} stages waiting; dropping some", StageLog::Queue::MAX_WAITING);
			}
			return;
		}
		if (a_live) {
			liveAt[Key(a_stage.quest, a_stage.stage)] = Live{ Clock::now(), a_stage.playerId };
		}
	}

	std::vector<std::uint16_t> Waiting(std::uint32_t a_quest)
	{
		return waiting.Waiting(a_quest);
	}

	void ApplyDone(const Protocol::QuestDone& a_done)
	{
		const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_done.quest);
		const char* name = quest ? quest->GetFullName() : nullptr;
		if (!quest || !Watched(quest) || !Official::IsOfficialForm(quest) || !name || !*name) {
			return;
		}
		Hud::Notify(std::format("{} completed {}", RemotePlayers::NameOf(a_done.playerId), name));
	}

	void Frame(bool a_inSession)
	{
		const auto now = Clock::now();
		if (now < nextPoll) {
			return;
		}
		nextPoll = now + POLL_INTERVAL;

		if (!WorldSync::InWorld()) {
			// A loading screen: no script runs, so no hold runs out.
			Game game{ now };
			waiting.Poll(now, game, true);
			return;
		}
		// Misc quests (radiant ones give XP too) are only watched for stage changes.
		for (const auto quest : AllQuests()) {
			if (quest && quest->data.questType == 6) {
				const auto [it, inserted] = miscStages.try_emplace(quest->GetFormID(), quest->currentStage);
				if (!inserted && it->second != quest->currentStage) {
					it->second = quest->currentStage;
					lastStageChange = now;
				}
			}
		}
		if (!Config::Get().syncQuests) {
			for (const auto quest : AllQuests()) {
				if (quest && Watched(quest)) {
					const auto [it, inserted] = knownStages.try_emplace(quest->GetFormID(), quest->currentStage);
					if (!inserted && it->second != quest->currentStage) {
						it->second = quest->currentStage;
						lastStageChange = now;
					}
				}
			}
			return;
		}

		// Friends' stages are only set during a session (OnSessionEnded drops the rest), except what
		// gives the player their controls back after a lock stage set here.
		if (a_inSession || locked) {
			busy = PlayerBusy(now);
			combatHold = CombatHold(now);
			std::erase_if(liveAt, [&](const auto& a_entry) { return now - a_entry.second.at >= LIVE_FOR; });
			Game game{ now };
			for (const auto& stage : waiting.Poll(now, game, Paused())) {
				++gaveUp;
				REX::WARN("QuestSync: {:08X} stage {} still not done {} s after setting it; going on with the next", stage.quest, stage.stage,
					std::chrono::duration_cast<std::chrono::seconds>(StageLog::Queue::TIMEOUT).count());
			}
			std::erase_if(waited, [](std::uint32_t a_quest) { return !waiting.Contains(a_quest); });
			FlushCaughtUp(now);
			CheckLock(now);
		}

		if (!reportFrom) {
			reportFrom = now + SETTLE_TIME;
		}
		const bool report = now >= *reportFrom;

		// Every poll, not once: the catch-up itself can leave MQ104 past 210 without it (a session
		// file from before 210 was reported), after this check first ran.
		if (a_inSession && report && Story::Shared() && !Story::InOpening()) {
			RepairStory();
		}

		for (const auto quest : AllQuests()) {
			if (!quest || !Watched(quest)) {
				continue;
			}
			const auto id = quest->GetFormID();

			// Completed here, not because of another player's stage: tell the others who did it.
			const bool done = (quest->data.flags & QUEST_COMPLETED) != 0;
			const auto [doneIt, firstLook] = knownDone.try_emplace(id, done);
			if (!firstLook && doneIt->second != done) {
				doneIt->second = done;
				const auto remote = remoteStageAt.find(id);
				const bool byOther = remote != remoteStageAt.end() && now - remote->second < REMOTE_COMPLETION_WINDOW;
				const char* name = quest->GetFullName();
				if (done && report && !byOther && name && *name && (id >> 24) != 0xFF) {
					outgoing.push_back(Protocol::Encode(Protocol::QuestDone{ 0, id }, Protocol::MessageType::kReportQuestDone));
					++completed;
				}
			}

			// Any stage change gives quest XP, not kill XP (see Party).
			const auto [it, inserted] = knownStages.try_emplace(id, quest->currentStage);
			if (!inserted && it->second != quest->currentStage) {
				it->second = quest->currentStage;
				lastStageChange = now;
			}

			// Every stage done since the last look is progress, not just the current stage: a stage whose
			// script moves the quest on at once (MQ104's 210 stops it) is never seen as the current one,
			// and the others then never ran it, nor what it does (starting MQ105, an achievement).
			auto       stages = QuestStageLayout::Done(quest);
			const auto [known, firstLookStages] = doneStages.try_emplace(id);
			if (firstLookStages) {
				known->second = std::move(stages);
				continue;
			}
			const auto fresh = StageLog::NewlyDone(known->second, std::move(stages));
			if (fresh.empty()) {
				continue;
			}
			lastStageChange = now;
			// Only of quests this player has started: the game moves some quests along in the background,
			// and a new game sets up many while the player is still in the opening.
			const auto remote = remoteStageAt.find(id);
			const bool reaction = remote != remoteStageAt.end() && now - remote->second < SCRIPT_REACTION_WINDOW;
			if (!report || reaction || (id >> 24) == 0xFF || !Shared(quest) || !PlayerStarted(quest) || Story::InOpening()) {
				continue;
			}
			for (const auto stage : fresh) {
				outgoing.push_back(Protocol::Encode(Protocol::QuestStage{ id, stage }, Protocol::MessageType::kReportQuestStage));
				++reported;
			}
		}
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing(bool a_sending)
	{
		if (!a_sending) {
			// Kept until connected: a stage is seen done only once, so a report dropped now (the welcome
			// still to come, a reconnect) would never be sent, and what its script starts (MQ104's 210
			// starts MQ105) would wait forever in the others' games.
			if (outgoing.size() > MAX_UNSENT) {
				outgoing.erase(outgoing.begin(), outgoing.end() - MAX_UNSENT);
			}
			return {};
		}
		return std::exchange(outgoing, {});
	}

	void OnSessionEnded()
	{
		// A stage set here has the player's controls: its quest goes on until they are given back.
		if (locked) {
			waiting.Keep({ locked->quest, locked->releaseQuest });
		} else {
			waiting.Clear();
		}
		waited.clear();
		liveAt.clear();
		caughtUpNames.clear();
		warnedAt.reset();
		sceneSince.reset();
		lockWaitLogged.clear();
	}

	void Rebaseline()
	{
		knownStages.clear();
		doneStages.clear();
		miscStages.clear();
		knownDone.clear();
		remoteStageAt.clear();
		reportFrom.reset();
		waiting.Clear();
		waited.clear();
		liveAt.clear();
		caughtUpNames.clear();
		warnedAt.reset();
		sceneSince.reset();
		lockWaitLogged.clear();
		locked.reset();  // the save says whether the player has their controls
		outgoing.clear();  // progress of the game before the load
	}

	Clock::time_point LastStageChange()
	{
		return lastStageChange;
	}

	std::string Describe()
	{
		return std::format("quests: reported={} applied={} caughtUp={} waiting={} setting={} gaveUp={} skipped={} unknown={} refused={} completed={} repaired={} released={} unsent={}{}",
			reported, applied, caughtUp, waiting.Size(), waiting.Setting(), gaveUp, skipped, unknown, refused, completed, repaired, released, outgoing.size(), locked ? " locked" : "");
	}
}
