#include "game/QuestSync.h"
#include "game/Official.h"

#include "Config.h"
#include "game/Achievements.h"
#include "game/FadeWatch.h"
#include "game/GameStop.h"
#include "game/Hud.h"
#include "game/OpenMenus.h"
#include "game/Party.h"
#include "game/QuestFilter.h"
#include "game/QuestStageLayout.h"
#include "game/RadiantRules.h"
#include "game/RadiantSync.h"
#include "game/RemotePlayers.h"
#include "game/SceneHold.h"
#include "game/StageLog.h"
#include "game/StageRules.h"
#include "game/Story.h"
#include "game/WorldSync.h"
#include "Text.h"

#include <deque>

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

		// A quest as the sharing filter sees it (QuestFilter): the type the game files it under, whether it
		// has objectives, and the name its designers gave it.
		QuestFilter::Quest Filed(const RE::TESQuest* a_quest)
		{
			const char* editorId = a_quest->formEditorID.c_str();
			return { a_quest->GetFormID(), a_quest->data.questType, !a_quest->objectives.empty(), editorId ? editorId : "" };
		}

		// The factions' last quests, filed under no type (QuestFilter::STORY_FINALES).
		bool IsFinale(const RE::TESQuest* a_quest)
		{
			return QuestFilter::IsFinale(Filed(a_quest));
		}

		// A dialogue quest: a place's people's lines and scenes, under no type and with no objectives, some
		// of whose stages open the way on (Diamond City's entrance scene opens the gate; see QuestFilter).
		// Shared with the rest of the story when the host shares it. With no objective ever shown, such a
		// quest counts as started once it is running or has stages done (SessionStarted).
		bool IsDialogue(const RE::TESQuest* a_quest)
		{
			return QuestFilter::IsDialogue(Filed(a_quest));
		}

		// Quests whose progress is watched here: the shared types, the factions' finales and the dialogue quests, and the
		// radiants shared by their run whatever their type (Diamond City's wanted posters are misc quests).
		bool Watched(const RE::TESQuest* a_quest)
		{
			return QuestFilter::Watched(Filed(a_quest)) || RadiantRules::ShareOf(a_quest->GetFormID()) == RadiantRules::Share::kShared;
		}

		// Whether the player has started the quest here, as reporting, setting friends' stages and the
		// host's baseline read it: an objective shown (PlayerStarted), or for a dialogue quest, which has
		// none, running or with stages done.
		bool SessionStarted(const RE::TESQuest* a_quest)
		{
			if (PlayerStarted(a_quest)) {
				return true;
			}
			return IsDialogue(a_quest) && ((a_quest->data.flags & QUEST_RUNNING) || !QuestStageLayout::Done(a_quest).empty());
		}

		// A quest that completes this soon after a stage from another player was completed by them.
		constexpr auto REMOTE_COMPLETION_WINDOW = 10s;
		// Stages a quest's script sets this soon after another player's stage are its reaction to
		// that stage, not this player's progress: every game that applies it gets there by itself.
		constexpr auto SCRIPT_REACTION_WINDOW = 3s;
		// Live stages, the friend nearby and the warning's cover: StageRules (LIVE_FOR, NEAR_FRIEND, WARNING_COVERS).
		using StageRules::LIVE_FOR;
		using StageRules::NEAR_FRIEND;
		using StageRules::WARNING_COVERS;
		// Caught-up stages wait while the player fights and a moment after: a stage that moves them
		// mustn't come mid-fight, and its quest XP would be taken for kill XP (see Party). Not forever:
		// something out of reach can keep the player in combat.
		constexpr auto AFTER_COMBAT = 5s;
		constexpr auto MAX_COMBAT_HOLD = 60s;
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

		// The host's progress reported when its session starts: at most this many stages (a story played to
		// its end has a few thousand), put in the outbox this many at a time (it keeps MAX_UNSENT).
		constexpr std::size_t MAX_BASELINE = 8192;
		constexpr std::size_t BASELINE_BATCH = 256;

		std::unordered_map<std::uint32_t, std::uint16_t> knownStages;  // last current stage seen per quest
		std::unordered_map<std::uint32_t, StageLog::Stages> doneStages;  // the stages seen done per quest
		std::unordered_map<std::uint32_t, bool>          knownDone;    // last completed state seen per quest
		std::unordered_map<std::uint32_t, Clock::time_point> remoteStageAt;  // when another player last moved it
		StageLog::Queue                                  waiting;  // stages from other players, until they are set here
		std::unordered_set<std::uint32_t>                waited;   // quests whose stages waited for the quest to start here
		std::unordered_map<std::uint32_t, StageLog::Wait> waitLogged;  // why each quest's stages wait, as last logged
		StageLog::Unstarted                              unstarted;  // stages done here just before their quest was started
		std::unordered_set<std::uint32_t>                startedHere;  // quests not started here that a friend's stage started for the session
		std::unordered_set<std::uint32_t>                catchingUp;   // ... with friends' stages still to set (logged once they are)
		std::unordered_map<std::uint64_t, Clock::duration> startHolds;  // a stage that started its quest: the start-up stage's hold too
		bool                                             inSession = false;  // the last Frame's
		// The host's progress so far, to report (QueueBaseline): a few at a time, so the outbox keeps them all.
		struct Baselined
		{
			std::uint32_t quest;
			std::uint16_t stage;
			bool          story;
		};
		std::deque<Baselined>                            baseline;
		bool                                             baselineQueued = false;
		std::uint32_t                                    baselined = 0;  // reported of it
		struct Live
		{
			Clock::time_point at;
			std::uint32_t     playerId;  // who did it (0: not known)
		};
		std::unordered_map<std::uint64_t, Live>          liveAt;  // friends' stages that came live (quest << 16 | stage)
		// A stage set here took the player's controls away (StageRules::LOCKS), and the stage that gives
		// them back is still to come. How long that has taken is the queue's (Queue::Lock).
		struct Locked
		{
			std::uint32_t quest;
			std::uint16_t stage;
			std::uint32_t releaseQuest;
			std::uint16_t releaseStage;
		};
		std::optional<Locked>                            locked;
		std::unordered_set<std::uint64_t>                lockWaitLogged;  // lock stages logged as waiting for their release
		std::unordered_set<std::uint64_t>                openingWaitLogged;  // stages logged as waiting for the opening to end
		// Stages logged as never done in time, once each while the game runs: some never read as done here at all (a
		// dialogue quest's stage 0 a friend's game set, 000190BC each load in the 0.10.0 rc soak); gaveUp counts the rest.
		std::unordered_set<std::uint64_t>                gaveUpLogged;
		std::optional<Clock::time_point>                 reportFrom;  // set on the first in-game poll
		StageLog::Outbox<std::vector<std::uint8_t>>      outgoing;  // reports, kept or dropped while not connected
		Clock::time_point                                nextPoll{};
		bool                                             busy = false;        // this poll: a conversation or scene
		bool                                             combatHold = false;  // this poll: caught-up stages wait
		std::optional<Clock::time_point>                 combatSince;
		Clock::time_point                                lastCombat{};
		SceneHold::Watch                                 sceneHold;  // whether the scene the player is in holds friends' stages back
		std::uint32_t                                    sessionSceneLogged = 0;  // the session's scene last logged (0: none)
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
		std::uint32_t                                    openingDropped = 0;  // the opening's, never shared (StageRules::IsOpening)
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

		// A stage set in the middle of a conversation or scene can break it; wait until it's over. Not a scene
		// a friend's stage started here (Codsworth's search, started by his talk's stage): it is the session's,
		// the friend's later stages end it, and held back by it they would never come (SceneHold). The player's
		// own scene of a quest with stages waiting here only holds them for SceneHold::MAX_HOLD.
		bool PlayerBusy(Clock::time_point a_now)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto scene = player ? player->GetCurrentScene() : nullptr;
			const auto quest = scene && scene->parentQuest ? scene->parentQuest->GetFormID() : 0u;
			const bool held = sceneHold.Holds({ .talking = OpenMenus::Open(OpenMenus::Menu::kDialogue),
												  .scene = scene ? scene->GetFormID() : 0u,
												  .sceneQuest = quest,
												  .sceneQuestWaiting = quest != 0 && waiting.Contains(quest) },
				a_now);
			if (const auto sessions = sceneHold.SessionScene(); sessions != sessionSceneLogged) {
				sessionSceneLogged = sessions;
				if (sessions) {
					REX::INFO("QuestSync: the player is in scene {:08X} of {:08X}, started by a friend's stage; it holds no stages back, and its people are theirs", sessions, quest);
				}
			}
			return held;
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
			return GameStop::Now();
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

		// The main story and faction quests, their finales and the dialogue quests too.
		bool IsStory(const RE::TESQuest* a_quest)
		{
			return IsFinale(a_quest) || IsDialogue(a_quest) || Story::IsStoryType(a_quest->data.questType);
		}

		// Quests a session can share: the shared types, the factions' finales and the dialogue quests of the
		// game's own files. Quests from other mods are never shared (see Official.h).
		// The radiants aren't: one is shared by its run, not by its quest (RadiantSync).
		bool Shareable(const RE::TESQuest* a_quest)
		{
			return Official::IsOfficialForm(a_quest) && Watched(a_quest) && !RadiantRules::IsRadiant(a_quest->GetFormID());
		}

		// Quests this session shares now: the story's only if the host chose a shared story. A radiant's while its run here
		// is shared (RadiantSync).
		bool Shared(const RE::TESQuest* a_quest)
		{
			return (Shareable(a_quest) && (Story::Shared() || !IsStory(a_quest))) || RadiantSync::Shares(a_quest->GetFormID());
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

		// A stage of the opening (Vault 111; StageRules::IsOpening): every player's own, never shared.
		bool IsOpeningStage(const RE::TESQuest* a_quest, std::uint16_t a_stage)
		{
			const auto file = Official::IsOfficialForm(a_quest) ? FileOf(a_quest) : std::nullopt;
			return file && StageRules::IsOpening(*file, a_quest->GetFormID() & 0x00FFFFFF, a_stage);
		}

		// Whether a stage of this quest goes on from a lock stage set here (MQ206's 1000, 1050 and 1200
		// after its 500): without a combat hold, since the player can't fight anyway, and without
		// another warning, since they were warned before the lock.
		bool Unlocking(std::uint32_t a_quest)
		{
			return locked && (a_quest == locked->quest || a_quest == locked->releaseQuest);
		}

		const char* WhyWaiting(StageLog::Wait a_wait)
		{
			switch (a_wait) {
			case StageLog::Wait::kLoading:
				return "the game is loading";
			case StageLog::Wait::kOpening:
				return "the player is still in the opening";
			case StageLog::Wait::kNotRunning:
				return "the quest isn't running here";
			case StageLog::Wait::kNotStarted:
				return "the player hasn't started the quest (no objective shown)";
			case StageLog::Wait::kBusy:
				return "the player is in a conversation or a scene";
			case StageLog::Wait::kStopped:
				return "the quest ran here before and was stopped; starting it again would start it over";
			case StageLog::Wait::kStart:
				return "the quest isn't started here; they start it for the session once the stages that came before them are set";
			default:
				return "";
			}
		}

		// The quest ran here before and was stopped (it isn't running now): stages done, or past its first.
		bool RanBefore(const RE::TESQuest* a_quest)
		{
			return (a_quest->data.flags & QUEST_COMPLETED) || a_quest->currentStage > 0 || !QuestStageLayout::Done(a_quest).empty();
		}

		// Whether stages from other players can be set on this quest now (see StageLog::WaitsFor). A quest
		// this player hasn't started is started for the session by the first of them (Quest::kStart), after
		// the stages that came before it: another quest's stage may start it (MQ104's 210 starts MQ105). A
		// lock's quests go on after the session, when the story is no longer shared (StageLog::Settable).
		// Why a quest's stages wait is logged once for each reason.
		StageLog::Quest StateOf(std::uint32_t a_quest)
		{
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			if (!quest || !StageLog::Settable(Shared(quest), Unlocking(a_quest)) || StageLog::Finished(quest->data.flags)) {
				return StageLog::Quest::kGone;
			}
			// A dialogue quest that ran here and stopped is over (its 100 stops Diamond City's entrance scene,
			// the gate open): starting it again would play it over, so what is left of it is dropped, not
			// kept waiting.
			if (IsDialogue(quest) && !(quest->data.flags & QUEST_RUNNING) && RanBefore(quest)) {
				if (const auto stages = waiting.Waiting(a_quest); !stages.empty()) {
					REX::INFO("QuestSync: {} stage(s) of {:08X} from friends dropped (the first {}): the scene ran here and is over", stages.size(), a_quest, stages.front());
				}
				return StageLog::Quest::kGone;
			}
			StageLog::QuestHere here;
			// Quest scripts must not run at the main menu or during a loading screen.
			here.inWorld = WorldSync::InWorld();
			here.opening = Story::InOpening();
			here.running = (quest->data.flags & QUEST_RUNNING) != 0;
			here.started = SessionStarted(quest);
			here.busy = busy;
			here.inSession = inSession && Shared(quest);
			here.ranBefore = !here.running && RanBefore(quest);
			here.startedHere = startedHere.contains(a_quest) || RadiantSync::Shares(a_quest);  // a shared run: started here for the session
			const auto wait = StageLog::WaitsFor(here);
			if (wait == StageLog::Wait::kNone) {
				waitLogged.erase(a_quest);
				return StageLog::Quest::kReady;
			}
			if (wait != StageLog::Wait::kBusy || !here.running || !(here.started || here.startedHere)) {
				waited.insert(a_quest);
			}
			if (wait != StageLog::Wait::kLoading && waitLogged[a_quest] != wait) {
				waitLogged[a_quest] = wait;
				const char* name = quest->GetFullName();
				const auto  stages = waiting.Waiting(a_quest);
				REX::INFO("QuestSync: {} stage(s) of {:08X} '{}' from friends wait (the first {}): {}", stages.size(), a_quest, name ? name : "",
					stages.empty() ? 0 : stages.front(), WhyWaiting(wait));
			}
			return wait == StageLog::Wait::kStart ? StageLog::Quest::kStart : StageLog::Quest::kWait;
		}

		// Stages only move forward: never undo progress a player already has. A stage this quest is
		// already past here is left alone, done or not (it may be a branch this player didn't take).
		// Except a dialogue quest's: its stages aren't steps in order (a player at Diamond City's gate can
		// have the entrance scene's 400 without the 32, 40 and 100 that open the gate and end it), so only
		// a stage done here is.
		bool IsSettled(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			if (!quest) {
				return true;
			}
			if (IsDialogue(quest)) {
				return QuestStageLayout::IsStageDone(quest, a_stage);
			}
			return a_stage <= quest->currentStage || QuestStageLayout::IsStageDone(quest, a_stage);
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
				names += " " + Text::Format("quest.and_more", { { "n", caughtUpNames.size() - shown } });
			}
			Hud::Notify(Text::Format("quest.caught_up", { { "names", names } }));
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
			FadeWatch::Arm(std::format("set {:08X} stage {}", id, a_stage));
			knownStages[id] = std::max(a_quest->currentStage, a_stage);
			if (const auto known = doneStages.find(id); known != doneStages.end()) {
				StageLog::Insert(known->second, a_stage);
			}
			remoteStageAt[id] = Clock::now();
			lastStageChange = Clock::now();  // its quest XP is ours, not kill XP
			sceneHold.StageSet(id, Clock::now());  // its script may start a scene with the player in it: the session's
			++applied;
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
			// Not started here: this stage starts it for the session (StateOf), and if it isn't running, the
			// game runs its start-up stage too.
			const bool  starting = !(quest->data.flags & QUEST_RUNNING) || (!SessionStarted(quest) && !startedHere.contains(a_quest) && !RadiantSync::Shares(a_quest));
			const auto  startRule = file && !(quest->data.flags & QUEST_RUNNING) ? StageRules::Find(*file, local, 0) : nullptr;
			const auto  judged = StageRules::Starting(startRule, file ? StageRules::Find(*file, local, a_stage) : nullptr);
			const auto  rule = judged ? &*judged : nullptr;
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
			now.opening = Story::InOpening();
			now.friendInOpening = Party::FriendInOpening();
			switch (StageRules::Judge(rule, now)) {
			case StageRules::Verdict::kSkip:
				++skipped;
				REX::INFO("QuestSync: not setting {:08X} '{}' stage {}: {}", a_quest, name ? name : "", a_stage, WhySkipped(rule ? rule->flags : 0));
				return StageLog::Set::kSkipped;
			case StageRules::Verdict::kWarn:
				warnedAt = a_now;
				Hud::Notify(Text::Format("quest.catching_up", { { "n", std::chrono::duration_cast<std::chrono::seconds>(StageRules::WARNING).count() },
					{ "quest", name ? std::string{ name } : Text::Get("quest.a_quest") } }));
				return StageLog::Set::kLater;
			case StageRules::Verdict::kLater:
				return StageLog::Set::kLater;
			case StageRules::Verdict::kQuestWaits:
				if (lockWaitLogged.insert(Key(a_quest, a_stage)).second) {
					REX::INFO("QuestSync: {:08X} '{}' stage {} takes the player's controls away; waiting for {:08X} stage {}, which gives them back",
						a_quest, name ? name : "", a_stage, releaseId, lock ? lock->releaseStage : 0);
				}
				return StageLog::Set::kQuestWaits;
			case StageRules::Verdict::kOpeningWaits:
				if (openingWaitLogged.insert(Key(a_quest, a_stage)).second) {
					REX::INFO("QuestSync: {:08X} '{}' stage {} moves the player; waiting while this game is in the opening", a_quest, name ? name : "", a_stage);
				}
				return StageLog::Set::kQuestWaits;
			case StageRules::Verdict::kSet:
				break;
			}

			SetStageHere(quest, a_stage);
			REX::INFO("QuestSync: set {:08X} '{}' stage {} (was {}){}{}", a_quest, name ? name : "", a_stage, quest->currentStage, now.live ? "" : " (catching up)",
				starting ? " (starting the quest here for the session)" : "");
			if (starting) {
				startedHere.insert(a_quest);
				catchingUp.insert(a_quest);
				startHolds[Key(a_quest, a_stage)] = StageRules::Hold(rule);
			}
			if (lock) {
				locked = Locked{ a_quest, a_stage, releaseId, lock->releaseStage };
				waiting.Lock(a_quest, releaseId);
			}
			if (waited.erase(a_quest) > 0) {
				++caughtUp;
				NoteCaughtUp(IsDialogue(quest) ? nullptr : name, a_now);  // a dialogue quest's name is the editor's, not one the player knows
			}
			Achievements::AfterStageSet(a_quest, a_stage);
			return StageLog::Set::kSet;
		}

		// Out of a session only the lock kept its quests' stages (OnSessionEnded); once the player has
		// their controls back, the rest of them are the session's and go too.
		void Unlock(bool a_inSession)
		{
			locked.reset();
			waiting.Unlock();
			if (!a_inSession) {
				waiting.Clear();
			}
		}

		// The player has their controls back once the lock's release stage is done. Looked at before the
		// queue's poll too: out of a session, the poll that finds the release done would go on to set the
		// next of its quest's stages (one of the session's past MQ206's 1200).
		void CheckReleased(bool a_inSession)
		{
			if (!locked) {
				return;
			}
			const auto release = RE::TESForm::GetFormByID<RE::TESQuest>(locked->releaseQuest);
			if (!release || QuestStageLayout::IsStageDone(release, locked->releaseStage)) {
				Unlock(a_inSession);
			}
		}

		// If the lock's quests have done nothing for Queue::LOCK_TIMEOUT of play (a stage given up on, or
		// one that can't be set), the release is set outright rather than leave the player unable to move.
		// Not while the teleport is still to be tried: the time a stage of theirs is being set or its
		// script runs on doesn't count, nor time paused (a menu, alt-tab, a loading screen), when no
		// script runs.
		void CheckLock(bool a_inSession)
		{
			CheckReleased(a_inSession);
			if (!locked || !waiting.LockOverdue()) {
				return;
			}
			const auto release = RE::TESForm::GetFormByID<RE::TESQuest>(locked->releaseQuest);
			REX::WARN("QuestSync: {:08X} stage {} took the player's controls, and nothing has given them back in {} s of play; setting {:08X} stage {}",
				locked->quest, locked->stage, std::chrono::duration_cast<std::chrono::seconds>(StageLog::Queue::LOCK_TIMEOUT).count(), locked->releaseQuest, locked->releaseStage);
			SetStageHere(release, locked->releaseStage);
			++released;
			Hud::Notify(Text::Get("quest.controls_back"));
			Unlock(a_inSession);
		}

		// The game as the queue sees it.
		struct Game
		{
			Clock::time_point now;

			StageLog::Quest Quest(std::uint32_t a_quest) const { return StateOf(a_quest); }
			bool            Settled(std::uint32_t a_quest, std::uint16_t a_stage) const { return IsSettled(a_quest, a_stage); }
			StageLog::Set   Set(std::uint32_t a_quest, std::uint16_t a_stage) const { return SetHere(a_quest, a_stage, now); }
			Clock::duration Hold(std::uint32_t a_quest, std::uint16_t a_stage) const
			{
				// A stage that started its quest: the start-up stage's script may run on too (SetHere).
				if (const auto start = startHolds.extract(Key(a_quest, a_stage))) {
					return start.mapped();
				}
				return StageRules::Hold(RuleFor(a_quest, a_stage));
			}
			bool SetsAnother(std::uint32_t a_quest, std::uint16_t a_stage) const
			{
				const auto file = FileOf(RE::TESForm::GetFormByID<RE::TESQuest>(a_quest));
				return file && StageRules::SetsAnother(*file, a_quest & 0x00FFFFFF, a_stage);
			}
		};

		// An older version set only the highest stage of a friend's quest. MQ104 went straight to 1000,
		// its 210 never ran here, and so MQ105 never started: the main story stopped there, and the
		// session file doesn't have 210 to send again. Starts MQ105 for a player left like that, as a
		// stage caught up from the session would be: starting it runs its stage 0, which moves the player
		// (StageRules), so after a warning, not mid-fight, nor in a conversation or a menu.
		void RepairStory(Clock::time_point a_now)
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
			if (busy || Paused()) {
				return;
			}
			const char* name = mq105->GetFullName();
			const auto  shown = name && *name ? std::string{ name } : Text::Get("quest.next");
			StageRules::Situation now;
			now.sharedStory = true;
			now.held = combatHold;
			now.opening = Story::InOpening();
			now.friendInOpening = Party::FriendInOpening();
			if (warnedAt && a_now - *warnedAt < WARNING_COVERS) {
				now.warned = a_now - *warnedAt;
			}
			switch (StageRules::Judge(RuleFor(MQ105, 0), now)) {
			case StageRules::Verdict::kOpeningWaits:
				return;
			case StageRules::Verdict::kWarn:
				warnedAt = a_now;
				Hud::Notify(Text::Format("quest.stuck_in", { { "quest", shown }, { "n", std::chrono::duration_cast<std::chrono::seconds>(StageRules::WARNING).count() } }));
				return;
			case StageRules::Verdict::kLater:
				return;
			default:
				break;
			}
			REX::WARN("QuestSync: MQ104 is at {} without its stage {}, and MQ105 never started; starting it", mq104->currentStage, MQ104_STARTS_MQ105);
			RE::Console::ExecuteCommand(std::format("setstage {:08X} {}", MQ105, MQ105_FIRST).c_str());
			FadeWatch::Arm(std::format("set {:08X} stage {}", MQ105, MQ105_FIRST));
			sceneHold.StageSet(MQ105, a_now);
			++repaired;
			Hud::Notify(Text::Format("quest.stuck", { { "quest", shown } }));
		}

		// The session's story starts where the host's is: every stage done in the host's game of the shared
		// quests its player has started or finished (the story's only if it is shared), quest by quest in
		// the game's order and each quest's in ascending order. The server keeps those it doesn't have yet
		// like any of the session's stages, so a friend joining now or later is pushed through them in order
		// (StageLog::Queue: scripts, holds, locks, never backwards), not only through what was done since
		// hosting began. Only the host's game does this: a joiner's progress before joining stays theirs.
		void QueueBaseline()
		{
			baseline.clear();
			std::size_t quests = 0;
			std::size_t left = 0;
			for (const auto quest : AllQuests()) {
				if (!quest || !Shareable(quest) || (quest->GetFormID() >> 24) == 0xFF || (IsStory(quest) && !Story::Shared())) {
					continue;
				}
				if (!SessionStarted(quest) && !(quest->data.flags & QUEST_COMPLETED)) {
					continue;  // run in the background, not the player's progress
				}
				const auto done = QuestStageLayout::Done(quest);
				if (done.empty()) {
					continue;
				}
				++quests;
				for (const auto stage : done) {
					if (IsOpeningStage(quest, stage)) {
						continue;  // every player's own
					}
					if (baseline.size() < MAX_BASELINE) {
						baseline.push_back({ quest->GetFormID(), stage, IsStory(quest) });
					} else {
						++left;
					}
				}
			}
			REX::INFO("QuestSync: the session starts from this game's story: {} stages of {} quests{}", baseline.size(), quests,
				left ? std::format(" ({} more left out)", left) : std::string());
		}
	}

	bool PlayerStarted(const RE::TESQuest* a_quest)
	{
		return a_quest && std::ranges::any_of(a_quest->objectives, [](const RE::BGSQuestObjective* a_objective) {
			return a_objective && a_objective->state != static_cast<char>(RE::QUEST_OBJECTIVE_STATE::kDormant);
		});
	}

	const char* Kind(const RE::TESQuest* a_quest)
	{
		if (a_quest && RadiantRules::ShareOf(a_quest->GetFormID()) != RadiantRules::Share::kNotRadiant) {
			if (RadiantRules::ShareOf(a_quest->GetFormID()) == RadiantRules::Share::kLeftAlone) {
				return "radiant (each player's own)";
			}
			return RadiantSync::Shares(a_quest->GetFormID()) ? "radiant (this run shared)" : "radiant (by its run)";
		}
		if (!a_quest || !Shareable(a_quest)) {
			return "no";
		}
		if (IsFinale(a_quest)) {
			return "finale";
		}
		if (IsDialogue(a_quest)) {
			return "dialogue";
		}
		return Story::IsStoryType(a_quest->data.questType) ? "story" : "side";
	}

	// Queues a friend's stage to set here (Frame).
	static void Queue(const Protocol::QuestStage& a_stage, bool a_live);

	void Apply(const Protocol::QuestStage& a_stage, bool a_live)
	{
		// A radiant's stages come by its run (ApplyRadiant): one that comes as the quest's (an older game's, or the
		// session file's from before) is of some run with other targets.
		if (RadiantRules::IsRadiant(a_stage.quest)) {
			return;
		}
		Queue(a_stage, a_live);
	}

	void ApplyRadiant(std::uint32_t a_quest, std::uint16_t a_stage, std::uint32_t a_playerId, bool a_live)
	{
		Queue(Protocol::QuestStage{ a_quest, a_stage, a_playerId }, a_live);
	}

	static void Queue(const Protocol::QuestStage& a_stage, bool a_live)
	{
		if (!Config::Get().syncQuests) {
			return;
		}
		const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_stage.quest);
		if (!quest || !Shared(quest) || StageLog::Finished(quest->data.flags) || IsSettled(a_stage.quest, a_stage.stage)) {
			return;
		}
		// The opening is every player's own (Vault 111): a friend's stage of it, from an older version's report or a
		// session file that has one, would pull the player back into the vault's cryopod or start its scenes.
		if (IsOpeningStage(quest, a_stage.stage)) {
			if (openingDropped++ == 0) {
				REX::INFO("QuestSync: not setting {:08X} stage {}: it is the opening's, which every player plays in their own game (and any more like it)",
					a_stage.quest, a_stage.stage);
			}
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
		REX::INFO("QuestSync: player {} completed {:08X} '{}'", a_done.playerId, a_done.quest, name);
		Hud::Notify(Text::Format("quest.completed", { { "player", RemotePlayers::NameOf(a_done.playerId) }, { "quest", name } }));
	}

	void Frame(bool a_inSession, bool a_hosting)
	{
		const auto now = Clock::now();
		if (now < nextPoll) {
			return;
		}
		nextPoll = now + POLL_INTERVAL;
		inSession = a_inSession;

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
			const bool paused = Paused();
			CheckReleased(a_inSession);
			for (const auto& stage : waiting.Poll(now, game, paused)) {
				++gaveUp;
				if (gaveUpLogged.insert(Key(stage.quest, stage.stage)).second) {
					REX::WARN("QuestSync: {:08X} stage {} still not done {} s after setting it; going on with the next (logged once; gaveUp counts each)", stage.quest, stage.stage,
						std::chrono::duration_cast<std::chrono::seconds>(StageLog::Queue::TIMEOUT).count());
				}
			}
			std::erase_if(waited, [](std::uint32_t a_quest) { return !waiting.Contains(a_quest); });
			std::erase_if(catchingUp, [](std::uint32_t a_quest) {
				if (waiting.Contains(a_quest)) {
					return false;
				}
				const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
				const char* name = quest ? quest->GetFullName() : nullptr;
				REX::INFO("QuestSync: caught up on {:08X} '{}' to stage {} (started here for the session)", a_quest, name ? name : "", quest ? quest->currentStage : 0);
				return true;
			});
			FlushCaughtUp(now);
			if (!paused) {
				CheckLock(a_inSession);
			}
		}

		if (!reportFrom) {
			reportFrom = now + SETTLE_TIME;
		}
		const bool report = now >= *reportFrom;

		// The session's story starts from the host's: once its own server has welcomed it (and again after
		// a drop or a load), the host's game reports its progress so far (QueueBaseline).
		if (a_hosting && a_inSession && report && !Story::InOpening() && !baselineQueued) {
			baselineQueued = true;
			QueueBaseline();
		}
		while (!baseline.empty() && outgoing.Size() < BASELINE_BATCH) {
			outgoing.Add(Protocol::Encode(Protocol::QuestStage{ baseline.front().quest, baseline.front().stage }, Protocol::MessageType::kReportQuestStage),
				baseline.front().story);
			baseline.pop_front();
			++baselined;
		}

		// Every poll, not once: the catch-up itself can leave MQ104 past 210 without it (a session
		// file from before 210 was reported), after this check first ran.
		if (a_inSession && report && Story::Shared() && !Story::InOpening()) {
			RepairStory(now);
		}

		// Radiants' runs: ours begun, friends' started here, ended ones closed (before the stages done here are looked at,
		// so a run begun now takes the stages done so far with its start).
		if (report) {
			RadiantSync::Frame(a_inSession);
		}

		unstarted.Expire(now);
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
				if (done && report && !byOther && name && *name && (id >> 24) != 0xFF && !IsDialogue(quest)) {
					outgoing.Add(Protocol::Encode(Protocol::QuestDone{ 0, id }, Protocol::MessageType::kReportQuestDone));
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
			auto fresh = StageLog::NewlyDone(known->second, std::move(stages));
			if (!fresh.empty()) {
				lastStageChange = now;
			} else if (!unstarted.Has(id)) {
				continue;
			}
			// Only of quests this player has started: the game moves some quests along in the background,
			// and a new game sets up many while the player is still in the opening. What it reached just
			// before it was started goes once it is (StageLog::Unstarted).
			const auto remote = remoteStageAt.find(id);
			const bool reaction = remote != remoteStageAt.end() && now - remote->second < SCRIPT_REACTION_WINDOW;
			// The story's stages whatever the story is now while the connection is being remade: it is the
			// player's own then (Story::SetShared(false)), and these stages are seen done only now. The outbox
			// sends them only if the welcome says the session shares the story (StageLog::Outbox).
			const bool story = IsStory(quest);
			// A radiant's stages go by its run, if it is shared (RadiantSync); a stage of it set here from a friend is
			// never seen fresh, and one its script sets in reply goes too (each game takes each stage of a run once).
			if (RadiantRules::IsRadiant(id)) {
				if (report && !Story::InOpening()) {
					RadiantSync::OnDone(quest, fresh);
				}
				continue;
			}
			if (!report || reaction || (id >> 24) == 0xFF || !Shareable(quest) || (story && a_inSession && !Story::Shared()) || Story::InOpening()) {
				continue;
			}
			if (!SessionStarted(quest) && !startedHere.contains(id)) {
				unstarted.Add(id, fresh, now);
				continue;
			}
			fresh = unstarted.Take(id, std::move(fresh), now);
			for (const auto stage : fresh) {
				if (IsOpeningStage(quest, stage)) {
					continue;  // the opening is every player's own: a stage of it done here (the vault's 110 outside) isn't reported
				}
				outgoing.Add(Protocol::Encode(Protocol::QuestStage{ id, stage }, Protocol::MessageType::kReportQuestStage), story);
				++reported;
			}
		}
		// A radiant's run is the story's: a session that doesn't share the story doesn't get it (StageLog::Outbox).
		for (auto& packet : RadiantSync::TakeOutgoing()) {
			outgoing.Add(std::move(packet), true);
		}
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing(bool a_sending, bool a_inSession)
	{
		// Welcomed, Story::Shared is the welcome's.
		return outgoing.Take(Clock::now(), { a_sending, a_inSession, Story::Shared() });
	}

	void OnServerChanged()
	{
		outgoing.NewSession();
		RadiantSync::OnServerChanged();
	}

	void OnSessionEnded()
	{
		// Nothing of the session reaches a solo game: no stage still waiting is set, and what other players
		// moved lately belongs to the session. A quest started later in solo play must not be caught up to
		// the session's stage. Except a lock: a stage set here has the player's controls, and its quest goes
		// on until they are given back.
		if (locked) {
			waiting.Keep({ locked->quest, locked->releaseQuest });
		} else {
			waiting.Clear();
		}
		waited.clear();
		liveAt.clear();
		caughtUpNames.clear();
		warnedAt.reset();
		sceneHold.Clear();  // what was set is forgotten; a scene of the session's the player is still in stays the session's (a reconnect)
		sessionSceneLogged = 0;
		lockWaitLogged.clear();
		openingWaitLogged.clear();
		waitLogged.clear();
		remoteStageAt.clear();
		startedHere.clear();
		catchingUp.clear();
		startHolds.clear();
		RadiantSync::OnSessionEnded();  // the radiants go on, each player's own
		// A host's game reports its progress again once its server welcomes it back (QueueBaseline).
		baseline.clear();
		baselineQueued = false;
		// Reporting goes on without a new settle (not reportFrom.reset): a stage done while the connection is
		// remade is reported once it is back (outgoing), and playing alone the reports are dropped.
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
		sceneHold.Clear();
		sessionSceneLogged = 0;
		lockWaitLogged.clear();
		openingWaitLogged.clear();
		waitLogged.clear();
		unstarted.Clear();
		locked.reset();  // the save says whether the player has their controls (waiting.Clear drops the queue's)
		outgoing.Clear();  // progress of the game before the load
		startedHere.clear();
		catchingUp.clear();
		startHolds.clear();
		baseline.clear();  // a host's game reports the loaded save's progress instead
		baselineQueued = false;
		RadiantSync::Rebaseline();
	}

	Clock::time_point LastStageChange()
	{
		return lastStageChange;
	}

	std::uint32_t SessionScene()
	{
		return sceneHold.SessionScene();
	}

	std::string Describe()
	{
		return std::format("quests: reported={} baseline={} applied={} caughtUp={} startedHere={} waiting={} setting={} gaveUp={} skipped={} openingStages={} unknown={} refused={} completed={} repaired={} released={} unsent={} unstarted={}{}",
			reported, baselined, applied, caughtUp, startedHere.size(), waiting.Size(), waiting.Setting(), gaveUp, skipped, openingDropped, unknown, refused, completed, repaired, released, outgoing.Size(),
			unstarted.Size(), locked ? " locked" : "") +
		       (sceneHold.SessionScene() ? std::format(" sessionScene={:08X}", sceneHold.SessionScene()) : std::string()) + " " + RadiantSync::Describe();
	}
}
